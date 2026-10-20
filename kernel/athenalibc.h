#ifndef ATHENALIBC_H
#define ATHENALIBC_H
#include "../include/types.h"
#include "string.h"

/* AthenaLibC: small freestanding libc. builds on the kernel's string/mem primitives (string.h, unchanged)
 * and adds some general helpers. offline_packages should include this instead of string.h from now on.
 *

void  *k_memmove(void *dst, const void *src, size_t n);
char  *k_strchr(const char *s, int c);
char  *k_strstr(const char *haystack, const char *needle);

/* very limited printf: only %s %d %u %x %c %%, no width/precision/padding. writes at most bufsz-1 bytes + NUL. */
 * returns the length it would have written (like snprintf) so truncation is detectable, nothing smarter than that */
int k_snprintf(char *buf, size_t bufsz, const char *fmt, ...);
/* same engine taking an already running one va_list, so other varargs funcs (stdio fprintf) can forward their arguments nigga */
int k_vsnprintf(char *buf, size_t bufsz, const char *fmt, __builtin_va_list ap);

/* ---- POSIX-named string/mem functions ----

void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int val, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);

#endif
