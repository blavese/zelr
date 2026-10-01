/* A macroblock's syntax (7.3.5), by CAVLC (9.2) or CABAC (9.3), and the
 * slice data around it (7.3.4). Each macroblock is predicted and its
 * residual added as soon as it has been read (h264rec.h). Part of h264.h.
 */

/* A trace of what each macroblock said, for comparing with a reference
   decoder's while developing; nothing unless H264_TRACE is defined. */
#ifdef H264_TRACE
#define H264_T(...) printf(__VA_ARGS__)
static void h264_trace_levels(const char *name, const short *lv, int n) {
    int run = 0;
    for (int i = 0; i < n; i++) {
        if (!lv[i]) { run++; continue; }
        printf("%s %d %d\n", name, lv[i], run);
        run = 0;
    }
    printf("%s 0 0\n", name);
}
#else
#define H264_T(...) ((void)0)
#define h264_trace_levels(name, lv, n) ((void)0)
#endif

/* --- reading the slice data's bits ------------------------------------------------------- */

static inline int sd_bit(h264_dec *d) {
    int at = d->pos >> 3;
    int v = at < d->data_n ? (d->data[at] >> (7 - (d->pos & 7))) & 1 : 0;
    d->pos++;
    return v;
}
static inline u32 sd_peek(h264_dec *d, int k) {
    h264_bits b = { d->data, d->data_n, d->pos };
    return hb_peek(&b, k);
}
static inline u32 sd_u(h264_dec *d, int k) {
    h264_bits b = { d->data, d->data_n, d->pos };
    u32 v = hb_u(&b, k);
    d->pos = b.pos;
    return v;
}
static inline u32 sd_ue(h264_dec *d) {                /* held as hb_ue is */
    int z = 0;
    while (z < 32 && !sd_bit(d)) z++;
    if (z >= 27) { if (z < 32) sd_u(d, z); return H264_UE_MAX; }
    u32 v = ((1u << z) - 1) + sd_u(d, z);
    return v > H264_UE_MAX ? H264_UE_MAX : v;
}
static inline int sd_se(h264_dec *d) {
    u32 k = sd_ue(d);
    return (k & 1) ? (int)((k + 1) >> 1) : -(int)(k >> 1);
}

/* --- the CABAC engine (9.3.1.2, 9.3.3.2) ------------------------------------------------- */

static void cab_start(h264_dec *d) {
    d->range = 510;
    d->offset = sd_u(d, 9);
}

static void cab_contexts(h264_dec *d) {
    const signed char *t = d->sl.type == H264_I ? H264_CABAC_I
                         : d->sl.cabac_init_idc == 0 ? H264_CABAC_PB0
                         : d->sl.cabac_init_idc == 1 ? H264_CABAC_PB1 : H264_CABAC_PB2;
    int qp = h264_clip3(0, 51, d->sl.qp);
    for (int i = 0; i < 1024; i++) {
        int pre = h264_clip3(1, 126, ((t[2 * i] * qp) >> 4) + t[2 * i + 1]);
        d->ctx[i] = pre <= 63 ? (u8)((63 - pre) << 1) : (u8)(((pre - 64) << 1) | 1);
    }
}

static inline int cab(h264_dec *d, int ci) {
    u8 s = d->ctx[ci];
    int pst = s >> 1, mps = s & 1, bin;
    u32 lps = H264_RANGE_LPS[pst * 4 + ((d->range >> 6) & 3)];
    d->range -= lps;
    if (d->offset >= d->range) {
        bin = !mps;
        d->offset -= d->range;
        d->range = lps;
        if (pst == 0) mps = 1 - mps;
        pst = H264_NEXT_LPS[pst];
    } else {
        bin = mps;
        if (pst < 62) pst++;
    }
    d->ctx[ci] = (u8)((pst << 1) | mps);
    while (d->range < 256) { d->range <<= 1; d->offset = (d->offset << 1) | (u32)sd_bit(d); }
    return bin;
}

static inline int cab_bypass(h264_dec *d) {
    d->offset = (d->offset << 1) | (u32)sd_bit(d);
    if (d->offset >= d->range) { d->offset -= d->range; return 1; }
    return 0;
}

static inline int cab_term(h264_dec *d) {
    d->range -= 2;
    if (d->offset >= d->range) return 1;
    while (d->range < 256) { d->range <<= 1; d->offset = (d->offset << 1) | (u32)sd_bit(d); }
    return 0;
}

/* --- neighbours of the macroblock, for contexts ------------------------------------------ */

static inline int mb_left(const h264_dec *d) { return d->m.x > 0 && h264_mb_ok(d, d->m.addr - 1) ? d->m.addr - 1 : -1; }
static inline int mb_top(const h264_dec *d) { return d->m.y > 0 && h264_mb_ok(d, d->m.addr - d->mb_w) ? d->m.addr - d->mb_w : -1; }

/* --- CABAC syntax elements (9.3.3.1) ---------------------------------------------------------- */

static int cab_skip(h264_dec *d) {
    int a = mb_left(d), b = mb_top(d);
    int inc = (a >= 0 && !(d->mb_kind[a] & H264_K_SKIP)) + (b >= 0 && !(d->mb_kind[b] & H264_K_SKIP));
    return cab(d, (d->sl.type == H264_B ? 24 : 11) + inc);
}

/* The I macroblock types (Table 9-36) from ctxIdx base for bin 0 (or, in a
   P or B slice, the suffix's own base). */
static int cab_mb_type_i(h264_dec *d, int in_islice, int base) {
    int b0;
    if (in_islice) {
        int a = mb_left(d), b = mb_top(d);
        int inc = (a >= 0 && !(d->mb_kind[a] & H264_K_INXN)) + (b >= 0 && !(d->mb_kind[b] & H264_K_INXN));
        b0 = cab(d, 3 + inc);
    } else {
        b0 = cab(d, base);
    }
    if (!b0) return 0;
    if (cab_term(d)) return 25;
    int luma, chroma, pred;
    if (in_islice) {
        luma = cab(d, 6);
        chroma = cab(d, 7);
        if (chroma) chroma += cab(d, 8);
        pred = cab(d, 9) << 1;
        pred |= cab(d, 10);
    } else {
        luma = cab(d, base + 1);
        chroma = cab(d, base + 2);
        if (chroma) chroma += cab(d, base + 2);
        pred = cab(d, base + 3) << 1;
        pred |= cab(d, base + 3);
    }
    return 1 + pred + 4 * chroma + 12 * luma;
}

static int cab_mb_type(h264_dec *d) {
    if (d->sl.type == H264_I) return cab_mb_type_i(d, 1, 3);
    if (d->sl.type == H264_P) {
        if (cab(d, 14)) return 5 + cab_mb_type_i(d, 0, 17);
        if (!cab(d, 15)) return cab(d, 16) ? 3 : 0;
        return cab(d, 17) ? 1 : 2;
    }
    int a = mb_left(d), b = mb_top(d);
    int inc = (a >= 0 && !(d->mb_kind[a] & (H264_K_SKIP | H264_K_DIRECT))) +
              (b >= 0 && !(d->mb_kind[b] & (H264_K_SKIP | H264_K_DIRECT)));
    if (!cab(d, 27 + inc)) return 0;
    if (!cab(d, 30)) return cab(d, 32) ? 2 : 1;
    if (cab(d, 31)) {
        int v = 12;
        v += cab(d, 32) << 3;
        v += cab(d, 32) << 2;
        v += cab(d, 32) << 1;
        if (v == 24) return 11;
        if (v == 26) return 22;
        if (v == 22) return 23 + cab_mb_type_i(d, 0, 32);
        return v + cab(d, 32);
    }
    int v = 3;
    v += cab(d, 32) << 2;
    v += cab(d, 32) << 1;
    v += cab(d, 32);
    return v;
}

