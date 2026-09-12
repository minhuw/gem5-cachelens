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
#include <sstream>

#include "mem/cache/replacement_policies/lru_rp.hh"
#include "mem/ruby/structures/CacheMemory.hh"
#include "mem/ruby/system/RubyPort.hh"
#include "params/LRURP.hh"
#include "params/RubyCache.hh"

namespace gem5::ruby
{

// CacheRecorder is linked because CacheMemory exposes checkpoint recording,
// but these unit tests never instantiate a RubyPort or exercise its functional
// write path.
bool
RubyPort::functionalWriteToRubySystem(Packet *)
{
    return false;
}

class TestEntry : public AbstractCacheEntry
{
  public:
    void print(std::ostream &out) const override { out << m_Address; }
};

class CacheMemoryTest : public testing::Test
{
  protected:
    static constexpr Addr SameSetStride = 256;

    Tick tick = 0;
    std::unique_ptr<CacheMemory> cache;

    void SetUp() override { Gem5Internal::_curTickPtr = &tick; }
    void TearDown() override
    {
        cache.reset();
        Gem5Internal::_curTickPtr = nullptr;
    }

    void
    makeCache(int ddioWays, bool addrHash = false)
    {
        LRURPParams lruParams;
        lruParams.eventq_index = 0;
        auto *lru = new replacement_policy::LRU(lruParams);
        RubyCacheParams params;
        params.name = "cache_memory_test";
        params.eventq_index = 0;
        params.size = 1024;
        params.assoc = 4;
        params.block_size = 64;
        params.start_index_bit = 6;
        params.replacement_policy = lru;
        params.ddio_way_part = ddioWays;
        params.addr_hash = addrHash;
        cache = std::make_unique<CacheMemory>(params);
        cache->init();
    }

    Addr
    sameSetAddress(Addr address, unsigned tagOffset = 1) const
    {
        const Addr other = address + tagOffset * SameSetStride;
        EXPECT_EQ(cache->addressToCacheSet(address),
                  cache->addressToCacheSet(other));
        return other;
    }

