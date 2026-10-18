/* See include/shot.h.
 *
 * The deflate is LZ77 over a 32 KiB window, matches found through a hash of
 * three bytes with a short chain, written with the fixed Huffman codes. A
 * desktop is flat colour and straight edges, and that compresses well
 * without building codes of its own: a typical screen comes out a few
 * hundred kilobytes rather than the two and a third megabytes it is. Each
 * row is filtered before it (none, the pixel to the left, or the row above)
 * by whichever leaves the smallest numbers, as PNG writers commonly choose,
 * so a gradient wallpaper becomes runs of the same few bytes. */
#include "shot.h"
#include "fb.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "vfs.h"
#include "zone.h"
#include "parts.h"                      /* crc32 */

/* --- bits, least significant first --------------------------------------- */

typedef struct {
    u8 *out;
    u32 cap, len;
    u32 acc;
    int nacc;
    bool full;
} bits_t;

static void put_bits(bits_t *b, u32 v, int n) {
    b->acc |= v << b->nacc;
    b->nacc += n;
    while (b->nacc >= 8) {
        if (b->len < b->cap) b->out[b->len++] = (u8)b->acc;
        else b->full = true;
        b->acc >>= 8;
        b->nacc -= 8;
    }
}

/* A Huffman code goes out most significant bit first, inside a stream that
   is otherwise least significant first: the classic trap of this format. */
static void put_code(bits_t *b, u32 code, int n) {
    u32 r = 0;
    for (int i = 0; i < n; i++) r |= ((code >> i) & 1) << (n - 1 - i);
    put_bits(b, r, n);
}

static void put_literal(bits_t *b, u32 v) {
    if (v < 144)      put_code(b, 0x30 + v, 8);
    else if (v < 256) put_code(b, 0x190 + (v - 144), 9);
    else if (v < 280) put_code(b, v - 256, 7);
    else              put_code(b, 0xC0 + (v - 280), 8);
}

static const u16 LEN_BASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 LEN_EXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 DIST_BASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                   257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                   8193, 12289, 16385, 24577 };
static const u8 DIST_EXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                   7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void put_match(bits_t *b, u32 len, u32 dist) {
    int i = 28;
    while (LEN_BASE[i] > len) i--;
    put_literal(b, 257 + (u32)i);
    if (LEN_EXTRA[i]) put_bits(b, len - LEN_BASE[i], LEN_EXTRA[i]);
    int d = 29;
    while (DIST_BASE[d] > dist) d--;
    put_code(b, (u32)d, 5);
    if (DIST_EXTRA[d]) put_bits(b, dist - DIST_BASE[d], DIST_EXTRA[d]);
}

/* --- deflate ---------------------------------------------------------------- */

#define WINDOW     32768u
#define HASH_BITS  15
#define HASH_SIZE  (1u << HASH_BITS)
#define MAX_CHAIN  24
#define MIN_MATCH  3
#define MAX_MATCH  258

static u32 hash3(const u8 *p) {
    return ((u32)p[0] << 10 ^ (u32)p[1] << 5 ^ p[2]) & (HASH_SIZE - 1);
}

u32 shot_deflate(const u8 *in, u32 n, u8 *out, u32 cap) {
    u32 *head = (u32 *)kmalloc(HASH_SIZE * sizeof(u32));
    u32 *prev = (u32 *)kmalloc(WINDOW * sizeof(u32));
    if (!head || !prev) { kfree(head); kfree(prev); return 0; }
    for (u32 i = 0; i < HASH_SIZE; i++) head[i] = 0;       /* 0 is none: positions are kept plus one */

    bits_t b = { out, cap, 0, 0, 0, false };
    put_bits(&b, 1, 1);                 /* the last block */
    put_bits(&b, 1, 2);                 /* the fixed codes */

    u32 i = 0;
    while (i < n) {
        u32 best = 0, best_dist = 0;
        if (i + MIN_MATCH <= n) {
            u32 h = hash3(in + i);
            u32 cand = head[h];
            u32 limit = n - i < MAX_MATCH ? n - i : MAX_MATCH;
            for (int chain = 0; cand && chain < MAX_CHAIN; chain++) {
                u32 at = cand - 1;
                if (i - at > WINDOW - 1) break;
                if (in[at + best] == in[i + best]) {
                    u32 l = 0;
                    while (l < limit && in[at + l] == in[i + l]) l++;
                    if (l > best) { best = l; best_dist = i - at; if (l == limit) break; }
                }
                u32 next = prev[at & (WINDOW - 1)];
                if (next >= cand) break;        /* the chain has wrapped into newer entries */
                cand = next;
            }
        }
        u32 step = best >= MIN_MATCH ? best : 1;
        if (best >= MIN_MATCH) put_match(&b, best, best_dist);
        else put_literal(&b, in[i]);
        /* Every position passed over goes into the hash, so a later match
           can start anywhere. */
        for (u32 k = 0; k < step; k++, i++) {
            if (i + MIN_MATCH <= n) {
                u32 h = hash3(in + i);
                prev[i & (WINDOW - 1)] = head[h];
                head[h] = i + 1;
            }
        }
    }
    put_literal(&b, 256);               /* the end of the block */
    if (b.nacc) put_bits(&b, 0, 8 - b.nacc);
    kfree(head);
    kfree(prev);
    return b.full ? 0 : b.len;
}

