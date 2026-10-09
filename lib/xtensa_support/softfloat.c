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
 * Soft float for Xtensa, call0 ABI.
 *
 * The esp32s3 core has single-precision multiply, add and int->float in
 * hardware, so GCC emits mul.s/add.s/ufloat.s directly.  It has no div.s, and
 * double is entirely soft, so GCC still calls these.  The toolchain's versions
 * are windowed, which is why they have to be ours.
 *
 * Nothing below may use float or double arithmetic - that is exactly what calls
 * libgcc, and we would recurse.  Values are handled as bit patterns only.
 *
 * Round to nearest, ties to even.  NaN and infinity pass through; no rounding
 * modes, no exceptions, no errno.
 */

#include <config.h>
#include <stdint.h>

static inline uint64_t dbits(double d)
{
    union { double d; uint64_t u; } x;
    x.d = d;
    return x.u;
}

static inline double dval(uint64_t u)
{
    union { double d; uint64_t u; } x;
    x.u = u;
    return x.d;
}

static inline uint32_t fbits(float f)
{
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}

static inline float fval(uint32_t u)
{
    union { float f; uint32_t u; } x;
    x.u = u;
    return x.f;
}

/* ---- double ---- */

#define DMANT_MASK ((1ULL << 52) - 1)
#define DHID       (1ULL << 52)
#define DINF       (0x7FFULL << 52)
#define DNAN       ((0x7FFULL << 52) | (1ULL << 51))

static inline int dexp(uint64_t u)  { return (int)((u >> 52) & 0x7FF); }
static inline int dsign(uint64_t u) { return (int)(u >> 63); }
static int msb64(uint64_t m);

static int msb64(uint64_t m)
{
    if (!m) return -1;
    int p = 63;
    while (!(m & (1ULL << 63))) { m <<= 1; p--; }
    return p;
}

/* Sign, biased exponent, and a mantissa with the hidden bit set.  Subnormals
 * are normalised, so exp can come back <= 0.  Zero returns mantissa 0. */
static void dsplit(uint64_t u, int *sign, int *exp, uint64_t *mant)
{
    *sign = dsign(u);
    *exp = dexp(u);
    *mant = u & DMANT_MASK;

    if (*exp == 0) {
        if (*mant == 0) return;
        int k = 0;
        while (!(*mant & DHID)) { *mant <<= 1; k++; }
        *exp = 1 - k;
    } else {
        *mant |= DHID;
    }
}

/* Assemble from sign, biased exponent, 53-bit mantissa and the lost bits. */
static double dpack(int sign, int exp, uint64_t mant, int lost_hi, int lost_any)
{
    if (mant == 0) return dval((uint64_t)sign << 63);

    if (exp <= 0) {
        int sh = 1 - exp;
        if (sh > 53) return dval((uint64_t)sign << 63);
        /* The bits about to be shifted out sit above the originally lost ones,
         * so the whole lost part (including its own round bit) is now sticky. */
        int r = (int)((mant >> (sh - 1)) & 1);
        int s = ((mant & ((1ULL << (sh - 1)) - 1)) != 0) || lost_hi || lost_any;
        uint64_t kept = mant >> sh;
        if (r && (s || (kept & 1))) kept++;
        return dval(((uint64_t)sign << 63) | kept);
    }

    if (lost_hi && (lost_any || (mant & 1))) {
        mant++;
        if (mant >> 53) { mant >>= 1; exp++; }
    }

    if (exp > 2046) return dval(((uint64_t)sign << 63) | DINF);
    return dval(((uint64_t)sign << 63) | ((uint64_t)exp << 52) |
                (mant & DMANT_MASK));
}

