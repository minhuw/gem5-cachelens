/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __DEV_NET_LOAD_GENERATOR_MAC_HH__
#define __DEV_NET_LOAD_GENERATOR_MAC_HH__

#include <array>
#include <cstdint>

namespace gem5
{

using LoadGeneratorMac = std::array<uint8_t, 6>;

/** Return the deterministic locally administered MAC of a paired NIC. */
inline constexpr LoadGeneratorMac
loadGeneratorNicMac(uint8_t loadgen_id)
{
    return {0x02, 0x90, 0x00, 0x00, 0x00,
            static_cast<uint8_t>(loadgen_id + 1)};
}

} // namespace gem5

#endif // __DEV_NET_LOAD_GENERATOR_MAC_HH__
