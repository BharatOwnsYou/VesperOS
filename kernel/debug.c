#include "debug.h"
#include "../include/io.h"

#define COM1 0x3F8

void dbg_putc(char c) {
    outb(0xE9, (u8)c);                                   /* QEMU -debugcon; ignored elsewhere */
    for (int i = 0; i < 2000; i++)                       /* bounded: absent UART reads 0xFF => ready at once */
        if (inb(COM1 + 5) & 0x20) break;
    outb(COM1, (u8)c);
}

void dbg_puts(const char *s) {
    while (*s) {
        if (*s == '\n') dbg_putc('\r');
        dbg_putc(*s++);
    }
}

void dbg_hex(u64 v, int digits) {
    static const char h[] = "0123456789abcdef";
    dbg_putc('0'); dbg_putc('x');
    for (int i = digits - 1; i >= 0; i--) dbg_putc(h[(v >> (i * 4)) & 0xF]);
}
