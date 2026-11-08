// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause
// Conservative Ruby cut: preserve native link timing and bandwidth. Unix
// sockets bootstrap a bounded, lock-free shared-memory mailbox per producer.
#include "mem/ruby/network/simple/SplitRubyBridge.hh"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "arch/generic/isa.hh"
#include "base/logging.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "mem/ruby/network/simple/SplitMessage.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "sim/sim_exit.hh"

namespace gem5 { namespace ruby {
namespace split { void testCodec(RubySystem *); }
namespace {
constexpr uint64_t Magic = 0x53504c4954525542ULL;
constexpr uint64_t Version = 2;
constexpr size_t MaxBatch = 8 * 1024 * 1024;
constexpr uint64_t MaxFrames = 16384;
constexpr int MaxRoles = 8;

void put(std::vector<uint8_t> &out, uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back((value >> shift) & 255);
}

class Reader {
    const std::vector<uint8_t> &data;
    size_t offset = 0;
  public:
    explicit Reader(const std::vector<uint8_t> &bytes) : data(bytes) {}
    uint64_t get()
    {
        fatal_if(data.size() - offset < 8, "Truncated split batch header");
        uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) value = (value << 8) | data[offset++];
        return value;
    }
    std::vector<uint8_t> bytes(size_t count)
    {
        fatal_if(count > data.size() - offset, "Truncated split message");
        std::vector<uint8_t> result(data.begin() + offset, data.begin() + offset + count);
        offset += count;
        return result;
    }
    bool empty() const { return offset == data.size(); }
};
}

// Only lock-free host atomics are used across processes. Each acknowledgement
// has one writer and a separate cache line. No process-local pointers are shared.
struct SplitRubyBridge::SharedRegion {
    struct alignas(64) Ack { uint64_t value; };
    struct alignas(64) Slot {
        uint64_t published, length;
        Ack consumed[MaxRoles];
        alignas(64) uint8_t payload[MaxBatch];
    };
    Slot slots[MaxRoles];
};
static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr),
              "Split shared memory requires lock-free 64-bit host atomics");

SplitRubyBridge::SplitRubyBridge(const Params &p)
    : SimObject(p), ruby(p.ruby_system), buffers(p.buffers), monitors(p.monitor_buffers),
      sources(p.source_roles), destinations(p.destination_roles), role(p.role),
      numRoles(p.num_roles), mode(p.mode), socketPath(p.socket_path),
      configTag(p.config_tag), transport(p.transport), adaptive(p.adaptive),
      quantum(p.quantum), stopTick(p.stop_tick),
      ownedCpus(p.owned_cpus), inactiveCpus(p.inactive_cpus),
      expectedChecksum(p.expected_checksum),
      barrierEvent([this] { barrier(); }, name() + ".safeHorizon", false, Event::Minimum_Pri),
      peerFds(numRoles, -1), producerSequences(numRoles, 0), inputs(numRoles)
{
    fatal_if(mode != "native" && mode != "codec" && mode != "split", "Unknown split mode");
    fatal_if(numRoles < 2 || numRoles > MaxRoles, "Invalid number of split roles");
    fatal_if((mode == "split") != (role >= 0 && role < numRoles) || role < -1,
             "Invalid split role");
    fatal_if(transport != "socket" && transport != "shm", "Unknown split transport");
    fatal_if(mode == "split" && transport == "socket" && numRoles != 2,
             "Socket transport supports exactly two roles");
    fatal_if(!quantum || !stopTick || stopTick % quantum, "Invalid split horizons");
    fatal_if(configTag.empty() || configTag.size() > 4096, "Missing/oversize split contract");
    fatal_if(buffers.empty() || buffers.size() != sources.size() ||
             buffers.size() != destinations.size(), "Invalid split cut vectors");
    fatal_if(monitors.empty(), "No split quiescence monitors");
}

SplitRubyBridge::~SplitRubyBridge()
{
    if (shared) ::munmap(shared, sizeof(SharedRegion));
    for (int fd : peerFds) if (fd >= 0) ::close(fd);
    if (listenFd >= 0) ::close(listenFd);
    if (mode == "split" && role == 0) ::unlink(socketPath.c_str());
}

