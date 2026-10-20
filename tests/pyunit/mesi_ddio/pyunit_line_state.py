# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause
"""Failure tests for the checker used by both native protocol fixtures."""

import importlib.util
import unittest
from pathlib import Path
from unittest.mock import Mock, patch


_ROOT = Path(__file__).resolve().parents[3]
_SPEC = importlib.util.spec_from_file_location(
    "line_state_checks", _ROOT / "tests/gem5/mesi_ddio/line_state_checks.py"
)
_CHECKS = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_CHECKS)


def fixture_stats(expected, prefix=""):
    stats = {}
    for direction in ("Read", "Write"):
        stem = prefix + "dma" + direction
        tag = expected[direction, "TagPresence"]
        stats[stem + "Hits"] = tag.get("present", 0)
        stats[stem + "Misses"] = tag.get("absent", 0)
        stats[stem + "Requests"] = sum(tag.values())
        for dimension, labels in _CHECKS.DIMENSIONS.items():
            values = expected[direction, dimension]
            for label in labels:
                stats[stem + dimension + "::" + label] = values.get(label, 0)
            stats[stem + dimension + "::total"] = sum(values.values())
    return stats


class LineStateChecksTest(unittest.TestCase):
    def setUp(self):
        self.expected = _CHECKS.expected_marginals(
            read=[("CHI_RU", False, False, True)],
            write=[("CHI_UD_RU", True, True, True)],
        )
        self.stats = fixture_stats(self.expected)

    def check(self, stats):
        _CHECKS.check_marginals(stats, "", "CHI", self.expected)

    def test_directory_only_owner_and_stale_resident_dirty_are_independent(self):
        self.check(self.stats)

    def test_balanced_wrong_owner_fails_exact_expectation(self):
        self.stats["dmaReadPrivateOwner::present"] = 0
        self.stats["dmaReadPrivateOwner::absent"] = 1
        with self.assertRaisesRegex(ValueError, "dmaReadPrivateOwner: expected"):
            self.check(self.stats)

    def test_balanced_wrong_native_state_fails_exact_expectation(self):
        self.stats["dmaWriteState::CHI_UD_RU"] = 0
        self.stats["dmaWriteState::CHI_UD"] = 1
        with self.assertRaisesRegex(ValueError, "dmaWriteState: expected"):
            self.check(self.stats)

    def test_inactive_protocol_bucket_fails_without_exact_expectations(self):
        self.stats["dmaReadState::CHI_RU"] = 0
        self.stats["dmaReadState::MESI_MT"] = 1
        with self.assertRaisesRegex(ValueError, "inactive protocol"):
            _CHECKS.check_marginals(self.stats, "", "CHI")

    def test_missing_scalar_is_a_validation_failure(self):
        del self.stats["dmaReadHits"]
        with self.assertRaisesRegex(ValueError, "missing/invalid requests"):
            self.check(self.stats)

    def test_zero_direction_still_requires_all_buckets(self):
        expected = _CHECKS.expected_marginals()
        stats = fixture_stats(expected)
        del stats["dmaReadState::CHI_null"]
        with self.assertRaisesRegex(ValueError, "missing/invalid buckets"):
            _CHECKS.check_marginals(stats, "", "CHI", expected)

    def test_nonfinite_negative_and_fractional_counts_fail(self):
        for invalid in (float("nan"), float("inf"), -1, 0.5):
            with self.subTest(invalid=invalid):
                stats = dict(self.stats, dmaReadRequests=invalid)
                with self.assertRaises(ValueError):
                    self.check(stats)

    def test_bank_errors_cannot_cancel_in_system_aggregate(self):
        stats = fixture_stats(self.expected, "bank0.")
        stats.update(fixture_stats(self.expected, "bank1."))
        # Overall totals still match, but one bank counted the other's lookup.
        stats["bank0.dmaReadState::CHI_RU"] = 0
        stats["bank1.dmaReadState::CHI_RU"] = 2
        for bank in (0, 1):
            with self.subTest(bank=bank), self.assertRaises(ValueError):
                _CHECKS.check_marginals(stats, f"bank{bank}.", "CHI", self.expected)

    def live_stats(self):
        """Match get_simstat's named vectors and unrelated integer keys."""
        cache = {}
        for name, value in self.stats.items():
            if "::" not in name:
                cache[name] = {"value": value}
                continue
            vector, label = name.split("::")
            if label != "total":
                cache.setdefault(vector, {"value": {}})["value"][label] = {
                    "value": value
                }
        # Unnamed vector elements use integers, even in to_json() output.
        cache["wayAccess"] = {"value": {0: {"value": 1}}}
        return {"cache": cache}

    def check_live(self, node):
        get_simstat = Mock(return_value=Mock(to_json=Mock(return_value=node)))
        module = Mock(get_simstat=get_simstat)
        cache = object()
        with patch.dict("sys.modules", {"m5.stats.gem5stats": module}):
            _CHECKS.check_cache_marginals(cache, "CHI", self.expected)
        get_simstat.assert_called_once_with(cache, prepare_stats=True)

    def test_live_adapter_parses_all_37_state_labels_with_integer_keys(self):
        labels = _CHECKS.DIMENSIONS["State"]
        self.assertEqual(len(labels), 37)
        self.assertEqual(len(set(labels)), 37)
        self.check_live(self.live_stats())

    def test_live_adapter_requires_every_state_label_in_both_directions(self):
        for direction in ("Read", "Write"):
            for label in _CHECKS.DIMENSIONS["State"]:
                with self.subTest(direction=direction, label=label):
                    node = self.live_stats()
                    del node["cache"][f"dma{direction}State"]["value"][label]
                    with self.assertRaisesRegex(ValueError, "missing/invalid buckets"):
                        self.check_live(node)


if __name__ == "__main__":
    unittest.main()
