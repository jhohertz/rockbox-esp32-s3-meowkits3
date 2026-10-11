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
 * A live thread is not just a0-a15: the register windows above the current one
 * hold the state of every ancestor call. Before a switch those windows are
 * written to the thread's own stack by the window overflow handlers (see
 * firmware/target/xtensa/vectors.S), which is what
 * __xtensa_libgcc_window_spill() provokes. After that only the current window is
 * live, so saving it plus WINDOWBASE/WINDOWSTART/PS/SAR is the whole context.
 *
 * On the way back in, WINDOWSTART tells the hardware which windows are spilled,
 * and the underflow handlers refill them as the thread unwinds. The spilled data
 * stays on the thread's stack, below the saved sp, and is only read back before
 * that stack grows again.
 */
struct regs
{
    uint32_t r[16];      /* a0, a2-a15 of the window that was current (a1 in sp) */
    uint32_t sp;
    uint32_t ps;
    uint32_t windowbase;
    uint32_t windowstart;
    uint32_t sar;
    uint32_t start;      /* thread entry point, or NULL once running */
};

#define XT_A0    0
#define XT_PS    68
#define XT_WB    72
#define XT_WS    76
#define XT_SAR   80
#define XT_START 84

/* The level-1 handler runs on the interrupted thread's stack: 80 bytes for its
 * own frame, then the dispatch, the GDMA completion path and the mixer chain
 * that the completion callback drives. */
#define DEFAULT_STACK_SIZE 0x1000 /* Bytes */