static double dadd(uint64_t ba, uint64_t bb)
{
    int ea = dexp(ba), eb = dexp(bb);

    if (ea == 0x7FF) {
        if (ba & DMANT_MASK) return dval(ba);            /* a is NaN */
        if (eb == 0x7FF) {
            if (bb & DMANT_MASK) return dval(bb);        /* b is NaN */
            if (dsign(ba) != dsign(bb)) return dval(DNAN);
        }
        return dval(ba);
    }
    if (eb == 0x7FF) {
        if (bb & DMANT_MASK) return dval(bb);
        return dval(bb);
    }

    int sa, sb, ea2, eb2;
    uint64_t ma, mb;
    dsplit(ba, &sa, &ea2, &ma);
    dsplit(bb, &sb, &eb2, &mb);
    if (ma == 0) return dval(bb);
    if (mb == 0) return dval(ba);

    int exp = ea2 > eb2 ? ea2 : eb2;
    int d = exp - (ea2 < eb2 ? ea2 : eb2);   /* how far the smaller one shifts */

    /* L holds the bits that fall off the end, as an integer of d bits. Keeping
     * them as a value rather than as round/sticky flags is what makes the
     * borrow in the subtraction case work. */
    uint64_t L = 0;
    int bigshift = 0;                 /* d > 53: nothing of L can reach the mantissa */
    if (d > 53) {
        L = ea2 < eb2 ? ma : mb;
        if (ea2 < eb2) ma = 0; else mb = 0;
        bigshift = 1;
    } else if (d) {
        if (ea2 < eb2) { L = ma & ((1ULL << d) - 1); ma >>= d; }
        else { L = mb & ((1ULL << d) - 1); mb >>= d; }
    }

    uint64_t mant;
    int lowner = d ? (ea2 < eb2 ? 1 : 2) : 0;   /* which operand L came from */

    if (sa == sb) {
        mant = ma + mb;
    } else {
        /* Which operand is the minuend. A tie in the kept mantissas is broken
         * by the lost part, which belongs to lowner. */
        int minuend;
        if (ma > mb) minuend = 1;
        else if (ma < mb) minuend = 2;
        else minuend = L ? lowner : 0;

        if (minuend == 0) return dval((uint64_t)sa << 63);
        mant = (minuend == 1) ? ma - mb : mb - ma;
        sa = (minuend == 1) ? sa : sb;

        /* L is added if it belongs to the minuend, subtracted otherwise. */
        if (L && !bigshift && lowner != minuend) { mant--; L = (1ULL << d) - L; }
    }

    if (mant == 0 && L == 0) return dval((uint64_t)sa << 63);

    int round, sticky;

    if (mant >> 53) {
        /* Two 53-bit mantissas can sum to a 54-bit value. The bit shifted out
         * becomes the rounding bit and the lost part drops below it. */
        round = (int)(mant & 1);
        sticky = (L != 0);
        mant >>= 1;
        exp++;
    } else {
        /* Normalise. Shifting the mantissa left pulls bits out of the lost
         * part, so the mantissa is not just the integer part of the result. */
        while (mant < DHID) {
            if (d && L && !bigshift) {
                mant = (mant << 1) | (int)((L >> (d - 1)) & 1);
                L = (L << 1) & ((1ULL << d) - 1);
            } else {
                mant <<= 1;
            }
            exp--;
        }
        if (bigshift) { round = 0; sticky = (L != 0); }
        else { round = d ? (int)((L >> (d - 1)) & 1) : 0;
               sticky = d ? (int)((L & ((1ULL << (d - 1)) - 1)) != 0) : 0; }
    }

    return dpack(sa, exp, mant, round, sticky);
}

double __adddf3(double a, double b) { return dadd(dbits(a), dbits(b)); }
double __subdf3(double a, double b) { return dadd(dbits(a), dbits(b) ^ (1ULL << 63)); }

/* 53 x 53 -> 106 bits in two words, using only 32-bit products. */
static void mul53(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t a0 = a & 0xFFFFFFFF, a1 = a >> 32;
    uint64_t b0 = b & 0xFFFFFFFF, b1 = b >> 32;

    uint64_t p0 = a0 * b0;
    uint64_t mid = a0 * b1 + a1 * b0;
    uint64_t top = a1 * b1;

    uint64_t fold = (mid & 0xFFFFFFFF) + (p0 >> 32);
    *hi = top + (mid >> 32) + (fold >> 32);
    *lo = ((fold & 0xFFFFFFFF) << 32) | (p0 & 0xFFFFFFFF);
}

double __muldf3(double a, double b)
{
    uint64_t ua = dbits(a), ub = dbits(b);
    int ea = dexp(ua), eb = dexp(ub);

    if (ea == 0x7FF) {
        if (ua & DMANT_MASK) return dval(ua);
        if (eb == 0x7FF && (ub & DMANT_MASK)) return dval(ub);
        if (!(ub & DMANT_MASK) && eb == 0 && ub == 0) return dval(DNAN);  /* inf * 0 */
        if (eb == 0x7FF && dsign(ua) != dsign(ub)) return dval(DNAN);
        return dval(((uint64_t)(dsign(ua) ^ dsign(ub)) << 63) | DINF);
    }
    if (eb == 0x7FF) {
        if (ub & DMANT_MASK) return dval(ub);
        if (ua == 0) return dval(DNAN);                                   /* 0 * inf */
        return dval(((uint64_t)(dsign(ua) ^ dsign(ub)) << 63) | DINF);
    }

    int sa, sb, ea2, eb2;
    uint64_t ma, mb;
    dsplit(ua, &sa, &ea2, &ma);
    dsplit(ub, &sb, &eb2, &mb);
    if (ma == 0 || mb == 0) return dval((uint64_t)(sa ^ sb) << 63);

    uint64_t hi, lo;
    mul53(ma, mb, &hi, &lo);

    /* hi holds the top of the 106-bit product. Normalise to 53 bits: the low
     * bits of the mantissa come from the top of lo, not from zeros. */
    int q = msb64(hi);
    int sh = 52 - q;
    uint64_t mant = (hi << sh) | (sh ? (lo >> (64 - sh)) : 0);
    uint64_t lost = lo & ((1ULL << (q + 12)) - 1);
    int lost_hi = (int)((lost >> (q + 11)) & 1);
    int lost_any = (lost & ((1ULL << (q + 11)) - 1)) != 0;

    return dpack(sa ^ sb, ea2 + eb2 - 1011 - sh, mant, lost_hi, lost_any);
}

