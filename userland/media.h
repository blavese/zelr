/* What a page's <video> or <audio> plays, below the script: samples a page
 * appended (Media Source Extensions, fragmented MP4) or a file it named,
 * decoded (h264.h, aac.h) and given out on a clock.
 *
 * A media_t is one element's pipeline. Its sources are the SourceBuffers a
 * script added: each keeps the bytes appended and not yet whole boxes, its
 * initialisation segment (mp4.h), and the samples it has read, in a list
 * by decoding time with their presentation times in seconds. What has been
 * read gives the buffered ranges a script asks for. Playing takes samples
 * in order from the playing position: sound is decoded at once into a queue
 * and handed to the card as it has room; pictures are decoded as they are
 * needed and each is shown when the sound reaches it (or, with no sound, a
 * clock of ticks). Seeking drops what is queued and starts again from the
 * last picture that needs no other before the new position.
 *
 * Memory: samples are copies, freed as they are played or removed; a source
 * keeps at most MEDIA_KEEP seconds behind the playing position.
 */
#pragma once
#include "mp4.h"

#define MEDIA_SOURCES 4
#define MEDIA_KEEP 30.0                  /* seconds kept behind the playing position */
#define MEDIA_QUEUE (48000 * 4)          /* four seconds of sound at 48 kHz, stereo */

typedef struct media_sample {
    struct media_sample *next;
    double dts, pts, dur;
    int key, len;
    u8 data[];
} media_sample;

typedef struct {
    int used, kind;                      /* MP4_VIDEO, MP4_AUDIO or both (a file with both) */
    u8 *pend;                            /* bytes appended and not yet whole boxes */
    long long pend_n, pend_cap;
    mp4_init init;
    int have_init;
    double offset;                       /* timestampOffset */
    media_sample *head, *tail;           /* by decoding time */
    int count;
    char why[80];
} media_source;

typedef struct {
    media_source src[MEDIA_SOURCES];
    int nsrc;
    double duration;                     /* NaN until known */
    double position;                     /* the playing position, seconds */
    int playing, ended, seeking;
    int eos, flushed;                    /* the page said the stream has ended; the decoder emptied */
    double volume;
    int muted;
    int no_sound;                        /* another element has the card: played on the clock */
    /* Video. */
    h264_dec *vdec;
    int vw, vh;                          /* the size of the last frame */
    u8 *frame;                           /* the last frame shown, three bytes a pixel, vw x vh */
    int frame_new;                       /* a new frame since it was last drawn */
    double vnext;                        /* the decoding time of the next video sample to feed */
    int need_key;                        /* at the start and after a seek: from a picture that needs no other */
    double show_from;                    /* pictures before this are decoded, not shown (a seek's) */
    int frames_shown, frames_dropped;
    /* Sound. */
    aac_dec *adec;
    int a_open, a_rate, a_chans;
    short *queue;
    int qhead, qlen;
    double a_next;                       /* the decoding time of the next audio sample to decode */
    double a_end;                        /* the time at the end of the queue */
    u32 step, frac;
    double clock_at;                     /* with no sound: when (media_seconds) position was last moved */
    double clock_pos;
    /* For a test: the seconds it says have passed, standing for the
       processor's ticks (negative: the real ones), and each decoded picture
       before it is converted. */
    double test_clock;
    void (*on_picture)(void *ctx, const h264_picture *p);
    void *on_ctx;
} media_t;

static int media_dev_rate, media_dev_chans, media_have_sound = -1;

static void media_sound_setup(void) {
    if (media_have_sound >= 0) return;
    media_have_sound = 0;
    zelr_sound snd;
    if (sound_info(&snd) == 0 && snd.present) {
        media_have_sound = 1;
        media_dev_rate = (int)snd.rate;
        media_dev_chans = (int)snd.channels > 2 ? 2 : (int)snd.channels;
        if (media_dev_chans < 1) media_dev_chans = 2;
    }
}

