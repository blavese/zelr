/* The arithmetic that is not one instruction.
 *
 * Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every one of these is worked out here: a series, an iteration or a
 * reduction written down, with the range reduction that makes the series
 * converge. Nothing is looked up from a table somebody else computed, which
 * is the rule for this whole project and is also why the list is short -- a
 * function that has not been written is not declared.
 *
 * Accuracy is about twelve significant figures, which is short of the
 * fifteen a double can hold. That is a limitation of the method rather than
 * a bug: getting the last three would mean argument reduction in higher
 * precision than the type being reduced.
 */
#include <math.h>

int isnan(double x) { return __builtin_isnan(x); }
int isinf(double x) { return __builtin_isinf(x); }

double fabs(double x) { return x < 0 ? -x : x; }

double trunc(double x) {
    if (isnan(x) || isinf(x)) return x;
    if (fabs(x) >= 9.007199254740992e15) return x;   /* already whole */
    long long w = (long long)x;
    return (double)w;
}

double floor(double x) {
    double t = trunc(x);
    return (x < 0 && t != x) ? t - 1 : t;
}

double ceil(double x) {
    double t = trunc(x);
    return (x > 0 && t != x) ? t + 1 : t;
}

double round(double x) {
    return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5);
}

double fmod(double a, double b) {
    if (b == 0 || isnan(a) || isnan(b) || isinf(a)) return NAN;
    if (isinf(b)) return a;
    double q = trunc(a / b);
    return a - q * b;
}

/* Newton's method, which doubles the number of correct digits each time, so
   five rounds from a rough start is more than a double can hold. The rough
   start is the exponent halved, done by hand on the bits: starting from 1.0
   for a number near 10^300 would take hundreds of rounds. */
double sqrt(double x) {
    if (isnan(x) || x < 0) return NAN;
    if (x == 0 || isinf(x)) return x;

    union { double d; unsigned long long u; } bits;
    bits.d = x;
    long long e = (long long)((bits.u >> 52) & 0x7FF) - 1023;
    bits.u = (bits.u & 0x000FFFFFFFFFFFFFULL) | ((unsigned long long)1023 << 52);
    double m = bits.d;                       /* in [1, 2) */

    double guess = 1 + (m - 1) / 2;          /* good to a few per cent */
    for (int i = 0; i < 5; i++) guess = (guess + m / guess) / 2;

    /* And the exponent back, halved. An odd exponent leaves a factor of
       two behind, which goes into the mantissa's root. */
    if (e & 1) {
        guess *= 1.4142135623730951;
        e -= 1;
    }
    bits.d = guess;
    bits.u += (unsigned long long)(e / 2) << 52;
    return bits.d;
}

/* e^x by reducing to a fraction of ln 2 and using the Taylor series, which
   converges in a dozen terms once the argument is under one. */
double exp(double x) {
    if (isnan(x)) return x;
    if (x > 709.78) return HUGE_VAL;
    if (x < -745.0) return 0;

    double k = round(x / 0.6931471805599453);
    double r = x - k * 0.6931471805599453;    /* |r| <= ln2 / 2 */

    double term = 1, sum = 1;
    for (int i = 1; i < 18; i++) {
        term *= r / i;
        sum += term;
    }

    /* Times 2^k, by adding to the exponent rather than multiplying. */
    union { double d; unsigned long long u; } bits;
    bits.d = sum;
    long long e = (long long)((bits.u >> 52) & 0x7FF) + (long long)k;
    if (e <= 0) return 0;
    if (e >= 0x7FF) return HUGE_VAL;
    bits.u = (bits.u & ~((unsigned long long)0x7FF << 52))
           | ((unsigned long long)e << 52);
    return bits.d;
}

/* ln x, by taking the exponent out and using the series in (m-1)/(m+1),
   which converges quickly for a mantissa in [1, 2). */