static int cab_sub_type(h264_dec *d) {
    if (d->sl.type == H264_P) {
        if (cab(d, 21)) return 0;
        if (!cab(d, 22)) return 1;
        return cab(d, 23) ? 2 : 3;
    }
    if (!cab(d, 36)) return 0;
    if (!cab(d, 37)) return 1 + cab(d, 39);
    if (cab(d, 38)) {
        if (cab(d, 39)) return 11 + cab(d, 39);
        int v = 7;
        v += cab(d, 39) << 1;
        v += cab(d, 39);
        return v;
    }
    int v = 3;
    v += cab(d, 39) << 1;
    v += cab(d, 39);
    return v;
}

/* --- motion data's contexts ---------------------------------------------------------------- */

/* The reference index a neighbour of block (x, y) of this macroblock uses
   for list l, for ref_idx's context: 0 when it is not there, intra, not
   from l, or predicted (skip and direct). */
static int ctx_ref_gt0(h264_dec *d, int x, int y, int l) {
    int blk, addr = h264_nb(d, x, y, &blk);
    if (addr < 0) return 0;
    int b8 = ((blk >> 3) << 1) | ((blk & 3) >> 1);
    if (addr == d->m.addr) {
        if ((d->m.kind & H264_K_DIRECT) || (d->mb_direct8[addr] >> b8) & 1) return 0;
        return d->m.ref[l][b8] > 0;
    }
    if (d->mb_kind[addr] & (H264_K_INTRA | H264_K_SKIP | H264_K_DIRECT)) return 0;
    if ((d->mb_direct8[addr] >> b8) & 1) return 0;
    return d->cur->ri[addr][l][b8] > 0;
}

static int cab_ref(h264_dec *d, int l, int x, int y) {
    int inc = ctx_ref_gt0(d, x - 1, y, l) + 2 * ctx_ref_gt0(d, x, y - 1, l);
    if (!cab(d, 54 + inc)) return 0;
    int v = 1;
    if (!cab(d, 58)) return v;
    v++;
    while (cab(d, 59) && v < 32) v++;
    return v;
}

static int ctx_absmvd(h264_dec *d, int x, int y, int l, int c) {
    int blk, addr = h264_nb(d, x, y, &blk);
    if (addr < 0) return 0;
    return d->mvd[addr][l][blk][c];
}

static int cab_mvd(h264_dec *d, int l, int c, int x, int y) {
    int sum = ctx_absmvd(d, x - 1, y, l, c) + ctx_absmvd(d, x, y - 1, l, c);
    int base = c ? 47 : 40;
    int inc = sum < 3 ? 0 : sum > 32 ? 2 : 1;
    if (!cab(d, base + inc)) return 0;
    int v = 1, k = 3;
    static const u8 NEXT[8] = { 3, 4, 5, 6, 6, 6, 6, 6 };
    while (v < 9 && cab(d, base + NEXT[v - 1])) v++;
    if (v >= 9) {
        while (cab_bypass(d)) { v += 1 << k; k++; if (k > 24) break; }
        while (k--) v += cab_bypass(d) << k;
    }
    return cab_bypass(d) ? -v : v;
}

static int cab_cbp(h264_dec *d) {
    int cbp = 0;
    for (int b8 = 0; b8 < 4; b8++) {
        int bx = (b8 & 1) * 8, by = (b8 >> 1) * 8, cond[2];
        for (int n = 0; n < 2; n++) {
            int blk, addr = h264_nb(d, n ? bx : bx - 1, n ? by - 1 : by, &blk);
            int b8n = ((blk >> 3) << 1) | ((blk & 3) >> 1);
            if (addr < 0) cond[n] = 0;
            else if (addr == d->m.addr) cond[n] = !((cbp >> b8n) & 1);
            else if (d->mb_kind[addr] & H264_K_PCM) cond[n] = 0;
            else if (d->mb_kind[addr] & H264_K_SKIP) cond[n] = 1;
            else cond[n] = !((d->mb_cbp[addr] >> b8n) & 1);
        }
        if (cab(d, 73 + cond[0] + 2 * cond[1])) cbp |= 1 << b8;
    }
    int a = mb_left(d), b = mb_top(d), ca[2], cb2[2];
    for (int k = 0; k < 2; k++) {
        int nn[2] = { a, b }, *out = k ? cb2 : ca;
        for (int n = 0; n < 2; n++) {
            int addr = nn[n];
            if (addr < 0) out[n] = 0;
            else if (d->mb_kind[addr] & H264_K_PCM) out[n] = 1;
            else if (d->mb_kind[addr] & H264_K_SKIP) out[n] = 0;
            else out[n] = k == 0 ? (d->mb_cbp[addr] >> 4) != 0 : (d->mb_cbp[addr] >> 4) == 2;
        }
    }
    if (cab(d, 77 + ca[0] + 2 * ca[1])) {
        cbp |= cab(d, 81 + cb2[0] + 2 * cb2[1]) ? 32 : 16;
    }
    return cbp;
}

static int cab_qp_delta(h264_dec *d) {
    if (!cab(d, 60 + (d->last_dqp != 0))) return 0;
    int k = 1;
    if (cab(d, 62)) {
        k++;
        while (cab(d, 63) && k < 104) k++;
    }
    return (k & 1) ? (k + 1) / 2 : -(k / 2);
}

static int cab_cpred(h264_dec *d) {
    int a = mb_left(d), b = mb_top(d), inc = 0;
    int nn[2] = { a, b };
    for (int n = 0; n < 2; n++) {
        int addr = nn[n];
        if (addr >= 0 && (d->mb_kind[addr] & H264_K_INTRA) && !(d->mb_kind[addr] & H264_K_PCM) && d->mb_cpred[addr]) inc++;
    }
    if (!cab(d, 64 + inc)) return 0;
    if (!cab(d, 67)) return 1;
    return cab(d, 67) ? 3 : 2;
}

static int cab_t8(h264_dec *d) {
    int a = mb_left(d), b = mb_top(d);
    int inc = (a >= 0 && d->mb_t8[a]) + (b >= 0 && d->mb_t8[b]);
    return cab(d, 399 + inc);
}

/* coded_block_flag's context (9.3.3.1.1.9). cat 0 luma DC, 1 and 2 a luma
   4x4 (raster blk), 3 chroma DC of c, 4 chroma AC block blk of c. */
