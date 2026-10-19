#ifndef VESPEROS_PCI_H
#define VESPEROS_PCI_H
#include "../include/types.h"

typedef struct {
    u8  bus, slot, func;
    u16 vendor_id, device_id;
    u8  class_code, subclass, prog_if;
    u32 bar4;
} pci_device_t;

void pci_scan(void);
u32  pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset);
u16  pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset);
void pci_config_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 val);

int  pci_find_uhci(pci_device_t *out);
int  pci_device_count(void);
const pci_device_t *pci_device_at(int i);

#endif
