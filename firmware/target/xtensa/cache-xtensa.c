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

#endif /* CPU_XTENSA && !__SIMULATOR__ */
