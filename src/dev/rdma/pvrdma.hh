// SPDX-License-Identifier: BSD-3-Clause

#ifndef __DEV_RDMA_PVRDMA_HH__
#define __DEV_RDMA_PVRDMA_HH__

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "base/statistics.hh"
#include "dev/net/etherint.hh"
#include "dev/pci/device.hh"
#include "dev/rdma/pvrdma_abi.hh"
#include "dev/rdma/pvrdma_ring.hh"
#include "dev/rdma/pvrdma_transport.hh"
#include "params/Pvrdma.hh"
#include "sim/byteswap.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace pvrdma
{

inline constexpr uint32_t UnsupportedError = 0xffff;
inline constexpr uint32_t CommandError = 1;
inline constexpr uint32_t InitialInterruptMask = 0xffffffff;
inline constexpr uint32_t GidTableEntries = 8;
inline constexpr uint32_t ObjectTableEntries = 64;
inline constexpr uint32_t PageSize = 4096;
inline constexpr uint32_t PageEntries = PageSize / sizeof(uint64_t);
inline constexpr uint32_t SlotBits = 6;
inline constexpr uint32_t SlotMask = ObjectTableEntries - 1;
inline constexpr uint32_t MaxGeneration =
    std::numeric_limits<uint32_t>::max() >> SlotBits;
inline constexpr uint32_t MrPageSize = PageSize;
inline constexpr uint32_t MrEntriesPerPage = PageEntries;
inline constexpr uint32_t MrSlotBits = SlotBits;
inline constexpr uint32_t MrSlotMask = SlotMask;
inline constexpr uint32_t MaxMrGeneration = MaxGeneration;
inline constexpr uint32_t CqeSize = 64;
inline constexpr uint32_t SqStride = 128;
inline constexpr uint32_t RqStride = 32;
inline constexpr uint32_t SupportedMrAccess = AccessLocalWrite |
    AccessRemoteWrite | AccessRemoteRead | AccessRemoteAtomic;
inline constexpr uint32_t FixedMtu = 1024;
inline constexpr uint32_t MaxMessageSegments =
    std::numeric_limits<uint16_t>::max();
inline constexpr uint32_t MaxMessageSize = MaxMessageSegments * FixedMtu;
inline constexpr std::array<uint32_t, 32> RnrTimerMicros = {
    655360, 10, 20, 30, 40, 60, 80, 120,
    160, 240, 320, 480, 640, 960, 1280, 1920,
    2560, 3840, 5120, 7680, 10240, 15360, 20480, 30720,
    40960, 61440, 81920, 122880, 163840, 245760, 327680, 491520,
};
inline constexpr uint16_t FullMembershipPkey = 0xffff;
inline constexpr uint32_t QpDoorbellOffset = 0x00;
inline constexpr uint32_t CqDoorbellOffset = 0x04;
inline constexpr uint32_t DoorbellHandleMask = 0x00ffffff;
inline constexpr uint32_t SqDoorbellAction = 0x40000000;
inline constexpr uint32_t RqDoorbellAction = 0x80000000;
inline constexpr uint32_t CqArmSolicitedAction = 0x20000000;
inline constexpr uint32_t CqArmAnyAction = 0x40000000;
inline constexpr uint32_t CqPollAction = 0x80000000;

enum class QueueKind : uint8_t
{
    None,
    Sq,
    Rq,
    Cq,
};

enum class DoorbellAction : uint8_t
{
    Invalid,
    Sq,
    Rq,
    CqPoll,
    CqArmSolicited,
    CqArmAny,
};

enum class CqArmMode : uint32_t
{
    Unarmed,
    Solicited,
    Any,
};

enum class CompletionSubmitResult : uint8_t
{
    Queued,
    Busy,
    Rejected,
    Backpressured,
    Published,
};

struct CompletionRecord
{
    uint32_t cqHandle = 0;
    uint32_t qpHandle = 0;
    uint32_t cqGeneration = 0;
    uint32_t qpGeneration = 0;
    uint64_t workRequestId = 0;
    CompletionOpcode opcode = CompletionOpcode::Send;
    CompletionStatus status = CompletionStatus::Success;
    uint32_t byteLength = 0;
    uint32_t sourceQp = 0;
};

inline bool
supportedCompletionStatus(CompletionStatus status)
{
    switch (status) {
      case CompletionStatus::Success:
      case CompletionStatus::LocalLengthError:
      case CompletionStatus::LocalQpOperationError:
      case CompletionStatus::LocalProtectionError:
      case CompletionStatus::LocalAccessError:
      case CompletionStatus::RemoteInvalidRequestError:
      case CompletionStatus::RemoteAccessError:
      case CompletionStatus::RemoteOperationError:
      case CompletionStatus::RetryExceededError:
      case CompletionStatus::RnrRetryExceededError:
      case CompletionStatus::GeneralError:
        return true;
      default:
        return false;
    }
}

inline bool
validCompletionRecord(const CompletionRecord &record)
{
    const bool send = record.opcode == CompletionOpcode::Send;
    return record.cqHandle && record.cqHandle < ObjectTableEntries &&
        record.qpHandle && record.qpHandle < ObjectTableEntries &&
        (send || record.opcode == CompletionOpcode::Receive) &&
        supportedCompletionStatus(record.status) &&
        (send || (record.status != CompletionStatus::RetryExceededError &&
                  record.status != CompletionStatus::RnrRetryExceededError)) &&
        (!send || (!record.byteLength && !record.sourceQp));
}

inline CompletionQueueElement
encodeCompletion(const CompletionRecord &record)
{
    CompletionQueueElement cqe{};
    cqe.workRequestId = htole(record.workRequestId);
    cqe.qp = htole(static_cast<uint64_t>(record.qpHandle));
    cqe.opcode = htole(static_cast<uint32_t>(record.opcode));
    cqe.status = htole(static_cast<uint32_t>(record.status));
    cqe.byteLength = htole(record.byteLength);
    cqe.sourceQp = htole(record.sourceQp);
    return cqe;
}

struct Doorbell
{
    DoorbellAction action = DoorbellAction::Invalid;
    uint32_t handle = 0;
    uint32_t uar = 0;
};

constexpr QueueKind
queueKind(DoorbellAction action)
{
    switch (action) {
      case DoorbellAction::Sq: return QueueKind::Sq;
      case DoorbellAction::Rq: return QueueKind::Rq;
      case DoorbellAction::CqPoll:
      case DoorbellAction::CqArmSolicited:
      case DoorbellAction::CqArmAny:
        return QueueKind::Cq;
      default:
        return QueueKind::None;
    }
}

constexpr bool
decodeDoorbell(uint64_t offset, unsigned size, uint32_t value,
               Doorbell &doorbell)
{
    doorbell = {};
    if (size != sizeof(uint32_t))
        return false;
    const uint32_t page_offset = offset % UarPageSize;
    const uint32_t handle = value & DoorbellHandleMask;
    if (!handle || handle >= ObjectTableEntries)
        return false;

    uint32_t action = 0;
    if (page_offset == QpDoorbellOffset) {
        action = value & (SqDoorbellAction | RqDoorbellAction);
        if (action != SqDoorbellAction && action != RqDoorbellAction)
            return false;
        if (value & ~(DoorbellHandleMask | action))
            return false;
        doorbell.action = action == SqDoorbellAction ?
            DoorbellAction::Sq : DoorbellAction::Rq;
    } else if (page_offset == CqDoorbellOffset) {
        action = value & (CqArmSolicitedAction | CqArmAnyAction |
                          CqPollAction);
        if (action != CqArmSolicitedAction && action != CqArmAnyAction &&
            action != CqPollAction)
            return false;
        if (value & ~(DoorbellHandleMask | action))
            return false;
        doorbell.action = action == CqArmSolicitedAction ?
            DoorbellAction::CqArmSolicited :
            action == CqArmAnyAction ? DoorbellAction::CqArmAny :
                                      DoorbellAction::CqPoll;
    } else {
        return false;
    }
    doorbell.handle = handle;
    doorbell.uar = offset / UarPageSize;
    return true;
}

inline bool
observeProducer(const Ring &observed, uint32_t depth,
                uint32_t &shadow_producer, uint32_t shadow_consumer,
                uint32_t &delta)
{
    const uint32_t producer = letoh(observed.producerTail);
    const uint32_t consumer = letoh(observed.consumerHead);
    const int32_t old = ringForwardDistance(
        shadow_producer, shadow_consumer, depth);
    const int32_t advance = ringForwardDistance(
        producer, shadow_producer, depth);
    if (consumer != shadow_consumer || !ringSnapshotValid(
            producer, consumer, depth) || old < 0 ||
        static_cast<uint32_t>(old) > depth || advance < 0 ||
        static_cast<uint32_t>(advance) > depth - old)
        return false;
    delta = advance;
    shadow_producer = producer;
    return true;
}

inline bool
observeConsumer(const Ring &observed, uint32_t depth,
                uint32_t shadow_producer, uint32_t &shadow_consumer,
                uint32_t &delta)
{
    const uint32_t producer = letoh(observed.producerTail);
    const uint32_t consumer = letoh(observed.consumerHead);
    const int32_t old = ringForwardDistance(
        shadow_producer, shadow_consumer, depth);
    const int32_t advance = ringForwardDistance(
        consumer, shadow_consumer, depth);
    if (producer != shadow_producer || !ringSnapshotValid(
            producer, consumer, depth) || old < 0 || advance < 0 ||
        static_cast<uint32_t>(advance) > static_cast<uint32_t>(old))
        return false;
    delta = advance;
    shadow_consumer = consumer;
    return true;
}

enum class Register
{
    Version,
    DsrLow,
    DsrHigh,
    Control,
    Request,
    Error,
    InterruptCause,
    InterruptMask,
    MacLow,
    MacHigh,
    Invalid,
};

enum class ControlState : uint8_t
{
    Unconfigured,
    Ready,
    Active,
    ReadingDsr,
    WritingCaps,
    ReadingCommand,
    ReadingObjectDirectory,
    ReadingObjectTable,
    WritingResponse,
};

constexpr bool
stable(ControlState state)
{
    return state == ControlState::Unconfigured ||
           state == ControlState::Ready || state == ControlState::Active;
}

constexpr bool
beginDsr(ControlState &state)
{
    if (state != ControlState::Unconfigured)
        return false;
    state = ControlState::ReadingDsr;
    return true;
}

constexpr bool
finishDsrRead(ControlState &state, bool valid)
{
    if (state != ControlState::ReadingDsr)
        return false;
    state = valid ? ControlState::WritingCaps : ControlState::Unconfigured;
    return valid;
}

constexpr bool
finishCapsWrite(ControlState &state)
{
    if (state != ControlState::WritingCaps)
        return false;
    state = ControlState::Ready;
    return true;
}

constexpr bool
applyControl(ControlState &state, DeviceControl control)
{
    if (!stable(state))
        return false;

    switch (control) {
      case DeviceControl::Activate:
        if (state != ControlState::Ready)
            return false;
        state = ControlState::Active;
        return true;
      case DeviceControl::Unquiesce:
        return state == ControlState::Active;
      case DeviceControl::Reset:
        state = ControlState::Unconfigured;
        return true;
    }
    return false;
}

constexpr bool
beginCommand(ControlState &state)
{
    if (state != ControlState::Active)
        return false;
    state = ControlState::ReadingCommand;
    return true;
}

constexpr bool
finishCommandRead(ControlState &state, bool has_response)
{
    if (state != ControlState::ReadingCommand)
        return false;
    state = has_response ? ControlState::WritingResponse :
                           ControlState::Active;
    return true;
}

constexpr bool
beginObjectDirectory(ControlState &state)
{
    if (state != ControlState::ReadingCommand)
        return false;
    state = ControlState::ReadingObjectDirectory;
    return true;
}

constexpr bool
beginObjectTables(ControlState &state)
{
    if (state != ControlState::ReadingObjectDirectory)
        return false;
    state = ControlState::ReadingObjectTable;
    return true;
}

constexpr bool
finishObjectWalk(ControlState &state)
{
    if (state != ControlState::ReadingObjectDirectory &&
        state != ControlState::ReadingObjectTable)
        return false;
    state = ControlState::WritingResponse;
    return true;
}

constexpr bool
finishResponseWrite(ControlState &state, uint32_t &pending)
{
    if (state != ControlState::WritingResponse)
        return false;
    state = ControlState::Active;
    pending |= InterruptCauseResponse;
    return true;
}

constexpr bool
checkpointStable(ControlState state, bool dma_pending,
                 bool observation_queued = false,
                 bool observation_active = false,
                 bool completion_active = false,
                 bool transport_active = false,
                 bool runnable_sq = false,
                 bool active_mr = false)
{
    return stable(state) && !dma_pending && !observation_queued &&
           !observation_active && !completion_active && !transport_active &&
           !runnable_sq && !active_mr;
}

constexpr Register
decodeRegister(uint64_t offset)
{
    switch (offset) {
      case RegVersion: return Register::Version;
      case RegDsrLow: return Register::DsrLow;
      case RegDsrHigh: return Register::DsrHigh;
      case RegControl: return Register::Control;
      case RegRequest: return Register::Request;
      case RegError: return Register::Error;
      case RegInterruptCause: return Register::InterruptCause;
      case RegInterruptMask: return Register::InterruptMask;
      case RegMacLow: return Register::MacLow;
      case RegMacHigh: return Register::MacHigh;
      default: return Register::Invalid;
    }
}

constexpr bool
validRegisterAccess(uint64_t offset, unsigned size)
{
    return size == sizeof(uint32_t) && (offset % sizeof(uint32_t)) == 0 &&
           decodeRegister(offset) != Register::Invalid;
}

constexpr bool
registerReadable(Register reg)
{
    switch (reg) {
      case Register::Version:
      case Register::Error:
      case Register::InterruptCause:
      case Register::InterruptMask:
      case Register::MacLow:
      case Register::MacHigh:
        return true;
      default:
        return false;
    }
}

constexpr bool
registerWritable(Register reg)
{
    switch (reg) {
      case Register::DsrLow:
      case Register::DsrHigh:
      case Register::Control:
      case Register::Request:
      case Register::InterruptMask:
      case Register::MacLow:
      case Register::MacHigh:
        return true;
      default:
        return false;
    }
}

constexpr uint32_t
unmaskedInterrupts(uint32_t pending, uint32_t mask)
{
    return pending & ~mask;
}

constexpr bool
interruptPending(uint32_t pending, uint32_t mask)
{
    return unmaskedInterrupts(pending, mask) != 0;
}

inline uint32_t
acknowledgeInterrupts(uint32_t &pending)
{
    const uint32_t causes = pending;
    pending = 0;
    return causes;
}

struct RegisterState
{
    uint64_t dsrAddress = 0;
    uint32_t control = 0;
    uint32_t request = 0;
    uint32_t error = UnsupportedError;
    uint32_t pendingCauses = 0;
    uint32_t interruptMask = InitialInterruptMask;
    uint32_t macLow = 0;
    uint32_t macHigh = 0;
    bool dsrLowPending = false;

    RegisterState() = default;
    RegisterState(uint32_t low, uint32_t high) : macLow(low), macHigh(high) {}

    void
    reset()
    {
        dsrAddress = 0;
        control = 0;
        request = 0;
        error = UnsupportedError;
        pendingCauses = 0;
        interruptMask = InitialInterruptMask;
        dsrLowPending = false;
    }

    void
    writeDsrLow(uint32_t value)
    {
        dsrAddress = value;
        dsrLowPending = true;
    }

    bool
    writeDsrHigh(uint32_t value)
    {
        if (!dsrLowPending)
            return false;
        dsrAddress |= static_cast<uint64_t>(value) << 32;
        dsrLowPending = false;
        return true;
    }

    void
    writeMacHigh(uint32_t value)
    {
        macHigh = value & 0xffff;
    }

    uint32_t
    acknowledgeInterrupts()
    {
        return pvrdma::acknowledgeInterrupts(pendingCauses);
    }
};

using GidTable = std::array<Gid, GidTableEntries>;
using GidValidTable = std::array<uint8_t, GidTableEntries>;

struct UarRange
{
    uint64_t start = 0;
    uint64_t size = 0;
};

struct ObjectTables
{
    std::array<uint32_t, ObjectTableEntries> contextUar{};
    std::array<uint32_t, ObjectTableEntries> contextPdChildren{};
    std::array<uint32_t, ObjectTableEntries> contextCqChildren{};
    std::array<uint32_t, ObjectTableEntries> pdAllocated{};
    std::array<uint32_t, ObjectTableEntries> pdParent{};
    std::array<uint32_t, ObjectTableEntries> pdChildren{};

    void
    reset()
    {
        contextUar = {};
        contextPdChildren = {};
        contextCqChildren = {};
        pdAllocated = {};
        pdParent = {};
        pdChildren = {};
    }
};

struct MemoryRegion
{
    bool valid = false;
    uint32_t generation = 0;
    uint32_t mrHandle = 0;
    uint32_t lkey = 0;
    uint32_t rkey = 0;
    uint32_t pdHandle = 0;
    uint32_t accessFlags = 0;
    uint32_t activeReferences = 0;
    uint64_t start = 0;
    uint64_t length = 0;
    uint64_t end = 0;
    std::vector<uint64_t> pages;
};

struct PageDirectoryBuild
{
    uint32_t numChunks = 0;
    uint64_t pageDirectoryDma = 0;
    std::vector<uint64_t> pages;

    void reset()
    {
        numChunks = 0;
        pageDirectoryDma = 0;
        pages.clear();
    }
};

struct MemoryRegionBuild : PageDirectoryBuild
{
    uint32_t slot = 0;
    uint32_t generation = 0;
    uint32_t mrHandle = 0;
    uint32_t pdHandle = 0;
    uint32_t accessFlags = 0;
    uint64_t start = 0;
    uint64_t length = 0;
    uint64_t end = 0;
};

struct DmaChunk
{
    uint64_t address = 0;
    uint32_t length = 0;

    bool
    operator==(const DmaChunk &other) const
    {
        return address == other.address && length == other.length;
    }
};

struct MemoryRegionLease
{
    uint32_t slot = 0;
    uint32_t generation = 0;
    std::vector<DmaChunk> chunks;
};

struct PreparedWqe
{
    QueueKind kind = QueueKind::None;
    uint64_t workRequestId = 0;
    uint64_t address = 0;
    uint32_t length = 0;
    uint32_t lkey = 0;
    bool signaled = false;
};

enum class MrKeyType
{
    Local,
    Remote,
};

struct MemoryRegionTable
{
    std::array<MemoryRegion, ObjectTableEntries> entries{};

    void reset() { entries = {}; }

    bool
    allocate(MemoryRegionBuild &build) const
    {
        for (uint32_t slot = 1; slot < ObjectTableEntries; ++slot) {
            const auto &entry = entries[slot];
            if (!entry.valid && entry.generation <= MaxMrGeneration) {
                build.slot = slot;
                build.generation = entry.generation;
                build.mrHandle = (build.generation << MrSlotBits) | slot;
                return true;
            }
        }
        return false;
    }

    bool
    commit(MemoryRegionBuild &&build, ObjectTables &objects)
    {
        if (!build.slot || build.slot >= ObjectTableEntries ||
            !build.pdHandle || build.pdHandle >= ObjectTableEntries ||
            build.pages.size() != build.numChunks ||
            !objects.pdAllocated[build.pdHandle])
            return false;
        auto &entry = entries[build.slot];
        if (entry.valid || entry.generation != build.generation ||
            build.mrHandle != ((build.generation << MrSlotBits) | build.slot))
            return false;
        entry.valid = true;
        entry.generation = build.generation;
        entry.mrHandle = build.mrHandle;
        entry.lkey = build.mrHandle;
        entry.rkey = build.mrHandle;
        entry.pdHandle = build.pdHandle;
        entry.accessFlags = build.accessFlags;
        entry.start = build.start;
        entry.length = build.length;
        entry.end = build.end;
        entry.pages = std::move(build.pages);
        ++objects.pdChildren[entry.pdHandle];
        return true;
    }

    bool
    destroy(uint32_t handle, ObjectTables &objects)
    {
        const uint32_t slot = handle & MrSlotMask;
        if (!slot || slot >= ObjectTableEntries)
            return false;
        auto &entry = entries[slot];
        if (!entry.valid || entry.mrHandle != handle ||
            entry.activeReferences || !objects.pdChildren[entry.pdHandle])
            return false;

        const uint32_t generation = entry.generation;
        --objects.pdChildren[entry.pdHandle];
        entry = {};
        entry.generation = generation + 1;
        return true;
    }
};

struct CompletionQueue
{
    bool valid = false;
    uint32_t generation = 0;
    uint32_t cqHandle = 0;
    uint32_t contextHandle = 0;
    uint32_t uar = 0;
    uint32_t cqe = 0;
    uint32_t qpReferences = 0;
    uint32_t armFlags = 0;
    uint32_t producerTail = 0;
    uint32_t consumerHead = 0;
    std::vector<uint64_t> pages;
};

struct CompletionQueueBuild : PageDirectoryBuild
{
    uint32_t slot = 0;
    uint32_t generation = 0;
    uint32_t contextHandle = 0;
    uint32_t cqe = 0;
};

struct CompletionQueueTable
{
    std::array<CompletionQueue, ObjectTableEntries> entries{};

    void reset() { entries = {}; }

    bool allocate(CompletionQueueBuild &build) const
    {
        for (uint32_t slot = 1; slot < ObjectTableEntries; ++slot) {
            if (!entries[slot].valid &&
                entries[slot].generation <= MaxGeneration) {
                build.slot = slot;
                build.generation = entries[slot].generation;
                return true;
            }
        }
        return false;
    }

    bool commit(CompletionQueueBuild &&build, ObjectTables &objects)
    {
        if (!build.slot || build.slot >= ObjectTableEntries ||
            !build.contextHandle ||
            build.contextHandle >= ObjectTableEntries ||
            !objects.contextUar[build.contextHandle] || !build.cqe ||
            (build.cqe & (build.cqe - 1)) || build.cqe > 1024 ||
            build.numChunks != 1 +
                (uint64_t{build.cqe} * CqeSize + PageSize - 1) / PageSize ||
            build.pages.size() != build.numChunks ||
            std::any_of(build.pages.begin(), build.pages.end(),
                [](uint64_t page) {
                    return !page || (page % PageSize) != 0;
                }))
            return false;
        auto &entry = entries[build.slot];
        if (entry.valid || entry.generation != build.generation)
            return false;
        entry.valid = true;
        entry.generation = build.generation;
        entry.cqHandle = build.slot;
        entry.contextHandle = build.contextHandle;
        entry.uar = objects.contextUar[build.contextHandle];
        entry.cqe = build.cqe;
        entry.pages = std::move(build.pages);
        ++objects.contextCqChildren[entry.contextHandle];
        return true;
    }

    bool destroy(uint32_t handle, ObjectTables &objects)
    {
        if (!handle || handle >= ObjectTableEntries)
            return false;
        auto &entry = entries[handle];
        if (!entry.valid || entry.cqHandle != handle ||
            entry.qpReferences ||
            !objects.contextCqChildren[entry.contextHandle])
            return false;
        --objects.contextCqChildren[entry.contextHandle];
        const uint32_t generation = entry.generation;
        entry = {};
        entry.generation = generation + 1;
        return true;
    }
};

struct FinalReplay
{
    bool valid = false;
    uint32_t qpGeneration = 0;
    transport::MacAddress localMac{};
    transport::MacAddress remoteMac{};
    uint32_t localQpn = 0;
    uint32_t remoteQpn = 0;
    uint32_t finalPsn = 0;
    uint64_t messageId = 0;
    uint32_t totalLength = 0;
    uint16_t segmentIndex = 0;
    uint16_t segmentCount = 0;
};

struct QueuePair
{
    bool valid = false;
    uint32_t generation = 0;
    uint32_t qpHandle = 0;
    uint32_t qpn = 0;
    uint32_t pdHandle = 0;
    uint32_t sendCqHandle = 0;
    uint32_t recvCqHandle = 0;
    uint32_t contextHandle = 0;
    uint32_t uar = 0;
    uint32_t sendChunks = 0;
    uint32_t recvChunks = 0;
    uint32_t totalChunks = 0;
    bool signalAllSendWr = false;
    QpState state = QpState::Reset;
    QpCap capabilities{};
    QpAttr attributes{};
    uint32_t sqProducerTail = 0;
    uint32_t sqConsumerHead = 0;
    uint32_t rqProducerTail = 0;
    uint32_t rqConsumerHead = 0;
    FinalReplay finalReplay{};
    std::vector<uint64_t> pages;
};

struct QueuePairBuild : PageDirectoryBuild
{
    uint32_t slot = 0;
    uint32_t generation = 0;
    uint32_t qpn = 0;
    uint32_t pdHandle = 0;
    uint32_t sendCqHandle = 0;
    uint32_t recvCqHandle = 0;
    uint32_t contextHandle = 0;
    uint32_t sendChunks = 0;
    uint32_t recvChunks = 0;
    bool signalAllSendWr = false;
    QpCap capabilities{};
};

struct QueuePairTable
{
    std::array<QueuePair, ObjectTableEntries> entries{};

    void reset() { entries = {}; }

    bool allocate(QueuePairBuild &build) const
    {
        for (uint32_t slot = 1; slot < ObjectTableEntries; ++slot) {
            const auto &entry = entries[slot];
            if (!entry.valid && entry.generation <= MaxGeneration) {
                build.slot = slot;
                build.generation = entry.generation;
                build.qpn = (build.generation << SlotBits) | slot;
                return true;
            }
        }
        return false;
    }

    bool commit(QueuePairBuild &&build, ObjectTables &objects,
                CompletionQueueTable &cqs)
    {
        if (!build.slot || build.slot >= ObjectTableEntries ||
            !build.pdHandle || build.pdHandle >= ObjectTableEntries ||
            !objects.pdAllocated[build.pdHandle] ||
            !build.contextHandle ||
            objects.pdParent[build.pdHandle] != build.contextHandle ||
            !objects.contextUar[build.contextHandle] ||
            !build.sendCqHandle ||
            build.sendCqHandle >= ObjectTableEntries ||
            !build.recvCqHandle ||
            build.recvCqHandle >= ObjectTableEntries ||
            !cqs.entries[build.sendCqHandle].valid ||
            !cqs.entries[build.recvCqHandle].valid ||
            cqs.entries[build.sendCqHandle].contextHandle !=
                build.contextHandle ||
            cqs.entries[build.recvCqHandle].contextHandle !=
                build.contextHandle ||
            !build.capabilities.maxSendWr ||
            (build.capabilities.maxSendWr &
             (build.capabilities.maxSendWr - 1)) ||
            build.capabilities.maxSendWr > 256 ||
            !build.capabilities.maxRecvWr ||
            (build.capabilities.maxRecvWr &
             (build.capabilities.maxRecvWr - 1)) ||
            build.capabilities.maxRecvWr > 256 ||
            build.capabilities.maxSendSge != 1 ||
            build.capabilities.maxRecvSge != 1 ||
            build.capabilities.maxInlineData ||
            build.capabilities.reserved ||
            build.sendChunks !=
                (uint64_t{build.capabilities.maxSendWr} * SqStride +
                 PageSize - 1) / PageSize ||
            build.recvChunks !=
                (uint64_t{build.capabilities.maxRecvWr} * RqStride +
                 PageSize - 1) / PageSize ||
            build.numChunks != 1 + build.sendChunks + build.recvChunks ||
            build.pages.size() != build.numChunks ||
            std::any_of(build.pages.begin(), build.pages.end(),
                [](uint64_t page) {
                    return !page || (page % PageSize) != 0;
                }))
            return false;
        auto &entry = entries[build.slot];
        if (entry.valid || entry.generation != build.generation ||
            build.qpn != ((build.generation << SlotBits) | build.slot))
            return false;
        entry.valid = true;
        entry.generation = build.generation;
        entry.qpHandle = build.slot;
        entry.qpn = build.qpn;
        entry.pdHandle = build.pdHandle;
        entry.sendCqHandle = build.sendCqHandle;
        entry.recvCqHandle = build.recvCqHandle;
        entry.contextHandle = build.contextHandle;
        entry.uar = objects.contextUar[build.contextHandle];
        entry.sendChunks = build.sendChunks;
        entry.recvChunks = build.recvChunks;
        entry.totalChunks = build.numChunks;
        entry.signalAllSendWr = build.signalAllSendWr;
        entry.capabilities = build.capabilities;
        entry.attributes.qpState = QpState::Reset;
        entry.attributes.currentQpState = QpState::Reset;
        entry.attributes.capabilities = build.capabilities;
        entry.pages = std::move(build.pages);
        ++objects.pdChildren[entry.pdHandle];
        ++cqs.entries[entry.sendCqHandle].qpReferences;
        ++cqs.entries[entry.recvCqHandle].qpReferences;
        return true;
    }

    bool destroy(uint32_t handle, ObjectTables &objects,
                 CompletionQueueTable &cqs)
    {
        if (!handle || handle >= ObjectTableEntries)
            return false;
        auto &entry = entries[handle];
        const uint32_t send_refs = entry.sendCqHandle == entry.recvCqHandle ?
            2 : 1;
        if (!entry.valid || entry.qpHandle != handle ||
            !entry.pdHandle || entry.pdHandle >= ObjectTableEntries ||
            !entry.sendCqHandle ||
            entry.sendCqHandle >= ObjectTableEntries ||
            !entry.recvCqHandle ||
            entry.recvCqHandle >= ObjectTableEntries ||
            !cqs.entries[entry.sendCqHandle].valid ||
            !cqs.entries[entry.recvCqHandle].valid ||
            !objects.pdChildren[entry.pdHandle] ||
            cqs.entries[entry.sendCqHandle].qpReferences < send_refs ||
            cqs.entries[entry.recvCqHandle].qpReferences < 1)
            return false;
        --objects.pdChildren[entry.pdHandle];
        --cqs.entries[entry.sendCqHandle].qpReferences;
        --cqs.entries[entry.recvCqHandle].qpReferences;
        const uint32_t generation = entry.generation;
        entry = {};
        entry.generation = generation + 1;
        return true;
    }
};

inline QueuePair *
findQueuePair(QueuePairTable &qps, uint32_t qpn)
{
    const uint32_t slot = qpn & SlotMask;
    if (!slot || slot >= ObjectTableEntries)
        return nullptr;
    auto &qp = qps.entries[slot];
    return qp.valid && qp.qpn == qpn &&
        qp.generation == (qpn >> SlotBits) ? &qp : nullptr;
}

inline const QueuePair *
findQueuePair(const QueuePairTable &qps, uint32_t qpn)
{
    const uint32_t slot = qpn & SlotMask;
    if (!slot || slot >= ObjectTableEntries)
        return nullptr;
    const auto &qp = qps.entries[slot];
    return qp.valid && qp.qpn == qpn &&
        qp.generation == (qpn >> SlotBits) ? &qp : nullptr;
}

constexpr bool
validPsn(uint32_t psn)
{
    return !(psn & ~transport::PsnMask);
}

constexpr uint32_t
advancePsn(uint32_t psn)
{
    return (psn + 1) & transport::PsnMask;
}

constexpr uint32_t
psnDistance(uint32_t from, uint32_t to)
{
    return (to - from) & transport::PsnMask;
}

constexpr uint64_t
saturatingMultiply(uint64_t value, uint64_t scale, uint64_t maximum)
{
    return scale && value > maximum / scale ? maximum : value * scale;
}

constexpr uint64_t
saturatingAdd(uint64_t value, uint64_t addend, uint64_t maximum)
{
    return addend > maximum - value ? maximum : value + addend;
}

constexpr uint64_t
ackTimeoutNanoseconds(uint8_t timeout)
{
    return timeout ? uint64_t{4096} << timeout : 0;
}

constexpr bool
useRetry(uint8_t &remaining)
{
    if (remaining == 7)
        return true;
    if (!remaining)
        return false;
    --remaining;
    return true;
}

inline bool
segmentGeometry(uint32_t length, uint16_t &count)
{
    const uint64_t segments = length ?
        (uint64_t{length} + FixedMtu - 1) / FixedMtu : 1;
    if (segments > MaxMessageSegments)
        return false;
    count = segments;
    return true;
}

inline bool
canonicalData(const transport::Frame &frame)
{
    uint16_t count = 0;
    if (!segmentGeometry(frame.totalLength, count) ||
        frame.segmentCount != count || frame.segmentIndex >= count)
        return false;
    const uint32_t offset = uint32_t{frame.segmentIndex} * FixedMtu;
    const uint32_t length = frame.totalLength == 0 ? 0 :
        std::min(FixedMtu, frame.totalLength - offset);
    return frame.payloadOffset == offset && frame.payload.size == length &&
        frame.flags == ((frame.segmentIndex == 0 ? transport::First : 0) |
            (frame.segmentIndex + 1 == count ? transport::Last : 0));
}

inline bool
leaseRange(const MemoryRegionLease &lease, size_t offset, size_t length,
           size_t &chunk_index, size_t &chunk_offset)
{
    size_t cursor = 0;
    for (chunk_index = 0; chunk_index < lease.chunks.size(); ++chunk_index) {
        const size_t next = cursor + lease.chunks[chunk_index].length;
        if (offset < next || (offset == next && !length)) {
            chunk_offset = offset - cursor;
            size_t available = lease.chunks[chunk_index].length -
                chunk_offset;
            for (size_t i = chunk_index + 1; available < length &&
                 i < lease.chunks.size(); ++i)
                available += lease.chunks[i].length;
            return available >= length;
        }
        cursor = next;
    }
    if (!length && offset == cursor) {
        chunk_index = lease.chunks.size();
        chunk_offset = 0;
        return true;
    }
    return false;
}

inline bool
queueConsumerAddress(const QueuePair &qp, QueueKind kind, uint64_t &address)
{
    if (!qp.valid || qp.pages.empty() ||
        (kind != QueueKind::Sq && kind != QueueKind::Rq))
        return false;
    address = qp.pages[0] +
        (kind == QueueKind::Sq ? offsetof(RingState, tx) :
                                 offsetof(RingState, rx)) +
        offsetof(Ring, consumerHead);
    return true;
}

inline bool
wqeAddress(const QueuePair &qp, QueueKind kind, uint32_t consumer,
           uint64_t &address)
{
    if (!qp.valid || !qp.qpHandle || qp.qpHandle >= ObjectTableEntries ||
        qp.generation > MaxGeneration ||
        qp.qpn != ((qp.generation << SlotBits) | qp.qpHandle) ||
        (kind != QueueKind::Sq && kind != QueueKind::Rq))
        return false;

    const auto valid_depth = [](uint32_t depth) {
        return depth && !(depth & (depth - 1)) && depth <= 256;
    };
    if (!valid_depth(qp.capabilities.maxSendWr) ||
        !valid_depth(qp.capabilities.maxRecvWr) ||
        qp.capabilities.maxSendSge != 1 ||
        qp.capabilities.maxRecvSge != 1 ||
        qp.capabilities.maxInlineData || qp.capabilities.reserved ||
        qp.sendChunks !=
            (static_cast<uint64_t>(qp.capabilities.maxSendWr) * SqStride +
             PageSize - 1) / PageSize ||
        qp.recvChunks !=
            (static_cast<uint64_t>(qp.capabilities.maxRecvWr) * RqStride +
             PageSize - 1) / PageSize ||
        PageSize % SqStride || PageSize % RqStride ||
        qp.totalChunks != 1 + qp.sendChunks + qp.recvChunks ||
        qp.pages.size() != qp.totalChunks ||
        std::any_of(qp.pages.begin(), qp.pages.end(), [](uint64_t page) {
            return !page || page % PageSize;
        }))
        return false;

    const uint32_t depth = kind == QueueKind::Sq ?
        qp.capabilities.maxSendWr : qp.capabilities.maxRecvWr;
    const uint32_t stride = kind == QueueKind::Sq ? SqStride : RqStride;
    const uint32_t chunks = kind == QueueKind::Sq ?
        qp.sendChunks : qp.recvChunks;
    const uint64_t offset =
        static_cast<uint64_t>(consumer & (depth - 1)) * stride;
    const uint32_t page = offset / PageSize;
    const uint32_t page_offset = offset % PageSize;
    const uint32_t first = 1 +
        (kind == QueueKind::Rq ? qp.sendChunks : 0);
    if (page >= chunks || first + page >= qp.pages.size() ||
        page_offset + stride > PageSize)
        return false;
    address = qp.pages[first + page] + page_offset;
    return true;
}

inline bool
decodeSqWqe(const QueuePair &qp,
            const std::array<uint8_t, SqStride> &slot,
            PreparedWqe &prepared)
{
    if (!qp.valid)
        return false;
    SendWqeHeader header;
    Sge sge;
    std::memcpy(&header, slot.data(), sizeof(header));
    std::memcpy(&sge, slot.data() + sizeof(header), sizeof(sge));
    const uint32_t flags = letoh(header.sendFlags);
    if (letoh(header.numSge) != 1 ||
        letoh(header.opcode) !=
            static_cast<uint32_t>(WorkRequestOpcode::Send) ||
        (flags != 0 && flags != SendSignaled) ||
        letoh(header.beImmediateData) || letoh(header.reserved) ||
        std::any_of(std::begin(header.operationData),
                    std::end(header.operationData),
                    [](uint8_t byte) { return byte != 0; }) ||
        letoh(sge.length) > MaxMessageSize)
        return false;

    prepared = {QueueKind::Sq, letoh(header.workRequestId),
                letoh(sge.address), letoh(sge.length), letoh(sge.lkey),
                qp.signalAllSendWr || (flags & SendSignaled)};
    return true;
}

inline bool
decodeRqWqe(const QueuePair &qp,
            const std::array<uint8_t, RqStride> &slot,
            PreparedWqe &prepared)
{
    if (!qp.valid)
        return false;
    ReceiveWqeHeader header;
    Sge sge;
    std::memcpy(&header, slot.data(), sizeof(header));
    std::memcpy(&sge, slot.data() + sizeof(header), sizeof(sge));
    if (letoh(header.numSge) != 1 || letoh(sge.length) > MaxMessageSize)
        return false;

    prepared = {QueueKind::Rq, letoh(header.workRequestId),
                letoh(sge.address), letoh(sge.length), letoh(sge.lkey),
                false};
    return true;
}

inline bool
validDoorbell(const Doorbell &doorbell, ControlState control_state,
              const CompletionQueueTable &cqs, const QueuePairTable &qps)
{
    if (control_state == ControlState::Unconfigured ||
        control_state == ControlState::Ready || !doorbell.handle ||
        doorbell.handle >= ObjectTableEntries)
        return false;
    const auto kind = queueKind(doorbell.action);
    if (kind == QueueKind::None)
        return false;
    if (kind == QueueKind::Cq) {
        const auto &cq = cqs.entries[doorbell.handle];
        return cq.valid && cq.cqHandle == doorbell.handle &&
            cq.uar == doorbell.uar;
    }
    const auto &qp = qps.entries[doorbell.handle];
    return qp.valid && qp.qpHandle == doorbell.handle &&
        qp.uar == doorbell.uar &&
        (kind == QueueKind::Sq ? qp.state == QpState::ReadyToSend :
            (qp.state == QpState::Init ||
             qp.state == QpState::ReadyToReceive ||
             qp.state == QpState::ReadyToSend));
}

inline bool
cqPollNeedsObservation(const CompletionQueue &cq)
{
    return cq.producerTail != cq.consumerHead;
}

inline bool
queueCommandTargetBusy(const CommandRequest &request, uint64_t qp_busy,
                       uint64_t cq_busy)
{
    const uint32_t command = letoh(request.header.command);
    uint32_t handle = 0;
    if (command == static_cast<uint32_t>(Command::DestroyQp)) {
        handle = letoh(request.destroyQp.qpHandle);
        return handle < ObjectTableEntries &&
            (qp_busy & (uint64_t{1} << handle));
    }
    if (command == static_cast<uint32_t>(Command::DestroyCq)) {
        handle = letoh(request.destroyCq.cqHandle);
        return handle < ObjectTableEntries &&
            (cq_busy & (uint64_t{1} << handle));
    }
    if (command == static_cast<uint32_t>(Command::ModifyQp) &&
        letoh(static_cast<uint32_t>(request.modifyQp.attributes.qpState)) ==
            static_cast<uint32_t>(QpState::Reset)) {
        handle = letoh(request.modifyQp.qpHandle);
        return handle < ObjectTableEntries &&
            (qp_busy & (uint64_t{1} << handle));
    }
    return false;
}

enum class PendingCreateKind : uint8_t
{
    None,
    Mr,
    Cq,
    Qp,
};

struct CommandResult
{
    bool hasResponse;
    uint32_t error;
};

struct OperationErrorState
{
    uint64_t generation = 0;
    uint64_t operationGeneration = 0;

    void
    set(uint32_t &error, uint32_t value)
    {
        error = value;
        ++generation;
    }

    void
    begin(uint32_t &error)
    {
        set(error, 0);
        operationGeneration = generation;
    }

    void
    complete(uint32_t &error, uint32_t value) const
    {
        if (generation == operationGeneration)
            error = value;
    }

    void
    reset()
    {
        generation = operationGeneration = 0;
    }
};

namespace detail
{

inline uint64_t
macGuid(uint32_t mac_low, uint32_t mac_high)
{
    const std::array<uint8_t, 8> bytes = {
        static_cast<uint8_t>(static_cast<uint8_t>(mac_low) ^ 0x02),
        static_cast<uint8_t>(mac_low >> 8),
        static_cast<uint8_t>(mac_low >> 16),
        0xff,
        0xfe,
        static_cast<uint8_t>(mac_low >> 24),
        static_cast<uint8_t>(mac_high),
        static_cast<uint8_t>(mac_high >> 8),
    };
    uint64_t guid = 0;
    for (const auto byte : bytes)
        guid = (guid << 8) | byte;
    return guid;
}

inline bool
alignedPage(uint64_t address)
{
    return address && (address % UarPageSize) == 0;
}

inline bool
mrPageCount(uint64_t start, uint64_t length, uint32_t &pages)
{
    if (!length || start > std::numeric_limits<uint64_t>::max() - length)
        return false;
    const uint64_t bytes = (start & (MrPageSize - 1)) + length;
    const uint64_t count = bytes / MrPageSize + (bytes % MrPageSize != 0);
    if (!count || count > PageDirectoryMaxPages)
        return false;
    pages = count;
    return true;
}

inline bool
validMrAccess(uint32_t access)
{
    return !(access & ~SupportedMrAccess) &&
        (!((access & (AccessRemoteWrite | AccessRemoteAtomic)) &&
           !(access & AccessLocalWrite)));
}

inline bool
powerOfTwo(uint32_t value)
{
    return value && !(value & (value - 1));
}

inline uint32_t
chunksFor(uint32_t entries, uint32_t stride)
{
    const uint64_t bytes = static_cast<uint64_t>(entries) * stride;
    return (bytes + PageSize - 1) / PageSize;
}

inline bool
validSupportedState(QpState state)
{
    return state == QpState::Reset || state == QpState::Init ||
        state == QpState::ReadyToReceive ||
        state == QpState::ReadyToSend || state == QpState::Error;
}

inline bool
validAddressVector(const AddressHandleAttr &attr)
{
    return attr.globalRoute.sourceGidIndex < GidTableEntries &&
        attr.portNumber == 1;
}

inline bool
zeroAddressVector(const AddressHandleAttr &attr)
{
    return std::all_of(std::begin(attr.globalRoute.destinationGid.raw),
                       std::end(attr.globalRoute.destinationGid.raw),
                       [](uint8_t byte) { return byte == 0; }) &&
        !attr.globalRoute.flowLabel && !attr.globalRoute.sourceGidIndex &&
        !attr.globalRoute.hopLimit && !attr.globalRoute.trafficClass &&
        !attr.globalRoute.reserved && !attr.destinationLid && !attr.vlanId &&
        !attr.serviceLevel && !attr.sourcePathBits && !attr.staticRate &&
        !attr.flags && !attr.portNumber &&
        std::all_of(std::begin(attr.destinationMac),
                    std::end(attr.destinationMac),
                    [](uint8_t byte) { return byte == 0; }) &&
        !attr.reserved;
}

inline bool
validStoredQpAttributes(const QueuePair &qp)
{
    const auto &attr = qp.attributes;
    if (attr.qpState != qp.state || attr.currentQpState != qp.state ||
        attr.pathMigrationState != MigrationState::Migrated || attr.qkey ||
        attr.alternatePkeyIndex || attr.enableSqdAsyncNotify ||
        attr.sendQueueDraining || attr.alternatePortNumber ||
        attr.alternateTimeout ||
        std::any_of(std::begin(attr.reserved), std::end(attr.reserved),
                    [](uint8_t byte) { return byte != 0; }) ||
        attr.capabilities.maxSendWr != qp.capabilities.maxSendWr ||
        attr.capabilities.maxRecvWr != qp.capabilities.maxRecvWr ||
        attr.capabilities.maxSendSge != qp.capabilities.maxSendSge ||
        attr.capabilities.maxRecvSge != qp.capabilities.maxRecvSge ||
        attr.capabilities.maxInlineData != qp.capabilities.maxInlineData ||
        attr.capabilities.reserved ||
        attr.addressHandle.globalRoute.reserved ||
        attr.addressHandle.reserved || !zeroAddressVector(
            attr.alternateAddressHandle))
        return false;

    if (qp.state == QpState::Reset)
        return attr.pathMtu == static_cast<Mtu>(0) &&
            !attr.receivePsn && !attr.sendPsn &&
            !attr.destinationQpNumber && !attr.qpAccessFlags &&
            !attr.pkeyIndex && !attr.maxReadAtomic &&
            !attr.maxDestinationReadAtomic && !attr.minRnrTimer &&
            !attr.portNumber && !attr.timeout && !attr.retryCount &&
            !attr.rnrRetry && zeroAddressVector(attr.addressHandle);

    if (!validMrAccess(attr.qpAccessFlags) || attr.pkeyIndex ||
        attr.portNumber != 1)
        return false;
    if (qp.state == QpState::Init)
        return attr.pathMtu == static_cast<Mtu>(0) &&
            !attr.receivePsn && !attr.sendPsn &&
            !attr.destinationQpNumber && !attr.maxReadAtomic &&
            !attr.maxDestinationReadAtomic && !attr.minRnrTimer &&
            !attr.timeout && !attr.retryCount && !attr.rnrRetry &&
            zeroAddressVector(attr.addressHandle);

    if (attr.pathMtu != Mtu::Mtu1024 || !attr.destinationQpNumber ||
        !validPsn(attr.receivePsn) ||
        attr.maxDestinationReadAtomic > 1 || attr.minRnrTimer > 31 ||
        !validAddressVector(attr.addressHandle))
        return false;
    if (qp.state == QpState::ReadyToReceive)
        return !attr.sendPsn && !attr.maxReadAtomic && !attr.timeout &&
            !attr.retryCount && !attr.rnrRetry;

    return (qp.state == QpState::ReadyToSend ||
            qp.state == QpState::Error) && validPsn(attr.sendPsn) &&
        attr.maxReadAtomic <= 1 && attr.timeout <= 31 &&
        attr.retryCount <= 7 && attr.rnrRetry <= 7;
}

inline bool
validFinalReplay(const QueuePair &qp)
{
    const auto &replay = qp.finalReplay;
    const auto zero_mac = [](const auto &mac) {
        return std::all_of(mac.begin(), mac.end(),
                           [](uint8_t byte) { return byte == 0; });
    };
    if (!replay.valid)
        return replay.qpGeneration == 0 && replay.localQpn == 0 &&
            replay.remoteQpn == 0 && replay.finalPsn == 0 &&
            replay.messageId == 0 && replay.totalLength == 0 &&
            replay.segmentIndex == 0 && replay.segmentCount == 0 &&
            zero_mac(replay.localMac) && zero_mac(replay.remoteMac);
    uint16_t count = 0;
    return replay.qpGeneration == qp.generation &&
        replay.localQpn == qp.qpn &&
        replay.remoteQpn == qp.attributes.destinationQpNumber &&
        std::equal(replay.remoteMac.begin(), replay.remoteMac.end(),
                   qp.attributes.addressHandle.destinationMac) &&
        validPsn(replay.finalPsn) && replay.messageId &&
        segmentGeometry(replay.totalLength, count) &&
        replay.segmentCount == count &&
        replay.segmentIndex + 1 == replay.segmentCount &&
        advancePsn(replay.finalPsn) == qp.attributes.receivePsn;
}

inline bool
prepareCreateMr(const CommandRequest &request, const ObjectTables &objects,
                const MemoryRegionTable &mrs, MemoryRegionBuild &build)
{
    const auto &cmd = request.createMr;
    const uint32_t pd = letoh(cmd.pdHandle);
    const uint32_t access = letoh(cmd.accessFlags);
    const uint32_t flags = letoh(cmd.flags);
    const uint32_t chunks = letoh(cmd.numChunks);
    const uint64_t start = letoh(cmd.start);
    const uint64_t length = letoh(cmd.length);
    const uint64_t directory = letoh(cmd.pageDirectoryDma);
    uint32_t expected = 0;

    if (letoh(request.header.reserved) || flags || !validMrAccess(access) ||
        !pd || pd >= ObjectTableEntries || !objects.pdAllocated[pd] ||
        !alignedPage(directory) || !mrPageCount(start, length, expected) ||
        chunks != expected || !mrs.allocate(build))
        return false;

    build.pdHandle = pd;
    build.accessFlags = access;
    build.numChunks = chunks;
    build.start = start;
    build.length = length;
    build.end = start + length;
    build.pageDirectoryDma = directory;
    build.pages.clear();
    build.pages.reserve(chunks);
    return true;
}

inline bool
validMemoryRegions(const MemoryRegionTable &mrs,
                   const ObjectTables &objects)
{
    if (mrs.entries[0].valid || mrs.entries[0].generation)
        return false;

    std::array<uint32_t, ObjectTableEntries> children{};
    for (uint32_t slot = 1; slot < ObjectTableEntries; ++slot) {
        const auto &mr = mrs.entries[slot];
        if (!mr.valid) {
            if (mr.mrHandle || mr.lkey || mr.rkey || mr.pdHandle ||
                mr.accessFlags || mr.activeReferences || mr.start ||
                mr.length || mr.end || !mr.pages.empty())
                return false;
            continue;
        }

        uint32_t pages = 0;
        if (mr.generation > MaxMrGeneration ||
            mr.mrHandle != ((mr.generation << MrSlotBits) | slot) ||
            mr.lkey != mr.mrHandle || mr.rkey != mr.mrHandle ||
            !mr.pdHandle || mr.pdHandle >= ObjectTableEntries ||
            !objects.pdAllocated[mr.pdHandle] ||
            !validMrAccess(mr.accessFlags) ||
            !mrPageCount(mr.start, mr.length, pages) ||
            mr.end != mr.start + mr.length || mr.pages.size() != pages ||
            std::any_of(mr.pages.begin(), mr.pages.end(),
                        [](uint64_t page) { return !alignedPage(page); }))
            return false;
        ++children[mr.pdHandle];
    }
    return children == objects.pdChildren;
}

inline bool
validQueueObjects(const CompletionQueueTable &cqs,
                  const QueuePairTable &qps,
                  const MemoryRegionTable &mrs,
                  const ObjectTables &objects)
{
    if (cqs.entries[0].valid || cqs.entries[0].generation ||
        qps.entries[0].valid || qps.entries[0].generation)
        return false;
    ObjectTables mr_objects = objects;
    mr_objects.pdChildren = {};
    for (uint32_t slot = 1; slot < ObjectTableEntries; ++slot) {
        const auto &mr = mrs.entries[slot];
        if (mr.valid) {
            if (!mr.pdHandle || mr.pdHandle >= ObjectTableEntries)
                return false;
            ++mr_objects.pdChildren[mr.pdHandle];
        }
    }
    if (!validMemoryRegions(mrs, mr_objects))
        return false;
    std::array<uint32_t, ObjectTableEntries> context_cqs{};
    auto pd_children = mr_objects.pdChildren;
    std::array<uint32_t, ObjectTableEntries> cq_references{};
    for (uint32_t slot = 1; slot < ObjectTableEntries; ++slot) {
        const auto &cq = cqs.entries[slot];
        if (!cq.valid) {
            if (cq.generation > MaxGeneration + uint64_t{1} ||
                cq.cqHandle || cq.contextHandle || cq.uar || cq.cqe ||
                cq.qpReferences || cq.armFlags || cq.producerTail ||
                cq.consumerHead || !cq.pages.empty())
                return false;
        } else {
            const uint32_t chunks = 1 + chunksFor(cq.cqe, CqeSize);
            if (cq.generation > MaxGeneration || cq.cqHandle != slot ||
                !cq.contextHandle ||
                cq.contextHandle >= ObjectTableEntries ||
                !objects.contextUar[cq.contextHandle] ||
                cq.uar != objects.contextUar[cq.contextHandle] ||
                !powerOfTwo(cq.cqe) || cq.cqe > 1024 ||
                cq.armFlags > static_cast<uint32_t>(CqArmMode::Any) ||
                !ringSnapshotValid(cq.producerTail, cq.consumerHead,
                                   cq.cqe) ||
                cq.pages.size() != chunks ||
                std::any_of(cq.pages.begin(), cq.pages.end(),
                    [](uint64_t page) { return !alignedPage(page); }))
                return false;
            ++context_cqs[cq.contextHandle];
        }

        const auto &qp = qps.entries[slot];
        if (!qp.valid) {
            if (qp.generation > MaxGeneration + uint64_t{1} ||
                qp.qpHandle || qp.qpn || qp.pdHandle ||
                qp.sendCqHandle || qp.recvCqHandle ||
                qp.contextHandle || qp.uar || qp.sendChunks ||
                qp.recvChunks || qp.totalChunks || qp.signalAllSendWr ||
                qp.state != QpState::Reset ||
                qp.capabilities.maxSendWr || qp.capabilities.maxRecvWr ||
                qp.capabilities.maxSendSge || qp.capabilities.maxRecvSge ||
                qp.capabilities.maxInlineData ||
                qp.capabilities.reserved ||
                !validStoredQpAttributes(qp) ||
                qp.sqProducerTail || qp.sqConsumerHead ||
                qp.rqProducerTail || qp.rqConsumerHead ||
                !validFinalReplay(qp) || !qp.pages.empty())
                return false;
            continue;
        }
        if (qp.generation > MaxGeneration || qp.qpHandle != slot ||
            qp.qpn != ((qp.generation << SlotBits) | slot) ||
            !qp.pdHandle || qp.pdHandle >= ObjectTableEntries ||
            !objects.pdAllocated[qp.pdHandle] || !qp.contextHandle ||
            objects.pdParent[qp.pdHandle] != qp.contextHandle ||
            !objects.contextUar[qp.contextHandle] ||
            qp.uar != objects.contextUar[qp.contextHandle] ||
            !qp.sendCqHandle || qp.sendCqHandle >= ObjectTableEntries ||
            !qp.recvCqHandle || qp.recvCqHandle >= ObjectTableEntries ||
            !cqs.entries[qp.sendCqHandle].valid ||
            !cqs.entries[qp.recvCqHandle].valid ||
            cqs.entries[qp.sendCqHandle].contextHandle !=
                qp.contextHandle ||
            cqs.entries[qp.recvCqHandle].contextHandle !=
                qp.contextHandle ||
            !powerOfTwo(qp.capabilities.maxSendWr) ||
            qp.capabilities.maxSendWr > 256 ||
            !powerOfTwo(qp.capabilities.maxRecvWr) ||
            qp.capabilities.maxRecvWr > 256 ||
            qp.capabilities.maxSendSge != 1 ||
            qp.capabilities.maxRecvSge != 1 ||
            qp.capabilities.maxInlineData || qp.capabilities.reserved ||
            qp.sendChunks != chunksFor(qp.capabilities.maxSendWr,
                                        SqStride) ||
            qp.recvChunks != chunksFor(qp.capabilities.maxRecvWr,
                                        RqStride) ||
            qp.totalChunks != 1 + qp.sendChunks + qp.recvChunks ||
            qp.pages.size() != qp.totalChunks ||
            !validSupportedState(qp.state) ||
            !validStoredQpAttributes(qp) || !validFinalReplay(qp) ||
            !ringSnapshotValid(qp.sqProducerTail, qp.sqConsumerHead,
                               qp.capabilities.maxSendWr) ||
            !ringSnapshotValid(qp.rqProducerTail, qp.rqConsumerHead,
                               qp.capabilities.maxRecvWr) ||
            (qp.state == QpState::Reset &&
             (qp.sqProducerTail != qp.sqConsumerHead ||
              qp.rqProducerTail != qp.rqConsumerHead)) ||
            std::any_of(qp.pages.begin(), qp.pages.end(),
                [](uint64_t page) { return !alignedPage(page); }))
            return false;
        ++pd_children[qp.pdHandle];
        ++cq_references[qp.sendCqHandle];
        ++cq_references[qp.recvCqHandle];
    }
    for (uint32_t slot = 1; slot < ObjectTableEntries; ++slot) {
        if (cqs.entries[slot].qpReferences != cq_references[slot])
            return false;
    }
    return context_cqs == objects.contextCqChildren &&
        pd_children == objects.pdChildren;
}

inline bool
translate(const MemoryRegionTable &mrs, MrKeyType key_type, uint32_t key,
          uint64_t address, uint64_t length, uint32_t required_access,
          std::vector<DmaChunk> &chunks)
{
    chunks.clear();
    if (!length || (required_access & ~SupportedMrAccess) ||
        address > std::numeric_limits<uint64_t>::max() - length)
        return false;

    const MemoryRegion *mr = nullptr;
    for (uint32_t slot = 1; slot < ObjectTableEntries; ++slot) {
        const auto &candidate = mrs.entries[slot];
        const uint32_t candidate_key = key_type == MrKeyType::Local ?
            candidate.lkey : candidate.rkey;
        if (candidate.valid && candidate_key == key) {
            mr = &candidate;
            break;
        }
    }
    if (!mr || (mr->accessFlags & required_access) != required_access ||
        address < mr->start || address + length > mr->end)
        return false;

    uint64_t offset = address - mr->start;
    uint64_t remaining = length;
    const uint64_t first_offset = mr->start & (MrPageSize - 1);
    while (remaining) {
        const uint64_t mapped = first_offset + offset;
        const uint64_t page_index = mapped / MrPageSize;
        const uint32_t page_offset = mapped % MrPageSize;
        if (page_index >= mr->pages.size()) {
            chunks.clear();
            return false;
        }
        const uint32_t size = std::min<uint64_t>(
            remaining, MrPageSize - page_offset);
        chunks.push_back({mr->pages[page_index] + page_offset, size});
        remaining -= size;
        offset += size;
    }
    return true;
}

inline bool
consumePageDirectory(const PageDirectoryBuild &build,
                     const std::array<uint64_t, PageEntries> &directory,
                     std::vector<uint64_t> &tables)
{
    const uint32_t count = (build.numChunks + PageEntries - 1) /
                           PageEntries;
    tables.clear();
    tables.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t address = letoh(directory[i]);
        if (!alignedPage(address)) {
            tables.clear();
            return false;
        }
        tables.push_back(address);
    }
    return true;
}

inline bool
consumePageTable(PageDirectoryBuild &build, uint32_t table_index,
                 const std::array<uint64_t, PageEntries> &table)
{
    if (table_index != build.pages.size() / PageEntries ||
        build.pages.size() >= build.numChunks)
        return false;

    const uint32_t count = std::min<uint32_t>(
        PageEntries, build.numChunks - build.pages.size());
    const size_t original_size = build.pages.size();
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t address = letoh(table[i]);
        if (!alignedPage(address)) {
            build.pages.resize(original_size);
            return false;
        }
        build.pages.push_back(address);
    }
    return true;
}

