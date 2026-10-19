/*
 * VesperOS 4 - xHCI host controller driver, see xhci.h for scope
 */
#include "../include/dma.h"
#include "../include/mmio.h"
#include "xhci.h"
#include "pci.h"
#include "hid.h"
#include "../include/io.h"
#include "../kernel/athenalibc.h"

typedef struct __attribute__((packed, aligned(16))) {
    /* volatile: the controller reads/writes these over DMA. without it the compiler can reorder the stores
     * (cycle bit in 'control' has to be written LAST) or cache a load across the polling loops */
    volatile u32 p_lo;
    volatile u32 p_hi;
    volatile u32 status;
    volatile u32 control;
} trb_t;

#define TRB_CYCLE        0x1u
#define TRB_TYPE_SHIFT   10
#define TRB_TYPE(t)      ((u32)(t) << TRB_TYPE_SHIFT)
#define TRB_GET_TYPE(c)  (((c) >> TRB_TYPE_SHIFT) & 0x3Fu)

#define TT_NORMAL          1
#define TT_SETUP_STAGE     2
#define TT_DATA_STAGE      3
#define TT_STATUS_STAGE    4
#define TT_LINK            6
#define TT_ENABLE_SLOT     9
#define TT_DISABLE_SLOT    10
#define TT_ADDRESS_DEVICE  11
#define TT_CONFIGURE_EP    12
#define TT_EVALUATE_CTX    13
#define TT_NOOP_CMD        23
#define TT_TRANSFER_EVENT  32
#define TT_CMD_COMPLETION  33
#define TT_PORT_STATUS_CHG 34

static uptr cap_base, op_base, rt_base, db_base;   /* MMIO addresses: pointer-sized */
static u8  context_size_64 = 0;
static u8  max_slots = 0;
static u8  max_ports = 0;
static u8  max_slots_to_enable = 0;

static inline u32 mmio_r32(uptr addr) { return *(volatile u32*)addr; }
static inline void mmio_w32(uptr addr, u32 val) { *(volatile u32*)addr = val; }
static inline u64 mmio_r64(uptr addr) {
    u32 lo = mmio_r32(addr);
    u32 hi = mmio_r32(addr + 4);
    return ((u64)hi << 32) | lo;
}
static inline void mmio_w64(uptr addr, u64 val) {
    mmio_w32(addr, (u32)(val & 0xFFFFFFFFu));
    mmio_w32(addr + 4, (u32)(val >> 32));
}

#define OP(off)  (op_base + (off))
#define RT(off)  (rt_base + (off))
#define DB(off)  (db_base + (off))

#define USBCMD    OP(0x00)
#define USBSTS    OP(0x04)
#define CRCR      OP(0x18)
#define DCBAAP    OP(0x30)
#define CONFIG    OP(0x38)
#define PORTSC(n) OP(0x400 + (u32)(n) * 0x10)

#define USBCMD_RS    0x1u
#define USBCMD_HCRST 0x2u
#define USBSTS_HCH   0x1u
#define USBSTS_CNR   (1u << 11)

#define IR0_ERSTSZ RT(0x28)
#define IR0_ERSTBA RT(0x30)
#define IR0_ERDP   RT(0x38)

#define CMD_RING_TRBS   16
#define EVT_RING_TRBS   64
#define XFER_RING_TRBS  16
#define MAX_HID_DEVS    2

/* DMA structures. xHCI spec table 6-1: contexts, DCBAA and scratchpad must not cross a 4K page, TRB rings and
 * event ring segments must not cross 64K. where the compiler puts an unaligned static differs per build
 * (32 vs 64 bit, gcc vs clang) so it could break by luck: enumeration works, later key presses never arrive.
 * aligning each object to 4K (all <= 4K) fixes both rules. tests/verify_boot.py rechecks the final image */
#define XHCI_DMA_ALIGN __attribute__((aligned(4096)))
static u64 dcbaa[256]     XHCI_DMA_ALIGN;
static trb_t cmd_ring[CMD_RING_TRBS]   XHCI_DMA_ALIGN;
static trb_t evt_ring[EVT_RING_TRBS]   XHCI_DMA_ALIGN;
static u8    erst[16]                  XHCI_DMA_ALIGN;

