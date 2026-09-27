# SPDX-License-Identifier: BSD-3-Clause
"""Generic CHI policy permits normal HNF allocation for NIC reads."""

from m5.stats.gem5stats import get_simstat
from m5.tlm_chi import Size

from chi_tlm_test_utils import (
    find_stat,
    schedule_read,
    vector_values,
)


ADDRESS = 0x80000000
EXPECTED = bytes(64)

ddio_eligible = [False]
nic_read_no_allocate = True  # Must not opt a noneligible requestor into DDIO.
hnf_enable_dmt = False
ddio_way_part = 2


def test_generators(generators):
    (reader,) = generators
    schedule_read(
        reader,
        when=10,
        address=ADDRESS,
        expected=EXPECTED,
        txn_id=1,
    )
    schedule_read(
        reader,
        when=1000,
        address=ADDRESS,
        expected=EXPECTED,
        txn_id=2,
        size=Size.SIZE_16,
        data_beats=1,
    )


def check(system):
    # Data bytes, masks, snoops and completion ordering are checked above.
    stats = get_simstat(system.ruby.hnf, prepare_stats=True).to_json()
    assert sum(vector_values(find_stat(stats, "ddioWayFill"))) == 0
    assert find_stat(stats, "m_demand_misses") == 1
    assert find_stat(stats, "m_demand_hits") == 1
    print("Untagged CHI data/coherence and aggregate accounting passed")
