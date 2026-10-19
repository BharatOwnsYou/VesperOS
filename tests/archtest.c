/* host-side tests for x86_64 arch code that can't be booted here: the IDT/TSS bit encodings the cpu will parse,
 * the trap-frame layout the asm stubs build, the crash dump text, and the page-table editing in mmio_map().
 * privileged instrs are compiled out under HOST_TEST, the rest is the real kernel code.
 * expected values are built byte by byte from the Intel SDM layouts, NOT by reusing the code under test.
 * build/run: make test-arch */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "../arch/x86_64/traps.h"
#include "../include/mmio.h"

static int n_pass = 0, n_fail = 0;
static void check(int ok, const char *what) { if (ok) n_pass++; else { n_fail++; printf("  FAIL: %s\n", what); } }
#define CHECK(c) check((c), #c)

/* ---- symbols the kernel normally gets from the linker/asm/other files ---- */
__asm__(".pushsection .data\n.balign 4096\n.globl __kernel_start\n__kernel_start: .skip 0x20000\n.globl __kernel_end\n__kernel_end: .skip 8\n"
        ".balign 16\n.globl boot_stack_bottom\nboot_stack_bottom: .skip 65536\n.globl boot_stack_top\nboot_stack_top: .skip 8\n.popsection\n");
u64 isr_stub_table[32];
u64 boot_pdpt[512] __attribute__((aligned(4096)));
extern char __kernel_start[], __kernel_end[], boot_stack_bottom[], boot_stack_top[];
extern idt_entry_t idt[256]; extern u64 gdt[5]; extern tss_t tss; extern volatile int in_exception;
extern u64 pd_pool[8][512]; extern int pd_used;
void arch_early_init(void);

static char dbg_out[1 << 16], scr_out[1 << 16];
void dbg_putc(char c) { size_t n = strlen(dbg_out); if (n < sizeof(dbg_out) - 1) dbg_out[n] = c; }
void dbg_puts(const char *s) { size_t n = strlen(dbg_out); strncat(dbg_out, s, sizeof(dbg_out) - 1 - n); }
void dbg_hex(u64 v, int d) { char b[40]; snprintf(b, sizeof b, "0x%0*llx", d, (unsigned long long)v); dbg_puts(b); }
static int vga_inited = 0, vga_init_calls = 0;
void vga_puts_col(const char *s, u8 fg, u8 bg) { (void)fg; (void)bg; size_t n = strlen(scr_out); strncat(scr_out, s, sizeof(scr_out) - 1 - n); }
bool vga_is_initialized(void) { return vga_inited; }
void vga_init(void) { vga_inited = 1; vga_init_calls++; }

static int max_line_len(const char *s) {
    int best = 0, cur = 0;
    for (; *s; s++) { if (*s == '\n') { if (cur > best) best = cur; cur = 0; } else cur++; }
    return cur > best ? cur : best;
}

static void test_layouts(void) {
    printf("[layout] structures shared with hardware / assembly\n");
    CHECK(sizeof(idt_entry_t) == 16);
    CHECK(sizeof(tss_t) == 104);
    CHECK(sizeof(trap_frame_t) == 22 * 8);                       /* isr.S pushes 15 GPRs + vector + error + 5 CPU words */
    CHECK(offsetof(trap_frame_t, r15) == 0);                     /* r15 is pushed last => lowest address */
    CHECK(offsetof(trap_frame_t, rax) == 14 * 8);                /* rax pushed first of the GPRs */
    CHECK(offsetof(trap_frame_t, vector) == 15 * 8 && offsetof(trap_frame_t, error) == 16 * 8);
    CHECK(offsetof(trap_frame_t, rip) == 17 * 8 && offsetof(trap_frame_t, ss) == 21 * 8);
    CHECK(offsetof(tss_t, ist) == 36);                           /* SDM: IST1 is at offset 0x24 */
    CHECK(offsetof(tss_t, iomap_base) == 102);
    CHECK((sizeof(trap_frame_t) % 16) == 0);                     /* keeps rsp 16-byte aligned at the call into C */
}

