#include "netcmd.h"
#include "netif.h"
#include "netutil.h"
#include "hespernetd.h"
#include "icmp.h"
#include "dns.h"
#include "arp.h"
#include "../kernel/vga.h"
#include "../kernel/string.h"
#include "../kernel/athenalibc.h"
#include "../drivers/hid.h"
#include "../drivers/usb_uhci.h"
#include "../drivers/xhci.h"
#include "../drivers/netdrv.h"
#include "../drivers/rtl8168.h"

const char NETCMD_HELP[] =
    "  ifconfig [iface]           show network interfaces and addresses\n"
    "  lsnet                      list network controllers and driver binding\n"
    "  ping <host|ip> [-c N]      ICMP echo (ESC aborts)\n"
    "  nslookup <host>            resolve a name via the DHCP-provided DNS\n"
    "  dhcp [iface]               restart DHCP\n"
    "  hespernetctl <cmd>         status | nic | log | restart [if] | static <if> <ip> <mask> [gw] [dns]\n"
    "  venus smite hespernetd     start the network manager (enabled at boot)\n";

static void puts_(const char *s) { vga_puts(s); }
static void putsc(const char *s, u8 fg) { vga_puts_col(s, fg, VGA_BLACK); }

static void hex2(u8 v, char *out) {
    static const char h[] = "0123456789abcdef";
    out[0] = h[v >> 4]; out[1] = h[v & 15];
}
static void pci_loc(const netif_t *n, char *out) {      /* "03:00.0" */
    hex2(n->pci_bus, out); out[2] = ':';
    hex2(n->pci_slot, out + 3); out[5] = '.';
    out[6] = (char)('0' + (n->pci_func & 7)); out[7] = 0;
}
static void pci_loc_dev(const pci_device_t *d, char *out) {
    hex2(d->bus, out); out[2] = ':';
    hex2(d->slot, out + 3); out[5] = '.';
    out[6] = (char)('0' + (d->func & 7)); out[7] = 0;
}

void netcmd_pump(void) {
    hid_poll_all();
    usb_uhci_poll();
    xhci_poll();
    hespernetd_poll();
}

/* true if the user pressed ESC; drains other keys so they don't leak into the prompt */
static bool esc_pressed(void) {
    char c;
    while ((c = hid_getchar()) != 0) if (c == 27) return true;
    return false;
}

static int parse_uint(const char *s, u32 *out) {
    if (!*s) return 0;
    u32 v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (u32)(*s - '0');
        if (v > 100000) return 0;
    }
    *out = v;
    return 1;
}

static int mask_prefix(u32 mask) {
    int n = 0;
    while (mask & 0x80000000u) { n++; mask <<= 1; }
    return n;
}

/* ------------------------------------------------------------ ifconfig */
static void print_iface(const netif_t *n) {
    char line[160], a[16], b[16], c[16], d[16], mac[18], loc[8];
    mac_to_str(n->mac, mac);
    putsc(n->name, VGA_LIGHT_CYAN);
    k_snprintf(line, sizeof(line), ": link %s   driver %s", n->link_up ? "UP" : "DOWN", n->driver);
    puts_(line);
    if (n->has_pci) {
        pci_loc(n, loc);
        k_snprintf(line, sizeof(line), "   pci %s [%x:%x]", loc, (u32)n->pci_vendor, (u32)n->pci_device);
        puts_(line);
    }
    puts_("\n");
    k_snprintf(line, sizeof(line), "      %s\n", n->desc);
    puts_(line);
    k_snprintf(line, sizeof(line), "      ether %s   state: %s", mac, hespernetd_state_name(n));
    puts_(line);
    u32 left = hespernetd_lease_remaining_s(n);
    if (left) { k_snprintf(line, sizeof(line), "  (lease %us left)", left); puts_(line); }
    puts_("\n");
    if (n->configured) {
        ip_to_str(n->ip, a); ip_to_str(n->mask, b);
        k_snprintf(line, sizeof(line), "      inet %s/%d  netmask %s", a, mask_prefix(n->mask), b);
        puts_(line);
        if (n->gw)  { ip_to_str(n->gw, c);  k_snprintf(line, sizeof(line), "  gateway %s", c); puts_(line); }
        if (n->dns) { ip_to_str(n->dns, d); k_snprintf(line, sizeof(line), "  dns %s", d); puts_(line); }
        puts_("\n");
    } else {
        puts_("      inet (not configured)\n");
    }
    k_snprintf(line, sizeof(line),
               "      RX %u pkts %u bytes %u err %u drop   TX %u pkts %u bytes %u err\n",
               n->rx_packets, n->rx_bytes, n->rx_errors, n->rx_dropped,
               n->tx_packets, n->tx_bytes, n->tx_errors);
    puts_(line);
}