static u8 input_ctx[1056]  XHCI_DMA_ALIGN;
static u8 device_ctx[MAX_HID_DEVS][1056] XHCI_DMA_ALIGN;     /* all MAX_HID_DEVS contexts share this one page */

typedef struct {
    u8  slot_id;
    u8  port;
    u8  in_use;
    hid_type_t type;
    u8  ep_dci;
    u8  report_len;
    u8  toggle;
    trb_t xfer_ring[XFER_RING_TRBS] __attribute__((aligned(64)));
    u32  xfer_enqueue;
    u8   xfer_cycle;
    u8   report_buf[8] __attribute__((aligned(16)));
    u8   prev_kbd[6];
} hid_dev_t;

static hid_dev_t hid_devs[MAX_HID_DEVS] XHCI_DMA_ALIGN;   /* holds each device's transfer ring: keep it inside one 64 KiB window */
static int hid_dev_count = 0;

static u32 cmd_enqueue = 0;
static u8  cmd_cycle = 1;
static u32 evt_dequeue = 0;
static u8  evt_cycle = 1;

static int controller_present = 0;

/* What happened during bring-up, printed at boot when no HID device came up (see xhci_diag_line). */
static struct {
    u16 hciver; u32 usbsts;
    u8  conn; u32 sc_before, sc_after; u8 speed;
    const char *step, *fail_step;
    u32 ccode, cmd_to, xfer_to;
} xd = { 0, 0, 0, 0, 0, 0, "-", "-", 0, 0, 0 };

static void trb_set(trb_t *t, u32 p_lo, u32 p_hi, u32 status, u32 control) {
    t->p_lo = p_lo; t->p_hi = p_hi; t->status = status; t->control = control;
}

static void ring_command_doorbell(void) { mmio_w32(DB(0), 0); }
static void ring_device_doorbell(u8 slot_id, u8 target) { mmio_w32(DB((u32)slot_id * 4), target); }

static int post_command(u32 p_lo, u32 p_hi, u32 status, u32 control, u8 *out_slot_id, u32 *out_ccode) {
    trb_t *t = &cmd_ring[cmd_enqueue];
    trb_set(t, p_lo, p_hi, status, (control & ~TRB_CYCLE) | (cmd_cycle ? TRB_CYCLE : 0));

    cmd_enqueue++;
    if (cmd_enqueue >= CMD_RING_TRBS - 1) {
        trb_t *link = &cmd_ring[CMD_RING_TRBS - 1];
        trb_set(link, dma_lo(cmd_ring), dma_hi(cmd_ring), 0,
                TRB_TYPE(TT_LINK) | 0x2u | (cmd_cycle ? TRB_CYCLE : 0));
        cmd_enqueue = 0;
        cmd_cycle ^= 1;
    }

    ring_command_doorbell();

    u32 timeout = 2000000;
    while (timeout--) {
        trb_t *ev = &evt_ring[evt_dequeue];
        u32 ctrl = ev->control;
        if ((ctrl & TRB_CYCLE) != evt_cycle) { io_wait(); continue; }

        u32 type = TRB_GET_TYPE(ctrl);
        if (type == TT_CMD_COMPLETION) {
            u32 ccode = (ev->status >> 24) & 0xFFu;
            u8 slot = (u8)((ctrl >> 24) & 0xFFu);
            evt_dequeue++;
            if (evt_dequeue >= EVT_RING_TRBS) { evt_dequeue = 0; evt_cycle ^= 1; }
            mmio_w64(IR0_ERDP, dma_addr(&evt_ring[evt_dequeue]) | (1u << 3));
            if (out_slot_id) *out_slot_id = slot;
            if (out_ccode) *out_ccode = ccode;
            xd.ccode = ccode;
            return (ccode == 1) ? 1 : 0;
        }
        evt_dequeue++;
        if (evt_dequeue >= EVT_RING_TRBS) { evt_dequeue = 0; evt_cycle ^= 1; }
        mmio_w64(IR0_ERDP, dma_addr(&evt_ring[evt_dequeue]) | (1u << 3));
    }
    xd.cmd_to++;                                   /* the controller never completed the command */
    return 0;
}

