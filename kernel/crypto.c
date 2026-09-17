/* See include/crypto.h for what this is for.
 *
 * Written the way the specifications write it rather than the way a library
 * writes it. A fast AES is four tables of a thousand words that fold the
 * substitution and the mixing together, and it is unreadable: you cannot
 * look at it and see the cipher. This one does each step separately and is
 * perhaps a tenth the speed, which for a handshake that happens once when
 * joining a network is a tenth of nothing.
 */
#include "crypto.h"
#include "string.h"

/* --- SHA-1 ---------------------------------------------------------------- */

static u32 rol(u32 v, u32 n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(sha1_t *s, const u8 *p) {
    u32 w[80];

    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16)
             | ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];

    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }

        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void sha1_init(sha1_t *s) {
    s->h[0] = 0x67452301u;
    s->h[1] = 0xEFCDAB89u;
    s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xC3D2E1F0u;
    s->bits = 0;
    s->have = 0;
}

void sha1_update(sha1_t *s, const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    s->bits += (u64)len * 8;

    while (len) {
        u32 room = 64 - s->have;
        u32 take = len < room ? len : room;
        memcpy(s->buf + s->have, p, take);
        s->have += take;
        p += take;
        len -= take;
        if (s->have == 64) {
            sha1_block(s, s->buf);
            s->have = 0;
        }
    }
}

void sha1_final(sha1_t *s, u8 out[SHA1_SIZE]) {
    u64 bits = s->bits;

    /* A one bit, then zeros, then the length. The length has to land in the
       last eight bytes of a block, so when there is not room for it this
       fills the block out and uses the next one. */
    u8 one = 0x80;
    sha1_update(s, &one, 1);
    s->bits = bits;                       /* the padding is not message */

    while (s->have != 56) {
        u8 zero = 0;
        sha1_update(s, &zero, 1);
        s->bits = bits;
    }

    u8 tail[8];
    for (int i = 0; i < 8; i++) tail[i] = (u8)(bits >> (56 - i * 8));
    sha1_update(s, tail, 8);

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (u8)(s->h[i] >> 24);
        out[i * 4 + 1] = (u8)(s->h[i] >> 16);
        out[i * 4 + 2] = (u8)(s->h[i] >> 8);
        out[i * 4 + 3] = (u8)(s->h[i]);
    }
}

void sha1(const void *data, u32 len, u8 out[SHA1_SIZE]) {
    sha1_t s;
    sha1_init(&s);
    sha1_update(&s, data, len);
    sha1_final(&s, out);
}

/* --- HMAC-SHA1 ------------------------------------------------------------ */

void hmac_sha1_vector(const u8 *key, u32 key_len,
                      const u8 *const *parts, const u32 *lens, u32 n,
                      u8 out[SHA1_SIZE]) {
    u8 k[64];
    u8 inner[SHA1_SIZE];
    sha1_t s;

    /* A key longer than a block is replaced by its hash, which is the one
       place a specification says to do something that looks like a mistake
       and is not. */
    memset(k, 0, sizeof(k));
    if (key_len > 64) sha1(key, key_len, k);
    else              memcpy(k, key, key_len);

    u8 pad[64];
    for (int i = 0; i < 64; i++) pad[i] = (u8)(k[i] ^ 0x36);
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    for (u32 i = 0; i < n; i++) sha1_update(&s, parts[i], lens[i]);
    sha1_final(&s, inner);

    for (int i = 0; i < 64; i++) pad[i] = (u8)(k[i] ^ 0x5C);
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, inner, SHA1_SIZE);
    sha1_final(&s, out);
}

void hmac_sha1(const u8 *key, u32 key_len,
               const u8 *data, u32 data_len, u8 out[SHA1_SIZE]) {
    const u8 *parts[1] = { data };
    u32 lens[1] = { data_len };
    hmac_sha1_vector(key, key_len, parts, lens, 1, out);
}

/* --- PBKDF2 --------------------------------------------------------------- */

void pbkdf2_sha1(const char *password, const u8 *salt, u32 salt_len,
                 u32 iterations, u8 *out, u32 out_len) {
    u32 pass_len = 0;
    while (password[pass_len]) pass_len++;

    u32 block = 1;
    while (out_len) {
        /* Each block is the hash of the salt with the block number after it,
           and then that hash hashed again, the whole count, every round
           folded into the answer. */
        u8 counter[4] = { (u8)(block >> 24), (u8)(block >> 16),
                          (u8)(block >> 8), (u8)block };
        const u8 *parts[2] = { salt, counter };
        u32 lens[2] = { salt_len, 4 };

        u8 u[SHA1_SIZE], acc[SHA1_SIZE];
        hmac_sha1_vector((const u8 *)password, pass_len, parts, lens, 2, u);
        memcpy(acc, u, SHA1_SIZE);

        for (u32 i = 1; i < iterations; i++) {
            hmac_sha1((const u8 *)password, pass_len, u, SHA1_SIZE, u);
            for (int j = 0; j < SHA1_SIZE; j++) acc[j] ^= u[j];
        }

        u32 take = out_len < SHA1_SIZE ? out_len : SHA1_SIZE;
        memcpy(out, acc, take);
        out += take;
        out_len -= take;
        block++;
    }
}

/* --- AES ------------------------------------------------------------------ */

static const u8 SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static u8 INV_SBOX[256];
static bool inv_ready;

static void build_inverse(void) {
    if (inv_ready) return;
    for (int i = 0; i < 256; i++) INV_SBOX[SBOX[i]] = (u8)i;
    inv_ready = true;
}

/* Multiplying by x in the field the cipher is defined over: a shift, and if
   it went off the top, the polynomial folded back in. Every other
   multiplication is built out of this one. */
