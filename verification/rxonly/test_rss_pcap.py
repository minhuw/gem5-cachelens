#!/usr/bin/env python3
"""Six stdlib-only PCAP format/checksum/accounting checks."""
import collections
import struct
import tempfile
import unittest
from pathlib import Path

from make_rss_pcap import checksum, decode_frame, frame, read_pcap, write_pcap


class PcapTests(unittest.TestCase):
    def test_balanced_accounting(self):
        decoded = [decode_frame(frame(i)) for i in range(4096)]
        self.assertEqual(collections.Counter(f for f, _, _ in decoded), dict.fromkeys(range(64), 64))
        self.assertEqual([i for _, _, i in decoded], list(range(4096)))
        self.assertEqual({s for f, s, _ in decoded if f == 63}, set(range(64)))

    def test_checksums(self):
        packet = frame(123)
        self.assertEqual(checksum(packet[14:34]), 0)
        pseudo = packet[26:34] + struct.pack('!BBH', 0, 17, len(packet) - 34)
        self.assertEqual(checksum(pseudo + packet[34:]), 0)
        self.assertNotEqual(packet[40:42], b'\0\0')
        damaged = bytearray(packet)
        damaged[-1] ^= 1
        with self.assertRaises(ValueError):
            decode_frame(bytes(damaged))
        damaged = bytearray(packet)
        damaged[22] ^= 1
        with self.assertRaises(ValueError):
            decode_frame(bytes(damaged))

    def test_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            a, b = Path(directory) / 'a', Path(directory) / 'b'
            write_pcap(a)
            write_pcap(b)
            self.assertEqual(a.read_bytes(), b.read_bytes())
            self.assertEqual(len(read_pcap(a)), 4096)

    def test_timestamps(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace'
            write_pcap(path, 3)
            self.assertEqual([t for t, _ in read_pcap(path)], [0, 2500, 5000])
            # EtherDump's little-endian microsecond PCAP uses the same parser.
            path.write_bytes(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 1500, 1)
                             + struct.pack('<IIII', 1, 23, 1472, 1472) + frame(0))
            self.assertEqual(read_pcap(path), [(1_000_023_000, frame(0))])

    def test_reject_malformed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace'
            write_pcap(path, 1)
            good = path.read_bytes()
            for bad in (b'', good[:23], good[:30], good[:-1], good[:36] + struct.pack('<I', 1473) + good[40:]):
                path.write_bytes(bad)
                with self.assertRaises(ValueError):
                    read_pcap(path)
            for count, rate in ((0, 400000), (4096, 0), (4096, 800001)):
                with self.assertRaises(ValueError):
                    write_pcap(path, count, rate)

    def test_payload_layout(self):
        packet = frame(4095)
        self.assertEqual(packet[:12], bytes.fromhex('029000000001008000000001'))
        self.assertEqual(struct.unpack('!HH', packet[34:38]), (10063, 11211))
        self.assertEqual(struct.unpack('!4sIII', packet[42:58]), (b'CLRX', 63, 63, 4095))
        self.assertEqual(decode_frame(packet), (63, 63, 4095))


if __name__ == '__main__':
    unittest.main()
