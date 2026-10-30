/***************************************************************************
 * ESP32-S3 target header (arch-level fallback).
 *
 * Nothing here initialises memory or caches: Rockbox is entered by ESP-IDF's
 * second-stage bootloader, which has already configured the SPI-RAM controller
 * and enabled both caches.
 ****************************************************************************/
#ifndef SYSTEM_TARGET_H
#define SYSTEM_TARGET_H

#include <stdint.h>
#include <stdbool.h>
#include "cache-xtensa.h"   /* per-range maintenance, declared not inlined */
/*
 * Whole-cache maintenance comes from xtensa HAL asm objects on the firmware
 * side and does not exist on the freestanding side, where codecs reach the
 * cache through codec_api instead. Declaring the three entry points rather than
 * including <xtensa/hal.h> matters more than it looks: system-target.h is
 * reached from system.h, so an IDF include here puts the vendor's headers in
 * front of every Rockbox file in the build -- measured at 919 objects, which is
 * what made -Wno-undef a global flag. The prototypes below are the same three
 * extern declarations hal.h itself carries. Per-range maintenance, which needs
 * IDF's cache HAL, is in cache-xtensa.h and is included by the three drivers
 * that actually call it.
 */
void xthal_dcache_all_writeback(void);
void xthal_dcache_all_writeback_inv(void);
void xthal_icache_all_invalidate(void);

extern long cpu_frequency;   /* set by system_init */

/* Hold ESP_PM_NO_LIGHT_SLEEP while a peripheral whose interrupt cannot wake
 * the core is armed. Only the USB device needs it today: see
 * xt_light_sleep_block() in system-esp32s3.c for why, and
 * idf-app/sdkconfig.defaults for why sleeping is safe on this port at all --
 * the peripheral domain is deliberately left powered, which is what keeps
 * hand-written drivers intact across a sleep. Codecs and plugins never call
 * it, and the firmware side is the only definition. */
void xt_light_sleep_block(bool block);

/*
 * Frequency ladder.
 *
 * All three points are taps of the BBPLL, so nothing but the core moves when
 * the CPU is clocked down: AHB/APB, which is GDMA's clock, is a fixed 80 MHz
 * whenever CPU_CLK comes from the PLL; MSPI (flash and PSRAM) takes its core
 * clock from the PLL tap selected by CORE_CLK_SEL; GPSPI2 (the display) uses
 * PLL_CLK_80M; and I2S, I2C and the timer groups are clocked from XTAL.
 *
 * XTAL (40 MHz) is deliberately not in the ladder: it would drop APB to 40 MHz
 * and invalidate the MSPI core clock selection that IDF's PSRAM driver
 * programs.
 */
#define CPUFREQ_MAX     240000000
#define CPUFREQ_NORMAL  160000000
#define CPUFREQ_DEFAULT  80000000

/* The esp32s3 core config has no byte-swap instruction (tested: 'swap' is
 * not a valid opcode), so the generic C byte-swaps in
 * firmware/libc/include/endian.h are used. Same as the jz47xx target. */
#define NEED_GENERIC_BYTESWAPS

#define nop asm volatile ("nop")

/*
 * Interrupt masking on Xtensa is by level: an interrupt is taken only when its
 * level is strictly greater than INTLEVEL. INTLEVEL is the low four bits of PS
 * (xtensa/corebits.h: PS_INTLEVEL_SHIFT 0, PS_INTLEVEL_MASK 0x0F), so 15 is the
 * largest value and at 15 nothing can exceed it, which is how interrupts are
 * disabled. The rest of PS is EXCM (bit 4), UM (5), RING (6-7), OWB (8-11),
 * CALLINC (16-17) and WOE (18).
 *
 * rsr.intlevel / wsr.intlevel do not exist on the esp32s3 core config, so PS
 * has to be read and written as a whole.
 */
#ifdef ESP_PLATFORM
/*
 * ESP-IDF provides xtensa/corebits.h, which defines PS_INTLEVEL_MASK and PS_WOE
 * itself. Defining them here as well collides as soon as an IDF xtensa header
 * reaches the same translation unit (via FreeRTOSConfig.h -> xtensa_config.h ->
 * xtensa_context.h -> corebits.h). The values agree -- corebits.h has
 * PS_WOE_MASK 0x00040000 and PS_INTLEVEL_MASK 0x0000000F -- so this is a name
 * collision rather than a disagreement, but taking IDF's is the only option
 * that works in both orders: corebits.h defines them unguarded, so an #ifndef
 * here would not suppress it.
 *
 * The masking helpers below are transitional too. On the IDF path interrupt
 * masking and the tick belong to FreeRTOS, and this whole block goes away with
 * the bare-metal startup.
 */
#include <xtensa/corebits.h>
#else
#define PS_INTLEVEL_MASK 0x0000000F
#define PS_WOE           0x00040000
#endif

#define HIGHEST_IRQ_LEVEL   15
#define DISABLE_INTERRUPTS  15

