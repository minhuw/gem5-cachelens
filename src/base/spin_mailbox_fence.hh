// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause
#ifndef __BASE_SPIN_MAILBOX_FENCE_HH__
#define __BASE_SPIN_MAILBOX_FENCE_HH__

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <stdexcept>

namespace gem5
{

/**
 * Host-only rendezvous with fixed membership and one fixed coordinator.
 * Exactly one stable owner calls wait for each role. All roles use the same
 * strictly increasing, nonzero epochs; calls must not be reentrant. There is
 * no reset, epoch wrap, or destruction while any caller is live. The callback
 * supplied by the coordinator alone runs, once per epoch. Exceptions terminate.
 *
 * Each role publishes its preceding writes with its arrival. The coordinator
 * acquires every arrival before the callback, then publishes the callback's
 * writes to followers through completion. A follower of an earlier generation
 * cannot miss completion: the next callback requires that follower's next
 * arrival, which happens only after its current wait (and intervening reads).
 * This also permits ordinary shared callback results to be read before the
 * next arrival without racing the next callback's writes.
 */
class SpinMailboxFence
{
  private:
    struct alignas(64) Epoch
    {
        std::atomic<uint64_t> value{0};
    };
    static_assert(sizeof(Epoch) >= 64, "Epochs must not share cache lines");
    const unsigned participants;
    const unsigned coordinator;
    std::array<Epoch, 8> arrivals;
    Epoch completion;

    static void pause() noexcept
    {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause");
#endif
    }

  public:
    SpinMailboxFence(unsigned count, unsigned leader)
        : participants(count), coordinator(leader)
    {
        if (!count || count > arrivals.size() || leader >= count)
            throw std::invalid_argument("Invalid mailbox fence membership/coordinator");
    }
    SpinMailboxFence(const SpinMailboxFence &) = delete;
    SpinMailboxFence &operator=(const SpinMailboxFence &) = delete;

    template<class Callback>
    void wait(unsigned role, uint64_t epoch, Callback &&callback) noexcept
    {
        if (role >= participants || !epoch)
            std::terminate();
        // Only this role's stable owner writes this slot. This relaxed load
        // validates the local sequence; it is not a synchronization operation.
        if (epoch <= arrivals[role].value.load(std::memory_order_relaxed))
            std::terminate();
        arrivals[role].value.store(epoch, std::memory_order_release);
        if (role == coordinator) {
            for (unsigned peer = 0; peer < participants; ++peer) {
                while (arrivals[peer].value.load(std::memory_order_acquire) != epoch)
                    pause();
            }
            callback(); // wait is noexcept, so a throwing callback terminates.
            completion.value.store(epoch, std::memory_order_release);
        } else {
            while (completion.value.load(std::memory_order_acquire) < epoch)
                pause();
        }
    }
};

} // namespace gem5
#endif // __BASE_SPIN_MAILBOX_FENCE_HH__
