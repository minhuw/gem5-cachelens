/*
 * Copyright (c) 2026 minhuw
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "mem/request.hh"
#include "mem/ruby/system/DMASequencerUtils.hh"

namespace gem5::ruby
{

namespace
{

RequestPtr
makeRequest(Addr address, Request::Flags flags)
{
    auto request = std::make_shared<Request>();
    request->setPaddr(address);
    request->setFlags(flags);
    return request;
}

std::vector<bool>
allEnabled(int size)
{
    return std::vector<bool>(size, true);
}

void
expectMask(const WriteMask &mask, const std::vector<int> &enabled)
{
    EXPECT_EQ(mask.count(), enabled.size());
    for (int byte = 0; byte < mask.getBlockSize(); ++byte) {
        const bool expected =
            std::find(enabled.begin(), enabled.end(), byte) != enabled.end();
        EXPECT_EQ(mask.test(byte), expected) << "byte " << byte;
    }
}

void
expectContiguousMask(const WriteMask &mask, int offset, int len)
{
    std::vector<int> enabled;
    for (int byte = offset; byte < offset + len; ++byte)
        enabled.push_back(byte);
    expectMask(mask, enabled);
}

} // anonymous namespace

TEST(DMASequencerFragmentTest, SplitsCrossLineAccessExactly)
{
    constexpr Addr Start = 0x103c;
    constexpr int Length = 70;
    constexpr int BlockSize = 64;
    auto request = makeRequest(Start, Request::NIC_RX_PAYLOAD_WRITE);
    const auto byte_enable = allEnabled(Length);

    auto first = makeDMARequestFragment(
        Start, Length, 0, BlockSize, request, byte_enable);
    EXPECT_EQ(first.physicalAddress, Start);
    EXPECT_EQ(first.lineAddress, 0x1000);
    EXPECT_EQ(first.requestOffset, 0);
    EXPECT_EQ(first.len, 4);
    EXPECT_EQ(first.seqReq, request);
    expectContiguousMask(first.accessMask, 60, 4);

    auto middle = makeDMARequestFragment(
        Start, Length, first.len, BlockSize, request, byte_enable);
    EXPECT_EQ(middle.physicalAddress, 0x1040);
    EXPECT_EQ(middle.lineAddress, 0x1040);
    EXPECT_EQ(middle.requestOffset, 4);
    EXPECT_EQ(middle.len, BlockSize);
    EXPECT_EQ(middle.seqReq, request);
    EXPECT_TRUE(middle.accessMask.isFull());

    auto last = makeDMARequestFragment(
        Start, Length, first.len + middle.len, BlockSize, request,
        byte_enable);
    EXPECT_EQ(last.physicalAddress, 0x1080);
    EXPECT_EQ(last.lineAddress, 0x1080);
    EXPECT_EQ(last.requestOffset, 68);
    EXPECT_EQ(last.len, 2);
    EXPECT_EQ(last.seqReq, request);
    expectContiguousMask(last.accessMask, 0, 2);
}

TEST(DMASequencerFragmentTest, IntersectsSparseMaskWithEachLineExtent)
{
    constexpr Addr Start = 0x103c;
    constexpr int Length = 72;
    constexpr int BlockSize = 64;
    std::vector<bool> byte_enable(Length, false);
    for (const int byte : {0, 3, 68, 71})
        byte_enable[byte] = true;

    auto first = makeDMARequestFragment(
        Start, Length, 0, BlockSize, nullptr, byte_enable);
    EXPECT_TRUE(first.hasEnabledBytes());
    expectMask(first.accessMask, {60, 63});

    auto middle = makeDMARequestFragment(
        Start, Length, 4, BlockSize, nullptr, byte_enable);
    EXPECT_FALSE(middle.hasEnabledBytes());
    EXPECT_TRUE(middle.accessMask.isEmpty());

    auto last = makeDMARequestFragment(
        Start, Length, 68, BlockSize, nullptr, byte_enable);
    EXPECT_TRUE(last.hasEnabledBytes());
    expectMask(last.accessMask, {0, 3});
}

TEST(DMASequencerFragmentTest, MapsCrossLineSparseDataToPhysicalOffsets)
{
    constexpr Addr Start = 0x103e;
    constexpr int Length = 8;
    constexpr int BlockSize = 64;
    const std::vector<uint8_t> data = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    };
    const std::vector<bool> byte_enable = {
        true, false, false, true, false, false, false, true,
    };

    auto first = makeDMARequestFragment(
        Start, Length, 0, BlockSize, nullptr, byte_enable);
    EXPECT_EQ(first.lineOffset(), 62);
    EXPECT_EQ(first.requestOffsetForLineByte(62), 0);
    EXPECT_EQ(first.requestOffsetForLineByte(63), 1);
    EXPECT_EQ(data[first.requestOffsetForLineByte(62)], 0xa0);
    EXPECT_EQ(data[first.requestOffsetForLineByte(63)], 0xa1);
    expectMask(first.accessMask, {62});

    auto second = makeDMARequestFragment(
        Start, Length, first.len, BlockSize, nullptr, byte_enable);
    EXPECT_EQ(second.lineOffset(), 0);
    for (int byte = 0; byte < second.len; ++byte) {
        EXPECT_EQ(second.requestOffsetForLineByte(byte), first.len + byte);
        EXPECT_EQ(data[second.requestOffsetForLineByte(byte)],
                  data[first.len + byte]);
    }
    expectMask(second.accessMask, {1, 5});
}

TEST(DMASequencerFragmentTest, PreservesNicAndGenericProvenance)
{
    constexpr Addr Address = 0x2003;
    constexpr int Length = 80;
    constexpr int BlockSize = 64;
    const auto byte_enable = allEnabled(Length);

    auto payload = makeRequest(Address, Request::NIC_RX_PAYLOAD_WRITE);
    auto payload_fragment = makeDMARequestFragment(
        Address, Length, 61, BlockSize, payload, byte_enable);
    ASSERT_EQ(payload_fragment.seqReq, payload);
    EXPECT_TRUE(payload_fragment.seqReq->isNicRxPayloadWrite());

    auto header = makeRequest(Address, Request::NIC_RX_HEADER_WRITE);
    auto header_fragment = makeDMARequestFragment(
        Address, Length, 61, BlockSize, header, byte_enable);
    ASSERT_EQ(header_fragment.seqReq, header);
    EXPECT_TRUE(header_fragment.seqReq->isNicRxHeaderWrite());

    auto tx_payload = makeRequest(Address, Request::NIC_TX_PAYLOAD_READ);
    auto tx_fragment = makeDMARequestFragment(
        Address, Length, 61, BlockSize, tx_payload, byte_enable);
    ASSERT_EQ(tx_fragment.seqReq, tx_payload);
    EXPECT_TRUE(tx_fragment.seqReq->isNicTxPayloadRead());

    auto descriptor = makeRequest(Address, Request::NIC_TX_DESC_WRITEBACK);
    auto descriptor_fragment = makeDMARequestFragment(
        Address, Length, 61, BlockSize, descriptor, byte_enable);
    ASSERT_EQ(descriptor_fragment.seqReq, descriptor);
    EXPECT_TRUE(descriptor_fragment.seqReq->isNicDescDma());

    auto generic = makeRequest(Address, Request::UNCACHEABLE);
    auto generic_fragment = makeDMARequestFragment(
        Address, Length, 61, BlockSize, generic, byte_enable);
    ASSERT_EQ(generic_fragment.seqReq, generic);
    EXPECT_FALSE(generic_fragment.seqReq->isNicDmaRead());
    EXPECT_FALSE(generic_fragment.seqReq->isNicDmaWrite());

    auto no_provenance = makeDMARequestFragment(
        Address, Length, 0, BlockSize, nullptr, byte_enable);
    EXPECT_EQ(no_provenance.seqReq, nullptr);
    expectContiguousMask(no_provenance.accessMask, 3, 61);
}

TEST(DMASequencerReservationTest, MapsEveryLineAndRejectsLaterOverlap)
{
    constexpr int BlockSize = 64;
    DMARequestLineReservations reservations;

    ASSERT_TRUE(reservations.tryReserve(0x103c, 70, BlockSize));
    EXPECT_EQ(reservations.size(), 3);

    Addr request_address = 0;
    for (const Addr line : {0x1000, 0x1040, 0x1080}) {
        ASSERT_TRUE(reservations.findRequest(line, request_address));
        EXPECT_EQ(request_address, 0x1000);
    }

    EXPECT_FALSE(reservations.tryReserve(0x1040, BlockSize, BlockSize));
    EXPECT_EQ(reservations.size(), 3);
    ASSERT_TRUE(reservations.findRequest(0x1040, request_address));
    EXPECT_EQ(request_address, 0x1000);
}

TEST(DMASequencerReservationTest, ReservesOnlySparseEnabledLines)
{
    constexpr int BlockSize = 64;
    constexpr Addr Start = 0x103c;
    constexpr int Length = 72;
    std::vector<bool> byte_enable(Length, false);
    byte_enable[0] = true;
    byte_enable[71] = true;
    DMARequestLineReservations reservations;
    Addr request_address = 0;

    ASSERT_TRUE(reservations.tryReserve(
        Start, Length, BlockSize, byte_enable, request_address));
    EXPECT_EQ(request_address, 0x1000);
    EXPECT_EQ(reservations.size(), 2);

    Addr owner = 0;
    EXPECT_TRUE(reservations.findRequest(0x1000, owner));
    EXPECT_EQ(owner, request_address);
    EXPECT_FALSE(reservations.findRequest(0x1040, owner));
    EXPECT_TRUE(reservations.findRequest(0x1080, owner));
    EXPECT_EQ(owner, request_address);

    // The all-disabled middle line remains available to an independent DMA.
    EXPECT_TRUE(reservations.tryReserve(0x1040, BlockSize, BlockSize));
    EXPECT_EQ(reservations.size(), 3);
}

TEST(DMASequencerReservationTest, RejectsMultiLineRequestAtomically)
{
    constexpr int BlockSize = 64;
    DMARequestLineReservations reservations;

    ASSERT_TRUE(reservations.tryReserve(0x1040, BlockSize, BlockSize));
    EXPECT_FALSE(reservations.tryReserve(0x103c, 70, BlockSize));
    EXPECT_EQ(reservations.size(), 1);

    Addr request_address = 0;
    EXPECT_FALSE(reservations.findRequest(0x1000, request_address));
    ASSERT_TRUE(reservations.findRequest(0x1040, request_address));
    EXPECT_EQ(request_address, 0x1040);
    EXPECT_FALSE(reservations.findRequest(0x1080, request_address));
}

TEST(DMASequencerReservationTest, CleanupPreservesNonOverlapAndAllowsReuse)
{
    constexpr int BlockSize = 64;
    DMARequestLineReservations reservations;

    ASSERT_TRUE(reservations.tryReserve(0x103c, 70, BlockSize));
    ASSERT_TRUE(reservations.tryReserve(0x10c0, BlockSize, BlockSize));
    EXPECT_EQ(reservations.size(), 4);

    ASSERT_TRUE(reservations.release(0x1000));
    EXPECT_EQ(reservations.size(), 1);
    EXPECT_FALSE(reservations.release(0x1000));

    Addr request_address = 0;
    EXPECT_FALSE(reservations.findRequest(0x1000, request_address));
    EXPECT_FALSE(reservations.findRequest(0x1040, request_address));
    EXPECT_FALSE(reservations.findRequest(0x1080, request_address));
    ASSERT_TRUE(reservations.findRequest(0x10c0, request_address));
    EXPECT_EQ(request_address, 0x10c0);

    ASSERT_TRUE(reservations.tryReserve(0x1040, BlockSize, BlockSize));
    ASSERT_TRUE(reservations.findRequest(0x1040, request_address));
    EXPECT_EQ(request_address, 0x1040);
    EXPECT_EQ(reservations.size(), 2);
}

TEST(DMASequencerFragmentTest, MaskedWritesRequireProtocolCapability)
{
    EXPECT_TRUE(isSupportedDMARequest(false, false));
    EXPECT_TRUE(isSupportedDMARequest(false, true));
    EXPECT_FALSE(isSupportedDMARequest(true, false));
    EXPECT_TRUE(isSupportedDMARequest(true, true));
}

TEST(DMASequencerFragmentTest, DetectsZeroEnabledWrites)
{
    EXPECT_TRUE(hasEnabledDMABytes({false, true, false}));
    EXPECT_FALSE(hasEnabledDMABytes({false, false, false}));
}

} // namespace gem5::ruby
