#include "../kernel/vga.h"
#include "../kernel/vpkg.h"
#include "../kernel/vfs.h"
#include "../kernel/string.h"
#include "../kernel/shell.h"
#include "../drivers/hid.h"
#include "../drivers/usb_uhci.h"
#include "../drivers/xhci.h"

static void pump(void) { hid_poll_all(); usb_uhci_poll(); xhci_poll(); }
static char blocking_getchar(void) { char c; while ((c = hid_getchar()) == 0) pump(); return c; }

static void se_run(int argc, char **argv) {
    if (argc < 2) { vga_puts("usage: se <filename>\n"); return; }

    vfs_node_t *cwd = sysvenus_cwd();
    vfs_node_t *n = vfs_find_child(cwd, argv[1]);

    if (n && n->type != VFS_FILE) {
        vga_puts("se: that name is a directory\n");
        return;
    }
    if (n && !n->wbuf) {
        vga_puts("se: that file is read-only (built-in) and can't be edited\n");
        return;
    }
    if (!n) {
        n = vfs_mkfile_writable(cwd, argv[1]);
        if (!n) { vga_puts("se: couldn't create file (out of write slots)\n"); return; }
    }

    vga_clear();
    vga_puts_col("SE - editing ", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_col(argv[1], VGA_LIGHT_CYAN, VGA_BLACK);
    vga_putc('\n');
    vga_puts_col(
        "Line editor: backspace works within the current line only.\n"
        "Press ESC when done, then y to save or n to discard.\n\n",
        VGA_DARK_GREY, VGA_BLACK);

    char buf[VFS_WRITE_BUF];
    size_t len = 0;
    if (n->wbuf[0]) {
        k_strcpy(buf, n->wbuf);
        len = k_strlen(buf);
        vga_puts(buf);
    } else {
        buf[0] = 0;
    }

    size_t line_floor = len;
    while (1) {
        char c = blocking_getchar();
        if (c == 27) break;

        if (c == '\n') {
            if (len < n->wcap - 1) { buf[len++] = '\n'; buf[len] = 0; }
            vga_putc('\n');
            line_floor = len;
            continue;
        }
        if (c == '\b') {
            if (len > line_floor) {
                len--; buf[len] = 0;
                vga_putc('\b'); vga_putc(' '); vga_putc('\b');
            }
            continue;
        }
        if (c >= 32 && c < 127 && len < n->wcap - 1) {
            buf[len++] = c; buf[len] = 0;
            vga_putc(c);
        }
    }

    vga_puts_col("\nsave changes? (y/n): ", VGA_LIGHT_CYAN, VGA_BLACK);
    char ans = blocking_getchar();
    vga_putc(ans);
    vga_putc('\n');
    if (ans == 'y' || ans == 'Y') {
        k_strcpy(n->wbuf, buf);
        vga_puts("saved.\n");
    } else {
        vga_puts("discarded.\n");
    }
}

void se_register(void) {
    vpkg_register("se", "basic line editor - se <filename> (ESC then y/n to save)", se_run);
}
