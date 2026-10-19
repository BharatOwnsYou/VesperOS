#include "../include/mmio.h"
#include "vga.h"
#include "string.h"
#include "athenalibc.h"
#include "multiboot2.h"
#include "font8x16.h"
#include "../include/io.h"

#define VGA_MEM     ((volatile u16*)0xB8000)
#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define CHAR_W      8
#define CHAR_H      16

extern mb2_info_t g_mb2; /* filled by kernel_main() before vga_init() runs */

static int cur_row = 0, cur_col = 0;
static u8  cur_color = 0x0F; /* white on black, 8-bit attr byte */

/* set in vga_init() if grub gave us a usable RGB linear framebuffer instead of EGA/VGA text mode
 * (real UEFI/GOP, no CSM). the logical grid stays 80x25 either way, each cell maps to an 8x16 pixel block
 * in fb mode, so shell.c, OrionWM etc don't care which mode is active */
static bool fb_mode = false;
static bool vga_ready = false;
static u8  *fb_base;          /* mapped framebuffer (see mmio_map) */
static u32  fb_pitch;
static u8   fb_bpp;
static u8   fb_r_pos, fb_r_size, fb_g_pos, fb_g_size, fb_b_pos, fb_b_size;

/* standard 16-color VGA/CGA palette, RGB */
static const u8 vga_palette[16][3] = {
    {0,0,0}, {0,0,170}, {0,170,0}, {0,170,170},
    {170,0,0}, {170,0,170}, {170,85,0}, {170,170,170},
    {85,85,85}, {85,85,255}, {85,255,85}, {85,255,255},
    {255,85,85}, {255,85,255}, {255,255,85}, {255,255,255},
};

static inline u16 vga_entry(char c, u8 color) {
    return (u16)(u8)c | ((u16)color << 8);
}

