# Copyright (c) 2026 minhuw
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import argparse
import os

import m5
from m5.objects import *
from m5.util import addToPath

addToPath(
    os.path.abspath(
        os.path.join(os.path.dirname(__file__), "../../../../configs")
    )
)

from common import Options
from ruby import Ruby


parser = argparse.ArgumentParser()
Options.addNoISAOptions(parser)
parser.add_argument("--scenario", required=True)
Ruby.define_options(parser)
args = parser.parse_args()

args.num_cpus = 2
args.num_dirs = 1
args.cacheline_size = 64
args.mem_size = "64MiB"
args.l1d_size = "256B"
args.l1i_size = "256B"
args.l1d_assoc = 2
args.l1i_assoc = 2
args.l2_size = "512B"
args.l2_assoc = 4
args.topology = "Crossbar"

if args.num_l2caches <= 0 or (
    args.num_l2caches & (args.num_l2caches - 1)
):
    raise ValueError("--num-l2caches must be a positive power of two")

system = System(
    clk_domain=SrcClockDomain(clock=args.sys_clock),
    mem_ranges=[AddrRange(args.mem_size)],
)
system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)
system.clk_domain.voltage_domain = system.voltage_domain
system.cpu = MESIDDIODirectedTester(
    scenario=args.scenario,
    block_size=args.cacheline_size,
    routing_banks=args.num_l2caches,
    set_stride=128 * args.num_l2caches,
)

dma_ports = [system.cpu.dma_port]
if args.scenario in {
    "partial_claim_dma_read_race",
    "partial_claim_dma_write_race",
}:
    dma_ports.append(system.cpu.dma_race_port)

Ruby.create_system(
    args,
    False,
    system,
    dma_ports=dma_ports,
    cpus=[system.cpu] * args.num_cpus,
)
system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock, voltage_domain=system.voltage_domain
)
system.ruby.randomization = False

assert len(system.ruby._cpu_ports) == args.num_cpus
for ruby_port in system.ruby._cpu_ports:
    system.cpu.cpu_ports = ruby_port.in_ports
    ruby_port.deadlock_threshold = 1000000

root = Root(full_system=False, system=system)
root.system.mem_mode = "timing"
m5.ticks.setGlobalFrequency("1ns")
m5.instantiate()

exit_event = m5.simulate(args.abs_max_tick)
print("Exiting @ tick", m5.curTick(), "because", exit_event.getCause())
if exit_event.getCause() != "MESI DDIO directed scenario completed":
    raise RuntimeError(f"Unexpected simulation exit: {exit_event.getCause()}")
