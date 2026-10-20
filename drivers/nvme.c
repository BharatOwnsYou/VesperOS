#include "nvme.h"
#include "pci.h"
#include "diskdetect.h"
#include "../kernel/blockdev.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"
#include "../kernel/athenalibc.h"
#include "../net/netutil.h"
#include "../include/dma.h"
#include "../include/mmio.h"

/* ---- controller registers (NVMe 1.4, ch. 3) ---- */
#define R_CAP    0x00          /* 64-bit, read as two dwords */
#define R_CC     0x14
#define R_CSTS   0x1C
#define R_AQA    0x24
#define R_ASQ    0x28          /* 64-bit */
#define R_ACQ    0x30          /* 64-bit */
#define DB_BASE  0x1000        /* doorbells, stride from CAP.DSTRD */

#define CC_EN     (1u << 0)
#define CSTS_RDY  (1u << 0)
#define CSTS_CFS  (1u << 1)

/* admin opcodes */
#define A_DELETE_SQ 0x00
#define A_CREATE_SQ 0x01
#define A_CREATE_CQ 0x05
#define A_IDENTIFY  0x06
#define A_SET_FEAT  0x09
/* nvm opcodes */
#define IO_FLUSH    0x00
#define IO_WRITE    0x01
#define IO_READ     0x02

#define QD            16              /* entries per queue */
#define PAGE          4096
#define BOUNCE_BYTES  (64 * 1024)     /* data moves through this page-aligned buffer, so caller buffers need no alignment */
#define NSID          1
#define MAX_CTRLS     4

typedef struct {
    volatile u8 *sq;
    volatile u8 *cq;
    u32 sq_tail, cq_head, phase;
    u32 sq_db, cq_db;                 /* register offsets */
    u16 cid;
} queue_t;

typedef struct {
    volatile u8 *bar;
    queue_t admin, io;
    u32 sector_size;
    u32 max_bytes;                    /* largest transfer the controller takes (MDTS), capped at the bounce buffer */
} ctrl_t;

/* DMA memory, queues and PRP pages have to be page aligned. single instance, so only the first
 * controller gets used (see init_one) */
static u8 asq[QD * 64]   __attribute__((aligned(PAGE)));
static u8 acq[QD * 16]   __attribute__((aligned(PAGE)));
static u8 isq[QD * 64]   __attribute__((aligned(PAGE)));
static u8 icq[QD * 16]   __attribute__((aligned(PAGE)));
static u8 idbuf[PAGE]    __attribute__((aligned(PAGE)));
static u64 prp_list[PAGE / 8] __attribute__((aligned(PAGE)));
static u8 bounce[BOUNCE_BYTES] __attribute__((aligned(PAGE)));

static ctrl_t ctrls[MAX_CTRLS];
static int nctrls = 0;

static inline u32  rd(volatile u8 *b, u32 off)        { return *(volatile u32 *)(b + off); }
static inline void wr(volatile u8 *b, u32 off, u32 v) { *(volatile u32 *)(b + off) = v; }

static void logf2(const char *fmt, u32 a, u32 b) {
    char s[110];
    k_snprintf(s, sizeof s, fmt, a, b);
    dbg_puts(s);
}

/* poll CSTS.RDY until it equals `want`, timeout is CAP.TO in 500ms units */
static int wait_rdy(ctrl_t *c, u32 want, u32 to_units) {
    u32 limit = (to_units + 1) * 500;
    u32 t0 = net_time_ms();
    for (;;) {
        u32 st = rd(c->bar, R_CSTS);
        if ((st & CSTS_RDY) == want) return 0;
        if (st & CSTS_CFS) return -1;
        if ((u32)(net_time_ms() - t0) > limit) return -1;
    }
}

/* submit one 64-byte command (16 dwords) and poll its completion. returns 0 on success, -1 on
 * error or timeout. *status gets the 15-bit status field of the completion entry */
