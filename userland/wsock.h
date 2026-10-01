#pragma once
/* WebSocket, the client's side of RFC 6455: a connection a page keeps open
 * to a server, over which either end sends a message whenever it has one.
 * Chat, live scores, a news site's updates and the GraphQL clients that
 * carry them all sit on it; a page that asked for one and found no
 * WebSocket at all stopped, and Al Jazeera's sat at "Loading" for ever.
 *
 * This is the protocol and nothing of the page: an upgrade asked for over
 * HTTP/1.1 (on TLS for wss://), the server's answer checked against the key
 * sent, then frames -- masked going out, as a client's must be; text,
 * binary, a message in pieces, ping answered with pong, and close answered
 * with close. jsws.h is the page's WebSocket on top of it.
 *
 * A socket here is read without waiting (sock_wait 0, syscall 68): the
 * browser asks it on each pass whether anything came, and it never holds
 * the browser up for a quiet server. Opening it does hold it up, as a
 * fetch does: the connection, the TLS handshake and the request go out in
 * the call, and the answer is read on the passes after.
 *
 * Not done: extensions (permessage-deflate is not offered, so a server
 * does not use it), and a proxy. */
#include "zelr.h"
#include "alloc.h"
#include "web.h"
#include "fetch.h"

/* --- SHA-1 (FIPS 180-4) --------------------------------------------------
 *
 * Only for the handshake: the server proves it read the key by answering
 * with the hash of it and a fixed string. Nothing secret rests on it. */
typedef struct {
    u32 h[5];
    u8  buf[64];
    u32 n;
    u64 total;
} ws_sha1;

static inline u32 ws_rol(u32 x, int k) { return (x << k) | (x >> (32 - k)); }

