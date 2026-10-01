#pragma once
/* HTTP Live Streaming (RFC 8216), as a live channel on Twitch and a great
 * many other streams arrive: a master playlist naming the renditions (each
 * its bandwidth, resolution, codecs and a playlist of its own) and a media
 * playlist naming the segments in order, each a few seconds of the stream
 * and numbered by the media sequence. A live media playlist is asked for
 * again every target duration or so and grows at the end.
 *
 * With it, the fetching such a stream needs, which the browser's (fetch.h)
 * does not do: addresses of any length (Twitch's carries its whole access
 * token, well past url_t's path), and a connection kept to each host, since
 * the playlists come from one host and the segments from another and every
 * new TLS connection is a handshake the stream would have to wait out. The
 * body is either a length or chunks; nothing is asked for compressed. */
#include "zelr.h"
#include "alloc.h"
#include "web.h"

#define HLS_URI   2048
#define HLS_CONNS 2

/* --- addresses ---------------------------------------------------------------------------- */

typedef struct { char host[URL_HOST]; int port, secure; const char *path; } hls_where;

static int hls_split(const char *url, hls_where *w) {
    const char *p = url;
    w->secure = 1;
    if (w_starts_fold(p, "https://")) p += 8;
    else if (w_starts_fold(p, "http://")) { w->secure = 0; p += 7; }
    else return 0;
    int n = 0;
    while (*p && *p != '/' && *p != '?' && n < URL_HOST - 1) w->host[n++] = *p++;
    w->host[n] = 0;
    w->port = w->secure ? 443 : 80;
    for (int i = 0; w->host[i]; i++)
        if (w->host[i] == ':') {
            int v = 0;
            for (int j = i + 1; w->host[j] >= '0' && w->host[j] <= '9'; j++) v = v * 10 + (w->host[j] - '0');
            if (v > 0 && v < 65536) w->port = v;
            w->host[i] = 0;
            break;
        }
    w->path = *p ? p : "/";
    return n > 0;
}

/* An address relative to a playlist's, made whole. */
static void hls_join(const char *base, const char *ref, char *out, int cap) {
    if (w_starts_fold(ref, "https://") || w_starts_fold(ref, "http://")) { w_copy(out, cap, ref, cap); return; }
    int n = 0, cut = 0;
    if (ref[0] == '/') {
        const char *p = base;
        int slashes = 0;
        for (; *p && n < cap - 1; p++) {
            if (*p == '/' && ++slashes == 3) break;
            out[n++] = *p;
        }
    } else {
        for (int i = 0; base[i] && base[i] != '?' && i < cap - 1; i++) { out[i] = base[i]; if (base[i] == '/') cut = i + 1; }
        n = cut;
    }
    for (const char *p = ref; *p && n < cap - 1; p++) out[n++] = *p;
    out[n] = 0;
}

/* --- fetching ----------------------------------------------------------------------------- */

typedef struct { char host[URL_HOST]; int port, secure, sock; } hls_conn;
static hls_conn hls_conns[HLS_CONNS];
static int hls_conn_next;

static int hls_send_all(int sock, const char *p, int n) {
    while (n > 0) {
        int k = n > 1400 ? 1400 : n;
        if (send(sock, p, k) != k) return 0;
        p += k;
        n -= k;
    }
    return 1;
}

/* A kept connection to the host, or a new one in the place of the oldest. */
static int hls_connect(const hls_where *w, int fresh) {
    for (int i = 0; i < HLS_CONNS; i++) {
        hls_conn *c = &hls_conns[i];
        if (c->sock > 0 && c->port == w->port && c->secure == w->secure && w_same_fold(c->host, w->host)) {
            if (!fresh) return i;
            disconnect(c->sock);
            c->sock = 0;
            break;
        }
    }
    int i = -1;
    for (int k = 0; k < HLS_CONNS; k++) if (hls_conns[k].sock <= 0) { i = k; break; }
    if (i < 0) {
        i = hls_conn_next;
        hls_conn_next = (hls_conn_next + 1) % HLS_CONNS;
        disconnect(hls_conns[i].sock);
        hls_conns[i].sock = 0;
    }
    int s = w->secure ? connect_tls(w->host, w->port) : connect(w->host, w->port);
    if (s < 0) return -1;
    hls_conn *c = &hls_conns[i];
    w_copy(c->host, sizeof(c->host), w->host, sizeof(c->host));
    c->port = w->port;
    c->secure = w->secure;
    c->sock = s;
    return i;
}