double __divdf3(double a, double b)
{
    uint64_t ua = dbits(a), ub = dbits(b);
    int ea = dexp(ua), eb = dexp(ub);

    if (ea == 0x7FF) {
        if (ua & DMANT_MASK) return dval(ua);
        if (eb == 0x7FF && (ub & DMANT_MASK)) return dval(ub);
        return dval(DNAN);                       /* inf / inf */
    }
    if (eb == 0x7FF) {
        if (ub & DMANT_MASK) return dval(ub);                     /* b is NaN */
        if (ea == 0x7FF) return dval(DNAN);                /* inf / inf */
        return dval((uint64_t)(dsign(ua) ^ dsign(ub)) << 63);     /* finite / inf */
    }

    int sa, sb, ea2, eb2;
    uint64_t ma, mb;
    dsplit(ua, &sa, &ea2, &ma);
    dsplit(ub, &sb, &eb2, &mb);

    if (mb == 0 && ma == 0) return dval(DNAN);   /* 0/0 */
    if (mb == 0) return dval((uint64_t)(sa ^ sb) << 63 | DINF);
    if (ma == 0) return dval((uint64_t)(sa ^ sb) << 63);

    int exp = ea2 - eb2;
    if (ma < mb) { ma <<= 1; exp--; }

    uint64_t r = ma, q = 0;
    for (int i = 52; i >= 0; i--) {
        if (r >= mb) { q |= 1ULL << i; r -= mb; }
        if (i) r <<= 1;
    }

    int E = exp + 1023;
    if (E <= 0) {
        /* Subnormal result: round at the precision that actually survives.
         * The dropped part is low/2^sh + r/(mb*2^sh), so the round condition is
         * low + r/mb > 2^(sh-1). Since r < mb and low is an integer, low below
         * half can never reach half, so only low > half or an exact tie matters. */
        int sh = 1 - E;
        if (sh > 53) return dval((uint64_t)(sa ^ sb) << 63);
        uint64_t kept = q >> sh;
        uint64_t half = 1ULL << (sh - 1);
        uint64_t low = q & ((1ULL << sh) - 1);
        if (low > half || (low == half && (r != 0 || (kept & 1)))) kept++;
        return dval((uint64_t)(sa ^ sb) << 63 | kept);
    }

    /* r/mb is what was dropped: round if it is more than half an ulp, or
     * exactly half and the last kept bit is odd. */
    uint64_t twice = r << 1;
    if (twice > mb || (twice == mb && (q & 1))) q++;

    if (q >> 53) { q >>= 1; exp++; }
    return dpack(sa ^ sb, exp + 1023, q & (DHID | DMANT_MASK), 0, 0);
}

/* ---- double comparisons ---- */

int __eqdf2(double a, double b) { return dbits(a) == dbits(b); }
int __nedf2(double a, double b) { return dbits(a) != dbits(b); }

static int dcmp(uint64_t a, uint64_t b)
{
    if (dexp(a) == 0x7FF || dexp(b) == 0x7FF) return 2;   /* unordered */
    int sa = dsign(a), sb = dsign(b);
    if (sa != sb) return sa > sb ? -1 : 1;
    if (a == b) return 0;
    /* Bit order matches value order for positives, but is reversed for
     * negatives: -1.0 has a smaller bit pattern than -2.0 yet is greater. */
    if (sa) return a > b ? -1 : 1;
    return a > b ? 1 : -1;
}

/* Every unordered comparison is false, so dcmp's "unordered" must not be read
 * as "greater". */
