// SPDX-License-Identifier: BSD-3-Clause
#include "test_objects/dma_observe_tester.hh"
#include "base/logging.hh"
#include "sim/core.hh"
namespace gem5
{
DmaObserveTester::DmaObserveTester(const DmaObserveTesterParams &p)
    : ClockedObject(p), dma(this, p.system),
      begin([this] {
          data.fill(0xa7);
          dma.dmaAction(MemCmd::WriteReq, 0x4000, data.size(),
                        &written, data.data(), 0);
      }, name() + ".begin"),
      written([this] {
          data.fill(0);
          dma.dmaAction(MemCmd::ReadReq, 0x4000, data.size(),
                        &read, data.data(), 0);
      }, name() + ".written"),
      read([this] {
          for (auto byte : data) panic_if(byte != 0xa7, "Untagged DMA mismatch");
          inform("DMA_OBSERVE_UNTAGGED_OK write=64 read=64");
      }, name() + ".read")
{}
void DmaObserveTester::startup()
{
    schedule(begin, curTick() + 5 * sim_clock::as_int::us);
}
Port &DmaObserveTester::getPort(const std::string &name, PortID idx)
{
    return name == "dma" ? dma : ClockedObject::getPort(name, idx);
}
}