void SplitRubyBridge::init()
{
    SimObject::init();
    fatal_if(ruby->getProtocolInfo().getName() != "MESI_Three_Level" ||
             ruby->getBlockSizeBytes() != 64 || ruby->getRandomization(),
             "Split prototype requires deterministic 64B MESI_Three_Level");
    for (unsigned i = 0; i < buffers.size(); ++i) {
        fatal_if(sources[i] < 0 || sources[i] >= numRoles || destinations[i] < 0 ||
                 destinations[i] >= numRoles || sources[i] == destinations[i],
                 "Invalid split channel ownership");
        fatal_if(static_cast<const MessageBuffer::Params &>(buffers[i]->params()).buffer_size != 0,
                 "Finite cut buffers need distributed credits; unsupported");
    }
    if (mode == "split") openPeer();
    fatal_if(adaptive && (numMainEventQueues != 1 || eventQueue() != getEventQueue(0)),
             "Adaptive split horizons require one closed local event queue");
}

void SplitRubyBridge::mapShared(int fd)
{
    struct stat status;
    fatal_if(::fstat(fd, &status) || status.st_size != sizeof(SharedRegion),
             "Invalid split shared-memory size");
    void *mapping = ::mmap(nullptr, sizeof(SharedRegion), PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
    fatal_if(mapping == MAP_FAILED, "Cannot map split shared memory");
    shared = static_cast<SharedRegion *>(mapping);
}

void SplitRubyBridge::shareFd(int fd, bool sending)
{
    char token = 'S';
    iovec vector{&token, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &vector; message.msg_iovlen = 1;
    message.msg_control = control; message.msg_controllen = sizeof(control);
    if (sending) {
        auto *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &fd, sizeof(fd));
        fatal_if(::sendmsg(socketFd, &message, MSG_NOSIGNAL) != 1,
                 "Cannot publish split shared-memory descriptor");
    } else {
        fatal_if(::recvmsg(socketFd, &message, MSG_CMSG_CLOEXEC) != 1 || token != 'S' ||
                 (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)),
                 "Cannot receive split shared-memory descriptor");
        auto *header = CMSG_FIRSTHDR(&message);
        fatal_if(!header || header->cmsg_level != SOL_SOCKET ||
                 header->cmsg_type != SCM_RIGHTS || header->cmsg_len != CMSG_LEN(sizeof(int)) ||
                 CMSG_NXTHDR(&message, header), "Invalid split shared-memory descriptor");
        int receivedFd;
        std::memcpy(&receivedFd, CMSG_DATA(header), sizeof(receivedFd));
        mapShared(receivedFd);
        ::close(receivedFd);
    }
}

