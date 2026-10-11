/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
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
 * The live register windows above the current one hold the state of every
 * ancestor call, so setjmp has to spill them onto the stack first; libgcc's
 * __xtensa_libgcc_window_spill does that by nesting entry/call frames until the
 * hardware raises a window overflow for each one. Saving a0-a15 plus
 * WINDOWBASE/WINDOWSTART/PS/SAR is then the whole context: longjmp puts the
 * window back and the underflow handlers refill the ancestors as the stack
 * unwinds.
 *
 * PS has to be part of the save area, not just for INTLEVEL but for CALLINC:
 * the return point longjmp jumps to belongs to a function entered with a
 * particular CALLINC, and its retw rotates by whatever PS.CALLINC says.
 */

#define JB_A0    0
#define JB_PS    64
#define JB_WB    68
#define JB_WS    72
#define JB_SAR   76

        .text
        .align 4

        .globl  setjmp
        .type   setjmp, @function
setjmp:
        entry   a1, 32
        mov     a3, a2                    /* a3 = jmp_buf */

        l32r    a4, __xtensa_libgcc_window_spill
        callx8  a4

        rsr     a4, windowbase
        s32i    a4, a3, JB_WB
        rsr     a4, windowstart
        s32i    a4, a3, JB_WS
        rsr     a4, ps
        s32i    a4, a3, JB_PS
        rsr     a4, sar
        s32i    a4, a3, JB_SAR

        s32i    a0,  a3, 0
        s32i    a1,  a3, 4
        s32i    a2,  a3, 8
        s32i    a3,  a3, 12
        s32i    a4,  a3, 16
        s32i    a5,  a3, 20
        s32i    a6,  a3, 24
        s32i    a7,  a3, 28
        s32i    a8,  a3, 32
        s32i    a9,  a3, 36
        s32i    a10, a3, 40
        s32i    a11, a3, 44
        s32i    a12, a3, 48
        s32i    a13, a3, 52
        s32i    a14, a3, 56
        s32i    a15, a3, 60

        movi    a2, 0
        retw

        .globl  longjmp
        .type   longjmp, @function
longjmp:
        entry   a1, 32                    /* a2 = jmp_buf, a3 = value */

        l32i    a4, a2, JB_WB
        wsr     a4, windowbase
        rsync
        l32i    a4, a2, JB_WS
        wsr     a4, windowstart
        l32i    a4, a2, JB_PS
        wsr     a4, ps
        l32i    a4, a2, JB_SAR
        wsr     a4, sar
        rsync

        /* WINDOWBASE has moved, so keep the save area in a scratch that is
         * written last. */
        mov     a15, a2
        mov     a14, a3
        l32i    a0,  a15, 0
        l32i    a1,  a15, 4
        l32i    a2,  a15, 8
        l32i    a3,  a15, 12
        l32i    a4,  a15, 16
        l32i    a5,  a15, 20
        l32i    a6,  a15, 24
        l32i    a7,  a15, 28
        l32i    a8,  a15, 32
        l32i    a9,  a15, 36
        l32i    a10, a15, 40
        l32i    a11, a15, 44
        l32i    a12, a15, 48
        l32i    a13, a15, 52
        l32i    a14, a15, 56
        l32i    a15, a15, 60

        mov     a2, a14                   /* longjmp() returns in a2 */
        jx      a0
