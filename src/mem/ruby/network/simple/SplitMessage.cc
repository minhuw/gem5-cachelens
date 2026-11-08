// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#include "mem/ruby/network/simple/SplitMessage.hh"

#include <limits>
#include <utility>

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

constexpr uint32_t Magic = 0x4d455333; // MES3
constexpr uint16_t Version = 1;
constexpr uint32_t BlockBytes = 64;
constexpr uint32_t MaxMachines = 65536;
constexpr size_t HeaderBytes = 16;
constexpr size_t MaxWireBytes = 256 + 8 * MaxMachines;
enum class Kind : uint8_t { Request = 1, Response = 2 };

class Writer
{
  public:
    std::vector<uint8_t> bytes;

    void
    put(uint64_t value, unsigned width)
    {
        fatal_if(bytes.size() + width > MaxWireBytes,
                 "SplitMessage: frame exceeds wire bound");
        for (unsigned i = width; i > 0; --i)
            bytes.push_back((value >> (8 * (i - 1))) & 0xff);
    }

    void
    boolean(bool value)
    {
        put(value ? 1 : 0, 1);
    }

    void
    signed32(int value)
    {
        // Conversion to unsigned is defined modulo 2^32, including negatives.
        static_assert(std::numeric_limits<int>::digits == 31);
        put(static_cast<uint32_t>(value), 4);
    }

    void
    finish()
    {
        const uint32_t size = bytes.size();
        for (unsigned i = 0; i < 4; ++i)
            bytes[12 + i] = (size >> (8 * (3 - i))) & 0xff;
    }
};

class Reader
{
  private:
    const std::vector<uint8_t> &bytes;
    size_t position = 0;

  public:
    explicit Reader(const std::vector<uint8_t> &input) : bytes(input) {}

    size_t remaining() const { return bytes.size() - position; }

    uint64_t
    get(unsigned width)
    {
        fatal_if(width > remaining(), "SplitMessage: truncated frame");
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value = (value << 8) | bytes[position++];
        return value;
    }

    bool
    boolean()
    {
        const auto value = get(1);
        fatal_if(value > 1, "SplitMessage: invalid boolean");
        return value != 0;
    }

    int
    signed32()
    {
        const uint32_t value = get(4);
        // Avoid implementation-defined unsigned-to-signed conversion.
        if (value <= std::numeric_limits<int32_t>::max())
            return value;
        return -1 - static_cast<int>(UINT32_MAX - value);
    }
};

template<class E>
E
checkedEnum(uint64_t value, E first, E end)
{
    fatal_if(value < static_cast<uint64_t>(first) ||
             value >= static_cast<uint64_t>(end),
             "SplitMessage: enum value out of range (%llu)",
             static_cast<unsigned long long>(value));
    return static_cast<E>(value);
}

template<class E>
void
writeEnum(Writer &out, E value, E first, E end)
{
    out.put(static_cast<uint32_t>(checkedEnum(
        static_cast<uint64_t>(value), first, end)), 4);
}

uint32_t
checkedLength(uint64_t value)
{
    fatal_if(value > BlockBytes, "SplitMessage: length exceeds 64 bytes");
    return value;
}

void
checkMachine(const MachineID &id, RubySystem *rs = nullptr)
{
    checkedEnum(static_cast<uint64_t>(id.type), MachineType_FIRST,
                MachineType_NUM);
    fatal_if(id.num >= MaxMachines,
             "SplitMessage: machine number exceeds wire bound");
    if (rs) {
        const int count = rs->MachineType_base_count(id.type);
        fatal_if(count < 0 || id.num >= static_cast<uint32_t>(count),
                 "SplitMessage: machine ID is outside local topology");
    }
}

void
writeMachine(Writer &out, const MachineID &id)
{
    checkMachine(id);
    out.put(static_cast<uint32_t>(id.type), 4);
    out.put(id.num, 4);
}

MachineID
readMachine(Reader &in, RubySystem *rs)
{
    const auto type = checkedEnum(in.get(4), MachineType_FIRST, MachineType_NUM);
    const uint32_t number = in.get(4);
    const MachineID id(type, number);
    checkMachine(id, rs);
    return id;
}

