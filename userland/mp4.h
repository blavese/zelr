/* MP4 (ISO/IEC 14496-12) as Media Source Extensions feed it: an
 * initialisation segment (ftyp, moov) that says what the tracks are, then
 * media segments (moof, mdat) that carry their samples. What a page's
 * player appends to a SourceBuffer, and what DASH and most sites' video is.
 *
 * Tracks of H.264 (avc1, with avcC: the parameter sets and the length of a
 * NAL unit's length) and AAC (mp4a, with esds: the AudioSpecificConfig).
 * Anything else is noted and passed over. A sample comes out with its
 * decoding and presentation times in the track's timescale, whether it can
 * be decoded on its own, and its bytes (still in the file's buffer: valid
 * until the caller's next append).
 *
 * Every box's size is checked against what holds it, so a damaged or hostile
 * segment is refused, never read past.
 */
#pragma once

#define MP4_TRACKS 4

typedef struct {
    int id, kind;                       /* MP4_VIDEO or MP4_AUDIO; 0 not one this reads */
    u32 timescale;
    int width, height, nal_len;         /* H.264 */
    u8 params[1024];                    /* its SPS and PPS, as Annex B */
    int params_n;
    int sfi, channels, aot;             /* AAC: the rate's index, channels, object type */
    u32 def_duration, def_size, def_flags;   /* the trex defaults */
    long long next_dts;                 /* where a fragment without tfdt carries on */
} mp4_track;

enum { MP4_VIDEO = 1, MP4_AUDIO = 2 };

typedef struct {
    mp4_track t[MP4_TRACKS];
    int n;
    u32 movie_timescale;
    long long duration;                 /* in the movie's timescale, 0 when it does not say */
    char why[80];
} mp4_init;

typedef void (*mp4_sample_fn)(void *ctx, const mp4_track *t, long long dts, long long pts, int key,
                              const u8 *data, int len);

static inline u32 mp4_u32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline u32 mp4_u16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static inline u64 mp4_u64(const u8 *p) { return (u64)mp4_u32(p) << 32 | mp4_u32(p + 4); }

static void mp4_why(mp4_init *m, const char *s) {
    int i = 0;
    for (; s[i] && i < (int)sizeof(m->why) - 1; i++) m->why[i] = s[i];
    m->why[i] = 0;
}

/* The next box in [*at, end): its type, and where its body starts and ends.
   0 at the end or when a size does not fit. */
static int mp4_box(const u8 *p, long long end, long long *at, u32 *type, long long *body, long long *box_end) {
    long long a = *at;
    if (a + 8 > end) return 0;
    u64 size = mp4_u32(p + a);
    *type = mp4_u32(p + a + 4);
    long long head = 8;
    if (size == 1) {
        if (a + 16 > end) return 0;
        size = mp4_u64(p + a + 8);
        head = 16;
    } else if (size == 0) {
        size = (u64)(end - a);
    }
    if (size < (u64)head || (u64)(end - a) < size) return 0;
    *body = a + head;
    *box_end = a + (long long)size;
    *at = *box_end;
    return 1;
}

#define MP4_T(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

/* The first box of a type in [at, end), or -1. */
static int mp4_find(const u8 *p, long long at, long long end, u32 want, long long *body, long long *bend) {
    u32 t;
    long long b, e;
    while (mp4_box(p, end, &at, &t, &b, &e))
        if (t == want) { *body = b; *bend = e; return 1; }
    return 0;
}

/* An MPEG-4 descriptor's length (up to four bytes of seven bits). */
static int mp4_desc_len(const u8 *p, long long end, long long *at) {
    int n = 0;
    for (int i = 0; i < 4 && *at < end; i++) {
        u8 b = p[(*at)++];
        n = (n << 7) | (b & 0x7F);
        if (!(b & 0x80)) return n;
    }
    return n;
}

