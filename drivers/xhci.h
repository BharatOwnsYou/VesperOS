#ifndef VESPEROS_XHCI_H
#define VESPEROS_XHCI_H
#include "../include/types.h"

/*
 * VesperOS 4 - xHCI driver. the native usb2/3 interface on modern boards (incl ASRock B550), unlike
 * UHCI (usb1.1, io-port) it's MMIO with command/event rings.
 *
 * scope: boot-protocol HID kbd + mice on the root hub only. no hubs behind hubs, no streams, no isoc/bulk.
 * only 32-byte device contexts (HCCPARAMS1.CSZ==0, the common case), 64-byte ones are reported as unsupported
 */

int  xhci_module_init(void);
void xhci_module_exit(void);
void xhci_enumerate(void);
void xhci_poll(void);
int  xhci_present(void);
void xhci_diag_line(int which, char *out, int max);   /* 0 = controller summary, 1 = where enumeration failed */
int  xhci_hid_count(void);

#endif
