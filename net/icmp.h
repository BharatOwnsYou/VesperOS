#ifndef HESPERNET_ICMP_H
#define HESPERNET_ICMP_H
#include "netif.h"

void icmp_input(netif_t *nif, u32 src, u32 dst, u8 ttl,
                const u8 *msg, u16 len, const u8 src_mac[6]);

/* Blocking echo request. Returns:
 *   1 = reply received (*rtt_ms and *ttl_out filled in)
 *   0 = timed out
 *  -1 = no route, -2 = ARP failed, -3/-4 = send error */
int icmp_ping(u32 dst, u16 seq, u16 payload_len, u32 timeout_ms,
              u32 *rtt_ms, u8 *ttl_out);

#endif
