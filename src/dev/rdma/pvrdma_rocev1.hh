// SPDX-License-Identifier: BSD-3-Clause

#ifndef __DEV_RDMA_PVRDMA_ROCEV1_HH__
#define __DEV_RDMA_PVRDMA_ROCEV1_HH__

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gem5
{
namespace pvrdma
{
namespace rocev1
{

inline constexpr uint16_t EtherType = 0x8915;
inline constexpr uint8_t GrhNextHeader = 0x1b;
inline constexpr uint16_t DefaultPKey = 0xffff;
inline constexpr size_t EthernetHeaderSize = 14;
inline constexpr size_t GrhSize = 40;
inline constexpr size_t BthSize = 12;
inline constexpr size_t AethSize = 4;
inline constexpr size_t IcrcSize = 4;
inline constexpr size_t SendHeaderSize =
    EthernetHeaderSize + GrhSize + BthSize;
inline constexpr size_t ControlFrameSize =
    SendHeaderSize + AethSize + IcrcSize;
inline constexpr size_t MaxPayloadSize = 1024;
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
    BadGrhVersion,
    BadGrhNextHeader,
    BadGrhLength,
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
    if (packet.flowLabel & ~uint32_t{0x000fffff})
        return CodecError::BadGrhVersion;
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
            (i == 7 || i == 44 ? 0xff : roce[i]);
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    }
    return ~crc;
}

} // namespace detail

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
              bytes);
    std::copy(packet.sourceMac.begin(), packet.sourceMac.end(), bytes + 6);
    detail::put16(bytes, 12, EtherType);

    const uint32_t grhFirst = (uint32_t{6} << 28) |
                              (uint32_t{packet.trafficClass} << 20) |
                              packet.flowLabel;
    bytes[14] = grhFirst >> 24;
    bytes[15] = grhFirst >> 16;
    bytes[16] = grhFirst >> 8;
    bytes[17] = grhFirst;
    detail::put16(bytes, 18, size - EthernetHeaderSize - GrhSize);
    bytes[20] = GrhNextHeader;
    bytes[21] = packet.hopLimit;
    std::copy(packet.sourceGid.begin(), packet.sourceGid.end(), bytes + 22);
    std::copy(packet.destinationGid.begin(), packet.destinationGid.end(),
              bytes + 38);

    const size_t pad = detail::isSend(packet.opcode) ?
        detail::padSize(packet.payload.size) : 0;
    bytes[54] = static_cast<uint8_t>(packet.opcode);
    bytes[55] = (packet.solicitedEvent ? 0x80 : 0) | (pad << 4);
    detail::put16(bytes, 56, packet.pKey);
    bytes[58] = 0;
    detail::put24(bytes, 59, packet.destinationQpn);
    bytes[62] = detail::isSend(packet.opcode) && packet.ackRequest ? 0x80 : 0;
    detail::put24(bytes, 63, packet.psn);

    size_t bodyEnd = SendHeaderSize;
    if (detail::isSend(packet.opcode)) {
        if (packet.payload.size) {
            std::memcpy(bytes + SendHeaderSize, packet.payload.data,
                        packet.payload.size);
        }
        std::fill_n(bytes + SendHeaderSize + packet.payload.size, pad, 0);
        bodyEnd += packet.payload.size + pad;
    } else {
        bytes[66] = packet.syndrome == Syndrome::Ack ? packet.ackCredit :
            static_cast<uint8_t>(packet.syndrome) |
                (packet.syndrome == Syndrome::Rnr ? packet.rnrTimer : 0);
        detail::put24(bytes, 67, packet.msn);
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
    if (detail::get16(bytes, 12) != EtherType)
        return {CodecError::BadEtherType, {}};
    if ((bytes[14] >> 4) != 6)
        return {CodecError::BadGrhVersion, {}};
    if (bytes[20] != GrhNextHeader)
        return {CodecError::BadGrhNextHeader, {}};

    const size_t grhPayloadSize = detail::get16(bytes, 18);
    if (grhPayloadSize < BthSize + IcrcSize)
        return {CodecError::BadGrhLength, {}};
    const size_t expectedSize = EthernetHeaderSize + GrhSize + grhPayloadSize;
    if (logicalLength < expectedSize)
        return {CodecError::Truncated, {}};
    if (logicalLength > expectedSize)
        return {CodecError::ExtraBytes, {}};

    const Opcode opcode = static_cast<Opcode>(bytes[54]);
    if (!detail::supportedOpcode(opcode))
        return {CodecError::BadOpcode, {}};
    if (bytes[55] & 0x0f)
        return {CodecError::BadBthVersion, {}};
    if ((bytes[55] & 0x40) || (bytes[58] & 0x3f) ||
        (bytes[62] & 0x7f)) {
        return {CodecError::BadReserved, {}};
    }
    if (detail::get16(bytes, 56) != DefaultPKey)
        return {CodecError::BadPKey, {}};

    const bool send = detail::isSend(opcode);
    const bool solicitedEvent = bytes[55] & 0x80;
    const bool ackRequest = bytes[62] & 0x80;
    if (solicitedEvent && opcode != Opcode::SendLast &&
        opcode != Opcode::SendOnly) {
        return {CodecError::BadReserved, {}};
    }
    if (!send && ackRequest)
        return {CodecError::BadAckRequest, {}};
    const size_t pad = (bytes[55] >> 4) & 3;
    const size_t bodySize = grhPayloadSize - BthSize - IcrcSize;
    if ((!send && pad != 0) || bodySize < pad || (send && bodySize % 4))
        return {CodecError::BadPad, {}};

    Packet packet;
    std::copy_n(bytes, 6, packet.destinationMac.begin());
    std::copy_n(bytes + 6, 6, packet.sourceMac.begin());
    std::copy_n(bytes + 22, 16, packet.sourceGid.begin());
    std::copy_n(bytes + 38, 16, packet.destinationGid.begin());
    packet.trafficClass = ((bytes[14] & 0x0f) << 4) | (bytes[15] >> 4);
    packet.flowLabel = (uint32_t{bytes[15]} & 0x0f) << 16 |
                       (uint32_t{bytes[16]} << 8) | bytes[17];
    packet.hopLimit = bytes[21];
    packet.pKey = detail::get16(bytes, 56);
    packet.opcode = opcode;
    packet.solicitedEvent = solicitedEvent;
    packet.ackRequest = ackRequest;
    packet.destinationQpn = detail::get24(bytes, 59);
    packet.psn = detail::get24(bytes, 63);
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
        const uint8_t wireSyndrome = bytes[66];
        if ((wireSyndrome & 0xe0) == 0) {
            packet.syndrome = Syndrome::Ack;
            packet.ackCredit = wireSyndrome;
        } else if ((wireSyndrome & 0xe0) == 0x20) {
            packet.syndrome = Syndrome::Rnr;
            packet.rnrTimer = wireSyndrome & 0x1f;
        } else {
            packet.syndrome = static_cast<Syndrome>(wireSyndrome);
        }
        packet.msn = detail::get24(bytes, 67);
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

} // namespace rocev1
} // namespace pvrdma
} // namespace gem5

#endif // __DEV_RDMA_PVRDMA_ROCEV1_HH__
