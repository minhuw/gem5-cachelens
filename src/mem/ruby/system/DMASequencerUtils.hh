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

#ifndef __MEM_RUBY_SYSTEM_DMASEQUENCERUTILS_HH__
#define __MEM_RUBY_SYSTEM_DMASEQUENCERUTILS_HH__

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mem/request.hh"
#include "mem/ruby/common/DataBlock.hh"
#include "mem/ruby/common/WriteMask.hh"

namespace gem5
{

namespace ruby
{

/**
 * Metadata for one cache-line-bounded geometric fragment of a DMA request.
 *
 * accessMask is the intersection of this geometric extent and the original
 * request byte-enable vector. requestOffset identifies the corresponding
 * bytes in the packet payload, while physicalAddress identifies their first
 * physical byte. A sparse request may therefore produce an empty fragment;
 * callers must skip it rather than issuing an uncompletable protocol request.
 */
struct DMARequestFragment
{
    DMARequestFragment(Addr physical_address, Addr line_address,
                       int request_offset, int len, int block_size,
                       const RequestPtr &seq_req,
                       const std::vector<bool> &byte_enable)
        : physicalAddress(physical_address), lineAddress(line_address),
          requestOffset(request_offset), len(len), accessMask(block_size),
          seqReq(seq_req)
    {
        assert(requestOffset >= 0);
        assert(len > 0);
        assert(physicalAddress >= lineAddress);
        assert(static_cast<size_t>(requestOffset + len) <=
               byte_enable.size());
        const int line_offset =
            static_cast<int>(physicalAddress - lineAddress);
        assert(line_offset + len <= block_size);

        for (int byte = 0; byte < len; ++byte) {
            if (byte_enable[requestOffset + byte]) {
                accessMask.setMask(line_offset + byte, 1);
            }
        }
    }

    bool hasEnabledBytes() const { return !accessMask.isEmpty(); }
    int lineOffset() const
    {
        return static_cast<int>(physicalAddress - lineAddress);
    }
    int requestOffsetForLineByte(int line_byte) const
    {
        assert(line_byte >= lineOffset());
        assert(line_byte < lineOffset() + len);
        return requestOffset + line_byte - lineOffset();
    }

