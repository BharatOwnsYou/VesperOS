#include "netutil.h"
#include "../kernel/string.h"

u32 net_csum_add(u32 sum, const u8 *data, u32 len) {
    while (len > 1) {
        sum += ((u32)data[0] << 8) | data[1];
        data += 2; len -= 2;
    }
    if (len) sum += (u32)data[0] << 8;   /* odd trailing byte is the high half of a final word */
    return sum;
}

u16 net_csum_fold(u32 sum) {
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)~sum;
}

u16 net_checksum(const u8 *data, u32 len) {
    return net_csum_fold(net_csum_add(0, data, len));
}

void net_strlcpy(char *dst, const char *src, u32 max) {
    if (max == 0) return;
    u32 i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int net_mem_eq(const u8 *a, const u8 *b, u32 n) {
    while (n--) if (*a++ != *b++) return 0;
    return 1;
}

void ip_to_str(u32 ip, char out[16]) {
    char part[12];
    out[0] = 0;
    for (int i = 3; i >= 0; i--) {
        k_itoa((ip >> (i * 8)) & 0xFFu, part, 10);
        k_strcat(out, part);
        if (i) k_strcat(out, ".");
    }
}

int ip_parse(const char *s, u32 *out) {
    u32 ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        u32 v = 0; int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (u32)(*s - '0');
            if (++digits > 3 || v > 255) return 0;
            s++;
        }
        ip = (ip << 8) | v;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *out = ip;
    return 1;
}

void mac_to_str(const u8 mac[6], char out[18]) {
    static const char hex[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < 6; i++) {
        out[o++] = hex[mac[i] >> 4];
        out[o++] = hex[mac[i] & 0xF];
        if (i < 5) out[o++] = ':';
    }
    out[o] = 0;
}

u32 ip_default_mask(u32 ip) {
    if ((ip >> 31) == 0)       return 0xFF000000u;  /* class A */
    if ((ip >> 30) == 2)       return 0xFFFF0000u;  /* class B */
    return 0xFFFFFF00u;                              /* class C and everything else */
}