static void media_open(media_t *m) {
    for (int i = 0; i < (int)sizeof(*m); i++) ((volatile u8 *)m)[i] = 0;
    m->duration = 0.0 / 0.0;
    m->volume = 1;
    m->test_clock = -1;
    m->need_key = 1;                     /* the parameter sets go in before the first picture */
    media_sound_setup();
}

static void media_free_samples(media_source *s) {
    while (s->head) { media_sample *x = s->head; s->head = x->next; free(x); }
    s->tail = 0;
    s->count = 0;
}

static void media_close(media_t *m) {
    for (int i = 0; i < MEDIA_SOURCES; i++) {
        media_free_samples(&m->src[i]);
        if (m->src[i].pend) free(m->src[i].pend);
    }
    if (m->vdec) { h264_close(m->vdec); free(m->vdec); }
    if (m->adec) free(m->adec);
    if (m->frame) free(m->frame);
    if (m->queue) free(m->queue);
    for (int i = 0; i < (int)sizeof(*m); i++) ((volatile u8 *)m)[i] = 0;
}

/* --- appending ---------------------------------------------------------------------------- */

typedef struct { media_t *m; media_source *s; } media_ctx;

static void media_take(void *ctx, const mp4_track *t, long long dts, long long pts, int key, const u8 *data, int len) {
    media_ctx *c = (media_ctx *)ctx;
    media_source *s = c->s;
    if (!t->timescale || len <= 0 || s->count > 200000) return;
    media_sample *x = (media_sample *)malloc(sizeof(media_sample) + (u64)len);
    if (!x) return;
    x->next = 0;
    x->dts = (double)dts / t->timescale + s->offset;
    x->pts = (double)pts / t->timescale + s->offset;
    x->dur = t->def_duration ? (double)t->def_duration / t->timescale : 0;
    x->key = key;
    x->len = len;
    for (int i = 0; i < len; i++) x->data[i] = data[i];
    s->kind |= t->kind;
    /* In decoding order: an append is normally after what is there, a
       re-append of an earlier stretch replaces its samples. */
    if (!s->tail || x->dts > s->tail->dts) {
        if (s->tail) { if (!s->tail->dur) s->tail->dur = x->dts - s->tail->dts; s->tail->next = x; }
        else s->head = x;
        s->tail = x;
        s->count++;
        return;
    }
    media_sample **pp = &s->head;
    while (*pp && (*pp)->dts < x->dts) pp = &(*pp)->next;
    if (*pp && (*pp)->dts == x->dts) {          /* the same sample again: the new one wins */
        media_sample *old = *pp;
        x->next = old->next;
        *pp = x;
        if (s->tail == old) s->tail = x;
        free(old);
        return;
    }
    x->next = *pp;
    *pp = x;
    if (!x->next) s->tail = x;
    s->count++;
}

/* Bytes a script appended to source k; -1 with why said when they are not
   MP4 this reads. */
