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
 * Calls out of .iram cannot use a range-limited `call`: the call offset is 18 bits
 * signed, and the flash instruction window is 1.6 MB away. Every target here is
 * reached with l32r + callx8. GCC handles this for C code because xtensacc passes
 * -mlongcalls; hand-written assembly has to do it by hand.
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
.Lsramlo:     .word _srambssstart
.Lsramhi:     .word _srambssend
.Lpslo:       .word _psrambssstart
.Lpshi:       .word _psrambssend
.Lirommap:    .word irom_mmu_map
.Lcanary:     .word irom_canary
.Lpsinit:     .word psram_init
.Lmmuinit:    .word mmu_init
.Lpspresent:  .word psram_present
.Lbtrace:     .word boot_trace
.Lmain:       .word main
/* ROM trampoline for ets_write_char_uart, from
 * components/esp_rom/esp32s3/ld/esp32s3.rom.ld. It is in the base file rather
 * than esp32s3.rom.version.ld, so it does not move between ROM revisions. */
.Lwdtconf:    .word 0x6001F048
.Luartput:    .word 0x40043ce8
/* External-memory cache functions, also from the base ROM ld file. */
.Ldeficache:  .word 0x4004e3ac   /* Cache_Enable_Defalut_ICache_Mode */
.Lenicache:   .word 0x4004f308   /* Cache_Enable_ICache(mode) */
.Linvicache:  .word 0x4004eab0   /* Cache_Invalidate_ICache_All */
.Linvdcache:  .word 0x4004eac4   /* Cache_Invalidate_DCache_All */
.Libusshut:   .word 0x600C4064   /* EXTMEM_ICACHE_CTRL1_REG */
.Ldbusshut:   .word 0x600C4004   /* EXTMEM_DCACHE_CTRL1_REG */
.Lmmutable:   .word 0x600C5000
.Lmmu144:     .word 0x600C5240
.Lmmu300:     .word 0x600C54B0
.Liomux:      .word 0x6000906C
.Liromstat:   .word irom_map_status
.Lromfirst:   .word irom_first
.Lromphys:    .word irom_phys
.Lrompages:   .word irom_pages
.Ltrace:      .word crt0_trace
.Ldiagvec:    .word diag_vector


/* A windowed call into a C function. The first argument goes in the caller's
 * a10 and the second in a11, which is what the callee's entry reads as a2/a3
 * once the window has rotated by 8; the result comes back in the caller's a10.
 *
 * A function entered with CALLINC 0 has no base-save area, and the window
 * overflow handler walks the base-save area of every ancestor frame. Entering
 * every C function with CALLINC 8 is what makes that walk valid.
 */
    .global start
start:
    /* The mask ROM arms the main watchdog (TIMG0 WDT) as part of the flash boot
     * sequence. ESP-IDF's second-stage bootloader disables it; we have no second
     * stage, so a program that never feeds it gets TG0WDT_SYS_RST a few seconds
     * in. Clear wdt_en (bit 31) before doing anything else.
     */
    l32r    a2, .Lwdtconf
    movi    a3, 0x80
    slli    a3, a3, 24
    l32i    a4, a2, 0
    and     a4, a4, a3
    s32i    a4, a2, 0

    /* Own stack. Top of internal DRAM. */
    l32r    a1, .Lstack

    /* Nothing called us, so the register window is whatever the loader left:
     * a non-zero WINDOWBASE and a WINDOWSTART with bits for windows that were
     * never created by an `entry`. The overflow handler walks the base-save area
     * of every ancestor window, and a window that has no frame has nothing to
     * walk. Pin the window to zero and start from a clean PS so the first call8
     * opens a real frame, with `start` itself acting as the root.
     */
    movi    a2, 0
    wsr     a2, windowbase
    wsr     a2, windowstart
    wsr     a2, ps
    wsr     a2, sar
    rsync

    /* 'R' - the ROM loader reached the entry point. Each marker below says how
     * far the bring-up got: 'C' after the external cache is up, 'I' after the
     * flash MMU is armed, 'P' after the PSRAM chip is up, 'M' after its pages are
     * mapped, 'G' before main.
     */
    movi    a10, 'R'
    call8   putchar_rom

    /* Point the vectors at a trap that records exccause and epc1 before anything
     * else can fault. The ROM's own handler just loops in the debug vector, which
     * tells us nothing; system-xtensa.c installs the real table later.
     */
    l32r    a2, .Ldiagvec
    wsr     a2, vecbase
    movi    a10, 'V'
    call8   putchar_rom

