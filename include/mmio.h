#ifndef VESPEROS_MMIO_H
#define VESPEROS_MMIO_H
#include "types.h"

/* pointer for accessing phys range [phys, phys+len) of device memory (PCI BAR, framebuffer ...), NULL if unreachable.
 *  - below 4G: identity-mapped by the boot code on both archs, returned as-is
 *  - above 4G: x86_64 only, adds an uncached identity mapping on demand
 * always check the result, on i686 a BAR above 4G just can't be reached */
volatile void *mmio_map(u64 phys, u64 len);

/* combines a BAR (+ upper half for 64-bit BARs) into a phys addr, 0 if it's an I/O BAR or unassigned.
 * *is64 says whether the BAR was 64-bit */
static inline u64 mmio_bar_base(u32 lo, u32 hi, int *is64) {
    if (lo & 1) { if (is64) *is64 = 0; return 0; }
    int wide = ((lo >> 1) & 3) == 2;
    if (is64) *is64 = wide;
    u64 base = (u64)(lo & ~0xFu);
    if (wide) base |= (u64)hi << 32;
    return base;
}
#endif