static int cab_cbf_inc(h264_dec *d, int cat, int blk, int c) {
    int cond[2];
    int intra = (d->m.kind & H264_K_INTRA) != 0;
    for (int n = 0; n < 2; n++) {
        int x, y;
        if (cat == 1 || cat == 2) { x = (blk & 3) * 4; y = (blk >> 2) * 4; }
        else if (cat == 4) { x = (blk & 1) * 8; y = (blk >> 1) * 8; }
        else { x = 0; y = 0; }
        if (n == 0) x--; else y--;
        int nb, addr = h264_nb(d, x, y, &nb);
        if (addr < 0) { cond[n] = intra; continue; }
        u32 bits = addr == d->m.addr ? d->mb_cbf[addr] : d->mb_cbf[addr];
        int kind = addr == d->m.addr ? d->m.kind : d->mb_kind[addr];
        if (kind & H264_K_PCM) { cond[n] = 1; continue; }
        if (cat == 0) { cond[n] = (kind & H264_K_I16) ? (int)((bits >> 16) & 1) : 0; continue; }
        if (kind & H264_K_SKIP) { cond[n] = 0; continue; }
        int cbp = addr == d->m.addr ? d->m.cbp : d->mb_cbp[addr];
        if (cat == 1 || cat == 2) {
            int b8 = ((nb >> 3) << 1) | ((nb & 3) >> 1);
            if (!((cbp >> b8) & 1)) cond[n] = 0;
            else if (addr != d->m.addr && d->mb_t8[addr]) cond[n] = 1;
            else cond[n] = (int)((bits >> nb) & 1);
        } else if (cat == 3) {
            cond[n] = (cbp >> 4) ? (int)((bits >> (17 + c)) & 1) : 0;
        } else {
            int cblk = ((((y + 16) & 15) >> 3) << 1) | (((x + 16) & 15) >> 3);
            cond[n] = (cbp >> 4) == 2 ? (int)((bits >> (19 + 4 * c + cblk)) & 1) : 0;
        }
    }
    return cond[0] + 2 * cond[1];
}

/* A residual block by CABAC (7.3.5.3.3): levels in scan order into lv[0..n),
   returns how many are not zero. cat as above, plus 5 for a luma 8x8. */
static int cab_residual(h264_dec *d, int cat, int blk, int c, int n, short *lv) {
    static const u8 CBF_OFF[5] = { 0, 4, 8, 12, 16 };
    static const u8 SIG_OFF[5] = { 0, 15, 29, 44, 47 };
    static const u8 LVL_OFF[5] = { 0, 10, 20, 30, 39 };
    for (int i = 0; i < n; i++) lv[i] = 0;
    if (cat != 5) {
        if (!cab(d, 85 + CBF_OFF[cat] + cab_cbf_inc(d, cat, blk, c))) return 0;
    }
    int sig_base = cat == 5 ? 402 : 105 + SIG_OFF[cat];
    int last_base = cat == 5 ? 417 : 166 + SIG_OFF[cat];
    int lvl_base = cat == 5 ? 426 : 227 + LVL_OFF[cat];
    u8 sig[64];
    int num = n, count = 0;
    for (int i = 0; i < n - 1; i++) {
        int si = cat == 5 ? H264_SIG8_FRAME[i] : cat == 3 ? h264_min(i, 2) : i;
        int li = cat == 5 ? H264_LAST8[i] : cat == 3 ? h264_min(i, 2) : i;
        sig[i] = (u8)cab(d, sig_base + si);
        if (sig[i] && cab(d, last_base + li)) { num = i + 1; break; }
    }
    if (num == n) sig[n - 1] = 1;
    int gt1 = 0, eq1 = 0;
    int maxc = cat == 3 ? 3 : 4;
    for (int i = num - 1; i >= 0; i--) {
        if (!sig[i]) continue;
        int inc0 = gt1 ? 0 : h264_min(4, 1 + eq1);
        int v;
        if (!cab(d, lvl_base + inc0)) {
            v = 1;
        } else {
            int inc = 5 + h264_min(maxc, gt1);
            int prefix = 1;
            while (prefix < 14 && cab(d, lvl_base + inc)) prefix++;
            v = prefix + 1;
            if (prefix >= 14) {
                int k = 0, s = 0;
                while (cab_bypass(d)) { s += 1 << k; k++; if (k > 24) break; }
                while (k--) s += cab_bypass(d) << k;
                v += s;
            }
        }
        if (v == 1) eq1++; else gt1++;
        lv[i] = (short)(cab_bypass(d) ? -v : v);
        count++;
    }
    return count;
}

/* --- CAVLC (9.2) ------------------------------------------------------------------------------ */

static const h264_vlc *const H264_CT[4] = { H264_COEFF_TOKEN_0, H264_COEFF_TOKEN_1, H264_COEFF_TOKEN_2, H264_COEFF_TOKEN_DC };
static const int H264_CT_N[4] = {
    (int)(sizeof(H264_COEFF_TOKEN_0) / sizeof(h264_vlc)), (int)(sizeof(H264_COEFF_TOKEN_1) / sizeof(h264_vlc)),
    (int)(sizeof(H264_COEFF_TOKEN_2) / sizeof(h264_vlc)), (int)(sizeof(H264_COEFF_TOKEN_DC) / sizeof(h264_vlc)),
};

static int vlc_read(h264_dec *d, const h264_vlc *t, int n, int *a, int *b) {
    u32 bits = sd_peek(d, 16);
    for (int i = 0; i < n; i++) {
        if ((bits >> (16 - t[i].len)) == t[i].code) {
            d->pos += t[i].len;
            *a = t[i].a;
            if (b) *b = t[i].b;
            return 0;
        }
    }
    return -1;
}

static const h264_vlc *const H264_TZ_DC[3] = { H264_TOTAL_ZEROS_DC_1, H264_TOTAL_ZEROS_DC_2, H264_TOTAL_ZEROS_DC_3 };
static const int H264_TZ_DC_N[3] = {
    (int)(sizeof(H264_TOTAL_ZEROS_DC_1) / sizeof(h264_vlc)), (int)(sizeof(H264_TOTAL_ZEROS_DC_2) / sizeof(h264_vlc)),
    (int)(sizeof(H264_TOTAL_ZEROS_DC_3) / sizeof(h264_vlc)),
};

/* A residual block by CAVLC (7.3.5.3.2), nC as 9.2.1 gives it (-1 for
   chroma DC). Levels of positions start..end into lv; *total gets
   TotalCoeff. -1 when the codes do not decode. */
static int vlc_residual(h264_dec *d, int nc, int start, int end, int maxn, short *lv, int *total) {
    for (int i = 0; i < maxn; i++) lv[i] = 0;
    int tc, t1;
    if (nc >= 8) {
        int v = (int)sd_u(d, 6);
        if (v == 3) { tc = 0; t1 = 0; } else { tc = (v >> 2) + 1; t1 = v & 3; }
    } else {
        int tab = nc < 0 ? 3 : nc < 2 ? 0 : nc < 4 ? 1 : 2;
        if (vlc_read(d, H264_CT[tab], H264_CT_N[tab], &tc, &t1) < 0) return -1;
    }
    *total = tc;
    if (!tc) return 0;
    if (tc > end - start + 1) return -1;
    int level[16], suffix_len = tc > 10 && t1 < 3 ? 1 : 0;
    for (int i = 0; i < tc; i++) {
        if (i < t1) { level[i] = sd_bit(d) ? -1 : 1; continue; }
        int prefix = 0;
        while (!sd_bit(d)) { prefix++; if (prefix > 31) return -1; }
        int code = h264_min(15, prefix) << suffix_len;
        int size = (prefix == 14 && suffix_len == 0) ? 4 : prefix >= 15 ? prefix - 3 : suffix_len;
        if (size > 0) code += (int)sd_u(d, size);
        if (prefix >= 15 && suffix_len == 0) code += 15;
        if (prefix >= 16) code += (1 << (prefix - 3)) - 4096;
        if (i == t1 && t1 < 3) code += 2;
        int v = (code % 2 == 0) ? (code + 2) >> 1 : (-code - 1) >> 1;
        level[i] = v;
        if (suffix_len == 0) suffix_len = 1;
        if (h264_abs(v) > (3 << (suffix_len - 1)) && suffix_len < 6) suffix_len++;
    }
    int zeros = 0;
    if (tc < end - start + 1) {
        if (maxn == 4) {
            if (vlc_read(d, H264_TZ_DC[tc - 1], H264_TZ_DC_N[tc - 1], &zeros, 0) < 0) return -1;
        } else {
            if (vlc_read(d, H264_TOTAL_ZEROS[tc - 1], H264_TOTAL_ZEROS_N[tc - 1], &zeros, 0) < 0) return -1;
        }
    }
    int run[16];
    for (int i = 0; i < tc - 1; i++) {
        run[i] = 0;
        if (zeros > 0) {
            int k = h264_min(zeros, 7) - 1;
            if (vlc_read(d, H264_RUN_BEFORE[k], H264_RUN_BEFORE_N[k], &run[i], 0) < 0) return -1;
        }
        zeros -= run[i];
        if (zeros < 0) return -1;
    }
    run[tc - 1] = zeros;
    int pos = -1;
    for (int i = tc - 1; i >= 0; i--) {
        pos += run[i] + 1;
        if (start + pos > end) return -1;
        lv[start + pos] = (short)level[i];
    }
    return 0;
}

