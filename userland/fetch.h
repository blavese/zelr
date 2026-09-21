/* Asking a web server for a page, and knowing when it has finished.
 *
 * The part of this that takes work is not the request. It is the three
 * different ways a server can say where the body stops, and a client that
 * only knows one of them loses the end of about half the pages on the web
 * without ever reporting a problem:
 *
 *   a content length, and the body is exactly that many bytes
 *   chunked, and the body arrives in pieces each headed by its own size
 *   neither, and the body ends when the connection does
 *
 * All three are here. Which one is in use is decided by the headers and not
 * by whether the bytes happen to look like one.
 */
#pragma once
#include "zelr.h"
#include "alloc.h"
#include "inflate.h"
#include "web.h"

#define WEB_ERR_SCHEME   -1     /* a scheme that is not http or https */
#define WEB_ERR_CONNECT  -2
#define WEB_ERR_SEND     -3
#define WEB_ERR_EMPTY    -4
#define WEB_ERR_HEADERS  -5     /* an answer with no blank line in it */
#define WEB_ERR_TLS      -6     /* the connection would not prove who it was */
#define WEB_ERR_DOWN     -7     /* no card, or no address on it */
#define WEB_ERR_RESOLVE  -8     /* the name did not turn into an address */
#define WEB_ERR_BUSY     -9     /* the one connection is already in use */
#define WEB_ERR_ENCODING -10    /* compressed in a way this cannot undo */

/* The kernel answers with a reason; this is the same reason in this file's
   numbering. Collapsing them all to "could not connect" is what made a
   machine with no address report a refused certificate. */
static inline int web_err_from(int rc) {
    switch (rc) {
        case NET_ERR_DOWN:    return WEB_ERR_DOWN;
        case NET_ERR_RESOLVE: return WEB_ERR_RESOLVE;
        case NET_ERR_TLS:     return WEB_ERR_TLS;
        case NET_ERR_BUSY:    return WEB_ERR_BUSY;
        default:              return WEB_ERR_CONNECT;
    }
}

typedef struct {
    int   status;
    char *body;
    int   len;
    int   truncated;               /* the page is bigger than the buffer */
    char  location[URL_TEXT];      /* where a redirect points */
    char  ctype[64];
    int   secure;                  /* it was encrypted, and to whom it said */
    char  how[64];                 /* what was agreed, or why it was refused */
} response_t;

/* --- building the request ------------------------------------------------ */

static inline int wh_add(char *buf, int cap, int n, const char *s) {
    for (; *s; s++) {
        if (n >= cap - 1) return -1;
        buf[n++] = *s;
    }
    buf[n] = 0;
    return n;
}

