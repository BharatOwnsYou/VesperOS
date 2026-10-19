#include "../include/version.h"
#include "dhcp.h"
#include "netutil.h"
#include "../kernel/string.h"

#define BOOTP_FIXED 236
static const u8 MAGIC[4] = { 99, 130, 83, 99 };
static const char HOSTNAME[] = VESPER_HOSTNAME;     /* DHCP option 12 */

u16 dhcp_build(u8 *b, u8 type, const u8 mac[6], u32 xid,
               u32 ciaddr, u32 req_ip, u32 server_id) {
    k_memset(b, 0, 320);
    b[0] = 1;                 /* BOOTREQUEST */
    b[1] = 1;                 /* Ethernet */
    b[2] = 6;
    wr32(b + 4, xid);
    wr16(b + 10, 0x8000);     /* broadcast flag: we have no IP to receive unicast on yet */
    wr32(b + 12, ciaddr);
    k_memcpy(b + 28, mac, 6); /* chaddr */
    k_memcpy(b + BOOTP_FIXED, MAGIC, 4);

    u8 *o = b + BOOTP_FIXED + 4;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 61; *o++ = 7; *o++ = 1; k_memcpy(o, mac, 6); o += 6;       /* client identifier */
    if (req_ip)    { *o++ = 50; *o++ = 4; wr32(o, req_ip);    o += 4; }
    if (server_id) { *o++ = 54; *o++ = 4; wr32(o, server_id); o += 4; }
    *o++ = 12; *o++ = (u8)(sizeof(HOSTNAME) - 1);
    k_memcpy(o, HOSTNAME, sizeof(HOSTNAME) - 1); o += sizeof(HOSTNAME) - 1;
    *o++ = 55; *o++ = 5; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15; *o++ = 51;  /* param request list */
    *o++ = 255;
    u16 len = (u16)(o - b);
    return len < 300 ? 300 : len;   /* some servers insist on >= 300 bytes */
}

int dhcp_parse(const u8 *p, u16 len, const u8 mac[6], u32 xid, dhcp_msg_t *m) {
    k_memset(m, 0, sizeof(*m));
    if (len < BOOTP_FIXED + 4 + 3) return 0;
    if (p[0] != 2 || p[1] != 1 || p[2] != 6) return 0;
    if (rd32(p + 4) != xid) return 0;
    if (!net_mem_eq(p + 28, mac, 6)) return 0;
    if (!net_mem_eq(p + BOOTP_FIXED, MAGIC, 4)) return 0;

    m->yiaddr = rd32(p + 16);
    u32 i = BOOTP_FIXED + 4;
    while (i < len) {
        u8 code = p[i++];
        if (code == 0) continue;           /* pad */
        if (code == 255) break;            /* end */
        if (i >= len) return 0;
        u8 olen = p[i++];
        if (i + olen > len) return 0;
        const u8 *v = p + i;
        switch (code) {
            case 53: if (olen == 1) m->type = v[0]; break;
            case 54: if (olen == 4) { m->server_id = rd32(v); m->has_server_id = true; } break;
            case 1:  if (olen == 4) { m->mask = rd32(v);   m->has_mask = true; } break;
            case 3:  if (olen >= 4) { m->router = rd32(v); m->has_router = true; } break;
            case 6:  if (olen >= 4) { m->dns = rd32(v);    m->has_dns = true; } break;
            case 51: if (olen == 4) { m->lease_s = rd32(v); m->has_lease = true; } break;
            default: break;
        }
        i += olen;
    }
    return m->type != 0;
}
