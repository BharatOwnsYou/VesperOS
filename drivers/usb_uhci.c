/*
 * VesperOS 4 - UHCI usb host driver. usb1.1, io-port based, boot-protocol kbd/mice on the root hub only.
 * works in qemu -usb. real B550M boards mostly route the main ports through xHCI (drivers/xhci.c)
 */
#include "../include/dma.h"
#include "usb_uhci.h"
#include "pci.h"
#include "hid.h"
#include "../include/io.h"
#include "../kernel/string.h"

#define UHCI_USBCMD   0x00
#define UHCI_USBSTS   0x02
#define UHCI_USBINTR  0x04
#define UHCI_FRNUM    0x06
#define UHCI_FRBASEADD 0x08
#define UHCI_SOFMOD   0x0C
#define UHCI_PORTSC1  0x10
#define UHCI_PORTSC2  0x12

#define USBCMD_RS     0x01
#define USBCMD_HCRESET 0x02
#define USBCMD_GRESET 0x04
#define USBCMD_CF     0x40
#define USBCMD_MAXP   0x80

#define PORTSC_CONNECT      0x0001
#define PORTSC_CONNECT_CHG  0x0002
#define PORTSC_ENABLE       0x0004
#define PORTSC_LOW_SPEED    0x0100
#define PORTSC_RESET        0x0200

typedef struct { u32 link; u32 cs; u32 token; u32 buffer; } __attribute__((packed, aligned(16))) uhci_td_t;
typedef struct { u32 head_link; u32 element_link; u32 _pad0; u32 _pad1; } __attribute__((packed, aligned(16))) uhci_qh_t;

#define TD_LINK_TERM   0x00000001u
#define TD_LINK_QH     0x00000002u
#define TD_LINK_DEPTH  0x00000004u

#define TD_CS_ACTLEN_MASK 0x7FF
#define TD_CS_STALLED   (1u<<22)
#define TD_CS_DBUFERR   (1u<<21)
#define TD_CS_BABBLE    (1u<<20)
#define TD_CS_NAK       (1u<<19)
#define TD_CS_CRCTO     (1u<<18)
#define TD_CS_BITSTUFF  (1u<<17)
#define TD_CS_ACTIVE    (1u<<23)
#define TD_CS_IOC       (1u<<24)
#define TD_CS_LS        (1u<<26)
#define TD_CS_CERR(n)   ((u32)(n)<<27)
#define TD_CS_SPD       (1u<<29)
#define TD_CS_ERR_MASK  (TD_CS_STALLED|TD_CS_DBUFERR|TD_CS_BABBLE|TD_CS_CRCTO|TD_CS_BITSTUFF)

#define PID_IN    0x69
#define PID_OUT   0xE1
#define PID_SETUP 0x2D

static u32 mk_token(u8 pid, u8 addr, u8 ep, u8 toggle, u16 len) {
    u32 maxlen = (len == 0) ? 0x7FF : ((u32)(len - 1) & 0x7FF);
    return (u32)pid | ((u32)(addr & 0x7F) << 8) | ((u32)(ep & 0x0F) << 15)
         | ((u32)(toggle & 1) << 19) | (maxlen << 21);
}

static u32 frame_list[1024] __attribute__((aligned(4096)));

static uhci_qh_t schedule_qh[3] __attribute__((aligned(16)));
static uhci_td_t enum_tds[8]    __attribute__((aligned(16)));
static uhci_td_t kbd_td         __attribute__((aligned(16)));
static uhci_td_t mouse_td       __attribute__((aligned(16)));

static u16 io_base = 0;
static int controller_present = 0;

typedef struct {
    bool     in_use;
    hid_type_t type;
    u8       dev_addr;
    u8       endpoint;
    u8       maxpacket;
    u8       ls;
    u8       toggle;
    u8       report_len;
    u8       prev_report[8];
} usb_hid_dev_t;

static usb_hid_dev_t hid_kbd = {0};
static usb_hid_dev_t hid_mouse = {0};

static void reg_w16(u16 off, u16 val) { outw((u16)(io_base + off), val); }
static u16  reg_r16(u16 off)          { return inw((u16)(io_base + off)); }
static void reg_w32(u16 off, u32 val) { outl((u16)(io_base + off), val); }

