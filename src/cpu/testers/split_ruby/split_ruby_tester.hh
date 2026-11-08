/*
 * Copyright (c) 2026 minhuw
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CPU_TESTERS_SPLIT_RUBY_SPLIT_RUBY_TESTER_HH__
#define __CPU_TESTERS_SPLIT_RUBY_SPLIT_RUBY_TESTER_HH__

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/SplitRubyDirectedTester.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{

class SplitRubyDirectedTester : public ClockedObject
{
  private:
    class TestPort : public RequestPort
    {
      private:
        SplitRubyDirectedTester &tester;

      public:
        TestPort(const std::string &name, SplitRubyDirectedTester &tester,
                 PortID id = InvalidPortID)
            : RequestPort(name, id), tester(tester)
        {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
        void recvTimingSnoopReq(PacketPtr pkt) override {}
        void recvFunctionalSnoop(PacketPtr pkt) override {}
        Tick recvAtomicSnoop(PacketPtr pkt) override { return 0; }
    };

    struct Operation
    {
        unsigned phase;
        unsigned port; // CPU0, CPU1, DMA = 0, 1, 2.
        bool read;
        Addr address;
        std::vector<uint8_t> data;
        RequestPtr request;
        PacketPtr packet = nullptr;
        bool attempted = false;
        bool accepted = false;
        bool retryPending = false;
        unsigned completions = 0;
    };

    bool owns(const Operation &op) const;
    TestPort &portFor(const Operation &op);
    void issuePhase();
    void retry(TestPort &port);
    void complete(TestPort &port, PacketPtr pkt);
    void checkOperations() const;
    void localCheck();

    EventFunctionWrapper phaseEvent;
    EventFunctionWrapper checkEvent;
    std::array<std::unique_ptr<TestPort>, 2> cpuPorts;
    TestPort dmaPort;
    std::vector<Operation> operations;
    unsigned nextPhase = 1;
    bool localPassed = false;
    const int role;
    const Tick phaseTicks;
    const Tick stopTick;
    const Tick checkTick;
    const RequestorID requestorId;

  public:
    using Params = SplitRubyDirectedTesterParams;
    explicit SplitRubyDirectedTester(const Params &params);
    ~SplitRubyDirectedTester() override;
    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    void startup() override;
    void assertComplete() const;
};

} // namespace gem5

#endif // __CPU_TESTERS_SPLIT_RUBY_SPLIT_RUBY_TESTER_HH__
