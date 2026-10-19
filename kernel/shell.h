#ifndef VESPEROS_SHELL_H
#define VESPEROS_SHELL_H
#include "vfs.h"

/* SysVenus is the init: brings up modules/VFS/packages, prints the banner, then becomes the shell. never returns */
void sysvenus_main(void) __attribute__((noreturn));

/* shell's cwd, exposed so offline_packages like se (the editor) can create/find files relative to where
 * the user is instead of always the VFS root */
vfs_node_t *sysvenus_cwd(void);

#endif
