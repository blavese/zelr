#pragma once
#include "zelr.h"

/* DEFLATE, which is the compression every picture on the web is inside.
 *
 * Written from RFC 1951, like everything else here. It is the one piece of
 * this that cannot be skipped or approximated: a PNG is a zlib stream and a
 * zlib stream is this, so a browser that cannot do this cannot show a
 * picture at all, whatever else it can do.
 *
 * The format is three kinds of block, one after another, until one of them
 * says it is the last:
 *
 *   stored   the bytes, as they were, with a length in front
 *   fixed    Huffman codes everybody already agrees on
 *   dynamic  Huffman codes described at the front of the block
 *
 * and inside the last two, a stream of symbols where 0..255 is a byte and
 * 257..285 says "copy some of what you have already written", which is the
 * whole of how it saves anything.
 *
 * Bits come out least significant first, which is worth saying out loud
 * because the Huffman codes inside are packed most significant first and
 * getting those two the same way round is the mistake this format is famous
 * for.
 */

#define INF_OK        0
#define INF_TRUNCATED -1     /* the input ended in the middle of something */
#define INF_FULL      -2     /* more output than there was room for */
#define INF_BAD       -3     /* it does not decode: not deflate, or damaged */

/* Enough for the biggest alphabet: 288 literal/length codes. */
#define INF_SYMS 288

typedef struct {
    short count[16];         /* how many codes of each length */
    short symbol[INF_SYMS];  /* the symbols, in code order */
} inf_huff;

typedef struct {
    const u8 *in;
    int n, at;               /* the input, and how far into it */
    u32 bits;                /* what has been read and not used */
    int nbits;

    u8 *out;
    int cap, len;            /* the output, and how much is in it */

    int err;
} inf_state;

/* --- bits ----------------------------------------------------------------
 *
 * Least significant first, across byte boundaries, which is why this keeps
 * its own little buffer rather than indexing the input by bit. */
static inline int inf_bits(inf_state *s, int need) {
    while (s->nbits < need) {
        if (s->at >= s->n) { s->err = INF_TRUNCATED; return 0; }
        s->bits |= (u32)s->in[s->at++] << s->nbits;
        s->nbits += 8;
    }
    int v = (int)(s->bits & ((1u << need) - 1));
    s->bits >>= need;
    s->nbits -= need;
    return v;
}

/* --- Huffman -------------------------------------------------------------
 *
 * The codes are canonical: given only the length of each symbol's code, the
 * codes themselves follow. So a table is a count of how many codes there are
 * of each length and the symbols sorted by length, and decoding walks the
 * lengths one bit at a time asking whether the code so far is one of the
 * ones that length. */
static inline int inf_build(inf_huff *h, const short *len, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[len[i]]++;

    /* All of one length and none of any other is a table with nothing in it,
       which happens and is legal: a block with no distances, for instance. */
    if (h->count[0] == n) return INF_OK;

    /* Over-subscribed is damage; under-subscribed is legal only for the one
       code case, and both are worth telling apart from working. */
    int left = 1;
    for (int i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return INF_BAD;
    }

    short offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (len[i]) h->symbol[offs[len[i]]++] = (short)i;
    return INF_OK;
}