static trb_t ep0_ring[XFER_RING_TRBS] XHCI_DMA_ALIGN;
static u32   ep0_enqueue = 0;
static u8    ep0_cycle = 1;

static int wait_transfer_event(u32 *out_status) {
    u32 timeout = 2000000;
    while (timeout--) {
        trb_t *ev = &evt_ring[evt_dequeue];
        u32 ctrl = ev->control;
        if ((ctrl & TRB_CYCLE) != evt_cycle) { io_wait(); continue; }
        u32 type = TRB_GET_TYPE(ctrl);
        u32 status = ev->status;
        evt_dequeue++;
        if (evt_dequeue >= EVT_RING_TRBS) { evt_dequeue = 0; evt_cycle ^= 1; }
        mmio_w64(IR0_ERDP, dma_addr(&evt_ring[evt_dequeue]) | (1u << 3));
        if (type == TT_TRANSFER_EVENT) {
            if (out_status) *out_status = status;
            return 1;
        }
    }
    xd.xfer_to++;                                  /* no transfer event arrived */
    return 0;
}

static void ep0_push(trb_t *ring, u32 *enq, u8 *cyc, u32 p_lo, u32 p_hi, u32 status, u32 control) {
    trb_t *t = &ring[*enq];
    trb_set(t, p_lo, p_hi, status, (control & ~TRB_CYCLE) | (*cyc ? TRB_CYCLE : 0));
    (*enq)++;
    if (*enq >= XFER_RING_TRBS - 1) {
        trb_t *link = &ring[XFER_RING_TRBS - 1];
        trb_set(link, dma_lo(ring), dma_hi(ring), 0, TRB_TYPE(TT_LINK) | 0x2u | (*cyc ? TRB_CYCLE : 0));
        *enq = 0;
        *cyc ^= 1;
    }
}

static int control_transfer(u8 slot_id, u8 setup[8], void *data, u16 len, int data_in) {
    u32 setup_p_lo = *(u32*)&setup[0];
    u32 setup_p_hi = *(u32*)&setup[4];

    u32 trt = (len == 0) ? 0 : (data_in ? 3u : 2u);
    ep0_push(ep0_ring, &ep0_enqueue, &ep0_cycle,
             setup_p_lo, setup_p_hi, 8, TRB_TYPE(TT_SETUP_STAGE) | (1u << 6) | (trt << 16));

    if (len > 0) {
        ep0_push(ep0_ring, &ep0_enqueue, &ep0_cycle,
                 dma_lo(data), dma_hi(data), len, TRB_TYPE(TT_DATA_STAGE) | (data_in ? (1u << 16) : 0));
    }

    ep0_push(ep0_ring, &ep0_enqueue, &ep0_cycle,
             0, 0, 0, TRB_TYPE(TT_STATUS_STAGE) | ((len == 0 || !data_in) ? (1u << 16) : 0) | (1u << 5));

    ring_device_doorbell(slot_id, 1);

    u32 status;
    int ok = wait_transfer_event(&status);
    if (!ok) return 0;
    u32 ccode = (status >> 24) & 0xFFu;
    return (ccode == 1 || ccode == 13) ? 1 : 0;
}

static int get_descriptor(u8 slot_id, u8 type, u8 index, void *buf, u16 len) {
    u8 setup[8] = { 0x80, 0x06, index, type, 0x00, 0x00, (u8)(len & 0xFF), (u8)(len >> 8) };
    return control_transfer(slot_id, setup, buf, len, 1);
}

static int set_configuration(u8 slot_id, u8 cfg) {
    u8 setup[8] = { 0x00, 0x09, cfg, 0x00, 0x00, 0x00, 0x00, 0x00 };
    return control_transfer(slot_id, setup, NULL, 0, 1);
}

static int hid_set_boot_protocol(u8 slot_id, u8 iface) {
    u8 setup[8] = { 0x21, 0x0B, 0x00, 0x00, iface, 0x00, 0x00, 0x00 };
    return control_transfer(slot_id, setup, NULL, 0, 1);
}

