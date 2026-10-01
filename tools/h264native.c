/* zelr's H.264 decoder (userland/h264.h) built for the host, for
 * tools/h264check.py: an Annex B file in, raw I420 frames out in display
 * order, and what the stream used. A host tool; nothing here runs on zelr.
 *
 *   h264native IN.264 OUT.yuv
 */
#include <stdio.h>
#include <stdlib.h>
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;
typedef int i32;
#define H264_NO_ZELR
#define h264_alloc(n) calloc(1, (size_t)(n))
#define h264_free(p) free(p)
#include "h264.h"

static h264_dec dec;

static int take(FILE *o) {
    h264_picture p;
    int n = 0;
    while (h264_frame(&dec, &p)) {
        for (int y = 0; y < p.height; y++) fwrite(p.y + y * p.stride_y, 1, p.width, o);
        for (int y = 0; y < p.height / 2; y++) fwrite(p.cb + y * p.stride_c, 1, p.width / 2, o);
        for (int y = 0; y < p.height / 2; y++) fwrite(p.cr + y * p.stride_c, 1, p.width / 2, o);
        n++;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "h264native IN.264 OUT.yuv\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8 *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    FILE *o = fopen(argv[2], "wb");
    if (!o) return 1;
    h264_open(&dec);
    int at = 0, s, len, shown = 0;
    while (h264_next_nal(buf, (int)n, &at, &s, &len)) {
        h264_nal(&dec, buf + s, len);
        shown += take(o);
    }
    h264_flush(&dec);
    shown += take(o);
    fclose(o);
    printf("uses: ");
    int first = 1;
    for (int i = 0; i < 16; i++)
        if (dec.seen & (1u << i)) { printf("%s%s", first ? "" : ", ", H264_SEEN_NAMES[i]); first = 0; }
    printf("\n%d decoded, %d shown, %d errors%s%s\n", dec.frames_decoded, shown, dec.errors,
           dec.errors ? ": " : "", dec.why);
    h264_close(&dec);
    return dec.errors ? 1 : 0;
}