void SplitRubyBridge::openPeer()
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    fatal_if(socketPath.empty() || socketPath.size() >= sizeof(address.sun_path),
             "Invalid private split socket path");
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    int sharedFd = -1;
    if (role == 0) {
        listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        fatal_if(listenFd < 0 || ::bind(listenFd, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) < 0 || ::listen(listenFd, numRoles) < 0,
                 "Cannot create fresh split listener: %s", std::strerror(errno));
        if (transport == "shm") {
            sharedFd = ::syscall(SYS_memfd_create, "split-ruby", 1 /* MFD_CLOEXEC */);
            fatal_if(sharedFd < 0 || ::ftruncate(sharedFd, sizeof(SharedRegion)),
                     "Cannot create split shared memory");
            mapShared(sharedFd);
            // Begin object lifetime without touching 64 MiB of payload pages.
            new (shared) SharedRegion;
            for (int r = 0; r < numRoles; ++r) {
                shared->slots[r].published = shared->slots[r].length = 0;
                for (auto &ack : shared->slots[r].consumed) ack.value = 0;
            }
        }
    }
    const int connections = role == 0 ? numRoles - 1 : 1;
    for (int connection = 0; connection < connections; ++connection) {
        if (role == 0) {
            pollfd descriptor{listenFd, POLLIN, 0};
            fatal_if(::poll(&descriptor, 1, 30000) != 1, "Split peer connection deadline");
            socketFd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            fatal_if(socketFd < 0, "Cannot accept split peer");
        } else {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            do {
                socketFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                fatal_if(socketFd < 0, "Cannot create split socket");
                if (::connect(socketFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
                    break;
                int error = errno;
                ::close(socketFd); socketFd = -1;
                fatal_if(error != ENOENT && error != ECONNREFUSED,
                         "Split connect failed: %s", std::strerror(error));
                fatal_if(std::chrono::steady_clock::now() >= deadline, "Split connection deadline");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (true);
        }
        timeval timeout{10, 0};
        fatal_if(::setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
                 ::setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)),
                 "Cannot set split peer deadline");
        std::vector<uint8_t> hello;
        for (auto value : {Magic, Version, uint64_t(role), uint64_t(quantum),
                          uint64_t(stopTick), uint64_t(buffers.size()), uint64_t(configTag.size())})
            put(hello, value);
        hello.insert(hello.end(), configTag.begin(), configTag.end());
        put(hello, numRoles); put(hello, adaptive); put(hello, transport == "shm");
        auto peerBytes = exchange(hello);
        Reader peer(peerBytes);
        auto magic = peer.get(), version = peer.get(), peerRole = peer.get();
        fatal_if(magic != Magic || version != Version || peerRole >= uint64_t(numRoles) ||
                 peerRole == uint64_t(role) || (role != 0 && peerRole != 0) ||
                 peerFds[peerRole] >= 0 || peer.get() != quantum ||
                 peer.get() != stopTick || peer.get() != buffers.size(),
                 "Split peer protocol/ownership/clock mismatch");
        auto tag = peer.bytes(peer.get());
        fatal_if(std::string(tag.begin(), tag.end()) != configTag || peer.get() != uint64_t(numRoles) ||
                 peer.get() != uint64_t(adaptive) || peer.get() != uint64_t(transport == "shm") ||
                 !peer.empty(), "Split peer static topology/initial-state contract differs");
        peerFds[peerRole] = socketFd;
        if (transport == "shm") shareFd(sharedFd, role == 0);
    }
    if (sharedFd >= 0) ::close(sharedFd);
}

void SplitRubyBridge::transfer(void *data, size_t size, bool sending)
{
    auto *bytes = static_cast<uint8_t *>(data);
    while (size) {
        ssize_t count = sending ? ::send(socketFd, bytes, size, MSG_NOSIGNAL) :
                                  ::recv(socketFd, bytes, size, 0);
        if (count < 0 && errno == EINTR) continue;
        fatal_if(count <= 0, "Split peer closed/stalled: %s", count == 0 ? "EOF" : std::strerror(errno));
        bytes += count; size -= count;
    }
}

std::vector<uint8_t> SplitRubyBridge::exchange(const std::vector<uint8_t> &data)
{
    fatal_if(data.size() > MaxBatch, "Split batch exceeds prototype bound");
    std::vector<uint8_t> incoming;
    auto transmit = [&] {
        std::vector<uint8_t> size;
        put(size, data.size());
        transfer(size.data(), size.size(), true);
        if (!data.empty()) transfer(const_cast<uint8_t *>(data.data()), data.size(), true);
    };
    auto receive = [&] {
        std::vector<uint8_t> size(8);
        transfer(size.data(), size.size(), false);
        Reader reader(size);
        auto length = reader.get();
        fatal_if(length > MaxBatch, "Invalid split frame length");
        incoming.resize(length);
        if (length) transfer(incoming.data(), incoming.size(), false);
    };
    if (role == 0) { transmit(); receive(); }
    else { receive(); transmit(); }
    return incoming;
}

