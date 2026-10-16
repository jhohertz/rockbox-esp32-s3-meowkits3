/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2014 by Michael Sevakis
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "kernel-internal.h"
#include <string.h>
#include "mrsw_lock.h"

#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
/* TEMPORARY bring-up: the recursion shortcut in mrsw_write_acquire() compares
 * __running_self_entry() against splay.blocker.thread. On a free lock the
 * blocker is NULL, so if the kernel ever reports no running thread the compare
 * succeeds and count-- is applied to a lock that was never taken. These count
 * how often that happens. */
volatile int mrsw_null_self;      /* write_acquire seen with no running thread */
volatile int mrsw_null_self_read; /* read_acquire seen with no running thread  */
volatile int mrsw_recursion_hits; /* acquires that took the recursion shortcut */
#endif /* CPU_XTENSA && THREAD_EXTRA_CHECKS */

/* TEMPORARY bring-up: the counters say the owner/count pair desynchronises but
 * not where. This records the last transitions with the thread that made them,
 * so the exact handoff that leaves count<0 with a NULL owner is visible. */
struct mrsw_evt { char op; char name[14]; short count; char nullowner; };
volatile struct mrsw_evt mrsw_evt[40];
volatile int mrsw_evt_i;

static void mrsw_evt_log(char op, const struct thread_entry *t, int c,
                         const struct mrsw_lock *m)
{
    volatile struct mrsw_evt *e;
    int i = mrsw_evt_i;

    mrsw_evt_i = (i + 1) % (int)(sizeof mrsw_evt / sizeof mrsw_evt[0]);
    e = &mrsw_evt[i];
    e->op = op;
    e->name[0] = '\0';
    if (t && t->name)
    {
        int j;
        for (j = 0; j < 13 && t->name[j]; j++)
            e->name[j] = t->name[j];
        e->name[j] = '\0';
    }
    else
    {
        e->name[0] = '-';
        e->name[1] = '\0';
    }
    e->count = (short)c;
    e->nullowner = (m && m->splay.blocker.thread == NULL) ? 1 : 0;
}

#ifdef HAVE_PRIORITY_SCHEDULING

static FORCE_INLINE void
mrsw_reader_claim(struct mrsw_lock *mrsw, struct thread_entry *current,
                  int count, unsigned int slotnum)
{
    /* no need to lock this; if a reader can claim, noone is in the queue */
    threadbit_set_bit(&mrsw->splay.mask, slotnum);
    mrsw->splay.blocker.thread = count == 1 ? current : NULL;
}

static FORCE_INLINE void
mrsw_reader_relinquish(struct mrsw_lock *mrsw, struct thread_entry *current,
                       struct thread_entry *first, int count,
                       unsigned int slotnum)
{
    /* If no writer is queued or has ownership then noone is queued;
       if a writer owns it, then the reader would be blocked instead.
       Therefore, if the queue has threads, then the next after the
       owning readers is a writer and this is not the last reader. */
    if (first)
        corelock_lock(&mrsw->splay.cl);

    threadbit_clear_bit(&mrsw->splay.mask, slotnum);

    if (count == 0)
    {
        /* There is noone waiting; we'd be calling mrsw_wakeup_writer()
           at this time instead */
        mrsw->splay.blocker.thread = NULL;
        return;
    }

    if (count == 1)
    {
        KERNEL_ASSERT(threadbit_popcount(&mrsw->splay.mask) == 1,
                      "mrsw_reader_relinquish() - "
                      "threadbits has wrong popcount: %d\n",
                      threadbit_popcount(&mrsw->splay.mask));
        /* switch owner to sole remaining reader */
        slotnum = threadbit_ffs(&mrsw->splay.mask);
        mrsw->splay.blocker.thread = __thread_slot_entry(slotnum);
    }

    if (first)
    {
        priority_disinherit(current, &mrsw->splay.blocker);
        corelock_unlock(&mrsw->splay.cl);
    }
}

static FORCE_INLINE unsigned int
mrsw_reader_wakeup_writer(struct mrsw_lock *mrsw, struct thread_entry *thread,
                          unsigned int slotnum)
{
    threadbit_clear_bit(&mrsw->splay.mask, slotnum);
    return wakeup_thread(thread, WAKEUP_TRANSFER);
}

static FORCE_INLINE unsigned int
mrsw_writer_wakeup_writer(struct mrsw_lock *mrsw, struct thread_entry *thread)
{
    return wakeup_thread(thread, WAKEUP_TRANSFER);
    (void)mrsw;
}

