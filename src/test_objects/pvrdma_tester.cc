// SPDX-License-Identifier: BSD-3-Clause

#include "test_objects/pvrdma_tester.hh"

#include <algorithm>
#include <array>
#include <cstddef>

#include "base/logging.hh"
#include "base/statistics.hh"
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
constexpr Addr CqDirectoryAddress = 0xb000;
constexpr Addr CqTableAddress = 0xc000;
constexpr Addr QpDirectoryAddress = 0xd000;
constexpr Addr QpTableAddress = 0xe000;
constexpr Addr BadQpDirectoryAddress = 0xf000;
constexpr Addr BadQpTableAddress = 0x10000;
constexpr Addr MrLeafAddress = 0x100000;
constexpr Addr CqLeafAddress = 0x400000;
constexpr Addr QpLeafAddress = 0x500000;
constexpr Addr BadQpLeafAddress = 0x600000;
constexpr Addr PayloadAddress = 0x700000;

statistics::Counter
statValue(const std::string &name)
{
    auto *info = statistics::resolve(name);
    panic_if(!info, "Missing PVRDMA statistic %s", name);
    auto *scalar = dynamic_cast<const statistics::ScalarInfo *>(info);
    panic_if(!scalar, "PVRDMA statistic %s is not scalar", name);
    return scalar->value();
}

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
PvrdmaTester::prepareQueuePages(Addr directory_addr, Addr table_addr,
                                Addr first_page, uint32_t pages,
                                bool malformed)
{
    std::array<uint64_t, pvrdma::PageEntries> directory{};
    std::array<uint64_t, pvrdma::PageEntries> table{};
    directory[0] = htole(static_cast<uint64_t>(table_addr));
    for (uint32_t i = 0; i < pages; ++i) {
        uint64_t page = first_page + uint64_t{i} * pvrdma::PageSize;
        if (malformed && i + 1 == pages)
            ++page;
        table[i] = htole(page);
    }
    write(directory_addr, directory);
    write(table_addr, table);
}

void
PvrdmaTester::startUserContext(uint64_t response)
{
    pvrdma::CommandRequest request{};
    request.header.response = htole(response);
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::CreateUc));
    request.createUc.pfn64 = htole(
        static_cast<uint64_t>(UarBarAddress / pvrdma::UarPageSize + 1));
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
}

void
PvrdmaTester::startUserPd(uint64_t response)
{
    pvrdma::CommandRequest request{};
    request.header.response = htole(response);
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::CreatePd));
    request.createPd.contextHandle = htole(uint32_t{1});
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
}

void
PvrdmaTester::startCq(uint64_t response, uint32_t cqe)
{
    const uint32_t pages = 1 + pvrdma::detail::chunksFor(
        cqe, pvrdma::CqeSize);
    prepareQueuePages(CqDirectoryAddress, CqTableAddress, CqLeafAddress,
                      pages);
    pvrdma::CommandRequest request{};
    request.header.response = htole(response);
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::CreateCq));
    request.createCq.pageDirectoryDma = htole(
        static_cast<uint64_t>(CqDirectoryAddress));
    request.createCq.contextHandle = htole(uint32_t{1});
    request.createCq.cqe = htole(cqe);
    request.createCq.numChunks = htole(pages);
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
}

void
PvrdmaTester::startQp(uint64_t response, bool malformed)
{
    constexpr uint32_t SendWr = 32;
    constexpr uint32_t RecvWr = 128;
    const uint32_t send_chunks = pvrdma::detail::chunksFor(
        SendWr, pvrdma::SqStride);
    const uint32_t pages = 1 + send_chunks + pvrdma::detail::chunksFor(
        RecvWr, pvrdma::RqStride);
    const Addr directory = malformed ? BadQpDirectoryAddress :
        QpDirectoryAddress;
    prepareQueuePages(directory,
                      malformed ? BadQpTableAddress : QpTableAddress,
                      malformed ? BadQpLeafAddress : QpLeafAddress,
                      pages, malformed);
    pvrdma::CommandRequest request{};
    request.header.response = htole(response);
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::CreateQp));
    request.createQp.pageDirectoryDma = htole(
        static_cast<uint64_t>(directory));
    request.createQp.pdHandle = htole(uint32_t{1});
    request.createQp.sendCqHandle = htole(uint32_t{1});
    request.createQp.recvCqHandle = htole(uint32_t{1});
    request.createQp.maxSendWr = htole(SendWr);
    request.createQp.maxRecvWr = htole(RecvWr);
    request.createQp.maxSendSge = htole(uint32_t{1});
    request.createQp.maxRecvSge = htole(uint32_t{1});
    request.createQp.accessFlags = htole(pvrdma::AccessLocalWrite);
    request.createQp.totalChunks = htole(static_cast<uint16_t>(pages));
    request.createQp.sendChunks = htole(
        static_cast<uint16_t>(send_chunks));
    request.createQp.qpType = static_cast<uint8_t>(pvrdma::QpType::Rc);
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
}

