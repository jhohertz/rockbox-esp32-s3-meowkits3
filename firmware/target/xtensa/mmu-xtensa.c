/***************************************************************************
 * ESP32-S3 external data cache MMU.
 *
 * The 32 MB window at 0x3C000000 is shared by flash rodata and PSRAM. It is
 * bank-switched per 64 KB page by a 512-entry table of 16-bit values at
 * 0x600C5000:
 *
 *   bit 15   target: 0 = flash, 1 = PSRAM
 *   bit 14   invalid
 *   bits 0-13 physical page number
 *
 * ESP-IDF's second-stage bootloader maps the pages for the segments present in
 * the image, which covers .rodata. It does not map the audiobuffer, because the
 * audiobuffer is NOLOAD and therefore not in the image file. So Rockbox has to
 * map its own pages here, before anything reads the audiobuffer.
 *
 * This deliberately does not touch entries that are already valid: those belong
 * to the rodata mapping the bootloader established.
 ****************************************************************************/
#include <stdint.h>
#include "config.h"

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

/* Defined by the linker script (target/xtensa/app.lds). */
extern unsigned char audiobuffer[];
extern unsigned char audiobufend[];

/* The MMU indexes by the masked address, not by the address itself: the window
 * is 32 MB and the mask is 0x1FFFFFF, so 0x3C010000 is entry 1, not 15361. */
static inline int mmu_index(unsigned int vaddr)
{
    return (int)((vaddr & MMU_VADDR_MASK) / MMU_PAGE_SIZE);
}

void mmu_init(void)
{
    volatile unsigned short *table = (volatile unsigned short *)MMU_TABLE_BASE;

    /* Cast through an integer rather than arithmetic on the linker arrays: they
     * have unknown size, so `audiobufend - 1` trips -Warray-bounds. */
    int first = mmu_index((unsigned int)(uintptr_t)audiobuffer);
    int last  = mmu_index((unsigned int)(uintptr_t)audiobufend - 1);

    if (first < 0 || last >= MMU_ENTRY_NUM) return;   /* window overrun */

    for (int i = first; i <= last; i++) {
        if (table[i] & MMU_INVALID) {
            table[i] = MMU_TARGET_PSRAM | (PSRAM_PADDR_BASE + (i - first));
        }
        /* Already valid means the bootloader mapped it (rodata). Leave it. */
    }
}

#endif /* CPU_XTENSA */
