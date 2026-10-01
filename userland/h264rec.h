/* What a macroblock's syntax becomes: its prediction, intra (8.3) or from
 * other pictures (8.4), its residual through the inverse transforms (8.5),
 * and, once the picture is whole, the deblocking filter (8.7). Part of
 * h264.h.
 *
 * Blocks are kept in raster order within their macroblock (the standard
 * numbers them by 8x8 quarters; H264_BLK_R and H264_R_BLK turn one into
 * the other). Pictures have no border: a reference sample outside one is
 * the nearest edge sample, which is what the standard says it is.
 */

static const u8 H264_BLK_X[16] = { 0, 4, 0, 4, 8, 12, 8, 12, 0, 4, 0, 4, 8, 12, 8, 12 };
static const u8 H264_BLK_Y[16] = { 0, 0, 4, 4, 0, 0, 4, 4, 8, 8, 12, 12, 8, 8, 12, 12 };
static const u8 H264_BLK_R[16] = { 0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15 };  /* both ways */
#define H264_R_BLK H264_BLK_R

static inline u8 h264_clip1(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void h264_scales(h264_dec *d) {
    const h264_pps *pp = d->pp;
    for (int l = 0; l < 6; l++)
        for (int q = 0; q < 6; q++)
            for (int i = 0; i < 16; i++) d->ls4[l][q][i] = pp->sl4[l][i] * H264_DEQUANT4[q * 16 + i];
    for (int l = 0; l < 2; l++)
        for (int q = 0; q < 6; q++)
            for (int i = 0; i < 64; i++) d->ls8[l][q][i] = pp->sl8[l][i] * H264_DEQUANT8[q * 64 + i];
}

/* 8.5.12.1 and 8.5.13.1: one level, scaled. */
/* The standard scales up with a left shift, which in C is undefined for a
   negative level; a multiplication by the power of two is the same number. */
static inline int h264_dq4(const int *ls, int c, int qp, int i) {
    if (qp >= 24) return (c * ls[i]) * (1 << (qp / 6 - 4));
    return (c * ls[i] + (1 << (3 - qp / 6))) >> (4 - qp / 6);
}
static inline int h264_dq8(const int *ls, int c, int qp, int i) {
    if (qp >= 36) return (c * ls[i]) * (1 << (qp / 6 - 6));
    return (c * ls[i] + (1 << (5 - qp / 6))) >> (6 - qp / 6);
}

static int h264_qpc(const h264_dec *d, int qpy, int c) {
    int q = h264_clip3(0, 51, qpy + d->pp->chroma_qp_offset[c]);
    return H264_QP_CHROMA[q];
}

/* --- the inverse transforms (8.5.12, 8.5.13) --------------------------------------------- */

static void h264_idct4_add(const short *c, u8 *dst, int stride) {
    int t[16];
    for (int i = 0; i < 4; i++) {
        int d0 = c[i * 4], d1 = c[i * 4 + 1], d2 = c[i * 4 + 2], d3 = c[i * 4 + 3];
        int e0 = d0 + d2, e1 = d0 - d2, e2 = (d1 >> 1) - d3, e3 = d1 + (d3 >> 1);
        t[i * 4] = e0 + e3; t[i * 4 + 1] = e1 + e2; t[i * 4 + 2] = e1 - e2; t[i * 4 + 3] = e0 - e3;
    }
    for (int j = 0; j < 4; j++) {
        int f0 = t[j], f1 = t[4 + j], f2 = t[8 + j], f3 = t[12 + j];
        int g0 = f0 + f2, g1 = f0 - f2, g2 = (f1 >> 1) - f3, g3 = f1 + (f3 >> 1);
        int h0 = g0 + g3, h1 = g1 + g2, h2 = g1 - g2, h3 = g0 - g3;
        dst[j] = h264_clip1(dst[j] + ((h0 + 32) >> 6));
        dst[stride + j] = h264_clip1(dst[stride + j] + ((h1 + 32) >> 6));
        dst[2 * stride + j] = h264_clip1(dst[2 * stride + j] + ((h2 + 32) >> 6));
        dst[3 * stride + j] = h264_clip1(dst[3 * stride + j] + ((h3 + 32) >> 6));
    }
}

static void h264_idct8_1d(const int *d, int *g, int step) {
    int d0 = d[0], d1 = d[step], d2 = d[2 * step], d3 = d[3 * step];
    int d4 = d[4 * step], d5 = d[5 * step], d6 = d[6 * step], d7 = d[7 * step];
    int e0 = d0 + d4, e1 = -d3 + d5 - d7 - (d7 >> 1), e2 = d0 - d4, e3 = d1 + d7 - d3 - (d3 >> 1);
    int e4 = (d2 >> 1) - d6, e5 = -d1 + d7 + d5 + (d5 >> 1), e6 = d2 + (d6 >> 1), e7 = d3 + d5 + d1 + (d1 >> 1);
    int f0 = e0 + e6, f1 = e1 + (e7 >> 2), f2 = e2 + e4, f3 = e3 + (e5 >> 2);
    int f4 = e2 - e4, f5 = (e3 >> 2) - e5, f6 = e0 - e6, f7 = e7 - (e1 >> 2);
    g[0] = f0 + f7; g[step] = f2 + f5; g[2 * step] = f4 + f3; g[3 * step] = f6 + f1;
    g[4 * step] = f6 - f1; g[5 * step] = f4 - f3; g[6 * step] = f2 - f5; g[7 * step] = f0 - f7;
}

static void h264_idct8_add(const short *c, u8 *dst, int stride) {
    int a[64], b[64];
    for (int i = 0; i < 64; i++) a[i] = c[i];
    for (int i = 0; i < 8; i++) h264_idct8_1d(a + i * 8, b + i * 8, 1);
    for (int j = 0; j < 8; j++) h264_idct8_1d(b + j, a + j, 8);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) dst[y * stride + x] = h264_clip1(dst[y * stride + x] + ((a[y * 8 + x] + 32) >> 6));
}

/* 8.5.10: the luma DC of a 16x16 prediction, c raster over the 4x4 grid of
   blocks; out is each block's DC, scaled. */
static void h264_luma_dc(const short *c, int *out, const int *ls, int qp) {
    int t[16], f[16];
    for (int i = 0; i < 4; i++) {
        int a = c[i * 4], b = c[i * 4 + 1], e = c[i * 4 + 2], g = c[i * 4 + 3];
        t[i * 4] = a + b + e + g; t[i * 4 + 1] = a + b - e - g;
        t[i * 4 + 2] = a - b - e + g; t[i * 4 + 3] = a - b + e - g;
    }
    for (int j = 0; j < 4; j++) {
        int a = t[j], b = t[4 + j], e = t[8 + j], g = t[12 + j];
        f[j] = a + b + e + g; f[4 + j] = a + b - e - g;
        f[8 + j] = a - b - e + g; f[12 + j] = a - b + e - g;
    }
    for (int i = 0; i < 16; i++) {
        if (qp >= 36) out[i] = (f[i] * ls[0]) * (1 << (qp / 6 - 6));
        else out[i] = (f[i] * ls[0] + (1 << (5 - qp / 6))) >> (6 - qp / 6);
    }
}

