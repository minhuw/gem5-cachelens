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

#include "base/gtest/cur_tick_fake.hh"
#include "mem/request.hh"

using namespace gem5;

GTestTickHandler tickHandler;

TEST(RequestNicFlags, LeafCategories)
{
    Request rx_desc(0x1000, 64, Request::NIC_RX_DESC_READ, 0);
    EXPECT_TRUE(rx_desc.isNicRxDescRead());
    EXPECT_TRUE(rx_desc.isNicDescDma());
    EXPECT_FALSE(rx_desc.isNicPayloadDma());
    EXPECT_TRUE(Request::isValidNicDmaReadFlags(
        Request::NIC_RX_DESC_READ));
    EXPECT_FALSE(Request::isValidNicDmaWriteFlags(
        Request::NIC_RX_DESC_READ));

    Request rx_payload(0x1000, 64, Request::NIC_RX_PAYLOAD_WRITE, 0);
    EXPECT_TRUE(rx_payload.isNicRxPayloadWrite());
    EXPECT_TRUE(rx_payload.isNicPayloadDma());
    EXPECT_FALSE(rx_payload.isNicDescDma());
    EXPECT_TRUE(Request::isValidNicDmaWriteFlags(
        Request::NIC_RX_PAYLOAD_WRITE));
    EXPECT_FALSE(Request::isValidNicDmaReadFlags(
        Request::NIC_RX_PAYLOAD_WRITE));

    Request rx_wb(0x1000, 64, Request::NIC_RX_DESC_WRITEBACK, 0);
    EXPECT_TRUE(rx_wb.isNicRxDescWriteback());
    EXPECT_TRUE(rx_wb.isNicDescDma());

    Request tx_desc(0x1000, 64, Request::NIC_TX_DESC_READ, 0);
    EXPECT_TRUE(tx_desc.isNicTxDescRead());
    EXPECT_TRUE(tx_desc.isNicDescDma());

    Request tx_payload(0x1000, 64, Request::NIC_TX_PAYLOAD_READ, 0);
    EXPECT_TRUE(tx_payload.isNicTxPayloadRead());
    EXPECT_TRUE(tx_payload.isNicPayloadDma());
    EXPECT_FALSE(tx_payload.isReadModifyWrite());

    Request tx_wb(0x1000, 64, Request::NIC_TX_DESC_WRITEBACK, 0);
    EXPECT_TRUE(tx_wb.isNicTxDescWriteback());
    EXPECT_TRUE(tx_wb.isNicDescDma());
    EXPECT_TRUE(tx_wb.isNicDmaWrite());
    EXPECT_FALSE(tx_wb.isNicDmaRead());

    EXPECT_TRUE(rx_desc.isNicDmaRead());
    EXPECT_FALSE(rx_desc.isNicDmaWrite());

    Request rx_header(0x1000, 64, Request::NIC_RX_HEADER_WRITE, 0);
    EXPECT_TRUE(rx_header.isNicRxHeaderWrite());
    EXPECT_TRUE(rx_header.isNicPayloadDma());
}

TEST(RequestNicFlags, CombinedAndUnrelatedCategories)
{
    Request combined(
        0x1000, 64,
        Request::NIC_RX_DESC_READ | Request::NIC_TX_PAYLOAD_READ, 0);
    EXPECT_TRUE(combined.isNicDescDma());
    EXPECT_TRUE(combined.isNicPayloadDma());
    EXPECT_FALSE(Request::hasOneNicDmaCategory(combined.getFlags()));
    EXPECT_FALSE(Request::isValidNicDmaReadFlags(combined.getFlags()));

    Request unrelated(0x1000, 64, Request::UNCACHEABLE, 0);
    EXPECT_FALSE(unrelated.isNicDescDma());
    EXPECT_FALSE(unrelated.isNicPayloadDma());
    EXPECT_FALSE(Request::hasOneNicDmaCategory(unrelated.getFlags()));

    EXPECT_TRUE(Request::isValidNicDmaReadFlags(
        Request::NIC_TX_PAYLOAD_READ | Request::UNCACHEABLE));

    Request rmw(0x1000, 64, Request::READ_MODIFY_WRITE, 0);
    EXPECT_TRUE(rmw.isReadModifyWrite());
    EXPECT_FALSE(rmw.isNicDescDma());
    EXPECT_FALSE(rmw.isNicPayloadDma());

    Request copied(combined);
    EXPECT_TRUE(copied.isNicRxDescRead());
    EXPECT_TRUE(copied.isNicTxPayloadRead());
}
