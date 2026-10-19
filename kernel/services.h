#ifndef VESPEROS_SERVICES_H
#define VESPEROS_SERVICES_H
#include "../include/types.h"

/* OpenRC-style services/runlevels. services have init/exit hooks, can be enabled/disabled at runtime
 * and are tied to runlevels. SysVenus manages "interactive" */

typedef enum { RUNLEVEL_BOOT, RUNLEVEL_INTERACTIVE } runlevel_t;

typedef struct {
    char name[32];
    char desc[64];
    int (*start)(void);  /* returns 0 on success */
    void (*stop)(void);
    bool started;
    bool auto_start; /* start at boot if runlevel permits */
} service_t;

void services_init(void);
int  service_start(const char *name);
int  service_stop(const char *name);
int  service_status(const char *name);
void services_list(void (*printer)(const char *name, const char *desc, bool started));
void services_register(const char *name, const char *desc, int (*start)(void), void (*stop)(void), bool auto_start);

#endif
