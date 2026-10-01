/* The AAC decoder, against streams it did not make.
 *
 * The streams come from tools/genaac.py: sounds made there, encoded by
 * Windows' own AAC encoder, and the answer is what Windows' own decoder
 * makes of the same frames. Two decoders working in floating point do not
 * agree to the last bit, so the measure is how far apart they are: the
 * signal to the difference, in decibels, over a stretch from well inside
 * each sound, at the alignment where they agree best (the two may leave out
 * different amounts of the encoder's start-up delay).
 *
 * One sound comes again the way Twitch sends a stream -- ADTS frames in PES
 * packets in an MPEG transport stream -- for ts.h and the ADTS reader.
 *
 * And the transform on its own, against the formula it stands for.
 */
#include "zelr.h"
#include "alloc.h"
#include "aac.h"
#include "ts.h"
#include "aacdata.h"

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

/* 10 log10(x), near enough to read a margin by. */
static double db(double x) {
    if (x <= 0) return -999;
    int e = 0;
    while (x >= 10) { x /= 10; e++; }
    while (x < 1) { x *= 10; e--; }
    /* log10 of 1 to 10 by the series for ln, which converges well here */
    double y = (x - 1) / (x + 1), y2 = y * y, s = 0, t = y;
    for (int k = 1; k < 40; k += 2) { s += t / k; t *= y2; }
    return 10.0 * (e + 2 * s / 2.302585092994046);
}

/* The inverse MDCT straight from its definition, for m coefficients. */
static void imdct_slow(const float *x, double *y, int m) {
    int n2 = 2 * m;
    double n0 = n2 / 4.0 + 0.5;
    for (int n = 0; n < n2; n++) {
        double s = 0;
        for (int k = 0; k < m; k++) s += x[k] * aac_cos(2 * AAC_PI / n2 * (n + n0) * (k + 0.5));
        y[n] = s / m;
    }
}

static int transform_close(int m) {
    static float x[1024], y[2048];
    static double want[2048];
    u32 seed = 12345;
    for (int i = 0; i < m; i++) {
        seed = seed * 1103515245u + 12345u;
        x[i] = (float)((int)(seed >> 8) % 20001 - 10000);
    }
    aac_imdct(x, y, m);
    imdct_slow(x, want, m);
    double err = 0, sig = 0;
    for (int n = 0; n < 2 * m; n++) {
        double d = y[n] - want[n];
        err += d * d;
        sig += want[n] * want[n];
    }
    return (int)db(sig / (err > 0 ? err : 1e-30));
}

/* How close a stretch of the answer is to what was decoded, at the best of
   the alignments tried; *at gets the alignment. */
static double agree(const short *pcm, int total, int chans, const short *answer, int from, int len, int *at) {
    double best = -999;
    for (int off = -4096; off <= 4096; off++) {
        int f = from + off;
        if (f < 0 || f + len > total) continue;
        double sig = 0, err = 0;
        for (int i = 0; i < len * chans; i++) {
            double a = answer[i], b = pcm[f * chans + i];
            sig += a * a;
            err += (a - b) * (a - b);
        }
        double r = db(sig / (err > 0 ? err : 1e-9));
        if (r > best) { best = r; *at = off; }
    }
    return best;
}

/* The transport stream's frames, decoded as they come out of the demuxer. */
static aac_dec ts_dec;
static short ts_pcm[64 * 1024 * 2];
static int ts_opened, ts_frames, ts_bad, ts_pes;
static long long ts_first_pts = -2;

static void on_pes(void *ctx, int type, long long pts, const u8 *p, int n) {
    (void)ctx;
    if (type != TS_AAC) return;
    if (ts_first_pts == -2) ts_first_pts = pts;
    ts_pes++;
    int at = 0;
    while (at + 7 <= n) {
        aac_adts h;
        if (!aac_adts_read(p + at, n - at, &h) || at + h.frame_len > n) { ts_bad++; return; }
        if (!ts_opened && !(ts_opened = aac_open(&ts_dec, h.sfi, h.channels))) { ts_bad++; return; }
        if (ts_frames < 64 && aac_decode(&ts_dec, p + at + h.header_len, h.frame_len - h.header_len,
                                         ts_pcm + ts_frames * 1024 * 2) == 1024) ts_frames++;
        else ts_bad++;
        at += h.frame_len;
    }
}

