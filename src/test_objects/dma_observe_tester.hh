// SPDX-License-Identifier: BSD-3-Clause
#ifndef __TEST_OBJECTS_DMA_OBSERVE_TESTER_HH__
#define __TEST_OBJECTS_DMA_OBSERVE_TESTER_HH__
#include <array>
#include "dev/dma_device.hh"
#include "params/DmaObserveTester.hh"
#include "sim/clocked_object.hh"
namespace gem5
{
// Test only: exercise untagged eligible I/O in the existing counter population.
class DmaObserveTester : public ClockedObject
{
    DmaPort dma;
    std::array<uint8_t, 64> data;
    EventFunctionWrapper begin, written, read;
  public:
    DmaObserveTester(const DmaObserveTesterParams &p);
    void startup() override;
    Port &getPort(const std::string &name, PortID idx=InvalidPortID) override;
};
}
#endif
