#include "../kernel/vga.h"
#include "../kernel/vpkg.h"
#include "../kernel/vfs.h"
#include "../kernel/string.h"

#define NOTE_MAX 256
static char note_buf[NOTE_MAX];

static void vespnote_run(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("usage: vespnote write <text...>   |   vespnote read\n");
        return;
    }
    if (k_streq(argv[1], "write")) {
        note_buf[0] = 0;
        for (int i = 2; i < argc; i++) {
            k_strcat(note_buf, argv[i]);
            if (i < argc - 1) k_strcat(note_buf, " ");
        }
        k_strcat(note_buf, "\n");
        if (!vfs_find_child(vfs_dir_home, "note.txt"))
            vfs_mkfile(vfs_dir_home, "note.txt", note_buf);
        vga_puts("saved to /home/note.txt\n");
    } else if (k_streq(argv[1], "read")) {
        if (note_buf[0]) vga_puts(note_buf);
        else vga_puts("(no note yet - try: vespnote write hello world)\n");
    } else {
        vga_puts("usage: vespnote write <text...>   |   vespnote read\n");
    }
}

void vespnote_register(void) {
    vpkg_register("vespnote", "tiny sticky-note CLI backed by /home/note.txt", vespnote_run);
}