static inline int wh_add_num(char *buf, int cap, int n, int v) {
    char d[12];
    int k = 0;
    if (!v) d[k++] = '0';
    while (v > 0) { d[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) {
        if (n >= cap - 1) return -1;
        buf[n++] = d[--k];
    }
    buf[n] = 0;
    return n;
}

/* --- reading the headers ------------------------------------------------- */

/* The value of one header, or an empty string. Folded, because a server may
   send Content-Length or content-length and both mean the same thing. */
static inline int wh_header(const char *head, int hlen, const char *name,
                            char *out, int cap) {
    out[0] = 0;
    int nlen = w_len(name);
    for (int i = 0; i + nlen + 1 < hlen; i++) {
        if (i && head[i - 1] != '\n') continue;      /* only at a line start */
        int ok = 1;
        for (int j = 0; j < nlen; j++)
            if (w_lower(head[i + j]) != w_lower(name[j])) { ok = 0; break; }
        if (!ok || head[i + nlen] != ':') continue;

        int at = i + nlen + 1;
        while (at < hlen && (head[at] == ' ' || head[at] == '\t')) at++;
        int n = 0;
        while (at < hlen && head[at] != '\r' && head[at] != '\n') {
            if (n < cap - 1) out[n++] = head[at];
            at++;
        }
        out[n] = 0;
        return 1;
    }
    return 0;
}

/* The nth header of a name, because Set-Cookie is the one header a server
   sends several of and taking the first would lose every session that needs
   two. */
static inline int wh_header_nth(const char *head, int hlen, const char *name,
                                char *out, int cap, int nth) {
    out[0] = 0;
    int nlen = w_len(name);
    int seen = 0;
    for (int i = 0; i + nlen + 1 < hlen; i++) {
        if (i && head[i - 1] != '\n') continue;
        int ok = 1;
        for (int j = 0; j < nlen; j++)
            if (w_lower(head[i + j]) != w_lower(name[j])) { ok = 0; break; }
        if (!ok || head[i + nlen] != ':') continue;
        if (seen++ != nth) continue;

        int at = i + nlen + 1;
        while (at < hlen && (head[at] == ' ' || head[at] == '\t')) at++;
        int n = 0;
        while (at < hlen && head[at] != '\r' && head[at] != '\n') {
            if (n < cap - 1) out[n++] = head[at];
            at++;
        }
        out[n] = 0;
        return 1;
    }
    return 0;
}

/* --- gzip -----------------------------------------------------------------
 *
 * A server allowed to compress will, and everything this asks for used to
 * say identity because there was nothing here to undo it. There is: the
 * deflate written for PNG is the same deflate, with a different wrapper in
 * front of it -- ten bytes, some optional strings, and eight bytes of
 * checksum behind that nothing here reads, because what a checksum would
 * catch the transport has already checked and a page that decodes is a
 * page.
 *
 * Three to five times less to move over one connection at a time, which on
 * this machine is the difference between a page arriving and a page
 * arriving eventually.
 */
static inline int wh_gunzip(char *body, int len, int cap) {
    if (len < 12) return -1;
    const u8 *p = (const u8 *)body;
    if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 8) return -1;

    int flg = p[3];
    int at = 10;
    if (flg & 4) {                                   /* extra */
        if (at + 2 > len) return -1;
        at += 2 + (p[at] | (p[at + 1] << 8));
    }
    if (flg & 8)  { while (at < len && p[at]) at++; at++; }    /* a name */
    if (flg & 16) { while (at < len && p[at]) at++; at++; }    /* a comment */
    if (flg & 2)  at += 2;                                     /* a head sum */
    if (at >= len) return -1;

    u8 *out = (u8 *)malloc((u64)cap);
    if (!out) return -1;

    int got = inflate_raw(p + at, len - at, out, cap);
    if (got < 0) { free(out); return -1; }
    for (int i = 0; i < got; i++) body[i] = (char)out[i];
    free(out);
    return got;
}

/* --- cookies --------------------------------------------------------------
 *
 * Without these a session does not survive a click: a site sets one when
 * you sign in, sends you to the next page, and that page has never heard of
 * you. So there is a jar.
 *
 * They live in memory and go when the browser does. That is a decision and
 * not half a job: a cookie written to disk is a thing somebody has to be
 * able to find and delete, and this machine has nowhere to say so yet.
 * Expires and Max-Age are read only far enough to notice a server deleting
 * one, which is how a sign-out works.
 */
#define CK_MAX     64
#define CK_NAME    96
#define CK_VALUE   768
#define CK_DOMAIN  URL_HOST
#define CK_PATH    160

typedef struct {
    char name[CK_NAME];
    char value[CK_VALUE];
    char domain[CK_DOMAIN];
    char path[CK_PATH];
    int  secure;
    int  used;
} cookie_t;

static cookie_t ck_jar[CK_MAX];

static inline void ck_forget_all(void) {
    for (int i = 0; i < CK_MAX; i++) ck_jar[i].used = 0;
}

/* host ends with domain, on a label boundary. "example.com" is a cookie for
   "www.example.com" and is emphatically not one for "notexample.com". */
static inline int ck_domain_ok(const char *host, const char *domain) {
    if (!domain[0]) return 0;
    int h = w_len(host), d = w_len(domain);
    if (d > h) return 0;
    for (int i = 0; i < d; i++)
        if (w_lower(host[h - d + i]) != w_lower(domain[i])) return 0;
    return d == h || host[h - d - 1] == '.';
}

