#pragma once
#include "zelr.h"
#include "alloc.h"
#include "png.h"
#include "webptab.h"

/* WebP, from RFC 9649 (the container and the lossless format) and RFC 6386
 * (VP8, the lossy format inside it).
 *
 * A growing share of the pictures on the web are WebP: sites and their image
 * servers send it to any browser that says it can take it, and some send
 * nothing else. It is two formats in one wrapper, and they have almost
 * nothing in common but the wrapper.
 *
 * The wrapper is RIFF: "RIFF", a length, "WEBP", then chunks, each a four
 * letter name, a length and its bytes, padded to an even length. The simple
 * kinds are one chunk, "VP8 " or "VP8L". The extended kind starts with
 * "VP8X", which gives the canvas size and says what else to expect: an
 * "ALPH" chunk of transparency in front of a "VP8 " one, or an animation
 * ("ANIM", then an "ANMF" per frame, each holding its own chunks), and
 * colour profiles and metadata, which are skipped.
 *
 * VP8L, the lossless kind, keeps ARGB pixels exactly. Each pixel is green,
 * red, blue and alpha, each read with a canonical prefix (Huffman) code; or a
 * length and distance saying to copy pixels already decoded, as deflate
 * does, with the smallest distances standing for the 120 nearest pixels
 * above and to the left rather than for the last few in the row; or an index
 * into a small cache of recently seen colours. The picture can be cut into
 * blocks that each use their own set of codes (an "entropy image" says which
 * block uses which). Before any of that the encoder may have applied up to
 * four transforms, each recorded with its own small image: a predictor (each
 * pixel minus a guess from its neighbours, the guess chosen per block), a
 * colour transform (red and blue less a multiple of green, and blue less a
 * multiple of red), subtracting green from red and blue, and a colour table,
 * with two, four or eight indices packed into one pixel when the table is
 * small. Decoding reads the transforms, decodes the pixels, then undoes the
 * transforms last first.
 *
 * VP8 is a key frame of the VP8 video format: the lossy kind, and the one
 * photographs arrive in. Like JPEG it is brightness and two colour
 * differences, the colour at half resolution, in blocks whose frequencies
 * have been divided down and mostly thrown away. Unlike JPEG, each 16x16
 * macroblock is first predicted from the pixels above and to its left --
 * a whole-block guess, one of four, or a guess for each 4x4 part, one of ten
 * -- and only the difference is coded. Everything is read with a boolean
 * arithmetic decoder, each bit with a probability that depends on what came
 * before, so the probability tables (RFC 6386 prints them; see webptab.h)
 * are part of the format. A coefficient is a token from a small tree,
 * dequantised by a factor that depends on the block's segment, and the
 * inverse transform is a 4x4 integer DCT, with the sixteen brightness DCs
 * sent through a second, Walsh-Hadamard, transform when the block was
 * predicted whole. Last, the loop filter smooths the edges between blocks,
 * where the quantising shows most, and brightness and colour are turned
 * into red, green and blue by the BT.601 equations, each pixel's colour
 * interpolated from the four half-resolution samples nearest it rather than
 * copied from one (a copy puts a two pixel staircase along every colour
 * edge).
 *
 * ALPH is transparency for a VP8 picture: its bytes as they are, or a VP8L
 * stream with no header whose green channel is the alpha, and either way one
 * of three filters (the value to the left, the one above, or a gradient of
 * both) to undo. Transparent pixels are laid over `bg` like a PNG's.
 *
 * This was written from the RFCs' descriptions; no decoder's source was
 * copied. The constant tables are taken out of the RFC text by
 * tools/genwebptab.py.
 *
 * What is not here: any frame of an animation after the first (the first is
 * drawn where the canvas puts it, over the page); colour profiles, which
 * are not applied; the animation's background colour, which is not used,
 * since browsers show the page through instead; and VP8's scaling bits,
 * which ask for the picture to be shown larger and are ignored, as other
 * decoders ignore them. A VP8 frame that is not a key frame cannot stand
 * alone and is refused by name. */

#define WEBP_OK           0
#define WEBP_NOT_WEBP    -1
#define WEBP_TRUNCATED   -2
#define WEBP_UNSUPPORTED -3   /* a VP8 frame that is not a key frame */
#define WEBP_TOO_BIG     -4
#define WEBP_BAD         -5

/* As for PNG: nothing bigger is a page element. */
#define WEBP_MAX_SIDE 4096
#define WEBP_MAX_PIXELS (16 * 1024 * 1024)

/* What the prefix codes of one lossless image may take. A hostile stream
   can ask for 65536 sets of five codes with a few bits each, and each code
   is up to 2328 symbols; this is where that stops being a picture. */
#define WEBP_CODE_BUDGET (24 * 1024 * 1024)

/* A test build can count what the decoder met, to know which parts of it
   its files reached. Nothing else defines this. */
#ifdef WEBP_TRACE
static struct {
    int transform[4], cache, meta, simple_code, normal_code, backref, cache_hit;
    int vp8, simple_filter, normal_filter, bpred, ymode[5], segments, partitions;
    int alph_raw, alph_lossless, alph_filter[4], anim;
} webp_seen;
#define WEBP_SAW(what) (webp_seen.what++)
#else
#define WEBP_SAW(what) ((void)0)
#endif

static inline u32 webp_le16(const u8 *p) { return p[0] | ((u32)p[1] << 8); }
static inline u32 webp_le24(const u8 *p) { return webp_le16(p) | ((u32)p[2] << 16); }
static inline u32 webp_le32(const u8 *p) { return webp_le24(p) | ((u32)p[3] << 24); }
static inline int webp_abs(int v) { return v < 0 ? -v : v; }
static inline int webp_clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ======================================================================
 * VP8L, the lossless format (RFC 9649 section 3)
 * ====================================================================== */

/* --- bits ------------------------------------------------------------------
 *
 * Least significant first, like deflate. Reading past the end sets `err`
 * and returns zeros, so a loop can run to where it checks rather than every
 * caller checking every read. */
typedef struct {
    const u8 *in;
    int n, at;
    u64 acc;                 /* bits not yet used, the next one lowest */
    int bits;                /* how many of them there are */
    int err;
} wl_bits;

static inline void wl_fill(wl_bits *b) {
    while (b->bits <= 56 && b->at < b->n) {
        b->acc |= (u64)b->in[b->at++] << b->bits;
        b->bits += 8;
    }
}

static inline u32 wl_read(wl_bits *b, int n) {
    if (b->bits < n) {
        wl_fill(b);
        if (b->bits < n) { b->err = WEBP_TRUNCATED; return 0; }
    }
    u32 v = (u32)(b->acc & ((1ull << n) - 1));
    b->acc >>= n;
    b->bits -= n;
    return v;
}

/* --- prefix codes ----------------------------------------------------------
 *
 * Canonical, as in deflate: only each symbol's code length is sent, a count
 * of codes of each length and the symbols in code order are enough to
 * decode, and the codes are packed first bit first. A code of one symbol
 * takes no bits at all, however long the stream says it is. */
typedef struct {
    u16 count[16];
    u16 *sym;                /* the symbols in code order */
    int single;              /* the one symbol of a code of one, or -1 */
    int maxlen;
} wl_code;

/* Sorts the symbols into `sym` (room for n) and checks the lengths describe
   a whole code: a stream whose codes leave some bit pattern meaning nothing,
   or give one pattern two meanings, is damaged, not merely unusual. */
static inline int wl_build(wl_code *c, const u8 *len, int n, u16 *sym) {
    for (int i = 0; i < 16; i++) c->count[i] = 0;
    int used = 0, last = 0;
    for (int i = 0; i < n; i++)
        if (len[i]) { c->count[len[i]]++; used++; last = i; }
    c->sym = sym;
    c->maxlen = 0;
    c->single = -1;
    if (used == 0) return -1;
    if (used == 1) { c->single = last; return 1; }

    int left = 1;
    for (int l = 1; l < 16; l++) {
        left = (left << 1) - c->count[l];
        if (left < 0) return -1;
        if (c->count[l]) c->maxlen = l;
    }
    if (left != 0) return -1;

    u16 at[16];
    at[1] = 0;
    for (int l = 1; l < 15; l++) at[l + 1] = (u16)(at[l] + c->count[l]);
    for (int i = 0; i < n; i++)
        if (len[i]) sym[at[len[i]]++] = (u16)i;
    return used;
}

/* One symbol. The next fifteen bits are looked at together and only the
   code's own length is taken, which is what lets a code near the end of
   the stream finish without asking for bits that are not there. */
