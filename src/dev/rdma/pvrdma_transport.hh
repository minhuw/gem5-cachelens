// SPDX-License-Identifier: BSD-3-Clause

#ifndef __DEV_RDMA_PVRDMA_TRANSPORT_HH__
#define __DEV_RDMA_PVRDMA_TRANSPORT_HH__

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "dev/rdma/pvrdma_abi.hh"

namespace gem5
{
namespace pvrdma
{
namespace transport
{

inline constexpr uint16_t EtherType = 0x88b5;
inline constexpr size_t EthernetHeaderSize = 14;
inline constexpr size_t HeaderSize = 48;
inline constexpr uint8_t Version = 1;
inline constexpr size_t MaxPayloadSize = 1024;
inline constexpr uint32_t PsnMask = 0x00ffffff;
inline constexpr uint16_t First = uint16_t{1} << 0;
inline constexpr uint16_t Last = uint16_t{1} << 1;

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

enum class Kind : uint8_t
{
    Data = 0,
    Ack = 1,
    Rnr = 2,
    Error = 3,
};

enum class CodecError
{
    None,
    BufferTooSmall,
    Truncated,
    ExtraPayload,
    BadMagic,
    BadEtherType,
    BadVersion,
    BadKind,
    BadFlags,
    BadReserved,
    BadStatus,
    BadQpn,
    BadPsn,
    BadMessageId,
    BadLength,
    BadSegment,
    BadRetryTimer,
    PayloadTooLarge,
    LengthOverflow,
};

using MacAddress = std::array<uint8_t, 6>;

struct Frame
{
    Kind kind = Kind::Data;
    uint16_t flags = 0;
    CompletionStatus status = CompletionStatus::Success;
    uint32_t sourceQpn = 0;
    uint32_t destinationQpn = 0;
    uint32_t psn = 0;
    uint64_t messageId = 0;
    uint32_t totalLength = 0;
    uint32_t payloadOffset = 0;
    uint16_t segmentIndex = 0;
    uint16_t segmentCount = 0;
    uint8_t retryTimer = 0;
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
    Frame frame;

