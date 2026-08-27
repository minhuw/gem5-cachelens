/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include "mem/ruby/common/WriteMask.hh"

namespace gem5
{
namespace ruby
{
namespace
{

void
expectSetBytes(const WriteMask &mask, std::initializer_list<int> expected)
{
    std::vector<bool> wanted(mask.getBlockSize(), false);
    for (const int byte : expected)
        wanted.at(byte) = true;

    for (int byte = 0; byte < mask.getBlockSize(); ++byte)
        EXPECT_EQ(mask.test(byte), wanted.at(byte)) << "byte " << byte;
}

TEST(WriteMask, ExtractFirstChunkAlignsCrossSliceWrite)
{
    WriteMask pending(64);
    pending.setMask(24, 16);

    const WriteMask first = pending.extractFirstChunk(32);
    expectSetBytes(first, {24, 25, 26, 27, 28, 29, 30, 31});
    expectSetBytes(pending, {32, 33, 34, 35, 36, 37, 38, 39});

    const WriteMask second = pending.extractFirstChunk(32);
    expectSetBytes(second, {32, 33, 34, 35, 36, 37, 38, 39});
    EXPECT_TRUE(pending.isEmpty());
}

TEST(WriteMask, ExtractFirstChunkPreservesSparseBytesPerSlice)
{
    WriteMask pending(64);
    for (const int byte : {5, 19, 27, 34, 47, 58})
        pending.setMask(byte, 1);

    EXPECT_EQ(pending.countChunks(32), 2);

    const WriteMask first = pending.extractFirstChunk(32);
    expectSetBytes(first, {5, 19, 27});
    expectSetBytes(pending, {34, 47, 58});

    const WriteMask second = pending.extractFirstChunk(32);
    expectSetBytes(second, {34, 47, 58});
    EXPECT_TRUE(pending.isEmpty());
}

TEST(WriteMask, ExtractFirstChunkPreservesFullLineBeats)
{
    WriteMask pending(64);
    pending.fillMask();

    const WriteMask first = pending.extractFirstChunk(32);
    EXPECT_EQ(first.count(), 32);
    EXPECT_EQ(pending.count(), 32);
    for (int byte = 0; byte < 64; ++byte) {
        EXPECT_EQ(first.test(byte), byte < 32);
        EXPECT_EQ(pending.test(byte), byte >= 32);
    }
}

} // anonymous namespace
} // namespace ruby
} // namespace gem5
