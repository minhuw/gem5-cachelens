/*
 * Copyright 2022 Max Planck Institute for Software Systems, and
 * National University of Singapore
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <debug/SimBricksPci.hh>
#include <simbricks/pci.hh>
#include "sim/sim_exit.hh"
#include "sim/core.hh"

#include "base/trace.hh"
#include "base/output.hh"
#include "simbricks/pci_safety.hh"

namespace gem5 {
namespace simbricks {
namespace pci {

constexpr uint16_t PciCommandIntxDisable = 1U << 10;

extern "C" {
#include <simbricks/pcie/if.h>

}

Device::Device(const Params &p)
    : PciDevice(p),
    base::GenericBaseAdapter<SimbricksProtoPcieD2H, SimbricksProtoPcieH2D>::
        Interface(*this),
    adapter(*this, *this, p.sync),
    overridePort(name() + ".pio", *this),
    sync(p.sync),
    writesPosted(true),
    semanticStats(this)
{
    DPRINTF(SimBricksPci, "simbricks-pci: device constructed\n");

    fatal_if(!p.semantic_dma && !p.semantic_dma_trace.empty(),
             "semantic_dma_trace requires semantic_dma");
    if (!p.semantic_dma_trace.empty()) {
        semanticTrace.open(simout.resolve(p.semantic_dma_trace));
        fatal_if(!semanticTrace, "Cannot open semantic DMA trace");
        registerExitCallback([this]() {
            semanticTrace.flush();
            fatal_if(!semanticTrace, "Failed flushing semantic DMA trace");
        });
        semanticTrace << "op_id,timestamp_ps,request_id,kind,queue_id,port_id,packet_id,packet_offset,packet_length,pci_address,physical_address,length,write\n";
    }
    adapter.cfgSetPollInterval(p.poll_interval);
    if (p.listen)
        adapter.listen(p.uxsocket_path, p.shm_path);
    else
        adapter.connect(p.uxsocket_path);
}

Device::SemanticStats::SemanticStats(statistics::Group *parent)
    : statistics::Group(parent, "semanticDma"),
      ADD_STAT(descReadOps, statistics::units::Count::get(), "RX descriptor reads per queue"),
      ADD_STAT(descReadBytes, statistics::units::Byte::get(), "RX descriptor bytes per queue"),
      ADD_STAT(payloadWriteOps, statistics::units::Count::get(), "RX payload writes per queue"),
      ADD_STAT(payloadWriteBytes, statistics::units::Byte::get(), "RX payload bytes per queue"),
      ADD_STAT(untaggedReadBytes, statistics::units::Byte::get(), "Untagged DMA read bytes"),
      ADD_STAT(untaggedWriteBytes, statistics::units::Byte::get(), "Untagged DMA write bytes"),
      ADD_STAT(traceDropped, statistics::units::Count::get(), "DMA rows beyond CSV bound")
{
    for (auto *v : {&descReadOps, &descReadBytes, &payloadWriteOps, &payloadWriteBytes})
        v->init(SIMBRICKS_PCIE_DMA_QUEUES).flags(statistics::total | statistics::nozero);
}

Request::Flags
Device::semanticFlags(const volatile uint8_t *pad, bool write,
                      uint64_t rid, Addr addr, uint64_t len)
{
    if (!params().semantic_dma) return 0;
    uint8_t bytes[30];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = pad[i];
    SimbricksPcieDmaMetadata m;
    panic_if(!SimbricksPcieDmaDecode(bytes, sizeof(bytes), write, len, &m),
             "Invalid PCI semantic DMA metadata");
    Request::Flags flags = 0;
    if (m.kind == SIMBRICKS_PCIE_DMA_RX_DESC) {
        flags = Request::NIC_RX_DESC_READ;
        ++semanticStats.descReadOps[m.queue];
        semanticStats.descReadBytes[m.queue] += len;
    } else if (m.kind == SIMBRICKS_PCIE_DMA_RX_PAYLOAD) {
        flags = Request::NIC_RX_PAYLOAD_WRITE;
        ++semanticStats.payloadWriteOps[m.queue];
        semanticStats.payloadWriteBytes[m.queue] += len;
    } else if (write) semanticStats.untaggedWriteBytes += len;
    else semanticStats.untaggedReadBytes += len;
    const uint64_t id = ++semanticOpId;
    if (semanticTrace.is_open()) {
        if (id <= params().semantic_dma_trace_limit) {
            semanticTrace << id << ',' << curTick() / sim_clock::as_int::ps << ','
                << rid << ',' << unsigned(m.kind) << ',' << m.queue << ','
                << m.port << ',' << m.packet << ',' << m.offset << ',' << m.total
                << ',' << addr << ',' << pciToDma(addr) << ',' << len << ',' << write << '\n';
            panic_if(!semanticTrace, "Failed writing semantic DMA trace");
        } else ++semanticStats.traceDropped;
    }
    return flags;
}

Device::~Device()
{
}

size_t
Device::introOutPrepare(void *data, size_t maxlen)
{
    size_t introlen = sizeof(struct SimbricksProtoPcieHostIntro) +
        (params().semantic_dma ? SIMBRICKS_PCIE_SEMANTIC_INTRO_SIZE : 0);
    assert(introlen <= maxlen);
    memset(data, 0, introlen);
    if (params().semantic_dma)
        SimbricksPcieSemanticIntro(static_cast<uint8_t *>(data) +
                                  sizeof(SimbricksProtoPcieHostIntro));
    return introlen;
}

void
Device::introInReceived(const void *data, size_t len)
{
    struct SimbricksProtoPcieDevIntro *di =
        (struct SimbricksProtoPcieDevIntro *) data;
    panic_if(!SimbricksPcieSemanticIntroValid(data, len, sizeof(*di),
                                            params().semantic_dma),
             "PCI semantic DMA handshake mismatch");

    panic_if(!validIntro(*di), "Invalid SimBricks PCI introduction");
    MSICAP_BASE = MSIXCAP_BASE = 0;
    msicap = {}; msixcap = {};
    config.capabilityPtr = 0;
    config.vendor = di->pci_vendor_id;
    config.device = di->pci_device_id;
    config.revision = di->pci_revision;
    config.classCode = di->pci_class;
    config.subClassCode = di->pci_subclass;
    config.progIF = di->pci_progif;

    // fill in BAR details
    for (int i = 0; i < 6; i++) {
        if (di->bars[i].len > 0) {
            Bar *bar = getSimBricksBar(i);
            assert(bar != NULL);
            bar->setup(*this, i, di->bars[i].len,
                di->bars[i].flags);
        }
    }

    for (int i = 0; i < 6; ++i)
        config.baseAddr[i] = htole(BARs[i]->write(hostInterface, 0));

    // Prepare MSI and MSI-X capabilities as needed (ugh)
    int cap_off = 64;
    uint8_t *next_ptr = nullptr;
    if (di->pci_msi_nvecs > 0) {
        MSICAP_BASE = cap_off;
        if (next_ptr)
            *next_ptr = MSICAP_BASE;
        else
            config.capabilityPtr = MSICAP_BASE;
        cap_off += sizeof(msicap);
        next_ptr = (((uint8_t *) &msicap) + 1);

        msicap.mid = 0x5;
        msicap.mc = 0x80;
        uint8_t x = 0;
        switch (di->pci_msi_nvecs) {
            case 1: x = 0; break;
            case 2: x = 1; break;
            case 4: x = 2; break;
            case 8: x = 3; break;
            case 16: x = 4; break;
            case 32: x = 5; break;
            default:
                panic("Invalid number of msi vectors");
        }
        msicap.mc |= (x << 1);
    }
    if (di->pci_msix_nvecs > 0) {
        MSIXCAP_BASE = cap_off;
        if (next_ptr)
            *next_ptr = MSIXCAP_BASE;
        else
            config.capabilityPtr = MSIXCAP_BASE;
        cap_off += sizeof(msixcap);
        next_ptr = ((uint8_t *) &msixcap + 1);

        msixcap.mxid = 0x11;
        msixcap.mxc = di->pci_msix_nvecs - 1;
        msixcap.mtab = di->pci_msix_table_offset | di->pci_msix_table_bar;
        msixcap.mpba = di->pci_msix_pba_offset | di->pci_msix_pba_bar;

        MSIXTable tmp1 = {{0UL,0UL,0UL,0UL}};
        msix_table.resize(di->pci_msix_nvecs , tmp1);
        MSIXPbaEntry tmp2 = {0};
        int pba_size = di->pci_msix_nvecs / MSIXVECS_PER_PBA;
        if ((di->pci_msix_nvecs % MSIXVECS_PER_PBA) > 0) {
            pba_size++;
        }
        msix_pba.resize(pba_size, tmp2);

        MSIX_TABLE_BAR = di->pci_msix_table_bar;
        MSIX_TABLE_OFFSET = di->pci_msix_table_offset;
        MSIX_TABLE_END = MSIX_TABLE_OFFSET +
                        di->pci_msix_nvecs * sizeof(MSIXTable);
        MSIX_PBA_BAR = di->pci_msix_pba_bar;
        MSIX_PBA_OFFSET = di->pci_msix_pba_offset;
        MSIX_PBA_END = MSIX_PBA_OFFSET + msix_pba.size() * sizeof(MSIXPbaEntry);
    }
}

void
Device::initIfParams(SimbricksBaseIfParams &p)
{
    SimbricksPcieIfDefaultParams(&p);
    p.link_latency = params().link_latency;
    p.sync_interval = params().sync_tx_interval;
}

ResponsePort
&Device::getPioPort()
{
    return overridePort;
}

void
Device::init()
{
    /* not calling parent init on purpose, as that will cause problems because
     * PIO port is not connected */
    if (!overridePort.isConnected())
        panic("Pio port (override) of %s not connected!", name());
    if (!dmaPort.isConnected())
        panic("DMA port (override) of %s not connected!", name());

    adapter.init();
    overridePort.sendRangeChange();
}

