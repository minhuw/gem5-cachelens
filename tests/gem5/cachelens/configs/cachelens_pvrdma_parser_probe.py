# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import contextlib
import importlib.util
import io
from pathlib import Path

repo = Path(__file__).parents[4]
config_path = repo / "configs/example/gem5_library/cachelens-fs.py"
spec = importlib.util.spec_from_file_location("cachelens_fs", config_path)
cachelens_fs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cachelens_fs)


def parse(isa, num_nics, enable_pvrdma):
    argv = [
        "--isa",
        isa,
        "--kernel",
        __file__,
        "--disk-image",
        __file__,
        "--num-nics",
        str(num_nics),
    ]
    if isa == "arm":
        argv += ["--bootloader", __file__]
    if enable_pvrdma:
        argv.append("--enable-pvrdma")
    parser = cachelens_fs._create_parser()
    args = parser.parse_args(argv)
    cachelens_fs._validate_args(parser, args)
    return args


def rejects(isa, num_nics):
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            parse(isa, num_nics, True)
        except SystemExit as error:
            assert error.code == 2
            return
    raise AssertionError("invalid PVRDMA configuration was accepted")


default = parse("x86", 1, False)
assert not default.enable_pvrdma
assert "enable_pvrdma" not in cachelens_fs._network_options(default)

opt_in = parse("x86", 1, True)
assert cachelens_fs._network_options(opt_in)["enable_pvrdma"]

rejects("arm", 1)
rejects("x86", 0)
rejects("x86", 2)
print("CACHELENS_PVRDMA_PARSER_OK")