static void test_idt_gate(void) {
    printf("[idt] gate encoding (Intel SDM Vol.3 6.14.1)\n");
    idt_entry_t e;
    idt_make_gate(&e, 0x0000000000123456ULL, 1);
    u8 want1[16] = { 0x56,0x34, 0x08,0x00, 0x01, 0x8E, 0x12,0x00, 0,0,0,0, 0,0,0,0 };
    CHECK(memcmp(&e, want1, 16) == 0);
    idt_make_gate(&e, 0x00000001FEDCBA98ULL, 0);                 /* needs the high dword */
    u8 want2[16] = { 0x98,0xBA, 0x08,0x00, 0x00, 0x8E, 0xDC,0xFE, 0x01,0x00,0x00,0x00, 0,0,0,0 };
    CHECK(memcmp(&e, want2, 16) == 0);
    idt_make_gate(&e, 0xFFFFFFFF80100000ULL, 1);                 /* a higher-half address, for the future */
    u8 want3[16] = { 0x00,0x00, 0x08,0x00, 0x01, 0x8E, 0x10,0x80, 0xFF,0xFF,0xFF,0xFF, 0,0,0,0 };
    CHECK(memcmp(&e, want3, 16) == 0);
}

static void test_tss_desc(void) {
    printf("[gdt] TSS descriptor encoding (Intel SDM Vol.3 7.2.3)\n");
    u64 base = 0x00000001FEDC4560ULL; u32 limit = 103; u64 d[2];
    gdt_make_tss(d, base, limit);
    u8 want[16];                                                  /* built field-by-field from the SDM figure */
    want[0] = limit & 0xFF;            want[1] = (limit >> 8) & 0xFF;
    want[2] = base & 0xFF;             want[3] = (base >> 8) & 0xFF;      want[4] = (base >> 16) & 0xFF;
    want[5] = 0x89;                                                       /* P=1, DPL=0, type=9 (available 64-bit TSS) */
    want[6] = (u8)((limit >> 16) & 0xF);                                  /* G=0, limit 19:16 */
    want[7] = (base >> 24) & 0xFF;
    want[8] = (base >> 32) & 0xFF;     want[9] = (base >> 40) & 0xFF;     want[10] = (base >> 48) & 0xFF; want[11] = (base >> 56) & 0xFF;
    want[12] = want[13] = want[14] = want[15] = 0;
    CHECK(memcmp(d, want, 16) == 0);
}

static void test_early_init(void) {
    printf("[init] arch_early_init() builds the IDT/GDT/TSS the CPU will use\n");
    for (int i = 0; i < 32; i++) isr_stub_table[i] = 0x101000ULL + 0x9ULL * (u64)i;
    memset(idt, 0, sizeof idt);
    arch_early_init();
    int ok_gate = 1, ok_ist = 1;
    for (int v = 0; v < 32; v++) {
        u64 h = (u64)idt[v].off_lo | ((u64)idt[v].off_mid << 16) | ((u64)idt[v].off_hi << 32);
        if (h != isr_stub_table[v] || idt[v].sel != 0x08 || idt[v].attr != 0x8E || idt[v].zero != 0) ok_gate = 0;
        u8 want_ist = (v == 2 || v == 8 || v == 18) ? 1 : 0;           /* NMI, #DF, #MC get the dedicated stack */
        if (idt[v].ist != want_ist) ok_ist = 0;
    }
    check(ok_gate, "vectors 0-31 point at their stubs with selector 0x08 and type 0x8E");
    check(ok_ist, "IST1 is used for exactly NMI, #DF and #MC");
    CHECK(idt[32].attr == 0 && idt[255].attr == 0);                    /* everything else stays not-present */
    CHECK(gdt[0] == 0 && gdt[1] == 0x00AF9A000000FFFFULL && gdt[2] == 0x00CF92000000FFFFULL);
    u64 d[2]; gdt_make_tss(d, (u64)(uintptr_t)&tss, sizeof(tss) - 1);
    CHECK(gdt[3] == d[0] && gdt[4] == d[1]);
    CHECK(tss.iomap_base == 104);
    CHECK((tss.ist[0] & 15) == 0 && tss.ist[0] != 0);                  /* 16-byte aligned top of a real stack */
    CHECK(dbg_out[0] != 0 && strstr(dbg_out, "GDT/TSS/IDT ready") != NULL);
}

