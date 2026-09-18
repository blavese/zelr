/* SHA-256, HMAC-SHA256 and HKDF.
 *
 * The compression function is written out as the specification states it
 * rather than unrolled, because the unrolled form is faster to run and much
 * harder to check against the document it came from, and this is a thing
 * whose only defence is that it agrees with everybody else's copy. */
#include "sha256.h"
#include "string.h"

static const u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static u32 ror(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

static void compress(u32 h[8], const u8 block[64]) {
    u32 w[64];
    for (u32 i = 0; i < 16; i++)
        w[i] = ((u32)block[i * 4] << 24) | ((u32)block[i * 4 + 1] << 16)
             | ((u32)block[i * 4 + 2] << 8) | (u32)block[i * 4 + 3];
    for (u32 i = 16; i < 64; i++) {
        u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u32 a = h[0], b = h[1], c = h[2], d = h[3];
    u32 e = h[4], f = h[5], g = h[6], hh = h[7];

    for (u32 i = 0; i < 64; i++) {
        u32 S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = hh + S1 + ch + K[i] + w[i];
        u32 S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_init(sha256_t *s) {
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85;
    s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c;
    s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->bits = 0;
    s->have = 0;
}

void sha256_update(sha256_t *s, const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    s->bits += (u64)len * 8;
    while (len) {
        u32 room = SHA256_BLOCK - s->have;
        u32 take = len < room ? len : room;
        memcpy(s->buf + s->have, p, take);
        s->have += take;
        p += take;
        len -= take;
        if (s->have == SHA256_BLOCK) {
            compress(s->h, s->buf);
            s->have = 0;
        }
    }
}

void sha256_final(sha256_t *s, u8 out[SHA256_SIZE]) {
    u64 bits = s->bits;
    u8 pad = 0x80;
    sha256_update(s, &pad, 1);
    /* The length has to land in the last eight bytes of a block, so pad with
       zeros until there is exactly that much room left. */
    u8 zero = 0;
    while (s->have != SHA256_BLOCK - 8) sha256_update(s, &zero, 1);
    for (int i = 7; i >= 0; i--) {
        u8 b = (u8)(bits >> (i * 8));
        memcpy(s->buf + s->have, &b, 1);
        s->have++;
    }
    compress(s->h, s->buf);
    for (u32 i = 0; i < 8; i++) {
        out[i * 4]     = (u8)(s->h[i] >> 24);
        out[i * 4 + 1] = (u8)(s->h[i] >> 16);
        out[i * 4 + 2] = (u8)(s->h[i] >> 8);
        out[i * 4 + 3] = (u8)(s->h[i]);
    }
}

void sha256(const void *data, u32 len, u8 out[SHA256_SIZE]) {
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, out);
}

void hmac_sha256(const u8 *key, u32 key_len,
                 const u8 *data, u32 data_len, u8 out[SHA256_SIZE]) {
    u8 k[SHA256_BLOCK];
    u8 inner[SHA256_SIZE];
    sha256_t s;

    memset(k, 0, sizeof(k));
    /* A key longer than a block is replaced by its hash, which is the one
       place a careless implementation silently accepts two different keys as
       the same one. */
    if (key_len > SHA256_BLOCK) sha256(key, key_len, k);
    else                        memcpy(k, key, key_len);

    u8 pad[SHA256_BLOCK];
    for (u32 i = 0; i < SHA256_BLOCK; i++) pad[i] = (u8)(k[i] ^ 0x36);
    sha256_init(&s);
    sha256_update(&s, pad, SHA256_BLOCK);
    sha256_update(&s, data, data_len);
    sha256_final(&s, inner);

    for (u32 i = 0; i < SHA256_BLOCK; i++) pad[i] = (u8)(k[i] ^ 0x5c);
    sha256_init(&s);
    sha256_update(&s, pad, SHA256_BLOCK);
    sha256_update(&s, inner, SHA256_SIZE);
    sha256_final(&s, out);
}

void hkdf_extract(const u8 *salt, u32 salt_len,
                  const u8 *ikm, u32 ikm_len, u8 out[SHA256_SIZE]) {
    /* Extract is HMAC with the roles the other way round: the salt is the
       key and the secret is the data. */
    static const u8 zeros[SHA256_SIZE] = { 0 };
    if (!salt || !salt_len) { salt = zeros; salt_len = SHA256_SIZE; }
    hmac_sha256(salt, salt_len, ikm, ikm_len, out);
}

bool hkdf_expand(const u8 prk[SHA256_SIZE], const u8 *info, u32 info_len,
                 u8 *out, u32 out_len) {
    if (out_len > 255 * SHA256_SIZE) return false;

    u8 block[SHA256_SIZE];
    u32 have = 0;
    u8 counter = 1;

    while (have < out_len) {
        /* Each block is the hash of the one before it, the info and the
           counter. The counter is why two lengths of output starting the
           same way are not a problem. */
        u8 msg[SHA256_SIZE + 256 + 1];
        u32 n = 0;
        if (counter > 1) { memcpy(msg, block, SHA256_SIZE); n = SHA256_SIZE; }
        if (info_len > 256) return false;
        memcpy(msg + n, info, info_len);
        n += info_len;
        msg[n++] = counter;

        hmac_sha256(prk, SHA256_SIZE, msg, n, block);

        u32 take = out_len - have;
        if (take > SHA256_SIZE) take = SHA256_SIZE;
        memcpy(out + have, block, take);
        have += take;
        counter++;
    }
    return true;
}

bool hkdf_expand_label(const u8 secret[SHA256_SIZE], const char *label,
                       const u8 *ctx, u32 ctx_len, u8 *out, u32 out_len) {
    /* struct {
           uint16 length;
           opaque label<7..255>;      "tls13 " and then the name
           opaque context<0..255>;
       } HkdfLabel; */
    u8 info[2 + 1 + 6 + 32 + 1 + 64];
    u32 n = 0;

    u32 label_len = 0;
    while (label[label_len]) label_len++;
    if (label_len > 32 || ctx_len > 64) return false;

    info[n++] = (u8)(out_len >> 8);
    info[n++] = (u8)(out_len);
    info[n++] = (u8)(6 + label_len);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, label_len);
    n += label_len;
    info[n++] = (u8)ctx_len;
    if (ctx_len) memcpy(info + n, ctx, ctx_len);
    n += ctx_len;

    return hkdf_expand(secret, info, n, out, out_len);
}

bool tls13_derive_secret(const u8 secret[SHA256_SIZE], const char *label,
                         const u8 *transcript_hash, u32 hash_len,
                         u8 out[SHA256_SIZE]) {
    return hkdf_expand_label(secret, label, transcript_hash, hash_len,
                             out, SHA256_SIZE);
}
