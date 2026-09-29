/* Decimals, read and written exactly.
 *
 * A number in a script is a double and the text of one is decimal, and the
 * two do not meet: 0.1 is not a double, only the double nearest to it is.
 * Reading has to find that nearest double every time, and writing has to
 * find the shortest decimal that reads back as the same double, which is what
 * the standard asks for and what every other engine prints.
 *
 * Both used to be done in doubles. A literal was read a digit at a time
 * against a scale that was itself rounded, so 0.3 came out as the double
 * above it; and a number was written as fifteen digits, rounded, which hid
 * that, and hid everything else too: 0.1 + 0.2 printed as 0.3, and a script
 * was told the two were different numbers while being shown the same one.
 *
 * Now the hard cases are done in whole numbers of whatever size they need,
 * the way a person would do them on paper, and the easy ones -- fifteen
 * digits or fewer, and a power of ten a double holds exactly -- still take
 * one multiplication or one division, which is exact because both sides are
 * exact and the operation rounds once.
 */
#pragma once
#include "zelr.h"

/* --- whole numbers of any size, as far as a double needs ------------------
 *
 * Thirty two bit limbs, least significant first. 4096 bits: the widest thing
 * made here is a power of ten for the smallest decimal worth reading, a
 * thousand places or so, with sixty eight bits of room to divide into. */
#define JN_LIMBS  128
#define JN_DIGITS 780    /* digits kept; past these, only whether any is not 0 */

typedef struct { u32 w[JN_LIMBS]; int n; } jbig;

static const double JN_POW10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

static void jb_set(jbig *b, u64 v) {
    b->n = 0;
    while (v) { b->w[b->n++] = (u32)v; v >>= 32; }
}

static void jb_trim(jbig *b) {
    while (b->n && !b->w[b->n - 1]) b->n--;
}

static void jb_mul(jbig *b, u32 m) {
    u64 carry = 0;
    for (int i = 0; i < b->n; i++) {
        u64 t = (u64)b->w[i] * m + carry;
        b->w[i] = (u32)t;
        carry = t >> 32;
    }
    if (carry && b->n < JN_LIMBS) b->w[b->n++] = (u32)carry;
}

static void jb_add(jbig *b, u32 a) {
    u64 carry = a;
    for (int i = 0; i < b->n && carry; i++) {
        u64 t = (u64)b->w[i] + carry;
        b->w[i] = (u32)t;
        carry = t >> 32;
    }
    if (carry && b->n < JN_LIMBS) b->w[b->n++] = (u32)carry;
}

/* b times base to the k, base five or ten, in as few passes as fit a limb. */
static void jb_mul_pow(jbig *b, u32 base, int k) {
    u32 big = base == 5 ? 1220703125u : 1000000000u;   /* 5^13, 10^9 */
    int step = base == 5 ? 13 : 9;
    while (k >= step) { jb_mul(b, big); k -= step; }
    u32 r = 1;
    while (k-- > 0) r *= base;
    if (r > 1) jb_mul(b, r);
}

static void jb_shl(jbig *b, int s) {
    if (!b->n || s <= 0) return;
    int words = s / 32, bits = s % 32;
    int n = b->n + words + 1;
    if (n > JN_LIMBS) n = JN_LIMBS;
    for (int i = n - 1; i >= 0; i--) {
        int src = i - words;
        u32 hi = (src >= 0 && src < b->n) ? b->w[src] : 0;
        u32 lo = (src >= 1 && src - 1 < b->n) ? b->w[src - 1] : 0;
        b->w[i] = bits ? (hi << bits) | (lo >> (32 - bits)) : hi;
    }
    b->n = n;
    jb_trim(b);
}

static void jb_shr1(jbig *b) {
    for (int i = 0; i < b->n; i++)
        b->w[i] = (b->w[i] >> 1) | (i + 1 < b->n ? b->w[i + 1] << 31 : 0);
    jb_trim(b);
}

static int jb_bits(const jbig *b) {
    if (!b->n) return 0;
    u32 t = b->w[b->n - 1];
    int k = 0;
    while (t) { k++; t >>= 1; }
    return (b->n - 1) * 32 + k;
}