uint32_t
checkTopology(RubySystem *rs)
{
    fatal_if(!rs, "SplitMessage: decode requires a RubySystem");
    fatal_if(rs->getBlockSizeBytes() != BlockBytes,
             "SplitMessage: only 64-byte Ruby cache blocks are supported");
    fatal_if(rs->getProtocolInfo().getName() != "MESI_Three_Level",
             "SplitMessage: RubySystem protocol must be MESI_Three_Level");
    uint64_t total = 0;
    for (int type = MachineType_FIRST; type < MachineType_NUM; ++type) {
        const int count = rs->MachineType_base_count(
            static_cast<MachineType>(type));
        fatal_if(count < 0, "SplitMessage: negative topology count");
        total += count;
        fatal_if(total > MaxMachines,
                 "SplitMessage: topology exceeds 65,536 machines");
    }
    return total;
}

void
writeDestination(Writer &out, const NetDest &destination)
{
    fatal_if(destination.getSize() != MachineType_NUM,
             "SplitMessage: uninitialized or incompatible NetDest");
    const int count = destination.count();
    fatal_if(count < 0 || count > MaxMachines,
             "SplitMessage: destination count exceeds wire bound");
    out.put(count, 4);
    // Work on a value copy: getAllDest() loses machine types and is nonconst.
    NetDest pending = destination;
    for (int i = 0; i < count; ++i) {
        const MachineID id = pending.smallestElement();
        writeMachine(out, id);
        pending.remove(id);
    }
}

NetDest
readDestination(Reader &in, RubySystem *rs, uint32_t machines)
{
    const uint32_t count = in.get(4);
    fatal_if(count > machines || count > MaxMachines ||
             count > in.remaining() / 8,
             "SplitMessage: invalid destination count");
    NetDest destination(rs);
    MachineID previous;
    for (uint32_t i = 0; i < count; ++i) {
        const MachineID id = readMachine(in, rs);
        fatal_if(i && (id.type < previous.type ||
                 (id.type == previous.type && id.num <= previous.num)),
                 "SplitMessage: duplicate or unordered destinations");
        destination.add(id);
        previous = id;
    }
    return destination;
}

void
writeData(Writer &out, const DataBlock &data)
{
    fatal_if(data.getBlockSize() != BlockBytes || !data.isAlloc(),
             "SplitMessage: DataBlock must own 64 bytes");
    fatal_if(data.numAtomicLogEntries() != 0,
             "SplitMessage: atomic DataBlock logs cannot cross processes");
    for (unsigned i = 0; i < BlockBytes; ++i)
        out.put(data.getByte(i), 1);
}

void
readData(Reader &in, DataBlock &data)
{
    for (unsigned i = 0; i < BlockBytes; ++i)
        data.setByte(i, in.get(1));
}

void
writeMask(Writer &out, const WriteMask &mask)
{
    fatal_if(mask.getBlockSize() != BlockBytes,
             "SplitMessage: accessMask must have 64 bits");
    fatal_if(!mask.getAtomicOps().empty(),
             "SplitMessage: atomic mask functors cannot cross processes");
    for (unsigned byte = 0; byte < BlockBytes / 8; ++byte) {
        uint8_t packed = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (mask.test(byte * 8 + bit))
                packed |= 1u << bit;
        }
        out.put(packed, 1);
    }
}

void
readMask(Reader &in, WriteMask &mask)
{
    for (unsigned byte = 0; byte < BlockBytes / 8; ++byte) {
        const uint8_t packed = in.get(1);
        for (unsigned bit = 0; bit < 8; ++bit)
            mask.setMask(byte * 8 + bit, 1, (packed >> bit) & 1);
    }
}

void
checkAckCount(int count, uint32_t machines)
{
    // MESI uses negative counts for the number of outstanding invalidations.
    fatal_if(static_cast<int64_t>(count) < -static_cast<int64_t>(machines) ||
             static_cast<int64_t>(count) > machines,
             "SplitMessage: AckCount exceeds machine count bound");
}

void
checkNetworkState(int link, int vnet)
{
    fatal_if(link < -1 || vnet < -1,
             "SplitMessage: invalid incoming link or virtual network");
}

struct TransportState
{
    Tick created;
    Tick enqueued;
    Tick delayed;
    uint64_t counter;
    int link;
    int vnet;

    void
    restore(Message &message) const
    {
        message.restoreTransportState(created, enqueued, delayed, counter,
                                      link, vnet);
    }
};

