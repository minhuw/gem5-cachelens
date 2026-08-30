# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.Ethernet import EtherInt
from m5.objects.PciDevice import PciDevice, PciMemBar
from m5.params import *


class Pvrdma(PciDevice):
    type = "Pvrdma"
    cxx_header = "dev/rdma/pvrdma.hh"
    cxx_class = "gem5::Pvrdma"

    VendorID = 0x15AD
    DeviceID = 0x0820
    Revision = 1
    SubsystemVendorID = VendorID
    SubsystemID = 1

    ClassCode = 0x02
    SubClassCode = 0x80
    ProgIF = 0x00

    InterruptPin = 0x01

    BAR0 = PciMemBar(size="16KiB")
    BAR1 = PciMemBar(size="256B")
    BAR2 = PciMemBar(size="2MiB")

    interface = EtherInt("PVRDMA Ethernet interface")

    hardware_address = Param.EthernetAddr(
        NextEthernetAddr, "PVRDMA MAC address"
    )
    control_completion_latency = Param.Latency(
        "10us", "Conservative DSR and command PIO completion latency"
    )