static FORCE_INLINE unsigned int
mrsw_writer_wakeup_readers(struct mrsw_lock *mrsw, struct thread_entry *first)
{
    unsigned int result = wakeup_thread(first, WAKEUP_TRANSFER_MULTI);
    mrsw->count = __running_self_entry()->retval;
    return result;
}

#else /* !HAVE_PRIORITY_SCHEDULING */

#define mrsw_reader_claim(mrsw, current, count, slotnum) \
    do {} while (0)

#define mrsw_reader_relinquish(mrsw, current, first, count, slotnum) \
    do {} while (0)

static FORCE_INLINE unsigned int
mrsw_reader_wakeup_writer(struct mrsw_lock *mrsw, struct thread_entry *thread)
{
    mrsw->splay.blocker.thread = thread;
    return wakeup_thread(thread);
}

static FORCE_INLINE unsigned int
mrsw_writer_wakeup_writer(struct mrsw_lock *mrsw, struct thread_entry *thread)
{
    mrsw->splay.blocker.thread = thread;
    return wakeup_thread(thread);
}

static FORCE_INLINE unsigned int
mrsw_writer_wakeup_readers(struct mrsw_lock *mrsw, struct thread_entry *first)
{
    mrsw->splay.blocker.thread = NULL;
    int count = 1;

    while (1)
    {
        wakeup_thread(first);

        if (!(first = WQ_THREAD_FIRST(&mrsw->queue)) || first->retval == 0)
            break;

        count++;
    }

    mrsw->count = count;
    return THREAD_OK;
}

#endif /* HAVE_PRIORITY_SCHEDULING */

/** Public interface **/

/* Initializes a multi-reader, single-writer object */
void mrsw_init(struct mrsw_lock *mrsw)
{
    mrsw->count = 0;
    wait_queue_init(&mrsw->queue);
    blocker_splay_init(&mrsw->splay);
#ifdef HAVE_PRIORITY_SCHEDULING
    memset(mrsw->rdrecursion, 0, sizeof (mrsw->rdrecursion));
#endif
    corelock_init(&mrsw->cl);
}

/* Request reader thread lock. Any number of reader threads may enter which
 * also locks-out all writer threads. Same thread may safely acquire read
 * access recursively. The current writer is ignored and gets access. */
void mrsw_read_acquire(struct mrsw_lock *mrsw)
{
    ASSERT_CPU_MODE(CPU_MODE_THREAD_CONTEXT);

    struct thread_entry *current = __running_self_entry();

#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
    if (current == NULL)
        mrsw_null_self_read++;
#endif

    if (current == mrsw->splay.blocker.thread IF_PRIO( && mrsw->count < 0 ))
        return; /* Read request while holding write access; pass */

#ifdef HAVE_PRIORITY_SCHEDULING
    /* Track recursion counts for each thread:
       IF_PRIO, mrsw->count just tracks unique owners  */
    unsigned int slotnum = THREAD_ID_SLOT(current->id);
    if (threadbit_test_bit(&mrsw->splay.mask, slotnum))
    {
        KERNEL_ASSERT(mrsw->rdrecursion[slotnum] < UINT8_MAX,
                      "mrsw_read_acquire() - "
                      "Thread %s did too many claims\n",
                      current->name);
        mrsw->rdrecursion[slotnum]++;
        return;
    }
#endif /* HAVE_PRIORITY_SCHEDULING */

    corelock_lock(&mrsw->cl);

    int count = mrsw->count;

    if (LIKELY(count >= 0 && mrsw->queue.head == NULL))
    {
        /* Lock open to readers:
           IFN_PRIO, mrsw->count tracks reader recursion */
        mrsw->count = ++count;
        mrsw_reader_claim(mrsw, current, count, slotnum);
        corelock_unlock(&mrsw->cl);
        return;
    }

    /* A writer owns it or is waiting; block... */
    current->retval = 1; /* indicate multi-wake candidate */

    disable_irq();
    block_thread(current, TIMEOUT_BLOCK, &mrsw->queue, &mrsw->splay.blocker);

    corelock_unlock(&mrsw->cl);

    /* ...and turn control over to next thread */
    switch_thread();
}

/* Release reader thread lockout of writer thread. The last reader to
 * leave opens up access to writer threads. The current writer is ignored. */
