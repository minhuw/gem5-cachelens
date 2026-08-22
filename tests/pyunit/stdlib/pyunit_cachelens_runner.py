# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import contextlib
import importlib.util
import io
import unittest
from pathlib import Path

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.isas import ISA
from gem5.runtime import get_supported_protocols

_REPOSITORY = Path(__file__).resolve().parents[3]
_RUNNER_PATH = _REPOSITORY / "configs/example/gem5_library/cachelens-fs.py"
_SPEC = importlib.util.spec_from_file_location("cachelens_fs", _RUNNER_PATH)
_RUNNER = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_RUNNER)


class _BoardStub:
    def get_nic_bdfs(self):
        return ["0000:00:02.0"]


class CacheLensRunnerTestSuite(unittest.TestCase):
    def _parse(self, *extra):
        return _RUNNER._create_parser().parse_args(
            [
                "--isa",
                "x86",
                "--kernel",
                str(_RUNNER_PATH),
                "--disk-image",
                str(_RUNNER_PATH),
                *extra,
            ]
        )

    def test_chi_remains_the_default(self) -> None:
        parser = _RUNNER._create_parser()
        args = self._parse()
        self.assertEqual("chi", args.coherence_protocol)
        self.assertIsNone(args.hnf_inclusion)
        _RUNNER._validate_args(parser, args)
        self.assertEqual(
            CoherenceProtocol.CHI, _RUNNER._selected_protocol(args)
        )

    def test_mesi_options_report_protocol_inclusion(self) -> None:
        parser = _RUNNER._create_parser()
        args = self._parse(
            "--coherence-protocol",
            "mesi-two-level",
            "--hnf-inclusion",
            "inclusive",
        )
        _RUNNER._validate_args(parser, args)
        self.assertEqual(
            CoherenceProtocol.MESI_TWO_LEVEL,
            _RUNNER._selected_protocol(args),
        )

        for invalid in (
            ("--hnf-inclusion", "noninclusive"),
            ("--indexing-policy", "splitmix64"),
            ("--dealloc-on-unique",),
            ("--l1d-size", "128KiB"),
        ):
            with self.subTest(invalid=invalid):
                invalid_args = self._parse(
                    "--coherence-protocol", "mesi-two-level", *invalid
                )
                with contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        _RUNNER._validate_args(parser, invalid_args)

    def test_atomic_preparation_stays_uncached_for_both_selections(
        self,
    ) -> None:
        expected_mappings = {
            "chi": (
                "timing restore will map l1d_* to private CHI L1D, l2_* to "
                "private CHI L2, and hnf_* to shared CHI HNF"
            ),
            "mesi-two-level": (
                "timing restore will map l2_* to private MESI data cache and "
                "hnf_* to inclusive shared L2"
            ),
        }
        for protocol, expected_mapping in expected_mappings.items():
            with self.subTest(protocol=protocol):
                args = self._parse(
                    "--coherence-protocol",
                    protocol,
                    "--cpu-type",
                    "atomic",
                )
                hierarchy, name = _RUNNER._create_cache_hierarchy(
                    args, ISA.X86
                )
                self.assertIsInstance(hierarchy, NoCache)
                self.assertEqual("no-cache-checkpoint-prep", name)

                banner = _RUNNER._format_configuration_banner(
                    args, _BoardStub(), hierarchy, name
                )
                selected = "CHI" if protocol == "chi" else "MESI_Two_Level"
                self.assertIn(f"protocol={selected}", banner)
                self.assertIn(
                    "topology='uncached-checkpoint-preparation'", banner
                )
                self.assertIn(expected_mapping, banner)
                self.assertIn("private_data=inactive/deferred", banner)
                self.assertIn("hnfs=inactive/deferred", banner)
                self.assertIn("ddio_way_part=inactive/deferred", banner)
                if protocol == "mesi-two-level":
                    self.assertNotIn("native CHI", banner)

    def test_selected_protocol_must_be_compiled(self) -> None:
        supported = get_supported_protocols()
        if (
            CoherenceProtocol.CHI in supported
            and CoherenceProtocol.MESI_TWO_LEVEL in supported
        ):
            self.skipTest("The current binary contains both protocols.")

        selected = (
            "mesi-two-level" if CoherenceProtocol.CHI in supported else "chi"
        )
        required = "MESI_Two_Level" if selected == "mesi-two-level" else "CHI"
        args = self._parse("--coherence-protocol", selected)
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit):
                _RUNNER._require_selected_protocol(
                    _RUNNER._create_parser(), args
                )
        self.assertIn(
            f"requires the compiled Ruby protocol {required}",
            stderr.getvalue(),
        )


if __name__ == "__main__":
    unittest.main()
