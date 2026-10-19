#ifndef VESPEROS_MODULES_H
#define VESPEROS_MODULES_H
#include "../include/types.h"

typedef struct {
    char name[24];
    char desc[48];
    int (*init)(void);
    void (*exit)(void);
    bool loaded;
    bool builtin;
} kmodule_t;

void modules_init(void);
int  module_enable(const char *name);
int  module_disable(const char *name);
void module_list(void (*printer)(const char *name, const char *desc, bool loaded));
int  module_count(void);

#endif
