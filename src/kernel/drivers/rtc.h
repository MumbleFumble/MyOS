#pragma once
#include <stdint.h>

typedef struct {
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_time_t;

/* Read the current time from the CMOS RTC. */
void rtc_read(rtc_time_t *t);
