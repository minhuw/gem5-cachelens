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

#include <debug/SimBricksEthernet.hh>
#include <simbricks/ethernet.hh>

#include "base/trace.hh"

namespace gem5 {
namespace simbricks {
namespace ethernet {

extern "C" {
#include <simbricks/network/if.h>

}

Adapter::EthernetStats::EthernetStats(statistics::Group *parent)
    : statistics::Group(parent, "Ethernet"),
      ADD_STAT(incomingPackets, statistics::units::Count::get(), "NIC to gem5 packets"),
      ADD_STAT(incomingBytes, statistics::units::Byte::get(), "NIC to gem5 bytes"),
      ADD_STAT(acceptedPackets, statistics::units::Count::get(), "NIC packets accepted by gem5"),
      ADD_STAT(acceptedBytes, statistics::units::Byte::get(), "NIC bytes accepted by gem5"),
      ADD_STAT(droppedPackets, statistics::units::Count::get(), "NIC packets dropped on gem5 backpressure"),
      ADD_STAT(droppedBytes, statistics::units::Byte::get(), "NIC bytes dropped on gem5 backpressure"),
      ADD_STAT(outgoingPackets, statistics::units::Count::get(), "gem5 to NIC packets"),
      ADD_STAT(outgoingBytes, statistics::units::Byte::get(), "gem5 to NIC bytes"),
      ADD_STAT(pendingPackets, statistics::units::Count::get(), "Owned NIC packets awaiting gem5 acceptance"),
      ADD_STAT(pendingHighWatermark, statistics::units::Count::get(), "Maximum pending NIC packets"),
      ADD_STAT(retryAttempts, statistics::units::Count::get(), "Completion-driven NIC packet retry attempts"),
      ADD_STAT(overflowPackets, statistics::units::Count::get(), "Fatal NIC receive FIFO overflows")
{}

Adapter::Adapter(const Params &p)
    : SimObject(p),
    base::GenericBaseAdapter<SimbricksProtoNetMsg, SimbricksProtoNetMsg>::
        Interface(*this),
    adapter(*this, *this, p.sync),
    sync(p.sync),
    rxBufferPackets(p.rx_buffer_packets),
    ethernetStats(this)
{
    DPRINTF(SimBricksEthernet, "device constructed\n");
    fatal_if(!rxBufferPackets, "rx_buffer_packets must be positive");

    interface = new Interface(name() + ".int0", this);

    adapter.cfgCheckpoint(p.initialized_checkpoint);
    adapter.cfgSetPollInterval(p.poll_interval);
    if (p.listen)
        adapter.listen(p.uxsocket_path, p.shm_path);
    else
        adapter.connect(p.uxsocket_path);
}

Adapter::~Adapter()
{
    // No retry events are owned here. The deque releases any unsent frames;
    // accepted frames are owned by the peer, independent of this interface.
    delete interface;
}

size_t
Adapter::introOutPrepare(void *data, size_t maxlen)
{
    size_t introlen = sizeof(struct SimbricksProtoNetIntro);
    assert(introlen <= maxlen);
    memset(data, 0, introlen);
    return introlen;
}

void
Adapter::introInReceived(const void *data, size_t len)
{
    panic_if(len != sizeof(struct SimbricksProtoNetIntro), "Invalid Ethernet intro");
}

void
Adapter::initIfParams(SimbricksBaseIfParams &p)
{
    SimbricksNetIfDefaultParams(&p);
    p.link_latency = params().link_latency;
    p.sync_interval = params().sync_tx_interval;
}

Port &
Adapter::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "int0") {
        return *interface;
    }
    return SimObject::getPort(if_name, idx);
}

void
Adapter::init()
{
    /* not calling parent init on purpose, as that will cause problems because
     * PIO port is not connected */
    if (!interface->isConnected())
        panic("int0 interface of %s not connected!", name());

    adapter.init();
}

void
Adapter::handleInMsg(volatile union SimbricksProtoNetMsg *msg)
{
    packetHistory = true;
    volatile struct SimbricksProtoNetMsgPacket *pkt = &msg->packet;

    uint8_t ty = adapter.inType(msg);
    if (ty != SIMBRICKS_PROTO_NET_MSG_PACKET) {
        panic("simbricks::ethernet: unsupported msg type=%x", ty);
    }

    unsigned len = pkt->len;
    panic_if(!len || len > adapter.inPayload(), "Invalid SimBricks Ethernet length");
    DPRINTF(SimBricksEthernet, "real->sim len=%d\n", len);
    ethernetStats.incomingPackets++;
    ethernetStats.incomingBytes += len;
    if (rxPending.size() == rxBufferPackets) {
        ethernetStats.overflowPackets++;
        fatal("SimBricks Ethernet receive FIFO full: %u pending packets, "
              "rx_buffer_packets=%u; cannot suspend SimBricks input",
              static_cast<unsigned>(rxPending.size()), rxBufferPackets);
    }

    EthPacketPtr packet = std::make_shared<EthPacketData>(len);
    packet->length = len;
    packet->simLength = len;
    memcpy(packet->data, (const void *) pkt->data, len);
    // Never retain ring storage: enqueue the deep copy before releasing it.
    rxPending.push_back(packet);
    ethernetStats.pendingPackets = rxPending.size();
    if (rxPending.size() > ethernetStats.pendingHighWatermark.value())
        ethernetStats.pendingHighWatermark = rxPending.size();
    adapter.inDone(msg);
    pumpRx();
}

void
Adapter::pumpRx()
{
    if (rxPumping || rxBlocked)
        return;

    // recvDone() may call sendDone() synchronously from sendPacket(). Keep
    // the front frame until the return value establishes peer acceptance.
    rxPumping = true;
    while (!rxPending.empty()) {
        EthPacketPtr packet = rxPending.front();
        if (!interface->sendPacket(packet)) {
            rxBlocked = true;
            break;
        }
        rxPending.pop_front();
        ethernetStats.pendingPackets = rxPending.size();
        ethernetStats.acceptedPackets++;
        ethernetStats.acceptedBytes += packet->length;
    }
    rxPumping = false;
    if (rxPending.empty())
        signalDrainDone();
}

void
Adapter::sendDone()
{
    // EtherInt completion can be synchronous; an idle notification is also
    // harmless. A failed send waits here instead of spinning or overtaking.
    rxBlocked = false;
    if (rxPumping)
        return;
    if (!rxPending.empty())
        ethernetStats.retryAttempts++;
    pumpRx();
}

DrainState
Adapter::drain()
{
    // A true sendPacket() transfers ownership to the peer. Not all EtherInt
    // peers issue a completion for accepted frames; only our owned work drains
    // here. sendDone() can finish draining the pending FIFO asynchronously.
    return rxPending.empty() && !rxPumping ?
        DrainState::Drained : DrainState::Draining;
}

bool
Adapter::recvPacket(EthPacketPtr packet)
{
    packetHistory = true;
    DPRINTF(SimBricksEthernet, "sending out a packet\n");
    volatile union SimbricksProtoNetMsg *msg_to = adapter.outAlloc();

    volatile struct SimbricksProtoNetMsgPacket *pkt_to = &msg_to->packet;
    panic_if(!packet->length || packet->length > adapter.outPayload(),
             "Invalid SimBricks Ethernet transmit length");
    pkt_to->len = packet->length;
    pkt_to->port = 0;
    memcpy((void *)pkt_to->data, packet->data, packet->length);

    adapter.outSend(msg_to, SIMBRICKS_PROTO_NET_MSG_PACKET);
    ethernetStats.outgoingPackets++;
    ethernetStats.outgoingBytes += packet->length;
    interface->recvDone();
    return true;
}

void
Adapter::checkInitializedCheckpoint() const
{
    fatal_if(!params().initialized_checkpoint || packetHistory,
             "Initialized NIC checkpoint requires traffic-free Ethernet");
}

void
Adapter::serialize(CheckpointOut &cp) const
{
    checkInitializedCheckpoint();
    adapter.checkpointSerialize(cp);
    SimObject::serialize(cp);
}

void
Adapter::unserialize(CheckpointIn &cp)
{
    checkInitializedCheckpoint();
    adapter.checkpointUnserialize(cp);
    SimObject::unserialize(cp);
}

void
Adapter::startup()
{
    adapter.startup();
}

} // namespace ethernet
} // namespace simbricks
} // namespace gem5
