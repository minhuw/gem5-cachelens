# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import os
import sys

import m5
from m5.objects import (
    AddrRange,
    Process,
    Root,
    SEWorkload,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
    X86AtomicSimpleCPU,
    X86TimingSimpleCPU,
)

from gem5.components.memory import DualChannelDDR4_2400


parser = argparse.ArgumentParser()
parser.add_argument("--timing", action="store_true")
args = parser.parse_args()

binary = os.path.abspath(
    os.path.join(
        os.path.dirname(__file__),
        "..",
        "..",
        "test-progs",
        "hello",
        "bin",
        "x86",
        "linux",
        "hello",
    )
)

ranges = [AddrRange("64MiB"), AddrRange(start="1GiB", size="64MiB")]
memory = DualChannelDDR4_2400(size="128MiB")
memory.set_memory_range(ranges)

system = System(
    mem_mode="timing" if args.timing else "atomic", mem_ranges=ranges
)
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)
system.workload = SEWorkload.init_compatible(binary)
system.cpu = X86TimingSimpleCPU() if args.timing else X86AtomicSimpleCPU()
system.membus = SystemXBar()
system.memory = memory

system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.cpu.createInterruptController()
system.cpu.interrupts[0].pio = system.membus.mem_side_ports
system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports
for _, port in memory.get_mem_ports():
    system.membus.mem_side_ports = port
system.system_port = system.membus.cpu_side_ports

process = Process(cmd=[binary])
system.cpu.workload = process
system.cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()
exit_event = m5.simulate()
if exit_event.getCause() != "exiting with last active thread context":
    print(f"Unexpected exit: {exit_event.getCause()}", file=sys.stderr)
    sys.exit(1)
