/***************************************************************************
 * Xtensa vector table and level-1 interrupt handler for the esp32s3 core.
 *
 * The esp32s3 core config has no EXCB and no INTBASE, so there is no separate
 * interrupt vector table: level-1 interrupts are taken through the exception
 * vectors, which are the kernel vector at +0x300 and the user vector at +0x340.
 * Which one is used depends on PS.UM at the time the interrupt is taken, so the
 * same handler is installed at both.
 *
 * VECBASE is relocatable on this core (XCHAL_HAVE_VECBASE 1) and is set by
 * system_init to the table below. It has to be a physical address the core can
 * fetch from, so the table lives in internal SRAM, not in the flash cache window.
 *
 * The window vectors occupy 0x000 through 0x17C (overflow/underflow for CALLINC
 * 4, 8 and 12, with the alloca handler in the gaps). Rockbox builds Xtensa with
 * the toolchain's default windowed ABI, so those vectors are load-bearing: the
 * esp32s3 core config has no REGB option, so there is no spills/refill opcode and
 * the register windows are moved to and from the stack by these handlers instead.
 * Leaving them as traps makes any nested call that crosses a window boundary raise
 * a double exception.
 *
 * rfe is available even though rsr.sregs and rsr.eps1 are not: the saved PS and
 * window base live in internal state (the assembler calls it sysregs) that only
 * rfe can read. That is also why the handler does not have to save PS.
 ****************************************************************************/
#include "config.h"
#include "cpu.h"

    .section .vector,"ax",@progbits
    .global _vector_table
    .align 1024

_vector_table:
    .org _vector_table + 0x000
    j xt_window_overflow_4

    .org _vector_table + 0x040
    j xt_window_underflow_4

    .org _vector_table + 0x080
    j xt_window_overflow_8

    .org _vector_table + 0x0c0
    j xt_window_underflow_8

    .org _vector_table + 0x100
    j xt_window_overflow_12

    .org _vector_table + 0x140
    j xt_window_underflow_12

    .org _vector_table + 0x180
    j xt_unused_trap2              /* level 2 */
    .org _vector_table + 0x1c0
    j xt_unused_trap3              /* level 3 */
    .org _vector_table + 0x200
    j xt_unused_trap4              /* level 4 */
    .org _vector_table + 0x240
    j xt_unused_trap5              /* level 5 */
    .org _vector_table + 0x280
    j xt_unused_trap6              /* level 6 / debug */
    .org _vector_table + 0x2c0
    j xt_unused_trapNMI            /* NMI */

    .org _vector_table + 0x300
    j xt_level1_handler               /* level 1, supervisor mode */
    .org _vector_table + 0x340
    j xt_level1_handler               /* level 1, user mode */

    .org _vector_table + 0x380
    j xt_unused_trapINV            /* no vector at 0x380 on this core */
    .org _vector_table + 0x3c0
    j xt_double_trap               /* double exception - XCHAL_DOUBLEEXC_VECOFS */
    .org _vector_table + 0x400
    j xt_unused_trapHW             /* end of the vector table */

    .size _vector_table, . - _vector_table

/*
 * Window overflow and underflow handlers.
 *
 * On overflow the window has rotated onto a set of registers that still hold live
 * values from an ancestor frame; those have to be written to the end of that
 * ancestor's stack frame before the current frame can use them. On underflow the
 * reverse: retw is returning to a frame whose registers were spilled, so they are
 * read back.
 *
 * a5/a9/a13 is the callee's stack pointer on entry, and the ancestor frame's
 * pointer is fetched from the callee's frame at -12. The offsets below are the
 * spill area at the end of each frame, which is why they are negative and why the
 * frame size has to be at least the CALLINC the compiler chose.
 */

    .section .vector,"ax",@progbits
    .align 4

    .global xt_window_overflow_4
    .type xt_window_overflow_4, @function
xt_window_overflow_4:
    s32e    a0, a5, -16
    s32e    a1, a5, -12
    s32e    a2, a5, -8
    s32e    a3, a5, -4
    rfwo

    .global xt_window_underflow_4
    .type xt_window_underflow_4, @function
