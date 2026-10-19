#ifndef VESPEROS_VGA_H
#define VESPEROS_VGA_H
#include "../include/types.h"

enum vga_color {
    VGA_BLACK = 0, VGA_BLUE = 1, VGA_GREEN = 2, VGA_CYAN = 3,
    VGA_RED = 4, VGA_MAGENTA = 5, VGA_BROWN = 6, VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8, VGA_LIGHT_BLUE = 9, VGA_LIGHT_GREEN = 10, VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12, VGA_LIGHT_MAGENTA = 13, VGA_YELLOW = 14, VGA_WHITE = 15
};

void vga_init(void);
void vga_clear(void);
void vga_set_color(u8 fg, u8 bg);
void vga_putc(char c);
void vga_puts(const char *s);
void vga_puts_col(const char *s, u8 fg, u8 bg);
void vga_put_hex(u32 val);
void vga_put_dec(u32 val);
void vga_set_cursor(int row, int col);
void vga_scroll_if_needed(void);
bool vga_is_initialized(void);      /* has vga_init() completed? (the exception handler uses this) */
bool vga_is_framebuffer_mode(void); /* true if rendering via a real GOP/VBE pixel framebuffer instead of legacy 0xB8000 text mode */

#endif
