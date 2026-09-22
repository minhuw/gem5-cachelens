# SPDX-License-Identifier: MIT
from m5.SimObject import SimObject
from m5.params import Param, ResponsePort
from m5.util.pybind import PyBindMethod


class SimBricksCheckpointInterruptTester(SimObject):
    type = 'SimBricksCheckpointInterruptTester'
    cxx_class = 'gem5::simbricks::CheckpointInterruptTester'
    cxx_header = 'simbricks/checkpoint_interrupt_tester.hh'
    cxx_exports = [PyBindMethod('armPit')]
    pit = Param.I8254('Real PIT')
    ioapic = Param.I82094AA('Real IOAPIC')
    system = Param.System('Timing-mode system')
    inject_writeback = Param.Bool(False, 'Pulse IOAPIC during memWriteback')
    interrupt = ResponsePort('Timing interrupt target (no CPU execution)')
