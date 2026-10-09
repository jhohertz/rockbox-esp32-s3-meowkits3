/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2024 by Rockbox developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
/*
 * PCF8563 real-time clock, driven through the synchronous I2C API that every
 * target provides.
 *
 * The existing RTC_E8564 driver covers the same register layout, but it is
 * written against the PP asynchronous I2C layer (i2c-pp.h), so it cannot be
 * reused by a target that only has i2c_readmem()/i2c_writemem().
 *
 * Two things worth knowing about this chip:
 *
 *   - CTRL1 bit 7 halts the oscillator. A PCF8563 that has been unpowered comes
 *     back stopped if the previous firmware stopped it, so rtc_init() clears the
 *     bit rather than assuming the default.
 *   - Bit 7 of the seconds register is the voltage-low flag. When it is set the
 *     chip says its timekeeping is not guaranteed, which is the only signal we
 *     get that the RTC lost power.
 */

#include "config.h"
#include "i2c.h"
#include "rtc.h"
#include "system.h"
#include "timefuncs.h"

#define RTC_ADDR 0x51

#define RTC_CTRL1   0x00
#define RTC_CTRL2   0x01
#define RTC_SECONDS 0x02
#define RTC_MINUTES 0x03
#define RTC_HOURS   0x04
#define RTC_DAYS    0x05
#define RTC_WEEKDAY 0x06
#define RTC_MONTHS  0x07
#define RTC_YEARS   0x08

#define RTC_STOP    0x80    /* CTRL1: oscillator halted */
#define RTC_VL      0x80    /* seconds: voltage low, time not reliable */

/* The time registers are consecutive and accept a burst write, which is one
 * transaction instead of seven. */
static bool rtc_read_time(unsigned char *buf)
{
    return i2c_readmem(RTC_ADDR, RTC_SECONDS, buf, 7) == 7;
}

void rtc_init(void)
{
    unsigned char ctrl1;

    if (i2c_readmem(RTC_ADDR, RTC_CTRL1, &ctrl1, 1) != 1)
        return;

    if (ctrl1 & RTC_STOP)
        i2c_writemem(RTC_ADDR, RTC_CTRL1, (const unsigned char *)"\x00", 1);
}

int rtc_read_datetime(struct tm *tm)
{
    unsigned char buf[7];

    if (!rtc_read_time(buf))
        return 0;

    tm->tm_sec  = BCD2DEC(buf[0] & 0x7f);
    tm->tm_min  = BCD2DEC(buf[1] & 0x7f);
    tm->tm_hour = BCD2DEC(buf[2] & 0x3f);   /* bit 7 is the 12h AM/PM flag */
    tm->tm_mday = BCD2DEC(buf[3] & 0x3f);
    tm->tm_wday = BCD2DEC(buf[4] & 0x07);
    tm->tm_mon  = BCD2DEC(buf[5] & 0x1f) - 1;   /* bit 7 is the century */
    tm->tm_year = BCD2DEC(buf[6]) + 100;        /* struct tm: years since 1900 */

    set_day_of_year(tm);

    return 1;
}

int rtc_write_datetime(const struct tm *tm)
{
    unsigned char buf[7];

    buf[0] = DEC2BCD(tm->tm_sec);
    buf[1] = DEC2BCD(tm->tm_min);
    buf[2] = DEC2BCD(tm->tm_hour);
    buf[3] = DEC2BCD(tm->tm_mday);
    buf[4] = DEC2BCD(tm->tm_wday);
    buf[5] = DEC2BCD(tm->tm_mon + 1);
    buf[6] = DEC2BCD(tm->tm_year - 100);

    return i2c_writemem(RTC_ADDR, RTC_SECONDS, buf, sizeof(buf)) == sizeof(buf);
}
