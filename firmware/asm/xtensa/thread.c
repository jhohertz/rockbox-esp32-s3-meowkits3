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
#include <stddef.h>
#include <xtensa_context.h>

/* The save area in asm/xtensa/thread.h is a bare word array, because that
 * header is also read by the freestanding domain. These tie it to the real
 * structure IDF uses, so the two cannot drift apart silently. */
_Static_assert(sizeof(XtExcFrame) <= sizeof(uint32_t) * XT_FRAME_WORDS,
               "struct regs.frame is smaller than XtExcFrame");
_Static_assert(offsetof(XtExcFrame, exit) == XT_F_EXIT * 4, "XT_F_EXIT");
_Static_assert(offsetof(XtExcFrame, pc)   == XT_F_PC   * 4, "XT_F_PC");
_Static_assert(offsetof(XtExcFrame, ps)   == XT_F_PS   * 4, "XT_F_PS");
_Static_assert(offsetof(XtExcFrame, a0)   == XT_F_A0   * 4, "XT_F_A0");
_Static_assert(offsetof(XtExcFrame, a1)   == XT_F_A1   * 4, "XT_F_A1");
_Static_assert(offsetof(XtExcFrame, a2)   == XT_F_A2   * 4, "XT_F_A2");
_Static_assert(offsetof(XtExcFrame, a6)   == XT_F_A6   * 4, "XT_F_A6");
_Static_assert(offsetof(XtExcFrame, a12)  == 60, "XT_STK_A12 byte offset");
_Static_assert(offsetof(XtExcFrame, a13)  == 64, "XT_STK_A13 byte offset");
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
 *
 * IDF's own model, not a hand-rolled one: build the same XtExcFrame that
 * _xt_handle_exc builds on an exception, and let _xt_context_save fill the
 * register half of it. The resume side is then _xt_context_restore followed by
 * _xt_user_exit, which is exactly the pair FreeRTOS runs through
 * _frxt_dispatch's stk path on every interrupt-driven task switch.
 */
static inline void store_context(void* addr)
{
    struct regs* ctx = (struct regs*)addr;
    uint32_t* f = ctx->frame;
    unsigned int saved, pc;

    /* a0 is read before anything else runs. It is both the address the thread
       resumes at (the instruction after the call into the blocking primitive)
       and, at that address, the value a0 has to hold -- the call has already
       returned by then, so one word serves both slots of the frame. */
    asm volatile ("mov    %0, a0" : "=r" (pc));
    asm volatile ("rsr    %0, ps" : "=r" (saved));
    asm volatile ("mov    %0, a1" : "=r" (ctx->sp));

    f[XT_F_PC]   = pc;
    f[XT_F_A0]   = pc;
    f[XT_F_A1]   = ctx->sp;
    f[XT_F_EXIT] = (uint32_t)(uintptr_t)&_xt_user_exit;

    /* rfe requires PS.EXCM set at the moment it executes, and _xt_user_exit
       writes this value to PS immediately before doing that. UM is not
       cosmetic: ESP-IDF implements _KernelExceptionVector as `break 1, 0`, an
       explicit unhandled-exception stub, so a thread without UM dies on its
       first level-1 interrupt. CALLINC 8 is the window the resume point lives
       in -- switch_thread is entered with call8, and the resume point is the
       instruction right after that call, still in the callee's window. */
    f[XT_F_PS] = (saved & ~(XT_PS_INTLEVEL_MASK | XT_PS_CALLINC_MASK))
                 | XT_PS_UM | XT_PS_EXCM_MASK | XT_PS_WOE_MASK
                 | XT_PS_CALLINC_8;

    /* The register half is filled by IDF's routine rather than by hand. It
       addresses the frame through sp, so sp is moved to the frame for the
       duration: the frame lives in the thread table rather than on a stack, and
       the routine only ever writes inside it. It is entered with call0 because
       that is how IDF's own vectors enter it (_xt_handle_exc, _xt_syscall_exc),
       and it requires its caller to have placed a12 and a13 in the frame
       first -- byte offsets 60 and 64 are XT_STK_A12 and XT_STK_A13.
       sp is then reloaded from the frame's own A1 slot, which is the same
       value switch_thread needs back. */
    xt_context_addr = (uint32_t)(uintptr_t)f;

    asm volatile ("j 2f                       \n"
                  ".align 4                   \n"
                  "10: .long xt_context_addr  \n"
                  "11: .long _xt_context_save \n"
              "2:                             \n"
                  "l32r    a6, 10b            \n"
                  "l32i    a6, a6, 0          \n"
                  "mov     a1, a6             \n"   /* sp = the frame */
                  "s32i    a12, a1, 60        \n"   /* XT_STK_A12 */
                  "s32i    a13, a1, 64        \n"   /* XT_STK_A13 */
                  "l32r    a6, 11b            \n"
                  "callx0  a6                 \n"
                  "l32i    a1, a1, 16         \n"   /* XT_STK_A1: our sp back */
                  :
                  :
                  : "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9",
                    "a10", "a11", "a12", "a13", "memory");
    /* a12 and a13 have to be in that list even though the frame receives their
       values: _xt_context_save copies them to XT_STK_TMP0/TMP1 and then uses
       the registers themselves as scratch. Under the windowed ABI a12-a15 are
       callee-saved, so without the declaration GCC is told nothing about a
       caller-saved-looking destruction of callee-saved state, and whatever it
       had parked there is silently lost. Omitting them produced a
       LoadProhibited at EXCVADDR 0x28 in check_tmo_expired, i.e. a NULL
       core_entry, several switches later and nowhere near the cause. */
}