static inline int wl_symbol(wl_bits *b, const wl_code *c) {
    if (c->single >= 0) return c->single;
    if (b->bits < 15) wl_fill(b);
    u32 peek = (u32)b->acc;
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= c->maxlen; len++) {
        code |= (int)((peek >> (len - 1)) & 1);
        int count = c->count[len];
        if (code - first < count) {
            if (len > b->bits) { b->err = WEBP_TRUNCATED; return 0; }
            b->acc >>= len;
            b->bits -= len;
            return c->sym[index + code - first];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    b->err = WEBP_BAD;              /* a whole code cannot get here */
    return 0;
}

/* Reads one code's lengths (section 3.7.2.1) and builds it. `len` and
   `sym` are scratch of the alphabet's size; what the code keeps is copied
   out of `sym` into memory of its own, counted against `budget`. */
static inline int wl_read_code(wl_bits *b, int alphabet, wl_code *c, u8 *len,
                               u16 *sym, int *budget) {
    for (int i = 0; i < alphabet; i++) len[i] = 0;
    if (wl_read(b, 1)) {
        /* The simple kind: one or two symbols, each a one bit code. */
        WEBP_SAW(simple_code);
        int two = (int)wl_read(b, 1);
        int wide = (int)wl_read(b, 1);
        int s0 = (int)wl_read(b, wide ? 8 : 1);
        if (s0 >= alphabet) return WEBP_BAD;
        len[s0] = 1;
        if (two) {
            int s1 = (int)wl_read(b, 8);
            if (s1 >= alphabet) return WEBP_BAD;
            len[s1] = 1;
        }
    } else {
        /* The normal kind: the lengths are themselves prefix coded, with
           three codes for runs, and the lengths of that code come first in
           an order that puts the ones usually zero last. */
        WEBP_SAW(normal_code);
        u8 cl[19];
        u16 clsym[19];
        for (int i = 0; i < 19; i++) cl[i] = 0;
        int ncl = (int)wl_read(b, 4) + 4;
        for (int i = 0; i < ncl; i++) cl[VP8L_CODE_LENGTH_ORDER[i]] = (u8)wl_read(b, 3);
        if (b->err) return b->err;
        wl_code lc;
        if (wl_build(&lc, cl, 19, clsym) < 0) return WEBP_BAD;

        /* How many of the length codes are sent, when fewer than the
           alphabet: a count of codes read, runs counting once. */
        int reads = alphabet;
        if (wl_read(b, 1)) {
            int nbits = 2 + 2 * (int)wl_read(b, 3);
            reads = 2 + (int)wl_read(b, nbits);
            if (reads > alphabet) return WEBP_BAD;
        }
        int s = 0, prev = 8;
        while (s < alphabet && reads-- > 0) {
            int v = wl_symbol(b, &lc);
            if (b->err) return b->err;
            if (v < 16) {
                len[s++] = (u8)v;
                if (v) prev = v;
                continue;
            }
            int repeat, fill = 0;
            if (v == 16) { repeat = 3 + (int)wl_read(b, 2); fill = prev; }
            else if (v == 17) repeat = 3 + (int)wl_read(b, 3);
            else repeat = 11 + (int)wl_read(b, 7);
            if (s + repeat > alphabet) return WEBP_BAD;
            while (repeat-- > 0) len[s++] = (u8)fill;
        }
    }
    if (b->err) return b->err;

    int used = wl_build(c, len, alphabet, sym);
    if (used < 0) return WEBP_BAD;
    c->sym = 0;
    if (used > 1) {
        *budget -= used * 2 + 16;
        if (*budget < 0) return WEBP_TOO_BIG;
        c->sym = (u16 *)malloc((u64)used * 2);
        if (!c->sym) return WEBP_TOO_BIG;
        for (int i = 0; i < used; i++) c->sym[i] = sym[i];
    }
    return WEBP_OK;
}

/* A length or a distance: a prefix symbol and that many raw bits more
   (section 3.6.2.2). */
static inline int wl_prefix_value(wl_bits *b, int prefix) {
    if (prefix < 4) return prefix + 1;
    int extra = (prefix - 2) >> 1;
    int offset = (2 + (prefix & 1)) << extra;
    return offset + (int)wl_read(b, extra) + 1;
}

static inline void wl_free_codes(wl_code *codes, int n) {
    if (!codes) return;
    for (int i = 0; i < n; i++) if (codes[i].sym) free(codes[i].sym);
    free(codes);
}

/* One entropy-coded image of w x h pixels into `out` (section 3.7). `whole`
   is set for the picture itself, which alone may use more than one set of
   codes; the transforms' images and the entropy image always use one. */
static int wl_image(wl_bits *b, int w, int h, int whole, u32 *out, int *budget) {
    int cache_bits = 0;
    if (wl_read(b, 1)) {
        cache_bits = (int)wl_read(b, 4);
        if (cache_bits < 1 || cache_bits > 11) return b->err ? b->err : WEBP_BAD;
        WEBP_SAW(cache);
    }

    int rc = WEBP_OK;
    u32 *meta = 0;
    int meta_bits = 0, meta_w = 0, groups = 1;
    if (whole && wl_read(b, 1)) {
        WEBP_SAW(meta);
        meta_bits = (int)wl_read(b, 3) + 2;
        meta_w = (w + (1 << meta_bits) - 1) >> meta_bits;
        int meta_h = (h + (1 << meta_bits) - 1) >> meta_bits;
        meta = (u32 *)malloc((u64)meta_w * meta_h * 4);
        if (!meta) return WEBP_TOO_BIG;
        rc = wl_image(b, meta_w, meta_h, 0, meta, budget);
        if (rc != WEBP_OK) { free(meta); return rc; }
        /* Red and green together are the number of the set of codes; the
           largest says how many sets the stream holds. */
        for (int i = 0; i < meta_w * meta_h; i++) {
            meta[i] = (meta[i] >> 8) & 0xFFFF;
            if ((int)meta[i] + 1 > groups) groups = (int)meta[i] + 1;
        }
    }
    if (b->err) { if (meta) free(meta); return b->err; }

    int cache_size = cache_bits ? 1 << cache_bits : 0;
    int alphabet[5] = { 256 + 24 + cache_size, 256, 256, 256, 40 };
    *budget -= groups * 5 * (int)sizeof(wl_code);
    if (*budget < 0) { if (meta) free(meta); return WEBP_TOO_BIG; }
    wl_code *codes = (wl_code *)calloc((u64)groups * 5 * sizeof(wl_code));
    u8 *len = (u8 *)malloc(256 + 24 + 2048);
    u16 *sym = (u16 *)malloc((256 + 24 + 2048) * 2);
    u32 *cache = cache_size ? (u32 *)calloc((u64)cache_size * 4) : 0;
    if (!codes || !len || !sym || (cache_size && !cache)) { rc = WEBP_TOO_BIG; goto done; }

    for (int g = 0; g < groups && rc == WEBP_OK; g++)
        for (int k = 0; k < 5 && rc == WEBP_OK; k++)
            rc = wl_read_code(b, alphabet[k], &codes[g * 5 + k], len, sym, budget);
    if (rc != WEBP_OK) goto done;

    int total = w * h, pos = 0, x = 0, y = 0;
    const wl_code *set = codes;
    while (pos < total) {
        if (meta) set = codes + 5 * meta[(y >> meta_bits) * meta_w + (x >> meta_bits)];
        int s = wl_symbol(b, &set[0]);
        if (s < 256) {
            u32 r = (u32)wl_symbol(b, &set[1]);
            u32 bl = (u32)wl_symbol(b, &set[2]);
            u32 a = (u32)wl_symbol(b, &set[3]);
            u32 px = (a << 24) | (r << 16) | ((u32)s << 8) | bl;
            out[pos++] = px;
            if (cache) cache[(0x1e35a7bdu * px) >> (32 - cache_bits)] = px;
            if (++x == w) { x = 0; y++; }
        } else if (s < 256 + 24) {
            WEBP_SAW(backref);
            int length = wl_prefix_value(b, s - 256);
            int code = wl_prefix_value(b, wl_symbol(b, &set[4]));
            int dist;
            if (code > 120) dist = code - 120;
            else {
                /* The nearest 120 by where they are, not how far back. */
                dist = VP8L_DISTANCE_MAP[code - 1][0] + VP8L_DISTANCE_MAP[code - 1][1] * w;
                if (dist < 1) dist = 1;
            }
            if (b->err) break;
            if (dist > pos || length > total - pos) { rc = WEBP_BAD; goto done; }
            /* One at a time: a copy may overlap what it is writing. */
            for (int i = 0; i < length; i++) {
                u32 px = out[pos - dist];
                out[pos++] = px;
                if (cache) cache[(0x1e35a7bdu * px) >> (32 - cache_bits)] = px;
            }
            x += length;
            while (x >= w) { x -= w; y++; }
        } else {
            WEBP_SAW(cache_hit);
            int i = s - 256 - 24;
            if (i >= cache_size) { rc = WEBP_BAD; goto done; }
            out[pos++] = cache[i];
            if (++x == w) { x = 0; y++; }
        }
        if (b->err) break;
    }
    if (b->err) rc = b->err;

done:
    if (meta) free(meta);
    wl_free_codes(codes, groups * 5);
    if (len) free(len);
    if (sym) free(sym);
    if (cache) free(cache);
    return rc;
}

/* --- the transforms (section 3.5) ---------------------------------------- */

/* Each channel added separately, carries not crossing into the next. */
static inline u32 wl_add(u32 a, u32 b) {
    return (((a & 0x00FF00FFu) + (b & 0x00FF00FFu)) & 0x00FF00FFu)
         | (((a & 0xFF00FF00u) + (b & 0xFF00FF00u)) & 0xFF00FF00u);
}

/* (a + b) / 2 in each channel, without the sums leaving eight bits. */
static inline u32 wl_avg(u32 a, u32 b) {
    return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b);
}

static inline u32 wl_select(u32 l, u32 t, u32 tl) {
    int pl = 0, pt = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int cl = (int)((l >> sh) & 0xFF), ct = (int)((t >> sh) & 0xFF), ctl = (int)((tl >> sh) & 0xFF);
        int p = cl + ct - ctl;
        pl += webp_abs(p - cl);
        pt += webp_abs(p - ct);
    }
    return pl < pt ? l : t;
}

static inline u32 wl_clamp_full(u32 a, u32 b, u32 c) {
    u32 out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int v = (int)((a >> sh) & 0xFF) + (int)((b >> sh) & 0xFF) - (int)((c >> sh) & 0xFF);
        out |= (u32)webp_clamp255(v) << sh;
    }
    return out;
}

/* a + (a - b) / 2, the division in C's way, toward zero. */
static inline u32 wl_clamp_half(u32 a, u32 b) {
    u32 out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
        out |= (u32)webp_clamp255(ca + (ca - cb) / 2) << sh;
    }
    return out;
}

