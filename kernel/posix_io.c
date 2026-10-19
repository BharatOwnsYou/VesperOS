#include "../include/unistd.h"
#include "../include/errno.h"
#include "vfs.h"
#include "shell.h"
#include "vga.h"
#include "athenalibc.h"
#include "../drivers/hid.h"

#define MAX_FDS 16

typedef struct {
    bool in_use;
    vfs_node_t *node;
    u32 pos;
} fd_entry_t;

static fd_entry_t fds[MAX_FDS];
static bool fds_initialized = false;

static void ensure_reserved(void) {
    if (fds_initialized) return;
    fds[0].in_use = true; fds[0].node = NULL; fds[0].pos = 0;
    fds[1].in_use = true; fds[1].node = NULL; fds[1].pos = 0;
    fds[2].in_use = true; fds[2].node = NULL; fds[2].pos = 0;
    fds_initialized = true;
}

int open(const char *path, int flags) {
    ensure_reserved();
    vfs_node_t *cwd = sysvenus_cwd();
    vfs_node_t *n = vfs_find_child(cwd, path);

    if (!n) {
        if (!(flags & O_CREAT)) { errno = ENOENT; return -1; }
        n = vfs_mkfile_writable(cwd, path);
        if (!n) { errno = ENOSPC; return -1; }
    }
    if (n->type != VFS_FILE) { errno = EISDIR; return -1; }
    if (!n->wbuf) { errno = EACCES; return -1; }

    for (int i = 3; i < MAX_FDS; i++) {
        if (!fds[i].in_use) {
            fds[i].in_use = true;
            fds[i].node = n;
            fds[i].pos = (flags & O_APPEND) ? (u32)k_strlen(n->wbuf) : 0;
            if (flags & O_TRUNC) { n->wbuf[0] = 0; fds[i].pos = 0; }
            return i;
        }
    }
    errno = ENOSPC;
    return -1;
}

ssize_t read(int fd, void *buf, size_t count) {
    ensure_reserved();
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].in_use) { errno = EBADF; return -1; }

    if (fd == STDIN_FILENO) {
        char *out = (char*)buf;
        size_t n = 0;
        while (n < count) {
            char c;
            do { hid_poll_all(); c = hid_getchar(); } while (c == 0);
            out[n++] = c;
            if (c == '\n') break;
        }
        return (ssize_t)n;
    }

    fd_entry_t *e = &fds[fd];
    if (!e->node || !e->node->wbuf) { errno = EBADF; return -1; }
    size_t len = k_strlen(e->node->wbuf);
    if (e->pos >= len) return 0;
    size_t avail = len - e->pos;
    size_t n = (count < avail) ? count : avail;
    k_memcpy(buf, e->node->wbuf + e->pos, n);
    e->pos += (u32)n;
    return (ssize_t)n;
}

ssize_t write(int fd, const void *buf, size_t count) {
    ensure_reserved();
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].in_use) { errno = EBADF; return -1; }

    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        const char *p = (const char*)buf;
        for (size_t i = 0; i < count; i++) vga_putc(p[i]);
        return (ssize_t)count;
    }

    fd_entry_t *e = &fds[fd];
    if (!e->node || !e->node->wbuf) { errno = EBADF; return -1; }
    if (e->pos + 1 >= e->node->wcap) { errno = ENOSPC; return 0; }
    size_t remaining_cap = e->node->wcap - 1 - e->pos;
    size_t n = (count < remaining_cap) ? count : remaining_cap;
    k_memcpy(e->node->wbuf + e->pos, buf, n);
    e->pos += (u32)n;
    if (e->pos > k_strlen(e->node->wbuf)) e->node->wbuf[e->pos] = 0;
    return (ssize_t)n;
}

int close(int fd) {
    ensure_reserved();
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].in_use) { errno = EBADF; return -1; }
    if (fd >= 3) { fds[fd].in_use = false; fds[fd].node = NULL; fds[fd].pos = 0; }
    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    ensure_reserved();
    if (fd < 3 || fd >= MAX_FDS || !fds[fd].in_use || !fds[fd].node) { errno = EBADF; return -1; }
    fd_entry_t *e = &fds[fd];
    s32 base;
    s32 len = (s32)k_strlen(e->node->wbuf);
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (s32)e->pos;
    else if (whence == SEEK_END) base = len;
    else { errno = EINVAL; return -1; }

    s32 newpos = base + offset;
    if (newpos < 0) { errno = EINVAL; return -1; }
    e->pos = (u32)newpos;
    return newpos;
}