static void slot_ctx_write(u8 *ctx32, u32 route, u8 speed, u8 ctx_entries, u8 root_port) {
    u32 *d = (u32*)ctx32;
    d[0] = (route & 0xFFFFFu) | ((u32)(speed & 0xFu) << 20) | ((u32)(ctx_entries & 0x1Fu) << 27);
    d[1] = ((u32)root_port << 16);
    d[2] = 0;
    d[3] = 0;
}

static void ep_ctx_write_interrupt_in(u8 *ctx32, u8 interval, u8 max_packet, u64 tr_ptr_with_dcs, u8 avg_trb_len_lo, u8 avg_trb_len_hi) {
    u32 *d = (u32*)ctx32;
    d[0] = ((u32)interval << 16);
    d[1] = ((u32)7u << 3) | (0u << 1) | ((u32)max_packet << 16);
    u32 tr_lo = (u32)(tr_ptr_with_dcs & 0xFFFFFFFFu);
    u32 tr_hi = (u32)(tr_ptr_with_dcs >> 32);
    d[2] = tr_lo;
    d[3] = tr_hi;
    d[4] = (u32)avg_trb_len_lo | ((u32)avg_trb_len_hi << 8);
    d[5] = 0; d[6] = 0; d[7] = 0;
}

static u8 dci_for_endpoint(u8 ep_addr) {
    u8 epnum = ep_addr & 0x0F;
    u8 dir_in = (ep_addr & 0x80) ? 1 : 0;
    return (u8)(2u * epnum + dir_in);
}

/* arm one Normal TRB on a device's interrupt-IN ring, advance enqueue and handle the wrap (Link TRB w/ Toggle Cycle).
 * rewriting index 0 with the same cycle bit every time doesn't look like new work to the controller after the first TRB */
static void arm_interrupt_in(hid_dev_t *dev) {
    trb_t *t = &dev->xfer_ring[dev->xfer_enqueue];
    trb_set(t, dma_lo(dev->report_buf), dma_hi(dev->report_buf), dev->report_len,
            TRB_TYPE(TT_NORMAL) | (1u << 5) | (dev->xfer_cycle ? TRB_CYCLE : 0));

    dev->xfer_enqueue++;
    if (dev->xfer_enqueue >= XFER_RING_TRBS - 1) {
        trb_t *link = &dev->xfer_ring[XFER_RING_TRBS - 1];
        trb_set(link, dma_lo(dev->xfer_ring), dma_hi(dev->xfer_ring), 0,
                TRB_TYPE(TT_LINK) | 0x2u | (dev->xfer_cycle ? TRB_CYCLE : 0));
        dev->xfer_enqueue = 0;
        dev->xfer_cycle ^= 1;
    }

    ring_device_doorbell(dev->slot_id, dev->ep_dci);
}

