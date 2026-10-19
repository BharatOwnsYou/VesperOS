#include "netif.h"
#include "netutil.h"
#include "arp.h"
#include "ipv4.h"
#include "../kernel/string.h"

static netif_t ifs[NETIF_MAX];
static int if_count = 0;
static u8 txbuf[1536];
static int in_poll = 0;

const u8 ETH_BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

netif_t *netif_register(const char *driver, const char *desc, const u8 mac[6],
                        int (*send)(netif_t *, const u8 *, u16),
                        void (*poll)(netif_t *)) {
    if (if_count >= NETIF_MAX) return NULL;
    netif_t *n = &ifs[if_count];
    k_memset(n, 0, sizeof(*n));
    n->index = if_count;
    n->used = true;
    char d[8];
    k_itoa((u32)if_count, d, 10);
    k_strcpy(n->name, "eth");
    k_strcat(n->name, d);
    net_strlcpy(n->driver, driver, sizeof(n->driver));
    net_strlcpy(n->desc, desc, sizeof(n->desc));
    k_memcpy(n->mac, mac, 6);
    n->send = send;
    n->poll = poll;
    if_count++;
    return n;
}

int netif_count(void) { return if_count; }
netif_t *netif_at(int i) { return (i >= 0 && i < if_count) ? &ifs[i] : NULL; }

netif_t *netif_by_name(const char *name) {
    for (int i = 0; i < if_count; i++)
        if (k_streq(ifs[i].name, name)) return &ifs[i];
    return NULL;
}

u8 *net_txbuf(void) { return txbuf; }

void net_poll_all(void) {
    if (in_poll) return;
    in_poll = 1;
    for (int i = 0; i < if_count; i++)
        if (ifs[i].poll) ifs[i].poll(&ifs[i]);
    in_poll = 0;
}

int eth_send(netif_t *nif, const u8 dst[6], u16 ethertype, u16 payload_len) {
    u32 total = (u32)ETH_HLEN + payload_len;
    if (total > ETH_FRAME_MAX) { nif->tx_errors++; return -1; }
    k_memcpy(txbuf, dst, 6);
    k_memcpy(txbuf + 6, nif->mac, 6);
    wr16(txbuf + 12, ethertype);
    if (total < ETH_ZLEN) {                    /* pad runt frames with zeros */
        k_memset(txbuf + total, 0, ETH_ZLEN - total);
        total = ETH_ZLEN;
    }
    if (!nif->send || nif->send(nif, txbuf, (u16)total) != 0) { nif->tx_errors++; return -1; }
    nif->tx_packets++;
    nif->tx_bytes += total;
    return 0;
}

void netif_rx(netif_t *nif, const u8 *frame, u16 len) {
    if (len < ETH_HLEN) { nif->rx_errors++; return; }
    nif->rx_packets++;
    nif->rx_bytes += len;

    /* software MAC filter: ours, broadcast or multicast bit set */
    bool for_us = (frame[0] & 1) || net_mem_eq(frame, nif->mac, 6);
    if (!for_us) { nif->rx_dropped++; return; }

    u16 type = rd16(frame + 12);
    const u8 *payload = frame + ETH_HLEN;
    u16 plen = (u16)(len - ETH_HLEN);
    if (type == ETHERTYPE_ARP)       arp_input(nif, payload, plen);
    else if (type == ETHERTYPE_IPV4) ipv4_input(nif, payload, plen, frame + 6);
    else                             nif->rx_dropped++;
}
