# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause
"""Run with python3 tests/pyunit/mesi_ddio/pyunit_three_level_smoke.py."""

import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("ddio_smoke", ROOT / "verification/ddio-smoke.py")
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)
from three_level_checks import check_output, check_stats, expected_stats


class ThreeLevelSmokeTest(unittest.TestCase):
    def test_exact_stats_missing_wrong_and_zero_allocations(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stats.txt"
            for scenario in smoke._EXPECTED:
                expected = expected_stats(scenario)
                text = "\n".join(f"{key} {value}" for key, value in expected.items())
                path.write_text(text)
                check_stats(path, scenario)
                path.write_text("")
                with self.assertRaisesRegex(ValueError, "missing"):
                    check_stats(path, scenario)
                key = next(key for key in expected if key.endswith("ddioWayFill::total"))
                path.write_text(text + f"\n{key} {expected[key] + 1}\n")
                with self.assertRaisesRegex(ValueError, "ddioWayFill::total"):
                    check_stats(path, scenario)
            for scenario in ("noneligible_dma", "disabled_ddio", "zero_mask", "tx_miss_no_allocate"):
                self.assertEqual(smoke._EXPECTED[scenario]["ddioAllocWays::total"], 0)

    def test_all_completion_markers_required(self):
        markers = [
            "CacheLens native three-level hierarchy assertions passed",
            "MESI DDIO directed scenario 'disabled_ddio' passed",
            "Exiting @ tick 100 because MESI DDIO directed scenario completed",
        ]
        check_output("\n".join(markers), "disabled_ddio")
        for index in range(len(markers)):
            with self.assertRaises(ValueError):
                check_output("\n".join(markers[:index] + markers[index + 1:]), "disabled_ddio")

    def test_process_failure_timeout_and_false_success(self):
        for failure in (subprocess.CalledProcessError(3, ["gem5"]),
                        subprocess.TimeoutExpired(["gem5"], 1), OSError("not executable"), None):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                with patch.object(smoke.subprocess, "run", side_effect=failure) as run:
                    result = smoke.run_case(Path("gem5"), Path(directory), "disabled_ddio", 1)
                self.assertFalse(result["passed"])
                self.assertIn("error", result)
                self.assertTrue(run.call_args.kwargs["check"])
                self.assertEqual(run.call_args.kwargs["timeout"], 1)
                self.assertIn("--ddio-way-part=-1", result["command"])
                self.assertTrue((Path(directory) / "disabled_ddio/run.log").exists())

    def test_refuse_existing_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "keep"
            marker.write_text("evidence")
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                smoke.main(["--gem5", __file__, "--output", directory])
            self.assertEqual(error.exception.code, 2)
            self.assertEqual(marker.read_text(), "evidence")


if __name__ == "__main__":
    unittest.main()
