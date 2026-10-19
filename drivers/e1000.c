#include "e1000.h"
#include "netdrv.h"
#include "../kernel/string.h"
#include "../include/dma.h"
#include "../include/mmio.h"

/* ---- register map (8254x software developer's manual, ch. 13) ---- */
#define R_CTRL    0x0000
#define R_STATUS  0x0008
#define R_EERD    0x0014
#define R_ICR     0x00C0
#define R_IMC     0x00D8
#define R_RCTL    0x0100
#define R_TCTL    0x0400
#define R_TIPG    0x0410
#define R_RDBAL   0x2800
#define R_RDBAH   0x2804
#define R_RDLEN   0x2808
#define R_RDH     0x2810
#define R_RDT     0x2818
#define R_RXDCTL  0x2828
#define R_TDBAL   0x3800
#define R_TDBAH   0x3804
#define R_TDLEN   0x3808
#define R_TDH     0x3810
#define R_TDT     0x3818
#define R_TXDCTL  0x3828
#define R_MTA     0x5200
#define R_RAL0    0x5400
#define R_RAH0    0x5404

#define CTRL_SLU   (1u << 5)
#define CTRL_ASDE  (1u << 6)
#define CTRL_RST   (1u << 26)
#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define STATUS_LU  (1u << 1)

#define RX_DESCS 32
#define TX_DESCS 16
#define BUF_SIZE 2048

typedef struct { u32 addr_lo, addr_hi; u16 length; u16 csum; u8 status; u8 errors; u16 special; } rx_desc_t;
typedef struct { u32 addr_lo, addr_hi; u16 length; u8 cso; u8 cmd; u8 status; u8 css; u16 special; } tx_desc_t;

/* DMA memory: identity-mapped (no paging), so virtual == physical. */
static volatile rx_desc_t rx_ring[RX_DESCS] __attribute__((aligned(128)));
static volatile tx_desc_t tx_ring[TX_DESCS] __attribute__((aligned(128)));
static u8 rx_bufs[RX_DESCS][BUF_SIZE] __attribute__((aligned(16)));
static u8 tx_bufs[TX_DESCS][BUF_SIZE] __attribute__((aligned(16)));

static volatile u8 *mmio;
static bool bound = false, is_82574 = false;
static u16 rx_cur, tx_tail;
static u32 last_link_check;

static inline u32  rd(u32 off)          { return *(volatile u32 *)(mmio + off); }
static inline void wr(u32 off, u32 v)   { *(volatile u32 *)(mmio + off) = v; }

static const struct { u16 id; const char *name; } MODELS[] = {
    { 0x100E, "Intel 82540EM Gigabit Ethernet (PRO/1000 MT)" },
    { 0x100F, "Intel 82545EM Gigabit Ethernet" },
    { 0x1004, "Intel 82543GC Gigabit Ethernet" },
    { 0x1000, "Intel 82542 Gigabit Ethernet" },
    { 0x1001, "Intel 82543GC Gigabit Ethernet (fiber)" },
    { 0x1008, "Intel 82544EI Gigabit Ethernet" },
    { 0x1009, "Intel 82544EI Gigabit Ethernet (fiber)" },
    { 0x100C, "Intel 82544GC Gigabit Ethernet" },
    { 0x100D, "Intel 82540EM-A Gigabit Ethernet" },
    { 0x1010, "Intel 82546EB Gigabit Ethernet" },
    { 0x1011, "Intel 82545EM Gigabit Ethernet (fiber)" },
    { 0x1012, "Intel 82546EB Gigabit Ethernet (fiber)" },
    { 0x1013, "Intel 82541EI Gigabit Ethernet" },
    { 0x1015, "Intel 82540EM Gigabit Ethernet (LOM)" },
    { 0x1016, "Intel 82540EP Gigabit Ethernet" },
    { 0x1017, "Intel 82540EP Gigabit Ethernet" },
    { 0x1018, "Intel 82541EI Gigabit Ethernet" },
    { 0x1019, "Intel 82547EI Gigabit Ethernet" },
    { 0x101E, "Intel 82540EP Gigabit Ethernet" },
    { 0x1026, "Intel 82545GM Gigabit Ethernet" },
    { 0x1027, "Intel 82545GM Gigabit Ethernet" },
    { 0x1028, "Intel 82545GM Gigabit Ethernet" },
    { 0x1075, "Intel 82547GI Gigabit Ethernet" },
    { 0x1076, "Intel 82541GI Gigabit Ethernet" },
    { 0x1077, "Intel 82541GI Gigabit Ethernet" },
    { 0x1078, "Intel 82541ER Gigabit Ethernet" },
    { 0x1079, "Intel 82546GB Gigabit Ethernet" },
    { 0x107A, "Intel 82546GB Gigabit Ethernet" },
    { 0x107B, "Intel 82546GB Gigabit Ethernet" },
    { 0x10D3, "Intel 82574L Gigabit Ethernet (experimental)" },
};