void
Device::readAsync(PciPioCompl &comp)
{
    int bar;
    Addr daddr;

    if (!getBAR(comp.pkt->getAddr(), bar, daddr)) {
        panic("Invalid PCI memory address\n");
    }

    DPRINTF(SimBricksPci, "simbricks-pci: sending read addr %x size %x "
            "id %lu bar %d offs %x\n",
            comp.pkt->getAddr(), comp.pkt->getSize(), (uint64_t) &comp,
            bar, daddr);

    panic_if(!within(daddr, comp.pkt->getSize(), BARs[bar]->size()), "PIO read crosses BAR");
    if (readMsix(comp, daddr, bar))
        return;

    /* Send read message */
    volatile union SimbricksProtoPcieH2D *h2d_msg = adapter.outAlloc();
    volatile struct SimbricksProtoPcieH2DRead *read = &h2d_msg->read;
    panic_if(!validDma(daddr, comp.pkt->getSize(), adapter.inPayload()), "Invalid PIO read");
    const uint64_t id = nextRequest++;
    read->req_id = id;
    pending.emplace(id, &comp);
    read->offset = daddr;
    read->len = comp.pkt->getSize();
    read->bar = bar;
    adapter.outSend(h2d_msg, SIMBRICKS_PROTO_PCIE_H2D_MSG_READ);
}

