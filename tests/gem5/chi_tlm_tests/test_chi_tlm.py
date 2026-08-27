# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Focused CHI protocol tests driven through the Arm CHI-TLM bridge."""

from testlib import *


TEST_CONFIG = joinpath(
    absdirpath(__file__),
    "configs",
    "ruby_mem_test.py",
)
SUITE_DIR = joinpath(absdirpath(__file__), "configs", "suites")

# Suite name, generator count, and whether the HNF returns standalone DBIDResp
# and Comp messages. TLM integration is built only for Arm, and these tests
# require a CHI Ruby build.
SUITES = (
    (
        "dma-partial-write-cross-slice",
        "dma_partial_write_cross_slice.py",
        2,
        False,
    ),
    ("nic-read-no-allocation", "nic_read_no_allocation.py", 1, False),
    ("nic-read-normal-allocation", "nic_read_normal_allocation.py", 1, False),
    ("write-unique-desc-ddio", "write_unique_desc_ddio.py", 3, False),
    ("write-unique-full-ddio", "write_unique_full_ddio.py", 2, False),
    (
        "write-unique-full-ddio-separate-comp",
        "write_unique_full_ddio.py",
        2,
        True,
    ),
    ("write-unique-no-ddio", "write_unique_no_ddio.py", 2, False),
    ("write-unique-no-ddio-hit", "write_unique_no_ddio_hit.py", 3, False),
    (
        "write-unique-partial-dirty-owner",
        "write_unique_partial_dirty_owner.py",
        3,
        False,
    ),
    (
        "write-unique-partial-masks",
        "write_unique_partial_masks.py",
        4,
        False,
    ),
    (
        "write-unique-zero-no-ddio",
        "write_unique_zero_no_ddio.py",
        4,
        False,
    ),
)

for name, suite, num_cpus, separate_comp in SUITES:
    config_args = [
        joinpath(SUITE_DIR, suite),
        f"--num-cpus={num_cpus}",
        "--abs-max-tick=10000",
    ]
    if separate_comp:
        config_args.append("--hnf-comp-wu")

    gem5_verify_config(
        name=f"chi-tlm-{name}",
        fixtures=(),
        verifiers=(),
        config=TEST_CONFIG,
        config_args=config_args,
        valid_isas=(constants.arm_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
        protocol="CHI",
    )