static const char *model_name(u16 id) {
    for (u32 i = 0; i < sizeof(MODELS) / sizeof(MODELS[0]); i++)
        if (MODELS[i].id == id) return MODELS[i].name;
    return NULL;
}

static u16 eeprom_read(u8 addr) {
    if (is_82574) wr(R_EERD, 1u | ((u32)addr << 2));
    else          wr(R_EERD, 1u | ((u32)addr << 8));
    for (int i = 0; i < 2000; i++) {
        u32 v = rd(R_EERD);
        if (v & (is_82574 ? (1u << 1) : (1u << 4))) return (u16)(v >> 16);
        net_udelay(10);
    }
    return 0;
}

static int e1000_send(netif_t *nif, const u8 *frame, u16 len) {
    (void)nif;
    if (len > ETH_FRAME_MAX) return -1;
    volatile tx_desc_t *d = &tx_ring[tx_tail];
    if (!(d->status & 1)) {                       /* previous use of this slot not finished (DD clear) */
        for (int i = 0; i < 1000 && !(d->status & 1); i++) net_udelay(5);
        if (!(d->status & 1)) return -1;
    }
    k_memcpy(tx_bufs[tx_tail], frame, len);
    d->addr_lo = dma_lo(tx_bufs[tx_tail]);
    d->addr_hi = dma_hi(tx_bufs[tx_tail]);
    d->length = len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->status = 0;
    d->cmd = 0x0B;                                /* EOP | IFCS | RS */
    tx_tail = (u16)((tx_tail + 1) % TX_DESCS);
    net_mb();
    wr(R_TDT, tx_tail);
    return 0;
}

static void e1000_poll(netif_t *nif) {
    while (rx_ring[rx_cur].status & 1) {                        /* DD */
        volatile rx_desc_t *d = &rx_ring[rx_cur];
        u16 len = d->length;
        if ((d->status & 2) && d->errors == 0 && len >= ETH_HLEN && len <= BUF_SIZE)  /* EOP, no errors */
            netif_rx(nif, rx_bufs[rx_cur], len);
        else
            nif->rx_errors++;
        d->status = 0;
        net_mb();
        wr(R_RDT, rx_cur);                                      /* hand the descriptor back */
        rx_cur = (u16)((rx_cur + 1) % RX_DESCS);
    }
    u32 now = net_time_ms();
    if ((u32)(now - last_link_check) >= 250) {
        last_link_check = now;
        nif->link_up = (rd(R_STATUS) & STATUS_LU) != 0;
    }
}

