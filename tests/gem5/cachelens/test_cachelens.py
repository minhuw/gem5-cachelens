# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import re
import shutil
import subprocess
import sys
from pathlib import Path

from testlib import *
from testlib.helper import log_call

resource_directory = (
    config.bin_path
    if config.bin_path
    else joinpath(config.base_dir, "tests", "gem5", "resources")
)


class CacheLensExactProtocolFixture(Gem5Fixture):
    """Build a CacheLens binary from one exact ISA/protocol defconfig."""

    _protocol_defconfigs = {
        ("X86", "CHI"): "X86_CHI",
        ("X86", "MESI_Two_Level"): "X86_MESI_Two_Level",
        ("ARM", "CHI"): "ARM_CHI",
    }

    def __new__(cls, isa, variant, protocol):
        if (isa.upper(), protocol) not in cls._protocol_defconfigs:
            raise ValueError(f"Unsupported CacheLens protocol: {protocol}")
        return super().__new__(cls, isa, variant, protocol=protocol)

    def _init(self, isa, variant, protocol):
        self._exact_protocol = protocol
        super()._init(isa, variant)

    def _get_defconfig(self):
        return self._protocol_defconfigs[(self.isa.upper(), self._exact_protocol)]


def cachelens_board_test(
    isa, num_nics, expect_reject=False, enable_pvrdma=False
):
    marker = (
        "CACHELENS_BOARD_REJECT_OK"
        if expect_reject
        else "CACHELENS_BOARD_CONFIG_OK"
    )
    config_args = ["--isa", isa, "--num-nics", str(num_nics)]
    if expect_reject:
        config_args.append("--expect-reject")
    if enable_pvrdma:
        config_args.append("--enable-pvrdma")

    suffix = "-pvrdma" if enable_pvrdma else ""
    gem5_verify_config(
        name=f"cachelens-{isa}-{num_nics}-nic{suffix}-board-config",
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(
                re.compile(f"{marker} isa={isa} nics={num_nics}")
            ),
        ),
        config=joinpath(
            absdirpath(__file__),
            "configs",
            "cachelens_board_probe.py",
        ),
        config_args=config_args,
        valid_isas=(constants.all_compiled_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
    )


def cachelens_arm_chi_profile_test(profile, inclusion="noninclusive"):
    marker = (
        "CACHELENS_ARM_CHI_REJECT_OK"
        if profile == "intel-ddio"
        else "CACHELENS_ARM_CHI_GRAPH_OK"
    )
    gem5_verify_config(
        name=f"cachelens-arm-chi-{profile}-{inclusion}-graph",
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(re.compile(f"{marker} profile={profile}")),
        ),
        config=joinpath(
            absdirpath(__file__), "configs", "arm_chi_profile_probe.py"
        ),
        config_args=["--model-profile", profile, "--hnf-inclusion", inclusion],
        valid_isas=(constants.arm_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
        protocol="CHI",
        gem5_fixture_factory=CacheLensExactProtocolFixture,
    )


def cachelens_chi_dma_test(scenario, inclusion=None,
                           read_not_shared_dirty=False, dealloc=False):
    # Protocol/data-value coverage only: no guest execution or external TLM.
    config_args = ["--scenario", scenario]
    suffix = ""
    if inclusion is not None:
        config_args += ["--hnf-inclusion", inclusion, "--ddio-way-part", "4"]
        suffix += f"-{inclusion}"
    if read_not_shared_dirty:
        config_args.append("--read-not-shared-dirty")
        suffix += "-read-not-shared-dirty"
    if dealloc:
        config_args.append("--dealloc-on-unique")
        suffix += "-legacy-dealloc"
    gem5_verify_config(
        name=f"cachelens-arm-chi-dma-{scenario}{suffix}",
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(re.compile(
                f"CACHELENS_CHI_DMA_OK scenario={scenario}"
            )),
        ),
        config=joinpath(
            absdirpath(__file__), "configs", "chi_dma_directed.py"
        ),
        config_args=config_args,
        valid_isas=(constants.arm_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
        protocol="CHI",
        gem5_fixture_factory=CacheLensExactProtocolFixture,
    )


