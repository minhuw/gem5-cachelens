# SPDX-License-Identifier: BSD-3-Clause
"""A full NIC payload write allocates only in enabled DDIO ways."""

from m5.stats.gem5stats import get_simstat

from chi_tlm_test_utils import (
    find_stat,
    schedule_read,
    schedule_write,
    vector_values,
)


ADDRESS = 0x80000000
WRITTEN = bytes((0x80 + index) & 0xFF for index in range(64))

nic_dma_categories = ["rdma_rx_payload", ""]
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
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    requests = find_stat(stats, "rxPayloadRequests")
    allocations = vector_values(find_stat(stats, "rxPayloadAllocWays"))
    generic_requests = vector_values(find_stat(stats, "nicDmaRequests"))
    requested_bytes = vector_values(
        find_stat(stats, "nicDmaRequestedBytes")
    )
    merge_reads = vector_values(find_stat(stats, "nicDmaMergeReads"))
    assert requests == 1, requests
    assert find_stat(stats, "rdmaRxPayloadRequests") == 1
    # Source 0 (RX payload), operation 1 (write_full): 0 * 3 + 1.
    assert generic_requests[1] == 1, generic_requests
    assert requested_bytes[1] == 64, requested_bytes
    assert merge_reads[1] == 0, merge_reads
    assert sum(allocations) == 1, allocations
    assert all(
        value == 0 for value in allocations[ddio_way_part:]
    ), allocations
    print("Enabled DDIO way allocation and data readback passed", allocations)