static inline int inf_decode(inf_state *s, const inf_huff *h) {
    int code = 0, first = 0, index = 0;
    for (int length = 1; length < 16; length++) {
        code |= inf_bits(s, 1);
        if (s->err) return -1;
        int count = h->count[length];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = INF_BAD;
    return -1;
}

/* --- the tables the format fixes ----------------------------------------- */

static const short INF_LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const short INF_LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const short INF_DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const short INF_DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* --- a block of symbols ---------------------------------------------------
 *
 * The same loop whichever way the codes were arrived at. A symbol under 256
 * is a byte; 256 ends the block; anything above is a length, and a distance
 * follows it, and together they say to copy what was written earlier.
 *
 * The copy has to go one byte at a time. A distance shorter than the length
 * is not a mistake, it is how a run is written — three bytes at distance one
 * means repeat the last byte three times — and copying in blocks would read
 * what has not been written yet. */
static inline void inf_block(inf_state *s, const inf_huff *lit,
                             const inf_huff *dist) {
    for (;;) {
        int sym = inf_decode(s, lit);
        if (s->err) return;

        if (sym < 256) {
            if (s->len >= s->cap) { s->err = INF_FULL; return; }
            s->out[s->len++] = (u8)sym;
            continue;
        }
        if (sym == 256) return;                 /* the end of this block */

        sym -= 257;
        if (sym >= 29) { s->err = INF_BAD; return; }
        int length = INF_LEN_BASE[sym] + inf_bits(s, INF_LEN_EXTRA[sym]);

        int dsym = inf_decode(s, dist);
        if (s->err || dsym < 0 || dsym >= 30) { s->err = INF_BAD; return; }
        int back = INF_DIST_BASE[dsym] + inf_bits(s, INF_DIST_EXTRA[dsym]);
        if (s->err) return;

        if (back > s->len) { s->err = INF_BAD; return; }
        if (s->len + length > s->cap) { s->err = INF_FULL; return; }

        int from = s->len - back;
        for (int i = 0; i < length; i++) s->out[s->len++] = s->out[from + i];
    }
}

/* --- the three kinds of block --------------------------------------------- */

static inline void inf_fixed(inf_state *s) {
    /* The lengths everybody agrees on, from the specification. */
    short len[INF_SYMS];
    int i = 0;
    for (; i < 144; i++) len[i] = 8;
    for (; i < 256; i++) len[i] = 9;
    for (; i < 280; i++) len[i] = 7;
    for (; i < 288; i++) len[i] = 8;

    inf_huff lit, dist;
    if (inf_build(&lit, len, 288) != INF_OK) { s->err = INF_BAD; return; }

    short dl[30];
    for (i = 0; i < 30; i++) dl[i] = 5;
    if (inf_build(&dist, dl, 30) != INF_OK) { s->err = INF_BAD; return; }

    inf_block(s, &lit, &dist);
}

static inline void inf_dynamic(inf_state *s) {
    int nlen = inf_bits(s, 5) + 257;
    int ndist = inf_bits(s, 5) + 1;
    int ncode = inf_bits(s, 4) + 4;
    if (s->err) return;
    if (nlen > 286 || ndist > 30) { s->err = INF_BAD; return; }

    /* The lengths of the code lengths, in an order the format chose so that
       the ones most likely to be zero come last and can be left out. */
    static const short ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    short clen[19];
    for (int i = 0; i < 19; i++) clen[i] = 0;
    for (int i = 0; i < ncode; i++) {
        clen[ORDER[i]] = (short)inf_bits(s, 3);
        if (s->err) return;
    }

    inf_huff code;
    if (inf_build(&code, clen, 19) != INF_OK) { s->err = INF_BAD; return; }

    /* And now the lengths themselves, which are written with three symbols
       that mean "repeat" so that a run of the same length is short. */
    short len[INF_SYMS + 30];
    int at = 0;
    while (at < nlen + ndist) {
        int sym = inf_decode(s, &code);
        if (s->err) return;

        if (sym < 16) { len[at++] = (short)sym; continue; }

        int repeat, value = 0;
        if (sym == 16) {
            if (at == 0) { s->err = INF_BAD; return; }
            value = len[at - 1];
            repeat = 3 + inf_bits(s, 2);
        } else if (sym == 17) {
            repeat = 3 + inf_bits(s, 3);
        } else {
            repeat = 11 + inf_bits(s, 7);
        }
        if (s->err) return;
        if (at + repeat > nlen + ndist) { s->err = INF_BAD; return; }
        while (repeat--) len[at++] = (short)value;
    }

    inf_huff lit, dist;
    if (inf_build(&lit, len, nlen) != INF_OK) { s->err = INF_BAD; return; }
    if (inf_build(&dist, len + nlen, ndist) != INF_OK) { s->err = INF_BAD; return; }

    inf_block(s, &lit, &dist);
}

static inline void inf_stored(inf_state *s) {
    /* Whole bytes from here, so whatever is left of the current one goes. */
    s->bits = 0;
    s->nbits = 0;
    if (s->at + 4 > s->n) { s->err = INF_TRUNCATED; return; }

    int length = s->in[s->at] | (s->in[s->at + 1] << 8);
    int check = s->in[s->at + 2] | (s->in[s->at + 3] << 8);
    s->at += 4;
    if ((length ^ 0xFFFF) != check) { s->err = INF_BAD; return; }

    if (s->at + length > s->n) { s->err = INF_TRUNCATED; return; }
    if (s->len + length > s->cap) { s->err = INF_FULL; return; }
    for (int i = 0; i < length; i++) s->out[s->len++] = s->in[s->at++];
}

/* --- the way in -----------------------------------------------------------
 *
 * The loop itself. Returns INF_OK or one of the other INF_ numbers, and
 * says separately how much came out -- which matters when what went wrong
 * was INF_FULL, because then the output is not rubbish, it is the front of
 * the answer and the rest did not fit. A page read down to where the room
 * ran out is worth more than no page.
 *
 * Not so for a picture: half a PNG is not half a picture, because the rows
 * after the cut are the ones the decoder was told to copy from. So the
 * callers that decode an image use inflate_raw below, which treats running
 * out of room as the failure it is for them. */
static inline int inf_run(const u8 *in, int n, u8 *out, int cap, int *out_len) {
    inf_state s;
    s.in = in; s.n = n; s.at = 0;
    s.bits = 0; s.nbits = 0;
    s.out = out; s.cap = cap; s.len = 0;
    s.err = INF_OK;

    for (;;) {
        int last = inf_bits(&s, 1);
        int kind = inf_bits(&s, 2);
        if (s.err) break;

        if (kind == 0) inf_stored(&s);
        else if (kind == 1) inf_fixed(&s);
        else if (kind == 2) inf_dynamic(&s);
        else { s.err = INF_BAD; break; }

        if (s.err) break;
        if (last) break;
    }
    if (out_len) *out_len = s.len;
    return s.err;
}

/* Returns how many bytes came out, or one of the INF_ numbers. */
static inline int inflate_raw(const u8 *in, int n, u8 *out, int cap) {
    int len = 0;
    int err = inf_run(in, n, out, cap, &len);
    return err ? err : len;
}

/* A zlib stream, which is deflate with two bytes in front saying how it was
   compressed and four behind checking it. The trailing checksum is not
   verified: what it would catch — a corrupted download — the transport has
   already checked, and a picture that decodes is a picture. */
static inline int inflate_zlib(const u8 *in, int n, u8 *out, int cap) {
    if (n < 2) return INF_TRUNCATED;

    int cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8) return INF_BAD;          /* not deflate */
    if (((cmf << 8) | flg) % 31) return INF_BAD;    /* the header's own check */
    if (flg & 0x20) return INF_BAD;                 /* a preset dictionary */

    return inflate_raw(in + 2, n - 2, out, cap);
}
