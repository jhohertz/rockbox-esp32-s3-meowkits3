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

#ifdef HAVE_RTC_ALARM
/* Alarm registers and CTRL2 bit values are taken from rtc_e8564.c, which drives
 * the same chip family (E8564 is a PCF8563 clone) through the PP I2C layer. The
 * stock MeowKit driver documents alarm support in its header comment but
 * implements none, so there is nothing local to copy.
 *
 * CTRL2: TIE 0x01, AIE 0x02, TF 0x04, AF 0x08, TI_TP 0x10. Each alarm register
 * has bit 7 (AE) meaning "this field is not compared", which is how a time-only
 * alarm ignores the day and weekday. The alarm registers are consecutive from
 * 0x09, so a set is one burst write rather than four transactions.
 */
#define RTC_ALARM_MINUTES 0x09
#define RTC_ALARM_HOURS   0x0a
#define RTC_ALARM_DAY     0x0b
#define RTC_ALARM_WDAY    0x0c

#define RTC_AIE 0x02    /* CTRL2: raise AF and drive INT at the alarm time */
#define RTC_AF  0x08    /* CTRL2: alarm flag, set by the chip, cleared by us */
#define RTC_AE  0x80    /* alarm register: field not compared */

/* Set while an alarm flag has not yet been consumed, so that a shutdown or a
 * powermgmt pass cannot clear the alarm before apps/root_menu.c:896 has asked
 * whether this boot was started by it. Same name and role as in rtc_e8564.c, so
 * target power code can hold it the same way. */
bool rtc_lock_alarm_clear = true;

static int rtc_read_ctrl2(unsigned char *val)
{
    return i2c_readmem(RTC_ADDR, RTC_CTRL2, val, 1) == 1;
}

static void rtc_write_ctrl2(unsigned char val)
{
    i2c_writemem(RTC_ADDR, RTC_CTRL2, &val, 1);
}

void rtc_set_alarm(int h, int m)
{
    unsigned char ctrl2;
    unsigned char buf[4];

    /* Clear a stale flag but leave the enable bits alone; enabling is a
     * separate call, which is what apps/alarm_menu.c expects. */
    if (rtc_read_ctrl2(&ctrl2))
        rtc_write_ctrl2(ctrl2 & ~RTC_AF);

    buf[0] = (m >= 0) ? DEC2BCD(m) : RTC_AE;
    buf[1] = (h >= 0) ? DEC2BCD(h) : RTC_AE;
    buf[2] = RTC_AE;      /* day */
    buf[3] = RTC_AE;      /* weekday */

    i2c_writemem(RTC_ADDR, RTC_ALARM_MINUTES, buf, sizeof(buf));
    /* The alarm is armed by rtc_enable_alarm(), not here. */
}

void rtc_get_alarm(int *h, int *m)
{
    unsigned char buf[4];

    if (i2c_readmem(RTC_ADDR, RTC_ALARM_MINUTES, buf, sizeof(buf)) != sizeof(buf))
        return;

    /* rtc_e8564.c reads the hours out of buf[0], i.e. out of the minute
     * register. buf[1] is the hour register, masked to 6 bits because bit 7 is
     * the AE flag and bit 6 would be the 12h flag in the time registers. */
    *m = BCD2DEC(buf[0] & 0x7f);
    *h = BCD2DEC(buf[1] & 0x3f);
}

void rtc_enable_alarm(bool enable)
{
    unsigned char ctrl2;

    if (enable)
    {
        if (!rtc_read_ctrl2(&ctrl2))
            return;
        rtc_write_ctrl2((ctrl2 & ~RTC_AF) | RTC_AIE);
    }
    else
    {
        /* Hold off clearing until the boot-time check has run. */
        if (rtc_lock_alarm_clear)
            return;
        if (!rtc_read_ctrl2(&ctrl2))
            return;
        rtc_write_ctrl2(ctrl2 & ~(RTC_AIE | RTC_AF));
    }
}

bool rtc_check_alarm_started(bool release_alarm)
{
    static bool alarm_state, run_before = false;
    unsigned char ctrl2;
    bool started;

    if (run_before)
    {
        /* Later calls in the same boot must report the same answer, otherwise
         * a second caller would see the alarm as "not this boot". */
        started = alarm_state;
        alarm_state &= !release_alarm;
    }
    else
    {
        if (!rtc_read_ctrl2(&ctrl2))
            return false;

        /* Both bits: AF says the alarm fired, AIE says it fired because of an
         * alarm we armed rather than a flag left over from a previous life. */
        alarm_state = started = ((ctrl2 & RTC_AF) && (ctrl2 & RTC_AIE));

        if (release_alarm && started)
        {
            if (rtc_read_ctrl2(&ctrl2))
                rtc_write_ctrl2(ctrl2 & ~(RTC_AF | RTC_AIE));
        }
        run_before = true;
        rtc_lock_alarm_clear = false;
    }

    return started;
}

bool rtc_check_alarm_flag(void)
{
    unsigned char ctrl2;

    if (!rtc_read_ctrl2(&ctrl2))
        return false;

    return (ctrl2 & RTC_AF) != 0;
}
#endif /* HAVE_RTC_ALARM */
