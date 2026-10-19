/* x86_64 exceptions: GDT + TSS + IDT and a handler that dumps registers.
 * without this any cpu fault triple-faults and the machine just reboots with no clue why.
 * dumps to screen + debug ports (debugcon / COM1) then halts. IF stays 0, this is exceptions only */
#include "../../include/types.h"
#include "../../include/arch.h"
#include "../../kernel/vga.h"
#include "../../kernel/debug.h"

extern char __kernel_start[], __kernel_end[];
extern char boot_stack_bottom[], boot_stack_top[];
extern u64  isr_stub_table[32];

#include "traps.h"

typedef struct { u16 limit; u64 base; } __attribute__((packed)) dtr_t;   /* GDTR / IDTR image */

/* Under HOST_TEST (tests/archtest.c) the tables are visible to the test; in the kernel they are private. */
#ifdef HOST_TEST
#  define TESTABLE
#else
#  define TESTABLE static
#endif
TESTABLE idt_entry_t idt[256] __attribute__((aligned(16)));
TESTABLE u64   gdt[5] __attribute__((aligned(16)));          /* null, code, data, TSS (2 slots) */
TESTABLE tss_t tss __attribute__((aligned(16)));
static u8    ist1_stack[16384] __attribute__((aligned(16))); /* #DF / NMI / #MC run here, never on a possibly-broken stack */
static dtr_t idtr, gdtr;

void idt_make_gate(idt_entry_t *e, u64 handler, u8 ist) {
    e->off_lo  = (u16)(handler & 0xFFFF);
    e->sel     = 0x08;
    e->ist     = ist;
    e->attr    = 0x8E;                     /* present, DPL 0, 64-bit interrupt gate (IF cleared on entry) */
    e->off_mid = (u16)((handler >> 16) & 0xFFFF);
    e->off_hi  = (u32)(handler >> 32);
    e->zero    = 0;
}

static void set_gate(int v, u64 handler, u8 ist) { idt_make_gate(&idt[v], handler, ist); }

void gdt_make_tss(u64 out[2], u64 base, u32 limit) {
    out[0] = ((u64)limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40)    /* present, type 9 = available 64-bit TSS */
           | ((((u64)limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    out[1] = base >> 32;
}

static void install_gdt_tss(void) {
    u64 base = (u64)(uptr)&tss;
    u64 limit = sizeof(tss) - 1;
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;        /* 0x08 code64 */
    gdt[2] = 0x00CF92000000FFFFULL;        /* 0x10 data   */
    gdt_make_tss(&gdt[3], base, (u32)limit);                                      /* 0x18 TSS, available */
    tss.ist[0] = (u64)(uptr)(ist1_stack + sizeof(ist1_stack));
    tss.iomap_base = sizeof(tss);          /* no I/O permission bitmap */
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (u64)(uptr)gdt;
#ifndef HOST_TEST
    __asm__ volatile (
        "lgdt %0\n\t"
        "pushq $0x08\n\t"                  /* reload CS through a far return */
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorl %%eax, %%eax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw $0x18, %%ax\n\t"
        "ltr %%ax\n\t"
        :: "m"(gdtr) : "rax", "memory");
#endif
}

void arch_early_init(void) {
    dbg_puts("[x64] kernel_main reached; installing IDT\n");
    for (int v = 0; v < 32; v++) set_gate(v, isr_stub_table[v], 0);
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (u64)(uptr)idt;
#ifndef HOST_TEST
    __asm__ volatile ("lidt %0" :: "m"(idtr) : "memory");     /* from here on, faults are reported */
#endif

    install_gdt_tss();
    set_gate(2,  isr_stub_table[2],  1);                       /* NMI   */
    set_gate(8,  isr_stub_table[8],  1);                       /* #DF   */
    set_gate(18, isr_stub_table[18], 1);                       /* #MC   */
    dbg_puts("[x64] GDT/TSS/IDT ready\n");
}

const char *arch_describe(void) {
    return "x86_64 long mode (0-4 GiB identity map, exception handlers + IST1 installed)";
}

void arch_crash_test(void) {
    dbg_puts("[x64] crashtest: executing ud2\n");
#ifndef HOST_TEST
    __asm__ volatile ("ud2");
#endif
}

/* ------------------------------------------------------------------------------------------------ dump */
static const char *const EXC_NAME[32] = {
    "#DE Divide Error", "#DB Debug", "NMI", "#BP Breakpoint", "#OF Overflow", "#BR Bound Range Exceeded",
    "#UD Invalid Opcode", "#NM Device Not Available", "#DF Double Fault", "Coprocessor Segment Overrun",
    "#TS Invalid TSS", "#NP Segment Not Present", "#SS Stack-Segment Fault", "#GP General Protection Fault",
    "#PF Page Fault", "Reserved", "#MF x87 Floating-Point", "#AC Alignment Check", "#MC Machine Check",
    "#XM SIMD Floating-Point", "#VE Virtualization", "#CP Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "#HV Hypervisor Injection", "#VC VMM Communication",
    "#SX Security", "Reserved"
};

static u64 read_cr(int n) {
    u64 v = 0;
#ifdef HOST_TEST
    (void)n;
#else
    switch (n) {
        case 0: __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); break;
        case 2: __asm__ volatile ("mov %%cr2, %0" : "=r"(v)); break;
        case 3: __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); break;
        case 4: __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); break;
    }
#endif
    return v;
}

