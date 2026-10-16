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
#ifndef MRSW_LOCK_H
#define MRSW_LOCK_H

#include "thread.h"

/* Multi-reader, single-writer object that allows mutltiple readers or a
 * single writer thread access to a critical section.
 *
 * Readers and writers, as with a mutex, may claim the lock recursively for
 * the same type of access they were already granted.
 *
 * Writers are trivially granted read access by ignoring the request; the
 * object is not changed.
 *
 * Reader promotion to writer is NOT supported and a reader attempting write
 * access will result in a deadlock. That could change but for now, be warned.
 *
 * Full priority inheritance is implemented.
 */
struct mrsw_lock
{
    int volatile         count; /* counter; >0 = reader(s), <0 = writer */
    struct __wait_queue  queue; /* waiter list */
    struct blocker_splay splay; /* priority inheritance/owner info  */
    uint8_t rdrecursion[MAXTHREADS]; /* per-thread reader recursion counts */
    IF_COP( struct corelock cl; )
};

void mrsw_init(struct mrsw_lock *mrsw);

#if defined(CPU_XTENSA) && !defined(SIMULATOR)
/* TEMPORARY bring-up: read the lock's state without being its owner. Returns
 * mrsw->count (>0 readers, <0 writer, 0 free), the owner's name, and the number
 * of waiters. */
int mrsw_debug_state(const struct mrsw_lock *mrsw, char *owner_name, int sz,
                     int *waiters);
/* TEMPORARY bring-up: render the last lock transitions into out. */
int mrsw_debug_trace(char *out, int sz);
/* TEMPORARY bring-up: who owns the lock, what they are blocked on, and who is
 * queued behind them. */
int mrsw_debug_report(const struct mrsw_lock *mrsw, char *out, int sz);
#endif
void mrsw_read_acquire(struct mrsw_lock *mrsw);
void mrsw_read_release(struct mrsw_lock *mrsw);
void mrsw_write_acquire(struct mrsw_lock *mrsw);
void mrsw_write_release(struct mrsw_lock *mrsw);

#endif /* MRSW_LOCK_H */
