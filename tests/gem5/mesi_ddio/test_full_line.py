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
import subprocess
from pathlib import Path

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
_PARTIAL_REJECTION = (
    "MESI DDIO classified NIC DMA writes require one aligned full cache "
    "line; partial writes are unsupported until Plan 004"
)


class MESIDDIOStatsVerifier(verifier.Verifier):
    def __init__(self, expected, minimum=None, runtime_expected=None):
        super().__init__()
        self._expected = expected
        self._minimum = minimum or {}
        self._runtime_expected = runtime_expected or {}

    @staticmethod
    def _read_stats(path):
        stats = {}
        with open(path, encoding="utf-8") as stats_file:
            for line in stats_file:
                fields = line.split()
                if len(fields) < 2:
                    continue
                try:
                    value = float(fields[1])
                except ValueError:
                    continue
                stats[fields[0]] = value
        return stats

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        stats_path = joinpath(tempdir, constants.gem5_simulation_stats)
        if not os.path.isfile(stats_path):
            test_util.fail(f"Could not find gem5 stats file: {stats_path}")

        stats = self._read_stats(stats_path)
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
            # Protocol transition stats with zero occurrences are omitted.
            observed = stats.get(name, 0)
            if observed != expected:
                test_util.fail(
                    f"Unexpected runtime statistic {name}: "
                    f"observed={observed}, expected={expected}"
                )


def _register(
    name,
    scenario,
    expected,
    ddio_way_part=1,
    minimum=None,
    runtime_expected=None,
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
            MESIDDIOStatsVerifier(expected, minimum, runtime_expected),
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


def _register_partial_rejection(name, scenario):
    def run_rejection(params):
        tempdir = Path(
            params.fixtures[constants.tempdir_fixture_name].path
        )
        gem5 = params.fixtures[constants.gem5_binary_fixture_name].path
        output_dir = tempdir / scenario
        result = subprocess.run(
            (
                gem5,
                "-d",
                output_dir.as_posix(),
                _CONFIG,
                f"--scenario={scenario}",
                "--ddio-way-part=1",
                "--abs-max-tick=10000000",
            ),
            cwd=config.base_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        print(result.stdout)
        if result.returncode == 0:
            test_util.fail(
                f"Classified partial-write scenario {scenario} silently "
                "completed"
            )
        if _PARTIAL_REJECTION not in result.stdout:
            test_util.fail(
                f"Scenario {scenario} failed without the deterministic "
                "Plan 004 partial-write rejection"
            )
        if f"directed scenario '{scenario}' passed" in result.stdout:
            test_util.fail(
                f"Scenario {scenario} reported completion after rejection"
            )

    for host in constants.supported_hosts:
        suite_name = f"{name}-X86-{host}-opt-MESI_Two_Level"
        TestSuite(
            name=suite_name,
            fixtures=(
                Gem5Fixture(
                    constants.x86_tag,
                    constants.opt_tag,
                    protocol="MESI_Two_Level",
                ),
                TempdirFixture(),
            ),
            tests=(TestFunction(run_rejection, name=suite_name),),
            tags=(
                constants.x86_tag,
                constants.opt_tag,
                constants.quick_tag,
                host,
            ),
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
        "system.ruby.Directory_Controller.M.DMA_WRITE": 1,
        "system.ruby.Directory_Controller.M_DWR.Data": 1,
        "system.ruby.Directory_Controller.M_DWR.CleanReplacement": 0,
        "system.ruby.Directory_Controller.M_DWRI.Memory_Ack": 1,
    },
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
        "system.ruby.Directory_Controller.M_DRD.Data": 0,
        "system.ruby.Directory_Controller.ID.Memory_Data": 1,
        "system.ruby.L2Cache_Controller.M.L2_Replacement_clean": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
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
        "system.ruby.Directory_Controller.M.DMA_WRITE": 1,
        "system.ruby.Directory_Controller.M_DWR.CleanReplacement": 1,
        "system.ruby.Directory_Controller.M_DWR.Data": 0,
        "system.ruby.Directory_Controller.ID_W.Memory_Ack": 6,
        "system.ruby.L2Cache_Controller.M.L2_Replacement_clean": 1,
        "system.ruby.L2Cache_Controller.M_I.Mem_Ack": 1,
    },
)

_register_partial_rejection(
    "mesi-ddio-rejects-classified-one-byte-write",
    "partial_one_byte",
)
_register_partial_rejection(
    "mesi-ddio-rejects-classified-unaligned-write",
    "partial_unaligned",
)
_register_partial_rejection(
    "mesi-ddio-rejects-classified-cross-line-write",
    "partial_cross_line",
)