void
PvrdmaTester::startModifyQp(uint64_t response, pvrdma::QpState state,
                            uint32_t mask,
                            const pvrdma::QpAttr &attributes)
{
    pvrdma::CommandRequest request{};
    request.header.response = htole(response);
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::ModifyQp));
    request.modifyQp.qpHandle = htole(uint32_t{1});
    request.modifyQp.attributeMask = htole(mask);
    request.modifyQp.attributes = attributes;
    request.modifyQp.attributes.qpState = static_cast<pvrdma::QpState>(
        htole(static_cast<uint32_t>(state)));
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
}

void
PvrdmaTester::startQueryQp(uint64_t response)
{
    pvrdma::CommandRequest request{};
    request.header.response = htole(response);
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::QueryQp));
    request.queryQp.qpHandle = htole(uint32_t{1});
    request.queryQp.attributeMask = htole((uint32_t{1} << 21) - 1);
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
}

pvrdma::CommandResponse
PvrdmaTester::verifyResponse(pvrdma::Command command, uint64_t token)
{
    const auto response = read<pvrdma::CommandResponse>(ResponseAddress);
    const uint32_t cause = read<uint32_t>(
        RegisterBarAddress + pvrdma::RegInterruptCause, MmioFlags);
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0 ||
                 !(cause & pvrdma::InterruptCauseResponse) ||
                 letoh(response.header.response) != token ||
                 letoh(response.header.acknowledgement) !=
                     pvrdma::responseCommand(command) ||
                 response.header.error,
             "PVRDMA command %u failed", static_cast<uint32_t>(command));
    return response;
}

void
PvrdmaTester::destroyQp()
{
    pvrdma::CommandRequest request{};
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::DestroyQp));
    request.destroyQp.qpHandle = htole(uint32_t{1});
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
}

void
PvrdmaTester::destroyCq()
{
    pvrdma::CommandRequest request{};
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::DestroyCq));
    request.destroyCq.cqHandle = htole(uint32_t{1});
    write(CommandAddress, request);
    write(RegisterBarAddress + pvrdma::RegRequest, uint32_t{0}, MmioFlags);
}

void
PvrdmaTester::createUserParentAtomic()
{
    startUserContext(0x1001001001001001);
    auto response = verifyResponse(pvrdma::Command::CreateUc,
                                   0x1001001001001001);
    panic_if(letoh(response.createUc.contextHandle) != 1,
             "PVRDMA CREATE_UC returned wrong handle");
    startUserPd(0x2002002002002002);
    response = verifyResponse(pvrdma::Command::CreatePd,
                              0x2002002002002002);
    panic_if(letoh(response.createPd.pdHandle) != 1,
             "PVRDMA user CREATE_PD returned wrong handle");
}

void
PvrdmaTester::createQueuePairAtomic(uint32_t expected_qpn)
{
    startCq(0x3003003003003003);
    auto response = verifyResponse(pvrdma::Command::CreateCq,
                                   0x3003003003003003);
    panic_if(letoh(response.createCq.cqHandle) != 1 ||
                 letoh(response.createCq.cqe) != 64,
             "PVRDMA CREATE_CQ returned wrong geometry");
    startQp(0x4004004004004004);
    response = verifyResponse(pvrdma::Command::CreateQp,
                              0x4004004004004004);
    panic_if(letoh(response.createQpV2.qpHandle) != 1 ||
                 letoh(response.createQpV2.qpn) != expected_qpn ||
                 letoh(response.createQpV2.maxSendWr) != 32 ||
                 letoh(response.createQpV2.maxRecvWr) != 128,
             "PVRDMA CREATE_QP V2 returned wrong handle/QPN/capabilities");
}

