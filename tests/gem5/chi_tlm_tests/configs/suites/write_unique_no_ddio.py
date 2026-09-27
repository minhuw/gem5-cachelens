# SPDX-License-Identifier: BSD-3-Clause
"""A cold full NIC write is write-through with DDIO disabled."""

from m5.stats.gem5stats import get_simstat

from chi_tlm_test_utils import (
    find_stat,
    schedule_read,
    schedule_write,
    vector_values,
)


ADDRESS = 0x80000000
WRITTEN = bytes((index * 3 + 1) & 0xFF for index in range(64))

ddio_eligible = [True, False]
hnf_alloc_on_writeback = True
ddio_way_part = -1


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
    assert sum(vector_values(find_stat(stats, "ddioWayFill"))) == 0
    assert sum(vector_values(find_stat(stats, "ddioWayAccess"))) == 0
    print("Untagged CHI data/coherence and aggregate accounting passed")