/* --- PNG ---------------------------------------------------------------------- */

static void be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

/* A chunk's length, type and data are already at p; this adds the CRC of
   the type and data and says where the next chunk starts. */
static u32 seal_chunk(u8 *p, u32 data_len) {
    be32(p + 8 + data_len, crc32(p + 4, 4 + data_len));
    return 12 + data_len;
}

static u32 absum(const u8 *f, u32 n) {
    u32 s = 0;
    for (u32 i = 0; i < n; i++) s += f[i] < 128 ? f[i] : 256 - f[i];
    return s;
}

u8 *shot_png(u32 w, u32 h, const u32 *(*row)(u32 y, void *arg), void *arg, u32 *len) {
    u32 stride = 1 + 3 * w;
    u32 raw_len = stride * h;
    u8 *raw = (u8 *)kmalloc(raw_len);
    u8 *line = (u8 *)kmalloc(3 * w * 2);
    u8 *trial = (u8 *)kmalloc(3 * w);
    if (!raw || !line || !trial) { kfree(raw); kfree(line); kfree(trial); return 0; }

    u8 *above = line, *here = line + 3 * w;
    memset(above, 0, 3 * w);
    for (u32 y = 0; y < h; y++) {
        const u32 *src = row(y, arg);
        for (u32 x = 0; x < w; x++) {
            u32 p = src ? src[x] : 0;
            here[3 * x] = (u8)(p >> 16); here[3 * x + 1] = (u8)(p >> 8); here[3 * x + 2] = (u8)p;
        }
        /* None, Sub and Up, and whichever is smallest. */
        u8 *out = raw + y * stride;
        out[0] = 0;
        memcpy(out + 1, here, 3 * w);
        u32 bestsum = absum(here, 3 * w);
        for (int f = 1; f <= 2; f++) {
            for (u32 k = 0; k < 3 * w; k++) {
                u8 left = k >= 3 ? here[k - 3] : 0;
                trial[k] = (u8)(here[k] - (f == 1 ? left : above[k]));
            }
            u32 sum = absum(trial, 3 * w);
            if (sum < bestsum) { bestsum = sum; out[0] = (u8)f; memcpy(out + 1, trial, 3 * w); }
        }
        u8 *t = above; above = here; here = t;
    }
    kfree(line);
    kfree(trial);

    /* zlib around the deflate: a header saying deflate with a 32 KiB window,
       and the Adler-32 of what was compressed at the end. */
    u32 zcap = raw_len + raw_len / 8 + 64;
    u32 total_cap = 8 + 25 + 12 + 2 + zcap + 4 + 12;
    u8 *png = (u8 *)kmalloc(total_cap);
    if (!png) { kfree(raw); return 0; }
    static const u8 SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    memcpy(png, SIG, 8);
    u32 at = 8;

    be32(png + at, 13);
    memcpy(png + at + 4, "IHDR", 4);
    be32(png + at + 8, w);
    be32(png + at + 12, h);
    png[at + 16] = 8;                   /* bits a channel */
    png[at + 17] = 2;                   /* red, green and blue */
    png[at + 18] = 0;                   /* deflate */
    png[at + 19] = 0;                   /* the five filters */
    png[at + 20] = 0;                   /* not interlaced */
    at += seal_chunk(png + at, 13);

    u8 *idat = png + at;
    memcpy(idat + 4, "IDAT", 4);
    u8 *z = idat + 8;
    z[0] = 0x78;
    z[1] = 0x01;
    u32 dn = shot_deflate(raw, raw_len, z + 2, zcap);
    if (!dn) { kfree(raw); kfree(png); return 0; }
    u32 a = 1, bsum = 0;
    for (u32 i = 0; i < raw_len; i++) {
        a += raw[i];
        if (a >= 65521) a -= 65521;
        bsum += a;
        if (bsum >= 65521) bsum -= 65521;
    }
    be32(z + 2 + dn, (bsum << 16) | a);
    u32 zlen = 2 + dn + 4;
    be32(idat, zlen);
    at += seal_chunk(idat, zlen);
    kfree(raw);

    be32(png + at, 0);
    memcpy(png + at + 4, "IEND", 4);
    at += seal_chunk(png + at, 0);
    *len = at;
    return png;
}

/* --- the screen ---------------------------------------------------------------- */

static const u32 *screen_row(u32 y, void *arg) {
    (void)arg;
    return fb_row((int)y);
}

const char *shot_save(const char **why) {
    static char path[96];
    *why = "";
    if (!fb_active()) { *why = "there is no screen"; return 0; }

    zone_time_t z;
    bool timed = zone_now(&z);
    vfs_mkdir("/home");
    vfs_mkdir("/home/pictures");
    if (timed)
        kformat(path, sizeof(path), "/home/pictures/screen %04d-%02d-%02d %02d.%02d.%02d.png",
                z.local.year, z.local.month, z.local.day, z.local.hour, z.local.minute, z.local.second);
    else
        kformat(path, sizeof(path), "/home/pictures/screen.png");

    u32 len = 0;
    u8 *png = shot_png(fb_width(), fb_height(), screen_row, 0, &len);
    if (!png) { *why = "not enough memory"; return 0; }
    bool ok = vfs_write(path, png, len);
    kfree(png);
    if (!ok) { *why = "the disk would not take it"; return 0; }
    return path;
}
