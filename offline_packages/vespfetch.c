#include "../include/version.h"
#include "../kernel/vga.h"
#include "../kernel/vpkg.h"
#include "../kernel/vfs.h"
#include "../kernel/athenalibc.h"
#include "../kernel/cpuid.h"
#include "../kernel/multiboot2.h"
#include "../drivers/hid.h"

extern mb2_info_t g_mb2;

static const char *star_art[10] = {
    "        _..._        ",
    "      .'     `.      ",
    "     /  \\   /  \\     ",
    "    |    \\ /    |    ",
    "    |     *     |    ",
    "     \\   / \\   /     ",
    "      `./   \\.'      ",
    "       /     \\       ",
    "     .'       `.     ",
    "    /           \\    ",
};

static int installed_count = 0;
static void count_installed(const char *n, const char *d, int inst) {
    (void)n; (void)d;
    if (inst) installed_count++;
}

static void vespfetch_run(int argc, char **argv) {
    (void)argc; (void)argv;

    char vendor[13];
    cpuid_vendor(vendor);
    char brand[49];
    cpuid_brand(brand);

    char cpuline[80];
    k_strcpy(cpuline, "CPU: ");
    k_strcat(cpuline, vendor);
    if (brand[0]) { k_strcat(cpuline, " "); k_strcat(cpuline, brand); }

    installed_count = 0;
    vpkg_list(count_installed);
    char pkgline[48];
    char num[12];
    k_itoa((u32)installed_count, num, 10);
    k_strcpy(pkgline, "Packages installed: ");
    k_strcat(pkgline, num);

    char hidline[32];
    k_itoa((u32)hid_device_count(), num, 10);
    k_strcpy(hidline, "HID devices active: ");
    k_strcat(hidline, num);

    char memline[64];
    if (g_mb2.has_mmap && g_mb2.total_ram_bytes > 0) {
        u32 mb = (u32)(g_mb2.total_ram_bytes / (1024 * 1024));
        if (mb >= 1024) {
            u32 gb_whole = mb / 1024;
            u32 gb_tenths = ((mb % 1024) * 10) / 1024;
            char gbbuf[16];
            k_snprintf(gbbuf, sizeof(gbbuf), "%u.%u", gb_whole, gb_tenths);
            k_snprintf(memline, sizeof(memline), "Memory: ~%s GB (full mb2 memory map)", gbbuf);
        } else {
            k_snprintf(memline, sizeof(memline), "Memory: ~%u MB (full mb2 memory map)", mb);
        }
    } else if (g_mb2.has_mem) {
        k_snprintf(memline, sizeof(memline), "Memory: ~%u MB detected (basic mb2 tag only)",
                   (g_mb2.mem_upper_kb / 1024) + 1);
    } else {
        k_strcpy(memline, "Memory: unknown (no multiboot2 mem tag)");
    }

    const char *info[10] = {
        VESPER_OS_FULLNAME,
        "Kernel: " VESPER_KERNEL_TAG " (" VESPER_CODENAME ")",
        "Boot: GRUB / multiboot2 (BIOS + UEFI)",
        cpuline,
        "Display: framebuffer / VGA text, 8-bit color",
        "Shell: SysVenus (custom)",
        pkgline,
        hidline,
        memline,
        "",
    };

    vga_putc('\n');
    for (int row = 0; row < 10; row++) {
        u8 col = (row % 2 == 0) ? VGA_LIGHT_BLUE : VGA_LIGHT_MAGENTA;
        vga_puts_col(star_art[row], col, VGA_BLACK);
        vga_puts("   ");
        if (info[row][0]) vga_puts_col(info[row], VGA_WHITE, VGA_BLACK);
        vga_putc('\n');
    }
    vga_putc('\n');
}

void vespfetch_register(void) {
    vpkg_register("vespfetch", "fastfetch-style system info, blue/purple star logo", vespfetch_run);
}