template <size_t N>
inline bool
allZero(const uint8_t (&bytes)[N])
{
    return std::all_of(std::begin(bytes), std::end(bytes),
                       [](uint8_t byte) { return byte == 0; });
}

inline bool
prepareCreateCq(const CommandRequest &request, const ObjectTables &objects,
                const CompletionQueueTable &cqs,
                CompletionQueueBuild &build)
{
    const auto &cmd = request.createCq;
    const uint32_t context = letoh(cmd.contextHandle);
    const uint32_t cqe = letoh(cmd.cqe);
    const uint32_t chunks = letoh(cmd.numChunks);
    const uint64_t directory = letoh(cmd.pageDirectoryDma);
    const uint32_t expected = 1 + chunksFor(cqe, CqeSize);
    if (letoh(request.header.reserved) || !allZero(cmd.reserved) ||
        !context || context >= ObjectTableEntries ||
        !objects.contextUar[context] || !powerOfTwo(cqe) || cqe > 1024 ||
        chunks != expected || !alignedPage(directory) ||
        !cqs.allocate(build))
        return false;
    build.contextHandle = context;
    build.cqe = cqe;
    build.numChunks = chunks;
    build.pageDirectoryDma = directory;
    build.pages.clear();
    build.pages.reserve(chunks);
    return true;
}

