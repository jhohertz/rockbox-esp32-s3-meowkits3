#ifndef CACHE_XTENSA_H
#define CACHE_XTENSA_H

#include <stddef.h>
#include <stdint.h>

/*
 * Cache maintenance for DMA-facing buffers.
 *
 * Rockbox's drivers ask for these by name; every other arch gets them from
 * inline asm in its own target header. esp32s3 has no cache instructions at
 * all (probeD, flushD, lockD, rdtag, wttag, rinvalidate and binvalidate are
 * invalid opcodes on this core config), so the work is done in
 * cache-xtensa.c, which is the only place on this path that includes the
 * platform's cache HAL. Keeping that include out of this header is what stops
 * the vendor's headers from reaching every object in the build: system-target.h
 * is reached from system.h, and an include here measured at 919 objects.
 *
 * The documented platform entry point is esp_cache_msync(), and it is the wrong
 * one here. Two things about its implementation decide that:
 *
 *  - It takes a _lock_t around the write-back path (the chunked C2M ops, which
 *    sdkconfig enables by default). The platform's lock code aborts when it is
 *    asked to block from a context that may not -- interrupt or critical
 *    section -- and Rockbox drivers do call these from exactly those contexts:
 *    the audio path commits its descriptors with interrupts off, as it does on
 *    every other target. The failure is an abort() inside lock_acquire_generic
 *    with nothing in the message pointing at the cache call.
 *  - It resolves the address through cache_hal_vaddr_to_cache_level_id(), which
 *    on esp32s3 accepts only IRAM0 and DRAM0. A buffer in the PSRAM window is
 *    rejected with "invalid addr or null pointer" and no maintenance happens at
 *    all -- the message reads like a null pointer bug and is really an address
 *    in a region the API will not touch.
 *
 * So cache-xtensa.c goes to the HAL entry points directly:
 * cache_hal_writeback_addr() and cache_hal_invalidate_addr() do the same
 * register work, take no lock, and are safe from an interrupt. They apply the
 * same address restriction, which is a fact about the part rather than about
 * the API: the D-cache only covers internal SRAM, so a DMA buffer has to live
 * there. That is the rule the audio and SD paths already follow (see AGENTS.md
 * on descriptor and buffer placement); passing a PSRAM address here is a no-op,
 * not an error.
 *
 * The bare-metal route (calling the mask ROM's Cache_* entry points by absolute
 * address) is on the tagged branch baremetal-pre-idf.
 */

/* commit: the CPU's dirty lines go to memory before a DMA reads them */
void commit_dcache_range(const void *base, unsigned int size);

/* discard: drop the lines so the CPU re-reads what the DMA wrote */
void discard_dcache_range(const void *base, unsigned int size);

void commit_discard_dcache_range(const void *base, unsigned int size);

/* esp32s3 D-cache line. Used for buffer alignment, so it needs no cache
 * operation and no platform header to answer it. */
unsigned int cache_dcache_line_size(void);

#endif /* CACHE_XTENSA_H */
