/***************************************************************************
 * Arm the cache MMU for Rockbox's own code and rodata.
 *
 * The mask ROM loader boots an ESP image at flash offset 0: it copies the RAM
 * segments and maps the flash-mapped ones through the cache MMU. It maps the
 * *data* side only. ESP-IDF's own second-stage bootloader is IRAM-only, which
 * is the clearest evidence: the ROM cannot fetch instructions from the IROM
 * window, so the bootloader is the thing that maps IROM for an application.
 * Rockbox has no second stage, so everything in .text is unreachable until we
 * map it. That is why crt0 lives in .iram and calls this before anything in
 * .text runs.
 *
 * MMU indexing, from soc/ext_mem_defs.h and hal/esp32s3/include/hal/mmu_ll.h:
 *   entry = (vaddr & 0x1FFFFFF) >> 16, page size 64 KB, 512 entries
 *   value = (paddr >> 16) | target,  bit 15 = PSRAM, bit 14 = invalid
 * The 512 entries cover one 32 MB linear range shared by both cache buses, so
 * 0x42000000 and 0x3C000000 are the same entry. app.lds puts IROM above the DROM
 * range for exactly that reason; with both at +0x20 they would overwrite each
 * other's mapping.
 *
 * The physical flash offset of a segment is decided by the packer, not by the
 * link, so it is only in the image header. The header is at flash offset 0, i.e.
 * physical page 0, and page 0 is linear address 0x3C000000 - entry 0. Arming
 * entry 0 by hand gives a view of the header without trusting what the ROM did.
 *
 * The function is ICODE_ATTR because it has to execute before the thing it is
 * mapping is reachable, and it cannot log: logging lives in the code that is not
 * mapped yet. The result is kept in irom_map_status for dbg_hw_info() to report.
 */
#include "config.h"
#include "cpu.h"
#include <stdint.h>

#define MMU_TABLE         0x600C5000
#define MMU_ENTRIES       512
#define MMU_PAGE          0x10000
#define MMU_LINEAR_MASK   0x1FFFFFF
#define MMU_INVALID       0x4000
#define MMU_TARGET_PSRAM  0x8000

#define DROM_VIEW         0x3C000000    /* linear 0 on the data bus */
#define FLASH_WINDOW_LOW  0x3C000000
#define FLASH_WINDOW_HIGH 0x44000000
#define IMAGE_MAGIC       0xE9
#define HEADER_SIZE       24
#define SEG_HEADER_SIZE   8

/* 0 = not run, -1 = header unreadable, -2 = header inconsistent,
 * otherwise the number of flash segments mapped. */
int irom_map_status;

static void ICODE_ATTR map_flash(uint32_t vaddr, uint32_t paddr, uint32_t len)
{
    volatile uint32_t *mmu = (volatile uint32_t *)MMU_TABLE;
    uint32_t first = (vaddr & MMU_LINEAR_MASK) >> 16;
    uint32_t phys = paddr >> 16;
    uint32_t pages = ((vaddr & 0xFFFF) + len + MMU_PAGE - 1) >> 16;
    uint32_t i;

    if (first + pages > MMU_ENTRIES)
        return;

    /* Consecutive entries map consecutive physical pages, which is what the
     * packer guarantees by aligning each segment to a page boundary. */
    for (i = 0; i < pages; i++)
        mmu[first + i] = phys + i;    /* flash target: bits 14 and 15 clear */
}

void ICODE_ATTR irom_mmu_map(void)
{
    volatile uint32_t *mmu = (volatile uint32_t *)MMU_TABLE;
    const uint32_t *hdr;
    unsigned off, count, i, mapped = 0;

    mmu[0] = 0;                       /* linear 0 -> page 0 = the image header */
    hdr = (const uint32_t *)DROM_VIEW;

    if ((hdr[0] & 0xFF) != IMAGE_MAGIC) {
        irom_map_status = -1;
        return;
    }

    count = hdr[1] & 0xFF;
    if (count == 0 || count > 32) {
        irom_map_status = -2;
        return;
    }

    off = HEADER_SIZE;
    for (i = 0; i < count; i++) {
        uint32_t addr = hdr[off];
        uint32_t len = hdr[off + 1];
        unsigned data = off + SEG_HEADER_SIZE;

        if (addr >= FLASH_WINDOW_LOW && addr < FLASH_WINDOW_HIGH) {
            map_flash(addr, data, len);
            mapped++;
        }

        off = data + len;
        /* The header is read through MMU entry 0, so only the first page of the
         * image is reachable this way. A segment table that long means this is
         * not the image we packed. */
        if (off >= MMU_PAGE)
            break;
    }

    irom_map_status = (int)mapped;
}
