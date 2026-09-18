/* NIST P-256, and ECDSA verification on it.
 *
 * Two moduli are in play and they are easy to confuse: the prime the
 * coordinates live in, and the order of the group, which is a different
 * number of nearly the same size. Mixing them up produces something that
 * verifies most signatures and rejects a few, which is the worst possible
 * failure because it looks like flakiness. So both are held in a structure
 * that carries its own Montgomery constants and nothing is reduced without
 * saying which field it is being reduced in.
 *
 * The prime here has a special shape that allows a much faster reduction
 * than Montgomery's, written as a sequence of additions and subtractions of
 * shifted words. It is also where implementations of this curve most often
 * go wrong, because the carries are unobvious and a mistake shows up only
 * for particular inputs. Montgomery is uniform, already written for RSA,
 * and fast enough: a signature check is a few million operations, which is
 * nothing next to the network round trip that delivered the signature.
 *
 * Points are kept in Jacobian coordinates, so that adding two of them needs
 * no division. Only the final answer is converted back, which costs one
 * inversion instead of one per step. */
#include "p256.h"
#include "string.h"

typedef unsigned __int128 u128;

#define L 4                     /* limbs in a 256 bit number */

typedef struct { u64 v[L]; } fe;

typedef struct {
    u64 m[L];                   /* the modulus */
    u64 n0inv;                  /* -m^-1 mod 2^64 */
    fe  rr;                     /* R^2 mod m, for entering Montgomery form */
    fe  one;                    /* R mod m, which is one in Montgomery form */
} field_t;

/* p, the prime the coordinates are taken modulo. */
static const u64 P_M[L] = {
    0xffffffffffffffffULL, 0x00000000ffffffffULL,
    0x0000000000000000ULL, 0xffffffff00000001ULL
};

/* n, the order of the group, which is what the scalars are taken modulo. */
static const u64 N_M[L] = {
    0xf3b9cac2fc632551ULL, 0xbce6faada7179e84ULL,
    0xffffffffffffffffULL, 0xffffffff00000000ULL
};

/* b, the curve constant. a is -3 and is not stored. */
static const u8 CURVE_B[32] = {
    0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7,
    0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
    0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6,
    0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b
};

/* The base point. */
static const u8 GX[32] = {
    0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47,
    0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
    0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0,
    0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96
};
static const u8 GY[32] = {
    0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b,
    0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
    0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce,
    0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5
};

/* --- plain 256 bit arithmetic -------------------------------------------- */

static void fe_zero(fe *a) { for (int i = 0; i < L; i++) a->v[i] = 0; }

static bool fe_is_zero(const fe *a) {
    u64 acc = 0;
    for (int i = 0; i < L; i++) acc |= a->v[i];
    return acc == 0;
}

static bool fe_eq(const fe *a, const fe *b) {
    u64 diff = 0;
    for (int i = 0; i < L; i++) diff |= a->v[i] ^ b->v[i];
    return diff == 0;
}

/* Is a >= m, both 256 bits. */
static bool ge_mod(const u64 *a, const u64 *m) {
    for (int i = L - 1; i >= 0; i--)
        if (a[i] != m[i]) return a[i] > m[i];
    return true;
}

static u64 sub_in_place(u64 *a, const u64 *b) {
    u64 borrow = 0;
    for (int i = 0; i < L; i++) {
        u64 x = a[i], y = b[i];
        u64 d = x - y - borrow;
        borrow = (x < y + borrow) || (borrow && y + borrow == 0);
        a[i] = d;
    }
    return borrow;
}

static u64 add_in_place(u64 *a, const u64 *b) {
    u64 carry = 0;
    for (int i = 0; i < L; i++) {
        u64 x = a[i];
        u64 t = x + b[i];
        u64 c1 = t < x;
        u64 u = t + carry;
        carry = c1 | (u < t);
        a[i] = u;
    }
    return carry;
}

/* Both of these work in a local and copy out at the end, so that the
   result may be either operand. Writing straight into out is faster and
   silently wrong when out is the second operand: the copy in destroys it
   before it is read, and the answer is a + a rather than a + b. That is one
   character of difference in the caller and it cost an afternoon. */
static void fe_add_mod(fe *out, const fe *a, const fe *b, const field_t *f) {
    fe t = *a;
    u64 carry = add_in_place(t.v, b->v);
    if (carry || ge_mod(t.v, f->m)) sub_in_place(t.v, f->m);
    *out = t;
}

