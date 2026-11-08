// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

// CHI support for the single-process thread bridge. CHI messages cross a
// partition only as message clones (value_transport); there is no wire
// codec, so encode/decode are unreachable once the bridge has checked
// hasCodec(). A CHIRequestMsg carries the process-local seqReq provenance
// of the originating CPU or DMA request; it is moved out of the clone before
// staging and restored before delivery, exactly as for MESI_Three_Level.
// CHIResponseMsg and CHIDataMsg hold no such pointers (WriteMask atomic
// functors are shared by clones, as by native multicast clones).

#include "base/logging.hh"
#include "mem/ruby/network/simple/SplitMessage.hh"
#include "mem/ruby/protocol/CHI/CHIRequestMsg.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{
namespace ruby
{
namespace split
{

bool
supportsProtocol(RubySystem *ruby_system)
{
    return ruby_system->getProtocolInfo().getName() == "CHI";
}

bool
hasCodec()
{
    return false;
}

std::vector<uint8_t>
encode(const MsgPtr &)
{
    panic("SplitMessage: CHI has no wire codec; use value_transport");
}

MsgPtr
decode(const std::vector<uint8_t> &, RubySystem *)
{
    panic("SplitMessage: CHI has no wire codec; use value_transport");
}

LocalRequest
detachLocalRequest(Message &message)
{
    LocalRequest local;
    if (auto *request = dynamic_cast<CHI::CHIRequestMsg *>(&message)) {
        local.request = request->getseqReq();
        local.valid = request->getisSeqReqValid();
        fatal_if(bool(local.request) != local.valid,
                 "Inconsistent local request provenance");
        request->setseqReq(nullptr);
        request->setisSeqReqValid(false);
    }
    return local;
}

void
attachLocalRequest(Message &message, const LocalRequest &local)
{
    if (auto *request = dynamic_cast<CHI::CHIRequestMsg *>(&message)) {
        request->setseqReq(local.request);
        request->setisSeqReqValid(local.valid);
    } else {
        fatal_if(local.request || local.valid,
                 "Local request provenance attached to non-request message");
    }
}

} // namespace split
} // namespace ruby
} // namespace gem5
