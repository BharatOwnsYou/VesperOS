#include "netdrv.h"
#include "rtl8168.h"
#include "rtl8139.h"
#include "e1000.h"
#include "virtio_net.h"
#include "../kernel/string.h"

#define NETDRV_MAX 8
static netdrv_dev_t devs[NETDRV_MAX];
static int ndevs = 0;
static char last_fail[56];

u32 netdrv_bar(const pci_device_t *d, int idx) {
    return pci_config_read32(d->bus, d->slot, d->func, (u8)(0x10 + 4 * idx));
}

/* walks up the PCI tree from d, fills out[] with every PCI-PCI bridge to the root, nearest first.
 * PCIe DMA goes upstream so each bridge needs Bus Master Enable or it silently drops it.
 * linux does this in pci_set_master(), firmware might not */
int netdrv_upstream_bridges(const pci_device_t *d, pci_device_t *out, int max) {
    int n = 0;
    u8 bus = d->bus;
    while (bus != 0 && n < max) {
        const pci_device_t *found = NULL;
        for (int i = 0; i < pci_device_count(); i++) {
            const pci_device_t *b = pci_device_at(i);
            if (b->class_code != 0x06 || b->subclass != 0x04) continue;      /* PCI-PCI bridge */
            u32 busreg = pci_config_read32(b->bus, b->slot, b->func, 0x18);   /* pri | sec | sub */
            if (((busreg >> 8) & 0xFF) == bus) { found = b; break; }
        }
        if (!found) break;
        out[n++] = *found;
        bus = found->bus;
    }
    return n;
}

void netdrv_pci_enable(const pci_device_t *d, int io, int mem) {
    pci_device_t br[6];
    int nb = netdrv_upstream_bridges(d, br, 6);
    for (int i = nb - 1; i >= 0; i--) {                 /* root port first, then down to the device */
        u16 c = pci_config_read16(br[i].bus, br[i].slot, br[i].func, 0x04);
        u16 want = (u16)(c | 0x0007);                   /* I/O + memory decode + bus master */
        if (want != c) pci_config_write16(br[i].bus, br[i].slot, br[i].func, 0x04, want);
    }
    u16 cmd = pci_config_read16(d->bus, d->slot, d->func, 0x04);
    if (io)  cmd |= 0x0001;
    if (mem) cmd |= 0x0002;
    cmd |= 0x0004;                 /* bus master: the NIC DMAs descriptors and packets */
    cmd &= (u16)~0x0400;           /* never mask the INTx line */
    pci_config_write16(d->bus, d->slot, d->func, 0x04, cmd);
}

void netdrv_attach_pci(netif_t *nif, const pci_device_t *d) {
    nif->has_pci = true;
    nif->pci_bus = d->bus; nif->pci_slot = d->slot; nif->pci_func = d->func;
    nif->pci_vendor = d->vendor_id; nif->pci_device = d->device_id;
}

void netdrv_fail(const char *reason) { net_strlcpy(last_fail, reason, sizeof(last_fail)); }

int netdrv_dev_count(void) { return ndevs; }
const netdrv_dev_t *netdrv_dev_at(int i) { return (i >= 0 && i < ndevs) ? &devs[i] : NULL; }

static netdrv_dev_t *find_rec(const pci_device_t *d) {
    for (int i = 0; i < ndevs; i++)
        if (devs[i].dev.bus == d->bus && devs[i].dev.slot == d->slot && devs[i].dev.func == d->func)
            return &devs[i];
    return NULL;
}

int netdrv_probe_all(void) {
    int bound_now = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *d = pci_device_at(i);
        if (d->class_code != 0x02) continue;                /* network controllers only */

        netdrv_dev_t *rec = find_rec(d);
        if (rec && rec->status == 1) continue;             /* already bound by an earlier probe */
        if (!rec) {
            if (ndevs >= NETDRV_MAX) continue;
            rec = &devs[ndevs++];
            k_memset(rec, 0, sizeof(*rec));
            rec->dev = *d;
        }

        last_fail[0] = 0;
        const char *drv = "";
        int r = rtl8168_probe(d);   if (r) drv = "rtl8168";
        if (!r) { r = rtl8139_probe(d);     if (r) drv = "rtl8139"; }
        if (!r) { r = e1000_probe(d);       if (r) drv = "e1000"; }
        if (!r) { r = virtio_net_probe(d);  if (r) drv = "virtio-net"; }

        net_strlcpy(rec->driver, drv, sizeof(rec->driver));
        rec->status = r;
        if (r == 1) { bound_now++; rec->note[0] = 0; }
        else if (r == -1) net_strlcpy(rec->note, last_fail[0] ? last_fail : "init failed", sizeof(rec->note));
        else net_strlcpy(rec->note, d->subclass == 0x80 ? "no driver (wireless/other)" : "no driver for this chip", sizeof(rec->note));
    }
    return bound_now;
}