static void fe_sub_mod(fe *out, const fe *a, const fe *b, const field_t *f) {
    fe t = *a;
    u64 borrow = sub_in_place(t.v, b->v);
    if (borrow) add_in_place(t.v, f->m);
    *out = t;
}

/* a = 2a mod m, used only to build R^2 without writing long division. */
static void fe_dbl_mod(fe *a, const u64 *m) {
    u64 carry = 0;
    for (int i = 0; i < L; i++) {
        u64 x = a->v[i];
        a->v[i] = (x << 1) | carry;
        carry = x >> 63;
    }
    if (carry || ge_mod(a->v, m)) sub_in_place(a->v, m);
}

static u64 n0_inverse(u64 m0) {
    u64 inv = 1;
    for (int i = 0; i < 6; i++) inv *= 2 - m0 * inv;
    return 0 - inv;
}

static void mont_mul(fe *out, const fe *a, const fe *b, const field_t *f) {
    u64 t[L + 2];
    for (int i = 0; i < L + 2; i++) t[i] = 0;

    for (int i = 0; i < L; i++) {
        u64 carry = 0;
        for (int j = 0; j < L; j++) {
            u128 p = (u128)a->v[j] * b->v[i] + t[j] + carry;
            t[j] = (u64)p;
            carry = (u64)(p >> 64);
        }
        u128 s = (u128)t[L] + carry;
        t[L] = (u64)s;
        t[L + 1] = (u64)(s >> 64);

        u64 mm = t[0] * f->n0inv;
        u128 p = (u128)mm * f->m[0] + t[0];
        carry = (u64)(p >> 64);
        for (int j = 1; j < L; j++) {
            p = (u128)mm * f->m[j] + t[j] + carry;
            t[j - 1] = (u64)p;
            carry = (u64)(p >> 64);
        }
        s = (u128)t[L] + carry;
        t[L - 1] = (u64)s;
        t[L] = t[L + 1] + (u64)(s >> 64);
    }

    for (int i = 0; i < L; i++) out->v[i] = t[i];
    if (t[L] || ge_mod(out->v, f->m)) sub_in_place(out->v, f->m);
}

static void mont_sq(fe *out, const fe *a, const field_t *f) {
    mont_mul(out, a, a, f);
}

