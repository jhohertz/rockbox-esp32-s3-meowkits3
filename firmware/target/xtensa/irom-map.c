/***************************************************************************
 * Arm the cache MMU for Rockbox's own code and rodata.
 *
 * The mask ROM boots an ESP image at flash offset 0: it copies the RAM segments
 * and jumps to the entry point. It does not arm a single cache MMU entry -
 * Cache_MMU_Init writes 0x4000 (invalid) into all 512 of them and the loader
 * never maps anything back. ESP-IDF's own second-stage bootloader is IRAM-only,
 * which is the clearest evidence: nothing can fetch instructions from the IROM
 * window until the application maps it. Rockbox has no second stage, so
 * everything in .text is unreachable until we map it. That is why crt0 lives in
 * .iram and calls this before anything in .text runs.
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
 * physical page 0, and page 0 is linear address 0x3C000000 - entry 0. crt0 arms
 * entry 0 before calling this, because nothing else does.
 *
 * The function is ICODE_ATTR because it has to execute before the thing it is
 * mapping is reachable, and it cannot log: logging lives in the code that is not
 * mapped yet. The result is kept in irom_map_status for dbg_hw_info() to report.
 */
#include "config.h"
#include "cpu.h"
#include <stdint.h>
#include "bootlog.h"

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
 * otherwise the number of flash segments mapped.
 *
 * .srambss rather than .bss: crt0 reads these from internal SRAM before mmu_init()
 * has mapped the PSRAM window that .bss lives in.
 */
__attribute__((section(".srambss"))) int irom_map_status;

/* What the mapping loop computed for the IROM segment, so a memory dump can
 * compare the intent against what the MMU table actually contains.
 */
__attribute__((section(".srambss"))) int irom_first, irom_phys, irom_pages;

/* Deliberately not ICODE_ATTR: this is the first thing executed in the IROM
 * window, so it tells us whether the mapping crt0 just armed is actually
 * reachable. crt0 prints 'T' before it and 'K' after it returns.
 */
/* Which of the armed IROM pages can actually be fetched? Read the first word of
 * each page through both cache windows and put the triple in the boot log: a page
 * that reads back 0xffffffff is not reachable, and whatever code lands in it will
 * fault the moment control gets there.
 */
static void ICODE_ATTR irom_scan(void)
{
    volatile uint32_t *mmu = (volatile uint32_t *)MMU_TABLE;
    int e;

    boot_trace('S');
    for (e = 144; e < 150; e++) {
        boot_trace_hex(mmu[e]);
        boot_trace_hex(*(volatile uint32_t *)(DROM_VIEW + (uint32_t)e * MMU_PAGE));
        boot_trace_hex(*(volatile uint32_t *)(0x42000000u + (uint32_t)e * MMU_PAGE));
    }
}

int irom_canary(void)
{
    irom_scan();
    return 0x5a5a;
}

static void ICODE_ATTR map_flash(uint32_t vaddr, uint32_t paddr, uint32_t len)
{
    volatile uint32_t *mmu = (volatile uint32_t *)MMU_TABLE;
    uint32_t first = (vaddr & MMU_LINEAR_MASK) >> 16;
    uint32_t phys = paddr >> 16;
    uint32_t pages = ((vaddr & 0xFFFF) + len + MMU_PAGE - 1) >> 16;
    uint32_t i;

    if (first + pages > MMU_ENTRIES)
        return;

    if (vaddr >= 0x42000000) {
        irom_first = (int)first;
        irom_phys = (int)phys;
        irom_pages = (int)pages;
    }

    /* Consecutive entries map consecutive physical pages, which is what the
     * packer guarantees by aligning each segment to a page boundary. */
    for (i = 0; i < pages; i++)
        mmu[first + i] = phys + i;    /* flash target: bits 14 and 15 clear */
}

static void ICODE_ATTR mmu_self_map(uint32_t offset)
{
    volatile uint32_t *mmu = (volatile uint32_t *)MMU_TABLE;
    uint32_t page = offset >> 16;

    /* Below 32 MB the linear page number and the physical page number are the
     * same, so this is an identity mapping that makes the page readable.
     */
    mmu[(offset & MMU_LINEAR_MASK) >> 16] = page;
}

static uint32_t ICODE_ATTR le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void ICODE_ATTR irom_mmu_map(void)
{
    const unsigned char *hdr;
    unsigned off, count, i, mapped = 0;

    /* The header is not a multiple of four: magic and segment count are single
     * bytes, followed by the flash mode, the flash size/frequency code and the
     * entry point. Reading it as uint32_t[] puts the count in the wrong place.
     */
    hdr = (const unsigned char *)DROM_VIEW;

    if (hdr[0] != IMAGE_MAGIC) {
        irom_map_status = -1;
        return;
    }

    count = hdr[1];
    if (count == 0 || count > 32) {
        irom_map_status = -2;
        return;
    }

    off = HEADER_SIZE;
    for (i = 0; i < count; i++) {
        uint32_t addr, len;
        unsigned data;

        /* The descriptors are interleaved with the segment data, so the table is
         * not contiguous and each descriptor sits in a page that has not been
         * mapped yet. Arm the page we are about to read.
         */
        if ((off >> 16) < MMU_ENTRIES)
            mmu_self_map(off);

        addr = le32(hdr + off);
        len = le32(hdr + off + 4);
        data = off + SEG_HEADER_SIZE;

        if (addr >= FLASH_WINDOW_LOW && addr < FLASH_WINDOW_HIGH) {
            map_flash(addr, data, len);
            mapped++;
        }

        off = data + len;
    }

    irom_map_status = (int)mapped;
}
