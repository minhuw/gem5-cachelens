# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""Asset-free CHI data-value checks, not guest boot or DVM/ISA validation.

Two requestor adapters drive real private caches using the existing directed
CPU/DMA tester. Empty decoder lists deliberately issue no instructions/DVM.
No external TLM library, guest resources, or protocol policy overrides.
"""

import argparse
from types import SimpleNamespace

import m5
from m5.objects import (
    AddrRange, MESIDDIODirectedTester, Root, SimpleMemory, SrcClockDomain,
    System, VoltageDomain,
)
from gem5.isas import ISA
from gem5.prebuilt.cachelens.cache_hierarchy import CacheLensCHIHierarchy


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
        return ISA.ARM


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
        return [self.system.tester.dma_port]

    def get_memory(self):
        return self

    def get_uninterleaved_range(self):
        return self.system.mem_ranges

    def get_mem_ports(self):
        return [(self.system.mem_ranges[0], self.system.memory.port)]

    def connect_system_port(self, port):
        self.system.system_port = port


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--scenario", required=True, choices=(
    "cold_full_write", "partial_dirty_owner", "pvrdma_provenance",
    "zero_mask", "sparse_mask",  # Unsupported by RubySequencer: must fail.
))
parser.add_argument("--abs-max-tick", type=int, default=10000000000)
args = parser.parse_args()

system = System(
    clk_domain=SrcClockDomain(clock="1GHz", voltage_domain=VoltageDomain()),
    mem_ranges=[AddrRange("64MiB")], mem_mode="timing", cache_line_size=64,
)
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.tester = MESIDDIODirectedTester(scenario=args.scenario, block_size=64)
system.cache_hierarchy = CacheLensCHIHierarchy(
    l1i_size="256B", l1i_assoc=2, l1d_size="256B", l1d_assoc=2,
    l2_size="256B", l2_assoc=2, hnf_size="512B", hnf_assoc=4,
    num_hnfs=1, ddio_way_part=1, model_profile="abstract",
    hnf_inclusion="inclusive", indexing_policy="linear",
)
hierarchy = system.cache_hierarchy
hierarchy.incorporate_cache(DirectedBoard(system))
hierarchy.ruby_system.randomization = False
metadata = hierarchy.get_configuration()
assert metadata["coherence_protocol"] == "CHI"
assert metadata["hnf_inclusion"] == "inclusive"
assert metadata["ddio_way_part"] == 1
assert metadata["nic_read_no_allocate"] is False
assert len(hierarchy.core_clusters) == 2
assert len(hierarchy.hnfs) == len(hierarchy.dma_requestors) == 1
assert len(hierarchy.misc_nodes) == 1
for cluster in hierarchy.core_clusters:
    assert list(cluster.l1d.downstream_destinations) == [cluster.l2]
    assert list(cluster.l2.downstream_destinations) == list(hierarchy.hnfs)
hnf = hierarchy.hnfs[0]
assert bool(hnf.is_HN) and not bool(hnf.nic_read_no_allocate)
assert bool(hnf.alloc_on_readunique)
assert bool(hnf.dealloc_backinv_unique) and bool(hnf.dealloc_backinv_shared)
assert not bool(hnf.enable_DMT) and not bool(hnf.enable_DCT)
assert int(hnf.cache.ddio_way_part) == 1
assert list(hierarchy.dma_requestors[0].downstream_destinations) == [hnf]

root = Root(full_system=False, system=system)
m5.ticks.setGlobalFrequency("1ps")
m5.instantiate()
exit_event = m5.simulate(args.abs_max_tick)
print("Exiting @ tick", m5.curTick(), "because", exit_event.getCause())
if exit_event.getCause() != "MESI DDIO directed scenario completed":
    raise RuntimeError(f"Unexpected simulation exit: {exit_event.getCause()}")
# CHI counts tag presence at HNF acceptance, not MESI eviction outcomes.
# The inclusive HNF retains the dirty-owner line; the partial RX is a hit.
partial = args.scenario == "partial_dirty_owner"
rdma = args.scenario == "pvrdma_provenance"
tx = int(partial or rdma)
expected = {
    "rxPayloadRequests": 1,
    "rxPayloadHits": int(partial),
    "rxPayloadMisses": int(not partial),
    "txPayloadRequests": tx,
    "txPayloadHits": tx,
    "txPayloadMisses": 0,
    "nicDmaWriteRequests": 1,
    "nicDmaReadRequests": tx,
    "rdmaRxPayloadRequests": int(rdma),
    "rdmaTxPayloadRequests": int(rdma),
    "rdmaRxPayloadCpuUniqueLines": int(rdma),
}
for name, value in expected.items():
    stat = hnf.cache.resolveStat(name)
    stat.prepare()
    assert stat.value == value, f"{name}: observed={stat.value}, expected={value}"
    print(f"CHI_HNF_STAT {name}={stat.value}")
print(f"CACHELENS_CHI_DMA_OK scenario={args.scenario}")
