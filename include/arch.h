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


void arch_early_init(void);

/* raises a cpu exception on puorpose  to test the fault handler (via using 'crashtest' shell cmd).
 * will returns normally and does nothing on archs without a handler */
void arch_crash_test(void);

/* Human-readable for the boot log. */
const char *arch_describe(void);
#endif
