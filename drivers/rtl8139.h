#ifndef VESPEROS_RTL8139_H
#define VESPEROS_RTL8139_H
#include "pci.h"
/* Realtek RTL8139 Fast Ethernet (QEMU "-nic model=rtl8139", old PCI cards). One instance. */
int rtl8139_probe(const pci_device_t *d);
#endif
