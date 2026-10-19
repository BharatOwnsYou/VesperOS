#include "hespernetd.h"
#include "arp.h"
#include "udp.h"
#include "dhcp.h"
#include "netutil.h"
#include "../kernel/string.h"

int netdrv_probe_all(void);    /* drivers/netdrv.c (the host test supplies its own) */

/* ------------------------------------------------------------ event log */
#define LOG_LINES 24
#define LOG_W     84
static char logbuf[LOG_LINES][LOG_W];
static int  log_total = 0;

static void hlog(const char *a, const char *b, const char *c, const char *d) {
    char *line = logbuf[log_total % LOG_LINES];
    line[0] = 0;
    const char *parts[4] = { a, b, c, d };
    u32 used = 0;
    for (int i = 0; i < 4; i++) {
        if (!parts[i]) continue;
        for (const char *p = parts[i]; *p && used < LOG_W - 1; p++) line[used++] = *p;
    }
    line[used] = 0;
    log_total++;
}

int hespernetd_log_count(void) { return log_total < LOG_LINES ? log_total : LOG_LINES; }
const char *hespernetd_log_line(int i) {
    int n = hespernetd_log_count();
    if (i < 0 || i >= n) return "";
    int first = log_total - n;
    return logbuf[(first + i) % LOG_LINES];
}

/* --------------------------------------------------------------- state */
typedef struct {
    ns_state_t state;
    bool   static_cfg;
    u32    xid;
    u32    t_tx;          /* last DHCP transmit */
    u32    interval;      /* current retransmit interval, ms */
    u8     tries;
    u32    t_bound;       /* when the lease was (re)acquired */
    u32    lease_ms;      /* 0 = infinite */
    bool   have_msg;      /* RX handler -> main loop mailbox (single slot) */
    dhcp_msg_t msg;
} ns_if_t;

static ns_if_t ns[NETIF_MAX];
static bool running = false;
static bool probed = false;
static u32  xid_counter = 0x5E5B0001u;
static u32  bind_seq = 0;
static int  last_bound_idx = -1;

#define ELAPSED(t) ((u32)(net_time_ms() - (t)))

static u32 next_xid(const netif_t *nif) {
    xid_counter += 0x9E37u;
    return xid_counter ^ ((u32)nif->mac[4] << 8) ^ ((u32)nif->mac[5]) ^ (net_time_ms() << 16);
}

static void unconfigure(netif_t *nif) {
    nif->configured = false;
    nif->ip = nif->mask = nif->gw = nif->dns = 0;
    arp_flush();
}

/* UDP/68 handler - RX context: parse, stash, return. The main loop acts on it. */
static void dhcp_rx(netif_t *nif, u32 src, u16 sport, const u8 *data, u16 len) {
    (void)src;
    if (sport != DHCP_SERVER_PORT || nif->index < 0 || nif->index >= NETIF_MAX) return;
    ns_if_t *s = &ns[nif->index];
    if (s->state != NS_DISCOVER && s->state != NS_REQUEST && s->state != NS_RENEW) return;
    if (s->have_msg) return;
    dhcp_msg_t m;
    if (!dhcp_parse(data, len, nif->mac, s->xid, &m)) return;
    s->msg = m;
    s->have_msg = true;
}

static void tx_dhcp(netif_t *nif, ns_if_t *s, u8 type, u32 ciaddr, u32 req, u32 srv) {
    u8 buf[320];
    u16 n = dhcp_build(buf, type, nif->mac, s->xid, ciaddr, req, srv);
    udp_send_broadcast(nif, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, buf, n);
    s->t_tx = net_time_ms();
}

static void start_discover(netif_t *nif, ns_if_t *s) {
    s->xid = next_xid(nif);
    s->have_msg = false;
    s->state = NS_DISCOVER;
    s->tries = 1;
    s->interval = 2000;
    tx_dhcp(nif, s, DHCP_DISCOVER, 0, 0, 0);
    hlog(nif->name, ": DHCP discover", 0, 0);
}

