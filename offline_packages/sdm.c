#include "../kernel/vga.h"
#include "../kernel/vpkg.h"
#include "../kernel/string.h"

void sdm_run(int argc, char **argv) {
    (void)argc; (void)argv;

    vga_clear();
    vga_puts_col("=== Solar Display Manager (SDM) ===\n\n", VGA_LIGHT_MAGENTA, VGA_BLACK);
    vga_puts("Checking dependencies...\n");

    if (vpkg_install("orionwm") == 1) {
        vga_puts_col("[ok] ", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("OrionWM installed\n");
    } else if (vpkg_install("orionwm") == 0) {
        vga_puts_col("[ok] ", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("OrionWM already installed\n");
    } else {
        vga_puts_col("[err] ", VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("OrionWM not available\n");
        return;
    }

    vga_putc('\n');
    vga_puts_col("SDM ready. Launch the window manager with: ", VGA_WHITE, VGA_BLACK);
    vga_puts_col("orionwm\n", VGA_LIGHT_CYAN, VGA_BLACK);
}

void sdm_register(void) {
    vpkg_register("sdm", "Solar Display Manager (installs and launches OrionWM)", sdm_run);
}
