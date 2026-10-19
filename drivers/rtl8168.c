/* Realtek RTL8111/8168/8211/8411 driver. regs + descriptors follow linux r8169 (C+ format).
 * uses the I/O port BAR0 if firmware gave one: the chip mirrors its regs there and ports work from this
 * 32-bit non-paged kernel wherever the PCIe BARs ended up. MMIO BAR2 is the fallback */
#include "rtl8168.h"
#include "netdrv.h"
#include "../kernel/string.h"
#include "../include/dma.h"
#include "../include/mmio.h"

#define R_IDR0      0x00
#define R_MAR0      0x08
#define R_TNPDS     0x20      /* normal-priority TX descriptor ring, 64-bit */
#define R_CR        0x37
#define R_TPPOLL    0x38
#define R_IMR       0x3C
#define R_ISR       0x3E
#define R_TCR       0x40
#define R_RCR       0x44
#define R_9346CR    0x50
#define R_PHYAR     0x60
#define R_PHYSTATUS 0x6C
#define R_PMCH      0x6F
#define R_RMS       0xDA
#define R_CPLUSCMD  0xE0
#define R_RDSAR     0xE4      /* RX descriptor ring, 64-bit */
#define R_MTPS      0xEC
#define R_MISC      0xF0      /* 8168 only */

#define MISC_RXDV_GATED_EN (1u << 19)   /* 8168G and newer: blocks receive until cleared */
#define TCR_AUTO_FIFO      (1u << 7)    /* 8168EVL and newer */

#define CR_RST      0x10
#define CR_RE       0x08
#define CR_TE       0x04
#define TPPOLL_NPQ  0x40

#define DESC_OWN    (1u << 31)
#define DESC_EOR    (1u << 30)
#define DESC_FS     (1u << 29)
#define DESC_LS     (1u << 28)
#define RX_RES      (1u << 21)     /* receive error summary */

#define PHYST_LINK  0x02
#define PHYST_1000  0x10
#define PHYST_100   0x08
#define PHYST_10    0x04

#define RX_N     32
#define TX_N     16
#define BUF_SZ   2048

typedef struct { u32 opts1, opts2, addr_lo, addr_hi; } desc_t;

static volatile desc_t rx_ring[RX_N] __attribute__((aligned(256)));
static volatile desc_t tx_ring[TX_N] __attribute__((aligned(256)));
static u8 rx_buf[RX_N][BUF_SZ] __attribute__((aligned(16)));
static u8 tx_buf[TX_N][BUF_SZ] __attribute__((aligned(16)));

static bool use_io;
static u16  io;
static volatile u8 *mm;
static bool bound = false;
static u16  rx_cur, tx_cur;
static u32  last_link_check;
static u32  chip_xid, tx_kicks, cr_fixups;
static u8   cr_readback, cr_after_init;
static bool gen_g_plus, gen_evl_plus;

static inline u8   r8(u32 o)          { return use_io ? inb((u16)(io + o))  : *(volatile u8  *)(mm + o); }
static inline u16  r16(u32 o)         { return use_io ? inw((u16)(io + o))  : *(volatile u16 *)(mm + o); }
static inline u32  r32(u32 o)         { return use_io ? inl((u16)(io + o))  : *(volatile u32 *)(mm + o); }
static inline void w8(u32 o, u8 v)    { if (use_io) outb((u16)(io + o), v);  else *(volatile u8  *)(mm + o) = v; }
static inline void w16(u32 o, u16 v)  { if (use_io) outw((u16)(io + o), v);  else *(volatile u16 *)(mm + o) = v; }
static inline void w32(u32 o, u32 v)  { if (use_io) outl((u16)(io + o), v);  else *(volatile u32 *)(mm + o) = v; }

/* ---- internal PHY via PHYAR (bit 31 = write-in-progress / read-data-valid) ---- */
static int phy_write(u8 reg, u16 val) {
    w32(R_PHYAR, 0x80000000u | ((u32)(reg & 0x1F) << 16) | val);
    for (int i = 0; i < 2000; i++) {
        if (!(r32(R_PHYAR) & 0x80000000u)) return 0;
        net_udelay(20);
    }
    return -1;
}

/* (re)enable tx/rx. on real hw (RTL8168H) ChipCmd read 0 after init, both engines off, so this is redone
 * after init and whenever poll()/send() sees the bits missing. readback is kept for 'hespernetctl nic' */
static void enable_txrx(void) {
    w8(R_CR, (u8)(r8(R_CR) | CR_RE | CR_TE));
    cr_readback = r8(R_CR);
    cr_fixups++;
}

