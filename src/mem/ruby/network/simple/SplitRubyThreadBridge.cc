// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause
// Full-system entry: a single authoritative device/CPU state; only complete
// HNF/controller/router islands migrate, after the guest workbegin exit.
#include "mem/ruby/network/simple/SplitRubyThreadBridge.hh"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <mutex>
#include "base/logging.hh"
#include "base/spin_mailbox_fence.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "mem/translating_port_proxy.hh"
#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/network/simple/SplitMessage.hh"
#include "mem/ruby/network/simple/Switch.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "sim/global_event.hh"
#include "sim/simulate.hh"
#include "sim/serialize.hh"
#include "sim/timing_fence.hh"
#include "sim/sim_events.hh"
#include "mem/packet_queue.hh"

namespace gem5 { namespace ruby {

SplitRubyThreadBridge *SplitRubyThreadBridge::running = nullptr;

class SplitRubyThreadBridge::Fence : public BaseGlobalEventTemplate<Fence>
{
    using Base = BaseGlobalEventTemplate<Fence>;
    SplitRubyThreadBridge &owner;
    std::unique_ptr<SpinMailboxFence> mailbox;
    const uint64_t barrierBaseline;
    uint64_t callbacks = 0;
    // Written only by the serialized callback; read by roles after completion.
    uint64_t traceSamples = 0, spreadSum = 0, spreadMax = 0;
    uint64_t latestTies = 0;
    struct ArrivalSample { uint64_t lag = 0; bool latest = false; };
    std::array<ArrivalSample, 8> arrivalSample{};

    static uint64_t hostNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

  public:
    class BarrierEvent : public Base::BarrierEvent
    {
      public:
        unsigned role = 0;
        uint64_t epoch = 0;
        // Only this event's stable owner writes its slot. The callback reads
        // arrivalNs only, after arrival synchronization and before completion.
        struct alignas(64) Trace
        {
            uint64_t arrivalNs = 0, samples = 0;
            uint64_t lagSum = 0, lagMax = 0, latestCount = 0, latestTieCount = 0;
            uint64_t firstSum = 0, firstMax = 0, secondSum = 0, secondMax = 0;
            uint64_t totalSum = 0, totalMax = 0, callbacks = 0;
        } trace;

        BarrierEvent(Base *event, Priority p, Flags f)
            : Base::BarrierEvent(event, p, f) {}
        void process() override
        {
            auto &fence = *static_cast<Fence *>(_globalEvent);
            fatal_if(++epoch == 0, "Fence epoch wrapped");
            const unsigned stride = fence.owner.syncTraceStride;
            const bool sample = stride && (epoch - 1) % stride == 0;
            uint64_t before = 0, first = 0, after = 0;
            if (sample) trace.arrivalNs = before = hostNs();
            if (!fence.owner.mailboxFence) {
                // Retain the exact legacy two-globalBarrier() lock discipline.
                const bool leader = globalBarrier();
                if (sample) first = hostNs();
                if (leader) {
                    if (sample) fence.sampleArrivals();
                    ++trace.callbacks;
                    fence.process();
                }
                globalBarrier();
                if (sample) after = hostNs();
            } else {
                EventQueue *queue = curEventQueue();
                {
                    // Release before publishing arrival. Followers restore
                    // their own lock only after acquiring completion.
                    EventQueue::ScopedRelease release(queue);
                    fence.mailbox->wait(role, epoch, [&] {
                        // ScopedMigration inside process requires this lock.
                        // The guard dies before wait publishes completion.
                        std::lock_guard<EventQueue> lock(*queue);
                        if (sample) fence.sampleArrivals();
                        ++trace.callbacks;
                        fence.process();
                    });
                }
                if (sample) after = hostNs();
            }
            if (sample) {
                // Callback snapshots stay unchanged until ALL roles publish
                // the next arrival. Finish reading them before our next call.
                ++trace.samples;
                const auto lag = fence.arrivalSample[role].lag;
                trace.lagSum += lag;
                trace.lagMax = std::max(trace.lagMax, lag);
                if (fence.arrivalSample[role].latest) {
                    ++trace.latestCount;
                    if (fence.latestTies > 1) ++trace.latestTieCount;
                }
                trace.totalSum += after - before;
                trace.totalMax = std::max(trace.totalMax, after - before);
                if (!fence.owner.mailboxFence) {
                    trace.firstSum += first - before;
                    trace.firstMax = std::max(trace.firstMax, first - before);
                    trace.secondSum += after - first;
                    trace.secondMax = std::max(trace.secondMax, after - first);
                }
            }
        }
    };

