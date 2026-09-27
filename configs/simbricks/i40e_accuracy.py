"""Fresh-boot CacheLens CHI/i40e scaffolding, NOT an accuracy measurement.

Configuration mode is asset-free and never instantiates/connects sockets.
Boot-mmio mode reuses the readonly RSS guest inputs and readiness-gated PCAP.
"""
import argparse
import hashlib
import json
from pathlib import Path

import m5
from m5.objects import AddrRange, EtherLink, LoadGeneratorPcap, SimBricksEthernet, SimBricksPci
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.prebuilt.cachelens.board import CacheLensX86Board
from gem5.prebuilt.cachelens.cache_hierarchy import CacheLensCHIHierarchy
from gem5.resources.resource import DiskImageResource, KernelResource

GIB = 1 << 30
# Pilot manifest selects Xeon geometry; its measure/config.ini records 500ps
# core and 1000ps board periods (NOT 3GHz). See verification/accuracy/README.md.
XEON_PILOT_CORE_CLOCK = "2GHz"
XEON_PILOT_BOARD_CLOCK = "1GHz"
GEOMETRY = dict(
    l1i_size="32KiB", l1i_assoc=8, l1d_size="32KiB", l1d_assoc=8,
    l2_size="256KiB", l2_assoc=8, hnf_size="2560KiB", hnf_assoc=20,
    num_hnfs=8, ddio_way_part=2, model_profile="intel-ddio",
    hnf_inclusion="inclusive", indexing_policy="splitmix64",
    cache_stashing="off", core_clock=XEON_PILOT_CORE_CLOCK,
)
KERNEL_ARGS = [
    "earlyprintk=ttyS0", "console=ttyS0", "lpj=7999923", "pci=noacpi",
    "root=/dev/sda", "rw", "cachelens.skip_init_checkpoint=1",
    "vfio.enable_unsafe_noiommu_mode=1",
    "initcall_blacklist=print_s5_reset_status_mmio",
]


class AccuracyBoard(CacheLensX86Board):
    def __init__(self, pci_socket, **kwargs):
        self._accuracy_pci_socket = str(pci_socket)
        super().__init__(network_options={"num_nics": 1}, **kwargs)

    def _setup_memory_ranges(self):
        # Local, exact-16GiB exception to the base board's legacy 8GiB guard.
        memory = self.get_memory()
        if memory.get_size() != 16 * GIB:
            raise ValueError("i40e accuracy scaffold requires exactly 16GiB")
        if not self.get_cache_hierarchy().is_ruby():
            raise ValueError("i40e accuracy scaffold requires Ruby")
        self._ram_ranges = [AddrRange(3 * GIB), AddrRange(4 * GIB, size=13 * GIB)]
        memory.set_memory_range(self._ram_ranges)
        self.main_mem_ranges = list(self._ram_ranges)
        self.mem_ranges = list(self._ram_ranges) + [AddrRange(0xC0000000, size=0x100000)]
        # No high-MMIO bridge/aperture: RubyPort routes every non-RAM request
        # directly to iobus; the NIC advertises its BAR ranges dynamically.

    def _attach_cachelens_network(self):
        nic = SimBricksPci(
            host=self.pc.pci_host, pci_bus=0, pci_dev=2, pci_func=0,
            InterruptLine=17, InterruptPin=1,
            uxsocket_path=self._accuracy_pci_socket, listen=False, sync=True,
            link_latency="500ns", sync_tx_interval="500ns",
        )
        nic.pio = self.iobus.mem_side_ports
        self.nics = [nic]
        # get_dma_ports() gives IDE and NIC separate CHI requestors. Never
        # attach nic.dma to iobus (that loses the dedicated requestor path).


def make_board(pci_socket):
    return AccuracyBoard(
        pci_socket, clk_freq=XEON_PILOT_BOARD_CLOCK,
        processor=SimpleProcessor(cpu_type=CPUTypes.TIMING, num_cores=8, isa=ISA.X86),
        memory=DualChannelDDR4_2400(size="16GiB"),
        cache_hierarchy=CacheLensCHIHierarchy(**GEOMETRY),
    )


