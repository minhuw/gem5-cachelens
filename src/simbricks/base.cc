/*
 * Copyright 2022 Max Planck Institute for Software Systems, and
 * National University of Singapore
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "simbricks/base.hh"

#include "base/trace.hh"
#include "debug/SimBricks.hh"
#include "debug/SimBricksSync.hh"
#include "sim/core.hh"
#include "sim/serialize.hh"
#include <sstream>
#include <unistd.h>
#include "simbricks/cur_tick.hh"
#include "simbricks/init_manager.hh"

namespace gem5 {
namespace simbricks {
namespace base {

Adapter::Adapter(SimObject &parent, bool sync_)
    : EventManager(parent),
      sync(sync_), isListen(false), pollInterval(500000),
      inEvent([this]{ processInEvent(); }, name() + "SimBricksIn", false, 10),
      outSyncEvent([this]{ processOutSyncEvent(); }, name() + "SimBricksSync",
                   false, 0),
      pool(nullptr)
{
}

Adapter::~Adapter()
{
}

void
Adapter::close()
{
    if (checkpointEnabled) {
        // Opt-in peers observe control-socket EOF. The legacy close spins
        // forever trying to publish TERMINATE if a stalled peer left a full ring.
        if (baseIf.conn_fd >= 0) ::close(baseIf.conn_fd);
        if (baseIf.listen_fd >= 0) ::close(baseIf.listen_fd);
    } else {
        SimbricksBaseIfClose(&baseIf);
    }
    if (isListen)
        SimbricksBaseIfUnlink(&baseIf);

    if (pool) {
        SimbricksBaseIfSHMPoolUnlink(pool);
        SimbricksBaseIfSHMPoolUnmap(pool);
    }
}

void
Adapter::processInEvent()
{
    DPRINTF(SimBricks, "simbricks: processInEvent\n");

    /* run what we can */
    while (poll());

    if (sync) {
        /* in sychronized mode we might need to wait till we get a message with
         * a timestamp allowing us to proceed */
        uint64_t nextTs;
        while ((nextTs = SimbricksBaseIfInTimestamp(&baseIf)) <=
               curTickAsSimbricksTs()) {
          poll();
        }

        schedule(inEvent, fromSimbricksTs(nextTs));
    } else {
        /* in non-synchronized mode just poll at fixed intervals */
        schedule(inEvent, curTick() + pollInterval);
    }
}

void
Adapter::processOutSyncEvent()
{
  DPRINTF(SimBricksSync, "simbricks: sending sync message for ts=%lu\n",
          curTickAsSimbricksTs());
  while (SimbricksBaseIfOutSync(&baseIf, curTickAsSimbricksTs()))
    checkCheckpointDeadline();
  schedule(outSyncEvent, fromSimbricksTs(SimbricksBaseIfOutNextSync(&baseIf)));
}

void
Adapter::commonInit(const std::string &sock_path)
{
    SimbricksBaseIfDefaultParams(&params);
    initIfParams(params);

    params.sock_path = sock_path.c_str();
    params.blocking_conn = false;
    if (sync)
        params.sync_mode = kSimbricksBaseIfSyncRequired;
    else
        params.sync_mode = kSimbricksBaseIfSyncDisabled;

    if (SimbricksBaseIfInit(&baseIf, &params))
        panic("base init failed");

    registerExitCallback([this]() { close(); });
}

size_t
Adapter::introOutPrepare(void *data, size_t maxlen)
{
    return 0;
}

void
Adapter::introInReceived(const void *data, size_t len)
{
}

void
Adapter::handleInMsg(volatile union SimbricksProtoBaseMsg *msg)
{
    DPRINTF(SimBricks, "simbricks: ignoring unhandled incoming message\n");
    // by default just ignore messages but mark them as done
    inDone(msg);
}

void
Adapter::initIfParams(SimbricksBaseIfParams &p)
{
}


void
Adapter::connect(const std::string &sock_path)
{
    DPRINTF(SimBricks, "simbricks: initiating outgoing connection\n");

    commonInit(sock_path);

    if (SimbricksBaseIfConnect(&baseIf))
        panic("connecting failed");

    // register this adapter for the rest of the initialization
    // see init_manager.hh for rationale
    InitManager::get().registerAdapter(*this);
}

void
Adapter::listen(const std::string &sock_path, const std::string &shm_path)
{
    DPRINTF(SimBricks, "simbricks: listening for incomping connection\n");
    commonInit(sock_path);

    pool = new SimbricksBaseIfSHMPool;
    if (SimbricksBaseIfSHMPoolCreate(pool, shm_path.c_str(),
            SimbricksBaseIfSHMSize(&params)))
        panic("creating SHM pool failed");

    if (SimbricksBaseIfListen(&baseIf, pool))
        panic("listening failed");

    isListen = true;

    // register this adapter for the rest of the initialization
    // see init_manager.hh for rationale
    InitManager::get().registerAdapter(*this);
}