inline bool
prepareCreateQp(const CommandRequest &request, const ObjectTables &objects,
                const CompletionQueueTable &cqs,
                const QueuePairTable &qps, QueuePairBuild &build)
{
    const auto &cmd = request.createQp;
    const uint32_t pd = letoh(cmd.pdHandle);
    const uint32_t send_cq = letoh(cmd.sendCqHandle);
    const uint32_t recv_cq = letoh(cmd.recvCqHandle);
    const uint32_t send_wr = letoh(cmd.maxSendWr);
    const uint32_t recv_wr = letoh(cmd.maxRecvWr);
    const uint32_t send_chunks = letoh(cmd.sendChunks);
    const uint32_t total_chunks = letoh(cmd.totalChunks);
    const uint32_t expected_send = chunksFor(send_wr, SqStride);
    const uint32_t recv_chunks = chunksFor(recv_wr, RqStride);
    const uint32_t context = pd < ObjectTableEntries ?
        objects.pdParent[pd] : 0;
    const uint64_t directory = letoh(cmd.pageDirectoryDma);
    if (letoh(request.header.reserved) || !allZero(cmd.reserved) ||
        !pd || pd >= ObjectTableEntries || !objects.pdAllocated[pd] ||
        !context || !objects.contextUar[context] || !send_cq ||
        send_cq >= ObjectTableEntries || !recv_cq ||
        recv_cq >= ObjectTableEntries || !cqs.entries[send_cq].valid ||
        !cqs.entries[recv_cq].valid ||
        cqs.entries[send_cq].contextHandle != context ||
        cqs.entries[recv_cq].contextHandle != context ||
        letoh(cmd.srqHandle) || cmd.isSrq ||
        cmd.qpType != static_cast<uint8_t>(QpType::Rc) ||
        !powerOfTwo(send_wr) || send_wr > 256 ||
        !powerOfTwo(recv_wr) || recv_wr > 256 ||
        letoh(cmd.maxSendSge) != 1 || letoh(cmd.maxRecvSge) != 1 ||
        letoh(cmd.maxInlineData) || letoh(cmd.lkey) ||
        letoh(cmd.accessFlags) != AccessLocalWrite ||
        letoh(cmd.maxAtomicArgument) || cmd.signalAllSendWr > 1 ||
        send_chunks != expected_send ||
        total_chunks != 1 + expected_send + recv_chunks ||
        !alignedPage(directory) || !qps.allocate(build))
        return false;
    build.pdHandle = pd;
    build.sendCqHandle = send_cq;
    build.recvCqHandle = recv_cq;
    build.contextHandle = context;
    build.sendChunks = expected_send;
    build.recvChunks = recv_chunks;
    build.signalAllSendWr = cmd.signalAllSendWr != 0;
    build.capabilities.maxSendWr = send_wr;
    build.capabilities.maxRecvWr = recv_wr;
    build.capabilities.maxSendSge = 1;
    build.capabilities.maxRecvSge = 1;
    build.numChunks = total_chunks;
    build.pageDirectoryDma = directory;
    build.pages.clear();
    build.pages.reserve(total_chunks);
    return true;
}

