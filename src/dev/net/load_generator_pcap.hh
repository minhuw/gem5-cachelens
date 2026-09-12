#ifndef __DEV_NET_LOAD_GENERATOR_PCAP_HH__
#define __DEV_NET_LOAD_GENERATOR_PCAP_HH__

#include <cstdint>
#include <pcap/pcap.h>

#include "base/statistics.hh"
#include "dev/net/etherint.hh"
#include "params/LoadGeneratorPcap.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5 {

class LoadGenPcapInt;

class LoadGeneratorPcap : public SimObject {
  // Configuration modes.
  enum class StackMode { Kernel, DPDK };
  enum class ReplayMode {
    SimpleReplay,
    ReplayAndAdjustThroughput,
    ConstThroughput
  };

 private:
  // Unique ID of this module to be used as a part of the "device"'s MAC
  // address.
  const uint8_t loadgenId;

  // General configs.
  StackMode stackMode;
  LoadGenPcapInt *interface;
  const Tick startTick;
  const Tick stopTick;
  const uint64_t maxPcktSize;
  const uint16_t portFilter;
  const std::string srcIP, destIP;
  ReplayMode replayMode;
  uint64_t packetRate;
  uint64_t incrementInterval;
  const uint64_t checkLossInterval;
  const Tick checkLossWait;
  const bool exitOnEof;
  const Tick eofDrainDelay;

  // Stats for checking the loss.
  uint64_t lastRxCount;
  uint64_t lastTxCount;

  int count_packets;

  // Pcap related configuration.
  std::string pcapFilename;
  pcap_t *pcap_h;
  uint64_t captureFileHash;
  uint64_t recordsConsumed;
  EthPacketPtr pendingPacket;
  bool sendInProgress;
  bool restoredFromCheckpoint;
  bool draining;
  bool drainSendEvent;
  bool drainLossEvent;
  bool drainEofEvent;
  Tick drainSendDelay;
  Tick drainLossDelay;
  Tick drainEofDelay;
  bool eofSeen;
  EventFunctionWrapper eofEvent;

  // Scheduling events.
  EventFunctionWrapper sendPacketEvent;
  EventFunctionWrapper checkLossEvent;

  struct LoadGeneratorPcapStats : public statistics::Group {
    LoadGeneratorPcapStats(statistics::Group *parent);
    statistics::Scalar sentPackets;
    statistics::Scalar recvPackets;
    statistics::Scalar uncorrelatedPackets;
  } loadGeneratorPcapStats;

  // Scheduling event callbacks.
  void sendPacket();
  void checkLoss();
  Tick frequency();
  bool scheduleSendAfter(Tick delay);
  bool scheduleLossAfter(Tick delay);
  void endTest() const;
  void finishAtEof();
  bool rewriteIpv4Packet(EthPacketPtr packet) const;

  // Rewrite the endpoint addresses while preserving the frame protocol.
  void rewriteEthernetAddresses(EthPacketPtr packet) const;


 public:
  LoadGeneratorPcap(const LoadGeneratorPcapParams &p);
  ~LoadGeneratorPcap();

  Port &getPort(const std::string &if_name, PortID idx);
  void startup();
  void sendDone();

  bool processRxPkt(EthPacketPtr pkt);

  DrainState drain() override;
  void drainResume() override;
  void serialize(CheckpointOut &cp) const override;
  void unserialize(CheckpointIn &cp) override;
};

class LoadGenPcapInt : public EtherInt {
 private:
  LoadGeneratorPcap *dev;

 public:
  LoadGenPcapInt(const std::string &name, LoadGeneratorPcap *d)
      : EtherInt(name), dev(d) {}

  virtual bool recvPacket(EthPacketPtr pkt) { return dev->processRxPkt(pkt); }
  virtual void sendDone() { dev->sendDone(); }
};

}  // namespace gem5

#endif  // __DEV_NET_LOAD_GENERATOR_PCAP_HH__
