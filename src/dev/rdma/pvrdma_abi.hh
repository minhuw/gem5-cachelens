// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2012-2016 VMware, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __DEV_RDMA_PVRDMA_ABI_HH__
#define __DEV_RDMA_PVRDMA_ABI_HH__

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace gem5
{
namespace pvrdma
{

/*
 * Clean-room declarations derived from the BSD-2-Clause alternative of the
 * VMware PVRDMA Linux ABI in pvrdma_dev_api.h, pvrdma_verbs.h, and
 * vmw_pvrdma-abi.h. No QEMU implementation code is used here.
 *
 * Multi-byte scalar fields are ABI storage, not host-endian values. The later
 * device must explicitly convert ordinary fields as little-endian and the
 * fields named be* as big-endian before interpreting them. Layout assertions
 * below make accidental host-layout changes a compile error.
 */

inline constexpr uint16_t PciVendorId = 0x15ad;
inline constexpr uint16_t PciDeviceId = 0x0820;
inline constexpr uint8_t BoardId = 1;
inline constexpr uint8_t RevisionId = 1;

inline constexpr unsigned MsixBar = 0;
inline constexpr unsigned RegisterBar = 1;
inline constexpr unsigned UarBar = 2;
inline constexpr uint64_t MsixBarSize = 16 * 1024;
inline constexpr uint64_t RegisterBarSize = 256;
inline constexpr uint64_t UarPageSize = 4096;
inline constexpr uint32_t MaxUserContexts = 512;
inline constexpr uint64_t UarBarSize = UarPageSize * MaxUserContexts;
inline constexpr unsigned MaxInterrupts = 3;
inline constexpr uint64_t MsixTableOffset = 0x0000;
inline constexpr uint64_t MsixPbaOffset = 0x2000;

inline constexpr uint32_t RoceV2Version = 18;
inline constexpr uint32_t Ppn64Version = 19;
inline constexpr uint32_t QpHandleVersion = 20;
inline constexpr uint32_t Version = QpHandleVersion;

inline constexpr uint32_t RegVersion = 0x00;
inline constexpr uint32_t RegDsrLow = 0x04;
inline constexpr uint32_t RegDsrHigh = 0x08;
inline constexpr uint32_t RegControl = 0x0c;
inline constexpr uint32_t RegRequest = 0x10;
inline constexpr uint32_t RegError = 0x14;
inline constexpr uint32_t RegInterruptCause = 0x18;
inline constexpr uint32_t RegInterruptMask = 0x1c;
inline constexpr uint32_t RegMacLow = 0x20;
inline constexpr uint32_t RegMacHigh = 0x24;

inline constexpr uint32_t UarHandleMask = 0x00ffffff;
inline constexpr uint32_t UarQpOffset = 0;
inline constexpr uint32_t UarQpSend = uint32_t{1} << 30;
inline constexpr uint32_t UarQpRecv = uint32_t{1} << 31;
inline constexpr uint32_t UarCqOffset = 4;
inline constexpr uint32_t UarCqArmSolicited = uint32_t{1} << 29;
inline constexpr uint32_t UarCqArm = uint32_t{1} << 30;
inline constexpr uint32_t UarCqPoll = uint32_t{1} << 31;

inline constexpr uint32_t CqFlagArmedSolicited = uint32_t{1} << 0;
inline constexpr uint32_t CqFlagArmed = uint32_t{1} << 1;
inline constexpr uint32_t MrFlagDma = uint32_t{1} << 0;

inline constexpr uint32_t GidTypeRoceV2 = uint32_t{1} << 1;

inline constexpr unsigned PageDirectoryShift = 18;
inline constexpr unsigned PageTableShift = 9;
inline constexpr uint32_t PageDirectoryMaxPages = 512 * 512;
inline constexpr uint32_t NumRingPages = 4;
inline constexpr uint32_t QpHeaderPages = 1;

/* Deliberately unsupported by the first gem5 PVRDMA device. */
inline constexpr bool SupportsSrq = false;
inline constexpr bool SupportsAh = false;
inline constexpr bool SupportsRdmaRead = false;
inline constexpr bool SupportsRdmaWrite = false;
inline constexpr bool SupportsAtomics = false;
inline constexpr bool SupportsSendRecv = true;

enum class DeviceControl : uint32_t
{
    Activate = 0,
    Unquiesce = 1,
    Reset = 2,
};

enum class InterruptVector : uint32_t
{
    Response = 0,
    Async = 1,
    Completion = 2,
};

inline constexpr uint32_t InterruptCauseResponse = uint32_t{1} << 0;
inline constexpr uint32_t InterruptCauseAsync = uint32_t{1} << 1;
inline constexpr uint32_t InterruptCauseCompletion = uint32_t{1} << 2;

enum class GuestOsBits : uint32_t
{
    Unknown = 0,
    Bits32 = 1,
    Bits64 = 2,
};

enum class GuestOsType : uint32_t
{
    Unknown = 0,
    Linux = 1,
};

enum class DeviceMode : uint8_t
{
    Roce = 0,
    Iwarp = 1,
    InfiniBand = 2,
};

constexpr uint32_t
guestOsInfo(GuestOsBits bits, GuestOsType type, uint16_t version,
            uint16_t misc = 0)
{
    return (static_cast<uint32_t>(bits) & 0x3) |
           ((static_cast<uint32_t>(type) & 0xf) << 2) |
           (static_cast<uint32_t>(version) << 6) |
           ((static_cast<uint32_t>(misc) & 0x3ff) << 22);
}

struct GuestOsInfo
{
    uint32_t info;
    uint32_t reserved;
};

struct DeviceCaps
{
    uint64_t fwVersion;
    uint64_t beNodeGuid;
    uint64_t beSystemImageGuid;
    uint64_t maxMrSize;
    uint64_t pageSizeCap;
    uint64_t atomicArgSizes;
    uint32_t exCompMask;
    uint32_t deviceCapFlags2;
    uint32_t maxFaBitBoundary;
    uint32_t logMaxAtomicInlineArg;
    uint32_t vendorId;
    uint32_t vendorPartId;
    uint32_t hardwareVersion;
    uint32_t maxQp;
    uint32_t maxQpWr;
    uint32_t deviceCapFlags;
    uint32_t maxSge;
    uint32_t maxSgeRd;
    uint32_t maxCq;
    uint32_t maxCqe;
    uint32_t maxMr;
    uint32_t maxPd;
    uint32_t maxQpRdAtom;
    uint32_t maxEeRdAtom;
    uint32_t maxResRdAtom;
    uint32_t maxQpInitRdAtom;
    uint32_t maxEeInitRdAtom;
    uint32_t maxEe;
    uint32_t maxRdd;
    uint32_t maxMw;
    uint32_t maxRawIpv6Qp;
    uint32_t maxRawEthernetQp;
    uint32_t maxMulticastGroup;
    uint32_t maxMulticastQpAttach;
    uint32_t maxTotalMulticastQpAttach;
    uint32_t maxAh;
    uint32_t maxFmr;
    uint32_t maxMapPerFmr;
    uint32_t maxSrq;
    uint32_t maxSrqWr;
    uint32_t maxSrqSge;
    uint32_t maxUar;
    uint32_t gidTableLength;
    uint16_t maxPkeys;
    uint8_t localCaAckDelay;
    uint8_t physicalPortCount;
    uint8_t mode;
    uint8_t atomicOps;
    uint8_t bmmeFlags;
    uint8_t gidTypes;
    uint32_t maxFastRegPageListLength;
};

struct RingPageInfo
{
    uint32_t numPages;
    uint32_t reserved;
    uint64_t pageDirectoryDma;
};

struct DeviceSharedRegion
{
    uint32_t driverVersion;
    uint32_t reserved0;
    GuestOsInfo guestOsInfo;
    uint64_t commandSlotDma;
    uint64_t responseSlotDma;
    RingPageInfo asyncRingPages;
    RingPageInfo completionRingPages;
    union
    {
        uint32_t uarPfn;
        uint64_t uarPfn64;
    };
    DeviceCaps caps;
};

struct Ring
{
    uint32_t producerTail;
    uint32_t consumerHead;
};

struct RingState
{
    Ring tx;
    Ring rx;
};

enum class Mtu : uint32_t
{
    Mtu256 = 1,
    Mtu512 = 2,
    Mtu1024 = 3,
    Mtu2048 = 4,
    Mtu4096 = 5,
};

enum class PortState : uint32_t
{
    Nop = 0,
    Down = 1,
    Init = 2,
    Armed = 3,
    Active = 4,
    ActiveDeferred = 5,
};

struct PortAttr
{
    PortState state;
    Mtu maxMtu;
    Mtu activeMtu;
    uint32_t gidTableLength;
    uint32_t portCapFlags;
    uint32_t maxMessageSize;
    uint32_t badPkeyCounter;
    uint32_t qkeyViolationCounter;
    uint16_t pkeyTableLength;
    uint16_t lid;
    uint16_t subnetManagerLid;
    uint8_t lmc;
    uint8_t maxVlNum;
    uint8_t subnetManagerSl;
    uint8_t subnetTimeout;
    uint8_t initTypeReply;
    uint8_t activeWidth;
    uint8_t activeSpeed;
    uint8_t physicalState;
    uint8_t reserved[2];
};

struct alignas(8) Gid
{
    uint8_t raw[16];
};

struct GlobalRoute
{
    Gid destinationGid;
    uint32_t flowLabel;
    uint8_t sourceGidIndex;
    uint8_t hopLimit;
    uint8_t trafficClass;
    uint8_t reserved;
};

/* Layout-only: AH creation and UD transport are not supported. */
struct AddressHandleAttr
{
    GlobalRoute globalRoute;
    uint16_t destinationLid;
    uint16_t vlanId;
    uint8_t serviceLevel;
    uint8_t sourcePathBits;
    uint8_t staticRate;
    uint8_t flags;
    uint8_t portNumber;
    uint8_t destinationMac[6];
    uint8_t reserved;
};

struct QpCap
{
    uint32_t maxSendWr;
    uint32_t maxRecvWr;
    uint32_t maxSendSge;
    uint32_t maxRecvSge;
    uint32_t maxInlineData;
    uint32_t reserved;
};

enum class QpType : uint8_t
{
    Smi = 0,
    Gsi = 1,
    Rc = 2,
    Uc = 3,
    Ud = 4,
    RawIpv6 = 5,
    RawEtherType = 6,
    RawPacket = 8,
    XrcInitiator = 9,
    XrcTarget = 10,
};

enum class QpState : uint32_t
{
    Reset = 0,
    Init = 1,
    ReadyToReceive = 2,
    ReadyToSend = 3,
    SendQueueDrain = 4,
    SendQueueError = 5,
    Error = 6,
};

enum class MigrationState : uint32_t
{
    Migrated = 0,
    Rearm = 1,
    Armed = 2,
};

inline constexpr uint32_t QpAttrState = uint32_t{1} << 0;
inline constexpr uint32_t QpAttrCurrentState = uint32_t{1} << 1;
inline constexpr uint32_t QpAttrEnableSqdAsyncNotify = uint32_t{1} << 2;
inline constexpr uint32_t QpAttrAccessFlags = uint32_t{1} << 3;
inline constexpr uint32_t QpAttrPkeyIndex = uint32_t{1} << 4;
inline constexpr uint32_t QpAttrPort = uint32_t{1} << 5;
inline constexpr uint32_t QpAttrQkey = uint32_t{1} << 6;
inline constexpr uint32_t QpAttrAddressVector = uint32_t{1} << 7;
inline constexpr uint32_t QpAttrPathMtu = uint32_t{1} << 8;
inline constexpr uint32_t QpAttrTimeout = uint32_t{1} << 9;
inline constexpr uint32_t QpAttrRetryCount = uint32_t{1} << 10;
inline constexpr uint32_t QpAttrRnrRetry = uint32_t{1} << 11;
inline constexpr uint32_t QpAttrReceivePsn = uint32_t{1} << 12;
inline constexpr uint32_t QpAttrMaxQpReadAtomic = uint32_t{1} << 13;
inline constexpr uint32_t QpAttrAlternatePath = uint32_t{1} << 14;
inline constexpr uint32_t QpAttrMinRnrTimer = uint32_t{1} << 15;
inline constexpr uint32_t QpAttrSendPsn = uint32_t{1} << 16;
inline constexpr uint32_t QpAttrMaxDestReadAtomic = uint32_t{1} << 17;
inline constexpr uint32_t QpAttrPathMigrationState = uint32_t{1} << 18;
inline constexpr uint32_t QpAttrCapabilities = uint32_t{1} << 19;
inline constexpr uint32_t QpAttrDestinationQpn = uint32_t{1} << 20;

inline constexpr uint32_t AccessLocalWrite = uint32_t{1} << 0;
inline constexpr uint32_t AccessRemoteWrite = uint32_t{1} << 1;
inline constexpr uint32_t AccessRemoteRead = uint32_t{1} << 2;
inline constexpr uint32_t AccessRemoteAtomic = uint32_t{1} << 3;
inline constexpr uint32_t AccessMemoryWindowBind = uint32_t{1} << 4;
inline constexpr uint32_t AccessZeroBased = uint32_t{1} << 5;
inline constexpr uint32_t AccessOnDemand = uint32_t{1} << 6;

struct QpAttr
{
    QpState qpState;
    QpState currentQpState;
    Mtu pathMtu;
    MigrationState pathMigrationState;
    uint32_t qkey;
    uint32_t receivePsn;
    uint32_t sendPsn;
    uint32_t destinationQpNumber;
    uint32_t qpAccessFlags;
    uint16_t pkeyIndex;
    uint16_t alternatePkeyIndex;
    uint8_t enableSqdAsyncNotify;
    uint8_t sendQueueDraining;
    uint8_t maxReadAtomic;
    uint8_t maxDestinationReadAtomic;
    uint8_t minRnrTimer;
    uint8_t portNumber;
    uint8_t timeout;
    uint8_t retryCount;
    uint8_t rnrRetry;
    uint8_t alternatePortNumber;
    uint8_t alternateTimeout;
    uint8_t reserved[5];
    QpCap capabilities;
    AddressHandleAttr addressHandle;
    AddressHandleAttr alternateAddressHandle;
};

enum class Command : uint32_t
{
    QueryPort = 0,
    QueryPkey = 1,
    CreatePd = 2,
    DestroyPd = 3,
    CreateMr = 4,
    DestroyMr = 5,
    CreateCq = 6,
    ResizeCq = 7,
    DestroyCq = 8,
    CreateQp = 9,
    ModifyQp = 10,
    QueryQp = 11,
    DestroyQp = 12,
    CreateUc = 13,
    DestroyUc = 14,
    CreateBind = 15,
    DestroyBind = 16,
    CreateSrq = 17,
    ModifySrq = 18,
    QuerySrq = 19,
    DestroySrq = 20,
};

constexpr uint32_t responseCommand(Command command)
{
    return (uint32_t{1} << 31) | static_cast<uint32_t>(command);
}

struct CommandHeader
{
    uint64_t response;
    uint32_t command;
    uint32_t reserved;
};

struct ResponseHeader
{
    uint64_t response;
    uint32_t acknowledgement;
    uint8_t error;
    uint8_t reserved[3];
};

struct QueryPortCommand
{
    CommandHeader header;
    uint8_t portNumber;
    uint8_t reserved[7];
};

struct QueryPortResponse
{
    ResponseHeader header;
    PortAttr attributes;
};

struct QueryPkeyCommand
{
    CommandHeader header;
    uint8_t portNumber;
    uint8_t index;
    uint8_t reserved[6];
};

struct QueryPkeyResponse
{
    ResponseHeader header;
    uint16_t pkey;
    uint8_t reserved[6];
};

struct CreateUcCommand
{
    CommandHeader header;
    union
    {
        uint32_t pfn;
        uint64_t pfn64;
    };
};

struct CreateUcResponse
{
    ResponseHeader header;
    uint32_t contextHandle;
    uint8_t reserved[4];
};

struct DestroyUcCommand
{
    CommandHeader header;
    uint32_t contextHandle;
    uint8_t reserved[4];
};

struct CreatePdCommand
{
    CommandHeader header;
    uint32_t contextHandle;
    uint8_t reserved[4];
};

struct CreatePdResponse
{
    ResponseHeader header;
    uint32_t pdHandle;
    uint8_t reserved[4];
};

struct DestroyPdCommand
{
    CommandHeader header;
    uint32_t pdHandle;
    uint8_t reserved[4];
};

struct CreateMrCommand
{
    CommandHeader header;
    uint64_t start;
    uint64_t length;
    uint64_t pageDirectoryDma;
    uint32_t pdHandle;
    uint32_t accessFlags;
    uint32_t flags;
    uint32_t numChunks;
};

struct CreateMrResponse
{
    ResponseHeader header;
    uint32_t mrHandle;
    uint32_t lkey;
    uint32_t rkey;
    uint8_t reserved[4];
};

struct DestroyMrCommand
{
    CommandHeader header;
    uint32_t mrHandle;
    uint8_t reserved[4];
};

struct CreateCqCommand
{
    CommandHeader header;
    uint64_t pageDirectoryDma;
    uint32_t contextHandle;
    uint32_t cqe;
    uint32_t numChunks;
    uint8_t reserved[4];
};

struct CreateCqResponse
{
    ResponseHeader header;
    uint32_t cqHandle;
    uint32_t cqe;
};

struct DestroyCqCommand
{
    CommandHeader header;
    uint32_t cqHandle;
    uint8_t reserved[4];
};

struct CreateQpCommand
{
    CommandHeader header;
    uint64_t pageDirectoryDma;
    uint32_t pdHandle;
    uint32_t sendCqHandle;
    uint32_t recvCqHandle;
    uint32_t srqHandle;
    uint32_t maxSendWr;
    uint32_t maxRecvWr;
    uint32_t maxSendSge;
    uint32_t maxRecvSge;
    uint32_t maxInlineData;
    uint32_t lkey;
    uint32_t accessFlags;
    uint16_t totalChunks;
    uint16_t sendChunks;
    uint16_t maxAtomicArgument;
    uint8_t signalAllSendWr;
    uint8_t qpType;
    uint8_t isSrq;
    uint8_t reserved[3];
};

struct CreateQpResponse
{
    ResponseHeader header;
    uint32_t qpn;
    uint32_t maxSendWr;
    uint32_t maxRecvWr;
    uint32_t maxSendSge;
    uint32_t maxRecvSge;
    uint32_t maxInlineData;
};

struct CreateQpResponseV2
{
    ResponseHeader header;
    uint32_t qpn;
    uint32_t qpHandle;
    uint32_t maxSendWr;
    uint32_t maxRecvWr;
    uint32_t maxSendSge;
    uint32_t maxRecvSge;
    uint32_t maxInlineData;
};

struct ModifyQpCommand
{
    CommandHeader header;
    uint32_t qpHandle;
    uint32_t attributeMask;
    QpAttr attributes;
};

struct QueryQpCommand
{
    CommandHeader header;
    uint32_t qpHandle;
    uint32_t attributeMask;
};

struct QueryQpResponse
{
    ResponseHeader header;
    QpAttr attributes;
};

struct DestroyQpCommand
{
    CommandHeader header;
    uint32_t qpHandle;
    uint8_t reserved[4];
};

struct DestroyQpResponse
{
    ResponseHeader header;
    uint32_t eventsReported;
    uint8_t reserved[4];
};

/* Required by Linux RoCE GID add/remove callbacks. */
struct CreateBindCommand
{
    CommandHeader header;
    uint32_t mtu;
    uint32_t vlan;
    uint32_t index;
    uint8_t newGid[16];
    uint8_t gidType;
    uint8_t reserved[3];
};

struct DestroyBindCommand
{
    CommandHeader header;
    uint32_t index;
    uint8_t destinationGid[16];
    uint8_t reserved[4];
};

union CommandRequest
{
    CommandHeader header;
    QueryPortCommand queryPort;
    QueryPkeyCommand queryPkey;
    CreateUcCommand createUc;
    DestroyUcCommand destroyUc;
    CreatePdCommand createPd;
    DestroyPdCommand destroyPd;
    CreateMrCommand createMr;
    DestroyMrCommand destroyMr;
    CreateCqCommand createCq;
    DestroyCqCommand destroyCq;
    CreateQpCommand createQp;
    ModifyQpCommand modifyQp;
    QueryQpCommand queryQp;
    DestroyQpCommand destroyQp;
    CreateBindCommand createBind;
    DestroyBindCommand destroyBind;
};

union CommandResponse
{
    ResponseHeader header;
    QueryPortResponse queryPort;
    QueryPkeyResponse queryPkey;
    CreateUcResponse createUc;
    CreatePdResponse createPd;
    CreateMrResponse createMr;
    CreateCqResponse createCq;
    CreateQpResponse createQp;
    CreateQpResponseV2 createQpV2;
    QueryQpResponse queryQp;
    DestroyQpResponse destroyQp;
};

enum class WorkRequestOpcode : uint32_t
{
    RdmaWrite = 0,
    RdmaWriteWithImmediate = 1,
    Send = 2,
    SendWithImmediate = 3,
    RdmaRead = 4,
    AtomicCompareSwap = 5,
    AtomicFetchAdd = 6,
    Lso = 7,
    SendWithInvalidate = 8,
    RdmaReadWithInvalidate = 9,
    LocalInvalidate = 10,
    FastRegisterMr = 11,
    MaskedAtomicCompareSwap = 12,
    MaskedAtomicFetchAdd = 13,
    BindMemoryWindow = 14,
    RegisterSignatureMr = 15,
    Error = 16,
};

inline constexpr uint32_t SendFence = uint32_t{1} << 0;
inline constexpr uint32_t SendSignaled = uint32_t{1} << 1;
inline constexpr uint32_t SendSolicited = uint32_t{1} << 2;
inline constexpr uint32_t SendInline = uint32_t{1} << 3;
inline constexpr uint32_t SendIpChecksum = uint32_t{1} << 4;

enum class CompletionStatus : uint32_t
{
    Success = 0,
    LocalLengthError = 1,
    LocalQpOperationError = 2,
    LocalEecOperationError = 3,
    LocalProtectionError = 4,
    WorkRequestFlushError = 5,
    MemoryWindowBindError = 6,
    BadResponseError = 7,
    LocalAccessError = 8,
    RemoteInvalidRequestError = 9,
    RemoteAccessError = 10,
    RemoteOperationError = 11,
    RetryExceededError = 12,
    RnrRetryExceededError = 13,
    GeneralError = 21,
};

enum class CompletionOpcode : uint32_t
{
    Send = 0,
    RdmaWrite = 1,
    RdmaRead = 2,
    CompareSwap = 3,
    FetchAdd = 4,
    Receive = uint32_t{1} << 7,
    ReceiveRdmaWithImmediate = (uint32_t{1} << 7) + 1,
};

inline constexpr uint32_t CompletionWithGrh = uint32_t{1} << 0;
inline constexpr uint32_t CompletionWithImmediate = uint32_t{1} << 1;
inline constexpr uint32_t CompletionWithInvalidate = uint32_t{1} << 2;
inline constexpr uint32_t CompletionIpChecksumOk = uint32_t{1} << 3;

struct Sge
{
    uint64_t address;
    uint32_t length;
    uint32_t lkey;
};

struct ReceiveWqeHeader
{
    uint64_t workRequestId;
    uint32_t numSge;
    uint32_t reservedLength;
};

struct SendWqeHeader
{
    uint64_t workRequestId;
    uint32_t numSge;
    uint32_t reservedLength;
    uint32_t opcode;
    uint32_t sendFlags;
    union
    {
        uint32_t beImmediateData;
        uint32_t invalidateRkey;
    };
    uint32_t reserved;
    /* SEND/RECV does not interpret the operation-specific union. */
    uint8_t operationData[48];
};

struct CompletionQueueElement
{
    uint64_t workRequestId;
    uint64_t qp;
    uint32_t opcode;
    uint32_t status;
    uint32_t byteLength;
    uint32_t beImmediateData;
    uint32_t sourceQp;
    uint32_t flags;
    uint32_t vendorError;
    uint16_t pkeyIndex;
    uint16_t sourceLid;
    uint8_t serviceLevel;
    uint8_t destinationLidPathBits;
    uint8_t portNumber;
    uint8_t sourceMac[6];
    uint8_t networkHeaderType;
    uint8_t reserved[6];
};

static_assert(std::is_standard_layout_v<DeviceSharedRegion>);
static_assert(std::is_trivially_copyable_v<DeviceSharedRegion>);
static_assert(sizeof(GuestOsInfo) == 8);
static_assert(sizeof(DeviceCaps) == 208);
static_assert(offsetof(DeviceCaps, mode) == 200);
static_assert(sizeof(RingPageInfo) == 16);
static_assert(sizeof(DeviceSharedRegion) == 280);
static_assert(offsetof(DeviceSharedRegion, caps) == 72);
static_assert(sizeof(Ring) == 8);
static_assert(sizeof(RingState) == 16);
static_assert(sizeof(PortAttr) == 48);
static_assert(sizeof(QpAttr) == 160);
static_assert(offsetof(QpAttr, capabilities) == 56);
static_assert(sizeof(CommandHeader) == 16);
static_assert(sizeof(ResponseHeader) == 16);
static_assert(sizeof(CommandRequest) == sizeof(ModifyQpCommand));
static_assert(sizeof(CommandRequest) == 184);
static_assert(sizeof(CommandResponse) == 176);
static_assert(sizeof(Sge) == 16);
static_assert(sizeof(ReceiveWqeHeader) == 16);
static_assert(sizeof(SendWqeHeader) == 80);
static_assert(offsetof(SendWqeHeader, operationData) == 32);
static_assert(sizeof(CompletionQueueElement) == 64);
static_assert(offsetof(CompletionQueueElement, sourceMac) == 51);

} // namespace pvrdma
} // namespace gem5

#endif // __DEV_RDMA_PVRDMA_ABI_HH__
