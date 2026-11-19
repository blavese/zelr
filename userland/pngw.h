#pragma once
#include "zelr.h"

/* PNG, written: what Paint saves.
 *
 * An RGB picture (colour type 2, eight bits a sample), every row with no
 * filter, the rows in a zlib stream of stored deflate blocks: nothing is
 * compressed. That is the whole of the format a reader must accept, so any
 * program that opens PNG opens these, and there is no encoder here to be
 * wrong. A picture the size of the window is under a megabyte this way,
 * which the disk does not mind.
 *
 * Its own CRC-32 (PNG's: reflected, 0xEDB88320) and Adler-32 (zlib's), so
 * this needs nothing but itself. */

static inline u32 pngw_crc(u32 c, const u8 *p, u32 n) {
    static u32 table[256];
    static int made;
    if (!made) {
        for (u32 i = 0; i < 256; i++) {
            u32 v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        made = 1;
    }
    c ^= 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static inline void pngw_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

/* The rows as the stream carries them: a filter byte (0) and the pixels. */
static inline u32 pngw_raw_size(int w, int h) { return (u32)h * (1 + (u32)w * 3); }

/* How many bytes the file for a w by h picture takes. */
static inline u32 png_write_size(int w, int h) {
    u32 raw = pngw_raw_size(w, h);
    u32 blocks = raw / 65535 + 1;
    return 8 + (12 + 13) + (12 + 2 + raw + blocks * 5 + 4) + 12;
}

/* A chunk's length and type, its data already in place after them, and its
   CRC after that. Returns where the next chunk goes. */
static inline u32 pngw_chunk(u8 *out, u32 at, const char *type, u32 len) {
    pngw_be32(out + at, len);
    for (int i = 0; i < 4; i++) out[at + 4 + i] = (u8)type[i];
    pngw_be32(out + at + 8 + len, pngw_crc(0, out + at + 4, 4 + len));
    return at + 12 + len;
}

/* The picture px (w by h, stride pixels a row, 0x00RRGGBB) as a PNG file in
   out, which must hold png_write_size(w, h) bytes: how many it took, or -1. */
static inline int png_write(const u32 *px, int stride, int w, int h, u8 *out, u32 cap) {
    if (w <= 0 || h <= 0 || cap < png_write_size(w, h)) return -1;
    static const u8 SIG[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    u32 at = 0;
    for (int i = 0; i < 8; i++) out[at++] = SIG[i];

    u8 *ih = out + at + 8;
    pngw_be32(ih, (u32)w);
    pngw_be32(ih + 4, (u32)h);
    ih[8] = 8; ih[9] = 2; ih[10] = 0; ih[11] = 0; ih[12] = 0;
    at = pngw_chunk(out, at, "IHDR", 13);

    /* IDAT: the zlib header, stored blocks, the Adler-32. */
    u32 idat = at, d = at + 8;
    out[d++] = 0x78;
    out[d++] = 0x01;
    u32 raw = pngw_raw_size(w, h), done = 0;
    u32 a = 1, b = 0;
    int row = 0, col = -1;               /* where in the rows the next byte comes from; -1 the filter byte */
    while (done < raw || raw == 0) {
        u32 n = raw - done > 65535 ? 65535 : raw - done;
        out[d++] = done + n == raw ? 1 : 0;
        out[d++] = (u8)n; out[d++] = (u8)(n >> 8);
        out[d++] = (u8)~n; out[d++] = (u8)(~n >> 8);
        for (u32 i = 0; i < n; i++) {
            u8 v;
            if (col < 0) v = 0;
            else {
                u32 p = px[(u32)row * (u32)stride + (u32)(col / 3)];
                v = (u8)(p >> (16 - 8 * (col % 3)));
            }
            if (++col == w * 3) { col = -1; row++; }
            out[d++] = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
        done += n;
        if (!raw) break;
    }
    pngw_be32(out + d, (b << 16) | a);
    d += 4;
    at = pngw_chunk(out, idat, "IDAT", d - (idat + 8));
    at = pngw_chunk(out, at, "IEND", 0);
    return (int)at;
}