def cachelens_chi_stash_test(scenario, mode="hnf", decline=False,
                            inclusion="inclusive"):
    # Native synthetic DMA, including RDMA provenance; not ARM PVRDMA boot.
    config_args = ["--scenario", scenario, "--cache-stashing", mode,
                   "--ddio-way-part", "-1", "--hnf-inclusion", inclusion]
    if decline:
        config_args.append("--decline-stash")
    gem5_verify_config(
        name=(f"cachelens-arm-chi-stash-{mode}-{scenario}"
              f"-decline-{int(decline)}-{inclusion}"),
        fixtures=(),
        verifiers=(verifier.MatchRegex(re.compile(
            f"CACHELENS_CHI_STASH_OK scenario={scenario} "
            f"mode={mode} decline={int(decline)}"
        )),),
        config=joinpath(
            absdirpath(__file__), "configs", "chi_dma_directed.py"
        ),
        config_args=config_args,
        valid_isas=(constants.arm_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
        protocol="CHI",
        gem5_fixture_factory=CacheLensExactProtocolFixture,
    )


def cachelens_chi_dma_mask_rejection_test(host):
    def run_rejection(params):
        tempdir = Path(params.fixtures[constants.tempdir_fixture_name].path)
        gem5 = params.fixtures[constants.gem5_binary_fixture_name].path
        probe = joinpath(
            absdirpath(__file__), "configs", "chi_dma_directed.py"
        )
        for scenario, extra, diagnostic in (
            ("zero_mask", [], r"fatal:.*RubySequencer does not support masked writes"),
            ("sparse_mask", [], r"fatal:.*RubySequencer does not support masked writes"),
            ("cold_full_write", ["--cache-stashing", "hnf", "--abs-max-tick", "1"],
             r"RuntimeError: Unexpected simulation exit:"),
        ):
            result = subprocess.run(
                [
                    gem5,
                    "-d",
                    (tempdir / scenario).as_posix(),
                    probe,
                    "--scenario",
                    scenario,
                    *extra,
                ],
                cwd=config.base_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=120,
            )
            print(result.stdout)
            assert result.returncode != 0, f"{scenario} unexpectedly succeeded"
            assert re.search(
                diagnostic,
                result.stdout,
            ), f"{scenario} did not report {diagnostic}"

    name = f"cachelens-arm-chi-dma-mask-rejection-{host}-opt"
    TestSuite(
        name=name,
        fixtures=[
            CacheLensExactProtocolFixture("arm", "opt", "CHI"),
            TempdirFixture(),
        ],
        tags=["ARM", "opt", constants.quick_tag, host],
        tests=[TestFunction(run_rejection, name=name)],
    )


def cachelens_pvrdma_parser_test():
    gem5_verify_config(
        name="cachelens-pvrdma-parser-config",
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(re.compile("CACHELENS_PVRDMA_PARSER_OK")),
        ),
        config=joinpath(
            absdirpath(__file__),
            "configs",
            "cachelens_pvrdma_parser_probe.py",
        ),
        config_args=(),
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
    )