/* 8.5.11: chroma DC for 4:2:0. */
static void h264_chroma_dc(const short *c, int *out, const int *ls, int qp) {
    int f0 = c[0] + c[1] + c[2] + c[3], f1 = c[0] - c[1] + c[2] - c[3];
    int f2 = c[0] + c[1] - c[2] - c[3], f3 = c[0] - c[1] - c[2] + c[3];
    out[0] = ((f0 * ls[0]) * (1 << (qp / 6))) >> 5;
    out[1] = ((f1 * ls[0]) * (1 << (qp / 6))) >> 5;
    out[2] = ((f2 * ls[0]) * (1 << (qp / 6))) >> 5;
    out[3] = ((f3 * ls[0]) * (1 << (qp / 6))) >> 5;
}

/* --- neighbours ------------------------------------------------------------------------------- */

/* Whether macroblock addr can be used by the one being decoded: in the
   picture, in this slice, already decoded. */
static inline int h264_mb_ok(const h264_dec *d, int addr) {
    return addr >= 0 && addr < d->mbs && d->mb_slice[addr] == d->sl.number;
}

/* The macroblock a luma location (x, y) relative to the current one falls
   in, x and y from -1 to 16; -1 when not available. *blk is the raster 4x4
   block within it. */
static int h264_nb(const h264_dec *d, int x, int y, int *blk) {
    const h264_mbctx *m = &d->m;
    int addr;
    if (y < 0) {
        if (x < 0) addr = m->x > 0 && m->y > 0 ? m->addr - d->mb_w - 1 : -1;
        else if (x < 16) addr = m->y > 0 ? m->addr - d->mb_w : -1;
        else addr = m->y > 0 && m->x < d->mb_w - 1 ? m->addr - d->mb_w + 1 : -1;
    } else if (y < 16) {
        if (x < 0) addr = m->x > 0 ? m->addr - 1 : -1;
        else if (x < 16) addr = m->addr;
        else return -1;
    } else {
        return -1;
    }
    if (addr < 0 || (addr != m->addr && !h264_mb_ok(d, addr))) return -1;
    *blk = (((y + 16) & 15) >> 2) * 4 + (((x + 16) & 15) >> 2);
    return addr;
}

/* --- intra prediction (8.3) ------------------------------------------------------------------ */

/* For intra prediction a neighbour coded by inter prediction is not there
   when the stream asks for constrained intra prediction. */
static inline int h264_intra_ok(const h264_dec *d, int addr) {
    if (addr < 0) return 0;
    if (addr == d->m.addr) return 1;
    return !(d->pp->constrained_intra && !(d->mb_kind[addr] & H264_K_INTRA));
}

/* p[-1,-1], p[0..15,-1] and p[-1,0..15] of an NxN block at luma (bx, by)
   in the current macroblock, with which of them there are. */
typedef struct { int a, b, c, dd; u8 top[16], left[16], tl; } h264_edge;

static void h264_luma_edge(h264_dec *d, int bx, int by, int n, h264_edge *e) {
    int W = d->mb_w * 16;
    int px = d->m.x * 16 + bx, py = d->m.y * 16 + by;
    const u8 *pic = d->cur->y;
    int blk, addr;
    addr = h264_nb(d, bx - 1, by, &blk);
    e->a = h264_intra_ok(d, addr);
    addr = h264_nb(d, bx, by - 1, &blk);
    e->b = h264_intra_ok(d, addr);
    addr = h264_nb(d, bx - 1, by - 1, &blk);
    e->dd = h264_intra_ok(d, addr);
    addr = h264_nb(d, bx + n, by - 1, &blk);
    e->c = h264_intra_ok(d, addr);
    if (e->c && addr == d->m.addr) {
        /* Inside this macroblock: there only if decoded already, which for a
           4x4 or 8x8 block depends on where it falls in decoding order. */
        int cur = n == 4 ? H264_R_BLK[(by >> 2) * 4 + (bx >> 2)] : ((by >> 3) * 2 + (bx >> 3)) * 4;
        int other = H264_R_BLK[blk];
        if (n == 8) other = (other >> 2) << 2;
        e->c = other < cur;
    }
    if (e->b) {
        for (int i = 0; i < n; i++) e->top[i] = pic[(py - 1) * W + px + i];
        /* Above and to the right, for the 4x4 and 8x8 modes that lean that
           way; what is not there repeats the last sample that is. */
        if (n < 16) {
            if (e->c) for (int i = n; i < 2 * n; i++) e->top[i] = pic[(py - 1) * W + px + i];
            else for (int i = n; i < 2 * n; i++) e->top[i] = e->top[n - 1];
        }
    }
    if (e->a) for (int i = 0; i < n; i++) e->left[i] = pic[(py + i) * W + px - 1];
    if (e->dd) e->tl = pic[(py - 1) * W + px - 1];
}

