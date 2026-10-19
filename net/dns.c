#include "dns.h"
#include "netif.h"
#include "udp.h"
#include "netutil.h"
#include "../kernel/string.h"

u16 dns_build_query(u8 *buf, u16 bufsz, const char *name, u16 id) {
    u32 nlen = (u32)k_strlen(name);
    if (nlen == 0 || nlen > 253 || bufsz < 12 + nlen + 2 + 4) return 0;
    k_memset(buf, 0, 12);
    wr16(buf, id);
    wr16(buf + 2, 0x0100);    /* recursion desired */
    wr16(buf + 4, 1);         /* one question */
    u8 *o = buf + 12;
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        u32 l = (u32)(dot - p);
        if (l == 0 || l > 63) return 0;      /* empty or oversized label */
        *o++ = (u8)l;
        k_memcpy(o, p, l); o += l;
        p = dot;
        if (*p == '.') { p++; if (!*p) break; }   /* tolerate one trailing dot */
    }
    *o++ = 0;
    wr16(o, 1); o += 2;       /* QTYPE A */
    wr16(o, 1); o += 2;       /* QCLASS IN */
    return (u16)(o - buf);
}

/* Skips an encoded name (labels or compression pointer); returns new offset or -1. */
static int skip_name(const u8 *b, u16 len, u16 off) {
    int hops = 0;
    while (off < len) {
        u8 c = b[off];
        if (c == 0) return off + 1;
        if ((c & 0xC0) == 0xC0) return (off + 2 <= len) ? off + 2 : -1;
        if (c & 0xC0) return -1;
        off = (u16)(off + 1 + c);
        if (++hops > 128) return -1;
    }
    return -1;
}

int dns_parse_response(const u8 *b, u16 len, u16 id, u32 *ip) {
    if (len < 12 || rd16(b) != id) return -1;
    u16 flags = rd16(b + 2);
    if (!(flags & 0x8000)) return -1;                 /* not a response */
    if (flags & 0x000F) return -2;                    /* RCODE != 0 */
    u16 qd = rd16(b + 4), an = rd16(b + 6);

    int off = 12;
    for (u16 i = 0; i < qd; i++) {
        off = skip_name(b, len, (u16)off);
        if (off < 0 || off + 4 > len) return -1;
        off += 4;
    }
    for (u16 i = 0; i < an; i++) {
        off = skip_name(b, len, (u16)off);
        if (off < 0 || off + 10 > len) return -1;
        u16 type = rd16(b + off), cls = rd16(b + off + 2), rdlen = rd16(b + off + 8);
        off += 10;
        if (off + rdlen > len) return -1;
        if (type == 1 && cls == 1 && rdlen == 4) { *ip = rd32(b + off); return 1; }
        off += rdlen;                                  /* CNAME etc: skip, A record follows */
    }
    return 0;
}

/* ---- blocking resolver ---- */
static volatile bool resp_ready;
static u8  resp[512];
static u16 resp_len;
static u16 want_id;          /* only a reply with this transaction ID ... */
static u32 want_server;      /* ... from this server may occupy the mailbox */

static void dns_rx(netif_t *nif, u32 src, u16 sport, const u8 *data, u16 len) {
    (void)nif;
    if (sport != 53 || src != want_server || len < 12 || len > sizeof(resp) || resp_ready) return;
    if (rd16(data) != want_id) return;     /* spoofed/stale reply must not displace the real one */
    k_memcpy(resp, data, len);
    resp_len = len;
    resp_ready = true;
}

int dns_resolve(const char *name, u32 *ip) {
    if (ip_parse(name, ip)) return 1;

    u32 server = 0;
    for (int i = 0; i < netif_count(); i++) {
        netif_t *n = netif_at(i);
        if (n->configured && n->link_up && n->dns) { server = n->dns; break; }
    }
    if (!server) return -1;

    static u16 next_id = 0x2B00;
    static u16 next_port = 49152;
    for (int attempt = 0; attempt < 3; attempt++) {
        u16 id = (u16)(next_id++ ^ (net_time_ms() & 0xFF));
        u16 port = next_port++;
        if (next_port > 60000) next_port = 49152;
        u8 q[300];
        u16 qlen = dns_build_query(q, sizeof(q), name, id);
        if (!qlen) return -1;

        resp_ready = false;
        want_id = id;
        want_server = server;
        if (udp_bind(port, dns_rx) != 0) return -1;
        int sr = udp_send(server, port, 53, q, qlen);
        int result = 0;
        if (sr == 0) {
            u32 t0 = net_time_ms();
            while ((u32)(net_time_ms() - t0) < 1500) {
                net_poll_all();
                if (resp_ready) {
                    int pr = dns_parse_response(resp, resp_len, id, ip);
                    if (pr == 1) result = 1;
                    else if (pr == -2) result = -2;
                    else if (pr == 0) result = -3;
                    else { resp_ready = false; continue; }   /* mismatched/garbled: keep waiting */
                    break;
                }
            }
        } else {
            result = -1;
        }
        udp_unbind(port);
        if (result != 0) return result;
    }
    return 0;
}