static void field_setup(field_t *f, const u64 *m) {
    for (int i = 0; i < L; i++) f->m[i] = m[i];
    f->n0inv = n0_inverse(m[0]);

    fe_zero(&f->rr);
    f->rr.v[0] = 1;
    for (int i = 0; i < 2 * L * 64; i++) fe_dbl_mod(&f->rr, m);

    fe_zero(&f->one);
    f->one.v[0] = 1;
    for (int i = 0; i < L * 64; i++) fe_dbl_mod(&f->one, m);
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

/* a^-1 mod m, as a^(m-2), which is only valid because both moduli here are
   prime. */
static void fe_inv(fe *out, const fe *a, const field_t *f) {
    /* exponent = m - 2 */
    u64 e[L];
    for (int i = 0; i < L; i++) e[i] = f->m[i];
    u64 two[L] = { 2, 0, 0, 0 };
    sub_in_place(e, two);

    fe acc = f->one;
    fe base = *a;
    for (int i = L * 64 - 1; i >= 0; i--) {
        mont_sq(&acc, &acc, f);
        if ((e[i / 64] >> (i % 64)) & 1) mont_mul(&acc, &acc, &base, f);
    }
    *out = acc;
}

static void fe_from_bytes(fe *out, const u8 b[32]) {
    fe_zero(out);
    for (int i = 0; i < 32; i++) {
        int pos = 31 - i;
        out->v[pos / 8] |= (u64)b[i] << ((pos % 8) * 8);
    }
}

static void fe_to_bytes(u8 b[32], const fe *a) {
    for (int i = 0; i < 32; i++) {
        int pos = 31 - i;
        b[i] = (u8)(a->v[pos / 8] >> ((pos % 8) * 8));
    }
}

/* --- points -------------------------------------------------------------- */

/* Jacobian: the affine point is (X/Z^2, Y/Z^3), and Z of zero is the point
   at infinity, which is the identity. */
typedef struct { fe x, y, z; } point_t;

static bool pt_is_inf(const point_t *p) { return fe_is_zero(&p->z); }

static void pt_set_inf(point_t *p) {
    fe_zero(&p->x); fe_zero(&p->y); fe_zero(&p->z);
}

/* Doubling, for a = -3, which is what lets alpha be one multiplication
   instead of three. */
static void pt_double(point_t *out, const point_t *a, const field_t *f) {
    if (pt_is_inf(a) || fe_is_zero(&a->y)) { pt_set_inf(out); return; }

    fe delta, gamma, beta, alpha, t1, t2, x3, y3, z3;

    mont_sq(&delta, &a->z, f);              /* Z^2 */
    mont_sq(&gamma, &a->y, f);              /* Y^2 */
    mont_mul(&beta, &a->x, &gamma, f);      /* X*Y^2 */

    fe_sub_mod(&t1, &a->x, &delta, f);
    fe_add_mod(&t2, &a->x, &delta, f);
    mont_mul(&alpha, &t1, &t2, f);          /* (X-Z^2)(X+Z^2) */
    fe_add_mod(&t1, &alpha, &alpha, f);
    fe_add_mod(&alpha, &t1, &alpha, f);     /* times three */

    mont_sq(&x3, &alpha, f);
    fe_add_mod(&t1, &beta, &beta, f);
    fe_add_mod(&t1, &t1, &t1, f);           /* 4*beta */
    fe_sub_mod(&x3, &x3, &t1, f);
    fe_sub_mod(&x3, &x3, &t1, f);           /* minus 8*beta */

    fe_add_mod(&t2, &a->y, &a->z, f);
    mont_sq(&z3, &t2, f);
    fe_sub_mod(&z3, &z3, &gamma, f);
    fe_sub_mod(&z3, &z3, &delta, f);

    fe_sub_mod(&t2, &t1, &x3, f);           /* 4*beta - X3 */
    mont_mul(&y3, &alpha, &t2, f);
    mont_sq(&t1, &gamma, f);                /* gamma^2 */
    fe_add_mod(&t1, &t1, &t1, f);
    fe_add_mod(&t1, &t1, &t1, f);
    fe_add_mod(&t1, &t1, &t1, f);           /* 8*gamma^2 */
    fe_sub_mod(&y3, &y3, &t1, f);

    out->x = x3; out->y = y3; out->z = z3;
}

static void pt_add(point_t *out, const point_t *a, const point_t *b,
                   const field_t *f) {
    if (pt_is_inf(a)) { *out = *b; return; }
    if (pt_is_inf(b)) { *out = *a; return; }

    fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, r, v, t1, t2, x3, y3, z3;

    mont_sq(&z1z1, &a->z, f);
    mont_sq(&z2z2, &b->z, f);
    mont_mul(&u1, &a->x, &z2z2, f);
    mont_mul(&u2, &b->x, &z1z1, f);
    mont_mul(&t1, &b->z, &z2z2, f);
    mont_mul(&s1, &a->y, &t1, f);
    mont_mul(&t2, &a->z, &z1z1, f);
    mont_mul(&s2, &b->y, &t2, f);

    if (fe_eq(&u1, &u2)) {
        /* The same x. Either the same point, and this is a doubling, or
           opposite points, and the answer is infinity. Getting this wrong
           gives a wrong answer for one input in 2^128 and is why the case
           is written out rather than left to the general formula. */
        if (fe_eq(&s1, &s2)) { pt_double(out, a, f); return; }
        pt_set_inf(out);
        return;
    }

    fe_sub_mod(&h, &u2, &u1, f);
    fe_add_mod(&t1, &h, &h, f);
    mont_sq(&i, &t1, f);                    /* (2H)^2 */
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
    for (int i = L * 64 - 1; i >= 0; i--) {
        pt_double(&acc, &acc, f);
        if ((k->v[i / 64] >> (i % 64)) & 1) pt_add(&acc, &acc, p, f);
    }
    *out = acc;
}

/* Back to an x coordinate, which is all the verification needs. */
static bool pt_affine_x(fe *out, const point_t *p, const field_t *f) {
    if (pt_is_inf(p)) return false;
    fe zi, zi2;
    fe_inv(&zi, &p->z, f);
    mont_sq(&zi2, &zi, f);
    mont_mul(out, &p->x, &zi2, f);
    return true;
}

/* Is (x, y) actually on the curve: y^2 == x^3 - 3x + b. A point that is not
   is the classic way to get a verifier to leak or to accept, so it is
   checked before the key is used for anything. */
static bool on_curve(const fe *x, const fe *y, const field_t *f) {
    fe lhs, x3, t, three_x, b;

    mont_sq(&lhs, y, f);
    mont_sq(&x3, x, f);
    mont_mul(&x3, &x3, x, f);

    fe_add_mod(&three_x, x, x, f);
    fe_add_mod(&three_x, &three_x, x, f);
    fe_sub_mod(&t, &x3, &three_x, f);

    fe b_raw;
    fe_from_bytes(&b_raw, CURVE_B);
    to_mont(&b, &b_raw, f);
    fe_add_mod(&t, &t, &b, f);

    return fe_eq(&lhs, &t);
}

bool p256_verify(const u8 pubkey[P256_POINT_LEN],
                 const u8 hash[32],
                 const u8 *r_bytes, u32 r_len,
                 const u8 *s_bytes, u32 s_len) {
    if (pubkey[0] != 0x04) return false;        /* only the uncompressed form */
    if (r_len == 0 || r_len > 32 || s_len == 0 || s_len > 32) return false;

    field_t fp, fn;
    field_setup(&fp, P_M);
    field_setup(&fn, N_M);

    /* The signature, as two scalars modulo the group order. */
    u8 rb[32], sb[32];
    memset(rb, 0, 32); memset(sb, 0, 32);
    memcpy(rb + (32 - r_len), r_bytes, r_len);
    memcpy(sb + (32 - s_len), s_bytes, s_len);

    fe r, s;
    fe_from_bytes(&r, rb);
    fe_from_bytes(&s, sb);
    if (fe_is_zero(&r) || fe_is_zero(&s)) return false;
    if (ge_mod(r.v, fn.m) || ge_mod(s.v, fn.m)) return false;

    /* The key, checked for being a real point before it is used. */
    fe qx_raw, qy_raw, qx, qy;
    fe_from_bytes(&qx_raw, pubkey + 1);
    fe_from_bytes(&qy_raw, pubkey + 33);
    if (ge_mod(qx_raw.v, fp.m) || ge_mod(qy_raw.v, fp.m)) return false;
    to_mont(&qx, &qx_raw, &fp);
    to_mont(&qy, &qy_raw, &fp);
    if (!on_curve(&qx, &qy, &fp)) return false;

    /* The hash, taken as a number and reduced into the scalar field. For
       this curve the hash and the order are the same width, so this is a
       conditional subtract rather than a truncation. */
    fe e;
    fe_from_bytes(&e, hash);
    if (ge_mod(e.v, fn.m)) sub_in_place(e.v, fn.m);

    /* u1 = e/s, u2 = r/s, both modulo the order. */
    fe s_m, si, e_m, r_m, u1, u2;
    to_mont(&s_m, &s, &fn);
    fe_inv(&si, &s_m, &fn);
    to_mont(&e_m, &e, &fn);
    to_mont(&r_m, &r, &fn);
    mont_mul(&u1, &e_m, &si, &fn);
    mont_mul(&u2, &r_m, &si, &fn);
    from_mont(&u1, &u1, &fn);
    from_mont(&u2, &u2, &fn);

    /* R = u1*G + u2*Q */
    point_t g, q, p1, p2, sum;
    fe gx_raw, gy_raw;
    fe_from_bytes(&gx_raw, GX);
    fe_from_bytes(&gy_raw, GY);
    to_mont(&g.x, &gx_raw, &fp);
    to_mont(&g.y, &gy_raw, &fp);
    g.z = fp.one;

    q.x = qx; q.y = qy; q.z = fp.one;

    pt_mul(&p1, &u1, &g, &fp);
    pt_mul(&p2, &u2, &q, &fp);
    pt_add(&sum, &p1, &p2, &fp);

    fe xm, x;
    if (!pt_affine_x(&xm, &sum, &fp)) return false;
    from_mont(&x, &xm, &fp);

    /* The check is on the x coordinate reduced modulo the order, not
       modulo the prime, which is the other place these two get confused. */
    if (ge_mod(x.v, fn.m)) sub_in_place(x.v, fn.m);
    return fe_eq(&x, &r);
}

bool p256_base_x(const u8 k[P256_SCALAR_LEN], u8 out_x[32]) {
    field_t fp;
    field_setup(&fp, P_M);

    point_t g, r;
    fe gx_raw, gy_raw;
    fe_from_bytes(&gx_raw, GX);
    fe_from_bytes(&gy_raw, GY);
    to_mont(&g.x, &gx_raw, &fp);
    to_mont(&g.y, &gy_raw, &fp);
    g.z = fp.one;

    fe scalar;
    fe_from_bytes(&scalar, k);
    pt_mul(&r, &scalar, &g, &fp);

    fe xm, x;
    if (!pt_affine_x(&xm, &r, &fp)) return false;
    from_mont(&x, &xm, &fp);
    fe_to_bytes(out_x, &x);
    return true;
}