/* bring up one device on a 0-based root hub port. returns slot id (>0), 0 on failure */
static u8 bring_up_device_impl(u8 port0based, u8 speed) {
    u8 slot_id = 0; u32 ccode = 0;
    xd.step = "ENABLE_SLOT";
    if (!post_command(0, 0, 0, TRB_TYPE(TT_ENABLE_SLOT), &slot_id, &ccode) || slot_id == 0) return 0;
    if (slot_id >= MAX_HID_DEVS + 8) return 0;

    u8 *devctx = device_ctx[hid_dev_count];
    k_memset(devctx, 0, 1056);
    dcbaa[slot_id] = dma_addr(devctx);

    k_memset(input_ctx, 0, sizeof(input_ctx));
    u32 *ctrl = (u32*)input_ctx;
    /* Add Context Flags: bit0 = A0 = slot ctx, bit(DCI) = that endpoint ctx.
     * checked against real hw command completion, NOT the "DCI+1" offset some summaries imply */
    ctrl[1] = (1u << 0) | (1u << 1); /* slot + EP0 (dci=1) */

    u8 *slot_in = input_ctx + 32;
    slot_ctx_write(slot_in, 0, speed, 1, (u8)(port0based + 1));

    u8 *ep0_in = input_ctx + 64;
    u8 default_mps = (speed == 2) ? 8 : 64;
    ep0_enqueue = 0; ep0_cycle = 1;
    k_memset(ep0_ring, 0, sizeof(ep0_ring));
    u64 ep0_tr = dma_addr(ep0_ring) | 1u;
    {
        u32 *d = (u32*)ep0_in;
        d[1] = (4u << 3) | (3u << 1) | ((u32)default_mps << 16);
        d[2] = (u32)(ep0_tr & 0xFFFFFFFFu);
        d[3] = (u32)(ep0_tr >> 32);
        d[4] = 8;
    }

    xd.step = "ADDRESS_DEVICE";
    if (!post_command(dma_lo(input_ctx), dma_hi(input_ctx), 0, TRB_TYPE(TT_ADDRESS_DEVICE) | ((u32)slot_id << 24), NULL, &ccode)) {
        return 0;
    }

    u8 devdesc[18];
    k_memset(devdesc, 0, sizeof(devdesc));
    xd.step = "GET_DESCRIPTOR(device,8)";
    if (!get_descriptor(slot_id, 0x01, 0, devdesc, 8)) return 0;
    u8 real_mps = devdesc[7] ? devdesc[7] : default_mps;
    if (real_mps != default_mps) {
        k_memset(input_ctx, 0, sizeof(input_ctx));
        ((u32*)input_ctx)[1] = (1u << 1); /* evaluate EP0 (dci=1) */
        u32 *d = (u32*)(input_ctx + 64);
        d[1] = (4u << 3) | (3u << 1) | ((u32)real_mps << 16);
        post_command(dma_lo(input_ctx), dma_hi(input_ctx), 0, TRB_TYPE(TT_EVALUATE_CTX) | ((u32)slot_id << 24), NULL, &ccode);
    }

    xd.step = "GET_DESCRIPTOR(device,18)";
    if (!get_descriptor(slot_id, 0x01, 0, devdesc, 18)) return 0;

    u8 cfgbuf[64];
    k_memset(cfgbuf, 0, sizeof(cfgbuf));
    xd.step = "GET_DESCRIPTOR(config,9)";
    if (!get_descriptor(slot_id, 0x02, 0, cfgbuf, 9)) return 0;
    u16 total_len = (u16)(cfgbuf[2] | (cfgbuf[3] << 8));
    if (total_len > sizeof(cfgbuf)) total_len = sizeof(cfgbuf);
    xd.step = "GET_DESCRIPTOR(config,full)";
    if (!get_descriptor(slot_id, 0x02, 0, cfgbuf, total_len)) return 0;
    u8 cfg_value = cfgbuf[5];

    u8 iface_num = 0, iface_protocol = 0, ep_addr = 0, ep_maxpkt = 8, ep_interval = 8;
    int found_iface = 0;
    for (u16 off = 9; off + 2 <= total_len; ) {
        u8 dlen = cfgbuf[off], dtype = cfgbuf[off + 1];
        if (dlen == 0) break;
        if (dtype == 0x04 && off + 9 <= total_len) {
            if (cfgbuf[off + 5] == 0x03) { iface_num = cfgbuf[off + 2]; iface_protocol = cfgbuf[off + 7]; found_iface = 1; }
            else found_iface = 0;
        } else if (dtype == 0x05 && found_iface && off + 7 <= total_len) {
            if (cfgbuf[off + 2] & 0x80) {
                ep_addr = cfgbuf[off + 2];
                ep_maxpkt = cfgbuf[off + 4];
                ep_interval = cfgbuf[off + 6];
                if (ep_maxpkt == 0 || ep_maxpkt > 8) ep_maxpkt = 8;
                break;
            }
        }
        off += dlen;
    }

    xd.step = "SET_CONFIGURATION";
    if (!set_configuration(slot_id, cfg_value)) return 0;
    xd.step = "no HID interrupt endpoint in config";
    if (ep_addr == 0) return 0;
    hid_set_boot_protocol(slot_id, iface_num);

    u8 dci = dci_for_endpoint(ep_addr);

    hid_dev_t *dev = &hid_devs[hid_dev_count];
    k_memset(dev, 0, sizeof(*dev));
    dev->slot_id = slot_id;
    dev->port = port0based;
    dev->in_use = 1;
    dev->ep_dci = dci;
    dev->report_len = (iface_protocol == 1) ? 8 : 4;
    dev->type = (iface_protocol == 1) ? HID_KEYBOARD : HID_MOUSE;
    dev->xfer_enqueue = 0;
    dev->xfer_cycle = 1;

    k_memset(input_ctx, 0, sizeof(input_ctx));
    ((u32*)input_ctx)[1] = (1u << 0) | (1u << dci); /* A0=slot, A(dci)=this endpoint */
    u8 *slot_upd = input_ctx + 32;
    k_memcpy(slot_upd, devctx, 32);
    u32 *sd = (u32*)slot_upd;
    sd[0] = (sd[0] & ~((u32)0x1Fu << 27)) | ((u32)dci << 27);

    u8 *ep_in = input_ctx + 32 + (u32)dci * 32;
    u64 tr_ptr = dma_addr(dev->xfer_ring) | 1u;
    ep_ctx_write_interrupt_in(ep_in, ep_interval, ep_maxpkt, tr_ptr, dev->report_len, 0);

    xd.step = "CONFIGURE_ENDPOINT";
    if (!post_command(dma_lo(input_ctx), dma_hi(input_ctx), 0, TRB_TYPE(TT_CONFIGURE_EP) | ((u32)slot_id << 24), NULL, &ccode)) return 0;

    arm_interrupt_in(dev);

    hid_register((dev->type == HID_KEYBOARD) ? "usb-keyboard (xhci)" : "usb-mouse (xhci)", dev->type);
    hid_dev_count++;
    return slot_id;
}

