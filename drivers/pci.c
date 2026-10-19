#include "pci.h"
#include "../include/io.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define MAX_PCI_DEVICES 128

static pci_device_t devs[MAX_PCI_DEVICES];
static int dev_count = 0;

static u32 pci_addr(u8 bus, u8 slot, u8 func, u8 offset) {
    return (u32)0x80000000
        | ((u32)bus << 16)
        | ((u32)(slot & 0x1F) << 11)
        | ((u32)(func & 0x07) << 8)
        | (offset & 0xFC);
}

u32 pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

u16 pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 v = pci_config_read32(bus, slot, func, offset & 0xFC);
    return (u16)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_config_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 val) {
    u32 addr = pci_addr(bus, slot, func, offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    u32 old = inl(PCI_CONFIG_DATA);
    u32 shift = (offset & 2) * 8;
    u32 mask = 0xFFFF << shift;
    u32 nv = (old & ~mask) | ((u32)val << shift);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, nv);
}

void pci_scan(void) {
    dev_count = 0;
    for (u16 bus = 0; bus < 256 && dev_count < MAX_PCI_DEVICES; bus++) {
        for (u8 slot = 0; slot < 32 && dev_count < MAX_PCI_DEVICES; slot++) {
            for (u8 func = 0; func < 8 && dev_count < MAX_PCI_DEVICES; func++) {
                u16 vendor = pci_config_read16((u8)bus, slot, func, 0x00);
                if (vendor == 0xFFFF) continue;

                pci_device_t *d = &devs[dev_count++];
                d->bus = (u8)bus; d->slot = slot; d->func = func;
                d->vendor_id = vendor;
                d->device_id = pci_config_read16((u8)bus, slot, func, 0x02);

                u16 classreg = pci_config_read16((u8)bus, slot, func, 0x0A);
                d->subclass   = (u8)(classreg & 0xFF);
                d->class_code = (u8)(classreg >> 8);
                d->prog_if    = (u8)(pci_config_read16((u8)bus, slot, func, 0x08) >> 8);
                d->bar4       = pci_config_read32((u8)bus, slot, func, 0x20);

                if (func == 0) {
                    u16 hdrtype = pci_config_read16((u8)bus, slot, func, 0x0E);
                    if (!(hdrtype & 0x0080)) break;
                }
            }
        }
    }
}

int pci_device_count(void) { return dev_count; }
const pci_device_t *pci_device_at(int i) {
    if (i < 0 || i >= dev_count) return NULL;
    return &devs[i];
}

int pci_find_uhci(pci_device_t *out) {
    for (int i = 0; i < dev_count; i++) {
        if (devs[i].class_code == 0x0C && devs[i].subclass == 0x03 && devs[i].prog_if == 0x00) {
            if (out) *out = devs[i];
            return 1;
        }
    }
    return 0;
}
