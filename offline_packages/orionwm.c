#include "../kernel/vga.h"
#include "../kernel/vpkg.h"
#include "../drivers/hid.h"

static const char BLOCK_MID = (char)0xB1;

static void draw_background(void) {
    for (int y = 0; y < 25; y++) {
        for (int x = 0; x < 80; x++) {
            u8 color;
            if ((x + y) % 3 == 0) color = VGA_LIGHT_MAGENTA;
            else if ((x + y) % 3 == 1) color = VGA_LIGHT_BLUE;
            else color = VGA_MAGENTA;

            vga_set_cursor(y, x);
            vga_puts_col(" ", color & 0x0F, (color >> 4) & 0x0F);
        }
    }
}

static void draw_taskbar(void) {
    vga_set_cursor(0, 0);
    vga_puts_col("[ OrionWM ]  Press 'T' for Terminal  ESC=Exit",
                 VGA_WHITE, VGA_LIGHT_BLUE);
}

static void draw_terminal_window(void) {
    int x_start = 10, y_start = 2;
    int width = 60, height = 20;

    for (int y = y_start; y < y_start + height; y++) {
        for (int x = x_start; x < x_start + width; x++) {
            vga_set_cursor(y, x);
            char c[2] = { ' ', 0 };
            if (y == y_start || y == y_start + height - 1) c[0] = BLOCK_MID;
            else if (x == x_start || x == x_start + width - 1) c[0] = BLOCK_MID;
            vga_puts_col(c, VGA_WHITE, VGA_LIGHT_MAGENTA);
        }
    }

    vga_set_cursor(y_start, x_start + 2);
    vga_puts_col("[ OrionWM Terminal ]", VGA_BLACK, VGA_LIGHT_CYAN);

    vga_set_cursor(y_start + 2, x_start + 2);
    vga_puts_col("orion $ ", VGA_LIGHT_GREEN, VGA_LIGHT_MAGENTA);
}

void orionwm_run(int argc, char **argv) {
    (void)argc; (void)argv;

    vga_clear();
    draw_background();
    draw_taskbar();

    int terminal_active = 0;

    vga_set_cursor(10, 20);
    vga_puts_col("Welcome to OrionWM", VGA_WHITE, VGA_LIGHT_MAGENTA);

    while (1) {
        hid_poll_all();

        char c = hid_getchar();
        if (c == 0) continue;

        if (c == 27) break;

        if (c == 't' || c == 'T') {
            if (!terminal_active) {
                draw_terminal_window();
                vga_set_cursor(12, 12);
                vga_puts_col("Terminal started. Press ESC to exit WM.",
                             VGA_WHITE, VGA_LIGHT_MAGENTA);
                terminal_active = 1;
            }
        }
    }

    vga_clear();
    vga_puts_col("Exiting OrionWM...\n", VGA_LIGHT_CYAN, VGA_BLACK);
}

void orionwm_register(void) {
    vpkg_register("orionwm", "Orion Window Manager (text-mode desktop)", orionwm_run);
}
