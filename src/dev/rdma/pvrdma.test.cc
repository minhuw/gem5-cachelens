// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <tuple>
#include <utility>

#include "dev/rdma/pvrdma.hh"
#include "sim/byteswap.hh"

namespace gem5
{
namespace pvrdma
{

namespace
{

CommandRequest
mrRequest(uint64_t start, uint64_t length, uint32_t chunks,
          uint32_t access = AccessLocalWrite)
{
    CommandRequest request{};
    request.header.response = htole(uint64_t{0x123456789abcdef0});
    request.header.command = htole(
        static_cast<uint32_t>(Command::CreateMr));
    request.createMr.start = htole(start);
    request.createMr.length = htole(length);
    request.createMr.pageDirectoryDma = htole(uint64_t{0x1000});
    request.createMr.pdHandle = htole(uint32_t{1});
    request.createMr.accessFlags = htole(access);
    request.createMr.numChunks = htole(chunks);
    return request;
}

bool
buildMr(const CommandRequest &request, ObjectTables &objects,
        MemoryRegionTable &mrs, MemoryRegionBuild &build,
        uint64_t first_page = 0x4000)
{
    if (!prepareCreateMr(request, objects, mrs, build))
        return false;
    std::array<uint64_t, MrEntriesPerPage> directory{};
    std::vector<uint64_t> tables;
    const uint32_t table_count =
        (build.numChunks + MrEntriesPerPage - 1) / MrEntriesPerPage;
    for (uint32_t i = 0; i < table_count; ++i)
        directory[i] = htole(uint64_t{0x2000} + i * MrPageSize);
    if (!consumeMrDirectory(build, directory, tables))
        return false;
    for (uint32_t table_index = 0; table_index < table_count; ++table_index) {
        std::array<uint64_t, MrEntriesPerPage> table{};
        const uint32_t count = std::min<uint32_t>(
            MrEntriesPerPage,
            build.numChunks - table_index * MrEntriesPerPage);
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t page = first_page +
                uint64_t{table_index * MrEntriesPerPage + i} * MrPageSize;
            table[i] = htole(page);
        }
        if (!consumeMrTable(build, table_index, table))
            return false;
    }
    return true;
}

CommandRequest
cqRequest(uint32_t cqe, uint32_t context = 1)
{
    CommandRequest request{};
    request.header.response = htole(uint64_t{0xc0c0c0c0c0c0c0c0});
    request.header.command = htole(
        static_cast<uint32_t>(Command::CreateCq));
    request.createCq.pageDirectoryDma = htole(uint64_t{0x1000});
    request.createCq.contextHandle = htole(context);
    request.createCq.cqe = htole(cqe);
    request.createCq.numChunks = htole(1 + detail::chunksFor(cqe, CqeSize));
    return request;
}

CommandRequest
qpRequest(uint32_t send_wr, uint32_t recv_wr, uint32_t send_cq = 1,
          uint32_t recv_cq = 1)
{
    CommandRequest request{};
    request.header.response = htole(uint64_t{0x5151515151515151});
    request.header.command = htole(
        static_cast<uint32_t>(Command::CreateQp));
    request.createQp.pageDirectoryDma = htole(uint64_t{0x1000});
    request.createQp.pdHandle = htole(uint32_t{1});
    request.createQp.sendCqHandle = htole(send_cq);
    request.createQp.recvCqHandle = htole(recv_cq);
    request.createQp.maxSendWr = htole(send_wr);
    request.createQp.maxRecvWr = htole(recv_wr);
    request.createQp.maxSendSge = htole(uint32_t{1});
    request.createQp.maxRecvSge = htole(uint32_t{1});
    request.createQp.accessFlags = htole(AccessLocalWrite);
    request.createQp.sendChunks = htole(static_cast<uint16_t>(
        detail::chunksFor(send_wr, SqStride)));
    request.createQp.totalChunks = htole(static_cast<uint16_t>(
        1 + detail::chunksFor(send_wr, SqStride) +
        detail::chunksFor(recv_wr, RqStride)));
    request.createQp.qpType = static_cast<uint8_t>(QpType::Rc);
    return request;
}

template <class Build>
bool
walkPages(Build &build, uint64_t first_page = 0x4000)
{
    std::array<uint64_t, PageEntries> directory{};
    std::vector<uint64_t> tables;
    const uint32_t table_count =
        (build.numChunks + PageEntries - 1) / PageEntries;
    for (uint32_t i = 0; i < table_count; ++i)
        directory[i] = htole(uint64_t{0x2000} + i * PageSize);
    if (!consumePageDirectory(build, directory, tables))
        return false;
    for (uint32_t table_index = 0; table_index < table_count; ++table_index) {
        std::array<uint64_t, PageEntries> table{};
        const uint32_t count = std::min<uint32_t>(
            PageEntries, build.numChunks - table_index * PageEntries);
        for (uint32_t i = 0; i < count; ++i) {
            table[i] = htole(first_page +
                uint64_t{table_index * PageEntries + i} * PageSize);
        }
        if (!consumePageTable(build, table_index, table))
            return false;
    }
    return true;
}

void
setupUserParent(ObjectTables &objects)
{
    objects.contextUar[1] = 1;
    objects.contextPdChildren[1] = 1;
    objects.pdAllocated[1] = 1;
    objects.pdParent[1] = 1;
}

bool
buildCq(const CommandRequest &request, ObjectTables &objects,
        CompletionQueueTable &cqs, CompletionQueueBuild &build,
        uint64_t first_page = 0x4000)
{
    return prepareCreateCq(request, objects, cqs, build) &&
        walkPages(build, first_page);
}

bool
buildQp(const CommandRequest &request, ObjectTables &objects,
        CompletionQueueTable &cqs, QueuePairTable &qps,
        QueuePairBuild &build, uint64_t first_page = 0x8000)
{
    return prepareCreateQp(request, objects, cqs, qps, build) &&
        walkPages(build, first_page);
}

QueuePair
testQueuePair(uint32_t send_wr = 64, uint32_t recv_wr = 256)
{
    QueuePair qp;
    qp.valid = true;
    qp.generation = 2;
    qp.qpHandle = 1;
    qp.qpn = (qp.generation << SlotBits) | qp.qpHandle;
    qp.pdHandle = 1;
    qp.capabilities.maxSendWr = send_wr;
    qp.capabilities.maxRecvWr = recv_wr;
    qp.capabilities.maxSendSge = 1;
    qp.capabilities.maxRecvSge = 1;
    qp.sendChunks = detail::chunksFor(send_wr, SqStride);
    qp.recvChunks = detail::chunksFor(recv_wr, RqStride);
    qp.totalChunks = 1 + qp.sendChunks + qp.recvChunks;
    for (uint32_t i = 0; i < qp.totalChunks; ++i)
        qp.pages.push_back(0x8000 + uint64_t{i} * PageSize);
    return qp;
}

std::array<uint8_t, SqStride>
sqSlot(const SendWqeHeader &header, const Sge &sge)
{
    std::array<uint8_t, SqStride> slot{};
    std::memcpy(slot.data(), &header, sizeof(header));
    std::memcpy(slot.data() + sizeof(header), &sge, sizeof(sge));
    return slot;
}

std::array<uint8_t, RqStride>
rqSlot(const ReceiveWqeHeader &header, const Sge &sge)
{
    std::array<uint8_t, RqStride> slot{};
    std::memcpy(slot.data(), &header, sizeof(header));
    std::memcpy(slot.data() + sizeof(header), &sge, sizeof(sge));
    return slot;
}

CommandResult
modify(QueuePairTable &qps, QpState state, uint32_t mask,
       const QpAttr &attributes, CommandResponse &response)
{
    CommandRequest request{};
    request.header.command = htole(
        static_cast<uint32_t>(Command::ModifyQp));
    request.modifyQp.qpHandle = htole(uint32_t{1});
    request.modifyQp.attributeMask = htole(mask);
    request.modifyQp.attributes = attributes;
    request.modifyQp.attributes.qpState = static_cast<QpState>(
        htole(static_cast<uint32_t>(state)));
    return detail::modifyQp(request, response, qps);
}

} // anonymous namespace

TEST(PvrdmaRegisterTest, DecodesOnlyDefinedAlignedRegisters)
{
    EXPECT_EQ(decodeRegister(RegVersion), Register::Version);
    EXPECT_EQ(decodeRegister(RegDsrLow), Register::DsrLow);
    EXPECT_EQ(decodeRegister(RegDsrHigh), Register::DsrHigh);
    EXPECT_EQ(decodeRegister(RegControl), Register::Control);
    EXPECT_EQ(decodeRegister(RegRequest), Register::Request);
    EXPECT_EQ(decodeRegister(RegError), Register::Error);
    EXPECT_EQ(decodeRegister(RegInterruptCause), Register::InterruptCause);
    EXPECT_EQ(decodeRegister(RegInterruptMask), Register::InterruptMask);
    EXPECT_EQ(decodeRegister(RegMacLow), Register::MacLow);
    EXPECT_EQ(decodeRegister(RegMacHigh), Register::MacHigh);
    EXPECT_EQ(decodeRegister(0x28), Register::Invalid);
    EXPECT_EQ(decodeRegister(RegVersion + 1), Register::Invalid);
}

TEST(PvrdmaRegisterTest, RequiresExactWidthAndAlignment)
{
    EXPECT_TRUE(validRegisterAccess(RegVersion, sizeof(uint32_t)));
    EXPECT_TRUE(validRegisterAccess(RegMacHigh, sizeof(uint32_t)));
    EXPECT_FALSE(validRegisterAccess(RegVersion, sizeof(uint8_t)));
    EXPECT_FALSE(validRegisterAccess(RegVersion, sizeof(uint16_t)));
    EXPECT_FALSE(validRegisterAccess(RegVersion, sizeof(uint64_t)));
    EXPECT_FALSE(validRegisterAccess(RegDsrLow + 1, sizeof(uint32_t)));
    EXPECT_FALSE(validRegisterAccess(0x28, sizeof(uint32_t)));
}

TEST(PvrdmaRegisterTest, EnforcesRegisterDirections)
{
    EXPECT_TRUE(registerReadable(Register::Version));
    EXPECT_TRUE(registerReadable(Register::Error));
    EXPECT_TRUE(registerReadable(Register::InterruptCause));
    EXPECT_TRUE(registerReadable(Register::InterruptMask));
    EXPECT_TRUE(registerReadable(Register::MacLow));
    EXPECT_TRUE(registerReadable(Register::MacHigh));
    EXPECT_FALSE(registerReadable(Register::DsrLow));
    EXPECT_FALSE(registerReadable(Register::Control));
    EXPECT_FALSE(registerReadable(Register::Request));

    EXPECT_TRUE(registerWritable(Register::DsrLow));
    EXPECT_TRUE(registerWritable(Register::DsrHigh));
    EXPECT_TRUE(registerWritable(Register::Control));
    EXPECT_TRUE(registerWritable(Register::Request));
    EXPECT_TRUE(registerWritable(Register::InterruptMask));
    EXPECT_TRUE(registerWritable(Register::MacLow));
    EXPECT_TRUE(registerWritable(Register::MacHigh));
    EXPECT_FALSE(registerWritable(Register::Version));
    EXPECT_FALSE(registerWritable(Register::Error));
    EXPECT_FALSE(registerWritable(Register::InterruptCause));
}

TEST(PvrdmaRegisterTest, AssemblesDsrInLowHighOrder)
{
    RegisterState regs;

    EXPECT_FALSE(regs.writeDsrHigh(0x11223344));
    regs.writeDsrLow(0x89abcdef);
    EXPECT_TRUE(regs.dsrLowPending);
    EXPECT_TRUE(regs.writeDsrHigh(0x01234567));
    EXPECT_EQ(regs.dsrAddress, 0x0123456789abcdefULL);
    EXPECT_FALSE(regs.dsrLowPending);

    regs.writeDsrLow(0x76543210);
    EXPECT_TRUE(regs.writeDsrHigh(0xfedcba98));
    EXPECT_EQ(regs.dsrAddress, 0xfedcba9876543210ULL);
}

TEST(PvrdmaRegisterTest, MasksAndAcknowledgesInterrupts)
{
    uint32_t pending = InterruptCauseResponse | InterruptCauseCompletion;

    EXPECT_FALSE(interruptPending(pending, InitialInterruptMask));
    EXPECT_TRUE(interruptPending(pending, 0));
    EXPECT_EQ(unmaskedInterrupts(pending, InterruptCauseResponse),
              InterruptCauseCompletion);
    EXPECT_EQ(acknowledgeInterrupts(pending),
              InterruptCauseResponse | InterruptCauseCompletion);
    EXPECT_EQ(pending, 0);
}

TEST(PvrdmaRegisterTest, ResetRestoresMutableConstructorState)
{
    RegisterState regs(0x44332211, 0x6655);
    regs.writeDsrLow(0x89abcdef);
    ASSERT_TRUE(regs.writeDsrHigh(0x01234567));
    regs.control = static_cast<uint32_t>(DeviceControl::Unquiesce);
    regs.request = 0x1234;
    regs.error = 0;
    regs.pendingCauses = InterruptCauseAsync;
    regs.interruptMask = 0;
    regs.writeMacHigh(0x12346655);
    EXPECT_EQ(regs.macHigh, 0x6655);

    regs.reset();

    EXPECT_EQ(regs.dsrAddress, 0);
    EXPECT_EQ(regs.control, 0);
    EXPECT_EQ(regs.request, 0);
    EXPECT_EQ(regs.error, UnsupportedError);
    EXPECT_EQ(regs.pendingCauses, 0);
    EXPECT_EQ(regs.interruptMask, InitialInterruptMask);
    EXPECT_FALSE(regs.dsrLowPending);
    EXPECT_EQ(regs.macLow, 0x44332211);
    EXPECT_EQ(regs.macHigh, 0x6655);
}

TEST(PvrdmaRegisterTest, DerivesCompanionTransportMac)
{
    const RegisterState regs(0x00009002, 0x0100);
    const rocev2::MacAddress configured = {0x02, 0x90, 0, 0, 0, 1};
    const rocev2::MacAddress companion = {0x90, 0x02, 0, 0, 1, 0};

    EXPECT_EQ(transportMac(regs, false), configured);
    EXPECT_EQ(transportMac(regs, true), companion);
}

TEST(PvrdmaControlTest, PublishesFrozenCapabilitiesAndGuid)
{
    const auto caps = makeCapabilities(0x33221100, 0x5544);

    EXPECT_EQ(letoh(caps.fwVersion), 1);
    EXPECT_EQ(betoh(caps.beNodeGuid), 0x021122fffe334455ULL);
    EXPECT_EQ(caps.beNodeGuid, caps.beSystemImageGuid);
    EXPECT_EQ(letoh(caps.maxMrSize), UINT64_MAX);
    EXPECT_EQ(letoh(caps.pageSizeCap), 4096);
    EXPECT_EQ(letoh(caps.vendorId), 0x15ad);
    EXPECT_EQ(letoh(caps.vendorPartId), 0x0820);
    EXPECT_EQ(letoh(caps.hardwareVersion), 1);
    EXPECT_EQ(letoh(caps.maxQp), 64);
    EXPECT_EQ(letoh(caps.maxQpWr), 256);
    EXPECT_EQ(letoh(caps.maxSge), 1);
    EXPECT_EQ(letoh(caps.maxSgeRd), 0);
    EXPECT_EQ(letoh(caps.maxCq), 64);
    EXPECT_EQ(letoh(caps.maxCqe), 1024);
    EXPECT_EQ(letoh(caps.maxMr), 64);
    EXPECT_EQ(letoh(caps.maxPd), 64);
    EXPECT_EQ(letoh(caps.maxQpRdAtom), 1);
    EXPECT_EQ(letoh(caps.maxQpInitRdAtom), 1);
    EXPECT_EQ(letoh(caps.maxUar), 64);
    EXPECT_EQ(letoh(caps.gidTableLength), 8);
    EXPECT_EQ(letoh(caps.maxPkeys), 1);
    EXPECT_EQ(caps.physicalPortCount, 1);
    EXPECT_EQ(caps.mode, static_cast<uint8_t>(DeviceMode::Roce));
    EXPECT_EQ(caps.gidTypes, GidTypeRoceV2);
    EXPECT_EQ(caps.atomicOps, 0);
    EXPECT_EQ(caps.bmmeFlags, 0);
    EXPECT_EQ(letoh(caps.deviceCapFlags), 0);
    EXPECT_EQ(letoh(caps.maxAh), 0);
    EXPECT_EQ(letoh(caps.maxSrq), 0);
    EXPECT_EQ(letoh(caps.maxMw), 0);
    EXPECT_EQ(letoh(caps.maxFmr), 0);
}

TEST(PvrdmaControlTest, ValidatesOnlyUsableSharedRegions)
{
    DeviceSharedRegion dsr{};
    dsr.driverVersion = htole(Version);
    dsr.commandSlotDma = htole(uint64_t{0x2000});
    dsr.responseSlotDma = htole(uint64_t{0x3000});
    dsr.asyncRingPages.numPages = htole(NumRingPages);
    dsr.asyncRingPages.pageDirectoryDma = htole(uint64_t{0x4000});
    dsr.completionRingPages.numPages = htole(NumRingPages);
    dsr.completionRingPages.pageDirectoryDma = htole(uint64_t{0x5000});

    EXPECT_TRUE(validSharedRegion(dsr, 0x1000));
    dsr.driverVersion = htole(Version - 1);
    EXPECT_FALSE(validSharedRegion(dsr, 0x1000));
    dsr.driverVersion = htole(Version);
    EXPECT_FALSE(validSharedRegion(dsr, 0));
    EXPECT_FALSE(validSharedRegion(dsr, 0x1001));
    dsr.commandSlotDma = 0;
    EXPECT_FALSE(validSharedRegion(dsr, 0x1000));
    dsr.commandSlotDma = htole(uint64_t{0x2001});
    EXPECT_FALSE(validSharedRegion(dsr, 0x1000));
    dsr.commandSlotDma = htole(uint64_t{0x2000});
    dsr.asyncRingPages.numPages = 0;
    EXPECT_FALSE(validSharedRegion(dsr, 0x1000));
}

TEST(PvrdmaControlTest, EnforcesBoundedStateTransitions)
{
    ControlState state = ControlState::Unconfigured;

    EXPECT_TRUE(beginDsr(state));
    EXPECT_EQ(state, ControlState::ReadingDsr);
    EXPECT_FALSE(beginDsr(state));
    EXPECT_FALSE(applyControl(state, DeviceControl::Reset));
    EXPECT_TRUE(finishDsrRead(state, true));
    EXPECT_EQ(state, ControlState::WritingCaps);
    EXPECT_TRUE(finishCapsWrite(state));
    EXPECT_EQ(state, ControlState::Ready);
    EXPECT_FALSE(beginCommand(state));
    EXPECT_TRUE(applyControl(state, DeviceControl::Activate));
    EXPECT_TRUE(applyControl(state, DeviceControl::Unquiesce));
    EXPECT_TRUE(beginCommand(state));
    EXPECT_FALSE(beginCommand(state));
    EXPECT_FALSE(applyControl(state, DeviceControl::Reset));
    EXPECT_TRUE(finishCommandRead(state, false));
    EXPECT_EQ(state, ControlState::Active);
    EXPECT_TRUE(applyControl(state, DeviceControl::Reset));
    EXPECT_EQ(state, ControlState::Unconfigured);
    EXPECT_FALSE(applyControl(state, DeviceControl::Activate));
}

TEST(PvrdmaCompletionTest, EncodesOnlySupportedSendAndReceiveCqes)
{
    CompletionRecord record;
    record.cqHandle = 3;
    record.qpHandle = 7;
    record.workRequestId = 0x1122334455667788;

    for (const auto opcode : {CompletionOpcode::Send,
                              CompletionOpcode::Receive}) {
        for (const auto status : {CompletionStatus::Success,
                                  CompletionStatus::GeneralError}) {
            record.opcode = opcode;
            record.status = status;
            record.byteLength = opcode == CompletionOpcode::Receive ?
                0xaabbccdd : 0;
            record.sourceQp = opcode == CompletionOpcode::Receive ?
                0x123456 : 0;
            ASSERT_TRUE(validCompletionRecord(record));
            const auto cqe = encodeCompletion(record);
            EXPECT_EQ(letoh(cqe.workRequestId), record.workRequestId);
            EXPECT_EQ(letoh(cqe.qp), 7);
            EXPECT_EQ(letoh(cqe.opcode),
                      opcode == CompletionOpcode::Send ? 0 : 128);
            EXPECT_EQ(letoh(cqe.status),
                      status == CompletionStatus::Success ? 0 : 21);
            EXPECT_EQ(letoh(cqe.byteLength), record.byteLength);
            EXPECT_EQ(letoh(cqe.sourceQp), record.sourceQp);
            EXPECT_EQ(cqe.beImmediateData, 0);
            EXPECT_EQ(cqe.flags, 0);
            EXPECT_EQ(cqe.vendorError, 0);
            EXPECT_TRUE(std::all_of(
                reinterpret_cast<const uint8_t *>(&cqe) + 36,
                reinterpret_cast<const uint8_t *>(&cqe) + sizeof(cqe),
                [](uint8_t byte) { return byte == 0; }));
        }
    }

    record.opcode = CompletionOpcode::RdmaWrite;
    EXPECT_FALSE(validCompletionRecord(record));
    record.opcode = CompletionOpcode::Send;
    record.byteLength = record.sourceQp = 0;
    for (const auto status : {
             CompletionStatus::LocalLengthError,
             CompletionStatus::LocalQpOperationError,
             CompletionStatus::LocalProtectionError,
             CompletionStatus::LocalAccessError,
             CompletionStatus::RemoteInvalidRequestError,
             CompletionStatus::RemoteAccessError,
             CompletionStatus::RemoteOperationError,
             CompletionStatus::RetryExceededError,
             CompletionStatus::RnrRetryExceededError,
             CompletionStatus::GeneralError}) {
        record.status = status;
        EXPECT_TRUE(validCompletionRecord(record));
    }
    record.opcode = CompletionOpcode::Receive;
    record.status = CompletionStatus::RetryExceededError;
    EXPECT_FALSE(validCompletionRecord(record));
    record.status = CompletionStatus::Success;
    record.opcode = CompletionOpcode::Send;
    record.byteLength = 1;
    EXPECT_FALSE(validCompletionRecord(record));
}

TEST(PvrdmaCommandTest, BuildsResponseHeaderAndRejectsUnsupportedCommands)
{
    CommandRequest request{};
    CommandResponse response{};
    GidTable gids{};
    GidValidTable valid{};
    ObjectTables objects{};
    UarRange range{0x100000, UarBarSize};
    request.header.response = htole(uint64_t{0x123456789abcdef0});
    request.header.command = htole(
        static_cast<uint32_t>(Command::CreateMr));

    const auto result = processCommand(request, response, gids, valid,
                                       objects, range);

    EXPECT_TRUE(result.hasResponse);
    EXPECT_EQ(result.error, CommandError);
    EXPECT_EQ(response.header.response, request.header.response);
    EXPECT_EQ(letoh(response.header.acknowledgement),
              responseCommand(Command::CreateMr));
    EXPECT_EQ(response.header.error, CommandError);

    request.header.command = htole(uint32_t{99});
    processCommand(request, response, gids, valid, objects, range);
    EXPECT_EQ(response.header.response, request.header.response);
    EXPECT_EQ(response.header.acknowledgement, 0);
    EXPECT_EQ(response.header.error, CommandError);
}

TEST(PvrdmaCommandTest, ValidatesQueryPortAndPkey)
{
    CommandRequest request{};
    CommandResponse response{};
    GidTable gids{};
    GidValidTable valid{};
    ObjectTables objects{};
    UarRange range{0x100000, UarBarSize};

    request.header.command = htole(
        static_cast<uint32_t>(Command::QueryPort));
    request.queryPort.portNumber = 1;
    auto result = processCommand(request, response, gids, valid, objects,
                                 range);
    EXPECT_TRUE(result.hasResponse);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(letoh(response.header.acknowledgement),
              responseCommand(Command::QueryPort));
    EXPECT_EQ(letoh(static_cast<uint32_t>(
                  response.queryPort.attributes.state)),
              static_cast<uint32_t>(PortState::Active));
    EXPECT_EQ(letoh(static_cast<uint32_t>(
                  response.queryPort.attributes.maxMtu)),
              static_cast<uint32_t>(Mtu::Mtu1024));
    EXPECT_EQ(letoh(response.queryPort.attributes.gidTableLength), 8);
    EXPECT_EQ(letoh(response.queryPort.attributes.pkeyTableLength), 1);
    EXPECT_EQ(letoh(response.queryPort.attributes.maxMessageSize),
              MaxMessageSize);
    EXPECT_EQ(letoh(response.queryPort.attributes.portCapFlags), 0);
    EXPECT_EQ(letoh(response.queryPort.attributes.badPkeyCounter), 0);
    EXPECT_EQ(letoh(response.queryPort.attributes.qkeyViolationCounter), 0);

    request.queryPort.portNumber = 2;
    result = processCommand(request, response, gids, valid, objects, range);
    EXPECT_EQ(result.error, CommandError);
    EXPECT_EQ(response.header.error, CommandError);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::QueryPkey));
    request.queryPkey.portNumber = 1;
    request.queryPkey.index = 0;
    result = processCommand(request, response, gids, valid, objects, range);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(letoh(response.queryPkey.pkey), FullMembershipPkey);

    request.queryPkey.index = 1;
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    request.queryPkey.index = 0;
    request.queryPkey.portNumber = 2;
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
}