#ifdef ESP_STAGE2_BOOTLOADER
    /* ESP-IDF's second-stage loader has already armed the cache MMU for the
     * flash-mapped segments, switched both external caches on and released the
     * instruction bus. Nothing here has to repeat it; the only thing worth doing
     * is throwing away any line the loader left behind so the diagnostics below
     * read the MMU table rather than a stale copy.
     */
    l32r    a2, .Linvdcache
    movi    a10, 0
    movi    a11, 0
    movi    a12, 0
    movi    a13, 0
    movi    a14, 0
    movi    a15, 0
    callx8  a2
#else
    /* The ROM leaves the external instruction cache disabled and both
     * instruction buses shut, so a fetch from the IROM window faults with
     * IllegalInstruction even when the MMU entry is armed. ESP-IDF's
     * second-stage bootloader does this before it jumps into flash mapped code;
     * we have no second stage, so it has to happen here. It has to come before
     * irom_mmu_map(), which reads the image header through the D-cache: without
     * the invalidation that read returns stale lines and the mapping is skipped.
     */
    /* The mask ROM does not map the cache MMU at all: Cache_MMU_Init leaves all
     * 512 entries invalid, and nothing in the flash loader arms one. So entry 0,
     * which is the page holding the image header, has to be armed before it can
     * be read. Arm it, invalidate, and only then walk the header.
     */
    l32r    a2, .Ldeficache
    movi    a10, 0
    movi    a11, 0
    movi    a12, 0
    movi    a13, 0
    movi    a14, 0
    movi    a15, 0
    callx8  a2

    /* bit 0 = core0 bus shut, bit 1 = core1 bus shut; both are set by default */
    movi    a3, 0
    l32r    a6, .Libusshut
    s32i    a3, a6, 0
    l32r    a6, .Ldbusshut
    s32i    a3, a6, 0
    memw

    l32r    a2, .Lenicache
    movi    a10, 4                /* CACHE_AUTOLOAD_MODE */
    movi    a11, 0
    movi    a12, 0
    movi    a13, 0
    movi    a14, 0
    movi    a15, 0
    callx8  a2

    l32r    a2, .Linvicache
    movi    a10, 0
    movi    a11, 0
    movi    a12, 0
    movi    a13, 0
    movi    a14, 0
    movi    a15, 0
    callx8  a2

    l32r    a6, .Lmmutable
    movi    a3, 0                 /* linear page 0 -> physical page 0 = the header */
    s32i    a3, a6, 0
    memw

    l32r    a2, .Linvdcache
    movi    a10, 0
    movi    a11, 0
    movi    a12, 0
    movi    a13, 0
    movi    a14, 0
    movi    a15, 0
    callx8  a2
    movi    a10, 'C'
    call8   putchar_rom
#endif

#ifndef ESP_STAGE2_BOOTLOADER
    /* Map every flash-mapped segment before anything in .text can be executed. */
    l32r    a2, .Lirommap
    mov     a10, a2
    movi    a11, 0
    callx8  a2
    movi    a10, 'I'
    call8   putchar_rom

    /* The MMU entries irom_mmu_map just armed are invisible to the cache until
     * the lines that were fetched against the old mapping are thrown away.
     */
    l32r    a2, .Linvicache
    movi    a10, 0
    movi    a11, 0
    movi    a12, 0
    movi    a13, 0
    movi    a14, 0
    movi    a15, 0
    callx8  a2

    l32r    a2, .Linvdcache
    movi    a10, 0
    movi    a11, 0
    movi    a12, 0
    movi    a13, 0
    movi    a14, 0
    movi    a15, 0
    callx8  a2
