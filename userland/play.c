/* A player for live streams: a Twitch channel, or any HLS stream.
 *
 *   play twitch:<channel>         what the channel is broadcasting now
 *   play https://.../index.m3u8   any HLS stream of H.264 and AAC in MPEG-TS
 *
 * The browser starts it from a live channel's page. A program of its own,
 * so the waiting a stream does -- for the next segment, for room in the
 * sound buffer -- never holds up a page.
 *
 * For Twitch the stream is found the way Twitch's own page finds it: an
 * access token from its API (sites.h, twitch_ask, which also learns the
 * Client-Id again if Twitch changes it), then the channel's master playlist
 * from usher with that token, then a rendition's media playlist, read again
 * every couple of seconds for the segments it adds. Each segment is an MPEG
 * transport stream (ts.h). Its AAC frames (aac.h) are decoded at once into a
 * queue of samples, handed to the sound card as fast as it has room, at the
 * card's rate. Its H.264 access units (h264.h) are kept and decoded only as
 * the picture needs them, and each frame is shown when the sound reaches
 * its time: the clock is the sample being heard, worked out from what is
 * queued here and in the card. A frame already late is passed over.
 *
 * The rendition starts at the smallest with a picture and moves up while
 * decoding takes well under the time it covers, down when it does not keep
 * up; Twitch's renditions are cut at the same moments, each piece starting
 * with a picture that needs no other, so a change is clean.
 *
 * Starting a little behind the live edge (the last two segments) gives the
 * queue something to play while the next segment is fetched; a stream that
 * falls further behind than the playlist reaches starts again at its end. A
 * finished stream (a playlist that says it has ended) plays from its start.
 *
 * Started where there is no desktop (the kernel shell on a machine without
 * a screen) it plays without a window, says how it is going on the console,
 * and leaves when the stream ends.
 */
#include "zelr.h"
#include "alloc.h"
#include "ui.h"
#include "web.h"
#include "fetch.h"
#include "sites.h"
#include "hls.h"
#include "ts.h"
#include "aac.h"
#include "h264.h"

#define QUEUE_FRAMES (48000 * 20)        /* twenty seconds at 48 kHz */
#define SEGMENT_MAX  (8 << 20)
#define BAR 40                           /* the status line under the picture */
#define AHEAD_MS 6000                    /* how much to have fetched ahead */

static char status[160] = "starting";
static char title[96] = "";
static int dirty = 1;

static void say(const char *s) {
    if (!w_same(status, s)) { w_copy(status, sizeof(status), s, sizeof(status)); dirty = 1; }
    puts("play: ");
    puts(s);
    putc('\n');
}

/* --- the sound out ---------------------------------------------------------------------- */

static short *queue;                     /* interleaved stereo at the stream's rate */
static int qhead, qlen;                  /* frames */
static int stream_rate = 48000;
static int dev_rate, dev_chans, have_sound;
static u32 step, frac;                   /* stream frames an output frame moves, 16.16 */
static long long audio_end = -1;         /* the time (90 kHz) at the end of what is queued */

static void queue_put(const short *pcm, int frames, int chans) {
    for (int i = 0; i < frames; i++) {
        if (qlen >= QUEUE_FRAMES) return;          /* full: the newest is lost, not the oldest */
        int at = (qhead + qlen) % QUEUE_FRAMES;
        short l = pcm[i * chans], r = chans > 1 ? pcm[i * chans + 1] : l;
        queue[at * 2] = l;
        queue[at * 2 + 1] = r;
        qlen++;
    }
}

/* What the card has room for, from the queue, at the card's rate. */
static void feed(void) {
    if (!have_sound) return;
    zelr_sound snd;
    if (sound_info(&snd) != 0) return;
    int room = (int)snd.room;
    static short out[4096 * 2];
    while (room > 64 && qlen > 2) {
        int made = 0;
        int want = room < 4096 ? room : 4096;
        while (made < want && qlen > 2) {
            int i0 = qhead, i1 = (qhead + 1) % QUEUE_FRAMES;
            int f = (int)(frac & 0xFFFF);
            for (int c = 0; c < dev_chans; c++) {
                int cc = c < 2 ? c : 1;
                int a = queue[i0 * 2 + cc], b = queue[i1 * 2 + cc];
                out[made * dev_chans + c] = (short)(a + (((b - a) * f) >> 16));
            }
            made++;
            frac += step;
            while (frac >= 0x10000) { frac -= 0x10000; qhead = (qhead + 1) % QUEUE_FRAMES; qlen--; }
        }
        if (!made) break;
        sound_write(out, made);
        room -= made;
    }
}

