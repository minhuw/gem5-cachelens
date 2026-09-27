# SPDX-License-Identifier: BSD-3-Clause
"""An eligible untagged WriteUniqueZero is write-through when disabled."""

from m5.stats.gem5stats import get_simstat
from m5.tlm_chi import ReqOpcode

from chi_tlm_test_utils import (
    find_stat,
    is_completion,
    is_dbid,
    payload,
    phase,
    schedule_read,
    schedule_write,
    vector_values,
)


ADDRESS = 0x80000000
ORIGINAL = bytes((0x70 + index) & 0xFF for index in range(64))

ddio_eligible = [True, False, True, False]
hnf_alloc_on_writeback = True
hnf_enable_dmt = False
ddio_way_part = -1


def test_generators(generators):
    seeder, cache_reader, writer, reader = generators
    # Seed backing memory through the no-DDIO write-through path, then fetch
    # the distinctive line into the HNF so WriteUniqueZero also exercises hit
    # invalidation and deallocation.
    schedule_write(
        seeder,
        when=10,
        address=ADDRESS,
        data=ORIGINAL,
        txn_id=1,
    )
    schedule_read(
        cache_reader,
        when=1000,
        address=ADDRESS,
        expected=ORIGINAL,
        txn_id=2,
    )

    transaction = writer.injectAt(
        2000,
        payload(ADDRESS),
        phase(ReqOpcode.WRITE_UNIQUE_ZERO, 3),
    )
    transaction.ASSERT("zero DBID", is_dbid)
    transaction.ASSERT("zero completion", is_completion)

    schedule_read(
        reader,
        when=3000,
        address=ADDRESS,
        expected=bytes(64),
        txn_id=4,
    )


def check(system):
    # Data bytes, masks, snoops and completion ordering are checked above.
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    assert sum(vector_values(find_stat(stats, "wayDeallocations"))) >= 1
    print("Untagged CHI data/coherence and aggregate accounting passed")