static int r8168_send(netif_t *nif, const u8 *frame, u16 len) {
    (void)nif;
    if (len > ETH_FRAME_MAX) return -1;
    if (!(r8(R_CR) & CR_TE)) enable_txrx();                      /* transmitter must be on or the NIC ignores the ring */
    volatile desc_t *d = &tx_ring[tx_cur];
    if (d->opts1 & DESC_OWN) {                                   /* slot still owned by the NIC */
        for (int i = 0; i < 1000 && (d->opts1 & DESC_OWN); i++) net_udelay(5);
        if (d->opts1 & DESC_OWN) return -1;
    }
    k_memcpy(tx_buf[tx_cur], frame, len);
    d->opts2 = 0;
    d->addr_lo = dma_lo(tx_buf[tx_cur]);
    d->addr_hi = dma_hi(tx_buf[tx_cur]);
    net_mb();                                                    /* descriptor body visible before OWN */
    d->opts1 = DESC_OWN | DESC_FS | DESC_LS | len | (tx_cur == TX_N - 1 ? DESC_EOR : 0);
    net_mb();
    w8(R_TPPOLL, TPPOLL_NPQ);                                    /* kick the normal-priority queue */
    tx_kicks++;
    tx_cur = (u16)((tx_cur + 1) % TX_N);
    return 0;
}

static void r8168_poll(netif_t *nif) {
    int budget = RX_N;
    while (budget--) {
        volatile desc_t *d = &rx_ring[rx_cur];
        u32 o = d->opts1;
        if (o & DESC_OWN) break;                                 /* still the NIC's */
        net_cbarrier();
        u32 len = o & 0x3FFF;                                    /* includes the 4-byte FCS */
        if ((o & (DESC_FS | DESC_LS)) == (DESC_FS | DESC_LS) && !(o & RX_RES)
            && len >= ETH_HLEN + 4 && len - 4 <= BUF_SZ)
            netif_rx(nif, rx_buf[rx_cur], (u16)(len - 4));
        else
            nif->rx_errors++;
        d->opts2 = 0;
        net_mb();
        d->opts1 = DESC_OWN | (rx_cur == RX_N - 1 ? DESC_EOR : 0) | BUF_SZ;   /* recycle */
        rx_cur = (u16)((rx_cur + 1) % RX_N);
    }
    u32 now = net_time_ms();
    if ((u32)(now - last_link_check) >= 250) {
        last_link_check = now;
        nif->link_up = (r8(R_PHYSTATUS) & PHYST_LINK) != 0;
        if ((r8(R_CR) & (CR_RE | CR_TE)) != (CR_RE | CR_TE)) {     /* engines got switched off: turn them back on */
            enable_txrx();
            w8(R_TPPOLL, TPPOLL_NPQ);                              /* and make it look at any queued TX descriptor */
        }
    }
}