/* nC for a luma 4x4 (raster blk) or, with c >= 0, a chroma AC block. */
static int vlc_nc(h264_dec *d, int blk, int c) {
    int n[2], ok[2];
    for (int k = 0; k < 2; k++) {
        int x, y;
        if (c < 0) { x = (blk & 3) * 4; y = (blk >> 2) * 4; }
        else { x = (blk & 1) * 8; y = (blk >> 1) * 8; }
        if (k == 0) x--; else y--;
        int nb, addr = h264_nb(d, x, y, &nb);
        ok[k] = addr >= 0;
        if (!ok[k]) continue;
        int idx = c < 0 ? nb : 16 + 4 * c + ((((y + 16) & 15) >> 3) << 1) + (((x + 16) & 15) >> 3);
        if (addr != d->m.addr && (d->mb_kind[addr] & H264_K_PCM)) n[k] = 16;
        else if (addr != d->m.addr && (d->mb_kind[addr] & H264_K_SKIP)) n[k] = 0;
        else n[k] = d->nzc[addr][idx];
    }
    if (ok[0] && ok[1]) return (n[0] + n[1] + 1) >> 1;
    if (ok[0]) return n[0];
    if (ok[1]) return n[1];
    return 0;
}

/* --- the residual (7.3.5.3) ------------------------------------------------------------------- */

static int h264_residual(h264_dec *d) {
    h264_mbctx *m = &d->m;
    int cabac = d->pp->cabac;
    int intra = (m->kind & H264_K_INTRA) != 0;
    int qp = m->qp, qpc[2] = { h264_qpc(d, qp, 0), h264_qpc(d, qp, 1) };
    const int *ls = d->ls4[intra ? 0 : 3][qp % 6];
    short lv[64];
    int a = m->addr;
    /* The flags go straight into the macroblock's record: the next block's
       context reads the ones before it in this macroblock. */
    u32 *cbf = &d->mb_cbf[a];
    *cbf = 0;
    for (int i = 0; i < 24; i++) d->nzc[a][i] = 0;
    m->coded = 0;
    m->dc_coded = m->cdc_coded = m->cac_coded = 0;
    if (m->kind & H264_K_I16) {
        int total = 0, nz;
        if (cabac) nz = cab_residual(d, 0, 0, 0, 16, lv);
        else { if (vlc_residual(d, vlc_nc(d, 0, -1), 0, 15, 16, lv, &total) < 0) return -1; nz = total; }
        h264_trace_levels("lumadc", lv, 16);
        if (nz) *cbf |= 1u << 16;
        short c[16];
        for (int k = 0; k < 16; k++) c[H264_ZIGZAG4[k]] = lv[k];
        int dc[16];
        h264_luma_dc(c, dc, ls, qp);
        for (int r = 0; r < 16; r++) {
            m->dc[r] = (short)dc[r];
            if (dc[r]) m->dc_coded |= 1 << r;
        }
    }
    for (int b8 = 0; b8 < 4; b8++) {
        if (!((m->cbp >> b8) & 1)) continue;
        if (m->t8 && cabac) {
            int nz = cab_residual(d, 5, b8, 0, 64, lv);
            h264_trace_levels("luma8", lv, 64);
            const int *l8 = d->ls8[intra ? 0 : 1][qp % 6];
            short *co = m->coef8[b8];
            for (int k = 0; k < 64; k++) co[k] = 0;
            for (int k = 0; k < 64; k++) if (lv[k]) co[H264_ZIGZAG8[k]] = (short)h264_dq8(l8, lv[k], qp, H264_ZIGZAG8[k]);
            if (nz) {
                m->coded |= 1 << b8;
                for (int s = 0; s < 4; s++) {
                    int r = H264_BLK_R[b8 * 4 + s];
                    *cbf |= 1u << r;
                    d->nzc[a][r] = (u8)nz;
                }
            }
            continue;
        }
        if (m->t8) {
            /* CAVLC sends an 8x8 as four 4x4s, interleaved. */
            short *co = m->coef8[b8];
            short l8[64];
            for (int k = 0; k < 64; k++) { co[k] = 0; l8[k] = 0; }
            int any = 0;
            for (int s = 0; s < 4; s++) {
                int r = H264_BLK_R[b8 * 4 + s], total = 0;
                if (vlc_residual(d, vlc_nc(d, r, -1), 0, 15, 16, lv, &total) < 0) return -1;
                h264_trace_levels("luma", lv, 16);
                d->nzc[a][r] = (u8)total;
                for (int k = 0; k < 16; k++) l8[4 * k + s] = lv[k];
                any |= total;
            }
            const int *ls8 = d->ls8[intra ? 0 : 1][qp % 6];
            for (int k = 0; k < 64; k++) if (l8[k]) co[H264_ZIGZAG8[k]] = (short)h264_dq8(ls8, l8[k], qp, H264_ZIGZAG8[k]);
            if (any) m->coded |= 1 << b8;
            continue;
        }
        for (int s = 0; s < 4; s++) {
            int r = H264_BLK_R[b8 * 4 + s], nz, total = 0;
            int ac = (m->kind & H264_K_I16) != 0;
            if (cabac) nz = cab_residual(d, ac ? 1 : 2, r, 0, ac ? 15 : 16, lv);
            else { if (vlc_residual(d, vlc_nc(d, r, -1), 0, ac ? 14 : 15, ac ? 15 : 16, lv, &total) < 0) return -1; nz = total; }
            h264_trace_levels("luma", lv, ac ? 15 : 16);
            d->nzc[a][r] = (u8)nz;
            short *co = m->coef[r];
            for (int k = 0; k < 16; k++) co[k] = 0;
            if (nz) {
                *cbf |= 1u << r;
                m->coded |= 1 << r;
                for (int k = 0; k < (ac ? 15 : 16); k++)
                    if (lv[k]) { int p = H264_ZIGZAG4[k + ac]; co[p] = (short)h264_dq4(ls, lv[k], qp, p); }
            }
        }
    }
    if (m->cbp >> 4) {
        for (int c = 0; c < 2; c++) {
            int nz, total = 0;
            if (cabac) nz = cab_residual(d, 3, 0, c, 4, lv);
            else { if (vlc_residual(d, -1, 0, 3, 4, lv, &total) < 0) return -1; nz = total; }
            h264_trace_levels("chromadc", lv, 4);
            if (nz) *cbf |= 1u << (17 + c);
            int dc[4];
            h264_chroma_dc(lv, dc, d->ls4[(intra ? 1 : 4) + c][qpc[c] % 6], qpc[c]);
            for (int k = 0; k < 4; k++) { m->cdc[c][k] = (short)dc[k]; if (dc[k]) m->cdc_coded |= 1 << (4 * c + k); }
        }
    }
    if ((m->cbp >> 4) == 2) {
        for (int c = 0; c < 2; c++) {
            const int *lc = d->ls4[(intra ? 1 : 4) + c][qpc[c] % 6];
            for (int b = 0; b < 4; b++) {
                int nz, total = 0;
                if (cabac) nz = cab_residual(d, 4, b, c, 15, lv);
                else { if (vlc_residual(d, vlc_nc(d, b, c), 0, 14, 15, lv, &total) < 0) return -1; nz = total; }
                h264_trace_levels("chromaac", lv, 15);
                d->nzc[a][16 + 4 * c + b] = (u8)nz;
                short *co = m->cac[c][b];
                for (int k = 0; k < 16; k++) co[k] = 0;
                if (nz) {
                    *cbf |= 1u << (19 + 4 * c + b);
                    m->cac_coded |= 1 << (4 * c + b);
                    for (int k = 0; k < 15; k++)
                        if (lv[k]) { int p = H264_ZIGZAG4[k + 1]; co[p] = (short)h264_dq4(lc, lv[k], qpc[c], p); }
                }
            }
        }
    }

    return 0;
}