TEST(PvrdmaCommandTest, ValidatesGidBindBoundsMtuTypeAndDestroyMatch)
{
    CommandRequest request{};
    CommandResponse response{};
    GidTable gids{};
    GidValidTable valid{};
    ObjectTables objects{};
    UarRange range{0x100000, UarBarSize};
    const std::array<uint8_t, 16> gid = {
        0xfe, 0x80, 0, 0, 0, 0, 0, 0, 2, 0x11, 0x22, 0xff, 0xfe, 0x33,
        0x44, 0x55,
    };

    request.header.command = htole(
        static_cast<uint32_t>(Command::CreateBind));
    request.createBind.mtu = htole(FixedMtu);
    request.createBind.index = htole(uint32_t{7});
    request.createBind.gidType = GidTypeRoceV2;
    std::copy(gid.begin(), gid.end(), request.createBind.newGid);
    auto result = processCommand(request, response, gids, valid, objects,
                                 range);
    EXPECT_FALSE(result.hasResponse);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(valid[7], 1);
    EXPECT_TRUE(std::equal(gid.begin(), gid.end(), gids[7].raw));

    request.createBind.index = htole(uint32_t{8});
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    request.createBind.index = htole(uint32_t{0});
    request.createBind.mtu = htole(uint32_t{2048});
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    request.createBind.mtu = htole(FixedMtu);
    request.createBind.gidType = uint8_t{1};
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);

    const std::array<uint8_t, 16> mapped = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1,
    };
    request.createBind.index = htole(uint32_t{6});
    request.createBind.gidType = GidTypeRoceV2;
    std::copy(mapped.begin(), mapped.end(), request.createBind.newGid);
    EXPECT_TRUE(detail::ipv4MappedGid(request.createBind.newGid));
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    EXPECT_EQ(valid[6], 0);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyBind));
    request.destroyBind.index = htole(uint32_t{7});
    std::copy(gid.begin(), gid.end(), request.destroyBind.destinationGid);
    request.destroyBind.destinationGid[15] ^= 1;
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    EXPECT_EQ(valid[7], 1);

    request.destroyBind.destinationGid[15] ^= 1;
    result = processCommand(request, response, gids, valid, objects, range);
    EXPECT_FALSE(result.hasResponse);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(valid[7], 0);
    EXPECT_TRUE(std::all_of(std::begin(gids[7].raw), std::end(gids[7].raw),
                            [](uint8_t byte) { return byte == 0; }));
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
}

