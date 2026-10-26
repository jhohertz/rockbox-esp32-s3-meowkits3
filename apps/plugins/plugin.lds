#define __ASSEMBLER__
#include "config.h"
#include "cpu.h"

#ifdef CPU_XTENSA
/* The text/data window split below and the buffer symbols in the firmware's
 * own linker fragments have to agree exactly -- apps/codecs.c and apps/plugin.c
 * reject a loaded image whose load_addr is not codecbuf or pluginbuf -- so the
 * numbers live in one place. */
#include "memory-xtensa.h"
#endif

/* These output formats should be in the config-files */

#ifdef CPU_COLDFIRE
OUTPUT_FORMAT(elf32-m68k)
#elif defined(CPU_ARM)
OUTPUT_FORMAT(elf32-littlearm)
#elif defined(CPU_MIPS)
OUTPUT_FORMAT(elf32-littlemips)
#elif defined(CPU_XTENSA)
OUTPUT_FORMAT(elf32-xtensa-le)
#else
/* We can have an #error here we don't use this file when build sims! */
#error Unknown CPU architecture
#endif

/* MCF5249 have 96KB of IRAM */
#if CONFIG_CPU == MCF5249
#define DRAMORIG 0x31000000
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE
#define IRAMORIG 0x1000c000
#define IRAMSIZE 0xc000

/* MCF5250 have 128KB of IRAM */
#elif CONFIG_CPU == MCF5250
#define DRAMORIG 0x31000000
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE
#define IRAMORIG 0x1000c800
#define IRAMSIZE 0x13800

#elif CONFIG_CPU == PP5022 || CONFIG_CPU == PP5024
/* PP5022/24 have 128KB of IRAM */
#define DRAMORIG 0x00000000
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE
#define IRAMORIG 0x4000c000
#define IRAMSIZE 0x14000

#elif defined(CPU_PP)
/* all other PP's have 96KB of IRAM */
#define DRAMORIG 0x00000000
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE
#define IRAMORIG 0x4000c000
#define IRAMSIZE 0x0c000

#elif CONFIG_CPU == IMX31L
#define DRAMORIG 0x0
#define DRAMSIZE ((MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE \
                  - CODEC_SIZE - QHARRAY_SIZE - FRAME_SIZE - TTB_SIZE)

#elif CONFIG_CPU == S3C2440
#define DRAMORIG 0x0
#define DRAMSIZE ((MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE \
                  - CODEC_SIZE - LCD_BUFFER_SIZE - TTB_SIZE)

#elif CONFIG_CPU==DM320
#define DRAMORIG CONFIG_SDRAM_START
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE - LCD_TTB_AREA
/* The bit of IRAM that is available is used in the core */

#elif CONFIG_CPU==TCC7801
#define DRAMORIG 0x20000000
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE - TTB_SIZE
#define IRAMORIG 0x1000c000
#define IRAMSIZE 0xc000

#elif CONFIG_CPU==AS3525 && !defined(AMS_LOWMEM)
#define DRAMORIG DRAM_ORIG
#define DRAMSIZE (DRAM_SIZE - PLUGIN_BUFFER_SIZE - CODEC_SIZE - TTB_SIZE)
#define IRAMORIG (IRAM_ORIG + 0x20000)
#define IRAMSIZE (IRAM_ORIG + IRAM_SIZE - IRAMORIG)

#elif CONFIG_CPU==AS3525v2 || (CONFIG_CPU == AS3525 && defined(AMS_LOWMEM))
#define DRAMORIG DRAM_ORIG
#define DRAMSIZE (DRAM_SIZE - PLUGIN_BUFFER_SIZE - TTB_SIZE)
/* No IRAM defined, instead IRAM is exclusively used for the entire codec */
#define CODEC_ORIGIN (IRAM_ORIG + IRAM_SIZE - CODEC_SIZE)
#define PLUGIN_ORIGIN (DRAM_ORIG + DRAMSIZE)

#elif CONFIG_CPU==S5L8700
/* S5L8700 have 256KB of IRAM */
#define DRAMORIG 0x08000000
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE
#define IRAMORIG (0x00000000 + (48*1024))
#define IRAMSIZE (208*1024)

#elif CONFIG_CPU==S5L8701
/* S5L8701 have 176KB of IRAM */
#define DRAMORIG 0x08000000
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE
#define IRAMORIG (0x00000000 + (48*1024))
#define IRAMSIZE (128*1024)

#elif CONFIG_CPU==S5L8702
/* S5L8702 have 256KB of IRAM */
/* IRAM1 (2nd 128KB block) is not used because it is slower than DRAM */
#define DRAMORIG 0x08000000
#define DRAMSIZE (DRAM_SIZE - PLUGIN_BUFFER_SIZE - CODEC_SIZE - TTB_SIZE)
#define IRAMORIG (0x00000000 + (48*1024))
#define IRAMSIZE (80*1024)

