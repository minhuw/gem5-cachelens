#include "dev/net/load_generator_pcap.hh"

#include <inttypes.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include <base/stats/types.hh>

#include "base/trace.hh"
#include "debug/LoadgenDebug.hh"
#include "dev/net/load_generator_mac.hh"
#include "sim/core.hh"
#include "sim/sim_exit.hh"

static constexpr unsigned kEtherHeaderSize = 14;
static constexpr unsigned kMinimumEtherFrameSize = 60;
static uint64_t maxPacketRate()
{
  return gem5::sim_clock::as_int::s;
}

namespace gem5 {

namespace
{

uint64_t
captureHash(FILE *file, const std::string &filename)
{
  uint64_t hash = 1469598103934665603ULL;
  std::array<unsigned char, 64 * 1024> buffer;
  size_t count;
  while ((count = fread(buffer.data(), 1, buffer.size(), file)) != 0) {
    for (size_t i = 0; i < count; ++i) {
      hash ^= buffer[i];
      hash *= 1099511628211ULL;
    }
  }
  fatal_if(ferror(file), "Failed to read PCAP file %s", filename);
  clearerr(file);
  fatal_if(fseek(file, 0, SEEK_SET) != 0,
           "Failed to rewind PCAP file %s", filename);
  return hash;
}

} // anonymous namespace

LoadGeneratorPcap::LoadGeneratorPcapStats::LoadGeneratorPcapStats(
    statistics::Group *parent)
    : statistics::Group(parent, "LoadGeneratorPcap"),
      ADD_STAT(sentPackets, statistics::units::Count::get(),
               "Number of Generated Packets"),
      ADD_STAT(recvPackets, statistics::units::Count::get(),
               "Number of Recieved Packets"),
      ADD_STAT(uncorrelatedPackets, statistics::units::Count::get(),
               "Responses without defensible request correlation") {
  sentPackets.precision(0);
  recvPackets.precision(0);
}

LoadGeneratorPcap::LoadGeneratorPcap(const LoadGeneratorPcapParams &p)
    : SimObject(p),
      loadgenId(p.loadgen_id),
      startTick(p.start_tick),
      stopTick(p.stop_tick),
      maxPcktSize(p.max_packetsize),
      portFilter(p.port_filter),
      srcIP(p.replace_src_ip),
      destIP(p.replace_dest_ip),
      packetRate(p.packet_rate),
      incrementInterval(p.increment_interval),
      checkLossInterval(p.check_loss_interval),
      checkLossWait(p.check_loss_wait),
      exitOnEof(p.exit_on_eof),
      eofDrainDelay(p.eof_drain_delay),
      lastRxCount(0),
      lastTxCount(0),
      count_packets(0),
      pcapFilename(p.pcap_filename),
      pcap_h(nullptr),
      captureFileHash(0),
      recordsConsumed(0),
      pendingPacket(nullptr),
      sendInProgress(false),
      restoredFromCheckpoint(false),
      draining(false),
      drainSendEvent(false),
      drainLossEvent(false),
      drainEofEvent(false),
      drainSendDelay(0),
      drainLossDelay(0),
      drainEofDelay(0),
      eofSeen(false),
      eofEvent([this] { finishAtEof(); }, name()),
      sendPacketEvent([this] { sendPacket(); }, name()),
      checkLossEvent([this] { checkLoss(); }, name()),
      loadGeneratorPcapStats(this) {
  fatal_if(p.loadgen_id > 254,
           "loadgen_id must be in [0, 254]");
  fatal_if(p.max_packetsize < kEtherHeaderSize ||
           p.max_packetsize > std::numeric_limits<uint32_t>::max(),
           "max_packetsize (the full L2 frame size without FCS) must be in "
           "[%u, %u]",
           kEtherHeaderSize, std::numeric_limits<uint32_t>::max());
  fatal_if(portFilter == 0, "port_filter must be in [1, 65535]");
  fatal_if(p.check_loss_interval == 0,
           "check_loss_interval must be positive");
  fatal_if(p.check_loss_wait == 0,
           "check_loss_wait must be positive");
  fatal_if(p.packet_rate == 0 || p.packet_rate > maxPacketRate(),
           "packet_rate must be in [1, %llu]",
           (unsigned long long)maxPacketRate());
  fatal_if(incrementInterval == 0, "increment_interval must be positive");
  struct in_addr address;
  fatal_if(inet_pton(AF_INET, srcIP.c_str(), &address) != 1,
           "replace_src_ip must be a valid IPv4 address");
  fatal_if(inet_pton(AF_INET, destIP.c_str(), &address) != 1,
           "replace_dest_ip must be a valid IPv4 address");
  fatal_if(startTick > stopTick,
           "load generator start_tick must not exceed stop_tick");
  LoadGeneratorPcap::interface = new LoadGenPcapInt("interface", this);

  // Setup pcap trace file.
  char errbuff[PCAP_ERRBUF_SIZE];
  FILE *capture = fopen(pcapFilename.c_str(), "rb");
  fatal_if(capture == nullptr, "Failed to open %s pcap trace file",
           pcapFilename);
  captureFileHash = captureHash(capture, pcapFilename);
  pcap_h = pcap_fopen_offline(capture, errbuff);
  if (pcap_h == nullptr) {
    fatal("Failed to open %s pcap trace file, error: %s", pcapFilename.c_str(),
          errbuff);
  } else {
    inform("Pcap trace file is loaded: %s", pcapFilename.c_str());
  }

  // Stack mode.
  if (p.stack_mode == "KernelStack") {
    stackMode = StackMode::Kernel;
    DPRINTF(LoadgenDebug, "Running Pcap load generator in Kernel stack mode\n");
  } else if (p.stack_mode == "DPDKStack") {
    stackMode = StackMode::DPDK;
    DPRINTF(LoadgenDebug, "Running Pcap load generator in DPDK stack mode\n");
  } else
    fatal("Unknown stack mode");

  // Other params.
  if (p.replay_mode == "SimpleReplay") {
    replayMode = ReplayMode::SimpleReplay;
    DPRINTF(LoadgenDebug, "Running Pcap load generator in SimpleReplay mode\n");
  } else if (p.replay_mode == "ReplayAndAdjustThroughput") {
    replayMode = ReplayMode::ReplayAndAdjustThroughput;
    DPRINTF(LoadgenDebug,
            "Running Pcap load generator in ReplayAndAdjustThroughput mode, "
            "base packet rate: %llu pps, interval: %llu\n",
            (unsigned long long)packetRate,
            (unsigned long long)incrementInterval);
  } else if (p.replay_mode == "ConstThroughput") {
    replayMode = ReplayMode::ConstThroughput;
    DPRINTF(LoadgenDebug,
            "Running Pcap load generator in ConstThroughput mode, target "
            "packet rate: %llu pps\n",
            (unsigned long long)packetRate);
  } else
    fatal("Unknown replay mode");
}

LoadGeneratorPcap::~LoadGeneratorPcap() {
  if (pcap_h != nullptr)
    pcap_close(pcap_h);
}
Tick LoadGeneratorPcap::frequency()
    {
        const Tick period = gem5::sim_clock::as_int::s / packetRate;
        fatal_if(period == 0, "packet_rate produces a sub-tick period");
        return period;
    }
bool LoadGeneratorPcap::scheduleSendAfter(Tick delay) {
  if (curTick() > stopTick || delay > stopTick - curTick())
    return false;
  schedule(sendPacketEvent, curTick() + delay);
  return true;
}
bool LoadGeneratorPcap::scheduleLossAfter(Tick delay) {
  if (curTick() > stopTick || delay > stopTick - curTick())
    return false;
  schedule(checkLossEvent, curTick() + delay);
  return true;
}
void LoadGeneratorPcap::startup() {
  if (restoredFromCheckpoint)
    return;

  const Tick first = std::max(curTick(), startTick);
  if (first < stopTick) {
    DPRINTF(LoadgenDebug, "Starting LoadGenPcap\n");
    schedule(sendPacketEvent, first + 1);
  }
}

Port &LoadGeneratorPcap::getPort(const std::string &if_name, PortID idx) {
  return *interface;
}

void
LoadGeneratorPcap::rewriteEthernetAddresses(EthPacketPtr packet) const
{
    assert(packet->length >= kEtherHeaderSize);
    const auto dstMac = loadGeneratorNicMac(loadgenId);
    const uint8_t srcMac[6] = {
        0x00, 0x80, 0x00, 0x00, 0x00,
        static_cast<uint8_t>(0x01 + loadgenId)};

    memcpy(packet->data, dstMac.data(), dstMac.size());
    memcpy(packet->data + dstMac.size(), srcMac, sizeof(srcMac));
}

namespace
{

uint16_t
read16(const uint8_t *data)
{
  uint16_t value;
  memcpy(&value, data, sizeof(value));
  return ntohs(value);
}

void
write16(uint8_t *data, uint16_t value)
{
  value = htons(value);
  memcpy(data, &value, sizeof(value));
}

uint16_t
internetChecksum(const uint8_t *data, size_t length, uint32_t sum = 0)
{
  while (length >= 2) {
    sum += (uint16_t(data[0]) << 8) | data[1];
    data += 2;
    length -= 2;
  }
  if (length)
    sum += uint16_t(data[0]) << 8;
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return uint16_t(~sum);
}

uint16_t
adjustChecksum(uint16_t checksum, const uint8_t *oldData,
               const uint8_t *newData, size_t length)
{
  assert(length % 2 == 0);
  uint32_t sum = uint16_t(~checksum);
  while (length != 0) {
    const uint16_t oldWord = (uint16_t(oldData[0]) << 8) | oldData[1];
    const uint16_t newWord = (uint16_t(newData[0]) << 8) | newData[1];
    sum += uint16_t(~oldWord);
    sum += newWord;
    oldData += 2;
    newData += 2;
    length -= 2;
  }
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return uint16_t(~sum);
}

} // anonymous namespace

bool
LoadGeneratorPcap::rewriteIpv4Packet(EthPacketPtr packet) const
{
    if (packet->length < kEtherHeaderSize)
        return false;
    if (read16(packet->data + 12) != ETHERTYPE_IP)
        return true;
    if (packet->length < kEtherHeaderSize + 20)
        return false;

    uint8_t *ip = packet->data + kEtherHeaderSize;
    const size_t ipHeaderLen = size_t(ip[0] & 0xf) * 4;
    const size_t ipTotalLen = read16(ip + 2);
    if ((ip[0] >> 4) != 4 || ipHeaderLen < 20 ||
        ipTotalLen < ipHeaderLen ||
        ipTotalLen > packet->length - kEtherHeaderSize) {
        return false;
    }

    uint8_t oldAddresses[8];
    memcpy(oldAddresses, ip + 12, sizeof(oldAddresses));

    struct in_addr src;
    struct in_addr dest;
    inet_pton(AF_INET, srcIP.c_str(), &src);
    inet_pton(AF_INET, destIP.c_str(), &dest);
    memcpy(ip + 12, &src.s_addr, sizeof(src.s_addr));
    memcpy(ip + 16, &dest.s_addr, sizeof(dest.s_addr));
    write16(ip + 10, 0);
    write16(ip + 10, internetChecksum(ip, ipHeaderLen));

    const uint8_t protocol = ip[9];
    if (protocol != IPPROTO_UDP && protocol != IPPROTO_TCP)
        return true;

    const uint16_t fragment = read16(ip + 6);
    const size_t fragmentOffset = fragment & 0x1fff;
    const bool moreFragments = fragment & 0x2000;
    if (fragmentOffset != 0) {
        // The transport checksum is carried only by the first fragment. Its
        // pseudo-header is adjusted when that fragment is replayed.
        return true;
    }

    uint8_t *transport = ip + ipHeaderLen;
    const size_t transportLength = ipTotalLen - ipHeaderLen;
    size_t checksumLength = transportLength;
    size_t checksumOffset;
    if (protocol == IPPROTO_UDP) {
        if (transportLength < sizeof(udphdr))
            return false;
        checksumOffset = 6;
        const size_t udpLength = read16(transport + 4);
        if (udpLength < sizeof(udphdr) ||
            (!moreFragments && udpLength > transportLength) ||
            (moreFragments && udpLength <= transportLength)) {
            return false;
        }
        if (!moreFragments)
            checksumLength = udpLength;
    } else {
        constexpr size_t tcpHeaderSize = 20;
        checksumOffset = 16;
        if (moreFragments) {
            // A first fragment only needs to carry the checksum field. The
            // complete TCP header, including declared options, can continue
            // in later fragments.
            if (transportLength < checksumOffset + sizeof(uint16_t))
                return false;
        } else {
            if (transportLength < tcpHeaderSize)
                return false;
            const size_t tcpHeaderLength =
                size_t(transport[12] >> 4) * 4;
            if (tcpHeaderLength < tcpHeaderSize ||
                tcpHeaderLength > transportLength) {
                return false;
            }
        }
    }

    const uint16_t oldChecksum = read16(transport + checksumOffset);
    if (protocol == IPPROTO_UDP && oldChecksum == 0)
        return true;

    uint16_t checksum;
    if (moreFragments) {
        checksum = adjustChecksum(
            oldChecksum, oldAddresses, ip + 12, sizeof(oldAddresses));
    } else {
        write16(transport + checksumOffset, 0);
        uint32_t pseudo = 0;
        for (int offset = 12; offset < 20; offset += 2)
            pseudo += (uint16_t(ip[offset]) << 8) | ip[offset + 1];
        pseudo += protocol;
        pseudo += checksumLength;
        checksum = internetChecksum(transport, checksumLength, pseudo);
    }
    if (protocol == IPPROTO_UDP && checksum == 0)
        checksum = 0xffff;
    write16(transport + checksumOffset, checksum);
    return true;
}

void
LoadGeneratorPcap::finishAtEof()
{
  if (eofSeen)
    exitSimLoop("END OF PCAP TRACE\nSIM TERMINATED BY LOADGEN");
}

void
LoadGeneratorPcap::sendPacket()
{
    if (curTick() > stopTick)
        return;

    DPRINTF(LoadgenDebug, "LoadGenPcap::sendPacket executed\n");
    if (!pendingPacket) {
        // Read a packet only after the previous record has been accepted. This
        // preserves capture order across Ethernet backpressure.
        pcap_pkthdr *pcapHeader;
        const u_char *pcapData;
        if (pcap_h == nullptr) {
            warn("No pcap file loaded, nothing will be scheduled next!");
            return;
        }

        const int ret = pcap_next_ex(pcap_h, &pcapHeader, &pcapData);
        if (ret == -2) {
            DPRINTF(LoadgenDebug,
                    "End of pcap trace is reached; generation stops.\n");
            eofSeen = true;
            if (exitOnEof && !eofEvent.scheduled())
                schedule(eofEvent, curTick() + eofDrainDelay);
            return;
        }
        if (ret == -1) {
            fatal("Failed to read %s pcap trace: %s", pcapFilename.c_str(),
                  pcap_geterr(pcap_h));
        }
        if (ret != 1) {
            warn("Unexpected pcap_next_ex return value %d; retrying", ret);
            scheduleSendAfter(1);
            return;
        }
        recordsConsumed++;

        // The load generator requires complete Ethernet records. Both the
        // capture length and the configured maximum describe full L2 frames,
        // including the Ethernet header and excluding any FCS.
        if (pcapHeader->len != pcapHeader->caplen) {
            DPRINTF(LoadgenDebug,
                    "Truncated pcap record detected, skipping\n");
            scheduleSendAfter(1);
            return;
        }
        if (pcapHeader->len > maxPcktSize) {
            DPRINTF(LoadgenDebug,
                    "Frame exceeds max_packetsize, skipping\n");
            scheduleSendAfter(1);
            return;
        }
        if (pcapHeader->caplen < kEtherHeaderSize) {
            DPRINTF(LoadgenDebug, "Truncated Ethernet frame, skipping\n");
            scheduleSendAfter(1);
            return;
        }

        EthPacketPtr txPacket;
        if (stackMode == StackMode::Kernel) {
            if (read16(pcapData + 12) != ETHERTYPE_IP) {
                DPRINTF(LoadgenDebug,
                        "Non-IPv4 frame in kernel trace, skipping\n");
                scheduleSendAfter(1);
                return;
            }

            const uint8_t *ip = pcapData + kEtherHeaderSize;
            const size_t remaining =
                pcapHeader->caplen - kEtherHeaderSize;
            if (remaining < 20) {
                DPRINTF(LoadgenDebug, "Truncated IPv4 header, skipping\n");
                scheduleSendAfter(1);
                return;
            }

            const uint8_t ipVersion = ip[0] >> 4;
            const size_t ipHeaderLen = size_t(ip[0] & 0xf) * 4;
            const size_t ipTotalLen = read16(ip + 2);
            if (ipVersion != 4 || ipHeaderLen < 20 ||
                ipHeaderLen > remaining || ipTotalLen < ipHeaderLen ||
                ipTotalLen > remaining) {
                DPRINTF(LoadgenDebug, "Malformed IPv4 frame, skipping\n");
                scheduleSendAfter(1);
                return;
            }

            // Kernel-mode filtering needs an unfragmented UDP header.
            if ((read16(ip + 6) & 0x3fff) != 0) {
                DPRINTF(LoadgenDebug, "Fragmented IPv4 frame, skipping\n");
                scheduleSendAfter(1);
                return;
            }
            if (ip[9] != IPPROTO_UDP) {
                DPRINTF(LoadgenDebug, "Non-UDP IPv4 frame, skipping\n");
                scheduleSendAfter(1);
                return;
            }
            const size_t transportLength = ipTotalLen - ipHeaderLen;
            if (transportLength < sizeof(udphdr)) {
                DPRINTF(LoadgenDebug, "Malformed UDP datagram, skipping\n");
                scheduleSendAfter(1);
                return;
            }
            const size_t udpLength = read16(ip + ipHeaderLen + 4);
            if (udpLength < sizeof(udphdr) ||
                udpLength > transportLength) {
                DPRINTF(LoadgenDebug, "Malformed UDP datagram, skipping\n");
                scheduleSendAfter(1);
                return;
            }
            if (read16(ip + ipHeaderLen + 2) != portFilter) {
                DPRINTF(LoadgenDebug, "UDP frame filtered by port\n");
                scheduleSendAfter(1);
                return;
            }

            const size_t replayLength = kEtherHeaderSize + ipTotalLen;
            const bool unpadded = pcapHeader->len == replayLength;
            const bool legallyPadded =
                replayLength < kMinimumEtherFrameSize &&
                pcapHeader->len == kMinimumEtherFrameSize;
            if (!unpadded && !legallyPadded) {
                DPRINTF(LoadgenDebug,
                        "Inconsistent IPv4 frame length, skipping\n");
                scheduleSendAfter(1);
                return;
            }

            // Strip legal Ethernet padding: the kernel-facing replay is the
            // 14-byte Ethernet header followed by exactly the IPv4 packet.
            txPacket = std::make_shared<EthPacketData>(replayLength);
            txPacket->length = replayLength;
            txPacket->simLength = replayLength;
            memcpy(txPacket->data, pcapData, kEtherHeaderSize);
            memcpy(txPacket->data + kEtherHeaderSize, ip, ipTotalLen);
            rewriteEthernetAddresses(txPacket);
            if (!rewriteIpv4Packet(txPacket)) {
                DPRINTF(LoadgenDebug,
                        "Malformed IPv4 frame after filtering, skipping\n");
                scheduleSendAfter(1);
                return;
            }
        } else {
            // DPDK replay preserves the complete original frame. Only endpoint
            // MAC addresses are always rewritten; IP data and checksums are
            // changed only when the original EtherType is IPv4.
            txPacket =
                std::make_shared<EthPacketData>(pcapHeader->len);
            txPacket->length = pcapHeader->len;
            txPacket->simLength = pcapHeader->len;
            memcpy(txPacket->data, pcapData, pcapHeader->len);
            rewriteEthernetAddresses(txPacket);
            if (!rewriteIpv4Packet(txPacket)) {
                DPRINTF(LoadgenDebug,
                        "Malformed IPv4 frame in trace, skipping\n");
                scheduleSendAfter(1);
                return;
            }
        }
        pendingPacket = txPacket;
    }

    // Send packet.
    sendInProgress = true;
    const bool accepted = interface->sendPacket(pendingPacket);
    sendInProgress = false;
    if (!accepted) {
        DPRINTF(LoadgenDebug, "Peer rejected packet\n");
        scheduleSendAfter(frequency());
        return;
    }
    pendingPacket.reset();
    DPRINTF(LoadgenDebug, "Packet was sent!\n");

    loadGeneratorPcapStats.sentPackets++;
    lastTxCount++;

    if (curTick() < stopTick) {
        if (replayMode == ReplayMode::ConstThroughput ||
            replayMode == ReplayMode::SimpleReplay) {
            scheduleSendAfter(frequency());
        } else if (replayMode == ReplayMode::ReplayAndAdjustThroughput) {
            if (lastTxCount == checkLossInterval)
                scheduleLossAfter(checkLossWait);
            else
                scheduleSendAfter(frequency());
        } else {
            warn("Weird replay mode detected, nothing will be scheduled "
                 "next!");
        }
    }
}

void
LoadGeneratorPcap::sendDone()
{
  if (draining || sendInProgress || !pendingPacket || curTick() >= stopTick)
    return;

  // A periodic fallback may already be pending for peers which do not provide
  // completion notification. Prefer retrying promptly once a peer does.
  if (sendPacketEvent.scheduled())
    deschedule(sendPacketEvent);
  scheduleSendAfter(1);
}

void LoadGeneratorPcap::checkLoss() {
  if (curTick() > stopTick)
    return;

  const uint64_t loss = lastTxCount > lastRxCount ?
      lastTxCount - lastRxCount : 0;
  if (loss < 5) {
    // No loss - incrrement packet rate.
    if (incrementInterval > maxPacketRate() - packetRate)
      packetRate = maxPacketRate();
    else
      packetRate += incrementInterval;
    scheduleSendAfter(frequency());
    DPRINTF(LoadgenDebug, "Rate Incremented, now sending packets at %llu \n",
            (unsigned long long)packetRate);
    DPRINTF(LoadgenDebug, "Rx %llu, Tx %llu \n",
            (unsigned long long)lastRxCount,
            (unsigned long long)lastTxCount);
  } else {
    // Loss deteceted - dectement rate.
    if (incrementInterval >= packetRate) {
      warn("Adaptive PCAP load generator reached its minimum rate");
      packetRate = 1;
    } else {
      packetRate -= incrementInterval;
    }


    // add extra delay to prevent previouse loss from affecting results
    const Tick period = frequency();
    if (period <= stopTick - curTick() &&
        checkLossWait <= stopTick - curTick() - period)
      scheduleSendAfter(period + checkLossWait);
    DPRINTF(LoadgenDebug, "Loss Detected, now sending packets at %llu \n ",
            (unsigned long long)packetRate);
    DPRINTF(LoadgenDebug, "Rx %llu, Tx %llu \n",
            (unsigned long long)lastRxCount,
            (unsigned long long)lastTxCount);
    // exitSimLoop("LOSS DETECTED" "SIM TERMINATED BY LOADGEN");
  }

  lastTxCount = 0;
  lastRxCount = 0;
}

void LoadGeneratorPcap::endTest() const {
  exitSimLoop("m5_exit by loadgen End Simulator.", 0, curTick(), 0, true);
}

bool LoadGeneratorPcap::processRxPkt(EthPacketPtr pkt) {
  if (pkt->length < kEtherHeaderSize) {
    warn("Ignoring short PCAP load-generator response (%u bytes)",
         pkt->length);
    return true;
  }
  const uint8_t expectedDst[6] = {
      0x00, 0x80, 0x00, 0x00, 0x00,
      static_cast<uint8_t>(0x01 + loadgenId)};
  const auto expectedSrc = loadGeneratorNicMac(loadgenId);
  if (memcmp(pkt->data, expectedDst, sizeof(expectedDst)) != 0 ||
      memcmp(pkt->data + sizeof(expectedDst), expectedSrc.data(),
             expectedSrc.size()) != 0) {
    warn("Ignoring response for another PCAP load-generator endpoint");
    return true;
  }
  loadGeneratorPcapStats.recvPackets++;
  // Generic PCAP responses carry no request identifier, so they cannot be
  // matched defensibly to accepted sends for latency. Aggregate RX counts do
  // not require ordering and remain valid input to adaptive loss control.
  lastRxCount++;
  loadGeneratorPcapStats.uncorrelatedPackets++;
  return true;
}

void
LoadGeneratorPcap::serialize(CheckpointOut &cp) const
{
  fatal_if(sendInProgress,
           "Cannot checkpoint LoadGeneratorPcap during a send callback");

  const unsigned checkpointVersion = 2;
  SERIALIZE_SCALAR(checkpointVersion);
  SERIALIZE_SCALAR(packetRate);
  SERIALIZE_SCALAR(lastRxCount);
  SERIALIZE_SCALAR(lastTxCount);
  SERIALIZE_SCALAR(recordsConsumed);

  const uint64_t pcapFileHash = captureFileHash;
  SERIALIZE_SCALAR(pcapFileHash);
  SERIALIZE_SCALAR(eofSeen);

  const bool pendingPacketExists = pendingPacket != nullptr;
  SERIALIZE_SCALAR(pendingPacketExists);
  if (pendingPacketExists)
    pendingPacket->serialize("pendingPacket", cp);

  const bool sendEventScheduled = draining ?
      drainSendEvent : sendPacketEvent.scheduled();
  SERIALIZE_SCALAR(sendEventScheduled);
  if (sendEventScheduled) {
    const Tick sendEventTick = draining ?
        curTick() + drainSendDelay : sendPacketEvent.when();
    SERIALIZE_SCALAR(sendEventTick);
  }

  const bool lossEventScheduled = draining ?
      drainLossEvent : checkLossEvent.scheduled();
  SERIALIZE_SCALAR(lossEventScheduled);
  if (lossEventScheduled) {
    const Tick lossEventTick = draining ?
        curTick() + drainLossDelay : checkLossEvent.when();
    SERIALIZE_SCALAR(lossEventTick);
  }

  const bool eofEventScheduled = draining ?
      drainEofEvent : eofEvent.scheduled();
  SERIALIZE_SCALAR(eofEventScheduled);
  if (eofEventScheduled) {
    const Tick eofEventTick = draining ?
        curTick() + drainEofDelay : eofEvent.when();
    SERIALIZE_SCALAR(eofEventTick);
  }
}

void
LoadGeneratorPcap::unserialize(CheckpointIn &cp)
{
    unsigned checkpointVersion;
    UNSERIALIZE_SCALAR(checkpointVersion);
    fatal_if(checkpointVersion != 1 && checkpointVersion != 2,
             "Unsupported LoadGeneratorPcap checkpoint version %u",
             checkpointVersion);

    uint64_t checkpointPacketRate;
    uint64_t checkpointLastRxCount;
    uint64_t checkpointLastTxCount;
    uint64_t checkpointRecordsConsumed;
    paramIn(cp, "packetRate", checkpointPacketRate);
    paramIn(cp, "lastRxCount", checkpointLastRxCount);
    paramIn(cp, "lastTxCount", checkpointLastTxCount);
    paramIn(cp, "recordsConsumed", checkpointRecordsConsumed);

    uint64_t checkpointPcapFileHash;
    paramIn(cp, "pcapFileHash", checkpointPcapFileHash);

    bool checkpointEofSeen = false;
    if (checkpointVersion >= 2)
        paramIn(cp, "eofSeen", checkpointEofSeen);

    bool pendingPacketExists;
    UNSERIALIZE_SCALAR(pendingPacketExists);
    EthPacketPtr checkpointPendingPacket;
    if (pendingPacketExists) {
        checkpointPendingPacket = std::make_shared<EthPacketData>();
        checkpointPendingPacket->unserialize("pendingPacket", cp);
    }

    bool sendEventScheduled;
    UNSERIALIZE_SCALAR(sendEventScheduled);
    Tick sendEventTick = 0;
    if (sendEventScheduled)
        UNSERIALIZE_SCALAR(sendEventTick);

    bool lossEventScheduled;
    UNSERIALIZE_SCALAR(lossEventScheduled);
    Tick lossEventTick = 0;
    if (lossEventScheduled)
        UNSERIALIZE_SCALAR(lossEventTick);

    bool eofEventScheduled = false;
    Tick eofEventTick = 0;
    if (checkpointVersion >= 2) {
        UNSERIALIZE_SCALAR(eofEventScheduled);
        if (eofEventScheduled)
            UNSERIALIZE_SCALAR(eofEventTick);
    }

    // Determine whether this checkpoint contains replay activity before
    // enforcing capture identity. A checkpoint made while the generator is
    // dormant intentionally carries no stream position and can be restored
    // with a different PCAP and a new replay configuration.
    const bool activeCheckpoint = checkpointRecordsConsumed != 0 ||
        checkpointLastRxCount != 0 || checkpointLastTxCount != 0 ||
        checkpointEofSeen || pendingPacketExists || sendEventScheduled ||
        lossEventScheduled || eofEventScheduled;

    if (activeCheckpoint) {
        fatal_if(captureFileHash != checkpointPcapFileHash,
                 "PCAP file %s changed since the checkpoint was created",
                 pcapFilename);
        fatal_if(sendEventScheduled && sendEventTick < curTick(),
                 "LoadGeneratorPcap send event is in the past");
        fatal_if(lossEventScheduled && lossEventTick < curTick(),
                 "LoadGeneratorPcap loss event is in the past");
        fatal_if(eofEventScheduled && eofEventTick < curTick(),
                 "LoadGeneratorPcap EOF event is in the past");
        fatal_if(pcap_h == nullptr,
                 "Cannot restore active PCAP stream without a capture");

        // Restore libpcap's stream before publishing state or scheduling any
        // event. recordsConsumed includes filtered and malformed records, and
        // includes the record stored in pendingPacket.
        for (uint64_t record = 0;
             record < checkpointRecordsConsumed; ++record) {
            pcap_pkthdr *header;
            const u_char *data;
            const int ret = pcap_next_ex(pcap_h, &header, &data);
            fatal_if(ret != 1,
                     "PCAP file %s ended before restored record %llu",
                     pcapFilename, (unsigned long long)record);
        }

        packetRate = checkpointPacketRate;
        lastRxCount = checkpointLastRxCount;
        lastTxCount = checkpointLastTxCount;
        recordsConsumed = checkpointRecordsConsumed;
        pendingPacket = checkpointPendingPacket;
        eofSeen = checkpointEofSeen;
        restoredFromCheckpoint = true;
    } else {
        // Keep the newly configured replay rate/window and the freshly opened
        // capture stream for a dormant restore.
        lastRxCount = 0;
        lastTxCount = 0;
        recordsConsumed = 0;
        pendingPacket.reset();
        eofSeen = false;
        restoredFromCheckpoint = false;
    }
    sendInProgress = false;

    // All checkpoint fields, capture identity, event times, and stream
    // position have now been validated. Only now may events become visible.
    if (activeCheckpoint) {
        if (sendEventScheduled)
            schedule(sendPacketEvent, sendEventTick);
        if (lossEventScheduled)
            schedule(checkLossEvent, lossEventTick);
        if (eofEventScheduled)
            schedule(eofEvent, eofEventTick);
        if (pendingPacketExists && !sendEventScheduled &&
            !lossEventScheduled && !eofEventScheduled) {
            scheduleSendAfter(1);
        }
    }
}

DrainState
LoadGeneratorPcap::drain()
{
  if (draining)
    return DrainState::Drained;

  draining = true;
  drainSendEvent = sendPacketEvent.scheduled();
  if (drainSendEvent) {
    drainSendDelay = sendPacketEvent.when() - curTick();
    deschedule(sendPacketEvent);
  }
  drainLossEvent = checkLossEvent.scheduled();
  if (drainLossEvent) {
    drainLossDelay = checkLossEvent.when() - curTick();
    deschedule(checkLossEvent);
  }
  drainEofEvent = eofEvent.scheduled();
  if (drainEofEvent) {
    drainEofDelay = eofEvent.when() - curTick();
    deschedule(eofEvent);
  }
  return DrainState::Drained;
}

void
LoadGeneratorPcap::drainResume()
{
  if (!draining)
    return;

  draining = false;
  if (drainSendEvent)
    schedule(sendPacketEvent, curTick() + drainSendDelay);
  if (drainLossEvent)
    schedule(checkLossEvent, curTick() + drainLossDelay);
  if (drainEofEvent)
    schedule(eofEvent, curTick() + drainEofDelay);
  drainSendEvent = false;
  drainLossEvent = false;
  drainEofEvent = false;
}
}  // namespace gem5