static u8 bring_up_device(u8 port0based, u8 speed) {
    u8 r = bring_up_device_impl(port0based, speed);
    if (!r) xd.fail_step = xd.step;
    else    xd.step = "done";
    return r;
}

#define PORTSC_CCS   0x1u
#define PORTSC_PR    (1u << 4)
#define PORTSC_PRC   (1u << 21)
#define PORTSC_CSC   (1u << 17)
/* PORTSC RW bits to preserve on write: 2-4 (incl PR, harmless), bit 9 PP (port power), 25-27 wake enables.
 * leaving PP out was a real bug, it killed port power mid-reset and the device silently disconnected */
#define PORTSC_PRESERVE_MASK 0x0E00021Cu

void xhci_enumerate(void) {
    if (!controller_present) return;
    for (u8 p = 0; p < max_ports && hid_dev_count < MAX_HID_DEVS; p++) {
        u32 sc = mmio_r32(PORTSC(p));
        if (!(sc & PORTSC_CCS)) continue;
        xd.conn++; xd.sc_before = sc;

        mmio_w32(PORTSC(p), (sc & PORTSC_PRESERVE_MASK) | PORTSC_PR);
        for (volatile int i = 0; i < 500000; i++) io_wait();

        sc = mmio_r32(PORTSC(p));
        if (!(sc & PORTSC_CCS)) continue;
        mmio_w32(PORTSC(p), (sc & PORTSC_PRESERVE_MASK) | PORTSC_PRC | PORTSC_CSC);

        u8 speed = (u8)((sc >> 10) & 0xFu);
        xd.speed = speed; xd.sc_after = sc;
        bring_up_device(p, speed);
    }
}