/* Without a sound card the queue is let go at the rate it would be heard,
   so the picture still has a clock to keep to. */
static int drain_tick = -1;
static void drain(void) {
    if (have_sound) return;
    int now = ticks();
    if (drain_tick < 0) drain_tick = now;
    int frames = (int)((long long)(now - drain_tick) * stream_rate / 100);
    if (frames <= 0) return;
    drain_tick += (int)((long long)frames * 100 / stream_rate);
    if (frames > qlen) frames = qlen;
    qhead = (qhead + frames) % QUEUE_FRAMES;
    qlen -= frames;
}

/* The time of the sample being heard now, or -1 before there is sound. */
static long long heard_now(void) {
    if (audio_end < 0 || !stream_rate) return -1;
    long long behind = (long long)qlen * 90000 / stream_rate;
    if (have_sound) {
        zelr_sound snd;
        if (sound_info(&snd) == 0 && dev_rate) behind += (long long)snd.queued * 90000 / dev_rate;
    }
    return audio_end - behind;
}

/* --- the picture ------------------------------------------------------------------------- */

typedef struct au { struct au *next; long long pts; int n; u8 data[]; } au;
static au *au_head, *au_tail;
static int au_count;
static h264_dec vdec;
static int have_video, video_w, video_h;
static int shown, dropped, decode_ticks;
static long long decoded_span;           /* the time the decoded units cover, 90 kHz */
static long long last_au_pts = -1;
static int video_failed;

static void au_put(long long pts, const u8 *p, int n) {
    if (au_count > 2000) return;                   /* a stream far ahead of what is shown */
    au *a = (au *)malloc(sizeof(au) + (u64)n);
    if (!a) return;
    a->next = 0;
    a->pts = pts;
    a->n = n;
    for (int i = 0; i < n; i++) a->data[i] = p[i];
    if (au_tail) au_tail->next = a; else au_head = a;
    au_tail = a;
    au_count++;
}

static void au_clear(void) {
    while (au_head) { au *a = au_head; au_head = a->next; free(a); }
    au_tail = 0;
    au_count = 0;
}

/* Whether the YUV is BT.709 (HD) or BT.601, as the stream says or its size
   suggests, and limited or full range: the multipliers, times 256. */
static void colour_terms(const h264_picture *p, int *cy, int *yo, int *rv, int *gu, int *gv, int *bu) {
    int hd = p->matrix == 1 || (p->matrix != 5 && p->matrix != 6 && p->height > 576);
    int full = p->full_range;
    *cy = full ? 256 : 298;
    *yo = full ? 0 : 16;
    if (hd) { *rv = full ? 403 : 459; *gu = full ? 48 : 55; *gv = full ? 120 : 136; *bu = full ? 475 : 541; }
    else    { *rv = full ? 359 : 409; *gu = full ? 88 : 100; *gv = full ? 183 : 208; *bu = full ? 454 : 516; }
}

static inline u32 clip8(int v) { return (u32)(v < 0 ? 0 : v > 255 ? 255 : v); }

static int boxed_w = -1, boxed_h = -1, boxed_ww = -1, boxed_wh = -1;

/* A frame into the window's picture area, scaled to fit and centred, the
   rest black. */
static void blit(int win, const h264_picture *p) {
    int w = win_width(win), h = win_height(win) - BAR;
    u32 *px = win_surface(win);
    if (!px || w <= 0 || h <= 0 || p->width <= 0 || p->height <= 0) return;
    int dw = w, dh = (int)((long long)p->height * w / p->width);
    if (dh > h) { dh = h; dw = (int)((long long)p->width * h / p->height); }
    if (dw < 1 || dh < 1) return;
    int ox = (w - dw) / 2, oy = (h - dh) / 2;
    if (boxed_w != dw || boxed_h != dh || boxed_ww != w || boxed_wh != h) {
        for (int i = 0; i < w * h; i++) px[i] = 0;
        boxed_w = dw; boxed_h = dh; boxed_ww = w; boxed_wh = h;
    }
    int cy, yo, rv, gu, gv, bu;
    colour_terms(p, &cy, &yo, &rv, &gu, &gv, &bu);
    static int xmap[4096];
    if (dw > 4096) dw = 4096;
    for (int x = 0; x < dw; x++) xmap[x] = (int)((long long)x * p->width / dw);
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long long)y * p->height / dh);
        const u8 *ly = p->y + sy * p->stride_y;
        const u8 *lu = p->cb + (sy >> 1) * p->stride_c, *lv = p->cr + (sy >> 1) * p->stride_c;
        u32 *out = px + (oy + y) * w + ox;
        for (int x = 0; x < dw; x++) {
            int sx = xmap[x];
            int c = (ly[sx] - yo) * cy, d = lu[sx >> 1] - 128, e = lv[sx >> 1] - 128;
            out[x] = clip8((c + rv * e + 128) >> 8) << 16 | clip8((c - gu * d - gv * e + 128) >> 8) << 8 |
                     clip8((c + bu * d + 128) >> 8);
        }
    }
}

