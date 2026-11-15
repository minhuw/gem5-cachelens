// SPDX-License-Identifier: BSD-3-Clause
#include "test_objects/i40e_observe_host.hh"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <unistd.h>

#include "base/logging.hh"
#include "mem/dma_trace.hh"
#include "sim/core.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace
{
// Values/layouts copied from immutable producer 50ce8d4 i40e_register.h,
// i40e_adminq_cmd.h, i40e_rxtxq.h and initialized_checkpoint_test.cc.
constexpr Addr AtqBal = 0x80000, AtqBah = 0x80100, AtqLen = 0x80200;
constexpr Addr AtqHead = 0x80300, AtqTail = 0x80400;
constexpr Addr HmcCommand = 0xc0000, HmcLow = 0xc0100, HmcHigh = 0xc0200;
constexpr Addr HmcRxBase = 0xc6400, Hena = 0x245900, Hlut = 0x240000;
constexpr Addr RxEnable = 0x120004, RxTail = 0x128004; // queue 1
constexpr Addr AdminWire = 0x10000, ContextWire = 0x20020;
constexpr Addr RingWire = 0x30000, PayloadWire = 0x60000;
constexpr unsigned FrameBytes = 1472;

void put(std::vector<uint8_t> &data, size_t offset, uint64_t value, size_t size)
{
    panic_if(offset + size > data.size(), "i40e host layout overflow");
    for (size_t i = 0; i < size; ++i) data[offset + i] = value >> (8 * i);
}
uint64_t get(const std::vector<uint8_t> &data, size_t offset, size_t size)
{
    panic_if(offset + size > data.size(), "i40e host response layout overflow");
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= uint64_t(data[offset + i]) << (8 * i);
    return value;
}
int create(const std::string &path)
{
    fatal_if(path.empty() || path.front() != '/', "i40e host output must be absolute");
    int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    fatal_if(fd < 0, "Cannot exclusively create i40e host output %s: %s",
             path, strerror(errno));
    return fd;
}
} // namespace

I40eObserveHost::PioPort::PioPort(I40eObserveHost &owner)
    : RequestPort(owner.name() + ".pio"), host(owner)
{}
bool I40eObserveHost::PioPort::recvTimingResp(PacketPtr pkt)
{
    host.pioResponse(pkt);
    return true;
}
void I40eObserveHost::PioPort::recvReqRetry() { host.retryPio(); }

