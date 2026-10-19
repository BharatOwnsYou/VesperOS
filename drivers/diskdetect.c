#include "diskdetect.h"
#include "pci.h"

#define MAX_DISK_CTRLS 8
static disk_ctrl_t ctrls[MAX_DISK_CTRLS];
static int ctrl_count = 0;

void disk_detect_scan(void) {
    ctrl_count = 0;
    int total = pci_device_count();
    for (int i = 0; i < total && ctrl_count < MAX_DISK_CTRLS; i++) {
        const pci_device_t *d = pci_device_at(i);
        if (!d) continue;
        if (d->class_code != 0x01) continue;

        disk_ctrl_kind_t kind;
        if (d->subclass == 0x08 && d->prog_if == 0x02) kind = DISK_CTRL_NVME;
        else if (d->subclass == 0x06 && d->prog_if == 0x01) kind = DISK_CTRL_AHCI;
        else if (d->subclass == 0x01 || d->subclass == 0x05) kind = DISK_CTRL_IDE;
        else if (d->subclass == 0x04) kind = DISK_CTRL_RAID;
        else kind = DISK_CTRL_OTHER;

        disk_ctrl_t *c = &ctrls[ctrl_count++];
        c->kind = kind;
        c->bus = d->bus; c->slot = d->slot; c->func = d->func;
        c->vendor_id = d->vendor_id; c->device_id = d->device_id;
    }
}

int disk_detect_count(void) { return ctrl_count; }

const disk_ctrl_t *disk_detect_at(int i) {
    if (i < 0 || i >= ctrl_count) return NULL;
    return &ctrls[i];
}

const char *disk_ctrl_kind_name(disk_ctrl_kind_t k) {
    switch (k) {
        case DISK_CTRL_NVME:  return "NVMe";
        case DISK_CTRL_AHCI:  return "AHCI/SATA";
        case DISK_CTRL_IDE:   return "IDE/PATA";
        case DISK_CTRL_RAID:  return "RAID";
        default:              return "storage controller (other)";
    }
}