xt_window_underflow_4:
    l32e    a0, a5, -16
    l32e    a1, a5, -12
    l32e    a2, a5, -8
    l32e    a3, a5, -4
    rfwu

    .global xt_window_overflow_8
    .type xt_window_overflow_8, @function
xt_window_overflow_8:
    s32e    a0, a9, -16
    l32e    a0, a1, -12             /* a0 <- ancestor's ancestor sp */
    s32e    a1, a9, -12
    s32e    a2, a9, -8
    s32e    a3, a9, -4
    s32e    a4, a0, -32
    s32e    a5, a0, -28
    s32e    a6, a0, -24
    s32e    a7, a0, -20
    rfwo

    .global xt_window_underflow_8
    .type xt_window_underflow_8, @function
xt_window_underflow_8:
    /* Count before refilling a0-a3: on an underflow those are the registers being
     * restored, so scratch has to be used first, not after. */
    l32e    a0, a9, -16
    l32e    a1, a9, -12
    l32e    a2, a9, -8
    l32e    a7, a1, -12             /* a7 <- ancestor's ancestor sp */
    l32e    a3, a9, -4
    l32e    a4, a7, -32
    l32e    a5, a7, -28
    l32e    a6, a7, -24
    l32e    a7, a7, -20
    rfwu

    .global xt_window_overflow_12
    .type xt_window_overflow_12, @function
xt_window_overflow_12:
    s32e    a0,  a13, -16
    l32e    a0,  a1,  -12
    s32e    a1,  a13, -12
    s32e    a2,  a13, -8
    s32e    a3,  a13, -4
    s32e    a4,  a0,  -48
    s32e    a5,  a0,  -44
    s32e    a6,  a0,  -40
    s32e    a7,  a0,  -36
    s32e    a8,  a0,  -32
    s32e    a9,  a0,  -28
    s32e    a10, a0,  -24
    s32e    a11, a0,  -20
    rfwo

    .global xt_window_underflow_12
    .type xt_window_underflow_12, @function
xt_window_underflow_12:
    l32e    a0,  a13, -16
    l32e    a1,  a13, -12
    l32e    a2,  a13, -8
    l32e    a11, a1,  -12
    l32e    a3,  a13, -4
    l32e    a4,  a11, -48
    l32e    a5,  a11, -44
    l32e    a6,  a11, -40
    l32e    a7,  a11, -36
    l32e    a8,  a11, -32
    l32e    a9,  a11, -28
    l32e    a10, a11, -24
    l32e    a11, a11, -20
    rfwu

/*
 * 'movsp' (alloca) raises the Alloca exception cause, which arrives through the
 * level-1 kernel/user vectors rather than a vector of its own on this core. The
 * base-save area has already been spilled, so the four registers of the current
 * window are free scratch; a0 is in EXCSAVE_1. The handler adjusts PS.OWB so the
 * stack pointer points at the allocated area and returns.
 */
    .align 4
    .global xt_alloca_exc
    .type xt_alloca_exc, @function
xt_alloca_exc:
    rsr     a0, windowbase
    rotw    -1                      /* a4 gets the old window base */
    rsr     a2, ps
    extui   a3, a2, 18, 4          /* PS.OWB */
    xor     a3, a3, a4
    rsr     a4, excsave1           /* the faulting a0, with the CALLINC in its top bits */
    slli    a3, a3, 18
    xor     a2, a2, a3
    wsr     a2, ps
    rsync
    /* 'movsp' has moved the stack pointer but not rotated the window, so the
     * registers that were live in the ancestor window are now unreachable. The
     * underflow handler refills them from the frames they were spilled into and
     * returns to the instruction after the movsp. There is no return address here:
     * a0 was the faulting instruction's, not a call, so falling through to `ret`
     * would jump to garbage.
     */
    bbci.l  a4, 31, xt_window_underflow_4
    rotw    -1                      /* the faulting a0 moves to a8 */
    bbci.l  a8, 30, xt_window_underflow_8
    rotw    -1
    j       xt_window_underflow_12

