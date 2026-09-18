/* RSA verification: the big integers, and the two paddings.
 *
 * Verifying is raising the signature to the public exponent modulo the
 * modulus, which recovers a block that should have a particular shape with
 * the hash inside it. Everything hangs on checking that shape properly. A
 * verifier that finds the hash somewhere in the block and is satisfied
 * accepts forged signatures, and has, famously, more than once. So the
 * padding is checked byte for byte along its whole length here, and the
 * only thing the hash is compared against is where it is required to be.
 *
 * Montgomery multiplication does the modular arithmetic, because it turns a
 * reduction into a shift. The constant it needs is worked out by doubling
 * rather than by dividing, which avoids writing long division altogether.
 *
 * None of this is secret: the modulus, the exponent and the signature are
 * all published. So none of it is written to hide its timing. */
#include "rsa.h"
#include "sha256.h"
#include "string.h"

typedef unsigned __int128 u128;

#define LIMBS (RSA_MAX_BYTES / 8)

typedef struct {
    u64 v[LIMBS];           /* little end first */
    u32 len;                /* limbs in use */
} bn_t;

static void bn_zero(bn_t *a) {
    for (u32 i = 0; i < LIMBS; i++) a->v[i] = 0;
    a->len = 0;
}

/* Big end first bytes, as every certificate stores them, into limbs. */
static bool bn_from_bytes(bn_t *a, const u8 *b, u32 len, u32 want_limbs) {
    if (want_limbs > LIMBS) return false;
    bn_zero(a);
    a->len = want_limbs;
    if (len > want_limbs * 8) return false;
    for (u32 i = 0; i < len; i++) {
        u32 pos = len - 1 - i;              /* byte i from the little end */
        a->v[pos / 8] |= (u64)b[i] << ((pos % 8) * 8);
    }
    return true;
}

static void bn_to_bytes(const bn_t *a, u8 *out, u32 len) {
    for (u32 i = 0; i < len; i++) {
        u32 pos = len - 1 - i;
        out[i] = (pos / 8 < a->len) ? (u8)(a->v[pos / 8] >> ((pos % 8) * 8)) : 0;
    }
}

/* Is a at least as big as b, both the same length. */
static bool bn_ge(const bn_t *a, const bn_t *b) {
    for (u32 i = a->len; i-- > 0; ) {
        if (a->v[i] != b->v[i]) return a->v[i] > b->v[i];
    }
    return true;
}

/* a -= b, which the callers only ever do when the answer is not negative. */
static void bn_sub(bn_t *a, const bn_t *b) {
    u64 borrow = 0;
    for (u32 i = 0; i < a->len; i++) {
        u64 x = a->v[i], y = b->v[i];
        u64 d = x - y - borrow;
        borrow = (x < y + borrow) || (borrow && y + borrow == 0);
        a->v[i] = d;
    }
}

/* a = 2a mod n, given a < n. Doubling and a conditional subtract, which is
   how the Montgomery constant gets worked out without long division. */
static void bn_dbl_mod(bn_t *a, const bn_t *n) {
    u64 carry = 0;
    for (u32 i = 0; i < a->len; i++) {
        u64 x = a->v[i];
        a->v[i] = (x << 1) | carry;
        carry = x >> 63;
    }
    /* The doubled value can be one bit wider than the modulus, so a single
       subtract is enough, and the carry out says it definitely overflowed. */
    if (carry || bn_ge(a, n)) bn_sub(a, n);
}

/* -n^-1 mod 2^64, by Newton's method: each step doubles the number of
   correct bits, and one is correct to start with because n is odd. */
static u64 n0_inverse(u64 n0) {
    u64 inv = 1;
    for (int i = 0; i < 6; i++) inv *= 2 - n0 * inv;
    return 0 - inv;
}

