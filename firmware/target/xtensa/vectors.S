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
    j .                               /* level 2 - not used */
    .org _vector_table + 0x1c0
    j .                               /* level 3 */
    .org _vector_table + 0x200
    j .                               /* level 4 */
    .org _vector_table + 0x240
    j .                               /* level 5 */
    .org _vector_table + 0x280
    j .                               /* level 6 / debug */
    .org _vector_table + 0x2c0
    j .                               /* NMI */

    .org _vector_table + 0x300
    j xt_level1_handler               /* level 1, supervisor mode */
    .org _vector_table + 0x340
    j xt_level1_handler               /* level 1, user mode */

    .org _vector_table + 0x380
    j xt_double_trap                   /* double exception - the real one */
    .org _vector_table + 0x3c0
    j .                               /* invalid exception */
    .org _vector_table + 0x400
    j .                               /* hardware exception */

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
 * from the interrupted code's point of view, so all of them are scratch here. The
 * windows above the current one are spilled by the overflow handlers as the C call
 * rotates through them, and refilled by the underflow handlers on the way out, so
 * saving the current window is enough.
 *
 * The frame is 80 bytes so that a1 stays on the 16-byte boundary the ABI requires.
 */
#define FR_A0   0
#define FR_A2   4
#define FR_A3   8
#define FR_A4   12
#define FR_A5   16
#define FR_A6   20
#define FR_A7   24
#define FR_A8   28
#define FR_A9   32
#define FR_A10  36
#define FR_A11  40
#define FR_A12  44
#define FR_A13  48
#define FR_A14  52
#define FR_A15  56
#define FR_PS   60
#define FR_SIZE 64

    .section .vector,"ax",@progbits
    .align 4
    .global xt_level1_handler
    .type xt_level1_handler, @function

xt_level1_handler:
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
    addi    a1, a1, -FR_SIZE
    rsr     a2, ps
    s32i    a2, a1, FR_PS
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

    /* INTERRUPT is the pending bitmask over all 32 slots. The handler is entered
     * for any level-1 slot, so the dispatch has to work out which one it is. */
    /* C code cannot run in exception mode: PS.EXCM disables the register window,
     * so the `entry`/`retw` pair every dispatched handler compiles to would not
     * rotate anything. Clear EXCM, keep interrupts masked at level 1, and let the
     * window machinery work. rfe restores PS from the saved copy, so the value we
     * stash above is what the interruptee gets back.
     */
    movi    a2, 0x00040001            /* PS_INTLEVEL(1) | PS_WOE, EXCM clear */
    wsr     a2, ps
    rsync

    rsr     a2, interrupt
    call8   xt_irq_dispatch

    l32i    a2, a1, FR_PS
    wsr     a2, ps
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
    addi    a1, a1, FR_SIZE

    rfe

/*
 * Anything that reaches here is a real fault. Record enough to identify it in
 * internal SRAM, which survives the reset esptool performs when it attaches, then
 * spin so the chip stays reachable.
 */
    .section .ibss,"aw",@progbits
    .align 4
    .global xt_exc_dump
xt_exc_dump:
    .word 0, 0, 0, 0, 0, 0, 0, 0

    .section .vector,"ax",@progbits
    .align 4
    .global xt_exc_trap
    .type xt_exc_trap, @function
xt_exc_trap:
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
3:
    j       3b

    .global xt_double_trap
    .type xt_double_trap, @function
xt_double_trap:
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
4:
    j       4b
