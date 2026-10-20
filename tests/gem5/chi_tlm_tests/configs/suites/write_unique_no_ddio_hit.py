# SPDX-License-Identifier: BSD-3-Clause
"""A partial no-DDIO write-through deallocates an HNF hit."""

from m5.stats.gem5stats import get_simstat
from m5.tlm_chi import ReqOpcode, Size

from chi_tlm_test_utils import (
    find_stat,
    schedule_read,
    schedule_write,
    vector_values,
)


BASE = 0x80000000
OFFSET = 16
ORIGINAL = bytes((0x20 + index) & 0xFF for index in range(64))
UPDATE = bytes((0xE0 + index) & 0xFF for index in range(16))
REQUEST_DATA = ORIGINAL[:OFFSET] + UPDATE + ORIGINAL[OFFSET + 16 :]
EXPECTED = ORIGINAL[:OFFSET] + UPDATE + ORIGINAL[OFFSET + 16 :]

ddio_eligible = [False, True, False]
hnf_alloc_on_writeback = True
ddio_way_part = -1


def test_generators(generators):
    filler, writer, reader = generators
    schedule_write(
        filler,
        when=10,
        address=BASE,
        data=ORIGINAL,
        txn_id=1,
    )
    schedule_write(
        writer,
        when=1000,
        address=BASE + OFFSET,
        data=REQUEST_DATA,
        txn_id=2,
        size=Size.SIZE_16,
        byte_enable=((1 << 16) - 1) << OFFSET,
        opcode=ReqOpcode.WRITE_UNIQUE_PTL,
        data_ids=(0,),
    )
    schedule_read(
        reader,
        when=2000,
        address=BASE,
        expected=EXPECTED,
        txn_id=3,
    )


def check(system):
    # Data bytes, masks, snoops and completion ordering are checked above.
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    assert sum(vector_values(find_stat(stats, "ddioWayAccess"))) == 1
    assert sum(vector_values(find_stat(stats, "wayDeallocations"))) >= 1
    print("Untagged CHI data/coherence and aggregate accounting passed")
