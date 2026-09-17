"""Asset-free tagged-DMA policy proof, NOT SimBricks/i40e integration.

The existing tester requires two data-only CPU ports. It uses no instruction
execution or guest assets; LLC policy/geometry match the eight-core config.
Adapters follow tests/gem5/cachelens/configs/chi_dma_directed.py.
"""
import argparse
import sys
from pathlib import Path
from types import SimpleNamespace

import m5
from m5.objects import (AddrRange, MESIDDIODirectedTester, Root, SimpleMemory,
                        SrcClockDomain, System, VoltageDomain)
from gem5.isas import ISA
from gem5.prebuilt.cachelens.cache_hierarchy import CacheLensCHIHierarchy

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "configs/simbricks"))
from i40e_accuracy import GEOMETRY, XEON_PILOT_BOARD_CLOCK


class DirectedCore:
    """Only the data port is driven; no instruction fetch, MMU or interrupts."""

    def __init__(self, tester):
        self.tester = tester

    def get_simobject(self):
        # No instruction decoder: the tester sends data requests only.
        return SimpleNamespace(decoder=())

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

    def get_num_cores(self):
        return len(self.cores)

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
        return self.system.cache_line_size

    def get_clock_domain(self):
        return self.system.clk_domain

    def has_io_bus(self):
        return False

    def has_dma_ports(self):
        return True

    def get_dma_ports(self):
        return [self.system.tester.dma_race_port, self.system.tester.dma_port]

    def get_memory(self):
        return self

    def get_uninterleaved_range(self):
        return self.system.mem_ranges

    def get_mem_ports(self):
        return [(self.system.mem_ranges[0], self.system.memory.port)]

    def connect_system_port(self, port):
        self.system.system_port = port


parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
parser.add_argument("--scenario", choices=("hit_update", "nic_cpu_nic", "stash_unclassified"), default="hit_update")
args = parser.parse_args()
system = System(
    clk_domain=SrcClockDomain(clock=XEON_PILOT_BOARD_CLOCK, voltage_domain=VoltageDomain()),
    mem_ranges=[AddrRange("64MiB")], mem_mode="timing", cache_line_size=64,
)
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.tester = MESIDDIODirectedTester(scenario=args.scenario, block_size=64)
system.cache_hierarchy = CacheLensCHIHierarchy(**GEOMETRY)
hierarchy = system.cache_hierarchy
hierarchy.incorporate_cache(DirectedBoard(system))
hierarchy.ruby_system.randomization = False
assert len(hierarchy.dma_requestors) == 2
assert hierarchy.dma_requestors[1].sequencer.in_ports[0].peer is system.tester.dma_port
assert hierarchy.dma_requestors[0].sequencer.in_ports[0].peer is system.tester.dma_race_port
assert list(hierarchy.dma_requestors[1].downstream_destinations) == list(hierarchy.hnfs)
assert len(hierarchy.hnfs) == 8
assert hierarchy.get_total_hnf_capacity_bytes() == 20 * (1 << 20)
for node in hierarchy.hnfs:
    assert int(node.cache.ddio_way_part) == 2 and int(node.cache.assoc) == 20
    assert bool(node.cache.addr_hash) and bool(node.nic_read_no_allocate)
    assert bool(node.alloc_on_readunique) and bool(node.dealloc_backinv_unique)
    assert bool(node.dealloc_backinv_shared) and not bool(node.dealloc_on_unique)
    assert not bool(node.enable_DMT) and not bool(node.enable_DCT)
root = Root(full_system=False, system=system)
m5.ticks.setGlobalFrequency("1ps")
m5.instantiate()
event = m5.simulate(10000000000)
assert event.getCause() == "MESI DDIO directed scenario completed", event.getCause()

# Tagged requests are counted at HNF acceptance, never inferred from CPU hits.
expected = dict(rxPayloadRequests=2, rxPayloadHits=1, rxPayloadMisses=1,
                txPayloadRequests=1, txPayloadHits=1, txPayloadMisses=0,
                nicDmaWriteRequests=2, nicDmaReadRequests=1,
                nicDescWriteRequests=0, nicDescReadRequests=0,
                rxHeaderRequests=0, rdmaRxPayloadRequests=0, rdmaTxPayloadRequests=0)
if args.scenario == "stash_unclassified":
    # Scenario name is historical: stashing is OFF. Descriptor/header/generic
    # and CPU operations must not become RX payload requests. TX retention is
    # not asserted here: this test checks classification, not allocation.
    expected.update(rxPayloadRequests=0, rxPayloadHits=0, rxPayloadMisses=0,
                    txPayloadRequests=3, nicDmaReadRequests=4,
                    nicDescWriteRequests=1, nicDescReadRequests=1,
                    rxHeaderRequests=1)
    del expected["txPayloadHits"], expected["txPayloadMisses"]
for name, value in expected.items():
    total = 0
    for node in hierarchy.hnfs:
        stat = node.cache.resolveStat(name)
        stat.prepare()
        total += stat.value
        print(f"ACCURACY_POLICY_STAT {node.cache.path()}.{name}={stat.value}")
    assert total == value, (name, total, value)
if args.scenario != "stash_unclassified":
    bank = hierarchy.hnf_index_for_address(int(system.tester.base_addr))
    for index, node in enumerate(hierarchy.hnfs):
        requests = node.cache.resolveStat("rxPayloadRequests")
        requests.prepare()
        assert requests.value == (2 if index == bank else 0)
        fills = node.cache.resolveStat("ddioWayFill")
        fills.prepare()
        # Source row zero is RX payload; unlike rxPayloadAllocWays this
        # histogram records all ways, so an out-of-partition fill is visible.
        payload_ways = list(fills.value)[:20]
        assert sum(payload_ways[2:]) == 0, payload_ways
        assert sum(payload_ways[:2]) == (1 if index == bank else 0)
m5.stats.dump()
print(f"ACCURACY_TAGGED_DMA_POLICY_OK scenario={args.scenario}; NOT i40e integration")