    explicit Fence(SplitRubyThreadBridge &bridge)
        : Base(Event::Minimum_Pri, 0), owner(bridge),
          mailbox(owner.mailboxFence ?
              std::make_unique<SpinMailboxFence>(numMainEventQueues, owner.fenceLeader) :
              nullptr), barrierBaseline(owner.barriers)
    {
        // Base constructs events before the Fence's members exist. Assign
        // stable queue identities only after that construction has completed.
        for (unsigned role = 0; role < barrierEvent.size(); ++role)
            static_cast<BarrierEvent *>(barrierEvent[role])->role = role;
    }

    void sampleArrivals()
    {
        uint64_t earliest = std::numeric_limits<uint64_t>::max();
        uint64_t latest = 0;
        for (auto *event : barrierEvent) {
            const auto stamp = static_cast<BarrierEvent *>(event)->trace.arrivalNs;
            earliest = std::min(earliest, stamp);
            latest = std::max(latest, stamp);
        }
        latestTies = 0;
        for (unsigned role = 0; role < barrierEvent.size(); ++role) {
            const auto stamp =
                static_cast<BarrierEvent *>(barrierEvent[role])->trace.arrivalNs;
            arrivalSample[role] = {stamp - earliest, stamp == latest};
            if (stamp == latest) ++latestTies;
        }
        ++traceSamples;
        spreadSum += latest - earliest;
        spreadMax = std::max(spreadMax, latest - earliest);
    }

