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
constexpr Addr PeerPciConfigAddress =
    PciConfigBase + ((3 << 3 | 1) << 8);
constexpr Addr PeerMsixBarAddress = 0x11000000;
constexpr Addr PeerRegisterBarAddress = 0x11004000;
constexpr Addr PeerUarBarAddress = 0x11200000;
constexpr Addr PairSenderQp = 0x1000000;
constexpr Addr PairSenderSq = 0x1010000;
constexpr Addr PairSenderRq = 0x1020000;
constexpr Addr PairSenderCq = 0x1030000;
constexpr Addr PairSenderCqe = 0x1040000;
constexpr Addr PairSenderPayload = 0x1050000;
constexpr Addr PairReceiverQp = 0x2000000;
constexpr Addr PairReceiverSq = 0x2010000;
constexpr Addr PairReceiverRq = 0x2020000;
constexpr Addr PairReceiverCq = 0x2030000;
constexpr Addr PairReceiverCqe = 0x2040000;
constexpr Addr PairReceiverPayload = 0x2050000;

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

EthPacketPtr
transportPacket(const pvrdma::transport::Frame &frame,
                const pvrdma::transport::MacAddress &source,
                const pvrdma::transport::MacAddress &destination)
{
    const size_t size = pvrdma::transport::EthernetHeaderSize +
        pvrdma::transport::HeaderSize + frame.payload.size;
    auto packet = std::make_shared<EthPacketData>(size);
    const auto encoded = pvrdma::transport::encodeEthernet(
        frame, source, destination,
        {packet->data, packet->bufLength});
    panic_if(!encoded, "PVRDMA test DATA frame failed encode");
    packet->length = packet->simLength = encoded.size;
    return packet;
}

const Request::Flags MmioFlags = Request::UNCACHEABLE;

} // anonymous namespace

bool
PvrdmaTester::FaultPort::recvPacket(EthPacketPtr packet)
{
    const auto decoded = pvrdma::transport::decodeEthernet(
        {packet->data, packet->bufLength}, packet->length);
    panic_if(!decoded, "PVRDMA fault-link endpoint received a bad frame");
    if (tester.faultRejectOnce[side] && decoded.frame.messageId == 7) {
        tester.faultRejectOnce[side] = false;
        tester.faultDrainWhileRejected =
            tester.testLink->drain() == DrainState::Draining;
        return false;
    }
    tester.faultReceived[side].push_back(decoded.frame.messageId);
    return true;
}

void
PvrdmaTester::FaultPort::sendDone()
{
    ++tester.faultSendDone[side];
}

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
      faultPort0(name() + ".fault0", *this, 0),
      faultPort1(name() + ".fault1", *this, 1),
      requestorId(system->getRequestorId(this)),
      testEvent([this] { run(); }, name() + ".test"),
      commandTest(p.command_test), testMode(p.test_mode)
{}

Port &
PvrdmaTester::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port")
        return port;
    if (if_name == "fault0")
        return faultPort0;
    if (if_name == "fault1")
        return faultPort1;
    return Platform::getPort(if_name, idx);
}

void
PvrdmaTester::startup()
{
    if (testMode == "fault-link" || testMode == "timing-fault-link") {
        testLink = dynamic_cast<PvrdmaTestLink *>(
            SimObject::find("system.rdma_link"));
        panic_if(!testLink, "PVRDMA tester could not find test link");
        schedule(testEvent, curTick());
        return;
    }

    rdma = dynamic_cast<Pvrdma *>(SimObject::find("system.rdma"));
    panic_if(!rdma, "PVRDMA tester could not find system.rdma");
    const bool reliability_pair =
        testMode == "reliability-pair" ||
        testMode == "timing-reliability-pair" ||
        testMode == "reliability-rnr-pair" ||
        testMode == "timing-reliability-rnr-pair" ||
        testMode == "reliability-timeout-zero-pair" ||
        testMode == "timing-reliability-timeout-zero-pair" ||
        testMode == "reliability-invalid-pair" ||
        testMode == "timing-reliability-invalid-pair" ||
        testMode == "reliability-unrelated-pair" ||
        testMode == "timing-reliability-unrelated-pair" ||
        testMode == "reliability-cq-pair" ||
        testMode == "timing-reliability-cq-pair" ||
        testMode == "reliability-cq-abort-pair" ||
        testMode == "timing-reliability-cq-abort-pair" ||
        testMode == "timing-reliability-precommit-abort-pair" ||
        testMode == "timing-reliability-commit-pair" ||
        testMode == "timing-reliability-commit-boundary-pair";
    if (testMode == "transport-pair" ||
        testMode == "timing-transport-pair" ||
        testMode == "semantic-pair" ||
        testMode == "timing-semantic-pair" || reliability_pair) {
        peerRdma = dynamic_cast<Pvrdma *>(
            SimObject::find("system.peer_rdma"));
        panic_if(!peerRdma, "PVRDMA pair tester could not find peer");
        if (reliability_pair) {
            testLink = dynamic_cast<PvrdmaTestLink *>(
                SimObject::find("system.rdma_link"));
            panic_if(!testLink, "PVRDMA reliability tester has no fault link");
        }
    }
    auto &interface = rdma->getPort("interface");
    panic_if(&interface != &rdma->interface ||
                 &rdma->getPort("dma") == &interface,
             "PVRDMA Ethernet and PCI DMA ports are not independent");
    if (testMode == "timing-observation" || testMode == "stats-reset" ||
        testMode == "checkpoint-observation-save" ||
        testMode == "checkpoint-observation-restore")
        rdma->transportPaused = true;
    if (!peerRdma) {
        panic_if(rdma->interface.recvPacket(nullptr),
                 "PVRDMA foundation accepted an inbound packet");
        rdma->interface.sendDone();
    }
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

pvrdma::CompletionSubmitResult
PvrdmaTester::submitCompletion(pvrdma::CompletionOpcode opcode,
                               pvrdma::CompletionStatus status,
                               uint64_t wr_id, uint32_t byte_length,
                               uint32_t source_qp)
{
    panic_if(!rdma, "PVRDMA synthetic completion test has no device");
    pvrdma::CompletionRecord record;
    record.cqHandle = 1;
    record.qpHandle = 1;
    record.cqGeneration = rdma->completionQueues.entries[1].generation;
    record.qpGeneration = rdma->queuePairs.entries[1].generation;
    record.workRequestId = wr_id;
    record.opcode = opcode;
    record.status = status;
    record.byteLength = byte_length;
    record.sourceQp = source_qp;
    return rdma->submitCompletion(record);
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
            timingStage = TimingStage::ObservationActive;
        } else {
            startQueryQp(0x8080808080808080);
            timingStage = TimingStage::DestroyQp;
        }
        schedule(testEvent, curTick() +
            (timingStage == TimingStage::ObservationActive ?
                 2 * sim_clock::as_int::ns : LongDelay));
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
        panic_if(!rdma->queueDma.active(),
                 "PVRDMA queue observation did not start");
        startQueryQp(0x8080808080808080);
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                MmioFlags) != 0,
                 "PVRDMA active observation rejected command DMA");
        timingStage = TimingStage::ObservationCommandActive;
        schedule(testEvent, curTick() + microseconds(1));
        return;
      case TimingStage::ObservationCommandActive:
        panic_if(!rdma->queueDma.active() ||
                     rdma->controlState !=
                         pvrdma::ControlState::ReadingCommand,
                 "PVRDMA observation did not overlap command DMA");
        timingStage = TimingStage::ObservationCommandDone;
        schedule(testEvent, curTick() + LongDelay);
        return;
      case TimingStage::ObservationCommandDone:
        verifyResponse(pvrdma::Command::QueryQp,
                       0x8080808080808080);
        postObservedRings(4, 6);
        ringDoorbell(pvrdma::SqDoorbellAction);
        ringDoorbell(pvrdma::RqDoorbellAction);
        destroyQp();
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                MmioFlags) != 0,
                 "PVRDMA queued observation rejected command DMA");
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
PvrdmaTester::runTimingCompletion()
{
    panic_if(system->isAtomicMode(),
             "PVRDMA timing completion test requires timing mode");
    const Tick LongDelay = microseconds(40);
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
      case TimingStage::UserPd:
        verifyResponse(pvrdma::Command::CreateUc, 0x1010101010101010);
        startUserPd(0x2020202020202020);
        timingStage = TimingStage::Cq;
        schedule(testEvent, curTick() + LongDelay);
        return;
      case TimingStage::Cq:
        verifyResponse(pvrdma::Command::CreatePd, 0x2020202020202020);
        startCq(0x3030303030303030);
        timingStage = TimingStage::Qp;
        schedule(testEvent, curTick() + LongDelay);
        return;
      case TimingStage::Qp:
        verifyResponse(pvrdma::Command::CreateCq, 0x3030303030303030);
        startQp(0x4040404040404040);
        timingStage = TimingStage::Init;
        schedule(testEvent, curTick() + LongDelay);
        return;
      case TimingStage::Init: {
        verifyResponse(pvrdma::Command::CreateQp, 0x4040404040404040);
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
        panic_if(submitCompletion(pvrdma::CompletionOpcode::Receive,
                                  pvrdma::CompletionStatus::Success,
                                  0xabcdef0123456789, 512,
                                  0x123456) !=
                     pvrdma::CompletionSubmitResult::Queued,
                 "PVRDMA timing completion was not queued");
        timingStage = TimingStage::CompletionTimingCqe;
        schedule(testEvent, curTick() + microseconds(7));
        return;
      case TimingStage::CompletionTimingCqe: {
        const auto cqe = read<pvrdma::CompletionQueueElement>(
            CqLeafAddress + pvrdma::PageSize);
        const auto cq = read<pvrdma::Ring>(
            CqLeafAddress + offsetof(pvrdma::RingState, rx));
        panic_if(letoh(cqe.workRequestId) != 0xabcdef0123456789 ||
                     letoh(cqe.qp) != 1 || letoh(cqe.opcode) != 128 ||
                     letoh(cqe.status) != 0 ||
                     letoh(cqe.byteLength) != 512 ||
                     letoh(cqe.sourceQp) != 0x123456 || cq.producerTail,
                 "PVRDMA CQ producer became visible before the full CQE");
        ringDoorbell(pvrdma::CqArmSolicitedAction, 1, true);
        timingStage = TimingStage::CompletionTimingCqProducer;
        schedule(testEvent, curTick() + microseconds(9));
        return;
      }
      case TimingStage::CompletionTimingCqProducer: {
        const auto cqe = read<pvrdma::CompletionQueueElement>(
            CqLeafAddress + pvrdma::PageSize);
        const auto cq = read<pvrdma::Ring>(
            CqLeafAddress + offsetof(pvrdma::RingState, rx));
        panic_if(letoh(cqe.workRequestId) != 0xabcdef0123456789 ||
                     letoh(cqe.opcode) != 128 || letoh(cqe.status) != 0 ||
                     letoh(cqe.byteLength) != 512 ||
                     letoh(cqe.sourceQp) != 0x123456 ||
                     letoh(cq.producerTail) != 1 ||
                     rdma->completionQueues.entries[1].armFlags !=
                         static_cast<uint32_t>(
                             pvrdma::CqArmMode::Solicited) ||
                     (rdma->regs.pendingCauses &
                      pvrdma::InterruptCauseCompletion),
                 "PVRDMA CQ producer ordering/polling-only state mismatch");
        inform("PVRDMA timing completion DMA ordering test passed");
        exitSimLoop("PVRDMA timing completion test passed");
        return;
      }
      default:
        panic("Invalid PVRDMA timing completion stage");
    }
}

