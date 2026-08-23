# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import unittest

from m5.objects import RubySystem

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.cachehierarchies.ruby.mesi_two_level_cache_hierarchy import (
    MESITwoLevelCacheHierarchy,
)
from gem5.isas import ISA
from gem5.prebuilt import cachelens
from gem5.prebuilt.cachelens import CacheLensMESITwoLevelHierarchy


class CacheLensMESIHierarchyTestSuite(unittest.TestCase):
    def test_public_export_and_protocol(self) -> None:
        self.assertIn("CacheLensMESITwoLevelHierarchy", cachelens.__all__)
        hierarchy = CacheLensMESITwoLevelHierarchy()
        self.assertEqual(
            CoherenceProtocol.MESI_TWO_LEVEL,
            hierarchy.get_coherence_protocol(),
        )

    def test_private_data_and_llc_mapping_is_explicit(self) -> None:
        hierarchy = CacheLensMESITwoLevelHierarchy(
            l1i_size="16KiB",
            l1i_assoc=2,
            l2_size="512KiB",
            l2_assoc=4,
            hnf_size="2MiB",
            hnf_assoc=8,
            num_hnfs=4,
            ddio_way_part=2,
            model_profile="abstract",
        )
        configuration = hierarchy.get_configuration()

        # The stdlib MESI L1D is deliberately sourced from CacheLens l2_*.
        self.assertEqual("512KiB", hierarchy._l1d_size)
        self.assertEqual(4, hierarchy._l1d_assoc)
        # The stdlib MESI L2 is the shared inclusive LLC/HNF analogue.
        self.assertEqual("2MiB", hierarchy._l2_size)
        self.assertEqual(8, hierarchy._l2_assoc)
        self.assertEqual(4, hierarchy._num_l2_banks)
        self.assertEqual(2, hierarchy._l2_select_num_bits)

        self.assertEqual("MESI_Two_Level", configuration["coherence_protocol"])
        self.assertEqual(
            "CacheLens l2_size/l2_assoc -> MESI_Two_Level L1D",
            configuration["private_data_mapping"],
        )
        self.assertFalse(configuration["topology_matches_chi_three_level"])
        self.assertEqual("inclusive", configuration["hnf_inclusion"])
        self.assertEqual(
            "MESI_Two_Level protocol",
            configuration["llc_inclusion_source"],
        )
        self.assertEqual(4, configuration["num_llc_banks"])
        self.assertEqual(8, configuration["hnf_assoc"])
        self.assertEqual(8 << 20, hierarchy.get_total_hnf_capacity_bytes())
        self.assertEqual("LRU", configuration["llc_replacement_policy"])
        self.assertEqual(2, configuration["ddio_way_part"])
        self.assertEqual(64, configuration["cache_line_size"])
        self.assertEqual(6, configuration["bank_select_low_bit"])
        self.assertEqual(2, configuration["bank_select_bits"])
        self.assertTrue(configuration["nic_read_no_allocate"])
        self.assertEqual("cold", configuration["cache_state_restore_policy"])
        self.assertFalse(configuration["cache_state_continuity"])

    def test_defaults_are_bounded(self) -> None:
        hierarchy = CacheLensMESITwoLevelHierarchy()
        configuration = hierarchy.get_configuration()
        self.assertEqual(1, configuration["num_llc_banks"])
        self.assertEqual("1MiB", configuration["hnf_size_per_hnf"])
        self.assertEqual(8, configuration["hnf_assoc"])
        self.assertEqual(-1, configuration["ddio_way_part"])
        self.assertEqual("TreePLRU", configuration["llc_replacement_policy"])
        self.assertEqual("linear", hierarchy.get_indexing_policy())
        self.assertEqual("linear", configuration["indexing_policy"])
        self.assertFalse(hierarchy._l2_addr_hash)

        experimental = CacheLensMESITwoLevelHierarchy(
            indexing_policy="splitmix64"
        )
        self.assertEqual("splitmix64", experimental.get_indexing_policy())
        self.assertEqual(
            "splitmix64", experimental.get_configuration()["indexing_policy"]
        )
        self.assertTrue(experimental._l2_addr_hash)

        ddio = CacheLensMESITwoLevelHierarchy(model_profile="intel-ddio")
        self.assertEqual(2, ddio.get_configuration()["ddio_way_part"])

    def test_validation(self) -> None:
        for banks in (1, 2, 8):
            CacheLensMESITwoLevelHierarchy(num_hnfs=banks)
        for banks in (0, 3, 6):
            with self.subTest(banks=banks):
                with self.assertRaisesRegex(ValueError, "power of two"):
                    CacheLensMESITwoLevelHierarchy(num_hnfs=banks)

        for ways in (0, 9):
            with self.subTest(ways=ways):
                with self.assertRaisesRegex(ValueError, "ddio_way_part"):
                    CacheLensMESITwoLevelHierarchy(
                        hnf_assoc=8, ddio_way_part=ways
                    )
        with self.assertRaisesRegex(ValueError, "Positive ddio_way_part"):
            CacheLensMESITwoLevelHierarchy(
                ddio_way_part=2, model_profile="x86-generic"
            )
        with self.assertRaisesRegex(ValueError, "power-of-two number"):
            CacheLensMESITwoLevelHierarchy(hnf_size="192KiB", hnf_assoc=8)
        with self.assertRaisesRegex(ValueError, "x86-only"):
            CacheLensMESITwoLevelHierarchy(model_profile="arm-generic")
        with self.assertRaisesRegex(ValueError, "indexing_policy"):
            CacheLensMESITwoLevelHierarchy(indexing_policy="xor")

    def test_stock_mesi_shared_l2_hash_is_opt_in(self) -> None:
        common = {
            "l1i_size": "16KiB",
            "l1i_assoc": 2,
            "l1d_size": "32KiB",
            "l1d_assoc": 4,
            "l2_size": "1MiB",
            "l2_assoc": 8,
            "num_l2_banks": 2,
        }
        self.assertFalse(MESITwoLevelCacheHierarchy(**common)._l2_addr_hash)
        self.assertTrue(
            MESITwoLevelCacheHierarchy(
                **common, l2_addr_hash=True
            )._l2_addr_hash
        )
        with self.assertRaisesRegex(TypeError, "l2_addr_hash"):
            MESITwoLevelCacheHierarchy(**common, l2_addr_hash=1)

    def test_architecture_and_bank_selection(self) -> None:
        hierarchy = CacheLensMESITwoLevelHierarchy(num_hnfs=4)
        hierarchy.validate_architecture(ISA.X86)
        with self.assertRaisesRegex(ValueError, "only on x86"):
            hierarchy.validate_architecture(ISA.ARM)

        selected = [
            hierarchy.hnf_index_for_address(line * 64) for line in range(32)
        ]
        self.assertEqual({0, 1, 2, 3}, set(selected))
        self.assertEqual([line % 4 for line in range(32)], selected)

    def test_cachelens_restore_does_not_change_stock_ruby_default(
        self,
    ) -> None:
        self.assertTrue(bool(RubySystem().cache_trace_warmup))


if __name__ == "__main__":
    unittest.main()
