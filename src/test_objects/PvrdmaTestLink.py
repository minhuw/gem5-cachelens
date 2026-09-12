# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.Ethernet import EtherInt
from m5.SimObject import SimObject


class PvrdmaTestLink(SimObject):
    type = "PvrdmaTestLink"
    cxx_header = "test_objects/pvrdma_test_link.hh"
    cxx_class = "gem5::PvrdmaTestLink"

    int0 = EtherInt("interface 0")
    int1 = EtherInt("interface 1")