static void h264_intra4(h264_dec *d, int bx, int by, int mode) {
    h264_edge e;
    h264_luma_edge(d, bx, by, 4, &e);
    int W = d->mb_w * 16;
    u8 *dst = d->cur->y + (d->m.y * 16 + by) * W + d->m.x * 16 + bx;
    /* One run of edge samples, p[-1,3] .. p[-1,0], p[-1,-1], p[0,-1] .. p[7,-1]. */
    int E[13];
    for (int i = 0; i < 4; i++) E[3 - i] = e.left[i];
    E[4] = e.tl;
    for (int i = 0; i < 8; i++) E[5 + i] = e.top[i];
#define P_TOP(x) E[5 + (x)]
#define P_LEFT(y) E[3 - (y)]
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int v = 128;
            switch (mode) {
            case 0: v = P_TOP(x); break;
            case 1: v = P_LEFT(y); break;
            case 2: {
                int s = 0;
                if (e.a && e.b) { for (int i = 0; i < 4; i++) s += P_TOP(i) + P_LEFT(i); v = (s + 4) >> 3; }
                else if (e.a) { for (int i = 0; i < 4; i++) s += P_LEFT(i); v = (s + 2) >> 2; }
                else if (e.b) { for (int i = 0; i < 4; i++) s += P_TOP(i); v = (s + 2) >> 2; }
                break;
            }
            case 3:
                v = (x == 3 && y == 3) ? (P_TOP(6) + 3 * P_TOP(7) + 2) >> 2
                                       : (P_TOP(x + y) + 2 * P_TOP(x + y + 1) + P_TOP(x + y + 2) + 2) >> 2;
                break;
            case 4: v = (E[3 + x - y] + 2 * E[4 + x - y] + E[5 + x - y] + 2) >> 2; break;
            case 5: {
                int z = 2 * x - y;
                if (z >= 0 && !(z & 1)) v = (P_TOP(x - (y >> 1) - 1) + P_TOP(x - (y >> 1)) + 1) >> 1;
                else if (z > 0) v = (P_TOP(x - (y >> 1) - 2) + 2 * P_TOP(x - (y >> 1) - 1) + P_TOP(x - (y >> 1)) + 2) >> 2;
                else if (z == -1) v = (P_LEFT(0) + 2 * E[4] + P_TOP(0) + 2) >> 2;
                else v = (P_LEFT(y - 1) + 2 * P_LEFT(y - 2) + P_LEFT(y - 3) + 2) >> 2;
                break;
            }
            case 6: {
                int z = 2 * y - x;
                if (z >= 0 && !(z & 1)) v = (P_LEFT(y - (x >> 1) - 1) + P_LEFT(y - (x >> 1)) + 1) >> 1;
                else if (z > 0) v = (P_LEFT(y - (x >> 1) - 2) + 2 * P_LEFT(y - (x >> 1) - 1) + P_LEFT(y - (x >> 1)) + 2) >> 2;
                else if (z == -1) v = (P_LEFT(0) + 2 * E[4] + P_TOP(0) + 2) >> 2;
                else v = (P_TOP(x - 1) + 2 * P_TOP(x - 2) + P_TOP(x - 3) + 2) >> 2;
                break;
            }
            case 7:
                if (!(y & 1)) v = (P_TOP(x + (y >> 1)) + P_TOP(x + (y >> 1) + 1) + 1) >> 1;
                else v = (P_TOP(x + (y >> 1)) + 2 * P_TOP(x + (y >> 1) + 1) + P_TOP(x + (y >> 1) + 2) + 2) >> 2;
                break;
            case 8: {
                int z = x + 2 * y;
                if (z > 5) v = P_LEFT(3);
                else if (z == 5) v = (P_LEFT(2) + 3 * P_LEFT(3) + 2) >> 2;
                else if (!(z & 1)) v = (P_LEFT(y + (x >> 1)) + P_LEFT(y + (x >> 1) + 1) + 1) >> 1;
                else v = (P_LEFT(y + (x >> 1)) + 2 * P_LEFT(y + (x >> 1) + 1) + P_LEFT(y + (x >> 1) + 2) + 2) >> 2;
                break;
            }
            }
            dst[y * W + x] = (u8)v;
        }
    }
#undef P_TOP
#undef P_LEFT
}

static void h264_intra8(h264_dec *d, int bx, int by, int mode) {
    h264_edge e;
    h264_luma_edge(d, bx, by, 8, &e);
    int W = d->mb_w * 16;
    u8 *dst = d->cur->y + (d->m.y * 16 + by) * W + d->m.x * 16 + bx;
    /* 8.3.2.2.1: the reference samples are filtered first. */
    int t[16], l[8], tl = 0;
    if (e.b) {
        t[0] = e.dd ? (e.tl + 2 * e.top[0] + e.top[1] + 2) >> 2 : (3 * e.top[0] + e.top[1] + 2) >> 2;
        for (int x = 1; x < 15; x++) t[x] = (e.top[x - 1] + 2 * e.top[x] + e.top[x + 1] + 2) >> 2;
        t[15] = (e.top[14] + 3 * e.top[15] + 2) >> 2;
    }
    if (e.dd) {
        if (!e.b || !e.a) {
            if (e.b) tl = (3 * e.tl + e.top[0] + 2) >> 2;
            else if (e.a) tl = (3 * e.tl + e.left[0] + 2) >> 2;
            else tl = e.tl;
        } else {
            tl = (e.top[0] + 2 * e.tl + e.left[0] + 2) >> 2;
        }
    }
    if (e.a) {
        l[0] = e.dd ? (e.tl + 2 * e.left[0] + e.left[1] + 2) >> 2 : (3 * e.left[0] + e.left[1] + 2) >> 2;
        for (int y = 1; y < 7; y++) l[y] = (e.left[y - 1] + 2 * e.left[y] + e.left[y + 1] + 2) >> 2;
        l[7] = (e.left[6] + 3 * e.left[7] + 2) >> 2;
    }
    /* p'[-1,7] .. p'[-1,0], p'[-1,-1], p'[0,-1] .. p'[15,-1] */
    int E[25];
    for (int i = 0; i < 8; i++) E[7 - i] = e.a ? l[i] : 0;
    E[8] = tl;
    for (int i = 0; i < 16; i++) E[9 + i] = e.b ? t[i] : 0;
#define P_TOP(x) E[9 + (x)]
#define P_LEFT(y) E[7 - (y)]
    int dc = 128;
    if (mode == 2) {
        int s = 0;
        if (e.a && e.b) { for (int i = 0; i < 8; i++) s += P_TOP(i) + P_LEFT(i); dc = (s + 8) >> 4; }
        else if (e.a) { for (int i = 0; i < 8; i++) s += P_LEFT(i); dc = (s + 4) >> 3; }
        else if (e.b) { for (int i = 0; i < 8; i++) s += P_TOP(i); dc = (s + 4) >> 3; }
    }
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int v = dc;
            switch (mode) {
            case 0: v = P_TOP(x); break;
            case 1: v = P_LEFT(y); break;
            case 3:
                v = (x == 7 && y == 7) ? (P_TOP(14) + 3 * P_TOP(15) + 2) >> 2
                                       : (P_TOP(x + y) + 2 * P_TOP(x + y + 1) + P_TOP(x + y + 2) + 2) >> 2;
                break;
            case 4: v = (E[7 + x - y] + 2 * E[8 + x - y] + E[9 + x - y] + 2) >> 2; break;
            case 5: {
                int z = 2 * x - y;
                if (z >= 0 && !(z & 1)) v = (P_TOP(x - (y >> 1) - 1) + P_TOP(x - (y >> 1)) + 1) >> 1;
                else if (z > 0) v = (P_TOP(x - (y >> 1) - 2) + 2 * P_TOP(x - (y >> 1) - 1) + P_TOP(x - (y >> 1)) + 2) >> 2;
                else if (z == -1) v = (P_LEFT(0) + 2 * E[8] + P_TOP(0) + 2) >> 2;
                else v = (P_LEFT(y - 2 * x - 1) + 2 * P_LEFT(y - 2 * x - 2) + P_LEFT(y - 2 * x - 3) + 2) >> 2;
                break;
            }
            case 6: {
                int z = 2 * y - x;
                if (z >= 0 && !(z & 1)) v = (P_LEFT(y - (x >> 1) - 1) + P_LEFT(y - (x >> 1)) + 1) >> 1;
                else if (z > 0) v = (P_LEFT(y - (x >> 1) - 2) + 2 * P_LEFT(y - (x >> 1) - 1) + P_LEFT(y - (x >> 1)) + 2) >> 2;
                else if (z == -1) v = (P_LEFT(0) + 2 * E[8] + P_TOP(0) + 2) >> 2;
                else v = (P_TOP(x - 2 * y - 1) + 2 * P_TOP(x - 2 * y - 2) + P_TOP(x - 2 * y - 3) + 2) >> 2;
                break;
            }
            case 7:
                if (!(y & 1)) v = (P_TOP(x + (y >> 1)) + P_TOP(x + (y >> 1) + 1) + 1) >> 1;
                else v = (P_TOP(x + (y >> 1)) + 2 * P_TOP(x + (y >> 1) + 1) + P_TOP(x + (y >> 1) + 2) + 2) >> 2;
                break;
            case 8: {
                int z = x + 2 * y;
                if (z > 13) v = P_LEFT(7);
                else if (z == 13) v = (P_LEFT(6) + 3 * P_LEFT(7) + 2) >> 2;
                else if (!(z & 1)) v = (P_LEFT(y + (x >> 1)) + P_LEFT(y + (x >> 1) + 1) + 1) >> 1;
                else v = (P_LEFT(y + (x >> 1)) + 2 * P_LEFT(y + (x >> 1) + 1) + P_LEFT(y + (x >> 1) + 2) + 2) >> 2;
                break;
            }
            }
            dst[y * W + x] = (u8)v;
        }
    }
