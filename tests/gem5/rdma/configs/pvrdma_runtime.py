# SPDX-License-Identifier: BSD-3-Clause

import argparse
from pathlib import Path

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
parser.add_argument(
    "--mode",
    choices=(
        "timing-mr",
        "timing-queues",
        "checkpoint-save",
        "checkpoint-restore",
    ),
    required=True,
)
parser.add_argument("--checkpoint", type=Path)
args = parser.parse_args()
if args.mode.startswith("checkpoint-") and args.checkpoint is None:
    parser.error("checkpoint modes require --checkpoint")

system = System(
    mem_mode="timing" if args.mode.startswith("timing-") else "atomic",
    mem_ranges=[AddrRange("64MiB")],
)
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.voltage_domain
)
system.bus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="5us")
system.tester = PvrdmaTester(test_mode=args.mode)
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
m5.instantiate(
    args.checkpoint.as_posix() if args.mode == "checkpoint-restore" else None
)
exit_event = m5.simulate()

if args.mode == "timing-mr":
    assert exit_event.getCause() == "PVRDMA timing MR test passed"
    print("PVRDMA_TIMING_MR_OK")
elif args.mode == "timing-queues":
    assert exit_event.getCause() == "PVRDMA timing queue test passed"
    print("PVRDMA_TIMING_QUEUES_OK")
elif args.mode == "checkpoint-save":
    assert exit_event.getCause() == "PVRDMA checkpoint save ready"
    args.checkpoint.mkdir(parents=True, exist_ok=True)
    m5.checkpoint(args.checkpoint.as_posix())
    print("PVRDMA_CHECKPOINT_SAVED")
else:
    assert exit_event.getCause() == "PVRDMA checkpoint restore test passed"
    print("PVRDMA_CHECKPOINT_RESTORED")
