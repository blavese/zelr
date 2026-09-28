#pragma once
#include "zelr.h"
#include "alloc.h"
#include "png.h"

/* JPEG, from the specification, baseline sequential and progressive.
 *
 * The other half of the pictures on the web. A PNG is what a logo is; a JPEG
 * is what a photograph is, and a browser that reads one and not the other
 * shows half of most pages.
 *
 * It is a different kind of format from PNG in every way. PNG keeps the
 * pixels and compresses them losslessly; JPEG throws pixels away on purpose
 * and keeps a description of what the picture looked like. The shape of it:
 *
 *   the image is cut into blocks of eight by eight
 *   each block is turned into sixty four frequencies by a cosine transform
 *   each frequency is divided by a number from a table — this is the part
 *     that loses, and the table is chosen so it loses what the eye does not
 *     look at
 *   what is left is mostly zeroes, and those are run length coded and then
 *     Huffman coded
 *
 * so decoding is that backwards: Huffman, run lengths, multiply by the
 * table, inverse cosine transform, and the block is pixels again.
 *
 * Colour is kept as brightness and two colour differences rather than red,
 * green and blue, because the eye sees detail in brightness and not much in
 * colour — so the colour parts are usually stored at half resolution and
 * stretched back afterwards. That is what the sampling factors are for and
 * it is most of why the format is small.
 *
 * Progressive JPEG sends the same coefficients in several passes -- the
 * lowest frequency of every block first, then bands of the others, then the
 * low bits the first passes held back -- so that a picture can be shown blurry
 * and sharpened as it arrives. A large share of the photographs on the web are
 * sent that way. It is decoded into every block's coefficients, one scan at a
 * time, and turned into pixels once they have all arrived (jpg_progressive).
 *
 * What is not here: arithmetic coding, which almost nothing uses, and twelve
 * bit samples. Each is refused by name.
 */

#define JPG_OK           0
#define JPG_NOT_JPEG    -1
#define JPG_TRUNCATED   -2
#define JPG_PROGRESSIVE -3
#define JPG_UNSUPPORTED -4
#define JPG_TOO_BIG     -5
#define JPG_BAD         -6

#define JPG_MAX_SIDE 8192
#define JPG_MAX_PIXELS (24 * 1024 * 1024)
#define JPG_COMPONENTS 4

/* The order the sixty four frequencies are written in: out from the corner
   in a zigzag, so that the ones most likely to be zero end up together at
   the end where a run length can swallow them all. */
static const u8 JPG_ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct {
    u8  bits[17];            /* how many codes of each length, 1..16 */
    u8  vals[256];
    int mincode[17], maxcode[18], valptr[17];
    int present;
} jhuff;

typedef struct {
    int id, h, v, tq;        /* sampling factors and quantisation table */
    int td, ta;              /* the Huffman tables this scan uses */
    int dc;                  /* the running DC predictor */
    u8 *pix;                 /* its own plane, blocks * 8 across */
    int bw, bh;              /* in blocks */
    short *coef;             /* progressive: every block's 64, in block order */
} jcomp;

typedef struct {
    const u8 *in;
    int n, at;

    u32 bits;                /* the entropy coded bit buffer */
    int nbits;
    int marker_hit;          /* a marker was reached while reading bits */

    u16 quant[4][64];
    jhuff hdc[4], hac[4];

    int w, h, ncomp;
    jcomp comp[JPG_COMPONENTS];
    int hmax, vmax;
    int restart;             /* MCUs between restart markers, or 0 */

    int progressive;         /* the frame was SOF2 */
    int ss, se, ah, al;      /* this scan: the band, and the bits held back */
    int eobrun;              /* blocks left with nothing more in this band */

    int err;
} jstate;

/* --- bits ----------------------------------------------------------------
 *
 * Most significant first, the opposite way round from deflate, and with one
 * wrinkle: a 0xFF byte in the entropy coded data is written as 0xFF 0x00, so
 * that a real marker can always be found by scanning for 0xFF followed by
 * something else. */
