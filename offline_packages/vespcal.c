#include "../kernel/vga.h"
#include "../kernel/vpkg.h"
#include "../kernel/rtc.h"
#include "../kernel/string.h"

static const char *month_names[13] = {
    "", "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static int days_in_month(int month, int year) {
    static const int d[13] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return d[month];
}

static int first_weekday(int month, int year) {
    int m = month, y = year;
    if (m < 3) { m += 12; y -= 1; }
    int k = y % 100, j = y / 100;
    int h = (1 + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return (h + 6) % 7;
}

static void vespcal_run(int argc, char **argv) {
    (void)argc; (void)argv;
    rtc_time_t t;
    rtc_read(&t);

    char header[32];
    k_strcpy(header, month_names[t.month]);
    k_strcat(header, " ");
    char ynum[8]; k_itoa(t.year, ynum, 10);
    k_strcat(header, ynum);

    vga_putc('\n');
    vga_puts_col(header, VGA_LIGHT_CYAN, VGA_BLACK);
    vga_putc('\n');
    vga_puts_col("Su Mo Tu We Th Fr Sa\n", VGA_LIGHT_GREY, VGA_BLACK);

    int start = first_weekday(t.month, t.year);
    int total = days_in_month(t.month, t.year);
    int col = 0;
    for (int i = 0; i < start; i++) { vga_puts("   "); col++; }
    for (int day = 1; day <= total; day++) {
        char buf[4];
        if (day < 10) { buf[0] = ' '; buf[1] = (char)('0' + day); buf[2] = ' '; buf[3] = 0; }
        else { k_itoa((u32)day, buf, 10); k_strcat(buf, " "); }

        if (day == t.day) vga_puts_col(buf, VGA_BLACK, VGA_LIGHT_CYAN);
        else vga_puts(buf);

        col++;
        if (col == 7) { vga_putc('\n'); col = 0; }
    }
    if (col != 0) vga_putc('\n');
    vga_putc('\n');
}

void vespcal_register(void) {
    vpkg_register("vespcal", "cal-style month calendar (reads CMOS RTC)", vespcal_run);
}
