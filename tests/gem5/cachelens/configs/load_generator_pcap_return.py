# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Inject raw returns through EtherTapStub into the load-generator RX hook."""

import argparse
from pathlib import Path
import socket
import struct
import sys

import m5
from m5.objects import EtherDump, EtherTapStub, LoadGeneratorPcap, Root

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_pcap_replay_tests import read_pcap


parser = argparse.ArgumentParser()
parser.add_argument("--pcap", required=True)
parser.add_argument("--frames", required=True)
parser.add_argument("--socket", required=True)
parser.add_argument("--dump")
parser.add_argument("--maxlen", type=int, default=65535)
args = parser.parse_args()

root = Root(full_system=False)
root.loadgen = LoadGeneratorPcap(
    pcap_filename=args.pcap,
    deferred_start=True,
    stop_tick=(1 << 64) - 1,
)
if args.dump:
    root.loadgen.return_dump = EtherDump(file=args.dump, maxlen=args.maxlen)
root.tap = EtherTapStub(port=args.socket)
root.tap.tap = root.loadgen.interface
m5.instantiate()

# This binding establishes direction. The PCAP comparison only checks content.
# FASYNC setup in EtherTapStub schedules polling even for already queued data.
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
    peer.connect(args.socket)
    frames = read_pcap(Path(args.frames))
    peer.sendall(b"".join(struct.pack("!I", len(frame)) + frame for frame in frames))
    for _ in range(4):
        m5.simulate(1)

m5.stats.dump()
print(f"PCAP_RETURN_DONE injected={len(frames)} tick={m5.curTick()}")
