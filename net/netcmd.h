#ifndef HESPERNET_NETCMD_H
#define HESPERNET_NETCMD_H
#include "../include/types.h"

/* Shell commands for the network stack: ifconfig, lsnet, ping, nslookup,
 * hespernetctl. Returns 1 if argv[0] was one of them (and has been run). */
int  netcmd_dispatch(int argc, char **argv);

/* Prints the per-interface summary shown at boot (after hespernetd has run). */
void netcmd_boot_report(bool verbose);

/* Announces (one line) a DHCP lease that arrived after the boot-time wait. Call from the shell's input pump. */
void netcmd_async_notice(void);
void netcmd_notice_sync(void);     /* mark everything announced so far as already reported (call after the boot report) */

/* One pass of everything that must keep running while the shell waits for input. */
void netcmd_pump(void);

extern const char NETCMD_HELP[];

#endif
