/* ECDSA over P-256 and P-384.
 *
 * Both curves have the same shape: y^2 = x^3 - 3x + b over a prime field,
 * with a group of prime order. Only the numbers differ, so there is one
 * implementation here and a table of constants, rather than two copies
 * diverging quietly from each other.
 *
 * Two moduli are in play for each curve and they are nearly the same size:
 * the prime the coordinates live in, and the order of the group, which is
 * what scalars are taken modulo. Confusing them gives something that
 * verifies most signatures and rejects a few, which is the worst way for
 * this to fail because it reads as flakiness rather than as a bug. So each
 * is held in a structure carrying its own constants and nothing is reduced
 * without saying which one it is being reduced in.
 *
 * Both primes have a special shape allowing a much faster reduction than
 * Montgomery's, written as additions and subtractions of shifted words.
 * That is also where implementations of these curves most often go wrong,
 * because the carries are unobvious and a mistake shows only for particular
 * inputs. A signature check is a few million operations either way, which is
 * nothing beside the round trip that delivered the signature, so the uniform
 * method is used for both.
 *
 * Points are Jacobian, so adding two of them needs no division; only the
 * final answer is converted back, at the cost of one inversion rather than
 * one per step. */
#include "ec.h"
#include "string.h"

typedef unsigned __int128 u128;

#define MAXL 6                  /* limbs in a 384 bit number */

typedef struct { u64 v[MAXL]; } fe;

typedef struct {
    int n;                      /* limbs in use */
    u64 m[MAXL];                /* the modulus */
    u64 n0inv;                  /* -m^-1 mod 2^64 */
    fe  rr;                     /* R^2 mod m, for entering Montgomery form */
    fe  one;                    /* R mod m, which is one in Montgomery form */
} field_t;

typedef struct {
    int limbs;
    u32 bytes;
    field_t fp;                 /* the coordinate field */
    field_t fn;                 /* the scalar field */
    fe b, gx, gy;               /* all in Montgomery form */
} curve_t;

/* --- the curves ---------------------------------------------------------- */

static const u64 P256_P[4] = {
    0xffffffffffffffffULL, 0x00000000ffffffffULL,
    0x0000000000000000ULL, 0xffffffff00000001ULL
};
static const u64 P256_N[4] = {
    0xf3b9cac2fc632551ULL, 0xbce6faada7179e84ULL,
    0xffffffffffffffffULL, 0xffffffff00000000ULL
};
static const u8 P256_B[32] = {
    0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,
    0x76,0x98,0x86,0xbc,0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,
    0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b
};
static const u8 P256_GX[32] = {
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,
    0x63,0xa4,0x40,0xf2,0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,
    0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96
};
static const u8 P256_GY[32] = {
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,
    0x7c,0x0f,0x9e,0x16,0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,
    0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5
};

static const u64 P384_P[6] = {
    0x00000000ffffffffULL, 0xffffffff00000000ULL,
    0xfffffffffffffffeULL, 0xffffffffffffffffULL,
    0xffffffffffffffffULL, 0xffffffffffffffffULL
};
static const u64 P384_N[6] = {
    0xecec196accc52973ULL, 0x581a0db248b0a77aULL,
    0xc7634d81f4372ddfULL, 0xffffffffffffffffULL,
    0xffffffffffffffffULL, 0xffffffffffffffffULL
};
static const u8 P384_B[48] = {
    0xb3,0x31,0x2f,0xa7,0xe2,0x3e,0xe7,0xe4,0x98,0x8e,0x05,0x6b,
    0xe3,0xf8,0x2d,0x19,0x18,0x1d,0x9c,0x6e,0xfe,0x81,0x41,0x12,
    0x03,0x14,0x08,0x8f,0x50,0x13,0x87,0x5a,0xc6,0x56,0x39,0x8d,
    0x8a,0x2e,0xd1,0x9d,0x2a,0x85,0xc8,0xed,0xd3,0xec,0x2a,0xef
};
static const u8 P384_GX[48] = {
    0xaa,0x87,0xca,0x22,0xbe,0x8b,0x05,0x37,0x8e,0xb1,0xc7,0x1e,
    0xf3,0x20,0xad,0x74,0x6e,0x1d,0x3b,0x62,0x8b,0xa7,0x9b,0x98,
    0x59,0xf7,0x41,0xe0,0x82,0x54,0x2a,0x38,0x55,0x02,0xf2,0x5d,
    0xbf,0x55,0x29,0x6c,0x3a,0x54,0x5e,0x38,0x72,0x76,0x0a,0xb7
};
static const u8 P384_GY[48] = {
    0x36,0x17,0xde,0x4a,0x96,0x26,0x2c,0x6f,0x5d,0x9e,0x98,0xbf,
    0x92,0x92,0xdc,0x29,0xf8,0xf4,0x1d,0xbd,0x28,0x9a,0x14,0x7c,
    0xe9,0xda,0x31,0x13,0xb5,0xf0,0xb8,0xc0,0x0a,0x60,0xb1,0xce,
    0x1d,0x7e,0x81,0x9d,0x7a,0x43,0x1d,0x7c,0x90,0xea,0x0e,0x5f
};

