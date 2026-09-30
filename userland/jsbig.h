/* BigInt: whole numbers of any size. GitHub's and Spotify's scripts stopped
 * at "BigInt is not defined": protocol buffers keep 64-bit numbers in them,
 * and a 64-bit hash is multiplied and masked in them.
 *
 * A BigInt is a sign and a magnitude in 32-bit limbs, least significant
 * first, with no zero limb at the top (zero has none and no sign), made
 * once and never changed, in the context's region like a string. The
 * arithmetic is done on paper's methods: school multiplication, and Knuth's
 * long division (Hacker's Delight's divmnu) with a single-limb divisor done
 * short. The bitwise operators work on two's complement one limb wider than
 * either side, as the language defines them on numbers of unbounded width.
 * Mixing a BigInt with a Number in arithmetic is a TypeError, as it is in
 * every engine; comparing them is exact.
 *
 * A million bits is the most a BigInt may have here; past it, a RangeError. */
#pragma once

#define JSB_MAX_LIMBS 32768

static jval js_from_big(jbint *b) { jval v; v.t = JS_BIG; v.big = b; return v; }

static jbint *jsb_new(jctx *J, u32 n) {
    if (n > JSB_MAX_LIMBS) {
        js_throw(J, JS_ERR_RANGE, "a BigInt too large for this machine", J->error_line);
        return 0;
    }
    jbint *b = (jbint *)js_alloc(J, (u32)sizeof(jbint) + (n ? n - 1 : 0) * 4);
    if (!b) return 0;
    b->n = n;
    b->neg = 0;
    for (u32 i = 0; i < n; i++) b->d[i] = 0;
    return b;
}

static jbint *jsb_trim(jbint *b) {
    while (b->n && !b->d[b->n - 1]) b->n--;
    if (!b->n) b->neg = 0;
    return b;
}

static jbint *jsb_from_u64(jctx *J, u64 v, int neg) {
    jbint *b = jsb_new(J, 2);
    if (!b) return 0;
    b->d[0] = (u32)v;
    b->d[1] = (u32)(v >> 32);
    b->neg = (u32)neg;
    return jsb_trim(b);
}

static jbint *jsb_copy(jctx *J, const jbint *a, u32 room, int neg) {
    jbint *b = jsb_new(J, room > a->n ? room : a->n);
    if (!b) return 0;
    for (u32 i = 0; i < a->n; i++) b->d[i] = a->d[i];
    b->neg = (u32)neg;
    return b;
}

/* --- magnitudes -------------------------------------------------------------------- */

