// SPDX-License-Identifier: BSD-3-Clause

#include "dev/rdma/pvrdma.hh"

#include <cstring>

#include "base/logging.hh"
#include "mem/packet_access.hh"
#include "sim/core.hh"
#include "sim/serialize.hh"

namespace gem5
{

Pvrdma::QueueStats::QueueStats(Pvrdma &parent)
    : statistics::Group(&parent, "queues"), device(parent),
      ADD_STAT(sqDepth, statistics::units::Count::get(),
               "Aggregate live SQ depth"),
      ADD_STAT(rqDepth, statistics::units::Count::get(),
               "Aggregate live RQ depth"),
      ADD_STAT(cqDepth, statistics::units::Count::get(),
               "Aggregate live CQ depth"),
      ADD_STAT(sqOutstanding, statistics::units::Count::get(),
               "Observed outstanding SQ entries"),
      ADD_STAT(rqAvailable, statistics::units::Count::get(),
               "Observed available RQ entries"),
      ADD_STAT(cqOutstanding, statistics::units::Count::get(),
               "Observed outstanding CQ entries"),
      ADD_STAT(sqOutstandingAtReset, statistics::units::Count::get(),
               "SQ occupancy at statistics reset"),
      ADD_STAT(rqAvailableAtReset, statistics::units::Count::get(),
               "RQ occupancy at statistics reset"),
      ADD_STAT(cqOutstandingAtReset, statistics::units::Count::get(),
               "CQ occupancy at statistics reset"),
      ADD_STAT(sqPosted, statistics::units::Count::get(),
               "Observed SQ producer advances"),
      ADD_STAT(rqPosted, statistics::units::Count::get(),
               "Observed RQ producer advances"),
      ADD_STAT(sqConsumed, statistics::units::Count::get(),
               "Completed SQ consumer advances"),
      ADD_STAT(rqConsumed, statistics::units::Count::get(),
               "Completed RQ consumer advances"),
      ADD_STAT(cqReclaimed, statistics::units::Count::get(),
               "Observed CQ consumer advances"),
      ADD_STAT(cqPublished, statistics::units::Count::get(),
               "Published completion queue entries"),
      ADD_STAT(cqErrorPublished, statistics::units::Count::get(),
               "Published error completion queue entries"),
      ADD_STAT(cqPublicationRejected, statistics::units::Count::get(),
               "Rejected completion publication requests"),
      ADD_STAT(cqPublicationBackpressured, statistics::units::Count::get(),
               "Completion publications blocked by full rings"),
      ADD_STAT(sqResetDiscarded, statistics::units::Count::get(),
               "SQ occupancy discarded by reset or removal"),
      ADD_STAT(rqResetDiscarded, statistics::units::Count::get(),
               "RQ occupancy discarded by reset or removal"),
      ADD_STAT(cqResetDiscarded, statistics::units::Count::get(),
               "CQ occupancy discarded by reset or removal"),
      ADD_STAT(doorbellWrites, statistics::units::Count::get(),
               "BAR2 doorbell writes"),
      ADD_STAT(sqDoorbells, statistics::units::Count::get(),
               "Accepted SQ doorbells"),
      ADD_STAT(rqDoorbells, statistics::units::Count::get(),
               "Accepted RQ doorbells"),
      ADD_STAT(cqPollDoorbells, statistics::units::Count::get(),
               "Accepted CQ poll doorbells"),
      ADD_STAT(cqArmDoorbells, statistics::units::Count::get(),
               "Accepted CQ arm doorbells"),
      ADD_STAT(cqArmSolicitedDoorbells, statistics::units::Count::get(),
               "Accepted solicited-only CQ arm doorbells"),
      ADD_STAT(doorbellWritesRejected, statistics::units::Count::get(),
               "Rejected BAR2 accesses"),
      ADD_STAT(ringObservationsRejected, statistics::units::Count::get(),
               "Rejected coherent ring snapshots"),
      ADD_STAT(conservationViolations, statistics::units::Count::get(),
               "Queue accounting conservation violations"),
      ADD_STAT(sqOccupancy, statistics::units::Count::get(),
               "Time-weighted aggregate SQ occupancy"),
      ADD_STAT(rqOccupancy, statistics::units::Count::get(),
               "Time-weighted aggregate RQ occupancy"),
      ADD_STAT(cqOccupancy, statistics::units::Count::get(),
               "Time-weighted aggregate CQ occupancy")
{
    sqOccupancy.init(0, 65534, 255);
    rqOccupancy.init(0, 65534, 255);
    cqOccupancy.init(0, 65534, 255);
}

void
Pvrdma::QueueStats::preDumpStats()
{
    statistics::Group::preDumpStats();
    device.sampleQueueOccupancy();
    device.checkQueueConservation();
}

Pvrdma::Pvrdma(const Params &p)
    : PciDevice(p), interface(name() + ".interface", *this),
      controlCompletionLatency(p.control_completion_latency),
      queueStats(*this),
      dsrReadEvent([this] { dsrReadDone(); }, name() + ".dsrRead"),
      capsWriteEvent([this] { capsWriteDone(); }, name() + ".capsWrite"),
      commandReadEvent([this] { commandReadDone(); }, name() + ".commandRead"),
      objectDirectoryReadEvent([this] { objectDirectoryReadDone(); },
                               name() + ".objectDirectoryRead"),
      objectTableReadEvent([this] { objectTableReadDone(); },
                           name() + ".objectTableRead"),
      responseWriteEvent([this] { responseWriteDone(); },
                         name() + ".responseWrite"),
      observationEvent([this] { startObservation(); },
                       name() + ".queueObservation"),
      queueDmaEvent([this] { queueDmaDone(); }, name() + ".queueDma"),
      completionDmaEvent(
          [this] {
              if (completionDma.queued())
                  startCompletion();
              else
                  completionDmaDone();
          }, name() + ".completionDma"),
      transportEvent([this] { runTransport(); }, name() + ".transport"),
      transportDmaEvent([this] { transportDmaDone(); },
                        name() + ".transportDma"),
      transportTimerEvent([this] { transportTimerExpired(); },
                          name() + ".transportTimer")
{
    const auto *mac = p.hardware_address.bytes();
    regs.macLow = static_cast<uint32_t>(mac[0]) |
                  (static_cast<uint32_t>(mac[1]) << 8) |
                  (static_cast<uint32_t>(mac[2]) << 16) |
                  (static_cast<uint32_t>(mac[3]) << 24);
    regs.macHigh = static_cast<uint32_t>(mac[4]) |
                   (static_cast<uint32_t>(mac[5]) << 8);
    regs.reset();
    capabilities = pvrdma::makeCapabilities(regs.macLow, regs.macHigh);
    queueStatsReset();
    statistics::registerResetCallback([this] { queueStatsReset(); });
}

Port &
Pvrdma::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "interface")
        return interface;
    return PciDevice::getPort(if_name, idx);
}

void
Pvrdma::startup()
{
    PciDevice::startup();
    queueStatsReset();
}

Tick
Pvrdma::read(PacketPtr pkt)
{
    int bar;
    Addr offset;
    panic_if(!getBAR(pkt->getAddr(), bar, offset),
             "PVRDMA read from unmapped PCI address %#x", pkt->getAddr());
    if (bar == pvrdma::UarBar) {
        std::memset(pkt->getPtr<uint8_t>(), 0, pkt->getSize());
        queueStats.doorbellWritesRejected++;
        pkt->makeAtomicResponse();
        return pioDelay;
    }
    panic_if(bar != pvrdma::RegisterBar,
             "PVRDMA datapath BAR%d read is not implemented", bar);
    panic_if(!pvrdma::validRegisterAccess(offset, pkt->getSize()),
             "Invalid PVRDMA register read at offset %#x size %u", offset,
             pkt->getSize());

    const auto reg = pvrdma::decodeRegister(offset);
    panic_if(!pvrdma::registerReadable(reg),
             "Read from write-only PVRDMA register at offset %#x", offset);

    uint32_t value = 0;
    switch (reg) {
      case pvrdma::Register::Version:
        value = pvrdma::Version;
        break;
      case pvrdma::Register::Error:
        value = regs.error;
        break;
      case pvrdma::Register::InterruptCause:
        value = regs.acknowledgeInterrupts();
        updateInterrupt();
        break;
      case pvrdma::Register::InterruptMask:
        value = regs.interruptMask;
        break;
      case pvrdma::Register::MacLow:
        value = regs.macLow;
        break;
      case pvrdma::Register::MacHigh:
        value = regs.macHigh;
        break;
      default:
        panic("Invalid readable PVRDMA register at offset %#x", offset);
    }

    pkt->setLE<uint32_t>(value);
    pkt->makeAtomicResponse();
    return pioDelay;
}

Tick
Pvrdma::write(PacketPtr pkt)
{
    int bar;
    Addr offset;
    panic_if(!getBAR(pkt->getAddr(), bar, offset),
             "PVRDMA write to unmapped PCI address %#x", pkt->getAddr());
    if (bar == pvrdma::UarBar) {
        writeDoorbell(offset, pkt);
        pkt->makeAtomicResponse();
        return pioDelay;
    }
    panic_if(bar != pvrdma::RegisterBar,
             "PVRDMA datapath BAR%d write is not implemented", bar);
    panic_if(!pvrdma::validRegisterAccess(offset, pkt->getSize()),
             "Invalid PVRDMA register write at offset %#x size %u", offset,
             pkt->getSize());

    const auto reg = pvrdma::decodeRegister(offset);
    panic_if(!pvrdma::registerWritable(reg),
             "Write to read-only PVRDMA register at offset %#x", offset);

    const uint32_t value = pkt->getLE<uint32_t>();
    Tick delay = pioDelay;
    switch (reg) {
      case pvrdma::Register::DsrLow:
        if (controlState == pvrdma::ControlState::Unconfigured)
            regs.writeDsrLow(value);
        else
            operationError.set(regs.error, pvrdma::CommandError);
        break;
      case pvrdma::Register::DsrHigh:
        if (controlState != pvrdma::ControlState::Unconfigured ||
            !regs.writeDsrHigh(value)) {
            operationError.set(regs.error, pvrdma::CommandError);
        } else {
            startDsr();
        }
        // ponytail: this unloaded-probe delay is a ceiling; use deferred PIO
        // completion if loaded Ruby paths can exceed it.
        delay = controlCompletionLatency;
        break;
      case pvrdma::Register::Control:
        writeControl(value);
        break;
      case pvrdma::Register::Request:
        startCommand(value);
        delay = controlCompletionLatency;
        break;
      case pvrdma::Register::InterruptMask:
        regs.interruptMask = value;
        updateInterrupt();
        break;
      case pvrdma::Register::MacLow:
        if (transportActive())
            operationError.set(regs.error, pvrdma::CommandError);
        else
            regs.macLow = value;
        break;
      case pvrdma::Register::MacHigh:
        if (transportActive())
            operationError.set(regs.error, pvrdma::CommandError);
        else
            regs.writeMacHigh(value);
        break;
      default:
        panic("Invalid writable PVRDMA register at offset %#x", offset);
    }

    pkt->makeAtomicResponse();
    return delay;
}

bool
Pvrdma::observationQueued() const
{
    return sqDirty || rqDirty || cqDirty || observationEvent.scheduled();
}

bool
Pvrdma::completionBusy() const
{
    return completionDma.active() || completionDmaEvent.scheduled();
}

bool
Pvrdma::commandBlockedByObservation() const
{
    return observationQueued() || queueDma.active() || completionBusy() ||
        transportActive();
}

bool
Pvrdma::validDoorbell(const pvrdma::Doorbell &doorbell) const
{
    return pvrdma::validDoorbell(doorbell, controlState, completionQueues,
                                 queuePairs);
}

void
Pvrdma::writeDoorbell(uint64_t offset, PacketPtr pkt)
{
    queueStats.doorbellWrites++;
    pvrdma::Doorbell doorbell;
    if (pkt->getSize() != sizeof(uint32_t) ||
        !pvrdma::decodeDoorbell(offset, pkt->getSize(),
                               pkt->getLE<uint32_t>(), doorbell) ||
        !validDoorbell(doorbell)) {
        queueStats.doorbellWritesRejected++;
        return;
    }

    switch (doorbell.action) {
      case pvrdma::DoorbellAction::Sq:
        queueStats.sqDoorbells++;
        break;
      case pvrdma::DoorbellAction::Rq:
        queueStats.rqDoorbells++;
        break;
      case pvrdma::DoorbellAction::CqPoll:
        queueStats.cqPollDoorbells++;
        if (transport.active() && transport.cqHandle == doorbell.handle)
            transport.completionBackpressured = false;
        break;
      case pvrdma::DoorbellAction::CqArmSolicited:
        queueStats.cqArmDoorbells++;
        queueStats.cqArmSolicitedDoorbells++;
        completionQueues.entries[doorbell.handle].armFlags =
            static_cast<uint32_t>(pvrdma::CqArmMode::Solicited);
        break;
      case pvrdma::DoorbellAction::CqArmAny:
        queueStats.cqArmDoorbells++;
        completionQueues.entries[doorbell.handle].armFlags =
            static_cast<uint32_t>(pvrdma::CqArmMode::Any);
        break;
      default:
        panic("PVRDMA accepted invalid doorbell action");
    }
    markDirty(pvrdma::queueKind(doorbell.action), doorbell.handle);
}

void
Pvrdma::markDirty(pvrdma::QueueKind kind, uint32_t handle)
{
    const uint64_t bit = uint64_t{1} << handle;
    if (kind == pvrdma::QueueKind::Sq)
        sqDirty |= bit;
    else if (kind == pvrdma::QueueKind::Rq)
        rqDirty |= bit;
    else
        cqDirty |= bit;
    scheduleObservation();
}

void
Pvrdma::scheduleObservation()
{
    if (controlState == pvrdma::ControlState::Active &&
        !queueDma.active() && !completionBusy() && !transportDmaBusy() &&
        observationQueued() &&
        !observationEvent.scheduled())
        schedule(observationEvent, nextCycle());
}

