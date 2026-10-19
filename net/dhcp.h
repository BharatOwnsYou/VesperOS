#ifndef HESPERNET_DHCP_H
#define HESPERNET_DHCP_H
#include "../include/types.h"

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_DECLINE  4
#define DHCP_ACK      5
#define DHCP_NAK      6

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

typedef struct {
    u8   type;                 /* DHCP message type, 0 if absent */
    u32  yiaddr;               /* address offered/assigned */
    u32  server_id;
    u32  mask, router, dns, lease_s;
    bool has_mask, has_router, has_dns, has_lease, has_server_id;
} dhcp_msg_t;

/* Builds a client message into buf (>= 320 bytes). ciaddr is only set for renewals;
 * req_ip / server_id are emitted as options 50/54 when non-zero. Returns length. */
u16 dhcp_build(u8 *buf, u8 type, const u8 mac[6], u32 xid,
               u32 ciaddr, u32 req_ip, u32 server_id);

/* Parses a server reply. Returns 1 if it is a well-formed BOOTREPLY for this
 * client (matching xid and chaddr) with a DHCP message type; 0 otherwise. */
int  dhcp_parse(const u8 *pkt, u16 len, const u8 mac[6], u32 xid, dhcp_msg_t *out);

#endif
