// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include "dev/rdma/pvrdma.hh"

namespace gem5
{
namespace pvrdma
{

TEST(PvrdmaRegisterTest, DecodesOnlyDefinedAlignedRegisters)
{
    EXPECT_EQ(decodeRegister(RegVersion), Register::Version);
    EXPECT_EQ(decodeRegister(RegDsrLow), Register::DsrLow);
    EXPECT_EQ(decodeRegister(RegDsrHigh), Register::DsrHigh);
    EXPECT_EQ(decodeRegister(RegControl), Register::Control);
    EXPECT_EQ(decodeRegister(RegRequest), Register::Request);
    EXPECT_EQ(decodeRegister(RegError), Register::Error);
    EXPECT_EQ(decodeRegister(RegInterruptCause), Register::InterruptCause);
    EXPECT_EQ(decodeRegister(RegInterruptMask), Register::InterruptMask);
    EXPECT_EQ(decodeRegister(RegMacLow), Register::MacLow);
    EXPECT_EQ(decodeRegister(RegMacHigh), Register::MacHigh);
    EXPECT_EQ(decodeRegister(0x28), Register::Invalid);
    EXPECT_EQ(decodeRegister(RegVersion + 1), Register::Invalid);
}

TEST(PvrdmaRegisterTest, RequiresExactWidthAndAlignment)
{
    EXPECT_TRUE(validRegisterAccess(RegVersion, sizeof(uint32_t)));
    EXPECT_TRUE(validRegisterAccess(RegMacHigh, sizeof(uint32_t)));
    EXPECT_FALSE(validRegisterAccess(RegVersion, sizeof(uint8_t)));
    EXPECT_FALSE(validRegisterAccess(RegVersion, sizeof(uint16_t)));
    EXPECT_FALSE(validRegisterAccess(RegVersion, sizeof(uint64_t)));
    EXPECT_FALSE(validRegisterAccess(RegDsrLow + 1, sizeof(uint32_t)));
    EXPECT_FALSE(validRegisterAccess(0x28, sizeof(uint32_t)));
}

TEST(PvrdmaRegisterTest, EnforcesRegisterDirections)
{
    EXPECT_TRUE(registerReadable(Register::Version));
    EXPECT_TRUE(registerReadable(Register::Error));
    EXPECT_TRUE(registerReadable(Register::InterruptCause));
    EXPECT_TRUE(registerReadable(Register::InterruptMask));
    EXPECT_TRUE(registerReadable(Register::MacLow));
    EXPECT_TRUE(registerReadable(Register::MacHigh));
    EXPECT_FALSE(registerReadable(Register::DsrLow));
    EXPECT_FALSE(registerReadable(Register::Control));
    EXPECT_FALSE(registerReadable(Register::Request));

    EXPECT_TRUE(registerWritable(Register::DsrLow));
    EXPECT_TRUE(registerWritable(Register::DsrHigh));
    EXPECT_TRUE(registerWritable(Register::Control));
    EXPECT_TRUE(registerWritable(Register::Request));
    EXPECT_TRUE(registerWritable(Register::InterruptMask));
    EXPECT_TRUE(registerWritable(Register::MacLow));
    EXPECT_TRUE(registerWritable(Register::MacHigh));
    EXPECT_FALSE(registerWritable(Register::Version));
    EXPECT_FALSE(registerWritable(Register::Error));
    EXPECT_FALSE(registerWritable(Register::InterruptCause));
}

TEST(PvrdmaRegisterTest, AssemblesDsrInLowHighOrder)
{
    RegisterState regs;

    EXPECT_FALSE(regs.writeDsrHigh(0x11223344));
    regs.writeDsrLow(0x89abcdef);
    EXPECT_TRUE(regs.dsrLowPending);
    EXPECT_TRUE(regs.writeDsrHigh(0x01234567));
    EXPECT_EQ(regs.dsrAddress, 0x0123456789abcdefULL);
    EXPECT_FALSE(regs.dsrLowPending);

    regs.writeDsrLow(0x76543210);
    EXPECT_TRUE(regs.writeDsrHigh(0xfedcba98));
    EXPECT_EQ(regs.dsrAddress, 0xfedcba9876543210ULL);
}

TEST(PvrdmaRegisterTest, MasksAndAcknowledgesInterrupts)
{
    uint32_t pending = InterruptCauseResponse | InterruptCauseCompletion;

    EXPECT_FALSE(interruptPending(pending, InitialInterruptMask));
    EXPECT_TRUE(interruptPending(pending, 0));
    EXPECT_EQ(unmaskedInterrupts(pending, InterruptCauseResponse),
              InterruptCauseCompletion);
    EXPECT_EQ(acknowledgeInterrupts(pending),
              InterruptCauseResponse | InterruptCauseCompletion);
    EXPECT_EQ(pending, 0);
}

TEST(PvrdmaRegisterTest, ResetRestoresMutableConstructorState)
{
    RegisterState regs(0x44332211, 0x6655);
    regs.writeDsrLow(0x89abcdef);
    ASSERT_TRUE(regs.writeDsrHigh(0x01234567));
    regs.control = static_cast<uint32_t>(DeviceControl::Unquiesce);
    regs.request = 0x1234;
    regs.error = 0;
    regs.pendingCauses = InterruptCauseAsync;
    regs.interruptMask = 0;
    regs.writeMacHigh(0x12346655);
    EXPECT_EQ(regs.macHigh, 0x6655);

    regs.reset();

    EXPECT_EQ(regs.dsrAddress, 0);
    EXPECT_EQ(regs.control, 0);
    EXPECT_EQ(regs.request, 0);
    EXPECT_EQ(regs.error, UnsupportedError);
    EXPECT_EQ(regs.pendingCauses, 0);
    EXPECT_EQ(regs.interruptMask, InitialInterruptMask);
    EXPECT_FALSE(regs.dsrLowPending);
    EXPECT_EQ(regs.macLow, 0x44332211);
    EXPECT_EQ(regs.macHigh, 0x6655);
}

} // namespace pvrdma
} // namespace gem5