static void mp4_esds(const u8 *p, long long b, long long e, mp4_track *t) {
    long long at = b + 4;                       /* version and flags */
    while (at + 2 <= e) {
        int tag = p[at++];
        int len = mp4_desc_len(p, e, &at);
        long long end = at + len;
        if (end > e) return;
        if (tag == 3) {                         /* ES_Descriptor */
            if (at + 3 > end) return;
            int flags = p[at + 2];
            at += 3;
            if (flags & 0x80) at += 2;
            if (flags & 0x40) { if (at >= end) return; at += 1 + p[at]; }
            if (flags & 0x20) at += 2;
            continue;                           /* its children follow */
        }
        if (tag == 4) { at += 13; continue; }  /* DecoderConfig: its own fields, then children */
        if (tag == 5 && len >= 2) {            /* the AudioSpecificConfig */
            int aot = p[at] >> 3;
            int sfi = ((p[at] & 7) << 1) | (p[at + 1] >> 7);
            int ch = (p[at + 1] >> 3) & 15;
            if (aot == 31) return;              /* an extended type: not one this reads */
            t->aot = aot;
            t->sfi = sfi;
            t->channels = ch;
            return;
        }
        at = end;
    }
}

static void mp4_avcc(const u8 *p, long long b, long long e, mp4_track *t) {
    if (b + 7 > e) return;
    t->nal_len = (p[b + 4] & 3) + 1;
    long long at = b + 5;
    int n = 0;
    for (int set = 0; set < 2; set++) {
        if (at >= e) return;
        int count = set == 0 ? (p[at] & 31) : p[at];
        at++;
        for (int i = 0; i < count; i++) {
            if (at + 2 > e) return;
            int len = (int)mp4_u16(p + at);
            at += 2;
            if (at + len > e || n + 4 + len > (int)sizeof(t->params)) return;
            t->params[n] = 0; t->params[n + 1] = 0; t->params[n + 2] = 0; t->params[n + 3] = 1;
            for (int k = 0; k < len; k++) t->params[n + 4 + k] = p[at + k];
            n += 4 + len;
            at += len;
        }
    }
    t->params_n = n;
}

static void mp4_trak(const u8 *p, long long b, long long e, mp4_init *m) {
    if (m->n >= MP4_TRACKS) return;
    mp4_track *t = &m->t[m->n];
    for (int i = 0; i < (int)sizeof(*t); i++) ((volatile u8 *)t)[i] = 0;
    long long tb, te, mb, me, nb, ne, sb, se;
    if (mp4_find(p, b, e, MP4_T('t', 'k', 'h', 'd'), &tb, &te) && tb + 24 <= te)
        t->id = (int)mp4_u32(p + tb + (p[tb] == 1 ? 20 : 12));
    if (!mp4_find(p, b, e, MP4_T('m', 'd', 'i', 'a'), &mb, &me)) return;
    long long hb, he;
    if (mp4_find(p, mb, me, MP4_T('m', 'd', 'h', 'd'), &hb, &he) && hb + 24 <= he)
        t->timescale = mp4_u32(p + hb + (p[hb] == 1 ? 20 : 12));
    u32 handler = 0;
    if (mp4_find(p, mb, me, MP4_T('h', 'd', 'l', 'r'), &hb, &he) && hb + 12 <= he) handler = mp4_u32(p + hb + 8);
    if (!mp4_find(p, mb, me, MP4_T('m', 'i', 'n', 'f'), &nb, &ne)) return;
    if (!mp4_find(p, nb, ne, MP4_T('s', 't', 'b', 'l'), &nb, &ne)) return;
    if (!mp4_find(p, nb, ne, MP4_T('s', 't', 's', 'd'), &sb, &se) || sb + 8 > se) return;
    long long at = sb + 8, eb, ee;
    u32 type;
    if (!mp4_box(p, se, &at, &type, &eb, &ee)) return;
    if (handler == MP4_T('v', 'i', 'd', 'e') && (type == MP4_T('a', 'v', 'c', '1') || type == MP4_T('a', 'v', 'c', '3'))) {
        if (eb + 78 > ee) return;
        t->width = (int)mp4_u16(p + eb + 24);
        t->height = (int)mp4_u16(p + eb + 26);
        long long cb, ce;
        if (mp4_find(p, eb + 78, ee, MP4_T('a', 'v', 'c', 'C'), &cb, &ce)) mp4_avcc(p, cb, ce, t);
        t->kind = MP4_VIDEO;
    } else if (handler == MP4_T('s', 'o', 'u', 'n') && type == MP4_T('m', 'p', '4', 'a')) {
        if (eb + 28 > ee) return;
        t->channels = (int)mp4_u16(p + eb + 16);
        long long cb, ce;
        if (mp4_find(p, eb + 28, ee, MP4_T('e', 's', 'd', 's'), &cb, &ce)) mp4_esds(p, cb, ce, t);
        t->kind = MP4_AUDIO;
    }
    if (t->timescale) m->n++;
}

