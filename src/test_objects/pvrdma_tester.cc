// SPDX-License-Identifier: BSD-3-Clause

#include "test_objects/pvrdma_tester.hh"

#include <algorithm>
#include <array>
#include <cstddef>

#include "base/logging.hh"
#include "dev/pci/pcireg.h"
#include "dev/rdma/pvrdma.hh"
#include "mem/packet.hh"
#include "sim/byteswap.hh"
#include "sim/core.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5
{

namespace
{

constexpr Addr PciConfigBase = 0x20000000;
constexpr Addr PciConfigAddress = PciConfigBase + ((2 << 3 | 1) << 8);
constexpr Addr MsixBarAddress = 0x10000000;
constexpr Addr RegisterBarAddress = 0x10004000;
constexpr Addr UarBarAddress = 0x10200000;
constexpr Addr DsrAddress = 0x1000;
constexpr Addr CommandAddress = 0x2000;
constexpr Addr ResponseAddress = 0x3000;
constexpr Addr MrDirectoryAddress = 0x8000;
constexpr Addr MrTableAddress = 0x9000;
constexpr Addr MrLeafAddress = 0x100000;
Tick
microseconds(uint64_t value)
{
    return value * sim_clock::as_int::us;
}

const Request::Flags MmioFlags = Request::UNCACHEABLE;

} // anonymous namespace

bool
PvrdmaTester::TestPort::recvTimingResp(PacketPtr)
{
    panic("PVRDMA tester received an unexpected timing response");
}

void
PvrdmaTester::TestPort::recvReqRetry()
{
    panic("PVRDMA tester received an unexpected request retry");
}

PvrdmaTester::PvrdmaTester(const Params &p)
    : Platform(p), port(name() + ".port", *this),
      requestorId(system->getRequestorId(this)),
      testEvent([this] { run(); }, name() + ".test"),
      commandTest(p.command_test), testMode(p.test_mode)
{}

Port &
PvrdmaTester::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port")
        return port;
    return Platform::getPort(if_name, idx);
}

void
PvrdmaTester::startup()
{
    schedule(testEvent, curTick());
}

template <typename T>
void
PvrdmaTester::write(Addr addr, const T &value, Request::Flags flags)
{
    auto request = std::make_shared<Request>(addr, sizeof(T), flags,
                                             requestorId);
    Packet packet(request, MemCmd::WriteReq);
    T data = value;
    packet.dataStatic(reinterpret_cast<uint8_t *>(&data));
    port.sendAtomic(&packet);
    panic_if(!packet.isResponse() || packet.isError(),
             "PVRDMA tester write failed at %#x", addr);
}

template <typename T>
T
PvrdmaTester::read(Addr addr, Request::Flags flags)
{
    auto request = std::make_shared<Request>(addr, sizeof(T), flags,
                                             requestorId);
    Packet packet(request, MemCmd::ReadReq);
    T data{};
    packet.dataStatic(reinterpret_cast<uint8_t *>(&data));
    port.sendAtomic(&packet);
    panic_if(!packet.isResponse() || packet.isError(),
             "PVRDMA tester read failed at %#x", addr);
    return data;
}

void
PvrdmaTester::configurePci()
{
    write(PciConfigAddress + PCI0_BASE_ADDR0,
          htole(static_cast<uint32_t>(MsixBarAddress)), MmioFlags);
    write(PciConfigAddress + PCI0_BASE_ADDR1,
          htole(static_cast<uint32_t>(RegisterBarAddress)), MmioFlags);
    write(PciConfigAddress + PCI0_BASE_ADDR2,
          htole(static_cast<uint32_t>(UarBarAddress)), MmioFlags);
    write(PciConfigAddress + PCI_COMMAND,
          htole(static_cast<uint16_t>(PCI_CMD_MSE | PCI_CMD_BME)), MmioFlags);
}