/* The fourteen guesses. A mode past 13 is not in the specification; it is
   given the first, which is also what the row that starts a picture uses. */
static inline u32 wl_predict(int mode, u32 l, u32 t, u32 tl, u32 tr) {
    switch (mode) {
    case 1:  return l;
    case 2:  return t;
    case 3:  return tr;
    case 4:  return tl;
    case 5:  return wl_avg(wl_avg(l, tr), t);
    case 6:  return wl_avg(l, tl);
    case 7:  return wl_avg(l, t);
    case 8:  return wl_avg(tl, t);
    case 9:  return wl_avg(t, tr);
    case 10: return wl_avg(wl_avg(l, tl), wl_avg(t, tr));
    case 11: return wl_select(l, t, tl);
    case 12: return wl_clamp_full(l, t, tl);
    case 13: return wl_clamp_half(wl_avg(l, t), tl);
    default: return 0xFF000000u;
    }
}

/* In place and in scan order: every neighbour a guess uses is already
   final by the time it is used. The pixel above and to the right of the
   last one in a row is, by the specification, the first of the row itself,
   which is where it falls in memory anyway. */
static inline void wl_unpredict(u32 *px, int w, int h, const u32 *modes, int bits) {
    int mw = (w + (1 << bits) - 1) >> bits;
    for (int y = 0; y < h; y++) {
        u32 *row = px + y * w;
        for (int x = 0; x < w; x++) {
            u32 guess;
            if (y == 0) guess = x ? row[x - 1] : 0xFF000000u;
            else if (x == 0) guess = row[x - w];
            else {
                int mode = (int)((modes[(y >> bits) * mw + (x >> bits)] >> 8) & 0xF);
                guess = wl_predict(mode, row[x - 1], row[x - w], row[x - w - 1], row[x - w + 1]);
            }
            row[x] = wl_add(row[x], guess);
        }
    }
}

/* The multiplier is a signed 3.5 fixed point number and the channel a
   signed byte. */
static inline int wl_delta(u32 t, u32 c) {
    return ((int)(signed char)(u8)t * (int)(signed char)(u8)c) >> 5;
}

static inline void wl_uncolour(u32 *px, int w, int h, const u32 *elems, int bits) {
    int ew = (w + (1 << bits) - 1) >> bits;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 e = elems[(y >> bits) * ew + (x >> bits)];
            u32 p = px[y * w + x];
            u32 g = (p >> 8) & 0xFF;
            u32 r = ((p >> 16) + (u32)wl_delta(e, g)) & 0xFF;          /* green to red */
            u32 bl = (p + (u32)wl_delta(e >> 8, g)) & 0xFF;            /* green to blue */
            bl = (bl + (u32)wl_delta(e >> 16, r)) & 0xFF;              /* red to blue */
            px[y * w + x] = (p & 0xFF00FF00u) | (r << 16) | bl;
        }
}

static inline void wl_add_green(u32 *px, int n) {
    for (int i = 0; i < n; i++) {
        u32 p = px[i], g = (p >> 8) & 0xFF;
        px[i] = (p & 0xFF00FF00u) | ((((p >> 16) + g) & 0xFF) << 16) | ((p + g) & 0xFF);
    }
}

typedef struct {
    int type;                /* 0 predictor, 1 colour, 2 subtract green, 3 colour table */
    int bits;                /* block size, or indices per pixel for a table */
    int w;                   /* the width the inverse gives back */
    u32 *data;               /* the transform's image, or the table (256) */
} wl_transform;

/* A whole lossless stream: with its header (`header`, the VP8L chunk) or
   without (an ALPH chunk, whose size is given). On success *out is the ARGB
   pixels, w * h of them. */
static int vp8l_decode(const u8 *d, int n, int header, int *wp, int *hp, u32 **out) {
    *out = 0;
    wl_bits b;
    b.in = d; b.n = n; b.at = 0; b.acc = 0; b.bits = 0; b.err = 0;
    int w = *wp, h = *hp;
    if (header) {
        if (n < 5) return WEBP_TRUNCATED;
        if (d[0] != 0x2F) return WEBP_BAD;
        b.at = 1;
        w = (int)wl_read(&b, 14) + 1;
        h = (int)wl_read(&b, 14) + 1;
        wl_read(&b, 1);                      /* alpha_is_used: a hint only */
        if (wl_read(&b, 3) != 0) return WEBP_BAD;
    }
    if (w > WEBP_MAX_SIDE || h > WEBP_MAX_SIDE || w * h > WEBP_MAX_PIXELS) return WEBP_TOO_BIG;

    wl_transform t[4];
    int nt = 0, seen = 0, xsize = w, rc = WEBP_OK;
    int budget = WEBP_CODE_BUDGET;
    u32 *px = 0;

    while (wl_read(&b, 1)) {
        int type = (int)wl_read(&b, 2);
        if (b.err) { rc = b.err; goto fail; }
        if (seen & (1 << type)) { rc = WEBP_BAD; goto fail; }   /* each once */
        seen |= 1 << type;
        WEBP_SAW(transform[type]);
        wl_transform *tr = &t[nt++];
        tr->type = type;
        tr->w = xsize;
        tr->data = 0;
        tr->bits = 0;
        if (type == 0 || type == 1) {
            tr->bits = (int)wl_read(&b, 3) + 2;
            int bw = (xsize + (1 << tr->bits) - 1) >> tr->bits;
            int bh = (h + (1 << tr->bits) - 1) >> tr->bits;
            tr->data = (u32 *)malloc((u64)bw * bh * 4);
            if (!tr->data) { rc = WEBP_TOO_BIG; goto fail; }
            rc = wl_image(&b, bw, bh, 0, tr->data, &budget);
            if (rc != WEBP_OK) goto fail;
        } else if (type == 3) {
            int colours = (int)wl_read(&b, 8) + 1;
            tr->data = (u32 *)calloc(256 * 4);
            if (!tr->data) { rc = WEBP_TOO_BIG; goto fail; }
            rc = wl_image(&b, colours, 1, 0, tr->data, &budget);
            if (rc != WEBP_OK) goto fail;
            /* Each colour is sent as its difference from the one before. */
            for (int i = 1; i < colours; i++) tr->data[i] = wl_add(tr->data[i], tr->data[i - 1]);
            tr->bits = colours <= 2 ? 3 : colours <= 4 ? 2 : colours <= 16 ? 1 : 0;
            xsize = (xsize + (1 << tr->bits) - 1) >> tr->bits;
        }
    }
    if (b.err) { rc = b.err; goto fail; }

    px = (u32 *)malloc((u64)xsize * h * 4);
    if (!px) { rc = WEBP_TOO_BIG; goto fail; }
    rc = wl_image(&b, xsize, h, 1, px, &budget);
    if (rc != WEBP_OK) goto fail;

    for (int i = nt - 1; i >= 0; i--) {
        wl_transform *tr = &t[i];
        if (tr->type == 0) wl_unpredict(px, tr->w, h, tr->data, tr->bits);
        else if (tr->type == 1) wl_uncolour(px, tr->w, h, tr->data, tr->bits);
        else if (tr->type == 2) wl_add_green(px, tr->w * h);
        else {
            /* Indices into the table, unpacked when several share a pixel:
               the first in the lowest bits. An index past the table is
               transparent black, which the zeroed table gives. */
            int packed_w = (tr->w + (1 << tr->bits) - 1) >> tr->bits;
            u32 *wide = (u32 *)malloc((u64)tr->w * h * 4);
            if (!wide) { rc = WEBP_TOO_BIG; goto fail; }
            int per = 1 << tr->bits, width = 8 >> tr->bits, mask = (1 << width) - 1;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < tr->w; x++) {
                    u32 g = (px[y * packed_w + (x >> tr->bits)] >> 8) & 0xFF;
                    int idx = (int)(g >> ((x & (per - 1)) * width)) & mask;
                    wide[y * tr->w + x] = tr->data[idx];
                }
            free(px);
            px = wide;
        }
    }

    for (int i = 0; i < nt; i++) if (t[i].data) free(t[i].data);
    *wp = w;
    *hp = h;
    *out = px;
    return WEBP_OK;

fail:
    for (int i = 0; i < nt; i++) if (t[i].data) free(t[i].data);
    if (px) free(px);
    return rc;
}

/* ======================================================================
 * VP8, the lossy format (RFC 6386)
 * ====================================================================== */

/* --- the boolean decoder (section 7) -------------------------------------
 *
 * Arithmetic decoding one bit at a time, each with its own probability of
 * being zero. `value` holds the next 8 + `bits` bits of the stream; its top
 * eight are what is compared with the split, and normalising moves that
 * window down rather than shifting the value up. Past the end of its
 * partition it reads zeros, as the encoder's padding does; the lengths of
 * the partitions are checked against the chunk before anything is read. */
typedef struct {
    const u8 *p, *end;
    u32 value;
    int bits;
    u32 range;               /* 128..255 between bits */
} vb_dec;

static inline void vb_fill(vb_dec *d) {
    while (d->bits < 16) {
        u32 byte = d->p < d->end ? *d->p++ : 0;
        d->value = (d->value << 8) | byte;
        d->bits += 8;
    }
}

static inline void vb_init(vb_dec *d, const u8 *p, int n) {
    d->p = p;
    d->end = p + n;
    d->value = 0;
    d->bits = -8;
    d->range = 255;
    vb_fill(d);
}