static int run_control_chain(uhci_td_t *tds, int count) {
    for (int i = 0; i < count; i++) {
        tds[i].link = (i == count - 1)
            ? TD_LINK_TERM
            : ((dma_lo(&tds[i + 1])) | TD_LINK_DEPTH);
    }
    schedule_qh[0].element_link = dma_lo(&tds[0]);

    reg_w16(UHCI_USBCMD, USBCMD_RS | USBCMD_CF | USBCMD_MAXP);

    int timeout = 200000;
    while (timeout--) {
        if (schedule_qh[0].element_link & TD_LINK_TERM) break;
        u32 last_cs = tds[count - 1].cs;
        if (!(last_cs & TD_CS_ACTIVE)) break;
        io_wait();
    }

    int ok = 1;
    for (int i = 0; i < count; i++) {
        if (tds[i].cs & TD_CS_ERR_MASK) ok = 0;
        if (tds[i].cs & TD_CS_ACTIVE) ok = 0;
    }
    schedule_qh[0].element_link = TD_LINK_TERM;
    return ok;
}

static int control_transfer(u8 addr, u8 maxpacket, u8 ls,
                             u8 setup[8], void *data, u16 data_len, int data_in) {
    int n = 0;
    u32 cs_base = TD_CS_ACTIVE | TD_CS_CERR(3) | (ls ? TD_CS_LS : 0);

    enum_tds[n].cs = cs_base;
    enum_tds[n].token = mk_token(PID_SETUP, addr, 0, 0, 8);
    enum_tds[n].buffer = dma_lo(setup);
    n++;

    u8 toggle = 1;
    u16 remaining = data_len;
    u8 *p = (u8*)data;
    while (remaining > 0 && n < 7) {
        u16 chunk = (remaining > maxpacket) ? maxpacket : remaining;
        enum_tds[n].cs = cs_base;
        enum_tds[n].token = mk_token(data_in ? PID_IN : PID_OUT, addr, 0, toggle, chunk);
        enum_tds[n].buffer = dma_lo(p);
        n++;
        p += chunk;
        remaining -= chunk;
        toggle ^= 1;
    }

    int status_in = (data_len > 0) ? !data_in : 1;
    enum_tds[n].cs = cs_base | TD_CS_IOC;
    enum_tds[n].token = mk_token(status_in ? PID_IN : PID_OUT, addr, 0, 1, 0);
    enum_tds[n].buffer = 0;
    n++;

    return run_control_chain(enum_tds, n);
}

static int get_descriptor(u8 addr, u8 maxpacket, u8 ls, u8 type, u8 index, void *buf, u16 len) {
    u8 setup[8] = { 0x80, 0x06, index, type, 0x00, 0x00, (u8)(len & 0xFF), (u8)(len >> 8) };
    return control_transfer(addr, maxpacket, ls, setup, buf, len, 1);
}

static int set_address(u8 old_addr, u8 maxpacket, u8 ls, u8 new_addr) {
    u8 setup[8] = { 0x00, 0x05, new_addr, 0x00, 0x00, 0x00, 0x00, 0x00 };
    return control_transfer(old_addr, maxpacket, ls, setup, NULL, 0, 1);
}

static int set_configuration(u8 addr, u8 maxpacket, u8 ls, u8 cfg) {
    u8 setup[8] = { 0x00, 0x09, cfg, 0x00, 0x00, 0x00, 0x00, 0x00 };
    return control_transfer(addr, maxpacket, ls, setup, NULL, 0, 1);
}

static int hid_set_boot_protocol(u8 addr, u8 maxpacket, u8 ls, u8 iface) {
    u8 setup[8] = { 0x21, 0x0B, 0x00, 0x00, iface, 0x00, 0x00, 0x00 };
    return control_transfer(addr, maxpacket, ls, setup, NULL, 0, 1);
}

