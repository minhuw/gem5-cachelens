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

#include <fstream>
#include <iterator>
#include <memory>

#include "base/gtest/serialization_fixture.hh"
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

class CacheMemoryTest : public SerializationFixture
{
  protected:
    static constexpr Addr SameSetStride = 256;

    Tick tick = 0;
    std::unique_ptr<CacheMemory> cache;

    void SetUp() override
    {
        SerializationFixture::SetUp();
        Gem5Internal::_curTickPtr = &tick;
    }
    void TearDown() override
    {
        cache.reset();
        Gem5Internal::_curTickPtr = nullptr;
        SerializationFixture::TearDown();
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
    Counter dmaRoutingProxies() const
    {
        return cache->cacheMemoryStats.dmaRoutingProxyRequests.value();
    }
    Counter dmaRoutingRecycles() const
    {
        return cache->cacheMemoryStats.dmaRoutingTransientRecycles.value();
    }
    Counter rdmaRxRequests() const
    {
        return cache->cacheMemoryStats.rdmaRxPayloadRequests.value();
    }
    Counter rdmaRxHits() const
    {
        return cache->cacheMemoryStats.rdmaRxPayloadHits.value();
    }
    Counter rdmaTxRequests() const
    {
        return cache->cacheMemoryStats.rdmaTxPayloadRequests.value();
    }
    Counter rdmaCpuAccessAt(int way) const
    {
        return cache->cacheMemoryStats.rdmaRxPayloadCpuAccessWays[way].value();
    }
    Counter rdmaCpuFillAt(int way) const
    {
        return cache->cacheMemoryStats.rdmaRxPayloadCpuFillWays[way].value();
    }
    Counter rdmaCpuUnique() const
    {
        return cache->cacheMemoryStats.rdmaRxPayloadCpuUniqueLines.value();
    }
    void setSlot(int set, int way, AbstractCacheEntry *entry)
    {
        cache->m_cache[set][way] = entry;
    }
    void setTag(Addr address, int way) { cache->m_tag_index[address] = way; }
    size_t rxProvenanceCount() const
    {
        return cache->rxPayloadEverAddrs.size();
    }
    size_t rdmaProvenanceCount() const
    {
        return cache->rdmaRxPayloadEverAddrs.size();
    }
    void saveCheckpoint() const
    {
        std::ofstream cp(getCptPath());
        Serializable::ScopedCheckpointSection section(cp, "cache");
        cache->serialize(cp);
    }
    void restoreCheckpoint()
    {
        CheckpointIn cp(getDirName());
        Serializable::ScopedCheckpointSection section(cp, "cache");
        cache->unserialize(cp);
    }
    std::string checkpointContents() const
    {
        std::ifstream cp(getCptPath());
        return {std::istreambuf_iterator<char>(cp), {}};
    }
};

using CacheMemoryDeathTest = CacheMemoryTest;

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

TEST_F(CacheMemoryTest, DDIOSubsetProbeUsesLRUAndReusesFreedWay)
{
    makeCache(2);
    constexpr Addr firstAddress = 0;
    const Addr secondAddress = sameSetAddress(firstAddress, 1);
    const Addr outsideAddress0 = sameSetAddress(firstAddress, 2);
    const Addr outsideAddress1 = sameSetAddress(firstAddress, 3);
    const Addr incomingAddress = sameSetAddress(firstAddress, 4);

    // Seed globally older lines outside the DDIO subset. A full-set probe
    // would choose one of these lines instead of either subset entry.
    setNextWay(setOf(firstAddress), 2);
    tick = 1;
    auto *outside0 = new TestEntry;
    cache->allocate(outsideAddress0, outside0);
    outside0->m_Permission = AccessPermission_Read_Write;
    tick = 2;
    auto *outside1 = new TestEntry;
    cache->allocate(outsideAddress1, outside1);
    outside1->m_Permission = AccessPermission_Read_Write;
    ASSERT_EQ(wayOf(outsideAddress0), 2);
    ASSERT_EQ(wayOf(outsideAddress1), 3);

    EXPECT_TRUE(cache->cacheAvailInWays(firstAddress, 2));
    tick = 3;
    auto *first = new TestEntry;
    cache->allocateInWays(firstAddress, first, 2);
    first->m_Permission = AccessPermission_Read_Write;
    EXPECT_EQ(wayOf(firstAddress), 0);

    EXPECT_TRUE(cache->cacheAvailInWays(secondAddress, 2));
    tick = 4;
    auto *second = new TestEntry;
    cache->allocateInWays(secondAddress, second, 2);
    second->m_Permission = AccessPermission_Read_Write;
    EXPECT_EQ(wayOf(secondAddress), 1);
    EXPECT_FALSE(cache->cacheAvailInWays(incomingAddress, 2));

    // Explicitly make way 0 most-recently used, so way 1 is the subset LRU.
    tick = 10;
    cache->setMRU(secondAddress);
    tick = 11;
    cache->setMRU(firstAddress);

    const Addr victim = cache->cacheProbeInWays(incomingAddress, 2);
    EXPECT_EQ(victim, secondAddress);
    EXPECT_LT(wayOf(victim), 2);
    EXPECT_NE(victim, outsideAddress0);
    EXPECT_NE(victim, outsideAddress1);

    cache->deallocate(victim);
    EXPECT_TRUE(cache->cacheAvailInWays(incomingAddress, 2));
    tick = 13;
    auto *incoming = new TestEntry;
    cache->allocateInWays(incomingAddress, incoming, 2);
    incoming->m_Permission = AccessPermission_Read_Write;
    EXPECT_EQ(wayOf(incomingAddress), 1);
    EXPECT_EQ(tagCount(secondAddress), 0);
    EXPECT_EQ(wayOf(outsideAddress0), 2);
    EXPECT_EQ(wayOf(outsideAddress1), 3);
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

TEST_F(CacheMemoryTest, PvrdmaProfilesAggregateAndDedicatedTelemetry)
{
    makeCache(2);
    auto *entry = new TestEntry;
    cache->allocateInWays(0, entry, 2);
    entry->m_Permission = AccessPermission_Read_Write;
    const int way = wayOf(0);

    auto rdma = std::make_shared<Request>(
        0, 64, Request::NIC_RX_PAYLOAD_WRITE | Request::NIC_PVRDMA, 0);
    auto generic = std::make_shared<Request>(
        0, 64, Request::NIC_RX_PAYLOAD_WRITE, 0);
    EXPECT_TRUE(cache->isNicRxPayloadWriteReq(rdma));
    EXPECT_TRUE(cache->isNicPvrdmaReq(rdma));
    EXPECT_FALSE(cache->isNicPvrdmaReq(generic));

    cache->profileRxPayload(0);
    cache->profileRdmaRxPayload(0);
    cache->profileTxPayload(0);
    cache->profileRdmaTxPayload(0);
    EXPECT_EQ(payloadRequests(), 1);
    EXPECT_EQ(rdmaRxRequests(), 1);
    EXPECT_EQ(rdmaRxHits(), 1);
    EXPECT_EQ(rdmaTxRequests(), 1);

    cache->profileDdioWayAccess(3, way, 0);
    EXPECT_EQ(rdmaCpuAccessAt(way), 1);
    EXPECT_EQ(rdmaCpuUnique(), 1);

    cache->resetStats();
    cache->profileDdioWayFill(3, way, 0);
    EXPECT_EQ(rdmaCpuAccessAt(way), 0);
    EXPECT_EQ(rdmaCpuFillAt(way), 1);
    EXPECT_EQ(rdmaCpuUnique(), 1);
}

TEST_F(CacheMemoryTest, PvrdmaProvenanceSurvivesCheckpointAndStatsReset)
{
    makeCache(2);
    constexpr Addr address = 256;
    cache->profileRxPayload(address);
    cache->profileRdmaRxPayload(address);
    saveCheckpoint();

    makeCache(2);
    restoreCheckpoint();

    cache->resetStats();
    cache->profileDdioWayAccess(3, 0, address);
    cache->profileDdioWayFill(3, 1, address);
    EXPECT_EQ(rdmaCpuAccessAt(0), 1);
    EXPECT_EQ(rdmaCpuFillAt(1), 1);
    EXPECT_EQ(rdmaCpuUnique(), 1);
}

TEST_F(CacheMemoryTest, ProvenanceSerializationIsSorted)
{
    makeCache(2);
    cache->profileRxPayload(256);
    cache->profileRdmaRxPayload(192);
    cache->profileRxPayload(64);
    cache->profileRdmaRxPayload(128);
    saveCheckpoint();

    EXPECT_EQ(checkpointContents(),
              "\n[cache]\n"
              "rxPayloadEverAddrs=64 128 192 256\n"
              "rdmaRxPayloadEverAddrs=128 192\n");
}

TEST_F(CacheMemoryTest, MissingProvenanceRestoresEmpty)
{
    makeCache(2);
    cache->profileRdmaRxPayload(64);
    simulateSerialization("\n[cache]\n");
    restoreCheckpoint();

    EXPECT_EQ(rxProvenanceCount(), 0);
    EXPECT_EQ(rdmaProvenanceCount(), 0);
}

TEST_F(CacheMemoryDeathTest, RejectsUnalignedRxProvenance)
{
    makeCache(2);
    simulateSerialization("\n[cache]\nrxPayloadEverAddrs=65\n");
    ASSERT_ANY_THROW(restoreCheckpoint());
}

TEST_F(CacheMemoryDeathTest, RejectsUnalignedRdmaProvenance)
{
    makeCache(2);
    simulateSerialization(
        "\n[cache]\nrxPayloadEverAddrs=64\nrdmaRxPayloadEverAddrs=65\n");
    ASSERT_ANY_THROW(restoreCheckpoint());
}

TEST_F(CacheMemoryDeathTest, RejectsRdmaOutsideRxProvenance)
{
    makeCache(2);
    simulateSerialization(
        "\n[cache]\nrxPayloadEverAddrs=64\nrdmaRxPayloadEverAddrs=128\n");
    ASSERT_ANY_THROW(restoreCheckpoint());
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

TEST_F(CacheMemoryTest, DMARoutingTelemetryIsIndependentAndResettable)
{
    makeCache(2);
    cache->profileDmaRoutingProxy();
    cache->profileDmaRoutingProxy();
    cache->profileDmaRoutingTransientRecycle();

    EXPECT_EQ(dmaRoutingProxies(), 2);
    EXPECT_EQ(dmaRoutingRecycles(), 1);
    EXPECT_EQ(payloadRequests(), 0);
    EXPECT_EQ(headerRequests(), 0);

    cache->resetStats();
    EXPECT_EQ(dmaRoutingProxies(), 0);
    EXPECT_EQ(dmaRoutingRecycles(), 0);
}

TEST_F(CacheMemoryTest, DDIOWritePreparationPreservesSnoopData)
{
    makeCache(2);
    EXPECT_FALSE(cache->ddioWriteNeedsRead(true, true));  // dirty owner data
    EXPECT_TRUE(cache->ddioWriteNeedsRead(true, false)); // clean/no owner
    EXPECT_FALSE(cache->ddioWriteNeedsRead(false, false)); // full write
}

} // namespace gem5::ruby