bool
Pvrdma::selectObservation()
{
    for (uint32_t i = 0; i < 3 * pvrdma::ObjectTableEntries; ++i) {
        const uint32_t index = (observationCursor + i) %
            (3 * pvrdma::ObjectTableEntries);
        const uint32_t handle = index % pvrdma::ObjectTableEntries;
        if (!handle)
            continue;
        const auto kind = static_cast<pvrdma::QueueKind>(
            index / pvrdma::ObjectTableEntries + 1);
        uint64_t *mask = kind == pvrdma::QueueKind::Sq ? &sqDirty :
            kind == pvrdma::QueueKind::Rq ? &rqDirty : &cqDirty;
        const uint64_t bit = uint64_t{1} << handle;
        if (!(*mask & bit))
            continue;
        *mask &= ~bit;
        observationCursor = (index + 1) %
            (3 * pvrdma::ObjectTableEntries);
        queueDma.kind = kind;
        queueDma.handle = handle;
        if (kind == pvrdma::QueueKind::Cq) {
            const auto &cq = completionQueues.entries[handle];
            queueDma.generation = cq.generation;
            queueDma.uar = cq.uar;
            queueDma.depth = cq.cqe;
            queueDma.ringAddress = cq.pages.empty() ? 0 :
                cq.pages[0] + offsetof(pvrdma::RingState, rx);
        } else {
            const auto &qp = queuePairs.entries[handle];
            queueDma.generation = qp.generation;
            queueDma.uar = qp.uar;
            queueDma.depth = kind == pvrdma::QueueKind::Sq ?
                qp.capabilities.maxSendWr : qp.capabilities.maxRecvWr;
            queueDma.ringAddress = qp.pages.empty() ? 0 : qp.pages[0] +
                (kind == pvrdma::QueueKind::Sq ?
                     offsetof(pvrdma::RingState, tx) :
                     offsetof(pvrdma::RingState, rx));
        }
        return true;
    }
    return false;
}

bool
Pvrdma::revalidateObservation() const
{
    const auto kind = queueDma.kind;
    const uint32_t handle = queueDma.handle;
    if (!handle || handle >= pvrdma::ObjectTableEntries)
        return false;
    if (kind == pvrdma::QueueKind::Cq) {
        const auto &cq = completionQueues.entries[handle];
        return cq.valid && cq.cqHandle == handle &&
            cq.generation == queueDma.generation &&
            cq.uar == queueDma.uar && cq.cqe == queueDma.depth &&
            !cq.pages.empty() && queueDma.ringAddress ==
                cq.pages[0] + offsetof(pvrdma::RingState, rx);
    }
    const auto &qp = queuePairs.entries[handle];
    const uint32_t depth = kind == pvrdma::QueueKind::Sq ?
        qp.capabilities.maxSendWr : qp.capabilities.maxRecvWr;
    const uint64_t address = qp.pages.empty() ? 0 : qp.pages[0] +
        (kind == pvrdma::QueueKind::Sq ?
             offsetof(pvrdma::RingState, tx) :
             offsetof(pvrdma::RingState, rx));
    return qp.valid && qp.qpHandle == handle &&
        qp.generation == queueDma.generation && qp.uar == queueDma.uar &&
        depth == queueDma.depth && address == queueDma.ringAddress &&
        (kind == pvrdma::QueueKind::Sq ?
             qp.state == pvrdma::QpState::ReadyToSend :
             (qp.state == pvrdma::QpState::ReadyToReceive ||
              qp.state == pvrdma::QpState::ReadyToSend));
}

void
Pvrdma::startObservation()
{
    if (controlState != pvrdma::ControlState::Active ||
        queueDma.active() || completionBusy() || transportDmaBusy() ||
        !selectObservation())
        return;
    if (!revalidateObservation() || !queueDma.ringAddress) {
        queueStats.ringObservationsRejected++;
        finishObservation();
        return;
    }

    queueDma.ring = {};
    dmaRead(pciToDma(queueDma.ringAddress), sizeof(queueDma.ring),
            sys->isAtomicMode() ? nullptr : &queueDmaEvent,
            reinterpret_cast<uint8_t *>(&queueDma.ring));
    if (sys->isAtomicMode())
        queueDmaDone();
}

void
Pvrdma::queueDmaDone()
{
    uint32_t delta = 0;
    bool accepted = revalidateObservation();
    if (accepted && queueDma.kind == pvrdma::QueueKind::Cq) {
        auto &cq = completionQueues.entries[queueDma.handle];
        uint32_t consumer = cq.consumerHead;
        accepted = pvrdma::observeConsumer(
            queueDma.ring, cq.cqe, cq.producerTail, consumer, delta);
        if (accepted && delta) {
            sampleQueueOccupancy();
            cq.consumerHead = consumer;
            queueStats.cqReclaimed += delta;
        }
    } else if (accepted) {
        auto &qp = queuePairs.entries[queueDma.handle];
        uint32_t &stored_producer = queueDma.kind == pvrdma::QueueKind::Sq ?
            qp.sqProducerTail : qp.rqProducerTail;
        const uint32_t consumer = queueDma.kind == pvrdma::QueueKind::Sq ?
            qp.sqConsumerHead : qp.rqConsumerHead;
        uint32_t producer = stored_producer;
        accepted = pvrdma::observeProducer(
            queueDma.ring, queueDma.depth, producer, consumer, delta);
        if (accepted && delta) {
            sampleQueueOccupancy();
            stored_producer = producer;
            if (queueDma.kind == pvrdma::QueueKind::Sq)
                queueStats.sqPosted += delta;
            else
                queueStats.rqPosted += delta;
        }
    }

    if (!accepted) {
        queueStats.ringObservationsRejected++;
    } else if (delta) {
        refreshQueueGauges();
        sampleCurrentQueueOccupancy();
        checkQueueConservation();
    } else {
        checkQueueConservation();
    }
    finishObservation();
}

void
Pvrdma::finishObservation()
{
    queueDma.reset();
    scheduleObservation();
    scheduleTransport();
    if (pvrdma::checkpointStable(controlState, dmaPending(),
                                 observationQueued(), queueDma.active(),
                                 completionBusy(), transportActive(),
                                 runnableSq(), activeMr()))
        signalDrainDone();
}

void
Pvrdma::clearObservations()
{
    if (observationEvent.scheduled())
        deschedule(observationEvent);
    sqDirty = rqDirty = cqDirty = 0;
    observationCursor = 0;
    queueDma.reset();
}

void
Pvrdma::sampleQueueOccupancy()
{
    const Tick now = curTick();
    Tick remaining = now - lastQueueSample;
    lastQueueSample = now;
    while (remaining) {
        const int weight = std::min<Tick>(
            remaining, std::numeric_limits<int>::max());
        queueStats.sqOccupancy.sample(queueStats.sqOutstanding.value(),
                                      weight);
        queueStats.rqOccupancy.sample(queueStats.rqAvailable.value(),
                                      weight);
        queueStats.cqOccupancy.sample(queueStats.cqOutstanding.value(),
                                      weight);
        remaining -= weight;
    }
}

void
Pvrdma::sampleCurrentQueueOccupancy()
{
    queueStats.sqOccupancy.sample(queueStats.sqOutstanding.value(), 0);
    queueStats.rqOccupancy.sample(queueStats.rqAvailable.value(), 0);
    queueStats.cqOccupancy.sample(queueStats.cqOutstanding.value(), 0);
}

void
Pvrdma::refreshQueueGauges()
{
    uint64_t sq_depth = 0;
    uint64_t rq_depth = 0;
    uint64_t cq_depth = 0;
    uint64_t sq_occupancy = 0;
    uint64_t rq_occupancy = 0;
    uint64_t cq_occupancy = 0;
    for (uint32_t handle = 1; handle < pvrdma::ObjectTableEntries;
         ++handle) {
        const auto &qp = queuePairs.entries[handle];
        if (qp.valid) {
            sq_depth += qp.capabilities.maxSendWr;
            rq_depth += qp.capabilities.maxRecvWr;
            sq_occupancy += pvrdma::ringForwardDistance(
                qp.sqProducerTail, qp.sqConsumerHead,
                qp.capabilities.maxSendWr);
            rq_occupancy += pvrdma::ringForwardDistance(
                qp.rqProducerTail, qp.rqConsumerHead,
                qp.capabilities.maxRecvWr);
        }
        const auto &cq = completionQueues.entries[handle];
        if (cq.valid) {
            cq_depth += cq.cqe;
            cq_occupancy += pvrdma::ringForwardDistance(
                cq.producerTail, cq.consumerHead, cq.cqe);
        }
    }
    queueStats.sqDepth = sq_depth;
    queueStats.rqDepth = rq_depth;
    queueStats.cqDepth = cq_depth;
    queueStats.sqOutstanding = sq_occupancy;
    queueStats.rqAvailable = rq_occupancy;
    queueStats.cqOutstanding = cq_occupancy;
}

void
Pvrdma::queueStatsReset()
{
    refreshQueueGauges();
    queueStats.sqOutstandingAtReset = queueStats.sqOutstanding.value();
    queueStats.rqAvailableAtReset = queueStats.rqAvailable.value();
    queueStats.cqOutstandingAtReset = queueStats.cqOutstanding.value();
    lastQueueSample = curTick();
    sampleCurrentQueueOccupancy();
}

void
Pvrdma::checkQueueConservation()
{
    if (queueStats.sqOutstandingAtReset.value() +
            queueStats.sqPosted.value() !=
        queueStats.sqConsumed.value() +
            queueStats.sqResetDiscarded.value() +
            queueStats.sqOutstanding.value())
        queueStats.conservationViolations++;
    if (queueStats.rqAvailableAtReset.value() +
            queueStats.rqPosted.value() !=
        queueStats.rqConsumed.value() +
            queueStats.rqResetDiscarded.value() +
            queueStats.rqAvailable.value())
        queueStats.conservationViolations++;
    if (queueStats.cqOutstandingAtReset.value() +
            queueStats.cqPublished.value() !=
        queueStats.cqReclaimed.value() +
            queueStats.cqResetDiscarded.value() +
            queueStats.cqOutstanding.value())
        queueStats.conservationViolations++;
    if (queueStats.cqErrorPublished.value() >
        queueStats.cqPublished.value())
        queueStats.conservationViolations++;
}

namespace
{

pvrdma::transport::MacAddress
deviceMac(const pvrdma::RegisterState &regs)
{
    return {
        static_cast<uint8_t>(regs.macLow),
        static_cast<uint8_t>(regs.macLow >> 8),
        static_cast<uint8_t>(regs.macLow >> 16),
        static_cast<uint8_t>(regs.macLow >> 24),
        static_cast<uint8_t>(regs.macHigh),
        static_cast<uint8_t>(regs.macHigh >> 8),
    };
}

pvrdma::transport::MacAddress
storedMac(const pvrdma::QueuePair &qp)
{
    pvrdma::transport::MacAddress mac;
    std::copy_n(qp.attributes.addressHandle.destinationMac, mac.size(),
                mac.begin());
    return mac;
}

Tick
saturatingTicks(uint64_t value, Tick scale)
{
    return pvrdma::saturatingMultiply(value, scale, MaxTick);
}

Tick
ackDelay(uint8_t timeout)
{
    return saturatingTicks(pvrdma::ackTimeoutNanoseconds(timeout),
                           sim_clock::as_int::ns);
}

Tick
rnrDelay(uint8_t timer)
{
    return std::max<Tick>(saturatingTicks(pvrdma::RnrTimerMicros[timer],
                                          sim_clock::as_int::us), 1);
}

Tick
saturatingDeadline(Tick now, Tick delay)
{
    return pvrdma::saturatingAdd(now, delay, MaxTick);
}

} // anonymous namespace

bool
Pvrdma::Interface::recvPacket(EthPacketPtr packet)
{
    return device.recvTransportPacket(std::move(packet));
}

void
Pvrdma::Interface::sendDone()
{
    device.transportSendDone();
}

bool
Pvrdma::transportActive() const
{
    return transport.active() || transportEvent.scheduled() ||
        transportTimerEvent.scheduled() || transport.dmaBusy ||
        pendingRxPacket || pendingErrorPacket ||
        precommitCompletionAbort || precommitCompletionAbortPacket;
}

bool
Pvrdma::transportDmaBusy() const
{
    return transport.dmaBusy || transportDmaEvent.scheduled();
}

bool
Pvrdma::activeMr() const
{
    return std::any_of(memoryRegions.entries.begin(),
                       memoryRegions.entries.end(),
                       [](const auto &mr) { return mr.activeReferences; });
}

bool
Pvrdma::runnableSq() const
{
    for (uint32_t handle = 1; handle < pvrdma::ObjectTableEntries;
         ++handle) {
        const auto &qp = queuePairs.entries[handle];
        if (qp.valid && qp.state == pvrdma::QpState::ReadyToSend &&
            qp.sqProducerTail != qp.sqConsumerHead)
            return true;
    }
    return false;
}

bool
Pvrdma::revalidateTransport(bool require_cq) const
{
    if (controlState != pvrdma::ControlState::Active ||
        !transport.qpHandle ||
        transport.qpHandle >= pvrdma::ObjectTableEntries)
        return false;
    const auto &qp = queuePairs.entries[transport.qpHandle];
    if (!qp.valid || qp.qpHandle != transport.qpHandle ||
        qp.generation != transport.qpGeneration ||
        qp.qpn != transport.localQpn ||
        qp.attributes.destinationQpNumber != transport.remoteQpn ||
        storedMac(qp) != transport.remoteMac)
        return false;
    if (transport.kind == pvrdma::QueueKind::Sq) {
        if (qp.state != pvrdma::QpState::ReadyToSend ||
            qp.sendCqHandle != transport.cqHandle ||
            qp.sqConsumerHead != transport.consumer ||
            qp.attributes.sendPsn != transport.livePsn)
            return false;
    } else if (transport.kind == pvrdma::QueueKind::Rq) {
        if ((qp.state != pvrdma::QpState::ReadyToReceive &&
             qp.state != pvrdma::QpState::ReadyToSend) ||
            qp.recvCqHandle != transport.cqHandle ||
            qp.rqConsumerHead != transport.consumer ||
            qp.attributes.receivePsn != transport.livePsn)
            return false;
    } else {
        return false;
    }
    if (!require_cq)
        return true;
    if (!transport.cqHandle ||
        transport.cqHandle >= pvrdma::ObjectTableEntries)
        return false;
    const auto &cq = completionQueues.entries[transport.cqHandle];
    return cq.valid && cq.cqHandle == transport.cqHandle &&
        cq.generation == transport.cqGeneration;
}

