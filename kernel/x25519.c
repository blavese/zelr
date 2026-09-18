/* X25519, following RFC 7748.
 *
 * A field element is five limbs of fifty one bits, little end first, held
 * loosely: a limb is allowed to run slightly over during a calculation and
 * is brought back into range by the carry step. Only the conversion to
 * bytes insists on a fully reduced number, which is the one place it
 * matters, because that is what goes on the wire.
 *
 * Nothing here branches on a secret, and the scalar is walked a fixed
 * number of times whatever it contains. The conditional swap at the heart
 * of the ladder is written as a mask rather than an if, because an if on a
 * key bit is a key bit that can be read off the timing. */
#include "x25519.h"
#include "string.h"

typedef unsigned __int128 u128;
typedef u64 fe[5];

#define MASK51 0x7ffffffffffffULL

static void fe_zero(fe h) { for (int i = 0; i < 5; i++) h[i] = 0; }
static void fe_one(fe h)  { fe_zero(h); h[0] = 1; }
static void fe_copy(fe h, const fe f) { for (int i = 0; i < 5; i++) h[i] = f[i]; }

static void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 5; i++) h[i] = f[i] + g[i];
}

/* Subtraction adds twice the modulus first, so a limb never goes below
   zero: these are unsigned, and a borrow would wrap to an enormous value
   that the carry step cannot undo. */
static void fe_sub(fe h, const fe f, const fe g) {
    h[0] = f[0] + 0xfffffffffffdaULL - g[0];
    h[1] = f[1] + 0xffffffffffffeULL - g[1];
    h[2] = f[2] + 0xffffffffffffeULL - g[2];
    h[3] = f[3] + 0xffffffffffffeULL - g[3];
    h[4] = f[4] + 0xffffffffffffeULL - g[4];
}

static void fe_carry(fe h) {
    u64 c;
    c = h[0] >> 51; h[0] &= MASK51; h[1] += c;
    c = h[1] >> 51; h[1] &= MASK51; h[2] += c;
    c = h[2] >> 51; h[2] &= MASK51; h[3] += c;
    c = h[3] >> 51; h[3] &= MASK51; h[4] += c;
    c = h[4] >> 51; h[4] &= MASK51; h[0] += c * 19;
    c = h[0] >> 51; h[0] &= MASK51; h[1] += c;
}

/* The reduction: anything above bit 255 comes back in multiplied by 19,
   because 2^255 is congruent to 19. That is the whole reason for this
   particular prime. */
static void fe_mul(fe h, const fe f, const fe g) {
    u128 t[5];
    t[0] = (u128)f[0] * g[0]
         + 19 * ((u128)f[1] * g[4] + (u128)f[2] * g[3]
               + (u128)f[3] * g[2] + (u128)f[4] * g[1]);
    t[1] = (u128)f[0] * g[1] + (u128)f[1] * g[0]
         + 19 * ((u128)f[2] * g[4] + (u128)f[3] * g[3] + (u128)f[4] * g[2]);
    t[2] = (u128)f[0] * g[2] + (u128)f[1] * g[1] + (u128)f[2] * g[0]
         + 19 * ((u128)f[3] * g[4] + (u128)f[4] * g[3]);
    t[3] = (u128)f[0] * g[3] + (u128)f[1] * g[2] + (u128)f[2] * g[1]
         + (u128)f[3] * g[0] + 19 * ((u128)f[4] * g[4]);
    t[4] = (u128)f[0] * g[4] + (u128)f[1] * g[3] + (u128)f[2] * g[2]
         + (u128)f[3] * g[1] + (u128)f[4] * g[0];

    u64 out[5];
    u64 c;
    t[1] += (u64)(t[0] >> 51); out[0] = (u64)t[0] & MASK51;
    t[2] += (u64)(t[1] >> 51); out[1] = (u64)t[1] & MASK51;
    t[3] += (u64)(t[2] >> 51); out[2] = (u64)t[2] & MASK51;
    t[4] += (u64)(t[3] >> 51); out[3] = (u64)t[3] & MASK51;
    c     = (u64)(t[4] >> 51); out[4] = (u64)t[4] & MASK51;

    out[0] += c * 19;
    c = out[0] >> 51; out[0] &= MASK51; out[1] += c;
    c = out[1] >> 51; out[1] &= MASK51; out[2] += c;

    for (int i = 0; i < 5; i++) h[i] = out[i];
}