void
PvrdmaTester::configureDsr()
{
    pvrdma::DeviceSharedRegion dsr{};
    dsr.driverVersion = htole(pvrdma::Version);
    dsr.commandSlotDma = htole(static_cast<uint64_t>(CommandAddress));
    dsr.responseSlotDma = htole(static_cast<uint64_t>(ResponseAddress));
    dsr.asyncRingPages.numPages = htole(pvrdma::NumRingPages);
    dsr.asyncRingPages.pageDirectoryDma = htole(uint64_t{0x4000});
    dsr.completionRingPages.numPages = htole(pvrdma::NumRingPages);
    dsr.completionRingPages.pageDirectoryDma = htole(uint64_t{0x5000});
    write(DsrAddress, dsr);

    write(RegisterBarAddress + pvrdma::RegDsrLow,
          htole(static_cast<uint32_t>(DsrAddress)), MmioFlags);
    write(RegisterBarAddress + pvrdma::RegDsrHigh, uint32_t{0}, MmioFlags);
    dsrConfigured = true;
}

void
PvrdmaTester::testCapabilities()
{
    const auto caps = read<pvrdma::DeviceCaps>(
        DsrAddress + offsetof(pvrdma::DeviceSharedRegion, caps));
    panic_if(caps.mode != static_cast<uint8_t>(pvrdma::DeviceMode::Roce) ||
                 caps.gidTypes != pvrdma::GidTypeRoceV1,
             "PVRDMA capabilities were not visible after DSR completion");
}

void
PvrdmaTester::activateAndCreatePd()
{
    write(RegisterBarAddress + pvrdma::RegControl,
          htole(static_cast<uint32_t>(pvrdma::DeviceControl::Activate)),
          MmioFlags);

    pvrdma::CommandRequest request{};
    request.header.response = htole(uint64_t{0x1111222233334444});
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::CreatePd));
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
}

void
PvrdmaTester::verifyPd()
{
    const auto response = read<pvrdma::CommandResponse>(ResponseAddress);
    const uint32_t cause = read<uint32_t>(
        RegisterBarAddress + pvrdma::RegInterruptCause, MmioFlags);
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0 ||
                 letoh(response.header.acknowledgement) !=
                     pvrdma::responseCommand(pvrdma::Command::CreatePd) ||
                 letoh(response.createPd.pdHandle) != 1 ||
                 !(cause & pvrdma::InterruptCauseResponse),
             "PVRDMA CREATE_PD failed");
}

void
PvrdmaTester::prepareMrPages(uint32_t pages, bool malformed)
{
    std::array<uint64_t, pvrdma::MrEntriesPerPage> directory{};
    std::array<uint64_t, pvrdma::MrEntriesPerPage> table{};
    directory[0] = htole(static_cast<uint64_t>(MrTableAddress));
    if (pages > pvrdma::MrEntriesPerPage) {
        directory[1] = htole(
            static_cast<uint64_t>(MrTableAddress + pvrdma::MrPageSize));
    }
    write(MrDirectoryAddress, directory);

    const uint32_t first = std::min(pages, pvrdma::MrEntriesPerPage);
    for (uint32_t i = 0; i < first; ++i) {
        table[i] = htole(static_cast<uint64_t>(
            MrLeafAddress + uint64_t{i} * pvrdma::MrPageSize));
    }
    write(MrTableAddress, table);

    if (pages > pvrdma::MrEntriesPerPage) {
        table = {};
        for (uint32_t i = pvrdma::MrEntriesPerPage; i < pages; ++i) {
            uint64_t leaf = MrLeafAddress +
                uint64_t{i} * pvrdma::MrPageSize;
            if (malformed && i + 1 == pages)
                ++leaf;
            table[i - pvrdma::MrEntriesPerPage] = htole(leaf);
        }
        write(MrTableAddress + pvrdma::MrPageSize, table);
    }
}

void
PvrdmaTester::startMr(uint32_t pages, uint64_t response)
{
    pvrdma::CommandRequest request{};
    request.header.response = htole(response);
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::CreateMr));
    request.createMr.start = 0;
    request.createMr.length = htole(
        uint64_t{pages} * pvrdma::MrPageSize);
    request.createMr.pageDirectoryDma = htole(
        static_cast<uint64_t>(MrDirectoryAddress));
    request.createMr.pdHandle = htole(uint32_t{1});
    request.createMr.accessFlags = htole(pvrdma::AccessLocalWrite);
    request.createMr.numChunks = htole(pages);
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
}