void SplitRubyBridge::waitValue(uint64_t *address, uint64_t value)
{
    uint64_t spins = 0;
    auto start = std::chrono::steady_clock::time_point{};
    while (__atomic_load_n(address, __ATOMIC_ACQUIRE) != value) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause");
#endif
        if ((++spins & 65535) != 0) continue;
        auto now = std::chrono::steady_clock::now();
        if (start == std::chrono::steady_clock::time_point{}) start = now;
        fatal_if(now - start > std::chrono::seconds(10), "Split shared-memory peer stalled");
        // A peer may close after all terminal acknowledgements have been
        // published. Final waits remain bounded but allow this normal closure.
        if (curTick() == stopTick) continue;
        for (int fd : peerFds) if (fd >= 0) {
            char byte;
            ssize_t count = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            fatal_if(count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR),
                     "Split shared-memory peer closed");
        }
    }
}

void SplitRubyBridge::exchangeShared()
{
    fatal_if(!shared || epoch == std::numeric_limits<uint64_t>::max() || batch.size() > MaxBatch,
             "Invalid shared-memory split epoch/batch");
    auto &own = shared->slots[role];
    for (int peer = 0; peer < numRoles; ++peer) if (peer != role)
        waitValue(&own.consumed[peer].value, epoch);
    ++epoch;
    own.length = batch.size();
    std::memcpy(own.payload, batch.data(), batch.size());
    __atomic_store_n(&own.published, epoch, __ATOMIC_RELEASE);
    // Publish before waiting on anyone else. Each reader acknowledges only
    // after its private copy is complete; producers can then reuse the slot.
    for (int peer = 0; peer < numRoles; ++peer) if (peer != role) {
        auto &slot = shared->slots[peer];
        waitValue(&slot.published, epoch);
        fatal_if(slot.length > MaxBatch, "Invalid shared-memory split frame length");
        inputs[peer].assign(slot.payload, slot.payload + slot.length);
        __atomic_store_n(&slot.consumed[role].value, epoch, __ATOMIC_RELEASE);
    }
    if (curTick() == stopTick)
        for (int peer = 0; peer < numRoles; ++peer) if (peer != role)
            waitValue(&own.consumed[peer].value, epoch);
}

void SplitRubyBridge::startup()
{
    if (mode == "codec") {
        split::testCodec(ruby);
        inform("SPLIT_CODEC_SELFTEST_PASS");
    }
    for (unsigned i = 0; i < buffers.size(); ++i) {
        buffers[i]->setSplitEnqueue([this, i](MsgPtr message, Tick issued, Tick latency,
                bool random, bool warmup, bool bypass) {
            send(i, std::move(message), issued, latency, random, warmup, bypass);
        });
    }
    schedule(barrierEvent, 0);
}

void SplitRubyBridge::send(unsigned channel, MsgPtr message, Tick issued, Tick latency,
                           bool random, bool warmup, bool bypass)
{
    fatal_if(random || warmup || latency < quantum || issued != curTick(),
             "Unsupported random/warmup/short-latency split enqueue");
    fatal_if(latency > MaxTick - issued, "Split arrival overflow");
    if (mode != "split") {
        if (mode == "codec") message = split::decode(split::encode(message), ruby);
        buffers[channel]->enqueue(std::move(message), issued, latency, false, false, bypass, true);
        ++sent; return;
    }
    fatal_if(sources[channel] != role, "Unowned router produced a split message");
    fatal_if(pending.size() >= MaxFrames, "Too many semantic messages in a split horizon");
    pending.push_back(Frame{channel, issued, latency, sent++, bypass, split::encode(message)});
}

