// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2012-2016 VMware, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __DEV_RDMA_PVRDMA_RING_HH__
#define __DEV_RDMA_PVRDMA_RING_HH__

#include <cstdint>

namespace gem5
{
namespace pvrdma
{

inline constexpr int32_t InvalidRingIndex = -1;

constexpr bool
ringSizeValid(uint32_t entries)
{
    return entries && entries <= (uint32_t{1} << 30);
}

constexpr bool
ringIndexValid(uint32_t index, uint32_t entries)
{
    return ringSizeValid(entries) &&
           !(index & ~((entries << 1) - 1));
}

constexpr bool
ringIndicesValid(uint32_t producerTail, uint32_t consumerHead,
                 uint32_t entries)
{
    return ringIndexValid(producerTail, entries) &&
           ringIndexValid(consumerHead, entries);
}

constexpr uint32_t
ringSlot(uint32_t index, uint32_t entries)
{
    return index & (entries - 1);
}

constexpr int32_t
ringIndex(uint32_t index, uint32_t entries)
{
    return ringIndexValid(index, entries) ? ringSlot(index, entries) :
                                            InvalidRingIndex;
}

constexpr uint32_t
ringAdvance(uint32_t index, uint32_t entries)
{
    return (index + 1) & ((entries << 1) - 1);
}

constexpr int32_t
ringHasData(uint32_t producerTail, uint32_t consumerHead, uint32_t entries,
            uint32_t &head)
{
    if (!ringIndicesValid(producerTail, consumerHead, entries))
        return InvalidRingIndex;

    head = ringSlot(consumerHead, entries);
    return producerTail != consumerHead;
}

constexpr int32_t
ringHasSpace(uint32_t producerTail, uint32_t consumerHead, uint32_t entries,
             uint32_t &tail)
{
    if (!ringIndicesValid(producerTail, consumerHead, entries))
        return InvalidRingIndex;

    tail = ringSlot(producerTail, entries);
    return producerTail != (consumerHead ^ entries);
}

constexpr bool
ringEmpty(uint32_t producerTail, uint32_t consumerHead, uint32_t entries)
{
    return ringIndicesValid(producerTail, consumerHead, entries) &&
           producerTail == consumerHead;
}

constexpr bool
ringFull(uint32_t producerTail, uint32_t consumerHead, uint32_t entries)
{
    return ringIndicesValid(producerTail, consumerHead, entries) &&
           producerTail == (consumerHead ^ entries);
}

constexpr bool
ringHasData(uint32_t producerTail, uint32_t consumerHead, uint32_t entries)
{
    return ringIndicesValid(producerTail, consumerHead, entries) &&
           producerTail != consumerHead;
}

constexpr bool
ringHasSpace(uint32_t producerTail, uint32_t consumerHead, uint32_t entries)
{
    return ringIndicesValid(producerTail, consumerHead, entries) &&
           producerTail != (consumerHead ^ entries);
}

} // namespace pvrdma
} // namespace gem5

#endif // __DEV_RDMA_PVRDMA_RING_HH__
