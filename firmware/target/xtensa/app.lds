#include "config.h"

ENTRY(start)

OUTPUT_FORMAT(elf32-xtensa-le)
STARTUP(target/xtensa/crt0.o)

/*
 * ESP32-S3 layout. Three things here come from the silicon, not from taste:
 *
 *  1. The instruction bus and the data bus see the same physical SRAM at
 *     different addresses: 0x40378000 on the instruction bus is 0x3FC88000 on the
 *     data bus (offset 0x6F0000). Code in .iram and data in .data/.bss therefore
 *     share memory. DRAMORIG is the alias of the end of the instruction region,
 *     which is reserved up front rather than computed from SIZEOF(.iram), because
 *     plugins are linked by a separate script that cannot see where the firmware's
 *     code ends.
 *  2. PSRAM and flash rodata share one 32 MB window on the external data cache.
 *     They are bank-switched per 64 KB page by the cache MMU. Because .rodata and
 *     .audiobuf are both in DROM here, the location counter already continues
 *     past rodata and no dummy section is needed - ESP-IDF needs one only because
 *     it puts PSRAM in a separate region.
 *  3. The MMU has 512 entries indexed by (vaddr & 0x1FFFFFF) >> 16, and that
 *     linear range is shared by the instruction and data buses: 0x42000000 and
 *     0x3C000000 are entry 0. IROM therefore cannot start at the same offset as
 *     DROM, or the two mappings overwrite each other.
 *  4. The audiobuffer is NOLOAD, so the image loader does not map its MMU pages.
 *     That mapping happens at runtime in mmu-xtensa.c, which is why audiobuffer
 *     must start on a 64 KB page boundary.
 *
 * Rockbox is entered by the mask ROM loader: it reads the image header at flash
 * offset 0, copies the RAM segments, maps the flash *data* side of the cache MMU
 * and jumps to the entry point. It does not map the instruction side - ESP-IDF's
 * second-stage bootloader does that, and it is IRAM-only so that the ROM can reach
 * it - and it does not bring up the external PSRAM. Both are crt0's job, which is
 * why .startup is placed in .iram rather than in .text: the entry point has to be
 * somewhere the ROM can already fetch from.
 */

/* Internal SRAM, data-bus view. The instruction bus sees the same physical memory
 * at IRAMORIG (offset 0x6F0000), so DRAMORIG has to be the alias of the end of the
 * whole instruction region: firmware .iram plus the reserve for plugins, which are
 * linked by a separate script that cannot see where the firmware's code ends.
 * 0x3FCF0000 is the top of the data-bus view. */
#define IRAMORIG 0x40378000
#define IRAMSIZE 0xA900
#define PLUGIN_IRAMORIG 0x40382900
#define DRAMORIG 0x3FC99900
/* ESP-IDF's memory.ld.in puts the top of internal SRAM at SRAM_IRAM_END 0x403CB700
 * on the instruction bus; the data bus sees the same bytes 0x6F0000 lower, so the
 * data-bus end is 0x3FCDB700. The previous 0x57000 ran 96 KB past the end of the
 * chip: .bss and the stack were being placed in memory that does not exist. */
#define SRAM_DATA_END 0x3FCDB700
#define DRAMSIZE (SRAM_DATA_END - DRAMORIG)

/* Flash instruction-cache window. The +0x20 is not cosmetic: the cache MMU
 * requires paddr % 64KB == vaddr % 64KB, and the image file header (0x18) plus
 * the segment header (0x8) make 0x20 land on a page boundary. esptool reads this
 * out of the program headers, so it has to stay.
 *
 * The 0x900000 is not cosmetic either. The MMU's 512 entries index one shared
 * 32 MB linear range, so entry (vaddr & 0x1FFFFFF) >> 16 is the same entry for
 * 0x42xxxxxx and 0x3Cxxxxxx. DROM plus the codec/plugin buffers claim entries
 * 0..128, so IROM has to start above them. */
#define IROMORIG 0x42900020
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
#define FRAMEBUFFER_SIZE (LCD_WIDTH * LCD_HEIGHT * (LCD_DEPTH / 8))
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

/* Exported so dbg_hw_info() can report how much of the static region is left.
 * Assigning to a named symbol rather than to `.` avoids the binutils behaviour
 * where an absolute constant in a dot-assignment is read as a section address. */
