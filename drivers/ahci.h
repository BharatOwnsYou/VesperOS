#ifndef VESPEROS_AHCI_H
#define VESPEROS_AHCI_H

/* AHCI (SATA) driver. finds every AHCI controller diskdetect saw, brings up each port with a plain
 * SATA disk on it and registers it as a blkdev ("sata0", "sata1" ...). returns how many drives it found.
 * polled, one command slot, no NCQ, no hotplug, no ATAPI, 512-byte sectors and LBA48 only.
 * call after pci_scan() + disk_detect_scan() */
int ahci_init(void);

#endif
