#include "services.h"
#include "string.h"

#define MAX_SERVICES 32
static service_t services[MAX_SERVICES];
static int service_count = 0;

void services_init(void) {
    service_count = 0;
}

void services_register(const char *name, const char *desc, int (*start)(void), void (*stop)(void), bool auto_start) {
    if (service_count >= MAX_SERVICES) return;
    service_t *s = &services[service_count++];
    k_strcpy(s->name, name);
    k_strcpy(s->desc, desc);
    s->start = start;
    s->stop = stop;
    s->started = false;
    s->auto_start = auto_start;
    
    if (auto_start && s->start() == 0)
        s->started = true;      /* a service that fails to start must not be reported as running */
}

static service_t *find_service(const char *name) {
    for (int i = 0; i < service_count; i++)
        if (k_streq(services[i].name, name)) return &services[i];
    return NULL;
}

int service_start(const char *name) {
    service_t *s = find_service(name);
    if (!s) return -1;
    if (s->started) return 0;            /* already running */
    if (s->start() != 0) return -2;      /* start hook failed */
    s->started = true;
    return 1;                            /* newly started */
}

int service_stop(const char *name) {
    service_t *s = find_service(name);
    if (!s) return -1;
    if (!s->started) return 0;
    s->stop();
    s->started = false;
    return 1;
}

int service_status(const char *name) {
    service_t *s = find_service(name);
    if (!s) return -1;
    return s->started ? 1 : 0;
}

void services_list(void (*printer)(const char *name, const char *desc, bool started)) {
    for (int i = 0; i < service_count; i++)
        printer(services[i].name, services[i].desc, services[i].started);
}
