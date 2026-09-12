/*
 * Copyright (c) 2020-2021 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 1999-2012 Mark D. Hill and David A. Wood
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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

#include "mem/ruby/structures/CacheMemory.hh"

#include "base/compiler.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "debug/HtmMem.hh"
#include "debug/RubyCache.hh"
#include "debug/RubyCacheTrace.hh"
#include "debug/RubyResourceStalls.hh"
#include "debug/RubyStats.hh"
#include "mem/cache/replacement_policies/lru_rp.hh"
#include "mem/cache/replacement_policies/weighted_lru_rp.hh"
#include "mem/ruby/protocol/AccessPermission.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{

namespace ruby
{

std::ostream&
operator<<(std::ostream& out, const CacheMemory& obj)
{
    obj.print(out);
    out << std::flush;
    return out;
}

CacheMemory::CacheMemory(const Params &p)
    : SimObject(p),
    dataArray(p.dataArrayBanks, p.dataAccessLatency, p.start_index_bit),
    tagArray(p.tagArrayBanks, p.tagAccessLatency, p.start_index_bit),
    atomicALUArray(p.atomicALUs, p.atomicLatency),
    cacheMemoryStats(this)
{
    m_cache_size = p.size;
    m_cache_assoc = p.assoc;
    m_replacementPolicy_ptr = p.replacement_policy;
    m_start_index_bit = p.start_index_bit;
    m_is_instruction_only_cache = p.is_icache;
    m_resource_stalls = p.resourceStalls;
    m_block_size = p.block_size;  // may be 0 at this point. Updated in init()
    m_use_occupancy = dynamic_cast<replacement_policy::WeightedLRU*>(
                                    m_replacementPolicy_ptr) ? true : false;
    m_ddio_way_part = p.ddio_way_part;
    fatal_if(m_ddio_way_part != -1 &&
             (m_ddio_way_part < 1 || m_ddio_way_part > m_cache_assoc),
             "DDIO way partition must be -1 or in [1, %d], got %d",
             m_cache_assoc, m_ddio_way_part);
    fatal_if(m_ddio_way_part > 0 &&
             !dynamic_cast<replacement_policy::LRU*>(
                 m_replacementPolicy_ptr),
             "DDIO way partition requires the subset-safe LRU replacement "
             "policy");
    m_addr_hash = p.addr_hash;
}

void
CacheMemory::setRubySystem(RubySystem* rs)
{
    dataArray.setClockPeriod(rs->clockPeriod());
    tagArray.setClockPeriod(rs->clockPeriod());
    atomicALUArray.setClockPeriod(rs->clockPeriod());
    atomicALUArray.setBlockSize(rs->getBlockSizeBytes());

    if (m_block_size == 0) {
        m_block_size = rs->getBlockSizeBytes();
    }

    m_ruby_system = rs;
}

void
CacheMemory::init()
{
    assert(m_block_size != 0);
    m_cache_num_sets = (m_cache_size / m_cache_assoc) / m_block_size;
    assert(m_cache_num_sets > 1);
    m_cache_num_set_bits = floorLog2(m_cache_num_sets);
    assert(m_cache_num_set_bits > 0);

    m_cache.resize(m_cache_num_sets,
                    std::vector<AbstractCacheEntry*>(m_cache_assoc, nullptr));
    m_next_invalid_way.resize(m_cache_num_sets, 0);
    replacement_data.resize(m_cache_num_sets,
                               std::vector<ReplData>(m_cache_assoc, nullptr));

    // Size the DDIO way-histogram stats now that the associativity is known
    cacheMemoryStats.rxPayloadHitWays.init(m_cache_assoc).flags(
        statistics::total);
    cacheMemoryStats.rxPayloadAllocWays.init(m_cache_assoc).flags(
        statistics::total);
    cacheMemoryStats.ddioAllocWays.init(m_cache_assoc).flags(
        statistics::total);

    // Per-way, per-source accounting vectors ([src][way] flattened). Keep
    // zero-valued way entries visible so runtime contracts can distinguish a
    // real zero from a missing statistic.
    static const char *ddio_src_names[5] = {
        "nic_rx_payload", "nic_tx_payload", "nic_desc", "cpu_other",
        "nic_rx_header"};
    cacheMemoryStats.ddioWayAccess.init(5 * m_cache_assoc).flags(
        statistics::total);
    cacheMemoryStats.ddioWayFill.init(5 * m_cache_assoc).flags(
        statistics::total);
    cacheMemoryStats.wayDeallocations.init(m_cache_assoc).flags(
        statistics::total);
    cacheMemoryStats.rxPayloadCpuAccessWays.init(m_cache_assoc).flags(
        statistics::total);
    cacheMemoryStats.rxPayloadCpuFillWays.init(m_cache_assoc).flags(
        statistics::total);
    for (int s = 0; s < 5; s++) {
        for (int w = 0; w < m_cache_assoc; w++) {
            cacheMemoryStats.ddioWayAccess.subname(
                s * m_cache_assoc + w,
                csprintf("%s_way%d", ddio_src_names[s], w));
            cacheMemoryStats.ddioWayFill.subname(
                s * m_cache_assoc + w,
                csprintf("%s_way%d", ddio_src_names[s], w));
        }
    }
    for (int w = 0; w < m_cache_assoc; w++) {
        cacheMemoryStats.wayDeallocations.subname(w, csprintf("way%d", w));
        cacheMemoryStats.rxPayloadCpuAccessWays.subname(
            w, csprintf("way%d", w));
        cacheMemoryStats.rxPayloadCpuFillWays.subname(
            w, csprintf("way%d", w));
    }

    // Per-set payload access counters
    cacheMemoryStats.rxPayloadSetHits.init(m_cache_num_sets).flags(
        statistics::nozero | statistics::total);
    cacheMemoryStats.rxPayloadSetMisses.init(m_cache_num_sets).flags(
        statistics::nozero | statistics::total);
    // instantiate all the replacement_data here
    for (int i = 0; i < m_cache_num_sets; i++) {
        for ( int j = 0; j < m_cache_assoc; j++) {
            replacement_data[i][j] =
                                m_replacementPolicy_ptr->instantiateEntry();
        }
    }
}

CacheMemory::~CacheMemory()
{
    if (m_replacementPolicy_ptr)
        delete m_replacementPolicy_ptr;
    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            delete m_cache[i][j];
        }
    }
}

void
CacheMemory::resetStats()
{
    SimObject::resetStats();
    rxPayloadUniqueAddrs.clear();
    txPayloadUniqueAddrs.clear();
    rxPayloadCpuUniqueAddrs.clear();
}

// convert a Address to its location in the cache
int64_t
CacheMemory::addressToCacheSet(Addr address) const
{
    assert(address == makeLineAddress(address));
    Addr idx = bitSelect(address, m_start_index_bit,
                         m_start_index_bit + m_cache_num_set_bits - 1);
    if (m_addr_hash) {
        // SplitMix64's finalizer avalanches every input bit into the set
        // index. This decorrelates contiguous lines as well as allocator and
        // ring strides whose low or high address bits repeat periodically.
        uint64_t line = address >> m_start_index_bit;
        line += 0x9e3779b97f4a7c15ULL;
        line = (line ^ (line >> 30)) * 0xbf58476d1ce4e5b9ULL;
        line = (line ^ (line >> 27)) * 0x94d049bb133111ebULL;
        line ^= line >> 31;
        idx = line & (m_cache_num_sets - 1);
    }
    return idx;
}

// Given a cache index: returns the index of the tag in a set.
// returns -1 if the tag is not found.
int
CacheMemory::findTagInSet(int64_t cacheSet, Addr tag) const
{
    assert(tag == makeLineAddress(tag));
    // search the set for the tags
    auto it = m_tag_index.find(tag);
    if (it != m_tag_index.end() && it->second >= 0 &&
        it->second < m_cache_assoc) {
        AbstractCacheEntry* entry = m_cache[cacheSet][it->second];
        if (entry && entry->m_Address == tag &&
            entry->m_Permission !=
            AccessPermission_NotPresent)
            return it->second;
    }
    return -1; // Not found
}

// Given a cache index: returns the index of the tag in a set.
// returns -1 if the tag is not found.
int
CacheMemory::findTagInSetIgnorePermissions(int64_t cacheSet,
                                           Addr tag) const
{
    assert(tag == makeLineAddress(tag));
    // search the set for the tags
    auto it = m_tag_index.find(tag);
    if (it != m_tag_index.end())
        return it->second;
    return -1; // Not found
}

// Given an unique cache block identifier (idx): return the valid address
// stored by the cache block.  If the block is invalid/notpresent, the
// function returns the 0 address
Addr
CacheMemory::getAddressAtIdx(int idx) const
{
    Addr tmp(0);

    int set = idx / m_cache_assoc;
    assert(set < m_cache_num_sets);

    int way = idx - set * m_cache_assoc;
    assert (way < m_cache_assoc);

    AbstractCacheEntry* entry = m_cache[set][way];
    if (entry == NULL ||
        entry->m_Permission == AccessPermission_Invalid ||
        entry->m_Permission == AccessPermission_NotPresent) {
        return tmp;
    }
    return entry->m_Address;
}

bool
CacheMemory::tryCacheAccess(Addr address, RubyRequestType type,
                            DataBlock*& data_ptr)
{
    DPRINTF(RubyCache, "trying to access address: %#x\n", address);
    AbstractCacheEntry* entry = lookup(address);
    if (entry != nullptr) {
        // Do we even have a tag match?
        m_replacementPolicy_ptr->touch(entry->replacementData);
        entry->setLastAccess(curTick());
        data_ptr = &(entry->getDataBlk());

        if (entry->m_Permission == AccessPermission_Read_Write) {
            DPRINTF(RubyCache, "Have permission to access address: %#x\n",
                        address);
            return true;
        }
        if ((entry->m_Permission == AccessPermission_Read_Only) &&
            (type == RubyRequestType_LD || type == RubyRequestType_IFETCH)) {
            DPRINTF(RubyCache, "Have permission to access address: %#x\n",
                        address);
            return true;
        }
        // The line must not be accessible
    }
    DPRINTF(RubyCache, "Do not have permission to access address: %#x\n",
                address);
    data_ptr = NULL;
    return false;
}

bool
CacheMemory::testCacheAccess(Addr address, RubyRequestType type,
                             DataBlock*& data_ptr)
{
    DPRINTF(RubyCache, "testing address: %#x\n", address);
    AbstractCacheEntry* entry = lookup(address);
    if (entry != nullptr) {
        // Do we even have a tag match?
        m_replacementPolicy_ptr->touch(entry->replacementData);
        entry->setLastAccess(curTick());
        data_ptr = &(entry->getDataBlk());

        DPRINTF(RubyCache, "have permission for address %#x?: %d\n",
                    address,
                    entry->m_Permission != AccessPermission_NotPresent);
        return entry->m_Permission != AccessPermission_NotPresent;
    }

    DPRINTF(RubyCache, "do not have permission for address %#x\n",
                address);
    data_ptr = NULL;
    return false;
}

// tests to see if an address is present in the cache
bool
CacheMemory::isTagPresent(Addr address) const
{
    const AbstractCacheEntry* const entry = lookup(address);
    if (entry == nullptr) {
        // We didn't find the tag
        DPRINTF(RubyCache, "No tag match for address: %#x\n", address);
        return false;
    }
    DPRINTF(RubyCache, "address: %#x found\n", address);
    return true;
}

// Returns true if there is:
//   a) a tag match on this address or there is
//   b) an unused line in the same cache "way"
bool
CacheMemory::cacheAvail(Addr address) const
{
    assert(address == makeLineAddress(address));

    int64_t cacheSet = addressToCacheSet(address);

    for (int i = 0; i < m_cache_assoc; i++) {
        AbstractCacheEntry* entry = m_cache[cacheSet][i];
        if (entry != NULL) {
            if (entry->m_Address == address ||
                entry->m_Permission == AccessPermission_NotPresent) {
                // Already in the cache or we found an empty entry
                return true;
            }
        } else {
            return true;
        }
    }
    return false;
}

bool
CacheMemory::cacheAvailInWays(Addr address, int ways) const
{
    assert(address == makeLineAddress(address));
    fatal_if(ways < 1 || ways > m_cache_assoc,
             "Invalid DDIO way subset [0, %d) for %d-way cache",
             ways, m_cache_assoc);

    // A present tag match in *any* way is available (NIC writes may hit in
    // any way, like real DDIO). A NotPresent placeholder outside the subset
    // is not capacity in the subset; allocateInWays will reclaim it safely.
    int64_t cacheSet = addressToCacheSet(address);
    int matching_way = -1;
    for (int i = 0; i < m_cache_assoc; i++) {
        AbstractCacheEntry* entry = m_cache[cacheSet][i];
        if (entry != NULL && entry->m_Address == address) {
            fatal_if(matching_way >= 0,
                     "Duplicate cache entries for address %#x in ways %d "
                     "and %d", address, matching_way, i);
            matching_way = i;
        }
    }
    if (matching_way >= 0) {
        AbstractCacheEntry* entry = m_cache[cacheSet][matching_way];
        if (entry->m_Permission != AccessPermission_NotPresent) {
            return true;
        }
        if (matching_way < ways)
            return true;
    }
    for (int i = 0; i < ways; i++) {
        AbstractCacheEntry* entry = m_cache[cacheSet][i];
        if (entry == NULL ||
            entry->m_Permission == AccessPermission_NotPresent) {
            return true;
        }
    }
    return false;
}

AbstractCacheEntry*
CacheMemory::allocateInWays(Addr address, AbstractCacheEntry *entry,
                            int ways)
{
    assert(address == makeLineAddress(address));
    assert(!isTagPresent(address));
    fatal_if(ways < 1 || ways > m_cache_assoc,
             "Invalid DDIO way subset [0, %d) for %d-way cache",
             ways, m_cache_assoc);
    DPRINTF(RubyCache, "allocating address (DDIO ways [0,%d)): %#x\n",
            ways, address);

    entry->initBlockSize(m_block_size);
    entry->setRubySystem(m_ruby_system);

    int64_t cacheSet = addressToCacheSet(address);
    std::vector<AbstractCacheEntry*> &set = m_cache[cacheSet];

    // NotPresent is not a protocol-visible line. Reclaim a stale
    // same-address placeholder even when it was left in an unrestricted way,
    // then allocate the line in the configured subset.
    int way = -1;
    for (int i = 0; i < m_cache_assoc; i++) {
        if (set[i] && set[i]->m_Address == address) {
            fatal_if(way >= 0,
                     "Duplicate cache entries for address %#x in ways %d "
                     "and %d", address, way, i);
            fatal_if(set[i]->m_Permission != AccessPermission_NotPresent,
                     "Cannot allocate already-present address %#x in DDIO "
                     "ways [0, %d)", address, ways);
            if (i < ways) {
                way = i;
            } else {
                auto old = m_tag_index.find(address);
                if (old != m_tag_index.end() && old->second == i)
                    m_tag_index.erase(old);
                m_replacementPolicy_ptr->invalidate(
                    replacement_data[cacheSet][i]);
                delete set[i];
                set[i] = nullptr;
            }
        }
    }

    // Otherwise find a free slot within the DDIO way subset, again using
    // the replacement policy among the free slots rather than way order.
    if (way < 0) {
        std::vector<ReplaceableEntry*> free_candidates;
        std::vector<std::unique_ptr<ReplaceableEntry>> tmp_entries;
        for (int i = 0; i < ways; i++) {
            if (!set[i]) {
                auto te = std::make_unique<ReplaceableEntry>();
                te->setPosition(cacheSet, i);
                te->replacementData = replacement_data[cacheSet][i];
                free_candidates.push_back(te.get());
                tmp_entries.push_back(std::move(te));
            } else if (set[i]->m_Permission == AccessPermission_NotPresent) {
                free_candidates.push_back(
                    static_cast<ReplaceableEntry*>(set[i]));
            }
        }
        if (!free_candidates.empty()) {
            // Invalid LRU entries commonly all have tick zero. Select such
            // ties round-robin rather than relying on candidate order.
            for (int offset = 0; offset < ways; ++offset) {
                const int candidate =
                    (m_next_invalid_way[cacheSet] + offset) % ways;
                if (!set[candidate] || set[candidate]->m_Permission ==
                        AccessPermission_NotPresent) {
                    way = candidate;
                    break;
                }
            }
        }
    }

    if (way >= 0) {
        int i = way;
        if (set[i] && set[i]->m_Address != address) {
            auto old = m_tag_index.find(set[i]->m_Address);
            if (old != m_tag_index.end() && old->second == i) {
                m_tag_index.erase(old);
            }
        }
        if (set[i] && set[i] != entry) {
            m_replacementPolicy_ptr->invalidate(replacement_data[cacheSet][i]);
            delete set[i];
        }
        set[i] = entry;  // Init entry
        set[i]->m_Address = address;
        set[i]->m_Permission = AccessPermission_Invalid;
        set[i]->m_locked = -1;
        m_tag_index[address] = i;
        set[i]->setPosition(cacheSet, i);
        set[i]->replacementData = replacement_data[cacheSet][i];
        set[i]->setLastAccess(curTick());

        // Call reset function here to set initial value for different
        // replacement policies.
        m_replacementPolicy_ptr->reset(entry->replacementData);
        m_next_invalid_way[cacheSet] = (i + 1) % ways;

        return entry;
    }
    panic("allocateInWays didn't find an available entry in ways [0,%d)",
          ways);
}

Addr
CacheMemory::cacheProbeInWays(Addr address, int ways) const
{
    assert(address == makeLineAddress(address));
    assert(!cacheAvailInWays(address, ways));
    fatal_if(ways < 1 || ways > m_cache_assoc,
             "Invalid DDIO way subset [0, %d) for %d-way cache",
             ways, m_cache_assoc);

    // Choose a victim only among the DDIO way subset.  The replacement
    // policy must be subset-safe (e.g. LRU); tree/associativity-indexed
    // policies are not.
    int64_t cacheSet = addressToCacheSet(address);
    std::vector<ReplaceableEntry*> candidates;
    for (int i = 0; i < ways; i++) {
        candidates.push_back(static_cast<ReplaceableEntry*>(
                                                       m_cache[cacheSet][i]));
    }
    return m_cache[cacheSet][m_replacementPolicy_ptr->
                        getVictim(candidates)->getWay()]->m_Address;
}

AbstractCacheEntry*
CacheMemory::allocate(Addr address, AbstractCacheEntry *entry)
{
    assert(address == makeLineAddress(address));
    assert(!isTagPresent(address));
    assert(cacheAvail(address));
    DPRINTF(RubyCache, "allocating address: %#x\n", address);

    entry->initBlockSize(m_block_size);
    entry->setRubySystem(m_ruby_system);

    // Find a free slot.  Pick the replacement policy's victim among the
    // free slots (LRU naturally prefers the one invalidated longest ago,
    // since invalidate() zeroes the touch tick) instead of the first
    // slot in way order: with a first-free-way policy, low-numbered ways
    // are consumed preferentially by all fill sources, which biases any
    // way-partitioned region (e.g. DDIO ways).
    int64_t cacheSet = addressToCacheSet(address);
    std::vector<AbstractCacheEntry*> &set = m_cache[cacheSet];
    int free_way = -1;
    // Only policies that are safe on candidate subsets (e.g. LRU; tree-
    // based policies index by full associativity) can pick among free
    // slots.  Otherwise fall back to first-free-way.
    const bool subset_safe =
        dynamic_cast<replacement_policy::LRU*>(m_replacementPolicy_ptr);
    if (subset_safe) {
        std::vector<ReplaceableEntry*> free_candidates;
        // Temporary entry views for NULL (deallocated) slots; their
        // replacement data persists across deallocation.
        std::vector<std::unique_ptr<ReplaceableEntry>> tmp_entries;
        for (int i = 0; i < m_cache_assoc; i++) {
            if (!set[i]) {
                auto te = std::make_unique<ReplaceableEntry>();
                te->setPosition(cacheSet, i);
                te->replacementData = replacement_data[cacheSet][i];
                free_candidates.push_back(te.get());
                tmp_entries.push_back(std::move(te));
            } else if (set[i]->m_Permission == AccessPermission_NotPresent) {
                free_candidates.push_back(
                    static_cast<ReplaceableEntry*>(set[i]));
            }
        }
        if (!free_candidates.empty() && m_ddio_way_part > 0) {
            for (int offset = 0; offset < m_cache_assoc; ++offset) {
                const int candidate =
                    (m_next_invalid_way[cacheSet] + offset) % m_cache_assoc;
                if (!set[candidate] || set[candidate]->m_Permission ==
                        AccessPermission_NotPresent) {
                    free_way = candidate;
                    break;
                }
            }
        } else if (!free_candidates.empty()) {
            free_way = m_replacementPolicy_ptr->
                getVictim(free_candidates)->getWay();
        }
    }
    for (int i = 0; i < m_cache_assoc; i++) {
        if (subset_safe && i != free_way) continue;
        if (!set[i] || set[i]->m_Permission == AccessPermission_NotPresent) {
            if (set[i] && set[i]->m_Address != address) {
                auto old = m_tag_index.find(set[i]->m_Address);
                if (old != m_tag_index.end() && old->second == i) {
                    m_tag_index.erase(old);
                }
            }
            if (set[i] && (set[i] != entry)) {
                warn_once("This protocol contains a cache entry handling bug: "
                    "Entries in the cache should never be NotPresent! If\n"
                    "this entry (%#x) is not tracked elsewhere, it will memory "
                    "leak here. Fix your protocol to eliminate these!",
                    address);
            }
            set[i] = entry;  // Init entry
            set[i]->m_Address = address;
            set[i]->m_Permission = AccessPermission_Invalid;
            DPRINTF(RubyCache, "Allocate clearing lock for addr: 0x%x\n",
                    address);
            set[i]->m_locked = -1;
            m_tag_index[address] = i;
            set[i]->setPosition(cacheSet, i);
            set[i]->replacementData = replacement_data[cacheSet][i];
            set[i]->setLastAccess(curTick());

            // Call reset function here to set initial value for different
            // replacement policies.
            m_replacementPolicy_ptr->reset(entry->replacementData);
            if (m_ddio_way_part > 0) {
                m_next_invalid_way[cacheSet] =
                    (i + 1) % m_cache_assoc;
            }

            return entry;
        }
    }
    panic("Allocate didn't find an available entry");
}

void
CacheMemory::deallocate(Addr address)
{
    DPRINTF(RubyCache, "deallocating address: %#x\n", address);
    AbstractCacheEntry* entry = lookup(address);
    assert(entry != nullptr);
    m_replacementPolicy_ptr->invalidate(entry->replacementData);
    uint32_t cache_set = entry->getSet();
    uint32_t way = entry->getWay();
    cacheMemoryStats.wayDeallocations[way]++;
    delete entry;
    m_cache[cache_set][way] = NULL;
    m_tag_index.erase(address);
}

// Returns with the physical address of the conflicting cache line
Addr
CacheMemory::cacheProbe(Addr address) const
{
    assert(address == makeLineAddress(address));
    assert(!cacheAvail(address));

    int64_t cacheSet = addressToCacheSet(address);
    std::vector<ReplaceableEntry*> candidates;
    for (int i = 0; i < m_cache_assoc; i++) {
        candidates.push_back(static_cast<ReplaceableEntry*>(
                                                       m_cache[cacheSet][i]));
    }
    return m_cache[cacheSet][m_replacementPolicy_ptr->
                        getVictim(candidates)->getWay()]->m_Address;
}

// looks an address up in the cache
AbstractCacheEntry*
CacheMemory::lookup(Addr address)
{
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc == -1) return NULL;
    return m_cache[cacheSet][loc];
}

// looks an address up in the cache
const AbstractCacheEntry*
CacheMemory::lookup(Addr address) const
{
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc == -1) return NULL;
    return m_cache[cacheSet][loc];
}

// Sets the most recently used bit for a cache block
void
CacheMemory::setMRU(Addr address)
{
    AbstractCacheEntry* entry = lookup(makeLineAddress(address));
    if (entry != nullptr) {
        m_replacementPolicy_ptr->touch(entry->replacementData);
        entry->setLastAccess(curTick());
    }
}

void
CacheMemory::setMRU(AbstractCacheEntry *entry)
{
    assert(entry != nullptr);
    m_replacementPolicy_ptr->touch(entry->replacementData);
    entry->setLastAccess(curTick());
}

void
CacheMemory::setMRU(Addr address, int occupancy)
{
    AbstractCacheEntry* entry = lookup(makeLineAddress(address));
    if (entry != nullptr) {
        // m_use_occupancy can decide whether we are using WeightedLRU
        // replacement policy. Depending on different replacement policies,
        // use different touch() function.
        if (m_use_occupancy) {
            static_cast<replacement_policy::WeightedLRU*>(
                m_replacementPolicy_ptr)->touch(
                entry->replacementData, occupancy);
        } else {
            m_replacementPolicy_ptr->touch(entry->replacementData);
        }
        entry->setLastAccess(curTick());
    }
}

int
CacheMemory::getReplacementWeight(int64_t set, int64_t loc)
{
    assert(set < m_cache_num_sets);
    assert(loc < m_cache_assoc);
    int ret = 0;
    if (m_cache[set][loc] != NULL) {
        ret = m_cache[set][loc]->getNumValidBlocks();
        assert(ret >= 0);
    }

    return ret;
}

void
CacheMemory::recordCacheContents(int cntrl, CacheRecorder* tr) const
{
    uint64_t warmedUpBlocks = 0;
    [[maybe_unused]] uint64_t totalBlocks = (uint64_t)m_cache_num_sets *
                                         (uint64_t)m_cache_assoc;

    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            if (m_cache[i][j] != NULL) {
                AccessPermission perm = m_cache[i][j]->m_Permission;
                RubyRequestType request_type = RubyRequestType_NULL;
                if (perm == AccessPermission_Read_Only) {
                    if (m_is_instruction_only_cache) {
                        request_type = RubyRequestType_IFETCH;
                    } else {
                        request_type = RubyRequestType_LD;
                    }
                } else if (perm == AccessPermission_Read_Write) {
                    request_type = RubyRequestType_ST;
                }

                if (request_type != RubyRequestType_NULL) {
                    Tick lastAccessTick;
                    lastAccessTick = m_cache[i][j]->getLastAccess();
                    tr->addRecord(cntrl, m_cache[i][j]->m_Address,
                                  0, request_type, lastAccessTick,
                                  m_cache[i][j]->getDataBlk());
                    warmedUpBlocks++;
                }
            }
        }
    }

    DPRINTF(RubyCacheTrace, "%s: %lli blocks of %lli total blocks"
            "recorded %.2f%% \n", name().c_str(), warmedUpBlocks,
            totalBlocks, (float(warmedUpBlocks) / float(totalBlocks)) * 100.0);
}

void
CacheMemory::print(std::ostream& out) const
{
    out << "Cache dump: " << name() << std::endl;
    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            if (m_cache[i][j] != NULL) {
                out << "  Index: " << i
                    << " way: " << j
                    << " entry: " << *m_cache[i][j] << std::endl;
            } else {
                out << "  Index: " << i
                    << " way: " << j
                    << " entry: NULL" << std::endl;
            }
        }
    }
}

void
CacheMemory::printData(std::ostream& out) const
{
    out << "printData() not supported" << std::endl;
}

void
CacheMemory::setLocked(Addr address, int context)
{
    DPRINTF(RubyCache, "Setting Lock for addr: %#x to %d\n", address, context);
    AbstractCacheEntry* entry = lookup(address);
    assert(entry != nullptr);
    entry->setLocked(context);
}

void
CacheMemory::clearLocked(Addr address)
{
    DPRINTF(RubyCache, "Clear Lock for addr: %#x\n", address);
    AbstractCacheEntry* entry = lookup(address);
    assert(entry != nullptr);
    entry->clearLocked();
}

void
CacheMemory::clearLockedAll(int context)
{
    // iterate through every set and way to get a cache line
    for (auto i = m_cache.begin(); i != m_cache.end(); ++i) {
        std::vector<AbstractCacheEntry*> set = *i;
        for (auto j = set.begin(); j != set.end(); ++j) {
            AbstractCacheEntry *line = *j;
            if (line && line->isLocked(context)) {
                DPRINTF(RubyCache, "Clear Lock for addr: %#x\n",
                    line->m_Address);
                line->clearLocked();
            }
        }
    }
}

bool
CacheMemory::isLocked(Addr address, int context)
{
    AbstractCacheEntry* entry = lookup(address);
    assert(entry != nullptr);
    DPRINTF(RubyCache, "Testing Lock for addr: %#llx cur %d con %d\n",
            address, entry->m_locked, context);
    return entry->isLocked(context);
}

CacheMemory::
CacheMemoryStats::CacheMemoryStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(numDataArrayReads, "Number of data array reads"),
      ADD_STAT(numDataArrayWrites, "Number of data array writes"),
      ADD_STAT(numTagArrayReads, "Number of tag array reads"),
      ADD_STAT(numTagArrayWrites, "Number of tag array writes"),
      ADD_STAT(numTagArrayStalls, "Number of stalls caused by tag array"),
      ADD_STAT(numDataArrayStalls, "Number of stalls caused by data array"),
      ADD_STAT(numAtomicALUOperations, "Number of atomic ALU operations"),
      ADD_STAT(numAtomicALUArrayStalls, "Number of stalls caused by atomic ALU array"),
      ADD_STAT(htmTransCommitReadSet, "Read set size of a committed "
                                      "transaction"),
      ADD_STAT(htmTransCommitWriteSet, "Write set size of a committed "
                                       "transaction"),
      ADD_STAT(htmTransAbortReadSet, "Read set size of a aborted transaction"),
      ADD_STAT(htmTransAbortWriteSet, "Write set size of a aborted "
                                      "transaction"),
      ADD_STAT(m_demand_hits, "Number of cache demand hits"),
      ADD_STAT(m_demand_misses, "Number of cache demand misses"),
      ADD_STAT(m_demand_accesses, "Number of cache demand accesses",
               m_demand_hits + m_demand_misses),
      ADD_STAT(m_prefetch_hits, "Number of cache prefetch hits"),
      ADD_STAT(m_prefetch_misses, "Number of cache prefetch misses"),
      ADD_STAT(m_prefetch_accesses, "Number of cache prefetch accesses",
               m_prefetch_hits + m_prefetch_misses),
      ADD_STAT(m_accessModeType, ""),
      ADD_STAT(dmaRoutingProxyRequests,
               "Classified DMA requests proxied through this cache bank"),
      ADD_STAT(dmaRoutingTransientRecycles,
               "Classified DMA requests recycled in a transient cache state"),
      ADD_STAT(ddioReplacementStalls,
               "Classified DMA writes stalled for DDIO subset replacement"),
      ADD_STAT(ddioOwnershipRequests,
               "Retained DDIO ownership requests sent to the directory"),
      ADD_STAT(ddioOwnershipAcks,
               "Retained DDIO ownership acknowledgements received"),
      ADD_STAT(rxPayloadRequests,
               "Number of NIC RX data DMA write line transactions"),
      ADD_STAT(rxPayloadHits,
               "Number of NIC RX data DMA writes hitting a present line"),
      ADD_STAT(rxPayloadMisses,
               "Number of NIC RX data DMA writes missing at acceptance"),
      ADD_STAT(rxPayloadHitRate, "NIC RX data DMA write hit rate",
               rxPayloadHits / (rxPayloadHits + rxPayloadMisses)),
      ADD_STAT(rxPayloadHitWays, "Way histogram of RX data hits"),
      ADD_STAT(rxPayloadAllocWays, "Way histogram of RX payload allocations"),
      ADD_STAT(ddioAllocWays, "Way histogram of all NIC DDIO allocations"),
      ADD_STAT(rxHeaderRequests, "Number of NIC RX header DMA write line transactions"),
      ADD_STAT(rxHeaderHits, "Number of NIC RX header DMA writes hitting a present line"),
      ADD_STAT(rxHeaderMisses, "Number of NIC RX header DMA writes missing at acceptance"),
      ADD_STAT(rxHeaderHitRate, "NIC RX header DMA write hit rate",
               rxHeaderHits / (rxHeaderHits + rxHeaderMisses)),
      ADD_STAT(txPayloadRequests,
               "Number of NIC TX payload DMA read line transactions"),
      ADD_STAT(txPayloadHits,
               "Number of NIC TX payload DMA reads hitting a present line"),
      ADD_STAT(txPayloadMisses,
               "Number of NIC TX payload DMA reads missing"),
      ADD_STAT(txPayloadHitRate, "NIC TX payload DMA read hit rate",
               txPayloadHits / (txPayloadHits + txPayloadMisses)),
      ADD_STAT(ddioWayAccess,
               "Accesses per way by requester class (src x way)"),
      ADD_STAT(ddioWayFill,
               "Allocations per way by requester class (src x way)"),
      ADD_STAT(wayDeallocations,
               "Cache-entry deallocations per way (all causes)"),
      ADD_STAT(rxPayloadCpuAccessWays,
               "CPU/general hits to prior RX payload addresses by way"),
      ADD_STAT(rxPayloadCpuFillWays,
               "CPU/general fills of prior RX payload addresses by way"),
      ADD_STAT(rxPayloadCpuUniqueLines,
               "Prior RX payload lines touched by CPU/general requests"),
      ADD_STAT(rxPayloadSetHits, "RX payload hits per cache set"),
      ADD_STAT(rxPayloadSetMisses, "RX payload misses per cache set"),
      ADD_STAT(rxPayloadUniqueLines, "Unique RX payload line addresses"),
      ADD_STAT(txPayloadUniqueLines, "Unique TX payload line addresses")
{
    rxPayloadHitRate.flags(statistics::nonan);
    txPayloadHitRate.flags(statistics::nonan);
    numDataArrayReads
        .flags(statistics::nozero);

    numDataArrayWrites
        .flags(statistics::nozero);

    numTagArrayReads
        .flags(statistics::nozero);

    numTagArrayWrites
        .flags(statistics::nozero);

    numTagArrayStalls
        .flags(statistics::nozero);

    numDataArrayStalls
        .flags(statistics::nozero);

    numAtomicALUOperations
        .flags(statistics::nozero);

    numAtomicALUArrayStalls
        .flags(statistics::nozero);

    htmTransCommitReadSet
        .init(8)
        .flags(statistics::pdf | statistics::dist | statistics::nozero |
            statistics::nonan);

    htmTransCommitWriteSet
        .init(8)
        .flags(statistics::pdf | statistics::dist | statistics::nozero |
            statistics::nonan);

    htmTransAbortReadSet
        .init(8)
        .flags(statistics::pdf | statistics::dist | statistics::nozero |
            statistics::nonan);

    htmTransAbortWriteSet
        .init(8)
        .flags(statistics::pdf | statistics::dist | statistics::nozero |
            statistics::nonan);

    m_prefetch_hits
        .flags(statistics::nozero);

    m_prefetch_misses
        .flags(statistics::nozero);

    m_prefetch_accesses
        .flags(statistics::nozero);

    m_accessModeType
        .init(RubyRequestType_NUM)
        .flags(statistics::pdf | statistics::total);

    for (int i = 0; i < RubyAccessMode_NUM; i++) {
        m_accessModeType
            .subname(i, RubyAccessMode_to_string(RubyAccessMode(i)))
            .flags(statistics::nozero)
            ;
    }

    // NOTE: the way-histogram vectors are sized in CacheMemory::init(),
    // since m_cache_assoc is not yet set when this constructor runs. DDIO
    // scalar zeros intentionally remain printable for strict test contracts.
    rxHeaderHitRate.flags(statistics::nonan);
}

// assumption: SLICC generated files will only call this function
// once **all** resources are granted
void
CacheMemory::recordRequestType(CacheRequestType requestType, Addr addr)
{
    DPRINTF(RubyStats, "Recorded statistic: %s\n",
            CacheRequestType_to_string(requestType));
    switch(requestType) {
    case CacheRequestType_DataArrayRead:
        if (m_resource_stalls)
            dataArray.reserve(addressToCacheSet(addr));
        cacheMemoryStats.numDataArrayReads++;
        return;
    case CacheRequestType_DataArrayWrite:
        if (m_resource_stalls)
            dataArray.reserve(addressToCacheSet(addr));
        cacheMemoryStats.numDataArrayWrites++;
        return;
    case CacheRequestType_TagArrayRead:
        if (m_resource_stalls)
            tagArray.reserve(addressToCacheSet(addr));
        cacheMemoryStats.numTagArrayReads++;
        return;
    case CacheRequestType_TagArrayWrite:
        if (m_resource_stalls)
            tagArray.reserve(addressToCacheSet(addr));
        cacheMemoryStats.numTagArrayWrites++;
        return;
    case CacheRequestType_AtomicALUOperation:
        if (m_resource_stalls)
            atomicALUArray.reserve(addr);
        cacheMemoryStats.numAtomicALUOperations++;
        return;
    default:
        warn("CacheMemory access_type not found: %s",
             CacheRequestType_to_string(requestType));
    }
}

bool
CacheMemory::checkResourceAvailable(CacheResourceType res, Addr addr)
{
    if (!m_resource_stalls) {
        return true;
    }

    if (res == CacheResourceType_TagArray) {
        if (tagArray.tryAccess(addressToCacheSet(addr))) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Tag array stall on addr %#x in set %d\n",
                    addr, addressToCacheSet(addr));
            cacheMemoryStats.numTagArrayStalls++;
            return false;
        }
    } else if (res == CacheResourceType_DataArray) {
        if (dataArray.tryAccess(addressToCacheSet(addr))) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Data array stall on addr %#x in set %d\n",
                    addr, addressToCacheSet(addr));
            cacheMemoryStats.numDataArrayStalls++;
            return false;
        }
    } else if (res == CacheResourceType_AtomicALUArray) {
        if (atomicALUArray.tryAccess(addr)) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Atomic ALU array stall on addr %#x in line address %#x\n",
                    addr, makeLineAddress(addr));
            cacheMemoryStats.numAtomicALUArrayStalls++;
            return false;
        }
    } else {
        panic("Unrecognized cache resource type.");
    }
}

bool
CacheMemory::isBlockInvalid(int64_t cache_set, int64_t loc)
{
  return (m_cache[cache_set][loc]->m_Permission == AccessPermission_Invalid);
}

bool
CacheMemory::isBlockNotBusy(int64_t cache_set, int64_t loc)
{
  return (m_cache[cache_set][loc]->m_Permission != AccessPermission_Busy);
}

/* hardware transactional memory */