void mrsw_read_release(struct mrsw_lock *mrsw)
{
    struct thread_entry *current = __running_self_entry();

    if (current == mrsw->splay.blocker.thread IF_PRIO( && mrsw->count < 0 ))
        return; /* Read release while holding write access; ignore */

#ifdef HAVE_PRIORITY_SCHEDULING
    unsigned int slotnum = THREAD_ID_SLOT(current->id);
    KERNEL_ASSERT(threadbit_test_bit(&mrsw->splay.mask, slotnum),
                  "mrsw_read_release() -"
                  " thread '%s' not reader\n",
                  current->name);

    uint8_t *rdcountp = &mrsw->rdrecursion[slotnum];
    unsigned int rdcount = *rdcountp;
    if (rdcount > 0)
    {
        /* Reader is releasing recursive claim */
        *rdcountp = rdcount - 1;
        return;
    }
#endif /* HAVE_PRIORITY_SCHEDULING */

    corelock_lock(&mrsw->cl);
    int count = mrsw->count;

    KERNEL_ASSERT(count > 0, "mrsw_read_release() - no readers!\n");

    unsigned int result = THREAD_NONE;
    const int oldlevel = disable_irq_save();

    struct thread_entry *thread = WQ_THREAD_FIRST(&mrsw->queue);
    if (--count == 0 && thread != NULL)
    {
        /* No readers remain and a writer is waiting */
        mrsw->count = -1;
        result = mrsw_reader_wakeup_writer(mrsw, thread IF_PRIO(, slotnum));
#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
        /* The one ownership transfer that does not pass through
         * mrsw_write_acquire(), so without this the ring shows a new owner
         * appearing from nowhere. */
        mrsw_evt_log('W', thread, mrsw->count, mrsw);
#endif
    }
    else
    {
        /* Giving up readership; we may be the last, or not */
        mrsw->count = count;
        mrsw_reader_relinquish(mrsw, current, thread, count, slotnum);
    }

    restore_irq(oldlevel);
    corelock_unlock(&mrsw->cl);

#ifdef HAVE_PRIORITY_SCHEDULING
    if (result & THREAD_SWITCH)
        switch_thread();
#endif /* HAVE_PRIORITY_SCHEDULING */
    (void)result;
}

/* Acquire writer thread lock which provides exclusive access. If a thread
 * that is holding read access calls this it will deadlock. The writer may
 * safely call recursively. */
void mrsw_write_acquire(struct mrsw_lock *mrsw)
{
    ASSERT_CPU_MODE(CPU_MODE_THREAD_CONTEXT);

    struct thread_entry *current = __running_self_entry();

#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
    if (current == NULL)
        mrsw_null_self++;
    else if (current == mrsw->splay.blocker.thread)
        mrsw_recursion_hits++;
#endif

    if (current == mrsw->splay.blocker.thread)
    {
        /* Current thread already has write access */
        mrsw->count--;
#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
        mrsw_evt_log('R', current, mrsw->count, mrsw);
#endif
        return;
    }

    corelock_lock(&mrsw->cl);

    int count = mrsw->count;

    if (LIKELY(count == 0))
    {
        /* Lock is open to a writer */
        mrsw->count = -1;
        mrsw->splay.blocker.thread = current;
#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
        mrsw_evt_log('T', current, -1, mrsw);
#endif
        corelock_unlock(&mrsw->cl);
        return;
    }

    /* Readers present or a writer owns it - block... */
#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
    mrsw_evt_log('B', current, mrsw->count, mrsw);
#endif
    current->retval = 0; /* indicate single-wake candidate */

    disable_irq();
    block_thread(current, TIMEOUT_BLOCK, &mrsw->queue, &mrsw->splay.blocker);

    corelock_unlock(&mrsw->cl);

    /* ...and turn control over to next thread */
    switch_thread();
}

/* Release writer thread lock and open the lock to readers and writers */
void mrsw_write_release(struct mrsw_lock *mrsw)
{
    KERNEL_ASSERT(__running_self_entry() == mrsw->splay.blocker.thread,
                  "mrsw_write_release->wrong thread (%s != %s)\n",
                  __running_self_entry()->name,
                  mrsw->splay.blocker.thread->name);

    int count = mrsw->count;
    if (count < -1)
    {
        /* This thread still owns write lock */
        mrsw->count = count + 1;
#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
        mrsw_evt_log('r', __running_self_entry(), count + 1, mrsw);
#endif
        return;
    }

    unsigned int result = THREAD_NONE;

    corelock_lock(&mrsw->cl);
    const int oldlevel = disable_irq_save();

    struct thread_entry *thread = WQ_THREAD_FIRST(&mrsw->queue);
    if (thread == NULL)           /* 'count' becomes zero */
    {
        mrsw->splay.blocker.thread = NULL;
        mrsw->count = 0;
#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
        mrsw_evt_log('F', __running_self_entry(), 0, mrsw);
#endif
    }
    else if (thread->retval == 0) /* 'count' stays -1 */
    {
        result = mrsw_writer_wakeup_writer(mrsw, thread);
#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
        mrsw_evt_log('h', thread, mrsw->count, mrsw);
#endif
    }
    else                          /* 'count' becomes # of readers */
    {
        result = mrsw_writer_wakeup_readers(mrsw, thread);
#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
        mrsw_evt_log('H', thread, mrsw->count, mrsw);
#endif
    }

    restore_irq(oldlevel);
    corelock_unlock(&mrsw->cl);

#ifdef HAVE_PRIORITY_SCHEDULING
    if (result & THREAD_SWITCH)
        switch_thread();
#endif /* HAVE_PRIORITY_SCHEDULING */
    (void)result;
}