TEST(PvrdmaObjectTest, ValidatesBarRelativePfn64AndUarOwnership)
{
    CommandRequest request{};
    CommandResponse response{};
    GidTable gids{};
    GidValidTable valid{};
    ObjectTables objects{};
    const UarRange range{uint64_t{1} << 44, UarBarSize};
    request.header.command = htole(
        static_cast<uint32_t>(Command::CreateUc));
    request.header.response = htole(uint64_t{0x123456789abcdef0});

    request.createUc.pfn64 = htole(range.start / UarPageSize);
    auto result = processCommand(request, response, gids, valid, objects,
                                 range);
    EXPECT_TRUE(result.hasResponse);
    EXPECT_EQ(result.error, CommandError);
    EXPECT_EQ(response.header.error, CommandError);

    request.createUc.pfn64 = htole(range.start / UarPageSize + 1);
    result = processCommand(request, response, gids, valid, objects, range);
    ASSERT_EQ(result.error, 0);
    EXPECT_EQ(response.header.response, request.header.response);
    EXPECT_EQ(letoh(response.header.acknowledgement),
              responseCommand(Command::CreateUc));
    EXPECT_EQ(response.header.error, 0);
    EXPECT_EQ(letoh(response.createUc.contextHandle), 1);
    EXPECT_EQ(objects.contextUar[1], 1);

    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    request.createUc.pfn64 = htole(range.start / UarPageSize + 64);
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    request.createUc.pfn64 = htole(
        range.start / UarPageSize + UarBarSize / UarPageSize);
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    request.createUc.pfn64 = htole(UINT64_MAX);
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    request.header.reserved = htole(uint32_t{1});
    request.createUc.pfn64 = htole(range.start / UarPageSize + 2);
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
}

TEST(PvrdmaObjectTest, AllocatesExhaustsAndReusesLowestContextHandle)
{
    CommandRequest request{};
    CommandResponse response{};
    GidTable gids{};
    GidValidTable valid{};
    ObjectTables objects{};
    const UarRange range{0x100000, ObjectTableEntries * UarPageSize};
    request.header.command = htole(
        static_cast<uint32_t>(Command::CreateUc));

    for (uint32_t handle = 1; handle < ObjectTableEntries; ++handle) {
        request.createUc.pfn64 = htole(
            range.start / UarPageSize + handle);
        ASSERT_EQ(processCommand(request, response, gids, valid, objects,
                                 range).error, 0);
        EXPECT_EQ(letoh(response.createUc.contextHandle), handle);
    }
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyUc));
    request.destroyUc.contextHandle = htole(uint32_t{17});
    request.destroyUc.reserved[0] = 1;
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    request.destroyUc.reserved[0] = 0;
    auto result = processCommand(request, response, gids, valid, objects,
                                 range);
    EXPECT_FALSE(result.hasResponse);
    ASSERT_EQ(result.error, 0);
    EXPECT_EQ(objects.contextUar[17], 0);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::CreateUc));
    request.createUc.pfn64 = htole(range.start / UarPageSize + 17);
    ASSERT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, 0);
    EXPECT_EQ(letoh(response.createUc.contextHandle), 17);
}

TEST(PvrdmaObjectTest, EnforcesPdParentsChildrenAndNoResponseDestroy)
{
    CommandRequest request{};
    CommandResponse response{};
    GidTable gids{};
    GidValidTable valid{};
    ObjectTables objects{};
    const UarRange range{0x100000, ObjectTableEntries * UarPageSize};

    request.header.command = htole(
        static_cast<uint32_t>(Command::CreateUc));
    request.createUc.pfn64 = htole(range.start / UarPageSize + 1);
    ASSERT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, 0);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::CreatePd));
    request.header.response = htole(uint64_t{0xfedcba9876543210});
    request.createPd.contextHandle = htole(uint32_t{1});
    auto result = processCommand(request, response, gids, valid, objects,
                                 range);
    ASSERT_TRUE(result.hasResponse);
    ASSERT_EQ(result.error, 0);
    EXPECT_EQ(response.header.response, request.header.response);
    EXPECT_EQ(letoh(response.header.acknowledgement),
              responseCommand(Command::CreatePd));
    EXPECT_EQ(letoh(response.createPd.pdHandle), 1);
    EXPECT_EQ(objects.contextPdChildren[1], 1);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyUc));
    request.destroyUc.contextHandle = htole(uint32_t{1});
    result = processCommand(request, response, gids, valid, objects, range);
    EXPECT_FALSE(result.hasResponse);
    EXPECT_EQ(result.error, CommandError);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyPd));
    request.destroyPd.pdHandle = htole(uint32_t{1});
    request.destroyPd.reserved[0] = 1;
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    request.destroyPd.reserved[0] = 0;
    objects.pdChildren[1] = 1;
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    objects.pdChildren[1] = 0;
    result = processCommand(request, response, gids, valid, objects, range);
    EXPECT_FALSE(result.hasResponse);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(objects.contextPdChildren[1], 0);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::CreatePd));
    request.createPd.contextHandle = htole(uint32_t{63});
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    request.createPd.contextHandle = 0;
    request.createPd.reserved[0] = 1;
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    request.createPd.reserved[0] = 0;
    for (uint32_t handle = 1; handle < ObjectTableEntries; ++handle) {
        ASSERT_EQ(processCommand(request, response, gids, valid, objects,
                                 range).error, 0);
        EXPECT_EQ(letoh(response.createPd.pdHandle), handle);
    }
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);
    EXPECT_EQ(objects.contextPdChildren[1], 0);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyPd));
    request.destroyPd.pdHandle = htole(uint32_t{17});
    ASSERT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, 0);
    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::CreatePd));
    ASSERT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, 0);
    EXPECT_EQ(letoh(response.createPd.pdHandle), 17);
}

TEST(PvrdmaObjectTest, ResetsAndValidatesRestoreInvariants)
{
    ObjectTables objects{};
    const UarRange range{0x100000, ObjectTableEntries * UarPageSize};
    objects.contextUar[1] = 1;
    objects.contextPdChildren[1] = 1;
    objects.pdAllocated[1] = 1;
    objects.pdParent[1] = 1;
    EXPECT_TRUE(validObjectTables(objects, range));

    auto malformed = objects;
    malformed.contextPdChildren[1] = 0;
    EXPECT_FALSE(validObjectTables(malformed, range));
    malformed = objects;
    malformed.pdParent[1] = 2;
    EXPECT_FALSE(validObjectTables(malformed, range));
    malformed = objects;
    malformed.contextUar[2] = 1;
    EXPECT_FALSE(validObjectTables(malformed, range));
    malformed = objects;
    malformed.contextUar[1] = ObjectTableEntries;
    EXPECT_FALSE(validObjectTables(malformed, range));
    malformed = objects;
    malformed.pdChildren[1] = 1;
    EXPECT_TRUE(validObjectTables(malformed, range));
    MemoryRegionTable emptyMrs;
    EXPECT_FALSE(validMemoryRegions(emptyMrs, malformed));

    objects.reset();
    EXPECT_TRUE(validObjectTables(objects, range));
    EXPECT_TRUE(std::all_of(objects.contextUar.begin(),
                            objects.contextUar.end(),
                            [](uint32_t value) { return value == 0; }));
    EXPECT_TRUE(std::all_of(objects.pdAllocated.begin(),
                            objects.pdAllocated.end(),
                            [](uint32_t value) { return value == 0; }));
}

TEST(PvrdmaControlTest, PreservesNewerRejectionAcrossOlderCompletion)
{
    OperationErrorState state;
    uint32_t error = UnsupportedError;

    state.begin(error);
    EXPECT_EQ(error, 0);
    state.set(error, CommandError);
    state.complete(error, 0);
    EXPECT_EQ(error, CommandError);

    state.begin(error);
    state.complete(error, 0);
    EXPECT_EQ(error, 0);
    state.begin(error);
    state.complete(error, CommandError);
    EXPECT_EQ(error, CommandError);
}

TEST(PvrdmaControlTest, PublishesResponseCauseOnlyAfterWriteCompletion)
{
    ControlState state = ControlState::Active;
    uint32_t pending = 0;

    ASSERT_TRUE(beginCommand(state));
    ASSERT_TRUE(beginObjectDirectory(state));
    EXPECT_EQ(state, ControlState::ReadingObjectDirectory);
    EXPECT_EQ(pending, 0);
    ASSERT_TRUE(beginObjectTables(state));
    EXPECT_EQ(state, ControlState::ReadingObjectTable);
    EXPECT_EQ(pending, 0);
    ASSERT_TRUE(finishObjectWalk(state));
    EXPECT_EQ(state, ControlState::WritingResponse);
    EXPECT_EQ(pending, 0);
    EXPECT_FALSE(checkpointStable(state, false));
    EXPECT_TRUE(finishResponseWrite(state, pending));
    EXPECT_EQ(state, ControlState::Active);
    EXPECT_EQ(pending, InterruptCauseResponse);
}

TEST(PvrdmaControlTest, CheckpointsOnlyStableIdleState)
{
    EXPECT_TRUE(checkpointStable(ControlState::Unconfigured, false));
    EXPECT_TRUE(checkpointStable(ControlState::Ready, false));
    EXPECT_TRUE(checkpointStable(ControlState::Active, false));
    EXPECT_FALSE(checkpointStable(ControlState::Active, true));
    EXPECT_FALSE(checkpointStable(ControlState::Active, false, true));
    EXPECT_FALSE(checkpointStable(ControlState::Active, false, false, true));
    EXPECT_FALSE(checkpointStable(ControlState::Active, false, false, false,
                                  false, false, false, true));
    EXPECT_FALSE(checkpointStable(ControlState::ReadingDsr, false));
    EXPECT_FALSE(checkpointStable(ControlState::WritingCaps, false));
    EXPECT_FALSE(checkpointStable(ControlState::ReadingCommand, false));
    EXPECT_FALSE(checkpointStable(ControlState::ReadingObjectDirectory, false));
    EXPECT_FALSE(checkpointStable(ControlState::ReadingObjectTable, false));
    EXPECT_FALSE(checkpointStable(ControlState::WritingResponse, false));
}

TEST(PvrdmaMrTest, ComputesExactLinuxPageGeometry)
{
    uint32_t pages = 0;
    EXPECT_TRUE(detail::mrPageCount(0x1000, 1, pages));
    EXPECT_EQ(pages, 1);
    EXPECT_TRUE(detail::mrPageCount(0x1003, 4093, pages));
    EXPECT_EQ(pages, 1);
    EXPECT_TRUE(detail::mrPageCount(0x1003, 4094, pages));
    EXPECT_EQ(pages, 2);
    EXPECT_TRUE(detail::mrPageCount(0, uint64_t{512} * MrPageSize, pages));
    EXPECT_EQ(pages, 512);
    EXPECT_TRUE(detail::mrPageCount(0, uint64_t{513} * MrPageSize, pages));
    EXPECT_EQ(pages, 513);
    EXPECT_FALSE(detail::mrPageCount(0, 0, pages));
    EXPECT_FALSE(detail::mrPageCount(UINT64_MAX, 1, pages));
    EXPECT_FALSE(detail::mrPageCount(
        0, uint64_t{PageDirectoryMaxPages + 1} * MrPageSize, pages));
}

TEST(PvrdmaMrTest, RejectsMalformedCreateBeforeDma)
{
    ObjectTables objects{};
    objects.pdAllocated[1] = 1;
    MemoryRegionTable mrs{};
    MemoryRegionBuild build{};
    auto request = mrRequest(0x1003, 4094, 2);
    EXPECT_TRUE(prepareCreateMr(request, objects, mrs, build));
    EXPECT_EQ(build.slot, 1);
    EXPECT_EQ(build.mrHandle, 1);

    auto malformed = request;
    malformed.header.reserved = htole(uint32_t{1});
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed = request;
    malformed.createMr.flags = htole(MrFlagDma);
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed = request;
    malformed.createMr.accessFlags = htole(AccessMemoryWindowBind);
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed.createMr.accessFlags = htole(AccessRemoteWrite);
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed.createMr.accessFlags = htole(AccessRemoteAtomic);
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed = request;
    malformed.createMr.pdHandle = htole(uint32_t{2});
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed = request;
    malformed.createMr.pageDirectoryDma = 0;
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed.createMr.pageDirectoryDma = htole(uint64_t{0x1001});
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed = request;
    malformed.createMr.length = 0;
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed = mrRequest(UINT64_MAX, 1, 1);
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed = request;
    malformed.createMr.numChunks = htole(uint32_t{0});
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed.createMr.numChunks = htole(uint32_t{1});
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
    malformed.createMr.numChunks = htole(PageDirectoryMaxPages + 1);
    EXPECT_FALSE(prepareCreateMr(malformed, objects, mrs, build));
}