void
CacheMemory::htmAbortTransaction()
{
    uint64_t htmReadSetSize = 0;
    uint64_t htmWriteSetSize = 0;

    // iterate through every set and way to get a cache line
    for (auto i = m_cache.begin(); i != m_cache.end(); ++i)
    {
        std::vector<AbstractCacheEntry*> set = *i;

        for (auto j = set.begin(); j != set.end(); ++j)
        {
            AbstractCacheEntry *line = *j;

            if (line != nullptr) {
                htmReadSetSize += (line->getInHtmReadSet() ? 1 : 0);
                htmWriteSetSize += (line->getInHtmWriteSet() ? 1 : 0);
                if (line->getInHtmWriteSet()) {
                    line->invalidateEntry();
                }
                line->setInHtmWriteSet(false);
                line->setInHtmReadSet(false);
                line->clearLocked();
            }
        }
    }

    cacheMemoryStats.htmTransAbortReadSet.sample(htmReadSetSize);
    cacheMemoryStats.htmTransAbortWriteSet.sample(htmWriteSetSize);
    DPRINTF(HtmMem, "htmAbortTransaction: read set=%u write set=%u\n",
        htmReadSetSize, htmWriteSetSize);
}

void
CacheMemory::htmCommitTransaction()
{
    uint64_t htmReadSetSize = 0;
    uint64_t htmWriteSetSize = 0;

    // iterate through every set and way to get a cache line
    for (auto i = m_cache.begin(); i != m_cache.end(); ++i)
    {
        std::vector<AbstractCacheEntry*> set = *i;

        for (auto j = set.begin(); j != set.end(); ++j)
        {
            AbstractCacheEntry *line = *j;
            if (line != nullptr) {
                htmReadSetSize += (line->getInHtmReadSet() ? 1 : 0);
                htmWriteSetSize += (line->getInHtmWriteSet() ? 1 : 0);
                line->setInHtmWriteSet(false);
                line->setInHtmReadSet(false);
                line->clearLocked();
             }
        }
    }

    cacheMemoryStats.htmTransCommitReadSet.sample(htmReadSetSize);
    cacheMemoryStats.htmTransCommitWriteSet.sample(htmWriteSetSize);
    DPRINTF(HtmMem, "htmCommitTransaction: read set=%u write set=%u\n",
        htmReadSetSize, htmWriteSetSize);
}

