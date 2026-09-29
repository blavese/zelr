#pragma once
#include "zelr.h"
#include "alloc.h"
#include "png.h"

/* GIF, the first picture of the file: logos, spacers, the small pictures old
 * pages and plenty of new ones are still made of.
 *
 * The shape: six bytes saying GIF87a or GIF89a, the size of the screen it
 * draws on and whether a table of colours for all of it follows, then blocks
 * until a byte 0x3B ends it. An extension block (0x21) is skipped, except the
 * one saying which colour index is transparent; an image block (0x2C) is a
 * rectangle of the screen, with its own table of colours or not, drawn
 * top to bottom or in four passes (interlaced), and its indices compressed
 * with LZW in chunks of at most 255 bytes.
 *
 * LZW here is the variable-width kind: codes start one bit wider than the
 * file says, a clear code starts the table again, an end code stops, and a
 * code one past the table is the previous string with its own first byte on
 * the end. The table stops growing at 4096 entries and the codes at twelve
 * bits, and a file that goes on regardless is decoded with the table as it
 * is, which is what every decoder does.
 *
 * What is not here: any frame after the first -- an animation shows where it
 * starts -- and the screen's background colour, since a transparent pixel is
 * laid over the page's (`bg`) like a PNG's. */

#define GIF_OK          0
#define GIF_NOT_GIF    -1
#define GIF_TRUNCATED  -2
#define GIF_TOO_BIG    -4
#define GIF_BAD        -5