void
PvrdmaTester::moveQueuePairToRtsAtomic()
{
    pvrdma::QpAttr attrs{};
    attrs.qpAccessFlags = htole(pvrdma::AccessLocalWrite);
    attrs.portNumber = 1;
    startModifyQp(0x5005005005005005, pvrdma::QpState::Init,
                  pvrdma::QpAttrState | pvrdma::QpAttrAccessFlags |
                  pvrdma::QpAttrPkeyIndex | pvrdma::QpAttrPort, attrs);
    verifyResponse(pvrdma::Command::ModifyQp, 0x5005005005005005);

    attrs = {};
    attrs.pathMtu = static_cast<pvrdma::Mtu>(
        htole(static_cast<uint32_t>(pvrdma::Mtu::Mtu1024)));
    attrs.destinationQpNumber = htole(uint32_t{1});
    attrs.receivePsn = htole(uint32_t{0x12345});
    attrs.maxDestinationReadAtomic = 1;
    attrs.minRnrTimer = 12;
    attrs.addressHandle.portNumber = 1;
    startModifyQp(0x6006006006006006,
                  pvrdma::QpState::ReadyToReceive,
                  pvrdma::QpAttrState | pvrdma::QpAttrAddressVector |
                  pvrdma::QpAttrPathMtu | pvrdma::QpAttrReceivePsn |
                  pvrdma::QpAttrMaxDestReadAtomic |
                  pvrdma::QpAttrMinRnrTimer |
                  pvrdma::QpAttrDestinationQpn,
                  attrs);
    verifyResponse(pvrdma::Command::ModifyQp, 0x6006006006006006);

    attrs = {};
    attrs.timeout = 14;
    attrs.retryCount = 6;
    attrs.rnrRetry = 7;
    attrs.sendPsn = htole(uint32_t{0x54321});
    attrs.maxReadAtomic = 1;
    startModifyQp(0x7007007007007007,
                  pvrdma::QpState::ReadyToSend,
                  pvrdma::QpAttrState | pvrdma::QpAttrTimeout |
                  pvrdma::QpAttrRetryCount | pvrdma::QpAttrRnrRetry |
                  pvrdma::QpAttrSendPsn |
                  pvrdma::QpAttrMaxQpReadAtomic,
                  attrs);
    verifyResponse(pvrdma::Command::ModifyQp, 0x7007007007007007);
}

void
PvrdmaTester::postObservedRings(uint32_t sq, uint32_t rq)
{
    pvrdma::RingState rings{};
    rings.tx.producerTail = htole(sq);
    rings.rx.producerTail = htole(rq);
    write(QpLeafAddress, rings);
}

