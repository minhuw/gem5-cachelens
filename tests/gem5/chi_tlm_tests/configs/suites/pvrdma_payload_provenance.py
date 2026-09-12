# SPDX-License-Identifier: BSD-3-Clause
"""PVRDMA payload provenance overlays existing CHI DDIO classification."""

from m5.stats.gem5stats import get_simstat

from chi_tlm_test_utils import (
    find_stat,
    schedule_read,
    schedule_write,
    vector_values,
)


ADDRESS = 0x80000000
WRITTEN = bytes((0x40 + index) & 0xFF for index in range(64))

nic_dma_categories = ["rdma_rx_payload", "rdma_tx_payload_read", ""]
hnf_alloc_on_writeback = False
ddio_way_part = 2


def test_generators(generators):
    writer, rdma_reader, cpu_reader = generators
    schedule_write(writer, when=10, address=ADDRESS, data=WRITTEN, txn_id=1)
    schedule_read(
        rdma_reader,
        when=1000,
        address=ADDRESS,
        expected=WRITTEN,
        txn_id=2,
    )
    schedule_read(
        cpu_reader,
        when=2000,
        address=ADDRESS,
        expected=WRITTEN,
        txn_id=3,
    )


def check(system):
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    assert find_stat(stats, "rxPayloadRequests") == 1
    assert find_stat(stats, "rxPayloadHits") == 0
    assert find_stat(stats, "rxPayloadMisses") == 1
    assert find_stat(stats, "rdmaRxPayloadRequests") == 1
    assert find_stat(stats, "rdmaRxPayloadHits") == 0
    assert find_stat(stats, "rdmaRxPayloadMisses") == 1
    assert find_stat(stats, "rdmaRxPayloadUniqueLines") == 1
    assert find_stat(stats, "txPayloadRequests") == 1
    assert find_stat(stats, "txPayloadHits") == 1
    assert find_stat(stats, "txPayloadMisses") == 0
    assert find_stat(stats, "rdmaTxPayloadRequests") == 1
    assert find_stat(stats, "rdmaTxPayloadHits") == 1
    assert find_stat(stats, "rdmaTxPayloadMisses") == 0
    assert find_stat(stats, "rdmaTxPayloadUniqueLines") == 1
    assert sum(
        vector_values(find_stat(stats, "rdmaRxPayloadCpuAccessWays"))
    ) == 1
    assert sum(
        vector_values(find_stat(stats, "rdmaRxPayloadCpuFillWays"))
    ) == 0
    assert find_stat(stats, "rdmaRxPayloadCpuUniqueLines") == 1
    print("PVRDMA CHI aggregate and dedicated provenance passed")
