/* A player for live streams: a Twitch channel, or any HLS stream.
 *
 *   play twitch:<channel>         what the channel is broadcasting now
 *   play https://.../index.m3u8   any HLS stream of AAC in MPEG-TS
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
 * transport stream (ts.h); its AAC frames (aac.h) are decoded into a queue
 * of samples, and the queue is handed to the sound card as fast as it has
 * room, at the card's rate. This is the sound only: the picture needs an
 * H.264 decoder, which is next.
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

#define QUEUE_FRAMES (48000 * 20)        /* twenty seconds at 48 kHz */
#define SEGMENT_MAX  (4 << 20)

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
    if (!have_sound) { qlen = 0; return; }
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

/* --- the stream in --------------------------------------------------------------------- */

static aac_dec dec;
static int dec_open;
static int frames_decoded, frames_bad;

static void on_pes(void *ctx, int type, long long pts, const u8 *p, int n) {
    (void)ctx; (void)pts;
    if (type != TS_AAC) return;
    static short pcm[1024 * AAC_MAX_CH];
    int at = 0;
    while (at + 7 <= n) {
        aac_adts h;
        if (!aac_adts_read(p + at, n - at, &h) || at + h.frame_len > n) return;
        if (!dec_open) {
            dec_open = aac_open(&dec, h.sfi, h.channels);
            if (!dec_open) { say("the stream's sound is not one this can play"); return; }
            stream_rate = AAC_RATES[dec.rate_idx].rate;
            step = (u32)(((u64)stream_rate << 16) / (u32)(dev_rate ? dev_rate : stream_rate));
        }
        if (aac_decode(&dec, p + at + h.header_len, h.frame_len - h.header_len, pcm) == 1024) {
            queue_put(pcm, 1024, dec.channels);
            frames_decoded++;
        } else {
            frames_bad++;
        }
        at += h.frame_len;
    }
}

static char *segbuf, *textbuf;

/* A segment fetched and played into the queue; 0 when it could not be had. */
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

/* The rendition to play: Twitch's sound-only one, or the smallest. */
static int pick(const hls_variant *v, int n) {
    int best = -1;
    for (int i = 0; i < n; i++) if (w_same(v[i].name, "audio_only")) return i;
    for (int i = 0; i < n; i++) if (best < 0 || v[i].bandwidth < v[best].bandwidth) best = i;
    return best;
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

static void draw(int win) {
    int w = win_width(win), h = win_height(win);
    u32 *px = win_surface(win);
    if (!px || w <= 0 || h <= 0) return;
    surface s = { px, w, h };
    ui_theme t = ui_load_theme();
    fill(&s, t.bg);
    face_draw(&s, UI_PAD * 2, UI_PAD * 2, title, t.fg, UI_FACE_HEAD);
    face_draw(&s, UI_PAD * 2, UI_PAD * 2 + 34, status, t.dim, UI_FACE_BODY);
    char line[96];
    int n = 0, secs10 = stream_rate ? qlen * 10 / stream_rate : 0;
    n = wh_add(line, sizeof(line), n, "sound only; ");
    n = wh_add_num(line, sizeof(line), n, secs10 / 10);
    n = wh_add(line, sizeof(line), n, ".");
    n = wh_add_num(line, sizeof(line), n, secs10 % 10);
    wh_add(line, sizeof(line), n, " s waiting to be played");
    face_draw(&s, UI_PAD * 2, UI_PAD * 2 + 58, line, t.dim, UI_FACE_BODY);
    win_commit(win);
}

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "";
    int win = win_create("Player", 460, 140);
    queue = (short *)malloc((u64)QUEUE_FRAMES * 4);
    segbuf = (char *)malloc(SEGMENT_MAX);
    textbuf = (char *)malloc(1 << 20);
    if (!queue || !segbuf || !textbuf) { say("not enough memory for a stream"); exit(1); }

    zelr_sound snd;
    if (sound_info(&snd) == 0 && snd.present) {
        have_sound = 1;
        dev_rate = (int)snd.rate;
        dev_chans = (int)snd.channels > 2 ? 2 : (int)snd.channels;
        if (dev_chans < 1) dev_chans = 2;
    } else {
        say("this machine has no sound hardware; the stream is read but not heard");
    }
    step = 0x10000;

    static char master[HLS_URI];
    if (w_starts_fold(what, "twitch:")) {
        int k = 0;
        for (const char *p = "Twitch: "; *p; p++) title[k++] = *p;
        for (const char *p = what + 7; *p && k < (int)sizeof(title) - 1; p++) title[k++] = *p;
        title[k] = 0;
        draw(win);
        say("asking Twitch for the stream");
        if (!twitch_master(what + 7, master, sizeof(master))) { draw(win); linger(win); }
    } else if (w_starts_fold(what, "http://") || w_starts_fold(what, "https://")) {
        w_copy(master, sizeof(master), what, sizeof(master));
        w_copy(title, sizeof(title), what, sizeof(title));
    } else {
        w_copy(title, sizeof(title), "Player", sizeof(title));
        say("give it twitch:<channel> or the address of a playlist");
        draw(win);
        linger(win);
    }

    static hls_segment segs[64];
    int ns = 0, target = 0, fails = 0, retry_at = 0;
    long long next_seq = -1;
    int last_list = -1000, ended = 0;
    int last_draw = 0;

    /* The master playlist, or a media one given directly, which is then
       already read: a finished one is never asked for twice. */
    static hls_variant variants[16];
    static char media[HLS_URI];
    int st = 0;
    int n = hls_get(master, textbuf, (1 << 20) - 1, &st);
    if (n > 0 && st == 200) {
        textbuf[n] = 0;
        int nv = hls_master(master, textbuf, n, variants, 16);
        if (nv > 0) {
            int k = pick(variants, nv);
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
        while (win >= 0 && win_poll(win, &ev)) if (ev.type == WIN_EV_CLOSE) { hls_drop_all(); exit(0); }

        if (playing) {
            feed();
            int queued_ms = stream_rate ? (int)((long long)qlen * 1000 / stream_rate) : 0;
            /* Segments the playlist listed that are not taken yet. Until they
               are, it is not read again: a finished one never is, so a queue
               that filled before the last of them must still come back for
               it. */
            int left = ns > 0 && next_seq >= 0 && next_seq <= segs[ns - 1].seq;
            if (!ended && !left && queued_ms < 6000 && ticks() - last_list >= 100) {
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
            if (left && queued_ms < 6000 && ticks() >= retry_at) {
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
            /* What feed leaves behind is less than an output frame's worth. */
            if (ended && !left && qlen <= 2) {
                say("the stream has ended");
                playing = 0;
                hls_drop_all();
                /* Without a window there is nothing to stay for once the card
                   has played what it holds. */
                if (win < 0) { sleep_ms(1500); exit(0); }
            }
        }

        if (win < 0) { sleep_ms(40); continue; }
        if (dirty || ticks() - last_draw >= 50) { draw(win); dirty = 0; last_draw = ticks(); }
        ui_due(ticks() + 4);
        ui_wait(win);
    }
}
