# Copyright (c) 2026; SPDX-License-Identifier: BSD-3-Clause
from m5.params import *
from m5.proxy import Parent
from m5.SimObject import SimObject


class ArmPartitionTimingProbe(SimObject):
    """Exact-tick companion to the directed EL1 guest; never in production boards."""
    type = 'ArmPartitionTimingProbe'
    cxx_header = 'dev/arm/partition_timing_probe.hh'
    cxx_class = 'gem5::ArmPartitionTimingProbe'
    system = Param.ArmSystem(Parent.any, 'Directed-test system')
    gic = Param.GicV2('GIC to exercise')
    timer = Param.GenericTimer('Core timer to exercise')
    start = Param.Tick(80_000_123, 'After the guest has parked with IRQs masked')