static int reset_and_enumerate_port(u16 portsc_off, u8 new_addr) {
    u16 sc = reg_r16(portsc_off);
    if (!(sc & PORTSC_CONNECT)) return 0;

    reg_w16(portsc_off, sc | PORTSC_RESET);
    for (volatile int i = 0; i < 500000; i++) io_wait();
    sc = reg_r16(portsc_off);
    reg_w16(portsc_off, sc & (u16)~PORTSC_RESET);
    for (volatile int i = 0; i < 20000; i++) io_wait();

    sc = reg_r16(portsc_off);
    reg_w16(portsc_off, sc | PORTSC_ENABLE);
    for (volatile int i = 0; i < 20000; i++) io_wait();

    sc = reg_r16(portsc_off);
    if (!(sc & PORTSC_CONNECT)) return 0;
    u8 ls = (sc & PORTSC_LOW_SPEED) ? 1 : 0;

    u8 devdesc[18];
    k_memset(devdesc, 0, sizeof(devdesc));
    if (!get_descriptor(0, 8, ls, 0x01, 0, devdesc, 8)) return 0;
    u8 mps0 = devdesc[7] ? devdesc[7] : 8;

    if (!set_address(0, mps0, ls, new_addr)) return 0;
    for (volatile int i = 0; i < 20000; i++) io_wait();

    if (!get_descriptor(new_addr, mps0, ls, 0x01, 0, devdesc, 18)) return 0;

    u8 cfgbuf[64];
    k_memset(cfgbuf, 0, sizeof(cfgbuf));
    if (!get_descriptor(new_addr, mps0, ls, 0x02, 0, cfgbuf, 9)) return 0;
    u16 total_len = (u16)(cfgbuf[2] | (cfgbuf[3] << 8));
    if (total_len > sizeof(cfgbuf)) total_len = sizeof(cfgbuf);
    if (!get_descriptor(new_addr, mps0, ls, 0x02, 0, cfgbuf, total_len)) return 0;
    u8 cfg_value = cfgbuf[5];

    u8 iface_num = 0, iface_protocol = 0;
    u8 ep_addr = 0, ep_maxpkt = 8;
    int found_iface = 0;
    for (u16 off = 9; off + 2 <= total_len; ) {
        u8 dlen = cfgbuf[off];
        u8 dtype = cfgbuf[off + 1];
        if (dlen == 0) break;
        if (dtype == 0x04 && off + 9 <= total_len) {
            if (cfgbuf[off + 5] == 0x03) {
                iface_num = cfgbuf[off + 2];
                iface_protocol = cfgbuf[off + 7];
                found_iface = 1;
            } else {
                found_iface = 0;
            }
        } else if (dtype == 0x05 && found_iface && off + 7 <= total_len) {
            if (cfgbuf[off + 2] & 0x80) {
                ep_addr = (u8)(cfgbuf[off + 2] & 0x0F);
                ep_maxpkt = cfgbuf[off + 4];
                if (ep_maxpkt == 0 || ep_maxpkt > 8) ep_maxpkt = 8;
                break;
            }
        }
        off += dlen;
    }

    if (!set_configuration(new_addr, mps0, ls, cfg_value)) return 0;
    if (ep_addr == 0) return 0;
    hid_set_boot_protocol(new_addr, mps0, ls, iface_num);

    if (iface_protocol == 1 && !hid_kbd.in_use) {
        hid_kbd = (usb_hid_dev_t){ .in_use = true, .type = HID_KEYBOARD, .dev_addr = new_addr,
            .endpoint = ep_addr, .maxpacket = ep_maxpkt, .ls = ls, .toggle = 0, .report_len = 8 };
        hid_register("usb-keyboard (uhci)", HID_KEYBOARD);
        kbd_td.cs = 0;
    } else if (iface_protocol == 2 && !hid_mouse.in_use) {
        hid_mouse = (usb_hid_dev_t){ .in_use = true, .type = HID_MOUSE, .dev_addr = new_addr,
            .endpoint = ep_addr, .maxpacket = ep_maxpkt, .ls = ls, .toggle = 0, .report_len = 4 };
        hid_register("usb-mouse (uhci)", HID_MOUSE);
        mouse_td.cs = 0;
    }
    return 1;
}

void usb_uhci_enumerate(void) {
    if (!controller_present) return;
    u8 next_addr = 2;
    if (reset_and_enumerate_port(UHCI_PORTSC1, next_addr)) next_addr++;
    reset_and_enumerate_port(UHCI_PORTSC2, next_addr);

    schedule_qh[0].head_link = dma_lo(&schedule_qh[1]) | TD_LINK_QH;
    schedule_qh[1].head_link = dma_lo(&schedule_qh[2]) | TD_LINK_QH;
    schedule_qh[2].head_link = TD_LINK_TERM;
    schedule_qh[1].element_link = TD_LINK_TERM;
    schedule_qh[2].element_link = TD_LINK_TERM;
}

static void arm_poll_td(uhci_td_t *td, uhci_qh_t *qh, usb_hid_dev_t *dev, u8 *buf) {
    td->link = TD_LINK_TERM;
    td->token = mk_token(PID_IN, dev->dev_addr, dev->endpoint, dev->toggle, dev->report_len);
    td->buffer = dma_lo(buf);
    td->cs = TD_CS_ACTIVE | TD_CS_CERR(3) | (dev->ls ? TD_CS_LS : 0) | TD_CS_SPD;
    qh->element_link = dma_lo(td);
}

static u8 kbd_buf[8];
static u8 mouse_buf[8];
static u8 kbd_pressed_prev[6] = {0};