inline bool
recognizedCommand(uint32_t command)
{
    return command <= static_cast<uint32_t>(Command::DestroySrq);
}

inline bool
uarIndex(uint64_t pfn, UarRange range, uint32_t &index)
{
    if (!range.size || (range.start % UarPageSize) != 0 ||
        pfn > std::numeric_limits<uint64_t>::max() / UarPageSize)
        return false;

    const uint64_t address = pfn * UarPageSize;
    if (address < range.start || range.size < UarPageSize)
        return false;

    const uint64_t offset = address - range.start;
    if ((offset % UarPageSize) != 0 || offset > range.size - UarPageSize)
        return false;

    const uint64_t page = offset / UarPageSize;
    if (page == 0 || page >= ObjectTableEntries)
        return false;
    index = page;
    return true;
}

inline bool
validObjectTables(const ObjectTables &objects, UarRange range)
{
    if (objects.contextUar[0] || objects.contextPdChildren[0] ||
        objects.contextCqChildren[0] || objects.pdAllocated[0] ||
        objects.pdParent[0] || objects.pdChildren[0])
        return false;

    std::array<uint8_t, ObjectTableEntries> ownedUars{};
    std::array<uint32_t, ObjectTableEntries> pdChildren{};
    for (uint32_t handle = 1; handle < ObjectTableEntries; ++handle) {
        const uint32_t uar = objects.contextUar[handle];
        if (!uar) {
            if (objects.contextPdChildren[handle] ||
                objects.contextCqChildren[handle])
                return false;
            continue;
        }
        if (uar >= ObjectTableEntries || ownedUars[uar] ||
            range.size < UarPageSize ||
            static_cast<uint64_t>(uar) * UarPageSize >
                range.size - UarPageSize)
            return false;
        ownedUars[uar] = 1;
    }

    for (uint32_t handle = 1; handle < ObjectTableEntries; ++handle) {
        if (objects.pdAllocated[handle] > 1)
            return false;
        if (!objects.pdAllocated[handle]) {
            if (objects.pdParent[handle] || objects.pdChildren[handle])
                return false;
            continue;
        }
        const uint32_t parent = objects.pdParent[handle];
        if (parent) {
            if (parent >= ObjectTableEntries ||
                !objects.contextUar[parent])
                return false;
            ++pdChildren[parent];
        }
    }

    return pdChildren == objects.contextPdChildren;
}

