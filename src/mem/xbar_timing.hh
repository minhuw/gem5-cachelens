/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_XBAR_TIMING_HH__
#define __MEM_XBAR_TIMING_HH__

#include <algorithm>
#include <cstdint>

#include "base/intmath.hh"
#include "base/types.hh"
#include "mem/packet.hh"

namespace gem5
{

/** Timing calculations shared by crossbar implementations and unit tests. */
class XBarTiming
{
  public:
    static void
    annotatePacket(PacketPtr pkt, Tick clock_offset, Tick header_delay,
                   Tick clock_period, uint32_t width,
                   bool timing_transparent)
    {
        if (timing_transparent)
            return;

        pkt->headerDelay += clock_offset + header_delay;
        if (pkt->hasData()) {
            pkt->payloadDelay = std::max<Tick>(
                pkt->payloadDelay,
                divCeil(pkt->getSize(), width) * clock_period);
        }
    }

    static Tick
    layerReleaseTick(bool timing_transparent, Tick now,
                     Tick packet_finish_time)
    {
        return timing_transparent ? now + 1 : packet_finish_time;
    }
};

} // namespace gem5

#endif // __MEM_XBAR_TIMING_HH__