void
writeTransport(Writer &out, const Message &message)
{
    checkNetworkState(message.getIncomingLink(), message.getVnet());
    out.put(message.getTime(), 8);
    out.put(message.getLastEnqueueTime(), 8);
    out.put(message.getDelayedTicks(), 8);
    out.put(message.getMsgCounter(), 8);
    out.signed32(message.getIncomingLink());
    out.signed32(message.getVnet());
}

TransportState
readTransport(Reader &in)
{
    TransportState state;
    state.created = in.get(8);
    state.enqueued = in.get(8);
    state.delayed = in.get(8);
    state.counter = in.get(8);
    state.link = in.signed32();
    state.vnet = in.signed32();
    checkNetworkState(state.link, state.vnet);
    return state;
}

void
writeRequest(Writer &out, const RequestMsg &message)
{
    fatal_if(message.getseqReq() != nullptr || message.getisSeqReqValid(),
             "SplitMessage: seqReq/isSeqReqValid cannot cross processes "
             "(including DMA requests)");
    out.put(message.getaddr(), 8);
    writeEnum(out, message.getType(), CoherenceRequestType_FIRST,
              CoherenceRequestType_NUM);
    writeEnum(out, message.getAccessMode(), RubyAccessMode_FIRST,
              RubyAccessMode_NUM);
    writeMachine(out, message.getRequestor());
    writeDestination(out, message.getDestination());
    writeEnum(out, message.getMessageSize(), MessageSizeType_FIRST,
              MessageSizeType_NUM);
    writeData(out, message.getDataBlk());
    out.put(checkedLength(message.getLen()), 4);
    out.put(message.getaccessAddr(), 8);
    out.put(checkedLength(message.getaccessSize()), 4);
    writeMask(out, message.getaccessMask());
    out.boolean(false); // seqReq presence; no pointer payload is permitted.
    out.boolean(message.getddioEligible());
    out.boolean(message.getisSeqReqValid());
    out.boolean(message.getDirty());
    writeEnum(out, message.getPrefetch(), PrefetchBit_FIRST, PrefetchBit_NUM);
}

MsgPtr
readRequest(Reader &in, RubySystem *rs, uint32_t machines,
            const TransportState &state)
{
    auto message = std::make_shared<RequestMsg>(state.created, BlockBytes, rs);
    message->setaddr(in.get(8));
    message->setType(checkedEnum(in.get(4), CoherenceRequestType_FIRST,
                                 CoherenceRequestType_NUM));
    message->setAccessMode(checkedEnum(in.get(4), RubyAccessMode_FIRST,
                                       RubyAccessMode_NUM));
    message->setRequestor(readMachine(in, rs));
    message->setDestination(readDestination(in, rs, machines));
    message->setMessageSize(checkedEnum(in.get(4), MessageSizeType_FIRST,
                                        MessageSizeType_NUM));
    readData(in, message->getDataBlk());
    message->setLen(checkedLength(in.get(4)));
    message->setaccessAddr(in.get(8));
    message->setaccessSize(checkedLength(in.get(4)));
    readMask(in, message->getaccessMask());
    const bool seq_req_present = in.boolean();
    message->setddioEligible(in.boolean());
    const bool seq_req_valid = in.boolean();
    fatal_if(seq_req_present || seq_req_valid,
             "SplitMessage: seqReq/isSeqReqValid cannot cross processes");
    message->setseqReq(nullptr);
    message->setisSeqReqValid(false);
    message->setDirty(in.boolean());
    message->setPrefetch(checkedEnum(in.get(4), PrefetchBit_FIRST,
                                     PrefetchBit_NUM));
    state.restore(*message);
    return message;
}

void
writeResponse(Writer &out, const ResponseMsg &message)
{
    out.put(message.getaddr(), 8);
    writeEnum(out, message.getType(), CoherenceResponseType_FIRST,
              CoherenceResponseType_NUM);
    writeMachine(out, message.getSender());
    writeDestination(out, message.getDestination());
    writeData(out, message.getDataBlk());
    out.boolean(message.getDirty());
    checkAckCount(message.getAckCount(), MaxMachines);
    out.signed32(message.getAckCount());
    writeEnum(out, message.getMessageSize(), MessageSizeType_FIRST,
              MessageSizeType_NUM);
}