    Addr physicalAddress;
    Addr lineAddress;
    int requestOffset;
    int len;
    WriteMask accessMask;
    RequestPtr seqReq;
};

inline DMARequestFragment
makeDMARequestFragment(Addr start_paddr, int total_len, int bytes_completed,
                       int block_size, const RequestPtr &seq_req,
                       const std::vector<bool> &byte_enable)
{
    assert(total_len > 0);
    assert(bytes_completed >= 0);
    assert(bytes_completed < total_len);
    assert(block_size > 0);
    assert(byte_enable.size() == total_len);

    const Addr physical_address = start_paddr + bytes_completed;
    const Addr line_address =
        physical_address - (physical_address % block_size);
    const int offset = static_cast<int>(physical_address - line_address);
    const int remaining = total_len - bytes_completed;
    const int available = block_size - offset;
    const int fragment_len = remaining < available ? remaining : available;

    return DMARequestFragment(physical_address, line_address, bytes_completed,
                              fragment_len, block_size, seq_req, byte_enable);
}

/** Place packet bytes at their physical offsets in a line-sized DataBlock. */
inline void
setDMARequestFragmentData(DataBlock &block,
                          const DMARequestFragment &fragment,
                          const uint8_t *request_data)
{
    assert(request_data != nullptr);
    assert(block.getBlockSize() == fragment.accessMask.getBlockSize());
    block.setData(request_data + fragment.requestOffset,
                  fragment.lineOffset(), fragment.len);
}

inline bool
hasEnabledDMABytes(const std::vector<bool> &byte_enable)
{
    return std::find(byte_enable.begin(), byte_enable.end(), true) !=
           byte_enable.end();
}

/**
 * Tracks exclusive ownership of every cache line touched by enabled bytes of
 * active requests.
 *
 * DMASequencer issues one non-empty fragment at a time but may have multiple
 * whole requests outstanding. Reserving the complete enabled-line set before
 * admission makes each fragment callback map to exactly one request-table
 * entry, without spuriously reserving geometrically covered all-disabled
 * lines.
 */
class DMARequestLineReservations
{
  public:
    bool
    tryReserve(Addr start_address, int total_len, int block_size,
               const std::vector<bool> &byte_enable,
               Addr &request_address)
    {
        if (total_len <= 0 || block_size <= 0 ||
            byte_enable.size() != static_cast<size_t>(total_len) ||
            !hasEnabledDMABytes(byte_enable)) {
            return false;
        }

        const Addr span = static_cast<Addr>(total_len - 1);
        if (start_address > std::numeric_limits<Addr>::max() - span) {
            return false;
        }

        std::vector<Addr> lines;
        for (int completed = 0; completed < total_len;) {
            const auto fragment = makeDMARequestFragment(
                start_address, total_len, completed, block_size, nullptr,
                byte_enable);
            if (fragment.hasEnabledBytes()) {
                lines.push_back(fragment.lineAddress);
            }
            completed += fragment.len;
        }
        assert(!lines.empty());

        const Addr candidate_request_address = lines.front();
        if (m_requestToLines.find(candidate_request_address) !=
            m_requestToLines.end()) {
            return false;
        }
        for (const Addr line : lines) {
            if (m_lineToRequest.find(line) != m_lineToRequest.end()) {
                return false;
            }
        }

        auto request = m_requestToLines.emplace(
            candidate_request_address, std::move(lines));
        if (!request.second) {
            return false;
        }
        for (const Addr line : request.first->second) {
            const bool inserted = m_lineToRequest.emplace(
                line, candidate_request_address).second;
            if (!inserted) {
                for (const Addr rollback_line : request.first->second) {
                    const auto rollback = m_lineToRequest.find(rollback_line);
                    if (rollback != m_lineToRequest.end() &&
                        rollback->second == candidate_request_address) {
                        m_lineToRequest.erase(rollback);
                    }
                }
                m_requestToLines.erase(request.first);
                return false;
            }
        }
        request_address = candidate_request_address;
        return true;
    }

    bool
    tryReserve(Addr start_address, int total_len, int block_size)
    {
        if (total_len <= 0) {
            return false;
        }
        std::vector<bool> byte_enable(total_len, true);
        Addr request_address = 0;
        return tryReserve(start_address, total_len, block_size, byte_enable,
                          request_address);
    }

    bool
    findRequest(Addr line_address, Addr &request_address) const
    {
        const auto reservation = m_lineToRequest.find(line_address);
        if (reservation == m_lineToRequest.end()) {
            return false;
        }

        request_address = reservation->second;
        return true;
    }

    bool
    release(Addr request_address)
    {
        const auto request = m_requestToLines.find(request_address);
        if (request == m_requestToLines.end()) {
            return false;
        }

        for (const Addr line : request->second) {
            const auto reservation = m_lineToRequest.find(line);
            if (reservation == m_lineToRequest.end() ||
                reservation->second != request_address) {
                return false;
            }
        }
        for (const Addr line : request->second) {
            m_lineToRequest.erase(line);
        }
        m_requestToLines.erase(request);
        return true;
    }

    size_t size() const { return m_lineToRequest.size(); }

  private:
    std::unordered_map<Addr, Addr> m_lineToRequest;
    std::unordered_map<Addr, std::vector<Addr>> m_requestToLines;
};

/** Masked writes require an explicitly capable Ruby protocol path. */
inline bool
isSupportedDMARequest(bool is_masked_write, bool supports_masked_writes)
{
    return !is_masked_write || supports_masked_writes;
}

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_SYSTEM_DMASEQUENCERUTILS_HH__
