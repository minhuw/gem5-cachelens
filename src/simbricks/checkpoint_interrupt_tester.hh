// SPDX-License-Identifier: MIT
#ifndef __SIMBRICKS_CHECKPOINT_INTERRUPT_TESTER_HH__
#define __SIMBRICKS_CHECKPOINT_INTERRUPT_TESTER_HH__
#include "dev/x86/i8254.hh"
#include "dev/x86/i82094aa.hh"
#include "mem/tport.hh"
#include "params/SimBricksCheckpointInterruptTester.hh"
#include "sim/system.hh"

namespace gem5::simbricks {
// Only the endpoint is a fixture: PIT, IOAPIC, IntRequestPort and PacketQueue
// are production implementations. No thread or CPU is resumed for the test.
class CheckpointInterruptTester : public SimObject
{
    unsigned delivered = 0;
    class Sink : public SimpleTimingPort {
        CheckpointInterruptTester &owner;
      public:
        explicit Sink(CheckpointInterruptTester &o)
          : SimpleTimingPort(o.name() + ".interrupt", &o), owner(o) {}
        AddrRangeList getAddrRanges() const override { return {AddrRange(0, MaxAddr)}; }
        Tick recvAtomic(PacketPtr pkt) override {
            fatal_if(!pkt->isWrite(), "Expected an IOAPIC interrupt write");
            ++owner.delivered;
            pkt->makeAtomicResponse();
            return 1000;
        }
    } sink;
    EventFunctionWrapper proof;

    void observe() {
        fatal_if(!DrainManager::instance().isDrained() || !params().pit->outputHigh(0),
                 "PIT must expire during the globally drained barrier");
        inform("PIT_BARRIER_FIRED tick=%llu delivered=%u", curTick(), delivered);
    }

  public:
    PARAMS(SimBricksCheckpointInterruptTester);
    explicit CheckpointInterruptTester(const Params &p)
      : SimObject(p), sink(*this), proof([this]{ observe(); }, name() + ".proof") {}
    Port &getPort(const std::string &name, PortID id=InvalidPortID) override {
        if (name == "interrupt") return sink;
        return SimObject::getPort(name, id);
    }
    void armPit() {
        fatal_if(!params().system->isTimingMode(), "PIT probe requires timing mode");
        params().ioapic->writeReg(0x10, 0x41); // pin 0: unmasked fixed vector
        params().ioapic->writeReg(0x11, 0);    // physical destination 0
        params().pit->writeControl(0x30);    // counter 0, LSB/MSB, mode 0
        params().pit->writeCounter(0, 1);
        params().pit->writeCounter(0, 0);     // expire after ~838 ns
        schedule(proof, curTick() + 900000); // final 1-us barrier step
        inform("PIT_BARRIER_ARMED tick=%llu", curTick());
    }
    void memWriteback() override {
        if (!params().inject_writeback) return;
        fatal_if(!DrainManager::instance().isDrained(), "Writeback must stay drained");
        params().ioapic->writeReg(0x10, 0x41);
        params().ioapic->writeReg(0x11, 0);
        params().ioapic->raiseInterruptPin(0);
        params().ioapic->lowerInterruptPin(0);
        inform("WRITEBACK_INTERRUPT_QUEUED tick=%llu", curTick());
    }
};
}
#endif