void
Device::writeAsync(PciPioCompl &comp)
{
    int bar;
    Addr daddr;

    if (!getBAR(comp.pkt->getAddr(), bar, daddr)) {
        panic("Invalid PCI memory address\n");
    }

    DPRINTF(SimBricksPci, "simbricks-pci: sending write addr %x size %x "
            "id %lu bar %d offs %x posted %d\n",
            comp.pkt->getAddr(), comp.pkt->getSize(), (uint64_t) &comp,
            bar, daddr, writesPosted);

    panic_if(!within(daddr, comp.pkt->getSize(), BARs[bar]->size()), "PIO write crosses BAR");
    if (writeMsix(comp, daddr, bar))
        return;

    /* Send write message */
    volatile union SimbricksProtoPcieH2D *h2d_msg = adapter.outAlloc();
    volatile struct SimbricksProtoPcieH2DWrite *write = &h2d_msg->write;
    panic_if(!validDma(daddr, comp.pkt->getSize(), adapter.outPayload()), "Invalid PIO write");
    const uint64_t id = nextRequest++;
    write->req_id = id;
    if (!writesPosted) pending.emplace(id, &comp);
    write->offset = daddr;
    write->len = comp.pkt->getSize();
    write->bar = bar;
    memcpy((void *)write->data, comp.pkt->getPtr<uint8_t>(),
            comp.pkt->getSize());

    if (writesPosted) {
        comp.setDone();
        adapter.outSend(h2d_msg, SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITE_POSTED);
    } else {
        adapter.outSend(h2d_msg, SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITE);
    }
}