/* Shows the frame that is due, if the sound has reached it; decodes what
   is needed to have one due. Returns whether the window changed. */
static int video_step(int win) {
    if (!have_video || video_failed) return 0;
    long long now = heard_now();
    for (int guard = 0; guard < 16; guard++) {
        long long pts;
        if (!h264_peek(&vdec, &pts)) {
            if (!au_head || h264_room(&vdec) <= 0) return 0;
            au *a = au_head;
            au_head = a->next;
            if (!au_head) au_tail = 0;
            au_count--;
            int t0 = ticks();
            if (h264_decode_au(&vdec, a->data, a->n, a->pts) < 0 && vdec.errors > 50 && !vdec.frames_decoded) {
                video_failed = 1;
                say(vdec.why);
            }
            decode_ticks += ticks() - t0;
            if (last_au_pts >= 0 && a->pts > last_au_pts && a->pts - last_au_pts < 90000) decoded_span += a->pts - last_au_pts;
            last_au_pts = a->pts;
            free(a);
            continue;
        }
        if (now < 0 || pts > now + 900) return 0;      /* not its time yet */
        h264_picture p;
        h264_frame(&vdec, &p);
        if (pts < now - 9000 && h264_peek(&vdec, &pts) && pts <= now) {   /* late, and another is due: pass it */
            dropped++;
            continue;
        }
        if (win >= 0) blit(win, &p);
        if (p.width != video_w || p.height != video_h) { video_w = p.width; video_h = p.height; dirty = 1; }
        shown++;
        return 1;
    }
    return 0;
}

/* --- the stream in --------------------------------------------------------------------- */

static aac_dec dec;
static int dec_open;
static int frames_decoded, frames_bad;

static void on_pes(void *ctx, int type, long long pts, const u8 *p, int n) {
    (void)ctx;
    if (type == TS_H264) {
        have_video = 1;
        au_put(pts, p, n);
        return;
    }
    if (type != TS_AAC) return;
    static short pcm[1024 * AAC_MAX_CH];
    int at = 0, made = 0;
    while (at + 7 <= n) {
        aac_adts h;
        if (!aac_adts_read(p + at, n - at, &h) || at + h.frame_len > n) break;
        if (!dec_open) {
            dec_open = aac_open(&dec, h.sfi, h.channels);
            if (!dec_open) { say("the stream's sound is not one this can play"); return; }
            stream_rate = AAC_RATES[dec.rate_idx].rate;
            step = (u32)(((u64)stream_rate << 16) / (u32)(dev_rate ? dev_rate : stream_rate));
        }
        if (aac_decode(&dec, p + at + h.header_len, h.frame_len - h.header_len, pcm) == 1024) {
            queue_put(pcm, 1024, dec.channels);
            frames_decoded++;
            made++;
        } else {
            frames_bad++;
        }
        at += h.frame_len;
    }
    /* The time at the end of the queue: from the packet's own when it has
       one, so the clock keeps to the stream's even across a lost piece. */
    if (made && stream_rate) {
        long long dur = (long long)made * 1024 * 90000 / stream_rate;
        audio_end = pts >= 0 ? pts + dur : (audio_end >= 0 ? audio_end + dur : -1);
    }
}

static char *segbuf, *textbuf;

/* A segment fetched and taken apart; 0 when it could not be had. */
static int take_segment(const char *url) {
    int st = 0;
    int n = hls_get(url, segbuf, SEGMENT_MAX, &st);
    if (n <= 0 || st != 200) return 0;
    ts_demux t;
    ts_init(&t, on_pes, 0);
    ts_feed(&t, (const u8 *)segbuf, n);
    ts_flush(&t);
    ts_free(&t);
    return 1;
}

/* --- finding the stream ---------------------------------------------------------------- */

