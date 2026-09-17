// SPDX-License-Identifier: MIT
#ifndef __SIMBRICKS_PCI_PORT_TESTER_HH__
#define __SIMBRICKS_PCI_PORT_TESTER_HH__
#include "simbricks/pci.hh"
#include "dev/platform.hh"
#include "params/SimBricksTestPlatform.hh"
#include "params/SimBricksPciPortTester.hh"
namespace gem5::simbricks::pci {
class TestPlatform : public Platform
{
  public:
    TestPlatform(const SimBricksTestPlatformParams &p) : Platform(p) {}
    void postConsoleInt() override {}
    void clearConsoleInt() override {}
};
class PortTester : public Device
{
  public:
    PARAMS(SimBricksPciPortTester);
    PortTester(const Params &p) : Device(p) {}
    void init() override;
    void startup() override;

  private:
    void testMsix();
    unsigned msixStep = 0;
};
}
#endif
