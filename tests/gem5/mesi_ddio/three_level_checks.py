# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""Shared native three-level smoke expectations; no gem5/testlib imports."""

from pathlib import Path

_RUBY = "system.cache_hierarchy.ruby_system."

# Native shared-L3 counters. CPU data checks remain in the C++ tester and
# traverse physical private L1 and L2, not the legacy two-level hierarchy.
_EXPECTED = {
    'cold_full_write': {
        'ddioWayFill::way0': 1,
        'ddioWayAccess::way0': 0,
        'wayDeallocations::way0': 1,
        'wayDeallocations::total': 1,
        'dmaRoutingProxyRequests': 0,
        'dmaRoutingTransientRecycles': 0,
        'ddioReplacementStalls': 0,
        'ddioOwnershipRequests': 1,
        'ddioOwnershipAcks': 1,
    },
    'partial_dirty_owner': {
        'ddioWayAccess::way0': 2,
        'dmaRoutingProxyRequests': 0,
        'dmaRoutingTransientRecycles': 0,
        'ddioReplacementStalls': 0,
        'ddioOwnershipRequests': 0,
        'ddioOwnershipAcks': 0,
    },
    'partial_ss_sharers': {
        'ddioWayAccess::way0': 2,
        'dmaRoutingProxyRequests': 0,
        'dmaRoutingTransientRecycles': 0,
        'ddioReplacementStalls': 0,
        'ddioOwnershipRequests': 0,
        'ddioOwnershipAcks': 0,
    },
    'tx_miss_no_allocate': {
        'dmaRoutingProxyRequests': 2,
        'ddioOwnershipRequests': 0,
        'ddioOwnershipAcks': 0,
    },
    'clean_subset_mt': {
        'ddioWayFill::way0': 1,
        'ddioWayAccess::way0': 1,
        'wayDeallocations::way0': 1,
        'wayDeallocations::total': 1,
        'dmaRoutingProxyRequests': 1,
        'dmaRoutingTransientRecycles': 0,
        'ddioReplacementStalls': 1,
        'ddioOwnershipRequests': 1,
        'ddioOwnershipAcks': 1,
    },
    'subset_full_victim': {
        'ddioWayFill::way0': 2,
        'wayDeallocations::way0': 1,
        'ddioWayAccess::way0': 1,
        'dmaRoutingProxyRequests': 0,
        'ddioReplacementStalls': 1,
        'ddioOwnershipRequests': 2,
        'ddioOwnershipAcks': 2,
    },
    'sparse_mask': {
        'ddioWayFill::way0': 2,
        'ddioWayAccess::way0': 2,
        'wayDeallocations::way0': 1,
        'wayDeallocations::total': 1,
        'dmaRoutingProxyRequests': 2,
        'dmaRoutingTransientRecycles': 0,
        'ddioReplacementStalls': 1,
        'ddioOwnershipRequests': 2,
        'ddioOwnershipAcks': 2,
    },
    'zero_mask': {
        'dmaRoutingProxyRequests': 1,
        'dmaRoutingTransientRecycles': 0,
        'ddioReplacementStalls': 0,
        'ddioOwnershipRequests': 0,
        'ddioOwnershipAcks': 0,
    },
}

_EXPECTED["eligible_untagged"] = {
    'ddioWayFill::way0': 2,
    'ddioWayAccess::way0': 5,
    'ddioOwnershipRequests': 2,
    'ddioOwnershipAcks': 2,
    'dmaRoutingProxyRequests': 3,
}
_EXPECTED["noneligible_dma"] = {
    'ddioOwnershipRequests': 0,
    'dmaRoutingProxyRequests': 1,
}

# Exact per-bank expectations (not summed across banks).
_EXPECTED["routing_full_line"] = {
    'dmaRoutingProxyRequests': 0,
    'dmaRoutingTransientRecycles': 0,
    'ddioReplacementStalls': 0,
    'ddioOwnershipRequests': 1,
    'ddioOwnershipAcks': 1,
    'ddioWayFill::way0': 1,
    'ddioWayAccess::way0': 1,
}


_EXPECTED["disabled_ddio"] = {
    "ddioWayAccess::way0": 1,
    "wayDeallocations::way0": 1,
    "wayDeallocations::total": 1,
    "dmaRoutingProxyRequests": 5,
    "dmaRoutingTransientRecycles": 0,
    "ddioReplacementStalls": 0,
    "ddioOwnershipRequests": 0,
    "ddioOwnershipAcks": 0,
}

