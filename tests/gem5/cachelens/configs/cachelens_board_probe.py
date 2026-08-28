# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
from pathlib import Path

import m5

from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA

parser = argparse.ArgumentParser()
parser.add_argument("--isa", choices=("arm", "x86"), required=True)
parser.add_argument("--num-nics", type=int, required=True)
parser.add_argument("--enable-pvrdma", action="store_true")
parser.add_argument("--expect-reject", action="store_true")
parser.add_argument(
    "--hierarchy",
    choices=("classic", "mesi-two-level"),
    default="classic",
)
parser.add_argument("--phase", choices=("save", "restore"))
parser.add_argument("--checkpoint", type=Path)
parser.add_argument("--kernel", type=Path)
args = parser.parse_args()

isa = ISA.ARM if args.isa == "arm" else ISA.X86
if args.phase and (isa != ISA.X86 or not args.enable_pvrdma):
    parser.error("Checkpoint probes require x86 PVRDMA.")
if args.phase and (args.checkpoint is None or args.kernel is None):
    parser.error("Checkpoint probes require --checkpoint and --kernel.")
if args.hierarchy == "mesi-two-level":
    from gem5.prebuilt.cachelens.mesi_two_level_cache_hierarchy import (
        CacheLensMESITwoLevelHierarchy,
    )

    cache_hierarchy = CacheLensMESITwoLevelHierarchy(
        l1i_size="16KiB",
        l2_size="32KiB",
        l2_assoc=4,
        hnf_size="64KiB",
        hnf_assoc=4,
        model_profile="x86-generic",
    )
else:
    cache_hierarchy = NoCache()
processor = SimpleProcessor(
    cpu_type=(
        CPUTypes.TIMING
        if args.hierarchy == "mesi-two-level"
        else CPUTypes.ATOMIC
    ),
    isa=isa,
    num_cores=1,
)
memory = DualChannelDDR4_2400(size="2GiB")
network_options = {
    "num_nics": args.num_nics,
    "num_loadgens": args.num_nics,
}
if args.enable_pvrdma:
    network_options["enable_pvrdma"] = True

board_type = None
if isa == ISA.ARM:
    from gem5.prebuilt.cachelens.board import CacheLensArmBoard

    board_type = CacheLensArmBoard
else:
    from gem5.prebuilt.cachelens.board import CacheLensX86Board

    board_type = CacheLensX86Board

try:
    board = board_type(
        clk_freq="1GHz",
        processor=processor,
        memory=memory,
        cache_hierarchy=cache_hierarchy,
        network_options=network_options,
    )
except ValueError as error:
    if not args.expect_reject:
        raise
    message = str(error)
    if isa == ISA.ARM:
        assert "at most three NICs" in message
        assert "IDE controller's INTx route" in message
    else:
        assert (
            "supports one NIC" in message
            or "requires exactly one CacheLens NIC" in message
        )
    print(
        f"CACHELENS_BOARD_REJECT_OK isa={args.isa} "
        f"nics={args.num_nics} error={message}"
    )
    raise SystemExit(0)

assert not args.expect_reject, "NIC count was expected to be rejected"


# The default board-attachment probes stay Python-only. The focused PVRDMA
# checkpoint phases below also instantiate the same board.
class _DiskImage:
    def get_local_path(self) -> str:
        return Path(__file__).as_posix()


board._set_fullsystem(True)
board._add_disk_to_board(_DiskImage())

expected_bdfs = [
    f"0000:00:{2 + index:02x}.0" for index in range(args.num_nics)
]
assert board.get_nic_bdfs() == expected_bdfs
assert len(board.nics) == args.num_nics
assert len(board.loadgens) == args.num_nics
assert len(board.links) == args.num_nics
assert [str(nic.hardware_address) for nic in board.nics] == [
    f"02:90:00:00:00:{index + 1:02x}" for index in range(args.num_nics)
]
assert [int(loadgen.loadgen_id) for loadgen in board.loadgens] == list(
    range(args.num_nics)
)

