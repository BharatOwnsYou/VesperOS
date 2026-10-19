#include "rtl8139.h"
#include "netdrv.h"
#include "../kernel/string.h"
#include "../include/dma.h"

#define R_IDR0   0x00
#define R_TSD0   0x10
#define R_TSAD0  0x20
#define R_RBSTART 0x30
#define R_CR     0x37
#define R_CAPR   0x38
#define R_IMR    0x3C
#define R_ISR    0x3E
#define R_TCR    0x40
#define R_RCR    0x44
#define R_CONFIG1 0x52
#define R_BMSR   0x64

#define CR_BUFE  0x01
#define CR_TE    0x04
#define CR_RE    0x08
#define CR_RST   0x10
#define TSD_OWN  (1u << 13)

#define RX_LEN   8192
#define RX_BUF_BYTES (RX_LEN + 16 + 1536)   /* WRAP mode: a frame may run past the end */
#define TX_SLOTS 4
#define TX_BUF   1792

static u8 rx_buf[RX_BUF_BYTES] __attribute__((aligned(16)));
static u8 tx_buf[TX_SLOTS][TX_BUF] __attribute__((aligned(16)));
static u16 io;
static u16 rx_off;
static u8  tx_slot;
static bool tx_used[TX_SLOTS];
static bool bound = false;
static u32 last_link_check;

static void rx_enable(void) {
    outb(io + R_CR, CR_RE | CR_TE);
    outl(io + R_RBSTART, dma_lo(rx_buf));
    outl(io + R_RCR, 0xE78E);          /* accept broadcast+multicast+our MAC, WRAP, no FIFO threshold, unlimited DMA burst */
    outl(io + R_TCR, 0x03000700);      /* standard IFG, unlimited DMA burst */
    rx_off = 0;
    outw(io + R_CAPR, (u16)(0 - 16));
    outw(io + R_ISR, 0xFFFF);
}

static int r8139_send(netif_t *nif, const u8 *frame, u16 len) {
    (void)nif;
    if (len > ETH_FRAME_MAX) return -1;
    u8 s = tx_slot;
    if (tx_used[s]) {                                   /* wait for the previous transmission from this slot */
        for (int i = 0; i < 2000 && !(inl(io + R_TSD0 + 4 * s) & TSD_OWN); i++) net_udelay(5);
        if (!(inl(io + R_TSD0 + 4 * s) & TSD_OWN)) return -1;
    }
    k_memcpy(tx_buf[s], frame, len);
    net_mb();
    outl(io + R_TSAD0 + 4 * s, dma_lo(tx_buf[s]));
    outl(io + R_TSD0 + 4 * s, len);                     /* size in bits 0..12, OWN clear => DMA starts */
    tx_used[s] = true;
    tx_slot = (u8)((s + 1) % TX_SLOTS);
    return 0;
}

static void r8139_poll(netif_t *nif) {
    int guard = 64;
    while (!(inb(io + R_CR) & CR_BUFE) && guard--) {
        u32 hdr = (u32)rx_buf[rx_off] | ((u32)rx_buf[rx_off + 1] << 8)
                | ((u32)rx_buf[rx_off + 2] << 16) | ((u32)rx_buf[rx_off + 3] << 24);
        u16 status = (u16)hdr, plen = (u16)(hdr >> 16);   /* plen includes the 4-byte CRC */
        if (!(status & 1) || plen < 18 || plen > 1536 + 4) {   /* ring is out of sync: restart receiver */
            nif->rx_errors++;
            outb(io + R_CR, CR_TE);
            rx_enable();
            break;
        }
        netif_rx(nif, rx_buf + rx_off + 4, (u16)(plen - 4));
        rx_off = (u16)((rx_off + plen + 4 + 3) & ~3u);
        if (rx_off >= RX_LEN) rx_off = (u16)(rx_off - RX_LEN);
        outw(io + R_CAPR, (u16)(rx_off - 16));
        outw(io + R_ISR, 0x0005);                         /* ack ROK/TOK */
    }
    u32 now = net_time_ms();
    if ((u32)(now - last_link_check) >= 250) {
        last_link_check = now;
        nif->link_up = (inw(io + R_BMSR) & 0x0004) != 0;
    }
}

int rtl8139_probe(const pci_device_t *d) {
    if (d->vendor_id != 0x10EC || d->device_id != 0x8139) return 0;
    if (bound) { netdrv_fail("second RTL8139 (driver supports one)"); return -1; }
    u32 bar0 = netdrv_bar(d, 0);
    if (!(bar0 & 1)) { netdrv_fail("BAR0 is not I/O space"); return -1; }
    io = (u16)(bar0 & ~3u);
    if (!io) { netdrv_fail("I/O BAR not assigned"); return -1; }
    netdrv_pci_enable(d, 1, 0);

    /* The RTL8139's DMA engine only has 32-bit address registers: every buffer must sit below 4 GiB. */
    u32 chk;
    if (dma_lo32_checked(rx_buf, &chk) || dma_lo32_checked(tx_buf[TX_SLOTS - 1], &chk)) {
        netdrv_fail("DMA buffers above 4 GiB (RTL8139 is 32-bit only)");
        return -1;
    }

    outb(io + R_CONFIG1, 0x00);                          /* power on */
    outb(io + R_CR, CR_RST);
    for (int i = 0; i < 100 && (inb(io + R_CR) & CR_RST); i++) net_delay_ms(1);
    if (inb(io + R_CR) & CR_RST) { netdrv_fail("reset did not complete"); return -1; }
    outw(io + R_IMR, 0x0000);                            /* polled: no interrupts */
    rx_enable();

    u8 mac[6];
    for (int i = 0; i < 6; i++) mac[i] = inb((u16)(io + R_IDR0 + i));

    netif_t *nif = netif_register("rtl8139", "Realtek RTL8139 Fast Ethernet", mac, r8139_send, r8139_poll);
    if (!nif) { netdrv_fail("too many network interfaces"); return -1; }
    netdrv_attach_pci(nif, d);
    nif->link_up = (inw(io + R_BMSR) & 0x0004) != 0;
    last_link_check = net_time_ms();
    bound = true;
    return 1;
}
