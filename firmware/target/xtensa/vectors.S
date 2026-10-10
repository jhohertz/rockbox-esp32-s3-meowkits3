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
 * 4, 8 and 12, with the alloca handler in the gaps) and level 2 starts at 0x180.
 * Under the call0 ABI GCC never emits a spill or reflect, so no window vector can
 * be reached; each one is a self-loop trap rather than a run of zeros, so an
 * unexpected exception stops cleanly instead of executing padding.
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
    j .                               /* window overflow  4 */
    .org _vector_table + 0x040
    j .                               /* window underflow 4 */
    .org _vector_table + 0x080
    j .                               /* window overflow  8 */
    .org _vector_table + 0x0c0
    j .                               /* window underflow 8 */
    .org _vector_table + 0x100
    j .                               /* window overflow  12 */
    .org _vector_table + 0x140
    j .                               /* window underflow 12 */

    .org _vector_table + 0x180
    j .                               /* level 2 */
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
    j .                               /* software exception */
    .org _vector_table + 0x3c0
    j .                               /* double exception */
    .org _vector_table + 0x400
    j .                               /* invalid exception */

    .size _vector_table, . - _vector_table

/*
 * Kept in the same section as the table so that the call0 below is inside the 18-bit
 * range of the j that reaches it. -mlongcalls does not help hand-written asm.
 */

/*
 * The handler runs on the interrupted thread's stack. a0 and a2-a15 have to be
 * saved because the C dispatch is free to clobber them: under call0 a0 and a2-a11
 * are caller-saved and a12-a15 are callee-saved, but a handler is not a call from
 * the interrupted code's point of view, so all of them are scratch here.
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
#define FR_SIZE 80

    .section .vector,"ax",@progbits
    .align 4
    .global xt_level1_handler
    .type xt_level1_handler, @function

xt_level1_handler:
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

    /* INTERRUPT is the pending bitmask over all 32 slots. The handler is entered
     * for any level-1 slot, so the dispatch has to work out which one it is. */
    rsr     a2, interrupt
    call0   xt_irq_dispatch

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
