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
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/*
 * GCC does not support the naked attribute on Xtensa, so there is no
 * start_thread() here. The first run of a thread is handled inside
 * load_context instead: a0 is pointed at a trampoline before jumping to the
 * thread function, so the function's ret.n lands in thread_exit().
 */

/*---------------------------------------------------------------------------
 * Store non-volatile context.
 *---------------------------------------------------------------------------
 */
static inline void store_context(void* addr)
{
    asm volatile(
        /* %0 may be one of the registers we are about to store, so get the
         * destination into a scratch register first. */
        "mov     a2, %0              \n"
        "s32i.n  a4, a2, 0           \n" /* a4 */
        "s32i.n  a5, a2, 4           \n" /* a5 */
        "s32i.n  a6, a2, 8           \n" /* a6 */
        "s32i.n  a7, a2, 12          \n" /* a7 */
        "s32i.n  a8, a2, 16          \n" /* a8 */
        "s32i.n  a9, a2, 20          \n" /* a9 */
        "s32i.n  a10, a2, 24         \n" /* a10 */
        "s32i.n  a11, a2, 28         \n" /* a11 */
        "s32i.n  sp, a2, 32          \n" /* a1 */
        "s32i.n  a0, a2, 36          \n" /* a0 */
        : : "r" (addr) : "a2"
    );
}

/*---------------------------------------------------------------------------
 * Load non-volatile context, or start the thread if it has not run yet.
 *---------------------------------------------------------------------------
 */
static inline void load_context(const void* addr)
{
    asm volatile(
        "mov     a2, %0              \n" /* a2 = context */
        "l32i.n  a3, a2, 40          \n" /* start address */
        "beqz.n  a3, xt_running      \n" /* NULL -> already running */
        "movi    a0, xt_trampoline    \n" /* where the thread lands on return */
        "l32i.n  sp, a2, 32          \n" /* initial stack pointer */
        "jx      a3                  \n" /* run the thread */
    "xt_trampoline:                  \n"
        "call0   thread_exit         \n"
    "xt_running:                     \n"
        "l32i.n  a4, a2, 0           \n"
        "l32i.n  a5, a2, 4           \n"
        "l32i.n  a6, a2, 8           \n"
        "l32i.n  a7, a2, 12          \n"
        "l32i.n  a8, a2, 16          \n"
        "l32i.n  a9, a2, 20          \n"
        "l32i.n  a10, a2, 24         \n"
        "l32i.n  a11, a2, 28         \n"
        "l32i.n  sp, a2, 32          \n"
        "l32i.n  a0, a2, 36          \n"
        : : "r" (addr) : "a2", "a3"
    ); /* Only a2 and a3 are clobbered: the rest is the new thread's context */
}

/* Nothing else is needed to start a thread: load_context() checks start. */
#define THREAD_STARTUP_INIT(core, thread, function) \
    ({ (thread)->context.start = (uint32_t)(function); })