#undef P_TOP
#undef P_LEFT
}

static void h264_intra16(h264_dec *d, int mode) {
    h264_edge e;
    h264_luma_edge(d, 0, 0, 16, &e);
    int W = d->mb_w * 16;
    u8 *dst = d->cur->y + d->m.y * 16 * W + d->m.x * 16;
    if (mode == 3) {
        int H = 0, V = 0;
        for (int i = 0; i < 8; i++) {
            H += (i + 1) * (e.top[8 + i] - (i < 7 ? e.top[6 - i] : e.tl));
            V += (i + 1) * (e.left[8 + i] - (i < 7 ? e.left[6 - i] : e.tl));
        }
        int a = 16 * (e.left[15] + e.top[15]), b = (5 * H + 32) >> 6, c = (5 * V + 32) >> 6;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) dst[y * W + x] = h264_clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
        return;
    }
    int dc = 128;
    if (mode == 2) {
        int s = 0;
        if (e.a && e.b) { for (int i = 0; i < 16; i++) s += e.top[i] + e.left[i]; dc = (s + 16) >> 5; }
        else if (e.a) { for (int i = 0; i < 16; i++) s += e.left[i]; dc = (s + 8) >> 4; }
        else if (e.b) { for (int i = 0; i < 16; i++) s += e.top[i]; dc = (s + 8) >> 4; }
    }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) dst[y * W + x] = (u8)(mode == 0 ? e.top[x] : mode == 1 ? e.left[y] : dc);
}

static void h264_intra_chroma(h264_dec *d, int mode) {
    int CW = d->mb_w * 8;
    int blk;
    int a = h264_intra_ok(d, h264_nb(d, -1, 0, &blk));
    int b = h264_intra_ok(d, h264_nb(d, 0, -1, &blk));
    int dd = h264_intra_ok(d, h264_nb(d, -1, -1, &blk));
    for (int c = 0; c < 2; c++) {
        u8 *pl = c ? d->cur->cr : d->cur->cb;
        u8 *dst = pl + d->m.y * 8 * CW + d->m.x * 8;
        u8 top[8], left[8], tl = 0;
        if (b) for (int i = 0; i < 8; i++) top[i] = dst[-CW + i];
        if (a) for (int i = 0; i < 8; i++) left[i] = dst[i * CW - 1];
        if (dd) tl = dst[-CW - 1];
        if (mode == 0) {
            /* 8.3.4.1-3: the corner blocks use both edges; the top-right
               one what is above it before what is to its left, the
               bottom-left one the other way round. */
            for (int q = 0; q < 4; q++) {
                int xo = (q & 1) * 4, yo = (q >> 1) * 4, st = 0, sl = 0, v = 128;
                for (int i = 0; i < 4; i++) { st += top[xo + i]; sl += left[yo + i]; }
                if (b) st += 0; else st = -1;
                if (a) sl += 0; else sl = -1;
                if (q == 0 || q == 3) {
                    if (a && b) v = (st + sl + 4) >> 3;
                    else if (a) v = (sl + 2) >> 2;
                    else if (b) v = (st + 2) >> 2;
                } else if (q == 1) {
                    if (b) v = (st + 2) >> 2;
                    else if (a) v = (sl + 2) >> 2;
                } else {
                    if (a) v = (sl + 2) >> 2;
                    else if (b) v = (st + 2) >> 2;
                }
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) dst[(yo + y) * CW + xo + x] = (u8)v;
            }
        } else if (mode == 1) {
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) dst[y * CW + x] = left[y];
        } else if (mode == 2) {
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) dst[y * CW + x] = top[x];
        } else {
            int H = 0, V = 0;
            for (int i = 0; i < 4; i++) {
                H += (i + 1) * (top[4 + i] - (i < 3 ? top[2 - i] : tl));
                V += (i + 1) * (left[4 + i] - (i < 3 ? left[2 - i] : tl));
            }
            int aa = 16 * (left[7] + top[7]), bb = (34 * H + 32) >> 6, cc = (34 * V + 32) >> 6;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) dst[y * CW + x] = h264_clip1((aa + bb * (x - 3) + cc * (y - 3) + 16) >> 5);
        }
    }
}

/* --- inter prediction (8.4.2) ---------------------------------------------------------------- */

static inline int h264_tap(int a, int b, int c, int dd, int e, int f) { return a - 5 * b + 20 * c + 20 * dd - 5 * e + f; }

/* A w x h block of luma from ref at integer (x, y) plus the fraction
   (fx, fy) in quarters, into dst (stride 16). */
