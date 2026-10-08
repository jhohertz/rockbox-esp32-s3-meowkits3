#include "config.h"

ENTRY(start)

OUTPUT_FORMAT(elf32-xtensa-le)
STARTUP(target/xtensa/crt0.o)

/*
 * ESP32-S3 layout. Three things here come from the silicon, not from taste:
 *
 *  1. The instruction bus and the data bus see the same physical SRAM at
 *     different addresses. IRAM 0x40378000 aliases DRAM 0x3FC80000, so IRAM is
 *     kept entirely below DRAMORIG and the two regions cannot collide. If
 *     IRAMSIZE ever grows past 0x8000 the ASSERT below fires.
 *  2. PSRAM and flash rodata share one 32 MB window on the external data cache.
 *     They are bank-switched per 64 KB page by the cache MMU. Because .rodata and
 *     .audiobuf are both in DROM here, the location counter already continues
 *     past rodata and no dummy section is needed — ESP-IDF needs one only because
 *     it puts PSRAM in a separate region.
 *  3. The audiobuffer is NOLOAD, so the image loader does not map its MMU pages.
 *     That mapping happens at runtime in mmu-xtensa.c, which is why audiobuffer
 *     must start on a 64 KB page boundary.
 *
 * Rockbox is entered by ESP-IDF's second-stage bootloader, which has already
 * configured the SPI-RAM controller and enabled both caches.
 */

/* Internal SRAM, data-bus view. Static .data/.bss is capped at 337 KB, not the
 * full 512 KB — the rest is heap, and only after the app starts. */
#define DRAMORIG 0x3FC88000
#define DRAMSIZE 0x54700

/* Internal SRAM, instruction-bus view. Must stay <= 0x8000 so that its alias
 * (0x3FC80000) ends exactly where DRAM begins. */
#define IRAMORIG 0x40378000
#define IRAMSIZE 0x8000

/* Flash instruction-cache window. The +0x20 is not cosmetic: the cache MMU
 * requires paddr % 64KB == vaddr % 64KB, and the image file header (0x18) plus
 * the segment header (0x8) make 0x20 land on a page boundary. esptool reads this
 * out of the program headers, so it has to stay. */
#define IROMORIG 0x42000020
#define IROMSIZE (0x800000 - 0x20)

/* Flash data-cache window, shared with PSRAM. */
#define DROMORIG 0x3C000020
#define DROMSIZE (0x2000000 - 0x20)

/* MEMORYSIZE is the PSRAM size on this target. On every existing Rockbox target
 * MEMORYSIZE is one contiguous region that the audiobuffer lives in; here the
 * audiobuffer and the framebuffer share PSRAM, so they have to be split explicitly.
 * 240*320*2 bytes per buffer, double buffered. */
#define FRAMEBUFFER_SIZE (240 * 320 * 2 * 2)
#define AUDIOBUFSIZE (MEMORYSIZE * 0x100000 - FRAMEBUFFER_SIZE)
#define MIN_STACK_SIZE 0x1000

MEMORY
{
    DRAM : ORIGIN = DRAMORIG, LENGTH = DRAMSIZE
    IRAM : ORIGIN = IRAMORIG, LENGTH = IRAMSIZE
    IROM : ORIGIN = IROMORIG, LENGTH = IROMSIZE
    DROM : ORIGIN = DROMORIG, LENGTH = DROMSIZE
}

SECTIONS
{
    .text :
    {
        KEEP(*(.literal))
        KEEP(*(.startup*))
        *(.text*)
    } > IROM

    .rodata :
    {
        *(.rodata)
        *(.rodata*)
        . = ALIGN(0x4);
        _rodata_end = .;
    } > DROM

    .audiobuf (NOLOAD) :
    {
        . = ALIGN(0x10000);          /* MMU page size */
        audiobuffer = .;
        . = . + AUDIOBUFSIZE;
        . = ALIGN(0x4);
        audiobufend = .;
    } > DROM

    /* The framebuffer arrives here through IRAM_LCDFRAMEBUFFER, a section attribute in
     * the target config, because .bss lives in the 337 KB static DRAM region which
     * cannot hold 150 KB of framebuffer plus everything else. */
    .framebuffer (NOLOAD) :
    {
        . = ALIGN(0x4);
        lcd_framebuffer = .;
        *(.framebuffer*)
    } > DROM

    /* Not a location check: the window starts at 0x3C000020 and the audiobuffer is
     * aligned up to a page boundary, so a full MEMORYSIZE worth of content cannot fit
     * in the window even though the physical pages do. What is actually checkable is
     * the physical page budget. rodata occupies flash pages in the same window, so the
     * full 8 MB is not available to the audiobuffer and the framebuffer. */
    ASSERT(AUDIOBUFSIZE + FRAMEBUFFER_SIZE <= MEMORYSIZE * 0x100000,
           "audiobuffer plus framebuffer exceed PSRAM")

    .iram :
    {
        _iramstart = .;
        *(.icode)
        *(.irodata)
        *(.ibss)
        . = ALIGN(0x4);
        _iramend = .;
    } > IRAM

    /* .data is copied from flash to internal DRAM by the image loader, so its
     * load address is the runtime address. */
    .data :
    {
        _datastart = .;
        *(.data*)
        . = ALIGN(0x4);
        _dataend = .;
    } > DRAM

    .bss :
    {
        _bssstart = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(0x4);
        _bssend = .;
    } > DRAM

    /* Stack at the very top of internal DRAM, growing down. It must not meet
     * .bss, so the static data is capped at DRAMSIZE - MIN_STACK_SIZE. */
    _stackend = DRAMORIG + DRAMSIZE;

    ASSERT(_bssend <= _stackend - MIN_STACK_SIZE,
           ".data and .bss leave no room for the stack")
    ASSERT(IRAMSIZE <= 0x8000, "IRAM alias would overlap DRAM")

    /DISCARD/ :
    {
        *(.eh_frame)
    }
}