void xhci_poll(void) {
    if (!controller_present) return;

    for (int guard = 0; guard < 8; guard++) {
        trb_t *ev = &evt_ring[evt_dequeue];
        u32 ctrl = ev->control;
        if ((ctrl & TRB_CYCLE) != evt_cycle) break;
        dma_rmb();                                  /* event is valid: only now read the report buffer it describes */

        u32 type = TRB_GET_TYPE(ctrl);
        if (type == TT_TRANSFER_EVENT) {
            u8 slot_id = (u8)((ctrl >> 24) & 0xFFu);
            u8 dci     = (u8)((ctrl >> 16) & 0x1Fu);
            u32 ccode  = (ev->status >> 24) & 0xFFu;

            for (int i = 0; i < hid_dev_count; i++) {
                hid_dev_t *dev = &hid_devs[i];
                if (!dev->in_use || dev->slot_id != slot_id || dev->ep_dci != dci) continue;
                if (ccode == 1 || ccode == 13) {
                    if (dev->type == HID_KEYBOARD) {
                        static const char usage_to_ascii[] = {
                            0,0,0,0,'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r',
                            's','t','u','v','w','x','y','z','1','2','3','4','5','6','7','8','9','0',
                            '\n',27,'\b','\t',' ','-','=','[',']','\\',0,';','\'','`',',','.','/'
                        };
                        u8 mods = dev->report_buf[0];
                        for (int k = 2; k < 8; k++) {
                            u8 usage = dev->report_buf[k];
                            int was_pressed = 0;
                            for (int j = 0; j < 6; j++) if (dev->prev_kbd[j] == usage) was_pressed = 1;
                            if (usage != 0 && !was_pressed && usage < sizeof(usage_to_ascii)) {
                                char c = usage_to_ascii[usage];
                                if ((mods & 0x22) && c >= 'a' && c <= 'z') c -= 32;
                                if (c) hid_push_key(c);
                            }
                        }
                        for (int j = 0; j < 6; j++) dev->prev_kbd[j] = dev->report_buf[2 + j];
                    } else {
                        u8 buttons = dev->report_buf[0];
                        s8 dx = (s8)dev->report_buf[1];
                        s8 dy = (s8)dev->report_buf[2];
                        hid_push_mouse_packet(dx, (s8)(-dy),
                            (buttons & 1) != 0, (buttons & 2) != 0, (buttons & 4) != 0);
                    }
                }
                arm_interrupt_in(dev);
                break;
            }
        }

        evt_dequeue++;
        if (evt_dequeue >= EVT_RING_TRBS) { evt_dequeue = 0; evt_cycle ^= 1; }
        mmio_w64(IR0_ERDP, dma_addr(&evt_ring[evt_dequeue]) | (1u << 3));
    }
}

int xhci_present(void) { return controller_present; }
int xhci_hid_count(void) { return hid_dev_count; }

#define SCRATCHPAD_MAX 8
static u8  scratchpad_bufs[SCRATCHPAD_MAX][4096] __attribute__((aligned(4096)));
_Static_assert(sizeof(dcbaa) <= 4096 && sizeof(input_ctx) <= 4096 && sizeof(device_ctx) <= 4096 &&
               sizeof(cmd_ring) <= 4096 && sizeof(evt_ring) <= 4096 && sizeof(ep0_ring) <= 4096 && sizeof(hid_devs) <= 4096,
               "an xHCI DMA structure outgrew its 4 KiB page: revisit the alignment rules above");
static u64 scratchpad_array[SCRATCHPAD_MAX]       XHCI_DMA_ALIGN;

