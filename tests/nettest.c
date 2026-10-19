/* host-side tests for the net stack + hespernetd.
 *
 * builds the REAL net sources (not copies) against a fake NIC on a simulated LAN: a gateway that answers
 * ARP/ICMP, a DHCP server, a DNS server. the "world" side checks every checksum with its own independent
 * implementation so a bug in the stack's checksum code can't hide itself.
 * build/run: make test-net   (-fsanitize=address,undefined)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../net/netif.h"
#include "../net/netutil.h"
#include "../net/arp.h"
#include "../net/ipv4.h"
#include "../net/icmp.h"
#include "../net/udp.h"
#include "../net/dhcp.h"
#include "../net/dns.h"
#include "../net/hespernetd.h"

/* ---------------------------------------------------------- test plumbing */
static int n_pass = 0, n_fail = 0;
static void check(int ok, const char *what) {
    if (ok) n_pass++; else { n_fail++; printf("  FAIL: %s\n", what); }
}
#define CHECK(c) check((c), #c)
#define CHECKM(c, m) check((c), m)

/* fake clock: each read advances 1 ms so every wait loop terminates */
static u32 fake_ms = 100000;
u32 net_time_ms(void) { return fake_ms++; }
void net_delay_ms(u32 ms) { fake_ms += ms; }

