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

    .align 4
.Lwinctr:
    .long   0x50001400          /* RTC window-handler counters */

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
 * Level-1 interrupt / exception handler.
 *
 * This is ESP-IDF's _xt_lowint1 + _xt_context_save + _xt_user_exit structure,
 * written out for Rockbox's single-core kernel.
 *
 * Why the spill is mandatory: the esp32s3 core config has no REGB option, so
 * there are no spill/refill opcodes. Register windows are moved to the stack by
 * the window overflow/underflow handlers instead. The interruptee's call chain
 * still lives in the physical register file. If the dispatch calls anything (a
 * call8 rotates the window by 8), the overflow handler walks the ABI base-save
 * chain of the ancestor frames - and unless those ancestors have been spilled,
 * it reads a stack pointer out of memory that belongs to nobody and stores
 * through it. Shallow ISRs happen to survive; a dispatch that nests does not.
 *
 * SPILL_ALL_WINDOWS (Cadence xt_asm_utils.h, XCHAL_NUM_AREGS == 64) is a no-op
 * write to a12 followed by a window rotation, repeated until the window has made
 * a full 16-register turn. Each write raises a window overflow if a12 holds a
 * live ancestor register, and the overflow handler spills that window into the
 * frame that owns it. The net rotation is zero, so the window ends where it
 * started with every ancestor window on the stack.
 *
 * The spill has to run with sp set back to the interruptee's own stack pointer,
 * not the handler's frame: only the interruptee's frames have a valid base-save
 * area for the handlers to walk.
 *
 * EPC1 is stashed around the spill because the overflow exceptions overwrite it.
 * The zero-overhead loop state is saved and LCOUNT cleared, because the dispatch
 * runs its own loops.
 */
#define FR_A0     0
#define FR_A1     4
#define FR_A2     8
#define FR_A3     12
#define FR_A4     16
#define FR_A5     20
#define FR_A6     24
#define FR_A7     28
#define FR_A8     32
#define FR_A9     36
#define FR_A10    40
#define FR_A11    44
#define FR_A12    48
#define FR_A13    52
#define FR_A14    56
#define FR_A15    60
#define FR_PS     64
#define FR_PC     68
#define FR_CAUSE  72
#define FR_SAR    76
#define FR_LBEG   80
#define FR_LEND   84
#define FR_LCOUNT 88
#define FR_EPC1   92
#define FR_INT    96
#define FR_INTEN  100
#define FR_SIZE   256           /* 16-byte aligned; offsets 128..255 are the
                             * spill/ancestor area the overflow handlers use */

    .section .vector,"ax",@progbits
    .align 4
/* XT_ISR_SPILL: 1 = run ESP-IDF's SPILL_ALL_WINDOWS before dispatching.
 * The dispatch is a C call chain; without the spill its nested frames overflow
 * into ancestor windows the handler never made reachable.
 */
.equ    XT_ISR_SPILL, 1
.equ    XT_ISR_DISPATCH, 0
.equ    ISR_STACK_SIZE, 4096

    .section .srambss
    .align  16
    .global xt_hb_ctr
xt_hb_ctr:
    .zero   4
    .global xt_isr_stack
xt_isr_stack:
    .zero   ISR_STACK_SIZE

    .section .vector,"ax",@progbits
    .align 4
    .global xt_level1_handler
    .type xt_level1_handler, @function

xt_level1_handler:
    /* Level-1 handler, built up in stages so a fault can be attributed.
     *
     * Stage 1: a small frame on the interruptee's stack, scratch registers only,
     * timer deasserted, tick counter in internal SRAM. No PSRAM, no RTC, no call.
     */
    j       1f
    .align 4
2:
    .long   0x6002007C          /* TIMERG1 tn_int_clr   */
3:
    .long   0x60020000          /* TIMERG1 tn_config(0) */
4:
    .long   0x60020024          /* TIMERG1 tn_config(1) */
5:
    .long   xt_tick_count
6:
    .long   current_tick
1:
    movi    a2, 4
    rsr     a3, exccause
    bne     a3, a2, 9f

    addi    sp, sp, -16
    s32i    a2, sp, 0
    s32i    a3, sp, 4
    s32i    a4, sp, 8

    /* tn_int_clr bit0/1 = alarm fired for timer 0/1 */
    l32r    a2, 2b
    movi    a3, 3
    s32i    a3, a2, 0
    /* tn_alarm_en must be re-armed: it self-clears when the alarm fires. */
    l32r    a2, 3b
    l32i    a3, a2, 0
    movi    a4, 0x400
    or      a3, a3, a4
    s32i    a3, a2, 0
    l32r    a2, 4b
    l32i    a3, a2, 0
    movi    a4, 0x400
    or      a3, a3, a4
    s32i    a3, a2, 0

    l32r    a2, 5b
    l32i    a3, a2, 0
    addi    a3, a3, 1
    s32i    a3, a2, 0
    /* The kernel clock. This handler deliberately does not branch into C, so
     * current_tick is bumped here rather than by timer_irq(); without it every
     * thread blocks in sleep() forever.
     */
    l32r    a2, 6b
    l32i    a3, a2, 0
    addi    a3, a3, 1
    s32i    a3, a2, 0
    /* Mirror into the RTC liveness block so a probe can see the tick advancing
     * without needing the D-cache to be coherent. */
    movi    a4, 0x50000C00
    s32i    a3, a4, 12
    /* Heartbeat into RTC so a probe can tell "the CPU is alive" from "a thread
     * is spinning without a tick". */
    movi    a2, 0x50000D60
    l32r    a3, 5b
    l32i    a3, a3, 0
    s32i    a3, a2, 0

    l32i    a2, sp, 0
    l32i    a3, sp, 4
    l32i    a4, sp, 8
    addi    sp, sp, 16
