# SPDX-License-Identifier: BSD-3-Clause
"""Generic CHI policy permits normal HNF allocation for NIC reads."""

from m5.stats.gem5stats import get_simstat
from m5.tlm_chi import Size

from chi_tlm_test_utils import (
    find_stat,
    schedule_read,
    vector_values,
)


ADDRESS = 0x80000000
EXPECTED = bytes(64)

nic_dma_categories = ["tx_payload_read"]
nic_read_no_allocate = False
hnf_enable_dmt = False
ddio_way_part = -1


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
        size=Size.SIZE_16,
        data_beats=1,
    )


def check(system):
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    fills = vector_values(find_stat(stats, "ddioWayFill"))
    accesses = vector_values(find_stat(stats, "ddioWayAccess"))
    assoc = len(fills) // 5
    tx_fills = fills[assoc : 2 * assoc]
    tx_accesses = accesses[assoc : 2 * assoc]
    requests = vector_values(find_stat(stats, "nicDmaRequests"))
    requested_bytes = vector_values(
        find_stat(stats, "nicDmaRequestedBytes")
    )
    assert sum(tx_fills) == 1, tx_fills
    assert sum(tx_accesses) == 1, tx_accesses
    assert requests[9] == 2, requests
    assert requested_bytes[9] == 80, requested_bytes
    # The direct TLM path preserves this size in seqReq. Real NIC requests
    # rely on the RNI forwarding that original seqReq even when CHI accSize is
    # expanded to a full block.
    print("Normal generic NIC-read allocation, size, and data checks passed")