static int jsb_mag_cmp(const u32 *a, u32 na, const u32 *b, u32 nb) {
    if (na != nb) return na < nb ? -1 : 1;
    for (u32 i = na; i-- > 0; ) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* r has room for the longer and one more. */
static u32 jsb_mag_add(u32 *r, const u32 *a, u32 na, const u32 *b, u32 nb) {
    if (na < nb) { const u32 *t = a; a = b; b = t; u32 tn = na; na = nb; nb = tn; }
    u64 c = 0;
    u32 i = 0;
    for (; i < na; i++) {
        c += (u64)a[i] + (i < nb ? b[i] : 0);
        r[i] = (u32)c;
        c >>= 32;
    }
    if (c) r[i++] = (u32)c;
    return i;
}

/* a >= b; r has room for a. */
static u32 jsb_mag_sub(u32 *r, const u32 *a, u32 na, const u32 *b, u32 nb) {
    u32 borrow = 0;
    for (u32 i = 0; i < na; i++) {
        u64 t = (u64)a[i] - (i < nb ? b[i] : 0) - borrow;
        r[i] = (u32)t;
        borrow = (u32)(t >> 63);
    }
    while (na && !r[na - 1]) na--;
    return na;
}

/* r has room for na + nb and is not a or b. */
static u32 jsb_mag_mul(u32 *r, const u32 *a, u32 na, const u32 *b, u32 nb) {
    for (u32 i = 0; i < na + nb; i++) r[i] = 0;
    for (u32 i = 0; i < na; i++) {
        u64 c = 0;
        for (u32 j = 0; j < nb; j++) {
            c += (u64)a[i] * b[j] + r[i + j];
            r[i + j] = (u32)c;
            c >>= 32;
        }
        r[i + nb] = (u32)c;
    }
    u32 n = na + nb;
    while (n && !r[n - 1]) n--;
    return n;
}

/* a := a * m + add, in place, a having room for one more limb. */
static u32 jsb_mag_muladd(u32 *a, u32 na, u32 m, u32 add) {
    u64 c = add;
    for (u32 i = 0; i < na; i++) {
        c += (u64)a[i] * m;
        a[i] = (u32)c;
        c >>= 32;
    }
    if (c) a[na++] = (u32)c;
    return na;
}

/* a := a / d in place; the remainder back. */
static u32 jsb_mag_divsmall(u32 *a, u32 *na, u32 d) {
    u64 r = 0;
    for (u32 i = *na; i-- > 0; ) {
        u64 cur = (r << 32) | a[i];
        a[i] = (u32)(cur / d);
        r = cur % d;
    }
    while (*na && !a[*na - 1]) (*na)--;
    return (u32)r;
}

static int jsb_clz(u32 x) {
    int n = 0;
    if (!x) return 32;
    while (!(x & 0x80000000u)) { x <<= 1; n++; }
    return n;
}

/* q = u / v and r = u % v, with nu >= nv >= 2 and v's top limb not zero.
   q has room for nu - nv + 1, r for nv. Knuth's algorithm D. */
static int jsb_mag_divmod(u32 *q, u32 *nq, u32 *r, u32 *nr, const u32 *u, u32 nu, const u32 *v, u32 nv) {
    u32 *un = (u32 *)malloc((u64)(nu + 1) * 4), *vn = (u32 *)malloc((u64)nv * 4);
    if (!un || !vn) { free(un); free(vn); return 0; }
    int s = jsb_clz(v[nv - 1]);
    for (u32 i = nv - 1; i > 0; i--) vn[i] = s ? (v[i] << s) | (v[i - 1] >> (32 - s)) : v[i];
    vn[0] = v[0] << s;
    un[nu] = s ? u[nu - 1] >> (32 - s) : 0;
    for (u32 i = nu - 1; i > 0; i--) un[i] = s ? (u[i] << s) | (u[i - 1] >> (32 - s)) : u[i];
    un[0] = u[0] << s;
    const u64 base = 1ull << 32;
    for (int j = (int)(nu - nv); j >= 0; j--) {
        u64 num = ((u64)un[j + nv] << 32) | un[j + nv - 1];
        u64 qhat = num / vn[nv - 1], rhat = num % vn[nv - 1];
        while (qhat >= base || qhat * vn[nv - 2] > ((rhat << 32) | un[j + nv - 2])) {
            qhat--;
            rhat += vn[nv - 1];
            if (rhat >= base) break;
        }
        long long borrow = 0, t;
        for (u32 i = 0; i < nv; i++) {
            u64 p = qhat * vn[i];
            t = (long long)un[i + j] - borrow - (long long)(p & 0xFFFFFFFFull);
            un[i + j] = (u32)t;
            borrow = (long long)(p >> 32) - (t >> 32);
        }
        t = (long long)un[j + nv] - borrow;
        un[j + nv] = (u32)t;
        q[j] = (u32)qhat;
        if (t < 0) {
            q[j]--;
            u64 k = 0;
            for (u32 i = 0; i < nv; i++) {
                k += (u64)un[i + j] + vn[i];
                un[i + j] = (u32)k;
                k >>= 32;
            }
            un[j + nv] += (u32)k;
        }
    }
    for (u32 i = 0; i < nv; i++) r[i] = s ? (un[i] >> s) | (un[i + 1] << (32 - s)) : un[i];
    *nq = nu - nv + 1;
    while (*nq && !q[*nq - 1]) (*nq)--;
    *nr = nv;
    while (*nr && !r[*nr - 1]) (*nr)--;
    free(un);
    free(vn);
    return 1;
}

/* --- the operations --------------------------------------------------------------- */

static int jsb_cmp(const jbint *a, const jbint *b) {
    if (a->neg != b->neg) return a->neg ? -1 : 1;
    int c = jsb_mag_cmp(a->d, a->n, b->d, b->n);
    return a->neg ? -c : c;
}

/* a + b, or a - b when sub. */
static jbint *jsb_addsub(jctx *J, const jbint *a, const jbint *b, int sub) {
    u32 bneg = sub ? !b->neg && b->n : b->neg;
    u32 room = (a->n > b->n ? a->n : b->n) + 1;
    jbint *r = jsb_new(J, room);
    if (!r) return 0;
    if (a->neg == bneg) {
        r->n = jsb_mag_add(r->d, a->d, a->n, b->d, b->n);
        r->neg = a->neg;
    } else if (jsb_mag_cmp(a->d, a->n, b->d, b->n) >= 0) {
        r->n = jsb_mag_sub(r->d, a->d, a->n, b->d, b->n);
        r->neg = a->neg;
    } else {
        r->n = jsb_mag_sub(r->d, b->d, b->n, a->d, a->n);
        r->neg = bneg;
    }
    return jsb_trim(r);
}

static jbint *jsb_mul(jctx *J, const jbint *a, const jbint *b) {
    jbint *r = jsb_new(J, a->n + b->n);
    if (!r) return 0;
    r->n = jsb_mag_mul(r->d, a->d, a->n, b->d, b->n);
    r->neg = a->neg ^ b->neg;
    return jsb_trim(r);
}

/* Truncating division: the quotient's sign is both signs', the remainder's
   the dividend's. */
static int jsb_divmod(jctx *J, const jbint *a, const jbint *b, jbint **q, jbint **r) {
    if (!b->n) { js_throw(J, JS_ERR_RANGE, "division of a BigInt by zero", J->error_line); return 0; }
    if (jsb_mag_cmp(a->d, a->n, b->d, b->n) < 0) {
        if (q) *q = jsb_new(J, 0);
        if (r) *r = jsb_copy(J, a, 0, a->neg);
        return J->sig == JS_OK;
    }
    jbint *qq = jsb_new(J, a->n), *rr = jsb_new(J, b->n);
    if (!qq || !rr) return 0;
    if (b->n == 1) {
        for (u32 i = 0; i < a->n; i++) qq->d[i] = a->d[i];
        u32 nq = a->n;
        rr->d[0] = jsb_mag_divsmall(qq->d, &nq, b->d[0]);
        qq->n = nq;
        rr->n = 1;
    } else if (!jsb_mag_divmod(qq->d, &qq->n, rr->d, &rr->n, a->d, a->n, b->d, b->n)) {
        js_throw(J, JS_ERR_RANGE, "no memory to divide a BigInt", J->error_line);
        return 0;
    }
    qq->neg = a->neg ^ b->neg;
    rr->neg = a->neg;
    jsb_trim(qq);
    jsb_trim(rr);
    if (q) *q = qq;
    if (r) *r = rr;
    return 1;
}

static u32 jsb_bits(const jbint *a) { return a->n ? (a->n - 1) * 32 + (32 - (u32)jsb_clz(a->d[a->n - 1])) : 0; }

static jbint *jsb_pow(jctx *J, const jbint *a, const jbint *e) {
    if (e->neg) { js_throw(J, JS_ERR_RANGE, "a BigInt to a negative power", J->error_line); return 0; }
    jbint *one = jsb_from_u64(J, 1, 0);
    if (!e->n) return one;
    if (!a->n) return jsb_new(J, 0);
    if (a->n == 1 && a->d[0] == 1) {
        jbint *r = jsb_from_u64(J, 1, a->neg && (e->d[0] & 1));
        return r;
    }
    if (e->n > 1 || (double)jsb_bits(a) * e->d[0] > (double)JSB_MAX_LIMBS * 32) {
        js_throw(J, JS_ERR_RANGE, "a BigInt too large for this machine", J->error_line);
        return 0;
    }
    u32 k = e->d[0];
    jbint *res = one, *base = jsb_copy(J, a, 0, a->neg);
    while (res && base) {
        if (k & 1) res = jsb_mul(J, res, base);
        k >>= 1;
        if (!k) break;
        base = jsb_mul(J, base, base);
    }
    return res;
}

/* Two's complement in len limbs, len being more than the magnitude's. */
static void jsb_to_tc(const jbint *b, u32 *tc, u32 len) {
    for (u32 i = 0; i < len; i++) tc[i] = i < b->n ? b->d[i] : 0;
    if (b->neg) {
        u64 c = 1;
        for (u32 i = 0; i < len; i++) {
            c += (u32)~tc[i];
            tc[i] = (u32)c;
            c >>= 32;
        }
    }
}

static jbint *jsb_from_tc(jctx *J, const u32 *tc, u32 len) {
    jbint *r = jsb_new(J, len);
    if (!r) return 0;
    int neg = (int)(tc[len - 1] >> 31);
    for (u32 i = 0; i < len; i++) r->d[i] = tc[i];
    if (neg) {
        u64 c = 1;
        for (u32 i = 0; i < len; i++) {
            c += (u32)~r->d[i];
            r->d[i] = (u32)c;
            c >>= 32;
        }
        r->neg = 1;
    }
    return jsb_trim(r);
}

/* & | ^ as 0, 1, 2. */
static jbint *jsb_bitop(jctx *J, const jbint *a, const jbint *b, int op) {
    u32 len = (a->n > b->n ? a->n : b->n) + 1;
    u32 *x = (u32 *)malloc((u64)len * 8);
    if (!x) { js_throw(J, JS_ERR_RANGE, "no memory for a BigInt", J->error_line); return 0; }
    u32 *y = x + len;
    jsb_to_tc(a, x, len);
    jsb_to_tc(b, y, len);
    for (u32 i = 0; i < len; i++) x[i] = op == 0 ? x[i] & y[i] : op == 1 ? x[i] | y[i] : x[i] ^ y[i];
    jbint *r = jsb_from_tc(J, x, len);
    free(x);
    return r;
}

static jbint *jsb_shift_mag(jctx *J, const jbint *a, u32 k, int left) {
    u32 limbs = k / 32, bits = k % 32;
    if (left) {
        jbint *r = jsb_new(J, a->n + limbs + 1);
        if (!r) return 0;
        for (u32 i = 0; i < a->n; i++) {
            r->d[i + limbs] |= a->d[i] << bits;
            if (bits) r->d[i + limbs + 1] |= a->d[i] >> (32 - bits);
        }
        r->neg = a->neg;
        return jsb_trim(r);
    }
    if (limbs >= a->n) return jsb_new(J, 0);
    jbint *r = jsb_new(J, a->n - limbs);
    if (!r) return 0;
    for (u32 i = 0; i < r->n; i++) {
        u32 lo = a->d[i + limbs] >> bits;
        u32 hi = bits && i + limbs + 1 < a->n ? a->d[i + limbs + 1] << (32 - bits) : 0;
        r->d[i] = lo | hi;
    }
    r->neg = a->neg;
    return jsb_trim(r);
}

/* a << k, and a >> k as the floor of a / 2^k; a negative k shifts the
   other way. */
static jbint *jsb_shift(jctx *J, const jbint *a, const jbint *k, int left) {
    if (k->neg) left = !left;
    if (k->n > 1 || (left && k->n && k->d[0] > (u32)JSB_MAX_LIMBS * 32)) {
        if (!left) return a->neg ? jsb_from_u64(J, 1, 1) : jsb_new(J, 0);
        js_throw(J, JS_ERR_RANGE, "a BigInt too large for this machine", J->error_line);
        return 0;
    }
    u32 by = k->n ? k->d[0] : 0;
    if (left || !a->neg) return jsb_shift_mag(J, a, by, left);
    /* -x >> k is -((x - 1) >> k) - 1. */
    jbint *one = jsb_from_u64(J, 1, 0);
    jbint *m = one ? jsb_addsub(J, jsb_copy(J, a, 0, 0), one, 1) : 0;
    jbint *s = m ? jsb_shift_mag(J, m, by, 0) : 0;
    jbint *r = s ? jsb_addsub(J, s, one, 0) : 0;
    if (r && r->n) r->neg = 1;
    return r;
}

/* BigInt.asUintN and asIntN: a modulo 2^bits, and that read as signed. */
static jbint *jsb_as_n(jctx *J, const jbint *a, u32 bits, int is_signed) {
    if (!bits) return jsb_new(J, 0);
    u32 len = (bits + 31) / 32 + 1;
    if (len < a->n + 1) len = a->n + 1;
    u32 *x = (u32 *)malloc((u64)len * 4);
    if (!x) { js_throw(J, JS_ERR_RANGE, "no memory for a BigInt", J->error_line); return 0; }
    jsb_to_tc(a, x, len);
    u32 whole = bits / 32, part = bits % 32;
    for (u32 i = whole + (part ? 1 : 0); i < len; i++) x[i] = 0;
    if (part) x[whole] &= (1u << part) - 1;
    int top = (int)((x[(bits - 1) / 32] >> ((bits - 1) % 32)) & 1);
    if (is_signed && top) {
        /* Take 2^bits off: set every bit above as a two's complement sign. */
        if (part) x[whole] |= ~((1u << part) - 1);
        for (u32 i = whole + (part ? 1 : 0); i < len; i++) x[i] = 0xFFFFFFFFu;
    }
    jbint *r = jsb_from_tc(J, x, len);
    free(x);
    return r;
}

/* --- text and numbers ----------------------------------------------------------- */

static jstr *jsb_to_str(jctx *J, const jbint *a, int radix) {
    if (!a->n) return js_str(J, "0");
    static const char DIG[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    u32 chunk = (u32)radix, k = 1;
    while ((u64)chunk * (u32)radix < 0xFFFFFFFFull) { chunk *= (u32)radix; k++; }
    u32 n = a->n;
    u32 *m = (u32 *)malloc((u64)n * 4);
    /* At most 32 digits a limb, in radix 2. */
    char *out = (char *)malloc((u64)n * 32 + 2);
    if (!m || !out) { free(m); free(out); return js_str(J, ""); }
    for (u32 i = 0; i < n; i++) m[i] = a->d[i];
    u32 w = 0;
    while (n) {
        u32 r = jsb_mag_divsmall(m, &n, chunk);
        for (u32 i = 0; i < k && (n || r); i++) { out[w++] = DIG[r % (u32)radix]; r /= (u32)radix; }
    }
    if (a->neg) out[w++] = '-';
    for (u32 i = 0; i < w / 2; i++) { char t = out[i]; out[i] = out[w - 1 - i]; out[w - 1 - i] = t; }
    jstr *s = js_str_n(J, out, w);
    free(m);
    free(out);
    return s;
}

static int jsb_digit(char c, int radix) {
    int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
    return v < radix ? v : -1;
}

/* Digits in a radix, underscores passed over (a literal's); 0 when one is
   not a digit or there are none. */
static jbint *jsb_parse_digits(jctx *J, const char *s, u32 n, int radix, int neg) {
    /* At most six bits a digit, in any radix up to 36. */
    jbint *b = jsb_new(J, n * 6 / 32 + 2);
    if (!b) return 0;
    u32 nb = 0, got = 0;
    for (u32 i = 0; i < n; i++) {
        if (s[i] == '_') continue;
        int d = jsb_digit(s[i], radix);
        if (d < 0) return 0;
        nb = jsb_mag_muladd(b->d, nb, (u32)radix, (u32)d);
        got++;
    }
    if (!got) return 0;
    b->n = nb;
    b->neg = (u32)neg;
    return jsb_trim(b);
}

/* A literal's text, as the lexer kept it: 123, 0x1f, 0o17, 0b101. */
static jbint *jsb_literal(jctx *J, const jstr *s) {
    if (s->len > 2 && s->s[0] == '0') {
        char p = (char)(s->s[1] | 0x20);
        int radix = p == 'x' ? 16 : p == 'o' ? 8 : p == 'b' ? 2 : 0;
        if (radix) return jsb_parse_digits(J, s->s + 2, s->len - 2, radix, 0);
    }
    return jsb_parse_digits(J, s->s, s->len, 10, 0);
}

/* StringToBigInt: white space round it, a sign for decimal only, a prefix,
   and nothing for 0n. 0 when it is not one. */
static jbint *jsb_from_text(jctx *J, const char *s, u32 n) {
    u32 i = 0, e = n;
    while (i < e && (js_blank(s[i]) || (u8)s[i] == 0xA0)) i++;
    while (e > i && (js_blank(s[e - 1]) || (u8)s[e - 1] == 0xA0)) e--;
    if (i == e) return jsb_new(J, 0);
    int neg = 0;
    if (e - i > 2 && s[i] == '0') {
        char p = (char)(s[i + 1] | 0x20);
        int radix = p == 'x' ? 16 : p == 'o' ? 8 : p == 'b' ? 2 : 0;
        if (radix) {
            for (u32 k = i + 2; k < e; k++) if (s[k] == '_') return 0;
            return jsb_parse_digits(J, s + i + 2, e - i - 2, radix, 0);
        }
    }
    if (s[i] == '+' || s[i] == '-') { neg = s[i] == '-'; i++; }
    for (u32 k = i; k < e; k++) if (s[k] == '_') return 0;
    if (i == e) return 0;
    jbint *b = jsb_parse_digits(J, s + i, e - i, 10, neg);
    return b;
}

/* A whole, finite number, exactly. */
static jbint *jsb_from_double(jctx *J, double d) {
    int neg = d < 0;
    if (neg) d = -d;
    if (d < 18446744073709551616.0) return jsb_from_u64(J, (u64)d, neg);
    union { double d; u64 u; } x;
    x.d = d;
    int e = (int)((x.u >> 52) & 0x7FF) - 1075;
    u64 m = (x.u & 0xFFFFFFFFFFFFFull) | (1ull << 52);
    jbint *b = jsb_from_u64(J, m, 0);
    if (!b) return 0;
    jbint *r = jsb_shift_mag(J, b, (u32)e, 1);
    if (r) r->neg = (u32)neg;
    return r;
}

/* The nearest double, through the decimal text, which is read exactly. */
static double jsb_to_double(jctx *J, const jbint *a) {
    if (!a->n) return 0;
    if (a->n <= 2) {
        u64 v = a->d[0] | (a->n > 1 ? (u64)a->d[1] << 32 : 0);
        if (v < (1ull << 53)) return a->neg ? -(double)v : (double)v;
    }
    jstr *s = jsb_to_str(J, a, 10);
    return js_str_to_num(s->s, s->len);
}

static int js_is_integral(double d) { return d - d == 0 && d == js_trunc(d); }

/* A BigInt against a Number, exactly: -1, 0 or 1, or 2 for NaN. */
static int jsb_cmp_num(jctx *J, const jbint *a, double d) {
    if (d != d) return 2;
    if (d - d != 0) return d > 0 ? -1 : 1;
    double fl = js_floor(d);
    jbint *f = jsb_from_double(J, fl);
    if (!f) return 2;
    int c = jsb_cmp(a, f);
    if (c) return c;
    return d > fl ? -1 : 0;
}

static u32 jsb_hash(const jbint *a) {
    u32 h = a->neg ? 0x9E3779B9u : 0x7F4A7C15u;
    for (u32 i = 0; i < a->n; i++) h = (h ^ a->d[i]) * 16777619u;
    return h;
}

/* --- the language's operators --------------------------------------------------------- */

static jval jsb_mixed(jctx *J) {
    return js_throw(J, JS_ERR_TYPE, "BigInt and other types cannot be mixed: convert one of them", J->error_line);
}

/* A binary operator where one side is a BigInt (both primitives). */
static jval jsb_binary(jctx *J, jop op, jval l, jval r) {
    if (l.t != JS_BIG || r.t != JS_BIG) return jsb_mixed(J);
    const jbint *a = l.big, *b = r.big;
    jbint *res = 0;
    switch (op) {
        case OP_ADD: res = jsb_addsub(J, a, b, 0); break;
        case OP_SUB: res = jsb_addsub(J, a, b, 1); break;
        case OP_MUL: res = jsb_mul(J, a, b); break;
        case OP_DIV: jsb_divmod(J, a, b, &res, 0); break;
        case OP_MOD: jsb_divmod(J, a, b, 0, &res); break;
        case OP_POW: res = jsb_pow(J, a, b); break;
        case OP_BAND: res = jsb_bitop(J, a, b, 0); break;
        case OP_BOR: res = jsb_bitop(J, a, b, 1); break;
        case OP_BXOR: res = jsb_bitop(J, a, b, 2); break;
        case OP_SHL: res = jsb_shift(J, a, b, 1); break;
        case OP_SHR: res = jsb_shift(J, a, b, 0); break;
        case OP_USHR: return js_throw(J, JS_ERR_TYPE, "a BigInt has no unsigned shift", J->error_line);
        default: return js_undef();
    }
    return res && J->sig == JS_OK ? js_from_big(res) : js_undef();
}

/* <, >, <=, >= with a BigInt on one side and a BigInt, Number or string on
   the other; a string that is no BigInt compares false. */
static jval jsb_relational(jctx *J, jop op, jval l, jval r) {
    int c;
    if (l.t == JS_BIG && r.t == JS_BIG) c = jsb_cmp(l.big, r.big);
    else {
        int flip = l.t != JS_BIG;
        jval big = flip ? r : l, other = flip ? l : r;
        if (other.t == JS_STR) {
            jbint *o = jsb_from_text(J, other.str->s, other.str->len);
            if (!o) return js_bool(0);
            c = jsb_cmp(big.big, o);
        } else {
            double d = js_to_num(J, other);
            if (J->sig != JS_OK) return js_undef();
            c = jsb_cmp_num(J, big.big, d);
            if (c == 2) return js_bool(0);
        }
        if (flip) c = -c;
    }
    switch (op) {
        case OP_LT: return js_bool(c < 0);
        case OP_GT: return js_bool(c > 0);
        case OP_LE: return js_bool(c <= 0);
        default:    return js_bool(c >= 0);
    }
}

/* == with a BigInt on one side and a primitive that is not one on the other. */
static int jsb_loose_eq(jctx *J, jval big, jval other) {
    if (other.t == JS_STR) {
        jbint *o = jsb_from_text(J, other.str->s, other.str->len);
        return o && !jsb_cmp(big.big, o);
    }
    if (other.t == JS_NUM || other.t == JS_BOOL) {
        double d = other.t == JS_BOOL ? (other.b ? 1 : 0) : other.num;
        return jsb_cmp_num(J, big.big, d) == 0;
    }
    return 0;
}

static jval jsb_negate(jctx *J, jval v) {
    jbint *r = jsb_copy(J, v.big, 0, v.big->n ? !v.big->neg : 0);
    return r ? js_from_big(r) : js_undef();
}

static jval jsb_bitnot(jctx *J, jval v) {
    jbint *one = jsb_from_u64(J, 1, 0);
    jbint *r = one ? jsb_addsub(J, v.big, one, 0) : 0;
    if (!r) return js_undef();
    if (r->n) r->neg = !r->neg;
    return js_from_big(r);
}

static jval jsb_step(jctx *J, jval v, int up) {
    jbint *one = jsb_from_u64(J, 1, 0);
    jbint *r = one ? jsb_addsub(J, v.big, one, !up) : 0;
    return r ? js_from_big(r) : js_undef();
}

/* --- BigInt and its prototype ------------------------------------------------------------ */

/* ToBigInt: what BigInt() and asIntN take. */
static jbint *jsb_to_big(jctx *J, jval v) {
    if (v.t == JS_OBJ) { v = js_to_primitive(J, v, 1); if (J->sig != JS_OK) return 0; }
    switch (v.t) {
        case JS_BIG: return v.big;
        case JS_BOOL: return jsb_from_u64(J, v.b ? 1 : 0, 0);
        case JS_STR: {
            jbint *b = jsb_from_text(J, v.str->s, v.str->len);
            if (!b && J->sig == JS_OK) js_throw_named(J, JS_ERR_SYNTAX, "", v.str, " cannot be made into a BigInt");
            return b;
        }
        case JS_NUM:
            js_throw(J, JS_ERR_TYPE, "a Number is made into a BigInt with BigInt()", J->error_line);
            return 0;
        default:
            js_throw(J, JS_ERR_TYPE, "that cannot be made into a BigInt", J->error_line);
            return 0;
    }
}

static jval nat_bigint(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (J->new_target.t != JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "BigInt is not a constructor", J->error_line);
    jval v = js_arg(a, n, 0);
    if (v.t == JS_OBJ) { v = js_to_primitive(J, v, 1); if (J->sig != JS_OK) return js_undef(); }
    if (v.t == JS_NUM) {
        if (!js_is_integral(v.num)) return js_throw(J, JS_ERR_RANGE, "a Number that is not whole cannot be a BigInt", J->error_line);
        jbint *b = jsb_from_double(J, v.num);
        return b ? js_from_big(b) : js_undef();
    }
    jbint *b = jsb_to_big(J, v);
    return b ? js_from_big(b) : js_undef();
}

static jbint *jsb_this(jctx *J, jval t) {
    if (t.t == JS_BIG) return t.big;
    if (js_is_obj(t) && t.obj->kind == JO_BOXED && t.obj->ival.t == JS_BIG) return t.obj->ival.big;
    js_throw(J, JS_ERR_TYPE, "this is not a BigInt", J->error_line);
    return 0;
}

static jval nat_bigint_tostring(jctx *J, jval t, jval *a, int n) {
    jbint *b = jsb_this(J, t);
    if (!b) return js_undef();
    int radix = 10;
    if (n > 0 && a[0].t != JS_UNDEF) {
        double r = js_to_num(J, a[0]);
        if (r != r || r < 2 || r > 36) return js_throw(J, JS_ERR_RANGE, "toString takes a radix from 2 to 36", J->error_line);
        radix = (int)r;
    }
    return js_from_str(jsb_to_str(J, b, radix));
}

static jval nat_bigint_valueof(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jbint *b = jsb_this(J, t);
    return b ? js_from_big(b) : js_undef();
}

static jval intl_bigint_text(jctx *J, jval v, jval locales, jval opts);

static jval nat_bigint_tolocale(jctx *J, jval t, jval *a, int n) {
    jbint *b = jsb_this(J, t);
    if (!b) return js_undef();
    return intl_bigint_text(J, js_from_big(b), js_arg(a, n, 0), js_arg(a, n, 1));
}

static jval jsb_as(jctx *J, jval *a, int n, int is_signed) {
    double bits = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (J->sig != JS_OK) return js_undef();
    if (bits != bits || bits < 0 || bits > 9007199254740991.0) return js_throw(J, JS_ERR_RANGE, "a number of bits out of range", J->error_line);
    jbint *b = jsb_to_big(J, js_arg(a, n, 1));
    if (!b) return js_undef();
    if (bits > (double)JSB_MAX_LIMBS * 32) {
        if (!is_signed && b->neg) return js_throw(J, JS_ERR_RANGE, "a BigInt too large for this machine", J->error_line);
        return js_from_big(b);
    }
    jbint *r = jsb_as_n(J, b, (u32)bits, is_signed);
    return r ? js_from_big(r) : js_undef();
}

static jval nat_bigint_asintn(jctx *J, jval t, jval *a, int n) { (void)t; return jsb_as(J, a, n, 1); }
static jval nat_bigint_asuintn(jctx *J, jval t, jval *a, int n) { (void)t; return jsb_as(J, a, n, 0); }

/* DataView's BigInt methods: eight bytes, big end first unless asked, as a
   64-bit number signed or not. GitHub's protocol buffers look for all four
   before they use BigInt for their 64-bit fields. */
static jval jsb_view(jctx *J, jval t, jval *a, int n, int is_signed, int set) {
    if (!js_is_obj(t) || t.obj->kind != JO_VIEW)
        return js_throw(J, JS_ERR_TYPE, "this is not a DataView", J->error_line);
    jtyped *x = (jtyped *)t.obj->internal;
    double at = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (J->sig != JS_OK) return js_undef();
    if (at < 0 || at + 8 > x->len) return js_throw(J, JS_ERR_RANGE, "that is past the end of the view", J->error_line);
    int little = js_to_bool(js_arg(a, n, set ? 2 : 1));
    u8 *p = ta_bytes(x->buf) + x->off + (u32)at;
    if (set) {
        jbint *b = jsb_to_big(J, js_arg(a, n, 1));
        jbint *m = b ? jsb_as_n(J, b, 64, 0) : 0;
        if (!m) return js_undef();
        u64 v = (m->n > 0 ? m->d[0] : 0) | (m->n > 1 ? (u64)m->d[1] << 32 : 0);
        for (int k = 0; k < 8; k++) p[little ? k : 7 - k] = (u8)(v >> (8 * k));
        return js_undef();
    }
    u64 v = 0;
    for (int k = 7; k >= 0; k--) v = (v << 8) | p[little ? k : 7 - k];
    jbint *r = is_signed && (v >> 63) ? jsb_from_u64(J, ~v + 1, 1) : jsb_from_u64(J, v, 0);
    return r ? js_from_big(r) : js_undef();
}

static jval nat_view_getbigint64(jctx *J, jval t, jval *a, int n) { return jsb_view(J, t, a, n, 1, 0); }
static jval nat_view_getbiguint64(jctx *J, jval t, jval *a, int n) { return jsb_view(J, t, a, n, 0, 0); }
static jval nat_view_setbigint64(jctx *J, jval t, jval *a, int n) { return jsb_view(J, t, a, n, 1, 1); }
static jval nat_view_setbiguint64(jctx *J, jval t, jval *a, int n) { return jsb_view(J, t, a, n, 0, 1); }

static void js_setup_bigint(jctx *J) {
    if (J->p_view) {
        js_method(J, J->p_view, "getBigInt64", nat_view_getbigint64, 1);
        js_method(J, J->p_view, "getBigUint64", nat_view_getbiguint64, 1);
        js_method(J, J->p_view, "setBigInt64", nat_view_setbigint64, 2);
        js_method(J, J->p_view, "setBigUint64", nat_view_setbiguint64, 2);
    }
    J->p_bigint = js_object_with(J, JO_PLAIN, J->p_object);
    jobj *p = J->p_bigint;
    jobj *c = js_ctor(J, "BigInt", nat_bigint, 1, p);
    if (!p || !c) return;
    js_method(J, c, "asIntN", nat_bigint_asintn, 2);
    js_method(J, c, "asUintN", nat_bigint_asuintn, 2);
    js_method(J, p, "toString", nat_bigint_tostring, 0);
    js_method(J, p, "valueOf", nat_bigint_valueof, 0);
    js_method(J, p, "toLocaleString", nat_bigint_tolocale, 0);
    js_tag(J, p, "BigInt");
}
