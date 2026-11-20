#pragma once
#include "zelr.h"

/* SHA-256, from FIPS 180-4. The browser's crypto.subtle digests with it
   (jscrypto.h) and Settings hashes the screen lock's password with it; the
   kernel has its own (kernel/sha256.c), which checks the password, and the
   two must agree, which the self test's [windows] and lockcheck.py see.
   The terminal's sha256sum feeds it a file a piece at a time
   (jc_sha256_start, _add, _end), which is what jc_sha256 is made of. */

/* --- SHA-256 ------------------------------------------------------------------ */

static const u32 JC_K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static u32 jc_r32(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static void jc_sha256_block(u32 h[8], const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) | ((u32)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = jc_r32(w[i - 15], 7) ^ jc_r32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = jc_r32(w[i - 2], 17) ^ jc_r32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = k + (jc_r32(e, 6) ^ jc_r32(e, 11) ^ jc_r32(e, 25)) + ((e & f) ^ (~e & g)) + JC_K256[i] + w[i];
        u32 t2 = (jc_r32(a, 2) ^ jc_r32(a, 13) ^ jc_r32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

/* A digest being made: the state, the part of a block not yet full, and
   how many bytes have gone in (the padding ends with it, in bits). */
typedef struct {
    u32 h[8];
    u8  part[64];
    u32 npart;
    unsigned long long total;
} jc_sha256_t;

static void jc_sha256_start(jc_sha256_t *s) {
    static const u32 H0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    for (int k = 0; k < 8; k++) s->h[k] = H0[k];
    s->npart = 0;
    s->total = 0;
}

static void jc_sha256_add(jc_sha256_t *s, const u8 *data, u32 n) {
    s->total += n;
    u32 i = 0;
    if (s->npart) {
        while (i < n && s->npart < 64) s->part[s->npart++] = data[i++];
        if (s->npart < 64) return;
        jc_sha256_block(s->h, s->part);
        s->npart = 0;
    }
    for (; i + 64 <= n; i += 64) jc_sha256_block(s->h, data + i);
    while (i < n) s->part[s->npart++] = data[i++];
}

static void jc_sha256_end(jc_sha256_t *s, u8 out[32]) {
    u8 last[128];
    u32 r = s->npart, pad = r < 56 ? 64 : 128;
    for (u32 k = 0; k < pad; k++) last[k] = k < r ? s->part[k] : 0;
    last[r] = 0x80;
    unsigned long long bits = s->total * 8;
    for (int k = 0; k < 8; k++) last[pad - 1 - k] = (u8)(bits >> (8 * k));
    for (u32 k = 0; k < pad; k += 64) jc_sha256_block(s->h, last + k);
    for (int k = 0; k < 8; k++) {
        out[4 * k] = (u8)(s->h[k] >> 24); out[4 * k + 1] = (u8)(s->h[k] >> 16);
        out[4 * k + 2] = (u8)(s->h[k] >> 8); out[4 * k + 3] = (u8)s->h[k];
    }
}

/* Inline, as the terminal uses only the three above. */
static inline void jc_sha256(const u8 *data, u32 n, u8 out[32]) {
    jc_sha256_t s;
    jc_sha256_start(&s);
    jc_sha256_add(&s, data, n);
    jc_sha256_end(&s, out);
}