/*
 * The handler runs on the interrupted thread's stack. a0 and a2-a15 have to be
 * saved because the C dispatch is free to clobber them: a handler is not a call
 * from the interrupted code's point of view, so all of them are scratch here.
 *
 * That is not enough. The interruptee's call chain lives in register windows that
 * are still in the physical register file, and the esp32s3 core config has no REGB
 * option, so those windows have no spills/refill opcodes - they are moved to the
 * stack by the window overflow/underflow handlers instead. Until they are spilled,
 * any nested call inside the dispatch rotates through windows that still hold the
 * interruptee's ancestors, overwrites them, and the matching underflow on the way
 * back reads garbage. Shallow ISRs survive; anything that nests deeper dies at
 * retw.
 *
 * So the handler does what ESP-IDF's _xt_lowint1 + _xt_context_save do: clear PS.EXCM
 * and set PS.WOE, then run SPILL_ALL_WINDOWS with the stack pointer set back to the
 * interruptee's frame. That is the point of the exercise - the overflow handlers walk
 * the ABI base-save chain, and only the interruptee's own frames have a valid one.
 * Running the spill from the handler's own frame would make the handlers read the
 * ancestor's sp out of memory that belongs to nobody.
 *
 * SPILL_ALL_WINDOWS (Cadence's xt_asm_utils.h, XCHAL_NUM_AREGS == 64) is a no-op
 * write to a12 followed by a window rotation, repeated until the window has made a
 * full 16-register turn. Each write to a12 raises a window overflow if a12 holds a
 * live ancestor register, and the overflow handler spills it into that ancestor's
 * frame. The net rotation is zero, so the window is where it was.
 *
 * EPC1 has to be stashed around the spill because the overflow exceptions overwrite
 * it, and the zero-overhead loop state has to be saved and cleared, because the C
 * dispatch uses loops of its own.
 */
#define FR_A0     0
#define FR_A2     4
#define FR_A3     8
#define FR_A4     12
#define FR_A5     16
#define FR_A6     20
#define FR_A7     24
#define FR_A8     28
#define FR_A9     32
#define FR_A10    36
#define FR_A11    40
#define FR_A12    44
#define FR_A13    48
#define FR_A14    52
#define FR_A15    56
#define FR_PS     60
#define FR_A1     64
#define FR_PC     68
#define FR_SAR    72
#define FR_LBEG   76
#define FR_LEND   80
#define FR_LCOUNT 84
#define FR_EPC1   88
#define FR_SIZE   128           /* 16-byte aligned, with slack for the spill area */

/* ESP-IDF's SPILL_ALL_WINDOWS, kept behind a switch. The handler restores PS.EXCM
 * before the dispatch, which pins the register window, so nested calls cannot
 * rotate through the interruptee's ancestor windows and there is nothing to spill.
 * Set to 1 only if a dispatch ever needs to nest with window rotation. */
#define XT_ISR_SPILL 0

    .section .vector,"ax",@progbits
    .align 4
    .global xt_level1_handler
    .type xt_level1_handler, @function

xt_level1_handler:
    /* The address of the diagnostic block has to be parked as a literal in this
     * section, before any l32r that names it: the linker rejects a literal that
     * lands after the instruction that uses it. Jump over it. */
    j       1f
    .align 4
