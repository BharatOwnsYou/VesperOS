#include "icmp.h"
#include "ipv4.h"
#include "netutil.h"
#include "../kernel/string.h"

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8
#define PING_ID           0x4845   /* "HE" */

static volatile bool ping_waiting = false;
static volatile bool ping_got = false;
static u16 ping_seq_wanted;
static u32 ping_dst_wanted;
static u8  ping_ttl;

void icmp_input(netif_t *nif, u32 src, u32 dst, u8 ttl,
                const u8 *m, u16 len, const u8 src_mac[6]) {
    if (len < 8) { nif->rx_errors++; return; }
    if (net_checksum(m, len) != 0) { nif->rx_errors++; return; }

    if (m[0] == ICMP_ECHO_REQUEST && m[1] == 0) {
        /* only answer pings aimed at us (not broadcast/smurf) and only when configured.
                 * reply goes straight to the sender's MAC, no ARP, so it's safe in RX context */
        if (!nif->configured || dst != nif->ip) { nif->rx_dropped++; return; }
        if (len > IPV4_MAX_PAYLOAD) return;
        u8 reply[IPV4_MAX_PAYLOAD];
        k_memcpy(reply, m, len);
        reply[0] = ICMP_ECHO_REPLY;
        wr16(reply + 2, 0);
        wr16(reply + 2, net_checksum(reply, len));
        u8 mac[6];
        k_memcpy(mac, src_mac, 6);
        ipv4_send_on(nif, nif->ip, src, IPPROTO_ICMP, reply, len, mac);
    } else if (m[0] == ICMP_ECHO_REPLY && m[1] == 0) {
        if (ping_waiting && rd16(m + 4) == PING_ID && rd16(m + 6) == ping_seq_wanted
            && src == ping_dst_wanted) {
            ping_ttl = ttl;
            ping_got = true;
        }
    }
}

int icmp_ping(u32 dst, u16 seq, u16 payload_len, u32 timeout_ms,
              u32 *rtt_ms, u8 *ttl_out) {
    if (payload_len > IPV4_MAX_PAYLOAD - 8) payload_len = IPV4_MAX_PAYLOAD - 8;
    u8 msg[IPV4_MAX_PAYLOAD];
    msg[0] = ICMP_ECHO_REQUEST; msg[1] = 0;
    wr16(msg + 2, 0);
    wr16(msg + 4, PING_ID);
    wr16(msg + 6, seq);
    for (u16 i = 0; i < payload_len; i++) msg[8 + i] = (u8)(0x10 + (i & 0x3F));
    wr16(msg + 2, net_checksum(msg, (u32)8 + payload_len));

    ping_seq_wanted = seq;
    ping_dst_wanted = dst;
    ping_got = false;
    ping_waiting = true;

    u32 t0 = net_time_ms();
    int r = ipv4_send(dst, IPPROTO_ICMP, msg, (u16)(8 + payload_len));
    if (r != 0) { ping_waiting = false; return r; }

    while ((u32)(net_time_ms() - t0) < timeout_ms) {
        net_poll_all();
        if (ping_got) {
            if (rtt_ms) *rtt_ms = (u32)(net_time_ms() - t0);
            if (ttl_out) *ttl_out = ping_ttl;
            ping_waiting = false;
            return 1;
        }
    }
    ping_waiting = false;
    return 0;
}
