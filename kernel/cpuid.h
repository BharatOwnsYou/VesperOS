#ifndef VESPEROS_CPUID_H
#define VESPEROS_CPUID_H
#include "../include/types.h"

/* fills a 13-byte buffer (12 chars + NUL) with the CPU vendor string, e.g. "AuthenticAMD" */
void cpuid_vendor(char out[13]);
/* fills a 49-byte buffer with the brand string if the CPU supports it, else empty string */
void cpuid_brand(char out[49]);

#endif