#elif CONFIG_CPU==S5L8720
/* S5L8720 have 192KB of IRAM */
/* TODO: check if IRAM1 is also slow as in S5L8702 above */
#define DRAMORIG 0x08000000
#define DRAMSIZE (DRAM_SIZE - PLUGIN_BUFFER_SIZE - CODEC_SIZE - TTB_SIZE)
#define IRAMORIG (0x00000000 + (56*1024))
#define IRAMSIZE (136*1024)

#elif CONFIG_CPU == JZ4732 || CONFIG_CPU == JZ4760B
/* fake 'iram' region occupies first 16k */
#define DRAMORIG 0x80004000
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE - 0x4000
/* The bit of IRAM that is available is used in the core */

#elif CONFIG_CPU == X1000
#define DRAMORIG X1000_DRAM_BASE
#define DRAMSIZE (X1000_DRAM_SIZE - PLUGIN_BUFFER_SIZE - CODEC_SIZE)

#elif CONFIG_CPU == RK27XX
#define DRAMORIG 0x60000000
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE

#elif CONFIG_CPU == IMX233
/* The IRAM is too small and already partly used by the core */
#define DRAMORIG CACHED_DRAM_ADDR
#define DRAMSIZE (DRAM_SIZE - PLUGIN_BUFFER_SIZE - CODEC_SIZE - FRAME_SIZE - TTB_SIZE)

#elif defined(CPU_XTENSA)
/* ESP32-S3. Codecs and plugins live at the top of the PSRAM part of the external
 * data-cache window. apps/codecs.c and apps/plugin.c reject any image whose
 * declared load_addr is not exactly &codecbuf / &pluginbuf, so this script and
 * the firmware MUST produce the same numbers -- and the only way to guarantee
 * that is to derive them from the same header rather than restate them here.
 *
 * They were restated, as `DRAMORIG 0x3C000020` plus the size formula, which
 * silently diverged the moment memory-xtensa.h moved codecbuf down by 0x20 to
 * bring the arena back inside the mapped MMU window: the firmware's codecbuf
 * became 0x3c680000 while every codec still linked at 0x3c680020, and each was
 * then refused on load. The comment above has always claimed the numbers live in
 * one place; now they actually do.
 *
 * IRAM here is the instruction-bus view of internal SRAM. It has to sit in the
 * gap app.lds leaves between the firmware's .iram and the firmware's data region,
 * because .data/.bss alias the same physical memory from the data bus: 0x40378000
 * is 0x3FC88000. */
#define DRAMORIG  XT_DROMORIG
#define DRAMSIZE  (XT_CODECBUF_ADDR - XT_DROMORIG)
#define IRAMORIG 0x40382900
#define IRAMSIZE 0x6A00

#elif CONFIG_CPU == STM32H743
#define DRAMORIG STM32_SDRAM1_BASE
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE - CODEC_SIZE

#else
# error "DRAM/IRAM memory map not defined!"
#endif

#define PLUGIN_LENGTH PLUGIN_BUFFER_SIZE


#ifndef CODEC_ORIGIN /* targets can specify another origin */
#define CODEC_ORIGIN (DRAMORIG + (DRAMSIZE))
#endif

#ifndef PLUGIN_ORIGIN /* targets can specify another origin */
#define PLUGIN_ORIGIN (CODEC_ORIGIN + CODEC_SIZE)
#endif

#ifdef CPU_XTENSA
/* ESP32-S3 reaches external RAM through two cache windows that index the same
 * MMU entries and so alias the same physical pages: 0x42000000 and 0x3C000000
 * are both entry 0. They differ in what the hardware will do through them --
 * the 0x42... instruction window serves fetches and loads but faults on a
 * store, the 0x3c... data window serves loads and stores but faults on a
 * fetch. That is why no single base address can host a loaded image: this was
 * measured both ways, InstructionFetchError from 0x3c... and LoadStoreError
 * from 0x42...
 *
 * So what splits is fetched against merely-accessed. .text and .literal go to
 * the instruction window: they are the only things a core fetches, and l32r
 * needs its pools within PC-relative reach of the code. .header, .rodata, .data
 * and .bss stay in the data window, which is the only place a store is served.
 * .header is written by codec_load_ram() and plugin_load() when they fill in
 * the api pointer, and .rodata has to stay writable too -- see the note on its
 * section below.
 *
 * Because the windows alias, the two halves must occupy *different* physical
 * pages, so the text halves are carved from the gap below codecbuf rather than
 * placed at the same offset as their data. Sizes come from memory-xtensa.h: the
 * largest codec .text+.literal+.rodata measured is 182 KB against a 256 KB codec
 * half, and the largest .rock is 482,560 B against a 512 KB plugin half. THIS_TEXT_LENGTH
 * below is what makes an oversized image fail at link time rather than at load.
 * This file is preprocessed into a cached .link, so memory-xtensa.h is a declared
 * dependency of those rules -- without it a size change here silently links images at
 * the stale origin.
 *
 * CODEC_ORIGIN and PLUGIN_ORIGIN are deliberately untouched: apps/codecs.c and
 * apps/plugin.c compare a loaded image's load_addr against codecbuf and
 * pluginbuf, and those symbols stay exactly where they were. */