uint32_t
PvrdmaTester::verifyMr(uint64_t response_token, uint32_t key)
{
    const auto response = read<pvrdma::CommandResponse>(ResponseAddress);
    const uint32_t cause = read<uint32_t>(
        RegisterBarAddress + pvrdma::RegInterruptCause, MmioFlags);
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0 ||
                 !(cause & pvrdma::InterruptCauseResponse) ||
                 letoh(response.header.response) != response_token ||
                 letoh(response.header.acknowledgement) !=
                     pvrdma::responseCommand(pvrdma::Command::CreateMr) ||
                 response.header.error != 0 ||
                 letoh(response.createMr.mrHandle) != key ||
                 letoh(response.createMr.lkey) != key ||
                 letoh(response.createMr.rkey) != key,
             "PVRDMA CREATE_MR failed for key %u", key);
    return key;
}

void
PvrdmaTester::destroyMr(uint32_t handle)
{
    pvrdma::CommandRequest request{};
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::DestroyMr));
    request.destroyMr.mrHandle = htole(handle);
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
}

void
PvrdmaTester::testCommand()
{
    panic_if(!dsrConfigured, "PVRDMA command test has no configured DSR");
    write(RegisterBarAddress + pvrdma::RegControl,
          htole(static_cast<uint32_t>(pvrdma::DeviceControl::Activate)),
          MmioFlags);

    pvrdma::CommandRequest request{};
    request.header.response = htole(uint64_t{0x123456789abcdef0});
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::QueryPort));
    request.queryPort.portNumber = 1;
    write(CommandAddress, request);

    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
    const auto response = read<pvrdma::CommandResponse>(ResponseAddress);
    panic_if(response.header.response != request.header.response ||
                 letoh(response.header.acknowledgement) !=
                     pvrdma::responseCommand(pvrdma::Command::QueryPort) ||
                 response.header.error != 0 ||
                 letoh(static_cast<uint32_t>(
                     response.queryPort.attributes.state)) !=
                     static_cast<uint32_t>(pvrdma::PortState::Active),
             "PVRDMA response was not visible when REQUEST returned");

    request = {};
    request.header.response = htole(uint64_t{0x1111222233334444});
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::CreatePd));
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
    verifyPd();

    prepareMrPages(1, false);
    startMr(1, 0x5555666677778888);
    verifyMr(0x5555666677778888, 1);
    destroyMr(1);
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0,
             "PVRDMA DESTROY_MR failed in atomic visibility test");

    request = {};
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::DestroyPd));
    request.destroyPd.pdHandle = htole(uint32_t{1});
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0,
             "PVRDMA DESTROY_PD remained busy after DESTROY_MR");
}