void
PvrdmaTester::ringDoorbell(uint32_t action, uint32_t handle, bool cq)
{
    const Addr offset = cq ? pvrdma::CqDoorbellOffset :
                             pvrdma::QpDoorbellOffset;
    write(UarBarAddress + pvrdma::UarPageSize + offset,
          htole(action | handle), MmioFlags);
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
      default:
        panic("Invalid PVRDMA timing MR stage");
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
PvrdmaTester::runTimingQueues()
{
    panic_if(system->isAtomicMode(),
             "PVRDMA timing queue test requires timing mode");
    const Tick LongDelay = microseconds(40);
    const Tick ShortDelay = microseconds(10);

    switch (timingStage) {
      case TimingStage::Configure:
        configurePci();
        configureDsr();
        timingStage = TimingStage::UserContext;
        schedule(testEvent, curTick() + LongDelay);
        return;
      case TimingStage::UserContext:
        testCapabilities();
        write(RegisterBarAddress + pvrdma::RegControl,
              htole(static_cast<uint32_t>(
                  pvrdma::DeviceControl::Activate)), MmioFlags);
        startUserContext(0x1010101010101010);
        timingStage = TimingStage::UserPd;
        schedule(testEvent, curTick() + LongDelay);
        return;
      case TimingStage::UserPd: {
        const auto response = verifyResponse(pvrdma::Command::CreateUc,
                                             0x1010101010101010);
        panic_if(letoh(response.createUc.contextHandle) != 1,
                 "PVRDMA timing CREATE_UC returned wrong handle");
        startUserPd(0x2020202020202020);
        timingStage = TimingStage::Cq;
        schedule(testEvent, curTick() + LongDelay);
        return;
      }
      case TimingStage::Cq: {
        const auto response = verifyResponse(pvrdma::Command::CreatePd,
                                             0x2020202020202020);
        panic_if(letoh(response.createPd.pdHandle) != 1,
                 "PVRDMA timing CREATE_PD returned wrong handle");
        startCq(0x3030303030303030);
        timingStage = TimingStage::Qp;
        schedule(testEvent, curTick() + LongDelay);
        return;
      }
      case TimingStage::Qp: {
        const auto response = verifyResponse(pvrdma::Command::CreateCq,
                                             0x3030303030303030);
        panic_if(letoh(response.createCq.cqHandle) != 1 ||
                     letoh(response.createCq.cqe) != 64,
                 "PVRDMA timing CREATE_CQ returned wrong response");
        startQp(0x4040404040404040);
        timingStage = TimingStage::Init;
        schedule(testEvent, curTick() + LongDelay);
        return;
      }
      case TimingStage::Init: {
        const auto response = verifyResponse(pvrdma::Command::CreateQp,
                                             0x4040404040404040);
        panic_if(letoh(response.createQpV2.qpn) != 1 ||
                     letoh(response.createQpV2.qpHandle) != 1 ||
                     letoh(response.createQpV2.maxSendWr) != 32 ||
                     letoh(response.createQpV2.maxRecvWr) != 128,
                 "PVRDMA timing CREATE_QP V2 returned wrong response");
        pvrdma::QpAttr attrs{};
        attrs.qpAccessFlags = htole(pvrdma::AccessLocalWrite);
        attrs.portNumber = 1;
        startModifyQp(0x5050505050505050, pvrdma::QpState::Init,
                      pvrdma::QpAttrState | pvrdma::QpAttrAccessFlags |
                      pvrdma::QpAttrPkeyIndex | pvrdma::QpAttrPort,
                      attrs);
        timingStage = TimingStage::Rtr;
        schedule(testEvent, curTick() + LongDelay);
        return;
      }
      case TimingStage::Rtr: {
        verifyResponse(pvrdma::Command::ModifyQp, 0x5050505050505050);
        pvrdma::QpAttr attrs{};
        attrs.pathMtu = static_cast<pvrdma::Mtu>(
            htole(static_cast<uint32_t>(pvrdma::Mtu::Mtu1024)));
        attrs.destinationQpNumber = htole(uint32_t{1});
        attrs.receivePsn = htole(uint32_t{0x12345});
        attrs.maxDestinationReadAtomic = 1;
        attrs.minRnrTimer = 12;
        attrs.addressHandle.portNumber = 1;
        startModifyQp(0x6060606060606060,
                      pvrdma::QpState::ReadyToReceive,
                      pvrdma::QpAttrState | pvrdma::QpAttrAddressVector |
                      pvrdma::QpAttrPathMtu | pvrdma::QpAttrReceivePsn |
                      pvrdma::QpAttrMaxDestReadAtomic |
                      pvrdma::QpAttrMinRnrTimer |
                      pvrdma::QpAttrDestinationQpn,
                      attrs);
        timingStage = TimingStage::Rts;
        schedule(testEvent, curTick() + LongDelay);
        return;
      }
      case TimingStage::Rts: {
        verifyResponse(pvrdma::Command::ModifyQp, 0x6060606060606060);
        pvrdma::QpAttr attrs{};
        attrs.timeout = 14;
        attrs.retryCount = 6;
        attrs.rnrRetry = 7;
        attrs.sendPsn = htole(uint32_t{0x54321});
        attrs.maxReadAtomic = 1;
        startModifyQp(0x7070707070707070,
                      pvrdma::QpState::ReadyToSend,
                      pvrdma::QpAttrState | pvrdma::QpAttrTimeout |
                      pvrdma::QpAttrRetryCount | pvrdma::QpAttrRnrRetry |
                      pvrdma::QpAttrSendPsn |
                      pvrdma::QpAttrMaxQpReadAtomic,
                      attrs);
        timingStage = TimingStage::Query;
        schedule(testEvent, curTick() + LongDelay);
        return;
      }
      case TimingStage::Query:
        verifyResponse(pvrdma::Command::ModifyQp, 0x7070707070707070);
        if (testMode == "timing-observation") {
            panic_if(read<uint32_t>(
                         UarBarAddress + pvrdma::UarPageSize,
                         MmioFlags) != 0,
                     "PVRDMA BAR2 read was not deterministic");
            postObservedRings(3, 5);
            write(QpLeafAddress + pvrdma::PageSize,
                  uint64_t{0x1122334455667788});
            write(CqLeafAddress + pvrdma::PageSize,
                  uint64_t{0x8877665544332211});
            write(PayloadAddress, uint64_t{0xfeedfacecafebeef});
            ringDoorbell(pvrdma::SqDoorbellAction);
            ringDoorbell(pvrdma::SqDoorbellAction);
            ringDoorbell(pvrdma::RqDoorbellAction);
            ringDoorbell(pvrdma::CqPollAction, 1, true);
            ringDoorbell(pvrdma::CqArmSolicitedAction, 1, true);
            ringDoorbell(pvrdma::CqArmAnyAction, 1, true);
            write(UarBarAddress + 2 * pvrdma::UarPageSize,
                  htole(pvrdma::SqDoorbellAction | 1), MmioFlags);
            write(UarBarAddress + pvrdma::UarPageSize,
                  htole(pvrdma::SqDoorbellAction | 0x01000000 | 1),
                  MmioFlags);
            write(UarBarAddress + pvrdma::UarPageSize,
                  htole(static_cast<uint16_t>(
                      pvrdma::SqDoorbellAction | 1)), MmioFlags);
            ringDoorbell(pvrdma::SqDoorbellAction, 2);
            startQueryQp(0x8080808080808080);
            timingStage = TimingStage::ObservationActive;
        } else {
            startQueryQp(0x8080808080808080);
            timingStage = TimingStage::DestroyQp;
        }
        schedule(testEvent, curTick() +
            (timingStage == TimingStage::ObservationActive ?
                 microseconds(1) : LongDelay));
        return;
      case TimingStage::DestroyQp: {
        const auto response = verifyResponse(pvrdma::Command::QueryQp,
                                             0x8080808080808080);
        const auto &attrs = response.queryQp.attributes;
        panic_if(letoh(static_cast<uint32_t>(attrs.qpState)) !=
                         static_cast<uint32_t>(
                             pvrdma::QpState::ReadyToSend) ||
                     letoh(attrs.receivePsn) != 0x12345 ||
                     letoh(attrs.sendPsn) != 0x54321 ||
                     letoh(attrs.capabilities.maxSendWr) != 32 ||
                     letoh(attrs.capabilities.maxRecvWr) != 128,
                 "PVRDMA timing QUERY_QP lost stored attributes");
        destroyQp();
        timingStage = TimingStage::BadQp;
        schedule(testEvent, curTick() + LongDelay);
        return;
      }
      case TimingStage::BadQp:
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                MmioFlags) != 0,
                 "PVRDMA timing DESTROY_QP failed");
        startQp(0xbad1bad1bad1bad1, true);
        timingStage = TimingStage::BadQpEarly;
        schedule(testEvent, curTick() + ShortDelay);
        return;
      case TimingStage::BadQpEarly: {
        pioError = read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                  MmioFlags);
        const auto early = read<pvrdma::CommandResponse>(ResponseAddress);
        const uint32_t cause = read<uint32_t>(
            RegisterBarAddress + pvrdma::RegInterruptCause, MmioFlags);
        panic_if(pioError != 0 || cause ||
                     (letoh(early.header.response) ==
                          0xbad1bad1bad1bad1 &&
                      letoh(early.header.acknowledgement) ==
                          pvrdma::responseCommand(
                              pvrdma::Command::CreateQp)),
                 "Linux could accept malformed QP at REQUEST completion");
        timingStage = TimingStage::BadQpLate;
        schedule(testEvent, curTick() + LongDelay);
        return;
      }
      case TimingStage::BadQpLate: {
        const auto response = read<pvrdma::CommandResponse>(ResponseAddress);
        const uint32_t cause = read<uint32_t>(
            RegisterBarAddress + pvrdma::RegInterruptCause, MmioFlags);
        const uint32_t final_error = read<uint32_t>(
            RegisterBarAddress + pvrdma::RegError, MmioFlags);
        panic_if(pioError != 0 ||
                     !(cause & pvrdma::InterruptCauseResponse) ||
                     final_error != pvrdma::CommandError ||
                     letoh(response.header.response) !=
                         0xbad1bad1bad1bad1 ||
                     letoh(response.header.acknowledgement) ==
                         pvrdma::responseCommand(
                             pvrdma::Command::CreateQp) ||
                     response.header.error != pvrdma::CommandError ||
                     response.createQpV2.qpn ||
                     response.createQpV2.qpHandle,
                 "Late malformed QP did not clear acknowledgement");
        destroyCq();
        timingStage = TimingStage::DestroyCq;
        schedule(testEvent, curTick() + LongDelay);
        return;
      }
      case TimingStage::DestroyCq:
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                MmioFlags) != 0,
                 "PVRDMA timing DESTROY_CQ failed");
        inform("PVRDMA timing CQ/QP walk and RTS/query test passed");
        exitSimLoop("PVRDMA timing queue test passed");
        return;
      case TimingStage::ObservationActive:
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                MmioFlags) != pvrdma::CommandError,
                 "PVRDMA queued observation did not reject command");
        postObservedRings(4, 6);
        ringDoorbell(pvrdma::SqDoorbellAction);
        ringDoorbell(pvrdma::RqDoorbellAction);
        write(RegisterBarAddress + pvrdma::RegControl,
              htole(static_cast<uint32_t>(
                  pvrdma::DeviceControl::Reset)), MmioFlags);
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                MmioFlags) != pvrdma::CommandError,
                 "PVRDMA reset raced active queue DMA");
        destroyQp();
        timingStage = TimingStage::ObservationDone;
        schedule(testEvent, curTick() + LongDelay);
        return;
      case TimingStage::ObservationDone: {
        const auto rings = read<pvrdma::RingState>(QpLeafAddress);
        const auto cq_ring = read<pvrdma::Ring>(
            CqLeafAddress + offsetof(pvrdma::RingState, rx));
        const uint32_t cause = read<uint32_t>(
            RegisterBarAddress + pvrdma::RegInterruptCause, MmioFlags);
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                MmioFlags) != pvrdma::CommandError || cause ||
                     letoh(rings.tx.producerTail) != 4 ||
                     letoh(rings.tx.consumerHead) != 0 ||
                     letoh(rings.rx.producerTail) != 6 ||
                     letoh(rings.rx.consumerHead) != 0 ||
                     cq_ring.producerTail || cq_ring.consumerHead ||
                     read<uint64_t>(QpLeafAddress + pvrdma::PageSize) !=
                         0x1122334455667788 ||
                     read<uint64_t>(CqLeafAddress + pvrdma::PageSize) !=
                         0x8877665544332211 ||
                     read<uint64_t>(PayloadAddress) !=
                         0xfeedfacecafebeef,
                 "PVRDMA observation mutated guest-owned queue data");
        const std::string prefix = "system.rdma.queues.";
        panic_if(statValue(prefix + "sqDepth") != 32 ||
                     statValue(prefix + "rqDepth") != 128 ||
                     statValue(prefix + "cqDepth") != 64 ||
                     statValue(prefix + "sqOutstanding") != 4 ||
                     statValue(prefix + "rqAvailable") != 6 ||
                     statValue(prefix + "cqOutstanding") != 0 ||
                     statValue(prefix + "sqPosted") != 4 ||
                     statValue(prefix + "rqPosted") != 6 ||
                     statValue(prefix + "sqDoorbells") != 3 ||
                     statValue(prefix + "rqDoorbells") != 2 ||
                     statValue(prefix + "cqPollDoorbells") != 1 ||
                     statValue(prefix + "cqArmDoorbells") != 2 ||
                     statValue(prefix + "cqArmSolicitedDoorbells") != 1 ||
                     statValue(prefix + "doorbellWritesRejected") != 5 ||
                     statValue(prefix + "ringObservationsRejected") != 0 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA observation statistics mismatch");
        auto malformed = rings;
        malformed.tx.consumerHead = htole(uint32_t{1});
        write(QpLeafAddress, malformed);
        ringDoorbell(pvrdma::SqDoorbellAction);
        timingStage = TimingStage::ObservationMalformed;
        schedule(testEvent, curTick() + LongDelay);
        return;
      }
      case TimingStage::ObservationMalformed: {
        const std::string prefix = "system.rdma.queues.";
        panic_if(statValue(prefix + "sqOutstanding") != 4 ||
                     statValue(prefix + "sqPosted") != 4 ||
                     statValue(prefix + "ringObservationsRejected") != 1 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA malformed observation changed queue shadow");
        startModifyQp(0x9090909090909090, pvrdma::QpState::Reset,
                      pvrdma::QpAttrState, {});
        timingStage = TimingStage::ObservationReset;
        schedule(testEvent, curTick() + LongDelay);
        return;
      }
      case TimingStage::ObservationReset: {
        verifyResponse(pvrdma::Command::ModifyQp, 0x9090909090909090);
        const std::string prefix = "system.rdma.queues.";
        panic_if(statValue(prefix + "sqOutstanding") != 0 ||
                     statValue(prefix + "rqAvailable") != 0 ||
                     statValue(prefix + "sqResetDiscarded") != 4 ||
                     statValue(prefix + "rqResetDiscarded") != 6 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA timing nonempty QP reset accounting mismatch");
        inform("PVRDMA timing coherent queue observation test passed");
        exitSimLoop("PVRDMA timing observation test passed");
        return;
      }
      default:
        panic("Invalid PVRDMA timing queue stage");
    }
}

