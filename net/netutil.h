#ifndef HESPERNET_NETUTIL_H
#define HESPERNET_NETUTIL_H
#include "../include/types.h"

/* big-endian (network order) accessors. all protocol code goes through these instead of casting
 * packet bytes to structs, so alignment/packing never matters */
static inline u16 rd16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static inline u32 rd32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static inline void wr16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void wr32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

/* IPv4 addresses are kept in host byte order everywhere: 10.0.2.15 == 0x0A00020F */
#define IP4(a,b,c,d) ((((u32)(a)) << 24) | (((u32)(b)) << 16) | (((u32)(c)) << 8) | ((u32)(d)))
#define IP4_BROADCAST 0xFFFFFFFFu

/* RFC 1071 internet checksum. net_csum_add() accumulates (call repeatedly for
 * pseudo-headers), net_csum_fold() folds + complements. */
u32 net_csum_add(u32 sum, const u8 *data, u32 len);
u16 net_csum_fold(u32 sum);
u16 net_checksum(const u8 *data, u32 len);

void ip_to_str(u32 ip, char out[16]);
int  ip_parse(const char *s, u32 *out);       /* strict dotted quad; 1 = ok */
void mac_to_str(const u8 mac[6], char out[18]);
void net_strlcpy(char *dst, const char *src, u32 max);
int  net_mem_eq(const u8 *a, const u8 *b, u32 n);   /* 1 if equal */
u32  ip_default_mask(u32 ip);                  /* classful fallback if a server sends no mask */

/* Monotonic millisecond clock + busy delay. Kernel: net/nettime.c (PIT).
 * Host tests supply their own fake clock. Compare with (u32)(now - then). */
u32  net_time_ms(void);
void net_delay_ms(u32 ms);

#endif
