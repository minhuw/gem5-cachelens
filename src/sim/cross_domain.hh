// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause
#ifndef __SIM_CROSS_DOMAIN_HH__
#define __SIM_CROSS_DOMAIN_HH__

#include <list>
#include <memory>
#include <limits>

#include "sim/drain.hh"
#include "sim/eventq.hh"
#include "sim/partition_participant.hh"
#include "sim/timing_fence.hh"

namespace gem5
{

/** Producer-owned wire endpoint. Native calls are AutoDelete Minimum_Pri
 * events; fenced calls use the canonical dispatcher at that same priority.
 * The producer, or the dispatcher with all producers stopped, owns this state.
 * Drain consumes all calls, so arbitrary closures never enter checkpoints.
 */
class CrossDomainEndpoint : public TimingFenceEndpoint, public Drainable,
                            public PartitionParticipant
{
    struct Call {
        EventManager *destination;
        Tick when, issued;
        std::function<void()> callback;
        Event *native = nullptr;
    };
    std::list<std::shared_ptr<Call>> calls;

    void deliver(const std::shared_ptr<Call> &call)
    {
        fatal_if(curTick() != call->when ||
                 curEventQueue() != call->destination->eventQueue(),
                 "%s: cross-domain delivery changed deadline or owner", id);
        call->native = nullptr;
        if (observer) observer(*this, call->issued, call->when, true);
        call->callback();
        calls.remove(call);
        if (calls.empty() && drainState() == DrainState::Draining)
            signalDrainDone();
    }

    void prepareWhileStopped() override
    {
        // Withdraw native events before any manager changes queue ownership.
        // Descheduling releases (and deletes) an AutoDelete event.
        for (auto &call : calls) {
            fatal_if(!call->native, "%s: pending dispatcher call at activation", id);
            call->destination->deschedule(call->native);
            call->native = nullptr;
        }
    }

    void beginWhileStopped() override
    {
        // The bridge has installed its hook and is still inside the fence.
        for (auto &call : calls)
            fatal_if(!timingFenceHandoff || !timingFenceHandoff(*this,
                call->destination, call->when,
                [this, call] { deliver(call); }, true),
                "%s: failed to adopt native wire delivery", id);
    }

  public:
    // Optional directed-test observer, installed only with simulation stopped.
    inline static void (*observer)(const TimingFenceEndpoint &, Tick, Tick, bool)
        = nullptr;
    CrossDomainEndpoint(TimingFenceDomain domain, const std::string &id)
        : TimingFenceEndpoint(domain, id) {}

    bool pending() const { return !calls.empty(); }
    DrainState drain() override
    {
        return pending() ? DrainState::Draining : DrainState::Drained;
    }

    void call(EventManager &destination, Tick when, std::function<void()> fn)
    {
        fatal_if(when < curTick(), "%s: cross-domain call in the past", id);
        auto call = std::make_shared<Call>(
            Call{&destination, when, curTick(), std::move(fn)});
        if (observer) observer(*this, call->issued, when, false);
        calls.push_back(call);
        auto callback = [this, call] { deliver(call); };
        if (timingFenceHandoff) {
            fatal_if(!timingFenceHandoff(*this, &destination, when, callback, true),
                     "%s: active dispatcher refused forced wire delivery", id);
        } else {
            // Deactivation restores owners to queue 0, but gem5 retains the
            // idle worker queues and still enters parallel simulate(). Native
            // events are safe within the producer's queue in that state.
            fatal_if(inParallelMode && curEventQueue() != destination.eventQueue(),
                     "%s: native wire crosses event queues without a dispatcher", id);
            fatal_if(sequence == std::numeric_limits<uint64_t>::max(),
                     "%s: wire sequence wrapped", id);
            ++sequence;
            call->native = new EventFunctionWrapper(callback, id, true,
                                                    Event::Minimum_Pri);
            destination.schedule(call->native, when);
        }
    }
};

inline void
crossDomainCall(CrossDomainEndpoint &endpoint, EventManager &destination,
                Tick when, std::function<void()> fn)
{
    endpoint.call(destination, when, std::move(fn));
}

} // namespace gem5
#endif
