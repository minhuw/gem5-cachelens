// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cstddef>

#include "dev/rdma/pvrdma_abi.hh"
#include "dev/rdma/pvrdma_ring.hh"

namespace gem5
{
namespace pvrdma
{

TEST(PvrdmaAbiTest, DeviceAndSharedRegionLayout)
{
    EXPECT_EQ(PciVendorId, 0x15ad);
    EXPECT_EQ(PciDeviceId, 0x0820);
    EXPECT_EQ(MsixBarSize, 0x4000);
    EXPECT_EQ(RegisterBarSize, 0x100);
    EXPECT_EQ(UarBarSize, 0x200000);

    EXPECT_EQ(sizeof(DeviceCaps), 208);
    EXPECT_EQ(offsetof(DeviceCaps, vendorId), 64);
    EXPECT_EQ(offsetof(DeviceCaps, maxUar), 188);
    EXPECT_EQ(offsetof(DeviceCaps, mode), 200);
    EXPECT_EQ(offsetof(DeviceCaps, maxFastRegPageListLength), 204);

    EXPECT_EQ(sizeof(DeviceSharedRegion), 280);
    EXPECT_EQ(offsetof(DeviceSharedRegion, guestOsInfo), 8);
    EXPECT_EQ(offsetof(DeviceSharedRegion, commandSlotDma), 16);
    EXPECT_EQ(offsetof(DeviceSharedRegion, asyncRingPages), 32);
    EXPECT_EQ(offsetof(DeviceSharedRegion, completionRingPages), 48);
    EXPECT_EQ(offsetof(DeviceSharedRegion, uarPfn64), 64);
    EXPECT_EQ(offsetof(DeviceSharedRegion, caps), 72);

    EXPECT_EQ(sizeof(Ring), 8);
    EXPECT_EQ(sizeof(RingState), 16);
    EXPECT_EQ(offsetof(RingState, rx), 8);
}

TEST(PvrdmaAbiTest, CommandLayout)
{
    EXPECT_EQ(sizeof(CommandHeader), 16);
    EXPECT_EQ(offsetof(CommandHeader, command), 8);
    EXPECT_EQ(sizeof(ResponseHeader), 16);
    EXPECT_EQ(offsetof(ResponseHeader, error), 12);

    EXPECT_EQ(sizeof(QueryPortCommand), 24);
    EXPECT_EQ(sizeof(QueryPortResponse), 64);
    EXPECT_EQ(sizeof(QueryPkeyCommand), 24);
    EXPECT_EQ(sizeof(QueryPkeyResponse), 24);
    EXPECT_EQ(sizeof(CreateUcCommand), 24);
    EXPECT_EQ(sizeof(CreatePdCommand), 24);
    EXPECT_EQ(sizeof(CreateMrCommand), 56);
    EXPECT_EQ(offsetof(CreateMrCommand, start), 16);
    EXPECT_EQ(offsetof(CreateMrCommand, length), 24);
    EXPECT_EQ(offsetof(CreateMrCommand, pageDirectoryDma), 32);
    EXPECT_EQ(offsetof(CreateMrCommand, pdHandle), 40);
    EXPECT_EQ(offsetof(CreateMrCommand, accessFlags), 44);
    EXPECT_EQ(offsetof(CreateMrCommand, flags), 48);
    EXPECT_EQ(offsetof(CreateMrCommand, numChunks), 52);
    EXPECT_EQ(sizeof(CreateMrResponse), 32);
    EXPECT_EQ(offsetof(CreateMrResponse, mrHandle), 16);
    EXPECT_EQ(offsetof(CreateMrResponse, lkey), 20);
    EXPECT_EQ(offsetof(CreateMrResponse, rkey), 24);
    EXPECT_EQ(sizeof(DestroyMrCommand), 24);
    EXPECT_EQ(offsetof(DestroyMrCommand, mrHandle), 16);
    EXPECT_EQ(sizeof(CreateCqCommand), 40);
    EXPECT_EQ(offsetof(CreateCqCommand, pageDirectoryDma), 16);
    EXPECT_EQ(offsetof(CreateCqCommand, contextHandle), 24);
    EXPECT_EQ(offsetof(CreateCqCommand, cqe), 28);
    EXPECT_EQ(offsetof(CreateCqCommand, numChunks), 32);
    EXPECT_EQ(offsetof(CreateCqCommand, reserved), 36);
    EXPECT_EQ(sizeof(CreateCqResponse), 24);
    EXPECT_EQ(offsetof(CreateCqResponse, cqHandle), 16);
    EXPECT_EQ(offsetof(CreateCqResponse, cqe), 20);
    EXPECT_EQ(sizeof(DestroyCqCommand), 24);
    EXPECT_EQ(offsetof(DestroyCqCommand, reserved), 20);
    EXPECT_EQ(sizeof(CreateQpCommand), 80);
    EXPECT_EQ(offsetof(CreateQpCommand, pageDirectoryDma), 16);
    EXPECT_EQ(offsetof(CreateQpCommand, pdHandle), 24);
    EXPECT_EQ(offsetof(CreateQpCommand, sendCqHandle), 28);
    EXPECT_EQ(offsetof(CreateQpCommand, recvCqHandle), 32);
    EXPECT_EQ(offsetof(CreateQpCommand, maxSendWr), 40);
    EXPECT_EQ(offsetof(CreateQpCommand, maxRecvWr), 44);
    EXPECT_EQ(offsetof(CreateQpCommand, accessFlags), 64);
    EXPECT_EQ(offsetof(CreateQpCommand, totalChunks), 68);
    EXPECT_EQ(offsetof(CreateQpCommand, sendChunks), 70);
    EXPECT_EQ(offsetof(CreateQpCommand, maxAtomicArgument), 72);
    EXPECT_EQ(offsetof(CreateQpCommand, signalAllSendWr), 74);
    EXPECT_EQ(offsetof(CreateQpCommand, qpType), 75);
    EXPECT_EQ(offsetof(CreateQpCommand, isSrq), 76);
    EXPECT_EQ(offsetof(CreateQpCommand, reserved), 77);
    EXPECT_EQ(sizeof(CreateQpResponseV2), 48);
    EXPECT_EQ(offsetof(CreateQpResponseV2, qpn), 16);
    EXPECT_EQ(offsetof(CreateQpResponseV2, qpHandle), 20);
    EXPECT_EQ(offsetof(CreateQpResponseV2, maxSendWr), 24);
    EXPECT_EQ(offsetof(CreateQpResponseV2, maxInlineData), 40);
    EXPECT_EQ(sizeof(ModifyQpCommand), 184);
    EXPECT_EQ(offsetof(ModifyQpCommand, qpHandle), 16);
    EXPECT_EQ(offsetof(ModifyQpCommand, attributeMask), 20);
    EXPECT_EQ(offsetof(ModifyQpCommand, attributes), 24);
    EXPECT_EQ(sizeof(QueryQpCommand), 24);
    EXPECT_EQ(offsetof(QueryQpCommand, qpHandle), 16);
    EXPECT_EQ(offsetof(QueryQpCommand, attributeMask), 20);
    EXPECT_EQ(sizeof(QueryQpResponse), 176);
    EXPECT_EQ(offsetof(QueryQpResponse, attributes), 16);
    EXPECT_EQ(sizeof(DestroyQpCommand), 24);
    EXPECT_EQ(offsetof(DestroyQpCommand, qpHandle), 16);
    EXPECT_EQ(offsetof(DestroyQpCommand, reserved), 20);
    EXPECT_EQ(sizeof(DestroyQpResponse), 24);
    EXPECT_EQ(offsetof(DestroyQpResponse, eventsReported), 16);
    EXPECT_EQ(sizeof(CreateBindCommand), 48);
    EXPECT_EQ(sizeof(DestroyBindCommand), 40);
    EXPECT_EQ(sizeof(CommandRequest), 184);
    EXPECT_EQ(sizeof(CommandResponse), 176);

    EXPECT_EQ(static_cast<uint32_t>(Command::CreateQp), 9);
    EXPECT_EQ(responseCommand(Command::QueryPort), 0x80000000);
    EXPECT_EQ(responseCommand(Command::DestroyQp), 0x8000000c);
}

TEST(PvrdmaAbiTest, RcDataPathLayout)
{
    EXPECT_EQ(sizeof(PortAttr), 48);
    EXPECT_EQ(offsetof(PortAttr, pkeyTableLength), 32);
    EXPECT_EQ(sizeof(AddressHandleAttr), 40);
    EXPECT_EQ(sizeof(QpAttr), 160);
    EXPECT_EQ(offsetof(QpAttr, capabilities), 56);
    EXPECT_EQ(offsetof(QpAttr, addressHandle), 80);

    EXPECT_EQ(sizeof(Sge), 16);
    EXPECT_EQ(offsetof(Sge, lkey), 12);
    EXPECT_EQ(sizeof(ReceiveWqeHeader), 16);
    EXPECT_EQ(sizeof(SendWqeHeader), 80);
    EXPECT_EQ(offsetof(SendWqeHeader, opcode), 16);
    EXPECT_EQ(offsetof(SendWqeHeader, operationData), 32);
    EXPECT_EQ(sizeof(CompletionQueueElement), 64);
    EXPECT_EQ(offsetof(CompletionQueueElement, opcode), 16);
    EXPECT_EQ(offsetof(CompletionQueueElement, sourceMac), 51);

    EXPECT_TRUE(SupportsSendRecv);
    EXPECT_FALSE(SupportsSrq);
    EXPECT_FALSE(SupportsAh);
    EXPECT_FALSE(SupportsRdmaRead);
    EXPECT_FALSE(SupportsRdmaWrite);
    EXPECT_FALSE(SupportsAtomics);
}

TEST(PvrdmaRingTest, Empty)
{
    constexpr uint32_t entries = 8;
    uint32_t index = 99;

    EXPECT_TRUE(ringIndexValid(0, entries));
    EXPECT_EQ(ringIndex(0, entries), 0);
    EXPECT_EQ(ringHasData(0, 0, entries, index), 0);
    EXPECT_EQ(index, 0);
    EXPECT_EQ(ringHasSpace(0, 0, entries, index), 1);
    EXPECT_EQ(index, 0);
    EXPECT_TRUE(ringEmpty(0, 0, entries));
    EXPECT_FALSE(ringFull(0, 0, entries));
    EXPECT_FALSE(ringHasData(0, 0, entries));
    EXPECT_TRUE(ringHasSpace(0, 0, entries));
}

TEST(PvrdmaRingTest, NonEmpty)
{
    constexpr uint32_t entries = 8;

    EXPECT_FALSE(ringEmpty(1, 0, entries));
    EXPECT_TRUE(ringHasData(1, 0, entries));
    EXPECT_TRUE(ringHasSpace(1, 0, entries));
    EXPECT_EQ(ringSlot(0, entries), 0);
}

TEST(PvrdmaRingTest, FullUsesOppositeGeneration)
{
    constexpr uint32_t entries = 8;
    uint32_t tail = 99;

    EXPECT_TRUE(ringFull(8, 0, entries));
    EXPECT_TRUE(ringHasData(8, 0, entries));
    EXPECT_FALSE(ringHasSpace(8, 0, entries));
    EXPECT_EQ(ringHasSpace(8, 0, entries, tail), 0);
    EXPECT_EQ(tail, 0);
    EXPECT_EQ(ringSlot(8, entries), 0);
}

TEST(PvrdmaRingTest, WrapFlipsGeneration)
{
    constexpr uint32_t entries = 8;

    EXPECT_EQ(ringAdvance(7, entries), 8);
    EXPECT_EQ(ringAdvance(15, entries), 0);
    EXPECT_EQ(ringSlot(15, entries), 7);
    EXPECT_TRUE(ringHasData(0, 15, entries));
    EXPECT_TRUE(ringHasSpace(0, 15, entries));
    EXPECT_EQ(ringSlot(15, entries), 7);
}

TEST(PvrdmaRingTest, SupportsDeviceNotificationRingSizes)
{
    for (const uint32_t entries : {3072U, 1536U}) {
        uint32_t index = 99;

        EXPECT_TRUE(ringSizeValid(entries));
        EXPECT_TRUE(ringIndexValid(0, entries));
        EXPECT_EQ(ringHasSpace(0, 0, entries, index), 1);
        EXPECT_EQ(index, 0);
        EXPECT_EQ(ringHasData(1, 0, entries, index), 1);
        EXPECT_EQ(index, 0);
    }
}

TEST(PvrdmaRingTest, RejectsMalformedIndicesAndSizes)
{
    uint32_t index = 99;

    EXPECT_FALSE(ringSizeValid(0));
    EXPECT_TRUE(ringSizeValid(7));
    EXPECT_FALSE(ringIndexValid(16, 8));
    EXPECT_EQ(ringIndex(16, 8), InvalidRingIndex);
    EXPECT_EQ(ringHasData(16, 0, 8, index), InvalidRingIndex);
    EXPECT_EQ(index, 99);
    EXPECT_FALSE(ringHasData(16, 0, 8));
    EXPECT_EQ(ringHasSpace(0, 0, 0, index), InvalidRingIndex);
}

} // namespace pvrdma
} // namespace gem5
