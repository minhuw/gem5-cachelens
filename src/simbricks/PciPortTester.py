# SPDX-License-Identifier: MIT
from m5.objects.SimBricks import SimBricksPci
from m5.params import Param

class SimBricksPciPortTester(SimBricksPci):
    type = 'SimBricksPciPortTester'
    cxx_class = 'gem5::simbricks::pci::PortTester'
    cxx_header = 'simbricks/pci_port_tester.hh'
    failure = Param.String('', 'Directed invalid external input')

from m5.objects.Platform import Platform
class SimBricksTestPlatform(Platform):
    type = 'SimBricksTestPlatform'
    cxx_class = 'gem5::simbricks::pci::TestPlatform'
    cxx_header = 'simbricks/pci_port_tester.hh'

from m5.objects.ClockedObject import ClockedObject
from m5.params import ResponsePort
class SimBricksDmaChecker(ClockedObject):
    type = 'SimBricksDmaChecker'
    cxx_class = 'gem5::simbricks::pci::DmaChecker'
    cxx_header = 'simbricks/dma_checker.hh'
    port = ResponsePort('Directed DMA flag checker')

class SimBricksCheckpointTester(SimBricksPci):
    type = 'SimBricksCheckpointTester'
    cxx_class = 'gem5::simbricks::pci::CheckpointTester'
    cxx_header = 'simbricks/checkpoint_tester.hh'
    restore_probe = Param.Bool(False, 'Read preserved NIC state without initialization')
