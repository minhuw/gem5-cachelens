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

TEST(IGbETxLifecycle, RebaseCursorTracksNonzeroHead)
{
    EXPECT_EQ(0, igbeDescCacheRebaseCursor(0, 8));
    EXPECT_EQ(5, igbeDescCacheRebaseCursor(5, 8));
    EXPECT_EQ(1, igbeDescCacheRebaseCursor(9, 8));
    EXPECT_EQ(0, igbeDescCacheRebaseCursor(7, 0));
}

TEST(IGbETxLifecycle, QuickAccountingChecksLocalAlignment)
{
    const IGbETxLifecycleCounts counts{
        .posted = 3, .cached = 2, .inFlight = 1, .complete = 2};
    EXPECT_TRUE(igbeTxLifecycleQuickValid(counts, 3, 2, 3, 2));
    EXPECT_FALSE(igbeTxLifecycleQuickValid(counts, 2, 2, 3, 2));
    EXPECT_FALSE(igbeTxLifecycleQuickValid(counts, 3, 2, 2, 2));
    EXPECT_FALSE(igbeTxLifecycleQuickValid(counts, 3, 1, 3, 2));
}

TEST(IGbETxLifecycle, StrictRestoreLayoutAcceptsWraparound)
{
    std::vector<IGbETxDescState> states(
        8, IGbETxDescState::Uninitialized);
    states[6] = IGbETxDescState::CompleteNotVisible;
    states[7] = IGbETxDescState::PayloadDmaInFlight;
    states[0] = IGbETxDescState::NicDescCached;
    states[1] = IGbETxDescState::HostPosted;
    states[2] = IGbETxDescState::HostPosted;

    EXPECT_TRUE(igbeTxRestoreLayoutValid(
        states, {7, 0}, {6}, 6, 3, 1, 8));
}

TEST(IGbETxLifecycle, StrictRestoreLayoutRejectsInvalidIndicesAndOrder)
{
    std::vector<IGbETxDescState> states(
        8, IGbETxDescState::Uninitialized);
    states[6] = IGbETxDescState::CompleteNotVisible;
    states[7] = IGbETxDescState::NicDescCached;
    states[0] = IGbETxDescState::NicDescCached;
    states[1] = IGbETxDescState::HostPosted;
    states[2] = IGbETxDescState::HostPosted;

    EXPECT_FALSE(igbeTxRestoreLayoutValid(
        states, {7, 7}, {6}, 6, 3, 1, 8));
    EXPECT_FALSE(igbeTxRestoreLayoutValid(
        states, {7, 8}, {6}, 6, 3, 1, 8));
    EXPECT_FALSE(igbeTxRestoreLayoutValid(
        states, {0, 7}, {6}, 6, 3, 1, 8));
    EXPECT_FALSE(igbeTxRestoreLayoutValid(
        states, {7, 0}, {7}, 6, 3, 1, 8));

    states[7] = IGbETxDescState::CompleteNotVisible;
    EXPECT_FALSE(igbeTxRestoreLayoutValid(
        states, {7, 0}, {6}, 6, 3, 1, 8));
}

TEST(IGbETxLifecycle, StrictRestoreAllowsOnlyFrontUnusedPayloadDma)
{
    std::vector<IGbETxDescState> states(
        4, IGbETxDescState::Uninitialized);
    states[1] = IGbETxDescState::PayloadDmaInFlight;
    states[2] = IGbETxDescState::NicDescCached;
    EXPECT_TRUE(igbeTxRestoreLayoutValid(
        states, {1, 2}, {}, 1, 3, 3, 4));

    states[1] = IGbETxDescState::NicDescCached;
    states[2] = IGbETxDescState::PayloadDmaInFlight;
    EXPECT_FALSE(igbeTxRestoreLayoutValid(
        states, {1, 2}, {}, 1, 3, 3, 4));
}

TEST(IGbETxLifecycle, PublicationWaitSamplesOnlyOncePerGeneration)
{
    EXPECT_TRUE(igbeTxPostedWaitShouldSample(true, false));
    EXPECT_FALSE(igbeTxPostedWaitShouldSample(true, true));
    EXPECT_FALSE(igbeTxPostedWaitShouldSample(false, false));
}

TEST(IGbETxLifecycle, PublicationTelemetryPreservesUnknownLegacyTick)
{
    EXPECT_TRUE(igbeTxPublicationTelemetryValid(
        IGbETxDescState::HostPosted, 0, false, false, 100));
    EXPECT_TRUE(igbeTxPublicationTelemetryValid(
        IGbETxDescState::NicDescCached, 0, false, true, 100));
    EXPECT_TRUE(igbeTxPublicationTelemetryValid(
        IGbETxDescState::PayloadDmaInFlight, 100, true, true, 100));
    EXPECT_FALSE(igbeTxPublicationTelemetryValid(
        IGbETxDescState::HostPosted, 101, true, false, 100));
    EXPECT_FALSE(igbeTxPublicationTelemetryValid(
        IGbETxDescState::HostPosted, 1, false, false, 100));
    EXPECT_FALSE(igbeTxPublicationTelemetryValid(
        IGbETxDescState::Uninitialized, 0, false, true, 100));
}

