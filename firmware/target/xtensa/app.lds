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

/* Flash data-cache window, shared with PSRAM. The window is 32 MB wide; we only
 * claim MEMORYSIZE worth of it, and the rest is left to whatever the bootloader
 * mapped for flash rodata. */
#define DROMORIG 0x3C000020

/* MEMORYSIZE is the PSRAM size on this target. On every existing Rockbox target
 * MEMORYSIZE is one contiguous region that the audiobuffer lives in; here the
 * audiobuffer, the framebuffer and the codec/plugin buffers all share PSRAM, so
 * they have to be split explicitly.
 *
 * The codec and plugin buffers sit at the top of the PSRAM part of the window at a
 * fixed address. apps/plugins/plugin.lds computes CODEC_ORIGIN from MEMORYSIZE,
 * PLUGIN_BUFFER_SIZE and CODEC_SIZE, and apps/codecs.c rejects a codec whose load
 * address is not exactly codecbuf, so the two scripts have to agree and the only
 * way to make them agree is to pin the address. */
#define FRAMEBUFFER_SIZE (240 * 320 * 2 * 2)
#define CODECP_SIZE (CODEC_SIZE + PLUGIN_BUFFER_SIZE)
#define CODECBUF_ORIG (DROMORIG + MEMORYSIZE * 0x100000 - CODECP_SIZE)
#define MIN_STACK_SIZE 0x1000

MEMORY
{
    DRAM : ORIGIN = DRAMORIG, LENGTH = DRAMSIZE
    IRAM : ORIGIN = IRAMORIG, LENGTH = IRAMSIZE
    IROM : ORIGIN = IROMORIG, LENGTH = IROMSIZE
    DROM : ORIGIN = DROMORIG, LENGTH = MEMORYSIZE * 0x100000 - CODECP_SIZE
    CODECP : ORIGIN = CODECBUF_ORIG, LENGTH = CODECP_SIZE
}

SECTIONS
{
    .text :
    {
        /* -ffunction-sections gives each function its own .literal.<fn> and
         * .text.<fn>. An l32r can only reach a literal placed before the use, so
         * every literal section has to come before every text section - the same
         * ordering ESP-IDF's own linker script uses. With *(.text*) alone the
         * unmatched .literal.* sections fall to the end of the script and ld
         * reports "dangerous relocation: l32r: literal placed after use".
         */
        KEEP(*(.literal))
        KEEP(*(.startup*))
        KEEP(*(.literal.*))
        KEEP(*(.text))
        KEEP(*(.text.*))
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
        /* rodata already took the low part of the window, so the audiobuffer is
         * whatever is left between it and the framebuffer reservation at the top of
         * DROM. Sizing it from the location counter is the only way to account for
         * rodata, whose size is not known until the link is done.
         *
         * ORIGIN()/LENGTH() rather than the literal addresses: this binutils treats
         * an assignment whose expression contains a bare absolute constant as an
         * absolute section address, and then computes the region usage from that
         * address instead of from the section size, reporting a spurious overflow. */
        . = . + LENGTH(DROM) - FRAMEBUFFER_SIZE - (. - ORIGIN(DROM));
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
        . = ALIGN(0x4);
        framebufferend = .;
    } > DROM

    ASSERT(framebufferend <= CODECBUF_ORIG,
           "rodata plus the framebuffer does not fit below the codec buffer")

    /* VECBASE has to be a physical address the core can fetch from, so the vector
     * table lives here, on the instruction-bus view of internal SRAM, and cannot be
     * in the flash cache window. It is aligned to 1024 because the table spans
     * offsets 0x000 to 0x400.
     *
     * The handler has to sit right after the table: the `j` in the table has an 18-bit
     * signed range and cannot reach the flash window 1.6 MB away. C calls are not
     * limited because xtensacc uses -mlongcalls, which emits callx0 with an absolute
     * literal rather than a range-limited call8, so the dispatch can reach code in the
     * flash window. Only the hand-written call0 in the handler is range-limited, which
     * is why .icode.irq is placed immediately after the table.
     */
    .iram :
    {
        _iramstart = .;
        /* ld evaluates ASSERT at the end of the enclosing section rather than at the
         * point where it is written, so an alignment check here would test the wrong
         * thing. IRAMORIG is a multiple of 1024 and the ALIGN below puts the table on
         * a boundary, which is what VECBASE needs. */
        . = ALIGN(1024);
        KEEP(*(.vector))
        KEEP(*(.icode.irq.literal*))
        KEEP(*(.icode.irq))
        /* ICODE_ATTR functions keep their literals inside .icode when compiled with
         * -mtext-section-literals, so the l32r in the handler reaches them. The
         * literal still has to come before the use, so it is placed first. */
        KEEP(*(.icode.literal*))
        KEEP(*(.icode))
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

    /* The kernel's thread code reads stackbegin/stackend as symbols, so they have
     * to be defined here rather than as a computed value. */
    .stack (NOLOAD) :
    {
        . = ALIGN(0x10);              /* Xtensa ABI keeps the stack on 16 bytes */
        stackbegin = .;
        . = . + MIN_STACK_SIZE;
        stackend = .;
        /* crt0.S sets a1 from this before the kernel takes over */
        _stackend = .;
    } > DRAM

    /* Codecs and plugins are loaded from storage into these buffers. The address is
     * pinned so it equals the CODEC_ORIGIN/PLUGIN_ORIGIN that plugin.lds puts into
     * the loaded image header. mmu-xtensa.c maps the MMU pages for this range.
     * rolo.c wants loadaddress, which is the end of the usable PSRAM. */
    .codecbuf (NOLOAD) :
    {
        codecbuf = .;
        . = . + CODEC_SIZE;
        pluginbuf = .;
        . = . + PLUGIN_BUFFER_SIZE;
        loadaddress = .;
        psramend = .;
    } > CODECP

    ASSERT(stackend <= DRAMORIG + DRAMSIZE,
           ".data, .bss and the stack do not fit in the static DRAM region")
    ASSERT(IRAMSIZE <= 0x8000, "IRAM alias would overlap DRAM")

    /DISCARD/ :
    {
        *(.eh_frame)
    }
}
