/* i686: legacy 32-bit kernel. no paging, no idt. anything above 4G is unreachable */
#include "../../include/arch.h"
#include "../../include/mmio.h"

void arch_early_init(void) { }
void arch_crash_test(void) { }
const char *arch_describe(void) { return "x86_32 protected mode (no paging, no exception handlers)"; }

volatile void *mmio_map(u64 phys, u64 len) {
    if (len == 0) len = 1;
    u64 end = phys + len;
    if (end < phys || end > (1ULL << 32)) return NULL;
    return (volatile void *)(uptr)phys;
}