def check_topology(board):
    """Structural assertions, not evidence that a live NIC BAR responds."""
    ram = [(int(r.start), int(r.start) + int(r.size())) for r in board.main_mem_ranges]
    assert ram == [(0, 3 * GIB), (4 * GIB, 17 * GIB)]
    assert sum(end - start for start, end in ram) == 16 * GIB
    for start, end in [(0xC0000000, 0xC0100000), (17 * GIB, 33 * GIB)]:
        assert all(end <= lo or start >= hi for lo, hi in ram)
    assert not hasattr(board, "bridge")
    assert board.get_nic_bdfs() == ["0000:00:02.0"]
    assert board.nics[0].pio.peer.simobj is board.iobus
    assert any(int(entry.addr) == 4 * GIB and int(entry.size) == 13 * GIB
               and int(entry.range_type) == 1 for entry in board.workload.e820_table.entries)
    hierarchy = board.get_cache_hierarchy()
    assert len(hierarchy.core_clusters) == len(hierarchy.hnfs) == 8
    assert len(hierarchy.dma_requestors) == 2
    for port, requestor in zip(board.get_dma_ports(), hierarchy.dma_requestors):
        assert requestor.sequencer.in_ports[0].peer is port
        assert bool(requestor.ddio_eligible) == (port is board.nics[0].dma)
        assert list(requestor.downstream_destinations) == list(hierarchy.hnfs)
    for cluster in hierarchy.core_clusters:
        assert cluster.l1d.sequencer.mem_request_port.peer.simobj is board.iobus
        assert list(cluster.l2.downstream_destinations) == list(hierarchy.hnfs)
    for hnf in hierarchy.hnfs:
        assert int(hnf.cache.ddio_way_part) == 2
        assert bool(hnf.cache.addr_hash) and bool(hnf.nic_read_no_allocate)
        assert bool(hnf.alloc_on_readunique) and bool(hnf.dealloc_backinv_unique)
        assert bool(hnf.dealloc_backinv_shared) and not bool(hnf.dealloc_on_unique)
        assert not bool(hnf.enable_DMT) and not bool(hnf.enable_DCT)
    assert hierarchy.get_total_hnf_capacity_bytes() == 20 * (1 << 20)
    configuration = hierarchy.get_configuration()
    configuration["cache_line_size"] = int(configuration["cache_line_size"])
    return dict(scope="configuration/functional boot only; NOT accuracy",
                core_clock=XEON_PILOT_CORE_CLOCK, board_clock=XEON_PILOT_BOARD_CLOCK,
                ram_ranges=ram, high_bar_start=17 * GIB,
                dma_order=["IDE", "i40e"], geometry=GEOMETRY,
                hierarchy=configuration)


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--mode", choices=("configuration", "boot-mmio"), default="configuration")
    for name in ("kernel", "disk-image", "readfile", "pcap", "pci-socket", "ethernet-socket"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--packet-rate", type=int, default=400000)
    parser.add_argument("--abs-max-tick", type=int, default=120000000000000)
    args = parser.parse_args()
    if not 1 <= args.packet_rate <= 800000 or not 1 < args.abs_max_tick <= m5.MaxTick:
        parser.error("packet rate must be 1..800000; tick bound must be 2..MaxTick")
    boot = args.mode == "boot-mmio"
    inputs = {}
    if boot:
        for name in ("kernel", "disk_image", "readfile", "pcap"):
            path = getattr(args, name)
            if path is None or not path.is_file():
                parser.error(f"boot-mmio requires existing --{name.replace('_', '-')}")
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1 << 20), b""):
                    digest.update(block)
            inputs[name] = dict(path=str(path.resolve()), sha256=digest.hexdigest())
        if args.pci_socket is None or args.ethernet_socket is None:
            parser.error("boot-mmio requires both synchronized adapter sockets")
    board = make_board(args.pci_socket or "/unused/configuration-pci.sock")
    if boot:
        board.set_kernel_disk_workload(
            kernel=KernelResource(str(args.kernel.resolve()), architecture=ISA.X86),
            disk_image=DiskImageResource(str(args.disk_image.resolve())),
            disk_device="/dev/sda", readfile=str(args.readfile.resolve()),
            kernel_args=KERNEL_ARGS,
        )
    else:
        board._set_fullsystem(True)
        board._attach_cachelens_network()
    root = board._pre_instantiate()
    root.i40e_network = SimBricksEthernet(
        uxsocket_path=str(args.ethernet_socket or "/unused/configuration-eth.sock"),
        shm_path="", listen=False, sync=True,
        link_latency="500ns", sync_tx_interval="500ns",
    )
    root.probe_link = EtherLink(speed="10Gbps", delay="200us")
    root.probe_sink = LoadGeneratorPcap(
        pcap_filename=str(args.pcap.resolve()) if args.pcap else "/unused/configuration.pcap",
        deferred_start=True, start_tick=0, stop_tick=args.abs_max_tick,
        max_packetsize=1472, stack_mode="DPDKStack", replay_mode="SimpleReplay",
        packet_rate=args.packet_rate, exit_on_eof=False,
        replace_src_ip="10.10.10.11", replace_dest_ip="10.10.10.10",
    )
    root.i40e_network.int0 = root.probe_link.int0
    root.probe_sink.interface = root.probe_link.int1
    evidence = check_topology(board)
    evidence.update(mode=args.mode, inputs=inputs, accuracy_ready=False)
    Path(m5.options.outdir, "accuracy-configuration.json").write_text(json.dumps(evidence, indent=2) + "\n")
    if not boot:
        print("ACCURACY_CONFIGURATION_ONLY_OK (not instantiated)")
        return
    board.exit_on_work_items = True
    m5.instantiate()
    board._post_instantiate()
    events = []
    started = ended = False
    while m5.curTick() < args.abs_max_tick:
        event = m5.simulate(args.abs_max_tick - m5.curTick())
        cause = event.getCause()
        events.append(dict(tick=m5.curTick(), cause=cause, code=event.getCode()))
        Path(m5.options.outdir, "accuracy-boot-events.json").write_text(json.dumps(events, indent=2) + "\n")
        if cause == "workbegin" and not started and event.getCode() == 0:
            started = True
            root.probe_sink.startReplay()
        elif cause == "workend" and started and not ended and event.getCode() == 0:
            ended = True
            m5.stats.dump()  # Boot diagnostics only: no accuracy ROI/reset.
        elif cause == "m5_exit instruction encountered" and ended and event.getCode() == 0:
            print("ACCURACY_FUNCTIONAL_GUEST_EXIT (inspect guest RSS checks; NOT accuracy)")
            return
        else:
            raise RuntimeError(f"unexpected functional boot exit: {cause}, code={event.getCode()}")
    raise RuntimeError("functional boot reached tick bound without guest exit")


if __name__ == "__m5_main__":
    main()
