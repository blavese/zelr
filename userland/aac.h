#pragma once
/* AAC, the low complexity profile (MPEG-4 audio object type 2), which is
 * the audio of Twitch's streams, of YouTube's MP4 formats and of most other
 * video on the web. Written from ISO/IEC 14496-3's description of the
 * stream; the constant tables are generated (tools/mkaac.py, aactab.h).
 *
 * A frame is 1024 samples a channel. What is decoded, in the standard's
 * order: the elements of a raw data block (one channel, a pair, and the fill
 * and data elements that carry nothing for the sound); for each channel the
 * window, the sections and their codebooks, the scale factors, pulses,
 * temporal noise shaping and the spectral values; then the values made
 * linear and scaled, mid/side and intensity stereo, noise substitution,
 * noise shaping, and the inverse MDCT -- a DCT-IV through a complex FFT of a
 * quarter of the window -- windowed (sine or Kaiser-Bessel-derived) and
 * overlapped with the half the frame before left behind.
 *
 * What it takes: one or two channels; the nine sampling rates from 8 kHz to
 * 48 kHz; an ADTS stream (Twitch's) or the AudioSpecificConfig an MP4 track
 * carries (YouTube's). Not here: more than two channels, the coupling
 * channel element, and the extensions that ride in fill elements (SBR, so
 * HE-AAC plays as its low band only, as the standard allows). */
#include "zelr.h"
#include "aactab.h"

#define AAC_FRAME   1024
#define AAC_MAX_CH  2

/* --- bits ---------------------------------------------------------------------------- */

typedef struct {
    const u8 *p;
    int n;               /* bytes */
    int pos;             /* bits read */
    int bad;             /* read past the end */
} aac_bits;