void
PvrdmaTester::runCompletionErrors()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA completion error test requires atomic mode");
    const std::string prefix = "system.rdma.queues.";
    switch (timingStage) {
      case TimingStage::Configure: {
        configurePci();
        configureDsr();
        testCapabilities();
        write(RegisterBarAddress + pvrdma::RegControl,
              htole(static_cast<uint32_t>(
                  pvrdma::DeviceControl::Activate)), MmioFlags);
        createUserParentAtomic();
        createQueuePairAtomic(1);
        moveQueuePairToRtsAtomic();
        statistics::reset();

        pvrdma::CompletionRecord stale;
        stale.cqHandle = stale.qpHandle = 1;
        stale.cqGeneration = rdma->completionQueues.entries[1].generation;
        stale.qpGeneration = rdma->queuePairs.entries[1].generation + 1;
        stale.opcode = pvrdma::CompletionOpcode::Send;
        panic_if(rdma->submitCompletion(stale) !=
                     pvrdma::CompletionSubmitResult::Rejected,
                 "PVRDMA stale completion was accepted");
        auto &qp = rdma->queuePairs.entries[1];
        const auto saved_state = qp.state;
        qp.state = pvrdma::QpState::Reset;
        panic_if(submitCompletion(pvrdma::CompletionOpcode::Send,
                                  pvrdma::CompletionStatus::Success, 1) !=
                     pvrdma::CompletionSubmitResult::Rejected,
                 "PVRDMA completion accepted an invalid QP state");
        qp.state = saved_state;
        const uint32_t saved_cq = qp.sendCqHandle;
        qp.sendCqHandle = 2;
        panic_if(submitCompletion(pvrdma::CompletionOpcode::Send,
                                  pvrdma::CompletionStatus::Success, 2) !=
                     pvrdma::CompletionSubmitResult::Rejected,
                 "PVRDMA completion accepted a wrong CQ association");
        qp.sendCqHandle = saved_cq;
        rdma->completionQueues.entries[1].armFlags =
            static_cast<uint32_t>(pvrdma::CqArmMode::Any);
        panic_if(read<pvrdma::Ring>(CqLeafAddress +
                     offsetof(pvrdma::RingState, rx)).producerTail ||
                     read<uint64_t>(CqLeafAddress + pvrdma::PageSize) ||
                     statValue(prefix + "cqPublicationRejected") != 3,
                 "PVRDMA rejected completion performed a partial write");

        auto &cq = rdma->completionQueues.entries[1];
        cq.producerTail = 64;
        cq.consumerHead = 0;
        pvrdma::Ring full{};
        full.producerTail = htole(uint32_t{64});
        write(CqLeafAddress + offsetof(pvrdma::RingState, rx), full);
        panic_if(submitCompletion(pvrdma::CompletionOpcode::Send,
                                  pvrdma::CompletionStatus::Success,
                                  0x1111) !=
                     pvrdma::CompletionSubmitResult::Queued,
                 "PVRDMA full CQ preflight was not queued");
        timingStage = TimingStage::CompletionCqFull;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      }
      case TimingStage::CompletionCqFull: {
        const auto cq_ring = read<pvrdma::Ring>(
            CqLeafAddress + offsetof(pvrdma::RingState, rx));
        panic_if(letoh(cq_ring.producerTail) != 64 ||
                     read<uint64_t>(CqLeafAddress + pvrdma::PageSize) ||
                     rdma->completionQueues.entries[1].armFlags !=
                         static_cast<uint32_t>(pvrdma::CqArmMode::Any) ||
                     statValue(prefix + "cqPublicationBackpressured") != 1 ||
                     statValue(prefix + "cqPublicationRejected") != 3 ||
                     statValue(prefix + "cqPublished") != 0 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA full CQ partially published a completion");
        auto &cq = rdma->completionQueues.entries[1];
        cq.producerTail = cq.consumerHead = 0;
        pvrdma::Ring malformed{};
        malformed.producerTail = htole(uint32_t{1});
        write(CqLeafAddress + offsetof(pvrdma::RingState, rx), malformed);
        panic_if(submitCompletion(pvrdma::CompletionOpcode::Receive,
                                  pvrdma::CompletionStatus::Success,
                                  0x2222, 32, 1) !=
                     pvrdma::CompletionSubmitResult::Queued,
                 "PVRDMA malformed CQ preflight was not queued");
        timingStage = TimingStage::CompletionMalformed;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      }
      case TimingStage::CompletionMalformed:
        panic_if(letoh(read<pvrdma::Ring>(CqLeafAddress +
                     offsetof(pvrdma::RingState, rx)).producerTail) != 1 ||
                     read<uint64_t>(CqLeafAddress + pvrdma::PageSize) ||
                     rdma->completionQueues.entries[1].armFlags !=
                         static_cast<uint32_t>(pvrdma::CqArmMode::Any) ||
                     (rdma->regs.pendingCauses &
                      pvrdma::InterruptCauseCompletion) ||
                     statValue(prefix + "cqPublicationRejected") != 4 ||
                     statValue(prefix + "cqPublicationBackpressured") != 1 ||
                     statValue(prefix + "cqPublished") != 0 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA malformed CQ partially published a completion");
        inform("PVRDMA completion rejection/backpressure test passed");
        exitSimLoop("PVRDMA completion error test passed");
        return;
      default:
        panic("Invalid PVRDMA completion error stage");
    }
}

void
PvrdmaTester::runCompletion()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA completion test requires atomic mode");
    const std::string prefix = "system.rdma.queues.";
    switch (timingStage) {
      case TimingStage::Configure: {
        configurePci();
        configureDsr();
        testCapabilities();
        write(RegisterBarAddress + pvrdma::RegControl,
              htole(static_cast<uint32_t>(
                  pvrdma::DeviceControl::Activate)), MmioFlags);
        createUserParentAtomic();
        createQueuePairAtomic(1);
        moveQueuePairToRtsAtomic();
        auto &cq = rdma->completionQueues.entries[1];
        cq.producerTail = cq.consumerHead = 126;
        pvrdma::Ring ring{};
        ring.producerTail = ring.consumerHead = htole(uint32_t{126});
        write(CqLeafAddress + offsetof(pvrdma::RingState, rx), ring);
        ringDoorbell(pvrdma::CqArmAnyAction, 1, true);
        panic_if(submitCompletion(pvrdma::CompletionOpcode::Send,
                                  pvrdma::CompletionStatus::Success,
                                  0x1111222233334444) !=
                     pvrdma::CompletionSubmitResult::Busy,
                 "PVRDMA completion ignored queued ARM observation");
        timingStage = TimingStage::CompletionSendDone;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      }
      case TimingStage::CompletionSendDone:
        panic_if(submitCompletion(pvrdma::CompletionOpcode::Send,
                                  pvrdma::CompletionStatus::Success,
                                  0x1111222233334444) !=
                     pvrdma::CompletionSubmitResult::Queued ||
                 submitCompletion(pvrdma::CompletionOpcode::Send,
                                  pvrdma::CompletionStatus::Success, 2) !=
                     pvrdma::CompletionSubmitResult::Busy,
                 "PVRDMA completion single-flight admission failed");
        timingStage = TimingStage::CompletionReceiveDone;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      case TimingStage::CompletionReceiveDone: {
        const auto cqe = read<pvrdma::CompletionQueueElement>(
            CqLeafAddress + pvrdma::PageSize + 62 * pvrdma::CqeSize);
        const auto cq = read<pvrdma::Ring>(
            CqLeafAddress + offsetof(pvrdma::RingState, rx));
        panic_if(letoh(cq.producerTail) != 127 ||
                     letoh(cqe.workRequestId) != 0x1111222233334444 ||
                     letoh(cqe.qp) != 1 || letoh(cqe.opcode) != 0 ||
                     letoh(cqe.status) != 0 ||
                     rdma->completionQueues.entries[1].armFlags !=
                         static_cast<uint32_t>(pvrdma::CqArmMode::Any) ||
                     (rdma->regs.pendingCauses &
                      pvrdma::InterruptCauseCompletion),
                 "PVRDMA SEND polling publication mismatch");
        panic_if(submitCompletion(pvrdma::CompletionOpcode::Receive,
                                  pvrdma::CompletionStatus::Success,
                                  0x5555666677778888, 128,
                                  0x123456) !=
                     pvrdma::CompletionSubmitResult::Queued,
                 "PVRDMA RECV success was not queued");
        timingStage = TimingStage::CompletionErrorDone;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      }
      case TimingStage::CompletionErrorDone: {
        const auto cqe = read<pvrdma::CompletionQueueElement>(
            CqLeafAddress + pvrdma::PageSize + 63 * pvrdma::CqeSize);
        const auto cq = read<pvrdma::Ring>(
            CqLeafAddress + offsetof(pvrdma::RingState, rx));
        panic_if(letoh(cq.producerTail) != 0 ||
                     letoh(cqe.workRequestId) != 0x5555666677778888 ||
                     letoh(cqe.opcode) != 128 || letoh(cqe.status) != 0 ||
                     letoh(cqe.byteLength) != 128 ||
                     letoh(cqe.sourceQp) != 0x123456,
                 "PVRDMA RECV success/wrap publication mismatch");
        panic_if(submitCompletion(pvrdma::CompletionOpcode::Receive,
                                  pvrdma::CompletionStatus::GeneralError,
                                  0x9999aaaabbbbcccc, 256,
                                  0x654321) !=
                     pvrdma::CompletionSubmitResult::Queued,
                 "PVRDMA RECV error was not queued");
        timingStage = TimingStage::CompletionReclaimed;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      }
      case TimingStage::CompletionReclaimed: {
        const auto cqe = read<pvrdma::CompletionQueueElement>(
            CqLeafAddress + pvrdma::PageSize);
        const auto cq = read<pvrdma::Ring>(
            CqLeafAddress + offsetof(pvrdma::RingState, rx));
        panic_if(letoh(cq.producerTail) != 1 ||
                     letoh(cqe.workRequestId) != 0x9999aaaabbbbcccc ||
                     letoh(cqe.opcode) != 128 || letoh(cqe.status) != 21 ||
                     letoh(cqe.byteLength) != 256 ||
                     letoh(cqe.sourceQp) != 0x654321 ||
                     statValue(prefix + "cqPublished") != 3 ||
                     statValue(prefix + "cqErrorPublished") != 1 ||
                     statValue(prefix + "cqOutstanding") != 3 ||
                     statValue(prefix + "cqPublicationRejected") != 0 ||
                     statValue(prefix + "cqPublicationBackpressured") != 0 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA RECV error/statistics mismatch");
        pvrdma::Ring consumed{};
        consumed.producerTail = consumed.consumerHead = htole(uint32_t{1});
        write(CqLeafAddress + offsetof(pvrdma::RingState, rx), consumed);
        ringDoorbell(pvrdma::CqPollAction, 1, true);
        timingStage = TimingStage::CompletionWrapDone;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      }
      case TimingStage::CompletionWrapDone:
        panic_if(rdma->completionQueues.entries[1].consumerHead != 1 ||
                     rdma->completionQueues.entries[1].armFlags !=
                         static_cast<uint32_t>(pvrdma::CqArmMode::Any) ||
                     statValue(prefix + "cqReclaimed") != 3 ||
                     statValue(prefix + "cqOutstanding") != 0 ||
                     (rdma->regs.pendingCauses &
                      pvrdma::InterruptCauseCompletion),
                 "PVRDMA polling reclamation/ARM preservation mismatch");
        panic_if(submitCompletion(pvrdma::CompletionOpcode::Send,
                                  pvrdma::CompletionStatus::GeneralError,
                                  0xddddeeeeffff0000) !=
                     pvrdma::CompletionSubmitResult::Queued,
                 "PVRDMA post-wrap SEND was not queued");
        timingStage = TimingStage::CompletionDestroyDone;
        schedule(testEvent, curTick() + microseconds(10));
        return;
      case TimingStage::CompletionDestroyDone: {
        const auto cqe = read<pvrdma::CompletionQueueElement>(
            CqLeafAddress + pvrdma::PageSize + pvrdma::CqeSize);
        const auto cq = read<pvrdma::Ring>(
            CqLeafAddress + offsetof(pvrdma::RingState, rx));
        panic_if(letoh(cq.producerTail) != 2 ||
                     letoh(cqe.workRequestId) != 0xddddeeeeffff0000 ||
                     letoh(cqe.opcode) != 0 || letoh(cqe.status) != 21 ||
                     statValue(prefix + "cqPublished") != 4 ||
                     statValue(prefix + "cqErrorPublished") != 2 ||
                     statValue(prefix + "cqOutstanding") != 1 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA post-wrap publication mismatch");
        destroyQp();
        destroyCq();
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                MmioFlags) != 0 ||
                     statValue(prefix + "cqOutstanding") != 0 ||
                     statValue(prefix + "cqResetDiscarded") != 1 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA CQ destroy conservation mismatch");
        write(RegisterBarAddress + pvrdma::RegControl,
              htole(static_cast<uint32_t>(
                  pvrdma::DeviceControl::Reset)), MmioFlags);
        panic_if(statValue(prefix + "cqResetDiscarded") != 1 ||
                     statValue(prefix + "conservationViolations") != 0,
                 "PVRDMA completion reset conservation mismatch");
        inform("PVRDMA synthetic polling completion test passed");
        exitSimLoop("PVRDMA completion publication test passed");
        return;
      }
      default:
        panic("Invalid PVRDMA completion test stage");
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
    auto &qp = rdma->queuePairs.entries[1];
    qp.finalReplay.valid = true;
    qp.finalReplay.qpGeneration = qp.generation;
    qp.finalReplay.localMac = {0x02, 0, 0, 0, 0, 1};
    std::copy_n(qp.attributes.addressHandle.destinationMac,
                qp.finalReplay.remoteMac.size(),
                qp.finalReplay.remoteMac.begin());
    qp.finalReplay.localQpn = qp.qpn;
    qp.finalReplay.remoteQpn = qp.attributes.destinationQpNumber;
    qp.finalReplay.finalPsn = 0x12344;
    qp.finalReplay.messageId = 0xfeed;
    qp.finalReplay.totalLength = 1;
    qp.finalReplay.segmentCount = 1;
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
                 letoh(response.queryQp.attributes.sendPsn) != 0x54321 ||
                 !rdma->queuePairs.entries[1].finalReplay.valid ||
                 rdma->queuePairs.entries[1].finalReplay.localMac[0] != 2 ||
                 rdma->queuePairs.entries[1].finalReplay.localMac[5] != 1 ||
                 rdma->queuePairs.entries[1].finalReplay.messageId !=
                     0xfeed ||
                 rdma->queuePairs.entries[1].finalReplay.finalPsn !=
                     0x12344,
             "Restored PVRDMA QP lost RTS/replay attributes");
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
PvrdmaTester::testCheckpointCompletionSave()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA completion checkpoint setup requires atomic mode");
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
        timingStage = TimingStage::CompletionSendDone;
        schedule(testEvent, curTick() + microseconds(10));
        return;
    }
    if (timingStage == TimingStage::CompletionSendDone) {
        panic_if(submitCompletion(pvrdma::CompletionOpcode::Receive,
                                  pvrdma::CompletionStatus::Success,
                                  0xc0dec0dec0dec0de, 64,
                                  0x123456) !=
                     pvrdma::CompletionSubmitResult::Queued,
                 "PVRDMA checkpoint completion was not queued");
        timingStage = TimingStage::CompletionDone;
        schedule(testEvent, curTick() + microseconds(10));
        return;
    }
    panic_if(timingStage != TimingStage::CompletionDone ||
                 rdma->completionBusy(),
             "Invalid PVRDMA completion checkpoint stage");
    const auto cq = read<pvrdma::Ring>(
        CqLeafAddress + offsetof(pvrdma::RingState, rx));
    panic_if(letoh(cq.producerTail) != 1,
             "PVRDMA completion checkpoint missed publication");
    inform("PVRDMA nonempty completion checkpoint ready");
    exitSimLoop("PVRDMA completion checkpoint save ready");
}

void
PvrdmaTester::testCheckpointCompletionRestore()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA completion checkpoint restore requires atomic mode");
    const std::string prefix = "system.rdma.queues.";
    const auto cqe = read<pvrdma::CompletionQueueElement>(
        CqLeafAddress + pvrdma::PageSize);
    const auto cq = read<pvrdma::Ring>(
        CqLeafAddress + offsetof(pvrdma::RingState, rx));
    panic_if(letoh(cqe.workRequestId) != 0xc0dec0dec0dec0de ||
                 letoh(cqe.opcode) != 128 || letoh(cqe.status) != 0 ||
                 letoh(cqe.byteLength) != 64 ||
                 letoh(cqe.sourceQp) != 0x123456 ||
                 letoh(cq.producerTail) != 1 ||
                 statValue(prefix + "cqOutstanding") != 1 ||
                 statValue(prefix + "cqOutstandingAtReset") != 1,
             "PVRDMA completion checkpoint restore duplicated/lost data");
    write(RegisterBarAddress + pvrdma::RegControl,
          htole(static_cast<uint32_t>(pvrdma::DeviceControl::Reset)),
          MmioFlags);
    panic_if(statValue(prefix + "cqResetDiscarded") != 1 ||
                 statValue(prefix + "conservationViolations") != 0,
             "PVRDMA restored completion reset conservation failed");
    inform("PVRDMA nonempty completion checkpoint restored");
    exitSimLoop("PVRDMA completion checkpoint restored");
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
        postObservedRings(0, 7);
        ringDoorbell(pvrdma::RqDoorbellAction);
        ringDoorbell(pvrdma::CqArmAnyAction, 1, true);
        timingStage = TimingStage::CheckpointObservationReady;
        schedule(testEvent, curTick() + microseconds(10));
        return;
    }
    panic_if(timingStage != TimingStage::CheckpointObservationReady,
             "Invalid PVRDMA observation checkpoint stage");
    const std::string prefix = "system.rdma.queues.";
    panic_if(statValue(prefix + "sqOutstanding") != 0 ||
                 statValue(prefix + "rqAvailable") != 7,
             "PVRDMA checkpoint missed queue observations");
    pvrdma::Ring cq_ring{};
    cq_ring.producerTail = htole(uint32_t{3});
    write(CqLeafAddress + offsetof(pvrdma::RingState, rx), cq_ring);
    rdma->completionQueues.entries[1].producerTail = 3;
    rdma->refreshQueueGauges();
    rdma->queueStatsReset();
    inform("PVRDMA nonempty queue observation checkpoint ready");
    exitSimLoop("PVRDMA observation checkpoint save ready");
}

