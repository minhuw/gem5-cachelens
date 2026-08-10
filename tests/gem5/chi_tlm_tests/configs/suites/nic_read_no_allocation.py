# SPDX-License-Identifier: BSD-3-Clause
"""Intel-DDIO-style policy prevents HNF allocation for NIC reads."""

from m5.stats.gem5stats import get_simstat

from chi_tlm_test_utils import find_stat, schedule_read, vector_values


ADDRESS = 0x80000000
EXPECTED = bytes(64)

nic_dma_categories = ["tx_payload_read"]
nic_read_no_allocate = True
hnf_enable_dmt = False
ddio_way_part = 2


def test_generators(generators):
    (reader,) = generators
    schedule_read(
        reader,
        when=10,
        address=ADDRESS,
        expected=EXPECTED,
        txn_id=1,
    )
    schedule_read(
        reader,
        when=1000,
        address=ADDRESS,
        expected=EXPECTED,
        txn_id=2,
    )


def check(system):
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    fills = vector_values(find_stat(stats, "ddioWayFill"))
    accesses = vector_values(find_stat(stats, "ddioWayAccess"))
    assoc = len(fills) // 5
    tx_fills = fills[assoc : 2 * assoc]
    tx_accesses = accesses[assoc : 2 * assoc]
    assert sum(tx_fills) == 0, tx_fills
    assert sum(tx_accesses) == 0, tx_accesses
    print("NIC-read no-allocation policy and data checks passed")
