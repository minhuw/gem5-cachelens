// SPDX-License-Identifier: BSD-3-Clause

#ifndef __TEST_OBJECTS_PVRDMA_TESTER_HH__
#define __TEST_OBJECTS_PVRDMA_TESTER_HH__

#include "dev/platform.hh"
#include "mem/port.hh"
#include "params/PvrdmaTester.hh"
#include "sim/eventq.hh"

namespace gem5
{

class PvrdmaTester : public Platform
{
  private:
    class TestPort : public RequestPort
    {
      public:
        TestPort(const std::string &name, PvrdmaTester &tester)
            : RequestPort(name), tester(tester)
        {}

      private:
        PvrdmaTester &tester;
        bool recvTimingResp(PacketPtr) override;
        void recvReqRetry() override;
    };

    TestPort port;
    RequestorID requestorId;
    EventFunctionWrapper testEvent;
    const bool commandTest;
    bool dsrConfigured = false;

    template <typename T>
    void write(Addr addr, const T &value, Request::Flags flags = 0);

    template <typename T>
    T read(Addr addr, Request::Flags flags = 0);

    void run();
    void configurePci();
    void configureDsr();
    void testCapabilities();
    void testCommand();

  public:
    PARAMS(PvrdmaTester);
    PvrdmaTester(const Params &params);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    void startup() override;
    void postConsoleInt() override {}
    void clearConsoleInt() override {}
};

} // namespace gem5

#endif // __TEST_OBJECTS_PVRDMA_TESTER_HH__