static inline int jpg_bit(jstate *s) {
    if (s->nbits == 0) {
        if (s->at >= s->n) { s->err = JPG_TRUNCATED; return 0; }
        int b = s->in[s->at++];
        if (b == 0xFF) {
            int next = s->at < s->n ? s->in[s->at] : 0xD9;
            if (next == 0x00) s->at++;          /* a stuffed 0xFF */
            else { s->marker_hit = 1; b = 0; s->at--; }
        }
        s->bits = (u32)b;
        s->nbits = 8;
    }
    s->nbits--;
    return (int)((s->bits >> s->nbits) & 1);
}

static inline int jpg_bits(jstate *s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | jpg_bit(s);
    return v;
}

/* A coefficient is written as a size and then that many bits. The bits are
   the value itself when the top one is set, and the value offset the other
   way when it is not, which is how negative numbers are written without a
   sign bit. */
static inline int jpg_extend(int v, int size) {
    if (size == 0) return 0;
    return v < (1 << (size - 1)) ? v - (1 << size) + 1 : v;
}

/* --- Huffman --------------------------------------------------------------
 *
 * Canonical again, but described differently from deflate: a count of codes
 * per length and the values in order, and decoding walks lengths comparing
 * against the largest code of each. */
static inline void jpg_huff_build(jhuff *h) {
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->valptr[len] = k;
        h->mincode[len] = code;
        code += h->bits[len];
        k += h->bits[len];
        h->maxcode[len] = code - 1;
        code <<= 1;
        if (h->bits[len] == 0) h->maxcode[len] = -1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->present = 1;
}

static inline int jpg_decode(jstate *s, const jhuff *h) {
    if (!h->present) { s->err = JPG_BAD; return 0; }

    int code = jpg_bit(s);
    for (int len = 1; len <= 16; len++) {
        if (h->maxcode[len] >= 0 && code <= h->maxcode[len])
            return h->vals[h->valptr[len] + code - h->mincode[len]];
        code = (code << 1) | jpg_bit(s);
        if (s->err) return 0;
    }
    s->err = JPG_BAD;
    return 0;
}

/* --- the inverse transform -----------------------------------------------
 *
 * Written as what it is: two passes of an eight point inverse cosine
 * transform, rows and then columns, against a table of cosines worked out
 * once. There are much faster arrangements of this — the whole thing can be
 * done in a few dozen additions per row — and they are faster by rearranging
 * the arithmetic until it no longer looks like the formula it came from.
 * This is the formula. It is the part of a photograph that takes the time,
 * and if that ever matters more than being able to read it, the faster one
 * can be written against this one's output.
 */
static float JPG_COS[8][8];
static int   JPG_COS_READY;

static inline void jpg_cos_init(void) {
    if (JPG_COS_READY) return;

    /* cos((2x + 1) u pi / 16), and the scale each row carries. Worked out
       from a series rather than taken from a table of constants: there is
       no cosine in this system to ask. */
    for (int u = 0; u < 8; u++) {
        for (int x = 0; x < 8; x++) {
            double a = (2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0;

            /* Brought into one turn before anything else.
             *
             * This angle reaches twenty radians at the top of the table, and
             * the series below is only quick near zero: at twenty it is not
             * slow, it is wrong, because the terms grow to ten million
             * before they start shrinking and what is left after they cancel
             * is nothing like a cosine. The table came out as noise for
             * every frequency above the first two, which a flat colour does
             * not use and a gradient does — so flat pictures decoded
             * perfectly and anything with detail in it fell apart. */
            const double TWO_PI = 6.283185307179586477;
            while (a > 3.14159265358979323846) a -= TWO_PI;
            while (a < -3.14159265358979323846) a += TWO_PI;

            /* cos by its series, which converges quickly for a below pi. */
            double t = 1.0, sum = 1.0;
            for (int k = 1; k < 12; k++) {
                t *= -a * a / ((2 * k - 1) * (2 * k));
                sum += t;
            }
            double scale = u == 0 ? 0.353553390593273762 : 0.5;  /* 1/sqrt8, 1/2 */
            JPG_COS[u][x] = (float)(sum * scale);
        }
    }
    JPG_COS_READY = 1;
}

/* Still the formula, with the terms that are nothing left out.
 *
 * After quantisation most of a block's sixty four coefficients are zero --
 * a flat patch of sky is one number and sixty three zeroes -- and the
 * formula multiplied every one of them by its cosine and added the zero in,
 * a thousand multiplications a block for a handful that mattered. Adding a
 * zero, of either sign, to a sum leaves the sum as it was, and the terms that
 * are left are added in the same order as before, so every byte that comes
 * out is the byte that came out of the whole formula. A row with no
 * coefficients at all is zero across and is not walked in the second pass. */
static inline void jpg_idct(const int *in, u8 *out, int stride) {
    float tmp[64];
    int live[8];

    /* rows */
    for (int y = 0; y < 8; y++) {
        const int *row = in + y * 8;
        live[y] = row[0] | row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7];
        if (!live[y]) {
            for (int x = 0; x < 8; x++) tmp[y * 8 + x] = 0.0f;
            continue;
        }
        for (int x = 0; x < 8; x++) {
            float sum = 0;
            for (int u = 0; u < 8; u++)
                if (row[u]) sum += JPG_COS[u][x] * (float)row[u];
            tmp[y * 8 + x] = sum;
        }
    }

    /* columns, and out as bytes with the level shift back on */
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            float sum = 0;
            for (int v = 0; v < 8; v++)
                if (live[v]) sum += JPG_COS[v][y] * tmp[v * 8 + x];

            int val = (int)(sum + 128.5f);
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            out[y * stride + x] = (u8)val;
        }
    }
}

