/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include "dev/net/i8254xGBe_rx_desc.hh"

namespace gem5
{
namespace
{

TEST(IGbEDescWriteback, QueuesDescriptorsCompletedDuringActiveDma)
{
    EXPECT_FALSE(igbeDescWritebackNeedsFollowup(8, 8, 0, 0));
    EXPECT_TRUE(igbeDescWritebackNeedsFollowup(9, 8, 0, 0));
    EXPECT_TRUE(igbeDescWritebackNeedsFollowup(8, 8, 0, 3));
}

TEST(IGbERxDescWriteback, BatchesBusyReceiveTrafficToWthresh)
{
    constexpr uint32_t wthresh = 8;

    for (uint32_t used = 1; used < wthresh; ++used) {
        const auto writeback = igbeRxDescWriteback(
            wthresh, used, 1, false, false);
        EXPECT_FALSE(writeback.required) << "used=" << used;
    }

    const auto writeback = igbeRxDescWriteback(
        wthresh, wthresh, 1, false, false);
    EXPECT_TRUE(writeback.required);
    EXPECT_EQ(wthresh, writeback.descriptorCount);
    EXPECT_EQ(0, writeback.argument);
}

TEST(IGbERxDescWriteback, WritesEveryDescriptorAtNonPowerOfTwoThresholds)
{
    for (const uint32_t wthresh : {3U, 6U}) {
        const auto writeback = igbeRxDescWriteback(
            wthresh, wthresh, 1, false, false);

        EXPECT_TRUE(writeback.required) << "WTHRESH=" << wthresh;
        EXPECT_EQ(wthresh, writeback.descriptorCount)
            << "WTHRESH=" << wthresh;
        EXPECT_EQ(0, writeback.argument) << "WTHRESH=" << wthresh;
    }
}

TEST(IGbERxDescWriteback, KeepsPartialBatchUntilModeledFlush)
{
    constexpr uint32_t wthresh = 32;
    constexpr uint32_t used = 31;

    EXPECT_FALSE(
        igbeRxDescWriteback(wthresh, used, 1, false, false).required);
    EXPECT_TRUE(
        igbeRxDescWriteback(wthresh, used, 1, true, false).required);
    EXPECT_TRUE(
        igbeRxDescWriteback(wthresh, used, 1, false, true).required);
}

TEST(IGbERxDescWriteback, ZeroWthreshWritesBackImmediately)
{
    const auto writeback = igbeRxDescWriteback(
        0, 1, 1, false, false);

    EXPECT_TRUE(writeback.required);
    EXPECT_EQ(1, writeback.descriptorCount);
    EXPECT_EQ(0, writeback.argument);
}

TEST(IGbERxDescWriteback, FullCacheForcesProgressBelowWthresh)
{
    constexpr uint32_t wthresh = 6;
    constexpr uint32_t cache_capacity = 4;
    constexpr uint32_t packets = 10;
    uint32_t used = 0;
    uint32_t written = 0;
    uint32_t writebacks = 0;

    for (uint32_t packet = 0; packet < packets; ++packet) {
        ++used;
        ASSERT_LE(used, cache_capacity);
        const uint32_t unused = cache_capacity - used;
        EXPECT_EQ(used == cache_capacity,
                  igbeRxDescCacheBlocked(used, unused));
        const auto writeback = igbeRxDescWriteback(
            wthresh, used, unused, false, false);
        if (writeback.required) {
            EXPECT_EQ(used, writeback.descriptorCount);
            EXPECT_EQ(0, writeback.argument);
            written += writeback.descriptorCount;
            used -= writeback.descriptorCount;
            ++writebacks;
        }
    }

    EXPECT_EQ(packets, written + used);
    EXPECT_EQ(2, writebacks);
    EXPECT_LT(used, cache_capacity);
}

} // anonymous namespace
} // namespace gem5
