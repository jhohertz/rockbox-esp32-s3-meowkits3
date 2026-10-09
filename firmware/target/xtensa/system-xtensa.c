/***************************************************************************
 * ESP32-S3 system level.
 *
 * Rockbox is entered by ESP-IDF's second-stage bootloader, so caches, the SPI-RAM
 * controller and the clock tree are already up. What Rockbox still has to do here
 * is the parts the bootloader does not hand over:
 *
 *   - the vector base, which the bootloader leaves pointing at the ROM;
 *   - the peripheral clock gating for the blocks Rockbox uses;
 *   - the RTC watchdog, which the bootloader enables and never turns off.
 *
 * The last one matters more than it looks. CONFIG_BOOTLOADER_WDT_ENABLE defaults to
 * y, so by the time Rockbox runs the RTC watchdog is armed with a stage-0 action of
 * "reset the chip" and a timeout sized for the bootloader (300 ms by default). If
 * nothing feeds it, Rockbox resets every few hundred milliseconds. It is kept, and
 * fed from the tick, because that is freeze protection Rockbox would otherwise not
 * have on this target.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "kernel.h"
#include "power.h"
#include "tick.h"
#include "xtensa.h"

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

/* The vector table is placed by app.lds at the start of the instruction-bus view of
 * internal SRAM. VECBASE has to be a physical address the core can fetch from, so it
 * cannot point at the flash cache window. */
extern char _vector_table[];

/* RTC watchdog registers (RTC_CNTL base + offset) */
#define RWDT_CONFIG0   0x98
#define RWDT_FEED      0xAC
#define RWDT_WPROTECT  0xB0
#define RWDT_EN_BIT    (1u << 31)
#define RWDT_KEY       0x8F1D312Au       /* RTC_CNTL_SWD_WKEY_VALUE, written to
                                         * re-enable write protection */

/* RTC_CNTL_OPTIONS0: bit 5 is a write-only software reset of the PRO CPU. */
#define SW_PROCPU_RST  (1u << 5)

void system_init(void)
{
    cpu_frequency = CPU_FREQ;             /* 240 MHz */

    /* Peripheral clock gating. The timer groups default to enabled but GDMA does not
     * (its bit is 0 in PERIP_CLK_EN1), so the writes are made unconditional rather
     * than assumed. Setting a bit that is already set costs nothing. */
    wr(XT_SYSTEM_BASE + 0x18, rd(XT_SYSTEM_BASE + 0x18) | (1 << 15)); /* TIMERGROUP1 */
    wr(XT_SYSTEM_BASE + 0x18, rd(XT_SYSTEM_BASE + 0x18) | (1 << 13)); /* TIMERGROUP0 */
    wr(XT_SYSTEM_BASE + 0x1C, rd(XT_SYSTEM_BASE + 0x1C) | (1 << 6));  /* GDMA */

    /* Take over the vectors. Start from nothing enabled: the bootloader may have
     * left slots unmasked, and the only handler installed is the level-1 one. */
    asm volatile ("wsr %0, intenable" : : "r"(0));
    asm volatile ("wsr %0, intclear" : : "r"(0xFFFFFFFFu));
    asm volatile ("wsr %0, vecbase" : : "r"((unsigned int)_vector_table));
    asm volatile ("rsync");
}

void system_reboot(void)
{
    wr(XT_RTCCNTL_BASE, SW_PROCPU_RST);
    while (1);
}

/* There is no exception handler that can return here: the double-exception vector
 * traps, so a panic never comes back. Halting is the honest answer, and the RTC
 * watchdog will reset the chip if it is still armed. */
void system_exception_wait(void)
{
    while (1)
        asm volatile ("waiti 0");
}

void ICODE_ATTR feed_rwdt(void);

void power_init(void)
{
    /* Nothing to probe: the AXP173 PMU is on I2C and is not driven yet. The only
     * thing to decide is whether the watchdog the bootloader left running is kept.
     * It is, and the tick feeds it, which is what gives this target freeze
     * protection. */
    if (rd(XT_RTCCNTL_BASE + RWDT_CONFIG0) & RWDT_EN_BIT)
        tick_add_task(feed_rwdt);
}

void power_off(void)
{
    /* No low-power state is reachable from bare metal without the clock tree and the
     * PMU, so this halts. The watchdog is disabled first so halting is a halt and not
     * a reset loop. */
    wr(XT_RTCCNTL_BASE + RWDT_WPROTECT, 0);            /* drop write protection */
    wr(XT_RTCCNTL_BASE + RWDT_CONFIG0,
       rd(XT_RTCCNTL_BASE + RWDT_CONFIG0) & ~RWDT_EN_BIT);
    wr(XT_RTCCNTL_BASE + RWDT_WPROTECT, RWDT_KEY);     /* restore it */

    while (1)
        asm volatile ("waiti 0");
}

/* Called from the tick handler. Only does something if the bootloader armed the
 * watchdog, which by default it did. */
void ICODE_ATTR feed_rwdt(void)
{
    if (rd(XT_RTCCNTL_BASE + RWDT_CONFIG0) & RWDT_EN_BIT)
        wr(XT_RTCCNTL_BASE + RWDT_FEED, 1);
}
