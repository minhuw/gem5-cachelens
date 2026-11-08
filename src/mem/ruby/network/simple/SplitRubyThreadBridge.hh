// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_RUBY_NETWORK_SIMPLE_SPLIT_RUBY_THREAD_BRIDGE_HH__
#define __MEM_RUBY_NETWORK_SIMPLE_SPLIT_RUBY_THREAD_BRIDGE_HH__
#include <functional>
#include <memory>
#include <vector>
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/request.hh"
#include "params/SplitRubyThreadBridge.hh"
#include "sim/sim_object.hh"

namespace gem5 {
class ClockedObject;
class BaseCPU;
namespace ruby {
class RubySystem;
class SplitRubyThreadBridge : public SimObject
{
  private:
    class Fence;
    class TestPacketQueue;
    std::vector<std::unique_ptr<TestPacketQueue>> testQueues;
    struct Frame {
        unsigned channel;
        Tick issued, latency;
        bool bypass;
        std::vector<uint8_t> bytes;
        MsgPtr value;
        RequestPtr localRequest;
        bool localRequestValid;
    };
    RubySystem *ruby;
    BaseCPU *controlCpu;
    const std::string mode;
    const bool spinWaitRequested;
    const bool fuseSync;
    const bool mailboxFence;
    const unsigned fenceLeader;
    const unsigned syncTraceStride;
    uint64_t importedAsyncPublishes = 0;
    const Tick quantum;
    const unsigned queueCount;
    const bool valueTransport;
    const bool exerciseIo;
    std::vector<std::unique_ptr<EventManager>> testManagers;
    const std::vector<SimObject *> managers;
    const std::vector<int> managerRoles, routerRoles;
    const std::vector<MessageBuffer *> buffers;
    const std::vector<int> sources, destinations;
    const std::vector<ClockedObject *> controllers, routers;
    std::unique_ptr<Fence> fence;
    std::vector<std::vector<Frame>> pending;
    std::vector<uint64_t> sent;
    struct Call { EventManager *destination; Tick when; std::function<void()> callback; };
    std::vector<std::vector<Call>> calls;
    static SplitRubyThreadBridge *running;
    uint64_t deferredCalls = 0;
    std::vector<uint64_t> stagingTicks;
    uint64_t barriers = 0;
    bool active = false;
    bool insideFence = false;
    Tick activation = 0;
    void capture(unsigned channel, MsgPtr message, Tick issued, Tick latency,
                 bool random, bool warmup, bool bypass);
    void importNativeAsync();
    void deliver();
    MsgPtr decode(const Frame &frame);
    void moveIsland(EventQueue *from, EventQueue *to);
    void movePartition(bool activate);
    Tick nextFenceTick() const;
    void scheduleIoTests();
  public:
    using Params = SplitRubyThreadBridgeParams;
    explicit SplitRubyThreadBridge(const Params &p);
    ~SplitRubyThreadBridge() override;
    static bool defer(EventManager *destination, Tick when, std::function<void()> callback, bool force = false);
    static bool isActive() { return running != nullptr; }
    static bool safeHandoffContext() { return running && (running->insideFence || !inParallelMode); }
    void activate();
    void deactivate();
    void report();
    void exerciseSync(const std::string &test, Tick issue, Tick when);
    std::vector<uint8_t> readGuest(Addr address, unsigned size);
    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;
};
} }
#endif