static void bind_lease(netif_t *nif, ns_if_t *s, const dhcp_msg_t *m) {
    nif->ip = m->yiaddr;
    nif->mask = m->has_mask ? m->mask : ip_default_mask(m->yiaddr);
    nif->gw = m->has_router ? m->router : 0;
    nif->dns = m->has_dns ? m->dns : 0;
    nif->configured = true;
    arp_flush();

    u32 secs = m->has_lease ? m->lease_s : 3600;
    if (secs == 0xFFFFFFFFu) s->lease_ms = 0;                  /* infinite */
    else if (secs == 0) s->lease_ms = 3600u * 1000u;
    else s->lease_ms = (secs > 4294966u ? 4294966u : secs) * 1000u;
    if (s->state != NS_RENEW) { bind_seq++; last_bound_idx = nif->index; }   /* first lease only, not renewals */
    s->t_bound = net_time_ms();
    s->state = NS_BOUND;
    s->tries = 0;

    char a[16];
    ip_to_str(nif->ip, a);
    hlog(nif->name, ": bound ", a, nif->gw ? " (gateway set)" : " (no gateway)");
}

static void tick_iface(netif_t *nif, ns_if_t *s) {
    if (s->state == NS_STOPPED) return;

    if (!nif->link_up) {
        if (s->state != NS_NOLINK) {
            if (!s->static_cfg) unconfigure(nif);
            arp_flush();
            s->state = NS_NOLINK;
            s->have_msg = false;
            hlog(nif->name, ": link down", 0, 0);
        }
        return;
    }

    switch (s->state) {
    case NS_NOLINK:
        hlog(nif->name, ": link up", 0, 0);
        if (s->static_cfg && nif->configured) s->state = NS_STATIC;
        else start_discover(nif, s);
        break;

    case NS_DISCOVER:
        if (s->have_msg) {
            dhcp_msg_t m = s->msg;
            s->have_msg = false;
            if (m.type == DHCP_OFFER && m.yiaddr && m.has_server_id) {
                s->msg = m;                       /* keep for reference */
                tx_dhcp(nif, s, DHCP_REQUEST, 0, m.yiaddr, m.server_id);
                s->state = NS_REQUEST;
                s->tries = 1;
                s->interval = 2000;
                hlog(nif->name, ": DHCP offer, requesting", 0, 0);
            }
        } else if (ELAPSED(s->t_tx) >= s->interval) {
            tx_dhcp(nif, s, DHCP_DISCOVER, 0, 0, 0);
            s->tries++;
            if (s->interval < 16000) s->interval *= 2;
            if (s->tries == 4) hlog(nif->name, ": no DHCP server answering yet, still trying", 0, 0);
        }
        break;

    case NS_REQUEST:
        if (s->have_msg) {
            dhcp_msg_t m = s->msg;
            s->have_msg = false;
            if (m.type == DHCP_ACK && m.yiaddr) bind_lease(nif, s, &m);
            else if (m.type == DHCP_NAK) {
                hlog(nif->name, ": DHCP NAK, restarting", 0, 0);
                start_discover(nif, s);
            }
        } else if (ELAPSED(s->t_tx) >= s->interval) {
            if (s->tries >= 4) {
                hlog(nif->name, ": DHCP request timed out, restarting", 0, 0);
                start_discover(nif, s);
            } else {
                tx_dhcp(nif, s, DHCP_REQUEST, 0, s->msg.yiaddr, s->msg.server_id);
                s->tries++;
                s->interval *= 2;
            }
        }
        break;

    case NS_BOUND:
        if (s->lease_ms && ELAPSED(s->t_bound) >= s->lease_ms / 2) {   /* T1 */
            s->xid = next_xid(nif);
            s->have_msg = false;
            s->state = NS_RENEW;
            tx_dhcp(nif, s, DHCP_REQUEST, nif->ip, 0, 0);
            hlog(nif->name, ": renewing lease", 0, 0);
        }
        break;

    case NS_RENEW:
        if (s->have_msg) {
            dhcp_msg_t m = s->msg;
            s->have_msg = false;
            if (m.type == DHCP_ACK && m.yiaddr == nif->ip) {
                bind_lease(nif, s, &m);
                hlog(nif->name, ": lease renewed", 0, 0);
            } else if (m.type == DHCP_NAK || (m.type == DHCP_ACK && m.yiaddr != nif->ip)) {
                hlog(nif->name, ": renewal refused, restarting", 0, 0);
                unconfigure(nif);
                start_discover(nif, s);
            }
        } else if (ELAPSED(s->t_bound) >= s->lease_ms) {
            hlog(nif->name, ": lease expired", 0, 0);
            unconfigure(nif);
            start_discover(nif, s);
        } else if (ELAPSED(s->t_tx) >= 10000) {
            tx_dhcp(nif, s, DHCP_REQUEST, nif->ip, 0, 0);
        }
        break;

    case NS_STATIC:
    case NS_STOPPED:
        break;
    }
}

