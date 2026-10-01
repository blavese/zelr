/* What a page's <video> plays, below the script: fragmented MP4 appended as
 * Media Source Extensions append it (mp4.h), and the pipeline that decodes
 * and times it (media.h).
 *
 * The streams are tools/genmedia.py's: h264data.h's CABAC stream with B
 * pictures and aacdata.h's tone, put into fragmented MP4 there and read
 * back by Windows before they were written. So every picture that comes out
 * must be the one Windows' decoder made, in display order -- the same
 * checksums h264test holds -- and every sound sample the AAC frame it was.
 * Appends arrive in pieces that cut boxes in two, as a page's network
 * reads do; a seek must start again from the right picture; damage must be
 * refused without a fault.
 */
#include "zelr.h"
#include "alloc.h"
#include "aac.h"
#include "h264.h"
#include "ts.h"
#include "media.h"
#include "h264data.h"
#include "aacdata.h"
#include "mediadata.h"

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

/* Each picture the pipeline shows: which of the answers it is, by its
   checksums, or -1. */
static int seen[64], nseen;
static void on_picture(void *ctx, const h264_picture *p) {
    (void)ctx;
    const h264_case *c = &H264_CASES[MEDIA_VIDEO_CASE];
    unsigned y = fnv(p->y, p->width, p->height, p->stride_y);
    unsigned u = fnv(p->cb, p->width / 2, p->height / 2, p->stride_c);
    unsigned v = fnv(p->cr, p->width / 2, p->height / 2, p->stride_c);
    int which = -1;
    for (int f = 0; f < c->frames; f++)
        if (c->sums[3 * f] == y && c->sums[3 * f + 1] == u && c->sums[3 * f + 2] == v) { which = f; break; }
    if (nseen < 64) seen[nseen++] = which;
}

static media_t med;

static int append_in_pieces(int k, const u8 *p, int n, int piece) {
    for (int at = 0; at < n; at += piece) {
        int len = n - at < piece ? n - at : piece;
        if (media_append(&med, k, p + at, len) < 0) return -1;
    }
    return 0;
}

/* Plays from the position to `until`, a thirtieth of a second at a time
   on the test's clock, which only ever goes forward. */
static double test_now;
static void play_to(double from, double until) {
    for (double t = from; t <= until + 0.0001; t += 1.0 / 30) {
        med.test_clock = test_now;
        media_step(&med);
        test_now += 1.0 / 30;
    }
}

/* A file, answered as a server answers ranges: whatever the pipeline asks
   for, as much of it as the file has, and no more than `piece` at once (a
   server may send less than it was asked for). How many asks it took. */
static int asks, piece = 1 << 30;
static int feed_file(const u8 *f, long long n) {
    long long from;
    int len;
    for (int guard = 0; guard < 4000 && media_file_want(&med, &from, &len); guard++) {
        if (from >= n) return -1;
        if (len > piece) len = piece;
        long long l = from + len > n ? n - from : len;
        asks++;
        if (media_file_feed(&med, from, f + from, l, n) < 0) return -1;
    }
    return 0;
}

/* Plays a file to `until`, its bytes fed as it asks for them. */
static void play_file_to(const u8 *f, long long n, double from, double until) {
    for (double t = from; t <= until + 0.0001; t += 1.0 / 30) {
        feed_file(f, n);
        med.test_clock = test_now;
        media_step(&med);
        test_now += 1.0 / 30;
    }
}

/* The ADTS frames of aacdata.h's transport stream, as the answer for what
   the MP4 carries. */
static const u8 *adts_at[64];
static int adts_len[64], nadts;
static void on_pes(void *ctx, int type, long long pts, const u8 *p, int n) {
    (void)ctx; (void)pts;
    if (type != TS_AAC) return;
    int at = 0;
    while (at + 7 <= n && nadts < 64) {
        aac_adts h;
        if (!aac_adts_read(p + at, n - at, &h) || at + h.frame_len > n) break;
        static u8 store[64][2048];
        int len = h.frame_len - h.header_len;
        if (len > 2048) break;
        for (int i = 0; i < len; i++) store[nadts][i] = p[at + h.header_len + i];
        adts_at[nadts] = store[nadts];
        adts_len[nadts] = len;
        nadts++;
        at += h.frame_len;
    }
}

