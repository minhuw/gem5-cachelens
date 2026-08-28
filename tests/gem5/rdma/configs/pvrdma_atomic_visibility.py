# SPDX-License-Identifier: BSD-3-Clause

import argparse

import m5
from m5.objects import (
    AddrRange,
    GenericPciHost,
    Pvrdma,
    PvrdmaTester,
    Root,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
)

parser = argparse.ArgumentParser()
parser.add_argument("--command", action="store_true")
args = parser.parse_args()

system = System(mem_mode="atomic", mem_ranges=[AddrRange("64MiB")])
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.voltage_domain
)
system.bus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.tester = PvrdmaTester(command_test=args.command)
system.pci_host = GenericPciHost(
    platform=system.tester,
    conf_base=0x20000000,
    conf_size="64KiB",
    conf_device_bits=8,
)
system.rdma = Pvrdma(
    host=system.pci_host,
    pci_bus=0,
    pci_dev=2,
    pci_func=1,
    InterruptLine=18,
    InterruptPin=2,
    hardware_address="02:00:00:00:00:01",
)

system.system_port = system.bus.cpu_side_ports
system.memory.port = system.bus.mem_side_ports
system.tester.port = system.bus.cpu_side_ports
system.pci_host.pio = system.bus.mem_side_ports
system.rdma.pio = system.bus.mem_side_ports
system.rdma.dma = system.bus.cpu_side_ports

root = Root(full_system=False, system=system)
m5.instantiate()
exit_event = m5.simulate()
assert exit_event.getCause() == "PVRDMA atomic visibility test passed"
print(
    "PVRDMA_ATOMIC_COMMAND_OK" if args.command else "PVRDMA_ATOMIC_DSR_OK"
)
