# Copyright (c) 2026 minhuw
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import os
import re

from testlib import *
from testlib import test_util


_CONFIG = joinpath(
    config.base_dir,
    "tests",
    "gem5",
    "mesi_ddio",
    "configs",
    "mesi_ddio_directed.py",
)
_PREFIX = "system.ruby.l2_cntrl0.L2cache."


class DMASequencerCapabilityVerifier(verifier.Verifier):
    def __init__(self, expected_count=1):
        super().__init__()
        self._expected_count = expected_count

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        config_path = joinpath(tempdir, constants.gem5_simulation_config_ini)
        if not os.path.isfile(config_path):
            test_util.fail(f"Could not find gem5 config file: {config_path}")

        with open(config_path, encoding="utf-8") as config_file:
            config_text = config_file.read()
        sections = re.findall(
            r"(?ms)^\[([^]]+)\]\n(.*?)(?=^\[|\Z)", config_text
        )
        dma_sections = {
            name: body
            for name, body in sections
            if re.search(r"(?m)^type=DMASequencer$", body)
        }
        if len(dma_sections) != self._expected_count:
            test_util.fail(
                "MESI_Two_Level config has an unexpected number of "
                f"DMASequencers: observed={len(dma_sections)}, "
                f"expected={self._expected_count}"
            )
        for name, body in dma_sections.items():
            if not re.search(
                r"(?m)^supports_masked_writes=true$", body
            ):
                test_util.fail(
                    f"MESI_Two_Level DMASequencer {name} did not "
                    "explicitly enable masked writes"
                )


class MESIDDIOStatsVerifier(verifier.Verifier):
    def __init__(
        self,
        expected,
        minimum=None,
        runtime_expected=None,
        runtime_vector_expected=None,
        runtime_absent=None,
    ):
        super().__init__()
        self._expected = expected
        self._minimum = minimum or {}
        self._runtime_expected = runtime_expected or {}
        self._runtime_vector_expected = runtime_vector_expected or {}
        self._runtime_absent = runtime_absent or ()

    @staticmethod
    def _read_stats(path):
        stats = {}
        vector_stats = {}
        with open(path, encoding="utf-8") as stats_file:
            for line in stats_file:
                fields = line.split()
                if len(fields) < 2:
                    continue
                if fields[1] == "|":
                    values = re.findall(
                        r"\|\s+([-+]?(?:\d+(?:\.\d*)?|\.\d+)"
                        r"(?:[eE][-+]?\d+)?)",
                        line,
                    )
                    vector_stats[fields[0]] = tuple(
                        float(value) for value in values
                    )
                    continue
                try:
                    value = float(fields[1])
                except ValueError:
                    continue
                stats[fields[0]] = value
        return stats, vector_stats

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        stats_path = joinpath(tempdir, constants.gem5_simulation_stats)
        if not os.path.isfile(stats_path):
            test_util.fail(f"Could not find gem5 stats file: {stats_path}")

        stats, vector_stats = self._read_stats(stats_path)
        for suffix, expected in self._expected.items():
            name = _PREFIX + suffix
            if name not in stats:
                test_util.fail(
                    f"Missing required MESI DDIO statistic {name}"
                )
            observed = stats[name]
            if observed != expected:
                test_util.fail(
                    f"Unexpected MESI DDIO statistic {name}: "
                    f"observed={observed}, expected={expected}"
                )

        for suffix, minimum in self._minimum.items():
            name = _PREFIX + suffix
            if name not in stats:
                test_util.fail(
                    f"Missing required MESI DDIO statistic {name}"
                )
            observed = stats[name]
            if observed < minimum:
                test_util.fail(
                    f"MESI DDIO statistic {name} did not reach its minimum: "
                    f"observed={observed}, minimum={minimum}"
                )

        for name, expected in self._runtime_expected.items():
            if name not in stats:
                test_util.fail(f"Missing required runtime statistic {name}")
            observed = stats[name]
            if observed != expected:
                test_util.fail(
                    f"Unexpected runtime statistic {name}: "
                    f"observed={observed}, expected={expected}"
                )

        for name, expected in self._runtime_vector_expected.items():
            if name not in vector_stats:
                test_util.fail(
                    f"Missing required runtime vector statistic {name}"
                )
            observed = vector_stats[name]
            if observed != tuple(expected):
                test_util.fail(
                    f"Unexpected runtime vector statistic {name}: "
                    f"observed={observed}, expected={tuple(expected)}"
                )

        # gem5 omits protocol transition statistics with zero occurrences.
        # Absence is therefore an explicit expectation, not an implicit zero.
        for name in self._runtime_absent:
            if name in stats:
                test_util.fail(
                    f"Runtime statistic {name} was present with value "
                    f"{stats[name]}, expected it to be omitted"
                )


