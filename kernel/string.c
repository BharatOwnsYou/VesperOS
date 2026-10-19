#include "string.h"

size_t k_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int k_strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int k_strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

int k_streq(const char *a, const char *b) {
    return k_strcmp(a, b) == 0;
}

char *k_strcpy(char *dst, const char *src) {
    char *o = dst;
    while ((*dst++ = *src++));
    return o;
}

char *k_strcat(char *dst, const char *src) {
    char *o = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++));
    return o;
}

void *k_memset(void *dst, int val, size_t n) {
    u8 *d = (u8*)dst;
    while (n--) *d++ = (u8)val;
    return dst;
}

void *k_memcpy(void *dst, const void *src, size_t n) {
    u8 *d = (u8*)dst;
    const u8 *s = (const u8*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void k_itoa(u32 val, char *buf, int base) {
    char tmp[32];
    int i = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (val) {
        int d = val % base;
        tmp[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        val /= base;
    }
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
}

/* splits line in place on whitespace, fills argv, returns argc */
int k_split_ws(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = 0; p++; }
    }
    return argc;
}