/* --- adding the residual ---------------------------------------------------------------------- */

static void h264_add_luma(h264_dec *d) {
    h264_mbctx *m = &d->m;
    int W = d->mb_w * 16;
    u8 *base = d->cur->y + m->y * 16 * W + m->x * 16;
    if (m->t8) {
        for (int b8 = 0; b8 < 4; b8++)
            if ((m->coded >> b8) & 1) h264_idct8_add(m->coef8[b8], base + (b8 >> 1) * 8 * W + (b8 & 1) * 8, W);
        return;
    }
    for (int r = 0; r < 16; r++) {
        if (m->kind & H264_K_I16) {
            if (!((m->coded >> r) & 1)) for (int k = 0; k < 16; k++) m->coef[r][k] = 0;
            m->coef[r][0] = m->dc[r];
            if (!(((m->coded | m->dc_coded) >> r) & 1)) continue;
        } else if (!((m->coded >> r) & 1)) {
            continue;
        }
        h264_idct4_add(m->coef[r], base + (r >> 2) * 4 * W + (r & 3) * 4, W);
    }
}

static void h264_add_chroma(h264_dec *d) {
    h264_mbctx *m = &d->m;
    int CW = d->mb_w * 8;
    for (int c = 0; c < 2; c++) {
        u8 *base = (c ? d->cur->cr : d->cur->cb) + m->y * 8 * CW + m->x * 8;
        for (int b = 0; b < 4; b++) {
            int ac = (m->cac_coded >> (4 * c + b)) & 1, dc = (m->cdc_coded >> (4 * c + b)) & 1;
            if (!ac && !dc) continue;
            if (!ac) for (int k = 0; k < 16; k++) m->cac[c][b][k] = 0;
            m->cac[c][b][0] = dc ? m->cdc[c][b] : 0;
            h264_idct4_add(m->cac[c][b], base + (b >> 1) * 4 * CW + (b & 1) * 4, CW);
        }
    }
}

/* --- a macroblock (7.3.5) ----------------------------------------------------------------------- */

static const u8 H264_B_PRED0[23] = { 0, 1, 2, 3, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 3, 3, 3, 3, 3, 3, 0 };
static const u8 H264_B_PRED1[23] = { 0, 0, 0, 0, 1, 1, 2, 2, 2, 2, 1, 1, 3, 3, 3, 3, 1, 1, 2, 2, 3, 3, 0 };
static const u8 H264_BSUB_SHAPE[13] = { 0, 0, 0, 0, 1, 2, 1, 2, 1, 2, 3, 3, 3 };
static const u8 H264_BSUB_PRED[13] = { 3, 1, 2, 3, 1, 1, 2, 2, 3, 3, 1, 2, 3 };

/* Where the sub-partitions of an 8x8 of a shape are, in 4x4 units. */
static void h264_subparts(int shape, int k, int *x, int *y, int *w, int *h) {
    switch (shape) {
    case 0: *x = 0; *y = 0; *w = 2; *h = 2; break;
    case 1: *x = 0; *y = k; *w = 2; *h = 1; break;
    case 2: *x = k; *y = 0; *w = 1; *h = 2; break;
    default: *x = k & 1; *y = k >> 1; *w = 1; *h = 1; break;
    }
}
static const u8 H264_SUBN[4] = { 1, 2, 2, 4 };

static int read_ref(h264_dec *d, int l, int x, int y) {
    int n = d->sl.num_ref[l];
    if (n <= 1) return 0;
    if (d->pp->cabac) return cab_ref(d, l, x, y);
    if (n == 2) return !sd_bit(d);
    return (int)sd_ue(d);
}

static int read_mvd(h264_dec *d, int l, int c, int x, int y) {
    if (d->pp->cabac) return cab_mvd(d, l, c, x, y);
    return sd_se(d);
}

/* How many partitions a layout has: parts is the layout (1 16x16, 2 16x8,
   3 8x16, 4 8x8), not the count. */
#define H264_NPARTS(parts) ((parts) == 1 ? 1 : (parts) == 4 ? 4 : 2)

/* Partition p of the macroblock's layout, in luma samples. */
static void h264_part(int parts, int p, int *x, int *y, int *w, int *h) {
    if (parts == 1) { *x = 0; *y = 0; *w = 16; *h = 16; }
    else if (parts == 2) { *x = 0; *y = p * 8; *w = 16; *h = 8; }
    else { *x = p * 8; *y = 0; *w = 8; *h = 16; }
}

static void h264_store_mvd(h264_dec *d, int l, int bx4, int by4, int w4, int h4, int mx, int my) {
    int ax = h264_abs(mx), ay = h264_abs(my);
    if (ax > 255) ax = 255;
    if (ay > 255) ay = 255;
    for (int j = by4; j < by4 + h4; j++)
        for (int i = bx4; i < bx4 + w4; i++) {
            d->mvd[d->m.addr][l][j * 4 + i][0] = (u8)ax;
            d->mvd[d->m.addr][l][j * 4 + i][1] = (u8)ay;
            d->m.mvd[l][j * 4 + i][0] = (short)mx;
            d->m.mvd[l][j * 4 + i][1] = (short)my;
        }
}

/* The macroblock's motion, worked out and predicted, partition by
   partition (the order later ones' prediction needs). */
