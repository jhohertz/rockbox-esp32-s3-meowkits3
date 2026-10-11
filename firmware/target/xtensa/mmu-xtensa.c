/***************************************************************************
 * ESP32-S3 external data cache MMU.
 *
 * The 32 MB window at 0x3C000000 is shared by flash rodata and PSRAM. It is
 * bank-switched per 64 KB page by a 512-entry table of 32-bit values at
 * 0x600C5000 (DR_REG_MMU_TABLE):
 *
 *   bit 15   target: 0 = flash, 1 = PSRAM
 *   bit 14   invalid
 *   bits 0-13 physical page number
 *
 * The ROM loader maps the pages for the segments present in the image, which
 * covers .rodata. It does not map .psrambss, the audiobuffer, the framebuffer or
 * the codec/plugin buffers, because all of them are NOLOAD and therefore not in
 * the image file. So Rockbox has to map its own pages here, before anything reads
 * them.
 *
 * Mapping a page to PSRAM is only meaningful once the chip has been brought up;
 * see psram-xtensa.c, which crt0 runs before this.
 ****************************************************************************/
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "rom_cache.h"
#include "psram-xtensa.h"
#include "bootlog.h"

#ifdef CPU_XTENSA

#define MMU_TABLE_BASE      0x600C5000
#define MMU_ENTRY_NUM       512
#define MMU_PAGE_SIZE       0x10000        /* 64 KB */
#define MMU_DBUS_VADDR_BASE 0x3C000000
#define MMU_VADDR_MASK      0x1FFFFFF

#define MMU_INVALID         (1 << 14)
#define MMU_TARGET_PSRAM    (1 << 15)
#define MMU_PADDR_MASK      0x3FFF

/* Physical page numbering of the PSRAM chip: page 0 is its first 64 KB. */
#define PSRAM_PADDR_BASE    0

/* Defined by the linker script (target/xtensa/app.lds). _psrambssstart is the
 * first NOLOAD region in the window, psramend the last; everything between them is
 * PSRAM. */
extern unsigned char _psrambssstart[];
extern unsigned char psramend[];

/* The MMU indexes by the masked address, not by the address itself: the window
 * is 32 MB and the mask is 0x1FFFFFF, so 0x3C010000 is entry 1, not 15361. */
static inline int mmu_index(unsigned int vaddr)
{
    return (int)((vaddr & MMU_VADDR_MASK) / MMU_PAGE_SIZE);
}

void mmu_init(void)
{
    volatile uint32_t *table = (volatile uint32_t *)MMU_TABLE_BASE;

    boot_trace('m');

    /* Mapping a page to PSRAM is pointless - and actively wrong - if the chip was
     * not brought up. psram_init() has already run by the time we get here.
     */
    if (!psram_present())
        return;

    /* Cast through an integer rather than arithmetic on the linker arrays: they
     * have unknown size, so `psramend - 1` trips -Warray-bounds. */
    int first = mmu_index((unsigned int)(uintptr_t)_psrambssstart);
    int last  = mmu_index((unsigned int)(uintptr_t)psramend - 1);

    if (first < 0 || last >= MMU_ENTRY_NUM) return;   /* window overrun */

    for (int i = first; i <= last; i++) {
        if (table[i] & MMU_INVALID) {
            table[i] = MMU_TARGET_PSRAM | (PSRAM_PADDR_BASE + (i - first));
        }
        /* Already valid means the bootloader mapped it (rodata). Leave it. */
    }

    /* A page that was invalid before now points at PSRAM, and any line the cache
     * kept for that range is stale. ESP-IDF does the same thing after every
     * esp_mmu_map(). */
    discard_dcache_range((const void *)_psrambssstart,
                         (unsigned int)((uintptr_t)psramend -
                                       (uintptr_t)_psrambssstart));
}

#endif /* CPU_XTENSA */
