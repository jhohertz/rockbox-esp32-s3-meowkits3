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
 *   1. `callx8` sets CALLINC to 8 and rotates the window by 8. CALLINC cannot be
 *      read or written on this core config (there is no REGB option, so
 *      mtsr/mfsr are not valid opcodes), but the call instruction itself sets it,
 *      and the callee's `entry` and `retw` both use that same value. Nothing has
 *      to be arranged at startup;
 *
 *   2. the callee's window is then eight above the caller's, so it cannot reach
 *      physical a0-a7, but it does cover a8-a23. Under call0 only a12-a15 are
 *      callee-saved, so those are the four that have to be stashed;
 *
 *   3. arguments land in the callee's a2 and a3, which are the caller's a10 and
 *      a11, and the return value comes back the same way: the callee's a2 is the
 *      caller's a10, not the caller's a2. `ret.n` hands the result to C in a2, so
 *      the trampoline has to copy it.
 *
 * callx4 is not usable here. It assembles and disassembles, and the core config
 * advertises windowed ABI, but executing it raises an exception that the ROM's
 * unhandled-exception handler turns into a `break`, which lands in the debug
 * vector and loops until the watchdog resets the chip. callx8 and callx12 both
 * work; callx8 is what ESP-IDF's own callers use, so it is the one the ROM
 * functions are exercised by.
 ****************************************************************************/

    /* .icode, not .text: every ROM call happens during bring-up, while the MSPI
     * block is being reprogrammed, so the trampoline must not be fetched from the
     * flash instruction-cache window it is calling into the ROM from.
     */
    .section .icode,"ax",@progbits

    .align 4
    .global xt_rom_call
xt_rom_call:                  /* (uint32_t fn, uint32_t arg0, uint32_t arg1) */
    addi    a1, a1, -32
    s32i    a12, a1, 0
    s32i    a13, a1, 4
    s32i    a14, a1, 8
    s32i    a15, a1, 12

    mov     a10, a3           /* callee a2 */
    mov     a11, a4           /* callee a3 */
    callx8  a2                /* a2 is the ROM function */
    mov     a2, a10           /* callee a2 == caller a10 */

    l32i    a12, a1, 0
    l32i    a13, a1, 4
    l32i    a14, a1, 8
    l32i    a15, a1, 12
    addi    a1, a1, 32
    ret.n

/*
 * A note on the ROM's OPI command helper. esp_rom_opiflash_exec_cmd takes thirteen
 * arguments, more than a call0 caller can hand to a windowed callee: callx8 exposes
 * only callee a2-a7 (the caller's a10-a15), and callx0 exposes all fourteen but the
 * callee's own `retw.n` raises IllegalInstruction when CALLINC is zero. Verified on
 * hardware: the transaction runs and the fault lands exactly on 0x4004e0cc, the
 * function's retw. So the ROM's OPI path is unreachable from call0 code and the
 * PSRAM transaction has to be assembled by hand in psram-xtensa.c.
 */
