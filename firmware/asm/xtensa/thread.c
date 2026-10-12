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
/* The address has to be re-read after the window rotation, so it cannot live in
 * a register. .srambss rather than .bss: .bss sits in the PSRAM window, which is
 * not mapped until mmu_init(), and the bring-up test below runs before then. */
static volatile uint32_t xt_context_addr
    __attribute__((section(".srambss")));

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
        /* A thread context must never carry an interrupt level: switch_thread()
         * is reached from paths that have disabled interrupts, and restoring that
         * level would leave the resumed thread with the tick and every device
         * interrupt masked. PS.INTLEVEL is bits 0-3. */
        "movi    a8, -16             \n"
        "rsr     a9, ps              \n"
        "and     a9, a9, a8          \n"
        "s32i    a9, a6, 68          \n"
        "rsr     a7, sar             \n"
        "s32i    a7, a6, 80          \n"
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
        "l32i    a7, a6, 84         \n"
        "beqz    a7, 1f             \n"

        /* First run: a clean window at level 0, then enter the starter as if it
         * had been called with CALLINC 0. PS is not zeroed wholesale: WOE has to
         * stay set, otherwise the window overflow vectors cannot fire for the
         * thread's own nested calls and ENTRY faults. */
        "movi    a7, 0              \n"
        "wsr     a7, windowbase     \n"
        "wsr     a7, windowstart    \n"
        "movi    a7, 0x40000        \n"
        "wsr     a7, ps             \n"
        "wsr     a7, sar            \n"
        "rsync                      \n"
        "l32r    a6, 10b            \n"
        "l32i    a6, a6, 0          \n"
        "l32i    a1, a6, 64         \n"
        "l32i    a2, a6, 84         \n"
        /* The entry point is consumed. Clear it so a later load_context on the
         * same context resumes the thread instead of restarting it - the same
         * thing the MIPS and SPARC implementations do. */
        "s32i    a7, a6, 84         \n"
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

#if XTENSA_THREAD_SELFTEST
/* boot_trace lives in the target, not in a header asm/ can reach reliably. */
void boot_trace(char c);
void boot_trace_hex(unsigned int v);

/*
 * Bring-up test for the windowed-ABI context switch, run from crt0 before the
 * kernel exists.
 *
 * Two bodies alternate on two separate stacks in internal SRAM. Each recurses
 * XT_DEPTH deep and calls the switch from the innermost frame, so several
 * register windows are live at the moment of the save. The interesting part is
 * the resume: load_context restores a0, sp, PS, WINDOWBASE and WINDOWSTART, and
 * the epilogue's retw then rotates back through the underflow handlers, which
 * refill the spilled windows from the frames on that thread's own stack.
 *
 * The trace is the evidence. xt_depth(6) returns 21 only if every frame above
 * the switch unwound with its registers intact, so a run that prints
 * a #00000015 b #00000015 ... eight times, and then z, has exercised the whole
 * spill/refill path. A broken restore takes an exception instead, which the
 * bring-up trap records in xt_exc_dump.
 */
#define XT_STACK_WORDS 512
#define XT_DEPTH       6
#define XT_ROUNDS      4

/* .srambss is NOLOAD and crt0 zeroes it, so none of these can carry a static
 * initialiser - the values have to be set at run time.
 */
static volatile int xt_depth_target __attribute__((section(".srambss")));
static volatile int xt_rounds __attribute__((section(".srambss")));
static int (*volatile xt_recurse)(int) __attribute__((section(".srambss")));

static uint32_t xt_stack[2][XT_STACK_WORDS]
    __attribute__((section(".srambss"), aligned(16)));
/* [0] and [1] are the two threads; [2] is the entry context, so the test can
 * hand control back to crt0 rather than spinning.
 */
static struct regs xt_ctx[3] __attribute__((section(".srambss")));
static int xt_cur __attribute__((section(".srambss")));
/* Result cells so the outcome is readable even if the trace ring is confusing:
 * [0] magic, [1] switches, [2] last xt_depth() result, [3] xt_cur at the end.
 */
volatile uint32_t xt_result[8] __attribute__((section(".srambss")));

/* Everything here runs from .text: the test executes before the PSRAM is touched,
 * so the flash cache window is still healthy. It cannot be ICODE_ATTR - internal
 * SRAM is only 43 KB and the firmware's IRAM code already nearly fills it.
 *
 * GCC flattens a plain self-recurrence into a loop, which leaves one frame live
 * and tests nothing. Recursing through a function pointer keeps the frames real,
 * and the asm barrier keeps each frame's own value in a register instead of
 * folding it into an accumulator.
 */
__attribute__((noinline)) static int xt_depth(int n)
{
    int live = n;
    asm volatile("" : "+r"(live));
    if (n == 0) {
        store_context(&xt_ctx[xt_cur]);
        xt_cur ^= 1;
        xt_result[1]++;
        load_context(&xt_ctx[xt_cur]);
        /* Not reached in this thread: load_context returns into the other one. */
        return live;
    }
    return xt_recurse(n - 1) + live;
}

static void xt_loop(int id)
{
    for (int i = 0; i < xt_rounds; i++) {
        int v = xt_depth(xt_depth_target);
        xt_result[2] = (uint32_t)v;
        boot_trace((char)('a' + id));
        boot_trace_hex((unsigned int)v);
    }
    /* The body that runs last unwinds back to crt0 through the saved entry
     * context; the other thread stays suspended on its own stack.
     */
    boot_trace((char)('z' + id));
    xt_result[3] = (uint32_t)xt_cur;
    load_context(&xt_ctx[2]);
}

static void xt_body_a(void) { xt_loop(0); }
static void xt_body_b(void) { xt_loop(1); }

void xtensa_thread_selftest(void)
{
    boot_trace('T');

    xt_depth_target = XT_DEPTH;
    xt_rounds = XT_ROUNDS;
    xt_recurse = xt_depth;
    xt_result[0] = 0x54485244u; /* 'THRD' */
    xt_result[1] = 0;

    /* Snapshot the state crt0 handed us so the test can return to it. */
    store_context(&xt_ctx[2]);

    xt_ctx[0].sp    = (uint32_t)(uintptr_t)&xt_stack[0][XT_STACK_WORDS];
    xt_ctx[0].start = (uint32_t)(uintptr_t)xt_body_a;
    xt_ctx[1].sp    = (uint32_t)(uintptr_t)&xt_stack[1][XT_STACK_WORDS];
    xt_ctx[1].start = (uint32_t)(uintptr_t)xt_body_b;
    xt_cur = 0;

    load_context(&xt_ctx[0]);
}

#endif /* XTENSA_THREAD_SELFTEST */