void
PvrdmaTester::runStatsReset()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA statistics test requires atomic mode");
    const std::string prefix = "system.rdma.queues.";
    switch (timingStage) {
      case TimingStage::Configure:
        configurePci();
        configureDsr();
        testCapabilities();
        write(RegisterBarAddress + pvrdma::RegControl,
              htole(static_cast<uint32_t>(
                  pvrdma::DeviceControl::Activate)), MmioFlags);
        createUserParentAtomic();
        createQueuePairAtomic(1);
        moveQueuePairToRtsAtomic();
        postObservedRings(3, 5);
        ringDoorbell(pvrdma::SqDoorbellAction);
        ringDoorbell(pvrdma::RqDoorbellAction);
        timingStage = TimingStage::StatsPosted;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      case TimingStage::StatsPosted:
        panic_if(statValue(prefix + "sqOutstanding") != 3 ||
                     statValue(prefix + "rqAvailable") != 5,
                 "PVRDMA pre-reset queue statistics mismatch");
        statistics::reset();
        panic_if(statValue(prefix + "sqOutstanding") != 3 ||
                     statValue(prefix + "rqAvailable") != 5 ||
                     statValue(prefix + "sqOutstandingAtReset") != 3 ||
                     statValue(prefix + "rqAvailableAtReset") != 5 ||
                     statValue(prefix + "sqPosted") != 0 ||
                     statValue(prefix + "rqPosted") != 0,
                 "PVRDMA statistics reset lost live occupancy");
        timingStage = TimingStage::StatsAdvance;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      case TimingStage::StatsAdvance:
        postObservedRings(5, 8);
        ringDoorbell(pvrdma::SqDoorbellAction);
        ringDoorbell(pvrdma::RqDoorbellAction);
        timingStage = TimingStage::StatsDone;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      case TimingStage::StatsDone: {
        panic_if(statValue(prefix + "sqOutstanding") != 5 ||
                     statValue(prefix + "rqAvailable") != 8 ||
                     statValue(prefix + "sqPosted") != 2 ||
                     statValue(prefix + "rqPosted") != 3 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA post-reset queue statistics mismatch");
        startModifyQp(0x9009009009009009, pvrdma::QpState::Reset,
                      pvrdma::QpAttrState, {});
        verifyResponse(pvrdma::Command::ModifyQp, 0x9009009009009009);
        panic_if(statValue(prefix + "sqOutstanding") != 0 ||
                     statValue(prefix + "rqAvailable") != 0 ||
                     statValue(prefix + "sqResetDiscarded") != 5 ||
                     statValue(prefix + "rqResetDiscarded") != 8 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA nonempty QP reset accounting mismatch");
        moveQueuePairToRtsAtomic();
        postObservedRings(2, 4);
        ringDoorbell(pvrdma::SqDoorbellAction);
        ringDoorbell(pvrdma::RqDoorbellAction);
        timingStage = TimingStage::StatsReposted;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      }
      case TimingStage::StatsReposted: {
        destroyQp();
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                MmioFlags) != 0 ||
                     statValue(prefix + "sqOutstanding") != 0 ||
                     statValue(prefix + "rqAvailable") != 0 ||
                     statValue(prefix + "sqResetDiscarded") != 7 ||
                     statValue(prefix + "rqResetDiscarded") != 12 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA nonempty QP destroy accounting mismatch");
        write(RegisterBarAddress + pvrdma::RegControl,
              htole(static_cast<uint32_t>(
                  pvrdma::DeviceControl::Reset)), MmioFlags);
        panic_if(statValue(prefix + "sqOutstanding") != 0 ||
                     statValue(prefix + "rqAvailable") != 0 ||
                     statValue(prefix + "sqResetDiscarded") != 7 ||
                     statValue(prefix + "rqResetDiscarded") != 12 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA device reset queue accounting mismatch");
        inform("PVRDMA queue statistics/reset test passed");
        exitSimLoop("PVRDMA queue statistics test passed");
        return;
      }
      default:
        panic("Invalid PVRDMA statistics stage");
    }
}

void
PvrdmaTester::testCheckpointSave()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA checkpoint setup requires atomic mode");
    configurePci();
    configureDsr();
    testCapabilities();
    write(RegisterBarAddress + pvrdma::RegControl,
          htole(static_cast<uint32_t>(pvrdma::DeviceControl::Activate)),
          MmioFlags);
    createUserParentAtomic();
    prepareMrPages(1, false);
    startMr(1, 0xc001c001c001c001);
    verifyMr(0xc001c001c001c001, 1);
    destroyMr(1);
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0,
             "PVRDMA checkpoint generation setup destroy failed");
    startMr(1, 0xc065c065c065c065);
    verifyMr(0xc065c065c065c065, 65);
    createQueuePairAtomic(1);
    moveQueuePairToRtsAtomic();
    inform("PVRDMA checkpoint live MR/CQ/RTS-QP ready");
    exitSimLoop("PVRDMA checkpoint save ready");
}

