// Copyright (c) 2026; SPDX-License-Identifier: BSD-3-Clause
#include "dev/arm/partition_timing_probe.hh"

#include <iostream>

#include "arch/arm/interrupts.hh"
#include "arch/arm/system.hh"
#include "cpu/base.hh"
#include "dev/arm/generic_timer.hh"
#include "dev/arm/gic_v2.hh"
#include "sim/cross_domain.hh"

namespace gem5
{
using namespace ArmISA;

ArmPartitionTimingProbe::ArmPartitionTimingProbe(const Params &p)
    : SimObject(p), system(*p.system), gic(*p.gic), timer(*p.timer),
      start(p.start)
{}

void
ArmPartitionTimingProbe::at(EventManager &manager, Tick when,
                           std::function<void()> fn, Event::Priority priority)
{
    manager.schedule(new EventFunctionWrapper(std::move(fn), name(), true,
                                              priority), when);
}

void
ArmPartitionTimingProbe::irq(bool expected, const char *label)
{
    auto *tc = system.threads[0];
    auto *interrupts = static_cast<Interrupts *>(
        tc->getCpuPtr()->getInterruptController(tc->threadId()));
    fatal_if(interrupts->checkRaw(INT_IRQ) != expected,
             "%s: %s raw IRQ mismatch at %llu", name(), label, curTick());
    std::cout << "ARM_TIMING_PROBE_PASS label=" << label << " tick="
              << curTick() << " irq=" << expected << std::endl;
}

void
ArmPartitionTimingProbe::ppi(bool expected, const char *label)
{
    fatal_if(bool(gic.cpuPpiPending[0] & (1 << (27 - 16))) != expected,
             "%s: %s PPI mismatch at %llu", name(), label, curTick());
    std::cout << "ARM_TIMING_PROBE_PASS label=" << label << " tick="
              << curTick() << " ppi=" << expected << std::endl;
}

void
ArmPartitionTimingProbe::timerCase(Tick base, int mask_offset, bool disable)
{
    // The test counter is 2GHz: align the expiry to a 500-tick boundary.
    const Tick expiry = (base / 500 + 20) * 500;
    const Tick change = expiry + mask_offset;
    const Tick latency = system.partitionWireLatency;
    auto &cpu = *system.threads[0]->getCpuPtr();
    at(cpu, base, [this, expiry] {
        timer.setMiscReg(MISCREG_CNTV_CTL_EL0, 0, 0);
        timer.setMiscReg(MISCREG_CNTV_CVAL_EL0, 0, expiry / 500);
        timer.setMiscReg(MISCREG_CNTV_CTL_EL0, 0, 1);
    });
    // At expiry, run the control write BEFORE the timer's Default_Pri event.
    at(cpu, change, [this, disable, mask_offset] {
        timer.setMiscReg(MISCREG_CNTV_CTL_EL0, 0, disable ? 0 : 3);
        std::cout << "ARM_TIMING_CONTROL disable=" << disable
                  << " offset=" << mask_offset << " tick=" << curTick()
                  << std::endl;
    }, Event::Default_Pri - 1);
    at(gic, expiry + latency - 1, [this] { ppi(false, "ppi-before-wire"); });
    at(gic, expiry + latency, [this, mask_offset] {
        ppi(mask_offset > 0, "ppi-at-wire");
    });
    at(gic, expiry + latency + 2, [this] { ppi(false, "ppi-after-control"); });
    // A post already in flight MUST still land even after the PPI is cleared.
    // Check it, then explicitly clear the wire before the next case.
    at(cpu, expiry + latency + gic.intLatency, [this, mask_offset] {
        irq(mask_offset > 0, "post-survives-ppi-clear");
    });
    at(gic, expiry + latency + gic.intLatency + 10, [this] {
        gic.clearInt(ContextID(0), 27);
    });
    at(cpu, expiry + 2 * latency + gic.intLatency + 10, [this] {
        irq(false, "case-cleanup");
    });
}

void
ArmPartitionTimingProbe::startup()
{
    SimObject::startup();
    fatal_if(!system.partitionTiming || system.partitionWireLatency != 1000 ||
             gic.intLatency != 10000,
             "%s: probe requires 1ns wires and 10ns GIC latency", name());
    fatal_if(CrossDomainEndpoint::observer, "%s: duplicate wire observer", name());
    CrossDomainEndpoint::observer = [](const TimingFenceEndpoint &endpoint,
                                     Tick issued, Tick when, bool delivered) {
        fatal_if(when - issued < 1000, "Directed ARM wire has no 1ns lookahead");
        std::cout << "ARM_WIRE " << (delivered ? "land" : "issue")
                  << " endpoint=" << endpoint.id << " issued=" << issued
                  << " when=" << when << std::endl;
    };
    const Tick latency = system.partitionWireLatency;
    const Tick first = start + gic.intLatency;
    const Tick clear_issue = first - latency + 100;
    const Tick repost_issue = clear_issue + 100;
    const Tick second = repost_issue + gic.intLatency;
    auto &cpu = *system.threads[0]->getCpuPtr();
    at(gic, start, [this, first] {
        fatal_if(!gic.deliveries.empty() || gic.pendingDelayedInterrupts,
                 "%s: guest not quiescent at probe start", name());
        irq(false, "race-initial");
        gic.postInt(0, first);
        gic.postInt(0, first + 1); // Same generation: must be deduplicated.
        fatal_if(gic.pendingDelayedInterrupts != 1, "Initial post not deduplicated");
    });
    at(gic, clear_issue, [this] { gic.clearInt(ContextID(0), 27); });
    at(gic, repost_issue, [this, second] {
        gic.postInt(0, second);
        gic.postInt(0, second + 1);
        fatal_if(gic.pendingDelayedInterrupts != 2,
                 "Post after clear lost or duplicated while old post in flight");
    });
    at(cpu, first - 1, [this] { irq(false, "race-before-first"); });
    at(cpu, first, [this] {
        irq(true, "race-first-post");
        fatal_if(gic.pendingDelayedInterrupts != 1, "First post count mismatch");
    });
    at(cpu, clear_issue + latency - 1, [this] { irq(true, "race-before-clear"); });
    at(cpu, clear_issue + latency, [this] { irq(false, "race-clear"); });
    at(cpu, second - 1, [this] { irq(false, "race-before-second"); });
    at(cpu, second, [this] {
        irq(true, "race-second-post");
        fatal_if(gic.pendingDelayedInterrupts || !gic.deliveries.empty(),
                 "Race left a duplicate delivery");
    });
    at(gic, second + 10, [this] { gic.clearInt(ContextID(0), 27); });
    at(cpu, second + latency + 10, [this] { irq(false, "race-final-clear"); });
    unsigned index = 0;
    for (bool disable : {false, true})
        for (int offset : {-1, 0, 1})
            timerCase(start + (++index) * 100000, offset, disable);
    const Tick short_base = start + 700000;
    const Tick short_expiry = (short_base / 500 + 1) * 500;
    at(cpu, short_base, [this, short_expiry] {
        const auto count = timer.readMiscReg(MISCREG_CNTVCT_EL0, 0);
        fatal_if((count + 1) * 500 != short_expiry,
                 "Probe counter phase changed");
        timer.setMiscReg(MISCREG_CNTV_CTL_EL0, 0, 0);
        timer.setMiscReg(MISCREG_CNTV_CVAL_EL0, 0, count + 1);
        timer.setMiscReg(MISCREG_CNTV_CTL_EL0, 0, 1);
        std::cout << "ARM_TIMING_SHORT_CVAL tick=" << curTick()
                  << " expiry=" << short_expiry
                  << " ticks_ahead=" << short_expiry - curTick() << std::endl;
    });
    at(gic, short_expiry + latency - 1,
       [this] { ppi(false, "short-cval-before-wire"); });
    at(gic, short_expiry + latency,
       [this] { ppi(true, "short-cval-at-wire"); });
    at(cpu, short_expiry + latency + 10, [this] {
        timer.setMiscReg(MISCREG_CNTV_CTL_EL0, 0, 0);
    });
    at(gic, short_expiry + 2 * latency + 10,
       [this] { ppi(false, "short-cval-clear"); });
    at(cpu, short_expiry + latency + gic.intLatency,
       [this] { irq(true, "short-cval-post"); });
    at(gic, short_expiry + latency + gic.intLatency + 10,
       [this] { gic.clearInt(ContextID(0), 27); });
    at(cpu, start + 800000, [this] {
        fatal_if(gic.pendingDelayedInterrupts || !gic.deliveries.empty(),
                 "Probe left a pending interrupt delivery");
        std::cout << "ARM_TIMING_PROBE_COMPLETE tick=" << curTick() << std::endl;
    });
}
} // namespace gem5
