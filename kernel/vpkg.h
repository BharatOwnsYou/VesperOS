#ifndef VESPEROS_VPKG_H
#define VESPEROS_VPKG_H
#include "../include/types.h"

/* every offline package is a program compiled into the kernel image and registered here, no on-disk loader yet.
 * /offline_packages is the VFS dir listing them, `vpkg install <n>` links one into /bin so it runs by name */
void vpkg_register(const char *name, const char *desc, void (*run)(int, char**));
void vpkg_init_builtin_packages(void); /* registers vespfetch, vesmatrix, etc */

int  vpkg_install(const char *name);
int  vpkg_remove(const char *name);
int  vpkg_is_installed(const char *name);
void vpkg_list(void (*printer)(const char *name, const char *desc, int installed));
void vpkg_install_all(void); /* installs every registered package into /bin - used by install-vesper */

#endif
