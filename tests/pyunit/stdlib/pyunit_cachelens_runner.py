# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import contextlib
import importlib.util
import io
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import (
    Mock,
    call,
    patch,
)

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
        self.assertEqual("off", args.cache_stashing)
        self.assertIsNone(args.hnf_inclusion)
        _RUNNER._validate_args(parser, args)
        self.assertEqual(
            CoherenceProtocol.CHI, _RUNNER._selected_protocol(args)
        )

    def test_stats_window_validation(self) -> None:
        parser = _RUNNER._create_parser()
        self.assertIsNone(self._parse().stats_reset_delay)
        for isa in ("arm", "x86"):
            assets = ("--bootloader", str(_RUNNER_PATH)) if isa == "arm" else ()
            for cpu in ("timing", "o3"):
                args = self._parse(
                    "--isa", isa, *assets, "--cpu-type", cpu,
                    "--max-ticks", "100", "--stats-reset-delay", "25",
                )
                _RUNNER._validate_args(parser, args)
            for invalid in (
                ("--stats-reset-delay", "0"),
                ("--stats-reset-delay", "-1"),
                ("--stats-reset-delay", "100"),
                ("--stats-reset-delay", "101"),
                ("--max-ticks", "0"),
                ("--max-ticks", "-1"),
                ("--cpu-type", "atomic"),
                ("--checkpoint-at-max-tick",),
            ):
                with self.subTest(isa=isa, invalid=invalid):
                    args = self._parse(
                        "--isa", isa, *assets, "--max-ticks", "100",
                        "--stats-reset-delay", "25", *invalid,
                    )
                    with contextlib.redirect_stderr(io.StringIO()):
                        with self.assertRaises(SystemExit):
                            _RUNNER._validate_args(parser, args)
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self._parse("--stats-reset-delay", "1.5")

    def test_stats_window_default_unchanged(self) -> None:
        simulator, processor = Mock(), Mock()
        with patch.object(_RUNNER.m5, "stats") as stats:
            _RUNNER._run_with_stats_window(simulator, processor, self._parse())
        self.assertEqual([call.run()], simulator.mock_calls)
        self.assertEqual([], processor.mock_calls)
        self.assertEqual([], stats.mock_calls)

    def test_stats_window_order_and_relative_ticks(self) -> None:
        for isa in ("arm", "x86"):
            for cpu in ("timing", "o3"):
                with self.subTest(isa=isa, cpu=cpu):
                    args = self._parse(
                        "--isa", isa, "--cpu-type", cpu,
                        "--max-ticks", "100", "--stats-reset-delay", "25",
                    )
                    simulator, processor, events = Mock(), Mock(), Mock()
                    cores = [Mock(), Mock()]
                    processor.get_cores.return_value = cores
                    for core in cores:
                        core.get_simobject.return_value.do_statistics_insts = True
                    tick = 1000  # A restored checkpoint, not tick zero.

                    def run(max_ticks):
                        nonlocal tick
                        for core in cores:
                            self.assertIs(
                                core.get_simobject().do_statistics_insts, False
                            )
                        tick += max_ticks

                    simulator.run.side_effect = run
                    simulator.get_tick_stopwatch.return_value = []
                    simulator.get_last_exit_event_cause.return_value = (
                        "simulate() limit reached"
                    )
                    events.attach_mock(simulator.run, "run")
                    stdout = io.StringIO()
                    with patch.object(_RUNNER.m5, "stats") as stats, patch.object(
                        _RUNNER.m5, "curTick", side_effect=lambda: tick
                    ), contextlib.redirect_stdout(stdout):
                        events.attach_mock(stats.dump, "dump")
                        events.attach_mock(stats.reset, "reset")
                        _RUNNER._run_with_stats_window(simulator, processor, args)
                    self.assertEqual(
                        [call.run(max_ticks=25), call.dump(), call.reset(),
                         call.run(max_ticks=75)], events.mock_calls,
                    )
                    self.assertEqual(
                        "CACHELENS_STATS_RESET tick=1025\n"
                        "CACHELENS_STATS_COMPLETE tick=1100\n",
                        stdout.getvalue(),
                    )

    def test_stats_window_rejects_early_exits(self) -> None:
        args = self._parse("--max-ticks", "100", "--stats-reset-delay", "25")
        for phase in ("warmup", "measurement"):
            with self.subTest(phase=phase):
                simulator, processor = Mock(), Mock()
                processor.get_cores.return_value = []
                simulator.get_tick_stopwatch.return_value = []
                simulator.get_last_exit_event_cause.side_effect = (
                    ["m5_exit instruction encountered"] if phase == "warmup"
                    else ["simulate() limit reached", "m5_exit instruction encountered"]
                )
                stdout = io.StringIO()
                with patch.object(_RUNNER.m5, "stats") as stats, patch.object(
                    _RUNNER.m5, "curTick", return_value=25
                ), contextlib.redirect_stdout(stdout):
                    with self.assertRaisesRegex(RuntimeError, phase):
                        _RUNNER._run_with_stats_window(simulator, processor, args)
                self.assertNotIn("CACHELENS_STATS_COMPLETE", stdout.getvalue())
                if phase == "warmup":
                    simulator.run.assert_called_once_with(max_ticks=25)
                    self.assertEqual([], stats.mock_calls)
                    self.assertEqual("", stdout.getvalue())
                else:
                    self.assertEqual(2, simulator.run.call_count)
                    stats.dump.assert_called_once_with()
                    stats.reset.assert_called_once_with()

    def test_stats_window_rejects_continued_exit(self) -> None:
        args = self._parse("--max-ticks", "100", "--stats-reset-delay", "25")
        simulator, processor = Mock(), Mock()
        processor.get_cores.return_value = []
        simulator.get_last_exit_event_cause.return_value = "simulate() limit reached"
        simulator.get_tick_stopwatch.return_value = [
            (_RUNNER.ExitEvent.WORKBEGIN, 10), (_RUNNER.ExitEvent.MAX_TICK, 35)
        ]
        with patch.object(_RUNNER.m5, "stats") as stats:
            with self.assertRaisesRegex(RuntimeError, "warmup"):
                _RUNNER._run_with_stats_window(simulator, processor, args)
            stats.reset.assert_not_called()

    def test_arm_chi_validation(self) -> None:
        args = self._parse(
            "--isa", "arm", "--bootloader", str(_RUNNER_PATH)
        )
        _RUNNER._validate_args(_RUNNER._create_parser(), args)
        self.assertEqual(
            CoherenceProtocol.CHI, _RUNNER._selected_protocol(args)
        )

    def test_cache_stashing_forwarding(self) -> None:
        args = self._parse(
            "--isa", "arm", "--bootloader", str(_RUNNER_PATH),
            "--cache-stashing", "hnf",
        )
        _RUNNER._validate_args(_RUNNER._create_parser(), args)
        module_name = "gem5.prebuilt.cachelens.cache_hierarchy"
        module = types.ModuleType(module_name)
        module.CacheLensCHIHierarchy = Mock()
        with patch.dict(sys.modules, {module_name: module}):
            _RUNNER._create_cache_hierarchy(args, ISA.ARM)
        forwarded = module.CacheLensCHIHierarchy.call_args.kwargs
        self.assertEqual("hnf", forwarded["cache_stashing"])
        self.assertEqual("arm-generic", forwarded["model_profile"])
        self.assertIsNone(forwarded["ddio_way_part"])

    def test_cache_stashing_rejected_before_guest_assets(self) -> None:
        for protocol in ("chi", "mesi-two-level", "mesi-three-level"):
            for isa in ("arm", "x86"):
                if (protocol, isa) == ("chi", "arm"):
                    continue
                for cpu_type in ("atomic", "timing"):
                    with self.subTest(
                        protocol=protocol, isa=isa, cpu=cpu_type
                    ):
                        argv = [
                            str(_RUNNER_PATH), "--isa", isa,
                            "--coherence-protocol", protocol,
                            "--cpu-type", cpu_type,
                            "--cache-stashing", "hnf",
                            "--kernel", "/missing/kernel",
                            "--disk-image", "/missing/disk",
                        ]
                        stderr = io.StringIO()
                        with patch.object(sys, "argv", argv), patch.object(
                            _RUNNER, "KernelResource"
                        ) as kernel, patch.object(
                            _RUNNER, "DiskImageResource"
                        ) as disk, contextlib.redirect_stderr(stderr):
                            with self.assertRaises(SystemExit):
                                _RUNNER.main()
                        self.assertIn("requires ARM CHI", stderr.getvalue())
                        kernel.assert_not_called()
                        disk.assert_not_called()
        args = self._parse(
            "--isa", "arm", "--bootloader", str(_RUNNER_PATH),
            "--cache-stashing", "hnf", "--ddio-way-part", "2",
        )
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
            _RUNNER._validate_args(_RUNNER._create_parser(), args)
        self.assertIn("requires --ddio-way-part=-1", stderr.getvalue())

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

        splitmix_args = self._parse(
            "--coherence-protocol",
            "mesi-two-level",
            "--hnf-inclusion",
            "inclusive",
            "--indexing-policy",
            "splitmix64",
        )
        _RUNNER._validate_args(parser, splitmix_args)
        self.assertEqual("splitmix64", splitmix_args.indexing_policy)

        for invalid in (
            ("--hnf-inclusion", "noninclusive"),
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

    def test_atomic_preparation_stays_uncached_for_all_selections(
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
        expected_mappings["mesi-three-level"] = (
            "timing restore will map l1d_* to private MESI L1D, l2_* to "
            "private MESI L2, and hnf_* to inclusive shared L3"
        )
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
                selected = _RUNNER._selected_protocol(args).value
                self.assertIn(f"protocol={selected}", banner)
                self.assertIn(
                    "topology='uncached-checkpoint-preparation'", banner
                )
                self.assertIn(expected_mapping, banner)
                self.assertIn("private_data=inactive/deferred", banner)
                self.assertIn("hnfs=inactive/deferred", banner)
                self.assertIn("ddio_way_part=inactive/deferred", banner)
                if protocol != "chi":
                    self.assertNotIn("CHI", banner)
                    self.assertIn(
                        "hnf_inclusion=deferred-to-inclusive-timing-restore",
                        banner,
                    )

    def test_three_level_options_and_arm_rejections(self) -> None:
        parser = _RUNNER._create_parser()
        args = self._parse(
            "--coherence-protocol", "mesi-three-level",
            "--l1d-size", "128KiB", "--l1d-assoc", "4",
            "--hnf-inclusion", "inclusive",
        )
        _RUNNER._validate_args(parser, args)
        self.assertEqual(
            CoherenceProtocol.MESI_THREE_LEVEL,
            _RUNNER._selected_protocol(args),
        )
        for protocol in ("mesi-two-level", "mesi-three-level"):
            for invalid in (
                ("--hnf-inclusion", "noninclusive"),
                ("--dealloc-on-unique",),
                ("--isa", "arm", "--bootloader", str(_RUNNER_PATH)),
            ):
                with self.subTest(protocol=protocol, invalid=invalid):
                    args = self._parse(
                        "--coherence-protocol", protocol, *invalid
                    )
                    with contextlib.redirect_stderr(io.StringIO()):
                        with self.assertRaises(SystemExit):
                            _RUNNER._validate_args(parser, args)

    def test_timing_hierarchy_selection_and_parameters(self) -> None:
        for protocol, isa, module_name, class_name in (
            ("chi", ISA.ARM, "cache_hierarchy", "CacheLensCHIHierarchy"),
            ("mesi-two-level", ISA.X86, "mesi_two_level_cache_hierarchy",
             "CacheLensMESITwoLevelHierarchy"),
            ("mesi-three-level", ISA.X86, "mesi_three_level_cache_hierarchy",
             "CacheLensMESIThreeLevelHierarchy"),
        ):
            with self.subTest(protocol=protocol):
                args = self._parse(
                    "--coherence-protocol", protocol,
                    "--l1d-size", "128KiB", "--l1d-assoc", "4",
                    "--l2-size", "512KiB", "--l2-assoc", "16",
                    "--hnf-size", "4MiB", "--hnf-assoc", "32",
                    "--num-hnfs", "4", "--indexing-policy", "splitmix64",
                )
                module_name = "gem5.prebuilt.cachelens." + module_name
                module = types.ModuleType(module_name)
                constructor = Mock(return_value=object())
                setattr(module, class_name, constructor)
                with patch.dict(sys.modules, {module_name: module}):
                    hierarchy, name = _RUNNER._create_cache_hierarchy(
                        args, isa
                    )
                self.assertIs(hierarchy, constructor.return_value)
                self.assertEqual("cachelens-" + protocol, name)
                expected = {
                    "l1i_size": "32KiB", "l1i_assoc": 2,
                    "l2_size": "512KiB", "l2_assoc": 16,
                    "hnf_size": "4MiB", "hnf_assoc": 32, "num_hnfs": 4,
                    "ddio_way_part": None,
                    "model_profile": (
                        "arm-generic" if isa == ISA.ARM else "x86-generic"
                    ),
                    "core_clock": "2GHz", "link_latency": 1,
                    "router_latency": 1, "network_buffer_size": 4,
                    "indexing_policy": "splitmix64",
                }
                if protocol != "mesi-two-level":
                    expected.update(l1d_size="128KiB", l1d_assoc=4)
                if protocol == "chi":
                    expected.update(
                        addr_hash=None, dealloc_on_unique=False,
                        hnf_inclusion="noninclusive", cache_stashing="off",
                    )
                constructor.assert_called_once_with(**expected)
                banner = _RUNNER._format_configuration_banner(
                    args, _BoardStub(), hierarchy, name
                )
                topology = {
                    "chi": "chi-private-l1-private-l2-shared-hnf",
                    "mesi-two-level": "mesi-private-l1-shared-l2",
                    "mesi-three-level": "mesi-private-l1-private-l2-shared-l3",
                }[protocol]
                self.assertIn(f"topology='{topology}'", banner)
                inclusion = (
                    "noninclusive" if protocol == "chi" else "inclusive"
                )
                self.assertIn(f"hnf_inclusion={inclusion}", banner)

    def test_protocol_build_hints(self) -> None:
        for isa, protocol, target in (
            ("arm", "chi", "ARM_CHI"),
            ("x86", "chi", "X86_CHI"),
            ("x86", "mesi-two-level", "X86_MESI_Two_Level"),
            ("x86", "mesi-three-level", "X86_MESI_Three_Level"),
        ):
            with self.subTest(isa=isa, protocol=protocol):
                args = self._parse(
                    "--isa", isa, "--coherence-protocol", protocol
                )
                stderr = io.StringIO()
                with patch.object(
                    _RUNNER, "get_supported_protocols", return_value=set()
                ):
                    with contextlib.redirect_stderr(stderr):
                        with self.assertRaises(SystemExit):
                            _RUNNER._require_selected_protocol(
                                _RUNNER._create_parser(), args
                            )
                self.assertIn(f"build/{target}/gem5.opt", stderr.getvalue())

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