int __ltdf2(double a, double b) { int r = dcmp(dbits(a), dbits(b)); return r == -1; }
int __gtdf2(double a, double b) { int r = dcmp(dbits(a), dbits(b)); return r == 1; }
int __ledf2(double a, double b) { int r = dcmp(dbits(a), dbits(b)); return r == -1 || r == 0; }
int __gedf2(double a, double b) { int r = dcmp(dbits(a), dbits(b)); return r == 1 || r == 0; }

/* ---- conversions ---- */

double __extendsfdf2(float f)
{
    uint32_t u = fbits(f);
    int s = (int)(u >> 31), e = (int)((u >> 23) & 0xFF);
    uint32_t m = u & 0x7FFFFF;

    if (e == 0xFF) return dval(((uint64_t)s << 63) | DINF | (m ? ((uint64_t)m << 29) : 0));
    if (e == 0) {
        if (m == 0) return dval((uint64_t)s << 63);        /* true zero */
        int k = 0;
        while (!(m & 0x800000)) { m <<= 1; k++; }
        return dpack(s, 897 - k, (uint64_t)m << 29, 0, 0);
    }
    return dpack(s, e + 896, DHID | ((uint64_t)m << 29), 0, 0);
}

float __truncdfsf2(double d)
{
    uint64_t u = dbits(d);
    int s = dsign(u), e = dexp(u);
    uint64_t m = u & DMANT_MASK;

    if (e == 0x7FF) return fval(((uint32_t)s << 31) | (0xFFu << 23) |
                                (m ? (uint32_t)(m >> 29) : 0));
    if (e == 0) {
        if (m == 0) return fval((uint32_t)s << 31);
        int k = 0;
        while (!(m & DHID)) { m <<= 1; k++; }
        e = 1 - k;
    } else {
        m |= DHID;
    }

    int fe = e - 896;                       /* double biased exp -> float biased exp */
    if (fe > 254) return fval(((uint32_t)s << 31) | (0xFFu << 23));

    uint32_t fm = (uint32_t)(m >> 29);
    int hi = (int)((m >> 28) & 1);
    int any = (int)((m & ((1ULL << 28) - 1)) != 0);
    if (hi && (any || (fm & 1))) fm++;
    if (fm >> 24) { fm >>= 1; fe++; if (fe > 254) return fval(((uint32_t)s << 31) | (0xFFu << 23)); }

    if (fe <= 0) {
        int sh = 1 - fe;
        if (sh > 24) return fval((uint32_t)s << 31);
        return fval(((uint32_t)s << 31) | (fm >> sh));
    }
    return fval(((uint32_t)s << 31) | ((uint32_t)fe << 23) | (fm & 0x7FFFFF));
}

static double from_int(uint64_t m, int sign, int msb)
{
    if (m == 0) return dval(0);
    return dpack(sign, 1023 + msb, m << (52 - msb), 0, 0);
}

double __floatsidf(int32_t v)
{
    int s = v < 0 ? 1 : 0;
    uint64_t m = (uint64_t)(v < 0 ? -(int64_t)v : v);
    return from_int(m, s, msb64(m));
}

double __floatunsidf(uint32_t v)
{
    return from_int(v, 0, msb64(v));
}

double __floatdidf(int64_t v)
{
    if (v == 0) return dval(0);
    int s = v < 0 ? 1 : 0;
    uint64_t m = (uint64_t)(v < 0 ? (uint64_t)(-v) : v);
    int p = msb64(m);

    if (p <= 52) return from_int(m, s, p);

    uint64_t mant = m >> (p - 52);
    int hi = (int)((m >> (p - 53)) & 1);
    int any = (int)((m & ((1ULL << (p - 53)) - 1)) != 0);
    return dpack(s, 1023 + p, mant, hi, any);
}

float __floatdisf(int64_t v)
{
    if (v == 0) return fval(0);
    int s = v < 0 ? 1 : 0;
    uint64_t m = (uint64_t)(v < 0 ? (uint64_t)(-v) : v);
    int p = msb64(m);

    int e = 127 + p;
    if (e > 254) return fval(((uint32_t)s << 31) | (0xFFu << 23));

    uint32_t fm = (uint32_t)(m >> (p - 23));
    int hi = (int)((m >> (p - 24)) & 1);
    int any = (int)((m & ((1ULL << (p - 24)) - 1)) != 0);
    if (hi && (any || (fm & 1))) fm++;
    if (fm >> 24) { fm >>= 1; e++; if (e > 254) return fval(((uint32_t)s << 31) | (0xFFu << 23)); }
    return fval(((uint32_t)s << 31) | ((uint32_t)e << 23) | (fm & 0x7FFFFF));
}

