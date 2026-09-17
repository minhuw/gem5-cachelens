// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
extern "C" {
#include <simbricks/pcie/proto.h>
}
#include "simbricks/pci_safety.hh"
#include "dev/pci/pcireg.h"

namespace gem5::simbricks::pci {
TEST(SimBricksPci, CapabilityMasksAndBounds)
{
    MSIXCAP cap = {};
    cap.mxid = 0x5011; cap.mxc = 0x003f;
    cap.mtab = 0x1004; cap.mpba = 0x2005;
    for (size_t i = 0; i < sizeof(cap); ++i) maskedByte(cap.data[i], 0xff, msixMask(i));
    EXPECT_EQ(cap.mxid, 0x5011); EXPECT_EQ(cap.mxc, 0xc03f);
    EXPECT_EQ(cap.mtab & 7, 4); EXPECT_EQ(cap.mtab & ~7U, 0x1000);
    EXPECT_EQ(cap.mpba & 7, 5); EXPECT_EQ(cap.mpba & ~7U, 0x2000);
    for (size_t i = 0; i < sizeof(cap); ++i) maskedByte(cap.data[i], 0, msixMask(i));
    EXPECT_EQ(cap.mxc, 0x003f);
    EXPECT_FALSE(within(11, 2, sizeof(cap)));
    EXPECT_FALSE(within(UINT64_MAX, 4, sizeof(cap)));
    EXPECT_TRUE(within(1, 4, sizeof(cap))); // unaligned byte path is safe
    EXPECT_EQ(msiMask(0), 0); EXPECT_EQ(msiMask(1), 0);
    EXPECT_EQ(msiMask(2), 0x71); EXPECT_EQ(msiMask(4), 0xfc);
    EXPECT_EQ(msiMask(20), 0);
}
TEST(SimBricksPci, ExternalDmaAndIntroduction)
{
    EXPECT_FALSE(validDma(0, 0, 4096));
    EXPECT_FALSE(validDma(0, 4097, 4096));
    EXPECT_FALSE(validDma(UINT64_MAX, 2, 4096));
    EXPECT_TRUE(validDma(UINT64_MAX - 3, 4, 4096));
    SimbricksProtoPcieDevIntro di = {};
    di.bars[4].len = di.bars[5].len = 8192;
    di.pci_msix_nvecs = 64; di.pci_msix_table_bar = 4; di.pci_msix_pba_bar = 5;
    di.pci_msix_table_offset = 4096; di.pci_msix_pba_offset = 4096;
    EXPECT_TRUE(validIntro(di));
    di.pci_msix_table_bar = 6; EXPECT_FALSE(validIntro(di));
    di.pci_msix_table_bar = 4; di.pci_msix_table_offset = 4097;
    EXPECT_FALSE(validIntro(di));
    di.pci_msix_table_offset = 8192 - 8; EXPECT_FALSE(validIntro(di));
    di.pci_msix_table_offset = 4096; di.pci_msix_nvecs = 2049;
    EXPECT_FALSE(validIntro(di));
    di.pci_msix_nvecs = 64; di.pci_msix_pba_bar = 4;
    EXPECT_FALSE(validIntro(di)); // overlapping table/PBA
    di.pci_msix_pba_bar = 5; di.bars[5].flags = 2;
    EXPECT_FALSE(validIntro(di)); // 64-bit BAR without upper slot
}
}
