#include "simbricks/dma_checker.hh"
#include "mem/packet.hh"
#include "sim/sim_exit.hh"
#include "sim/core.hh"
#include <cstring>
namespace gem5::simbricks::pci {
gem5::Port &DmaChecker::getPort(const std::string &n, PortID i) {
    return n == "port" ? port : ClockedObject::getPort(n, i);
}
void DmaChecker::init() { port.sendRangeChange(); }
AddrRangeList DmaChecker::Port::getAddrRanges() const {
    return {AddrRange(0, 0xffff)};
}
void DmaChecker::check(PacketPtr p) {
    const Addr a = p->getAddr();
    uint64_t expected;
    if (a >= 0x1038 && a + p->getSize() <= 0x10d8) {
        expected = Request::NIC_RX_DESC_READ;
        panic_if(!p->isRead(), "descriptor direction"); ++descChunks;
    } else if ((a >= 0x2000 && a + p->getSize() <= 0x25c0) ||
               (a >= 0x3038 && a + p->getSize() <= 0x35f8)) {
        expected = Request::NIC_RX_PAYLOAD_WRITE;
        panic_if(!p->isWrite(), "payload direction"); ++payloadChunks;
    } else {
        panic_if(a != 0x4000 && a != 0x4040, "unexpected DMA address");
        expected = 0; ++controlChunks;
    }
    panic_if((p->req->getFlags() & Request::NIC_DMA_CATEGORY_MASK) != expected,
             "DMA flag lost/wrong at memory: address %#x flags %#x expected %#x",
             a, p->req->getFlags(), expected);
    panic_if(p->getSize() > 64 || (a % 64) + p->getSize() > 64, "unsplit DMA");
    bytes += p->getSize();
    ++chunks;
    if (p->isRead()) memset(p->getPtr<uint8_t>(), 0, p->getSize());
    else for (unsigned i = 0; i < p->getSize(); ++i)
        panic_if(p->getConstPtr<uint8_t>()[i] != 0xa5, "DMA payload corrupted");
    p->makeResponse();
    if (chunks == 53) {
        panic_if(descChunks != 4 || payloadChunks != 47 || controlChunks != 2 || bytes != 3136,
                 "split DMA counts wrong");
        schedule(new EventFunctionWrapper([this]() {
            panic_if(chunks != 53, "extra DMA requests");
            inform("DMA_MEMORY_FLAGS_OK chunks=%u desc=%u payload=%u control=%u bytes=%lu",
                   chunks, descChunks, payloadChunks, controlChunks, bytes);
            exitSimLoop("DMA_MEMORY_FLAGS_OK");
        }, name() + ".finish", true), curTick() + 1000 * sim_clock::as_int::ns);
    }
}
Tick DmaChecker::Port::recvAtomic(PacketPtr p) {
    owner.check(p); return 10 * sim_clock::as_int::ns;
}
void DmaChecker::Port::recvFunctional(PacketPtr p) { panic("unexpected functional DMA"); }
bool DmaChecker::Port::recvTimingReq(PacketPtr p) {
    owner.check(p);
    schedTimingResp(p, curTick() + 10 * sim_clock::as_int::ns);
    return true;
}
}