double log(double x) {
    if (isnan(x)) return x;
    if (x < 0) return NAN;
    if (x == 0) return -HUGE_VAL;
    if (isinf(x)) return x;

    union { double d; unsigned long long u; } bits;
    bits.d = x;
    long long e = (long long)((bits.u >> 52) & 0x7FF) - 1023;
    bits.u = (bits.u & 0x000FFFFFFFFFFFFFULL) | ((unsigned long long)1023 << 52);
    double m = bits.d;

    /* Centre the mantissa on 1 rather than on 1.5, which halves the worst
       case the series has to cope with. */
    if (m > 1.4142135623730951) { m /= 2; e += 1; }

    double z = (m - 1) / (m + 1);
    double z2 = z * z;
    double sum = 0, p = z;
    for (int i = 1; i < 32; i += 2) {
        sum += p / i;
        p *= z2;
    }
    return 2 * sum + (double)e * 0.6931471805599453;
}

double log2(double x)  { return log(x) / 0.6931471805599453; }
double log10(double x) { return log(x) / 2.302585092994046; }

double pow(double base, double e) {
    if (e == 0) return 1;
    if (base == 0) return e > 0 ? 0 : HUGE_VAL;

    /* A whole exponent is repeated multiplication, which is exact where the
       logarithm would not be: 10^3 has to come out 1000 and not 999.999. */
    if (e == trunc(e) && fabs(e) < 1024) {
        double r = 1, b = base;
        long long n = (long long)(e < 0 ? -e : e);
        while (n) {
            if (n & 1) r *= b;
            b *= b;
            n >>= 1;
        }
        return e < 0 ? 1 / r : r;
    }

    if (base < 0) return NAN;           /* a fractional power of a negative */
    return exp(e * log(base));
}

/* sin and cos, reduced into [-pi/4, pi/4] where the Taylor series is worth
   using, and then chosen by which quarter of the circle it came from. */
static double sin_small(double x) {
    double x2 = x * x, term = x, sum = x;
    for (int i = 1; i < 8; i++) {
        term *= -x2 / ((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

static double cos_small(double x) {
    double x2 = x * x, term = 1, sum = 1;
    for (int i = 1; i < 8; i++) {
        term *= -x2 / ((2 * i - 1) * (2 * i));
        sum += term;
    }
    return sum;
}

static double circle(double x, int want_cos) {
    if (isnan(x) || isinf(x)) return NAN;

    double q = round(x / 1.5707963267948966);      /* quarter turns */
    double r = x - q * 1.5707963267948966;
    long long n = ((long long)q + (want_cos ? 1 : 0)) & 3;
    if (n < 0) n += 4;

    switch (n) {
        case 0:  return sin_small(r);
        case 1:  return cos_small(r);
        case 2:  return -sin_small(r);
        default: return -cos_small(r);
    }
}

double sin(double x) { return circle(x, 0); }
double cos(double x) { return circle(x, 1); }

double tan(double x) {
    double c = cos(x);
    if (c == 0) return HUGE_VAL;
    return sin(x) / c;
}

/* atan by the series in x/(1+x^2), which converges everywhere, with the
   argument folded into [0, 1] first because near 1 the plain series needs
   hundreds of terms. */
double atan(double x) {
    if (isnan(x)) return x;
    int neg = x < 0;
    if (neg) x = -x;

    int folded = 0;
    if (x > 1) { x = 1 / x; folded = 1; }

    double z = x / (1 + x * x);
    double z2 = z * z;
    double term = z, sum = z, num = 1, den = 1;
    for (int i = 1; i < 40; i++) {
        num *= 2.0 * i;
        den *= 2.0 * i + 1;
        term *= z2;
        sum += (num / den) * term;
    }

    if (folded) sum = 1.5707963267948966 - sum;
    return neg ? -sum : sum;
}

double atan2(double y, double x) {
    if (x == 0 && y == 0) return 0;
    if (x == 0) return y > 0 ? 1.5707963267948966 : -1.5707963267948966;
    double a = atan(y / x);
    if (x > 0) return a;
    return y >= 0 ? a + M_PI : a - M_PI;
}
