/* The WebP decoder, against files it did not make and a decoder it is not.
 *
 * The pictures are this project's own, drawn pixel by pixel in
 * tools/genwebp.html, but the files were made by a browser's WebP encoder,
 * and what they are compared with is what Windows' own WebP decoder makes of
 * them (tools/genwebp.ps1 writes both into webpdata.h). They are: a scene of
 * gradients, hard edges, stripes, noise, flat colour and checks, lossy at
 * three qualities (the loop filter at levels from gentle to 63); a lossless
 * picture in four quarters, big enough to be given several sets of codes;
 * three lossless ones of few colours, which the encoder packs into colour
 * tables four, eight and two indices to a pixel; a picture with soft and hard
 * transparent edges, lossy (with an ALPH chunk) and lossless; and three small
 * ones whose alpha is shaped so the encoder filters it each of the other ways.
 *
 * A lossless file has one right answer, so every pixel must be exact: its
 * hash over every pixel must be Windows'. So does a lossy one, as far as
 * brightness and colour: VP8 decoding is exact, and only turning it into red,
 * green and blue is left to the decoder, which rounds its own way. Measured
 * over every pixel of every lossy file, no channel is more than 1 from
 * Windows' and at most half a percent are off by that much; WEBP_TOLERANCE
 * allows 2, and the mean difference over the samples must stay under
 * WEBP_MEAN_TOLERANCE thousandths. Anything wrong in the decoding itself (a
 * prediction, a filter, a quantiser) moves many pixels by more. Alpha is
 * exact everywhere (it is lossless even beside a lossy picture), and is read
 * back by laying a picture over black and over white: the difference is
 * 255 - alpha.
 *
 * The browser's encoder, which makes its lossless files the fast way, never
 * uses the colour transform, the colour cache or ten of the fourteen
 * predictors; nor, lossy, the simple loop filter or alpha stored raw; and it
 * makes no animations. So those are made here, and say so: three lossless
 * streams assembled bit by bit from RFC 9649, raw ALPH chunks under each
 * filter around one of the lossy files, an animation whose first frame is
 * the lossless picture, and two parts of VP8 checked by themselves against
 * values worked by hand from RFC 6386. Those are this project's reading of
 * the RFCs, not a second encoder's.
 *
 * Then the ways a file can be wrong: not a WebP, too big, cut short at every
 * kind of place, and damaged in the middle, where it must give an error or a
 * picture of its own size, never read past the bytes it was given (they end
 * against a page nobody mapped), never write outside what it allocated (the
 * heap is walked), and give back everything it took.
 */
#include "zelr.h"
#include "alloc.h"
#define WEBP_TRACE
#include "webp.h"
#include "webpdata.h"

/* See the top of this file for why these are what they are. */
#define WEBP_TOLERANCE 2
#define WEBP_MEAN_TOLERANCE 20

static int failed;

static void ok(const char *what, int cond) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    putc('\n');
    if (!cond) failed++;
}

static void okn(const char *what, int cond, int n) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    puts("  ");
    putn(n);
    putc('\n');
    if (!cond) failed++;
}

static void note(const char *what, int n, const char *unit) {
    puts("        ");
    puts(what);
    puts("  ");
    putn(n);
    puts(unit);
    putc('\n');
}

static u8 *unbase64(const char *s, int *len) {
    int n = strlen(s);
    u8 *out = (u8 *)malloc((u64)n / 4 * 3 + 3);
    int k = 0, acc = 0, bits = 0;
    for (int i = 0; i < n; i++) {
        int c = s[i], v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue;                           /* padding */
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[k++] = (u8)(acc >> bits); }
    }
    *len = k;
    return out;
}

/* FNV-1a, as tools/genwebp.ps1 hashes Windows' pixels. */
static u32 fnv(const u8 *p, int n) {
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* Every block the allocator holds, walked from the bottom of the heap:
   each one's size at its head must be the size at its foot. A decoder that
   wrote past what it was given breaks one of them. */
static int heap_intact(void) {
    u64 at = al_bottom;
    while (at && at < al_top) {
        al_block *b = (al_block *)at;
        u64 next = at + AL_HDR + AL_SIZE(b) + AL_FOOT;
        if (next > al_top || *al_footer(b) != b->size) return 0;
        at = next;
    }
    return at == al_top;
}

/* A copy of n bytes placed against the end of a mapping of its own, so that
   reading even one byte past them touches a page nobody mapped: here and on
   the host alike that ends the program, and this never prints its last
   line. (The heap is sbrk's, so nothing else is mapped after it.) */
typedef struct { u8 *region; u64 size; } guard;

static u8 *guarded(const u8 *src, int n, guard *g) {
    g->size = ((u64)n + 4095) & ~4095ull;
    if (!g->size) g->size = 4096;
    g->region = (u8 *)map(g->size, PROT_READ | PROT_WRITE);
    if (!g->region) return 0;
    u8 *at = g->region + g->size - n;
    for (int i = 0; i < n; i++) at[i] = src[i];
    return at;
}

static void unguard(guard *g) {
    if (g->region) unmap(g->region, g->size);
    g->region = 0;
}

static u32 rgb_at(const picture *p, int x, int y) {
    const u8 *px = p->rgb + (y * p->w + x) * 3;
    return ((u32)px[0] << 16) | ((u32)px[1] << 8) | px[2];
}

/* --- the files --------------------------------------------------------------- */

typedef struct {
    const webp_ref *ref;
    u8 *file;
    int n;
} webp_file;

static webp_file files[WEBP_REF_COUNT];

static const webp_file *file_named(const char *name) {
    for (int i = 0; i < WEBP_REF_COUNT; i++)
        if (!strcmp(files[i].ref->name, name)) return &files[i];
    return 0;
}

/* The body of the first chunk of that name after the RIFF header, or 0;
   *at is where the chunk's own header starts. */
static const u8 *find_chunk(const u8 *file, int n, const char *name, int *size, int *at) {
    int i = 12;
    while (i + 8 <= n) {
        int s = (int)webp_le32(file + i + 4);
        if (png_is(file + i, name)) { *size = s; if (at) *at = i; return file + i + 8; }
        i += 8 + s + (s & 1);
    }
    return 0;
}

typedef struct {
    int max, mean1000, over;         /* worst channel difference, mean in thousandths */
} diff;

/* The picture against a lossy file's samples (laid over WEBP_REF_BG). */
static diff compare(const picture *p, const webp_ref *r, const u8 *ref) {
    diff d = { 0, 0, 0 };
    long long sum = 0;
    int n = 0, k = 0;
    for (int y = 0; y < r->h; y += r->step)
        for (int x = 0; x < r->w; x += r->step, k++)
            for (int c = 0; c < 3; c++) {
                int e = p->rgb[(y * p->w + x) * 3 + c] - ref[k * 3 + c];
                if (e < 0) e = -e;
                if (e > d.max) d.max = e;
                if (e > WEBP_TOLERANCE) d.over++;
                sum += e;
                n++;
            }
    d.mean1000 = n ? (int)(sum * 1000 / n) : 0;
    return d;
}

/* The hash of every pixel's alpha, from the picture over black and over
   white; *decoded says whether it decoded at all. */
static u32 alpha_hash(const u8 *file, int n, int *decoded) {
    picture black, white;
    int a = webp_decode(file, n, &black, 0x000000);
    int b = webp_decode(file, n, &white, 0xFFFFFF);
    u32 h = 0;
    *decoded = a == WEBP_OK && b == WEBP_OK;
    if (*decoded) {
        u8 *alpha = (u8 *)malloc((u64)black.w * black.h);
        for (int i = 0; i < black.w * black.h; i++)
            alpha[i] = (u8)(255 - (white.rgb[i * 3 + 1] - black.rgb[i * 3 + 1]));
        h = fnv(alpha, black.w * black.h);
        free(alpha);
    }
    picture_free(&black);
    picture_free(&white);
    return h;
}

/* One file of the table, every way it can be compared. */
static void check_file(const webp_file *f) {
    const webp_ref *r = f->ref;
    picture p;
    puts(r->name);
    puts(r->lossless ? ", lossless\n" : ", lossy\n");
    u64 before = heap_live();
    int rc = webp_decode(f->file, f->n, &p, WEBP_REF_BG);
    okn("  decodes", rc == WEBP_OK, rc);
    if (rc == WEBP_OK) {
        ok("  at the size it says", p.w == r->w && p.h == r->h);
        if (r->lossless) {
            ok("  every pixel exactly Windows' colour", fnv(p.rgb, p.w * p.h * 3) == r->rgb_hash);
        } else {
            int rn;
            u8 *ref = unbase64(r->rgb, &rn);
            diff d = compare(&p, r, ref);
            okn("  every sample within the tolerance of Windows' (the worst)", d.over == 0, d.max);
            okn("  and the mean difference, in thousandths, within its tolerance",
                d.mean1000 <= WEBP_MEAN_TOLERANCE, d.mean1000);
            free(ref);
        }
        ok("  having written nowhere it should not (the heap intact)", heap_intact());
        picture_free(&p);
    }
    int decoded;
    u32 h = alpha_hash(f->file, f->n, &decoded);
    ok("  every pixel's alpha exactly Windows'", decoded && h == r->alpha_hash);
    ok("  and every byte it took is given back", heap_live() == before);
}

/* --- files made here, around the browser's ------------------------------------ */

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void put24(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); }

