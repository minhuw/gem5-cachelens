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

#ifndef __MEM_RUBY_STRUCTURES_CACHEMEMORY_HH__
#define __MEM_RUBY_STRUCTURES_CACHEMEMORY_HH__

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/ruby/common/DataBlock.hh"
#include "mem/ruby/protocol/CacheRequestType.hh"
#include "mem/ruby/protocol/CacheResourceType.hh"
#include "mem/ruby/protocol/RubyRequest.hh"
#include "mem/ruby/slicc_interface/AbstractCacheEntry.hh"
#include "mem/ruby/slicc_interface/RubySlicc_ComponentMapping.hh"
#include "mem/ruby/structures/BankedArray.hh"
#include "mem/ruby/structures/ALUFreeListArray.hh"
#include "mem/ruby/system/CacheRecorder.hh"
#include "params/RubyCache.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace ruby
{

class CacheMemory : public SimObject
{
    friend class CacheMemoryTest;

  public:
    typedef RubyCacheParams Params;
    typedef std::shared_ptr<replacement_policy::ReplacementData> ReplData;
    CacheMemory(const Params &p);
    ~CacheMemory();

    void init();
    void resetStats() override;

    // Public Methods
    // perform a cache access and see if we hit or not.  Return true on a hit.
    bool tryCacheAccess(Addr address, RubyRequestType type,
                        DataBlock*& data_ptr);

    // similar to above, but doesn't require full access check
    bool testCacheAccess(Addr address, RubyRequestType type,
                         DataBlock*& data_ptr);

    // tests to see if an address is present in the cache
    bool isTagPresent(Addr address) const;

    // Returns true if there is:
    //   a) a tag match on this address or there is
    //   b) an unused line in the same cache "way"
    bool cacheAvail(Addr address) const;

    // DDIO way-partitioned variants: like cacheAvail/allocate/cacheProbe,
    // but only consider ways [0, ways). Used for NIC RX data fills so packet
    // lines, including split headers, are confined to the DDIO way subset.
    bool cacheAvailInWays(Addr address, int ways) const;
    AbstractCacheEntry* allocateInWays(Addr address,
                                       AbstractCacheEntry* new_entry,
                                       int ways);
    Addr cacheProbeInWays(Addr address, int ways) const;

    // Number of NIC DDIO allocation ways (-1 = NIC-write no allocation).
    int getDDIOWayPart() const { return m_ddio_way_part; }

    // Returns a NULL entry that acts as a placeholder for invalid lines
    AbstractCacheEntry*
    getNullEntry() const
    {
        return nullptr;
    }

    // find an unused entry and sets the tag appropriate for the address
    AbstractCacheEntry* allocate(Addr address, AbstractCacheEntry* new_entry);
    void allocateVoid(Addr address, AbstractCacheEntry* new_entry)
    {
        allocate(address, new_entry);
    }

    // Explicitly free up this address
    void deallocate(Addr address);

    // Returns with the physical address of the conflicting cache line
    Addr cacheProbe(Addr address) const;

    // looks an address up in the cache
    AbstractCacheEntry* lookup(Addr address);
    const AbstractCacheEntry* lookup(Addr address) const;

    Cycles getTagLatency() const { return tagArray.getLatency(); }
    Cycles getDataLatency() const { return dataArray.getLatency(); }

    bool isBlockInvalid(int64_t cache_set, int64_t loc);
    bool isBlockNotBusy(int64_t cache_set, int64_t loc);

    // Hook for checkpointing the contents of the cache
    void recordCacheContents(int cntrl, CacheRecorder* tr) const;

    // Set this address to most recently used
    void setMRU(Addr address);
    void setMRU(Addr addr, int occupancy);
    void setMRU(AbstractCacheEntry* entry);
    int getReplacementWeight(int64_t set, int64_t loc);

    // Functions for locking and unlocking cache lines corresponding to the
    // provided address.  These are required for supporting atomic memory
    // accesses.  These are to be used when only the address of the cache entry
    // is available.  In case the entry itself is available. use the functions
    // provided by the AbstractCacheEntry class.
    void setLocked (Addr addr, int context);
    void clearLocked (Addr addr);
    void clearLockedAll (int context);
    bool isLocked (Addr addr, int context);

    // Print cache contents
    void print(std::ostream& out) const;
    void printData(std::ostream& out) const;

    bool checkResourceAvailable(CacheResourceType res, Addr addr);
    void recordRequestType(CacheRequestType requestType, Addr addr);

    // hardware transactional memory
    void htmAbortTransaction();
    void htmCommitTransaction();

    void setRubySystem(RubySystem* rs);

  public:
    int getCacheSize() const { return m_cache_size; }
    int getCacheAssoc() const { return m_cache_assoc; }
    int getNumBlocks() const { return m_cache_num_sets * m_cache_assoc; }
    Addr getAddressAtIdx(int idx) const;

  private:
    // convert a Address to its location in the cache
    int64_t addressToCacheSet(Addr address) const;

    // Given a cache tag: returns the index of the tag in a set.
    // returns -1 if the tag is not found.
    int findTagInSet(int64_t line, Addr tag) const;
    int findTagInSetIgnorePermissions(int64_t cacheSet, Addr tag) const;

    // Private copy constructor and assignment operator
    CacheMemory(const CacheMemory& obj);
    CacheMemory& operator=(const CacheMemory& obj);

  private:
    // Data Members (m_prefix)
    bool m_is_instruction_only_cache;

    // The first index is the # of cache lines.
    // The second index is the the amount associativity.
    std::unordered_map<Addr, int> m_tag_index;
    std::vector<std::vector<AbstractCacheEntry*> > m_cache;
    /** Next way considered when selecting among equally invalid slots. */
    std::vector<int> m_next_invalid_way;

    /** We use the replacement policies from the Classic memory system. */
    replacement_policy::Base *m_replacementPolicy_ptr;

    /** Number of NIC DDIO allocation ways [0, D); -1 is no allocation. */
    int m_ddio_way_part;

    /** Avalanche the full line address into the set index when true. */
    bool m_addr_hash;

    BankedArray dataArray;
    BankedArray tagArray;
    ALUFreeListArray atomicALUArray;

    int m_cache_size;
    int m_cache_num_sets;
    int m_cache_num_set_bits;
    int m_cache_assoc;
    int m_start_index_bit;
    bool m_resource_stalls;
    int m_block_size;

    /**
     * We store all the ReplacementData in a 2-dimensional array. By doing
     * this, we can use all replacement policies from Classic system. Ruby
     * cache will deallocate cache entry every time we evict the cache block
     * so we cannot store the ReplacementData inside the cache entry.
     * Instantiate ReplacementData for multiple times will break replacement
     * policy like TreePLRU.
     */
    std::vector<std::vector<ReplData> > replacement_data;

    /**
     * Set to true when using WeightedLRU replacement policy, otherwise, set to
     * false.
     */
    bool m_use_occupancy;

    RubySystem *m_ruby_system = nullptr;

    Addr
    makeLineAddress(Addr addr) const
    {
        return ruby::makeLineAddress(addr, floorLog2(m_block_size));
    }

    private:
      struct CacheMemoryStats : public statistics::Group
      {
          CacheMemoryStats(statistics::Group *parent);

          statistics::Scalar numDataArrayReads;
          statistics::Scalar numDataArrayWrites;
          statistics::Scalar numTagArrayReads;
          statistics::Scalar numTagArrayWrites;

          statistics::Scalar numTagArrayStalls;
          statistics::Scalar numDataArrayStalls;

          statistics::Scalar numAtomicALUOperations;
          statistics::Scalar numAtomicALUArrayStalls;

          // hardware transactional memory
          statistics::Histogram htmTransCommitReadSet;
          statistics::Histogram htmTransCommitWriteSet;
          statistics::Histogram htmTransAbortReadSet;
          statistics::Histogram htmTransAbortWriteSet;

          statistics::Scalar m_demand_hits;
          statistics::Scalar m_demand_misses;
          statistics::Formula m_demand_accesses;

          statistics::Scalar m_prefetch_hits;
          statistics::Scalar m_prefetch_misses;
          statistics::Formula m_prefetch_accesses;

          statistics::Vector m_accessModeType;

          // MESI classified-DMA routing telemetry. These counters describe
          // proxy routing only and are separate from DDIO cache hit
          // statistics.
          statistics::Scalar dmaRoutingProxyRequests;
          statistics::Scalar dmaRoutingTransientRecycles;

          // DDIO (NIC RX data DMA write) accounting. A request is a packet
          // data line transaction seen by this cache; a hit means the line
          // was already present at acceptance.
          statistics::Scalar rxPayloadRequests;
          statistics::Scalar rxPayloadHits;
          statistics::Scalar rxPayloadMisses;
          statistics::Formula rxPayloadHitRate;
          statistics::Vector rxPayloadHitWays;
          statistics::Vector rxPayloadAllocWays;
          statistics::Vector ddioAllocWays;

          // RX header writes retain separate request/hit/miss telemetry.
          statistics::Scalar rxHeaderRequests;
          statistics::Scalar rxHeaderHits;
          statistics::Scalar rxHeaderMisses;
          statistics::Formula rxHeaderHitRate;

          // NIC TX payload DMA read line-transaction accounting
          statistics::Scalar txPayloadRequests;
          statistics::Scalar txPayloadHits;
          statistics::Scalar txPayloadMisses;
          statistics::Formula txPayloadHitRate;

          // Per-way, per-source accounting to understand eviction
          // pressure on the DDIO ways.  Source classes:
          //   0 = NIC RX payload write, 1 = NIC TX payload read,
          //   2 = NIC descriptor DMA, 3 = CPU/other, 4 = NIC RX header.
          // Each vector is [src][way] flattened (src*assoc + way).
          statistics::Vector ddioWayAccess;
          statistics::Vector ddioWayFill;
          statistics::Vector wayDeallocations;

          // CPU/general transactions to addresses previously written as RX
          // payload. These parameterize the processor-touch phase of the
          // RX-only DDIO residency model without using RX hit outcomes.
          statistics::Vector rxPayloadCpuAccessWays;
          statistics::Vector rxPayloadCpuFillWays;
          statistics::Scalar rxPayloadCpuUniqueLines;

          // Per-set payload access counters (set-index imbalance of the
          // payload stream on the DDIO ways).
          statistics::Vector rxPayloadSetHits;
          statistics::Vector rxPayloadSetMisses;

          // Unique payload line addresses seen in the current stats window.
          statistics::Scalar rxPayloadUniqueLines;
          statistics::Scalar txPayloadUniqueLines;
      } cacheMemoryStats;

      std::unordered_set<Addr> rxPayloadUniqueAddrs;
      std::unordered_set<Addr> txPayloadUniqueAddrs;
      std::unordered_set<Addr> rxPayloadCpuUniqueAddrs;
      // Unlike the per-window uniqueness set, retain this set across a stats
      // reset so post-reset CPU touches to warmed payload buffers are visible.
      std::unordered_set<Addr> rxPayloadEverAddrs;

    public:
      // These function increment the number of demand hits/misses by one
      // each time they are called
      void profileDemandHit();
      void profileDemandMiss();
      void profilePrefetchHit();
      void profilePrefetchMiss();

      void profileDmaRoutingProxy()
      {
          cacheMemoryStats.dmaRoutingProxyRequests++;
      }
      void profileDmaRoutingTransientRecycle()
      {
          cacheMemoryStats.dmaRoutingTransientRecycles++;
      }

      // DDIO accounting hooks (called from the CHI home node).
      // profileRxPayload/profileRxHeader count one line transaction and record
      // whether the line was present at acceptance. Allocation ways are
      // recorded by profileDdioWayFill().
      void profileRxPayload(Addr address);
      void profileRxHeader(Addr address);
      void profileTxPayload(Addr address);

      // Per-way, per-source accounting hooks (see CacheMemoryStats for
      // the source classes).
      void profileDdioWayAccess(int src, int way, Addr address)
      {
          if (way >= 0) {
              cacheMemoryStats.ddioWayAccess[src * m_cache_assoc + way]++;
              if (src == 3 && rxPayloadEverAddrs.count(address)) {
                  cacheMemoryStats.rxPayloadCpuAccessWays[way]++;
                  if (rxPayloadCpuUniqueAddrs.insert(address).second)
                      cacheMemoryStats.rxPayloadCpuUniqueLines++;
              }
          }
      }
      void profileDdioWayFill(int src, int way, Addr address)
      {
          if (way >= 0) {
              cacheMemoryStats.ddioWayFill[src * m_cache_assoc + way]++;
              if (m_ddio_way_part > 0 && way < m_ddio_way_part &&
                  (src == 0 || src == 2 || src == 4)) {
                  cacheMemoryStats.ddioAllocWays[way]++;
                  if (src == 0)
                      cacheMemoryStats.rxPayloadAllocWays[way]++;
              }
              if (src == 3 && rxPayloadEverAddrs.count(address)) {
                  cacheMemoryStats.rxPayloadCpuFillWays[way]++;
                  if (rxPayloadCpuUniqueAddrs.insert(address).second)
                      cacheMemoryStats.rxPayloadCpuUniqueLines++;
              }
          }
      }

      // NIC DMA classification helpers (SLICC-visible; the gem5 Request
      // carries the NIC category flags end-to-end as seqReq).
      bool isNicRxWriteReq(const RequestPtr &req) const
      {
          return req &&
              (req->isNicRxPayloadWrite() || req->isNicRxHeaderWrite());
      }
      bool isNicDdioWriteReq(const RequestPtr &req) const
      {
          return req && req->isNicDmaWrite();
      }
      bool isNicRxPayloadWriteReq(const RequestPtr &req) const
      {
          return req && req->isNicRxPayloadWrite();
      }
      bool isNicRxHeaderWriteReq(const RequestPtr &req) const
      {
          return req && req->isNicRxHeaderWrite();
      }
      bool isNicDdioReadReq(const RequestPtr &req) const
      {
          return req && req->isNicDmaRead();
      }
      bool isNicTxPayloadReadReq(const RequestPtr &req) const
      {
          return req && req->isNicTxPayloadRead();
      }
      bool ddioWriteNeedsRead(bool partial, bool data_valid) const
      {
          return partial && !data_valid;
      }
      bool isNicDescDmaReq(const RequestPtr &req) const
      {
          return req && req->isNicDescDma();
      }
};

std::ostream& operator<<(std::ostream& out, const CacheMemory& obj);

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_STRUCTURES_CACHEMEMORY_HH__