static void poll_one(usb_hid_dev_t *dev, uhci_td_t *td, uhci_qh_t *qh, u8 *buf) {
    if (!dev->in_use) return;

    if (td->cs == 0) { arm_poll_td(td, qh, dev, buf); return; }

    if (td->cs & TD_CS_ACTIVE) return;

    if (td->cs & TD_CS_ERR_MASK) {
        arm_poll_td(td, qh, dev, buf);
        return;
    }

    if (dev->type == HID_KEYBOARD) {
        u8 mods = buf[0];
        static const char usage_to_ascii[] = {
            0,0,0,0,'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r',
            's','t','u','v','w','x','y','z','1','2','3','4','5','6','7','8','9','0',
            '\n',27,'\b','\t',' ','-','=','[',']','\\',0,';','\'','`',',','.','/'
        };
        for (int i = 2; i < 8; i++) {
            u8 usage = buf[i];
            bool was_pressed = false;
            for (int j = 0; j < 6; j++) if (kbd_pressed_prev[j] == usage) was_pressed = true;
            if (usage != 0 && !was_pressed && usage < sizeof(usage_to_ascii)) {
                char c = usage_to_ascii[usage];
                if (mods & 0x22) {
                    if (c >= 'a' && c <= 'z') c -= 32;
                }
                if (c) hid_push_key(c);
            }
        }
        for (int j = 0; j < 6; j++) kbd_pressed_prev[j] = buf[2 + j];
    } else if (dev->type == HID_MOUSE) {
        u8 buttons = buf[0];
        s8 dx = (s8)buf[1];
        s8 dy = (s8)buf[2];
        hid_push_mouse_packet(dx, (s8)(-dy),
            (buttons & 0x01) != 0, (buttons & 0x02) != 0, (buttons & 0x04) != 0);
    }

    dev->toggle ^= 1;
    arm_poll_td(td, qh, dev, buf);
}

void usb_uhci_poll(void) {
    if (!controller_present) return;
    poll_one(&hid_kbd, &kbd_td, &schedule_qh[1], kbd_buf);
    poll_one(&hid_mouse, &mouse_td, &schedule_qh[2], mouse_buf);
}

int usb_uhci_present(void) { return controller_present; }
int usb_uhci_hid_count(void) { return (hid_kbd.in_use ? 1 : 0) + (hid_mouse.in_use ? 1 : 0); }

int usb_uhci_module_init(void) {
    pci_device_t dev;
    controller_present = 0;
    hid_kbd.in_use = false;
    hid_mouse.in_use = false;
    kbd_td.cs = 0; mouse_td.cs = 0;

    if (!pci_find_uhci(&dev)) return 0;

    io_base = (u16)(dev.bar4 & 0xFFFC);
    if (io_base == 0) return 0;

    /* UHCI DMA is 32-bit only so every schedule struct, buffer and the stack (setup packets live there)
     * has to be under 4G. static data always is but check anyway, refuse the controller
     * instead of corrupting memory */
    {
        u32 chk; u8 on_stack;
        if (dma_lo32_checked(frame_list, &chk) || dma_lo32_checked(&schedule_qh[2], &chk) ||
            dma_lo32_checked(&enum_tds[7], &chk) || dma_lo32_checked(&kbd_td, &chk) ||
            dma_lo32_checked(&mouse_td, &chk) || dma_lo32_checked(&on_stack, &chk)) return 0;
    }

    u16 cmd = pci_config_read16(dev.bus, dev.slot, dev.func, 0x04);
    pci_config_write16(dev.bus, dev.slot, dev.func, 0x04, (u16)(cmd | 0x0001 | 0x0004));

    reg_w16(UHCI_USBCMD, USBCMD_GRESET);
    for (volatile int i = 0; i < 50000; i++) io_wait();
    reg_w16(UHCI_USBCMD, 0);

    reg_w16(UHCI_USBINTR, 0);
    reg_w16(UHCI_FRNUM, 0);

    k_memset(frame_list, 0, sizeof(frame_list));
    k_memset(schedule_qh, 0, sizeof(schedule_qh));
    for (int i = 0; i < 3; i++) {
        schedule_qh[i].head_link = TD_LINK_TERM;
        schedule_qh[i].element_link = TD_LINK_TERM;
    }
    for (int i = 0; i < 1024; i++) frame_list[i] = dma_lo(&schedule_qh[0]) | TD_LINK_QH;

    reg_w32(UHCI_FRBASEADD, dma_lo(frame_list));
    reg_w16(UHCI_USBCMD, USBCMD_RS | USBCMD_CF | USBCMD_MAXP);

    controller_present = 1;
    return 1;
}

void usb_uhci_module_exit(void) {
    if (controller_present) reg_w16(UHCI_USBCMD, 0);
    controller_present = 0;
}
