#!/usr/bin/env python3
"""Deterministic 1472-byte Ethernet/IPv4/UDP RSS input, no FCS.

PCAP timestamps describe offered spacing; LoadGeneratorPcap is constant-paced
by --packet-rate. EtherDump records actual link serialization completion at
microsecond resolution. Payload header at offset 42: !4sIII (CLRX, flow, seq, id).
"""
import argparse
import socket
import struct
from pathlib import Path

FRAME_SIZE = 1472
FLOWS = 64
SRC_MAC = bytes.fromhex('008000000001')
DST_MAC = bytes.fromhex('029000000001')
SRC_IP = socket.inet_aton('10.10.10.11')
DST_IP = socket.inet_aton('10.10.10.10')


def checksum(data):
    data += b'\0' * (len(data) % 2)
    total = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def frame(index):
    flow, seq = index % FLOWS, index // FLOWS
    payload = struct.pack('!4sIII', b'CLRX', flow, seq, index)
    payload += bytes([flow]) * (FRAME_SIZE - 42 - len(payload))
    udp = struct.pack('!HHHH', 10000 + flow, 11211, 8 + len(payload), 0) + payload
    pseudo = SRC_IP + DST_IP + struct.pack('!BBH', 0, 17, len(udp))
    udp = udp[:6] + struct.pack('!H', checksum(pseudo + udp) or 65535) + udp[8:]
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), index % 65536,
                     0x4000, 64, 17, 0, SRC_IP, DST_IP)
    ip = ip[:10] + struct.pack('!H', checksum(ip)) + ip[12:]
    return DST_MAC + SRC_MAC + b'\x08\x00' + ip + udp


def write_pcap(path, count=4096, packet_rate=400_000):
    if not 1 <= count <= 2**32 or not 1 <= packet_rate <= 800_000:
        raise ValueError('count must fit uint32 indices; rate must be 1..800000 pps')
    with Path(path).open('wb') as out:
        # Nanosecond PCAP avoids rounding the offered 2.5us interval.
        out.write(struct.pack('<IHHIIII', 0xa1b23c4d, 2, 4, 0, 0, FRAME_SIZE, 1))
        for i in range(count):
            sec, ns = divmod(i * 1_000_000_000 // packet_rate, 1_000_000_000)
            out.write(struct.pack('<IIII', sec, ns, FRAME_SIZE, FRAME_SIZE))
            out.write(frame(i))


def read_pcap(path):
    """Return (timestamp_ns, full_frame) records; reject truncated captures."""
    data = Path(path).read_bytes()
    formats = {b'\x4d\x3c\xb2\xa1': ('<', 1), b'\xa1\xb2\x3c\x4d': ('>', 1),
               b'\xd4\xc3\xb2\xa1': ('<', 1000), b'\xa1\xb2\xc3\xd4': ('>', 1000)}
    if len(data) < 24 or data[:4] not in formats:
        raise ValueError('invalid PCAP header')
    endian, scale = formats[data[:4]]
    major, minor, _, _, snaplen, link = struct.unpack(endian + 'HHIIII', data[4:24])
    if (major, minor, link) != (2, 4, 1) or not snaplen:
        raise ValueError('expected Ethernet PCAP v2.4')
    records, pos = [], 24
    while pos < len(data):
        if len(data) - pos < 16:
            raise ValueError('truncated PCAP record header')
        sec, sub, caplen, length = struct.unpack_from(endian + 'IIII', data, pos)
        pos += 16
        if caplen != length or caplen > snaplen or caplen < 14 or pos + caplen > len(data):
            raise ValueError('truncated or invalid Ethernet record')
        timestamp = sec * 1_000_000_000 + sub * scale
        if sub * scale >= 1_000_000_000 or (records and timestamp < records[-1][0]):
            raise ValueError('invalid/nonmonotonic PCAP timestamp')
        records.append((timestamp, data[pos:pos + caplen]))
        pos += caplen
    return records


def decode_frame(packet):
    """Validate wire checksums and CLRX identifiers; return (flow, seq, id)."""
    if len(packet) != FRAME_SIZE or packet[12:14] != b'\x08\x00':
        raise ValueError('expected 1472-byte IPv4 Ethernet frame')
    ip, udp = packet[14:34], packet[34:]
    if (ip[0] != 0x45 or ip[9] != 17 or struct.unpack('!H', ip[2:4])[0] != 1458
            or struct.unpack('!H', ip[6:8])[0] & 0x3fff or checksum(ip)):
        raise ValueError('invalid IPv4 header/checksum')
    src, dst, length, check = struct.unpack('!HHHH', udp[:8])
    pseudo = ip[12:20] + struct.pack('!BBH', 0, 17, len(udp))
    if dst != 11211 or length != len(udp) or not check or checksum(pseudo + udp):
        raise ValueError('invalid UDP header/checksum')
    magic, flow, seq, index = struct.unpack('!4sIII', udp[8:24])
    if magic != b'CLRX' or flow >= FLOWS or src != 10000 + flow or (flow, seq) != (index % FLOWS, index // FLOWS):
        raise ValueError('invalid CLRX flow/sequence accounting')
    return flow, seq, index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--count', type=int, default=4096)
    parser.add_argument('--packet-rate', type=int, default=400_000)
    args = parser.parse_args()
    try:
        write_pcap(args.output, args.count, args.packet_rate)
    except ValueError as error:
        parser.error(str(error))
    print(f'RSS_PCAP path={args.output} frames={args.count} flows={FLOWS} bytes={FRAME_SIZE} pps={args.packet_rate}')


if __name__ == '__main__':
    main()
