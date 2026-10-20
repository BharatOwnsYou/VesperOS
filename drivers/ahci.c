#include "ahci.h"
#include "pci.h"
#include "diskdetect.h"
#include "../kernel/blockdev.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"
#include "../kernel/athenalibc.h"
#include "../net/netutil.h"
#include "../include/dma.h"
#include "../include/mmio.h"

/* ---- register map (AHCI 1.3.1) ---- */
#define H_CAP    0x00
#define H_GHC    0x04
#define H_PI     0x0C
#define H_CAP2   0x24
#define H_BOHC   0x28
#define GHC_AE   (1u << 31)
#define CAP_S64A (1u << 31)

#define PORT_BASE(n) (0x100u + (n) * 0x80u)
#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_IE    0x14
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SERR  0x30
#define P_CI    0x38

#define PCMD_ST   (1u << 0)
#define PCMD_SUD  (1u << 1)
#define PCMD_FRE  (1u << 4)
#define PCMD_FR   (1u << 14)
#define PCMD_CR   (1u << 15)
#define TFD_ERR   (1u << 0)
#define TFD_DRQ   (1u << 3)
#define TFD_BSY   (1u << 7)
#define PIS_TFES  (1u << 30)

#define SIG_ATA   0x00000101u

#define ATA_IDENTIFY       0xEC
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA

#define AHCI_MAX_PORTS 8
#define MAX_SECT       128          /* sectors per command, 64K, fits in one PRD entry */
#define SECTOR         512

/* per-port DMA memory. alignment rules: command list 1K, received FIS 256, command table 128.
 * with the struct 1K aligned and this layout all three hold (cl@0, fis@1024, ct@1280) */
typedef struct {
    u8 cl[1024];      /* 32 command headers x 32 bytes, only slot 0 is ever used */
    u8 fis[256];
    u8 ct[256];       /* CFIS 0x00-0x3F, ACMD 0x40-0x4F, reserved, one PRD entry at 0x80 */
} __attribute__((aligned(1024))) port_mem_t;

typedef struct {
    volatile u8 *hba;
    volatile u8 *pr;
    u32 port;
    int s64;
    port_mem_t *mem;
} drive_t;

static port_mem_t pmem[AHCI_MAX_PORTS];
static drive_t    drives[AHCI_MAX_PORTS];
static int        ndrives = 0;
static u16        ident[256] __attribute__((aligned(2)));

static inline u32  ahci_rd(volatile u8 *b, u32 off)         { return *(volatile u32 *)(b + off); }
static inline void ahci_wr(volatile u8 *b, u32 off, u32 v)  { *(volatile u32 *)(b + off) = v; }
static inline void cbarrier(void) { __asm__ volatile ("" ::: "memory"); }

static void logf1(const char *fmt, u32 a, u32 b) {
    char s[96];
    k_snprintf(s, sizeof s, fmt, a, b);
    dbg_puts(s);
}

/* poll until (reg & mask) == 0, false on timeout */
static int wait_clear(volatile u8 *b, u32 off, u32 mask, u32 ms) {
    u32 t0 = net_time_ms();
    while (ahci_rd(b, off) & mask)
        if ((u32)(net_time_ms() - t0) > ms) return -1;
    return 0;
}

static int port_stop(drive_t *d) {
    ahci_wr(d->pr, P_CMD, ahci_rd(d->pr, P_CMD) & ~PCMD_ST);
    if (wait_clear(d->pr, P_CMD, PCMD_CR, 500)) return -1;
    ahci_wr(d->pr, P_CMD, ahci_rd(d->pr, P_CMD) & ~PCMD_FRE);
    if (wait_clear(d->pr, P_CMD, PCMD_FR, 500)) return -1;
    return 0;
}

static int port_start(drive_t *d) {
    if (wait_clear(d->pr, P_CMD, PCMD_CR, 500)) return -1;
    if (wait_clear(d->pr, P_TFD, TFD_BSY | TFD_DRQ, 1000)) return -1;
    ahci_wr(d->pr, P_CMD, ahci_rd(d->pr, P_CMD) | PCMD_FRE);
    ahci_wr(d->pr, P_CMD, ahci_rd(d->pr, P_CMD) | PCMD_ST);
    return 0;
}

