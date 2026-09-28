# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""Asset-free CHI data-value checks, not guest boot or DVM/ISA validation.

Two requestor adapters drive real private caches using the existing directed
CPU/DMA tester. Empty decoder lists deliberately issue no instructions/DVM.
No external TLM library or guest resources. Explicit stash tests use CHI-TLM.
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
        return [self.system.tester.dma_port, self.system.tester.dma_race_port]

    def get_ddio_dma_ports(self):
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
    "eligible_untagged", "noneligible_dma",
    "cold_full_write", "partial_dirty_owner",
    "partial_ss_sharers", "hit_update",
    "nic_cpu_nic", "clean_private_victim", "hnf_evict_shared",
    "tx_miss_no_allocate", "disabled_ddio",
    "zero_mask", "sparse_mask",  # Unsupported by RubySequencer: must fail.
))
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
ddio = args.ddio_way_part if args.ddio_way_part is not None else 1

system = System(
    clk_domain=SrcClockDomain(clock="1GHz", voltage_domain=VoltageDomain()),
    mem_ranges=[AddrRange("64MiB")], mem_mode="timing", cache_line_size=64,
)
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.tester = MESIDDIODirectedTester(
    scenario=args.scenario, block_size=64,
)
system.cache_hierarchy = CacheLensCHIHierarchy(
    l1i_size="256B", l1i_assoc=2, l1d_size="256B", l1d_assoc=2,
    l2_size="256B", l2_assoc=2, hnf_size="512B", hnf_assoc=4,
    num_hnfs=1, ddio_way_part=ddio,
    model_profile="abstract",
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
assert len(hierarchy.hnfs) == 1
assert len(hierarchy.dma_requestors) == 2
assert bool(hierarchy.dma_requestors[0].ddio_eligible)
assert not bool(hierarchy.dma_requestors[1].ddio_eligible)
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

if args.scenario in ("noneligible_dma", "tx_miss_no_allocate"):
    # Distinguish generic behavior from DDIO even when the legacy
    # knob is set: the seed write bypasses; only a generic read may allocate.
    hnf.alloc_on_writeback = False
    hnf.nic_read_no_allocate = True

root = Root(full_system=False, system=system)
m5.ticks.setGlobalFrequency("1ps")
m5.instantiate()
exit_event = m5.simulate(args.abs_max_tick)
print("Exiting @ tick", m5.curTick(), "because", exit_event.getCause())
if exit_event.getCause() != "MESI DDIO directed scenario completed":
    raise RuntimeError(f"Unexpected simulation exit: {exit_event.getCause()}")
# Aggregate eligible-DMA accounting only. No source/provenance counters exist.
for name in ("writeStashRequests", "writeStashAccepted", "writeStashDeclined",
             "writeStashAllocations"):
    stat = hnf.cache.resolveStat(name)
    stat.prepare()
    assert stat.value == 0, (name, stat.value)
fills = hnf.cache.resolveStat("ddioWayFill")
accesses = hnf.cache.resolveStat("ddioWayAccess")
fills.prepare()
accesses.prepare()
assert len(fills.value) == int(hnf.cache.assoc)
if ddio > 0:
    assert all(v == 0 for v in fills.value[ddio:]), fills.value
if args.scenario == "eligible_untagged":
    assert sum(fills.value) == 2, fills.value
    assert sum(accesses.value) == 5, accesses.value
elif args.scenario == "noneligible_dma":
    assert sum(fills.value) == 0, fills.value
elif args.scenario == "cold_full_write":
    assert sum(fills.value) == int(ddio > 0), fills.value
elif args.scenario == "hit_update":
    assert sum(fills.value) == int(ddio > 0), fills.value
    assert sum(accesses.value) == 2 * int(ddio > 0), accesses.value
# dma-llc-tag-v1: HNF tag presence, NOT data service or directory presence.
# Noninclusive cold CPU reads use DMT and can leave directory-only entries.
retained = int(ddio > 0)
lookup_expectations = {
    "eligible_untagged": (4, 3, 1, 2),
    "noneligible_dma": (1, 0, 0, 0),  # generic read filled before eligible probe
    "cold_full_write": (0, 0, 0, 1),
    "hit_update": (retained, 1 - retained, retained, 2 - retained),
    "tx_miss_no_allocate": (0, 2, 0, 0),
    # The generic seed write allocates. Only mostly-exclusive removes that
    # tag on the CPU's unique grant; directory-only RU is a write miss.
    "partial_dirty_owner": (
        retained, 1 - retained, int(not mostly_exclusive), int(mostly_exclusive)
    ),
    # A second CPU sharer restores/retains an HNF tag in all three policies.
    "partial_ss_sharers": (retained, 1 - retained, 1, 0),
    "nic_cpu_nic": (1, 0, int(not mostly_exclusive), 1 + int(mostly_exclusive)),
    "clean_private_victim": (2, 0, int(not mostly_exclusive), int(mostly_exclusive)),
    "disabled_ddio": (0, 3, int(not mostly_exclusive), 1 + int(mostly_exclusive)),
    "hnf_evict_shared": (0, 1, 0, 4),
}
if args.scenario in lookup_expectations:
    rh, rm, wh, wm = lookup_expectations[args.scenario]
    expected_lookups = (rh + rm, rh, rm, wh + wm, wh, wm)
else:
    expected_lookups = None
observed_lookups = []
for direction in ("Read", "Write"):
    values = []
    for outcome in ("Requests", "Hits", "Misses"):
        name = f"dma{direction}{outcome}"
        stat = hnf.cache.resolveStat(name)
        stat.prepare()
        values.append(stat.value)
        print(f"CHI_DMA_LOOKUP {name}={stat.value}")
    assert values[0] == values[1] + values[2], values
    observed_lookups.extend(values)
if expected_lookups is not None:
    assert tuple(observed_lookups) == expected_lookups, (
        observed_lookups, expected_lookups
    )

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
print(f"CACHELENS_CHI_DMA_OK scenario={args.scenario}")
