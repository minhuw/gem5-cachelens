# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""No guest/resources: drive the production CacheLens hierarchy with timing IO."""

import argparse

import m5
from m5.objects import (
    AddrRange,
    MESIDDIODirectedTester,
    Root,
    SimpleMemory,
    SrcClockDomain,
    System,
    VoltageDomain,
)

from gem5.isas import ISA
from gem5.prebuilt.cachelens.mesi_three_level_cache_hierarchy import (
    CacheLensMESIThreeLevelHierarchy,
)


class DirectedCore:
    """Only the data port is driven; no instruction fetch, MMU or interrupts."""

    def __init__(self, tester):
        self.tester = tester

    def get_simobject(self):
        return self.tester

    def requires_send_evicts(self):
        return False

    def connect_dcache(self, port):
        self.tester.cpu_ports = port

    def connect_icache(self, port):
        pass

    def connect_walker_ports(self, *ports):
        pass

    def connect_interrupt(self, *ports):
        pass


class DirectedProcessor:
    def __init__(self, tester):
        self.cores = [DirectedCore(tester), DirectedCore(tester)]

    def get_cores(self):
        return self.cores

    def get_isa(self):
        return ISA.X86


class DirectedBoard:
    """Test-only adapter; all SimObjects remain owned by the real System."""

    def __init__(self, system):
        self.system = system
        self.processor = DirectedProcessor(system.tester)

    def get_processor(self):
        return self.processor

    def get_cache_line_size(self):
        return 64

    def get_clock_domain(self):
        return self.system.clk_domain

    def has_io_bus(self):
        return False

    def has_dma_ports(self):
        return True

    def get_dma_ports(self):
        return [self.system.tester.dma_port]

    def get_mem_ports(self):
        return [(self.system.mem_ranges[0], self.system.memory.port)]

    def connect_system_port(self, port):
        self.system.system_port = port


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--scenario", required=True)
parser.add_argument("--num-hnfs", type=int, choices=(1, 2), default=1)
parser.add_argument("--abs-max-tick", type=int, default=10000000000)
args = parser.parse_args()

system = System(
    clk_domain=SrcClockDomain(clock="1GHz", voltage_domain=VoltageDomain()),
    mem_ranges=[AddrRange("64MiB")],
    mem_mode="timing",
    cache_line_size=64,
)
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.tester = MESIDDIODirectedTester(
    scenario=args.scenario,
    block_size=64,
    routing_banks=args.num_hnfs,
    set_stride=128 * args.num_hnfs,
)
system.cache_hierarchy = CacheLensMESIThreeLevelHierarchy(
    l1i_size="256B",
    l1i_assoc=2,
    l1d_size="256B",
    l1d_assoc=2,
    l2_size="256B",
    l2_assoc=2,
    hnf_size="512B",
    hnf_assoc=4,
    num_hnfs=args.num_hnfs,
    ddio_way_part=1,
    core_clock="2GHz",
    indexing_policy="linear",
)
hierarchy = system.cache_hierarchy
hierarchy.incorporate_cache(DirectedBoard(system))
ruby = hierarchy.ruby_system
ruby.randomization = False

metadata = hierarchy.get_configuration()
assert metadata["coherence_protocol"] == "MESI_Three_Level"
assert metadata["hnf_inclusion"] == "inclusive"
assert metadata["nic_read_no_allocate"] is True
assert metadata["private_l2_size_bytes"] == 256
assert metadata["total_hnf_capacity_bytes"] == 512 * args.num_hnfs
assert metadata["core_clock"] == "2GHz"
assert metadata["hnf_clock"] == "board"
assert metadata["bank_select_low_bit"] == 6
assert metadata["bank_select_bits"] == args.num_hnfs.bit_length() - 1
assert len(hierarchy.l1_controllers) == len(hierarchy.l2_controllers) == 2
assert len(hierarchy.l3_controllers) == args.num_hnfs
assert len(hierarchy.directory_controllers) == 1
assert len(hierarchy.dma_controllers) == 1
for l1, l2 in zip(hierarchy.l1_controllers, hierarchy.l2_controllers):
    assert l1.type == "MESI_Three_Level_L0Cache_Controller"
    assert l2.type == "MESI_Three_Level_L1Cache_Controller"
    assert l2.bufferFromL0 is l1.bufferToL1
    assert l2.bufferToL0 is l1.bufferFromL1
    assert l1.clk_domain is hierarchy.core_clk_domain
    assert l2.clk_domain is hierarchy.core_clk_domain
    assert int(l2.l2_select_num_bits) == metadata["bank_select_bits"]
    l1.sequencer.deadlock_threshold = 1000000
for bank, l3 in enumerate(hierarchy.l3_controllers):
    assert l3.type == "MESI_Three_Level_L2Cache_Controller"
    assert int(l3.L2cache.start_index_bit) == 6 + metadata["bank_select_bits"]
    assert int(l3.L2cache.ddio_way_part) == 1
    assert not bool(l3.L2cache.addr_hash)
    assert hierarchy.hnf_index_for_address(0x10000 + bank * 64) == bank
for dma in hierarchy.dma_controllers:
    assert bool(dma.dma_sequencer.supports_masked_writes)
    assert int(dma.l2_select_num_bits) == metadata["bank_select_bits"]

root = Root(full_system=False, system=system)
# Unlike the legacy 1ns test clock, preserve the distinct 2GHz core period.
m5.ticks.setGlobalFrequency("1ps")
m5.instantiate()
for l3 in hierarchy.l3_controllers:
    assert l3.clk_domain is system.clk_domain
print("CacheLens native three-level hierarchy assertions passed")
exit_event = m5.simulate(args.abs_max_tick)
print("Exiting @ tick", m5.curTick(), "because", exit_event.getCause())
if exit_event.getCause() != "MESI DDIO directed scenario completed":
    raise RuntimeError(f"Unexpected simulation exit: {exit_event.getCause()}")
