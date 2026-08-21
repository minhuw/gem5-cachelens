/*
 * Copyright (c) 2026 minhuw
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_TESTERS_MESI_DDIO_MESI_DDIO_TESTER_HH__
#define __CPU_TESTERS_MESI_DDIO_MESI_DDIO_TESTER_HH__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/MESIDDIODirectedTester.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{

class MESIDDIODirectedTester : public ClockedObject
{
  private:
    enum class PortKind
    {
        Cpu,
        Dma,
    };

    struct Operation
    {
        unsigned phase;
        unsigned issueDelay;
        PortKind port;
        unsigned cpu;
        bool read;
        Addr address;
        Request::Flags flags;
        std::vector<uint8_t> data;
        std::string label;

        PacketPtr packet = nullptr;
        bool issued = false;
        unsigned completions = 0;
    };

    class TestPort : public RequestPort
    {
      private:
        MESIDDIODirectedTester &tester;

      public:
        TestPort(const std::string &name, MESIDDIODirectedTester &tester,
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

    struct RetryState
    {
        TestPort *port;
        PacketPtr packet;
        size_t operation;
    };

    void buildScenario();
    void issue();
    void finish();
    void complete(TestPort &port, PacketPtr pkt);
    void retry(TestPort &port);
    void timeout();

    unsigned reservePhase();
    void addRead(PortKind port, unsigned cpu, Addr address,
                 Request::Flags flags, const std::vector<uint8_t> &expected,
                 const std::string &label);
    void addWrite(PortKind port, unsigned cpu, Addr address,
                  Request::Flags flags, const std::vector<uint8_t> &data,
                  const std::string &label);
    void addReadAt(unsigned phase, unsigned issue_delay, PortKind port,
                   unsigned cpu, Addr address, Request::Flags flags,
                   const std::vector<uint8_t> &expected,
                   const std::string &label);
    void addWriteAt(unsigned phase, unsigned issue_delay, PortKind port,
                    unsigned cpu, Addr address, Request::Flags flags,
                    const std::vector<uint8_t> &data,
                    const std::string &label);

    std::vector<uint8_t> bytes(unsigned size, uint8_t seed) const;
    std::vector<uint8_t> pattern(uint8_t seed) const;
    TestPort &portFor(const Operation &op);
    size_t retryIndex(const TestPort &port) const;
    bool phaseComplete(unsigned phase) const;
    void issueOperation(size_t index);
    void noteProgress();

    EventFunctionWrapper issueEvent;
    EventFunctionWrapper finishEvent;
    EventFunctionWrapper timeoutEvent;

    std::vector<TestPort *> cpuPorts;
    TestPort dmaPort;
    std::vector<Operation> operations;
    std::vector<RetryState> retries;
    unsigned phaseCount = 0;
    unsigned currentPhase = 0;
    Tick phaseStart = 0;
    bool finishing = false;

    const std::string scenario;
    const Addr baseAddress;
    const Addr setStride;
    const unsigned blockSize;
    const unsigned routingBanks;
    const Cycles responseTimeout;
    const Cycles completionQuietPeriod;
    const RequestorID requestorId;

  public:
    using Params = MESIDDIODirectedTesterParams;

    explicit MESIDDIODirectedTester(const Params &params);
    ~MESIDDIODirectedTester() override;

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    void startup() override;
};

} // namespace gem5

#endif // __CPU_TESTERS_MESI_DDIO_MESI_DDIO_TESTER_HH__
