/*
 * Copyright (c) 2015 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

#include "dev/pci/host.hh"

#include <algorithm>
#include <utility>
#include <vector>

#include "debug/PciHost.hh"
#include "dev/pci/device.hh"
#include "dev/platform.hh"
#include "params/GenericPciHost.hh"
#include "params/PciHost.hh"
#include "sim/serialize.hh"

namespace gem5
{

PciHost::PciHost(const PciHostParams &p)
    : PioDevice(p)
{
}

PciHost::~PciHost()
{
}

PciHost::DeviceInterface
PciHost::registerDevice(PciDevice *device, PciBusAddr bus_addr, PciIntPin pin)
{
    auto map_entry = devices.emplace(bus_addr, device);

    DPRINTF(PciHost, "%02x:%02x.%i: Registering device\n",
            bus_addr.bus, bus_addr.dev, bus_addr.func);

    fatal_if(!map_entry.second,
             "%02x:%02x.%i: PCI bus ID collision\n",
             bus_addr.bus, bus_addr.dev, bus_addr.func);

    return DeviceInterface(*this, bus_addr, pin);
}

PciDevice *
PciHost::getDevice(const PciBusAddr &addr)
{
    auto device = devices.find(addr);
    return device != devices.end() ? device->second : nullptr;
}

const PciDevice *
PciHost::getDevice(const PciBusAddr &addr) const
{
    auto device = devices.find(addr);
    return device != devices.end() ? device->second : nullptr;
}

PciHost::DeviceInterface::DeviceInterface(
    PciHost &_host,
    PciBusAddr &bus_addr, PciIntPin interrupt_pin)
    : host(_host),
      busAddr(bus_addr), interruptPin(interrupt_pin)
{
}

const std::string
PciHost::DeviceInterface::name() const
{
    return csprintf("%s.interface[%02x:%02x.%i]",
                    host.name(), busAddr.bus, busAddr.dev, busAddr.func);
}

void
PciHost::DeviceInterface::postInt()
{
    DPRINTF(PciHost, "postInt\n");

    host.postInt(busAddr, interruptPin);
}

void
PciHost::DeviceInterface::clearInt()
{
    DPRINTF(PciHost, "clearInt\n");

    host.clearInt(busAddr, interruptPin);
}


GenericPciHost::GenericPciHost(const GenericPciHostParams &p)
    : PciHost(p),
      platform(*p.platform),
      confBase(p.conf_base), confSize(p.conf_size),
      confDeviceBits(p.conf_device_bits),
      pciPioBase(p.pci_pio_base), pciMemBase(p.pci_mem_base),
      pciDmaBase(p.pci_dma_base), aggregateIntx(p.aggregate_intx)
{
}

GenericPciHost::~GenericPciHost()
{
}


Tick
GenericPciHost::read(PacketPtr pkt)
{
    const auto dev_addr(decodeAddress(pkt->getAddr() - confBase));
    const Addr size(pkt->getSize());

    DPRINTF(PciHost, "%02x:%02x.%i: read: offset=0x%x, size=0x%x\n",
            dev_addr.first.bus, dev_addr.first.dev, dev_addr.first.func,
            dev_addr.second,
            size);

    PciDevice *const pci_dev(getDevice(dev_addr.first));
    if (pci_dev) {
        // @todo Remove this after testing
        pkt->headerDelay = pkt->payloadDelay = 0;
        return pci_dev->readConfig(pkt);
    } else {
        uint8_t *pkt_data(pkt->getPtr<uint8_t>());
        std::fill(pkt_data, pkt_data + size, 0xFF);
        pkt->makeAtomicResponse();
        return 0;
    }
}

Tick
GenericPciHost::write(PacketPtr pkt)
{
    const auto dev_addr(decodeAddress(pkt->getAddr() - confBase));

    DPRINTF(PciHost, "%02x:%02x.%i: write: offset=0x%x, size=0x%x\n",
            dev_addr.first.bus, dev_addr.first.dev, dev_addr.first.func,
            dev_addr.second,
            pkt->getSize());

    PciDevice *const pci_dev(getDevice(dev_addr.first));
    warn_if(!pci_dev,
            "%02x:%02x.%i: Write to config space on non-existent PCI device\n",
            dev_addr.first.bus, dev_addr.first.dev, dev_addr.first.func);

    if (!pci_dev) {
        pkt->makeAtomicResponse();
        return 20000; // 20ns default from PciDevice.py
    }

    // @todo Remove this after testing
    pkt->headerDelay = pkt->payloadDelay = 0;

    return pci_dev->writeConfig(pkt);
}

AddrRangeList
GenericPciHost::getAddrRanges() const
{
    return AddrRangeList({ RangeSize(confBase, confSize) });
}

std::pair<PciBusAddr, Addr>
GenericPciHost::decodeAddress(Addr addr)
{
    const Addr offset(addr & mask(confDeviceBits));
    const Addr bus_addr(addr >> confDeviceBits);

    return std::make_pair(
        PciBusAddr(bits(bus_addr, 15, 8),
                   bits(bus_addr,  7, 3),
                   bits(bus_addr,  2, 0)),
        offset);
}


void
GenericPciHost::postInt(const PciBusAddr &addr, PciIntPin pin)
{
    const uint32_t line = mapPciInterrupt(addr, pin);
    if (aggregateIntx) {
        const uint32_t source = intxSource(addr, pin);
        const auto existing = intxSources.find(source);
        if (existing != intxSources.end()) {
            panic_if(existing->second != line,
                     "PCI interrupt route changed while asserted");
            return;
        }
        const bool asserted = std::any_of(
            intxSources.begin(), intxSources.end(),
            [line](const auto &entry) { return entry.second == line; });
        intxSources.emplace(source, line);
        if (asserted)
            return;
    }
    platform.postPciInt(line);
}

void
GenericPciHost::clearInt(const PciBusAddr &addr, PciIntPin pin)
{
    uint32_t line;
    if (aggregateIntx) {
        const auto source = intxSources.find(intxSource(addr, pin));
        if (source == intxSources.end())
            return;
        line = source->second;
        intxSources.erase(source);
        if (std::any_of(intxSources.begin(), intxSources.end(),
                [line](const auto &entry) { return entry.second == line; }))
            return;
    } else {
        line = mapPciInterrupt(addr, pin);
    }
    platform.clearPciInt(line);
}


uint32_t
GenericPciHost::intxSource(const PciBusAddr &addr, PciIntPin pin)
{
    panic_if(pin == PciIntPin::NO_INT, "INTx source has no interrupt pin");
    return (uint32_t{addr.bus} << 16) | (uint32_t{addr.dev} << 8) |
        (uint32_t{addr.func} << 3) | static_cast<uint8_t>(pin);
}

void
GenericPciHost::startup()
{
    PciHost::startup();
    // Device config (including guest-written InterruptLine) is restored
    // independently. Validate routes only after every object was restored.
    for (const auto &entry : intxSources) {
        const auto source = entry.first;
        const PciBusAddr addr((source >> 16) & 0xff,
                             (source >> 8) & 0x1f, (source >> 3) & 7);
        fatal_if(mapPciInterrupt(addr, static_cast<PciIntPin>(source & 7)) !=
                     entry.second,
                 "PCI INTx checkpoint route differs from restored device");
    }
}

void
GenericPciHost::serialize(CheckpointOut &cp) const
{
    PciHost::serialize(cp);
    if (!aggregateIntx)
        return;
    paramOut(cp, "aggregate_intx", aggregateIntx);
    std::vector<uint32_t> sources, lines;
    for (const auto &entry : intxSources) {
        sources.push_back(entry.first);
        lines.push_back(entry.second);
    }
    arrayParamOut(cp, "intx_sources", sources);
    arrayParamOut(cp, "intx_lines", lines);
}

void
GenericPciHost::unserialize(CheckpointIn &cp)
{
    PciHost::unserialize(cp);
    bool checkpoint_aggregate = false;
    optParamIn(cp, "aggregate_intx", checkpoint_aggregate, false);
    fatal_if(checkpoint_aggregate != aggregateIntx,
             "PCI INTx aggregation mode differs from checkpoint");
    if (!aggregateIntx)
        return;
    std::vector<uint32_t> sources, lines;
    arrayParamIn(cp, "intx_sources", sources);
    arrayParamIn(cp, "intx_lines", lines);
    fatal_if(sources.size() != lines.size(), "Invalid PCI INTx checkpoint");
    intxSources.clear();
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto source = sources[i];
        const auto pin = static_cast<PciIntPin>(source & 7);
        const PciBusAddr addr((source >> 16) & 0xff,
                             (source >> 8) & 0x1f, (source >> 3) & 7);
        fatal_if(!getDevice(addr) || pin == PciIntPin::NO_INT ||
                     (source & 7) > 4 || intxSource(addr, pin) != source ||
                     !intxSources.emplace(source, lines[i]).second,
                 "Invalid PCI INTx checkpoint source");
    }
    // The platform's interrupt controller restores its own input state.
}

uint32_t
GenericPciHost::mapPciInterrupt(const PciBusAddr &addr, PciIntPin pin) const
{
    const PciDevice *dev(getDevice(addr));
    assert(dev);

    return dev->interruptLine();
}

} // namespace gem5