Tick
Device::read(PacketPtr pkt)
{
    PciPioCompl pc(pkt);

    if (sync)
        panic("simbricks-pci: atomic/functional read in synchronous mode");

    readAsync(pc);

    /* wait for operation to complete */
    while (!pc.done)
        adapter.poll();

    pkt->makeAtomicResponse();
    return 1;
}

Tick
Device::write(PacketPtr pkt)
{
    PciPioCompl pc(pkt);

    if (sync)
        panic("simbricks-pci: atomic/functional write in synchronous mode");

    writeAsync(pc);

    /* wait for operation to complete */
    while (!pc.done)
        adapter.poll();

    pkt->makeAtomicResponse();
    return 1;
}

bool
Device::configAccess(PacketPtr pkt, bool write)
{
    const size_t off = pkt->getAddr() & PCI_CONFIG_SIZE;
    const size_t size = pkt->getSize();
    panic_if((size != 1 && size != 2 && size != 4) ||
             !within(off, size, PCI_CONFIG_SIZE + 1), "Invalid PCI config access");
    if (off < PCI_DEVICE_SPECIFIC) {
        panic_if(!within(off, size, PCI_DEVICE_SPECIFIC) || off % size,
                 "Invalid PCI header access");
        return false;
    }
    uint8_t bytes[4] = {};
    if (write) memcpy(bytes, pkt->getConstPtr<uint8_t>(), size);
    for (size_t i = 0; i < size; ++i) {
        const size_t pos = off + i;
        if (MSICAP_BASE && within(pos - MSICAP_BASE, 1, sizeof(msicap))) {
            auto index = pos - MSICAP_BASE;
            if (write) maskedByte(msicap.data[index], bytes[i], msiMask(index));
            else bytes[i] = msicap.data[index];
        } else if (MSIXCAP_BASE && within(pos - MSIXCAP_BASE, 1, sizeof(msixcap))) {
            auto index = pos - MSIXCAP_BASE;
            if (write) maskedByte(msixcap.data[index], bytes[i], msixMask(index));
            else bytes[i] = msixcap.data[index];
        }
    }
    if (write) {
        unsigned mmc = (msicap.mc >> 1) & 7;
        if (((msicap.mc >> 4) & 7) > mmc)
            msicap.mc = (msicap.mc & ~0x70) | (mmc << 4);
    } else pkt->setData(bytes);
    pkt->makeAtomicResponse();
    return true;
}

Tick
Device::readConfig(PacketPtr pkt)
{
    return configAccess(pkt, false) ? configDelay : PciDevice::readConfig(pkt);
}

Tick
Device::writeConfig(PacketPtr pkt)
{
    bool intx_before = !!(config.command & PciCommandIntxDisable);
    bool msi_before = (msicap.mc & 0x1);
    bool msix_before = (msixcap.mxc & 0x8000);
    bool msix_ready_before = (msixcap.mxc & 0xc000) == 0x8000;

    Tick t = configAccess(pkt, true) ? configDelay : PciDevice::writeConfig(pkt);

    bool intx_after = !!(config.command & PciCommandIntxDisable);
    bool msi_after = (msicap.mc & 0x1);
    bool msix_after = (msixcap.mxc & 0x8000);

    /* send devctrl message if interrupt config changed */
    if (intx_before != intx_after || msi_before != msi_after ||
            msix_before != msix_after)
    {
        volatile union SimbricksProtoPcieH2D *msg = adapter.outAlloc();
        volatile struct SimbricksProtoPcieH2DDevctrl *devctrl = &msg->devctrl;

        devctrl->flags = 0;
        if (!intx_after)
            devctrl->flags |= SIMBRICKS_PROTO_PCIE_CTRL_INTX_EN;
        if (msi_after)
            devctrl->flags |= SIMBRICKS_PROTO_PCIE_CTRL_MSI_EN;
        if (msix_after)
            devctrl->flags |= SIMBRICKS_PROTO_PCIE_CTRL_MSIX_EN;

        adapter.outSend(msg, SIMBRICKS_PROTO_PCIE_H2D_MSG_DEVCTRL);
    }

    // Publish devctrl before delivering interrupts released by this write.
    if (!msix_ready_before)
        replayPendingMsix();

    return t;
}