static inline int vb_read(vb_dec *d, int prob) {
    u32 split = 1 + (((d->range - 1) * (u32)prob) >> 8);
    u32 big = split << d->bits;
    int bit;
    if (d->value >= big) { d->range -= split; d->value -= big; bit = 1; }
    else { d->range = split; bit = 0; }
    int shift = __builtin_clz(d->range) - 24;
    d->range <<= shift;
    d->bits -= shift;
    if (d->bits < 0) vb_fill(d);
    return bit;
}

static inline int vb_lit(vb_dec *d, int n) {
    int v = 0;
    while (n--) v = (v << 1) | vb_read(d, 128);
    return v;
}

/* A flag, then a magnitude and a sign; zero when the flag is clear. */
static inline int vb_maybe_signed(vb_dec *d, int n) {
    if (!vb_read(d, 128)) return 0;
    int v = vb_lit(d, n);
    return vb_read(d, 128) ? -v : v;
}

/* A value from one of the RFC's trees: a positive entry is the next node,
   anything else the negated leaf (section 8.1). */
static inline int vb_tree(vb_dec *d, const signed char *tree, const u8 *probs, int at) {
    while ((at = tree[at + vb_read(d, probs[at >> 1])]) > 0) {}
    return -at;
}

/* --- the decoder's state ------------------------------------------------- */

enum { VP8_DC_PRED, VP8_V_PRED, VP8_H_PRED, VP8_TM_PRED, VP8_B_PRED };
enum { VP8_B_DC, VP8_B_TM, VP8_B_VE, VP8_B_HE, VP8_B_LD, VP8_B_RD, VP8_B_VR, VP8_B_VL,
       VP8_B_HD, VP8_B_HU };

typedef struct {
    int w, h, mbw, mbh;
    u8 *y, *u, *v;           /* whole macroblocks, before the loop filter */
    int ys, uvs;

    int seg_on, seg_map, seg_abs;
    int seg_q[4], seg_lf[4];
    u8 seg_prob[3];
    int simple, level, sharpness;
    int lf_adj, ref_delta[4], mode_delta[4];
    short dq[4][3][2];       /* per segment: Y, Y2, chroma; DC then AC */
    u8 coeff[4][8][3][11];
    int skip_on, skip_prob;

    u8 *mb_level;            /* per macroblock, for the loop filter */
    u8 *mb_inner;
    u8 *above_modes;         /* subblock modes along the row above, 4 per macroblock */
    u8 left_modes[4];
    u8 *above_nz;            /* 9 per macroblock: 4 Y, 2 U, 2 V, Y2 */
    u8 left_nz[9];
} vp8_dec;

/* --- coefficients (section 13) ------------------------------------------- */

/* One block's tokens, dequantised into zigzag order in `out`. Returns
   whether any token came before the end of the block, which is what the
   blocks to the right and below take as their context, and what decides
   whether the loop filter touches the edges inside a macroblock. */
static inline int vp8_block(vb_dec *d, u8 (*probs)[3][11], int ctx, int first,
                            const short *dq, short *out) {
    int c = first;
    const u8 *p = probs[VP8_COEFF_BANDS[c]][ctx];
    if (!vb_read(d, p[0])) return 0;             /* the end, straight away */
    for (;;) {
        if (!vb_read(d, p[1])) {
            /* A zero; the end of the block cannot follow one, so the next
               token skips that branch of the tree. */
            if (++c == 16) return 1;
            p = probs[VP8_COEFF_BANDS[c]][0];
            continue;
        }
        int tok = vb_tree(d, VP8_COEFF_TREE, p, 4);
        int v = tok;
        if (tok > 4) {
            static const u8 *const EXTRA[6] = { VP8_PCAT1, VP8_PCAT2, VP8_PCAT3,
                                                VP8_PCAT4, VP8_PCAT5, VP8_PCAT6 };
            const u8 *e = EXTRA[tok - 5];
            int more = 0;
            while (*e) more = more + more + vb_read(d, *e++);
            v = VP8_DCT_CAT_BASE[tok - 5] + more;
        }
        int neg = vb_read(d, 128);
        /* Stored in sixteen bits, as the specification has it. */
        out[VP8_ZIGZAG[c]] = (short)((neg ? -v : v) * dq[c > 0]);
        if (++c == 16) return 1;
        p = probs[VP8_COEFF_BANDS[c]][v > 1 ? 2 : 1];
        if (!vb_read(d, p[0])) return 1;
    }
}

/* --- the inverse transforms (section 14) -------------------------------- */

static inline void vp8_iwht(const short *in, short *dc) {
    int t[16];
    for (int i = 0; i < 4; i++) {
        int a = in[i] + in[12 + i], b = in[4 + i] + in[8 + i];
        int c = in[4 + i] - in[8 + i], d = in[i] - in[12 + i];
        t[i] = a + b;
        t[4 + i] = c + d;
        t[8 + i] = a - b;
        t[12 + i] = d - c;
    }
    for (int r = 0; r < 4; r++) {
        const int *s = t + r * 4;
        int a = s[0] + s[3], b = s[1] + s[2], c = s[1] - s[2], d = s[0] - s[3];
        dc[r * 4 + 0] = (short)((a + b + 3) >> 3);
        dc[r * 4 + 1] = (short)((c + d + 3) >> 3);
        dc[r * 4 + 2] = (short)((a - b + 3) >> 3);
        dc[r * 4 + 3] = (short)((d - c + 3) >> 3);
    }
}

/* x * sqrt(2) * cos(pi/8) and x * sqrt(2) * sin(pi/8) in sixteen bit fixed
   point; the first is over one, so it is x plus x times the rest. */
static inline int vp8_mul_cos(int x) { return x + ((x * 20091) >> 16); }
static inline int vp8_mul_sin(int x) { return (x * 35468) >> 16; }

/* Columns, then rows, the halfway values kept in sixteen bits; the result
   is added to the prediction already in `dst`. */
static inline void vp8_idct_add(const short *in, u8 *dst, int stride) {
    short t[16];
    for (int i = 0; i < 4; i++) {
        int a = in[i] + in[8 + i], b = in[i] - in[8 + i];
        int c = vp8_mul_sin(in[4 + i]) - vp8_mul_cos(in[12 + i]);
        int d = vp8_mul_cos(in[4 + i]) + vp8_mul_sin(in[12 + i]);
        t[i] = (short)(a + d);
        t[4 + i] = (short)(b + c);
        t[8 + i] = (short)(b - c);
        t[12 + i] = (short)(a - d);
    }
    for (int r = 0; r < 4; r++) {
        const short *s = t + r * 4;
        int a = s[0] + s[2], b = s[0] - s[2];
        int c = vp8_mul_sin(s[1]) - vp8_mul_cos(s[3]);
        int d = vp8_mul_cos(s[1]) + vp8_mul_sin(s[3]);
        u8 *o = dst + r * stride;
        o[0] = (u8)webp_clamp255(o[0] + ((a + d + 4) >> 3));
        o[1] = (u8)webp_clamp255(o[1] + ((b + c + 4) >> 3));
        o[2] = (u8)webp_clamp255(o[2] + ((b - c + 4) >> 3));
        o[3] = (u8)webp_clamp255(o[3] + ((a - d + 4) >> 3));
    }
}

/* --- prediction (section 12) ---------------------------------------------
 *
 * From the pixels above and to the left, already reconstructed and not yet
 * loop filtered (the filter runs over the whole frame afterwards). Outside
 * the frame the row above counts as 127 and the column to the left as 129,
 * the corner going with the row above. */

/* A whole block of `size` (16 luma, 8 chroma) at (x0, y0) of a plane. */
static inline void vp8_predict_block(u8 *plane, int stride, int x0, int y0, int size, int mode) {
    u8 above[16], left[16];
    int has_above = y0 > 0, has_left = x0 > 0;
    for (int i = 0; i < size; i++) {
        above[i] = has_above ? plane[(y0 - 1) * stride + x0 + i] : 127;
        left[i] = has_left ? plane[(y0 + i) * stride + x0 - 1] : 129;
    }
    int corner = !has_above ? 127 : !has_left ? 129 : plane[(y0 - 1) * stride + x0 - 1];
    u8 *dst = plane + y0 * stride + x0;
    int shift = size == 16 ? 4 : 3;

    if (mode == VP8_DC_PRED) {
        /* The average of what is there: both edges, one, or neither. */
        int sum = 0, dc;
        if (has_above && has_left) {
            for (int i = 0; i < size; i++) sum += above[i] + left[i];
            dc = (sum + size) >> (shift + 1);
        } else if (has_above || has_left) {
            for (int i = 0; i < size; i++) sum += has_above ? above[i] : left[i];
            dc = (sum + (size >> 1)) >> shift;
        } else dc = 128;
        for (int r = 0; r < size; r++)
            for (int c = 0; c < size; c++) dst[r * stride + c] = (u8)dc;
    } else if (mode == VP8_V_PRED) {
        for (int r = 0; r < size; r++)
            for (int c = 0; c < size; c++) dst[r * stride + c] = above[c];
    } else if (mode == VP8_H_PRED) {
        for (int r = 0; r < size; r++)
            for (int c = 0; c < size; c++) dst[r * stride + c] = left[r];
    } else {
        for (int r = 0; r < size; r++)
            for (int c = 0; c < size; c++)
                dst[r * stride + c] = (u8)webp_clamp255(left[r] + above[c] - corner);
    }
}

static inline u8 vp8_avg3(int x, int y, int z) { return (u8)((x + y + y + z + 2) >> 2); }
static inline u8 vp8_avg2(int x, int y) { return (u8)((x + y + 1) >> 1); }

/* A 4x4 subblock from its edges: A[0..7] above (four over it and four over
   and to the right), L[0..3] to its left, and P, the corner (section 12.3).
   E is the edge as one line, from the bottom of L round to the end of the
   four over the block, which is what the diagonal modes walk along. */
