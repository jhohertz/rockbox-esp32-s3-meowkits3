/***************************************************************************
 * ESP32-S3 entry.
 *
 * This is NOT a reset vector. The chip boots from mask ROM, the ROM loader reads
 * an image header from flash, copies the RAM segments, maps the flash side of
 * the cache MMU and JUMPS to the entry point recorded in that header. So:
 *
 *   - there is no vector table here, the ROM owns the vectors;
 *   - a0 is not a return address, because nothing called us;
 *   - a1 is whatever the loader left, so we set our own stack;
 *   - .bss is not in the image file, so we must zero it ourselves.
 *
 * The entry point has to be in internal SRAM. The ROM loader maps the *data*
 * side of the cache MMU for the flash-mapped segments but never the instruction
 * side - that is ESP-IDF's second-stage bootloader's job, and it is why that
 * bootloader is IRAM-only. So .startup is placed in .iram by app.lds, and this
 * file maps the IROM window before the first instruction is fetched from it.
 *
 * Calls out of .iram cannot use `call0`: the call offset is 18 bits signed, and
 * the flash instruction window is 1.6 MB away. Every target here is reached with
 * l32r + callx0. GCC handles this for C code because xtensacc passes -mlongcalls;
 * hand-written assembly has to do it by hand.
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
.Lstack:      .word _stackend
.Lbsslo:      .word _bssstart
.Lbsshi:      .word _bssend
.Lpslo:       .word _psrambssstart
.Lpshi:       .word _psrambssend
.Lirommap:    .word irom_mmu_map
.Lpsinit:     .word psram_init
.Lmmuinit:    .word mmu_init
.Lpspresent:  .word psram_present
.Lmain:       .word main
/* ROM trampoline for ets_write_char_uart, from
 * components/esp_rom/esp32s3/ld/esp32s3.rom.ld. It is in the base file rather
 * than esp32s3.rom.version.ld, so it does not move between ROM revisions. */
.Luartput:    .word 0x400006b4

    .global start
start:
    /* Pin the register window so that a windowed callee's window is a known
     * rotation away from the one call0 code uses. See rom_cache.S.
     */
    movi    a2, 0
    wsr     a2, windowbase

    /* Own stack. Top of internal DRAM. */
    l32r    a1, .Lstack

    /* 'R' - the ROM loader reached the entry point. Each marker below says how
     * far the bring-up got: 'I' after the IROM window is mapped, 'P' after the
     * PSRAM chip is up, 'M' after its pages are mapped, 'G' before main.
     */
    movi    a3, 'R'
    call0   putchar_rom

    /* Map the IROM window before anything in .text can be executed. */
    l32r    a2, .Lirommap
    callx0  a2
    movi    a3, 'I'
    call0   putchar_rom

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
    movi    a3, 'P'
    call0   putchar_rom
    l32r    a2, .Lpsinit
    callx0  a2

    /* Map the PSRAM pages before touching them. */
    movi    a3, 'M'
    call0   putchar_rom
    l32r    a2, .Lmmuinit
    callx0  a2

    /* .psrambss lives in the PSRAM window, so it cannot be zeroed until the chip
     * is up and its pages are mapped. If psram_present() says no, mmu_init()
     * mapped nothing and every write here would go to an invalid MMU entry; in
     * that case the audiobuffer and framebuffer are unusable too, so the target
     * is already broken and there is nothing to preserve.
     */
    l32r    a2, .Lpspresent
    callx0  a2
    beqz    a0, 4f
    l32r    a2, .Lpslo
    l32r    a3, .Lpshi
    movi    a4, 0
3:
    bgeu    a2, a3, 4f
    s32i    a4, a2, 0
    addi.n  a2, a2, 4
    j       3b
4:

    movi    a3, 'G'
    call0   putchar_rom
    l32r    a2, .Lmain
    callx0  a2
    /* main does not return on Rockbox; if it does, spin. */
5:
    j       5b

/* One character through the ROM's ets_write_char_uart, which is built for the
 * windowed ABI. This has to be self-contained: xt_rom_call lives in .text, and
 * at the first call the IROM window is not mapped yet.
 *
 * callx4 sets CALLINC to 4 as part of the call, so the callee's a2 and a3 are
 * the caller's a6 and a7, and its return value comes back in the caller's a6.
 * Under call0 only a12-a15 are callee-saved, and a window rotated by 4 can
 * reach physical a4-a15, so those four are the ones that need stashing.
 */
    .align 4
putchar_rom:                      /* a3 = character */
    addi    a1, a1, -16
    s32i    a12, a1, 0
    s32i    a13, a1, 4
    mov     a6, a3                /* callee a2 */
    movi    a7, 0                 /* callee a3 */
    l32r    a2, .Luartput
    callx4  a2
    mov     a2, a6                /* callee a2 == caller a6 */
    l32i    a12, a1, 0
    l32i    a13, a1, 4
    addi    a1, a1, 16
    ret.n