/* out = a * b * R^-1 mod n, the coarsely integrated operand scanning form. */
static void mont_mul(bn_t *out, const bn_t *a, const bn_t *b,
                     const bn_t *n, u64 n0inv) {
    u32 len = n->len;
    u64 t[LIMBS + 2];
    for (u32 i = 0; i < len + 2; i++) t[i] = 0;

    for (u32 i = 0; i < len; i++) {
        u64 carry = 0;
        for (u32 j = 0; j < len; j++) {
            u128 p = (u128)a->v[j] * b->v[i] + t[j] + carry;
            t[j] = (u64)p;
            carry = (u64)(p >> 64);
        }
        u128 s = (u128)t[len] + carry;
        t[len] = (u64)s;
        t[len + 1] = (u64)(s >> 64);

        /* The multiple of n that makes the bottom limb vanish, which is
           what lets the whole thing shift down by a limb. */
        u64 m = t[0] * n0inv;
        u128 p = (u128)m * n->v[0] + t[0];
        carry = (u64)(p >> 64);
        for (u32 j = 1; j < len; j++) {
            p = (u128)m * n->v[j] + t[j] + carry;
            t[j - 1] = (u64)p;
            carry = (u64)(p >> 64);
        }
        s = (u128)t[len] + carry;
        t[len - 1] = (u64)s;
        t[len] = t[len + 1] + (u64)(s >> 64);
    }

    bn_zero(out);
    out->len = len;
    for (u32 i = 0; i < len; i++) out->v[i] = t[i];
    if (t[len] || bn_ge(out, n)) bn_sub(out, n);
}

/* out = base^e mod n, with e small and public. */
static void bn_mod_exp(bn_t *out, const bn_t *base, u32 e, const bn_t *n) {
    u64 n0inv = n0_inverse(n->v[0]);
    u32 len = n->len;

    /* R^2 mod n, by starting at one and doubling as many times as R^2 has
       bits. Slower than a division would be and about fifteen lines
       shorter, and it happens once. */
    bn_t r2;
    bn_zero(&r2);
    r2.len = len;
    r2.v[0] = 1;
    for (u32 i = 0; i < 2 * len * 64; i++) bn_dbl_mod(&r2, n);

    bn_t base_m, acc, one;
    mont_mul(&base_m, base, &r2, n, n0inv);

    bn_zero(&one);
    one.len = len;
    one.v[0] = 1;
    mont_mul(&acc, &one, &r2, n, n0inv);     /* R mod n, the Montgomery one */

    /* Square and multiply, top bit first. */
    int top = 31;
    while (top > 0 && !((e >> top) & 1)) top--;
    for (int i = top; i >= 0; i--) {
        bn_t sq;
        mont_mul(&sq, &acc, &acc, n, n0inv);
        if ((e >> i) & 1) mont_mul(&acc, &sq, &base_m, n, n0inv);
        else              acc = sq;
    }

    mont_mul(out, &acc, &one, n, n0inv);     /* back out of Montgomery form */
}

/* The signature raised to the public exponent, giving the padded block. */
static bool rsa_public(const rsa_key_t *key, const u8 *sig, u32 sig_len,
                       u8 *em, u32 em_len) {
    if (key->n_len == 0 || key->n_len > RSA_MAX_BYTES) return false;
    if (sig_len != key->n_len || em_len != key->n_len) return false;
    if (key->e < 3 || !(key->e & 1)) return false;

    u32 limbs = (key->n_len + 7) / 8;
    bn_t n, s, m;
    if (!bn_from_bytes(&n, key->n, key->n_len, limbs)) return false;
    if (!bn_from_bytes(&s, sig, sig_len, limbs)) return false;
    if (!(n.v[0] & 1)) return false;                  /* even modulus: not a key */
    if (bn_ge(&s, &n)) return false;                  /* signature out of range */

    bn_mod_exp(&m, &s, key->e, &n);
    bn_to_bytes(&m, em, em_len);
    return true;
}

/* Compares without an early exit, so nothing is learned from how long it
   took to disagree. */
static bool same(const u8 *a, const u8 *b, u32 len) {
    u8 diff = 0;
    for (u32 i = 0; i < len; i++) diff |= (u8)(a[i] ^ b[i]);
    return diff == 0;
}

/* The prefix a SHA-256 digest is wrapped in for PKCS#1 v1.5: the DER for
   "this is a sha-256 hash", written out rather than parsed, because the
   only correct value is this one and parsing it invites accepting others. */
static const u8 SHA256_DER[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
    0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
};