static int jb_cmp(const jbig *a, const jbig *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

/* a minus b, where a is not the smaller. */
static void jb_sub(jbig *a, const jbig *b) {
    u32 borrow = 0;
    for (int i = 0; i < a->n; i++) {
        u64 take = (u64)(i < b->n ? b->w[i] : 0) + borrow;
        u64 have = a->w[i];
        if (have >= take) { a->w[i] = (u32)(have - take); borrow = 0; }
        else { a->w[i] = (u32)(have + 0x100000000ull - take); borrow = 1; }
    }
    jb_trim(a);
}

static u32 jb_div_small(jbig *b, u32 d) {
    u64 rem = 0;
    for (int i = b->n - 1; i >= 0; i--) {
        u64 cur = (rem << 32) | b->w[i];
        b->w[i] = (u32)(cur / d);
        rem = cur % d;
    }
    jb_trim(b);
    return (u32)rem;
}

/* The top sixty four bits of b, its highest set bit at the top, and in
   *sticky whether anything below them is not zero. */
static u64 jb_top(const jbig *b, int *sticky) {
    int len = jb_bits(b);
    u64 r = 0;
    for (int k = 0; k < 64; k++) {
        int bit = len - 1 - k;
        r <<= 1;
        if (bit >= 0 && ((b->w[bit / 32] >> (bit % 32)) & 1)) r |= 1;
    }
    *sticky = 0;
    int low = len - 64;
    if (low > 0) {
        int wi = low / 32, bi = low % 32;
        for (int i = 0; i < wi && !*sticky; i++) if (b->w[i]) *sticky = 1;
        if (!*sticky && bi && (b->w[wi] & ((1u << bi) - 1))) *sticky = 1;
    }
    return r;
}

/* --- to a double ------------------------------------------------------- */

static double jn_bits(u64 bits) {
    union { u64 u; double d; } x;
    x.u = bits;
    return x.d;
}

#define JN_INF 0x7FF0000000000000ull

/* The double nearest hi times two to the e, where hi has its top bit set and
   `sticky` says whether something smaller than its last bit was cut off.
   Ties go to the even one, which is the rule every reader keeps. Past the
   largest double is infinity, and below half the smallest is nought. */
static double jn_round(u64 hi, int e, int sticky) {
    int top = e + 63;                   /* hi * 2^e is in [2^top, 2^(top+1)) */
    if (top > 1023) return jn_bits(JN_INF);
    int shift = 11;                     /* the bits of hi below the last kept */
    if (top < -1022) {
        shift += -1022 - top;           /* a subnormal keeps fewer */
        if (shift > 64) return 0.0;
    }
    u64 keep = shift == 64 ? 0 : hi >> shift;
    u64 rest = shift == 64 ? hi : hi & ((1ull << shift) - 1);
    u64 half = 1ull << (shift - 1);
    if (rest > half || (rest == half && (sticky || (keep & 1)))) keep++;

    if (top < -1022) return jn_bits(keep);   /* rounding up into the normals is right as it is */
    if (keep == (1ull << 53)) {
        keep >>= 1;
        if (++top > 1023) return jn_bits(JN_INF);
    }
    return jn_bits(((u64)(top + 1023) << 52) | (keep & 0xFFFFFFFFFFFFFull));
}

/* The double nearest the decimal whose digits are dg[0..nd) with the last of
   them worth ten to the exp10, plus a little more if `sticky`: digits were
   dropped past what is kept, and not all of them were nought. */
static double jn_from_digits(const char *dg, int nd, int exp10, int sticky) {
    while (nd > 0 && dg[nd - 1] == '0') { nd--; exp10++; }
    if (nd == 0) return 0.0;
    int top = nd + exp10;               /* the decimal is below 10^top */
    if (top > 310) return jn_bits(JN_INF);
    if (top < -324) return 0.0;

    if (nd <= 15 && !sticky && exp10 >= -22 && exp10 <= 22) {
        u64 v = 0;
        for (int i = 0; i < nd; i++) v = v * 10 + (u64)(dg[i] - '0');
        double x = (double)v;
        return exp10 >= 0 ? x * JN_POW10[exp10] : x / JN_POW10[-exp10];
    }

    jbig D;
    jb_set(&D, 0);
    for (int i = 0; i < nd; ) {
        u32 chunk = 0, scale = 1;
        for (int k = 0; k < 9 && i < nd; k++, i++) {
            chunk = chunk * 10 + (u32)(dg[i] - '0');
            scale *= 10;
        }
        jb_mul(&D, scale);
        jb_add(&D, chunk);
    }

    int st;
    if (exp10 >= 0) {
        jb_mul_pow(&D, 10, exp10);
        u64 hi = jb_top(&D, &st);
        return jn_round(hi, jb_bits(&D) - 64, st | sticky);
    }

    /* A division, done the long way a bit at a time: D over ten to the
       -exp10, with D shifted first so that the quotient has sixty six or
       sixty seven bits -- enough to round from, with what is left over as
       the one bit more that says whether it was exact. */
    jbig P, Q;
    jb_set(&P, 1);
    jb_mul_pow(&P, 10, -exp10);
    int s = jb_bits(&P) - jb_bits(&D) + 66;
    if (s >= 0) jb_shl(&D, s); else jb_shl(&P, -s);
    jb_set(&Q, 0);
    jb_shl(&P, 68);
    for (int b = 68; b >= 0; b--) {
        jb_shl(&Q, 1);
        if (jb_cmp(&D, &P) >= 0) { jb_sub(&D, &P); jb_add(&Q, 1); }
        jb_shr1(&P);
    }
    u64 hi = jb_top(&Q, &st);
    return jn_round(hi, jb_bits(&Q) - 64 - s, st | sticky | (D.n != 0));
}

/* A decimal at the start of s: digits, a point and more digits, and an
   exponent, any part but the digits optional and at least one digit
   somewhere. No sign: the callers each have their own rules for one. The
   double nearest it, and in *used how much of s it was, nought if none. */
static double js_decimal(const char *s, u32 n, u32 *used) {
    char dg[JN_DIGITS];
    int nd = 0, exp10 = 0, sticky = 0, any = 0, lead = 1;
    u32 i = 0;

    while (i < n && s[i] >= '0' && s[i] <= '9') {
        any = 1;
        if (!(lead && s[i] == '0')) {
            lead = 0;
            if (nd < JN_DIGITS) dg[nd++] = s[i];
            else { exp10++; if (s[i] != '0') sticky = 1; }
        }
        i++;
    }
    if (i < n && s[i] == '.') {
        u32 at = i + 1;
        int more = 0;
        while (at < n && s[at] >= '0' && s[at] <= '9') {
            more = 1;
            if (lead && s[at] == '0') exp10--;
            else {
                lead = 0;
                if (nd < JN_DIGITS) { dg[nd++] = s[at]; exp10--; }
                else if (s[at] != '0') sticky = 1;
            }
            at++;
        }
        if (any || more) { any = 1; i = at; }
    }
    if (!any) { *used = 0; return 0.0; }

    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        u32 at = i + 1;
        int neg = 0;
        if (at < n && (s[at] == '+' || s[at] == '-')) neg = s[at++] == '-';
        if (at < n && s[at] >= '0' && s[at] <= '9') {
            int e = 0;
            while (at < n && s[at] >= '0' && s[at] <= '9') {
                if (e < 100000) e = e * 10 + (s[at] - '0');
                at++;
            }
            exp10 += neg ? -e : e;
            i = at;
        }
    }
    *used = i;
    return jn_from_digits(dg, nd, exp10, sticky);
}

