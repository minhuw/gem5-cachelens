# SPDX-License-Identifier: BSD-3-Clause

"""Small helpers for focused CHI-TLM data/coherence regressions."""

from m5.tlm_chi import *


LINE_SIZE = 64
FULL_ENABLE = (1 << LINE_SIZE) - 1


def payload(address, data=None, size=Size.SIZE_64, byte_enable=FULL_ENABLE):
    value = TlmPayload()
    value.address = address
    value.ns = True
    value.size = size
    value.data = bytes(LINE_SIZE) if data is None else bytes(data)
    value.byte_enable = byte_enable
    return value


def phase(opcode, txn_id, *, exp_comp_ack=False):
    value = TlmPhase()
    value.channel = Channel.REQ
    value.opcode = opcode
    value.src_id = 0
    value.tgt_id = 0
    value.txn_id = txn_id
    value.exp_comp_ack = exp_comp_ack
    return value


def is_dbid(transaction):
    return (
        transaction.phase.channel == Channel.RSP
        and transaction.phase.opcode
        in (RspOpcode.DBID_RESP, RspOpcode.COMP_DBID_RESP)
    )


def is_completion(transaction):
    return (
        transaction.phase.channel == Channel.RSP
        and transaction.phase.opcode
        in (RspOpcode.COMP, RspOpcode.COMP_DBID_RESP)
    )


def is_comp_data(transaction):
    return (
        transaction.phase.channel == Channel.DAT
        and transaction.phase.opcode
        in (DatOpcode.COMP_DATA, DatOpcode.DATA_SEP_RESP)
    )


def send_write_data(data_ids):
    def send(transaction):
        combined_completion = is_completion(transaction)
        if not combined_completion:
            transaction.ASSERT("write completion", is_completion)

        transaction.phase.channel = Channel.DAT
        transaction.phase.opcode = DatOpcode.NON_COPY_BACK_WR_DATA
        for data_id in data_ids:
            transaction.phase.data_id = data_id
            transaction.inject()
        return False

    return send


def send_comp_ack(transaction):
    transaction.phase.channel = Channel.RSP
    transaction.phase.opcode = RspOpcode.COMP_ACK
    transaction.inject()
    return False


def wait(transaction):
    return True


def schedule_write(
    generator,
    *,
    when,
    address,
    data,
    txn_id,
    size=Size.SIZE_64,
    byte_enable=FULL_ENABLE,
    opcode=ReqOpcode.WRITE_UNIQUE_FULL,
    data_ids=(0, 2),
):
    write_payload = payload(address, data, size, byte_enable)
    transaction = generator.injectAt(
        when, write_payload, phase(opcode, txn_id)
    )
    transaction.ASSERT("write DBID", is_dbid)
    transaction.DO(send_write_data(data_ids))
    # A combined CompDBIDResp completes on the first response. For a standalone
    # DBIDResp, send_write_data appends an assertion for the later Comp before
    # this wait point is consumed.
    transaction.DO_WAIT(wait)
    return transaction


def schedule_read(
    generator,
    *,
    when,
    address,
    expected,
    txn_id,
    opcode=ReqOpcode.READ_ONCE,
):
    read_payload = payload(address)
    transaction = generator.injectAt(
        when,
        read_payload,
        phase(opcode, txn_id, exp_comp_ack=True),
    )
    transaction.ASSERT("read first data", is_comp_data)
    transaction.DO_WAIT(wait)
    transaction.ASSERT("read second data", is_comp_data)
    transaction.ASSERT(
        "read merged line",
        lambda unused: bytes(read_payload.data) == bytes(expected),
    )
    transaction.DO(send_comp_ack)
    return transaction


def find_stat(node, name):
    if isinstance(node, dict):
        for key, value in node.items():
            if key == name:
                return value["value"]
            found = find_stat(value, name)
            if found is not None:
                return found
    elif isinstance(node, list):
        for value in node:
            found = find_stat(value, name)
            if found is not None:
                return found
    return None


def vector_values(vector):
    return [
        value["value"] if isinstance(value, dict) else value
        for value in vector.values()
    ]
