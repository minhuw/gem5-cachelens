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
    ("transport-pair", "transport-pair", "PVRDMA_TRANSPORT_PAIR_OK"),
    (
        "transport-pair-timing",
        "timing-transport-pair",
        "PVRDMA_TRANSPORT_PAIR_OK",
    ),
    ("publication", "completion", "PVRDMA_COMPLETION_OK"),
    ("errors", "completion-errors", "PVRDMA_COMPLETION_ERRORS_OK"),
):
    gem5_verify_config(
        name=f"pvrdma-completion-{name}",
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


for mode in ("semantic-pair", "timing-semantic-pair"):
    gem5_verify_config(
        name=f"pvrdma-{mode}",
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(re.compile("PVRDMA_SEMANTIC_PAIR_OK")),
        ),
        config=joinpath(
            absdirpath(__file__), "configs", "pvrdma_runtime.py"
        ),
        config_args=("--mode", mode),
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
    )


for name, marker in (
    (
        "precommit-abort-pair",
        "PVRDMA_RELIABILITY_PRECOMMIT_ABORT_PAIR_OK",
    ),
    ("commit-pair", "PVRDMA_RELIABILITY_COMMIT_PAIR_OK"),
    (
        "commit-boundary-pair",
        "PVRDMA_RELIABILITY_COMMIT_BOUNDARY_PAIR_OK",
    ),
):
    gem5_verify_config(
        name=f"pvrdma-timing-reliability-{name}",
        fixtures=(),
        verifiers=(verifier.MatchRegex(re.compile(marker)),),
        config=joinpath(
            absdirpath(__file__), "configs", "pvrdma_runtime.py"
        ),
        config_args=("--mode", f"timing-reliability-{name}"),
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
    )


for name, marker in (
    ("reliability-pair", "PVRDMA_RELIABILITY_PAIR_OK"),
    ("reliability-rnr-pair", "PVRDMA_RELIABILITY_RNR_PAIR_OK"),
    (
        "reliability-timeout-zero-pair",
        "PVRDMA_RELIABILITY_TIMEOUT_ZERO_PAIR_OK",
    ),
    ("reliability-invalid-pair", "PVRDMA_RELIABILITY_INVALID_PAIR_OK"),
    (
        "reliability-unrelated-pair",
        "PVRDMA_RELIABILITY_UNRELATED_PAIR_OK",
    ),
    ("reliability-cq-pair", "PVRDMA_RELIABILITY_CQ_PAIR_OK"),
    (
        "reliability-cq-abort-pair",
        "PVRDMA_RELIABILITY_CQ_ABORT_PAIR_OK",
    ),
    ("fault-link", "PVRDMA_FAULT_LINK_OK"),
):
    for prefix in ("", "timing-"):
        mode = f"{prefix}{name}"
        gem5_verify_config(
            name=f"pvrdma-{mode}",
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


for name, mode, marker in (
    ("mr-walk", "timing-mr", "PVRDMA_TIMING_MR_OK"),
    ("queue-walk", "timing-queues", "PVRDMA_TIMING_QUEUES_OK"),
    (
        "queue-observation",
        "timing-observation",
        "PVRDMA_TIMING_OBSERVATION_OK",
    ),
    (
        "completion-ordering",
        "timing-completion",
        "PVRDMA_TIMING_COMPLETION_OK",
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
                contents = (checkpoint / "m5.cpt").read_text()
                match = re.search(
                    r"\[system\.rdma\.cq1\][\s\S]*?^producerTail=(\d+)$",
                    contents,
                    flags=re.MULTILINE,
                )
                assert match and match.group(1) == "3"

    name = f"pvrdma-checkpoint-queue-observation-{host}-opt"
    TestSuite(
        name=name,
        fixtures=[Gem5Fixture("x86", "opt"), TempdirFixture()],
        tags=["X86", "opt", constants.quick_tag, host],
        tests=[TestFunction(run_test, name=name)],
    )


def pvrdma_completion_checkpoint_test(host):
    def run_test(params):
        tempdir = Path(params.fixtures[constants.tempdir_fixture_name].path)
        gem5 = params.fixtures[constants.gem5_binary_fixture_name].path
        config_path = joinpath(
            absdirpath(__file__), "configs", "pvrdma_runtime.py"
        )
        checkpoint = tempdir / "pvrdma-completion-checkpoint"
        for phase, marker in (
            ("save", "PVRDMA_COMPLETION_CHECKPOINT_SAVED"),
            ("restore", "PVRDMA_COMPLETION_CHECKPOINT_RESTORED"),
        ):
            result = subprocess.run(
                [
                    gem5,
                    "-d",
                    (tempdir / f"completion-{phase}").as_posix(),
                    config_path,
                    "--mode",
                    f"checkpoint-completion-{phase}",
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

    name = f"pvrdma-checkpoint-completion-{host}-opt"
    TestSuite(
        name=name,
        fixtures=[Gem5Fixture("x86", "opt"), TempdirFixture()],
        tags=["X86", "opt", constants.quick_tag, host],
        tests=[TestFunction(run_test, name=name)],
    )


def pvrdma_polling_only_source_test(_):
    root = Path(config.base_dir)
    implementation = "\n".join(
        (root / path).read_text()
        for path in (
            "src/dev/rdma/pvrdma.cc",
            "src/dev/rdma/pvrdma.hh",
            "src/dev/rdma/pvrdma_ring.hh",
        )
    )
    for forbidden in (
        "completionRingDirectoryReadDone",
        "completionRingForwardDistance",
        "ReadNotificationRing",
        "WriteNotificationEntry",
        "cqNotificationsPublished",
        "cqCompletionInterrupts",
        "regs.pendingCauses |= pvrdma::InterruptCauseCompletion",
    ):
        assert forbidden not in implementation


for host in constants.supported_hosts:
    name = f"pvrdma-completion-polling-only-source-{host}"
    TestSuite(
        name=name,
        fixtures=[],
        tags=["ALL", constants.quick_tag, host],
        tests=[TestFunction(pvrdma_polling_only_source_test, name=name)],
    )
    pvrdma_checkpoint_test(host)
    pvrdma_observation_checkpoint_test(host)
    pvrdma_completion_checkpoint_test(host)