static u8 xtime(u8 v) {
    return (u8)((v << 1) ^ ((v & 0x80) ? 0x1B : 0x00));
}

static u8 mul(u8 a, u8 b) {
    u8 r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

bool aes_set_key(aes_t *a, const u8 *key, u32 bits) {
    u32 nk;
    if (bits == 128)      { nk = 4;  a->rounds = 10; }
    else if (bits == 256) { nk = 8;  a->rounds = 14; }
    else                  { a->rounds = 0; return false; }

    build_inverse();

    u32 total = 4 * (u32)(a->rounds + 1);       /* words in the schedule */
    memcpy(a->round_key, key, nk * 4);

    u8 rcon = 1;
    for (u32 i = nk; i < total; i++) {
        u8 t[4];
        memcpy(t, a->round_key + (i - 1) * 4, 4);

        if (i % nk == 0) {
            u8 tmp = t[0];
            t[0] = (u8)(SBOX[t[1]] ^ rcon);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[tmp];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = SBOX[t[j]];
        }

        for (int j = 0; j < 4; j++)
            a->round_key[i * 4 + j] = (u8)(a->round_key[(i - nk) * 4 + j] ^ t[j]);
    }
    return true;
}

static void add_round_key(u8 s[16], const u8 *rk) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

/* The state is a column at a time, so byte i is row i%4 of column i/4, and
   shifting a row means moving bytes four apart. */
static void shift_rows(u8 s[16]) {
    u8 t[16];
    memcpy(t, s, 16);
    for (int r = 1; r < 4; r++)
        for (int c = 0; c < 4; c++)
            s[c * 4 + r] = t[((c + r) % 4) * 4 + r];
}

static void inv_shift_rows(u8 s[16]) {
    u8 t[16];
    memcpy(t, s, 16);
    for (int r = 1; r < 4; r++)
        for (int c = 0; c < 4; c++)
            s[((c + r) % 4) * 4 + r] = t[c * 4 + r];
}

static void mix_columns(u8 s[16]) {
    for (int c = 0; c < 4; c++) {
        u8 *p = s + c * 4;
        u8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (u8)(mul(a0, 2) ^ mul(a1, 3) ^ a2 ^ a3);
        p[1] = (u8)(a0 ^ mul(a1, 2) ^ mul(a2, 3) ^ a3);
        p[2] = (u8)(a0 ^ a1 ^ mul(a2, 2) ^ mul(a3, 3));
        p[3] = (u8)(mul(a0, 3) ^ a1 ^ a2 ^ mul(a3, 2));
    }
}

static void inv_mix_columns(u8 s[16]) {
    for (int c = 0; c < 4; c++) {
        u8 *p = s + c * 4;
        u8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (u8)(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
        p[1] = (u8)(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
        p[2] = (u8)(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
        p[3] = (u8)(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
    }
}

void aes_encrypt_block(const aes_t *a, const u8 in[16], u8 out[16]) {
    u8 s[16];
    memcpy(s, in, 16);

    add_round_key(s, a->round_key);
    for (int r = 1; r < a->rounds; r++) {
        for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, a->round_key + r * 16);
    }
    for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];
    shift_rows(s);
    add_round_key(s, a->round_key + a->rounds * 16);

    memcpy(out, s, 16);
}

void aes_decrypt_block(const aes_t *a, const u8 in[16], u8 out[16]) {
    u8 s[16];
    memcpy(s, in, 16);

    add_round_key(s, a->round_key + a->rounds * 16);
    for (int r = a->rounds - 1; r >= 1; r--) {
        inv_shift_rows(s);
        for (int i = 0; i < 16; i++) s[i] = INV_SBOX[s[i]];
        add_round_key(s, a->round_key + r * 16);
        inv_mix_columns(s);
    }
    inv_shift_rows(s);
    for (int i = 0; i < 16; i++) s[i] = INV_SBOX[s[i]];
    add_round_key(s, a->round_key);

    memcpy(out, s, 16);
}

/* --- unwrapping a key ----------------------------------------------------- */

bool aes_unwrap_key(const u8 *kek, u32 kek_bits,
                    const u8 *in, u32 in_len, u8 *out) {
    if (in_len < 16 || (in_len % 8) != 0) return false;

    aes_t a;
    if (!aes_set_key(&a, kek, kek_bits)) return false;

    u32 n = in_len / 8 - 1;             /* eight byte blocks of real key */
    u8 acc[8];
    memcpy(acc, in, 8);                 /* the check value, so far */
    memcpy(out, in + 8, n * 8);

    /* Six passes backwards over the blocks, which is the wrapping run in
       reverse. The round number is mixed in, so the blocks cannot be
       reordered without the check failing. */
    for (int round = 5; round >= 0; round--) {
        for (int i = (int)n; i >= 1; i--) {
            u8 block[16];
            u32 t = (u32)(n * (u32)round + (u32)i);

            memcpy(block, acc, 8);
            block[7] ^= (u8)t;
            block[6] ^= (u8)(t >> 8);
            block[5] ^= (u8)(t >> 16);
            block[4] ^= (u8)(t >> 24);
            memcpy(block + 8, out + (i - 1) * 8, 8);

            aes_decrypt_block(&a, block, block);
            memcpy(acc, block, 8);
            memcpy(out + (i - 1) * 8, block + 8, 8);
        }
    }

    /* What should be left is the constant the standard names, and nothing
       else. A wrong password produces eight bytes of noise here, which is
       the only signal there is that it was wrong. */
    for (int i = 0; i < 8; i++)
        if (acc[i] != 0xA6) return false;
    return true;
}