TEST(PvrdmaMrTest, WalksOne512And513PageDirectories)
{
    ObjectTables objects{};
    objects.pdAllocated[1] = 1;
    for (const uint32_t count : {1U, 512U, 513U}) {
        MemoryRegionTable mrs{};
        MemoryRegionBuild build{};
        ASSERT_TRUE(buildMr(mrRequest(0, uint64_t{count} * MrPageSize,
                                      count),
                            objects, mrs, build));
        EXPECT_EQ(build.pages.size(), count);
        EXPECT_EQ(build.pages.front(), 0x4000);
        EXPECT_EQ(build.pages.back(),
                  0x4000 + uint64_t{count - 1} * MrPageSize);
    }
}

TEST(PvrdmaMrTest, RejectsZeroAndUnalignedDirectoryTableAndLeavesAtomically)
{
    ObjectTables objects{};
    objects.pdAllocated[1] = 1;
    MemoryRegionTable mrs{};
    MemoryRegionBuild build{};
    ASSERT_TRUE(prepareCreateMr(mrRequest(0, 2 * MrPageSize, 2),
                                objects, mrs, build));
    std::array<uint64_t, MrEntriesPerPage> directory{};
    std::vector<uint64_t> tables;
    EXPECT_FALSE(consumeMrDirectory(build, directory, tables));
    directory[0] = htole(uint64_t{0x2001});
    EXPECT_FALSE(consumeMrDirectory(build, directory, tables));
    directory[0] = htole(uint64_t{0x2000});
    ASSERT_TRUE(consumeMrDirectory(build, directory, tables));

    std::array<uint64_t, MrEntriesPerPage> table{};
    table[0] = htole(uint64_t{0x4000});
    EXPECT_FALSE(consumeMrTable(build, 1, table));
    EXPECT_FALSE(consumeMrTable(build, 0, table));
    EXPECT_TRUE(build.pages.empty());
    table[1] = htole(uint64_t{0x5001});
    EXPECT_FALSE(consumeMrTable(build, 0, table));
    EXPECT_TRUE(build.pages.empty());
    table[1] = htole(uint64_t{0x5000});
    ASSERT_TRUE(consumeMrTable(build, 0, table));
    ASSERT_TRUE(mrs.commit(std::move(build), objects));
    EXPECT_EQ(objects.pdChildren[1], 1);
    EXPECT_EQ(mrs.entries[1].pages.size(), 2);
}

TEST(PvrdmaMrTest, AllocatesExhaustsDestroysAndRejectsStaleOrBusyHandles)
{
    ObjectTables objects{};
    objects.pdAllocated[1] = 1;
    MemoryRegionTable mrs{};
    std::vector<uint32_t> handles;
    for (uint32_t slot = 1; slot < ObjectTableEntries; ++slot) {
        MemoryRegionBuild build{};
        ASSERT_TRUE(buildMr(mrRequest(0, MrPageSize, 1), objects, mrs,
                            build, 0x100000 + uint64_t{slot} * MrPageSize));
        EXPECT_EQ(build.mrHandle, slot);
        handles.push_back(build.mrHandle);
        ASSERT_TRUE(mrs.commit(std::move(build), objects));
    }
    MemoryRegionBuild exhausted{};
    EXPECT_FALSE(prepareCreateMr(mrRequest(0, MrPageSize, 1), objects,
                                 mrs, exhausted));
    EXPECT_EQ(objects.pdChildren[1], ObjectTableEntries - 1);

    const uint32_t old_handle = handles[16];
    mrs.entries[17].activeReferences = 1;
    EXPECT_FALSE(mrs.destroy(old_handle, objects));
    mrs.entries[17].activeReferences = 0;
    EXPECT_TRUE(mrs.destroy(old_handle, objects));
    EXPECT_FALSE(mrs.destroy(old_handle, objects));
    EXPECT_FALSE(mrs.destroy(0, objects));

    MemoryRegionBuild reused{};
    ASSERT_TRUE(buildMr(mrRequest(0, MrPageSize, 1), objects, mrs, reused));
    EXPECT_EQ(reused.slot, 17);
    EXPECT_EQ(reused.mrHandle, 17 + ObjectTableEntries);
    EXPECT_NE(reused.mrHandle, old_handle);
    ASSERT_TRUE(mrs.commit(std::move(reused), objects));
    EXPECT_TRUE(validMemoryRegions(mrs, objects));

    mrs.reset();
    objects.pdChildren = {};
    EXPECT_TRUE(validMemoryRegions(mrs, objects));
}

TEST(PvrdmaMrTest, PdDestroyAndDestroyMrHonorDependenciesAndReservedBytes)
{
    ObjectTables objects{};
    objects.pdAllocated[1] = 1;
    MemoryRegionTable mrs{};
    MemoryRegionBuild build{};
    ASSERT_TRUE(buildMr(mrRequest(0, MrPageSize, 1), objects, mrs, build));
    ASSERT_TRUE(mrs.commit(std::move(build), objects));

    CommandRequest request{};
    CommandResponse response{};
    GidTable gids{};
    GidValidTable gids_valid{};
    UarRange range{};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyPd));
    request.destroyPd.pdHandle = htole(uint32_t{1});
    EXPECT_EQ(processCommand(request, response, gids, gids_valid, objects,
                             mrs, range).error, CommandError);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyMr));
    request.destroyMr.mrHandle = htole(uint32_t{1});
    request.destroyMr.reserved[0] = 1;
    EXPECT_EQ(processCommand(request, response, gids, gids_valid, objects,
                             mrs, range).error, CommandError);
    request.destroyMr.reserved[0] = 0;
    EXPECT_EQ(processCommand(request, response, gids, gids_valid, objects,
                             mrs, range).error, 0);
    EXPECT_EQ(objects.pdChildren[1], 0);
    EXPECT_EQ(processCommand(request, response, gids, gids_valid, objects,
                             mrs, range).error, CommandError);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyPd));
    request.destroyPd.pdHandle = htole(uint32_t{1});
    EXPECT_EQ(processCommand(request, response, gids, gids_valid, objects,
                             mrs, range).error, 0);
}

TEST(PvrdmaMrTest, TranslatesOneSgeWithinAndAcrossPages)
{
    ObjectTables objects{};
    objects.pdAllocated[1] = 1;
    MemoryRegionTable mrs{};
    MemoryRegionBuild build{};
    auto request = mrRequest(0x1003, 5000, 2,
                             AccessLocalWrite | AccessRemoteRead);
    ASSERT_TRUE(buildMr(request, objects, mrs, build, 0x8000));
    ASSERT_TRUE(mrs.commit(std::move(build), objects));

    std::vector<DmaChunk> chunks;
    EXPECT_TRUE(translate(mrs, MrKeyType::Local, 1, 0x1010, 16, 0,
                          chunks));
    EXPECT_EQ(chunks, (std::vector<DmaChunk>{{0x8010, 16}}));
    EXPECT_TRUE(translate(mrs, MrKeyType::Local, 1, 0x1ff0, 32, 0,
                          chunks));
    EXPECT_EQ(chunks, (std::vector<DmaChunk>{{0x8ff0, 16},
                                             {0x9000, 16}}));
    EXPECT_TRUE(translate(mrs, MrKeyType::Remote, 1, 0x1003, 1,
                          AccessRemoteRead, chunks));
    EXPECT_FALSE(translate(mrs, MrKeyType::Local, 2, 0x1003, 1, 0,
                           chunks));
    EXPECT_FALSE(translate(mrs, MrKeyType::Remote, 1, 0x1003, 1,
                           AccessRemoteWrite, chunks));
    EXPECT_FALSE(translate(mrs, MrKeyType::Local, 1, 0x1002, 1, 0,
                           chunks));
    EXPECT_FALSE(translate(mrs, MrKeyType::Local, 1, 0x1003, 0, 0,
                           chunks));
    EXPECT_FALSE(translate(mrs, MrKeyType::Local, 1, UINT64_MAX, 2, 0,
                           chunks));
    EXPECT_FALSE(translate(mrs, MrKeyType::Local, 1, 0x1003 + 5000, 1, 0,
                           chunks));

    ASSERT_TRUE(mrs.destroy(1, objects));
    EXPECT_FALSE(translate(mrs, MrKeyType::Local, 1, 0x1003, 1, 0,
                           chunks));
}

TEST(PvrdmaMrTest, AcquiresAndReleasesGenerationSafeLocalKeys)
{
    ObjectTables objects{};
    objects.pdAllocated[1] = 1;
    MemoryRegionTable mrs{};
    MemoryRegionBuild build{};
    ASSERT_TRUE(buildMr(mrRequest(0x1003, 5000, 2,
                                  AccessLocalWrite | AccessRemoteRead),
                        objects, mrs, build, 0x8000));
    ASSERT_TRUE(mrs.commit(std::move(build), objects));
    auto qp = testQueuePair();

    MemoryRegionLease lease{9, 9, {{1, 2}}};
    auto wrong_pd = qp;
    wrong_pd.pdHandle = 2;
    EXPECT_FALSE(acquireLocalMr(mrs, wrong_pd, QueueKind::Sq, 1,
                                0x1ff0, 32, lease));
    EXPECT_EQ(mrs.entries[1].activeReferences, 0);
    EXPECT_EQ(lease.slot, 9);
    EXPECT_EQ(lease.generation, 9);
    EXPECT_EQ(lease.chunks, (std::vector<DmaChunk>{{1, 2}}));

    EXPECT_FALSE(acquireLocalMr(mrs, qp, QueueKind::Sq, 2,
                                0x1ff0, 32, lease));
    EXPECT_FALSE(acquireLocalMr(mrs, qp, QueueKind::None, 1,
                                0x1ff0, 32, lease));
    EXPECT_FALSE(acquireLocalMr(mrs, qp, QueueKind::Sq, 1,
                                0x1002, 1, lease));
    EXPECT_EQ(mrs.entries[1].activeReferences, 0);
    const uint32_t access = mrs.entries[1].accessFlags;
    mrs.entries[1].accessFlags = 0;
    EXPECT_FALSE(acquireLocalMr(mrs, qp, QueueKind::Rq, 1,
                                0x1003, 1, lease));
    EXPECT_EQ(mrs.entries[1].activeReferences, 0);
    ASSERT_TRUE(acquireLocalMr(mrs, qp, QueueKind::Sq, 1,
                               0x1003, 1, lease));
    EXPECT_EQ(mrs.entries[1].activeReferences, 1);
    EXPECT_TRUE(releaseMr(mrs, lease));
    mrs.entries[1].accessFlags = access;

    ASSERT_TRUE(acquireLocalMr(mrs, qp, QueueKind::Rq, 1,
                               0x1ff0, 32, lease));
    EXPECT_EQ(lease.slot, 1);
    EXPECT_EQ(lease.generation, 0);
    EXPECT_EQ(lease.chunks, (std::vector<DmaChunk>{{0x8ff0, 16},
                                                   {0x9000, 16}}));
    EXPECT_EQ(mrs.entries[1].activeReferences, 1);
    EXPECT_FALSE(mrs.destroy(1, objects));
    const auto stale = lease;
    EXPECT_TRUE(releaseMr(mrs, lease));
    EXPECT_FALSE(releaseMr(mrs, lease));
    EXPECT_EQ(mrs.entries[1].activeReferences, 0);

    for (const uint64_t address : {uint64_t{0x1003},
                                   mrs.entries[1].end}) {
        ASSERT_TRUE(acquireLocalMr(mrs, qp, QueueKind::Rq, 1,
                                   address, 0, lease));
        EXPECT_TRUE(lease.chunks.empty());
        EXPECT_TRUE(releaseMr(mrs, lease));
    }
    EXPECT_FALSE(acquireLocalMr(mrs, qp, QueueKind::Sq, 1,
                                0x1002, 0, lease));
    EXPECT_FALSE(acquireLocalMr(mrs, qp, QueueKind::Sq, 1,
                                mrs.entries[1].end + 1, 0, lease));
    EXPECT_EQ(mrs.entries[1].activeReferences, 0);

    ASSERT_TRUE(mrs.destroy(1, objects));
    ASSERT_TRUE(buildMr(mrRequest(0x1003, 5000, 2,
                                  AccessLocalWrite | AccessRemoteRead),
                        objects, mrs, build, 0xa000));
    ASSERT_TRUE(mrs.commit(std::move(build), objects));
    ASSERT_TRUE(acquireLocalMr(mrs, qp, QueueKind::Rq, 65,
                               0x1003, 1, lease));
    EXPECT_FALSE(releaseMr(mrs, stale));
    EXPECT_EQ(mrs.entries[1].activeReferences, 1);
    EXPECT_TRUE(releaseMr(mrs, lease));
}

TEST(PvrdmaMrTest, ValidatesRestoreConsistency)
{
    ObjectTables objects{};
    objects.pdAllocated[1] = 1;
    MemoryRegionTable mrs{};
    MemoryRegionBuild build{};
    ASSERT_TRUE(buildMr(mrRequest(0x1003, 4094, 2), objects, mrs, build));
    ASSERT_TRUE(mrs.commit(std::move(build), objects));
    EXPECT_TRUE(validMemoryRegions(mrs, objects));

    auto malformed = mrs;
    malformed.entries[1].pages.pop_back();
    EXPECT_FALSE(validMemoryRegions(malformed, objects));
    malformed = mrs;
    malformed.entries[1].pages[0]++;
    EXPECT_FALSE(validMemoryRegions(malformed, objects));
    malformed = mrs;
    malformed.entries[1].end++;
    EXPECT_FALSE(validMemoryRegions(malformed, objects));
    auto malformed_objects = objects;
    malformed_objects.pdChildren[1] = 0;
    EXPECT_FALSE(validMemoryRegions(mrs, malformed_objects));
}