#endif

    /* Bring-up trace: a snapshot of the MMU table taken at boot time. Reading the
     * table itself is useless afterwards, because attaching esptool resets the chip
     * and the ROM re-initialises it.
     */
    l32r    a6, .Ltrace
    l32r    a7, .Lmmu144
    movi    a4, 16
10:
    l32i    a8, a7, 0
    s32i    a8, a6, 0
    addi    a7, a7, 4
    addi    a6, a6, 4
    addi    a4, a4, -1
    bnez    a4, 10b
    l32r    a7, .Lmmutable
    movi    a4, 16
11:
    l32i    a8, a7, 0
    s32i    a8, a6, 0
    addi    a7, a7, 4
    addi    a6, a6, 4
    addi    a4, a4, -1
    bnez    a4, 11b
    l32r    a6, .Ltrace
    l32r    a7, .Liromstat
    l32i    a4, a7, 0
    s32i    a4, a6, 128
    l32r    a7, .Lromfirst
    l32i    a4, a7, 0
    s32i    a4, a6, 132
    l32r    a7, .Lromphys
    l32i    a4, a7, 0
    s32i    a4, a6, 136
    l32r    a7, .Lrompages
    l32i    a4, a7, 0
    s32i    a4, a6, 140

    /* Does the MMU table read back what was written to it? Entries 300..315 are
     * unused by the firmware, so they are safe to overwrite and compare.
     */
    l32r    a7, .Lmmu300
    movi    a8, 16
    slli    a8, a8, 8
    movi    a4, 16
12:
    s32i    a8, a7, 0
    addi    a7, a7, 4
    addi    a8, a8, 1
    addi    a4, a4, -1
    bnez    a4, 12b
    l32r    a7, .Lmmu300
    l32r    a6, .Ltrace
    addi    a6, a6, 256
    movi    a4, 16
13:
    l32i    a8, a7, 0
    s32i    a8, a6, 0
    addi    a7, a7, 4
    addi    a6, a6, 4
    addi    a4, a4, -1
    bnez    a4, 13b

    /* Does a plain peripheral read work from .iram? The IO MUX pad for CS1 is the
     * first thing psram_init touches, and the fault lands on that load, so read it
     * here with hand-written asm in .startup and keep the result in the trace.
     */
    l32r    a6, .Ltrace
    l32r    a7, .Liomux
    movi    a4, 0x11
    s32i    a4, a6, 236
    memw
    l32i    a5, a7, 0
    s32i    a5, a6, 240

    /* Zero the internal-SRAM statics. .srambss is NOLOAD and lives on the data bus,
     * so it is reachable without the MMU; it is where the GDMA descriptors and the
     * pre-MMU globals go.
     */
    l32r    a2, .Lsramlo
    l32r    a3, .Lsramhi
    movi    a4, 0
8:
    bgeu    a2, a3, 9f
    s32i    a4, a2, 0
    addi.n  a2, a2, 4
    j       8b