static void hls_drop_all(void) {
    for (int i = 0; i < HLS_CONNS; i++) if (hls_conns[i].sock > 0) { disconnect(hls_conns[i].sock); hls_conns[i].sock = 0; }
}

/* Reads until `need` bytes are in hand (or the end); how many there are. */
static int hls_fill(int sock, char *buf, int have, int need, int cap) {
    if (need > cap) need = cap;
    int quiet = 0;
    while (have < need) {
        int k = recv(sock, buf + have, cap - have > 65536 ? 65536 : cap - have);
        if (k > 0) { have += k; quiet = 0; continue; }
        if (k == NET_EOF || k < 0 || ++quiet > 3) break;    /* closed, or twelve seconds of nothing */
    }
    return have;
}

/* Whether a word is on the header line that starts at s, in any case. */
static int hls_line_has(const char *s, const char *word) {
    int wl = (int)w_len(word);
    for (int i = 0; s[i] && s[i] != '\r' && s[i] != '\n'; i++) {
        int k = 0;
        while (k < wl && s[i + k] && (s[i + k] | 32) == (word[k] | 32)) k++;
        if (k == wl) return 1;
    }
    return 0;
}

static int hls_hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* GET, with a kept connection when there is one. The body into out (up to
   cap); its length, or a negative number. Once more on a fresh connection
   when a kept one turns out closed. */