/* run one command in slot 0 and wait for it. bytes == 0 means no data phase */
static int port_cmd(drive_t *d, u8 cmd, u64 lba, u32 count, void *buf, u32 bytes, int write, u32 timeout_ms) {
    port_mem_t *m = d->mem;
    u8 *ct = m->ct;

    if (bytes && ((uptr)buf & 1)) return -1;               /* PRD base has to be word aligned */
    if (bytes && !d->s64 && dma_hi(buf)) return -1;        /* 32-bit-only HBA can't reach it */

    k_memset(ct, 0, 0x80 + 16);
    ct[0]  = 0x27;                       /* register host-to-device FIS */
    ct[1]  = 0x80;                       /* C bit: this carries a command */
    ct[2]  = cmd;
    ct[4]  = (u8)lba;        ct[5]  = (u8)(lba >> 8);   ct[6]  = (u8)(lba >> 16);
    ct[7]  = (cmd == ATA_IDENTIFY) ? 0 : 0x40;           /* device reg: LBA mode */
    ct[8]  = (u8)(lba >> 24); ct[9]  = (u8)(lba >> 32); ct[10] = (u8)(lba >> 40);
    ct[12] = (u8)count;      ct[13] = (u8)(count >> 8);

    u32 prdtl = 0;
    if (bytes) {
        u32 *prd = (u32 *)(ct + 0x80);
        prd[0] = dma_lo(buf);
        prd[1] = dma_hi(buf);
        prd[2] = 0;
        prd[3] = bytes - 1;              /* byte count minus one, no interrupt bit */
        prdtl = 1;
    }

    u32 *h = (u32 *)m->cl;
    h[0] = 5u | (write ? (1u << 6) : 0u) | (prdtl << 16);   /* CFL = 5 dwords, W, PRDTL */
    h[1] = 0;                                                /* PRDBC, hw fills it in */
    h[2] = dma_lo(ct);
    h[3] = dma_hi(ct);
    h[4] = h[5] = h[6] = h[7] = 0;

    if (wait_clear(d->pr, P_TFD, TFD_BSY | TFD_DRQ, 1000)) return -1;

    ahci_wr(d->pr, P_IS, 0xFFFFFFFFu);
    cbarrier();
    ahci_wr(d->pr, P_CI, 1);

    u32 t0 = net_time_ms();
    int err = 0;
    for (;;) {
        if (ahci_rd(d->pr, P_IS) & PIS_TFES) { err = 1; break; }
        if (!(ahci_rd(d->pr, P_CI) & 1)) break;
        if ((u32)(net_time_ms() - t0) > timeout_ms) { err = 2; break; }
    }
    if (!err && (ahci_rd(d->pr, P_TFD) & TFD_ERR)) err = 1;

    if (err) {
        logf1("[ahci] port %u: command failed (%u)\n", d->port, (u32)err);
        /* restart the port so a stuck slot doesn't wedge everything after it */
        port_stop(d);
        ahci_wr(d->pr, P_SERR, 0xFFFFFFFFu);
        ahci_wr(d->pr, P_IS, 0xFFFFFFFFu);
        port_start(d);
        return -1;
    }
    dma_rmb();
    return 0;
}

static int ahci_read(blkdev_t *bd, u64 lba, u32 count, void *buf) {
    drive_t *d = (drive_t *)bd->priv;
    u8 *p = (u8 *)buf;
    while (count) {
        u32 n = count > MAX_SECT ? MAX_SECT : count;
        if (port_cmd(d, ATA_READ_DMA_EXT, lba, n, p, n * SECTOR, 0, 5000)) return BLK_EIO;
        lba += n; count -= n; p += n * SECTOR;
    }
    return BLK_OK;
}

static int ahci_write(blkdev_t *bd, u64 lba, u32 count, const void *buf) {
    drive_t *d = (drive_t *)bd->priv;
    const u8 *p = (const u8 *)buf;
    while (count) {
        u32 n = count > MAX_SECT ? MAX_SECT : count;
        if (port_cmd(d, ATA_WRITE_DMA_EXT, lba, n, (void *)p, n * SECTOR, 1, 5000)) return BLK_EIO;
        lba += n; count -= n; p += n * SECTOR;
    }
    /* push the drive's write cache to the platter before we report success */
    if (port_cmd(d, ATA_FLUSH_EXT, 0, 0, NULL, 0, 0, 15000)) return BLK_EIO;
    return BLK_OK;
}

/* IDENTIFY strings are 16-bit words with the characters swapped, space padded */
static void ata_string(const u16 *w, int first, int nwords, char *out) {
    int n = nwords * 2;
    for (int i = 0; i < nwords; i++) {
        out[2 * i]     = (char)(w[first + i] >> 8);
        out[2 * i + 1] = (char)(w[first + i] & 0xFF);
    }
    out[n] = 0;
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0)) out[--n] = 0;
    for (int i = 0; i < n; i++) if (out[i] < 0x20 || out[i] > 0x7E) out[i] = '?';
}

static int port_init(drive_t *d) {
    if (port_stop(d)) return -1;
    k_memset(d->mem, 0, sizeof *d->mem);

    if (!d->s64 && (dma_hi(d->mem->cl) || dma_hi(d->mem->fis) || dma_hi(d->mem->ct))) return -1;
    ahci_wr(d->pr, P_CLB,  dma_lo(d->mem->cl));
    ahci_wr(d->pr, P_CLBU, dma_hi(d->mem->cl));
    ahci_wr(d->pr, P_FB,   dma_lo(d->mem->fis));
    ahci_wr(d->pr, P_FBU,  dma_hi(d->mem->fis));
    ahci_wr(d->pr, P_SERR, 0xFFFFFFFFu);
    ahci_wr(d->pr, P_IS,   0xFFFFFFFFu);
    ahci_wr(d->pr, P_IE,   0);              /* polled, no interrupts */
    return port_start(d);
}

