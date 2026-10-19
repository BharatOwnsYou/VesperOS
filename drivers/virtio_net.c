#include "virtio_net.h"
#include "netdrv.h"
#include "../kernel/string.h"
#include "../include/dma.h"

/* legacy virtio PCI header, offsets from the I/O BAR */
#define VP_HOST_FEATURES   0x00
#define VP_GUEST_FEATURES  0x04
#define VP_QUEUE_PFN       0x08
#define VP_QUEUE_NUM       0x0C
#define VP_QUEUE_SEL       0x0E
#define VP_QUEUE_NOTIFY    0x10
#define VP_STATUS          0x12
#define VP_ISR             0x13
#define VP_CONFIG          0x14     /* virtio-net config: mac[6], status u16 */

#define ST_ACK        0x01
#define ST_DRIVER     0x02
#define ST_DRIVER_OK  0x04
#define ST_FAILED     0x80

#define F_MAC     (1u << 5)
#define F_STATUS  (1u << 16)

#define VQ_MEM     12288            /* enough for a 256-entry split ring */
#define RX_BUFS    32
#define TX_BUFS    8
#define BUF_SIZE   2048
#define NET_HDR    10               /* struct virtio_net_hdr without MRG_RXBUF */

typedef struct { u32 addr_lo, addr_hi, len; u16 flags, next; } vdesc_t;

typedef struct {
    u8  *mem;
    u16  num;
    u16  avail_idx;                 /* next avail slot we will publish */
    u16  last_used;                 /* last used-ring index we consumed */
    u32  avail_off, used_off;
} vq_t;

static u8 rxq_mem[VQ_MEM] __attribute__((aligned(4096)));
static u8 txq_mem[VQ_MEM] __attribute__((aligned(4096)));
static u8 rx_buf[RX_BUFS][BUF_SIZE] __attribute__((aligned(16)));
static u8 tx_buf[TX_BUFS][BUF_SIZE] __attribute__((aligned(16)));

static u16  io;
static vq_t rxq, txq;
static bool bound = false, have_status = false;
static u16  tx_head, tx_inflight;
static u32  last_link_check;

/* ring accessors */
static inline volatile vdesc_t *vdesc(vq_t *q, u16 i) { return (volatile vdesc_t *)(q->mem + 16u * i); }
static inline volatile u16 *vavail(vq_t *q) { return (volatile u16 *)(q->mem + q->avail_off); }   /* [0]=flags [1]=idx [2+]=ring */
static inline volatile u16 *vused(vq_t *q)  { return (volatile u16 *)(q->mem + q->used_off); }    /* [0]=flags [1]=idx */
static inline volatile u32 *vused_ring(vq_t *q) { return (volatile u32 *)(q->mem + q->used_off + 4); } /* {id,len} pairs */

static int vq_setup(vq_t *q, u8 *mem, u16 index) {
    outw(io + VP_QUEUE_SEL, index);
    u16 n = inw(io + VP_QUEUE_NUM);
    if (n == 0 || n > 256) return -1;            /* queue absent, or larger than our static memory */
    q->mem = mem;
    q->num = n;
    q->avail_off = 16u * n;
    q->used_off = (q->avail_off + 6u + 2u * n + 4095u) & ~4095u;
    if (q->used_off + 6u + 8u * n > VQ_MEM) return -1;
    k_memset(mem, 0, VQ_MEM);
    q->avail_idx = 0;
    q->last_used = 0;
    if (dma_addr(mem) >> 44) return -1;                          /* legacy PFN register is 32 bits of 4 KiB pages */
    outl(io + VP_QUEUE_PFN, (u32)(dma_addr(mem) >> 12));
    return 0;
}

static void notify(u16 index) { net_mb(); outw(io + VP_QUEUE_NOTIFY, index); }

static int vnet_send(netif_t *nif, const u8 *frame, u16 len) {
    (void)nif;
    if (len > ETH_FRAME_MAX) return -1;
    volatile u16 *us = vused(&txq);
    while (txq.last_used != us[1]) { txq.last_used++; if (tx_inflight) tx_inflight--; }   /* reclaim */
    if (tx_inflight >= TX_BUFS) return -1;

    u16 slot = (u16)(tx_head % TX_BUFS);
    tx_head++;
    k_memset(tx_buf[slot], 0, NET_HDR);
    k_memcpy(tx_buf[slot] + NET_HDR, frame, len);

    volatile vdesc_t *d = vdesc(&txq, slot);
    d->addr_lo = dma_lo(tx_buf[slot]);
    d->addr_hi = dma_hi(tx_buf[slot]);
    d->len = (u32)NET_HDR + len;
    d->flags = 0;
    d->next = 0;

    volatile u16 *av = vavail(&txq);
    av[2 + (txq.avail_idx % txq.num)] = slot;
    txq.avail_idx++;
    net_mb();
    av[1] = txq.avail_idx;
    tx_inflight++;
    notify(1);
    return 0;
}

