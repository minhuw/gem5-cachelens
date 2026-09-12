/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __DEV_PCI_CONFIG_WRITE_HH__
#define __DEV_PCI_CONFIG_WRITE_HH__

#include <cassert>
#include <cstdint>

#include "dev/pci/pcireg.h"

namespace gem5
{
namespace pci
{

/** Standard PCI Status bits implemented as write-one-to-clear. */
constexpr uint16_t StatusW1cMask =
    (1U << 15) | // Detected parity error.
    (1U << 14) | // Signaled system error.
    (1U << 13) | // Received master abort.
    (1U << 12) | // Received target abort.
    (1U << 11) | // Signaled target abort.
    (1U << 8);   // Master data parity error.

struct CommandStatusWriteResult
{
    uint16_t command;
    uint16_t status;
    bool rangeChanged;
};

/**
 * Apply a byte, word, or dword config write within Command/Status.
 *
 * Command bits retain their existing read/write behavior. Status fields are
 * preserved except for implemented W1C bits selected by the write. Values are
 * in host byte order; the caller is responsible for converting config-space
 * storage to and from little endian.
 */
inline CommandStatusWriteResult
applyCommandStatusWrite(uint16_t command, uint16_t status, unsigned offset,
                        unsigned size, uint32_t value)
{
    assert(offset >= PCI_COMMAND);
    assert(offset + size <= PCI_STATUS + sizeof(uint16_t));
    assert(size == sizeof(uint8_t) || size == sizeof(uint16_t) ||
           size == sizeof(uint32_t));

    const unsigned shift = (offset - PCI_COMMAND) * 8;
    const uint32_t accessMask = size == sizeof(uint32_t) ?
        0xffffffffU : ((1U << (size * 8)) - 1) << shift;
    const uint32_t writeData = value << shift;

    const uint16_t commandMask = accessMask;
    const uint16_t newCommand =
        (command & ~commandMask) | (writeData & commandMask);

    const uint16_t statusMask = accessMask >> 16;
    const uint16_t statusData = writeData >> 16;
    const uint16_t newStatus =
        status & ~(statusData & statusMask & StatusW1cMask);

    constexpr uint16_t decodeMask = PCI_CMD_IOSE | PCI_CMD_MSE;
    return {newCommand, newStatus,
            static_cast<bool>((command ^ newCommand) & decodeMask)};
}

} // namespace pci
} // namespace gem5

#endif // __DEV_PCI_CONFIG_WRITE_HH__
