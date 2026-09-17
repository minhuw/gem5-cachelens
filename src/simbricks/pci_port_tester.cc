// SPDX-License-Identifier: MIT
#include "simbricks/pci_port_tester.hh"
#include "dev/pci/config_write.hh"
#include "mem/packet_access.hh"
#include "sim/sim_exit.hh"
#include "sim/core.hh"
namespace gem5::simbricks::pci {
void PortTester::init()
{
    // Listening socket only: inject a directed introduction, no external model.
    PioDevice::init();
    panic_if(&getPort("pio") != &getPioPort() || pioPort.isConnected(),
             "getPioPort dispatch failed");
    SimbricksProtoPcieDevIntro di = {};
    di.bars[4].len = di.bars[5].len = 8192;
    di.pci_msi_nvecs = 4; di.pci_msix_nvecs = 64;
    di.pci_msix_table_bar = 4; di.pci_msix_pba_bar = 5;
    di.pci_msix_table_offset = di.pci_msix_pba_offset = 4096;
    if (params().failure == "intro") di.pci_msix_table_bar = 6;
    introInReceived(&di, sizeof(di));
}
void PortTester::startup()
{
    auto access = [this](unsigned off, unsigned size, uint32_t value, bool write) {
        auto req = std::make_shared<Request>(off, size, 0, Request::funcRequestorId);
        Packet pkt(req, write ? MemCmd::WriteReq : MemCmd::ReadReq);
        pkt.allocate();
        uint8_t bytes[4] = {};
        for (unsigned i = 0; i < size; ++i) bytes[i] = value >> (8 * i);
        if (write) pkt.setData(bytes);
        if (!configAccess(&pkt, write)) {
            if (write) PciDevice::writeConfig(&pkt);
            else PciDevice::readConfig(&pkt);
        }
        uint32_t result = 0;
        if (!write) for (unsigned i = 0; i < size; ++i)
            result |= uint32_t(pkt.getConstPtr<uint8_t>()[i]) << (8 * i);
        return result;
    };
    if (params().failure == "vector") msix_signal(64);
    if (params().failure == "config") access(4095, 4, 0, false);
    if (params().failure == "table") {
        auto req = std::make_shared<Request>(0, 8, 0, Request::funcRequestorId);
        Packet pkt(req, MemCmd::ReadReq); pkt.allocate();
        PciPioCompl comp(&pkt); readMsix(comp, 4096 + 16 * 64 - 1, 4);
    }
    access(PCI0_BASE_ADDR4, 4, 0x10000000, true);
    access(PCI0_BASE_ADDR5, 4, 0x11000000, true);
    for (unsigned size : {1U, 2U, 4U}) {
        config.command = 0; config.status = 0xffff;
        access(PCI_COMMAND, size, 3, true);
        panic_if(config.command != 3 || config.status != 0xffff,
                 "Legacy command write changed status");
        if (size == 4) access(PCI_COMMAND, 4, 0xffff0003, true);
        else access(PCI_STATUS + (size == 1), size, 0xffff, true);
        panic_if(config.command != 3 || config.status != 0x06ff,
                 "Legacy W1C regression");
    }
    const unsigned cap = MSIXCAP_BASE;
    uint32_t identity = access(cap, 4, 0, false);
    access(cap, 4, 0xffffffff, true);
    panic_if(access(cap, 4, 0, false) != (identity | 0xc0000000), "MSIX mask regression");
    access(cap + 4, 4, 0xffffffff, true);
    access(cap + 8, 4, 0xffffffff, true);
    panic_if(access(cap + 4, 4, 0, false) != 0x1004 ||
             access(cap + 8, 4, 0, false) != 0x1005, "MSIX BIR/offset regression");
    access(cap + 1, 2, 0, true); // unaligned, readonly bytes
    access(cap + 3, 1, 0, true);
    panic_if(access(cap, 4, 0, false) != identity, "MSIX readonly/disable regression");
    testMsix();
}

void PortTester::testMsix()
{
    // Each stage runs 100 ns apart, allowing MSI DMA completion events to fire.
    // Atomic DMA reads/writes with nullptr completion are synchronous.
    const unsigned scenario = msixStep / 5, stage = msixStep % 5;
    const uint64_t pending = (uint64_t(1) << 63) | 1;
    const uint32_t sentinel = 0xa5a5a5a5;
    const uint32_t data[] = {0x12345678, 0x87654321};
    auto tableWrite = [this](unsigned vec, unsigned col, uint32_t value) {
        auto req = std::make_shared<Request>(0, 4, 0, Request::funcRequestorId);
        Packet pkt(req, MemCmd::WriteReq);
        pkt.allocate();
        pkt.setLE<uint32_t>(value);
        PciPioCompl comp(&pkt);
        panic_if(!writeMsix(comp, MSIX_TABLE_OFFSET + 16 * vec + col, 4) ||
                 !comp.done, "MSI-X table write failed");
    };
    auto vectorMask = [&](bool masked) {
        for (unsigned vec : {0U, 63U}) tableWrite(vec, 12, masked);
    };
    auto control = [this](uint16_t value) {
        auto req = std::make_shared<Request>(MSIXCAP_BASE + 2, 2, 0,
                                            Request::funcRequestorId);
        Packet pkt(req, MemCmd::WriteReq);
        pkt.allocate();
        pkt.setLE<uint16_t>(value);
        panic_if(!configAccess(&pkt, true), "MSI-X control write failed");
        // Same production replay path, without publishing devctrl to a peer.
        replayPendingMsix();
    };
    auto fill = [&] {
        for (unsigned i = 0; i < 2; ++i) {
            uint32_t value = htole(sentinel);
            dmaWrite(0x1000 + i * 4, 4, nullptr,
                     reinterpret_cast<uint8_t *>(&value));
        }
    };
    auto check = [&](bool delivered, uint64_t bits) {
        panic_if(msix_pba[0].bits != bits,
                 "MSI-X PBA mismatch: scenario %u stage %u", scenario, stage);
        for (unsigned i = 0; i < 2; ++i) {
            uint32_t value = 0;
            dmaRead(0x1000 + i * 4, 4, nullptr,
                    reinterpret_cast<uint8_t *>(&value));
            panic_if(letoh(value) != (delivered ? data[i] : sentinel),
                     "MSI-X DMA mismatch: scenario %u stage %u vector %u",
                     scenario, stage, i ? 63 : 0);
        }
    };
    // Function mask, vector mask, combined (both release orders), disabled
    // with existing pending, and disabled fresh signals (discard, not defer).
    switch (stage) {
      case 0:
        control(0);
        fill();
        for (unsigned i = 0; i < 2; ++i) {
            unsigned vec = i ? 63 : 0;
            tableWrite(vec, 0, 0x1000 + i * 4);
            tableWrite(vec, 4, 0);
            tableWrite(vec, 8, data[i]);
        }
        vectorMask(scenario >= 1 && scenario <= 3);
        control(scenario == 5 ? 0 : scenario == 1 ? 0x8000 : 0xc000);
        for (unsigned vec : {0U, 63U}) {
            msix_signal(vec);
            msix_signal(vec); // repeated masked signals coalesce in the PBA
        }
        break;
      case 1:
        check(false, scenario == 5 ? 0 : pending);
        if (scenario == 2) control(0x8000); // vector still masked
        if (scenario == 3) vectorMask(false); // function still masked
        if (scenario == 4) {
            control(0); // retain existing PBA while disabled
            msix_signal(0);
            msix_signal(63);
        }
        replayPendingMsix();
        break;
      case 2:
        check(false, scenario == 5 ? 0 : pending);
        control(0x8000);
        vectorMask(false); // actual writeMsix unmask must deliver pending DMA
        break;
      case 3:
        check(scenario != 5, 0);
        fill();
        replayPendingMsix();
        vectorMask(true);
        vectorMask(false);
        control(0xc000);
        control(0x8000);
        control(0);
        control(0x8000); // none of these may replay an already consumed bit
        break;
      case 4:
        check(false, 0); // sentinel proves no second delivery on replay
        break;
    }
    if (++msixStep == 6 * 5) {
        exitSimLoop("SIMBRICKS_PCI_PORT_OK");
    } else {
        schedule(new EventFunctionWrapper([this] { testMsix(); },
                 name() + ".testMsix", true), curTick() + 100 * sim_clock::as_int::ns);
    }
}
}
