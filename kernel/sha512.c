/* SHA-512 and SHA-384. The same construction as SHA-256 with sixty four bit
   words, eighty rounds, and its own constants. */
#include "sha512.h"
#include "string.h"

static const u64 K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static u64 ror(u64 x, u32 n) { return (x >> n) | (x << (64 - n)); }

static void compress(u64 h[8], const u8 block[128]) {
    u64 w[80];
    for (u32 i = 0; i < 16; i++) {
        w[i] = 0;
        for (u32 j = 0; j < 8; j++) w[i] = (w[i] << 8) | block[i * 8 + j];
    }
    for (u32 i = 16; i < 80; i++) {
        u64 s0 = ror(w[i - 15], 1) ^ ror(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = ror(w[i - 2], 19) ^ ror(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u64 a = h[0], b = h[1], c = h[2], d = h[3];
    u64 e = h[4], f = h[5], g = h[6], hh = h[7];

    for (u32 i = 0; i < 80; i++) {
        u64 S1 = ror(e, 14) ^ ror(e, 18) ^ ror(e, 41);
        u64 ch = (e & f) ^ (~e & g);
        u64 t1 = hh + S1 + ch + K[i] + w[i];
        u64 S0 = ror(a, 28) ^ ror(a, 34) ^ ror(a, 39);
        u64 maj = (a & b) ^ (a & c) ^ (b & c);
        u64 t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha512_init(sha512_t *s) {
    s->h[0] = 0x6a09e667f3bcc908ULL; s->h[1] = 0xbb67ae8584caa73bULL;
    s->h[2] = 0x3c6ef372fe94f82bULL; s->h[3] = 0xa54ff53a5f1d36f1ULL;
    s->h[4] = 0x510e527fade682d1ULL; s->h[5] = 0x9b05688c2b3e6c1fULL;
    s->h[6] = 0x1f83d9abfb41bd6bULL; s->h[7] = 0x5be0cd19137e2179ULL;
    s->bits = 0; s->have = 0; s->out_len = SHA512_SIZE;
}

void sha384_init(sha512_t *s) {
    s->h[0] = 0xcbbb9d5dc1059ed8ULL; s->h[1] = 0x629a292a367cd507ULL;
    s->h[2] = 0x9159015a3070dd17ULL; s->h[3] = 0x152fecd8f70e5939ULL;
    s->h[4] = 0x67332667ffc00b31ULL; s->h[5] = 0x8eb44a8768581511ULL;
    s->h[6] = 0xdb0c2e0d64f98fa7ULL; s->h[7] = 0x47b5481dbefa4fa4ULL;
    s->bits = 0; s->have = 0; s->out_len = SHA384_SIZE;
}

void sha512_update(sha512_t *s, const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    s->bits += (u64)len * 8;
    while (len) {
        u32 room = SHA512_BLOCK - s->have;
        u32 take = len < room ? len : room;
        memcpy(s->buf + s->have, p, take);
        s->have += take;
        p += take;
        len -= take;
        if (s->have == SHA512_BLOCK) { compress(s->h, s->buf); s->have = 0; }
    }
}

void sha512_final(sha512_t *s, u8 *out) {
    u64 bits = s->bits;
    u8 pad = 0x80;
    sha512_update(s, &pad, 1);

    /* The length occupies the last sixteen bytes of a block. Only the low
       eight are ever non zero here, because nothing this hashes is an
       exabyte, but the space has to be left or every digest is wrong. */
    u8 zero = 0;
    while (s->have != SHA512_BLOCK - 16) sha512_update(s, &zero, 1);
    for (u32 i = 0; i < 8; i++) s->buf[s->have++] = 0;
    for (int i = 7; i >= 0; i--) s->buf[s->have++] = (u8)(bits >> (i * 8));
    compress(s->h, s->buf);

    for (u32 i = 0; i < s->out_len; i++)
        out[i] = (u8)(s->h[i / 8] >> (56 - (i % 8) * 8));
}

void sha384(const void *data, u32 len, u8 out[SHA384_SIZE]) {
    sha512_t s;
    sha384_init(&s);
    sha512_update(&s, data, len);
    sha512_final(&s, out);
}

void sha512(const void *data, u32 len, u8 out[SHA512_SIZE]) {
    sha512_t s;
    sha512_init(&s);
    sha512_update(&s, data, len);
    sha512_final(&s, out);
}