static inline int ck_path_ok(const char *path, const char *cpath) {
    if (!cpath[0] || (cpath[0] == '/' && !cpath[1])) return 1;
    int c = w_len(cpath);
    for (int i = 0; i < c; i++)
        if (path[i] != cpath[i]) return 0;
    return path[c] == 0 || path[c] == '/' || cpath[c - 1] == '/';
}

static inline int ck_same(const cookie_t *k, const char *name,
                          const char *domain, const char *path) {
    return w_same(k->name, name) && w_same(k->domain, domain)
        && w_same(k->path, path);
}

/* One Set-Cookie line. */
static inline void ck_take_one(const url_t *u, const char *line) {
    char name[CK_NAME], value[CK_VALUE];
    char domain[CK_DOMAIN], path[CK_PATH];
    int secure = 0, drop = 0;

    int i = 0;
    int n = 0;
    while (line[i] && line[i] != '=' && line[i] != ';') {
        if (n < CK_NAME - 1) name[n++] = line[i];
        i++;
    }
    name[n] = 0;
    while (n > 0 && name[n - 1] == ' ') name[--n] = 0;
    if (!name[0] || line[i] != '=') return;
    i++;

    n = 0;
    while (line[i] && line[i] != ';') {
        if (n < CK_VALUE - 1) value[n++] = line[i];
        i++;
    }
    value[n] = 0;
    while (n > 0 && value[n - 1] == ' ') value[--n] = 0;

    w_copy(domain, sizeof(domain), u->host, sizeof(domain));
    w_copy(path, sizeof(path), "/", sizeof(path));

    while (line[i] == ';') {
        i++;
        while (line[i] == ' ') i++;

        char key[32];
        n = 0;
        while (line[i] && line[i] != '=' && line[i] != ';') {
            if (n < (int)sizeof(key) - 1) key[n++] = line[i];
            i++;
        }
        key[n] = 0;

        char val[160];
        n = 0;
        if (line[i] == '=') {
            i++;
            while (line[i] && line[i] != ';') {
                if (n < (int)sizeof(val) - 1) val[n++] = line[i];
                i++;
            }
        }
        val[n] = 0;

        if (w_same_fold(key, "secure")) secure = 1;
        else if (w_same_fold(key, "domain")) {
            const char *d = val[0] == '.' ? val + 1 : val;
            /* A server may only widen a cookie to a domain it is inside. */
            if (ck_domain_ok(u->host, d))
                w_copy(domain, sizeof(domain), d, sizeof(domain));
        } else if (w_same_fold(key, "path")) {
            if (val[0] == '/') w_copy(path, sizeof(path), val, sizeof(path));
        } else if (w_same_fold(key, "max-age")) {
            if (val[0] == '0' || val[0] == '-') drop = 1;
        } else if (w_same_fold(key, "expires")) {
            /* Only far enough to see a server deleting one, which is what a
               sign-out is. Anything in nineteen-seventy is in the past. */
            for (int q = 0; val[q] && val[q + 3]; q++)
                if (val[q] == '1' && val[q + 1] == '9' && val[q + 2] == '7'
                    && val[q + 3] == '0') { drop = 1; break; }
        }
    }

    int free_slot = -1;
    for (int k = 0; k < CK_MAX; k++) {
        if (!ck_jar[k].used) { if (free_slot < 0) free_slot = k; continue; }
        if (ck_same(&ck_jar[k], name, domain, path)) {
            if (drop) ck_jar[k].used = 0;
            else w_copy(ck_jar[k].value, CK_VALUE, value, CK_VALUE);
            return;
        }
    }
    if (drop || free_slot < 0) return;

    cookie_t *k = &ck_jar[free_slot];
    w_copy(k->name, CK_NAME, name, CK_NAME);
    w_copy(k->value, CK_VALUE, value, CK_VALUE);
    w_copy(k->domain, CK_DOMAIN, domain, CK_DOMAIN);
    w_copy(k->path, CK_PATH, path, CK_PATH);
    k->secure = secure;
    k->used = 1;
}

