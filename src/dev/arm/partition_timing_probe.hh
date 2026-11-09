// Copyright (c) 2026; SPDX-License-Identifier: BSD-3-Clause
#ifndef __DEV_ARM_PARTITION_TIMING_PROBE_HH__
#define __DEV_ARM_PARTITION_TIMING_PROBE_HH__

#include "params/ArmPartitionTimingProbe.hh"
#include "sim/sim_object.hh"

namespace gem5
{
class ArmSystem;
class GicV2;
class GenericTimer;

/** Companion to the bare-metal test. Drives real model entry points and checks
 * raw interrupt state at precise ticks, while the guest records handled IRQs.
 */
class ArmPartitionTimingProbe : public SimObject
{
    ArmSystem &system;
    GicV2 &gic;
    GenericTimer &timer;
    Tick start;
    void at(EventManager &manager, Tick when, std::function<void()> fn,
            Event::Priority priority = Event::Default_Pri);
    void irq(bool expected, const char *label);
    void ppi(bool expected, const char *label);
    void timerCase(Tick base, int mask_offset, bool disable);
  public:
    using Params = ArmPartitionTimingProbeParams;
    explicit ArmPartitionTimingProbe(const Params &p);
    void startup() override;
};
} // namespace gem5
#endif