static void ws_sha1_block(ws_sha1 *s, const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ws_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        u32 t = ws_rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ws_rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void ws_sha1_init(ws_sha1 *s) {
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
    s->n = 0;
    s->total = 0;
}

static void ws_sha1_add(ws_sha1 *s, const void *data, u32 n) {
    const u8 *p = (const u8 *)data;
    s->total += n;
    while (n--) {
        s->buf[s->n++] = *p++;
        if (s->n == 64) { ws_sha1_block(s, s->buf); s->n = 0; }
    }
}

static void ws_sha1_done(ws_sha1 *s, u8 out[20]) {
    u64 bits = s->total * 8;
    u8 one = 0x80, zero = 0;
    ws_sha1_add(s, &one, 1);
    while (s->n != 56) ws_sha1_add(s, &zero, 1);
    u8 len[8];
    for (int i = 0; i < 8; i++) len[i] = (u8)(bits >> (56 - 8 * i));
    ws_sha1_add(s, len, 8);
    for (int i = 0; i < 20; i++) out[i] = (u8)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

/* Base64 of n bytes into out, which holds 4 * ((n + 2) / 3) + 1. */
static void ws_b64(const u8 *in, int n, char *out) {
    static const char AL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int w = 0;
    for (int i = 0; i < n; i += 3) {
        u32 v = (u32)in[i] << 16 | (i + 1 < n ? (u32)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[w++] = AL[(v >> 18) & 63];
        out[w++] = AL[(v >> 12) & 63];
        out[w++] = i + 1 < n ? AL[(v >> 6) & 63] : '=';
        out[w++] = i + 2 < n ? AL[v & 63] : '=';
    }
    out[w] = 0;
}

/* What the server must answer a key with: base64 of the SHA-1 of the key
   and the protocol's own string. */
static void ws_accept_for(const char *key, char out[29]) {
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    ws_sha1 s;
    u8 d[20];
    ws_sha1_init(&s);
    ws_sha1_add(&s, key, (u32)w_len(key));
    ws_sha1_add(&s, GUID, (u32)sizeof(GUID) - 1);
    ws_sha1_done(&s, d);
    ws_b64(d, 20, out);
}

/* --- a connection -------------------------------------------------------- */

enum { WS_CONNECTING = 0, WS_OPEN, WS_CLOSING, WS_CLOSED };
enum { WSE_NONE = 0, WSE_OPEN, WSE_MESSAGE, WSE_CLOSE, WSE_ERROR };

/* The most a message may be, kept whole until its last piece: past it the
   connection is closed with 1009, as the protocol says, rather than the
   browser's memory being given to one server. */
#define WS_MESSAGE_MAX (8u << 20)
#define WS_HEAD_MAX    16384           /* the server's answer to the upgrade */

typedef struct {
    int  sock, secure, state;
    char key[25];
    char protocol[64];                 /* what the server chose, or "" */
    u8  *in;                           /* what came and is not read yet */
    u32  in_len, in_cap;
    u8  *msg;                          /* a message being put together */
    u32  msg_len, msg_cap;
    int  msg_op;                       /* 1 text, 2 binary, 0 none started */
    int  sent_close;
    int  closing_since;                /* ticks(), when we sent close */
    int  close_code;
    char close_reason[124];
    char why[96];                      /* why it failed, for the console */
} wsock;

typedef struct {
    int type;
    int binary;
    const u8 *data;                    /* a message: until the next pump */
    u32 len;
    int code, clean;
    const char *reason;
} ws_event;

static void ws_init(wsock *w) {
    w->sock = -1;
    w->secure = 0;
    w->state = WS_CLOSED;
    w->key[0] = w->protocol[0] = w->close_reason[0] = w->why[0] = 0;
    w->in = w->msg = 0;
    w->in_len = w->in_cap = w->msg_len = w->msg_cap = 0;
    w->msg_op = 0;
    w->sent_close = 0;
    w->closing_since = 0;
    w->close_code = 1006;
}

static void ws_drop(wsock *w) {
    if (w->sock >= 0) disconnect(w->sock);
    w->sock = -1;
    w->state = WS_CLOSED;
    free(w->in);
    free(w->msg);
    w->in = w->msg = 0;
    w->in_len = w->in_cap = w->msg_len = w->msg_cap = 0;
}

/* All of it, in pieces a plain socket takes in one go. */
static int ws_send_all(wsock *w, const u8 *p, u32 n) {
    while (n) {
        int k = n > 1400 ? 1400 : (int)n;
        if (send(w->sock, p, k) != k) return 0;
        p += k;
        n -= (u32)k;
    }
    return 1;
}

static int ws_grow(u8 **buf, u32 *cap, u32 need) {
    if (need <= *cap) return 1;
    u32 c = *cap ? *cap : 4096;
    while (c < need) c *= 2;
    u8 *b = (u8 *)realloc(*buf, c);
    if (!b) return 0;
    *buf = b;
    *cap = c;
    return 1;
}

/* One frame: FIN, the opcode, the length, a fresh mask and the payload
   masked by it. A client's frames are all masked, so that what a page
   sends cannot be made to look like something else to a cache in between. */
static int ws_frame(wsock *w, int op, const u8 *data, u32 len) {
    if (w->sock < 0) return 0;
    u8 *f = (u8 *)malloc(len + 14);
    if (!f) return 0;
    u32 h = 0;
    f[h++] = (u8)(0x80 | op);
    if (len < 126) f[h++] = (u8)(0x80 | len);
    else if (len < 65536) { f[h++] = 0x80 | 126; f[h++] = (u8)(len >> 8); f[h++] = (u8)len; }
    else {
        f[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) f[h++] = i >= 4 ? 0 : (u8)(len >> (8 * i));
    }
    u8 mask[4];
    if (random_bytes(mask, 4) != 4) { free(f); return 0; }
    for (int i = 0; i < 4; i++) f[h++] = mask[i];
    for (u32 i = 0; i < len; i++) f[h + i] = data[i] ^ mask[i & 3];
    int ok = ws_send_all(w, f, h + len);
    free(f);
    return ok;
}

/* Starts one: connects (with TLS for wss://), asks for the upgrade, and
   leaves the answer to the pump. 0 and `why` when it cannot. `protocols`
   is what the page offered, comma separated, or "". */
static int ws_start(wsock *w, const char *address, const char *protocols, const char *origin,
                    const char *cookies) {
    ws_init(w);
    char http[URL_TEXT];
    const char *rest = address;
    int secure;
    if (w_starts_fold(address, "wss://")) { secure = 1; rest = address + 6; }
    else if (w_starts_fold(address, "ws://")) { secure = 0; rest = address + 5; }
    else { w_copy(w->why, sizeof(w->why), "not a ws: or wss: address", sizeof(w->why)); return 0; }
    int n = 0;
    const char *sch = secure ? "https://" : "http://";
    for (const char *p = sch; *p && n < URL_TEXT - 1; p++) http[n++] = *p;
    for (const char *p = rest; *p && n < URL_TEXT - 1; p++) http[n++] = *p;
    http[n] = 0;
    url_t u;
    if (!url_parse(http, &u)) { w_copy(w->why, sizeof(w->why), "no host in the address", sizeof(w->why)); return 0; }

    u8 raw[16];
    if (random_bytes(raw, 16) != 16) { w_copy(w->why, sizeof(w->why), "no randomness for the key", sizeof(w->why)); return 0; }
    ws_b64(raw, 16, w->key);

    int s = secure ? connect_tls(u.host, u.port) : connect(u.host, u.port);
    if (s < 0) {
        if (secure && s == NET_ERR_TLS) tls_why(w->why, sizeof(w->why));
        else w_copy(w->why, sizeof(w->why), "the server could not be reached", sizeof(w->why));
        return 0;
    }
    w->sock = s;
    w->secure = secure;

    char req[URL_PATH + URL_HOST + 4096];
    int r = 0;
    r = wh_add(req, sizeof(req), r, "GET ");
    if (r >= 0) r = wh_add(req, sizeof(req), r, u.path);
    if (r >= 0) r = wh_add(req, sizeof(req), r, " HTTP/1.1\r\nHost: ");
    if (r >= 0) r = wh_add(req, sizeof(req), r, u.host);
    if (r >= 0 && u.port != (secure ? 443 : 80)) {
        r = wh_add(req, sizeof(req), r, ":");
        if (r >= 0) r = wh_add_num(req, sizeof(req), r, u.port);
    }
    if (r >= 0) r = wh_add(req, sizeof(req), r, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
    if (r >= 0) r = wh_add(req, sizeof(req), r, w->key);
    if (r >= 0) r = wh_add(req, sizeof(req), r, "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: " WEB_USER_AGENT "\r\n");
    if (r >= 0 && origin && *origin) {
        r = wh_add(req, sizeof(req), r, "Origin: ");
        if (r >= 0) r = wh_add(req, sizeof(req), r, origin);
        if (r >= 0) r = wh_add(req, sizeof(req), r, "\r\n");
    }
    if (r >= 0 && protocols && *protocols) {
        r = wh_add(req, sizeof(req), r, "Sec-WebSocket-Protocol: ");
        if (r >= 0) r = wh_add(req, sizeof(req), r, protocols);
        if (r >= 0) r = wh_add(req, sizeof(req), r, "\r\n");
    }
    if (r >= 0 && cookies && *cookies) {
        r = wh_add(req, sizeof(req), r, "Cookie: ");
        if (r >= 0) r = wh_add(req, sizeof(req), r, cookies);
        if (r >= 0) r = wh_add(req, sizeof(req), r, "\r\n");
    }
    if (r >= 0) r = wh_add(req, sizeof(req), r, "\r\n");
    if (r < 0 || !ws_send_all(w, (const u8 *)req, (u32)r)) {
        w_copy(w->why, sizeof(w->why), "the request could not be sent", sizeof(w->why));
        ws_drop(w);
        return 0;
    }
    sock_wait(s, 0);
    w->state = WS_CONNECTING;
    return 1;
}

/* A header of the answer, by name, into out; 0 when it is not there. */
static int ws_header(const char *head, int n, const char *name, char *out, int cap) {
    int nl = (int)w_len(name);
    for (int i = 0; i < n; i++) {
        if (i && head[i - 1] != '\n') continue;
        int k = 0;
        while (k < nl && i + k < n && (head[i + k] | 32) == (name[k] | 32)) k++;
        if (k < nl || i + k >= n || head[i + k] != ':') continue;
        int p = i + k + 1, w = 0;
        while (p < n && head[p] == ' ') p++;
        while (p < n && head[p] != '\r' && head[p] != '\n' && w < cap - 1) out[w++] = head[p++];
        out[w] = 0;
        return 1;
    }
    return 0;
}

/* Whether a header's list of words has this one, as Connection: keep-alive,
   Upgrade does. */
static int ws_has_word(const char *list, const char *word) {
    int wl = (int)w_len(word);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') p++;
        int k = 0;
        while (k < wl && p[k] && (p[k] | 32) == (word[k] | 32)) k++;
        if (k == wl && (p[k] == 0 || p[k] == ',' || p[k] == ' ')) return 1;
        while (*p && *p != ',') p++;
    }
    return 0;
}

/* The connection failed: what the protocol calls failing it -- a close
   with `code` when it was open, then gone. */
static int ws_fail(wsock *w, int code, const char *why, ws_event *ev) {
    if (w->state == WS_OPEN && !w->sent_close) {
        u8 c[2] = { (u8)(code >> 8), (u8)code };
        ws_frame(w, 8, c, 2);
    }
    w_copy(w->why, sizeof(w->why), why, sizeof(w->why));
    ws_drop(w);
    w->close_code = 1006;
    ev->type = WSE_ERROR;
    return 1;
}

/* The server's answer to the upgrade, once all of it has come. */
static int ws_answer(wsock *w, ws_event *ev, const char *offered) {
    const char *h = (const char *)w->in;
    int end = -1;
    for (u32 i = 3; i < w->in_len; i++)
        if (h[i - 3] == '\r' && h[i - 2] == '\n' && h[i - 1] == '\r' && h[i] == '\n') { end = (int)i + 1; break; }
    if (end < 0) {
        if (w->in_len > WS_HEAD_MAX) return ws_fail(w, 1002, "the server's answer did not end", ev);
        return 0;
    }
    if (end < 12 || !w_starts_fold(h, "HTTP/1.1 101") || (h[12] != ' ' && h[12] != '\r'))
        return ws_fail(w, 1002, "the server did not agree to the upgrade", ev);
    char v[128], want[29];
    if (!ws_header(h, end, "Upgrade", v, sizeof(v)) || !w_same_fold(v, "websocket"))
        return ws_fail(w, 1002, "the answer did not say Upgrade: websocket", ev);
    if (!ws_header(h, end, "Connection", v, sizeof(v)) || !ws_has_word(v, "upgrade"))
        return ws_fail(w, 1002, "the answer did not say Connection: Upgrade", ev);
    ws_accept_for(w->key, want);
    if (!ws_header(h, end, "Sec-WebSocket-Accept", v, sizeof(v)) || !w_same(v, want))
        return ws_fail(w, 1002, "the server's Sec-WebSocket-Accept was not for our key", ev);
    if (ws_header(h, end, "Sec-WebSocket-Extensions", v, sizeof(v)) && v[0])
        return ws_fail(w, 1002, "the server used an extension that was not offered", ev);
    w->protocol[0] = 0;
    if (ws_header(h, end, "Sec-WebSocket-Protocol", v, sizeof(v)) && v[0]) {
        if (!offered || !ws_has_word(offered, v))
            return ws_fail(w, 1002, "the server chose a subprotocol that was not offered", ev);
        w_copy(w->protocol, sizeof(w->protocol), v, sizeof(w->protocol));
    }
    /* Frames may have come in the same reads as the answer. */
    u32 left = w->in_len - (u32)end;
    for (u32 i = 0; i < left; i++) w->in[i] = w->in[end + i];
    w->in_len = left;
    w->state = WS_OPEN;
    ev->type = WSE_OPEN;
    return 1;
}

/* Valid UTF-8, which a text message must be. */
static int ws_utf8_ok(const u8 *p, u32 n) {
    u32 i = 0;
    while (i < n) {
        u8 c = p[i];
        int k;
        u32 lo = 0x80, hi = 0xBF;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) k = 1;
        else if (c >= 0xE0 && c <= 0xEF) { k = 2; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
        else if (c >= 0xF0 && c <= 0xF4) { k = 3; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
        else return 0;
        if (i + (u32)k >= n) return 0;
        for (int q = 1; q <= k; q++) {
            u8 d = p[i + q];
            if (d < lo || d > hi) return 0;
            lo = 0x80; hi = 0xBF;
        }
        i += (u32)k + 1;
    }
    return 1;
}

/* What has happened, one thing a call: WSE_NONE when nothing more has.
   `offered` is the subprotocols the page asked for. */
static int ws_pump(wsock *w, ws_event *ev, const char *offered) {
    ev->type = WSE_NONE;
    ev->binary = 0;
    ev->data = 0;
    ev->len = 0;
    ev->code = 0;
    ev->clean = 0;
    ev->reason = "";
    if (w->state == WS_CLOSED || w->sock < 0) return 0;

    /* A close we sent and nobody answered, after five seconds: gone. */
    if (w->state == WS_CLOSING && ticks() - w->closing_since > 500) {
        ws_drop(w);
        ev->type = WSE_CLOSE;
        ev->code = w->close_code = 1006;
        ev->reason = "";
        return 1;
    }

    /* What has come, without waiting for more. */
    for (int rounds = 0; rounds < 64; rounds++) {
        if (!ws_grow(&w->in, &w->in_cap, w->in_len + 16384)) return ws_fail(w, 1009, "out of memory", ev);
        int k = recv(w->sock, w->in + w->in_len, 16384);
        if (k > 0) { w->in_len += (u32)k; continue; }
        if (k == NET_EOF && w->in_len == 0) {
            int was_open = w->state != WS_CONNECTING;
            ws_drop(w);
            if (!was_open) { w_copy(w->why, sizeof(w->why), "the server closed before answering", sizeof(w->why)); ev->type = WSE_ERROR; return 1; }
            ev->type = WSE_CLOSE;
            ev->code = w->close_code = 1006;
            return 1;
        }
        break;
    }

    if (w->state == WS_CONNECTING) return ws_answer(w, ev, offered);

    /* One frame, if all of it is here. */
    for (;;) {
        if (w->in_len < 2) return 0;
        u8 b0 = w->in[0], b1 = w->in[1];
        int fin = b0 & 0x80, op = b0 & 0x0F;
        if (b0 & 0x70) return ws_fail(w, 1002, "a frame used bits no extension was agreed for", ev);
        if (b1 & 0x80) return ws_fail(w, 1002, "the server masked a frame", ev);
        u64 len = b1 & 0x7F;
        u32 at = 2;
        if (len == 126) {
            if (w->in_len < 4) return 0;
            len = (u64)w->in[2] << 8 | w->in[3];
            at = 4;
        } else if (len == 127) {
            if (w->in_len < 10) return 0;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | w->in[2 + i];
            at = 10;
        }
        if (op >= 8 && (!fin || len > 125)) return ws_fail(w, 1002, "a control frame that was too long or in pieces", ev);
        if (len > WS_MESSAGE_MAX) return ws_fail(w, 1009, "a message too big to keep", ev);
        if (w->in_len < at + (u32)len) return 0;
        const u8 *pay = w->in + at;
        u32 plen = (u32)len, used = at + plen;

        if (op == 9) {                                  /* ping: the same back */
            ws_frame(w, 10, pay, plen);
        } else if (op == 8) {                           /* close */
            int code = 1005;
            char reason[124];
            reason[0] = 0;
            if (plen == 1) return ws_fail(w, 1002, "a close with half a code", ev);
            if (plen >= 2) {
                code = pay[0] << 8 | pay[1];
                u32 rl = plen - 2 < sizeof(reason) - 1 ? plen - 2 : (u32)sizeof(reason) - 1;
                for (u32 i = 0; i < rl; i++) reason[i] = (char)pay[2 + i];
                reason[rl] = 0;
            }
            if (!w->sent_close) {
                u8 c[2] = { (u8)(code >> 8), (u8)code };
                ws_frame(w, 8, c, plen >= 2 ? 2 : 0);
                w->sent_close = 1;
            }
            w_copy(w->close_reason, sizeof(w->close_reason), reason, sizeof(w->close_reason));
            w->close_code = code;
            ws_drop(w);
            ev->type = WSE_CLOSE;
            ev->code = code;
            ev->clean = 1;
            ev->reason = w->close_reason;
            return 1;
        } else if (op == 10) {                          /* pong: nothing to do */
        } else if (op == 0 || op == 1 || op == 2) {
            if (op == 0 && !w->msg_op) return ws_fail(w, 1002, "a continuation with nothing to continue", ev);
            if (op != 0 && w->msg_op) return ws_fail(w, 1002, "a new message before the last one ended", ev);
            if (op) { w->msg_op = op; w->msg_len = 0; }
            if (w->msg_len + plen > WS_MESSAGE_MAX) return ws_fail(w, 1009, "a message too big to keep", ev);
            if (!ws_grow(&w->msg, &w->msg_cap, w->msg_len + plen + 1)) return ws_fail(w, 1009, "out of memory", ev);
            for (u32 i = 0; i < plen; i++) w->msg[w->msg_len + i] = pay[i];
            w->msg_len += plen;
            if (fin) {
                int binary = w->msg_op == 2;
                w->msg_op = 0;
                if (!binary && !ws_utf8_ok(w->msg, w->msg_len)) return ws_fail(w, 1007, "a text message that was not UTF-8", ev);
                w->msg[w->msg_len] = 0;
                for (u32 i = used; i < w->in_len; i++) w->in[i - used] = w->in[i];
                w->in_len -= used;
                ev->type = WSE_MESSAGE;
                ev->binary = binary;
                ev->data = w->msg;
                ev->len = w->msg_len;
                return 1;
            }
        } else {
            return ws_fail(w, 1002, "a frame of a kind there is none of", ev);
        }
        for (u32 i = used; i < w->in_len; i++) w->in[i - used] = w->in[i];
        w->in_len -= used;
    }
}

/* A message: text (UTF-8) or binary. */
static int ws_send(wsock *w, int binary, const u8 *data, u32 len) {
    if (w->state != WS_OPEN) return 0;
    return ws_frame(w, binary ? 2 : 1, data, len);
}

/* Asks to close: the server's close in answer ends it (ws_pump). */
static void ws_close(wsock *w, int code, const char *reason) {
    if (w->state == WS_CONNECTING) { ws_drop(w); return; }
    if (w->state != WS_OPEN) return;
    u8 c[2 + 123];
    u32 n = 0;
    if (code) {
        c[n++] = (u8)(code >> 8);
        c[n++] = (u8)code;
        for (const char *p = reason; p && *p && n < sizeof(c); p++) c[n++] = (u8)*p;
    }
    ws_frame(w, 8, c, n);
    w->sent_close = 1;
    w->close_code = code ? code : 1005;
    w->state = WS_CLOSING;
    w->closing_since = ticks();
}
