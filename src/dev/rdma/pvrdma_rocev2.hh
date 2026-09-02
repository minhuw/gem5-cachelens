// SPDX-License-Identifier: BSD-3-Clause

#ifndef __DEV_RDMA_PVRDMA_ROCEV2_HH__
#define __DEV_RDMA_PVRDMA_ROCEV2_HH__

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gem5
{
namespace pvrdma
{
namespace rocev2
{

inline constexpr uint16_t EtherType = 0x86dd;
inline constexpr uint8_t Ipv6NextHeader = 17;
inline constexpr uint16_t RoceUdpDestinationPort = 4791;
inline constexpr uint16_t MinUdpSourcePort = 0xc000;
inline constexpr uint16_t MaxUdpSourcePort = 0xffff;
inline constexpr uint16_t DefaultPKey = 0xffff;
inline constexpr size_t EthernetHeaderSize = 14;
inline constexpr size_t Ipv6HeaderSize = 40;
inline constexpr size_t UdpHeaderSize = 8;
inline constexpr size_t BthSize = 12;
inline constexpr size_t AethSize = 4;
inline constexpr size_t IcrcSize = 4;
inline constexpr size_t EthernetDestinationOffset = 0;
inline constexpr size_t EthernetSourceOffset = 6;
inline constexpr size_t EtherTypeOffset = 12;
inline constexpr size_t Ipv6Offset = EthernetHeaderSize;
inline constexpr size_t Ipv6PayloadLengthOffset = Ipv6Offset + 4;
inline constexpr size_t Ipv6NextHeaderOffset = Ipv6Offset + 6;
inline constexpr size_t Ipv6HopLimitOffset = Ipv6Offset + 7;
inline constexpr size_t Ipv6SourceOffset = Ipv6Offset + 8;
inline constexpr size_t Ipv6DestinationOffset = Ipv6Offset + 24;
inline constexpr size_t UdpOffset = Ipv6Offset + Ipv6HeaderSize;
inline constexpr size_t UdpSourcePortOffset = UdpOffset;
inline constexpr size_t UdpDestinationPortOffset = UdpOffset + 2;
inline constexpr size_t UdpLengthOffset = UdpOffset + 4;
inline constexpr size_t UdpChecksumOffset = UdpOffset + 6;
inline constexpr size_t BthOffset = UdpOffset + UdpHeaderSize;
inline constexpr size_t BthFlagsOffset = BthOffset + 1;
inline constexpr size_t BthPKeyOffset = BthOffset + 2;
inline constexpr size_t BthQpnOffset = BthOffset + 4;
inline constexpr size_t BthAckOffset = BthOffset + 8;
inline constexpr size_t BthPsnOffset = BthOffset + 9;
inline constexpr size_t SendHeaderSize = BthOffset + BthSize;
inline constexpr size_t AethOffset = SendHeaderSize;
inline constexpr size_t ControlFrameSize =
    SendHeaderSize + AethSize + IcrcSize;
inline constexpr size_t MaxPayloadSize = 1024;
inline constexpr uint32_t FlowLabelMask = 0x000fffff;
inline constexpr uint32_t Field24Mask = 0x00ffffff;

struct Bytes
{
    const uint8_t *data = nullptr;
    size_t size = 0;
};

struct MutableBytes
{
    uint8_t *data = nullptr;
    size_t size = 0;
};

using MacAddress = std::array<uint8_t, 6>;
using Gid = std::array<uint8_t, 16>;

enum class Opcode : uint8_t
{
    SendFirst = 0x00,
    SendMiddle = 0x01,
    SendLast = 0x02,
    SendOnly = 0x04,
    Acknowledge = 0x11,
};

enum class Syndrome : uint8_t
{
    Ack = 0x00,
    Rnr = 0x20,
    SequenceNak = 0x60,
    InvalidRequestNak = 0x61,
    RemoteAccessNak = 0x62,
    RemoteOperationNak = 0x63,
};

enum class CodecError
{
    None,
    BufferTooSmall,
    Truncated,
    ExtraBytes,
    BadEtherType,
    BadIpv6Version,
    BadIpv6NextHeader,
    BadIpv6Length,
    BadHopLimit,
    BadUdpSourcePort,
    BadUdpDestinationPort,
    BadUdpLength,
    BadUdpChecksum,
    BadBthVersion,
    BadReserved,
    BadOpcode,
    BadAckRequest,
    BadPKey,
    BadQpn,
    BadPsn,
    BadMsn,
    BadPad,
    BadPayloadLength,
    PayloadTooLarge,
    BadSyndrome,
    BadIcrc,
};

struct Packet
{
    MacAddress destinationMac{};
    MacAddress sourceMac{};
    Gid sourceGid{};
    Gid destinationGid{};
    uint8_t trafficClass = 0;
    uint32_t flowLabel = 0;
    uint8_t hopLimit = 0xff;
    uint16_t sourcePort = MinUdpSourcePort;
    uint16_t pKey = DefaultPKey;
    Opcode opcode = Opcode::SendOnly;
    bool solicitedEvent = false;
    bool ackRequest = true;
    uint32_t destinationQpn = 0;
    uint32_t psn = 0;
    Syndrome syndrome = Syndrome::Ack;
    uint8_t ackCredit = 31;
    uint8_t rnrTimer = 0;
    uint32_t msn = 0;
    Bytes payload;
};

struct EncodeResult
{
    CodecError error = CodecError::None;
    size_t size = 0;

    explicit operator bool() const { return error == CodecError::None; }
};

struct DecodeResult
{
    CodecError error = CodecError::None;
    Packet packet;

    explicit operator bool() const { return error == CodecError::None; }
};

namespace detail
{

inline bool
isSend(Opcode opcode)
{
    return opcode == Opcode::SendFirst || opcode == Opcode::SendMiddle ||
           opcode == Opcode::SendLast || opcode == Opcode::SendOnly;
}

inline bool
supportedOpcode(Opcode opcode)
{
    return isSend(opcode) || opcode == Opcode::Acknowledge;
}

inline size_t
padSize(size_t payloadSize)
{
    return (-payloadSize) & 3;
}

inline void
put16(uint8_t *bytes, size_t offset, uint16_t value)
{
    bytes[offset] = value >> 8;
    bytes[offset + 1] = value;
}

inline void
put24(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = value >> 16;
    bytes[offset + 1] = value >> 8;
    bytes[offset + 2] = value;
}

inline void
put32Le(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = value;
    bytes[offset + 1] = value >> 8;
    bytes[offset + 2] = value >> 16;
    bytes[offset + 3] = value >> 24;
}

inline uint16_t
get16(const uint8_t *bytes, size_t offset)
{
    return (uint16_t{bytes[offset]} << 8) | bytes[offset + 1];
}

inline uint32_t
get24(const uint8_t *bytes, size_t offset)
{
    return (uint32_t{bytes[offset]} << 16) |
           (uint32_t{bytes[offset + 1]} << 8) | bytes[offset + 2];
}

inline uint32_t
get32Le(const uint8_t *bytes, size_t offset)
{
    return uint32_t{bytes[offset]} |
           (uint32_t{bytes[offset + 1]} << 8) |
           (uint32_t{bytes[offset + 2]} << 16) |
           (uint32_t{bytes[offset + 3]} << 24);
}

inline CodecError
validatePayload(Opcode opcode, Bytes payload)
{
    if (payload.size && !payload.data)
        return CodecError::BadPayloadLength;
    if (payload.size > MaxPayloadSize)
        return CodecError::PayloadTooLarge;
    switch (opcode) {
      case Opcode::SendFirst:
      case Opcode::SendMiddle:
        return payload.size == MaxPayloadSize ? CodecError::None :
                                               CodecError::BadPayloadLength;
      case Opcode::SendLast:
        return payload.size != 0 ? CodecError::None :
                                   CodecError::BadPayloadLength;
      case Opcode::SendOnly:
        return CodecError::None;
      case Opcode::Acknowledge:
        return payload.size == 0 ? CodecError::None :
                                   CodecError::BadPayloadLength;
      default:
        return CodecError::BadOpcode;
    }
}

inline CodecError
validateSyndrome(Syndrome syndrome, uint8_t credit, uint8_t timer)
{
    switch (syndrome) {
      case Syndrome::Ack:
        return credit <= 31 && timer == 0 ? CodecError::None :
                                            CodecError::BadSyndrome;
      case Syndrome::SequenceNak:
      case Syndrome::InvalidRequestNak:
      case Syndrome::RemoteAccessNak:
      case Syndrome::RemoteOperationNak:
        return timer == 0 ? CodecError::None : CodecError::BadSyndrome;
      case Syndrome::Rnr:
        return timer <= 31 ? CodecError::None : CodecError::BadSyndrome;
      default:
        return CodecError::BadSyndrome;
    }
}

inline CodecError
validate(const Packet &packet)
{
    if (!supportedOpcode(packet.opcode))
        return CodecError::BadOpcode;
    if (packet.flowLabel & ~FlowLabelMask)
        return CodecError::BadIpv6Version;
    if (packet.sourcePort < MinUdpSourcePort)
        return CodecError::BadUdpSourcePort;
    if (packet.pKey != DefaultPKey)
        return CodecError::BadPKey;
    if (packet.destinationQpn == 0 ||
        packet.destinationQpn == Field24Mask ||
        (packet.destinationQpn & ~Field24Mask)) {
        return CodecError::BadQpn;
    }
    if (packet.psn & ~Field24Mask)
        return CodecError::BadPsn;
    if (packet.solicitedEvent && packet.opcode != Opcode::SendLast &&
        packet.opcode != Opcode::SendOnly) {
        return CodecError::BadReserved;
    }
    if (!isSend(packet.opcode) && packet.ackRequest)
        return CodecError::BadAckRequest;
    const CodecError payloadError = validatePayload(packet.opcode,
                                                    packet.payload);
    if (payloadError != CodecError::None)
        return payloadError;
    if (packet.opcode == Opcode::Acknowledge) {
        if (packet.msn & ~Field24Mask)
            return CodecError::BadMsn;
        return validateSyndrome(packet.syndrome, packet.ackCredit,
                                packet.rnrTimer);
    }
    return CodecError::None;
}

inline uint32_t
icrc(const uint8_t *roce, size_t size)
{
    uint32_t crc = 0xdebb20e3;
    static constexpr uint8_t maskedFirst[] = {0x6f, 0xff, 0xff, 0xff};
    for (size_t i = 0; i < size; ++i) {
        const uint8_t byte = i < 4 ? maskedFirst[i] :
            (i == 7 || i == 46 || i == 47 || i == 52 ? 0xff : roce[i]);
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    }
    return ~crc;
}

inline bool
validUdpChecksum(const uint8_t *bytes, uint32_t udpLength)
{
    uint32_t sum = (udpLength >> 16) + (udpLength & 0xffff) + Ipv6NextHeader;
    const auto add = [&sum](const uint8_t *data, size_t size) {
        while (size >= 2) {
            sum += (uint16_t{data[0]} << 8) | data[1];
            data += 2;
            size -= 2;
        }
        if (size)
            sum += uint16_t{data[0]} << 8;
    };
    add(bytes + Ipv6SourceOffset, 2 * Gid{}.size());
    add(bytes + UdpOffset, udpLength);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return sum == 0xffff;
}

} // namespace detail

inline uint16_t
udpSourcePort(uint32_t flowLabel, uint32_t localQpn, uint32_t remoteQpn)
{
    if (flowLabel == 0) {
        uint64_t folded = uint64_t{localQpn} * remoteQpn;
        folded ^= folded >> 20;
        folded ^= folded >> 40;
        flowLabel = folded & FlowLabelMask;
    }
    return MinUdpSourcePort |
        ((flowLabel & 0x03fff) ^ ((flowLabel & 0xfc000) >> 14));
}

inline size_t
encodedSize(const Packet &packet)
{
    return detail::isSend(packet.opcode) ?
        SendHeaderSize + packet.payload.size +
            detail::padSize(packet.payload.size) + IcrcSize :
        ControlFrameSize;
}

inline EncodeResult
encode(const Packet &packet, MutableBytes output)
{
    const CodecError error = detail::validate(packet);
    if (error != CodecError::None)
        return {error, 0};

    const size_t size = encodedSize(packet);
    if (!output.data || output.size < size)
        return {CodecError::BufferTooSmall, 0};

    uint8_t *const bytes = output.data;
    std::copy(packet.destinationMac.begin(), packet.destinationMac.end(),
              bytes + EthernetDestinationOffset);
    std::copy(packet.sourceMac.begin(), packet.sourceMac.end(),
              bytes + EthernetSourceOffset);
    detail::put16(bytes, EtherTypeOffset, EtherType);

    const uint32_t ipv6First = (uint32_t{6} << 28) |
                               (uint32_t{packet.trafficClass} << 20) |
                               packet.flowLabel;
    bytes[Ipv6Offset] = ipv6First >> 24;
    bytes[Ipv6Offset + 1] = ipv6First >> 16;
    bytes[Ipv6Offset + 2] = ipv6First >> 8;
    bytes[Ipv6Offset + 3] = ipv6First;
    const uint16_t udpLength = size - EthernetHeaderSize - Ipv6HeaderSize;
    detail::put16(bytes, Ipv6PayloadLengthOffset, udpLength);
    bytes[Ipv6NextHeaderOffset] = Ipv6NextHeader;
    bytes[Ipv6HopLimitOffset] = packet.hopLimit;
    std::copy(packet.sourceGid.begin(), packet.sourceGid.end(),
              bytes + Ipv6SourceOffset);
    std::copy(packet.destinationGid.begin(), packet.destinationGid.end(),
              bytes + Ipv6DestinationOffset);

    detail::put16(bytes, UdpSourcePortOffset, packet.sourcePort);
    detail::put16(bytes, UdpDestinationPortOffset, RoceUdpDestinationPort);
    detail::put16(bytes, UdpLengthOffset, udpLength);
    detail::put16(bytes, UdpChecksumOffset, 0);

    const size_t pad = detail::isSend(packet.opcode) ?
        detail::padSize(packet.payload.size) : 0;
    bytes[BthOffset] = static_cast<uint8_t>(packet.opcode);
    bytes[BthFlagsOffset] =
        (packet.solicitedEvent ? 0x80 : 0) | (pad << 4);
    detail::put16(bytes, BthPKeyOffset, packet.pKey);
    bytes[BthQpnOffset] = 0;
    detail::put24(bytes, BthQpnOffset + 1, packet.destinationQpn);
    bytes[BthAckOffset] =
        detail::isSend(packet.opcode) && packet.ackRequest ? 0x80 : 0;
    detail::put24(bytes, BthPsnOffset, packet.psn);

    size_t bodyEnd = SendHeaderSize;
    if (detail::isSend(packet.opcode)) {
        if (packet.payload.size) {
            std::memcpy(bytes + SendHeaderSize, packet.payload.data,
                        packet.payload.size);
        }
        std::fill_n(bytes + SendHeaderSize + packet.payload.size, pad, 0);
        bodyEnd += packet.payload.size + pad;
    } else {
        bytes[AethOffset] = packet.syndrome == Syndrome::Ack ?
            packet.ackCredit : static_cast<uint8_t>(packet.syndrome) |
                (packet.syndrome == Syndrome::Rnr ? packet.rnrTimer : 0);
        detail::put24(bytes, AethOffset + 1, packet.msn);
        bodyEnd += AethSize;
    }

    detail::put32Le(bytes, bodyEnd,
                    detail::icrc(bytes + EthernetHeaderSize,
                                 bodyEnd - EthernetHeaderSize));
    return {CodecError::None, size};
}

inline DecodeResult
decode(Bytes input, size_t logicalLength)
{
    if (!input.data || logicalLength > input.size ||
        logicalLength < SendHeaderSize + IcrcSize) {
        return {CodecError::Truncated, {}};
    }
    const uint8_t *const bytes = input.data;
    if (detail::get16(bytes, EtherTypeOffset) != EtherType)
        return {CodecError::BadEtherType, {}};
    if ((bytes[Ipv6Offset] >> 4) != 6)
        return {CodecError::BadIpv6Version, {}};
    if (bytes[Ipv6NextHeaderOffset] != Ipv6NextHeader)
        return {CodecError::BadIpv6NextHeader, {}};

    const size_t ipv6PayloadSize =
        detail::get16(bytes, Ipv6PayloadLengthOffset);
    if (ipv6PayloadSize < UdpHeaderSize + BthSize + IcrcSize)
        return {CodecError::BadIpv6Length, {}};
    const size_t expectedSize =
        EthernetHeaderSize + Ipv6HeaderSize + ipv6PayloadSize;
    if (logicalLength < expectedSize)
        return {CodecError::Truncated, {}};
    if (logicalLength > expectedSize)
        return {CodecError::ExtraBytes, {}};

    const uint16_t sourcePort = detail::get16(bytes, UdpSourcePortOffset);
    if (sourcePort < MinUdpSourcePort)
        return {CodecError::BadUdpSourcePort, {}};
    if (detail::get16(bytes, UdpDestinationPortOffset) !=
        RoceUdpDestinationPort) {
        return {CodecError::BadUdpDestinationPort, {}};
    }
    if (detail::get16(bytes, UdpLengthOffset) != ipv6PayloadSize)
        return {CodecError::BadUdpLength, {}};
    if (detail::get16(bytes, UdpChecksumOffset) != 0 &&
        !detail::validUdpChecksum(bytes, ipv6PayloadSize)) {
        return {CodecError::BadUdpChecksum, {}};
    }

    const Opcode opcode = static_cast<Opcode>(bytes[BthOffset]);
    if (!detail::supportedOpcode(opcode))
        return {CodecError::BadOpcode, {}};
    if (bytes[BthFlagsOffset] & 0x0f)
        return {CodecError::BadBthVersion, {}};
    if ((bytes[BthFlagsOffset] & 0x40) ||
        (bytes[BthQpnOffset] & 0x3f) || (bytes[BthAckOffset] & 0x7f)) {
        return {CodecError::BadReserved, {}};
    }
    if (detail::get16(bytes, BthPKeyOffset) != DefaultPKey)
        return {CodecError::BadPKey, {}};

    const bool send = detail::isSend(opcode);
    const bool solicitedEvent = bytes[BthFlagsOffset] & 0x80;
    const bool ackRequest = bytes[BthAckOffset] & 0x80;
    if (solicitedEvent && opcode != Opcode::SendLast &&
        opcode != Opcode::SendOnly) {
        return {CodecError::BadReserved, {}};
    }
    if (!send && ackRequest)
        return {CodecError::BadAckRequest, {}};
    const size_t pad = (bytes[BthFlagsOffset] >> 4) & 3;
    const size_t bodySize = ipv6PayloadSize - UdpHeaderSize - BthSize -
                            IcrcSize;
    if ((!send && pad != 0) || bodySize < pad || (send && bodySize % 4))
        return {CodecError::BadPad, {}};

    Packet packet;
    std::copy_n(bytes + EthernetDestinationOffset, 6,
                packet.destinationMac.begin());
    std::copy_n(bytes + EthernetSourceOffset, 6, packet.sourceMac.begin());
    std::copy_n(bytes + Ipv6SourceOffset, 16, packet.sourceGid.begin());
    std::copy_n(bytes + Ipv6DestinationOffset, 16,
                packet.destinationGid.begin());
    packet.trafficClass = ((bytes[Ipv6Offset] & 0x0f) << 4) |
                          (bytes[Ipv6Offset + 1] >> 4);
    packet.flowLabel = (uint32_t{bytes[Ipv6Offset + 1]} & 0x0f) << 16 |
                       (uint32_t{bytes[Ipv6Offset + 2]} << 8) |
                       bytes[Ipv6Offset + 3];
    packet.hopLimit = bytes[Ipv6HopLimitOffset];
    packet.sourcePort = sourcePort;
    packet.pKey = detail::get16(bytes, BthPKeyOffset);
    packet.opcode = opcode;
    packet.solicitedEvent = solicitedEvent;
    packet.ackRequest = ackRequest;
    packet.destinationQpn = detail::get24(bytes, BthQpnOffset + 1);
    packet.psn = detail::get24(bytes, BthPsnOffset);
    if (packet.destinationQpn == 0 || packet.destinationQpn == Field24Mask)
        return {CodecError::BadQpn, {}};

    const size_t icrcOffset = logicalLength - IcrcSize;
    if (send) {
        packet.payload = {bytes + SendHeaderSize, bodySize - pad};
        const CodecError payloadError = detail::validatePayload(opcode,
                                                               packet.payload);
        if (payloadError != CodecError::None)
            return {payloadError, {}};
        for (size_t i = icrcOffset - pad; i < icrcOffset; ++i) {
            if (bytes[i] != 0)
                return {CodecError::BadPad, {}};
        }
    } else {
        if (bodySize != AethSize)
            return {CodecError::BadPayloadLength, {}};
        const uint8_t wireSyndrome = bytes[AethOffset];
        if ((wireSyndrome & 0xe0) == 0) {
            packet.syndrome = Syndrome::Ack;
            packet.ackCredit = wireSyndrome;
        } else if ((wireSyndrome & 0xe0) == 0x20) {
            packet.syndrome = Syndrome::Rnr;
            packet.rnrTimer = wireSyndrome & 0x1f;
        } else {
            packet.syndrome = static_cast<Syndrome>(wireSyndrome);
        }
        packet.msn = detail::get24(bytes, AethOffset + 1);
        const CodecError syndromeError = detail::validateSyndrome(
            packet.syndrome, packet.ackCredit, packet.rnrTimer);
        if (syndromeError != CodecError::None)
            return {syndromeError, {}};
    }

    if (detail::get32Le(bytes, icrcOffset) !=
        detail::icrc(bytes + EthernetHeaderSize,
                     icrcOffset - EthernetHeaderSize)) {
        return {CodecError::BadIcrc, {}};
    }
    return {CodecError::None, packet};
}

} // namespace rocev2
} // namespace pvrdma
} // namespace gem5

#endif // __DEV_RDMA_PVRDMA_ROCEV2_HH__
