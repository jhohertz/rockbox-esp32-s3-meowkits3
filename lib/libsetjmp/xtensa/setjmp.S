/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\______ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by the Rockbox project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"

/*
 * setjmp/longjmp for Xtensa under the windowed ABI.
 *
 * This follows the algorithm in the toolchain's own picolibc object
 * (libc_machine_xtensa_setjmp.S.o in picolibc/.../lib/esp32s3/libc.a), which was
 * read off the disassembly because the source is not in the IDF tree. It is not
 * a stylistic preference. The previous version here rotated WINDOWBASE, restored
 * PS and finished with `jx a0`, and that approach cannot be repaired:
 *
 *   after `wsr WINDOWBASE` the names a0..a15 denote different physical
 *   registers, so there is no way to name the target window's registers from
 *   inside the current one. Anything held in a register before the rotation is
 *   simply no longer where that name points afterwards.
 *
 * picolibc never touches WINDOWBASE or PS at all -- measured on its
 * disassembly: zero writes to either, zero `jx`, two `retw`. Instead it
 * reconstructs the base-save areas in memory and returns normally, and the
 * hardware window-underflow exception refills the registers on the way out. The
 * window stride comes from the return address itself: CALLINC is encoded in its
 * top two bits, which is what makes the ancestor walk possible.
 *
 * Save area layout, in bytes:
 *
 *   0  .. 15   this frame's base-save area, i.e. the caller's a0..a3
 *   16 ..      the ancestor base-save areas below the caller, CALLINC-1 chunks
 *              of 16 bytes (only reached when the caller used call8 or call12).
 *              Setjmp walks them from a1+4 and longjmp re-reads that same value
 *              out of buf[52], so the two agree on the base without either one
 *              having to recompute it.
 *   48 .. 63   this frame's spill area, a1+0,4,8,12
 *   64         the return address longjmp goes back to
 *
 * _JBLEN is 20 words = 80 bytes, which covers the 68 bytes used above.
 */

#define JB_SPILL_AREA  48    /* buf[48..63]: this frame's spill area */
#define JB_RETADDR     64    /* buf[64]:    return address           */

        .text
        .align 4

/* l32r reaches its operand through a PC-relative literal, and the pool entry
 * has to exist. Letting the assembler synthesise one for an external symbol
 * does not survive this binutils and linker-script combination: inside a codec
 * image the reference resolved to the symbol's own address instead of to a
 * literal, so l32r loaded the contents of that address -- the first word of
 * __xtensa_libgcc_window_spill, `entry a1, ...' = 0x75006136 -- and the very
 * next instruction callx8'd to it. IllegalInstruction at a garbage PC,
 * identical on every attempt, which is what made it look like a corrupt image
 * rather than a missing literal.
 *
 * Emitting the literal explicitly, in the same section and ahead of the use, is
 * the same workaround this tree already needs for linker-script-defined symbols
 * for the same reason. */
        .align 2
.Lspill_lit:
        .word __xtensa_libgcc_window_spill
        .align 4

        .globl  setjmp
        .type   setjmp, @function
setjmp:
        entry   a1, 16                    /* a2 = jmp_buf */

        /* callx4, not callx8: the spill routine has to nest frames so that the
         * hardware raises a window overflow for each one, and it walks the
         * window we are actually in. */
        l32r    a4, .Lspill_lit
        callx4  a4

        /* Our own base-save area holds the caller's a0..a3, placed there by the
         * overflow handler. That is the state longjmp has to put back. */
        addi    a5, a1, -16
        l32i.n  a3, a5, 0
        l32i.n  a4, a5, 4
        s32i.n  a3, a2, 0
        s32i.n  a4, a2, 4
        l32i.n  a3, a5, 8
        l32i.n  a4, a5, 12
        s32i.n  a3, a2, 8
        s32i.n  a4, a2, 12

        /* Walk the ancestors, but only if the caller overlaps more than its own
         * window. CALLINC lives in bits 31:30 of the return address; call8 is 2
         * and call12 is 3, and either means the frames below ours hold spilled
         * windows that a longjmp back here would otherwise lose. */
        extui   a3, a0, 30, 2
        blti    a3, 2, .Lsetjmp_ancestors_done

        l32i.n  a7, a1, 4                 /* caller's stack pointer */
        slli    a4, a3, 4                 /* CALLINC * 16 */
        sub     a5, a7, a4                /* first ancestor base-save area */
        addi    a6, a2, 16                /* destination in the buffer */
        addi    a7, a7, -16               /* stop when we reach ours */