/* --- fixed width arithmetic ---------------------------------------------- */

static void fe_zero(fe *a) { for (int i = 0; i < MAXL; i++) a->v[i] = 0; }

static bool fe_is_zero(const fe *a, int n) {
    u64 acc = 0;
    for (int i = 0; i < n; i++) acc |= a->v[i];
    return acc == 0;
}

static bool fe_eq(const fe *a, const fe *b, int n) {
    u64 diff = 0;
    for (int i = 0; i < n; i++) diff |= a->v[i] ^ b->v[i];
    return diff == 0;
}

static bool ge_mod(const u64 *a, const u64 *m, int n) {
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != m[i]) return a[i] > m[i];
    return true;
}

static u64 sub_in_place(u64 *a, const u64 *b, int n) {
    u64 borrow = 0;
    for (int i = 0; i < n; i++) {
        u64 x = a[i], y = b[i];
        u64 d = x - y - borrow;
        borrow = (x < y + borrow) || (borrow && y + borrow == 0);
        a[i] = d;
    }
    return borrow;
}

static u64 add_in_place(u64 *a, const u64 *b, int n) {
    u64 carry = 0;
    for (int i = 0; i < n; i++) {
        u64 x = a[i];
        u64 t = x + b[i];
        u64 c1 = t < x;
        u64 u = t + carry;
        carry = c1 | (u < t);
        a[i] = u;
    }
    return carry;
}

/* Both of these work in a local and copy out at the end, so the result may
   be either operand. Writing straight into out is faster and silently wrong
   when out is the second operand: the copy in destroys it before it is
   read, and the answer is a + a rather than a + b. That is one character of
   difference at the call site and it cost an afternoon. */
static void fe_add_mod(fe *out, const fe *a, const fe *b, const field_t *f) {
    fe t = *a;
    u64 carry = add_in_place(t.v, b->v, f->n);
    if (carry || ge_mod(t.v, f->m, f->n)) sub_in_place(t.v, f->m, f->n);
    *out = t;
}

static void fe_sub_mod(fe *out, const fe *a, const fe *b, const field_t *f) {
    fe t = *a;
    u64 borrow = sub_in_place(t.v, b->v, f->n);
    if (borrow) add_in_place(t.v, f->m, f->n);
    *out = t;
}

/* a = 2a mod m, used only to build the Montgomery constants without
   writing long division. */
static void fe_dbl_mod(fe *a, const u64 *m, int n) {
    u64 carry = 0;
    for (int i = 0; i < n; i++) {
        u64 x = a->v[i];
        a->v[i] = (x << 1) | carry;
        carry = x >> 63;
    }
    if (carry || ge_mod(a->v, m, n)) sub_in_place(a->v, m, n);
}

static u64 n0_inverse(u64 m0) {
    u64 inv = 1;
    for (int i = 0; i < 6; i++) inv *= 2 - m0 * inv;
    return 0 - inv;
}