9:

    /* First code fetched from the IROM window. 'K' means the mapping works.
     * The return value is kept in a0 so the panic dump shows it if it does not.
     */
    movi    a10, 'T'
    call8   putchar_rom
    l32r    a2, .Lcanary
    mov     a10, a2
    movi    a11, 0
    callx8  a2
    movi    a10, 'K'
    call8   putchar_rom

    /* Bring the PSRAM chip up before mapping it. The ROM loader only sets up the
     * flash side of the MSPI block; nothing else initialises the external RAM.
     */
    movi    a10, 'P'
    call8   putchar_rom
    l32r    a2, .Lpsinit
    mov     a10, a2
    movi    a11, 0
    callx8  a2
    movi    a2, 'q'
    l32r    a3, .Lbtrace
    mov     a10, a2
    movi    a11, 0
    callx8  a3
    l32r    a6, .Ltrace
    movi    a4, 'p'
    s32i    a4, a6, 144

    /* Is the IROM window readable as data? mmu_init is the first function that
     * lives in .text rather than .iram, so if the fetch of its page is broken the
     * fault will land on its first instruction. Read the same bytes through the
     * DROM alias of the same MMU page and keep them, so a bad page is visible
     * without a serial connection.
     */
    l32r    a6, .Ltrace
    l32r    a7, .Lmmuinit
    movi    a4, 0x60
    slli    a4, a4, 20
    sub     a7, a7, a4
    memw
    l32i    a5, a7, 0
    s32i    a5, a6, 244
    l32i    a5, a7, 4
    s32i    a5, a6, 248

    /* Map the PSRAM pages before touching them. */
    movi    a10, 'M'
    call8   putchar_rom
    l32r    a2, .Lmmuinit
    mov     a10, a2
    movi    a11, 0
    callx8  a2
    movi    a2, 'n'
    l32r    a3, .Lbtrace
    mov     a10, a2
    movi    a11, 0
    callx8  a3
    l32r    a6, .Ltrace
    movi    a4, 'm'
    s32i    a4, a6, 148

    /* .bss and .psrambss both live in the PSRAM window, so neither can be zeroed
     * until the chip is up and its pages are mapped. If psram_present() says no,
     * mmu_init() mapped nothing and every write here would go to an invalid MMU
     * entry; in that case the audiobuffer and framebuffer are unusable too, so the
     * target is already broken and there is nothing to preserve.
     */
    l32r    a2, .Lpspresent
    mov     a10, a2
    movi    a11, 0
    callx8  a2
    l32r    a6, .Ltrace
    s32i    a10, a6, 152
    beqz    a10, 4f
    l32r    a2, .Lpslo
    l32r    a3, .Lpshi
    movi    a4, 0
3:
    bgeu    a2, a3, 4f
    s32i    a4, a2, 0
    addi.n  a2, a2, 4
    j       3b
4:
    movi    a2, 'B'
    l32r    a3, .Lbtrace
    mov     a10, a2
    movi    a11, 0
    callx8  a3

    movi    a10, 'G'
    call8   putchar_rom
    l32r    a6, .Ltrace
    movi    a4, 'G'
    s32i    a4, a6, 156
    movi    a2, 'R'
    l32r    a3, .Lbtrace
    mov     a10, a2
    movi    a11, 0
    callx8  a3
    l32r    a2, .Lmain
    mov     a10, a2
    movi    a11, 0
    callx8  a2
    /* main does not return on Rockbox; if it does, spin. */
5:
    j       5b

/* One-character trace through the ROM's ets_write_char_uart. This has to be
 * self-contained: xt_rom_call lives in .text, and at the first call the IROM window
 * is not mapped yet.
 *
 * It is a real windowed frame, not a leaf. callx8 sets CALLINC to 8, the ROM callee's
 * entry rotates the window by 8, and the overflow handler then walks the base-save
 * area of every ancestor frame - so this function has to have one.
 */
    .align 4
putchar_rom:                      /* a10 = character, per the windowed ABI */
    entry   a1, 32                /* a base-save area, so the overflow handler has
                                   * a frame to walk when the ROM callee rotates */
    mov     a10, a2               /* our a2 is the caller's a10 */
    movi    a11, 0                /* the ROM callee's second argument */
    l32r    a2, .Luartput
    callx8  a2
    retw.n

/* Scratch for the bring-up trace, readable with `esptool dump_mem` after a reset:
 * internal SRAM survives a reset, so the values are still there when esptool
 * attaches. Each slot is one step of the bring-up.
 */
    .section .ibss,"aw",@progbits
    .align 4
    .global crt0_trace
crt0_trace:
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0

/* Exception trap for the bring-up phase. Every vector slot points at the same
 * trap, so whatever class of fault happens it lands here, and the cause and the
 * faulting PC are left in crt0_trace so they can be read after a reset. The
 * slots have to sit at the real vector offsets, hence .org rather than a run of
 * consecutive entries.
 */
    .section .ivectors,"ax",@progbits
    .align 1024
    .global diag_vector
