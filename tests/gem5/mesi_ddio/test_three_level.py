# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""Resource-free tests of the actual CacheLens native three-level hierarchy."""

import re

from testlib import *
from testlib import test_util


_CONFIG = joinpath(
    config.base_dir,
    "tests",
    "gem5",
    "mesi_ddio",
    "configs",
    "mesi_three_level_directed.py",
)
_RUBY = "system.cache_hierarchy.ruby_system."

# Exact LLC counters from test_full_line.py: the shared native L3 uses the
# same modified MESI_Two_Level L2 state machine. CPU data checks remain in
# MESIDDIODirectedTester and traverse both private levels, not legacy Ruby.
_EXPECTED = {
    "cold_full_write": {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 1,
        "txPayloadRequests": 0,
        "txPayloadHits": 0,
        "txPayloadMisses": 0,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayFill::total": 1,
        "ddioWayAccess::nic_tx_payload_way0": 0,
        "ddioWayAccess::total": 0,
        "wayDeallocations::way0": 1,
        "wayDeallocations::total": 1,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 1,
        "ddioOwnershipAcks": 1,
    },
    "partial_dirty_owner": {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 0,
        "txPayloadRequests": 1,
        "txPayloadHits": 1,
        "txPayloadMisses": 0,
        "ddioWayFill::cpu_other_way0": 1,
        "ddioWayFill::total": 1,
        "ddioWayAccess::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::total": 2,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    "partial_ss_sharers": {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 0,
        "txPayloadRequests": 1,
        "txPayloadHits": 1,
        "txPayloadMisses": 0,
        "ddioWayFill::cpu_other_way0": 1,
        "ddioWayFill::total": 1,
        "ddioWayAccess::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::cpu_other_way0": 3,
        "ddioWayAccess::total": 5,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    "tx_miss_no_allocate": {
        "txPayloadRequests": 2,
        "txPayloadHits": 0,
        "txPayloadMisses": 2,
        "ddioWayFill::total": 0,
        "ddioWayAccess::total": 0,
        "dmaRoutingProxyRequests": 2,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    "clean_subset_mt": {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 1,
        "txPayloadRequests": 2,
        "txPayloadHits": 1,
        "txPayloadMisses": 1,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayFill::cpu_other_way0": 1,
        "ddioWayFill::total": 2,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::total": 1,
        "wayDeallocations::way0": 1,
        "wayDeallocations::total": 1,
        "dmaRoutingProxyRequests": 1,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 1,
        "ddioOwnershipRequests": 1,
        "ddioOwnershipAcks": 1,
    },
    "subset_full_victim": {
        "rxPayloadRequests": 2,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 2,
        "ddioWayFill::nic_rx_payload_way0": 2,
        "wayDeallocations::way0": 1,
        "txPayloadRequests": 1,
        "txPayloadHits": 1,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "dmaRoutingProxyRequests": 0,
        "ddioReplacementStalls": 1,
        "ddioOwnershipRequests": 2,
        "ddioOwnershipAcks": 2,
    },
    "sparse_mask": {
        "rxPayloadRequests": 3,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 2,
        "txPayloadRequests": 3,
        "txPayloadHits": 1,
        "txPayloadMisses": 2,
        "ddioWayFill::nic_rx_payload_way0": 2,
        "ddioWayFill::total": 2,
        "ddioWayAccess::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::total": 2,
        "wayDeallocations::way0": 1,
        "wayDeallocations::total": 1,
        "dmaRoutingProxyRequests": 2,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 1,
        "ddioOwnershipRequests": 2,
        "ddioOwnershipAcks": 2,
    },
    "zero_mask": {
        "rxPayloadRequests": 0,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 0,
        "txPayloadRequests": 1,
        "txPayloadHits": 0,
        "txPayloadMisses": 1,
        "ddioWayFill::total": 0,
        "ddioWayAccess::total": 0,
        "dmaRoutingProxyRequests": 1,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    "pvrdma_provenance": {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 1,
        "rdmaRxPayloadRequests": 1,
        "rdmaRxPayloadHits": 0,
        "rdmaRxPayloadMisses": 1,
        "rdmaRxPayloadUniqueLines": 1,
        "txPayloadRequests": 1,
        "txPayloadHits": 1,
        "txPayloadMisses": 0,
        "rdmaTxPayloadRequests": 1,
        "rdmaTxPayloadHits": 1,
        "rdmaTxPayloadMisses": 0,
        "rdmaTxPayloadUniqueLines": 1,
        "rdmaRxPayloadCpuAccessWays::way0": 1,
        "rdmaRxPayloadCpuFillWays::way0": 0,
        "rdmaRxPayloadCpuUniqueLines": 1,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::cpu_other_way0": 1,
    },
}

_EXPECTED["eligible_untagged"] = {
    "rxPayloadRequests": 0, "txPayloadRequests": 3,
    "txPayloadHits": 2, "txPayloadMisses": 1,
    "ddioWayFill::cpu_other_way0": 2, "ddioWayFill::total": 2,
    "ddioOwnershipRequests": 2, "ddioOwnershipAcks": 2,
    "dmaRoutingProxyRequests": 3,
}
_EXPECTED["tagged_noneligible"] = {
    "rxPayloadRequests": 0, "txPayloadRequests": 1,
    "txPayloadHits": 0, "txPayloadMisses": 1,
    "ddioWayFill::total": 0, "ddioOwnershipRequests": 0,
    "dmaRoutingProxyRequests": 1,
}

# Per-bank expectations from test_dma_routing.py (not summed across banks).
_EXPECTED["routing_full_line"] = {
    "dmaRoutingProxyRequests": 0,
    "dmaRoutingTransientRecycles": 0,
    "ddioReplacementStalls": 0,
    "ddioOwnershipRequests": 1,
    "ddioOwnershipAcks": 1,
    "rxPayloadRequests": 1,
    "rxPayloadHits": 0,
    "rxPayloadMisses": 1,
    "txPayloadRequests": 1,
    "txPayloadHits": 1,
    "txPayloadMisses": 0,
    "ddioWayFill::nic_rx_payload_way0": 1,
    "ddioWayAccess::nic_tx_payload_way0": 1,
}


class ThreeLevelStatsVerifier(verifier.Verifier):
    def __init__(self, scenario, banks):
        super().__init__()
        self.scenario = scenario
        self.banks = banks

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        stats = {}
        with open(joinpath(tempdir, constants.gem5_simulation_stats)) as stream:
            for line in stream:
                fields = line.split()
                if len(fields) >= 2:
                    try:
                        stats[fields[0]] = float(fields[1])
                    except ValueError:
                        pass
        expected = {
            f"{_RUBY}l3_controllers{bank if self.banks > 1 else ''}.L2cache.{suffix}": value
            for bank in range(self.banks)
            for suffix, value in _EXPECTED[self.scenario].items()
        }
        if self.scenario == "routing_full_line":
            for event in ("ReadRequest", "Data", "WriteRequest", "Ack"):
                expected[f"{_RUBY}DMA_Controller.{event}::total"] = self.banks
        for name, value in expected.items():
            if stats.get(name) != value:
                test_util.fail(
                    f"{self.scenario}: {name}: expected {value}, "
                    f"observed {stats.get(name, 'missing')}"
                )


for scenario in _EXPECTED:
    banks = 2 if scenario == "routing_full_line" else 1
    gem5_verify_config(
        name=f"cachelens-mesi-three-level-{scenario.replace('_', '-')}",
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(
                re.compile(
                    rf".*MESI DDIO directed scenario '{scenario}' passed"
                )
            ),
            ThreeLevelStatsVerifier(scenario, banks),
        ),
        config=_CONFIG,
        config_args=(
            f"--scenario={scenario}",
            f"--num-hnfs={banks}",
            "--abs-max-tick=10000000000",
        ),
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
        protocol="MESI_Three_Level",
    )