static inline void vp8_predict_sub(u8 *dst, int stride, const u8 *A, const u8 *L, int P, int mode) {
    u8 B[4][4];
    int E[9] = { L[3], L[2], L[1], L[0], P, A[0], A[1], A[2], A[3] };
    switch (mode) {
    case VP8_B_DC: {
        int v = 4;
        for (int i = 0; i < 4; i++) v += A[i] + L[i];
        v >>= 3;
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) B[r][c] = (u8)v;
        break;
    }
    case VP8_B_TM:
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) B[r][c] = (u8)webp_clamp255(L[r] + A[c] - P);
        break;
    case VP8_B_VE:
        for (int c = 0; c < 4; c++) {
            u8 v = vp8_avg3(c ? A[c - 1] : P, A[c], A[c + 1]);
            for (int r = 0; r < 4; r++) B[r][c] = v;
        }
        break;
    case VP8_B_HE:
        for (int r = 0; r < 4; r++) {
            u8 v = r == 3 ? vp8_avg3(L[2], L[3], L[3]) : vp8_avg3(r ? L[r - 1] : P, L[r], L[r + 1]);
            for (int c = 0; c < 4; c++) B[r][c] = v;
        }
        break;
    case VP8_B_LD:
        /* Down and to the left: each anti-diagonal is the smoothed pixel
           above at its own place, the last with no pixel past it. */
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                int i = r + c;
                B[r][c] = i == 6 ? vp8_avg3(A[6], A[7], A[7]) : vp8_avg3(A[i], A[i + 1], A[i + 2]);
            }
        break;
    case VP8_B_RD:
        /* Down and to the right: along E, from the bottom left. */
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                int i = 4 - r + c;
                B[r][c] = vp8_avg3(E[i - 1], E[i], E[i + 1]);
            }
        break;
    case VP8_B_VR:
        B[3][0] = vp8_avg3(E[1], E[2], E[3]);
        B[2][0] = vp8_avg3(E[2], E[3], E[4]);
        B[3][1] = B[1][0] = vp8_avg3(E[3], E[4], E[5]);
        B[2][1] = B[0][0] = vp8_avg2(E[4], E[5]);
        B[3][2] = B[1][1] = vp8_avg3(E[4], E[5], E[6]);
        B[2][2] = B[0][1] = vp8_avg2(E[5], E[6]);
        B[3][3] = B[1][2] = vp8_avg3(E[5], E[6], E[7]);
        B[2][3] = B[0][2] = vp8_avg2(E[6], E[7]);
        B[1][3] = vp8_avg3(E[6], E[7], E[8]);
        B[0][3] = vp8_avg2(E[7], E[8]);
        break;
    case VP8_B_VL:
        B[0][0] = vp8_avg2(A[0], A[1]);
        B[1][0] = vp8_avg3(A[0], A[1], A[2]);
        B[2][0] = B[0][1] = vp8_avg2(A[1], A[2]);
        B[1][1] = B[3][0] = vp8_avg3(A[1], A[2], A[3]);
        B[2][1] = B[0][2] = vp8_avg2(A[2], A[3]);
        B[3][1] = B[1][2] = vp8_avg3(A[2], A[3], A[4]);
        B[2][2] = B[0][3] = vp8_avg2(A[3], A[4]);
        B[3][2] = B[1][3] = vp8_avg3(A[3], A[4], A[5]);
        /* The last two break the pattern, as the specification says. */
        B[2][3] = vp8_avg3(A[4], A[5], A[6]);
        B[3][3] = vp8_avg3(A[5], A[6], A[7]);
        break;
    case VP8_B_HD:
        B[3][0] = vp8_avg2(E[0], E[1]);
        B[3][1] = vp8_avg3(E[0], E[1], E[2]);
        B[2][0] = B[3][2] = vp8_avg2(E[1], E[2]);
        B[2][1] = B[3][3] = vp8_avg3(E[1], E[2], E[3]);
        B[2][2] = B[1][0] = vp8_avg2(E[2], E[3]);
        B[2][3] = B[1][1] = vp8_avg3(E[2], E[3], E[4]);
        B[1][2] = B[0][0] = vp8_avg2(E[3], E[4]);
        B[1][3] = B[0][1] = vp8_avg3(E[3], E[4], E[5]);
        B[0][2] = vp8_avg3(E[4], E[5], E[6]);
        B[0][3] = vp8_avg3(E[5], E[6], E[7]);
        break;
    default: /* VP8_B_HU */
        B[0][0] = vp8_avg2(L[0], L[1]);
        B[0][1] = vp8_avg3(L[0], L[1], L[2]);
        B[0][2] = B[1][0] = vp8_avg2(L[1], L[2]);
        B[0][3] = B[1][1] = vp8_avg3(L[1], L[2], L[3]);
        B[1][2] = B[2][0] = vp8_avg2(L[2], L[3]);
        B[1][3] = B[2][1] = vp8_avg3(L[2], L[3], L[3]);
        B[2][2] = B[2][3] = B[3][0] = B[3][1] = B[3][2] = B[3][3] = L[3];
        break;
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) dst[r * stride + c] = B[r][c];
}

/* The edges subblock i (0..15, in raster order) of the macroblock at
   (mx, my) is predicted from, out of the frame as far as it has been
   reconstructed (section 12.3). The four pixels over and to the right of
   the macroblock serve every subblock down its right side, since the ones
   beside those are not decoded yet; off the right edge of the frame they
   repeat the last pixel above, and on the top row they are 127 like the
   rest of the row above. */
static inline void vp8_sub_edges(const vp8_dec *s, int mx, int my, int i, u8 *A, u8 *L, int *P) {
    int bx = i & 3, by = i >> 2;
    int x0 = mx * 16, y0 = my * 16;
    int sx = x0 + bx * 4, sy = y0 + by * 4;
    for (int k = 0; k < 4; k++) {
        A[k] = sy > 0 ? s->y[(sy - 1) * s->ys + sx + k] : 127;
        L[k] = sx > 0 ? s->y[(sy + k) * s->ys + sx - 1] : 129;
        if (bx < 3) A[4 + k] = sy > 0 ? s->y[(sy - 1) * s->ys + sx + 4 + k] : 127;
        else if (my == 0) A[4 + k] = 127;
        else if (mx + 1 < s->mbw) A[4 + k] = s->y[(y0 - 1) * s->ys + x0 + 16 + k];
        else A[4 + k] = s->y[(y0 - 1) * s->ys + x0 + 15];
    }
    *P = sy == 0 ? 127 : sx == 0 ? 129 : s->y[(sy - 1) * s->ys + sx - 1];
}

/* --- one macroblock (sections 10 to 14) ---------------------------------- */