static void cmd_ifconfig(int argc, char **argv) {
    if (netif_count() == 0) {
        puts_("no network interfaces (no supported NIC found - see 'lsnet')\n");
        return;
    }
    if (argc > 1) {
        netif_t *n = netif_by_name(argv[1]);
        if (!n) { puts_("ifconfig: no such interface\n"); return; }
        print_iface(n);
        return;
    }
    for (int i = 0; i < netif_count(); i++) print_iface(netif_at(i));
}

/* --------------------------------------------------------------- lsnet */
static void cmd_lsnet(void) {
    int n = netdrv_dev_count();
    if (n == 0) { puts_("no PCI network controllers found\n"); return; }
    for (int i = 0; i < n; i++) {
        const netdrv_dev_t *r = netdrv_dev_at(i);
        char line[160], loc[8];
        pci_loc_dev(&r->dev, loc);
        k_snprintf(line, sizeof(line), "%s [%x:%x] ", loc, (u32)r->dev.vendor_id, (u32)r->dev.device_id);
        puts_(line);
        if (r->status == 1) {
            k_snprintf(line, sizeof(line), "driver %s: bound\n", r->driver);
            putsc(line, VGA_LIGHT_GREEN);
        } else if (r->status == -1) {
            k_snprintf(line, sizeof(line), "driver %s: FAILED - %s\n", r->driver, r->note);
            putsc(line, VGA_LIGHT_RED);
        } else {
            k_snprintf(line, sizeof(line), "%s\n", r->note);
            putsc(line, VGA_YELLOW);
        }
    }
}

/* ---------------------------------------------------------------- ping */
static int resolve_host(const char *host, u32 *ip) {
    int r = dns_resolve(host, ip);
    if (r == 1) return 1;
    switch (r) {
        case -1: puts_("cannot resolve: no DNS server (is an interface up? try 'ifconfig')\n"); break;
        case -2: puts_("cannot resolve: name does not exist\n"); break;
        case -3: puts_("cannot resolve: no IPv4 address for that name\n"); break;
        default: puts_("cannot resolve: DNS server did not answer\n"); break;
    }
    return 0;
}

static void cmd_ping(int argc, char **argv) {
    u32 count = 4;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (k_streq(argv[i], "-c") && i + 1 < argc) {
            if (!parse_uint(argv[++i], &count) || count == 0) { puts_("ping: bad count\n"); return; }
        } else host = argv[i];
    }
    if (!host) { puts_("usage: ping <host|ip> [-c count]\n"); return; }
    if (!hespernetd_running()) { puts_("ping: hespernetd is not running (venus smite hespernetd)\n"); return; }

    u32 ip;
    if (!resolve_host(host, &ip)) return;

    char line[160], ipstr[16];
    ip_to_str(ip, ipstr);
    k_snprintf(line, sizeof(line), "PING %s (%s) 56 data bytes  (ESC to stop)\n", host, ipstr);
    puts_(line);

    u32 sent = 0, got = 0;
    for (u32 seq = 1; seq <= count; seq++) {
        u32 rtt = 0; u8 ttl = 0;
        sent++;
        int r = icmp_ping(ip, (u16)seq, 56, 2000, &rtt, &ttl);
        if (r == 1) {
            got++;
            if (rtt == 0) k_snprintf(line, sizeof(line), "64 bytes from %s: seq=%u ttl=%u time<1 ms\n", ipstr, seq, (u32)ttl);
            else          k_snprintf(line, sizeof(line), "64 bytes from %s: seq=%u ttl=%u time=%u ms\n", ipstr, seq, (u32)ttl, rtt);
            puts_(line);
        } else if (r == 0) {
            k_snprintf(line, sizeof(line), "seq=%u: request timed out\n", seq);
            puts_(line);
        } else {
            const char *why = r == -1 ? "network unreachable (no route / link down)"
                            : r == -2 ? "host unreachable (ARP got no answer)" : "send failed";
            k_snprintf(line, sizeof(line), "seq=%u: %s\n", seq, why);
            puts_(line);
        }
        if (seq == count) break;
        u32 t0 = net_time_ms();                                  /* 1 s between probes, still servicing the NIC */
        bool stop = false;
        while ((u32)(net_time_ms() - t0) < 1000) {
            netcmd_pump();
            if (esc_pressed()) { stop = true; break; }
        }
        if (stop) break;
    }
    k_snprintf(line, sizeof(line), "--- %s: %u transmitted, %u received, %u%% loss ---\n",
               host, sent, got, sent ? ((sent - got) * 100) / sent : 0);
    puts_(line);
}

