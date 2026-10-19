#ifndef VESPEROS_VIRTIO_NET_H
#define VESPEROS_VIRTIO_NET_H
#include "pci.h"
/* virtio-net over the legacy (transitional) PCI io transport: qemu/kvm -device virtio-net-pci etc.
 * modern-only (virtio 1.0, 0x1041) devices are NOT supported. one instance */
int virtio_net_probe(const pci_device_t *d);
#endif
