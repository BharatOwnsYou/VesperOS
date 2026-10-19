#ifndef VESPEROS_NETDRV_H
#define VESPEROS_NETDRV_H
#include "../include/types.h"
#include "../include/io.h"
#include "pci.h"
#include "../net/netif.h"
#include "../net/netutil.h"

/* Full memory barrier (also orders against device DMA) / compiler barrier. */
#if defined(__x86_64__)
static inline void net_mb(void)       { __asm__ volatile("lock; addl $0,(%%rsp)" ::: "memory"); }
#else
static inline void net_mb(void)       { __asm__ volatile("lock; addl $0,(%%esp)" ::: "memory"); }
#endif
static inline void net_cbarrier(void) { __asm__ volatile("" ::: "memory"); }
static inline void net_udelay(u32 us) { while (us--) io_wait(); }   /* ~1 us per port 0x80 write */

/* PCI helpers shared by all NIC drivers */
u32  netdrv_bar(const pci_device_t *d, int idx);
void netdrv_pci_enable(const pci_device_t *d, int io, int mem);      /* also sets bus mastering on the device AND every upstream bridge */
int  netdrv_upstream_bridges(const pci_device_t *d, pci_device_t *out, int max);   /* nearest first */
void netdrv_attach_pci(netif_t *nif, const pci_device_t *d);
void netdrv_fail(const char *reason);   /* a probe that returns -1 calls this first */

/* Probe table: every driver has int xxx_probe(const pci_device_t*):
 *   1 = claimed and registered a netif, 0 = not my device, -1 = my device but init failed. */
typedef struct {
    pci_device_t dev;
    int  status;                 /* 1 bound, 0 unsupported, -1 init failed */
    char driver[16];
    char note[56];               /* failure reason or product name */
} netdrv_dev_t;

int  netdrv_probe_all(void);     /* scans PCI for network controllers; returns #newly bound */
int  netdrv_dev_count(void);
const netdrv_dev_t *netdrv_dev_at(int i);

#endif
