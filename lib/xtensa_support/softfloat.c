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

static inline int dexp(uint64_t u)  { return (int)((u >> 52) & 0x7FF); }
static inline int dsign(uint64_t u) { return (int)(u >> 63); }
static int msb64(uint64_t m);

static int msb64(uint64_t m)
{
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

    if (lost_hi && (lost_any || (mant & 1))) {
        mant++;
        if (mant >> 53) { mant >>= 1; exp++; }
    }

    if (exp > 2046) return dval(((uint64_t)sign << 63) | DINF);
    if (exp <= 0) {
        int sh = 1 - exp;
        if (sh > 53) return dval((uint64_t)sign << 63);
        return dval(((uint64_t)sign << 63) | (mant >> sh));
    }
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
            if (dsign(ba) != dsign(bb)) return dval(DINF | DHID);
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

    int ha = 0, aa = 0, hb = 0, ab = 0;

    if (ea2 < eb2) {
        int d = eb2 - ea2;
        if (d > 53) { ma = 0; ha = 1; }
        else if (d) {
            ha = (int)((ma >> (d - 1)) & 1);
            aa = (int)((ma & ((1ULL << (d - 1)) - 1)) != 0);
            ma >>= d;
        }
    } else if (eb2 < ea2) {
        int d = ea2 - eb2;
        if (d > 53) { mb = 0; hb = 1; }
        else if (d) {
            hb = (int)((mb >> (d - 1)) & 1);
            ab = (int)((mb & ((1ULL << (d - 1)) - 1)) != 0);
            mb >>= d;
        }
    }

    int exp = ea2 < eb2 ? eb2 : ea2;

    if (sa == sb) {
        uint64_t sum = ma + mb;
        int hi = ha | hb, any = aa | ab;
        if (sum >> 53) {
            hi = (int)(sum & 1) | hi;
            sum >>= 1;
            exp++;
        }
        return dpack(sa, exp, sum, hi, any);
    }

    /* Opposite signs: subtract the smaller mantissa from the larger. */
    if (ma < mb) {
        uint64_t t = ma; ma = mb; mb = t;
        int s = sa; sa = sb; sb = s;
        int h = ha; ha = hb; hb = h;
        int a2 = aa; aa = ab; ab = a2;
    }
    uint64_t diff = ma - mb;
    if (diff == 0) return dval(0);
    while (!(diff & DHID)) { diff <<= 1; exp--; }
    return dpack(sa, exp, diff, ha | hb, aa | ab);
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
        if (eb == 0x7FF && dsign(ua) != dsign(ub)) return dval(DINF | DHID);
        return dval(((uint64_t)(dsign(ua) ^ dsign(ub)) << 63) | DINF);
    }
    if (eb == 0x7FF) {
        if (ub & DMANT_MASK) return dval(ub);
        return dval(((uint64_t)(dsign(ua) ^ dsign(ub)) << 63) | DINF);
    }

    int sa, sb, ea2, eb2;
    uint64_t ma, mb;
    dsplit(ua, &sa, &ea2, &ma);
    dsplit(ub, &sb, &eb2, &mb);
    if (ma == 0 || mb == 0) return dval((uint64_t)(sa ^ sb) << 63);

    uint64_t hi, lo;
    mul53(ma, mb, &hi, &lo);

    /* hi holds the top of the 106-bit product; normalise it to 53 bits and
     * treat all of lo as lost. */
    int sh = 52 - msb64(hi);
    uint64_t mant = hi << sh;
    int lost_hi = (int)((lo >> 63) & 1);
    int lost_any = (int)((lo & ((1ULL << 63) - 1)) != 0);

    return dpack(sa ^ sb, ea2 + eb2 - 1011 - sh, mant, lost_hi, lost_any);
}

