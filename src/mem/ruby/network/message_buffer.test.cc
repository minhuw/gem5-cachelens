/*
 * Copyright (c) 2026 minhuw
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gtest/gtest.h>

#include "base/gtest/logging.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "sim/cur_tick.hh"

namespace gem5::ruby
{

// Only the wakeup sink is stubbed: the real MessageBuffer implementation,
// heap, message timestamps/counters, statistics and FIFO guard are linked.
// This test does not certify Consumer clock rounding or event dispatch.
Consumer::Consumer(ClockedObject *obj, Event::Priority priority)
    : m_wakeup_event([] {}, "test wakeup", false, priority), em(obj)
{}
void Consumer::scheduleEventAbsolute(Tick time) { m_wakeup_ticks.insert(time); }
void Consumer::scheduleEvent(Cycles) { FAIL() << "unexpected dequeue throttle"; }

class TestConsumer : public Consumer
{
  public:
    TestConsumer() : Consumer(nullptr) {}
    void wakeup() override {}
    void print(std::ostream &out) const override { out << "test consumer"; }
};

class TestMessage : public Message
{
  public:
    explicit TestMessage(Tick time) : Message(time, 64, nullptr) {}
    MsgPtr clone() const override { return std::make_shared<TestMessage>(*this); }
    void print(std::ostream &out) const override { out << "test message"; }
};

class MessageBufferTest : public ::testing::Test
{
  protected:
    static constexpr Tick T = 1349865259000;
    Tick tick = T - 1000;
    TestConsumer consumer;
    std::unique_ptr<MessageBuffer> buffer;
    MsgPtr older, younger;

    void SetUp() override { Gem5Internal::_curTickPtr = &tick; }
    void TearDown() override
    {
        buffer.reset();
        Gem5Internal::_curTickPtr = nullptr;
    }
    void makeBuffer(bool ordered = true,
                    MessageRandomization random = MessageRandomization::disabled,
                    bool warmup = false, bool zero = false)
    {
        MessageBufferParams p;
        p.name = "bufferFromL1";
        p.eventq_index = 0;
        p.ordered = ordered;
        p.buffer_size = 4;
        p.max_dequeue_rate = 0;
        p.randomization = random;
        p.allow_zero_latency = zero;
        p.routing_priority = 0;
        buffer = std::make_unique<MessageBuffer>(p);
        buffer->setConsumer(&consumer);
        older = std::make_shared<TestMessage>(tick);
        younger = std::make_shared<TestMessage>(T);
        buffer->enqueue(older, tick, 1000, false, warmup);
        tick = T;
        buffer->enqueue(younger, tick, 1000, false, warmup);
    }
    double stat(const char *name)
    {
        auto *info = dynamic_cast<const statistics::ScalarInfo *>(
            buffer->resolveStat(name));
        EXPECT_NE(info, nullptr);
        return info ? info->value() : -1;
    }
    void expectDrain(Tick arrival)
    {
        EXPECT_EQ(buffer->getSize(T + 1), 2);
        EXPECT_EQ(stat("m_buf_msgs"), 2);
        EXPECT_EQ(stat("m_msg_count"), 0);
        EXPECT_FALSE(buffer->isReady(T + 499));
        EXPECT_EQ(buffer->peekMsgPtr(), younger);
        tick = T + 1000;
        buffer->dequeue(tick);
        EXPECT_EQ(buffer->peekMsgPtr(), older);
        EXPECT_EQ(buffer->isReady(arrival - 1), false);
        tick = arrival;
        EXPECT_TRUE(buffer->isReady(tick));
        buffer->dequeue(tick);
        EXPECT_TRUE(buffer->isEmpty());
        EXPECT_EQ(stat("m_buf_msgs"), 0);
        EXPECT_EQ(stat("m_msg_count"), 2);
    }
};

// Run only against the original header. GTest turns panic into an exception;
// verify the exact runtime diagnostic, not merely any failure.
TEST_F(MessageBufferTest, DISABLED_UnpatchedExactPanic)
{
    makeBuffer();
    ASSERT_TRUE(buffer->isReady(T));
    ASSERT_EQ(buffer->peekMsgPtr(), older);
    EXPECT_THROW(buffer->delayHead(T, 500, false, false), GTestException);
    const auto log = gtestLogOutput.str();
    EXPECT_NE(log.find("FIFO ordering violated"), std::string::npos);
    EXPECT_NE(log.find("current time: 1349865259000 delta: 500 "
                       "arrival_time: 1349865259500 "
                       "last arrival_time: 1349865260000"), std::string::npos);
    std::cout << log;
}

TEST_F(MessageBufferTest, OrderedRetryClampsToTailAndRotatesTie)
{
    makeBuffer();
    ASSERT_TRUE(buffer->isReady(T));
    buffer->delayHead(T, 500, false, false);
    EXPECT_EQ(older->getLastEnqueueTime(), T + 1000);
    EXPECT_LT(younger->getMsgCounter(), older->getMsgCounter());
    EXPECT_TRUE(consumer.alreadyScheduled(T + 1000));
    EXPECT_FALSE(consumer.alreadyScheduled(T + 500));
    EXPECT_FALSE(buffer->isReady(T + 999));
    EXPECT_EQ(buffer->getSize(T + 1), 2);
    EXPECT_EQ(stat("m_buf_msgs"), 2);
    EXPECT_EQ(stat("m_msg_count"), 0);
    // Both are ready at the tie; the younger message has the smaller counter.
    EXPECT_EQ(buffer->peekMsgPtr(), younger);
    tick = T + 1000;
    buffer->dequeue(tick);
    EXPECT_EQ(buffer->peekMsgPtr(), older);
    EXPECT_TRUE(buffer->isReady(tick));
    buffer->dequeue(tick);
    EXPECT_TRUE(buffer->isEmpty());
    EXPECT_EQ(stat("m_buf_msgs"), 0);
    EXPECT_EQ(stat("m_msg_count"), 2);
}

TEST_F(MessageBufferTest, LongerRequestedDelayIsPreserved)
{
    makeBuffer();
    buffer->delayHead(T, 1500, false, false);
    EXPECT_EQ(older->getLastEnqueueTime(), T + 1500);
    expectDrain(T + 1500);
}

TEST_F(MessageBufferTest, UnorderedRetryAndGauge)
{
    makeBuffer(false);
    buffer->delayHead(T, 500, false, false);
    EXPECT_EQ(older->getLastEnqueueTime(), T + 500);
    EXPECT_EQ(buffer->peekMsgPtr(), older);
    EXPECT_EQ(buffer->getSize(T + 1), 2);
    EXPECT_EQ(stat("m_buf_msgs"), 2);
    EXPECT_FALSE(buffer->isReady(T + 499));
    tick = T + 500;
    buffer->dequeue(tick);
    EXPECT_EQ(buffer->peekMsgPtr(), younger);
    tick = T + 1000;
    buffer->dequeue(tick);
    EXPECT_TRUE(buffer->isEmpty());
    EXPECT_EQ(stat("m_buf_msgs"), 0);
}

TEST_F(MessageBufferTest, ProducerFifoGuardIsUnchanged)
{
    makeBuffer();
    auto third = std::make_shared<TestMessage>(T);
    EXPECT_THROW(buffer->enqueue(third, T, 500, false, false), GTestException);
    EXPECT_NE(gtestLogOutput.str().find("FIFO ordering violated"),
              std::string::npos);
}

TEST_F(MessageBufferTest, ZeroRetryStillRejectedWithPendingTail)
{
    makeBuffer();
    EXPECT_THROW(buffer->delayHead(T, 0, false, false), GTestException);
    EXPECT_NE(gtestLogOutput.str().find("Delta equals zero"), std::string::npos);
}

TEST_F(MessageBufferTest, PastTailDoesNotUnderflow)
{
    makeBuffer();
    tick = T + 2000;
    buffer->delayHead(tick, 500, false, false);
    EXPECT_EQ(older->getLastEnqueueTime(), T + 2500);
}

TEST_F(MessageBufferTest, RandomizationKeepsExistingIgnoreDeltaSemantics)
{
    makeBuffer(true, MessageRandomization::ruby_system);
    // Seed deterministic arrivals with ruby_is_random=false, then enable it.
    buffer->delayHead(T, 10000, true, false);
    EXPECT_GT(older->getLastEnqueueTime(), T + 1000);
    EXPECT_LT(older->getLastEnqueueTime(), T + 10000);
    EXPECT_EQ(stat("m_buf_msgs"), 2);
}

TEST_F(MessageBufferTest, WarmupDoesNotTrackTail)
{
    makeBuffer(true, MessageRandomization::disabled, true);
    buffer->delayHead(T, 500, false, true);
    EXPECT_EQ(older->getLastEnqueueTime(), T + 500);
    EXPECT_EQ(buffer->peekMsgPtr(), older);
    EXPECT_EQ(stat("m_buf_msgs"), 2);
}

TEST_F(MessageBufferTest, RepeatedRetriesConserveContentsAndCounts)
{
    makeBuffer();
    buffer->delayHead(T, 500, false, false);
    tick = T + 1000;
    buffer->delayHead(tick, 500, false, false); // younger moves past older
    EXPECT_EQ(buffer->peekMsgPtr(), older);
    buffer->delayHead(tick, 500, false, false); // tie rotates again
    EXPECT_EQ(buffer->peekMsgPtr(), younger);
    EXPECT_EQ(buffer->getSize(tick + 1), 2);
    EXPECT_EQ(stat("m_buf_msgs"), 2);
    EXPECT_EQ(stat("m_msg_count"), 0);
    EXPECT_FALSE(buffer->areNSlotsAvailable(3, tick));
    EXPECT_TRUE(buffer->areNSlotsAvailable(2, tick));
    tick += 500;
    buffer->dequeue(tick);
    EXPECT_EQ(buffer->peekMsgPtr(), older);
    buffer->dequeue(tick);
    EXPECT_TRUE(buffer->isEmpty());
    EXPECT_EQ(stat("m_buf_msgs"), 0);
    EXPECT_EQ(stat("m_msg_count"), 2);
}

TEST_F(MessageBufferTest, AllowedZeroRetryStillClampsToTail)
{
    makeBuffer(true, MessageRandomization::disabled, false, true);
    buffer->delayHead(T, 0, false, false);
    EXPECT_EQ(older->getLastEnqueueTime(), T + 1000);
}

TEST_F(MessageBufferTest, RejectsNonreadyHead)
{
    makeBuffer();
    buffer->delayHead(T, 500, false, false);
    EXPECT_DEATH(buffer->delayHead(T, 500, false, false), "isReady");
}

TEST_F(MessageBufferTest, LongRetryCanStillRejectLaterProducer)
{
    makeBuffer();
    buffer->delayHead(T, 10000, false, false);
    tick = T + 500;
    auto third = std::make_shared<TestMessage>(tick);
    // Intentional candidate limitation: retry advances the recorded tail.
    // A subsequent shorter producer arrival remains a strict FIFO violation.
    EXPECT_THROW(buffer->enqueue(third, tick, 1000, false, false),
                 GTestException);
    EXPECT_NE(gtestLogOutput.str().find("FIFO ordering violated"),
              std::string::npos);
}

} // namespace gem5::ruby