static void test_dump(void) {
    printf("[dump] crash-dump text\n");
    dbg_out[0] = scr_out[0] = 0;
    trap_frame_t f; memset(&f, 0, sizeof f);
    f.vector = 14; f.error = 0x2; f.rip = (u64)(uintptr_t)__kernel_start + 0x1234; f.cs = 8; f.rflags = 0x202;
    f.rsp = (u64)(uintptr_t)boot_stack_top - 0x100; f.ss = 0x10;
    f.rax = 0x1111111111111111ULL; f.r15 = 0xFEEDFACECAFEBEEFULL; f.rdi = 0xDEADBEEF;
    ((u64 *)(uintptr_t)f.rsp)[0] = 0xA0A0A0A0A0A0A0A0ULL;
    trap_dump(&f, 1);
    CHECK(strstr(scr_out, "KERNEL EXCEPTION") != NULL);
    CHECK(strstr(scr_out, "#PF Page Fault") != NULL);
    CHECK(strstr(scr_out, "err=0x0000000000000002") != NULL);
    CHECK(strstr(scr_out, "vector=0x000000000000000e") != NULL);
    char want_rip[64]; snprintf(want_rip, sizeof want_rip, "RIP=0x%016llx", (unsigned long long)f.rip);
    CHECK(strstr(scr_out, want_rip) != NULL);
    CHECK(strstr(scr_out, "RAX=0x1111111111111111") != NULL);
    CHECK(strstr(scr_out, "R15=0xfeedfacecafebeef") != NULL);
    CHECK(strstr(scr_out, "RDI=0x00000000deadbeef") != NULL);
    CHECK(strstr(scr_out, "RIP_offset_in_image=0x0000000000001234") != NULL);  /* offset from the image start, for nm */
    CHECK(strstr(scr_out, "a0a0a0a0a0a0a0a0") != NULL);                 /* stack words were dumped (rsp is inside our stack) */
    CHECK(strcmp(scr_out, dbg_out) == 0);                               /* screen and debug ports carry identical text */
    check(max_line_len(scr_out) <= 79, "no dump line is wider than the 80-column screen");
    printf("       (longest line: %d columns)\n", max_line_len(scr_out));

    /* RIP outside the image and a wild RSP: must not read memory it should not, and must say so */
    dbg_out[0] = scr_out[0] = 0;
    f.rip = 0xDEAD0000ULL; f.rsp = 0x10;
    trap_dump(&f, 1);                                                   /* would segfault here if it dereferenced rsp=0x10 */
    CHECK(strstr(scr_out, "OUTSIDE the kernel image") != NULL);
    CHECK(strstr(scr_out, "[rsp+00]") == NULL);
    check(max_line_len(scr_out) <= 79, "no line too wide in the outside-image case either");

    /* all 32 vector names exist and fit */
    int named = 1;
    for (int v = 0; v < 32; v++) {
        dbg_out[0] = scr_out[0] = 0; f.vector = (u64)v; trap_dump(&f, 1);
        if (max_line_len(scr_out) > 79 || strlen(scr_out) < 100) named = 0;
    }
    check(named, "every vector 0-31 produces a well-formed dump");
}

static void test_dispatch(void) {
    printf("[dispatch] exception_dispatch policy\n");
    trap_frame_t f; memset(&f, 0, sizeof f);
    f.rsp = (u64)(uintptr_t)boot_stack_top - 0x100;
    dbg_out[0] = scr_out[0] = 0; in_exception = 0; vga_inited = 0; vga_init_calls = 0;
    f.vector = 2;                                                       /* NMI: survive, print nothing on screen */
    exception_dispatch(&f);
    CHECK(strstr(dbg_out, "NMI ignored") != NULL && scr_out[0] == 0 && in_exception == 0);

    dbg_out[0] = scr_out[0] = 0; f.vector = 13;                         /* first real fault: serial, then (initialised) screen */
    exception_dispatch(&f);
    CHECK(in_exception == 1 && vga_init_calls == 1);                    /* screen was not ready, so it got initialised first */
    CHECK(strstr(scr_out, "#GP General Protection") != NULL && strstr(dbg_out, "#GP General Protection") != NULL);

    dbg_out[0] = scr_out[0] = 0; f.vector = 14;                         /* fault inside the handler: debug ports ONLY */
    exception_dispatch(&f);
    CHECK(strstr(dbg_out, "exception while handling an exception") != NULL);
    CHECK(scr_out[0] == 0);                                             /* never touches the (possibly broken) screen again */
}

