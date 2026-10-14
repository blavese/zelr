#pragma once
/* crypto.subtle: digest, and the rest refused by name.
 *
 * Included from jsdom.h, after jsws.h (whose SHA-1 is the one used here).
 * Odysee's page stopped on "SubtleCrypto not available!": libraries look for
 * crypto.subtle before they will start, and hash with it -- a login's PKCE
 * challenge, a file's checksum, a cache key. So a page that came over an
 * encrypted connection (where the standard puts it) has crypto.subtle with
 * digest for SHA-1, SHA-256, SHA-384 and SHA-512, written here from FIPS
 * 180-4, answering with an ArrayBuffer as a promise does. Every other
 * method -- keys, signing, encryption -- is there and rejects with
 * NotSupportedError, which is what a browser says of an algorithm it lacks:
 * a page told so plainly can fall back, where one handed something that
 * pretends would go on with keys that are not keys.
 *
 * Not done: workers' crypto.subtle; anything but digest. */

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

static void jc_sha256(const u8 *data, u32 n, u8 out[32]) {
    u32 h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    u32 i = 0;
    for (; i + 64 <= n; i += 64) jc_sha256_block(h, data + i);
    u8 last[128];
    u32 r = n - i, pad = r < 56 ? 64 : 128;
    for (u32 k = 0; k < pad; k++) last[k] = k < r ? data[i + k] : 0;
    last[r] = 0x80;
    unsigned long long bits = (unsigned long long)n * 8;
    for (int k = 0; k < 8; k++) last[pad - 1 - k] = (u8)(bits >> (8 * k));
    for (u32 k = 0; k < pad; k += 64) jc_sha256_block(h, last + k);
    for (int k = 0; k < 8; k++) {
        out[4 * k] = (u8)(h[k] >> 24); out[4 * k + 1] = (u8)(h[k] >> 16);
        out[4 * k + 2] = (u8)(h[k] >> 8); out[4 * k + 3] = (u8)h[k];
    }
}

/* --- SHA-512 and SHA-384 -------------------------------------------------------- */

typedef unsigned long long jc_u64;

static const jc_u64 JC_K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static jc_u64 jc_r64(jc_u64 x, int n) { return (x >> n) | (x << (64 - n)); }