bool
Pvrdma::revalidateTransportLease() const
{
    if (!transport.leaseHeld || !revalidateTransport())
        return false;
    const auto &lease = transport.lease;
    if (!lease.slot || lease.slot >= pvrdma::ObjectTableEntries)
        return false;
    const auto &mr = memoryRegions.entries[lease.slot];
    const auto &qp = queuePairs.entries[transport.qpHandle];
    return mr.valid && mr.generation == lease.generation &&
        mr.mrHandle == transport.wqe.lkey && mr.lkey == transport.wqe.lkey &&
        mr.pdHandle == qp.pdHandle && mr.activeReferences;
}

bool
Pvrdma::finalReceiveCommitted() const
{
    using CompletionStage = CompletionDmaState::Stage;
    if (!transport.active() || transport.kind != pvrdma::QueueKind::Rq)
        return false;
    const auto &record = completionDma.record;
    const bool completion = completionBusy() && completionDma.active() &&
        (completionDma.stage == CompletionStage::WriteCqe ||
         completionDma.stage == CompletionStage::PublishCqProducer) &&
        record.opcode == pvrdma::CompletionOpcode::Receive &&
        record.qpHandle == transport.qpHandle &&
        record.qpGeneration == transport.qpGeneration &&
        record.cqHandle == transport.cqHandle &&
        record.cqGeneration == transport.cqGeneration &&
        record.workRequestId == transport.wqe.workRequestId;
    return completion ||
        transport.stage == TransportState::Stage::WriteRqConsumer ||
        (transport.stage == TransportState::Stage::TryAck &&
         !transport.keepAfterControl);
}

bool
Pvrdma::cancelUncommittedTransportCompletion()
{
    using CompletionStage = CompletionDmaState::Stage;
    const auto &record = completionDma.record;
    if (!precommitCompletionAbort || !transport.active() ||
        transport.kind != pvrdma::QueueKind::Rq ||
        transport.stage != TransportState::Stage::WaitReceiveCq ||
        (completionDma.stage != CompletionStage::Queued &&
         completionDma.stage != CompletionStage::ReadCqRing) ||
        record.opcode != pvrdma::CompletionOpcode::Receive ||
        record.qpHandle != transport.qpHandle ||
        record.qpGeneration != transport.qpGeneration ||
        record.cqHandle != transport.cqHandle ||
        record.cqGeneration != transport.cqGeneration ||
        record.workRequestId != transport.wqe.workRequestId)
        return false;
    if (completionDma.stage == CompletionStage::Queued &&
        completionDmaEvent.scheduled())
        deschedule(completionDmaEvent);
    completionDma.reset();
    return true;
}

bool
Pvrdma::recvTransportPacket(EthPacketPtr packet)
{
    if (!packet)
        return false;
    if (packet->length > packet->bufLength)
        return true;
    const auto decoded = pvrdma::transport::decodeEthernet(
        {packet->data, packet->bufLength}, packet->length);
    if (!decoded || decoded.destination != deviceMac(regs))
        return true;

    const auto &frame = decoded.frame;
    if (finalReceiveCommitted()) {
        // Keep the first later frame for interpretation after final ACK.
        if (!pendingRxPacket)
            pendingRxPacket = std::move(packet);
        return true;
    }
    using CompletionStage = CompletionDmaState::Stage;
    const auto &record = completionDma.record;
    const bool precommit = transport.active() &&
        transport.kind == pvrdma::QueueKind::Rq &&
        transport.stage == TransportState::Stage::WaitReceiveCq &&
        (completionDma.stage == CompletionStage::Queued ||
         completionDma.stage == CompletionStage::ReadCqRing) &&
        record.opcode == pvrdma::CompletionOpcode::Receive &&
        record.qpHandle == transport.qpHandle &&
        record.qpGeneration == transport.qpGeneration &&
        record.cqHandle == transport.cqHandle &&
        record.cqGeneration == transport.cqGeneration &&
        record.workRequestId == transport.wqe.workRequestId;
    if (precommit) {
        const bool route = decoded.source == transport.remoteMac &&
            frame.sourceQpn == transport.remoteQpn &&
            frame.destinationQpn == transport.localQpn &&
            frame.messageId == transport.messageId;
        if (!route) {
            queueReverseError(frame, decoded.source);
            scheduleTransport();
            return true;
        }
        const bool duplicate = frame.kind ==
                pvrdma::transport::Kind::Data &&
            frame.totalLength == transport.totalLength &&
            frame.segmentCount == transport.segmentCount &&
            frame.segmentIndex == transport.acceptedSegmentIndex &&
            frame.psn == transport.acceptedPsn &&
            pvrdma::canonicalData(frame) &&
            (!frame.payload.size ||
             std::equal(frame.payload.data,
                        frame.payload.data + frame.payload.size,
                        transport.payload.begin() + frame.payloadOffset));
        if (duplicate)
            return true;
        const bool terminal_error =
            frame.kind == pvrdma::transport::Kind::Error &&
            (frame.psn == transport.livePsn ||
             frame.psn == transport.acceptedPsn);
        if (terminal_error ||
            frame.kind == pvrdma::transport::Kind::Data) {
            if (!precommitCompletionAbort) {
                precommitCompletionAbort = true;
                precommitCompletionAbortPacket = std::move(packet);
            }
            return true;
        }
    }
    if (frame.kind != pvrdma::transport::Kind::Data) {
        if (transport.active() && transport.kind == pvrdma::QueueKind::Rq &&
            frame.kind == pvrdma::transport::Kind::Error &&
            decoded.source == transport.remoteMac &&
            frame.sourceQpn == transport.remoteQpn &&
            frame.destinationQpn == transport.localQpn &&
            frame.messageId == transport.messageId &&
            (frame.psn == transport.livePsn ||
             frame.psn == transport.acceptedPsn)) {
            if (transport.dmaBusy)
                transport.abortAfterDma = true;
            else
                finishTransport();
            return true;
        }
        if (!transport.active() ||
            transport.kind != pvrdma::QueueKind::Sq)
            return true;
        if (decoded.source != transport.remoteMac ||
            frame.sourceQpn != transport.remoteQpn ||
            frame.destinationQpn != transport.localQpn ||
            frame.messageId != transport.messageId) {
            failSend(pvrdma::CompletionStatus::GeneralError, true, true);
            scheduleTransport();
            return true;
        }
        const uint32_t distance = pvrdma::psnDistance(
            transport.initialPsn, frame.psn);
        if (distance < transport.segmentIndex)
            return true;
        const bool response_pending =
            transport.stage == TransportState::Stage::WaitResponse ||
            (transport.stage == TransportState::Stage::TryData &&
             transport.retryPending);
        if (distance != transport.segmentIndex || !response_pending) {
            failSend(pvrdma::CompletionStatus::GeneralError, true, true);
            scheduleTransport();
            return true;
        }
        cancelTransportTimer();
        transport.retryPending = false;
        transport.packet.reset();
        auto &qp = queuePairs.entries[transport.qpHandle];
        if (!revalidateTransport(true)) {
            failSend(pvrdma::CompletionStatus::GeneralError, true, true);
        } else if (frame.kind == pvrdma::transport::Kind::Ack) {
            qp.attributes.sendPsn = pvrdma::advancePsn(
                qp.attributes.sendPsn);
            transport.livePsn = qp.attributes.sendPsn;
            if (++transport.segmentIndex < transport.segmentCount) {
                transport.psn = pvrdma::advancePsn(transport.psn);
                transport.stage = TransportState::Stage::TryData;
            } else {
                transport.status = pvrdma::CompletionStatus::Success;
                transport.stage = transport.wqe.signaled ?
                    TransportState::Stage::WaitSendCq :
                    TransportState::Stage::WriteSqConsumer;
            }
        } else if (frame.kind == pvrdma::transport::Kind::Rnr) {
            if (pvrdma::useRetry(transport.rnrRetryRemaining)) {
                transport.stage = TransportState::Stage::RetryWait;
                armTransportTimer(TransportTimerKind::Rnr,
                                  rnrDelay(frame.retryTimer));
            } else {
                failSend(pvrdma::CompletionStatus::RnrRetryExceededError,
                         true, true);
            }
        } else {
            failSend(frame.status, false, true);
        }
        scheduleTransport();
        return true;
    }

    if (transport.active() && transport.kind == pvrdma::QueueKind::Rq)
        return handleInboundContinuation(decoded);
    if (transport.active()) {
        queueReverseError(frame, decoded.source);
        scheduleTransport();
        return true;
    }
    if (replayFinal(decoded)) {
        scheduleTransport();
        return true;
    }
    if (pendingRxPacket) {
        const auto pending = pvrdma::transport::decodeEthernet(
            {pendingRxPacket->data, pendingRxPacket->bufLength},
            pendingRxPacket->length);
        if (pending && pending.source == decoded.source &&
            pending.frame.sourceQpn == frame.sourceQpn &&
            pending.frame.destinationQpn == frame.destinationQpn &&
            pending.frame.psn == frame.psn &&
            pending.frame.messageId == frame.messageId &&
            pending.frame.totalLength == frame.totalLength &&
            pending.frame.segmentIndex == frame.segmentIndex &&
            pending.frame.segmentCount == frame.segmentCount)
            return true;
        queueReverseError(frame, decoded.source);
        scheduleTransport();
        return true;
    }
    pendingRxPacket = std::move(packet);
    scheduleTransport();
    return true;
}

bool
Pvrdma::handleInboundContinuation(
    const pvrdma::transport::EthernetDecodeResult &decoded)
{
    const auto &frame = decoded.frame;
    const bool route = decoded.source == transport.remoteMac &&
        frame.sourceQpn == transport.remoteQpn &&
        frame.destinationQpn == transport.localQpn &&
        frame.messageId == transport.messageId;
    if (!route) {
        queueReverseError(frame, decoded.source);
        scheduleTransport();
        return true;
    }
    const bool canonical = frame.totalLength == transport.totalLength &&
        frame.segmentCount == transport.segmentCount &&
        pvrdma::canonicalData(frame);
    const bool in_flight = canonical &&
        transport.stage == TransportState::Stage::WriteRqPayload &&
        frame.segmentIndex == transport.segmentIndex &&
        frame.psn == transport.psn;
    if (in_flight)
        return true;
    const bool duplicate = canonical &&
        frame.segmentIndex == transport.acceptedSegmentIndex &&
        frame.psn == transport.acceptedPsn;
    if (duplicate) {
        if (frame.segmentIndex + 1 == frame.segmentCount)
            return true;
        if (transport.stage == TransportState::Stage::WaitReceiveData) {
            transport.psn = frame.psn;
            transport.keepAfterControl = true;
            prepareControl(pvrdma::transport::Kind::Ack);
        }
        return true;
    }
    const bool expected = canonical &&
        transport.stage == TransportState::Stage::WaitReceiveData &&
        frame.segmentIndex == transport.acceptedSegmentIndex + 1 &&
        frame.psn == transport.livePsn;
    if (!expected) {
        if (transport.dmaBusy) {
            queueReverseError(frame, decoded.source);
            transport.abortAfterDma = true;
        } else {
            transport.psn = frame.psn;
            transport.completionBackpressured = false;
            transport.keepAfterControl = false;
            prepareControl(pvrdma::transport::Kind::Error,
                           pvrdma::CompletionStatus::GeneralError);
        }
        return true;
    }

    transport.psn = frame.psn;
    transport.segmentIndex = frame.segmentIndex;
    if (frame.payload.size)
        std::copy(frame.payload.data, frame.payload.data + frame.payload.size,
                  transport.payload.begin() + frame.payloadOffset);
    if (!frame.payload.size) {
        auto &qp = queuePairs.entries[transport.qpHandle];
        qp.attributes.receivePsn = pvrdma::advancePsn(
            qp.attributes.receivePsn);
        transport.livePsn = qp.attributes.receivePsn;
        transport.acceptedPsn = frame.psn;
        transport.acceptedSegmentIndex = frame.segmentIndex;
        transport.stage = frame.segmentIndex + 1 == frame.segmentCount ?
            TransportState::Stage::WaitReceiveCq :
            TransportState::Stage::TryAck;
        transport.keepAfterControl =
            frame.segmentIndex + 1 != frame.segmentCount;
    } else if (!beginPayloadDma(true, frame.payloadOffset,
                                frame.payload.size)) {
        transport.keepAfterControl = false;
        prepareControl(pvrdma::transport::Kind::Error,
                       pvrdma::CompletionStatus::GeneralError);
    }
    scheduleTransport();
    return true;
}

bool
Pvrdma::replayFinal(
    const pvrdma::transport::EthernetDecodeResult &decoded)
{
    const auto &frame = decoded.frame;
    auto *qp = pvrdma::findQueuePair(queuePairs, frame.destinationQpn);
    if (!qp || !pvrdma::canonicalData(frame) ||
        frame.segmentIndex + 1 != frame.segmentCount)
        return false;
    const auto &replay = qp->finalReplay;
    if (!replay.valid || replay.qpGeneration != qp->generation ||
        replay.localMac != decoded.destination ||
        replay.remoteMac != decoded.source ||
        replay.localQpn != frame.destinationQpn ||
        replay.remoteQpn != frame.sourceQpn || replay.finalPsn != frame.psn ||
        replay.messageId != frame.messageId ||
        replay.totalLength != frame.totalLength ||
        replay.segmentIndex != frame.segmentIndex ||
        replay.segmentCount != frame.segmentCount)
        return false;

    transport.reset();
    transport.kind = pvrdma::QueueKind::Rq;
    transport.qpHandle = qp->qpHandle;
    transport.qpGeneration = qp->generation;
    transport.cqHandle = qp->recvCqHandle;
    transport.cqGeneration = completionQueues.entries[
        qp->recvCqHandle].generation;
    transport.consumer = qp->rqConsumerHead;
    transport.localMac = deviceMac(regs);
    transport.remoteMac = decoded.source;
    transport.localQpn = qp->qpn;
    transport.remoteQpn = frame.sourceQpn;
    transport.psn = transport.acceptedPsn = frame.psn;
    transport.livePsn = qp->attributes.receivePsn;
    transport.messageId = frame.messageId;
    transport.totalLength = frame.totalLength;
    transport.segmentIndex = transport.acceptedSegmentIndex =
        frame.segmentIndex;
    transport.segmentCount = frame.segmentCount;
    transport.responseKind = pvrdma::transport::Kind::Ack;
    transport.stage = TransportState::Stage::TryAck;
    return true;
}

