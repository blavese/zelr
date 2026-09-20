#pragma once
#include "zelr.h"
#include "alloc.h"
#include "inflate.h"

/* PNG, from the specification.
 *
 * A picture on the web is one of four things: a PNG, a JPEG, a GIF or an
 * SVG. This is the first of them, and it is the one worth doing first
 * because it is the one everything that is not a photograph uses — logos,
 * icons, buttons, screenshots, diagrams.
 *
 * The shape of the format is: eight bytes saying it is a PNG, then chunks,
 * each with a length, a four letter name, its bytes and a checksum. The ones
 * that matter here are IHDR, which says how big it is and how the pixels are
 * written; PLTE, which is a table of colours for the kind that uses one;
 * IDAT, which is the pixels, deflated, and which may be split across any
 * number of chunks that have to be joined before any of it can be read; and
 * IEND, which ends it.
 *
 * Inside, each row is preceded by a byte saying which of five filters was
 * used on it. The filters are all "this byte minus one of its neighbours",
 * which makes a run of similar pixels into a run of small numbers and is
 * most of why the format compresses at all. Undoing them has to happen in
 * order, because every row after the first is relative to the one above it.
 *
 * What is not here: interlaced images, which arrive in seven passes and are
 * rare enough on the modern web to refuse rather than half draw; and bit
 * depth 16, which is four times the memory for a difference nothing on this
 * screen could show. Both are refused by name rather than decoded wrongly.
 */

#define PNG_OK          0
#define PNG_NOT_PNG    -1
#define PNG_TRUNCATED  -2
#define PNG_UNSUPPORTED -3   /* interlaced, 16 bit, or a kind not listed */
#define PNG_TOO_BIG    -4
#define PNG_BAD        -5

/* A picture, once it is pixels. Always three bytes per pixel: whatever the
   file said, this is what the screen wants, and carrying an alpha channel
   through the layout for the few images that have one would mean every
   drawing path had to know about it. Transparent pixels are composited onto
   the page's background here instead. */
typedef struct {
    int w, h;
    u8 *rgb;             /* w * h * 3, from malloc */
} picture;

/* Nothing this big is a page element; it is a mistake or an attack. */
#define PNG_MAX_SIDE 4096
#define PNG_MAX_PIXELS (16 * 1024 * 1024)

static inline u32 png_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static inline int png_is(const u8 *p, const char *name) {
    return p[0] == (u8)name[0] && p[1] == (u8)name[1]
        && p[2] == (u8)name[2] && p[3] == (u8)name[3];
}

/* --- the filters ----------------------------------------------------------
 *
 * Each is undone in place, left to right, because a byte's neighbour to the
 * left has already been undone by the time it is needed. `bpp` is the
 * distance to the pixel on the left in bytes; for anything under eight bits
 * a pixel the distance is one, which the specification says plainly and
 * which is easy to get wrong by computing it. */
static inline int png_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static inline void png_unfilter(u8 *row, const u8 *prev, int len, int bpp,
                                int filter) {
    switch (filter) {
    case 0:
        break;
    case 1:                                   /* the pixel to the left */
        for (int i = bpp; i < len; i++) row[i] = (u8)(row[i] + row[i - bpp]);
        break;
    case 2:                                   /* the pixel above */
        if (prev) for (int i = 0; i < len; i++) row[i] = (u8)(row[i] + prev[i]);
        break;
    case 3:                                   /* the average of both */
        for (int i = 0; i < len; i++) {
            int left = i >= bpp ? row[i - bpp] : 0;
            int up = prev ? prev[i] : 0;
            row[i] = (u8)(row[i] + ((left + up) >> 1));
        }
        break;
    case 4:                                   /* whichever of three is nearest */
        for (int i = 0; i < len; i++) {
            int left = i >= bpp ? row[i - bpp] : 0;
            int up = prev ? prev[i] : 0;
            int upleft = (prev && i >= bpp) ? prev[i - bpp] : 0;
            row[i] = (u8)(row[i] + png_paeth(left, up, upleft));
        }
        break;
    default:
        break;
    }
}

