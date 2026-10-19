#ifndef HESPERNET_ARP_H
#define HESPERNET_ARP_H
#include "netif.h"

void arp_input(netif_t *nif, const u8 *pkt, u16 len);
/* Blocking: sends up to 3 requests (400 ms each), polling NICs while waiting.
 * NEVER call from an RX handler. 1 = resolved, 0 = no answer. */
int  arp_resolve(netif_t *nif, u32 ip, u8 mac_out[6]);
int  arp_lookup(u32 ip, u8 mac_out[6]);
void arp_flush(void);
int  arp_entry_count(void);

#endif