/* --- one block ------------------------------------------------------------ */
static inline void jpg_block(jstate *s, jcomp *c, u8 *dst, int stride) {
    int coef[64];
    for (int i = 0; i < 64; i++) coef[i] = 0;

    const u16 *q = s->quant[c->tq & 3];

    /* The DC is written as a difference from the last block of the same
       component, which is why a decoder cannot start in the middle. */
    int t = jpg_decode(s, &s->hdc[c->td & 3]);
    if (s->err) return;
    int diff = t ? jpg_extend(jpg_bits(s, t), t) : 0;
    c->dc += diff;
    coef[0] = c->dc * q[0];

    /* Then the rest, as runs of zeroes and a value, until either sixty four
       have been written or the end of block symbol arrives. */
    for (int k = 1; k < 64;) {
        int rs = jpg_decode(s, &s->hac[c->ta & 3]);
        if (s->err) return;

        int run = rs >> 4, size = rs & 15;
        if (size == 0) {
            if (run != 15) break;               /* end of block */
            k += 16;                            /* sixteen zeroes */
            continue;
        }
        k += run;
        if (k > 63) break;
        coef[JPG_ZIGZAG[k]] = jpg_extend(jpg_bits(s, size), size) * q[k];
        k++;
    }

    jpg_idct(coef, dst, stride);
}

/* --- progressive ---------------------------------------------------------------
 *
 * Annex G. A scan carries either the DC of its components or one band of one
 * component's AC, and either the first pass at those (with al low bits held
 * back) or a refinement that sends the next bit down (ah is the bit it was
 * sent to before). Everything goes into the block's coefficients as read;
 * they are multiplied out and transformed at the end, when all of it is in. */

/* The first pass at the DC: the difference from the last block of the
   component, as in a baseline block, shifted up past the bits held back. */
static inline void jpg_dc_first(jstate *s, jcomp *c, short *blk) {
    int t = jpg_decode(s, &s->hdc[c->td & 3]);
    if (s->err) return;
    int diff = t ? jpg_extend(jpg_bits(s, t), t) : 0;
    c->dc += diff;
    blk[0] = (short)(c->dc * (1 << s->al));
}

/* A refinement of the DC is one bit, sent as it is. */
static inline void jpg_dc_refine(jstate *s, short *blk) {
    if (jpg_bit(s)) blk[0] = (short)(blk[0] | (1 << s->al));
}

/* The first pass at a band of AC: runs and values as in a baseline block,
   and an end of band that can say how many of the following blocks have
   nothing in this band either (eobrun), which is most of what makes a
   progressive file small. */