/* A chunk: name, size, the bytes, a pad byte to even. */
static int put_chunk(u8 *out, const char *name, const u8 *body, int n) {
    for (int i = 0; i < 4; i++) out[i] = (u8)name[i];
    put32(out + 4, (u32)n);
    for (int i = 0; i < n; i++) out[8 + i] = body[i];
    if (n & 1) out[8 + n] = 0;
    return 8 + n + (n & 1);
}

static void put_riff(u8 *file, int total) {
    file[0] = 'R'; file[1] = 'I'; file[2] = 'F'; file[3] = 'F';
    put32(file + 4, (u32)(total - 8));
    file[8] = 'W'; file[9] = 'E'; file[10] = 'B'; file[11] = 'P';
}

static int put_vp8x(u8 *out, int flags, int w, int h) {
    u8 body[10];
    for (int i = 0; i < 10; i++) body[i] = 0;
    body[0] = (u8)flags;
    put24(body + 4, (u32)(w - 1));
    put24(body + 7, (u32)(h - 1));
    return put_chunk(out, "VP8X", body, 10);
}

/* The alpha the made-up ALPH chunks carry: ramps and a disc, so that every
   filter has something to guess at. */
static int made_alpha(int x, int y) {
    int dx = x - 100, dy = y - 80;
    if (dx * dx + dy * dy < 50 * 50) return 255;
    return (x * 3 + y * 5) & 0xFF;
}

/* Filters alpha the way an encoder would (RFC 9649 section 2.7.1.2): each
   value less its guess, modulo 256. */
static void filter_alpha(const u8 *a, u8 *out, int w, int h, int filter) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const u8 *p = a + y * w + x;
            int guess;
            if (!filter || (x == 0 && y == 0)) guess = 0;
            else if (y == 0) guess = p[-1];
            else if (x == 0) guess = p[-w];
            else if (filter == 1) guess = p[-1];
            else if (filter == 2) guess = p[-w];
            else {
                guess = p[-1] + p[-w] - p[-w - 1];
                guess = guess < 0 ? 0 : guess > 255 ? 255 : guess;
            }
            out[y * w + x] = (u8)(p[0] - guess);
        }
}

/* --- a lossless stream assembled here ----------------------------------------
 *
 * Ten by four pixels of three colours, so a colour table packs four indices
 * to a pixel (the last packed pixel of each row only half used); the table
 * itself sent as differences with two-symbol codes; the packed picture, 3 x 4,
 * with a sixteen-entry colour cache, a normal code for green with runs of
 * zero lengths, and one index past the table, which must come out
 * transparent. Its twelve packed pixels, A to E and Y being literals:
 *
 *   A  B  C      three literals
 *   A  D  B      A and B from the cache, where only literals put them
 *   Y  A  A      Y shares A's slot and takes it; A copied from above and to
 *                the left (distance code 3), which puts A back; A from the
 *                cache, which is A only if the copy did that
 *   Y  A  A      three copied from the row above (distance code 1)
 *
 * Written from RFC 9649; the decoder's own code is not used. */
typedef struct { u8 *buf; int n; u32 acc; int bits; } bitw;

static void bw_put(bitw *w, u32 v, int n) {
    for (int i = 0; i < n; i++) {
        w->acc |= ((v >> i) & 1) << w->bits;
        if (++w->bits == 8) { w->buf[w->n++] = (u8)w->acc; w->acc = 0; w->bits = 0; }
    }
}

/* A prefix code goes first bit first, the other way round from the rest. */
static void bw_code(bitw *w, int code, int len) {
    for (int i = len - 1; i >= 0; i--) bw_put(w, (u32)(code >> i) & 1, 1);
}

