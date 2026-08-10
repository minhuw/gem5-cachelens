#ifndef __LOAD_GENERATOR_HH__
#define __LOAD_GENERATOR_HH__

#include <cstdint>

#include "params/LoadGenerator.hh"
#include "dev/net/etherint.hh"
#include "sim/sim_object.hh"
#include "base/statistics.hh"
#include "sim/eventq.hh"

namespace gem5
{
    class LoadGenInt;

    class LoadGenerator : public SimObject
    {

        enum class Mode { Static, Increment, Burst};

        private:

            static constexpr unsigned MACHeaderSize = 14;
            Mode loadgenMode;
            LoadGenInt *interface;
            void sendPacket();
            void checkLoss();
            Tick frequency();
            bool scheduleSendAfter(Tick delay);
            bool scheduleLossAfter(Tick delay);
            void endTest();
            const uint8_t loadgenId;
            uint64_t packetSize;
            uint64_t packetRate;
            const Tick startTick;
            const Tick stopTick;
            const uint64_t checkLossInterval;
            const Tick checkLossWait;
            uint64_t incrementInterval;
            Tick burstWidth;
            Tick burstGap;
            Tick burstStartTick;
            uint64_t lastRxCount;
            uint64_t lastTxCount;
            bool restoredFromCheckpoint;
            bool draining;
            bool drainSendEvent;
            bool drainLossEvent;
            Tick drainSendDelay;
            Tick drainLossDelay;
            EventFunctionWrapper sendPacketEvent;
            EventFunctionWrapper checkLossEvent;
            struct LoadGeneratorStats : public statistics::Group
            {
                LoadGeneratorStats(statistics::Group *parent);
                statistics::Scalar sentPackets;
                statistics::Scalar recvPackets;
                statistics::Histogram latency;
            } loadGeneratorStats;

        public:
            LoadGenerator(const LoadGeneratorParams &p);
            Port & getPort(const std::string &if_name, PortID idx);
            void buildPacket(EthPacketPtr ethpacket);
            void startup();
            bool processRxPkt(EthPacketPtr pkt);
            DrainState drain() override;
            void drainResume() override;
            void serialize(CheckpointOut &cp) const override;
            void unserialize(CheckpointIn &cp) override;
    };

    class LoadGenInt : public EtherInt
    {
        private:
            LoadGenerator* dev;

        public:
            LoadGenInt(const std::string &name, LoadGenerator *d)
                : EtherInt(name), dev(d)
            { }

            virtual bool recvPacket(EthPacketPtr pkt) { return dev->processRxPkt(pkt); }
            virtual void sendDone() { return; }
    };
}
#endif // __LOAD_GENERATOR_HH__