static void jc_sha512_block(jc_u64 h[8], const u8 *p) {
    jc_u64 w[80];
    for (int i = 0; i < 16; i++) {
        jc_u64 v = 0;
        for (int k = 0; k < 8; k++) v = (v << 8) | p[8 * i + k];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        jc_u64 s0 = jc_r64(w[i - 15], 1) ^ jc_r64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        jc_u64 s1 = jc_r64(w[i - 2], 19) ^ jc_r64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    jc_u64 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 80; i++) {
        jc_u64 t1 = k + (jc_r64(e, 14) ^ jc_r64(e, 18) ^ jc_r64(e, 41)) + ((e & f) ^ (~e & g)) + JC_K512[i] + w[i];
        jc_u64 t2 = (jc_r64(a, 28) ^ jc_r64(a, 34) ^ jc_r64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

/* SHA-512, or with wide 0 SHA-384 (another start, and six words out). */
static void jc_sha512(const u8 *data, u32 n, u8 *out, int wide) {
    static const jc_u64 H512[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
    };
    static const jc_u64 H384[8] = {
        0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL
    };
    jc_u64 h[8];
    for (int k = 0; k < 8; k++) h[k] = wide ? H512[k] : H384[k];
    u32 i = 0;
    for (; i + 128 <= n; i += 128) jc_sha512_block(h, data + i);
    u8 last[256];
    u32 r = n - i, pad = r < 112 ? 128 : 256;
    for (u32 k = 0; k < pad; k++) last[k] = k < r ? data[i + k] : 0;
    last[r] = 0x80;
    jc_u64 bits = (jc_u64)n * 8;
    for (int k = 0; k < 8; k++) last[pad - 1 - k] = (u8)(bits >> (8 * k));
    for (u32 k = 0; k < pad; k += 128) jc_sha512_block(h, last + k);
    int words = wide ? 8 : 6;
    for (int k = 0; k < words; k++)
        for (int b = 0; b < 8; b++) out[8 * k + b] = (u8)(h[k] >> (56 - 8 * b));
}

/* --- the object ---------------------------------------------------------------------- */

/* The bytes a BufferSource names: an ArrayBuffer, a typed array or a
   DataView (the part of its buffer it covers). 0 for anything else. */
static const u8 *jc_source_bytes(jval v, u32 *n) {
    if (!js_is_obj(v)) return 0;
    jobj *o = v.obj;
    if (o->kind == JO_BUFFER) { *n = ta_buflen(o); return ta_bytes(o); }
    if ((o->kind == JO_TYPED || o->kind == JO_VIEW) && o->internal) {
        jtyped *t = (jtyped *)o->internal;
        u32 size = o->kind == JO_TYPED ? TA_SIZE[t->type] : 1;
        *n = t->len * size;
        return ta_bytes(t->buf) + t->off;
    }
    return 0;
}

static jval jc_rejected(jctx *J, jobj *p, const char *name, const char *why) {
    jobj *e = js_domexc_new(J, 0, js_str(J, why), js_str(J, name));
    if (e) js_promise_settle(J, p, 0, js_from_obj(e));
    return js_from_obj(p);
}

static jval nat_subtle_digest(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *p = js_promise_new(J);
    if (!p) return js_undef();
    jval alg = js_arg(a, n, 0);
    jstr *name = alg.t == JS_STR ? alg.str : 0;
    if (!name && js_is_obj(alg)) {
        jval v = js_get(J, alg, js_str(J, "name"));
        if (J->sig != JS_OK) return js_undef();
        if (v.t == JS_STR) name = v.str;
    }
    u32 len = 0;
    const u8 *bytes = jc_source_bytes(js_arg(a, n, 1), &len);
    if (!name) return jc_rejected(J, p, "TypeError", "digest needs an algorithm");
    if (!bytes) return jc_rejected(J, p, "TypeError", "digest needs an ArrayBuffer or a view of one");
    u8 out[64];
    u32 olen = 0;
    if (w_same_fold(name->s, "SHA-1")) {
        ws_sha1 s;
        ws_sha1_init(&s);
        ws_sha1_add(&s, bytes, len);
        ws_sha1_done(&s, out);
        olen = 20;
    } else if (w_same_fold(name->s, "SHA-256")) { jc_sha256(bytes, len, out); olen = 32; }
    else if (w_same_fold(name->s, "SHA-384")) { jc_sha512(bytes, len, out, 0); olen = 48; }
    else if (w_same_fold(name->s, "SHA-512")) { jc_sha512(bytes, len, out, 1); olen = 64; }
    else return jc_rejected(J, p, "NotSupportedError", "this browser digests with SHA-1, SHA-256, SHA-384 and SHA-512");
    jobj *buf = ta_new_buffer(J, 0, olen);
    if (!buf) return js_undef();
    u8 *d = ta_bytes(buf);
    for (u32 k = 0; k < olen; k++) d[k] = out[k];
    js_promise_resolve_with(J, p, js_from_obj(buf));
    return js_from_obj(p);
}

static jval nat_subtle_refused(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jobj *p = js_promise_new(J);
    if (!p) return js_undef();
    return jc_rejected(J, p, "NotSupportedError", "this browser's SubtleCrypto only digests");
}

static void jd_setup_subtle(jctx *J, jobj *crypto_proto) {
    jobj *sp = jd_interface(J, "SubtleCrypto", 0, 0, 0);
    jd_method(J, sp, "digest", nat_subtle_digest, 2);
    static const char *const REFUSED[] = { "encrypt", "decrypt", "sign", "verify", "generateKey", "deriveKey",
                                           "deriveBits", "importKey", "exportKey", "wrapKey", "unwrapKey", 0 };
    for (int i = 0; REFUSED[i]; i++) jd_method(J, sp, REFUSED[i], nat_subtle_refused, 2);
    jobj *s = js_object_with(J, JO_PLAIN, sp);
    if (s) js_set(J, crypto_proto, "subtle", js_from_obj(s));
}
