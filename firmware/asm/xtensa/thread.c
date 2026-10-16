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
    rb_window_spill();

    /* The two slots take different forms of the same word.
       a0 keeps the value verbatim, which is what the resumed thread expects to
       find in a0 at that point. pc may not: in the windowed ABI the *top two
       bits* of a0 (31:30) carry the window increment, so the address's own top
       bits have been overwritten, not merely flagged. retw reconstructs that;
       _xt_user_exit does not -- it writes the frame's pc straight to EPC_1 and
       executes rfe, which jumps to the value verbatim. Masking bit 31 alone
       leaves bit 30 wrong and yields InstrFetchProhibited at 0x020522f3.
       ESP-IDF's own decoder (esp_cpu_utils.h, esp_cpu_process_stack_pc) maps
       the pair back with `| 0x40000000`, which is valid here because every
       executable region on this chip -- flash .text at 0x42xxxxxx, IRAM at
       0x4037xxxx, and the PSRAM codec window -- has bits 31:30 set to 01. */
    f[XT_F_PC]   = (pc & 0x80000000u) ? ((pc & 0x3fffffffu) | 0x40000000u)
                                      : pc;
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
