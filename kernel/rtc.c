#include "rtc.h"
#include "../include/io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static u8 cmos_read(u8 reg) {
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static u8 bcd2bin(u8 v) { return (u8)((v & 0x0F) + ((v >> 4) * 10)); }

void rtc_read(rtc_time_t *out) {
    while (update_in_progress()) io_wait();

    u8 sec = cmos_read(0x00);
    u8 min = cmos_read(0x02);
    u8 hour = cmos_read(0x04);
    u8 day = cmos_read(0x07);
    u8 month = cmos_read(0x08);
    u8 year = cmos_read(0x09);
    u8 statusB = cmos_read(0x0B);

    if (!(statusB & 0x04)) { /* BCD mode */
        sec = bcd2bin(sec); min = bcd2bin(min);
        hour = bcd2bin(hour & 0x7F) | (hour & 0x80);
        day = bcd2bin(day); month = bcd2bin(month); year = bcd2bin(year);
    }
    if (!(statusB & 0x02) && (hour & 0x80)) { /* 12-hour mode, PM bit set */
        hour = (u8)(((hour & 0x7F) + 12) % 24);
    }

    out->sec = sec; out->min = min; out->hour = hour;
    out->day = day; out->month = month;
    out->year = (u16)(2000 + year); /* fine until 2100, no century register handling */
}