def cachelens_pvrdma_instantiate_test(host):
    def run_test(params):
        tempdir = Path(params.fixtures[constants.tempdir_fixture_name].path)
        gem5 = params.fixtures[constants.gem5_binary_fixture_name].path
        probe = joinpath(
            absdirpath(__file__),
            "configs",
            "cachelens_board_probe.py",
        )
        kernel = joinpath(
            config.base_dir,
            "tests",
            "test-progs",
            "cachelens-checkpoint",
            "bin",
            "x86",
            "linux",
            "cachelens-checkpoint",
        )
        for hierarchy in ("classic", "mesi-two-level"):
            checkpoint = tempdir / f"{hierarchy}-checkpoint"
            for phase in ("save", "restore"):
                command = [
                    gem5,
                    "-d",
                    (tempdir / f"{hierarchy}-{phase}").as_posix(),
                    probe,
                    "--isa",
                    "x86",
                    "--num-nics",
                    "1",
                    "--enable-pvrdma",
                    "--hierarchy",
                    hierarchy,
                    "--phase",
                    phase,
                    "--checkpoint",
                    checkpoint.as_posix(),
                    "--kernel",
                    kernel,
                ]
                result = subprocess.run(
                    command,
                    cwd=config.base_dir,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
                print(result.stdout)
                assert result.returncode == 0
                assert (
                    "CACHELENS_PVRDMA_INSTANTIATED "
                    f"hierarchy={hierarchy} phase={phase} "
                    "dma=IDE,E1000,PVRDMA"
                ) in result.stdout

    name = f"cachelens-pvrdma-instantiate-{host}-opt"
    TestSuite(
        name=name,
        fixtures=[
            CacheLensExactProtocolFixture("x86", "opt", "MESI_Two_Level"),
            TempdirFixture(),
        ],
        tags=["X86", "opt", constants.quick_tag, host],
        tests=[TestFunction(run_test, name=name)],
    )


def cachelens_test(
    isa,
    length,
    max_ticks=None,
    cpu_type="timing",
    model_profile="abstract",
    hnf_inclusion="noninclusive",
    coherence_protocol="chi",
    indexing_policy="linear",
):
    suffix = "config" if max_ticks is not None else "boot"
    config_args = [
        "--isa",
        isa,
        "--coherence-protocol",
        coherence_protocol,
        "--cpu-type",
        cpu_type,
        "--resource-directory",
        resource_directory,
        "--model-profile",
        model_profile,
        "--hnf-inclusion",
        hnf_inclusion,
        "--indexing-policy",
        indexing_policy,
    ]
    if max_ticks is not None:
        config_args += ["--max-ticks", str(max_ticks)]

    if coherence_protocol == "mesi-two-level":
        if isa != "x86":
            raise ValueError("The CacheLens MESI probe is x86-only.")
        valid_isas = (constants.x86_tag,)
        fixture_protocol = "MESI_Two_Level"
    else:
        valid_isas = (
            (constants.x86_tag,)
            if isa == "x86"
            else (constants.all_compiled_tag,)
        )
        fixture_protocol = "CHI" if isa == "x86" else None

    indexing_suffix = (
        "" if indexing_policy == "linear" else f"-{indexing_policy}"
    )
    gem5_verify_config(
        name=(
            f"cachelens-modern-{isa}-{coherence_protocol}-{cpu_type}-"
            f"{model_profile}-{hnf_inclusion}{indexing_suffix}-8gib-{suffix}"
        ),
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(
                re.compile(
                    f"CACHELENS_CONFIG_OK isa={isa}.*protocol="
                    f"{'CHI' if coherence_protocol == 'chi' else 'MESI_Two_Level'}"
                )
            ),
        ),
        config=joinpath(
            absdirpath(__file__),
            "configs",
            "cachelens_config_probe.py",
        ),
        config_args=config_args,
        valid_isas=valid_isas,
        valid_hosts=constants.supported_hosts,
        length=length,
        protocol=fixture_protocol,
        gem5_fixture_factory=(
            CacheLensExactProtocolFixture if fixture_protocol else None
        ),
    )


