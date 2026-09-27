# SPDX-License-Identifier: BSD-3-Clause
"""Explicit full/partial stash opcodes, ordinary ways, no semantic metadata."""
from m5.stats.gem5stats import get_simstat
from m5.tlm_chi import ReqOpcode, Size
from chi_tlm_test_utils import find_stat, schedule_read, schedule_write

BASE = 0x80000000
FULL = bytes(range(64))
PARTIAL = bytes(16) + bytes(range(16)) + bytes(32)
ORIGINAL = bytes((0x21 + index * 5) & 0xFF for index in range(64))
SPARSE_OFFSETS = (5, 19, 34, 58)
SPARSE_MASK = sum(1 << offset for offset in SPARSE_OFFSETS)
SPARSE_DATA = bytes((0xA0 + index) & 0xFF for index in range(64))
EXPECTED = bytearray(ORIGINAL)
for offset in SPARSE_OFFSETS:
    EXPECTED[offset] = SPARSE_DATA[offset]

ddio_eligible = [False]
ddio_way_part = -1
hnf_alloc_on_writeback = False
hnf_enable_dmt = False
accept_write_stash = True
completed_reads = []


def read_complete(transaction):
    completed_reads.append(transaction.phase.txn_id)
    return True


def test_generators(generators):
    (requestor,) = generators
    schedule_write(requestor, when=10, address=BASE, data=FULL, txn_id=1,
                   opcode=ReqOpcode.WRITE_UNIQUE_FULL_STASH)
    schedule_write(requestor, when=1000, address=BASE + 80, data=PARTIAL,
                   txn_id=2, size=Size.SIZE_16,
                   byte_enable=((1 << 16) - 1) << 16,
                   opcode=ReqOpcode.WRITE_UNIQUE_PTL_STASH, data_ids=(0,))
    schedule_read(requestor, when=2000, address=BASE,
                  expected=FULL, txn_id=3).DO(read_complete)
    schedule_read(requestor, when=3000, address=BASE + 64,
                  expected=PARTIAL, txn_id=4).DO(read_complete)

    # Seed nonzero bytes, then update disjoint bytes in both data flits. Disabled
    # bytes deliberately differ from the seed, so accidental overwrites fail.
    schedule_write(requestor, when=4000, address=BASE + 128, data=ORIGINAL,
                   txn_id=5)
    schedule_write(requestor, when=5000, address=BASE + 128,
                   data=SPARSE_DATA, txn_id=6, size=Size.SIZE_64,
                   byte_enable=SPARSE_MASK,
                   opcode=ReqOpcode.WRITE_UNIQUE_PTL_STASH, data_ids=(0, 2))
    schedule_read(requestor, when=6000, address=BASE + 128,
                  expected=bytes(EXPECTED), txn_id=7).DO(read_complete)


def check(system):
    assert sorted(completed_reads) == [3, 4, 7], completed_reads
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    assert find_stat(stats, "writeStashRequests") == 3
    assert find_stat(stats, "writeStashAccepted") == 3
    assert find_stat(stats, "writeStashDeclined") == 0
    assert find_stat(stats, "writeStashAllocations") == 3
    print("Explicit CHI full/contiguous/sparse stash data and allocation passed")
