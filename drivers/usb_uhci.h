#ifndef VESPEROS_USB_UHCI_H
#define VESPEROS_USB_UHCI_H
#include "../include/types.h"

int  usb_uhci_module_init(void);
void usb_uhci_module_exit(void);
void usb_uhci_enumerate(void);
void usb_uhci_poll(void);
int  usb_uhci_present(void);
int  usb_uhci_hid_count(void);

#endif