def cachelens_export_test(protocol):
    gem5_verify_config(
        name=f"cachelens-wildcard-exports-{protocol}",
        fixtures=(),
        verifiers=(),
        config=joinpath(
            config.base_dir,
            "tests",
            "pyunit",
            "stdlib",
            "pyunit_cachelens_exports.py",
        ),
        config_args=(),
        valid_isas=(constants.x86_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
        protocol=protocol,
        gem5_fixture_factory=CacheLensExactProtocolFixture,
    )


def cachelens_checkpoint_test(isa, host, coherence_protocol="chi"):
    def run_checkpoint_test(params):
        tempdir = Path(params.fixtures[constants.tempdir_fixture_name].path)
        gem5 = params.fixtures[constants.gem5_binary_fixture_name].path
        config_path = joinpath(
            absdirpath(__file__),
            "configs",
            "cachelens_checkpoint.py",
        )
        binary = joinpath(
            config.base_dir,
            "tests",
            "test-progs",
            "cachelens-checkpoint",
            "bin",
            isa,
            "linux",
            "cachelens-checkpoint",
        )

        def run_phase(
            name,
            hierarchy,
            cpu_type,
            phase,
            checkpoint,
            max_ticks=None,
            expect_success=True,
        ):
            output_dir = tempdir / name
            command = [
                gem5,
                "-d",
                output_dir.as_posix(),
                "--debug-flags=RubyCacheTrace",
                config_path,
                "--isa",
                isa,
                "--coherence-protocol",
                coherence_protocol,
                "--phase",
                phase,
                "--hierarchy",
                hierarchy,
                "--cpu-type",
                cpu_type,
                "--binary",
                binary,
                "--checkpoint",
                checkpoint.as_posix(),
            ]
            if max_ticks is not None:
                command.extend(("--max-ticks", str(max_ticks)))
            result = subprocess.run(
                command,
                cwd=config.base_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            print(result.stdout)
            if expect_success:
                assert (
                    result.returncode == 0
                ), f"{name} failed with exit code {result.returncode}"
            else:
                assert (
                    result.returncode != 0
                ), f"{name} unexpectedly accepted a corrupt checkpoint"
            return result.stdout

        timing_checkpoint = tempdir / "timing-checkpoint"
        run_phase(
            "cachelens-timing-save",
            "cachelens",
            "timing",
            "save",
            timing_checkpoint,
        )
        cold_output = run_phase(
            "cachelens-timing-restore",
            "cachelens",
            "timing",
            "restore",
            timing_checkpoint,
        )
        assert "CACHELENS_CHECKPOINT_DATA_OK" in cold_output
        assert "Ruby cache-trace replay is disabled" in cold_output
        assert "Starting ruby cache warmup" not in cold_output
        assert "Issuing [TraceRecord" not in cold_output

        corrupt_checkpoint = tempdir / "corrupt-checkpoint"
        shutil.copytree(timing_checkpoint, corrupt_checkpoint)
        trace_file = next(corrupt_checkpoint.glob("*.cache.gz"))
        trace_file.write_bytes(trace_file.read_bytes()[:-4])
        corrupt_output = run_phase(
            "cachelens-corrupt-restore",
            "cachelens",
            "timing",
            "restore",
            corrupt_checkpoint,
            expect_success=False,
        )
        assert "Unable to validate cache trace" in corrupt_output

        generic_checkpoint = tempdir / "generic-checkpoint"
        run_phase(
            "generic-ruby-save",
            "generic",
            "timing",
            "save",
            generic_checkpoint,
        )
        warm_output = run_phase(
            "generic-ruby-restore",
            "generic",
            "timing",
            "restore",
            generic_checkpoint,
            max_ticks=1,
        )
        assert "Starting ruby cache warmup" in warm_output
        assert "Issuing [TraceRecord" in warm_output

        atomic_checkpoint = tempdir / "atomic-checkpoint"
        run_phase(
            "atomic-prep-save",
            "none",
            "atomic",
            "save",
            atomic_checkpoint,
        )
        atomic_restore_output = run_phase(
            "atomic-to-timing-restore",
            "cachelens",
            "timing",
            "restore",
            atomic_checkpoint,
        )
        assert "CACHELENS_CHECKPOINT_DATA_OK" in atomic_restore_output
        assert "Starting ruby cache warmup" not in atomic_restore_output

    fixture_protocol = (
        "CHI" if coherence_protocol == "chi" else "MESI_Two_Level"
    )
    if isa == "arm" and coherence_protocol == "chi":
        fixture_protocol = None
    gem5_fixture = (
        CacheLensExactProtocolFixture(isa, "opt", fixture_protocol)
        if fixture_protocol
        else Gem5Fixture(isa, "opt")
    )
    TestSuite(
        name=(f"cachelens-checkpoint-{isa}-{coherence_protocol}-{host}-opt"),
        fixtures=[
            gem5_fixture,
            TempdirFixture(),
        ],
        tags=[isa.upper(), "opt", constants.quick_tag, host],
        tests=[
            TestFunction(
                run_checkpoint_test,
                name=f"cachelens-checkpoint-{isa}-{coherence_protocol}",
            )
        ],
    )


def cachelens_protocol_rejection_test(
    binary_protocol, selected_protocol, required_protocol, host
):
    def run_rejection(params):
        gem5 = params.fixtures[constants.gem5_binary_fixture_name].path
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        runner = joinpath(
            config.base_dir,
            "configs",
            "example",
            "gem5_library",
            "cachelens-fs.py",
        )
        command = [
            gem5,
            "-d",
            tempdir,
            runner,
            "--isa",
            "x86",
            "--coherence-protocol",
            selected_protocol,
            "--kernel",
            runner,
            "--disk-image",
            runner,
        ]
        result = subprocess.run(
            command,
            cwd=config.base_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        print(result.stdout)
        assert result.returncode != 0
        assert (
            f"requires the compiled Ruby protocol {required_protocol}"
            in result.stdout
        )
        assert f"but this binary provides {binary_protocol}" in result.stdout

    name = (
        f"cachelens-protocol-reject-{binary_protocol}-to-"
        f"{selected_protocol}-{host}-opt"
    )
    TestSuite(
        name=name,
        fixtures=[
            CacheLensExactProtocolFixture("x86", "opt", binary_protocol),
            TempdirFixture(),
        ],
        tags=["X86", "opt", constants.quick_tag, host],
        tests=[TestFunction(run_rejection, name=name)],
    )


def run_pcap_replay_tests(params):
    gem5 = params.fixtures[constants.gem5_binary_fixture_name].path
    command = [
        sys.executable,
        joinpath(absdirpath(__file__), "run_pcap_replay_tests.py"),
        "--gem5",
        gem5,
        "--source",
        config.base_dir,
    ]
    log_call(
        params.log,
        command,
        time=params.time,
        stdout=sys.stdout,
        stderr=sys.stderr,
    )


for scenario in ("cold_full_write", "partial_dirty_owner", "pvrdma_provenance"):
    cachelens_chi_dma_test(scenario)

# All normal NIC allocation ways, no stashing. Include old policies as controls.
for inclusion in ("mostly-exclusive", "inclusive", "noninclusive"):
    for scenario in ("nic_cpu_nic", "clean_private_victim", "hnf_evict_shared",
                     "partial_ss_sharers", "partial_dirty_owner"):
        cachelens_chi_dma_test(scenario, inclusion)
    for scenario in ("nic_cpu_nic", "clean_private_victim"):
        cachelens_chi_dma_test(scenario, inclusion, read_not_shared_dirty=True)
for scenario in ("cold_full_write", "pvrdma_provenance"):
    cachelens_chi_dma_test(scenario, "mostly-exclusive")
# dealloc alone must not change the historical ReadShared grant behavior.
cachelens_chi_dma_test("nic_cpu_nic", "noninclusive", dealloc=True)

gem5_verify_config(
    name="cachelens-arm-chi-inclusion-parameters",
    fixtures=(),
    verifiers=(verifier.MatchRegex(re.compile(
        "CACHELENS_CHI_INCLUSION_PARAMETERS_OK"
    )),),
    config=joinpath(absdirpath(__file__), "configs", "chi_inclusion_probe.py"),
    config_args=(),
    valid_isas=(constants.arm_tag,),
    valid_hosts=constants.supported_hosts,
    length=constants.quick_tag,
    protocol="CHI",
    gem5_fixture_factory=CacheLensExactProtocolFixture,
)


for scenario in (
    "cold_full_write", "stash_partial_cold", "partial_dirty_owner",
    "partial_ss_sharers", "hit_update", "stash_unclassified", "stash_clean_owner",
    "stash_replacement", "pvrdma_provenance",
):
    cachelens_chi_stash_test(scenario)
for scenario in ("cold_full_write", "stash_partial_cold", "partial_dirty_owner",
                 "stash_clean_owner"):
    cachelens_chi_stash_test(scenario, decline=True)
for scenario in ("cold_full_write", "stash_partial_cold", "stash_unclassified",
                 "stash_clean_owner",
                 "pvrdma_provenance"):
    cachelens_chi_stash_test(scenario, mode="off")


for scenario in ("cold_full_write", "stash_partial_cold", "partial_dirty_owner",
                 "stash_clean_owner", "pvrdma_provenance"):
    cachelens_chi_stash_test(scenario, inclusion="noninclusive")

for host in constants.supported_hosts:
    cachelens_chi_dma_mask_rejection_test(host)


for profile in ("arm-generic", "abstract"):
    for inclusion in ("noninclusive", "inclusive"):
        cachelens_arm_chi_profile_test(profile, inclusion)
cachelens_arm_chi_profile_test("intel-ddio")

for num_nics in range(4):
    cachelens_board_test(isa="arm", num_nics=num_nics)
cachelens_board_test(isa="arm", num_nics=4, expect_reject=True)
cachelens_board_test(isa="x86", num_nics=0)
cachelens_board_test(isa="x86", num_nics=1)
cachelens_board_test(isa="x86", num_nics=1, enable_pvrdma=True)
cachelens_board_test(isa="x86", num_nics=2, expect_reject=True)
cachelens_pvrdma_parser_test()
for host in constants.supported_hosts:
    cachelens_pvrdma_instantiate_test(host)

for protocol in ("CHI", "MESI_Two_Level"):
    cachelens_export_test(protocol)

cachelens_test(
    isa="x86",
    length=constants.quick_tag,
    max_ticks=1,
    cpu_type="timing",
    model_profile="abstract",
    hnf_inclusion="inclusive",
    coherence_protocol="mesi-two-level",
)
cachelens_test(
    isa="x86",
    length=constants.quick_tag,
    max_ticks=1,
    cpu_type="timing",
    model_profile="abstract",
    hnf_inclusion="inclusive",
    coherence_protocol="mesi-two-level",
    indexing_policy="splitmix64",
)
cachelens_test(
    isa="x86",
    length=constants.long_tag,
    cpu_type="timing",
    model_profile="abstract",
    hnf_inclusion="inclusive",
    coherence_protocol="mesi-two-level",
)

for isa in ("arm", "x86"):
    cachelens_test(
        isa=isa,
        length=constants.quick_tag,
        max_ticks=1,
        cpu_type="timing",
    )
    cachelens_test(
        isa=isa,
        length=constants.quick_tag,
        max_ticks=1,
        cpu_type="timing",
        model_profile=("arm-generic" if isa == "arm" else "x86-generic"),
    )
    cachelens_test(
        isa=isa,
        length=constants.quick_tag,
        max_ticks=1,
        cpu_type="timing",
        hnf_inclusion="inclusive",
    )
    cachelens_test(
        isa=isa,
        length=constants.quick_tag,
        max_ticks=1,
        cpu_type="o3",
    )
    cachelens_test(
        isa=isa,
        length=constants.long_tag,
        cpu_type="timing",
    )
    for host in constants.supported_hosts:
        cachelens_checkpoint_test(isa, host)
        if isa == "x86":
            cachelens_checkpoint_test(
                isa, host, coherence_protocol="mesi-two-level"
            )

for host in constants.supported_hosts:
    cachelens_protocol_rejection_test(
        "CHI", "mesi-two-level", "MESI_Two_Level", host
    )
    cachelens_protocol_rejection_test("MESI_Two_Level", "chi", "CHI", host)

    name = f"cachelens-pcap-replay-NULL-{host}-opt"
    TestSuite(
        name=name,
        fixtures=(Gem5Fixture(constants.null_tag, constants.opt_tag),),
        tests=(TestFunction(run_pcap_replay_tests, name=name),),
        tags=(
            constants.null_tag,
            constants.opt_tag,
            constants.quick_tag,
            host,
        ),
    )
