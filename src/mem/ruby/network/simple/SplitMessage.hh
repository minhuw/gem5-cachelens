// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __MEM_RUBY_NETWORK_SIMPLE_SPLIT_MESSAGE_HH__
#define __MEM_RUBY_NETWORK_SIMPLE_SPLIT_MESSAGE_HH__

#include <cstdint>
#include <vector>

#include "mem/request.hh"
#include "mem/ruby/slicc_interface/Message.hh"

namespace gem5
{
namespace ruby
{

class RubySystem;

namespace split
{

/**
 * MESI_Three_Level value-message wire format, version 1, fixed 64-byte lines.
 * All multibyte integers use big endian; no C++ object layout is transmitted.
 *
 * Header: magic u32 ("MES3"), version u16, kind u8 (request=1/response=2),
 * reserved u8 (zero), block size u32, total frame bytes u32.
 * Transport: creation/last enqueue/delayed ticks/counter u64 each, incoming
 * link/vnet i32 each. Body fields follow the generated schema order. Enums
 * and nonnegative lengths are u32, addresses u64, booleans u8, MachineIDs
 * (type u32, number u32), NetDest (count u32, sorted MachineIDs), DataBlock
 * 64 individual bytes, accessMask 8 bytes (bit i%8 is line byte i).
 * AckCount uses two's-complement i32. seqReq is a presence boolean, which
 * must be false, as must isSeqReqValid. There is no pointer payload.
 *
 * Fatal on unsupported types, non-64B data/masks, atomic functors/data logs,
 * seqReq state, or malformed wire data. At most 65,536 machines/destinations
 * are supported. decode validates IDs against the supplied RubySystem;
 * encode has no RubySystem argument and checks representational bounds.
 * Neither function enqueues the message or advances its transport state.
 */
std::vector<uint8_t> encode(const MsgPtr &message);
MsgPtr decode(const std::vector<uint8_t> &bytes, RubySystem *ruby_system);

/**
 * Whether this build carries a message codec for the protocol of
 * @p ruby_system. Builds of other protocols link SplitMessageUnsupported.cc,
 * whose functions are unreachable once a bridge has checked this.
 */
bool supportsProtocol(RubySystem *ruby_system);

/** Process-local DMA request provenance, which never enters the codec. */
struct LocalRequest
{
    RequestPtr request;
    bool valid = false;
};

/** Move local provenance out of @p message, leaving it pointer free. */
LocalRequest detachLocalRequest(Message &message);

/** Restore provenance taken by detachLocalRequest before delivery. */
void attachLocalRequest(Message &message, const LocalRequest &local);

} // namespace split
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_SIMPLE_SPLIT_MESSAGE_HH__
