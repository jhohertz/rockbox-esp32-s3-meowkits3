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
#include "esp32s3/rom/cache.h"   /* Cache_WriteBack_All, not wrapped by the LL */

void commit_dcache_range(const void *base, unsigned int size)
{
    cache_hal_writeback_addr((uint32_t)base, size);
}

void discard_dcache_range(const void *base, unsigned int size)
{
    cache_hal_invalidate_addr((uint32_t)base, size);
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

void commit_dcache(void)
{
    Cache_WriteBack_All();
}

void commit_discard_dcache(void)
{
    Cache_WriteBack_All();
    cache_ll_invalidate_all(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA,
                            CACHE_LL_ID_ALL);
}

void commit_discard_idcache(void)
{
    /* Write back first, then invalidate: invalidating a D-cache line that is
     * still dirty discards the write, and an instruction fetch after this
     * must not find a line belonging to whatever occupied these pages before
     * the image was placed. See cache-xtensa.h for why neither half can be a
     * range operation on this part. */
    Cache_WriteBack_All();
    cache_ll_invalidate_all(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL,
                            CACHE_LL_ID_ALL);
}

#endif /* CPU_XTENSA && !__SIMULATOR__ */
