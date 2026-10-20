# Storage (SATA + NVMe)

Two block drivers sit behind one small layer (`kernel/blockdev.[ch]`):

| driver | file | what it handles |
|---|---|---|
| AHCI (SATA SSD/HDD) | `drivers/ahci.c` | every AHCI controller, every port with a plain SATA disk, 512-byte sectors, LBA48 |
| NVMe (M.2 / PCIe SSD) | `drivers/nvme.c` | the first NVMe controller, namespace 1, 512 or 4096 byte sectors |

Both are polled (no interrupts), one command at a time, and have no hotplug. Drives show up as `sata0`, `sata1`,
`nvme0` ... The boot screen prints a line per driver family, and the debug port (qemu `qemu-debug.log`, COM1)
gets one line per drive with its size and model.

Not supported: ATAPI (SATA optical), IDE/PATA, RAID-mode controllers, NVMe namespaces other than 1, NCQ, MSI.
A controller the driver can't handle is skipped with a line on the debug port, it never aborts boot.

## Shell commands

    disks                        list block devices
    diskread <dev> <lba>         hex dump of the start of a sector (lba 0 also reports the 55AA boot signature)
    diskwrites on|off            unlock / lock writes (LOCKED at every boot)
    diskwrite <dev> <lba> <b> [n]  overwrite n sectors (default 1), sector i = byte (b+i), then read back and compare

Writes are locked until `diskwrites on`. That is deliberate: nothing should be able to overwrite a disk by
accident, and anything that writes (a future installer) must unlock it explicitly after confirming with the user.
`diskwrite` destroys data on the sectors you give it. Don't point it at a disk you care about.

## Testing

    make run-ahci      # QEMU with a 64M scratch disk.img on an emulated SATA controller
    make run-nvme      # same on an emulated NVMe drive
    make test-disk     # automated: i686 + x86_64 kernels x SATA / NVMe / 4K-sector NVMe, checked on the host image

`make test-disk` types the commands through the QEMU monitor and then reads the disk image file directly, so a
write that "verified" inside the OS but never reached the disk would still fail. It needs `qemu-system-x86_64`
and `grub-mkrescue`, and takes a few minutes because every run waits for GRUB and the boot.

Only emulated hardware has been tested so far. Real SATA/NVMe drives behave differently (link training time,
controllers that need a BIOS handoff, odd MDTS values), so try reads first (`disks`, `diskread`) before any write.
