#include "../include/version.h"
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/errno.h"
#include "../kernel/vga.h"
#include "../kernel/vpkg.h"

static void posixtest_run(int argc, char **argv) {
    (void)argc; (void)argv;
    vga_puts_col("=== AthenaLibC POSIX-layer self-test ===\n\n", VGA_LIGHT_CYAN, VGA_BLACK);

    vga_puts("[1] malloc(64) ... ");
    char *buf = (char*)malloc(64);
    if (!buf) { vga_puts_col("FAIL (NULL)\n", VGA_LIGHT_RED, VGA_BLACK); return; }
    for (int i = 0; i < 63; i++) buf[i] = (char)('A' + (i % 26));
    buf[63] = 0;
    vga_puts_col("ok\n", VGA_LIGHT_GREEN, VGA_BLACK);

    vga_puts("[2] fopen(\"posixtest.txt\", \"w\") + fprintf ... ");
    FILE *f = fopen("posixtest.txt", "w");
    if (!f) { vga_puts_col("FAIL\n", VGA_LIGHT_RED, VGA_BLACK); free(buf); return; }
    fprintf(f, "hello from AthenaLibC, x=%d, name=%s\n", 42, VESPER_HOSTNAME);
    fclose(f);
    vga_puts_col("ok\n", VGA_LIGHT_GREEN, VGA_BLACK);

    vga_puts("[3] fopen(\"posixtest.txt\", \"r\") + fgets, verify ... ");
    FILE *f2 = fopen("posixtest.txt", "r");
    if (!f2) { vga_puts_col("FAIL\n", VGA_LIGHT_RED, VGA_BLACK); free(buf); return; }
    char line[128];
    fgets(line, sizeof(line), f2);
    fclose(f2);

    const char *expected = "hello from AthenaLibC, x=42, name=" VESPER_HOSTNAME "\n";
    int match = 1;
    for (int i = 0; expected[i]; i++) if (line[i] != expected[i]) { match = 0; break; }
    if (match) vga_puts_col("ok\n", VGA_LIGHT_GREEN, VGA_BLACK);
    else { vga_puts_col("FAIL\n", VGA_LIGHT_RED, VGA_BLACK); vga_puts("  got: "); vga_puts(line); }

    vga_puts("  content read back: ");
    vga_puts(line);

    vga_puts("[4] printf(\"...\") + errno on missing file ... ");
    FILE *missing = fopen("does_not_exist.txt", "r");
    if (missing == NULL && errno == ENOENT) vga_puts_col("ok\n", VGA_LIGHT_GREEN, VGA_BLACK);
    else vga_puts_col("FAIL\n", VGA_LIGHT_RED, VGA_BLACK);

    printf("  printf() itself works: 2+2=%d, hex(255)=%x\n", 2 + 2, 255);

    free(buf);
    vga_puts_col("\nall tests complete.\n", VGA_LIGHT_CYAN, VGA_BLACK);
}

void posixtest_register(void) {
    vpkg_register("posixtest", "AthenaLibC POSIX-layer self-test (malloc/fopen/fprintf/errno)", posixtest_run);
}
