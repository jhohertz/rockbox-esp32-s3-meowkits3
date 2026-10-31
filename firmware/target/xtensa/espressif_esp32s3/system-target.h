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
 * Cache maintenance is declared in cache-xtensa.h and implemented in
 * cache-xtensa.c, which is the only object on this path that includes the
 * platform's cache headers. system-target.h is reached from system.h, so a
 * vendor include here would put IDF's headers in front of every Rockbox file in
 * the build -- measured at 919 objects, which is what made -Wno-undef a global
 * flag. Declarations cost nothing; includes do not.
 */

extern long cpu_frequency;   /* set by system_init */

/* Hold ESP_PM_NO_LIGHT_SLEEP while a peripheral whose interrupt cannot wake
 * the core is armed. Only the USB device needs it today: see
 * xt_light_sleep_block() in system-esp32s3.c for why, and
 * espressif_esp32s3/idf-app/sdkconfig.defaults for why sleeping is safe on this port at all --
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

/* commit_dcache(), commit_discard_dcache() and commit_discard_idcache() are
 * declared in cache-xtensa.h. They used to be inline here, calling the
 * toolchain's xthal_* cache entry points -- which on esp32s3 are empty stubs
 * (`entry a1, 16; retw.n`, verified by disassembly), so the contract was
 * satisfied in the source and not in the binary. The measurement that
 * recorded a stale instruction word at a codec's faulting pc was describing
 * exactly that: nothing ever invalidated or wrote back anything. */

#endif /* SYSTEM_TARGET_H */