bool rsa_verify_pkcs1(const rsa_key_t *key, const u8 *sig, u32 sig_len,
                      const u8 *hash, u32 hash_len) {
    if (hash_len != SHA256_SIZE) return false;

    u8 em[RSA_MAX_BYTES];
    if (!rsa_public(key, sig, sig_len, em, key->n_len)) return false;

    /* 0x00 0x01 <0xff...> 0x00 <der> <hash>, and every one of those bytes
       is checked. The attacks on this are all about a verifier that looks
       for the hash and does not mind what is in front of it. */
    u32 want = 3 + sizeof(SHA256_DER) + SHA256_SIZE;   /* 00 01 ... 00 */
    if (key->n_len < want + 8) return false;           /* too little padding */

    u32 pad_len = key->n_len - 2 - 1 - sizeof(SHA256_DER) - SHA256_SIZE;

    u8 bad = 0;
    bad |= em[0];
    bad |= (u8)(em[1] ^ 0x01);
    for (u32 i = 0; i < pad_len; i++) bad |= (u8)(em[2 + i] ^ 0xff);
    bad |= em[2 + pad_len];
    if (bad) return false;

    const u8 *p = em + 3 + pad_len;
    if (!same(p, SHA256_DER, sizeof(SHA256_DER))) return false;
    return same(p + sizeof(SHA256_DER), hash, SHA256_SIZE);
}

/* MGF1, the mask generating function PSS is defined in terms of: the hash
   of the seed and a counter, over and over, until there are enough bytes. */
static void mgf1(const u8 *seed, u32 seed_len, u8 *mask, u32 mask_len) {
    u32 done = 0;
    u32 counter = 0;
    while (done < mask_len) {
        u8 c[4] = { (u8)(counter >> 24), (u8)(counter >> 16),
                    (u8)(counter >> 8), (u8)counter };
        u8 block[SHA256_SIZE];
        sha256_t s;
        sha256_init(&s);
        sha256_update(&s, seed, seed_len);
        sha256_update(&s, c, 4);
        sha256_final(&s, block);

        u32 take = mask_len - done;
        if (take > SHA256_SIZE) take = SHA256_SIZE;
        memcpy(mask + done, block, take);
        done += take;
        counter++;
    }
}

bool rsa_verify_pss(const rsa_key_t *key, const u8 *sig, u32 sig_len,
                    const u8 *hash, u32 hash_len) {
    if (hash_len != SHA256_SIZE) return false;

    u8 em[RSA_MAX_BYTES];
    u32 em_len = key->n_len;
    if (!rsa_public(key, sig, sig_len, em, em_len)) return false;

    /* TLS 1.3 fixes the salt length at the hash length, so there is one
       shape to accept rather than a range. */
    const u32 h_len = SHA256_SIZE, s_len = SHA256_SIZE;
    if (em_len < h_len + s_len + 2) return false;

    if (em[em_len - 1] != 0xbc) return false;

    const u8 *masked_db = em;
    const u8 *h = em + em_len - h_len - 1;
    u32 db_len = em_len - h_len - 1;

    /* The modulus is a whole number of bytes here, so the top bit of the
       block has to be clear rather than being part of the value. */
    if (em[0] & 0x80) return false;

    u8 db[RSA_MAX_BYTES];
    mgf1(h, h_len, db, db_len);
    for (u32 i = 0; i < db_len; i++) db[i] ^= masked_db[i];
    db[0] &= 0x7f;

    /* db is zeros, then a single 0x01, then the salt. Both parts are
       checked: a verifier that only looks for the 0x01 accepts blocks it
       should not. */
    u32 i = 0;
    while (i < db_len - s_len - 1 && db[i] == 0) i++;
    if (i != db_len - s_len - 1) return false;
    if (db[i] != 0x01) return false;

    const u8 *salt = db + db_len - s_len;

    /* The hash is over eight zero bytes, the message hash and the salt.
       Nothing else. */
    static const u8 eight_zeros[8] = { 0 };
    u8 want[SHA256_SIZE];
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, eight_zeros, 8);
    sha256_update(&s, hash, h_len);
    sha256_update(&s, salt, s_len);
    sha256_final(&s, want);

    return same(want, h, h_len);
}