/* An initialisation segment; how many bytes it was, or -1 with why said. */
static long long mp4_parse_init(const u8 *p, long long n, mp4_init *m) {
    long long at = 0, b, e;
    u32 type;
    long long moov_end = -1;
    for (int i = 0; i < (int)sizeof(*m); i++) ((volatile u8 *)m)[i] = 0;
    while (mp4_box(p, n, &at, &type, &b, &e)) {
        if (type != MP4_T('m', 'o', 'o', 'v')) continue;
        long long hb, he;
        if (mp4_find(p, b, e, MP4_T('m', 'v', 'h', 'd'), &hb, &he) && hb + 28 <= he) {
            int v1 = p[hb] == 1;
            m->movie_timescale = mp4_u32(p + hb + (v1 ? 20 : 12));
            m->duration = v1 ? (long long)mp4_u64(p + hb + 24) : (long long)mp4_u32(p + hb + 16);
        }
        long long at2 = b, tb, te;
        u32 t2;
        while (mp4_box(p, e, &at2, &t2, &tb, &te))
            if (t2 == MP4_T('t', 'r', 'a', 'k')) mp4_trak(p, tb, te, m);
        long long xb, xe;
        if (mp4_find(p, b, e, MP4_T('m', 'v', 'e', 'x'), &xb, &xe)) {
            long long at3 = xb, rb, re;
            u32 t3;
            while (mp4_box(p, xe, &at3, &t3, &rb, &re)) {
                if (t3 != MP4_T('t', 'r', 'e', 'x') || rb + 24 > re) continue;
                int id = (int)mp4_u32(p + rb + 4);
                for (int k = 0; k < m->n; k++)
                    if (m->t[k].id == id) {
                        m->t[k].def_duration = mp4_u32(p + rb + 12);
                        m->t[k].def_size = mp4_u32(p + rb + 16);
                        m->t[k].def_flags = mp4_u32(p + rb + 20);
                    }
            }
        }
        moov_end = e;
        break;
    }
    if (moov_end < 0) { mp4_why(m, "no moov in the initialisation segment"); return -1; }
    if (!m->n) { mp4_why(m, "no track this can play (H.264 or AAC)"); return -1; }
    return moov_end;
}

/* One traf of a moof whose start is moof_at: its samples, read from the
   buffer p of n bytes. */