/* The master playlist's address for a channel, from an access token the way
   Twitch's page asks for one; 0 with a reason said. */
static int twitch_master(const char *login, char *out, int cap) {
    for (const char *p = login; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            say("that is not a channel's name");
            return 0;
        }
    static char query[512], buf[65536];
    int n = 0;
    n = wh_add(query, sizeof(query), n, "query{streamPlaybackAccessToken(channelName:\"");
    if (n >= 0) n = wh_add(query, sizeof(query), n, login);
    if (n >= 0) n = wh_add(query, sizeof(query), n, "\", params:{platform:\"web\",playerBackend:\"mediaplayer\",playerType:\"site\"}){value signature}}");
    if (n < 0) return 0;
    response_t r;
    int rc = twitch_ask(query, buf, sizeof(buf), &r);
    if (rc != 200) { say("Twitch did not give an access token"); return 0; }
    int tok = sj_find(r.body, 0, r.len, "streamPlaybackAccessToken");
    if (tok < 0 || r.body[tok] != '{') { say("Twitch gave no access token for that channel"); return 0; }
    static char value[4096], sig[128], ev[12288];
    int end = sj_skip(r.body, tok, r.len);
    int vat = sj_find(r.body, tok, end, "value"), sat = sj_find(r.body, tok, end, "signature");
    if (vat < 0 || sat < 0) { say("Twitch's access token was not as expected"); return 0; }
    sj_str(r.body, vat, end, value, sizeof(value));
    sj_str(r.body, sat, end, sig, sizeof(sig));
    url_escape(value, ev, sizeof(ev));
    n = 0;
    n = wh_add(out, cap, n, "https://usher.ttvnw.net/api/channel/hls/");
    if (n >= 0) n = wh_add(out, cap, n, login);
    if (n >= 0) n = wh_add(out, cap, n, ".m3u8?allow_source=true&allow_audio_only=true&fast_bread=true&player=twitchweb&p=");
    if (n >= 0) n = wh_add_num(out, cap, n, (int)(ticks() % 1000000));
    if (n >= 0) n = wh_add(out, cap, n, "&sig=");
    if (n >= 0) n = wh_add(out, cap, n, sig);
    if (n >= 0) n = wh_add(out, cap, n, "&token=");
    if (n >= 0) n = wh_add(out, cap, n, ev);
    return n >= 0;
}

/* The renditions with a picture, smallest first (by height, then bandwidth);
   Twitch's sound-only one apart. */
static hls_variant variants[16];
static int nvariants, order[16], norder, cur_rung = -1, audio_variant = -1;

static void rank_variants(void) {
    norder = 0;
    for (int i = 0; i < nvariants; i++) {
        if (w_same(variants[i].name, "audio_only") || (variants[i].codecs[0] && !hls_line_has(variants[i].codecs, "avc1"))) {
            if (w_same(variants[i].name, "audio_only")) audio_variant = i;
            continue;
        }
        order[norder++] = i;
    }
    for (int a = 1; a < norder; a++) {
        int x = order[a], b = a - 1;
        while (b >= 0 && (variants[order[b]].height > variants[x].height ||
                          (variants[order[b]].height == variants[x].height && variants[order[b]].bandwidth > variants[x].bandwidth))) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = x;
    }
}

/* --- the window ------------------------------------------------------------------------ */

/* Nothing more to do but show why: until the window is closed, or at once
   when there is none to show it in. */
static void linger(int win) {
    if (win < 0) exit(1);
    for (;;) {
        win_event ev;
        if (win_poll(win, &ev) && ev.type == WIN_EV_CLOSE) exit(0);
        ui_wait(win);
    }
}