int xhci_module_init(void) {
    controller_present = 0;
    hid_dev_count = 0;
    k_memset(hid_devs, 0, sizeof(hid_devs));

    pci_device_t dev;
    int found = 0;
    for (int i = 0; i < pci_device_count() && !found; i++) {
        const pci_device_t *d = pci_device_at(i);
        if (d && d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30) {
            dev = *d;
            found = 1;
        }
    }
    if (!found) return 0;

    u32 bar0 = pci_config_read32(dev.bus, dev.slot, dev.func, 0x10);
    u32 bar1 = pci_config_read32(dev.bus, dev.slot, dev.func, 0x14);
    int wide;
    u64 base = mmio_bar_base(bar0, bar1, &wide);
    if (base == 0) return 0;

    u16 cmd = pci_config_read16(dev.bus, dev.slot, dev.func, 0x04);
    pci_config_write16(dev.bus, dev.slot, dev.func, 0x04, (u16)(cmd | 0x0002 | 0x0004));

    /* below 4G: identity-mapped like before. above 4G (64-bit BAR placed high) needs x86_64's on-demand mapping.
     * on i686 mmio_map() returns NULL and we skip the controller like the old code did */
    volatile void *regs = mmio_map(base, 0x10000);
    if (!regs) return 0;

    cap_base = (uptr)regs;
    u8 caplength = *(volatile u8*)cap_base;
    u32 hcsparams1 = mmio_r32(cap_base + 0x04);
    u32 hcsparams2 = mmio_r32(cap_base + 0x08);
    u32 hccparams1 = mmio_r32(cap_base + 0x10);
    u32 dboff = mmio_r32(cap_base + 0x14) & ~0x3u;
    u32 rtsoff = mmio_r32(cap_base + 0x18) & ~0x1Fu;

    max_slots = (u8)(hcsparams1 & 0xFFu);
    max_ports = (u8)((hcsparams1 >> 24) & 0xFFu);
    context_size_64 = (u8)((hccparams1 >> 2) & 0x1u);
    if (context_size_64) return 0;
    if (max_slots == 0 || max_ports == 0 || max_ports > 32) return 0;

    op_base = cap_base + caplength;
    rt_base = cap_base + rtsoff;
    db_base = cap_base + dboff;

    mmio_w32(USBCMD, mmio_r32(USBCMD) & ~USBCMD_RS);
    u32 timeout = 500000;
    while ((mmio_r32(USBSTS) & USBSTS_HCH) == 0 && timeout--) io_wait();

    mmio_w32(USBCMD, mmio_r32(USBCMD) | USBCMD_HCRST);
    timeout = 1000000;
    while ((mmio_r32(USBCMD) & USBCMD_HCRST) && timeout--) io_wait();
    timeout = 1000000;
    while ((mmio_r32(USBSTS) & USBSTS_CNR) && timeout--) io_wait();
    if (mmio_r32(USBSTS) & USBSTS_CNR) return 0;

    max_slots_to_enable = (max_slots < 8) ? max_slots : 8;
    mmio_w32(CONFIG, max_slots_to_enable);

    k_memset(dcbaa, 0, sizeof(dcbaa));

    u32 max_scratch = ((hcsparams2 >> 27) & 0x1Fu) << 5 | ((hcsparams2 >> 21) & 0x1Fu);
    if (max_scratch > SCRATCHPAD_MAX) max_scratch = SCRATCHPAD_MAX;
    if (max_scratch > 0) {
        for (u32 i = 0; i < max_scratch; i++) scratchpad_array[i] = dma_addr(scratchpad_bufs[i]);
        dcbaa[0] = dma_addr(scratchpad_array);
    }

    mmio_w64(DCBAAP, dma_addr(dcbaa));

    k_memset(cmd_ring, 0, sizeof(cmd_ring));
    cmd_enqueue = 0; cmd_cycle = 1;
    mmio_w64(CRCR, dma_addr(cmd_ring) | 1u);

    k_memset(evt_ring, 0, sizeof(evt_ring));
    evt_dequeue = 0; evt_cycle = 1;
    k_memset(erst, 0, sizeof(erst));
    {
        u64 *e = (u64*)erst;
        e[0] = dma_addr(evt_ring);
        *((u32*)(erst + 8)) = EVT_RING_TRBS;
    }
    mmio_w32(IR0_ERSTSZ, 1);
    mmio_w64(IR0_ERSTBA, dma_addr(erst));
    mmio_w64(IR0_ERDP, dma_addr(evt_ring));

    mmio_w32(USBCMD, mmio_r32(USBCMD) | USBCMD_RS);
    timeout = 500000;
    while ((mmio_r32(USBSTS) & USBSTS_HCH) && timeout--) io_wait();
    if (mmio_r32(USBSTS) & USBSTS_HCH) return 0;

    controller_present = 1;
    xd.hciver = (u16)(mmio_r32(cap_base) >> 16);
    xd.usbsts = mmio_r32(USBSTS);
    return 1;
}

void xhci_module_exit(void) {
    if (controller_present) mmio_w32(USBCMD, mmio_r32(USBCMD) & ~USBCMD_RS);
    controller_present = 0;
}

/* Two short lines describing xHCI bring-up, for the boot screen when no HID device came up. */
void xhci_diag_line(int which, char *out, int max) {
    if (which == 0)
        k_snprintf(out, (size_t)max, "xHCI v%x: %u ports, %u device(s) connected, PORTSC %x -> %x, speed %u, USBSTS %x",
                   (u32)xd.hciver, (u32)max_ports, (u32)xd.conn, xd.sc_before, xd.sc_after, (u32)xd.speed, xd.usbsts);
    else
        k_snprintf(out, (size_t)max, "failed at: %s (last completion code %u, command timeouts %u, transfer timeouts %u)",
                   xd.fail_step, xd.ccode, xd.cmd_to, xd.xfer_to);
}
