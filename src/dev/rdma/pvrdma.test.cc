// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <array>

#include "dev/rdma/pvrdma.hh"
#include "sim/byteswap.hh"

namespace gem5
{
namespace pvrdma
{

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
    EXPECT_EQ(caps.gidTypes, GidTypeRoceV1);
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
    request.createBind.gidType = GidTypeRoceV1;
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
    request.createBind.gidType = GidTypeRoceV2;
    EXPECT_EQ(processCommand(request, response, gids, valid, objects,
                             range).error, CommandError);

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
    EXPECT_FALSE(validObjectTables(malformed, range));

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
    ASSERT_TRUE(finishCommandRead(state, true));
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
    EXPECT_FALSE(checkpointStable(ControlState::ReadingDsr, false));
    EXPECT_FALSE(checkpointStable(ControlState::WritingCaps, false));
    EXPECT_FALSE(checkpointStable(ControlState::ReadingCommand, false));
    EXPECT_FALSE(checkpointStable(ControlState::WritingResponse, false));
}

} // namespace pvrdma
} // namespace gem5