TEST(PvrdmaCqTest, CreateFailuresClearAcknowledgement)
{
    CommandHeader request{};
    request.response = htole(uint64_t{0x123456789abcdef0});
    ResponseHeader response{};
    detail::setCreateResponseHeader(
        response, request, static_cast<uint32_t>(Command::CreateCq), false);
    EXPECT_EQ(response.response, request.response);
    EXPECT_EQ(response.acknowledgement, 0);
    EXPECT_EQ(response.error, CommandError);
    detail::setCreateResponseHeader(
        response, request, static_cast<uint32_t>(Command::CreateQp), true);
    EXPECT_EQ(letoh(response.acknowledgement),
              responseCommand(Command::CreateQp));
    EXPECT_EQ(response.error, 0);
}

TEST(PvrdmaCqTest, EnforcesFrozenGeometryAndMalformedWalks)
{
    ObjectTables objects{};
    objects.contextUar[1] = 1;
    CompletionQueueTable cqs{};

    for (const auto &[cqe, chunks] :
         {std::pair{1U, 2U}, {64U, 2U}, {1024U, 17U}}) {
        CompletionQueueBuild build{};
        ASSERT_TRUE(buildCq(cqRequest(cqe), objects, cqs, build));
        EXPECT_EQ(build.numChunks, chunks);
        EXPECT_EQ(build.pages.size(), chunks);
    }
    EXPECT_EQ(1 + detail::chunksFor(65, CqeSize), 3);

    CompletionQueueBuild build{};
    auto malformed = cqRequest(64);
    malformed.header.reserved = htole(uint32_t{1});
    EXPECT_FALSE(prepareCreateCq(malformed, objects, cqs, build));
    malformed = cqRequest(64);
    malformed.createCq.reserved[0] = 1;
    EXPECT_FALSE(prepareCreateCq(malformed, objects, cqs, build));
    for (const uint32_t cqe : {0U, 3U, 65U, 2048U}) {
        malformed = cqRequest(64);
        malformed.createCq.cqe = htole(cqe);
        EXPECT_FALSE(prepareCreateCq(malformed, objects, cqs, build));
    }
    malformed = cqRequest(65);
    malformed.createCq.numChunks = htole(uint32_t{2});
    EXPECT_FALSE(prepareCreateCq(malformed, objects, cqs, build));
    malformed = cqRequest(64);
    malformed.createCq.pageDirectoryDma = htole(uint64_t{0x1001});
    EXPECT_FALSE(prepareCreateCq(malformed, objects, cqs, build));
    malformed = cqRequest(64, 0);
    EXPECT_FALSE(prepareCreateCq(malformed, objects, cqs, build));

    ASSERT_TRUE(prepareCreateCq(cqRequest(128), objects, cqs, build));
    std::array<uint64_t, PageEntries> directory{};
    std::vector<uint64_t> tables;
    EXPECT_FALSE(consumePageDirectory(build, directory, tables));
    directory[0] = htole(uint64_t{0x2001});
    EXPECT_FALSE(consumePageDirectory(build, directory, tables));
    directory[0] = htole(uint64_t{0x2000});
    ASSERT_TRUE(consumePageDirectory(build, directory, tables));
    std::array<uint64_t, PageEntries> table{};
    table[0] = htole(uint64_t{0x4000});
    table[1] = htole(uint64_t{0x5000});
    EXPECT_FALSE(consumePageTable(build, 1, table));
    EXPECT_FALSE(consumePageTable(build, 0, table));
    EXPECT_TRUE(build.pages.empty());
    table[2] = htole(uint64_t{0x6001});
    EXPECT_FALSE(consumePageTable(build, 0, table));
    EXPECT_TRUE(build.pages.empty());
}

TEST(PvrdmaCqTest, EnforcesDependenciesAndReusesDirectHandles)
{
    ObjectTables objects{};
    objects.contextUar[1] = 1;
    CompletionQueueTable cqs{};
    CompletionQueueBuild build{};
    ASSERT_TRUE(buildCq(cqRequest(64), objects, cqs, build));
    ASSERT_TRUE(cqs.commit(std::move(build), objects));
    EXPECT_EQ(cqs.entries[1].cqHandle, 1);
    EXPECT_EQ(cqs.entries[1].uar, 1);
    EXPECT_EQ(objects.contextCqChildren[1], 1);

    CommandRequest request{};
    CommandResponse response{};
    GidTable gids{};
    GidValidTable gid_valid{};
    MemoryRegionTable mrs{};
    QueuePairTable qps{};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyUc));
    request.destroyUc.contextHandle = htole(uint32_t{1});
    EXPECT_EQ(processCommand(request, response, gids, gid_valid, objects,
                             mrs, cqs, qps, {}).error, CommandError);

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyCq));
    request.destroyCq.cqHandle = htole(uint32_t{1});
    request.destroyCq.reserved[0] = 1;
    EXPECT_EQ(processCommand(request, response, gids, gid_valid, objects,
                             mrs, cqs, qps, {}).error, CommandError);
    request.destroyCq.reserved[0] = 0;
    cqs.entries[1].producerTail = 3;
    cqs.entries[1].armFlags = static_cast<uint32_t>(CqArmMode::Any);
    auto result = processCommand(request, response, gids, gid_valid, objects,
                                 mrs, cqs, qps, {});
    EXPECT_FALSE(result.hasResponse);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(objects.contextCqChildren[1], 0);
    EXPECT_EQ(cqs.entries[1].producerTail, 0);
    EXPECT_EQ(cqs.entries[1].consumerHead, 0);
    EXPECT_EQ(cqs.entries[1].armFlags, 0);
    EXPECT_EQ(cqs.entries[1].generation, 1);

    ASSERT_TRUE(buildCq(cqRequest(64), objects, cqs, build));
    EXPECT_EQ(build.slot, 1);
    ASSERT_TRUE(cqs.commit(std::move(build), objects));
}

TEST(PvrdmaQpTest, EnforcesFrozenGeometryAndCreateValidation)
{
    ObjectTables objects{};
    setupUserParent(objects);
    CompletionQueueTable cqs{};
    CompletionQueueBuild cq_build{};
    ASSERT_TRUE(buildCq(cqRequest(64), objects, cqs, cq_build));
    ASSERT_TRUE(cqs.commit(std::move(cq_build), objects));
    QueuePairTable qps{};

    for (const auto &[send_wr, recv_wr, send_chunks, recv_chunks] :
         {std::tuple{1U, 1U, 1U, 1U}, {32U, 128U, 1U, 1U},
          {64U, 256U, 2U, 2U}, {256U, 256U, 8U, 2U}}) {
        QueuePairBuild build{};
        ASSERT_TRUE(buildQp(qpRequest(send_wr, recv_wr), objects, cqs,
                            qps, build));
        EXPECT_EQ(build.sendChunks, send_chunks);
        EXPECT_EQ(build.recvChunks, recv_chunks);
        EXPECT_EQ(build.numChunks, 1 + send_chunks + recv_chunks);
    }

    QueuePairBuild build{};
    auto malformed = qpRequest(32, 128);
    malformed.header.reserved = htole(uint32_t{1});
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    malformed = qpRequest(32, 128);
    malformed.createQp.reserved[0] = 1;
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    malformed = qpRequest(32, 128);
    malformed.createQp.qpType = static_cast<uint8_t>(QpType::Ud);
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    malformed = qpRequest(32, 128);
    malformed.createQp.srqHandle = htole(uint32_t{1});
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    malformed = qpRequest(32, 128);
    malformed.createQp.isSrq = 1;
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    for (const uint32_t depth : {0U, 3U, 512U}) {
        malformed = qpRequest(32, 128);
        malformed.createQp.maxSendWr = htole(depth);
        EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    }
    malformed = qpRequest(32, 128);
    malformed.createQp.maxSendSge = htole(uint32_t{2});
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    malformed = qpRequest(32, 128);
    malformed.createQp.maxInlineData = htole(uint32_t{1});
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    malformed = qpRequest(32, 128);
    malformed.createQp.lkey = htole(uint32_t{1});
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    malformed = qpRequest(32, 128);
    malformed.createQp.accessFlags = 0;
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    malformed = qpRequest(32, 128);
    malformed.createQp.maxAtomicArgument = htole(uint16_t{1});
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    malformed = qpRequest(32, 128);
    malformed.createQp.sendChunks = htole(uint16_t{2});
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
    malformed = qpRequest(32, 128);
    malformed.createQp.totalChunks = htole(uint16_t{4});
    EXPECT_FALSE(prepareCreateQp(malformed, objects, cqs, qps, build));
}

TEST(PvrdmaQpTest, AllocatesOnlyUnicastTwentyFourBitQpns)
{
    ObjectTables objects{};
    setupUserParent(objects);
    CompletionQueueTable cqs{};
    CompletionQueueBuild cq_build{};
    ASSERT_TRUE(buildCq(cqRequest(64), objects, cqs, cq_build));
    ASSERT_TRUE(cqs.commit(std::move(cq_build), objects));
    QueuePairTable qps{};
    for (uint32_t slot = 1; slot < ObjectTableEntries; ++slot)
        qps.entries[slot].valid = true;

    qps.entries[1].valid = false;
    qps.entries[1].generation = MaxQpGeneration;
    QueuePairBuild build{};
    ASSERT_TRUE(buildQp(qpRequest(32, 128), objects, cqs, qps, build));
    EXPECT_EQ(build.qpn, (MaxQpGeneration << SlotBits) | 1);
    EXPECT_TRUE(validQpn(build.qpn));

    qps.entries[1].generation = MaxQpGeneration + 1;
    qps.entries[ObjectTableEntries - 1].valid = false;
    qps.entries[ObjectTableEntries - 1].generation = MaxQpGeneration;
    EXPECT_FALSE(prepareCreateQp(
        qpRequest(32, 128), objects, cqs, qps, build));
    EXPECT_EQ(MaxMrGeneration, MaxGeneration);
    EXPECT_GT(MaxGeneration, MaxQpGeneration);
}

TEST(PvrdmaQpTest, LooksUpOnlyExactGenerationDerivedQpn)
{
    QueuePairTable qps;
    qps.entries[1] = testQueuePair();
    EXPECT_EQ(findQueuePair(qps, qps.entries[1].qpn), &qps.entries[1]);
    EXPECT_EQ(findQueuePair(qps, 1), nullptr);
    EXPECT_EQ(findQueuePair(qps, qps.entries[1].qpn + ObjectTableEntries),
              nullptr);
    qps.entries[1].valid = false;
    EXPECT_EQ(findQueuePair(qps, (2 << SlotBits) | 1), nullptr);
}

TEST(PvrdmaTransportTest, DecodesReliabilityTimersAndRetries)
{
    EXPECT_EQ(ackTimeoutNanoseconds(0), 0);
    EXPECT_EQ(ackTimeoutNanoseconds(1), 8192);
    EXPECT_EQ(ackTimeoutNanoseconds(8), 1048576);
    EXPECT_EQ(ackTimeoutNanoseconds(31), uint64_t{4096} << 31);
    EXPECT_EQ(saturatingMultiply(5, 7, 100), 35);
    EXPECT_EQ(saturatingMultiply(51, 2, 100), 100);
    EXPECT_EQ(saturatingAdd(40, 50, 100), 90);
    EXPECT_EQ(saturatingAdd(40, 61, 100), 100);
    constexpr std::array<uint32_t, 32> expected = {
        655360, 10, 20, 30, 40, 60, 80, 120,
        160, 240, 320, 480, 640, 960, 1280, 1920,
        2560, 3840, 5120, 7680, 10240, 15360, 20480, 30720,
        40960, 61440, 81920, 122880, 163840, 245760, 327680, 491520,
    };
    EXPECT_EQ(RnrTimerMicros, expected);

    uint8_t retry = 0;
    EXPECT_FALSE(useRetry(retry));
    retry = 2;
    EXPECT_TRUE(useRetry(retry));
    EXPECT_EQ(retry, 1);
    EXPECT_TRUE(useRetry(retry));
    EXPECT_EQ(retry, 0);
    EXPECT_FALSE(useRetry(retry));
    retry = 7;
    for (int attempt = 0; attempt < 100; ++attempt) {
        EXPECT_TRUE(useRetry(retry));
        EXPECT_EQ(retry, 7);
    }
}

TEST(PvrdmaTransportTest, BoundsSequenceNakProgressAndRetries)
{
    uint8_t retry = 2;
    EXPECT_EQ(sequenceNakAction(0x100, 0, 3, 0x101, retry),
              SequenceNakAction::Advance);
    EXPECT_EQ(retry, 2);
    EXPECT_EQ(sequenceNakAction(0x100, 0, 3, 0x102, retry),
              SequenceNakAction::Invalid);
    EXPECT_EQ(sequenceNakAction(0x100, 2, 3, 0x103, retry),
              SequenceNakAction::Advance);
    EXPECT_EQ(retry, 2);

    EXPECT_EQ(sequenceNakAction(0x100, 1, 3, 0x101, retry),
              SequenceNakAction::Restart);
    EXPECT_EQ(retry, 1);
    EXPECT_EQ(sequenceNakAction(0x100, 1, 3, 0x100, retry),
              SequenceNakAction::Restart);
    EXPECT_EQ(retry, 0);
    EXPECT_EQ(sequenceNakAction(0x100, 1, 3, 0x101, retry),
              SequenceNakAction::RetryExceeded);

    retry = 1;
    EXPECT_EQ(sequenceNakAction(rocev2::Field24Mask, 0, 2, 0, retry),
              SequenceNakAction::Advance);
    EXPECT_EQ(sequenceNakAction(rocev2::Field24Mask, 0, 2, 1, retry),
              SequenceNakAction::Invalid);
    EXPECT_EQ(sequenceNakAction(rocev2::Field24Mask, 1, 2,
                                rocev2::Field24Mask, retry),
              SequenceNakAction::Restart);
}

