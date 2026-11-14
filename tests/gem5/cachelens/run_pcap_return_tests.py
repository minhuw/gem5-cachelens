#!/usr/bin/env python3
# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Native RX-boundary regression; requires a gem5 build with HAVE_PCAP.

Run with --gem5 /path/to/matching/gem5.opt. Direction comes from the config's
EtherTapStub -> LoadGeneratorPcap binding and the dedicated return_dump hook,
not from these byte comparisons. Production forward gates must require that
binding and its output; disabled capture remains valid legacy RX behavior.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile

from run_pcap_replay_tests import read_pcap_records, write_pcap


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument(
        "--cases", nargs="+", choices=("enabled", "disabled", "small", "zero"),
        default=("enabled", "disabled", "small", "zero"),
    )
    args = parser.parse_args()
    gem5 = args.gem5.resolve()
    config = Path(__file__).resolve().parent / "configs/load_generator_pcap_return.py"
    # A logical 1472B frame. Capture compares all bytes; no implicit FCS strip.
    unexpected = bytes.fromhex("0290000000010080000000010800") + bytes(
        index % 256 for index in range(1472 - 14)
    )
    short = b"short\x00\xff"
    accepted = bytes.fromhex("0080000000010290000000010800") + bytes(range(46))
    frames = [unexpected, short, accepted]

    with tempfile.TemporaryDirectory(prefix="gem5-return-") as temp:
        work = Path(temp)
        replay = work / "replay.pcap"
        injected = work / "injected.pcap"
        write_pcap(replay, [])  # Deferred replay must never send anything.
        write_pcap(injected, frames)
        for case in args.cases:
            out = work / case
            dump = out / "returns.pcap"
            command = [
                str(gem5), "--listener-mode=on", "--outdir", str(out), str(config),
                "--pcap", str(replay), "--frames", str(injected),
                "--socket", str(work / f"{case}.sock"),
            ]
            if case != "disabled":
                command += ["--dump", str(dump)]
            if case == "small":
                command += ["--maxlen", "96"]
            elif case == "zero":
                command += ["--maxlen", "0"]
            result = subprocess.run(
                command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=30,
            )
            if case in ("small", "zero"):
                assert result.returncode != 0, result.stdout
                diagnostic = (
                    "would truncate a 1472-byte return frame" if case == "small"
                    else "return_dump maxlen must be positive"
                )
                assert diagnostic in result.stdout, result.stdout
            else:
                assert result.returncode == 0, result.stdout
                assert "PCAP_RETURN_DONE injected=3" in result.stdout, result.stdout
                assert "Ignoring short PCAP load-generator response" in result.stdout
                assert "Ignoring response for another PCAP load-generator endpoint" in result.stdout
                stats = {}
                for line in (out / "stats.txt").read_text().splitlines():
                    columns = line.split()
                    if len(columns) >= 2 and not line.startswith("-"):
                        stats[columns[0]] = columns[1]
                prefix = "loadgen.LoadGeneratorPcap."
                assert stats[prefix + "recvPackets"] == "1", stats
                assert stats[prefix + "uncorrelatedPackets"] == "1", stats
                assert stats[prefix + "sentPackets"] == "0", stats
                if case == "enabled":
                    records = read_pcap_records(dump)
                    # Exact list equality checks cardinality, ordering, short-frame
                    # preservation, and the 1472-byte payload without FCS heuristics.
                    assert [frame for _, frame in records] == frames, records
                    assert [stamp for stamp, _ in records] == [0, 0, 0], records
                else:
                    assert not dump.exists(), dump
            print(f"PASS {case}")


if __name__ == "__main__":
    main()