dramend = DRAMORIG + DRAMSIZE;

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

    /* Statics that do not need internal SRAM. They are NOLOAD, so they are not in
     * the image file and crt0 zeroes them; mmu_init() maps the MMU pages they
     * occupy onto PSRAM before anything reads them. Placing them here costs the
     * audiobuffer, which is sized from the location counter, so it shrinks
     * automatically.
     *
     * Both ends have to be on a 64 KB boundary, because the MMU maps whole pages.
     * Without the leading ALIGN the region would begin inside the page that ends
     * .rodata: the ROM loader has already mapped that page to flash, mmu_init()
     * skips pages that are already valid, and the first few tens of kilobytes of
     * the PSRAM region would silently read flash instead of PSRAM. Without the
     * trailing ALIGN the page numbering to codecbuf would not be contiguous.
     */
    /* .bss lives in the PSRAM window rather than internal SRAM: the 337 KB static
     * region has to hold the firmware's IRAM code, the plugin IRAM reserve, .data
     * and the stack, and the firmware's own statics are 280 KB, so it cannot fit.
     * It is NOLOAD, so the image loader does not touch it and crt0 zeroes it after
     * mmu_init() has mapped the pages. Both ends are page aligned because the MMU
     * maps whole 64 KB pages, and _psrambssstart is set here so mmu_init() covers
     * .bss as well as .psrambss.
     */
    .bss (NOLOAD) :
    {
        . = ALIGN(0x10000);
        _psrambssstart = .;
        _bssstart = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(0x10000);
        _bssend = .;
    } > DROM

    .psrambss (NOLOAD) :
    {
        *(.psrambss*)
        . = ALIGN(0x10000);
        _psrambssend = .;
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

    /* The 512 MMU entries index one shared 32 MB linear range, so an IROM page and
     * a DROM page with the same linear offset are the same entry. */
    ASSERT(((IROMORIG & 0x1FFFFFF) >> 16) >=
           ((CODECBUF_ORIG + CODECP_SIZE - DROMORIG) >> 16),
           "IROM starts inside the MMU pages claimed by DROM")

    /* VECBASE has to be a physical address the core can fetch from, so the vector
     * table lives here, on the instruction-bus view of internal SRAM, and cannot be
     * in the flash cache window. It is aligned to 1024 because the table spans
     * offsets 0x000 to 0x400.
     *
     * The handler has to sit right after the table: the `j` in the table has an 18-bit
     * signed range and cannot reach the flash window 1.6 MB away. C calls are not
     * limited because xtensacc uses -mlongcalls, which emits callx8 with an absolute
     * literal rather than a range-limited call8, so the dispatch can reach code in the
     * flash window. Only the hand-written `j` in the handler is range-limited, which
     * is why .icode.irq is placed immediately after the table.
     */
    .iram :
    {
        _iramstart = .;
        /* The entry point. It has to be on the instruction bus in internal SRAM,
         * because the ROM loader never maps the IROM side of the MMU; crt0 maps it
         * from here before anything in .text runs. */
        KEEP(*(.startup*))
        /* Bring-up exception trap table (crt0.S). It is a second VECBASE table, so
         * it needs the same 1024 alignment as the real one, and it has to be in
         * internal SRAM for the same reason.
         */
        . = ALIGN(1024);
        KEEP(*(.ivectors*))
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

    /* Statics that must be reachable before the MMU is armed, or that a DMA engine
     * reads directly. GDMA takes the descriptor address from a 20-bit register based
     * at 0x3FC00000, so a descriptor in PSRAM loses its high bits; the descriptor
     * rings and the ping-pong buffers have to stay in internal SRAM.
     */
    .srambss (NOLOAD) :
    {
        . = ALIGN(0x4);
        _srambssstart = .;
        *(.srambss*)
        . = ALIGN(0x4);
        _srambssend = .;
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
           ".iram, .data, .bss and the stack do not fit in internal SRAM")
    ASSERT(_iramend <= IRAMORIG + IRAMSIZE,
           ".iram overflows the region reserved for firmware code")
    ASSERT(PLUGIN_IRAMORIG + 0x6A00 <= DRAMORIG + 0x6F0000,
           "plugin IRAM aliases into the firmware data region")

    /DISCARD/ :
    {
        *(.eh_frame)
    }
}