static inline void ck_take(const url_t *u, const char *head, int hlen) {
    char line[CK_VALUE + 256];
    for (int nth = 0; nth < 16; nth++) {
        if (!wh_header_nth(head, hlen, "set-cookie", line, sizeof(line), nth))
            break;
        ck_take_one(u, line);
    }
}

/* What to send with this request, as "a=1; b=2", or nothing. */
static inline int ck_header(const url_t *u, char *out, int cap) {
    int w = 0;
    out[0] = 0;
    for (int i = 0; i < CK_MAX; i++) {
        cookie_t *k = &ck_jar[i];
        if (!k->used) continue;
        if (k->secure && !u->secure) continue;
        if (!ck_domain_ok(u->host, k->domain)) continue;
        if (!ck_path_ok(u->path, k->path)) continue;

        int need = w_len(k->name) + w_len(k->value) + 4;
        if (w + need >= cap) break;
        if (w) { out[w++] = ';'; out[w++] = ' '; }
        for (const char *p = k->name; *p; p++) out[w++] = *p;
        out[w++] = '=';
        for (const char *p = k->value; *p; p++) out[w++] = *p;
    }
    out[w] = 0;
    return w;
}

static inline int wh_number(const char *s) {
    int v = 0, any = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = 1; }
    return any ? v : -1;
}

static inline int wh_hex(const char *s, int len, int *used) {
    int v = 0, i = 0;
    for (; i < len; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16 + d;
    }
    *used = i;
    return i ? v : -1;
}

/* Where the headers end, or -1. A server is entitled to use bare newlines,
   and one that does is not broken enough to refuse to talk to. */
static inline int wh_split(const char *buf, int len, int *skip) {
    for (int i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') { *skip = 2; return i; }
        if (i + 3 < len && buf[i] == '\r' && buf[i + 1] == '\n'
            && buf[i + 2] == '\r' && buf[i + 3] == '\n') { *skip = 4; return i; }
    }
    return -1;
}

/* Turns a chunked body into a plain one, in the buffer it is already in.
   Every byte written is behind the byte being read, so this cannot run over
   itself. Returns the length, or -1 if the pieces do not add up. */
static inline int wh_dechunk(char *body, int len) {
    int r = 0, w = 0;
    for (;;) {
        int used = 0;
        int size = wh_hex(body + r, len - r, &used);
        if (size < 0) return w ? w : -1;
        r += used;
        /* Anything after the size on that line is an extension, and the line
           ends at the newline whatever is in it. */
        while (r < len && body[r] != '\n') r++;
        r++;
        if (size == 0) return w;
        if (r + size > len) {
            /* The last piece is short: keep what there is rather than
               throwing away a page that is all there but the tail. */
            size = len - r;
            if (size <= 0) return w;
            for (int i = 0; i < size; i++) body[w + i] = body[r + i];
            return w + size;
        }
        for (int i = 0; i < size; i++) body[w + i] = body[r + i];
        w += size;
        r += size;
        while (r < len && (body[r] == '\r' || body[r] == '\n')) r++;
    }
}

/* --- the fetch ------------------------------------------------------------
 *
 * Everything the server says goes into `buf`, and the body is a pointer into
 * it once the headers have been measured off the front. One buffer rather
 * than two because a program here has no allocator, and a second buffer of
 * the same size would be most of what a machine with 64 MiB has spare. */
/* --- keeping the connection ----------------------------------------------
 *
 * Every request used to open a connection, ask, and close it. A page with a
 * dozen pictures on it therefore paid a dozen handshakes, and over https a
 * dozen of the expensive kind; on a machine whose stack holds one
 * connection at a time that is the largest single cost of showing a page.
 *
 * So the connection is kept when the answer said how long it was -- by a
 * length or by chunks -- and the server did not ask for it to be closed.
 * When the answer did not say, the only thing that marks its end is the
 * close, so there is nothing to keep.
 *
 * A kept connection can be closed at the other end at any moment and
 * without warning, which is not a fault but the ordinary way of things. A
 * request that fails on one is therefore tried once more on a new one --
 * but only when nothing came back at all, because a request the server
 * answered and then dropped may have been acted on, and asking again is how
 * somebody orders twice.
 */
