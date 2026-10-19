#include "vpkg.h"
#include "vfs.h"
#include "string.h"

#define MAX_PKGS 16

typedef struct {
    char name[32];
    char desc[64];
    vfs_node_t *node;
} pkg_entry_t;

static pkg_entry_t pkgs[MAX_PKGS];
static int pkg_count = 0;

/* declared by each package's own source file */
extern void vespfetch_register(void);
extern void vesmatrix_register(void);
extern void vespcal_register(void);
extern void vespnote_register(void);
extern void chronos_register(void);
extern void sdm_register(void);
extern void orionwm_register(void);
extern void se_register(void);
extern void posixtest_register(void);

void vpkg_register(const char *name, const char *desc, void (*run)(int, char**)) {
    if (pkg_count >= MAX_PKGS) return;
    vfs_node_t *n = vfs_mkprogram(vfs_dir_pkgs, name, run);
    pkg_entry_t *p = &pkgs[pkg_count++];
    k_strcpy(p->name, name);
    k_strcpy(p->desc, desc);
    p->node = n;
}

void vpkg_init_builtin_packages(void) {
    pkg_count = 0;
    vespfetch_register();
    vesmatrix_register();
    vespcal_register();
    vespnote_register();
    chronos_register();
    sdm_register();
    orionwm_register();
    se_register();
    posixtest_register();
}

static pkg_entry_t *find_pkg(const char *name) {
    for (int i = 0; i < pkg_count; i++)
        if (k_streq(pkgs[i].name, name)) return &pkgs[i];
    return NULL;
}

int vpkg_is_installed(const char *name) {
    return vfs_find_child(vfs_dir_bin, name) != NULL;
}

int vpkg_install(const char *name) {
    pkg_entry_t *p = find_pkg(name);
    if (!p) return -1; /* not in /offline_packages */
    if (vpkg_is_installed(name)) return 0; /* already installed */
    vfs_link(vfs_dir_bin, p->node);
    return 1;
}

int vpkg_remove(const char *name) {
    if (!find_pkg(name)) return -1;
    vfs_node_t *bin = vfs_dir_bin;
    for (int i = 0; i < bin->child_count; i++) {
        if (k_streq(bin->children[i]->name, name)) {
            for (int j = i; j < bin->child_count - 1; j++)
                bin->children[j] = bin->children[j + 1];
            bin->child_count--;
            return 1;
        }
    }
    return 0;
}

void vpkg_list(void (*printer)(const char *name, const char *desc, int installed)) {
    for (int i = 0; i < pkg_count; i++)
        printer(pkgs[i].name, pkgs[i].desc, vpkg_is_installed(pkgs[i].name));
}

void vpkg_install_all(void) {
    for (int i = 0; i < pkg_count; i++)
        vpkg_install(pkgs[i].name);
}
