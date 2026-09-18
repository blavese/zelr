/* AES-GCM.
 *
 * Two things happen to every message. It is encrypted by exclusive-or with
 * the cipher run over a counter, which is ordinary and would be enough if
 * nobody were hostile. And it is hashed, along with the header, in a finite
 * field of 2^128 elements, and the result encrypted: that is the tag, and it
 * is what makes a changed message detectable rather than merely wrong.
 *
 * The field multiplication is written a bit at a time. Tables make it four
 * times faster and make the timing depend on the data, which is how keys
 * have been recovered from implementations that were otherwise correct.
 * A web page arriving in a tenth of a second rather than a fortieth is not
 * worth that. */
#include "gcm.h"
#include "string.h"

/* Multiplication in GF(2^128), in the bit order the specification uses:
   bit 0 of byte 0 is the highest power, and the reduction polynomial is
   x^128 + x^7 + x^2 + x + 1, which is why 0xe1 appears. */
static void gf_mul(u8 z[16], const u8 x[16], const u8 y[16]) {
    u8 v[16], acc[16];
    memcpy(v, y, 16);
    memset(acc, 0, 16);

    for (u32 i = 0; i < 128; i++) {
        /* Branch on the bit without branching: a conditional exclusive-or
           written as a mask, so both cases do the same work. */
        u8 bit = (u8)((x[i >> 3] >> (7 - (i & 7))) & 1);
        u8 mask = (u8)(0 - bit);
        for (u32 j = 0; j < 16; j++) acc[j] ^= (u8)(v[j] & mask);

        u8 lsb = (u8)(v[15] & 1);
        for (u32 j = 15; j > 0; j--)
            v[j] = (u8)((v[j] >> 1) | (u8)(v[j - 1] << 7));
        v[0] >>= 1;
        v[0] ^= (u8)(0xe1 & (u8)(0 - lsb));
    }
    memcpy(z, acc, 16);
}

/* The running hash: each block is added to what is there and the sum
   multiplied by the subkey. */
static void ghash_block(const gcm_t *g, u8 y[16], const u8 *block, u32 len) {
    u8 padded[16];
    memset(padded, 0, 16);
    memcpy(padded, block, len);
    for (u32 i = 0; i < 16; i++) y[i] ^= padded[i];
    gf_mul(y, y, g->h);
}

static void ghash(const gcm_t *g, const u8 *aad, u32 aad_len,
                  const u8 *ct, u32 ct_len, u8 out[16]) {
    u8 y[16];
    memset(y, 0, 16);

    for (u32 i = 0; i < aad_len; i += 16) {
        u32 n = aad_len - i < 16 ? aad_len - i : 16;
        ghash_block(g, y, aad + i, n);
    }
    for (u32 i = 0; i < ct_len; i += 16) {
        u32 n = ct_len - i < 16 ? ct_len - i : 16;
        ghash_block(g, y, ct + i, n);
    }

    /* And finally the two lengths, in bits, as sixty four bit numbers. This
       is what stops a block moving from the header into the body without
       being noticed. */
    u8 lens[16];
    u64 a_bits = (u64)aad_len * 8, c_bits = (u64)ct_len * 8;
    for (u32 i = 0; i < 8; i++) {
        lens[i]     = (u8)(a_bits >> (56 - i * 8));
        lens[8 + i] = (u8)(c_bits >> (56 - i * 8));
    }
    ghash_block(g, y, lens, 16);

    memcpy(out, y, 16);
}

/* Counter mode over the data, starting from the given counter block. */
static void ctr_xor(const gcm_t *g, u8 counter[16], u8 *data, u32 len) {
    u8 stream[16];
    for (u32 i = 0; i < len; i += 16) {
        /* The counter is the low four bytes, big endian, and it wraps
           within those four rather than carrying into the nonce. */
        for (int j = 15; j >= 12; j--)
            if (++counter[j]) break;

        aes_encrypt_block(&g->aes, counter, stream);
        u32 n = len - i < 16 ? len - i : 16;
        for (u32 j = 0; j < n; j++) data[i + j] ^= stream[j];
    }
}

bool gcm_init(gcm_t *g, const u8 *key, u32 bits) {
    if (!aes_set_key(&g->aes, key, bits)) return false;
    /* The hash subkey is the cipher applied to a block of zeros. */
    u8 zero[16];
    memset(zero, 0, 16);
    aes_encrypt_block(&g->aes, zero, g->h);
    return true;
}

/* The two counter blocks a twelve byte nonce implies: the tag is masked with
   the cipher over counter one, and the data starts at counter two. */
static void counter0(const u8 iv[GCM_IV], u8 out[16]) {
    memcpy(out, iv, GCM_IV);
    out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
}

void gcm_seal(const gcm_t *g, const u8 iv[GCM_IV],
              const u8 *aad, u32 aad_len,
              u8 *data, u32 len, u8 tag[GCM_TAG]) {
    u8 j0[16], counter[16], mask[16], h[16];

    counter0(iv, j0);
    aes_encrypt_block(&g->aes, j0, mask);

    memcpy(counter, j0, 16);
    ctr_xor(g, counter, data, len);

    ghash(g, aad, aad_len, data, len, h);
    for (u32 i = 0; i < GCM_TAG; i++) tag[i] = (u8)(h[i] ^ mask[i]);
}

bool gcm_open(const gcm_t *g, const u8 iv[GCM_IV],
              const u8 *aad, u32 aad_len,
              u8 *data, u32 len, const u8 tag[GCM_TAG]) {
    u8 j0[16], counter[16], mask[16], h[16], want[GCM_TAG];

    counter0(iv, j0);
    aes_encrypt_block(&g->aes, j0, mask);

    /* The tag is over the ciphertext, so it is checked before a single byte
       is decrypted. Decrypting first and checking after means the plaintext
       of a forged message exists in memory, and the bugs that leak it are
       the ordinary kind. */
    ghash(g, aad, aad_len, data, len, h);
    for (u32 i = 0; i < GCM_TAG; i++) want[i] = (u8)(h[i] ^ mask[i]);

    /* Compared without an early exit. An implementation that returns on the
       first wrong byte says how many were right, and a tag can be found one
       byte at a time from that. */
    u8 diff = 0;
    for (u32 i = 0; i < GCM_TAG; i++) diff |= (u8)(want[i] ^ tag[i]);
    if (diff) {
        memset(data, 0, len);
        return false;
    }

    memcpy(counter, j0, 16);
    ctr_xor(g, counter, data, len);
    return true;
}