static void mont_mul(fe *out, const fe *a, const fe *b, const field_t *f) {
    int n = f->n;
    u64 t[MAXL + 2];
    for (int i = 0; i < n + 2; i++) t[i] = 0;

    for (int i = 0; i < n; i++) {
        u64 carry = 0;
        for (int j = 0; j < n; j++) {
            u128 p = (u128)a->v[j] * b->v[i] + t[j] + carry;
            t[j] = (u64)p;
            carry = (u64)(p >> 64);
        }
        u128 s = (u128)t[n] + carry;
        t[n] = (u64)s;
        t[n + 1] = (u64)(s >> 64);

        /* The multiple of m that makes the bottom limb vanish, which is
           what lets the whole thing shift down by a limb instead of being
           divided. */
        u64 mm = t[0] * f->n0inv;
        u128 p = (u128)mm * f->m[0] + t[0];
        carry = (u64)(p >> 64);
        for (int j = 1; j < n; j++) {
            p = (u128)mm * f->m[j] + t[j] + carry;
            t[j - 1] = (u64)p;
            carry = (u64)(p >> 64);
        }
        s = (u128)t[n] + carry;
        t[n - 1] = (u64)s;
        t[n] = t[n + 1] + (u64)(s >> 64);
    }

    fe_zero(out);
    for (int i = 0; i < n; i++) out->v[i] = t[i];
    if (t[n] || ge_mod(out->v, f->m, n)) sub_in_place(out->v, f->m, n);
}

static void mont_sq(fe *out, const fe *a, const field_t *f) {
    mont_mul(out, a, a, f);
}

static void field_setup(field_t *f, const u64 *m, int n) {
    f->n = n;
    for (int i = 0; i < MAXL; i++) f->m[i] = 0;
    for (int i = 0; i < n; i++) f->m[i] = m[i];
    f->n0inv = n0_inverse(m[0]);

    fe_zero(&f->rr);
    f->rr.v[0] = 1;
    for (int i = 0; i < 2 * n * 64; i++) fe_dbl_mod(&f->rr, f->m, n);

    fe_zero(&f->one);
    f->one.v[0] = 1;
    for (int i = 0; i < n * 64; i++) fe_dbl_mod(&f->one, f->m, n);
}

static void to_mont(fe *out, const fe *a, const field_t *f) {
    mont_mul(out, a, &f->rr, f);
}
static void from_mont(fe *out, const fe *a, const field_t *f) {
    fe one;
    fe_zero(&one);
    one.v[0] = 1;
    mont_mul(out, a, &one, f);
}

/* a^-1 as a^(m-2), which is only valid because both moduli are prime. */
static void fe_inv(fe *out, const fe *a, const field_t *f) {
    u64 e[MAXL];
    for (int i = 0; i < f->n; i++) e[i] = f->m[i];
    u64 two[MAXL] = { 2, 0, 0, 0, 0, 0 };
    sub_in_place(e, two, f->n);

    fe acc = f->one;
    fe base = *a;
    for (int i = f->n * 64 - 1; i >= 0; i--) {
        mont_sq(&acc, &acc, f);
        if ((e[i / 64] >> (i % 64)) & 1) mont_mul(&acc, &acc, &base, f);
    }
    *out = acc;
}

static void fe_from_bytes(fe *out, const u8 *b, u32 len) {
    fe_zero(out);
    for (u32 i = 0; i < len; i++) {
        u32 pos = len - 1 - i;
        out->v[pos / 8] |= (u64)b[i] << ((pos % 8) * 8);
    }
}

static void fe_to_bytes(u8 *b, const fe *a, u32 len) {
    for (u32 i = 0; i < len; i++) {
        u32 pos = len - 1 - i;
        b[i] = (u8)(a->v[pos / 8] >> ((pos % 8) * 8));
    }
}

/* --- points -------------------------------------------------------------- */

/* Jacobian: the affine point is (X/Z^2, Y/Z^3), and Z of zero is the point
   at infinity, which is the identity. */
typedef struct { fe x, y, z; } point_t;

