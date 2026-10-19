#include "ipv4.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "netutil.h"
#include "../kernel/string.h"

static u16 ip_ident = 0x4800;

netif_t *net_route(u32 dst, u32 *next_hop) {
    netif_t *via_gw = NULL;
    for (int i = 0; i < netif_count(); i++) {
        netif_t *n = netif_at(i);
        if (!n->configured || !n->link_up) continue;
        if ((dst & n->mask) == (n->ip & n->mask)) { *next_hop = dst; return n; }
        if (!via_gw && n->gw) via_gw = n;
    }
    if (via_gw) { *next_hop = via_gw->gw; return via_gw; }
    return NULL;
}

int ipv4_send_on(netif_t *nif, u32 src, u32 dst, u8 proto,
                 const u8 *payload, u16 plen, const u8 dst_mac[6]) {
    if (plen > IPV4_MAX_PAYLOAD) return -3;
    u8 *h = net_txbuf() + ETH_HLEN;
    u16 total = (u16)(20 + plen);
    h[0] = 0x45;                 /* v4, 20-byte header */
    h[1] = 0;
    wr16(h + 2, total);
    wr16(h + 4, ip_ident++);
    wr16(h + 6, 0);              /* no fragmentation (we never send > MTU) */
    h[8] = 64;
    h[9] = proto;
    wr16(h + 10, 0);
    wr32(h + 12, src);
    wr32(h + 16, dst);
    wr16(h + 10, net_checksum(h, 20));
    k_memcpy(h + 20, payload, plen);
    return eth_send(nif, dst_mac, ETHERTYPE_IPV4, total) == 0 ? 0 : -4;
}

int ipv4_send(u32 dst, u8 proto, const u8 *payload, u16 plen) {
    if (plen > IPV4_MAX_PAYLOAD) return -3;
    u32 nh = 0;
    netif_t *nif = net_route(dst, &nh);
    if (!nif) return -1;

    u8 mac[6];
    if (dst == IP4_BROADCAST || dst == (nif->ip | ~nif->mask)) {
        k_memcpy(mac, ETH_BROADCAST, 6);
    } else if (!arp_resolve(nif, nh, mac)) {
        return -2;
    }
    return ipv4_send_on(nif, nif->ip, dst, proto, payload, plen, mac);
}

void ipv4_input(netif_t *nif, const u8 *pkt, u16 len, const u8 src_mac[6]) {
    if (len < 20) { nif->rx_errors++; return; }
    if ((pkt[0] >> 4) != 4) { nif->rx_dropped++; return; }
    u32 ihl = (u32)(pkt[0] & 0x0F) * 4;
    u16 total = rd16(pkt + 2);
    if (ihl < 20 || ihl > len || total < ihl || total > len) { nif->rx_errors++; return; }
    if (net_checksum(pkt, ihl) != 0) { nif->rx_errors++; return; }
    if (rd16(pkt + 6) & 0x3FFF) { nif->rx_dropped++; return; }   /* fragments: not supported */

    u8  ttl   = pkt[8];
    u8  proto = pkt[9];
    u32 src   = rd32(pkt + 12);
    u32 dst   = rd32(pkt + 16);

    bool mine;
    if (nif->configured)
        mine = dst == nif->ip || dst == IP4_BROADCAST || dst == (nif->ip | ~nif->mask);
    else  /* before DHCP completes: accept broadcast, and any UDP (offers may be unicast to yiaddr) */
        mine = dst == IP4_BROADCAST || proto == IPPROTO_UDP;
    if (!mine) { nif->rx_dropped++; return; }

    const u8 *payload = pkt + ihl;
    u16 plen = (u16)(total - ihl);
    if (proto == IPPROTO_ICMP)     icmp_input(nif, src, dst, ttl, payload, plen, src_mac);
    else if (proto == IPPROTO_UDP) udp_input(nif, src, dst, payload, plen);
    else                           nif->rx_dropped++;
}
