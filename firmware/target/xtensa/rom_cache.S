/***************************************************************************
 * Calling the ROM from call0 code.
 *
 * The ESP32-S3 ROM is built for the windowed ABI, and Rockbox is call0. The two
 * do not mix: a windowed callee rotates the register window, so its a8-a15 are
 * not the physical registers a call0 caller keeps live across the call, and
 * `retw` returns to a0 of the caller's window rather than to physical a0.
 *
 * Two things make the call safe:
 *
 *   1. WINDOW_BASE is pinned to 0 at startup (see crt0.S), so window 0 is the
 *      window whose registers call0 code uses, and a windowed callee's window is
 *      a rotation away from it;
 *   2. the call0 callee-saved set (a12-a15) is saved around the call, which is
 *      what this trampoline does.
 *
 * `retw` also needs CALLINC. It cannot be read or written on this core config -
 * there is no REGB option, so mtsr/mfsr/spills/refill are not valid opcodes -
 * but entry sets it to 4, and call0 code never changes it. crt0.S executes one
 * entry instruction at startup so that CALLINC is 4 for the rest of the run.
 *
 * This is the part that cannot be checked from the assembler: it is ISA
 * behaviour, and it has to be confirmed on real silicon.
 ****************************************************************************/

    .section .text.rom_call,"ax",@progbits

    .align 4
    .global xt_rom_call
xt_rom_call:                  /* (uint32_t fn, uint32_t arg0, uint32_t arg1) */
    addi    a1, a1, -32
    s32i    a12, a1, 0
    s32i    a13, a1, 4
    s32i    a14, a1, 8
    s32i    a15, a1, 12

    callx0  a2                /* a2 is the ROM function, a3 and a4 its arguments */

    l32i    a12, a1, 0
    l32i    a13, a1, 4
    l32i    a14, a1, 8
    l32i    a15, a1, 12
    addi    a1, a1, 32
    ret.n
