#include "cpuid.h"
#include "string.h"

static void do_cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile ("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf));
}

void cpuid_vendor(char out[13]) {
    u32 a, b, c, d;
    do_cpuid(0, &a, &b, &c, &d);
    k_memcpy(out + 0, &b, 4);
    k_memcpy(out + 4, &d, 4);
    k_memcpy(out + 8, &c, 4);
    out[12] = 0;
}

void cpuid_brand(char out[49]) {
    u32 a, b, c, d;
    do_cpuid(0x80000000, &a, &b, &c, &d);
    out[0] = 0;
    if (a < 0x80000004) return;
    for (u32 leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        do_cpuid(leaf, &a, &b, &c, &d);
        int off = (int)((leaf - 0x80000002) * 16);
        k_memcpy(out + off + 0, &a, 4);
        k_memcpy(out + off + 4, &b, 4);
        k_memcpy(out + off + 8, &c, 4);
        k_memcpy(out + off + 12, &d, 4);
    }
    out[48] = 0;
}
