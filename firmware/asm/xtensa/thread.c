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

#ifdef HAVE_SYSTEM_LIBC
#include <string.h>
#include <xtensa_context.h>
#endif

/*
 * A thread's first run is handled inside load_context: xt_init_frame() lays an
 * exception frame at the top of the thread's stack and load_context() leaves it
 * through ESP-IDF's own exit dispatcher, which is how a thread gets a window
 * chain that belongs to it alone. The frame's pc is xt_thread_start(), which
 * runs the thread and calls thread_exit() if it ever returns. GCC has no naked
 * attribute on Xtensa, so there is no start_thread().
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

/* ESP-IDF's own exception plumbing, reused for a thread's first run. Both are
 * already in the image: the vectors use them for every real interrupt. */
extern void _xt_context_restore(void);
extern void _xt_user_exit(void);

#define XT_PS_INTLEVEL_MASK 0x0000000Fu
#define XT_PS_EXCM_MASK     0x00000010u
#define XT_PS_WOE_MASK      0x00040000u
#define XT_PS_CALLINC_MASK  0x00030000u
#define XT_PS_CALLINC_8     0x00020000u
#define XT_PS_CALLINC_4     0x00010000u
#define XT_PS_UM            0x00000020u
/* XCHAL_EXCM_LEVEL for this core. store_context raises to it before the
 * spill; load_context raises to it with rsil so that rfe is legal. */
#define XT_EXCM_LEVEL       3u

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

    /* Narrow WINDOWSTART to the current window alone, and record that window's
     * index as part of the thread's context.
     *
     * This is the step that was missing, and it is what the measured failure
     * came from. xthal_window_spill_nw restores WINDOWSTART to whatever it
     * found, and every thread start rotates WINDOWBASE by 1 (the starter's
     * ENTRY consumes the frame's CALLINC) without retiring the windows the
     * previous thread left behind. WINDOWSTART therefore accumulates bits that
     * belong to no live frame -- measured as 0xaa02 with WINDOWBASE 1, i.e.
     * windows 1, 11, 13, 15 and 17, a call8 chain with a hole where windows
     * 3..9 used to be. The next spill walks those bits, treats whatever is at
     * ancestor_sp-16 as a base save area, and stores through the "ancestor"
     * pointer it finds there. Dumping that chain showed those pointers to be
     * ASCII: "Silent M", "urder.mp3", "System of a Down", ".mp3", "The Killer"
     * -- dircache directory-entry names on the thread's own stack. The store
     * lands in the middle of that data, and the eventual fault (an MMU entry
     * fault from the cache, or an instruction fetch at a data address,
     * depending on which word the overwrite hit) is nowhere near the cause.
     *
     * After the spill no window above the base holds anything the ABI keeps in
     * registers: a0/a2/a3/a4 are in the base save areas and a12-a15 in their
     * frame slots. So marking those windows free is not a suppression, it is
     * the true state -- and it is what makes a later retw take the underflow
     * handler and refill from the areas the spill just wrote, which is the
     * whole resume model. The first-run path already narrows the same way.
     *
     * WINDOWBASE still has to be saved, because it is where the chain is rooted
     * and it is core state: a thread that blocked at base 17 cannot be resumed
     * with the core at base 1, and the two thread starts that produced the hole
     * above each moved the base by one. */
    asm volatile ("rsr    a2, windowbase        \n"
                  "s32i   a2, %0, 16            \n"
                  "movi   a3, 1                 \n"
                  "ssl    a2                    \n"
                  "sll    a3, a3                \n"
                  "wsr    a3, windowstart       \n"
                  "rsync                        \n"
                  :
                  : "r" (addr)
                  : "a2", "a3", "memory");

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
/*---------------------------------------------------------------------------
 * A thread that has never run: build the frame the ABI expects.
 *---------------------------------------------------------------------------
 *
 * Such a thread cannot be entered from here by jump or by call. Either leaves
 * the *outgoing* thread's frames above it in the window chain, so the spill and
 * the window handlers walk past its root frame and do their stores through the
 * other thread's stack pointer. That is not a detail that can be patched: the
 * chain above a call frame is the caller, and the caller belongs to somebody
 * else.
 *
 * The ABI does have a self-rooted shape, and it is the one an exception entry
 * builds. ESP-IDF's vectors already know how to fill it in and how to leave it,
 * so this is the same two-step FreeRTOS uses on its first-run path,
 * _frxt_dispatch's .L_frxt_dispatch_stk:
 *
 *     call0 _xt_context_restore    SAR, LBEG/LEND/LCOUNT and a2-a15 come
 *                                  from the frame at sp
 *     l32i  a0, sp, XT_STK_EXIT
 *     ret                          the exit dispatcher restores a0, a1, PC and
 *                                  PS from the frame and executes rfe
 *
 * rfe needs PS.EXCM set. rsil to XCHAL_EXCM_LEVEL supplies it and is a user-mode
 * instruction, so it is legal from an ordinary task -- which is exactly how
 * vPortYield reaches the same path.
 *
 * The frame is XtExcFrame from xtensa_context.h, the layout IDF's own vectors
 * use, and XT_STK_FRMSZ already includes the interruptee's base save area and
 * the spill scratch space, rounded to 16.
 */
