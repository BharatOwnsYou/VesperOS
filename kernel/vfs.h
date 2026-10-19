#ifndef VESPEROS_VFS_H
#define VESPEROS_VFS_H
#include "../include/types.h"

#define VFS_MAX_NAME     32
#define VFS_MAX_CHILDREN 24
#define VFS_MAX_NODES    96
#define VFS_WRITE_BUF     2048   /* capacity of an editable file created via vfs_mkfile_writable */

typedef enum { VFS_DIR, VFS_FILE, VFS_PROGRAM } vfs_type_t;

typedef struct vfs_node {
    char name[VFS_MAX_NAME];
    vfs_type_t type;
    struct vfs_node *parent;
    struct vfs_node *children[VFS_MAX_CHILDREN];
    int child_count;
    const char *text;        /* for VFS_FILE: static content to `cat` */
    char *wbuf;               /* for a writable VFS_FILE created via vfs_mkfile_writable - NULL for static/const files */
    size_t wcap;              /* capacity of wbuf */
    void (*run)(int argc, char **argv); /* for VFS_PROGRAM */
} vfs_node_t;

void vfs_init(void);
vfs_node_t *vfs_root(void);
vfs_node_t *vfs_mkdir(vfs_node_t *parent, const char *name);
vfs_node_t *vfs_mkfile(vfs_node_t *parent, const char *name, const char *text);
/* creates a file with its own fixed writable buffer (VFS_WRITE_BUF bytes from a static pool, no heap).
 * `se` uses this */
vfs_node_t *vfs_mkfile_writable(vfs_node_t *parent, const char *name);
vfs_node_t *vfs_mkprogram(vfs_node_t *parent, const char *name, void (*run)(int, char**));
vfs_node_t *vfs_find_child(vfs_node_t *dir, const char *name);
vfs_node_t *vfs_link(vfs_node_t *dest_dir, vfs_node_t *existing_node); /* installs a reference, not a copy */
void vfs_path(vfs_node_t *node, char *out, size_t outsz);

/* well-known directories, valid after vfs_init() */
extern vfs_node_t *vfs_dir_bin;
extern vfs_node_t *vfs_dir_etc;
extern vfs_node_t *vfs_dir_home;
extern vfs_node_t *vfs_dir_pkgs; /* /offline_packages */

#endif
