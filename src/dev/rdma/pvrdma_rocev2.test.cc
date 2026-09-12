// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dev/rdma/pvrdma_rocev2.hh"

namespace gem5
{
namespace pvrdma
{
namespace rocev2
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
    packet.sourcePort = udpSourcePort(packet.flowLabel, 0, 0);
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
    packet.sourcePort = udpSourcePort(packet.flowLabel, 0, 0);
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
    EXPECT_EQ(actual.sourcePort, expected.sourcePort);
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
expectGolden(const char *hex, const Packet &packet,
             uint16_t udpChecksum = 0)
{
    const auto golden = fromHex(hex);
    EXPECT_EQ(detail::get16(golden.data(), UdpChecksumOffset), udpChecksum);
    auto encoded = encodePacket(packet);
    EXPECT_EQ(detail::get16(encoded.data(), UdpChecksumOffset), 0);
    detail::put16(encoded.data(), UdpChecksumOffset, udpChecksum);
    EXPECT_EQ(encoded, golden);
    const auto result = decode({golden.data(), golden.size()}, golden.size());
    ASSERT_TRUE(result);
    expectPacket(result.packet, packet);
    if (packet.payload.size) {
        EXPECT_EQ(result.packet.payload.data,
                  golden.data() + SendHeaderSize);
    }
    if (udpChecksum) {
        auto bad = golden;
        bad[UdpChecksumOffset + 1] ^= 1;
        expectError(bad, CodecError::BadUdpChecksum);
    }
}

TEST(PvrdmaRocev2Test, ConstantsAndUdpSourcePort)
{
    EXPECT_EQ(EtherType, 0x86dd);
    EXPECT_EQ(Ipv6NextHeader, 17);
    EXPECT_EQ(RoceUdpDestinationPort, 4791);
    EXPECT_EQ(BthOffset, 62);
    EXPECT_EQ(SendHeaderSize, 74);
    EXPECT_EQ(ControlFrameSize, 82);
    EXPECT_EQ(MaxPayloadSize, 1024);

    EXPECT_EQ(udpSourcePort(0, 0, 0), 0xc000);
    EXPECT_EQ(udpSourcePort(1, 0, 0), 0xc001);
    EXPECT_EQ(udpSourcePort(0x3fff, 0, 0), 0xffff);
    EXPECT_EQ(udpSourcePort(0x4000, 0, 0), 0xc001);
    EXPECT_EQ(udpSourcePort(0xfffff, 0, 0), 0xffc0);
    EXPECT_EQ(udpSourcePort(0, 1, 1), 0xc001);
    EXPECT_EQ(udpSourcePort(0, 1, 0xffffff), 0xffcf);
    EXPECT_EQ(udpSourcePort(0, 0xffffff, 0xffffff), 0xff21);
    EXPECT_EQ(udpSourcePort(0, 0x123456, 0xabcdef), 0xc3c8);
    EXPECT_EQ(udpSourcePort(0, 0x123456, 0xabcdef),
              udpSourcePort(0, 0xabcdef, 0x123456));
}

TEST(PvrdmaRocev2Test, LiteralSendGoldenVectors)
{
    const std::array<uint8_t, 3> onlyPayload = {0xde, 0xad, 0xbe};
    expectGolden(
        "02000000000202000000000186dd62a34567001c1140"
        "20010db800000000000000000000000120010db8000000000000000000000002"
        "c56a12b7001c7f220410ffff0001020380112230deadbe009a3d6dbb",
        sendPacket(Opcode::SendOnly, 0x112230,
                   onlyPayload.data(), onlyPayload.size()),
        0x7f22);

    const std::array<uint8_t, 5> lastPayload = {0xca, 0xfe, 0xba, 0xbe, 1};
    expectGolden(
        "02000000000202000000000186dd62a3456700201140"
        "20010db800000000000000000000000120010db8000000000000000000000002"
        "c56a12b7002000000230ffff0001020380112235cafebabe01000000f87da8b2",
        sendPacket(Opcode::SendLast, 0x112235,
                   lastPayload.data(), lastPayload.size()));
}

TEST(PvrdmaRocev2Test, LiteralControlGoldenVectors)
{
    struct Vector
    {
        const char *hex;
        Syndrome syndrome;
        uint8_t timer;
        uint32_t psn;
    };
    const Vector vectors[] = {
        {"02000000000102000000000286dd62a34567001c1140"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "c56a12b7001c00001100ffff000a0b0c001122351f4455665fa51a5f",
         Syndrome::Ack, 0, 0x112235},
        {"02000000000102000000000286dd62a34567001c1140"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "c56a12b7001c00001100ffff000a0b0c00112233394455661da00355",
         Syndrome::Rnr, 25, 0x112233},
        {"02000000000102000000000286dd62a34567001c1140"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "c56a12b7001c00001100ffff000a0b0c0011223460445566253c2651",
         Syndrome::SequenceNak, 0, 0x112234},
        {"02000000000102000000000286dd62a34567001c1140"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "c56a12b7001c00001100ffff000a0b0c0011223561445566f072fad4",
         Syndrome::InvalidRequestNak, 0, 0x112235},
        {"02000000000102000000000286dd62a34567001c1140"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "c56a12b7001c00001100ffff000a0b0c00112235624455661edd4fc6",
         Syndrome::RemoteAccessNak, 0, 0x112235},
        {"02000000000102000000000286dd62a34567001c1140"
         "20010db800000000000000000000000220010db8000000000000000000000001"
         "c56a12b7001c00001100ffff000a0b0c00112235634455667bbaf37e",
         Syndrome::RemoteOperationNak, 0, 0x112235},
    };
    for (const auto &vector : vectors) {
        Packet packet = controlPacket(vector.syndrome, vector.psn);
        packet.rnrTimer = vector.timer;
        expectGolden(vector.hex, packet);
    }
}

TEST(PvrdmaRocev2Test, SendShapesAndPayloadBounds)
{
    std::array<uint8_t, MaxPayloadSize> payload{};
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = i;

    for (const size_t size : {size_t{0}, size_t{1}, MaxPayloadSize}) {
        const Packet packet = sendPacket(Opcode::SendOnly, size,
                                         payload.data(), size);
        const auto bytes = encodePacket(packet);
        const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
        ASSERT_TRUE(result) << size;
        expectPacket(result.packet, packet);
    }

    for (const Opcode opcode : {Opcode::SendFirst, Opcode::SendMiddle}) {
        const Packet packet = sendPacket(opcode, 1, payload.data(),
                                         payload.size());
        const auto bytes = encodePacket(packet);
        const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
        ASSERT_TRUE(result) << static_cast<unsigned>(opcode);
        expectPacket(result.packet, packet);
    }
    for (const size_t size : {size_t{1}, MaxPayloadSize}) {
        const Packet packet = sendPacket(Opcode::SendLast, 2,
                                         payload.data(), size);
        const auto bytes = encodePacket(packet);
        const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
        ASSERT_TRUE(result) << size;
        expectPacket(result.packet, packet);
    }
}

TEST(PvrdmaRocev2Test, AckCreditsRnrTimersAndErrorSyndromes)
{
    for (uint8_t credit = 0; credit <= 31; ++credit) {
        Packet packet = controlPacket(Syndrome::Ack);
        packet.ackCredit = credit;
        const auto bytes = encodePacket(packet);
        EXPECT_EQ(bytes[AethOffset], credit);
        const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
        ASSERT_TRUE(result) << unsigned{credit};
        EXPECT_EQ(result.packet.syndrome, Syndrome::Ack);
        EXPECT_EQ(result.packet.ackCredit, credit);
    }
    for (uint8_t timer = 0; timer <= 31; ++timer) {
        Packet packet = controlPacket(Syndrome::Rnr);
        packet.rnrTimer = timer;
        const auto bytes = encodePacket(packet);
        EXPECT_EQ(bytes[AethOffset], 0x20 | timer);
        const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
        ASSERT_TRUE(result) << unsigned{timer};
        EXPECT_EQ(result.packet.syndrome, Syndrome::Rnr);
        EXPECT_EQ(result.packet.rnrTimer, timer);
    }
    for (const Syndrome syndrome : {
            Syndrome::SequenceNak, Syndrome::InvalidRequestNak,
            Syndrome::RemoteAccessNak, Syndrome::RemoteOperationNak}) {
        const auto bytes = encodePacket(controlPacket(syndrome));
        const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
        ASSERT_TRUE(result) << static_cast<unsigned>(syndrome);
        EXPECT_EQ(result.packet.syndrome, syndrome);
    }
}

TEST(PvrdmaRocev2Test, RejectsIpv6AndUdpFields)
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
        {EtherTypeOffset, 0, CodecError::BadEtherType},
        {Ipv6Offset, 0x52, CodecError::BadIpv6Version},
        {Ipv6NextHeaderOffset, 6, CodecError::BadIpv6NextHeader},
        {UdpSourcePortOffset, 0x80, CodecError::BadUdpSourcePort},
        {UdpDestinationPortOffset + 1, 0, CodecError::BadUdpDestinationPort},
    };
    for (const auto &mutation : mutations) {
        auto bad = good;
        bad[mutation.offset] = mutation.value;
        fixIcrc(bad);
        expectError(bad, mutation.error);
    }