1:
    /*
     * Exception entry on this core does not rotate the register window: the
     * handler sees the interruptee's a0..a15 in the current window, and PS.OWB
     * holds the caller's window base. a0 is therefore the interruptee's return
     * address, which is exactly what rfe's RETW needs. EXCSAVE_1 is not written
     * by the hardware here - it reads 0 - so taking a0 from it, as the generic
     * XEA2 recipe suggests, returns to address 0 and the core faults on the next
     * instruction fetch.
     *
     * The handler runs on the interrupted thread's stack, so every register it
     * touches has to be stashed. PS.EXCM is restored before the dispatch, which
     * pins the register window: nested calls inside the dispatch cannot rotate
     * through the interruptee's ancestor windows, so ESP-IDF's SPILL_ALL_WINDOWS
     * is not needed on this path.
     */
    addi    a1, a1, -FR_SIZE
    s32i    a0, a1, FR_A0
    s32i    a2, a1, FR_A2
    s32i    a3, a1, FR_A3
    s32i    a4, a1, FR_A4
    s32i    a5, a1, FR_A5
    s32i    a6, a1, FR_A6
    s32i    a7, a1, FR_A7
    s32i    a8, a1, FR_A8
    s32i    a9, a1, FR_A9
    s32i    a10, a1, FR_A10
    s32i    a11, a1, FR_A11
    s32i    a12, a1, FR_A12
    s32i    a13, a1, FR_A13
    s32i    a14, a1, FR_A14
    s32i    a15, a1, FR_A15

    /* Synthesise the ABI base-save area that ENTRY would have created.
     *
     * The dispatch is entered with call8, which rotates the register window.
     * When that rotation reaches a window that still holds a live ancestor,
     * the overflow handler walks the base-save chain: frame k's area at
     * sp_k-16 .. sp_k-4 holds frame k-1's a0..a3, and sp_k-12 is how it finds
     * the ancestor's stack pointer. A frame built with `addi` has no such
     * area, so the handler reads uninitialised stack as the ancestor sp and
     * stores through it. That is the StoreProhibited-at-0 that came out of
     * xt_double_trap: a0 read 32 out of a1-12 and the handler wrote a4 at
     * a0-32 = 0.
     *
     * The area lands below the handler frame, i.e. below the interruptee's
     * stack pointer, which is unused stack. a0, a2 and a3 still hold the
     * interruptee's live values here - they were copied to the frame, not
     * overwritten.
     */
    addi    a10, a1, FR_SIZE            /* the interruptee's sp */
    s32e    a0,  a1, -16
    s32e    a10, a1, -12
    s32e    a2,  a1, -8
    s32e    a3,  a1, -4

    /* The interruptee's stack pointer is the frame base plus the frame size. */
    addi    a0, a1, FR_SIZE
    s32i    a0, a1, FR_A1
    rsr     a0, ps
    s32i    a0, a1, FR_PS
    rsr     a0, epc1
    s32i    a0, a1, FR_PC
    rsr     a0, sar
    s32i    a0, a1, FR_SAR
    rsr     a0, lbeg
    s32i    a0, a1, FR_LBEG
    rsr     a0, lend
    s32i    a0, a1, FR_LEND
    rsr     a0, lcount
    s32i    a0, a1, FR_LCOUNT
    movi    a0, 0
    wsr     a0, lcount                /* the dispatch's own loops must not rewind
                                       * the interruptee's */
    rsync

    /* Level 1 is shared by interrupts and exceptions: EXCCAUSE tells them apart,
     * and treating an Alloca or a LoadProhibited as an interrupt would corrupt the
     * thread instead of reporting it. */
    rsr     a0, exccause
    movi    a2, 4                     /* EXCCAUSE_LEVEL1INTERRUPT */
    beq     a0, a2, 1f
    movi    a2, 5                     /* EXCCAUSE_ALLOCA */
    beq     a0, a2, xt_alloca_exc
    j       xt_exc_trap
1:

    /* C code cannot run in exception mode: PS.EXCM pins the register window, so
     * the entry/retw pair every dispatched handler compiles to would not rotate
     * anything. INTLEVEL is raised to XCHAL_EXCM_LEVEL (3) while EXCM is cleared,
     * so level 1 cannot re-open to itself, and window overflow is enabled in case
     * the dispatch nests deeply.
     */
    movi    a2, 0x00040023            /* PS_INTLEVEL(3) | PS_UM | PS_WOE */
    wsr     a2, ps
    rsync

