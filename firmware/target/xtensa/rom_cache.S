/***************************************************************************
 * Calling the ROM from call0 code.
 *
 * The ESP32-S3 ROM is built for the windowed ABI, and Rockbox is call0. The two
 * do not mix: a windowed callee rotates the register window, so its a8-a15 are
 * not the physical registers a call0 caller keeps live across the call, and
 * `retw` returns to a0 of the caller's window rather than to physical a0.
 *
 * The trampoline makes the call look like an ordinary windowed call:
 *
 *   1. `callx4` sets CALLINC to 4 and rotates the window by 4. CALLINC cannot be
 *      read or written on this core config (there is no REGB option, so
 *      mtsr/mfsr are not valid opcodes), but the call instruction itself sets it,
 *      and the callee's `entry` and `retw` both use that same value. Nothing has
 *      to be arranged at startup;
 *
 *   2. the callee's window is then four above the caller's, so it cannot reach
 *      physical a0-a3 (the caller's a0-a1 are safe), but it does cover a4-a15.
 *      Under call0 only a12-a15 are callee-saved, so those are the four that have
 *      to be stashed;
 *
 *   3. arguments land in the callee's a2 and a3, which are the caller's a6 and a7,
 *      and the return value comes back the same way: the callee's a2 is the
 *      caller's a6, not the caller's a2. `ret.n` hands the result to C in a2, so
 *      the trampoline has to copy it.
 *
 * WINDOW_BASE is pinned to 0 in crt0.S so that the window call0 code uses is
 * window 0, which makes the register arithmetic above explicit rather than
 * relative to whatever the bootloader left behind.
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

    mov     a6, a3            /* callee a2 */
    mov     a7, a4            /* callee a3 */
    callx4  a2                /* a2 is the ROM function */
    mov     a2, a6            /* callee a2 == caller a6 */

    l32i    a12, a1, 0
    l32i    a13, a1, 4
    l32i    a14, a1, 8
    l32i    a15, a1, 12
    addi    a1, a1, 32
    ret.n