int main(void) {
    puts("aac\n");
    ok("the tables make twelve whole codebooks", aac_init_tables());
    int t = transform_close(128);
    okn("the short transform is the formula it stands for, to (dB)", t > 100, t);
    t = transform_close(1024);
    okn("and so is the long one (dB)", t > 100, t);

    static short pcm[64 * 1024 * 2];
    for (int k = 0; k < (int)(sizeof(AAC_CASES) / sizeof(AAC_CASES[0])); k++) {
        const aac_case *c = &AAC_CASES[k];
        int sfi, chans;
        aac_dec *d = (aac_dec *)malloc(sizeof(aac_dec));
        int cfg = aac_config_read(c->config, c->config_len, &sfi, &chans);
        int opened = d && cfg && chans == c->channels && aac_open(d, sfi, chans);
        ok(c->name, opened);
        if (!opened) { free(d); continue; }
        const unsigned char *p = c->data;
        int frames = 0, bad = 0;
        for (int f = 0; f < c->frames && f < 64; f++) {
            int got = aac_decode(d, p, c->sizes[f], pcm + frames * 1024 * chans);
            p += c->sizes[f];
            if (got != 1024) {
                if (!bad) { puts("          frame "); putn(f); puts(": "); puts(d->why); putc('\n'); }
                bad++;
                continue;
            }
            frames++;
        }
        okn("  every frame decodes", bad == 0, bad);

        /* Where Windows' answer lines up with this, and how close it is there. */
        double best = -999;
        int at = 0;
        int total = frames * 1024;
        for (int off = -4096; off <= 4096; off++) {
            int from = c->answer_from + off;
            if (from < 0 || from + c->answer_len > total) continue;
            double sig = 0, err = 0;
            for (int i = 0; i < c->answer_len * chans; i++) {
                double a = c->answer[i], b = pcm[from * chans + i];
                sig += a * a;
                err += (a - b) * (a - b);
            }
            double r = db(sig / (err > 0 ? err : 1e-9));
            if (r > best) { best = r; at = off; }
        }
        okn("  and agrees with Windows' decoder, to (dB)", best > 50, (int)best);
        puts("          lined up "); putn(at); puts(" samples along; short windows "); putn(d->seen_short);
        puts(", noise shaping "); putn(d->seen_tns); puts(", mid/side bands "); putn(d->seen_ms);
        puts(", intensity bands "); putn(d->seen_is); puts(", noise bands "); putn(d->seen_noise);
        puts(", pulses "); putn(d->seen_pulse); putc('\n');
        if (k == 1) okn("  and the clicks were coded in short windows", d->seen_short > 0, d->seen_short);
        if (k == 3) okn("  and the noise in mid/side bands", d->seen_ms > 0, d->seen_ms);
        free(d);
    }
    ts_demux td;
    ts_init(&td, on_pes, 0);
    int bad = ts_feed(&td, AACD_TS, (int)sizeof(AACD_TS));
    ts_flush(&td);
    okn("a transport stream's packets are read, none of them bad", bad == 0 && td.packets > 100, td.packets);
    okn("and its AAC frames come out of it whole, a PES packet each", ts_bad == 0 && ts_frames == ts_pes && ts_frames > 40, ts_frames);
    okn("the first with its time (90 kHz ticks)", ts_first_pts == 90000, (int)ts_first_pts);
    int at = 0;
    double r = agree(ts_pcm, ts_frames * 1024, 2, AACD_TS_ANSWER, AACD_TS_FROM, AACD_TS_LEN, &at);
    okn("and what they make agrees with Windows' decoding of the same file, to (dB)", r > 50, (int)r);
    ts_free(&td);

    puts(failed ? "AACTEST_FAIL\n" : "AACTEST_PASS\n");
    return failed;
}