static void h264_mc_luma(const u8 *src, int W, int H, int x, int y, int fx, int fy, int w, int h, u8 *dst) {
    u8 win[21 * 21];
    int ws = w + 5;
    for (int j = 0; j < h + 5; j++) {
        int yy = h264_clip3(0, H - 1, y - 2 + j);
        const u8 *row = src + yy * W;
        for (int i = 0; i < ws; i++) win[j * ws + i] = row[h264_clip3(0, W - 1, x - 2 + i)];
    }
#define G(i, j) ((int)win[((j) + 2) * ws + (i) + 2])
    if (!fx && !fy) {
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) dst[j * 16 + i] = (u8)G(i, j);
        return;
    }
    /* Half samples: b1 across, h1 down, j1 both, unrounded. */
    int b1[21 * 21], h1[21 * 21];
    /* Across: every column of the block, on the rows j's taps reach and the
       one below the block (s). Down: the rows of the block, and the column
       right of it too (m). Nothing reaches further, and the window ends
       there. */
    for (int j = -2; j < h + 3; j++)
        for (int i = 0; i < w; i++)
            b1[(j + 2) * ws + i] = h264_tap(G(i - 2, j), G(i - 1, j), G(i, j), G(i + 1, j), G(i + 2, j), G(i + 3, j));
    for (int j = 0; j < h; j++)
        for (int i = 0; i <= w; i++)
            h1[j * ws + i] = h264_tap(G(i, j - 2), G(i, j - 1), G(i, j), G(i, j + 1), G(i, j + 2), G(i, j + 3));
#define B1(i, j) b1[((j) + 2) * ws + (i)]
#define BH(i, j) h264_clip1((B1(i, j) + 16) >> 5)
#define HV(i, j) h264_clip1((h1[(j) * ws + (i)] + 16) >> 5)
#define JC(i, j) h264_clip1((h264_tap(B1(i, (j) - 2), B1(i, (j) - 1), B1(i, j), B1(i, (j) + 1), B1(i, (j) + 2), B1(i, (j) + 3)) + 512) >> 10)
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int v;
            switch (fy * 4 + fx) {
            case 1: v = (G(i, j) + BH(i, j) + 1) >> 1; break;
            case 2: v = BH(i, j); break;
            case 3: v = (BH(i, j) + G(i + 1, j) + 1) >> 1; break;
            case 4: v = (G(i, j) + HV(i, j) + 1) >> 1; break;
            case 8: v = HV(i, j); break;
            case 12: v = (HV(i, j) + G(i, j + 1) + 1) >> 1; break;
            case 10: v = JC(i, j); break;
            case 6: v = (BH(i, j) + JC(i, j) + 1) >> 1; break;
            case 14: v = (JC(i, j) + BH(i, j + 1) + 1) >> 1; break;
            case 9: v = (HV(i, j) + JC(i, j) + 1) >> 1; break;
            case 11: v = (JC(i, j) + HV(i + 1, j) + 1) >> 1; break;
            case 5: v = (BH(i, j) + HV(i, j) + 1) >> 1; break;
            case 7: v = (BH(i, j) + HV(i + 1, j) + 1) >> 1; break;
            case 13: v = (HV(i, j) + BH(i, j + 1) + 1) >> 1; break;
            default: v = (HV(i + 1, j) + BH(i, j + 1) + 1) >> 1; break;   /* 15 */
            }
            dst[j * 16 + i] = (u8)v;
        }
    }
#undef G
#undef B1
#undef BH
#undef HV
#undef JC
}

static void h264_mc_chroma(const u8 *src, int W, int H, int x, int y, int fx, int fy, int w, int h, u8 *dst) {
    for (int j = 0; j < h; j++) {
        int y0 = h264_clip3(0, H - 1, y + j), y1 = h264_clip3(0, H - 1, y + j + 1);
        for (int i = 0; i < w; i++) {
            int x0 = h264_clip3(0, W - 1, x + i), x1 = h264_clip3(0, W - 1, x + i + 1);
            int A = src[y0 * W + x0], B = src[y0 * W + x1], C = src[y1 * W + x0], D = src[y1 * W + x1];
            dst[j * 8 + i] = (u8)(((8 - fx) * (8 - fy) * A + fx * (8 - fy) * B + (8 - fx) * fy * C + fx * fy * D + 32) >> 6);
        }
    }
}

/* Predicts one block (luma x, y, w, h within the macroblock) from the lists
   it uses, weighs it and writes it into the picture. */
static void h264_inter_block(h264_dec *d, int bx, int by, int w, int h, int predflags,
                             int ref0, int ref1, const short *mv0, const short *mv1) {
    int W = d->mb_w * 16, H = d->mb_h * 16, CW = W / 2, CH = H / 2;
    int px = d->m.x * 16 + bx, py = d->m.y * 16 + by;
    static u8 pl[2][256], pc[2][2][64];
    int refs[2] = { ref0, ref1 };
    const short *mvs[2] = { mv0, mv1 };
    for (int l = 0; l < 2; l++) {
        if (!(predflags & (1 << l))) continue;
        h264_pic *r = d->sl.list[l][refs[l]];
        if (!r) r = d->cur;
        int mx = mvs[l][0], my = mvs[l][1];
        h264_mc_luma(r->y, W, H, px + (mx >> 2), py + (my >> 2), mx & 3, my & 3, w, h, pl[l]);
        int cx = px / 2 + (mx >> 3), cy = py / 2 + (my >> 3);
        h264_mc_chroma(r->cb, CW, CH, cx, cy, mx & 7, my & 7, w / 2, h / 2, pc[l][0]);
        h264_mc_chroma(r->cr, CW, CH, cx, cy, mx & 7, my & 7, w / 2, h / 2, pc[l][1]);
    }
    const h264_slice *s = &d->sl;
    int explicit_w = (s->type == H264_P && d->pp->weighted_pred) || (s->type == H264_B && d->pp->weighted_bipred == 1);
    int implicit_w = s->type == H264_B && d->pp->weighted_bipred == 2 && predflags == 3;
    u8 *dy = d->cur->y + py * W + px;
    u8 *dc[2] = { d->cur->cb + (py / 2) * CW + px / 2, d->cur->cr + (py / 2) * CW + px / 2 };
    for (int plane = 0; plane < 3; plane++) {
        int bw = plane ? w / 2 : w, bh = plane ? h / 2 : h, ps = plane ? 8 : 16;
        u8 *out = plane ? dc[plane - 1] : dy;
        int os = plane ? CW : W;
        const u8 *a = plane ? pc[0][plane - 1] : pl[0], *b = plane ? pc[1][plane - 1] : pl[1];
        if (predflags != 3) {
            int l = predflags == 1 ? 0 : 1;
            const u8 *p = l ? b : a;
            if (explicit_w) {
                int logwd = plane ? s->chroma_denom : s->luma_denom;
                int wt = plane ? s->cw[l][refs[l]][plane - 1] : s->lw[l][refs[l]];
                int of = plane ? s->co[l][refs[l]][plane - 1] : s->lo[l][refs[l]];
                for (int j = 0; j < bh; j++)
                    for (int i = 0; i < bw; i++) {
                        int v = p[j * ps + i];
                        out[j * os + i] = logwd >= 1 ? h264_clip1(((v * wt + (1 << (logwd - 1))) >> logwd) + of)
                                                     : h264_clip1(v * wt + of);
                    }
            } else {
                for (int j = 0; j < bh; j++) for (int i = 0; i < bw; i++) out[j * os + i] = p[j * ps + i];
            }
        } else if (explicit_w || implicit_w) {
            int logwd, w0, w1, o0, o1;
            if (implicit_w) {
                logwd = 5; w1 = s->implicit[ref0][ref1]; w0 = 64 - w1; o0 = o1 = 0;
            } else {
                logwd = plane ? s->chroma_denom : s->luma_denom;
                w0 = plane ? s->cw[0][ref0][plane - 1] : s->lw[0][ref0];
                w1 = plane ? s->cw[1][ref1][plane - 1] : s->lw[1][ref1];
                o0 = plane ? s->co[0][ref0][plane - 1] : s->lo[0][ref0];
                o1 = plane ? s->co[1][ref1][plane - 1] : s->lo[1][ref1];
            }
            for (int j = 0; j < bh; j++)
                for (int i = 0; i < bw; i++)
                    out[j * os + i] = h264_clip1(((a[j * ps + i] * w0 + b[j * ps + i] * w1 + (1 << logwd)) >> (logwd + 1)) +
                                                 ((o0 + o1 + 1) >> 1));
        } else {
            for (int j = 0; j < bh; j++)
                for (int i = 0; i < bw; i++) out[j * os + i] = (u8)((a[j * ps + i] + b[j * ps + i] + 1) >> 1);
        }
    }
}