8:
    rfe

/* Anything other than the timer alarm reaching the level-1 vector is a real
 * exception. Returning with rfe would re-execute the faulting instruction
 * forever, so record it and stop instead. */
9:
    movi    a2, 0x50001900
    rsr     a3, exccause
    s32i    a3, a2, 0
    rsr     a3, epc1
    s32i    a3, a2, 4
    /* Fetch the faulting word through the DROM alias of the same MMU page: the
     * IROM window is instruction-cache only, so a data load at epc1 itself would
     * fault and hide the record. */
    mov     a4, a3
    movi    a5, 0x42000000
    sub     a4, a4, a5
    movi    a5, 0x3C000000
    add     a4, a4, a5
    l32i    a5, a4, 0
    s32i    a5, a2, 24              /* instruction word at epc1 */
    rsr     a3, excvaddr
    s32i    a3, a2, 8
    rsr     a3, ps
    s32i    a3, a2, 12
    rsr     a3, windowbase
    s32i    a3, a2, 16
    rsr     a3, windowstart
    s32i    a3, a2, 20
    mov     a5, sp
    s32i    a5, a2, 28              /* handler sp */
    rsr     a5, excsave1
    s32i    a5, a2, 32              /* return address of the handler */
    mov     a5, a0
    s32i    a5, a2, 36
    mov     a5, a1
    s32i    a5, a2, 40
    mov     a5, a2
    s32i    a5, a2, 44
    mov     a5, a3
    s32i    a5, a2, 48
    movi    a3, 0x50001400
    l32i    a4, a3, 24
    addi    a4, a4, 1
    s32i    a4, a3, 24
    j       xt_exc_trap

/*
 * Anything that reaches here is a real fault. The record lives in internal SRAM
 * on the instruction bus, so the trap code can reach it with l32r without
 * crossing the 0x6F0000 gap to the data-bus view; a probe reads it through the
 * alias. Words 0-11 are the plain exception, 56/57 the trap counts, and 32-60
 * the nested (double) exception.
 */
    /* RTC slow memory (8 KB at 0x50000000) survives the reset esptool must do to
     * read memory, so the fault record of a crashed run is still readable when the
     * probe attaches. Internal SRAM is zeroed by crt0, which hides it.
     */
    .global xt_exc_dump
    .equ  xt_exc_dump, 0x50000E80

    .section .vector,"ax",@progbits
    .align 4
.Ldiag_lit2:
    .long   xt_exc_dump
.Lblog_lit:
    .long   boot_log
.Lblen_lit:
    .long   boot_log_len

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
7:
    .long   0x50000C00
