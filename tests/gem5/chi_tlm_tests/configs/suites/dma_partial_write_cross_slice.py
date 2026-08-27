# SPDX-License-Identifier: BSD-3-Clause
"""DMA partial write spanning two aligned CHI data-channel slices."""

from m5.tlm_chi import ReqOpcode

from chi_tlm_test_utils import (
    is_comp_data,
    payload,
    phase,
    schedule_write,
    send_comp_ack,
    wait,
)


BASE = 0x80000000
OFFSET = 24
WRITE_SIZE = 16
ORIGINAL = bytes(0x80 + index for index in range(64))

# Add one real timing requester. CHI.create_system wires its port through the
# SLICC CHI_RNI_DMA controller, while the TLM requesters only seed and inspect
# the target line.
dma_generator_count = 1
read_completed = False


def _schedule_checked_read(generator, when, txn_id):
    read_payload = payload(BASE)
    transaction = generator.injectAt(
        when,
        read_payload,
        phase(ReqOpcode.READ_ONCE, txn_id, exp_comp_ack=True),
    )
    transaction.ASSERT("read first data", is_comp_data)
    transaction.DO_WAIT(wait)
    transaction.ASSERT("read second data", is_comp_data)

    def check_data(unused):
        global read_completed
        data = bytes(read_payload.data)
        untouched = range(0, OFFSET), range(OFFSET + WRITE_SIZE, 64)
        for byte_range in untouched:
            if any(data[index] != ORIGINAL[index] for index in byte_range):
                return False

        update = data[OFFSET : OFFSET + WRITE_SIZE]
        if len(set(update)) != 1:
            return False
        if update == ORIGINAL[OFFSET : OFFSET + WRITE_SIZE]:
            return False

        read_completed = True
        return True

    transaction.ASSERT("DMA write and untouched bytes", check_data)
    transaction.DO(send_comp_ack)


def test_generators(generators):
    seeder, reader = generators
    schedule_write(
        seeder,
        when=10,
        address=BASE,
        data=ORIGINAL,
        txn_id=13,
    )
    _schedule_checked_read(reader, when=3000, txn_id=71)


def _dma_traffic(generator):
    # Allow the TLM seeder to establish the original line, then issue exactly
    # one 16-byte timing write at line offset 24.
    yield generator.createIdle(1000)
    yield generator.createLinear(
        100,
        BASE + OFFSET,
        BASE + OFFSET + WRITE_SIZE,
        WRITE_SIZE,
        1,
        1,
        0,
        WRITE_SIZE,
    )
    yield generator.createIdle(0)


def start_dma(generators):
    generators[0].start(_dma_traffic(generators[0]))


def check(system):
    assert read_completed, "DMA partial write did not complete and validate"
    print("DMA cross-slice partial write preserved untouched bytes")