Device::DMACompl::DMACompl(Device *dev_, uint64_t id_, size_t bufsiz_,
        enum ctype ty_, const std::string &name_)
    : EventFunctionWrapper([this]{ done(); }, name_, true), dev(dev_), id(id_),
    ty(ty_), buf(new uint8_t[bufsiz_]), bufsiz(bufsiz_)
{
}

Device::DMACompl::~DMACompl()
{
    delete[] buf;
}

void
Device::DMACompl::done()
{
    dev->dmaDone(*this);
}

void
Device::dmaDone(DMACompl &comp)
{
    DPRINTF(SimBricksPci, "simbricks-pci: completed DMA id %u\n", comp.id);

    if (comp.ty == DMACompl::READ) {
        volatile union SimbricksProtoPcieH2D *msg = adapter.outAlloc();
        volatile struct SimbricksProtoPcieH2DReadcomp *rc;
        /* read completion */
        rc = &msg->readcomp;
        rc->req_id = comp.id;
        memcpy((void *) rc->data, comp.buf, comp.bufsiz);
        adapter.outSend(msg, SIMBRICKS_PROTO_PCIE_H2D_MSG_READCOMP);
    } else if (comp.ty == DMACompl::WRITE) {
        volatile union SimbricksProtoPcieH2D *msg = adapter.outAlloc();
        volatile struct SimbricksProtoPcieH2DWritecomp *wc;
        /* write completion */
        wc = &msg->writecomp;
        wc->req_id = comp.id;
        adapter.outSend(msg, SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITECOMP);
    } else if (comp.ty == DMACompl::MSI) {
        /* MSI interrupt */
    } else {
        panic("simbricks-pci: invalid completion");
    }
}

