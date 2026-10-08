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

extern long cpu_frequency;   /* set by system_init */

/* The esp32s3 core config has no byte-swap instruction (tested: 'swap' is not a
 * valid opcode), so the generic C byte-swaps in firmware/libc/include/endian.h are
 * used. Same as the jz47xx target. */
#define NEED_GENERIC_BYTESWAPS

#define nop asm volatile ("nop")

/*
 * Interrupt masking on Xtensa is by level: an interrupt is taken only when its
 * level is strictly greater than INTLEVEL. INTLEVEL is the low 13 bits of the PS
 * register, so the largest value is 0x1FFF and at that value nothing can exceed
 * it, which is how interrupts are disabled.
 *
 * rsr.intlevel / wsr.intlevel do not exist on the esp32s3 core config, so PS has
 * to be read and written as a whole.
 */
#define PS_INTLEVEL_MASK 0x1FFF

#define HIGHEST_IRQ_LEVEL   15
#define DISABLE_INTERRUPTS  0x1FFF

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

static inline void enable_irq(void)
{
    unsigned int ps;

    asm volatile ("rsr.ps %0" : "=r"(ps));
    ps &= ~PS_INTLEVEL_MASK;
    asm volatile ("wsr.ps %0" : : "r"(ps));
    asm volatile ("rsync");
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

/* Put the core in a power-saving state until an interrupt wakes it. */
static inline void core_sleep(void)
{
    asm volatile ("waiti 0");
}

/*
 * Cycle-count delay, following the jz47xx pattern.
 *
 * The loop is two instructions per iteration (bnez + addi.n), but a taken backward
 * branch costs more than one cycle on this core, so the real delay is somewhat
 * longer than requested. That is the safe direction for I2C bit-banging, which only
 * tolerates a slow clock, but it must not be relied on for anything timing-critical.
 *
 * It also depends on cpu_frequency being set, which happens in system_init. The core
 * runs at 240 MHz unless something changes it.
 */
static inline void udelay(unsigned us)
{
    unsigned i = us * (cpu_frequency / 2000000);

    asm volatile (
        "1:\n"
        "bnez %0, 1b\n"
        "addi %0, %0, -1\n"
        : "+r"(i)
    );
}

static inline void mdelay(unsigned ms)
{
    while (ms--) udelay(1000);
}

/*
 * Cache maintenance is left to the bootloader's configuration. The external data
 * cache is write-through by default, so there is nothing to commit. If the
 * bootloader is configured write-back (CONFIG_SPIRAM_CACHE_WRITEBACK), these
 * become real work and have to call the ROM cache functions, because Rockbox
 * cannot link against ESP-IDF.
 *
 * These are also what a DMA path would need if its buffers live in PSRAM: the DMA
 * bypasses the cache, so the CPU's dirty lines have to be committed before the DMA
 * reads and the DMA's writes have to be invalidated before the CPU reads. ESP-IDF
 * calls esp_cache_msync() for exactly this. The simplest way to avoid the whole
 * problem is to keep DMA buffers in internal SRAM, which is ESP-IDF's default
 * guidance as well.
 */
static inline void commit_dcache(void) {}
static inline void commit_discard_dcache(void) {}
static inline void commit_discard_idcache(void) {}

#endif /* SYSTEM_TARGET_H */
