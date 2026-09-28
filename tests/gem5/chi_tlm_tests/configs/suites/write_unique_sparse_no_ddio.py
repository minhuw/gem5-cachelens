# SPDX-License-Identifier: BSD-3-Clause
"""Noneligible sparse writes merge on a miss and preserve ordinary hit policy."""
from m5.stats.gem5stats import get_simstat
from m5.tlm_chi import ReqOpcode, Size
from chi_tlm_test_utils import (
    check_dma_lookups,
    find_stat, schedule_read, schedule_write, vector_values,
)

BASE = 0x80000000
ORIGINAL = bytes((0x21 + index * 5) & 0xFF for index in range(64))
OFFSETS = (5, 19, 34, 58)
MASK = sum(1 << offset for offset in OFFSETS)
DATA = bytes(0xA0 + index for index in range(64))
HIT_DATA = bytes(0x60 + index for index in range(64))
EXPECTED = bytearray(ORIGINAL)
HIT_EXPECTED = bytearray(ORIGINAL)
for offset in OFFSETS:
    EXPECTED[offset] = DATA[offset]
    HIT_EXPECTED[offset] = HIT_DATA[offset]

# Available DDIO ways must not opt an ordinary requestor into allocation.
ddio_eligible = [False]
ddio_way_part = 2
hnf_alloc_on_writeback = False
hnf_enable_dmt = False
completed = []


def read_complete(transaction):
    completed.append(transaction.phase.txn_id)
    return True


def test_generators(generators):
    (requestor,) = generators
    schedule_write(requestor, when=10, address=BASE, data=ORIGINAL, txn_id=1)
    schedule_write(requestor, when=1000, address=BASE, data=DATA, txn_id=2,
                   size=Size.SIZE_64, byte_enable=MASK,
                   opcode=ReqOpcode.WRITE_UNIQUE_PTL, data_ids=(0, 2))
    schedule_read(requestor, when=2000, address=BASE, expected=bytes(EXPECTED),
                  txn_id=3).DO(read_complete)
    # The read allocated an ordinary HNF line. A sparse hit still updates it
    # locally; the merge/write-through fix must not force its deallocation.
    schedule_write(requestor, when=3000, address=BASE, data=HIT_DATA, txn_id=4,
                   size=Size.SIZE_64, byte_enable=MASK,
                   opcode=ReqOpcode.WRITE_UNIQUE_PTL, data_ids=(0, 2))
    schedule_read(requestor, when=4000, address=BASE,
                  expected=bytes(HIT_EXPECTED), txn_id=5).DO(read_complete)


def check(system):
    assert completed == [3, 5], completed
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    check_dma_lookups(stats, 0, 0, 0, 0)
    assert find_stat(stats, "m_demand_misses") == 3
    assert find_stat(stats, "m_demand_hits") == 2
    for name in ("ddioWayFill", "ddioWayAccess", "wayDeallocations"):
        assert sum(vector_values(find_stat(stats, name))) == 0, name
    assert find_stat(stats, "writeStashRequests") == 0
    print("Noneligible sparse miss/hit bytes, completion and allocation passed")
