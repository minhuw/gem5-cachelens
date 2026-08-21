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


_MEMTEST_CONFIG = joinpath(
    config.base_dir, "configs", "example", "ruby_mem_test.py"
)
_DIRECTED_CONFIG = joinpath(
    config.base_dir,
    "tests",
    "gem5",
    "mesi_ddio",
    "configs",
    "mesi_ddio_directed.py",
)


class DMARoutingStatsVerifier(verifier.Verifier):
    def __init__(self, l2_banks, classified, max_reads=0):
        super().__init__()
        self._l2_banks = l2_banks
        self._classified = classified
        self._max_reads = max_reads

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

    @staticmethod
    def _integer_stat(stats, name):
        if name not in stats:
            test_util.fail(f"Missing required runtime statistic: {name}")
        value = stats[name]
        if not value.is_integer():
            test_util.fail(f"Expected integer statistic {name}, got {value}")
        return int(value)

    def _check_classified(self, stats):
        per_bank = {
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
        for bank in range(self._l2_banks):
            prefix = f"system.ruby.l2_cntrl{bank}.L2cache."
            for suffix, expected in per_bank.items():
                observed = self._integer_stat(stats, prefix + suffix)
                if observed != expected:
                    test_util.fail(
                        "Classified full-line routing used an unexpected "
                        f"outcome for bank {bank} {suffix}: "
                        f"observed={observed}, expected={expected}"
                    )

        for event in ("ReadRequest", "Data", "WriteRequest", "Ack"):
            observed = self._integer_stat(
                stats, f"system.ruby.DMA_Controller.{event}"
            )
            if observed != self._l2_banks:
                test_util.fail(
                    f"DMA controller {event} count {observed} does not "
                    f"match one full-line operation per L2 bank "
                    f"({self._l2_banks})"
                )

    def _check_generic(self, stats):
        dma_reads = self._integer_stat(
            stats, "system.dma_devices.numReads"
        )
        dma_writes = self._integer_stat(
            stats, "system.dma_devices.numWrites"
        )
        dma_atomics = self._integer_stat(
            stats, "system.dma_devices.numAtomics"
        )
        if (
            dma_reads != self._max_reads
            or dma_writes == 0
            or dma_atomics != 0
        ):
            test_util.fail(
                "Generic DMA MemTest did not complete the expected timing "
                f"requests: reads={dma_reads}, writes={dma_writes}, "
                f"atomics={dma_atomics}"
            )

        completed_events = (
            ("ReadRequest", dma_reads),
            ("Data", dma_reads),
            ("WriteRequest", dma_writes),
            ("Ack", dma_writes),
        )
        for event, completed in completed_events:
            observed = self._integer_stat(
                stats, f"system.ruby.DMA_Controller.{event}"
            )
            if observed != completed:
                test_util.fail(
                    f"DMA controller {event} count {observed} does not "
                    f"match completed count {completed}"
                )

        for bank in range(self._l2_banks):
            prefix = f"system.ruby.l2_cntrl{bank}.L2cache."
            for suffix in (
                "dmaRoutingProxyRequests",
                "dmaRoutingTransientRecycles",
                "ddioReplacementStalls",
                "ddioOwnershipRequests",
                "ddioOwnershipAcks",
                "rxPayloadRequests",
                "txPayloadRequests",
            ):
                observed = self._integer_stat(stats, prefix + suffix)
                if observed != 0:
                    test_util.fail(
                        "Generic DMA must route directly to the directory: "
                        f"bank={bank}, stat={suffix}, observed={observed}"
                    )

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        stats_path = joinpath(tempdir, constants.gem5_simulation_stats)
        if not os.path.isfile(stats_path):
            test_util.fail(f"Could not find gem5 stats file: {stats_path}")

        stats = self._read_stats(stats_path)
        if self._classified:
            self._check_classified(stats)
        else:
            self._check_generic(stats)


def _register_classified(name, l2_banks):
    gem5_verify_config(
        name=name,
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(
                re.compile(
                    r".*MESI DDIO directed scenario "
                    r"'routing_full_line' passed"
                )
            ),
            DMARoutingStatsVerifier(l2_banks, classified=True),
        ),
        config=_DIRECTED_CONFIG,
        config_args=(
            "--scenario=routing_full_line",
            f"--num-l2caches={l2_banks}",
            "--ddio-way-part=1",
            "--abs-max-tick=10000000",
        ),
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
        protocol="MESI_Two_Level",
    )


def _register_generic(name, l2_banks):
    max_reads = 100
    gem5_verify_config(
        name=name,
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(
                re.compile(
                    r"Exiting @ tick \d+ because maximum number of "
                    r"loads reached"
                )
            ),
            DMARoutingStatsVerifier(
                l2_banks, classified=False, max_reads=max_reads
            ),
        ),
        config=_MEMTEST_CONFIG,
        config_args=(
            "--num-cpus=2",
            "--num-dmas=1",
            "--num-nic-dmas=0",
            f"--num-l2caches={l2_banks}",
            "--maxloads=0",
            f"--dma-maxloads={max_reads}",
            "--tester-size=64",
            "--percent-reads=100",
            "--progress=100",
            "--abs-max-tick=100000000",
        ),
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
        protocol="MESI_Two_Level",
    )


_register_classified(
    name="mesi-ddio-classified-dma-single-l2-bank",
    l2_banks=1,
)
_register_classified(
    name="mesi-ddio-classified-dma-four-l2-banks",
    l2_banks=4,
)
_register_generic(
    name="mesi-ddio-generic-dma-directory-route",
    l2_banks=4,
)
