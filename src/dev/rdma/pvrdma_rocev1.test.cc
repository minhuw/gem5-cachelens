// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dev/rdma/pvrdma_rocev1.hh"

namespace gem5
{
namespace pvrdma
{
namespace rocev1
{
namespace
{

const MacAddress MacA = {0x02, 0, 0, 0, 0, 1};
const MacAddress MacB = {0x02, 0, 0, 0, 0, 2};
const Gid GidA = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0, 0, 1};
const Gid GidB = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0, 0, 2};

std::vector<uint8_t>
fromHex(const std::string &hex)
{
    EXPECT_EQ(hex.size() % 2, 0);
    std::vector<uint8_t> bytes(hex.size() / 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        unsigned value = 0;
        EXPECT_EQ(std::sscanf(hex.c_str() + 2 * i, "%2x", &value), 1);
        bytes[i] = value;
    }
    return bytes;
}

Packet
sendPacket(Opcode opcode, uint32_t psn, const uint8_t *payload, size_t size)
{
    Packet packet;
    packet.destinationMac = MacB;
    packet.sourceMac = MacA;
    packet.sourceGid = GidA;
    packet.destinationGid = GidB;
    packet.trafficClass = 0x2a;
    packet.flowLabel = 0x34567;
    packet.hopLimit = 0x40;
    packet.opcode = opcode;
    packet.destinationQpn = 0x010203;
    packet.psn = psn;
    packet.payload = {payload, size};
    return packet;
}

Packet
controlPacket(Syndrome syndrome, uint32_t psn = 0x112235)
{
    Packet packet;
    packet.destinationMac = MacA;
    packet.sourceMac = MacB;
    packet.sourceGid = GidB;
    packet.destinationGid = GidA;
    packet.trafficClass = 0x2a;
    packet.flowLabel = 0x34567;
    packet.hopLimit = 0x40;
    packet.opcode = Opcode::Acknowledge;
    packet.ackRequest = false;
    packet.destinationQpn = 0x0a0b0c;
    packet.psn = psn;
    packet.syndrome = syndrome;
    packet.msn = 0x445566;
    return packet;
}

std::vector<uint8_t>
encodePacket(const Packet &packet)
{
    std::vector<uint8_t> bytes(encodedSize(packet));
    const auto result = encode(packet, {bytes.data(), bytes.size()});
    EXPECT_TRUE(result);
    EXPECT_EQ(result.size, bytes.size());
    return bytes;
}

void
fixIcrc(std::vector<uint8_t> &bytes)
{
    const size_t roceSize = bytes.size() - EthernetHeaderSize - IcrcSize;
    detail::put32Le(bytes.data(), bytes.size() - IcrcSize,
                    detail::icrc(bytes.data() + EthernetHeaderSize,
                                 roceSize));
}

void
expectError(const std::vector<uint8_t> &bytes, CodecError error)
{
    const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, error);
}

void
expectPacket(const Packet &actual, const Packet &expected)
{
    EXPECT_EQ(actual.destinationMac, expected.destinationMac);
    EXPECT_EQ(actual.sourceMac, expected.sourceMac);
    EXPECT_EQ(actual.sourceGid, expected.sourceGid);
    EXPECT_EQ(actual.destinationGid, expected.destinationGid);
    EXPECT_EQ(actual.trafficClass, expected.trafficClass);
    EXPECT_EQ(actual.flowLabel, expected.flowLabel);
    EXPECT_EQ(actual.hopLimit, expected.hopLimit);
    EXPECT_EQ(actual.pKey, expected.pKey);
    EXPECT_EQ(actual.opcode, expected.opcode);
    EXPECT_EQ(actual.solicitedEvent, expected.solicitedEvent);
    EXPECT_EQ(actual.ackRequest, expected.ackRequest);
    EXPECT_EQ(actual.destinationQpn, expected.destinationQpn);
    EXPECT_EQ(actual.psn, expected.psn);
    EXPECT_EQ(actual.syndrome, expected.syndrome);
    EXPECT_EQ(actual.ackCredit, expected.ackCredit);
    EXPECT_EQ(actual.rnrTimer, expected.rnrTimer);
    EXPECT_EQ(actual.msn, expected.msn);
    ASSERT_EQ(actual.payload.size, expected.payload.size);
    if (actual.payload.size) {
        EXPECT_TRUE(std::equal(actual.payload.data,
                               actual.payload.data + actual.payload.size,
                               expected.payload.data));
    }
}

void
expectGolden(const char *hex, const Packet &packet)
{
    const auto golden = fromHex(hex);
    EXPECT_EQ(encodePacket(packet), golden);
    const auto result = decode({golden.data(), golden.size()}, golden.size());
    ASSERT_TRUE(result);
    expectPacket(result.packet, packet);
    if (packet.payload.size) {
        EXPECT_EQ(result.packet.payload.data,
                  golden.data() + SendHeaderSize);
    }
}

uint32_t
rotateRight(uint32_t value, unsigned shift)
{
    return (value >> shift) | (value << (32 - shift));
}

std::array<uint8_t, 32>
sha256(const std::vector<uint8_t> &input)
{
    static constexpr uint32_t K[] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    std::vector<uint8_t> message = input;
    const uint64_t bitLength = uint64_t{message.size()} * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56)
        message.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back(bitLength >> shift);

