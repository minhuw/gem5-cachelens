// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "dev/rdma/pvrdma_transport.hh"

namespace gem5
{
namespace pvrdma
{
namespace transport
{
namespace
{

Frame
baseData(const uint8_t *payload, size_t size)
{
    Frame frame;
    frame.kind = Kind::Data;
    frame.flags = First | Last;
    frame.sourceQpn = 0x01020304;
    frame.destinationQpn = 0x11223344;
    frame.psn = 0x00a1b2c3;
    frame.messageId = 0x0102030405060708ULL;
    frame.totalLength = size;
    frame.segmentCount = 1;
    frame.payload = {payload, size};
    return frame;
}

Frame
baseControl(Kind kind)
{
    Frame frame;
    frame.kind = kind;
    frame.sourceQpn = 0x01020304;
    frame.destinationQpn = 0x11223344;
    frame.psn = 0x00a1b2c3;
    frame.messageId = 0x0102030405060708ULL;
    return frame;
}

std::vector<uint8_t>
encoded(const Frame &frame)
{
    std::vector<uint8_t> bytes(HeaderSize + frame.payload.size);
    const auto result = encode(frame, {bytes.data(), bytes.size()});
    EXPECT_TRUE(result);
    EXPECT_EQ(result.size, bytes.size());
    return bytes;
}

void
expectError(const std::vector<uint8_t> &bytes, CodecError error)
{
    const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, error);
}

void
expectRoundTrip(const Frame &frame)
{
    const auto bytes = encoded(frame);
    const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
    ASSERT_TRUE(result);
    EXPECT_EQ(result.frame.kind, frame.kind);
    EXPECT_EQ(result.frame.flags, frame.flags);
    EXPECT_EQ(result.frame.status, frame.status);
    EXPECT_EQ(result.frame.sourceQpn, frame.sourceQpn);
    EXPECT_EQ(result.frame.destinationQpn, frame.destinationQpn);
    EXPECT_EQ(result.frame.psn, frame.psn);
    EXPECT_EQ(result.frame.messageId, frame.messageId);
    EXPECT_EQ(result.frame.totalLength, frame.totalLength);
    EXPECT_EQ(result.frame.payloadOffset, frame.payloadOffset);
    EXPECT_EQ(result.frame.segmentIndex, frame.segmentIndex);
    EXPECT_EQ(result.frame.segmentCount, frame.segmentCount);
    EXPECT_EQ(result.frame.retryTimer, frame.retryTimer);
    ASSERT_EQ(result.frame.payload.size, frame.payload.size);
    for (size_t i = 0; i < frame.payload.size; ++i)
        EXPECT_EQ(result.frame.payload.data[i], frame.payload.data[i]);
}

TEST(PvrdmaTransportTest, Constants)
{
    EXPECT_EQ(EtherType, 0x88b5);
    EXPECT_EQ(EthernetHeaderSize, 14);
    EXPECT_EQ(HeaderSize, 48);
    EXPECT_EQ(Version, 1);
    EXPECT_EQ(MaxPayloadSize, 1024);
    EXPECT_EQ(PsnMask, 0x00ffffff);
}

TEST(PvrdmaTransportTest, EthernetEnvelopeRoundTripAndFiltering)
{
    const MacAddress source = {0x02, 0, 0, 0, 0, 1};
    const MacAddress destination = {0x02, 0, 0, 0, 0, 2};
    const std::array<uint8_t, 2> payload = {0xaa, 0xbb};
    const auto frame = baseData(payload.data(), payload.size());
    std::array<uint8_t, EthernetHeaderSize + HeaderSize + 2> bytes{};
    const auto encoded = encodeEthernet(
        frame, source, destination, {bytes.data(), bytes.size()});
    ASSERT_TRUE(encoded);
    EXPECT_EQ(encoded.size, bytes.size());
    EXPECT_TRUE(std::equal(destination.begin(), destination.end(),
                           bytes.begin()));
    EXPECT_TRUE(std::equal(source.begin(), source.end(), bytes.begin() + 6));
    EXPECT_EQ(bytes[12], 0x88);
    EXPECT_EQ(bytes[13], 0xb5);

    const auto decoded = decodeEthernet(
        {bytes.data(), bytes.size()}, bytes.size());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.source, source);
    EXPECT_EQ(decoded.destination, destination);
    EXPECT_EQ(decoded.frame.sourceQpn, frame.sourceQpn);
    EXPECT_EQ(decoded.frame.payload.size, payload.size());