static int mp4_traf(const u8 *p, long long n, long long b, long long e, long long moof_at, mp4_init *m,
                    mp4_sample_fn fn, void *ctx) {
    long long hb, he;
    if (!mp4_find(p, b, e, MP4_T('t', 'f', 'h', 'd'), &hb, &he) || hb + 8 > he) return -1;
    u32 flags = mp4_u32(p + hb) & 0xFFFFFF;
    int id = (int)mp4_u32(p + hb + 4);
    mp4_track *t = 0;
    for (int k = 0; k < m->n; k++) if (m->t[k].id == id) t = &m->t[k];
    if (!t) return 0;                          /* a track this does not read */
    long long at = hb + 8;
    long long base = moof_at;
    u32 def_dur = t->def_duration, def_size = t->def_size, def_flags = t->def_flags;
    if (flags & 0x01) { if (at + 8 > he) return -1; base = (long long)mp4_u64(p + at); at += 8; }
    if (flags & 0x02) at += 4;
    if (flags & 0x08) { if (at + 4 > he) return -1; def_dur = mp4_u32(p + at); at += 4; }
    if (flags & 0x10) { if (at + 4 > he) return -1; def_size = mp4_u32(p + at); at += 4; }
    if (flags & 0x20) { if (at + 4 > he) return -1; def_flags = mp4_u32(p + at); at += 4; }
    long long dts = t->next_dts;
    long long db, de;
    if (mp4_find(p, b, e, MP4_T('t', 'f', 'd', 't'), &db, &de) && db + 8 <= de)
        dts = p[db] == 1 && db + 12 <= de ? (long long)mp4_u64(p + db + 4) : (long long)mp4_u32(p + db + 4);
    /* Each trun in turn: the samples follow on from the last one's end
       unless a data offset says otherwise. */
    long long at2 = b, rb, re, data = base;
    u32 type;
    while (mp4_box(p, e, &at2, &type, &rb, &re)) {
        if (type != MP4_T('t', 'r', 'u', 'n') || rb + 8 > re) continue;
        int version = p[rb];
        u32 tf = mp4_u32(p + rb) & 0xFFFFFF;
        u32 count = mp4_u32(p + rb + 4);
        long long q = rb + 8;
        if (tf & 0x001) { if (q + 4 > re) return -1; data = base + (int)mp4_u32(p + q); q += 4; }
        u32 first_flags = 0;
        int has_first = 0;
        if (tf & 0x004) { if (q + 4 > re) return -1; first_flags = mp4_u32(p + q); has_first = 1; q += 4; }
        int per = ((tf & 0x100) ? 4 : 0) + ((tf & 0x200) ? 4 : 0) + ((tf & 0x400) ? 4 : 0) + ((tf & 0x800) ? 4 : 0);
        if (count > 1000000 || q + (long long)per * count > re) return -1;
        for (u32 i = 0; i < count; i++) {
            u32 dur = def_dur, size = def_size, sflags = (i == 0 && has_first) ? first_flags : def_flags;
            int cto = 0;
            if (tf & 0x100) { dur = mp4_u32(p + q); q += 4; }
            if (tf & 0x200) { size = mp4_u32(p + q); q += 4; }
            if (tf & 0x400) { u32 f = mp4_u32(p + q); q += 4; if (!(i == 0 && has_first)) sflags = f; }
            if (tf & 0x800) { cto = version ? (int)mp4_u32(p + q) : (int)mp4_u32(p + q); q += 4; }
            if (data < 0 || data + (long long)size > n) return -1;
            int key = !(sflags & 0x00010000);
            if (t->kind) fn(ctx, t, dts, dts + cto, key, p + data, (int)size);
            data += size;
            dts += dur;
        }
    }
    t->next_dts = dts;
    return 0;
}

/* Media segments: every moof and its samples; how many bytes were whole
   boxes (a caller keeps the rest for the next append), or -1 with why said. */
static long long mp4_parse_media(const u8 *p, long long n, mp4_init *m, mp4_sample_fn fn, void *ctx) {
    long long at = 0, b, e, done = 0;
    u32 type;
    for (;;) {
        long long start = at;
        if (!mp4_box(p, n, &at, &type, &b, &e)) break;
        if (type == MP4_T('m', 'o', 'o', 'f')) {
            /* Its samples are in the mdat after it: wait for that to arrive
               whole before reading either. */
            long long at2 = e, mb, me;
            u32 t2;
            if (!mp4_box(p, n, &at2, &t2, &mb, &me)) break;
            long long moof_at = start;
            long long tb, te, at3 = b;
            u32 t3;
            while (mp4_box(p, e, &at3, &t3, &tb, &te))
                if (t3 == MP4_T('t', 'r', 'a', 'f') && mp4_traf(p, n, tb, te, moof_at, m, fn, ctx) < 0) {
                    mp4_why(m, "a fragment whose samples are not where it says");
                    return -1;
                }
            at = me;
            done = me;
            continue;
        }
        if (type == MP4_T('m', 'o', 'o', 'v') || type == MP4_T('f', 't', 'y', 'p')) {
            /* An initialisation segment again (a change of quality, or a
               player that sends it each time): read for what it says. */
            if (type == MP4_T('m', 'o', 'o', 'v')) {
                static mp4_init again;
                if (mp4_parse_init(p + start, e - start, &again) > 0) {
                    /* Where each track had got to stays: the times run on. */
                    for (int k = 0; k < again.n; k++)
                        for (int j = 0; j < m->n; j++)
                            if (again.t[k].id == m->t[j].id) again.t[k].next_dts = m->t[j].next_dts;
                    for (int i = 0; i < (int)sizeof(again); i++) ((volatile u8 *)m)[i] = ((u8 *)&again)[i];
                }
            }
        }
        done = e;
    }
    return done;
}