static void h264_inter_mb(h264_dec *d) {
    h264_mbctx *m = &d->m;
    m->done = 0;
    if (m->kind & H264_K_SKIP && d->sl.type == H264_P) {
        int mx, my;
        h264_pskip_mv(d, &mx, &my);
        h264_set_motion(d, 0, 0, 4, 4, 0, 0, mx, my);
        h264_set_motion(d, 0, 0, 4, 4, 1, -1, 0, 0);
        h264_mark_done(d, 0, 0, 4, 4);
        short mv[2] = { (short)mx, (short)my }, z[2] = { 0, 0 };
        h264_inter_block(d, 0, 0, 16, 16, 1, 0, 0, mv, z);
        return;
    }
    if (m->kind & H264_K_DIRECT) {
        for (int b8 = 0; b8 < 4; b8++) h264_direct(d, b8);
        return;
    }
    if (m->parts < 4) {
        for (int p = 0; p < H264_NPARTS(m->parts); p++) {
            int x, y, w, h;
            h264_part(m->parts, p, &x, &y, &w, &h);
            short mv[2][2] = { { 0, 0 }, { 0, 0 } };
            int ref[2] = { -1, -1 };
            for (int l = 0; l < 2; l++) {
                if (!((m->pred[p] >> l) & 1)) { h264_set_motion(d, x / 4, y / 4, w / 4, h / 4, l, -1, 0, 0); continue; }
                int px, py;
                ref[l] = m->ref[l][(y / 8) * 2 + x / 8];
                h264_mvp(d, l, ref[l], x, y, w, h, &px, &py);
                mv[l][0] = (short)(px + m->mvd[l][(y / 4) * 4 + x / 4][0]);
                mv[l][1] = (short)(py + m->mvd[l][(y / 4) * 4 + x / 4][1]);
                h264_set_motion(d, x / 4, y / 4, w / 4, h / 4, l, ref[l], mv[l][0], mv[l][1]);
            }
            h264_mark_done(d, x / 4, y / 4, w / 4, h / 4);
            h264_inter_block(d, x, y, w, h, m->pred[p], ref[0] < 0 ? 0 : ref[0], ref[1] < 0 ? 0 : ref[1], mv[0], mv[1]);
        }
        return;
    }
    for (int b8 = 0; b8 < 4; b8++) {
        if (d->sl.type == H264_B && m->sub[b8] == 0) { h264_direct(d, b8); continue; }
        int shape = d->sl.type == H264_B ? H264_BSUB_SHAPE[m->sub[b8]] : m->sub[b8];
        int pf = m->subpred[b8];
        int ox = (b8 & 1) * 2, oy = (b8 >> 1) * 2;
        for (int k = 0; k < H264_SUBN[shape]; k++) {
            int sx, sy, sw, sh;
            h264_subparts(shape, k, &sx, &sy, &sw, &sh);
            int x4 = ox + sx, y4 = oy + sy;
            short mv[2][2] = { { 0, 0 }, { 0, 0 } };
            int ref[2] = { -1, -1 };
            for (int l = 0; l < 2; l++) {
                if (!((pf >> l) & 1)) { h264_set_motion(d, x4, y4, sw, sh, l, -1, 0, 0); continue; }
                int px, py;
                ref[l] = m->ref[l][b8];
                h264_mvp(d, l, ref[l], x4 * 4, y4 * 4, sw * 4, sh * 4, &px, &py);
                mv[l][0] = (short)(px + m->mvd[l][y4 * 4 + x4][0]);
                mv[l][1] = (short)(py + m->mvd[l][y4 * 4 + x4][1]);
                h264_set_motion(d, x4, y4, sw, sh, l, ref[l], mv[l][0], mv[l][1]);
            }
            h264_mark_done(d, x4, y4, sw, sh);
            h264_inter_block(d, x4 * 4, y4 * 4, sw * 4, sh * 4, pf, ref[0] < 0 ? 0 : ref[0], ref[1] < 0 ? 0 : ref[1], mv[0], mv[1]);
        }
    }
}

/* The intra 4x4 or 8x8 modes from what was sent (8.3.1.1, 8.3.2.1). */
static int h264_pred_mode(h264_dec *d, int bx, int by, int n) {
    int modes[2];
    for (int k = 0; k < 2; k++) {
        int blk, addr = h264_nb(d, k ? bx : bx - 1, k ? by - 1 : by, &blk);
        if (addr < 0 || (addr != d->m.addr && d->pp->constrained_intra && !(d->mb_kind[addr] & H264_K_INTRA))) return 2;
        int kind = addr == d->m.addr ? d->m.kind : d->mb_kind[addr];
        if (!(kind & H264_K_INXN)) { modes[k] = 2; continue; }
        if (n == 8 && addr != d->m.addr && !d->mb_t8[addr]) {
            /* An 8x8 next to a macroblock of 4x4s takes the 4x4 nearest. */
            int b8 = ((blk >> 3) << 1) | ((blk & 3) >> 1);
            int r = H264_BLK_R[b8 * 4 + (k ? 2 : 1)];
            modes[k] = d->ipm[addr][r];
        } else {
            modes[k] = addr == d->m.addr ? d->m.ipm[blk] : d->ipm[addr][blk];
        }
    }
    return h264_min(modes[0], modes[1]);
}

static void h264_store_mb(h264_dec *d) {
    h264_mbctx *m = &d->m;
    int a = m->addr;
    d->mb_kind[a] = (u8)m->kind;
    d->mb_cbp[a] = (u8)m->cbp;
    d->mb_t8[a] = (u8)m->t8;
    d->mb_cpred[a] = (u8)m->cpred;
    d->mb_qp[a] = (signed char)m->qp;
    d->mb_qpc[0][a] = (signed char)h264_qpc(d, m->qp, 0);
    d->mb_qpc[1][a] = (signed char)h264_qpc(d, m->qp, 1);
    d->mb_dbidc[a] = (u8)d->sl.deblock_idc;
    d->mb_alpha[a] = (signed char)d->sl.alpha_off;
    d->mb_beta[a] = (signed char)d->sl.beta_off;
    for (int r = 0; r < 16; r++) d->ipm[a][r] = (m->kind & H264_K_INXN) ? m->ipm[r] : 2;
    u16 nz = 0;
    if (m->t8) {
        for (int b8 = 0; b8 < 4; b8++)
            if ((m->coded >> b8) & 1) for (int s = 0; s < 4; s++) nz |= (u16)(1 << H264_BLK_R[b8 * 4 + s]);
    } else if (!(m->kind & H264_K_I16)) {
        nz = (u16)m->coded;
    }
    d->mb_nz[a] = nz;
    d->cur->intra[a] = (u8)((m->kind & H264_K_INTRA) != 0);
    if (m->kind & H264_K_INTRA)
        for (int l = 0; l < 2; l++) {
            for (int b = 0; b < 4; b++) { d->cur->ri[a][l][b] = -1; d->cur->refid[a][l][b] = 0; }
            for (int b = 0; b < 16; b++) d->cur->mv[a][l][b][0] = d->cur->mv[a][l][b][1] = 0;
        }
}

