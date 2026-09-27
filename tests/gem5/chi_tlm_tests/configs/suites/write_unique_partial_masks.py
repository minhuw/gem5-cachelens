# SPDX-License-Identifier: BSD-3-Clause
"""Partial writes preserve cross-flit and sparse CHI byte enables."""

from m5.stats.gem5stats import get_simstat
from m5.tlm_chi import ReqOpcode, Size

from chi_tlm_test_utils import (
    find_stat,
    schedule_read,
    schedule_write,
    vector_values,
)


BASE = 0x80000000
ORIGINAL = bytes((0x21 + index * 5) & 0xFF for index in range(64))

CROSS_OFFSET = 24
CROSS_UPDATE = bytes(0xA0 + index for index in range(16))
CROSS_MASK = ((1 << len(CROSS_UPDATE)) - 1) << CROSS_OFFSET
CROSS_DATA = (
    bytes(CROSS_OFFSET)
    + CROSS_UPDATE
    + bytes(64 - CROSS_OFFSET - len(CROSS_UPDATE))
)

SPARSE_OFFSETS = (5, 19, 34, 58)
SPARSE_MASK = sum(1 << offset for offset in SPARSE_OFFSETS)
SPARSE_DATA = bytearray(64)
for index, offset in enumerate(SPARSE_OFFSETS):
    SPARSE_DATA[offset] = 0xD0 + index

EXPECTED = bytearray(ORIGINAL)
EXPECTED[CROSS_OFFSET : CROSS_OFFSET + len(CROSS_UPDATE)] = CROSS_UPDATE
for offset in SPARSE_OFFSETS:
    EXPECTED[offset] = SPARSE_DATA[offset]

ddio_eligible = [False, True, True, False]
hnf_alloc_on_writeback = False
ddio_way_part = 2


def test_generators(generators):
    seeder, cross_writer, sparse_writer, reader = generators
    schedule_write(
        seeder,
        when=10,
        address=BASE,
        data=ORIGINAL,
        txn_id=11,
    )
    schedule_write(
        cross_writer,
        when=1000,
        address=BASE + CROSS_OFFSET,
        data=CROSS_DATA,
        txn_id=37,
        size=Size.SIZE_16,
        byte_enable=CROSS_MASK,
        opcode=ReqOpcode.WRITE_UNIQUE_PTL,
        data_ids=(0, 2),
    )
    schedule_write(
        sparse_writer,
        when=2000,
        address=BASE + SPARSE_OFFSETS[0],
        data=bytes(SPARSE_DATA),
        txn_id=61,
        size=Size.SIZE_64,
        byte_enable=SPARSE_MASK,
        opcode=ReqOpcode.WRITE_UNIQUE_PTL,
        data_ids=(0, 2),
    )
    schedule_read(
        reader,
        when=3000,
        address=BASE,
        expected=bytes(EXPECTED),
        txn_id=79,
    )


def check(system):
    # Data bytes, masks, snoops and completion ordering are checked above.
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    allocations = vector_values(find_stat(stats, "ddioAllocWays"))
    assert sum(allocations) == 1, allocations
    assert all(v == 0 for v in allocations[ddio_way_part:]), allocations
    accesses = vector_values(find_stat(stats, "ddioWayAccess"))
    assert sum(accesses) == 1, accesses
    print("Untagged CHI data/coherence and aggregate accounting passed")