static void canonical(const u8 *len, int n, int *code) {
    /* Arrays are zeroed by loops here: an initialiser of zeros is a call to
       memset, which a program here does not link against. */
    int count[16], next[16];
    for (int l = 0; l < 16; l++) count[l] = 0;
    for (int i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    int c = 0;
    for (int l = 1; l < 16; l++) { c = (c + count[l - 1]) << 1; next[l] = c; }
    for (int i = 0; i < n; i++) if (len[i]) code[i] = next[len[i]]++;
}

static void put_simple1(bitw *w, int sym) {
    bw_put(w, 1, 1);
    bw_put(w, 0, 1);
    if (sym < 2) { bw_put(w, 0, 1); bw_put(w, (u32)sym, 1); }
    else { bw_put(w, 1, 1); bw_put(w, (u32)sym, 8); }
}

static void put_simple2(bitw *w, int s0, int s1) {
    bw_put(w, 1, 1);
    bw_put(w, 1, 1);
    bw_put(w, 1, 1);
    bw_put(w, (u32)s0, 8);
    bw_put(w, (u32)s1, 8);
}

/* A symbol of a two-symbol code: the smaller is 0. */
static void put_of2(bitw *w, int sym, int s0, int s1) {
    bw_put(w, (u32)(sym == (s0 > s1 ? s0 : s1)), 1);
}

static const u8 CL_ORDER[7] = { 17, 18, 0, 1, 2, 3, 4 };

/* A normal code whose lengths are all 0, 2, 3 or 4: the code for the
   lengths gives 17 and 18 (runs of zeros) two bits, and 0, 2, 3, 4 three. */
static void put_normal(bitw *w, const u8 *len, int n) {
    u8 cl[19];
    int clcode[19];
    for (int i = 0; i < 19; i++) cl[i] = 0;
    cl[17] = cl[18] = 2;
    cl[0] = cl[2] = cl[3] = cl[4] = 3;
    canonical(cl, 19, clcode);
    bw_put(w, 0, 1);
    bw_put(w, 7 - 4, 4);
    for (int i = 0; i < 7; i++) bw_put(w, cl[CL_ORDER[i]], 3);
    bw_put(w, 0, 1);                             /* every length is sent */
    for (int s = 0; s < n;) {
        int run = 0;
        while (s + run < n && !len[s + run] && run < 138) run++;
        if (run >= 11) { bw_code(w, clcode[18], 2); bw_put(w, (u32)(run - 11), 7); s += run; continue; }
        if (run >= 3) { bw_code(w, clcode[17], 2); bw_put(w, (u32)(run - 3), 3); s += run; continue; }
        bw_code(w, clcode[len[s]], 3);
        s++;
    }
}

static u32 cache_slot(u32 argb, int bits) { return (0x1e35a7bdu * argb) >> (32 - bits); }

/* The packed pixels' green values, four two-bit indices each, the first in
   the lowest bits: A 0,1,2,0  B 1,1,2,2  C 0,2,0,0  D 0,1,2,3  Y 2,2,0,1. Y
   is chosen to share A's slot in a cache of sixteen. */
enum { MADE_A = 36, MADE_B = 165, MADE_C = 8, MADE_D = 228, MADE_Y = 74 };

/* What the made stream decodes to: the index of each pixel (3 is past the
   table), and the table. */
static const u8 MADE_INDEX[4][10] = {
    { 0, 1, 2, 0, 1, 1, 2, 2, 0, 2 },
    { 0, 1, 2, 0, 0, 1, 2, 3, 1, 1 },
    { 2, 2, 0, 1, 0, 1, 2, 0, 0, 1 },
    { 2, 2, 0, 1, 0, 1, 2, 0, 0, 1 },
};
static const u32 MADE_TABLE[3] = { 0xFF0A141Eu, 0xFF32465Au, 0xFF5A7896u };

/* The ways the made stream can be made wrong, to see each refused. */
enum { MADE_GOOD, MADE_CACHE12, MADE_VERSION, MADE_TWICE, MADE_TOO_FAR, MADE_INCOMPLETE, MADE_OVERRUN };

static int make_lossless(u8 *out, int variant) {
    bitw w = { out, 0, 0, 0 };
    bw_put(&w, 0x2F, 8);
    bw_put(&w, 10 - 1, 14);
    bw_put(&w, 4 - 1, 14);
    bw_put(&w, 0, 1);
    bw_put(&w, variant == MADE_VERSION ? 1 : 0, 3);
    if (variant == MADE_TWICE) {                 /* subtract green, twice */
        bw_put(&w, 1, 1); bw_put(&w, 2, 2);
        bw_put(&w, 1, 1); bw_put(&w, 2, 2);
    }
    /* The colour table: 3 colours, sent as the first and then the
       difference from the one before, (0, 40, 50, 60) both times. */
    bw_put(&w, 1, 1);
    bw_put(&w, 3, 2);
    bw_put(&w, 3 - 1, 8);
    bw_put(&w, 0, 1);                            /* no cache */
    put_simple2(&w, 20, 50);                     /* green */
    put_simple2(&w, 10, 40);                     /* red */
    put_simple2(&w, 30, 60);                     /* blue */
    put_simple2(&w, 255, 0);                     /* alpha */
    put_simple1(&w, 0);                          /* distance, unused */
    for (int i = 0; i < 3; i++) {
        put_of2(&w, i ? 50 : 20, 20, 50);
        put_of2(&w, i ? 40 : 10, 10, 40);
        put_of2(&w, i ? 60 : 30, 30, 60);
        put_of2(&w, i ? 0 : 255, 255, 0);
    }
    bw_put(&w, 0, 1);                            /* no more transforms */

    /* The packed picture. Every literal's red and blue are 0 and its alpha
       255, each from a code of one symbol, which takes no bits. */
    int bits = variant == MADE_CACHE12 ? 12 : 4;
    bw_put(&w, 1, 1);
    bw_put(&w, (u32)(bits & 15), 4);
    bw_put(&w, 0, 1);                            /* one set of codes */
    int alphabet = 256 + 24 + 16;
    u8 len[256 + 24 + 16];
    int code[256 + 24 + 16];
    for (int i = 0; i < alphabet; i++) len[i] = 0;
    int hit_a = 280 + (int)cache_slot(0xFF000000u | MADE_A << 8, 4);
    int hit_b = 280 + (int)cache_slot(0xFF000000u | MADE_B << 8, 4);
    int copy1 = 256 + 0, copy3 = 256 + 2;        /* lengths 1 and 3 */
    if (variant == MADE_OVERRUN) copy3 = 256 + 3;   /* 4, one past the last pixel */
    len[MADE_A] = len[MADE_B] = len[MADE_C] = len[MADE_D] = len[MADE_Y] = 3;
    len[copy1] = len[copy3] = 3;
    len[hit_a] = len[hit_b] = 4;
    if (variant == MADE_INCOMPLETE) len[copy3] = 4;
    canonical(len, alphabet, code);
    put_normal(&w, len, alphabet);
    put_simple1(&w, 0);                          /* red */
    put_simple1(&w, 0);                          /* blue */
    put_simple1(&w, 255);                        /* alpha */
    put_simple2(&w, 0, 2);                       /* distance prefixes 0 and 2: codes 1 and 3 */
    #define SYM(s) bw_code(&w, code[s], len[s])
    if (variant == MADE_TOO_FAR) { SYM(copy3); put_of2(&w, 0, 0, 2); }   /* before the start */
    SYM(MADE_A); SYM(MADE_B); SYM(MADE_C);
    SYM(hit_a); SYM(MADE_D); SYM(hit_b);
    SYM(MADE_Y); SYM(copy1); put_of2(&w, 2, 0, 2); SYM(hit_a);
    SYM(copy3); put_of2(&w, 0, 0, 2);
    #undef SYM
    if (w.bits) bw_put(&w, 0, 8 - w.bits);
    return w.n;
}

/* --- and one for the colour transform ----------------------------------------
 *
 * Four by two pixels under a colour transform of one block, whose element
 * adds green to red at 1.0, green to blue at -0.5 and red to blue at 0.5
 * (3.5 fixed point: 32, -16 and 16). Each channel of the stored pixels takes
 * one of two values, so every code is a simple one. What the pixels must
 * come back as is worked out below from RFC 9649 section 3.5.2; the blue is
 * where a decoder that used the red from before the transform goes wrong. */
static const u8 CT_G[8] = { 40, 200, 40, 200, 200, 40, 200, 40 };
static const u8 CT_R[8] = { 10, 10, 250, 250, 10, 250, 10, 250 };
static const u8 CT_B[8] = { 5, 100, 100, 5, 5, 5, 100, 100 };

static int ct_delta(int t, int c) { return ((int)(signed char)t * (int)(signed char)c) >> 5; }

static u32 ct_expected(int i) {
    int r = (CT_R[i] + ct_delta(32, CT_G[i])) & 0xFF;
    int b = (CT_B[i] + ct_delta(0xF0, CT_G[i]) + ct_delta(16, r)) & 0xFF;
    return ((u32)r << 16) | ((u32)CT_G[i] << 8) | (u32)b;
}

static int make_colour_transform(u8 *out) {
    bitw w = { out, 0, 0, 0 };
    bw_put(&w, 0x2F, 8);
    bw_put(&w, 4 - 1, 14);
    bw_put(&w, 2 - 1, 14);
    bw_put(&w, 0, 1);
    bw_put(&w, 0, 3);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);                            /* the colour transform */
    bw_put(&w, 0, 3);                            /* blocks of 4: one */
    bw_put(&w, 0, 1);                            /* no cache */
    put_simple1(&w, 0xF0);                       /* green: green to blue */
    put_simple1(&w, 16);                         /* red: red to blue */
    put_simple1(&w, 32);                         /* blue: green to red */
    put_simple1(&w, 255);
    put_simple1(&w, 0);
    bw_put(&w, 0, 1);                            /* no more transforms */
    bw_put(&w, 0, 1);                            /* no cache */
    bw_put(&w, 0, 1);                            /* one set of codes */
    put_simple2(&w, 40, 200);
    put_simple2(&w, 10, 250);
    put_simple2(&w, 5, 100);
    put_simple1(&w, 255);
    put_simple1(&w, 0);
    for (int i = 0; i < 8; i++) {
        put_of2(&w, CT_G[i], 40, 200);
        put_of2(&w, CT_R[i], 10, 250);
        put_of2(&w, CT_B[i], 5, 100);
    }
    if (w.bits) bw_put(&w, 0, 8 - w.bits);
    return w.n;
}

