#ifndef CACHE_XTENSA_H
#define CACHE_XTENSA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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
 * register work, take no lock, and are safe from an interrupt. They carry the
 * same address restriction, which is a fact about the part rather than about
 * the API: the resolver only accepts IRAM0 and DRAM0, so these are meaningful
 * only for buffers in internal SRAM -- which is where the audio and SD paths
 * put their descriptors and buffers anyway, for the separate reason that GDMA
 * takes a descriptor address from a 20-bit window based there (see AGENTS.md).
 * A PSRAM address is a no-op rather than an error.
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
/*
 * Narrow maintenance for a freshly placed image.
 *
 * The whole-cache operations above are correct but blunt: a full write-back
 * walks every dirty line of the external cache, and the audiobuffer, the
 * framebuffer, codecbuf and pluginbuf are megabytes of PSRAM that are dirty
 * during normal playback, so an image load pays for all of it (measured: about
 * 29 ms per codec load at 240 MHz, most of which is unrelated to the image).
 * Worse, doing it while audio and SD traffic are live coincided with aborts
 * reached from thread switching -- measured by bisect, the same codec-switching
 * soak that was clean on the build without cache maintenance faulted on the
 * build with the whole-cache version.
 *
 * The ROM exposes range forms, and these are the ones to use for an image:
 * write back the bytes that were copied, and drop only the instruction lines
 * for the pages that just became code. cache_addr_through_icache() answers
 * whether an address is served by the instruction cache at all, so the layout
 * is measured rather than assumed -- the ROM returns 0 for anything outside
 * the I-cache window.
 *
 * cache_invalidate_icache_range() returns the number of lines it dropped, or 0
 * if it cannot answer (no reported line size), which is the caller's cue to
 * fall back to the whole-cache form rather than to skip the maintenance.
 */
bool cache_writeback_range(unsigned int addr, unsigned int size);
bool cache_addr_through_icache(unsigned int addr);
unsigned int cache_invalidate_icache_range(unsigned int addr, unsigned int size);

/*
 * Whole-cache maintenance: the contract every arch implements under these
 * names (see firmware/target/arm/cpucache-arm.h for the wording). The core
 * calls commit_discard_idcache() after placing a relocatable image --
 * firmware/lc-rock.c, firmware/rolo.c and this target's load_code
 * implementation -- and before jumping into it.
 *
 * On esp32s3 the only operations that reach the caches are the ROM ones.
 * The core config has no cache instructions (probeD, flushD, lockD, wttag,
 * rinvalidate and binvalidate are all invalid opcodes), so the Tensilica HAL
 * entry points that other archs call are compiled away: xthal_dcache_all_writeback,
 * xthal_dcache_all_writeback_inv and xthal_icache_all_invalidate exist in the
 * esp32s3 toolchain as `entry a1, 16; retw.n` and nothing else, verified by
 * disassembly of a linked image. A commit_discard_idcache() built on them does
 * nothing at all, silently, and the symptom is the core fetching instructions
 * that were never written back -- an IllegalInstruction or a mid-instruction
 * PC in freshly loaded plugin or codec code.
 *
 * Range operations are no substitute here. cache_hal_writeback_addr() and
 * cache_hal_invalidate_addr() resolve the address through
 * cache_ll_vaddr_to_cache_level_id(), which on esp32s3 accepts only IRAM0 and
 * DRAM0, and plugin, codec and imageviewer decoder images are all placed in
 * PSRAM. The whole-cache ROM operations are what IDF's own cache_ll
 * invalidate-all routes to, so this uses the platform's layer where it exists
 * and the ROM prototype where it does not.
 *
 * Called from thread context only, once per image load: invalidating the
 * whole instruction cache costs a refetch of whatever runs afterwards, which
 * is far cheaper than the fault it prevents. This build is single-core, so no
 * cross-core coordination is required.
 *
 * Neither of these discards the data cache. Whole-cache invalidation of the
 * external D-cache would drop lines backing memory that is live while an image
 * loads -- the audiobuffer, the framebuffer, codecbuf and pluginbuf are all in
 * PSRAM -- and after a full write-back there is nothing that discarding gains.
 * Worth noting what the platform itself does: in this IDF the callers of
 * cache_hal_disable() and cache_hal_suspend() for CACHE_LL_LEVEL_EXT_MEM are
 * cpu_start, the bootloader, the app image loader and the TEE panic path --
 * all before the application is running. Nothing at runtime throws away the
 * external data cache, and this target does not either.
 */
void commit_dcache(void);
void commit_discard_dcache(void);
void commit_discard_idcache(void);

#endif /* CACHE_XTENSA_H */
