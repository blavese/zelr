#pragma once
/* MPEG transport streams (ISO/IEC 13818-1), which is what an HLS segment
 * from Twitch is: 188-byte packets, each carrying a piece of one stream
 * named by its PID. The program association table (PID 0) says where the
 * program map is; the map says which PIDs carry what (0x0F is AAC in ADTS
 * frames, 0x1B is H.264); the pieces of each are put back together into
 * PES packets, each with the time it is to be presented (PTS, in 90 kHz
 * ticks), and handed to whoever asked.
 *
 * A PES packet is finished when the next one on its PID starts, or when the
 * segment ends (ts_flush): a video packet's length is often written as 0,
 * meaning "until the next". Packets with errors flagged, adaptation fields,
 * stuffing and the continuity counter are dealt with as the standard says;
 * a stream other than the two kinds above is left out. */
#include "zelr.h"
#include "alloc.h"

#define TS_PACKET 188
#define TS_STREAMS 4

enum { TS_AAC = 0x0F, TS_H264 = 0x1B };

typedef void (*ts_pes_fn)(void *ctx, int type, long long pts, const u8 *data, int len);

typedef struct {
    int pid, type;
    u8 *buf;
    int len, cap;
    long long pts;
    int started;
} ts_stream;

typedef struct {
    int pmt_pid;
    ts_stream s[TS_STREAMS];
    int nstreams;
    ts_pes_fn fn;
    void *ctx;
    int packets, errors;
} ts_demux;

static void ts_init(ts_demux *t, ts_pes_fn fn, void *ctx) {
    for (u32 i = 0; i < sizeof(*t); i++) ((volatile u8 *)t)[i] = 0;
    t->pmt_pid = -1;
    t->fn = fn;
    t->ctx = ctx;
}

static void ts_free(ts_demux *t) {
    for (int i = 0; i < t->nstreams; i++) { free(t->s[i].buf); t->s[i].buf = 0; }
}

/* A finished PES packet: its header read for the time, its payload handed on. */
static void ts_emit(ts_demux *t, ts_stream *s) {
    if (!s->started || s->len < 9) { s->len = 0; return; }
    const u8 *p = s->buf;
    if (p[0] != 0 || p[1] != 0 || p[2] != 1) { s->len = 0; t->errors++; return; }
    int flags = p[7], hlen = p[8];
    long long pts = -1;
    if ((flags & 0x80) && s->len >= 14)
        pts = ((long long)(p[9] & 0x0E) << 29) | ((long long)p[10] << 22) | ((long long)(p[11] & 0xFE) << 14)
              | ((long long)p[12] << 7) | (p[13] >> 1);
    int at = 9 + hlen;
    if (at <= s->len && t->fn) t->fn(t->ctx, s->type, pts, p + at, s->len - at);
    s->len = 0;
}

static int ts_append(ts_stream *s, const u8 *p, int n) {
    if (s->len + n > s->cap) {
        int cap = s->cap ? s->cap : 65536;
        while (cap < s->len + n) cap *= 2;
        u8 *b = (u8 *)realloc(s->buf, (u32)cap);
        if (!b) return 0;
        s->buf = b;
        s->cap = cap;
    }
    for (int i = 0; i < n; i++) s->buf[s->len + i] = p[i];
    s->len += n;
    return 1;
}

/* The sections of a table, past their pointer field. */
static void ts_table(ts_demux *t, int pid, const u8 *p, int n) {
    if (n < 1) return;
    int at = 1 + p[0];
    if (at + 8 > n) return;
    const u8 *s = p + at;
    int len = ((s[1] & 0x0F) << 8) | s[2];
    if (at + 3 + len > n || len < 9) return;
    int end = 3 + len - 4;                       /* before the CRC */
    if (pid == 0 && s[0] == 0x00) {              /* program association */
        for (int i = 8; i + 4 <= end; i += 4) {
            int program = (s[i] << 8) | s[i + 1];
            if (program != 0) { t->pmt_pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3]; break; }
        }
    } else if (pid == t->pmt_pid && s[0] == 0x02) {   /* program map */
        int info = ((s[10] & 0x0F) << 8) | s[11];
        for (int i = 12 + info; i + 5 <= end; ) {
            int type = s[i], es = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
            int eslen = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
            if (type == TS_AAC || type == TS_H264) {
                int known = 0;
                for (int k = 0; k < t->nstreams; k++) if (t->s[k].pid == es) known = 1;
                if (!known && t->nstreams < TS_STREAMS) {
                    ts_stream *st = &t->s[t->nstreams++];
                    st->pid = es;
                    st->type = type;
                }
            }
            i += 5 + eslen;
        }
    }
}

/* Packets, as many as are in data (a whole segment, or any run of whole
   packets). Answers how many were bad. */
static int ts_feed(ts_demux *t, const u8 *data, int n) {
    int bad = 0;
    for (int at = 0; at + TS_PACKET <= n; at += TS_PACKET) {
        const u8 *p = data + at;
        t->packets++;
        if (p[0] != 0x47) {
            /* Lost sync: find the next packet start. */
            int k = at + 1;
            while (k + TS_PACKET <= n && !(data[k] == 0x47 && (k + TS_PACKET >= n || data[k + TS_PACKET] == 0x47))) k++;
            at = k - TS_PACKET;
            bad++;
            continue;
        }
        if (p[1] & 0x80) { bad++; continue; }    /* the transport error indicator */
        int start = p[1] & 0x40;
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        int adapt = (p[3] >> 4) & 3;
        int off = 4;
        if (adapt == 2 || adapt == 3) off += 1 + p[4];
        if (adapt == 0 || adapt == 2 || off >= TS_PACKET) continue;   /* no payload */
        const u8 *pay = p + off;
        int plen = TS_PACKET - off;
        if (pid == 0 || pid == t->pmt_pid) {
            if (start) ts_table(t, pid, pay, plen);
            continue;
        }
        for (int k = 0; k < t->nstreams; k++) {
            ts_stream *s = &t->s[k];
            if (s->pid != pid) continue;
            if (start) {
                ts_emit(t, s);
                s->started = 1;
            }
            if (s->started && !ts_append(s, pay, plen)) bad++;
            /* A PES packet that says its length is done when it has it. */
            if (s->started && s->len >= 6) {
                int want = (s->buf[4] << 8) | s->buf[5];
                if (want && s->len >= want + 6) { s->len = want + 6; ts_emit(t, s); s->started = 0; }
            }
        }
    }
    t->errors += bad;
    return bad;
}

/* The end of a segment: what is left of each stream is a whole packet. */
static void ts_flush(ts_demux *t) {
    for (int k = 0; k < t->nstreams; k++) ts_emit(t, &t->s[k]);
}
