# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause
"""Exact accepted HNF states for the native directed CPU/DMA fixture."""

from line_state_checks import expected_marginals

_I = ("CHI_I", False, False, False)
_UD = ("CHI_UD", True, True, False)
_UC = ("CHI_UC", True, False, False)
_RU = ("CHI_RU", False, False, True)
_UD_RU = ("CHI_UD_RU", True, True, True)
_UC_RSC = ("CHI_UC_RSC", True, False, False)
_UD_RSD = ("CHI_UD_RSD", True, True, True)
_RUSD = ("CHI_RUSD", False, False, True)


def directed_marginals(scenario, inclusion, ddio, read_not_shared_dirty=False,
                       dealloc_on_unique=False):
    """Keep owner existence independent of resident tag and dirty data.

    The generic seed in the partial fixtures writes back dirty HNF data.
    A mostly-exclusive unique grant removes the tag but retains the owner.
    Eligible writes subsequently leave UD when retained, I when disabled.
    """
    after_write = _UD if ddio > 0 else _I
    unique_dealloc = inclusion == "mostly-exclusive" or (
        inclusion == "noninclusive" and dealloc_on_unique
    )
    dirty_owner = _RU if unique_dealloc else _UD_RU
    if scenario == "cold_full_write":
        return expected_marginals(write=[_I])
    if scenario == "hit_update":
        return expected_marginals(read=[after_write], write=[_I, after_write])
    if scenario == "tx_miss_no_allocate":
        return expected_marginals(read=[_I, _I])
    if scenario == "partial_dirty_owner":
        return expected_marginals(read=[after_write], write=[dirty_owner])
    if scenario == "disabled_ddio":
        return expected_marginals(read=[_I] * 3, write=[_I, dirty_owner])
    if scenario == "eligible_untagged":
        return expected_marginals(read=[_UD] * 4 + [_I] * 3,
                                  write=[_I, _UD, _I])
    if scenario == "noneligible_dma":
        # Generic read allocates clean data even with NIC no-allocation set.
        return expected_marginals(read=[_UC])
    if scenario == "partial_ss_sharers":
        # Shared dirty CHI data retains an explicit upstream owner. MESI SS
        # has sharers without an owner; the two protocols must not be merged.
        return expected_marginals(read=[after_write], write=[_UD_RSD])
    if scenario == "nic_cpu_nic":
        if inclusion == "mostly-exclusive":
            after_cpu = _RU
        else:
            after_cpu = _UD_RU if read_not_shared_dirty else _UD_RSD
        return expected_marginals(read=[after_write], write=[_I, after_cpu])
    if scenario == "clean_private_victim":
        # Clean private eviction inserts UC; a new shared reader leaves
        # UC_RSC with no explicit owner except for the mostly-exclusive grant.
        after_cpu = _RU if inclusion == "mostly-exclusive" else _UC_RSC
        return expected_marginals(read=[_UC, after_write], write=[after_cpu])
    if scenario == "hnf_evict_shared":
        # A noninclusive eviction removes the tag while preserving directory
        # ownership. Inclusive eviction invalidates private copies as well.
        after_evict = _I if inclusion == "inclusive" else _RUSD
        return expected_marginals(read=[after_evict], write=[_I] * 4)
    raise ValueError(f"No exact CHI directed expectations for {scenario}")


def tlm_marginals(suite):
    """Exact one-HNF TLM fixture expectations, independent of native CPU tests."""
    if suite in ("write_stash_explicit", "write_stash_declined",
                 "write_unique_sparse_no_ddio", "nic_read_normal_allocation",
                 "dma_partial_write_cross_slice"):
        return expected_marginals()
    if suite in ("write_unique_full_ddio", "write_unique_no_ddio",
                 "write_unique_desc_ddio"):
        return expected_marginals(write=[_I])
    if suite == "write_unique_partial_dirty_owner":
        # Noneligible ReadUnique grants a private owner; no resident HNF tag.
        return expected_marginals(write=[_RU])
    if suite == "write_unique_no_ddio_hit":
        return expected_marginals(write=[_UD])
    if suite == "write_unique_partial_masks":
        return expected_marginals(write=[_I, _UD])
    if suite == "nic_read_no_allocation":
        return expected_marginals(read=[_I, _I])
    if suite == "untagged_ddio":
        return expected_marginals(read=[_UD, _UD, _I, _I], write=[_I, _I])
    if suite == "write_unique_zero_no_ddio":
        return expected_marginals(write=[_I, _UC])
    return None
