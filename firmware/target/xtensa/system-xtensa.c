/***************************************************************************
 * ESP32-S3 system level.
 *
 * Rockbox is entered by ESP-IDF's second-stage bootloader, so caches, the SPI-RAM
 * controller and the clock tree are already up. What Rockbox still has to do here
 * is the parts the bootloader does not hand over:
 *
 *   - the vector base, which the bootloader leaves pointing at the ROM;
 *   - the peripheral clock gating for the blocks Rockbox uses.
 *
 * The RTC watchdog is also left running by the bootloader, but it belongs with
 * power management: it is fed from a tick task and disabled on power off, so it
 * lives in power-xtensa.c.
 ****************************************************************************/
#include "config.h"
#include "bootlog.h"

#include "system.h"
#include "xtensa.h"

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

/* The vector table is placed by app.lds at the start of the instruction-bus view of
 * internal SRAM. VECBASE has to be a physical address the core can fetch from, so it
 * cannot point at the flash cache window. */
extern char _vector_table[];

/* RTC_CNTL_OPTIONS0: bit 5 is a write-only software reset of the PRO CPU. */
#define SW_PROCPU_RST  (1u << 5)

/*
 * The core clock is the BBPLL (480 MHz) divided by CPUPERIOD_SEL. PRE_DIV_CNT in
 * SYSCLK_CONF belongs to the XTAL and RC_FAST paths, so it is not part of the
 * PLL ladder and is left at whatever the loader set.
 *
 * Staying on the PLL is a requirement, not a preference: MSPI (flash and PSRAM)
 * derives its core clock from the PLL tap selected by CORE_CLK_SEL, GPSPI2 (the
 * display) uses PLL_CLK_80M, and AHB/APB - GDMA's clock - is a fixed 80 MHz while
 * CPU_CLK is PLL driven. psram-xtensa.c already programs MSPI_CORE_CLK_SEL for the
 * 160 MHz tap, so the BBPLL being up is load-bearing for the whole target.
 *
 * ESP-IDF also lowers the analog regulator bias (dbias) when it leaves 240 MHz, via
 * REGI2C and the ROM's PVT calibration globals. Rockbox has neither, and running at
 * 160 or 80 MHz with the 240 MHz bias is over-voltage rather than under-voltage, so
 * the bias is simply left alone. The LDO slave count is still adjusted, using the
 * same values ESP-IDF uses at the same frequencies.
 *
 * The order mirrors ESP-IDF's rtc_clk_cpu_freq_to_pll_mhz(): slaves are removed
 * before the clock is raised and added after it is lowered, so the LDO voltage does
 * not dip at the instant the clock changes.
 */
struct cpu_freq_step {
    long hz;
    uint32_t period;
    uint32_t slaves;
};

/* Indexed by CPUPERIOD_SEL, so the entry position is the register value. */
static const struct cpu_freq_step freq_ladder[] = {
    {  80000000, XT_CPU_PERIOD_80M,  3 },
    { 160000000, XT_CPU_PERIOD_160M, 1 },
    { 240000000, XT_CPU_PERIOD_240M, 0 },
};

static const struct cpu_freq_step *find_step(long hz)
{
    for (unsigned i = 0; i < sizeof(freq_ladder) / sizeof(freq_ladder[0]); i++)
        if (freq_ladder[i].hz == hz)
            return &freq_ladder[i];

    return NULL;
}

static void set_ldo_slaves(uint32_t count)
{
    uint32_t v = rd(XT_RTC_DATE_REG);

    v = (v & ~(XT_LDO_SLAVE_MASK << XT_LDO_SLAVE_SHIFT)) |
        ((count & XT_LDO_SLAVE_MASK) << XT_LDO_SLAVE_SHIFT);
    wr(XT_RTC_DATE_REG, v);
}

/* What the hardware is actually running at, so cpu_frequency is not just the value
 * the config file claims. */
static long read_cpu_frequency(void)
{
    uint32_t sel = (rd(XT_SYS_CLK_CONF) >> XT_SYS_CLK_SEL_SHIFT) & 3;

    if (sel == XT_SYS_CLK_SEL_PLL) {
        uint32_t conf = rd(XT_SYS_CPU_PER_CONF);
        uint32_t period = conf & XT_CPU_PERIOD_MASK;

        /* The ladder is built on the 480 MHz PLL tap. On the 320 MHz tap
         * CPUPERIOD_SEL = 2 is an undefined state - ESP-IDF rejects it in
         * clk_hal_cpu_get_freq_hz - so the frequency is not knowable there.
         */
        if (period >= sizeof(freq_ladder) / sizeof(freq_ladder[0]) ||
            (period == XT_CPU_PERIOD_240M && !(conf & XT_PLL_FREQ_SEL)))
            return CPU_FREQ;

        return freq_ladder[period].hz;
    }

    if (sel == XT_SYS_CLK_SEL_XTAL)
        return XT_XTAL_FREQ / ((rd(XT_SYS_CLK_CONF) & XT_SYS_PRE_DIV_MASK) + 1);

    return CPU_FREQ;    /* RC_FAST or an encoding we do not recognise */
}

void set_cpu_frequency(long frequency)
{
    const struct cpu_freq_step *step = find_step(frequency);
    long old = cpu_frequency;
    uint32_t conf;

    /* Only the PLL taps are reachable. Anything else is ignored rather than
     * approximated, because a frequency the hardware cannot produce would leave
     * cpu_frequency lying to udelay().
     *
     * If the loader had left the core on XTAL or RC_FAST the BBPLL divider would
     * not be programmed, and switching onto an unlocked PLL is not something to do
     * blind. It does not happen in practice: psram-xtensa.c and spi-xtensa.c already
     * depend on the PLL taps.
     */
    if (!step || step->hz == old ||
        ((rd(XT_SYS_CLK_CONF) >> XT_SYS_CLK_SEL_SHIFT) & 3) != XT_SYS_CLK_SEL_PLL)
        return;

    if (step->hz > old)
        set_ldo_slaves(step->slaves);

    conf = rd(XT_SYS_CPU_PER_CONF);
    wr(XT_SYS_CPU_PER_CONF, (conf & ~XT_CPU_PERIOD_MASK) | step->period);

    if (step->hz < old)
        set_ldo_slaves(step->slaves);

    /* Last, so that any udelay() running during the switch is measured against the
     * clock the core is still on: the error is then a delay that is too long, which
     * every user of udelay() here tolerates, rather than one that is too short.
     */
    cpu_frequency = step->hz;
}

void system_init(void)
{
    cpu_frequency = read_cpu_frequency();

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
    boot_trace('s');                    /* vector table installed */
    boot_trace_hex((unsigned)cpu_frequency);
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