/* --- and one for the predictor -----------------------------------------------
 *
 * Sixteen by sixteen under a predictor transform of 4 x 4 blocks, one block
 * for each of the fourteen modes (the four that read the pixel above and to
 * the right down the right-hand side, where that pixel is the row's first);
 * the residues of every channel one of two values, picked by a sequence.
 * What the pixels must come back as is worked out here, a channel at a time,
 * from RFC 9649 section 3.5.1, not with the decoder's code. */
static const u8 PR_MODES[16] = { 0, 1, 2, 3, 4, 6, 7, 9, 8, 11, 12, 10, 13, 11, 13, 5 };
static const u8 PR_G[2] = { 5, 200 }, PR_R[2] = { 0, 77 }, PR_B[2] = { 3, 250 }, PR_A[2] = { 0, 255 };

static int pr_channel(u32 v, int c) { return (int)((v >> (c * 8)) & 0xFF); }

static u32 pr_join(const int *ch) {
    return (u32)(ch[0] & 0xFF) | (u32)(ch[1] & 0xFF) << 8 | (u32)(ch[2] & 0xFF) << 16 | (u32)(ch[3] & 0xFF) << 24;
}

static u32 pr_average(u32 a, u32 b) {
    int ch[4];
    for (int c = 0; c < 4; c++) ch[c] = (pr_channel(a, c) + pr_channel(b, c)) / 2;
    return pr_join(ch);
}

static int pr_clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static u32 pr_predict(int mode, u32 l, u32 t, u32 tl, u32 tr) {
    int ch[4];
    switch (mode) {
    case 0: return 0xFF000000u;
    case 1: return l;
    case 2: return t;
    case 3: return tr;
    case 4: return tl;
    case 5: return pr_average(pr_average(l, tr), t);
    case 6: return pr_average(l, tl);
    case 7: return pr_average(l, t);
    case 8: return pr_average(tl, t);
    case 9: return pr_average(t, tr);
    case 10: return pr_average(pr_average(l, tl), pr_average(t, tr));
    case 11: {
        int pl = 0, pt = 0;
        for (int c = 0; c < 4; c++) {
            int p = pr_channel(l, c) + pr_channel(t, c) - pr_channel(tl, c);
            pl += p > pr_channel(l, c) ? p - pr_channel(l, c) : pr_channel(l, c) - p;
            pt += p > pr_channel(t, c) ? p - pr_channel(t, c) : pr_channel(t, c) - p;
        }
        return pl < pt ? l : t;
    }
    case 12:
        for (int c = 0; c < 4; c++) ch[c] = pr_clamp(pr_channel(l, c) + pr_channel(t, c) - pr_channel(tl, c));
        return pr_join(ch);
    default: {
        u32 a = pr_average(l, t);
        for (int c = 0; c < 4; c++) ch[c] = pr_clamp(pr_channel(a, c) + (pr_channel(a, c) - pr_channel(tl, c)) / 2);
        return pr_join(ch);
    }
    }
}

/* The residue choices of pixel i: four bits, one a channel. */
static int pr_bits(int i) { return (int)(((u32)i * 2654435761u) >> 20) & 15; }

static void pr_expected(u32 *out) {
    for (int i = 0; i < 256; i++) {
        int x = i & 15, y = i >> 4, b = pr_bits(i), ch[4];
        u32 guess;
        if (!x && !y) guess = 0xFF000000u;
        else if (!y) guess = out[i - 1];
        else if (!x) guess = out[i - 16];
        else {
            /* Above and to the right, except down the right-hand side,
               where the specification makes it the row's first pixel. */
            u32 tr = x == 15 ? out[y * 16] : out[(y - 1) * 16 + x + 1];
            guess = pr_predict(PR_MODES[(y >> 2) * 4 + (x >> 2)], out[i - 1], out[i - 16], out[i - 17], tr);
        }
        u32 res = (u32)PR_B[b & 1] | (u32)PR_G[(b >> 1) & 1] << 8 | (u32)PR_R[(b >> 2) & 1] << 16
                | (u32)PR_A[(b >> 3) & 1] << 24;
        for (int c = 0; c < 4; c++) ch[c] = pr_channel(guess, c) + pr_channel(res, c);
        out[i] = pr_join(ch);
    }
}

static int make_predictor(u8 *out) {
    bitw w = { out, 0, 0, 0 };
    bw_put(&w, 0x2F, 8);
    bw_put(&w, 16 - 1, 14);
    bw_put(&w, 16 - 1, 14);
    bw_put(&w, 1, 1);
    bw_put(&w, 0, 3);
    bw_put(&w, 1, 1);
    bw_put(&w, 0, 2);                            /* the predictor */
    bw_put(&w, 0, 3);                            /* blocks of 4 */
    /* The modes, a 4 x 4 image whose green is the mode: fourteen symbols,
       two of three bits and twelve of four. */
    u8 len[256 + 24];
    int code[256 + 24];
    for (int i = 0; i < 256 + 24; i++) len[i] = 0;
    for (int m = 0; m < 14; m++) len[m] = m < 2 ? 3 : 4;
    canonical(len, 256 + 24, code);
    bw_put(&w, 0, 1);                            /* no cache */
    put_normal(&w, len, 256 + 24);
    put_simple1(&w, 0);
    put_simple1(&w, 0);
    put_simple1(&w, 255);
    put_simple1(&w, 0);
    for (int i = 0; i < 16; i++) bw_code(&w, code[PR_MODES[i]], len[PR_MODES[i]]);
    bw_put(&w, 0, 1);                            /* no more transforms */
    bw_put(&w, 0, 1);                            /* no cache */
    bw_put(&w, 0, 1);                            /* one set of codes */
    put_simple2(&w, PR_G[0], PR_G[1]);
    put_simple2(&w, PR_R[0], PR_R[1]);
    put_simple2(&w, PR_B[0], PR_B[1]);
    put_simple2(&w, PR_A[0], PR_A[1]);
    put_simple1(&w, 0);
    for (int i = 0; i < 256; i++) {
        int b = pr_bits(i);
        put_of2(&w, PR_G[(b >> 1) & 1], PR_G[0], PR_G[1]);
        put_of2(&w, PR_R[(b >> 2) & 1], PR_R[0], PR_R[1]);
        put_of2(&w, PR_B[b & 1], PR_B[0], PR_B[1]);
        put_of2(&w, PR_A[(b >> 3) & 1], PR_A[0], PR_A[1]);
    }
    if (w.bits) bw_put(&w, 0, 8 - w.bits);
    return w.n;
}