static inline void vp8_macroblock(vp8_dec *s, vb_dec *modes, vb_dec *tokens, int mx, int my) {
    int seg = s->seg_map ? vb_tree(modes, VP8_SEGMENT_TREE, s->seg_prob, 0) : 0;
    int skip = s->skip_on ? vb_read(modes, s->skip_prob) : 0;
    int ymode = vb_tree(modes, VP8_KF_YMODE_TREE, VP8_KF_YMODE_PROB, 0);
    WEBP_SAW(ymode[ymode]);

    /* The subblock modes, each read with the modes above and to the left
       of it as context -- which for a macroblock predicted whole is the one
       its whole mode stands for. */
    u8 bmodes[16];
    u8 *above = s->above_modes + mx * 4;
    if (ymode == VP8_B_PRED) {
        WEBP_SAW(bpred);
        for (int i = 0; i < 16; i++) {
            int a = i < 4 ? above[i] : bmodes[i - 4];
            int l = (i & 3) ? bmodes[i - 1] : s->left_modes[i >> 2];
            bmodes[i] = (u8)vb_tree(modes, VP8_BMODE_TREE, VP8_KF_BMODE_PROB[a][l], 0);
        }
    } else {
        static const u8 STANDS_FOR[4] = { VP8_B_DC, VP8_B_VE, VP8_B_HE, VP8_B_TM };
        for (int i = 0; i < 16; i++) bmodes[i] = STANDS_FOR[ymode];
    }
    for (int i = 0; i < 4; i++) {
        above[i] = bmodes[12 + i];
        s->left_modes[i] = bmodes[i * 4 + 3];
    }
    int uvmode = vb_tree(modes, VP8_UV_MODE_TREE, VP8_KF_UV_MODE_PROB, 0);

    /* The residue: Y2 first when there is one, then 16 Y, 4 U, 4 V. */
    short coef[25 * 16];
    for (int i = 0; i < 25 * 16; i++) coef[i] = 0;
    u8 *anz = s->above_nz + mx * 9, *lnz = s->left_nz;
    int has_y2 = ymode != VP8_B_PRED;
    int any = 0;
    short (*dq)[2] = s->dq[seg];
    if (!skip) {
        int first = 0;
        u8 (*yprobs)[3][11] = s->coeff[3];
        if (has_y2) {
            int nz = vp8_block(tokens, s->coeff[1], anz[8] + lnz[8], 0, dq[1], coef + 24 * 16);
            anz[8] = lnz[8] = (u8)nz;
            any |= nz;
            first = 1;
            yprobs = s->coeff[0];
        }
        for (int by = 0; by < 4; by++)
            for (int bx = 0; bx < 4; bx++) {
                int nz = vp8_block(tokens, yprobs, anz[bx] + lnz[by], first, dq[0],
                                   coef + (by * 4 + bx) * 16);
                anz[bx] = lnz[by] = (u8)nz;
                any |= nz;
            }
        for (int k = 0; k < 2; k++)                      /* U, then V */
            for (int by = 0; by < 2; by++)
                for (int bx = 0; bx < 2; bx++) {
                    int nz = vp8_block(tokens, s->coeff[2], anz[4 + k * 2 + bx] + lnz[4 + k * 2 + by],
                                       0, dq[2], coef + (16 + k * 4 + by * 2 + bx) * 16);
                    anz[4 + k * 2 + bx] = lnz[4 + k * 2 + by] = (u8)nz;
                    any |= nz;
                }
    } else {
        /* Nothing coded: the contexts go empty, except Y2's, which belongs
           to the last macroblock that had one. */
        for (int i = 0; i < 8; i++) anz[i] = lnz[i] = 0;
        if (has_y2) anz[8] = lnz[8] = 0;
    }

    /* Luma: predicted whole and the residue added, or subblock by subblock,
       each predicted from the ones already done. */
    int x0 = mx * 16, y0 = my * 16;
    if (has_y2) {
        short dc[16];
        vp8_iwht(coef + 24 * 16, dc);
        for (int i = 0; i < 16; i++) coef[i * 16] = dc[i];
        vp8_predict_block(s->y, s->ys, x0, y0, 16, ymode);
        for (int i = 0; i < 16; i++)
            vp8_idct_add(coef + i * 16, s->y + (y0 + (i >> 2) * 4) * s->ys + x0 + (i & 3) * 4, s->ys);
    } else {
        for (int i = 0; i < 16; i++) {
            u8 A[8], L[4];
            int P;
            vp8_sub_edges(s, mx, my, i, A, L, &P);
            u8 *dst = s->y + (y0 + (i >> 2) * 4) * s->ys + x0 + (i & 3) * 4;
            vp8_predict_sub(dst, s->ys, A, L, P, bmodes[i]);
            vp8_idct_add(coef + i * 16, dst, s->ys);
        }
    }

    for (int k = 0; k < 2; k++) {
        u8 *plane = k ? s->v : s->u;
        vp8_predict_block(plane, s->uvs, mx * 8, my * 8, 8, uvmode);
        for (int i = 0; i < 4; i++)
            vp8_idct_add(coef + (16 + k * 4 + i) * 16,
                         plane + (my * 8 + (i >> 1) * 4) * s->uvs + mx * 8 + (i & 1) * 4, s->uvs);
    }

    /* What the loop filter will need (section 15.4, and 9.3 for the
       segment's level): the level, and whether the edges inside the
       macroblock are filtered at all. */
    int level = s->level;
    if (s->seg_on) level = s->seg_abs ? s->seg_lf[seg] : level + s->seg_lf[seg];
    level = level < 0 ? 0 : level > 63 ? 63 : level;
    if (s->lf_adj) {
        level += s->ref_delta[0];                    /* every key frame block is intra */
        if (ymode == VP8_B_PRED) level += s->mode_delta[0];
        level = level < 0 ? 0 : level > 63 ? 63 : level;
    }
    s->mb_level[my * s->mbw + mx] = (u8)level;
    s->mb_inner[my * s->mbw + mx] = (u8)(ymode == VP8_B_PRED || any);
}

/* --- the loop filter (section 15) ---------------------------------------
 *
 * Each filter looks at pixels either side of an edge, p3..p0 before it and
 * q0..q3 after, `step` apart (1 across a vertical edge, the stride across a
 * horizontal one), and pulls the two sides together when the difference is
 * small enough to be the quantiser's doing rather than the picture's. */

