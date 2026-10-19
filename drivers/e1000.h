#ifndef VESPEROS_E1000_H
#define VESPEROS_E1000_H
#include "pci.h"
/* Intel 8254x PRO/1000: qemu default (82540EM), virtualbox "PRO/1000 MT Desktop", vmware E1000.
 * also 82574L (e1000e), experimental. one instance */
int e1000_probe(const pci_device_t *d);
#endif