1:
    /* Fault cause, PC and bad address in live[19..21] so a probe that only reads
     * the live block still sees the crash. */
    l32r    a6, 7b
    rsr     a4, exccause
    s32i    a4, a6, 76
    rsr     a4, epc1
    s32i    a4, a6, 80
    rsr     a4, excvaddr
    s32i    a4, a6, 84
    l32i    a4, sp, 68
    s32i    a4, a6, 88
    l32i    a4, sp, 0
    s32i    a4, a6, 92
    l32i    a4, sp, 4
    s32i    a4, a6, 96
    l32i    a4, sp, 8
    s32i    a4, a6, 100
    l32i    a4, sp, 12
    s32i    a4, a6, 104
    l32i    a4, sp, 16
    s32i    a4, a6, 108
    l32i    a4, sp, 20
    s32i    a4, a6, 112
    l32i    a4, sp, 24
    s32i    a4, a6, 116
    l32i    a4, sp, 28
    s32i    a4, a6, 120
    l32r    a5, 2b
    l32i    a6, sp, 0
    s32i    a6, a5, 16
    l32i    a6, sp, 4
    s32i    a6, a5, 20
    l32i    a6, sp, 8
    s32i    a6, a5, 24
    l32i    a6, sp, 12
    s32i    a6, a5, 28
    l32i    a6, sp, 16
    s32i    a6, a5, 32
    l32i    a6, sp, 20
    s32i    a6, a5, 36
    l32i    a6, sp, 24
    s32i    a6, a5, 40
    l32i    a6, sp, 28
    s32i    a6, a5, 44
    l32i    a6, sp, 32
    s32i    a6, a5, 48
    l32i    a6, sp, 36
    s32i    a6, a5, 52
    l32i    a6, sp, 40
    s32i    a6, a5, 56
    l32i    a6, sp, 44
    s32i    a6, a5, 60
    l32i    a6, sp, 48
    s32i    a6, a5, 64
    l32i    a6, sp, 52
    s32i    a6, a5, 68
    l32i    a6, sp, 56
    s32i    a6, a5, 72
    l32i    a6, sp, 60
    s32i    a6, a5, 76
    l32i    a6, sp, 64
    s32i    a6, a5, 80          /* saved PS */
    l32i    a6, sp, 76
    s32i    a6, a5, 84          /* saved SAR */

    movi    a6, 0
    s32i    a6, a5, 32              /* marker: a4's slot is not usable */
    l32i    a6, a5, 228
    addi    a6, a6, 1
    s32i    a6, a5, 228
    rsr     a6, exccause
    s32i    a6, a5, 0
    rsr     a6, epc1
    s32i    a6, a5, 4
    rsr     a6, excvaddr
    s32i    a6, a5, 8
    rsr     a6, epc2
    s32i    a6, a5, 12
    rsr     a6, ps
    s32i    a6, a5, 104
    l32i    a6, sp, 64
    s32i    a6, a5, 132
    rsr     a6, windowbase
    s32i    a6, a5, 108
    rsr     a6, windowstart
    s32i    a6, a5, 112
    rsr     a6, excsave1
    s32i    a6, a5, 116
    rsr     a6, intenable
    s32i    a6, a5, 120
    rsr     a6, interrupt
    s32i    a6, a5, 124
    /* Stamp the boot log at the moment of the fault. boot_trace() is C and
     * cannot be called in exception mode, so the byte is written by hand. This
     * tells us how far boot got before the trap, which is the difference between
     * "crashed during boot" and "crashed after boot in a thread".
     */
    l32r    a6, .Lblen_lit
    l32i    a6, a6, 0
    s32i    a6, a5, 128
    /* stamp 'X' into the RTC ring so the probe can see where the trap happened */
    movi    a6, 0x50001004
    l32i    a7, a6, 0
    movi    a4, 0x50000000
    add     a4, a4, a7
    movi    a7, 0x58
    s8i     a7, a4, 0
    addi    a7, a7, 0
    s32i    a7, a6, 0
    l32r    a6, .Lblog_lit
    l32r    a4, .Lblen_lit
    l32i    a4, a4, 0
    movi    a7, 8191
    blt     a4, a7, 4f
    movi    a4, 0
4:
    movi    a7, 0x58                /* 'X' */
    add     a6, a6, a4
    s32i    a7, a6, 0
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
3:
    .long   0x50001004          /* RTC boot_log length */
4:
    .long   0x50000000          /* RTC boot_log base */
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
    /* Stamp the ring so a probe can see which vector was reached: the ring is
     * the only thing that survives the reset esptool needs. */
    l32r    a4, 3b
    l32i    a5, a4, 0
    l32r    a6, 4b
    add     a6, a6, a5
    movi    a7, 0x40 + \id
    s8i     a7, a6, 0
    addi    a5, a5, 1
    s32i    a5, a4, 0
5:
    j       5b
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

    .section .vector,"ax",@progbits
    .align 4
    .global xt_double_trap
    .type xt_double_trap, @function
xt_double_trap:
    l32r    a0, .Ldiag_lit2
    l32i    a1, a0, 224
    addi    a1, a1, 1
    s32i    a1, a0, 224
    j       5f
    .align  4
6:
    .long   0x50001004
7:
    .long   0x50000000
5:
    l32r    a1, 6b
    l32i    a2, a1, 0
    l32r    a3, 7b
    add     a3, a3, a2
    movi    a4, 0x44              /* 'D' = double exception reached */
    s8i     a4, a3, 0
    addi    a2, a2, 1
    s32i    a2, a1, 0
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
    mov     a4, sp
    s32i    a4, a3, 64
    rsr     a4, excsave1
    s32i    a4, a3, 68
    mov     a4, a0
    s32i    a4, a3, 72
    mov     a4, a1
    s32i    a4, a3, 76
    mov     a4, a2
    s32i    a4, a3, 80
    mov     a4, a3
    s32i    a4, a3, 84
    mov     a4, a4
    s32i    a4, a3, 88
    mov     a4, a5
    s32i    a4, a3, 92
    mov     a4, a6
    s32i    a4, a3, 96
    mov     a4, a7
    s32i    a4, a3, 100
    mov     a4, a8
    s32i    a4, a3, 104
    mov     a4, a9
    s32i    a4, a3, 108
    mov     a4, a10
    s32i    a4, a3, 112
    mov     a4, a11
    s32i    a4, a3, 116
    mov     a4, a12
    s32i    a4, a3, 120
    mov     a4, a13
    s32i    a4, a3, 124
    mov     a4, a14
    s32i    a4, a3, 128
    mov     a4, a15
    s32i    a4, a3, 132
4:
    j       4b
