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
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/*
 * Xtensa under the call0 ABI. GCC treats a12-a15 as callee-saved (see
 * CALL_REALLY_USED_REGISTERS and xtensa_conditional_register_usage), a1 is the
 * stack pointer and a0 holds the return address. The windowed ABI would also
 * have to save the live register windows, so Rockbox builds Xtensa targets with
 * -mabi=call0 and this layout stays small.
 */
struct regs
{
    uint32_t r[4];  /* a12-a15 */
    uint32_t sp;    /* a1 */
    uint32_t ra;    /* a0 */
    uint32_t start; /* thread start address, or NULL when started */
};

/* The level-1 handler runs on the interrupted thread's stack: 80 bytes for its own
 * frame, then the dispatch, the GDMA completion path and the mixer chain that the
 * completion callback drives. That is a few hundred bytes on top of whatever the
 * thread was already using, so this matches what ARM uses for the same reason rather
 * than the 0x400 that m68k and mips get away with. */
#define DEFAULT_STACK_SIZE 0x1000 /* Bytes */
