#include "athenalibc.h"

typedef __builtin_va_list ac_va_list;
#define ac_va_start(ap, last) __builtin_va_start(ap, last)
#define ac_va_arg(ap, type)   __builtin_va_arg(ap, type)
#define ac_va_end(ap)         __builtin_va_end(ap)

void *k_memmove(void *dst, const void *src, size_t n) {
    u8 *d = (u8*)dst;
    const u8 *s = (const u8*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

char *k_strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return (c == 0) ? (char*)s : NULL;
}

char *k_strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

static void put_char_bounded(char *buf, size_t bufsz, size_t *pos, char c) {
    if (*pos < bufsz - 1) buf[*pos] = c;
    (*pos)++;
}

static void put_str_bounded(char *buf, size_t bufsz, size_t *pos, const char *s) {
    while (*s) put_char_bounded(buf, bufsz, pos, *s++);
}

static void put_uint_bounded(char *buf, size_t bufsz, size_t *pos, u32 val, int base, int is_signed, int negative) {
    char tmp[16];
    int i = 0;
    if (val == 0) { tmp[i++] = '0'; }
    while (val) {
        int d = (int)(val % (u32)base);
        tmp[i++] = (char)((d < 10) ? ('0' + d) : ('a' + d - 10));
        val /= (u32)base;
    }
    if (is_signed && negative) put_char_bounded(buf, bufsz, pos, '-');
    while (i) put_char_bounded(buf, bufsz, pos, tmp[--i]);
}

int k_vsnprintf(char *buf, size_t bufsz, const char *fmt, ac_va_list ap) {
    size_t pos = 0;
    if (bufsz == 0) return 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { put_char_bounded(buf, bufsz, &pos, *p); continue; }
        p++;
        if (!*p) break;
        switch (*p) {
            case 's': {
                const char *s = ac_va_arg(ap, const char*);
                put_str_bounded(buf, bufsz, &pos, s ? s : "(null)");
                break;
            }
            case 'd': {
                s32 v = ac_va_arg(ap, s32);
                u32 uv = (v < 0) ? (u32)(-v) : (u32)v;
                put_uint_bounded(buf, bufsz, &pos, uv, 10, 1, v < 0);
                break;
            }
            case 'u': {
                u32 v = ac_va_arg(ap, u32);
                put_uint_bounded(buf, bufsz, &pos, v, 10, 0, 0);
                break;
            }
            case 'x': {
                u32 v = ac_va_arg(ap, u32);
                put_uint_bounded(buf, bufsz, &pos, v, 16, 0, 0);
                break;
            }
            case 'c': {
                int v = ac_va_arg(ap, int);
                put_char_bounded(buf, bufsz, &pos, (char)v);
                break;
            }
            case '%': {
                put_char_bounded(buf, bufsz, &pos, '%');
                break;
            }
            default: {
                put_char_bounded(buf, bufsz, &pos, '%');
                put_char_bounded(buf, bufsz, &pos, *p);
                break;
            }
        }
    }

    size_t term = (pos < bufsz - 1) ? pos : bufsz - 1;
    buf[term] = 0;
    return (int)pos;
}

int k_snprintf(char *buf, size_t bufsz, const char *fmt, ...) {
    ac_va_list ap;
    ac_va_start(ap, fmt);
    int r = k_vsnprintf(buf, bufsz, fmt, ap);
    ac_va_end(ap);
    return r;
}

/* ---- POSIX-named string/memory wrappers - see athenalibc.h ---- */
void  *memcpy(void *dst, const void *src, size_t n)  { return k_memcpy(dst, src, n); }
void  *memset(void *dst, int val, size_t n)           { return k_memset(dst, val, n); }
void  *memmove(void *dst, const void *src, size_t n) { return k_memmove(dst, src, n); }
size_t strlen(const char *s)                          { return k_strlen(s); }
int    strcmp(const char *a, const char *b)           { return k_strcmp(a, b); }
int    strncmp(const char *a, const char *b, size_t n){ return k_strncmp(a, b, n); }
char  *strcpy(char *dst, const char *src)             { return k_strcpy(dst, src); }
char  *strcat(char *dst, const char *src)              { return k_strcat(dst, src); }
char  *strchr(const char *s, int c)                    { return k_strchr(s, c); }
char  *strstr(const char *haystack, const char *needle){ return k_strstr(haystack, needle); }
