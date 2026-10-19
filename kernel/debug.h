#ifndef VESPEROS_DEBUG_H
#define VESPEROS_DEBUG_H
#include "../include/types.h"

/* debug output that works without a screen: qemu debugcon (0xE9) and COM1 (0x3F8).
 * no-ops on hw with no UART (the wait loops are bounded) */
void dbg_putc(char c);
void dbg_puts(const char *s);
void dbg_hex(u64 v, int digits);   /* prints 0x + exactly `digits` hex digits */
#endif
