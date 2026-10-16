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
/* Not sizeof(XtExcFrame): see the comment on XT_FRAME_WORDS. The extra state
 * IDF stores for the FP register file lives beyond the struct. */
_Static_assert(sizeof(uint32_t) * XT_FRAME_WORDS >= XT_STK_FRMSZ,
               "struct regs.frame is smaller than the area IDF writes");
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

/* TEMPORARY: non-rotating breadcrumb slots, see target/xtensa/scan_trace.c */
extern void scan_trace_sticky(int tag, unsigned long a, unsigned long b,
                              unsigned long c);

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

/* Spill the live register windows into the base-save areas of their frames.

   Written as a bare function rather than inline asm: xtensa GCC does not honour
   clobbers of windowed registers, so an inline block that borrows a6/a7 silently
   corrupts whatever the compiler had parked there.

   It is entered windowed (call8 from C) and leaves that way; only the helper is
   entered with callx0, because xthal_window_spill_nw is a call0-ABI leaf and it
   lives in IRAM, roughly 29 MB from flash .text, far outside any direct call's
   range -- hence the address in a literal rather than a call.

   WINDOWSTART is then narrowed to the current window alone. The helper walks
   every window the mask marks as started, using the sp held in that window's
   own registers as the base-save pointer. Rockbox threads are entered through
   rfe onto stacks of their own, so the mask also still carries windows belonging
   to whichever thread ran here last -- and their sp registers are still in the
   file. Spilling those writes into another thread's frames. ESP-IDF hits the
   same hazard once at scheduler start and says so in port.c: "Windows contain
   references to the startup stack which will be reclaimed by the main task."
   Narrowing after the spill means each thread's chain is described only by its
   own base-save areas, which the spill just wrote, and a later retw refills
   from them through the underflow handler. */
extern void rb_spill_asm(void);

void rb_window_spill(void)
{
    unsigned int ws, wb, ps;

    asm volatile ("rsr %0, windowstart" : "=r"(ws));
    asm volatile ("rsr %0, windowbase"  : "=r"(wb));
    asm volatile ("rsr %0, ps"          : "=r"(ps));
    STRACE(86, ws, wb, ps);

    rb_spill_asm();

    asm volatile ("rsr %0, windowstart" : "=r"(ws));
    asm volatile ("rsr %0, windowbase"  : "=r"(wb));
    asm volatile ("rsr %0, ps"          : "=r"(ps));
    STRACE(87, ws, wb, ps);
}