int rtl8168_probe(const pci_device_t *d) {
    if (d->vendor_id != 0x10EC || (d->device_id != 0x8168 && d->device_id != 0x8161)) return 0;
    if (bound) { netdrv_fail("second RTL8168 (driver supports one)"); return -1; }

    /* pick a register window: I/O BAR0 preferred, 32-bit MMIO BAR2 as fallback */
    u32 bar0 = netdrv_bar(d, 0), bar2 = netdrv_bar(d, 2);
    int wide;
    u64 mbase = mmio_bar_base(bar2, netdrv_bar(d, 3), &wide);
    if ((bar0 & 1) && (bar0 & ~3u) != 0) {
        use_io = true;
        io = (u16)(bar0 & ~3u);
    } else if (mbase && (mm = (volatile u8 *)mmio_map(mbase, 0x1000)) != NULL) {
        use_io = false;
    } else {
        netdrv_fail("no usable register BAR (I/O unassigned, MMIO unreachable)");
        return -1;
    }
    netdrv_pci_enable(d, 1, 1);

    u32 tcr = r32(R_TCR);
    chip_xid = (tcr >> 20) & 0x7CF;                               /* same decode Linux r8169 uses */
    gen_evl_plus = chip_xid >= 0x2C8;                             /* 8168EVL and everything after */
    gen_g_plus   = chip_xid >= 0x4C0;                             /* 8168G / GU / H / FP ... */
    if (tcr == 0xFFFFFFFFu) { netdrv_fail("registers read as all-ones (bad BAR/power state)"); return -1; }

    /* keep the PLL powered and (re)start internal-PHY autonegotiation */
    w8(R_PMCH, (u8)(r8(R_PMCH) | 0x80));
    w8(R_CR, CR_RST);
    int i;
    for (i = 0; i < 100 && (r8(R_CR) & CR_RST); i++) net_delay_ms(1);
    if (r8(R_CR) & CR_RST) { netdrv_fail("chip reset did not complete"); return -1; }

    if (gen_g_plus) w32(R_MISC, r32(R_MISC) & ~MISC_RXDV_GATED_EN);   /* un-gate the receiver */

    phy_write(0x1F, 0x0000);                  /* PHY register page 0 */
    phy_write(0x04, 0x01E1);                  /* advertise 10/100 half+full */
    phy_write(0x09, 0x0300);                  /* advertise 1000 half+full */
    phy_write(0x00, 0x1200);                  /* BMCR: autoneg enable + restart */

    u8 mac[6];
    for (i = 0; i < 6; i++) mac[i] = r8(R_IDR0 + (u32)i);
    bool zero = true, ones = true;
    for (i = 0; i < 6; i++) { if (mac[i]) zero = false; if (mac[i] != 0xFF) ones = false; }
    if (zero || ones) { netdrv_fail("MAC address registers empty"); return -1; }

    /* descriptor rings */
    for (i = 0; i < RX_N; i++) {
        rx_ring[i].opts2 = 0;
        rx_ring[i].addr_lo = dma_lo(rx_buf[i]);
        rx_ring[i].addr_hi = dma_hi(rx_buf[i]);
        rx_ring[i].opts1 = DESC_OWN | (i == RX_N - 1 ? DESC_EOR : 0) | BUF_SZ;
    }
    for (i = 0; i < TX_N; i++) {
        tx_ring[i].opts1 = (i == TX_N - 1 ? DESC_EOR : 0);
        tx_ring[i].opts2 = 0;
        tx_ring[i].addr_lo = 0;
        tx_ring[i].addr_hi = 0;
    }
    rx_cur = tx_cur = 0;
    net_mb();

    w8(R_9346CR, 0xC0);                                           /* unlock config registers */
    w16(R_CPLUSCMD, (u16)(r16(R_CPLUSCMD) & ~((1u << 6) | (1u << 5))));   /* no VLAN strip, no RX checksum offload */
    w32(R_RDSAR + 4, dma_hi((const void *)rx_ring));  w32(R_RDSAR, dma_lo((const void *)rx_ring));
    w32(R_TNPDS + 4, dma_hi((const void *)tx_ring));  w32(R_TNPDS, dma_lo((const void *)tx_ring));
    w16(R_RMS, 0x0600);                                           /* max RX frame 1536 */
    w8(R_MTPS, 0x3B);                                             /* max TX packet size */
    enable_txrx();                                                /* enable before TCR/RCR (per r8169) */
    w32(R_TCR, 0x03000700 | (gen_evl_plus ? TCR_AUTO_FIFO : 0));  /* IFG normal, unlimited DMA burst */
    if (gen_g_plus) w32(R_MISC, r32(R_MISC) & ~MISC_RXDV_GATED_EN);
    /* AB|AM|APM + unlimited burst; newer chips use 128B-int/multi-queue/early-off bits instead of the FIFO threshold */
    w32(R_RCR, gen_g_plus ? 0x0000CF0E : 0x0000E70E);
    w32(R_MAR0, 0xFFFFFFFFu); w32(R_MAR0 + 4, 0xFFFFFFFFu);       /* accept all multicast hashes */
    w16(R_IMR, 0x0000);                                           /* polled: no interrupts */
    w16(R_ISR, 0xFFFF);
    w8(R_9346CR, 0x00);                                           /* lock config registers */
    enable_txrx();                                                /* again after locking, in case the write only sticks now */
    cr_after_init = r8(R_CR);

    char desc[72];
    k_strcpy(desc, "Realtek RTL8111/8168/8211/8411 PCIe Gigabit Ethernet (rev 0x");
    {
        static const char hex[] = "0123456789abcdef";
        u8 rev = (u8)pci_config_read32(d->bus, d->slot, d->func, 0x08);
        char tail[4] = { hex[rev >> 4], hex[rev & 15], ')', 0 };
        k_strcat(desc, tail);
    }

    netif_t *nif = netif_register("rtl8168", desc, mac, r8168_send, r8168_poll);
    if (!nif) { netdrv_fail("too many network interfaces"); return -1; }
    netdrv_attach_pci(nif, d);
    nif->link_up = (r8(R_PHYSTATUS) & PHYST_LINK) != 0;
    last_link_check = net_time_ms();
    bound = true;
    return 1;
}

int rtl8168_diag(rtl8168_diag_t *o) {
    if (!bound) return 0;
    o->use_io = use_io;
    o->base = use_io ? (u32)io : (u32)(uptr)mm;
    o->xid = chip_xid;
    o->cr = r8(R_CR); o->phystatus = r8(R_PHYSTATUS);
    o->cplus = r16(R_CPLUSCMD); o->isr = r16(R_ISR);
    o->tcr = r32(R_TCR); o->rcr = r32(R_RCR); o->misc = r32(R_MISC);
    o->tx_cur = tx_cur; o->rx_cur = rx_cur;
    o->tx_last_opts1 = tx_ring[(tx_cur + TX_N - 1) % TX_N].opts1;
    o->rx_head_opts1 = rx_ring[rx_cur].opts1;
    o->tx_kicks = tx_kicks;
    o->cr_after_init = cr_after_init; o->cr_readback = cr_readback; o->cr_fixups = cr_fixups;
    return 1;
}
