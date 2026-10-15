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

/*
 * Xtensa under the windowed ABI.
 *
 * The save area below is ESP-IDF's own exception frame, XtExcFrame from
 * xtensa_context.h -- the same structure IDF builds on every level-1 interrupt
 * and every task switch, and the same one _xt_context_save,
 * _xt_context_restore and _xt_user_exit already know how to fill in and leave.
 * It is spelled out in words here rather than included because this header is
 * also read by the freestanding domain (codecs and plugins), which has no IDF
 * include path; the .c file static_asserts the correspondence.
 *
 * This replaces an earlier design that saved only sp/pc/ps/WINDOWBASE and let
 * the window underflow handlers refill the registers as the thread unwound.
 * That model needs the resumed thread's window *chain* to still be live in the
 * hardware window slots, and any other thread that ran in between overwrites
 * them. It also needs a8 and a9 -- the registers _WindowUnderflow8 uses to find
 * the base save area -- to survive the switch, which is exactly what a switch
 * wants to use them for. IDF does not do that. It flattens:
 * _xt_context_restore loads a2..a15 straight out of the frame and never touches
 * WINDOWBASE or WINDOWSTART. There is no such field in the frame at all.
 *
 * Flattening is correct here for the same reason it is correct for FreeRTOS:
 * every switch happens at the same call depth (switch_thread), so every thread
 * presents the same window layout above that point, and the frame's a2..a15
 * mean the same thing for all of them no matter where WINDOWBASE happens to
 * sit. Depth, not base, is what has to match.
 */

/* >= sizeof(XtExcFrame)/4, with slack. Asserted in the .c file. */
#define XT_FRAME_WORDS 40

struct regs
{
    uint32_t sp;      /* stack pointer at the switch; before a thread has run
                       * this is the top of its stack */
    uint32_t start;   /* entry point, zero once the thread has run */
    uint32_t frame[XT_FRAME_WORDS];   /* XtExcFrame */
};

/* Fixed prefix of XtExcFrame, in words. Mirrors xtensa_context.h; the .c file
 * asserts each one against the real struct. */
#define XT_F_EXIT   0
#define XT_F_PC     1
#define XT_F_PS     2
#define XT_F_A0     3
#define XT_F_A1     4
#define XT_F_A2     5
#define XT_F_A6     9

/* The level-1 handler runs on the interrupted thread's stack: its own frame,
 * then the dispatch, the GDMA completion path and the mixer chain that the
 * completion callback drives. */
#define DEFAULT_STACK_SIZE 0x1000 /* Bytes */