TEST(PvrdmaTransportTest, ComputesCanonicalSegmentsAndLeaseRanges)
{
    for (const auto &[length, count] : {
             std::pair{0U, uint16_t{1}}, {1U, uint16_t{1}},
             {1023U, uint16_t{1}}, {1024U, uint16_t{1}},
             {1025U, uint16_t{2}}, {MaxMessageSize, uint16_t{65535}}}) {
        uint16_t actual = 0;
        EXPECT_TRUE(segmentGeometry(length, actual));
        EXPECT_EQ(actual, count);
    }
    uint16_t count = 0;
    EXPECT_FALSE(segmentGeometry(MaxMessageSize + 1, count));

    MemoryRegionLease lease{1, 0, {{0x1000, 3}, {0x2000, 4}}};
    size_t chunk = 0;
    size_t offset = 0;
    EXPECT_TRUE(leaseRange(lease, 2, 4, chunk, offset));
    EXPECT_EQ(chunk, 0);
    EXPECT_EQ(offset, 2);
    EXPECT_TRUE(leaseRange(lease, 3, 4, chunk, offset));
    EXPECT_EQ(chunk, 1);
    EXPECT_EQ(offset, 0);
    EXPECT_TRUE(leaseRange(lease, 7, 0, chunk, offset));
    EXPECT_EQ(chunk, 1);
    EXPECT_EQ(offset, 4);
    EXPECT_FALSE(leaseRange(lease, 4, 4, chunk, offset));
}

TEST(PvrdmaQpTest, ComputesExactConsumerAddressesAndPsnWrap)
{
    const auto qp = testQueuePair();
    uint64_t address = 0;
    ASSERT_TRUE(queueConsumerAddress(qp, QueueKind::Sq, address));
    EXPECT_EQ(address, qp.pages[0] + offsetof(RingState, tx) +
                           offsetof(Ring, consumerHead));
    ASSERT_TRUE(queueConsumerAddress(qp, QueueKind::Rq, address));
    EXPECT_EQ(address, qp.pages[0] + offsetof(RingState, rx) +
                           offsetof(Ring, consumerHead));
    EXPECT_FALSE(queueConsumerAddress(qp, QueueKind::Cq, address));
    EXPECT_TRUE(validPsn(rocev2::Field24Mask));
    EXPECT_FALSE(validPsn(rocev2::Field24Mask + 1));
    EXPECT_EQ(advancePsn(rocev2::Field24Mask), 0);
}

TEST(PvrdmaQpTest, ComputesExactSqAndRqWqeAddresses)
{
    const auto qp = testQueuePair();
    uint64_t address = 0;
    ASSERT_TRUE(wqeAddress(qp, QueueKind::Sq, 0, address));
    EXPECT_EQ(address, 0x9000);
    ASSERT_TRUE(wqeAddress(qp, QueueKind::Sq, 31, address));
    EXPECT_EQ(address, 0x9f80);
    ASSERT_TRUE(wqeAddress(qp, QueueKind::Sq, 32, address));
    EXPECT_EQ(address, 0xa000);
    ASSERT_TRUE(wqeAddress(qp, QueueKind::Sq, 64, address));
    EXPECT_EQ(address, 0x9000);

    ASSERT_TRUE(wqeAddress(qp, QueueKind::Rq, 0, address));
    EXPECT_EQ(address, 0xb000);
    ASSERT_TRUE(wqeAddress(qp, QueueKind::Rq, 127, address));
    EXPECT_EQ(address, 0xbfe0);
    ASSERT_TRUE(wqeAddress(qp, QueueKind::Rq, 128, address));
    EXPECT_EQ(address, 0xc000);
    ASSERT_TRUE(wqeAddress(qp, QueueKind::Rq, 256, address));
    EXPECT_EQ(address, 0xb000);
}

TEST(PvrdmaQpTest, RejectsMalformedWqeAddressGeometry)
{
    const auto qp = testQueuePair();
    uint64_t address = 0xdeadbeef;
    EXPECT_FALSE(wqeAddress({}, QueueKind::Sq, 0, address));
    EXPECT_FALSE(wqeAddress(qp, QueueKind::None, 0, address));
    EXPECT_FALSE(wqeAddress(qp, QueueKind::Cq, 0, address));
    EXPECT_FALSE(wqeAddress(qp, static_cast<QueueKind>(0xff), 0, address));

    auto malformed = qp;
    malformed.capabilities.maxSendWr = 3;
    EXPECT_FALSE(wqeAddress(malformed, QueueKind::Sq, 0, address));
    malformed = qp;
    malformed.sendChunks++;
    EXPECT_FALSE(wqeAddress(malformed, QueueKind::Sq, 0, address));
    malformed = qp;
    malformed.totalChunks--;
    EXPECT_FALSE(wqeAddress(malformed, QueueKind::Rq, 0, address));
    malformed = qp;
    malformed.pages.resize(1);
    EXPECT_FALSE(wqeAddress(malformed, QueueKind::Sq, 32, address));
    malformed = qp;
    malformed.pages[1] = 0;
    EXPECT_FALSE(wqeAddress(malformed, QueueKind::Sq, 0, address));
    malformed = qp;
    malformed.pages.back()++;
    EXPECT_FALSE(wqeAddress(malformed, QueueKind::Rq, 128, address));
    malformed = qp;
    malformed.generation++;
    EXPECT_FALSE(wqeAddress(malformed, QueueKind::Sq, 0, address));
    EXPECT_EQ(address, 0xdeadbeef);
}

TEST(PvrdmaWqeTest, DecodesLittleEndianSqAndSignalPolicy)
{
    SendWqeHeader header{};
    header.workRequestId = htole(uint64_t{0x1122334455667788});
    header.numSge = htole(uint32_t{1});
    header.reservedLength = htole(uint32_t{0xffffffff});
    header.opcode = htole(
        static_cast<uint32_t>(WorkRequestOpcode::Send));
    Sge sge{htole(uint64_t{0x8877665544332211}),
            htole(uint32_t{MaxMessageSize}), htole(uint32_t{0x12345678})};
    auto slot = sqSlot(header, sge);
    slot.back() = 0xaa;
    PreparedWqe prepared;
    auto qp = testQueuePair();
    auto invalid_qp = qp;
    invalid_qp.valid = false;
    EXPECT_FALSE(decodeSqWqe(invalid_qp, slot, prepared));
    ASSERT_TRUE(decodeSqWqe(qp, slot, prepared));
    EXPECT_EQ(prepared.kind, QueueKind::Sq);
    EXPECT_EQ(prepared.workRequestId, 0x1122334455667788);
    EXPECT_EQ(prepared.address, 0x8877665544332211);
    EXPECT_EQ(prepared.length, MaxMessageSize);
    EXPECT_EQ(prepared.lkey, 0x12345678);
    EXPECT_FALSE(prepared.signaled);

    header.sendFlags = htole(SendSignaled);
    ASSERT_TRUE(decodeSqWqe(qp, sqSlot(header, sge), prepared));
    EXPECT_TRUE(prepared.signaled);
    header.sendFlags = 0;
    qp.signalAllSendWr = true;
    ASSERT_TRUE(decodeSqWqe(qp, sqSlot(header, sge), prepared));
    EXPECT_TRUE(prepared.signaled);
}

TEST(PvrdmaWqeTest, RejectsEachUnsupportedSqField)
{
    SendWqeHeader header{};
    header.numSge = htole(uint32_t{1});
    header.opcode = htole(
        static_cast<uint32_t>(WorkRequestOpcode::Send));
    Sge sge{htole(uint64_t{0x1000}), htole(uint32_t{1}),
            htole(uint32_t{1})};
    PreparedWqe prepared;
    const auto qp = testQueuePair();

    auto malformed = header;
    malformed.numSge = htole(uint32_t{2});
    EXPECT_FALSE(decodeSqWqe(qp, sqSlot(malformed, sge), prepared));
    malformed = header;
    malformed.opcode = htole(
        static_cast<uint32_t>(WorkRequestOpcode::RdmaWrite));
    EXPECT_FALSE(decodeSqWqe(qp, sqSlot(malformed, sge), prepared));
    malformed = header;
    malformed.sendFlags = htole(SendFence);
    EXPECT_FALSE(decodeSqWqe(qp, sqSlot(malformed, sge), prepared));
    malformed = header;
    malformed.sendFlags = htole(SendSignaled | SendFence);
    EXPECT_FALSE(decodeSqWqe(qp, sqSlot(malformed, sge), prepared));
    malformed = header;
    malformed.invalidateRkey = htole(uint32_t{1});
    EXPECT_FALSE(decodeSqWqe(qp, sqSlot(malformed, sge), prepared));
    malformed = header;
    malformed.reserved = htole(uint32_t{1});
    EXPECT_FALSE(decodeSqWqe(qp, sqSlot(malformed, sge), prepared));
    malformed = header;
    malformed.operationData[47] = 1;
    EXPECT_FALSE(decodeSqWqe(qp, sqSlot(malformed, sge), prepared));
    auto long_sge = sge;
    long_sge.length = htole(MaxMessageSize + 1);
    EXPECT_FALSE(decodeSqWqe(qp, sqSlot(header, long_sge), prepared));
}

TEST(PvrdmaWqeTest, DecodesAndValidatesLittleEndianRq)
{
    ReceiveWqeHeader header{};
    header.workRequestId = htole(uint64_t{0x1122334455667788});
    header.numSge = htole(uint32_t{1});
    header.reservedLength = htole(uint32_t{0xffffffff});
    Sge sge{htole(uint64_t{0x8877665544332211}),
            htole(uint32_t{MaxMessageSize}), htole(uint32_t{0x12345678})};
    PreparedWqe prepared;
    const auto qp = testQueuePair();
    auto invalid_qp = qp;
    invalid_qp.valid = false;
    EXPECT_FALSE(decodeRqWqe(invalid_qp, rqSlot(header, sge), prepared));
    ASSERT_TRUE(decodeRqWqe(qp, rqSlot(header, sge), prepared));
    EXPECT_EQ(prepared.kind, QueueKind::Rq);
    EXPECT_EQ(prepared.workRequestId, 0x1122334455667788);
    EXPECT_EQ(prepared.address, 0x8877665544332211);
    EXPECT_EQ(prepared.length, MaxMessageSize);
    EXPECT_EQ(prepared.lkey, 0x12345678);
    EXPECT_FALSE(prepared.signaled);

    header.numSge = htole(uint32_t{2});
    EXPECT_FALSE(decodeRqWqe(qp, rqSlot(header, sge), prepared));
    header.numSge = htole(uint32_t{1});
    sge.length = htole(MaxMessageSize + 1);
    EXPECT_FALSE(decodeRqWqe(qp, rqSlot(header, sge), prepared));
}

TEST(PvrdmaQpTest, TracksDependenciesDirectHandleAndGenerationSafeQpn)
{
    ObjectTables objects{};
    setupUserParent(objects);
    CompletionQueueTable cqs{};
    CompletionQueueBuild cq_build{};
    ASSERT_TRUE(buildCq(cqRequest(64), objects, cqs, cq_build));
    ASSERT_TRUE(cqs.commit(std::move(cq_build), objects));
    QueuePairTable qps{};
    QueuePairBuild build{};
    ASSERT_TRUE(buildQp(qpRequest(32, 128), objects, cqs, qps, build));
    ASSERT_TRUE(qps.commit(std::move(build), objects, cqs));
    EXPECT_EQ(qps.entries[1].qpHandle, 1);
    EXPECT_EQ(qps.entries[1].qpn, 1);
    EXPECT_EQ(cqs.entries[1].qpReferences, 2);
    EXPECT_EQ(objects.pdChildren[1], 1);
    EXPECT_FALSE(cqs.destroy(1, objects));

    CommandRequest request{};
    CommandResponse response{};
    GidTable gids{};
    GidValidTable gid_valid{};
    MemoryRegionTable mrs{};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyQp));
    request.destroyQp.qpHandle = htole(uint32_t{1});
    request.destroyQp.reserved[0] = 1;
    auto result = processCommand(request, response, gids, gid_valid, objects,
                                 mrs, cqs, qps, {});
    EXPECT_FALSE(result.hasResponse);
    EXPECT_EQ(result.error, CommandError);
    request.destroyQp.reserved[0] = 0;
    qps.entries[1].sqProducerTail = 3;
    qps.entries[1].rqProducerTail = 5;
    result = processCommand(request, response, gids, gid_valid, objects,
                            mrs, cqs, qps, {});
    EXPECT_FALSE(result.hasResponse);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(cqs.entries[1].qpReferences, 0);
    EXPECT_EQ(objects.pdChildren[1], 0);

    ASSERT_TRUE(buildQp(qpRequest(32, 128), objects, cqs, qps, build));
    EXPECT_EQ(build.slot, 1);
    EXPECT_EQ(build.qpn, 65);
    ASSERT_TRUE(qps.commit(std::move(build), objects, cqs));
    EXPECT_EQ(qps.entries[1].qpHandle, 1);
    EXPECT_EQ(qps.entries[1].qpn, 65);
}