/* --- two parts of VP8 by themselves ---------------------------------------- */

/* Sixteen rows of one segment across a vertical edge, p3 p2 p1 p0 | q0 q1
   q2 q3, for the filters, which work on sixteen rows at a time. */
static u8 segment[16][8];

static void set_segment(const u8 *v) {
    for (int r = 0; r < 16; r++)
        for (int i = 0; i < 8; i++) segment[r][i] = v[i];
}

static int segment_is(const u8 *v) {
    for (int r = 0; r < 16; r++)
        for (int i = 0; i < 8; i++)
            if (segment[r][i] != v[i]) return 0;
    return 1;
}

/* The pixels of the made frame the subblock edges are read from. */
static int edge_px(int x, int y) { return (x * 7 + y * 29 + 3) & 0xFF; }

/* A whole file around a lossless chunk body. */
static int wrap_lossless(u8 *file, const u8 *body, int n) {
    int total = 12 + put_chunk(file + 12, "VP8L", body, n);
    put_riff(file, total);
    return total;
}

int main(void) {
    puts("webp pictures\n");
    picture p;

    for (int i = 0; i < WEBP_REF_COUNT; i++) {
        files[i].ref = &WEBP_REFS[i];
        files[i].file = unbase64(WEBP_REFS[i].file, &files[i].n);
    }
    const webp_file *scene = file_named("scene 0.9"), *photo = file_named("photo 1");
    const webp_file *alpha = file_named("alpha 0.8");

    /* --- each file ------------------------------------------------------------ */
    for (int i = 0; i < WEBP_REF_COUNT; i++) check_file(&files[i]);
    puts("between them\n");
    ok("  the predictor, subtract-green and colour table transforms, copies and several sets of codes",
       webp_seen.transform[0] && webp_seen.transform[2] && webp_seen.transform[3]
       && webp_seen.backref && webp_seen.meta);
    ok("  the normal loop filter, segments and subblock prediction",
       webp_seen.normal_filter && webp_seen.segments && webp_seen.bpred);
    ok("  lossless ALPH chunks under each of the four filters",
       webp_seen.alph_lossless && webp_seen.alph_filter[0] && webp_seen.alph_filter[1]
       && webp_seen.alph_filter[2] && webp_seen.alph_filter[3]);

    /* --- two parts of VP8 by themselves ------------------------------------------
     *
     * The simple loop filter, which the encoder never chooses, and the
     * normal one on steps worked out beforehand; and where each subblock's
     * edges come from, the last macroblock of a row above all. Each is
     * checked against values worked by hand from RFC 6386, sections 15 and
     * 12.3: this project's reading of the RFC, then, not a second decoder's,
     * but it is what holds them still. */
    puts("the loop filter and the subblock edges, worked from RFC 6386\n");
    {
        /* A step of 40: signed, p = -48 and q = -8, so w = (p1 - q1) + 3 (q0 - p0)
           = 80, and the three adjustments are (27 w + 63) >> 7 = 17,
           (18 w + 63) >> 7 = 11 and (9 w + 63) >> 7 = 6. */
        static const u8 STEP[8] = { 80, 80, 80, 80, 120, 120, 120, 120 };
        static const u8 STEP_MB[8] = { 80, 86, 91, 97, 103, 109, 114, 120 };
        set_segment(STEP);
        lf_mb_edge(&segment[0][4], 1, 8, 16, 193, 63, 0);
        ok("  the macroblock filter on a step of 40", segment_is(STEP_MB));

        /* High variance (p1 to p0 is 10, over 2): only p0 and q0 move, by
           the common adjustment: a = (p1 - q1) + 3 (q0 - p0) = -60 + 120 =
           60, q0 less (a + 4) >> 3 = 8, p0 plus (a + 3) >> 3 = 7. */
        static const u8 ROUGH[8] = { 80, 80, 70, 80, 120, 130, 120, 120 };
        static const u8 ROUGH_OUT[8] = { 80, 80, 70, 87, 112, 130, 120, 120 };
        set_segment(ROUGH);
        lf_mb_edge(&segment[0][4], 1, 8, 16, 193, 63, 2);
        ok("  and on high variance, only the two pixels at the edge", segment_is(ROUGH_OUT));
        set_segment(ROUGH);
        lf_inner_edge(&segment[0][4], 1, 8, 16, 193, 63, 2);
        ok("  and the subblock filter the same there", segment_is(ROUGH_OUT));

        /* The subblock filter on a step of 20, low variance: a = 3 * 20 = 60
           without the outer pixels, so q0 - 8 and p0 + 7, then q1 and p1 by
           (8 + 1) >> 1 = 4. */
        static const u8 STEP20[8] = { 100, 100, 100, 100, 120, 120, 120, 120 };
        static const u8 STEP20_SUB[8] = { 100, 100, 104, 107, 112, 116, 120, 120 };
        set_segment(STEP20);
        lf_inner_edge(&segment[0][4], 1, 8, 16, 100, 63, 0);
        ok("  the subblock filter on a step of 20", segment_is(STEP20_SUB));

        /* Too rough inside (p3 to p2 is 40, over 30): nothing moves. */
        static const u8 INSIDE[8] = { 100, 140, 100, 100, 120, 120, 120, 120 };
        set_segment(INSIDE);
        lf_mb_edge(&segment[0][4], 1, 8, 16, 193, 30, 0);
        ok("  and nothing when the pixels either side are too rough", segment_is(INSIDE));

        /* The simple filter: 2 |p0 - q0| + |p1 - q1| / 2 = 50 is at a limit of
           50 and over one of 49. With the outer pixels a = -20 + 60 = 40, so
           q0 - 5 and p0 + 5. */
        static const u8 SIMPLE_OUT[8] = { 100, 100, 100, 105, 115, 120, 120, 120 };
        set_segment(STEP20);
        lf_simple_edge(&segment[0][4], 1, 8, 50);
        ok("  the simple filter at its limit", segment_is(SIMPLE_OUT));
        set_segment(STEP20);
        lf_simple_edge(&segment[0][4], 1, 8, 49);
        ok("  and not one past it", segment_is(STEP20));

        /* The edges of subblocks in a frame of two by two macroblocks. */
        vp8_dec s;
        memset(&s, 0, sizeof s);
        u8 plane[32 * 32];
        s.mbw = s.mbh = 2;
        s.ys = 32;
        s.y = plane;
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) plane[y * 32 + x] = (u8)edge_px(x, y);
        /* Each: macroblock, subblock, then where A[0..3], A[4..7], L and P
           come from, as (x, y) or -1 for the row above the frame (127) and
           -2 for the column left of it (129). A[4..7] marked with x -3 is
           one pixel, (15 of the macroblock, the row above), four times. */
        static const int CASES[][12] = {
            /* mx my  i  A0x A0y  A4x A4y  Lx  Ly   Px  Py */
            { 0, 0,  0,  -1, 0,   -1, 0,   -2, 0,   -1, 0 },   /* the corner goes with the row */
            { 1, 0,  3,  -1, 0,   -1, 0,   27, 0,   -1, 0 },   /* top row, right side */
            { 0, 1,  0,   0, 15,   4, 15,  -2, 0,   -2, 0 },
            { 0, 1,  7,  12, 19,  16, 15,  11, 20,  11, 19 },  /* over and right: the row above the macroblock */
            { 1, 1,  3,  28, 15,  -3, 15,  27, 16,  27, 15 },  /* the last macroblock: repeated */
            { 1, 1, 15,  28, 27,  -3, 15,  27, 28,  27, 27 },
            { 1, 1,  5,  20, 19,  24, 19,  19, 20,  19, 19 },
        };
        int wrong = 0;
        for (unsigned c = 0; c < sizeof CASES / sizeof CASES[0]; c++) {
            const int *k = CASES[c];
            u8 A[8], L[4];
            int P;
            vp8_sub_edges(&s, k[0], k[1], k[2], A, L, &P);
            for (int j = 0; j < 4; j++) {
                int a0 = k[3] == -1 ? 127 : edge_px(k[3] + j, k[4]);
                int a4 = k[5] == -1 ? 127 : k[5] == -3 ? edge_px(k[0] * 16 + 15, k[6]) : edge_px(k[5] + j, k[6]);
                int l = k[7] == -2 ? 129 : edge_px(k[7], k[8] + j);
                wrong += (A[j] != a0) + (A[4 + j] != a4) + (L[j] != l);
            }
            int p0 = k[9] == -1 ? 127 : k[9] == -2 ? 129 : edge_px(k[9], k[10]);
            wrong += P != p0;
        }
        okn("  every subblock edge where section 12.3 puts it (wrong)", wrong == 0, wrong);
    }

    /* --- the lossless streams made here --------------------------------------- */
    puts("lossless streams assembled here\n");
    {
        u8 body[512], file[600];
        int size = wrap_lossless(file, body, make_lossless(body, MADE_GOOD));
        u64 before = heap_live();
        int rc = webp_decode(file, size, &p, 0x123456);
        okn("  decodes", rc == WEBP_OK, rc);
        if (rc == WEBP_OK) {
            ok("  ten by four", p.w == 10 && p.h == 4);
            int wrong = 0;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 10; x++) {
                    int i = MADE_INDEX[y][x];
                    u32 want = i < 3 ? MADE_TABLE[i] & 0xFFFFFF : 0x123456;
                    if (rgb_at(&p, x, y) != want) wrong++;
                }
            okn("  every pixel its colour from the table, four to a packed pixel (wrong)", wrong == 0, wrong);
            picture_free(&p);
        }
        ok("  through the colour cache", webp_seen.cache_hit);
        ok("  and every byte it took is given back", heap_live() == before);

        static const char *const WHY[] = { "", "a colour cache of 4096 is refused",
            "a version other than 0 is refused", "a transform used twice is refused",
            "a copy from before the first pixel is refused", "a code that is not whole is refused",
            "a copy one past the last pixel is refused" };
        for (int v = MADE_CACHE12; v <= MADE_OVERRUN; v++) {
            size = wrap_lossless(file, body, make_lossless(body, v));
            rc = webp_decode(file, size, &p, 0);
            okn(WHY[v], rc == WEBP_BAD, rc);
            picture_free(&p);
        }

        size = wrap_lossless(file, body, make_colour_transform(body));
        rc = webp_decode(file, size, &p, 0);
        okn("  under a colour transform, decodes", rc == WEBP_OK, rc);
        if (rc == WEBP_OK) {
            int wrong = 0;
            for (int i = 0; i < 8; i++)
                if (p.w != 4 || p.h != 2 || rgb_at(&p, i & 3, i >> 2) != ct_expected(i)) wrong++;
            okn("  with red and blue put back as section 3.5.2 says (wrong)", wrong == 0, wrong);
            picture_free(&p);
        }
        ok("  through the colour transform", webp_seen.transform[1]);

        u8 pbody[1024], pfile[1100];
        size = wrap_lossless(pfile, pbody, make_predictor(pbody));
        rc = webp_decode(pfile, size, &p, 0x123456);
        okn("  under the predictor, all fourteen modes, decodes", rc == WEBP_OK, rc);
        if (rc == WEBP_OK) {
            u32 want[256];
            pr_expected(want);
            int wrong = 0;
            for (int i = 0; i < 256; i++) {
                /* Laid over the page the way webp.h lays any pixel. */
                int a = (int)(want[i] >> 24), rgb[3];
                for (int c = 0; c < 3; c++) {
                    int v = pr_channel(want[i], 2 - c), bgc = (0x123456 >> ((2 - c) * 8)) & 0xFF;
                    rgb[c] = a == 255 ? v : (v * a + bgc * (255 - a)) / 255;
                }
                u32 expect = (u32)rgb[0] << 16 | (u32)rgb[1] << 8 | (u32)rgb[2];
                if (p.w != 16 || p.h != 16 || rgb_at(&p, i & 15, i >> 4) != expect) wrong++;
            }
            okn("  with every pixel as section 3.5.1's predictors say (wrong)", wrong == 0, wrong);
            picture_free(&p);
        }
    }

    /* --- raw alpha, under each filter ---------------------------------------- */
    puts("raw ALPH chunks made here around scene 0.9\n");
    {
        int vsize;
        const u8 *vp8 = find_chunk(scene->file, scene->n, "VP8 ", &vsize, 0);
        int w = scene->ref->w, h = scene->ref->h;
        u8 *a = (u8 *)malloc((u64)w * h);
        u8 *alph = (u8 *)malloc((u64)w * h + 1);
        u8 *file = (u8 *)malloc((u64)w * h + (u64)vsize + 64);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) a[y * w + x] = (u8)made_alpha(x, y);
        picture plain;
        int prc = webp_decode(scene->file, scene->n, &plain, 0);
        static const char *const NAMES[4] = {
            "  unfiltered: every pixel's alpha exact (wrong)",
            "  filtered from the left: every pixel's alpha exact (wrong)",
            "  filtered from above: every pixel's alpha exact (wrong)",
            "  filtered by gradient: every pixel's alpha exact (wrong)" };
        for (int f = 0; f < 4; f++) {
            alph[0] = (u8)(f << 2);              /* stored raw, filter f */
            filter_alpha(a, alph + 1, w, h, f);
            int at = 12;
            at += put_vp8x(file + at, 0x10, w, h);
            at += put_chunk(file + at, "ALPH", alph, w * h + 1);
            at += put_chunk(file + at, "VP8 ", vp8, vsize);
            put_riff(file, at);
            picture black, white;
            int b1 = webp_decode(file, at, &black, 0x000000);
            int b2 = webp_decode(file, at, &white, 0xFFFFFF);
            int wrong = -1, colour = 0;
            if (vp8 && b1 == WEBP_OK && b2 == WEBP_OK && prc == WEBP_OK) {
                wrong = 0;
                for (int i = 0; i < w * h; i++) {
                    if (255 - (white.rgb[i * 3 + 1] - black.rgb[i * 3 + 1]) != a[i]) wrong++;
                    /* Where it is opaque, the colours are the scene's own. */
                    if (a[i] == 255 && (black.rgb[i * 3] != plain.rgb[i * 3]
                                        || black.rgb[i * 3 + 2] != plain.rgb[i * 3 + 2])) colour++;
                }
            }
            okn(NAMES[f], wrong == 0, wrong);
            if (f == 3) okn("  and the colours where it is opaque unchanged (wrong)", colour == 0, colour);
            picture_free(&black);
            picture_free(&white);
        }
        picture_free(&plain);
        free(a); free(alph); free(file);
    }

    /* --- the container ------------------------------------------------------- */
    puts("containers made here around photo 1\n");
    {
        int lsize, vsize;
        const u8 *vp8l = find_chunk(photo->file, photo->n, "VP8L", &lsize, 0);
        const u8 *vp8 = find_chunk(scene->file, scene->n, "VP8 ", &vsize, 0);
        int pw = photo->ref->w, ph = photo->ref->h;
        u8 *file = (u8 *)malloc((u64)lsize + (u64)vsize + 256);

        /* The simple form of the same picture, which the encoder does not
           write when it has a colour profile to add. */
        int at = wrap_lossless(file, vp8l, lsize);
        int rc = webp_decode(file, at, &p, WEBP_REF_BG);
        ok("  RIFF then VP8L alone decodes, every pixel exact",
           rc == WEBP_OK && fnv(p.rgb, p.w * p.h * 3) == photo->ref->rgb_hash);
        picture_free(&p);

        at = 12;
        at += put_vp8x(file + at, 0x10, pw + 1, ph);   /* a canvas one wider than the picture */
        at += put_chunk(file + at, "VP8L", vp8l, lsize);
        put_riff(file, at);
        rc = webp_decode(file, at, &p, 0);
        okn("  and a canvas wider than its picture is refused", rc == WEBP_BAD, rc);
        picture_free(&p);
        put24(file + 12 + 8 + 4, (u32)(pw - 2));        /* and one narrower */
        rc = webp_decode(file, at, &p, 0);
        okn("  and one narrower, which the picture would overrun", rc == WEBP_BAD, rc);
        picture_free(&p);

        /* An animation: a 300 x 250 canvas, the first frame at (80, 60),
           a second frame after it that must not be drawn. */
        at = 12;
        at += put_vp8x(file + at, 0x12, 300, 250);
        u8 anim[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 0 };
        at += put_chunk(file + at, "ANIM", anim, 6);
        int frame = at;
        for (int i = 0; i < 4; i++) file[at + i] = (u8)"ANMF"[i];
        u8 *hdr = file + at + 8;
        put24(hdr, 40); put24(hdr + 3, 30);
        put24(hdr + 6, (u32)(pw - 1)); put24(hdr + 9, (u32)(ph - 1));
        put24(hdr + 12, 100); hdr[15] = 0;
        int inner = 16 + put_chunk(hdr + 16, "VP8L", vp8l, lsize);
        put32(file + frame + 4, (u32)inner);
        at += 8 + inner;
        for (int i = 0; i < 4; i++) file[at + i] = (u8)"ANMF"[i];
        hdr = file + at + 8;
        put24(hdr, 0); put24(hdr + 3, 0);
        put24(hdr + 6, (u32)(scene->ref->w - 1)); put24(hdr + 9, (u32)(scene->ref->h - 1));
        put24(hdr + 12, 100); hdr[15] = 0;
        inner = 16 + put_chunk(hdr + 16, "VP8 ", vp8, vsize);
        put32(file + at + 4, (u32)inner);
        at += 8 + inner;
        put_riff(file, at);
        rc = webp_decode(file, at, &p, WEBP_REF_BG);
        okn("  an animation decodes", rc == WEBP_OK, rc);
        if (rc == WEBP_OK) {
            ok("  at the canvas's size", p.w == 300 && p.h == 250);
            u8 *region = (u8 *)malloc((u64)pw * ph * 3);
            for (int y = 0; y < ph && p.w == 300; y++)
                for (int x = 0; x < pw * 3; x++) region[y * pw * 3 + x] = p.rgb[((y + 60) * 300 + 80) * 3 + x];
            ok("  with its first frame where the frame says, every pixel exact",
               fnv(region, pw * ph * 3) == photo->ref->rgb_hash);
            free(region);
            int around = 0;
            for (int y = 0; y < p.h; y++)
                for (int x = 0; x < p.w; x++)
                    if ((x < 80 || x >= 80 + pw || y < 60 || y >= 60 + ph) && rgb_at(&p, x, y) != WEBP_REF_BG)
                        around++;
            okn("  and the page around it, not the second frame (wrong)", around == 0, around);
            picture_free(&p);
        }
        put24(file + frame + 8, 60);                 /* the frame out past the canvas */
        rc = webp_decode(file, at, &p, 0);
        okn("  and a frame that does not fit the canvas is refused", rc == WEBP_BAD, rc);
        picture_free(&p);
        free(file);
    }

    /* --- refusals ------------------------------------------------------------ */
    puts("what is not a picture\n");
    {
        static const u8 WAVE[16] = { 'R', 'I', 'F', 'F', 8, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' };
        static const u8 PNG[16] = { 137, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
        okn("  a RIFF file that is not a WebP is refused", webp_decode(WAVE, 16, &p, 0) == WEBP_NOT_WEBP,
            webp_decode(WAVE, 16, &p, 0));
        okn("  and a PNG", webp_decode(PNG, 16, &p, 0) == WEBP_NOT_WEBP, webp_decode(PNG, 16, &p, 0));
        u8 file[64];
        for (int i = 0; i < 64; i++) file[i] = 0;
        int at = 12 + put_chunk(file + 12, "ABCD", file + 40, 4);
        put_riff(file, at);
        okn("  and a WebP of a chunk nobody knows", webp_decode(file, at, &p, 0) == WEBP_BAD,
            webp_decode(file, at, &p, 0));

        /* Too big, by the lossless header and by the canvas: 5000 x 1,
           which is too wide for a page but small enough to hold, so the
           limit is the only thing that can refuse it. */
        u8 body[16];
        for (int i = 0; i < 16; i++) body[i] = 0;
        bitw hw = { body, 0, 0, 0 };
        bw_put(&hw, 0x2F, 8);
        bw_put(&hw, 5000 - 1, 14);
        bw_put(&hw, 0, 14);
        at = wrap_lossless(file, body, 16);
        okn("  a lossless picture 5000 wide is refused as too big",
            webp_decode(file, at, &p, 0) == WEBP_TOO_BIG, webp_decode(file, at, &p, 0));
        /* The canvas alone too big: the picture in it is the small one made
           above, so nothing else can be what refuses it. */
        u8 small[512], big[600];
        int sn = make_lossless(small, MADE_GOOD);
        at = 12 + put_vp8x(big + 12, 0, 5000, 5000);
        at += put_chunk(big + at, "VP8L", small, sn);
        put_riff(big, at);
        okn("  and a canvas of 5000 x 5000", webp_decode(big, at, &p, 0) == WEBP_TOO_BIG,
            webp_decode(big, at, &p, 0));

        /* A VP8 frame that is not a key frame. */
        int vsize, vat;
        find_chunk(scene->file, scene->n, "VP8 ", &vsize, &vat);
        u8 *copy = (u8 *)malloc((u64)scene->n);
        for (int i = 0; i < scene->n; i++) copy[i] = scene->file[i];
        copy[vat + 8] |= 1;
        int rc = webp_decode(copy, scene->n, &p, 0);
        okn("  a VP8 frame that is not a key frame is refused by name", rc == WEBP_UNSUPPORTED, rc);
        copy[vat + 8] &= 0xFE;
        copy[vat + 8 + 3] = 0;                       /* the start code */
        rc = webp_decode(copy, scene->n, &p, 0);
        okn("  and one without its start code", rc == WEBP_BAD, rc);
        free(copy);
    }

    /* --- cut short ------------------------------------------------------------ */
    puts("files cut short\n");
    {
        /* The last two cuts are two and three bytes short: one short can be
           only the pad byte after an odd-sized chunk, which is no loss. */
        const webp_file *three[3] = { scene, alpha, photo };
        int all = 0, tried = 0;
        for (int f = 0; f < 3; f++) {
            int n = three[f]->n;
            int cuts[12] = { 0, 7, 11, 12, 19, 20, 30, n / 4, n / 2, n * 3 / 4, n - 3, n - 2 };
            for (int c = 0; c < 12; c++, tried++) {
                guard g;
                u8 *in = guarded(three[f]->file, cuts[c], &g);
                if (in && webp_decode(in, cuts[c], &p, 0) == WEBP_TRUNCATED) all++;
                picture_free(&p);
                unguard(&g);
            }
        }
        okn("  at twelve places in each of three files, every one says so, reading nothing past the cut",
            all == tried, all);

        /* Cut in the middle and the sizes written to match, so the only
           thing that knows is the decoder reading the stream: the bytes
           after the cut are still there, and a decoder that read them would
           decode the whole picture. */
        int lsize, lat;
        find_chunk(photo->file, photo->n, "VP8L", &lsize, &lat);
        u8 *copy = (u8 *)malloc((u64)photo->n);
        for (int i = 0; i < photo->n; i++) copy[i] = photo->file[i];
        int cut = lat + 8 + lsize / 2;
        put32(copy + 4, (u32)(cut - 8));
        put32(copy + lat + 4, (u32)(lsize / 2));
        int rc = webp_decode(copy, cut, &p, 0);
        okn("  a lossless stream that stops half way is cut short", rc == WEBP_TRUNCATED, rc);
        picture_free(&p);
        free(copy);

        int vsize, vat;
        find_chunk(scene->file, scene->n, "VP8 ", &vsize, &vat);
        copy = (u8 *)malloc((u64)scene->n);
        for (int i = 0; i < scene->n; i++) copy[i] = scene->file[i];
        cut = vat + 8 + vsize * 2 / 3;
        put32(copy + 4, (u32)(cut - 8));
        put32(copy + vat + 4, (u32)(vsize * 2 / 3));
        rc = webp_decode(copy, cut, &p, WEBP_REF_BG);
        okn("  a lossy one cut in its coefficients still decodes", rc == WEBP_OK, rc);
        if (rc == WEBP_OK) {
            /* The top is right; the bottom is not, having been read from
               nothing (the zeros past the end of a partition). */
            int rn, top = 0, bottom = 0, k = 0;
            u8 *ref = unbase64(scene->ref->rgb, &rn);
            int step = scene->ref->step;
            for (int y = 0; y < p.h; y += step)
                for (int x = 0; x < p.w; x += step, k++)
                    for (int ch = 0; ch < 3; ch++) {
                        int e = p.rgb[(y * p.w + x) * 3 + ch] - ref[k * 3 + ch];
                        if (e < 0) e = -e;
                        if (y < 32 && e > WEBP_TOLERANCE) top++;
                        if (y >= p.h - 32 && e > 16) bottom++;
                    }
            okn("  right above the cut (samples wrong)", top == 0, top);
            okn("  and not drawn from past it (samples far off)", bottom > 200, bottom);
            free(ref);
            picture_free(&p);
        }
        free(copy);
    }

    /* --- damaged ---------------------------------------------------------------- */
    puts("files damaged in the middle\n");
    {
        const webp_file *three[3] = { scene, alpha, photo };
        u32 seed = 12345;
        int tries = 0, heads = 0, head_sane = 0, body_sane = 0, intact = 0, returned = 0, errors = 0;
        for (int f = 0; f < 3; f++) {
            int n = three[f]->n, psize, pat;
            if (!find_chunk(three[f]->file, n, "VP8 ", &psize, &pat))
                find_chunk(three[f]->file, n, "VP8L", &psize, &pat);
            u8 *copy = (u8 *)malloc((u64)n);
            for (int t = 0; t < 40; t++, tries++) {
                for (int i = 0; i < n; i++) copy[i] = three[f]->file[i];
                /* One to four bytes: for the first ten, in the 64 from the
                   picture chunk's header, where its sizes and modes are; for
                   the rest, anywhere else past the canvas header. */
                int head = t < 10;
                seed = seed * 1103515245u + 12345u;
                int flips = 1 + (int)((seed >> 16) & 3);
                for (int k = 0; k < flips; k++) {
                    seed = seed * 1103515245u + 12345u;
                    int at;
                    if (head) at = pat + (int)((seed >> 8) % 64u);
                    else {
                        at = 30 + (int)((seed >> 8) % (u32)(n - 30 - 64));
                        if (at >= pat) at += 64;
                    }
                    seed = seed * 1103515245u + 12345u;
                    copy[at] ^= (u8)(1 + ((seed >> 16) % 255));
                }
                u64 before = heap_live();
                guard g;
                u8 *in = guarded(copy, n, &g);
                int rc = in ? webp_decode(in, n, &p, 0) : WEBP_TOO_BIG;
                unguard(&g);
                int fine;
                if (rc == WEBP_OK) {
                    /* A damaged header can say another size, and that is
                       then the picture's size; damage past it cannot. */
                    fine = head ? p.w >= 1 && p.h >= 1 && p.w <= WEBP_MAX_SIDE && p.h <= WEBP_MAX_SIDE && p.rgb
                                : p.w == three[f]->ref->w && p.h == three[f]->ref->h;
                } else {
                    fine = rc < 0 && rc >= WEBP_BAD && !p.rgb;
                    errors++;
                }
                if (head) { heads++; head_sane += fine; } else body_sane += fine;
                /* Walked before the picture is freed: freeing a block writes
                   its size at its foot again, which would hide an overrun of
                   the picture itself. */
                intact += heap_intact();
                picture_free(&p);
                returned += heap_live() == before;
            }
            free(copy);
        }
        okn("  in the picture's headers: an error, or a picture the size they now say (of 30)",
            head_sane == heads, head_sane);
        okn("  elsewhere: an error, or a picture of its own size (of 90)", body_sane == tries - heads, body_sane);
        okn("  and none writes anywhere it should not (heaps intact, of 120)", intact == tries, intact);
        okn("  and each gives back everything it took (of 120)", returned == tries, returned);
        note("of which errors", errors, "");
    }

    /* --- how long it takes --------------------------------------------------------- */
    {
        int t0 = ticks();
        for (int i = 0; i < 50; i++) { webp_decode(scene->file, scene->n, &p, 0); picture_free(&p); }
        int t1 = ticks();
        for (int i = 0; i < 50; i++) { webp_decode(photo->file, photo->n, &p, 0); picture_free(&p); }
        int t2 = ticks();
        note("scene 0.9 (208 x 157 lossy), fifty decodes in", (t1 - t0) * 10, " ms");
        note("photo 1 (192 x 144 lossless), fifty decodes in", (t2 - t1) * 10, " ms");
    }

    for (int i = 0; i < WEBP_REF_COUNT; i++) free(files[i].file);
    puts(failed ? "WEBPTEST_FAIL\n" : "WEBPTEST_PASS\n");
    return failed;
}