static void test_mmio(void) {
    printf("[mmio] on-demand mapping above 4 GiB edits the page tables correctly\n");
    memset(boot_pdpt, 0, sizeof boot_pdpt);
    for (int g = 0; g < 4; g++) boot_pdpt[g] = 0x1000ULL * (u64)(g + 1) | 3;   /* as set up by boot_tables.S */
    u64 saved[4]; memcpy(saved, boot_pdpt, sizeof saved);

    CHECK(mmio_map(0xFCD04000ULL, 0x10000) == (volatile void *)(uintptr_t)0xFCD04000ULL);   /* below 4 GiB: identity, untouched */
    CHECK(memcmp(saved, boot_pdpt, sizeof saved) == 0 && pd_used == 0);
    CHECK(mmio_map(0xFFFF0000ULL, 0x10000) == (volatile void *)(uintptr_t)0xFFFF0000ULL);   /* ends exactly at 4 GiB */
    CHECK(pd_used == 0);

    u64 phys = 0x7F0000000ULL + 0x1FF000ULL;                            /* 29.9 GiB: straddles a 2 MiB boundary */
    volatile void *p = mmio_map(phys, 0x3000);
    CHECK(p == (volatile void *)(uintptr_t)phys);
    u64 gib = phys >> 30;
    CHECK(gib == 0x1F && (boot_pdpt[gib] & 3) == 3 && pd_used == 1);
    u64 *pd = (u64 *)(uintptr_t)(boot_pdpt[gib] & ~0xFFFULL);
    CHECK(pd == pd_pool[0] && ((uintptr_t)pd % 4096) == 0);
    u64 page0 = phys >> 21, page1 = (phys + 0x2FFF) >> 21;
    CHECK(page1 == page0 + 1);
    CHECK(pd[page0 & 511] == ((page0 << 21) | 0x9B));                   /* P|RW|PWT|PCD|PS = uncached 2 MiB page */
    CHECK(pd[page1 & 511] == ((page1 << 21) | 0x9B));
    CHECK((0x9B & 0x01) && (0x9B & 0x02) && (0x9B & 0x08) && (0x9B & 0x10) && (0x9B & 0x80));
    int stray = 0; for (int i = 0; i < 512; i++) if (pd[i] && i != (int)(page0 & 511) && i != (int)(page1 & 511)) stray++;
    CHECK(stray == 0);                                                  /* nothing else got mapped */

    CHECK(mmio_map(phys, 0x1000) == (volatile void *)(uintptr_t)phys && pd_used == 1);     /* idempotent, reuses the page directory */
    CHECK(mmio_map(0x7F0000000ULL, 0x1000) != NULL && pd_used == 1);                        /* same GiB, different page */
    CHECK(mmio_map(0x3C0000000ULL, 0x1000) != NULL && pd_used == 2);                        /* 15 GiB: a second PD */
    CHECK(mmio_map(0x100000000ULL - 0x1000, 0x2000) != NULL && pd_used == 3);               /* straddles 4 GiB: low half is in the boot map */
    CHECK((boot_pdpt[4] & 1) && memcmp(saved, boot_pdpt, sizeof saved) == 0);               /* PDPT[0..3] never altered */

    CHECK(mmio_map(~0ULL - 0x100, 0x1000) == NULL);                     /* arithmetic wrap */
    CHECK(mmio_map((1ULL << 39) - 0x1000, 0x2000) == NULL);             /* beyond the 512 GiB one PDPT can map */
    CHECK(mmio_map(0x7F0000000ULL, 0) != NULL);                         /* zero length is treated as one byte */

    for (u64 g = 5; g < 5 + 16; g++) mmio_map(g << 30, 0x1000);         /* exhaust the page-directory pool */
    CHECK(pd_used == 8);
    CHECK(mmio_map(60ULL << 30, 0x1000) == NULL);                       /* pool exhausted: clean NULL, no corruption */
}

int main(void) {
    test_layouts(); test_idt_gate(); test_tss_desc(); test_early_init(); test_dump(); test_dispatch(); test_mmio();
    printf("\n%d checks passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