#if XT_ISR_SPILL
    /* ESP-IDF's SPILL_ALL_WINDOWS, run with a1 at the interruptee's frame: the
     * overflow handlers walk the ABI base-save chain, and only the interruptee's
     * frames have a valid one. EPC1 is stashed because the overflow exceptions
     * overwrite it. */
    rsr     a2, ps
    rsr     a0, epc1
    l32i    a1, a1, FR_A1
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    4
    addi    a1, a1, -FR_SIZE
    wsr     a2, ps
    rsync
    wsr     a0, epc1
    rsync
#endif

    /* INTERRUPT is the pending bitmask over all 32 slots. It reports asserted
     * sources even when INTENABLE masks them, so a tick interrupt would dispatch a
     * device handler whose slot is deliberately masked - and that handler would then
     * consume the other device's event latches. Mask with INTENABLE, as ESP-IDF's
     * _xt_interrupt_pending_list() does.
     *
     * The dispatch is entered with CALLINC 8, so the windowed ABI's first argument
     * is the caller's a10.
     */
    rsr     a10, interrupt
    rsr     a11, intenable
    and     a10, a10, a11
    call8   xt_irq_dispatch

    l32i    a2, a1, FR_SAR
    wsr     a2, sar
    rsync
    l32i    a2, a1, FR_LBEG
    l32i    a3, a1, FR_LEND
    l32i    a4, a1, FR_LCOUNT
    wsr     a2, lbeg
    wsr     a3, lend
    wsr     a4, lcount
    rsync
    /* PS.CALLINC has to be zero when rfe executes. rfe is RESTOREPS + RETW, and
     * RETW rotates the window back by CALLINC. The handler was entered by a jump
     * from the vector, not by a call, so there is no forward rotation to undo:
     * leaving CALLINC at the interruptee's value (8 here) moves windowbase two
     * windows down and the thread resumes with the wrong registers. ESP-IDF gets
     * this for free because its whole exception path is call0; we call the
     * dispatch with call8, so the field is cleared explicitly. RESTOREPS is a
     * no-op at INTLEVEL 0, so the value written here is the one RETW sees.
     */
    l32i    a2, a1, FR_PS
    movi    a3, -0x10000              /* clear PS bits 16-17 (CALLINC) */
    and     a2, a2, a3
    wsr     a2, ps
    rsync
    l32i    a2, a1, FR_PC
    wsr     a2, epc1
    rsync
    l32i    a0, a1, FR_A0
    l32i    a2, a1, FR_A2
    l32i    a3, a1, FR_A3
    l32i    a4, a1, FR_A4
    l32i    a5, a1, FR_A5
    l32i    a6, a1, FR_A6
    l32i    a7, a1, FR_A7
    l32i    a8, a1, FR_A8
    l32i    a9, a1, FR_A9
    l32i    a10, a1, FR_A10
    l32i    a11, a1, FR_A11
    l32i    a12, a1, FR_A12
    l32i    a13, a1, FR_A13
    l32i    a14, a1, FR_A14
    l32i    a15, a1, FR_A15
    l32i    a1, a1, FR_A1

    rsync
    rfe

/*
 * Anything that reaches here is a real fault. The record lives in internal SRAM
 * on the instruction bus, so the trap code can reach it with l32r without
 * crossing the 0x6F0000 gap to the data-bus view; a probe reads it through the
 * alias. Words 0-11 are the plain exception, 56/57 the trap counts, and 32-60
 * the nested (double) exception.
 */
    .section .iramdata,"aw",@progbits
    .align 4
    .global xt_exc_dump
    .type xt_exc_dump,@object
xt_exc_dump:
    .word 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .word 0, 0, 0, 0

    .section .vector,"ax",@progbits
    .align 4
.Ldiag_lit2:
    .long   xt_exc_dump

    .global xt_exc_trap
    .type xt_exc_trap, @function
xt_exc_trap:
    l32r    a0, .Ldiag_lit2
    l32i    a1, a0, 228
    addi    a1, a1, 1
    s32i    a1, a0, 228
    /* l32r reaches only 128 KB either way, and xt_exc_dump lives in internal SRAM
     * on the data bus while this code is on the instruction bus. The address is
     * parked as a literal in the same section and jumped over. */
    j       1f
    .align 4