static inline void jpg_ac_first(jstate *s, jcomp *c, short *blk) {
    if (s->eobrun) { s->eobrun--; return; }
    for (int k = s->ss; k <= s->se; k++) {
        int rs = jpg_decode(s, &s->hac[c->ta & 3]);
        if (s->err) return;
        int r = rs >> 4, size = rs & 15;
        if (size == 0) {
            if (r < 15) {                       /* this block and eobrun more */
                s->eobrun = (1 << r) - 1;
                if (r) s->eobrun += jpg_bits(s, r);
                return;
            }
            k += 15;                            /* sixteen zeroes */
            continue;
        }
        k += r;
        if (k > 63) { s->err = JPG_BAD; return; }
        blk[JPG_ZIGZAG[k]] = (short)(jpg_extend(jpg_bits(s, size), size) * (1 << s->al));
    }
}

/* One more bit of every coefficient in a band. A coefficient that was not
 * zero gets its bit as it is passed (a correction bit, raising it by one step
 * away from zero if set); one that was zero and becomes one step from zero
 * is written as a run of zero coefficients to skip -- counting only those
 * still zero -- and its sign. The run of blocks with nothing new in them is
 * the same eobrun as the first pass, and those blocks still take correction
 * bits for what they had. */
static inline void jpg_refine_one(jstate *s, short *co, int p1) {
    if (jpg_bit(s) && (*co & p1) == 0)
        *co = (short)(*co >= 0 ? *co + p1 : *co - p1);
}

static inline void jpg_ac_refine(jstate *s, jcomp *c, short *blk) {
    int p1 = 1 << s->al;
    int k = s->ss;
    if (!s->eobrun) {
        for (; k <= s->se; k++) {
            int rs = jpg_decode(s, &s->hac[c->ta & 3]);
            if (s->err) return;
            int r = rs >> 4, size = rs & 15, value = 0;
            if (size) {
                if (size != 1) { s->err = JPG_BAD; return; }
                value = jpg_bit(s) ? p1 : -p1;
            } else if (r != 15) {
                s->eobrun = 1 << r;
                if (r) s->eobrun += jpg_bits(s, r);
                break;                          /* the rest of the band, below */
            }
            /* Past r coefficients that are still zero, refining the others on
               the way, to where the new one goes. */
            for (; k <= s->se; k++) {
                short *co = &blk[JPG_ZIGZAG[k]];
                if (*co) {
                    jpg_refine_one(s, co, p1);
                } else {
                    if (r == 0) break;
                    r--;
                }
                if (s->err) return;
            }
            if (value && k <= s->se) blk[JPG_ZIGZAG[k]] = (short)value;
        }
    }
    if (s->eobrun) {
        for (; k <= s->se; k++) {
            short *co = &blk[JPG_ZIGZAG[k]];
            if (*co) jpg_refine_one(s, co, p1);
            if (s->err) return;
        }
        s->eobrun--;
    }
}

/* One scan. With several components it is MCU by MCU, as a baseline one is;
   with one, which every band of AC is, it is that component's own blocks
   row by row -- only those that cover the picture, not the ones an MCU
   rounds out to, which a scan of one never sends. */
