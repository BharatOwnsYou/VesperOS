/* host test for the framebuffer text renderer (kernel/vga.c).
 *
 * the renderer got rewritten (RAM cell grid + batched wide stores) to get rid of the slow "screen wipe".
 * this checks the new code makes EXACTLY the same pixels as the old per-pixel renderer (kept below as ref_*)
 * over thousands of random ops at 32 and 24 bpp, and never touches memory outside the 640x400 console area.
 * build/run: make test-vga */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/vga.h"
#include "../kernel/multiboot2.h"
#include "../include/mmio.h"

mb2_info_t g_mb2;
volatile void *mmio_map(u64 phys, u64 len) { (void)len; return (volatile void *)(uintptr_t)phys; }
extern unsigned vga_test_flushes;

static int n_pass = 0, n_fail = 0;
static void check(int ok, const char *what) { if (ok) n_pass++; else { n_fail++; printf("  FAIL: %s\n", what); } }
#define CHECK(c) check((c), #c)

/* ================= reference: the OLD renderer, copied verbatim in behaviour ================= */
#include "../kernel/font8x16.h"
#define W 80
#define H 25
#define CW 8
#define CH 16
static const u8 pal[16][3] = {
    {0,0,0}, {0,0,170}, {0,170,0}, {0,170,170}, {170,0,0}, {170,0,170}, {170,85,0}, {170,170,170},
    {85,85,85}, {85,85,255}, {85,255,85}, {85,255,255}, {255,85,85}, {255,85,255}, {255,255,85}, {255,255,255},
};
static u8 *rb; static u32 rpitch; static u8 rbpp;
static int rrow, rcol; static u8 rcolor;
static u32 rpack(u8 r, u8 g, u8 b) { return ((u32)r << 16) | ((u32)g << 8) | b; }          /* R=16 G=8 B=0, 8 bits each */
static void rput(u32 x, u32 y, u32 c) {
    u8 *row = rb + (size_t)y * rpitch;
    if (rbpp == 32) *(u32 *)(row + x * 4) = c;
    else { u8 *p = row + x * 3; p[0] = c & 0xFF; p[1] = (c >> 8) & 0xFF; p[2] = (c >> 16) & 0xFF; }
}
static void rblit(int col, int row, char ch, u8 fg, u8 bg) {
    u32 f = rpack(pal[fg][0], pal[fg][1], pal[fg][2]), b = rpack(pal[bg][0], pal[bg][1], pal[bg][2]);
    const u8 *g = (ch >= 32 && ch < 127) ? font8x16[(u8)ch - 32] : font8x16[0];
    for (int y = 0; y < CH; y++) for (int x = 0; x < CW; x++)
        rput((u32)(col * CW + x), (u32)(row * CH + y), (g[y] & (0x80 >> x)) ? f : b);
}
static void rclear_all(u8 bg) {
    u32 b = rpack(pal[bg][0], pal[bg][1], pal[bg][2]);
    for (u32 y = 0; y < H * CH; y++) for (u32 x = 0; x < W * CW; x++) rput(x, y, b);
}
static void rscroll(u8 bg) {
    u32 bpp = rbpp / 8;
    for (u32 y = CH; y < H * CH; y++) memmove(rb + (size_t)(y - CH) * rpitch, rb + (size_t)y * rpitch, W * CW * bpp);
    u32 b = rpack(pal[bg][0], pal[bg][1], pal[bg][2]);
    for (u32 y = H * CH - CH; y < H * CH; y++) for (u32 x = 0; x < W * CW; x++) rput(x, y, b);
}
static void rscroll_if(void) { if (rrow < H) return; rscroll((rcolor >> 4) & 0xF); rrow = H - 1; }
static void rputc(char c) {
    if (c == '\n') { rcol = 0; rrow++; }
    else if (c == '\r') rcol = 0;
    else if (c == '\b') { if (rcol > 0) { rcol--; rblit(rcol, rrow, ' ', rcolor & 0xF, (rcolor >> 4) & 0xF); } }
    else { rblit(rcol, rrow, c, rcolor & 0xF, (rcolor >> 4) & 0xF); rcol++; if (rcol >= W) { rcol = 0; rrow++; } }
    rscroll_if();
}
static void rputs(const char *s) { while (*s) rputc(*s++); }
static void rclear(void) { rclear_all((rcolor >> 4) & 0xF); rrow = rcol = 0; }

/* ================= harness ================= */
static unsigned seed = 2026;
static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFF; }

#define FBW 1024
#define FBH 768
static u8 *fb_new, *fb_ref;

static void setup(int bpp, u32 pitch) {
    free(fb_new); free(fb_ref);
    size_t sz = (size_t)pitch * FBH;
    fb_new = malloc(sz); fb_ref = malloc(sz);
    memset(fb_new, 0xAB, sz); memset(fb_ref, 0xAB, sz);        /* sentinel: anything outside the console must stay 0xAB */
    memset(&g_mb2, 0, sizeof g_mb2);
    g_mb2.has_fb = true; g_mb2.fb_type = 1; g_mb2.fb_bpp = (u8)bpp; g_mb2.fb_addr = (u64)(uintptr_t)fb_new;
    g_mb2.fb_pitch = pitch; g_mb2.fb_width = FBW; g_mb2.fb_height = FBH;
    g_mb2.fb_red_pos = 16; g_mb2.fb_red_size = 8; g_mb2.fb_green_pos = 8; g_mb2.fb_green_size = 8;
    g_mb2.fb_blue_pos = 0; g_mb2.fb_blue_size = 8;
    rb = fb_ref; rpitch = pitch; rbpp = (u8)bpp; rcolor = 0x0F; rrow = rcol = 0;
    vga_init();
    rclear_all(0);                                              /* vga_init() clears the console; mirror it */
}