void
CacheMemory::profileDemandHit()
{
    cacheMemoryStats.m_demand_hits++;
}

void
CacheMemory::profileDemandMiss()
{
    cacheMemoryStats.m_demand_misses++;
}

void
CacheMemory::profileRxPayload(Addr address)
{
    cacheMemoryStats.rxPayloadRequests++;
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc >= 0) {
        cacheMemoryStats.rxPayloadHits++;
        cacheMemoryStats.rxPayloadHitWays[loc]++;
        cacheMemoryStats.rxPayloadSetHits[cacheSet]++;
    } else {
        cacheMemoryStats.rxPayloadMisses++;
        cacheMemoryStats.rxPayloadSetMisses[cacheSet]++;
    }
    if (rxPayloadUniqueAddrs.insert(address).second)
        cacheMemoryStats.rxPayloadUniqueLines++;
    rxPayloadEverAddrs.insert(address);
}

void
CacheMemory::profileRxHeader(Addr address)
{
    cacheMemoryStats.rxHeaderRequests++;
    const int64_t cacheSet = addressToCacheSet(address);
    if (findTagInSet(cacheSet, address) >= 0) {
        cacheMemoryStats.rxHeaderHits++;
    } else {
        cacheMemoryStats.rxHeaderMisses++;
    }
}

void
CacheMemory::profileTxPayload(Addr address)
{
    cacheMemoryStats.txPayloadRequests++;
    if (isTagPresent(address)) {
        cacheMemoryStats.txPayloadHits++;
    } else {
        cacheMemoryStats.txPayloadMisses++;
    }
    if (txPayloadUniqueAddrs.insert(address).second)
        cacheMemoryStats.txPayloadUniqueLines++;
}

void
CacheMemory::profilePrefetchHit()
{
    cacheMemoryStats.m_prefetch_hits++;
}

void
CacheMemory::profilePrefetchMiss()
{
    cacheMemoryStats.m_prefetch_misses++;
}

} // namespace ruby
} // namespace gem5