void
Device::handleInMsg(volatile union SimbricksProtoPcieD2H *msg)
{
    volatile struct SimbricksProtoPcieD2HRead *read;
    volatile struct SimbricksProtoPcieD2HWrite *write;
    volatile struct SimbricksProtoPcieD2HReadcomp *rc;
    volatile struct SimbricksProtoPcieD2HWritecomp *wc;
    volatile struct SimbricksProtoPcieD2HInterrupt *intr;
    DMACompl *dc;
    PciPioCompl *pc;
    uint64_t rid, addr, len;
    uint8_t ty;

    ty = adapter.inType(msg);
    switch (ty) {
        case SIMBRICKS_PROTO_PCIE_D2H_MSG_READ:
            /* Read */
            read = &msg->read;

            rid = read->req_id;
            addr = read->offset;
            len = read->len;
            DPRINTF(SimBricksPci, "simbricks-pci: received DMA read id %u "
                    "addr %x size %x\n", rid, addr, len);

            panic_if(!validDma(addr, len, adapter.outPayload()) ||
                     !validDma(pciToDma(addr), len, adapter.outPayload()),
                     "Invalid SimBricks DMA range/length");
            dc = new DMACompl(this, rid, len, DMACompl::READ, name());
            dmaPort.dmaAction(MemCmd::ReadReq, pciToDma(addr), len, dc, dc->buf, 0,
                    semanticFlags(read->pad, false, rid, addr, len));
            break;

        case SIMBRICKS_PROTO_PCIE_D2H_MSG_WRITE:
            /* Write */
            write = &msg->write;

            rid = write->req_id;
            addr = write->offset;
            len = write->len;
            DPRINTF(SimBricksPci, "simbricks-pci: received DMA write id %u "
                    "addr %x size %x\n", rid, addr, len);

            panic_if(!validDma(addr, len, adapter.inPayload()) ||
                     !validDma(pciToDma(addr), len, adapter.inPayload()),
                     "Invalid SimBricks DMA range/length");
            dc = new DMACompl(this, rid, len, DMACompl::WRITE, name());
            memcpy(dc->buf, (void *) write->data, len);
            dmaPort.dmaAction(MemCmd::WriteReq, pciToDma(addr), len, dc, dc->buf, 0,
                     semanticFlags(write->pad, true, rid, addr, len));
            break;

        case SIMBRICKS_PROTO_PCIE_D2H_MSG_INTERRUPT:
            /* Interrupt */
            intr = &msg->interrupt;
            if (intr->inttype == SIMBRICKS_PROTO_PCIE_INT_MSI) {
                panic_if(intr->vector >= 32, "Invalid MSI vector");
                msi_signal(intr->vector);
            } else if (intr->inttype == SIMBRICKS_PROTO_PCIE_INT_MSIX) {
                msix_signal(intr->vector);
            } else if (intr->inttype == SIMBRICKS_PROTO_PCIE_INT_LEGACY_HI) {
                intrPost();
            } else if (intr->inttype == SIMBRICKS_PROTO_PCIE_INT_LEGACY_LO) {
                intrClear();
            } else {
                panic("unsupported inttype=0x%x", intr->inttype);
            }
            break;

        case SIMBRICKS_PROTO_PCIE_D2H_MSG_READCOMP:
            /* Receive read complete message */
            rc = &msg->readcomp;

            rid = rc->req_id;
            DPRINTF(SimBricksPci, "simbricks-pci: received read completion "
                    "id %lu\n", rid);

            panic_if(!pending.count(rid), "Unknown SimBricks completion ID");
            pc = pending.at(rid);
            panic_if(pc->pkt->isRead() != (ty == SIMBRICKS_PROTO_PCIE_D2H_MSG_READCOMP),
                     "Wrong SimBricks completion type");
            pending.erase(rid);
            panic_if(pc->pkt->getSize() > adapter.inPayload(), "Short read completion");
            pc->pkt->setData((const uint8_t *) rc->data);
            pc->setDone();
            break;

        case SIMBRICKS_PROTO_PCIE_D2H_MSG_WRITECOMP:
            /* Receive write complete message */
            wc = &msg->writecomp;

            rid = wc->req_id;
            DPRINTF(SimBricksPci, "simbricks-pci: received write completion "
                    "id %lu\n", rid);

            panic_if(!pending.count(rid), "Unknown SimBricks completion ID");
            pc = pending.at(rid);
            panic_if(pc->pkt->isRead() != (ty == SIMBRICKS_PROTO_PCIE_D2H_MSG_READCOMP),
                     "Wrong SimBricks completion type");
            pending.erase(rid);
            pc->setDone();
            break;

        default:
            panic("Simbricks::Pci::pollQueues: unsupported type=%x", ty);
    }

    adapter.inDone(msg);
}

void
Device::msi_signal(uint16_t vec)
{
    DMACompl *dc;
    panic_if(!MSICAP_BASE || vec >= (1U << ((msicap.mc >> 4) & 7)),
             "Invalid MSI vector");

    DPRINTF(SimBricksPci, "simbricks-pci: received MSI intr vec %u\n", vec);

    if ((msicap.mc & 0x1) != 0 &&
            ((msicap.mmask & (1 << vec)) == 0))
    {
        DPRINTF(SimBricksPci, "simbricks-pci: MSI addr=%x val=%x mask=%x\n",
                msicap.ma, msicap.md, msicap.mmask);
        dc = new DMACompl(this, 0, 4, DMACompl::MSI, name());
        uint16_t data = htole(uint16_t((msicap.md & ~((1U << ((msicap.mc >> 4) & 7)) - 1)) | vec));
        memcpy(dc->buf, &data, 2);
        memset(dc->buf + 2, 0, 2);

        dmaWrite(pciToDma(msicap.ma | ((uint64_t) msicap.mua << 32)),
                4, dc, dc->buf, 0);
    } else {
        DPRINTF(SimBricksPci, "simbricks-pci: MSI masked\n");
    }
}

void
Device::replayPendingMsix()
{
    if ((msixcap.mxc & 0xc000) != 0x8000)
        return;
    for (size_t vec = 0; vec < msix_table.size(); ++vec) {
        if (!(msix_table[vec].fields.vec_ctrl & 1) &&
            (msix_pba[vec / MSIXVECS_PER_PBA].bits &
             (uint64_t(1) << (vec % MSIXVECS_PER_PBA))))
            msix_signal(vec);
    }
}