static int submit(ctrl_t *c, queue_t *q, const u32 cmd[16], u32 timeout_ms, u32 *status) {
    u16 cid = ++q->cid;
    volatile u32 *slot = (volatile u32 *)(q->sq + q->sq_tail * 64);
    for (int i = 0; i < 16; i++) slot[i] = cmd[i];
    slot[0] = (cmd[0] & 0xFFFFu) | ((u32)cid << 16);

    q->sq_tail = (q->sq_tail + 1) % QD;
    wr(c->bar, q->sq_db, q->sq_tail);                    /* ring the submission doorbell */

    volatile u32 *e = (volatile u32 *)(q->cq + q->cq_head * 16);
    u32 t0 = net_time_ms();
    for (;;) {
        u32 dw3 = e[3];
        if (((dw3 >> 16) & 1u) == q->phase) {            /* phase tag flipped: entry is new */
            dma_rmb();
            u32 st = dw3 >> 17;
            u16 got = (u16)(dw3 & 0xFFFFu);
            q->cq_head++;
            if (q->cq_head == QD) { q->cq_head = 0; q->phase ^= 1; }
            wr(c->bar, q->cq_db, q->cq_head);            /* tell the controller we consumed it */
            if (status) *status = st;
            return (st == 0 && got == cid) ? 0 : -1;
        }
        if (rd(c->bar, R_CSTS) & CSTS_CFS) return -1;
        if ((u32)(net_time_ms() - t0) > timeout_ms) return -1;
    }
}

static void cmd_clear(u32 cmd[16]) { for (int i = 0; i < 16; i++) cmd[i] = 0; }

static int admin(ctrl_t *c, u32 cmd[16]) {
    u32 st = 0;
    int r = submit(c, &c->admin, cmd, 3000, &st);
    if (r) logf2("[nvme] admin command %x failed, status %x\n", cmd[0] & 0xFF, st);
    return r;
}

/* ---- block I/O ---- */

static int xfer(ctrl_t *c, int write, u64 lba, u32 count, void *buf) {
    u32 bytes = count * c->sector_size;
    u32 pages = (bytes + PAGE - 1) / PAGE;
    if (write) k_memcpy(bounce, buf, bytes);

    u32 cmd[16];
    cmd_clear(cmd);
    cmd[0] = write ? IO_WRITE : IO_READ;
    cmd[1] = NSID;
    cmd[6] = dma_lo(bounce);                      /* PRP1 */
    cmd[7] = dma_hi(bounce);
    if (pages == 2) {                             /* PRP2 = second page */
        cmd[8] = dma_lo(bounce + PAGE);
        cmd[9] = dma_hi(bounce + PAGE);
    } else if (pages > 2) {                       /* PRP2 = list of the remaining pages */
        for (u32 i = 1; i < pages; i++) prp_list[i - 1] = dma_addr(bounce + (u64)i * PAGE);
        cmd[8] = dma_lo(prp_list);
        cmd[9] = dma_hi(prp_list);
    }
    cmd[10] = (u32)lba;
    cmd[11] = (u32)(lba >> 32);
    cmd[12] = count - 1;                          /* NLB is zero-based */

    u32 st = 0;
    if (submit(c, &c->io, cmd, 5000, &st)) {
        logf2("[nvme] I/O failed, status %x (lba %u)\n", st, (u32)lba);
        return BLK_EIO;
    }
    if (!write) { dma_rmb(); k_memcpy(buf, bounce, bytes); }
    return BLK_OK;
}

static u32 chunk_sectors(ctrl_t *c) { return c->max_bytes / c->sector_size; }

static int nvme_read(blkdev_t *bd, u64 lba, u32 count, void *buf) {
    ctrl_t *c = (ctrl_t *)bd->priv;
    u8 *p = (u8 *)buf;
    u32 max = chunk_sectors(c);
    while (count) {
        u32 n = count > max ? max : count;
        int r = xfer(c, 0, lba, n, p);
        if (r) return r;
        lba += n; count -= n; p += (u64)n * c->sector_size;
    }
    return BLK_OK;
}

static int nvme_write(blkdev_t *bd, u64 lba, u32 count, const void *buf) {
    ctrl_t *c = (ctrl_t *)bd->priv;
    const u8 *p = (const u8 *)buf;
    u32 max = chunk_sectors(c);
    while (count) {
        u32 n = count > max ? max : count;
        int r = xfer(c, 1, lba, n, (void *)p);
        if (r) return r;
        lba += n; count -= n; p += (u64)n * c->sector_size;
    }
    u32 cmd[16];
    cmd_clear(cmd);
    cmd[0] = IO_FLUSH;
    cmd[1] = NSID;
    u32 st = 0;
    if (submit(c, &c->io, cmd, 15000, &st)) { logf2("[nvme] flush failed, status %x (%u)\n", st, 0); return BLK_EIO; }
    return BLK_OK;
}