    std::array<uint32_t, 8> hash = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    for (size_t offset = 0; offset < message.size(); offset += 64) {
        uint32_t words[64] = {};
        for (size_t i = 0; i < 16; ++i) {
            words[i] = (uint32_t{message[offset + 4 * i]} << 24) |
                       (uint32_t{message[offset + 4 * i + 1]} << 16) |
                       (uint32_t{message[offset + 4 * i + 2]} << 8) |
                       message[offset + 4 * i + 3];
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotateRight(words[i - 15], 7) ^
                                rotateRight(words[i - 15], 18) ^
                                (words[i - 15] >> 3);
            const uint32_t s1 = rotateRight(words[i - 2], 17) ^
                                rotateRight(words[i - 2], 19) ^
                                (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3];
        uint32_t e = hash[4], f = hash[5], g = hash[6], h = hash[7];
        for (size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^
                                rotateRight(e, 25);
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + choice + K[i] + words[i];
            const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^
                                rotateRight(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        const uint32_t state[] = {a, b, c, d, e, f, g, h};
        for (size_t i = 0; i < hash.size(); ++i)
            hash[i] += state[i];
    }

    std::array<uint8_t, 32> digest{};
    for (size_t i = 0; i < hash.size(); ++i) {
        digest[4 * i] = hash[i] >> 24;
        digest[4 * i + 1] = hash[i] >> 16;
        digest[4 * i + 2] = hash[i] >> 8;
        digest[4 * i + 3] = hash[i];
    }
    return digest;
}

TEST(PvrdmaRocev1Test, Constants)
{
    EXPECT_EQ(EtherType, 0x8915);
    EXPECT_EQ(GrhNextHeader, 0x1b);
    EXPECT_EQ(DefaultPKey, 0xffff);
    EXPECT_EQ(SendHeaderSize, 66);
    EXPECT_EQ(ControlFrameSize, 74);
    EXPECT_EQ(MaxPayloadSize, 1024);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::SendFirst), 0x00);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::SendMiddle), 0x01);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::SendLast), 0x02);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::SendOnly), 0x04);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::Acknowledge), 0x11);
    EXPECT_EQ(static_cast<uint8_t>(Syndrome::Ack), 0x00);
}

TEST(PvrdmaRocev1Test, LiteralSendGoldenVectors)
{
    const std::array<uint8_t, 3> onlyPayload = {0xde, 0xad, 0xbe};
    expectGolden(
        "020000000002020000000001891562a3456700141b40"
        "20010db800000000000000000000000120010db8000000000000000000000002"
        "0410ffff0001020380112230deadbe00e2e22e75",
        sendPacket(Opcode::SendOnly, 0x112230,
                   onlyPayload.data(), onlyPayload.size()));

    const std::array<uint8_t, 5> lastPayload = {0xca, 0xfe, 0xba, 0xbe, 1};
    expectGolden(
        "020000000002020000000001891562a3456700181b40"
        "20010db800000000000000000000000120010db8000000000000000000000002"
        "0230ffff0001020380112235cafebabe010000000c7d7811",
        sendPacket(Opcode::SendLast, 0x112235,
                   lastPayload.data(), lastPayload.size()));
}