void
PvrdmaTester::runTimingMr()
{
    panic_if(system->isAtomicMode(),
             "PVRDMA timing MR test requires timing mode");

    switch (timingStage) {
      case TimingStage::Configure:
        configurePci();
        configureDsr();
        timingStage = TimingStage::Pd;
        schedule(testEvent, curTick() + microseconds(40));
        return;
      case TimingStage::Pd:
        testCapabilities();
        activateAndCreatePd();
        timingStage = TimingStage::Mr512Pio;
        schedule(testEvent, curTick() + microseconds(40));
        return;
      case TimingStage::Mr512Pio:
        verifyPd();
        prepareMrPages(512, false);
        startMr(512, 0x5120512051205120);
        timingStage = TimingStage::Mr512Done;
        schedule(testEvent, curTick() + microseconds(40));
        return;
      case TimingStage::Mr512Done:
        verifyMr(0x5120512051205120, 1);
        destroyMr(1);
        timingStage = TimingStage::Mr513Pio;
        schedule(testEvent, curTick() + microseconds(40));
        return;
      case TimingStage::Mr513Pio:
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                MmioFlags) != 0,
                 "PVRDMA timing DESTROY_MR(1) failed");
        prepareMrPages(513, false);
        startMr(513, 0x5130513051305130);
        timingStage = TimingStage::Mr513Done;
        schedule(testEvent, curTick() + microseconds(40));
        return;
      case TimingStage::Mr513Done:
        verifyMr(0x5130513051305130, 65);
        destroyMr(65);
        timingStage = TimingStage::BadMrPio;
        schedule(testEvent, curTick() + microseconds(40));
        return;
      case TimingStage::BadMrPio:
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                MmioFlags) != 0,
                 "PVRDMA timing DESTROY_MR(65) failed");
        prepareMrPages(513, true);
        startMr(513, 0xbad0bad0bad0bad0);
        timingStage = TimingStage::BadMrDone;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      case TimingStage::BadMrDone: {
        pioError = read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                  MmioFlags);
        const auto early = read<pvrdma::CommandResponse>(ResponseAddress);
        const uint32_t cause = read<uint32_t>(
            RegisterBarAddress + pvrdma::RegInterruptCause, MmioFlags);
        panic_if(pioError != 0 || cause ||
                     (letoh(early.header.response) == 0xbad0bad0bad0bad0 &&
                      letoh(early.header.acknowledgement) ==
                          pvrdma::responseCommand(
                              pvrdma::Command::CreateMr)),
                 "Linux could accept malformed MR at REQUEST completion");
        timingStage = TimingStage::LateMrDone;
        schedule(testEvent, curTick() + microseconds(40));
        return;
      }
      case TimingStage::LateMrDone:
        break;
    }

    const auto response = read<pvrdma::CommandResponse>(ResponseAddress);
    const uint32_t cause = read<uint32_t>(
        RegisterBarAddress + pvrdma::RegInterruptCause, MmioFlags);
    const uint32_t final_error = read<uint32_t>(
        RegisterBarAddress + pvrdma::RegError, MmioFlags);
    panic_if(pioError != 0 ||
                 !(cause & pvrdma::InterruptCauseResponse) ||
                 final_error != pvrdma::CommandError ||
                 letoh(response.header.response) != 0xbad0bad0bad0bad0 ||
                 letoh(response.header.acknowledgement) ==
                     pvrdma::responseCommand(pvrdma::Command::CreateMr) ||
                 response.header.error != pvrdma::CommandError ||
                 response.createMr.mrHandle || response.createMr.lkey ||
                 response.createMr.rkey,
             "Late malformed MR did not deterministically map to EFAULT");
    inform("PVRDMA timing 512/513-page MR and late-EFAULT test passed");
    exitSimLoop("PVRDMA timing MR test passed");
}

void
PvrdmaTester::testCheckpointSave()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA checkpoint setup requires atomic mode");
    configurePci();
    configureDsr();
    testCapabilities();
    activateAndCreatePd();
    verifyPd();
    prepareMrPages(1, false);
    startMr(1, 0xc001c001c001c001);
    verifyMr(0xc001c001c001c001, 1);
    destroyMr(1);
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0,
             "PVRDMA checkpoint generation setup destroy failed");
    startMr(1, 0xc065c065c065c065);
    verifyMr(0xc065c065c065c065, 65);
    inform("PVRDMA checkpoint live generation-1 MR ready");
    exitSimLoop("PVRDMA checkpoint save ready");
}

void
PvrdmaTester::testCheckpointRestore()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA checkpoint restore test requires atomic mode");
    destroyMr(65);
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0,
             "Restored PVRDMA live MR was missing");
    startMr(1, 0xc129c129c129c129);
    verifyMr(0xc129c129c129c129, 129);
    inform("PVRDMA checkpoint restored live MR and key generation");
    exitSimLoop("PVRDMA checkpoint restore test passed");
}

void
PvrdmaTester::run()
{
    if (testMode == "timing-mr") {
        runTimingMr();
        return;
    }
    if (testMode == "checkpoint-save") {
        testCheckpointSave();
        return;
    }
    if (testMode == "checkpoint-restore") {
        testCheckpointRestore();
        return;
    }

    panic_if(!system->isAtomicMode(),
             "PVRDMA visibility tester requires atomic mode");
    if (!dsrConfigured) {
        configurePci();
        configureDsr();
        if (commandTest) {
            schedule(testEvent, curTick() + 20 * sim_clock::as_int::us);
            return;
        }
        testCapabilities();
    } else {
        testCommand();
    }

    inform("PVRDMA atomic %s visibility test passed",
           commandTest ? "command response" : "DSR capability");
    exitSimLoop("PVRDMA atomic visibility test passed");
}

} // namespace gem5
