/* The H.264 decoder, against streams it did not make.
 *
 * The streams come from tools/genh264.py: clips made there, encoded by
 * Windows' own H.264 encoder in four ways (CAVLC and CABAC, with and without
 * B pictures, a size that is cropped), and the answer is a checksum of each
 * plane of each frame Windows' own decoder made of them. Decoding H.264 is
 * exact, so the frames must be the same to the last sample, in the same
 * order.
 *
 * And damaged streams: a stream comes over the network, so the decoder must
 * survive anything in one -- bytes changed, a stream cut short -- by
 * refusing what does not decode, never by reading or writing where it
 * should not. Reaching the end of this program is that check.
 */
#include "zelr.h"
#include "alloc.h"
#include "h264.h"
#include "h264data.h"

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

static unsigned fnv(const u8 *p, int w, int h, int stride) {
    unsigned v = 2166136261u;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) v = (v ^ p[y * stride + x]) * 16777619u;
    return v;
}

static h264_dec dec;

/* Decodes a stream NAL by NAL, taking frames as they are due; with sums,
   compares each frame with them. Returns frames shown; *wrong gets the
   first frame that differs, or -1. */
static int run(const u8 *data, int len, const unsigned *sums, int nsums, int *wrong, int *w, int *h) {
    h264_open(&dec);
    *wrong = -1;
    int shown = 0, at = 0, s, n;
    h264_picture p;
    while (h264_next_nal(data, len, &at, &s, &n)) {
        h264_nal(&dec, data + s, n);
        while (h264_frame(&dec, &p)) {
            if (sums && shown < nsums) {
                unsigned y = fnv(p.y, p.width, p.height, p.stride_y);
                unsigned u = fnv(p.cb, p.width / 2, p.height / 2, p.stride_c);
                unsigned v = fnv(p.cr, p.width / 2, p.height / 2, p.stride_c);
                if ((y != sums[3 * shown] || u != sums[3 * shown + 1] || v != sums[3 * shown + 2]) && *wrong < 0)
                    *wrong = shown;
            }
            *w = p.width;
            *h = p.height;
            shown++;
        }
    }
    h264_flush(&dec);
    while (h264_frame(&dec, &p)) {
        if (sums && shown < nsums) {
            unsigned y = fnv(p.y, p.width, p.height, p.stride_y);
            unsigned u = fnv(p.cb, p.width / 2, p.height / 2, p.stride_c);
            unsigned v = fnv(p.cr, p.width / 2, p.height / 2, p.stride_c);
            if ((y != sums[3 * shown] || u != sums[3 * shown + 1] || v != sums[3 * shown + 2]) && *wrong < 0)
                *wrong = shown;
        }
        shown++;
    }
    return shown;
}

int main(void) {
    puts("h264\n");
    for (int k = 0; k < (int)(sizeof(H264_CASES) / sizeof(H264_CASES[0])); k++) {
        const h264_case *c = &H264_CASES[k];
        int wrong, w = 0, h = 0;
        int shown = run(c->data, c->len, c->sums, c->frames, &wrong, &w, &h);
        ok(c->name, 1);
        if (dec.errors) { puts("          "); puts(dec.why); putc('\n'); }
        okn("  every frame decodes and is shown", shown == c->frames && dec.errors == 0, shown);
        okn("  at the size the stream says it shows", w == c->width && h == c->height, w * 10000 + h);
        okn("  and each is Windows' decoding of it, in order (first differing frame)", wrong < 0, wrong);
        h264_close(&dec);
    }

    /* Damage. Every so many bytes after the parameter sets changed, at
       several spacings and starting points, in the streams that use each
       entropy coder; then each stream cut short at several places. */
    static u8 copy[80000];
    int runs = 0, refused = 0;
    for (int k = 0; k < (int)(sizeof(H264_CASES) / sizeof(H264_CASES[0])); k++) {
        const h264_case *c = &H264_CASES[k];
        if (c->len > (int)sizeof(copy)) continue;
        for (int step = 97; step < 2000; step = step * 2 + 1) {
            for (int i = 0; i < c->len; i++) copy[i] = c->data[i];
            for (int i = 64 + step / 3; i < c->len; i += step) copy[i] ^= (u8)(0x5A + i);
            int wrong, w, h;
            run(copy, c->len, 0, 0, &wrong, &w, &h);
            runs++;
            if (dec.errors) refused++;
            h264_close(&dec);
        }
        for (int cut = c->len / 7; cut < c->len; cut += c->len / 7) {
            int wrong, w, h;
            run(c->data, cut, 0, 0, &wrong, &w, &h);
            runs++;
            h264_close(&dec);
        }
    }
    okn("damaged and cut streams are decoded or refused, and nothing faults", runs > 20, runs);
    okn("and the damage is noticed, not decoded as if it were whole", refused > 0, refused);

    puts(failed ? "H264TEST_FAIL\n" : "H264TEST_PASS\n");
    return failed ? 1 : 0;
}
