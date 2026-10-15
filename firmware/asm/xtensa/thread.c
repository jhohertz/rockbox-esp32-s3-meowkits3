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
/* The address has to be re-read after the window rotation, so it cannot live
 * in a register. Plain .bss is right: under ESP-IDF .bss is placed in the
 * internal DRAM segment, which is what the old .srambss section in Rockbox's
 * own linker script was for. That script is gone, and a section name nothing
 * declares is not a request - the linker would place it wherever its default
 * rules happen to put it. */
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
/* FreeRTOS's own cooperative yield on this core calls this spelling of the
 * spill (portasm.S, vPortYield). Declared here rather than pulled from an IDF
 * header: this file is also read by translation units that do not see IDF. */
extern void xthal_window_spill_nw(void);

#define XT_PS_INTLEVEL_MASK 0x0000000Fu
#define XT_PS_WOE_MASK      0x00040000u
#define XT_EXCM_LEVEL       1u

static inline void store_context(void* addr)
{
    /* The spill has to be the _nw variant, and it has to be done with WOE off.
     *
     * __xtensa_libgcc_window_spill() -- what the bare-metal branch used -- is a
     * chain of ENTRYs that works by *taking* window overflow exceptions. A
     * window exception cannot be taken while PS.INTLEVEL is non-zero, and
     * switch_thread() has already masked interrupts by raising it to 15. The
     * result is a double exception reported with PC inside that ENTRY chain, PS
     * showing INTLEVEL 0xF and EXCM set, and a backtrace whose return address is
     * really a data address.
     *
     * xthal_window_spill_nw() walks the base-save areas with plain stores and
     * never needs an exception; clearing WOE makes sure none is attempted while
     * it runs. This is the sequence FreeRTOS uses, including raising the level to
     * EXCM_LEVEL across it rather than leaving it wherever the caller left it. */
    unsigned int ps, saved;

    asm volatile ("rsr    %0, ps" : "=r" (saved));
    ps = (saved & ~(XT_PS_INTLEVEL_MASK | XT_PS_WOE_MASK)) | XT_EXCM_LEVEL;
    asm volatile ("wsr    %0, ps" : : "r" (ps));
    asm volatile ("rsync");

    xthal_window_spill_nw();

    asm volatile ("wsr    %0, ps" : : "r" (saved));
    asm volatile ("rsync");

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
        /* A thread context must never carry an interrupt level or the exception
         * flag: switch_thread() is reached from paths that have disabled
         * interrupts, and restoring a non-zero INTLEVEL would leave the resumed
         * thread with the tick and every device interrupt masked. More
         * importantly, PS.EXCM (bit 4) makes RETW illegal, so a context saved
         * with EXCM set faults on the first retw after being restored.
         * PS.INTLEVEL is bits 0-3, PS.EXCM is bit 4. */
        "movi    a8, -32             \n"
        "rsr     a9, ps              \n"
        "and     a9, a9, a8          \n"
        "s32i    a9, a6, 68          \n"
        "rsr     a7, sar             \n"
        "s32i    a7, a6, 80          \n"
        "rsr     a7, lbeg            \n"
        "s32i    a7, a6, 84          \n"
        "rsr     a7, lend            \n"
        "s32i    a7, a6, 88          \n"
        "rsr     a7, lcount          \n"
        "s32i    a7, a6, 92          \n"
        "rsync                       \n"
        : : "r" (addr) : "a6", "a7", "a8", "a9"
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
        "l32i    a7, a6, 96         \n"
        "beqz    a7, 1f             \n"

        /* First run: a clean window, then enter the starter. PS is not zeroed
         * wholesale: WOE has to stay set, otherwise the window overflow vectors
         * cannot fire for the thread's own nested calls and ENTRY faults.
         *
         * CALLINC has to be 8, not 0. The starter is windowed C, so its ENTRY
         * rotates the window by CALLINC and reserves the base-save area that
         * every later overflow handler walks. Entering windowed code with
         * CALLINC 0 leaves that area unreserved, and the overflow handler then
         * reads garbage as the ancestor stack pointer and stores through it --
         * which is what this looked like: a double exception whose reported
         * frame had a data address as a return address. The bare-metal form of
         * this file ran under the call0 ABI, where 0 was right; under IDF the
         * ABI is windowed and 8 is the only legal entry. */
        /* Window overflow/underflow is enabled by PS.WOE. Moving WINDOWBASE
         * while WOE is set and WINDOWSTART still marks the interruptee's
         * ancestor windows as live makes the hardware walk their base-save
         * areas - which for a thread that has never run do not exist - and the
         * overflow handler stores through garbage. Clear WOE first, move the
         * window, then re-enable it for the thread's own nested calls. */
        "movi    a7, 0              \n"
        "wsr     a7, ps             \n"
        "rsync                      \n"
        "wsr     a7, windowstart    \n"
        "wsr     a7, windowbase     \n"
        "movi    a7, 0x40002000     \n"   /* PS.WOE | CALLINC 8 */
        "wsr     a7, ps             \n"
        "movi    a7, 0              \n"
        "wsr     a7, sar            \n"
        "wsr     a7, lcount         \n"
        "rsync                      \n"
        "l32r    a6, 10b            \n"
        "l32i    a6, a6, 0          \n"
        "l32i    a1, a6, 64         \n"
        /* The starter is entered by jump, so its argument is a2: the entry
         * point has to be moved there before it is cleared. Clearing it makes a
         * later load_context() resume the thread instead of restarting it, the
         * same way the MIPS and SPARC implementations do. */
        "l32i    a2, a6, 96         \n"
        "movi    a7, 0              \n"
        "s32i    a7, a6, 96         \n"
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
        /* PS.CALLINC has to be 8 on the way out, whatever it was on the way in.
         * switch_thread() ends with the compiler's retw, and RETW only strips
         * the window encoding from a0 when PS.CALLINC is non-zero: with 0 it
         * behaves as a plain return and jumps to the raw saved address, which on
         * the windowed ABI still has bit 31 set. The fault this produces is an
         * InstrFetchProhibited at the aligned form of that address -- PC
         * 0x82052047, EXCVADDR 0x82052044, i.e. the instruction after the call8
         * to switch_thread in sleep(), with the encoding bit still on it. */
        "l32i    a7, a6, 68              \n"
        "movi    a9, 3                   \n"
        "slli    a9, a9, 16              \n"
        "movi    a8, -1                  \n"
        "xor     a8, a8, a9              \n"   /* ~CALLINC field */
        "and     a7, a7, a8              \n"
        "movi    a9, 2                   \n"
        "slli    a9, a9, 16              \n"   /* CALLINC 8 */
        "or      a7, a7, a9              \n"
        "wsr     a7, ps                  \n"
        "l32i    a7, a6, 80              \n"
        "wsr     a7, sar                 \n"
        "l32i    a7, a6, 84              \n"
        "wsr     a7, lbeg                \n"
        "l32i    a7, a6, 88              \n"
        "wsr     a7, lend                \n"
        "l32i    a7, a6, 92              \n"
        "wsr     a7, lcount              \n"
        "rsync                           \n"
        /* a6 is both the base of every load and one of the registers being
         * loaded, so its value has to be parked before the rest are read and
         * installed last. Loading it in place makes the following stores use
         * the thread's saved a6 as the context pointer. */
        "l32i    a7,  a6, 24             \n"
        "l32i    a0,  a6, 0              \n"
        "l32i    a1,  a6, 64             \n"
        "l32i    a2,  a6, 8              \n"
        "l32i    a3,  a6, 12             \n"
        "l32i    a4,  a6, 16             \n"
        "l32i    a5,  a6, 20             \n"
        "l32i    a8,  a6, 32             \n"
        "l32i    a9,  a6, 36             \n"
        "l32i    a10, a6, 40             \n"
        "l32i    a11, a6, 44             \n"
        "l32i    a12, a6, 48             \n"
        "l32i    a13, a6, 52             \n"
        "l32i    a14, a6, 56             \n"
        "l32i    a15, a6, 60             \n"
        "mov     a6, a7                  \n"
        : : "r" (addr) : "a6", "a7", "a8", "a9"
    ); /* a6-a9 are scratch; everything else is the new thread's context */
}

/* Nothing else is needed to start a thread: load_context() checks start. */
#define THREAD_STARTUP_INIT(core, thread, function) \
    ({ (thread)->context.start = (uint32_t)(uintptr_t)(function); })
