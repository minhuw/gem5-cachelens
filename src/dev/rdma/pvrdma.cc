// SPDX-License-Identifier: BSD-3-Clause

#include "dev/rdma/pvrdma.hh"

#include <cstring>

#include "base/logging.hh"
#include "mem/packet_access.hh"
#include "sim/core.hh"
#include "sim/serialize.hh"

namespace gem5
{

Pvrdma::TransportStats::TransportStats(Pvrdma &parent)
    : statistics::Group(&parent, "transport"), device(parent),
      ADD_STAT(txDataFramesAccepted, statistics::units::Count::get(),
               "DATA frames accepted by the downstream Ethernet peer"),
      ADD_STAT(txDataEncodedBytesAccepted, statistics::units::Byte::get(),
               "Encoded DATA bytes accepted for Ethernet serialization"),
      ADD_STAT(txDataPayloadBytesAccepted, statistics::units::Byte::get(),
               "DATA payload bytes accepted, including retries"),
      ADD_STAT(txNewDataSegmentsAccepted, statistics::units::Count::get(),
               "First accepted transmissions of logical DATA segments"),
      ADD_STAT(txNewDataPayloadBytesAccepted,
               statistics::units::Byte::get(),
               "Payload bytes in first accepted DATA transmissions"),
      ADD_STAT(txRetransmittedSegmentsAccepted,
               statistics::units::Count::get(),
               "Accepted retransmissions of logical DATA segments"),
      ADD_STAT(txRetransmittedPayloadBytesAccepted,
               statistics::units::Byte::get(),
               "Payload bytes in accepted retransmissions"),
      ADD_STAT(txControlFramesAccepted, statistics::units::Count::get(),
               "ACK, NAK, and RNR frames accepted downstream"),
      ADD_STAT(txControlEncodedBytesAccepted,
               statistics::units::Byte::get(),
               "Encoded control bytes accepted for serialization"),
      ADD_STAT(txDataSerializationTicks, statistics::units::Tick::get(),
               "Observed downstream DATA serialization busy ticks"),
      ADD_STAT(txControlSerializationTicks, statistics::units::Tick::get(),
               "Observed downstream control serialization busy ticks"),
      ADD_STAT(txSegmentsInFlight, statistics::units::Count::get(),
               "Live unique sent DATA segments not cumulatively retired"),
      ADD_STAT(txSegmentsInFlightAtReset, statistics::units::Count::get(),
               "Live sent DATA segments at statistics reset"),
      ADD_STAT(txSegmentsAcknowledged, statistics::units::Count::get(),
               "Unique DATA segments cumulatively acknowledged"),
      ADD_STAT(txSegmentsAbandoned, statistics::units::Count::get(),
               "Unique in-flight DATA segments abandoned by terminal error"),
      ADD_STAT(txMaxSegmentsInFlight, statistics::units::Count::get(),
               "Maximum live unique DATA segments after reset"),
      ADD_STAT(rxDataFramesReceived, statistics::units::Count::get(),
               "Routed decoded DATA frames accepted by the device"),
      ADD_STAT(rxDataEncodedBytesReceived, statistics::units::Byte::get(),
               "Encoded bytes in accepted routed DATA frames"),
      ADD_STAT(rxDataPayloadBytesReceived, statistics::units::Byte::get(),
               "Payload bytes in accepted routed DATA frames"),
      ADD_STAT(rxNewDataSegmentsCommitted, statistics::units::Count::get(),
               "Unique in-order DATA segments committed by payload DMA"),
      ADD_STAT(rxNewDataPayloadBytesCommitted,
               statistics::units::Byte::get(),
               "Unique in-order payload bytes committed by DMA"),
      ADD_STAT(rxDuplicateDataFrames, statistics::units::Count::get(),
               "Accepted DATA frames classified as duplicate or replay"),
      ADD_STAT(rxOutOfOrderDataFrames, statistics::units::Count::get(),
               "Accepted DATA frames classified as future sequence"),
      ADD_STAT(rxQueuedSegments, statistics::units::Count::get(),
               "Live DATA frames in the bounded receive backlog"),
      ADD_STAT(rxQueuedSegmentsAtReset, statistics::units::Count::get(),
               "Receive backlog at statistics reset"),
      ADD_STAT(rxSegmentsEnqueued, statistics::units::Count::get(),
               "DATA frames admitted to the receive backlog"),
      ADD_STAT(rxSegmentsDequeued, statistics::units::Count::get(),
               "DATA frames removed from the receive backlog"),
      ADD_STAT(rxSegmentsDiscarded, statistics::units::Count::get(),
               "Queued DATA frames discarded by reset or cleanup"),
      ADD_STAT(rxMaxQueuedSegments, statistics::units::Count::get(),
               "Maximum live receive backlog after reset")
{}

void
Pvrdma::TransportStats::preDumpStats()
{
    statistics::Group::preDumpStats();
    device.sampleTransportSerialization();
}

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
      companionMacWordSwap(p.companion_mac_word_swap),
      controlCompletionLatency(p.control_completion_latency),
      transportWindowSegments(p.transport_window_segments),
      transportStats(*this), queueStats(*this),
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
      sendEvent([this] { runSend(); }, name() + ".send"),
      transportDmaEvent([this] { transportDmaDone(); },
                        name() + ".transportDma"),
      transportTimerEvent([this] { transportTimerExpired(); },
                          name() + ".transportTimer")
{
    fatal_if(!transportWindowSegments ||
                 transportWindowSegments >= pvrdma::PsnHalfRange,
             "PVRDMA transport_window_segments must be in [1, %u]",
             pvrdma::PsnHalfRange - 1);
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
    transportStatsReset();
    statistics::registerResetCallback([this] {
        queueStatsReset();
        transportStatsReset();
    });
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
    transportStatsReset();
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
        if (!pvrdma::cqPollNeedsObservation(
                completionQueues.entries[doorbell.handle]) &&
            !(completionDma.stage ==
                  CompletionDmaState::Stage::PublishCqProducer &&
              completionDma.record.cqHandle == doorbell.handle)) {
            scheduleTransport();
            return;
        }
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
             (qp.state == pvrdma::QpState::Init ||
              qp.state == pvrdma::QpState::ReadyToReceive ||
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

uint64_t
Pvrdma::sendSegmentsInFlight() const
{
    uint64_t count = 0;
    for (const auto &send : sendWqes)
        count += send.sentSegments - send.ackedSegments;
    if (transportWindowSegments == 1 &&
        transport.kind == pvrdma::QueueKind::Sq &&
        (transport.stage == TransportState::Stage::WaitResponse ||
         transport.stage == TransportState::Stage::RetryWait ||
         (transport.stage == TransportState::Stage::TryData &&
          transport.retryPending)))
        ++count;
    return count;
}

size_t
Pvrdma::receiveBacklog() const
{
    return (pendingRxPacket ? 1 : 0) + pendingRxPackets.size();
}

void
Pvrdma::sampleTransportSerialization()
{
    if (txBusyClass == TxFrameClass::None)
        return;
    const Tick now = curTick();
    const Tick elapsed = now - txBusyStart;
    if (txBusyClass == TxFrameClass::Data)
        transportStats.txDataSerializationTicks += elapsed;
    else
        transportStats.txControlSerializationTicks += elapsed;
    txBusyStart = now;
}

void
Pvrdma::transportStatsReset()
{
    const uint64_t in_flight = sendSegmentsInFlight();
    const size_t backlog = receiveBacklog();
    transportStats.txSegmentsInFlight = in_flight;
    transportStats.txSegmentsInFlightAtReset = in_flight;
    transportStats.txMaxSegmentsInFlight = in_flight;
    transportStats.rxQueuedSegments = backlog;
    transportStats.rxQueuedSegmentsAtReset = backlog;
    transportStats.rxMaxQueuedSegments = backlog;
    if (txBusyClass != TxFrameClass::None)
        txBusyStart = curTick();
}

bool
Pvrdma::queueReceivePacket(EthPacketPtr packet)
{
    if (!packet || receiveBacklog() >= transportWindowSegments)
        return false;
    if (!pendingRxPacket)
        pendingRxPacket = std::move(packet);
    else
        pendingRxPackets.push_back(std::move(packet));
    transportStats.rxSegmentsEnqueued++;
    const uint64_t queued = receiveBacklog();
    transportStats.rxQueuedSegments = queued;
    if (queued > transportStats.rxMaxQueuedSegments.value())
        transportStats.rxMaxQueuedSegments = queued;
    return true;
}

EthPacketPtr
Pvrdma::popReceivePacket()
{
    if (!pendingRxPacket)
        return {};
    EthPacketPtr packet = std::move(pendingRxPacket);
    if (!pendingRxPackets.empty()) {
        pendingRxPacket = std::move(pendingRxPackets.front());
        pendingRxPackets.pop_front();
    }
    transportStats.rxSegmentsDequeued++;
    transportStats.rxQueuedSegments = receiveBacklog();
    return packet;
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

pvrdma::rocev2::MacAddress
storedMac(const pvrdma::QueuePair &qp)
{
    pvrdma::rocev2::MacAddress mac;
    std::copy_n(qp.attributes.addressHandle.destinationMac, mac.size(),
                mac.begin());
    return mac;
}

bool
zeroGid(const pvrdma::rocev2::Gid &gid)
{
    return std::all_of(gid.begin(), gid.end(),
                       [](uint8_t byte) { return byte == 0; });
}

pvrdma::rocev2::Gid
linkLocalGid(const pvrdma::rocev2::MacAddress &mac)
{
    const uint32_t low = uint32_t{mac[0]} | (uint32_t{mac[1]} << 8) |
        (uint32_t{mac[2]} << 16) | (uint32_t{mac[3]} << 24);
    const uint32_t high = uint32_t{mac[4]} | (uint32_t{mac[5]} << 8);
    const uint64_t guid = pvrdma::detail::macGuid(low, high);
    pvrdma::rocev2::Gid gid{};
    gid[0] = 0xfe;
    gid[1] = 0x80;
    for (size_t i = 0; i < 8; ++i)
        gid[8 + i] = guid >> (56 - 8 * i);
    return gid;
}

bool
routeGids(const pvrdma::QueuePair &qp, const pvrdma::GidTable &gids,
          const pvrdma::GidValidTable &valid,
          const pvrdma::rocev2::MacAddress &local_mac,
          pvrdma::rocev2::Gid &local_gid,
          pvrdma::rocev2::Gid &remote_gid)
{
    const auto &route = qp.attributes.addressHandle.globalRoute;
    const bool local_configured = route.sourceGidIndex < valid.size() &&
        valid[route.sourceGidIndex];
    std::copy_n(route.destinationGid.raw, remote_gid.size(),
                remote_gid.begin());
    const bool remote_configured = !zeroGid(remote_gid);
    if (local_configured != remote_configured)
        return false;
    if (local_configured) {
        std::copy_n(gids[route.sourceGidIndex].raw, local_gid.size(),
                    local_gid.begin());
    } else {
        local_gid = linkLocalGid(local_mac);
        remote_gid = linkLocalGid(storedMac(qp));
    }
    return !zeroGid(local_gid) &&
        !pvrdma::detail::ipv4MappedGid(local_gid.data()) &&
        !pvrdma::detail::ipv4MappedGid(remote_gid.data());
}

bool
routeMatches(const pvrdma::QueuePair &qp,
             const pvrdma::rocev2::Packet &packet,
             const pvrdma::rocev2::MacAddress &local_mac,
             const pvrdma::rocev2::Gid &local_gid,
             const pvrdma::rocev2::Gid &remote_gid)
{
    return packet.destinationMac == local_mac &&
        packet.destinationGid == local_gid && packet.sourceGid == remote_gid &&
        packet.destinationQpn == qp.qpn &&
        packet.pKey == pvrdma::FullMembershipPkey &&
        (qp.state == pvrdma::QpState::ReadyToReceive ||
         qp.state == pvrdma::QpState::ReadyToSend) &&
        pvrdma::validQpn(qp.attributes.destinationQpNumber);
}

bool
nakableShapeError(pvrdma::rocev2::CodecError error)
{
    using pvrdma::rocev2::CodecError;
    switch (error) {
      case CodecError::BadOpcode:
      case CodecError::BadBthVersion:
      case CodecError::BadReserved:
      case CodecError::BadAckRequest:
      case CodecError::BadIpv6Length:
      case CodecError::BadUdpLength:
      case CodecError::ExtraBytes:
      case CodecError::BadPad:
      case CodecError::BadPayloadLength:
        return true;
      default:
        return false;
    }
}

bool
routeFromMalformed(const uint8_t *bytes, size_t length,
                   pvrdma::rocev2::Packet &packet)
{
    using namespace pvrdma::rocev2;
    if (!bytes || length < SendHeaderSize + IcrcSize ||
        detail::get32Le(bytes, length - IcrcSize) !=
            detail::icrc(bytes + EthernetHeaderSize,
                         length - EthernetHeaderSize - IcrcSize) ||
        detail::get16(bytes, EtherTypeOffset) != EtherType ||
        (bytes[Ipv6Offset] >> 4) != 6 ||
        bytes[Ipv6NextHeaderOffset] != Ipv6NextHeader ||
        detail::get16(bytes, UdpDestinationPortOffset) !=
            RoceUdpDestinationPort ||
        detail::get16(bytes, UdpSourcePortOffset) < MinUdpSourcePort)
        return false;
    std::copy_n(bytes + EthernetDestinationOffset, 6,
                packet.destinationMac.begin());
    std::copy_n(bytes + EthernetSourceOffset, 6, packet.sourceMac.begin());
    std::copy_n(bytes + Ipv6SourceOffset, 16, packet.sourceGid.begin());
    std::copy_n(bytes + Ipv6DestinationOffset, 16,
                packet.destinationGid.begin());
    packet.trafficClass = ((bytes[Ipv6Offset] & 0x0f) << 4) |
                          (bytes[Ipv6Offset + 1] >> 4);
    packet.flowLabel = (uint32_t{bytes[Ipv6Offset + 1]} & 0x0f) << 16 |
                       (uint32_t{bytes[Ipv6Offset + 2]} << 8) |
                       bytes[Ipv6Offset + 3];
    packet.hopLimit = bytes[Ipv6HopLimitOffset];
    packet.sourcePort = detail::get16(bytes, UdpSourcePortOffset);
    packet.pKey = detail::get16(bytes, BthPKeyOffset);
    packet.opcode = static_cast<Opcode>(bytes[BthOffset]);
    packet.destinationQpn = detail::get24(bytes, BthQpnOffset + 1);
    packet.psn = detail::get24(bytes, BthPsnOffset);
    return pvrdma::validQpn(packet.destinationQpn);
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

// ponytail: the device-wide single receive flight is abandoned after this
// local ceiling; move to per-QP receive timers if concurrent transport lands.
constexpr uint8_t ReceiveIdleTimeoutEncoding = 10;

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
        sendEvent.scheduled() || transportTimerEvent.scheduled() ||
        transport.dmaBusy || !sendWqes.empty() || sendPacket ||
        txBusyClass != TxFrameClass::None || pendingRxPacket ||
        !pendingRxPackets.empty() || pendingErrorPacket;
}

bool
Pvrdma::transportDmaBusy() const
{
    return transport.dmaBusy || transportDmaEvent.scheduled();
}

bool
Pvrdma::terminalCompletionBackpressured() const
{
    const auto stage = transport.kind == pvrdma::QueueKind::Rq ?
        TransportState::Stage::WaitReceiveCq :
        TransportState::Stage::WaitSendCq;
    return transport.active() &&
        (transport.kind == pvrdma::QueueKind::Rq ||
         transport.kind == pvrdma::QueueKind::Sq) &&
        transport.terminalQpError && transport.stage == stage &&
        transport.completionBackpressured && !completionBusy() &&
        !transportDmaBusy() && !dmaPending() &&
        txBusyClass == TxFrameClass::None && !sendEvent.scheduled();
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
    pvrdma::rocev2::Gid local_gid{}, remote_gid{};
    if (!routeGids(qp, gids, gidValid, transport.localMac,
                   local_gid, remote_gid) ||
        local_gid != transport.localGid || remote_gid != transport.remoteGid)
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
Pvrdma::receiveSegmentMatches(const pvrdma::rocev2::Packet &frame,
                              bool accepted) const
{
    if (!transport.active() || transport.kind != pvrdma::QueueKind::Rq ||
        (accepted && !transport.hasAcceptedSegment) ||
        frame.destinationMac != transport.localMac ||
        frame.sourceGid != transport.remoteGid ||
        frame.destinationGid != transport.localGid ||
        frame.destinationQpn != transport.localQpn)
        return false;

    const auto opcode = accepted ? transport.acceptedOpcode :
                                   transport.opcode;
    const uint32_t psn = accepted ? transport.acceptedPsn : transport.psn;
    const size_t index = accepted ? transport.acceptedSegmentIndex :
                                    transport.segmentIndex;
    const size_t offset = index * pvrdma::FixedMtu;
    if (!accepted && offset > transport.totalLength)
        return false;
    const size_t length = accepted ? transport.acceptedSegmentLength :
                                     transport.totalLength - offset;
    return frame.opcode == opcode && frame.psn == psn &&
        frame.payload.size == length &&
        offset <= transport.payload.size() &&
        length <= transport.payload.size() - offset &&
        (!length || std::equal(frame.payload.data,
                               frame.payload.data + length,
                               transport.payload.begin() + offset));
}

bool
Pvrdma::handleSendResponse(const pvrdma::rocev2::Packet &frame,
                           pvrdma::QueuePair &qp)
{
    using pvrdma::rocev2::Syndrome;
    if (sendWqes.empty() || qp.qpHandle != sendQpHandle ||
        !revalidateSendWqe(sendWqes.front()))
        return true;
    const uint64_t in_flight = sendSegmentsInFlight();
    if (!in_flight)
        return true;
    const uint32_t distance = pvrdma::psnDistance(
        qp.attributes.sendPsn, frame.psn);
    if (distance >= pvrdma::PsnHalfRange)
        return true;

    const auto acknowledge = [&](uint64_t count) {
        panic_if(count > sendSegmentsInFlight(),
                 "PVRDMA acknowledged unsent pipeline segments");
        uint64_t remaining = count;
        for (auto &send : sendWqes) {
            const uint64_t available = send.sentSegments -
                send.ackedSegments;
            const uint64_t accepted = std::min(remaining, available);
            send.ackedSegments += accepted;
            if (accepted)
                send.lastAckMsn = frame.msn;
            remaining -= accepted;
            if (!remaining)
                break;
        }
        qp.attributes.sendPsn = (qp.attributes.sendPsn + count) &
            pvrdma::rocev2::Field24Mask;
        transport.livePsn = qp.attributes.sendPsn;
        transportStats.txSegmentsAcknowledged += count;
        transportStats.txSegmentsInFlight = sendSegmentsInFlight();
        if (sendRetryActive) {
            const uint32_t retry_distance = pvrdma::psnDistance(
                qp.attributes.sendPsn, sendRetryPsn);
            if (retry_distance >= pvrdma::PsnHalfRange)
                sendRetryPsn = qp.attributes.sendPsn;
            if (!sendSegmentsInFlight())
                sendRetryActive = false;
        }
    };
    const auto begin_retry = [&](bool rnr, uint8_t timer) {
        size_t wqe_index = 0;
        uint16_t segment_index = 0;
        if (!findSendSegment(qp.attributes.sendPsn, wqe_index,
                             segment_index)) {
            failSendPipeline(0,
                pvrdma::CompletionStatus::GeneralError);
            return;
        }
        auto &send = sendWqes[wqe_index];
        uint8_t &remaining = rnr ? send.rnrRetryRemaining :
                                  send.retryRemaining;
        if (!pvrdma::useRetry(remaining)) {
            failSendPipeline(wqe_index, rnr ?
                pvrdma::CompletionStatus::RnrRetryExceededError :
                pvrdma::CompletionStatus::RetryExceededError);
            return;
        }
        cancelTransportTimer();
        sendRetryPsn = qp.attributes.sendPsn;
        sendRetryEndPsn = (qp.attributes.sendPsn +
            sendSegmentsInFlight()) & pvrdma::rocev2::Field24Mask;
        sendRetryActive = !rnr;
        sendRnrWait = rnr;
        if (rnr)
            armTransportTimer(TransportTimerKind::Rnr, rnrDelay(timer));
        else
            scheduleSend();
    };

    // Continuations already on the wire can NAK after their first segment
    // selected RNR backoff.  Only suppress responses for that same WQE;
    // errors for a later WQE must remain actionable.
    bool stale_rnr_response = false;
    if (sendRnrWait &&
        (frame.syndrome == Syndrome::Rnr ||
         frame.syndrome == Syndrome::SequenceNak ||
         frame.syndrome == Syndrome::InvalidRequestNak)) {
        size_t retry_wqe = 0;
        size_t response_wqe = 0;
        uint16_t retry_segment = 0;
        uint16_t response_segment = 0;
        stale_rnr_response =
            findSendSegment(sendRetryPsn, retry_wqe, retry_segment) &&
            findSendSegment(frame.psn, response_wqe, response_segment) &&
            retry_wqe == response_wqe;
    }
    if (stale_rnr_response)
        return true;

    if (frame.syndrome == Syndrome::Ack) {
        if (distance >= in_flight) {
            armSendAckTimer();
            return true;
        }
        cancelTransportTimer();
        sendRnrWait = false;
        acknowledge(distance + 1);
        if (sendSegmentsInFlight())
            armSendAckTimer();
    } else if (frame.syndrome == Syndrome::SequenceNak) {
        cancelTransportTimer();
        sendRnrWait = false;
        if (distance > in_flight) {
            // The deque front may already own a success retirement snapshot.
            // Fail the first unacknowledged WQE, not that completed prefix.
            size_t wqe_index = 0;
            uint16_t segment_index = 0;
            panic_if(!findSendSegment(qp.attributes.sendPsn, wqe_index,
                                     segment_index),
                     "PVRDMA live SEND PSN has no pipeline WQE");
            failSendPipeline(wqe_index,
                pvrdma::CompletionStatus::GeneralError);
            return true;
        }
        if (distance)
            acknowledge(distance);
        if (distance < in_flight)
            begin_retry(false, 0);
    } else if (frame.syndrome == Syndrome::Rnr) {
        if (distance >= in_flight) {
            armSendAckTimer();
            return true;
        }
        cancelTransportTimer();
        if (distance)
            acknowledge(distance);
        begin_retry(true, frame.rnrTimer);
    } else {
        if (distance >= in_flight) {
            armSendAckTimer();
            return true;
        }
        cancelTransportTimer();
        sendRnrWait = false;
        if (distance)
            acknowledge(distance);
        size_t wqe_index = 0;
        uint16_t segment_index = 0;
        if (!findSendSegment(qp.attributes.sendPsn, wqe_index,
                             segment_index)) {
            failSendPipeline(0,
                pvrdma::CompletionStatus::GeneralError);
            return true;
        }
        const auto status = frame.syndrome == Syndrome::InvalidRequestNak ?
            pvrdma::CompletionStatus::RemoteInvalidRequestError :
            frame.syndrome == Syndrome::RemoteAccessNak ?
                pvrdma::CompletionStatus::RemoteAccessError :
                pvrdma::CompletionStatus::RemoteOperationError;
        failSendPipeline(wqe_index, status);
    }
    scheduleTransport();
    scheduleSend();
    return true;
}

bool
Pvrdma::recvTransportPacket(EthPacketPtr packet)
{
    using namespace pvrdma::rocev2;
    if (!packet)
        return false;
    if (packet->length > packet->bufLength)
        return true;
    const auto decoded = decode({packet->data, packet->bufLength},
                                packet->length);
    if (!decoded) {
        pvrdma::rocev2::Packet malformed;
        if (nakableShapeError(decoded.error) &&
            routeFromMalformed(packet->data, packet->length, malformed)) {
            queueReverseError(malformed,
                              pvrdma::rocev2::Syndrome::InvalidRequestNak,
                              malformed.psn);
            scheduleTransport();
        }
        return true;
    }
    const auto &frame = decoded.packet;
    const auto local_mac = pvrdma::transportMac(regs, companionMacWordSwap);
    if (frame.destinationMac != local_mac)
        return true;

    auto *qp = pvrdma::findQueuePair(queuePairs, frame.destinationQpn);
    if (!qp)
        return true;
    Gid local_gid{}, remote_gid{};
    if (!routeGids(*qp, gids, gidValid, local_mac, local_gid, remote_gid) ||
        !routeMatches(*qp, frame, local_mac, local_gid, remote_gid)) {
        queueReverseError(frame, Syndrome::InvalidRequestNak, frame.psn);
        scheduleTransport();
        return true;
    }

    if (frame.opcode == Opcode::Acknowledge &&
        transportWindowSegments > 1 && sendQpHandle == qp->qpHandle)
        return handleSendResponse(frame, *qp);

    if (frame.opcode == Opcode::Acknowledge) {
        if (!transport.active() || transport.kind != pvrdma::QueueKind::Sq ||
            transport.qpHandle != qp->qpHandle ||
            transport.localGid != local_gid ||
            transport.remoteGid != remote_gid)
            return true;
        const uint32_t distance = pvrdma::psnDistance(
            transport.initialPsn, frame.psn);
        const bool response_pending =
            transport.stage == TransportState::Stage::WaitResponse ||
            (transport.stage == TransportState::Stage::TryData &&
             transport.retryPending);
        if (!response_pending)
            return true;
        if (frame.syndrome != Syndrome::SequenceNak &&
            distance != transport.segmentIndex)
            return true;

        cancelTransportTimer();
        transport.retryPending = false;
        transport.packet.reset();
        const auto accept_current = [&] {
            transportStats.txSegmentsAcknowledged++;
            transportStats.txSegmentsInFlight = 0;
            qp->attributes.sendPsn = pvrdma::advancePsn(
                qp->attributes.sendPsn);
            transport.livePsn = qp->attributes.sendPsn;
            if (++transport.segmentIndex < transport.segmentCount) {
                transport.psn = transport.livePsn;
                transport.stage = TransportState::Stage::TryData;
            } else {
                transport.status = pvrdma::CompletionStatus::Success;
                transport.stage = transport.wqe.signaled ?
                    TransportState::Stage::WaitSendCq :
                    TransportState::Stage::WriteSqConsumer;
            }
        };
        if (!revalidateTransport(true)) {
            failSend(pvrdma::CompletionStatus::GeneralError, true);
        } else if (frame.syndrome == Syndrome::Ack) {
            transport.lastAckMsn = frame.msn;
            accept_current();
        } else if (frame.syndrome == Syndrome::Rnr) {
            if (pvrdma::useRetry(transport.rnrRetryRemaining)) {
                transport.stage = TransportState::Stage::RetryWait;
                armTransportTimer(TransportTimerKind::Rnr,
                                  rnrDelay(frame.rnrTimer));
            } else {
                failSend(pvrdma::CompletionStatus::RnrRetryExceededError,
                         true);
            }
        } else if (frame.syndrome == Syndrome::SequenceNak) {
            const auto action = pvrdma::sequenceNakAction(
                transport.initialPsn, transport.segmentIndex,
                transport.segmentCount, frame.psn,
                transport.retryRemaining);
            if (action == pvrdma::SequenceNakAction::Advance) {
                accept_current();
            } else if (action == pvrdma::SequenceNakAction::Restart) {
                transport.segmentIndex = pvrdma::psnDistance(
                    transport.initialPsn, frame.psn);
                transport.psn = frame.psn;
                qp->attributes.sendPsn = frame.psn;
                transport.livePsn = frame.psn;
                transport.stage = TransportState::Stage::TryData;
            } else if (action ==
                       pvrdma::SequenceNakAction::RetryExceeded) {
                failSend(pvrdma::CompletionStatus::RetryExceededError,
                         true);
            } else {
                failSend(pvrdma::CompletionStatus::GeneralError, true);
            }
        } else {
            const auto status = frame.syndrome == Syndrome::InvalidRequestNak ?
                pvrdma::CompletionStatus::RemoteInvalidRequestError :
                frame.syndrome == Syndrome::RemoteAccessNak ?
                    pvrdma::CompletionStatus::RemoteAccessError :
                    pvrdma::CompletionStatus::RemoteOperationError;
            failSend(status, true);
        }
        scheduleTransport();
        return true;
    }

    const size_t encoded_length = packet->simLength;
    const auto record_data = [&] {
        transportStats.rxDataFramesReceived++;
        transportStats.rxDataEncodedBytesReceived += encoded_length;
        transportStats.rxDataPayloadBytesReceived += frame.payload.size;
    };
    if (transport.active() && transport.kind == pvrdma::QueueKind::Rq &&
        transport.stage == TransportState::Stage::WaitReceiveData) {
        record_data();
        return handleInboundContinuation(frame);
    }
    if (transport.active() && transport.kind == pvrdma::QueueKind::Rq) {
        if (receiveSegmentMatches(frame, false) ||
            receiveSegmentMatches(frame, true)) {
            record_data();
            transportStats.rxDuplicateDataFrames++;
            return true;
        }
        if (!queueReceivePacket(std::move(packet)))
            return false;
        record_data();
        scheduleTransport();
        return true;
    }
    if (transport.active()) {
        record_data();
        queueReverseError(frame, Syndrome::InvalidRequestNak, frame.psn);
        scheduleTransport();
        return true;
    }
    if (replayFinal(frame)) {
        record_data();
        transportStats.rxDuplicateDataFrames++;
        scheduleTransport();
        return true;
    }
    if (!queueReceivePacket(std::move(packet)))
        return false;
    record_data();
    scheduleTransport();
    return true;
}

bool
Pvrdma::handleInboundContinuation(const pvrdma::rocev2::Packet &frame)
{
    using namespace pvrdma::rocev2;
    const bool route = frame.destinationMac == transport.localMac &&
        frame.sourceGid == transport.remoteGid &&
        frame.destinationGid == transport.localGid &&
        frame.destinationQpn == transport.localQpn;
    if (!route) {
        queueReverseError(frame, Syndrome::InvalidRequestNak,
                          transport.livePsn);
        return true;
    }
    if (transport.stage == TransportState::Stage::WriteRqPayload &&
        receiveSegmentMatches(frame, false)) {
        transportStats.rxDuplicateDataFrames++;
        return true;
    }
    if (receiveSegmentMatches(frame, true)) {
        transportStats.rxDuplicateDataFrames++;
        if (transport.stage == TransportState::Stage::WaitReceiveData) {
            transport.responseMsn = queuePairs.entries[
                transport.qpHandle].responderMsn;
            transport.keepAfterControl = true;
            prepareControl(Syndrome::Ack, frame.psn);
        }
        return true;
    }
    if (frame.psn != transport.livePsn) {
        const uint32_t distance = pvrdma::psnDistance(
            transport.livePsn, frame.psn);
        if (distance < pvrdma::PsnHalfRange) {
            transportStats.rxOutOfOrderDataFrames++;
            transport.keepAfterControl = true;
            prepareControl(Syndrome::SequenceNak, transport.livePsn);
        } else {
            transportStats.rxDuplicateDataFrames++;
            transport.keepAfterControl = true;
            prepareControl(Syndrome::SequenceNak, transport.livePsn);
        }
        return true;
    }
    if (transport.stage != TransportState::Stage::WaitReceiveData ||
        !pvrdma::continuationOpcode(frame.opcode)) {
        failReceive(pvrdma::CompletionStatus::LocalQpOperationError,
                    true, Syndrome::InvalidRequestNak);
        return true;
    }
    if (frame.payload.size > transport.wqe.length ||
        transport.totalLength > transport.wqe.length - frame.payload.size) {
        failReceive(pvrdma::CompletionStatus::LocalLengthError,
                    true, Syndrome::InvalidRequestNak);
        return true;
    }

    cancelTransportTimer();
    const size_t offset = transport.totalLength;
    transport.psn = frame.psn;
    transport.opcode = frame.opcode;
    ++transport.segmentIndex;
    transport.payload.insert(transport.payload.end(), frame.payload.data,
                             frame.payload.data + frame.payload.size);
    transport.totalLength += frame.payload.size;
    if (!frame.payload.size ||
        !beginPayloadDma(true, offset, frame.payload.size)) {
        transport.payload.resize(offset);
        transport.totalLength = offset;
        failReceive(pvrdma::CompletionStatus::LocalQpOperationError,
                    true, Syndrome::RemoteOperationNak);
    }
    scheduleTransport();
    return true;
}

bool
Pvrdma::replayFinal(const pvrdma::rocev2::Packet &frame)
{
    auto *qp = pvrdma::findQueuePair(queuePairs, frame.destinationQpn);
    if (!qp || !pvrdma::finalOpcode(frame.opcode))
        return false;
    const auto &replay = qp->finalReplay;
    if (!replay.valid || replay.qpGeneration != qp->generation ||
        replay.localMac != frame.destinationMac ||
        replay.localGid != frame.destinationGid ||
        replay.remoteGid != frame.sourceGid ||
        replay.localQpn != frame.destinationQpn ||
        replay.finalPsn != frame.psn || replay.finalOpcode != frame.opcode ||
        replay.finalSegmentLength != frame.payload.size)
        return false;

    transport.reset();
    transport.kind = pvrdma::QueueKind::Rq;
    transport.qpHandle = qp->qpHandle;
    transport.qpGeneration = qp->generation;
    transport.cqHandle = qp->recvCqHandle;
    transport.cqGeneration = completionQueues.entries[
        qp->recvCqHandle].generation;
    transport.consumer = qp->rqConsumerHead;
    transport.localMac = replay.localMac;
    transport.remoteMac = replay.remoteMac;
    transport.localGid = replay.localGid;
    transport.remoteGid = replay.remoteGid;
    transport.localQpn = replay.localQpn;
    transport.remoteQpn = replay.remoteQpn;
    transport.psn = transport.acceptedPsn = replay.finalPsn;
    transport.livePsn = qp->attributes.receivePsn;
    transport.opcode = transport.acceptedOpcode = replay.finalOpcode;
    transport.acceptedSegmentLength = replay.finalSegmentLength;
    transport.hasAcceptedSegment = true;
    transport.responseMsn = replay.completedMsn;
    transport.stage = TransportState::Stage::TryAck;
    return true;
}

bool
Pvrdma::sendAccepted(EthPacketPtr packet, TxFrameClass frame_class,
                     size_t payload_bytes, bool retry)
{
    if (!packet || txBusyClass != TxFrameClass::None)
        return false;
    txBusyClass = frame_class;
    txBusyStart = curTick();
    if (!interface.sendPacket(packet)) {
        txBusyClass = TxFrameClass::None;
        return false;
    }
    if (frame_class == TxFrameClass::Data) {
        transportStats.txDataFramesAccepted++;
        transportStats.txDataEncodedBytesAccepted += packet->simLength;
        transportStats.txDataPayloadBytesAccepted += payload_bytes;
        if (retry) {
            transportStats.txRetransmittedSegmentsAccepted++;
            transportStats.txRetransmittedPayloadBytesAccepted +=
                payload_bytes;
        } else {
            transportStats.txNewDataSegmentsAccepted++;
            transportStats.txNewDataPayloadBytesAccepted += payload_bytes;
        }
    } else {
        transportStats.txControlFramesAccepted++;
        transportStats.txControlEncodedBytesAccepted += packet->simLength;
    }
    return true;
}

void
Pvrdma::transportSendDone()
{
    if (txBusyClass != TxFrameClass::None) {
        sampleTransportSerialization();
        txBusyClass = TxFrameClass::None;
    }
    scheduleSend();
    scheduleTransport();
}

bool
Pvrdma::revalidateSendWqe(const SendWqeState &send) const
{
    if (controlState != pvrdma::ControlState::Active ||
        !send.qpHandle || send.qpHandle >= pvrdma::ObjectTableEntries ||
        !send.leaseHeld)
        return false;
    const auto &qp = queuePairs.entries[send.qpHandle];
    if (!qp.valid || qp.qpHandle != send.qpHandle ||
        qp.generation != send.qpGeneration ||
        qp.state != pvrdma::QpState::ReadyToSend ||
        qp.qpn != send.localQpn ||
        qp.attributes.destinationQpNumber != send.remoteQpn ||
        qp.sendCqHandle != send.cqHandle || storedMac(qp) != send.remoteMac)
        return false;
    pvrdma::rocev2::Gid local_gid{}, remote_gid{};
    if (!routeGids(qp, gids, gidValid, send.localMac,
                   local_gid, remote_gid) || local_gid != send.localGid ||
        remote_gid != send.remoteGid)
        return false;
    const auto &lease = send.lease;
    if (!lease.slot || lease.slot >= pvrdma::ObjectTableEntries)
        return false;
    const auto &mr = memoryRegions.entries[lease.slot];
    return mr.valid && mr.generation == lease.generation &&
        mr.mrHandle == send.wqe.lkey && mr.lkey == send.wqe.lkey &&
        mr.pdHandle == qp.pdHandle && mr.activeReferences;
}

bool
Pvrdma::findSendSegment(uint32_t psn, size_t &wqe_index,
                        uint16_t &segment_index) const
{
    for (size_t i = 0; i < sendWqes.size(); ++i) {
        const auto &send = sendWqes[i];
        const uint32_t distance = pvrdma::psnDistance(send.initialPsn, psn);
        if (distance < send.sentSegments) {
            wqe_index = i;
            segment_index = distance;
            return true;
        }
    }
    return false;
}

void
Pvrdma::scheduleSend()
{
    if (transportPaused || controlState != pvrdma::ControlState::Active ||
        txBusyClass != TxFrameClass::None || sendRnrWait ||
        sendEvent.scheduled() || sendWqes.empty() ||
        (sendPipelineFailed && !sendRetryActive))
        return;
    if (sendPacket || sendRetryActive ||
        sendSegmentsInFlight() < transportWindowSegments)
        schedule(sendEvent, nextCycle());
}

void
Pvrdma::runSend()
{
    using namespace pvrdma::rocev2;
    if (transportPaused || controlState != pvrdma::ControlState::Active ||
        txBusyClass != TxFrameClass::None || sendRnrWait ||
        sendWqes.empty() || (sendPipelineFailed && !sendRetryActive))
        return;

    if (!sendPacket) {
        size_t wqe_index = 0;
        uint16_t segment_index = 0;
        bool retry = sendRetryActive;
        if (retry) {
            if (sendRetryPsn == sendRetryEndPsn) {
                sendRetryActive = false;
                armSendAckTimer();
                retry = false;
            } else if (!findSendSegment(sendRetryPsn, wqe_index,
                                        segment_index)) {
                failSendPipeline(0,
                    pvrdma::CompletionStatus::GeneralError);
                return;
            }
        }
        if (!retry) {
            if (sendSegmentsInFlight() >= transportWindowSegments)
                return;
            const auto entry = std::find_if(
                sendWqes.begin(), sendWqes.end(), [](const auto &send) {
                    return !send.terminalQpError &&
                        send.sentSegments < send.segmentCount;
                });
            if (entry == sendWqes.end()) {
                scheduleTransport();
                return;
            }
            wqe_index = std::distance(sendWqes.begin(), entry);
            segment_index = entry->sentSegments;
        }

        auto &send = sendWqes[wqe_index];
        if (!revalidateSendWqe(send)) {
            failSendPipeline(wqe_index,
                pvrdma::CompletionStatus::GeneralError);
            return;
        }
        const size_t offset = size_t{segment_index} * pvrdma::FixedMtu;
        const size_t length = send.wqe.length == 0 ? 0 :
            std::min<size_t>(pvrdma::FixedMtu,
                             send.wqe.length - offset);
        pvrdma::rocev2::Packet frame;
        frame.destinationMac = send.remoteMac;
        frame.sourceMac = send.localMac;
        frame.destinationGid = send.remoteGid;
        frame.sourceGid = send.localGid;
        frame.opcode = pvrdma::sendOpcode(segment_index,
                                           send.segmentCount);
        frame.ackRequest = true;
        frame.destinationQpn = send.remoteQpn;
        frame.psn = (send.initialPsn + segment_index) &
            pvrdma::rocev2::Field24Mask;
        const auto &route = queuePairs.entries[
            send.qpHandle].attributes.addressHandle.globalRoute;
        frame.trafficClass = route.trafficClass;
        frame.flowLabel = route.flowLabel;
        frame.hopLimit = route.hopLimit ? route.hopLimit : 0xff;
        frame.sourcePort = udpSourcePort(frame.flowLabel, send.localQpn,
                                         send.remoteQpn);
        frame.payload = {length ? send.payload.data() + offset : nullptr,
                         length};
        const size_t size = encodedSize(frame);
        sendPacket = std::make_shared<EthPacketData>(size);
        const auto encoded = encode(
            frame, {sendPacket->data, sendPacket->bufLength});
        if (!encoded) {
            sendPacket.reset();
            failSendPipeline(wqe_index,
                pvrdma::CompletionStatus::GeneralError);
            return;
        }
        sendPacket->length = sendPacket->simLength = encoded.size;
        sendPacketPsn = frame.psn;
        sendPacketWqeIndex = wqe_index;
        sendPacketSegmentIndex = segment_index;
        sendPacketPayloadBytes = length;
        sendPacketRetry = retry;
    }

    if (!interface.getPeer()) {
        sendPacket.reset();
        failSendPipeline(sendPacketWqeIndex,
            pvrdma::CompletionStatus::GeneralError);
        return;
    }
    if (!sendAccepted(sendPacket, TxFrameClass::Data,
                      sendPacketPayloadBytes, sendPacketRetry))
        return;

    auto &send = sendWqes[sendPacketWqeIndex];
    const uint64_t before = sendSegmentsInFlight();
    if (sendPacketRetry) {
        sendRetryPsn = pvrdma::advancePsn(sendRetryPsn);
        if (sendRetryPsn == sendRetryEndPsn) {
            sendRetryActive = false;
            armSendAckTimer();
        }
    } else {
        panic_if(send.sentSegments != sendPacketSegmentIndex,
                 "PVRDMA sent a DATA segment out of WQE order");
        ++send.sentSegments;
        const uint64_t current = before + 1;
        transportStats.txSegmentsInFlight = current;
        if (current > transportStats.txMaxSegmentsInFlight.value())
            transportStats.txMaxSegmentsInFlight = current;
        if (!before)
            armSendAckTimer();
    }
    sendPacket.reset();
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
Pvrdma::startSendLoad()
{
    if (transportWindowSegments == 1 ||
        transport.kind != pvrdma::QueueKind::Sq || transport.sendLoading ||
        sendPipelineFailed || sendWqes.size() >= 2 ||
        !transport.qpHandle ||
        transport.qpHandle >= pvrdma::ObjectTableEntries)
        return false;
    auto &qp = queuePairs.entries[transport.qpHandle];
    uint32_t &consumer = sqFetchHead[transport.qpHandle];
    if (consumer == qp.sqProducerTail)
        return false;

    SendWqeState send;
    send.qpHandle = transport.qpHandle;
    send.qpGeneration = transport.qpGeneration;
    send.cqHandle = transport.cqHandle;
    send.cqGeneration = transport.cqGeneration;
    send.consumer = consumer;
    send.nextConsumer = pvrdma::ringAdvance(
        consumer, qp.capabilities.maxSendWr);
    send.consumerLe = htole(send.nextConsumer);
    send.localMac = transport.localMac;
    send.remoteMac = transport.remoteMac;
    send.localGid = transport.localGid;
    send.remoteGid = transport.remoteGid;
    send.localQpn = transport.localQpn;
    send.remoteQpn = transport.remoteQpn;
    send.retryRemaining = qp.attributes.retryCount;
    send.rnrRetryRemaining = qp.attributes.rnrRetry;
    if (!pvrdma::wqeAddress(qp, pvrdma::QueueKind::Sq, consumer,
                            send.wqeAddress)) {
        consumer = send.nextConsumer;
        sendWqes.push_back(std::move(send));
        failSendPipeline(sendWqes.size() - 1,
            pvrdma::CompletionStatus::GeneralError);
        return true;
    }
    consumer = send.nextConsumer;
    sendWqes.push_back(std::move(send));
    transport.sendLoading = true;
    transport.stage = TransportState::Stage::ReadSqWqe;
    auto &loading = sendWqes.back();
    startTransportDma(false, loading.wqeAddress, loading.sqSlot.size(),
                      loading.sqSlot.data());
    return true;
}

void
Pvrdma::finishSendLoad(pvrdma::CompletionStatus status)
{
    transport.sendLoading = false;
    if (status != pvrdma::CompletionStatus::Success) {
        failSendPipeline(sendWqes.empty() ? 0 : sendWqes.size() - 1,
                         status);
        return;
    }
    transport.stage = TransportState::Stage::TryData;
    scheduleSend();
    scheduleTransport();
}

void
Pvrdma::startSendRetirement()
{
    if (sendWqes.empty() || !sendWqes.front().readyToRetire())
        return;
    auto &send = sendWqes.front();
    transport.kind = pvrdma::QueueKind::Sq;
    transport.qpHandle = send.qpHandle;
    transport.qpGeneration = send.qpGeneration;
    transport.cqHandle = send.cqHandle;
    transport.cqGeneration = send.cqGeneration;
    transport.consumer = send.consumer;
    transport.nextConsumer = send.nextConsumer;
    transport.consumerLe = send.consumerLe;
    transport.wqe = send.wqe;
    transport.lease = send.lease;
    transport.leaseHeld = send.leaseHeld;
    transport.localMac = send.localMac;
    transport.remoteMac = send.remoteMac;
    transport.localGid = send.localGid;
    transport.remoteGid = send.remoteGid;
    transport.localQpn = send.localQpn;
    transport.remoteQpn = send.remoteQpn;
    transport.initialPsn = send.initialPsn;
    transport.segmentCount = send.segmentCount;
    transport.segmentIndex = send.ackedSegments;
    transport.livePsn = queuePairs.entries[
        send.qpHandle].attributes.sendPsn;
    transport.retryRemaining = send.retryRemaining;
    transport.rnrRetryRemaining = send.rnrRetryRemaining;
    transport.lastAckMsn = send.lastAckMsn;
    transport.status = send.status;
    transport.terminalQpError = send.terminalQpError;
    transport.stage = (send.wqe.signaled || send.terminalQpError) ?
        TransportState::Stage::WaitSendCq :
        TransportState::Stage::WriteSqConsumer;
    if (transport.stage == TransportState::Stage::WaitSendCq)
        submitTransportCompletion();
    else
        publishConsumer(pvrdma::QueueKind::Sq);
}

void
Pvrdma::failSendPipeline(size_t wqe_index,
                         pvrdma::CompletionStatus status)
{
    if (sendWqes.empty()) {
        transport.reset();
        sendQpHandle = 0;
        return;
    }
    wqe_index = std::min(wqe_index, sendWqes.size() - 1);
    // A suffix failure does not own the accepted prefix's retry timer.
    uint64_t prefix_in_flight = 0;
    for (size_t i = 0; i < wqe_index; ++i)
        prefix_in_flight += sendWqes[i].sentSegments -
            sendWqes[i].ackedSegments;
    if (prefix_in_flight) {
        const auto &qp = queuePairs.entries[sendQpHandle];
        sendRetryEndPsn = (qp.attributes.sendPsn + prefix_in_flight) &
            pvrdma::rocev2::Field24Mask;
        if (sendRetryActive && pvrdma::psnDistance(
                qp.attributes.sendPsn, sendRetryPsn) >= prefix_in_flight)
            sendRetryPsn = qp.attributes.sendPsn;
        sendRetryActive &= sendRetryPsn != sendRetryEndPsn;
    } else {
        cancelTransportTimer();
        sendRetryActive = false;
        sendRnrWait = false;
    }
    sendPipelineFailed = true;
    sendPacket.reset();
    auto &failed = sendWqes[wqe_index];
    failed.status = status;
    failed.terminalQpError = true;
    const bool loading_later = transport.sendLoading &&
        wqe_index + 1 < sendWqes.size();
    const bool retiring = !transport.sendLoading && !sendWqes.empty() &&
        transport.kind == pvrdma::QueueKind::Sq &&
        transport.consumer == sendWqes.front().consumer &&
        (transport.stage == TransportState::Stage::WaitSendCq ||
         transport.stage == TransportState::Stage::WriteSqConsumer);
    sendAbortLoading |= loading_later;
    uint64_t abandoned = 0;
    for (size_t i = wqe_index; i < sendWqes.size(); ++i)
        abandoned += sendWqes[i].sentSegments -
            sendWqes[i].ackedSegments;
    transportStats.txSegmentsAbandoned += abandoned;
    transportStats.txSegmentsInFlight =
        sendSegmentsInFlight() - abandoned;
    failed.sentSegments = failed.ackedSegments;
    if (!loading_later) {
        while (sendWqes.size() > wqe_index + 1) {
            auto &send = sendWqes.back();
            if (send.leaseHeld) {
                panic_if(!pvrdma::releaseMr(memoryRegions, send.lease),
                         "PVRDMA failed pipeline lost a later MR lease");
            }
            sendWqes.pop_back();
        }
    }
    // Active loading and retirement callbacks still own transport's DMA
    // identity and backing storage until their callback completes.
    if (!transport.sendLoading && !retiring)
        transport.reset();
    if (prefix_in_flight && !sendRnrWait && !sendRetryActive &&
        !transportTimerEvent.scheduled())
        armSendAckTimer();
    scheduleTransport();
}

void
Pvrdma::armSendAckTimer()
{
    if (sendRnrWait || sendRetryActive || !sendSegmentsInFlight() ||
        !sendQpHandle)
        return;
    const auto &qp = queuePairs.entries[sendQpHandle];
    const Tick delay = ackDelay(qp.attributes.timeout);
    if (delay)
        armTransportTimer(TransportTimerKind::Ack, delay);
}

bool
Pvrdma::selectSend()
{
    // ponytail: one device-wide QP flight; the window pipelines WQEs only
    // within that selected QP.
    if (transportWindowSegments > 1) {
        uint32_t handle = sendQpHandle;
        if (!handle) {
            const uint32_t first = observationCursor %
                pvrdma::ObjectTableEntries;
            for (uint32_t i = 0; i < pvrdma::ObjectTableEntries; ++i) {
                const uint32_t candidate = (first + i) %
                    pvrdma::ObjectTableEntries;
                if (!candidate)
                    continue;
                const auto &qp = queuePairs.entries[candidate];
                if (qp.valid && qp.state == pvrdma::QpState::ReadyToSend &&
                    qp.sqProducerTail != qp.sqConsumerHead) {
                    handle = sendQpHandle = candidate;
                    sqFetchHead[handle] = qp.sqConsumerHead;
                    sendNextPsn = qp.attributes.sendPsn;
                    observationCursor = (handle + 1) %
                        pvrdma::ObjectTableEntries;
                    break;
                }
            }
        }
        if (!handle)
            return false;
        auto &qp = queuePairs.entries[handle];
        if (!qp.valid || qp.state != pvrdma::QpState::ReadyToSend ||
            qp.qpHandle != handle) {
            if (!sendWqes.empty())
                failSendPipeline(0,
                    pvrdma::CompletionStatus::GeneralError);
            return false;
        }
        transport.reset();
        transport.kind = pvrdma::QueueKind::Sq;
        transport.qpHandle = handle;
        transport.qpGeneration = qp.generation;
        transport.cqHandle = qp.sendCqHandle;
        transport.cqGeneration = completionQueues.entries[
            qp.sendCqHandle].generation;
        transport.localMac = pvrdma::transportMac(
            regs, companionMacWordSwap);
        transport.remoteMac = storedMac(qp);
        transport.localQpn = qp.qpn;
        transport.remoteQpn = qp.attributes.destinationQpNumber;
        transport.livePsn = qp.attributes.sendPsn;
        if (!routeGids(qp, gids, gidValid, transport.localMac,
                       transport.localGid, transport.remoteGid) ||
            !pvrdma::validQpn(transport.localQpn) ||
            !pvrdma::validQpn(transport.remoteQpn)) {
            SendWqeState failed;
            failed.qpHandle = handle;
            failed.qpGeneration = qp.generation;
            failed.cqHandle = qp.sendCqHandle;
            failed.cqGeneration = transport.cqGeneration;
            failed.consumer = sqFetchHead[handle];
            failed.nextConsumer = pvrdma::ringAdvance(
                failed.consumer, qp.capabilities.maxSendWr);
            failed.wqe.kind = pvrdma::QueueKind::Sq;
            sendWqes.push_back(std::move(failed));
            sqFetchHead[handle] = sendWqes.back().nextConsumer;
            transport.reset();
            failSendPipeline(sendWqes.size() - 1,
                pvrdma::CompletionStatus::GeneralError);
            return true;
        }
        if (startSendLoad())
            return true;
        transport.reset();
        return false;
    }

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
        transport.localMac = pvrdma::transportMac(
            regs, companionMacWordSwap);
        transport.remoteMac = storedMac(qp);
        transport.localQpn = qp.qpn;
        transport.remoteQpn = qp.attributes.destinationQpNumber;
        transport.psn = transport.initialPsn = qp.attributes.sendPsn;
        transport.livePsn = transport.psn;
        transport.retryRemaining = qp.attributes.retryCount;
        transport.rnrRetryRemaining = qp.attributes.rnrRetry;
        observationCursor = (handle + 1) % pvrdma::ObjectTableEntries;
        if (!routeGids(qp, gids, gidValid, transport.localMac,
                       transport.localGid, transport.remoteGid) ||
            !pvrdma::validQpn(transport.localQpn) ||
            !pvrdma::validQpn(transport.remoteQpn) ||
            !pvrdma::wqeAddress(qp, pvrdma::QueueKind::Sq,
                                transport.consumer,
                                transport.wqeAddress)) {
            failSend(pvrdma::CompletionStatus::GeneralError, true);
            return true;
        }
        startTransportDma(false, transport.wqeAddress,
                          transport.sqSlot.size(), transport.sqSlot.data());
        return true;
    }
    return false;
}

void
Pvrdma::queueReverseError(const pvrdma::rocev2::Packet &received,
                          pvrdma::rocev2::Syndrome syndrome,
                          uint32_t response_psn)
{
    using namespace pvrdma::rocev2;
    if (pendingErrorPacket || received.opcode == Opcode::Acknowledge)
        return;
    auto *qp = pvrdma::findQueuePair(queuePairs, received.destinationQpn);
    if (!qp)
        return;
    Gid local_gid{}, remote_gid{};
    const auto local_mac = pvrdma::transportMac(regs, companionMacWordSwap);
    if (!routeGids(*qp, gids, gidValid, local_mac, local_gid, remote_gid) ||
        !routeMatches(*qp, received, local_mac, local_gid, remote_gid))
        return;
    pvrdma::rocev2::Packet error;
    error.destinationMac = storedMac(*qp);
    error.sourceMac = local_mac;
    error.destinationGid = received.sourceGid;
    error.sourceGid = local_gid;
    error.opcode = Opcode::Acknowledge;
    error.ackRequest = false;
    error.destinationQpn = qp->attributes.destinationQpNumber;
    error.psn = response_psn;
    const auto &route = qp->attributes.addressHandle.globalRoute;
    error.trafficClass = route.trafficClass;
    error.flowLabel = route.flowLabel;
    error.hopLimit = route.hopLimit ? route.hopLimit : 0xff;
    error.sourcePort = udpSourcePort(error.flowLabel, qp->qpn,
                                     error.destinationQpn);
    error.syndrome = syndrome;
    error.ackCredit = 31;
    error.rnrTimer = syndrome == Syndrome::Rnr ?
        qp->attributes.minRnrTimer : 0;
    error.msn = qp->responderMsn;
    pendingErrorPacket = std::make_shared<EthPacketData>(ControlFrameSize);
    const auto encoded = encode(error, {pendingErrorPacket->data,
                                        pendingErrorPacket->bufLength});
    if (!encoded) {
        pendingErrorPacket.reset();
        return;
    }
    pendingErrorPacket->length = pendingErrorPacket->simLength = encoded.size;
}

void
Pvrdma::startInbound()
{
    using namespace pvrdma::rocev2;
    if (!pendingRxPacket || transport.active())
        return;
    const EthPacketPtr packet = popReceivePacket();
    const auto decoded = decode({packet->data, packet->bufLength},
                                packet->length);
    if (!decoded || decoded.packet.opcode == Opcode::Acknowledge)
        return;
    const auto &frame = decoded.packet;
    if (replayFinal(frame)) {
        transportStats.rxDuplicateDataFrames++;
        scheduleTransport();
        return;
    }
    auto *qp = pvrdma::findQueuePair(queuePairs, frame.destinationQpn);
    if (!qp)
        return;
    const auto local_mac = pvrdma::transportMac(regs, companionMacWordSwap);
    Gid local_gid{}, remote_gid{};
    if (!routeGids(*qp, gids, gidValid, local_mac, local_gid, remote_gid) ||
        !routeMatches(*qp, frame, local_mac, local_gid, remote_gid)) {
        queueReverseError(frame, Syndrome::InvalidRequestNak, frame.psn);
        scheduleTransport();
        return;
    }
    if (!pvrdma::startingOpcode(frame.opcode)) {
        queueReverseError(frame, Syndrome::InvalidRequestNak,
                          qp->attributes.receivePsn);
        scheduleTransport();
        return;
    }
    if (frame.psn != qp->attributes.receivePsn) {
        const uint32_t distance = pvrdma::psnDistance(
            qp->attributes.receivePsn, frame.psn);
        if (distance < pvrdma::PsnHalfRange)
            transportStats.rxOutOfOrderDataFrames++;
        else
            transportStats.rxDuplicateDataFrames++;
        queueReverseError(frame, Syndrome::SequenceNak,
                          qp->attributes.receivePsn);
        scheduleTransport();
        return;
    }
    if (qp->rqProducerTail == qp->rqConsumerHead) {
        queueReverseError(frame, Syndrome::Rnr, frame.psn);
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
    transport.localMac = local_mac;
    transport.remoteMac = storedMac(*qp);
    transport.localGid = local_gid;
    transport.remoteGid = remote_gid;
    transport.localQpn = qp->qpn;
    transport.remoteQpn = qp->attributes.destinationQpNumber;
    transport.psn = transport.initialPsn = frame.psn;
    transport.livePsn = qp->attributes.receivePsn;
    transport.opcode = frame.opcode;
    transport.totalLength = frame.payload.size;
    transport.payload.assign(frame.payload.data,
                             frame.payload.data + frame.payload.size);
    transport.stage = TransportState::Stage::ReadRqWqe;
    if (!pvrdma::wqeAddress(*qp, pvrdma::QueueKind::Rq,
                            transport.consumer, transport.wqeAddress)) {
        prepareControl(Syndrome::RemoteOperationNak, frame.psn);
        return;
    }
    startTransportDma(false, transport.wqeAddress,
                      transport.rqSlot.size(), transport.rqSlot.data());
}

void
Pvrdma::startTransportDma(bool write, uint64_t address, size_t size,
                          uint8_t *data, Request::Flags flags)
{
    panic_if(transport.dmaBusy || dmaPending(),
             "PVRDMA overlapping transport DMA");
    assert(!flags || (write ? Request::isValidNicDmaWriteFlags(flags) :
                              Request::isValidNicDmaReadFlags(flags)));
    transport.dmaBusy = true;
    dmaPort.dmaAction(write ? MemCmd::WriteReq : MemCmd::ReadReq,
                      pciToDma(address), size,
                      sys->isAtomicMode() ? nullptr : &transportDmaEvent,
                      data, 0, flags);
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
        if (transport.kind == pvrdma::QueueKind::Sq) {
            failSend(pvrdma::CompletionStatus::GeneralError, true);
        } else {
            failReceive(pvrdma::CompletionStatus::LocalQpOperationError,
                        true,
                        pvrdma::rocev2::Syndrome::RemoteOperationNak);
        }
        scheduleTransport();
        return;
    }
    const auto &chunk = transport.lease.chunks[transport.chunkIndex];
    transport.dmaChunkLength = std::min(
        transport.dmaRemaining, chunk.length - transport.chunkOffset);
    startTransportDma(write, chunk.address + transport.chunkOffset,
                      transport.dmaChunkLength,
                      transport.payload.data() + transport.dmaPayloadOffset,
                      pvrdma::payloadDmaFlags(write));
}

void
Pvrdma::failSend(pvrdma::CompletionStatus status, bool qp_error)
{
    if (transportWindowSegments == 1 && sendSegmentsInFlight()) {
        transportStats.txSegmentsAbandoned++;
        transportStats.txSegmentsInFlight = 0;
    }
    cancelTransportTimer();
    transport.status = status;
    transport.packet.reset();
    transport.completionBackpressured = false;
    transport.keepAfterControl = false;
    transport.retryPending = false;
    transport.terminalQpError = qp_error;
    transport.stage = TransportState::Stage::WaitSendCq;
}

void
Pvrdma::failReceive(pvrdma::CompletionStatus status, bool send_nak,
                    pvrdma::rocev2::Syndrome syndrome)
{
    if (!transport.rqWqeSelected) {
        if (send_nak)
            prepareControl(syndrome, transport.livePsn);
        else
            finishTransport();
        return;
    }
    cancelTransportTimer();
    transport.status = status;
    transport.packet.reset();
    transport.completionBackpressured = false;
    transport.keepAfterControl = false;
    transport.retryPending = false;
    transport.terminalQpError = true;
    if (transport.qpHandle &&
        transport.qpHandle < pvrdma::ObjectTableEntries)
        queuePairs.entries[transport.qpHandle].finalReplay = {};
    if (send_nak) {
        prepareControl(syndrome, transport.livePsn);
    } else {
        transport.stage = TransportState::Stage::WaitReceiveCq;
        scheduleTransport();
    }
}

void
Pvrdma::prepareControl(pvrdma::rocev2::Syndrome syndrome,
                       uint32_t response_psn)
{
    transport.responseSyndrome = syndrome;
    transport.psn = response_psn;
    transport.packet.reset();
    if (transport.qpHandle &&
        transport.qpHandle < pvrdma::ObjectTableEntries) {
        transport.responseMsn = queuePairs.entries[
            transport.qpHandle].responderMsn;
    }
    transport.stage = syndrome == pvrdma::rocev2::Syndrome::Ack ?
        TransportState::Stage::TryAck :
        syndrome == pvrdma::rocev2::Syndrome::Rnr ?
            TransportState::Stage::TryRnr :
            TransportState::Stage::TryError;
    scheduleTransport();
}

bool
Pvrdma::tryTransportPacket()
{
    using namespace pvrdma::rocev2;
    const bool data = transport.stage == TransportState::Stage::TryData;
    if (data && !revalidateTransportLease()) {
        failSend(pvrdma::CompletionStatus::GeneralError, true);
        scheduleTransport();
        return false;
    }
    if ((transport.stage == TransportState::Stage::TryAck ||
         transport.stage == TransportState::Stage::TryError) &&
        transport.leaseHeld && !transport.terminalQpError &&
        !revalidateTransportLease()) {
        failReceive(pvrdma::CompletionStatus::LocalQpOperationError,
                    true, Syndrome::RemoteOperationNak);
        return false;
    }
    if (transport.stage == TransportState::Stage::TryRnr &&
        !revalidateTransport()) {
        finishTransport();
        return false;
    }
    const auto fail_control = [this] {
        transport.packet.reset();
        if (transport.kind == pvrdma::QueueKind::Rq &&
            transport.terminalQpError) {
            transport.stage = TransportState::Stage::WaitReceiveCq;
        } else if (transport.kind == pvrdma::QueueKind::Rq &&
                   transport.rqWqeSelected && transport.keepAfterControl) {
            failReceive(pvrdma::CompletionStatus::LocalQpOperationError);
        } else {
            finishTransport();
        }
    };
    if (!transport.packet) {
        pvrdma::rocev2::Packet frame;
        frame.destinationMac = transport.remoteMac;
        frame.sourceMac = transport.localMac;
        frame.destinationGid = transport.remoteGid;
        frame.sourceGid = transport.localGid;
        frame.destinationQpn = transport.remoteQpn;
        frame.psn = transport.psn;
        frame.trafficClass = queuePairs.entries[
            transport.qpHandle].attributes.addressHandle.globalRoute.
                trafficClass;
        frame.flowLabel = queuePairs.entries[
            transport.qpHandle].attributes.addressHandle.globalRoute.
                flowLabel;
        frame.hopLimit = queuePairs.entries[
            transport.qpHandle].attributes.addressHandle.globalRoute.
                hopLimit;
        if (!frame.hopLimit)
            frame.hopLimit = 0xff;
        frame.sourcePort = udpSourcePort(frame.flowLabel,
                                         transport.localQpn,
                                         transport.remoteQpn);
        if (data) {
            const size_t offset = size_t{transport.segmentIndex} *
                pvrdma::FixedMtu;
            const size_t length = transport.totalLength == 0 ? 0 :
                std::min<size_t>(pvrdma::FixedMtu,
                                 transport.totalLength - offset);
            frame.opcode = pvrdma::sendOpcode(transport.segmentIndex,
                                               transport.segmentCount);
            frame.ackRequest = true;
            frame.payload = {length ? transport.payload.data() + offset :
                                      nullptr,
                             length};
        } else {
            frame.opcode = Opcode::Acknowledge;
            frame.ackRequest = false;
            frame.syndrome = transport.responseSyndrome;
            frame.ackCredit = 31;
            if (frame.syndrome == Syndrome::Rnr) {
                frame.rnrTimer = queuePairs.entries[
                    transport.qpHandle].attributes.minRnrTimer;
            }
            frame.msn = transport.responseMsn;
        }
        const size_t size = encodedSize(frame);
        transport.packet = std::make_shared<EthPacketData>(size);
        const auto encoded = encode(
            frame, {transport.packet->data, transport.packet->bufLength});
        if (!encoded) {
            if (data)
                failSend(pvrdma::CompletionStatus::GeneralError, true);
            else
                fail_control();
            scheduleTransport();
            return false;
        }
        transport.packet->length = encoded.size;
        transport.packet->simLength = encoded.size;
    }
    if (!interface.getPeer()) {
        if (data)
            failSend(pvrdma::CompletionStatus::GeneralError, true);
        else
            fail_control();
        scheduleTransport();
        return false;
    }
    const size_t payload_bytes = data && transport.totalLength ?
        std::min<size_t>(pvrdma::FixedMtu,
            transport.totalLength - size_t{transport.segmentIndex} *
                pvrdma::FixedMtu) : 0;
    const bool retry = transport.retryPending;
    if (!sendAccepted(transport.packet,
                      data ? TxFrameClass::Data : TxFrameClass::Control,
                      payload_bytes, retry))
        return false;
    transport.packet.reset();
    if (data) {
        if (!retry) {
            transportStats.txSegmentsInFlight++;
            if (transportStats.txSegmentsInFlight.value() >
                transportStats.txMaxSegmentsInFlight.value())
                transportStats.txMaxSegmentsInFlight =
                    transportStats.txSegmentsInFlight.value();
        }
        transport.retryPending = false;
        transport.stage = TransportState::Stage::WaitResponse;
        const auto &qp = queuePairs.entries[transport.qpHandle];
        const Tick delay = ackDelay(qp.attributes.timeout);
        if (delay)
            armTransportTimer(TransportTimerKind::Ack, delay);
    } else if (transport.kind == pvrdma::QueueKind::Rq &&
               transport.terminalQpError) {
        transport.stage = TransportState::Stage::WaitReceiveCq;
        scheduleTransport();
    } else if (transport.keepAfterControl) {
        transport.keepAfterControl = false;
        transport.stage = TransportState::Stage::WaitReceiveData;
        if (transport.kind == pvrdma::QueueKind::Rq &&
            transport.hasAcceptedSegment &&
            !pvrdma::finalOpcode(transport.acceptedOpcode)) {
            armTransportTimer(TransportTimerKind::ReceiveIdle,
                              ackDelay(ReceiveIdleTimeoutEncoding));
        }
        scheduleTransport();
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
            prepareControl(pvrdma::rocev2::Syndrome::RemoteOperationNak,
                           transport.livePsn);
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
        record.byteLength = transport.totalLength;
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
        (transport.leaseHeld && !transport.terminalQpError &&
         !revalidateTransportLease())) {
        if (kind == pvrdma::QueueKind::Sq)
            finishTransport();
        else
            prepareControl(pvrdma::rocev2::Syndrome::RemoteOperationNak,
                           transport.livePsn);
        return;
    }
    uint64_t address = 0;
    if (!pvrdma::queueConsumerAddress(
            queuePairs.entries[transport.qpHandle], kind, address)) {
        if (kind == pvrdma::QueueKind::Sq)
            failSend();
        else
            prepareControl(pvrdma::rocev2::Syndrome::RemoteOperationNak,
                           transport.livePsn);
        return;
    }
    startTransportDma(true, address, sizeof(transport.consumerLe),
                      reinterpret_cast<uint8_t *>(&transport.consumerLe));
}

void
Pvrdma::finishTransport(bool release_lease)
{
    if (!(transportWindowSegments > 1 && sendQpHandle &&
          (sendRnrWait || sendRetryActive || sendSegmentsInFlight())))
        cancelTransportTimer();
    if (release_lease && transport.leaseHeld) {
        if (!pvrdma::releaseMr(memoryRegions, transport.lease))
            operationError.set(regs.error, pvrdma::CommandError);
        transport.leaseHeld = false;
    }
    transport.reset();
    scheduleObservation();
    scheduleTransport();
    if (pvrdma::checkpointStable(controlState, dmaPending(),
                                 observationQueued(), queueDma.active(),
                                 completionBusy(), transportActive(),
                                 runnableSq(), activeMr()))
        signalDrainDone();
}

void
Pvrdma::clearSendPipeline(bool count_discarded)
{
    if (sendEvent.scheduled())
        deschedule(sendEvent);
    if (count_discarded) {
        const uint64_t abandoned = sendSegmentsInFlight();
        transportStats.txSegmentsAbandoned += abandoned;
        transportStats.txSegmentsInFlight = 0;
    }
    for (auto &send : sendWqes) {
        if (send.leaseHeld) {
            panic_if(!pvrdma::releaseMr(memoryRegions, send.lease),
                     "PVRDMA cleanup lost an MR lease");
        }
    }
    sendWqes.clear();
    sqFetchHead = {};
    sendQpHandle = 0;
    sendNextPsn = sendRetryPsn = sendRetryEndPsn = 0;
    sendRetryActive = sendRnrWait = sendPipelineFailed = false;
    sendAbortLoading = false;
    sendPacket.reset();
}

void
Pvrdma::clearTransport()
{
    if (transportEvent.scheduled())
        deschedule(transportEvent);
    cancelTransportTimer();
    panic_if(transport.dmaBusy,
             "PVRDMA cleared an active transport DMA");
    if (transportWindowSegments > 1)
        clearSendPipeline();
    else
        panic_if(transport.leaseHeld,
                 "PVRDMA cleared active transport resources");
    transport.reset();
    const uint64_t discarded = receiveBacklog();
    transportStats.rxSegmentsDiscarded += discarded;
    transportStats.rxQueuedSegments = 0;
    pendingRxPacket.reset();
    pendingRxPackets.clear();
    pendingErrorPacket.reset();
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
    if (transportWindowSegments > 1 && sendQpHandle) {
        auto &qp = queuePairs.entries[sendQpHandle];
        if (kind == TransportTimerKind::Rnr && sendRnrWait) {
            sendRnrWait = false;
            sendRetryActive = sendRetryPsn != sendRetryEndPsn;
            scheduleSend();
        } else if (kind == TransportTimerKind::Ack &&
                   sendSegmentsInFlight()) {
            size_t wqe_index = 0;
            uint16_t segment_index = 0;
            if (!findSendSegment(qp.attributes.sendPsn, wqe_index,
                                 segment_index)) {
                failSendPipeline(0,
                    pvrdma::CompletionStatus::GeneralError);
            } else if (!pvrdma::useRetry(
                           sendWqes[wqe_index].retryRemaining)) {
                failSendPipeline(wqe_index,
                    pvrdma::CompletionStatus::RetryExceededError);
            } else {
                sendRetryPsn = qp.attributes.sendPsn;
                sendRetryEndPsn = (qp.attributes.sendPsn +
                    sendSegmentsInFlight()) &
                    pvrdma::rocev2::Field24Mask;
                sendRetryActive = true;
                scheduleSend();
            }
        }
        return;
    }
    if (!transport.active())
        return;
    if (transport.kind == pvrdma::QueueKind::Rq) {
        if (kind == TransportTimerKind::ReceiveIdle &&
            transport.stage == TransportState::Stage::WaitReceiveData) {
            failReceive(pvrdma::CompletionStatus::LocalQpOperationError);
        }
        return;
    }
    if (transport.kind != pvrdma::QueueKind::Sq)
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
        } else {
            failSend(pvrdma::CompletionStatus::RetryExceededError, true);
        }
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
        if (!interface.getPeer()) {
            pendingErrorPacket.reset();
            scheduleTransport();
            if (pvrdma::checkpointStable(
                    controlState, dmaPending(), observationQueued(),
                    queueDma.active(), completionBusy(), transportActive(),
                    runnableSq(), activeMr()))
                signalDrainDone();
        } else if (sendAccepted(pendingErrorPacket,
                                TxFrameClass::Control)) {
            pendingErrorPacket.reset();
        }
        return;
    }
    if (transportWindowSegments > 1 && sendQpHandle) {
        const bool retiring = !transport.sendLoading && !sendWqes.empty() &&
            transport.kind == pvrdma::QueueKind::Sq &&
            transport.consumer == sendWqes.front().consumer &&
            (transport.stage == TransportState::Stage::WaitSendCq ||
             transport.stage == TransportState::Stage::WriteSqConsumer);
        if (!retiring) {
            if (!sendWqes.empty() && sendWqes.front().readyToRetire()) {
                startSendRetirement();
                return;
            }
            scheduleSend();
            // A send event queued before retirement cannot refill an empty
            // pipeline: runSend() returns immediately when no WQEs remain.
            if (!transport.sendLoading && !sendPipelineFailed &&
                sendWqes.size() < 2 &&
                (sendWqes.empty() || txBusyClass != TxFrameClass::None ||
                 !sendEvent.scheduled())) {
                if (transport.kind == pvrdma::QueueKind::Sq)
                    startSendLoad();
                else
                    selectSend();
            }
            return;
        }
    }
    if (!transport.active()) {
        if (pendingRxPacket)
            startInbound();
        else
            selectSend();
        return;
    }
    if (pendingRxPacket && transport.kind == pvrdma::QueueKind::Rq &&
        transport.stage == TransportState::Stage::WaitReceiveData) {
        const auto packet = popReceivePacket();
        const auto decoded = pvrdma::rocev2::decode(
            {packet->data, packet->bufLength}, packet->length);
        if (decoded)
            handleInboundContinuation(decoded.packet);
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
    using Stage = TransportState::Stage;
    if (transportWindowSegments > 1 && sendQpHandle &&
        transport.kind == pvrdma::QueueKind::Sq && transport.sendLoading) {
        panic_if(sendWqes.empty(),
                 "PVRDMA pipeline DMA completed without a WQE");
        auto &send = sendWqes.back();
        if (sendAbortLoading) {
            if (send.leaseHeld) {
                panic_if(!pvrdma::releaseMr(memoryRegions, send.lease),
                         "PVRDMA aborted load lost its MR lease");
            }
            sendWqes.pop_back();
            sendAbortLoading = false;
            transport.reset();
            scheduleTransport();
            return;
        }
        auto &qp = queuePairs.entries[send.qpHandle];
        if (transport.stage == Stage::ReadSqWqe) {
            pvrdma::SendWqeHeader header{};
            std::memcpy(&header, send.sqSlot.data(), sizeof(header));
            send.wqe.workRequestId = letoh(header.workRequestId);
            if (!qp.valid || qp.qpHandle != send.qpHandle ||
                qp.generation != send.qpGeneration ||
                qp.state != pvrdma::QpState::ReadyToSend ||
                qp.sendCqHandle != send.cqHandle ||
                !pvrdma::decodeSqWqe(qp, send.sqSlot, send.wqe) ||
                !pvrdma::segmentGeometry(send.wqe.length,
                                         send.segmentCount) ||
                !pvrdma::acquireLocalMr(
                    memoryRegions, qp, pvrdma::QueueKind::Sq,
                    send.wqe.lkey, send.wqe.address, send.wqe.length,
                    send.lease)) {
                finishSendLoad(pvrdma::CompletionStatus::GeneralError);
                return;
            }
            send.leaseHeld = true;
            send.initialPsn = sendNextPsn;
            sendNextPsn = (sendNextPsn + send.segmentCount) &
                pvrdma::rocev2::Field24Mask;
            send.payload.resize(send.wqe.length);
            if (!send.wqe.length) {
                finishSendLoad();
                return;
            }
            if (!pvrdma::leaseRange(send.lease, 0, send.wqe.length,
                                    send.chunkIndex, send.chunkOffset)) {
                finishSendLoad(pvrdma::CompletionStatus::GeneralError);
                return;
            }
            send.dmaPayloadOffset = 0;
            send.dmaRemaining = send.wqe.length;
        } else if (transport.stage == Stage::ReadSqPayload) {
            if (!revalidateSendWqe(send)) {
                finishSendLoad(pvrdma::CompletionStatus::GeneralError);
                return;
            }
            send.dmaPayloadOffset += send.dmaChunkLength;
            send.dmaRemaining -= send.dmaChunkLength;
            send.chunkOffset += send.dmaChunkLength;
            if (send.chunkOffset ==
                send.lease.chunks[send.chunkIndex].length) {
                ++send.chunkIndex;
                send.chunkOffset = 0;
            }
            if (!send.dmaRemaining) {
                finishSendLoad();
                return;
            }
        } else {
            panic("Invalid PVRDMA pipeline DMA stage");
        }
        if (!revalidateSendWqe(send) ||
            send.chunkIndex >= send.lease.chunks.size()) {
            finishSendLoad(pvrdma::CompletionStatus::GeneralError);
            return;
        }
        const auto &chunk = send.lease.chunks[send.chunkIndex];
        send.dmaChunkLength = std::min(
            send.dmaRemaining, chunk.length - send.chunkOffset);
        transport.stage = Stage::ReadSqPayload;
        startTransportDma(false, chunk.address + send.chunkOffset,
                          send.dmaChunkLength,
                          send.payload.data() + send.dmaPayloadOffset,
                          pvrdma::payloadDmaFlags(false));
        return;
    }
    const auto receive_segment_done = [this] {
        auto &qp = queuePairs.entries[transport.qpHandle];
        qp.attributes.receivePsn = pvrdma::advancePsn(
            qp.attributes.receivePsn);
        transport.livePsn = qp.attributes.receivePsn;
        transport.acceptedPsn = transport.psn;
        transport.acceptedOpcode = transport.opcode;
        transport.acceptedSegmentLength = transport.totalLength -
            size_t{transport.segmentIndex} * pvrdma::FixedMtu;
        transportStats.rxNewDataSegmentsCommitted++;
        transportStats.rxNewDataPayloadBytesCommitted +=
            transport.acceptedSegmentLength;
        transport.acceptedSegmentIndex = transport.segmentIndex;
        transport.hasAcceptedSegment = true;
        if (pvrdma::finalOpcode(transport.opcode)) {
            transport.stage = Stage::WaitReceiveCq;
        } else {
            transport.keepAfterControl = true;
            prepareControl(pvrdma::rocev2::Syndrome::Ack,
                           transport.acceptedPsn);
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
            failSend(pvrdma::CompletionStatus::GeneralError, true);
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
            failSend(pvrdma::CompletionStatus::GeneralError, true);
        else
            return;
        break;
      }
      case Stage::ReadSqPayload:
        if (!revalidateTransportLease()) {
            failSend(pvrdma::CompletionStatus::GeneralError, true);
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
            prepareControl(pvrdma::rocev2::Syndrome::RemoteOperationNak,
                           transport.livePsn);
            break;
        }
        auto &qp = queuePairs.entries[transport.qpHandle];
        if (!pvrdma::decodeRqWqe(qp, transport.rqSlot, transport.wqe)) {
            prepareControl(pvrdma::rocev2::Syndrome::InvalidRequestNak,
                           transport.livePsn);
            break;
        }
        transport.rqWqeSelected = true;
        if (transport.wqe.length < transport.totalLength) {
            failReceive(pvrdma::CompletionStatus::LocalLengthError, true,
                        pvrdma::rocev2::Syndrome::InvalidRequestNak);
            break;
        }
        if (!pvrdma::acquireLocalMr(
                memoryRegions, qp, pvrdma::QueueKind::Rq,
                transport.wqe.lkey, transport.wqe.address,
                transport.wqe.length, transport.lease)) {
            failReceive(pvrdma::CompletionStatus::LocalProtectionError,
                        true,
                        pvrdma::rocev2::Syndrome::RemoteAccessNak);
            break;
        }
        transport.leaseHeld = true;
        if (!transport.totalLength) {
            receive_segment_done();
            break;
        }
        if (!beginPayloadDma(true, 0, transport.totalLength)) {
            failReceive(pvrdma::CompletionStatus::LocalQpOperationError,
                        true,
                        pvrdma::rocev2::Syndrome::RemoteOperationNak);
            break;
        }
        return;
      }
      case Stage::WriteRqPayload:
        if (!revalidateTransportLease()) {
            failReceive(pvrdma::CompletionStatus::LocalQpOperationError,
                        true,
                        pvrdma::rocev2::Syndrome::RemoteOperationNak);
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
        const bool pipeline = transportWindowSegments > 1 &&
            sendQpHandle == transport.qpHandle && !sendWqes.empty() &&
            sendWqes.front().nextConsumer == transport.consumer;
        const bool terminal = transport.terminalQpError;
        finishTransport();
        if (pipeline) {
            sendWqes.pop_front();
            if (terminal) {
                sendPipelineFailed = false;
                sendQpHandle = 0;
            } else if (sendWqes.empty() &&
                       sqFetchHead[qp.qpHandle] == qp.sqProducerTail) {
                sendQpHandle = 0;
                sendNextPsn = qp.attributes.sendPsn;
            }
            scheduleSend();
            scheduleTransport();
        }
        return;
      }
      case Stage::WriteRqConsumer: {
        panic_if(!revalidateTransport() ||
                     (!transport.terminalQpError &&
                      !revalidateTransportLease()),
                 "PVRDMA RQ changed during consumer publication");
        sampleQueueOccupancy();
        auto &qp = queuePairs.entries[transport.qpHandle];
        qp.rqConsumerHead = transport.nextConsumer;
        transport.consumer = transport.nextConsumer;
        queueStats.rqConsumed++;
        if (transport.terminalQpError) {
            qp.state = pvrdma::QpState::Error;
            qp.attributes.qpState = qp.attributes.currentQpState = qp.state;
            qp.finalReplay = {};
            refreshQueueGauges();
            sampleCurrentQueueOccupancy();
            checkQueueConservation();
            finishTransport();
            return;
        }
        qp.responderMsn = pvrdma::advancePsn(qp.responderMsn);
        transport.responseMsn = qp.responderMsn;
        qp.finalReplay = {true, qp.generation, transport.localMac,
                          transport.remoteMac, transport.localGid,
                          transport.remoteGid, transport.localQpn,
                          transport.remoteQpn, transport.acceptedPsn,
                          transport.acceptedOpcode,
                          transport.acceptedSegmentLength,
                          qp.responderMsn};
        refreshQueueGauges();
        sampleCurrentQueueOccupancy();
        checkQueueConservation();
        transport.keepAfterControl = false;
        prepareControl(pvrdma::rocev2::Syndrome::Ack,
                       transport.acceptedPsn);
        transport.responseMsn = qp.responderMsn;
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
        const bool cancel_terminal = terminalCompletionBackpressured();
        if (!pvrdma::stable(controlState) || queueDma.active() ||
            completionBusy() || dmaPending() ||
            (transportActive() && !cancel_terminal)) {
            operationError.set(regs.error, pvrdma::CommandError);
            return;
        }
        if (cancel_terminal && transport.leaseHeld) {
            panic_if(!pvrdma::releaseMr(memoryRegions, transport.lease),
                     "PVRDMA terminal reset lost its MR lease");
            transport.leaseHeld = false;
            if (transportWindowSegments > 1 && !sendWqes.empty() &&
                sendWqes.front().consumer == transport.consumer)
                sendWqes.front().leaseHeld = false;
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
    if (value != 0 || completionBusy() || transportActive() ||
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
    if (terminalCompletionBackpressured()) {
        // Drain drops an unpublished terminal error CQE for either queue so
        // the one-flight model can checkpoint; successes still wait for space.
        transport.completionBackpressured = false;
        transport.stage = transport.kind == pvrdma::QueueKind::Rq ?
            TransportState::Stage::WriteRqConsumer :
            TransportState::Stage::WriteSqConsumer;
    }
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
        paramOut(cp, "responderMsn", qp.responderMsn);
        paramOut(cp, "replayValid", qp.finalReplay.valid);
        paramOut(cp, "replayQpGeneration", qp.finalReplay.qpGeneration);
        arrayParamOut(cp, "replayLocalMac", qp.finalReplay.localMac.data(),
                      qp.finalReplay.localMac.size());
        arrayParamOut(cp, "replayRemoteMac", qp.finalReplay.remoteMac.data(),
                      qp.finalReplay.remoteMac.size());
        arrayParamOut(cp, "replayLocalGid", qp.finalReplay.localGid.data(),
                      qp.finalReplay.localGid.size());
        arrayParamOut(cp, "replayRemoteGid", qp.finalReplay.remoteGid.data(),
                      qp.finalReplay.remoteGid.size());
        paramOut(cp, "replayLocalQpn", qp.finalReplay.localQpn);
        paramOut(cp, "replayRemoteQpn", qp.finalReplay.remoteQpn);
        paramOut(cp, "replayFinalPsn", qp.finalReplay.finalPsn);
        paramOut(cp, "replayFinalOpcode",
                 static_cast<uint32_t>(qp.finalReplay.finalOpcode));
        paramOut(cp, "replayFinalSegmentLength",
                 qp.finalReplay.finalSegmentLength);
        paramOut(cp, "replayCompletedMsn", qp.finalReplay.completedMsn);
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
    panic_if(capabilities.gidTypes != pvrdma::GidTypeRoceV2,
             "PVRDMA checkpoint transport is not RoCEv2");
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
        paramIn(cp, "responderMsn", qp.responderMsn);
        paramIn(cp, "replayValid", qp.finalReplay.valid);
        paramIn(cp, "replayQpGeneration", qp.finalReplay.qpGeneration);
        arrayParamIn(cp, "replayLocalMac", qp.finalReplay.localMac.data(),
                     qp.finalReplay.localMac.size());
        arrayParamIn(cp, "replayRemoteMac", qp.finalReplay.remoteMac.data(),
                     qp.finalReplay.remoteMac.size());
        arrayParamIn(cp, "replayLocalGid", qp.finalReplay.localGid.data(),
                     qp.finalReplay.localGid.size());
        arrayParamIn(cp, "replayRemoteGid", qp.finalReplay.remoteGid.data(),
                     qp.finalReplay.remoteGid.size());
        paramIn(cp, "replayLocalQpn", qp.finalReplay.localQpn);
        paramIn(cp, "replayRemoteQpn", qp.finalReplay.remoteQpn);
        paramIn(cp, "replayFinalPsn", qp.finalReplay.finalPsn);
        uint32_t replay_opcode = 0;
        paramIn(cp, "replayFinalOpcode", replay_opcode);
        qp.finalReplay.finalOpcode =
            static_cast<pvrdma::rocev2::Opcode>(replay_opcode);
        paramIn(cp, "replayFinalSegmentLength",
                qp.finalReplay.finalSegmentLength);
        paramIn(cp, "replayCompletedMsn", qp.finalReplay.completedMsn);
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
