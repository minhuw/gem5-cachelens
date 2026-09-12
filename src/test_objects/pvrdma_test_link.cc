// SPDX-License-Identifier: BSD-3-Clause

#include "test_objects/pvrdma_test_link.hh"

#include <algorithm>

#include "base/logging.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

PvrdmaTestLink::PvrdmaTestLink(const Params &p)
    : SimObject(p),
      deliveryEvent([this] { processDeliveries(); }, name() + ".delivery"),
      completionEvent([this] { processCompletions(); }, name() + ".completion")
{
    interfaces[0] = new Interface(name() + ".int0", *this, 0);
    interfaces[1] = new Interface(name() + ".int1", *this, 1);
}

PvrdmaTestLink::~PvrdmaTestLink()
{
    delete interfaces[0];
    delete interfaces[1];
}

Port &
PvrdmaTestLink::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "int0")
        return *interfaces[0];
    if (if_name == "int1")
        return *interfaces[1];
    return SimObject::getPort(if_name, idx);
}

bool
PvrdmaTestLink::Interface::recvPacket(EthPacketPtr packet)
{
    return link.receive(side, std::move(packet));
}

bool
PvrdmaTestLink::sameId(const FrameId &left, const FrameId &right)
{
    return left.direction == right.direction &&
        left.opcode == right.opcode && left.syndrome == right.syndrome &&
        left.psn == right.psn;
}

void
PvrdmaTestLink::addRule(const FrameId &id, Action action, Tick delay)
{
    rules.push_back({id, action, delay});
}

void
PvrdmaTestLink::dropOnce(const FrameId &id)
{
    addRule(id, Action::Drop);
}

void
PvrdmaTestLink::duplicateOnce(const FrameId &id)
{
    addRule(id, Action::Duplicate);
}

void
PvrdmaTestLink::delayOnce(const FrameId &id, Tick delay)
{
    addRule(id, Action::Delay, delay);
}

void
PvrdmaTestLink::holdOnce(const FrameId &id)
{
    addRule(id, Action::Hold);
}

bool
PvrdmaTestLink::release(const FrameId &id)
{
    return releaseAt(id, curTick() + 1);
}

bool
PvrdmaTestLink::releaseAt(const FrameId &id, Tick when)
{
    const auto entry = std::find_if(held.begin(), held.end(),
        [&id](const Held &candidate) {
            return sameId(candidate.id, id);
        });
    if (entry == held.end() || when <= curTick())
        return false;
    queueDelivery(std::move(entry->delivery), when);
    held.erase(entry);
    return true;
}

bool
PvrdmaTestLink::receive(int source, EthPacketPtr packet)
{
    if (!packet)
        return false;
    panic_if(packet->length < pvrdma::rocev2::EthernetHeaderSize ||
                 packet->data[pvrdma::rocev2::EtherTypeOffset] != 0x86 ||
                 packet->data[pvrdma::rocev2::EtherTypeOffset + 1] != 0xdd,
             "PVRDMA test link observed a non-RoCEv2 production frame");

    const Direction direction = source == 0 ? Direction::Int0ToInt1 :
                                              Direction::Int1ToInt0;
    Action action = Action::Forward;
    Tick delay = 0;
    const auto decoded = packet->length <= packet->bufLength ?
        pvrdma::rocev2::decode(
            {packet->data, packet->bufLength}, packet->length) :
        pvrdma::rocev2::DecodeResult{};
    if (decoded) {
        const auto &frame = decoded.packet;
        const FrameId id{direction, frame.opcode,
            frame.opcode == pvrdma::rocev2::Opcode::Acknowledge ?
                frame.syndrome : pvrdma::rocev2::Syndrome::Ack,
            frame.psn};
        const auto rule = std::find_if(rules.begin(), rules.end(),
            [&id](const Rule &candidate) {
                return sameId(candidate.id, id);
            });
        if (rule != rules.end()) {
            action = rule->action;
            delay = rule->delay;
            rules.erase(rule);
        }

        if (action == Action::Hold) {
            held.push_back({id, {1 - source, packet}});
        } else if (action != Action::Drop) {
            const Tick when = curTick() + (action == Action::Delay ?
                std::max<Tick>(delay, 1) : 1);
            queueDelivery({1 - source, packet}, when);
            if (action == Action::Duplicate)
                queueDelivery({1 - source, packet}, when);
        }
    } else {
        queueDelivery({1 - source, packet}, curTick() + 1);
    }

    completions.push_back(source);
    if (!completionEvent.scheduled())
        schedule(completionEvent, curTick() + 1);
    return true;
}

void
PvrdmaTestLink::queueDelivery(Delivery delivery, Tick when)
{
    deliveries.emplace(when, std::move(delivery));
    if (!deliveryEvent.scheduled())
        schedule(deliveryEvent, when);
    else if (when < deliveryEvent.when())
        reschedule(deliveryEvent, when);
}

void
PvrdmaTestLink::processDeliveries()
{
    while (!deliveries.empty() && deliveries.begin()->first <= curTick()) {
        const auto delivery = deliveries.begin();
        if (!interfaces[delivery->second.destination]->sendPacket(
                delivery->second.packet)) {
            schedule(deliveryEvent, curTick() + 1);
            checkDrain();
            return;
        }
        deliveries.erase(delivery);
    }
    if (!deliveries.empty() && !deliveryEvent.scheduled())
        schedule(deliveryEvent, deliveries.begin()->first);
    checkDrain();
}

void
PvrdmaTestLink::processCompletions()
{
    const size_t count = completions.size();
    for (size_t i = 0; i < count; ++i) {
        const int source = completions.front();
        completions.pop_front();
        interfaces[source]->completeSender();
    }
    if (!completions.empty() && !completionEvent.scheduled())
        schedule(completionEvent, curTick() + 1);
    checkDrain();
}

bool
PvrdmaTestLink::idle() const
{
    return held.empty() && deliveries.empty() && completions.empty() &&
        !deliveryEvent.scheduled() && !completionEvent.scheduled();
}

DrainState
PvrdmaTestLink::drain()
{
    return idle() ? DrainState::Drained : DrainState::Draining;
}

void
PvrdmaTestLink::checkDrain()
{
    if (idle())
        signalDrainDone();
}

} // namespace gem5
