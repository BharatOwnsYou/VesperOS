#ifndef VESPEROS_RTL8168_H
#define VESPEROS_RTL8168_H
#include "pci.h"
/* Realtek RTL8111/8168/8211/8411 PCIe gigabit (10ec:8168, 10ec:8161), whole family incl 8168H/8111H (rev 0x15).
 * polled, 32 rx / 16 tx descriptors, one instance */
int rtl8168_probe(const pci_device_t *d);

/* Register/ring snapshot for 'hespernetctl nic' (real-hardware bring-up). 0 if no RTL8168 is bound. */
typedef struct {
    bool use_io; u32 base;
    u32  xid;                         /* chip generation code: (TCR >> 20) & 0x7CF */
    u8   cr, phystatus; u16 cplus, isr;
    u32  tcr, rcr, misc;
    u16  tx_cur, rx_cur;
    u32  tx_last_opts1, rx_head_opts1;  /* TX: OWN still set long after sending = NIC never fetched it */
    u32  tx_kicks;
    u8   cr_after_init, cr_readback;   /* ChipCmd right after init / right after the latest enable write */
    u32  cr_fixups;                    /* times TX/RX enable was (re)written; 2 = init only */
} rtl8168_diag_t;
int rtl8168_diag(rtl8168_diag_t *out);
#endif