static int media_append(media_t *m, int k, const u8 *p, long long n) {
    media_source *s = &m->src[k];
    if (s->pend_n + n > s->pend_cap) {
        long long cap = (s->pend_n + n) * 2 + 65536;
        u8 *b = (u8 *)malloc((u64)cap);
        if (!b) return -1;
        for (long long i = 0; i < s->pend_n; i++) b[i] = s->pend[i];
        if (s->pend) free(s->pend);
        s->pend = b;
        s->pend_cap = cap;
    }
    for (long long i = 0; i < n; i++) s->pend[s->pend_n + i] = p[i];
    s->pend_n += n;
    long long used = 0;
    for (;;) {
        /* A whole top-level box at a time; a box cut short waits for the
           rest of it in the next append. */
        u8 *q = s->pend + used;
        long long left = s->pend_n - used, at = 0, bb, be;
        u32 type;
        if (!mp4_box(q, left, &at, &type, &bb, &be)) break;
        if (type == MP4_T('m', 'o', 'o', 'v')) {
            static mp4_init fresh;
            if (mp4_parse_init(q, be, &fresh) < 0) {
                int i = 0;
                for (; fresh.why[i] && i < (int)sizeof(s->why) - 1; i++) s->why[i] = fresh.why[i];
                s->why[i] = 0;
                return -1;
            }
            for (int a2 = 0; a2 < fresh.n; a2++)
                for (int b2 = 0; b2 < s->init.n; b2++)
                    if (fresh.t[a2].id == s->init.t[b2].id) fresh.t[a2].next_dts = s->init.t[b2].next_dts;
            for (int i = 0; i < (int)sizeof(fresh); i++) ((volatile u8 *)&s->init)[i] = ((u8 *)&fresh)[i];
            s->have_init = 1;
            if (s->init.duration && s->init.movie_timescale) {
                double d = (double)s->init.duration / s->init.movie_timescale;
                if (!(m->duration == m->duration) || d > m->duration) m->duration = d;
            }
            used += be;
            continue;
        }
        if (type == MP4_T('m', 'o', 'o', 'f')) {
            if (!s->have_init) {
                const char *w = "media before its initialisation segment";
                int i = 0;
                for (; w[i]; i++) s->why[i] = w[i];
                s->why[i] = 0;
                return -1;
            }
            /* The fragment and the mdat after it, both whole. */
            long long at2 = be, mb, me;
            u32 t2;
            if (!mp4_box(q, left, &at2, &t2, &mb, &me)) break;
            media_ctx c = { m, s };
            long long got = mp4_parse_media(q, me, &s->init, media_take, &c);
            if (got < 0) {
                int i = 0;
                for (; s->init.why[i] && i < (int)sizeof(s->why) - 1; i++) s->why[i] = s->init.why[i];
                s->why[i] = 0;
                return -1;
            }
            used += me;
            continue;
        }
        used += be;                                  /* ftyp, styp, sidx, free and the rest */
    }
    long long keep = s->pend_n - used;
    for (long long i = 0; i < keep; i++) s->pend[i] = s->pend[used + i];
    s->pend_n = keep;
    return 0;
}

/* What source k holds, as ranges of presentation time joined across gaps
   of under a tenth of a second; returns how many, into r[2*i], r[2*i+1].
   The samples are in decoding order, and pictures that are shown later than
   they are decoded (B pictures) come out of order: each is put into the
   ranges where it belongs, which are kept sorted, rather than taken as the
   end of the last. */
#define MEDIA_RANGES 64
static int media_buffered(media_t *m, int k, double *r, int max) {
    media_source *s = &m->src[k];
    double lo[MEDIA_RANGES], hi[MEDIA_RANGES];
    int n = 0;
    const double tol = 0.1;
    for (media_sample *x = s->head; x; x = x->next) {
        double a = x->pts, b = x->pts + (x->dur > 0 ? x->dur : 0.02);
        int i = 0;
        while (i < n && hi[i] + tol < a) i++;
        if (i < n && lo[i] - tol <= b) {
            if (a < lo[i]) lo[i] = a;
            if (b > hi[i]) hi[i] = b;
            /* It may now reach the ranges after it. */
            while (i + 1 < n && lo[i + 1] - tol <= hi[i]) {
                if (hi[i + 1] > hi[i]) hi[i] = hi[i + 1];
                for (int j = i + 1; j + 1 < n; j++) { lo[j] = lo[j + 1]; hi[j] = hi[j + 1]; }
                n--;
            }
            continue;
        }
        if (n == MEDIA_RANGES) {                 /* no room: joined to its neighbour */
            if (i == n) i--;
            if (a < lo[i]) lo[i] = a;
            if (b > hi[i]) hi[i] = b;
            continue;
        }
        for (int j = n; j > i; j--) { lo[j] = lo[j - 1]; hi[j] = hi[j - 1]; }
        lo[i] = a;
        hi[i] = b;
        n++;
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) { r[2 * i] = lo[i]; r[2 * i + 1] = hi[i]; }
    return n;
}

/* Takes out what lies in [from, to) of source k. */
static void media_remove(media_t *m, int k, double from, double to) {
    media_source *s = &m->src[k];
    media_sample **pp = &s->head, *last = 0;
    while (*pp) {
        media_sample *x = *pp;
        if (x->pts >= from && x->pts < to) {
            *pp = x->next;
            free(x);
            s->count--;
            continue;
        }
        last = x;
        pp = &x->next;
    }
    s->tail = last;
}