static inline int jpg_prog_scan(jstate *s, const int *which, int ns) {
    int mcux = (s->w + s->hmax * 8 - 1) / (s->hmax * 8);
    int mcuy = (s->h + s->vmax * 8 - 1) / (s->vmax * 8);
    int units_x = mcux, units_y = mcuy;
    if (ns == 1) {
        jcomp *c = &s->comp[which[0]];
        int cw = (s->w * c->h + s->hmax - 1) / s->hmax;
        int ch = (s->h * c->v + s->vmax - 1) / s->vmax;
        units_x = (cw + 7) / 8;
        units_y = (ch + 7) / 8;
    }

    int since_restart = 0;
    s->eobrun = 0;
    for (int i = 0; i < s->ncomp; i++) s->comp[i].dc = 0;

    for (int uy = 0; uy < units_y; uy++) {
        for (int ux = 0; ux < units_x; ux++) {
            if (s->restart && since_restart == s->restart) {
                s->nbits = 0;
                s->marker_hit = 0;
                while (s->at + 1 < s->n) {
                    if (s->in[s->at] == 0xFF && s->in[s->at + 1] >= 0xD0
                        && s->in[s->at + 1] <= 0xD7) { s->at += 2; break; }
                    s->at++;
                }
                for (int i = 0; i < s->ncomp; i++) s->comp[i].dc = 0;
                s->eobrun = 0;
                since_restart = 0;
            }

            for (int w = 0; w < ns; w++) {
                jcomp *c = &s->comp[which[w]];
                int bh = ns == 1 ? 1 : c->v, bwn = ns == 1 ? 1 : c->h;
                for (int by = 0; by < bh; by++) {
                    for (int bx = 0; bx < bwn; bx++) {
                        int col = ns == 1 ? ux : ux * c->h + bx;
                        int row = ns == 1 ? uy : uy * c->v + by;
                        short *blk = c->coef + ((u32)row * (u32)c->bw + (u32)col) * 64u;
                        if (s->ss == 0) {
                            if (s->ah == 0) jpg_dc_first(s, c, blk);
                            else            jpg_dc_refine(s, blk);
                        } else {
                            if (s->ah == 0) jpg_ac_first(s, c, blk);
                            else            jpg_ac_refine(s, c, blk);
                        }
                        if (s->err) return s->err;
                    }
                }
            }
            since_restart++;
        }
    }
    return JPG_OK;
}

/* And once it is all in: each block multiplied by its table, in the order
   the table is written in, and transformed into its plane. */
static inline void jpg_progressive(jstate *s) {
    for (int i = 0; i < s->ncomp; i++) {
        jcomp *c = &s->comp[i];
        const u16 *q = s->quant[c->tq & 3];
        for (int by = 0; by < c->bh; by++)
            for (int bx = 0; bx < c->bw; bx++) {
                const short *blk = c->coef + ((u32)by * (u32)c->bw + (u32)bx) * 64u;
                int coef[64];
                for (int k = 0; k < 64; k++)
                    coef[JPG_ZIGZAG[k]] = blk[JPG_ZIGZAG[k]] * q[k];
                jpg_idct(coef, c->pix + by * 8 * (c->bw * 8) + bx * 8, c->bw * 8);
            }
    }
}

/* --- markers -------------------------------------------------------------- */
static inline int jpg_u16(const u8 *p) { return (p[0] << 8) | p[1]; }

static inline int jpg_scan(jstate *s) {
    /* Every MCU, in order: for each component, its sampling factors say how
       many blocks of it belong to one MCU. */
    int mcux = (s->w + s->hmax * 8 - 1) / (s->hmax * 8);
    int mcuy = (s->h + s->vmax * 8 - 1) / (s->vmax * 8);

    int since_restart = 0;

    for (int my = 0; my < mcuy; my++) {
        for (int mx = 0; mx < mcux; mx++) {
            if (s->restart && since_restart == s->restart) {
                /* A restart marker: the bit buffer is thrown away, the DC
                   predictors go back to zero and decoding carries on. It is
                   what lets a damaged file recover rather than turn to
                   noise from the first mistake onwards. */
                s->nbits = 0;
                s->marker_hit = 0;
                while (s->at + 1 < s->n) {
                    if (s->in[s->at] == 0xFF && s->in[s->at + 1] >= 0xD0
                        && s->in[s->at + 1] <= 0xD7) { s->at += 2; break; }
                    s->at++;
                }
                for (int i = 0; i < s->ncomp; i++) s->comp[i].dc = 0;
                since_restart = 0;
            }

            for (int i = 0; i < s->ncomp; i++) {
                jcomp *c = &s->comp[i];
                for (int by = 0; by < c->v; by++) {
                    for (int bx = 0; bx < c->h; bx++) {
                        int px = (mx * c->h + bx) * 8;
                        int py = (my * c->v + by) * 8;
                        if (px >= c->bw * 8 || py >= c->bh * 8) {
                            /* Outside the plane, but still in the stream and
                               still has to be read past. */
                            u8 waste[64];
                            jpg_block(s, c, waste, 8);
                        } else {
                            jpg_block(s, c, c->pix + py * (c->bw * 8) + px,
                                      c->bw * 8);
                        }
                        if (s->err) return s->err;
                    }
                }
            }
            since_restart++;
        }
    }
    return JPG_OK;
}

