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
 * A thread's live state is not a list of registers. Every call frame on the
 * thread's stack already reserves a 16-byte base save area, and the window
 * overflow/underflow handlers use it: window N's a0, a2, a3 and a4 live in the
 * base save area of window N-1's frame, at (ancestor sp - 16)..(ancestor sp -
 * 4). Spill the windows before switching and the whole call chain is on the
 * thread's own stack. From then on the only thing that identifies where the
 * thread stopped is its stack pointer -- the registers come back by itself,
 * refilled by the underflow handlers as the thread unwinds.
 *
 * So that is what is saved: sp, the a0 that RETW needs to leave the switch
 * frame, and PS. This is the same set FreeRTOS saves for a solicited (i.e.
 * cooperative, non-interrupt) yield -- see XT_SOL_PC / XT_SOL_PS in its
 * portasm.S, and _frxt_dispatch's solicited path, which restores exactly those
 * and then executes retw. Saving a0-a15 as well, which is what a call0 port
 * does, is not a longer version of the same thing: it overwrites a window the
 * hardware is about to refill from memory that is already correct.
 *
 * SAR and the zero-overhead loop registers are deliberately absent. Neither
 * survives a call in the windowed ABI, and a cooperative switch only happens
 * at a call boundary, so a thread cannot be holding live values in them.
 * FreeRTOS likewise does not save them on its solicited path; an interrupt can
 * land inside a zero-overhead loop and an unsolicited frame does carry them.
 */
struct regs
{
    uint32_t sp;      /* stack pointer of the switch_thread frame */
    uint32_t pc;      /* a0 exactly as the windowed ABI encoded it */
    uint32_t ps;      /* PS with INTLEVEL and EXCM cleared, CALLINC 8 */
    uint32_t start;   /* entry point, zero once the thread has run */
};

#define XT_SP    0
#define XT_PC    4
#define XT_PS    8
#define XT_START 12

/* The level-1 handler runs on the interrupted thread's stack: its own frame,
 * then the dispatch, the GDMA completion path and the mixer chain that the
 * completion callback drives. */
#define DEFAULT_STACK_SIZE 0x1000 /* Bytes */