asm (
"       .align 4                                  \n"
"       .globl  rb_spill_asm                      \n"
"       .type   rb_spill_asm, @function           \n"
"rb_spill_asm:                                    \n"
"       entry   a1, 32                            \n"
"       j       1f                                \n"
"       .align 4                                  \n"
"2:     .long   xthal_window_spill_nw             \n"
"3:     .long   ~0x4000f                          \n"  /* clear WOE, INTLEVEL */
"4:     .long   0x00040000                        \n"  /* WOE                 */
"1:     rsr.ps  a10                               \n"
"       l32r    a11, 3b                           \n"
"       and     a10, a10, a11                     \n"
"       addi    a10, a10, 3                       \n"  /* XCHAL_EXCM_LEVEL */
"       wsr.ps  a10                               \n"
"       rsync                                     \n"
"       l32r    a11, 2b                           \n"
"       callx0  a11                               \n"
"       rsr.ps  a10                               \n"
"       l32r    a11, 4b                           \n"
"       or      a10, a10, a11                     \n"
"       wsr.ps  a10                               \n"
"       rsync                                     \n"
"       retw                                      \n"
"       .size   rb_spill_asm, .-rb_spill_asm      \n"
);

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

    /* a0 is read before anything else runs: it is both where the thread
       resumes (the instruction after the call into the blocking primitive)
       and the value a0 has to hold there. The two frame slots take different
       forms of it -- see below. */
    asm volatile ("mov    %0, a0" : "=r" (pc));
    asm volatile ("rsr    %0, ps" : "=r" (saved));
    asm volatile ("mov    %0, a1" : "=r" (ctx->sp));

    /* Spill the register windows into their base-save areas before anything is
       saved. This is not an optimization: it is what makes the resume correct.

       The resume returns through the blocking primitive with retw (yield is
       `entry sp,32; call8 switch_thread; retw`). retw does not merely jump --
       it hands the caller's a0-a3 back, and on a core with no REGB option the
       only place those can come from is the base-save area at sp-16..sp-4 of
       the frame being left. Nothing writes that area unless a spill happens:
       there is no hardware spill on this core config, and GCC's windowed code
       emits manual stores for its own frame only, never for the caller's.
       So without an explicit spill, retw reloads the caller's a0-a3 from
       whatever the intervening threads left on that stack, and the thread
       returns to a garbage address. It then runs wild with interrupts masked,
       which is what the dead core-0 tick looked like from the outside.

       ESP-IDF does exactly this in vPortYield (portasm.S), the solicited-yield
       path that switch_thread is the analogue of. The _nw form is used rather
       than xthal_window_spill() because with WOE clear it does plain stores and
       takes no exceptions -- and __xtensa_libgcc_window_spill cannot be used
       here at all, since it works by *taking* window overflow exceptions, which
       switch_thread's disable_irq() has just masked.

       It is a real function rather than inline asm because xtensa GCC does not
       honour clobbers of windowed registers: an inline block that used a6/a7 as
       scratch left the compiler believing a7 still held the frame pointer, and
       the next C statement loaded from 0x40000 -- the WOE constant -- as a
       pointer. A call goes through the ABI and the conflict disappears. */
    STRACE(91, ctx->sp, ((uint32_t*)(uintptr_t)ctx->sp)[-8],
                ((uint32_t*)(uintptr_t)ctx->sp)[-4]);
    rb_window_spill();

    /* The base-save area of a CALLINC 2 frame is the eight words at sp-32 ..
       sp-4, and it is what retw refills the caller's a8-a11 and a12-a15 from.
       Sampled before and after the spill so the write can be seen to happen --
       or not. */
    STRACE(88, ctx->sp, ((uint32_t*)(uintptr_t)ctx->sp)[-8],
                ((uint32_t*)(uintptr_t)ctx->sp)[-4]);

    /* A solicited yield needs exactly three things: where to return, which
       stack was in use, and PS. That is all ESP-IDF's vPortYield saves for the
       windowed ABI -- a0 to XT_SOL_PC, PS to XT_SOL_PS, and sp recorded in the
       TCB -- and its restore (.L_frxt_dispatch_sol) is `wsr PS` then retw.

       a2-a11 are deliberately *not* saved, and that is not an oversight: they
       are caller-saved, so they are dead across the blocking primitive by ABI.
       Saving and restoring them is what was corrupting the port. With CALLINC 8
       the current window's a8-a15 are not this function's registers at all,
       they belong two windows up, so _xt_context_save/_xt_context_restore were
       overwriting registers the window machinery itself uses: the spill walks
       windows using a9 and a13 as its base-save pointers, and the underflow
       handler takes its ancestor pointer from a9. That is how a mask like
       WINDOWSTART 0xaa80 with WINDOWBASE 0x0f -- a mask no call chain produces
       -- gets built, and how the spill then stores a4-a7 through some other
       thread's stack frame. */
    f[XT_F_PC]   = pc;   /* verbatim, window encoding included. retw strips it
                            using PS.CALLINC from the same saved PS, so there is
                            nothing to decode; decoding was only needed for the
                            rfe/EPC_1 entry, which a resume no longer uses. */
    f[XT_F_A0]   = pc;
    f[XT_F_A1]   = ctx->sp;

    /* PS is restored by a plain `wsr ps`, not by rfe, so whatever is in the
       frame is what the thread runs with -- every bit of it, with none of the
       fixes rfe performs for free. Two of those fixes have to be applied by
       hand here:

       INTLEVEL. switch_thread() is reached through disable_irq(), so the PS
       sampled a few instructions earlier has INTLEVEL 15. Writing it back
       verbatim leaves the resumed thread with interrupts masked forever, which
       is indistinguishable from a dead tick: the alarm fires in hardware and
       the core never vectors to it.

       EXCM. A thread must run with EXCM clear. With it set, any exception the
       thread takes is by definition a second one, so the core goes to
       _DoubleExceptionVector, and IDF implements that (and the kernel and
       hardware vectors) as _xt_panic, which prints nothing and spins. That is
       what the watchdog's Saved PC was pointing at -- _xt_context_save and
       _xt_panic in IDF's xtensa_context.S / panic_handler_asm.S, over and over,
       with no Guru Meditation line ever reaching the console. rfe clears EXCM on
       the first-run path; nothing clears it on this one.

       Everything else is left exactly as the core had it: UM and RING decide
       which exception vector a level-1 interrupt uses (_KernelExceptionVector
       is `break 1, 0` here, so UM must stay set), WOE has to stay set for the
       window underflow that refills the caller's a0-a3 to be taken at all, and
       CALLINC has to stay non-zero because retw uses it to strip the window
       encoding from a0. OWB travels with PS and must not be edited either. */
    f[XT_F_PS]   = saved & ~(XT_PS_EXCM_MASK | XT_PS_INTLEVEL_MASK);
    f[XT_F_EXIT] = 0;     /* 0 flags a solicited frame, as IDF does */
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
    uint32_t* f = ctx->frame;

    /* Unconditional, and only the newest survives: whichever switch is in
       flight when the machine dies is the one worth reading. */
    scan_trace_sticky(4, (uintptr_t)ctx, ctx->start, ctx->sp);
    scan_trace_sticky(5, f[XT_F_PC], f[XT_F_PS], f[XT_F_A1]);

    if (ctx->start != 0)
    {
        uint32_t* fs;

        /* A thread that has never run has no frames to return through, so it is
           entered by way of an exception return: lay the full frame and let
           IDF's own restore and user-exit paths do the rest. */
        xt_init_frame(ctx);

        /* ... but not where xt_init_frame built it. Both halves of IDF's
           never-run path address the frame through sp -- _xt_context_restore
           ends by reloading sp from the frame's own A1 slot, and _xt_user_exit
           then reads PS, PC and A0 back through sp before it removes the frame.
           A frame parked in the thread table with an A1 slot naming the thread's
           stack therefore has its sp replaced halfway through, and the rest is
           read from the top of that stack instead. IDF's invariant, which the
           exception entry gives it for free, is that the frame sits at the top
           of the stack and its A1 slot names the frame itself. So build it in
           the thread table, where xt_init_frame can reach it, and move it down. */
        fs = (uint32_t*)((uintptr_t)ctx->sp - XT_STK_FRMSZ);
        memcpy(fs, ctx->frame, XT_STK_FRMSZ);
        /* FreeRTOS's own uxInitialiseStackFrame sets
         *   frame->a1 = uxStackPointer + XT_STK_FRMSZ
         * "saved stack pointer should point to physical top of stack frame".
         * _xt_user_exit then does `l32i sp, sp, XT_STK_A1` to remove the frame,
         * so the thread wakes with sp just above its own frame rather than at
         * its base. Naming the frame itself leaves 192 bytes stranded above the
         * live stack and disagrees with every other windowed task on this chip. */
        fs[XT_F_A1] = (uint32_t)(uintptr_t)fs + XT_STK_FRMSZ;
        ctx->sp = (uintptr_t)fs;
        xt_context_addr = (uint32_t)(uintptr_t)fs;

        /* Sticky: the ring rotates and the tick floods it, so the one record
           that matters -- what the very first frame of a thread held -- is gone
           before anything can read it. Measured: the crash after masking the
           level-4 cache path was an InstructionLoadProhibited at 0x3fcd14f0, a
           thread-table address, with a0 inside xt_thread_start's call of its
           argument. So xt_thread_start ran and its a2 was a thread pointer, not
           a function. These say what the frame actually carried at entry. */
        scan_trace_sticky(1, (uintptr_t)ctx, (uintptr_t)fs, f[XT_F_PC]);
        scan_trace_sticky(2, f[XT_F_A2], f[XT_F_A6], f[XT_F_A6 + 1]);
        scan_trace_sticky(3, f[XT_F_A0], f[XT_F_PS], f[XT_F_A1]);

        asm volatile ("j 3f                               \n"
                      ".align 4                           \n"
                      "10: .long xt_context_addr          \n"
                      "11: .long _xt_context_restore      \n"
                      "12: .long _xt_user_exit            \n"
                  "3:                                    \n"
                      "l32r    a6, 10b                    \n"
                      "l32i    a6, a6, 0                  \n"
                      "mov     a1, a6                     \n"   /* sp = the frame */
                      "rsil    a7, %0                     \n"   /* PS.EXCM set */
                      "l32r    a7, 11b                    \n"
                      "callx0  a7                         \n"   /* _xt_context_restore */
                      "l32r    a7, 12b                    \n"
                      "jx      a7                         \n"   /* _xt_user_exit -> rfe */
                      :
                      : "i"(XCHAL_EXCM_LEVEL)
                      : "memory");
        /* never returns */
    }

    xt_context_addr = (uint32_t)(uintptr_t)ctx->frame;

    STRACE(89, ctx->sp, ((uint32_t*)(uintptr_t)ctx->sp)[-8],
                ((uint32_t*)(uintptr_t)ctx->sp)[-4]);
    STRACE(90, f[XT_F_PC], f[XT_F_PS], f[XT_F_A1]);

    /* A thread that blocked in switch_thread is resumed the way IDF resumes one
       in _frxt_dispatch's .L_frxt_dispatch_sol branch: point sp at the frame,
       put PS back, load a0, and return through the blocking primitive with
       retw.

       Nothing else is restored, and that is the point. a2-a11 are caller-saved
       and dead by ABI across the blocking primitive, and writing a8-a15 here
       would be writing into windows that are not this call chain's -- which is
       what was feeding the window spill pointers that belonged to other
       threads.

       sp is loaded from the frame's A1 slot rather than set to the frame itself,
       and the distinction is load-bearing. retw refills the caller's a0-a3 from
       the base-save area at a1-16..a1-4, i.e. relative to whatever sp holds at
       that moment, and that area belongs to the blocking primitive's own frame
       on the thread's stack. Point sp at the thread table instead and the
       refill reads 16 bytes of unrelated thread entries. The frame address is
       only ever used to *find* the saved values, which is why a8 does that work
       and a1 is assigned last. */
    asm volatile ("j 4f                               \n"
                  ".align 4                           \n"
                  "20: .long xt_context_addr          \n"
              "4:                                    \n"
                  "l32r    a8, 20b                    \n"
                  "l32i    a8, a8, 0                  \n"   /* the frame */
                  "l32i    a2, a8, 8                  \n"   /* XT_STK_PS */
                  "l32i    a0, a8, 4                  \n"   /* XT_STK_PC */
                  "l32i    a1, a8, 16                 \n"   /* XT_STK_A1 */
                  "wsr     a2, ps                     \n"
                  "retw                               \n"
                  :
                  :
                  : "memory");
    /* never returns */
}

/* Nothing else is needed to start a thread: load_context() checks start, and
 * create_thread() already put the top of the stack in context.sp. */
#define THREAD_STARTUP_INIT(core, thread, function) \
    ({ (thread)->context.start = (uint32_t)(uintptr_t)(function); })