void
PvrdmaTester::testCheckpointRestore()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA checkpoint restore test requires atomic mode");
    startQueryQp(0xc0dec0dec0dec0de);
    auto response = verifyResponse(pvrdma::Command::QueryQp,
                                   0xc0dec0dec0dec0de);
    panic_if(letoh(static_cast<uint32_t>(
                 response.queryQp.attributes.qpState)) !=
                 static_cast<uint32_t>(pvrdma::QpState::ReadyToSend) ||
                 letoh(response.queryQp.attributes.sendPsn) != 0x54321,
             "Restored PVRDMA QP lost RTS attributes");
    destroyQp();
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0,
             "Restored PVRDMA live QP was missing");
    startQp(0xc041c041c041c041);
    response = verifyResponse(pvrdma::Command::CreateQp,
                              0xc041c041c041c041);
    panic_if(letoh(response.createQpV2.qpHandle) != 1 ||
                 letoh(response.createQpV2.qpn) != 65,
             "Restored PVRDMA QPN generation did not advance");
    destroyQp();
    destroyCq();
    destroyMr(65);
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0,
             "Restored PVRDMA live objects were missing");
    startMr(1, 0xc129c129c129c129);
    verifyMr(0xc129c129c129c129, 129);
    inform("PVRDMA checkpoint restored live MR/CQ/QP generations");
    exitSimLoop("PVRDMA checkpoint restore test passed");
}