static int h264_macroblock(h264_dec *d, int skipped) {
    h264_mbctx *m = &d->m;
    h264_slice *s = &d->sl;
    int cabac = d->pp->cabac;
    int a = m->addr;
    m->kind = 0;
    m->cbp = 0;
    m->t8 = 0;
    m->cpred = 0;
    m->parts = 1;
    m->coded = m->dc_coded = m->cdc_coded = m->cac_coded = 0;
    d->mb_direct8[a] = 0;
    d->mb_cbf[a] = 0;
    d->mb_slice[a] = (short)s->number;
    for (int l = 0; l < 2; l++)
        for (int b = 0; b < 16; b++) {
            d->mvd[a][l][b][0] = d->mvd[a][l][b][1] = 0;
            m->mvd[l][b][0] = m->mvd[l][b][1] = 0;
        }
    for (int l = 0; l < 2; l++) for (int b = 0; b < 4; b++) m->ref[l][b] = 0;
    for (int i = 0; i < 24; i++) d->nzc[a][i] = 0;

    H264_T("MB %d slice %d type %d\n", a, s->number, s->type);
    if (skipped) {
        H264_T("skip\n");
        m->kind = H264_K_SKIP | (s->type == H264_B ? H264_K_DIRECT : 0);
        m->qp = d->qp_prev;
        d->last_dqp = 0;
        if (s->type == H264_B) d->mb_direct8[a] = 15;
        h264_store_mb(d);
        h264_inter_mb(d);
        d->mb_nz[a] = 0;
        return 0;
    }

    int mbt = cabac ? cab_mb_type(d) : (int)sd_ue(d);
    H264_T("mb_type %d\n", mbt);
    int imb = -1;
    if (s->type == H264_I) imb = mbt;
    else if (s->type == H264_P) { if (mbt >= 5) imb = mbt - 5; }
    else if (mbt >= 23) imb = mbt - 23;
    if (imb > 25 || (s->type == H264_P && mbt > 30) || (s->type == H264_B && mbt > 48)) {
        h264_say(d, "a macroblock type that does not exist");
        return -1;
    }

    if (imb >= 0 && s->type != H264_I) d->seen |= H264_SEEN_INTRA_IN_P;
    if (imb == 25) {
        d->seen |= H264_SEEN_PCM;
        /* I_PCM: the samples as they are, byte aligned. */
        m->kind = H264_K_INTRA | H264_K_PCM;
        d->pos = (d->pos + 7) & ~7;
        for (int i = 0; i < 384; i++) m->pcm[i] = (u8)sd_u(d, 8);
        if (cabac) cab_start(d);
        m->qp = 0;
        m->cbp = 0x2F;
        d->last_dqp = 0;
        d->mb_cbf[a] = 0x7FFFFFF;
        for (int i = 0; i < 24; i++) d->nzc[a][i] = 16;
        int W = d->mb_w * 16, CW = W / 2;
        u8 *y = d->cur->y + m->y * 16 * W + m->x * 16;
        for (int j = 0; j < 16; j++) for (int i = 0; i < 16; i++) y[j * W + i] = m->pcm[j * 16 + i];
        for (int c = 0; c < 2; c++) {
            u8 *p = (c ? d->cur->cr : d->cur->cb) + m->y * 8 * CW + m->x * 8;
            for (int j = 0; j < 8; j++) for (int i = 0; i < 8; i++) p[j * CW + i] = m->pcm[256 + c * 64 + j * 8 + i];
        }
        m->coded = 0;
        h264_store_mb(d);
        d->mb_nz[a] = 0xFFFF;
        d->mb_qp[a] = 0;
        d->mb_qpc[0][a] = (signed char)h264_qpc(d, 0, 0);
        d->mb_qpc[1][a] = (signed char)h264_qpc(d, 0, 1);
        d->qp_prev = 0;
        for (int i = 0; i < 24; i++) d->nzc[a][i] = 16;
        return 0;
    }

    if (imb >= 0) {
        m->kind = H264_K_INTRA;
        if (imb == 0) {
            m->kind |= H264_K_INXN;
            if (d->pp->transform_8x8) m->t8 = cabac ? cab_t8(d) : sd_bit(d);
            if (m->t8) d->seen |= H264_SEEN_INTRA8;
            int n = m->t8 ? 4 : 16;
            int prev[16], rem[16];
            for (int i = 0; i < n; i++) {
                prev[i] = cabac ? cab(d, 68) : sd_bit(d);
                rem[i] = 0;
                if (!prev[i]) {
                    if (cabac) { rem[i] = cab(d, 69); rem[i] |= cab(d, 69) << 1; rem[i] |= cab(d, 69) << 2; }
                    else rem[i] = (int)sd_u(d, 3);
                }
                H264_T("ipm %d\n", prev[i] ? -1 : rem[i]);
            }
            /* The modes depend on the ones decided before them, so they are
               worked out in decoding order now. */
            for (int i = 0; i < n; i++) {
                int bx = m->t8 ? (i & 1) * 8 : H264_BLK_X[i], by = m->t8 ? (i >> 1) * 8 : H264_BLK_Y[i];
                int pred = h264_pred_mode(d, bx, by, m->t8 ? 8 : 4);
                int mode = prev[i] ? pred : rem[i] < pred ? rem[i] : rem[i] + 1;
                if (m->t8) {
                    for (int s4 = 0; s4 < 4; s4++) m->ipm[H264_BLK_R[i * 4 + s4]] = (signed char)mode;
                } else {
                    m->ipm[H264_BLK_R[i]] = (signed char)mode;
                }
            }
        } else {
            m->kind |= H264_K_I16;
            m->i16mode = (imb - 1) % 4;
            m->cbp = (((imb - 1) / 4) % 3) << 4 | (imb >= 13 ? 15 : 0);
        }
        m->cpred = cabac ? cab_cpred(d) : (int)sd_ue(d);
        H264_T("cpred %d\n", m->cpred);
        if (m->cpred > 3) { h264_say(d, "a chroma prediction mode that does not exist"); return -1; }
    } else {
        int no_small = 1;
        if (s->type == H264_P) {
            if (mbt < 3) {
                m->parts = mbt + 1;
                for (int p = 0; p < H264_NPARTS(m->parts); p++) m->pred[p] = 1;
            } else {
                m->parts = 4;
            }
        } else {
            if (mbt == 0) {
                m->kind = H264_K_DIRECT;
                d->mb_direct8[a] = 15;
                no_small = d->sp->direct_8x8_inference;
            } else if (mbt < 4) {
                m->parts = 1;
                m->pred[0] = H264_B_PRED0[mbt];
            } else if (mbt < 22) {
                m->parts = (mbt & 1) ? 3 : 2;
                m->pred[0] = H264_B_PRED0[mbt];
                m->pred[1] = H264_B_PRED1[mbt];
            } else {
                m->parts = 4;
            }
        }
        if (m->parts == 4) {
            m->kind |= H264_K_B8X8 * (s->type == H264_B);
            for (int b8 = 0; b8 < 4; b8++) {
                m->sub[b8] = cabac ? cab_sub_type(d) : (int)sd_ue(d);
                H264_T("sub %d\n", m->sub[b8]);
                if (m->sub[b8] > (s->type == H264_B ? 12 : 3)) { h264_say(d, "a sub-macroblock type that does not exist"); return -1; }
                if (s->type == H264_B) {
                    m->subpred[b8] = m->sub[b8] ? H264_BSUB_PRED[m->sub[b8]] : 0;
                    if (!m->sub[b8]) { d->mb_direct8[a] |= (u8)(1 << b8); if (!d->sp->direct_8x8_inference) no_small = 0; }
                    else if (H264_BSUB_SHAPE[m->sub[b8]]) no_small = 0;
                } else {
                    m->subpred[b8] = 1;
                    if (m->sub[b8]) no_small = 0;
                }
            }
            for (int l = 0; l < 2; l++)
                for (int b8 = 0; b8 < 4; b8++) {
                    int x = (b8 & 1) * 8, y = (b8 >> 1) * 8;
                    if (!((m->subpred[b8] >> l) & 1)) { m->ref[l][b8] = -1; continue; }
                    m->ref[l][b8] = (s->type == H264_P && mbt == 4) ? 0 : read_ref(d, l, x, y);
                    H264_T("ref%d %d\n", l, m->ref[l][b8]);
                    if (m->ref[l][b8] >= s->num_ref[l]) { h264_say(d, "a reference index past the list"); return -1; }
                }
            for (int l = 0; l < 2; l++)
                for (int b8 = 0; b8 < 4; b8++) {
                    if (!((m->subpred[b8] >> l) & 1)) continue;
                    int shape = s->type == H264_B ? H264_BSUB_SHAPE[m->sub[b8]] : m->sub[b8];
                    for (int k = 0; k < H264_SUBN[shape]; k++) {
                        int sx, sy, sw, sh;
                        h264_subparts(shape, k, &sx, &sy, &sw, &sh);
                        int x4 = (b8 & 1) * 2 + sx, y4 = (b8 >> 1) * 2 + sy;
                        int mx = read_mvd(d, l, 0, x4 * 4, y4 * 4);
                        int my = read_mvd(d, l, 1, x4 * 4, y4 * 4);
                        H264_T("mvd%d %d %d\n", l, mx, my);
                        h264_store_mvd(d, l, x4, y4, sw, sh, mx, my);
                    }
                }
        } else if (!(m->kind & H264_K_DIRECT)) {
            for (int l = 0; l < 2; l++)
                for (int p = 0; p < H264_NPARTS(m->parts); p++) {
                    int x, y, w, h;
                    h264_part(m->parts, p, &x, &y, &w, &h);
                    int r = -1;
                    if ((m->pred[p] >> l) & 1) {
                        r = read_ref(d, l, x, y);
                        H264_T("ref%d %d\n", l, r);
                        if (r >= s->num_ref[l]) { h264_say(d, "a reference index past the list"); return -1; }
                    }
                    /* Kept for each 8x8 the partition covers: the next
                       partition's context reads it that way. */
                    for (int b8 = 0; b8 < 4; b8++) {
                        int bx = (b8 & 1) * 8, by = (b8 >> 1) * 8;
                        if (bx >= x && bx < x + w && by >= y && by < y + h) m->ref[l][b8] = r;
                    }
                }
            for (int l = 0; l < 2; l++)
                for (int p = 0; p < H264_NPARTS(m->parts); p++) {
                    if (!((m->pred[p] >> l) & 1)) continue;
                    int x, y, w, h;
                    h264_part(m->parts, p, &x, &y, &w, &h);
                    int mx = read_mvd(d, l, 0, x, y);
                    int my = read_mvd(d, l, 1, x, y);
                    H264_T("mvd%d %d %d\n", l, mx, my);
                    h264_store_mvd(d, l, x / 4, y / 4, w / 4, h / 4, mx, my);
                }
        }
        m->no_small = no_small;
    }

    if (!(m->kind & H264_K_I16)) {
        if (cabac) m->cbp = cab_cbp(d);
        else {
            u32 k = sd_ue(d);
            if (k > 47) { h264_say(d, "a coded block pattern that does not exist"); return -1; }
            m->cbp = (m->kind & H264_K_INTRA) ? H264_CBP_INTRA[k] : H264_CBP_INTER[k];
            m->cbp = (m->cbp & 15) | ((m->cbp >> 4) << 4);
        }
        if ((m->cbp & 15) && d->pp->transform_8x8 && !(m->kind & H264_K_INTRA) && m->no_small &&
            (!(m->kind & H264_K_DIRECT) || d->sp->direct_8x8_inference))
            m->t8 = cabac ? cab_t8(d) : sd_bit(d);
    }

    if (!(m->kind & H264_K_I16)) H264_T("cbp %d\n", m->cbp);
    int dqp = 0;
    if (m->cbp || (m->kind & H264_K_I16)) {
        dqp = cabac ? cab_qp_delta(d) : sd_se(d);
        H264_T("dqp %d\n", dqp);
        if (dqp < -26 || dqp > 25) { h264_say(d, "a quantiser change out of range"); return -1; }
    }
    d->last_dqp = dqp;
    m->qp = (d->qp_prev + dqp + 52) % 52;
    d->qp_prev = m->qp;

    if (m->cbp || (m->kind & H264_K_I16)) {
        if (h264_residual(d) < 0) { h264_say(d, "a residual block that does not decode"); return -1; }
    } else {
        d->mb_cbf[a] = 0;
    }
    if (m->t8) d->seen |= H264_SEEN_T8;
    /* What the neighbours' contexts read must be there before the next
       macroblock, and the reference indices of this one before its
       prediction reads them back. */
    h264_store_mb(d);

    if (m->kind & H264_K_INTRA) {
        if (m->kind & H264_K_I16) {
            h264_intra16(d, m->i16mode);
            h264_add_luma(d);
        } else {
            int W = d->mb_w * 16;
            u8 *base = d->cur->y + m->y * 16 * W + m->x * 16;
            if (m->t8) {
                for (int b8 = 0; b8 < 4; b8++) {
                    h264_intra8(d, (b8 & 1) * 8, (b8 >> 1) * 8, m->ipm[H264_BLK_R[b8 * 4]]);
                    if ((m->coded >> b8) & 1) h264_idct8_add(m->coef8[b8], base + (b8 >> 1) * 8 * W + (b8 & 1) * 8, W);
                }
            } else {
                for (int i = 0; i < 16; i++) {
                    int r = H264_BLK_R[i];
                    h264_intra4(d, H264_BLK_X[i], H264_BLK_Y[i], m->ipm[r]);
                    if ((m->coded >> r) & 1) h264_idct4_add(m->coef[r], base + H264_BLK_Y[i] * W + H264_BLK_X[i], W);
                }
            }
        }
        h264_intra_chroma(d, m->cpred);
    } else {
        h264_inter_mb(d);
        h264_add_luma(d);
    }
    h264_add_chroma(d);
    return 0;
}

