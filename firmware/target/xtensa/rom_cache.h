/***************************************************************************
 * ROM cache functions, reached by absolute address.
 *
 * Rockbox links -nostdlib and cannot use ESP-IDF, and the esp32s3 core config
 * has no cache instructions (probeD/flushD/lockD/rdtag/wttag/rinvalidate/
 * binvalidate are all invalid on this core). So cache maintenance has to go
 * through the ROM.
 *
 * These addresses come from components/esp_rom/esp32s3/ld/esp32s3.rom.ld. They
 * are in the base file rather than esp32s3.rom.version.ld, so they do not move
 * between ROM revisions.
 *
 * Every call goes through xt_rom_call(), a plain indirect call compiled for the
 * windowed ABI - see rom_cache.c.
 ****************************************************************************/

#ifndef ROM_CACHE_H
#define ROM_CACHE_H

#define ROM_CACHE_GET_DCACHE_LINE_SIZE 0x40001608
#define ROM_CACHE_INVALIDATE_ADDR       0x400016b0
#define ROM_CACHE_WRITEBACK_ADDR        0x400016c8
#define ROM_CACHE_INVALIDATE_DCACHE_ALL 0x400016e0
#define ROM_CACHE_WRITEBACK_ALL         0x400016f8

extern unsigned int xt_rom_call(unsigned int fn,
                                unsigned int arg0,
                                unsigned int arg1);

static inline unsigned int cache_dcache_line_size(void)
{
    return xt_rom_call(ROM_CACHE_GET_DCACHE_LINE_SIZE, 0, 0);
}

/* Commit the CPU's dirty lines, then drop them so the next read sees whatever
 * the DMA wrote. A DMA bypasses the cache, so both halves are needed: writing
 * back before the DMA reads, invalidating before the CPU reads again. */
static inline void commit_discard_dcache_range(const void *base, unsigned int size)
{
    xt_rom_call(ROM_CACHE_WRITEBACK_ADDR, (unsigned int)base, size);
    xt_rom_call(ROM_CACHE_INVALIDATE_ADDR, (unsigned int)base, size);
}

static inline void commit_dcache_range(const void *base, unsigned int size)
{
    xt_rom_call(ROM_CACHE_WRITEBACK_ADDR, (unsigned int)base, size);
}

static inline void discard_dcache_range(const void *base, unsigned int size)
{
    xt_rom_call(ROM_CACHE_INVALIDATE_ADDR, (unsigned int)base, size);
}

#endif /* ROM_CACHE_H */