    int setOf(Addr address) const { return cache->addressToCacheSet(address); }
    int wayOf(Addr address) const { return cache->m_tag_index.at(address); }
    AbstractCacheEntry *entryAt(Addr address, int way) const
    {
        return cache->m_cache[cache->addressToCacheSet(address)][way];
    }
    void setNextWay(int set, int way) { cache->m_next_invalid_way[set] = way; }
    size_t tagCount(Addr address) const { return cache->m_tag_index.count(address); }
    Counter payloadAllocAt(int way) const
    {
        return cache->cacheMemoryStats.rxPayloadAllocWays[way].value();
    }
    Counter ddioAllocAt(int way) const
    {
        return cache->cacheMemoryStats.ddioAllocWays[way].value();
    }
    Counter payloadRequests() const
    {
        return cache->cacheMemoryStats.rxPayloadRequests.value();
    }
    Counter headerRequests() const
    {
        return cache->cacheMemoryStats.rxHeaderRequests.value();
    }
    Counter payloadMisses() const
    {
        return cache->cacheMemoryStats.rxPayloadMisses.value();
    }
    Counter headerMisses() const
    {
        return cache->cacheMemoryStats.rxHeaderMisses.value();
    }
    void setSlot(int set, int way, AbstractCacheEntry *entry)
    {
        cache->m_cache[set][way] = entry;
    }
    void setTag(Addr address, int way) { cache->m_tag_index[address] = way; }
};

TEST_F(CacheMemoryTest, ReuseRemovesOnlyStaleTagMapping)
{
    makeCache(2);
    constexpr Addr oldAddress = 64;
    const Addr replacementAddress = sameSetAddress(oldAddress);
    auto *old = new TestEntry;
    cache->allocateInWays(oldAddress, old, 2);
    old->m_Permission = AccessPermission_NotPresent;
    auto *replacement = new TestEntry;
    setNextWay(setOf(oldAddress), wayOf(oldAddress));
    cache->allocateInWays(replacementAddress, replacement, 2);
    EXPECT_EQ(tagCount(oldAddress), 0);
    // allocateInWays reclaims the invalid placeholder.

    // A stale same-address placeholder must not erase a newer mapping.
    constexpr Addr placeholderAddress = 0;
    const Addr otherAddress = sameSetAddress(placeholderAddress);
    auto *stale = new TestEntry;
    stale->m_Address = placeholderAddress;
    stale->m_Permission = AccessPermission_NotPresent;
    setSlot(0, 0, stale);
    auto *newer = new TestEntry;
    newer->m_Address = placeholderAddress;
    newer->m_Permission = AccessPermission_Invalid;
    setSlot(0, 1, newer);
    setTag(placeholderAddress, 1);
    setNextWay(0, 0);
    auto *other = new TestEntry;
    cache->allocateInWays(otherAddress, other, 2);
    EXPECT_EQ(wayOf(placeholderAddress), 1);
}

TEST_F(CacheMemoryTest, AllocateReusesSlotAndRemovesStaleTagMapping)
{
    makeCache(2);
    constexpr Addr oldAddress = 0;
    const Addr replacementAddress = sameSetAddress(oldAddress);
    auto *old = new TestEntry;
    cache->allocate(oldAddress, old);
    const int oldWay = wayOf(oldAddress);
    old->m_Permission = AccessPermission_NotPresent;
    setNextWay(setOf(oldAddress), oldWay);

    auto *replacement = new TestEntry;
    cache->allocate(replacementAddress, replacement);
    EXPECT_EQ(wayOf(replacementAddress), oldWay);
    EXPECT_EQ(tagCount(oldAddress), 0);
    delete old;
}

TEST_F(CacheMemoryTest, SameAddressPlaceholderOutsideSubsetMigrates)
{
    makeCache(2);
    setNextWay(0, 3);
    auto *placeholder = new TestEntry;
    cache->allocate(0, placeholder);
    ASSERT_GE(wayOf(0), 2);
    placeholder->m_Permission = AccessPermission_NotPresent;

    auto *replacement = new TestEntry;
    EXPECT_TRUE(cache->cacheAvailInWays(0, 2));
    cache->allocateInWays(0, replacement, 2);
    EXPECT_LT(wayOf(0), 2);
    EXPECT_EQ(cache->lookup(0), replacement);
}

TEST_F(CacheMemoryTest, DDIOInvalidAllocationsRotate)
{
    makeCache(2);
    constexpr Addr firstAddress = 0;
    const Addr secondAddress = sameSetAddress(firstAddress);
    auto *first = new TestEntry;
    cache->allocateInWays(firstAddress, first, 2);
    auto *second = new TestEntry;
    cache->allocateInWays(secondAddress, second, 2);
    EXPECT_EQ(wayOf(firstAddress), 0);
    EXPECT_EQ(wayOf(secondAddress), 1);
}

TEST_F(CacheMemoryTest, LookupRejectsInvalidStaleTagIndex)
{
    makeCache(2);
    constexpr Addr address = 0;
    setTag(address, 0);
    EXPECT_EQ(cache->lookup(address), nullptr);

    setTag(address, 4);
    EXPECT_EQ(cache->lookup(address), nullptr);
}

TEST_F(CacheMemoryTest, DisabledDDIOUsesReplacementPolicySelection)
{
    makeCache(-1);
    setNextWay(0, 3);
    auto *entry = new TestEntry;
    cache->allocate(0, entry);
    // LRU's equal-tick tie behavior is the pre-DDIO candidate-order choice.
    EXPECT_EQ(wayOf(0), 0);
}

TEST_F(CacheMemoryTest, AddressHashAvalanchesFullLineAddress)
{
    makeCache(2, true);
    // Four sets: these are the stable low bits of SplitMix64 for line
    // addresses 0 through 7. Contiguous lines are deliberately non-linear.
    constexpr int expected[] = {3, 1, 2, 1, 2, 2, 0, 3};
    for (int line = 0; line < 8; ++line)
        EXPECT_EQ(setOf(line * 64), expected[line]);
}

TEST_F(CacheMemoryTest, RxHeaderIsClassifiedAsDDIOWrite)
{
    makeCache(2);
    auto header = std::make_shared<Request>(
        0, 64, Request::NIC_RX_HEADER_WRITE, 0);
    auto payload = std::make_shared<Request>(
        0, 64, Request::NIC_RX_PAYLOAD_WRITE, 0);
    auto unrelated = std::make_shared<Request>(0, 64, Request::UNCACHEABLE, 0);
    auto descriptor = std::make_shared<Request>(
        0, 64, Request::NIC_RX_DESC_WRITEBACK, 0);
    EXPECT_TRUE(cache->isNicRxWriteReq(header));
    EXPECT_TRUE(cache->isNicRxWriteReq(payload));
    EXPECT_TRUE(cache->isNicDdioWriteReq(descriptor));
    EXPECT_FALSE(cache->isNicDdioReadReq(descriptor));
    EXPECT_FALSE(cache->isNicDdioWriteReq(unrelated));
}

TEST_F(CacheMemoryTest, PayloadAndHeaderTelemetryAreSeparate)
{
    makeCache(2);
    cache->profileRxPayload(0);
    cache->profileRxHeader(64);
    EXPECT_EQ(payloadRequests(), 1);
    EXPECT_EQ(headerRequests(), 1);
    EXPECT_EQ(payloadMisses(), 1);
    EXPECT_EQ(headerMisses(), 1);
}

TEST_F(CacheMemoryTest, PayloadAllocationHistogramIsPayloadOnly)
{
    makeCache(2);
    auto *entry = new TestEntry;
    cache->allocateInWays(0, entry, 2);
    cache->profileDdioWayFill(2, wayOf(0), 0);
    // The descriptor source updates general DDIO accounting, not payload-only
    // allocation accounting. The source-agnostic way is checked indirectly by
    // exercising the source-specific request classification above.
    EXPECT_EQ(payloadAllocAt(wayOf(0)), 0);
    EXPECT_EQ(ddioAllocAt(wayOf(0)), 1);
}

TEST_F(CacheMemoryTest, DDIOWritePreparationPreservesSnoopData)
{
    makeCache(2);
    EXPECT_FALSE(cache->ddioWriteNeedsRead(true, true));  // dirty owner data
    EXPECT_TRUE(cache->ddioWriteNeedsRead(true, false)); // clean/no owner
    EXPECT_FALSE(cache->ddioWriteNeedsRead(false, false)); // full write
}

} // namespace gem5::ruby
