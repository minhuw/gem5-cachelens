# Copyright (c) 2026 minhuw
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.stats.gem5stats import get_simstat
from m5.tlm_chi import *

classify_ddio = True
hnf_alloc_on_writeback = False
ddio_way_part = 2


def is_dbid(transaction):
    return (
        transaction.phase.channel == Channel.RSP
        and transaction.phase.opcode
        in (
            RspOpcode.DBID_RESP,
            RspOpcode.COMP_DBID_RESP,
        )
    )


def send_data(transaction):
    transaction.phase.channel = Channel.DAT
    transaction.phase.opcode = DatOpcode.NON_COPY_BACK_WR_DATA
    transaction.phase.data_id = 0
    transaction.inject()
    transaction.phase.data_id = 2
    transaction.inject()
    return False


def is_completion(transaction):
    return (
        transaction.phase.channel == Channel.RSP
        and transaction.phase.opcode
        in (
            RspOpcode.COMP,
            RspOpcode.COMP_DBID_RESP,
        )
    )


def test_all(generator):
    payload = TlmPayload()
    payload.address = 0x80000000
    payload.ns = True
    payload.size = Size.SIZE_64
    payload.data = bytes(range(64))
    payload.byte_enable = (1 << 64) - 1

    phase = TlmPhase()
    phase.channel = Channel.REQ
    phase.opcode = ReqOpcode.WRITE_UNIQUE_FULL
    phase.src_id = 0
    phase.tgt_id = 0
    phase.txn_id = 1

    transaction = generator.injectAt(10, payload, phase)
    transaction.ASSERT("is_dbid", is_dbid)
    transaction.ASSERT("is_completion", is_completion)
    transaction.DO(send_data)


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


def check(system):
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    requests = find_stat(stats, "rxPayloadRequests")
    allocations = find_stat(stats, "rxPayloadAllocWays")

    assert requests == 1, f"expected one RX data request, got {requests}"
    assert allocations is not None, "rxPayloadAllocWays statistic not found"
    allocation_counts = [
        value["value"] if isinstance(value, dict) else value
        for value in allocations.values()
    ]
    assert sum(allocation_counts) == 1, allocation_counts
    assert all(
        value == 0 for value in allocation_counts[ddio_way_part:]
    ), allocation_counts
    print("DDIO stats assertions passed:", requests, allocation_counts)