static void cmd_nslookup(int argc, char **argv) {
    if (argc < 2) { puts_("usage: nslookup <host>\n"); return; }
    if (!hespernetd_running()) { puts_("nslookup: hespernetd is not running\n"); return; }
    u32 ip;
    char line[120], a[16], d[16] = "?";
    for (int i = 0; i < netif_count(); i++) {
        netif_t *n = netif_at(i);
        if (n->configured && n->dns) { ip_to_str(n->dns, d); break; }
    }
    if (!resolve_host(argv[1], &ip)) return;
    ip_to_str(ip, a);
    k_snprintf(line, sizeof(line), "Server:  %s\nName:    %s\nAddress: %s\n", d, argv[1], a);
    puts_(line);
}

/* -------------------------------------------------------- hespernetctl */
static void ctl_status(void) {
    putsc("hespernetd ", VGA_LIGHT_MAGENTA);
    puts_(hespernetd_running() ? "is running\n" : "is stopped\n");
    for (int i = 0; i < netif_count(); i++) {
        netif_t *n = netif_at(i);
        char line[120];
        k_snprintf(line, sizeof(line), "  %s  %s  link %s  %s\n", n->name, n->driver,
                   n->link_up ? "up" : "down", hespernetd_state_name(n));
        puts_(line);
    }
    if (netif_count() == 0) puts_("  (no interfaces)\n");
}


/* Real-hardware bring-up dump: PCI command registers along the DMA path, then chip registers. */
static void ctl_nic(void) {
    char line[200], loc[8];
    for (int i = 0; i < netdrv_dev_count(); i++) {
        const netdrv_dev_t *r = netdrv_dev_at(i);
        pci_loc_dev(&r->dev, loc);
        const pci_device_t *d = &r->dev;
        k_snprintf(line, sizeof(line), "NIC %s [%x:%x] driver '%s' status %d\n", loc,
                   (u32)d->vendor_id, (u32)d->device_id, r->driver, r->status);
        puts_(line);
        k_snprintf(line, sizeof(line), "  pci cmd=%x  bar0=%x bar2=%x bar4=%x\n",
                   (u32)pci_config_read16(d->bus, d->slot, d->func, 0x04),
                   pci_config_read32(d->bus, d->slot, d->func, 0x10),
                   pci_config_read32(d->bus, d->slot, d->func, 0x18),
                   pci_config_read32(d->bus, d->slot, d->func, 0x20));
        puts_(line);
        pci_device_t br[6];
        int nb = netdrv_upstream_bridges(d, br, 6);
        if (nb == 0) puts_("  upstream bridges: none found (device on root bus)\n");
        for (int b = 0; b < nb; b++) {
            pci_loc_dev(&br[b], loc);
            k_snprintf(line, sizeof(line), "  bridge %s [%x:%x] cmd=%x (needs bit2=bus master)\n", loc,
                       (u32)br[b].vendor_id, (u32)br[b].device_id,
                       (u32)pci_config_read16(br[b].bus, br[b].slot, br[b].func, 0x04));
            puts_(line);
        }
    }
    rtl8168_diag_t g;
    if (rtl8168_diag(&g)) {
        k_snprintf(line, sizeof(line), "  rtl8168: %s base=%x xid=%x CR=%x TCR=%x RCR=%x MISC=%x\n",
                   g.use_io ? "io-ports" : "mmio", g.base, g.xid, (u32)g.cr, g.tcr, g.rcr, g.misc);
        puts_(line);
        k_snprintf(line, sizeof(line), "  rtl8168: CR after init=%x, last enable read back=%x, TX/RX re-enabled %u times (2 = init only)\n",
                   (u32)g.cr_after_init, (u32)g.cr_readback, g.cr_fixups);
        puts_(line);
        k_snprintf(line, sizeof(line), "  rtl8168: PHYstatus=%x CPlus=%x ISR=%x  tx_cur=%u rx_cur=%u kicks=%u\n",
                   (u32)g.phystatus, (u32)g.cplus, (u32)g.isr, (u32)g.tx_cur, (u32)g.rx_cur, g.tx_kicks);
        puts_(line);
        k_snprintf(line, sizeof(line), "  rtl8168: last TX desc opts1=%x (bit31=OWN: still set => NIC never fetched it)  RX head opts1=%x\n",
                   g.tx_last_opts1, g.rx_head_opts1);
        puts_(line);
    }
    for (int i = 0; i < netif_count(); i++) {
        const netif_t *n = netif_at(i);
        k_snprintf(line, sizeof(line), "  %s: RX %u pkts (%u err, %u drop)  TX %u pkts (%u err)\n", n->name,
                   n->rx_packets, n->rx_errors, n->rx_dropped, n->tx_packets, n->tx_errors);
        puts_(line);
    }
}