if isa == ISA.X86:
    assert len(board.rdmas) == int(args.enable_pvrdma)
    assert len(board.get_dma_ports()) == 1 + args.num_nics + len(board.rdmas)
    assert board.get_dma_ports()[0] is board.pc.south_bridge.ide.dma
    if args.num_nics:
        nic = board.nics[0]
        assert board.get_dma_ports()[1] is nic.dma
        assert bool(int(nic.HeaderType) & 0x80) == args.enable_pvrdma
        assert int(nic.HeaderType) & 0x7F == 0
    if args.enable_pvrdma:
        rdma = board.rdmas[0]
        assert board.get_rdma_bdfs() == ["0000:00:02.1"]
        assert rdma.get_parent() is board
        assert str(rdma.hardware_address) == str(nic.hardware_address)
        assert int(rdma.VendorID) == 0x15AD
        assert int(rdma.DeviceID) == 0x0820
        assert int(rdma.Revision) == 1
        assert int(rdma.SubsystemVendorID) == 0x15AD
        assert int(rdma.SubsystemID) == 1
        assert int(rdma.ClassCode) == 0x02
        assert int(rdma.SubClassCode) == 0x80
        assert int(rdma.ProgIF) == 0
        assert int(rdma.HeaderType) & 0x80 == 0
        assert int(rdma.HeaderType) & 0x7F == 0
        assert [
            int(rdma.BAR0.size),
            int(rdma.BAR1.size),
            int(rdma.BAR2.size),
        ] == [
            16 << 10,
            256,
            2 << 20,
        ]
        assert int(rdma.InterruptPin) == 2
        assert int(rdma.InterruptLine) == 18
        assert rdma.host is board.pc.pci_host
        assert rdma.pio.peer.simobj is board.iobus
        assert board.get_dma_ports()[2] is rdma.dma
        assert rdma.dma is not nic.dma
        pci_mp_routes = [
            entry
            for entry in board._mp_base_entries
            if type(entry).__name__ == "X86IntelMPIOIntAssignment"
            and int(entry.source_bus_id) == 0
        ]
        assert {
            (int(entry.source_bus_irq), int(entry.dest_io_apic_intin))
            for entry in pci_mp_routes
        } == {(16, 16), (8, 17), (9, 18)}
    else:
        assert board.get_rdma_bdfs() == []

if isa == ISA.ARM:
    pci_devices = [int(device.pci_dev) for device in board._pci_devices]
    assert pci_devices == list(range(1, args.num_nics + 2))
    int_count = int(board.realview.pci_host.int_count)
    assert len({device % int_count for device in pci_devices}) == len(
        pci_devices
    )

if args.phase:
    board.workload.object_file = args.kernel.as_posix()
    root = board._pre_instantiate()
    dma_ports = board.get_dma_ports()
    assert dma_ports == [
        board.pc.south_bridge.ide.dma,
        board.nics[0].dma,
        board.rdmas[0].dma,
    ]
    if args.hierarchy == "classic":
        assert board.nics[0].dma.peer.simobj is board.iobus
        assert board.rdmas[0].dma.peer.simobj is board.iobus
    else:
        controllers = cache_hierarchy.dma_controllers
        assert len(controllers) == 3
        assert [int(ctrl.dma_sequencer.version) for ctrl in controllers] == [
            0,
            1,
            2,
        ]
        for controller, port in zip(controllers, dma_ports):
            assert len(controller.dma_sequencer.in_ports) == 1
            assert controller.dma_sequencer.in_ports[0].peer is port
    m5.instantiate(
        args.checkpoint.as_posix() if args.phase == "restore" else None
    )
    if args.phase == "save":
        args.checkpoint.mkdir(parents=True, exist_ok=True)
        m5.checkpoint(args.checkpoint.as_posix())
    print(
        "CACHELENS_PVRDMA_INSTANTIATED "
        f"hierarchy={args.hierarchy} phase={args.phase} "
        "dma=IDE,E1000,PVRDMA"
    )

print(
    f"CACHELENS_BOARD_CONFIG_OK isa={args.isa} nics={args.num_nics} "
    f"pvrdma={str(args.enable_pvrdma).lower()} "
    f"bdfs={','.join(expected_bdfs)}"
)
