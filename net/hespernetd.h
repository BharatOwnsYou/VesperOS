#ifndef HESPERNET_HESPERNETD_H
#define HESPERNET_HESPERNETD_H
#include "netif.h"

/* hespernetd: the network manager daemon. owns every NIC: probes drivers at start, tracks link state, runs a
 * non-blocking DHCP client per interface (discover/request/renew/expire), static config, small event log.
 * registered as an OpenRC-style service ("venus smite hespernetd"), enabled by default at boot.
 * no interrupts in this kernel so it's driven by hespernetd_poll(), which the shell's input pump calls constantly */

typedef enum {
    NS_STOPPED = 0, NS_NOLINK, NS_DISCOVER, NS_REQUEST,
    NS_BOUND, NS_RENEW, NS_STATIC
} ns_state_t;

int  hespernetd_service_start(void);   /* service hooks: 0 = success */
void hespernetd_service_stop(void);
void hespernetd_poll(void);
bool hespernetd_running(void);

/* boot helper: 1 once every interface has settled (bound/static/gave up on link) or 10 s after t_start
 * (a net_time_ms() value). a gigabit PHY can need 4-5 s to negotiate so a link that's still down gets 6.5 s
 * before we stop waiting for it */
int  hespernetd_boot_ready(u32 t_start);

ns_state_t  hespernetd_state(const netif_t *nif);
const char *hespernetd_state_name(const netif_t *nif);
u32         hespernetd_lease_remaining_s(const netif_t *nif);   /* 0 = n/a or infinite */

void hespernetd_dhcp_restart(netif_t *nif);          /* nif == NULL: all interfaces */
int  hespernetd_set_static(netif_t *nif, u32 ip, u32 mask, u32 gw, u32 dns);  /* 0 = ok */

/* bumped each time an interface gets its FIRST address from a DHCP server (renewals don't count),
 * so the shell can announce a lease that arrives after the boot wait */
u32            hespernetd_bind_seq(void);
const netif_t *hespernetd_last_bound(void);

int         hespernetd_log_count(void);
const char *hespernetd_log_line(int i);              /* 0 = oldest */

#endif