void
Pvrdma::transportSendDone()
{
    scheduleTransport();
}

void
Pvrdma::scheduleTransport()
{
    if (transportPaused || controlState != pvrdma::ControlState::Active ||
        transportDmaBusy() || queueDma.active() || completionBusy() ||
        dmaPending() || observationQueued() ||
        (transport.completionBackpressured && !pendingErrorPacket) ||
        transportEvent.scheduled())
        return;
    if (transport.active() || pendingRxPacket || pendingErrorPacket ||
        runnableSq())
        schedule(transportEvent, nextCycle());
}

bool
Pvrdma::selectSend()
{
    // ponytail: one device-wide flight; add per-QP scheduling only when
    // concurrent transport is required.
    const uint32_t first = observationCursor % pvrdma::ObjectTableEntries;
    for (uint32_t i = 0; i < pvrdma::ObjectTableEntries; ++i) {
        const uint32_t handle = (first + i) % pvrdma::ObjectTableEntries;
        if (!handle)
            continue;
        auto &qp = queuePairs.entries[handle];
        if (!qp.valid || qp.state != pvrdma::QpState::ReadyToSend ||
            qp.sqProducerTail == qp.sqConsumerHead)
            continue;
        transport.reset();
        transport.kind = pvrdma::QueueKind::Sq;
        transport.stage = TransportState::Stage::ReadSqWqe;
        transport.qpHandle = handle;
        transport.qpGeneration = qp.generation;
        transport.cqHandle = qp.sendCqHandle;
        transport.cqGeneration = completionQueues.entries[
            qp.sendCqHandle].generation;
        transport.consumer = qp.sqConsumerHead;
        transport.nextConsumer = pvrdma::ringAdvance(
            transport.consumer, qp.capabilities.maxSendWr);
        transport.consumerLe = htole(transport.nextConsumer);
        transport.localMac = deviceMac(regs);
        transport.remoteMac = storedMac(qp);
        transport.localQpn = qp.qpn;
        transport.remoteQpn = qp.attributes.destinationQpNumber;
        transport.psn = transport.initialPsn = qp.attributes.sendPsn;
        transport.livePsn = transport.psn;
        transport.messageId = (uint64_t{qp.qpn} << 32) |
            static_cast<uint32_t>(qp.sqConsumerHead + 1);
        transport.retryRemaining = qp.attributes.retryCount;
        transport.rnrRetryRemaining = qp.attributes.rnrRetry;
        observationCursor = (handle + 1) % pvrdma::ObjectTableEntries;
        if (!pvrdma::wqeAddress(qp, pvrdma::QueueKind::Sq,
                                transport.consumer,
                                transport.wqeAddress)) {
            failSend(pvrdma::CompletionStatus::GeneralError, true, true);
            return true;
        }
        startTransportDma(false, transport.wqeAddress,
                          transport.sqSlot.size(), transport.sqSlot.data());
        return true;
    }
    return false;
}

void
Pvrdma::queueReverseError(
    const pvrdma::transport::Frame &received,
    const pvrdma::transport::MacAddress &source_mac)
{
    if (pendingErrorPacket)
        return;
    pvrdma::transport::Frame error;
    error.kind = pvrdma::transport::Kind::Error;
    error.status = pvrdma::CompletionStatus::GeneralError;
    error.sourceQpn = received.destinationQpn;
    error.destinationQpn = received.sourceQpn;
    error.psn = received.psn;
    error.messageId = received.messageId;
    const size_t size = pvrdma::transport::EthernetHeaderSize +
        pvrdma::transport::HeaderSize;
    pendingErrorPacket = std::make_shared<EthPacketData>(size);
    const auto encoded = pvrdma::transport::encodeEthernet(
        error, deviceMac(regs), source_mac,
        {pendingErrorPacket->data, pendingErrorPacket->bufLength});
    if (!encoded) {
        pendingErrorPacket.reset();
        return;
    }
    pendingErrorPacket->length = encoded.size;
    pendingErrorPacket->simLength = encoded.size;
}

void
Pvrdma::startInbound()
{
    if (!pendingRxPacket || transport.active())
        return;
    const EthPacketPtr packet = std::move(pendingRxPacket);
    const auto decoded = pvrdma::transport::decodeEthernet(
        {packet->data, packet->bufLength}, packet->length);
    if (!decoded || decoded.destination != deviceMac(regs))
        return;
    if (replayFinal(decoded)) {
        scheduleTransport();
        return;
    }
    if (decoded.frame.kind != pvrdma::transport::Kind::Data)
        return;
    const auto &frame = decoded.frame;
    if (!pvrdma::canonicalData(frame) || frame.segmentIndex != 0) {
        queueReverseError(frame, decoded.source);
        scheduleTransport();
        return;
    }
    auto *qp = pvrdma::findQueuePair(queuePairs, frame.destinationQpn);
    if (!qp) {
        queueReverseError(frame, decoded.source);
        scheduleTransport();
        return;
    }

    transport.reset();
    transport.kind = pvrdma::QueueKind::Rq;
    transport.qpHandle = qp->qpHandle;
    transport.qpGeneration = qp->generation;
    transport.cqHandle = qp->recvCqHandle;
    transport.cqGeneration = completionQueues.entries[
        qp->recvCqHandle].generation;
    transport.consumer = qp->rqConsumerHead;
    transport.nextConsumer = pvrdma::ringAdvance(
        transport.consumer, qp->capabilities.maxRecvWr);
    transport.consumerLe = htole(transport.nextConsumer);
    transport.localMac = deviceMac(regs);
    transport.remoteMac = decoded.source;
    transport.localQpn = qp->qpn;
    transport.remoteQpn = frame.sourceQpn;
    transport.psn = transport.acceptedPsn = frame.psn;
    transport.livePsn = qp->attributes.receivePsn;
    transport.messageId = frame.messageId;
    transport.totalLength = frame.totalLength;
    transport.segmentIndex = transport.acceptedSegmentIndex =
        frame.segmentIndex;
    transport.segmentCount = frame.segmentCount;

    const bool route_ok =
        (qp->state == pvrdma::QpState::ReadyToReceive ||
         qp->state == pvrdma::QpState::ReadyToSend) &&
        qp->attributes.destinationQpNumber == frame.sourceQpn &&
        storedMac(*qp) == decoded.source && frame.psn == transport.livePsn;
    if (!route_ok) {
        prepareControl(pvrdma::transport::Kind::Error,
                       pvrdma::CompletionStatus::GeneralError);
        return;
    }
    if (qp->rqProducerTail == qp->rqConsumerHead) {
        prepareControl(pvrdma::transport::Kind::Rnr);
        return;
    }
    transport.payload.resize(frame.totalLength);
    if (frame.payload.size)
        std::copy(frame.payload.data, frame.payload.data + frame.payload.size,
                  transport.payload.begin() + frame.payloadOffset);
    transport.stage = TransportState::Stage::ReadRqWqe;
    if (!pvrdma::wqeAddress(*qp, pvrdma::QueueKind::Rq,
                            transport.consumer, transport.wqeAddress)) {
        prepareControl(pvrdma::transport::Kind::Error,
                       pvrdma::CompletionStatus::GeneralError);
        return;
    }
    startTransportDma(false, transport.wqeAddress,
                      transport.rqSlot.size(), transport.rqSlot.data());
}

void
Pvrdma::startTransportDma(bool write, uint64_t address, size_t size,
                          uint8_t *data)
{
    panic_if(transport.dmaBusy || dmaPending(),
             "PVRDMA overlapping transport DMA");
    transport.dmaBusy = true;
    if (write) {
        dmaWrite(pciToDma(address), size,
                 sys->isAtomicMode() ? nullptr : &transportDmaEvent, data);
    } else {
        dmaRead(pciToDma(address), size,
                sys->isAtomicMode() ? nullptr : &transportDmaEvent, data);
    }
    if (sys->isAtomicMode())
        transportDmaDone();
}

bool
Pvrdma::beginPayloadDma(bool write, size_t offset, size_t length)
{
    if (!revalidateTransportLease() ||
        !pvrdma::leaseRange(transport.lease, offset, length,
                            transport.chunkIndex, transport.chunkOffset))
        return false;
    transport.dmaPayloadOffset = offset;
    transport.dmaRemaining = length;
    if (!length)
        return true;
    if (write)
        ++receivePayloadDmaStarts;
    transport.stage = write ? TransportState::Stage::WriteRqPayload :
                              TransportState::Stage::ReadSqPayload;
    startPayloadDma(write);
    return true;
}

void
Pvrdma::startPayloadDma(bool write)
{
    if (!revalidateTransportLease() || !transport.dmaRemaining ||
        transport.chunkIndex >= transport.lease.chunks.size()) {
        if (transport.kind == pvrdma::QueueKind::Sq)
            failSend(pvrdma::CompletionStatus::GeneralError, true, true);
        else
            prepareControl(pvrdma::transport::Kind::Error,
                           pvrdma::CompletionStatus::GeneralError);
        scheduleTransport();
        return;
    }
    const auto &chunk = transport.lease.chunks[transport.chunkIndex];
    transport.dmaChunkLength = std::min(
        transport.dmaRemaining, chunk.length - transport.chunkOffset);
    startTransportDma(write, chunk.address + transport.chunkOffset,
                      transport.dmaChunkLength,
                      transport.payload.data() + transport.dmaPayloadOffset);
}

void
Pvrdma::failSend(pvrdma::CompletionStatus status, bool notify_remote,
                 bool qp_error)
{
    cancelTransportTimer();
    transport.status = status;
    transport.packet.reset();
    transport.completionBackpressured = false;
    transport.keepAfterControl = false;
    transport.retryPending = false;
    transport.terminalQpError = qp_error;
    transport.stage = notify_remote ? TransportState::Stage::TryError :
                                      TransportState::Stage::WaitSendCq;
    transport.responseKind = pvrdma::transport::Kind::Error;
}

void
Pvrdma::prepareControl(pvrdma::transport::Kind kind,
                       pvrdma::CompletionStatus status)
{
    transport.responseKind = kind;
    transport.status = status;
    transport.packet.reset();
    transport.stage = kind == pvrdma::transport::Kind::Ack ?
        TransportState::Stage::TryAck :
        kind == pvrdma::transport::Kind::Rnr ?
            TransportState::Stage::TryRnr :
            TransportState::Stage::TryError;
    scheduleTransport();
}

bool
Pvrdma::tryTransportPacket()
{
    using namespace pvrdma::transport;
    const bool data = transport.stage == TransportState::Stage::TryData;
    if (data && !revalidateTransportLease()) {
        failSend(pvrdma::CompletionStatus::GeneralError, true, true);
        scheduleTransport();
        return false;
    }
    if ((transport.stage == TransportState::Stage::TryAck ||
         transport.stage == TransportState::Stage::TryError) &&
        transport.leaseHeld && !revalidateTransportLease()) {
        finishTransport();
        return false;
    }
    if (transport.stage == TransportState::Stage::TryRnr &&
        !revalidateTransport()) {
        finishTransport();
        return false;
    }
    if (!transport.packet) {
        Frame frame;
        frame.kind = data ? Kind::Data : transport.responseKind;
        frame.sourceQpn = transport.localQpn;
        frame.destinationQpn = transport.remoteQpn;
        frame.psn = transport.psn;
        frame.messageId = transport.messageId;
        if (frame.kind == Kind::Data) {
            const size_t offset = size_t{transport.segmentIndex} *
                pvrdma::FixedMtu;
            const size_t length = transport.totalLength == 0 ? 0 :
                std::min<size_t>(pvrdma::FixedMtu,
                                 transport.totalLength - offset);
            frame.flags = (transport.segmentIndex == 0 ? First : 0) |
                (transport.segmentIndex + 1 == transport.segmentCount ?
                     Last : 0);
            frame.totalLength = transport.totalLength;
            frame.payloadOffset = offset;
            frame.segmentIndex = transport.segmentIndex;
            frame.segmentCount = transport.segmentCount;
            frame.payload = {length ? transport.payload.data() + offset :
                                      nullptr,
                             length};
        } else if (frame.kind == Kind::Error) {
            frame.status = transport.status;
        } else if (frame.kind == Kind::Rnr) {
            const auto &qp = queuePairs.entries[transport.qpHandle];
            frame.retryTimer = qp.attributes.minRnrTimer;
        }
        const size_t size = EthernetHeaderSize + HeaderSize +
            frame.payload.size;
        transport.packet = std::make_shared<EthPacketData>(size);
        const auto encoded = encodeEthernet(
            frame, transport.localMac, transport.remoteMac,
            {transport.packet->data, transport.packet->bufLength});
        if (!encoded) {
            if (data)
                failSend(pvrdma::CompletionStatus::GeneralError, false, true);
            else if (transport.kind == pvrdma::QueueKind::Sq &&
                     transport.terminalQpError)
                transport.stage = TransportState::Stage::WaitSendCq;
            else
                finishTransport();
            scheduleTransport();
            return false;
        }
        transport.packet->length = encoded.size;
        transport.packet->simLength = encoded.size;
    }
    if (!interface.getPeer()) {
        if (data)
            failSend(pvrdma::CompletionStatus::GeneralError, false, true);
        else if (transport.kind == pvrdma::QueueKind::Sq &&
                 transport.terminalQpError)
            transport.stage = TransportState::Stage::WaitSendCq;
        else
            finishTransport();
        scheduleTransport();
        return false;
    }
    if (!interface.sendPacket(transport.packet))
        return false;
    transport.packet.reset();
    if (data) {
        transport.retryPending = false;
        transport.stage = TransportState::Stage::WaitResponse;
        const auto &qp = queuePairs.entries[transport.qpHandle];
        const Tick delay = ackDelay(qp.attributes.timeout);
        if (delay)
            armTransportTimer(TransportTimerKind::Ack, delay);
    } else if (transport.kind == pvrdma::QueueKind::Sq &&
               transport.terminalQpError) {
        transport.stage = TransportState::Stage::WaitSendCq;
    } else if (transport.keepAfterControl) {
        transport.keepAfterControl = false;
        transport.stage = TransportState::Stage::WaitReceiveData;
    } else {
        finishTransport();
    }
    return true;
}