static void cmd_hespernetctl(int argc, char **argv) {
    if (argc < 2) {
        puts_("usage: hespernetctl <status|nic|log|restart [if]|static <if> <ip> <mask> [gw] [dns]>\n");
        return;
    }
    const char *sub = argv[1];
    if (k_streq(sub, "status")) { ctl_status(); return; }
    if (k_streq(sub, "nic")) { ctl_nic(); return; }
    if (k_streq(sub, "log")) {
        int n = hespernetd_log_count();
        if (!n) puts_("(log empty)\n");
        for (int i = 0; i < n; i++) { puts_(hespernetd_log_line(i)); puts_("\n"); }
        return;
    }
    if (!hespernetd_running()) { puts_("hespernetctl: hespernetd is not running (venus smite hespernetd)\n"); return; }
    if (k_streq(sub, "restart")) {
        netif_t *n = NULL;
        if (argc > 2) { n = netif_by_name(argv[2]); if (!n) { puts_("no such interface\n"); return; } }
        hespernetd_dhcp_restart(n);
        puts_("DHCP restarted\n");
        return;
    }
    if (k_streq(sub, "static")) {
        u32 ip, mask, gw = 0, dns = 0;
        if (argc < 5 || !ip_parse(argv[3], &ip) || !ip_parse(argv[4], &mask)
            || (argc > 5 && !ip_parse(argv[5], &gw)) || (argc > 6 && !ip_parse(argv[6], &dns))) {
            puts_("usage: hespernetctl static <if> <ip> <netmask> [gateway] [dns]\n");
            return;
        }
        netif_t *n = netif_by_name(argv[2]);
        if (!n) { puts_("no such interface\n"); return; }
        if (hespernetd_set_static(n, ip, mask, gw, dns) == 0) puts_("static configuration applied\n");
        else puts_("failed\n");
        return;
    }
    puts_("hespernetctl: unknown command\n");
}

