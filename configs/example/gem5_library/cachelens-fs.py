# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Run CacheLens ARM or x86 full-system experiments with the gem5 stdlib.

Build with ``build/ARM/gem5.opt`` or ``build/X86_CHI/gem5.opt``. The x86
8 GiB layout contains 3 GiB below the PCI hole and 5 GiB beginning at 4 GiB;
the memory component packs both ranges behind the original dual-channel DDR.
Timing and O3 CacheLens restores are explicitly cold-cache restores, not
cache-continuous continuations of the checkpointed run.
"""

import argparse
from pathlib import Path

import m5
from m5.util.convert import toMemorySize

from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.prebuilt.cachelens.cache_hierarchy import CacheLensCHIHierarchy
from gem5.resources.resource import (
    BootloaderResource,
    DiskImageResource,
    KernelResource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires


def _create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run a modern CacheLens full-system experiment."
    )
    parser.add_argument("--isa", choices=("arm", "x86"), required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--disk-image", type=Path, required=True)
    parser.add_argument("--bootloader", type=Path)
    parser.add_argument("--disk-device")
    parser.add_argument("--readfile", type=Path)
    parser.add_argument(
        "--checkpoint",
        type=Path,
        help=(
            "Restore architectural, device, and backing-memory state. "
            "Timing/O3 CacheLens restores start with empty caches."
        ),
    )
    parser.add_argument(
        "--checkpoint-output-dir", type=Path, default=Path("m5out")
    )
    parser.add_argument("--max-checkpoints", type=int, default=1)
    parser.add_argument(
        "--checkpoint-at-max-tick",
        action="store_true",
        help="Save a checkpoint when --max-ticks expires.",
    )
    parser.add_argument("--max-ticks", type=int, default=m5.MaxTick)
    parser.add_argument(
        "--cpu-type",
        choices=("atomic", "timing", "o3"),
        default="timing",
        help="Atomic is checkpoint preparation; timing/o3 are measurement cores.",
    )
    parser.add_argument("--num-cores", type=int, default=2)
    parser.add_argument("--clock", default="2GHz", help="CPU clock.")
    parser.add_argument(
        "--system-clock", default="1GHz", help="Board and device clock."
    )
    parser.add_argument("--mem-size", default="8GiB")

    parser.add_argument("--num-nics", type=int, default=1)
    parser.add_argument("--num-loadgens", type=int, default=1)
    parser.add_argument(
        "--loadgen-type", choices=("Simple", "Pcap"), default="Simple"
    )
    parser.add_argument("--packet-rate", type=int, default=1)
    parser.add_argument("--packet-size", type=int, default=64)
    parser.add_argument("--loadgen-start", type=int, default=1)
    parser.add_argument("--loadgen-stop", type=int, default=m5.MaxTick)
    parser.add_argument(
        "--loadgen-mode",
        choices=("Static", "Increment", "Burst"),
        default="Static",
    )
    parser.add_argument("--loadgen-pcap-file")
    parser.add_argument(
        "--loadgen-pcap-max-packet-size",
        type=int,
        default=1514,
        help=(
            "Maximum full L2 frame size in bytes, including the 14-byte "
            "Ethernet header but excluding FCS (default: 1514)."
        ),
    )
    parser.add_argument("--loadgen-pcap-port-filter", type=int, default=11211)
    parser.add_argument(
        "--loadgen-pcap-stack-mode",
        choices=("KernelStack", "DPDKStack"),
        default="KernelStack",
    )
    parser.add_argument("--loadgen-pcap-src-ip", default="10.10.10.11")
    parser.add_argument("--loadgen-pcap-dest-ip", default="10.10.10.10")
    parser.add_argument(
        "--loadgen-pcap-replay-mode",
        choices=(
            "SimpleReplay",
            "ReplayAndAdjustThroughput",
            "ConstThroughput",
        ),
        default="SimpleReplay",
    )
    parser.add_argument(
        "--loadgen-pcap-increment-interval", type=int, default=1
    )
    parser.add_argument(
        "--loadgen-check-loss-interval", type=int, default=5000
    )
    parser.add_argument("--loadgen-check-loss-wait", default="1ms")
    parser.add_argument("--ethernet-link-speed", default="1Gbps")
    parser.add_argument("--ethernet-link-delay", default="200us")
    parser.add_argument("--nic-rx-fifo-size", default="48KiB")
    parser.add_argument("--nic-tx-fifo-size", default="16KiB")
    parser.add_argument("--loadgen-pcap-exit-on-eof", action="store_true")
    parser.add_argument("--loadgen-pcap-eof-drain-delay", default="1ms")

    parser.add_argument("--l1i-size", default="32KiB")
    parser.add_argument("--l1i-assoc", type=int, default=2)
    parser.add_argument("--l1d-size", default="64KiB")
    parser.add_argument("--l1d-assoc", type=int, default=2)
    parser.add_argument("--l2-size", default="1MiB")
    parser.add_argument("--l2-assoc", type=int, default=8)
    parser.add_argument("--hnf-size", default="1MiB")
    parser.add_argument("--hnf-assoc", type=int, default=8)
    parser.add_argument("--num-hnfs", type=int, default=1)
    parser.add_argument(
        "--model-profile",
        choices=("abstract", "x86-generic", "arm-generic", "intel-ddio"),
        help="Validated architecture/model profile; defaults from --isa.",
    )
    parser.add_argument("--ddio-way-part", type=int, default=None)
    parser.add_argument(
        "--indexing-policy",
        choices=("linear", "splitmix64"),
        default="linear",
        help="splitmix64 is an explicitly experimental set-index policy.",
    )
    parser.add_argument("--link-latency", type=int, default=1)
    parser.add_argument("--router-latency", type=int, default=1)
    parser.add_argument("--network-buffer-size", type=int, default=4)

    # Compatibility spellings for older experiment scripts.
    addr_hash = parser.add_mutually_exclusive_group()
    addr_hash.add_argument(
        "--addr-hash", dest="addr_hash", action="store_true"
    )
    addr_hash.add_argument(
        "--no-addr-hash", dest="addr_hash", action="store_false"
    )
    parser.set_defaults(addr_hash=None)

    dealloc = parser.add_mutually_exclusive_group()
    dealloc.add_argument(
        "--dealloc-on-unique",
        dest="dealloc_on_unique",
        action="store_true",
    )
    dealloc.add_argument(
        "--no-dealloc-on-unique",
        dest="dealloc_on_unique",
        action="store_false",
    )
    parser.set_defaults(dealloc_on_unique=False)
    return parser


def _validate_args(parser, args) -> None:
    for name in ("kernel", "disk_image"):
        path = getattr(args, name)
        if not path.is_file():
            parser.error(f"--{name.replace('_', '-')} is not a file: {path}")

    if args.isa == "arm":
        if args.bootloader is None:
            parser.error("--bootloader is required for ARM.")
        if not args.bootloader.is_file():
            parser.error(f"--bootloader is not a file: {args.bootloader}")
    elif args.bootloader is not None:
        parser.error("--bootloader is not valid for x86.")

    if args.model_profile == "intel-ddio" and args.isa == "arm":
        parser.error("--model-profile intel-ddio is only valid for x86.")
    if args.model_profile == "arm-generic" and args.isa != "arm":
        parser.error("--model-profile arm-generic requires ARM.")
    if args.model_profile == "x86-generic" and args.isa != "x86":
        parser.error("--model-profile x86-generic requires x86.")

    if args.readfile is not None and not args.readfile.is_file():
        parser.error(f"--readfile is not a file: {args.readfile}")
    if args.checkpoint is not None and not args.checkpoint.is_dir():
        parser.error(f"--checkpoint is not a directory: {args.checkpoint}")
    if args.num_cores <= 0:
        parser.error("--num-cores must be positive.")
    if args.max_checkpoints <= 0:
        parser.error("--max-checkpoints must be positive.")
    if args.max_ticks <= 0:
        parser.error("--max-ticks must be positive.")
    if args.link_latency <= 0 or args.router_latency <= 0:
        parser.error("Network latencies must be positive.")
    if args.network_buffer_size <= 0:
        parser.error("--network-buffer-size must be positive.")
    if (
        args.loadgen_type == "Pcap"
        and args.loadgen_pcap_max_packet_size < 14
    ):
        parser.error(
            "--loadgen-pcap-max-packet-size must allow at least the 14-byte "
            "full L2 Ethernet header."
        )
    if args.addr_hash is not None:
        compatibility_policy = "splitmix64" if args.addr_hash else "linear"
        if (
            args.indexing_policy != "linear"
            and args.indexing_policy != compatibility_policy
        ):
            parser.error("--addr-hash conflicts with --indexing-policy.")
        args.indexing_policy = compatibility_policy

    memory_size = toMemorySize(args.mem_size)
    if memory_size <= 0 or memory_size > toMemorySize("8GiB"):
        parser.error("--mem-size must be greater than zero and at most 8GiB.")


def _network_options(args):
    return {
        "num_nics": args.num_nics,
        "num_loadgens": args.num_loadgens,
        "loadgen_type": args.loadgen_type,
        "packet_rate": args.packet_rate,
        "packet_size": args.packet_size,
        "loadgen_start": args.loadgen_start,
        "loadgen_stop": args.loadgen_stop,
        "loadgen_mode": args.loadgen_mode,
        "pcap_filename": args.loadgen_pcap_file,
        "pcap_max_packet_size": args.loadgen_pcap_max_packet_size,
        "pcap_port_filter": args.loadgen_pcap_port_filter,
        "pcap_stack_mode": args.loadgen_pcap_stack_mode,
        "pcap_src_ip": args.loadgen_pcap_src_ip,
        "pcap_dest_ip": args.loadgen_pcap_dest_ip,
        "pcap_replay_mode": args.loadgen_pcap_replay_mode,
        "pcap_increment_interval": args.loadgen_pcap_increment_interval,
        "check_loss_interval": args.loadgen_check_loss_interval,
        "check_loss_wait": args.loadgen_check_loss_wait,
        "link_speed": args.ethernet_link_speed,
        "link_delay": args.ethernet_link_delay,
        "rx_fifo_size": args.nic_rx_fifo_size,
        "tx_fifo_size": args.nic_tx_fifo_size,
        "pcap_exit_on_eof": args.loadgen_pcap_exit_on_eof,
        "pcap_eof_drain_delay": args.loadgen_pcap_eof_drain_delay,
    }


def _checkpoint_handler(output_dir: Path, maximum: int):
    output_dir.mkdir(parents=True, exist_ok=True)
    checkpoints = 0
    while True:
        path = output_dir / f"cpt.{m5.curTick()}"
        m5.checkpoint(path.as_posix())
        checkpoints += 1
        print(f"Saved checkpoint {checkpoints}/{maximum}: {path}")
        yield checkpoints >= maximum


def _max_tick_checkpoint_handler(output_dir: Path):
    output_dir.mkdir(parents=True, exist_ok=True)
    path = output_dir / f"cpt.{m5.curTick()}"
    m5.checkpoint(path.as_posix())
    print(f"Saved max-tick checkpoint: {path}")
    yield True


def _continue_work_items_handler():
    while True:
        yield False


def main() -> None:
    parser = _create_parser()
    args = parser.parse_args()
    _validate_args(parser, args)

    isa = ISA.ARM if args.isa == "arm" else ISA.X86
    requires(isa_required=isa)
    cpu_type = {
        "atomic": CPUTypes.ATOMIC,
        "timing": CPUTypes.TIMING,
        "o3": CPUTypes.O3,
    }[args.cpu_type]
    processor = SimpleProcessor(
        cpu_type=cpu_type,
        isa=isa,
        num_cores=args.num_cores,
    )
    # CacheLensCHIHierarchy assigns the measurement CPU cores and private
    # caches to one explicit core clock domain. Atomic preparation keeps the
    # traditional independent CPU clock assignment below.
    if args.cpu_type == "atomic":
        from m5.objects import (
            SrcClockDomain,
            VoltageDomain,
        )

        cpu_voltage = VoltageDomain()
        for core in processor.get_cores():
            core.get_simobject().clk_domain = SrcClockDomain(
                clock=args.clock, voltage_domain=cpu_voltage
            )
    memory = DualChannelDDR4_2400(size=args.mem_size)
    if args.cpu_type == "atomic":
        # Checkpoint preparation must not use Ruby's atomic_noncaching mode.
        # In particular, e1000 PMD initialization performs device DMA which
        # can stall behind an atomic CHI RNI. Like gem5's established FS
        # checkpoint workflow, prepare architectural/device state with a
        # modern uncached hierarchy, then restore it into CacheLens CHI with
        # Timing cores for measurement.
        cache_hierarchy = NoCache()
        hierarchy_name = "no-cache-checkpoint-prep"
    else:
        model_profile = args.model_profile or (
            "arm-generic" if isa == ISA.ARM else "x86-generic"
        )
        cache_hierarchy = CacheLensCHIHierarchy(
            l1i_size=args.l1i_size,
            l1i_assoc=args.l1i_assoc,
            l1d_size=args.l1d_size,
            l1d_assoc=args.l1d_assoc,
            l2_size=args.l2_size,
            l2_assoc=args.l2_assoc,
            hnf_size=args.hnf_size,
            hnf_assoc=args.hnf_assoc,
            num_hnfs=args.num_hnfs,
            ddio_way_part=args.ddio_way_part,
            indexing_policy=args.indexing_policy,
            addr_hash=args.addr_hash,
            dealloc_on_unique=args.dealloc_on_unique,
            model_profile=model_profile,
            core_clock=args.clock,
            link_latency=args.link_latency,
            router_latency=args.router_latency,
            network_buffer_size=args.network_buffer_size,
        )
        hierarchy_name = "cachelens-chi"

    network_options = _network_options(args)
    if args.cpu_type == "atomic":
        # Atomic mode is checkpoint preparation only. Keep the generator
        # dormant so the resulting checkpoint can take its measurement rate
        # and window from a later Timing restore command.
        network_options["loadgen_start"] = m5.MaxTick
        network_options["loadgen_stop"] = m5.MaxTick

    if isa == ISA.ARM:
        from m5.objects import Armv81

        from gem5.prebuilt.cachelens.board import CacheLensArmBoard

        board = CacheLensArmBoard(
            clk_freq=args.system_clock,
            processor=processor,
            memory=memory,
            cache_hierarchy=cache_hierarchy,
            release=Armv81(),
            network_options=network_options,
        )
        bootloader = BootloaderResource(args.bootloader.as_posix())
        disk_device = args.disk_device or "/dev/sda"
    else:
        from gem5.prebuilt.cachelens.board import CacheLensX86Board

        board = CacheLensX86Board(
            clk_freq=args.system_clock,
            processor=processor,
            memory=memory,
            cache_hierarchy=cache_hierarchy,
            network_options=network_options,
        )
        bootloader = None
        disk_device = args.disk_device or "/dev/sda"

    board.set_kernel_disk_workload(
        kernel=KernelResource(args.kernel.as_posix(), architecture=isa),
        disk_image=DiskImageResource(args.disk_image.as_posix()),
        bootloader=bootloader,
        disk_device=disk_device,
        readfile=args.readfile.as_posix() if args.readfile else None,
        checkpoint=args.checkpoint,
    )

    reported_ddio = getattr(
        cache_hierarchy, "_ddio_way_part", args.ddio_way_part
    )
    hierarchy_config = (
        cache_hierarchy.get_configuration()
        if hasattr(cache_hierarchy, "get_configuration")
        else {}
    )
    print(
        "CacheLens FS: "
        f"isa={args.isa} cpu={args.cpu_type} cores={args.num_cores} "
        f"hierarchy={hierarchy_name} memory={args.mem_size} "
        f"profile={hierarchy_config.get('model_profile', 'checkpoint-prep')} "
        f"indexing={hierarchy_config.get('indexing_policy', 'n/a')} "
        f"hnfs={args.num_hnfs} "
        f"hnf_capacity={hierarchy_config.get('total_hnf_capacity_bytes', 'n/a')} "
        f"ddio_way_part={reported_ddio} "
        f"network=link:{args.link_latency},"
        f"router:{args.router_latency},buffer:{args.network_buffer_size} "
        f"nics={','.join(board.get_nic_bdfs()) or 'none'} "
        f"restore={args.checkpoint or 'none'} "
        "cache_restore_policy="
        f"{hierarchy_config.get('cache_state_restore_policy', 'n/a')} "
        "cache_continuity="
        f"{str(hierarchy_config.get('cache_state_continuity', False)).lower()}"
    )

    exit_handlers = {
        ExitEvent.CHECKPOINT: _checkpoint_handler(
            args.checkpoint_output_dir, args.max_checkpoints
        ),
        # The CAL image uses work-item pseudo-ops internally. CacheLens
        # measurement intervals are controlled by load-generator ticks, so
        # these markers must not reset statistics or terminate simulation.
        ExitEvent.WORKBEGIN: _continue_work_items_handler(),
        ExitEvent.WORKEND: _continue_work_items_handler(),
    }
    if args.checkpoint_at_max_tick:
        exit_handlers[ExitEvent.MAX_TICK] = _max_tick_checkpoint_handler(
            args.checkpoint_output_dir
        )

    simulator = Simulator(
        board=board,
        on_exit_event=exit_handlers,
        max_ticks=args.max_ticks,
    )
    simulator.run()


if __name__ == "__m5_main__":
    main()
