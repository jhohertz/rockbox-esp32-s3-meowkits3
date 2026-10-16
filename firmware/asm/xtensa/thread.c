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
 * The xtensa thread layer: three calls, and no context switch.
 *
 * The register work is ESP-IDF's. This file only adapts Rockbox's arch
 * interface -- store_context / load_context / THREAD_STARTUP_INIT / core_sleep
 * -- to the gated-task model implemented in target/xtensa/thread-idf.c. It is
 * #included into kernel/thread.c, so it deliberately includes no IDF header;
 * see the comment at the top of thread-idf.c for why that matters.
 */

/*---------------------------------------------------------------------------
 * Store non-volatile context.
 *---------------------------------------------------------------------------
 *
 * Nothing to do: IDF saves the registers when it switches tasks, on the task's
 * own stack where the windowed ABI needs them, which is the part this file used
 * to get wrong. Thread 0 is the one special case -- it is the task app_main
 * started Rockbox on rather than one create_thread made, so it adopts that task
 * and grows a gate the first time it is switched out.
 */
static inline void rb_ref_fill(struct thread_entry *thread)
{
    struct rb_thread_ref *ref = (struct rb_thread_ref *)
        calloc(1, sizeof(struct rb_thread_ref));

    if (ref == NULL)
        panicf("thread ref: no memory");

    ref->ctx = &thread->context;
    ref->thread = thread;
    thread->context.thread = thread;
    ref->name = thread->name;
    ref->stack = thread->stack;
    ref->stack_size = thread->stack_size;
    thread->context.ref = ref;
}

static inline void store_context(void* addr)
{
    rb_thread_adopt_self((struct regs*)(uintptr_t)addr);
}

/*---------------------------------------------------------------------------
 * Resume another thread.
 *---------------------------------------------------------------------------
 *
 * Give the incoming thread its gate, then block on our own. The take returning
 * *is* the resumption: control comes back into switch_thread, which returns
 * into the blocking primitive that called it, which returns into the thread's
 * own code. That is the ordinary FreeRTOS blocking shape, and exactly the
 * semantics Rockbox expects from a cooperative yield.
 */
static inline void load_context(const void* addr)
{
    rb_thread_handoff((struct regs*)(uintptr_t)addr);
}

/*---------------------------------------------------------------------------
 * Start a thread.
 *---------------------------------------------------------------------------
 *
 * create_thread's arch hook, and the only point at which the stack, its size
 * and the entry function are all known at once.
 */
#define THREAD_STARTUP_INIT(core, thread, function)                           \
    ({                                                                        \
        (thread)->context.start = (uint32_t)(uintptr_t)(function);            \
        rb_ref_fill(thread),                                                  \
        rb_thread_task_create((struct rb_thread_ref *)(thread)->context.ref,  \
                              THREAD_ID_SLOT((thread)->id)),                  \
        /* FreeRTOS fills the stack buffer with its own 0xa5 pattern as it     \
           builds the task, over the word switch_thread() checks against       \
           DEADBEEF. Put it back now that the filling has happened, or every   \
           thread reports its own overflow on its first switch. */             \
        (thread)->stack[0] = DEADBEEF;                                        \
    })

/* core_sleep() is not defined here: target/xtensa/system-target.h already has
 * it, with the waiti-until-any-interrupt shape this design needs. */

/*---------------------------------------------------------------------------
 * Declare the running thread for this core.
 *
 * The kernel keeps corep->running and updates it inside switch_thread(). That
 * is sufficient when every change of the running thread passes through
 * switch_thread(), which is the assumption the hand-rolled switch made. Under
 * the gated-task model it is not: FreeRTOS decides who runs next, and a thread
 * can be resumed without the kernel ever recording it. When that happens the
 * pointer names a thread that is not on the CPU, and thread_exit() -- which
 * reads it to find the thread it is tearing down -- then evaluates the
 * priority-inheritance assertion for the wrong thread. That is the "abandon
 * ship!" panic, and it is why it lands on whichever thread the pointer happened
 * to be left pointing at rather than on any particular one.
 *
 * Setting it at each entry point makes the pointer a fact rather than a
 * bookkeeping guess.
 */
void rb_thread_set_running(void *thread)
{
    struct core_entry *corep;

    /* Thread 0 is adopted in store_context(), which has the context in hand but
     * not the thread_entry, so its context.thread is never filled. Leaving the
     * pointer alone there is correct rather than lossy: switch_thread() sets
     * corep->running before it ever calls load_context(), so thread 0 is
     * accounted for by the path that already works. Clearing it to NULL here
     * would be strictly worse than not touching it. */
    if (thread == NULL)
        return;

    corep = __core_id_entry(CURRENT_CORE);
    corep->running = (struct thread_entry *)thread;
}
