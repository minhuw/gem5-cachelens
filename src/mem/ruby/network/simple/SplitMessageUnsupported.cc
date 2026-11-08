// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

// Protocols without SplitSim message support (neither MESI_Three_Level nor
// CHI). The thread bridge still builds, so serial runs keep one binary per
// protocol, but its constructor rejects the protocol through
// supportsProtocol() before any message can cross.

#include "base/logging.hh"
#include "mem/ruby/network/simple/SplitMessage.hh"

namespace gem5
{
namespace ruby
{
namespace split
{

bool
supportsProtocol(RubySystem *)
{
    return false;
}

bool
hasCodec()
{
    return false;
}

std::vector<uint8_t>
encode(const MsgPtr &)
{
    panic("SplitMessage: no codec for this Ruby protocol");
}

MsgPtr
decode(const std::vector<uint8_t> &, RubySystem *)
{
    panic("SplitMessage: no codec for this Ruby protocol");
}

LocalRequest
detachLocalRequest(Message &)
{
    panic("SplitMessage: no codec for this Ruby protocol");
}

void
attachLocalRequest(Message &, const LocalRequest &)
{
    panic("SplitMessage: no codec for this Ruby protocol");
}

} // namespace split
} // namespace ruby
} // namespace gem5
