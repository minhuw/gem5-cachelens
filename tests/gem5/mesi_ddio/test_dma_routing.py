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


_CONFIG = joinpath(config.base_dir, "configs", "example", "ruby_mem_test.py")
_L2_ROUTING_STAT = re.compile(
    r"^system\.ruby\.l2_cntrl(?P<bank>\d+)\.L2cache\."
    r"(?P<stat>dmaRoutingProxyRequests|dmaRoutingTransientRecycles)$"
)


class DMARoutingStatsVerifier(verifier.Verifier):
    def __init__(self, l2_banks, classified, max_reads):
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

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        stats_path = joinpath(tempdir, constants.gem5_simulation_stats)
        if not os.path.isfile(stats_path):
            test_util.fail(f"Could not find gem5 stats file: {stats_path}")

        stats = self._read_stats(stats_path)
        proxy = {}
        recycle = {}
        for name, value in stats.items():
            match = _L2_ROUTING_STAT.match(name)
            if not match:
                continue
            bank = int(match.group("bank"))
            target = (
                proxy
                if match.group("stat").endswith("ProxyRequests")
                else recycle
            )
            target[bank] = self._integer_stat(stats, name)

        expected_banks = set(range(self._l2_banks))
        if set(proxy) != expected_banks or set(recycle) != expected_banks:
            test_util.fail(
                "Missing per-bank DMA routing telemetry: "
                f"proxy={proxy}, recycle={recycle}, expected={expected_banks}"
            )

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
                "DMA MemTest did not complete the expected timing requests: "
                f"reads={dma_reads}, writes={dma_writes}, "
                f"atomics={dma_atomics}"
            )

        # Request and response events are counted at the DMA controller. Their
        # equality proves that both read data and write acknowledgements made
        # the full round trip back to the DMA sequencer.
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
                    f"DMA controller {event} count {observed} does not match "
                    f"completed count {completed}"
                )

        completed = dma_reads + dma_writes
        if self._classified:
            expected_proxy = {bank: 0 for bank in expected_banks}
            expected_proxy[0] = completed
            if proxy != expected_proxy:
                test_util.fail(
                    "Classified DMA used unexpected L2 bank(s): "
                    f"observed={proxy}, expected={expected_proxy}"
                )

            # CPUs and the DMA tester repeatedly read the same two lines. This
            # deterministically places bank 0 in transient states while the
            # classified request arrives and exercises the recycle action.
            if recycle[0] <= 0 or any(
                count != 0 for bank, count in recycle.items() if bank != 0
            ):
                test_util.fail(
                    "Transient DMA recycling was not confined to L2 bank 0: "
                    f"{recycle}"
                )
        elif any(proxy.values()) or any(recycle.values()):
            test_util.fail(
                "Generic DMA must route directly to the directory without "
                f"L2 proxy/recycle activity: proxy={proxy}, recycle={recycle}"
            )


def _register_routing_smoke(name, l2_banks, classified):
    max_reads = 100
    args = [
        "--num-cpus=2",
        "--num-dmas=1",
        f"--num-nic-dmas={1 if classified else 0}",
        f"--num-l2caches={l2_banks}",
        "--maxloads=0",
        f"--dma-maxloads={max_reads}",
        "--tester-size=64",
        "--percent-reads=100",
        "--progress=100",
        "--abs-max-tick=100000000",
    ]

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
            DMARoutingStatsVerifier(l2_banks, classified, max_reads),
        ),
        config=_CONFIG,
        config_args=args,
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
        protocol="MESI_Two_Level",
    )


_register_routing_smoke(
    name="mesi-ddio-classified-dma-single-l2-bank",
    l2_banks=1,
    classified=True,
)
_register_routing_smoke(
    name="mesi-ddio-classified-dma-four-l2-banks",
    l2_banks=4,
    classified=True,
)
_register_routing_smoke(
    name="mesi-ddio-generic-dma-directory-route",
    l2_banks=4,
    classified=False,
)
