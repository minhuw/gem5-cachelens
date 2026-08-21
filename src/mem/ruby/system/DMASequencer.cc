/*
 * Copyright (c) 2021 ARM Limited
 * All rights reserved.
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
 * Copyright (c) 2008 Mark D. Hill and David A. Wood
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

#include "mem/ruby/system/DMASequencer.hh"

#include <limits>
#include <memory>
#include <utility>

#include "base/logging.hh"
#include "debug/RubyDma.hh"
#include "debug/RubyStats.hh"
#include "mem/ruby/protocol/SequencerMsg.hh"
#include "mem/ruby/protocol/SequencerRequestType.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "sim/eventq.hh"

namespace gem5
{

namespace ruby
{

namespace
{

void
setFragmentMetadata(SequencerMsg &msg, const DMARequestFragment &fragment)
{
    msg.getPhysicalAddress() = fragment.physicalAddress;
    msg.getLineAddress() = fragment.lineAddress;
    msg.getLen() = fragment.len;
    msg.getwriteMask() = fragment.accessMask;
    msg.getseqReq() = fragment.seqReq;
    msg.getisSeqReqValid() = fragment.seqReq != nullptr;
}

} // anonymous namespace

DMARequest::DMARequest(uint64_t start_paddr, int len, bool write,
                       int bytes_completed, int bytes_issued, uint8_t *data,
                       PacketPtr pkt, std::vector<bool> byte_enable)
    : start_paddr(start_paddr), len(len), write(write),
      bytes_completed(bytes_completed), bytes_issued(bytes_issued), data(data),
      pkt(pkt), seqReq(pkt ? pkt->req : nullptr),
      byteEnable(std::move(byte_enable))
{
}

DMASequencer::DMASequencer(const Params &p)
    : RubyPort(p), m_outstanding_count(0),
      m_max_outstanding_requests(p.max_outstanding_requests),
      m_supports_masked_writes(p.supports_masked_writes)
{
}

void
DMASequencer::init()
{
    RubyPort::init();
    m_data_block_mask = mask(m_ruby_system->getBlockSizeBits());
}

RequestStatus
DMASequencer::makeRequest(PacketPtr pkt)
{
    const bool is_masked_write = pkt->isMaskedWrite();
    panic_if(!isSupportedDMARequest(is_masked_write,
                                    m_supports_masked_writes),
             "DMASequencer %s rejected a masked DMA write because "
             "supports_masked_writes is false",
             name());

    if (m_outstanding_count >= m_max_outstanding_requests) {
        return RequestStatus_BufferFull;
    }

    const Addr paddr = pkt->getAddr();
    const int len = pkt->getSize();
    const bool write = pkt->isWrite();
    panic_if(len <= 0, "DMA request at %#x has invalid size %d", paddr, len);
    panic_if(paddr > std::numeric_limits<Addr>::max() -
                     static_cast<Addr>(len - 1),
             "DMA request at %#x with size %d wraps the address space",
             paddr, len);

    std::vector<bool> byte_enable(len, true);
    if (write) {
        byte_enable = pkt->req->getByteEnable();
        panic_if(byte_enable.size() != static_cast<size_t>(len),
                 "DMA write at %#x has %zu byte enables for %d bytes",
                 paddr, byte_enable.size(), len);
    }

    // An all-disabled write is a successful no-op. Complete it on the next
    // Ruby clock edge so recvTimingReq can return before its response arrives.
    if (write && !hasEnabledDMABytes(byte_enable)) {
        ++m_outstanding_count;
        schedule(new EventFunctionWrapper(
                     [this, pkt] { completeZeroEnabledRequest(pkt); },
                     name() + ".zero_enabled_dma", true),
                 clockEdge(Cycles(1)));
        return RequestStatus_Issued;
    }

    uint8_t *data = write
        ? const_cast<uint8_t *>(pkt->getConstPtr<uint8_t>())
        : pkt->getPtr<uint8_t>();

    assert(m_outstanding_count < m_max_outstanding_requests);
    const int blk_size = m_ruby_system->getBlockSizeBytes();
    Addr request_address = 0;

    // Reserve exactly the lines carrying enabled bytes before admission.
    // Reads use an all-enabled mask and retain their existing full-span
    // reservation behavior.
    if (!m_lineReservations.tryReserve(
            paddr, len, blk_size, byte_enable, request_address)) {
        DPRINTF(RubyDma, "DMA aliased: addr %p, len %d\n",
                makeLineAddress(paddr), len);
        return RequestStatus_Aliased;
    }

    auto emplace_pair = m_RequestTable.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(request_address),
        std::forward_as_tuple(paddr, len, write, 0, 0, data, pkt,
                              std::move(byte_enable)));
    if (!emplace_pair.second) {
        panic_if(!m_lineReservations.release(request_address),
                 "DMA request-table collision left invalid reservations");
        return RequestStatus_Aliased;
    }

    DPRINTF(RubyDma, "DMA req created: addr %p, len %d\n",
            request_address, len);

    ++m_outstanding_count;
    issueRequestFragment(request_address);
    return RequestStatus_Issued;
}

void
DMASequencer::issueRequestFragment(const Addr &request_address)
{
    auto request_it = m_RequestTable.find(request_address);
    assert(request_it != m_RequestTable.end());
    DMARequest &active_request = request_it->second;
    const int blk_size = m_ruby_system->getBlockSizeBytes();

    int next_offset = active_request.bytes_completed;
    while (next_offset < active_request.len) {
        const auto candidate = makeDMARequestFragment(
            active_request.start_paddr, active_request.len, next_offset,
            blk_size, active_request.seqReq, active_request.byteEnable);
        if (candidate.hasEnabledBytes()) {
            active_request.bytes_completed = candidate.requestOffset;
            active_request.bytes_issued =
                candidate.requestOffset + candidate.len;

            auto msg = std::make_shared<SequencerMsg>(
                clockEdge(), blk_size, m_ruby_system);
            setFragmentMetadata(*msg, candidate);

            if (active_request.pkt->req->isAtomic()) {
                msg->setType(SequencerRequestType_ATOMIC);

                // Atomic DMA requests remain single-line operations.
                const int atomic_offset =
                    active_request.pkt->getAddr() - candidate.lineAddress;
                assert(candidate.requestOffset == 0);
                assert(candidate.len == active_request.len);
                assert(atomic_offset + active_request.pkt->getSize() <=
                       blk_size);

                std::vector<std::pair<int, AtomicOpFunctor *>> atomic_ops;
                atomic_ops.emplace_back(
                    atomic_offset, active_request.pkt->getAtomicOp());
                msg->getwriteMask().setAtomicOps(atomic_ops);
            } else if (active_request.write) {
                msg->setType(SequencerRequestType_ST);
            } else {
                assert(active_request.pkt->isRead());
                msg->setType(SequencerRequestType_LD);
            }

            if (active_request.write && active_request.data != nullptr) {
                setDMARequestFragmentData(
                    msg->getDataBlk(), candidate, active_request.data);
            }

            assert(m_mandatory_q_ptr != nullptr);
            m_mandatory_q_ptr->enqueue(
                msg, clockEdge(), cyclesToTicks(Cycles(1)),
                m_ruby_system->getRandomization(),
                m_ruby_system->getWarmupEnabled());

            DPRINTF(RubyDma,
                    "DMA fragment issued: request %p, line %p, "
                    "offset %d, len %d, enabled %d\n",
                    request_address, candidate.lineAddress,
                    candidate.requestOffset, candidate.len,
                    candidate.accessMask.count());
            return;
        }
        next_offset += candidate.len;
    }

    completeRequest(request_address);
}

void
DMASequencer::issueNext(const Addr &request_address)
{
    auto request_it = m_RequestTable.find(request_address);
    assert(request_it != m_RequestTable.end());

    DMARequest &active_request = request_it->second;
    assert(m_outstanding_count <= m_max_outstanding_requests);
    active_request.bytes_completed = active_request.bytes_issued;
    issueRequestFragment(request_address);
}

void
DMASequencer::completeRequest(const Addr &request_address)
{
    auto request_it = m_RequestTable.find(request_address);
    assert(request_it != m_RequestTable.end());
    DMARequest &active_request = request_it->second;

    DPRINTF(RubyDma, "DMA request completed: addr %p, size %d\n",
            request_address, active_request.len);
    PacketPtr pkt = active_request.pkt;
    panic_if(!m_lineReservations.release(request_address),
             "Completed DMA request %#x had no line reservations",
             request_address);
    --m_outstanding_count;
    m_RequestTable.erase(request_it);
    ruby_hit_callback(pkt);
    testDrainComplete();
}

void
DMASequencer::completeZeroEnabledRequest(PacketPtr pkt)
{
    assert(m_outstanding_count > 0);
    --m_outstanding_count;
    ruby_hit_callback(pkt);
    testDrainComplete();
}

Addr
DMASequencer::requestAddressForCallback(const Addr &address) const
{
    Addr request_address = 0;
    panic_if(!m_lineReservations.findRequest(address, request_address),
             "DMA callback for unreserved line %#x", address);
    return request_address;
}

void
DMASequencer::dataCallback(const DataBlock &dblk, const Addr &address)
{
    const Addr request_address = requestAddressForCallback(address);
    auto request_it = m_RequestTable.find(request_address);
    assert(request_it != m_RequestTable.end());

    DMARequest &active_request = request_it->second;
    const int len =
        active_request.bytes_issued - active_request.bytes_completed;
    const Addr fragment_address =
        active_request.start_paddr + active_request.bytes_completed;
    const int offset = fragment_address & m_data_block_mask;
    assert(!active_request.write);
    if (active_request.data != nullptr) {
        memcpy(&active_request.data[active_request.bytes_completed],
               dblk.getData(offset, len), len);
    }
    issueNext(request_address);
}

void
DMASequencer::ackCallback(const Addr &address)
{
    const Addr request_address = requestAddressForCallback(address);
    assert(m_RequestTable.find(request_address) != m_RequestTable.end());
    issueNext(request_address);
}

void
DMASequencer::atomicCallback(const DataBlock &dblk, const Addr &address)
{
    const Addr request_address = requestAddressForCallback(address);
    auto request_it = m_RequestTable.find(request_address);
    assert(request_it != m_RequestTable.end());

    DMARequest &active_request = request_it->second;
    PacketPtr pkt = active_request.pkt;

    const int offset = active_request.start_paddr & m_data_block_mask;
    memcpy(pkt->getPtr<uint8_t>(), dblk.getData(offset, pkt->getSize()),
           pkt->getSize());

    panic_if(!m_lineReservations.release(request_address),
             "Completed atomic DMA request %#x had no line reservations",
             request_address);
    --m_outstanding_count;
    m_RequestTable.erase(request_it);
    ruby_hit_callback(pkt);
    testDrainComplete();
}

void
DMASequencer::recordRequestType(DMASequencerRequestType requestType)
{
    DPRINTF(RubyStats, "Recorded statistic: %s\n",
            DMASequencerRequestType_to_string(requestType));
}

} // namespace ruby
} // namespace gem5
