#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Host-only O3 drain/wakeup regression; requires a C++17 compiler.

Run: python3 verification/o3/test_drained_wakeup.py
Compiles real method bodies from this checkout with minimal CPU/event stubs.
This checks scheduling decisions, not a full gem5 event queue or system boot.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / "src/cpu/o3"


def method(source, name):
    start = source.index(f"void\n{name}(")
    # These file-scope method definitions end at an unindented closing brace.
    return source[start:source.index("\n}\n", start) + 3]


STUBS = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <list>
#include <vector>
#define DPRINTF(...) ((void)0)
namespace gem5 {
struct ThreadContext { enum Status { Active, Suspended }; };
enum class DrainState { Running, Draining, Drained };
namespace o3 {
using ThreadID = unsigned;
using Cycles = uint64_t;
uint64_t curTick() { return 100; }
struct BaseCPU { void activateContext(ThreadID) {} };
struct CPU;
struct Thread {
    gem5::ThreadContext::Status state = gem5::ThreadContext::Suspended;
    uint64_t lastActivate = 0;
    auto status() const { return state; }
    void setStatus(gem5::ThreadContext::Status s) { state = s; }
    ThreadID threadId() const { return 0; }
};
struct ThreadContext {
    CPU *cpu;
    Thread *thread;
    void activate();
};
struct CPU : BaseCPU {
    struct Event {
        bool pending = false;
        uint64_t when = 0;
        bool scheduled() const { return pending; }
    } tickEvent;
    struct Activity {
        bool value = false;
        bool active() const { return value; }
        void activity() { value = true; }
    } activityRec;
    struct Stage {
        unsigned resumes = 0;
        void drainResume() { ++resumes; }
        void wakeFromQuiesce() {}
    } fetch, commit;
    struct { uint64_t idleCycles = 0, quiesceCycles = 0; } cpuStats;
    struct { uint64_t numCycles = 0; } baseStats;
    enum Status { Idle, Running } _status = Idle;
    uint64_t lastActivatedCycle = 1, lastRunningCycle = 1;
    DrainState state = DrainState::Drained;
    unsigned schedules = 0;
    Thread t;
    std::vector<Thread *> thread{&t};
    ThreadContext ctx{this, &t};
    std::vector<ThreadContext *> threadContexts{&ctx};
    std::list<ThreadID> activeThreads;
    bool switchedOut() const { return false; }
    DrainState drainState() const { return state; }
    uint64_t curCycle() const { return 100; }
    uint64_t clockEdge() const { return 100; }
    uint64_t nextCycle() const { return 101; }
    void verifyMemoryMode() {}
    void schedulePowerGatingEvent() {}
    void schedule(Event &e, uint64_t when) {
        assert(!e.pending);
        e.pending = true;
        e.when = when;
        ++schedules;
    }
    // Only unsquashed events are used by this harness.
    void scheduleTickEvent(Cycles) {
        if (!tickEvent.scheduled()) schedule(tickEvent, clockEdge());
    }
    void activateThread(ThreadID);
    void activateContext(ThreadID);
    void wakeCPU();
    void wakeup(ThreadID);
    void drainResume();
};
"""

CHECKS = r"""
} // namespace o3
} // namespace gem5
int main() {
    using gem5::DrainState;
    using gem5::ThreadContext;
    using gem5::o3::CPU;
    CPU cpu;
    cpu.wakeCPU(); // Also cover non-interrupt callers of the shared helper.
    cpu.wakeup(0);
    cpu.wakeup(0); // Repeated notifications must not schedule a tick either.
    assert(cpu.t.status() == ThreadContext::Active);
    assert(cpu.t.lastActivate == 100);
    assert(cpu.activeThreads == std::list<unsigned>{0});
    assert(cpu.drainState() == DrainState::Drained);
    assert(!cpu.tickEvent.scheduled() && cpu.schedules == 0);
    assert(cpu.cpuStats.idleCycles == 0 && cpu.cpuStats.quiesceCycles == 0);
    assert(cpu.baseStats.numCycles == 0 && !cpu.activityRec.active());
    std::cout << "PASS drained wakeup activates thread without ticking/accounting\n";

    // Drainable::dmDrainResume changes the state before calling drainResume.
    cpu.state = DrainState::Running;
    cpu.drainResume();
    assert(cpu.tickEvent.scheduled() && cpu.tickEvent.when == cpu.nextCycle());
    assert(cpu.schedules == 1 && cpu._status == CPU::Running);
    assert(cpu.fetch.resumes == 1 && cpu.commit.resumes == 1);
    cpu.wakeCPU();
    cpu.wakeup(0);
    assert(cpu.schedules == 1);
    std::cout << "PASS drainResume schedules exactly one tick for the active thread\n";

    for (auto state : {DrainState::Draining, DrainState::Running}) {
        CPU awake;
        awake.state = state;
        awake.wakeup(0);
        assert(awake.t.status() == ThreadContext::Active);
        assert(awake.activeThreads == std::list<unsigned>{0});
        assert(awake.tickEvent.scheduled() && awake.schedules == 1);
        assert(awake.cpuStats.idleCycles > 0 && awake.baseStats.numCycles > 0);
        assert(awake.drainState() == state);
    }
    std::cout << "PASS Draining and Running wakeups retain scheduling/accounting\n";

    CPU asleep;
    asleep.state = DrainState::Running;
    asleep.drainResume();
    assert(asleep.schedules == 0 && asleep._status == CPU::Idle);
    std::cout << "PASS resume without an activated thread stays idle\n";
}
"""


class DrainedWakeupTest(unittest.TestCase):
    def test_wakeup_and_resume(self):
        cpu = (SOURCE / "cpu.cc").read_text()
        context = (SOURCE / "thread_context.cc").read_text()
        bodies = "\n".join(method(cpu, f"CPU::{name}") for name in (
            "activateThread", "activateContext", "wakeCPU", "wakeup",
            "drainResume",
        )) + method(context, "ThreadContext::activate")
        with tempfile.TemporaryDirectory(prefix="gem5-o3-wakeup-") as directory:
            path = Path(directory)
            source = path / "wakeup.cc"
            source.write_text(STUBS + bodies + CHECKS)
            binary = path / "wakeup"
            subprocess.run([
                *shlex.split(os.environ.get("CXX", "c++")),
                *shlex.split(os.environ.get("CXXFLAGS", "")),
                "-std=c++17", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
                str(source), "-o", str(binary),
            ], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
