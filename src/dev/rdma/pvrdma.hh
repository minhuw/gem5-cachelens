// SPDX-License-Identifier: BSD-3-Clause

#ifndef __DEV_RDMA_PVRDMA_HH__
#define __DEV_RDMA_PVRDMA_HH__

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

#include "dev/pci/device.hh"
#include "dev/rdma/pvrdma_abi.hh"
#include "params/Pvrdma.hh"
#include "sim/byteswap.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace pvrdma
{

inline constexpr uint32_t UnsupportedError = 0xffff;
inline constexpr uint32_t CommandError = 1;
inline constexpr uint32_t InitialInterruptMask = 0xffffffff;
inline constexpr uint32_t GidTableEntries = 8;
inline constexpr uint32_t FixedMtu = 1024;
inline constexpr uint32_t MaxMessageSize = uint32_t{1} << 31;
inline constexpr uint16_t FullMembershipPkey = 0xffff;

enum class Register
{
    Version,
    DsrLow,
    DsrHigh,
    Control,
    Request,
    Error,
    InterruptCause,
    InterruptMask,
    MacLow,
    MacHigh,
    Invalid,
};

enum class ControlState : uint8_t
{
    Unconfigured,
    Ready,
    Active,
    ReadingDsr,
    WritingCaps,
    ReadingCommand,
    WritingResponse,
};

constexpr bool
stable(ControlState state)
{
    return state == ControlState::Unconfigured ||
           state == ControlState::Ready || state == ControlState::Active;
}

constexpr bool
beginDsr(ControlState &state)
{
    if (state != ControlState::Unconfigured)
        return false;
    state = ControlState::ReadingDsr;
    return true;
}

constexpr bool
finishDsrRead(ControlState &state, bool valid)
{
    if (state != ControlState::ReadingDsr)
        return false;
    state = valid ? ControlState::WritingCaps : ControlState::Unconfigured;
    return valid;
}

constexpr bool
finishCapsWrite(ControlState &state)
{
    if (state != ControlState::WritingCaps)
        return false;
    state = ControlState::Ready;
    return true;
}

constexpr bool
applyControl(ControlState &state, DeviceControl control)
{
    if (!stable(state))
        return false;

    switch (control) {
      case DeviceControl::Activate:
        if (state != ControlState::Ready)
            return false;
        state = ControlState::Active;
        return true;
      case DeviceControl::Unquiesce:
        return state == ControlState::Active;
      case DeviceControl::Reset:
        state = ControlState::Unconfigured;
        return true;
    }
    return false;
}

constexpr bool
beginCommand(ControlState &state)
{
    if (state != ControlState::Active)
        return false;
    state = ControlState::ReadingCommand;
    return true;
}

constexpr bool
finishCommandRead(ControlState &state, bool has_response)
{
    if (state != ControlState::ReadingCommand)
        return false;
    state = has_response ? ControlState::WritingResponse :
                           ControlState::Active;
    return true;
}

constexpr bool
finishResponseWrite(ControlState &state, uint32_t &pending)
{
    if (state != ControlState::WritingResponse)
        return false;
    state = ControlState::Active;
    pending |= InterruptCauseResponse;
    return true;
}

constexpr bool
checkpointStable(ControlState state, bool dma_pending)
{
    return stable(state) && !dma_pending;
}

constexpr Register
decodeRegister(uint64_t offset)
{
    switch (offset) {
      case RegVersion: return Register::Version;
      case RegDsrLow: return Register::DsrLow;
      case RegDsrHigh: return Register::DsrHigh;
      case RegControl: return Register::Control;
      case RegRequest: return Register::Request;
      case RegError: return Register::Error;
      case RegInterruptCause: return Register::InterruptCause;
      case RegInterruptMask: return Register::InterruptMask;
      case RegMacLow: return Register::MacLow;
      case RegMacHigh: return Register::MacHigh;
      default: return Register::Invalid;
    }
}

constexpr bool
validRegisterAccess(uint64_t offset, unsigned size)
{
    return size == sizeof(uint32_t) && (offset % sizeof(uint32_t)) == 0 &&
           decodeRegister(offset) != Register::Invalid;
}

constexpr bool
registerReadable(Register reg)
{
    switch (reg) {
      case Register::Version:
      case Register::Error:
      case Register::InterruptCause:
      case Register::InterruptMask:
      case Register::MacLow:
      case Register::MacHigh:
        return true;
      default:
        return false;
    }
}

constexpr bool
registerWritable(Register reg)
{
    switch (reg) {
      case Register::DsrLow:
      case Register::DsrHigh:
      case Register::Control:
      case Register::Request:
      case Register::InterruptMask:
      case Register::MacLow:
      case Register::MacHigh:
        return true;
      default:
        return false;
    }
}

constexpr uint32_t
unmaskedInterrupts(uint32_t pending, uint32_t mask)
{
    return pending & ~mask;
}

constexpr bool
interruptPending(uint32_t pending, uint32_t mask)
{
    return unmaskedInterrupts(pending, mask) != 0;
}

inline uint32_t
acknowledgeInterrupts(uint32_t &pending)
{
    const uint32_t causes = pending;
    pending = 0;
    return causes;
}

struct RegisterState
{
    uint64_t dsrAddress = 0;
    uint32_t control = 0;
    uint32_t request = 0;
    uint32_t error = UnsupportedError;
    uint32_t pendingCauses = 0;
    uint32_t interruptMask = InitialInterruptMask;
    uint32_t macLow = 0;
    uint32_t macHigh = 0;
    bool dsrLowPending = false;

    RegisterState() = default;
    RegisterState(uint32_t low, uint32_t high) : macLow(low), macHigh(high) {}

    void
    reset()
    {
        dsrAddress = 0;
        control = 0;
        request = 0;
        error = UnsupportedError;
        pendingCauses = 0;
        interruptMask = InitialInterruptMask;
        dsrLowPending = false;
    }

    void
    writeDsrLow(uint32_t value)
    {
        dsrAddress = value;
        dsrLowPending = true;
    }

    bool
    writeDsrHigh(uint32_t value)
    {
        if (!dsrLowPending)
            return false;
        dsrAddress |= static_cast<uint64_t>(value) << 32;
        dsrLowPending = false;
        return true;
    }

    void
    writeMacHigh(uint32_t value)
    {
        macHigh = value & 0xffff;
    }

    uint32_t
    acknowledgeInterrupts()
    {
        return pvrdma::acknowledgeInterrupts(pendingCauses);
    }
};

using GidTable = std::array<Gid, GidTableEntries>;
using GidValidTable = std::array<uint8_t, GidTableEntries>;

struct CommandResult
{
    bool hasResponse;
    uint32_t error;
};

struct OperationErrorState
{
    uint64_t generation = 0;
    uint64_t operationGeneration = 0;

    void
    set(uint32_t &error, uint32_t value)
    {
        error = value;
        ++generation;
    }

    void
    begin(uint32_t &error)
    {
        set(error, 0);
        operationGeneration = generation;
    }

    void
    complete(uint32_t &error, uint32_t value) const
    {
        if (generation == operationGeneration)
            error = value;
    }

    void
    reset()
    {
        generation = operationGeneration = 0;
    }
};

namespace detail
{

inline uint64_t
macGuid(uint32_t mac_low, uint32_t mac_high)
{
    const std::array<uint8_t, 8> bytes = {
        static_cast<uint8_t>(static_cast<uint8_t>(mac_low) ^ 0x02),
        static_cast<uint8_t>(mac_low >> 8),
        static_cast<uint8_t>(mac_low >> 16),
        0xff,
        0xfe,
        static_cast<uint8_t>(mac_low >> 24),
        static_cast<uint8_t>(mac_high),
        static_cast<uint8_t>(mac_high >> 8),
    };
    uint64_t guid = 0;
    for (const auto byte : bytes)
        guid = (guid << 8) | byte;
    return guid;
}

inline bool
alignedPage(uint64_t address)
{
    return address && (address % UarPageSize) == 0;
}

inline bool
recognizedCommand(uint32_t command)
{
    return command <= static_cast<uint32_t>(Command::DestroySrq);
}

inline void
setResponseHeader(ResponseHeader &header, const CommandHeader &request,
                  uint32_t command, uint32_t error)
{
    header = {};
    header.response = request.response;
    header.acknowledgement = recognizedCommand(command) ?
        htole(responseCommand(static_cast<Command>(command))) : 0;
    header.error = error ? CommandError : 0;
}

inline CommandResult
queryPort(const CommandRequest &request, CommandResponse &response)
{
    const bool valid = letoh(request.header.reserved) == 0 &&
                       request.queryPort.portNumber == 1;
    setResponseHeader(response.header, request.header,
                      static_cast<uint32_t>(Command::QueryPort), !valid);
    if (!valid)
        return {true, CommandError};

    auto &attrs = response.queryPort.attributes;
    attrs = {};
    attrs.state = static_cast<PortState>(
        htole(static_cast<uint32_t>(PortState::Active)));
    attrs.maxMtu = static_cast<Mtu>(
        htole(static_cast<uint32_t>(Mtu::Mtu1024)));
    attrs.activeMtu = attrs.maxMtu;
    attrs.gidTableLength = htole(GidTableEntries);
    attrs.maxMessageSize = htole(MaxMessageSize);
    attrs.pkeyTableLength = htole(uint16_t{1});
    return {true, 0};
}

inline CommandResult
queryPkey(const CommandRequest &request, CommandResponse &response)
{
    const bool valid = letoh(request.header.reserved) == 0 &&
                       request.queryPkey.portNumber == 1 &&
                       request.queryPkey.index == 0;
    setResponseHeader(response.header, request.header,
                      static_cast<uint32_t>(Command::QueryPkey), !valid);
    if (!valid)
        return {true, CommandError};

    response.queryPkey.pkey = htole(FullMembershipPkey);
    return {true, 0};
}

inline CommandResult
createBind(const CommandRequest &request, GidTable &gids,
           GidValidTable &gid_valid)
{
    const auto &bind = request.createBind;
    const uint32_t index = letoh(bind.index);
    if (letoh(request.header.reserved) != 0 || index >= GidTableEntries ||
        letoh(bind.mtu) != FixedMtu || bind.gidType != GidTypeRoceV1)
        return {false, CommandError};

    std::copy(std::begin(bind.newGid), std::end(bind.newGid),
              std::begin(gids[index].raw));
    gid_valid[index] = 1;
    return {false, 0};
}

inline CommandResult
destroyBind(const CommandRequest &request, GidTable &gids,
            GidValidTable &gid_valid)
{
    const auto &bind = request.destroyBind;
    const uint32_t index = letoh(bind.index);
    if (letoh(request.header.reserved) != 0 || index >= GidTableEntries ||
        !gid_valid[index] ||
        !std::equal(std::begin(bind.destinationGid),
                    std::end(bind.destinationGid),
                    std::begin(gids[index].raw))) {
        return {false, CommandError};
    }

    gids[index] = {};
    gid_valid[index] = 0;
    return {false, 0};
}

} // namespace detail

inline DeviceCaps
makeCapabilities(uint32_t mac_low, uint32_t mac_high)
{
    DeviceCaps caps{};
    const uint64_t guid = detail::macGuid(mac_low, mac_high);
    caps.fwVersion = htole(uint64_t{1});
    caps.beNodeGuid = htobe(guid);
    caps.beSystemImageGuid = htobe(guid);
    caps.maxMrSize = htole(std::numeric_limits<uint64_t>::max());
    caps.pageSizeCap = htole(uint64_t{UarPageSize});
    caps.vendorId = htole(static_cast<uint32_t>(PciVendorId));
    caps.vendorPartId = htole(static_cast<uint32_t>(PciDeviceId));
    caps.hardwareVersion = htole(uint32_t{1});
    caps.maxQp = htole(uint32_t{64});
    caps.maxQpWr = htole(uint32_t{256});
    caps.maxSge = htole(uint32_t{1});
    caps.maxSgeRd = 0;
    caps.maxCq = htole(uint32_t{64});
    caps.maxCqe = htole(uint32_t{1024});
    caps.maxMr = htole(uint32_t{64});
    caps.maxPd = htole(uint32_t{64});
    caps.maxQpRdAtom = htole(uint32_t{1});
    caps.maxQpInitRdAtom = htole(uint32_t{1});
    caps.maxUar = htole(uint32_t{64});
    caps.gidTableLength = htole(GidTableEntries);
    caps.maxPkeys = htole(uint16_t{1});
    caps.physicalPortCount = 1;
    caps.mode = static_cast<uint8_t>(DeviceMode::Roce);
    caps.gidTypes = GidTypeRoceV1;
    return caps;
}

inline bool
validSharedRegion(const DeviceSharedRegion &dsr, uint64_t dsr_address)
{
    return detail::alignedPage(dsr_address) &&
           letoh(dsr.driverVersion) == Version &&
           detail::alignedPage(letoh(dsr.commandSlotDma)) &&
           detail::alignedPage(letoh(dsr.responseSlotDma)) &&
           letoh(dsr.asyncRingPages.numPages) == NumRingPages &&
           detail::alignedPage(letoh(dsr.asyncRingPages.pageDirectoryDma)) &&
           letoh(dsr.completionRingPages.numPages) == NumRingPages &&
           detail::alignedPage(
               letoh(dsr.completionRingPages.pageDirectoryDma));
}

inline CommandResult
processCommand(const CommandRequest &request, CommandResponse &response,
               GidTable &gids, GidValidTable &gid_valid)
{
    response = {};
    const uint32_t command = letoh(request.header.command);
    switch (command) {
      case static_cast<uint32_t>(Command::QueryPort):
        return detail::queryPort(request, response);
      case static_cast<uint32_t>(Command::QueryPkey):
        return detail::queryPkey(request, response);
      case static_cast<uint32_t>(Command::CreateBind):
        return detail::createBind(request, gids, gid_valid);
      case static_cast<uint32_t>(Command::DestroyBind):
        return detail::destroyBind(request, gids, gid_valid);
      default:
        detail::setResponseHeader(response.header, request.header, command,
                                  CommandError);
        return {true, CommandError};
    }
}

} // namespace pvrdma

