/* URLs, and getting what is at one.
 *
 * Separate from the browser's window so that the part which talks to a
 * server can be reasoned about on its own: everything here is bytes in and
 * bytes out, with no screen anywhere in it.
 */
#pragma once
#include "zelr.h"

#define URL_HOST 128
#define URL_PATH 640
#define URL_TEXT (URL_HOST + URL_PATH + 24)

typedef struct {
    char host[URL_HOST];
    char path[URL_PATH];
    int  port;
    int  secure;              /* https, so the connection is a TLS one */
} url_t;

/* --- small string helpers -------------------------------------------------
 *
 * Written out rather than pulled in, because the ones in the system header
 * are for paths and filenames and these are for a protocol: what matters
 * here is that they never write past the end of a fixed buffer and never
 * leave a string unterminated, and both of those are easier to see in four
 * lines than to look up. */

static inline int w_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static inline void w_copy(char *dst, int cap, const char *src, int n) {
    int i = 0;
    for (; i < n && i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static inline int w_same(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static inline char w_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case insensitive, because a header name is and a scheme is. */
static inline int w_same_fold(const char *a, const char *b) {
    while (*a && *b && w_lower(*a) == w_lower(*b)) { a++; b++; }
    return *a == *b;
}

static inline int w_starts_fold(const char *s, const char *prefix) {
    for (int i = 0; prefix[i]; i++)
        if (w_lower(s[i]) != w_lower(prefix[i])) return 0;
    return 1;
}

/* A url copied a field at a time.
 *
 * A struct assignment of this size becomes a call to memcpy, and there is no
 * library here to have one in: the inline one in the system header is not a
 * symbol the linker can reach. Every copy of one of these goes through here
 * for that reason and no other. */
static inline void url_copy(url_t *dst, const url_t *src) {
    for (int i = 0; i < URL_HOST; i++) dst->host[i] = src->host[i];
    for (int i = 0; i < URL_PATH; i++) dst->path[i] = src->path[i];
    dst->port = src->port;
    dst->secure = src->secure;
}

/* --- urls ---------------------------------------------------------------- */

/* Puts a url back together as text, which is what goes in the address bar
   and in the history. The port is left off when it is the one the scheme
   implies, because a bar that says :80 after every address is noise. */
static inline void url_text(const url_t *u, char *out, int cap) {
    int n = 0;
    const char *scheme = u->secure ? "https://" : "http://";
    for (const char *p = scheme; *p && n < cap - 1; p++) out[n++] = *p;
    for (const char *p = u->host; *p && n < cap - 1; p++) out[n++] = *p;
    if (u->port != (u->secure ? 443 : 80) && n < cap - 8) {
        out[n++] = ':';
        char d[8];
        int k = 0, v = u->port;
        if (!v) d[k++] = '0';
        while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
        while (k && n < cap - 1) out[n++] = d[--k];
    }
    for (const char *p = u->path; *p && n < cap - 1; p++) out[n++] = *p;
    out[n] = 0;
}

/* Takes an address the way a person types one. A missing path is the root,
   and anything after a # is for finding a place on the page rather than for
   the server, so it never goes in the request.
 *
   A bare name is https. It used to be http, for the honest reason that http
   was the only thing here that worked. Now that both do, guessing the
   unencrypted one would mean a typed address went out in the clear and got
   redirected, and the first request, the one carrying the address, would
   already have been readable by then. A site that only does http is still
   reachable by saying so. */
static inline int url_parse(const char *in, url_t *out) {
    while (*in == ' ') in++;

    out->secure = 1;
    if (w_starts_fold(in, "https://")) in += 8;
    else if (w_starts_fold(in, "http://")) { out->secure = 0; in += 7; }
    else if (w_starts_fold(in, "//")) in += 2;

    int n = 0;
    while (*in && *in != '/' && *in != '?' && *in != '#') {
        if (n < URL_HOST - 1) out->host[n++] = *in;
        in++;
    }
    out->host[n] = 0;
    if (!out->host[0]) return 0;

    /* A port on the end of the name. Split here rather than in the caller,
       because every user of a host name wants it without one. */
    out->port = out->secure ? 443 : 80;
    for (int i = 0; out->host[i]; i++) {
        if (out->host[i] != ':') continue;
        int v = 0;
        for (int j = i + 1; out->host[j]; j++) {
            if (out->host[j] < '0' || out->host[j] > '9') { v = 0; break; }
            v = v * 10 + (out->host[j] - '0');
        }
        if (v > 0 && v < 65536) out->port = v;
        out->host[i] = 0;
        break;
    }

    n = 0;
    if (*in != '/' && *in != '?') out->path[n++] = '/';
    while (*in && *in != '#') {
        if (n < URL_PATH - 1) out->path[n++] = *in;
        in++;
    }
    if (!n) out->path[n++] = '/';
    out->path[n] = 0;
    return 1;
}

/* Where a link on a page points.
 *
 * Most links on most pages are relative, so this is the routine that decides
 * whether a browser can follow anything at all. The cases are the ones the
 * web actually uses: a whole address, a scheme relative //host/path, a root
 * relative /path, a fragment that stays where it is, and a plain name that
 * replaces the last piece of the current path. */
static inline int url_join(const url_t *base, const char *href, url_t *out) {
    while (*href == ' ') href++;
    if (!*href || *href == '#') { url_copy(out, base); return 1; }

    if (w_starts_fold(href, "http://") || w_starts_fold(href, "https://"))
        return url_parse(href, out);

    /* A scheme relative link keeps the scheme of the page it is on, which is
       the entire reason for writing one. Parsing it on its own would apply
       the rule for something a person typed and could quietly take an
       encrypted page's links down to http. */
    if (w_starts_fold(href, "//")) {
        int stated = 0;
        for (const char *p = href + 2; *p && *p != '/' && *p != '?'; p++)
            if (*p == ':') { stated = 1; break; }
        if (!url_parse(href, out)) return 0;
        out->secure = base->secure;
        /* A port written in the link is the port. Only the one url_parse
           guessed from a scheme that was not there gets corrected. */
        if (!stated) out->port = out->secure ? 443 : 80;
        return 1;
    }

    /* Anything with a scheme this cannot speak is not a page to go to. */
    for (int i = 0; href[i] && href[i] != '/' && i < 12; i++) {
        if (href[i] != ':') continue;
        return 0;
    }

    url_copy(out, base);

    if (href[0] == '/') {
        int n = 0;
        while (href[n] && href[n] != '#' && n < URL_PATH - 1) {
            out->path[n] = href[n];
            n++;
        }
        out->path[n] = 0;
        return 1;
    }

    /* Relative to the directory the current page is in, which is everything
       up to and including its last slash. */
    int cut = 0;
    for (int i = 0; out->path[i]; i++)
        if (out->path[i] == '/') cut = i + 1;

    char joined[URL_PATH];
    int n = 0;
    for (int i = 0; i < cut && n < URL_PATH - 1; i++) joined[n++] = out->path[i];
    for (int i = 0; href[i] && href[i] != '#' && n < URL_PATH - 1; i++)
        joined[n++] = href[i];
    joined[n] = 0;

    /* Fold away . and .. so that a link three directories up does not end up
       as a path with ../../.. written in it, which no server will match. */
    char clean[URL_PATH];
    int c = 0;
    int i = 0;
    while (joined[i]) {
        if (joined[i] == '/') {
            if (joined[i + 1] == '.' && joined[i + 2] == '/') { i += 2; continue; }
            if (joined[i + 1] == '.' && joined[i + 2] == '.'
                && (joined[i + 3] == '/' || !joined[i + 3])) {
                while (c > 0 && clean[c - 1] != '/') c--;
                if (c > 0) c--;                 /* and the slash before it */
                i += 3;
                if (!joined[i]) { clean[c++] = '/'; break; }
                continue;
            }
        }
        if (c < URL_PATH - 1) clean[c++] = joined[i];
        i++;
    }
    clean[c] = 0;
    w_copy(out->path, URL_PATH, clean[0] ? clean : "/", URL_PATH);
    return 1;
}
