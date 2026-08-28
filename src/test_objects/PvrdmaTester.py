# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.Platform import Platform
from m5.params import Param, RequestPort


class PvrdmaTester(Platform):
    type = "PvrdmaTester"
    cxx_header = "test_objects/pvrdma_tester.hh"
    cxx_class = "gem5::PvrdmaTester"

    port = RequestPort("Port used to issue atomic memory and MMIO requests")
    command_test = Param.Bool(False, "Test command response visibility")