static int hls_get_once(const char *url, char *out, int cap, int *status, int fresh) {
    hls_where w;
    if (!hls_split(url, &w)) return -1;
    int ci = hls_connect(&w, fresh);
    if (ci < 0) return -2;
    hls_conn *c = &hls_conns[ci];
    static char req[HLS_URI + 512];
    int n = 0;
    n = wh_add(req, sizeof(req), n, "GET ");
    if (n >= 0) n = wh_add(req, sizeof(req), n, w.path);
    if (n >= 0) n = wh_add(req, sizeof(req), n, " HTTP/1.1\r\nHost: ");
    if (n >= 0) n = wh_add(req, sizeof(req), n, w.host);
    if (n >= 0) n = wh_add(req, sizeof(req), n, "\r\nUser-Agent: " WEB_USER_AGENT "\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n");
    if (n < 0) return -1;
    if (!hls_send_all(c->sock, req, n)) { disconnect(c->sock); c->sock = 0; return -3; }

    /* The head, up to its blank line. */
    static char head[16384];
    int have = 0, end = -1;
    while (end < 0 && have < (int)sizeof(head) - 1) {
        int k = recv(c->sock, head + have, (int)sizeof(head) - 1 - have);
        if (k <= 0) { disconnect(c->sock); c->sock = 0; return -3; }
        have += k;
        for (int i = 3; i < have; i++)
            if (head[i - 3] == '\r' && head[i - 2] == '\n' && head[i - 1] == '\r' && head[i] == '\n') { end = i + 1; break; }
    }
    if (end < 0) { disconnect(c->sock); c->sock = 0; return -4; }
    head[have] = 0;
    *status = 0;
    for (int i = 9; i < 12 && head[i] >= '0' && head[i] <= '9'; i++) *status = *status * 10 + (head[i] - '0');
    long length = -1;
    int chunked = 0, closing = 0;
    for (int i = 0; i < end; i++) {
        if (i && head[i - 1] != '\n') continue;
        if (w_starts_fold(head + i, "content-length:")) {
            const char *p = head + i + 15;
            while (*p == ' ') p++;
            length = 0;
            while (*p >= '0' && *p <= '9') length = length * 10 + (*p++ - '0');
        } else if (w_starts_fold(head + i, "transfer-encoding:") && hls_line_has(head + i, "chunked")) chunked = 1;
        else if (w_starts_fold(head + i, "connection:") && w_starts_fold(head + i + 11 + (head[i + 11] == ' '), "close")) closing = 1;
    }
    /* What came with the head is the start of the body. */
    int got = 0;
    for (int i = end; i < have && got < cap; i++) out[got++] = head[i];
    if (chunked) {
        /* Chunks, each a hex size and a line, gathered into out. */
        static char raw[1 << 20];
        int rn = got, rpos = 0, body = 0;
        for (int i = 0; i < got; i++) raw[i] = out[i];
        for (;;) {
            int line = -1;
            for (;;) {
                for (int i = rpos; i + 1 < rn; i++) if (raw[i] == '\r' && raw[i + 1] == '\n') { line = i; break; }
                if (line >= 0) break;
                if (rn >= (int)sizeof(raw)) { disconnect(c->sock); c->sock = 0; return -5; }
                int before = rn;
                rn = hls_fill(c->sock, raw, rn, rn + 1, (int)sizeof(raw));
                if (rn == before) { disconnect(c->sock); c->sock = 0; return -5; }
            }
            long size = 0;
            for (int i = rpos; i < line && hls_hex(raw[i]) >= 0; i++) size = size * 16 + hls_hex(raw[i]);
            rpos = line + 2;
            if (size == 0) break;
            while (size > 0) {
                if (rpos >= rn) {
                    /* Room: what has been taken out goes. */
                    rn = 0; rpos = 0;
                    rn = hls_fill(c->sock, raw, 0, size + 2 < (long)sizeof(raw) ? (int)size + 2 : (int)sizeof(raw), (int)sizeof(raw));
                    if (rn == 0) { disconnect(c->sock); c->sock = 0; return -5; }
                }
                int take = rn - rpos < size ? rn - rpos : (int)size;
                for (int i = 0; i < take && body < cap; i++) out[body++] = raw[rpos + i];
                rpos += take;
                size -= take;
            }
            /* The CRLF after a chunk's data. */
            while (rn - rpos < 2) {
                int before = rn;
                rn = hls_fill(c->sock, raw, rn, rn + 2, (int)sizeof(raw));
                if (rn == before) break;
            }
            rpos += 2;
        }
        /* The empty line after the last chunk. */
        got = body;
    } else if (length >= 0) {
        if (length > cap) { disconnect(c->sock); c->sock = 0; return -6; }
        got = hls_fill(c->sock, out, got, (int)length, cap);
        if (got < length) { disconnect(c->sock); c->sock = 0; return -5; }
    } else {
        got = hls_fill(c->sock, out, got, cap, cap);          /* until it closes */
        closing = 1;
    }
    if (closing) { disconnect(c->sock); c->sock = 0; }
    return got;
}

static int hls_get(const char *url, char *out, int cap, int *status) {
    int got = hls_get_once(url, out, cap, status, 0);
    if (got == -3) got = hls_get_once(url, out, cap, status, 1);
    return got;
}

/* --- playlists ---------------------------------------------------------------------------- */

typedef struct { char uri[HLS_URI]; int bandwidth, width, height, fps; char name[48]; char codecs[64]; } hls_variant;
typedef struct { char uri[HLS_URI]; int ms; long long seq; } hls_segment;

/* The next line, without its end; 0 at the end of the text. */
static int hls_line(const char *s, int n, int *at, char *out, int cap) {
    if (*at >= n) return 0;
    int o = 0;
    while (*at < n && s[*at] != '\n' && s[*at] != '\r') { if (o < cap - 1) out[o++] = s[*at]; (*at)++; }
    while (*at < n && (s[*at] == '\n' || s[*at] == '\r')) (*at)++;
    out[o] = 0;
    return 1;
}

