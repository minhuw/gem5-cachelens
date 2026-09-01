// SPDX-License-Identifier: BSD-3-Clause

#ifndef __TEST_OBJECTS_PVRDMA_TEST_LINK_HH__
#define __TEST_OBJECTS_PVRDMA_TEST_LINK_HH__

#include <cstdint>
#include <deque>
#include <list>
#include <map>

#include "dev/net/etherint.hh"
#include "dev/rdma/pvrdma_rocev1.hh"
#include "params/PvrdmaTestLink.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class PvrdmaTestLink : public SimObject
{
  public:
    enum class Direction
    {
        Int0ToInt1,
        Int1ToInt0,
    };

    struct FrameId
    {
        Direction direction;
        pvrdma::rocev1::Opcode opcode;
        pvrdma::rocev1::Syndrome syndrome;
        uint32_t psn;
    };

    using Params = PvrdmaTestLinkParams;
    PvrdmaTestLink(const Params &p);
    ~PvrdmaTestLink() override;

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    void dropOnce(const FrameId &id);
    void duplicateOnce(const FrameId &id);
    void delayOnce(const FrameId &id, Tick delay);
    void holdOnce(const FrameId &id);
    bool release(const FrameId &id);
    bool releaseAt(const FrameId &id, Tick when);

    size_t heldPackets() const { return held.size(); }
    size_t pendingRules() const { return rules.size(); }

    DrainState drain() override;

  private:
    class Interface : public EtherInt
    {
      public:
        Interface(const std::string &name, PvrdmaTestLink &link, int side)
            : EtherInt(name), link(link), side(side)
        {}

        bool recvPacket(EthPacketPtr packet) override;
        void sendDone() override {}
        void completeSender() { peer->sendDone(); }

      private:
        PvrdmaTestLink &link;
        const int side;
    };

    enum class Action
    {
        Forward,
        Drop,
        Duplicate,
        Delay,
        Hold,
    };

    struct Rule
    {
        FrameId id;
        Action action;
        Tick delay = 0;
    };

    struct Delivery
    {
        int destination;
        EthPacketPtr packet;
    };

    struct Held
    {
        FrameId id;
        Delivery delivery;
    };

    Interface *interfaces[2];
    std::list<Rule> rules;
    // ponytail: one global held list is enough for deterministic pair tests;
    // split it per direction only if high-volume fault scripts need it.
    std::list<Held> held;
    std::multimap<Tick, Delivery> deliveries;
    std::deque<int> completions;
    EventFunctionWrapper deliveryEvent;
    EventFunctionWrapper completionEvent;

    bool receive(int source, EthPacketPtr packet);
    void addRule(const FrameId &id, Action action, Tick delay = 0);
    void queueDelivery(Delivery delivery, Tick when);
    void processDeliveries();
    void processCompletions();
    void checkDrain();
    bool idle() const;

    static bool sameId(const FrameId &left, const FrameId &right);
};

} // namespace gem5

#endif // __TEST_OBJECTS_PVRDMA_TEST_LINK_HH__