    auto badIpv6Length = good;
    detail::put16(badIpv6Length.data(), Ipv6PayloadLengthOffset,
                  UdpHeaderSize + BthSize + IcrcSize - 1);
    expectError(badIpv6Length, CodecError::BadIpv6Length);

    auto longIpv6 = good;
    detail::put16(longIpv6.data(), Ipv6PayloadLengthOffset,
                  detail::get16(good.data(), Ipv6PayloadLengthOffset) + 1);
    expectError(longIpv6, CodecError::Truncated);

    auto badUdpLength = good;
    detail::put16(badUdpLength.data(), UdpLengthOffset,
                  detail::get16(good.data(), UdpLengthOffset) - 1);
    fixIcrc(badUdpLength);
    expectError(badUdpLength, CodecError::BadUdpLength);
}

TEST(PvrdmaRocev2Test, RejectsBthAethAndPadFields)
{
    const std::array<uint8_t, 3> payload = {1, 2, 3};
    const auto good = encodePacket(sendPacket(
        Opcode::SendOnly, 7, payload.data(), payload.size()));
    const struct {
        size_t offset;
        uint8_t value;
        CodecError error;
    } mutations[] = {
        {BthOffset, 0xff, CodecError::BadOpcode},
        {BthFlagsOffset, 0x11, CodecError::BadBthVersion},
        {BthFlagsOffset, 0x50, CodecError::BadReserved},
        {BthQpnOffset, 1, CodecError::BadReserved},
        {BthAckOffset, 0x81, CodecError::BadReserved},
        {BthPKeyOffset, 0, CodecError::BadPKey},
    };
    for (const auto &mutation : mutations) {
        auto bad = good;
        bad[mutation.offset] = mutation.value;
        fixIcrc(bad);
        expectError(bad, mutation.error);
    }

    auto badQpn = good;
    std::fill_n(badQpn.begin() + BthQpnOffset + 1, 3, 0);
    fixIcrc(badQpn);
    expectError(badQpn, CodecError::BadQpn);
    std::fill_n(badQpn.begin() + BthQpnOffset + 1, 3, 0xff);
    fixIcrc(badQpn);
    expectError(badQpn, CodecError::BadQpn);

    auto badPad = good;
    badPad[SendHeaderSize + payload.size()] = 1;
    fixIcrc(badPad);
    expectError(badPad, CodecError::BadPad);

    auto control = encodePacket(controlPacket(Syndrome::Ack));
    control[BthFlagsOffset] = 0x10;
    fixIcrc(control);
    expectError(control, CodecError::BadPad);

    control = encodePacket(controlPacket(Syndrome::Ack));
    control[BthAckOffset] = 0x80;
    fixIcrc(control);
    expectError(control, CodecError::BadAckRequest);

    control = encodePacket(controlPacket(Syndrome::Ack));
    control[AethOffset] = 0x40;
    fixIcrc(control);
    expectError(control, CodecError::BadSyndrome);
}

