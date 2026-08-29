# SPDX-License-Identifier: BSD-3-Clause

from pathlib import Path
import re
import subprocess

from testlib import *


for name, args, marker in (
    ("dsr", (), "PVRDMA_ATOMIC_DSR_OK"),
    ("command", ("--command",), "PVRDMA_ATOMIC_COMMAND_OK"),
):
    gem5_verify_config(
        name=f"pvrdma-atomic-{name}-visibility",
        fixtures=(),
        verifiers=(verifier.MatchRegex(re.compile(marker)),),
        config=joinpath(
            absdirpath(__file__),
            "configs",
            "pvrdma_atomic_visibility.py",
        ),
        config_args=args,
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
    )


for name, mode, marker in (
    ("mr-walk", "timing-mr", "PVRDMA_TIMING_MR_OK"),
    ("queue-walk", "timing-queues", "PVRDMA_TIMING_QUEUES_OK"),
    (
        "queue-observation",
        "timing-observation",
        "PVRDMA_TIMING_OBSERVATION_OK",
    ),
    ("queue-stats-reset", "stats-reset", "PVRDMA_STATS_RESET_OK"),
):
    gem5_verify_config(
        name=f"pvrdma-timing-{name}",
        fixtures=(),
        verifiers=(verifier.MatchRegex(re.compile(marker)),),
        config=joinpath(
            absdirpath(__file__), "configs", "pvrdma_runtime.py"
        ),
        config_args=("--mode", mode),
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
    )


def pvrdma_checkpoint_test(host):
    def run_test(params):
        tempdir = Path(params.fixtures[constants.tempdir_fixture_name].path)
        gem5 = params.fixtures[constants.gem5_binary_fixture_name].path
        config_path = joinpath(
            absdirpath(__file__), "configs", "pvrdma_runtime.py"
        )
        checkpoint = tempdir / "pvrdma-checkpoint"
        for phase, marker in (
            ("save", "PVRDMA_CHECKPOINT_SAVED"),
            ("restore", "PVRDMA_CHECKPOINT_RESTORED"),
        ):
            result = subprocess.run(
                [
                    gem5,
                    "-d",
                    (tempdir / phase).as_posix(),
                    config_path,
                    "--mode",
                    f"checkpoint-{phase}",
                    "--checkpoint",
                    checkpoint.as_posix(),
                ],
                cwd=config.base_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            print(result.stdout)
            assert result.returncode == 0
            assert marker in result.stdout

    name = f"pvrdma-checkpoint-live-mr-{host}-opt"
    TestSuite(
        name=name,
        fixtures=[Gem5Fixture("x86", "opt"), TempdirFixture()],
        tags=["X86", "opt", constants.quick_tag, host],
        tests=[TestFunction(run_test, name=name)],
    )


def pvrdma_observation_checkpoint_test(host):
    def run_test(params):
        tempdir = Path(params.fixtures[constants.tempdir_fixture_name].path)
        gem5 = params.fixtures[constants.gem5_binary_fixture_name].path
        config_path = joinpath(
            absdirpath(__file__), "configs", "pvrdma_runtime.py"
        )
        checkpoint = tempdir / "pvrdma-observation-checkpoint"
        for phase, marker in (
            ("save", "PVRDMA_OBSERVATION_CHECKPOINT_SAVED"),
            ("restore", "PVRDMA_OBSERVATION_CHECKPOINT_RESTORED"),
        ):
            result = subprocess.run(
                [
                    gem5,
                    "-d",
                    (tempdir / f"observation-{phase}").as_posix(),
                    config_path,
                    "--mode",
                    f"checkpoint-observation-{phase}",
                    "--checkpoint",
                    checkpoint.as_posix(),
                ],
                cwd=config.base_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            print(result.stdout)
            assert result.returncode == 0
            assert marker in result.stdout
            if phase == "save":
                path = checkpoint / "m5.cpt"
                contents, replacements = re.subn(
                    r"(\[system\.rdma\.cq1\][\s\S]*?^producerTail=)0$",
                    r"\g<1>3",
                    path.read_text(),
                    count=1,
                    flags=re.MULTILINE,
                )
                assert replacements == 1
                path.write_text(contents)

    name = f"pvrdma-checkpoint-queue-observation-{host}-opt"
    TestSuite(
        name=name,
        fixtures=[Gem5Fixture("x86", "opt"), TempdirFixture()],
        tags=["X86", "opt", constants.quick_tag, host],
        tests=[TestFunction(run_test, name=name)],
    )


for host in constants.supported_hosts:
    pvrdma_checkpoint_test(host)
    pvrdma_observation_checkpoint_test(host)
