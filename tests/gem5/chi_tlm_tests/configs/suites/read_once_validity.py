# SPDX-License-Identifier: BSD-3-Clause
"""Regression for a valid ReadOnce completion-data response."""

from m5.tlm_chi import *


def _payload():
    payload = TlmPayload()
    payload.address = 0x80000000
    payload.ns = True
    payload.size = Size.SIZE_64
    return payload


def _phase():
    phase = TlmPhase()
    phase.channel = Channel.REQ
    phase.opcode = ReqOpcode.READ_ONCE
    phase.src_id = 0
    phase.tgt_id = 0
    phase.exp_comp_ack = True
    return phase


def _is_valid_comp_data(transaction):
    return (
        expect_equal(transaction.phase.channel, Channel.DAT)
        and expect_equal(transaction.phase.opcode, DatOpcode.COMP_DATA)
        and transaction.phase.resp != Resp.RESP_I
    )


def _send_comp_ack(transaction):
    transaction.phase.channel = Channel.RSP
    transaction.phase.opcode = RspOpcode.COMP_ACK
    transaction.inject()
    return False


def test_all(generator):
    transaction = generator.injectAt(10, _payload(), _phase())
    transaction.EXPECT(_is_valid_comp_data)
    transaction.DO(_send_comp_ack)