TEST(PvrdmaRocev2Test, ValidatesEncodeFieldsAndPayloadLengths)
{
    std::array<uint8_t, MaxPayloadSize + 1> payload{};
    std::array<uint8_t, SendHeaderSize + MaxPayloadSize + 4 + IcrcSize>
        output{};
    Packet packet = controlPacket(Syndrome::Ack);

    packet.sourcePort = MinUdpSourcePort - 1;
    EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
              CodecError::BadUdpSourcePort);
    packet.sourcePort = MinUdpSourcePort;
    packet.flowLabel = FlowLabelMask + 1;
    EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
              CodecError::BadIpv6Version);
    packet.flowLabel = 0;

    for (const uint32_t qpn : {uint32_t{0}, Field24Mask,
                               Field24Mask + 1}) {
        packet.destinationQpn = qpn;
        EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
                  CodecError::BadQpn);
    }
    packet.destinationQpn = 1;
    packet.psn = Field24Mask + 1;
    EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
              CodecError::BadPsn);
    packet.psn = 0;
    packet.msn = Field24Mask + 1;
    EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
              CodecError::BadMsn);
    packet.msn = 0;
    packet.ackCredit = 32;
    EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
              CodecError::BadSyndrome);
    packet.ackCredit = 31;
    packet.syndrome = Syndrome::Rnr;
    packet.rnrTimer = 32;
    EXPECT_EQ(encode(packet, {output.data(), output.size()}).error,
              CodecError::BadSyndrome);

    for (const Opcode opcode : {Opcode::SendFirst, Opcode::SendMiddle}) {
        Packet send = sendPacket(opcode, 1, payload.data(),
                                 MaxPayloadSize - 1);
        EXPECT_EQ(encode(send, {output.data(), output.size()}).error,
                  CodecError::BadPayloadLength);
    }
    Packet last = sendPacket(Opcode::SendLast, 1, payload.data(), 0);
    EXPECT_EQ(encode(last, {output.data(), output.size()}).error,
              CodecError::BadPayloadLength);
    last.payload.size = MaxPayloadSize + 1;
    EXPECT_EQ(encode(last, {output.data(), output.size()}).error,
              CodecError::PayloadTooLarge);
    Packet ack = controlPacket(Syndrome::Ack);
    ack.payload = {payload.data(), 1};
    EXPECT_EQ(encode(ack, {output.data(), output.size()}).error,
              CodecError::BadPayloadLength);
}

