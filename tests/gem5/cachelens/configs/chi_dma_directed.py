# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""Asset-free CHI data-value checks, not guest boot or DVM/ISA validation.

Two requestor adapters drive real private caches using the existing directed
CPU/DMA tester. Empty decoder lists deliberately issue no instructions/DVM.
No external TLM library or guest resources. Stash decline is test-only.
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
    "stash_partial_cold", "partial_ss_sharers", "hit_update",
    "stash_unclassified", "stash_replacement", "stash_clean_owner",
    "nic_cpu_nic", "clean_private_victim", "hnf_evict_shared",
    "zero_mask", "sparse_mask",  # Unsupported by RubySequencer: must fail.
))
parser.add_argument("--cache-stashing", choices=("off", "hnf"), default=None)
parser.add_argument("--decline-stash", action="store_true")
parser.add_argument("--ddio-way-part", type=int, default=None)
parser.add_argument(
    "--hnf-inclusion",
    choices=("inclusive", "noninclusive", "mostly-exclusive"),
    default="inclusive",
)
parser.add_argument("--read-not-shared-dirty", action="store_true")
parser.add_argument("--dealloc-on-unique", action="store_true")
parser.add_argument("--abs-max-tick", type=int, default=10000000000)
args = parser.parse_args()
stash_test = args.cache_stashing is not None
if args.decline_stash and args.cache_stashing != "hnf":
    parser.error("--decline-stash requires --cache-stashing hnf")
if args.scenario.startswith("stash_") and not stash_test:
    parser.error("stash scenarios require --cache-stashing")
ddio = args.ddio_way_part
if ddio is None:
    ddio = -1 if stash_test else 1

system = System(
    clk_domain=SrcClockDomain(clock="1GHz", voltage_domain=VoltageDomain()),
    mem_ranges=[AddrRange("64MiB")], mem_mode="timing", cache_line_size=64,
)
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.tester = MESIDDIODirectedTester(
    scenario=args.scenario, block_size=64, cold_read_is_tx=stash_test,
)
system.cache_hierarchy = CacheLensCHIHierarchy(
    l1i_size="256B", l1i_assoc=2, l1d_size="256B", l1d_assoc=2,
    l2_size="256B", l2_assoc=2, hnf_size="512B", hnf_assoc=4,
    num_hnfs=1, ddio_way_part=ddio,
    model_profile="arm-generic" if stash_test else "abstract",
    cache_stashing=args.cache_stashing or "off",
    hnf_inclusion=args.hnf_inclusion, indexing_policy="linear",
    dealloc_on_unique=args.dealloc_on_unique,
)
hierarchy = system.cache_hierarchy
hierarchy.incorporate_cache(DirectedBoard(system))
hierarchy.ruby_system.randomization = False
metadata = hierarchy.get_configuration()
assert metadata["coherence_protocol"] == "CHI"
assert metadata["hnf_inclusion"] == args.hnf_inclusion
assert metadata["ddio_way_part"] == ddio
assert metadata["nic_read_no_allocate"] is False
assert len(hierarchy.core_clusters) == 2
assert len(hierarchy.hnfs) == len(hierarchy.dma_requestors) == 1
assert len(hierarchy.misc_nodes) == 1
for cluster in hierarchy.core_clusters:
    assert list(cluster.l1d.downstream_destinations) == [cluster.l2]
    assert list(cluster.l2.downstream_destinations) == list(hierarchy.hnfs)
    # Test the alternate read opcode emitted when shared dirty is disallowed.
    cluster.l2.allow_SD = not args.read_not_shared_dirty
hnf = hierarchy.hnfs[0]
assert bool(hnf.is_HN) and not bool(hnf.nic_read_no_allocate)
inclusive = args.hnf_inclusion == "inclusive"
mostly_exclusive = args.hnf_inclusion == "mostly-exclusive"
dealloc_unique = mostly_exclusive or (not inclusive and args.dealloc_on_unique)
assert bool(hnf.dealloc_on_unique) == dealloc_unique
assert bool(hnf.fwd_unique_on_readshared) == mostly_exclusive
assert metadata["hnf_dealloc_on_unique"] == dealloc_unique
assert metadata["hnf_fwd_unique_on_readshared"] == mostly_exclusive
assert not bool(hnf.dealloc_on_shared)
assert all(bool(getattr(hnf, flag)) for flag in (
    "alloc_on_readshared", "alloc_on_readonce", "alloc_on_writeback",
))
assert bool(hnf.alloc_on_readunique) == inclusive
assert bool(hnf.dealloc_backinv_unique) == inclusive
assert bool(hnf.dealloc_backinv_shared) == inclusive
assert bool(hnf.enable_DMT) == (not inclusive)
assert bool(hnf.enable_DCT) == (not inclusive)
assert int(hnf.cache.ddio_way_part) == ddio
assert list(hierarchy.dma_requestors[0].downstream_destinations) == [hnf]

if args.scenario == "stash_clean_owner":
    # Exercise UC_RU: retain the clean HNF copy while granting its private
    # L2 owner uniqueness. A normal ReadShared otherwise grants only SC.
    hnf.fwd_unique_on_readshared = True

if stash_test:
    assert metadata["model_profile"] == "arm-generic"
    assert bool(hierarchy.dma_requestors[0].emit_rx_write_stash) == (
        args.cache_stashing == "hnf"
    )
    if args.decline_stash:
        for node in hierarchy.hnfs:
            node.accept_write_stash = False
    assert bool(hnf.accept_write_stash) == (
        args.cache_stashing == "hnf" and not args.decline_stash
    )