static char ka_host[URL_HOST];
static int  ka_port, ka_secure, ka_live;

/* Which socket the kept connection is. It used to be unnecessary: there was
   one socket on the machine and every call meant that one. A number has to
   be carried now, and this file still keeps only one at a time -- what the
   several are for is the browser asking for a page and its pictures at
   once, which is a layer above this one. */
static int  ka_sock = -1;

static inline void web_drop(void) {
    if (ka_live) { disconnect(ka_sock); ka_live = 0; }
    ka_sock = -1;
}

static inline int ka_matches(const url_t *u) {
    return ka_live && u->port == ka_port && u->secure == ka_secure
        && w_same_fold(u->host, ka_host);
}

/* One request. A body means POST: the same head with a method, a length
   and a type on it, and the bytes after the blank line. Nothing else about
   the exchange differs, which is why it is one function and not two. */
static inline int web_fetch_once(const url_t *u, const char *body,
                                 char *buf, int cap, response_t *r) {
    r->status = 0;
    r->body = buf;
    r->len = 0;
    r->truncated = 0;
    r->location[0] = 0;
    r->ctype[0] = 0;
    r->secure = 0;
    r->how[0] = 0;

    if (ka_matches(u)) {
        /* Already there. A TLS connection kept is a handshake not done. */
        r->secure = u->secure;
        if (u->secure) tls_what(r->how, sizeof(r->how));
    } else if (u->secure) {
        web_drop();
        /* The handshake checks the certificate against u->host, so reaching
           the next line means the bytes after it are going to the site that
           was asked for and not merely to whatever answered. */
        int rc = connect_tls(u->host, u->port);
        if (rc < 0) {
            /* Only a handshake that was actually reached has a reason worth
               reading. Asking TLS why a machine with no address failed gets
               "no error", which is true and useless. */
            if (rc == NET_ERR_TLS) tls_why(r->how, sizeof(r->how));
            return web_err_from(rc);
        }
        ka_sock = rc;
        r->secure = 1;
        tls_what(r->how, sizeof(r->how));
    } else {
        web_drop();
        int rc = connect(u->host, u->port);
        if (rc < 0) return web_err_from(rc);
        ka_sock = rc;
    }

    char req[URL_PATH + URL_HOST + CK_VALUE + 512];
    int n = 0;
    n = wh_add(req, sizeof(req), n, body ? "POST " : "GET ");
    if (n >= 0) n = wh_add(req, sizeof(req), n, u->path);
    if (n >= 0) n = wh_add(req, sizeof(req), n, " HTTP/1.1\r\nHost: ");
    if (n >= 0) n = wh_add(req, sizeof(req), n, u->host);
    /* The port belongs in Host only when it is not the one the scheme
       implies. Sending "Host: www.google.com:443" is legal and a number of
       servers answer it with a redirect to themselves, forever. */
    if (n >= 0 && u->port != (u->secure ? 443 : 80)) {
        n = wh_add(req, sizeof(req), n, ":");
        if (n >= 0) n = wh_add_num(req, sizeof(req), n, u->port);
    }
    /* identity, because there is no decompressor here and a server that is
       allowed to gzip will. Close, because this makes one request per
       connection and a server holding the socket open afterwards is a wait
       for nothing. */
    if (n >= 0) n = wh_add(req, sizeof(req), n,
                           "\r\nUser-Agent: zelr\r\n"
                           "Accept: text/html,text/plain,*/*\r\n"
                           "Accept-Encoding: gzip\r\n"
                           "Connection: keep-alive\r\n");

    /* Whatever this site has already said to remember about itself. */
    char cookies[CK_VALUE];
    if (ck_header(u, cookies, sizeof(cookies))) {
        if (n >= 0) n = wh_add(req, sizeof(req), n, "Cookie: ");
        if (n >= 0) n = wh_add(req, sizeof(req), n, cookies);
        if (n >= 0) n = wh_add(req, sizeof(req), n, "\r\n");
    }
    /* A server is entitled to read exactly this many bytes and not one
       more, so the length has to be the body's and not the buffer's. */
    if (body) {
        if (n >= 0) n = wh_add(req, sizeof(req), n,
                               "Content-Type: application/x-www-form-"
                               "urlencoded\r\nContent-Length: ");
        if (n >= 0) n = wh_add_num(req, sizeof(req), n, w_len(body));
        if (n >= 0) n = wh_add(req, sizeof(req), n, "\r\n");
    }
    if (n >= 0) n = wh_add(req, sizeof(req), n, "\r\n");
    if (n < 0) { web_drop(); return WEB_ERR_SEND; }

    /* The socket takes 1400 bytes at a time, and a long path can be more
       than that. */
    int sent = 0;
    while (sent < n) {
        int piece = n - sent;
        if (piece > 1400) piece = 1400;
        if (send(ka_sock, req + sent, piece) < 0) { web_drop(); return WEB_ERR_SEND; }
        sent += piece;
    }

    /* And the body after the head, in the same sized pieces. Not part of
       req: a form can be longer than the buffer a request line fits in. */
    if (body) {
        int blen = w_len(body), bs = 0;
        while (bs < blen) {
            int piece = blen - bs;
            if (piece > 1400) piece = 1400;
            if (send(ka_sock, body + bs, piece) < 0) {
                web_drop();
                return WEB_ERR_SEND;
            }
            bs += piece;
        }
    }

    int total = 0, quiet = 0;
    int hlen = -1, skip = 0, want = -1, chunked = 0;

    for (;;) {
        int room = cap - 1 - total;
        if (room <= 0) { r->truncated = 1; break; }
        int piece = room > 32768 ? 32768 : room;

        int got = recv(ka_sock, buf + total, piece);
        if (got == NET_EOF) break;
        if (got < 0) break;
        if (got == 0) {
            /* Nothing for a while. Three of those in a row is a server that
               has stopped talking without saying so, which is different from
               one that is merely slow: the first two are waited through. */
            if (++quiet >= 3) break;
            continue;
        }
        quiet = 0;
        total += got;
        buf[total] = 0;

        if (hlen < 0) {
            hlen = wh_split(buf, total, &skip);
            if (hlen >= 0) {
                char v[64];
                if (wh_header(buf, hlen, "transfer-encoding", v, sizeof(v)))
                    chunked = w_starts_fold(v, "chunked");
                if (!chunked && wh_header(buf, hlen, "content-length", v, sizeof(v)))
                    want = wh_number(v);
            }
        }

        /* Stop as soon as the answer is complete rather than waiting for the
           connection to close. A server that ignores Connection: close would
           otherwise hold this here for the length of three timeouts on every
           single page. */
        if (hlen >= 0 && want >= 0 && total - hlen - skip >= want) break;
        if (hlen >= 0 && chunked) {
            /* The end of a chunked body is a zero sized piece. */
            int at = total - 5;
            if (at < hlen) at = hlen;
            for (int i = at; i + 4 < total + 1 && i + 4 <= total; i++) {
                if (buf[i] == '0' && buf[i + 1] == '\r' && buf[i + 2] == '\n'
                    && buf[i + 3] == '\r' && buf[i + 4] == '\n') {
                    quiet = 99;
                    break;
                }
            }
            if (quiet == 99) break;
        }
    }

    /* Kept only when the answer said how long it was. Where it did not,
       the close is the only thing that marks the end and there is nothing
       to keep. */
    int keep = 0;
    if (hlen >= 0 && (want >= 0 || chunked)) {
        char conn[32];
        if (!wh_header(buf, hlen, "connection", conn, sizeof(conn))
            || !w_starts_fold(conn, "close"))
            keep = 1;
    }
    if (keep) {
        ka_live = 1;
        ka_port = u->port;
        ka_secure = u->secure;
        w_copy(ka_host, sizeof(ka_host), u->host, sizeof(ka_host));
    } else {
        disconnect(ka_sock);
        ka_live = 0;
        ka_sock = -1;
    }

    buf[total < cap ? total : cap - 1] = 0;

    if (total == 0) return WEB_ERR_EMPTY;
    if (hlen < 0) hlen = wh_split(buf, total, &skip);
    if (hlen < 0) return WEB_ERR_HEADERS;

    if (total > 12 && buf[0] == 'H') {
        int s = 0;
        for (int i = 9; i < 12; i++) {
            if (buf[i] < '0' || buf[i] > '9') { s = 0; break; }
            s = s * 10 + (buf[i] - '0');
        }
        r->status = s;
    }

    wh_header(buf, hlen, "location", r->location, sizeof(r->location));
    wh_header(buf, hlen, "content-type", r->ctype, sizeof(r->ctype));
    ck_take(u, buf, hlen);

    r->body = buf + hlen + skip;
    r->len = total - hlen - skip;
    if (r->len < 0) r->len = 0;

    if (chunked) {
        int d = wh_dechunk(r->body, r->len);
        r->len = d > 0 ? d : 0;
    } else if (want >= 0 && want < r->len) {
        r->len = want;                       /* ignore anything after it */
    }
    r->body[r->len] = 0;

    /* Undone after the chunks, because the chunking is how it travelled and
       the compression is what it is. */
    char enc[32];
    if (wh_header(buf, hlen, "content-encoding", enc, sizeof(enc))
        && w_starts_fold(enc, "gzip")) {
        int room = cap - (int)(r->body - buf) - 1;
        int got = wh_gunzip(r->body, r->len, room);
        if (got < 0) {
            web_drop();
            return WEB_ERR_ENCODING;
        }
        r->len = got;
        r->body[r->len] = 0;
    }
    return r->status ? r->status : WEB_ERR_EMPTY;
}