void
Pvrdma::submitTransportCompletion()
{
    if (!revalidateTransport(true)) {
        if (transport.kind == pvrdma::QueueKind::Sq)
            finishTransport();
        else
            prepareControl(pvrdma::transport::Kind::Error,
                           pvrdma::CompletionStatus::GeneralError);
        return;
    }
    pvrdma::CompletionRecord record;
    record.cqHandle = transport.cqHandle;
    record.qpHandle = transport.qpHandle;
    record.cqGeneration = transport.cqGeneration;
    record.qpGeneration = transport.qpGeneration;
    record.workRequestId = transport.wqe.workRequestId;
    record.opcode = transport.kind == pvrdma::QueueKind::Sq ?
        pvrdma::CompletionOpcode::Send : pvrdma::CompletionOpcode::Receive;
    record.status = transport.status;
    if (transport.kind == pvrdma::QueueKind::Rq) {
        record.byteLength = transport.payload.size();
        record.sourceQp = transport.remoteQpn;
    }
    const auto result = submitCompletion(record, [this](auto completed) {
        transportCompletionDone(completed);
    });
    if (result == pvrdma::CompletionSubmitResult::Busy)
        scheduleObservation();
    else if (result == pvrdma::CompletionSubmitResult::Rejected)
        transportCompletionDone(result);
}

void
Pvrdma::transportCompletionDone(pvrdma::CompletionSubmitResult result)
{
    if (!transport.active())
        return;
    const auto expected = transport.kind == pvrdma::QueueKind::Sq ?
        TransportState::Stage::WaitSendCq :
        TransportState::Stage::WaitReceiveCq;
    if (transport.stage != expected)
        return;
    if (result == pvrdma::CompletionSubmitResult::Backpressured) {
        transport.completionBackpressured = true;
        return;
    }
    if (result == pvrdma::CompletionSubmitResult::Rejected) {
        if (revalidateTransport(true)) {
            transport.completionBackpressured = true;
        } else {
            finishTransport();
        }
        return;
    }
    if (result != pvrdma::CompletionSubmitResult::Published)
        return;
    if (transport.kind == pvrdma::QueueKind::Sq) {
        transport.stage = TransportState::Stage::WriteSqConsumer;
    } else {
        panic_if(!revalidateTransport(true),
                 "PVRDMA receive completion lost its QP");
        transport.stage = TransportState::Stage::WriteRqConsumer;
    }
    scheduleTransport();
}

void
Pvrdma::publishConsumer(pvrdma::QueueKind kind)
{
    if (!revalidateTransport() ||
        (transport.leaseHeld && !revalidateTransportLease())) {
        if (kind == pvrdma::QueueKind::Sq)
            finishTransport();
        else
            prepareControl(pvrdma::transport::Kind::Error,
                           pvrdma::CompletionStatus::GeneralError);
        return;
    }
    uint64_t address = 0;
    if (!pvrdma::queueConsumerAddress(
            queuePairs.entries[transport.qpHandle], kind, address)) {
        if (kind == pvrdma::QueueKind::Sq)
            failSend();
        else
            prepareControl(pvrdma::transport::Kind::Error,
                           pvrdma::CompletionStatus::GeneralError);
        return;
    }
    startTransportDma(true, address, sizeof(transport.consumerLe),
                      reinterpret_cast<uint8_t *>(&transport.consumerLe));
}

void
Pvrdma::finishTransport(bool release_lease)
{
    cancelTransportTimer();
    if (release_lease && transport.leaseHeld) {
        if (!pvrdma::releaseMr(memoryRegions, transport.lease))
            operationError.set(regs.error, pvrdma::CommandError);
        transport.leaseHeld = false;
    }
    transport.reset();
    precommitCompletionAbort = false;
    precommitCompletionAbortPacket.reset();
    scheduleObservation();
    scheduleTransport();
    if (pvrdma::checkpointStable(controlState, dmaPending(),
                                 observationQueued(), queueDma.active(),
                                 completionBusy(), transportActive(),
                                 runnableSq(), activeMr()))
        signalDrainDone();
}

void
Pvrdma::clearTransport()
{
    if (transportEvent.scheduled())
        deschedule(transportEvent);
    cancelTransportTimer();
    panic_if(transport.dmaBusy || transport.leaseHeld,
             "PVRDMA cleared active transport resources");
    transport.reset();
    pendingRxPacket.reset();
    pendingErrorPacket.reset();
    precommitCompletionAbortPacket.reset();
    precommitCompletionAbort = false;
}

void
Pvrdma::armTransportTimer(TransportTimerKind kind, Tick delay)
{
    cancelTransportTimer();
    transportTimerKind = kind;
    schedule(transportTimerEvent, saturatingDeadline(curTick(), delay));
}

void
Pvrdma::cancelTransportTimer()
{
    if (transportTimerEvent.scheduled())
        deschedule(transportTimerEvent);
    transportTimerKind = TransportTimerKind::None;
}

void
Pvrdma::transportTimerExpired()
{
    const auto kind = transportTimerKind;
    transportTimerKind = TransportTimerKind::None;
    if (!transport.active() || transport.kind != pvrdma::QueueKind::Sq)
        return;
    if (kind == TransportTimerKind::Rnr &&
        transport.stage == TransportState::Stage::RetryWait) {
        transport.retryPending = true;
        transport.stage = TransportState::Stage::TryData;
    } else if (kind == TransportTimerKind::Ack &&
               transport.stage == TransportState::Stage::WaitResponse) {
        if (pvrdma::useRetry(transport.retryRemaining)) {
            transport.retryPending = true;
            transport.stage = TransportState::Stage::TryData;
        } else
            failSend(pvrdma::CompletionStatus::RetryExceededError,
                     true, true);
    }
    scheduleTransport();
}

void
Pvrdma::runTransport()
{
    if (transportPaused || controlState != pvrdma::ControlState::Active ||
        transportDmaBusy() || queueDma.active() || completionBusy() ||
        dmaPending() || observationQueued() ||
        (transport.completionBackpressured && !pendingErrorPacket))
        return;
    if (pendingErrorPacket) {
        if (interface.getPeer() && interface.sendPacket(pendingErrorPacket))
            pendingErrorPacket.reset();
        return;
    }
    if (!transport.active()) {
        if (pendingRxPacket)
            startInbound();
        else
            selectSend();
        return;
    }

    switch (transport.stage) {
      case TransportState::Stage::TryData:
      case TransportState::Stage::TryAck:
      case TransportState::Stage::TryRnr:
      case TransportState::Stage::TryError:
        tryTransportPacket();
        return;
      case TransportState::Stage::WaitSendCq:
      case TransportState::Stage::WaitReceiveCq:
        submitTransportCompletion();
        return;
      case TransportState::Stage::WriteSqConsumer:
        publishConsumer(pvrdma::QueueKind::Sq);
        return;
      case TransportState::Stage::WriteRqConsumer:
        publishConsumer(pvrdma::QueueKind::Rq);
        return;
      case TransportState::Stage::WaitResponse:
      case TransportState::Stage::RetryWait:
      case TransportState::Stage::WaitReceiveData:
      case TransportState::Stage::ReadSqWqe:
      case TransportState::Stage::ReadSqPayload:
      case TransportState::Stage::ReadRqWqe:
      case TransportState::Stage::WriteRqPayload:
      case TransportState::Stage::Idle:
        return;
    }
}

void
Pvrdma::transportDmaDone()
{
    panic_if(!transport.dmaBusy, "Unexpected PVRDMA transport DMA callback");
    transport.dmaBusy = false;
    if (transport.abortAfterDma) {
        finishTransport();
        return;
    }
    using Stage = TransportState::Stage;
    const auto receive_segment_done = [this] {
        auto &qp = queuePairs.entries[transport.qpHandle];
        qp.attributes.receivePsn = pvrdma::advancePsn(
            qp.attributes.receivePsn);
        transport.livePsn = qp.attributes.receivePsn;
        transport.acceptedPsn = transport.psn;
        transport.acceptedSegmentIndex = transport.segmentIndex;
        if (transport.segmentIndex + 1 == transport.segmentCount) {
            transport.stage = Stage::WaitReceiveCq;
        } else {
            transport.keepAfterControl = true;
            prepareControl(pvrdma::transport::Kind::Ack);
        }
    };

    switch (transport.stage) {
      case Stage::ReadSqWqe: {
        if (!revalidateTransport(true)) {
            finishTransport();
            return;
        }
        pvrdma::SendWqeHeader header{};
        std::memcpy(&header, transport.sqSlot.data(), sizeof(header));
        transport.wqe.workRequestId = letoh(header.workRequestId);
        auto &qp = queuePairs.entries[transport.qpHandle];
        if (!pvrdma::decodeSqWqe(qp, transport.sqSlot, transport.wqe) ||
            !pvrdma::segmentGeometry(transport.wqe.length,
                                     transport.segmentCount) ||
            !pvrdma::acquireLocalMr(
                memoryRegions, qp, pvrdma::QueueKind::Sq,
                transport.wqe.lkey, transport.wqe.address,
                transport.wqe.length, transport.lease)) {
            failSend(pvrdma::CompletionStatus::GeneralError, true, true);
            break;
        }
        transport.leaseHeld = true;
        transport.totalLength = transport.wqe.length;
        transport.payload.resize(transport.totalLength);
        if (!transport.totalLength) {
            transport.stage = Stage::TryData;
            break;
        }
        if (!beginPayloadDma(false, 0, transport.totalLength))
            failSend(pvrdma::CompletionStatus::GeneralError, true, true);
        else
            return;
        break;
      }
      case Stage::ReadSqPayload:
        if (!revalidateTransportLease()) {
            failSend(pvrdma::CompletionStatus::GeneralError, true, true);
            break;
        }
        transport.dmaPayloadOffset += transport.dmaChunkLength;
        transport.dmaRemaining -= transport.dmaChunkLength;
        transport.chunkOffset += transport.dmaChunkLength;
        if (transport.chunkOffset ==
            transport.lease.chunks[transport.chunkIndex].length) {
            ++transport.chunkIndex;
            transport.chunkOffset = 0;
        }
        if (transport.dmaRemaining) {
            startPayloadDma(false);
            return;
        }
        transport.stage = Stage::TryData;
        break;
      case Stage::ReadRqWqe: {
        if (!revalidateTransport(true)) {
            prepareControl(pvrdma::transport::Kind::Error,
                           pvrdma::CompletionStatus::GeneralError);
            break;
        }
        auto &qp = queuePairs.entries[transport.qpHandle];
        if (!pvrdma::decodeRqWqe(qp, transport.rqSlot, transport.wqe) ||
            transport.wqe.length < transport.totalLength ||
            !pvrdma::acquireLocalMr(
                memoryRegions, qp, pvrdma::QueueKind::Rq,
                transport.wqe.lkey, transport.wqe.address,
                transport.totalLength, transport.lease)) {
            prepareControl(pvrdma::transport::Kind::Error,
                           pvrdma::CompletionStatus::GeneralError);
            break;
        }
        transport.leaseHeld = true;
        const size_t length = transport.totalLength == 0 ? 0 :
            std::min<size_t>(pvrdma::FixedMtu, transport.totalLength);
        if (!length) {
            receive_segment_done();
            break;
        }
        if (!beginPayloadDma(true, 0, length)) {
            prepareControl(pvrdma::transport::Kind::Error,
                           pvrdma::CompletionStatus::GeneralError);
            break;
        }
        return;
      }
      case Stage::WriteRqPayload:
        if (!revalidateTransportLease()) {
            prepareControl(pvrdma::transport::Kind::Error,
                           pvrdma::CompletionStatus::GeneralError);
            break;
        }
        transport.dmaPayloadOffset += transport.dmaChunkLength;
        transport.dmaRemaining -= transport.dmaChunkLength;
        transport.chunkOffset += transport.dmaChunkLength;
        if (transport.chunkOffset ==
            transport.lease.chunks[transport.chunkIndex].length) {
            ++transport.chunkIndex;
            transport.chunkOffset = 0;
        }
        if (transport.dmaRemaining) {
            startPayloadDma(true);
            return;
        }
        receive_segment_done();
        break;
      case Stage::WriteSqConsumer: {
        panic_if(!revalidateTransport() ||
                     (transport.leaseHeld && !revalidateTransportLease()),
                 "PVRDMA SQ changed during consumer publication");
        sampleQueueOccupancy();
        auto &qp = queuePairs.entries[transport.qpHandle];
        qp.sqConsumerHead = transport.nextConsumer;
        transport.consumer = transport.nextConsumer;
        queueStats.sqConsumed++;
        if (transport.terminalQpError) {
            qp.state = pvrdma::QpState::Error;
            qp.attributes.qpState = qp.attributes.currentQpState = qp.state;
        }
        refreshQueueGauges();
        sampleCurrentQueueOccupancy();
        checkQueueConservation();
        finishTransport();
        return;
      }
      case Stage::WriteRqConsumer: {
        panic_if(!revalidateTransportLease(),
                 "PVRDMA RQ changed during consumer publication");
        sampleQueueOccupancy();
        auto &qp = queuePairs.entries[transport.qpHandle];
        qp.rqConsumerHead = transport.nextConsumer;
        transport.consumer = transport.nextConsumer;
        queueStats.rqConsumed++;
        qp.finalReplay = {true, qp.generation, transport.localMac,
                          transport.remoteMac, transport.localQpn,
                          transport.remoteQpn,
                          transport.acceptedPsn, transport.messageId,
                          transport.totalLength,
                          transport.acceptedSegmentIndex,
                          transport.segmentCount};
        refreshQueueGauges();
        sampleCurrentQueueOccupancy();
        checkQueueConservation();
        transport.keepAfterControl = false;
        prepareControl(pvrdma::transport::Kind::Ack);
        break;
      }
      default:
        panic("Invalid PVRDMA transport DMA stage");
    }
    scheduleTransport();
}