def _register(
    name,
    scenario,
    expected,
    ddio_way_part=1,
    minimum=None,
    runtime_expected=None,
    runtime_vector_expected=None,
    runtime_absent=None,
    dma_controllers=1,
):
    gem5_verify_config(
        name=name,
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(
                re.compile(
                    rf".*MESI DDIO directed scenario '{scenario}' passed"
                )
            ),
            DMASequencerCapabilityVerifier(dma_controllers),
            MESIDDIOStatsVerifier(
                expected,
                minimum,
                runtime_expected,
                runtime_vector_expected,
                runtime_absent,
            ),
        ),
        config=_CONFIG,
        config_args=(
            f"--scenario={scenario}",
            f"--ddio-way-part={ddio_way_part}",
            "--abs-max-tick=10000000",
        ),
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
        protocol="MESI_Two_Level",
    )


_register(
    "mesi-ddio-cold-full-line-write",
    "cold_full_write",
    {
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
)

_register(
    "mesi-ddio-full-line-hit-update",
    "hit_update",
    {
        "rxPayloadRequests": 2,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 1,
        "rxPayloadHitWays::0": 1,
        "txPayloadRequests": 1,
        "txPayloadHits": 1,
        "txPayloadMisses": 0,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::total": 2,
        "dmaRoutingProxyRequests": 0,
        "ddioOwnershipRequests": 1,
        "ddioOwnershipAcks": 1,
    },
)

_register(
    "mesi-ddio-hit-outside-way-subset",
    "outside_subset_hit",
    {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 0,
        "rxPayloadHitWays::1": 1,
        "ddioWayFill::nic_rx_payload_way0": 0,
        "ddioWayFill::nic_rx_payload_way1": 0,
        "ddioWayAccess::nic_rx_payload_way1": 1,
        "ddioWayAccess::nic_tx_payload_way1": 1,
        "dmaRoutingProxyRequests": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
)

_register(
    "mesi-ddio-subset-full-inclusive-victim",
    "subset_full_victim",
    {
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
)

_register(
    "mesi-ddio-two-way-subset-lru-victim",
    "multi_way_subset_lru",
    {
        "rxPayloadRequests": 3,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 3,
        "txPayloadRequests": 4,
        "txPayloadHits": 3,
        "txPayloadMisses": 1,
        "rxPayloadAllocWays::0": 1,
        "rxPayloadAllocWays::1": 2,
        "rxPayloadAllocWays::2": 0,
        "rxPayloadAllocWays::3": 0,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayFill::nic_rx_payload_way1": 2,
        "ddioWayFill::nic_rx_payload_way2": 0,
        "ddioWayFill::nic_rx_payload_way3": 0,
        "ddioWayFill::total": 3,
        "ddioWayAccess::nic_tx_payload_way0": 2,
        "ddioWayAccess::nic_tx_payload_way1": 1,
        "ddioWayAccess::nic_tx_payload_way2": 0,
        "ddioWayAccess::nic_tx_payload_way3": 0,
        "ddioWayAccess::total": 3,
        "wayDeallocations::way0": 0,
        "wayDeallocations::way1": 1,
        "wayDeallocations::way2": 0,
        "wayDeallocations::way3": 0,
        "wayDeallocations::total": 1,
        "dmaRoutingProxyRequests": 1,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 1,
        "ddioOwnershipRequests": 3,
        "ddioOwnershipAcks": 3,
    },
    ddio_way_part=2,
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 4,
        "system.ruby.DMA_Controller.Data": 4,
        "system.ruby.DMA_Controller.WriteRequest": 3,
        "system.ruby.DMA_Controller.Ack": 3,
        "system.ruby.L2Cache_Controller.DDIO_Replacement": 1,
        "system.ruby.L2Cache_Controller.M.DDIO_Replacement": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
    runtime_absent=(
        "system.ruby.L2Cache_Controller.DDIO_Replacement_clean",
        "system.ruby.L2Cache_Controller.M.DDIO_Replacement_clean",
    ),
)

_register(
    "mesi-ddio-clean-ss-subset-replacement",
    "clean_subset_ss",
    {
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
        "ddioWayAccess::cpu_other_way0": 1,
        "ddioWayAccess::total": 2,
        "wayDeallocations::way0": 1,
        "wayDeallocations::total": 1,
        "dmaRoutingProxyRequests": 1,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 1,
        "ddioOwnershipRequests": 1,
        "ddioOwnershipAcks": 1,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 2,
        "system.ruby.DMA_Controller.Data": 2,
        "system.ruby.DMA_Controller.WriteRequest": 2,
        "system.ruby.DMA_Controller.Ack": 2,
        "system.ruby.L2Cache_Controller.DDIO_Replacement_clean": 1,
        "system.ruby.L2Cache_Controller.SS.DDIO_Replacement_clean": 1,
        "system.ruby.L2Cache_Controller.I_I.Ack": 1,
        "system.ruby.L2Cache_Controller.I_I.Ack_all": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
    runtime_absent=(
        "system.ruby.L2Cache_Controller.DDIO_Replacement",
        "system.ruby.L2Cache_Controller.SS.DDIO_Replacement",
    ),
)

_register(
    "mesi-ddio-clean-m-subset-replacement",
    "clean_subset_m",
    {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 1,
        "txPayloadRequests": 2,
        "txPayloadHits": 1,
        "txPayloadMisses": 1,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayFill::cpu_other_way0": 1,
        "ddioWayFill::cpu_other_way1": 1,
        "ddioWayFill::cpu_other_way2": 1,
        "ddioWayFill::cpu_other_way3": 0,
        "ddioWayFill::total": 4,
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
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 2,
        "system.ruby.DMA_Controller.Data": 2,
        "system.ruby.DMA_Controller.WriteRequest": 4,
        "system.ruby.DMA_Controller.Ack": 4,
        "system.ruby.L2Cache_Controller.DDIO_Replacement_clean": 1,
        "system.ruby.L2Cache_Controller.M.DDIO_Replacement_clean": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
    runtime_absent=(
        "system.ruby.L2Cache_Controller.DDIO_Replacement",
        "system.ruby.L2Cache_Controller.M.DDIO_Replacement",
    ),
)

_register(
    "mesi-ddio-clean-mt-subset-replacement",
    "clean_subset_mt",
    {
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
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 2,
        "system.ruby.DMA_Controller.Data": 2,
        "system.ruby.DMA_Controller.WriteRequest": 2,
        "system.ruby.DMA_Controller.Ack": 2,
        "system.ruby.L2Cache_Controller.DDIO_Replacement_clean": 1,
        "system.ruby.L2Cache_Controller.MT.DDIO_Replacement_clean": 1,
        "system.ruby.L2Cache_Controller.MCT_I.Ack_all": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
    runtime_absent=(
        "system.ruby.L2Cache_Controller.DDIO_Replacement",
        "system.ruby.L2Cache_Controller.MT.DDIO_Replacement",
    ),
)

_register(
    "mesi-ddio-dirty-ss-subset-replacement",
    "dirty_subset_ss",
    {
        "rxPayloadRequests": 2,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 2,
        "txPayloadRequests": 2,
        "txPayloadHits": 1,
        "txPayloadMisses": 1,
        "ddioWayFill::nic_rx_payload_way0": 2,
        "ddioWayFill::total": 2,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::cpu_other_way0": 2,
        "ddioWayAccess::total": 3,
        "wayDeallocations::way0": 1,
        "wayDeallocations::total": 1,
        "dmaRoutingProxyRequests": 1,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 1,
        "ddioOwnershipRequests": 2,
        "ddioOwnershipAcks": 2,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 2,
        "system.ruby.DMA_Controller.Data": 2,
        "system.ruby.DMA_Controller.WriteRequest": 2,
        "system.ruby.DMA_Controller.Ack": 2,
        "system.ruby.L2Cache_Controller.DDIO_Replacement": 1,
        "system.ruby.L2Cache_Controller.SS.DDIO_Replacement": 1,
        "system.ruby.L2Cache_Controller.S_I.Ack": 1,
        "system.ruby.L2Cache_Controller.S_I.Ack_all": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
    runtime_absent=(
        "system.ruby.L2Cache_Controller.DDIO_Replacement_clean",
        "system.ruby.L2Cache_Controller.SS.DDIO_Replacement_clean",
    ),
)

_register(
    "mesi-ddio-invalidates-inclusive-l1-sharers",
    "l1_sharer_invalidation",
    {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 0,
        "ddioWayAccess::nic_rx_payload_way0": 1,
        "dmaRoutingProxyRequests": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
)

_register(
    "mesi-ddio-disabled-write-through-no-retention",
    "disabled_ddio",
    {
        "rxPayloadRequests": 2,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 1,
        "txPayloadRequests": 3,
        "txPayloadHits": 0,
        "txPayloadMisses": 3,
        "ddioWayFill::cpu_other_way0": 1,
        "ddioWayFill::total": 1,
        "ddioWayAccess::nic_rx_payload_way0": 1,
        "ddioWayAccess::total": 1,
        "wayDeallocations::way0": 1,
        "wayDeallocations::total": 1,
        "dmaRoutingProxyRequests": 5,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    ddio_way_part=-1,
)

_register(
    "mesi-ddio-tx-hit-retains-line",
    "tx_hit_retains",
    {
        "rxPayloadRequests": 1,
        "rxPayloadMisses": 1,
        "txPayloadRequests": 2,
        "txPayloadHits": 2,
        "txPayloadMisses": 0,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_tx_payload_way0": 2,
        "dmaRoutingProxyRequests": 0,
        "ddioOwnershipRequests": 1,
        "ddioOwnershipAcks": 1,
    },
)

_register(
    "mesi-ddio-tx-miss-does-not-allocate",
    "tx_miss_no_allocate",
    {
        "txPayloadRequests": 2,
        "txPayloadHits": 0,
        "txPayloadMisses": 2,
        "ddioWayFill::total": 0,
        "ddioWayAccess::total": 0,
        "dmaRoutingProxyRequests": 2,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
)

_register(
    "mesi-ddio-tx-read-retrieves-dirty-private-owner",
    "dirty_owner_tx",
    {
        "txPayloadRequests": 2,
        "txPayloadHits": 2,
        "txPayloadMisses": 0,
        "ddioWayAccess::nic_tx_payload_way0": 2,
        "ddioWayFill::cpu_other_way0": 1,
        "dmaRoutingProxyRequests": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
)

_register(
    "mesi-ddio-tx-read-clean-exclusive-owner",
    "clean_owner_tx",
    {
        "rxPayloadRequests": 0,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 0,
        "txPayloadRequests": 2,
        "txPayloadHits": 2,
        "txPayloadMisses": 0,
        "ddioWayFill::nic_tx_payload_way0": 0,
        "ddioWayFill::cpu_other_way0": 1,
        "ddioWayFill::total": 1,
        "ddioWayAccess::nic_tx_payload_way0": 2,
        "ddioWayAccess::total": 2,
        "wayDeallocations::total": 0,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 2,
        "system.ruby.DMA_Controller.Data": 2,
        "system.ruby.DMA_Controller.WriteRequest": 1,
        "system.ruby.DMA_Controller.Ack": 1,
        "system.ruby.L2Cache_Controller.MT.DMA_TX_READ": 1,
        "system.ruby.L2Cache_Controller.DM_RT.Ack_all": 1,
        "system.ruby.L2Cache_Controller.M.DMA_TX_READ": 1,
    },
    runtime_absent=(
        "system.ruby.L2Cache_Controller.DM_RT.WB_Data",
        "system.ruby.L2Cache_Controller.DM_RT.WB_Data_clean",
    ),
)

_register(
    "mesi-ddio-telemetry-counts-once",
    "telemetry_exactness",
    {
        "rxPayloadRequests": 2,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 1,
        "rxHeaderRequests": 1,
        "rxHeaderHits": 0,
        "rxHeaderMisses": 1,
        "txPayloadRequests": 2,
        "txPayloadHits": 1,
        "txPayloadMisses": 1,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayFill::nic_rx_header_way0": 1,
        "ddioWayFill::nic_desc_way0": 1,
        "ddioWayFill::total": 3,
        "ddioWayAccess::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::nic_desc_way0": 1,
        "ddioWayAccess::total": 3,
        "dmaRoutingProxyRequests": 2,
        "ddioOwnershipRequests": 3,
        "ddioOwnershipAcks": 3,
    },
)

_register(
    "mesi-ddio-generic-partial-dma-unchanged",
    "generic_partial",
    {
        "rxPayloadRequests": 0,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 0,
        "txPayloadRequests": 0,
        "txPayloadHits": 0,
        "txPayloadMisses": 0,
        "ddioWayFill::total": 0,
        "ddioWayAccess::total": 0,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
)

_register(
    "mesi-ddio-generic-partial-write-merges-dirty-owner",
    "generic_partial_dirty_owner",
    {
        "rxPayloadRequests": 0,
        "txPayloadRequests": 0,
        "ddioWayFill::cpu_other_way0": 1,
        "ddioWayFill::total": 1,
        "ddioWayAccess::total": 0,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 1,
        "system.ruby.DMA_Controller.Data": 1,
        "system.ruby.DMA_Controller.WriteRequest": 2,
        "system.ruby.DMA_Controller.Ack": 2,
        "system.ruby.Directory_Controller.M.DMA_WRITE_PARTIAL": 1,
        "system.ruby.Directory_Controller.M_DWR.Data": 1,
        "system.ruby.Directory_Controller.M_DWRI.Memory_Ack": 1,
    },
    runtime_absent=(
        "system.ruby.Directory_Controller.M_DWR.CleanReplacement",
    ),
)

_register(
    "mesi-ddio-overlapping-dirty-full-line-write",
    "overlap_dirty_write",
    {
        "rxPayloadRequests": 2,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 1,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_rx_payload_way0": 1,
        "wayDeallocations::way0": 1,
        "dmaRoutingProxyRequests": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 1,
        "ddioOwnershipAcks": 1,
    },
    minimum={"dmaRoutingTransientRecycles": 1},
)

_register(
    "mesi-ddio-concurrent-subset-victim-replacement",
    "concurrent_subset_victim",
    {
        "rxPayloadRequests": 3,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 3,
        "ddioWayFill::nic_rx_payload_way0": 3,
        "wayDeallocations::way0": 3,
        "dmaRoutingProxyRequests": 0,
        "ddioReplacementStalls": 2,
        "ddioOwnershipRequests": 3,
        "ddioOwnershipAcks": 3,
    },
)

_register(
    "mesi-ddio-generic-dma-read-races-clean-l2-replacement",
    "clean_replacement_dma_read",
    {
        "rxPayloadRequests": 0,
        "txPayloadRequests": 0,
        "ddioWayFill::cpu_other_way0": 2,
        "ddioWayFill::cpu_other_way1": 1,
        "ddioWayFill::cpu_other_way2": 1,
        "ddioWayFill::cpu_other_way3": 1,
        "ddioWayFill::total": 5,
        "ddioWayAccess::total": 0,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 1,
        "system.ruby.DMA_Controller.Data": 1,
        "system.ruby.DMA_Controller.WriteRequest": 5,
        "system.ruby.DMA_Controller.Ack": 5,
        "system.ruby.Directory_Controller.M.DMA_READ": 1,
        "system.ruby.Directory_Controller.M_DRD.CleanReplacement": 1,
        "system.ruby.Directory_Controller.ID.Memory_Data": 1,
        "system.ruby.L2Cache_Controller.M.L2_Replacement_clean": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
    runtime_absent=("system.ruby.Directory_Controller.M_DRD.Data",),
)

_register(
    "mesi-ddio-generic-dma-write-races-clean-l2-replacement",
    "clean_replacement_dma_write",
    {
        "rxPayloadRequests": 0,
        "txPayloadRequests": 0,
        "ddioWayFill::cpu_other_way0": 2,
        "ddioWayFill::cpu_other_way1": 1,
        "ddioWayFill::cpu_other_way2": 1,
        "ddioWayFill::cpu_other_way3": 1,
        "ddioWayFill::total": 5,
        "ddioWayAccess::total": 0,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 1,
        "system.ruby.DMA_Controller.Data": 1,
        "system.ruby.DMA_Controller.WriteRequest": 6,
        "system.ruby.DMA_Controller.Ack": 6,
        "system.ruby.Directory_Controller.M.DMA_WRITE_FULL": 1,
        "system.ruby.Directory_Controller.M_DWR.CleanReplacement": 1,
        "system.ruby.Directory_Controller.ID_W.Memory_Ack": 6,
        "system.ruby.L2Cache_Controller.M.L2_Replacement_clean": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
    runtime_absent=("system.ruby.Directory_Controller.M_DWR.Data",),
)

_register(
    "mesi-ddio-partial-claim-races-generic-dma-read",
    "partial_claim_dma_read_race",
    {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 1,
        "txPayloadRequests": 0,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayFill::total": 1,
        "ddioWayAccess::total": 0,
        "wayDeallocations::way0": 1,
        "wayDeallocations::total": 1,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 1,
        "ddioOwnershipAcks": 1,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest::total": 2,
        "system.ruby.DMA_Controller.Data::total": 2,
        "system.ruby.DMA_Controller.WriteRequest::total": 2,
        "system.ruby.DMA_Controller.Ack::total": 2,
        "system.ruby.Directory_Controller.M.DMA_READ": 1,
        "system.ruby.Directory_Controller.M_DRD.DDIO_WRITE": 1,
        "system.ruby.Directory_Controller.M_DRD.Data": 1,
        "system.ruby.Directory_Controller.M_DRDI.Memory_Ack": 1,
        "system.ruby.Directory_Controller.I.DMA_READ": 1,
        "system.ruby.Directory_Controller.ID.Memory_Data": 1,
        "system.ruby.L2Cache_Controller.NP.DMA_WRITE_PARTIAL": 1,
        "system.ruby.L2Cache_Controller.DM_WF.Mem_Data": 1,
        "system.ruby.L2Cache_Controller.DM_WI.MEM_Inv": 2,
        "system.ruby.L2Cache_Controller.DM_WI.Ddio_Ack": 1,
        "system.ruby.L2Cache_Controller.M.MEM_Inv": 1,
        "system.ruby.L2Cache_Controller.M_I.MEM_Inv": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
    runtime_vector_expected={
        "system.ruby.DMA_Controller.ReadRequest": (0, 2),
        "system.ruby.DMA_Controller.Data": (0, 2),
        "system.ruby.DMA_Controller.WriteRequest": (1, 1),
        "system.ruby.DMA_Controller.Ack": (1, 1),
    },
    runtime_absent=(
        "system.ruby.Directory_Controller.I.DDIO_WRITE",
        "system.ruby.Directory_Controller.M.DDIO_WRITE",
        "system.ruby.Directory_Controller.M_DWR.DDIO_WRITE",
    ),
    dma_controllers=2,
)

_register(
    "mesi-ddio-partial-claim-races-generic-dma-write",
    "partial_claim_dma_write_race",
    {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 1,
        "txPayloadRequests": 0,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayFill::total": 1,
        "ddioWayAccess::total": 0,
        "wayDeallocations::way0": 1,
        "wayDeallocations::total": 1,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 1,
        "ddioOwnershipAcks": 1,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest::total": 1,
        "system.ruby.DMA_Controller.Data::total": 1,
        "system.ruby.DMA_Controller.WriteRequest::total": 3,
        "system.ruby.DMA_Controller.Ack::total": 3,
        "system.ruby.Directory_Controller.M.DMA_WRITE_FULL": 1,
        "system.ruby.Directory_Controller.M_DWR.DDIO_WRITE": 1,
        "system.ruby.Directory_Controller.M_DWR.Data": 1,
        "system.ruby.Directory_Controller.M_DWRI.Memory_Ack": 1,
        "system.ruby.Directory_Controller.I.DMA_READ": 1,
        "system.ruby.Directory_Controller.ID.Memory_Data": 1,
        "system.ruby.L2Cache_Controller.NP.DMA_WRITE_PARTIAL": 1,
        "system.ruby.L2Cache_Controller.DM_WF.Mem_Data": 1,
        "system.ruby.L2Cache_Controller.DM_WI.MEM_Inv": 2,
        "system.ruby.L2Cache_Controller.DM_WI.Ddio_Ack": 1,
        "system.ruby.L2Cache_Controller.M.MEM_Inv": 1,
        "system.ruby.L2Cache_Controller.M_I.MEM_Inv": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
    runtime_vector_expected={
        "system.ruby.DMA_Controller.ReadRequest": (0, 1),
        "system.ruby.DMA_Controller.Data": (0, 1),
        "system.ruby.DMA_Controller.WriteRequest": (1, 2),
        "system.ruby.DMA_Controller.Ack": (1, 2),
    },
    runtime_absent=(
        "system.ruby.Directory_Controller.I.DDIO_WRITE",
        "system.ruby.Directory_Controller.M.DDIO_WRITE",
        "system.ruby.Directory_Controller.M_DRD.DDIO_WRITE",
    ),
    dma_controllers=2,
)

_register(
    "mesi-ddio-retained-aligned-one-byte-write",
    "partial_one_byte",
    {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 1,
        "txPayloadRequests": 1,
        "txPayloadHits": 1,
        "txPayloadMisses": 0,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::total": 1,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 1,
        "ddioOwnershipAcks": 1,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 1,
        "system.ruby.DMA_Controller.Data": 1,
        "system.ruby.DMA_Controller.WriteRequest": 2,
        "system.ruby.DMA_Controller.Ack": 2,
        "system.ruby.L2Cache_Controller.NP.DMA_WRITE_PARTIAL": 1,
        "system.ruby.L2Cache_Controller.DM_WF.Mem_Data": 1,
        "system.ruby.L2Cache_Controller.DM_WI.Ddio_Ack": 1,
        "system.ruby.Directory_Controller.M.DDIO_WRITE": 1,
    },
)

_register(
    "mesi-ddio-retained-unaligned-partial-write",
    "partial_unaligned",
    {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 1,
        "txPayloadRequests": 1,
        "txPayloadHits": 1,
        "txPayloadMisses": 0,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::total": 1,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 1,
        "ddioOwnershipAcks": 1,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 1,
        "system.ruby.DMA_Controller.Data": 1,
        "system.ruby.DMA_Controller.WriteRequest": 2,
        "system.ruby.DMA_Controller.Ack": 2,
        "system.ruby.L2Cache_Controller.NP.DMA_WRITE_PARTIAL": 1,
        "system.ruby.L2Cache_Controller.DM_WF.Mem_Data": 1,
        "system.ruby.L2Cache_Controller.DM_WI.Ddio_Ack": 1,
        "system.ruby.Directory_Controller.M.DDIO_WRITE": 1,
    },
)

_register(
    "mesi-ddio-partial-hit-in-l2-m",
    "partial_l2_m",
    {
        "rxPayloadRequests": 2,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 1,
        "txPayloadRequests": 1,
        "txPayloadHits": 1,
        "txPayloadMisses": 0,
        "ddioWayFill::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::total": 2,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 1,
        "ddioOwnershipAcks": 1,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 1,
        "system.ruby.DMA_Controller.Data": 1,
        "system.ruby.DMA_Controller.WriteRequest": 2,
        "system.ruby.DMA_Controller.Ack": 2,
        "system.ruby.L2Cache_Controller.NP.DMA_WRITE_FULL": 1,
        "system.ruby.L2Cache_Controller.M.DMA_WRITE_PARTIAL": 1,
        "system.ruby.L2Cache_Controller.DM_WI.Ddio_Ack": 1,
        "system.ruby.Directory_Controller.I.DDIO_WRITE": 1,
    },
)

_register(
    "mesi-ddio-partial-invalidates-ss-sharers",
    "partial_ss_sharers",
    {
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
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 1,
        "system.ruby.DMA_Controller.Data": 1,
        "system.ruby.DMA_Controller.WriteRequest": 2,
        "system.ruby.DMA_Controller.Ack": 2,
        "system.ruby.L2Cache_Controller.SS.DMA_WRITE_PARTIAL": 1,
        "system.ruby.L2Cache_Controller.DM_WS.Ack": 1,
        "system.ruby.L2Cache_Controller.DM_WS.Ack_all": 1,
    },
    runtime_absent=("system.ruby.L2Cache_Controller.DM_WS.WB_Data",),
)

_register(
    "mesi-ddio-partial-write-clean-exclusive-owner",
    "partial_clean_owner",
    {
        "rxPayloadRequests": 1,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 0,
        "txPayloadRequests": 2,
        "txPayloadHits": 1,
        "txPayloadMisses": 1,
        "ddioWayFill::nic_rx_payload_way0": 0,
        "ddioWayFill::cpu_other_way0": 1,
        "ddioWayFill::total": 1,
        "ddioWayAccess::nic_rx_payload_way0": 1,
        "ddioWayAccess::nic_tx_payload_way0": 1,
        "ddioWayAccess::total": 2,
        "wayDeallocations::way0": 1,
        "wayDeallocations::total": 1,
        "dmaRoutingProxyRequests": 1,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 3,
        "system.ruby.DMA_Controller.Data": 3,
        "system.ruby.DMA_Controller.WriteRequest": 2,
        "system.ruby.DMA_Controller.Ack": 2,
        "system.ruby.L2Cache_Controller.MT.DMA_WRITE_PARTIAL": 1,
        "system.ruby.L2Cache_Controller.DM_WM.Ack_all": 1,
        "system.ruby.L2Cache_Controller.M.DMA_TX_READ": 1,
        "system.ruby.L2Cache_Controller.NP.DMA_TX_READ": 1,
    },
    runtime_absent=(
        "system.ruby.L2Cache_Controller.DM_WM.WB_Data",
        "system.ruby.L2Cache_Controller.DM_WM.WB_Data_clean",
    ),
)

_register(
    "mesi-ddio-partial-merges-dirty-private-owner",
    "partial_dirty_owner",
    {
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
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 1,
        "system.ruby.DMA_Controller.Data": 1,
        "system.ruby.DMA_Controller.WriteRequest": 2,
        "system.ruby.DMA_Controller.Ack": 2,
        "system.ruby.L2Cache_Controller.MT.DMA_WRITE_PARTIAL": 1,
        "system.ruby.L2Cache_Controller.DM_WM.WB_Data": 1,
    },
    runtime_absent=("system.ruby.L2Cache_Controller.DM_WM.Ack_all",),
)

_register(
    "mesi-ddio-no-retention-partial-clean-and-dirty-owner",
    "partial_no_retention",
    {
        "rxPayloadRequests": 2,
        "rxPayloadHits": 1,
        "rxPayloadMisses": 1,
        "txPayloadRequests": 2,
        "txPayloadHits": 0,
        "txPayloadMisses": 2,
        "ddioWayFill::cpu_other_way0": 1,
        "ddioWayFill::total": 1,
        "ddioWayAccess::nic_rx_payload_way0": 1,
        "ddioWayAccess::total": 1,
        "wayDeallocations::way0": 1,
        "wayDeallocations::total": 1,
        "dmaRoutingProxyRequests": 4,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    ddio_way_part=-1,
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 2,
        "system.ruby.DMA_Controller.Data": 2,
        "system.ruby.DMA_Controller.WriteRequest": 4,
        "system.ruby.DMA_Controller.Ack": 4,
        "system.ruby.Directory_Controller.I.DMA_WRITE_PARTIAL": 1,
        "system.ruby.Directory_Controller.ID_WF.Memory_Data": 1,
        "system.ruby.Directory_Controller.M.DMA_WRITE_PARTIAL": 1,
        "system.ruby.Directory_Controller.M_DWR.Data": 1,
        "system.ruby.Directory_Controller.M_DWRI.Memory_Ack": 1,
        "system.ruby.L2Cache_Controller.MT.MEM_Inv": 1,
    },
)

_register(
    "mesi-ddio-contiguous-cross-line-partial-write",
    "partial_cross_line",
    {
        "rxPayloadRequests": 2,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 2,
        "txPayloadRequests": 2,
        "txPayloadHits": 2,
        "txPayloadMisses": 0,
        "ddioWayFill::nic_rx_payload_way0": 2,
        "ddioWayFill::total": 2,
        "ddioWayAccess::nic_tx_payload_way0": 2,
        "ddioWayAccess::total": 2,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 2,
        "ddioOwnershipAcks": 2,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 2,
        "system.ruby.DMA_Controller.Data": 2,
        "system.ruby.DMA_Controller.WriteRequest": 4,
        "system.ruby.DMA_Controller.Ack": 4,
        "system.ruby.L2Cache_Controller.NP.DMA_WRITE_PARTIAL": 2,
        "system.ruby.L2Cache_Controller.DM_WF.Mem_Data": 2,
        "system.ruby.L2Cache_Controller.DM_WI.Ddio_Ack": 2,
        "system.ruby.Directory_Controller.M.DDIO_WRITE": 2,
    },
)

_register(
    "mesi-ddio-sparse-byte-enable-write",
    "sparse_mask",
    {
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
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 3,
        "system.ruby.DMA_Controller.Data": 3,
        "system.ruby.DMA_Controller.WriteRequest": 6,
        "system.ruby.DMA_Controller.Ack": 6,
        "system.ruby.L2Cache_Controller.NP.DMA_WRITE_PARTIAL": 2,
        "system.ruby.L2Cache_Controller.M.DMA_WRITE_PARTIAL": 1,
        "system.ruby.L2Cache_Controller.DM_WF.Mem_Data": 2,
        "system.ruby.L2Cache_Controller.DM_WI.Ddio_Ack": 2,
    },
)

_register(
    "mesi-ddio-partial-subset-replacement-race",
    "partial_subset_race",
    {
        "rxPayloadRequests": 3,
        "rxPayloadHits": 0,
        "rxPayloadMisses": 3,
        "txPayloadRequests": 0,
        "ddioWayFill::nic_rx_payload_way0": 3,
        "ddioWayFill::total": 3,
        "wayDeallocations::way0": 3,
        "wayDeallocations::total": 3,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 3,
        "ddioOwnershipRequests": 3,
        "ddioOwnershipAcks": 3,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 3,
        "system.ruby.DMA_Controller.Data": 3,
        "system.ruby.DMA_Controller.WriteRequest": 5,
        "system.ruby.DMA_Controller.Ack": 5,
        "system.ruby.L2Cache_Controller.NP.DMA_WRITE_PARTIAL": 2,
        "system.ruby.L2Cache_Controller.DM_WF.Mem_Data": 2,
        "system.ruby.L2Cache_Controller.DM_WI.DDIO_Replacement": 1,
        "system.ruby.L2Cache_Controller.DM_WI.Ddio_Ack": 3,
        "system.ruby.Directory_Controller.M.DDIO_WRITE": 2,
        "system.ruby.Directory_Controller.I.DDIO_WRITE": 1,
    },
)

_register(
    "mesi-ddio-zero-byte-enable-is-no-op",
    "zero_mask",
    {
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
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 1,
        "system.ruby.DMA_Controller.Data": 1,
        "system.ruby.DMA_Controller.WriteRequest": 1,
        "system.ruby.DMA_Controller.Ack": 1,
    },
    runtime_absent=(
        "system.ruby.L2Cache_Controller.NP.DMA_WRITE_PARTIAL",
        "system.ruby.L2Cache_Controller.NP.DMA_WRITE_FULL",
    ),
)

_register(
    "mesi-ddio-generic-partial-write-races-clean-l2-replacement",
    "clean_replacement_dma_partial_write",
    {
        "rxPayloadRequests": 0,
        "txPayloadRequests": 0,
        "ddioWayFill::cpu_other_way0": 2,
        "ddioWayFill::cpu_other_way1": 1,
        "ddioWayFill::cpu_other_way2": 1,
        "ddioWayFill::cpu_other_way3": 1,
        "ddioWayFill::total": 5,
        "ddioWayAccess::total": 0,
        "dmaRoutingProxyRequests": 0,
        "dmaRoutingTransientRecycles": 0,
        "ddioReplacementStalls": 0,
        "ddioOwnershipRequests": 0,
        "ddioOwnershipAcks": 0,
    },
    runtime_expected={
        "system.ruby.DMA_Controller.ReadRequest": 1,
        "system.ruby.DMA_Controller.Data": 1,
        "system.ruby.DMA_Controller.WriteRequest": 6,
        "system.ruby.DMA_Controller.Ack": 6,
        "system.ruby.Directory_Controller.M.DMA_WRITE_PARTIAL": 1,
        "system.ruby.Directory_Controller.M_DWR.CleanReplacementPartial": 1,
        "system.ruby.Directory_Controller.ID_WF.Memory_Data": 1,
        "system.ruby.Directory_Controller.ID_W.Memory_Ack": 6,
        "system.ruby.L2Cache_Controller.M.L2_Replacement_clean": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
    runtime_absent=("system.ruby.Directory_Controller.M_DWR.Data",),
)