static bool pt_is_inf(const point_t *p, int n) { return fe_is_zero(&p->z, n); }

static void pt_set_inf(point_t *p) {
    fe_zero(&p->x); fe_zero(&p->y); fe_zero(&p->z);
}

/* Doubling, for a = -3, which is what lets alpha be one multiplication
   rather than three. */
static void pt_double(point_t *out, const point_t *a, const field_t *f) {
    if (pt_is_inf(a, f->n) || fe_is_zero(&a->y, f->n)) { pt_set_inf(out); return; }

    fe delta, gamma, beta, alpha, t1, t2, x3, y3, z3;

    mont_sq(&delta, &a->z, f);
    mont_sq(&gamma, &a->y, f);
    mont_mul(&beta, &a->x, &gamma, f);

    fe_sub_mod(&t1, &a->x, &delta, f);
    fe_add_mod(&t2, &a->x, &delta, f);
    mont_mul(&alpha, &t1, &t2, f);
    fe_add_mod(&t1, &alpha, &alpha, f);
    fe_add_mod(&alpha, &t1, &alpha, f);        /* three times */

    mont_sq(&x3, &alpha, f);
    fe_add_mod(&t1, &beta, &beta, f);
    fe_add_mod(&t1, &t1, &t1, f);              /* four betas */
    fe_sub_mod(&x3, &x3, &t1, f);
    fe_sub_mod(&x3, &x3, &t1, f);              /* less eight of them */

    fe_add_mod(&t2, &a->y, &a->z, f);
    mont_sq(&z3, &t2, f);
    fe_sub_mod(&z3, &z3, &gamma, f);
    fe_sub_mod(&z3, &z3, &delta, f);

    fe_sub_mod(&t2, &t1, &x3, f);
    mont_mul(&y3, &alpha, &t2, f);
    mont_sq(&t1, &gamma, f);
    fe_add_mod(&t1, &t1, &t1, f);
    fe_add_mod(&t1, &t1, &t1, f);
    fe_add_mod(&t1, &t1, &t1, f);              /* eight gamma squared */
    fe_sub_mod(&y3, &y3, &t1, f);

    out->x = x3; out->y = y3; out->z = z3;
}

static void pt_add(point_t *out, const point_t *a, const point_t *b,
                   const field_t *f) {
    if (pt_is_inf(a, f->n)) { *out = *b; return; }
    if (pt_is_inf(b, f->n)) { *out = *a; return; }

    fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, r, v, t1, t2, x3, y3, z3;

    mont_sq(&z1z1, &a->z, f);
    mont_sq(&z2z2, &b->z, f);
    mont_mul(&u1, &a->x, &z2z2, f);
    mont_mul(&u2, &b->x, &z1z1, f);
    mont_mul(&t1, &b->z, &z2z2, f);
    mont_mul(&s1, &a->y, &t1, f);
    mont_mul(&t2, &a->z, &z1z1, f);
    mont_mul(&s2, &b->y, &t2, f);

    if (fe_eq(&u1, &u2, f->n)) {
        /* Same x. Either the same point, and this is a doubling, or
           opposite points, and the answer is infinity. The general formula
           gives zero over zero for both, so the case is written out. */
        if (fe_eq(&s1, &s2, f->n)) { pt_double(out, a, f); return; }
        pt_set_inf(out);
        return;
    }

    fe_sub_mod(&h, &u2, &u1, f);
    fe_add_mod(&t1, &h, &h, f);
    mont_sq(&i, &t1, f);
    mont_mul(&j, &h, &i, f);
    fe_sub_mod(&t2, &s2, &s1, f);
    fe_add_mod(&r, &t2, &t2, f);
    mont_mul(&v, &u1, &i, f);

    mont_sq(&x3, &r, f);
    fe_sub_mod(&x3, &x3, &j, f);
    fe_sub_mod(&x3, &x3, &v, f);
    fe_sub_mod(&x3, &x3, &v, f);

    fe_sub_mod(&t1, &v, &x3, f);
    mont_mul(&y3, &r, &t1, f);
    mont_mul(&t2, &s1, &j, f);
    fe_add_mod(&t2, &t2, &t2, f);
    fe_sub_mod(&y3, &y3, &t2, f);

    fe_add_mod(&t1, &a->z, &b->z, f);
    mont_sq(&t1, &t1, f);
    fe_sub_mod(&t1, &t1, &z1z1, f);
    fe_sub_mod(&t1, &t1, &z2z2, f);
    mont_mul(&z3, &t1, &h, f);

    out->x = x3; out->y = y3; out->z = z3;
}