int netcmd_dispatch(int argc, char **argv) {
    if (k_streq(argv[0], "ifconfig"))     { cmd_ifconfig(argc, argv);     return 1; }
    if (k_streq(argv[0], "lsnet"))        { cmd_lsnet();                  return 1; }
    if (k_streq(argv[0], "ping"))         { cmd_ping(argc, argv);         return 1; }
    if (k_streq(argv[0], "nslookup"))     { cmd_nslookup(argc, argv);     return 1; }
    if (k_streq(argv[0], "hespernetctl")) { cmd_hespernetctl(argc, argv); return 1; }
    if (k_streq(argv[0], "dhcp")) {                        /* shorthand: restart DHCP (optionally on one interface) */
        char *av[3] = { "hespernetctl", "restart", argc > 1 ? argv[1] : NULL };
        cmd_hespernetctl(argc > 1 ? 3 : 2, av);
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------- boot report */
static u32 notice_seen = 0;

void netcmd_notice_sync(void) { notice_seen = hespernetd_bind_seq(); }

void netcmd_async_notice(void) {
    u32 seq = hespernetd_bind_seq();
    if (seq == notice_seen) return;
    notice_seen = seq;
    const netif_t *n = hespernetd_last_bound();
    if (!n || !n->configured) return;
    char line[120], a[16];
    ip_to_str(n->ip, a);
    k_snprintf(line, sizeof(line), "\n[ net ] %s is now up: %s (DHCP). Type 'ifconfig' for details.\n", n->name, a);
    vga_puts_col(line, VGA_LIGHT_GREEN, VGA_BLACK);
}

/* short diagnosis for an interface with no address after the boot wait. shown on the boot screen so it
 * can be photographed even when the keyboard doesn't work */
static void boot_nic_diag(const netif_t *n) {
    char line[140];
    k_snprintf(line, sizeof(line), "        %s diag: link=%u state=%s rx=%u tx=%u rxerr=%u txerr=%u  t=%ums\n",
               n->name, (u32)n->link_up, hespernetd_state_name(n), n->rx_packets, n->tx_packets,
               n->rx_errors, n->tx_errors, net_time_ms());
    putsc(line, VGA_YELLOW);
    rtl8168_diag_t g;
    if (rtl8168_diag(&g)) {
        k_snprintf(line, sizeof(line), "        rtl8168: PHYstatus=%x CR=%x TCR=%x RCR=%x MISC=%x ISR=%x kicks=%u\n",
                   (u32)g.phystatus, (u32)g.cr, g.tcr, g.rcr, g.misc, (u32)g.isr, g.tx_kicks);
        putsc(line, VGA_YELLOW);
        k_snprintf(line, sizeof(line), "        rtl8168: CR after init=%x (re-enabled %u times)  last TX opts1=%x RX opts1=%x\n",
                   (u32)g.cr_after_init, g.cr_fixups, g.tx_last_opts1, g.rx_head_opts1);
        putsc(line, VGA_YELLOW);
    }
    int total = hespernetd_log_count();
    for (int i = total > 3 ? total - 3 : 0; i < total; i++) {
        puts_("        log: "); puts_(hespernetd_log_line(i)); puts_("\n");
    }
}

void netcmd_boot_report(bool verbose) {
    int n = netif_count();
    if (n == 0) {
        if (verbose) {
            vga_puts_col("[ warn ] ", VGA_YELLOW, VGA_BLACK);
            puts_("hespernetd: no supported network adapter found (see 'lsnet')\n");
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        netif_t *nif = netif_at(i);
        char line[160], mac[18], a[16], b[16];
        mac_to_str(nif->mac, mac);
        bool ok = nif->configured;
        if (!verbose && ok) continue;
        vga_puts_col(ok ? "[  ok  ] " : "[ warn ] ", ok ? VGA_LIGHT_GREEN : VGA_YELLOW, VGA_BLACK);
        if (ok) {
            ip_to_str(nif->ip, a); ip_to_str(nif->gw, b);
            k_snprintf(line, sizeof(line), "%s: %s %s  %s/%d gw %s (%s)\n", nif->name, nif->driver, mac, a,
                       mask_prefix(nif->mask), nif->gw ? b : "none",
                       hespernetd_state(nif) == NS_STATIC ? "static" : "DHCP");
        } else if (!nif->link_up) {
            k_snprintf(line, sizeof(line), "%s: %s %s  no link yet - hespernetd keeps watching (a notice appears when it is up)\n",
                       nif->name, nif->driver, mac);
        } else {
            k_snprintf(line, sizeof(line), "%s: %s %s  link up, no DHCP answer yet - hespernetd keeps trying\n",
                       nif->name, nif->driver, mac);
        }
        puts_(line);
        if (!ok) boot_nic_diag(nif);              /* shown even when the boot log is otherwise quiet */
    }
}
