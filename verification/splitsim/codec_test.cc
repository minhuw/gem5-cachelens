// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

// Codec mode runs this value-only check after controller registration and
// initial workload loads. It uses the real RubySystem topology and codec,
// without enqueueing messages or changing controller state.

#include "mem/ruby/network/simple/SplitMessage.hh"

#include <limits>

#include "base/logging.hh"
#include "mem/ruby/protocol/MESI_Three_Level/RequestMsg.hh"
#include "mem/ruby/protocol/MESI_Three_Level/ResponseMsg.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{
namespace ruby
{
namespace split
{
namespace
{

using namespace MESI_Three_Level;

void
check(bool condition, const char *field)
{
    fatal_if(!condition, "SplitMessage directed test failed: %s", field);
}

void
checkTransport(const Message &message)
{
    check(message.getTime() == 17, "creation time");
    check(message.getLastEnqueueTime() == 83, "last enqueue time");
    check(message.getDelayedTicks() == 41, "accumulated delay");
    check(message.getMsgCounter() == 0xfedcba9876543210ULL, "message counter");
    check(message.getIncomingLink() == 9, "incoming link");
    check(message.getVnet() == 2, "vnet");
}

uint8_t
pattern(unsigned i)
{
    return static_cast<uint8_t>(0x81 + 37 * i);
}

} // anonymous namespace

void
testCodec(RubySystem *rs)
{
    check(rs && rs->getBlockSizeBytes() == 64, "64-byte RubySystem");
    std::vector<MachineID> ids;
    for (int type = MachineType_FIRST; type < MachineType_NUM; ++type) {
        const auto machine_type = static_cast<MachineType>(type);
        const int count = rs->MachineType_base_count(machine_type);
        if (count) {
            ids.emplace_back(machine_type, 0);
            if (count > 1)
                ids.emplace_back(machine_type, count - 1);
        }
    }
    check(ids.size() >= 2, "topology with multiple machines");
    NetDest destination(rs);
    for (const auto &id : ids)
        destination.add(id);

    // Exercise every request enum, including pointer-free DMA and DDIO, with
    // high addresses, discontiguous masks, all data bytes, and multicast IDs.
    for (int type = CoherenceRequestType_FIRST;
         type < CoherenceRequestType_NUM; ++type) {
        auto request = std::make_shared<RequestMsg>(17, 64, rs);
        check(request->getIncomingLink() == -1 && request->getVnet() == -1,
              "default network state");
        request->setaddr(0x123456789abcdec0ULL);
        request->setType(static_cast<CoherenceRequestType>(type));
        request->setAccessMode(RubyAccessMode_Device);
        request->setRequestor(ids.back());
        request->setDestination(destination);
        request->setMessageSize(MessageSizeType_Writeback_Data);
        request->setLen(61);
        request->setaccessAddr(0x123456789abcdec3ULL);
        request->setaccessSize(61);
        request->setddioEligible(true);
        request->setDirty(true);
        request->setPrefetch(PrefetchBit_L2_HW);
        for (unsigned i = 0; i < 64; ++i) {
            request->getDataBlk().setByte(i, pattern(i));
            request->getaccessMask().setMask(i, 1, i % 3 == 0 || i == 63);
        }
        request->restoreTransportState(17, 83, 41, 0xfedcba9876543210ULL,
                                       9, 2);
        const auto bytes = encode(request);
        check(bytes.size() == 184 + 8 * ids.size(), "request frame length");
        check(bytes[0] == 'M' && bytes[1] == 'E' && bytes[2] == 'S' &&
              bytes[3] == '3' && bytes[4] == 0 && bytes[5] == 1,
              "magic and big-endian version");
        check(request->getLastEnqueueTime() == 83 &&
              request->getDelayedTicks() == 41 &&
              request->getDestination().isEqual(destination),
              "encode leaves sender state intact");
        const auto decoded = std::dynamic_pointer_cast<RequestMsg>(
            decode(bytes, rs));
        check(decoded != nullptr, "request dynamic type");
        check(decoded->getaddr() == 0x123456789abcdec0ULL, "request address");
        check(decoded->getType() == type, "request type");
        check(decoded->getAccessMode() == RubyAccessMode_Device, "access mode");
        check(decoded->getRequestor() == ids.back(), "requestor");
        check(decoded->getDestination().isEqual(destination), "destinations");
        check(decoded->getMessageSize() == MessageSizeType_Writeback_Data,
              "request message size");
        check(decoded->getLen() == 61, "Len");
        check(decoded->getaccessAddr() == 0x123456789abcdec3ULL,
              "fragment address");
        check(decoded->getaccessSize() == 61, "fragment size");
        check(decoded->getseqReq() == nullptr && !decoded->getisSeqReqValid(),
              "pointer-free sequence state");
        check(decoded->getddioEligible() && decoded->getDirty(),
              "DDIO eligibility and dirty state");
        check(decoded->getPrefetch() == PrefetchBit_L2_HW, "prefetch");
        for (unsigned i = 0; i < 64; ++i) {
            check(decoded->getDataBlk().getByte(i) == pattern(i),
                  "request data byte");
            check(decoded->getaccessMask().test(i) == (i % 3 == 0 || i == 63),
                  "noncontiguous access mask bit");
        }
        checkTransport(*decoded);
        // Receiver's regular enqueue can now accumulate the original delay.
        decoded->updateDelayedTicks(101);
        check(decoded->getDelayedTicks() == 59, "receiver delay accumulation");
    }

    // Both signs are meaningful in MESI: data responses can carry negative
    // outstanding-ack counts, while an acknowledgement usually contributes +1.
    for (int type = CoherenceResponseType_FIRST;
         type < CoherenceResponseType_NUM; ++type) {
        for (const int acks : {-1, 0, 1}) {
            auto response = std::make_shared<ResponseMsg>(17, 64, rs);
            response->setaddr(0xfedcba9876543200ULL);
            response->setType(static_cast<CoherenceResponseType>(type));
            response->setSender(ids.front());
            response->setDestination(destination);
            response->setDirty(acks != 0);
            response->setAckCount(acks);
            response->setMessageSize(MessageSizeType_Response_Data);
            for (unsigned i = 0; i < 64; ++i)
                response->getDataBlk().setByte(i, pattern(63 - i));
            response->restoreTransportState(17, 83, 41,
                                            0xfedcba9876543210ULL, 9, 2);
            const auto bytes = encode(response);
            check(bytes.size() == 153 + 8 * ids.size(), "response frame length");
            const auto decoded = std::dynamic_pointer_cast<ResponseMsg>(
                decode(bytes, rs));
            check(decoded != nullptr, "response dynamic type");
            check(decoded->getaddr() == 0xfedcba9876543200ULL,
                  "response address");
            check(decoded->getType() == type, "response type");
            check(decoded->getSender() == ids.front(), "sender");
            check(decoded->getDestination().isEqual(destination),
                  "response destinations");
            check(decoded->getDirty() == (acks != 0), "response dirty state");
            check(decoded->getAckCount() == acks, "signed AckCount");
            check(decoded->getMessageSize() == MessageSizeType_Response_Data,
                  "response message size");
            for (unsigned i = 0; i < 64; ++i)
                check(decoded->getDataBlk().getByte(i) == pattern(63 - i),
                      "response data byte");
            checkTransport(*decoded);
        }
    }

    // Empty NetDest and default link/vnet are also value states to preserve.
    auto response = std::make_shared<ResponseMsg>(0, 64, rs);
    response->setaddr(0);
    response->setType(CoherenceResponseType_ACK);
    response->setSender(ids.front());
    response->setMessageSize(MessageSizeType_Response_Control);
    response->setMsgCounter(std::numeric_limits<uint64_t>::max());
    const auto decoded = decode(encode(response), rs);
    check(decoded->getDestination().isEmpty(), "empty destination");
    check(decoded->getIncomingLink() == -1 && decoded->getVnet() == -1,
          "unset network state");
    check(decoded->getMsgCounter() == std::numeric_limits<uint64_t>::max(),
          "maximum message counter");
}

} // namespace split
} // namespace ruby
} // namespace gem5