void
Device::msix_signal(uint16_t vec)
{
    DMACompl *dc;
    panic_if(vec >= msix_table.size(), "Invalid MSI-X vector");
    MSIXTable &te = msix_table[vec];
    MSIXPbaEntry &pe = msix_pba[vec / MSIXVECS_PER_PBA];

    DPRINTF(SimBricksPci, "simbricks-pci: received MSI-X intr vec %u\n", vec);

    // Disabled signals are discarded; pending bits from before disable survive.
    if (!(msixcap.mxc & 0x8000)) return;
    if ((msixcap.mxc & 0x4000) || (te.fields.vec_ctrl & 1)) {
        pe.bits |= uint64_t(1) << (vec % MSIXVECS_PER_PBA);
        return;
    }

    pe.bits &= ~(uint64_t(1) << (vec % MSIXVECS_PER_PBA));
    dc = new DMACompl(this, 0, 4, DMACompl::MSI, name());
    memcpy(dc->buf, &te.fields.msg_data, 4);

    uint64_t addr = te.fields.addr_hi;
    addr = (addr << 32) | te.fields.addr_lo;
    dmaWrite(pciToDma(addr), 4, dc, dc->buf, 0);
}

bool
Device::readMsix(PciPioCompl &comp, Addr addr, int bar)
{
    if (!MSIXCAP_BASE)
        return false;

    if (bar == MSIX_TABLE_BAR && addr >= MSIX_TABLE_OFFSET &&
            addr < MSIX_TABLE_END)
    {
        uint32_t off = addr - MSIX_TABLE_OFFSET;
        uint16_t idx = off / 16;
        uint8_t col = off % 16;
        MSIXTable &entry = msix_table[idx];

        panic_if(!within(col, comp.pkt->getSize(), sizeof(entry)), "Invalid MSI-X access");

        comp.pkt->setData((const uint8_t *) entry.data + col);
        comp.setDone();
        return true;
    }

    if (bar == MSIX_PBA_BAR && addr >= MSIX_PBA_OFFSET &&
            addr < MSIX_PBA_END)
    {
        uint32_t off = addr - MSIX_PBA_OFFSET;
        uint16_t idx = off / (MSIXVECS_PER_PBA / 8);
        uint16_t col = off % (MSIXVECS_PER_PBA / 8);
        const MSIXPbaEntry &entry = msix_pba[idx];

        panic_if(!within(col, comp.pkt->getSize(), sizeof(entry)), "Invalid MSI-X access");

        comp.pkt->setData(((const uint8_t *) &entry) + col);
        comp.setDone();
        return true;
    }

    return false;
}

bool
Device::writeMsix(PciPioCompl &comp, Addr addr, int bar)
{
    if (!MSIXCAP_BASE)
        return false;

    if (bar == MSIX_TABLE_BAR && addr >= MSIX_TABLE_OFFSET &&
            addr < MSIX_TABLE_END)
    {
        uint32_t off = addr - MSIX_TABLE_OFFSET;
        uint16_t idx = off / 16;
        uint8_t col = off % 16;
        MSIXTable &entry = msix_table[idx];

        panic_if(!within(col, comp.pkt->getSize(), sizeof(entry)), "Invalid MSI-X access");

        bool masked_before = entry.fields.vec_ctrl & 1;
        for (size_t i = 0; i < comp.pkt->getSize(); ++i)
            maskedByte(((uint8_t *)entry.data)[col + i],
                       comp.pkt->getConstPtr<uint8_t>()[i],
                       col + i < 12 ? ((col + i == 0) ? 0xfc : 0xff) :
                       (col + i == 12 ? 1 : 0));
        if (masked_before && !(entry.fields.vec_ctrl & 1) &&
            (msixcap.mxc & 0xc000) == 0x8000 &&
            (msix_pba[idx / MSIXVECS_PER_PBA].bits &
             (uint64_t(1) << (idx % MSIXVECS_PER_PBA))))
            msix_signal(idx);
        comp.setDone();
        return true;
    }

    if (bar == MSIX_PBA_BAR && addr >= MSIX_PBA_OFFSET &&
            addr < MSIX_PBA_END)
    {
        uint32_t off = addr - MSIX_PBA_OFFSET;
        uint16_t idx = off / (MSIXVECS_PER_PBA / 8);
        uint16_t col = off % (MSIXVECS_PER_PBA / 8);
        MSIXPbaEntry &entry = msix_pba[idx];

        panic_if(!within(col, comp.pkt->getSize(), sizeof(entry)), "Invalid MSI-X access");

        // Pending bits are read-only to software.
        comp.setDone();
        return true;
    }

    return false;
}