/* MediaSource.endOfStream: nothing more will be appended. The duration is
   then where what is buffered ends. */
static void media_end_of_stream(media_t *m) {
    m->eos = 1;
    double end = 0;
    for (int k = 0; k < MEDIA_SOURCES; k++) {
        double r[64];
        int n = media_buffered(m, k, r, 32);
        if (n && r[2 * n - 1] > end) end = r[2 * n - 1];
    }
    if (end > 0) m->duration = end;
}

/* --- playing ------------------------------------------------------------------------------ */

/* Seconds from the processor's ticks, or the test's. */
static double media_seconds(const media_t *m) {
    return m->test_clock >= 0 ? m->test_clock : ticks() / 100.0;
}

/* The time of what is heard now, in seconds: the sound's while there is
   sound to hear, and on from there by the ticks when it runs out (a video
   longer than its sound, or none). */
static double media_clock(media_t *m) {
    if (!m->playing) return m->position;
    double now = media_seconds(m);
    if (m->a_open && media_have_sound && m->a_rate) {
        zelr_sound snd;
        int queued = sound_info(&snd) == 0 ? (int)snd.queued : 0;
        if (m->qlen > 0 || queued > 0) {
            double t = m->a_end - (double)m->qlen / m->a_rate - (media_dev_rate ? (double)queued / media_dev_rate : 0);
            if (t < m->position) t = m->position;
            m->clock_pos = t;
            m->clock_at = now;
            return t;
        }
    }
    return m->clock_pos + (now - m->clock_at);
}

static void media_seek(media_t *m, double t) {
    m->position = t;
    m->clock_pos = t;
    m->clock_at = media_seconds(m);
    m->qlen = 0;
    m->a_next = t;
    m->a_end = t;
    m->vnext = t;
    m->need_key = 1;
    m->show_from = t;
    m->ended = 0;
    m->flushed = 0;
    if (m->vdec) { h264_close(m->vdec); h264_open(m->vdec); }
}

static void media_play(media_t *m) {
    if (m->playing) return;
    m->playing = 1;
    m->clock_pos = m->position;
    m->clock_at = media_seconds(m);
}

__attribute__((unused)) static void media_pause(media_t *m) {
    if (!m->playing) return;
    m->position = media_clock(m);
    m->playing = 0;
    m->clock_pos = m->position;
    /* The sound starts again from here, not from where the queue had got
       to; what the card still holds plays out. */
    m->qlen = 0;
    m->a_next = m->position;
    m->a_end = m->position;
}