static inline int lf_c8(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static inline u8 lf_u8(int v) { return (u8)(lf_c8(v) + 128); }

static inline int lf_edge_ok(const u8 *p, int step, int limit) {
    return webp_abs(p[-step] - p[0]) * 2 + (webp_abs(p[-2 * step] - p[step]) >> 1) <= limit;
}

static inline int lf_normal_ok(const u8 *p, int step, int edge, int inner) {
    int p3 = p[-4 * step], p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
    int q0 = p[0], q1 = p[step], q2 = p[2 * step], q3 = p[3 * step];
    return lf_edge_ok(p, step, edge)
        && webp_abs(p3 - p2) <= inner && webp_abs(p2 - p1) <= inner && webp_abs(p1 - p0) <= inner
        && webp_abs(q3 - q2) <= inner && webp_abs(q2 - q1) <= inner && webp_abs(q1 - q0) <= inner;
}

static inline int lf_hev(const u8 *p, int step, int t) {
    return webp_abs(p[-2 * step] - p[-step]) > t || webp_abs(p[step] - p[0]) > t;
}

/* The adjustment every filter shares: about an eighth of three times the
   step at the edge (with the outer pixels too, when asked), taken from q0
   and given to p0, the halves rounded so one side gets the odd one. */
static inline int lf_common(u8 *p, int step, int outer) {
    int p1 = p[-2 * step] - 128, p0 = p[-step] - 128, q0 = p[0] - 128, q1 = p[step] - 128;
    int a = lf_c8((outer ? lf_c8(p1 - q1) : 0) + 3 * (q0 - p0));
    int b = lf_c8(a + 3) >> 3;
    a = lf_c8(a + 4) >> 3;
    p[0] = lf_u8(q0 - a);
    p[-step] = lf_u8(p0 + b);
    return a;
}

static inline void lf_simple_edge(u8 *p, int step, int along, int limit) {
    for (int i = 0; i < 16; i++, p += along)
        if (lf_edge_ok(p, step, limit)) lf_common(p, step, 1);
}

static inline void lf_inner_edge(u8 *p, int step, int along, int n, int edge, int inner, int hevt) {
    for (int i = 0; i < n; i++, p += along) {
        if (!lf_normal_ok(p, step, edge, inner)) continue;
        int hv = lf_hev(p, step, hevt);
        int a = (lf_common(p, step, hv) + 1) >> 1;
        if (!hv) {
            p[step] = lf_u8(p[step] - 128 - a);
            p[-2 * step] = lf_u8(p[-2 * step] - 128 + a);
        }
    }
}

static inline void lf_mb_edge(u8 *p, int step, int along, int n, int edge, int inner, int hevt) {
    for (int i = 0; i < n; i++, p += along) {
        if (!lf_normal_ok(p, step, edge, inner)) continue;
        if (lf_hev(p, step, hevt)) { lf_common(p, step, 1); continue; }
        int p2 = p[-3 * step] - 128, p1 = p[-2 * step] - 128, p0 = p[-step] - 128;
        int q0 = p[0] - 128, q1 = p[step] - 128, q2 = p[2 * step] - 128;
        int w = lf_c8(lf_c8(p1 - q1) + 3 * (q0 - p0));
        int a = lf_c8((27 * w + 63) >> 7);
        p[0] = lf_u8(q0 - a);
        p[-step] = lf_u8(p0 + a);
        a = lf_c8((18 * w + 63) >> 7);
        p[step] = lf_u8(q1 - a);
        p[-2 * step] = lf_u8(p1 + a);
        a = lf_c8((9 * w + 63) >> 7);
        p[2 * step] = lf_u8(q2 - a);
        p[-3 * step] = lf_u8(p2 + a);
    }
}

/* Macroblock by macroblock in raster order, each doing its left edge, the
   edges inside it, its top edge, then the ones inside the other way: the
   order matters, since pixels near a corner are filtered more than once. */
static inline void vp8_loop_filter(vp8_dec *s) {
    for (int my = 0; my < s->mbh; my++)
        for (int mx = 0; mx < s->mbw; mx++) {
            int level = s->mb_level[my * s->mbw + mx];
            if (level == 0) continue;
            int inner_on = s->mb_inner[my * s->mbw + mx];
            int interior = level;
            if (s->sharpness) {
                interior >>= s->sharpness > 4 ? 2 : 1;
                if (interior > 9 - s->sharpness) interior = 9 - s->sharpness;
            }
            if (interior < 1) interior = 1;
            int hevt = level >= 40 ? 2 : level >= 15 ? 1 : 0;
            int mb_edge = (level + 2) * 2 + interior, sub_edge = level * 2 + interior;
            u8 *y = s->y + my * 16 * s->ys + mx * 16;
            u8 *u = s->u + my * 8 * s->uvs + mx * 8;
            u8 *v = s->v + my * 8 * s->uvs + mx * 8;
            if (s->simple) {
                WEBP_SAW(simple_filter);
                if (mx > 0) lf_simple_edge(y, 1, s->ys, mb_edge);
                if (inner_on)
                    for (int e = 4; e < 16; e += 4) lf_simple_edge(y + e, 1, s->ys, sub_edge);
                if (my > 0) lf_simple_edge(y, s->ys, 1, mb_edge);
                if (inner_on)
                    for (int e = 4; e < 16; e += 4) lf_simple_edge(y + e * s->ys, s->ys, 1, sub_edge);
                continue;
            }
            WEBP_SAW(normal_filter);
            if (mx > 0) {
                lf_mb_edge(y, 1, s->ys, 16, mb_edge, interior, hevt);
                lf_mb_edge(u, 1, s->uvs, 8, mb_edge, interior, hevt);
                lf_mb_edge(v, 1, s->uvs, 8, mb_edge, interior, hevt);
            }
            if (inner_on) {
                for (int e = 4; e < 16; e += 4) lf_inner_edge(y + e, 1, s->ys, 16, sub_edge, interior, hevt);
                lf_inner_edge(u + 4, 1, s->uvs, 8, sub_edge, interior, hevt);
                lf_inner_edge(v + 4, 1, s->uvs, 8, sub_edge, interior, hevt);
            }
            if (my > 0) {
                lf_mb_edge(y, s->ys, 1, 16, mb_edge, interior, hevt);
                lf_mb_edge(u, s->uvs, 1, 8, mb_edge, interior, hevt);
                lf_mb_edge(v, s->uvs, 1, 8, mb_edge, interior, hevt);
            }
            if (inner_on) {
                for (int e = 4; e < 16; e += 4)
                    lf_inner_edge(y + e * s->ys, s->ys, 1, 16, sub_edge, interior, hevt);
                lf_inner_edge(u + 4 * s->uvs, s->uvs, 1, 8, sub_edge, interior, hevt);
                lf_inner_edge(v + 4 * s->uvs, s->uvs, 1, 8, sub_edge, interior, hevt);
            }
        }
}

/* --- colour --------------------------------------------------------------
 *
 * BT.601, as RFC 9649 says to use: brightness from 16 to 235 and colour
 * differences about 128, in sixteen bit fixed point (255/219, and the four
 * colour weights). */
static inline u32 vp8_rgb(int y, int u, int v) {
    int c = (y - 16) * 76309, d = u - 128, e = v - 128;
    int r = (c + 104597 * e + 32768) >> 16;
    int g = (c - 25675 * d - 53279 * e + 32768) >> 16;
    int b = (c + 132201 * d + 32768) >> 16;
    return ((u32)webp_clamp255(r) << 16) | ((u32)webp_clamp255(g) << 8) | (u32)webp_clamp255(b);
}

static inline void vp8_free(vp8_dec *s) {
    if (s->y) free(s->y);
    if (s->u) free(s->u);
    if (s->v) free(s->v);
    if (s->mb_level) free(s->mb_level);
    if (s->mb_inner) free(s->mb_inner);
    if (s->above_modes) free(s->above_modes);
    if (s->above_nz) free(s->above_nz);
}

/* A VP8 key frame (the "VP8 " chunk's bytes) to opaque ARGB pixels. */
static int vp8_decode(const u8 *d, int n, int *wp, int *hp, u32 **out) {
    *out = 0;
    if (n < 10) return WEBP_TRUNCATED;
    u32 tag = webp_le24(d);
    if (tag & 1) return WEBP_UNSUPPORTED;            /* not a key frame */
    if (((tag >> 1) & 7) > 3) return WEBP_BAD;       /* no such version */
    int first = (int)(tag >> 5);
    if (d[3] != 0x9D || d[4] != 0x01 || d[5] != 0x2A) return WEBP_BAD;
    int w = (int)(webp_le16(d + 6) & 0x3FFF), h = (int)(webp_le16(d + 8) & 0x3FFF);
    if (!w || !h) return WEBP_BAD;
    if (w > WEBP_MAX_SIDE || h > WEBP_MAX_SIDE || w * h > WEBP_MAX_PIXELS) return WEBP_TOO_BIG;
    if (first > n - 10) return WEBP_TRUNCATED;
    WEBP_SAW(vp8);

    vp8_dec s;
    memset(&s, 0, sizeof s);
    vb_dec hd;
    vb_init(&hd, d + 10, first);

    /* The frame header (section 9, in the order of section 19.2). */
    vb_lit(&hd, 1);                                  /* colour space: one is defined */
    vb_lit(&hd, 1);                                  /* clamping: done regardless */
    s.seg_on = vb_lit(&hd, 1);
    if (s.seg_on) {
        WEBP_SAW(segments);
        s.seg_map = vb_lit(&hd, 1);
        if (vb_lit(&hd, 1)) {
            s.seg_abs = vb_lit(&hd, 1);
            for (int i = 0; i < 4; i++) s.seg_q[i] = vb_maybe_signed(&hd, 7);
            for (int i = 0; i < 4; i++) s.seg_lf[i] = vb_maybe_signed(&hd, 6);
        }
        for (int i = 0; i < 3; i++) s.seg_prob[i] = 255;
        if (s.seg_map)
            for (int i = 0; i < 3; i++) if (vb_lit(&hd, 1)) s.seg_prob[i] = (u8)vb_lit(&hd, 8);
    }
    s.simple = vb_lit(&hd, 1);
    s.level = vb_lit(&hd, 6);
    s.sharpness = vb_lit(&hd, 3);
    s.lf_adj = vb_lit(&hd, 1);
    if (s.lf_adj && vb_lit(&hd, 1)) {
        for (int i = 0; i < 4; i++) s.ref_delta[i] = vb_maybe_signed(&hd, 6);
        for (int i = 0; i < 4; i++) s.mode_delta[i] = vb_maybe_signed(&hd, 6);
    }
    int parts = 1 << vb_lit(&hd, 2);
    if (parts > 1) WEBP_SAW(partitions);
    int q = vb_lit(&hd, 7);
    int ydc = vb_maybe_signed(&hd, 4), y2dc = vb_maybe_signed(&hd, 4), y2ac = vb_maybe_signed(&hd, 4);
    int uvdc = vb_maybe_signed(&hd, 4), uvac = vb_maybe_signed(&hd, 4);
    vb_lit(&hd, 1);                                  /* refresh_entropy_probs: one frame only */

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 3; k++)
                for (int t = 0; t < 11; t++)
                    s.coeff[i][j][k][t] = vb_read(&hd, VP8_COEFF_UPDATE_PROBS[i][j][k][t])
                                        ? (u8)vb_lit(&hd, 8) : VP8_DEFAULT_COEFF_PROBS[i][j][k][t];
    s.skip_on = vb_lit(&hd, 1);
    if (s.skip_on) s.skip_prob = vb_lit(&hd, 8);

    /* The dequantisation factors of each segment (section 14.1; the
       scaling of Y2 and the cap on chroma DC are RFC 6386's dixie.c). */
    for (int g = 0; g < 4; g++) {
        int base = q;
        if (s.seg_on) base = s.seg_abs ? s.seg_q[g] : q + s.seg_q[g];
        #define WEBP_QI(v) ((v) < 0 ? 0 : (v) > 127 ? 127 : (v))
        s.dq[g][0][0] = VP8_DC_QLOOKUP[WEBP_QI(base + ydc)];
        s.dq[g][0][1] = VP8_AC_QLOOKUP[WEBP_QI(base)];
        s.dq[g][1][0] = (short)(VP8_DC_QLOOKUP[WEBP_QI(base + y2dc)] * 2);
        int y2 = VP8_AC_QLOOKUP[WEBP_QI(base + y2ac)] * 155 / 100;
        s.dq[g][1][1] = (short)(y2 < 8 ? 8 : y2);
        int uv = VP8_DC_QLOOKUP[WEBP_QI(base + uvdc)];
        s.dq[g][2][0] = (short)(uv > 132 ? 132 : uv);
        s.dq[g][2][1] = VP8_AC_QLOOKUP[WEBP_QI(base + uvac)];
        #undef WEBP_QI
    }

    /* The token partitions follow the first, each but the last with its
       size in front, three bytes, together. */
    const u8 *at = d + 10 + first;
    int left = n - 10 - first;
    if (left < 3 * (parts - 1)) return WEBP_TRUNCATED;
    const u8 *pdata = at + 3 * (parts - 1);
    left -= 3 * (parts - 1);
    vb_dec tok[8];
    for (int i = 0; i < parts; i++) {
        int size = i < parts - 1 ? (int)webp_le24(at + 3 * i) : left;
        if (size > left) return WEBP_TRUNCATED;
        vb_init(&tok[i], pdata, size);
        pdata += size;
        left -= size;
    }

    s.w = w;
    s.h = h;
    s.mbw = (w + 15) >> 4;
    s.mbh = (h + 15) >> 4;
    s.ys = s.mbw * 16;
    s.uvs = s.mbw * 8;
    s.y = (u8 *)malloc((u64)s.ys * s.mbh * 16);
    s.u = (u8 *)malloc((u64)s.uvs * s.mbh * 8);
    s.v = (u8 *)malloc((u64)s.uvs * s.mbh * 8);
    s.mb_level = (u8 *)malloc((u64)s.mbw * s.mbh);
    s.mb_inner = (u8 *)malloc((u64)s.mbw * s.mbh);
    s.above_modes = (u8 *)calloc((u64)s.mbw * 4);
    s.above_nz = (u8 *)calloc((u64)s.mbw * 9);
    u32 *px = (u32 *)malloc((u64)w * h * 4);
    if (!s.y || !s.u || !s.v || !s.mb_level || !s.mb_inner || !s.above_modes || !s.above_nz || !px) {
        vp8_free(&s);
        if (px) free(px);
        return WEBP_TOO_BIG;
    }

    for (int my = 0; my < s.mbh; my++) {
        for (int i = 0; i < 4; i++) s.left_modes[i] = VP8_B_DC;
        for (int i = 0; i < 9; i++) s.left_nz[i] = 0;
        for (int mx = 0; mx < s.mbw; mx++)
            vp8_macroblock(&s, &hd, &tok[my & (parts - 1)], mx, my);
    }
    vp8_loop_filter(&s);

    /* Each pixel's colour from the four nearest colour samples: a sample
       sits at the centre of the two by two pixels it covers, so the nearest
       weighs 9/16, the next across and the next down 3/16 each, and the
       diagonal 1/16. At the edges of the picture the last sample repeats. */
    int uw = (w + 1) >> 1, uh = (h + 1) >> 1;
    for (int y = 0; y < h; y++) {
        int j0 = y >> 1, j1 = (y & 1) ? j0 + 1 : j0 - 1;
        if (j1 < 0) j1 = 0;
        if (j1 >= uh) j1 = uh - 1;
        const u8 *u0 = s.u + j0 * s.uvs, *u1 = s.u + j1 * s.uvs;
        const u8 *v0 = s.v + j0 * s.uvs, *v1 = s.v + j1 * s.uvs;
        for (int x = 0; x < w; x++) {
            int i0 = x >> 1, i1 = (x & 1) ? i0 + 1 : i0 - 1;
            if (i1 < 0) i1 = 0;
            if (i1 >= uw) i1 = uw - 1;
            int u = (9 * u0[i0] + 3 * u0[i1] + 3 * u1[i0] + u1[i1] + 8) >> 4;
            int v = (9 * v0[i0] + 3 * v0[i1] + 3 * v1[i0] + v1[i1] + 8) >> 4;
            px[y * w + x] = 0xFF000000u | vp8_rgb(s.y[y * s.ys + x], u, v);
        }
    }
    vp8_free(&s);
    *wp = w;
    *hp = h;
    *out = px;
    return WEBP_OK;
}