pvrdma::CompletionSubmitResult
Pvrdma::submitCompletion(
    const pvrdma::CompletionRecord &record,
    std::function<void(pvrdma::CompletionSubmitResult)> done)
{
    if (completionBusy() || observationQueued() || queueDma.active() ||
        dmaPending())
        return pvrdma::CompletionSubmitResult::Busy;
    if (controlState != pvrdma::ControlState::Active ||
        !pvrdma::validCompletionRecord(record)) {
        queueStats.cqPublicationRejected++;
        return pvrdma::CompletionSubmitResult::Rejected;
    }

    completionDma.reset();
    completionDma.record = record;
    completionDma.done = std::move(done);
    completionDma.stage = CompletionDmaState::Stage::Queued;
    if (!revalidateCompletion()) {
        completionDma.reset();
        queueStats.cqPublicationRejected++;
        return pvrdma::CompletionSubmitResult::Rejected;
    }
    schedule(completionDmaEvent, nextCycle());
    return pvrdma::CompletionSubmitResult::Queued;
}

bool
Pvrdma::revalidateCompletion() const
{
    const auto &record = completionDma.record;
    if (controlState != pvrdma::ControlState::Active ||
        !pvrdma::validCompletionRecord(record))
        return false;
    const auto &cq = completionQueues.entries[record.cqHandle];
    const auto &qp = queuePairs.entries[record.qpHandle];
    if (!cq.valid || cq.cqHandle != record.cqHandle ||
        cq.generation != record.cqGeneration || !qp.valid ||
        qp.qpHandle != record.qpHandle ||
        qp.generation != record.qpGeneration || cq.pages.empty())
        return false;
    if (record.opcode == pvrdma::CompletionOpcode::Send)
        return qp.sendCqHandle == record.cqHandle &&
            qp.state == pvrdma::QpState::ReadyToSend;
    return qp.recvCqHandle == record.cqHandle &&
        (qp.state == pvrdma::QpState::ReadyToReceive ||
         qp.state == pvrdma::QpState::ReadyToSend);
}

void
Pvrdma::startCompletion()
{
    if (!completionDma.queued())
        return;
    if (cancelUncommittedTransportCompletion()) {
        precommitCompletionAbort = false;
        auto packet = std::move(precommitCompletionAbortPacket);
        recvTransportPacket(std::move(packet));
        return;
    }
    if (!revalidateCompletion()) {
        rejectCompletion(false);
        return;
    }
    const auto &cq = completionQueues.entries[completionDma.record.cqHandle];
    completionDma.stage = CompletionDmaState::Stage::ReadCqRing;
    completionDma.ring = {};
    dmaRead(pciToDma(cq.pages[0] + offsetof(pvrdma::RingState, rx)),
            sizeof(completionDma.ring),
            sys->isAtomicMode() ? nullptr : &completionDmaEvent,
            reinterpret_cast<uint8_t *>(&completionDma.ring));
    if (sys->isAtomicMode())
        completionDmaDone();
}

void
Pvrdma::rejectCompletion(bool backpressure)
{
    if (backpressure)
        queueStats.cqPublicationBackpressured++;
    else
        queueStats.cqPublicationRejected++;
    auto done = std::move(completionDma.done);
    completionDma.reset();
    scheduleObservation();
    checkQueueConservation();
    if (done)
        done(backpressure ? pvrdma::CompletionSubmitResult::Backpressured :
                            pvrdma::CompletionSubmitResult::Rejected);
    scheduleTransport();
    if (pvrdma::checkpointStable(controlState, dmaPending(),
                                 observationQueued(), queueDma.active(),
                                 completionBusy(), transportActive(),
                                 runnableSq(), activeMr()))
        signalDrainDone();
}

void
Pvrdma::finishCompletion()
{
    auto done = std::move(completionDma.done);
    completionDma.reset();
    scheduleObservation();
    checkQueueConservation();
    if (done)
        done(pvrdma::CompletionSubmitResult::Published);
    scheduleTransport();
    if (pvrdma::checkpointStable(controlState, dmaPending(),
                                 observationQueued(), queueDma.active(),
                                 completionBusy(), transportActive(),
                                 runnableSq(), activeMr()))
        signalDrainDone();
}

void
Pvrdma::completionDmaDone()
{
    using Stage = CompletionDmaState::Stage;
    if (completionDma.stage == Stage::ReadCqRing &&
        cancelUncommittedTransportCompletion()) {
        precommitCompletionAbort = false;
        auto packet = std::move(precommitCompletionAbortPacket);
        recvTransportPacket(std::move(packet));
        return;
    }
    if (!revalidateCompletion()) {
        rejectCompletion(false);
        return;
    }

    auto &cq = completionQueues.entries[completionDma.record.cqHandle];
    switch (completionDma.stage) {
      case Stage::ReadCqRing: {
        uint32_t consumer = cq.consumerHead;
        uint32_t reclaimed = 0;
        if (!pvrdma::observeConsumer(completionDma.ring, cq.cqe,
                                     cq.producerTail, consumer,
                                     reclaimed)) {
            rejectCompletion(false);
            return;
        }
        if (reclaimed) {
            sampleQueueOccupancy();
            cq.consumerHead = consumer;
            queueStats.cqReclaimed += reclaimed;
            refreshQueueGauges();
            sampleCurrentQueueOccupancy();
            checkQueueConservation();
        }
        uint32_t slot = 0;
        if (!pvrdma::ringHasSpace(cq.producerTail, cq.consumerHead,
                                  cq.cqe, slot)) {
            rejectCompletion(true);
            return;
        }
        completionDma.cqSlot = slot;
        completionDma.cqNextProducer = pvrdma::ringAdvance(
            cq.producerTail, cq.cqe);
        completionDma.cqProducerLe = htole(
            completionDma.cqNextProducer);
        completionDma.cqe = pvrdma::encodeCompletion(completionDma.record);
        completionDma.stage = Stage::WriteCqe;
        const uint32_t offset = completionDma.cqSlot * pvrdma::CqeSize;
        dmaWrite(pciToDma(cq.pages[1 + offset / pvrdma::PageSize] +
                          offset % pvrdma::PageSize),
                 sizeof(completionDma.cqe),
                 sys->isAtomicMode() ? nullptr : &completionDmaEvent,
                 reinterpret_cast<uint8_t *>(&completionDma.cqe));
        if (sys->isAtomicMode())
            completionDmaDone();
        return;
      }
      case Stage::WriteCqe:
        completionDma.stage = Stage::PublishCqProducer;
        dmaWrite(pciToDma(cq.pages[0] +
                          offsetof(pvrdma::RingState, rx) +
                          offsetof(pvrdma::Ring, producerTail)),
                 sizeof(completionDma.cqProducerLe),
                 sys->isAtomicMode() ? nullptr : &completionDmaEvent,
                 reinterpret_cast<uint8_t *>(
                     &completionDma.cqProducerLe));
        if (sys->isAtomicMode())
            completionDmaDone();
        return;
      case Stage::PublishCqProducer:
        sampleQueueOccupancy();
        cq.producerTail = completionDma.cqNextProducer;
        queueStats.cqPublished++;
        if (completionDma.record.status !=
            pvrdma::CompletionStatus::Success)
            queueStats.cqErrorPublished++;
        refreshQueueGauges();
        sampleCurrentQueueOccupancy();
        checkQueueConservation();
        finishCompletion();
        return;
      default:
        panic("Invalid PVRDMA completion DMA stage");
    }
}

void
Pvrdma::startDsr()
{
    if (!pvrdma::beginDsr(controlState) || !regs.dsrAddress ||
        (regs.dsrAddress % pvrdma::UarPageSize) != 0) {
        controlState = pvrdma::ControlState::Unconfigured;
        operationError.set(regs.error, pvrdma::CommandError);
        return;
    }

    operationError.begin(regs.error);
    capabilities = pvrdma::makeCapabilities(regs.macLow, regs.macHigh);
    dsrDmaAddress = pciToDma(regs.dsrAddress);
    dmaRead(dsrDmaAddress, sizeof(dsr),
            sys->isAtomicMode() ? nullptr : &dsrReadEvent,
            reinterpret_cast<uint8_t *>(&dsr));
    if (sys->isAtomicMode())
        dsrReadDone();
}

void
Pvrdma::dsrReadDone()
{
    if (!pvrdma::validSharedRegion(dsr, regs.dsrAddress)) {
        pvrdma::finishDsrRead(controlState, false);
        commandSlotAddress = responseSlotAddress = 0;
        commandSlotDmaAddress = responseSlotDmaAddress = 0;
        operationError.complete(regs.error, pvrdma::CommandError);
        operationDone();
        return;
    }

    commandSlotAddress = letoh(dsr.commandSlotDma);
    responseSlotAddress = letoh(dsr.responseSlotDma);
    commandSlotDmaAddress = pciToDma(commandSlotAddress);
    responseSlotDmaAddress = pciToDma(responseSlotAddress);
    panic_if(!pvrdma::finishDsrRead(controlState, true),
             "PVRDMA completed DSR read in invalid state");
    dmaWrite(dsrDmaAddress + offsetof(pvrdma::DeviceSharedRegion, caps),
             sizeof(capabilities),
             sys->isAtomicMode() ? nullptr : &capsWriteEvent,
             reinterpret_cast<uint8_t *>(&capabilities));
    if (sys->isAtomicMode())
        capsWriteDone();
}

void
Pvrdma::capsWriteDone()
{
    panic_if(!pvrdma::finishCapsWrite(controlState),
             "PVRDMA completed capability write in invalid state");
    operationError.complete(regs.error, 0);
    operationDone();
}

void
Pvrdma::writeControl(uint32_t value)
{
    regs.control = value;
    if (value > static_cast<uint32_t>(pvrdma::DeviceControl::Reset)) {
        operationError.set(regs.error, pvrdma::CommandError);
        return;
    }

    const auto control = static_cast<pvrdma::DeviceControl>(value);
    if (control == pvrdma::DeviceControl::Reset) {
        if (!pvrdma::stable(controlState) || queueDma.active() ||
            completionBusy() || transportActive()) {
            operationError.set(regs.error, pvrdma::CommandError);
            return;
        }
        resetDevice();
        return;
    }

    if (!pvrdma::applyControl(controlState, control)) {
        operationError.set(regs.error, pvrdma::CommandError);
        return;
    }
    operationError.set(regs.error, 0);
}

void
Pvrdma::startCommand(uint32_t value)
{
    regs.request = value;
    if (value != 0 || commandBlockedByObservation() ||
        !pvrdma::beginCommand(controlState)) {
        operationError.set(regs.error, pvrdma::CommandError);
        return;
    }

    operationError.begin(regs.error);
    dmaRead(commandSlotDmaAddress, sizeof(command),
            sys->isAtomicMode() ? nullptr : &commandReadEvent,
            reinterpret_cast<uint8_t *>(&command));
    if (sys->isAtomicMode())
        commandReadDone();
}

