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

LLC_REPLACEMENT_POLICIES = ("LRURP", "TreePLRURP", "BRRIPRP", "RandomRP")


def llc_replacement_policy(name):
    """Return a fresh native replacement policy of type name for one LLC bank."""
    from m5.objects import (
        BRRIPRP,
        LRURP,
        RandomRP,
        TreePLRURP,
    )

    if name not in LLC_REPLACEMENT_POLICIES:
        choices = ", ".join(LLC_REPLACEMENT_POLICIES)
        raise ValueError(f"hnf_replacement must be one of {choices}.")
    if name == "BRRIPRP":
        # Bimodal RRIP (Jaleel et al., ISCA 2010): 2-bit RRPVs, a miss
        # inserts at distant RRPV except with 3% (~1/32) probability at
        # long RRPV, and a hit resets the RRPV to 0 (hit priority).
        return BRRIPRP(num_bits=2, btp=3, hit_priority=True)
    return {"LRURP": LRURP, "TreePLRURP": TreePLRURP, "RandomRP": RandomRP}[
        name
    ]()