/* An attribute of a tag's list (KEY=VALUE,KEY="VALUE"), as text; 0 when absent. */
static int hls_attr(const char *line, const char *key, char *out, int cap) {
    int kl = (int)w_len(key);
    const char *p = line;
    while (*p && *p != ':') p++;
    while (*p) {
        p++;
        while (*p == ' ') p++;
        int match = 1;
        for (int i = 0; i < kl; i++) if (p[i] != key[i]) { match = 0; break; }
        const char *v = p;
        while (*v && *v != '=') v++;
        if (!*v) return 0;
        v++;
        int o = 0;
        const char *e;
        if (*v == '"') { e = v + 1; while (*e && *e != '"') e++; if (match && p[kl] == '=') { for (const char *q = v + 1; q < e && o < cap - 1; q++) out[o++] = *q; out[o] = 0; return 1; } if (*e) e++; }
        else { e = v; while (*e && *e != ',') e++; if (match && p[kl] == '=') { for (const char *q = v; q < e && o < cap - 1; q++) out[o++] = *q; out[o] = 0; return 1; } }
        p = e;
        while (*p && *p != ',') p++;
        if (!*p) return 0;
    }
    return 0;
}

static int hls_num(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

/* The renditions a master playlist names; how many. */
static int hls_master(const char *base, const char *s, int n, hls_variant *out, int max) {
    static char line[HLS_URI + 256], v[96];
    int at = 0, count = 0, pending = 0;
    while (hls_line(s, n, &at, line, sizeof(line))) {
        if (w_starts_fold(line, "#EXT-X-STREAM-INF:")) {
            if (count >= max) break;
            hls_variant *x = &out[count];
            x->bandwidth = hls_attr(line, "BANDWIDTH", v, sizeof(v)) ? hls_num(v) : 0;
            x->width = x->height = x->fps = 0;
            if (hls_attr(line, "RESOLUTION", v, sizeof(v))) {
                x->width = hls_num(v);
                const char *xx = v;
                while (*xx && *xx != 'x') xx++;
                if (*xx) x->height = hls_num(xx + 1);
            }
            if (hls_attr(line, "FRAME-RATE", v, sizeof(v))) x->fps = hls_num(v);
            x->codecs[0] = x->name[0] = 0;
            hls_attr(line, "CODECS", x->codecs, sizeof(x->codecs));
            hls_attr(line, "VIDEO", x->name, sizeof(x->name));
            pending = 1;
        } else if (pending && line[0] && line[0] != '#') {
            hls_join(base, line, out[count].uri, HLS_URI);
            count++;
            pending = 0;
        }
    }
    return count;
}

/* The segments a media playlist names, numbered; how many. *target is its
   target duration in seconds and *ended whether it says it will not grow. */
static int hls_media(const char *base, const char *s, int n, hls_segment *out, int max, int *target, int *ended) {
    static char line[HLS_URI + 256];
    int at = 0, count = 0, ms = 0;
    long long seq = 0;
    *target = 0;
    *ended = 0;
    while (hls_line(s, n, &at, line, sizeof(line))) {
        if (w_starts_fold(line, "#EXT-X-MEDIA-SEQUENCE:")) {
            seq = 0;
            for (const char *p = line + 22; *p >= '0' && *p <= '9'; p++) seq = seq * 10 + (*p - '0');
        } else if (w_starts_fold(line, "#EXT-X-TARGETDURATION:")) *target = hls_num(line + 22);
        else if (w_starts_fold(line, "#EXT-X-ENDLIST")) *ended = 1;
        else if (w_starts_fold(line, "#EXTINF:")) {
            const char *p = line + 8;
            ms = hls_num(p) * 1000;
            while (*p >= '0' && *p <= '9') p++;
            if (*p == '.') { int scale = 100; for (p++; *p >= '0' && *p <= '9' && scale; p++, scale /= 10) ms += (*p - '0') * scale; }
        } else if (line[0] && line[0] != '#') {
            if (count < max) {
                hls_join(base, line, out[count].uri, HLS_URI);
                out[count].ms = ms;
                out[count].seq = seq;
                count++;
            }
            seq++;
            ms = 0;
        }
    }
    return count;
}