    bytes[13] ^= 1;
    EXPECT_EQ(decodeEthernet({bytes.data(), bytes.size()}, bytes.size()).error,
              CodecError::BadEtherType);
    EXPECT_EQ(decodeEthernet({bytes.data(), bytes.size()}, 13).error,
              CodecError::Truncated);
}

TEST(PvrdmaTransportTest, GoldenDataHeader)
{
    const std::array<uint8_t, 3> payload = {0xde, 0xad, 0xbe};
    const auto bytes = encoded(baseData(payload.data(), payload.size()));
    const std::array<uint8_t, HeaderSize> expected = {
        0x43, 0x4c, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44,
        0x00, 0xa1, 0xb2, 0xc3, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0x00, 0x03,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin()));
    EXPECT_EQ(std::vector<uint8_t>(bytes.begin() + HeaderSize, bytes.end()),
              std::vector<uint8_t>(payload.begin(), payload.end()));
}

TEST(PvrdmaTransportTest, GoldenAckHeader)
{
    const auto bytes = encoded(baseControl(Kind::Ack));
    const std::array<uint8_t, HeaderSize> expected = {
        0x43, 0x4c, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44,
        0x00, 0xa1, 0xb2, 0xc3, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    EXPECT_EQ(std::vector<uint8_t>(expected.begin(), expected.end()), bytes);
}

TEST(PvrdmaTransportTest, GoldenRnrHeader)
{
    Frame frame = baseControl(Kind::Rnr);
    frame.retryTimer = 31;
    const auto bytes = encoded(frame);
    const std::array<uint8_t, HeaderSize> expected = {
        0x43, 0x4c, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44,
        0x00, 0xa1, 0xb2, 0xc3, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    EXPECT_EQ(std::vector<uint8_t>(expected.begin(), expected.end()), bytes);
}

TEST(PvrdmaTransportTest, GoldenErrorHeader)
{
    Frame frame = baseControl(Kind::Error);
    frame.status = CompletionStatus::RemoteAccessError;
    const auto bytes = encoded(frame);
    const std::array<uint8_t, HeaderSize> expected = {
        0x43, 0x4c, 0x01, 0x03, 0x00, 0x00, 0x00, 0x0a,
        0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44,
        0x00, 0xa1, 0xb2, 0xc3, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    EXPECT_EQ(std::vector<uint8_t>(expected.begin(), expected.end()), bytes);
}

TEST(PvrdmaTransportTest, DataSizesRoundTrip)
{
    std::array<uint8_t, MaxPayloadSize> payload = {};
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = i;
    for (const size_t size : {size_t{0}, size_t{1}, size_t{64},
                              MaxPayloadSize}) {
        expectRoundTrip(baseData(payload.data(), size));
    }
}

TEST(PvrdmaTransportTest, ControlAndBoundaryValuesRoundTrip)
{
    Frame ack = baseControl(Kind::Ack);
    ack.sourceQpn = 1;
    ack.destinationQpn = UINT32_MAX;
    ack.psn = 0;
    ack.messageId = 1;
    expectRoundTrip(ack);

    ack.psn = PsnMask;
    ack.messageId = UINT64_MAX;
    expectRoundTrip(ack);

    const uint8_t payload = 0xff;
    Frame data = baseData(&payload, 1);
    data.flags = Last;
    data.sourceQpn = UINT32_MAX;
    data.destinationQpn = UINT32_MAX;
    data.psn = PsnMask;
    data.messageId = UINT64_MAX;
    data.totalLength = UINT32_MAX;
    data.payloadOffset = UINT32_MAX - 1;
    data.segmentIndex = UINT16_MAX - 1;
    data.segmentCount = UINT16_MAX;
    expectRoundTrip(data);

    Frame rnr = baseControl(Kind::Rnr);
    rnr.retryTimer = 31;
    expectRoundTrip(rnr);

    for (const auto status : {
             CompletionStatus::LocalLengthError,
             CompletionStatus::LocalQpOperationError,
             CompletionStatus::LocalProtectionError,
             CompletionStatus::LocalAccessError,
             CompletionStatus::RemoteInvalidRequestError,
             CompletionStatus::RemoteAccessError,
             CompletionStatus::RemoteOperationError,
             CompletionStatus::RetryExceededError,
             CompletionStatus::RnrRetryExceededError,
             CompletionStatus::GeneralError}) {
        Frame error = baseControl(Kind::Error);
        error.status = status;
        expectRoundTrip(error);
    }
}

TEST(PvrdmaTransportTest, MultiSegmentMetadata)
{
    const std::array<uint8_t, 4> payload = {1, 2, 3, 4};
    Frame frame = baseData(payload.data(), payload.size());
    frame.totalLength = 12;
    frame.segmentCount = 3;

    frame.flags = First;
    expectRoundTrip(frame);

    frame.flags = 0;
    frame.payloadOffset = 4;
    frame.segmentIndex = 1;
    expectRoundTrip(frame);

    frame.flags = Last;
    frame.payloadOffset = 8;
    frame.segmentIndex = 2;
    expectRoundTrip(frame);
}

TEST(PvrdmaTransportTest, RejectsEveryTruncatedHeaderLength)
{
    const auto bytes = encoded(baseControl(Kind::Ack));
    for (size_t size = 0; size < HeaderSize; ++size) {
        const std::vector<uint8_t> truncated(bytes.begin(),
                                             bytes.begin() + size);
        const auto result = decode(
            {truncated.data(), truncated.size()}, truncated.size());
        EXPECT_EQ(result.error, CodecError::Truncated) << size;
    }
}

TEST(PvrdmaTransportTest, RejectsPayloadTruncationAndExtraLogicalPayload)
{
    const std::array<uint8_t, 4> payload = {1, 2, 3, 4};
    auto bytes = encoded(baseData(payload.data(), payload.size()));
    const std::vector<uint8_t> truncated(bytes.begin(), bytes.end() - 1);
    EXPECT_EQ(decode({truncated.data(), truncated.size()},
                     truncated.size()).error,
              CodecError::Truncated);

    bytes.push_back(0);
    EXPECT_EQ(decode({bytes.data(), bytes.size()}, bytes.size()).error,
              CodecError::ExtraPayload);
    EXPECT_TRUE(decode({bytes.data(), bytes.size()}, bytes.size() - 1));
    EXPECT_EQ(decode({bytes.data(), bytes.size()}, bytes.size() + 1).error,
              CodecError::Truncated);
}

TEST(PvrdmaTransportTest, RejectsEnvelopeFields)
{
    const auto good = encoded(baseControl(Kind::Ack));
    auto bytes = good;
    bytes[0] = 0;
    expectError(bytes, CodecError::BadMagic);
    bytes = good;
    bytes[2] = 2;
    expectError(bytes, CodecError::BadVersion);
    bytes = good;
    bytes[3] = 4;
    expectError(bytes, CodecError::BadKind);
    bytes = good;
    bytes[43] = 1;
    expectError(bytes, CodecError::BadReserved);
    bytes = good;
    bytes[47] = 1;
    expectError(bytes, CodecError::BadReserved);
}

TEST(PvrdmaTransportTest, RejectsBadFlagsPsnAndIdentity)
{
    const auto good = encoded(baseControl(Kind::Ack));
    auto bytes = good;
    bytes[5] = 1;
    expectError(bytes, CodecError::BadFlags);
    bytes = good;
    bytes[16] = 1;
    expectError(bytes, CodecError::BadPsn);
    bytes = good;
    std::fill(bytes.begin() + 8, bytes.begin() + 12, 0);
    expectError(bytes, CodecError::BadQpn);
    bytes = good;
    std::fill(bytes.begin() + 12, bytes.begin() + 16, 0);
    expectError(bytes, CodecError::BadQpn);
    bytes = good;
    std::fill(bytes.begin() + 20, bytes.begin() + 28, 0);
    expectError(bytes, CodecError::BadMessageId);
}

TEST(PvrdmaTransportTest, RejectsBadDataMetadata)
{
    const std::array<uint8_t, 1> payload = {1};
    const auto good = encoded(baseData(payload.data(), payload.size()));
    auto bytes = good;
    bytes[5] = 7;
    expectError(bytes, CodecError::BadFlags);
    bytes = good;
    bytes[5] = First;
    expectError(bytes, CodecError::BadFlags);
    bytes = good;
    bytes[7] = 1;
    expectError(bytes, CodecError::BadStatus);
    bytes = good;
    bytes[42] = 1;
    expectError(bytes, CodecError::BadRetryTimer);
    bytes = good;
    bytes[40] = 0;
    bytes[41] = 0;
    expectError(bytes, CodecError::BadSegment);
    bytes = good;
    bytes[38] = 0;
    bytes[39] = 1;
    expectError(bytes, CodecError::BadSegment);
}

TEST(PvrdmaTransportTest, RejectsDataLengthOverflowMtuAndEmptyNonzeroMessage)
{
    const std::array<uint8_t, 1> payload = {1};
    Frame frame = baseData(payload.data(), payload.size());
    frame.payloadOffset = UINT32_MAX;
    frame.totalLength = UINT32_MAX;
    EXPECT_EQ(encode(frame, {}).error, CodecError::LengthOverflow);

    auto bytes = encoded(baseData(payload.data(), payload.size()));
    std::fill(bytes.begin() + 32, bytes.begin() + 36, 0xff);
    std::fill(bytes.begin() + 28, bytes.begin() + 32, 0xff);
    expectError(bytes, CodecError::LengthOverflow);
    bytes = encoded(baseData(payload.data(), payload.size()));
    std::fill(bytes.begin() + 28, bytes.begin() + 32, 0);
    expectError(bytes, CodecError::BadLength);

    std::array<uint8_t, MaxPayloadSize + 1> large = {};
    frame = baseData(large.data(), large.size());
    EXPECT_EQ(encode(frame, {}).error, CodecError::PayloadTooLarge);

    frame = baseData(nullptr, 0);
    frame.totalLength = 1;
    EXPECT_EQ(encode(frame, {}).error, CodecError::BadLength);

    bytes = encoded(baseData(payload.data(), payload.size()));
    bytes[36] = 0x04;
    bytes[37] = 0x01;
    bytes.resize(HeaderSize + MaxPayloadSize + 1);
    expectError(bytes, CodecError::PayloadTooLarge);
}

TEST(PvrdmaTransportTest, RejectsInvalidZeroLengthRepresentation)
{
    Frame frame = baseData(nullptr, 0);
    frame.payloadOffset = 1;
    EXPECT_EQ(encode(frame, {}).error, CodecError::BadLength);
    frame = baseData(nullptr, 0);
    frame.segmentCount = 2;
    frame.flags = First;
    EXPECT_EQ(encode(frame, {}).error, CodecError::BadLength);
}

TEST(PvrdmaTransportTest, RejectsInvalidControlCombinations)
{
    for (const Kind kind : {Kind::Ack, Kind::Rnr, Kind::Error}) {
        Frame frame = baseControl(kind);
        if (kind == Kind::Error)
            frame.status = CompletionStatus::GeneralError;

        Frame bad = frame;
        bad.flags = First;
        EXPECT_EQ(encode(bad, {}).error, CodecError::BadFlags);
        bad = frame;
        bad.totalLength = 1;
        EXPECT_EQ(encode(bad, {}).error, CodecError::BadLength);
        bad = frame;
        bad.payloadOffset = 1;
        EXPECT_EQ(encode(bad, {}).error, CodecError::BadLength);
        bad = frame;
        bad.segmentIndex = 1;
        EXPECT_EQ(encode(bad, {}).error, CodecError::BadLength);
        bad = frame;
        bad.segmentCount = 1;
        EXPECT_EQ(encode(bad, {}).error, CodecError::BadLength);
        bad = frame;
        const uint8_t payload = 1;
        bad.payload = {&payload, 1};
        EXPECT_EQ(encode(bad, {}).error, CodecError::BadLength);
    }

    Frame ack = baseControl(Kind::Ack);
    ack.status = CompletionStatus::LocalLengthError;
    EXPECT_EQ(encode(ack, {}).error, CodecError::BadStatus);
    ack = baseControl(Kind::Ack);
    ack.retryTimer = 1;
    EXPECT_EQ(encode(ack, {}).error, CodecError::BadRetryTimer);

    Frame rnr = baseControl(Kind::Rnr);
    rnr.status = CompletionStatus::LocalLengthError;
    EXPECT_EQ(encode(rnr, {}).error, CodecError::BadStatus);
    rnr = baseControl(Kind::Rnr);
    rnr.retryTimer = 32;
    EXPECT_EQ(encode(rnr, {}).error, CodecError::BadRetryTimer);

    Frame error = baseControl(Kind::Error);
    error.status = CompletionStatus::Success;
    EXPECT_EQ(encode(error, {}).error, CodecError::BadStatus);
    error.status = CompletionStatus::WorkRequestFlushError;
    EXPECT_EQ(encode(error, {}).error, CodecError::BadStatus);
    error.status = CompletionStatus::GeneralError;
    error.retryTimer = 1;
    EXPECT_EQ(encode(error, {}).error, CodecError::BadRetryTimer);
}

TEST(PvrdmaTransportTest, DecodeRejectsInvalidControlCombinations)
{
    for (const Kind kind : {Kind::Ack, Kind::Rnr, Kind::Error}) {
        Frame frame = baseControl(kind);
        if (kind == Kind::Error)
            frame.status = CompletionStatus::GeneralError;
        const auto good = encoded(frame);

        for (const size_t offset : {size_t{28}, size_t{32}, size_t{38},
                                    size_t{40}}) {
            auto bytes = good;
            bytes[offset + 1] = 1;
            expectError(bytes, CodecError::BadLength);
        }
        auto bytes = good;
        bytes[5] = First;
        expectError(bytes, CodecError::BadFlags);
        bytes = good;
        bytes.push_back(1);
        bytes[37] = 1;
        expectError(bytes, CodecError::BadLength);
    }

    auto bytes = encoded(baseControl(Kind::Ack));
    bytes[7] = 1;
    expectError(bytes, CodecError::BadStatus);
    bytes = encoded(baseControl(Kind::Ack));
    bytes[42] = 1;
    expectError(bytes, CodecError::BadRetryTimer);

    bytes = encoded(baseControl(Kind::Rnr));
    bytes[42] = 32;
    expectError(bytes, CodecError::BadRetryTimer);

    Frame error = baseControl(Kind::Error);
    error.status = CompletionStatus::GeneralError;
    bytes = encoded(error);
    bytes[6] = 0;
    bytes[7] = static_cast<uint8_t>(CompletionStatus::WorkRequestFlushError);
    expectError(bytes, CodecError::BadStatus);
    bytes = encoded(error);
    bytes[42] = 1;
    expectError(bytes, CodecError::BadRetryTimer);
}

TEST(PvrdmaTransportTest, RejectsSmallEncodeBuffer)
{
    Frame frame = baseControl(Kind::Ack);
    std::array<uint8_t, HeaderSize - 1> bytes = {};
    EXPECT_EQ(encode(frame, {bytes.data(), bytes.size()}).error,
              CodecError::BufferTooSmall);
}

} // anonymous namespace
} // namespace transport
} // namespace pvrdma
} // namespace gem5
