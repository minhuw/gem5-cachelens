# SPDX-License-Identifier: BSD-3-Clause
"""A partial descriptor write merges clean memory into a DDIO line."""

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
ORIGINAL = bytes((0x31 + index * 7) & 0xFF for index in range(64))
UPDATE = bytes(0xD0 + index for index in range(16))
REQUEST_DATA = bytes(OFFSET) + UPDATE + bytes(64 - OFFSET - len(UPDATE))
EXPECTED = ORIGINAL[:OFFSET] + UPDATE + ORIGINAL[OFFSET + len(UPDATE) :]

nic_dma_categories = ["", "rx_desc_writeback", ""]
ddio_eligible = [False, True, False]
hnf_alloc_on_writeback = False
ddio_way_part = 2


def test_generators(generators):
    seeder, writer, reader = generators
    # This unclassified, no-allocation full write seeds backing memory without
    # leaving an HNF copy for the partial DDIO write to merge against.
    schedule_write(
        seeder,
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
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    allocations = vector_values(find_stat(stats, "ddioAllocWays"))
    requests = vector_values(find_stat(stats, "nicDmaRequests"))
    requested_bytes = vector_values(
        find_stat(stats, "nicDmaRequestedBytes")
    )
    merge_reads = vector_values(find_stat(stats, "nicDmaMergeReads"))
    merge_read_bytes = vector_values(
        find_stat(stats, "nicDmaMergeReadBytes")
    )
    # Source 2 (RX descriptor), operation 2 (write_partial): 2 * 3 + 2.
    assert requests[8] == 1, requests
    assert requested_bytes[8] == 16, requested_bytes
    assert merge_reads[8] == 1, merge_reads
    assert merge_read_bytes[8] == 64, merge_read_bytes
    assert sum(allocations) == 1, allocations
    assert all(
        value == 0 for value in allocations[ddio_way_part:]
    ), allocations
    print("Partial clean-memory DDIO merge passed", allocations)
