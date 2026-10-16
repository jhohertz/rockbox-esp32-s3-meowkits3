/* Included by asm/thread.h and, for the IDF binding, directly by
 * target/xtensa/thread-idf.c -- which also reaches it through system.h,
 * so it needs a guard. */
#ifndef ROCKBOX_ASM_XTENSA_THREAD_H
#define ROCKBOX_ASM_XTENSA_THREAD_H

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
 * Xtensa under ESP-IDF: a Rockbox thread *is* a FreeRTOS task.
 *
 * What used to live here was a hand-rolled context switch -- XtExcFrame laid in
 * the thread table, _xt_context_save/_xt_context_restore, xthal_window_spill_nw,
 * retw back through the blocking primitive. It ran, after a fashion, and it was
 * the largest single source of faults in this port. The reason is structural: a
 * windowed-ABI context switch is exactly the thing IDF already implements,
 * correctly, against the real core configuration, and every re-derivation has
 * to rediscover on its own the base-save geometry, the spill ordering, the fact
 * that wsr.PS is not rfe, and the fact that inline asm cannot tell the compiler
 * that a block changes which physical registers the names refer to. Each of
 * those was a separate debugging campaign here. Hand-transcribing IDF's
 * vPortYield and _frxt_dispatch into a .S file produced three new bugs in an
 * hour, all of which are non-events in IDF's own code.
 *
 * So this layer no longer switches anything. The IDF half lives in
 * target/xtensa/thread-idf.c; what is left here is three one-line calls.
 *
 * The cooperative semantics are preserved by construction rather than by
 * policy: a task cannot run unless its gate has been given, and exactly one
 * gate is held at a time, so exactly one Rockbox thread runs -- and it is still
 * switched only at Rockbox's own blocking primitives, because that is the only
 * place that gives gates. The scheduler, the thread table, thread IDs, names,
 * priorities, the run queue, the timeout list and every blocking primitive are
 * untouched. Nothing in firmware/kernel knows that the threads are tasks.
 */

/* sp and start keep the names the kernel already writes: create_thread sets sp
 * to the top of the stack and THREAD_STARTUP_INIT sets start to the entry
 * point. The handles are void* rather than SemaphoreHandle_t and TaskHandle_t
 * because this header is also read by the freestanding domain (codecs and
 * plugins), which has no IDF include path. */
struct regs
{
    uint32_t sp;      /* top of the stack, as create_thread left it */
    uint32_t start;   /* entry point, from THREAD_STARTUP_INIT */
    void* gate;       /* binary semaphore: the right to run */
    void* task;       /* the FreeRTOS task this thread runs on */
    void* ref;        /* the rb_thread_ref handed to the task, which has to
                       * outlive the call that created it */
    void* thread;     /* struct thread_entry * this context belongs to. Lives
                       * here rather than only in rb_thread_ref because
                       * rb_thread_handoff() resumes with the context in hand
                       * and not the ref, and it needs to re-assert who is
                       * running. */
};

/* Implemented in target/xtensa/thread-idf.c. The ref is allocated and filled by
 * the caller's helper below and kept alive by living in struct regs. */
struct rb_thread_ref
{
    struct regs *ctx;
    void *thread;       /* struct thread_entry *, so the task can tell the
                         * kernel who is actually running. corep->running is
                         * per-core bookkeeping the kernel maintains in
                         * switch_thread(), but under the gated-task model a
                         * thread can gain the CPU without going through that
                         * path, and then the pointer names the wrong thread. */
    const char *name;
    void *stack;
    unsigned int stack_size;
};

void rb_thread_task_create(struct rb_thread_ref *ref, unsigned int slot);
void rb_thread_adopt_self(struct regs *ctx);
/* Declare which thread_entry this task is now running. Called on every entry
 * into a Rockbox thread, because thread_exit() and the scheduler both read
 * corep->running and must not be handed a stale one. */
void rb_thread_set_running(void *thread);
void rb_thread_handoff(struct regs *in);

/* The level-1 handler runs on the interrupted thread's stack: its own frame,
 * then the dispatch, the GDMA completion path and the mixer chain that the
 * completion callback drives. This is the FreeRTOS task stack, so it also has
 * to satisfy IDF's stack-overflow watermark at the buffer's bottom. */
#define DEFAULT_STACK_SIZE 0x1000 /* Bytes */

#endif /* ROCKBOX_ASM_XTENSA_THREAD_H */