void
PvrdmaTester::testCheckpointObservationRestore()
{
    panic_if(!system->isAtomicMode(),
             "PVRDMA observation checkpoint restore requires atomic mode");
    const std::string prefix = "system.rdma.queues.";
    panic_if(statValue(prefix + "sqOutstanding") != 0 ||
                 statValue(prefix + "rqAvailable") != 7 ||
                 statValue(prefix + "cqOutstanding") != 3 ||
                 statValue(prefix + "sqOutstandingAtReset") != 0 ||
                 statValue(prefix + "rqAvailableAtReset") != 7 ||
                 statValue(prefix + "cqOutstandingAtReset") != 3,
             "PVRDMA restored queue baselines were incorrect");
    destroyQp();
    panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0 ||
                 statValue(prefix + "sqOutstanding") != 0 ||
                 statValue(prefix + "rqAvailable") != 0 ||
                 statValue(prefix + "sqResetDiscarded") != 0 ||
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
PvrdmaTester::setupPairEndpoint(
    Pvrdma &device, Addr qp_page, Addr cq_page, Addr mr_page,
    const pvrdma::transport::MacAddress &remote_mac,
    uint32_t send_psn, uint32_t receive_psn)
{
    device.controlState = pvrdma::ControlState::Active;
    device.regs.error = 0;
    device.objects.contextUar[1] = 1;
    device.objects.contextPdChildren[1] = 1;
    device.objects.contextCqChildren[1] = 1;
    device.objects.pdAllocated[1] = 1;
    device.objects.pdParent[1] = 1;
    device.objects.pdChildren[1] = 2;

    auto &cq = device.completionQueues.entries[1];
    cq.valid = true;
    cq.cqHandle = 1;
    cq.contextHandle = 1;
    cq.uar = 1;
    cq.cqe = 8;
    cq.qpReferences = 2;
    cq.pages = {cq_page, cq_page + 0x10000};

    auto &qp = device.queuePairs.entries[1];
    qp.valid = true;
    qp.qpHandle = qp.qpn = 1;
    qp.pdHandle = qp.sendCqHandle = qp.recvCqHandle = 1;
    qp.contextHandle = qp.uar = 1;
    qp.sendChunks = qp.recvChunks = 1;
    qp.totalChunks = 3;
    qp.state = pvrdma::QpState::ReadyToSend;
    qp.capabilities.maxSendWr = qp.capabilities.maxRecvWr = 8;
    qp.capabilities.maxSendSge = qp.capabilities.maxRecvSge = 1;
    qp.attributes.qpState = qp.attributes.currentQpState = qp.state;
    qp.attributes.pathMtu = pvrdma::Mtu::Mtu1024;
    qp.attributes.pathMigrationState = pvrdma::MigrationState::Migrated;
    qp.attributes.destinationQpNumber = 1;
    qp.attributes.qpAccessFlags = pvrdma::AccessLocalWrite;
    qp.attributes.sendPsn = send_psn;
    qp.attributes.receivePsn = receive_psn;
    qp.attributes.portNumber = 1;
    qp.attributes.addressHandle.portNumber = 1;
    qp.attributes.capabilities = qp.capabilities;
    std::copy(remote_mac.begin(), remote_mac.end(),
              qp.attributes.addressHandle.destinationMac);
    qp.pages = {qp_page, qp_page + 0x10000, qp_page + 0x20000};

    auto &mr = device.memoryRegions.entries[1];
    mr.valid = true;
    mr.mrHandle = mr.lkey = mr.rkey = 1;
    mr.pdHandle = 1;
    mr.accessFlags = pvrdma::AccessLocalWrite;
    mr.start = mr_page;
    mr.length = 16 * pvrdma::PageSize;
    mr.end = mr.start + mr.length;
    for (uint32_t i = 0; i < 16; ++i)
        mr.pages.push_back(mr_page + uint64_t{i} * pvrdma::PageSize);
    device.receivePayloadDmaStarts = 0;
    device.queueStatsReset();
}

void
PvrdmaTester::setupPair()
{
    configurePci();
    write(PeerPciConfigAddress + PCI0_BASE_ADDR0,
          htole(static_cast<uint32_t>(PeerMsixBarAddress)), MmioFlags);
    write(PeerPciConfigAddress + PCI0_BASE_ADDR1,
          htole(static_cast<uint32_t>(PeerRegisterBarAddress)), MmioFlags);
    write(PeerPciConfigAddress + PCI0_BASE_ADDR2,
          htole(static_cast<uint32_t>(PeerUarBarAddress)), MmioFlags);
    write(PeerPciConfigAddress + PCI_COMMAND,
          htole(static_cast<uint16_t>(PCI_CMD_MSE | PCI_CMD_BME)), MmioFlags);

    const pvrdma::transport::MacAddress sender_mac =
        {0x02, 0, 0, 0, 0, 1};
    const pvrdma::transport::MacAddress receiver_mac =
        {0x02, 0, 0, 0, 0, 2};
    setupPairEndpoint(*rdma, PairSenderQp, PairSenderCq,
                      PairSenderPayload, receiver_mac, 0x100, 0x200);
    setupPairEndpoint(*peerRdma, PairReceiverQp, PairReceiverCq,
                      PairReceiverPayload, sender_mac, 0x200, 0x100);

    write(RegisterBarAddress + pvrdma::RegMacLow,
          htole(uint32_t{0x44332211}), MmioFlags);
    write(RegisterBarAddress + pvrdma::RegMacHigh,
          htole(uint32_t{0x6655}), MmioFlags);
    panic_if(letoh(read<uint32_t>(RegisterBarAddress + pvrdma::RegMacLow,
                                  MmioFlags)) != 0x44332211 ||
                 letoh(read<uint32_t>(RegisterBarAddress + pvrdma::RegMacHigh,
                                      MmioFlags)) != 0x6655,
             "PVRDMA idle MAC writes did not take effect");
    write(RegisterBarAddress + pvrdma::RegMacLow,
          htole(uint32_t{0x00000002}), MmioFlags);
    write(RegisterBarAddress + pvrdma::RegMacHigh,
          htole(uint32_t{0x0100}), MmioFlags);

    std::array<uint8_t, 512> payload{};
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = i ^ 0x5a;
    write(PairSenderPayload, payload);
    write(PairReceiverPayload, std::array<uint8_t, 512>{});

    for (uint32_t i = 0; i < 8; ++i) {
        pvrdma::SendWqeHeader send{};
        send.workRequestId = htole(uint64_t{0x1000 + i});
        send.numSge = htole(uint32_t{1});
        send.opcode = htole(static_cast<uint32_t>(
            pvrdma::WorkRequestOpcode::Send));
        send.sendFlags = htole(i == 1 ? 0U : pvrdma::SendSignaled);
        pvrdma::Sge send_sge{
            htole(static_cast<uint64_t>(PairSenderPayload + 64 * i)),
            htole(i == 2 ? 0U : i == 5 ? 1025U : 64U),
            htole(uint32_t{1})};
        std::array<uint8_t, pvrdma::SqStride> send_slot{};
        std::memcpy(send_slot.data(), &send, sizeof(send));
        std::memcpy(send_slot.data() + sizeof(send), &send_sge,
                    sizeof(send_sge));
        write(PairSenderSq + i * pvrdma::SqStride, send_slot);

        if (i >= 4)
            continue;
        pvrdma::ReceiveWqeHeader receive{};
        receive.workRequestId = htole(uint64_t{0x2000 + i});
        receive.numSge = htole(uint32_t{1});
        pvrdma::Sge receive_sge{
            htole(static_cast<uint64_t>(PairReceiverPayload + 64 * i)),
            htole(i == 3 ? 32U : 64U), htole(uint32_t{1})};
        std::array<uint8_t, pvrdma::RqStride> receive_slot{};
        std::memcpy(receive_slot.data(), &receive, sizeof(receive));
        std::memcpy(receive_slot.data() + sizeof(receive), &receive_sge,
                    sizeof(receive_sge));
        write(PairReceiverRq + i * pvrdma::RqStride, receive_slot);
    }

    peerRdma->commandSlotAddress = CommandAddress;
    peerRdma->responseSlotAddress = ResponseAddress;
    peerRdma->commandSlotDmaAddress = peerRdma->pciToDma(CommandAddress);
    peerRdma->responseSlotDmaAddress = peerRdma->pciToDma(ResponseAddress);

    auto &receiver_qp = peerRdma->queuePairs.entries[1];
    receiver_qp.state = pvrdma::QpState::Init;
    receiver_qp.attributes = {};
    receiver_qp.attributes.qpState = receiver_qp.attributes.currentQpState =
        receiver_qp.state;
    receiver_qp.attributes.qpAccessFlags = pvrdma::AccessLocalWrite;
    receiver_qp.attributes.portNumber = 1;
    receiver_qp.attributes.capabilities = receiver_qp.capabilities;
    pvrdma::RingState receiver_rings{};
    receiver_rings.rx.producerTail = htole(uint32_t{3});
    write(PairReceiverQp, receiver_rings);
    write(PeerUarBarAddress + pvrdma::UarPageSize,
          htole(pvrdma::RqDoorbellAction | 1), MmioFlags);

    pvrdma::CommandRequest request{};
    request.header.response = htole(uint64_t{0x6060606060606060});
    request.header.command = htole(
        static_cast<uint32_t>(pvrdma::Command::ModifyQp));
    request.modifyQp.qpHandle = htole(uint32_t{1});
    request.modifyQp.attributeMask = htole(
        pvrdma::QpAttrState | pvrdma::QpAttrAddressVector |
        pvrdma::QpAttrPathMtu | pvrdma::QpAttrReceivePsn |
        pvrdma::QpAttrMaxDestReadAtomic | pvrdma::QpAttrMinRnrTimer |
        pvrdma::QpAttrDestinationQpn);
    auto &attrs = request.modifyQp.attributes;
    attrs.qpState = static_cast<pvrdma::QpState>(htole(
        static_cast<uint32_t>(pvrdma::QpState::ReadyToReceive)));
    attrs.pathMtu = static_cast<pvrdma::Mtu>(htole(
        static_cast<uint32_t>(pvrdma::Mtu::Mtu1024)));
    attrs.destinationQpNumber = htole(uint32_t{1});
    attrs.receivePsn = htole(uint32_t{0x100});
    attrs.maxDestinationReadAtomic = 1;
    attrs.minRnrTimer = 12;
    attrs.addressHandle.portNumber = 1;
    std::copy(sender_mac.begin(), sender_mac.end(),
              attrs.addressHandle.destinationMac);
    write(CommandAddress, request);
    write(ResponseAddress, pvrdma::CommandResponse{});
    write(PeerRegisterBarAddress + pvrdma::RegRequest,
          uint32_t{0}, MmioFlags);
    panic_if(read<uint32_t>(PeerRegisterBarAddress + pvrdma::RegError,
                            MmioFlags) != 0,
             "PVRDMA immediate INIT-to-RTR command was rejected");
}

void
PvrdmaTester::setupReliabilityPair()
{
    configurePci();
    write(PeerPciConfigAddress + PCI0_BASE_ADDR0,
          htole(static_cast<uint32_t>(PeerMsixBarAddress)), MmioFlags);
    write(PeerPciConfigAddress + PCI0_BASE_ADDR1,
          htole(static_cast<uint32_t>(PeerRegisterBarAddress)), MmioFlags);
    write(PeerPciConfigAddress + PCI0_BASE_ADDR2,
          htole(static_cast<uint32_t>(PeerUarBarAddress)), MmioFlags);
    write(PeerPciConfigAddress + PCI_COMMAND,
          htole(static_cast<uint16_t>(PCI_CMD_MSE | PCI_CMD_BME)), MmioFlags);

    const pvrdma::transport::MacAddress sender_mac =
        {0x02, 0, 0, 0, 0, 1};
    const pvrdma::transport::MacAddress receiver_mac =
        {0x02, 0, 0, 0, 0, 2};
    setupPairEndpoint(*rdma, PairSenderQp, PairSenderCq,
                      PairSenderPayload, receiver_mac, 0x100, 0x200);
    setupPairEndpoint(*peerRdma, PairReceiverQp, PairReceiverCq,
                      PairReceiverPayload, sender_mac, 0x200, 0x100);
    rdma->queuePairs.entries[1].attributes.timeout = 8;
    rdma->queuePairs.entries[1].attributes.retryCount = 2;
    rdma->queuePairs.entries[1].attributes.rnrRetry = 2;
    peerRdma->queuePairs.entries[1].attributes.minRnrTimer = 1;
}

void
PvrdmaTester::postReliabilityReceive(uint32_t length)
{
    auto &qp = peerRdma->queuePairs.entries[1];
    const uint32_t slot = qp.rqProducerTail & 7;
    pvrdma::ReceiveWqeHeader receive{};
    receive.workRequestId = htole(uint64_t{0x9000 + reliabilityCase});
    receive.numSge = htole(uint32_t{1});
    const pvrdma::Sge sge{htole(static_cast<uint64_t>(PairReceiverPayload)),
                          htole(length), htole(uint32_t{1})};
    std::array<uint8_t, pvrdma::RqStride> bytes{};
    std::memcpy(bytes.data(), &receive, sizeof(receive));
    std::memcpy(bytes.data() + sizeof(receive), &sge, sizeof(sge));
    write(PairReceiverRq + slot * pvrdma::RqStride, bytes);
    auto rings = read<pvrdma::RingState>(PairReceiverQp);
    rings.rx.producerTail = htole(pvrdma::ringAdvance(
        qp.rqProducerTail, qp.capabilities.maxRecvWr));
    write(PairReceiverQp, rings);
    write(PeerUarBarAddress + pvrdma::UarPageSize,
          htole(pvrdma::RqDoorbellAction | 1), MmioFlags);
}

void
PvrdmaTester::postReliabilitySend(uint32_t length)
{
    auto &qp = rdma->queuePairs.entries[1];
    const uint32_t slot = qp.sqProducerTail & 7;
    std::array<uint8_t, 4097> payload{};
    for (size_t i = 0; i < length; ++i)
        payload[i] = static_cast<uint8_t>(i ^ reliabilityCase ^ 0x5a);
    write(PairSenderPayload, payload);
    pvrdma::SendWqeHeader send{};
    send.workRequestId = htole(uint64_t{0x8000 + reliabilityCase});
    send.numSge = htole(uint32_t{1});
    send.opcode = htole(static_cast<uint32_t>(
        pvrdma::WorkRequestOpcode::Send));
    send.sendFlags = htole(reliabilityCase == 6 ? 0U :
                           pvrdma::SendSignaled);
    const pvrdma::Sge sge{htole(static_cast<uint64_t>(PairSenderPayload)),
                          htole(length), htole(uint32_t{1})};
    std::array<uint8_t, pvrdma::SqStride> bytes{};
    std::memcpy(bytes.data(), &send, sizeof(send));
    std::memcpy(bytes.data() + sizeof(send), &sge, sizeof(sge));
    write(PairSenderSq + slot * pvrdma::SqStride, bytes);
    auto rings = read<pvrdma::RingState>(PairSenderQp);
    rings.tx.producerTail = htole(pvrdma::ringAdvance(
        qp.sqProducerTail, qp.capabilities.maxSendWr));
    write(PairSenderQp, rings);
    write(UarBarAddress + pvrdma::UarPageSize,
          htole(pvrdma::SqDoorbellAction | 1), MmioFlags);
}

void
PvrdmaTester::runSemanticPair()
{
    static constexpr std::array<uint32_t, 6> Lengths =
        {0, 1, 64, 1024, 1025, 4097};
    static constexpr uint32_t InitialPsn = 0x00fffffb;
    const pvrdma::transport::MacAddress sender_mac =
        {0x02, 0, 0, 0, 0, 1};
    const pvrdma::transport::MacAddress receiver_mac =
        {0x02, 0, 0, 0, 0, 2};
    const auto expected_psn = [&](size_t last) {
        uint32_t psn = InitialPsn;
        for (size_t i = 0; i <= last; ++i) {
            uint16_t segments = 0;
            panic_if(!pvrdma::segmentGeometry(Lengths[i], segments),
                     "PVRDMA semantic length was not segmentable");
            while (segments--)
                psn = pvrdma::advancePsn(psn);
        }
        return psn;
    };
    const auto clear_receive = [&] {
        std::array<uint8_t, 4097> marker;
        marker.fill(0xa5);
        write(PairReceiverPayload, marker);
    };

    switch (timingStage) {
      case TimingStage::Configure:
        setupReliabilityPair();
        reliabilityCase = 0;
        rdma->queuePairs.entries[1].attributes.sendPsn = InitialPsn;
        peerRdma->queuePairs.entries[1].attributes.receivePsn = InitialPsn;
        clear_receive();
        postReliabilityReceive(Lengths[0]);
        timingStage = TimingStage::SemanticPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      case TimingStage::SemanticPostSq:
        postReliabilitySend(Lengths[reliabilityCase]);
        timingStage = TimingStage::SemanticVerify;
        schedule(testEvent, curTick() + microseconds(1000));
        return;
      case TimingStage::SemanticVerify: {
        const uint32_t length = Lengths[reliabilityCase];
        const uint32_t completed = reliabilityCase + 1;
        const auto received = read<std::array<uint8_t, 4097>>(
            PairReceiverPayload);
        for (size_t i = 0; i < received.size(); ++i) {
            const uint8_t expected = i < length ?
                static_cast<uint8_t>(i ^ reliabilityCase ^ 0x5a) : 0xa5;
            panic_if(received[i] != expected,
                     "PVRDMA semantic payload mismatch case %u byte %u",
                     reliabilityCase, i);
        }
        const auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto sender_cq = read<pvrdma::Ring>(PairSenderCq +
            offsetof(pvrdma::RingState, rx));
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        uint64_t expected_bytes = 0;
        uint64_t completed_bytes = 0;
        for (size_t i = 0; i <= reliabilityCase; ++i) {
            const auto send = read<pvrdma::CompletionQueueElement>(
                PairSenderCqe + i * pvrdma::CqeSize);
            const auto receive = read<pvrdma::CompletionQueueElement>(
                PairReceiverCqe + i * pvrdma::CqeSize);
            expected_bytes += Lengths[i];
            completed_bytes += letoh(receive.byteLength);
            panic_if(letoh(send.workRequestId) != 0x8000 + i ||
                         letoh(send.opcode) != 0 || letoh(send.status) != 0 ||
                         letoh(send.byteLength) != 0 || letoh(send.qp) != 1 ||
                         letoh(send.sourceQp) != 0 ||
                         letoh(receive.workRequestId) != 0x9000 + i ||
                         letoh(receive.opcode) != 128 ||
                         letoh(receive.status) != 0 ||
                         letoh(receive.byteLength) != Lengths[i] ||
                         letoh(receive.qp) != 1 ||
                         letoh(receive.sourceQp) != 1,
                     "PVRDMA semantic CQE mismatch case %u", i);
        }
        panic_if(letoh(sender_ring.tx.consumerHead) != completed ||
                     letoh(receiver_ring.rx.consumerHead) != completed ||
                     letoh(sender_cq.producerTail) != completed ||
                     letoh(receiver_cq.producerTail) != completed ||
                     rdma->queuePairs.entries[1].sqConsumerHead != completed ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead !=
                         completed ||
                     rdma->queuePairs.entries[1].attributes.sendPsn !=
                         expected_psn(reliabilityCase) ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         expected_psn(reliabilityCase) ||
                     completed_bytes != expected_bytes ||
                     rdma->queueStats.sqPosted.value() != completed ||
                     rdma->queueStats.sqConsumed.value() != completed ||
                     rdma->queueStats.cqPublished.value() != completed ||
                     rdma->queueStats.cqErrorPublished.value() != 0 ||
                     peerRdma->queueStats.rqPosted.value() != completed ||
                     peerRdma->queueStats.rqConsumed.value() != completed ||
                     peerRdma->queueStats.cqPublished.value() != completed ||
                     peerRdma->queueStats.cqErrorPublished.value() != 0 ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     rdma->queueStats.conservationViolations.value() ||
                     peerRdma->queueStats.conservationViolations.value(),
                 "PVRDMA semantic accounting mismatch case %u",
                 reliabilityCase);
        if (++reliabilityCase < Lengths.size()) {
            clear_receive();
            postReliabilityReceive(Lengths[reliabilityCase]);
            timingStage = TimingStage::SemanticPostSq;
            schedule(testEvent, curTick() + microseconds(20));
            return;
        }

        clear_receive();
        postReliabilityReceive(128);
        timingStage = TimingStage::SemanticMalformedPost;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      }
      case TimingStage::SemanticMalformedPost: {
        panic_if(peerRdma->queuePairs.entries[1].rqProducerTail != 7,
                 "PVRDMA semantic malformed RQ was not observed");
        const std::array<uint8_t, 64> payload = {0x5a};
        pvrdma::transport::Frame malformed;
        malformed.kind = pvrdma::transport::Kind::Data;
        malformed.flags = pvrdma::transport::First;
        malformed.sourceQpn = malformed.destinationQpn = 1;
        malformed.psn = expected_psn(Lengths.size() - 1);
        malformed.messageId = 0x100000007;
        malformed.totalLength = 128;
        malformed.segmentCount = 2;
        malformed.payload = {payload.data(), payload.size()};
        panic_if(!rdma->interface.sendPacket(transportPacket(
                     malformed, sender_mac, receiver_mac)),
                 "PVRDMA semantic EtherLink rejected malformed DATA");
        timingStage = TimingStage::SemanticMalformedVerify;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::SemanticMalformedVerify: {
        const auto marker = read<std::array<uint8_t, 4097>>(
            PairReceiverPayload);
        const auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto sender_cq = read<pvrdma::Ring>(PairSenderCq +
            offsetof(pvrdma::RingState, rx));
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        const uint32_t psn = expected_psn(Lengths.size() - 1);
        panic_if(std::any_of(marker.begin(), marker.end(),
                            [](uint8_t byte) { return byte != 0xa5; }) ||
                     letoh(sender_ring.tx.consumerHead) != Lengths.size() ||
                     letoh(receiver_ring.rx.producerTail) != 7 ||
                     letoh(receiver_ring.rx.consumerHead) != Lengths.size() ||
                     peerRdma->queuePairs.entries[1].rqProducerTail != 7 ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead !=
                         Lengths.size() ||
                     letoh(sender_cq.producerTail) != Lengths.size() ||
                     letoh(receiver_cq.producerTail) != Lengths.size() ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != psn ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         psn ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     rdma->queueStats.conservationViolations.value() ||
                     peerRdma->queueStats.conservationViolations.value(),
                 "PVRDMA malformed DATA changed receive-visible state");

        postReliabilitySend(128);
        timingStage = TimingStage::SemanticMalformedValidVerify;
        schedule(testEvent, curTick() + microseconds(1000));
        return;
      }
      case TimingStage::SemanticMalformedValidVerify: {
        const auto receive = read<pvrdma::CompletionQueueElement>(
            PairReceiverCqe + Lengths.size() * pvrdma::CqeSize);
        const auto marker = read<std::array<uint8_t, 4097>>(
            PairReceiverPayload);
        const uint32_t next_psn = pvrdma::advancePsn(
            expected_psn(Lengths.size() - 1));
        for (size_t i = 0; i < marker.size(); ++i) {
            const uint8_t expected = i < 128 ?
                static_cast<uint8_t>(i ^ reliabilityCase ^ 0x5a) : 0xa5;
            panic_if(marker[i] != expected,
                     "PVRDMA malformed follow-up payload mismatch byte %u",
                     i);
        }
        panic_if(peerRdma->queuePairs.entries[1].rqConsumerHead != 7 ||
                     peerRdma->completionQueues.entries[1].producerTail != 7 ||
                     letoh(receive.workRequestId) != 0x9006 ||
                     letoh(receive.opcode) != 128 ||
                     letoh(receive.status) != 0 ||
                     letoh(receive.byteLength) != 128 ||
                     letoh(receive.qp) != 1 || letoh(receive.sourceQp) != 1 ||
                     rdma->queuePairs.entries[1].attributes.sendPsn !=
                         next_psn ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         next_psn ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     rdma->queueStats.conservationViolations.value() ||
                     peerRdma->queueStats.conservationViolations.value(),
                 "PVRDMA malformed DATA consumed or damaged posted RQ");
        clear_receive();
        rdma->queuePairs.entries[1].attributes.rnrRetry = 0;
        ++reliabilityCase;
        postReliabilitySend(64);
        timingStage = TimingStage::SemanticRnrVerify;
        schedule(testEvent, curTick() + microseconds(1000));
        return;
      }
      case TimingStage::SemanticRnrVerify: {
        const auto error = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe + Lengths.size() * pvrdma::CqeSize);
        const auto marker = read<std::array<uint8_t, 4097>>(
            PairReceiverPayload);
        const auto untouched = read<pvrdma::CompletionQueueElement>(
            PairReceiverCqe + (Lengths.size() + 1) * pvrdma::CqeSize);
        const auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto sender_cq = read<pvrdma::Ring>(PairSenderCq +
            offsetof(pvrdma::RingState, rx));
        const uint32_t psn = pvrdma::advancePsn(
            expected_psn(Lengths.size() - 1));
        panic_if(rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Error ||
                     rdma->queuePairs.entries[1].sqConsumerHead != 8 ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead != 7 ||
                     rdma->completionQueues.entries[1].producerTail != 7 ||
                     peerRdma->completionQueues.entries[1].producerTail != 7 ||
                     letoh(sender_ring.tx.consumerHead) != 8 ||
                     letoh(sender_cq.producerTail) != 7 ||
                     letoh(error.workRequestId) != 0x8007 ||
                     letoh(error.opcode) != 0 || letoh(error.status) != 13 ||
                     letoh(error.byteLength) != 0 || letoh(error.qp) != 1 ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != psn ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         psn ||
                     rdma->queueStats.sqPosted.value() != 8 ||
                     rdma->queueStats.sqConsumed.value() != 8 ||
                     rdma->queueStats.cqPublished.value() != 7 ||
                     rdma->queueStats.cqErrorPublished.value() != 1 ||
                     peerRdma->queueStats.rqPosted.value() != 7 ||
                     peerRdma->queueStats.rqConsumed.value() != 7 ||
                     peerRdma->queueStats.cqPublished.value() != 7 ||
                     std::any_of(marker.begin(), marker.end(),
                                 [](uint8_t byte) { return byte != 0xa5; }) ||
                     letoh(untouched.workRequestId) ||
                     letoh(untouched.status) ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     rdma->queueStats.conservationViolations.value() ||
                     peerRdma->queueStats.conservationViolations.value(),
                 "PVRDMA no-RQ/RNR accounting mismatch");

        pvrdma::CommandRequest request{};
        request.header.command = htole(static_cast<uint32_t>(
            pvrdma::Command::ModifyQp));
        request.modifyQp.qpHandle = htole(uint32_t{1});
        request.modifyQp.attributeMask = htole(pvrdma::QpAttrState);
        request.modifyQp.attributes.qpState = pvrdma::QpState::Reset;
        pvrdma::CommandResponse response{};
        panic_if(pvrdma::detail::modifyQp(
                     request, response, rdma->queuePairs).error,
                 "PVRDMA RNR terminal reset failed");
        write(PairSenderQp, pvrdma::RingState{});
        auto &qp = rdma->queuePairs.entries[1];
        qp.state = pvrdma::QpState::ReadyToSend;
        qp.attributes.qpState = qp.attributes.currentQpState = qp.state;
        qp.attributes.pathMtu = pvrdma::Mtu::Mtu1024;
        qp.attributes.destinationQpNumber = 1;
        qp.attributes.qpAccessFlags = pvrdma::AccessLocalWrite;
        qp.attributes.sendPsn = psn;
        qp.attributes.portNumber = 1;
        qp.attributes.addressHandle.portNumber = 1;
        qp.attributes.capabilities = qp.capabilities;
        std::copy(receiver_mac.begin(), receiver_mac.end(),
                  qp.attributes.addressHandle.destinationMac);
        reliabilityCase = 8;
        postReliabilityReceive(32);
        timingStage = TimingStage::SemanticShortPost;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      }
      case TimingStage::SemanticShortPost:
        panic_if(peerRdma->queuePairs.entries[1].rqProducerTail != 8,
                 "PVRDMA semantic short RQ was not observed");
        postReliabilitySend(64);
        timingStage = TimingStage::SemanticShortVerify;
        schedule(testEvent, curTick() + microseconds(1000));
        return;
      case TimingStage::SemanticShortVerify: {
        const auto error = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe + 7 * pvrdma::CqeSize);
        const auto marker = read<std::array<uint8_t, 4097>>(
            PairReceiverPayload);
        const auto untouched = read<pvrdma::CompletionQueueElement>(
            PairReceiverCqe + (Lengths.size() + 1) * pvrdma::CqeSize);
        const auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto sender_cq = read<pvrdma::Ring>(PairSenderCq +
            offsetof(pvrdma::RingState, rx));
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        const uint32_t psn = pvrdma::advancePsn(
            expected_psn(Lengths.size() - 1));
        panic_if(rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Error ||
                     rdma->queuePairs.entries[1].sqConsumerHead != 1 ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead != 7 ||
                     rdma->completionQueues.entries[1].producerTail != 8 ||
                     letoh(sender_ring.tx.consumerHead) != 1 ||
                     letoh(sender_cq.producerTail) != 8 ||
                     letoh(receiver_cq.producerTail) != 7 ||
                     letoh(error.workRequestId) != 0x8008 ||
                     letoh(error.opcode) != 0 || letoh(error.status) != 21 ||
                     letoh(error.byteLength) != 0 || letoh(error.qp) != 1 ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != psn ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         psn ||
                     rdma->queueStats.sqPosted.value() != 9 ||
                     rdma->queueStats.sqConsumed.value() != 9 ||
                     rdma->queueStats.cqPublished.value() != 8 ||
                     rdma->queueStats.cqErrorPublished.value() != 2 ||
                     peerRdma->queueStats.rqPosted.value() != 8 ||
                     peerRdma->queueStats.rqConsumed.value() != 7 ||
                     peerRdma->queueStats.cqPublished.value() != 7 ||
                     std::any_of(marker.begin(), marker.end(),
                                 [](uint8_t byte) { return byte != 0xa5; }) ||
                     letoh(untouched.workRequestId) ||
                     letoh(untouched.status) ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     rdma->queueStats.conservationViolations.value() ||
                     peerRdma->queueStats.conservationViolations.value(),
                 "PVRDMA terminal ERROR accounting mismatch");
        pvrdma::CommandRequest request{};
        request.header.command = htole(static_cast<uint32_t>(
            pvrdma::Command::ModifyQp));
        request.modifyQp.qpHandle = htole(uint32_t{1});
        request.modifyQp.attributeMask = htole(pvrdma::QpAttrState);
        request.modifyQp.attributes.qpState = pvrdma::QpState::Reset;
        pvrdma::CommandResponse response{};
        panic_if(pvrdma::detail::modifyQp(
                     request, response, rdma->queuePairs).error ||
                     rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Reset ||
                     rdma->queuePairs.entries[1].sqProducerTail ||
                     rdma->queuePairs.entries[1].sqConsumerHead ||
                     rdma->queuePairs.entries[1].finalReplay.valid,
                 "PVRDMA terminal ERROR-to-Reset recovery failed");
        inform("PVRDMA direct EtherLink semantic pair test passed");
        exitSimLoop("PVRDMA semantic pair test passed");
        return;
      }
      default:
        panic("Invalid PVRDMA semantic pair stage");
    }
}

