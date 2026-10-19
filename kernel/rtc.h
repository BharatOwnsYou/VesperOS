#ifndef VESPEROS_RTC_H
#define VESPEROS_RTC_H
#include "../include/types.h"

typedef struct { u8 sec, min, hour, day, month; u16 year; } rtc_time_t;

void rtc_read(rtc_time_t *out); /* reads the CMOS RTC, handles BCD conversion */

#endif
