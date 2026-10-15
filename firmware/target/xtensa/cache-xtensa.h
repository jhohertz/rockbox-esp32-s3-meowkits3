/***************************************************************************
 * Cache maintenance for DMA-facing buffers.
 *
 * Rockbox's drivers ask for these three by name; every other arch gets them
 * from its own asm. On esp32s3 the core config has no cache instructions at
 * all, and under ESP-IDF the cache is IDF's property, so the maintenance has to
 * come from IDF.
 *
 * The documented entry point is esp_cache_msync(), and it is the wrong one
 * here. Two things about its implementation decide that:
 *
 *  - It takes a _lock_t around the write-back path (the chunked C2M ops, which
 *    sdkconfig enables by default). IDF's lock code aborts when it is asked to
 *    block from a context that may not -- interrupt or critical section -- and
 *    Rockbox drivers do call these from exactly those contexts: the audio path
 *    commits its descriptors with interrupts off, as it does on every other
 *    target. The failure is an abort() inside lock_acquire_generic with nothing
 *    in the message pointing at the cache call.
 *  - It resolves the address through cache_hal_vaddr_to_cache_level_id(), which
 *    on esp32s3 accepts only IRAM0 and DRAM0. A buffer in the PSRAM window is
 *    rejected with "invalid addr or null pointer" and no maintenance happens at
 *    all -- the message reads like a null pointer bug and is really an address
 *    in a region the API will not touch.
 *
 * So this goes to the HAL entry points directly: cache_hal_writeback_addr() and
 * cache_hal_invalidate_addr() do the same register work, take no lock, and are
 * safe from an interrupt. They apply the same address restriction, which is a
 * fact about the part rather than about the API: the D-cache only covers
 * internal SRAM, so a DMA buffer has to live there. That is the rule the audio
 * and SD paths already follow (see AGENTS.md on descriptor and buffer
 * placement); passing a PSRAM address here is a no-op, not an error.
 *
 * The bare-metal route (calling the mask ROM's Cache_* entry points by absolute
 * address) is on the tagged branch baremetal-pre-idf.
 ****************************************************************************/

#ifndef CACHE_XTENSA_H
#define CACHE_XTENSA_H

#include <stddef.h>
#include "hal/cache_hal.h"

/* commit: the CPU's dirty lines go to memory before a DMA reads them */
static inline void commit_dcache_range(const void *base, unsigned int size)
{
    cache_hal_writeback_addr((uint32_t)base, size);
}

/* discard: drop the lines so the CPU re-reads what the DMA wrote */
static inline void discard_dcache_range(const void *base, unsigned int size)
{
    cache_hal_invalidate_addr((uint32_t)base, size);
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