/* --- the way in ----------------------------------------------------------- */
static inline int jpeg_decode(const u8 *data, int n, picture *out) {
    out->w = out->h = 0;
    out->rgb = 0;

    if (n < 4) return JPG_TRUNCATED;
    if (data[0] != 0xFF || data[1] != 0xD8) return JPG_NOT_JPEG;

    jpg_cos_init();

    jstate s;
    for (int i = 0; i < 4; i++) {
        s.hdc[i].present = 0;
        s.hac[i].present = 0;
        for (int k = 0; k < 64; k++) s.quant[i][k] = 1;
    }
    for (int i = 0; i < JPG_COMPONENTS; i++) { s.comp[i].pix = 0; s.comp[i].coef = 0; }
    s.in = data; s.n = n; s.at = 2;
    s.bits = 0; s.nbits = 0; s.marker_hit = 0;
    s.w = s.h = s.ncomp = 0;
    s.hmax = s.vmax = 1;
    s.restart = 0;
    s.progressive = 0;
    s.eobrun = 0;
    s.err = JPG_OK;

    int rc = JPG_BAD;
    int scanned = 0;

    while (s.at + 3 < n) {
        if (data[s.at] != 0xFF) { s.at++; continue; }
        int marker = data[s.at + 1];
        s.at += 2;

        if (marker == 0xD8 || marker == 0x01
            || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (marker == 0xD9) break;                       /* end of image */

        if (s.at + 1 >= n) { rc = JPG_TRUNCATED; goto done; }
        int len = jpg_u16(data + s.at);
        if (len < 2 || s.at + len > n) { rc = JPG_TRUNCATED; goto done; }
        const u8 *body = data + s.at + 2;
        int blen = len - 2;

        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            if (s.w) { rc = JPG_BAD; goto done; }        /* a second frame */
            s.progressive = marker == 0xC2;
            if (blen < 6) { rc = JPG_BAD; goto done; }
            if (body[0] != 8) { rc = JPG_UNSUPPORTED; goto done; }
            s.h = jpg_u16(body + 1);
            s.w = jpg_u16(body + 3);
            s.ncomp = body[5];

            if (s.w <= 0 || s.h <= 0 || s.w > JPG_MAX_SIDE || s.h > JPG_MAX_SIDE
                || s.w * s.h > JPG_MAX_PIXELS) { rc = JPG_TOO_BIG; goto done; }
            if (s.ncomp != 1 && s.ncomp != 3) { rc = JPG_UNSUPPORTED; goto done; }
            if (blen < 6 + s.ncomp * 3) { rc = JPG_BAD; goto done; }

            for (int i = 0; i < s.ncomp; i++) {
                const u8 *p = body + 6 + i * 3;
                s.comp[i].id = p[0];
                s.comp[i].h = p[1] >> 4;
                s.comp[i].v = p[1] & 15;
                s.comp[i].tq = p[2];
                s.comp[i].dc = 0;
                if (s.comp[i].h < 1 || s.comp[i].h > 4
                    || s.comp[i].v < 1 || s.comp[i].v > 4) {
                    rc = JPG_BAD; goto done;
                }
                if (s.comp[i].h > s.hmax) s.hmax = s.comp[i].h;
                if (s.comp[i].v > s.vmax) s.vmax = s.comp[i].v;
            }

            /* A plane each, rounded out to whole blocks of an MCU. */
            int mcux = (s.w + s.hmax * 8 - 1) / (s.hmax * 8);
            int mcuy = (s.h + s.vmax * 8 - 1) / (s.vmax * 8);
            for (int i = 0; i < s.ncomp; i++) {
                jcomp *c = &s.comp[i];
                c->bw = mcux * c->h;
                c->bh = mcuy * c->v;
                c->pix = (u8 *)malloc((u32)(c->bw * 8 * c->bh * 8));
                if (!c->pix) { rc = JPG_TOO_BIG; goto done; }
                for (int k = 0; k < c->bw * 8 * c->bh * 8; k++) c->pix[k] = 128;
                if (s.progressive) {
                    u32 n64 = (u32)c->bw * (u32)c->bh * 64u;
                    c->coef = (short *)malloc(n64 * sizeof(short));
                    if (!c->coef) { rc = JPG_TOO_BIG; goto done; }
                    for (u32 k = 0; k < n64; k++) c->coef[k] = 0;
                }
            }
        } else if (marker == 0xC9 || marker == 0xCA || marker == 0xCB) {
            rc = JPG_UNSUPPORTED; goto done;             /* arithmetic coded */
        } else if (marker == 0xC4) {                     /* Huffman tables */
            int at = 0;
            while (at + 17 <= blen) {
                int tc = body[at] >> 4, th = body[at] & 15;
                if (th > 3) { rc = JPG_BAD; goto done; }
                jhuff *h = tc ? &s.hac[th] : &s.hdc[th];

                int total = 0;
                h->bits[0] = 0;
                for (int i = 1; i <= 16; i++) {
                    h->bits[i] = body[at + i];
                    total += h->bits[i];
                }
                if (total > 256 || at + 17 + total > blen) { rc = JPG_BAD; goto done; }
                for (int i = 0; i < total; i++) h->vals[i] = body[at + 17 + i];
                jpg_huff_build(h);
                at += 17 + total;
            }
        } else if (marker == 0xDB) {                     /* quantisation */
            int at = 0;
            while (at < blen) {
                int pq = body[at] >> 4, tq = body[at] & 15;
                if (tq > 3) { rc = JPG_BAD; goto done; }
                at++;
                for (int i = 0; i < 64; i++) {
                    if (pq) {
                        if (at + 1 >= blen) { rc = JPG_BAD; goto done; }
                        s.quant[tq][i] = (u16)jpg_u16(body + at);
                        at += 2;
                    } else {
                        if (at >= blen) { rc = JPG_BAD; goto done; }
                        s.quant[tq][i] = body[at++];
                    }
                }
            }
        } else if (marker == 0xDD) {                     /* restart interval */
            if (blen >= 2) s.restart = jpg_u16(body);
        } else if (marker == 0xDA) {                     /* the scan */
            if (!s.w || !s.ncomp) { rc = JPG_BAD; goto done; }
            if (blen < 1) { rc = JPG_BAD; goto done; }

            int ns = body[0];
            if (ns < 1 || ns > s.ncomp || blen < 1 + ns * 2 + 3) { rc = JPG_BAD; goto done; }
            if (!s.progressive && ns != s.ncomp) { rc = JPG_UNSUPPORTED; goto done; }
            int which[JPG_COMPONENTS];
            for (int i = 0; i < ns; i++) {
                int id = body[1 + i * 2];
                int tt = body[2 + i * 2];
                which[i] = -1;
                for (int k = 0; k < s.ncomp; k++)
                    if (s.comp[k].id == id) {
                        s.comp[k].td = tt >> 4;
                        s.comp[k].ta = tt & 15;
                        which[i] = k;
                    }
                if (which[i] < 0) { rc = JPG_BAD; goto done; }
            }

            s.at += len;
            s.nbits = 0;
            s.marker_hit = 0;

            if (!s.progressive) {
                rc = jpg_scan(&s);
                scanned = 1;
                goto done;
            }

            /* A band of AC is of one component, a DC scan goes no further
               than the DC, and a refinement is one bit below the last. */
            const u8 *sp = body + 1 + ns * 2;
            s.ss = sp[0]; s.se = sp[1]; s.ah = sp[2] >> 4; s.al = sp[2] & 15;
            if ((s.ss == 0 ? s.se != 0 : (s.se < s.ss || s.se > 63 || ns != 1))
                || s.al > 13 || (s.ah && s.ah != s.al + 1)) { rc = JPG_BAD; goto done; }

            rc = jpg_prog_scan(&s, which, ns);
            if (rc == JPG_TRUNCATED && scanned) break;   /* show what came */
            if (rc != JPG_OK) goto done;
            scanned = 1;
            continue;
        }

        s.at += len;
    }

    /* The end of the file, or of what arrived of it: a progressive picture
       is shown from the scans that came. */
    if (s.progressive && scanned) rc = JPG_OK;

done:
    if (!scanned || rc != JPG_OK) {
        if (rc == JPG_OK) rc = JPG_TRUNCATED;
        for (int i = 0; i < JPG_COMPONENTS; i++) {
            if (s.comp[i].pix) free(s.comp[i].pix);
            if (s.comp[i].coef) free(s.comp[i].coef);
        }
        return rc;
    }
    if (s.progressive) jpg_progressive(&s);
    for (int i = 0; i < JPG_COMPONENTS; i++)
        if (s.comp[i].coef) { free(s.comp[i].coef); s.comp[i].coef = 0; }

    /* --- and out as pixels ------------------------------------------------
     *
     * The colour planes are usually smaller than the brightness one, so each
     * is stretched by repeating — which is what "nearest" means here and is
     * what the format expects: the numbers were averaged on the way in. */
    u8 *rgb = (u8 *)malloc((u32)(s.w * s.h * 3));
    if (!rgb) {
        for (int i = 0; i < JPG_COMPONENTS; i++)
            if (s.comp[i].pix) free(s.comp[i].pix);
        return JPG_TOO_BIG;
    }

    /* Where along its row each plane's sample for a column is. It was worked
       out with a division per plane per pixel, six divisions a pixel between
       the rows and the columns; the columns are the same on every row, so
       they are worked out once, and the rows once a row. */
    int *xs = (int *)malloc((u32)(s.w * 3) * sizeof(int));
    if (!xs) {
        free(rgb);
        for (int i = 0; i < JPG_COMPONENTS; i++)
            if (s.comp[i].pix) free(s.comp[i].pix);
        return JPG_TOO_BIG;
    }
    if (s.ncomp != 1)
        for (int c = 0; c < 3; c++)
            for (int x = 0; x < s.w; x++)
                xs[c * s.w + x] = x * s.comp[c].h / s.hmax;

    for (int y = 0; y < s.h; y++) {
        const u8 *ry = 0, *rb = 0, *rr = 0;
        if (s.ncomp != 1) {
            jcomp *cy = &s.comp[0], *cb = &s.comp[1], *cr = &s.comp[2];
            ry = cy->pix + (y * cy->v / s.vmax) * (cy->bw * 8);
            rb = cb->pix + (y * cb->v / s.vmax) * (cb->bw * 8);
            rr = cr->pix + (y * cr->v / s.vmax) * (cr->bw * 8);
        }
        for (int x = 0; x < s.w; x++) {
            int r, g, b;

            if (s.ncomp == 1) {
                jcomp *c = &s.comp[0];
                r = g = b = c->pix[y * (c->bw * 8) + x];
            } else {
                int yy = ry[xs[x]];
                int u = rb[xs[s.w + x]] - 128;
                int v = rr[xs[2 * s.w + x]] - 128;

                /* The conversion the format specifies, in whole numbers
                   scaled by 1024 so no rounding creeps in per pixel. */
                r = yy + (1436 * v >> 10);
                g = yy - (352 * u >> 10) - (731 * v >> 10);
                b = yy + (1815 * u >> 10);
            }

            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;

            u8 *px = rgb + (y * s.w + x) * 3;
            px[0] = (u8)r; px[1] = (u8)g; px[2] = (u8)b;
        }
    }
    free(xs);

    for (int i = 0; i < JPG_COMPONENTS; i++)
        if (s.comp[i].pix) free(s.comp[i].pix);

    out->w = s.w;
    out->h = s.h;
    out->rgb = rgb;
    return JPG_OK;
}

static inline const char *jpeg_why(int rc) {
    switch (rc) {
    case JPG_NOT_JPEG:    return "not a jpeg";
    case JPG_TRUNCATED:   return "the picture stopped part way";
    case JPG_PROGRESSIVE: return "progressive, which is a different decoder";
    case JPG_UNSUPPORTED: return "a kind of jpeg this does not read";
    case JPG_TOO_BIG:     return "too big to hold";
    default:              return "it does not decode";
    }
}
