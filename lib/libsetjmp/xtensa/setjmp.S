/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"

/* setjmp and longjmp for Xtensa under the call0 ABI: a4-a11 are callee-saved,
   a1 is the stack pointer and a0 holds the return address. Arguments and the
   return value go in a2 and a3, so those are not part of the saved state. */

        .text
        .align 4

        .globl  setjmp
setjmp:
        mov     a3, a2                    /* a3 = jmp_buf */
        s32i.n  a4, a3, 0
        s32i.n  a5, a3, 4
        s32i.n  a6, a3, 8
        s32i.n  a7, a3, 12
        s32i.n  a8, a3, 16
        s32i.n  a9, a3, 20
        s32i.n  a10, a3, 24
        s32i.n  a11, a3, 28
        s32i.n  sp, a3, 32
        s32i.n  a0, a3, 36
        movi    a2, 0
        ret.n

        .globl  longjmp
longjmp:
        mov     a0, a3                    /* the value to return */
        bnez.n  a0, nonzero
        movi    a0, 1
nonzero:
        l32i.n  a4, a2, 0
        l32i.n  a5, a2, 4
        l32i.n  a6, a2, 8
        l32i.n  a7, a2, 12
        l32i.n  a8, a2, 16
        l32i.n  a9, a2, 20
        l32i.n  a10, a2, 24
        l32i.n  a11, a2, 28
        l32i.n  sp, a2, 32
        l32i.n  a3, a2, 36                /* the caller's return address */
        mov     a2, a0                    /* longjmp() returns in a2 */
        jx      a3
