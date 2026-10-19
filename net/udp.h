#ifndef HESPERNET_UDP_H
#define HESPERNET_UDP_H
#include "netif.h"

/* handlers run in RX context: no blocking, no polling NICs, nothing that does (ARP-resolving sends included).
 * copy the data and return, act on it from the main loop */
typedef void (*udp_handler_t)(netif_t *nif, u32 src_ip, u16 src_port,
                              const u8 *data, u16 len);

int  udp_bind(u16 port, udp_handler_t handler);   /* 0 = ok, -1 = table full / in use */
void udp_unbind(u16 port);

void udp_input(netif_t *nif, u32 src, u32 dst, const u8 *p, u16 len);

/* Routed unicast send (blocking ARP). Return codes as ipv4_send(). */
int  udp_send(u32 dst, u16 sport, u16 dport, const u8 *data, u16 len);

/* Limited broadcast out of a specific interface, source 0.0.0.0 until the
 * interface is configured. Never blocks. Used by DHCP. */
int  udp_send_broadcast(netif_t *nif, u16 sport, u16 dport, const u8 *data, u16 len);

#endif
