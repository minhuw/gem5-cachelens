/*
 * Copyright (c) 2026 minhuw
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/testers/mesi_ddio/mesi_ddio_tester.hh"

#include <algorithm>

#include "base/cprintf.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/MESIDDIOTester.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5
{

bool
MESIDDIODirectedTester::TestPort::recvTimingResp(PacketPtr pkt)
{
    tester.complete(*this, pkt);
    return true;
}

void
MESIDDIODirectedTester::TestPort::recvReqRetry()
{
    tester.retry(*this);
}

MESIDDIODirectedTester::MESIDDIODirectedTester(const Params &params)
    : ClockedObject(params),
      issueEvent([this] { issue(); }, name() + ".issue"),
      finishEvent([this] { finish(); }, name() + ".finish"),
      timeoutEvent([this] { timeout(); }, name() + ".timeout"),
      dmaPort(name() + ".dma_port", *this),
      scenario(params.scenario),
      baseAddress(params.base_addr),
      setStride(params.set_stride),
      blockSize(params.block_size),
      routingBanks(params.routing_banks),
      responseTimeout(params.response_timeout),
      completionQuietPeriod(params.completion_quiet_period),
      requestorId(params.system->getRequestorId(this))
{
    fatal_if(blockSize == 0 || !isPowerOf2(blockSize),
             "MESI DDIO tester block_size must be a non-zero power of two");
    fatal_if(baseAddress & (blockSize - 1),
             "MESI DDIO tester base address %#x is not line aligned",
             baseAddress);
    fatal_if(setStride < blockSize || (setStride % blockSize) != 0,
             "MESI DDIO tester set stride %#x is not line aligned",
             setStride);
    fatal_if(routingBanks == 0 || !isPowerOf2(routingBanks),
             "MESI DDIO tester routing_banks must be a power of two");

    for (int id = 0; id < params.port_cpu_ports_connection_count; ++id) {
        cpuPorts.push_back(new TestPort(
            csprintf("%s.cpu_ports[%d]", name(), id), *this, id));
    }
    fatal_if(cpuPorts.size() != 2,
             "MESI DDIO directed scenarios require exactly two CPU ports");

    retries.reserve(cpuPorts.size() + 1);
    for (auto *port : cpuPorts)
        retries.push_back({port, nullptr, 0});
    retries.push_back({&dmaPort, nullptr, 0});

    buildScenario();
}

MESIDDIODirectedTester::~MESIDDIODirectedTester()
{
    for (auto &op : operations) {
        if (op.packet)
            delete op.packet;
    }
    for (auto *port : cpuPorts)
        delete port;
}

Port &
MESIDDIODirectedTester::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cpu_ports") {
        fatal_if(idx < 0 || idx >= cpuPorts.size(),
                 "Invalid MESI DDIO CPU port index %d", idx);
        return *cpuPorts[idx];
    }
    if (if_name == "dma_port")
        return dmaPort;
    return ClockedObject::getPort(if_name, idx);
}

void
MESIDDIODirectedTester::startup()
{
    phaseStart = clockEdge(Cycles(1));
    schedule(issueEvent, phaseStart);
}

std::vector<uint8_t>
MESIDDIODirectedTester::bytes(unsigned size, uint8_t seed) const
{
    std::vector<uint8_t> data(size);
    for (unsigned i = 0; i < size; ++i)
        data[i] = seed + i;
    return data;
}

std::vector<uint8_t>
MESIDDIODirectedTester::pattern(uint8_t seed) const
{
    return bytes(blockSize, seed);
}

unsigned
MESIDDIODirectedTester::reservePhase()
{
    return phaseCount++;
}

void
MESIDDIODirectedTester::addReadAt(
    unsigned phase, unsigned issue_delay, PortKind port, unsigned cpu,
    Addr address, Request::Flags flags, const std::vector<uint8_t> &expected,
    const std::string &label)
{
    fatal_if(expected.empty(),
             "Read operation %s has no expected bytes", label);
    operations.push_back({phase, issue_delay, port, cpu, true, address, flags,
                          expected, label, {}});
}

void
MESIDDIODirectedTester::addWriteAt(
    unsigned phase, unsigned issue_delay, PortKind port, unsigned cpu,
    Addr address, Request::Flags flags, const std::vector<uint8_t> &data,
    const std::string &label)
{
    addMaskedWriteAt(phase, issue_delay, port, cpu, address, flags, data,
                     std::vector<bool>(data.size(), true), label);
}

void
MESIDDIODirectedTester::addMaskedWriteAt(
    unsigned phase, unsigned issue_delay, PortKind port, unsigned cpu,
    Addr address, Request::Flags flags, const std::vector<uint8_t> &data,
    const std::vector<bool> &byte_enable, const std::string &label)
{
    fatal_if(data.empty(), "Write operation %s has no bytes", label);
    fatal_if(byte_enable.size() != data.size(),
             "Write operation %s has %zu bytes but %zu byte enables", label,
             data.size(), byte_enable.size());
    operations.push_back({phase, issue_delay, port, cpu, false, address, flags,
                          data, label, byte_enable});
}

void
MESIDDIODirectedTester::addRead(
    PortKind port, unsigned cpu, Addr address, Request::Flags flags,
    const std::vector<uint8_t> &expected, const std::string &label)
{
    addReadAt(reservePhase(), 0, port, cpu, address, flags, expected, label);
}

void
MESIDDIODirectedTester::addWrite(
    PortKind port, unsigned cpu, Addr address, Request::Flags flags,
    const std::vector<uint8_t> &data, const std::string &label)
{
    addWriteAt(reservePhase(), 0, port, cpu, address, flags, data, label);
}

void
MESIDDIODirectedTester::addMaskedWrite(
    PortKind port, unsigned cpu, Addr address, Request::Flags flags,
    const std::vector<uint8_t> &data,
    const std::vector<bool> &byte_enable, const std::string &label)
{
    addMaskedWriteAt(reservePhase(), 0, port, cpu, address, flags, data,
                     byte_enable, label);
}

void
MESIDDIODirectedTester::buildScenario()
{
    const Request::Flags generic;
    const Request::Flags rxPayload(Request::NIC_RX_PAYLOAD_WRITE);
    const Request::Flags rxHeader(Request::NIC_RX_HEADER_WRITE);
    const Request::Flags txPayload(Request::NIC_TX_PAYLOAD_READ);
    const Request::Flags rxDescRead(Request::NIC_RX_DESC_READ);
    const Request::Flags rxDescWrite(Request::NIC_RX_DESC_WRITEBACK);

    const Addr a = baseAddress;
    const Addr b = baseAddress + setStride;
    const Addr c = baseAddress + 2 * setStride;
    const Addr d = baseAddress + 3 * setStride;
    const Addr e = baseAddress + 4 * setStride;
    const auto first = pattern(0x10);
    const auto second = pattern(0x80);
    const auto third = pattern(0x40);
    const auto fourth = pattern(0xa0);
    const auto fifth = pattern(0xd0);

    if (scenario == "cold_full_write") {
        addWrite(PortKind::Dma, 0, a, rxPayload, first, "cold RX write");
        addRead(PortKind::Dma, 0, a, generic, first,
                "generic directory read after RX ACK");
    } else if (scenario == "hit_update") {
        addWrite(PortKind::Dma, 0, a, rxPayload, first, "cold RX write");
        addWrite(PortKind::Dma, 0, a, rxPayload, second,
                 "RX full-line hit update");
        addRead(PortKind::Dma, 0, a, txPayload, second,
                "TX observes RX hit update");
    } else if (scenario == "outside_subset_hit") {
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize way-zero line");
        addWrite(PortKind::Dma, 0, b, generic, second,
                 "initialize target line");
        addRead(PortKind::Cpu, 0, a, generic, {first[0]},
                "fill first L2 way from CPU");
        addRead(PortKind::Cpu, 0, b, generic, {second[0]},
                "fill target outside DDIO subset");
        addWrite(PortKind::Dma, 0, b, rxPayload, third,
                 "RX hit outside DDIO subset");
        addRead(PortKind::Dma, 0, b, txPayload, third,
                "TX observes outside-subset hit update");
    } else if (scenario == "subset_full_victim") {
        auto ownerData = first;
        ownerData[7] = 0xee;
        addWrite(PortKind::Dma, 0, a, rxPayload, first,
                 "fill only DDIO way");
        addRead(PortKind::Cpu, 0, a, generic, {first[0]},
                "create private owner for DDIO victim");
        addWrite(PortKind::Cpu, 0, a + 7, generic, {ownerData[7]},
                 "dirty private DDIO victim owner");
        addWrite(PortKind::Dma, 0, b, rxPayload, second,
                 "replace full DDIO subset victim");
        addRead(PortKind::Cpu, 0, a + 7, generic, {ownerData[7]},
                "read back inclusive dirty victim");
        addRead(PortKind::Dma, 0, b, txPayload, second,
                "replacement line remains retained");
    } else if (scenario == "l1_sharer_invalidation") {
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize shared line");
        addRead(PortKind::Cpu, 0, a, generic, {first[0]},
                "CPU zero reads shared line");
        addRead(PortKind::Cpu, 1, a, generic, {first[0]},
                "CPU one creates second sharer");
        addWrite(PortKind::Dma, 0, a, rxPayload, second,
                 "RX invalidates all L1 sharers");
        addRead(PortKind::Cpu, 0, a, generic, {second[0]},
                "CPU zero observes invalidation");
        addRead(PortKind::Cpu, 1, a + 1, generic, {second[1]},
                "CPU one observes invalidation");
    } else if (scenario == "disabled_ddio") {
        auto dirty = first;
        dirty[7] = 0xee;
        addWrite(PortKind::Dma, 0, a, rxPayload, first,
                 "cold write-through RX line");
        addWrite(PortKind::Dma, 0, b, generic, first,
                 "initialize retained line before disabled DDIO hit");
        addRead(PortKind::Cpu, 0, b, generic, {first[0]},
                "create private owner before disabled DDIO hit");
        addWrite(PortKind::Cpu, 0, b + 7, generic, {dirty[7]},
                 "dirty owner before disabled DDIO hit");
        addWrite(PortKind::Dma, 0, b, rxPayload, second,
                 "hit write-through invalidates retained copies");
        addRead(PortKind::Dma, 0, a, txPayload, first,
                "cold-write memory readback without retention");
        addRead(PortKind::Dma, 0, b, txPayload, second,
                "hit-write memory readback without retention");
        addRead(PortKind::Dma, 0, b, txPayload, second,
                "second TX miss proves no allocation");
    } else if (scenario == "tx_hit_retains") {
        addWrite(PortKind::Dma, 0, a, rxPayload, first,
                 "seed retained TX line");
        addRead(PortKind::Dma, 0, a, txPayload, first, "first TX hit");
        addRead(PortKind::Dma, 0, a, txPayload, first,
                "second TX hit remains resident");
    } else if (scenario == "tx_miss_no_allocate") {
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize memory without L2 allocation");
        addRead(PortKind::Dma, 0, a, txPayload, first, "first TX miss");
        addRead(PortKind::Dma, 0, a, txPayload, first,
                "second TX miss proves no allocation");
    } else if (scenario == "dirty_owner_tx") {
        auto dirty = first;
        dirty[7] = 0xee;
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize dirty-owner line");
        addRead(PortKind::Cpu, 0, a, generic, {first[0]},
                "create exclusive private owner");
        addWrite(PortKind::Cpu, 0, a + 7, generic, {dirty[7]},
                 "dirty private owner");
        addRead(PortKind::Dma, 0, a, txPayload, dirty,
                "TX retrieves dirty owner data");
        addRead(PortKind::Dma, 0, a, txPayload, dirty,
                "retrieved line is retained in L2");
    } else if (scenario == "telemetry_exactness") {
        const Addr txMiss = a + blockSize;
        const Addr header = a + 2 * blockSize;
        const Addr desc = a + 3 * blockSize;
        addWrite(PortKind::Dma, 0, a, rxPayload, first,
                 "telemetry RX miss");
        addWrite(PortKind::Dma, 0, a, rxPayload, second,
                 "telemetry RX hit");
        addRead(PortKind::Dma, 0, a, txPayload, second,
                "telemetry TX hit");
        addWrite(PortKind::Dma, 0, txMiss, generic, third,
                 "initialize TX miss memory");
        addRead(PortKind::Dma, 0, txMiss, txPayload, third,
                "telemetry TX miss");
        addWrite(PortKind::Dma, 0, header, rxHeader, first,
                 "telemetry RX header fill");
        addWrite(PortKind::Dma, 0, desc, rxDescWrite, second,
                 "telemetry descriptor fill");
        addRead(PortKind::Dma, 0, desc, rxDescRead, second,
                "telemetry descriptor access");
    } else if (scenario == "generic_partial") {
        const Addr next = a + blockSize;
        auto left = first;
        auto right = second;
        const auto oneByte = bytes(1, 0xee);
        const auto unaligned = bytes(13, 0xa0);
        const auto crossLine = bytes(16, 0xc0);
        left[7] = oneByte[0];
        std::copy(unaligned.begin(), unaligned.end(), left.begin() + 17);
        std::copy(crossLine.begin(), crossLine.begin() + 8,
                  left.end() - 8);
        std::copy(crossLine.begin() + 8, crossLine.end(), right.begin());

        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize first generic partial line");
        addWrite(PortKind::Dma, 0, next, generic, second,
                 "initialize second generic partial line");
        addWrite(PortKind::Dma, 0, a + 7, generic, oneByte,
                 "generic one-byte DMA write");
        addWrite(PortKind::Dma, 0, a + 17, generic, unaligned,
                 "generic unaligned DMA write");
        addWrite(PortKind::Dma, 0, a + blockSize - 8, generic, crossLine,
                 "generic cross-line DMA write");
        addRead(PortKind::Dma, 0, a, generic, left,
                "generic first-line partial DMA readback");
        addRead(PortKind::Dma, 0, next, generic, right,
                "generic second-line partial DMA readback");
    } else if (scenario == "generic_partial_dirty_owner") {
        auto merged = first;
        merged[1] = 0x92;
        merged[2] = 0x75;
        merged[4] = 0x39;
        const std::vector<uint8_t> sparse = {0x75, 0x66, 0x39, 0x44};
        const std::vector<bool> sparseMask = {true, false, true, false};
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize generic dirty-owner line");
        addRead(PortKind::Cpu, 0, a, generic, {first[0]},
                "create private owner before generic partial write");
        addWrite(PortKind::Cpu, 0, a + 1, generic, {merged[1]},
                 "dirty owner byte preserved across DMA write");
        addMaskedWrite(PortKind::Dma, 0, a + 2, generic, sparse, sparseMask,
                       "generic sparse DMA write into dirty owner");
        addRead(PortKind::Dma, 0, a, generic, merged,
                "merged generic dirty-owner DMA readback");
    } else if (scenario == "partial_one_byte") {
        auto merged = first;
        merged[0] = 0x11;
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize clean memory for aligned partial write");
        addWrite(PortKind::Dma, 0, a, rxPayload, {merged[0]},
                 "aligned one-byte retained NIC write");
        addRead(PortKind::Dma, 0, a, txPayload, merged,
                "TX observes aligned one-byte merge");
    } else if (scenario == "partial_unaligned") {
        auto merged = first;
        const auto partial = bytes(13, 0xa0);
        std::copy(partial.begin(), partial.end(), merged.begin() + 17);
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize clean memory for unaligned partial write");
        addWrite(PortKind::Dma, 0, a + 17, rxPayload, partial,
                 "unaligned retained NIC write");
        addRead(PortKind::Dma, 0, a, txPayload, merged,
                "TX observes unaligned merge");
    } else if (scenario == "partial_l2_m") {
        auto merged = first;
        const auto partial = bytes(5, 0xc0);
        std::copy(partial.begin(), partial.end(), merged.begin() + 9);
        addWrite(PortKind::Dma, 0, a, rxPayload, first,
                 "seed retained M line");
        addWrite(PortKind::Dma, 0, a + 9, rxPayload, partial,
                 "partial NIC hit in L2 M");
        addRead(PortKind::Dma, 0, a, txPayload, merged,
                "TX observes L2 M partial merge");
    } else if (scenario == "partial_ss_sharers") {
        auto merged = first;
        const auto partial = bytes(4, 0xd0);
        std::copy(partial.begin(), partial.end(), merged.begin() + 20);
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize clean shared line");
        addRead(PortKind::Cpu, 0, a, generic, {first[0]},
                "create first clean sharer");
        addRead(PortKind::Cpu, 1, a + 1, generic, {first[1]},
                "create second clean sharer");
        addWrite(PortKind::Dma, 0, a + 20, rxPayload, partial,
                 "partial NIC write invalidates SS sharers");
        addRead(PortKind::Dma, 0, a, txPayload, merged,
                "TX observes SS merge");
        addRead(PortKind::Cpu, 0, a + 20, generic, {merged[20]},
                "first CPU observes SS invalidation");
        addRead(PortKind::Cpu, 1, a + 23, generic, {merged[23]},
                "second CPU observes SS invalidation");
    } else if (scenario == "partial_dirty_owner") {
        auto merged = first;
        merged[7] = 0xee;
        const auto partial = bytes(3, 0x70);
        std::copy(partial.begin(), partial.end(), merged.begin() + 2);
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize dirty-owner partial line");
        addRead(PortKind::Cpu, 0, a, generic, {first[0]},
                "create private owner before partial NIC write");
        addWrite(PortKind::Cpu, 0, a + 7, generic, {merged[7]},
                 "dirty untouched owner byte");
        addWrite(PortKind::Dma, 0, a + 2, rxPayload, partial,
                 "partial NIC write retrieves dirty owner");
        addRead(PortKind::Dma, 0, a, txPayload, merged,
                "TX observes owner data before NIC merge");
    } else if (scenario == "partial_no_retention") {
        auto cleanMerged = first;
        cleanMerged[3] = 0x51;
        auto dirtyMerged = second;
        dirtyMerged[7] = 0xee;
        dirtyMerged[2] = 0x62;
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize clean no-retention memory line");
        addWrite(PortKind::Dma, 0, b, generic, second,
                 "initialize dirty-owner no-retention line");
        addRead(PortKind::Cpu, 0, b, generic, {second[0]},
                "create no-retention private owner");
        addWrite(PortKind::Cpu, 0, b + 7, generic, {dirtyMerged[7]},
                 "dirty no-retention owner byte");
        addWrite(PortKind::Dma, 0, a + 3, rxPayload, {cleanMerged[3]},
                 "clean no-retention partial NIC write");
        addWrite(PortKind::Dma, 0, b + 2, rxPayload, {dirtyMerged[2]},
                 "dirty-owner no-retention partial NIC write");
        addRead(PortKind::Dma, 0, a, txPayload, cleanMerged,
                "memory readback of clean no-retention merge");
        addRead(PortKind::Dma, 0, b, txPayload, dirtyMerged,
                "memory readback of dirty-owner no-retention merge");
    } else if (scenario == "partial_cross_line") {
        const Addr next = a + blockSize;
        auto left = first;
        auto right = second;
        const auto partial = bytes(16, 0x30);
        std::copy(partial.begin(), partial.begin() + 8, left.end() - 8);
        std::copy(partial.begin() + 8, partial.end(), right.begin());
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize first cross-line memory line");
        addWrite(PortKind::Dma, 0, next, generic, second,
                 "initialize second cross-line memory line");
        addWrite(PortKind::Dma, 0, a + blockSize - 8, rxPayload, partial,
                 "contiguous cross-line retained NIC write");
        addRead(PortKind::Dma, 0, a, txPayload, left,
                "TX observes first cross-line fragment");
        addRead(PortKind::Dma, 0, next, txPayload, right,
                "TX observes second cross-line fragment");
    } else if (scenario == "sparse_mask") {
        const Addr middle = a + blockSize;
        const Addr last = a + 2 * blockSize;
        auto firstMerged = first;
        auto lastMerged = third;
        const auto localData = bytes(8, 0x90);
        std::vector<bool> localMask(8, false);
        for (const int byte : {0, 3, 7}) {
            localMask[byte] = true;
            firstMerged[8 + byte] = localData[byte];
        }
        const auto spanningData = bytes(72, 0xb0);
        std::vector<bool> spanningMask(72, false);
        for (const int byte : {0, 3, 68, 71})
            spanningMask[byte] = true;
        firstMerged[60] = spanningData[0];
        firstMerged[63] = spanningData[3];
        lastMerged[0] = spanningData[68];
        lastMerged[3] = spanningData[71];

        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize first sparse memory line");
        addWrite(PortKind::Dma, 0, middle, generic, second,
                 "initialize all-disabled middle sparse line");
        addWrite(PortKind::Dma, 0, last, generic, third,
                 "initialize last sparse memory line");
        addMaskedWrite(PortKind::Dma, 0, a + 8, rxPayload, localData,
                       localMask, "single-line sparse NIC write");
        addMaskedWrite(PortKind::Dma, 0, a + 60, rxPayload, spanningData,
                       spanningMask,
                       "multi-line sparse NIC write with empty middle");
        addRead(PortKind::Dma, 0, a, txPayload, firstMerged,
                "TX observes first sparse line");
        addRead(PortKind::Dma, 0, middle, txPayload, second,
                "TX observes untouched all-disabled middle line");
        addRead(PortKind::Dma, 0, last, txPayload, lastMerged,
                "TX observes last sparse line");
    } else if (scenario == "partial_subset_race") {
        auto victim = first;
        victim[7] = 0xee;
        auto secondMerged = second;
        secondMerged[5] = 0x5b;
        auto thirdMerged = third;
        thirdMerged[9] = 0x6c;
        addWrite(PortKind::Dma, 0, b, generic, second,
                 "initialize first partial replacement target");
        addWrite(PortKind::Dma, 0, c, generic, third,
                 "initialize second partial replacement target");
        addWrite(PortKind::Dma, 0, a, rxPayload, first,
                 "seed partial replacement victim");
        addRead(PortKind::Cpu, 0, a, generic, {first[0]},
                "create private partial replacement victim");
        addWrite(PortKind::Cpu, 0, a + 7, generic, {victim[7]},
                 "dirty partial replacement victim");
        const unsigned phase = reservePhase();
        addWriteAt(phase, 0, PortKind::Dma, 0, b + 5, rxPayload,
                   {secondMerged[5]}, "first partial subset replacement");
        addWriteAt(phase, 1, PortKind::Dma, 0, c + 9, rxPayload,
                   {thirdMerged[9]}, "second partial subset replacement");
        addRead(PortKind::Dma, 0, a, generic, victim,
                "read back dirty partial replacement victim");
        addRead(PortKind::Dma, 0, b, generic, secondMerged,
                "read back first partial replacement target");
        addRead(PortKind::Dma, 0, c, generic, thirdMerged,
                "read back second partial replacement target");
    } else if (scenario == "masked_rejection") {
        const auto maskedData = bytes(4, 0xe0);
        const std::vector<bool> maskedBytes = {true, false, true, false};
        addMaskedWrite(PortKind::Dma, 0, a, generic, maskedData,
                       maskedBytes, "masked DMA capability probe");
    } else if (scenario == "zero_mask") {
        const auto disabledData = bytes(16, 0xf0);
        const std::vector<bool> disabledMask(disabledData.size(), false);
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize zero-mask memory line");
        addMaskedWrite(PortKind::Dma, 0, a + 4, rxPayload, disabledData,
                       disabledMask, "all-disabled NIC write no-op");
        addRead(PortKind::Dma, 0, a, txPayload, first,
                "TX observes zero-mask no-op");
    } else if (scenario == "routing_full_line") {
        for (unsigned bank = 0; bank < routingBanks; ++bank) {
            const Addr address = a + bank * blockSize;
            const auto data = pattern(0x20 + 0x20 * bank);
            addWrite(PortKind::Dma, 0, address, rxPayload, data,
                     csprintf("classified full-line write for bank %u", bank));
        }
        for (unsigned bank = 0; bank < routingBanks; ++bank) {
            const Addr address = a + bank * blockSize;
            const auto data = pattern(0x20 + 0x20 * bank);
            addRead(PortKind::Dma, 0, address, txPayload, data,
                    csprintf("classified full-line read for bank %u", bank));
        }
    } else if (scenario == "overlap_dirty_write") {
        auto dirty = first;
        dirty[7] = 0xee;
        addWrite(PortKind::Dma, 0, a, rxPayload, first,
                 "initialize overlapping write line");
        addRead(PortKind::Cpu, 0, a, generic, {first[0]},
                "create private owner before overlapping write");
        addWrite(PortKind::Cpu, 0, a + 7, generic, {dirty[7]},
                 "dirty private owner before overlapping write");
        const unsigned phase = reservePhase();
        addWriteAt(phase, 0, PortKind::Cpu, 1, a + 11, generic, {0x5a},
                   "overlapping CPU ownership transfer");
        addWriteAt(phase, 0, PortKind::Dma, 0, a, rxPayload, second,
                   "overlapping full-line DDIO write");
        addRead(PortKind::Dma, 0, a, generic, second,
                "generic final read after overlapping DDIO write");
    } else if (scenario == "concurrent_subset_victim") {
        auto ownerData = first;
        ownerData[7] = 0xee;
        addWrite(PortKind::Dma, 0, a, rxPayload, first,
                 "seed concurrent DDIO victim");
        addRead(PortKind::Cpu, 0, a, generic, {first[0]},
                "create private concurrent victim owner");
        addWrite(PortKind::Cpu, 0, a + 7, generic, {ownerData[7]},
                 "dirty concurrent victim owner");
        const unsigned phase = reservePhase();
        // The one-cycle offset is much shorter than the inclusive victim
        // invalidation/writeback round trip. Therefore the second accepted
        // request reaches the same DDIO subset while the first replacement
        // is still stalled. The directed stats require both replacement
        // stalls, and the readbacks below verify the resulting commit order.
        addWriteAt(phase, 0, PortKind::Dma, 0, b, rxPayload, second,
                   "first concurrent same-set DDIO replacement");
        addWriteAt(phase, 1, PortKind::Dma, 0, c, rxPayload, third,
                   "second concurrent same-set DDIO replacement");
        addRead(PortKind::Dma, 0, a, generic, ownerData,
                "generic dirty victim readback");
        addRead(PortKind::Dma, 0, b, generic, second,
                "generic first replacement readback");
        addRead(PortKind::Dma, 0, c, generic, third,
                "generic second replacement readback");
    } else if (scenario == "clean_replacement_dma_read" ||
               scenario == "clean_replacement_dma_write" ||
               scenario == "clean_replacement_dma_partial_write") {
        addWrite(PortKind::Dma, 0, a, generic, first,
                 "initialize clean replacement victim");
        addWrite(PortKind::Dma, 0, b, generic, second,
                 "initialize second same-set line");
        addWrite(PortKind::Dma, 0, c, generic, third,
                 "initialize third same-set line");
        addWrite(PortKind::Dma, 0, d, generic, fourth,
                 "initialize fourth same-set line");
        addWrite(PortKind::Dma, 0, e, generic, fifth,
                 "initialize replacement fill line");

        // Fill one four-way L2 set. The two-way L1 evicts the older clean
        // lines back to L2, leaving A as the clean M-state LRU victim.
        addRead(PortKind::Cpu, 0, a, generic, {first[0]},
                "fill clean victim into L2");
        addRead(PortKind::Cpu, 0, b, generic, {second[0]},
                "fill second clean L2 way");
        addRead(PortKind::Cpu, 0, c, generic, {third[0]},
                "fill third clean L2 way");
        addRead(PortKind::Cpu, 0, d, generic, {fourth[0]},
                "fill fourth clean L2 way");

        const unsigned phase = reservePhase();
        // Generic DMA reaches the directory first. Four cycles later the CPU
        // miss starts A's clean L2 replacement, whose ACK reaches the
        // directory before the outstanding invalidate reaches the L2.
        if (scenario == "clean_replacement_dma_read") {
            addReadAt(phase, 0, PortKind::Dma, 0, a, generic, first,
                      "DMA read racing clean L2 replacement");
        } else if (scenario == "clean_replacement_dma_write") {
            addWriteAt(phase, 0, PortKind::Dma, 0, a, generic, second,
                       "DMA write racing clean L2 replacement");
        } else {
            addWriteAt(phase, 0, PortKind::Dma, 0, a + 7, generic,
                       {second[7]},
                       "partial DMA write racing clean L2 replacement");
        }
        addReadAt(phase, 4, PortKind::Cpu, 1, e, generic, {fifth[0]},
                  "CPU fill completing clean L2 replacement");

        if (scenario == "clean_replacement_dma_write") {
            addRead(PortKind::Dma, 0, a, generic, second,
                    "final DMA read after clean-replacement write race");
        } else if (scenario == "clean_replacement_dma_partial_write") {
            auto merged = first;
            merged[7] = second[7];
            addRead(PortKind::Dma, 0, a, generic, merged,
                    "final DMA read after partial clean-replacement race");
        }
    } else {
        fatal("Unknown MESI DDIO directed scenario '%s'", scenario);
    }
}

MESIDDIODirectedTester::TestPort &
MESIDDIODirectedTester::portFor(const Operation &op)
{
    fatal_if(op.port == PortKind::Cpu && op.cpu >= cpuPorts.size(),
             "Operation %s selects invalid CPU port %u", op.label, op.cpu);
    return op.port == PortKind::Dma ? dmaPort : *cpuPorts[op.cpu];
}

size_t
MESIDDIODirectedTester::retryIndex(const TestPort &port) const
{
    for (size_t i = 0; i < retries.size(); ++i) {
        if (retries[i].port == &port)
            return i;
    }
    return retries.size();
}

bool
MESIDDIODirectedTester::phaseComplete(unsigned phase) const
{
    for (const auto &op : operations) {
        if (op.phase == phase && op.completions != 1)
            return false;
    }
    return true;
}

void
MESIDDIODirectedTester::noteProgress()
{
    if (timeoutEvent.scheduled())
        reschedule(timeoutEvent, clockEdge(responseTimeout));
    else
        schedule(timeoutEvent, clockEdge(responseTimeout));
}

void
MESIDDIODirectedTester::issueOperation(size_t index)
{
    Operation &op = operations[index];
    TestPort &port = portFor(op);
    const size_t retry_index = retryIndex(port);
    fatal_if(retry_index == retries.size(),
             "No retry state for operation %s", op.label);
    fatal_if(retries[retry_index].packet,
             "Operation %s shares a port with an unresolved retry", op.label);

    auto req = std::make_shared<Request>(
        op.address, op.data.size(), op.flags, requestorId);
    if (!op.read) {
        fatal_if(op.byteEnable.size() != op.data.size(),
                 "Write operation %s has an invalid byte-enable mask",
                 op.label);
        req->setByteEnable(op.byteEnable);
    }
    PacketPtr pkt = new Packet(
        req, op.read ? MemCmd::ReadReq : MemCmd::WriteReq);
    auto *data = new uint8_t[op.data.size()];
    if (op.read)
        std::fill(data, data + op.data.size(), 0);
    else
        std::copy(op.data.begin(), op.data.end(), data);
    pkt->dataDynamic(data);
    op.packet = pkt;

    DPRINTF(MESIDDIOTester,
            "Issuing phase %u %s at %#x (%zu bytes) via %s\n",
            op.phase, op.label, op.address, op.data.size(),
            op.port == PortKind::Dma ? "DMA" : "CPU");

    if (!port.sendTimingReq(pkt)) {
        retries[retry_index] = {&port, pkt, index};
        noteProgress();
        return;
    }

    op.issued = true;
    noteProgress();
}

void
MESIDDIODirectedTester::issue()
{
    if (finishing)
        return;

    if (currentPhase == phaseCount) {
        fatal_if(!operations.empty() && !phaseComplete(currentPhase - 1),
                 "MESI DDIO scenario %s reached completion early", scenario);
        if (timeoutEvent.scheduled())
            deschedule(timeoutEvent);
        finishing = true;
        schedule(finishEvent, clockEdge(completionQuietPeriod));
        return;
    }

    if (currentPhase > 0 && !phaseComplete(currentPhase - 1))
        return;

    bool future = false;
    Tick next_issue = MaxTick;
    for (size_t i = 0; i < operations.size(); ++i) {
        Operation &op = operations[i];
        if (op.phase != currentPhase || op.packet)
            continue;

        const Tick ready = phaseStart + cyclesToTicks(Cycles(op.issueDelay));
        TestPort &port = portFor(op);
        const size_t retry_index = retryIndex(port);
        if (retry_index != retries.size() && retries[retry_index].packet)
            continue;

        if (curTick() >= ready) {
            issueOperation(i);
        } else {
            future = true;
            next_issue = std::min(next_issue, ready);
        }
    }

    if (future && !issueEvent.scheduled())
        schedule(issueEvent, next_issue);
}

void
MESIDDIODirectedTester::retry(TestPort &port)
{
    const size_t retry_index = retryIndex(port);
    fatal_if(retry_index == retries.size() ||
             !retries[retry_index].packet,
             "Unexpected request retry on %s", port.name());

    RetryState &retry = retries[retry_index];
    if (!port.sendTimingReq(retry.packet))
        return;

    Operation &op = operations[retry.operation];
    fatal_if(op.packet != retry.packet || op.issued,
             "Invalid retry state for operation %s", op.label);
    op.issued = true;
    retry.packet = nullptr;
    noteProgress();
    if (!issueEvent.scheduled())
        schedule(issueEvent, clockEdge(Cycles(1)));
}

void
MESIDDIODirectedTester::complete(TestPort &port, PacketPtr pkt)
{
    size_t index = operations.size();
    for (size_t i = 0; i < operations.size(); ++i) {
        if (operations[i].packet == pkt) {
            index = i;
            break;
        }
    }
    fatal_if(index == operations.size(),
             "Unexpected or duplicate response for MESI DDIO scenario %s",
             scenario);

    Operation &op = operations[index];
    fatal_if(&portFor(op) != &port,
             "Operation %s completed on the wrong tester port", op.label);
    fatal_if(!op.issued,
             "Operation %s responded before request acceptance", op.label);
    fatal_if(op.completions != 0,
             "Operation %s completed more than once", op.label);
    fatal_if(pkt->isError(), "Operation %s returned a packet error", op.label);

    if (op.read) {
        const uint8_t *actual = pkt->getConstPtr<uint8_t>();
        for (size_t i = 0; i < op.data.size(); ++i) {
            fatal_if(actual[i] != op.data[i],
                     "Operation %s read byte %zu = %#x, expected %#x",
                     op.label, i, actual[i], op.data[i]);
        }
    }

    DPRINTF(MESIDDIOTester, "Completed phase %u %s\n", op.phase, op.label);
    ++op.completions;
    op.packet = nullptr;
    delete pkt;
    noteProgress();

    if (phaseComplete(currentPhase)) {
        ++currentPhase;
        phaseStart = clockEdge(Cycles(1));
        schedule(issueEvent, phaseStart);
    }
}

void
MESIDDIODirectedTester::finish()
{
    for (const auto &retry : retries) {
        fatal_if(retry.packet,
                 "MESI DDIO scenario %s finished with a pending retry",
                 scenario);
    }
    for (const auto &op : operations) {
        fatal_if(op.completions != 1,
                 "Operation %s completed %u times, expected exactly once",
                 op.label, op.completions);
    }

    inform("MESI DDIO directed scenario '%s' passed", scenario);
    exitSimLoop("MESI DDIO directed scenario completed");
}

void
MESIDDIODirectedTester::timeout()
{
    std::string pending;
    for (const auto &op : operations) {
        if (op.completions == 0 && op.phase <= currentPhase) {
            if (!pending.empty())
                pending += ", ";
            pending += op.label;
        }
    }
    panic("MESI DDIO directed scenario '%s' timed out in phase %u: %s",
          scenario, currentPhase, pending);
}

} // namespace gem5
