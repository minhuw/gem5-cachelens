/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __DEV_NET_I8254XGBE_RX_DESC_HH__
#define __DEV_NET_I8254XGBE_RX_DESC_HH__

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace gem5
{

enum class IGbETxDescState
{
    Uninitialized,
    HostPosted,
    NicDescCached,
    PayloadDmaInFlight,
    CompleteNotVisible
};

/** Publication wait is sampled at most once for each descriptor generation. */
inline bool
igbeTxPostedWaitShouldSample(bool posted_tick_valid, bool already_sampled)
{
    return posted_tick_valid && !already_sampled;
}

/** Validate publication telemetry, including unknown version-1 timestamps. */
inline bool
igbeTxPublicationTelemetryValid(IGbETxDescState state, uint64_t posted_tick,
                                bool posted_tick_valid, bool wait_sampled,
                                uint64_t current_tick)
{
    if (posted_tick_valid && posted_tick > current_tick)
        return false;
    if (!posted_tick_valid && posted_tick != 0)
        return false;
    if (state == IGbETxDescState::Uninitialized &&
        (posted_tick_valid || wait_sampled)) {
        return false;
    }
    return true;
}

struct IGbETxLifecycleCounts
{
    uint32_t posted = 0;
    uint32_t cached = 0;
    uint32_t inFlight = 0;
    uint32_t complete = 0;

    uint32_t
    active() const
    {
        return posted + cached + inFlight + complete;
    }
};

/** Normalize a descriptor-cache cursor to the current ring head. */
inline uint32_t
igbeDescCacheRebaseCursor(uint32_t head, uint32_t length)
{
    return length == 0 ? 0 : head % length;
}

/** Active entries in an exclusive-tail descriptor ring. */
inline uint32_t
igbeDescRingOccupancy(uint32_t head, uint32_t tail, uint32_t length)
{
    if (length <= 1)
        return 0;
    return (tail + length - head) % length;
}

/** Indices in the half-open ring interval [first, last). */
inline std::vector<uint32_t>
igbeDescRingRange(uint32_t first, uint32_t last, uint32_t length)
{
    std::vector<uint32_t> indices;
    if (length <= 1)
        return indices;

    first %= length;
    last %= length;
    for (uint32_t idx = first; idx != last; idx = (idx + 1) % length)
        indices.push_back(idx);
    return indices;
}

/** Consecutive ring indices used by fetch and writeback completions. */
inline std::vector<uint32_t>
igbeDescRingIndices(uint32_t first, uint32_t count, uint32_t length)
{
    std::vector<uint32_t> indices;
    if (length == 0)
        return indices;

    indices.reserve(count);
    for (uint32_t offset = 0; offset < count; ++offset)
        indices.push_back((first + offset) % length);
    return indices;
}

/** O(1) accounting check for cache/deque-aligned lifecycle boundaries. */
inline bool
igbeTxLifecycleQuickValid(const IGbETxLifecycleCounts &counts,
                          uint32_t unused_indices, uint32_t used_indices,
                          uint32_t descriptors_unused,
                          uint32_t descriptors_used)
{
    return counts.cached + counts.inFlight == unused_indices &&
        counts.complete == used_indices &&
        unused_indices == descriptors_unused &&
        used_indices == descriptors_used;
}

/**
 * Strictly validate serialized tracker deques and their ring ordering.
 *
 * Used descriptors start at TDH. Unused descriptors immediately follow them,
 * and cachePnt is the first descriptor not represented by either cache deque.
 * At most the front unused descriptor may have a payload DMA in flight.
 */
inline bool
igbeTxRestoreLayoutValid(const std::vector<IGbETxDescState> &states,
                         const std::deque<int> &unused_indices,
                         const std::deque<int> &used_indices,
                         uint32_t head, uint32_t tail, uint32_t cache_pnt,
                         uint32_t length)
{
    if (states.size() != length)
        return false;
    if (length == 0) {
        return head == 0 && tail == 0 && cache_pnt == 0 &&
            unused_indices.empty() && used_indices.empty();
    }
    if (head >= length || tail >= length || cache_pnt >= length)
        return false;
    if (unused_indices.size() + used_indices.size() > length)
        return false;

    std::vector<bool> represented(length, false);
    uint32_t cursor = head;
    for (int idx : used_indices) {
        if (idx < 0 || static_cast<uint32_t>(idx) >= length ||
            static_cast<uint32_t>(idx) != cursor || represented[idx] ||
            states[idx] != IGbETxDescState::CompleteNotVisible) {
            return false;
        }
        represented[idx] = true;
        cursor = (cursor + 1) % length;
    }
    for (size_t pos = 0; pos < unused_indices.size(); ++pos) {
        const int idx = unused_indices[pos];
        if (idx < 0 || static_cast<uint32_t>(idx) >= length ||
            static_cast<uint32_t>(idx) != cursor || represented[idx]) {
            return false;
        }
        const auto state = states[idx];
        if (state != IGbETxDescState::NicDescCached &&
            !(pos == 0 &&
              state == IGbETxDescState::PayloadDmaInFlight)) {
            return false;
        }
        represented[idx] = true;
        cursor = (cursor + 1) % length;
    }
    if (cursor != cache_pnt)
        return false;

    for (uint32_t idx = cache_pnt; idx != tail;
         idx = (idx + 1) % length) {
        if (represented[idx] || states[idx] != IGbETxDescState::HostPosted)
            return false;
        represented[idx] = true;
    }
    for (uint32_t idx = 0; idx < length; ++idx) {
        if (!represented[idx] &&
            states[idx] != IGbETxDescState::Uninitialized) {
            return false;
        }
    }
    return true;
}

/** Pure aggregate check shared by integration assertions and unit tests. */
inline bool
igbeTxLifecycleAccountingValid(const IGbETxLifecycleCounts &counts,
                               uint32_t initialized,
                               uint32_t unused_indices,
                               uint32_t used_indices,
                               uint32_t ring_occupancy)
{
    return counts.active() == initialized &&
        counts.cached + counts.inFlight == unused_indices &&
        counts.complete == used_indices &&
        counts.active() == ring_occupancy;
}

struct IGbETxPayloadRestoreState
{
    bool packetDone = false;
    bool packetWaiting = false;
    bool packetMultiDesc = false;
    bool packetPointer = false;
    bool tsoActive = false;
    bool tsoPacketHasHeader = false;
    // Serialized scratch/history; intentionally ignored by the live predicate.
    bool tsoLoadedHeader = false;
    uint32_t tsoDescBytesUsed = 0;
    uint32_t tsoCopyBytes = 0;
};

/**
 * Return whether TX packet assembly has live work.
 *
 * Completed TSO history such as tsoLoadedHeader and tsoCopyBytes is
 * deliberately absent: those scratch values can remain stale after useTso is
 * cleared and do not make an otherwise drained checkpoint active.
 */
inline bool
igbeTxPayloadWorkLive(const IGbETxPayloadRestoreState &state)
{
    return state.packetDone || state.packetWaiting || state.packetMultiDesc ||
        state.packetPointer || state.tsoActive || state.tsoPacketHasHeader;
}

/**
 * Return whether a pre-TX-lifecycle checkpoint is safe to reconstruct.
 *
 * With no serialized descriptor indices or states, only a completely empty
 * TX path is unambiguous. FIFO bytes and reservations are checked in addition
 * to packet count so a partially assembled packet cannot look empty.
 */
struct IGbELegacyTxRestoreState
{
    uint32_t ringOccupancy = 0;
    uint32_t descriptorsUsed = 0;
    uint32_t descriptorsUnused = 0;
    int descriptorsFetching = 0;
    int descriptorsWritingBack = 0;
    bool descriptorEvents = false;
    uint32_t fifoPackets = 0;
    uint32_t fifoBytes = 0;
    uint32_t fifoReserved = 0;
    uint32_t currentPacketBytes = 0;
    bool payloadWork = false;
};

inline bool
igbeLegacyTxRestoreQuiescent(const IGbELegacyTxRestoreState &state)
{
    return state.ringOccupancy == 0 && state.descriptorsUsed == 0 &&
        state.descriptorsUnused == 0 && state.descriptorsFetching == 0 &&
        state.descriptorsWritingBack == 0 && !state.descriptorEvents &&
        state.fifoPackets == 0 && state.fifoBytes == 0 &&
        state.fifoReserved == 0 && state.currentPacketBytes == 0 &&
        !state.payloadWork;
}

struct IGbERxDescWriteback
{
    bool required;
    uint32_t descriptorCount;
    uint32_t argument;
};

/**
 * Return whether an active DMA must be followed by another writeback.
 *
 * A later flush can request a less restrictive alignment, or descriptors can
 * complete after the active DMA count was fixed. Both cases require another
 * pass after the in-flight writeback completes.
 */
inline bool
igbeDescWritebackNeedsFollowup(size_t descriptors_used,
                               size_t descriptors_in_flight,
                               uint64_t requested_alignment,
                               uint64_t active_alignment)
{
    return descriptors_used > descriptors_in_flight ||
        requested_alignment < active_alignment;
}

/** Return whether completed descriptors prevent another cache fetch. */
inline bool
igbeRxDescCacheBlocked(uint32_t descriptors_used,
                       uint32_t descriptors_unused)
{
    return descriptors_used != 0 && descriptors_unused == 0;
}

/**
 * Select an RX descriptor writeback after a completed descriptor.
 *
 * WTHRESH controls batching while receive traffic remains queued. An idle
 * FIFO, exhausted ring, or descriptor cache with no reusable entries flushes
 * all completed descriptors so software and the fetch path can make progress.
 * A zero WTHRESH also requests immediate writeback.
 *
 * DescCache::writeback(0) writes every completed descriptor. A non-zero
 * argument is an alignment mask and may round the writeback count down, so a
 * threshold-triggered writeback must use zero even when WTHRESH is not a
 * power of two.
 */
inline IGbERxDescWriteback
igbeRxDescWriteback(uint32_t wthresh, uint32_t descriptors_used,
                    uint32_t descriptors_unused, bool fifo_idle,
                    bool ring_exhausted)
{
    const bool flush = wthresh == 0 || fifo_idle || ring_exhausted ||
        igbeRxDescCacheBlocked(descriptors_used, descriptors_unused);
    if (!flush && descriptors_used < wthresh)
        return {false, 0, 0};

    return {true, descriptors_used, 0};
}

} // namespace gem5

#endif // __DEV_NET_I8254XGBE_RX_DESC_HH__
