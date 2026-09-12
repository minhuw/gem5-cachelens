# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import unittest

from m5.objects import RubySystem

from gem5.isas import ISA
from gem5.prebuilt.cachelens.cache_hierarchy import CacheLensCHIHierarchy
from gem5.prebuilt.cachelens.topology import SimpleCrossbar


class CacheLensHierarchyTestSuite(unittest.TestCase):
    def test_ddio_partition_validation(self) -> None:
        CacheLensCHIHierarchy(ddio_way_part=-1)
        CacheLensCHIHierarchy(
            hnf_assoc=8, ddio_way_part=8, model_profile="abstract"
        )
        with self.assertRaisesRegex(ValueError, "ddio_way_part"):
            CacheLensCHIHierarchy(hnf_assoc=8, ddio_way_part=0)
        with self.assertRaisesRegex(ValueError, "ddio_way_part"):
            CacheLensCHIHierarchy(hnf_assoc=8, ddio_way_part=9)
        for profile in ("arm-generic", "x86-generic"):
            with self.assertRaisesRegex(ValueError, "Positive ddio_way_part"):
                CacheLensCHIHierarchy(
                    ddio_way_part=2, model_profile=profile
                )

    def test_hnf_count_validation(self) -> None:
        CacheLensCHIHierarchy(num_hnfs=1)
        CacheLensCHIHierarchy(num_hnfs=4)
        with self.assertRaisesRegex(ValueError, "power of two"):
            CacheLensCHIHierarchy(num_hnfs=0)
        with self.assertRaisesRegex(ValueError, "power of two"):
            CacheLensCHIHierarchy(num_hnfs=3)

    def test_hnf_inclusion_validation_and_reporting(self) -> None:
        default = CacheLensCHIHierarchy()
        self.assertEqual(
            "noninclusive", default.get_configuration()["hnf_inclusion"]
        )
        for mode in ("noninclusive", "inclusive"):
            hierarchy = CacheLensCHIHierarchy(hnf_inclusion=mode)
            self.assertEqual(
                mode, hierarchy.get_configuration()["hnf_inclusion"]
            )
        for mode in ("", "non-inclusive", "strict", "Inclusive"):
            with self.subTest(mode=mode):
                with self.assertRaisesRegex(ValueError, "hnf_inclusion"):
                    CacheLensCHIHierarchy(hnf_inclusion=mode)

    def test_default_model_is_bounded_and_explicit(self) -> None:
        hierarchy = CacheLensCHIHierarchy(num_hnfs=4, hnf_size="2MiB")
        configuration = hierarchy.get_configuration()
        self.assertEqual("abstract", hierarchy.get_model_profile())
        self.assertEqual("linear", hierarchy.get_indexing_policy())
        self.assertEqual("noninclusive", configuration["hnf_inclusion"])
        self.assertEqual(-1, configuration["ddio_way_part"])
        self.assertEqual(8 << 20, hierarchy.get_total_hnf_capacity_bytes())
        self.assertEqual("cold", configuration["cache_state_restore_policy"])
        self.assertFalse(configuration["cache_state_continuity"])

        experimental = CacheLensCHIHierarchy(indexing_policy="splitmix64")
        self.assertEqual("splitmix64", experimental.get_indexing_policy())
        self.assertTrue(experimental._addr_hash)

    def test_legacy_hash_spelling_is_explicit(self) -> None:
        self.assertEqual(
            "splitmix64",
            CacheLensCHIHierarchy(addr_hash=True).get_indexing_policy(),
        )
        with self.assertRaisesRegex(ValueError, "different policies"):
            CacheLensCHIHierarchy(addr_hash=True, indexing_policy="linear")

    def test_profile_architecture_validation(self) -> None:
        hierarchy = CacheLensCHIHierarchy(model_profile="intel-ddio")
        self.assertTrue(
            hierarchy.get_configuration()["nic_read_no_allocate"]
        )
        with self.assertRaisesRegex(ValueError, "only valid for x86"):
            hierarchy.validate_architecture(ISA.ARM)
        hierarchy.validate_architecture(ISA.X86)
        with self.assertRaisesRegex(ValueError, "requires ARM"):
            CacheLensCHIHierarchy(
                model_profile="arm-generic"
            ).validate_architecture(ISA.X86)

        for profile in ("abstract", "arm-generic", "x86-generic"):
            configuration = CacheLensCHIHierarchy(
                model_profile=profile
            ).get_configuration()
            self.assertFalse(configuration["nic_read_no_allocate"])

    def test_hnf_partition_selects_exactly_one_node(self) -> None:
        hierarchy = CacheLensCHIHierarchy(num_hnfs=4)
        selected = [
            hierarchy.hnf_index_for_address(line * 64) for line in range(32)
        ]
        self.assertEqual(set(range(4)), set(selected))
        for line in range(32):
            self.assertEqual(
                1,
                sum(index == selected[line] for index in range(4)),
            )

    def test_generic_ruby_restore_keeps_warm_replay_default(self) -> None:
        self.assertTrue(bool(RubySystem().cache_trace_warmup))

    def test_crossbar_defaults_are_explicit(self) -> None:
        network = SimpleCrossbar(
            RubySystem(), link_latency=2, router_latency=3, buffer_size=7
        )
        self.assertEqual(2, network._link_latency)
        self.assertEqual(3, network._router_latency)
        self.assertEqual(7, int(network.buffer_size))


if __name__ == "__main__":
    unittest.main()