static inline int set_irq_level(int level) __attribute__((always_inline));
static inline int set_irq_level(int level)
{
    unsigned int ps, old;

    asm volatile ("rsr.ps %0" : "=r"(ps));
    old = ps & PS_INTLEVEL_MASK;
    ps = (ps & ~PS_INTLEVEL_MASK) | ((unsigned int)level & PS_INTLEVEL_MASK);
    asm volatile ("wsr.ps %0" : : "r"(ps));
    asm volatile ("rsync");          /* CSR writes need a sync before use */

    return (int)old;
}

/* Masking by level is only half of it: a slot also has to be unmasked in
 * INTENABLE. Levels cannot be programmed on this core config, so the slot
 * has to already be the level you want (see firmware/include/xtensa.h). */

static inline unsigned int get_irq_level(void)
{
    unsigned int ps;

    asm volatile ("rsr.ps %0" : "=r"(ps));
    return ps & PS_INTLEVEL_MASK;
}

static inline unsigned int get_intenable(void)
{
    unsigned int v;

    asm volatile ("rsr %0, intenable" : "=r"(v));
    return v;
}

static inline void set_intenable(unsigned int mask)
{
    asm volatile ("wsr %0, intenable" : : "r"(mask));
    asm volatile ("rsync");
}

static inline void enable_irq(void)
{
    set_irq_level(0);
}

static inline void disable_irq(void)
{
    set_irq_level(DISABLE_INTERRUPTS);
}

static inline int disable_irq_save(void)
{
    return set_irq_level(DISABLE_INTERRUPTS);
}

static inline void restore_irq(int oldlevel)
{
    set_irq_level(oldlevel);
}

/* Put the core in a power-saving state until an interrupt wakes it.
 *
 * switch_thread() reaches here with interrupts masked by PS.INTLEVEL = 15, so a
 * bare waiti would stall forever: WAITI n waits for a slot of level n+1 or
 * higher that is enabled *and* able to be taken, and level 1 cannot be taken
 * while the core is already at level 15. The level has to be dropped before
 * waiting, the same way ARM's core_sleep re-enables and Coldfire's "stop
 * #0x2000" wakes on an interrupt. The handler's rfe puts the level back, so the
 * resumed thread is not left with interrupts open.
 */
static inline void core_sleep(void)
{
    set_irq_level(0);
    asm volatile ("waiti 0");
}

/*
 * Cycle-count delay, following the jz47xx pattern.
 *
 * The loop is two instructions per iteration (bnez + addi.n), but a taken
 * backward branch costs more than one cycle on this core, so the real delay is
 * somewhat longer than requested. That is the safe direction for I2C
 * bit-banging, which only tolerates a slow clock, but it must not be relied on
 * for anything timing-critical.
 *
 * It also depends on cpu_frequency being set, which happens in system_init. The
 * core runs at 240 MHz unless something changes it.
 */
static inline void udelay(unsigned us)
{
    unsigned cycles = us * (cpu_frequency / 1000000u);
    unsigned start, now;

    asm volatile ("rsr %0, ccount" : "=r"(start));
    do {
        asm volatile ("rsr %0, ccount" : "=r"(now));
    } while ((int)(now - start) < (int)cycles);
}

static inline void mdelay(unsigned ms)
{
    while (ms--) udelay(1000);
}

/*
 * Whole-cache maintenance. Under ESP-IDF this belongs to the xtensa HAL: the
 * esp32s3 core config has no cache instructions of its own (probeD, flushD,
 * lockD, rdtag, wttag, rinvalidate and binvalidate are all invalid opcodes on
 * this dynconfig), and libxt_hal.a carries the sequences that work. The bare-
 * metal route - calling the mask ROM's Cache_* entry points by absolute
 * address - is on the tagged branch.
 *
 * Internal SRAM (0x3FC88000-0x3FD00000, which is exactly
 * SOC_DMA_LOW/SOC_DMA_HIGH) is served by the L1 D-cache, so anything a DMA
 * touches there needs maintenance. That case is per-range, not whole-cache, and
 * lives in cache-xtensa.h.
 */
static inline void commit_dcache(void)
{
    xthal_dcache_all_writeback();
}

static inline void commit_discard_dcache(void)
{
    xthal_dcache_all_writeback_inv();
}

/* The instruction cache is a separate structure from the data cache and the two
 * are not coherent with each other. Code written through the data window lands
 * in the D-cache and is written back to memory, but the I-cache keeps whatever
 * previously occupied those lines -- on this target, whichever codec last used
 * the same load address -- so the core fetches stale bytes and reports
 * IllegalInstruction on a perfectly valid instruction.
 *
 * Measured rather than inferred: after loading vorbis, the word at the faulting
 * PC read 02 78 b9 aa where the linked image says 29 08 81 5f, while both
 * neighbouring words matched exactly. A single stale line, not corruption. The
 * panic handler's own memory dump showed the same stale bytes because it reads
 * the 0x42... range through the instruction side too.
 *
 * The name has always promised both halves; only the data-cache half was ever
 * implemented. */
static inline void commit_discard_idcache(void)
{
    commit_discard_dcache();
    xthal_icache_all_invalidate();
}

#endif /* SYSTEM_TARGET_H */
