# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.Ethernet import EtherInt
from m5.objects.Platform import Platform
from m5.params import Param, RequestPort


class PvrdmaTester(Platform):
    type = "PvrdmaTester"
    cxx_header = "test_objects/pvrdma_tester.hh"
    cxx_class = "gem5::PvrdmaTester"

    port = RequestPort("Port used to issue atomic memory and MMIO requests")
    fault0 = EtherInt("Fault-link test endpoint 0")
    fault1 = EtherInt("Fault-link test endpoint 1")
    command_test = Param.Bool(False, "Test command response visibility")
    test_mode = Param.String("atomic", "Focused PVRDMA tester mode")