diag_vector:
    /* Each vector tags which slot was taken before falling into the shared trap:
     * EXCCAUSE is 0 for a level-1 interrupt as well as for IllegalInstruction, so
     * the slot number is the only way to tell an interrupt from a real exception.
     *
     * The window vectors are the exception: they are not errors. Now that the
     * build uses the toolchain's default windowed ABI, every nested call that
     * crosses a window boundary lands here, and trapping instead of spilling turns
     * a normal call into a double exception. They go to the same handlers the real
     * table uses.
     */
    .org diag_vector + 0x000
    j       xt_window_overflow_4
    .org diag_vector + 0x040
    j       xt_window_underflow_4
    .org diag_vector + 0x080
    j       xt_window_overflow_8
    .org diag_vector + 0x0c0
    j       xt_window_underflow_8
    .org diag_vector + 0x100
    j       xt_window_overflow_12
    .org diag_vector + 0x140
    j       xt_window_underflow_12
    .org diag_vector + 0x180
    movi    a5, 0x180
    j       diag_trap
    .org diag_vector + 0x1c0
    movi    a5, 0x1c0
    j       diag_trap
    .org diag_vector + 0x200
    movi    a5, 0x200
    j       diag_trap
    .org diag_vector + 0x240
    movi    a5, 0x240
    j       diag_trap
    .org diag_vector + 0x280
    movi    a5, 0x280
    j       diag_trap
    .org diag_vector + 0x2c0
    movi    a5, 0x2c0
    j       diag_trap
    .org diag_vector + 0x300
    movi    a5, 0x300
    j       diag_trap
    .org diag_vector + 0x340
    movi    a5, 0x340
    j       diag_trap
    .org diag_vector + 0x380
    movi    a5, 0x380
    j       diag_trap
    .org diag_vector + 0x3c0
    movi    a5, 0x3c0
    j       diag_trap
    .org diag_vector + 0x400
    .align 4
diag_trap:
    l32r    a6, .Ltrace
    rsr     a2, exccause
    rsr     a3, epc1
    rsr     a4, excvaddr
    rsr     a7, epc2
    rsr     a8, intenable
    s32i    a2, a6, 160
    s32i    a3, a6, 164
    s32i    a4, a6, 168
    s32i    a7, a6, 172
    s32i    a5, a6, 336        /* which vector slot was taken */
    s32i    a8, a6, 340        /* INTENABLE */
    s32i    a0, a6, 176
    s32i    a1, a6, 180
    s32i    a2, a6, 184
    s32i    a3, a6, 188
    s32i    a4, a6, 192
    s32i    a5, a6, 196
    s32i    a6, a6, 200
    s32i    a7, a6, 204
    s32i    a8, a6, 208
    s32i    a9, a6, 212
    s32i    a10, a6, 216
    s32i    a11, a6, 220
    s32i    a12, a6, 320
    s32i    a13, a6, 324
    s32i    a14, a6, 328
    s32i    a15, a6, 332
    /* the words around the faulting instruction, aligned down so the read itself
     * does not fault: an unaligned l32i here is a second exception, and the double
     * exception is what makes the ROM print "Saved PC" and reset */
    /* Read the faulting bytes through the DROM alias of the same MMU page:
     * 0x4294xxxx and 0x3c94xxxx share MMU entry (addr & 0x1FFFFFF) >> 16, and
     * the data side of that window is the only reliable way to see what the
     * loader actually put in flash. A direct read of the 0x42 window from the
     * data bus returns stale cache lines.
     */
    movi    a2, -4
    and     a2, a3, a2
    movi    a4, 0x60
    slli    a4, a4, 20
    sub     a2, a2, a4
    l32r    a6, .Ltrace
    l32i    a4, a2, 0
    l32i    a5, a2, 4
    l32i    a7, a2, 8
    s32i    a4, a6, 224
    s32i    a5, a6, 228
    s32i    a7, a6, 232
1:
    j       1b