TEST(PvrdmaRocev1Test, LiteralControlGoldenVectors)
{
    struct Vector
    {
        const char *hex;
        Syndrome syndrome;
        uint8_t timer;
        uint32_t psn;
    };
    const Vector vectors[] = {
        {"020000000001020000000002891562a3456700141b40"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "1100ffff000a0b0c001122351f4455664791704f",
         Syndrome::Ack, 0, 0x112235},
        {"020000000001020000000002891562a3456700141b40"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "1100ffff000a0b0c001122333944556605946945",
         Syndrome::Rnr, 25, 0x112233},
        {"020000000001020000000002891562a3456700141b40"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "1100ffff000a0b0c00112234604455663d084c41",
         Syndrome::SequenceNak, 0, 0x112234},
        {"020000000001020000000002891562a3456700141b40"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "1100ffff000a0b0c0011223561445566e84690c4",
         Syndrome::InvalidRequestNak, 0, 0x112235},
        {"020000000001020000000002891562a3456700141b40"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "1100ffff000a0b0c001122356244556606e925d6",
         Syndrome::RemoteAccessNak, 0, 0x112235},
        {"020000000001020000000002891562a3456700141b40"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "1100ffff000a0b0c0011223563445566638e996e",
         Syndrome::RemoteOperationNak, 0, 0x112235},
    };
    for (const auto &vector : vectors) {
        Packet packet = controlPacket(vector.syndrome, vector.psn);
        packet.rnrTimer = vector.timer;
        expectGolden(vector.hex, packet);
    }
}

TEST(PvrdmaRocev1Test, AckCreditGoldenVectors)
{
    Packet ack0 = controlPacket(Syndrome::Ack);
    ack0.ackCredit = 0;
    expectGolden(
        "020000000001020000000002891562a3456700141b40"
        "20010db800000000000000000000000220010db8000000000000000000000001"
        "1100ffff000a0b0c00112235004455668ed60a47",
        ack0);

    Packet ack30 = controlPacket(Syndrome::Ack);
    ack30.ackCredit = 30;
    expectGolden(
        "020000000001020000000002891562a3456700141b40"
        "20010db800000000000000000000000220010db8000000000000000000000001"
        "1100ffff000a0b0c001122351e44556622f6ccf7",
        ack30);

    for (uint8_t credit = 0; credit <= 31; ++credit) {
        Packet packet = controlPacket(Syndrome::Ack);
        packet.ackCredit = credit;
        const auto bytes = encodePacket(packet);
        EXPECT_EQ(bytes[66], credit);
        const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
        ASSERT_TRUE(result) << unsigned{credit};
        EXPECT_EQ(result.packet.syndrome, Syndrome::Ack);
        EXPECT_EQ(result.packet.ackCredit, credit);
    }
}

TEST(PvrdmaRocev1Test, FirstAndMiddleGoldenHashes)
{
    std::array<uint8_t, MaxPayloadSize> firstPayload{};
    std::array<uint8_t, MaxPayloadSize> middlePayload{};
    for (size_t i = 0; i < MaxPayloadSize; ++i) {
        firstPayload[i] = i;
        middlePayload[i] = 0xff - i;
    }
    const auto first = encodePacket(sendPacket(
        Opcode::SendFirst, 0x112233,
        firstPayload.data(), firstPayload.size()));
    const auto middle = encodePacket(sendPacket(
        Opcode::SendMiddle, 0x112234,
        middlePayload.data(), middlePayload.size()));

    EXPECT_EQ(first.size(), 70 + MaxPayloadSize);
    EXPECT_EQ(middle.size(), 70 + MaxPayloadSize);
    EXPECT_EQ(std::vector<uint8_t>(first.end() - 4, first.end()),
              fromHex("b47c551d"));
    EXPECT_EQ(std::vector<uint8_t>(middle.end() - 4, middle.end()),
              fromHex("856c4c27"));
    const auto firstHash = sha256(first);
    const auto middleHash = sha256(middle);
    EXPECT_EQ(std::vector<uint8_t>(firstHash.begin(), firstHash.end()),
              fromHex("6013c90e76fbfaab60740b7e8ad7a132"
                      "bf8889a24c6cd4922dac017a24fc38db"));
    EXPECT_EQ(std::vector<uint8_t>(middleHash.begin(), middleHash.end()),
              fromHex("764d2553dca0f928c6f5a3cd82f6ab5d"
                      "c2a1c55fc73b9135db4566b0dd18dbe7"));

    ASSERT_TRUE(decode({first.data(), first.size()}, first.size()));
    ASSERT_TRUE(decode({middle.data(), middle.size()}, middle.size()));

    Packet noAckRequest = sendPacket(Opcode::SendFirst, 0x112233,
                                     firstPayload.data(), firstPayload.size());
    noAckRequest.ackRequest = false;
    const auto noAckRequestBytes = encodePacket(noAckRequest);
    EXPECT_EQ(noAckRequestBytes[62], 0);
    EXPECT_EQ(std::vector<uint8_t>(noAckRequestBytes.end() - IcrcSize,
                                   noAckRequestBytes.end()),
              fromHex("71807f94"));
    const auto noAckRequestResult = decode(
        {noAckRequestBytes.data(), noAckRequestBytes.size()},
        noAckRequestBytes.size());
    ASSERT_TRUE(noAckRequestResult);
    EXPECT_FALSE(noAckRequestResult.packet.ackRequest);
}