class Pvrdma : public PciDevice
{
  public:
    PARAMS(Pvrdma);
    Pvrdma(const Params &params);

    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
    DrainState drain() override;

    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

  private:
    pvrdma::RegisterState regs;
    pvrdma::ControlState controlState = pvrdma::ControlState::Unconfigured;
    uint64_t commandSlotAddress = 0;
    uint64_t responseSlotAddress = 0;
    Addr dsrDmaAddress = 0;
    Addr commandSlotDmaAddress = 0;
    Addr responseSlotDmaAddress = 0;
    pvrdma::DeviceSharedRegion dsr{};
    pvrdma::DeviceCaps capabilities{};
    pvrdma::CommandRequest command{};
    pvrdma::CommandResponse response{};
    pvrdma::GidTable gids{};
    pvrdma::GidValidTable gidValid{};
    pvrdma::OperationErrorState operationError;
    bool intxAsserted = false;
    const Tick controlCompletionLatency;

    EventFunctionWrapper dsrReadEvent;
    EventFunctionWrapper capsWriteEvent;
    EventFunctionWrapper commandReadEvent;
    EventFunctionWrapper responseWriteEvent;

    void startDsr();
    void dsrReadDone();
    void capsWriteDone();
    void startCommand(uint32_t value);
    void commandReadDone();
    void responseWriteDone();
    void writeControl(uint32_t value);
    void resetDevice();
    void updateInterrupt();
    void operationDone();
};

} // namespace gem5

#endif // __DEV_RDMA_PVRDMA_HH__