static int same(void) { return memcmp(fb_new, fb_ref, (size_t)g_mb2.fb_pitch * FBH) == 0; }

static void run_random(int bpp, u32 pitch, int ops) {
    setup(bpp, pitch);
    CHECK(vga_is_framebuffer_mode());
    CHECK(same());
    int bad = 0;
    for (int i = 0; i < ops && !bad; i++) {
        unsigned k = rnd() % 100;
        char buf[260];
        if (k < 35) {                                              /* single printable character */
            char c = (char)(32 + rnd() % 95); vga_putc(c); rputc(c);
        } else if (k < 45) { vga_putc('\n'); rputc('\n'); }
        else if (k < 50) { vga_putc('\b'); rputc('\b'); }
        else if (k < 53) { vga_putc('\r'); rputc('\r'); }
        else if (k < 58) {                                         /* odd characters: controls, high bytes */
            char c = (char)(rnd() % 256); if (c == '\n' || c == '\r' || c == '\b') c = 1;
            vga_putc(c); rputc(c);
        } else if (k < 80) {                                       /* a string with newlines, sometimes long */
            int n = (int)(rnd() % (k < 70 ? 40 : 250));
            for (int j = 0; j < n; j++) { unsigned r = rnd() % 14; buf[j] = r == 0 ? '\n' : (char)(32 + rnd() % 95); }
            buf[n] = 0; vga_puts(buf); rputs(buf);
        } else if (k < 88) {                                       /* coloured text: puts_col restores the colour */
            u8 fg = (u8)(rnd() % 16), bg = (u8)(rnd() % 16);
            int n = (int)(rnd() % 90);
            for (int j = 0; j < n; j++) buf[j] = (rnd() % 12 == 0) ? '\n' : (char)(32 + rnd() % 95);
            buf[n] = 0;
            vga_puts_col(buf, fg, bg);
            u8 saved = rcolor; rcolor = (u8)((bg << 4) | (fg & 0xF)); rputs(buf); rcolor = saved;
        } else if (k < 92) {                                       /* change the current colour */
            u8 fg = (u8)(rnd() % 16), bg = (u8)(rnd() % 16);
            vga_set_color(fg, bg); rcolor = (u8)((bg << 4) | (fg & 0xF));
        } else if (k < 95) { vga_clear(); rclear(); }
        else if (k < 98) {                                         /* jump the cursor anywhere on the grid, like OrionWM */
            int r = (int)(rnd() % H), c = (int)(rnd() % W);
            vga_set_cursor(r, c); rrow = r; rcol = c;
        } else { vga_scroll_if_needed(); rscroll_if(); }
        if (!same()) { bad = 1; printf("  mismatch after op %d (kind %u)\n", i, k); }
    }
    char what[96]; snprintf(what, sizeof what, "%d random operations at %d bpp, pitch %u: framebuffer identical to the old renderer", ops, bpp, pitch);
    check(!bad, what);
}

static void test_outside_untouched(void) {
    printf("[bounds] nothing outside the 640x400 console area is ever written\n");
    setup(32, FBW * 4);
    for (int i = 0; i < 500; i++) { vga_puts("outside check line with some text 0123456789\n"); }
    vga_clear(); vga_puts("end");
    int dirty = 0;
    for (u32 y = 0; y < FBH && !dirty; y++)
        for (u32 x = 0; x < (u32)g_mb2.fb_pitch && !dirty; x++) {
            int inside = (y < H * CH) && (x < W * CW * 4);
            if (!inside && fb_new[(size_t)y * g_mb2.fb_pitch + x] != 0xAB) dirty = 1;
        }
    check(!dirty, "every byte outside the console rectangle still holds its sentinel value");
}

static void test_batching(void) {
    printf("[flush] a whole vga_puts() repaints the framebuffer once, however much it scrolls\n");
    setup(32, FBW * 4);
    for (int i = 0; i < H + 2; i++) vga_puts("fill the screen so the next lines scroll\n");
    char big[4000]; big[0] = 0;
    for (int i = 0; i < 60; i++) strcat(big, "a line of output that will force a scroll\n");
    unsigned before = vga_test_flushes;
    vga_puts(big);                                              /* 60 lines => 60 scrolls inside ONE call */
    check(vga_test_flushes - before == 1, "60 scrolled lines in one vga_puts() cause exactly 1 framebuffer repaint");
    before = vga_test_flushes;
    for (int i = 0; i < 60; i++) vga_puts("one line per call\n");
    check(vga_test_flushes - before == 60, "60 separate calls repaint 60 times (output appears immediately, nothing is deferred)");
    before = vga_test_flushes;
    vga_putc('x');                                              /* typing echo: only the dirty cell is rendered */
    check(vga_test_flushes - before == 1, "a typed character repaints once");
    before = vga_test_flushes;
    vga_set_cursor(3, 4); vga_set_color(7, 0);
    check(vga_test_flushes == before, "moving the cursor / changing colour does not repaint anything");
}

int main(void) {
    printf("[pixels] new renderer vs the old per-pixel renderer\n");
    run_random(32, FBW * 4, 6000);
    run_random(32, FBW * 4 + 256, 3000);                        /* pitch with padding at the end of each scanline */
    run_random(24, FBW * 3 + 64, 3000);
    test_outside_untouched();
    test_batching();
    printf("\n%d checks passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
