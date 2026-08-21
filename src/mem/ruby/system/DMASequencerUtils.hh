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

#include <cassert>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mem/request.hh"
#include "mem/ruby/common/WriteMask.hh"

namespace gem5
{

namespace ruby
{

/**
 * Metadata for one cache-line-bounded fragment of a DMA request.
 *
 * The original RequestPtr remains shared by every fragment while the access
 * address, length, and mask describe only this fragment.
 */
struct DMARequestFragment
{
    DMARequestFragment(Addr physical_address, Addr line_address, int len,
                       int block_size, const RequestPtr &seq_req)
        : physicalAddress(physical_address), lineAddress(line_address),
          len(len), accessMask(block_size), seqReq(seq_req)
    {
        assert(len > 0);
        assert(physicalAddress >= lineAddress);
        const int offset = static_cast<int>(physicalAddress - lineAddress);
        assert(offset + len <= block_size);
        accessMask.setMask(offset, len);
    }

    Addr physicalAddress;
    Addr lineAddress;
    int len;
    WriteMask accessMask;
    RequestPtr seqReq;
};

inline DMARequestFragment
makeDMARequestFragment(Addr start_paddr, int total_len, int bytes_completed,
                       int block_size, const RequestPtr &seq_req)
{
    assert(total_len > 0);
    assert(bytes_completed >= 0);
    assert(bytes_completed < total_len);
    assert(block_size > 0);

    const Addr physical_address = start_paddr + bytes_completed;
    const Addr line_address =
        physical_address - (physical_address % block_size);
    const int offset = static_cast<int>(physical_address - line_address);
    const int remaining = total_len - bytes_completed;
    const int available = block_size - offset;
    const int fragment_len = remaining < available ? remaining : available;

    return DMARequestFragment(physical_address, line_address, fragment_len,
                              block_size, seq_req);
}

/**
 * Tracks exclusive ownership of every cache line touched by active requests.
 *
 * DMASequencer issues one fragment at a time but may have multiple whole
 * requests outstanding. Reserving the complete line span before admission
 * makes each fragment callback map to exactly one request-table entry.
 */
class DMARequestLineReservations
{
  public:
    bool
    tryReserve(Addr start_address, int total_len, int block_size)
    {
        assert(total_len > 0);
        assert(block_size > 0);

        const Addr block_size_addr = static_cast<Addr>(block_size);
        const Addr first_line =
            start_address - start_address % block_size_addr;
        const Addr span = static_cast<Addr>(total_len - 1);
        assert(start_address <= std::numeric_limits<Addr>::max() - span);
        const Addr last_address = start_address + span;
        const Addr last_line =
            last_address - last_address % block_size_addr;

        std::vector<Addr> lines;
        for (Addr line = first_line;; line += block_size_addr) {
            lines.push_back(line);
            if (line == last_line) {
                break;
            }
            assert(line <=
                   std::numeric_limits<Addr>::max() - block_size_addr);
        }

        for (const Addr line : lines) {
            if (m_lineToRequest.find(line) != m_lineToRequest.end()) {
                return false;
            }
        }

        auto request = m_requestToLines.emplace(first_line, std::move(lines));
        assert(request.second);
        for (const Addr line : request.first->second) {
            const auto reservation = m_lineToRequest.emplace(line, first_line);
            assert(reservation.second);
        }
        return true;
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
            assert(reservation != m_lineToRequest.end());
            assert(reservation->second == request_address);
            m_lineToRequest.erase(reservation);
        }
        m_requestToLines.erase(request);
        return true;
    }

    size_t
    size() const
    {
        return m_lineToRequest.size();
    }

  private:
    std::unordered_map<Addr, Addr> m_lineToRequest;
    std::unordered_map<Addr, std::vector<Addr>> m_requestToLines;
};

/** Masked DMA writes remain unsupported until their merge semantics exist. */
inline bool
isSupportedDMARequest(bool is_masked_write)
{
    return !is_masked_write;
}

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_SYSTEM_DMASEQUENCERUTILS_HH__
