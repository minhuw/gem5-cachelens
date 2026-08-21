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

#include <memory>

#include "base/gtest/cur_tick_fake.hh"
#include "mem/request.hh"
#include "mem/ruby/system/DMASequencerUtils.hh"

namespace gem5::ruby
{

GTestTickHandler tickHandler;

namespace
{

void
expectContiguousMask(const WriteMask &mask, int offset, int len)
{
    EXPECT_EQ(mask.count(), len);
    for (int byte = 0; byte < mask.getBlockSize(); ++byte) {
        EXPECT_EQ(mask.test(byte), byte >= offset && byte < offset + len)
            << "byte " << byte;
    }
}

} // anonymous namespace

TEST(DMASequencerFragmentTest, SplitsCrossLineAccessExactly)
{
    constexpr Addr Start = 0x103c;
    constexpr int Length = 70;
    constexpr int BlockSize = 64;
    auto request = std::make_shared<Request>(
        Start, Length, Request::NIC_RX_PAYLOAD_WRITE, 0);

    auto first = makeDMARequestFragment(Start, Length, 0, BlockSize, request);
    EXPECT_EQ(first.physicalAddress, Start);
    EXPECT_EQ(first.lineAddress, 0x1000);
    EXPECT_EQ(first.len, 4);
    EXPECT_EQ(first.seqReq, request);
    expectContiguousMask(first.accessMask, 60, 4);

    auto middle = makeDMARequestFragment(
        Start, Length, first.len, BlockSize, request);
    EXPECT_EQ(middle.physicalAddress, 0x1040);
    EXPECT_EQ(middle.lineAddress, 0x1040);
    EXPECT_EQ(middle.len, BlockSize);
    EXPECT_EQ(middle.seqReq, request);
    EXPECT_TRUE(middle.accessMask.isFull());

    auto last = makeDMARequestFragment(
        Start, Length, first.len + middle.len, BlockSize, request);
    EXPECT_EQ(last.physicalAddress, 0x1080);
    EXPECT_EQ(last.lineAddress, 0x1080);
    EXPECT_EQ(last.len, 2);
    EXPECT_EQ(last.seqReq, request);
    expectContiguousMask(last.accessMask, 0, 2);
}

TEST(DMASequencerFragmentTest, PreservesNicAndGenericProvenance)
{
    constexpr Addr Address = 0x2003;
    constexpr int Length = 80;
    constexpr int BlockSize = 64;

    auto payload = std::make_shared<Request>(
        Address, Length, Request::NIC_RX_PAYLOAD_WRITE, 0);
    auto payload_fragment = makeDMARequestFragment(
        Address, Length, 61, BlockSize, payload);
    ASSERT_EQ(payload_fragment.seqReq, payload);
    EXPECT_TRUE(payload_fragment.seqReq->isNicRxPayloadWrite());

    auto header = std::make_shared<Request>(
        Address, Length, Request::NIC_RX_HEADER_WRITE, 0);
    auto header_fragment = makeDMARequestFragment(
        Address, Length, 61, BlockSize, header);
    ASSERT_EQ(header_fragment.seqReq, header);
    EXPECT_TRUE(header_fragment.seqReq->isNicRxHeaderWrite());

    auto tx_payload = std::make_shared<Request>(
        Address, Length, Request::NIC_TX_PAYLOAD_READ, 0);
    auto tx_fragment = makeDMARequestFragment(
        Address, Length, 61, BlockSize, tx_payload);
    ASSERT_EQ(tx_fragment.seqReq, tx_payload);
    EXPECT_TRUE(tx_fragment.seqReq->isNicTxPayloadRead());

    auto descriptor = std::make_shared<Request>(
        Address, Length, Request::NIC_TX_DESC_WRITEBACK, 0);
    auto descriptor_fragment = makeDMARequestFragment(
        Address, Length, 61, BlockSize, descriptor);
    ASSERT_EQ(descriptor_fragment.seqReq, descriptor);
    EXPECT_TRUE(descriptor_fragment.seqReq->isNicDescDma());

    auto generic = std::make_shared<Request>(
        Address, Length, Request::UNCACHEABLE, 0);
    auto generic_fragment = makeDMARequestFragment(
        Address, Length, 61, BlockSize, generic);
    ASSERT_EQ(generic_fragment.seqReq, generic);
    EXPECT_FALSE(generic_fragment.seqReq->isNicDmaRead());
    EXPECT_FALSE(generic_fragment.seqReq->isNicDmaWrite());

    auto no_provenance = makeDMARequestFragment(
        Address, Length, 0, BlockSize, nullptr);
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

    // This request's first line is a later fragment of the active request.
    EXPECT_FALSE(reservations.tryReserve(0x1040, BlockSize, BlockSize));
    EXPECT_EQ(reservations.size(), 3);
    ASSERT_TRUE(reservations.findRequest(0x1040, request_address));
    EXPECT_EQ(request_address, 0x1000);
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

TEST(DMASequencerFragmentTest, MaskedWritesRemainUnsupported)
{
    EXPECT_TRUE(isSupportedDMARequest(false));
    EXPECT_FALSE(isSupportedDMARequest(true));
}

} // namespace gem5::ruby