static void vga_update_hw_cursor(void) {
    if (fb_mode) return; /* no legacy CRT controller to talk to under pure GOP */
    u16 pos = cur_row * VGA_WIDTH + cur_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

/* ---- framebuffer rendering ----
 *
 * a UEFI fb is video memory, usually uncached or write-combined. the old renderer scrolled by reading the picture
 * back OUT of video memory a byte at a time (reads there are very slow) and cleared one pixel per function call,
 * so every scroll or clear visibly swept down the screen.
 *
 * now the 80x25 grid lives in ordinary RAM (cells[]). changing a cell only marks it dirty, fb_flush() renders the
 * dirty column range of each row into a RAM scanline buffer and stores it with wide writes. the fb is never read
 * and a whole vga_puts() is flushed once however many lines it scrolled */

static u16 cells[VGA_HEIGHT][VGA_WIDTH];      /* character | (attribute << 8), the source of truth in fb mode */
static u8  dirty_c0[VGA_HEIGHT], dirty_c1[VGA_HEIGHT];   /* dirty column range [c0,c1) per row; c1 <= c0 means clean */
static u32 fb_pal[16];                         /* the 16 VGA colours packed for this framebuffer's pixel format */
static u32 linebuf[VGA_WIDTH * CHAR_W];        /* one rendered scanline of the grid */
static u8  linebuf24[VGA_WIDTH * CHAR_W * 3];

static u32 fb_pack_pixel(u8 r, u8 g, u8 b) {
    u32 rv = (fb_r_size >= 8) ? r : (u32)(r >> (8 - fb_r_size));
    u32 gv = (fb_g_size >= 8) ? g : (u32)(g >> (8 - fb_g_size));
    u32 bv = (fb_b_size >= 8) ? b : (u32)(b >> (8 - fb_b_size));
    return (rv << fb_r_pos) | (gv << fb_g_pos) | (bv << fb_b_pos);
}

static void cells_dirty(int row, int c0, int c1) {
    if (dirty_c1[row] <= dirty_c0[row]) { dirty_c0[row] = (u8)c0; dirty_c1[row] = (u8)c1; return; }
    if (c0 < dirty_c0[row]) dirty_c0[row] = (u8)c0;
    if (c1 > dirty_c1[row]) dirty_c1[row] = (u8)c1;
}

/* Store n dwords to the framebuffer with as few bus transactions as possible (never a read). */
static void fb_store32(u8 *dst, const u32 *src, u32 n) {
#if defined(__x86_64__) && !defined(HOST_TEST)
    u32 pairs = n >> 1;
    if (pairs) __asm__ volatile ("rep movsq" : "+D"(dst), "+S"(src), "+c"(pairs) :: "memory");
    if (n & 1) *(volatile u32 *)dst = *src;
#elif !defined(HOST_TEST)
    __asm__ volatile ("rep movsl" : "+D"(dst), "+S"(src), "+c"(n) :: "memory");
#else
    for (u32 i = 0; i < n; i++) ((u32 *)dst)[i] = src[i];
#endif
}

#ifdef HOST_TEST
unsigned vga_test_flushes;          /* tests/vgatest.c: how many times the framebuffer was actually repainted */
#endif

static void fb_flush(void) {
    if (!fb_mode) return;
    u32 bytes_pp = fb_bpp / 8;
#ifdef HOST_TEST
    bool painted = false;
#endif
    for (int row = 0; row < VGA_HEIGHT; row++) {
        int c0 = dirty_c0[row], c1 = dirty_c1[row];
        if (c1 <= c0) continue;
#ifdef HOST_TEST
        painted = true;
#endif
        u32 npx = (u32)(c1 - c0) * CHAR_W;
        for (int gy = 0; gy < CHAR_H; gy++) {
            u32 *p = linebuf;
            for (int c = c0; c < c1; c++) {
                u16 cell = cells[row][c];
                u8 ch = (u8)cell, attr = (u8)(cell >> 8);
                const u8 *glyph = (ch >= 32 && ch < 127) ? font8x16[ch - 32] : font8x16[0];   /* blank fallback */
                u8 bits = glyph[gy];
                u32 fg = fb_pal[attr & 0x0F], bg = fb_pal[(attr >> 4) & 0x0F];
                for (int x = 0; x < CHAR_W; x++) *p++ = (bits & (0x80 >> x)) ? fg : bg;
            }
            u8 *dst = fb_base + (uptr)(row * CHAR_H + gy) * fb_pitch + (uptr)c0 * CHAR_W * bytes_pp;
            if (fb_bpp == 32) {
                fb_store32(dst, linebuf, npx);
            } else {                                   /* 24 bpp: pack, then store the bytes */
                for (u32 i = 0; i < npx; i++) {
                    linebuf24[i * 3 + 0] = (u8)(linebuf[i] & 0xFF);
                    linebuf24[i * 3 + 1] = (u8)((linebuf[i] >> 8) & 0xFF);
                    linebuf24[i * 3 + 2] = (u8)((linebuf[i] >> 16) & 0xFF);
                }
                for (u32 i = 0; i < npx * 3; i++) dst[i] = linebuf24[i];
            }
        }
        dirty_c0[row] = dirty_c1[row] = 0;
    }
#ifdef HOST_TEST
    if (painted) vga_test_flushes++;
#endif
}

static void fb_clear_cells(u8 bg_idx) {
    u16 blank = (u16)(' ' | ((u16)(bg_idx << 4) << 8));      /* space; foreground irrelevant */
    for (int r = 0; r < VGA_HEIGHT; r++)
        for (int c = 0; c < VGA_WIDTH; c++) cells[r][c] = blank;
    for (int r = 0; r < VGA_HEIGHT; r++) { dirty_c0[r] = 0; dirty_c1[r] = VGA_WIDTH; }
}

static void fb_scroll_cells(u8 bg_idx) {
    for (int r = 1; r < VGA_HEIGHT; r++)
        for (int c = 0; c < VGA_WIDTH; c++) cells[r - 1][c] = cells[r][c];
    u16 blank = (u16)(' ' | ((u16)(bg_idx << 4) << 8));
    for (int c = 0; c < VGA_WIDTH; c++) cells[VGA_HEIGHT - 1][c] = blank;
    for (int r = 0; r < VGA_HEIGHT; r++) { dirty_c0[r] = 0; dirty_c1[r] = VGA_WIDTH; }
}

/* ---- public API - dispatches to text mode or framebuffer mode ---- */

void vga_init(void) {
    cur_row = 0; cur_col = 0;
    cur_color = 0x0F;

    fb_mode = false;
    if (g_mb2.has_fb && g_mb2.fb_type == 1 && (g_mb2.fb_bpp == 32 || g_mb2.fb_bpp == 24)
        && g_mb2.fb_width >= VGA_WIDTH * CHAR_W && g_mb2.fb_height >= VGA_HEIGHT * CHAR_H) {
        /* old code truncated the 64-bit fb address to 32 bits. map it properly. if firmware put it somewhere
         * this build can't reach (i686 + fb above 4G) fall back to text mode instead of scribbling over random memory */
        fb_base = (u8 *)mmio_map(g_mb2.fb_addr, (u64)g_mb2.fb_pitch * g_mb2.fb_height);
        if (fb_base) {
            fb_mode = true;
            fb_pitch  = g_mb2.fb_pitch;
            fb_bpp    = g_mb2.fb_bpp;
            fb_r_pos = g_mb2.fb_red_pos;   fb_r_size = g_mb2.fb_red_size;
            fb_g_pos = g_mb2.fb_green_pos; fb_g_size = g_mb2.fb_green_size;
            fb_b_pos = g_mb2.fb_blue_pos;  fb_b_size = g_mb2.fb_blue_size;
            for (int i = 0; i < 16; i++) fb_pal[i] = fb_pack_pixel(vga_palette[i][0], vga_palette[i][1], vga_palette[i][2]);
        }
    }

    vga_clear();
    vga_ready = true;
}

bool vga_is_initialized(void) { return vga_ready; }

bool vga_is_framebuffer_mode(void) { return fb_mode; }

void vga_clear(void) {
    if (fb_mode) {
        fb_clear_cells((cur_color >> 4) & 0x0F);
        fb_flush();
    } else {
        for (int y = 0; y < VGA_HEIGHT; y++)
            for (int x = 0; x < VGA_WIDTH; x++)
                VGA_MEM[y * VGA_WIDTH + x] = vga_entry(' ', cur_color);
    }
    cur_row = 0; cur_col = 0;
    vga_update_hw_cursor();
}

void vga_set_color(u8 fg, u8 bg) {
    cur_color = (u8)((bg << 4) | (fg & 0x0F));
}

void vga_set_cursor(int row, int col) {
    cur_row = row; cur_col = col;
    vga_update_hw_cursor();
}

/* Internal, no flush: lets one vga_puts() scroll several times and still paint the framebuffer only once. */
static void scroll_nofl(void) {
    if (cur_row < VGA_HEIGHT) return;
    if (fb_mode) {
        fb_scroll_cells((cur_color >> 4) & 0x0F);
    } else {
        for (int y = 1; y < VGA_HEIGHT; y++)
            for (int x = 0; x < VGA_WIDTH; x++)
                VGA_MEM[(y - 1) * VGA_WIDTH + x] = VGA_MEM[y * VGA_WIDTH + x];
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEM[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', cur_color);
    }
    cur_row = VGA_HEIGHT - 1;
}

void vga_scroll_if_needed(void) {
    scroll_nofl();
    fb_flush();
}

static void put_cell(int row, int col, char c) {
    if (fb_mode) {
        cells[row][col] = vga_entry(c, cur_color);
        cells_dirty(row, col, col + 1);
    } else {
        VGA_MEM[row * VGA_WIDTH + col] = vga_entry(c, cur_color);
    }
}

static void putc_nofl(char c) {
    if (c == '\n') {
        cur_col = 0; cur_row++;
    } else if (c == '\r') {
        cur_col = 0;
    } else if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            put_cell(cur_row, cur_col, ' ');
        }
    } else {
        put_cell(cur_row, cur_col, c);
        cur_col++;
        if (cur_col >= VGA_WIDTH) { cur_col = 0; cur_row++; }
    }
    scroll_nofl();
}

void vga_putc(char c) {
    putc_nofl(c);
    fb_flush();
    vga_update_hw_cursor();
}

void vga_puts(const char *s) {
    while (*s) putc_nofl(*s++);
    fb_flush();                          /* one framebuffer update per call, however many lines it scrolled */
    vga_update_hw_cursor();
}

void vga_puts_col(const char *s, u8 fg, u8 bg) {
    u8 saved = cur_color;
    vga_set_color(fg, bg);
    vga_puts(s);
    cur_color = saved;
}

void vga_put_hex(u32 val) {
    char buf[16];
    k_itoa(val, buf, 16);
    vga_puts("0x");
    vga_puts(buf);
}

void vga_put_dec(u32 val) {
    char buf[16];
    k_itoa(val, buf, 10);
    vga_puts(buf);
}
