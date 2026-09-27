# SPDX-License-Identifier: BSD-3-Clause
"""Declining an explicit stash hint must still publish every written byte."""
from pathlib import Path
import runpy
globals().update(runpy.run_path(str(Path(__file__).with_name("write_stash_explicit.py"))))
accept_write_stash = False


def check(system):
    assert sorted(completed_reads) == [3, 4, 7], completed_reads
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    assert find_stat(stats, "writeStashRequests") == 3
    assert find_stat(stats, "writeStashAccepted") == 0
    assert find_stat(stats, "writeStashDeclined") == 3
    assert find_stat(stats, "writeStashAllocations") == 0
    print("Explicit CHI full/contiguous/sparse stash decline and data passed")
