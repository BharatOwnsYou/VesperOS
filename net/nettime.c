/* ms clock for the net stack, on PIT channel 0.
 *
 * no interrupts so there's no tick counter to read. channel 0 runs as a free-running 16-bit down counter
 * (mode 2, reload 65536) and the elapsed counts get accumulated whenever somebody asks for the time.
 * it wraps every ~55 ms so time only advances right if it's read at least that often, which holds at the shell
 * prompt and in every blocking net call. anything that never reads it (a long busy-loop animation) makes the
 * clock lose time, lease timers just run late */
#include "netutil.h"
#include "../include/io.h"

#define PIT_HZ_PER_MS 1193u      /* 1193182 Hz / 1000, error 0.015% */

static bool inited = false;
static u16  last_count;
static u32  ms_total = 1000;
static u32  frac;

static u16 pit_read(void) {
    outb(0x43, 0x00);            /* latch channel 0 */
    u8 lo = inb(0x40);
    u8 hi = inb(0x40);
    return (u16)(((u16)hi << 8) | lo);
}

u32 net_time_ms(void) {
    if (!inited) {
        outb(0x43, 0x34);        /* ch0, lobyte/hibyte, mode 2, binary */
        outb(0x40, 0x00);
        outb(0x40, 0x00);        /* reload = 65536 */
        last_count = pit_read();
        inited = true;
    }
    u16 now = pit_read();
    u16 delta = (u16)(last_count - now);   /* counts DOWN; modular subtraction handles the wrap */
    last_count = now;
    frac += delta;
    ms_total += frac / PIT_HZ_PER_MS;
    frac %= PIT_HZ_PER_MS;
    return ms_total;
}

void net_delay_ms(u32 ms) {
    u32 t0 = net_time_ms();
    while ((u32)(net_time_ms() - t0) < ms) { }
}
