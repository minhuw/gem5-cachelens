/*
 * Copyright (c) 2026 minhuw
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu/testers/split_ruby/split_ruby_tester.hh"

#include <algorithm>
#include <iostream>
#include <limits>

#include "base/cprintf.hh"
#include "base/logging.hh"
#include "sim/system.hh"

namespace gem5
{

bool
SplitRubyDirectedTester::TestPort::recvTimingResp(PacketPtr pkt)
{
    tester.complete(*this, pkt);
    return true;
}

void
SplitRubyDirectedTester::TestPort::recvReqRetry()
{
    tester.retry(*this);
}

SplitRubyDirectedTester::SplitRubyDirectedTester(const Params &p)
    : ClockedObject(p),
      phaseEvent([this] { issuePhase(); }, name() + ".phase"),
      checkEvent([this] { localCheck(); }, name() + ".check"),
      dmaPort(name() + ".dma_port", *this),
      role(p.role), phaseTicks(p.phase_ticks), stopTick(p.stop_tick),
      checkTick(stopTick - phaseTicks / 2),
      requestorId(p.system->getRequestorId(this))
{
    fatal_if(role < -1 || role > 1, "Split Ruby invalid role %d", role);
    fatal_if(phaseTicks < 2 ||
             phaseTicks > std::numeric_limits<Tick>::max() / 9 ||
             stopTick <= phaseTicks / 2 || checkTick <= 8 * phaseTicks,
             "Split Ruby requires eight phases before its local check tick");
    fatal_if(p.port_cpu_ports_connection_count != 2,
             "Split Ruby requires exactly two connected CPU ports");
    for (unsigned i = 0; i < cpuPorts.size(); ++i) {
        cpuPorts[i] = std::make_unique<TestPort>(
            csprintf("%s.cpu_ports[%u]", name(), i), *this, i);
    }

    const std::vector<uint8_t> first(64, 0x11);
    const std::vector<uint8_t> second(64, 0x22);
    auto merged = second;
    std::fill(merged.begin() + 5, merged.begin() + 13, 0xaa);
    operations = {
        {1, 0, false, 0x10000, first, {}},
        {2, 1, true,  0x10000, first, {}},
        {3, 1, false, 0x10000, second, {}},
        {4, 0, true,  0x10000, second, {}},
        {5, 2, true,  0x10000, second, {}},
        {6, 2, false, 0x10005, std::vector<uint8_t>(8, 0xaa), {}},
        {7, 0, true,  0x10000, merged, {}},
        {8, 1, true,  0x10000, merged, {}},
    };
}

SplitRubyDirectedTester::~SplitRubyDirectedTester()
{
    // Accepted packets belong to the memory system until their response.
    for (auto &op : operations) {
        if (op.packet && !op.accepted)
            delete op.packet;
    }
}

Port &
SplitRubyDirectedTester::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cpu_ports") {
        fatal_if(idx < 0 || idx >= static_cast<PortID>(cpuPorts.size()),
                 "Split Ruby invalid CPU port index %d", idx);
        return *cpuPorts[idx];
    }
    if (if_name == "dma_port")
        return dmaPort;
    return ClockedObject::getPort(if_name, idx);
}

bool
SplitRubyDirectedTester::owns(const Operation &op) const
{
    return role == -1 || role == (op.port == 0 ? 0 : 1);
}

SplitRubyDirectedTester::TestPort &
SplitRubyDirectedTester::portFor(const Operation &op)
{
    return op.port == 2 ? dmaPort : *cpuPorts.at(op.port);
}

void
SplitRubyDirectedTester::startup()
{
    fatal_if(curTick() != 0,
             "Split Ruby directed tester requires a fresh tick-zero run");
    fatal_if(!dmaPort.isConnected(), "Split Ruby DMA port must be connected");
    for (const auto &port : cpuPorts) {
        fatal_if(!port->isConnected(),
                 "Split Ruby CPU ports must be connected in every role");
    }
    schedule(phaseEvent, phaseTicks);
    schedule(checkEvent, checkTick);
}

void
SplitRubyDirectedTester::issuePhase()
{
    fatal_if(localPassed || nextPhase < 1 || nextPhase > operations.size() ||
             curTick() != nextPhase * phaseTicks,
             "Split Ruby role %d bad phase %u at tick %llu", role, nextPhase,
             static_cast<unsigned long long>(curTick()));
    for (const auto &prior : operations) {
        fatal_if(owns(prior) && prior.phase < nextPhase &&
                 (prior.completions != 1 || prior.packet || prior.retryPending),
                 "Split Ruby role %d phase %u timed out before phase %u",
                 role, prior.phase, nextPhase);
    }

    auto &op = operations.at(nextPhase - 1);
    if (owns(op)) {
        fatal_if(op.attempted || op.packet || op.completions,
                 "Split Ruby role %d phase %u issued twice", role, op.phase);
        op.attempted = true;
        op.request = std::make_shared<Request>(
            op.address, op.data.size(), Request::Flags(), requestorId);
        op.packet = new Packet(
            op.request, op.read ? MemCmd::ReadReq : MemCmd::WriteReq);
        op.packet->allocate();
        if (!op.read) {
            std::copy(op.data.begin(), op.data.end(),
                      op.packet->getPtr<uint8_t>());
        }
        op.accepted = portFor(op).sendTimingReq(op.packet);
        op.retryPending = !op.accepted;
    }
    ++nextPhase;
    if (nextPhase <= operations.size())
        schedule(phaseEvent, nextPhase * phaseTicks);
}

void
SplitRubyDirectedTester::retry(TestPort &port)
{
    Operation *pending = nullptr;
    for (auto &op : operations) {
        if (op.retryPending && &portFor(op) == &port) {
            fatal_if(pending, "Split Ruby multiple retries on %s", port.name());
            pending = &op;
        }
    }
    fatal_if(!pending, "Split Ruby unexpected retry on %s", port.name());
    auto &op = *pending;
    fatal_if(!owns(op) || !op.attempted || !op.packet || op.accepted ||
             op.completions || curTick() >= (op.phase + 1) * phaseTicks ||
             curTick() >= checkTick,
             "Split Ruby role %d phase %u invalid or late retry", role,
             op.phase);
    op.accepted = port.sendTimingReq(op.packet);
    op.retryPending = !op.accepted;
}

void
SplitRubyDirectedTester::complete(TestPort &port, PacketPtr pkt)
{
    // Keep RequestPtrs alive after completion, so neither request identity nor
    // packet-address reuse can make a duplicate look like a later operation.
    auto found = std::find_if(operations.begin(), operations.end(),
        [pkt](const Operation &op) {
            return op.packet == pkt && op.request == pkt->req;
        });
    fatal_if(found == operations.end(),
             "Split Ruby role %d unexpected or duplicate response", role);
    auto &op = *found;
    fatal_if(!owns(op) || &portFor(op) != &port || !op.attempted ||
             !op.accepted || op.retryPending || op.completions != 0 ||
             !pkt->isResponse() || pkt->isError() ||
             pkt->cmd != (op.read ? MemCmd::ReadResp : MemCmd::WriteResp) ||
             pkt->getAddr() != op.address || pkt->getSize() != op.data.size(),
             "Split Ruby role %d phase %u invalid response", role, op.phase);
    fatal_if(curTick() < op.phase * phaseTicks ||
             curTick() >= (op.phase + 1) * phaseTicks ||
             curTick() >= checkTick,
             "Split Ruby role %d phase %u response outside phase", role,
             op.phase);

    if (op.read) {
        fatal_if(!pkt->hasData(),
                 "Split Ruby role %d phase %u missing read data", role,
                 op.phase);
        const auto *actual = pkt->getConstPtr<uint8_t>();
        for (size_t i = 0; i < op.data.size(); ++i) {
            fatal_if(actual[i] != op.data[i],
                     "Split Ruby role %d phase %u byte %zu got %#x expected %#x",
                     role, op.phase, i, actual[i], op.data[i]);
        }
    }

    std::string hex;
    for (const auto byte : op.data)
        hex += csprintf("%02x", static_cast<unsigned>(byte));
    std::cout << "SPLIT_RUBY_COMPLETE role=" << role
              << " phase=" << op.phase << " tick=" << curTick()
              << " issue_tick=" << op.phase * phaseTicks
              << " port=" << (op.port == 2 ? "dma" :
                               op.port == 0 ? "cpu0" : "cpu1")
              << " op=" << (op.read ? "read" : "write")
              << " addr=" << csprintf("%#x", op.address)
              << " data=" << hex << std::endl;
    ++op.completions;
    op.packet = nullptr;
    delete pkt;
}

void
SplitRubyDirectedTester::checkOperations() const
{
    fatal_if(nextPhase != operations.size() + 1 || phaseEvent.scheduled(),
             "Split Ruby role %d has not visited every phase", role);
    for (const auto &op : operations) {
        fatal_if(op.packet || op.retryPending,
                 "Split Ruby role %d phase %u has an outstanding request/retry",
                 role, op.phase);
        fatal_if(owns(op) ? (!op.attempted || !op.accepted ||
                            op.completions != 1) :
                           (op.attempted || op.accepted || op.completions),
                 "Split Ruby role %d phase %u completion/ownership violation",
                 role, op.phase);
    }
}

void
SplitRubyDirectedTester::localCheck()
{
    fatal_if(curTick() != checkTick || localPassed,
             "Split Ruby role %d invalid local check", role);
    checkOperations();
    localPassed = true;
    const unsigned planned = std::count_if(
        operations.begin(), operations.end(),
        [this](const Operation &op) { return owns(op); });
    std::cout << "SPLIT_RUBY_LOCAL_PASS role=" << role
              << " tick=" << curTick() << " phases=" << planned << std::endl;
    // The bridge alone negotiates and exits at the global stop tick.
}

void
SplitRubyDirectedTester::assertComplete() const
{
    fatal_if(!localPassed || curTick() < checkTick || curTick() != stopTick,
             "Split Ruby role %d did not reach the agreed completed stop",
             role);
    checkOperations();
}

} // namespace gem5
