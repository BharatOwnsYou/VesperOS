#ifndef VESPEROS_ARCH_H
#define VESPEROS_ARCH_H
#include "types.h"

#if defined(__x86_64__)
#  define VESPER_ARCH       "x86_64"
#  define VESPER_ARCH_BITS  64
#else
#  define VESPER_ARCH       "x86_32"
#  define VESPER_ARCH_BITS  32
#endif

/* first thing in kernel_main(). x86_64: installs IDT/GDT/TSS so a cpu fault prints a register dump
 * instead of silently rebooting. i686: nothing to do */
void arch_early_init(void);

/* raises a cpu exception on purpose to test the fault handler ('crashtest' shell cmd).
 * returns normally and does nothing on archs without a handler */
void arch_crash_test(void);

/* Human-readable one-liner describing the active arch features, for the boot log. */
const char *arch_describe(void);
#endif
