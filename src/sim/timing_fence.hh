/*
 * Generic hooks for exact-timestamp timing handoffs. Hook installation and
 * packet-queue registration changes require stopped simulation queues.
 */

#ifndef __SIM_TIMING_FENCE_HH__
#define __SIM_TIMING_FENCE_HH__

#include <atomic>
#include <functional>
#include <vector>

#include "base/types.hh"

namespace gem5
{

class EventManager;
class PacketQueue;

// A successful handoff owns the callback and dispatches it on the destination
// queue at exactly when. force also fences sends within the same event queue.
inline bool (*timingFenceHandoff)(EventManager *, Tick, std::function<void()>,
                                 bool force) = nullptr;
inline bool (*timingFenceIsSafe)() = nullptr;

// Installed only while all queues are stopped. The checked bridge supplies
// every quantum boundary; native async publication is restricted to delayed
// global exits, whose priority cannot reorder ordinary model events.
inline bool timingFenceOwnsQuantumSync = false;
inline std::atomic<uint64_t> timingFenceAsyncPublishes{0};

// Process lifetime avoids static-destruction ordering with interrupt ports.
inline std::vector<PacketQueue *> &
timingFencePacketQueues()
{
    static auto *queues = new std::vector<PacketQueue *>;
    return *queues;
}

} // namespace gem5

#endif // __SIM_TIMING_FENCE_HH__
