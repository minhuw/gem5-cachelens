# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import unittest

from m5.objects import (
    AddrRange,
    NoncoherentXBar,
)
from m5.util.convert import toMemorySize

from gem5.components.memory import (
    DualChannelDDR4_2400,
    HBM2Stack,
)


class ChanneledMemoryRangeTestSuite(unittest.TestCase):
    def test_single_range_preserves_channel_count(self) -> None:
        memory = DualChannelDDR4_2400(size="3GiB")
        memory.set_memory_range([AddrRange("3GiB")])

        self.assertEqual(2, len(memory.get_memory_controllers()))
        self.assertEqual(2, len(memory.get_mem_ports()))
        self.assertEqual(1, len(memory.get_uninterleaved_range()))

    def test_split_eight_gibibytes(self) -> None:
        memory = DualChannelDDR4_2400(size="8GiB")
        ranges = [
            AddrRange("3GiB"),
            AddrRange("4GiB", size="5GiB"),
        ]
        memory.set_memory_range(ranges)

        controllers = memory.get_memory_controllers()
        ports = memory.get_mem_ports()
        self.assertEqual(2, len(controllers))
        # There is one routing port per disjoint guest range. The physical
        # controller group remains two interleaved channels downstream.
        self.assertEqual(2, len(ports))
        self.assertEqual(ranges, memory.get_uninterleaved_range())
        self.assertEqual(toMemorySize("8GiB"), memory.get_size())

        self.assertEqual(1, len(memory._range_mapper_buses))
        bus = memory._range_mapper_buses[0]
        self.assertEqual(0, int(bus.frontend_latency))
        self.assertEqual(0, int(bus.forward_latency))
        self.assertEqual(0, int(bus.response_latency))
        self.assertEqual(0, int(bus.header_latency))
        self.assertEqual(64, int(bus.width))
        self.assertTrue(bool(bus.timing_transparent))
        self.assertEqual(2, len(memory._range_mappers))
        self.assertEqual(
            ranges, [m.original_ranges[0] for m in memory._range_mappers]
        )
        self.assertEqual(
            [
                (0, toMemorySize("3GiB")),
                (toMemorySize("3GiB"), toMemorySize("5GiB")),
            ],
            [
                (
                    int(m.remapped_ranges[0].start),
                    int(m.remapped_ranges[0].size()),
                )
                for m in memory._range_mappers
            ],
        )

        # Both physical channels cover the packed 8 GiB space and retain
        # their channel interleave matches; the guest high range is not
        # accidentally installed as a second physical controller group.
        physical_ranges = [ctrl.dram.range for ctrl in controllers]
        self.assertEqual({0, 1}, {rng.intlvMatch for rng in physical_ranges})
        self.assertTrue(all(int(rng.start) == 0 for rng in physical_ranges))
        self.assertTrue(
            all(rng.size() == toMemorySize("4GiB") for rng in physical_ranges)
        )
        guest_route_ranges = [rng for rng, _ in ports]
        self.assertEqual(
            [
                (0, toMemorySize("3GiB")),
                (toMemorySize("4GiB"), toMemorySize("9GiB")),
            ],
            [(int(rng.start), int(rng.end)) for rng in guest_route_ranges],
        )

    def test_invalid_ranges(self) -> None:
        memory = DualChannelDDR4_2400(size="8GiB")

        with self.assertRaisesRegex(ValueError, "must not overlap"):
            memory.set_memory_range(
                [
                    AddrRange("4GiB"),
                    AddrRange("3GiB", size="4GiB"),
                ]
            )

        with self.assertRaisesRegex(ValueError, "must be sorted"):
            memory.set_memory_range(
                [
                    AddrRange("4GiB", size="5GiB"),
                    AddrRange("3GiB"),
                ]
            )

        with self.assertRaisesRegex(ValueError, "summed memory range size"):
            memory.set_memory_range([AddrRange("3GiB")])

        with self.assertRaisesRegex(ValueError, "must not be empty"):
            memory.set_memory_range(
                [AddrRange(0), AddrRange("4GiB", size="8GiB")]
            )

    def test_hbm_rejects_multiple_ranges(self) -> None:
        memory = HBM2Stack(size="8GiB")
        with self.assertRaisesRegex(ValueError, "does not support multiple"):
            memory.set_memory_range(
                [
                    AddrRange("3GiB"),
                    AddrRange("4GiB", size="5GiB"),
                ]
            )

    def test_repeated_range_configuration_clears_mappers(self) -> None:
        memory = DualChannelDDR4_2400(size="8GiB")
        memory.set_memory_range(
            [AddrRange("3GiB"), AddrRange("4GiB", size="5GiB")]
        )
        memory.set_memory_range([AddrRange("8GiB")])

        self.assertEqual(2, len(memory.get_memory_controllers()))
        self.assertEqual(2, len(memory.get_mem_ports()))
        self.assertEqual([], memory._range_mappers)
        self.assertEqual([], memory._range_mapper_buses)
        self.assertNotIn("range_mappers", memory._children)
        self.assertNotIn("range_mapper_buses", memory._children)
        self.assertEqual([], getattr(memory, "extra_mem_ctrl", []))
        self.assertEqual(0, int(memory.mem_ctrl[0].dram.range.start))
        self.assertEqual(
            toMemorySize("4GiB"), memory.mem_ctrl[0].dram.range.size()
        )
        self.assertEqual(
            toMemorySize("8GiB"), int(memory.mem_ctrl[0].dram.range.end)
        )

    def test_factory_single_range_compatibility(self) -> None:
        memory = DualChannelDDR4_2400(size="1GiB")
        memory.set_memory_range([AddrRange("1GiB")])
        self.assertEqual(2, len(memory.get_mem_ports()))
        self.assertEqual([], memory._range_mapper_buses)

    def test_noncoherent_xbar_default_is_not_transparent(self) -> None:
        bus = NoncoherentXBar(
            frontend_latency=0,
            forward_latency=0,
            response_latency=0,
            width=64,
        )
        self.assertFalse(bool(bus.timing_transparent))


if __name__ == "__main__":
    unittest.main()