/* --- motion vectors (8.4.1) ------------------------------------------------------------------ */

/* A neighbour's motion for list l at luma (x, y) relative to the current
   macroblock. Returns whether that block is there at all; ref is -1 (and
   the vector 0) when it is there but intra or not predicted from l. */
static int h264_nb_motion(h264_dec *d, int x, int y, int l, int *ref, int *mx, int *my) {
    int blk, addr = h264_nb(d, x, y, &blk);
    *ref = -1; *mx = *my = 0;
    if (addr < 0) return 0;
    if (addr == d->m.addr && !(d->m.done & (1 << blk))) return 0;
    const h264_pic *p = d->cur;
    int b8 = ((blk >> 3) << 1) | ((blk & 3) >> 1);
    int r = p->ri[addr][l][b8];
    if (r >= 0) { *ref = r; *mx = p->mv[addr][l][blk][0]; *my = p->mv[addr][l][blk][1]; }
    return 1;
}

static inline int h264_median(int a, int b, int c) {
    int mx = a > b ? a : b, mn = a < b ? a : b;
    return c > mx ? mx : c < mn ? mn : c;
}

/* 8.4.1.3: the predicted vector of a partition at (x, y), w x h. */
static void h264_mvp(h264_dec *d, int l, int ref, int x, int y, int w, int h, int *px, int *py) {
    int ra, rb, rc, ax, ay, bxv, byv, cx, cy;
    int aa = h264_nb_motion(d, x - 1, y, l, &ra, &ax, &ay);
    int ab = h264_nb_motion(d, x, y - 1, l, &rb, &bxv, &byv);
    int ac = h264_nb_motion(d, x + w, y - 1, l, &rc, &cx, &cy);
    if (!ac) ac = h264_nb_motion(d, x - 1, y - 1, l, &rc, &cx, &cy);
    if (w == 16 && h == 8) {
        if (y == 0 && rb == ref) { *px = bxv; *py = byv; return; }
        if (y == 8 && ra == ref) { *px = ax; *py = ay; return; }
    } else if (w == 8 && h == 16) {
        if (x == 0 && ra == ref) { *px = ax; *py = ay; return; }
        if (x == 8 && rc == ref) { *px = cx; *py = cy; return; }
    }
    if (!ab && !ac && aa) { rb = rc = ra; bxv = cx = ax; byv = cy = ay; }
    int n = (ra == ref) + (rb == ref) + (rc == ref);
    if (n == 1) {
        if (ra == ref) { *px = ax; *py = ay; }
        else if (rb == ref) { *px = bxv; *py = byv; }
        else { *px = cx; *py = cy; }
        return;
    }
    *px = h264_median(ax, bxv, cx);
    *py = h264_median(ay, byv, cy);
}

/* Writes a block's motion into the picture and marks it known. */
static void h264_set_motion(h264_dec *d, int bx4, int by4, int w4, int h4, int l, int ref, int mx, int my) {
    h264_pic *p = d->cur;
    int a = d->m.addr;
    for (int j = by4; j < by4 + h4; j++)
        for (int i = bx4; i < bx4 + w4; i++) {
            int blk = j * 4 + i;
            p->mv[a][l][blk][0] = (short)mx;
            p->mv[a][l][blk][1] = (short)my;
            int b8 = (j >> 1) * 2 + (i >> 1);
            p->ri[a][l][b8] = (signed char)ref;
            p->refid[a][l][b8] = ref >= 0 && d->sl.list[l][ref] ? d->sl.list[l][ref]->id : 0;
        }
}

static void h264_mark_done(h264_dec *d, int bx4, int by4, int w4, int h4) {
    for (int j = by4; j < by4 + h4; j++)
        for (int i = bx4; i < bx4 + w4; i++) d->m.done |= (u16)(1 << (j * 4 + i));
}

/* P_Skip (8.4.1.1). */
static void h264_pskip_mv(h264_dec *d, int *mx, int *my) {
    int ra, rb, ax, ay, bx, by;
    int aa = h264_nb_motion(d, -1, 0, 0, &ra, &ax, &ay);
    int ab = h264_nb_motion(d, 0, -1, 0, &rb, &bx, &by);
    if (!aa || !ab || (ra == 0 && !ax && !ay) || (rb == 0 && !bx && !by)) { *mx = *my = 0; return; }
    h264_mvp(d, 0, 0, 0, 0, 16, 16, mx, my);
}

/* The colocated block of a 4x4 (raster) for direct prediction: its vector
   and reference index, the list they came from, and the picture referred
   to. Intra: ref -1. */
