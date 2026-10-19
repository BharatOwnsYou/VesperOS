# VesperOS networking (hespernetd + HesperNet stack)

## Boot behaviour
`hespernetd` is registered as a service with `auto_start`, so SysVenus starts it during boot:
it probes the PCI bus for network controllers, binds a driver to each one, waits for link,
and runs DHCP (up to 8 s at boot; it keeps trying in the background afterwards).

    [  ok  ] hespernetd started (network manager)
    [  ok  ] eth0: rtl8168 xx:xx:xx:xx:xx:xx  192.168.1.23/24 gw 192.168.1.1 (DHCP)

Manual control (OpenRC style):

    venus smite hespernetd      start it (re-probes drivers if no NIC was found earlier)
    venus stop hespernetd       stop it (DHCP interfaces are deconfigured)
    venus list                  shows it under "services"

## Commands
    ifconfig [iface]            addresses, link, driver, PCI location, counters
    lsnet                       every PCI network controller and whether a driver bound (or why not)
    ping <host|ip> [-c N]       ICMP echo; ESC aborts
    nslookup <host>             DNS A lookup via the DHCP-provided server
    hespernetctl status|log|restart [if]|static <if> <ip> <mask> [gw] [dns]

## Layout
    net/netif.[ch]      interface registry, RX entry point (netif_rx), shared TX buffer
    net/arp.c ipv4.c icmp.c udp.c dhcp.c dns.c    protocol layers
    net/hespernetd.[ch] the daemon: link tracking, DHCP state machine, static config, event log
    net/netcmd.[ch]     shell commands
    net/nettime.c       millisecond clock (PIT channel 0, polled)
    drivers/netdrv.[ch] PCI probe table shared by the NIC drivers
    drivers/rtl8168.c   Realtek RTL8111/8168/8211/8411 PCIe GbE (10ec:8168, 10ec:8161)
    drivers/e1000.c     Intel 8254x PRO/1000 (QEMU default, VirtualBox, VMware "E1000"); 82574L experimental
    drivers/virtio_net.c  virtio-net, legacy PCI transport (QEMU/KVM)
    drivers/rtl8139.c   Realtek RTL8139 (QEMU `-device rtl8139`)
    tests/nettest.c     host-side tests (make test-net)

## Testing
`make test-net` builds the real stack + daemon on the host against a simulated LAN
(gateway, DHCP server, DNS server) with ASan/UBSan and a 200k-frame fuzz run.
`make run`, `make run-virtio`, `make run-rtl8139` boot the ISO in QEMU with user-mode networking
(guest 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3). QEMU has no RTL8168 model: that driver can only
be exercised on real hardware.

## Known limits
* IPv4 only. UDP, ICMP, ARP, DHCP, DNS. **No TCP yet** (so no HTTP/sockets API), no IPv6, no IP fragmentation.
* Polled I/O (the kernel has no interrupt handling). Packets are only processed while something polls:
  the shell prompt, ping, nslookup. A long-running full-screen program that never polls will let the RX
  ring overflow, and the lease clock only advances while it is read.
* One device per driver. Not supported: vmxnet3, AMD PCnet, Hyper-V netvsc, virtio 1.0-only devices,
  Wi-Fi, RTL8169/8125 and other Realtek variants.
* On the x86_64 kernel, NIC/USB MMIO BARs above 4 GiB are reachable (`mmio_map`); on the i686 kernel they are not.
* The RTL8168 driver uses the I/O-port BAR0 when firmware assigned one (reachable from a 32-bit kernel
  regardless of where 64-bit MMIO BARs live) and falls back to a 32-bit MMIO BAR2.

## If the RTL8168 does not come up on real hardware
Boot, then send the output of: `lsnet`, `ifconfig`, `hespernetctl log`.
`lsnet` prints a specific reason if the driver refused the device (e.g. register BAR unassigned).
