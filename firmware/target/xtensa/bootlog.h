/***************************************************************************
 * Bring-up trace on UART0.
 *
 * See bootlog.c. Only useful while the target is being brought up; every call
 * is one character at 115200 baud through the mask ROM.
 ****************************************************************************/

#ifndef BOOTLOG_H
#define BOOTLOG_H

void usb_cdc_init(void);
#define RTC_SLOW_BASE 0x50000000u
#define RTC_LOG_SIZE  3072u
#define RTC_LIVE_ADDR     (RTC_SLOW_BASE + 3072u)   /* 24 liveness counters */
#define RTC_HIST_ADDR     (RTC_SLOW_BASE + 3200u)   /* handler cause histogram */
#define RTC_DIAG_ADDR     (RTC_SLOW_BASE + 3328u)   /* handler diagnostics */
#define RTC_SWITCH_ADDR   (RTC_SLOW_BASE + 3456u)   /* 32 (corep,thread) pairs */
#define RTC_LOOP_ADDR     (RTC_SLOW_BASE + 3584u)   /* switch_thread loop state */
#define RTC_EXC_ADDR      (RTC_SLOW_BASE + 3712u)   /* 96-word exception dump */
#define RTC_LOG_LEN_ADDR  (RTC_SLOW_BASE + 4100u)
#define RTC_IRQ_ADDR      (RTC_SLOW_BASE + 4104u)
#define RTC_TICK_ADDR     (RTC_SLOW_BASE + 4108u)
#define RTC_PREV_TICK     (RTC_SLOW_BASE + 4112u)
#define RTC_PREV_IRQ      (RTC_SLOW_BASE + 4116u)

void boot_trace_start(void);
void boot_phase(int n);
void boot_trace(char c);
void boot_trace_hex(unsigned int v);
void boot_trace_timer(void);
void boot_trace_mmu(unsigned int vaddr, int pages);
void boot_trace_extmem(void);
void boot_trace_raw(unsigned int v);
void rtc_bump(int idx);
void rtc_set(int idx, unsigned int v);
void rtc_or(int idx, unsigned int v);

#endif /* BOOTLOG_H */