MsgPtr
readResponse(Reader &in, RubySystem *rs, uint32_t machines,
             const TransportState &state)
{
    auto message = std::make_shared<ResponseMsg>(state.created, BlockBytes, rs);
    message->setaddr(in.get(8));
    message->setType(checkedEnum(in.get(4), CoherenceResponseType_FIRST,
                                 CoherenceResponseType_NUM));
    message->setSender(readMachine(in, rs));
    message->setDestination(readDestination(in, rs, machines));
    readData(in, message->getDataBlk());
    message->setDirty(in.boolean());
    const int acks = in.signed32();
    checkAckCount(acks, machines);
    message->setAckCount(acks);
    message->setMessageSize(checkedEnum(in.get(4), MessageSizeType_FIRST,
                                        MessageSizeType_NUM));
    state.restore(*message);
    return message;
}

} // anonymous namespace

std::vector<uint8_t>
encode(const MsgPtr &message)
{
    fatal_if(!message, "SplitMessage: cannot encode a null message");
    const auto *request = dynamic_cast<const RequestMsg *>(message.get());
    const auto *response = dynamic_cast<const ResponseMsg *>(message.get());
    fatal_if(!request && !response,
             "SplitMessage: only MESI_Three_Level RequestMsg/ResponseMsg "
             "can cross processes");
    Writer out;
    out.bytes.reserve(256);
    out.put(Magic, 4);
    out.put(Version, 2);
    out.put(static_cast<uint8_t>(request ? Kind::Request : Kind::Response), 1);
    out.put(0, 1);
    out.put(BlockBytes, 4);
    out.put(0, 4); // Filled with the total size after writing the body.
    writeTransport(out, *message);
    if (request)
        writeRequest(out, *request);
    else
        writeResponse(out, *response);
    out.finish();
    return std::move(out.bytes);
}

MsgPtr
decode(const std::vector<uint8_t> &bytes, RubySystem *ruby_system)
{
    fatal_if(bytes.size() < HeaderBytes || bytes.size() > MaxWireBytes,
             "SplitMessage: frame length outside wire bounds");
    Reader in(bytes);
    const auto magic = in.get(4);
    const auto version = in.get(2);
    const auto kind = in.get(1);
    const auto reserved = in.get(1);
    const auto block_bytes = in.get(4);
    const auto frame_bytes = in.get(4);
    fatal_if(magic != Magic, "SplitMessage: wrong protocol magic");
    fatal_if(version != Version, "SplitMessage: unsupported wire version");
    fatal_if(kind != static_cast<uint8_t>(Kind::Request) &&
             kind != static_cast<uint8_t>(Kind::Response),
             "SplitMessage: unsupported message kind");
    fatal_if(reserved != 0, "SplitMessage: nonzero reserved header byte");
    fatal_if(block_bytes != BlockBytes,
             "SplitMessage: unsupported wire block size");
    fatal_if(frame_bytes != bytes.size(), "SplitMessage: frame length mismatch");
    const auto machines = checkTopology(ruby_system);
    const auto state = readTransport(in);
    MsgPtr message = kind == static_cast<uint8_t>(Kind::Request) ?
        readRequest(in, ruby_system, machines, state) :
        readResponse(in, ruby_system, machines, state);
    fatal_if(in.remaining() != 0, "SplitMessage: trailing frame bytes");
    return message;
}

bool
supportsProtocol(RubySystem *ruby_system)
{
    return ruby_system->getProtocolInfo().getName() == "MESI_Three_Level";
}

LocalRequest
detachLocalRequest(Message &message)
{
    LocalRequest local;
    if (auto *request = dynamic_cast<RequestMsg *>(&message)) {
        local.request = request->getseqReq();
        local.valid = request->getisSeqReqValid();
        fatal_if(bool(local.request) != local.valid,
                 "Inconsistent local DMA request provenance");
        request->setseqReq(nullptr);
        request->setisSeqReqValid(false);
    }
    return local;
}

void
attachLocalRequest(Message &message, const LocalRequest &local)
{
    if (auto *request = dynamic_cast<RequestMsg *>(&message)) {
        request->setseqReq(local.request);
        request->setisSeqReqValid(local.valid);
    } else {
        fatal_if(local.request || local.valid,
                 "Local DMA provenance attached to non-request message");
    }
}

} // namespace split
} // namespace ruby
} // namespace gem5
