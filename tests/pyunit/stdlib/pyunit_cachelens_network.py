# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import inspect
import unittest

from m5.defines import buildEnv
from m5.objects import IGbE_e1000

from gem5.prebuilt.cachelens.network import build_cachelens_network


class CacheLensNetworkTestSuite(unittest.TestCase):
    def test_simple_network(self) -> None:
        network = build_cachelens_network(num_nics=1, num_loadgens=1)
        self.assertEqual(1, len(network.nics))
        self.assertEqual(1, len(network.loadgens))
        self.assertEqual(1, len(network.links))
        self.assertEqual(48 << 10, int(network.nics[0].rx_fifo_size))
        self.assertEqual(16 << 10, int(network.nics[0].tx_fifo_size))
        self.assertEqual(1_000_000_000, int(network.links[0].speed))

    def test_deterministic_paired_mac_addresses(self) -> None:
        default_nic = IGbE_e1000()
        network = build_cachelens_network(num_nics=3, num_loadgens=3)

        network_addresses = [
            str(nic.hardware_address) for nic in network.nics
        ]
        self.assertEqual(
            [
                "02:90:00:00:00:01",
                "02:90:00:00:00:02",
                "02:90:00:00:00:03",
            ],
            network_addresses,
        )
        self.assertEqual(
            [0, 1, 2],
            [int(loadgen.loadgen_id) for loadgen in network.loadgens],
        )

        default_address = str(
            default_nic.hardware_address.unproxy(default_nic)
        )
        self.assertTrue(default_address.startswith("00:90:"))
        self.assertNotIn(default_address, network_addresses)

    def test_configured_link_and_fifo(self) -> None:
        network = build_cachelens_network(
            num_nics=1,
            num_loadgens=1,
            link_speed="10Gbps",
            link_delay="50us",
            rx_fifo_size="64KiB",
            tx_fifo_size="32KiB",
        )
        self.assertEqual(64 << 10, int(network.nics[0].rx_fifo_size))
        self.assertEqual(32 << 10, int(network.nics[0].tx_fifo_size))
        self.assertEqual(10_000_000_000, int(network.links[0].speed))
        self.assertAlmostEqual(50e-6, float(network.links[0].delay))

    def test_pcap_full_l2_size(self) -> None:
        parameter = inspect.signature(build_cachelens_network).parameters[
            "pcap_max_packet_size"
        ]
        self.assertEqual(1514, parameter.default)

        if buildEnv["HAVE_PCAP"]:
            network = build_cachelens_network(
                num_nics=1,
                num_loadgens=1,
                loadgen_type="Pcap",
                pcap_filename="generated.pcap",
            )
            self.assertEqual(1514, int(network.loadgens[0].max_packetsize))
            with self.assertRaisesRegex(ValueError, "full L2 frame size"):
                build_cachelens_network(
                    num_nics=1,
                    num_loadgens=1,
                    loadgen_type="Pcap",
                    pcap_filename="generated.pcap",
                    pcap_max_packet_size=13,
                )

    def test_validation(self) -> None:
        invalid_options = [
            ({"num_nics": -1}, "non-negative"),
            ({"num_nics": 0, "num_loadgens": 1}, "cannot exceed"),
            ({"packet_rate": 0}, "must be positive"),
            ({"packet_size": 21}, "need 22 bytes"),
            ({"loadgen_start": 2, "loadgen_stop": 1}, "must not exceed"),
            ({"loadgen_type": "Unknown"}, "must be 'Simple' or 'Pcap'"),
            (
                {"loadgen_mode": "Increment", "check_loss_wait": "100us"},
                "cover the modeled RTT",
            ),
        ]
        for options, message in invalid_options:
            with self.subTest(options=options):
                with self.assertRaisesRegex(ValueError, message):
                    build_cachelens_network(**options)


if __name__ == "__main__":
    unittest.main()
