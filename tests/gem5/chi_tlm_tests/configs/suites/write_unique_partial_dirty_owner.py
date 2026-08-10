# SPDX-License-Identifier: BSD-3-Clause
"""A descriptor write merges with a dirty upstream owner's full line."""

from m5.stats.gem5stats import get_simstat
from m5.tlm_chi import (
    Channel,
    DatOpcode,
    ReqOpcode,
    Resp,
    Size,
    SnpOpcode,
)

from chi_tlm_test_utils import (
    FULL_ENABLE,
    find_stat,
    is_comp_data,
    payload,
    phase,
    schedule_read,
    schedule_write,
    send_comp_ack,
    vector_values,
    wait,
)


BASE = 0x80000000
OFFSET = 16
TXN_ID = 7
DIRTY_OWNER = bytes((0x40 + index * 5) & 0xFF for index in range(64))
UPDATE = bytes((0xA0 + index) & 0xFF for index in range(16))
REQUEST_DATA = bytes(OFFSET) + UPDATE + bytes(64 - OFFSET - len(UPDATE))
EXPECTED = DIRTY_OWNER[:OFFSET] + UPDATE + DIRTY_OWNER[OFFSET + 16 :]

nic_dma_categories = ["", "rx_desc_writeback", ""]
hnf_alloc_on_writeback = False
ddio_way_part = 2


def _is_clean_invalid_snoop(transaction):
    return (
        transaction.phase.channel == Channel.SNP
        and transaction.phase.opcode == SnpOpcode.SNP_CLEAN_INVALID
    )


def _send_dirty_owner_data(owner_payload):
    def send(transaction):
        owner_payload.data = DIRTY_OWNER
        owner_payload.byte_enable = FULL_ENABLE
        transaction.phase.channel = Channel.DAT
        transaction.phase.opcode = DatOpcode.SNP_RESP_DATA
        transaction.phase.resp = Resp.RESP_I_PD
        for data_id in (0, 2):
            transaction.phase.data_id = data_id
            transaction.inject()
        return False

    return send


def _schedule_dirty_owner(generator):
    owner_payload = payload(BASE)
    transaction = generator.injectAt(
        10,
        owner_payload,
        phase(ReqOpcode.READ_UNIQUE, TXN_ID, exp_comp_ack=True),
    )
    transaction.ASSERT("owner first data", is_comp_data)
    transaction.DO_WAIT(wait)
    transaction.ASSERT("owner second data", is_comp_data)
    transaction.ASSERT(
        "owner initial memory data",
        lambda unused: bytes(owner_payload.data) == bytes(64),
    )
    transaction.DO(send_comp_ack)
    transaction.DO_WAIT(wait)
    transaction.ASSERT("dirty owner snooped", _is_clean_invalid_snoop)
    transaction.DO(_send_dirty_owner_data(owner_payload))


def test_generators(generators):
    owner, writer, reader = generators
    _schedule_dirty_owner(owner)
    schedule_write(
        writer,
        when=1000,
        address=BASE + OFFSET,
        data=REQUEST_DATA,
        txn_id=TXN_ID,
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
        txn_id=8,
    )


def check(system):
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    allocations = vector_values(find_stat(stats, "ddioAllocWays"))
    assert sum(allocations) == 1, allocations
    assert all(
        value == 0 for value in allocations[ddio_way_part:]
    ), allocations
    print("Partial dirty-owner merge and coherent data readback passed")
