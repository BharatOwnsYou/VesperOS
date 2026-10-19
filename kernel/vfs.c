#include "../include/version.h"
#include "../include/arch.h"
#include "vfs.h"
#include "string.h"

static vfs_node_t pool[VFS_MAX_NODES];
static int pool_used = 0;
static vfs_node_t *root;

/* backing storage for writable files (vfs_mkfile_writable). no heap here so each file gets a fixed
 * slice of this pool when it's created */
#define WRITE_POOL_FILES 8
static char write_pool[WRITE_POOL_FILES][VFS_WRITE_BUF];
static int write_pool_used = 0;

vfs_node_t *vfs_dir_bin;
vfs_node_t *vfs_dir_etc;
vfs_node_t *vfs_dir_home;
vfs_node_t *vfs_dir_pkgs;

static vfs_node_t *alloc_node(vfs_node_t *parent, const char *name, vfs_type_t type) {
    if (pool_used >= VFS_MAX_NODES) return NULL;
    vfs_node_t *n = &pool[pool_used++];
    k_memset(n, 0, sizeof(*n));
    k_strcpy(n->name, name);
    n->type = type;
    n->parent = parent;
    if (parent && parent->child_count < VFS_MAX_CHILDREN) {
        parent->children[parent->child_count++] = n;
    }
    return n;
}

vfs_node_t *vfs_mkdir(vfs_node_t *parent, const char *name) {
    return alloc_node(parent, name, VFS_DIR);
}

vfs_node_t *vfs_mkfile(vfs_node_t *parent, const char *name, const char *text) {
    vfs_node_t *n = alloc_node(parent, name, VFS_FILE);
    if (n) n->text = text;
    return n;
}

vfs_node_t *vfs_mkfile_writable(vfs_node_t *parent, const char *name) {
    if (write_pool_used >= WRITE_POOL_FILES) return NULL; /* pool exhausted */
    vfs_node_t *n = alloc_node(parent, name, VFS_FILE);
    if (!n) return NULL;
    n->wbuf = write_pool[write_pool_used++];
    n->wcap = VFS_WRITE_BUF;
    n->wbuf[0] = 0;
    n->text = n->wbuf; /* so `cat` and anything else reading ->text sees live content */
    return n;
}

vfs_node_t *vfs_mkprogram(vfs_node_t *parent, const char *name, void (*run)(int, char**)) {
    vfs_node_t *n = alloc_node(parent, name, VFS_PROGRAM);
    if (n) n->run = run;
    return n;
}

vfs_node_t *vfs_find_child(vfs_node_t *dir, const char *name) {
    if (!dir) return NULL;
    for (int i = 0; i < dir->child_count; i++)
        if (k_streq(dir->children[i]->name, name)) return dir->children[i];
    return NULL;
}

/* for vpkg install: re-parents a *reference* into dest_dir's children without duplicating the node
 * (/offline_packages keeps its copy) */
vfs_node_t *vfs_link(vfs_node_t *dest_dir, vfs_node_t *existing_node) {
    if (!dest_dir || !existing_node) return NULL;
    if (dest_dir->child_count >= VFS_MAX_CHILDREN) return NULL;
    if (vfs_find_child(dest_dir, existing_node->name)) return vfs_find_child(dest_dir, existing_node->name);
    dest_dir->children[dest_dir->child_count++] = existing_node;
    return existing_node;
}

void vfs_path(vfs_node_t *node, char *out, size_t outsz) {
    char stack[8][VFS_MAX_NAME];
    int depth = 0;
    vfs_node_t *n = node;
    while (n && n->parent && depth < 8) {
        k_strcpy(stack[depth++], n->name);
        n = n->parent;
    }
    out[0] = 0;
    if (depth == 0) { k_strcpy(out, "/"); return; }
    for (int i = depth - 1; i >= 0; i--) {
        k_strcat(out, "/");
        k_strcat(out, stack[i]);
        (void)outsz;
    }
}

vfs_node_t *vfs_root(void) { return root; }

void vfs_init(void) {
    pool_used = 0;
    root = alloc_node(NULL, "/", VFS_DIR);
    vfs_dir_bin  = vfs_mkdir(root, "bin");
    vfs_dir_etc  = vfs_mkdir(root, "etc");
    vfs_dir_home = vfs_mkdir(root, "home");
    vfs_dir_pkgs = vfs_mkdir(root, "offline_packages");

    vfs_mkfile(vfs_dir_etc, "version",
        VESPER_OS_FULLNAME "\nKernel: " VESPER_KERNEL_TAG " (codename \"" VESPER_CODENAME "\")\nArch: " VESPER_ARCH "\nBoot: GRUB (multiboot2, BIOS+UEFI)\nLicense: GPLv2\n");
    vfs_mkfile(vfs_dir_etc, "motd",
        "Welcome to " VESPER_OS_FULLNAME ". Type 'help' to get started.\n");
}