void SplitRubyBridge::barrier()
{
    Tick now = curTick();
    if (now == 0) {
        for (auto *cpu : inactiveCpus) {
            fatal_if(cpu->totalInsts(), "Unowned CPU executed before split startup");
            cpu->getContext(0)->suspend();
        }
    }
    Tick next = now + std::min(quantum, stopTick - now);
    if (mode == "split") {
        if (adaptive && pending.empty() && now != stopTick) {
            Tick first = eventQueue()->empty() ? stopTick : std::min(stopTick, eventQueue()->nextTick());
            next = std::max(next, first - first % quantum);
        }
        batch.clear();
        put(batch, Magic); put(batch, Version); put(batch, now); put(batch, next); put(batch, pending.size());
        for (const auto &frame : pending) {
            put(batch, frame.channel); put(batch, frame.issued); put(batch, frame.latency);
            put(batch, frame.sequence); put(batch, frame.bypass); put(batch, frame.message.size());
            batch.insert(batch.end(), frame.message.begin(), frame.message.end());
        }
        if (transport == "shm") exchangeShared();
        else inputs[1 - role] = exchange(batch);
        pending.clear();
        for (int peer = 0; peer < numRoles; ++peer) if (peer != role) {
            Reader reader(inputs[peer]);
            fatal_if(reader.get() != Magic || reader.get() != Version || reader.get() != now,
                     "Split peers disagree on safe horizon");
            auto candidate = reader.get(), count = reader.get();
            const Tick fixedNext = now + std::min(quantum, stopTick - now);
            fatal_if(candidate > stopTick || candidate < fixedNext || candidate % quantum ||
                     ((!adaptive || count) && candidate != fixedNext) || count > MaxFrames,
                     "Invalid split next horizon/message count");
            next = std::min<Tick>(next, candidate);
            for (uint64_t i = 0; i < count; ++i) {
                auto channel = reader.get(), issued = reader.get(), latency = reader.get();
                auto sequence = reader.get(), bypass = reader.get(), size = reader.get();
                fatal_if(channel >= buffers.size() || sources[channel] != peer ||
                         sequence != producerSequences[peer]++ || bypass > 1 || latency < quantum ||
                         issued >= now || latency > MaxTick - issued || issued + latency < now,
                         "Invalid/late/unowned split message");
                auto bytes = reader.bytes(size);
                if (destinations[channel] == role) {
                    auto message = split::decode(bytes, ruby);
                    buffers[channel]->enqueue(message, issued, latency, false, false, bypass, true);
                    ++received;
                }
            }
            fatal_if(!reader.empty(), "Trailing split batch bytes");
            if (now == stopTick)
                fatal_if(count || batch.size() != 40, "Semantic traffic remains at agreed stop");
        }
        if (now != stopTick) skipped += (next - now) / quantum - 1;
    } else {
        // A genuine serial reference needs no intermediate synchronization.
        next = stopTick;
    }
    ++barriers;
    if (now == stopTick) {
        for (auto *buffer : monitors)
            fatal_if(buffer->hasPendingMessages(), "Split stop has pending buffer %s", buffer->name());
        for (auto *cpu : inactiveCpus)
            fatal_if(cpu->totalInsts() || cpu->getContext(0)->status() != ThreadContext::Suspended,
                     "Unowned CPU advanced during split execution");
        for (auto *cpu : ownedCpus) {
            auto *context = cpu->getContext(0);
            auto checksum = context->getReg(RegId(*context->getIsaPtr()->regClasses().at(IntRegClass), 0));
            fatal_if(!cpu->totalInsts() || context->status() != ThreadContext::Suspended ||
                     checksum != expectedChecksum, "Owned O3 workload did not finish correctly");
            std::cout << "SPLIT_O3_COMPLETE role=" << role << " cpu=" << cpu->cpuId()
                      << " insts=" << cpu->totalInsts() << " checksum=" << checksum << std::endl;
        }
        inform("SPLIT_RUBY_BRIDGE role=%d sent=%llu received=%llu barriers=%llu skipped=%llu transport=%s",
               role, (unsigned long long)sent, (unsigned long long)received,
               (unsigned long long)barriers, (unsigned long long)skipped, transport);
        exitSimLoop("Split Ruby prototype completed");
    } else {
        schedule(barrierEvent, next);
    }
}

void SplitRubyBridge::serialize(CheckpointOut &) const
{
    fatal("Split Ruby prototype does not support checkpoints");
}
void SplitRubyBridge::unserialize(CheckpointIn &)
{
    fatal("Split Ruby prototype does not support checkpoint restoration");
}
} }