double __divdf3(double a, double b)
{
    uint64_t ua = dbits(a), ub = dbits(b);
    int ea = dexp(ua), eb = dexp(ub);

    if (ea == 0x7FF) {
        if (ua & DMANT_MASK) return dval(ua);
        if (eb == 0x7FF && (ub & DMANT_MASK)) return dval(ub);
        return dval(DINF | DHID);                       /* inf / inf */
    }
    if (eb == 0x7FF) return dval((dsign(ua) ^ dsign(ub)) ? (1ULL << 63) : 0);
    if (ub & DMANT_MASK) return dval(ub);

    int sa, sb, ea2, eb2;
    uint64_t ma, mb;
    dsplit(ua, &sa, &ea2, &ma);
    dsplit(ub, &sb, &eb2, &mb);

    if (mb == 0) return dval((uint64_t)(sa ^ sb) << 63 | DINF);
    if (ma == 0) return dval((uint64_t)(sa ^ sb) << 63);

    int exp = ea2 - eb2;
    if (ma < mb) { ma <<= 1; exp--; }

    uint64_t r = ma, q = 0;
    for (int i = 52; i >= 0; i--) {
        if (r >= mb) { q |= 1ULL << i; r -= mb; }
        if (i) r <<= 1;
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
    return a > b ? 1 : -1;      /* same sign: bit order is value order */
}

int __ltdf2(double a, double b) { return dcmp(dbits(a), dbits(b)) < 0; }
int __gtdf2(double a, double b) { return dcmp(dbits(a), dbits(b)) > 0; }
int __ledf2(double a, double b) { int r = dcmp(dbits(a), dbits(b)); return r < 0 || r == 0; }
int __gedf2(double a, double b) { int r = dcmp(dbits(a), dbits(b)); return r > 0 || r == 0; }

/* ---- conversions ---- */

double __extendsfdf2(float f)
{
    uint32_t u = fbits(f);
    int s = (int)(u >> 31), e = (int)((u >> 23) & 0xFF);
    uint32_t m = u & 0x7FFFFF;

    if (e == 0xFF) return dval(((uint64_t)s << 63) | DINF | (m ? ((uint64_t)m << 29) : 0));
    if (m == 0) return dval((uint64_t)s << 63);

    int k = 0;
    if (e == 0) {
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
                                (m ? (uint32_t)(m >> 29) : 0x800000u));
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
    if (fm >> 23) { fm >>= 1; fe++; if (fe > 254) return fval(((uint32_t)s << 31) | (0xFFu << 23)); }

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
    if (fm >> 23) { fm >>= 1; e++; if (e > 254) return fval(((uint32_t)s << 31) | (0xFFu << 23)); }
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

    uint64_t v = m >> (52 - sh);
    if (!wide && sh > 30) return 0;
    return (int64_t)(s ? -(int64_t)v : v);
}

int32_t __fixdfsi(double d) { return (int32_t)to_int(dbits(d), 0); }
int64_t __fixdfdi(double d) { return to_int(dbits(d), 1); }

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

    uint64_t v = mm >> (23 - sh);
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
        if (eb == 0xFF && sa != sb) return fval(((uint32_t)(sa ^ sb) << 31) | (0xFFu << 23) | 0x800000u);
        return fval(((uint32_t)(sa ^ sb) << 31) | (0xFFu << 23));
    }
    if (eb == 0xFF) return fval((sa ^ sb) ? ((uint32_t)1 << 31) : 0);
    if (ub & 0x7FFFFF) return fval(ub);

    uint32_t ma = ua & 0x7FFFFF, mb = ub & 0x7FFFFF;
    if (ma == 0) return fval((uint32_t)(sa ^ sb) << 31);
    if (mb == 0) return fval(((uint32_t)(sa ^ sb) << 31) | (0xFFu << 23) | 0x800000u);

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

    uint32_t twice = r << 1;
    if (twice > mb || (twice == mb && (q & 1))) q++;

    if (q >> 24) { q >>= 1; exp++; }

    int E = exp + 127;
    if (E > 254) return fval(((uint32_t)(sa ^ sb) << 31) | (0xFFu << 23));
    if (E <= 0) {
        int sh = 1 - E;
        if (sh > 24) return fval((uint32_t)(sa ^ sb) << 31);
        return fval(((uint32_t)(sa ^ sb) << 31) | (q >> sh));
    }
    return fval(((uint32_t)(sa ^ sb) << 31) | ((uint32_t)E << 23) | (q & 0x7FFFFF));
}