/* k*P, top bit first. The scalars here are public, so the obvious loop is
   the right one. */
static void pt_mul(point_t *out, const fe *k, const point_t *p,
                   const field_t *f) {
    point_t acc;
    pt_set_inf(&acc);
    for (int i = f->n * 64 - 1; i >= 0; i--) {
        pt_double(&acc, &acc, f);
        if ((k->v[i / 64] >> (i % 64)) & 1) pt_add(&acc, &acc, p, f);
    }
    *out = acc;
}

static bool pt_affine_x(fe *out, const point_t *p, const field_t *f) {
    if (pt_is_inf(p, f->n)) return false;
    fe zi, zi2;
    fe_inv(&zi, &p->z, f);
    mont_sq(&zi2, &zi, f);
    mont_mul(out, &p->x, &zi2, f);
    return true;
}

/* --- curve setup --------------------------------------------------------- */

static bool curve_setup(curve_t *c, ec_curve_t which) {
    const u64 *p, *n;
    const u8 *b, *gx, *gy;
    int limbs;

    if (which == EC_P256) {
        limbs = 4; c->bytes = 32;
        p = P256_P; n = P256_N; b = P256_B; gx = P256_GX; gy = P256_GY;
    } else if (which == EC_P384) {
        limbs = 6; c->bytes = 48;
        p = P384_P; n = P384_N; b = P384_B; gx = P384_GX; gy = P384_GY;
    } else {
        return false;
    }

    c->limbs = limbs;
    field_setup(&c->fp, p, limbs);
    field_setup(&c->fn, n, limbs);

    fe raw;
    fe_from_bytes(&raw, b, c->bytes);  to_mont(&c->b, &raw, &c->fp);
    fe_from_bytes(&raw, gx, c->bytes); to_mont(&c->gx, &raw, &c->fp);
    fe_from_bytes(&raw, gy, c->bytes); to_mont(&c->gy, &raw, &c->fp);
    return true;
}

/* y^2 == x^3 - 3x + b. A key that is not a point on the curve is a known
   way to get information out of a verifier that does not look, so it is
   checked before the key is used for anything at all. */
static bool on_curve(const curve_t *c, const fe *x, const fe *y) {
    const field_t *f = &c->fp;
    fe lhs, x3, t, three_x;

    mont_sq(&lhs, y, f);
    mont_sq(&x3, x, f);
    mont_mul(&x3, &x3, x, f);

    fe_add_mod(&three_x, x, x, f);
    fe_add_mod(&three_x, &three_x, x, f);
    fe_sub_mod(&t, &x3, &three_x, f);
    fe_add_mod(&t, &t, &c->b, f);

    return fe_eq(&lhs, &t, f->n);
}

u32 ec_field_bytes(ec_curve_t curve) {
    return curve == EC_P384 ? 48 : 32;
}