TEST(PvrdmaQpTest, ReachesRtsQueriesStoredAttributesAndResets)
{
    ObjectTables objects{};
    setupUserParent(objects);
    CompletionQueueTable cqs{};
    CompletionQueueBuild cq_build{};
    ASSERT_TRUE(buildCq(cqRequest(64), objects, cqs, cq_build));
    ASSERT_TRUE(cqs.commit(std::move(cq_build), objects));
    QueuePairTable qps{};
    QueuePairBuild qp_build{};
    ASSERT_TRUE(buildQp(qpRequest(32, 128), objects, cqs, qps, qp_build));
    ASSERT_TRUE(qps.commit(std::move(qp_build), objects, cqs));
    CommandResponse response{};

    QpAttr init{};
    init.qpAccessFlags = htole(AccessLocalWrite | AccessRemoteRead);
    init.portNumber = 1;
    ASSERT_EQ(modify(qps, QpState::Init,
                     QpAttrState | QpAttrAccessFlags |
                     QpAttrPkeyIndex | QpAttrPort,
                     init, response).error, 0);

    QpAttr rtr{};
    rtr.pathMtu = static_cast<Mtu>(
        htole(static_cast<uint32_t>(Mtu::Mtu1024)));
    rtr.destinationQpNumber = htole(uint32_t{0x12345});
    rtr.receivePsn = htole(uint32_t{0x54321});
    rtr.maxDestinationReadAtomic = 1;
    rtr.minRnrTimer = 31;
    rtr.addressHandle.globalRoute.flowLabel = htole(uint32_t{0xabcde});
    rtr.addressHandle.globalRoute.sourceGidIndex = 7;
    rtr.addressHandle.destinationLid = htole(uint16_t{0x1234});
    rtr.addressHandle.vlanId = htole(uint16_t{0x456});
    rtr.addressHandle.portNumber = 1;
    ASSERT_EQ(modify(qps, QpState::ReadyToReceive,
                     QpAttrState | QpAttrAddressVector | QpAttrPathMtu |
                     QpAttrReceivePsn | QpAttrMaxDestReadAtomic |
                     QpAttrMinRnrTimer | QpAttrDestinationQpn,
                     rtr, response).error, 0);

    QpAttr rts{};
    rts.timeout = 31;
    rts.retryCount = 7;
    rts.rnrRetry = 7;
    rts.sendPsn = htole(uint32_t{0x13579});
    rts.maxReadAtomic = 1;
    ASSERT_EQ(modify(qps, QpState::ReadyToSend,
                     QpAttrState | QpAttrTimeout | QpAttrRetryCount |
                     QpAttrRnrRetry | QpAttrSendPsn |
                     QpAttrMaxQpReadAtomic,
                     rts, response).error, 0);

    CommandRequest query{};
    query.header.command = htole(
        static_cast<uint32_t>(Command::QueryQp));
    query.queryQp.qpHandle = htole(uint32_t{1});
    query.queryQp.attributeMask = htole((uint32_t{1} << 21) - 1);
    ASSERT_EQ(detail::queryQp(query, response, qps).error, 0);
    const auto &attr = response.queryQp.attributes;
    EXPECT_EQ(letoh(static_cast<uint32_t>(attr.qpState)),
              static_cast<uint32_t>(QpState::ReadyToSend));
    EXPECT_EQ(letoh(attr.qpAccessFlags),
              AccessLocalWrite | AccessRemoteRead);
    EXPECT_EQ(letoh(attr.destinationQpNumber), 0x12345);
    EXPECT_EQ(letoh(attr.receivePsn), 0x54321);
    EXPECT_EQ(letoh(attr.sendPsn), 0x13579);
    EXPECT_EQ(letoh(attr.addressHandle.globalRoute.flowLabel), 0xabcde);
    EXPECT_EQ(letoh(attr.addressHandle.destinationLid), 0x1234);
    EXPECT_EQ(letoh(attr.addressHandle.vlanId), 0x456);
    EXPECT_EQ(letoh(attr.capabilities.maxSendWr), 32);
    EXPECT_EQ(letoh(attr.capabilities.maxRecvWr), 128);
    EXPECT_EQ(letoh(attr.capabilities.maxSendSge), 1);
    EXPECT_EQ(letoh(attr.capabilities.maxRecvSge), 1);

    auto &stored = qps.entries[1];
    stored.state = QpState::Error;
    stored.attributes.qpState = stored.attributes.currentQpState =
        QpState::Error;
    stored.finalReplay.valid = true;
    stored.finalReplay.qpGeneration = stored.generation;
    stored.finalReplay.localMac = {0x02, 0, 0, 0, 0, 2};
    stored.finalReplay.remoteMac = {0x02, 0, 0, 0, 0, 1};
    std::copy(stored.finalReplay.remoteMac.begin(),
              stored.finalReplay.remoteMac.end(),
              stored.attributes.addressHandle.destinationMac);
    stored.finalReplay.localGid[0] = stored.finalReplay.remoteGid[0] = 0xfe;
    stored.finalReplay.localGid[1] = stored.finalReplay.remoteGid[1] = 0x80;
    stored.finalReplay.localGid[15] = 2;
    stored.finalReplay.remoteGid[15] = 1;
    stored.finalReplay.localQpn = stored.qpn;
    stored.finalReplay.remoteQpn = stored.attributes.destinationQpNumber;
    stored.finalReplay.finalPsn =
        (stored.attributes.receivePsn - 1) & rocev2::Field24Mask;
    stored.responderMsn = 1;
    stored.finalReplay.finalOpcode = rocev2::Opcode::SendOnly;
    stored.finalReplay.finalSegmentLength = 1;
    stored.finalReplay.completedMsn = 1;
    EXPECT_TRUE(detail::validFinalReplay(stored));
    ASSERT_EQ(detail::queryQp(query, response, qps).error, 0);
    EXPECT_EQ(letoh(static_cast<uint32_t>(
                  response.queryQp.attributes.qpState)),
              static_cast<uint32_t>(QpState::Error));
    EXPECT_EQ(modify(qps, QpState::Error, QpAttrState,
                     {}, response).error, CommandError);

    QpAttr reset{};
    ASSERT_EQ(modify(qps, QpState::Reset, QpAttrState,
                     reset, response).error, 0);
    EXPECT_EQ(qps.entries[1].state, QpState::Reset);
    EXPECT_EQ(qps.entries[1].attributes.capabilities.maxSendWr, 32);
    EXPECT_EQ(qps.entries[1].responderMsn, 0);
    EXPECT_FALSE(qps.entries[1].finalReplay.valid);
    EXPECT_TRUE(validQueueObjects(cqs, qps, MemoryRegionTable{}, objects));
}

TEST(PvrdmaQpTest, RejectsInvalidTransitionsAndResetsNonemptyQueues)
{
    ObjectTables objects{};
    setupUserParent(objects);
    CompletionQueueTable cqs{};
    CompletionQueueBuild cq_build{};
    ASSERT_TRUE(buildCq(cqRequest(64), objects, cqs, cq_build));
    ASSERT_TRUE(cqs.commit(std::move(cq_build), objects));
    QueuePairTable qps{};
    QueuePairBuild qp_build{};
    ASSERT_TRUE(buildQp(qpRequest(32, 128), objects, cqs, qps, qp_build));
    ASSERT_TRUE(qps.commit(std::move(qp_build), objects, cqs));
    CommandResponse response{};

    QpAttr attrs{};
    attrs.portNumber = 1;
    attrs.qpAccessFlags = htole(AccessLocalWrite);
    EXPECT_EQ(modify(qps, QpState::ReadyToReceive, QpAttrState,
                     attrs, response).error, CommandError);
    EXPECT_EQ(modify(qps, QpState::Init,
                     QpAttrState | QpAttrAccessFlags |
                     QpAttrPkeyIndex | QpAttrPort | QpAttrCapabilities,
                     attrs, response).error, CommandError);
    EXPECT_EQ(modify(qps, QpState::Init, uint32_t{1} << 31,
                     attrs, response).error, CommandError);
    attrs.portNumber = 2;
    EXPECT_EQ(modify(qps, QpState::Init,
                     QpAttrState | QpAttrAccessFlags |
                     QpAttrPkeyIndex | QpAttrPort,
                     attrs, response).error, CommandError);
    attrs.portNumber = 1;
    attrs.pkeyIndex = htole(uint16_t{1});
    EXPECT_EQ(modify(qps, QpState::Init,
                     QpAttrState | QpAttrAccessFlags |
                     QpAttrPkeyIndex | QpAttrPort,
                     attrs, response).error, CommandError);
    attrs.pkeyIndex = 0;
    attrs.reserved[0] = 1;
    EXPECT_EQ(modify(qps, QpState::Init,
                     QpAttrState | QpAttrAccessFlags |
                     QpAttrPkeyIndex | QpAttrPort,
                     attrs, response).error, CommandError);
    attrs.reserved[0] = 0;
    ASSERT_EQ(modify(qps, QpState::Init,
                     QpAttrState | QpAttrAccessFlags |
                     QpAttrPkeyIndex | QpAttrPort,
                     attrs, response).error, 0);

    QpAttr rtr{};
    rtr.pathMtu = static_cast<Mtu>(
        htole(static_cast<uint32_t>(Mtu::Mtu2048)));
    rtr.destinationQpNumber = htole(uint32_t{1});
    rtr.addressHandle.portNumber = 1;
    EXPECT_EQ(modify(qps, QpState::ReadyToReceive,
                     QpAttrState | QpAttrAddressVector | QpAttrPathMtu |
                     QpAttrReceivePsn | QpAttrMaxDestReadAtomic |
                     QpAttrMinRnrTimer | QpAttrDestinationQpn,
                     rtr, response).error, CommandError);
    rtr.pathMtu = static_cast<Mtu>(
        htole(static_cast<uint32_t>(Mtu::Mtu1024)));
    for (const uint32_t qpn : {0U, rocev2::Field24Mask,
                               rocev2::Field24Mask + 1}) {
        rtr.destinationQpNumber = htole(qpn);
        EXPECT_EQ(modify(qps, QpState::ReadyToReceive,
                         QpAttrState | QpAttrAddressVector | QpAttrPathMtu |
                         QpAttrReceivePsn | QpAttrMaxDestReadAtomic |
                         QpAttrMinRnrTimer | QpAttrDestinationQpn,
                         rtr, response).error, CommandError);
    }
    rtr.destinationQpNumber = htole(uint32_t{1});
    rtr.maxDestinationReadAtomic = 2;
    EXPECT_EQ(modify(qps, QpState::ReadyToReceive,
                     QpAttrState | QpAttrAddressVector | QpAttrPathMtu |
                     QpAttrReceivePsn | QpAttrMaxDestReadAtomic |
                     QpAttrMinRnrTimer | QpAttrDestinationQpn,
                     rtr, response).error, CommandError);
    rtr.maxDestinationReadAtomic = 0;
    rtr.receivePsn = htole(rocev2::Field24Mask + 1);
    EXPECT_EQ(modify(qps, QpState::ReadyToReceive,
                     QpAttrState | QpAttrAddressVector | QpAttrPathMtu |
                     QpAttrReceivePsn | QpAttrMaxDestReadAtomic |
                     QpAttrMinRnrTimer | QpAttrDestinationQpn,
                     rtr, response).error, CommandError);
    rtr.receivePsn = htole(uint32_t{0x123456});
    ASSERT_EQ(modify(qps, QpState::ReadyToReceive,
                     QpAttrState | QpAttrAddressVector | QpAttrPathMtu |
                     QpAttrReceivePsn | QpAttrMaxDestReadAtomic |
                     QpAttrMinRnrTimer | QpAttrDestinationQpn,
                     rtr, response).error, 0);

    constexpr uint32_t RtsMask = QpAttrState | QpAttrTimeout |
        QpAttrRetryCount | QpAttrRnrRetry | QpAttrSendPsn |
        QpAttrMaxQpReadAtomic;
    QpAttr rts{};
    rts.timeout = 32;
    rts.retryCount = 7;
    rts.rnrRetry = 7;
    EXPECT_EQ(modify(qps, QpState::ReadyToSend,
                     RtsMask, rts, response).error, CommandError);
    rts.timeout = 31;
    rts.retryCount = 8;
    EXPECT_EQ(modify(qps, QpState::ReadyToSend,
                     RtsMask, rts, response).error, CommandError);
    rts.retryCount = 7;
    rts.rnrRetry = 8;
    EXPECT_EQ(modify(qps, QpState::ReadyToSend,
                     RtsMask, rts, response).error, CommandError);
    rts.rnrRetry = 7;
    rts.sendPsn = htole(rocev2::Field24Mask + 1);
    EXPECT_EQ(modify(qps, QpState::ReadyToSend,
                     RtsMask, rts, response).error, CommandError);
    rts.sendPsn = htole(uint32_t{0x654321});
    ASSERT_EQ(modify(qps, QpState::ReadyToSend,
                     RtsMask, rts, response).error, 0);

    qps.entries[1].sqProducerTail = 3;
    qps.entries[1].rqProducerTail = 5;
    QpAttr reset{};
    EXPECT_EQ(modify(qps, QpState::Reset, QpAttrState,
                     reset, response).error, 0);
    EXPECT_EQ(qps.entries[1].state, QpState::Reset);
    EXPECT_EQ(qps.entries[1].sqProducerTail, 0);
    EXPECT_EQ(qps.entries[1].sqConsumerHead, 0);
    EXPECT_EQ(qps.entries[1].rqProducerTail, 0);
    EXPECT_EQ(qps.entries[1].rqConsumerHead, 0);
}