void
Pvrdma::commandReadDone()
{
    const uint32_t command_id = letoh(command.header.command);
    uint64_t qp_busy = sqDirty | rqDirty;
    uint64_t cq_busy = cqDirty;
    if (queueDma.active()) {
        const uint64_t bit = uint64_t{1} << queueDma.handle;
        if (queueDma.kind == pvrdma::QueueKind::Cq)
            cq_busy |= bit;
        else
            qp_busy |= bit;
    }
    if (completionBusy()) {
        cq_busy |= uint64_t{1} << completionDma.record.cqHandle;
        qp_busy |= uint64_t{1} << completionDma.record.qpHandle;
    }
    if (transport.active() && transport.qpHandle) {
        qp_busy |= uint64_t{1} << transport.qpHandle;
        if (transport.cqHandle)
            cq_busy |= uint64_t{1} << transport.cqHandle;
    }
    const bool target_busy = pvrdma::queueCommandTargetBusy(
        command, qp_busy, cq_busy);
    if (target_busy) {
        response = {};
        const bool has_response = command_id ==
            static_cast<uint32_t>(pvrdma::Command::ModifyQp);
        if (has_response) {
            pvrdma::detail::setResponseHeader(
                response.header, command.header, command_id,
                pvrdma::CommandError);
        }
        operationError.complete(regs.error, pvrdma::CommandError);
        panic_if(!pvrdma::finishCommandRead(controlState, has_response),
                 "PVRDMA rejected queue command in invalid state");
        if (!has_response) {
            operationDone();
            return;
        }
        dmaWrite(responseSlotDmaAddress, sizeof(response),
                 sys->isAtomicMode() ? nullptr : &responseWriteEvent,
                 reinterpret_cast<uint8_t *>(&response));
        if (sys->isAtomicMode())
            responseWriteDone();
        return;
    }
    if (command_id == static_cast<uint32_t>(pvrdma::Command::CreateMr) ||
        command_id == static_cast<uint32_t>(pvrdma::Command::CreateCq) ||
        command_id == static_cast<uint32_t>(pvrdma::Command::CreateQp)) {
        startObjectCreate(command_id ==
            static_cast<uint32_t>(pvrdma::Command::CreateMr) ?
                pvrdma::PendingCreateKind::Mr :
            command_id == static_cast<uint32_t>(pvrdma::Command::CreateCq) ?
                pvrdma::PendingCreateKind::Cq :
                pvrdma::PendingCreateKind::Qp);
        return;
    }

    bool lifecycle = false;
    uint32_t lifecycle_handle = 0;
    uint64_t sq_discarded = 0;
    uint64_t rq_discarded = 0;
    uint64_t cq_discarded = 0;
    if (command_id == static_cast<uint32_t>(pvrdma::Command::DestroyQp)) {
        lifecycle = true;
        lifecycle_handle = letoh(command.destroyQp.qpHandle);
    } else if (command_id ==
                   static_cast<uint32_t>(pvrdma::Command::DestroyCq)) {
        lifecycle = true;
        lifecycle_handle = letoh(command.destroyCq.cqHandle);
    } else if (command_id ==
                   static_cast<uint32_t>(pvrdma::Command::ModifyQp) &&
               letoh(static_cast<uint32_t>(
                   command.modifyQp.attributes.qpState)) ==
                   static_cast<uint32_t>(pvrdma::QpState::Reset)) {
        lifecycle = true;
        lifecycle_handle = letoh(command.modifyQp.qpHandle);
    }
    if (lifecycle) {
        sampleQueueOccupancy();
        if (lifecycle_handle < pvrdma::ObjectTableEntries) {
            if (command_id ==
                    static_cast<uint32_t>(pvrdma::Command::DestroyCq)) {
                const auto &cq = completionQueues.entries[lifecycle_handle];
                if (cq.valid)
                    cq_discarded = pvrdma::ringForwardDistance(
                        cq.producerTail, cq.consumerHead, cq.cqe);
            } else {
                const auto &qp = queuePairs.entries[lifecycle_handle];
                if (qp.valid) {
                    sq_discarded = pvrdma::ringForwardDistance(
                        qp.sqProducerTail, qp.sqConsumerHead,
                        qp.capabilities.maxSendWr);
                    rq_discarded = pvrdma::ringForwardDistance(
                        qp.rqProducerTail, qp.rqConsumerHead,
                        qp.capabilities.maxRecvWr);
                }
            }
        }
    }

    const auto result = pvrdma::processCommand(
        command, response, gids, gidValid, objects, memoryRegions,
        completionQueues, queuePairs,
        {BARs[pvrdma::UarBar]->addr(), BARs[pvrdma::UarBar]->size()});
    operationError.complete(regs.error, result.error);
    if (!result.error) {
        if (lifecycle) {
            queueStats.sqResetDiscarded += sq_discarded;
            queueStats.rqResetDiscarded += rq_discarded;
            queueStats.cqResetDiscarded += cq_discarded;
            const uint64_t bit = uint64_t{1} << lifecycle_handle;
            if (command_id ==
                    static_cast<uint32_t>(pvrdma::Command::DestroyCq)) {
                cqDirty &= ~bit;
            } else {
                sqDirty &= ~bit;
                rqDirty &= ~bit;
            }
        }
        refreshQueueGauges();
        if (lifecycle) {
            sampleCurrentQueueOccupancy();
            checkQueueConservation();
        }
    }
    panic_if(!pvrdma::finishCommandRead(controlState, result.hasResponse),
             "PVRDMA completed command read in invalid state");
    if (!result.hasResponse) {
        operationDone();
        return;
    }

    dmaWrite(responseSlotDmaAddress, sizeof(response),
             sys->isAtomicMode() ? nullptr : &responseWriteEvent,
             reinterpret_cast<uint8_t *>(&response));
    if (sys->isAtomicMode())
        responseWriteDone();
}

void
Pvrdma::startObjectCreate(pvrdma::PendingCreateKind kind)
{
    response = {};
    pendingCreate = kind;
    mrBuild = {};
    cqBuild = {};
    qpBuild = {};
    bool valid = false;
    switch (kind) {
      case pvrdma::PendingCreateKind::Mr:
        valid = pvrdma::detail::prepareCreateMr(
            command, objects, memoryRegions, mrBuild);
        break;
      case pvrdma::PendingCreateKind::Cq:
        valid = pvrdma::detail::prepareCreateCq(
            command, objects, completionQueues, cqBuild);
        break;
      case pvrdma::PendingCreateKind::Qp:
        valid = pvrdma::detail::prepareCreateQp(
            command, objects, completionQueues, queuePairs, qpBuild);
        break;
      case pvrdma::PendingCreateKind::None:
        break;
    }
    if (!valid) {
        pvrdma::detail::setCreateResponseHeader(
            response.header, command.header, letoh(command.header.command),
            false);
        operationError.complete(regs.error, pvrdma::CommandError);
        panic_if(!pvrdma::finishCommandRead(controlState, true),
                 "PVRDMA rejected object create in invalid state");
        pendingCreate = pvrdma::PendingCreateKind::None;
        mrBuild = {};
        cqBuild = {};
        qpBuild = {};
        objectDirectory = {};
        objectTable = {};
        objectTables.clear();
        objectTableIndex = 0;
        dmaWrite(responseSlotDmaAddress, sizeof(response),
                 sys->isAtomicMode() ? nullptr : &responseWriteEvent,
                 reinterpret_cast<uint8_t *>(&response));
        if (sys->isAtomicMode())
            responseWriteDone();
        return;
    }

    panic_if(!pvrdma::beginObjectDirectory(controlState),
             "PVRDMA began object directory read in invalid state");
    objectDirectory = {};
    auto &build = pendingPageBuild();
    dmaRead(pciToDma(build.pageDirectoryDma), sizeof(objectDirectory),
            sys->isAtomicMode() ? nullptr : &objectDirectoryReadEvent,
            reinterpret_cast<uint8_t *>(objectDirectory.data()));
    if (sys->isAtomicMode())
        objectDirectoryReadDone();
}

pvrdma::PageDirectoryBuild &
Pvrdma::pendingPageBuild()
{
    switch (pendingCreate) {
      case pvrdma::PendingCreateKind::Mr: return mrBuild;
      case pvrdma::PendingCreateKind::Cq: return cqBuild;
      case pvrdma::PendingCreateKind::Qp: return qpBuild;
      case pvrdma::PendingCreateKind::None:
        panic("PVRDMA has no pending page-backed create");
    }
    panic("Invalid PVRDMA pending create kind");
}

void
Pvrdma::objectDirectoryReadDone()
{
    if (!pvrdma::detail::consumePageDirectory(
            pendingPageBuild(), objectDirectory, objectTables)) {
        finishObjectCreate(false);
        return;
    }
    panic_if(!pvrdma::beginObjectTables(controlState),
             "PVRDMA completed object directory read in invalid state");
    objectTableIndex = 0;
    startObjectTableRead();
}

void
Pvrdma::startObjectTableRead()
{
    panic_if(controlState != pvrdma::ControlState::ReadingObjectTable ||
                 objectTableIndex >= objectTables.size(),
             "PVRDMA started invalid object table read");
    objectTable = {};
    dmaRead(pciToDma(objectTables[objectTableIndex]), sizeof(objectTable),
            sys->isAtomicMode() ? nullptr : &objectTableReadEvent,
            reinterpret_cast<uint8_t *>(objectTable.data()));
    if (sys->isAtomicMode())
        objectTableReadDone();
}

void
Pvrdma::objectTableReadDone()
{
    if (!pvrdma::detail::consumePageTable(
            pendingPageBuild(), objectTableIndex, objectTable)) {
        finishObjectCreate(false);
        return;
    }
    if (++objectTableIndex < objectTables.size()) {
        startObjectTableRead();
        return;
    }
    finishObjectCreate(true);
}

void
Pvrdma::finishObjectCreate(bool success)
{
    const auto kind = pendingCreate;
    if (success) {
        switch (kind) {
          case pvrdma::PendingCreateKind::Mr:
            success = memoryRegions.commit(std::move(mrBuild), objects);
            break;
          case pvrdma::PendingCreateKind::Cq:
            success = completionQueues.commit(std::move(cqBuild), objects);
            break;
          case pvrdma::PendingCreateKind::Qp:
            success = queuePairs.commit(std::move(qpBuild), objects,
                                        completionQueues);
            break;
          case pvrdma::PendingCreateKind::None:
            success = false;
            break;
        }
    }

    pvrdma::detail::setCreateResponseHeader(
        response.header, command.header, letoh(command.header.command),
        success);
    if (success && kind == pvrdma::PendingCreateKind::Mr) {
        const auto &mr = memoryRegions.entries[
            mrBuild.mrHandle & pvrdma::MrSlotMask];
        response.createMr.mrHandle = htole(mr.mrHandle);
        response.createMr.lkey = htole(mr.lkey);
        response.createMr.rkey = htole(mr.rkey);
    } else if (success && kind == pvrdma::PendingCreateKind::Cq) {
        const auto &cq = completionQueues.entries[cqBuild.slot];
        response.createCq.cqHandle = htole(cq.cqHandle);
        response.createCq.cqe = htole(cq.cqe);
    } else if (success && kind == pvrdma::PendingCreateKind::Qp) {
        const auto &qp = queuePairs.entries[qpBuild.slot];
        response.createQpV2.qpn = htole(qp.qpn);
        response.createQpV2.qpHandle = htole(qp.qpHandle);
        response.createQpV2.maxSendWr = htole(qp.capabilities.maxSendWr);
        response.createQpV2.maxRecvWr = htole(qp.capabilities.maxRecvWr);
        response.createQpV2.maxSendSge = htole(qp.capabilities.maxSendSge);
        response.createQpV2.maxRecvSge = htole(qp.capabilities.maxRecvSge);
        response.createQpV2.maxInlineData = 0;
    }
    operationError.complete(regs.error,
                            success ? 0 : pvrdma::CommandError);
    if (success)
        refreshQueueGauges();
    panic_if(!pvrdma::finishObjectWalk(controlState),
             "PVRDMA finished object walk in invalid state");
    pendingCreate = pvrdma::PendingCreateKind::None;
    mrBuild = {};
    cqBuild = {};
    qpBuild = {};
    objectDirectory = {};
    objectTable = {};
    objectTables.clear();
    objectTableIndex = 0;
    dmaWrite(responseSlotDmaAddress, sizeof(response),
             sys->isAtomicMode() ? nullptr : &responseWriteEvent,
             reinterpret_cast<uint8_t *>(&response));
    if (sys->isAtomicMode())
        responseWriteDone();
}

void
Pvrdma::responseWriteDone()
{
    panic_if(!pvrdma::finishResponseWrite(controlState,
                                           regs.pendingCauses),
             "PVRDMA completed response write in invalid state");
    updateInterrupt();
    operationDone();
}

void
Pvrdma::resetDevice()
{
    sampleQueueOccupancy();
    queueStats.sqResetDiscarded += queueStats.sqOutstanding.value();
    queueStats.rqResetDiscarded += queueStats.rqAvailable.value();
    queueStats.cqResetDiscarded += queueStats.cqOutstanding.value();
    clearObservations();
    clearTransport();
    if (completionDmaEvent.scheduled())
        deschedule(completionDmaEvent);
    completionDma.reset();
    regs.reset();
    regs.control = static_cast<uint32_t>(pvrdma::DeviceControl::Reset);
    regs.error = 0;
    controlState = pvrdma::ControlState::Unconfigured;
    commandSlotAddress = responseSlotAddress = 0;
    dsrDmaAddress = commandSlotDmaAddress = responseSlotDmaAddress = 0;
    dsr = {};
    command = {};
    response = {};
    gids = {};
    gidValid = {};
    objects.reset();
    memoryRegions.reset();
    completionQueues.reset();
    queuePairs.reset();
    pendingCreate = pvrdma::PendingCreateKind::None;
    mrBuild = {};
    cqBuild = {};
    qpBuild = {};
    objectDirectory = {};
    objectTable = {};
    objectTables.clear();
    objectTableIndex = 0;
    operationError.reset();
    transportPaused = false;
    refreshQueueGauges();
    sampleCurrentQueueOccupancy();
    checkQueueConservation();
    if (intxAsserted) {
        intrClear();
        intxAsserted = false;
    }
}

void
Pvrdma::updateInterrupt()
{
    const bool asserted = pvrdma::interruptPending(
        regs.pendingCauses, regs.interruptMask);
    if (asserted == intxAsserted)
        return;

    if (asserted)
        intrPost();
    else
        intrClear();
    intxAsserted = asserted;
}

void
Pvrdma::operationDone()
{
    scheduleObservation();
    scheduleTransport();
    if (pvrdma::checkpointStable(controlState, dmaPending(),
                                 observationQueued(), queueDma.active(),
                                 completionBusy(), transportActive(),
                                 runnableSq(), activeMr()))
        signalDrainDone();
}

DrainState
Pvrdma::drain()
{
    scheduleObservation();
    scheduleTransport();
    return pvrdma::checkpointStable(controlState, dmaPending(),
                                    observationQueued(), queueDma.active(),
                                    completionBusy(), transportActive(),
                                    runnableSq(), activeMr()) ?
        DrainState::Drained : DrainState::Draining;
}