/* The status line, or with no picture yet the whole window as words. */
static void draw_bar(int win) {
    int w = win_width(win), h = win_height(win);
    u32 *px = win_surface(win);
    if (!px || w <= 0 || h <= 0) return;
    surface s = { px, w, h };
    ui_theme t = ui_load_theme();
    char line[160];
    int n = 0, secs10 = stream_rate ? qlen * 10 / stream_rate : 0;
    if (!shown) {
        fill(&s, t.bg);
        boxed_w = -1;
        face_draw(&s, UI_PAD * 2, UI_PAD * 2, title, t.fg, UI_FACE_HEAD);
        face_draw(&s, UI_PAD * 2, UI_PAD * 2 + 34, status, t.dim, UI_FACE_BODY);
        n = wh_add(line, sizeof(line), n, have_video ? "the picture is on its way; " : "sound only; ");
    } else {
        for (int y = h - BAR; y < h; y++) for (int x = 0; x < w; x++) px[y * w + x] = t.bg;
        n = wh_add(line, sizeof(line), n, title);
        n = wh_add(line, sizeof(line), n, " -- ");
        n = wh_add_num(line, sizeof(line), n, video_w);
        n = wh_add(line, sizeof(line), n, "x");
        n = wh_add_num(line, sizeof(line), n, video_h);
        n = wh_add(line, sizeof(line), n, "; ");
    }
    n = wh_add_num(line, sizeof(line), n, secs10 / 10);
    n = wh_add(line, sizeof(line), n, ".");
    n = wh_add_num(line, sizeof(line), n, secs10 % 10);
    n = wh_add(line, sizeof(line), n, " s ahead");
    if (dropped) {
        n = wh_add(line, sizeof(line), n, "; ");
        n = wh_add_num(line, sizeof(line), n, dropped);
        n = wh_add(line, sizeof(line), n, " frames passed over");
    }
    if (shown) face_draw(&s, UI_PAD * 2, h - BAR + (BAR - 16) / 2, line, t.dim, UI_FACE_BODY);
    else face_draw(&s, UI_PAD * 2, UI_PAD * 2 + 58, line, t.dim, UI_FACE_BODY);
}

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "";
    int win = win_create("Player", 640, 360 + BAR);
    if (win >= 0) win_allow_resize(win);
    queue = (short *)malloc((u64)QUEUE_FRAMES * 4);
    segbuf = (char *)malloc(SEGMENT_MAX);
    textbuf = (char *)malloc(1 << 20);
    if (!queue || !segbuf || !textbuf) { say("not enough memory for a stream"); exit(1); }
    h264_open(&vdec);

    zelr_sound snd;
    if (sound_info(&snd) == 0 && snd.present) {
        have_sound = 1;
        dev_rate = (int)snd.rate;
        dev_chans = (int)snd.channels > 2 ? 2 : (int)snd.channels;
        if (dev_chans < 1) dev_chans = 2;
    } else {
        say("this machine has no sound hardware; the stream is shown but not heard");
    }
    step = 0x10000;

    static char master[HLS_URI];
    if (w_starts_fold(what, "twitch:")) {
        int k = 0;
        for (const char *p = "Twitch: "; *p; p++) title[k++] = *p;
        for (const char *p = what + 7; *p && k < (int)sizeof(title) - 1; p++) title[k++] = *p;
        title[k] = 0;
        if (win >= 0) { draw_bar(win); win_commit(win); }
        say("asking Twitch for the stream");
        if (!twitch_master(what + 7, master, sizeof(master))) {
            if (win >= 0) { draw_bar(win); win_commit(win); }
            linger(win);
        }
    } else if (w_starts_fold(what, "http://") || w_starts_fold(what, "https://")) {
        w_copy(master, sizeof(master), what, sizeof(master));
        w_copy(title, sizeof(title), what, sizeof(title));
    } else {
        w_copy(title, sizeof(title), "Player", sizeof(title));
        say("give it twitch:<channel> or the address of a playlist");
        if (win >= 0) { draw_bar(win); win_commit(win); }
        linger(win);
    }

    static hls_segment segs[64];
    int ns = 0, target = 0, fails = 0, retry_at = 0;
    long long next_seq = -1;
    int last_list = -1000, ended = 0;
    int last_draw = 0, last_judged = 0, judged_span_ticks = 0;

    /* The master playlist, or a media one given directly, which is then
       already read: a finished one is never asked for twice. */
    static char media[HLS_URI];
    int st = 0;
    int n = hls_get(master, textbuf, (1 << 20) - 1, &st);
    if (n > 0 && st == 200) {
        textbuf[n] = 0;
        nvariants = hls_master(master, textbuf, n, variants, 16);
        if (nvariants > 0) {
            rank_variants();
            int k = norder ? order[0] : audio_variant >= 0 ? audio_variant : 0;
            cur_rung = norder ? 0 : -1;
            w_copy(media, sizeof(media), variants[k].uri, sizeof(media));
        } else {
            w_copy(media, sizeof(media), master, sizeof(media));
            ns = hls_media(media, textbuf, n, segs, 64, &target, &ended);
            last_list = ticks();
        }
        say("playing");
    } else {
        say(st == 404 ? "the channel is not live" : "the stream could not be reached");
    }
    int playing = media[0] != 0;
    for (;;) {
        win_event ev;
        while (win >= 0 && win_poll(win, &ev)) {
            if (ev.type == WIN_EV_CLOSE) { hls_drop_all(); exit(0); }
            if (ev.type == WIN_EV_RESIZE) { boxed_w = -1; dirty = 1; }
        }

        if (playing) {
            feed();
            drain();
            if (video_step(win) && win >= 0) {
                if (dirty) { draw_bar(win); dirty = 0; last_draw = ticks(); }
                win_commit(win);
            }
            int queued_ms = stream_rate ? (int)((long long)qlen * 1000 / stream_rate) : 0;

            /* Every few seconds of picture, whether to change rendition: up
               while decoding takes under a third of the time it covers, down
               when it takes more than four fifths. */
            if (cur_rung >= 0 && decoded_span >= 4 * 90000) {
                int cost = (int)((long long)decode_ticks * 900 * 100 / decoded_span);   /* per cent */
                int to = cur_rung;
                if (cost < 33 && cur_rung + 1 < norder && variants[order[cur_rung + 1]].height <= 1080) to++;
                else if (cost > 80 && cur_rung > 0) to--;
                if (to != cur_rung) {
                    cur_rung = to;
                    w_copy(media, sizeof(media), variants[order[to]].uri, sizeof(media));
                    ns = 0;
                    last_list = -1000;
                }
                decode_ticks = 0;
                decoded_span = 0;
                (void)last_judged; (void)judged_span_ticks;
            }

            /* Segments the playlist listed that are not taken yet. Until they
               are, it is not read again: a finished one never is, so a queue
               that filled before the last of them must still come back for
               it. */
            int left = ns > 0 && next_seq >= 0 && next_seq <= segs[ns - 1].seq;
            if (!ended && !left && queued_ms < AHEAD_MS && ticks() - last_list >= 100) {
                last_list = ticks();
                int ln = hls_get(media, textbuf, (1 << 20) - 1, &st);
                if (ln > 0 && st == 200) {
                    textbuf[ln] = 0;
                    ns = hls_media(media, textbuf, ln, segs, 64, &target, &ended);
                } else {
                    ns = 0;
                    say("the stream stopped answering");
                }
            }
            if (ns > 0) {
                long long first = segs[0].seq, last = segs[ns - 1].seq;
                if (next_seq < 0 || next_seq < first || next_seq > last + 1)
                    next_seq = ended ? first : last - 1 >= first ? last - 1 : first;
            }
            left = ns > 0 && next_seq <= segs[ns - 1].seq;
            /* One segment a pass, so the card is fed and the window answers
               between them. A segment that cannot be had is asked for again a
               second later, and passed over after a third try: a gap in the
               sound is better than a stream that stops. */
            if (left && queued_ms < AHEAD_MS && ticks() >= retry_at) {
                for (int i = 0; i < ns; i++) {
                    if (segs[i].seq < next_seq) continue;
                    if (take_segment(segs[i].uri)) {
                        next_seq = segs[i].seq + 1;
                        fails = 0;
                        if (frames_bad) say("some of the sound could not be decoded");
                        else if (!w_same(status, "playing")) say("playing");
                    } else {
                        say("a segment could not be fetched");
                        retry_at = ticks() + 100;
                        if (++fails >= 3) { next_seq = segs[i].seq + 1; fails = 0; }
                    }
                    break;
                }
                feed();
                left = ns > 0 && next_seq <= segs[ns - 1].seq;
            }
            /* The end: everything fetched, heard and shown. */
            if (ended && !left && qlen <= 2 && !au_head) {
                h264_flush(&vdec);
                while (video_step(win)) { if (win >= 0) win_commit(win); }
                say("the stream has ended");
                puts("play: shown ");
                putn(shown);
                puts(" frames, passed over ");
                putn(dropped);
                putc('\n');
                playing = 0;
                hls_drop_all();
                au_clear();
                /* Without a window there is nothing to stay for once the card
                   has played what it holds. */
                if (win < 0) { sleep_ms(1500); exit(0); }
            }
        }

        if (win < 0) { sleep_ms(10); continue; }
        if (dirty || ticks() - last_draw >= 50) {
            draw_bar(win);
            win_commit(win);
            dirty = 0;
            last_draw = ticks();
        }
        /* A picture wants looking at every tick; sound alone every few. */
        ui_due(ticks() + (have_video && playing ? 1 : 4));
        ui_wait(win);
    }
}