/* Sound up to a little ahead of the clock, from every audio source. */
static void media_sound_step(media_t *m) {
    if (!media_have_sound || m->test_clock >= 0 || m->no_sound) return;
    for (int k = 0; k < MEDIA_SOURCES; k++) {
        media_source *s = &m->src[k];
        if (!(s->kind & MP4_AUDIO)) continue;
        mp4_track *t = 0;
        for (int i = 0; i < s->init.n; i++) if (s->init.t[i].kind == MP4_AUDIO) t = &s->init.t[i];
        if (!t) continue;
        if (!m->a_open) {
            if (!m->adec) m->adec = (aac_dec *)malloc(sizeof(aac_dec));
            if (!m->queue) m->queue = (short *)malloc((u64)MEDIA_QUEUE * 4);
            if (!m->adec || !m->queue || !aac_open(m->adec, t->sfi, t->channels ? t->channels : 2)) return;
            m->a_open = 1;
            m->a_rate = AAC_RATES[m->adec->rate_idx].rate;
            m->a_chans = m->adec->channels;
            m->step = (u32)(((u64)m->a_rate << 16) / (u32)(media_dev_rate ? media_dev_rate : m->a_rate));
            m->a_end = m->position;
        }
        static short pcm[1024 * AAC_MAX_CH];
        while (m->qlen + 1024 < MEDIA_QUEUE && m->a_end < media_clock(m) + 2.0) {
            media_sample *x = s->head;
            while (x && x->pts + 0.001 < m->a_next) x = x->next;
            if (!x) break;
            m->a_next = x->pts + (x->dur > 0 ? x->dur : 1024.0 / m->a_rate);
            if (aac_decode(m->adec, x->data, x->len, pcm) != 1024) continue;
            if (m->qlen == 0) m->a_end = x->pts;
            for (int i = 0; i < 1024 && m->qlen < MEDIA_QUEUE; i++) {
                int at = (m->qhead + m->qlen) % MEDIA_QUEUE;
                int v = m->muted ? 0 : (int)(m->volume * 256);
                short l = pcm[i * m->a_chans], r = m->a_chans > 1 ? pcm[i * m->a_chans + 1] : l;
                m->queue[at * 2] = (short)((l * v) >> 8);
                m->queue[at * 2 + 1] = (short)((r * v) >> 8);
                m->qlen++;
            }
            m->a_end = x->pts + 1024.0 / m->a_rate;
        }
        break;
    }
    if (!m->playing || !m->a_open) return;
    zelr_sound snd;
    if (sound_info(&snd) != 0) return;
    int room = (int)snd.room;
    static short out[4096 * 2];
    while (room > 64 && m->qlen > 2) {
        int made = 0, want = room < 4096 ? room : 4096;
        while (made < want && m->qlen > 2) {
            int i0 = m->qhead, i1 = (m->qhead + 1) % MEDIA_QUEUE, f = (int)(m->frac & 0xFFFF);
            for (int c = 0; c < media_dev_chans; c++) {
                int cc = c < 2 ? c : 1, a = m->queue[i0 * 2 + cc], b = m->queue[i1 * 2 + cc];
                out[made * media_dev_chans + c] = (short)(a + (((b - a) * f) >> 16));
            }
            made++;
            m->frac += m->step;
            while (m->frac >= 0x10000) { m->frac -= 0x10000; m->qhead = (m->qhead + 1) % MEDIA_QUEUE; m->qlen--; }
        }
        if (!made) break;
        sound_write(out, made);
        room -= made;
    }
}

static void media_convert(media_t *m, const h264_picture *p) {
    if (p->width <= 0 || p->height <= 0 || p->width > 4096 || p->height > 4096) return;
    if (m->vw != p->width || m->vh != p->height || !m->frame) {
        if (m->frame) free(m->frame);
        m->frame = (u8 *)malloc((u64)p->width * p->height * 3);
        if (!m->frame) return;
        m->vw = p->width;
        m->vh = p->height;
    }
    int hd = p->matrix == 1 || (p->matrix != 5 && p->matrix != 6 && p->height > 576), full = p->full_range;
    int cy = full ? 256 : 298, yo = full ? 0 : 16, rv, gu, gv, bu;
    if (hd) { rv = full ? 403 : 459; gu = full ? 48 : 55; gv = full ? 120 : 136; bu = full ? 475 : 541; }
    else    { rv = full ? 359 : 409; gu = full ? 88 : 100; gv = full ? 183 : 208; bu = full ? 454 : 516; }
    for (int y = 0; y < p->height; y++) {
        const u8 *ly = p->y + y * p->stride_y, *lu = p->cb + (y >> 1) * p->stride_c, *lv = p->cr + (y >> 1) * p->stride_c;
        u8 *o = m->frame + y * p->width * 3;
        for (int x = 0; x < p->width; x++) {
            int c = (ly[x] - yo) * cy, d = lu[x >> 1] - 128, e = lv[x >> 1] - 128;
            int r = (c + rv * e + 128) >> 8, g = (c - gu * d - gv * e + 128) >> 8, b = (c + bu * d + 128) >> 8;
            r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
            o[3 * x] = (u8)r;
            o[3 * x + 1] = (u8)g;
            o[3 * x + 2] = (u8)b;
        }
    }
    m->frame_new = 1;
}

/* Pictures: decode what is needed for one to be due, and show it when the
   clock reaches it. Returns whether a new frame was made. */
