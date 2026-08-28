// SPDX-License-Identifier: BSD-3-Clause

#ifndef __DEV_RDMA_PVRDMA_HH__
#define __DEV_RDMA_PVRDMA_HH__

#include <cstdint>

#include "dev/pci/device.hh"
#include "dev/rdma/pvrdma_abi.hh"
#include "params/Pvrdma.hh"

namespace gem5
{
namespace pvrdma
{

inline constexpr uint32_t UnsupportedError = 0xffff;
inline constexpr uint32_t InitialInterruptMask = 0xffffffff;

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

} // namespace pvrdma

class Pvrdma : public PciDevice
{
  public:
    PARAMS(Pvrdma);
    Pvrdma(const Params &params);

    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;

    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

  private:
    pvrdma::RegisterState regs;
    bool intxAsserted = false;

    void resetDevice();
    void updateInterrupt();
};

} // namespace gem5

#endif // __DEV_RDMA_PVRDMA_HH__