/*---------------------------------------------------------------------------
 * A thread that has never run: lay the frame so an exception return enters it.
 *---------------------------------------------------------------------------
 *
 * There is no separate start path any more. A first run and a resume are the
 * same two instructions -- _xt_context_restore then _xt_user_exit -- and the
 * only difference is what is in the frame. That is how IDF enters a task that
 * has never run: _frxt_dispatch's .L_frxt_dispatch_stk branch.
 */
static inline void xt_init_frame(struct regs* ctx)
{
    uint32_t* f = ctx->frame;

    memset(f, 0, sizeof(uint32_t) * XT_FRAME_WORDS);

    f[XT_F_EXIT] = (uint32_t)(uintptr_t)&_xt_user_exit;
    f[XT_F_PC]   = (uint32_t)(uintptr_t)&xt_thread_start;
    f[XT_F_A1]   = ctx->sp;         /* the stack create_thread handed us */
    f[XT_F_A0]   = 0;               /* xt_thread_start never returns */
    /* CALLINC 4, not 8: xt_thread_start's entry rotates a single window, which
       is what puts the argument in its a2, and the caller's a6 is the frame's
       a6. The same convention FreeRTOS uses for a task's first frame. */
    f[XT_F_PS]   = XT_PS_UM | XT_PS_EXCM_MASK | XT_PS_WOE_MASK
                   | XT_PS_CALLINC_4;
    f[XT_F_A6]   = ctx->start;
    ctx->start   = 0;
}

/*---------------------------------------------------------------------------
 * Load non-volatile context, or start the thread if it has not run yet.
 *---------------------------------------------------------------------------
 */
static inline void load_context(const void* addr)
{
    struct regs* ctx = (struct regs*)(uintptr_t)addr;

    if (ctx->start != 0)
        xt_init_frame(ctx);

    xt_context_addr = (uint32_t)(uintptr_t)ctx->frame;

    /* sp is pointed at the frame, which is what both IDF routines take as
       their argument, and _xt_user_exit takes its fields from. It restores PS,
       PC and A0 from the frame, sets sp from the frame's A1, and executes rfe.
       rsil supplies PS.EXCM, without which rfe is not a legal instruction; it
       is a user-mode instruction, so this is reachable from an ordinary task,
       which is how vPortYield reaches the same path.
       Nothing follows: control leaves through rfe and never comes back. */
    asm volatile ("j 3f                          \n"
                  ".align 4                      \n"
                  "10: .long xt_context_addr     \n"
                  "11: .long _xt_context_restore \n"
              "3:                                \n"
                  "l32r    a6, 10b               \n"
                  "l32i    a6, a6, 0             \n"
                  "mov     a1, a6                \n"   /* sp = the frame */
                  "rsil    a7, %0                \n"   /* PS.EXCM set */
                  "l32r    a7, 11b               \n"
                  "callx0  a7                    \n"   /* _xt_context_restore */
                  "l32i    a0, a1, 0             \n"   /* XT_STK_EXIT */
                  "jx      a0                    \n"   /* -> _xt_user_exit */
                  :
                  : "i" (XT_EXCM_LEVEL)
                  : "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
                    "a8", "a9", "a10", "a11", "memory");
}

/* Nothing else is needed to start a thread: load_context() checks start, and
 * create_thread() already put the top of the stack in context.sp. */
#define THREAD_STARTUP_INIT(core, thread, function) \
    ({ (thread)->context.start = (uint32_t)(uintptr_t)(function); })