void
PvrdmaTester::testCheckpointObservationSave()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA observation checkpoint setup requires atomic mode");
    if (timingStage == TimingStage::Configure) {
        configurePci();
        configureDsr();
        testCapabilities();
        write(RegisterBarAddress + pvrdma::RegControl,
              htole(static_cast<uint32_t>(
                  pvrdma::DeviceControl::Activate)), MmioFlags);
        createUserParentAtomic();
        createQueuePairAtomic(1);
        moveQueuePairToRtsAtomic();
        postObservedRings(5, 7);
        ringDoorbell(pvrdma::SqDoorbellAction);
        ringDoorbell(pvrdma::RqDoorbellAction);
        ringDoorbell(pvrdma::CqArmAnyAction, 1, true);
        timingStage = TimingStage::CheckpointObservationReady;
        schedule(testEvent, curTick() + microseconds(10));
        return;
    }
    panic_if(timingStage != TimingStage::CheckpointObservationReady,
             "Invalid PVRDMA observation checkpoint stage");
    const std::string prefix = "system.rdma.queues.";
    panic_if(statValue(prefix + "sqOutstanding") != 5 ||
                 statValue(prefix + "rqAvailable") != 7,
             "PVRDMA checkpoint missed queue observations");
    pvrdma::Ring cq_ring{};
    cq_ring.producerTail = htole(uint32_t{3});
    write(CqLeafAddress + offsetof(pvrdma::RingState, rx), cq_ring);
    inform("PVRDMA nonempty queue observation checkpoint ready");
    exitSimLoop("PVRDMA observation checkpoint save ready");
}