I40eObserveHost::I40eObserveHost(const I40eObserveHostParams &p)
    : ClockedObject(p), pio(*this), memory(this, p.system),
      pioId(p.system->getRequestorId(this)),
      advanceEvent([this] { advance(); }, name() + ".advance"),
      memoryDoneEvent([this] { memoryDone(); }, name() + ".memory_done"),
      timeoutEvent([this] { fatal("i40e test host timeout: ready=%d label=%s polls=%u",
          ready, actions.empty() ? "none" : actions.front().label, pollAttempts); },
          name() + ".timeout")
{}
I40eObserveHost::~I40eObserveHost()
{
    if (output) panic_if(fclose(output), "i40e host output close failed");
}
Port &I40eObserveHost::getPort(const std::string &name, PortID idx)
{
    if (name == "pio") return pio;
    if (name == "memory") return memory;
    return ClockedObject::getPort(name, idx);
}
void I40eObserveHost::event(const std::string &name, const std::string &fields)
{
    std::string line = "{\"schema\":\"cachelens.i40e.observe_host\",\"version\":1,"
        "\"event\":" + dmaTraceQuote(name) + ",\"tick\":" + std::to_string(curTick()) +
        ",\"ticks_per_second\":" + std::to_string(sim_clock::as_int::s) +
        ",\"host_action_seq\":" + std::to_string(actionSequence) +
        (fields.empty() ? "" : "," + fields) + "}\n";
    fatal_if(fwrite(line.data(), 1, line.size(), output) != line.size() || fflush(output),
             "i40e host output write failed");
}
void I40eObserveHost::addPio(bool write, Addr address, uint32_t value,
    const std::string &label,
    std::function<bool(const std::vector<uint8_t> &)> check)
{
    std::vector<uint8_t> data(4);
    put(data, 0, value, 4);
    actions.push_back({write ? Kind::PioWrite : Kind::PioRead,
                       address, std::move(data), label, std::move(check)});
}
void I40eObserveHost::addMemory(bool write, Addr wireAddress,
    std::vector<uint8_t> bytes, const std::string &label,
    std::function<bool(const std::vector<uint8_t> &)> check)
{
    actions.push_back({write ? Kind::MemoryWrite : Kind::MemoryRead,
        params().pci_dma_base + wireAddress, std::move(bytes), label, std::move(check)});
}
void I40eObserveHost::addDelay(Tick delay, const std::string &label)
{
    actions.push_back({Kind::Delay, 0, {}, label, {}, delay});
}
void I40eObserveHost::buildProgram()
{
    const auto config = params().config_address;
    const auto bar = params().bar0_address;
    fatal_if(bar > UINT32_MAX || (bar & 0x7fffff), "i40e BAR0 must be 8MiB aligned below 4GiB");
    addPio(false, config, 0, "pci_vendor", [](const auto &data) {
        fatal_if((get(data, 0, 4) & 0xffff) != 0x8086, "Real PCI introduction is not Intel");
        return true;
    });
    addPio(true, config + 0x10, bar, "pci_bar0");
    addPio(false, config + 0x10, 0, "pci_bar0_readback", [bar](const auto &data) {
        fatal_if((get(data, 0, 4) & ~uint64_t(15)) != bar, "Native BAR0 readback mismatch");
        return true;
    });
    addPio(true, config + 4, 6, "pci_memory_busmaster_enable");
    if (params().admin_only) {
        buildAdminProgram();
        return;
    }

    std::vector<uint8_t> admin(32);
    put(admin, 2, 1, 2); // opcode get_version; zero flags, no external buffer
    addMemory(true, AdminWire, std::move(admin), "init_admin_descriptor");
    std::vector<uint8_t> context(32);
    put(context, 4, RingWire / 128, 8);
    put(context, 11, 8 << 1, 2);
    put(context, 12, 4 << 6, 2); // 512-byte data buffers
    put(context, 13, get(context, 13, 2) | (1 << 12) | (1 << 13), 2);
    put(context, 21, 16 << 6, 4);
    addMemory(true, ContextWire, std::move(context), "init_rx_q1_context");
    std::vector<uint8_t> ring(8 * 32);
    for (unsigned i = 0; i < 7; ++i) put(ring, i * 32, PayloadWire + i * 512, 8);
    addMemory(true, RingWire, std::move(ring), "init_rx_descriptors");
    addMemory(true, PayloadWire, std::vector<uint8_t>(7 * 512, 0xcd), "init_rx_buffers");

    addPio(true, bar + AtqBal, AdminWire, "admin_base_low");
    addPio(true, bar + AtqBah, 0, "admin_base_high");
    addPio(true, bar + AtqLen, (1u << 31) | 8, "admin_enable");
    addPio(true, bar + AtqTail, 1, "admin_submit_get_version");
    addPio(false, bar + AtqHead, 0, "admin_head_poll", [](const auto &data) {
        return get(data, 0, 4) == 1;
    });
    addMemory(false, AdminWire, std::vector<uint8_t>(32), "verify_admin_descriptor",
        [](const auto &data) {
            fatal_if(!(get(data, 0, 2) & 1) || get(data, 6, 2),
                     "Real Admin get_version did not complete successfully");
            return true;
        });
    addPio(true, bar + HmcLow, 0x20000 | 3 | (511 << 2), "hmc_direct_segment_low");
    addPio(true, bar + HmcHigh, 0, "hmc_direct_segment_high");
    addPio(true, bar + HmcCommand, 1u << 31, "hmc_install_segment0");
    addPio(true, bar + HmcRxBase, 0, "hmc_rx_base");
    addPio(true, bar + Hena, 1u << 31, "rss_ipv4_udp");
    for (unsigned i = 0; i < 128; ++i)
        addPio(true, bar + Hlut + i * 128, 0x01010101, "rss_to_queue1");
    addPio(true, bar + RxTail, 7, "rx_post_seven_descriptors");
    addPio(true, bar + RxEnable, 1, "rx_enable_queue1");
    addPio(false, bar + RxEnable, 0, "rx_enabled_poll", [](const auto &data) {
        return (get(data, 0, 4) & 4) != 0;
    });
    addDelay(params().settle_interval, "settle_rx_prefetch");
    actions.push_back({Kind::Ready, 0, {}, "ready_for_net_only_peer", {}});
    addMemory(false, RingWire + 2 * 32 + 8, std::vector<uint8_t>(8),
        "rx_last_descriptor_poll", [](const auto &data) {
            return (get(data, 0, 8) & 3) == 3;
        });
    addDelay(params().settle_interval, "settle_rx_after_descriptor_done");
    addMemory(false, RingWire, std::vector<uint8_t>(3 * 32), "verify_rx_descriptors",
        [](const auto &data) {
            for (unsigned i = 0; i < 3; ++i) {
                uint64_t status = get(data, i * 32 + 8, 8);
                unsigned length = (status >> 38) & 0x3fff;
                fatal_if(!(status & 1) || bool(status & 2) != (i == 2) ||
                         length != (i == 2 ? 448 : 512),
                         "RX descriptor %u mismatch: status=%#llx length=%u", i, status, length);
            }
            return true;
        });
    addMemory(false, PayloadWire, std::vector<uint8_t>(FrameBytes), "verify_rx_payload",
        [this](const auto &data) {
            fatal_if(data != expectedFrame, "Actual real-Runner RX payload differs from NET input");
            FILE *file = fdopen(create(params().payload_output), "wb");
            fatal_if(!file, "Cannot open RX readback output");
            fatal_if(fwrite(data.data(), 1, data.size(), file) != data.size() || fclose(file),
                     "Cannot write RX readback output");
            return true;
        });
    actions.push_back({Kind::Done, 0, {}, "complete", {}});
}
void I40eObserveHost::buildAdminProgram()
{
    // Exact legal subset of producer ae7 initialized_checkpoint_test.cc
    // dma_trace_control_runner: ring_len4, desc0 get_version, desc1
    // driver_version RD|BUF16 at wire0x18000. Omit unassigned desc2=0xffff.
    // The actual source's generic supported completion for driver_version is
    // still opcode_name=null; this fixture does not invent a decoder name.
    constexpr Addr ReadBufferWire = 0x18000;
    constexpr unsigned ReadBufferBytes = 16;
    std::vector<uint8_t> descriptors(4 * 32);
    put(descriptors, 2, 1, 2); // get_version
    put(descriptors, 32, (1 << 10) | (1 << 12), 2); // RD|BUF
    put(descriptors, 32 + 2, 2, 2); // hardware driver_version, catalog name null
    put(descriptors, 32 + 4, ReadBufferBytes, 2);
    put(descriptors, 32 + 24, 0, 4); // external.addr_high
    put(descriptors, 32 + 28, ReadBufferWire, 4); // external.addr_low
    addMemory(true, AdminWire, std::move(descriptors), "admin_init_ring4");
    // The real v5 fixture initializes its host-memory vector to zero.
    addMemory(true, ReadBufferWire, std::vector<uint8_t>(ReadBufferBytes),
              "admin_init_rd_input_zeroes");
    const auto bar = params().bar0_address;
    addPio(true, bar + AtqBal, AdminWire, "admin_base_low");
    addPio(true, bar + AtqBah, 0, "admin_base_high");
    addPio(true, bar + AtqLen, (1u << 31) | 4, "admin_enable_ring4");
    addPio(true, bar + AtqTail, 2, "admin_submit_get_version_and_driver_version_rd");
    addPio(false, bar + AtqHead, 0, "admin_head2_poll", [](const auto &data) {
        return get(data, 0, 4) == 2;
    });
    addDelay(params().settle_interval, "admin_settle_after_real_head_completion");
    addMemory(false, AdminWire, std::vector<uint8_t>(2 * 32),
        "admin_verify_completed_descriptors", [this](const auto &data) {
            for (unsigned i = 0; i < 2; ++i) {
                const auto flags = get(data, i * 32, 2);
                fatal_if((flags & 3) != 3 || (flags & 4) ||
                         get(data, i * 32 + 6, 2) != 0 ||
                         get(data, i * 32 + 2, 2) != i + 1,
                         "Actual Admin descriptor %u did not complete DD|CMP/retval0", i);
            }
            fatal_if(get(data, 32 + 4, 2) != ReadBufferBytes ||
                     (get(data, 32, 2) & 0x1400) != 0x1400,
                     "Actual driver_version RD buffer descriptor changed");
            adminResult = data;
            return true;
        });
    addMemory(false, ReadBufferWire, std::vector<uint8_t>(ReadBufferBytes),
        "admin_verify_rd_input_unchanged", [this](const auto &data) {
            for (auto byte : data) fatal_if(byte, "Admin RD source buffer changed");
            adminResult.insert(adminResult.end(), data.begin(), data.end());
            FILE *file = fdopen(create(params().payload_output), "wb");
            fatal_if(!file, "Cannot open actual Admin readback output");
            fatal_if(fwrite(adminResult.data(), 1, adminResult.size(), file) !=
                     adminResult.size() || fclose(file), "Cannot write actual Admin readback");
            return true;
        });
    actions.push_back({Kind::Done, 0, {}, "admin_complete_no_send", {}});
}
void I40eObserveHost::startup()
{
    fatal_if(!pio.isConnected() || !memory.isConnected(), "i40e host needs real PIO and memory ports");
    fatal_if(!params().system->isTimingMode() || params().system->cacheLineSize() != 64,
             "i40e host fixture requires timing mode and 64-byte lines");
    if (!params().admin_only) {
        std::ifstream frame(params().expected_frame, std::ios::binary);
        fatal_if(!frame, "Cannot read exact NET input frame %s", params().expected_frame);
        expectedFrame.assign(std::istreambuf_iterator<char>(frame), {});
        fatal_if(frame.bad() || expectedFrame.size() != FrameBytes,
                 "Expected NET input must be 1472 bytes");
    }
    output = fdopen(create(params().output_file), "w");
    fatal_if(!output, "Cannot open i40e host output");
    event("scope", "\"population\":\"test_host_initiated_pio_and_memory\","
        "\"pci_dma_base\":" + std::to_string(params().pci_dma_base) +
        ",\"native_wire_identity\":null,\"producer_op_seq\":null");
    buildProgram();
    schedule(advanceEvent, clockEdge(Cycles(1)));
    schedule(timeoutEvent, curTick() + params().timeout);
}
void I40eObserveHost::advance()
{
    panic_if(actions.empty() || pioPacket || memoryActive, "i40e host action invariant");
    auto &action = actions.front();
    if (action.kind == Kind::Delay) {
        Tick delay = action.delay;
        actions.pop_front();
        schedule(advanceEvent, curTick() + delay);
        return;
    }
    if (action.kind == Kind::Ready) {
        ready = true;
        actions.pop_front();
        event("ready", "\"rx_queue\":1,\"qena_stat\":true,\"descriptor_ready\":null,\"send_authorized\":false,\"ring_wire_address\":196608,\"payload_wire_address\":393216");
        schedule(advanceEvent, curTick() + params().poll_interval);
        exitSimLoop("I40E_OBSERVE_READY");
        return;
    }
    if (action.kind == Kind::Done && params().admin_only) {
        event("admin_complete", "\"admin_descriptors_verified\":2,"
            "\"rd_input_bytes_verified\":16,\"packet_injections\":0,"
            "\"test_memory_actions\":" + std::to_string(memoryActions) +
            ",\"test_memory_read_bytes\":" + std::to_string(memoryReadBytes) +
            ",\"test_memory_write_bytes\":" + std::to_string(memoryWriteBytes));
        deschedule(timeoutEvent);
        actions.pop_front();
        exitSimLoop("I40E_ADMIN_OBSERVE_PASS");
        return;
    }
    if (action.kind == Kind::Done) {
        event("complete", "\"payload_bytes_verified\":1472,\"rx_descriptors_verified\":3,"
            "\"test_memory_actions\":" + std::to_string(memoryActions) +
            ",\"test_memory_read_bytes\":" + std::to_string(memoryReadBytes) +
            ",\"test_memory_write_bytes\":" + std::to_string(memoryWriteBytes));
        deschedule(timeoutEvent);
        actions.pop_front();
        exitSimLoop("I40E_OBSERVE_PASS");
        return;
    }
    ++actionSequence;
    const bool isMemory = action.kind == Kind::MemoryRead || action.kind == Kind::MemoryWrite;
    const bool write = action.kind == Kind::MemoryWrite || action.kind == Kind::PioWrite;
    event("issue", "\"population\":\"" + std::string(isMemory ? "test_host_memory" : "test_host_pio") +
        "\",\"label\":" + dmaTraceQuote(action.label) + ",\"physical_address\":" +
        std::to_string(action.address) + ",\"length_bytes\":" + std::to_string(action.bytes.size()) +
        ",\"direction\":\"" + (write ? "write" : "read") + "\"");
    if (isMemory) {
        memoryActive = true;
        ++memoryActions;
        (write ? memoryWriteBytes : memoryReadBytes) += action.bytes.size();
        memory.dmaAction(write ? MemCmd::WriteReq : MemCmd::ReadReq,
            action.address, action.bytes.size(), &memoryDoneEvent, action.bytes.data(), 0);
    } else {
        auto req = std::make_shared<Request>(action.address, action.bytes.size(),
                                             Request::UNCACHEABLE, pioId);
        pioPacket = new Packet(req, write ? MemCmd::WriteReq : MemCmd::ReadReq);
        pioPacket->allocate();
        if (write) pioPacket->setData(action.bytes.data());
        pioRetry = !pio.sendTimingReq(pioPacket);
    }
}
void I40eObserveHost::retryPio()
{
    panic_if(!pioRetry || !pioPacket, "Unexpected i40e host PIO retry");
    pioRetry = !pio.sendTimingReq(pioPacket);
}
void I40eObserveHost::pioResponse(PacketPtr pkt)
{
    panic_if(pkt != pioPacket || pkt->isError(), "i40e host PIO response mismatch/error");
    std::vector<uint8_t> bytes(pkt->getConstPtr<uint8_t>(), pkt->getConstPtr<uint8_t>() + pkt->getSize());
    pioPacket = nullptr;
    delete pkt;
    complete(bytes);
}
void I40eObserveHost::memoryDone()
{
    panic_if(!memoryActive, "Unexpected i40e host memory completion");
    memoryActive = false;
    complete(actions.front().bytes);
}
void I40eObserveHost::complete(const std::vector<uint8_t> &bytes)
{
    auto &action = actions.front();
    bool accepted = !action.check || action.check(bytes);
    event("response", "\"label\":" + dmaTraceQuote(action.label) +
        ",\"condition_satisfied\":" + (accepted ? "true" : "false") +
        (action.kind == Kind::PioRead ? ",\"read_value\":" + std::to_string(get(bytes, 0, 4)) : ""));
    if (accepted) {
        actions.pop_front();
        pollAttempts = 0;
    } else ++pollAttempts;
    schedule(advanceEvent, accepted ? clockEdge(Cycles(1)) : curTick() + params().poll_interval);
}
} // namespace gem5
