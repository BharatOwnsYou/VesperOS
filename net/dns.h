#ifndef HESPERNET_DNS_H
#define HESPERNET_DNS_H
#include "../include/types.h"

/* pure helpers (unit-tested) */
u16 dns_build_query(u8 *buf, u16 bufsz, const char *name, u16 id);   /* 0 = invalid name */
/* 1 = A record found (*ip set), 0 = no A record, -1 = malformed/mismatched, -2 = server error / NXDOMAIN */
int dns_parse_response(const u8 *buf, u16 len, u16 id, u32 *ip);

/* Blocking resolver using the DNS server learned from DHCP (or set statically).
 * Accepts dotted-quad literals. 1 = ok, 0 = timeout, -1 = no resolver/send failed,
 * -2 = name does not exist (or server failure), -3 = no IPv4 address in the answer. */
int dns_resolve(const char *name, u32 *ip);

#endif
