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
 * A thread's first run is handled inside load_context: the window is reset, sp
 * is set to the top of the thread's stack, and control is handed to
 * xt_thread_start(), which runs the thread and calls thread_exit() if it ever
 * returns. GCC has no naked attribute on Xtensa, so there is no start_thread().
 *
 * Everything else is the sp-only model described in asm/xtensa/thread.h: spill,
 * save sp/pc/ps, and let the retw chain refill the registers from the resumed
 * thread's own stack.
 */

/* The context address has to be re-read after the first-run path moves
 * WINDOWBASE: every a-register changes name when the window rotates, so the
 * argument register is no longer the one holding the pointer. A global is the
 * cheapest window-independent way to get it back.
 *
 * It cannot be reached with a plain `l32r sym`: the context lives in .bss in the
 * internal DRAM segment while the code is in the flash window, which is far
 * outside l32r's PC-relative range. So the *address* is emitted as a literal in
 * the same section as the code, and the value is loaded from it.
 *
 * Plain .bss is right: under ESP-IDF .bss is placed in internal DRAM, which is
 * what the old .srambss section in Rockbox's own linker script was for. That
 * script is gone, and a section name nothing declares is not a request -- the
 * linker would place it wherever its default rules happen to put it.
 */
static volatile uint32_t xt_context_addr;

void xt_thread_start(void (*fn)(void))
{
    fn();
    thread_exit();
}

/* FreeRTOS's own cooperative yield on this core calls this spelling of the
 * spill (portasm.S, vPortYield). Declared here rather than pulled from an IDF
 * header: this file is also read by translation units that do not see IDF.
 *
 * It is not the libgcc one, and that distinction is load-bearing.
 * __xtensa_libgcc_window_spill() is a chain of ENTRYs that works by
 * *deliberately taking* window overflow exceptions. A window exception cannot be
 * taken while PS.INTLEVEL is non-zero, and switch_thread() masks interrupts by
 * raising it to 15, so that function faults on its own mechanism: a double
 * exception with PC inside the ENTRY chain, PS showing INTLEVEL 0xF and EXCM
 * set, and a backtrace whose return address is really a data address.
 * xthal_window_spill_nw() walks the base save areas with plain stores and never
 * needs an exception. */
extern void xthal_window_spill_nw(void);

#define XT_PS_INTLEVEL_MASK 0x0000000Fu
#define XT_PS_EXCM_MASK     0x00000010u
#define XT_PS_WOE_MASK      0x00040000u
#define XT_PS_CALLINC_MASK  0x00030000u
#define XT_PS_CALLINC_8     0x00020000u
#define XT_EXCM_LEVEL       1u

/*---------------------------------------------------------------------------
 * Store non-volatile context.
 *---------------------------------------------------------------------------
 */
