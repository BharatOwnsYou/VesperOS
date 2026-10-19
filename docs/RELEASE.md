# VesperOS 4 "Nebula" - kernel 0.10

The identity lives in `include/version.h` (name, version, kernel version, codename, hostname). Build artefacts are
`vesperos4.iso`, `vesperos4-x86_64.bin` and `vesperos4-i686.bin`.

## What is new since VesperOS 3 (kernel 0.0.9 "Shooting-star")

* **x86_64 kernel.** A 64-bit build alongside the proven 32-bit one, selectable from the GRUB menu of one ISO
  (see `X86_64.md`). Exception handlers print a register dump instead of silently rebooting.
* **Networking.** A from-scratch IPv4 stack (Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS), the `hespernetd` network manager
  and drivers for Realtek RTL8111/8168/8211/8411, Intel e1000, virtio-net and RTL8139 (see `NETWORKING.md`).
  Shell commands: `ifconfig`, `lsnet`, `ping`, `nslookup`, `dhcp`, `hespernetctl`.
* **Framebuffer console no longer "wipes".** The text grid is kept in RAM and painted with wide stores; the framebuffer
  is never read back. Scrolling and `clear` used to sweep down the screen on UEFI because they read video memory.
* **xHCI structures are spec-aligned** (4 KiB-page aligned, so none crosses a page or a 64 KiB boundary in any build),
  which fixes lost events on some layouts, and a boot-time diagnosis is printed if a USB keyboard fails to enumerate.
* 64-bit-clean drivers (DMA addresses, BARs above 4 GiB through `mmio_map`), service-manager fixes, test suites:
  `make test` runs the network, architecture, renderer and image-layout tests.