void
Pvrdma::serialize(CheckpointOut &cp) const
{
    panic_if(!pvrdma::checkpointStable(controlState, dmaPending(),
                                        observationQueued(),
                                        queueDma.active(), completionBusy(),
                                        transportActive(), runnableSq(),
                                        activeMr()),
             "Cannot checkpoint PVRDMA with active transport or SEND");
    PciDevice::serialize(cp);
    SERIALIZE_SCALAR(regs.dsrAddress);
    SERIALIZE_SCALAR(regs.control);
    SERIALIZE_SCALAR(regs.request);
    SERIALIZE_SCALAR(regs.error);
    SERIALIZE_SCALAR(regs.pendingCauses);
    SERIALIZE_SCALAR(regs.interruptMask);
    SERIALIZE_SCALAR(regs.macLow);
    SERIALIZE_SCALAR(regs.macHigh);
    SERIALIZE_SCALAR(regs.dsrLowPending);
    SERIALIZE_ENUM(controlState);
    SERIALIZE_SCALAR(commandSlotAddress);
    SERIALIZE_SCALAR(responseSlotAddress);
    arrayParamOut(cp, "capabilities",
                  reinterpret_cast<const uint8_t *>(&capabilities),
                  sizeof(capabilities));
    arrayParamOut(cp, "gids", reinterpret_cast<const uint8_t *>(gids.data()),
                  sizeof(gids));
    arrayParamOut(cp, "gidValid", gidValid.data(), gidValid.size());
    arrayParamOut(cp, "contextUar", objects.contextUar.data(),
                  objects.contextUar.size());
    arrayParamOut(cp, "contextPdChildren",
                  objects.contextPdChildren.data(),
                  objects.contextPdChildren.size());
    arrayParamOut(cp, "contextCqChildren",
                  objects.contextCqChildren.data(),
                  objects.contextCqChildren.size());
    arrayParamOut(cp, "pdAllocated", objects.pdAllocated.data(),
                  objects.pdAllocated.size());
    arrayParamOut(cp, "pdParent", objects.pdParent.data(),
                  objects.pdParent.size());
    arrayParamOut(cp, "pdChildren", objects.pdChildren.data(),
                  objects.pdChildren.size());
    SERIALIZE_SCALAR(intxAsserted);
    for (uint32_t slot = 1; slot < pvrdma::ObjectTableEntries; ++slot) {
        const auto &mr = memoryRegions.entries[slot];
        ScopedCheckpointSection sec(cp, csprintf("mr%u", slot));
        paramOut(cp, "valid", mr.valid);
        paramOut(cp, "generation", mr.generation);
        if (!mr.valid)
            continue;
        paramOut(cp, "mrHandle", mr.mrHandle);
        paramOut(cp, "lkey", mr.lkey);
        paramOut(cp, "rkey", mr.rkey);
        paramOut(cp, "pdHandle", mr.pdHandle);
        paramOut(cp, "accessFlags", mr.accessFlags);
        paramOut(cp, "start", mr.start);
        paramOut(cp, "length", mr.length);
        paramOut(cp, "end", mr.end);
        arrayParamOut(cp, "pages", mr.pages);
    }
    for (uint32_t slot = 1; slot < pvrdma::ObjectTableEntries; ++slot) {
        const auto &cq = completionQueues.entries[slot];
        ScopedCheckpointSection sec(cp, csprintf("cq%u", slot));
        paramOut(cp, "valid", cq.valid);
        paramOut(cp, "generation", cq.generation);
        if (!cq.valid)
            continue;
        paramOut(cp, "cqHandle", cq.cqHandle);
        paramOut(cp, "contextHandle", cq.contextHandle);
        paramOut(cp, "uar", cq.uar);
        paramOut(cp, "cqe", cq.cqe);
        paramOut(cp, "qpReferences", cq.qpReferences);
        paramOut(cp, "armFlags", cq.armFlags);
        paramOut(cp, "producerTail", cq.producerTail);
        paramOut(cp, "consumerHead", cq.consumerHead);
        arrayParamOut(cp, "pages", cq.pages);
    }
    for (uint32_t slot = 1; slot < pvrdma::ObjectTableEntries; ++slot) {
        const auto &qp = queuePairs.entries[slot];
        ScopedCheckpointSection sec(cp, csprintf("qp%u", slot));
        paramOut(cp, "valid", qp.valid);
        paramOut(cp, "generation", qp.generation);
        if (!qp.valid)
            continue;
        paramOut(cp, "qpHandle", qp.qpHandle);
        paramOut(cp, "qpn", qp.qpn);
        paramOut(cp, "pdHandle", qp.pdHandle);
        paramOut(cp, "sendCqHandle", qp.sendCqHandle);
        paramOut(cp, "recvCqHandle", qp.recvCqHandle);
        paramOut(cp, "contextHandle", qp.contextHandle);
        paramOut(cp, "uar", qp.uar);
        paramOut(cp, "sendChunks", qp.sendChunks);
        paramOut(cp, "recvChunks", qp.recvChunks);
        paramOut(cp, "totalChunks", qp.totalChunks);
        paramOut(cp, "signalAllSendWr", qp.signalAllSendWr);
        paramOut(cp, "state", static_cast<uint32_t>(qp.state));
        arrayParamOut(cp, "capabilities",
                      reinterpret_cast<const uint8_t *>(&qp.capabilities),
                      sizeof(qp.capabilities));
        arrayParamOut(cp, "attributes",
                      reinterpret_cast<const uint8_t *>(&qp.attributes),
                      sizeof(qp.attributes));
        paramOut(cp, "sqProducerTail", qp.sqProducerTail);
        paramOut(cp, "sqConsumerHead", qp.sqConsumerHead);
        paramOut(cp, "rqProducerTail", qp.rqProducerTail);
        paramOut(cp, "rqConsumerHead", qp.rqConsumerHead);
        paramOut(cp, "replayValid", qp.finalReplay.valid);
        paramOut(cp, "replayQpGeneration", qp.finalReplay.qpGeneration);
        arrayParamOut(cp, "replayLocalMac", qp.finalReplay.localMac.data(),
                      qp.finalReplay.localMac.size());
        arrayParamOut(cp, "replayRemoteMac", qp.finalReplay.remoteMac.data(),
                      qp.finalReplay.remoteMac.size());
        paramOut(cp, "replayLocalQpn", qp.finalReplay.localQpn);
        paramOut(cp, "replayRemoteQpn", qp.finalReplay.remoteQpn);
        paramOut(cp, "replayFinalPsn", qp.finalReplay.finalPsn);
        paramOut(cp, "replayMessageId", qp.finalReplay.messageId);
        paramOut(cp, "replayTotalLength", qp.finalReplay.totalLength);
        paramOut(cp, "replaySegmentIndex", qp.finalReplay.segmentIndex);
        paramOut(cp, "replaySegmentCount", qp.finalReplay.segmentCount);
        arrayParamOut(cp, "pages", qp.pages);
    }
}

void
Pvrdma::unserialize(CheckpointIn &cp)
{
    PciDevice::unserialize(cp);
    UNSERIALIZE_SCALAR(regs.dsrAddress);
    UNSERIALIZE_SCALAR(regs.control);
    UNSERIALIZE_SCALAR(regs.request);
    UNSERIALIZE_SCALAR(regs.error);
    UNSERIALIZE_SCALAR(regs.pendingCauses);
    UNSERIALIZE_SCALAR(regs.interruptMask);
    UNSERIALIZE_SCALAR(regs.macLow);
    UNSERIALIZE_SCALAR(regs.macHigh);
    UNSERIALIZE_SCALAR(regs.dsrLowPending);
    UNSERIALIZE_ENUM(controlState);
    UNSERIALIZE_SCALAR(commandSlotAddress);
    UNSERIALIZE_SCALAR(responseSlotAddress);
    arrayParamIn(cp, "capabilities",
                 reinterpret_cast<uint8_t *>(&capabilities),
                 sizeof(capabilities));
    arrayParamIn(cp, "gids", reinterpret_cast<uint8_t *>(gids.data()),
                 sizeof(gids));
    arrayParamIn(cp, "gidValid", gidValid.data(), gidValid.size());
    arrayParamIn(cp, "contextUar", objects.contextUar.data(),
                 objects.contextUar.size());
    arrayParamIn(cp, "contextPdChildren",
                 objects.contextPdChildren.data(),
                 objects.contextPdChildren.size());
    arrayParamIn(cp, "contextCqChildren",
                 objects.contextCqChildren.data(),
                 objects.contextCqChildren.size());
    arrayParamIn(cp, "pdAllocated", objects.pdAllocated.data(),
                 objects.pdAllocated.size());
    arrayParamIn(cp, "pdParent", objects.pdParent.data(),
                 objects.pdParent.size());
    arrayParamIn(cp, "pdChildren", objects.pdChildren.data(),
                 objects.pdChildren.size());
    UNSERIALIZE_SCALAR(intxAsserted);
    memoryRegions.reset();
    for (uint32_t slot = 1; slot < pvrdma::ObjectTableEntries; ++slot) {
        auto &mr = memoryRegions.entries[slot];
        ScopedCheckpointSection sec(cp, csprintf("mr%u", slot));
        paramIn(cp, "valid", mr.valid);
        paramIn(cp, "generation", mr.generation);
        if (!mr.valid)
            continue;
        paramIn(cp, "mrHandle", mr.mrHandle);
        paramIn(cp, "lkey", mr.lkey);
        paramIn(cp, "rkey", mr.rkey);
        paramIn(cp, "pdHandle", mr.pdHandle);
        paramIn(cp, "accessFlags", mr.accessFlags);
        mr.activeReferences = 0;
        paramIn(cp, "start", mr.start);
        paramIn(cp, "length", mr.length);
        paramIn(cp, "end", mr.end);
        arrayParamIn(cp, "pages", mr.pages);
    }
    completionQueues.reset();
    for (uint32_t slot = 1; slot < pvrdma::ObjectTableEntries; ++slot) {
        auto &cq = completionQueues.entries[slot];
        ScopedCheckpointSection sec(cp, csprintf("cq%u", slot));
        paramIn(cp, "valid", cq.valid);
        paramIn(cp, "generation", cq.generation);
        if (!cq.valid)
            continue;
        paramIn(cp, "cqHandle", cq.cqHandle);
        paramIn(cp, "contextHandle", cq.contextHandle);
        paramIn(cp, "uar", cq.uar);
        paramIn(cp, "cqe", cq.cqe);
        paramIn(cp, "qpReferences", cq.qpReferences);
        paramIn(cp, "armFlags", cq.armFlags);
        paramIn(cp, "producerTail", cq.producerTail);
        paramIn(cp, "consumerHead", cq.consumerHead);
        arrayParamIn(cp, "pages", cq.pages);
    }
    queuePairs.reset();
    for (uint32_t slot = 1; slot < pvrdma::ObjectTableEntries; ++slot) {
        auto &qp = queuePairs.entries[slot];
        ScopedCheckpointSection sec(cp, csprintf("qp%u", slot));
        paramIn(cp, "valid", qp.valid);
        paramIn(cp, "generation", qp.generation);
        if (!qp.valid)
            continue;
        paramIn(cp, "qpHandle", qp.qpHandle);
        paramIn(cp, "qpn", qp.qpn);
        paramIn(cp, "pdHandle", qp.pdHandle);
        paramIn(cp, "sendCqHandle", qp.sendCqHandle);
        paramIn(cp, "recvCqHandle", qp.recvCqHandle);
        paramIn(cp, "contextHandle", qp.contextHandle);
        paramIn(cp, "uar", qp.uar);
        paramIn(cp, "sendChunks", qp.sendChunks);
        paramIn(cp, "recvChunks", qp.recvChunks);
        paramIn(cp, "totalChunks", qp.totalChunks);
        paramIn(cp, "signalAllSendWr", qp.signalAllSendWr);
        uint32_t state = 0;
        paramIn(cp, "state", state);
        qp.state = static_cast<pvrdma::QpState>(state);
        arrayParamIn(cp, "capabilities",
                     reinterpret_cast<uint8_t *>(&qp.capabilities),
                     sizeof(qp.capabilities));
        arrayParamIn(cp, "attributes",
                     reinterpret_cast<uint8_t *>(&qp.attributes),
                     sizeof(qp.attributes));
        paramIn(cp, "sqProducerTail", qp.sqProducerTail);
        paramIn(cp, "sqConsumerHead", qp.sqConsumerHead);
        paramIn(cp, "rqProducerTail", qp.rqProducerTail);
        paramIn(cp, "rqConsumerHead", qp.rqConsumerHead);
        paramIn(cp, "replayValid", qp.finalReplay.valid);
        paramIn(cp, "replayQpGeneration", qp.finalReplay.qpGeneration);
        arrayParamIn(cp, "replayLocalMac", qp.finalReplay.localMac.data(),
                     qp.finalReplay.localMac.size());
        arrayParamIn(cp, "replayRemoteMac", qp.finalReplay.remoteMac.data(),
                     qp.finalReplay.remoteMac.size());
        paramIn(cp, "replayLocalQpn", qp.finalReplay.localQpn);
        paramIn(cp, "replayRemoteQpn", qp.finalReplay.remoteQpn);
        paramIn(cp, "replayFinalPsn", qp.finalReplay.finalPsn);
        paramIn(cp, "replayMessageId", qp.finalReplay.messageId);
        paramIn(cp, "replayTotalLength", qp.finalReplay.totalLength);
        paramIn(cp, "replaySegmentIndex", qp.finalReplay.segmentIndex);
        paramIn(cp, "replaySegmentCount", qp.finalReplay.segmentCount);
        arrayParamIn(cp, "pages", qp.pages);
    }
    operationError.reset();
    transportPaused = false;

    panic_if(!pvrdma::stable(controlState),
             "PVRDMA checkpoint contains transient control state");
    panic_if(pvrdma::interruptPending(
                 regs.pendingCauses, regs.interruptMask) != intxAsserted,
             "PVRDMA checkpoint has inconsistent interrupt state");
    panic_if(!pvrdma::validObjectTables(
                 objects, {BARs[pvrdma::UarBar]->addr(),
                           BARs[pvrdma::UarBar]->size()}) ||
                 !pvrdma::validQueueObjects(completionQueues, queuePairs,
                                            memoryRegions, objects),
             "PVRDMA checkpoint has inconsistent object tables");
    clearObservations();
    clearTransport();
    completionDma.reset();
    pendingCreate = pvrdma::PendingCreateKind::None;
    mrBuild = {};
    cqBuild = {};
    qpBuild = {};
    objectDirectory = {};
    objectTable = {};
    objectTables.clear();
    objectTableIndex = 0;
    if (controlState == pvrdma::ControlState::Unconfigured) {
        dsrDmaAddress = commandSlotDmaAddress = responseSlotDmaAddress = 0;
    } else {
        panic_if(!regs.dsrAddress || !commandSlotAddress ||
                     !responseSlotAddress,
                 "Configured PVRDMA checkpoint has missing DMA address");
        dsrDmaAddress = pciToDma(regs.dsrAddress);
        commandSlotDmaAddress = pciToDma(commandSlotAddress);
        responseSlotDmaAddress = pciToDma(responseSlotAddress);
    }
    queueStatsReset();
    // The interrupt controller restores its own state. Reposting or clearing
    // the legacy line here can disturb another device sharing that line.
}

} // namespace gem5