static int media_video_step(media_t *m) {
    for (int k = 0; k < MEDIA_SOURCES; k++) {
        media_source *s = &m->src[k];
        if (!(s->kind & MP4_VIDEO)) continue;
        mp4_track *t = 0;
        for (int i = 0; i < s->init.n; i++) if (s->init.t[i].kind == MP4_VIDEO) t = &s->init.t[i];
        if (!t) continue;
        if (!m->vdec) {
            m->vdec = (h264_dec *)malloc(sizeof(h264_dec));
            if (!m->vdec) return 0;
            h264_open(m->vdec);
        }
        double now = media_clock(m);
        for (int guard = 0; guard < 16; guard++) {
            long long pts;
            if (h264_peek(m->vdec, &pts)) {
                double tp = (double)pts / 1000000.0;
                h264_picture p;
                if (tp < m->show_from - 0.001) {         /* before a seek's position: only to decode from */
                    h264_frame(m->vdec, &p);
                    continue;
                }
                if (m->playing ? tp > now + 0.01 : (m->frames_shown && tp > m->position + 0.01)) return 0;
                h264_frame(m->vdec, &p);
                long long np;
                if (m->playing && tp < now - 0.1 && h264_peek(m->vdec, &np) && (double)np / 1000000.0 <= now) {
                    m->frames_dropped++;
                    continue;
                }
                if (m->on_picture) m->on_picture(m->on_ctx, &p);
                media_convert(m, &p);
                m->frames_shown++;
                return 1;
            }
            if (h264_room(m->vdec) <= 0) return 0;
            media_sample *x = s->head;
            while (x && x->dts < m->vnext) x = x->next;      /* vnext is just past the last one fed */
            if (m->need_key) {
                /* Back to the last picture that needs no other at or before
                   the position. */
                media_sample *key = 0;
                for (media_sample *y = s->head; y; y = y->next) {
                    if (y->key && y->pts <= m->position + 0.001) key = y;
                    if (y->pts > m->position + 0.001 && key) break;
                }
                if (!key) key = s->head;
                while (key && !key->key) key = key->next;
                x = key;
                if (!x) return 0;
                m->need_key = 0;
                if (t->params_n) {
                    int at = 0, st, len;
                    while (h264_next_nal(t->params, t->params_n, &at, &st, &len)) h264_nal(m->vdec, t->params + st, len);
                }
            }
            if (!x) {
                /* Nothing more to feed: at the end of the stream, what the
                   decoder still holds for reordering comes out. */
                if (m->eos && !m->flushed) { h264_flush(m->vdec); m->flushed = 1; continue; }
                return 0;
            }
            m->vnext = x->dts + 0.0005;
            m->vdec->next_pts = (long long)(x->pts * 1000000.0);
            /* Length-prefixed NAL units. */
            int nl = t->nal_len ? t->nal_len : 4, at = 0;
            while (at + nl <= x->len) {
                int len = 0;
                for (int i = 0; i < nl; i++) len = (len << 8) | x->data[at + i];
                at += nl;
                if (len <= 0 || at + len > x->len) break;
                h264_nal(m->vdec, x->data + at, len);
                at += len;
            }
            m->vnext = x->dts + 0.0005;
        }
        return 0;
    }
    return 0;
}

/* Once a pass of the browser's loop: sound fed, the position moved on,
   pictures made, samples long played let go. Returns whether a frame is new. */
static int media_step(media_t *m) {
    media_sound_step(m);
    int fresh = media_video_step(m);
    if (m->playing) {
        double now = media_clock(m);
        if (now > m->position) m->position = now;
        if (m->duration == m->duration && m->position >= m->duration - 0.02) {
            m->ended = 1;
            m->playing = 0;
            m->position = m->duration;           /* the end is where it stops */
        }
        for (int k = 0; k < MEDIA_SOURCES; k++) {
            media_source *s = &m->src[k];
            while (s->head && s->head->pts + s->head->dur < m->position - MEDIA_KEEP) {
                media_sample *x = s->head;
                s->head = x->next;
                if (!s->head) s->tail = 0;
                free(x);
                s->count--;
            }
        }
    }
    return fresh;
}