#define CODEC_TEXT_ORIGIN \
    (CODEC_ORIGIN - XT_CODEC_TEXT_SIZE + XT_IROM_OFF)
#define PLUGIN_TEXT_ORIGIN \
    (CODEC_ORIGIN - XT_CODEC_TEXT_SIZE - XT_PLUGIN_TEXT_SIZE + XT_IROM_OFF)

#define TEXT_REGION PLUGIN_TEXT
#else
#define TEXT_REGION PLUGIN_RAM
#endif

#ifdef CODEC
#define THIS_LENGTH CODEC_SIZE
#define THIS_ORIGIN CODEC_ORIGIN
#elif defined OVERLAY_OFFSET
#define THIS_LENGTH (DRAMSIZE - OVERLAY_OFFSET)
#define THIS_ORIGIN (DRAMORIG + OVERLAY_OFFSET)
#elif defined IMGVDECODER_OFFSET
#define THIS_LENGTH (PLUGIN_LENGTH - IMGVDECODER_OFFSET)
#define THIS_ORIGIN (PLUGIN_ORIGIN + IMGVDECODER_OFFSET)
#else /* plugin */
#define THIS_LENGTH PLUGIN_LENGTH
#define THIS_ORIGIN PLUGIN_ORIGIN
#endif

#ifdef CPU_XTENSA
/* Overlays and the imgvdecoder blob are plugins as far as this script is
 * concerned, so they share the plugin text half. */
#ifdef CODEC
#define THIS_TEXT_LENGTH XT_CODEC_TEXT_SIZE
#define THIS_TEXT_ORIGIN CODEC_TEXT_ORIGIN
#else
#define THIS_TEXT_LENGTH XT_PLUGIN_TEXT_SIZE
#define THIS_TEXT_ORIGIN PLUGIN_TEXT_ORIGIN
#endif
#endif

#if defined(USE_IRAM) && (!defined(IRAMORIG) || !defined(IRAMSIZE))
# error "USE_IRAM is set, but no IRAM region is defined!"
#endif

MEMORY
{
   PLUGIN_RAM : ORIGIN = THIS_ORIGIN, LENGTH = THIS_LENGTH
#ifdef CPU_XTENSA
   PLUGIN_TEXT : ORIGIN = THIS_TEXT_ORIGIN, LENGTH = THIS_TEXT_LENGTH
#endif
#if defined(USE_IRAM)
   PLUGIN_IRAM : ORIGIN = IRAMORIG, LENGTH = IRAMSIZE
#endif
}

