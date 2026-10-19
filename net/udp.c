#include "udp.h"
#include "ipv4.h"
#include "netutil.h"
#include "../kernel/string.h"

#define UDP_BINDINGS 8
typedef struct { u16 port; udp_handler_t fn; } udp_bind_t;
static udp_bind_t binds[UDP_BINDINGS];

int udp_bind(u16 port, udp_handler_t fn) {
    int freeslot = -1;
    for (int i = 0; i < UDP_BINDINGS; i++) {
        if (binds[i].fn && binds[i].port == port) return -1;
        if (!binds[i].fn && freeslot < 0) freeslot = i;
    }
    if (freeslot < 0) return -1;
    binds[freeslot].port = port;
    binds[freeslot].fn = fn;
    return 0;
}

void udp_unbind(u16 port) {
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (binds[i].fn && binds[i].port == port) binds[i].fn = NULL;
}

static u16 udp_checksum(u32 src, u32 dst, const u8 *dgram, u16 len) {
    u8 pseudo[12];
    wr32(pseudo, src);
    wr32(pseudo + 4, dst);
    pseudo[8] = 0;
    pseudo[9] = IPPROTO_UDP;
    wr16(pseudo + 10, len);
    u32 sum = net_csum_add(0, pseudo, 12);
    sum = net_csum_add(sum, dgram, len);
    return net_csum_fold(sum);
}

void udp_input(netif_t *nif, u32 src, u32 dst, const u8 *p, u16 len) {
    if (len < 8) { nif->rx_errors++; return; }
    u16 ulen = rd16(p + 4);
    if (ulen < 8 || ulen > len) { nif->rx_errors++; return; }
    if (rd16(p + 6) != 0 && udp_checksum(src, dst, p, ulen) != 0) { nif->rx_errors++; return; }

    u16 dport = rd16(p + 2);
    for (int i = 0; i < UDP_BINDINGS; i++) {
        if (binds[i].fn && binds[i].port == dport) {
            binds[i].fn(nif, src, rd16(p), p + 8, (u16)(ulen - 8));
            return;
        }
    }
    nif->rx_dropped++;
}

static int build_dgram(u8 *buf, u32 src, u32 dst, u16 sport, u16 dport,
                       const u8 *data, u16 len) {
    if (len > IPV4_MAX_PAYLOAD - 8) return -3;
    u16 total = (u16)(8 + len);
    wr16(buf, sport);
    wr16(buf + 2, dport);
    wr16(buf + 4, total);
    wr16(buf + 6, 0);
    k_memcpy(buf + 8, data, len);
    u16 c = udp_checksum(src, dst, buf, total);
    wr16(buf + 6, c == 0 ? 0xFFFF : c);
    return total;
}

int udp_send(u32 dst, u16 sport, u16 dport, const u8 *data, u16 len) {
    u32 nh = 0;
    netif_t *nif = net_route(dst, &nh);
    if (!nif) return -1;
    u8 buf[IPV4_MAX_PAYLOAD];
    int total = build_dgram(buf, nif->ip, dst, sport, dport, data, len);
    if (total < 0) return total;
    return ipv4_send(dst, IPPROTO_UDP, buf, (u16)total);
}

int udp_send_broadcast(netif_t *nif, u16 sport, u16 dport, const u8 *data, u16 len) {
    u32 src = nif->configured ? nif->ip : 0;
    u8 buf[IPV4_MAX_PAYLOAD];
    int total = build_dgram(buf, src, IP4_BROADCAST, sport, dport, data, len);
    if (total < 0) return total;
    return ipv4_send_on(nif, src, IP4_BROADCAST, IPPROTO_UDP, buf, (u16)total, ETH_BROADCAST);
}