root = Root(full_system=False, system=system)
m5.ticks.setGlobalFrequency("1ps")
m5.instantiate()
exit_event = m5.simulate(args.abs_max_tick)
print("Exiting @ tick", m5.curTick(), "because", exit_event.getCause())
if exit_event.getCause() != "MESI DDIO directed scenario completed":
    raise RuntimeError(f"Unexpected simulation exit: {exit_event.getCause()}")
# Acceptance-time TX hits below occur before CPU demand reads: unlike a
# final tag lookup, these cannot mistake a later CPU fill for a stash fill.
partial = args.scenario in (
    "partial_dirty_owner", "partial_ss_sharers", "stash_clean_owner",
)
if args.scenario == "stash_clean_owner" and not inclusive:
    # DMT fills the clean private owner directly, bypassing HNF allocation.
    partial = False
if mostly_exclusive and args.scenario == "partial_dirty_owner":
    partial = False
rdma = args.scenario == "pvrdma_provenance"
retained = ddio >= 0 or (
    args.cache_stashing == "hnf" and not args.decline_stash
)
rx, rx_hits, tx, tx_hits = 1, int(partial), 1, int(retained)
allocations = int(retained and not partial)
if args.scenario == "cold_full_write" and not stash_test:
    tx = tx_hits = 0
elif args.scenario == "nic_cpu_nic":
    rx, rx_hits = 2, int(retained and not mostly_exclusive)
elif args.scenario == "clean_private_victim":
    rx, rx_hits = 1, int(not mostly_exclusive)
    tx = tx_hits = 2
elif args.scenario == "hnf_evict_shared":
    rx, rx_hits, tx, tx_hits = 4, 0, 1, 0
elif args.scenario == "hit_update":
    rx, rx_hits = 2, int(retained)
elif args.scenario == "stash_unclassified":
    rx = rx_hits = allocations = 0
    # Generic (unclassified) writes retain normally, without a stash hint.
    tx, tx_hits = 3, 1
elif args.scenario == "stash_replacement":
    rx, rx_hits, tx, tx_hits = 10, 0, 5, 4 if retained else 0
    allocations = 10 if retained else 0
expected = {
    "rxPayloadRequests": rx,
    "rxPayloadHits": rx_hits,
    "rxPayloadMisses": rx - rx_hits,
    "txPayloadRequests": tx,
    "txPayloadHits": tx_hits,
    "txPayloadMisses": tx - tx_hits,
    "nicDmaWriteRequests": rx,
    "nicDmaReadRequests": tx,
    "rdmaRxPayloadRequests": int(rdma),
    "rdmaTxPayloadRequests": int(rdma),
    "rdmaRxPayloadCpuUniqueLines": int(rdma),
}
if args.scenario in ("stash_partial_cold", "stash_clean_owner"):
    expected.update(nicDmaWriteRequests=2, nicDescWriteRequests=1)
if args.scenario == "stash_unclassified":
    expected.update(nicDmaWriteRequests=2, nicDmaReadRequests=4,
                    nicDescWriteRequests=1, nicDescReadRequests=1,
                    rxHeaderRequests=1, rxHeaderMisses=1)
if stash_test:
    hints = rx if args.cache_stashing == "hnf" else 0
    expected.update(
        writeStashRequests=hints,
        writeStashAccepted=0 if args.decline_stash else hints,
        writeStashDeclined=hints if args.decline_stash else 0,
        # Only new physical stash fills; hits and CPU/TX fills do not count.
        writeStashAllocations=allocations if hints else 0,
    )
for name, value in expected.items():
    stat = hnf.cache.resolveStat(name)
    stat.prepare()
    assert stat.value == value, f"{name}: observed={stat.value}, expected={value}"
    print(f"CHI_HNF_STAT {name}={stat.value}")
if args.scenario in ("clean_private_victim", "hnf_evict_shared"):
    # Demand-hit counts distinguish surviving private copies from refills;
    # full-line read checks in the tester alone cannot prove no backinv.
    clusters = (hierarchy.core_clusters if args.scenario == "hnf_evict_shared"
                else hierarchy.core_clusters[:1])
    for index, cluster in enumerate(clusters):
        for level in ("l1d", "l2"):
            if args.scenario == "hnf_evict_shared":
                misses = 2 if inclusive else 1
                hits = int(not inclusive and level == "l1d")
            else:
                misses, hits = 4, 0
            for name, value in (
                ("m_demand_hits", hits), ("m_demand_misses", misses),
            ):
                stat = getattr(cluster, level).cache.resolveStat(name)
                stat.prepare()
                assert stat.value == value, (index, level, name, stat.value, value)
                print(f"CHI_PRIVATE_STAT core={index} level={level} "
                      f"{name}={stat.value}")
if stash_test and args.scenario in (
    "stash_partial_cold", "stash_clean_owner", "partial_dirty_owner",
):
    stat = hnf.cache.resolveStat("nicDmaMergeReads")
    stat.prepare()
    merges = sum(stat.value)
    expected_merges = int(args.scenario != "partial_dirty_owner")
    assert merges == expected_merges, (merges, expected_merges)
    print(f"CHI_HNF_STAT nicDmaMergeReads.total={merges}")
if stash_test:
    print(f"CACHELENS_CHI_STASH_OK scenario={args.scenario} "
          f"mode={args.cache_stashing} decline={int(args.decline_stash)}")
print(f"CACHELENS_CHI_DMA_OK scenario={args.scenario}")