void
Adapter::init()
{
    // wait for initialization of this adapter to be complete
    InitManager::get().waitReady(*this);
}

void
Adapter::startup()
{
    setStartTick();
    if (checkpointEnabled) {
        fatal_if(sim_clock::Frequency != 1000000000000ULL, "NIC checkpoint requires picosecond ticks");
        std::string body;
        auto ready = nicbm::checkpoint::Receive(baseIf.conn_fd, body);
        fatal_if(ready.command != nicbm::checkpoint::Ready || !body.empty(),
                 "NIC checkpoint startup READY mismatch");
        checkpointPeer = ready;
        auto boot = checkpointToken;
        boot.command = nicbm::checkpoint::Boot;
        boot.epoch = deviceEpoch;
        boot.length = checkpointBody.size();
        boot.hash = nicbm::checkpoint::Hash(checkpointBody);
        nicbm::checkpoint::Send(baseIf.conn_fd, boot, checkpointBody);
    }
    // schedule first sync to be sent immediately
    if (sync)
        schedule(outSyncEvent, curTick());
    // next schedule
    schedule(inEvent, curTick() + 1);
}


void
Adapter::cfgCheckpoint(bool enabled)
{
    fatal_if(enabled && !sync, "Initialized NIC checkpoints require synchronization");
    fatal_if(enabled && eventQueue() != getEventQueue(0),
             "Initialized NIC checkpoint requires device event queue zero");
    checkpointEnabled = enabled;
}

void
Adapter::checkCheckpointDeadline() const
{
    fatal_if(checkpointGated && nicbm::checkpoint::Clock::now() >= checkpointDeadline,
             "Initialized NIC checkpoint wall deadline exceeded");
}

void
Adapter::checkpointBootReady()
{
    if (!checkpointEnabled) return;
    std::string body;
    auto ack = nicbm::checkpoint::Receive(baseIf.conn_fd, body);
    fatal_if(ack.command != nicbm::checkpoint::BootAck || !body.empty() ||
             !ack.SameToken(checkpointToken) || ack.epoch != deviceEpoch,
             "NIC checkpoint bootstrap token/epoch mismatch");
    checkpointBody.clear();
}

void
Adapter::checkpointBegin(uint64_t hi, uint64_t lo)
{
    fatal_if(!checkpointEnabled || checkpointGated || !baseIf.sync || !(hi || lo),
             "Invalid initialized NIC checkpoint begin");
    checkpointDeadline = nicbm::checkpoint::Deadline();
    checkpointGated = true;
    checkpointToken = nicbm::checkpoint::Empty(nicbm::checkpoint::Freeze);
    checkpointToken.hi = hi;
    checkpointToken.lo = lo;
    auto bytes = checkpointToken.Encode();
    // No waiting here: if there is no room for a FIFO marker, reject. Event
    // simulation starts only after BOTH host producers have been gated.
    auto *msg = SimbricksBaseIfOutAlloc(&baseIf, curTickAsSimbricksTs());
    fatal_if(!msg, "No transport space for NIC freeze marker");
    memcpy(const_cast<SimbricksProtoBaseMsg *>(msg), bytes.data(), 48);
    SimbricksBaseIfOutSend(&baseIf, msg, nicbm::checkpoint::MessageType);
}

void
Adapter::checkpointStop()
{
    fatal_if(!checkpointGated || !checkpointAck, "NIC checkpoint barrier incomplete");
    checkCheckpointDeadline();
    // Both endpoints are gated and both FIFO ACKs have arrived. Scan even
    // future slots for semantic messages; never discard them on reset.
    for (bool incoming : {true, false}) {
        auto *queue = static_cast<uint8_t *>(incoming ? baseIf.in_queue : baseIf.out_queue);
        size_t count = incoming ? baseIf.in_enum : baseIf.out_enum;
        size_t stride = incoming ? baseIf.in_elen : baseIf.out_elen;
        for (size_t i = 0; i < count; ++i) {
            auto *msg = reinterpret_cast<volatile SimbricksProtoBaseMsg *>(queue + i * stride);
            uint8_t own = atomic_load_explicit(
                reinterpret_cast<volatile _Atomic(uint8_t) *>(&msg->header.own_type),
                memory_order_acquire);
            fatal_if((own & SIMBRICKS_PROTO_MSG_OWN_MASK) == SIMBRICKS_PROTO_MSG_OWN_CON &&
                     (own & SIMBRICKS_PROTO_MSG_TYPE_MASK) != SIMBRICKS_PROTO_MSG_TYPE_SYNC,
                     "NIC checkpoint transport still contains a non-sync message");
        }
    }
    fatal_if(curTickAsSimbricksTs() > UINT64_MAX - deviceEpoch, "NIC epoch overflow");
    deviceEpoch += curTickAsSimbricksTs();
    checkpointToken.epoch = deviceEpoch;
    if (inEvent.scheduled()) deschedule(inEvent);
    if (outSyncEvent.scheduled()) deschedule(outSyncEvent);
}

