#include "vga.h"
#include "shell.h"
#include "multiboot2.h"
#include "../include/types.h"
#include "../include/arch.h"
#include "debug.h"

#define MULTIBOOT2_MAGIC 0x36d76289u

mb2_info_t g_mb2; /* filled here, read by vga.c, vespfetch, and anything else that wants real boot info */

void kernel_main(u32 magic, u32 mb_info_addr) {
    arch_early_init();   /* x86_64: install exception handlers first, so any later fault is reported, not a silent reboot */
    dbg_puts("[boot] kernel_main(" VESPER_ARCH ")\n");

    /* mb2_parse() has to run before vga_init(): vga_init() reads g_mb2 to pick text mode vs a real
         * GOP/VBE framebuffer. wrong order = it always falls back to text mode */
    mb2_parse(mb_info_addr, &g_mb2);
    vga_init();

    if (magic != MULTIBOOT2_MAGIC) {
        vga_puts_col("warning: not booted via multiboot2 (bad magic)\n", VGA_YELLOW, VGA_BLACK);
    }

    if (g_mb2.has_fb && g_mb2.fb_type != 2 && !vga_is_framebuffer_mode()) {
        /* got a fb tag but can't use it (bpp, too small, or not RGB/EGA). the one case where display may really not work */
        vga_puts_col(
            "warning: bootloader gave a framebuffer this kernel can't use "
            "(unsupported bpp/size); display may not work.\n",
            VGA_YELLOW, VGA_BLACK);
    }

    sysvenus_main(); /* never returns */

    while (1) __asm__ volatile ("cli; hlt");
}
