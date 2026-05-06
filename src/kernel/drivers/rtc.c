#include "rtc.h"
#include "../arch/port_io.h"

#define CMOS_ADDR  0x70
#define CMOS_DATA  0x71

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void)
{
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F));
}

void rtc_read(rtc_time_t *t)
{
    /* Wait until RTC is not in the middle of an update */
    while (update_in_progress());

    uint8_t secs, mins, hrs, day, mon, yr, cent, regB;

    /* Read twice, verify consistent (avoids partial-update reads) */
    do {
        secs  = cmos_read(0x00);
        mins  = cmos_read(0x02);
        hrs   = cmos_read(0x04);
        day   = cmos_read(0x07);
        mon   = cmos_read(0x08);
        yr    = cmos_read(0x09);
        cent  = cmos_read(0x32);   /* Century register (may be 0 on some hw) */
    } while (update_in_progress());

    regB = cmos_read(0x0B);

    /* Convert BCD to binary if necessary (bit 2 of register B = binary mode) */
    if (!(regB & 0x04)) {
        secs = bcd_to_bin(secs);
        mins = bcd_to_bin(mins);
        hrs  = bcd_to_bin(hrs & 0x7F) | (hrs & 0x80);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        yr   = bcd_to_bin(yr);
        cent = bcd_to_bin(cent);
    }

    /* 12-hour to 24-hour */
    if (!(regB & 0x02) && (hrs & 0x80)) {
        hrs = (uint8_t)(((hrs & 0x7F) + 12) % 24);
    }

    uint16_t year = (uint16_t)(cent ? (cent * 100 + yr) : (2000 + yr));

    t->seconds = secs;
    t->minutes = mins;
    t->hours   = hrs;
    t->day     = day;
    t->month   = mon;
    t->year    = year;
}
