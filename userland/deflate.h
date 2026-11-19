/* Deflate, the other way: bytes made smaller, as RFC 1951 says.
 *
 * inflate.h reads what everybody else writes; this writes what everybody
 * else reads, for a .zip made here. Matches are found the usual way: the
 * last place each three bytes were seen is kept in a table by a hash of
 * them, and each place points back to the one before with the same hash, so
 * a chain of earlier places can be walked for the longest match (64 of them
 * at most, 258 bytes, 32 KiB back). Codes are the standard's fixed ones:
 * no table to send, and within a few per cent of a table made for the data
 * on the text and programs people put in archives. Data that does not get
 * smaller this way (pictures and archives already compressed) the caller
 * keeps as it is: deflate_raw says so by answering -1.
 *
 * The tables are static, so this is one compression at a time.
 */
#pragma once
#include "zelr.h"

#define DEF_WINDOW   32768
#define DEF_HASH     32768
#define DEF_CHAIN    64
#define DEF_MIN      3
#define DEF_MAX      258

static int def_head[DEF_HASH];          /* the last place a hash was seen, plus one */
static int def_prev[DEF_WINDOW];        /* the place before with the same hash, plus one */

typedef struct {
    u8 *out;
    u32 cap, n;
    u32 bits;
    int nbits;
    int full;
} def_out;

static inline void def_put(def_out *o, u32 v, int k) {
    o->bits |= v << o->nbits;
    o->nbits += k;
    while (o->nbits >= 8) {
        if (o->n < o->cap) o->out[o->n++] = (u8)o->bits;
        else o->full = 1;
        o->bits >>= 8;
        o->nbits -= 8;
    }
}

/* A Huffman code goes out first bit first, the reverse of everything else. */
static inline void def_code(def_out *o, u32 code, int k) {
    u32 r = 0;
    for (int i = 0; i < k; i++) r |= ((code >> i) & 1) << (k - 1 - i);
    def_put(o, r, k);
}

static inline void def_literal(def_out *o, int c) {
    if (c < 144) def_code(o, 0x30 + (u32)c, 8);
    else if (c < 256) def_code(o, 0x190 + (u32)(c - 144), 9);
    else if (c < 280) def_code(o, (u32)(c - 256), 7);
    else def_code(o, 0xC0 + (u32)(c - 280), 8);
}

static const u16 DEF_LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                   67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 DEF_LEXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                   5, 5, 5, 5, 0 };
static const u16 DEF_DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                   769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 DEF_DEXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                   11, 11, 12, 12, 13, 13 };

static inline void def_match(def_out *o, int len, int dist) {
    int l = 28;
    while (DEF_LBASE[l] > len) l--;
    def_literal(o, 257 + l);
    if (DEF_LEXTRA[l]) def_put(o, (u32)(len - DEF_LBASE[l]), DEF_LEXTRA[l]);
    int d = 29;
    while (DEF_DBASE[d] > dist) d--;
    def_code(o, (u32)d, 5);
    if (DEF_DEXTRA[d]) def_put(o, (u32)(dist - DEF_DBASE[d]), DEF_DEXTRA[d]);
}

static inline u32 def_hash(const u8 *p) {
    return ((u32)p[0] * 506832829u ^ (u32)p[1] * 2654435761u ^ (u32)p[2] * 40503u) & (DEF_HASH - 1);
}

/* in[0..n) as raw deflate (no zlib wrapper) into out; how many bytes, or -1
   when it would not be smaller than n or does not fit in cap. One fixed
   block, which the standard allows to be as long as the data. */
static inline int deflate_raw(const u8 *in, u32 n, u8 *out, u32 cap) {
    def_out o = { out, cap < n ? cap : n, 0, 0, 0, 0 };
    for (int i = 0; i < DEF_HASH; i++) def_head[i] = 0;
    def_put(&o, 1, 1);                  /* the last block */
    def_put(&o, 1, 2);                  /* fixed codes */
    u32 i = 0;
    while (i < n && !o.full) {
        int best = 0, best_dist = 0;
        if (i + DEF_MIN <= n) {
            u32 h = def_hash(in + i);
            int cand = def_head[h];
            u32 most = n - i < DEF_MAX ? n - i : DEF_MAX;
            for (int chain = 0; cand && chain < DEF_CHAIN; chain++) {
                u32 at = (u32)(cand - 1);
                if (i - at > DEF_WINDOW - 1) break;
                if (in[at + (u32)best] == in[i + (u32)best]) {
                    u32 k = 0;
                    while (k < most && in[at + k] == in[i + k]) k++;
                    if ((int)k > best) { best = (int)k; best_dist = (int)(i - at); if (k == most) break; }
                }
                cand = def_prev[at & (DEF_WINDOW - 1)];
            }
        }
        u32 step = best >= DEF_MIN ? (u32)best : 1;
        if (best >= DEF_MIN) def_match(&o, best, best_dist);
        else def_literal(&o, in[i]);
        /* Every place passed is put in the tables, so later matches can
           start inside this one. */
        for (u32 k = 0; k < step; k++, i++) {
            if (i + DEF_MIN > n) continue;
            u32 h = def_hash(in + i);
            def_prev[i & (DEF_WINDOW - 1)] = def_head[h];
            def_head[h] = (int)i + 1;
        }
    }
    def_literal(&o, 256);               /* the end of the block */
    if (o.nbits) def_put(&o, 0, 8 - o.nbits);
    if (o.full || o.n >= n) return -1;
    return (int)o.n;
}