/* --- the slice data (7.3.4) ------------------------------------------------------------------- */

static int h264_slice_data(h264_dec *d, h264_bits *b) {
    h264_slice *s = &d->sl;
    d->data = b->p;
    d->data_n = b->n;
    d->pos = b->pos;
    d->qp_prev = s->qp;
    d->last_dqp = 0;
    int cabac = d->pp->cabac;
    if (cabac) {
        d->pos = (d->pos + 7) & ~7;
        cab_contexts(d);
        cab_start(d);
    }
    int addr = s->first_mb;
    if (addr >= d->mbs) { h264_say(d, "a slice that starts past the picture"); return -1; }
    for (;;) {
        int skip = 0;
        if (s->type != H264_I) {
            if (!cabac) {
                u32 run = sd_ue(d);
                if (run > (u32)(d->mbs - addr)) { h264_say(d, "a run of skipped macroblocks past the picture"); return -1; }
                for (u32 i = 0; i < run; i++) {
                    d->m.addr = addr; d->m.x = addr % d->mb_w; d->m.y = addr / d->mb_w;
                    h264_macroblock(d, 1);
                    d->mbs_decoded++;
                    addr++;
                }
                if (addr >= d->mbs || (run && !hb_more(&(h264_bits){ d->data, d->data_n, d->pos }))) break;
            } else {
                d->m.addr = addr; d->m.x = addr % d->mb_w; d->m.y = addr / d->mb_w;
                skip = cab_skip(d);
            }
        }
        d->m.addr = addr; d->m.x = addr % d->mb_w; d->m.y = addr / d->mb_w;
        if (h264_macroblock(d, skip) < 0) {
            /* What it left is half written: no neighbour may read it, and
               the filter passes it by. */
            d->mb_slice[addr] = -1;
            return -1;
        }
        d->mbs_decoded++;
        addr++;
        if (cabac) {
            if (cab_term(d)) break;
        } else if (!hb_more(&(h264_bits){ d->data, d->data_n, d->pos })) {
            break;
        }
        if (addr >= d->mbs) { h264_say(d, "a slice that runs past the picture"); return -1; }
    }
    return 0;
}