inline void
setResponseHeader(ResponseHeader &header, const CommandHeader &request,
                  uint32_t command, uint32_t error)
{
    header = {};
    header.response = request.response;
    header.acknowledgement = recognizedCommand(command) ?
        htole(responseCommand(static_cast<Command>(command))) : 0;
    header.error = error ? CommandError : 0;
}

inline void
setCreateResponseHeader(ResponseHeader &header, const CommandHeader &request,
                        uint32_t command, bool success)
{
    setResponseHeader(header, request, command, !success);
    // Linux ignores CQ/QP create response errors and checks only the ACK.
    if (!success)
        header.acknowledgement = 0;
}

inline CommandResult
queryPort(const CommandRequest &request, CommandResponse &response)
{
    const bool valid = letoh(request.header.reserved) == 0 &&
                       request.queryPort.portNumber == 1;
    setResponseHeader(response.header, request.header,
                      static_cast<uint32_t>(Command::QueryPort), !valid);
    if (!valid)
        return {true, CommandError};

    auto &attrs = response.queryPort.attributes;
    attrs = {};
    attrs.state = static_cast<PortState>(
        htole(static_cast<uint32_t>(PortState::Active)));
    attrs.maxMtu = static_cast<Mtu>(
        htole(static_cast<uint32_t>(Mtu::Mtu1024)));
    attrs.activeMtu = attrs.maxMtu;
    attrs.gidTableLength = htole(GidTableEntries);
    attrs.maxMessageSize = htole(MaxMessageSize);
    attrs.pkeyTableLength = htole(uint16_t{1});
    return {true, 0};
}

