// SPDX-License-Identifier: MIT
// USE_TEST_OBJECTS-only real-peer MMIO fixture. No synthetic NIC state injection.
#ifndef __SIMBRICKS_CHECKPOINT_TESTER_HH__
#define __SIMBRICKS_CHECKPOINT_TESTER_HH__
#include "simbricks/pci.hh"
#include "dev/pci/config_write.hh"
#include "mem/packet_access.hh"
#include "params/SimBricksCheckpointTester.hh"
#include "sim/sim_exit.hh"

namespace gem5::simbricks::pci {
class CheckpointTester : public Device
{
    static constexpr Addr RegBase = 0x10000000;
    static constexpr Addr RssKey = 0x00244800; // I40E_PFQF_HKEY(0)
    static constexpr uint32_t Value = 0x1234abcd;
    unsigned step = 0;
    std::unique_ptr<Packet> packet;
    std::unique_ptr<PciPioCompl> completion;
    EventFunctionWrapper event;

    void advance() {
        if (completion) {
            if (!completion->done) {
                schedule(event, curTick() + 10000);
                return;
            }
            if (packet->isRead())
                fatal_if(packet->getLE<uint32_t>() != Value,
                         "Initialized NIC probe lost RSS register state");
            completion.reset(); packet.reset();
            ++step;
        }
        if (step == 2) {
            exitSimLoop("INITIALIZED_NIC_PROBE_READY");
            return;
        }
        auto req = std::make_shared<Request>(RegBase + RssKey, 4, 0,
                                            Request::funcRequestorId);
        const bool write = step == 0;
        packet = std::make_unique<Packet>(req, write ? MemCmd::WriteReq : MemCmd::ReadReq);
        packet->allocate();
        if (write) packet->setLE<uint32_t>(Value);
        completion = std::make_unique<PciPioCompl>(packet.get());
        if (write) writeAsync(*completion);
        else readAsync(*completion);
        schedule(event, curTick() + 10000);
    }

  public:
    PARAMS(SimBricksCheckpointTester);
    CheckpointTester(const Params &p)
      : Device(p), event([this]{ advance(); }, name() + ".checkpointProbe") {}

    void startup() override {
        Device::startup();
        if (params().restore_probe) {
            step = 1; // No BAR writes, NIC initialization, or RSS writes.
        } else {
            auto req = std::make_shared<Request>(PCI0_BASE_ADDR0, 4, 0,
                                                Request::funcRequestorId);
            Packet cfg(req, MemCmd::WriteReq);
            cfg.allocate(); cfg.setLE<uint32_t>(RegBase);
            writeConfig(&cfg);
            auto command = std::make_shared<Request>(PCI_COMMAND, 2, 0,
                                                    Request::funcRequestorId);
            Packet enable(command, MemCmd::WriteReq);
            enable.allocate(); enable.setLE<uint16_t>(6);
            writeConfig(&enable);
        }
        schedule(event, curTick() + 1000000);
    }
};
} // namespace gem5::simbricks::pci
#endif
