// SPDX-License-Identifier: BSD-3-Clause

#ifndef __TEST_OBJECTS_PVRDMA_TESTER_HH__
#define __TEST_OBJECTS_PVRDMA_TESTER_HH__

#include <string>

#include "dev/platform.hh"
#include "dev/rdma/pvrdma_abi.hh"
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
        UserContext,
        UserPd,
        Cq,
        Qp,
        Init,
        Rtr,
        Rts,
        Query,
        DestroyQp,
        BadQp,
        BadQpEarly,
        BadQpLate,
        DestroyCq,
        ObservationActive,
        ObservationMalformed,
        ObservationReset,
        ObservationDone,
        StatsPosted,
        StatsAdvance,
        StatsDone,
        StatsReposted,
        CheckpointObservationReady,
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
    void prepareQueuePages(Addr directory, Addr table, Addr first_page,
                           uint32_t pages, bool malformed = false);
    void startUserContext(uint64_t response);
    void startUserPd(uint64_t response);
    void startCq(uint64_t response, uint32_t cqe = 64);
    void startQp(uint64_t response, bool malformed = false);
    void startModifyQp(uint64_t response, pvrdma::QpState state,
                       uint32_t mask, const pvrdma::QpAttr &attributes);
    void startQueryQp(uint64_t response);
    pvrdma::CommandResponse verifyResponse(pvrdma::Command command,
                                           uint64_t response);
    void destroyQp();
    void destroyCq();
    void createUserParentAtomic();
    void createQueuePairAtomic(uint32_t expected_qpn);
    void moveQueuePairToRtsAtomic();
    void postObservedRings(uint32_t sq, uint32_t rq);
    void ringDoorbell(uint32_t action, uint32_t handle = 1,
                      bool cq = false);
    void runTimingMr();
    void runTimingQueues();
    void runStatsReset();
    void testCheckpointSave();
    void testCheckpointRestore();
    void testCheckpointObservationSave();
    void testCheckpointObservationRestore();

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
