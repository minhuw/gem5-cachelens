# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import sys

import m5
from m5.objects import (
    AddrRange,
    PortTerminator,
    PyTrafficGen,
    Root,
    SimpleMemory,
    SrcClockDomain,
    System,
    VoltageDomain,
)
from m5.stats.gem5stats import get_simstat

from gem5.components.memory import DualChannelDDR4_2400


MEMORY_SIZE = "128MiB"
# Scale x86's [0, 3GiB) + [4GiB, 9GiB) RAM layout down by 64. The
# controllers still expose the packed [0, 128MiB) range downstream.
LOW_RANGE = AddrRange("48MiB")
HIGH_RANGE = AddrRange(start="64MiB", size="80MiB")
# This is both valid high RAM and the first byte outside the packed range.
HIGH_ADDRESS = 128 * 1024 * 1024
GAP_ADDRESS = 56 * 1024 * 1024
SYSTEM_PORT_RANGE = AddrRange(start="2GiB", size="4KiB")


def make_system(split, bus_clock, requestors):
    system = System(
        cache_line_size=64,
        mem_mode="timing",
        # The direct system deliberately leaves this empty to exercise the
        # backward-compatible PhysicalMemory classification fallback.
        main_mem_ranges=[LOW_RANGE, HIGH_RANGE] if split else [],
    )
    system.voltage_domain = VoltageDomain()
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=system.voltage_domain
    )
    system.bus_clk_domain = SrcClockDomain(
        clock=bus_clock, voltage_domain=system.voltage_domain
    )
    system.system_port_memory = SimpleMemory(range=SYSTEM_PORT_RANGE)
    system.system_port = system.system_port_memory.port

    system.memory = DualChannelDDR4_2400(size=MEMORY_SIZE)
    if split:
        system.memory.set_memory_range([LOW_RANGE, HIGH_RANGE])
        system.memory._range_mapper_buses[0].clk_domain = (
            system.bus_clk_domain
        )
    else:
        system.memory.set_memory_range([AddrRange(MEMORY_SIZE)])

    system.generators = [PyTrafficGen() for _ in range(requestors)]
    ports = system.memory.get_mem_ports()
    for generator, (_, port) in zip(system.generators, ports):
        generator.port = port
    if len(ports) > requestors:
        system.unused_port_terminator = PortTerminator()
        for _, port in ports[requestors:]:
            system.unused_port_terminator.req_ports = port
    return system


def linear_read(generator, address):
    return generator.createLinear(
        100000,
        address,
        address + 64,
        64,
        1,
        1,
        100,
        64,
    )


def one_read(generator, address, exit_after=False):
    yield linear_read(generator, address)
    if exit_after:
        yield generator.createIdle(1000000)
        yield generator.createExit(0)
    else:
        yield generator.createIdle(10000000)


def scalar_stats(simobject):
    output = get_simstat(simobject, prepare_stats=True).to_json()
    return {
        name: value["value"]
        for name, value in output.items()
        if isinstance(value, dict) and value.get("type") == "Scalar"
    }


direct_system = make_system(False, "1GHz", 1)
split_system = make_system(True, "1GHz", 1)
gap_system = make_system(True, "1GHz", 1)
slow_system = make_system(True, "250MHz", 2)
fast_system = make_system(True, "4GHz", 2)

root = Root(full_system=False, system=direct_system)
root.split_system = split_system
root.gap_system = gap_system
root.slow_system = slow_system
root.fast_system = fast_system
m5.instantiate()

split_system.generators[0].start(one_read(split_system.generators[0], 0))
gap_system.generators[0].start(
    one_read(gap_system.generators[0], GAP_ADDRESS)
)
slow_system.generators[0].start(one_read(slow_system.generators[0], 0))
slow_system.generators[1].start(
    one_read(slow_system.generators[1], HIGH_ADDRESS)
)
fast_system.generators[0].start(one_read(fast_system.generators[0], 0))
fast_system.generators[1].start(
    one_read(fast_system.generators[1], HIGH_ADDRESS)
)
direct_system.generators[0].start(
    one_read(direct_system.generators[0], 0, True)
)

exit_event = m5.simulate()
if "exit state" not in exit_event.getCause():
    print(f"Unexpected exit: {exit_event.getCause()}", file=sys.stderr)
    sys.exit(1)

stats = {
    "direct": scalar_stats(direct_system.generators[0]),
    "split": scalar_stats(split_system.generators[0]),
    "gap": scalar_stats(gap_system.generators[0]),
    "slow0": scalar_stats(slow_system.generators[0]),
    "slow1": scalar_stats(slow_system.generators[1]),
    "fast0": scalar_stats(fast_system.generators[0]),
    "fast1": scalar_stats(fast_system.generators[1]),
}

if (
    stats["direct"]["numPackets"],
    stats["direct"]["numSuppressed"],
    stats["direct"]["totalReads"],
) != (1, 0, 1):
    print(
        "Empty main_mem_ranges did not fall back to PhysicalMemory",
        file=sys.stderr,
    )
    sys.exit(1)

if (
    stats["gap"]["numPackets"],
    stats["gap"]["numSuppressed"],
    stats["gap"]["totalReads"],
) != (0, 1, 0):
    print(
        "The guest RAM gap was not suppressed as non-memory",
        file=sys.stderr,
    )
    sys.exit(1)

for name in ("slow1", "fast1"):
    if (
        stats[name]["numPackets"],
        stats[name]["numSuppressed"],
        stats[name]["totalReads"],
    ) != (1, 0, 1):
        print(
            "High guest RAM did not route through the packed controller",
            file=sys.stderr,
        )
        sys.exit(1)

if stats["direct"]["totalReadLatency"] != stats["split"]["totalReadLatency"]:
    print(
        "Split routing changed the uncontended read latency",
        file=sys.stderr,
    )
    sys.exit(1)

slow_retries = sorted(
    (stats["slow0"]["numRetries"], stats["slow1"]["numRetries"])
)
fast_retries = sorted(
    (stats["fast0"]["numRetries"], stats["fast1"]["numRetries"])
)
if slow_retries != [0, 1] or fast_retries != [0, 1]:
    print(
        "Expected exactly one routed request retry per pair",
        file=sys.stderr,
    )
    sys.exit(1)

slow_retry_ticks = sorted(
    (stats["slow0"]["retryTicks"], stats["slow1"]["retryTicks"])
)
fast_retry_ticks = sorted(
    (stats["fast0"]["retryTicks"], stats["fast1"]["retryTicks"])
)
if slow_retry_ticks != [0, 1] or fast_retry_ticks != [0, 1]:
    print("Transparent retry bookkeeping must take one tick", file=sys.stderr)
    sys.exit(1)

slow_latencies = sorted(
    (stats["slow0"]["totalReadLatency"], stats["slow1"]["totalReadLatency"])
)
fast_latencies = sorted(
    (stats["fast0"]["totalReadLatency"], stats["fast1"]["totalReadLatency"])
)
if slow_latencies != fast_latencies:
    print("Crossbar clock frequency changed routed latency", file=sys.stderr)
    sys.exit(1)