static void hex16(char *dst, u64 v) {
    static const char h[] = "0123456789abcdef";
    dst[0] = '0'; dst[1] = 'x';
    for (int i = 0; i < 16; i++) dst[2 + i] = h[(v >> ((15 - i) * 4)) & 0xF];
    dst[18] = 0;
}

/* appends "NAME=0x................ " to line */
static int add_reg(char *line, int pos, const char *name, u64 v) {
    while (*name) line[pos++] = *name++;
    line[pos++] = '=';
    hex16(line + pos, v);
    pos += 18;
    line[pos++] = ' ';
    line[pos] = 0;
    return pos;
}

static void emit(const char *s, int screen, u8 fg) {
    dbg_puts(s);
    if (screen) vga_puts_col(s, fg, VGA_BLACK);
}

void trap_dump(const trap_frame_t *f, int screen) {
    char line[100];
    emit("\n*** VESPEROS KERNEL EXCEPTION (x86_64) ***\n", screen, VGA_LIGHT_RED);

    int p = 0;
    const char *nm = f->vector < 32 ? EXC_NAME[f->vector] : "?";
    while (*nm) line[p++] = *nm++;
    line[p] = 0;
    emit(line, screen, VGA_YELLOW);
    p = add_reg(line, 0, "  vector", f->vector);
    p = add_reg(line, p, "err", f->error);
    emit(line, screen, VGA_YELLOW); emit("\n", screen, VGA_WHITE);

    p = add_reg(line, 0, "RIP", f->rip);   p = add_reg(line, p, "CS", f->cs);     add_reg(line, p, "RFL", f->rflags);
    emit(line, screen, VGA_WHITE); emit("\n", screen, VGA_WHITE);
    p = add_reg(line, 0, "RSP", f->rsp);   p = add_reg(line, p, "SS", f->ss);     add_reg(line, p, "CR2", read_cr(2));
    emit(line, screen, VGA_WHITE); emit("\n", screen, VGA_WHITE);
    p = add_reg(line, 0, "CR0", read_cr(0)); p = add_reg(line, p, "CR3", read_cr(3)); add_reg(line, p, "CR4", read_cr(4));
    emit(line, screen, VGA_WHITE); emit("\n", screen, VGA_WHITE);

    p = add_reg(line, 0, "RAX", f->rax); p = add_reg(line, p, "RBX", f->rbx); add_reg(line, p, "RCX", f->rcx);
    emit(line, screen, VGA_LIGHT_GREY); emit("\n", screen, VGA_WHITE);
    p = add_reg(line, 0, "RDX", f->rdx); p = add_reg(line, p, "RSI", f->rsi); add_reg(line, p, "RDI", f->rdi);
    emit(line, screen, VGA_LIGHT_GREY); emit("\n", screen, VGA_WHITE);
    p = add_reg(line, 0, "RBP", f->rbp); p = add_reg(line, p, "R8", f->r8);   add_reg(line, p, "R9", f->r9);
    emit(line, screen, VGA_LIGHT_GREY); emit("\n", screen, VGA_WHITE);
    p = add_reg(line, 0, "R10", f->r10); p = add_reg(line, p, "R11", f->r11); add_reg(line, p, "R12", f->r12);
    emit(line, screen, VGA_LIGHT_GREY); emit("\n", screen, VGA_WHITE);
    p = add_reg(line, 0, "R13", f->r13); p = add_reg(line, p, "R14", f->r14); add_reg(line, p, "R15", f->r15);
    emit(line, screen, VGA_LIGHT_GREY); emit("\n", screen, VGA_WHITE);

    p = add_reg(line, 0, "kernel", (u64)(uptr)__kernel_start); add_reg(line, p, "..", (u64)(uptr)__kernel_end);
    emit(line, screen, VGA_DARK_GREY); emit("\n", screen, VGA_WHITE);
    u64 rip = f->rip, ks = (u64)(uptr)__kernel_start, ke = (u64)(uptr)__kernel_end;
    if (rip >= ks && rip < ke) {
        add_reg(line, 0, "RIP_offset_in_image", rip - ks);
        emit(line, screen, VGA_DARK_GREY); emit("\n", screen, VGA_WHITE);
        emit("(run 'nm -n vesperos4-x86_64.bin' on the build host to name the function)\n", screen, VGA_DARK_GREY);
    } else {
        emit("RIP is OUTSIDE the kernel image (jumped through a bad pointer?)\n", screen, VGA_YELLOW);
    }

    /* a few stack words, but only if RSP is inside our own stack, so reading cannot fault */
    u64 sp = f->rsp, sb = (u64)(uptr)boot_stack_bottom, st = (u64)(uptr)boot_stack_top;
    if (sp >= sb && sp + 48 <= st && (sp & 7) == 0) {
        const u64 *q = (const u64 *)(uptr)sp;
        p = add_reg(line, 0, "[rsp+00]", q[0]); p = add_reg(line, p, "+08", q[1]); add_reg(line, p, "+10", q[2]);
        emit(line, screen, VGA_DARK_GREY); emit("\n", screen, VGA_WHITE);
        p = add_reg(line, 0, "[rsp+18]", q[3]); p = add_reg(line, p, "+20", q[4]); add_reg(line, p, "+28", q[5]);
        emit(line, screen, VGA_DARK_GREY); emit("\n", screen, VGA_WHITE);
    }
    emit("System halted. Reboot; pick the i686 entry for a working system.\n", screen, VGA_LIGHT_RED);
}

TESTABLE volatile int in_exception = 0;

void exception_dispatch(void *frame) {
    trap_frame_t *f = (trap_frame_t *)frame;

    if (f->vector == 2) {                  /* NMI: log quietly and carry on */
        dbg_puts("[x64] NMI ignored\n");
        return;
    }
    if (in_exception) {                    /* fault inside the handler: debug ports only, never recurse */
        dbg_puts("\n[x64] FATAL: exception while handling an exception\n");
        trap_dump(f, 0);
    } else {
        in_exception = 1;
        trap_dump(f, 0);                   /* serial first: cannot fault */
        if (!vga_is_initialized()) vga_init();
        trap_dump(f, 1);
    }
#ifdef HOST_TEST
    return;
#else
    for (;;) __asm__ volatile ("cli; hlt");
#endif
}
