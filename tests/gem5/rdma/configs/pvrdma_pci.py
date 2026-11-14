# SPDX-License-Identifier: BSD-3-Clause
"""Native PCI host/device proof; no Linux CPU or GIC delivery is simulated."""
import argparse
from pathlib import Path

import m5
from m5.objects import (
    AddrRange, GenericPciHost, IGbE_e1000, Pvrdma, PvrdmaTester,
    Root, SimpleMemory, SrcClockDomain, System, SystemXBar, VoltageDomain,
)

parser = argparse.ArgumentParser()
parser.add_argument("--host", choices=("generic", "arm"), default="generic")
parser.add_argument("--phase", choices=("run", "save", "restore"), default="run")
parser.add_argument("--checkpoint", type=Path)
parser.add_argument("--no-aggregate", action="store_true")
args = parser.parse_args()
if args.phase != "run" and args.checkpoint is None:
    parser.error("checkpoint phase requires --checkpoint")
mode = "pci-shared-intx" + ("" if args.phase == "run" else "-" + args.phase)
system = System(mem_mode="atomic", mem_ranges=[AddrRange(0x80000000, size="64MiB")])
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(clock="1GHz", voltage_domain=system.voltage_domain)
system.bus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.tester = PvrdmaTester(test_mode=mode)
host_type = GenericPciHost
host_options = {}
if args.host == "arm":
    from m5.objects import GenericArmPciHost
    host_type = GenericArmPciHost
    host_options = dict(int_policy="ARM_PCI_INT_DEV", int_base=100, int_count=4)
system.pci_host = host_type(
    platform=system.tester, conf_base=0x30000000, conf_size="256MiB",
    conf_device_bits=12, pci_mem_base=0x40000000, pci_dma_base=0x80000000,
    aggregate_intx=not args.no_aggregate, **host_options,
)
system.nic = IGbE_e1000(
    host=system.pci_host, pci_bus=0, pci_dev=2, pci_func=0,
    HeaderType=0x80, InterruptLine=102, InterruptPin=1,
)
system.rdma = Pvrdma(
    host=system.pci_host, pci_bus=0, pci_dev=2, pci_func=1,
    InterruptLine=102, InterruptPin=2,
)
system.system_port = system.bus.cpu_side_ports
system.memory.port = system.bus.mem_side_ports
system.tester.port = system.bus.cpu_side_ports
system.pci_host.pio = system.bus.mem_side_ports
for device in (system.nic, system.rdma):
    device.pio = system.bus.mem_side_ports
    device.dma = system.bus.cpu_side_ports
root = Root(full_system=False, system=system)
m5.instantiate(str(args.checkpoint) if args.phase == "restore" else None)
event = m5.simulate()
if args.phase == "save":
    assert event.getCause() == "PVRDMA PCI shared INTx checkpoint ready"
    args.checkpoint.mkdir(parents=True, exist_ok=True)
    m5.checkpoint(str(args.checkpoint))
    print("PVRDMA_PCI_SHARED_INTX_SAVED")
else:
    assert event.getCause() == "PVRDMA PCI shared INTx test passed"
    print("PVRDMA_PCI_SHARED_INTX_PASSED host=" + args.host + " phase=" + args.phase)
