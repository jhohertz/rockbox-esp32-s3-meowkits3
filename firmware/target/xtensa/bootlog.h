/***************************************************************************
 * Bring-up trace.
 *
 * The bare-metal version wrote one character at a time through the mask ROM
 * and parked its state in RTC slow memory, because the board had no console
 * and esptool has to reset the chip between runs. Under ESP-IDF there is a
 * real console (USB-Serial-JTAG) and the log survives a reset on its own, so
 * this is just a character sink with a small ISR guard.
 *
 * The call sites are leftovers from bring-up. They are being removed driver
 * by driver as each one is ported onto IDF drivers; the ones that remain are
 * in files that still drive hardware by hand.
 ****************************************************************************/

#ifndef BOOTLOG_H
#define BOOTLOG_H

void boot_trace_start(void);
void boot_phase(int n);
void boot_trace(char c);
void boot_trace_hex(unsigned int v);
void boot_trace_raw(unsigned int v);

/* Liveness counters. Named rtc_* because they used to live in RTC slow
 * memory; they are plain globals now. Read them with rtc_counter(). */
void rtc_bump(int idx);
void rtc_set(int idx, unsigned int v);
void rtc_or(int idx, unsigned int v);
unsigned int rtc_counter(int idx);

#endif /* BOOTLOG_H */