void
PvrdmaTester::testCheckpointObservationRestore()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA observation checkpoint restore requires atomic mode");
    const std::string prefix = "system.rdma.queues.";
    panic_if(statValue(prefix + "sqOutstanding") != 5 ||
                 statValue(prefix + "rqAvailable") != 7 ||
                 statValue(prefix + "cqOutstanding") != 3 ||
                 statValue(prefix + "sqOutstandingAtReset") != 5 ||
                 statValue(prefix + "rqAvailableAtReset") != 7 ||
                 statValue(prefix + "cqOutstandingAtReset") != 3,
             "PVRDMA restored queue baselines were incorrect");
    destroyQp();
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0 ||
                 statValue(prefix + "sqOutstanding") != 0 ||
                 statValue(prefix + "rqAvailable") != 0 ||
                 statValue(prefix + "sqResetDiscarded") != 5 ||
                 statValue(prefix + "rqResetDiscarded") != 7 ||
                 statValue(prefix + "cqOutstanding") != 3 ||
                 statValue(prefix + "conservationViolations") != 0,
             "Restored nonempty PVRDMA QP destroy accounting failed");
    destroyCq();
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0 ||
                 statValue(prefix + "cqOutstanding") != 0 ||
                 statValue(prefix + "cqResetDiscarded") != 3 ||
                 statValue(prefix + "conservationViolations") != 0,
             "Restored nonempty PVRDMA CQ destroy accounting failed");
    inform("PVRDMA nonempty queue observation checkpoint restored");
    exitSimLoop("PVRDMA observation checkpoint restored");
}

void
PvrdmaTester::run()
{
    if (testMode == "timing-mr") {
        runTimingMr();
        return;
    }
    if (testMode == "timing-queues" ||
        testMode == "timing-observation") {
        runTimingQueues();
        return;
    }
    if (testMode == "stats-reset") {
        runStatsReset();
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
    if (testMode == "checkpoint-observation-save") {
        testCheckpointObservationSave();
        return;
    }
    if (testMode == "checkpoint-observation-restore") {
        testCheckpointObservationRestore();
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
