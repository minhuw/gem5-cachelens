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

namespace gem5
{

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
