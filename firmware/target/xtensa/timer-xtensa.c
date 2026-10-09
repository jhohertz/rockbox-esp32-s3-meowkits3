/***************************************************************************
 * ESP32-S3 timer groups.
 *
 * TIMERG1 timer 0 drives the kernel tick and timer 1 drives Rockbox's timer API.
 * They are the two general-purpose counters in the group; the watchdog block in
 * TIMERG0 is left alone because the bootloader's RTC watchdog and the interrupt
 * watchdog live there.
 *
 * Both timers share one interrupt source, so one matrix entry routes the group to
 * one core slot. The slot has to be one that is already level 1 on this core
 * config, because levels cannot be programmed - see firmware/include/xtensa.h.
 *
 * The counter is 54 bits, so a period of more than 2^54 ticks cannot be expressed.
 * At 1 MHz that is about 54 years, so nothing realistic is lost.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "kernel.h"
#include "tick.h"
#include "timer.h"
#include "xtensa.h"

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

#define TG1(reg) (XT_TIMERGROUP1_BASE + (reg))

/* tnconfig bit positions for a write: the alarm-enable and divider-reset bits are
 * write-trigger, so they have to be included in the same write as the rest. */
#define TN_BASE (XT_TN_USE_XTAL | (XT_TIMER_PRESCALER << XT_TN_DIVIDER_SHIFT) | \
                 XT_TN_AUTORELOAD | XT_TN_INCREASE)

static void tn_set_alarm(int n, long cycles)
{
    /* `long` is 32 bits on this ABI, so Rockbox's timer API cannot ask for a period
     * that uses the upper 22 bits of the counter. The high half is always zero. */
    wr(TG1(XT_TN_ALARM_LO(n)), (uint32_t)cycles);
    wr(TG1(XT_TN_ALARM_HI(n)), 0);
}

static void tn_load_zero(int n)
{
    wr(TG1(XT_TN_LOAD_LO(n)), 0);
    wr(TG1(XT_TN_LOAD_HI(n)), 0);
    wr(TG1(XT_TN_LOAD(n)), 1);          /* write any value to reload */
}

static bool tn_program(int n, long cycles, bool start)
{
    if (cycles <= 0)
        return false;

    tn_set_alarm(n, cycles);
    tn_load_zero(n);

    wr(TG1(XT_TN_CONFIG(n)), TN_BASE | XT_TN_DIVCNT_RST | XT_TN_ALARM_EN |
                             (start ? XT_TN_EN : 0));

    return true;
}

/* The kernel tick. interval_in_ms is 1000/HZ with the default HZ. */
void tick_start(unsigned int interval_in_ms) INIT_ATTR;
void tick_start(unsigned int interval_in_ms)
{
    /* Peripheral clock gating: the bootloader leaves these off for the peripherals
     * Rockbox uses, so the timer group and the interrupt matrix have to be enabled
     * before anything is written to them. */
    wr(XT_SYSTEM_BASE + 0x18, rd(XT_SYSTEM_BASE + 0x18) | (1 << 15)); /* TIMERGROUP1 */
    wr(XT_SYSTEM_BASE + 0x1C, rd(XT_SYSTEM_BASE + 0x1C) | (1 << 6));  /* DMA */

    /* Route the group's interrupt to a level-1 slot and unmask that slot. */
    wr(XT_INTERRUPT_CORE0_BASE + 4 * XT_SRC_TIMERG1, XT_SLOT_TIMERG1);
    set_intenable(get_intenable() | (1u << XT_SLOT_TIMERG1));
    /* tn_int_clr is write-to-clear, and int_ena is the per-timer mask */

    if (!tn_program(0, (long)interval_in_ms * TIMER_FREQ / 1000, true))
        return;

    wr(TG1(XT_TN_INT_ENA), 1);           /* timer 0 only */
}

bool timer_set(long cycles, bool start)
{
    if (!tn_program(1, cycles, start))
        return false;

    if (start)
        wr(TG1(XT_TN_INT_ENA), 1 | 2);

    return true;
}

bool timer_start(void)
{
    wr(TG1(XT_TN_CONFIG(1)), TN_BASE | XT_TN_ALARM_EN | XT_TN_EN);
    wr(TG1(XT_TN_INT_ENA), 1 | 2);

    return true;
}

void timer_stop(void)
{
    wr(TG1(XT_TN_CONFIG(1)), TN_BASE);   /* enabled bits cleared, period kept */
    wr(TG1(XT_TN_INT_ENA), 1);           /* keep the tick, drop the user timer */
}

/* Called from the level-1 dispatch. Runs on the interrupted thread's stack, so it
 * has to stay small. */
void ICODE_ATTR timer_irq(void)
{
    unsigned int st = rd(TG1(XT_TN_INT_ST));

    if (st & 1)
        call_tick_tasks();

    if (st & 2)
    {
        if (pfn_timer != NULL)
            pfn_timer();
    }

    /* tn_alarm_en is self-clearing: it goes to 0 when the alarm fires. Autoreload
     * reloads the counter but does not re-arm the comparison, so without this the
     * tick would happen exactly once.
     */
    if (st & 1)
        wr(TG1(XT_TN_CONFIG(0)), rd(TG1(XT_TN_CONFIG(0))) | XT_TN_ALARM_EN);
    if (st & 2)
        wr(TG1(XT_TN_CONFIG(1)), rd(TG1(XT_TN_CONFIG(1))) | XT_TN_ALARM_EN);

    /* Clear only what fired. tn_int_clr is write-to-clear. */
    wr(TG1(XT_TN_INT_CLR), st);
}
