// SPDX-License-Identifier: BSD-3-Clause

#ifndef __TEST_OBJECTS_PVRDMA_TESTER_HH__
#define __TEST_OBJECTS_PVRDMA_TESTER_HH__

#include <string>

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

    enum class TimingStage
    {
        Configure,
        Pd,
        Mr512Pio,
        Mr512Done,
        Mr513Pio,
        Mr513Done,
        BadMrPio,
        BadMrDone,
        LateMrDone,
    };

    TestPort port;
    RequestorID requestorId;
    EventFunctionWrapper testEvent;
    const bool commandTest;
    const std::string testMode;
    bool dsrConfigured = false;
    TimingStage timingStage = TimingStage::Configure;
    uint32_t pioError = 0;

    template <typename T>
    void write(Addr addr, const T &value, Request::Flags flags = 0);

    template <typename T>
    T read(Addr addr, Request::Flags flags = 0);

    void run();
    void configurePci();
    void configureDsr();
    void testCapabilities();
    void testCommand();
    void activateAndCreatePd();
    void verifyPd();
    void prepareMrPages(uint32_t pages, bool malformed);
    void startMr(uint32_t pages, uint64_t response);
    uint32_t verifyMr(uint64_t response, uint32_t key);
    void destroyMr(uint32_t handle);
    void runTimingMr();
    void testCheckpointSave();
    void testCheckpointRestore();

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
