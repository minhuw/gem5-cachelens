# SPDX-License-Identifier: BSD-3-Clause
"""Eligible CHI requests need neither category tags nor a seqReq pointer."""
from m5.stats.gem5stats import get_simstat
from m5.tlm_chi import ReqOpcode, Size

from chi_tlm_test_utils import find_stat, schedule_read, schedule_write, vector_values

BASE = 0x80000000
FULL = bytes(range(64))
PARTIAL = bytes(16) + bytes(range(16)) + bytes(32)
ddio_eligible = [True]
ddio_way_part = 2
hnf_alloc_on_writeback = False
hnf_enable_dmt = False
nic_read_no_allocate = False


def test_generators(generators):
    (requestor,) = generators
    schedule_write(requestor, when=10, address=BASE, data=FULL, txn_id=1)
    schedule_read(requestor, when=1000, address=BASE, expected=FULL, txn_id=2)
    schedule_write(
        requestor, when=2000, address=BASE + 64 + 16, data=PARTIAL,
        txn_id=3, size=Size.SIZE_16, byte_enable=((1 << 16) - 1) << 16,
        opcode=ReqOpcode.WRITE_UNIQUE_PTL, data_ids=(0,),
    )
    schedule_read(requestor, when=3000, address=BASE + 64,
                  expected=PARTIAL, txn_id=4)
    for when, txn_id in ((4000, 5), (5000, 6)):
        schedule_read(requestor, when=when, address=BASE + 128,
                      expected=bytes(64), txn_id=txn_id)


def check(system):
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    fills = vector_values(find_stat(stats, "ddioWayFill"))
    accesses = vector_values(find_stat(stats, "ddioWayAccess"))
    assoc = len(fills) // 5
    untagged = fills[3 * assoc : 4 * assoc]
    assert sum(fills) == sum(untagged) == 2, fills
    assert all(v == 0 for v in untagged[ddio_way_part:]), untagged
    assert sum(accesses) == 2, accesses
    assert sum(vector_values(find_stat(stats, "nicDmaRequests"))) == 0
    assert sum(vector_values(find_stat(stats, "nicDmaMergeReads"))) == 0
    print("Untagged full/partial retention and read no-allocation passed")
