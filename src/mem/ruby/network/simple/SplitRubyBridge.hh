// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_RUBY_NETWORK_SIMPLE_SPLIT_RUBY_BRIDGE_HH__
#define __MEM_RUBY_NETWORK_SIMPLE_SPLIT_RUBY_BRIDGE_HH__

#include <cstdint>
#include <vector>
#include "mem/ruby/network/MessageBuffer.hh"
#include "params/SplitRubyBridge.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5 {
class BaseCPU;
namespace ruby {
class RubySystem;

class SplitRubyBridge : public SimObject
{
  private:
    struct Frame {
        uint64_t channel, issued, latency, sequence;
        bool bypass;
        std::vector<uint8_t> message;
    };
    struct SharedRegion;
    RubySystem *ruby;
    const std::vector<MessageBuffer *> buffers, monitors;
    const std::vector<int> sources, destinations;
    const int role, numRoles;
    const std::string mode, socketPath, configTag, transport;
    const bool adaptive;
    const Tick quantum, stopTick;
    const std::vector<BaseCPU *> ownedCpus, inactiveCpus;
    const uint64_t expectedChecksum;
    EventFunctionWrapper barrierEvent;
    int socketFd = -1, listenFd = -1;
    std::vector<int> peerFds;
    SharedRegion *shared = nullptr;
    uint64_t epoch = 0;
    uint64_t sent = 0, received = 0, barriers = 0, skipped = 0;
    std::vector<uint64_t> producerSequences;
    std::vector<Frame> pending;
    std::vector<uint8_t> batch;
    std::vector<std::vector<uint8_t>> inputs;
    void openPeer();
    void transfer(void *data, size_t size, bool sending);
    std::vector<uint8_t> exchange(const std::vector<uint8_t> &data);
    void mapShared(int fd);
    void shareFd(int fd, bool sending);
    void waitValue(uint64_t *address, uint64_t value);
    void exchangeShared();
    void send(unsigned channel, MsgPtr msg, Tick issued, Tick latency,
              bool random, bool warmup, bool bypass);
    void barrier();

  public:
    using Params = SplitRubyBridgeParams;
    explicit SplitRubyBridge(const Params &params);
    ~SplitRubyBridge() override;
    void init() override;
    void startup() override;
    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;
};
} }
#endif
