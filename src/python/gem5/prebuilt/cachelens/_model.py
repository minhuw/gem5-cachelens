# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Shared CacheLens model-profile metadata."""

PROFILE_ALIASES = {
    "x86": "x86-generic",
    "arm": "arm-generic",
    "intel_ddio": "intel-ddio",
    "x86_intel_ddio": "intel-ddio",
}

# These are intentionally descriptive model profiles, not claims that the
# resulting configuration is calibrated to a particular commercial CPU.
MODEL_PROFILES = {
    "abstract": {
        "description": "architecture-neutral abstract coherence model",
        "default_ddio_way_part": -1,
        "nic_read_no_allocate": False,
    },
    "x86-generic": {
        "description": "generic x86 coherence model (DDIO disabled by default)",
        "default_ddio_way_part": -1,
        "nic_read_no_allocate": False,
    },
    "arm-generic": {
        "description": "generic ARM coherence model (DDIO disabled by default)",
        "default_ddio_way_part": -1,
        "nic_read_no_allocate": False,
    },
    "intel-ddio": {
        "description": "experimental Intel-style DDIO model",
        "default_ddio_way_part": 2,
        "nic_read_no_allocate": True,
    },
}