2:
    .long   xt_exc_dump
1:
    l32r    a3, 2b
    rsr     a4, exccause
    s32i    a4, a3, 0
    rsr     a4, epc1
    s32i    a4, a3, 4
    rsr     a4, excvaddr
    s32i    a4, a3, 8
    rsr     a4, epc2
    s32i    a4, a3, 12
    s32i    a0, a3, 16
    s32i    a1, a3, 20
    rsr     a4, ps
    s32i    a4, a3, 24
    rsr     a4, windowbase
    s32i    a4, a3, 28
    rsr     a4, windowstart
    s32i    a4, a3, 32
    rsr     a4, excsave1
    s32i    a4, a3, 36
    rsr     a4, intenable
    s32i    a4, a3, 40
    rsr     a4, interrupt
    s32i    a4, a3, 44
3:
    j       3b

    /* An unused vector reached by accident: record the cause and spin, so the
     * board is still reachable over the serial console instead of running with a
     * corrupted window. */
    .macro XT_TRAP id
    j       1f
    .align  4
2:
    .long   xt_exc_dump
1:
    l32r    a3, 2b
    movi    a4, 0xbeef
    s32i    a4, a3, 0
    movi    a4, \id
    s32i    a4, a3, 4
    rsr     a4, exccause
    s32i    a4, a3, 8
    rsr     a4, epc1
    s32i    a4, a3, 12
    rsr     a4, excvaddr
    s32i    a4, a3, 16
    rsr     a4, windowbase
    s32i    a4, a3, 20
    rsr     a4, ps
    s32i    a4, a3, 24
3:
    j       3b
    .endm

    .global xt_unused_trap2
xt_unused_trap2:
    XT_TRAP 2
    .global xt_unused_trap3
xt_unused_trap3:
    XT_TRAP 3
    .global xt_unused_trap4
xt_unused_trap4:
    XT_TRAP 4
    .global xt_unused_trap5
xt_unused_trap5:
    XT_TRAP 5
    .global xt_unused_trap6
xt_unused_trap6:
    XT_TRAP 6
    .global xt_unused_trapNMI
xt_unused_trapNMI:
    XT_TRAP 7
    .global xt_unused_trapINV
xt_unused_trapINV:
    XT_TRAP 8
    .global xt_unused_trapHW
xt_unused_trapHW:
    XT_TRAP 9

    .global xt_double_trap
    .type xt_double_trap, @function
xt_double_trap:
    l32r    a0, .Ldiag_lit2
    l32i    a1, a0, 224
    addi    a1, a1, 1
    s32i    a1, a0, 224
    j       1f
    .align 4
2:
    .long   xt_exc_dump
1:
    l32r    a3, 2b
    movi    a4, 0xdead
    s32i    a4, a3, 32
    rsr     a4, exccause
    s32i    a4, a3, 36
    rsr     a4, epc1
    s32i    a4, a3, 40
    rsr     a4, epc2
    s32i    a4, a3, 44
    rsr     a4, excvaddr
    s32i    a4, a3, 48
    rsr     a4, ps
    s32i    a4, a3, 52
    rsr     a4, windowbase
    s32i    a4, a3, 56
    rsr     a4, windowstart
    s32i    a4, a3, 60
    s32i    a0, a3, 64
    s32i    a1, a3, 68
    s32i    a2, a3, 72
    s32i    a3, a3, 76
    s32i    a4, a3, 80
    s32i    a5, a3, 84
    s32i    a6, a3, 88
    s32i    a7, a3, 92
    s32i    a8, a3, 96
    s32i    a9, a3, 100
    s32i    a10, a3, 104
    s32i    a11, a3, 108
    s32i    a12, a3, 112
    s32i    a13, a3, 116
    s32i    a14, a3, 120
    s32i    a15, a3, 124
4:
    j       4b