static int probe_port(volatile u8 *hba, u32 n, int s64) {
    if (ndrives >= AHCI_MAX_PORTS) return 0;
    drive_t *d = &drives[ndrives];
    d->hba = hba;
    d->pr = hba + PORT_BASE(n);
    d->port = n;
    d->s64 = s64;
    d->mem = &pmem[ndrives];

    /* make sure the device is powered/spun up, then give the PHY a moment if we had to */
    u32 cmd = ahci_rd(d->pr, P_CMD);
    if (!(cmd & PCMD_SUD)) {
        ahci_wr(d->pr, P_CMD, cmd | PCMD_SUD);
        net_delay_ms(100);
    }

    u32 ssts = ahci_rd(d->pr, P_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) return 0;     /* nothing there / link down */

    u32 sig = ahci_rd(d->pr, P_SIG);
    if (sig != SIG_ATA) {
        logf1("[ahci] port %u: not a plain SATA disk (sig %x), skipped\n", n, sig);
        return 0;
    }
    if (port_init(d)) {
        logf1("[ahci] port %u: could not start the port\n", n, 0);
        return 0;
    }

    k_memset(ident, 0, sizeof ident);
    if (port_cmd(d, ATA_IDENTIFY, 0, 0, ident, sizeof ident, 0, 3000)) {
        logf1("[ahci] port %u: IDENTIFY failed\n", n, 0);
        return 0;
    }
    if (!(ident[83] & (1u << 10))) {
        logf1("[ahci] port %u: no LBA48, skipped\n", n, 0);
        return 0;
    }
    u32 lsz = 512;
    if ((ident[106] & 0xC000u) == 0x4000u && (ident[106] & (1u << 12)))
        lsz = ((u32)ident[117] | ((u32)ident[118] << 16)) * 2;
    if (lsz != SECTOR) {
        logf1("[ahci] port %u: %u-byte logical sectors not supported, skipped\n", n, lsz);
        return 0;
    }

    blkdev_t bd;
    k_memset(&bd, 0, sizeof bd);
    k_snprintf(bd.name, sizeof bd.name, "sata%d", ndrives);
    ata_string(ident, 27, 20, bd.model);
    bd.sectors = (u64)ident[100] | ((u64)ident[101] << 16) | ((u64)ident[102] << 32) | ((u64)ident[103] << 48);
    bd.sector_size = SECTOR;
    bd.read = ahci_read;
    bd.write = ahci_write;
    bd.priv = d;
    if (bd.sectors == 0 || blk_register(&bd) < 0) return 0;

    char s[128];
    k_snprintf(s, sizeof s, "[ahci] %s: port %u, %u MiB, %s\n", bd.name, n, (u32)(bd.sectors >> 11), bd.model);
    dbg_puts(s);
    ndrives++;
    return 1;
}

static int init_hba(const disk_ctrl_t *c) {
    /* memory space + bus master on */
    u16 pcmd = pci_config_read16(c->bus, c->slot, c->func, 0x04);
    pci_config_write16(c->bus, c->slot, c->func, 0x04, (u16)(pcmd | 0x0006));

    u32 lo = pci_config_read32(c->bus, c->slot, c->func, 0x24);      /* BAR5 = ABAR */
    u64 base = mmio_bar_base(lo, 0, NULL);
    if (!base) { dbg_puts("[ahci] no usable ABAR, skipped\n"); return 0; }
    volatile u8 *hba = (volatile u8 *)mmio_map(base, 0x1100);
    if (!hba) { dbg_puts("[ahci] ABAR not reachable (above 4G on i686?), skipped\n"); return 0; }

    ahci_wr(hba, H_GHC, ahci_rd(hba, H_GHC) | GHC_AE);

    /* BIOS/OS handoff: if the firmware still owns the HBA, ask for it and wait a bit */
    if (ahci_rd(hba, H_CAP2) & 1u) {
        u32 bohc = ahci_rd(hba, H_BOHC);
        if (bohc & 1u) {
            ahci_wr(hba, H_BOHC, bohc | 2u);
            u32 t0 = net_time_ms();
            while ((ahci_rd(hba, H_BOHC) & 1u) && (u32)(net_time_ms() - t0) < 2000) { }
        }
    }

    u32 cap = ahci_rd(hba, H_CAP);
    u32 pi = ahci_rd(hba, H_PI);
    int found = 0;
    for (u32 n = 0; n < 32; n++)
        if (pi & (1u << n)) found += probe_port(hba, n, (cap & CAP_S64A) != 0);
    return found;
}

int ahci_init(void) {
    int total = 0;
    int n = disk_detect_count();
    for (int i = 0; i < n; i++) {
        const disk_ctrl_t *c = disk_detect_at(i);
        if (c && c->kind == DISK_CTRL_AHCI) total += init_hba(c);
    }
    return total;
}
