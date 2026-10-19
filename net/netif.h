#ifndef HESPERNET_NETIF_H
#define HESPERNET_NETIF_H
#include "../include/types.h"

#define NETIF_MAX       4
#define ETH_HLEN        14
#define ETH_ZLEN        60      /* minimum frame length (without FCS) */
#define ETH_FRAME_MAX   1514    /* max frame length (without FCS) */
#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806

/* one network interface = one NIC driver instance. drivers fill in send/poll and call netif_rx() for every
 * received frame. everything above this struct (ARP/IPv4/ICMP/UDP/DHCP/DNS/hespernetd) is NIC-agnostic */
typedef struct netif {
    int   index;
    char  name[8];        /* "eth0" */
    char  driver[16];     /* "rtl8168", "e1000", "virtio-net", "rtl8139" */
    char  desc[72];       /* human-readable model */
    u8    mac[6];
    bool  used;
    bool  link_up;        /* maintained by the driver's poll() */
    bool  has_pci;
    u8    pci_bus, pci_slot, pci_func;
    u16   pci_vendor, pci_device;

    /* driver entry points */
    int  (*send)(struct netif *nif, const u8 *frame, u16 len);  /* 0 = queued/sent */
    void (*poll)(struct netif *nif);                            /* drain RX ring, update link_up */
    void *priv;

    /* layer-3 configuration (owned by hespernetd) */
    bool  configured;
    u32   ip, mask, gw, dns;

    /* statistics */
    u32 rx_packets, rx_bytes, rx_errors, rx_dropped;
    u32 tx_packets, tx_bytes, tx_errors;
} netif_t;

netif_t *netif_register(const char *driver, const char *desc, const u8 mac[6],
                        int (*send)(netif_t *, const u8 *, u16),
                        void (*poll)(netif_t *));
int      netif_count(void);
netif_t *netif_at(int i);
netif_t *netif_by_name(const char *name);

/* Called by drivers for each received frame (frame is only valid during the call). */
void netif_rx(netif_t *nif, const u8 *frame, u16 len);

/* Polls every NIC. Safe to call from anywhere except an RX handler. */
void net_poll_all(void);

/* shared tx scratch buffer. layers build their frame at net_txbuf()+ETH_HLEN then call eth_send().
 * driver send() routines copy the frame out synchronously so one buffer is enough */
u8  *net_txbuf(void);
int  eth_send(netif_t *nif, const u8 dst[6], u16 ethertype, u16 payload_len);

extern const u8 ETH_BROADCAST[6];

#endif