inline CommandResult
queryPkey(const CommandRequest &request, CommandResponse &response)
{
    const bool valid = letoh(request.header.reserved) == 0 &&
                       request.queryPkey.portNumber == 1 &&
                       request.queryPkey.index == 0;
    setResponseHeader(response.header, request.header,
                      static_cast<uint32_t>(Command::QueryPkey), !valid);
    if (!valid)
        return {true, CommandError};

    response.queryPkey.pkey = htole(FullMembershipPkey);
    return {true, 0};
}

inline CommandResult
createBind(const CommandRequest &request, GidTable &gids,
           GidValidTable &gid_valid)
{
    const auto &bind = request.createBind;
    const uint32_t index = letoh(bind.index);
    if (letoh(request.header.reserved) != 0 || index >= GidTableEntries ||
        letoh(bind.mtu) != FixedMtu || bind.gidType != GidTypeRoceV1)
        return {false, CommandError};

    std::copy(std::begin(bind.newGid), std::end(bind.newGid),
              std::begin(gids[index].raw));
    gid_valid[index] = 1;
    return {false, 0};
}

inline CommandResult
createUc(const CommandRequest &request, CommandResponse &response,
         ObjectTables &objects, UarRange range)
{
    uint32_t uar = 0;
    uint32_t handle = 1;
    const bool valid = letoh(request.header.reserved) == 0 &&
        uarIndex(letoh(request.createUc.pfn64), range, uar);
    while (handle < ObjectTableEntries && objects.contextUar[handle])
        ++handle;
    const bool duplicate = valid && std::find(objects.contextUar.begin(),
        objects.contextUar.end(), uar) != objects.contextUar.end();
    const bool success = valid && !duplicate && handle < ObjectTableEntries;
    setResponseHeader(response.header, request.header,
                      static_cast<uint32_t>(Command::CreateUc), !success);
    if (!success)
        return {true, CommandError};

    objects.contextUar[handle] = uar;
    response.createUc.contextHandle = htole(handle);
    return {true, 0};
}

inline CommandResult
destroyUc(const CommandRequest &request, ObjectTables &objects)
{
    const uint32_t handle = letoh(request.destroyUc.contextHandle);
    if (letoh(request.header.reserved) != 0 ||
        !allZero(request.destroyUc.reserved) || handle == 0 ||
        handle >= ObjectTableEntries || !objects.contextUar[handle] ||
        objects.contextPdChildren[handle] ||
        objects.contextCqChildren[handle])
        return {false, CommandError};

    objects.contextUar[handle] = 0;
    return {false, 0};
}

inline CommandResult
createPd(const CommandRequest &request, CommandResponse &response,
         ObjectTables &objects)
{
    const uint32_t parent = letoh(request.createPd.contextHandle);
    uint32_t handle = 1;
    while (handle < ObjectTableEntries && objects.pdAllocated[handle])
        ++handle;
    const bool valid = letoh(request.header.reserved) == 0 &&
        allZero(request.createPd.reserved) &&
        (parent == 0 || (parent < ObjectTableEntries &&
                         objects.contextUar[parent])) &&
        handle < ObjectTableEntries;
    setResponseHeader(response.header, request.header,
                      static_cast<uint32_t>(Command::CreatePd), !valid);
    if (!valid)
        return {true, CommandError};

    objects.pdAllocated[handle] = 1;
    objects.pdParent[handle] = parent;
    if (parent)
        ++objects.contextPdChildren[parent];
    response.createPd.pdHandle = htole(handle);
    return {true, 0};
}

inline CommandResult
destroyPd(const CommandRequest &request, ObjectTables &objects)
{
    const uint32_t handle = letoh(request.destroyPd.pdHandle);
    if (letoh(request.header.reserved) != 0 ||
        !allZero(request.destroyPd.reserved) || handle == 0 ||
        handle >= ObjectTableEntries || !objects.pdAllocated[handle] ||
        objects.pdChildren[handle])
        return {false, CommandError};

    const uint32_t parent = objects.pdParent[handle];
    if (parent)
        --objects.contextPdChildren[parent];
    objects.pdAllocated[handle] = 0;
    objects.pdParent[handle] = 0;
    return {false, 0};
}

inline CommandResult
destroyMr(const CommandRequest &request, ObjectTables &objects,
          MemoryRegionTable &mrs)
{
    if (letoh(request.header.reserved) ||
        !allZero(request.destroyMr.reserved) ||
        !mrs.destroy(letoh(request.destroyMr.mrHandle), objects))
        return {false, CommandError};
    return {false, 0};
}

inline void
convertQpCapFromAbi(QpCap &dst, const QpCap &src)
{
    dst = src;
    dst.maxSendWr = letoh(src.maxSendWr);
    dst.maxRecvWr = letoh(src.maxRecvWr);
    dst.maxSendSge = letoh(src.maxSendSge);
    dst.maxRecvSge = letoh(src.maxRecvSge);
    dst.maxInlineData = letoh(src.maxInlineData);
    dst.reserved = letoh(src.reserved);
}

inline void
convertQpCapToAbi(QpCap &dst, const QpCap &src)
{
    dst = src;
    dst.maxSendWr = htole(src.maxSendWr);
    dst.maxRecvWr = htole(src.maxRecvWr);
    dst.maxSendSge = htole(src.maxSendSge);
    dst.maxRecvSge = htole(src.maxRecvSge);
    dst.maxInlineData = htole(src.maxInlineData);
    dst.reserved = htole(src.reserved);
}

inline void
convertAhFromAbi(AddressHandleAttr &dst, const AddressHandleAttr &src)
{
    dst = src;
    dst.globalRoute.flowLabel = letoh(src.globalRoute.flowLabel);
    dst.destinationLid = letoh(src.destinationLid);
    dst.vlanId = letoh(src.vlanId);
}

inline void
convertAhToAbi(AddressHandleAttr &dst, const AddressHandleAttr &src)
{
    dst = src;
    dst.globalRoute.flowLabel = htole(src.globalRoute.flowLabel);
    dst.destinationLid = htole(src.destinationLid);
    dst.vlanId = htole(src.vlanId);
}

inline bool
convertQpAttrFromAbi(const QpAttr &src, QpAttr &dst)
{
    if (!allZero(src.reserved) || src.capabilities.reserved ||
        src.addressHandle.globalRoute.reserved ||
        src.addressHandle.reserved ||
        src.alternateAddressHandle.globalRoute.reserved ||
        src.alternateAddressHandle.reserved)
        return false;
    dst = src;
    dst.qpState = static_cast<QpState>(
        letoh(static_cast<uint32_t>(src.qpState)));
    dst.currentQpState = static_cast<QpState>(
        letoh(static_cast<uint32_t>(src.currentQpState)));
    dst.pathMtu = static_cast<Mtu>(
        letoh(static_cast<uint32_t>(src.pathMtu)));
    dst.pathMigrationState = static_cast<MigrationState>(
        letoh(static_cast<uint32_t>(src.pathMigrationState)));
    dst.qkey = letoh(src.qkey);
    dst.receivePsn = letoh(src.receivePsn);
    dst.sendPsn = letoh(src.sendPsn);
    dst.destinationQpNumber = letoh(src.destinationQpNumber);
    dst.qpAccessFlags = letoh(src.qpAccessFlags);
    dst.pkeyIndex = letoh(src.pkeyIndex);
    dst.alternatePkeyIndex = letoh(src.alternatePkeyIndex);
    convertQpCapFromAbi(dst.capabilities, src.capabilities);
    convertAhFromAbi(dst.addressHandle, src.addressHandle);
    convertAhFromAbi(dst.alternateAddressHandle,
                     src.alternateAddressHandle);
    return true;
}

inline void
convertQpAttrToAbi(const QpAttr &src, QpAttr &dst)
{
    dst = src;
    dst.qpState = static_cast<QpState>(
        htole(static_cast<uint32_t>(src.qpState)));
    dst.currentQpState = static_cast<QpState>(
        htole(static_cast<uint32_t>(src.currentQpState)));
    dst.pathMtu = static_cast<Mtu>(
        htole(static_cast<uint32_t>(src.pathMtu)));
    dst.pathMigrationState = static_cast<MigrationState>(
        htole(static_cast<uint32_t>(src.pathMigrationState)));
    dst.qkey = htole(src.qkey);
    dst.receivePsn = htole(src.receivePsn);
    dst.sendPsn = htole(src.sendPsn);
    dst.destinationQpNumber = htole(src.destinationQpNumber);
    dst.qpAccessFlags = htole(src.qpAccessFlags);
    dst.pkeyIndex = htole(src.pkeyIndex);
    dst.alternatePkeyIndex = htole(src.alternatePkeyIndex);
    convertQpCapToAbi(dst.capabilities, src.capabilities);
    convertAhToAbi(dst.addressHandle, src.addressHandle);
    convertAhToAbi(dst.alternateAddressHandle,
                   src.alternateAddressHandle);
}

inline CommandResult
destroyCq(const CommandRequest &request, ObjectTables &objects,
          CompletionQueueTable &cqs)
{
    if (letoh(request.header.reserved) ||
        !allZero(request.destroyCq.reserved) ||
        !cqs.destroy(letoh(request.destroyCq.cqHandle), objects))
        return {false, CommandError};
    return {false, 0};
}