# Accepted line lookups: (read hits, read misses, write hits, write misses).
# noneligible_dma ends with ONE eligible probe; generic_partial is entirely
# noneligible. Internal merge reads and the empty sparse fragment are excluded.
_DMA_LOOKUPS = {
    "cold_full_write": (0, 0, 0, 1),
    "partial_dirty_owner": (1, 0, 1, 0),
    "partial_ss_sharers": (1, 0, 1, 0),
    "tx_miss_no_allocate": (0, 2, 0, 0),
    "clean_subset_mt": (1, 1, 0, 1),
    "subset_full_victim": (1, 0, 0, 2),
    "sparse_mask": (1, 2, 1, 2),
    "zero_mask": (0, 1, 0, 0),
    "eligible_untagged": (4, 3, 1, 2),
    "noneligible_dma": (0, 1, 0, 0),
    "routing_full_line": (1, 0, 0, 1),
    "disabled_ddio": (0, 3, 1, 1),
    "hit_update": (1, 0, 1, 1),
    "generic_partial": (0, 0, 0, 0),
    "overlap_dirty_write": (0, 0, 1, 1),
}
_EXPECTED["hit_update"] = {
    "ddioWayFill::way0": 1, "ddioWayAccess::way0": 2,
    "ddioOwnershipRequests": 1, "ddioOwnershipAcks": 1,
}
_EXPECTED["generic_partial"] = {"ddioWayAccess::way0": 0}
# Native three-level timing forces two transient recycles, but only two
# accepted writes (one miss, one owner-backed tag hit) enter the denominator.
_EXPECTED["overlap_dirty_write"] = {
    "ddioWayFill::way0": 1, "ddioWayAccess::way0": 1,
    "dmaRoutingTransientRecycles": 2,
    "ddioOwnershipRequests": 1, "ddioOwnershipAcks": 1,
}
for scenario, (rh, rm, wh, wm) in _DMA_LOOKUPS.items():
    for direction, hits, misses in (("Read", rh, rm), ("Write", wh, wm)):
        _EXPECTED[scenario].update({
            f"dma{direction}Requests": hits + misses,
            f"dma{direction}Hits": hits,
            f"dma{direction}Misses": misses,
        })

# Hits on CPU-filled lines, read misses, zero masks, generic DMA and DDIO-off
# must not allocate DDIO lines. Totals also forbid fills in another way.
for counters in _EXPECTED.values():
    fills = counters.setdefault("ddioWayFill::way0", 0)
    counters["ddioWayFill::total"] = fills
    counters["ddioAllocWays::total"] = fills
_EXPECTED["noneligible_dma"]["ddioOwnershipAcks"] = 0
for scenario in ("noneligible_dma", "tx_miss_no_allocate", "zero_mask"):
    _EXPECTED[scenario]["ddioWayAccess::way0"] = 0
for counters in _EXPECTED.values():
    counters["ddioWayAccess::total"] = counters["ddioWayAccess::way0"]


def scenario_args(scenario):
    return (
        f"--scenario={scenario}",
        f"--num-hnfs={2 if scenario == 'routing_full_line' else 1}",
        f"--ddio-way-part={-1 if scenario == 'disabled_ddio' else 1}",
        "--abs-max-tick=10000000000",
    )


def expected_stats(scenario):
    banks = 2 if scenario == "routing_full_line" else 1
    expected = {
        f"{_RUBY}l3_controllers{bank if banks > 1 else ''}.L2cache.{suffix}": value
        for bank in range(banks)
        for suffix, value in _EXPECTED[scenario].items()
    }
    if scenario == "partial_ss_sharers":
        # Both CPUs miss before and after DMA invalidation at BOTH private
        # levels. This is native three-level traffic, not just an object graph.
        for core in range(2):
            for level, cache in ((1, "Dcache"), (2, "cache")):
                expected[
                    f"{_RUBY}l{level}_controllers{core}.{cache}.m_demand_misses"
                ] = 2
    if scenario == "routing_full_line":
        for event in ("ReadRequest", "Data", "WriteRequest", "Ack"):
            expected[f"{_RUBY}DMA_Controller.{event}::total"] = banks
    return expected


def check_stats(path, scenario):
    stats = {}
    for line in Path(path).read_text().splitlines():
        fields = line.split()
        if len(fields) >= 2:
            try:
                stats[fields[0]] = float(fields[1])
            except ValueError:
                pass
    errors = [
        f"{name}: expected {value}, observed {stats.get(name, 'missing')}"
        for name, value in expected_stats(scenario).items()
        if stats.get(name) != value
    ]
    if errors:
        raise ValueError(f"{scenario}: " + "; ".join(errors))


def check_output(output, scenario):
    for marker in (
        "CacheLens native three-level hierarchy assertions passed",
        f"MESI DDIO directed scenario '{scenario}' passed",
        "because MESI DDIO directed scenario completed",
    ):
        if marker not in output:
            raise ValueError(f"{scenario}: missing completion marker: {marker}")
