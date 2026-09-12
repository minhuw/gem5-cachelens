#!/usr/bin/env python3
# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Exercise generated-PCAP load-generator replay and checkpoint cases."""

import argparse
import ipaddress
from pathlib import Path
import struct
import subprocess
import tempfile


ETH_HEADER = 14
NEW_DST_MAC = bytes.fromhex("029000000001")
NEW_SRC_MAC = bytes.fromhex("008000000001")
OLD_DST_MAC = bytes.fromhex("102030405060")
OLD_SRC_MAC = bytes.fromhex("a0b0c0d0e0f0")
OLD_SRC_IP = ipaddress.IPv4Address("192.0.2.1").packed
OLD_DST_IP = ipaddress.IPv4Address("198.51.100.2").packed
NEW_SRC_IP = ipaddress.IPv4Address("10.10.10.11").packed
NEW_DST_IP = ipaddress.IPv4Address("10.10.10.10").packed


def checksum(data: bytes, initial: int = 0) -> int:
    if len(data) % 2:
        data += b"\x00"
    total = initial
    for offset in range(0, len(data), 2):
        total += (data[offset] << 8) | data[offset + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def pseudo_header(src: bytes, dst: bytes, protocol: int, length: int) -> bytes:
    return src + dst + struct.pack("!BBH", 0, protocol, length)


def udp_datagram(payload: bytes, src=OLD_SRC_IP, dst=OLD_DST_IP) -> bytes:
    header = struct.pack("!HHHH", 12345, 11211, 8 + len(payload), 0)
    value = checksum(
        pseudo_header(src, dst, 17, len(header) + len(payload))
        + header
        + payload
    )
    if value == 0:
        value = 0xFFFF
    return header[:6] + struct.pack("!H", value) + payload


def tcp_segment(
    payload: bytes,
    *,
    options: bytes = b"",
    src=OLD_SRC_IP,
    dst=OLD_DST_IP,
) -> bytes:
    assert len(options) % 4 == 0
    header_length = 20 + len(options)
    assert header_length <= 60
    header = (
        struct.pack(
            "!HHIIHHHH",
            23456,
            11211,
            0x12345678,
            0,
            ((header_length // 4) << 12) | 0x18,
            4096,
            0,
            0,
        )
        + options
    )
    value = checksum(
        pseudo_header(src, dst, 6, len(header) + len(payload))
        + header
        + payload
    )
    return header[:16] + struct.pack("!H", value) + header[18:] + payload


def ipv4_packet(
    payload: bytes,
    protocol: int,
    *,
    identification: int,
    fragment: int = 0,
    src=OLD_SRC_IP,
    dst=OLD_DST_IP,
) -> bytes:
    header = struct.pack(
        "!BBHHHBBH4s4s",
        0x45,
        0,
        20 + len(payload),
        identification,
        fragment,
        64,
        protocol,
        0,
        src,
        dst,
    )
    value = checksum(header)
    return header[:10] + struct.pack("!H", value) + header[12:] + payload


def ethernet(payload: bytes, ether_type: int) -> bytes:
    return OLD_DST_MAC + OLD_SRC_MAC + struct.pack("!H", ether_type) + payload


def write_pcap(path: Path, records) -> None:
    with path.open("wb") as output:
        output.write(
            struct.pack(
                "<IHHIIII",
                0xA1B2C3D4,
                2,
                4,
                0,
                0,
                65535,
                1,
            )
        )
        for index, record in enumerate(records):
            if isinstance(record, tuple):
                data, wire_length = record
            else:
                data = record
                wire_length = len(data)
            output.write(
                struct.pack(
                    "<IIII",
                    index,
                    index,
                    len(data),
                    wire_length,
                )
            )
            output.write(data)


def read_pcap_records(path: Path):
    data = path.read_bytes()
    assert len(data) >= 24, f"short output PCAP: {path}"
    magic, major, minor, _, _, _, linktype = struct.unpack_from(
        "<IHHIIII", data
    )
    assert magic == 0xA1B2C3D4
    assert (major, minor, linktype) == (2, 4, 1)
    records = []
    offset = 24
    while offset < len(data):
        assert offset + 16 <= len(data)
        seconds, microseconds, captured, wire_length = struct.unpack_from(
            "<IIII", data, offset
        )
        offset += 16
        assert captured == wire_length
        assert offset + captured <= len(data)
        timestamp = seconds * 1_000_000 + microseconds
        records.append((timestamp, data[offset : offset + captured]))
        offset += captured
    return records


def read_pcap(path: Path):
    return [frame for _, frame in read_pcap_records(path)]


def check_mac(frame: bytes) -> None:
    assert frame[:6] == NEW_DST_MAC
    assert frame[6:12] == NEW_SRC_MAC


def ipv4_parts(frame: bytes):
    assert frame[12:14] == b"\x08\x00"
    ip = frame[ETH_HEADER:]
    header_length = (ip[0] & 0xF) * 4
    total_length = struct.unpack_from("!H", ip, 2)[0]
    assert ip[0] >> 4 == 4
    assert total_length <= len(ip)
    header = ip[:header_length]
    assert checksum(header) == 0
    assert header[12:16] == NEW_SRC_IP
    assert header[16:20] == NEW_DST_IP
    return header, ip[header_length:total_length]


def verify_transport(frame: bytes, protocol: int) -> None:
    header, transport = ipv4_parts(frame)
    assert header[9] == protocol
    assert checksum(
        pseudo_header(NEW_SRC_IP, NEW_DST_IP, protocol, len(transport))
        + transport
    ) == 0


def verify_udp(frame: bytes, surplus: bytes = b"") -> None:
    header, transport = ipv4_parts(frame)
    assert header[9] == 17
    udp_length = struct.unpack_from("!H", transport, 4)[0]
    assert checksum(
        pseudo_header(NEW_SRC_IP, NEW_DST_IP, 17, udp_length)
        + transport[:udp_length]
    ) == 0
    assert transport[udp_length:] == surplus


def marker_frame(marker: bytes) -> bytes:
    payload = marker + bytes(46 - len(marker))
    return ethernet(payload, 0x88B5)


class PcapReplayTests:
    def __init__(self, gem5: Path, source: Path, work: Path):
        self.gem5 = gem5
        self.config = (
            source
            / "tests/gem5/cachelens/configs/load_generator_pcap_replay.py"
        )
        self.work = work
        self.case_number = 0

    def run(
        self,
        name: str,
        *arguments: str,
        expect_failure=False,
        gem5_arguments=(),
    ):
        self.case_number += 1
        output_dir = self.work / f"{self.case_number:02d}-{name}"
        output_dir.mkdir()
        command = [
            str(self.gem5),
            "-d",
            str(output_dir),
            *map(str, gem5_arguments),
            str(self.config),
            *map(str, arguments),
        ]
        result = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if expect_failure:
            assert result.returncode != 0, result.stdout
        else:
            assert result.returncode == 0, result.stdout
        return output_dir, result.stdout

    def replay_tests(self) -> None:
        large_udp = udp_datagram(
            bytes((index & 0xFF) for index in range(1472))
        )
        frame_1514 = ethernet(
            ipv4_packet(large_udp, 17, identification=1), 0x0800
        )
        assert len(frame_1514) == 1514

        short_udp = udp_datagram(b"pad!")
        short_unpadded = ethernet(
            ipv4_packet(short_udp, 17, identification=2), 0x0800
        )
        assert len(short_unpadded) == 46
        short_padded = short_unpadded + bytes(60 - len(short_unpadded))

        udp_surplus = b"surplus-after-udp"
        udp_with_surplus = ethernet(
            ipv4_packet(
                udp_datagram(b"checksum-only-udp") + udp_surplus,
                17,
                identification=3,
            ),
            0x0800,
        )

        malformed_ipv4 = ethernet(b"\x45" + bytes(12), 0x0800)
        truncated = (marker_frame(b"truncated")[:20], 60)
        kernel_pcap = self.work / "kernel.pcap"
        write_pcap(
            kernel_pcap,
            [
                frame_1514,
                short_padded,
                udp_with_surplus,
                malformed_ipv4,
                truncated,
            ],
        )
        kernel_dump = self.work / "kernel-out.pcap"
        self.run(
            "kernel",
            "--pcap",
            kernel_pcap,
            "--dump",
            kernel_dump,
            "--stack-mode",
            "KernelStack",
        )
        kernel_frames = read_pcap(kernel_dump)
        assert [len(frame) for frame in kernel_frames] == [
            1514,
            46,
            len(udp_with_surplus),
        ]
        for frame in kernel_frames:
            check_mac(frame)
        verify_udp(kernel_frames[0])
        verify_udp(kernel_frames[1])
        verify_udp(kernel_frames[2], udp_surplus)

        limited_dump = self.work / "kernel-limited-out.pcap"
        self.run(
            "kernel-limited",
            "--pcap",
            kernel_pcap,
            "--dump",
            limited_dump,
            "--stack-mode",
            "KernelStack",
            "--max-packet-size",
            "1500",
        )
        assert read_pcap(limited_dump) == kernel_frames[1:]

        arp_payload = struct.pack(
            "!HHBBH6s4s6s4s",
            1,
            0x0800,
            6,
            4,
            1,
            OLD_SRC_MAC,
            OLD_SRC_IP,
            bytes(6),
            OLD_DST_IP,
        )
        arp = ethernet(arp_payload + bytes(18), 0x0806)
        ipv6_header = struct.pack(
            "!IHBB16s16s",
            6 << 28,
            8,
            59,
            64,
            ipaddress.IPv6Address("2001:db8::1").packed,
            ipaddress.IPv6Address("2001:db8::2").packed,
        )
        ipv6 = ethernet(ipv6_header + b"ipv6data", 0x86DD)
        vlan = ethernet(
            struct.pack("!HH", 7, 0x0800)
            + ipv4_packet(udp_datagram(b"inner"), 17, identification=4),
            0x8100,
        )
        non_ipv4 = marker_frame(b"non-ipv4")
        udp = ethernet(
            ipv4_packet(
                udp_datagram(b"udp-checksum"), 17, identification=5
            ),
            0x0800,
        )
        tcp = ethernet(
            ipv4_packet(
                tcp_segment(b"tcp-checksum"), 6, identification=6
            ),
            0x0800,
        )
        fragmented_udp = udp_datagram(bytes(range(32)))
        first_payload = fragmented_udp[:16]
        second_payload = fragmented_udp[16:]
        first_fragment = ethernet(
            ipv4_packet(
                first_payload,
                17,
                identification=7,
                fragment=0x2000,
            ),
            0x0800,
        )
        second_fragment = ethernet(
            ipv4_packet(
                second_payload,
                17,
                identification=7,
                fragment=len(first_payload) // 8,
            ),
            0x0800,
        )

        fragmented_tcp = tcp_segment(
            b"fragmented-tcp-payload", options=b"\x01" * 12
        )
        tcp_first_payload = fragmented_tcp[:24]
        tcp_second_payload = fragmented_tcp[24:]
        tcp_first_fragment = ethernet(
            ipv4_packet(
                tcp_first_payload,
                6,
                identification=8,
                fragment=0x2000,
            ),
            0x0800,
        )
        tcp_second_fragment = ethernet(
            ipv4_packet(
                tcp_second_payload,
                6,
                identification=8,
                fragment=len(tcp_first_payload) // 8,
            ),
            0x0800,
        )
        incomplete_tcp_options = ethernet(
            ipv4_packet(
                tcp_first_payload,
                6,
                identification=9,
            ),
            0x0800,
        )

        dpdk_inputs = [
            arp,
            ipv6,
            vlan,
            non_ipv4,
            udp,
            udp_with_surplus,
            tcp,
            first_fragment,
            second_fragment,
            tcp_first_fragment,
            tcp_second_fragment,
        ]
        dpdk_pcap = self.work / "dpdk.pcap"
        write_pcap(
            dpdk_pcap,
            dpdk_inputs
            + [incomplete_tcp_options, malformed_ipv4, truncated],
        )
        dpdk_dump = self.work / "dpdk-out.pcap"
        self.run(
            "dpdk",
            "--pcap",
            dpdk_pcap,
            "--dump",
            dpdk_dump,
            "--stack-mode",
            "DPDKStack",
        )
        dpdk_frames = read_pcap(dpdk_dump)
        assert len(dpdk_frames) == len(dpdk_inputs)
        for original, replayed in zip(dpdk_inputs[:4], dpdk_frames[:4]):
            check_mac(replayed)
            assert replayed[12:] == original[12:]
        verify_udp(dpdk_frames[4])
        verify_udp(dpdk_frames[5], udp_surplus)
        verify_transport(dpdk_frames[6], 6)

        first_ip, replayed_first = ipv4_parts(dpdk_frames[7])
        second_ip, replayed_second = ipv4_parts(dpdk_frames[8])
        assert struct.unpack_from("!H", first_ip, 6)[0] == 0x2000
        assert struct.unpack_from("!H", second_ip, 6)[0] == 2
        reassembled = replayed_first + replayed_second
        assert checksum(
            pseudo_header(NEW_SRC_IP, NEW_DST_IP, 17, len(reassembled))
            + reassembled
        ) == 0

        tcp_first_ip, replayed_tcp_first = ipv4_parts(dpdk_frames[9])
        tcp_second_ip, replayed_tcp_second = ipv4_parts(dpdk_frames[10])
        assert struct.unpack_from("!H", tcp_first_ip, 6)[0] == 0x2000
        assert struct.unpack_from("!H", tcp_second_ip, 6)[0] == 3
        assert len(replayed_tcp_first) == 24
        assert (replayed_tcp_first[12] >> 4) * 4 == 32
        reassembled_tcp = replayed_tcp_first + replayed_tcp_second
        assert checksum(
            pseudo_header(NEW_SRC_IP, NEW_DST_IP, 6, len(reassembled_tcp))
            + reassembled_tcp
        ) == 0

    def pacing_tests(self) -> None:
        frames = [
            marker_frame(b"paced-1"),
            marker_frame(b"paced-2"),
            marker_frame(b"paced-3"),
        ]
        pcap = self.work / "pacing.pcap"
        write_pcap(pcap, frames)

        pacing_dump = self.work / "pacing-out.pcap"
        self.run(
            "pacing",
            "--pcap",
            pcap,
            "--dump",
            pacing_dump,
            "--packet-rate",
            "100000",
            "--link-speed",
            "100Gbps",
        )
        pacing_records = read_pcap_records(pacing_dump)
        assert [frame[12:] for _, frame in pacing_records] == [
            frame[12:] for frame in frames
        ]
        pacing_timestamps = [timestamp for timestamp, _ in pacing_records]
        assert [
            later - earlier
            for earlier, later in zip(
                pacing_timestamps, pacing_timestamps[1:]
            )
        ] == [10, 10]

        backpressure_dump = self.work / "backpressure-out.pcap"
        _, output = self.run(
            "backpressure",
            "--pcap",
            pcap,
            "--dump",
            backpressure_dump,
            "--packet-rate",
            "1000000",
            "--link-speed",
            "10Mbps",
            gem5_arguments=("--debug-flags=LoadgenDebug",),
        )
        backpressure_records = read_pcap_records(backpressure_dump)
        assert [frame[12:] for _, frame in backpressure_records] == [
            frame[12:] for frame in frames
        ]
        backpressure_timestamps = [
            timestamp for timestamp, _ in backpressure_records
        ]
        backpressure_spacing = [
            later - earlier
            for earlier, later in zip(
                backpressure_timestamps, backpressure_timestamps[1:]
            )
        ]
        assert backpressure_spacing == [48, 48]
        assert all(spacing > 1 for spacing in backpressure_spacing)
        assert "Peer rejected packet" in output

    def checkpoint_tests(self) -> None:
        frames_a = [
            marker_frame(b"active-1"),
            marker_frame(b"active-2"),
            marker_frame(b"active-3"),
        ]
        frames_b = [marker_frame(b"different-file")]
        pcap_a = self.work / "checkpoint-a.pcap"
        pcap_b = self.work / "checkpoint-b.pcap"
        write_pcap(pcap_a, frames_a)
        write_pcap(pcap_b, frames_b)

        dormant_checkpoint = self.work / "dormant.cpt"
        self.run(
            "dormant-save",
            "--pcap",
            pcap_a,
            "--dump",
            self.work / "dormant-save.pcap",
            "--start-tick",
            "0",
            "--stop-tick",
            "0",
            "--ticks",
            "1",
            "--checkpoint",
            dormant_checkpoint,
        )
        dormant_dump = self.work / "dormant-restore.pcap"
        self.run(
            "dormant-restore",
            "--pcap",
            pcap_b,
            "--dump",
            dormant_dump,
            "--restore",
            dormant_checkpoint,
        )
        dormant_frames = read_pcap(dormant_dump)
        assert len(dormant_frames) == 1
        check_mac(dormant_frames[0])
        assert dormant_frames[0][12:] == frames_b[0][12:]

        active_checkpoint = self.work / "active.cpt"
        self.run(
            "active-save",
            "--pcap",
            pcap_a,
            "--dump",
            self.work / "active-save.pcap",
            "--ticks",
            "1000000",
            "--checkpoint",
            active_checkpoint,
        )
        active_dump = self.work / "active-restore.pcap"
        self.run(
            "active-restore",
            "--pcap",
            pcap_a,
            "--dump",
            active_dump,
            "--ticks",
            "40000000",
            "--restore",
            active_checkpoint,
        )
        active_frames = read_pcap(active_dump)
        assert len(active_frames) == 2
        assert [frame[14:22] for frame in active_frames] == [
            b"active-2",
            b"active-3",
        ]

        _, output = self.run(
            "active-changed-file",
            "--pcap",
            pcap_b,
            "--dump",
            self.work / "active-changed-file.pcap",
            "--restore",
            active_checkpoint,
            expect_failure=True,
        )
        assert "changed since the checkpoint" in output


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path(__file__).resolve().parents[3],
    )
    args = parser.parse_args()
    assert args.gem5.is_file(), args.gem5

    with tempfile.TemporaryDirectory(prefix="gem5-pcap-tests-") as tempdir:
        tests = PcapReplayTests(
            args.gem5.resolve(), args.source.resolve(), Path(tempdir)
        )
        tests.replay_tests()
        tests.pacing_tests()
        tests.checkpoint_tests()
    print("PCAP_REPLAY_TESTS_OK")


if __name__ == "__main__":
    main()