.Lsetjmp_ancestor_loop:
        l32i.n  a3, a5, 0
        l32i.n  a4, a5, 4
        s32i.n  a3, a6, 0
        s32i.n  a4, a6, 4
        l32i.n  a3, a5, 8
        l32i.n  a4, a5, 12
        s32i.n  a3, a6, 8
        s32i.n  a4, a6, 12
        addi    a5, a5, 16
        addi    a6, a6, 16
        blt     a5, a7, .Lsetjmp_ancestor_loop
.Lsetjmp_ancestors_done:

        /* Our frame's spill area, which is what the underflow handler will read
         * when longjmp returns into the caller. */
        l32i.n  a3, a1, 0
        l32i.n  a4, a1, 4
        s32i    a3, a2, JB_SPILL_AREA
        s32i    a4, a2, JB_SPILL_AREA + 4
        l32i.n  a3, a1, 8
        l32i.n  a4, a1, 12
        s32i    a3, a2, JB_SPILL_AREA + 8
        s32i    a4, a2, JB_SPILL_AREA + 12

        s32i    a0, a2, JB_RETADDR

        movi    a2, 0
        retw

        .globl  longjmp
        .type   longjmp, @function
longjmp:
        entry   a1, 16                    /* a2 = jmp_buf, a3 = value */

        /* Mark only the current window active. This is not a rotation --
         * WINDOWBASE is left exactly as it is, so a2 keeps naming the buffer
         * for the whole routine, which is the property the old version
         * lacked. */
        rsr.windowbase a5
        movi    a4, 1
        ssl     a5
        sll     a4, a4
        wsr.windowstart a4
        rsync

        l32i    a0, a2, JB_RETADDR

        /* Put the caller's a0..a3 where our own retw will find them. */
        addi    a7, a1, -16
        l32i.n  a4, a2, 0
        l32i.n  a5, a2, 4
        s32i.n  a4, a7, 0
        s32i.n  a5, a7, 4
        l32i.n  a4, a2, 8
        l32i.n  a5, a2, 12
        s32i.n  a4, a7, 8
        s32i.n  a5, a7, 12

        /* And the ancestor windows, back onto their own frames' base-save
         * areas, so that unwinding through them refills each in turn. */
        extui   a7, a0, 30, 2
        blti    a7, 2, .Llongjmp_ancestors_done

        l32i    a8, a2, JB_SPILL_AREA + 4  /* the same a1+4 setjmp walked from */
        slli    a4, a7, 4
        sub     a6, a8, a4
        addi    a5, a2, 16                /* source in the buffer */
        addi    a8, a8, -16
.Llongjmp_ancestor_loop:
        l32i.n  a7, a5, 0
        l32i.n  a4, a5, 4
        s32i.n  a7, a6, 0
        s32i.n  a4, a6, 4
        l32i.n  a7, a5, 8
        l32i.n  a4, a5, 12
        s32i.n  a7, a6, 8
        s32i    a4, a6, 12
        addi    a5, a5, 16
        addi    a6, a6, 16
        blt     a6, a8, .Llongjmp_ancestor_loop
.Llongjmp_ancestors_done:

        /* Finally the frame we are actually returning into: buf[48..63] goes to
         * (caller sp - 16), its base-save area. */
        l32i.n  a7, a2, 4
        addi    a7, a7, -16
        l32i    a4, a2, JB_SPILL_AREA
        l32i    a5, a2, JB_SPILL_AREA + 4
        s32i.n  a4, a7, 0
        s32i.n  a5, a7, 4
        l32i    a4, a2, JB_SPILL_AREA + 8
        l32i    a5, a2, JB_SPILL_AREA + 12
        s32i.n  a4, a7, 8
        s32i.n  a5, a7, 12

        /* longjmp() appears to return `value', or 1 when value was 0. setjmp's
         * own 0 is the only other value a caller distinguishes. */
        movi    a2, 1
        movnez  a2, a3, a3

        retw
