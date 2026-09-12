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

nic_dma_categories = ["rx_payload", ""]
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
    assert requests == 1, requests
    assert sum(allocations) == 1, allocations
    assert all(
        value == 0 for value in allocations[ddio_way_part:]
    ), allocations
    print("Enabled DDIO way allocation and data readback passed", allocations)