static void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }

static void fe_mul121665(fe h, const fe f) {
    u128 t[5];
    for (int i = 0; i < 5; i++) t[i] = (u128)f[i] * 121665;

    u64 out[5];
    u64 c;
    t[1] += (u64)(t[0] >> 51); out[0] = (u64)t[0] & MASK51;
    t[2] += (u64)(t[1] >> 51); out[1] = (u64)t[1] & MASK51;
    t[3] += (u64)(t[2] >> 51); out[2] = (u64)t[2] & MASK51;
    t[4] += (u64)(t[3] >> 51); out[3] = (u64)t[3] & MASK51;
    c     = (u64)(t[4] >> 51); out[4] = (u64)t[4] & MASK51;

    out[0] += c * 19;
    c = out[0] >> 51; out[0] &= MASK51; out[1] += c;

    for (int i = 0; i < 5; i++) h[i] = out[i];
}

/* Swaps two elements when the bit is one, and does exactly the same amount
   of work when it is zero. */
static void fe_cswap(fe a, fe b, u64 bit) {
    u64 mask = 0 - bit;
    for (int i = 0; i < 5; i++) {
        u64 x = mask & (a[i] ^ b[i]);
        a[i] ^= x;
        b[i] ^= x;
    }
}

static void fe_frombytes(fe h, const u8 s[32]) {
    u64 w[4];
    for (int i = 0; i < 4; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) w[i] |= (u64)s[i * 8 + j] << (j * 8);
    }
    /* The top bit of the last byte is ignored, as the specification
       requires: a peer that sets it is not describing a different point. */
    h[0] = w[0] & MASK51;
    h[1] = ((w[0] >> 51) | (w[1] << 13)) & MASK51;
    h[2] = ((w[1] >> 38) | (w[2] << 26)) & MASK51;
    h[3] = ((w[2] >> 25) | (w[3] << 39)) & MASK51;
    h[4] = (w[3] >> 12) & MASK51;
}

static void fe_tobytes(u8 s[32], const fe f) {
    fe t;
    fe_copy(t, f);
    fe_carry(t);

    /* t is now below 2^255 + a little. It may still be one of the values
       between p and 2^255, which are the same number written differently,
       so work out whether adding 19 carries out of the top and subtract the
       modulus if it does. */
    u64 q;
    q = (t[0] + 19) >> 51;
    q = (t[1] + q) >> 51;
    q = (t[2] + q) >> 51;
    q = (t[3] + q) >> 51;
    q = (t[4] + q) >> 51;

    t[0] += 19 * q;
    u64 c;
    c = t[0] >> 51; t[0] &= MASK51; t[1] += c;
    c = t[1] >> 51; t[1] &= MASK51; t[2] += c;
    c = t[2] >> 51; t[2] &= MASK51; t[3] += c;
    c = t[3] >> 51; t[3] &= MASK51; t[4] += c;
    t[4] &= MASK51;

    u64 w[4];
    w[0] = t[0]        | (t[1] << 51);
    w[1] = (t[1] >> 13) | (t[2] << 38);
    w[2] = (t[2] >> 26) | (t[3] << 25);
    w[3] = (t[3] >> 39) | (t[4] << 12);

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) s[i * 8 + j] = (u8)(w[i] >> (j * 8));
}

/* The inverse, as z to the power p - 2, by an addition chain that gets
   there in 254 squarings and 11 multiplications rather than 254 of each. */
