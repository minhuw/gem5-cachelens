// SPDX-License-Identifier: MIT
#ifndef __SIMBRICKS_PCI_SAFETY_HH__
#define __SIMBRICKS_PCI_SAFETY_HH__

#include <cstddef>
#include <cstdint>
#include <climits>

namespace gem5::simbricks::pci {
inline bool within(uint64_t off, uint64_t len, uint64_t size)
{ return off <= size && len <= size - off; }

inline bool validDma(uint64_t addr, uint64_t len, uint64_t capacity)
{ return len && len <= capacity && len <= UINT16_MAX && len - 1 <= UINT64_MAX - addr; }

inline void maskedByte(uint8_t &dst, uint8_t value, uint8_t mask)
{ dst = (dst & ~mask) | (value & mask); }

inline uint8_t msixMask(size_t off) { return off == 3 ? 0xc0 : 0; }
inline uint8_t msiMask(size_t off)
{
    if (off == 2) return 0x71; // Enable and multiple-message enable only.
    if (off == 4) return 0xfc;
    return off >= 5 && off < 14 ? 0xff : 0;
}

// Template keeps the wire header's C namespace local to the adapter.
template<class Intro> bool validIntro(const Intro &di)
{
    for (unsigned i = 0; i < 6; ++i) {
        uint64_t len = di.bars[i].len, flags = di.bars[i].flags;
        if (flags & ~15ULL) return false;
        if (!len) continue;
        if ((len & (len - 1)) || len < ((flags & 1) ? 4 : 16)) return false;
        if ((flags & 1) && (flags & 6)) return false;
        if (!(flags & 2) && len > (1ULL << 32)) return false;
        if ((flags & 2) && (i == 5 || di.bars[i + 1].len)) return false;
    }
    unsigned msi = di.pci_msi_nvecs, n = di.pci_msix_nvecs;
    if (msi > 32 || (msi && (msi & (msi - 1))) || n > 2048) return false;
    if (!n) return true;
    unsigned tb = di.pci_msix_table_bar, pb = di.pci_msix_pba_bar;
    uint64_t to = di.pci_msix_table_offset, po = di.pci_msix_pba_offset;
    uint64_t ts = n * 16, ps = ((n + 63) / 64) * 8;
    if (tb >= 6 || pb >= 6 || (to & 7) || (po & 7)) return false;
    if ((di.bars[tb].flags | di.bars[pb].flags) & 1) return false;
    // Existing PciDevice stores ends in int; reject rather than truncate.
    if (!within(to, ts, INT_MAX) || !within(po, ps, INT_MAX)) return false;
    if (!within(to, ts, di.bars[tb].len) || !within(po, ps, di.bars[pb].len)) return false;
    return tb != pb || to + ts <= po || po + ps <= to;
}
}
#endif
