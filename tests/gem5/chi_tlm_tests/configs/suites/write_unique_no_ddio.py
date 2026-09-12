# SPDX-License-Identifier: BSD-3-Clause
"""A cold full NIC write is write-through with DDIO disabled."""

from m5.stats.gem5stats import get_simstat

from chi_tlm_test_utils import (
    find_stat,
    schedule_read,
    schedule_write,
    vector_values,
)


ADDRESS = 0x80000000
WRITTEN = bytes((index * 3 + 1) & 0xFF for index in range(64))

nic_dma_categories = ["rx_payload", ""]
hnf_alloc_on_writeback = True
ddio_way_part = -1


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
    fills = vector_values(find_stat(stats, "ddioWayFill"))
    assoc = len(fills) // 5
    payload_fills = fills[:assoc]
    cpu_accesses = vector_values(find_stat(stats, "ddioWayAccess"))[
        3 * assoc : 4 * assoc
    ]
    assert sum(payload_fills) == 0, payload_fills
    assert sum(cpu_accesses) == 0, cpu_accesses
    print("Cold no-DDIO write-through data and retention checks passed")
