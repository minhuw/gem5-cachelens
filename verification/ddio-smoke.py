#!/usr/bin/env python3
# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""Run the primary x86/MESI_Three_Level DDIO smoke with a supplied gem5 binary."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests/gem5/mesi_ddio"
sys.path.insert(0, str(TESTS))
from three_level_checks import (
    _EXPECTED,
    check_output,
    check_stats,
    scenario_args,
)


def run_case(gem5, output, scenario, timeout):
    directory = output / scenario
    directory.mkdir()
    command = [
        str(gem5),
        f"--outdir={directory}",
        str(TESTS / "configs/mesi_three_level_directed.py"),
        *scenario_args(scenario),
    ]
    result = {"scenario": scenario, "command": command, "passed": False}
    start = time.monotonic()
    try:
        with (directory / "run.log").open("w") as log:
            process = subprocess.run(
                command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                timeout=timeout, check=True,
            )
        result["returncode"] = process.returncode
        check_output((directory / "run.log").read_text(), scenario)
        check_stats(directory / "stats.txt", scenario)
        result["passed"] = True
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        if isinstance(error, subprocess.CalledProcessError):
            result["returncode"] = error.returncode
    result["elapsed_seconds"] = time.monotonic() - start
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", required=True, type=Path)
    parser.add_argument("--output", type=Path, help="new evidence directory (must not exist)")
    parser.add_argument("--timeout", type=float, default=120,
                        help="wall-clock seconds per scenario (default: 120)")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive")
    gem5 = args.gem5.resolve()
    if not gem5.is_file():
        parser.error(f"gem5 binary does not exist: {gem5}")
    try:
        if args.output is None:
            output = Path(tempfile.mkdtemp(prefix="cachelens-three-level-smoke-"))
        else:
            output = args.output.resolve()
            output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(str(error))
    print(f"Evidence: {output}", flush=True)
    results = []
    for scenario in _EXPECTED:
        result = run_case(gem5, output, scenario, args.timeout)
        results.append(result)
        (output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
        status = "PASS" if result["passed"] else "FAIL"
        print(f"{status} {scenario} ({result['elapsed_seconds']:.2f}s)", flush=True)
        if not result["passed"]:
            print(result["error"], file=sys.stderr, flush=True)
    passed = sum(result["passed"] for result in results)
    print(f"{passed}/{len(results)} passed", flush=True)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
