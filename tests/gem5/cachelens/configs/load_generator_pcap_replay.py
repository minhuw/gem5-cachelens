# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Minimal PCAP load-generator replay/checkpoint test configuration."""

import argparse

import m5
from m5.objects import (
    EtherDump,
    EtherLink,
    LoadGeneratorPcap,
    Root,
)


parser = argparse.ArgumentParser()
parser.add_argument("--pcap", required=True)
parser.add_argument("--dump", required=True)
parser.add_argument(
    "--stack-mode",
    choices=("KernelStack", "DPDKStack"),
    default="DPDKStack",
)
parser.add_argument("--max-packet-size", type=int, default=1514)
parser.add_argument("--packet-rate", type=int, default=100_000)
parser.add_argument("--link-speed", default="100Gbps")
parser.add_argument("--start-tick", type=int, default=1)
parser.add_argument("--stop-tick", type=int, default=(1 << 64) - 1)
parser.add_argument("--ticks", type=int, default=200_000_000)
parser.add_argument("--checkpoint")
parser.add_argument("--restore")
args = parser.parse_args()

root = Root(full_system=False)
root.loadgen = LoadGeneratorPcap(
    pcap_filename=args.pcap,
    max_packetsize=args.max_packet_size,
    port_filter=11211,
    stack_mode=args.stack_mode,
    replace_src_ip="10.10.10.11",
    replace_dest_ip="10.10.10.10",
    replay_mode="SimpleReplay",
    packet_rate=args.packet_rate,
    start_tick=args.start_tick,
    stop_tick=args.stop_tick,
)
root.dump = EtherDump(file=args.dump, maxlen=65535)
root.link = EtherLink(speed=args.link_speed, delay="0ns", dump=root.dump)
root.link.int0 = root.loadgen.interface

m5.instantiate(args.restore)
event = m5.simulate(args.ticks)
if args.checkpoint:
    m5.checkpoint(args.checkpoint)
    print(f"PCAP_CHECKPOINT_SAVED tick={m5.curTick()} path={args.checkpoint}")
else:
    print(f"PCAP_REPLAY_DONE tick={m5.curTick()} cause={event.getCause()}")
