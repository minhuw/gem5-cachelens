// Directed test only: observe actual split requests at the memory-side port.
#ifndef __SIMBRICKS_DMA_CHECKER_HH__
#define __SIMBRICKS_DMA_CHECKER_HH__
#include "sim/clocked_object.hh"
#include "mem/qport.hh"
#include "params/SimBricksDmaChecker.hh"
namespace gem5::simbricks::pci {
class DmaChecker : public ClockedObject {
    class Port : public QueuedResponsePort {
        RespPacketQueue responses;
        DmaChecker &owner;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        bool recvTimingReq(PacketPtr pkt) override;
        AddrRangeList getAddrRanges() const override;
      public:
        Port(DmaChecker &o) : QueuedResponsePort(o.name() + ".port", responses),
            responses(o, *this), owner(o) {}
    } port;
    unsigned chunks = 0, descChunks = 0, payloadChunks = 0, controlChunks = 0;
    uint64_t bytes = 0;
    void check(PacketPtr pkt);
  public:
    DmaChecker(const SimBricksDmaCheckerParams &p) : ClockedObject(p), port(*this) {}
    gem5::Port &getPort(const std::string &name, PortID idx=InvalidPortID) override;
    void init() override;
};
}
#endif
