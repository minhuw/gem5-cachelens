// SPDX-License-Identifier: BSD-3-Clause

#include "test_objects/pvrdma_tester.hh"

#include <cstddef>

#include "base/logging.hh"
#include "dev/pci/pcireg.h"
#include "dev/rdma/pvrdma_abi.hh"
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
const Request::Flags MmioFlags = Request::UNCACHEABLE;

} // anonymous namespace

bool
PvrdmaTester::TestPort::recvTimingResp(PacketPtr)
{
    panic("PVRDMA atomic tester received a timing response");
}

void
PvrdmaTester::TestPort::recvReqRetry()
{
    panic("PVRDMA atomic tester received a request retry");
}

PvrdmaTester::PvrdmaTester(const Params &p)
    : Platform(p), port(name() + ".port", *this),
      requestorId(system->getRequestorId(this)),
      testEvent([this] { run(); }, name() + ".test"),
      commandTest(p.command_test)
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
             "PVRDMA capabilities were not visible when DSRHIGH returned");
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
}

void
PvrdmaTester::run()
{
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
