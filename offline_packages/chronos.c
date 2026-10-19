#include "../kernel/vga.h"
#include "../kernel/vpkg.h"
#include "../kernel/vfs.h"
#include "../kernel/string.h"
#include "../kernel/rtc.h"

#define HISTORY_MAX 512
#define ENTRY_SIZE 96

static char history[HISTORY_MAX];
static int history_len = 0;

static void chronos_log_command(const char *cmd) {
    rtc_time_t t;
    rtc_read(&t);

    char entry[ENTRY_SIZE];
    k_strcpy(entry, "[");
    char timebuf[32];
    k_itoa(t.hour, timebuf, 10); k_strcat(entry, timebuf); k_strcat(entry, ":");
    k_itoa(t.min, timebuf, 10); k_strcat(entry, timebuf); k_strcat(entry, ":");
    k_itoa(t.sec, timebuf, 10); k_strcat(entry, timebuf);
    k_strcat(entry, "] ");
    k_strcat(entry, cmd);
    k_strcat(entry, "\n");

    int entry_len = k_strlen(entry);
    if (history_len + entry_len >= HISTORY_MAX) {
        k_memset(history, 0, HISTORY_MAX);
        history_len = 0;
    }
    k_strcat(history, entry);
    history_len += entry_len;
}

static void chronos_run(int argc, char **argv) {
    (void)argc; (void)argv;
    if (history_len == 0) {
        vga_puts("chronos: no history yet\n");
        return;
    }
    vga_puts_col("=== Command History (Chronos) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts(history);
}

void chronos_log(const char *cmd) {
    if (history_len >= HISTORY_MAX - 100) return;
    chronos_log_command(cmd);
}

void chronos_register(void) {
    vpkg_register("chronos", "command history tracker with timestamps", chronos_run);
}
