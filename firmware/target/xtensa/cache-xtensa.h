/***************************************************************************
 * Cache maintenance for DMA-facing buffers.
 *
 * Rockbox's drivers ask for these three by name; every other arch gets them
 * from its own asm. On esp32s3 the core config has no cache instructions at
 * all, and under ESP-IDF the cache is IDF's property, so the only supported
 * route is esp_cache_msync().
 *
 * Two rules that come from IDF's implementation rather than from the ISA:
 *
 *  - C2M and M2C cannot be combined into one call (esp_cache_msync.c rejects
 *    both_dir), so commit-then-discard is two calls, in that order.
 *  - Alignment is enforced unless UNALIGNED is passed. Rockbox hands over
 *    descriptor structs (12 bytes) and FIFO-sized buffers that are not cache
 *    line multiples, so the flag is always set here and IDF rounds out.
 *
 * The bare-metal route (calling the mask ROM's Cache_* entry points by
 * absolute address) is on the tagged branch baremetal-pre-idf.
 ****************************************************************************/

#ifndef CACHE_XTENSA_H
#define CACHE_XTENSA_H

#include <stddef.h>
#include "esp_cache.h"

#define CACHE_FLAGS_DATA  ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED

/* commit: the CPU's dirty lines go to memory before a DMA reads them */
static inline void commit_dcache_range(const void *base, unsigned int size)
{
    esp_cache_msync((void *)base, size, CACHE_FLAGS_DATA | ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

/* discard: drop the lines so the CPU re-reads what the DMA wrote */
static inline void discard_dcache_range(const void *base, unsigned int size)
{
    esp_cache_msync((void *)base, size,
                    CACHE_FLAGS_DATA | ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                    ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

static inline void commit_discard_dcache_range(const void *base, unsigned int size)
{
    commit_dcache_range(base, size);
    discard_dcache_range(base, size);
}

static inline unsigned int cache_dcache_line_size(void)
{
    return 32;  /* esp32s3 D-cache line, per cache_hal_get_cache_line_size */
}

#endif /* CACHE_XTENSA_H */