static inline int xt_init_frame(struct regs* ctx)
{
    XtExcFrame *f;
    uint32_t top = ctx->sp;
    uint32_t fn  = ctx->start;

    if (fn == 0)
        return 0;                   /* already running: this is a resume */
    ctx->start = 0;

    f = (XtExcFrame *)((top - XT_STK_FRMSZ) & ~0xFu);
    memset(f, 0, sizeof *f);

    f->exit = (uint32_t)(uintptr_t)&_xt_user_exit;
    f->pc   = (uint32_t)(uintptr_t)&xt_thread_start;
    /* PS_UM, and that is not a style choice. ESP-IDF implements
       _KernelExceptionVector as `break 1, 0` -- an explicit "unhandled kernel
       exception" stub -- because nothing of its own ever runs at ring 0 after
       startup. A thread started without UM therefore dies on its first level-1
       interrupt, which is what this build was showing: PC in _xt_kernel_exc,
       "BREAK instr", and a perfectly intact backtrace underneath it. With UM
       set the tick and every device interrupt go to the user vector, which IDF
       does implement. vPortYield writes PS with wsr from a task, so the resume
       path stays legal in user mode too.

       CALLINC 1 makes the starter's ENTRY rotate a single window, which is what
       puts the argument in the callee's a2. rfe clears EXCM whatever the frame
       says. This is the same value FreeRTOS gives a task's first frame. */
    f->ps   = XT_PS_UM | XT_PS_EXCM_MASK | XT_PS_WOE_MASK | XT_PS_CALLINC_4;
    f->a0   = 0;                    /* xt_thread_start never returns */
    f->a1   = top;                  /* the frame is deallocated by the exit */
    /* The starter is entered as if it had been call4'd, so its a2 is the
     * caller's a6. This is the same convention FreeRTOS uses for a task's first
     * frame; getting it wrong hands the thread function a garbage pointer. */
    f->a6   = fn;

    ctx->sp = (uint32_t)(uintptr_t)f;
    return 1;
}

/*---------------------------------------------------------------------------
 * Load non-volatile context, or start the thread if it has not run yet.
 *---------------------------------------------------------------------------
 */
static inline void load_context(const void* addr)
{
    int first = xt_init_frame((struct regs *)(uintptr_t)addr);
    xt_context_addr = (uint32_t)(uintptr_t)addr;

    asm volatile(
        "j 3f                       \n"
        ".align 4                   \n"
        "10: .long xt_context_addr  \n"
        "11: .long _xt_context_restore \n"
    "3:                             \n"
        "l32r    a6, 10b            \n"
        "l32i    a6, a6, 0          \n"
        "mov     a7, %0             \n"
        "beqz    a7, 1f             \n"

        /* First run. sp already points at the frame xt_init_frame built. */
        "l32i    a1, a6, 0          \n"
        /* Narrow WINDOWSTART to the current window, which is what the real
         * entry vectors do once they have spilled. It is load-bearing twice: it
         * stops the spill and the window handlers from walking into the
         * previous thread's windows, and it is the flag that makes a later retw
         * take a window underflow and refill from the base save areas the spill
         * wrote. */
        "rsr     a7, windowbase     \n"
        "movi    a8, 1              \n"
        "ssl     a7                 \n"
        "sll     a8, a8             \n"
        "wsr     a8, windowstart    \n"
        "rsil    a7, %1             \n"   /* PS.EXCM set, so rfe is legal */
        "l32r    a7, 11b            \n"
        "callx0  a7                 \n"   /* _xt_context_restore, frame at sp */
        "l32i    a0, a1, 0          \n"   /* XT_STK_EXIT is the frame's first word */
        "jx      a0                 \n"   /* -> _xt_user_exit -> rfe */

    "1:                             \n"
        /* Resume. Nothing is restored except the stack pointer, the encoded a0
         * and PS -- which is exactly what FreeRTOS's _frxt_dispatch does for a
         * solicited frame, ending in retw. The registers come back on their own:
         * each retw on the way out rotates the window, and the underflow handler
         * refills a0/a2/a3/a4 from the base save area of the frame being left,
         * which the spill filled and the sleeping thread's stack has held ever
         * since. WINDOWBASE and WINDOWSTART are deliberately not touched here;
         * they are that handler's input.
         *
         * The retw itself is switch_thread's epilogue, which is why nothing may
         * follow this asm in the function. */
        "l32i    a1, a6, 0          \n"   /* sp  */
        "l32i    a0, a6, 4          \n"   /* pc, still window-encoded */
        "l32i    a7, a6, 8          \n"
        /* Put the window state back with the stack it belongs to. WINDOWBASE is
         * where this thread's base save chain is rooted, and WINDOWSTART is
         * rebuilt from it because store narrowed it to that one bit. Both are
         * written before PS so the retw below rotates from the right origin. */
        "l32i    a8, a6, 16         \n"   /* windowbase */
        "wsr     a8, windowbase     \n"
        "movi    a9, 1              \n"
        "ssl     a8                 \n"
        "sll     a9, a9             \n"
        "wsr     a9, windowstart    \n"
        "wsr     a7, ps             \n"
        "rsync                      \n"
        "retw.n                     \n"
        "2:                         \n"
        "break   1, 2               \n"   /* unreachable */
        :
        : "r" (first), "i" (XT_EXCM_LEVEL)
        : "a0", "a6", "a7", "a8", "a9", "memory"
    );
}

/* Nothing else is needed to start a thread: load_context() checks start, and
 * create_thread() already put the top of the stack in context.sp. */
#define THREAD_STARTUP_INIT(core, thread, function) \
    ({ (thread)->context.start = (uint32_t)(uintptr_t)(function); })