TEST(IGbETxLifecycle, PayloadPredicateIgnoresDrainedTsoHistory)
{
    IGbETxPayloadRestoreState stale_history;
    stale_history.tsoLoadedHeader = true;
    stale_history.tsoDescBytesUsed = 128;
    stale_history.tsoCopyBytes = 64;
    EXPECT_FALSE(igbeTxPayloadWorkLive(stale_history));

    stale_history.tsoActive = true;
    EXPECT_TRUE(igbeTxPayloadWorkLive(stale_history));
}

TEST(IGbETxLifecycle, ActiveOccupancyUsesExclusiveTail)
{
    EXPECT_EQ(0, igbeDescRingOccupancy(3, 3, 8));
    EXPECT_EQ(4, igbeDescRingOccupancy(2, 6, 8));
    EXPECT_EQ(3, igbeDescRingOccupancy(6, 1, 8));
    EXPECT_EQ(7, igbeDescRingOccupancy(1, 0, 8));
    EXPECT_EQ(0, igbeDescRingOccupancy(0, 0, 1));
}

TEST(IGbETxLifecycle, PublicationRangeHandlesWraparound)
{
    EXPECT_EQ((std::vector<uint32_t>{2, 3, 4}),
              igbeDescRingRange(2, 5, 8));
    EXPECT_EQ((std::vector<uint32_t>{6, 7, 0}),
              igbeDescRingRange(6, 1, 8));
    EXPECT_TRUE(igbeDescRingRange(4, 4, 8).empty());
}

TEST(IGbETxLifecycle, FetchAndWritebackIndicesMoveConsecutively)
{
    EXPECT_EQ((std::vector<uint32_t>{5, 6, 7, 0}),
              igbeDescRingIndices(5, 4, 8));
    EXPECT_EQ((std::vector<uint32_t>{7, 0}),
              igbeDescRingIndices(7, 2, 8));
}

TEST(IGbETxLifecycle, AggregateAccountingIncludesDmaInUnusedDeque)
{
    const IGbETxLifecycleCounts counts{
        .posted = 3, .cached = 2, .inFlight = 1, .complete = 2};
    EXPECT_TRUE(igbeTxLifecycleAccountingValid(counts, 8, 3, 2, 8));
    EXPECT_FALSE(igbeTxLifecycleAccountingValid(counts, 7, 3, 2, 8));
    EXPECT_FALSE(igbeTxLifecycleAccountingValid(counts, 8, 2, 2, 8));
    EXPECT_FALSE(igbeTxLifecycleAccountingValid(counts, 8, 3, 1, 8));
    EXPECT_FALSE(igbeTxLifecycleAccountingValid(counts, 8, 3, 2, 7));
}

TEST(IGbETxLifecycle, LegacyRestoreAcceptsOnlyEmptyTxState)
{
    const IGbELegacyTxRestoreState empty;
    EXPECT_TRUE(igbeLegacyTxRestoreQuiescent(empty));

    auto active = empty;
    active.ringOccupancy = 1;
    EXPECT_FALSE(igbeLegacyTxRestoreQuiescent(active));

    auto cached = empty;
    cached.descriptorsUnused = 1;
    EXPECT_FALSE(igbeLegacyTxRestoreQuiescent(cached));

    auto fifo = empty;
    fifo.fifoPackets = 1;
    fifo.fifoBytes = 64;
    EXPECT_FALSE(igbeLegacyTxRestoreQuiescent(fifo));
}

TEST(IGbETxLifecycle, LegacyRestoreRejectsHiddenTxWork)
{
    IGbELegacyTxRestoreState state;

    state.descriptorsFetching = 1;
    EXPECT_FALSE(igbeLegacyTxRestoreQuiescent(state));
    state.descriptorsFetching = 0;

    state.descriptorEvents = true;
    EXPECT_FALSE(igbeLegacyTxRestoreQuiescent(state));
    state.descriptorEvents = false;

    state.fifoReserved = 64;
    EXPECT_FALSE(igbeLegacyTxRestoreQuiescent(state));
    state.fifoReserved = 0;

    state.currentPacketBytes = 64;
    EXPECT_FALSE(igbeLegacyTxRestoreQuiescent(state));
    state.currentPacketBytes = 0;

    state.payloadWork = true;
    EXPECT_FALSE(igbeLegacyTxRestoreQuiescent(state));
}

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