TEST(PvrdmaRocev1Test, SolicitedEventAndCongestionBits)
{
    const std::array<uint8_t, 3> payload = {0xde, 0xad, 0xbe};
    Packet solicited = sendPacket(Opcode::SendOnly, 0x112230,
                                  payload.data(), payload.size());
    solicited.solicitedEvent = true;
    expectGolden(
        "020000000002020000000001891562a3456700141b40"
        "20010db800000000000000000000000120010db8000000000000000000000002"
        "0490ffff0001020380112230deadbe005912a93b",
        solicited);

    auto congestion = encodePacket(sendPacket(
        Opcode::SendOnly, 0x112230, payload.data(), payload.size()));
    congestion[58] = 0xc0;
    fixIcrc(congestion);
    const auto result = decode({congestion.data(), congestion.size()},
                               congestion.size());
    ASSERT_TRUE(result);
    EXPECT_EQ(result.packet.payload.size, payload.size());

    std::array<uint8_t, MaxPayloadSize> fullPayload{};
    Packet first = sendPacket(Opcode::SendFirst, 1, fullPayload.data(),
                              fullPayload.size());
    first.solicitedEvent = true;
    std::vector<uint8_t> output(encodedSize(first));
    EXPECT_EQ(encode(first, {output.data(), output.size()}).error,
              CodecError::BadReserved);

    Packet control = controlPacket(Syndrome::Ack);
    control.solicitedEvent = true;
    EXPECT_EQ(encode(control, {output.data(), output.size()}).error,
              CodecError::BadReserved);
}

TEST(PvrdmaRocev1Test, ZeroLengthOnlyAndAllPadCounts)
{
    std::array<uint8_t, 4> payload = {1, 2, 3, 4};
    for (size_t size = 0; size <= payload.size(); ++size) {
        const auto packet = sendPacket(Opcode::SendOnly, 7,
                                       payload.data(), size);
        const auto bytes = encodePacket(packet);
        const size_t pad = (-size) & 3;
        EXPECT_EQ(bytes.size(), 70 + size + pad);
        EXPECT_EQ((bytes[55] >> 4) & 3, pad);
        EXPECT_TRUE(std::all_of(bytes.end() - IcrcSize - pad,
                                bytes.end() - IcrcSize,
                                [](uint8_t byte) { return byte == 0; }));
        const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
        ASSERT_TRUE(result) << size;
        EXPECT_EQ(result.packet.payload.size, size);
        EXPECT_EQ(result.packet.payload.data, bytes.data() + SendHeaderSize);
    }
}

TEST(PvrdmaRocev1Test, RejectsEveryTruncationAndExtraBytes)
{
    std::array<uint8_t, MaxPayloadSize> payload{};
    const std::vector<std::vector<uint8_t>> frames = {
        encodePacket(controlPacket(Syndrome::Ack)),
        encodePacket(sendPacket(Opcode::SendFirst, 1,
                                payload.data(), payload.size())),
    };
    for (const auto &frame : frames) {
        for (size_t size = 0; size < frame.size(); ++size) {
            EXPECT_EQ(decode({frame.data(), frame.size()}, size).error,
                      CodecError::Truncated) << size;
        }
    }

    const auto &bytes = frames.front();
    auto extra = bytes;
    extra.push_back(0);
    EXPECT_EQ(decode({extra.data(), extra.size()}, extra.size()).error,
              CodecError::ExtraBytes);
    EXPECT_TRUE(decode({extra.data(), extra.size()}, bytes.size()));
    EXPECT_EQ(decode({bytes.data(), bytes.size()}, bytes.size() + 1).error,
              CodecError::Truncated);
    EXPECT_EQ(decode({nullptr, bytes.size()}, bytes.size()).error,
              CodecError::Truncated);
}

