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
#include "esp32s3/rom/cache.h"   /* the whole-cache ops, not wrapped by the LL */
#include "esp_private/critical_section.h"
#include "sys/lock.h"

/*
 * Internal SRAM *is* cache-backed on esp32s3, and these range operations are
 * the reason this file exists.
 *
 * components/hal/esp32s3/include/hal/cache_ll.h maps both IRAM0 and DRAM0 to a
 * cache level, esp_cache_msync() maintains whatever that resolver accepts, and
 * IDF's own drivers do it unconditionally for the buffers they hand to a DMA:
 * components/esp_driver_sdmmc/src/sdmmc_transaction.c msyncs cmdinfo->data in
 * both directions and components/esp_driver_i2s/i2s_common.c msyncs its DMA
 * buffers and descriptors. SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE, which exists
 * only for esp32p4 in this IDF, is a statement about L1 strictness on that
 * part -- not about whether this part caches its internal SRAM. An earlier
 * revision of this file read that symbol as the latter and made every internal
 * range a no-op, which silently hollowed out two things this target owns:
 * usb_dw_target_dma_discard() (the DWC writes endpoint buffers in internal
 * DRAM, and usb-designware.c calls discard_dcache_range() for every CPU except
 * the ARM940T) and the GDMA descriptor ring, whose owner bit the transmit path
 * polls. Reading a stale line there is not a slowdown, it is the driver acting
 * on bytes that were never transferred.
 *
 * What the no-op was really protecting against was a race, not wasted work:
 * cache code running inside the GDMA interrupt interleaving with a whole-cache
 * operation in task context, which is the "Cache error" this file used to
 * report at 0x4004e5b7 inside the ROM cache module. The platform's answer to
 * that race is a lock, and it is visible in esp_cache_msync.c: every
 * cache_hal_writeback_addr() and cache_hal_invalidate_addr() call is bracketed
 * by esp_os_enter_critical_safe() on a static critical-section lock. Same here,
 * which is why the whole-cache entry points take it too -- the pair has to be
 * mutually exclusive, not each side silent.
 */
DEFINE_CRIT_SECTION_LOCK_STATIC(cache_sync_lock);

static inline void cache_sync_enter(void)
{
    esp_os_enter_critical_safe(&cache_sync_lock);
}

static inline void cache_sync_exit(void)
{
    esp_os_exit_critical_safe(&cache_sync_lock);
}

static unsigned int cache_line(void)
{
    return cache_dcache_line_size();
}

void commit_dcache_range(const void *base, unsigned int size)
{
    if (size == 0)
        return;

    cache_sync_enter();
    cache_hal_writeback_addr((unsigned int)base, size);
    cache_sync_exit();
}

/* Tree-wide contract, from cpucache-arm.h and mmu-mips.c: discard invalidates
 * without writing back, *except* for buffer edges that are not on a line
 * boundary, because the rest of such a line belongs to live data. Widening to
 * whole lines and writing back before invalidating satisfies that in the
 * strongest form: after a write-back no line is dirty, so the invalidation
 * cannot discard anything that had not reached memory. Both halves are under
 * one lock, so no writer can slip a dirty line in between them. */
void discard_dcache_range(const void *base, unsigned int size)
{
    unsigned int line = cache_line();
    unsigned int start = (unsigned int)base;
    unsigned int end = start + size;
    unsigned int first = start & ~(line - 1);
    unsigned int last = (end + line - 1) & ~(line - 1);

    if (size == 0)
        return;

    cache_sync_enter();
    if (first != start || last != end)
        cache_hal_writeback_addr(first, last - first);
    cache_hal_invalidate_addr(first, last - first);
    cache_sync_exit();
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

/* Target-local helper, not one of Rockbox's cache entry points: the ESP32-S3
 * loads an image into two aliased cache windows, and lc-esp32s3.c needs to
 * know whether the ROM accepted a write-back over the data window before it
 * lets the image be executed through the other one. */
bool cache_writeback_range(unsigned int addr, unsigned int size)
{
    bool ok;

    cache_sync_enter();
    ok = Cache_WriteBack_Addr(addr, size) == 0;
    cache_sync_exit();
    return ok;
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

    cache_sync_enter();
    Cache_Invalidate_ICache_Items(addr, items);
    cache_sync_exit();
    return items;
}

void commit_dcache(void)
{
    cache_sync_enter();
    Cache_WriteBack_All();
    cache_sync_exit();
}

void commit_discard_dcache(void)
{
    /* Write back, then discard -- which is what every other port does (ARM's
     * DCCISW, MIPS's DCIndexWBInv) and what callers expect: usb_storage.c
     * calls this around SCSI data, mpegplayer and fft around buffers that are
     * filled and then read by other code. An earlier revision wrote back only,
     * on the argument that a clean line is not a stale one. That is true, and
     * it is not the whole contract: skipping the discard leaves lines that no
     * longer match memory after anything else has touched those pages -- a DMA
     * being the common case, and on this part the DMA masters do not go through
     * the cache. Invalidating after a full write-back cannot lose data, so the
     * contract is restored and the cost is only refetches on pages this
     * function is called precisely to hand over. */
    cache_sync_enter();
    Cache_WriteBack_All();
    Cache_Invalidate_DCache_All();
    cache_sync_exit();
}

void commit_discard_idcache(void)
{
    /* Write back first: the image was copied through the data window, and an
     * instruction fetch cannot see a line the write-back has not pushed out.
     * Then discard both caches, which is the contract (cpucache-arm.h:
     * "Discards the entire ICache, and commit+discards the entire DCache").
     * The fetch must not land on a line belonging to whatever occupied these
     * pages before the image was placed, and the data side must not keep
     * reading the copy it made while building that image. */
    cache_sync_enter();
    Cache_WriteBack_All();
    Cache_Invalidate_DCache_All();
    cache_ll_invalidate_all(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_INSTRUCTION,
                            CACHE_LL_ID_ALL);
    cache_sync_exit();
}

#endif /* CPU_XTENSA && !__SIMULATOR__ */