bool ec_verify(ec_curve_t curve,
               const u8 *pubkey, u32 pubkey_len,
               const u8 *hash, u32 hash_len,
               const u8 *r_bytes, u32 r_len,
               const u8 *s_bytes, u32 s_len) {
    curve_t c;
    if (!curve_setup(&c, curve)) return false;

    if (pubkey_len != 1 + 2 * c.bytes) return false;
    if (pubkey[0] != 0x04) return false;          /* only the uncompressed form */
    if (r_len == 0 || r_len > c.bytes) return false;
    if (s_len == 0 || s_len > c.bytes) return false;
    if (hash_len == 0 || hash_len > 64) return false;

    /* The signature, as two scalars modulo the group order. */
    u8 rb[48], sb[48];
    memset(rb, 0, sizeof(rb)); memset(sb, 0, sizeof(sb));
    memcpy(rb + (c.bytes - r_len), r_bytes, r_len);
    memcpy(sb + (c.bytes - s_len), s_bytes, s_len);

    fe r, s;
    fe_from_bytes(&r, rb, c.bytes);
    fe_from_bytes(&s, sb, c.bytes);
    if (fe_is_zero(&r, c.limbs) || fe_is_zero(&s, c.limbs)) return false;
    if (ge_mod(r.v, c.fn.m, c.limbs) || ge_mod(s.v, c.fn.m, c.limbs)) return false;

    /* The key, checked for being a real point before it is used. */
    fe qx_raw, qy_raw, qx, qy;
    fe_from_bytes(&qx_raw, pubkey + 1, c.bytes);
    fe_from_bytes(&qy_raw, pubkey + 1 + c.bytes, c.bytes);
    if (ge_mod(qx_raw.v, c.fp.m, c.limbs)) return false;
    if (ge_mod(qy_raw.v, c.fp.m, c.limbs)) return false;
    to_mont(&qx, &qx_raw, &c.fp);
    to_mont(&qy, &qy_raw, &c.fp);
    if (!on_curve(&c, &qx, &qy)) return false;

    /* The hash as a number. When it is wider than the order, the low bits
       are dropped, not the high ones: the specification takes the leftmost
       bits, which is what makes a SHA-384 signature over P-256 mean
       anything. */
    u8 hb[48];
    memset(hb, 0, sizeof(hb));
    if (hash_len >= c.bytes) memcpy(hb, hash, c.bytes);
    else                     memcpy(hb + (c.bytes - hash_len), hash, hash_len);

    fe e;
    fe_from_bytes(&e, hb, c.bytes);
    if (ge_mod(e.v, c.fn.m, c.limbs)) sub_in_place(e.v, c.fn.m, c.limbs);

    /* u1 = e/s, u2 = r/s, both modulo the order. */
    fe s_m, si, e_m, r_m, u1, u2;
    to_mont(&s_m, &s, &c.fn);
    fe_inv(&si, &s_m, &c.fn);
    to_mont(&e_m, &e, &c.fn);
    to_mont(&r_m, &r, &c.fn);
    mont_mul(&u1, &e_m, &si, &c.fn);
    mont_mul(&u2, &r_m, &si, &c.fn);
    from_mont(&u1, &u1, &c.fn);
    from_mont(&u2, &u2, &c.fn);

    /* R = u1*G + u2*Q */
    point_t g, q, p1, p2, sum;
    g.x = c.gx; g.y = c.gy; g.z = c.fp.one;
    q.x = qx;   q.y = qy;   q.z = c.fp.one;

    pt_mul(&p1, &u1, &g, &c.fp);
    pt_mul(&p2, &u2, &q, &c.fp);
    pt_add(&sum, &p1, &p2, &c.fp);

    fe xm, x;
    if (!pt_affine_x(&xm, &sum, &c.fp)) return false;
    from_mont(&x, &xm, &c.fp);

    /* Reduced modulo the order, not modulo the prime, which is the other
       place these two get mixed up. */
    if (ge_mod(x.v, c.fn.m, c.limbs)) sub_in_place(x.v, c.fn.m, c.limbs);
    return fe_eq(&x, &r, c.limbs);
}

bool ec_base_x(ec_curve_t curve, const u8 *k, u32 k_len, u8 *out_x) {
    curve_t c;
    if (!curve_setup(&c, curve)) return false;
    if (k_len != c.bytes) return false;

    point_t g, r;
    g.x = c.gx; g.y = c.gy; g.z = c.fp.one;

    fe scalar;
    fe_from_bytes(&scalar, k, c.bytes);
    pt_mul(&r, &scalar, &g, &c.fp);

    fe xm, x;
    if (!pt_affine_x(&xm, &r, &c.fp)) return false;
    from_mont(&x, &xm, &c.fp);
    fe_to_bytes(out_x, &x, c.bytes);
    return true;
}
