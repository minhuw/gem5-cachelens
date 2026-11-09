/*
 * Generic hooks for exact-timestamp timing handoffs. Hook installation and
 * packet-queue registration changes require stopped simulation queues.
 */

#ifndef __SIM_TIMING_FENCE_HH__
#define __SIM_TIMING_FENCE_HH__

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "base/types.hh"

namespace gem5
{

class EventManager;
class PacketQueue;

// Direction is part of the domain: request and response producers can belong
// to different queues even when their RubyPort is the same.
enum class TimingFenceDomain : uint8_t
{
    RubyPioRequest, RubyPioResponse, RubyPioPortResponse, PacketQueueSend, IoTest
};

// Endpoint IDs are full port names (packet queues use their send-event name),
// compared lexicographically, never pointer values, queue IDs or hash values.
// Each endpoint belongs to one producing side. Only that queue thread writes
// sequence, or the serialized dispatcher while all queue threads are stopped.
struct TimingFenceEndpoint
{
    const TimingFenceDomain domain;
    const std::string id;
    uint64_t sequence = 0;
    TimingFenceEndpoint(TimingFenceDomain d, const std::string &name)
        : domain(d), id(name) {}
};

// A successful handoff owns the callback and dispatches it on the destination
// queue at exactly when. force also fences sends within the same event queue.
inline bool (*timingFenceHandoff)(TimingFenceEndpoint &, EventManager *,
                                 Tick, std::function<void()>,
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
