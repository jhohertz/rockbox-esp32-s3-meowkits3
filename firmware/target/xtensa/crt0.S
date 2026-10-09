/***************************************************************************
 * ESP32-S3 entry.
 *
 * This is NOT a reset vector. The chip boots from mask ROM, the ROM loader reads
 * an image header from flash, maps the segments through the cache MMU and JUMPS
 * to the entry point recorded in that header. So:
 *
 *   - there is no vector table here, the ROM owns the vectors;
 *   - a0 is not a return address, because nothing called us;
 *   - a1 is whatever the loader left, so we set our own stack;
 *   - .bss is not in the image file, so we must zero it ourselves.
 *
 * The ROM loader sets up the flash side of the MSPI block and the caches, but it
 * does not initialise the external PSRAM, and it has not mapped the MMU pages for
 * Rockbox's audiobuffer, framebuffer or codec buffers - those are NOLOAD, so they
 * are not in the image file. Both happen at runtime here: psram_init() brings the
 * chip up, mmu_init() maps the pages, and neither may be deferred past the first
 * read of that window.
 *
 * The literals are written as .word inside .startup rather than as bare
 * `l32r symbol`. This binutils hits an internal error (ldlang.c 1074) when an
 * l32r references a linker-script-defined symbol directly, even when the symbol
 * is in range. Keeping the literal in the same section as the code avoids it.
 ****************************************************************************/
#include "config.h"
#include "cpu.h"

    .section .startup,"ax",@progbits

    .align 4
.Lstack:  .word _stackend
.Lbsslo:  .word _bssstart
.Lbsshi:  .word _bssend

    .global start
start:
    /* Pin the register window so that a windowed callee's window is a known
     * rotation away from the one call0 code uses. See rom_cache.S.
     */
    movi    a2, 0
    wsr     a2, windowbase

    /* Own stack. Top of internal DRAM. */
    l32r    a1, .Lstack

    /* Zero .bss. The image loader copied .data but left .bss untouched. */
    l32r    a2, .Lbsslo
    l32r    a3, .Lbsshi
    movi    a4, 0
1:
    bgeu    a2, a3, 2f
    s32i    a4, a2, 0
    addi.n  a2, a2, 4
    j       1b
2:

    /* Bring the PSRAM chip up before mapping it. The ROM loader only sets up the
     * flash side of the MSPI block; nothing else initialises the external RAM.
     */
    call0   psram_init

    /* Map the PSRAM pages the audiobuffer occupies before touching it. */
    call0   mmu_init

    call0   main
    /* main does not return on Rockbox; if it does, spin. */
3:
    j       3b