inline CommandResult
modifyQp(const CommandRequest &request, CommandResponse &response,
         QueuePairTable &qps)
{
    const uint32_t handle = letoh(request.modifyQp.qpHandle);
    const uint32_t mask = letoh(request.modifyQp.attributeMask);
    constexpr uint32_t KnownMask = (uint32_t{1} << 21) - 1;
    constexpr uint32_t InitMask = QpAttrState | QpAttrAccessFlags |
        QpAttrPkeyIndex | QpAttrPort;
    constexpr uint32_t RtrMask = QpAttrState | QpAttrAddressVector |
        QpAttrPathMtu | QpAttrReceivePsn | QpAttrMaxDestReadAtomic |
        QpAttrMinRnrTimer | QpAttrDestinationQpn;
    constexpr uint32_t RtsMask = QpAttrState | QpAttrTimeout |
        QpAttrRetryCount | QpAttrRnrRetry | QpAttrSendPsn |
        QpAttrMaxQpReadAtomic;
    QpAttr attrs{};
    bool success = !letoh(request.header.reserved) && handle &&
        handle < ObjectTableEntries && qps.entries[handle].valid &&
        !(mask & ~KnownMask) && (mask & QpAttrState) &&
        !(mask & (QpAttrEnableSqdAsyncNotify | QpAttrQkey |
                  QpAttrAlternatePath | QpAttrPathMigrationState |
                  QpAttrCapabilities)) &&
        convertQpAttrFromAbi(request.modifyQp.attributes, attrs) &&
        validSupportedState(attrs.qpState) && attrs.qpState != QpState::Error;
    auto *qp = success ? &qps.entries[handle] : nullptr;
    if (success && (mask & QpAttrCurrentState))
        success = validSupportedState(attrs.currentQpState) &&
            attrs.currentQpState == qp->state;
    const uint32_t transition_mask = mask & ~QpAttrCurrentState;
    if (success && attrs.qpState == QpState::Reset) {
        success = transition_mask == QpAttrState;
        if (success) {
            qp->state = QpState::Reset;
            qp->attributes = {};
            qp->attributes.capabilities = qp->capabilities;
            qp->sqProducerTail = qp->sqConsumerHead = 0;
            qp->rqProducerTail = qp->rqConsumerHead = 0;
            qp->finalReplay = {};
        }
    } else if (success && qp->state == QpState::Reset &&
               attrs.qpState == QpState::Init) {
        success = transition_mask == InitMask && !attrs.pkeyIndex &&
            attrs.portNumber == 1 && validMrAccess(attrs.qpAccessFlags);
        if (success) {
            qp->state = QpState::Init;
            qp->attributes.qpAccessFlags = attrs.qpAccessFlags;
            qp->attributes.pkeyIndex = attrs.pkeyIndex;
            qp->attributes.portNumber = attrs.portNumber;
        }
    } else if (success && qp->state == QpState::Init &&
               attrs.qpState == QpState::ReadyToReceive) {
        success = transition_mask == RtrMask &&
            attrs.pathMtu == Mtu::Mtu1024 &&
            attrs.destinationQpNumber && validPsn(attrs.receivePsn) &&
            attrs.maxDestinationReadAtomic <= 1 &&
            attrs.minRnrTimer <= 31 &&
            validAddressVector(attrs.addressHandle);
        if (success) {
            qp->state = QpState::ReadyToReceive;
            qp->attributes.pathMtu = attrs.pathMtu;
            qp->attributes.destinationQpNumber =
                attrs.destinationQpNumber;
            qp->attributes.receivePsn = attrs.receivePsn;
            qp->attributes.maxDestinationReadAtomic =
                attrs.maxDestinationReadAtomic;
            qp->attributes.minRnrTimer = attrs.minRnrTimer;
            qp->attributes.addressHandle = attrs.addressHandle;
        }
    } else if (success && qp->state == QpState::ReadyToReceive &&
               attrs.qpState == QpState::ReadyToSend) {
        success = transition_mask == RtsMask && validPsn(attrs.sendPsn) &&
            attrs.maxReadAtomic <= 1 && attrs.timeout <= 31 &&
            attrs.retryCount <= 7 && attrs.rnrRetry <= 7;
        if (success) {
            qp->state = QpState::ReadyToSend;
            qp->attributes.timeout = attrs.timeout;
            qp->attributes.retryCount = attrs.retryCount;
            qp->attributes.rnrRetry = attrs.rnrRetry;
            qp->attributes.sendPsn = attrs.sendPsn;
            qp->attributes.maxReadAtomic = attrs.maxReadAtomic;
        }
    } else {
        success = false;
    }
    if (success) {
        qp->attributes.qpState = qp->state;
        qp->attributes.currentQpState = qp->state;
    }
    setResponseHeader(response.header, request.header,
                      static_cast<uint32_t>(Command::ModifyQp), !success);
    return {true, success ? 0U : CommandError};
}

inline CommandResult
queryQp(const CommandRequest &request, CommandResponse &response,
        const QueuePairTable &qps)
{
    const uint32_t handle = letoh(request.queryQp.qpHandle);
    const uint32_t mask = letoh(request.queryQp.attributeMask);
    constexpr uint32_t KnownMask = (uint32_t{1} << 21) - 1;
    const bool success = !letoh(request.header.reserved) && handle &&
        handle < ObjectTableEntries && qps.entries[handle].valid &&
        !(mask & ~KnownMask);
    setResponseHeader(response.header, request.header,
                      static_cast<uint32_t>(Command::QueryQp), !success);
    if (!success)
        return {true, CommandError};
    convertQpAttrToAbi(qps.entries[handle].attributes,
                       response.queryQp.attributes);
    return {true, 0};
}

inline CommandResult
destroyQp(const CommandRequest &request, ObjectTables &objects,
          CompletionQueueTable &cqs, QueuePairTable &qps)
{
    const bool success = !letoh(request.header.reserved) &&
        allZero(request.destroyQp.reserved) &&
        qps.destroy(letoh(request.destroyQp.qpHandle), objects, cqs);
    return {false, success ? 0U : CommandError};
}

inline CommandResult
destroyBind(const CommandRequest &request, GidTable &gids,
            GidValidTable &gid_valid)
{
    const auto &bind = request.destroyBind;
    const uint32_t index = letoh(bind.index);
    if (letoh(request.header.reserved) != 0 || index >= GidTableEntries ||
        !gid_valid[index] ||
        !std::equal(std::begin(bind.destinationGid),
                    std::end(bind.destinationGid),
                    std::begin(gids[index].raw))) {
        return {false, CommandError};
    }

    gids[index] = {};
    gid_valid[index] = 0;
    return {false, 0};
}

} // namespace detail

inline bool
validObjectTables(const ObjectTables &objects, UarRange range)
{
    return detail::validObjectTables(objects, range);
}

inline bool
prepareCreateMr(const CommandRequest &request, const ObjectTables &objects,
                const MemoryRegionTable &mrs, MemoryRegionBuild &build)
{
    return detail::prepareCreateMr(request, objects, mrs, build);
}

inline bool
consumeMrDirectory(const MemoryRegionBuild &build,
                   const std::array<uint64_t, MrEntriesPerPage> &directory,
                   std::vector<uint64_t> &tables)
{
    return detail::consumePageDirectory(build, directory, tables);
}

inline bool
consumeMrTable(MemoryRegionBuild &build, uint32_t table_index,
               const std::array<uint64_t, MrEntriesPerPage> &table)
{
    return detail::consumePageTable(build, table_index, table);
}

inline bool
prepareCreateCq(const CommandRequest &request, const ObjectTables &objects,
                const CompletionQueueTable &cqs,
                CompletionQueueBuild &build)
{
    return detail::prepareCreateCq(request, objects, cqs, build);
}

inline bool
prepareCreateQp(const CommandRequest &request, const ObjectTables &objects,
                const CompletionQueueTable &cqs,
                const QueuePairTable &qps, QueuePairBuild &build)
{
    return detail::prepareCreateQp(request, objects, cqs, qps, build);
}

inline bool
consumePageDirectory(const PageDirectoryBuild &build,
                     const std::array<uint64_t, PageEntries> &directory,
                     std::vector<uint64_t> &tables)
{
    return detail::consumePageDirectory(build, directory, tables);
}

inline bool
consumePageTable(PageDirectoryBuild &build, uint32_t table_index,
                 const std::array<uint64_t, PageEntries> &table)
{
    return detail::consumePageTable(build, table_index, table);
}

inline bool
validMemoryRegions(const MemoryRegionTable &mrs,
                   const ObjectTables &objects)
{
    return detail::validMemoryRegions(mrs, objects);
}

inline bool
validQueueObjects(const CompletionQueueTable &cqs,
                  const QueuePairTable &qps,
                  const MemoryRegionTable &mrs,
                  const ObjectTables &objects)
{
    return detail::validQueueObjects(cqs, qps, mrs, objects);
}

inline bool
translate(const MemoryRegionTable &mrs, MrKeyType key_type, uint32_t key,
          uint64_t address, uint64_t length, uint32_t required_access,
          std::vector<DmaChunk> &chunks)
{
    return detail::translate(mrs, key_type, key, address, length,
                             required_access, chunks);
}

inline bool
acquireLocalMr(MemoryRegionTable &mrs, const QueuePair &qp, QueueKind kind,
               uint32_t lkey, uint64_t address, uint64_t length,
               MemoryRegionLease &lease)
{
    const uint32_t slot = lkey & MrSlotMask;
    const uint32_t required_access = kind == QueueKind::Rq ?
        AccessLocalWrite : 0;
    if (!qp.valid || !qp.qpHandle || qp.qpHandle >= ObjectTableEntries ||
        qp.generation > MaxGeneration ||
        qp.qpn != ((qp.generation << SlotBits) | qp.qpHandle) ||
        !qp.pdHandle || qp.pdHandle >= ObjectTableEntries || !slot ||
        slot >= ObjectTableEntries ||
        (kind != QueueKind::Sq && kind != QueueKind::Rq))
        return false;

    auto &mr = mrs.entries[slot];
    if (!mr.valid || mr.generation > MaxMrGeneration ||
        mr.mrHandle != ((mr.generation << MrSlotBits) | slot) ||
        mr.lkey != lkey || mr.mrHandle != lkey ||
        mr.pdHandle != qp.pdHandle ||
        (mr.accessFlags & required_access) != required_access ||
        mr.activeReferences == std::numeric_limits<uint32_t>::max())
        return false;

    std::vector<DmaChunk> chunks;
    if (length) {
        if (!detail::translate(mrs, MrKeyType::Local, lkey, address, length,
                               required_access, chunks))
            return false;
    } else if (address < mr.start || address > mr.end) {
        return false;
    }

    ++mr.activeReferences;
    lease = {slot, mr.generation, std::move(chunks)};
    return true;
}

inline bool
releaseMr(MemoryRegionTable &mrs, const MemoryRegionLease &lease)
{
    if (!lease.slot || lease.slot >= ObjectTableEntries)
        return false;
    auto &mr = mrs.entries[lease.slot];
    if (!mr.valid || mr.generation > MaxMrGeneration ||
        mr.generation != lease.generation ||
        mr.mrHandle != ((mr.generation << MrSlotBits) | lease.slot) ||
        !mr.activeReferences)
        return false;
    --mr.activeReferences;
    return true;
}

inline DeviceCaps
makeCapabilities(uint32_t mac_low, uint32_t mac_high)
{
    DeviceCaps caps{};
    const uint64_t guid = detail::macGuid(mac_low, mac_high);
    caps.fwVersion = htole(uint64_t{1});
    caps.beNodeGuid = htobe(guid);
    caps.beSystemImageGuid = htobe(guid);
    caps.maxMrSize = htole(std::numeric_limits<uint64_t>::max());
    caps.pageSizeCap = htole(uint64_t{UarPageSize});
    caps.vendorId = htole(static_cast<uint32_t>(PciVendorId));
    caps.vendorPartId = htole(static_cast<uint32_t>(PciDeviceId));
    caps.hardwareVersion = htole(uint32_t{1});
    caps.maxQp = htole(uint32_t{64});
    caps.maxQpWr = htole(uint32_t{256});
    caps.maxSge = htole(uint32_t{1});
    caps.maxSgeRd = 0;
    caps.maxCq = htole(uint32_t{64});
    caps.maxCqe = htole(uint32_t{1024});
    caps.maxMr = htole(uint32_t{64});
    caps.maxPd = htole(uint32_t{64});
    caps.maxQpRdAtom = htole(uint32_t{1});
    caps.maxQpInitRdAtom = htole(uint32_t{1});
    caps.maxUar = htole(uint32_t{64});
    caps.gidTableLength = htole(GidTableEntries);
    caps.maxPkeys = htole(uint16_t{1});
    caps.physicalPortCount = 1;
    caps.mode = static_cast<uint8_t>(DeviceMode::Roce);
    caps.gidTypes = GidTypeRoceV1;
    return caps;
}

inline bool
validSharedRegion(const DeviceSharedRegion &dsr, uint64_t dsr_address)
{
    return detail::alignedPage(dsr_address) &&
           letoh(dsr.driverVersion) == Version &&
           detail::alignedPage(letoh(dsr.commandSlotDma)) &&
           detail::alignedPage(letoh(dsr.responseSlotDma)) &&
           letoh(dsr.asyncRingPages.numPages) == NumRingPages &&
           detail::alignedPage(letoh(dsr.asyncRingPages.pageDirectoryDma)) &&
           letoh(dsr.completionRingPages.numPages) == NumRingPages &&
           detail::alignedPage(
               letoh(dsr.completionRingPages.pageDirectoryDma));
}

inline CommandResult
processCommand(const CommandRequest &request, CommandResponse &response,
               GidTable &gids, GidValidTable &gid_valid,
               ObjectTables &objects, MemoryRegionTable &mrs,
               CompletionQueueTable &cqs, QueuePairTable &qps,
               UarRange uar_range)
{
    response = {};
    const uint32_t command = letoh(request.header.command);
    switch (command) {
      case static_cast<uint32_t>(Command::QueryPort):
        return detail::queryPort(request, response);
      case static_cast<uint32_t>(Command::QueryPkey):
        return detail::queryPkey(request, response);
      case static_cast<uint32_t>(Command::CreatePd):
        return detail::createPd(request, response, objects);
      case static_cast<uint32_t>(Command::DestroyPd):
        return detail::destroyPd(request, objects);
      case static_cast<uint32_t>(Command::DestroyMr):
        return detail::destroyMr(request, objects, mrs);
      case static_cast<uint32_t>(Command::DestroyCq):
        return detail::destroyCq(request, objects, cqs);
      case static_cast<uint32_t>(Command::ModifyQp):
        return detail::modifyQp(request, response, qps);
      case static_cast<uint32_t>(Command::QueryQp):
        return detail::queryQp(request, response, qps);
      case static_cast<uint32_t>(Command::DestroyQp):
        return detail::destroyQp(request, objects, cqs, qps);
      case static_cast<uint32_t>(Command::CreateUc):
        return detail::createUc(request, response, objects, uar_range);
      case static_cast<uint32_t>(Command::DestroyUc):
        return detail::destroyUc(request, objects);
      case static_cast<uint32_t>(Command::CreateBind):
        return detail::createBind(request, gids, gid_valid);
      case static_cast<uint32_t>(Command::DestroyBind):
        return detail::destroyBind(request, gids, gid_valid);
      default:
        detail::setResponseHeader(response.header, request.header, command,
                                  CommandError);
        return {true, CommandError};
    }
}