void
PvrdmaTester::runReliabilityPair()
{
    static constexpr std::array<uint32_t, 7> Lengths =
        {0, 1, 1023, 1024, 1025, 4097, 2049};
    using Direction = PvrdmaTestLink::Direction;
    using FrameId = PvrdmaTestLink::FrameId;
    const auto frame_id = [](Direction direction,
                             pvrdma::transport::Kind kind, uint32_t psn,
                             uint64_t message, uint16_t segment) {
        return FrameId{direction, kind, psn, message, segment};
    };

    switch (timingStage) {
      case TimingStage::Configure:
        setupReliabilityPair();
        reliabilityCase = 0;
        postReliabilityReceive(Lengths[reliabilityCase]);
        timingStage = TimingStage::ReliabilityPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      case TimingStage::ReliabilityPostSq: {
        const uint32_t length = Lengths[reliabilityCase];
        auto &send_qp = rdma->queuePairs.entries[1];
        auto &recv_qp = peerRdma->queuePairs.entries[1];
        if (reliabilityCase == 4) {
            send_qp.attributes.sendPsn = 0x00ffffff;
            recv_qp.attributes.receivePsn = 0x00ffffff;
        }
        const uint32_t psn = send_qp.attributes.sendPsn;
        const uint64_t message = (uint64_t{send_qp.qpn} << 32) |
            static_cast<uint32_t>(send_qp.sqConsumerHead + 1);
        if (reliabilityCase == 0) {
            testLink->dropOnce(frame_id(Direction::Int0ToInt1,
                pvrdma::transport::Kind::Data, psn, message, 0));
        } else if (reliabilityCase == 1) {
            reliabilityRxDmasBefore = peerRdma->receivePayloadDmaStarts;
            testLink->duplicateOnce(frame_id(Direction::Int0ToInt1,
                pvrdma::transport::Kind::Data, psn, message, 0));
        } else if (reliabilityCase == 2) {
            recv_qp.attributes.minRnrTimer = 5;
        } else if (reliabilityCase == 3) {
            testLink->holdOnce(frame_id(Direction::Int1ToInt0,
                pvrdma::transport::Kind::Ack, psn, message, 0));
        } else if (reliabilityCase == 4) {
            testLink->duplicateOnce(frame_id(Direction::Int1ToInt0,
                pvrdma::transport::Kind::Ack, psn, message, 0));
        } else if (reliabilityCase == 5) {
            testLink->dropOnce(frame_id(Direction::Int0ToInt1,
                pvrdma::transport::Kind::Data,
                pvrdma::advancePsn(pvrdma::advancePsn(psn)), message, 2));
            testLink->dropOnce(frame_id(Direction::Int0ToInt1,
                pvrdma::transport::Kind::Data,
                (psn + 4) & pvrdma::transport::PsnMask, message, 4));
        } else if (reliabilityCase == 6) {
            for (int retry = 0; retry < 3; ++retry)
                testLink->dropOnce(frame_id(Direction::Int0ToInt1,
                    pvrdma::transport::Kind::Data,
                    pvrdma::advancePsn(psn), message, 1));
        }
        postReliabilitySend(length);
        timingStage = reliabilityCase == 2 ?
            TimingStage::ReliabilityRnrPostRq :
            reliabilityCase == 3 ?
                TimingStage::ReliabilityDeadlineRelease :
                TimingStage::ReliabilityVerify;
        schedule(testEvent, curTick() +
            microseconds(reliabilityCase == 2 ? 40 :
                reliabilityCase == 3 ? 50 :
                reliabilityCase == 6 ? 7000 : 3000));
        return;
      }
      case TimingStage::ReliabilityRnrPostRq:
        panic_if(!rdma->transportTimerEvent.scheduled() ||
                     rdma->drain() != DrainState::Draining ||
                     !rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences,
                 "PVRDMA RNR timer was not drain-visible");
        postReliabilityReceive(Lengths[reliabilityCase]);
        timingStage = TimingStage::ReliabilityVerify;
        schedule(testEvent, curTick() + microseconds(3000));
        return;
      case TimingStage::ReliabilityDeadlineRelease: {
        const auto &qp = rdma->queuePairs.entries[1];
        const uint64_t message = (uint64_t{qp.qpn} << 32) |
            static_cast<uint32_t>(qp.sqConsumerHead + 1);
        const PvrdmaTestLink::FrameId ack{
            Direction::Int1ToInt0, pvrdma::transport::Kind::Ack,
            qp.attributes.sendPsn, message, 0};
        panic_if(!rdma->transportTimerEvent.scheduled() ||
                     rdma->transport.stage !=
                         Pvrdma::TransportState::Stage::WaitResponse ||
                     !testLink->releaseAt(
                         ack, rdma->transportTimerEvent.when()),
                 "PVRDMA exact-deadline ACK setup failed");
        timingStage = TimingStage::ReliabilityVerify;
        schedule(testEvent, rdma->transportTimerEvent.when() +
            microseconds(100));
        return;
      }
      case TimingStage::ReliabilityVerify: {
        const uint32_t length = Lengths[reliabilityCase];
        if (reliabilityCase == 6) {
            const uint32_t status = 12;
            const auto error = read<pvrdma::CompletionQueueElement>(
                PairSenderCqe + reliabilityCase * pvrdma::CqeSize);
            panic_if(rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Error ||
                     rdma->queuePairs.entries[1].sqConsumerHead !=
                         reliabilityCase + 1 ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead != 6 ||
                     rdma->completionQueues.entries[1].producerTail !=
                         reliabilityCase + 1 ||
                     peerRdma->completionQueues.entries[1].producerTail != 6 ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != 7 ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         7 ||
                     letoh(error.workRequestId) !=
                         0x8000 + reliabilityCase ||
                     letoh(error.status) != status ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     testLink->drain() != DrainState::Drained ||
                     rdma->queueStats.cqErrorPublished.value() != 1 ||
                     rdma->queueStats.conservationViolations.value() ||
                     peerRdma->queueStats.conservationViolations.value(),
                     "PVRDMA retry exhaustion mismatch case %u",
                     reliabilityCase);
            pvrdma::CommandRequest request{};
            request.header.command = htole(static_cast<uint32_t>(
                pvrdma::Command::ModifyQp));
            request.modifyQp.qpHandle = htole(uint32_t{1});
            request.modifyQp.attributeMask = htole(pvrdma::QpAttrState);
            request.modifyQp.attributes.qpState =
                pvrdma::QpState::Reset;
            pvrdma::CommandResponse response{};
            panic_if(pvrdma::detail::modifyQp(
                         request, response, rdma->queuePairs).error ||
                         rdma->queuePairs.entries[1].state !=
                             pvrdma::QpState::Reset ||
                         rdma->queuePairs.entries[1].finalReplay.valid,
                     "PVRDMA Error-to-Reset recovery failed");
            inform("PVRDMA segmented reliability pair test passed");
            exitSimLoop("PVRDMA reliability pair test passed");
            return;
        }
        const auto received = read<std::array<uint8_t, 4097>>(
            PairReceiverPayload);
        for (size_t i = 0; i < length; ++i)
            panic_if(received[i] !=
                         static_cast<uint8_t>(i ^ reliabilityCase ^ 0x5a),
                     "PVRDMA reliability payload mismatch case %u byte %u",
                     reliabilityCase, i);
        uint16_t segments = 0;
        panic_if(!pvrdma::segmentGeometry(length, segments),
                 "PVRDMA reliability test length was not segmentable");
        const auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto sender_cq = read<pvrdma::Ring>(PairSenderCq +
            offsetof(pvrdma::RingState, rx));
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        panic_if(letoh(sender_ring.tx.consumerHead) !=
                         rdma->queuePairs.entries[1].sqConsumerHead ||
                     letoh(receiver_ring.rx.consumerHead) !=
                         peerRdma->queuePairs.entries[1].rqConsumerHead ||
                     rdma->queuePairs.entries[1].sqConsumerHead !=
                         reliabilityCase + 1 ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead !=
                         reliabilityCase + 1 ||
                     letoh(sender_cq.producerTail) != reliabilityCase + 1 ||
                     letoh(receiver_cq.producerTail) != reliabilityCase + 1 ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     testLink->drain() != DrainState::Drained ||
                     rdma->queueStats.conservationViolations.value() ||
                     peerRdma->queueStats.conservationViolations.value() ||
                     !peerRdma->queuePairs.entries[1].finalReplay.valid ||
                     peerRdma->queuePairs.entries[1].finalReplay.
                         segmentCount != segments ||
                     (reliabilityCase == 1 &&
                      peerRdma->receivePayloadDmaStarts !=
                          reliabilityRxDmasBefore + 1) ||
                     (reliabilityCase == 4 &&
                      (rdma->queuePairs.entries[1].attributes.sendPsn != 1 ||
                       peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                           1)),
                 "PVRDMA reliability accounting mismatch case %u",
                 reliabilityCase);
        if (reliabilityCase == 1) {
            const auto replay = peerRdma->queuePairs.entries[1].finalReplay;
            const pvrdma::transport::MacAddress changed_mac =
                {0x02, 0, 0, 0, 0, 3};
            write(PeerRegisterBarAddress + pvrdma::RegMacHigh,
                  htole(uint32_t{0x0300}), MmioFlags);
            const uint8_t payload = 0x5b;
            pvrdma::transport::Frame frame;
            frame.kind = pvrdma::transport::Kind::Data;
            frame.flags = pvrdma::transport::First |
                pvrdma::transport::Last;
            frame.sourceQpn = replay.remoteQpn;
            frame.destinationQpn = replay.localQpn;
            frame.psn = replay.finalPsn;
            frame.messageId = replay.messageId;
            frame.totalLength = 1;
            frame.segmentCount = 1;
            frame.payload = {&payload, 1};
            const size_t size = pvrdma::transport::EthernetHeaderSize +
                pvrdma::transport::HeaderSize + 1;
            auto packet = std::make_shared<EthPacketData>(size);
            const auto encoded = pvrdma::transport::encodeEthernet(
                frame, replay.remoteMac, changed_mac,
                {packet->data, packet->bufLength});
            panic_if(!encoded, "PVRDMA mutated-MAC replay failed encode");
            packet->length = packet->simLength = encoded.size;
            const auto decoded = pvrdma::transport::decodeEthernet(
                {packet->data, packet->bufLength}, packet->length);
            panic_if(!decoded || peerRdma->replayFinal(decoded) ||
                         peerRdma->transportActive() ||
                         replay.localMac == changed_mac,
                     "PVRDMA replay accepted a mutated local MAC");
            write(PeerRegisterBarAddress + pvrdma::RegMacHigh,
                  htole(uint32_t{0x0200}), MmioFlags);
        }
        if (++reliabilityCase == Lengths.size()) {
            inform("PVRDMA segmented reliability pair test passed");
            exitSimLoop("PVRDMA reliability pair test passed");
            return;
        }
        if (reliabilityCase != 2 && reliabilityCase <= 6)
            postReliabilityReceive(Lengths[reliabilityCase]);
        timingStage = TimingStage::ReliabilityPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      }
      default:
        panic("Invalid PVRDMA reliability stage");
    }
}

void
PvrdmaTester::runReliabilityRnrPair()
{
    switch (timingStage) {
      case TimingStage::Configure:
        setupReliabilityPair();
        reliabilityCase = 0;
        rdma->queuePairs.entries[1].attributes.rnrRetry = 0;
        timingStage = TimingStage::ReliabilityPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      case TimingStage::ReliabilityPostSq:
        postReliabilitySend(64);
        timingStage = TimingStage::ReliabilityVerify;
        schedule(testEvent, curTick() + microseconds(3000));
        return;
      case TimingStage::ReliabilityVerify: {
        const auto error = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe);
        panic_if(rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Error ||
                     rdma->queuePairs.entries[1].sqConsumerHead != 1 ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead != 0 ||
                     rdma->completionQueues.entries[1].producerTail != 1 ||
                     peerRdma->completionQueues.entries[1].producerTail != 0 ||
                     letoh(error.workRequestId) != 0x8000 ||
                     letoh(error.status) != 13 ||
                     rdma->queueStats.cqErrorPublished.value() != 1 ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     testLink->drain() != DrainState::Drained ||
                     rdma->queueStats.conservationViolations.value() ||
                     peerRdma->queueStats.conservationViolations.value(),
                 "PVRDMA RNR exhaustion mismatch");
        pvrdma::CommandRequest request{};
        request.header.command = htole(static_cast<uint32_t>(
            pvrdma::Command::ModifyQp));
        request.modifyQp.qpHandle = htole(uint32_t{1});
        request.modifyQp.attributeMask = htole(pvrdma::QpAttrState);
        request.modifyQp.attributes.qpState = pvrdma::QpState::Reset;
        pvrdma::CommandResponse response{};
        panic_if(pvrdma::detail::modifyQp(
                     request, response, rdma->queuePairs).error ||
                     rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Reset ||
                     rdma->queuePairs.entries[1].finalReplay.valid,
                 "PVRDMA RNR Error-to-Reset recovery failed");
        inform("PVRDMA RNR exhaustion reliability test passed");
        exitSimLoop("PVRDMA reliability RNR pair test passed");
        return;
      }
      default:
        panic("Invalid PVRDMA RNR reliability stage");
    }
}