/* --- from a double ----------------------------------------------------- */

/* Adds one to the last of p digits. Returns 1 if that carried out of the
   first, in which case they are now a one and noughts. */
static int jn_bump(char *c, int p) {
    for (int i = p - 1; i >= 0; i--) {
        if (c[i] < '9') { c[i]++; return 0; }
        c[i] = '0';
    }
    c[0] = '1';
    return 1;
}

/* The shortest digits that read back as d, which is finite and above nought,
   with no noughts at the end; how many; and in *exp10 the power of ten of the
   first. Where two decimals that short both read back, the nearer.
 *
 * Every digit of d is worked out first -- a double is a whole number times a
 * power of two, and a power of two below one is a power of five over a power
 * of ten, so there are at most seven hundred and seventy of them -- and then
 * cut to one digit, two, and so on, each time rounded to the nearer, until
 * what is left reads back the same. The one further away is tried as well,
 * for the doubles just above a power of two, whose neighbour below is closer
 * than their neighbour above. */
static int js_shortest(double d, char *out, int *exp10) {
    union { double d; u64 u; } x;
    x.d = d;
    int ef = (int)((x.u >> 52) & 0x7FF);
    u64 m = x.u & 0xFFFFFFFFFFFFFull;
    int e2;
    if (ef) { m |= 1ull << 52; e2 = ef - 1075; }
    else e2 = -1074;

    jbig M;
    jb_set(&M, m);
    int last;                           /* the power of ten of the last digit */
    if (e2 >= 0) { jb_shl(&M, e2); last = 0; }
    else { jb_mul_pow(&M, 5, -e2); last = e2; }

    char rev[800];
    int len = 0;
    while (M.n && len + 9 <= (int)sizeof(rev)) {
        u32 r = jb_div_small(&M, 1000000000u);
        for (int k = 0; k < 9; k++) { rev[len++] = (char)('0' + r % 10); r /= 10; }
    }
    while (len > 1 && rev[len - 1] == '0') len--;
    char all[800];
    for (int i = 0; i < len; i++) all[i] = rev[len - 1 - i];
    int first = last + len - 1;         /* the power of ten of all[0] */

    char c[18], alt[18];
    int p = 1;
    for (; p <= 17 && p < len; p++) {
        for (int i = 0; i < p; i++) c[i] = alt[i] = all[i];
        int rest = 0;
        for (int i = p + 1; i < len; i++) if (all[i] != '0') { rest = 1; break; }
        char next = all[p];
        int up = next > '5' || (next == '5' && (rest || ((c[p - 1] - '0') & 1)));
        int exact = next == '0' && !rest;

        int ce = first, ae = first;
        if (up) { if (jn_bump(c, p)) ce++; }
        else if (!exact) { if (jn_bump(alt, p)) ae++; }

        if (jn_from_digits(c, p, ce - p + 1, 0) == d) {
            while (p > 1 && c[p - 1] == '0') p--;
            for (int i = 0; i < p; i++) out[i] = c[i];
            *exp10 = ce;
            return p;
        }
        if (!exact && jn_from_digits(alt, p, ae - p + 1, 0) == d) {
            while (p > 1 && alt[p - 1] == '0') p--;
            for (int i = 0; i < p; i++) out[i] = alt[i];
            *exp10 = ae;
            return p;
        }
    }

    /* Every digit there is, which is the number exactly. */
    int n = len < 17 ? len : 17;
    while (n > 1 && all[n - 1] == '0') n--;
    for (int i = 0; i < n; i++) out[i] = all[i];
    *exp10 = first;
    return n;
}