int e1000_probe(const pci_device_t *d) {
    if (d->vendor_id != 0x8086) return 0;
    const char *name = model_name(d->device_id);
    if (!name) return 0;
    if (bound) { netdrv_fail("second e1000 NIC (driver supports one)"); return -1; }

    u32 bar0 = netdrv_bar(d, 0);
    if (bar0 & 1) { netdrv_fail("BAR0 is I/O space, expected MMIO"); return -1; }
    int wide;
    u64 base = mmio_bar_base(bar0, netdrv_bar(d, 1), &wide);
    if (!base) { netdrv_fail("BAR0 not assigned by firmware"); return -1; }

    is_82574 = (d->device_id == 0x10D3);
    netdrv_pci_enable(d, 0, 1);
    mmio = (volatile u8 *)mmio_map(base, 0x20000);          /* 128 KiB register window */
    if (!mmio) { netdrv_fail("MMIO BAR is above 4 GiB and cannot be mapped"); return -1; }

    /* quiesce + reset */
    wr(R_IMC, 0xFFFFFFFFu);
    wr(R_RCTL, 0);
    wr(R_TCTL, TCTL_PSP);
    (void)rd(R_STATUS);
    net_delay_ms(10);
    wr(R_CTRL, rd(R_CTRL) | CTRL_RST);
    net_delay_ms(20);
    for (int i = 0; i < 100 && (rd(R_CTRL) & CTRL_RST); i++) net_delay_ms(1);
    if (rd(R_CTRL) & CTRL_RST) { netdrv_fail("reset did not complete"); return -1; }
    wr(R_IMC, 0xFFFFFFFFu);
    (void)rd(R_ICR);

    /* link: set-link-up + auto speed detect; clear link-reset, PHY-reset, VLAN-mode, invert-loss-of-signal */
    u32 ctrl = rd(R_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~((1u << 3) | (1u << 7) | (1u << 30) | (1u << 31));
    wr(R_CTRL, ctrl);

    /* MAC address: receive-address register 0 (loaded from EEPROM by hardware), EEPROM as fallback */
    u32 ral = rd(R_RAL0), rah = rd(R_RAH0);
    u8 mac[6] = { (u8)ral, (u8)(ral >> 8), (u8)(ral >> 16), (u8)(ral >> 24), (u8)rah, (u8)(rah >> 8) };
    bool zero = true, ones = true;
    for (int i = 0; i < 6; i++) { if (mac[i]) zero = false; if (mac[i] != 0xFF) ones = false; }
    if (!(rah & (1u << 31)) || zero || ones) {
        for (int w = 0; w < 3; w++) {
            u16 v = eeprom_read((u8)w);
            mac[w * 2] = (u8)v; mac[w * 2 + 1] = (u8)(v >> 8);
        }
        zero = true;
        for (int i = 0; i < 6; i++) if (mac[i]) zero = false;
        if (zero) { netdrv_fail("could not read a MAC address"); return -1; }
    }
    wr(R_RAL0, (u32)mac[0] | ((u32)mac[1] << 8) | ((u32)mac[2] << 16) | ((u32)mac[3] << 24));
    wr(R_RAH0, (u32)mac[4] | ((u32)mac[5] << 8) | (1u << 31));
    for (int i = 0; i < 128; i++) wr(R_MTA + (u32)i * 4, 0);

    /* RX ring */
    for (int i = 0; i < RX_DESCS; i++) {
        rx_ring[i].addr_lo = dma_lo(rx_bufs[i]);
        rx_ring[i].addr_hi = dma_hi(rx_bufs[i]);
        rx_ring[i].status = 0;
        rx_ring[i].errors = 0;
    }
    rx_cur = 0;
    wr(R_RDBAL, dma_lo((const void *)rx_ring));
    wr(R_RDBAH, dma_hi((const void *)rx_ring));
    wr(R_RDLEN, RX_DESCS * sizeof(rx_desc_t));
    wr(R_RDH, 0);
    wr(R_RDT, RX_DESCS - 1);
    if (is_82574) wr(R_RXDCTL, rd(R_RXDCTL) | (1u << 25));     /* queue enable */

    /* TX ring: all slots start "done" so they look free */
    for (int i = 0; i < TX_DESCS; i++) { tx_ring[i].status = 1; tx_ring[i].cmd = 0; }
    tx_tail = 0;
    wr(R_TDBAL, dma_lo((const void *)tx_ring));
    wr(R_TDBAH, dma_hi((const void *)tx_ring));
    wr(R_TDLEN, TX_DESCS * sizeof(tx_desc_t));
    wr(R_TDH, 0);
    wr(R_TDT, 0);
    if (is_82574) wr(R_TXDCTL, rd(R_TXDCTL) | (1u << 25));
    wr(R_TIPG, 0x0060200A);
    wr(R_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));   /* CT=15, COLD=64 (full duplex) */
    net_mb();
    wr(R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);                      /* 2048-byte buffers, strip CRC */

    netif_t *nif = netif_register("e1000", name, mac, e1000_send, e1000_poll);
    if (!nif) { netdrv_fail("too many network interfaces"); return -1; }
    netdrv_attach_pci(nif, d);
    nif->link_up = (rd(R_STATUS) & STATUS_LU) != 0;
    last_link_check = net_time_ms();
    bound = true;
    return 1;
}
