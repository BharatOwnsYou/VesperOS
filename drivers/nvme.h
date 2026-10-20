#ifndef VESPEROS_NVME_H
#define VESPEROS_NVME_H

/* NVMe (M.2 / PCIe SSD) driver. finds every NVMe controller diskdetect saw and registers namespace 1
 * of each one as a blkdev ("nvme0", "nvme1" ...). returns how many it found.
 * polled, one admin queue + one I/O queue (16 entries), one command at a time, no interrupts,
 * 4K controller pages, 512 or 4096 byte logical sectors, namespace 1 only.
 * call after pci_scan() + disk_detect_scan() */
int nvme_init(void);

#endif