static void h264_colocated(h264_dec *d, int blk, int *mx, int *my, int *ref, int *refid) {
    const h264_pic *col = d->sl.list[1][0];
    int a = d->m.addr;
    if (d->sp->direct_8x8_inference) {
        int b8 = ((blk >> 3) << 1) | ((blk & 3) >> 1);
        static const u8 CORNER[4] = { 0, 3, 12, 15 };
        blk = CORNER[b8];
    }
    int b8 = ((blk >> 3) << 1) | ((blk & 3) >> 1);
    *mx = *my = 0; *ref = -1; *refid = 0;
    if (!col || col->intra[a]) return;
    int l = col->ri[a][0][b8] >= 0 ? 0 : 1;
    if (col->ri[a][l][b8] < 0) return;
    *mx = col->mv[a][l][blk][0];
    *my = col->mv[a][l][blk][1];
    *ref = col->ri[a][l][b8];
    *refid = col->refid[a][l][b8];
}

static inline int h264_minpos(int a, int b) { return (a >= 0 && b >= 0) ? (a < b ? a : b) : (a > b ? a : b); }

/* Direct prediction of the 8x8 b8 (or, with which < 0, of the whole
   macroblock) (8.4.1.2), predicted and written into the picture. */
static void h264_direct(h264_dec *d, int b8) {
    h264_slice *s = &d->sl;
    int x0 = (b8 & 1) * 2, y0 = (b8 >> 1) * 2;
    if (s->direct_spatial) {
        int ref[2], mvx[2] = { 0, 0 }, mvy[2] = { 0, 0 };
        for (int l = 0; l < 2; l++) {
            int ra, rb, rc, t1, t2;
            h264_nb_motion(d, -1, 0, l, &ra, &t1, &t2);
            h264_nb_motion(d, 0, -1, l, &rb, &t1, &t2);
            if (!h264_nb_motion(d, 16, -1, l, &rc, &t1, &t2)) h264_nb_motion(d, -1, -1, l, &rc, &t1, &t2);
            ref[l] = h264_minpos(ra, h264_minpos(rb, rc));
        }
        int zero = ref[0] < 0 && ref[1] < 0;
        if (zero) ref[0] = ref[1] = 0;
        /* The neighbours' prediction is of the whole macroblock: blocks of
           this one already done must not count. */
        u16 keep = d->m.done;
        d->m.done = 0;
        for (int l = 0; l < 2; l++) if (!zero && ref[l] >= 0) h264_mvp(d, l, ref[l], 0, 0, 16, 16, &mvx[l], &mvy[l]);
        d->m.done = keep;
        int pf = zero ? 3 : ((ref[0] >= 0) | ((ref[1] >= 0) << 1));
        h264_pic *col = s->list[1][0];
        int col_short = col && col->ref != 2;
        int step = d->sp->direct_8x8_inference ? 2 : 1;
        for (int j = y0; j < y0 + 2; j += step) {
            for (int i = x0; i < x0 + 2; i += step) {
                int cmx, cmy, cref, cid;
                h264_colocated(d, j * 4 + i, &cmx, &cmy, &cref, &cid);
                int colzero = col_short && cref == 0 && cmx >= -1 && cmx <= 1 && cmy >= -1 && cmy <= 1;
                short mv[2][2];
                for (int l = 0; l < 2; l++) {
                    int zx = zero || ref[l] < 0 || (ref[l] == 0 && colzero);
                    mv[l][0] = (short)(zx ? 0 : mvx[l]);
                    mv[l][1] = (short)(zx ? 0 : mvy[l]);
                    h264_set_motion(d, i, j, step, step, l, (pf >> l) & 1 ? ref[l] : -1, mv[l][0], mv[l][1]);
                }
                h264_inter_block(d, i * 4, j * 4, step * 4, step * 4, pf, ref[0], ref[1], mv[0], mv[1]);
            }
        }
    } else {
        int step = d->sp->direct_8x8_inference ? 2 : 1;
        for (int j = y0; j < y0 + 2; j += step) {
            for (int i = x0; i < x0 + 2; i += step) {
                int cmx, cmy, cref, cid;
                h264_colocated(d, j * 4 + i, &cmx, &cmy, &cref, &cid);
                int r0 = 0;
                if (cref >= 0) {
                    r0 = -1;
                    for (int k = 0; k < s->num_ref[0] && r0 < 0; k++) if (s->list[0][k] && s->list[0][k]->id == cid) r0 = k;
                    if (r0 < 0) r0 = 0;            /* the picture is no longer a reference: the nearest there is */
                }
                h264_pic *p0 = s->list[0][r0], *p1 = s->list[1][0];
                short mv[2][2];
                int tb = h264_clip3(-128, 127, d->cur->poc - p0->poc);
                int td = h264_clip3(-128, 127, p1->poc - p0->poc);
                if (p0->ref == 2 || td == 0) {
                    mv[0][0] = (short)cmx; mv[0][1] = (short)cmy; mv[1][0] = mv[1][1] = 0;
                } else {
                    int tx = (16384 + h264_abs(td / 2)) / td;
                    int dsf = h264_clip3(-1024, 1023, (tb * tx + 32) >> 6);
                    mv[0][0] = (short)((dsf * cmx + 128) >> 8);
                    mv[0][1] = (short)((dsf * cmy + 128) >> 8);
                    mv[1][0] = (short)(mv[0][0] - cmx);
                    mv[1][1] = (short)(mv[0][1] - cmy);
                }
                h264_set_motion(d, i, j, step, step, 0, r0, mv[0][0], mv[0][1]);
                h264_set_motion(d, i, j, step, step, 1, 0, mv[1][0], mv[1][1]);
                h264_inter_block(d, i * 4, j * 4, step * 4, step * 4, 3, r0, 0, mv[0], mv[1]);
            }
        }
    }
    h264_mark_done(d, x0, y0, 2, 2);
}

/* --- the deblocking filter (8.7) ------------------------------------------------------------- */

