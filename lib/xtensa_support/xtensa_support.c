/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2026 Rockbox contributors
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
 * Xtensa has no 64-bit shift, divide or modulo instruction, so GCC calls the
 * helpers below.  The toolchain's libgcc is built for the windowed ABI in every
 * multilib, but Rockbox builds Xtensa with -mabi=call0 (see tools/configure):
 * a windowed helper may clobber a12-a15, which are callee-saved under call0,
 * and it returns with retw, which needs a window rotation that a call0 call
 * never sets up.
 *
 * So these have to be ours.  Every one of them uses only 32-bit operations -
 * a 64-bit shift or division here would make GCC emit a libgcc call and we
 * would recurse straight back into the windowed archive.
 *
 * Not covered: the soft-float helpers (__muldf3, __extendsfdf2, __floatsidf,
 * __truncdfsf2, ...), which are also windowed in the toolchain.  They cannot
 * be written in C using float or double, because that is exactly what calls
 * libgcc.  Either they need integer-only bit twiddling, or double has to stay
 * out of the xtensa build.  Two files in the tree currently need them:
 * apps/debug_menu.c and apps/plugins/pdbox/PDa/src/d_mayer_fft.c.
 */

#include <config.h>
#include <stdint.h>

union u64 {
    uint64_t v;
    uint32_t w[2];
};

#ifdef ROCKBOX_BIG_ENDIAN
# define LO 1
# define HI 0
#else
# define LO 0
# define HI 1
#endif

static inline uint64_t pack(uint32_t hi, uint32_t lo)
{
    union u64 u;
    u.w[HI] = hi;
    u.w[LO] = lo;
    return u.v;
}

int64_t __ashldi3(int64_t a, int b)
{
    union u64 u;
    u.v = (uint64_t)a;

    if (b == 0) return a;
    if (b < 32) {
        u.w[HI] = (u.w[HI] << b) | (u.w[LO] >> (32 - b));
        u.w[LO] <<= b;
    } else {
        u.w[HI] = u.w[LO] << (b - 32);
        u.w[LO] = 0;
    }
    return (int64_t)u.v;
}

int64_t __ashrdi3(int64_t a, int b)
{
    union u64 u;
    u.v = (uint64_t)a;

    if (b == 0) return a;
    if (b < 32) {
        u.w[LO] = (u.w[LO] >> b) | (u.w[HI] << (32 - b));
        u.w[HI] = (uint32_t)((int32_t)u.w[HI] >> b);   /* arithmetic */
    } else {
        u.w[LO] = (uint32_t)((int32_t)u.w[HI] >> (b - 32));
        u.w[HI] = (int32_t)u.w[HI] < 0 ? 0xFFFFFFFFU : 0;
    }
    return (int64_t)u.v;
}

uint64_t __lshrdi3(uint64_t a, int b)
{
    union u64 u;
    u.v = a;

    if (b == 0) return a;
    if (b < 32) {
        u.w[LO] = (u.w[LO] >> b) | (u.w[HI] << (32 - b));
        u.w[HI] >>= b;
    } else {
        u.w[LO] = u.w[HI] >> (b - 32);
        u.w[HI] = 0;
    }
    return u.v;
}

/* Binary long division, one bit at a time.  Slow, but it never asks libgcc for
 * anything.  q and r are returned through rptr when asked for. */
static uint64_t div64(uint64_t n, uint64_t d,
                                uint64_t *rptr)
{
    union u64 N, D, Q, R;
    Q.v = 0;
    R.v = 0;

    if (d == 0) {
        if (rptr) *rptr = 0;
        return 0;
    }

    N.v = n;
    D.v = d;

    for (int i = 63; i >= 0; i--) {
        uint32_t bit = i >= 32 ? ((N.w[HI] >> (i - 32)) & 1)
                                    : ((N.w[LO] >> i) & 1);
        uint32_t carry = (R.w[LO] >> 31) & 1;
        R.w[LO] = (R.w[LO] << 1) | bit;
        R.w[HI] = (R.w[HI] << 1) | carry;

        if (R.w[HI] > D.w[HI] || (R.w[HI] == D.w[HI] && R.w[LO] >= D.w[LO])) {
            uint32_t lo = R.w[LO] - D.w[LO];
            uint32_t hi = R.w[HI] - D.w[HI] - (R.w[LO] < D.w[LO]);
            R.w[LO] = lo;
            R.w[HI] = hi;
            if (i >= 32) Q.w[HI] |= 1U << (i - 32);
            else         Q.w[LO] |= 1U << i;
        }
    }

    if (rptr) *rptr = R.v;
    return Q.v;
}

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    return div64(n, d, 0);
}

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    div64(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d)
{
    uint64_t un = (uint64_t)(n < 0 ? -n : n);
    uint64_t ud = (uint64_t)(d < 0 ? -d : d);
    uint64_t q = div64(un, ud, 0);

    if ((n < 0) ^ (d < 0)) return -(int64_t)q;
    return (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t un = (uint64_t)(n < 0 ? -n : n);
    uint64_t ud = (uint64_t)(d < 0 ? -d : d);
    uint64_t r;
    div64(un, ud, &r);

    if (n < 0) return -(int64_t)r;
    return (int64_t)r;
}

unsigned int __bswapsi2(unsigned int a)
{
    uint32_t v = a;
    return (unsigned int)((v >> 24) | ((v >> 8) & 0xFF00) |
                          ((v & 0xFF) << 24) | ((v << 8) & 0xFF0000));
}

int __ctzdi2(uint64_t a)
{
    union u64 u;
    u.v = a;

    if (u.w[LO] == 0) {
        if (u.w[HI] == 0) return 64;
        return 32 + __builtin_ctz(u.w[HI]);
    }
    return __builtin_ctz(u.w[LO]);
}
