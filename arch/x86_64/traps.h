#ifndef VESPEROS_X64_TRAPS_H
#define VESPEROS_X64_TRAPS_H
#include "../../include/types.h"

/* must match the push order in isr.S (gen_isr.py), lowest address first. 22 qwords = 176 bytes */
typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 vector, error;
    u64 rip, cs, rflags, rsp, ss;         /* pushed by the CPU */
} trap_frame_t;

/* 64-bit IDT gate (Intel SDM Vol.3 6.14.1): 16 bytes. */
typedef struct { u16 off_lo; u16 sel; u8 ist; u8 attr; u16 off_mid; u32 off_hi; u32 zero; } __attribute__((packed)) idt_entry_t;
/* 64-bit TSS (Intel SDM Vol.3 7.7): 104 bytes. */
typedef struct {
    u32 rsvd0; u64 rsp0, rsp1, rsp2; u64 rsvd1; u64 ist[7]; u64 rsvd2; u16 rsvd3; u16 iomap_base;
} __attribute__((packed)) tss_t;

void idt_make_gate(idt_entry_t *e, u64 handler, u8 ist);
/* 16-byte available-64-bit-TSS descriptor: out[0] = low qword, out[1] = high qword */
void gdt_make_tss(u64 out[2], u64 base, u32 limit);
void trap_dump(const trap_frame_t *f, int screen);   /* register dump to the debug ports (and the screen if screen != 0) */
void exception_dispatch(void *frame);
#endif