static void vnet_poll(netif_t *nif) {
    volatile u16 *us = vused(&rxq);
    volatile u32 *ur = vused_ring(&rxq);
    volatile u16 *av = vavail(&rxq);
    bool any = false;
    while (rxq.last_used != us[1]) {
        u16 e = (u16)(rxq.last_used % rxq.num);
        u32 id = ur[2 * e], len = ur[2 * e + 1];
        if (id < RX_BUFS && len > NET_HDR && len - NET_HDR <= BUF_SIZE - NET_HDR)
            netif_rx(nif, rx_buf[id] + NET_HDR, (u16)(len - NET_HDR));
        else
            nif->rx_errors++;
        if (id < RX_BUFS) {                                      /* give the buffer back */
            av[2 + (rxq.avail_idx % rxq.num)] = (u16)id;
            rxq.avail_idx++;
            net_mb();
            av[1] = rxq.avail_idx;
        }
        rxq.last_used++;
        any = true;
    }
    if (any) notify(0);

    u32 now = net_time_ms();
    if ((u32)(now - last_link_check) >= 250) {
        last_link_check = now;
        nif->link_up = have_status ? (inw(io + VP_CONFIG + 6) & 1) != 0 : true;
    }
}

int virtio_net_probe(const pci_device_t *d) {
    if (d->vendor_id != 0x1AF4 || d->device_id != 0x1000) return 0;
    if (bound) { netdrv_fail("second virtio-net NIC (driver supports one)"); return -1; }

    u32 bar0 = netdrv_bar(d, 0);
    if (!(bar0 & 1)) { netdrv_fail("BAR0 is not I/O space (modern-only virtio?)"); return -1; }
    io = (u16)(bar0 & ~3u);
    if (!io) { netdrv_fail("I/O BAR not assigned"); return -1; }
    netdrv_pci_enable(d, 1, 0);

    outb(io + VP_STATUS, 0);                                     /* reset */
    outb(io + VP_STATUS, ST_ACK);
    outb(io + VP_STATUS, ST_ACK | ST_DRIVER);

    u32 host = inl(io + VP_HOST_FEATURES);
    if (!(host & F_MAC)) { outb(io + VP_STATUS, ST_FAILED); netdrv_fail("device offers no MAC feature"); return -1; }
    u32 guest = F_MAC | (host & F_STATUS);                      /* nothing else: no offloads, no MRG_RXBUF */
    outl(io + VP_GUEST_FEATURES, guest);
    have_status = (guest & F_STATUS) != 0;

    if (vq_setup(&rxq, rxq_mem, 0) != 0 || vq_setup(&txq, txq_mem, 1) != 0) {
        outb(io + VP_STATUS, ST_FAILED);
        netdrv_fail("virtqueue setup failed (size 0 or > 256)");
        return -1;
    }

    /* pre-post receive buffers */
    u16 post = rxq.num < RX_BUFS ? rxq.num : RX_BUFS;
    volatile u16 *av = vavail(&rxq);
    for (u16 i = 0; i < post; i++) {
        volatile vdesc_t *dd = vdesc(&rxq, i);
        dd->addr_lo = dma_lo(rx_buf[i]);
        dd->addr_hi = dma_hi(rx_buf[i]);
        dd->len = BUF_SIZE;
        dd->flags = 2;                                           /* VRING_DESC_F_WRITE: device writes here */
        dd->next = 0;
        av[2 + i] = i;
    }
    rxq.avail_idx = post;
    net_mb();
    av[1] = post;

    outb(io + VP_STATUS, ST_ACK | ST_DRIVER | ST_DRIVER_OK);
    notify(0);

    u8 mac[6];
    for (int i = 0; i < 6; i++) mac[i] = inb((u16)(io + VP_CONFIG + i));

    netif_t *nif = netif_register("virtio-net", "virtio network device (legacy PCI transport)",
                                  mac, vnet_send, vnet_poll);
    if (!nif) { netdrv_fail("too many network interfaces"); return -1; }
    netdrv_attach_pci(nif, d);
    nif->link_up = have_status ? (inw(io + VP_CONFIG + 6) & 1) != 0 : true;
    last_link_check = net_time_ms();
    bound = true;
    return 1;
}
