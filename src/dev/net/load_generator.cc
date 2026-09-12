#include "dev/net/load_generator.hh"

#include <inttypes.h>
#include <netinet/in.h>

#include <algorithm>
#include <cstdint>
#include <limits>

#include "base/trace.hh"
#include "debug/LoadgenDebug.hh"
#include "debug/LoadgenLatency.hh"
#include "dev/net/load_generator_mac.hh"
#include "sim/core.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
    static uint64_t maxPacketRate()
    {
        return sim_clock::as_int::s;
    }
    LoadGenerator::LoadGeneratorStats::LoadGeneratorStats(statistics::Group *parent)
        : statistics::Group(parent, "LoadGenerator"),
        ADD_STAT(sentPackets, statistics::units::Count::get(), "Number of Generated Packets"),
        ADD_STAT(recvPackets, statistics::units::Count::get(), "Number of Recieved Packets"),
        ADD_STAT(latency, statistics::units::Second::get(),
                 "Distribution of latency in seconds")
        {
            sentPackets.precision(0);
            recvPackets.precision(0);
            latency.init(100);
        }


    LoadGenerator::LoadGenerator(const LoadGeneratorParams &p) : SimObject(p),
    loadgenId(p.loadgen_id), packetSize(0), packetRate(0),
    startTick(p.start_tick), stopTick(p.stop_tick),
    checkLossInterval(p.check_loss_interval), checkLossWait(p.check_loss_wait),
    incrementInterval(0),
    burstWidth(p.burst_width), burstGap(p.burst_gap), burstStartTick(0),
    lastRxCount(0), lastTxCount(0),
    restoredFromCheckpoint(false),
    draining(false), drainSendEvent(false), drainLossEvent(false),
    drainSendDelay(0), drainLossDelay(0),
    sendPacketEvent([this]{sendPacket();}, name()), checkLossEvent([this]{checkLoss();}, name()), loadGeneratorStats(this)
    {
        fatal_if(p.loadgen_id > 254,
                 "loadgen_id must be in [0, 254]");
        fatal_if(p.packet_size < MACHeaderSize + sizeof(uint64_t) ||
                 p.packet_size > std::numeric_limits<uint16_t>::max(),
                 "packet_size must be in [%u, %u] bytes",
                 MACHeaderSize + sizeof(uint64_t),
                 std::numeric_limits<uint16_t>::max());
        fatal_if(p.check_loss_interval == 0,
                 "check_loss_interval must be positive");
        fatal_if(p.check_loss_wait == 0,
                 "check_loss_wait must be positive");
        fatal_if(p.packet_rate == 0 || p.packet_rate > maxPacketRate(),
                 "packet_rate must be in [1, %llu]",
                 (unsigned long long)maxPacketRate());
        fatal_if(startTick > stopTick,
                 "load generator start_tick must not exceed stop_tick");
        packetSize = p.packet_size;
        packetRate = p.packet_rate;
        const uint64_t packetBits = packetSize * 8;
        incrementInterval = (sim_clock::as_int::s / 2) / packetBits;
        if (incrementInterval == 0)
            incrementInterval = 1;
        if (p.mode == "Static")
            loadgenMode = Mode::Static;
        else if (p.mode == "Increment")
            loadgenMode = Mode::Increment;
        else if (p.mode == "Burst")
            loadgenMode = Mode::Burst;
        else
            fatal("Unknown load generator mode '%s'", p.mode);

        LoadGenerator::interface = new LoadGenInt("interface", this);
    }

    Tick LoadGenerator::frequency()
    {
        const Tick period = sim_clock::as_int::s / packetRate;
        fatal_if(period == 0, "packet_rate produces a sub-tick period");
        return period;
    }

    bool LoadGenerator::scheduleSendAfter(Tick delay)
    {
        if (curTick() > stopTick || delay > stopTick - curTick())
            return false;
        schedule(sendPacketEvent, curTick() + delay);
        return true;
    }

    bool LoadGenerator::scheduleLossAfter(Tick delay)
    {
        if (curTick() > stopTick || delay > stopTick - curTick())
            return false;
        schedule(checkLossEvent, curTick() + delay);
        return true;
    }

    void LoadGenerator::startup()
    {
        if (restoredFromCheckpoint)
            return;

        const Tick first = std::max(curTick(), startTick);
        if (first < stopTick) {
            if (loadgenMode == Mode::Burst)
                burstStartTick = first + 1;
            schedule(sendPacketEvent, first + 1);
        }
    }

    Port & LoadGenerator::getPort(const std::string &if_name, PortID idx)
    {
        return *interface;
    }

    void LoadGenerator::buildPacket(EthPacketPtr ethpacket)
    {
        // Build Packet header
        // DSTMAC 6 | SRCMAC 6 | LENGTH 2 | DATA
        const auto dst_mac = loadGeneratorNicMac(loadgenId);
        const uint8_t src_mac[6] = {0x00, 0x80, 0x00, 0x00, 0x00,
            static_cast<uint8_t>(0x01 + loadgenId)};

        uint16_t size = ethpacket->length;

        if(1 != htons(1)) size = htons(size);

        uint8_t head[MACHeaderSize];
        memcpy(head, dst_mac.data(), dst_mac.size());
        memcpy(head + 6, src_mac, 6);
        memcpy(head + 12, &size, 2);
        memcpy(ethpacket->data, head, MACHeaderSize);
        uint64_t timeStamp = gem5::curTick();
        memcpy(&(ethpacket->data[MACHeaderSize]), &timeStamp, sizeof(uint64_t));
    }

    void LoadGenerator::sendPacket()
    {
        if (curTick() > stopTick)
            return;

        EthPacketPtr txPacket = std::make_shared<EthPacketData>(packetSize);
        txPacket->length = packetSize;
        txPacket->simLength = packetSize;
        buildPacket(txPacket);
        if (interface->sendPacket(txPacket)) {
            loadGeneratorStats.sentPackets++;
            lastTxCount++;
        }

        if (curTick() < stopTick)
        {
            if (loadgenMode == Mode::Increment)
            {
                if (lastTxCount == checkLossInterval)
                    // allow enough time for any in flight packets to be recieved
                    scheduleLossAfter(checkLossWait);
                else
                    scheduleSendAfter(frequency());
            } else if (loadgenMode == Mode::Static)
            {
                scheduleSendAfter(frequency());
            } else if (loadgenMode == Mode::Burst)
            {
                if (curTick() - burstStartTick > burstWidth)
                    {
                        if (!scheduleSendAfter(burstGap))
                            return;
                        burstStartTick = curTick() + burstGap;
                        DPRINTF(LoadgenDebug, "Burst Ended, next Burst Starts "
                                "at %llu \n",
                                (unsigned long long)burstStartTick);
                    }
                else
                    scheduleSendAfter(frequency());
            }
        }
    }

    void LoadGenerator::checkLoss()
    {
        if (curTick() > stopTick)
            return;

        const uint64_t loss = lastTxCount > lastRxCount ?
            lastTxCount - lastRxCount : 0;
        if (loss < 10)
        {
            if (incrementInterval > maxPacketRate() - packetRate)
                packetRate = maxPacketRate();
            else
                packetRate += incrementInterval;
            scheduleSendAfter(frequency());
            DPRINTF(LoadgenDebug, "Rate Incremented, now sending packets at "
                    "%llu \n", (unsigned long long)packetRate);
            DPRINTF(LoadgenDebug, "Rx %llu, Tx %llu \n",
                    (unsigned long long)lastRxCount,
                    (unsigned long long)lastTxCount);
        }
        else
        {
            if (incrementInterval >= packetRate) {
                warn("Adaptive load generator reached its minimum rate");
                packetRate = 1;
            } else {
                packetRate -= incrementInterval;
            }

            // add extra delay to prevent previouse loss from affecting results
            const Tick period = frequency();
            if (period <= stopTick - curTick() &&
                checkLossWait <= stopTick - curTick() - period)
                scheduleSendAfter(period + checkLossWait);
            DPRINTF(LoadgenDebug, "Loss Detected, now sending packets at "
                    "%llu \n", (unsigned long long)packetRate);
            DPRINTF(LoadgenDebug, "Rx %llu, Tx %llu \n",
                    (unsigned long long)lastRxCount,
                    (unsigned long long)lastTxCount);
        }
            lastTxCount = 0;
            lastRxCount = 0;
    }

    void LoadGenerator::endTest()
    {
        exitSimLoop("m5_exit by loadgen End Simulator.", 0, curTick(), 0, true);
    }

    bool LoadGenerator::processRxPkt(EthPacketPtr pkt)
    {
        if (pkt->length < MACHeaderSize + sizeof(uint64_t)) {
            warn("Ignoring short load-generator response (%u bytes)",
                 pkt->length);
            return true;
        }

        const uint8_t expectedDst[6] = {0x00, 0x80, 0x00, 0x00, 0x00,
            static_cast<uint8_t>(0x01 + loadgenId)};
        const auto expectedSrc = loadGeneratorNicMac(loadgenId);
        if (memcmp(pkt->data, expectedDst, sizeof(expectedDst)) != 0 ||
            memcmp(pkt->data + sizeof(expectedDst), expectedSrc.data(),
                   expectedSrc.size()) != 0) {
            warn("Ignoring response for another load-generator endpoint");
            return true;
        }

        uint64_t sendTick;
        memcpy(&sendTick, &(pkt->data[MACHeaderSize]), sizeof(uint64_t));
        if (sendTick > curTick()) {
            warn("Ignoring load-generator response with a future timestamp");
            return true;
        }

        loadGeneratorStats.recvPackets++;
        lastRxCount++;

        const double delta = double(gem5::curTick() - sendTick) /
                             sim_clock::as_float::s;
        loadGeneratorStats.latency.sample(delta);
        DPRINTF(LoadgenLatency, "Latency %f \n", delta);
        return true;
    }

    void
    LoadGenerator::serialize(CheckpointOut &cp) const
    {
        const unsigned checkpointVersion = 1;
        SERIALIZE_SCALAR(checkpointVersion);
        SERIALIZE_SCALAR(packetRate);
        SERIALIZE_SCALAR(burstStartTick);
        SERIALIZE_SCALAR(lastRxCount);
        SERIALIZE_SCALAR(lastTxCount);

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
    }

    void
    LoadGenerator::unserialize(CheckpointIn &cp)
    {
        unsigned checkpointVersion;
        UNSERIALIZE_SCALAR(checkpointVersion);
        fatal_if(checkpointVersion != 1,
                 "Unsupported LoadGenerator checkpoint version %u",
                 checkpointVersion);
        uint64_t checkpointPacketRate;
        Tick checkpointBurstStartTick;
        uint64_t checkpointLastRxCount;
        uint64_t checkpointLastTxCount;
        paramIn(cp, "packetRate", checkpointPacketRate);
        paramIn(cp, "burstStartTick", checkpointBurstStartTick);
        paramIn(cp, "lastRxCount", checkpointLastRxCount);
        paramIn(cp, "lastTxCount", checkpointLastTxCount);

        bool sendEventScheduled;
        UNSERIALIZE_SCALAR(sendEventScheduled);
        if (sendEventScheduled) {
            Tick sendEventTick;
            UNSERIALIZE_SCALAR(sendEventTick);
            fatal_if(sendEventTick < curTick(),
                     "LoadGenerator send event is in the past");
            schedule(sendPacketEvent, sendEventTick);
        }

        bool lossEventScheduled;
        UNSERIALIZE_SCALAR(lossEventScheduled);
        if (lossEventScheduled) {
            Tick lossEventTick;
            UNSERIALIZE_SCALAR(lossEventTick);
            fatal_if(lossEventTick < curTick(),
                     "LoadGenerator loss event is in the past");
            schedule(checkLossEvent, lossEventTick);
        }

        // Preserve an in-flight checkpoint schedule exactly. A dormant
        // checkpoint intentionally takes its rate and measurement window from
        // the current configuration so it can be reused across experiments.
        restoredFromCheckpoint = sendEventScheduled || lossEventScheduled;
        if (restoredFromCheckpoint) {
            packetRate = checkpointPacketRate;
            burstStartTick = checkpointBurstStartTick;
            lastRxCount = checkpointLastRxCount;
            lastTxCount = checkpointLastTxCount;
        }
    }

    DrainState
    LoadGenerator::drain()
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
        return DrainState::Drained;
    }

    void
    LoadGenerator::drainResume()
    {
        if (!draining)
            return;

        draining = false;
        if (drainSendEvent)
            schedule(sendPacketEvent, curTick() + drainSendDelay);
        if (drainLossEvent)
            schedule(checkLossEvent, curTick() + drainLossDelay);
        drainSendEvent = false;
        drainLossEvent = false;
    }
}
