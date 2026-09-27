# SPDX-License-Identifier: BSD-3-Clause
"""A full eligible DMA write allocates only in enabled DDIO ways."""

from m5.stats.gem5stats import get_simstat

from chi_tlm_test_utils import (
    find_stat,
    schedule_read,
    schedule_write,
    vector_values,
)


ADDRESS = 0x80000000
WRITTEN = bytes((0x80 + index) & 0xFF for index in range(64))

ddio_eligible = [True, False]
hnf_alloc_on_writeback = False
ddio_way_part = 2


def test_generators(generators):
    writer, reader = generators
    schedule_write(
        writer,
        when=10,
        address=ADDRESS,
        data=WRITTEN,
        txn_id=1,
    )
    schedule_read(
        reader,
        when=1000,
        address=ADDRESS,
        expected=WRITTEN,
        txn_id=2,
    )


def check(system):
    # Data bytes, masks, snoops and completion ordering are checked above.
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    allocations = vector_values(find_stat(stats, "ddioAllocWays"))
    assert sum(allocations) == 1, allocations
    assert all(v == 0 for v in allocations[ddio_way_part:]), allocations
    print("Untagged CHI data/coherence and aggregate accounting passed")