void
PvrdmaTester::runReliabilityTimeoutZeroPair()
{
    using Direction = PvrdmaTestLink::Direction;
    const pvrdma::transport::MacAddress sender_mac =
        {0x02, 0, 0, 0, 0, 1};
    const pvrdma::transport::MacAddress receiver_mac =
        {0x02, 0, 0, 0, 0, 2};

    switch (timingStage) {
      case TimingStage::Configure: {
        setupReliabilityPair();
        reliabilityCase = 0;
        auto &qp = rdma->queuePairs.entries[1];
        qp.attributes.timeout = 0;
        const uint64_t message = (uint64_t{qp.qpn} << 32) | 1;
        testLink->dropOnce({Direction::Int0ToInt1,
            pvrdma::transport::Kind::Data, qp.attributes.sendPsn,
            message, 0});
        postReliabilitySend(64);
        timingStage = TimingStage::ReliabilityTimeoutZeroObserve;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::ReliabilityTimeoutZeroObserve: {
        auto &qp = rdma->queuePairs.entries[1];
        const uint64_t message = (uint64_t{qp.qpn} << 32) | 1;
        panic_if(rdma->transportTimerEvent.scheduled() ||
                     rdma->transport.stage !=
                         Pvrdma::TransportState::Stage::WaitResponse ||
                     rdma->transport.retryPending ||
                     qp.state != pvrdma::QpState::ReadyToSend ||
                     qp.sqConsumerHead ||
                     rdma->completionQueues.entries[1].producerTail ||
                     rdma->memoryRegions.entries[1].activeReferences != 1,
                 "PVRDMA timeout=0 armed or terminated under loss");
        reliabilityCase = 1;
        postReliabilitySend(64);
        auto packet = controlPacket(
            pvrdma::transport::Kind::Error,
            pvrdma::CompletionStatus::RemoteOperationError,
            qp.attributes.sendPsn, message, receiver_mac, sender_mac);
        panic_if(!rdma->recvTransportPacket(std::move(packet)),
                 "PVRDMA timeout=0 cleanup ERROR was rejected");
        timingStage = TimingStage::ReliabilityTimeoutZeroVerify;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::ReliabilityTimeoutZeroVerify: {
        auto &qp = rdma->queuePairs.entries[1];
        const auto error = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe);
        const auto rings = read<pvrdma::RingState>(PairSenderQp);
        pvrdma::CommandRequest request{};
        request.header.command = htole(static_cast<uint32_t>(
            pvrdma::Command::ModifyQp));
        request.modifyQp.qpHandle = htole(uint32_t{1});
        request.modifyQp.attributeMask = htole(pvrdma::QpAttrState);
        request.modifyQp.attributes.qpState = pvrdma::QpState::Reset;
        pvrdma::CommandResponse response{};
        panic_if(qp.state != pvrdma::QpState::Error ||
                     qp.sqProducerTail != 2 || qp.sqConsumerHead != 1 ||
                     letoh(rings.tx.producerTail) != 2 ||
                     letoh(rings.tx.consumerHead) != 1 ||
                     rdma->completionQueues.entries[1].producerTail != 1 ||
                     letoh(error.workRequestId) != 0x8000 ||
                     letoh(error.status) != 11 || rdma->transportActive() ||
                     pvrdma::detail::modifyQp(
                         request, response, rdma->queuePairs).error ||
                     qp.state != pvrdma::QpState::Reset ||
                     qp.sqProducerTail || qp.sqConsumerHead,
                 "PVRDMA timeout=0 remote ERROR/reset recovery mismatch");
        inform("PVRDMA timeout=0 reliability test passed");
        exitSimLoop("PVRDMA reliability timeout-zero pair test passed");
        return;
      }
      default:
        panic("Invalid PVRDMA timeout=0 reliability stage");
    }
}

void
PvrdmaTester::runReliabilityInvalidPair()
{
    using Direction = PvrdmaTestLink::Direction;
    using Kind = pvrdma::transport::Kind;
    static constexpr uint32_t Length = 2049;

    switch (timingStage) {
      case TimingStage::Configure:
        setupReliabilityPair();
        reliabilityCase = 0;
        postReliabilityReceive(Length);
        timingStage = TimingStage::ReliabilityPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      case TimingStage::ReliabilityPostSq: {
        const auto &qp = rdma->queuePairs.entries[1];
        const uint64_t message = (uint64_t{qp.qpn} << 32) | 1;
        testLink->holdOnce({Direction::Int1ToInt0, Kind::Ack,
                            qp.attributes.sendPsn, message, 0});
        postReliabilitySend(Length);
        timingStage = TimingStage::ReliabilityInvalidInject;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::ReliabilityInvalidInject: {
        auto &receiver = *peerRdma;
        panic_if(testLink->heldPackets() != 1 ||
                     !receiver.transport.active() ||
                     receiver.transport.stage !=
                         Pvrdma::TransportState::Stage::WaitReceiveData ||
                     receiver.transport.acceptedSegmentIndex != 0,
                 "PVRDMA invalid continuation setup did not become partial");
        const auto &sender_qp = rdma->queuePairs.entries[1];
        const uint64_t message = (uint64_t{sender_qp.qpn} << 32) | 1;
        const uint8_t payload = 0x5a;
        pvrdma::transport::Frame frame;
        frame.kind = Kind::Data;
        frame.flags = pvrdma::transport::Last;
        frame.sourceQpn = frame.destinationQpn = 1;
        frame.psn = pvrdma::advancePsn(
            pvrdma::advancePsn(sender_qp.attributes.sendPsn));
        frame.messageId = message;
        frame.totalLength = Length;
        frame.payloadOffset = 2048;
        frame.segmentIndex = 2;
        frame.segmentCount = 3;
        frame.payload = {&payload, 1};
        const pvrdma::transport::MacAddress sender_mac =
            {0x02, 0, 0, 0, 0, 1};
        const pvrdma::transport::MacAddress receiver_mac =
            {0x02, 0, 0, 0, 0, 2};
        const size_t size = pvrdma::transport::EthernetHeaderSize +
            pvrdma::transport::HeaderSize + 1;
        auto packet = std::make_shared<EthPacketData>(size);
        const auto encoded = pvrdma::transport::encodeEthernet(
            frame, sender_mac, receiver_mac,
            {packet->data, packet->bufLength});
        panic_if(!encoded, "PVRDMA out-of-order frame failed encode");
        packet->length = packet->simLength = encoded.size;
        panic_if(!receiver.recvTransportPacket(std::move(packet)),
                 "PVRDMA out-of-order frame was not accepted");
        timingStage = TimingStage::ReliabilityInvalidVerify;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::ReliabilityInvalidVerify: {
        const uint64_t message = (uint64_t{1} << 32) | 1;
        const PvrdmaTestLink::FrameId ack{
            Direction::Int1ToInt0, Kind::Ack, 0x100, message, 0};
        if (!reliabilityCase++) {
            const auto error = read<pvrdma::CompletionQueueElement>(
                PairSenderCqe);
            panic_if(rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Error ||
                     rdma->queuePairs.entries[1].sqConsumerHead != 1 ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead != 0 ||
                     rdma->completionQueues.entries[1].producerTail != 1 ||
                     peerRdma->completionQueues.entries[1].producerTail != 0 ||
                     letoh(error.workRequestId) != 0x8000 ||
                     letoh(error.status) != 21 ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         0x101 ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     !testLink->release(ack),
                     "PVRDMA out-of-order cleanup mismatch");
            postReliabilitySend(64);
            schedule(testEvent, curTick() + microseconds(20));
            return;
        }
        pvrdma::CommandRequest request{};
        request.header.command = htole(static_cast<uint32_t>(
            pvrdma::Command::ModifyQp));
        request.modifyQp.qpHandle = htole(uint32_t{1});
        request.modifyQp.attributeMask = htole(pvrdma::QpAttrState);
        request.modifyQp.attributes.qpState = pvrdma::QpState::Reset;
        pvrdma::CommandResponse response{};
        panic_if(rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Error ||
                     rdma->queuePairs.entries[1].sqConsumerHead != 1 ||
                     rdma->completionQueues.entries[1].producerTail != 1 ||
                     rdma->transportActive() ||
                     testLink->drain() != DrainState::Drained ||
                     pvrdma::detail::modifyQp(
                         request, response, rdma->queuePairs).error ||
                     rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Reset ||
                     rdma->queuePairs.entries[1].sqConsumerHead ||
                     rdma->queuePairs.entries[1].finalReplay.valid ||
                     rdma->queueStats.conservationViolations.value() ||
                     peerRdma->queueStats.conservationViolations.value(),
                 "PVRDMA remote ERROR did not block later SQ and reset");
        inform("PVRDMA invalid continuation reliability test passed");
        exitSimLoop("PVRDMA reliability invalid pair test passed");
        return;
      }
      default:
        panic("Invalid PVRDMA invalid-continuation test stage");
    }
}

void
PvrdmaTester::runReliabilityUnrelatedPair()
{
    using Direction = PvrdmaTestLink::Direction;
    using Kind = pvrdma::transport::Kind;
    static constexpr uint32_t Length = 2049;
    static constexpr uint32_t UnrelatedPsn = 0x456;
    const pvrdma::transport::MacAddress sender_mac =
        {0x02, 0, 0, 0, 0, 1};
    const pvrdma::transport::MacAddress receiver_mac =
        {0x02, 0, 0, 0, 0, 2};

    switch (timingStage) {
      case TimingStage::Configure:
        setupReliabilityPair();
        reliabilityCase = 0;
        postReliabilityReceive(Length);
        timingStage = TimingStage::ReliabilityPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      case TimingStage::ReliabilityPostSq: {
        const auto &qp = rdma->queuePairs.entries[1];
        const uint64_t message = (uint64_t{qp.qpn} << 32) | 1;
        testLink->holdOnce({Direction::Int1ToInt0, Kind::Ack,
                            qp.attributes.sendPsn, message, 0});
        testLink->dropOnce({Direction::Int1ToInt0, Kind::Error,
                            UnrelatedPsn, message + 1, 0});
        postReliabilitySend(Length);
        timingStage = TimingStage::ReliabilityUnrelatedInject;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::ReliabilityUnrelatedInject: {
        auto &receiver = *peerRdma;
        panic_if(testLink->heldPackets() != 1 ||
                     !receiver.transport.active() ||
                     receiver.transport.stage !=
                         Pvrdma::TransportState::Stage::WaitReceiveData ||
                     receiver.transport.acceptedSegmentIndex != 0,
                 "PVRDMA unrelated continuation setup did not become partial");
        const uint8_t payload = 0xa5;
        pvrdma::transport::Frame frame;
        frame.kind = Kind::Data;
        frame.flags = pvrdma::transport::First |
            pvrdma::transport::Last;
        frame.sourceQpn = frame.destinationQpn = 2;
        frame.psn = UnrelatedPsn;
        frame.messageId = (uint64_t{1} << 32) | 2;
        frame.totalLength = 1;
        frame.segmentCount = 1;
        frame.payload = {&payload, 1};
        panic_if(!receiver.recvTransportPacket(
                     transportPacket(frame, sender_mac, receiver_mac)),
                 "PVRDMA unrelated continuation was not accepted");
        timingStage = TimingStage::ReliabilityUnrelatedVerify;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::ReliabilityUnrelatedVerify: {
        const uint64_t message = (uint64_t{1} << 32) | 1;
        const PvrdmaTestLink::FrameId ack{
            Direction::Int1ToInt0, Kind::Ack, 0x100, message, 0};
        const auto &receiver = *peerRdma;
        panic_if(testLink->pendingRules() || testLink->heldPackets() != 1 ||
                     receiver.pendingErrorPacket ||
                     receiver.transport.stage !=
                         Pvrdma::TransportState::Stage::WaitReceiveData ||
                     receiver.transport.remoteMac != sender_mac ||
                     receiver.transport.remoteQpn != 1 ||
                     receiver.transport.localQpn != 1 ||
                     receiver.transport.messageId != message ||
                     receiver.transport.totalLength != Length ||
                     receiver.transport.segmentCount != 3 ||
                     receiver.transport.segmentIndex != 0 ||
                     receiver.transport.acceptedSegmentIndex != 0 ||
                     receiver.transport.psn != 0x100 ||
                     receiver.transport.acceptedPsn != 0x100 ||
                     receiver.transport.livePsn != 0x101 ||
                     !receiver.transport.leaseHeld ||
                     receiver.memoryRegions.entries[1].activeReferences != 1 ||
                     receiver.queuePairs.entries[1].rqConsumerHead ||
                     !testLink->release(ack),
                 "PVRDMA unrelated continuation disturbed active receive");
        timingStage = TimingStage::ReliabilityUnrelatedComplete;
        schedule(testEvent, curTick() + microseconds(3000));
        return;
      }
      case TimingStage::ReliabilityUnrelatedComplete: {
        const auto received = read<std::array<uint8_t, Length>>(
            PairReceiverPayload);
        for (size_t i = 0; i < received.size(); ++i)
            panic_if(received[i] != static_cast<uint8_t>(i ^ 0x5a),
                     "PVRDMA unrelated continuation payload mismatch at %u",
                     i);
        const auto send = read<pvrdma::CompletionQueueElement>(PairSenderCqe);
        const auto receive = read<pvrdma::CompletionQueueElement>(
            PairReceiverCqe);
        panic_if(rdma->queuePairs.entries[1].sqConsumerHead != 1 ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead != 1 ||
                     rdma->completionQueues.entries[1].producerTail != 1 ||
                     peerRdma->completionQueues.entries[1].producerTail != 1 ||
                     letoh(send.status) != 0 || letoh(receive.status) != 0 ||
                     letoh(receive.byteLength) != Length ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != 0x103 ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         0x103 ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     testLink->drain() != DrainState::Drained,
                 "PVRDMA receive did not complete after unrelated DATA");
        inform("PVRDMA unrelated continuation reliability test passed");
        exitSimLoop("PVRDMA reliability unrelated pair test passed");
        return;
      }
      default:
        panic("Invalid PVRDMA unrelated-continuation test stage");
    }
}

void
PvrdmaTester::runReliabilityCqPair()
{
    static constexpr uint32_t Length = 1025;

    switch (timingStage) {
      case TimingStage::Configure: {
        setupReliabilityPair();
        reliabilityCase = 0;
        postReliabilityReceive(Length);
        pvrdma::Ring blocked{};
        blocked.consumerHead = htole(uint32_t{7});
        write(PairReceiverCq + offsetof(pvrdma::RingState, rx), blocked);
        timingStage = TimingStage::ReliabilityPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      }
      case TimingStage::ReliabilityPostSq:
        postReliabilitySend(Length);
        timingStage = TimingStage::ReliabilityCqBlocked;
        schedule(testEvent, curTick() + microseconds(1500));
        return;
      case TimingStage::ReliabilityCqBlocked: {
        const auto received = read<std::array<uint8_t, Length>>(
            PairReceiverPayload);
        for (size_t i = 0; i < received.size(); ++i)
            panic_if(received[i] != static_cast<uint8_t>(i ^ 0x5a),
                     "PVRDMA CQ-backpressure payload mismatch at %u", i);
        panic_if(rdma->transport.stage !=
                         Pvrdma::TransportState::Stage::WaitResponse ||
                     rdma->transport.segmentIndex != 1 ||
                     rdma->transport.retryRemaining != 1 ||
                     peerRdma->transport.stage !=
                         Pvrdma::TransportState::Stage::WaitReceiveCq ||
                     !peerRdma->transport.completionBackpressured ||
                     rdma->queuePairs.entries[1].sqConsumerHead != 0 ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead != 0 ||
                     rdma->completionQueues.entries[1].producerTail != 0 ||
                     peerRdma->completionQueues.entries[1].producerTail != 0 ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != 0x101 ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         0x102 ||
                     rdma->memoryRegions.entries[1].activeReferences != 1 ||
                     peerRdma->memoryRegions.entries[1].activeReferences != 1,
                 "PVRDMA duplicate final was not held behind receive CQ");
        write(PairReceiverCq + offsetof(pvrdma::RingState, rx),
              pvrdma::Ring{});
        write(PeerUarBarAddress + pvrdma::UarPageSize +
                  pvrdma::CqDoorbellOffset,
              htole(pvrdma::CqPollAction | 1), MmioFlags);
        timingStage = TimingStage::ReliabilityCqVerify;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::ReliabilityCqVerify: {
        const auto send = read<pvrdma::CompletionQueueElement>(PairSenderCqe);
        const auto receive = read<pvrdma::CompletionQueueElement>(
            PairReceiverCqe);
        panic_if(rdma->queuePairs.entries[1].sqConsumerHead != 1 ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead != 1 ||
                     rdma->completionQueues.entries[1].producerTail != 1 ||
                     peerRdma->completionQueues.entries[1].producerTail != 1 ||
                     letoh(send.workRequestId) != 0x8000 ||
                     letoh(send.status) != 0 ||
                     letoh(receive.workRequestId) != 0x9000 ||
                     letoh(receive.status) != 0 ||
                     letoh(receive.byteLength) != Length ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != 0x102 ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         0x102 ||
                     !peerRdma->queuePairs.entries[1].finalReplay.valid ||
                     peerRdma->queuePairs.entries[1].finalReplay.
                         segmentIndex != 1 ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     testLink->drain() != DrainState::Drained ||
                     rdma->queueStats.conservationViolations.value() ||
                     peerRdma->queueStats.conservationViolations.value(),
                 "PVRDMA receive-CQ retry/replay recovery mismatch");
        inform("PVRDMA receive-CQ reliability test passed");
        exitSimLoop("PVRDMA reliability CQ pair test passed");
        return;
      }
      default:
        panic("Invalid PVRDMA receive-CQ test stage");
    }
}

void
PvrdmaTester::runReliabilityCqAbortPair()
{
    using Direction = PvrdmaTestLink::Direction;
    using Kind = pvrdma::transport::Kind;
    static constexpr uint32_t Length = 1025;

    switch (timingStage) {
      case TimingStage::Configure: {
        setupReliabilityPair();
        reliabilityCase = 0;
        postReliabilityReceive(Length);
        pvrdma::Ring full{};
        full.producerTail = htole(uint32_t{8});
        write(PairReceiverCq + offsetof(pvrdma::RingState, rx), full);
        peerRdma->completionQueues.entries[1].producerTail = 8;
        peerRdma->refreshQueueGauges();
        peerRdma->queueStatsReset();
        timingStage = TimingStage::ReliabilityPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      }
      case TimingStage::ReliabilityPostSq:
        postReliabilitySend(Length);
        timingStage = TimingStage::ReliabilityCqBlocked;
        schedule(testEvent, curTick() + microseconds(1500));
        return;
      case TimingStage::ReliabilityCqBlocked: {
        auto &receiver = *peerRdma;
        const auto &sender_qp = rdma->queuePairs.entries[1];
        const uint64_t message = (uint64_t{sender_qp.qpn} << 32) | 1;
        const uint32_t bad_psn = receiver.transport.acceptedPsn;
        panic_if(receiver.transport.stage !=
                         Pvrdma::TransportState::Stage::WaitReceiveCq ||
                     !receiver.transport.completionBackpressured ||
                     receiver.completionDma.active() ||
                     receiver.queuePairs.entries[1].rqConsumerHead ||
                     receiver.completionQueues.entries[1].producerTail != 8 ||
                     receiver.memoryRegions.entries[1].activeReferences != 1,
                 "PVRDMA CQ-abort setup did not reach backpressure");
        testLink->holdOnce({Direction::Int1ToInt0, Kind::Error,
                            bad_psn, message, 0});
        const uint8_t payload = 0x5a;
        pvrdma::transport::Frame frame;
        frame.kind = Kind::Data;
        frame.flags = pvrdma::transport::First |
            pvrdma::transport::Last;
        frame.sourceQpn = frame.destinationQpn = 1;
        frame.psn = bad_psn;
        frame.messageId = message;
        frame.totalLength = 1;
        frame.segmentCount = 1;
        frame.payload = {&payload, 1};
        const pvrdma::transport::MacAddress sender_mac =
            {0x02, 0, 0, 0, 0, 1};
        const pvrdma::transport::MacAddress receiver_mac =
            {0x02, 0, 0, 0, 0, 2};
        panic_if(!receiver.recvTransportPacket(
                     transportPacket(frame, sender_mac, receiver_mac)),
                 "PVRDMA CQ-backpressured malformed DATA was rejected");
        timingStage = TimingStage::ReliabilityCqAbortHeld;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::ReliabilityCqAbortHeld: {
        const uint64_t message = (uint64_t{1} << 32) | 1;
        const uint32_t bad_psn = 0x101;
        const PvrdmaTestLink::FrameId error{
            Direction::Int1ToInt0, Kind::Error, bad_psn, message, 0};
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        panic_if(testLink->heldPackets() != 1 ||
                     testLink->pendingRules() ||
                     peerRdma->transportActive() ||
                     peerRdma->transport.completionBackpressured ||
                     peerRdma->completionDma.active() ||
                     peerRdma->completionDmaEvent.scheduled() ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead ||
                     peerRdma->queueStats.rqConsumed.value() != 0 ||
                     letoh(receiver_ring.rx.consumerHead) != 0 ||
                     peerRdma->completionQueues.entries[1].producerTail != 8 ||
                     letoh(receiver_cq.producerTail) != 8 ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     !rdma->transportActive() ||
                     !testLink->release(error),
                 "PVRDMA CQ-backpressured abort retained receive state");
        timingStage = TimingStage::ReliabilityCqAbortVerify;
        schedule(testEvent, curTick() + microseconds(1000));
        return;
      }
      case TimingStage::ReliabilityCqAbortVerify: {
        const auto error = read<pvrdma::CompletionQueueElement>(PairSenderCqe);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        panic_if(rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Error ||
                     rdma->queuePairs.entries[1].sqConsumerHead != 1 ||
                     rdma->completionQueues.entries[1].producerTail != 1 ||
                     letoh(error.workRequestId) != 0x8000 ||
                     letoh(error.status) != 21 ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead ||
                     letoh(receiver_ring.rx.consumerHead) != 0 ||
                     peerRdma->completionQueues.entries[1].producerTail != 8 ||
                     letoh(receiver_cq.producerTail) != 8 ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     peerRdma->transport.completionBackpressured ||
                     peerRdma->completionDma.active() ||
                     peerRdma->queueStats.rqConsumed.value() != 0 ||
                     testLink->drain() != DrainState::Drained,
                 "PVRDMA CQ-backpressured abort cleanup mismatch");
        inform("PVRDMA receive-CQ abort reliability test passed");
        exitSimLoop("PVRDMA reliability CQ abort pair test passed");
        return;
      }
      default:
        panic("Invalid PVRDMA receive-CQ abort test stage");
    }
}

void
PvrdmaTester::runReliabilityPrecommitAbortPair()
{
    using CompletionStage = Pvrdma::CompletionDmaState::Stage;
    using Direction = PvrdmaTestLink::Direction;
    using Kind = pvrdma::transport::Kind;
    static constexpr uint32_t Length = 64;
    const pvrdma::transport::MacAddress sender_mac =
        {0x02, 0, 0, 0, 0, 1};
    const pvrdma::transport::MacAddress receiver_mac =
        {0x02, 0, 0, 0, 0, 2};
    const auto inject_final = [&] {
        std::array<uint8_t, Length> payload{};
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<uint8_t>(i ^ reliabilityCase ^ 0x5a);
        pvrdma::transport::Frame frame;
        frame.kind = Kind::Data;
        frame.flags = pvrdma::transport::First |
            pvrdma::transport::Last;
        frame.sourceQpn = frame.destinationQpn = 1;
        frame.psn = peerRdma->queuePairs.entries[1].attributes.receivePsn;
        frame.messageId = (uint64_t{1} << 32) | (reliabilityCase + 1);
        frame.totalLength = Length;
        frame.segmentCount = 1;
        frame.payload = {payload.data(), payload.size()};
        panic_if(!peerRdma->recvTransportPacket(
                     transportPacket(frame, sender_mac, receiver_mac)),
                 "PVRDMA precommit final DATA was rejected");
    };

    switch (timingStage) {
      case TimingStage::Configure: {
        setupReliabilityPair();
        reliabilityCase = 0;
        postReliabilityReceive(Length);
        pvrdma::Ring full{};
        full.producerTail = htole(uint32_t{8});
        write(PairReceiverCq + offsetof(pvrdma::RingState, rx), full);
        peerRdma->completionQueues.entries[1].producerTail = 8;
        peerRdma->refreshQueueGauges();
        peerRdma->queueStatsReset();
        inject_final();
        timingStage = TimingStage::ReliabilityPrecommitInject;
        schedule(testEvent, curTick() + microseconds(1));
        return;
      }
      case TimingStage::ReliabilityPrecommitInject: {
        auto &receiver = *peerRdma;
        if (receiver.completionDma.stage != CompletionStage::ReadCqRing) {
            schedule(testEvent, curTick() + microseconds(1));
            return;
        }
        const uint32_t psn = receiver.transport.acceptedPsn;
        const uint64_t message = receiver.transport.messageId;
        panic_if(receiver.transport.stage !=
                         Pvrdma::TransportState::Stage::WaitReceiveCq ||
                     receiver.queuePairs.entries[1].rqConsumerHead ||
                     receiver.memoryRegions.entries[1].activeReferences != 1,
                 "PVRDMA precommit abort missed CQ-ring DMA");
        pvrdma::transport::Frame duplicate;
        duplicate.kind = Kind::Data;
        duplicate.flags = pvrdma::transport::First |
            pvrdma::transport::Last;
        duplicate.sourceQpn = duplicate.destinationQpn = 1;
        duplicate.psn = psn;
        duplicate.messageId = message;
        duplicate.totalLength = Length;
        duplicate.segmentCount = 1;
        duplicate.payload = {receiver.transport.payload.data(), Length};
        panic_if(!receiver.recvTransportPacket(
                     transportPacket(duplicate, sender_mac, receiver_mac)) ||
                     receiver.precommitCompletionAbort ||
                     receiver.precommitCompletionAbortPacket ||
                     receiver.completionDma.stage !=
                         CompletionStage::ReadCqRing,
                 "PVRDMA exact precommit duplicate was not absorbed");
        if (!reliabilityCase) {
            panic_if(!receiver.recvTransportPacket(controlPacket(
                         Kind::Error,
                         pvrdma::CompletionStatus::RemoteOperationError,
                         psn, message, sender_mac, receiver_mac)),
                     "PVRDMA precommit matching ERROR was rejected");
        } else {
            testLink->holdOnce({Direction::Int1ToInt0, Kind::Error,
                                psn, message, 0});
            std::array<uint8_t, Length> payload{};
            pvrdma::transport::Frame frame;
            frame.kind = Kind::Data;
            frame.flags = pvrdma::transport::First |
                pvrdma::transport::Last;
            frame.sourceQpn = frame.destinationQpn = 1;
            frame.psn = psn;
            frame.messageId = message;
            frame.totalLength = Length + 1;
            frame.segmentCount = 1;
            frame.payload = {payload.data(), payload.size()};
            panic_if(!receiver.recvTransportPacket(
                         transportPacket(frame, sender_mac, receiver_mac)),
                     "PVRDMA precommit malformed DATA was rejected");
        }
        panic_if(!receiver.precommitCompletionAbort ||
                     !receiver.precommitCompletionAbortPacket ||
                     receiver.completionDma.stage !=
                         CompletionStage::ReadCqRing ||
                     receiver.transport.stage !=
                         Pvrdma::TransportState::Stage::WaitReceiveCq,
                 "PVRDMA precommit abort mutated an outstanding CQ read");
        timingStage = TimingStage::ReliabilityPrecommitVerify;
        schedule(testEvent, curTick() + microseconds(30));
        return;
      }
      case TimingStage::ReliabilityPrecommitVerify: {
        auto &receiver = *peerRdma;
        if (reliabilityCase == 2) {
            panic_if(testLink->heldPackets() || testLink->pendingRules() ||
                         testLink->drain() != DrainState::Drained,
                     "PVRDMA precommit ERROR release did not drain");
            inform("PVRDMA precommit receive-CQ abort test passed");
            exitSimLoop(
                "PVRDMA reliability precommit CQ abort test passed");
            return;
        }
        const auto rq = read<pvrdma::RingState>(PairReceiverQp);
        const auto cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        const auto cqe = read<pvrdma::CompletionQueueElement>(
            PairReceiverCqe);
        panic_if(receiver.transportActive() ||
                     receiver.completionDma.active() ||
                     receiver.completionDmaEvent.scheduled() ||
                     receiver.precommitCompletionAbort ||
                     receiver.precommitCompletionAbortPacket ||
                     receiver.queuePairs.entries[1].rqConsumerHead ||
                     letoh(rq.rx.consumerHead) != 0 ||
                     receiver.completionQueues.entries[1].producerTail != 8 ||
                     letoh(cq.producerTail) != 8 ||
                     letoh(cqe.workRequestId) != 0 ||
                     receiver.queueStats.rqConsumed.value() != 0 ||
                     receiver.queueStats.cqPublished.value() != 0 ||
                     receiver.queueStats.cqPublicationRejected.value() != 0 ||
                     receiver.queueStats.cqPublicationBackpressured.value() !=
                         0 ||
                     receiver.memoryRegions.entries[1].activeReferences,
                 "PVRDMA precommit CQ abort changed visible queue state");
        if (!reliabilityCase) {
            panic_if(receiver.pendingErrorPacket ||
                         testLink->heldPackets() || testLink->pendingRules(),
                     "PVRDMA matching ERROR generated a reverse ERROR");
            ++reliabilityCase;
            inject_final();
            timingStage = TimingStage::ReliabilityPrecommitInject;
            schedule(testEvent, curTick() + microseconds(1));
            return;
        }
        const uint32_t psn = 0x101;
        const uint64_t message = (uint64_t{1} << 32) | 2;
        const PvrdmaTestLink::FrameId error{
            Direction::Int1ToInt0, Kind::Error, psn, message, 0};
        panic_if(receiver.pendingErrorPacket ||
                     testLink->heldPackets() != 1 ||
                     testLink->pendingRules() || !testLink->release(error),
                 "PVRDMA malformed precommit DATA did not send ERROR");
        ++reliabilityCase;
        schedule(testEvent, curTick() + microseconds(30));
        return;
      }
      default:
        panic("Invalid PVRDMA precommit receive-CQ abort test stage");
    }
}

void
PvrdmaTester::runReliabilityCommitPair()
{
    using CompletionStage = Pvrdma::CompletionDmaState::Stage;
    using Direction = PvrdmaTestLink::Direction;
    using Kind = pvrdma::transport::Kind;
    static constexpr uint32_t Length = 64;
    static constexpr std::array<CompletionStage, 5> InjectionStages = {
        CompletionStage::WriteCqe,
        CompletionStage::PublishCqProducer,
        CompletionStage::WriteCqe,
        CompletionStage::PublishCqProducer,
        CompletionStage::WriteCqe,
    };
    const pvrdma::transport::MacAddress sender_mac =
        {0x02, 0, 0, 0, 0, 1};
    const pvrdma::transport::MacAddress receiver_mac =
        {0x02, 0, 0, 0, 0, 2};

    switch (timingStage) {
      case TimingStage::Configure:
        setupReliabilityPair();
        reliabilityCase = 0;
        postReliabilityReceive(Length);
        timingStage = TimingStage::ReliabilityPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      case TimingStage::ReliabilityPostSq: {
        const auto &qp = rdma->queuePairs.entries[1];
        if (reliabilityCase == InjectionStages.size() - 1) {
            const uint64_t message = (uint64_t{qp.qpn} << 32) |
                static_cast<uint32_t>(qp.sqConsumerHead + 1);
            testLink->holdOnce({Direction::Int1ToInt0, Kind::Ack,
                                qp.attributes.sendPsn, message, 0});
        }
        postReliabilitySend(Length);
        timingStage = TimingStage::ReliabilityCommitInject;
        schedule(testEvent, curTick() + microseconds(1));
        return;
      }
      case TimingStage::ReliabilityCommitInject: {
        auto &receiver = *peerRdma;
        if (receiver.completionDma.stage !=
                InjectionStages[reliabilityCase]) {
            schedule(testEvent, curTick() + microseconds(1));
            return;
        }
        panic_if(!receiver.completionBusy() ||
                     !receiver.transport.active() ||
                     receiver.transport.kind != pvrdma::QueueKind::Rq ||
                     receiver.transport.stage !=
                         Pvrdma::TransportState::Stage::WaitReceiveCq ||
                     receiver.pendingRxPacket ||
                     receiver.queuePairs.entries[1].rqConsumerHead !=
                         reliabilityCase,
                 "PVRDMA committed-receive injection missed active DMA");

        const auto psn = receiver.transport.acceptedPsn;
        const auto message = receiver.transport.messageId;
        EthPacketPtr packet;
        if (reliabilityCase < 2 || reliabilityCase == 4) {
            std::array<uint8_t, Length> payload{};
            pvrdma::transport::Frame frame;
            frame.kind = Kind::Data;
            frame.flags = pvrdma::transport::First |
                pvrdma::transport::Last;
            frame.sourceQpn = frame.destinationQpn = 1;
            frame.psn = psn;
            frame.messageId = message;
            frame.totalLength = reliabilityCase == 4 ? Length : Length + 1;
            frame.segmentCount = 1;
            frame.payload = {payload.data(), payload.size()};
            packet = transportPacket(frame, sender_mac, receiver_mac);
        } else {
            packet = controlPacket(
                Kind::Error, pvrdma::CompletionStatus::RemoteOperationError,
                psn, message, sender_mac, receiver_mac);
        }
        panic_if(!receiver.recvTransportPacket(std::move(packet)) ||
                     !receiver.pendingRxPacket ||
                     receiver.completionDma.stage !=
                         InjectionStages[reliabilityCase] ||
                     receiver.transport.stage !=
                         Pvrdma::TransportState::Stage::WaitReceiveCq ||
                     receiver.queuePairs.entries[1].rqConsumerHead !=
                         reliabilityCase ||
                     receiver.memoryRegions.entries[1].activeReferences != 1,
                 "PVRDMA committed receive was mutated by a deferred frame");
        timingStage = TimingStage::ReliabilityCommitVerify;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::ReliabilityCommitVerify: {
        const uint32_t completed = reliabilityCase + 1;
        const auto send = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe + reliabilityCase * pvrdma::CqeSize);
        const auto receive = read<pvrdma::CompletionQueueElement>(
            PairReceiverCqe + reliabilityCase * pvrdma::CqeSize);
        const auto extra = read<pvrdma::CompletionQueueElement>(
            PairReceiverCqe + completed * pvrdma::CqeSize);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        panic_if(rdma->queuePairs.entries[1].sqConsumerHead != completed ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead !=
                         completed ||
                     letoh(receiver_ring.rx.consumerHead) != completed ||
                     rdma->completionQueues.entries[1].producerTail !=
                         completed ||
                     peerRdma->completionQueues.entries[1].producerTail !=
                         completed ||
                     letoh(receiver_cq.producerTail) != completed ||
                     letoh(send.workRequestId) != 0x8000 + reliabilityCase ||
                     letoh(send.status) != 0 ||
                     letoh(receive.workRequestId) !=
                         0x9000 + reliabilityCase ||
                     letoh(receive.opcode) != 128 ||
                     letoh(receive.status) != 0 ||
                     letoh(receive.byteLength) != Length ||
                     letoh(extra.workRequestId) ||
                     peerRdma->queueStats.rqConsumed.value() != completed ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     !peerRdma->queuePairs.entries[1].finalReplay.valid ||
                     peerRdma->pendingRxPacket ||
                     peerRdma->queueStats.conservationViolations.value(),
                 "PVRDMA committed receive completion was not singular");
        if (reliabilityCase == InjectionStages.size() - 1) {
            const uint64_t message = (uint64_t{1} << 32) | completed;
            const PvrdmaTestLink::FrameId ack{
                Direction::Int1ToInt0, Kind::Ack,
                peerRdma->queuePairs.entries[1].finalReplay.finalPsn,
                message, 0};
            panic_if(testLink->heldPackets() != 1 ||
                         !testLink->release(ack),
                     "PVRDMA deferred final duplicate did not replay ACK");
            timingStage = TimingStage::ReliabilityCommitReplayVerify;
            schedule(testEvent, curTick() + microseconds(100));
            return;
        }
        ++reliabilityCase;
        postReliabilityReceive(Length);
        timingStage = TimingStage::ReliabilityPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      }
      case TimingStage::ReliabilityCommitReplayVerify:
        panic_if(rdma->transportActive() || peerRdma->transportActive() ||
                     testLink->drain() != DrainState::Drained ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead != 5 ||
                     peerRdma->completionQueues.entries[1].producerTail != 5 ||
                     peerRdma->memoryRegions.entries[1].activeReferences,
                 "PVRDMA deferred replay ACK cleanup mismatch");
        inform("PVRDMA committed receive reliability test passed");
        exitSimLoop("PVRDMA reliability committed receive test passed");
        return;
      default:
        panic("Invalid PVRDMA committed receive test stage");
    }
}

void
PvrdmaTester::runReliabilityCommitBoundaryPair()
{
    using Kind = pvrdma::transport::Kind;
    static constexpr uint32_t Length = 64;
    const pvrdma::transport::MacAddress sender_mac =
        {0x02, 0, 0, 0, 0, 1};
    const pvrdma::transport::MacAddress receiver_mac =
        {0x02, 0, 0, 0, 0, 2};
    const auto inject = [&] {
        auto &receiver = *peerRdma;
        const auto psn = receiver.transport.acceptedPsn;
        const auto message = receiver.transport.messageId;
        EthPacketPtr packet;
        if (reliabilityCase & 1) {
            std::array<uint8_t, Length> payload{};
            pvrdma::transport::Frame frame;
            frame.kind = Kind::Data;
            frame.flags = pvrdma::transport::First |
                pvrdma::transport::Last;
            frame.sourceQpn = frame.destinationQpn = 1;
            frame.psn = psn;
            frame.messageId = message;
            frame.totalLength = Length + 1;
            frame.segmentCount = 1;
            frame.payload = {payload.data(), payload.size()};
            packet = transportPacket(frame, sender_mac, receiver_mac);
        } else {
            packet = controlPacket(
                Kind::Error, pvrdma::CompletionStatus::RemoteOperationError,
                psn, message, sender_mac, receiver_mac);
        }
        panic_if(!receiver.finalReceiveCommitted() ||
                     !receiver.recvTransportPacket(std::move(packet)) ||
                     !receiver.pendingRxPacket,
                 "PVRDMA final receive did not defer boundary frame");
    };

    switch (timingStage) {
      case TimingStage::Configure:
        setupReliabilityPair();
        reliabilityCase = 0;
        postReliabilityReceive(Length);
        timingStage = TimingStage::ReliabilityPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      case TimingStage::ReliabilityPostSq:
        postReliabilitySend(Length);
        timingStage = TimingStage::ReliabilityBoundaryInject;
        schedule(testEvent, curTick() + microseconds(1));
        return;
      case TimingStage::ReliabilityBoundaryInject: {
        auto &receiver = *peerRdma;
        if (receiver.transport.stage !=
                Pvrdma::TransportState::Stage::WriteRqConsumer ||
            !receiver.transport.dmaBusy) {
            schedule(testEvent, curTick() + microseconds(1));
            return;
        }
        if (reliabilityCase >= 2) {
            receiver.transportPaused = true;
            timingStage = TimingStage::ReliabilityBoundaryTryAck;
            schedule(testEvent, curTick() + microseconds(1));
            return;
        }
        inject();
        panic_if(receiver.transport.stage !=
                         Pvrdma::TransportState::Stage::WriteRqConsumer ||
                     !receiver.transport.dmaBusy ||
                     receiver.transport.abortAfterDma ||
                     receiver.queuePairs.entries[1].rqConsumerHead !=
                         reliabilityCase ||
                     receiver.memoryRegions.entries[1].activeReferences != 1,
                 "PVRDMA RQ-consumer DMA injection mutated receive");
        timingStage = TimingStage::ReliabilityBoundaryVerify;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::ReliabilityBoundaryTryAck: {
        auto &receiver = *peerRdma;
        if (receiver.transport.stage !=
                Pvrdma::TransportState::Stage::TryAck ||
            receiver.transport.dmaBusy) {
            schedule(testEvent, curTick() + microseconds(1));
            return;
        }
        inject();
        const uint32_t completed = reliabilityCase + 1;
        panic_if(!receiver.transportPaused || receiver.transport.packet ||
                     receiver.transport.stage !=
                         Pvrdma::TransportState::Stage::TryAck ||
                     receiver.transport.keepAfterControl ||
                     receiver.queuePairs.entries[1].rqConsumerHead !=
                         completed ||
                     receiver.queueStats.rqConsumed.value() != completed ||
                     receiver.memoryRegions.entries[1].activeReferences != 1,
                 "PVRDMA final-ACK injection mutated receive");
        receiver.transportPaused = false;
        receiver.scheduleTransport();
        timingStage = TimingStage::ReliabilityBoundaryVerify;
        schedule(testEvent, curTick() + microseconds(100));
        return;
      }
      case TimingStage::ReliabilityBoundaryVerify: {
        const uint32_t completed = reliabilityCase + 1;
        const auto send = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe + reliabilityCase * pvrdma::CqeSize);
        const auto receive = read<pvrdma::CompletionQueueElement>(
            PairReceiverCqe + reliabilityCase * pvrdma::CqeSize);
        const auto extra = read<pvrdma::CompletionQueueElement>(
            PairReceiverCqe + completed * pvrdma::CqeSize);
        const auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        const auto &replay =
            peerRdma->queuePairs.entries[1].finalReplay;
        panic_if(rdma->queuePairs.entries[1].sqConsumerHead != completed ||
                     letoh(sender_ring.tx.consumerHead) != completed ||
                     peerRdma->queuePairs.entries[1].rqConsumerHead !=
                         completed ||
                     letoh(receiver_ring.rx.consumerHead) != completed ||
                     rdma->completionQueues.entries[1].producerTail !=
                         completed ||
                     peerRdma->completionQueues.entries[1].producerTail !=
                         completed ||
                     letoh(receiver_cq.producerTail) != completed ||
                     letoh(send.workRequestId) != 0x8000 + reliabilityCase ||
                     letoh(send.status) != 0 ||
                     letoh(receive.workRequestId) !=
                         0x9000 + reliabilityCase ||
                     letoh(receive.opcode) != 128 ||
                     letoh(receive.status) != 0 ||
                     letoh(receive.byteLength) != Length ||
                     letoh(extra.workRequestId) ||
                     peerRdma->queueStats.rqConsumed.value() != completed ||
                     !replay.valid || replay.totalLength != Length ||
                     replay.segmentIndex || replay.segmentCount != 1 ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->transportActive() || peerRdma->transportActive() ||
                     peerRdma->pendingRxPacket ||
                     peerRdma->pendingErrorPacket ||
                     testLink->drain() != DrainState::Drained ||
                     peerRdma->queueStats.conservationViolations.value(),
                 "PVRDMA receive commit-boundary accounting mismatch");
        if (++reliabilityCase == 4) {
            inform("PVRDMA receive commit-boundary reliability test passed");
            exitSimLoop(
                "PVRDMA reliability receive commit-boundary test passed");
            return;
        }
        postReliabilityReceive(Length);
        timingStage = TimingStage::ReliabilityPostSq;
        schedule(testEvent, curTick() + microseconds(20));
        return;
      }
      default:
        panic("Invalid PVRDMA receive commit-boundary test stage");
    }
}

void
PvrdmaTester::testInboundFrames()
{
    const pvrdma::transport::MacAddress sender_mac =
        {0x02, 0, 0, 0, 0, 1};
    const pvrdma::transport::MacAddress receiver_mac =
        {0x02, 0, 0, 0, 0, 2};
    const std::array<uint8_t, 64> payload = {0xa5};

    for (uint32_t test = 0; test < 3; ++test) {
        pvrdma::transport::Frame frame;
        frame.kind = pvrdma::transport::Kind::Data;
        frame.sourceQpn = frame.destinationQpn = 1;
        frame.psn = 0x103;
        frame.messageId = 0x3000 + test;
        frame.payload = {payload.data(), payload.size()};
        if (test == 0) {
            frame.flags = pvrdma::transport::First;
            frame.totalLength = 128;
            frame.segmentCount = 2;
        } else if (test == 1) {
            frame.flags = 0;
            frame.totalLength = 192;
            frame.payloadOffset = 64;
            frame.segmentIndex = 1;
            frame.segmentCount = 3;
        } else {
            frame.flags = pvrdma::transport::First |
                pvrdma::transport::Last;
            frame.totalLength = 65;
            frame.segmentCount = 1;
        }
        const size_t size = pvrdma::transport::EthernetHeaderSize +
            pvrdma::transport::HeaderSize + payload.size();
        auto packet = std::make_shared<EthPacketData>(size);
        const auto encoded = pvrdma::transport::encodeEthernet(
            frame, sender_mac, receiver_mac,
            {packet->data, packet->bufLength});
        panic_if(!encoded, "PVRDMA malformed-shape test frame failed encode");
        packet->length = packet->simLength = encoded.size;
        panic_if(!peerRdma->recvTransportPacket(std::move(packet)),
                 "PVRDMA malformed-shape test frame was not accepted");
        peerRdma->startInbound();
        panic_if(!peerRdma->pendingErrorPacket,
                 "PVRDMA malformed-shape DATA did not produce ERROR");
        const auto error = pvrdma::transport::decodeEthernet(
            {peerRdma->pendingErrorPacket->data,
             peerRdma->pendingErrorPacket->bufLength},
            peerRdma->pendingErrorPacket->length);
        panic_if(!error ||
                     error.frame.kind != pvrdma::transport::Kind::Error ||
                     error.frame.sourceQpn != frame.destinationQpn ||
                     error.frame.destinationQpn != frame.sourceQpn ||
                     error.frame.psn != frame.psn ||
                     error.frame.messageId != frame.messageId ||
                     error.source != receiver_mac ||
                     error.destination != sender_mac,
                 "PVRDMA malformed-shape ERROR identity mismatch");
        peerRdma->pendingErrorPacket.reset();
    }

    const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
    const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
        offsetof(pvrdma::RingState, rx));
    const auto untouched = read<std::array<uint8_t, 384>>(
        PairReceiverPayload + 128);
    panic_if(letoh(receiver_ring.rx.consumerHead) != 3 ||
                 letoh(receiver_cq.producerTail) != 3 ||
                 peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                     0x103 ||
                 peerRdma->queueStats.rqConsumed.value() != 3 ||
                 peerRdma->memoryRegions.entries[1].activeReferences ||
                 std::any_of(untouched.begin(), untouched.end(),
                             [](uint8_t byte) { return byte != 0; }),
             "PVRDMA malformed-shape DATA exposed receive success");
}

void
PvrdmaTester::runPair()
{
    switch (timingStage) {
      case TimingStage::Configure:
        setupPair();
        timingStage = TimingStage::PairPostSq;
        schedule(testEvent, curTick() + microseconds(50));
        return;
      case TimingStage::PairPostSq: {
        const auto response = read<pvrdma::CommandResponse>(ResponseAddress);
        const uint32_t cause = read<uint32_t>(
            PeerRegisterBarAddress + pvrdma::RegInterruptCause, MmioFlags);
        const auto &receiver_qp = peerRdma->queuePairs.entries[1];
        panic_if(read<uint32_t>(
                     PeerRegisterBarAddress + pvrdma::RegError,
                     MmioFlags) != 0 ||
                     !(cause & pvrdma::InterruptCauseResponse) ||
                     letoh(response.header.response) !=
                         0x6060606060606060 ||
                     letoh(response.header.acknowledgement) !=
                         pvrdma::responseCommand(
                             pvrdma::Command::ModifyQp) ||
                     response.header.error ||
                     receiver_qp.state !=
                         pvrdma::QpState::ReadyToReceive ||
                     receiver_qp.rqProducerTail != 3 ||
                     peerRdma->queueStats.rqPosted.value() != 3 ||
                     peerRdma->queueStats.doorbellWritesRejected.value(),
                 "PVRDMA INIT RQ/ immediate RTR command did not complete");
        peerRdma->transportPaused = true;
        pvrdma::RingState sender_rings{};
        sender_rings.tx.producerTail = htole(uint32_t{3});
        write(PairSenderQp, sender_rings);
        write(UarBarAddress + pvrdma::UarPageSize,
              htole(pvrdma::SqDoorbellAction | 1), MmioFlags);
        busyPolls = 0;
        timingStage = TimingStage::PairPollSq;
        schedule(testEvent, curTick() + sim_clock::as_int::ns);
        return;
      }
      case TimingStage::PairPollSq:
        if (rdma->transportActive() && peerRdma->pendingRxPacket) {
            panic_if(busyPolls < 2,
                     "PVRDMA SQ advanced without repeated empty CQ polls");
            timingStage = TimingStage::PairMacWrite;
            schedule(testEvent, curTick() + sim_clock::as_int::ns);
            return;
        }
        write(UarBarAddress + pvrdma::UarPageSize +
                  pvrdma::CqDoorbellOffset,
              htole(pvrdma::CqPollAction | 1), MmioFlags);
        panic_if(rdma->cqDirty & (uint64_t{1} << 1),
                 "PVRDMA empty CQ poll queued an observation");
        panic_if(++busyPolls > 100000,
                 "PVRDMA empty CQ polls starved runnable SQ transport");
        schedule(testEvent, curTick() + sim_clock::as_int::ns);
        return;
      case TimingStage::PairMacWrite: {
        panic_if(!rdma->transportActive() || !peerRdma->pendingRxPacket,
                 "PVRDMA pair DATA was not active for MAC-write test");
        const auto low = read<uint32_t>(
            RegisterBarAddress + pvrdma::RegMacLow, MmioFlags);
        const auto high = read<uint32_t>(
            RegisterBarAddress + pvrdma::RegMacHigh, MmioFlags);
        write(RegisterBarAddress + pvrdma::RegMacLow,
              htole(uint32_t{0xdeadbeef}), MmioFlags);
        write(RegisterBarAddress + pvrdma::RegMacHigh,
              htole(uint32_t{0xcafe}), MmioFlags);
        panic_if(read<uint32_t>(RegisterBarAddress + pvrdma::RegMacLow,
                                MmioFlags) != low ||
                     read<uint32_t>(RegisterBarAddress + pvrdma::RegMacHigh,
                                    MmioFlags) != high ||
                     read<uint32_t>(RegisterBarAddress + pvrdma::RegError,
                                    MmioFlags) != pvrdma::CommandError,
                 "PVRDMA active MAC write was not rejected unchanged");
        peerRdma->transportPaused = false;
        peerRdma->scheduleTransport();
        busyPolls = 0;
        sawCqPublishPollRace = false;
        timingStage = TimingStage::PairPollInbound;
        schedule(testEvent, curTick() + sim_clock::as_int::ns);
        return;
      }
      case TimingStage::PairPollInbound: {
        if (peerRdma->completionQueues.entries[1].producerTail !=
                peerRdma->completionQueues.entries[1].consumerHead) {
            panic_if(busyPolls < 2 ||
                         (!system->isAtomicMode() &&
                          !sawCqPublishPollRace),
                     "PVRDMA inbound empty-poll race was not exercised");
            timingStage = TimingStage::PairVerify;
            schedule(testEvent, curTick() + microseconds(1000));
            return;
        }
        const bool publish_in_flight =
            peerRdma->completionDma.stage ==
                Pvrdma::CompletionDmaState::Stage::PublishCqProducer &&
            peerRdma->completionDma.record.cqHandle == 1;
        write(PeerUarBarAddress + pvrdma::UarPageSize +
                  pvrdma::CqDoorbellOffset,
              htole(pvrdma::CqPollAction | 1), MmioFlags);
        const bool dirty = peerRdma->cqDirty & (uint64_t{1} << 1);
        panic_if(dirty != publish_in_flight,
                 "PVRDMA empty CQ poll publication-race handling mismatch");
        sawCqPublishPollRace |= publish_in_flight;
        panic_if(++busyPolls > 100000,
                 "PVRDMA empty CQ polls starved inbound transport");
        schedule(testEvent, curTick() + sim_clock::as_int::ns);
        return;
      }
      case TimingStage::PairVerify: {
        const auto received = read<std::array<uint8_t, 128>>(
            PairReceiverPayload);
        for (size_t i = 0; i < received.size(); ++i)
            panic_if(received[i] != static_cast<uint8_t>(i ^ 0x5a),
                     "PVRDMA pair payload mismatch at %u", i);
        const auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto sender_cq = read<pvrdma::Ring>(PairSenderCq +
            offsetof(pvrdma::RingState, rx));
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        const auto send0 = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe);
        const auto send1 = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe + pvrdma::CqeSize);
        panic_if(letoh(sender_ring.tx.consumerHead) != 3 ||
                     letoh(receiver_ring.rx.consumerHead) != 3 ||
                     letoh(sender_cq.producerTail) != 2 ||
                     letoh(receiver_cq.producerTail) != 3 ||
                     letoh(send0.workRequestId) != 0x1000 ||
                     letoh(send0.opcode) != 0 || letoh(send0.status) != 0 ||
                     letoh(send1.workRequestId) != 0x1002 ||
                     letoh(send1.opcode) != 0 || letoh(send1.status) != 0 ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != 0x103 ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         0x103 ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->queueStats.sqConsumed.value() != 3 ||
                     peerRdma->queueStats.rqConsumed.value() != 3 ||
                     rdma->queueStats.conservationViolations.value() != 0 ||
                     peerRdma->queueStats.conservationViolations.value() !=
                         0 ||
                     rdma->transportActive() || peerRdma->transportActive(),
                 "PVRDMA pair completion/consumer/PSN/accounting mismatch");
        for (uint32_t i = 0; i < 3; ++i) {
            const auto cqe = read<pvrdma::CompletionQueueElement>(
                PairReceiverCqe + i * pvrdma::CqeSize);
            panic_if(letoh(cqe.workRequestId) != 0x2000 + i ||
                         letoh(cqe.opcode) != 128 ||
                         letoh(cqe.status) != 0 ||
                         letoh(cqe.byteLength) != (i == 2 ? 0 : 64) ||
                         letoh(cqe.sourceQp) != 1,
                     "PVRDMA pair receive CQE %u mismatch", i);
        }
        testInboundFrames();
        auto rings = sender_ring;
        rings.tx.producerTail = htole(uint32_t{4});
        write(PairSenderQp, rings);
        write(UarBarAddress + pvrdma::UarPageSize,
              htole(pvrdma::SqDoorbellAction | 1), MmioFlags);
        timingStage = TimingStage::PairVerifyRnr;
        schedule(testEvent, curTick() + microseconds(1000));
        return;
      }
      case TimingStage::PairVerifyRnr: {
        const auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto sender_cq = read<pvrdma::Ring>(PairSenderCq +
            offsetof(pvrdma::RingState, rx));
        const auto error = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe + 2 * pvrdma::CqeSize);
        panic_if(letoh(sender_ring.tx.consumerHead) != 4 ||
                     letoh(receiver_ring.rx.consumerHead) != 3 ||
                     letoh(sender_cq.producerTail) != 3 ||
                     letoh(error.workRequestId) != 0x1003 ||
                     letoh(error.opcode) != 0 ||
                     letoh(error.status) != 13 ||
                     rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Error ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != 0x103 ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         0x103,
                 "PVRDMA pair terminal RNR mismatch");
        rdma->queuePairs.entries[1].state = pvrdma::QpState::ReadyToSend;
        rdma->queuePairs.entries[1].attributes.qpState =
            rdma->queuePairs.entries[1].attributes.currentQpState =
                pvrdma::QpState::ReadyToSend;
        auto rings = receiver_ring;
        rings.rx.producerTail = htole(uint32_t{4});
        write(PairReceiverQp, rings);
        write(PeerUarBarAddress + pvrdma::UarPageSize,
              htole(pvrdma::RqDoorbellAction | 1), MmioFlags);
        timingStage = TimingStage::PairPostShort;
        schedule(testEvent, curTick() + microseconds(50));
        return;
      }
      case TimingStage::PairPostShort: {
        panic_if(peerRdma->queuePairs.entries[1].rqProducerTail != 4,
                 "PVRDMA pair short RQ was not observed first");
        auto rings = read<pvrdma::RingState>(PairSenderQp);
        rings.tx.producerTail = htole(uint32_t{5});
        write(PairSenderQp, rings);
        write(UarBarAddress + pvrdma::UarPageSize,
              htole(pvrdma::SqDoorbellAction | 1), MmioFlags);
        timingStage = TimingStage::PairVerifyShort;
        schedule(testEvent, curTick() + microseconds(1000));
        return;
      }
      case TimingStage::PairVerifyShort: {
        auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto sender_cq = read<pvrdma::Ring>(PairSenderCq +
            offsetof(pvrdma::RingState, rx));
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        const auto error = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe + 3 * pvrdma::CqeSize);
        const auto untouched = read<std::array<uint8_t, 192>>(
            PairReceiverPayload + 128);
        panic_if(letoh(sender_ring.tx.consumerHead) != 5 ||
                     letoh(receiver_ring.rx.consumerHead) != 3 ||
                     letoh(sender_cq.producerTail) != 4 ||
                     letoh(receiver_cq.producerTail) != 3 ||
                     letoh(error.workRequestId) != 0x1004 ||
                     letoh(error.opcode) != 0 || letoh(error.status) != 21 ||
                     rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Error ||
                     std::any_of(untouched.begin(), untouched.end(),
                                 [](uint8_t byte) { return byte != 0; }) ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != 0x103 ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         0x103 ||
                     rdma->memoryRegions.entries[1].activeReferences ||
                     peerRdma->memoryRegions.entries[1].activeReferences ||
                     rdma->queueStats.sqConsumed.value() != 5 ||
                     peerRdma->queueStats.rqConsumed.value() != 3 ||
                     rdma->queueStats.conservationViolations.value() != 0 ||
                     peerRdma->queueStats.conservationViolations.value() !=
                         0 ||
                     rdma->transportActive() || peerRdma->transportActive(),
                 "PVRDMA pair terminal short-RQ mismatch");
        rdma->queuePairs.entries[1].state = pvrdma::QpState::ReadyToSend;
        rdma->queuePairs.entries[1].attributes.qpState =
            rdma->queuePairs.entries[1].attributes.currentQpState =
                pvrdma::QpState::ReadyToSend;
        sender_ring.tx.producerTail = htole(uint32_t{6});
        write(PairSenderQp, sender_ring);
        write(UarBarAddress + pvrdma::UarPageSize,
              htole(pvrdma::SqDoorbellAction | 1), MmioFlags);
        timingStage = TimingStage::PairVerifyMalformed;
        schedule(testEvent, curTick() + microseconds(1000));
        return;
      }
      case TimingStage::PairVerifyMalformed: {
        auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto sender_cq = read<pvrdma::Ring>(PairSenderCq +
            offsetof(pvrdma::RingState, rx));
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        const auto error = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe + 4 * pvrdma::CqeSize);
        panic_if(letoh(sender_ring.tx.consumerHead) != 6 ||
                     letoh(receiver_ring.rx.consumerHead) != 3 ||
                     letoh(sender_cq.producerTail) != 5 ||
                     letoh(receiver_cq.producerTail) != 3 ||
                     letoh(error.workRequestId) != 0x1005 ||
                     letoh(error.opcode) != 0 || letoh(error.status) != 21 ||
                     rdma->queuePairs.entries[1].state !=
                         pvrdma::QpState::Error ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != 0x103 ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         0x103 ||
                     rdma->queueStats.sqConsumed.value() != 6 ||
                     peerRdma->queueStats.rqConsumed.value() != 3 ||
                     rdma->transportActive() || peerRdma->transportActive(),
                 "PVRDMA pair terminal oversized-SEND mismatch");
        rdma->queuePairs.entries[1].state = pvrdma::QpState::ReadyToSend;
        rdma->queuePairs.entries[1].attributes.qpState =
            rdma->queuePairs.entries[1].attributes.currentQpState =
                pvrdma::QpState::ReadyToSend;

        pvrdma::ReceiveWqeHeader receive{};
        receive.workRequestId = htole(uint64_t{0x2006});
        receive.numSge = htole(uint32_t{1});
        const pvrdma::Sge receive_sge{
            htole(static_cast<uint64_t>(PairReceiverPayload + 384)),
            htole(uint32_t{64}), htole(uint32_t{1})};
        std::array<uint8_t, pvrdma::RqStride> receive_slot{};
        std::memcpy(receive_slot.data(), &receive, sizeof(receive));
        std::memcpy(receive_slot.data() + sizeof(receive), &receive_sge,
                    sizeof(receive_sge));
        write(PairReceiverRq + 3 * pvrdma::RqStride, receive_slot);
        auto posted = receiver_ring;
        posted.rx.producerTail = htole(uint32_t{4});
        write(PairReceiverQp, posted);
        write(PeerUarBarAddress + pvrdma::UarPageSize,
              htole(pvrdma::RqDoorbellAction | 1), MmioFlags);
        timingStage = TimingStage::PairPostCq;
        schedule(testEvent, curTick() + microseconds(50));
        return;
      }
      case TimingStage::PairPostCq: {
        panic_if(peerRdma->queuePairs.entries[1].rqProducerTail != 4,
                 "PVRDMA CQ recovery RQ was not observed first");
        pvrdma::Ring malformed_cq{};
        malformed_cq.producerTail = htole(uint32_t{3});
        malformed_cq.consumerHead = htole(uint32_t{7});
        write(PairReceiverCq + offsetof(pvrdma::RingState, rx),
              malformed_cq);
        auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        sender_ring.tx.producerTail = htole(uint32_t{7});
        write(PairSenderQp, sender_ring);
        write(UarBarAddress + pvrdma::UarPageSize,
              htole(pvrdma::SqDoorbellAction | 1), MmioFlags);
        timingStage = TimingStage::PairVerifyCqBlocked;
        schedule(testEvent, curTick() + microseconds(1000));
        return;
      }
      case TimingStage::PairVerifyCqBlocked: {
        const auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        panic_if(letoh(sender_ring.tx.consumerHead) != 6 ||
                     letoh(receiver_ring.rx.consumerHead) != 3 ||
                     letoh(receiver_cq.producerTail) != 3 ||
                     !rdma->transport.active() ||
                     rdma->transport.stage !=
                         Pvrdma::TransportState::Stage::WaitResponse ||
                     !peerRdma->transport.completionBackpressured ||
                     peerRdma->transport.stage !=
                         Pvrdma::TransportState::Stage::WaitReceiveCq,
                 "PVRDMA malformed CQ did not retain transport safely");
        pvrdma::Ring fixed_cq{};
        fixed_cq.producerTail = htole(uint32_t{3});
        write(PairReceiverCq + offsetof(pvrdma::RingState, rx), fixed_cq);
        write(PeerUarBarAddress + pvrdma::UarPageSize +
                  pvrdma::CqDoorbellOffset,
              htole(pvrdma::CqPollAction | 1), MmioFlags);
        panic_if(!(peerRdma->cqDirty & (uint64_t{1} << 1)),
                 "PVRDMA nonempty CQ poll skipped observation");
        timingStage = TimingStage::PairVerifyCqRecovered;
        schedule(testEvent, curTick() + microseconds(1000));
        return;
      }
      case TimingStage::PairVerifyCqRecovered: {
        auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
        const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
        const auto sender_cq = read<pvrdma::Ring>(PairSenderCq +
            offsetof(pvrdma::RingState, rx));
        const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
            offsetof(pvrdma::RingState, rx));
        const auto send = read<pvrdma::CompletionQueueElement>(
            PairSenderCqe + 5 * pvrdma::CqeSize);
        const auto receive = read<pvrdma::CompletionQueueElement>(
            PairReceiverCqe + 3 * pvrdma::CqeSize);
        panic_if(letoh(sender_ring.tx.consumerHead) != 7 ||
                     letoh(receiver_ring.rx.consumerHead) != 4 ||
                     letoh(sender_cq.producerTail) != 6 ||
                     letoh(receiver_cq.producerTail) != 4 ||
                     letoh(send.workRequestId) != 0x1006 ||
                     letoh(send.status) != 0 ||
                     letoh(receive.workRequestId) != 0x2006 ||
                     letoh(receive.status) != 0 ||
                     letoh(receive.byteLength) != 64 ||
                     rdma->queuePairs.entries[1].attributes.sendPsn != 0x104 ||
                     peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                         0x104 ||
                     rdma->transportActive() || peerRdma->transportActive(),
                 "PVRDMA CQ poll/fix did not recover transport");
        peerRdma->queuePairs.entries[1].generation++;
        peerRdma->queuePairs.entries[1].qpn +=
            pvrdma::ObjectTableEntries;
        sender_ring.tx.producerTail = htole(uint32_t{8});
        write(PairSenderQp, sender_ring);
        write(UarBarAddress + pvrdma::UarPageSize,
              htole(pvrdma::SqDoorbellAction | 1), MmioFlags);
        timingStage = TimingStage::PairVerifyStale;
        schedule(testEvent, curTick() + microseconds(1000));
        return;
      }
      case TimingStage::PairVerifyStale:
        break;
      default:
        panic("Invalid PVRDMA pair stage");
    }

    const auto sender_ring = read<pvrdma::RingState>(PairSenderQp);
    const auto receiver_ring = read<pvrdma::RingState>(PairReceiverQp);
    const auto sender_cq = read<pvrdma::Ring>(PairSenderCq +
        offsetof(pvrdma::RingState, rx));
    const auto receiver_cq = read<pvrdma::Ring>(PairReceiverCq +
        offsetof(pvrdma::RingState, rx));
    const auto error = read<pvrdma::CompletionQueueElement>(
        PairSenderCqe + 6 * pvrdma::CqeSize);
    panic_if(letoh(sender_ring.tx.consumerHead) != 8 ||
                 letoh(receiver_ring.rx.consumerHead) != 4 ||
                 letoh(sender_cq.producerTail) != 7 ||
                 letoh(receiver_cq.producerTail) != 4 ||
                 letoh(error.workRequestId) != 0x1007 ||
                 letoh(error.opcode) != 0 || letoh(error.status) != 21 ||
                 rdma->queuePairs.entries[1].attributes.sendPsn != 0x104 ||
                 peerRdma->queuePairs.entries[1].attributes.receivePsn !=
                     0x104 ||
                 rdma->memoryRegions.entries[1].activeReferences ||
                 peerRdma->memoryRegions.entries[1].activeReferences ||
                 rdma->queueStats.sqConsumed.value() != 8 ||
                 peerRdma->queueStats.rqConsumed.value() != 4 ||
                 rdma->queueStats.conservationViolations.value() != 0 ||
                 peerRdma->queueStats.conservationViolations.value() != 0 ||
                 rdma->transportActive() || peerRdma->transportActive(),
             "PVRDMA stale-QPN ERROR did not terminate sender");
    inform("PVRDMA one-segment pair SEND/RECV test passed");
    exitSimLoop("PVRDMA transport pair test passed");
}

