/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include "dev/pci/config_write.hh"

namespace gem5
{
namespace pci
{
namespace
{

TEST(PciCommandStatusWrite, CommandBytesPreserveOtherLanes)
{
    auto result = applyCommandStatusWrite(0xa504, 0xffff, PCI_COMMAND,
                                         1, 0x03);
    EXPECT_EQ(result.command, 0xa503);
    EXPECT_EQ(result.status, 0xffff);
    EXPECT_TRUE(result.rangeChanged);

    // IGbE observes interrupt-disable transitions in the command high byte.
    result = applyCommandStatusWrite(0x0003, 0xffff, PCI_COMMAND + 1,
                                     1, 0x04);
    EXPECT_EQ(result.command, 0x0403);
    EXPECT_EQ(result.status, 0xffff);
    EXPECT_FALSE(result.rangeChanged);
    result = applyCommandStatusWrite(result.command, result.status,
                                     PCI_COMMAND + 1, 1, 0);
    EXPECT_EQ(result.command, 0x0003);
    EXPECT_FALSE(result.rangeChanged);
}

TEST(PciCommandStatusWrite, StatusBytesPreserveCommandAndReadonlyBits)
{
    auto result = applyCommandStatusWrite(0xa503, 0xffff, PCI_STATUS,
                                         1, 0xff);
    EXPECT_EQ(result.command, 0xa503);
    EXPECT_EQ(result.status, 0xffff); // Low status byte is read-only.
    EXPECT_FALSE(result.rangeChanged);

    result = applyCommandStatusWrite(0xa503, 0xffff, PCI_STATUS + 1,
                                     1, 0xff);
    EXPECT_EQ(result.command, 0xa503);
    EXPECT_EQ(result.status, 0x06ff);
    EXPECT_FALSE(result.rangeChanged);
}

TEST(PciCommandStatusWrite, StatusWordClearsOnlyWrittenErrorBits)
{
    for (unsigned bit = 0; bit < 16; ++bit) {
        const uint16_t value = 1U << bit;
        const uint16_t expected = (bit == 8 || bit >= 11) ?
            uint16_t(0xffff ^ value) : 0xffff;
        const auto result = applyCommandStatusWrite(
            0xa503, 0xffff, PCI_STATUS, 2, value);
        EXPECT_EQ(result.command, 0xa503);
        EXPECT_EQ(result.status, expected) << "bit=" << bit;
        EXPECT_FALSE(result.rangeChanged);
    }
    EXPECT_EQ(applyCommandStatusWrite(3, 0xffff, PCI_STATUS, 2, 0).status,
              0xffff);
    EXPECT_EQ(applyCommandStatusWrite(3, 0, PCI_STATUS, 2, 0xffff).status,
              0); // Writes cannot set status bits.
}

TEST(PciCommandStatusWrite, CommandWordPreservesStatus)
{
    const auto result = applyCommandStatusWrite(0, 0xffff, PCI_COMMAND,
                                               2, 0xa503);
    EXPECT_EQ(result.command, 0xa503);
    EXPECT_EQ(result.status, 0xffff);
    EXPECT_TRUE(result.rangeChanged);
}

TEST(PciCommandStatusWrite, DwordUpdatesCommandAndClearsStatus)
{
    const auto result = applyCommandStatusWrite(0, 0xffff, PCI_COMMAND,
                                               4, 0x8100a503);
    EXPECT_EQ(result.command, 0xa503);
    EXPECT_EQ(result.status, 0x7eff);
    EXPECT_TRUE(result.rangeChanged);

    const auto all = applyCommandStatusWrite(3, 0xffff, PCI_COMMAND,
                                            4, 0xffffffff);
    EXPECT_EQ(all.command, 0xffff);
    EXPECT_EQ(all.status, 0x06ff);
    EXPECT_FALSE(all.rangeChanged);
}

TEST(PciCommandStatusWrite, OnlyIoAndMemoryDecodeChangesNotify)
{
    for (unsigned size : {1U, 2U, 4U}) {
        for (uint16_t oldCommand = 0; oldCommand < 8; ++oldCommand) {
            for (uint16_t command = 0; command < 8; ++command) {
                const auto result = applyCommandStatusWrite(
                    oldCommand, 0xffff, PCI_COMMAND, size, command);
                EXPECT_EQ(result.command, command);
                EXPECT_EQ(result.status, 0xffff);
                EXPECT_EQ(result.rangeChanged,
                          bool((oldCommand ^ command) & 3));
            }
        }
    }
}

} // anonymous namespace
} // namespace pci
} // namespace gem5