/* And once more on a fresh connection when a kept one had been closed at
   the far end. Only when nothing came back: a request the server answered
   and then dropped may already have been acted on. */
static inline int web_fetch(const url_t *u, const char *body,
                            char *buf, int cap, response_t *r) {
    int reused = ka_matches(u);
    int rc = web_fetch_once(u, body, buf, cap, r);
    if (rc >= 0 || !reused) return rc;
    if (rc != WEB_ERR_SEND && rc != WEB_ERR_EMPTY && rc != WEB_ERR_CONNECT)
        return rc;
    web_drop();
    return web_fetch_once(u, body, buf, cap, r);
}

/* Follows redirects, because a bare fetch lands on "301 moved" for a great
   many perfectly ordinary addresses. Bounded, and a loop is reported rather
   than followed until the machine gives up. */
#define WEB_MAX_HOPS 6

static inline int web_send(url_t *u, const char *body, char *buf, int cap,
                           response_t *r) {
    for (int hop = 0; hop < WEB_MAX_HOPS; hop++) {
        int rc = web_fetch(u, body, buf, cap, r);
        if (rc < 0) return rc;
        if (rc != 301 && rc != 302 && rc != 303 && rc != 307 && rc != 308)
            return rc;
        if (!r->location[0]) return rc;

        url_t next;
        if (!url_join(u, r->location, &next)) return rc;
        /* Pointing at itself. The scheme is part of that: http to https on
           the same host and path is the single most common redirect there
           is, and treating it as a loop would refuse every site that does
           the right thing. */
        if (w_same(next.host, u->host) && w_same(next.path, u->path)
            && next.port == u->port && next.secure == u->secure)
            return rc;
        url_copy(u, &next);

        /* What a redirect after a form means. 303 says plainly to ask again
           with GET, and 301 and 302 after a POST are treated the same way
           because that is what every browser settled on and what every
           server now expects; 307 and 308 were invented to say keep the
           method, so they do. Sending the form again to wherever it was
           sent is how somebody orders twice. */
        if (body && (rc == 301 || rc == 302 || rc == 303)) body = 0;
    }
    return r->status;
}

static inline int web_get(url_t *u, char *buf, int cap, response_t *r) {
    return web_send(u, 0, buf, cap, r);
}

static inline int web_post(url_t *u, const char *body, char *buf, int cap,
                           response_t *r) {
    return web_send(u, body ? body : "", buf, cap, r);
}