EthPacketPtr
PvrdmaTester::controlPacket(
    pvrdma::transport::Kind kind, pvrdma::CompletionStatus status,
    uint32_t psn, uint64_t message_id,
    const pvrdma::transport::MacAddress &source,
    const pvrdma::transport::MacAddress &destination)
{
    pvrdma::transport::Frame frame;
    frame.kind = kind;
    frame.sourceQpn = frame.destinationQpn = 1;
    frame.psn = psn;
    frame.messageId = message_id;
    frame.status = status;
    const size_t size = pvrdma::transport::EthernetHeaderSize +
        pvrdma::transport::HeaderSize;
    auto packet = std::make_shared<EthPacketData>(size);
    const auto encoded = pvrdma::transport::encodeEthernet(
        frame, source, destination, {packet->data, packet->bufLength});
    panic_if(!encoded, "PVRDMA control frame failed encode");
    packet->length = packet->simLength = encoded.size;
    return packet;
}

EthPacketPtr
PvrdmaTester::faultPacket(uint32_t psn, uint64_t message_id)
{
    const uint8_t payload = static_cast<uint8_t>(message_id);
    pvrdma::transport::Frame frame;
    frame.kind = pvrdma::transport::Kind::Data;
    frame.flags = pvrdma::transport::First | pvrdma::transport::Last;
    frame.sourceQpn = frame.destinationQpn = 1;
    frame.psn = psn;
    frame.messageId = message_id;
    frame.totalLength = 1;
    frame.segmentCount = 1;
    frame.payload = {&payload, 1};
    const size_t size = pvrdma::transport::EthernetHeaderSize +
        pvrdma::transport::HeaderSize + 1;
    auto packet = std::make_shared<EthPacketData>(size);
    const pvrdma::transport::MacAddress source = {2, 0, 0, 0, 0, 1};
    const pvrdma::transport::MacAddress destination = {2, 0, 0, 0, 0, 2};
    const auto encoded = pvrdma::transport::encodeEthernet(
        frame, source, destination, {packet->data, packet->bufLength});
    panic_if(!encoded, "PVRDMA fault-link frame failed encode");
    packet->length = packet->simLength = encoded.size;
    return packet;
}

