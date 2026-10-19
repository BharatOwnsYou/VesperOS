#ifndef VESPEROS_TYPES_H
#define VESPEROS_TYPES_H

#ifdef HOST_TEST
/* host-side tests (tests/nettest.c) build the pure-logic net code on a normal 64-bit box.
 * never defined for the kernel */
#include <stdint.h>
#include <stddef.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef uintptr_t uptr;
#else
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        s8;
typedef signed short       s16;
typedef signed int         s32;
/* Compiler-provided so they are right for both i686 (32-bit) and x86_64 (LP64). */
typedef __SIZE_TYPE__      size_t;
typedef __UINTPTR_TYPE__   uptr;    /* integer big enough to hold a pointer: use for every ptr<->int cast */
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef __cplusplus
typedef u8 bool;
#define true 1
#define false 0
#else
/* C++ has built-in bool, true, false */
#endif

#endif