static void h264_filter_line(u8 *p, int step, int bs, int alpha, int beta, int tc0, int chroma) {
    int p0 = p[-step], p1 = p[-2 * step], q0 = p[0], q1 = p[step];
    if (!(h264_abs(p0 - q0) < alpha && h264_abs(p1 - p0) < beta && h264_abs(q1 - q0) < beta)) return;
    if (chroma) {
        if (bs < 4) {
            int tc = tc0 + 1;
            int delta = h264_clip3(-tc, tc, (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
            p[-step] = h264_clip1(p0 + delta);
            p[0] = h264_clip1(q0 - delta);
        } else {
            p[-step] = (u8)((2 * p1 + p0 + q1 + 2) >> 2);
            p[0] = (u8)((2 * q1 + q0 + p1 + 2) >> 2);
        }
        return;
    }
    int p2 = p[-3 * step], q2 = p[2 * step];
    int ap = h264_abs(p2 - p0) < beta, aq = h264_abs(q2 - q0) < beta;
    if (bs < 4) {
        int tc = tc0 + ap + aq;
        int delta = h264_clip3(-tc, tc, (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
        p[-step] = h264_clip1(p0 + delta);
        p[0] = h264_clip1(q0 - delta);
        if (ap) p[-2 * step] = (u8)(p1 + h264_clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1));
        if (aq) p[step] = (u8)(q1 + h264_clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1));
    } else {
        int p3 = p[-4 * step], q3 = p[3 * step];
        int strong = h264_abs(p0 - q0) < ((alpha >> 2) + 2);
        if (ap && strong) {
            p[-step] = (u8)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
            p[-2 * step] = (u8)((p2 + p1 + p0 + q0 + 2) >> 2);
            p[-3 * step] = (u8)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
        } else {
            p[-step] = (u8)((2 * p1 + p0 + q1 + 2) >> 2);
        }
        if (aq && strong) {
            p[0] = (u8)((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
            p[step] = (u8)((p0 + q0 + q1 + q2 + 2) >> 2);
            p[2 * step] = (u8)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
        } else {
            p[0] = (u8)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

/* The pictures a block is predicted from, as ids, and its vectors. */
static int h264_block_refs(const h264_pic *p, int a, int blk, int *ids, const short **mvs) {
    int b8 = ((blk >> 3) << 1) | ((blk & 3) >> 1), n = 0;
    for (int l = 0; l < 2; l++)
        if (p->ri[a][l][b8] >= 0) { ids[n] = p->refid[a][l][b8]; mvs[n] = p->mv[a][l][blk]; n++; }
    return n;
}

static inline int h264_far(const short *a, const short *b) {
    return h264_abs(a[0] - b[0]) >= 4 || h264_abs(a[1] - b[1]) >= 4;
}

/* 8.7.2.1: how hard to filter between block bp of macroblock ap and bq of
   aq. */
static int h264_bs(const h264_dec *d, int ap, int bp, int aq, int bq, int mb_edge) {
    const h264_pic *p = d->cur;
    if ((d->mb_kind[ap] & H264_K_INTRA) || (d->mb_kind[aq] & H264_K_INTRA)) return mb_edge ? 4 : 3;
    if (((d->mb_nz[ap] >> bp) | (d->mb_nz[aq] >> bq)) & 1) return 2;
    int ip[2], iq[2];
    const short *mp[2], *mq[2];
    int np = h264_block_refs(p, ap, bp, ip, mp), nq = h264_block_refs(p, aq, bq, iq, mq);
    if (np != nq) return 1;
    if (np == 0) return 0;                 /* only in a damaged stream: nothing to compare */
    if (np == 1) return ip[0] != iq[0] || h264_far(mp[0], mq[0]);
    if (!((ip[0] == iq[0] && ip[1] == iq[1]) || (ip[0] == iq[1] && ip[1] == iq[0]))) return 1;
    if (ip[0] != ip[1]) {
        if (ip[0] == iq[0]) return h264_far(mp[0], mq[0]) || h264_far(mp[1], mq[1]);
        return h264_far(mp[0], mq[1]) || h264_far(mp[1], mq[0]);
    }
    return (h264_far(mp[0], mq[0]) || h264_far(mp[1], mq[1])) && (h264_far(mp[0], mq[1]) || h264_far(mp[1], mq[0]));
}

static void h264_deblock_mb(h264_dec *d, int addr) {
    int idc = d->mb_dbidc[addr];
    if (idc == 1) return;
    int mx = addr % d->mb_w, my = addr / d->mb_w;
    int W = d->mb_w * 16, CW = W / 2;
    int alpha_off = d->mb_alpha[addr], beta_off = d->mb_beta[addr];
    int t8 = d->mb_t8[addr];
    for (int dir = 0; dir < 2; dir++) {                 /* 0 vertical edges, 1 horizontal */
        for (int e = 0; e < 4; e++) {
            if (t8 && (e & 1)) continue;
            int other = addr;
            if (e == 0) {
                if (dir == 0 ? mx == 0 : my == 0) continue;
                other = dir == 0 ? addr - 1 : addr - d->mb_w;
                if (idc == 2 && d->mb_slice[other] != d->mb_slice[addr]) continue;
            }
            int bs[4], any = 0;
            for (int k = 0; k < 4; k++) {
                int bq = dir == 0 ? k * 4 + e : e * 4 + k;
                int bp = e ? (dir == 0 ? bq - 1 : bq - 4) : (dir == 0 ? k * 4 + 3 : 12 + k);
                bs[k] = h264_bs(d, other, bp, addr, bq, e == 0);
                any |= bs[k];
            }
            if (!any) continue;
            /* Luma. */
            int qp = (d->mb_qp[other] + d->mb_qp[addr] + 1) >> 1;
            int ia = h264_clip3(0, 51, qp + alpha_off), ib = h264_clip3(0, 51, qp + beta_off);
            int alpha = H264_ALPHA[ia], beta = H264_BETA[ib];
            u8 *base = d->cur->y + (my * 16) * W + mx * 16;
            for (int k = 0; k < 4; k++) {
                if (!bs[k]) continue;
                int tc0 = bs[k] < 4 ? H264_TC0[ia * 3 + bs[k] - 1] : 0;
                for (int i = 0; i < 4; i++) {
                    u8 *pt = dir == 0 ? base + (k * 4 + i) * W + e * 4 : base + (e * 4) * W + k * 4 + i;
                    h264_filter_line(pt, dir == 0 ? 1 : W, bs[k], alpha, beta, tc0, 0);
                }
            }
            /* Chroma: its edges are at luma 0 and 8. */
            if (e & 1) continue;
            for (int c = 0; c < 2; c++) {
                int qc = (d->mb_qpc[c][other] + d->mb_qpc[c][addr] + 1) >> 1;
                int ca = h264_clip3(0, 51, qc + alpha_off), cbt = h264_clip3(0, 51, qc + beta_off);
                int calpha = H264_ALPHA[ca], cbeta = H264_BETA[cbt];
                u8 *cb = (c ? d->cur->cr : d->cur->cb) + (my * 8) * CW + mx * 8;
                for (int k = 0; k < 4; k++) {
                    if (!bs[k]) continue;
                    int tc0 = bs[k] < 4 ? H264_TC0[ca * 3 + bs[k] - 1] : 0;
                    for (int i = 0; i < 2; i++) {
                        u8 *pt = dir == 0 ? cb + (k * 2 + i) * CW + e * 2 : cb + (e * 2) * CW + k * 2 + i;
                        h264_filter_line(pt, dir == 0 ? 1 : CW, bs[k], calpha, cbeta, tc0, 1);
                    }
                }
            }
        }
    }
}

static void h264_deblock_picture(h264_dec *d) {
    for (int a = 0; a < d->mbs; a++) if (d->mb_slice[a] >= 0) h264_deblock_mb(d, a);
}