SECTIONS
{
    .header : {
        _plugin_start_addr = .;
        plugin_start_addr = .;
        KEEP(*(.header))
    } > PLUGIN_RAM

    .text :
    {
#ifdef CPU_XTENSA
        /* -mtext-section-literals gives every function its own .literal.<fn>
         * pool. An l32r can only reach a literal placed before the use, so all of
         * the pools have to precede the code that references them; leaving them to
         * the end of the script produces "dangerous relocation: l32r: literal
         * placed after use". */
        KEEP(*(.literal))
        KEEP(*(.literal.*))
#endif
        *(.text*)
#ifdef CPU_ARM
        *(.glue_7)
        *(.glue_7t)
#endif
    } > TEXT_REGION

    /* .rodata stays in the data window even though it is read-only by
     * convention. Only .text and .literal need the instruction window, because
     * those are fetched; .rodata is merely loaded, which both windows serve.
     *
     * It also has to stay writable. On every other target PLUGIN_RAM *is* RAM,
     * so a codec writing through a non-const pointer into .rodata is undefined
     * but harmless, and old codec source does it. Putting .rodata where only
     * loads are served turned that latent habit into a LoadStoreError -- seen
     * here as a store to 0x42618cfe from inside the aac decoder. */
    .rodata :
    {
        *(.rodata*)
    } > PLUGIN_RAM

    .data :
    {
        *(.data*)
    } > PLUGIN_RAM

#if defined(NOCACHE_BASE)
    /*
     * Allocate .ncdata based on the following constraints:
     *
     * 1. the LMA must be allocated at the next free LMA in
     *    PLUGIN_RAM and aligned to a cacheline boundary.
     * 2. the VMA - LMA offset must equal NOCACHE_BASE.
     */
    .ncdata ALIGN(. + NOCACHE_BASE, CACHEALIGN_SIZE) : ALIGN(CACHEALIGN_SIZE)
    {
        *(.ncdata*)
        . = ALIGN(CACHEALIGN_SIZE);
    } AT> PLUGIN_RAM
#endif

    /DISCARD/ :
    {
        *(.eh_frame)
#if !defined(USE_IRAM)
		*(.icode)
        *(.irodata)
        *(.idata)
        *(.ibss)
#endif
#ifdef CPU_MIPS
        *(.MIPS.abiflags)
        *(.rel.dyn)
#endif
    }

    .bss (NOLOAD) :
    {
    	plugin_bss_start = .;
    	_plugin_bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(0x4);
    } > PLUGIN_RAM

#if defined(NOCACHE_BASE)
    /*
     * .ncbss has the same constraints as the .ncdata section
     * above but there is an extra complication: because it is
     * a NOLOAD section, unlike .ncdata it cannot consume any
     * VMAs or LMAs in PLUGIN_RAM itself.
     *
     * Because free VMA/LMAs in PLUGIN_RAM are unchanged, any
     * subsequent sections will get placed at the end of the
     * .bss section, overlapping .ncbss, which we don't want.
     *
     * One solution is to manually place the next sections to
     * account for the size of .ncbss, but a cleaner solution
     * is to define a dummy section (.ncbss_vma) allocated at
     * the same VMA as .ncbss and equal in size, and placed in
     * the PLUGIN_RAM region. This allocates the correct VMA
     * range in plugin RAM so that the next section will not
     * overlap .ncbss.
     */
    .ncbss ALIGN(. + NOCACHE_BASE, CACHEALIGN_SIZE) (NOLOAD) : ALIGN(CACHEALIGN_SIZE)
    {
    	*(.ncbss*)
        . = ALIGN(CACHEALIGN_SIZE);
    } AT> PLUGIN_RAM

    .ncbss_vma (NOLOAD) : ALIGN(CACHEALIGN_SIZE)
    {
        . += SIZEOF(.ncbss);
    } > PLUGIN_RAM
#endif

    /* Final end of plugin after IRAM setup. The plugin or codec buffer
       is considered unused by the in-RAM image at this point once IRAM
       is copied. */
    .pluginend (NOLOAD) :
    {
        _plugin_end_addr = .;
        plugin_end_addr = .;
    } > PLUGIN_RAM

#if defined(USE_IRAM)
    .iram IRAMORIG : AT(LOADADDR(.bss))
    {
        iramstart = .;
        *(.icode)
        *(.irodata)
        *(.idata)
        iramend = .;
    } > PLUGIN_IRAM
    iramcopy = LOADADDR(.iram);

    .ibss (NOLOAD) :
    {
        iedata = .;
        *(.ibss)
        . = ALIGN(0x4);
        iend = .;
    } > PLUGIN_IRAM

    loadendaddr = MAX(plugin_end_addr, LOADADDR(.iram) + SIZEOF(.iram));
#else
    loadendaddr = plugin_end_addr;
#endif

    /* This is for ovl_offset.pl and is the highest address that must
       be loaded into the plugin buffer (past the end of last data in
       stored image). */
    .pluginloadend loadendaddr :
    {
    	_plugin_load_end_addr = .;
    	plugin_load_end_addr = .;
    }

    _plugin_ram_end = ORIGIN(PLUGIN_RAM) + LENGTH(PLUGIN_RAM);
    plugin_ram_end = ORIGIN(PLUGIN_RAM) + LENGTH(PLUGIN_RAM);

    /* Special trick to avoid a linker error when no other sections are
       left after garbage collection (plugin not for this platform) */
    .comment 0 :
    {
        KEEP(*(.comment))
    }
}

#if defined(NOCACHE_BASE)
/* Some asserts to make sure nocache sections appear correctly defined */
ASSERT(LOADADDR(.ncdata) == ADDR(.ncdata) - NOCACHE_BASE, ".ncdata has incorrect LMA/VMA address");
ASSERT(LOADADDR(.ncdata) % CACHEALIGN_SIZE == 0,          ".ncdata incorrectly aligned");
ASSERT(LOADADDR(.ncbss) == ADDR(.ncbss) - NOCACHE_BASE,   ".ncbss has incorrect LMA/VMA address");
ASSERT(LOADADDR(.ncbss) % CACHEALIGN_SIZE == 0,           ".ncbss incorrectly aligned");
ASSERT(ADDR(.ncbss_vma) == LOADADDR(.ncbss),              ".ncbss_vma has wrong address");
ASSERT(SIZEOF(.ncbss_vma) == SIZEOF(.ncbss),              ".ncbss_vma has wrong size");
#endif
