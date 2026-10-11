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
 * GCC does not support the naked attribute on Xtensa, so there is no
 * start_thread() here. A thread's first run is handled inside load_context: the
 * window is reset, sp is set to the top of the thread's stack, and control is
 * handed to xt_thread_start(), which runs the thread and calls thread_exit() if
 * it ever returns.
 */

/* Provided by libgcc. It nests entry/call frames until the hardware raises a
 * window overflow for every ancestor window, writing each one into its frame.
 * Without that, only the current window is reachable and the state of the rest
 * of the thread's call chain is lost. */
extern void __xtensa_libgcc_window_spill(void);

/* load_context has to read the context address again after it has moved
 * WINDOWBASE: every a-register changes name when the window rotates, so the
 * argument register is no longer the one holding the pointer. A global is the
 * cheapest window-independent way to get it back.
 *
 * It cannot be reached with a plain `l32r sym`: the context lives in .bss in the
 * PSRAM window while the code is in the flash window, which is far outside l32r's
 * 16-bit PC-relative range. So the address is emitted as a literal in the same
 * section as the code and jumped over.
 */
static volatile uint32_t xt_context_addr;

void xt_thread_start(void (*fn)(void))
{
    fn();
    thread_exit();
}

/*---------------------------------------------------------------------------
 * Store non-volatile context.
 *---------------------------------------------------------------------------
 */
static inline void store_context(void* addr)
{
    __xtensa_libgcc_window_spill();

    asm volatile(
        /* The compiler's own spill slots are on this thread's stack, so
         * restoring sp brings them back with the rest of the frame. */
        "s32i    a0,  %0, 0          \n"
        "s32i    a1,  %0, 64         \n"
        "s32i    a2,  %0, 8          \n"
        "s32i    a3,  %0, 12         \n"
        "s32i    a4,  %0, 16         \n"
        "s32i    a5,  %0, 20         \n"
        "s32i    a6,  %0, 24         \n"
        "s32i    a7,  %0, 28         \n"
        "s32i    a8,  %0, 32         \n"
        "s32i    a9,  %0, 36         \n"
        "s32i    a10, %0, 40         \n"
        "s32i    a11, %0, 44         \n"
        "s32i    a12, %0, 48         \n"
        "s32i    a13, %0, 52         \n"
        "s32i    a14, %0, 56         \n"
        "s32i    a15, %0, 60         \n"
        "mov     a6, %0              \n"
        "rsr     a7, windowbase      \n"
        "s32i    a7, a6, 72          \n"
        "rsr     a7, windowstart     \n"
        "s32i    a7, a6, 76          \n"
        "rsr     a7, ps              \n"
        "s32i    a7, a6, 68          \n"
        "rsr     a7, sar             \n"
        "s32i    a7, a6, 80          \n"
        : : "r" (addr) : "a6", "a7"
    );
}

/*---------------------------------------------------------------------------
 * Load non-volatile context, or start the thread if it has not run yet.
 *---------------------------------------------------------------------------
 */
static inline void load_context(const void* addr)
{
    xt_context_addr = (uint32_t)(uintptr_t)addr;

    asm volatile(
        "j 3f                       \n"
        ".align 4                   \n"
        "10: .long xt_context_addr  \n"
        "11: .long xt_thread_start  \n"
    "3:                             \n"
        "l32r    a6, 10b            \n"
        "l32i    a6, a6, 0          \n"
        "l32i    a7, a6, 84         \n"
        "beqz    a7, 1f             \n"

        /* First run: a clean window at level 0, then enter the starter as if it
         * had been called with CALLINC 0. */
        "movi    a7, 0              \n"
        "wsr     a7, windowbase     \n"
        "wsr     a7, windowstart    \n"
        "wsr     a7, ps             \n"
        "wsr     a7, sar            \n"
        "rsync                      \n"
        "l32r    a6, 10b            \n"
        "l32i    a6, a6, 0          \n"
        "l32i    a1, a6, 64         \n"
        "l32i    a2, a6, 84         \n"
        "l32r    a7, 11b            \n"
        "jx      a7                 \n"

    "1:                             \n"
        /* WINDOWBASE first: the windows above it are refilled from the thread's
         * stack by the underflow handlers as it unwinds. */
        "l32i    a7, a6, 72              \n"
        "wsr     a7, windowbase          \n"
        "rsync                           \n"
        "l32r    a6, 10b                 \n"
        "l32i    a6, a6, 0               \n"
        "l32i    a7, a6, 76              \n"
        "wsr     a7, windowstart         \n"
        "l32i    a7, a6, 68              \n"
        "wsr     a7, ps                  \n"
        "l32i    a7, a6, 80              \n"
        "wsr     a7, sar                 \n"
        "rsync                           \n"
        "l32i    a0,  a6, 0              \n"
        "l32i    a1,  a6, 64             \n"
        "l32i    a2,  a6, 8              \n"
        "l32i    a3,  a6, 12             \n"
        "l32i    a4,  a6, 16             \n"
        "l32i    a5,  a6, 20             \n"
        "l32i    a6,  a6, 24             \n"
        "l32i    a7,  a6, 28             \n"
        "l32i    a8,  a6, 32             \n"
        "l32i    a9,  a6, 36             \n"
        "l32i    a10, a6, 40             \n"
        "l32i    a11, a6, 44             \n"
        "l32i    a12, a6, 48             \n"
        "l32i    a13, a6, 52             \n"
        "l32i    a14, a6, 56             \n"
        "l32i    a15, a6, 60             \n"
        : : "r" (addr) : "a6", "a7"
    ); /* Only a6 and a7 are scratch: the rest is the new thread's context */
}

/* Nothing else is needed to start a thread: load_context() checks start. */
#define THREAD_STARTUP_INIT(core, thread, function) \
    ({ (thread)->context.start = (uint32_t)(uintptr_t)(function); })
