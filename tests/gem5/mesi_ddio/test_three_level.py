# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""Resource-free tests of the actual CacheLens native three-level hierarchy."""

from pathlib import Path

from testlib import *
from testlib import test_util
from three_level_checks import _EXPECTED, check_output, check_stats, scenario_args


_CONFIG = joinpath(
    config.base_dir,
    "tests",
    "gem5",
    "mesi_ddio",
    "configs",
    "mesi_three_level_directed.py",
)


class ThreeLevelStatsVerifier(verifier.Verifier):
    def __init__(self, scenario):
        super().__init__()
        self.scenario = scenario

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        try:
            output = "\n".join(
                (Path(tempdir) / filename).read_text()
                for filename in (
                    constants.gem5_simulation_stdout,
                    constants.gem5_simulation_stderr,
                )
            )
            check_output(output, self.scenario)
            check_stats(
                joinpath(tempdir, constants.gem5_simulation_stats),
                self.scenario,
            )
        except (OSError, ValueError) as error:
            test_util.fail(str(error))


for scenario in _EXPECTED:
    gem5_verify_config(
        name=f"cachelens-mesi-three-level-{scenario.replace('_', '-')}",
        fixtures=(),
        verifiers=(ThreeLevelStatsVerifier(scenario),),
        config=_CONFIG,
        config_args=scenario_args(scenario),
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
        protocol="MESI_Three_Level",
    )