/* ======================================================================
 * ALPH (RFC 9649 section 2.7.1.2)
 * ====================================================================== */

/* Sets the alpha of `px` from an ALPH chunk's bytes. */
static int webp_alpha(const u8 *d, int n, int w, int h, u32 *px) {
    if (n < 1) return WEBP_TRUNCATED;
    int method = d[0] & 3, filter = (d[0] >> 2) & 3;
    u8 *a = (u8 *)malloc((u64)w * h);
    if (!a) return WEBP_TOO_BIG;
    if (method == 0) {
        WEBP_SAW(alph_raw);
        if (n - 1 < w * h) { free(a); return WEBP_TRUNCATED; }
        for (int i = 0; i < w * h; i++) a[i] = d[1 + i];
    } else if (method == 1) {
        WEBP_SAW(alph_lossless);
        u32 *g = 0;
        int gw = w, gh = h;
        int rc = vp8l_decode(d + 1, n - 1, 0, &gw, &gh, &g);
        if (rc != WEBP_OK) { free(a); return rc; }
        for (int i = 0; i < w * h; i++) a[i] = (u8)(g[i] >> 8);
        free(g);
    } else {
        free(a);
        return WEBP_BAD;
    }

    /* The filters: each value was sent less a guess from the one to its
       left, the one above, or both (the gradient, clamped); along the top
       row the guess is the left one and down the first column the one
       above, whatever the filter, and the very first is guessed as 0. */
    WEBP_SAW(alph_filter[filter]);
    if (filter)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int guess;
                u8 *p = a + y * w + x;
                if (x == 0 && y == 0) guess = 0;
                else if (y == 0) guess = p[-1];
                else if (x == 0) guess = p[-w];
                else if (filter == 1) guess = p[-1];
                else if (filter == 2) guess = p[-w];
                else guess = webp_clamp255(p[-1] + p[-w] - p[-w - 1]);
                *p = (u8)(*p + guess);
            }

    for (int i = 0; i < w * h; i++) px[i] = (px[i] & 0x00FFFFFFu) | ((u32)a[i] << 24);
    free(a);
    return WEBP_OK;
}

/* ======================================================================
 * the container (RFC 9649 section 2)
 * ====================================================================== */

/* The chunks ran out with no picture in them: cut short if the file is
   shorter than RIFF says, damaged if not. Never returned to a caller. */
#define WEBP_NO_PICTURE -100

/* One frame's chunks, from `d` to `d + n`: an optional ALPH, then the
   picture. On success *px holds its ARGB pixels. */
static int webp_frame(const u8 *d, int n, int *wp, int *hp, u32 **px) {
    const u8 *alph = 0;
    int alph_n = 0, at = 0;
    while (n - at >= 8) {
        const u8 *name = d + at;
        u32 size = webp_le32(d + at + 4);
        if (size > (u32)(n - at - 8)) return WEBP_TRUNCATED;
        const u8 *body = d + at + 8;
        if (png_is(name, "ALPH")) {
            if (!alph) { alph = body; alph_n = (int)size; }
        } else if (png_is(name, "VP8 ")) {
            int rc = vp8_decode(body, (int)size, wp, hp, px);
            if (rc == WEBP_OK && alph) {
                rc = webp_alpha(alph, alph_n, *wp, *hp, *px);
                if (rc != WEBP_OK) { free(*px); *px = 0; }
            }
            return rc;
        } else if (png_is(name, "VP8L")) {
            /* A lossless picture carries its own alpha; an ALPH beside it
               is ignored, as the specification allows. */
            return vp8l_decode(body, (int)size, 1, wp, hp, px);
        }
        at += 8 + (int)size + (int)(size & 1);
    }
    return WEBP_NO_PICTURE;
}

/* `bg` is what a transparent pixel is laid over, as for png_decode. */
static inline int webp_decode(const u8 *data, int n, picture *out, u32 bg) {
    out->w = out->h = 0;
    out->rgb = 0;
    static const char SIG[12] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P' };
    for (int i = 0; i < 12 && i < n; i++)
        if ((i < 4 || i >= 8) && data[i] != (u8)SIG[i]) return WEBP_NOT_WEBP;
    if (n < 12) return WEBP_TRUNCATED;

    /* The file ends where RIFF says, or where the bytes do if that is
       sooner; a chunk that needs more than is here is cut short. */
    u32 riff = webp_le32(data + 4);
    int cut = riff > (u32)(n - 8);
    int end = cut ? n : (int)riff + 8;

    int cw = 0, ch = 0, fx = 0, fy = 0, fw = 0, fh = 0;
    int rc;
    u32 *px = 0;
    if (end - 12 < 8) return cut ? WEBP_TRUNCATED : WEBP_BAD;
    const u8 *first = data + 12;
    if (png_is(first, "VP8 ") || png_is(first, "VP8L")) {
        rc = webp_frame(first, end - 12, &fw, &fh, &px);
        cw = fw;
        ch = fh;
    } else if (png_is(first, "VP8X")) {
        u32 size = webp_le32(first + 4);
        if (size > (u32)(end - 20)) return WEBP_TRUNCATED;
        if (size < 10) return WEBP_BAD;
        int flags = first[8];
        cw = (int)webp_le24(first + 12) + 1;
        ch = (int)webp_le24(first + 15) + 1;
        if (cw > WEBP_MAX_SIDE || ch > WEBP_MAX_SIDE || cw * ch > WEBP_MAX_PIXELS) return WEBP_TOO_BIG;
        int at = 12 + 8 + (int)size + (int)(size & 1);
        if (flags & 0x02) {
            /* An animation: the first ANMF is the first frame, drawn at
               its place on the canvas. ANIM and anything else before it
               are passed over. */
            WEBP_SAW(anim);
            rc = WEBP_NO_PICTURE;
            while (end - at >= 8) {
                u32 csize = webp_le32(data + at + 4);
                if (csize > (u32)(end - at - 8)) { rc = WEBP_TRUNCATED; break; }
                const u8 *body = data + at + 8;
                if (png_is(data + at, "ANMF")) {
                    if (csize < 16) { rc = WEBP_BAD; break; }
                    fx = (int)webp_le24(body) * 2;
                    fy = (int)webp_le24(body + 3) * 2;
                    int want_w = (int)webp_le24(body + 6) + 1, want_h = (int)webp_le24(body + 9) + 1;
                    if (fx + want_w > cw || fy + want_h > ch) { rc = WEBP_BAD; break; }
                    rc = webp_frame(body + 16, (int)csize - 16, &fw, &fh, &px);
                    if (rc == WEBP_OK && (fw != want_w || fh != want_h)) rc = WEBP_BAD;
                    break;
                }
                at += 8 + (int)csize + (int)(csize & 1);
            }
        } else {
            /* A still picture, which must be the canvas's size. */
            rc = webp_frame(data + at, end - at, &fw, &fh, &px);
            if (rc == WEBP_OK && (fw != cw || fh != ch)) rc = WEBP_BAD;
        }
    } else {
        return WEBP_BAD;
    }
    if (rc == WEBP_NO_PICTURE) rc = cut ? WEBP_TRUNCATED : WEBP_BAD;
    if (rc != WEBP_OK) {
        if (px) free(px);
        return rc;
    }

    u8 *rgb = (u8 *)malloc((u64)cw * ch * 3);
    if (!rgb) { free(px); return WEBP_TOO_BIG; }
    int bg_r = (int)((bg >> 16) & 0xFF), bg_g = (int)((bg >> 8) & 0xFF), bg_b = (int)(bg & 0xFF);
    for (int i = 0; i < cw * ch; i++) {
        rgb[i * 3] = (u8)bg_r;
        rgb[i * 3 + 1] = (u8)bg_g;
        rgb[i * 3 + 2] = (u8)bg_b;
    }
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++) {
            u32 p = px[y * fw + x];
            int a = (int)(p >> 24), r = (int)((p >> 16) & 0xFF), g = (int)((p >> 8) & 0xFF), b = (int)(p & 0xFF);
            u8 *o = rgb + ((fy + y) * cw + fx + x) * 3;
            if (a == 255) {
                o[0] = (u8)r; o[1] = (u8)g; o[2] = (u8)b;
            } else {
                o[0] = (u8)((r * a + bg_r * (255 - a)) / 255);
                o[1] = (u8)((g * a + bg_g * (255 - a)) / 255);
                o[2] = (u8)((b * a + bg_b * (255 - a)) / 255);
            }
        }
    free(px);
    out->w = cw;
    out->h = ch;
    out->rgb = rgb;
    return WEBP_OK;
}

static inline const char *webp_why(int rc) {
    switch (rc) {
    case WEBP_NOT_WEBP:     return "not a webp";
    case WEBP_TRUNCATED:    return "the picture stopped part way";
    case WEBP_UNSUPPORTED:  return "a vp8 frame that is not a key frame, which this does not read";
    case WEBP_TOO_BIG:      return "too big to hold";
    default:                return "it does not decode";
    }
}