#if defined(CPU_XTENSA) && THREAD_EXTRA_CHECKS
/* TEMPORARY bring-up accessor, target-gated and compiled out unless the kernel's
 * own extra checks are on. The xtensa port deadlocks on the filesystem lock
 * with no panic and no output, so the only way to see it is to ask the lock who
 * holds it. Reaching into struct mrsw_lock from target code would mean
 * duplicating kernel-internal layout out of kernel sight, which is exactly the
 * kind of thing that quietly goes wrong; this keeps the knowledge in the file
 * that owns the type. */
#include "thread-internal.h"

int mrsw_debug_report(const struct mrsw_lock *mrsw, char *out, int sz)
{
    struct thread_entry *owner = mrsw->splay.blocker.thread;
    struct thread_entry *t;
    int pos = 0;
    int n = 0;

    if (sz <= 0)
        return 0;
    out[0] = '\0';

    pos += snprintf(out + pos, sz - pos, "owner=%s",
                    owner && owner->name ? owner->name : "(none)");
    /* A blocked owner means the lock is held by a thread waiting on something
     * other than this lock -- which is the shape of this target's hang. */
    if (owner)
        pos += snprintf(out + pos, sz - pos, owner->blocker ?
                        "/BLOCKED@%p" : "/running",
                        (void *)owner->blocker);

    pos += snprintf(out + pos, sz - pos, " waiting=[");
    for (t = WQ_THREAD_FIRST((struct __wait_queue *)&mrsw->queue); t;
         t = WQ_THREAD_NEXT(t))
    {
        if (pos >= sz - 32)
            break;
        /* retval is set to 0 by a would-be writer and 1 by a would-be reader
         * just before blocking, so this says which role each waiter queued
         * under -- the difference between "a writer was never handed the lock"
         * and "everyone is merely behind one". */
        pos += snprintf(out + pos, sz - pos, "%s%s(%s)", n++ ? "," : "",
                        t->name ? t->name : "?",
                        t->retval == 0 ? "W" : "R");
    }
    snprintf(out + pos, sz - pos, "]%s", n ? "" : "none");
    return pos;
}

int mrsw_debug_trace(char *out, int sz)
{
    const int n = (int)(sizeof mrsw_evt / sizeof mrsw_evt[0]);
    int i, pos = 0;

    if (sz <= 0)
        return 0;
    out[0] = '\0';
    for (i = 0; i < n; i++)
    {
        const volatile struct mrsw_evt *e = &mrsw_evt[(mrsw_evt_i + i) % n];
        int left;

        if (e->op == 0)
            continue;
        if (pos >= sz - 40)
            break;
        left = sz - pos;
        pos += snprintf(out + pos, left, "%c:%s/%d%s ", e->op, e->name,
                        e->count, e->nullowner ? "*" : "");
    }
    return pos;
}

int mrsw_debug_state(const struct mrsw_lock *mrsw, char *owner_name, int sz,
                     int *waiters)
{
    struct thread_entry *owner = mrsw->splay.blocker.thread;
    int n = 0;
    struct thread_entry *t;

    for (t = WQ_THREAD_FIRST((struct __wait_queue *)&mrsw->queue); t;
         t = WQ_THREAD_NEXT(t))
        n++;

    if (owner_name && sz > 0)
    {
        if (owner && owner->name)
            snprintf(owner_name, sz, "%s", owner->name);
        else
            snprintf(owner_name, sz, "(none)");
    }

    if (waiters)
        *waiters = n;

    return mrsw->count;
}
#endif /* CPU_XTENSA && THREAD_EXTRA_CHECKS */