    explicit operator bool() const { return error == CodecError::None; }
};

struct EthernetDecodeResult : DecodeResult
{
    MacAddress destination;
    MacAddress source;
};

namespace detail
{

inline void
put16(uint8_t *bytes, size_t offset, uint16_t value)
{
    bytes[offset] = value >> 8;
    bytes[offset + 1] = value;
}

inline void
put32(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = value >> 24;
    bytes[offset + 1] = value >> 16;
    bytes[offset + 2] = value >> 8;
    bytes[offset + 3] = value;
}

inline void
put64(uint8_t *bytes, size_t offset, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i)
        bytes[offset + i] = value >> (56 - 8 * i);
}

inline uint16_t
get16(const uint8_t *bytes, size_t offset)
{
    return (uint16_t{bytes[offset]} << 8) | bytes[offset + 1];
}

inline uint32_t
get32(const uint8_t *bytes, size_t offset)
{
    return (uint32_t{bytes[offset]} << 24) |
           (uint32_t{bytes[offset + 1]} << 16) |
           (uint32_t{bytes[offset + 2]} << 8) |
           bytes[offset + 3];
}

inline uint64_t
get64(const uint8_t *bytes, size_t offset)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | bytes[offset + i];
    return value;
}

inline bool
supportedErrorStatus(CompletionStatus status)
{
    switch (status) {
      case CompletionStatus::LocalLengthError:
      case CompletionStatus::LocalQpOperationError:
      case CompletionStatus::LocalProtectionError:
      case CompletionStatus::LocalAccessError:
      case CompletionStatus::RemoteInvalidRequestError:
      case CompletionStatus::RemoteAccessError:
      case CompletionStatus::RemoteOperationError:
      case CompletionStatus::RetryExceededError:
      case CompletionStatus::RnrRetryExceededError:
      case CompletionStatus::GeneralError:
        return true;
      default:
        return false;
    }
}

inline CodecError
validate(const Frame &frame)
{
    if (frame.sourceQpn == 0 || frame.destinationQpn == 0)
        return CodecError::BadQpn;
    if (frame.psn & ~PsnMask)
        return CodecError::BadPsn;
    if (frame.messageId == 0)
        return CodecError::BadMessageId;

    switch (frame.kind) {
      case Kind::Data: {
        if (frame.flags & ~(First | Last))
            return CodecError::BadFlags;
        if (frame.status != CompletionStatus::Success)
            return CodecError::BadStatus;
        if (frame.retryTimer != 0)
            return CodecError::BadRetryTimer;
        if (frame.payload.size > MaxPayloadSize)
            return CodecError::PayloadTooLarge;
        if (frame.payload.size && !frame.payload.data)
            return CodecError::BadLength;
        if (frame.segmentCount == 0 ||
            frame.segmentIndex >= frame.segmentCount) {
            return CodecError::BadSegment;
        }
        const uint16_t expected_flags =
            (frame.segmentIndex == 0 ? First : 0) |
            (frame.segmentIndex + 1 == frame.segmentCount ? Last : 0);
        if (frame.flags != expected_flags)
            return CodecError::BadFlags;
        if (frame.payloadOffset >
            std::numeric_limits<uint32_t>::max() - frame.payload.size) {
            return CodecError::LengthOverflow;
        }
        if (frame.payloadOffset + frame.payload.size > frame.totalLength)
            return CodecError::BadLength;
        if (frame.totalLength == 0) {
            if (frame.payloadOffset != 0 || frame.payload.size != 0 ||
                frame.segmentIndex != 0 || frame.segmentCount != 1 ||
                frame.flags != (First | Last)) {
                return CodecError::BadLength;
            }
        } else if (frame.payload.size == 0) {
            return CodecError::BadLength;
        }
        return CodecError::None;
      }
      case Kind::Ack:
        if (frame.status != CompletionStatus::Success)
            return CodecError::BadStatus;
        break;
      case Kind::Rnr:
        if (frame.status != CompletionStatus::Success)
            return CodecError::BadStatus;
        if (frame.retryTimer > 31)
            return CodecError::BadRetryTimer;
        break;
      case Kind::Error:
        if (!supportedErrorStatus(frame.status))
            return CodecError::BadStatus;
        break;
      default:
        return CodecError::BadKind;
    }

    if (frame.flags != 0)
        return CodecError::BadFlags;
    if (frame.totalLength != 0 || frame.payloadOffset != 0 ||
        frame.payload.size != 0 || frame.segmentIndex != 0 ||
        frame.segmentCount != 0) {
        return CodecError::BadLength;
    }
    if (frame.kind != Kind::Rnr && frame.retryTimer != 0)
        return CodecError::BadRetryTimer;
    return CodecError::None;
}

} // namespace detail

inline EncodeResult
encode(const Frame &frame, MutableBytes output)
{
    const CodecError error = detail::validate(frame);
    if (error != CodecError::None)
        return {error, 0};

    const size_t size = HeaderSize + frame.payload.size;
    if (!output.data || output.size < size)
        return {CodecError::BufferTooSmall, 0};

    detail::put16(output.data, 0, 0x434c);
    output.data[2] = Version;
    output.data[3] = static_cast<uint8_t>(frame.kind);
    detail::put16(output.data, 4, frame.flags);
    detail::put16(output.data, 6, static_cast<uint16_t>(frame.status));
    detail::put32(output.data, 8, frame.sourceQpn);
    detail::put32(output.data, 12, frame.destinationQpn);
    detail::put32(output.data, 16, frame.psn);
    detail::put64(output.data, 20, frame.messageId);
    detail::put32(output.data, 28, frame.totalLength);
    detail::put32(output.data, 32, frame.payloadOffset);
    detail::put16(output.data, 36, frame.payload.size);
    detail::put16(output.data, 38, frame.segmentIndex);
    detail::put16(output.data, 40, frame.segmentCount);
    output.data[42] = frame.retryTimer;
    output.data[43] = 0;
    detail::put32(output.data, 44, 0);
    if (frame.payload.size) {
        std::memcpy(output.data + HeaderSize, frame.payload.data,
                    frame.payload.size);
    }
    return {CodecError::None, size};
}

inline DecodeResult
decode(Bytes input, size_t logicalLength)
{
    if (logicalLength > input.size || !input.data)
        return {CodecError::Truncated, {}};
    if (logicalLength < HeaderSize)
        return {CodecError::Truncated, {}};
    if (detail::get16(input.data, 0) != 0x434c)
        return {CodecError::BadMagic, {}};
    if (input.data[2] != Version)
        return {CodecError::BadVersion, {}};
    if (input.data[3] > static_cast<uint8_t>(Kind::Error))
        return {CodecError::BadKind, {}};
    if (input.data[43] != 0 || detail::get32(input.data, 44) != 0)
        return {CodecError::BadReserved, {}};

    Frame frame;
    frame.kind = static_cast<Kind>(input.data[3]);
    frame.flags = detail::get16(input.data, 4);
    frame.status = static_cast<CompletionStatus>(detail::get16(input.data, 6));
    frame.sourceQpn = detail::get32(input.data, 8);
    frame.destinationQpn = detail::get32(input.data, 12);
    frame.psn = detail::get32(input.data, 16);
    frame.messageId = detail::get64(input.data, 20);
    frame.totalLength = detail::get32(input.data, 28);
    frame.payloadOffset = detail::get32(input.data, 32);
    const size_t payload_size = detail::get16(input.data, 36);
    if (payload_size > MaxPayloadSize)
        return {CodecError::PayloadTooLarge, {}};
    frame.segmentIndex = detail::get16(input.data, 38);
    frame.segmentCount = detail::get16(input.data, 40);
    frame.retryTimer = input.data[42];

    const size_t expected_size = HeaderSize + payload_size;
    if (logicalLength < expected_size)
        return {CodecError::Truncated, {}};
    if (logicalLength > expected_size)
        return {CodecError::ExtraPayload, {}};
    frame.payload = {input.data + HeaderSize, payload_size};

    const CodecError error = detail::validate(frame);
    return {error, error == CodecError::None ? frame : Frame{}};
}

inline EncodeResult
encodeEthernet(const Frame &frame, const MacAddress &source,
               const MacAddress &destination, MutableBytes output)
{
    if (!output.data || output.size < EthernetHeaderSize)
        return {CodecError::BufferTooSmall, 0};
    std::copy(destination.begin(), destination.end(), output.data);
    std::copy(source.begin(), source.end(), output.data + 6);
    detail::put16(output.data, 12, EtherType);
    auto result = encode(frame,
        {output.data + EthernetHeaderSize, output.size - EthernetHeaderSize});
    if (result)
        result.size += EthernetHeaderSize;
    return result;
}

inline EthernetDecodeResult
decodeEthernet(Bytes input, size_t logicalLength)
{
    EthernetDecodeResult result;
    if (!input.data || logicalLength > input.size ||
        logicalLength < EthernetHeaderSize) {
        result.error = CodecError::Truncated;
        return result;
    }
    std::copy_n(input.data, 6, result.destination.begin());
    std::copy_n(input.data + 6, 6, result.source.begin());
    if (detail::get16(input.data, 12) != EtherType) {
        result.error = CodecError::BadEtherType;
        return result;
    }
    const auto decoded = decode(
        {input.data + EthernetHeaderSize, input.size - EthernetHeaderSize},
        logicalLength - EthernetHeaderSize);
    result.error = decoded.error;
    result.frame = decoded.frame;
    return result;
}

} // namespace transport
} // namespace pvrdma
} // namespace gem5

#endif // __DEV_RDMA_PVRDMA_TRANSPORT_HH__