TEST(PvrdmaQpTest, ValidatesRestoreCountersGeometryStateAndGeneration)
{
    ObjectTables objects{};
    setupUserParent(objects);
    CompletionQueueTable cqs{};
    CompletionQueueBuild cq_build{};
    ASSERT_TRUE(buildCq(cqRequest(128), objects, cqs, cq_build));
    ASSERT_TRUE(cqs.commit(std::move(cq_build), objects));
    QueuePairTable qps{};
    QueuePairBuild qp_build{};
    ASSERT_TRUE(buildQp(qpRequest(64, 256), objects, cqs, qps, qp_build));
    ASSERT_TRUE(qps.commit(std::move(qp_build), objects, cqs));
    auto &qp = qps.entries[1];
    qp.state = QpState::ReadyToSend;
    qp.attributes.qpState = QpState::ReadyToSend;
    qp.attributes.currentQpState = QpState::ReadyToSend;
    qp.attributes.pathMtu = Mtu::Mtu1024;
    qp.attributes.qpAccessFlags = AccessLocalWrite;
    qp.attributes.destinationQpNumber = 1;
    qp.attributes.portNumber = 1;
    qp.attributes.timeout = 31;
    qp.attributes.retryCount = 7;
    qp.attributes.rnrRetry = 7;
    qp.attributes.addressHandle.portNumber = 1;
    MemoryRegionTable mrs{};
    EXPECT_TRUE(validQueueObjects(cqs, qps, mrs, objects));

    auto malformed_cqs = cqs;
    malformed_cqs.entries[1].qpReferences = 1;
    EXPECT_FALSE(validQueueObjects(malformed_cqs, qps, mrs, objects));
    malformed_cqs = cqs;
    malformed_cqs.entries[1].pages.pop_back();
    EXPECT_FALSE(validQueueObjects(malformed_cqs, qps, mrs, objects));
    auto malformed_qps = qps;
    malformed_qps.entries[1].qpn = 65;
    EXPECT_FALSE(validQueueObjects(cqs, malformed_qps, mrs, objects));
    malformed_qps = qps;
    malformed_qps.entries[1].qpn = rocev2::Field24Mask;
    EXPECT_FALSE(validQueueObjects(cqs, malformed_qps, mrs, objects));
    malformed_qps = qps;
    malformed_qps.entries[1].pages.pop_back();
    EXPECT_FALSE(validQueueObjects(cqs, malformed_qps, mrs, objects));
    malformed_qps = qps;
    malformed_qps.entries[1].attributes.destinationQpNumber = 0;
    EXPECT_FALSE(validQueueObjects(cqs, malformed_qps, mrs, objects));
    malformed_qps = qps;
    malformed_qps.entries[1].attributes.destinationQpNumber =
        rocev2::Field24Mask;
    EXPECT_FALSE(validQueueObjects(cqs, malformed_qps, mrs, objects));
    malformed_qps = qps;
    malformed_qps.entries[1].attributes.timeout = 32;
    EXPECT_FALSE(validQueueObjects(cqs, malformed_qps, mrs, objects));
    malformed_qps = qps;
    malformed_qps.entries[1].attributes.retryCount = 8;
    EXPECT_FALSE(validQueueObjects(cqs, malformed_qps, mrs, objects));
    malformed_qps = qps;
    malformed_qps.entries[1].attributes.rnrRetry = 8;
    EXPECT_FALSE(validQueueObjects(cqs, malformed_qps, mrs, objects));
    auto malformed_objects = objects;
    malformed_objects.contextCqChildren[1] = 0;
    EXPECT_FALSE(validQueueObjects(cqs, qps, mrs, malformed_objects));

    ASSERT_TRUE(qps.destroy(1, objects, cqs));
    EXPECT_EQ(qps.entries[1].generation, 1);
    EXPECT_TRUE(validQueueObjects(cqs, qps, mrs, objects));
    qps.reset();
    cqs.reset();
    objects.contextCqChildren = {};
    EXPECT_EQ(qps.entries[1].generation, 0);
}

TEST(PvrdmaDoorbellTest, DecodesOnlyExactLittleEndianAbiEncodings)
{
    Doorbell doorbell;
    const uint64_t qp = 3 * UarPageSize + QpDoorbellOffset;
    const uint64_t cq = 3 * UarPageSize + CqDoorbellOffset;

    ASSERT_TRUE(decodeDoorbell(qp, 4, SqDoorbellAction | 7, doorbell));
    EXPECT_EQ(doorbell.action, DoorbellAction::Sq);
    EXPECT_EQ(doorbell.handle, 7);
    EXPECT_EQ(doorbell.uar, 3);
    ASSERT_TRUE(decodeDoorbell(qp, 4, RqDoorbellAction | 7, doorbell));
    EXPECT_EQ(doorbell.action, DoorbellAction::Rq);
    ASSERT_TRUE(decodeDoorbell(cq, 4, CqPollAction | 7, doorbell));
    EXPECT_EQ(doorbell.action, DoorbellAction::CqPoll);
    ASSERT_TRUE(decodeDoorbell(cq, 4,
                               CqArmSolicitedAction | 7, doorbell));
    EXPECT_EQ(doorbell.action, DoorbellAction::CqArmSolicited);
    ASSERT_TRUE(decodeDoorbell(cq, 4, CqArmAnyAction | 7, doorbell));
    EXPECT_EQ(doorbell.action, DoorbellAction::CqArmAny);

    EXPECT_FALSE(decodeDoorbell(qp, 1, SqDoorbellAction | 7, doorbell));
    EXPECT_FALSE(decodeDoorbell(qp, 8, SqDoorbellAction | 7, doorbell));
    EXPECT_FALSE(decodeDoorbell(qp + 1, 4,
                                SqDoorbellAction | 7, doorbell));
    EXPECT_FALSE(decodeDoorbell(qp, 4, SqDoorbellAction, doorbell));
    EXPECT_FALSE(decodeDoorbell(qp, 4,
        SqDoorbellAction | ObjectTableEntries, doorbell));
    EXPECT_FALSE(decodeDoorbell(qp, 4,
        SqDoorbellAction | RqDoorbellAction | 7, doorbell));
    EXPECT_FALSE(decodeDoorbell(cq, 4,
        CqArmSolicitedAction | CqArmAnyAction | 7, doorbell));
    EXPECT_FALSE(decodeDoorbell(qp, 4,
        SqDoorbellAction | 0x01000000 | 7, doorbell));
}

TEST(PvrdmaDoorbellTest, ValidatesLiveHandleUarAndQpState)
{
    CompletionQueueTable cqs{};
    QueuePairTable qps{};
    auto &cq = cqs.entries[1];
    cq.valid = true;
    cq.cqHandle = 1;
    cq.uar = 2;
    auto &qp = qps.entries[1];
    qp.valid = true;
    qp.qpHandle = 1;
    qp.uar = 2;
    qp.state = QpState::ReadyToSend;
    Doorbell doorbell;

    ASSERT_TRUE(decodeDoorbell(2 * UarPageSize, 4,
                               SqDoorbellAction | 1, doorbell));
    EXPECT_TRUE(validDoorbell(doorbell, ControlState::Active, cqs, qps));
    qp.state = QpState::ReadyToReceive;
    EXPECT_FALSE(validDoorbell(doorbell, ControlState::Active, cqs, qps));
    ASSERT_TRUE(decodeDoorbell(2 * UarPageSize, 4,
                               RqDoorbellAction | 1, doorbell));
    EXPECT_TRUE(validDoorbell(doorbell, ControlState::Active, cqs, qps));
    qp.state = QpState::Init;
    EXPECT_TRUE(validDoorbell(doorbell, ControlState::Active, cqs, qps));
    qp.state = QpState::Reset;
    EXPECT_FALSE(validDoorbell(doorbell, ControlState::Active, cqs, qps));
    qp.state = QpState::ReadyToSend;
    ASSERT_TRUE(decodeDoorbell(3 * UarPageSize, 4,
                               RqDoorbellAction | 1, doorbell));
    EXPECT_FALSE(validDoorbell(doorbell, ControlState::Active, cqs, qps));
    qps.entries[1].valid = false;
    EXPECT_FALSE(validDoorbell(doorbell, ControlState::Active, cqs, qps));

    ASSERT_TRUE(decodeDoorbell(2 * UarPageSize + CqDoorbellOffset, 4,
                               CqPollAction | 1, doorbell));
    EXPECT_TRUE(validDoorbell(doorbell, ControlState::Active, cqs, qps));
    EXPECT_FALSE(validDoorbell(doorbell, ControlState::Ready, cqs, qps));
    cqs.entries[1].valid = false;
    EXPECT_FALSE(validDoorbell(doorbell, ControlState::Active, cqs, qps));
}

TEST(PvrdmaDoorbellTest, EmptyCqPollsDoNotNeedObservation)
{
    CompletionQueue cq;
    EXPECT_FALSE(cqPollNeedsObservation(cq));
    cq.producerTail = 1;
    EXPECT_TRUE(cqPollNeedsObservation(cq));
    cq.consumerHead = 1;
    EXPECT_FALSE(cqPollNeedsObservation(cq));
}

TEST(PvrdmaDoorbellTest, BlocksOnlyQueueDestroyAndResetTargets)
{
    CommandRequest request{};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyQp));
    request.destroyQp.qpHandle = htole(uint32_t{5});
    EXPECT_TRUE(queueCommandTargetBusy(request, uint64_t{1} << 5, 0));
    EXPECT_FALSE(queueCommandTargetBusy(request, uint64_t{1} << 4, 0));

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::DestroyCq));
    request.destroyCq.cqHandle = htole(uint32_t{6});
    EXPECT_TRUE(queueCommandTargetBusy(request, 0, uint64_t{1} << 6));

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(Command::ModifyQp));
    request.modifyQp.qpHandle = htole(uint32_t{5});
    request.modifyQp.attributes.qpState = static_cast<QpState>(
        htole(static_cast<uint32_t>(QpState::Reset)));
    EXPECT_TRUE(queueCommandTargetBusy(request, uint64_t{1} << 5, 0));
    request.modifyQp.attributes.qpState = static_cast<QpState>(
        htole(static_cast<uint32_t>(QpState::ReadyToSend)));
    EXPECT_FALSE(queueCommandTargetBusy(request, uint64_t{1} << 5, 0));
}

TEST(PvrdmaObservationTest, AcceptsProducerAdvanceWrapFullAndDuplicates)
{
    uint32_t producer = 0;
    uint32_t delta = 99;
    Ring observed{htole(uint32_t{8}), htole(uint32_t{0})};
    ASSERT_TRUE(observeProducer(observed, 8, producer, 0, delta));
    EXPECT_EQ(producer, 8);
    EXPECT_EQ(delta, 8);
    delta = 99;
    ASSERT_TRUE(observeProducer(observed, 8, producer, 0, delta));
    EXPECT_EQ(delta, 0);

    producer = 14;
    observed = {htole(uint32_t{1}), htole(uint32_t{12})};
    ASSERT_TRUE(observeProducer(observed, 8, producer, 12, delta));
    EXPECT_EQ(producer, 1);
    EXPECT_EQ(delta, 3);
}

TEST(PvrdmaObservationTest, RejectsMalformedProducerSnapshotsAtomically)
{
    uint32_t producer = 4;
    uint32_t delta = 99;
    Ring observed{htole(uint32_t{5}), htole(uint32_t{1})};
    EXPECT_FALSE(observeProducer(observed, 8, producer, 0, delta));
    EXPECT_EQ(producer, 4);
    EXPECT_EQ(delta, 99);

    observed = {htole(uint32_t{9}), htole(uint32_t{0})};
    EXPECT_FALSE(observeProducer(observed, 8, producer, 0, delta));
    EXPECT_EQ(producer, 4);
    observed = {htole(uint32_t{16}), htole(uint32_t{0})};
    EXPECT_FALSE(observeProducer(observed, 8, producer, 0, delta));
    EXPECT_EQ(producer, 4);
}

TEST(PvrdmaObservationTest, AcceptsOnlyBoundedCqConsumerReclamation)
{
    uint32_t consumer = 6;
    uint32_t delta = 99;
    Ring observed{htole(uint32_t{10}), htole(uint32_t{9})};
    ASSERT_TRUE(observeConsumer(observed, 8, 10, consumer, delta));
    EXPECT_EQ(consumer, 9);
    EXPECT_EQ(delta, 3);

    observed = {htole(uint32_t{11}), htole(uint32_t{10})};
    EXPECT_FALSE(observeConsumer(observed, 8, 10, consumer, delta));
    EXPECT_EQ(consumer, 9);
    observed = {htole(uint32_t{10}), htole(uint32_t{15})};
    EXPECT_FALSE(observeConsumer(observed, 8, 10, consumer, delta));
    EXPECT_EQ(consumer, 9);

    consumer = 14;
    observed = {htole(uint32_t{1}), htole(uint32_t{0})};
    ASSERT_TRUE(observeConsumer(observed, 8, 1, consumer, delta));
    EXPECT_EQ(consumer, 0);
    EXPECT_EQ(delta, 2);
}

TEST(PvrdmaObservationTest, ValidatesNonemptyRestoredQueueSnapshots)
{
    ObjectTables objects{};
    setupUserParent(objects);
    CompletionQueueTable cqs{};
    CompletionQueueBuild cq_build{};
    ASSERT_TRUE(buildCq(cqRequest(64), objects, cqs, cq_build));
    ASSERT_TRUE(cqs.commit(std::move(cq_build), objects));
    QueuePairTable qps{};
    QueuePairBuild qp_build{};
    ASSERT_TRUE(buildQp(qpRequest(32, 128), objects, cqs, qps, qp_build));
    ASSERT_TRUE(qps.commit(std::move(qp_build), objects, cqs));
    MemoryRegionTable mrs{};

    qps.entries[1].state = QpState::Init;
    qps.entries[1].attributes.qpState = QpState::Init;
    qps.entries[1].attributes.currentQpState = QpState::Init;
    qps.entries[1].attributes.qpAccessFlags = AccessLocalWrite;
    qps.entries[1].attributes.portNumber = 1;
    qps.entries[1].sqProducerTail = 7;
    qps.entries[1].rqProducerTail = 9;
    cqs.entries[1].producerTail = 5;
    cqs.entries[1].consumerHead = 2;
    cqs.entries[1].armFlags = static_cast<uint32_t>(CqArmMode::Any);
    EXPECT_TRUE(validQueueObjects(cqs, qps, mrs, objects));

    qps.entries[1].sqProducerTail = 33;
    EXPECT_FALSE(validQueueObjects(cqs, qps, mrs, objects));
    qps.entries[1].sqProducerTail = 7;
    cqs.entries[1].consumerHead = 63;
    EXPECT_FALSE(validQueueObjects(cqs, qps, mrs, objects));
}

} // namespace pvrdma
} // namespace gem5