void hespernetd_poll(void) {
    if (!running) return;
    net_poll_all();
    for (int i = 0; i < netif_count(); i++) tick_iface(netif_at(i), &ns[i]);
}

bool hespernetd_running(void) { return running; }

int hespernetd_service_start(void) {
    if (running) return 0;
    if (!probed || netif_count() == 0) {
        netdrv_probe_all();
        probed = true;
    }
    if (netif_count() == 0) {
        hlog("hespernetd: no supported network adapter found", 0, 0, 0);
        return -1;
    }
    hlog("hespernetd: started", 0, 0, 0);
    for (int i = 0; i < netif_count(); i++) {
        netif_t *n = netif_at(i);
        ns[i].state = NS_NOLINK;
        ns[i].have_msg = false;
        hlog(n->name, ": ", n->driver, 0);
    }
    udp_bind(DHCP_CLIENT_PORT, dhcp_rx);
    running = true;
    return 0;
}

void hespernetd_service_stop(void) {
    if (!running) return;
    running = false;
    udp_unbind(DHCP_CLIENT_PORT);
    for (int i = 0; i < netif_count(); i++) {
        netif_t *n = netif_at(i);
        if (!ns[i].static_cfg) unconfigure(n);
        ns[i].state = NS_STOPPED;
        ns[i].have_msg = false;
    }
    hlog("hespernetd: stopped", 0, 0, 0);
}

int hespernetd_boot_ready(u32 t_start) {
    if (!running) return 1;
    u32 el = ELAPSED(t_start);
    if (el >= 10000) return 1;
    for (int i = 0; i < netif_count(); i++) {
        ns_state_t st = ns[i].state;
        if (st == NS_DISCOVER || st == NS_REQUEST) return 0;
        if (st == NS_NOLINK && el < 6500) return 0;
    }
    return 1;
}

u32 hespernetd_bind_seq(void) { return bind_seq; }
const netif_t *hespernetd_last_bound(void) { return last_bound_idx >= 0 ? netif_at(last_bound_idx) : NULL; }

ns_state_t hespernetd_state(const netif_t *nif) {
    if (!nif || nif->index < 0 || nif->index >= NETIF_MAX) return NS_STOPPED;
    return ns[nif->index].state;
}

const char *hespernetd_state_name(const netif_t *nif) {
    switch (hespernetd_state(nif)) {
        case NS_NOLINK:   return "no link";
        case NS_DISCOVER: return "dhcp: discovering";
        case NS_REQUEST:  return "dhcp: requesting";
        case NS_BOUND:    return "up (dhcp lease)";
        case NS_RENEW:    return "up (renewing lease)";
        case NS_STATIC:   return "up (static)";
        default:          return "stopped";
    }
}

u32 hespernetd_lease_remaining_s(const netif_t *nif) {
    if (!nif) return 0;
    const ns_if_t *s = &ns[nif->index];
    if ((s->state != NS_BOUND && s->state != NS_RENEW) || s->lease_ms == 0) return 0;
    u32 el = ELAPSED(s->t_bound);
    return el >= s->lease_ms ? 0 : (s->lease_ms - el) / 1000;
}

void hespernetd_dhcp_restart(netif_t *only) {
    if (!running) return;
    for (int i = 0; i < netif_count(); i++) {
        netif_t *n = netif_at(i);
        if (only && n != only) continue;
        ns[i].static_cfg = false;
        unconfigure(n);
        ns[i].have_msg = false;
        if (n->link_up) start_discover(n, &ns[i]);
        else ns[i].state = NS_NOLINK;
    }
}

int hespernetd_set_static(netif_t *nif, u32 ip, u32 mask, u32 gw, u32 dns) {
    if (!running || !nif) return -1;
    ns_if_t *s = &ns[nif->index];
    s->static_cfg = true;
    s->have_msg = false;
    nif->ip = ip; nif->mask = mask; nif->gw = gw; nif->dns = dns;
    nif->configured = true;
    arp_flush();
    s->state = nif->link_up ? NS_STATIC : NS_NOLINK;
    char a[16];
    ip_to_str(ip, a);
    hlog(nif->name, ": static address ", a, 0);
    return 0;
}
