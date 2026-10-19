#ifndef VESPEROS_DMA_H
#define VESPEROS_DMA_H
#include "types.h"

/* bus (DMA) addresses of kernel objects. kernel is identity-mapped so bus addr == pointer value.
 * descriptors take 64-bit addrs as two 32-bit halves, high half is 0 on i686.
 * never write (u32)ptr, it truncates on x86_64 (-Wpointer-to-int-cast flags it) */
static inline u64 dma_addr(const void *p) { return (u64)(uptr)p; }
static inline u32 dma_lo(const void *p)   { return (u32)((u64)(uptr)p & 0xFFFFFFFFu); }
static inline u32 dma_hi(const void *p)   { return (u32)((u64)(uptr)p >> 32); }

/* compiler barrier for "device just wrote this memory": stops loads of DMA buffers being hoisted above
 * the ready check (x86 already keeps loads ordered) */
static inline void dma_rmb(void) { __asm__ volatile ("" ::: "memory"); }

/* for 32-bit-only hw (UHCI frame lists, legacy virtio PFNs ...). static buffers sit just above 1M so this
 * always holds, the check turns a future violation into a clean failure instead of silent corruption.
 * returns 0 and sets *out on success, -1 if the address doesn't fit */
static inline int dma_lo32_checked(const void *p, u32 *out) {
    u64 a = (u64)(uptr)p;
    if (a >> 32) return -1;
    *out = (u32)a;
    return 0;
}
#endif