TEST(PvrdmaRocev1Test, RejectsCorruptionAndUnsupportedOpcode)
{
    const std::array<uint8_t, 3> payload = {1, 2, 3};
    const auto good = encodePacket(sendPacket(
        Opcode::SendOnly, 7, payload.data(), payload.size()));
    for (const size_t offset : {size_t{22}, SendHeaderSize,
                                good.size() - 1}) {
        auto bad = good;
        bad[offset] ^= 1;
        EXPECT_EQ(decode({bad.data(), bad.size()}, bad.size()).error,
                  CodecError::BadIcrc) << offset;
    }

    auto bad = good;
    bad[54] = 0xff;
    expectError(bad, CodecError::BadOpcode);
}

TEST(PvrdmaRocev1Test, RejectsBadGrhAndBthFields)
{
    const uint8_t payload = 1;
    const auto good = encodePacket(sendPacket(
        Opcode::SendOnly, 7, &payload, 1));

    struct Mutation
    {
        size_t offset;
        uint8_t value;
        CodecError error;
    };
    const Mutation mutations[] = {
        {12, 0, CodecError::BadEtherType},
        {14, 0x52, CodecError::BadGrhVersion},
        {20, 0x11, CodecError::BadGrhNextHeader},
        {55, 0x11, CodecError::BadBthVersion},
        {55, 0x50, CodecError::BadReserved},
        {58, 1, CodecError::BadReserved},
        {62, 0x81, CodecError::BadReserved},
        {56, 0, CodecError::BadPKey},
    };
    for (const auto &mutation : mutations) {
        auto bad = good;
        bad[mutation.offset] = mutation.value;
        fixIcrc(bad);
        expectError(bad, mutation.error);
    }

    auto shortGrh = good;
    shortGrh[18] = shortGrh[19] = 0;
    expectError(shortGrh, CodecError::BadGrhLength);

    auto longGrh = good;
    ++longGrh[19];
    expectError(longGrh, CodecError::Truncated);

    auto badPad = good;
    badPad[SendHeaderSize + 1] = 1;
    fixIcrc(badPad);
    expectError(badPad, CodecError::BadPad);

    badPad = encodePacket(controlPacket(Syndrome::Ack));
    badPad[55] = 0x10;
    fixIcrc(badPad);
    expectError(badPad, CodecError::BadPad);

    auto badAckRequest = encodePacket(controlPacket(Syndrome::Ack));
    badAckRequest[62] = 0x80;
    fixIcrc(badAckRequest);
    expectError(badAckRequest, CodecError::BadAckRequest);

    std::array<uint8_t, MaxPayloadSize> fullPayload{};
    auto badSolicited = encodePacket(sendPacket(
        Opcode::SendFirst, 1, fullPayload.data(), fullPayload.size()));
    badSolicited[55] |= 0x80;
    fixIcrc(badSolicited);
    expectError(badSolicited, CodecError::BadReserved);
}

