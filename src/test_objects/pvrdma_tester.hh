// SPDX-License-Identifier: BSD-3-Clause

#ifndef __TEST_OBJECTS_PVRDMA_TESTER_HH__
#define __TEST_OBJECTS_PVRDMA_TESTER_HH__

#include <memory>
#include <string>
#include <vector>

#include "dev/platform.hh"
#include "dev/rdma/pvrdma.hh"
#include "mem/port.hh"
#include "params/PvrdmaTester.hh"
#include "sim/eventq.hh"
#include "sim/probe/mem.hh"
#include "test_objects/pvrdma_test_link.hh"

namespace gem5
{

class Pvrdma;
class PvrdmaTestLink;

class PvrdmaTester : public Platform
{
  private:
    class FaultPort : public EtherInt
    {
      public:
        FaultPort(const std::string &name, PvrdmaTester &tester, int side)
            : EtherInt(name), tester(tester), side(side)
        {}

        bool recvPacket(EthPacketPtr packet) override;
        void sendDone() override;

      private:
        PvrdmaTester &tester;
        const int side;
    };

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
        ObservationCommandActive,
        ObservationCommandDone,
        ObservationMalformed,
        ObservationReset,
        ObservationDone,
        StatsPosted,
        StatsAdvance,
        StatsDone,
        StatsReposted,
        CheckpointObservationReady,
        CompletionSendDone,
        CompletionReceiveDone,
        CompletionErrorDone,
        CompletionReclaimed,
        CompletionWrapDone,
        CompletionDestroyDone,
        CompletionDone,
        CompletionCqFull,
        CompletionMalformed,
        CompletionTimingCqe,
        CompletionTimingCqProducer,
        PairPostSq,
        PairPollSq,
        PairMacWrite,
        PairPollInbound,
        PairVerify,
        PairVerifyRnr,
        PairPostShort,
        PairVerifyShort,
        PairPostOversized,
        PairVerifyOversized,
        FaultCheckBackpressure,
        FaultCheckHeld,
        FaultCheckReleased,
        FaultCheckDelayed,
        PairPostCq,
        PairVerifyCqBlocked,
        PairVerifyCqRecovered,
        PairVerifyStale,
        SemanticPostSq,
        SemanticVerify,
        RequestObservationPostSq,
        RequestObservationVerify,
        SemanticMalformedPost,
        SemanticMalformedVerify,
        SemanticMalformedValidVerify,
        SemanticRnrVerify,
        SemanticShortPost,
        SemanticShortVerify,
        ReliabilityPostSq,
        ReliabilityVerify,
        ReliabilityRnrPostRq,
        ReliabilityDeadlineRelease,
        ReliabilityTimeoutZeroObserve,
        ReliabilityTimeoutZeroVerify,
        ReliabilityInvalidInject,
        ReliabilityInvalidVerify,
        ReliabilitySequenceFutureInject,
        ReliabilitySequenceFutureVerify,
        ReliabilitySequenceRetryInject,
        ReliabilitySequenceRetryVerify,
        ReliabilityUnrelatedInject,
        ReliabilityUnrelatedVerify,
        ReliabilityUnrelatedComplete,
        ReliabilityCqBlocked,
        ReliabilityCqVerify,
        ReliabilityCqAbortHeld,
        ReliabilityCqAbortVerify,
        ReliabilityPrecommitInject,
        ReliabilityPrecommitCqInject,
        ReliabilityPrecommitVerify,
        ReliabilityCommitInject,
        ReliabilityCommitVerify,
        ReliabilityCommitReplayVerify,
        ReliabilityBoundaryInject,
        ReliabilityBoundaryTryAck,
        ReliabilityBoundaryVerify,
        TerminalInject,
        TerminalPartial,
        TerminalBackpressured,
        TerminalVerify,
    };

    TestPort port;
    FaultPort faultPort0;
    FaultPort faultPort1;
    RequestorID requestorId;
    EventFunctionWrapper testEvent;
    Pvrdma *rdma = nullptr;
    Pvrdma *peerRdma = nullptr;
    PvrdmaTestLink *testLink = nullptr;
    const bool commandTest;
    const std::string testMode;
    bool dsrConfigured = false;
    TimingStage timingStage = TimingStage::Configure;
    uint32_t pioError = 0;
    uint64_t faultSendDone[2] = {};
    bool faultRejectOnce[2] = {};
    bool faultDrainWhileRejected = false;
    std::vector<uint64_t> faultReceived[2];
    size_t reliabilityCase = 0;
    uint64_t reliabilityRxDmasBefore = 0;
    uint32_t busyPolls = 0;
    bool sawCqPublishPollRace = false;
    std::unique_ptr<ProbeListenerArgFunc<probing::PacketInfo>>
        senderDmaListener;
    std::unique_ptr<ProbeListenerArgFunc<probing::PacketInfo>>
        receiverDmaListener;
    uint64_t txPayloadRequests = 0;
    uint64_t rxPayloadRequests = 0;
    uint64_t unclassifiedWqeRequests = 0;
    uint64_t unclassifiedCqeRequests = 0;
    uint64_t unclassifiedConsumerRequests = 0;
    uint64_t unclassifiedControlRequests = 0;

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
    pvrdma::CompletionSubmitResult submitCompletion(
        pvrdma::CompletionOpcode opcode, pvrdma::CompletionStatus status,
        uint64_t wr_id, uint32_t byte_length = 0,
        uint32_t source_qp = 0);
    void runCompletion();
    void runCompletionErrors();
    void runTimingCompletion();
    void testCheckpointSave();
    void testCheckpointRestore();
    void testCheckpointObservationSave();
    void testCheckpointObservationRestore();
    void testCheckpointCompletionSave();
    void testCheckpointCompletionRestore();
    void setupPairEndpoint(Pvrdma &device, Addr qp_page, Addr cq_page,
                           Addr mr_page,
                           const pvrdma::rocev2::MacAddress &remote_mac,
                           uint32_t send_psn, uint32_t receive_psn);
    void setupPair();
    void setupReliabilityPair();
    void postReliabilityReceive(uint32_t length);
    void postReliabilitySend(uint32_t length);
    void testInboundFrames();
    void runPair();
    void runSemanticPair();
    void runRequestObservationPair();
    void observeDmaRequest(const probing::PacketInfo &packet, bool receiver);
    void runReliabilityPair();
    void runReliabilityRnrPair();
    void runReliabilityTimeoutZeroPair();
    void runReliabilityInvalidPair();
    void runReliabilitySequencePair();
    void runReliabilityUnrelatedPair();
    void runReliabilityCqPair();
    void runReliabilityCqAbortPair();
    void runReliabilityPrecommitAbortPair();
    void runReliabilityCommitPair();
    void runReliabilityCommitBoundaryPair();
    void runTerminalBackpressurePair();
    void testCheckpointTerminalDrainRestore();
    void runSqTerminalBackpressure();
    void testCheckpointSqTerminalDrainRestore();
    void runFaultLink();
    EthPacketPtr faultPacket(uint32_t psn);

  public:
    PARAMS(PvrdmaTester);
    PvrdmaTester(const Params &params);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    void regProbeListeners() override;
    void startup() override;
    void postConsoleInt() override {}
    void clearConsoleInt() override {}
};

} // namespace gem5

#endif // __TEST_OBJECTS_PVRDMA_TESTER_HH__
