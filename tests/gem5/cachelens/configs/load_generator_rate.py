# SPDX-License-Identifier: BSD-3-Clause

"""Asset-free offered-frame/backpressure check, not receiver-driver delivery."""

import argparse
from pathlib import Path
import re
import struct

import m5
from m5.objects import EtherDump, EtherLink, LoadGenerator, Root

parser = argparse.ArgumentParser()
parser.add_argument("--link-speed", choices=("1Gbps", "10Gbps"), required=True)
args = parser.parse_args()

m5.ticks.setGlobalFrequency("1THz")
packet_size = 1472  # Complete generated frame, including 14-byte MAC header.
packet_rate = 800000
period = 1250000  # Exactly 1.25 us in ticks.
attempts = 100
window = attempts * period
root = Root(full_system=False)
root.source = LoadGenerator(
    mode="Static", packet_size=packet_size, packet_rate=packet_rate,
    start_tick=0, stop_tick=window, loadgen_id=0,
)
# This sink emits nothing and intentionally rejects the generator-origin MACs.
# Its recvPackets is NOT a measure of delivery; PCAP counts serialization only.
root.sink = LoadGenerator(start_tick=0, stop_tick=0)
root.capture = EtherDump(file="serialized.pcap", maxlen=packet_size)
root.link = EtherLink(
    speed=args.link_speed, delay="200us", delay_var="0ns", dump=root.capture,
)
root.source.interface = root.link.int0
root.sink.interface = root.link.int1
m5.instantiate()
exit_event = m5.simulate(400000000)  # 400 us: generation + serialization + delay.
assert exit_event.getCause() == "simulate() limit reached"
m5.stats.dump()
stats = (Path(m5.options.outdir) / "stats.txt").read_text()


def stat(endpoint, name):
    match = re.search(
        rf"^{endpoint}\.LoadGenerator\.{name}\s+(\S+)", stats, re.MULTILINE
    )
    assert match, (endpoint, name)
    return int(match.group(1))


attempted = stat("source", "attemptedPackets")
sent = stat("source", "sentPackets")
dropped = stat("source", "backpressureDrops")
assert attempted == attempts == sent + dropped
assert stat("sink", "attemptedPackets") == 0
assert stat("sink", "recvPackets") == 0
assert stat("source", "recvPackets") == 0

# EtherLink uses ceil(frame_bytes * ticks_per_byte + 1); EtherDump truncates
# completion times to microseconds. Payload timestamps identify send attempts.
serialization_ticks = (1177600 if args.link_speed == "10Gbps" else 11776000) + 1
stride = (serialization_ticks + period - 1) // period
expected_sends = list(range(0, attempts, stride))
if args.link_speed == "10Gbps":
    assert (attempted, sent, dropped) == (100, 100, 0)
else:
    assert dropped > 0 and sent > 0
assert sent == len(expected_sends)

pcap = (Path(m5.options.outdir) / "serialized.pcap").read_bytes()
endian = "<" if pcap[:4] == b"\xd4\xc3\xb2\xa1" else ">"
magic, major, minor, zone, sigfigs, snaplen, network = struct.unpack(
    endian + "IHHIIII", pcap[:24]
)
assert (magic, major, minor, network) == (0xA1B2C3D4, 2, 4, 1)
offset = 24
serialized = 0
for attempt in expected_sends:
    seconds, micros, captured, length = struct.unpack_from(
        endian + "IIII", pcap, offset
    )
    offset += 16
    assert captured == length == packet_size
    frame = pcap[offset:offset + captured]
    assert len(frame) == packet_size
    assert frame[:14] == bytes.fromhex("02900000000100800000000105c0")
    send_tick = struct.unpack_from("=Q", frame, 14)[0]
    assert send_tick == 1 + attempt * period
    assert seconds * 1000000 + micros == (
        send_tick + serialization_ticks
    ) // 1000000
    offset += captured
    serialized += 1
assert offset == len(pcap)  # No extra records, including from the sink.
assert serialized == sent

# Rates use the fixed 125-us offered window, not the drain interval. No FCS,
# preamble, IFG, UDP payload, or receiver-driver throughput claim is made.
accepted_bps = sent * packet_size * 8 * 10**12 // window
serialized_bps = serialized * packet_size * 8 * 10**12 // window
print(
    f"CACHELENS_LOAD_GENERATOR_RATE_OK speed={args.link_speed} "
    f"attempted={attempted} sent={sent} backpressure_drops={dropped} "
    f"serialized_packets={serialized} offered_frame_bps=9420800000 "
    f"accepted_frame_bps={accepted_bps} serialized_frame_bps={serialized_bps} "
    "rate_window_us=125 receiver_driver_delivery=not_measured"
)