void
PvrdmaTester::runFaultLink()
{
    using Direction = PvrdmaTestLink::Direction;
    using FrameId = PvrdmaTestLink::FrameId;
    const auto id = [](Direction direction, uint32_t psn,
                       uint64_t message_id) {
        return FrameId{direction, pvrdma::transport::Kind::Data, psn,
                       message_id, 0};
    };

    switch (timingStage) {
      case TimingStage::Configure:
        faultRejectOnce[1] = true;
        panic_if(!faultPort0.sendPacket(faultPacket(7, 7)),
                 "PVRDMA fault link rejected backpressure packet");
        timingStage = TimingStage::FaultCheckBackpressure;
        schedule(testEvent, curTick() + 3);
        return;
      case TimingStage::FaultCheckBackpressure:
        panic_if(faultReceived[1] != std::vector<uint64_t>{7} ||
                     faultSendDone[0] != 1 || !faultDrainWhileRejected ||
                     testLink->drain() != DrainState::Drained,
                 "PVRDMA fault-link retained delivery was not retried once");
        testLink->dropOnce(id(Direction::Int0ToInt1, 2, 2));
        testLink->duplicateOnce(id(Direction::Int0ToInt1, 3, 3));
        testLink->holdOnce(id(Direction::Int0ToInt1, 4, 4));
        testLink->delayOnce(id(Direction::Int0ToInt1, 5, 5), 10);
        for (uint32_t value = 1; value <= 5; ++value) {
            panic_if(!faultPort0.sendPacket(faultPacket(value, value)),
                     "PVRDMA fault link rejected packet %u", value);
        }
        panic_if(!faultPort1.sendPacket(faultPacket(6, 6)),
                 "PVRDMA fault link rejected reverse packet");
        timingStage = TimingStage::FaultCheckHeld;
        schedule(testEvent, curTick() + 2);
        return;
      case TimingStage::FaultCheckHeld:
        panic_if(faultReceived[0] != std::vector<uint64_t>{6} ||
                     faultReceived[1] !=
                         std::vector<uint64_t>({7, 1, 3, 3}) ||
                     faultSendDone[0] != 6 || faultSendDone[1] != 1 ||
                     testLink->heldPackets() != 1 ||
                     testLink->pendingRules() != 0 ||
                     testLink->drain() != DrainState::Draining,
                 "PVRDMA fault-link drop/duplicate/hold result mismatch");
        panic_if(!testLink->release(
                     id(Direction::Int0ToInt1, 4, 4)) ||
                     testLink->release(id(Direction::Int0ToInt1, 4, 4)),
                 "PVRDMA fault-link release was not one-shot");
        timingStage = TimingStage::FaultCheckReleased;
        schedule(testEvent, curTick() + 2);
        return;
      case TimingStage::FaultCheckReleased:
        panic_if(faultReceived[1] !=
                         std::vector<uint64_t>({7, 1, 3, 3, 4}) ||
                     testLink->heldPackets() != 0 ||
                     testLink->drain() != DrainState::Draining,
                 "PVRDMA fault-link release/drain result mismatch");
        timingStage = TimingStage::FaultCheckDelayed;
        schedule(testEvent, curTick() + 8);
        return;
      case TimingStage::FaultCheckDelayed:
        panic_if(faultReceived[1] !=
                         std::vector<uint64_t>({7, 1, 3, 3, 4, 5}) ||
                     faultSendDone[0] != 6 ||
                     testLink->drain() != DrainState::Drained,
                 "PVRDMA fault-link delay/final drain result mismatch");
        inform("PVRDMA deterministic fault-link test passed");
        exitSimLoop("PVRDMA fault-link test passed");
        return;
      default:
        panic("Invalid PVRDMA fault-link test stage");
    }
}

