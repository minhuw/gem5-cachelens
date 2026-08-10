/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <array>

#include "dev/net/load_generator_mac.hh"

namespace gem5
{
namespace
{

TEST(LoadGeneratorMac, UsesLocallyAdministeredNonDefaultNicAddresses)
{
    const LoadGeneratorMac first = {0x02, 0x90, 0x00, 0x00, 0x00, 0x01};
    const LoadGeneratorMac sixth = {0x02, 0x90, 0x00, 0x00, 0x00, 0x06};

    EXPECT_EQ(first, loadGeneratorNicMac(0));
    EXPECT_EQ(sixth, loadGeneratorNicMac(5));
    EXPECT_EQ(0, first[0] & 0x01);
    EXPECT_NE(0, first[0] & 0x02);
    EXPECT_NE(0x00, first[0]);
}

} // anonymous namespace
} // namespace gem5