/* --- decoding -------------------------------------------------------------
 *
 * `bg` is what a transparent pixel is laid over, because the result has no
 * alpha of its own. The page's background is the right answer and the caller
 * knows it; this cannot.
 */
static inline int png_decode(const u8 *data, int n, picture *out, u32 bg) {
    out->w = out->h = 0;
    out->rgb = 0;

    static const u8 SIG[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (n < 8) return PNG_TRUNCATED;
    for (int i = 0; i < 8; i++) if (data[i] != SIG[i]) return PNG_NOT_PNG;

    int w = 0, h = 0, depth = 0, colour = 0, interlace = 0;
    u8 palette[256 * 3];
    int palette_n = 0;
    u8 trans[256];
    int trans_n = 0;
    for (int i = 0; i < 256; i++) trans[i] = 255;

    /* The compressed pixels, joined from however many IDAT chunks there
       were. Sized from the file: deflate cannot expand, so the whole of the
       input is always enough for the part of it that is IDAT. */
    u8 *zdata = (u8 *)malloc((u32)n);
    if (!zdata) return PNG_TOO_BIG;
    int zlen = 0;

    int at = 8;
    int rc = PNG_BAD;
    int seen_end = 0;

    while (at + 8 <= n) {
        u32 clen = png_be32(data + at);
        const u8 *name = data + at + 4;
        const u8 *body = data + at + 8;
        if (clen > (u32)(n - at - 12)) { rc = PNG_TRUNCATED; goto done; }

        if (png_is(name, "IHDR")) {
            if (clen < 13) { rc = PNG_BAD; goto done; }
            w = (int)png_be32(body);
            h = (int)png_be32(body + 4);
            depth = body[8];
            colour = body[9];
            interlace = body[12];

            if (w <= 0 || h <= 0 || w > PNG_MAX_SIDE || h > PNG_MAX_SIDE
                || w * h > PNG_MAX_PIXELS) { rc = PNG_TOO_BIG; goto done; }
            if (interlace) { rc = PNG_UNSUPPORTED; goto done; }
            if (depth == 16) { rc = PNG_UNSUPPORTED; goto done; }
            if (depth != 1 && depth != 2 && depth != 4 && depth != 8) {
                rc = PNG_BAD; goto done;
            }
            if (colour != 0 && colour != 2 && colour != 3 && colour != 4
                && colour != 6) { rc = PNG_BAD; goto done; }
            /* Only a palette is written in fewer than eight bits in
               practice, and greyscale at one bit is the same shape. */
            if (depth != 8 && colour != 3 && colour != 0) {
                rc = PNG_UNSUPPORTED; goto done;
            }
        } else if (png_is(name, "PLTE")) {
            palette_n = (int)(clen / 3);
            if (palette_n > 256) palette_n = 256;
            for (int i = 0; i < palette_n * 3; i++) palette[i] = body[i];
        } else if (png_is(name, "tRNS")) {
            trans_n = (int)clen > 256 ? 256 : (int)clen;
            for (int i = 0; i < trans_n; i++) trans[i] = body[i];
        } else if (png_is(name, "IDAT")) {
            for (u32 i = 0; i < clen && zlen < n; i++) zdata[zlen++] = body[i];
        } else if (png_is(name, "IEND")) {
            seen_end = 1;
        }

        at += 12 + (int)clen;                   /* length, name, body, crc */
        if (seen_end) break;
    }

    if (!w || !h) { rc = PNG_BAD; goto done; }
    if (!zlen) { rc = PNG_TRUNCATED; goto done; }

    int channels = colour == 0 ? 1 : colour == 2 ? 3
                 : colour == 3 ? 1 : colour == 4 ? 2 : 4;

    /* One byte of filter in front of each row, and rows are whole bytes. */
    int bits = w * channels * depth;
    int stride = (bits + 7) / 8;
    int raw_len = (stride + 1) * h;

    u8 *raw = (u8 *)malloc((u32)raw_len);
    if (!raw) { rc = PNG_TOO_BIG; goto done; }

    int got = inflate_zlib(zdata, zlen, raw, raw_len);
    if (got < 0) {
        free(raw);
        rc = got == INF_TRUNCATED ? PNG_TRUNCATED : PNG_BAD;
        goto done;
    }

    u8 *rgb = (u8 *)malloc((u32)(w * h * 3));
    if (!rgb) { free(raw); rc = PNG_TOO_BIG; goto done; }

    int bpp = (channels * depth + 7) / 8;
    if (bpp < 1) bpp = 1;

    int bg_r = (int)((bg >> 16) & 0xFF);
    int bg_g = (int)((bg >> 8) & 0xFF);
    int bg_b = (int)(bg & 0xFF);

    u8 *prev = 0;
    for (int y = 0; y < h; y++) {
        int off = y * (stride + 1);
        if (off + stride + 1 > got) {
            /* A picture that stopped part way is still a picture as far as
               it got; the rest is left as the background. */
            for (int fill = y; fill < h; fill++)
                for (int x = 0; x < w; x++) {
                    u8 *px = rgb + (fill * w + x) * 3;
                    px[0] = (u8)bg_r; px[1] = (u8)bg_g; px[2] = (u8)bg_b;
                }
            break;
        }

        u8 filter = raw[off];
        u8 *row = raw + off + 1;
        png_unfilter(row, prev, stride, bpp, filter);
        prev = row;

        for (int x = 0; x < w; x++) {
            int r = 0, g = 0, b = 0, a = 255;

            if (depth == 8) {
                const u8 *p = row + x * channels;
                if (colour == 0) { r = g = b = p[0]; }
                else if (colour == 2) { r = p[0]; g = p[1]; b = p[2]; }
                else if (colour == 3) {
                    int idx = p[0];
                    if (idx < palette_n) {
                        r = palette[idx * 3]; g = palette[idx * 3 + 1];
                        b = palette[idx * 3 + 2];
                    }
                    a = idx < trans_n ? trans[idx] : 255;
                } else if (colour == 4) { r = g = b = p[0]; a = p[1]; }
                else { r = p[0]; g = p[1]; b = p[2]; a = p[3]; }
            } else {
                /* Fewer than eight bits a pixel, packed high bits first. */
                int per = 8 / depth;
                int idx = (row[x / per] >> ((per - 1 - (x % per)) * depth))
                          & ((1 << depth) - 1);
                if (colour == 3) {
                    if (idx < palette_n) {
                        r = palette[idx * 3]; g = palette[idx * 3 + 1];
                        b = palette[idx * 3 + 2];
                    }
                    a = idx < trans_n ? trans[idx] : 255;
                } else {
                    int max = (1 << depth) - 1;
                    r = g = b = idx * 255 / max;
                }
            }

            u8 *px = rgb + (y * w + x) * 3;
            if (a >= 255) {
                px[0] = (u8)r; px[1] = (u8)g; px[2] = (u8)b;
            } else {
                px[0] = (u8)((r * a + bg_r * (255 - a)) / 255);
                px[1] = (u8)((g * a + bg_g * (255 - a)) / 255);
                px[2] = (u8)((b * a + bg_b * (255 - a)) / 255);
            }
        }
    }

    free(raw);
    free(zdata);
    out->w = w;
    out->h = h;
    out->rgb = rgb;
    return PNG_OK;

done:
    free(zdata);
    return rc;
}

static inline void picture_free(picture *p) {
    if (p->rgb) free(p->rgb);
    p->rgb = 0;
    p->w = p->h = 0;
}

static inline const char *png_why(int rc) {
    switch (rc) {
    case PNG_NOT_PNG:     return "not a png";
    case PNG_TRUNCATED:   return "the picture stopped part way";
    case PNG_UNSUPPORTED: return "interlaced or sixteen bit, which this does not read";
    case PNG_TOO_BIG:     return "too big to hold";
    default:              return "it does not decode";
    }
}