int main(void) {
    puts("media\n");
    const h264_case *c = &H264_CASES[MEDIA_VIDEO_CASE];

    /* --- pictures --------------------------------------------------------------------------- */
    media_open(&med);
    med.on_picture = on_picture;
    med.test_clock = 0;
    int a1 = append_in_pieces(0, MEDIA_VIDEO_INIT, (int)sizeof(MEDIA_VIDEO_INIT), 100);
    int a2 = append_in_pieces(0, MEDIA_VIDEO_MEDIA, (int)sizeof(MEDIA_VIDEO_MEDIA), 1777);
    ok("an initialisation segment and three fragments, appended in pieces that cut boxes in two", a1 == 0 && a2 == 0);
    media_source *s = &med.src[0];
    okn("it has one track, of pictures, at the size the stream has (width)",
        s->have_init && s->init.n == 1 && s->init.t[0].kind == MP4_VIDEO && s->init.t[0].width == c->width &&
        s->init.t[0].height == c->height, s->init.t[0].width);
    okn("every picture is read from the fragments", s->count == MEDIA_VIDEO_FRAMES, s->count);
    double r[8];
    int nr = media_buffered(&med, 0, r, 4);
    okn("and they make one buffered range, from 0 to a second (ms)", nr == 1 && r[0] > -0.001 && r[0] < 0.001 &&
        r[1] > 0.99 && r[1] < 1.05, nr == 1 ? (int)(r[1] * 1000) : -1);
    if (nr != 1) { puts("          ranges:"); for (int i = 0; i < nr; i++) { putc(' '); putn((int)(r[2 * i] * 1000)); putc('-'); putn((int)(r[2 * i + 1] * 1000)); } putc('\n'); }
    media_end_of_stream(&med);
    okn("the end of the stream makes the duration where it ends (ms)", med.duration > 0.99 && med.duration < 1.05,
        (int)(med.duration * 1000));
    media_play(&med);
    play_to(0, 1.2);
    int inorder = nseen == c->frames;
    for (int i = 0; i < nseen && inorder; i++) if (seen[i] != i) inorder = 0;
    okn("played, every picture is Windows' decoding of it, in display order (pictures shown)", inorder, nseen);
    if (!inorder) { puts("          seen:"); for (int i = 0; i < nseen; i++) { putc(' '); putn(seen[i]); } putc('\n'); }
    okn("and none was passed over", med.frames_dropped == 0, med.frames_dropped);

    /* A seek half way: back to the IDR picture at the half (the stream's
       second GOP) and on from there. */
    nseen = 0;
    media_seek(&med, 0.5);
    media_play(&med);                     /* it had reached the end and stopped */
    play_to(0.5, 1.2);
    okn("after a seek to half way, the first picture shown is the one there (its number)", nseen > 0 && seen[0] == 15,
        nseen ? seen[0] : -1);
    int rest = nseen == 15;
    for (int i = 0; i < nseen && rest; i++) if (seen[i] != 15 + i) rest = 0;
    okn("and the rest follow in order", rest, nseen);
    media_remove(&med, 0, 0, 0.5);
    nr = media_buffered(&med, 0, r, 4);
    okn("taking out the first half leaves what is buffered starting there (ms)", nr == 1 && r[0] > 0.49 && r[0] < 0.51,
        nr ? (int)(r[0] * 1000) : -1);
    media_close(&med);

    /* Pictures shown in another order than they are decoded, further apart
       than the gap that splits a range (ten a second, each P picture three
       ahead of the B pictures before it, as the colours are): still one
       range, from the first to the last. */
    media_open(&med);
    {
        mp4_track ft;
        for (int i = 0; i < (int)sizeof(ft); i++) ((volatile u8 *)&ft)[i] = 0;
        ft.kind = MP4_VIDEO;
        ft.timescale = 10;
        media_ctx cx = { &med, &med.src[2] };
        static const int shown[10] = { 0, 3, 1, 2, 6, 4, 5, 9, 7, 8 };
        static const u8 one[1] = { 0 };
        for (int i = 0; i < 10; i++) media_take(&cx, &ft, i, shown[i], 0, i == 0, one, 1);
        nr = media_buffered(&med, 2, r, 4);
        okn("pictures decoded out of the order they are shown still make one range (ranges)", nr == 1 && r[0] < 0.001 &&
            r[1] > 0.95, nr);
    }
    media_close(&med);

    /* --- sound ------------------------------------------------------------------------------ */
    media_open(&med);
    med.test_clock = 0;
    a1 = append_in_pieces(1, MEDIA_AUDIO_INIT, (int)sizeof(MEDIA_AUDIO_INIT), 37);
    a2 = append_in_pieces(1, MEDIA_AUDIO_MEDIA, (int)sizeof(MEDIA_AUDIO_MEDIA), 4093);
    s = &med.src[1];
    ok("a sound's initialisation segment and two fragments, appended in pieces", a1 == 0 && a2 == 0);
    static const int SFI_RATE[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    int rate = s->init.n && s->init.t[0].sfi < 13 ? SFI_RATE[s->init.t[0].sfi] : -1;
    okn("its track is AAC at the rate and channels it says (rate)",
        s->have_init && s->init.n == 1 && s->init.t[0].kind == MP4_AUDIO && rate == MEDIA_AUDIO_RATE &&
        s->init.t[0].channels == 2, rate);
    ts_demux td;
    ts_init(&td, on_pes, 0);
    ts_feed(&td, AACD_TS, (int)sizeof(AACD_TS));
    ts_flush(&td);
    ts_free(&td);
    int same = s->count == MEDIA_AUDIO_FRAMES && nadts == MEDIA_AUDIO_FRAMES, k = 0, timed = 1;
    for (media_sample *x = s->head; x && same; x = x->next, k++) {
        if (x->len != adts_len[k]) { same = 0; break; }
        for (int i = 0; i < x->len; i++) if (x->data[i] != adts_at[k][i]) { same = 0; break; }
        double want = k * 1024.0 / MEDIA_AUDIO_RATE;
        if (x->pts < want - 0.0001 || x->pts > want + 0.0001) timed = 0;
    }
    okn("every frame is the AAC frame the transport stream carries, byte for byte (frames)", same, s->count);
    ok("each at its time, 1024 samples after the last", timed);
    media_close(&med);

    /* --- files --------------------------------------------------------------------------------
     *
     * As a page names one for a <video>: Windows' own MP4 of the stream, its
     * moov after its media, so found by asking where the boxes say it is;
     * then the stream and the tone in one file, with an edit list. */
    media_open(&med);
    med.on_picture = on_picture;
    med.test_clock = test_now;
    media_file_open(&med);
    nseen = 0;
    asks = 0;
    piece = 4096;                 /* less than the media before the moov */
    int fed = feed_file(MEDIA_FILE_WINDOWS, (long long)sizeof(MEDIA_FILE_WINDOWS));
    okn("Windows' own file is read, answered in pieces smaller than its media: its moov found after it (asks)",
        fed == 0 && med.file_ready && asks >= 2, asks);
    okn("its duration is what its moov says (ms)", med.duration > 0.99 && med.duration < 1.01, (int)(med.duration * 1000));
    media_play(&med);
    play_file_to(MEDIA_FILE_WINDOWS, (long long)sizeof(MEDIA_FILE_WINDOWS), 0, 1.2);
    inorder = nseen == c->frames;
    for (int i = 0; i < nseen && inorder; i++) if (seen[i] != i) inorder = 0;
    okn("and every picture is Windows' decoding of it, in display order", inorder, nseen);
    if (!inorder) { puts("          seen:"); for (int i = 0; i < nseen; i++) { putc(' '); putn(seen[i]); } putc('\n'); }
    nseen = 0;
    media_seek(&med, 0.5);
    media_play(&med);
    play_file_to(MEDIA_FILE_WINDOWS, (long long)sizeof(MEDIA_FILE_WINDOWS), 0.5, 1.2);
    /* Windows writes no edit list and starts its offsets a picture in, so in
       its own file picture k is at (k + 1) / 30 and half a second is 14. */
    okn("a seek in a file starts from the picture there, in the file's own times (its number)",
        nseen > 0 && seen[0] == 14 && nseen == 16, nseen ? seen[0] : -1);
    media_close(&med);
    piece = 1 << 30;

    media_open(&med);
    med.on_picture = on_picture;
    med.test_clock = test_now;
    media_file_open(&med);
    nseen = 0;
    fed = feed_file(MEDIA_FILE_MUXED, (long long)sizeof(MEDIA_FILE_MUXED));
    s = &med.src[0];
    ok("a file with the pictures and the sound interleaved is read, both tracks in it",
       fed == 0 && s->init.n == 2 && s->kind == (MP4_VIDEO | MP4_AUDIO));
    int vcount = 0, acount = 0, aright = 1;
    for (media_sample *x = s->head; x; x = x->next) {
        if (x->kind == MP4_VIDEO) { vcount++; continue; }
        if (acount < nadts) {
            if (x->len != adts_len[acount]) aright = 0;
            for (int i = 0; aright && i < x->len; i++) if (x->data[i] != adts_at[acount][i]) aright = 0;
            double want = acount * 1024.0 / MEDIA_AUDIO_RATE;
            if (x->pts < want - 0.0001 || x->pts > want + 0.0001) aright = 0;
        }
        acount++;
    }
    okn("every picture and every sound frame comes out of it (frames of sound)",
        vcount == MEDIA_VIDEO_FRAMES && acount == MEDIA_FILE_AUDIO_FRAMES, acount);
    ok("the sound frames byte for byte and at their times", aright && acount <= nadts);
    nr = media_buffered(&med, 0, r, 4);
    okn("what it holds is where its pictures and its sound overlap (ms)", nr == 1 && r[0] < 0.001 && r[1] > 0.99 &&
        r[1] < 1.01, nr ? (int)(r[1] * 1000) : -1);
    media_play(&med);
    play_file_to(MEDIA_FILE_MUXED, (long long)sizeof(MEDIA_FILE_MUXED), 0, 1.2);
    inorder = nseen == c->frames;
    for (int i = 0; i < nseen && inorder; i++) if (seen[i] != i) inorder = 0;
    okn("and its pictures, the sound left to the sound, are Windows' decoding, from the edit list's start", inorder, nseen);
    if (!inorder) { puts("          seen:"); for (int i = 0; i < nseen; i++) { putc(' '); putn(seen[i]); } putc('\n'); }
    media_close(&med);

    /* --- damage ----------------------------------------------------------------------------- */
    static u8 copy[sizeof(MEDIA_VIDEO_MEDIA)];
    int refused = 0, runs = 0;
    for (int step = 13; step < 4000; step = step * 2 + 1) {
        for (int i = 0; i < (int)sizeof(copy); i++) copy[i] = MEDIA_VIDEO_MEDIA[i];
        for (int i = step / 2; i < (int)sizeof(copy); i += step) copy[i] ^= (u8)(0x5A + i);
        media_open(&med);
        med.test_clock = 0;
        media_append(&med, 0, MEDIA_VIDEO_INIT, (int)sizeof(MEDIA_VIDEO_INIT));
        if (media_append(&med, 0, copy, (int)sizeof(copy)) < 0) refused++;
        media_play(&med);
        play_to(0, 1.2);
        media_close(&med);
        runs++;
    }
    static u8 lie[64];
    for (int i = 0; i < 64; i++) lie[i] = 0;
    lie[3] = 40; lie[4] = 'm'; lie[5] = 'o'; lie[6] = 'o'; lie[7] = 'f';     /* a moof that says it holds a traf too big */
    lie[11] = 32; lie[12] = 't'; lie[13] = 'r'; lie[14] = 'a'; lie[15] = 'f'; lie[16] = 0x7F;
    media_open(&med);
    media_append(&med, 0, MEDIA_VIDEO_INIT, (int)sizeof(MEDIA_VIDEO_INIT));
    media_append(&med, 0, lie, 64);
    media_close(&med);
    okn("damaged fragments are decoded or refused, and nothing faults", runs > 5, runs);
    static u8 fcopy[sizeof(MEDIA_FILE_MUXED)];
    int fruns = 0, frefused = 0;
    for (int step = 7; step < 6000; step = step * 2 + 1) {
        for (int i = 0; i < (int)sizeof(fcopy); i++) fcopy[i] = MEDIA_FILE_MUXED[i];
        /* The moov, where the tables are, is at the end. */
        for (int i = (int)sizeof(fcopy) - 1 - step / 3; i > (int)sizeof(fcopy) - 1400; i -= step) fcopy[i] ^= (u8)(0x6B + i);
        media_open(&med);
        med.test_clock = test_now;
        media_file_open(&med);
        if (feed_file(fcopy, (long long)sizeof(fcopy)) < 0) frefused++;
        media_play(&med);
        play_file_to(fcopy, (long long)sizeof(fcopy), 0, 1.2);
        media_close(&med);
        fruns++;
    }
    okn("damaged files are played or refused, and nothing faults (refused)", fruns > 5, frefused);
    okn("and a fragment whose boxes do not add up is noticed", refused > 0, refused);

    puts(failed ? "MEDIATEST_FAIL\n" : "MEDIATEST_PASS\n");
    return failed ? 1 : 0;
}