static inline int gif_decode(const u8 *d, int n, picture *out, u32 bg) {
    out->w = out->h = 0;
    out->rgb = 0;
    if (n < 13) return GIF_TRUNCATED;
    if (d[0] != 'G' || d[1] != 'I' || d[2] != 'F' || d[3] != '8'
        || (d[4] != '7' && d[4] != '9') || d[5] != 'a') return GIF_NOT_GIF;

    int sw = d[6] | (d[7] << 8), sh = d[8] | (d[9] << 8);
    int flags = d[10];
    int at = 13;
    u8 global[256 * 3];
    int global_n = 0;
    if (flags & 0x80) {
        global_n = 2 << (flags & 7);
        if (at + global_n * 3 > n) return GIF_TRUNCATED;
        for (int i = 0; i < global_n * 3; i++) global[i] = d[at + i];
        at += global_n * 3;
    }
    int transparent = -1;

    while (at < n) {
        int kind = d[at++];
        if (kind == 0x3B) break;                     /* the end, with no picture */
        if (kind == 0x21) {
            if (at >= n) return GIF_TRUNCATED;
            int label = d[at++];
            /* The graphic control block, for the one thing in it that
               matters to a still picture: which index shows through. */
            if (label == 0xF9 && at + 5 < n && d[at] >= 4) {
                if (d[at + 1] & 1) transparent = d[at + 4];
            }
            while (at < n && d[at]) at += d[at] + 1;
            at++;
            continue;
        }
        if (kind != 0x2C) return GIF_BAD;

        if (at + 9 > n) return GIF_TRUNCATED;
        int ix = d[at] | (d[at + 1] << 8), iy = d[at + 2] | (d[at + 3] << 8);
        int iw = d[at + 4] | (d[at + 5] << 8), ih = d[at + 6] | (d[at + 7] << 8);
        int iflags = d[at + 8];
        at += 9;
        const u8 *table = global;
        int table_n = global_n;
        u8 local[256 * 3];
        if (iflags & 0x80) {
            table_n = 2 << (iflags & 7);
            if (at + table_n * 3 > n) return GIF_TRUNCATED;
            for (int i = 0; i < table_n * 3; i++) local[i] = d[at + i];
            table = local;
            at += table_n * 3;
        }
        int interlaced = (iflags & 0x40) != 0;

        /* The screen is what the file says it is, or the image when the
           screen is given as smaller than the image in it. */
        if (sw < ix + iw) sw = ix + iw;
        if (sh < iy + ih) sh = iy + ih;
        if (sw <= 0 || sh <= 0 || iw <= 0 || ih <= 0) return GIF_BAD;
        if (sw > PNG_MAX_SIDE || sh > PNG_MAX_SIDE || sw * sh > PNG_MAX_PIXELS) return GIF_TOO_BIG;

        u8 *rgb = (u8 *)malloc((u32)(sw * sh * 3));
        u8 *idx = (u8 *)malloc((u32)(iw * ih));
        /* The strings: each code is its prefix code and its last byte; its
           first byte is kept too, which is what the one-past-the-table case
           needs. */
        short *prefix = (short *)malloc(4096 * sizeof(short));
        u8 *suffix = (u8 *)malloc(4096), *first = (u8 *)malloc(4096), *stack = (u8 *)malloc(4097);
        if (!rgb || !idx || !prefix || !suffix || !first || !stack) {
            if (rgb) free(rgb);
            if (idx) free(idx);
            if (prefix) free(prefix);
            if (suffix) free(suffix);
            if (first) free(first);
            if (stack) free(stack);
            return GIF_TOO_BIG;
        }
        for (int i = 0; i < sw * sh; i++) {
            rgb[i * 3] = (u8)(bg >> 16);
            rgb[i * 3 + 1] = (u8)(bg >> 8);
            rgb[i * 3 + 2] = (u8)bg;
        }

        if (at >= n) goto short_file;
        int min = d[at++];
        if (min < 2 || min > 8) min = min < 2 ? 2 : 8;
        int clear = 1 << min, end = clear + 1;
        int size = min + 1, next = clear + 2, prev = -1;
        int decoded = 0;                   /* indices that arrived */
        for (int i = 0; i < clear; i++) { prefix[i] = -1; suffix[i] = (u8)i; first[i] = (u8)i; }

        {
            unsigned acc = 0;
            int bits = 0, left = 0, out_at = 0, total = iw * ih, done = 0;
            while (!done && out_at < total) {
                /* Enough bits for one code, across the chunk edges. */
                while (bits < size) {
                    if (left == 0) {
                        if (at >= n || d[at] == 0) { done = 1; break; }
                        left = d[at++];
                    }
                    if (at >= n) { done = 1; break; }
                    acc |= (unsigned)d[at++] << bits;
                    bits += 8;
                    left--;
                }
                if (done) break;
                int code = (int)(acc & ((1u << size) - 1));
                acc >>= size;
                bits -= size;

                if (code == clear) { size = min + 1; next = clear + 2; prev = -1; continue; }
                if (code == end) break;

                int cur = code, sp = 0;
                if (code >= next) {
                    /* One past the table: the previous string and its own
                       first byte. Anything further is a broken file. */
                    if (code > next || prev < 0) break;
                    stack[sp++] = first[prev];
                    cur = prev;
                }
                while (cur >= 0 && sp < 4097) {
                    stack[sp++] = suffix[cur];
                    if (cur < clear) break;
                    cur = prefix[cur];
                }
                while (sp > 0 && out_at < total) idx[out_at++] = stack[--sp];
                decoded = out_at;

                if (prev >= 0 && next < 4096) {
                    prefix[next] = (short)prev;
                    suffix[next] = code < next ? first[code] : first[prev];
                    first[next] = first[prev];
                    next++;
                    if (next == (1 << size) && size < 12) size++;
                }
                prev = code;
            }
        }

        /* Rows, in their passes when interlaced: every eighth from 0, every
           eighth from 4, every fourth from 2, every second from 1. */
        {
            static const int START[4] = { 0, 4, 2, 1 }, STEP[4] = { 8, 8, 4, 2 };
            int row = 0;
            for (int pass = 0; pass < (interlaced ? 4 : 1); pass++) {
                for (int y = interlaced ? START[pass] : 0; y < ih; y += interlaced ? STEP[pass] : 1) {
                    for (int x = 0; x < iw; x++) {
                        /* A file cut short leaves the page where its pixels
                           did not arrive, not the first colour in the table. */
                        if (row * iw + x >= decoded) continue;
                        int c = idx[row * iw + x];
                        if (c == transparent || c >= table_n) continue;
                        int px = ((iy + y) * sw + ix + x) * 3;
                        rgb[px] = table[c * 3];
                        rgb[px + 1] = table[c * 3 + 1];
                        rgb[px + 2] = table[c * 3 + 2];
                    }
                    row++;
                }
            }
        }
        free(idx); free(prefix); free(suffix); free(first); free(stack);
        out->w = sw;
        out->h = sh;
        out->rgb = rgb;
        return GIF_OK;

    short_file:
        free(rgb); free(idx); free(prefix); free(suffix); free(first); free(stack);
        return GIF_TRUNCATED;
    }
    return GIF_BAD;
}
