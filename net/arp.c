#include "arp.h"
#include "netutil.h"
#include "../kernel/string.h"

#define ARP_CACHE      16
#define ARP_TTL_MS     (5u * 60u * 1000u)
#define ARP_TRIES      3
#define ARP_WAIT_MS    400

typedef struct { u32 ip; u8 mac[6]; u32 stamp; bool valid; } arp_entry_t;
static arp_entry_t cache[ARP_CACHE];

void arp_flush(void) { k_memset(cache, 0, sizeof(cache)); }

int arp_entry_count(void) {
    int n = 0;
    for (int i = 0; i < ARP_CACHE; i++) if (cache[i].valid) n++;
    return n;
}

static void arp_learn(u32 ip, const u8 mac[6]) {
    u32 now = net_time_ms();
    int slot = -1, oldest = 0;
    for (int i = 0; i < ARP_CACHE; i++) {
        if (cache[i].valid && cache[i].ip == ip) { slot = i; break; }
        if (!cache[i].valid) { if (slot < 0) slot = i; }
        else if ((u32)(now - cache[i].stamp) > (u32)(now - cache[oldest].stamp)) oldest = i;
    }
    if (slot < 0) slot = oldest;
    cache[slot].ip = ip;
    k_memcpy(cache[slot].mac, mac, 6);
    cache[slot].stamp = now;
    cache[slot].valid = true;
}

int arp_lookup(u32 ip, u8 mac_out[6]) {
    u32 now = net_time_ms();
    for (int i = 0; i < ARP_CACHE; i++) {
        if (cache[i].valid && cache[i].ip == ip) {
            if ((u32)(now - cache[i].stamp) > ARP_TTL_MS) { cache[i].valid = false; return 0; }
            k_memcpy(mac_out, cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

/* Builds an ARP packet in the shared tx buffer and sends it. */
static void arp_send(netif_t *nif, u16 op, const u8 dst_mac[6],
                     const u8 tha[6], u32 tpa) {
    u8 *p = net_txbuf() + ETH_HLEN;
    wr16(p + 0, 1);              /* hardware type: Ethernet */
    wr16(p + 2, ETHERTYPE_IPV4); /* protocol type */
    p[4] = 6; p[5] = 4;
    wr16(p + 6, op);
    k_memcpy(p + 8, nif->mac, 6);
    wr32(p + 14, nif->configured ? nif->ip : 0);
    k_memcpy(p + 18, tha, 6);
    wr32(p + 24, tpa);
    eth_send(nif, dst_mac, ETHERTYPE_ARP, 28);
}

void arp_input(netif_t *nif, const u8 *p, u16 len) {
    if (len < 28) { nif->rx_errors++; return; }
    if (rd16(p) != 1 || rd16(p + 2) != ETHERTYPE_IPV4 || p[4] != 6 || p[5] != 4) {
        nif->rx_dropped++; return;
    }
    u16 op = rd16(p + 6);
    const u8 *sha = p + 8;
    u32 spa = rd32(p + 14);
    u32 tpa = rd32(p + 24);
    bool to_us = nif->configured && tpa == nif->ip;

    u8 tmp[6];
    if (spa != 0 && spa != IP4_BROADCAST && (to_us || arp_lookup(spa, tmp)))
        arp_learn(spa, sha);

    if (op == 1 && to_us) {
        u8 requester[6];
        k_memcpy(requester, sha, 6);           /* sha points into the RX buffer; copy before reuse */
        arp_send(nif, 2, requester, requester, spa);
    }
}

int arp_resolve(netif_t *nif, u32 ip, u8 mac_out[6]) {
    if (ip == IP4_BROADCAST) { k_memcpy(mac_out, ETH_BROADCAST, 6); return 1; }
    if (arp_lookup(ip, mac_out)) return 1;

    static const u8 zero[6] = { 0, 0, 0, 0, 0, 0 };
    for (int attempt = 0; attempt < ARP_TRIES; attempt++) {
        arp_send(nif, 1, ETH_BROADCAST, zero, ip);
        u32 t0 = net_time_ms();
        while ((u32)(net_time_ms() - t0) < ARP_WAIT_MS) {
            net_poll_all();
            if (arp_lookup(ip, mac_out)) return 1;
        }
    }
    return 0;
}
