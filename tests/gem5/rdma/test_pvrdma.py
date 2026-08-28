# SPDX-License-Identifier: BSD-3-Clause

import re

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
