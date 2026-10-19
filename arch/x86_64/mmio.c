/* identity-maps device memory above 4G (64-bit BARs, high fb) on demand. below 4G is already
 * mapped by boot_tables.S and comes back untouched. mappings are uncached 2M pages: fine for
 * registers, slow for a framebuffer (write-combining later maybe). single cpu, irqs off, no locking */
#include "../../include/mmio.h"
#include "../../kernel/debug.h"

#ifndef HOST_TEST
extern u64 boot_pdpt[512];        /* defined in boot_tables.S */
#else
extern u64 boot_pdpt[512];        /* defined by tests/archtest.c */
#endif

#define MMIO_PD_POOL 8                     /* each page directory covers 1 GiB; 8 = up to 8 distinct GiB above 4 GiB */
#ifdef HOST_TEST
u64 pd_pool[MMIO_PD_POOL][512] __attribute__((aligned(4096)));   /* visible to the test */
int pd_used = 0;
#else
static u64 pd_pool[MMIO_PD_POOL][512] __attribute__((aligned(4096)));
static int pd_used = 0;
#endif

volatile void *mmio_map(u64 phys, u64 len) {
    if (len == 0) len = 1;
    u64 end = phys + len;
    if (end < phys) return NULL;                         /* wrapped */
    if (end <= (1ULL << 32)) return (volatile void *)(uptr)phys;   /* already identity-mapped */
    if (end > (1ULL << 39)) return NULL;                 /* beyond what one PDPT (512 GiB) covers */

    for (u64 page = phys >> 21; page <= ((end - 1) >> 21); page++) {
        u64 gib = page >> 9;
        if (gib < 4) continue;                           /* part of the range is below 4 GiB: boot map has it */
        u64 *pd;
        if (!(boot_pdpt[gib] & 1)) {
            if (pd_used >= MMIO_PD_POOL) { dbg_puts("[mmio] page-directory pool exhausted\n"); return NULL; }
            pd = pd_pool[pd_used++];                     /* zero-filled (.bss) */
            boot_pdpt[gib] = (u64)(uptr)pd | 0x03;       /* present | writable */
        } else {
            pd = (u64 *)(uptr)(boot_pdpt[gib] & ~0xFFFULL);
        }
        u64 idx = page & 511;
        if (!(pd[idx] & 1))
            pd[idx] = (page << 21) | 0x9B;               /* present | writable | PWT | PCD | 2 MiB page */
    }
#ifndef HOST_TEST
    u64 cr3;                                             /* flush: new translations must be seen */
    __asm__ volatile ("mov %%cr3, %0\n\tmov %0, %%cr3" : "=r"(cr3) :: "memory");
#endif

    dbg_puts("[mmio] mapped phys "); dbg_hex(phys, 12); dbg_puts(" len "); dbg_hex(len, 8); dbg_puts(" (uncached)\n");
    return (volatile void *)(uptr)phys;
}
