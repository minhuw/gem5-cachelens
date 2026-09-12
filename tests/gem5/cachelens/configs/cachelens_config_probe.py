# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse

import m5

from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.prebuilt.cachelens.cache_hierarchy import CacheLensCHIHierarchy
from gem5.resources.resource import obtain_resource
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

parser = argparse.ArgumentParser()
parser.add_argument("--isa", choices=("arm", "x86"), required=True)
parser.add_argument("--resource-directory")
parser.add_argument("--max-ticks", type=int)
parser.add_argument("--cpu-type", choices=("timing", "o3"), default="timing")
parser.add_argument("--model-profile")
parser.add_argument(
    "--hnf-inclusion",
    choices=("noninclusive", "inclusive"),
    default="noninclusive",
)
args = parser.parse_args()

isa = ISA.ARM if args.isa == "arm" else ISA.X86
requires(isa_required=isa)

model_profile = args.model_profile or "abstract"
ddio_way_part = 2 if model_profile in ("abstract", "intel-ddio") else None
cpu_type = CPUTypes.O3 if args.cpu_type == "o3" else CPUTypes.TIMING
hierarchy = CacheLensCHIHierarchy(
    hnf_size="1MiB",
    hnf_assoc=8,
    num_hnfs=2,
    ddio_way_part=ddio_way_part,
    indexing_policy="linear",
    dealloc_on_unique=False,
    hnf_inclusion=args.hnf_inclusion,
    model_profile=model_profile,
    core_clock="3GHz",
    link_latency=2,
    router_latency=1,
    network_buffer_size=8,
)
processor = SimpleProcessor(
    cpu_type=cpu_type,
    isa=isa,
    num_cores=2,
)
memory = DualChannelDDR4_2400(size="8GiB")
network_options = {"num_nics": 1, "num_loadgens": 1}

if isa == ISA.ARM:
    from m5.objects import Armv81

    from gem5.prebuilt.cachelens.board import CacheLensArmBoard

    board = CacheLensArmBoard(
        clk_freq="3GHz",
        processor=processor,
        memory=memory,
        cache_hierarchy=hierarchy,
        release=Armv81(),
        network_options=network_options,
    )
    resource = obtain_resource(
        "arm-ubuntu-24.04-boot-with-systemd",
        resource_directory=args.resource_directory,
    )
else:
    from gem5.prebuilt.cachelens.board import CacheLensX86Board

    board = CacheLensX86Board(
        clk_freq="3GHz",
        processor=processor,
        memory=memory,
        cache_hierarchy=hierarchy,
        network_options=network_options,
    )
    resource = obtain_resource(
        "x86-ubuntu-18.04-boot",
        resource_directory=args.resource_directory,
        resource_version="2.0.0",
    )

board.set_workload(resource)
assert board.get_disk_device() == "/dev/sda"
assert board.get_nic_bdfs() == ["0000:00:02.0"]
if isa == ISA.X86:
    assert [
        (int(memory_range.start), int(memory_range.size()))
        for memory_range in board.main_mem_ranges
    ] == [(0, 3 << 30), (4 << 30, 5 << 30)]
    assert any(
        memory_range.contains(0xC0002800)
        for memory_range in board.mem_ranges
    )
    assert not any(
        memory_range.contains(0xC0002800)
        for memory_range in board.main_mem_ranges
    )

simulator = Simulator(
    board=board,
    max_ticks=args.max_ticks if args.max_ticks is not None else m5.MaxTick,
)
simulator.run()

assert len(hierarchy.core_clusters) == 2
assert all(core.get_type() == cpu_type for core in processor.get_cores())
assert len(hierarchy.hnfs) == 2
expected_ddio = 2 if ddio_way_part is not None else -1
assert int(hierarchy.hnfs[0].cache.ddio_way_part) == expected_ddio
assert bool(hierarchy.hnfs[0].nic_read_no_allocate) == (
    model_profile == "intel-ddio"
)
assert not bool(hierarchy.hnfs[0].cache.addr_hash)
assert hierarchy.get_indexing_policy() == "linear"
assert hierarchy.get_model_profile() == model_profile
assert hierarchy.get_total_hnf_capacity_bytes() == 2 << 20
assert len(hierarchy.get_hnf_ranges()) == 2
assert {hierarchy.hnf_index_for_address(line * 64) for line in range(32)} == {
    0,
    1,
}
assert not bool(hierarchy.ruby_system.cache_trace_warmup)
assert hierarchy.get_cache_state_restore_policy() == "cold"
assert not hierarchy.get_configuration()["cache_state_continuity"]
assert int(hierarchy.ruby_system.network.buffer_size) == 8
assert int(hierarchy.ruby_system.network.routers[0].int_routing_latency) == 1
assert int(hierarchy.ruby_system.network.int_links[0].latency) == 2
assert int(hierarchy.ruby_system.network.ext_links[0].latency) == 2
hnf = hierarchy.hnfs[0]
assert bool(hnf.enable_DMT) == (args.hnf_inclusion == "noninclusive")
assert bool(hnf.enable_DCT) == (args.hnf_inclusion == "noninclusive")
assert bool(hnf.alloc_on_readshared)
assert bool(hnf.alloc_on_readunique) == (args.hnf_inclusion == "inclusive")
assert bool(hnf.alloc_on_readonce)
assert bool(hnf.alloc_on_writeback)
assert not bool(hnf.dealloc_on_unique)
assert not bool(hnf.dealloc_on_shared)
assert bool(hnf.dealloc_backinv_unique) == (
    args.hnf_inclusion == "inclusive"
)
assert bool(hnf.dealloc_backinv_shared) == (
    args.hnf_inclusion == "inclusive"
)
assert hierarchy.get_configuration()["hnf_inclusion"] == args.hnf_inclusion
assert len(hierarchy.dma_requestors) == len(board.get_dma_ports())
if isa == ISA.X86:
    # The PCI-hole split is guest-visible only. There is still one
    # dual-channel physical controller group, and Ruby/CHI gets one routing
    # controller per disjoint guest range.
    assert len(memory.get_memory_controllers()) == 2
    assert len(hierarchy.memory_controllers) == 2

    pci_mp_routes = [
        entry
        for entry in board._mp_base_entries
        if type(entry).__name__ == "X86IntelMPIOIntAssignment"
        and int(entry.source_bus_id) == 0
    ]
    assert {
        (int(entry.source_bus_irq), int(entry.dest_io_apic_intin))
        for entry in pci_mp_routes
    } == {(16, 16), (8, 17)}

    # PCI device/pin routes must not be represented as MADT Interrupt Source
    # Overrides.  Those records are only used above for ISA IRQ overrides.
    assert all(
        not (
            type(record).__name__ == "X86ACPIMadtIntSourceOverride"
            and int(record.bus_source) == 0
        )
        for record in board._madt_entries
    )
    assert int(board.pc.south_bridge.ide.InterruptLine) == 16
    assert int(board.nics[0].InterruptLine) == 17

print(
    f"CACHELENS_CONFIG_OK isa={args.isa} cpu={args.cpu_type} "
    f"profile={hierarchy.get_model_profile()} "
    f"hnf_inclusion={args.hnf_inclusion} "
    f"indexing={hierarchy.get_indexing_policy()} "
    f"hnf_capacity={hierarchy.get_total_hnf_capacity_bytes()} "
    f"cache_restore={hierarchy.get_cache_state_restore_policy()} "
    "cache_continuity=false network=link:2,router:1,buffer:8"
)