void
PvrdmaTester::run()
{
    if (testMode == "fault-link" || testMode == "timing-fault-link") {
        runFaultLink();
        return;
    }
    if (testMode == "reliability-pair" ||
        testMode == "timing-reliability-pair") {
        runReliabilityPair();
        return;
    }
    if (testMode == "reliability-rnr-pair" ||
        testMode == "timing-reliability-rnr-pair") {
        runReliabilityRnrPair();
        return;
    }
    if (testMode == "reliability-timeout-zero-pair" ||
        testMode == "timing-reliability-timeout-zero-pair") {
        runReliabilityTimeoutZeroPair();
        return;
    }
    if (testMode == "reliability-invalid-pair" ||
        testMode == "timing-reliability-invalid-pair") {
        runReliabilityInvalidPair();
        return;
    }
    if (testMode == "reliability-unrelated-pair" ||
        testMode == "timing-reliability-unrelated-pair") {
        runReliabilityUnrelatedPair();
        return;
    }
    if (testMode == "reliability-cq-pair" ||
        testMode == "timing-reliability-cq-pair") {
        runReliabilityCqPair();
        return;
    }
    if (testMode == "reliability-cq-abort-pair" ||
        testMode == "timing-reliability-cq-abort-pair") {
        runReliabilityCqAbortPair();
        return;
    }
    if (testMode == "timing-reliability-precommit-abort-pair") {
        runReliabilityPrecommitAbortPair();
        return;
    }
    if (testMode == "timing-reliability-commit-pair") {
        runReliabilityCommitPair();
        return;
    }
    if (testMode == "timing-reliability-commit-boundary-pair") {
        runReliabilityCommitBoundaryPair();
        return;
    }
    if (testMode == "transport-pair" ||
        testMode == "timing-transport-pair") {
        runPair();
        return;
    }
    if (testMode == "semantic-pair" ||
        testMode == "timing-semantic-pair") {
        runSemanticPair();
        return;
    }
    if (testMode == "timing-mr") {
        runTimingMr();
        return;
    }
    if (testMode == "timing-queues" ||
        testMode == "timing-observation") {
        runTimingQueues();
        return;
    }
    if (testMode == "timing-completion") {
        runTimingCompletion();
        return;
    }
    if (testMode == "completion-errors") {
        runCompletionErrors();
        return;
    }
    if (testMode == "completion") {
        runCompletion();
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
    if (testMode == "checkpoint-completion-save") {
        testCheckpointCompletionSave();
        return;
    }
    if (testMode == "checkpoint-completion-restore") {
        testCheckpointCompletionRestore();
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
