/***************************************************************************
 * Cache maintenance for DMA-facing buffers -- implementation.
 *
 * This is the only Rockbox object on this target that includes the platform's
 * cache HAL, and that is the point: cache-xtensa.h declares the entry points,
 * so the vendor header stays out of system-target.h and therefore out of the
 * ~919 objects that reach it through system.h. See cache-xtensa.h for why the
 * platform's documented esp_cache_msync() is the wrong call here.
 ****************************************************************************/

#include "config.h"
#include "system.h"
#include "cache-xtensa.h"

#if defined(CPU_XTENSA) && !defined(__SIMULATOR__)
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include "soc/soc.h"             /* SOC_DRAM_* and SOC_IRAM_*, the uncached windows */
#include "esp32s3/rom/cache.h"   /* Cache_WriteBack_All, not wrapped by the LL */

/* Internal SRAM is not cached on esp32s3. SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
 * is defined only for esp32p4 in this IDF, and that is the capability the
 * platform itself keys on: IDF's SD and SPI drivers guard their descriptor
 * maintenance with it, so on this part they do none for buffers in internal
 * memory. Everything this target puts in front of a DMA -- GDMA descriptors
 * and ping-pong buffers, the SDHOST descriptor ring, USB endpoint buffers --
 * lives in that region, so these range calls have never had anything to do.
 *
 * They were not free. cache_hal_writeback_addr() and
 * cache_hal_invalidate_addr() end up in the ROM's cache routines, and the
 * audio path calls them from the GDMA interrupt handler on every descriptor.
 * Cache code running inside an interrupt can interleave with a cache
 * operation in task context, which is how loading a codec while the capture
 * interrupt was still firing produced a repeated "Cache error" at 0x4004e5b7
 * inside the ROM cache module, and how capture itself picked up a stutter at
 * 44.1 kHz. Doing nothing is both cheaper and safer, and it is what the
 * hardware actually needs. */
static bool internal_memory(unsigned int addr, unsigned int size)
{
    unsigned int end = addr + size - 1;

    if (size == 0)
        return true;

    return (addr >= SOC_DRAM_LOW && end < SOC_DRAM_HIGH)
        || (addr >= SOC_IRAM_LOW && end < SOC_IRAM_HIGH);
}

void commit_dcache_range(const void *base, unsigned int size)
{
    if (internal_memory((unsigned int)base, size))
        return;

    cache_hal_writeback_addr((unsigned int)base, size);
}

void discard_dcache_range(const void *base, unsigned int size)
{
    if (internal_memory((unsigned int)base, size))
        return;

    cache_hal_invalidate_addr((unsigned int)base, size);
}

void commit_discard_dcache_range(const void *base, unsigned int size)
{
    commit_dcache_range(base, size);
    discard_dcache_range(base, size);
}

unsigned int cache_dcache_line_size(void)
{
    return 32;  /* esp32s3 D-cache line, per cache_hal_get_cache_line_size */
}

bool cache_writeback_range(unsigned int addr, unsigned int size)
{
    /* Returns whether the ROM accepted the range. It declines anything outside
     * the D-cache address room, which is a fact about the part: internal SRAM
     * is not cached on esp32s3 (SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE is defined
     * only for esp32p4 in this IDF), so the ranges that matter are the ones in
     * the external window. */
    return Cache_WriteBack_Addr(addr, size) == 0;
}

unsigned int cache_invalidate_icache_range(unsigned int addr, unsigned int size)
{
    unsigned int line = cache_hal_get_cache_line_size(CACHE_LL_LEVEL_EXT_MEM,
                                                     CACHE_TYPE_INSTRUCTION);
    unsigned int items;

    if (line == 0 || size == 0)
        return 0;

    /* The ROM takes a line count from an aligned base, so widen the range to
     * whole lines at both ends rather than trusting the caller's bounds. */
    addr &= ~(line - 1);
    items = (size + (2 * line - 1)) / line;

    Cache_Invalidate_ICache_Items(addr, items);
    return items;
}

void commit_dcache(void)
{
    Cache_WriteBack_All();
}

void commit_discard_dcache(void)
{
    /* Write back, but do not discard. Other archs commit and discard here,
     * and on a part with a range operation that is cheap and safe. Here the
     * only available operation is whole-cache, and the external D-cache backs
     * memory that is live while this can be called: the audiobuffer, the
     * framebuffer, codecbuf and pluginbuf are all in PSRAM. Discarding those
     * lines drops whatever has not reached memory yet, and there is nothing to
     * gain from it after a full write-back -- the CPU reads those pages
     * through the same cache, so a clean line is not a stale one. */
    Cache_WriteBack_All();
}

void commit_discard_idcache(void)
{
    /* Write back first: the image was copied through the data window, and an
     * instruction fetch cannot see a line the write-back has not pushed out.
     * Then invalidate the instruction cache, which is the half this name is
     * actually for -- the fetch must not land on a line belonging to whatever
     * occupied these pages before the image was placed. The data cache is not
     * discarded, for the reason given above. */
    Cache_WriteBack_All();
    cache_ll_invalidate_all(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_INSTRUCTION,
                            CACHE_LL_ID_ALL);
}

#endif /* CPU_XTENSA && !__SIMULATOR__ */
