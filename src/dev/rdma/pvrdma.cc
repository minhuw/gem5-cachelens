// SPDX-License-Identifier: BSD-3-Clause

#include "dev/rdma/pvrdma.hh"

#include "base/logging.hh"
#include "mem/packet_access.hh"
#include "sim/serialize.hh"

namespace gem5
{

Pvrdma::Pvrdma(const Params &p)
    : PciDevice(p), controlCompletionLatency(p.control_completion_latency),
      dsrReadEvent([this] { dsrReadDone(); }, name() + ".dsrRead"),
      capsWriteEvent([this] { capsWriteDone(); }, name() + ".capsWrite"),
      commandReadEvent([this] { commandReadDone(); }, name() + ".commandRead"),
      objectDirectoryReadEvent([this] { objectDirectoryReadDone(); },
                               name() + ".objectDirectoryRead"),
      objectTableReadEvent([this] { objectTableReadDone(); },
                           name() + ".objectTableRead"),
      responseWriteEvent([this] { responseWriteDone(); },
                         name() + ".responseWrite")
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
}

Tick
Pvrdma::read(PacketPtr pkt)
{
    int bar;
    Addr offset;
    panic_if(!getBAR(pkt->getAddr(), bar, offset),
             "PVRDMA read from unmapped PCI address %#x", pkt->getAddr());
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
        regs.macLow = value;
        break;
      case pvrdma::Register::MacHigh:
        regs.writeMacHigh(value);
        break;
      default:
        panic("Invalid writable PVRDMA register at offset %#x", offset);
    }

    pkt->makeAtomicResponse();
    return delay;
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
        if (!pvrdma::stable(controlState)) {
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
    if (value != 0 || !pvrdma::beginCommand(controlState)) {
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

    const auto result = pvrdma::processCommand(
        command, response, gids, gidValid, objects, memoryRegions,
        completionQueues, queuePairs,
        {BARs[pvrdma::UarBar]->addr(), BARs[pvrdma::UarBar]->size()});
    operationError.complete(regs.error, result.error);
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
    if (pvrdma::checkpointStable(controlState, dmaPending()))
        signalDrainDone();
}

DrainState
Pvrdma::drain()
{
    return pvrdma::checkpointStable(controlState, dmaPending()) ?
        DrainState::Drained : DrainState::Draining;
}

void
Pvrdma::serialize(CheckpointOut &cp) const
{
    panic_if(!pvrdma::checkpointStable(controlState, dmaPending()),
             "Cannot checkpoint PVRDMA with active control DMA");
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
        paramOut(cp, "activeReferences", mr.activeReferences);
        paramOut(cp, "start", mr.start);
        paramOut(cp, "length", mr.length);
        paramOut(cp, "end", mr.end);
        arrayParamOut(cp, "pages", mr.pages);
    }
    for (uint32_t slot = 1; slot < pvrdma::ObjectTableEntries; ++slot) {
        const auto &cq = completionQueues.entries[slot];
        ScopedCheckpointSection sec(cp, csprintf("cq%u", slot));
        paramOut(cp, "valid", cq.valid);
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
        paramIn(cp, "activeReferences", mr.activeReferences);
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
        arrayParamIn(cp, "pages", qp.pages);
    }
    operationError.reset();

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
    // The interrupt controller restores its own state. Reposting or clearing
    // the legacy line here can disturb another device sharing that line.
}

} // namespace gem5