static void fe_invert(fe out, const fe z) {
    fe z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t;
    int i;

    fe_sq(z2, z);
    fe_sq(t, z2); fe_sq(t, t);
    fe_mul(z9, t, z);
    fe_mul(z11, z9, z2);
    fe_sq(t, z11);
    fe_mul(z2_5_0, t, z9);

    fe_sq(t, z2_5_0); for (i = 1; i < 5; i++) fe_sq(t, t);
    fe_mul(z2_10_0, t, z2_5_0);

    fe_sq(t, z2_10_0); for (i = 1; i < 10; i++) fe_sq(t, t);
    fe_mul(z2_20_0, t, z2_10_0);

    fe_sq(t, z2_20_0); for (i = 1; i < 20; i++) fe_sq(t, t);
    fe_mul(t, t, z2_20_0);

    for (i = 0; i < 10; i++) fe_sq(t, t);
    fe_mul(z2_50_0, t, z2_10_0);

    fe_sq(t, z2_50_0); for (i = 1; i < 50; i++) fe_sq(t, t);
    fe_mul(z2_100_0, t, z2_50_0);

    fe_sq(t, z2_100_0); for (i = 1; i < 100; i++) fe_sq(t, t);
    fe_mul(t, t, z2_100_0);

    for (i = 0; i < 50; i++) fe_sq(t, t);
    fe_mul(t, t, z2_50_0);

    for (i = 0; i < 5; i++) fe_sq(t, t);
    fe_mul(out, t, z11);
}

/* The Montgomery ladder. Every bit of the scalar does the same work, and
   which of the two running points is which is decided by a swap rather
   than by a branch, so the sequence of operations does not depend on the
   key. */
static void ladder(const u8 scalar[32], const fe x1, fe x2, fe z2) {
    fe x3, z3, a, aa, b, bb, e, c, d, da, cb, t;

    fe_one(x2);  fe_zero(z2);
    fe_copy(x3, x1); fe_one(z3);

    u64 swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        u64 bit = (scalar[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(a, x2, z2);   fe_carry(a);
        fe_sq(aa, a);
        fe_sub(b, x2, z2);   fe_carry(b);
        fe_sq(bb, b);
        fe_sub(e, aa, bb);   fe_carry(e);
        fe_add(c, x3, z3);   fe_carry(c);
        fe_sub(d, x3, z3);   fe_carry(d);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);

        fe_add(t, da, cb);   fe_carry(t);
        fe_sq(x3, t);
        fe_sub(t, da, cb);   fe_carry(t);
        fe_sq(t, t);
        fe_mul(z3, x1, t);

        fe_mul(x2, aa, bb);
        fe_mul121665(t, e);
        fe_add(t, t, aa);    fe_carry(t);
        fe_mul(z2, e, t);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
}

static void clamp(u8 k[32], const u8 secret[32]) {
    memcpy(k, secret, 32);
    /* Clearing the low three bits puts the result in the prime order
       subgroup; the top two make the scalar a fixed length so the ladder
       runs the same way every time. */
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
}

static bool all_zero(const u8 p[32]) {
    u8 acc = 0;
    for (int i = 0; i < 32; i++) acc |= p[i];
    return acc == 0;
}

bool x25519(const u8 secret[X25519_LEN], const u8 peer[X25519_LEN],
            u8 out[X25519_LEN]) {
    u8 k[32];
    fe x1, x2, z2, zi;

    clamp(k, secret);
    fe_frombytes(x1, peer);
    ladder(k, x1, x2, z2);

    fe_invert(zi, z2);
    fe_mul(x2, x2, zi);
    fe_tobytes(out, x2);

    /* A handful of peer values drive the result to zero whatever the
       private scalar was, which is a peer choosing the key rather than
       agreeing one. The specification says a client may refuse, and a
       client that does not is not agreeing a secret at all. */
    if (all_zero(out)) {
        memset(out, 0, X25519_LEN);
        return false;
    }
    return true;
}

void x25519_public(const u8 secret[X25519_LEN], u8 out[X25519_LEN]) {
    static const u8 base[32] = { 9 };
    /* The base point is not a small order point, so this cannot fail, and
       the return is ignored rather than checked for something that cannot
       happen. */
    (void)x25519(secret, base, out);
}