void
Device::serialize(CheckpointOut &cp) const
{
    fatal("SimBricks PCI checkpointing is unsupported");
}

void
Device::unserialize(CheckpointIn &cp)
{
    fatal("SimBricks PCI checkpoint restore is unsupported");
}

void
Device::startup()
{
    adapter.startup();
}

Bar *
Device::getSimBricksBar(size_t i) const
{
    return dynamic_cast<Bar *>(BARs[i]);
}

/*****************************************************************************/

TimingPioPort::TimingPioPort(const std::string &_name,
              Device &_dev,
              PortID _id)
    : QueuedResponsePort(_name, respQueue, _id), dev(_dev),
    respQueue(_dev, *this)
{
}

AddrRangeList TimingPioPort::getAddrRanges() const
{
    return dev.getAddrRanges();
}


void
TimingPioPort::recvFunctional(PacketPtr pkt)
{
    if (pkt->cacheResponding())
        panic("TimingPioPort: should not see cache responding");


    if (respQueue.trySatisfyFunctional(pkt))
        return;

    if (pkt->isRead())
        dev.read(pkt);
    else
        dev.write(pkt);

    assert(pkt->isResponse() || pkt->isError());
}

Tick
TimingPioPort::recvAtomic(PacketPtr pkt)
{
    if (pkt->cacheResponding())
        panic("TimingPioPort: should not see cache responding");

    // Technically the packet only reaches us after the header delay,
    // and typically we also need to deserialise any payload.
    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;

    const Tick delay =
        pkt->isRead() ? dev.read(pkt) : dev.write(pkt);
    assert(pkt->isResponse() || pkt->isError());
    return delay + receive_delay;
}

bool
TimingPioPort::recvTimingReq(PacketPtr pkt)
{
    TimingPioCompl *tpc;
    bool needResp;

    if (pkt->cacheResponding())
        panic("TimingPioPort: should not see cache responding");

    needResp = pkt->needsResponse();

    if (pkt->isWrite() && dev.writesPosted)
        needResp = false;

    tpc = new TimingPioCompl(*this, pkt, needResp);
    if (pkt->isRead()) {
        dev.readAsync(*tpc);
    } else if (pkt->isWrite()) {
        tpc->keep = true;
        dev.writeAsync(*tpc);

        if (pkt->isWrite() && dev.writesPosted && pkt->needsResponse()) {
            DPRINTF(SimBricksPci, "simbricks-pci: sending immediate response "
                    "for posted write\n");
            pkt->makeTimingResponse();
            schedTimingResp(pkt, curTick() + 1);
            tpc->pkt = 0;
        }

        if (tpc->done)
            delete tpc;
        else
            tpc->keep = false;
    } else {
        panic("TimingPioPort: unknown packet type");
    }

    return true;
}

void
TimingPioPort::timingPioCompl(TimingPioCompl &comp)
{
    if (!comp.needResp) {
        if (comp.pkt && !comp.keep) {
            delete comp.pkt;
            comp.pkt = nullptr;
        }
        return;
    }

    comp.pkt->makeTimingResponse();
    schedTimingResp(comp.pkt, curTick());
    comp.pkt = nullptr;
}

TimingPioCompl::TimingPioCompl(TimingPioPort &_port, PacketPtr _pkt,
        bool needResp_)
    : PciPioCompl(_pkt), port(_port), needResp(needResp_), keep(false)
{
}

void
TimingPioCompl::setDone()
{
    done = true;
    port.timingPioCompl(*this);
    if (!keep)
        delete this;
}

} // namespace Pci
} // namespace Simbricks
} // namespace gem5
