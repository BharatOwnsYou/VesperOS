#ifndef HESPERNET_IPV4_H
#define HESPERNET_IPV4_H
#include "netif.h"

#define IPPROTO_ICMP 1
#define IPPROTO_UDP  17
#define IPV4_MAX_PAYLOAD 1480

void ipv4_input(netif_t *nif, const u8 *pkt, u16 len, const u8 src_mac[6]);

/* Lowest-level send: caller supplies interface, source IP and next-hop MAC.
 * Never blocks (used for DHCP and for replies from RX handlers). */
int  ipv4_send_on(netif_t *nif, u32 src, u32 dst, u8 proto,
                  const u8 *payload, u16 plen, const u8 dst_mac[6]);

/* Routed send: picks an interface, resolves the next hop via ARP (blocking).
 * 0 = sent, -1 = no route, -2 = ARP failed, -3 = too big, -4 = driver error. */
int  ipv4_send(u32 dst, u8 proto, const u8 *payload, u16 plen);

netif_t *net_route(u32 dst, u32 *next_hop);

#endif