/* ---- bring-up ---- */

static void trim_copy(const u8 *src, int n, char *out) {
    int k = 0;
    for (int i = 0; i < n; i++) out[k++] = (src[i] >= 0x20 && src[i] <= 0x7E) ? (char)src[i] : ' ';
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static int init_one(const disk_ctrl_t *dc) {
    if (nctrls >= 1) {
        /* the queue memory above is static and single-instance */
        dbg_puts("[nvme] more than one controller, only the first one is used\n");
        return 0;
    }
    ctrl_t *c = &ctrls[nctrls];

    /* memory space + bus master on, INTx masked (we poll) */
    u16 pcmd = pci_config_read16(dc->bus, dc->slot, dc->func, 0x04);
    pci_config_write16(dc->bus, dc->slot, dc->func, 0x04, (u16)(pcmd | 0x0406));

    u32 lo = pci_config_read32(dc->bus, dc->slot, dc->func, 0x10);
    u32 hi = pci_config_read32(dc->bus, dc->slot, dc->func, 0x14);
    int is64 = 0;
    u64 base = mmio_bar_base(lo, hi, &is64);
    if (!base) { dbg_puts("[nvme] no usable BAR0, skipped\n"); return 0; }
    c->bar = (volatile u8 *)mmio_map(base, 0x2000);
    if (!c->bar) { dbg_puts("[nvme] BAR0 not reachable (above 4G on i686?), skipped\n"); return 0; }

    u32 cap_lo = rd(c->bar, R_CAP);
    u32 cap_hi = rd(c->bar, R_CAP + 4);
    u32 mqes   = cap_lo & 0xFFFFu;
    u32 to     = (cap_lo >> 24) & 0xFFu;
    u32 dstrd  = cap_hi & 0xFu;
    u32 nvm_cs = (cap_hi >> 5) & 1u;            /* CAP.CSS bit 0: NVM command set */
    u32 mpsmin = (cap_hi >> 16) & 0xFu;
    if (mqes + 1 < QD || !nvm_cs || mpsmin != 0) {
        logf2("[nvme] controller not supported (mqes %u, mpsmin %u), skipped\n", mqes, mpsmin);
        return 0;
    }
    u32 stride = 4u << dstrd;

    /* reset: disable and wait for RDY to drop */
    u32 cc = rd(c->bar, R_CC);
    if (cc & CC_EN) wr(c->bar, R_CC, cc & ~CC_EN);
    if (wait_rdy(c, 0, to)) { dbg_puts("[nvme] controller won't reset, skipped\n"); return 0; }

    k_memset(asq, 0, sizeof asq); k_memset(acq, 0, sizeof acq);
    k_memset(isq, 0, sizeof isq); k_memset(icq, 0, sizeof icq);

    c->admin.sq = asq; c->admin.cq = acq;
    c->admin.sq_tail = c->admin.cq_head = 0; c->admin.phase = 1; c->admin.cid = 0;
    c->admin.sq_db = DB_BASE;
    c->admin.cq_db = DB_BASE + stride;
    c->io.sq = isq; c->io.cq = icq;
    c->io.sq_tail = c->io.cq_head = 0; c->io.phase = 1; c->io.cid = 0;
    c->io.sq_db = DB_BASE + 2 * stride;
    c->io.cq_db = DB_BASE + 3 * stride;

    wr(c->bar, R_AQA, ((QD - 1) << 16) | (QD - 1));
    wr(c->bar, R_ASQ,     dma_lo(asq)); wr(c->bar, R_ASQ + 4, dma_hi(asq));
    wr(c->bar, R_ACQ,     dma_lo(acq)); wr(c->bar, R_ACQ + 4, dma_hi(acq));

    /* enable: 64-byte SQ entries (2^6), 16-byte CQ entries (2^4), 4K pages, NVM command set, round robin */
    wr(c->bar, R_CC, CC_EN | (6u << 16) | (4u << 20));
    if (wait_rdy(c, CSTS_RDY, to)) { dbg_puts("[nvme] controller didn't come ready, skipped\n"); return 0; }

    u32 cmd[16];

    /* ask for one I/O submission + one completion queue (values are zero-based) */
    cmd_clear(cmd); cmd[0] = A_SET_FEAT; cmd[10] = 0x07; cmd[11] = 0;
    if (admin(c, cmd)) return 0;

    cmd_clear(cmd); cmd[0] = A_CREATE_CQ;
    cmd[6] = dma_lo(icq); cmd[7] = dma_hi(icq);
    cmd[10] = ((QD - 1) << 16) | 1u;            /* queue size, queue id 1 */
    cmd[11] = 1u;                               /* physically contiguous, interrupts off */
    if (admin(c, cmd)) return 0;

    cmd_clear(cmd); cmd[0] = A_CREATE_SQ;
    cmd[6] = dma_lo(isq); cmd[7] = dma_hi(isq);
    cmd[10] = ((QD - 1) << 16) | 1u;
    cmd[11] = (1u << 16) | 1u;                  /* completes to CQ 1, physically contiguous */
    if (admin(c, cmd)) return 0;

    /* identify controller: model string and max transfer size */
    k_memset(idbuf, 0, sizeof idbuf);
    cmd_clear(cmd); cmd[0] = A_IDENTIFY;
    cmd[6] = dma_lo(idbuf); cmd[7] = dma_hi(idbuf);
    cmd[10] = 1;                                /* CNS 1 = controller */
    if (admin(c, cmd)) return 0;
    char model[41];
    trim_copy(idbuf + 24, 40, model);
    u32 mdts = idbuf[77];                       /* max transfer = 2^mdts pages, 0 = no limit */
    c->max_bytes = BOUNCE_BYTES;
    if (mdts && mdts < 4) c->max_bytes = (u32)PAGE << mdts;     /* below 64K, otherwise the bounce buffer is the limit */

    /* identify namespace 1: size and sector size */
    k_memset(idbuf, 0, sizeof idbuf);
    cmd_clear(cmd); cmd[0] = A_IDENTIFY; cmd[1] = NSID;
    cmd[6] = dma_lo(idbuf); cmd[7] = dma_hi(idbuf);
    cmd[10] = 0;                                /* CNS 0 = namespace */
    if (admin(c, cmd)) return 0;
    u64 nsze = (u64)(*(u32 *)idbuf) | ((u64)(*(u32 *)(idbuf + 4)) << 32);
    u32 fmt = idbuf[26] & 0xFu;                 /* FLBAS: index of the LBA format in use */
    u32 lbads = idbuf[128 + fmt * 4 + 2];       /* log2 of the data size */
    if (nsze == 0) { dbg_puts("[nvme] namespace 1 is empty/inactive, skipped\n"); return 0; }
    if (lbads != 9 && lbads != 12) {
        logf2("[nvme] unsupported sector size 2^%u (format %u), skipped\n", lbads, fmt);
        return 0;
    }
    c->sector_size = 1u << lbads;
    if (c->max_bytes < c->sector_size) return 0;

    blkdev_t bd;
    k_memset(&bd, 0, sizeof bd);
    k_snprintf(bd.name, sizeof bd.name, "nvme%d", nctrls);
    k_strcpy(bd.model, model);
    bd.sectors = nsze;
    bd.sector_size = c->sector_size;
    bd.read = nvme_read;
    bd.write = nvme_write;
    bd.priv = c;
    if (blk_register(&bd) < 0) return 0;

    char s[128];
    u32 mib = (u32)((nsze * c->sector_size) >> 20);
    k_snprintf(s, sizeof s, "[nvme] %s: %u MiB, %u-byte sectors, %s\n", bd.name, mib, c->sector_size, model);
    dbg_puts(s);
    nctrls++;
    return 1;
}

int nvme_init(void) {
    int total = 0;
    int n = disk_detect_count();
    for (int i = 0; i < n; i++) {
        const disk_ctrl_t *dc = disk_detect_at(i);
        if (dc && dc->kind == DISK_CTRL_NVME) total += init_one(dc);
    }
    return total;
}
