#include "../kernel/vga.h"
#include "../kernel/vpkg.h"
#include "../drivers/hid.h"
#include "../include/io.h"

#define MX_COLS 80
#define MX_ROWS 25

static int col_pos[MX_COLS];
static int col_speed[MX_COLS];
static u32 rng_state = 0x2f6a51c3;

static u32 xorshift(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static char rand_glyph(void) {
    static const char glyphs[] = "01ABCDEFGHIJKLMNOPQRSTUVWXYZ$%#@*+-<>/\\";
    return glyphs[xorshift() % (sizeof(glyphs) - 1)];
}

static void busy_delay(void) {
    for (volatile int i = 0; i < 400000; i++) io_wait();
}

static void vesmatrix_run(int argc, char **argv) {
    (void)argc; (void)argv;

    for (int c = 0; c < MX_COLS; c++) {
        col_pos[c] = (int)(xorshift() % MX_ROWS);
        col_speed[c] = 1 + (int)(xorshift() % 2);
    }

    vga_clear();
    vga_puts_col("vesmatrix - press any key to exit", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_putc('\n');

    while (1) {
        hid_poll_all();
        if (hid_getchar()) break;

        for (int c = 0; c < MX_COLS; c++) {
            int row = col_pos[c];
            if (row < MX_ROWS) {
                vga_set_cursor(row, c);
                u8 color = (xorshift() % 6 == 0) ? VGA_WHITE : VGA_LIGHT_GREEN;
                char cell[2] = { rand_glyph(), 0 };
                vga_puts_col(cell, color, VGA_BLACK);
            }
            int trail = row - 6;
            if (trail >= 0 && trail < MX_ROWS) {
                vga_set_cursor(trail, c);
                vga_puts_col(" ", VGA_BLACK, VGA_BLACK);
            }
            col_pos[c] += col_speed[c];
            if (col_pos[c] > MX_ROWS + 6) {
                col_pos[c] = 0;
                col_speed[c] = 1 + (int)(xorshift() % 2);
            }
        }
        busy_delay();
    }

    vga_clear();
}

void vesmatrix_register(void) {
    vpkg_register("vesmatrix", "cmatrix-style falling character rain", vesmatrix_run);
}