/* --- numbers as text -----------------------------------------------------
 *
 * The shortest decimal that reads back as the same double, which is what the
 * standard asks for: 0.1 + 0.2 is 0.30000000000000004, because that is the
 * number it is, and 0.3 is 0.3. The digits come from js_shortest; this is
 * only where the point goes. Here rather than with the evaluator because the
 * parser needs it too: a number written as a property's name is the text of
 * the number, {1.5: x} is the key "1.5".
 *
 * It printed fifteen digits, rounded, which made 0.1 + 0.2 look like 0.3
 * to a script that was being told the two were different. Before that it
 * cut after ten places rather than rounding, and 0.57 came out as
 * 0.5699999999 on every page that showed a price.
 */
static u32 js_num_text(double d, char *out, u32 cap) {
    u32 w = 0;
    if (cap < 32) { if (cap) out[0] = 0; return 0; }

    /* Not a number and the infinities, which compare false against
       themselves and against everything else. */
    if (d != d) {
        const char *s = "NaN";
        while (*s) out[w++] = *s++;
        out[w] = 0;
        return w;
    }
    if ((d - d) != (d - d)) {                    /* infinite: inf - inf is NaN */
        const char *s = d < 0 ? "-Infinity" : "Infinity";
        while (*s) out[w++] = *s++;
        out[w] = 0;
        return w;
    }

    int neg = d < 0;
    if (neg) d = -d;

    /* Whole, and small enough that a double holds it exactly. */
    if (d < 9007199254740992.0 && d == (double)(long long)d) {
        long long v = (long long)d;
        char rev[24];
        int r = 0;
        if (!v) rev[r++] = '0';
        while (v) { rev[r++] = (char)('0' + (int)(v % 10)); v /= 10; }
        if (neg) out[w++] = '-';
        while (r) out[w++] = rev[--r];
        out[w] = 0;
        return w;
    }

    char dig[18];
    int e = 0;
    int n = js_shortest(d, dig, &e);

    if (neg) out[w++] = '-';

    /* Very large or very small, where a plain decimal would be mostly
       zeros: one digit, the rest after a point, and the power of ten. */
    if (e >= 21 || e < -6) {
        out[w++] = dig[0];
        if (n > 1) {
            out[w++] = '.';
            for (int i = 1; i < n; i++) out[w++] = dig[i];
        }
        out[w++] = 'e';
        if (e < 0) { out[w++] = '-'; e = -e; } else out[w++] = '+';
        char rev[8];
        int r = 0;
        if (!e) rev[r++] = '0';
        while (e) { rev[r++] = (char)('0' + e % 10); e /= 10; }
        while (r) out[w++] = rev[--r];
        out[w] = 0;
        return w;
    }

    if (e >= 0) {
        /* e + 1 digits before the point, padded with zeros if the number
           ran out of significant ones first. */
        for (int i = 0; i <= e; i++) out[w++] = i < n ? dig[i] : '0';
        if (n > e + 1) {
            out[w++] = '.';
            for (int i = e + 1; i < n; i++) out[w++] = dig[i];
        }
    } else {
        out[w++] = '0';
        out[w++] = '.';
        for (int i = 0; i < -e - 1; i++) out[w++] = '0';
        for (int i = 0; i < n; i++) out[w++] = dig[i];
    }
    out[w] = 0;
    return w;
}