std::string
Adapter::checkpointFetch()
{
    fatal_if(!checkpointGated || !checkpointAck, "NIC snapshot before freeze");
    auto request = checkpointToken;
    request.command = nicbm::checkpoint::Snapshot;
    nicbm::checkpoint::Send(baseIf.conn_fd, request);
    std::string body;
    auto result = nicbm::checkpoint::Receive(baseIf.conn_fd, body);
    fatal_if(result.command != nicbm::checkpoint::State ||
             !result.SameToken(checkpointToken) || result.epoch != deviceEpoch || body.empty(),
             "NIC snapshot token/epoch mismatch");
    checkpointToken = result;
    return body;
}

void
Adapter::rememberIntro(const void *data, size_t len, bool incoming)
{
    std::string value;
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; ++i) {
        value += "0123456789abcdef"[p[i] >> 4];
        value += "0123456789abcdef"[p[i] & 15];
    }
    (incoming ? incomingIntro : outgoingIntro) = value;
}

std::string
Adapter::checkpointIdentity() const
{
    std::ostringstream out;
    out << incomingIntro << ':' << outgoingIntro << ':' << sync << ':' << isListen
        << ':' << pollInterval << ':' << baseIf.sync << ':' << params.link_latency
        << ':' << params.sync_interval << ':' << baseIf.in_elen << ':' << baseIf.in_enum
        << ':' << baseIf.out_elen << ':' << baseIf.out_enum;
    return out.str();
}

void
Adapter::checkpointSerialize(CheckpointOut &cp) const
{
    fatal_if(!checkpointEnabled || !checkpointGated || !checkpointAck ||
             inEvent.scheduled() || outSyncEvent.scheduled(), "NIC not checkpoint-frozen");
    paramOut(cp, "initializedCheckpointVersion", 1);
    paramOut(cp, "initializedCheckpointIdentity", checkpointIdentity());
    paramOut(cp, "initializedCheckpointEpoch", deviceEpoch);
    paramOut(cp, "initializedCheckpointTick", curTick());
    paramOut(cp, "initializedCheckpointHi", checkpointToken.hi);
    paramOut(cp, "initializedCheckpointLo", checkpointToken.lo);
}

void
Adapter::checkpointUnserialize(CheckpointIn &cp)
{
    fatal_if(!checkpointEnabled, "Initialized NIC checkpoint restore requires opt-in");
    unsigned version;
    std::string identity;
    uint64_t tick;
    paramIn(cp, "initializedCheckpointVersion", version);
    paramIn(cp, "initializedCheckpointIdentity", identity);
    paramIn(cp, "initializedCheckpointEpoch", deviceEpoch);
    paramIn(cp, "initializedCheckpointTick", tick);
    paramIn(cp, "initializedCheckpointHi", checkpointToken.hi);
    paramIn(cp, "initializedCheckpointLo", checkpointToken.lo);
    fatal_if(version != 1 || identity != checkpointIdentity() || tick != curTick() ||
             !(checkpointToken.hi || checkpointToken.lo), "NIC checkpoint version/identity/tick mismatch");
    checkpointToken.epoch = deviceEpoch;
}

bool
Adapter::poll()
{
    checkCheckpointDeadline();
    volatile union SimbricksProtoBaseMsg *msg =
        SimbricksBaseIfInPoll(&baseIf, curTickAsSimbricksTs());
    if (!msg)
        return false;

    // don't pass sync messages to handle msg function
    if (SimbricksBaseIfInType(&baseIf, msg) == SIMBRICKS_PROTO_MSG_TYPE_SYNC) {
        inDone(msg);
        return true;
    }

    if (SimbricksBaseIfInType(&baseIf, msg) == nicbm::checkpoint::MessageType) {
        std::string bytes(64, '\0');
        memcpy(bytes.data(), const_cast<const SimbricksProtoBaseMsg *>(msg), 32);
        auto control = nicbm::checkpoint::Control::Decode(bytes);
        fatal_if(!checkpointEnabled || !checkpointGated || checkpointAck ||
                 control.command != nicbm::checkpoint::Frozen ||
                 !control.SameToken(checkpointToken), "Invalid NIC freeze ACK");
        checkpointAck = true;
        inDone(msg);
        return true;
    }
    fatal_if(checkpointGated, "NIC semantic message after checkpoint producer gate");
    handleInMsg(msg);
    return true;
}

} // namespace base
} // namespace simbricks
} // namespace gem5