    void reportStopped() const
    {
        fatal_if(inParallelMode, "Fence diagnostics require stopped peers");
        uint64_t total = 0;
        for (auto *event : barrierEvent) {
            const auto &local = *static_cast<BarrierEvent *>(event);
            total += local.trace.callbacks;
            fatal_if(local.epoch != callbacks, "Fence role call count changed");
            fatal_if(owner.mailboxFence && local.trace.callbacks !=
                     (local.role == owner.fenceLeader ? callbacks : 0),
                     "Mailbox callback ran on the wrong role");
        }
        fatal_if(total != callbacks || barrierBaseline + callbacks != owner.barriers,
                 "Fence callback count changed");
        std::cout << "SPLIT_FENCE_COUNTS {\"mailbox_fence\":" << owner.mailboxFence
                  << ",\"fence_leader\":" << owner.fenceLeader
                  << ",\"baseline_barriers\":" << barrierBaseline
                  << ",\"callbacks\":" << callbacks
                  << ",\"barriers\":" << owner.barriers << ",\"role_callbacks\":[";
        for (unsigned role = 0; role < barrierEvent.size(); ++role) {
            if (role) std::cout << ',';
            std::cout << static_cast<BarrierEvent *>(barrierEvent[role])->trace.callbacks;
        }
        std::cout << "]}" << std::endl;
        if (!owner.syncTraceStride) return;
        const double samples = traceSamples ? traceSamples : 1;
        std::cout << "SPLIT_SYNC_TRACE {\"mailbox_fence\":" << owner.mailboxFence
                  << ",\"stride\":" << owner.syncTraceStride
                  << ",\"scope\":\"instrumented host diagnostic\""
                  << ",\"samples\":" << traceSamples
                  << ",\"arrival_spread_mean_ns\":" << spreadSum / samples
                  << ",\"arrival_spread_max_ns\":" << spreadMax << "}" << std::endl;
        for (auto *event : barrierEvent) {
            const auto &local = *static_cast<BarrierEvent *>(event);
            const auto &t = local.trace;
            fatal_if(t.samples != traceSamples, "Fence trace sample count changed");
            std::cout << "SPLIT_SYNC_ROLE {\"role\":" << local.role
                      << ",\"samples\":" << t.samples
                      << ",\"lag_sum_ns\":" << t.lagSum
                      << ",\"lag_mean_ns\":" << t.lagSum / samples
                      << ",\"lag_max_ns\":" << t.lagMax
                      << ",\"latest_count\":" << t.latestCount
                      << ",\"latest_tie_count\":" << t.latestTieCount
                      << ",\"total_sum_ns\":" << t.totalSum
                      << ",\"total_mean_ns\":" << t.totalSum / samples
                      << ",\"total_max_ns\":" << t.totalMax;
            if (!owner.mailboxFence) {
                std::cout << ",\"first_sum_ns\":" << t.firstSum
                          << ",\"first_mean_ns\":" << t.firstSum / samples
                          << ",\"first_max_ns\":" << t.firstMax
                          << ",\"second_sum_ns\":" << t.secondSum
                          << ",\"second_mean_ns\":" << t.secondSum / samples
                          << ",\"second_max_ns\":" << t.secondMax;
            }
            std::cout << "}" << std::endl;
        }
    }
    const char *description() const override { return "Split Ruby pre-arbitration fence"; }
    void process() override
    {
        owner.importNativeAsync();
        owner.deliver();
        Tick next = owner.nextFenceTick();
        // Every queue is at this global rendezvous. Ordinary GlobalEvent::
        // schedule uses async insertion, which would expose this minimum-
        // priority fence only at the *end* of the next native quantum. Arm
        // directly while all peers are stopped in the second legacy barrier
        // or awaiting mailbox completion. No model thread schedules concurrently.
        for (uint32_t role = 0; role < numMainEventQueues; ++role) {
            EventQueue::ScopedMigration context(getEventQueue(role));
            getEventQueue(role)->schedule(barrierEvent[role], next);
        }
        ++callbacks;
    }
};

SplitRubyThreadBridge::SplitRubyThreadBridge(const Params &p)
    : SimObject(p), ruby(p.ruby_system), controlCpu(p.control_cpu), mode(p.mode),
      spinWaitRequested(p.spin_wait), fuseSync(p.fuse_sync),
      mailboxFence(p.mailbox_fence), fenceLeader(p.fence_leader),
      syncTraceStride(p.sync_trace_stride), quantum(p.quantum),
      queueCount(p.queue_count), valueTransport(p.value_transport), exerciseIo(p.exercise_io),
      managers(p.managers), managerRoles(p.manager_roles), routerRoles(p.router_roles),
      buffers(p.buffers), sources(p.source_roles), destinations(p.destination_roles),
      controllers(p.controllers), routers(p.routers),
      pending(queueCount), sent(queueCount, 0), calls(queueCount), stagingTicks(buffers.size(), 0)
{
    fatal_if(mode != "native" && mode != "codec" && mode != "thread", "Unknown thread bridge mode");
    fatal_if(fuseSync && mode != "thread", "Fused synchronization requires thread mode");
    fatal_if(mailboxFence && (mode != "thread" || !spinWaitRequested || !fuseSync),
             "Mailbox fence requires thread+spin_wait+fuse_sync");
    fatal_if(mailboxFence && fenceLeader >= queueCount, "Invalid mailbox fence leader");
    fatal_if(syncTraceStride && mode != "thread", "Sync tracing requires thread mode");
    fatal_if(!quantum || buffers.empty() || buffers.size() != sources.size() ||
             buffers.size() != destinations.size() || controllers.empty() ||
             controllers.size() != routers.size() || queueCount < 2 || queueCount > 8 ||
             managers.size() != managerRoles.size() ||
             (!routerRoles.empty() && routerRoles.size() != routers.size()), "Invalid thread partition");
    fatal_if(!split::supportsProtocol(ruby) ||
             ruby->getBlockSizeBytes() != 64, "Thread bridge requires 64-byte MESI_Three_Level");
    for (unsigned i = 0; i < buffers.size(); ++i) {
        fatal_if(sources[i] < 0 || unsigned(sources[i]) >= queueCount || destinations[i] < 0 ||
                 unsigned(destinations[i]) >= queueCount || destinations[i] == sources[i] ||
                 int(static_cast<const MessageBuffer::Params &>(buffers[i]->params()).buffer_size) != 0,
                 "Invalid thread cut ownership/capacity");
    }
    // Preparation remains genuinely serial. The checked promotion gate
    // creates the worker only when Python activates the ready RX workload.
}

SplitRubyThreadBridge::~SplitRubyThreadBridge() = default;

void SplitRubyThreadBridge::moveIsland(EventQueue *from, EventQueue *to)
{
    fatal_if(inParallelMode || from->getCurTick() != to->getCurTick(),
             "HNF migration requires all queues stopped at one tick");
    for (auto *object : controllers) {
        auto *consumer = dynamic_cast<Consumer *>(object);
        fatal_if(!consumer || object->eventQueue() != from, "Invalid HNF consumer ownership");
        consumer->moveWakeupWhileStopped(from, to);
        object->setEventQueueWhileStopped(to);
    }
    for (auto *object : routers) {
        auto *router = dynamic_cast<Switch *>(object);
        fatal_if(!router || object->eventQueue() != from, "Invalid HNF router ownership");
        router->moveConsumersWhileStopped(from, to);
        object->setEventQueueWhileStopped(to);
    }
}

void SplitRubyThreadBridge::movePartition(bool activate)
{
    std::vector<std::pair<EventManager *, EventQueue *>> moves;
    for (unsigned i = 0; i < managers.size(); ++i) {
        fatal_if(managerRoles[i] < 0 || unsigned(managerRoles[i]) >= queueCount,
                 "Invalid manager role");
        if (managerRoles[i]) moves.emplace_back(managers[i], getEventQueue(activate ? managerRoles[i] : 0));
    }
    // New partitions include all controller/router descendants in managers.
    for (unsigned source = 0; source < queueCount; ++source) {
        std::vector<std::pair<EventManager *, EventQueue *>> local;
        for (const auto &move : moves)
            if (move.first->eventQueue() == getEventQueue(source)) local.push_back(move);
        auto count = getEventQueue(source)->moveManagedEventsWhileStopped(local);
        if (count) std::cout << "SPLIT_CPU_MIGRATE source=" << source
                            << " events=" << count << std::endl;
    }
    for (const auto &move : moves) move.first->setEventQueueWhileStopped(move.second);
}

bool SplitRubyThreadBridge::defer(EventManager *destination, Tick when,
                                  std::function<void()> callback, bool force)
{
    if (!running || (!force && destination->eventQueue() == curEventQueue())) return false;
    auto &bridge = *running;
    fatal_if(when < curTick() ||
             ((!bridge.insideFence && inParallelMode) && when - curTick() < bridge.quantum),
             "Cross-partition IO lacks native latency lookahead");
    unsigned source = 0;
    while (source < bridge.queueCount && getEventQueue(source) != curEventQueue()) ++source;
    fatal_if(source >= bridge.queueCount, "Unknown IO source queue");
    bridge.calls[source].push_back({destination, when, std::move(callback)});
    return true;
}

Tick SplitRubyThreadBridge::nextFenceTick() const
{
    // Retain the original lattice and add only known IO deadlines between
    // its points. Every deadline is published at least one quantum ahead.
    Tick next = activation + ((curTick() - activation) / quantum + 1) * quantum;
    for (const auto &batch : calls)
        for (const auto &call : batch) {
            fatal_if(call.when <= curTick(), "Unconsumed IO deadline");
            next = std::min(next, call.when);
        }
    return next;
}

class SplitRubyThreadBridge::TestPacketQueue : public PacketQueue
{
    std::string labelName;
    EventManager &manager;
  public:
    TestPacketQueue *sameTickPeer = nullptr;
    TestPacketQueue(EventManager &owner, const std::string &name)
        : PacketQueue(owner, name, name), labelName(name), manager(owner) {}
    ~TestPacketQueue() override = default;
    const std::string name() const override { return labelName; }
    bool sendTiming(PacketPtr packet) override
    {
        fatal_if(curTick() != packet->getAddr(), "Queued interrupt deadline changed");
        std::cout << "SPLIT_CPU_QUEUE_TEST_PASS tick=" << curTick() << std::endl;
        delete packet;
        if (sameTickPeer) {
            auto *peer = sameTickPeer;
            sameTickPeer = nullptr;
            EventQueue::ScopedMigration context(peer->manager.eventQueue());
            auto req = std::make_shared<Request>(curTick(), 1, 0, Request::intRequestorId);
            auto *followup = new Packet(req, MemCmd::WriteReq);
            followup->allocate();
            // The destination queue is idle. Its native send was already
            // due at this tick before the producer's deferred callback.
            peer->schedSendTimingAtFence(followup, curTick(), false);
        }
        return true;
    }
    void add(Tick when)
    {
        auto req = std::make_shared<Request>(when, 1, 0, Request::intRequestorId);
        auto *packet = new Packet(req, MemCmd::WriteReq);
        packet->allocate();
        schedSendTiming(packet, when);
    }
};

void SplitRubyThreadBridge::scheduleIoTests()
{
    for (unsigned role = 0; role < queueCount; ++role)
        testManagers.push_back(std::make_unique<EventManager>(getEventQueue(role)));
    // These packets exist before activation of the queue gate, including
    // two deadlines only one tick apart (the native anti-recursion gap).
    auto initial = std::make_unique<TestPacketQueue>(*testManagers[0], "initial IRQ test");
    initial->add(activation + 201);
    initial->add(activation + 202);
    initial->enableTimingFenceWhileStopped();
    testQueues.push_back(std::move(initial));
    auto live = std::make_unique<TestPacketQueue>(*testManagers[1], "live IRQ test");
    live->enableTimingFenceWhileStopped();
    auto *liveQueue = live.get();
    liveQueue->sameTickPeer = testQueues.front().get();
    testQueues.push_back(std::move(live));
    auto *issue = new EventFunctionWrapper([this, liveQueue] {
        liveQueue->add(activation + 2344);
    }, "Live IRQ test enqueue", true);
    testManagers[1]->schedule(issue, activation + 1344);
    for (unsigned index = 0; index < 4; ++index) {
        unsigned source = index % queueCount;
        unsigned destination = (index + 1) % queueCount;
        if (index == 3) destination = source; // Forced central CPU IO also fences.
        Tick issue = activation + 123 + index * 1777;
        if (index == 2) issue = 1000123; // During the first coherent transaction.
        Tick arrival = issue + quantum;
        auto *event = new EventFunctionWrapper([this, destination, arrival] {
            bool queued = defer(testManagers[destination].get(), arrival,
                [destination, arrival] {
                    fatal_if(curTick() != arrival || curEventQueue() != getEventQueue(destination),
                             "Directed IO changed its native arrival time or owner");
                    std::cout << "SPLIT_CPU_IO_TEST_PASS tick=" << curTick()
                              << " destination=" << destination << std::endl;
                }, true);
            fatal_if(!queued, "Directed IO failed to enter the handoff");
        }, "Split exact IO test", true);
        testManagers[source]->schedule(event, issue);
    }
}

void SplitRubyThreadBridge::activate()
{
    fatal_if(active || inParallelMode || curEventQueue() != getEventQueue(0),
             "Thread bridge activation requires stopped main queue");
    if (mode == "native") return;
    fatal_if(ruby->getRandomization(), "Thread bridge requires deterministic Ruby");
    activation = curTick();
    if (mode == "thread") {
        if (spinWaitRequested) Barrier::useSpinForNewBarriers(true);
        addStoppedEventQueues(queueCount);
        fatal_if(numMainEventQueues != queueCount || simQuantum != quantum,
                 "Thread bridge requires matching queues and native quantum");
        if (managers.empty()) moveIsland(getEventQueue(0), getEventQueue(1));
        else movePartition(true);
    }
    for (unsigned i = 0; i < buffers.size(); ++i)
        buffers[i]->setSplitEnqueue([this, i](MsgPtr message, Tick issued, Tick latency,
                bool random, bool warmup, bool bypass) {
            capture(i, std::move(message), issued, latency, random, warmup, bypass);
        }, mode == "codec");
    active = true;
    if (mode == "thread") {
        fatal_if(running, "Another thread bridge is active");
        running = this;
        fatal_if(timingFenceHandoff || timingFenceIsSafe || timingFenceOwnsQuantumSync,
                 "Another timing fence is installed");
        timingFenceHandoff = &SplitRubyThreadBridge::defer;
        timingFenceIsSafe = &SplitRubyThreadBridge::safeHandoffContext;
        if (fuseSync) {
            timingFenceAsyncPublishes.store(0, std::memory_order_relaxed);
            importedAsyncPublishes = 0;
            timingFenceOwnsQuantumSync = true;
        }
        for (auto *queue : timingFencePacketQueues()) queue->enableTimingFenceWhileStopped();
        std::cout << "SPLIT_CPU_IRQ_QUEUES count=" << timingFencePacketQueues().size() << std::endl;
        if (exerciseIo) scheduleIoTests();
        fence = std::make_unique<Fence>(*this);
        fence->schedule(curTick());
    }
    std::cout << "SPLIT_FS_ACTIVATED mode=" << mode << " tick=" << activation
              << " controllers=" << controllers.size() << std::endl;
}

void SplitRubyThreadBridge::capture(unsigned channel, MsgPtr message, Tick issued,
        Tick latency, bool random, bool warmup, bool bypass)
{
    fatal_if(!active || random || warmup || latency < quantum || issued != curTick(),
             "Unsupported full-system Ruby crossing");
    // DMA RequestPtr is process-local provenance in the native protocol.
    // This backend shares one process: keep that reference outside the
    // pointer-free value codec, and restore it before the destination sees
    // the message. The HNF protocol retains it in its TBE, without mutation.
    // The multiprocess codec continues to reject pointers unchanged.
    auto snapshot = message->clone();
    const auto local = split::detachLocalRequest(*snapshot);
    std::vector<uint8_t> bytes;
    if (!valueTransport) bytes = split::encode(snapshot);
    Frame frame{channel, issued, latency, bypass, std::move(bytes),
                valueTransport ? snapshot : MsgPtr{}, local.request, local.valid};
    unsigned source = sources[channel];
    if (mode == "codec") {
        auto value = decode(frame);
        buffers[channel]->enqueue(value, issued, latency, false, false, bypass, true);
    } else {
        fatal_if(curEventQueue() != getEventQueue(source), "Thread produced unowned Ruby message");
        fatal_if(pending[source].size() >= 16384, "Thread bridge batch exceeded bound");
        pending[source].push_back(std::move(frame));
    }
    ++sent[source];
}

MsgPtr SplitRubyThreadBridge::decode(const Frame &frame)
{
    auto value = valueTransport ? frame.value : split::decode(frame.bytes, ruby);
    split::attachLocalRequest(*value, {frame.localRequest, frame.localRequestValid});
    return value;
}

void SplitRubyThreadBridge::importNativeAsync()
{
    if (!fuseSync) return;
    auto published = timingFenceAsyncPublishes.load(std::memory_order_acquire);
    if (published == importedAsyncPublishes) return;
    // At a fence (or stopped deactivation), every model producer is stopped.
    // The guarded global exits have at least quantum lookahead. Import them
    // before delivery schedules same-tick ordinary work on the destination.
    for (uint32_t role = 0; role < numMainEventQueues; ++role) {
        EventQueue::ScopedMigration context(getEventQueue(role));
        getEventQueue(role)->handleAsyncInsertions();
    }
    importedAsyncPublishes = published;
}

void SplitRubyThreadBridge::deliver()
{
    Tick now = curTick();
    insideFence = true;
    for (unsigned source = 0; source < queueCount; ++source) {
        for (const auto &frame : pending[source]) {
            fatal_if(frame.issued >= now || frame.latency > MaxTick - frame.issued ||
                     frame.issued + frame.latency < now, "Late full-system Ruby delivery");
            stagingTicks[frame.channel] += now - frame.issued;
            auto value = decode(frame);
            EventQueue::ScopedMigration context(getEventQueue(destinations[frame.channel]));
            buffers[frame.channel]->enqueue(value, frame.issued, frame.latency,
                                            false, false, frame.bypass, true);
        }
        pending[source].clear();
    }
    bool consumed;
    unsigned dispatched = 0;
    do {
        consumed = false;
        for (unsigned source = 0; source < queueCount; ++source) {
            // Detach batches for reentrancy. A same-tick producer can append
            // to an already-visited source; drain it at this rendezvous too.
            auto batch = std::move(calls[source]);
            calls[source].clear();
            for (auto &call : batch) {
                fatal_if(call.when < now, "Late cross-partition IO delivery");
                if (call.when > now) {
                    calls[source].push_back(std::move(call));
                    continue;
                }
                fatal_if(++dispatched > 16384, "Unbounded same-tick IO callback chain");
                EventQueue::ScopedMigration context(call.destination->eventQueue());
                call.callback();
                consumed = true;
                ++deferredCalls;
            }
        }
    } while (consumed);
    insideFence = false;
    ++barriers;
}

void SplitRubyThreadBridge::deactivate()
{
    fatal_if(inParallelMode, "Thread bridge deactivation requires stopped queues");
    if (!active) return;
    if (mode == "thread") {
        // Python has stopped all queues at a native global exit. Remaining
        // handoffs have future arrival times; inject them without advancing
        // time before moving all singular state back to the main queue.
        importNativeAsync();
        if (fence->scheduled()) fence->deschedule();
        fence->reportStopped();
        // Restore each queue's pending event before stale callbacks are
        // flushed; restoration retains its original timestamp exactly.
        for (auto *queue : timingFencePacketQueues()) queue->disableTimingFenceWhileStopped();
        for (auto &queue : testQueues) queue->disableTimingFenceWhileStopped();
        for (unsigned source = 0; source < queueCount; ++source) {
            for (const auto &frame : pending[source]) {
                fatal_if(frame.issued + frame.latency < curTick(), "Late deactivation handoff");
                stagingTicks[frame.channel] += curTick() - frame.issued;
                auto value = decode(frame);
                buffers[frame.channel]->enqueue(value, frame.issued, frame.latency,
                                                false, false, frame.bypass, true);
            }
            pending[source].clear();
        }
        // Flush only the enqueue operation, retaining the original future time.
        for (auto &batch : calls) {
            for (auto &call : batch) {
                fatal_if(call.when < curTick(), "Late stopped IO handoff");
                EventQueue::ScopedMigration context(call.destination->eventQueue());
                call.callback();
                ++deferredCalls;
            }
            batch.clear();
        }
        timingFenceOwnsQuantumSync = false;
        timingFenceHandoff = nullptr;
        timingFenceIsSafe = nullptr;
        running = nullptr;
        if (managers.empty()) moveIsland(getEventQueue(1), getEventQueue(0));
        else movePartition(false);
        if (spinWaitRequested) Barrier::useSpinForNewBarriers(false);
    }
    for (auto *buffer : buffers) buffer->clearSplitEnqueueWhileStopped();
    active = false;
    report();
}

void SplitRubyThreadBridge::report()
{
    std::cout << "SPLIT_FS_REPORT mode=" << mode << " active=" << active
              << " tick=" << curTick() << " activation=" << activation
              << " sent0=" << sent[0] << " sent1=" << sent[1]
              << " queues=" << queueCount << " deferred_io=" << deferredCalls
              << " value_transport=" << valueTransport << " barriers=" << barriers << " spin_wait=" << spinWaitRequested
              << " fuse_sync=" << fuseSync << " mailbox_fence=" << mailboxFence
              << " fence_leader=" << fenceLeader << " sync_trace_stride=" << syncTraceStride
              << " native_async_publishes=" << importedAsyncPublishes
              << " pending0=" << pending[0].size()
              << " pending1=" << pending[1].size()
              << " occupancy_contract=physical-buffer-plus-explicit-staging-v1" << std::endl;
    for (unsigned channel = 0; channel < buffers.size(); ++channel)
        if (stagingTicks[channel])
            std::cout << "SPLIT_FS_STAGE buffer=" << buffers[channel]->name()
                      << " ticks=" << stagingTicks[channel] << std::endl;
}

std::vector<uint8_t> SplitRubyThreadBridge::readGuest(Addr address, unsigned size)
{
    fatal_if(active || inParallelMode || !controlCpu || !size || size > 1048576,
             "Guest observation requires stopped, native authoritative state");
    std::vector<uint8_t> bytes(size);
    TranslatingPortProxy proxy(controlCpu->getContext(0));
    proxy.readBlob(address, bytes.data(), size);
    return bytes;
}

void SplitRubyThreadBridge::exerciseSync(const std::string &test, Tick issue, Tick when)
{
    fatal_if(!exerciseIo || !active || mode != "thread" || inParallelMode ||
             issue <= curTick() || when <= issue,
             "Sync exercise requires a stopped directed thread partition");
    fatal_if(test != "exit" && test != "repeat-exit" && test != "short" && test != "ordinary",
             "Unknown native async sync exercise");
    auto *event = new EventFunctionWrapper([test, when] {
        if (test == "ordinary") {
            auto *arrival = new EventFunctionWrapper([when] {
                fatal_if(curTick() != when, "Native async exercise arrived late");
                std::cout << "SPLIT_SYNC_ASYNC_MODEL_PASS tick=" << curTick() << std::endl;
            }, "Native async model exercise", true);
            getEventQueue(0)->schedule(arrival, when);
        } else {
            new GlobalSimLoopExitEvent(when, "Split async exit exercise", 0,
                                       test == "repeat-exit" ? simQuantum : 0);
        }
    }, "Publish native async sync exercise", true);
    getEventQueue(1)->schedule(event, issue);
}

void SplitRubyThreadBridge::serialize(CheckpointOut &cp) const
{
    fatal_if(active, "Cannot checkpoint an active HNF thread island");
    unsigned version = 1;
    SERIALIZE_SCALAR(version);
}

void SplitRubyThreadBridge::unserialize(CheckpointIn &cp)
{
    unsigned version;
    UNSERIALIZE_SCALAR(version);
    fatal_if(version != 1 || active, "Invalid dormant thread bridge checkpoint");
}
} }