static int64_t to_int(uint64_t u, int wide)
{
    int s = dsign(u), e = dexp(u);
    if (e == 0x7FF) return 0;
    uint64_t m = u & DMANT_MASK;
    if (e == 0) return 0;
    m |= DHID;

    int sh = e - 1023;
    if (sh < 0) return 0;
    if (sh > 62) return 0;

    /* sh can be larger than the mantissa width, so the shift can go either way. */
    uint64_t v = (sh >= 52) ? (m << (sh - 52)) : (m >> (52 - sh));
    if (!wide && sh > 30) return 0;
    return (int64_t)(s ? -(int64_t)v : v);
}

int32_t __fixdfsi(double d) { return (int32_t)to_int(dbits(d), 0); }
int64_t __fixdfdi(double d) { return to_int(dbits(d), 1); }
uint32_t __fixunsdfsi(double d) { return (uint32_t)to_int(dbits(d), 1); }

int64_t __fixsfdi(float f)
{
    uint32_t u = fbits(f);
    int s = (int)(u >> 31), e = (int)((u >> 23) & 0xFF);
    if (e == 0xFF) return 0;
    uint32_t m = u & 0x7FFFFF;
    if (e == 0) return 0;
    uint64_t mm = (uint64_t)m | 0x800000ULL;

    int sh = e - 127;
    if (sh < 0) return 0;
    if (sh > 62) return 0;

    uint64_t v = (sh >= 23) ? (mm << (sh - 23)) : (mm >> (23 - sh));
    return (int64_t)(s ? -(int64_t)v : v);
}

/* Single-precision division: the esp32s3 core has no div.s. */
float __divsf3(float a, float b)
{
    uint32_t ua = fbits(a), ub = fbits(b);
    int sa = (int)(ua >> 31), sb = (int)(ub >> 31);
    int ea = (int)((ua >> 23) & 0xFF), eb = (int)((ub >> 23) & 0xFF);

    if (ea == 0xFF) {
        if (ua & 0x7FFFFF) return fval(ua);
        if (eb == 0xFF && (ub & 0x7FFFFF)) return fval(ub);
        if (eb == 0xFF && sa != sb) return fval(((uint32_t)(sa ^ sb) << 31) | (0xFFu << 23) | (1u << 22));
        return fval(((uint32_t)(sa ^ sb) << 31) | (0xFFu << 23));
    }
    if (eb == 0xFF) {
        if (ub & 0x7FFFFF) return fval(ub);                       /* b is NaN */
        if (ea == 0xFF) return fval(((uint32_t)(sa ^ sb) << 31) | (0xFFu << 23) | (1u << 22));
        return fval((sa ^ sb) ? ((uint32_t)1 << 31) : 0);         /* finite / inf */
    }

    uint32_t ma = ua & 0x7FFFFF, mb = ub & 0x7FFFFF;
    if (ea) ma |= 0x800000;                 /* the hidden bit */
    if (eb) mb |= 0x800000;
    if (ma == 0) return fval((uint32_t)(sa ^ sb) << 31);
    if (mb == 0) return fval(((uint32_t)(sa ^ sb) << 31) | (0xFFu << 23) | (1u << 22));

    if (ea == 0) {
        int k = 0;
        while (!(ma & 0x800000)) { ma <<= 1; k++; }
        ea = 1 - k;
    } else ma |= 0x800000;

    if (eb == 0) {
        int k = 0;
        while (!(mb & 0x800000)) { mb <<= 1; k++; }
        eb = 1 - k;
    } else mb |= 0x800000;

    int exp = ea - eb;
    if (ma < mb) { ma <<= 1; exp--; }

    uint32_t r = ma, q = 0;
    for (int i = 23; i >= 0; i--) {
        if (r >= mb) { q |= 1u << i; r -= mb; }
        if (i) r <<= 1;
    }

    int E = exp + 127;
    if (E <= 0) {
        int sh = 1 - E;
        if (sh > 24) return fval((uint32_t)(sa ^ sb) << 31);
        uint32_t kept = q >> sh;
        uint32_t half = 1u << (sh - 1);
        uint32_t low = q & ((1u << sh) - 1);
        if (low > half || (low == half && (r != 0 || (kept & 1)))) kept++;
        return fval((uint32_t)(sa ^ sb) << 31 | kept);
    }

    uint32_t twice = r << 1;
    if (twice > mb || (twice == mb && (q & 1))) q++;

    if (q >> 24) { q >>= 1; exp++; }

    if (exp + 127 > 254) return fval(((uint32_t)(sa ^ sb) << 31) | (0xFFu << 23));
    return fval(((uint32_t)(sa ^ sb) << 31) | ((uint32_t)(exp + 127) << 23) | (q & 0x7FFFFF));
}
