// SPDX-License-Identifier: BSD-3-Clause
#ifndef __TEST_OBJECTS_I40E_OBSERVE_HOST_HH__
#define __TEST_OBJECTS_I40E_OBSERVE_HOST_HH__

#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "dev/dma_device.hh"
#include "params/I40eObserveHost.hh"
#include "sim/clocked_object.hh"

namespace gem5
{

// Test initiator only. PCI, RAM, DMA completion and cache ownership remain in
// the production devices, ports and Ruby controllers.
class I40eObserveHost : public ClockedObject
{
    class PioPort : public RequestPort
    {
        I40eObserveHost &host;
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
      public:
        explicit PioPort(I40eObserveHost &owner);
    } pio;
    DmaPort memory;
    const RequestorID pioId;

    enum class Kind { PioRead, PioWrite, MemoryRead, MemoryWrite, Delay, Ready, Done };
    struct Action
    {
        Kind kind;
        Addr address;
        std::vector<uint8_t> bytes;
        std::string label;
        std::function<bool(const std::vector<uint8_t> &)> check;
        Tick delay = 0;
    };
    std::deque<Action> actions;
    PacketPtr pioPacket = nullptr;
    bool pioRetry = false;
    bool memoryActive = false;
    uint64_t actionSequence = 0;
    uint64_t memoryActions = 0;
    uint64_t memoryReadBytes = 0;
    uint64_t memoryWriteBytes = 0;
    unsigned pollAttempts = 0;
    bool ready = false;
    FILE *output = nullptr;
    std::vector<uint8_t> expectedFrame;
    std::vector<uint8_t> adminResult;
    EventFunctionWrapper advanceEvent, memoryDoneEvent, timeoutEvent;

    void buildProgram();
    void buildAdminProgram();
    void advance();
    void complete(const std::vector<uint8_t> &bytes);
    void pioResponse(PacketPtr pkt);
    void retryPio();
    void memoryDone();
    void event(const std::string &name, const std::string &fields="");
    void addPio(bool write, Addr address, uint32_t value,
                const std::string &label,
                std::function<bool(const std::vector<uint8_t> &)> check={});
    void addMemory(bool write, Addr wireAddress, std::vector<uint8_t> bytes,
                   const std::string &label,
                   std::function<bool(const std::vector<uint8_t> &)> check={});
    void addDelay(Tick delay, const std::string &label);

  public:
    PARAMS(I40eObserveHost);
    explicit I40eObserveHost(const I40eObserveHostParams &p);
    ~I40eObserveHost() override;
    void startup() override;
    Port &getPort(const std::string &name, PortID idx=InvalidPortID) override;
};
} // namespace gem5
#endif