/* independent checksum (deliberately not shared with the stack) */
static u16 ref_csum(const u8 *d, size_t n, u32 init) {
    u32 s = init;
    for (size_t i = 0; i + 1 < n; i += 2) s += (u32)((d[i] << 8) | d[i + 1]);
    if (n & 1) s += (u32)(d[n - 1] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}
static u32 ref_pseudo(u32 sip, u32 dip, u8 proto, u16 len) {
    u32 s = 0;
    s += (sip >> 16) + (sip & 0xFFFF);
    s += (dip >> 16) + (dip & 0xFFFF);
    s += proto; s += len;
    return s;
}

/* ------------------------------------------------------------- the world */
static const u8 NIC_MAC[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const u8 GW_MAC[6]  = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };
static const u8 DNS_MAC[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x03 };
static const u8 BC[6]      = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
#define GW_IP     IP4(10,0,2,2)
#define DNS_IP    IP4(10,0,2,3)
#define LEASE_IP  IP4(10,0,2,15)
#define ALT_GW_IP IP4(192,168,5,1)
#define BLACKHOLE IP4(1,2,3,4)

typedef struct { u8 d[1600]; u16 len; } frame_t;
static frame_t rxq[64];
static int rxq_head, rxq_tail;
static netif_t *nic;

static void enqueue(const u8 *f, u16 len) {
    frame_t *s = &rxq[rxq_tail++ % 64];
    memcpy(s->d, f, len); s->len = len;
}
static void fake_poll(netif_t *n) {
    while (rxq_head != rxq_tail) {
        frame_t *f = &rxq[rxq_head++ % 64];
        netif_rx(n, f->d, f->len);
    }
}

static u16 build_udp_frame(u8 *o, const u8 *edst, const u8 *esrc, u32 sip, u32 dip,
                           u16 sport, u16 dport, const u8 *pl, u16 plen, int csum_mode) {
    memcpy(o, edst, 6); memcpy(o + 6, esrc, 6); wr16(o + 12, 0x0800);
    u8 *ip = o + 14;
    u16 udp_len = (u16)(8 + plen), tot = (u16)(20 + udp_len);
    ip[0] = 0x45; ip[1] = 0; wr16(ip + 2, tot); wr16(ip + 4, 1); wr16(ip + 6, 0);
    ip[8] = 64; ip[9] = 17; wr16(ip + 10, 0); wr32(ip + 12, sip); wr32(ip + 16, dip);
    wr16(ip + 10, ref_csum(ip, 20, 0));
    u8 *u = ip + 20;
    wr16(u, sport); wr16(u + 2, dport); wr16(u + 4, udp_len); wr16(u + 6, 0);
    memcpy(u + 8, pl, plen);
    u16 c = ref_csum(u, udp_len, ref_pseudo(sip, dip, 17, udp_len));
    if (c == 0) c = 0xFFFF;
    if (csum_mode == 1) c = 0;
    if (csum_mode == 2) c ^= 0x1234;
    wr16(u + 6, c);
    u16 total = (u16)(14 + tot);
    if (total < 60) { memset(o + total, 0, 60 - total); total = 60; }
    return total;
}

static u16 build_icmp_frame(u8 *o, const u8 *edst, const u8 *esrc, u32 sip, u32 dip,
                            u8 type, u16 id, u16 seq, const u8 *pl, u16 plen,
                            int bad_icmp_csum, int bad_ip_csum) {
    memcpy(o, edst, 6); memcpy(o + 6, esrc, 6); wr16(o + 12, 0x0800);
    u8 *ip = o + 14; u16 icl = (u16)(8 + plen), tot = (u16)(20 + icl);
    ip[0] = 0x45; ip[1] = 0; wr16(ip + 2, tot); wr16(ip + 4, 7); wr16(ip + 6, 0);
    ip[8] = 64; ip[9] = 1; wr16(ip + 10, 0); wr32(ip + 12, sip); wr32(ip + 16, dip);
    wr16(ip + 10, ref_csum(ip, 20, 0) ^ (bad_ip_csum ? 0x00FF : 0));
    u8 *m = ip + 20;
    m[0] = type; m[1] = 0; wr16(m + 2, 0); wr16(m + 4, id); wr16(m + 6, seq);
    memcpy(m + 8, pl, plen);
    wr16(m + 2, ref_csum(m, icl, 0) ^ (bad_icmp_csum ? 0x0F0F : 0));
    u16 total = (u16)(14 + tot);
    if (total < 60) { memset(o + total, 0, 60 - total); total = 60; }
    return total;
}

static int  on_link(u32 ip) { return (ip & 0xFFFFFF00u) == IP4(10,0,2,0); }
static const u8 *mac_for(u32 ip) { return ip == DNS_IP ? DNS_MAC : GW_MAC; }
static int  owns_ip(u32 ip) { return ip == GW_IP || ip == DNS_IP || ip == ALT_GW_IP; }

/* observations the tests assert on */
static struct {
    int  dhcp_discover_seen, dhcp_request_seen;
    u8   disc_eth_dst[6]; u32 disc_ip_src, disc_ip_dst; u16 disc_sport, disc_dport;
    u16  disc_udp_len; u8 disc_chaddr[6]; int disc_magic_ok, disc_opt53;
    u32  req_opt50, req_opt54, req_ciaddr; int req_has50, req_has54;
    int  arp_requests_for_silent;
    int  ping_requests_seen;
    int  have_arp_reply; u8 arp_reply[60];
    int  have_echo_reply; u8 echo_reply[200]; u16 echo_reply_len;
    int  dns_queries;
} obs;

/* world knobs */
static int  dhcp_silent = 0, dhcp_nak_next = 0;
#define SILENT_IP IP4(10,0,2,77)

static void world_dhcp(const u8 *f, const u8 *udp_payload, u16 plen) {
    if (plen < 240) return;
    u32 xid = rd32(udp_payload + 4);
    u32 ciaddr = rd32(udp_payload + 12);
    const u8 *chaddr = udp_payload + 28;
    int type = 0, has50 = 0, has54 = 0; u32 o50 = 0, o54 = 0;
    for (u16 i = 240; i < plen;) {
        u8 c = udp_payload[i++]; if (c == 0) continue; if (c == 255) break;
        u8 l = udp_payload[i++];
        if (c == 53) type = udp_payload[i];
        if (c == 50) { has50 = 1; o50 = rd32(udp_payload + i); }
        if (c == 54) { has54 = 1; o54 = rd32(udp_payload + i); }
        i = (u16)(i + l);
    }
    if (type == 1) {
        obs.dhcp_discover_seen++;
        memcpy(obs.disc_eth_dst, f, 6);
        const u8 *ip = f + 14; const u8 *u = ip + 20;
        obs.disc_ip_src = rd32(ip + 12); obs.disc_ip_dst = rd32(ip + 16);
        obs.disc_sport = rd16(u); obs.disc_dport = rd16(u + 2); obs.disc_udp_len = rd16(u + 4);
        memcpy(obs.disc_chaddr, chaddr, 6);
        obs.disc_magic_ok = memcmp(udp_payload + 236, "\x63\x82\x53\x63", 4) == 0;
        obs.disc_opt53 = type;
    } else if (type == 3) {
        obs.dhcp_request_seen++;
        obs.req_has50 = has50; obs.req_opt50 = o50;
        obs.req_has54 = has54; obs.req_opt54 = o54; obs.req_ciaddr = ciaddr;
    }
    if (dhcp_silent || (type != 1 && type != 3)) return;

    int reply_type = 2; u32 yi = LEASE_IP;
    if (type == 3) {
        if (dhcp_nak_next) { reply_type = 6; yi = 0; dhcp_nak_next = 0; }
        else if (ciaddr) { reply_type = 5; yi = ciaddr; }
        else if (has50 && o50 == LEASE_IP && has54 && o54 == GW_IP) reply_type = 5;
        else { reply_type = 6; yi = 0; }
    }
    u8 b[400]; memset(b, 0, sizeof(b));
    b[0] = 2; b[1] = 1; b[2] = 6; wr32(b + 4, xid); wr32(b + 16, yi); wr32(b + 20, GW_IP);
    memcpy(b + 28, chaddr, 6); memcpy(b + 236, "\x63\x82\x53\x63", 4);
    u8 *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = (u8)reply_type;
    *o++ = 54; *o++ = 4; wr32(o, GW_IP); o += 4;
    if (reply_type != 6) {
        *o++ = 1; *o++ = 4; wr32(o, 0xFFFFFF00u); o += 4;
        *o++ = 3; *o++ = 4; wr32(o, GW_IP); o += 4;
        *o++ = 6; *o++ = 8; wr32(o, DNS_IP); wr32(o + 4, IP4(10,0,2,4)); o += 8;   /* two DNS servers */
        *o++ = 51; *o++ = 4; wr32(o, 86400); o += 4;
    }
    *o++ = 255;
    u8 fr[600]; u16 n;
    if (ciaddr) n = build_udp_frame(fr, NIC_MAC, GW_MAC, GW_IP, ciaddr, 67, 68, b, (u16)(o - b), 0);
    else        n = build_udp_frame(fr, BC, GW_MAC, GW_IP, IP4_BROADCAST, 67, 68, b, (u16)(o - b), 0);
    enqueue(fr, n);
}

static int dns_name(const u8 *q, u16 len, char *out) {          /* decode question name */
    u16 i = 0; int o = 0;
    while (i < len && q[i]) {
        u8 l = q[i++]; if (i + l > len) return 0;
        if (o) out[o++] = '.';
        memcpy(out + o, q + i, l); o += l; i = (u16)(i + l);
    }
    out[o] = 0; return 1;
}

static void world_dns(const u8 *f, const u8 *q, u16 qlen) {
    obs.dns_queries++;
    if (qlen < 17) return;
    char name[256];
    if (!dns_name(q + 12, (u16)(qlen - 12), name)) return;
    u16 id = rd16(q);
    u16 qsec_len = (u16)(strlen(name) + 2 + 4);     /* labels + root + qtype/class */
    u8 r[512]; memset(r, 0, sizeof(r));
    wr16(r, id); wr16(r + 2, 0x8180); wr16(r + 4, 1);
    memcpy(r + 12, q + 12, qsec_len);
    u16 off = (u16)(12 + qsec_len);
    int an = 0;

    if (!strcmp(name, "slow.test")) return;                       /* never answers */
    if (!strcmp(name, "nxdomain.test")) { wr16(r + 2, 0x8183); }
    else if (!strcmp(name, "empty.test")) { /* NOERROR, no answers */ }
    else {
        /* example.com / garbage.test: CNAME (compressed name) + A (name = pointer into CNAME rdata) */
        wr16(r + off, 0xC00C); wr16(r + off + 2, 5); wr16(r + off + 4, 1); wr32(r + off + 6, 60);
        u8 cname[] = { 4,'e','d','g','e',7,'e','x','a','m','p','l','e',3,'n','e','t',0 };
        wr16(r + off + 10, sizeof(cname));
        memcpy(r + off + 12, cname, sizeof(cname));
        u16 cname_off = (u16)(off + 12);
        off = (u16)(off + 12 + sizeof(cname));
        wr16(r + off, (u16)(0xC000 | cname_off)); wr16(r + off + 2, 1); wr16(r + off + 4, 1);
        wr32(r + off + 6, 60); wr16(r + off + 10, 4); wr32(r + off + 12, IP4(93,184,216,34));
        off = (u16)(off + 16); an = 2;
    }
    wr16(r + 6, (u16)an);
    const u8 *ip = f + 14; u32 cip = rd32(ip + 12); u16 cport = rd16(ip + 20);
    u8 fr[700];
    if (!strcmp(name, "garbage.test")) {                           /* wrong ID first: must be ignored */
        u8 bad[512]; memcpy(bad, r, off); wr16(bad, (u16)(id ^ 0x5555));
        enqueue(fr, build_udp_frame(fr, f + 6, DNS_MAC, DNS_IP, cip, 53, cport, bad, off, 0));
    }
    u16 n = build_udp_frame(fr, f + 6, DNS_MAC, DNS_IP, cip, 53, cport, r, off, 0);
    enqueue(fr, n);
}

static void world_ip(const u8 *f, u16 len) {
    const u8 *ip = f + 14;
    CHECKM((ip[0] >> 4) == 4 && (ip[0] & 15) == 5, "tx: IPv4 header 20 bytes");
    CHECKM(ref_csum(ip, 20, 0) == 0, "tx: IPv4 header checksum valid");
    u16 tot = rd16(ip + 2);
    CHECKM(tot + 14 <= len, "tx: IP total length fits frame");
    CHECKM(ip[8] > 0, "tx: TTL nonzero");
    u8 proto = ip[9]; u32 sip = rd32(ip + 12), dip = rd32(ip + 16);
    const u8 *pl = ip + 20; u16 plen = (u16)(tot - 20);

    if (proto == 17) {
        u16 ulen = rd16(pl + 4);
        CHECKM(ulen == plen, "tx: UDP length matches");
        if (rd16(pl + 6) != 0)
            CHECKM(ref_csum(pl, ulen, ref_pseudo(sip, dip, 17, ulen)) == 0, "tx: UDP checksum valid");
        u16 dport = rd16(pl + 2);
        if (dport == 67) world_dhcp(f, pl + 8, (u16)(ulen - 8));
        else if (dport == 53) {
            CHECKM(memcmp(f, DNS_MAC, 6) == 0, "tx: DNS query addressed to resolver MAC");
            world_dns(f, pl + 8, (u16)(ulen - 8));
        }
    } else if (proto == 1) {
        CHECKM(ref_csum(pl, plen, 0) == 0, "tx: ICMP checksum valid");
        if (pl[0] == 8) {                                          /* echo request from the stack */
            obs.ping_requests_seen++;
            const u8 *want = on_link(dip) ? mac_for(dip) : GW_MAC;
            CHECKM(memcmp(f, want, 6) == 0, "tx: ping uses correct next-hop MAC (on-link vs gateway)");
            if (dip == BLACKHOLE) return;
            u8 fr[1600];
            u16 n = build_icmp_frame(fr, f + 6, want, dip, sip, 0, rd16(pl + 4), rd16(pl + 6),
                                     pl + 8, (u16)(plen - 8), 0, 0);
            enqueue(fr, n);
        } else if (pl[0] == 0) {                                   /* the stack answered our request */
            obs.have_echo_reply = 1; obs.echo_reply_len = len;
            memcpy(obs.echo_reply, f, len < sizeof(obs.echo_reply) ? len : sizeof(obs.echo_reply));
        }
    }
}

static void world_arp(const u8 *f, u16 len) {
    CHECKM(len >= 60, "tx: ARP frame padded");
    const u8 *a = f + 14;
    u16 op = rd16(a + 6); u32 tpa = rd32(a + 24), spa = rd32(a + 14);
    if (op == 1) {
        if (owns_ip(tpa)) {
            u8 fr[60]; memset(fr, 0, 60);
            memcpy(fr, a + 8, 6); memcpy(fr + 6, mac_for(tpa), 6); wr16(fr + 12, 0x0806);
            u8 *r = fr + 14; wr16(r, 1); wr16(r + 2, 0x0800); r[4] = 6; r[5] = 4; wr16(r + 6, 2);
            memcpy(r + 8, mac_for(tpa), 6); wr32(r + 14, tpa); memcpy(r + 18, a + 8, 6); wr32(r + 24, spa);
            enqueue(fr, 60);
        } else if (tpa == SILENT_IP) obs.arp_requests_for_silent++;
    } else if (op == 2) {
        obs.have_arp_reply = 1; memcpy(obs.arp_reply, f, 60);
    }
}

static int fake_send(netif_t *n, const u8 *f, u16 len) {
    (void)n;
    CHECKM(len >= 60 && len <= 1514, "tx: frame length within [60,1514]");
    CHECKM(memcmp(f + 6, NIC_MAC, 6) == 0, "tx: source MAC is the NIC's");
    u16 et = rd16(f + 12);
    if (et == 0x0806) world_arp(f, len);
    else if (et == 0x0800) world_ip(f, len);
    return 0;
}

int netdrv_probe_all(void) {
    nic = netif_register("faker", "Fake test NIC", NIC_MAC, fake_send, fake_poll);
    nic->link_up = true;
    return 1;
}

/* ------------------------------------------------------------- helpers */
static void run(int steps) { for (int i = 0; i < steps; i++) hespernetd_poll(); }
static int run_until_state(ns_state_t st, int max_steps) {
    for (int i = 0; i < max_steps; i++) { hespernetd_poll(); if (hespernetd_state(nic) == st) return 1; }
    return 0;
}
static void inject(const u8 *f, u16 len) { enqueue(f, len); run(2); }

static int udp_hits; static u32 udp_last_src; static u16 udp_last_len;
static void test_udp_handler(netif_t *n, u32 src, u16 sport, const u8 *d, u16 len) {
    (void)n; (void)sport; (void)d; udp_hits++; udp_last_src = src; udp_last_len = len;
}

/* ================================================================= tests */
static void test_util(void) {
    printf("[util] checksums, address parsing\n");
    const u8 rfc[] = { 0x00,0x01,0xf2,0x03,0xf4,0xf5,0xf6,0xf7 };
    CHECK(net_checksum(rfc, 8) == 0x220d);                       /* RFC 1071 worked example */
    const u8 hdr[] = { 0x45,0,0,0x73,0,0,0x40,0,0x40,0x11,0,0,0xc0,0xa8,0,1,0xc0,0xa8,0,0xc7 };
    CHECK(net_checksum(hdr, 20) == 0xb861);                      /* well-known IPv4 header example */
    const u8 odd[] = { 0x12, 0x34, 0x56 };
    CHECK(net_checksum(odd, 3) == ref_csum(odd, 3, 0));          /* odd length */

    u32 ip = 0; char s[16];
    CHECK(ip_parse("10.0.2.15", &ip) && ip == IP4(10,0,2,15));
    CHECK(ip_parse("255.255.255.255", &ip) && ip == 0xFFFFFFFFu);
    CHECK(!ip_parse("256.1.1.1", &ip));
    CHECK(!ip_parse("1.2.3", &ip));
    CHECK(!ip_parse("1.2.3.4.5", &ip));
    CHECK(!ip_parse("1..2.3", &ip));
    CHECK(!ip_parse("a.b.c.d", &ip));
    CHECK(!ip_parse("", &ip));
    CHECK(!ip_parse("1.2.3.4 ", &ip));
    CHECK(!ip_parse("1.2.3.1234", &ip));
    ip_to_str(IP4(192,168,1,254), s); CHECK(!strcmp(s, "192.168.1.254"));
    ip_to_str(0, s); CHECK(!strcmp(s, "0.0.0.0"));
    char m[18]; mac_to_str(NIC_MAC, m); CHECK(!strcmp(m, "52:54:00:12:34:56"));
    CHECK(ip_default_mask(IP4(10,1,1,1)) == 0xFF000000u);
    CHECK(ip_default_mask(IP4(172,16,0,1)) == 0xFFFF0000u);
    CHECK(ip_default_mask(IP4(192,168,0,1)) == 0xFFFFFF00u);
}

static void test_parsers(void) {
    printf("[parsers] DHCP/DNS build+parse, malformed input\n");
    u8 buf[400]; dhcp_msg_t m;
    u16 n = dhcp_build(buf, DHCP_DISCOVER, NIC_MAC, 0xCAFEF00D, 0, 0, 0);
    CHECK(n >= 300);
    CHECK(!dhcp_parse(buf, n, NIC_MAC, 0xCAFEF00D, &m));         /* it's a BOOTREQUEST, not a reply */
    buf[0] = 2;                                                   /* pretend it is a reply */
    CHECK(dhcp_parse(buf, n, NIC_MAC, 0xCAFEF00D, &m) && m.type == DHCP_DISCOVER);
    CHECK(!dhcp_parse(buf, n, NIC_MAC, 0xCAFEF00E, &m));         /* wrong xid */
    u8 other[6] = { 1,2,3,4,5,6 };
    CHECK(!dhcp_parse(buf, n, other, 0xCAFEF00D, &m));           /* wrong chaddr */
    CHECK(!dhcp_parse(buf, 100, NIC_MAC, 0xCAFEF00D, &m));       /* truncated */
    u8 bad[300]; memcpy(bad, buf, 300);
    bad[236 + 4 + 1] = 200;                                       /* option length runs off the end */
    CHECK(!dhcp_parse(bad, 260, NIC_MAC, 0xCAFEF00D, &m));

    u8 q[300];
    u16 ql = dns_build_query(q, sizeof(q), "www.example.com", 0xABCD);
    CHECK(ql == 12 + 1+3 + 1+7 + 1+3 + 1 + 4);
    CHECK(q[12] == 3 && !memcmp(q + 13, "www", 3));
    CHECK(dns_build_query(q, sizeof(q), "a..b", 1) == 0);
    CHECK(dns_build_query(q, sizeof(q), "", 1) == 0);
    CHECK(dns_build_query(q, sizeof(q), ".", 1) == 0);
    char longlabel[80]; memset(longlabel, 'a', 70); longlabel[70] = 0;
    CHECK(dns_build_query(q, sizeof(q), longlabel, 1) == 0);
    CHECK(dns_build_query(q, sizeof(q), "example.com.", 1) > 0); /* one trailing dot is fine */
    u32 ipout;
    u8 r[64] = {0}; wr16(r, 9); wr16(r + 2, 0x8180); wr16(r + 4, 1); wr16(r + 6, 1);
    r[12] = 0xC0; r[13] = 12;                                     /* question "name" = pointer to itself: loop */
    CHECK(dns_parse_response(r, 40, 9, &ipout) < 1);              /* must terminate, not hang */
    CHECK(dns_parse_response(r, 5, 9, &ipout) == -1);             /* too short */
    CHECK(dns_parse_response(r, 40, 10, &ipout) == -1);           /* wrong id */
    wr16(r + 2, 0x8183); CHECK(dns_parse_response(r, 40, 9, &ipout) == -2);   /* NXDOMAIN */
}

static void test_boot_and_dhcp(void) {
    printf("[dhcp] daemon start, DISCOVER/OFFER/REQUEST/ACK\n");
    CHECK(hespernetd_service_start() == 0);
    CHECK(hespernetd_running());
    CHECK(netif_count() == 1 && nic != NULL);
    CHECK(!hespernetd_boot_ready(fake_ms - 100));                 /* link just came up: not settled yet */
    CHECK(run_until_state(NS_BOUND, 4000));
    CHECK(hespernetd_boot_ready(fake_ms - 100));
    CHECK(nic->configured && nic->ip == LEASE_IP);
    CHECK(nic->mask == 0xFFFFFF00u && nic->gw == GW_IP && nic->dns == DNS_IP);
    CHECK(hespernetd_lease_remaining_s(nic) > 86000 && hespernetd_lease_remaining_s(nic) <= 86400);

    CHECK(hespernetd_bind_seq() == 1 && hespernetd_last_bound() == nic);       /* first lease announced exactly once */
    CHECK(obs.dhcp_discover_seen >= 1 && obs.dhcp_request_seen >= 1);
    CHECK(memcmp(obs.disc_eth_dst, BC, 6) == 0);
    CHECK(obs.disc_ip_src == 0 && obs.disc_ip_dst == 0xFFFFFFFFu);
    CHECK(obs.disc_sport == 68 && obs.disc_dport == 67);
    CHECK(obs.disc_udp_len >= 300 + 8);
    CHECK(memcmp(obs.disc_chaddr, NIC_MAC, 6) == 0 && obs.disc_magic_ok && obs.disc_opt53 == 1);
    CHECK(obs.req_has50 && obs.req_opt50 == LEASE_IP && obs.req_has54 && obs.req_opt54 == GW_IP);
    CHECK(obs.req_ciaddr == 0);                                   /* SELECTING state must not set ciaddr */
    CHECK(hespernetd_log_count() > 0);
    printf("       log tail: %s\n", hespernetd_log_line(hespernetd_log_count() - 1));
}

static void test_arp_icmp_rx(void) {
    printf("[rx] ARP replies, echo replies, bad-packet rejection\n");
    u8 fr[100];
    /* ARP who-has 10.0.2.15 from the gateway */
    memset(fr, 0, sizeof(fr));
    memcpy(fr, BC, 6); memcpy(fr + 6, GW_MAC, 6); wr16(fr + 12, 0x0806);
    u8 *a = fr + 14; wr16(a, 1); wr16(a + 2, 0x0800); a[4] = 6; a[5] = 4; wr16(a + 6, 1);
    memcpy(a + 8, GW_MAC, 6); wr32(a + 14, GW_IP); wr32(a + 24, LEASE_IP);
    obs.have_arp_reply = 0;
    inject(fr, 60);
    CHECK(obs.have_arp_reply);
    const u8 *r = obs.arp_reply;
    CHECK(memcmp(r, GW_MAC, 6) == 0 && memcmp(r + 6, NIC_MAC, 6) == 0);
    CHECK(rd16(r + 14 + 6) == 2 && rd32(r + 14 + 14) == LEASE_IP && rd32(r + 14 + 24) == GW_IP);
    CHECK(memcmp(r + 14 + 8, NIC_MAC, 6) == 0 && memcmp(r + 14 + 18, GW_MAC, 6) == 0);

    /* ARP for someone else: no reply */
    wr32(a + 24, IP4(10,0,2,99)); obs.have_arp_reply = 0; inject(fr, 60);
    CHECK(!obs.have_arp_reply);

    /* echo request to us */
    u8 pl[40]; for (int i = 0; i < 40; i++) pl[i] = (u8)(i * 7);
    u8 e[200];
    u16 n = build_icmp_frame(e, NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 8, 0x1234, 77, pl, 40, 0, 0);
    obs.have_echo_reply = 0; inject(e, n);
    CHECK(obs.have_echo_reply);
    const u8 *ip = obs.echo_reply + 14;
    CHECK(rd32(ip + 12) == LEASE_IP && rd32(ip + 16) == GW_IP);
    CHECK(ip[20] == 0 && rd16(ip + 24) == 0x1234 && rd16(ip + 26) == 77);
    CHECK(memcmp(ip + 28, pl, 40) == 0);
    CHECK(memcmp(obs.echo_reply, GW_MAC, 6) == 0);               /* straight back to sender's MAC */

    /* things that must be dropped silently */
    n = build_icmp_frame(e, NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 8, 1, 1, pl, 8, 1, 0);   /* bad ICMP csum */
    obs.have_echo_reply = 0; inject(e, n); CHECK(!obs.have_echo_reply);
    n = build_icmp_frame(e, NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 8, 1, 1, pl, 8, 0, 1);   /* bad IP csum */
    inject(e, n); CHECK(!obs.have_echo_reply);
    n = build_icmp_frame(e, NIC_MAC, GW_MAC, GW_IP, IP4(10,0,2,16), 8, 1, 1, pl, 8, 0, 0); /* not our IP */
    inject(e, n); CHECK(!obs.have_echo_reply);
    n = build_icmp_frame(e, BC, GW_MAC, GW_IP, IP4_BROADCAST, 8, 1, 1, pl, 8, 0, 0);       /* broadcast ping */
    inject(e, n); CHECK(!obs.have_echo_reply);
    n = build_icmp_frame(e, (const u8[]){2,2,3,4,5,6}, GW_MAC, GW_IP, LEASE_IP, 8, 1, 1, pl, 8, 0, 0); /* wrong unicast dst MAC (first byte even: not multicast) */
    inject(e, n); CHECK(!obs.have_echo_reply);
    u32 before = nic->rx_errors;
    n = build_icmp_frame(e, NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 8, 1, 1, pl, 8, 0, 0);
    e[14 + 6] = 0x20;                                             /* set MF flag (also breaks csum) */
    inject(e, n); CHECK(!obs.have_echo_reply);
    CHECK(nic->rx_errors > before);
}

static void test_udp(void) {
    printf("[udp] port dispatch, checksum rules\n");
    CHECK(udp_bind(7777, test_udp_handler) == 0);
    CHECK(udp_bind(7777, test_udp_handler) == -1);               /* duplicate bind refused */
    u8 fr[200]; u8 pl[20] = "hello udp";
    udp_hits = 0;
    u16 n = build_udp_frame(fr, NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 1234, 7777, pl, 12, 0);
    inject(fr, n); CHECK(udp_hits == 1 && udp_last_src == GW_IP && udp_last_len == 12);
    n = build_udp_frame(fr, NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 1234, 7777, pl, 12, 1);   /* csum 0 = none */
    inject(fr, n); CHECK(udp_hits == 2);
    n = build_udp_frame(fr, NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 1234, 7777, pl, 12, 2);   /* wrong csum */
    inject(fr, n); CHECK(udp_hits == 2);
    n = build_udp_frame(fr, NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 1234, 7778, pl, 12, 0);   /* unbound port */
    inject(fr, n); CHECK(udp_hits == 2);
    n = build_udp_frame(fr, NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 1234, 7777, pl, 12, 0);
    wr16(fr + 14 + 20 + 4, 4000);                                 /* UDP length lies */
    inject(fr, n); CHECK(udp_hits == 2);
    udp_unbind(7777);
    n = build_udp_frame(fr, NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 1234, 7777, pl, 12, 0);
    inject(fr, n); CHECK(udp_hits == 2);
    /* outbound: stack -> world with valid checksums is asserted inside world_ip() */
    CHECK(udp_send(GW_IP, 5000, 9999, pl, 12) == 0);
    CHECK(udp_send(IP4(8,8,8,8), 5000, 9999, pl, 12) == 0);       /* via gateway */
    u8 big[1500];
    CHECK(udp_send(GW_IP, 5000, 9999, big, 1473) == -3);          /* 8+1473 > 1480 */
    CHECK(udp_send(GW_IP, 5000, 9999, big, 1472) == 0);           /* exactly fills the MTU */
}

static void test_ping(void) {
    printf("[icmp] ping on-link, via gateway, failures\n");
    u32 rtt = 0; u8 ttl = 0;
    CHECK(icmp_ping(GW_IP, 1, 56, 1000, &rtt, &ttl) == 1);
    CHECK(ttl == 64 && rtt < 50);
    CHECK(icmp_ping(DNS_IP, 2, 56, 1000, &rtt, &ttl) == 1);       /* different on-link host/MAC */
    CHECK(icmp_ping(IP4(8,8,8,8), 3, 56, 1000, &rtt, &ttl) == 1); /* off-link: routed via gateway MAC */
    CHECK(icmp_ping(IP4(8,8,8,8), 4, 1400, 1000, &rtt, &ttl) == 1);
    CHECK(icmp_ping(BLACKHOLE, 5, 56, 300, &rtt, &ttl) == 0);     /* gateway forwards but nobody answers */
    int arps = obs.arp_requests_for_silent;
    CHECK(icmp_ping(SILENT_IP, 6, 56, 300, &rtt, &ttl) == -2);    /* on-link host that never ARPs back */
    CHECK(obs.arp_requests_for_silent - arps == 3);               /* exactly 3 attempts */
}

static void test_dns(void) {
    printf("[dns] resolver: CNAME chain, errors, timeouts, bad replies\n");
    u32 ip = 0;
    CHECK(dns_resolve("1.2.3.4", &ip) == 1 && ip == IP4(1,2,3,4));     /* literal: no network */
    int q0 = obs.dns_queries;
    CHECK(dns_resolve("example.com", &ip) == 1 && ip == IP4(93,184,216,34));
    CHECK(obs.dns_queries == q0 + 1);
    CHECK(dns_resolve("nxdomain.test", &ip) == -2);
    CHECK(dns_resolve("empty.test", &ip) == -3);
    q0 = obs.dns_queries;
    CHECK(dns_resolve("slow.test", &ip) == 0);                          /* timeout */
    CHECK(obs.dns_queries - q0 == 3);                                   /* retried 3 times */
    CHECK(dns_resolve("garbage.test", &ip) == 1 && ip == IP4(93,184,216,34));  /* wrong-ID reply ignored */
    CHECK(dns_resolve("bad..name", &ip) == -1);
}

static void test_renew_nak_link(void) {
    printf("[dhcp] renew at T1, NAK recovery, link flap, lease expiry\n");
    /* T1 = 43200 s */
    fake_ms += 43200u * 1000u;
    int reqs = obs.dhcp_request_seen;
    run(3);
    CHECK(obs.dhcp_request_seen == reqs + 1);
    CHECK(obs.req_ciaddr == LEASE_IP && !obs.req_has50 && !obs.req_has54);   /* RENEWING per RFC 2131 */
    CHECK(run_until_state(NS_BOUND, 200));
    CHECK(nic->ip == LEASE_IP);
    CHECK(hespernetd_lease_remaining_s(nic) > 86000);                /* timer restarted */
    CHECK(hespernetd_bind_seq() == 1);                               /* a renewal is not a new lease */

    /* server NAKs the next renewal: must drop the address and start over */
    dhcp_nak_next = 1;
    fake_ms += 43200u * 1000u;
    int disc = obs.dhcp_discover_seen;
    run(3);
    CHECK(run_until_state(NS_BOUND, 2000));
    CHECK(obs.dhcp_discover_seen > disc);
    CHECK(nic->configured && nic->ip == LEASE_IP);

    /* link down: address removed; link up: DHCP runs again */
    nic->link_up = false; run(3);
    CHECK(hespernetd_state(nic) == NS_NOLINK && !nic->configured && nic->ip == 0);
    CHECK(icmp_ping(GW_IP, 9, 56, 200, NULL, NULL) == -1);          /* no route while down */
    disc = obs.dhcp_discover_seen;
    nic->link_up = true;
    CHECK(run_until_state(NS_BOUND, 2000));
    CHECK(obs.dhcp_discover_seen > disc && nic->ip == LEASE_IP);

    /* renewal unanswered until the lease actually expires */
    dhcp_silent = 1;
    fake_ms += 43200u * 1000u; run(3);
    CHECK(hespernetd_state(nic) == NS_RENEW);
    fake_ms += 43200u * 1000u; run(3);
    CHECK(hespernetd_state(nic) == NS_DISCOVER && !nic->configured);   /* expired -> deconfigured */
    dhcp_silent = 0;
    CHECK(run_until_state(NS_BOUND, 6000));                          /* backoff retries find the server */
}

static void test_static_and_stop(void) {
    printf("[static] static config, DHCP restart, service stop/start\n");
    CHECK(hespernetd_set_static(nic, IP4(192,168,5,10), 0xFFFFFF00u, ALT_GW_IP, ALT_GW_IP) == 0);
    CHECK(hespernetd_state(nic) == NS_STATIC && nic->ip == IP4(192,168,5,10));
    u32 rtt;
    CHECK(icmp_ping(ALT_GW_IP, 1, 56, 1000, &rtt, NULL) == 1);
    nic->link_up = false; run(3); nic->link_up = true; run(3);
    CHECK(hespernetd_state(nic) == NS_STATIC && nic->ip == IP4(192,168,5,10));  /* static survives a link flap */
    hespernetd_dhcp_restart(nic);
    CHECK(run_until_state(NS_BOUND, 2000) && nic->ip == LEASE_IP);

    hespernetd_service_stop();
    CHECK(!hespernetd_running() && !nic->configured);
    CHECK(hespernetd_state(nic) == NS_STOPPED);
    CHECK(hespernetd_service_start() == 0);
    CHECK(run_until_state(NS_BOUND, 2000) && nic->ip == LEASE_IP);
}

static unsigned rnd_seed = 12345;
static unsigned rnd(void) { rnd_seed = rnd_seed * 1103515245u + 12345u; return (rnd_seed >> 16) & 0x7FFF; }

static void test_fuzz(void) {
    printf("[fuzz] 200k random + mutated frames into the RX path (ASan/UBSan armed)\n");
    #define RND() rnd()
    /* known-good seeds to mutate: ARP, echo, UDP, DHCP offer, DNS reply */
    u8 base[6][700]; u16 blen[6];
    u8 pl[40] = {0};
    blen[0] = build_icmp_frame(base[0], NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 8, 1, 1, pl, 32, 0, 0);
    blen[1] = build_udp_frame(base[1], NIC_MAC, GW_MAC, GW_IP, LEASE_IP, 53, 4444, pl, 30, 0);
    u8 d[300]; memset(d, 0, sizeof(d)); d[0] = 2; d[1] = 1; d[2] = 6; memcpy(d + 28, NIC_MAC, 6);
    memcpy(d + 236, "\x63\x82\x53\x63", 4); d[240] = 53; d[241] = 1; d[242] = 2; d[243] = 255;
    blen[2] = build_udp_frame(base[2], BC, GW_MAC, GW_IP, IP4_BROADCAST, 67, 68, d, 300, 0);
    memset(base[3], 0, 700); memcpy(base[3], BC, 6); memcpy(base[3] + 6, GW_MAC, 6); wr16(base[3] + 12, 0x0806);
    wr16(base[3] + 14, 1); wr16(base[3] + 16, 0x0800); base[3][18] = 6; base[3][19] = 4; wr16(base[3] + 20, 1);
    blen[3] = 60;
    blen[4] = build_udp_frame(base[4], NIC_MAC, DNS_MAC, DNS_IP, LEASE_IP, 53, 49152, d, 100, 0);
    blen[5] = 0;
    for (int i = 0; i < 200000; i++) {
        u8 f[1600]; u16 len;
        if (i & 1) {
            len = (u16)(RND() % 1600);
            for (int k = 0; k < len; k++) f[k] = (u8)RND();
            if (len >= 6 && (i & 2)) memcpy(f, NIC_MAC, 6);       /* get past the MAC filter */
        } else {
            int b = (int)(RND() % 5);
            len = blen[b]; memcpy(f, base[b], len);
            int muts = 1 + (int)(RND() % 4);
            for (int k = 0; k < muts; k++) { unsigned pos = RND() % len; f[pos] = (u8)RND(); }
            if (RND() % 5 == 0) len = (u16)(RND() % (len + 1));   /* truncation */
        }
        netif_rx(nic, f, len);
        if ((i & 0x3FF) == 0) { rxq_head = rxq_tail; }            /* drop world's replies, keep queue sane */
    }
    CHECK(1);     /* reaching here without a sanitizer abort is the assertion */
}

int main(void) {
    test_util();
    test_parsers();
    test_boot_and_dhcp();
    test_arp_icmp_rx();
    test_udp();
    test_ping();
    test_dns();
    test_renew_nak_link();
    test_static_and_stop();
    test_fuzz();
    printf("\n%d checks passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
