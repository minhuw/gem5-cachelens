# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""Asset-free inclusion validation; no guest resources or hardware claims."""

import contextlib
import importlib.util
import io
from pathlib import Path

from gem5.prebuilt.cachelens.cache_hierarchy import CacheLensCHIHierarchy

repo = Path(__file__).parents[4]
spec = importlib.util.spec_from_file_location(
    "cachelens_fs", repo / "configs/example/gem5_library/cachelens-fs.py"
)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

for mode in ("inclusive", "noninclusive", "mostly-exclusive"):
    for dealloc in (False, True):
        hierarchy = CacheLensCHIHierarchy(
            hnf_inclusion=mode, dealloc_on_unique=dealloc,
            model_profile="abstract", hnf_assoc=4, ddio_way_part=4,
        )
        metadata = hierarchy.get_configuration()
        assert metadata["hnf_inclusion"] == mode
        assert metadata["ddio_way_part"] == 4
        assert metadata["cache_stashing"] == "off"
        assert metadata["hnf_dealloc_on_unique"] == (
            mode == "mostly-exclusive" or (mode == "noninclusive" and dealloc)
        )
        assert metadata["hnf_fwd_unique_on_readshared"] == (
            mode == "mostly-exclusive"
        )

for options, diagnostic in (
    ({"hnf_inclusion": "mostlyexclusive"}, "hnf_inclusion"),
    ({"hnf_inclusion": "exclusive"}, "hnf_inclusion"),
    ({"ddio_way_part": 0}, "ddio_way_part"),
    ({"ddio_way_part": 5}, "ddio_way_part"),
    ({"ddio_way_part": -2}, "ddio_way_part"),
):
    try:
        CacheLensCHIHierarchy(hnf_assoc=4, **options)
    except ValueError as error:
        assert diagnostic in str(error)
    else:
        raise AssertionError(f"Invalid hierarchy accepted: {options}")

# _validate_args runs before binary protocol selection: ARM_CHI can test
# rejection of x86 MESI options without constructing an x86 board or kernel.
for protocol in ("chi", "mesi-two-level", "mesi-three-level"):
    parser = runner._create_parser()
    args = parser.parse_args([
        "--isa", "x86", "--kernel", __file__, "--disk-image", __file__,
        "--coherence-protocol", protocol,
        "--hnf-inclusion", "mostly-exclusive",
        "--model-profile", "abstract", "--hnf-assoc", "4",
        "--ddio-way-part", "4",
    ])
    diagnostic = io.StringIO()
    with contextlib.redirect_stderr(diagnostic):
        try:
            runner._validate_args(parser, args)
        except SystemExit as error:
            assert protocol != "chi" and error.code == 2
            assert "inclusive by protocol" in diagnostic.getvalue()
            assert "--hnf-inclusion mostly-exclusive is invalid" in diagnostic.getvalue()
        else:
            assert protocol == "chi", "MESI accepted mostly-exclusive"

print("CACHELENS_CHI_INCLUSION_PARAMETERS_OK")