inline CommandResult
processCommand(const CommandRequest &request, CommandResponse &response,
               GidTable &gids, GidValidTable &gid_valid,
               ObjectTables &objects, MemoryRegionTable &mrs,
               UarRange uar_range)
{
    CompletionQueueTable cqs;
    QueuePairTable qps;
    return processCommand(request, response, gids, gid_valid, objects, mrs,
                          cqs, qps, uar_range);
}

inline CommandResult
processCommand(const CommandRequest &request, CommandResponse &response,
               GidTable &gids, GidValidTable &gid_valid,
               ObjectTables &objects, UarRange uar_range)
{
    MemoryRegionTable mrs;
    CompletionQueueTable cqs;
    QueuePairTable qps;
    return processCommand(request, response, gids, gid_valid, objects, mrs,
                          cqs, qps, uar_range);
}

} // namespace pvrdma

class PvrdmaTester;

class Pvrdma : public PciDevice
{
  public:
    PARAMS(Pvrdma);
    Pvrdma(const Params &params);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
    void startup() override;
    DrainState drain() override;

    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

  private:
    friend class PvrdmaTester;

    class Interface : public EtherInt
    {
      public:
        Interface(const std::string &name, Pvrdma &device)
            : EtherInt(name), device(device)
        {}
        bool recvPacket(EthPacketPtr packet) override;
        void sendDone() override;

      private:
        Pvrdma &device;
    } interface;

    pvrdma::RegisterState regs;
    pvrdma::ControlState controlState = pvrdma::ControlState::Unconfigured;
    uint64_t commandSlotAddress = 0;
    uint64_t responseSlotAddress = 0;
    Addr dsrDmaAddress = 0;
    Addr commandSlotDmaAddress = 0;
    Addr responseSlotDmaAddress = 0;
    pvrdma::DeviceSharedRegion dsr{};
    pvrdma::DeviceCaps capabilities{};
    pvrdma::CommandRequest command{};
    pvrdma::CommandResponse response{};
    pvrdma::GidTable gids{};
    pvrdma::GidValidTable gidValid{};
    pvrdma::ObjectTables objects{};
    pvrdma::MemoryRegionTable memoryRegions{};
    pvrdma::CompletionQueueTable completionQueues{};
    pvrdma::QueuePairTable queuePairs{};
    pvrdma::PendingCreateKind pendingCreate =
        pvrdma::PendingCreateKind::None;
    pvrdma::MemoryRegionBuild mrBuild{};
    pvrdma::CompletionQueueBuild cqBuild{};
    pvrdma::QueuePairBuild qpBuild{};
    std::array<uint64_t, pvrdma::PageEntries> objectDirectory{};
    std::array<uint64_t, pvrdma::PageEntries> objectTable{};
    std::vector<uint64_t> objectTables;
    uint32_t objectTableIndex = 0;
    pvrdma::OperationErrorState operationError;
    bool intxAsserted = false;
    const Tick controlCompletionLatency;

    struct QueueDmaState
    {
        pvrdma::QueueKind kind = pvrdma::QueueKind::None;
        uint32_t handle = 0;
        uint32_t generation = 0;
        uint32_t uar = 0;
        uint32_t depth = 0;
        uint64_t ringAddress = 0;
        pvrdma::Ring ring{};

        bool active() const { return kind != pvrdma::QueueKind::None; }
        void reset() { *this = {}; }
    } queueDma;

    struct CompletionDmaState
    {
        enum class Stage : uint8_t
        {
            Idle,
            Queued,
            ReadCqRing,
            WriteCqe,
            PublishCqProducer,
        } stage = Stage::Idle;

        pvrdma::CompletionRecord record{};
        std::function<void(pvrdma::CompletionSubmitResult)> done;
        pvrdma::Ring ring{};
        pvrdma::CompletionQueueElement cqe{};
        uint32_t cqSlot = 0;
        uint32_t cqNextProducer = 0;
        uint32_t cqProducerLe = 0;

        bool active() const { return stage != Stage::Idle; }
        bool queued() const { return stage == Stage::Queued; }
        void reset() { *this = {}; }
    } completionDma;

    struct TransportState
    {
        enum class Stage : uint8_t
        {
            Idle,
            ReadSqWqe,
            ReadSqPayload,
            TryData,
            WaitResponse,
            RetryWait,
            ReadRqWqe,
            WriteRqPayload,
            WaitReceiveData,
            WaitReceiveCq,
            WriteSqConsumer,
            WriteRqConsumer,
            TryAck,
            TryRnr,
            TryError,
            WaitSendCq,
        } stage = Stage::Idle;

        pvrdma::QueueKind kind = pvrdma::QueueKind::None;
        uint32_t qpHandle = 0;
        uint32_t qpGeneration = 0;
        uint32_t cqHandle = 0;
        uint32_t cqGeneration = 0;
        uint32_t consumer = 0;
        uint32_t nextConsumer = 0;
        uint32_t consumerLe = 0;
        uint64_t wqeAddress = 0;
        pvrdma::PreparedWqe wqe{};
        pvrdma::MemoryRegionLease lease{};
        bool leaseHeld = false;
        bool dmaBusy = false;
        bool completionBackpressured = false;
        bool keepAfterControl = false;
        bool terminalQpError = false;
        bool abortAfterDma = false;
        bool retryPending = false;
        size_t chunkIndex = 0;
        size_t chunkOffset = 0;
        size_t dmaPayloadOffset = 0;
        size_t dmaRemaining = 0;
        size_t dmaChunkLength = 0;
        std::array<uint8_t, pvrdma::SqStride> sqSlot{};
        std::array<uint8_t, pvrdma::RqStride> rqSlot{};
        std::vector<uint8_t> payload;
        EthPacketPtr packet;
        pvrdma::transport::MacAddress localMac{};
        pvrdma::transport::MacAddress remoteMac{};
        uint32_t localQpn = 0;
        uint32_t remoteQpn = 0;
        uint32_t psn = 0;
        uint32_t livePsn = 0;
        uint32_t initialPsn = 0;
        uint32_t acceptedPsn = 0;
        uint64_t messageId = 0;
        uint32_t totalLength = 0;
        uint16_t segmentIndex = 0;
        uint16_t segmentCount = 0;
        uint16_t acceptedSegmentIndex = 0;
        uint8_t retryRemaining = 0;
        uint8_t rnrRetryRemaining = 0;
        pvrdma::transport::Kind responseKind =
            pvrdma::transport::Kind::Data;
        pvrdma::CompletionStatus status =
            pvrdma::CompletionStatus::Success;

        bool active() const { return stage != Stage::Idle; }
        void reset() { *this = {}; }
    } transport;

    EthPacketPtr pendingRxPacket;
    EthPacketPtr pendingErrorPacket;
    EthPacketPtr precommitCompletionAbortPacket;
    bool precommitCompletionAbort = false;
    bool transportPaused = false;
    uint64_t receivePayloadDmaStarts = 0;
    enum class TransportTimerKind : uint8_t { None, Ack, Rnr };
    TransportTimerKind transportTimerKind = TransportTimerKind::None;

    struct QueueStats : public statistics::Group
    {
        QueueStats(Pvrdma &parent);
        void preDumpStats() override;

        Pvrdma &device;
        statistics::Scalar sqDepth;
        statistics::Scalar rqDepth;
        statistics::Scalar cqDepth;
        statistics::Scalar sqOutstanding;
        statistics::Scalar rqAvailable;
        statistics::Scalar cqOutstanding;
        statistics::Scalar sqOutstandingAtReset;
        statistics::Scalar rqAvailableAtReset;
        statistics::Scalar cqOutstandingAtReset;
        statistics::Scalar sqPosted;
        statistics::Scalar rqPosted;
        statistics::Scalar sqConsumed;
        statistics::Scalar rqConsumed;
        statistics::Scalar cqReclaimed;
        statistics::Scalar cqPublished;
        statistics::Scalar cqErrorPublished;
        statistics::Scalar cqPublicationRejected;
        statistics::Scalar cqPublicationBackpressured;
        statistics::Scalar sqResetDiscarded;
        statistics::Scalar rqResetDiscarded;
        statistics::Scalar cqResetDiscarded;
        statistics::Scalar doorbellWrites;
        statistics::Scalar sqDoorbells;
        statistics::Scalar rqDoorbells;
        statistics::Scalar cqPollDoorbells;
        statistics::Scalar cqArmDoorbells;
        statistics::Scalar cqArmSolicitedDoorbells;
        statistics::Scalar doorbellWritesRejected;
        statistics::Scalar ringObservationsRejected;
        statistics::Scalar conservationViolations;
        statistics::Distribution sqOccupancy;
        statistics::Distribution rqOccupancy;
        statistics::Distribution cqOccupancy;
    } queueStats;

    uint64_t sqDirty = 0;
    uint64_t rqDirty = 0;
    uint64_t cqDirty = 0;
    uint32_t observationCursor = 0;
    Tick lastQueueSample = 0;

    EventFunctionWrapper dsrReadEvent;
    EventFunctionWrapper capsWriteEvent;
    EventFunctionWrapper commandReadEvent;
    EventFunctionWrapper objectDirectoryReadEvent;
    EventFunctionWrapper objectTableReadEvent;
    EventFunctionWrapper responseWriteEvent;
    EventFunctionWrapper observationEvent;
    EventFunctionWrapper queueDmaEvent;
    EventFunctionWrapper completionDmaEvent;
    EventFunctionWrapper transportEvent;
    EventFunctionWrapper transportDmaEvent;
    EventFunctionWrapper transportTimerEvent;

    bool observationQueued() const;
    bool completionBusy() const;
    bool validDoorbell(const pvrdma::Doorbell &doorbell) const;
    void writeDoorbell(uint64_t offset, PacketPtr pkt);
    void markDirty(pvrdma::QueueKind kind, uint32_t handle);
    void scheduleObservation();
    void startObservation();
    bool selectObservation();
    bool revalidateObservation() const;
    void queueDmaDone();
    void finishObservation();
    void clearObservations();
    void sampleQueueOccupancy();
    void sampleCurrentQueueOccupancy();
    void refreshQueueGauges();
    void queueStatsReset();
    void checkQueueConservation();

    bool transportActive() const;
    bool transportDmaBusy() const;
    bool runnableSq() const;
    bool activeMr() const;
    bool revalidateTransport(bool requireCq = false) const;
    bool revalidateTransportLease() const;
    bool finalReceiveCommitted() const;
    bool cancelUncommittedTransportCompletion();
    bool recvTransportPacket(EthPacketPtr packet);
    void transportSendDone();
    void scheduleTransport();
    void runTransport();
    void transportDmaDone();
    bool selectSend();
    void queueReverseError(
        const pvrdma::transport::Frame &received,
        const pvrdma::transport::MacAddress &sourceMac);
    void startInbound();
    void startTransportDma(bool write, uint64_t address, size_t size,
                           uint8_t *data);
    bool beginPayloadDma(bool write, size_t offset, size_t length);
    void startPayloadDma(bool write);
    void submitTransportCompletion();
    void transportCompletionDone(pvrdma::CompletionSubmitResult result);
    void failSend(pvrdma::CompletionStatus status =
                      pvrdma::CompletionStatus::GeneralError,
                  bool notifyRemote = false, bool qpError = false);
    void prepareControl(pvrdma::transport::Kind kind,
                        pvrdma::CompletionStatus status =
                            pvrdma::CompletionStatus::Success);
    bool tryTransportPacket();
    void publishConsumer(pvrdma::QueueKind kind);
    void finishTransport(bool releaseLease = true);
    void clearTransport();
    void armTransportTimer(TransportTimerKind kind, Tick delay);
    void cancelTransportTimer();
    void transportTimerExpired();
    bool handleInboundContinuation(
        const pvrdma::transport::EthernetDecodeResult &decoded);
    bool replayFinal(const pvrdma::transport::EthernetDecodeResult &decoded);

    pvrdma::CompletionSubmitResult submitCompletion(
        const pvrdma::CompletionRecord &record,
        std::function<void(pvrdma::CompletionSubmitResult)> done = {});
    bool revalidateCompletion() const;
    void startCompletion();
    void completionDmaDone();
    void finishCompletion();
    void rejectCompletion(bool backpressure);

    void startDsr();
    void dsrReadDone();
    void capsWriteDone();
    void startCommand(uint32_t value);
    void commandReadDone();
    void startObjectCreate(pvrdma::PendingCreateKind kind);
    pvrdma::PageDirectoryBuild &pendingPageBuild();
    void objectDirectoryReadDone();
    void startObjectTableRead();
    void objectTableReadDone();
    void finishObjectCreate(bool success);
    void responseWriteDone();
    void writeControl(uint32_t value);
    void resetDevice();
    void updateInterrupt();
    void operationDone();
};

} // namespace gem5

#endif // __DEV_RDMA_PVRDMA_HH__
