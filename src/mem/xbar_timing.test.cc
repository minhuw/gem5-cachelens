/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <memory>

#include "mem/hmc_controller_params.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "mem/xbar_response.hh"
#include "mem/xbar_timing.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace
{

class XBarTimingTest : public testing::Test
{
  protected:
    Tick tick = 0;

    void SetUp() override { Gem5Internal::_curTickPtr = &tick; }
    void TearDown() override { Gem5Internal::_curTickPtr = nullptr; }

    RequestPtr
    makeRequest(Addr address=0x1000)
    {
        return std::make_shared<Request>(address, 64, 0, 0);
    }

    std::unique_ptr<Packet>
    makeWritePacket()
    {
        auto packet =
            std::make_unique<Packet>(makeRequest(), MemCmd::WriteReq);
        packet->allocate();
        return packet;
    }
};

TEST_F(XBarTimingTest, TransparentModePreservesPacketTiming)
{
    auto packet = makeWritePacket();
    packet->headerDelay = 17;
    packet->payloadDelay = 23;

    XBarTiming::annotatePacket(packet.get(), 997, 2000, 1000, 8, true);

    EXPECT_EQ(packet->headerDelay, 17);
    EXPECT_EQ(packet->payloadDelay, 23);
}

TEST_F(XBarTimingTest, NormalModeKeepsExistingSerializationBehavior)
{
    auto packet = makeWritePacket();
    packet->headerDelay = 17;
    packet->payloadDelay = 23;

    XBarTiming::annotatePacket(packet.get(), 997, 2000, 1000, 8, false);

    EXPECT_EQ(packet->headerDelay, 3014);
    EXPECT_EQ(packet->payloadDelay, 8000);
}

TEST_F(XBarTimingTest, TransparentBookkeepingIsClockIndependent)
{
    constexpr Tick now = 100;
    EXPECT_EQ(XBarTiming::layerReleaseTick(true, now, now + 250), now + 1);
    EXPECT_EQ(XBarTiming::layerReleaseTick(true, now, now + 4000), now + 1);
    EXPECT_EQ(XBarTiming::layerReleaseTick(false, now, now + 250), now + 250);
}

TEST_F(XBarTimingTest, SynchronousResponseCanReuseCompletedRoute)
{
    XBarResponseRoute::RouteMap routes;
    const RequestPtr request = makeRequest();
    routes.emplace(request, 1);

    const bool accepted = XBarResponseRoute::forward(
        routes, request, 1, [&routes, &request] {
            return routes.emplace(request, 2).second;
        });

    EXPECT_TRUE(accepted);
    ASSERT_EQ(routes.count(request), 1);
    EXPECT_EQ(routes.at(request), 2);
}

TEST_F(XBarTimingTest, RejectedResponseRestoresRouteForSynchronousRetry)
{
    XBarResponseRoute::RouteMap routes;
    const RequestPtr request = makeRequest();
    routes.emplace(request, 3);
    unsigned sends = 0;

    EXPECT_FALSE(XBarResponseRoute::forward(
        routes, request, 3, [&sends] { return ++sends == 2; }));
    ASSERT_EQ(routes.count(request), 1);
    EXPECT_EQ(routes.at(request), 3);

    EXPECT_TRUE(XBarResponseRoute::forward(
        routes, request, 3, [&sends] { return ++sends == 2; }));
    EXPECT_EQ(sends, 2);
    EXPECT_EQ(routes.count(request), 0);
}

TEST_F(XBarTimingTest, RejectedResponseRestoresRouteAfterRehash)
{
    XBarResponseRoute::RouteMap routes;
    const RequestPtr request = makeRequest();
    routes.emplace(request, 4);
    const size_t old_bucket_count = routes.bucket_count();

    EXPECT_FALSE(XBarResponseRoute::forward(
        routes, request, 4, [this, &routes, old_bucket_count] {
            for (Addr address = 0x2000;
                 routes.bucket_count() == old_bucket_count; address += 64) {
                routes.emplace(makeRequest(address), 5);
            }
            return false;
        }));

    EXPECT_GT(routes.bucket_count(), old_bucket_count);
    ASSERT_EQ(routes.count(request), 1);
    EXPECT_EQ(routes.at(request), 4);
}

TEST_F(XBarTimingTest, RejectedResponseDoesNotOverwriteReentrantRoute)
{
    XBarResponseRoute::RouteMap routes;
    const RequestPtr request = makeRequest();
    routes.emplace(request, 6);

    EXPECT_ANY_THROW(XBarResponseRoute::forward(
        routes, request, 6, [&routes, &request] {
            routes.emplace(request, 7);
            return false;
        }));

    ASSERT_EQ(routes.count(request), 1);
    EXPECT_EQ(routes.at(request), 7);
}

TEST(HMCControllerTimingTest, ConstructionRejectsTransparentMode)
{
    HMCControllerParams params;
    params.timing_transparent = false;
    EXPECT_EQ(&HMCControllerParamsValidator::validate(params), &params);

    params.timing_transparent = true;
    EXPECT_ANY_THROW(HMCControllerParamsValidator::validate(params));
}

} // anonymous namespace
} // namespace gem5