static inline void store_context(void* addr)
{
    /* Spill first, with WOE off so no window exception can be attempted while
     * the walk runs, and at EXCM_LEVEL rather than wherever the caller left it.
     * This is FreeRTOS's sequence verbatim. After it, every window in the call
     * chain has its a0/a2/a3/a4 in the base save area of its ancestor's frame,
     * which is on this thread's stack and stays there while the thread sleeps. */
    unsigned int ps, saved, pc;

    /* a0 has to be read before the spill call, not after it: callx0 overwrites
     * a0 with its own return address, and that address is not window-encoded,
     * so saving it afterwards hands the resume path a plain address that retw
     * then jumps to as if it were a frame return -- which lands in the middle of
     * switch_thread. */
    asm volatile ("mov    %0, a0" : "=r" (pc));

    asm volatile ("rsr    %0, ps" : "=r" (saved));
    ps = (saved & ~(XT_PS_INTLEVEL_MASK | XT_PS_WOE_MASK)) | XT_EXCM_LEVEL;
    asm volatile ("wsr    %0, ps" : : "r" (ps));
    asm volatile ("rsync");

    /* It has to be called with call0, not as a C function. xthal_window_spill_nw
     * is a call0-ABI leaf -- its exit is ret.n -- which is why FreeRTOS's
     * portasm.S calls it that way. Going through the windowed ABI instead hands
     * it the encoded a0 that call8 leaves behind, and ret.n does not strip that
     * encoding: the core jumps to the raw value and dies fetching instructions
     * there, which is the whole of the reported fault -- PC 0x82051fdb,
     * EXCVADDR 0x82051fd8, a valid-looking address with bit 31 still on it.
     *
     * The callee walks the windows with rotw and restores WINDOWBASE before it
     * returns, and it uses a2-a13, so everything call-clobbered in the windowed
     * ABI has to be declared. a0 and a1 are untouched: it is a leaf and never
     * moves the stack pointer. */
    asm volatile ("j 2f                       \n"
                  ".align 4                   \n"
                  "1: .long xthal_window_spill_nw \n"
                  "2:                         \n"
                  "l32r   a2, 1b              \n"
                  "callx0 a2                  \n"
                  :
                  :
                  : "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9",
                    "a10", "a11", "a12", "a13", "a14", "a15", "memory");

    asm volatile ("wsr    %0, ps" : : "r" (saved));
    asm volatile ("rsync");

    asm volatile(
        "mov     a6, %0              \n"
        /* pc is a0 as the windowed ABI holds it, encoding bit and all. It is
         * only ever used as the target of the retw that leaves switch_thread's
         * frame on the way back in, and RETW needs that encoding intact. */
        "s32i    %1, a6, 4           \n"
        "s32i    a1, a6, 0           \n"
        /* PS is saved with INTLEVEL and EXCM cleared. switch_thread() is
         * reached from paths that have masked interrupts by raising PS.INTLEVEL
         * to 15, and a context that carries a level would leave the resumed
         * thread with the tick and every device interrupt masked. PS.EXCM also
         * makes RETW illegal, so a context saved with it set faults on the first
         * retw after being restored.
         *
         * CALLINC is forced to 8. RETW only strips the window encoding from a0
         * when PS.CALLINC is non-zero; with 0 it behaves as a plain return and
         * jumps to the raw saved address, which still has bit 31 set. That
         * fault is an InstrFetchProhibited at the 4-byte-aligned form of the
         * address -- PC 0x82052047, EXCVADDR 0x82052044, the instruction after
         * the call8 to switch_thread in sleep(). */
        "rsr     a9, ps              \n"
        "movi    a8, -32             \n"   /* clear INTLEVEL and EXCM */
        "and     a9, a9, a8          \n"
        "movi    a8, 3               \n"
        "slli    a8, a8, 16          \n"
        "movi    a7, -1              \n"
        "xor     a7, a7, a8          \n"   /* ~CALLINC field */
        "and     a9, a9, a7          \n"
        "or      a9, a9, %2          \n"   /* CALLINC 8 */
        "s32i    a9, a6, 8           \n"
        "rsync                       \n"
        : : "r" (addr), "r" (pc), "r" (XT_PS_CALLINC_8) : "a6", "a7", "a8", "a9"
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
        "l32i    a7, a6, 12         \n"
        "beqz    a7, 1f             \n"

        /* First run. There is no frame to unwind into, so the thread is entered
         * rather than resumed.
         *
         * WINDOWBASE and WINDOWSTART are left exactly as they are. They are a
         * per-core rotating counter, and windowed code is position-independent
         * with respect to them: the retw chain on this thread's stack works
         * because the frames relate to each other, not because they sit at some
         * absolute window number. Pinning WINDOWBASE to 0 makes the very first
         * thread's frames agree with nothing, and every later resume of a thread
         * that was saved at a different window base returns through a window the
         * arithmetic does not line up with -- which is what it looked like here:
         * retw landing on the raw encoded a0 instead of the decoded address.
         * FreeRTOS never writes these either.
         *
         * What does have to be set is PS: WOE on, INTLEVEL and EXCM off, and
         * CALLINC 8, because the starter is windowed C and windowed C entered
         * with CALLINC 0 reserves no base save area -- every later overflow
         * handler then reads garbage as the ancestor stack pointer. CALLINC 0 is
         * what the call0 form of this file wanted; under ESP-IDF the ABI is
         * windowed and 8 is the only legal entry. */
        "rsr     a7, ps             \n"
        "movi    a8, -32            \n"   /* clear INTLEVEL and EXCM */
        "and     a7, a7, a8         \n"
        "movi    a8, 3              \n"
        "slli    a8, a8, 16         \n"
        "movi    a9, -1             \n"
        "xor     a9, a9, a8         \n"
        "and     a7, a7, a9         \n"
        "movi    a9, 2              \n"
        "slli    a9, a9, 16         \n"
        "or      a7, a7, a9         \n"
        "movi    a8, 4                \n"
        "slli    a8, a8, 16             \n"
        "or      a7, a7, a8             \n"   /* PS.WOE */
        "wsr     a7, ps             \n"
        "movi    a7, 0              \n"
        "wsr     a7, sar            \n"
        "wsr     a7, lcount         \n"
        "rsync                      \n"
        "l32r    a6, 10b            \n"
        "l32i    a6, a6, 0          \n"
        "l32i    a1, a6, 0          \n"   /* top of the thread's stack */
        /* The starter is entered with a real windowed call, so its argument is
         * the caller's a10. Clearing the entry point makes a later
         * load_context() resume the thread instead of restarting it, as the MIPS
         * and SPARC implementations do. */
        "l32i    a10, a6, 12        \n"   /* fn, in the caller's a10 */
        "movi    a7, 0              \n"
        "s32i    a7, a6, 12         \n"
        "l32r    a7, 11b            \n"
        /* callx8, not jx. A jump leaves a0 holding whatever the outgoing thread
         * had in it -- here the 0xdeadbeef stack canary -- and the starter's
         * ENTRY then takes a window overflow whose handler walks up from that
         * value as if it were a frame: the fault was a store at 0xdeadbecf, i.e.
         * 0xdeadbeef - 0x20, which is addix8 a9, a?, -0x20 in the underflow
         * handler. A real windowed call gives the starter a genuine frame with a
         * genuine return address, and CALLINC 8 comes from the call itself.
         *
         * The argument moves with the window: a callx8 callee sees its a2/a3 as
         * the caller's a10/a11, so fn goes in a10. */
        "callx8  a7                 \n"
        /* xt_thread_start runs the thread and calls thread_exit(), which never
         * returns. If it somehow does, falling into the resume path below would
         * be worse than stopping here. */
        "2:                         \n"
        "break   1, 2               \n"

    "1:                             \n"
        /* Resume. Nothing is restored except the stack pointer, the encoded a0
         * and PS -- which is exactly what FreeRTOS's _frxt_dispatch does for a
         * solicited frame, ending in retw. The registers come back on their own:
         * each retw on the way out rotates the window, and the underflow handler
         * refills a0/a2/a3/a4 from the base save area of the frame being left,
         * which the spill filled and the sleeping thread's stack has held ever
         * since. WINDOWBASE and WINDOWSTART are deliberately not touched; they
         * are that handler's input, and a synthetic value makes it refill from
         * areas that were never written.
         *
         * The retw itself is switch_thread's epilogue, which is why nothing may
         * follow this asm in the function. */
        "l32i    a1, a6, 0          \n"   /* sp  */
        "l32i    a0, a6, 4          \n"   /* pc, still window-encoded */
        "l32i    a7, a6, 8          \n"
        "wsr     a7, ps             \n"
        "rsync                      \n"
        : : "r" (addr) : "a0", "a6", "a7", "a8", "a9", "a10"
    );
}

/* Nothing else is needed to start a thread: load_context() checks start, and
 * create_thread() already put the top of the stack in context.sp. */
#define THREAD_STARTUP_INIT(core, thread, function) \
    ({ (thread)->context.start = (uint32_t)(uintptr_t)(function); })