TEST(PvrdmaRocev2Test, RejectsEveryTruncationAndExtraByte)
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

    for (const auto &frame : frames) {
        auto extra = frame;
        extra.push_back(0);
        EXPECT_EQ(decode({extra.data(), extra.size()}, extra.size()).error,
                  CodecError::ExtraBytes);
        EXPECT_TRUE(decode({extra.data(), extra.size()}, frame.size()));
    }
    const auto &frame = frames.front();
    EXPECT_EQ(decode({frame.data(), frame.size()}, frame.size() + 1).error,
              CodecError::Truncated);
    EXPECT_EQ(decode({nullptr, frame.size()}, frame.size()).error,
              CodecError::Truncated);
}

TEST(PvrdmaRocev2Test, IcrcMasksIpv6AndBthInvariantFields)
{
    const uint8_t payload = 1;
    Packet packet = sendPacket(Opcode::SendOnly, 7, &payload, 1);
    packet.hopLimit = 0;
    auto bytes = encodePacket(packet);
    bytes[Ipv6Offset] = 0x6f;
    bytes[Ipv6Offset + 1] = 0xff;
    bytes[Ipv6Offset + 2] = 0xff;
    bytes[Ipv6Offset + 3] = 0xff;
    bytes[BthQpnOffset] = 0xc0;
    const auto result = decode({bytes.data(), bytes.size()}, bytes.size());
    ASSERT_TRUE(result);
    EXPECT_EQ(result.packet.trafficClass, 0xff);
    EXPECT_EQ(result.packet.flowLabel, FlowLabelMask);
    EXPECT_EQ(result.packet.hopLimit, 0);
}

TEST(PvrdmaRocev2Test, RejectsIcrcCorruption)
{
    const std::array<uint8_t, 3> payload = {1, 2, 3};
    const auto good = encodePacket(sendPacket(
        Opcode::SendOnly, 7, payload.data(), payload.size()));
    for (const size_t offset : {Ipv6SourceOffset, SendHeaderSize,
                                good.size() - 1}) {
        auto bad = good;
        bad[offset] ^= 1;
        expectError(bad, CodecError::BadIcrc);
    }
}

TEST(PvrdmaRocev2Test, OutputBufferBounds)
{
    const uint8_t payload = 1;
    const Packet packet = sendPacket(Opcode::SendOnly, 1, &payload, 1);
    const size_t size = encodedSize(packet);
    std::vector<uint8_t> output(size + 1, 0xa5);

    EXPECT_EQ(encode(packet, {nullptr, size}).error,
              CodecError::BufferTooSmall);
    EXPECT_EQ(encode(packet, {output.data(), size - 1}).error,
              CodecError::BufferTooSmall);
    EXPECT_EQ(output, std::vector<uint8_t>(size + 1, 0xa5));

    const auto result = encode(packet, {output.data(), size});
    ASSERT_TRUE(result);
    EXPECT_EQ(result.size, size);
    EXPECT_EQ(output[size], 0xa5);
}

} // anonymous namespace
} // namespace rocev2
} // namespace pvrdma
} // namespace gem5