static inline u32 aac_get(aac_bits *b, int k) {
    u32 v = 0;
    for (int i = 0; i < k; i++) {
        int byte = b->pos >> 3;
        if (byte >= b->n) { b->bad = 1; b->pos++; v <<= 1; continue; }
        v = (v << 1) | ((b->p[byte] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

/* --- arithmetic this needs, without a maths library ------------------------------------- */

#define AAC_PI 3.14159265358979323846

static double aac_sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

/* sin and cos by the series, after bringing the angle within a quarter turn. */
static double aac_sin(double x) {
    double tp = 2 * AAC_PI;
    long long k = (long long)(x / tp);
    x -= (double)k * tp;
    if (x < 0) x += tp;
    int neg = 0;
    if (x > AAC_PI) { x -= AAC_PI; neg = 1; }
    if (x > AAC_PI / 2) x = AAC_PI - x;
    double x2 = x * x, term = x, sum = x;
    for (int i = 1; i < 12; i++) {
        term *= -x2 / (double)((2 * i) * (2 * i + 1));
        sum += term;
    }
    return neg ? -sum : sum;
}

static double aac_cos(double x) { return aac_sin(x + AAC_PI / 2); }

/* 2^(e/4), e any integer in range, exactly to the quarter. */
static double aac_pow2q(int e) {
    static const double Q[4] = { 1.0, 1.1892071150027210667, 1.4142135623730950488, 1.6817928305074290861 };
    int whole = e >> 2;                         /* floor division by four */
    double v = Q[e & 3];
    for (; whole > 0; whole--) v *= 2.0;
    for (; whole < 0; whole++) v *= 0.5;
    return v;
}

/* The zeroth modified Bessel function, for the Kaiser-Bessel-derived window. */
static double aac_bessel0(double x) {
    double sum = 1.0, term = 1.0, h = x / 2;
    for (int k = 1; k < 60; k++) {
        term *= (h / k) * (h / k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

/* --- tables made once ------------------------------------------------------------------ */

/* A codebook as a binary tree: to[bit] is a node (above 0), a codeword's
   entry (-(i + 1)), or nothing (0, which no codeword may reach). */
typedef struct { short to[2]; } aac_node;

static aac_node aac_tree[12][292];
static float aac_pow43[8192];
static float aac_win_long[2][1024], aac_win_short[2][128];   /* [shape][rising half] */
static float aac_pre_l[2][512], aac_post_l[2][512], aac_fft_l[2][256];
static float aac_pre_s[2][64], aac_post_s[2][64], aac_fft_s[2][32];
static int aac_ready;

static const aac_hcode *const AAC_BOOK[12] = {
    AAC_HCB1, AAC_HCB2, AAC_HCB3, AAC_HCB4, AAC_HCB5, AAC_HCB6,
    AAC_HCB7, AAC_HCB8, AAC_HCB9, AAC_HCB10, AAC_HCB11, AAC_HCB_SF
};
static const short AAC_BOOK_N[12] = { 81, 81, 81, 81, 81, 81, 64, 64, 169, 169, 289, 121 };

static int aac_tree_make(int b) {
    aac_node *t = aac_tree[b];
    int used = 1;
    t[0].to[0] = t[0].to[1] = 0;
    for (int i = 0; i < AAC_BOOK_N[b]; i++) {
        const aac_hcode *c = &AAC_BOOK[b][i];
        int at = 0;
        for (int k = c->len - 1; k >= 0; k--) {
            int bit = (int)((c->code >> k) & 1);
            if (k == 0) {
                if (t[at].to[bit]) return 0;
                t[at].to[bit] = (short)-(i + 1);
            } else {
                if (t[at].to[bit] < 0) return 0;
                if (!t[at].to[bit]) {
                    if (used >= 292) return 0;
                    t[used].to[0] = t[used].to[1] = 0;
                    t[at].to[bit] = (short)used++;
                }
                at = t[at].to[bit];
            }
        }
    }
    return 1;
}

/* q^(4/3), from a cube root found by Newton's method. */
static double aac_cbrt(double q) {
    if (q <= 0) return 0;
    double x = 1;
    while (x * x * x < q) x *= 2;
    for (int i = 0; i < 40; i++) x = (2 * x + q / (x * x)) / 3;
    return x;
}

static void aac_twiddles(int m, float *pre, float *post, float *fft) {
    for (int k = 0; k < m / 2; k++) {
        double a = -AAC_PI * (4 * k + 1) / (4.0 * m);
        pre[2 * k] = (float)aac_cos(a); pre[2 * k + 1] = (float)aac_sin(a);
        double b = -AAC_PI * k / m;
        post[2 * k] = (float)aac_cos(b); post[2 * k + 1] = (float)aac_sin(b);
    }
    for (int k = 0; k < m / 4; k++) {
        double c = -2 * AAC_PI * k / (m / 2.0);
        fft[2 * k] = (float)aac_cos(c); fft[2 * k + 1] = (float)aac_sin(c);
    }
}

static void aac_kbd(float *w, int n, double alpha) {
    /* Kaiser kernel over n/2 + 1 points, summed and normalised. */
    double total = 0, run = 0;
    int half = n / 2;
    for (int p = 0; p <= half; p++) {
        double r = (p - half / 2.0) / (half / 2.0);
        total += aac_bessel0(AAC_PI * alpha * aac_sqrt(1 - r * r > 0 ? 1 - r * r : 0));
    }
    for (int p = 0; p < half; p++) {
        double r = (p - half / 2.0) / (half / 2.0);
        run += aac_bessel0(AAC_PI * alpha * aac_sqrt(1 - r * r > 0 ? 1 - r * r : 0));
        w[p] = (float)aac_sqrt(run / total);
    }
}

static int aac_init_tables(void) {
    if (aac_ready) return aac_ready > 0;
    aac_ready = -1;
    for (int b = 0; b < 12; b++) if (!aac_tree_make(b)) return 0;
    for (int q = 0; q < 8192; q++) aac_pow43[q] = (float)(q * aac_cbrt((double)q));
    for (int n = 0; n < 1024; n++) aac_win_long[0][n] = (float)aac_sin(AAC_PI / 2048 * (n + 0.5));
    for (int n = 0; n < 128; n++) aac_win_short[0][n] = (float)aac_sin(AAC_PI / 256 * (n + 0.5));
    aac_kbd(aac_win_long[1], 2048, 4.0);
    aac_kbd(aac_win_short[1], 256, 6.0);
    aac_twiddles(1024, aac_pre_l[0], aac_post_l[0], aac_fft_l[0]);
    aac_twiddles(128, aac_pre_s[0], aac_post_s[0], aac_fft_s[0]);
    aac_ready = 1;
    return 1;
}

static int aac_huff(aac_bits *b, int book) {
    const aac_node *t = aac_tree[book];
    int at = 0;
    for (int depth = 0; depth < 32; depth++) {
        int v = t[at].to[aac_get(b, 1)];
        if (v < 0) return -v - 1;
        if (!v || b->bad) return -1;
        at = v;
    }
    return -1;
}

/* --- the transform ------------------------------------------------------------------------ */

/* In place, n complex values (re, im interleaved), forward. */
static void aac_fft(float *a, int n, const float *tw) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) {
            float tr = a[2 * i], ti = a[2 * i + 1];
            a[2 * i] = a[2 * j]; a[2 * i + 1] = a[2 * j + 1];
            a[2 * j] = tr; a[2 * j + 1] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; k++) {
                float wr = tw[2 * k * step], wi = tw[2 * k * step + 1];
                float *u = &a[2 * (i + k)], *v = &a[2 * (i + k + len / 2)];
                float xr = v[0] * wr - v[1] * wi, xi = v[0] * wi + v[1] * wr;
                v[0] = u[0] - xr; v[1] = u[1] - xi;
                u[0] += xr; u[1] += xi;
            }
        }
    }
}

/* The inverse MDCT of m coefficients into 2m samples, scaled by 2/(2m) as
   the standard has it: a DCT-IV of m points, unfolded. */
static void aac_imdct(const float *x, float *y, int m) {
    float t[1024], u[1024];
    const float *pre = m == 1024 ? aac_pre_l[0] : aac_pre_s[0];
    const float *post = m == 1024 ? aac_post_l[0] : aac_post_s[0];
    const float *tw = m == 1024 ? aac_fft_l[0] : aac_fft_s[0];
    int h = m / 2;
    for (int k = 0; k < h; k++) {
        float re = x[2 * k], im = x[m - 1 - 2 * k];
        t[2 * k] = re * pre[2 * k] - im * pre[2 * k + 1];
        t[2 * k + 1] = re * pre[2 * k + 1] + im * pre[2 * k];
    }
    aac_fft(t, h, tw);
    for (int n = 0; n < h; n++) {
        float re = t[2 * n] * post[2 * n] - t[2 * n + 1] * post[2 * n + 1];
        float im = t[2 * n] * post[2 * n + 1] + t[2 * n + 1] * post[2 * n];
        u[2 * n] = re;
        u[m - 1 - 2 * n] = -im;
    }
    float scale = 1.0f / (float)m;
    int n2 = 2 * m, q = n2 / 4;
    for (int n = 0; n < n2; n++) {
        float v;
        if (n < q) v = u[q + n];
        else if (n < 3 * q) v = -u[3 * q - 1 - n];
        else v = -u[n - 3 * q];
        y[n] = v * scale;
    }
}

/* --- one channel's stream -------------------------------------------------------------- */

enum { AAC_ONLY_LONG, AAC_LONG_START, AAC_EIGHT_SHORT, AAC_LONG_STOP };
enum { AAC_ZERO_HCB = 0, AAC_ESC_HCB = 11, AAC_NOISE_HCB = 13, AAC_INTENSITY2 = 14, AAC_INTENSITY = 15 };

typedef struct {
    int seq, shape, max_sfb;
    int ngroups, group_len[8];
    int nwin, nswb;
    const short *swb;                 /* band starts for one window */
} aac_info;

typedef struct {
    aac_info info;
    u8 cb[8][64];                     /* each group's band's codebook */
    short sf[8][64];                  /* and its scale factor (or position, or energy) */
    float spec[1024];                 /* [window * 128 + k] for short windows */
    int tns, nfilt[8], coef_res[8], len[8][4], order[8][4], dir[8][4], comp[8][4];
    signed char coef[8][4][32];
} aac_chan;

typedef struct {
    int rate_idx;                     /* into AAC_RATES */
    int channels;
    float overlap[AAC_MAX_CH][1024];
    int prev_shape[AAC_MAX_CH];
    u32 noise_seed;
    aac_chan ch[AAC_MAX_CH];
    char why[64];
    /* What the stream has used so far, counted: for a test to know which
       parts of the decoder it reached. */
    int seen_short, seen_tns, seen_ms, seen_is, seen_noise, seen_pulse;
} aac_dec;

/* The standard's sampling frequency index (0 is 96 kHz) to a row of
   AAC_RATES, or -1 for a rate this decoder does not take. */
static int aac_rate_row(int sfi) {
    static const int RATE[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    if (sfi < 0 || sfi > 12) return -1;
    for (int i = 0; i < 9; i++) if (AAC_RATES[i].rate == RATE[sfi]) return i;
    return -1;
}

static int aac_open(aac_dec *d, int sfi, int channels) {
    if (!aac_init_tables()) return 0;
    for (u32 i = 0; i < sizeof(*d); i++) ((volatile u8 *)d)[i] = 0;
    d->rate_idx = aac_rate_row(sfi);
    d->channels = channels;
    d->noise_seed = 0x1F2E3D4Cu;
    if (d->rate_idx < 0 || channels < 1 || channels > AAC_MAX_CH) return 0;
    return 1;
}

static int aac_fail(aac_dec *d, const char *why) {
    int i = 0;
    for (; why[i] && i < (int)sizeof(d->why) - 1; i++) d->why[i] = why[i];
    d->why[i] = 0;
    return -1;
}

static int aac_ics_info(aac_dec *d, aac_bits *b, aac_info *in) {
    const aac_rate *r = &AAC_RATES[d->rate_idx];
    aac_get(b, 1);                                   /* reserved */
    in->seq = (int)aac_get(b, 2);
    in->shape = (int)aac_get(b, 1);
    if (in->seq == AAC_EIGHT_SHORT) {
        in->max_sfb = (int)aac_get(b, 4);
        int grouping = (int)aac_get(b, 7);
        in->nwin = 8;
        in->nswb = r->nshort;
        in->swb = r->shrt;
        in->ngroups = 1;
        in->group_len[0] = 1;
        for (int w = 1; w < 8; w++) {
            if (grouping & (1 << (6 - (w - 1)))) in->group_len[in->ngroups - 1]++;
            else in->group_len[in->ngroups++] = 1;
        }
    } else {
        in->max_sfb = (int)aac_get(b, 6);
        if (aac_get(b, 1)) return aac_fail(d, "a predictor, which is not low complexity");
        in->nwin = 1;
        in->nswb = r->nlong;
        in->swb = r->lng;
        in->ngroups = 1;
        in->group_len[0] = 1;
    }
    if (in->max_sfb > in->nswb) return aac_fail(d, "more bands than the window has");
    return 0;
}

/* One channel's stream. Its spectrum is left made linear and scaled; noise
   and intensity bands are left at zero for the stereo and noise steps. */
static int aac_ics(aac_dec *d, aac_bits *b, aac_chan *c, int common) {
    aac_info *in = &c->info;
    int global = (int)aac_get(b, 8);
    if (!common && aac_ics_info(d, b, in) < 0) return -1;
    int short_win = in->seq == AAC_EIGHT_SHORT;

    /* Sections: runs of bands that share a codebook. */
    int lbits = short_win ? 3 : 5, esc = (1 << lbits) - 1;
    for (int g = 0; g < in->ngroups; g++) {
        int k = 0;
        while (k < in->max_sfb) {
            int cbk = (int)aac_get(b, 4);
            if (cbk == 12) return aac_fail(d, "a reserved codebook");
            int len = 0, incr;
            while ((incr = (int)aac_get(b, lbits)) == esc) { len += esc; if (b->bad) return aac_fail(d, "a frame cut short"); }
            len += incr;
            if (k + len > in->max_sfb) return aac_fail(d, "a section past the last band");
            for (int s = 0; s < len; s++) c->cb[g][k + s] = (u8)cbk;
            k += len;
            if (b->bad) return aac_fail(d, "a frame cut short");
        }
    }

    /* Scale factors, each a difference from the one before of its kind. */
    int sf = global, is_pos = 0, noise = global - 90, noise_first = 1;
    for (int g = 0; g < in->ngroups; g++) {
        for (int s = 0; s < in->max_sfb; s++) {
            int cbk = c->cb[g][s];
            if (cbk == AAC_ZERO_HCB) { c->sf[g][s] = 0; continue; }
            if (cbk == AAC_INTENSITY || cbk == AAC_INTENSITY2) {
                int e = aac_huff(b, 11);
                if (e < 0) return aac_fail(d, "a bad scale factor");
                is_pos += AAC_HCB_SF[e].v[0];
                c->sf[g][s] = (short)is_pos;
            } else if (cbk == AAC_NOISE_HCB) {
                if (noise_first) { noise_first = 0; noise += (int)aac_get(b, 9) - 256; }
                else {
                    int e = aac_huff(b, 11);
                    if (e < 0) return aac_fail(d, "a bad noise energy");
                    noise += AAC_HCB_SF[e].v[0];
                }
                c->sf[g][s] = (short)noise;
            } else {
                int e = aac_huff(b, 11);
                if (e < 0) return aac_fail(d, "a bad scale factor");
                sf += AAC_HCB_SF[e].v[0];
                if (sf < 0 || sf > 255) return aac_fail(d, "a scale factor out of range");
                c->sf[g][s] = (short)sf;
            }
        }
    }

    /* Pulses, which only a long window has. */
    int npulse = 0, pulse_start = 0, poff[4], pamp[4];
    if (aac_get(b, 1)) {
        if (short_win) return aac_fail(d, "pulses in a short window");
        npulse = (int)aac_get(b, 2) + 1;
        d->seen_pulse++;
        pulse_start = (int)aac_get(b, 6);
        for (int i = 0; i < npulse; i++) { poff[i] = (int)aac_get(b, 5); pamp[i] = (int)aac_get(b, 4); }
    }

    /* Temporal noise shaping: read now, applied after the stereo steps. */
    c->tns = (int)aac_get(b, 1);
    if (c->tns) {
        d->seen_tns++;
        for (int w = 0; w < in->nwin; w++) {
            c->nfilt[w] = (int)aac_get(b, short_win ? 1 : 2);
            if (!c->nfilt[w]) continue;
            c->coef_res[w] = (int)aac_get(b, 1);
            for (int f = 0; f < c->nfilt[w]; f++) {
                c->len[w][f] = (int)aac_get(b, short_win ? 4 : 6);
                c->order[w][f] = (int)aac_get(b, short_win ? 3 : 5);
                if (!c->order[w][f]) continue;
                c->dir[w][f] = (int)aac_get(b, 1);
                c->comp[w][f] = (int)aac_get(b, 1);
                int bits = c->coef_res[w] + 3 - c->comp[w][f];
                for (int i = 0; i < c->order[w][f]; i++) {
                    int v = (int)aac_get(b, bits);
                    if (v & (1 << (bits - 1))) v -= 1 << bits;
                    c->coef[w][f][i] = (signed char)v;
                }
            }
        }
    }
    if (aac_get(b, 1)) return aac_fail(d, "gain control, which is not low complexity");

    /* The spectral values, group by group in the stream's interleaved order:
       each band of every window of the group, then the next band. */
    static int q[1024];
    for (int i = 0; i < 1024; i++) c->spec[i] = 0;
    int win0 = 0;
    for (int g = 0; g < in->ngroups; g++) {
        int glen = in->group_len[g];
        int at = 0;
        for (int i = 0; i < 1024; i++) q[i] = 0;
        int start[65];
        for (int s = 0; s <= in->max_sfb; s++) { start[s] = at; if (s < in->max_sfb) at += (in->swb[s + 1] - in->swb[s]) * glen; }
        for (int s = 0; s < in->max_sfb; s++) {
            int cbk = c->cb[g][s];
            if (cbk == AAC_ZERO_HCB || cbk > AAC_ESC_HCB) continue;
            int dim = cbk < 5 ? 4 : 2;
            int unsigned_book = cbk >= 3 && cbk != 5 && cbk != 6;
            for (int k = start[s]; k < start[s + 1]; k += dim) {
                int e = aac_huff(b, cbk - 1);
                if (e < 0) return aac_fail(d, "a bad spectral codeword");
                int v[4];
                for (int j = 0; j < dim; j++) v[j] = AAC_BOOK[cbk - 1][e].v[j];
                if (unsigned_book)
                    for (int j = 0; j < dim; j++) if (v[j] && aac_get(b, 1)) v[j] = -v[j];
                if (cbk == AAC_ESC_HCB) {
                    for (int j = 0; j < 2; j++) {
                        int a = v[j] < 0 ? -v[j] : v[j];
                        if (a != 16) continue;
                        int n = 0;
                        while (aac_get(b, 1)) { if (++n > 8 || b->bad) return aac_fail(d, "a bad escape"); }
                        a = (1 << (n + 4)) + (int)aac_get(b, n + 4);
                        v[j] = v[j] < 0 ? -a : a;
                    }
                }
                for (int j = 0; j < dim; j++) q[k + j] = v[j];
            }
        }
        if (b->bad) return aac_fail(d, "a frame cut short");

        /* Pulses add to what was decoded, before it is made linear. */
        if (npulse && g == 0) {
            int k = in->swb[pulse_start < in->max_sfb ? pulse_start : in->max_sfb];
            for (int i = 0; i < npulse; i++) {
                k += poff[i];
                if (k >= 1024) return aac_fail(d, "a pulse past the end");
                q[k] += q[k] > 0 ? pamp[i] : -pamp[i];
            }
        }

        /* Out of the interleaving, linear and scaled: sign * |q|^(4/3) * 2^((sf - 100) / 4). */
        for (int s = 0; s < in->max_sfb; s++) {
            int cbk = c->cb[g][s];
            if (cbk == AAC_ZERO_HCB || cbk > AAC_ESC_HCB) continue;
            float gain = (float)aac_pow2q(c->sf[g][s] - 100);
            int width = in->swb[s + 1] - in->swb[s];
            for (int w = 0; w < glen; w++) {
                for (int j = 0; j < width; j++) {
                    int v = q[start[s] + w * width + j];
                    int a = v < 0 ? -v : v;
                    float x = a < 8192 ? aac_pow43[a] : (float)(a * aac_cbrt((double)a));
                    c->spec[(win0 + w) * 128 * short_win + in->swb[s] + j] = (v < 0 ? -x : x) * gain;
                }
            }
        }
        win0 += glen;
    }
    return 0;
}

/* Noise in place of a band's values, with the band's energy. */
static void aac_noise(aac_dec *d, aac_chan *c, int g, int s, int win0) {
    aac_info *in = &c->info;
    int width = in->swb[s + 1] - in->swb[s];
    int short_win = in->seq == AAC_EIGHT_SHORT;
    for (int w = 0; w < in->group_len[g]; w++) {
        float *x = &c->spec[(win0 + w) * 128 * short_win + in->swb[s]];
        double e = 0;
        for (int j = 0; j < width; j++) {
            d->noise_seed = d->noise_seed * 1664525u + 1013904223u;
            x[j] = (float)(int)d->noise_seed;
            e += (double)x[j] * x[j];
        }
        double scale = aac_pow2q(c->sf[g][s]) / aac_sqrt(e > 0 ? e : 1);
        for (int j = 0; j < width; j++) x[j] = (float)(x[j] * scale);
    }
}

static void aac_tns(aac_dec *d, aac_chan *c) {
    aac_info *in = &c->info;
    const aac_rate *r = &AAC_RATES[d->rate_idx];
    int short_win = in->seq == AAC_EIGHT_SHORT;
    int tmax = short_win ? r->tns_short : r->tns_long, omax = short_win ? 7 : 12;
    for (int w = 0; w < in->nwin; w++) {
        int bottom = in->nswb;
        for (int f = 0; f < c->nfilt[w]; f++) {
            int top = bottom;
            bottom = top - c->len[w][f] > 0 ? top - c->len[w][f] : 0;
            int order = c->order[w][f] < omax ? c->order[w][f] : omax;
            if (!order) continue;
            /* The coefficients as reflection coefficients, then as a filter. */
            int res = c->coef_res[w] + 3;
            double iq = ((1 << (res - 1)) - 0.5) / (AAC_PI / 2), iqm = ((1 << (res - 1)) + 0.5) / (AAC_PI / 2);
            double refl[32], a[33], t[33];
            for (int i = 0; i < order; i++) {
                int v = c->coef[w][f][i];
                refl[i] = aac_sin(v / (v >= 0 ? iq : iqm));
            }
            a[0] = 1;
            for (int m = 1; m <= order; m++) {
                for (int i = 1; i < m; i++) t[i] = a[i] + refl[m - 1] * a[m - i];
                for (int i = 1; i < m; i++) a[i] = t[i];
                a[m] = refl[m - 1];
            }
            int lo = bottom < tmax ? bottom : tmax; if (lo > in->max_sfb) lo = in->max_sfb;
            int hi = top < tmax ? top : tmax; if (hi > in->max_sfb) hi = in->max_sfb;
            int start = in->swb[lo], end = in->swb[hi], size = end - start;
            if (size <= 0) continue;
            float *x = &c->spec[w * 128 * short_win];
            int inc = 1, at = start;
            if (c->dir[w][f]) { inc = -1; at = end - 1; }
            double state[32];
            for (int i = 0; i < order; i++) state[i] = 0;
            for (int n = 0; n < size; n++, at += inc) {
                double y = x[at];
                for (int i = 0; i < order; i++) y -= a[i + 1] * state[i];
                for (int i = order - 1; i > 0; i--) state[i] = state[i - 1];
                state[0] = y;
                x[at] = (float)y;
            }
        }
    }
}

/* Inverse transform, window and overlap: 1024 samples out for one channel. */
static void aac_synth(aac_dec *d, int ch, float *out) {
    aac_chan *c = &d->ch[ch];
    aac_info *in = &c->info;
    int prev = d->prev_shape[ch], cur = in->shape;
    const float *ll = aac_win_long[prev], *lr = aac_win_long[cur];
    const float *sl = aac_win_short[prev], *sr = aac_win_short[cur];
    static float buf[2048];
    float *ov = d->overlap[ch];
    if (in->seq == AAC_EIGHT_SHORT) {
        static float y[256];
        for (int i = 0; i < 2048; i++) buf[i] = 0;
        for (int w = 0; w < 8; w++) {
            aac_imdct(&c->spec[w * 128], y, 128);
            const float *rise = w == 0 ? sl : sr;
            for (int n = 0; n < 128; n++) {
                buf[448 + w * 128 + n] += y[n] * rise[n];
                buf[448 + w * 128 + 128 + n] += y[128 + n] * sr[127 - n];
            }
        }
    } else {
        aac_imdct(c->spec, buf, 1024);
        if (in->seq == AAC_LONG_STOP) {
            for (int n = 0; n < 448; n++) buf[n] = 0;
            for (int n = 0; n < 128; n++) buf[448 + n] *= sl[n];
        } else {
            for (int n = 0; n < 1024; n++) buf[n] *= ll[n];
        }
        if (in->seq == AAC_LONG_START) {
            for (int n = 0; n < 128; n++) buf[1472 + n] *= sr[127 - n];
            for (int n = 1600; n < 2048; n++) buf[n] = 0;
        } else {
            for (int n = 0; n < 1024; n++) buf[1024 + n] *= lr[1023 - n];
        }
    }
    for (int n = 0; n < 1024; n++) {
        out[n] = buf[n] + ov[n];
        ov[n] = buf[1024 + n];
    }
    d->prev_shape[ch] = cur;
}

/* --- a frame --------------------------------------------------------------------------- */

static int aac_stereo(aac_dec *d, int ms_present, u8 ms[8][64]) {
    aac_chan *l = &d->ch[0], *r = &d->ch[1];
    aac_info *in = &l->info;
    int short_win = in->seq == AAC_EIGHT_SHORT;
    int win0 = 0;
    for (int g = 0; g < in->ngroups; g++) {
        for (int s = 0; s < in->max_sfb; s++) {
            int width = in->swb[s + 1] - in->swb[s];
            int lc = l->cb[g][s], rc = r->cb[g][s];
            int used = ms_present == 2 || (ms_present == 1 && ms[g][s]);
            for (int w = 0; w < in->group_len[g]; w++) {
                float *a = &l->spec[(win0 + w) * 128 * short_win + in->swb[s]];
                float *b = &r->spec[(win0 + w) * 128 * short_win + in->swb[s]];
                if (rc == AAC_INTENSITY || rc == AAC_INTENSITY2) {
                    d->seen_is++;
                    float scale = (float)aac_pow2q(-r->sf[g][s]);
                    if (rc == AAC_INTENSITY2) scale = -scale;
                    if (ms_present == 1 && ms[g][s]) scale = -scale;
                    for (int j = 0; j < width; j++) b[j] = a[j] * scale;
                } else if (used && lc != AAC_NOISE_HCB && rc != AAC_NOISE_HCB) {
                    d->seen_ms++;
                    for (int j = 0; j < width; j++) {
                        float m = a[j], sd = b[j];
                        a[j] = m + sd;
                        b[j] = m - sd;
                    }
                }
            }
        }
        win0 += in->group_len[g];
    }
    return 0;
}

static void aac_noise_bands(aac_dec *d, aac_chan *c) {
    aac_info *in = &c->info;
    int win0 = 0;
    for (int g = 0; g < in->ngroups; g++) {
        for (int s = 0; s < in->max_sfb; s++)
            if (c->cb[g][s] == AAC_NOISE_HCB) { aac_noise(d, c, g, s, win0); d->seen_noise++; }
        win0 += in->group_len[g];
    }
}

/* A raw data block into 1024 samples a channel, interleaved, as 16-bit.
   Answers the samples a channel, or -1 with d->why. */
static int aac_decode(aac_dec *d, const u8 *data, int n, short *pcm) {
    aac_bits b = { data, n, 0, 0 };
    int got = 0;                                /* channels decoded */
    for (int guard = 0; guard < 64; guard++) {
        int id = (int)aac_get(&b, 3);
        if (b.bad) return aac_fail(d, "a frame cut short");
        if (id == 7) break;                     /* the end */
        if (id == 0 || id == 3) {               /* one channel, or the low frequency one */
            aac_get(&b, 4);
            if (got >= d->channels) {           /* one channel more than asked for: decoded, not kept */
                static aac_chan spare;
                if (aac_ics(d, &b, &spare, 0) < 0) return -1;
                continue;
            }
            if (aac_ics(d, &b, &d->ch[got], 0) < 0) return -1;
            aac_noise_bands(d, &d->ch[got]);
            if (d->ch[got].tns) aac_tns(d, &d->ch[got]);
            got++;
        } else if (id == 1) {                   /* a pair */
            aac_get(&b, 4);
            if (got + 2 > d->channels) return aac_fail(d, "a channel pair in a stream of one");
            int common = (int)aac_get(&b, 1);
            int ms_present = 0;
            static u8 ms[8][64];
            if (common) {
                if (aac_ics_info(d, &b, &d->ch[got].info) < 0) return -1;
                d->ch[got + 1].info = d->ch[got].info;
                ms_present = (int)aac_get(&b, 2);
                if (ms_present == 1) {
                    aac_info *in = &d->ch[got].info;
                    for (int g = 0; g < in->ngroups; g++)
                        for (int s = 0; s < in->max_sfb; s++) ms[g][s] = (u8)aac_get(&b, 1);
                }
            }
            if (aac_ics(d, &b, &d->ch[got], common) < 0) return -1;
            if (aac_ics(d, &b, &d->ch[got + 1], common) < 0) return -1;
            if (common) aac_stereo(d, ms_present, ms);
            aac_noise_bands(d, &d->ch[got]);
            aac_noise_bands(d, &d->ch[got + 1]);
            for (int k = 0; k < 2; k++) if (d->ch[got + k].tns) aac_tns(d, &d->ch[got + k]);
            got += 2;
        } else if (id == 4) {                   /* data, for nobody here */
            aac_get(&b, 4);
            int align = (int)aac_get(&b, 1);
            int count = (int)aac_get(&b, 8);
            if (count == 255) count += (int)aac_get(&b, 8);
            if (align) b.pos = (b.pos + 7) & ~7;
            b.pos += count * 8;
        } else if (id == 6) {                   /* fill, and the extensions in it */
            int count = (int)aac_get(&b, 4);
            if (count == 15) count += (int)aac_get(&b, 8) - 1;
            b.pos += count * 8;
        } else {
            return aac_fail(d, id == 2 ? "a coupling channel" : "a program config element");
        }
        if (b.pos > n * 8) return aac_fail(d, "a frame cut short");
    }
    if (got < d->channels) return aac_fail(d, "fewer channels than the stream said");

    static float out[AAC_MAX_CH][1024];
    for (int ch = 0; ch < d->channels; ch++) {
        if (d->ch[ch].info.seq == AAC_EIGHT_SHORT) d->seen_short++;
        aac_synth(d, ch, out[ch]);
    }
    for (int i = 0; i < 1024; i++)
        for (int ch = 0; ch < d->channels; ch++) {
            float v = out[ch][i];
            int s = (int)(v < 0 ? v - 0.5f : v + 0.5f);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            pcm[i * d->channels + ch] = (short)s;
        }
    return 1024;
}

/* --- the ways a stream arrives ------------------------------------------------------------ */

/* An ADTS frame's header: its whole length, where its raw data starts, the
   rate and the channels. 0 when this is not one. */
typedef struct { int frame_len, header_len, sfi, channels, profile; } aac_adts;

__attribute__((unused)) static int aac_adts_read(const u8 *p, int n, aac_adts *h) {
    if (n < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return 0;
    int protection_absent = p[1] & 1;
    h->profile = (p[2] >> 6) + 1;                /* the object type: 2 is low complexity */
    h->sfi = (p[2] >> 2) & 15;
    h->channels = ((p[2] & 1) << 2) | (p[3] >> 6);
    h->frame_len = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
    h->header_len = protection_absent ? 7 : 9;
    if (h->frame_len < h->header_len || (p[6] & 3) != 0) return 0;   /* one raw block a frame */
    return 1;
}

/* An MP4 track's AudioSpecificConfig: the object type, rate and channels.
   The player reads ADTS, which carries the same in every frame. */
__attribute__((unused)) static int aac_config_read(const u8 *p, int n, int *sfi, int *channels) {
    aac_bits b = { p, n, 0, 0 };
    int type = (int)aac_get(&b, 5);
    if (type == 31) type = 32 + (int)aac_get(&b, 6);
    *sfi = (int)aac_get(&b, 4);
    if (*sfi == 15) return 0;                    /* a rate written out: none here */
    *channels = (int)aac_get(&b, 4);
    if (b.bad) return 0;
    return type == 2 || type == 5 || type == 29 ? 1 : 0;   /* HE-AAC carries LC underneath */
}