TEST(PvrdmaRocev1Test, ValidatesQpnPsnMsnAndSyndromeBoundaries)
{
    Packet packet = controlPacket(Syndrome::Ack);
    std::array<uint8_t, ControlFrameSize> output{};
    for (const uint32_t qpn : {uint32_t{1}, Field24Mask - 1}) {
        packet.destinationQpn = qpn;
        EXPECT_TRUE(encode(packet, {output.data(), output.size()}));
    }
    for (const uint32_t qpn : {uint32_t{0}, Field24Mask,
                               Field24Mask + 1}) {
        packet.destinationQpn = qpn;
        EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
                  CodecError::BadQpn);
    }

    packet.destinationQpn = 1;
    for (const uint32_t psn : {uint32_t{0}, Field24Mask}) {
        packet.psn = psn;
        EXPECT_TRUE(encode(packet, {output.data(), output.size()}));
    }
    packet.psn = Field24Mask + 1;
    EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
              CodecError::BadPsn);

    packet.psn = 0;
    for (const uint32_t msn : {uint32_t{0}, Field24Mask}) {
        packet.msn = msn;
        EXPECT_TRUE(encode(packet, {output.data(), output.size()}));
    }
    packet.msn = Field24Mask + 1;
    EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
              CodecError::BadMsn);

    packet.msn = 0;
    packet.ackCredit = 0;
    EXPECT_TRUE(encode(packet, {output.data(), output.size()}));
    packet.ackCredit = 31;
    EXPECT_TRUE(encode(packet, {output.data(), output.size()}));
    packet.ackCredit = 32;
    EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
              CodecError::BadSyndrome);

    packet.ackCredit = 31;
    packet.syndrome = Syndrome::Rnr;
    packet.rnrTimer = 31;
    EXPECT_TRUE(encode(packet, {output.data(), output.size()}));
    packet.rnrTimer = 32;
    EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
              CodecError::BadSyndrome);

    packet.syndrome = Syndrome::Ack;
    packet.rnrTimer = 0;
    auto bad = encodePacket(packet);
    bad[59] = bad[60] = bad[61] = 0;
    fixIcrc(bad);
    expectError(bad, CodecError::BadQpn);
    bad = encodePacket(packet);
    bad[66] = 0x40;
    fixIcrc(bad);
    expectError(bad, CodecError::BadSyndrome);
}

TEST(PvrdmaRocev1Test, RejectsMissingWirePadding)
{
    const std::array<uint8_t, 3> payload = {1, 2, 3};
    auto bytes = encodePacket(sendPacket(
        Opcode::SendOnly, 7, payload.data(), payload.size()));
    bytes.erase(bytes.end() - IcrcSize - 1);
    detail::put16(bytes.data(), 18, 19);
    bytes[55] = 0;
    fixIcrc(bytes);
    expectError(bytes, CodecError::BadPad);
}

TEST(PvrdmaRocev1Test, ValidatesPayloadLengthsAndOutputBuffer)
{
    std::array<uint8_t, MaxPayloadSize + 1> payload{};
    std::array<uint8_t, SendHeaderSize + MaxPayloadSize + 4 + IcrcSize>
        output{};

    for (const Opcode opcode : {Opcode::SendFirst, Opcode::SendMiddle}) {
        Packet packet = sendPacket(opcode, 1, payload.data(),
                                   MaxPayloadSize - 1);
        EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
                  CodecError::BadPayloadLength);
        packet.payload.size = MaxPayloadSize;
        EXPECT_TRUE(encode(packet, {output.data(), output.size()}));
    }

    Packet last = sendPacket(Opcode::SendLast, 1, payload.data(), 0);
    EXPECT_EQ(encode(last, {output.data(), output.size()}).error,
              CodecError::BadPayloadLength);
    last.payload.size = MaxPayloadSize;
    EXPECT_TRUE(encode(last, {output.data(), output.size()}));
    last.payload.size = MaxPayloadSize + 1;
    EXPECT_EQ(encode(last, {output.data(), output.size()}).error,
              CodecError::PayloadTooLarge);

    Packet ack = controlPacket(Syndrome::Ack);
    ack.payload = {payload.data(), 1};
    EXPECT_EQ(encode(ack, {output.data(), output.size()}).error,
              CodecError::BadPayloadLength);

    const uint8_t byte = 1;
    Packet only = sendPacket(Opcode::SendOnly, 1, &byte, 1);
    const size_t size = encodedSize(only);
    EXPECT_EQ(encode(only, {nullptr, size}).error,
              CodecError::BufferTooSmall);
    EXPECT_EQ(encode(only, {output.data(), size - 1}).error,
              CodecError::BufferTooSmall);

    auto wrongOpcode = encodePacket(only);
    wrongOpcode[54] = static_cast<uint8_t>(Opcode::SendFirst);
    fixIcrc(wrongOpcode);
    expectError(wrongOpcode, CodecError::BadPayloadLength);

    Packet empty = sendPacket(Opcode::SendOnly, 1, nullptr, 0);
    wrongOpcode = encodePacket(empty);
    wrongOpcode[54] = static_cast<uint8_t>(Opcode::SendLast);
    fixIcrc(wrongOpcode);
    expectError(wrongOpcode, CodecError::BadPayloadLength);
}

} // anonymous namespace
} // namespace rocev1
} // namespace pvrdma
} // namespace gem5
