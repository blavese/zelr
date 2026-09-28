#pragma once
#include "zelr.h"
#include "web.h"
#include "fetch.h"

/* Sites whose pages are applications, read another way.
 *
 * YouTube and Twitch do not send pages. They send a few kilobytes of markup
 * and megabytes of script, and the script builds the page -- out of custom
 * elements, shadow roots and a player that plays H.264 through Media Source
 * Extensions. A browser that cannot run that application sees an empty page,
 * which is what this one saw.
 *
 * But what the page would show is there to be read. YouTube puts it in the
 * page as data -- ytInitialData, a megabyte and a half of JSON with every
 * result in it, and ytInitialPlayerResponse for a video -- and Twitch answers
 * it from an API anybody may ask. So for these two the browser reads that and
 * draws a plain page of its own from it: the results, the titles, who made
 * them, how many watched, the pictures, and links that come back here. It is
 * said on the page that this is what happened.
 *
 * What it cannot do is play the video. That is H.264 inside MP4 or MPEG
 * transport streams, delivered in pieces chosen by the site's own script, and
 * there is no video decoder on this machine to hand it to. The page says that
 * too, rather than showing a player that does nothing.
 *
 * Google is not here, because there is nothing to read: its results page is
 * an anti-automation program whose output is the page, and a browser that
 * cannot run it is told that its browser is not supported (browser.c,
 * SEARCH_PREFIX). */

/* --- a little JSON, read where it lies -------------------------------------
 *
 * The data is a megabyte and more, and all that is wanted is a few fields of
 * a few dozen objects in it, so nothing is parsed into a tree: a value is
 * skipped by matching its brackets, and a key is found by looking for it,
 * quoted and followed by a colon, inside the value that holds it. A quote
 * inside a string is always written escaped, so an unescaped one is always
 * the start or the end of a string, and a string followed by a colon is a
 * key. */

/* Just past the value that starts at `at`: a string, an object or an array
   (with the strings inside skipped, escapes and all), or a bare word. */
static inline int sj_skip(const char *s, int at, int n) {
    if (at < 0 || at >= n) return n;
    char c = s[at];
    if (c == '"') {
        for (int i = at + 1; i < n; i++) {
            if (s[i] == '\\') { i++; continue; }
            if (s[i] == '"') return i + 1;
        }
        return n;
    }
    if (c == '{' || c == '[') {
        int depth = 0;
        for (int i = at; i < n; i++) {
            char d = s[i];
            if (d == '"') {
                for (i++; i < n; i++) {
                    if (s[i] == '\\') { i++; continue; }
                    if (s[i] == '"') break;
                }
                continue;
            }
            if (d == '{' || d == '[') depth++;
            else if ((d == '}' || d == ']') && --depth == 0) return i + 1;
        }
        return n;
    }
    int i = at;
    while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
    return i;
}

/* Where the value of "key" starts, anywhere inside from..to, or -1. */
static inline int sj_find(const char *s, int from, int to, const char *key) {
    int kl = 0;
    while (key[kl]) kl++;
    for (int i = from; i + kl + 2 < to; i++) {
        if (s[i] != '"') continue;
        int bs = 0;
        for (int j = i - 1; j >= from && s[j] == '\\'; j--) bs++;
        if (bs & 1) continue;
        int k = 0;
        while (k < kl && s[i + 1 + k] == key[k]) k++;
        if (k < kl || s[i + 1 + kl] != '"') continue;
        int v = i + kl + 2;
        while (v < to && s[v] == ' ') v++;
        if (v >= to || s[v] != ':') continue;
        v++;
        while (v < to && s[v] == ' ') v++;
        return v < to ? v : -1;
    }
    return -1;
}

/* A code point as UTF-8. */
static inline int sj_utf8(u32 cp, char *out, int room) {
    if (cp < 0x80) { if (room < 1) return 0; out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        if (room < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 63));
        return 2;
    }
    if (cp < 0x10000) {
        if (room < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    if (room < 4) return 0;
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static inline int sj_hex4(const char *s, int at, int n) {
    if (at + 4 > n) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[at + i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

/* The string at `at`, its escapes undone, as UTF-8 into out; its length.
   A number or a word is copied as it is written, which is how a count that
   one site sends quoted and another does not comes out the same. */
static inline int sj_str(const char *s, int at, int n, char *out, int cap) {
    int o = 0;
    if (cap <= 0) return 0;
    out[0] = 0;
    if (at < 0 || at >= n) return 0;
    if (s[at] != '"') {
        int e = sj_skip(s, at, n);
        for (int i = at; i < e && o < cap - 1; i++) out[o++] = s[i];
        out[o] = 0;
        return o;
    }
    for (int i = at + 1; i < n && o < cap - 1; i++) {
        char c = s[i];
        if (c == '"') break;
        if (c != '\\') { out[o++] = c; continue; }
        if (++i >= n) break;
        char e = s[i];
        u32 cp;
        switch (e) {
        case 'n': cp = '\n'; break;
        case 't': cp = '\t'; break;
        case 'r': cp = '\r'; break;
        case 'b': cp = 8; break;
        case 'f': cp = 12; break;
        case 'u': {
            int h = sj_hex4(s, i + 1, n);
            if (h < 0) { cp = '?'; break; }
            i += 4;
            cp = (u32)h;
            /* A character past the first sixty five thousand comes as two. */
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < n && s[i + 1] == '\\' && s[i + 2] == 'u') {
                int lo = sj_hex4(s, i + 3, n);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (u32)(lo - 0xDC00);
                    i += 6;
                }
            }
            break;
        }
        default: cp = (u8)e;                  /* a quote, a backslash, a slash */
        }
        o += sj_utf8(cp, out + o, cap - 1 - o);
    }
    out[o] = 0;
    return o;
}

/* What YouTube writes as text: a string, {"simpleText": ...}, {"content":
   ...}, or {"runs": [{"text": ...}, ...]} with the pieces run together. A
   number or a word comes out as it is written. */
static inline int sj_text(const char *s, int at, int n, char *out, int cap) {
    if (cap <= 0) return 0;
    out[0] = 0;
    if (at < 0 || at >= n) return 0;
    if (s[at] != '{') return sj_str(s, at, n, out, cap);
    int end = sj_skip(s, at, n);
    int v = sj_find(s, at, end, "simpleText");
    if (v >= 0) return sj_str(s, v, n, out, cap);
    v = sj_find(s, at, end, "runs");
    if (v >= 0 && s[v] == '[') {
        int re = sj_skip(s, v, n), o = 0;
        for (int p = v; p < re && o < cap - 1;) {
            int t = sj_find(s, p, re, "text");
            if (t < 0) break;
            o += sj_str(s, t, n, out + o, cap - o);
            p = sj_skip(s, t, n);
        }
        return o;
    }
    v = sj_find(s, at, end, "content");
    if (v >= 0) return sj_str(s, v, n, out, cap);
    return 0;
}

/* The field's text inside the object that starts at `obj`. */
static inline int sj_field(const char *s, int obj, int n, const char *key, char *out, int cap) {
    if (cap > 0) out[0] = 0;
    int end = sj_skip(s, obj, n);
    int v = sj_find(s, obj, end, key);
    return v < 0 ? 0 : sj_text(s, v, n, out, cap);
}

/* Where a piece of text first appears, from `from`, or -1. */
static inline int site_search(const char *s, int from, int n, const char *what) {
    int wl = 0;
    while (what[wl]) wl++;
    for (int i = from; i + wl <= n; i++) {
        int k = 0;
        while (k < wl && s[i + k] == what[k]) k++;
        if (k == wl) return i;
    }
    return -1;
}

/* --- writing the page -------------------------------------------------------- */

typedef struct {
    char *out;
    int n, cap;
} site_page;

static inline void sp_raw(site_page *p, const char *t) {
    while (*t && p->n < p->cap - 1) p->out[p->n++] = *t++;
    p->out[p->n] = 0;
}

/* Text, with what markup would read as markup written so that it does not. */
static inline void sp_text(site_page *p, const char *t) {
    for (; *t && p->n < p->cap - 8; t++) {
        char c = *t;
        if (c == '&') sp_raw(p, "&amp;");
        else if (c == '<') sp_raw(p, "&lt;");
        else if (c == '>') sp_raw(p, "&gt;");
        else if (c == '"') sp_raw(p, "&quot;");
        else if (c == '\n') sp_raw(p, "<br>");
        else if (c != '\r') { p->out[p->n++] = c; p->out[p->n] = 0; }
    }
}

static inline void sp_num(site_page *p, long long v) {
    char b[24];
    int i = 0;
    if (v < 0) { sp_raw(p, "-"); v = -v; }
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 22);
    char r[24];
    int k = 0;
    /* With commas, the way a count is read. */
    for (int j = i - 1; j >= 0; j--) {
        r[k++] = b[j];
        if (j && j % 3 == 0) r[k++] = ',';
    }
    r[k] = 0;
    sp_raw(p, r);
}

/* A number as its digits and nothing else, for an address. */
static inline void sp_digits(site_page *p, long long v) {
    char b[24];
    int i = 0;
    if (v < 0) v = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 22);
    char r[2] = { 0, 0 };
    while (i) { r[0] = b[--i]; sp_raw(p, r); }
}

/* Only the characters an identifier from these sites is made of, so that one
   written into a link or a query cannot close it and say something else. */
static inline int site_ident(const char *in, char *out, int cap, const char *also) {
    int o = 0;
    for (; *in && o < cap - 1; in++) {
        char c = *in;
        int fine = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        for (const char *a = also; *a && !fine; a++) if (c == *a) fine = 1;
        if (!fine) break;
        out[o++] = c;
    }
    out[o] = 0;
    return o;
}

static inline int site_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* %xx undone, and + as a space when it is a query's. */
static inline int site_unescape(const char *in, int n, char *out, int cap, int plus) {
    int o = 0;
    for (int i = 0; i < n && in[i] && o < cap - 1; i++) {
        if (plus && in[i] == '+') { out[o++] = ' '; continue; }
        if (in[i] == '%' && i + 2 < n && site_hexval(in[i + 1]) >= 0 && site_hexval(in[i + 2]) >= 0) {
            out[o++] = (char)(site_hexval(in[i + 1]) * 16 + site_hexval(in[i + 2]));
            i += 2;
            continue;
        }
        out[o++] = in[i];
    }
    out[o] = 0;
    return o;
}

/* A query string's value for a name, decoded (%xx and +). */
static inline int site_param(const char *path, const char *name, char *out, int cap) {
    out[0] = 0;
    const char *q = path;
    while (*q && *q != '?') q++;
    if (!*q) return 0;
    q++;
    int nl = 0;
    while (name[nl]) nl++;
    while (*q) {
        int k = 0;
        while (k < nl && q[k] == name[k]) k++;
        if (k == nl && q[k] == '=') {
            q += nl + 1;
            int len = 0;
            while (q[len] && q[len] != '&') len++;
            return site_unescape(q, len, out, cap, 1);
        }
        while (*q && *q != '&') q++;
        if (*q == '&') q++;
    }
    return 0;
}

static inline int site_host_is(const url_t *u, const char *const *hosts) {
    for (int i = 0; hosts[i]; i++)
        if (w_same_fold(u->host, hosts[i])) return 1;
    return 0;
}

/* A row: a picture on the left and words beside it. A flex row, because
   this layout lays tables out as blocks, one cell under the next. The words
   ask for half the width and grow into the rest, so a long title does not
   squeeze the picture: a flex row shrinks what is in it in proportion to
   what each asked for, and words ask for the whole line. */
static inline void sp_row_open(site_page *p, int pic_w) {
    sp_raw(p, "<div class=\"row\" style=\"display:flex;margin:10px 0\"><div style=\"width:");
    sp_num(p, pic_w + 12);
    sp_raw(p, "px\">");
}

static inline void sp_row_words(site_page *p) {
    sp_raw(p, "</div><div style=\"width:50%;flex:1\">");
}

static inline void sp_row_close(site_page *p) {
    sp_raw(p, "</div></div>\n");
}

/* --- Google ------------------------------------------------------------------
 *
 * Its home page is a page, with a form on it, and the form asks for
 * /search?q=. What comes back from there to a browser that does not run
 * Google's script is "please click here if you are not redirected": the
 * results are made by a program that decides first whether a person is
 * asking, and that program is not something to get round. So a search
 * asked of Google is asked of DuckDuckGo instead, which answers with a page,
 * and the page says that it was (browser.c). The words that were typed are
 * all that goes. */

static inline int site_is_google(const url_t *u) {
    const char *h = u->host;
    if (w_starts_fold(h, "www.")) h += 4;
    /* google.com, google.co.uk, google.de: the name, then one or two short
       parts that say which country -- and not google.example.org, which is
       somebody else's. */
    if (!w_starts_fold(h, "google.")) return 0;
    int parts = 0, run = 0;
    for (const char *t = h + 7;; t++) {
        if (*t == '.' || !*t) {
            if (run < 2 || run > 3) return 0;
            parts++;
            run = 0;
            if (!*t) break;
            continue;
        }
        if (!((*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z'))) return 0;
        run++;
    }
    return parts <= 2;
}

/* The words of a Google search, or 0 when the address is not one. */
static inline int site_google_search(const url_t *u, char *q, int cap) {
    if (cap > 0) q[0] = 0;
    if (!site_is_google(u)) return 0;
    if (!w_starts_fold(u->path, "/search?") && !w_same(u->path, "/search")) return 0;
    return site_param(u->path, "q", q, cap);
}

/* --- YouTube ----------------------------------------------------------------- */

static inline int site_is_youtube(const url_t *u) {
    static const char *const H[] = { "www.youtube.com", "youtube.com", "m.youtube.com", 0 };
    return site_host_is(u, H);
}

#define YT_FIELD 512

/* Where a channel's page is, from the object that names it: the address
   YouTube gives it, or failing that /channel/ and its id. Only the characters
   such an address is made of, all of them or none, since it is written into
   a link. */
static inline int yt_channel_href(const char *s, int at, int n, char *out, int cap) {
    static char raw[256];
    out[0] = 0;
    if (at < 0 || at >= n || s[at] != '{') return 0;
    int end = sj_skip(s, at, n);
    int v = sj_find(s, at, end, "canonicalBaseUrl");
    if (v >= 0) {
        int rl = sj_str(s, v, n, raw, sizeof(raw));
        if (raw[0] == '/' && site_ident(raw + 1, out + 1, cap - 1, "/@_.-%") == rl - 1) {
            out[0] = '/';
            return rl;
        }
    }
    v = sj_find(s, at, end, "browseId");
    if (v < 0) return 0;
    char id[64];
    int il = sj_str(s, v, n, raw, sizeof(raw));
    if (site_ident(raw, id, sizeof(id), "_-") != il || id[0] != 'U' || id[1] != 'C') return 0;
    int o = 0;
    for (const char *q = "/channel/"; *q && o < cap - 1; q++) out[o++] = *q;
    for (const char *q = id; *q && o < cap - 1; q++) out[o++] = *q;
    out[o] = 0;
    return o;
}

/* Who, linked to their channel when there is a channel to link to. */
static inline void yt_who(site_page *p, const char *who, const char *href) {
    if (!href[0]) { sp_text(p, who); return; }
    sp_raw(p, "<a href=\"");
    sp_raw(p, href);
    sp_raw(p, "\">");
    sp_text(p, who);
    sp_raw(p, "</a>");
}

/* One video as a row: its picture, which links to it, its title, who made it
   and what the site says about it. */
static inline void yt_row(site_page *p, const char *href, const char *pic, const char *title,
                          const char *who, const char *who_href, const char *a, const char *b,
                          const char *c, const char *snippet) {
    sp_row_open(p, 240);
    sp_raw(p, "<a href=\"");
    sp_raw(p, href);
    sp_raw(p, "\">");
    if (pic[0]) {
        sp_raw(p, "<img src=\"https://i.ytimg.com/vi/");
        sp_raw(p, pic);
        sp_raw(p, "/mqdefault.jpg\" width=\"240\" height=\"135\" alt=\"\">");
    }
    sp_raw(p, "</a>");
    sp_row_words(p);
    sp_raw(p, "<a href=\"");
    sp_raw(p, href);
    sp_raw(p, "\"><b>");
    sp_text(p, title[0] ? title : href);
    sp_raw(p, "</b></a><br><small>");
    yt_who(p, who, who_href);
    const char *bits[3] = { a, b, c };
    int first = 1;
    for (int i = 0; i < 3; i++) {
        if (!bits[i][0]) continue;
        sp_raw(p, first ? "<br>" : " &middot; ");
        sp_text(p, bits[i]);
        first = 0;
    }
    if (snippet && snippet[0]) {
        sp_raw(p, "<br>");
        sp_text(p, snippet);
    }
    sp_raw(p, "</small>");
    sp_row_close(p);
}

/* Every video the data lists, in the order it lists them, once each: the
   renderer a search or a channel uses, the one the side of a watch page used
   to, and the view model that has replaced them in places. */
static inline int yt_videos(site_page *p, const char *s, int n, int limit) {
    static char seen[64][16];
    int nseen = 0, shown = 0;
    static char id[64], title[YT_FIELD], who[YT_FIELD], views[128], age[128], len[64], snip[YT_FIELD];
    static char href[256];

    const char *const kinds[] = { "\"videoRenderer\":{", "\"compactVideoRenderer\":{",
                                  "\"gridVideoRenderer\":{", "\"lockupViewModel\":{",
                                  "\"shortsLockupViewModel\":{", 0 };
    static char list[64], link[96], pic[16];
    for (int kind = 0; kinds[kind] && shown < limit; kind++) {
        for (int at = site_search(s, 0, n, kinds[kind]); at >= 0 && shown < limit;
             at = site_search(s, at + 1, n, kinds[kind])) {
            int obj = at;
            while (obj < n && s[obj] != '{') obj++;
            int end = sj_skip(s, obj, n);
            id[0] = title[0] = who[0] = views[0] = age[0] = len[0] = snip[0] = href[0] = 0;

            list[0] = pic[0] = 0;
            if (kind == 4) {
                /* A Short: its id in the command that opens it, and what it
                   is and how many watched in the words read out for it,
                   "Title, 1.3 million views - play Short". Linked to the
                   ordinary watch page, which this reads; the Shorts page is
                   another application. */
                int re = sj_find(s, obj, end, "reelWatchEndpoint");
                if (re < 0) continue;
                sj_str(s, sj_find(s, re, sj_skip(s, re, n), "videoId"), n, id, sizeof(id));
                sj_field(s, obj, n, "accessibilityText", title, sizeof(title));
                int tl = w_len(title);
                const char *tail = " play Short";
                int kl = w_len(tail);
                if (tl > kl && w_same(title + tl - kl, tail)) {
                    title[tl - kl] = 0;
                    tl -= kl;
                    /* and the dash before it, an en dash or a hyphen, and
                       the spaces round it -- no more, so a title ending in
                       an accented letter keeps it */
                    while (tl > 0 && title[tl - 1] == ' ') title[--tl] = 0;
                    if (tl >= 3 && (u8)title[tl - 3] == 0xE2 && (u8)title[tl - 2] == 0x80
                        && (u8)title[tl - 1] == 0x93) { tl -= 3; title[tl] = 0; }
                    else if (tl > 0 && title[tl - 1] == '-') title[--tl] = 0;
                    while (tl > 0 && title[tl - 1] == ' ') title[--tl] = 0;
                }
                w_copy(age, sizeof(age), "Short", sizeof(age));
            } else if (kind == 3) {
                /* The view model says what it holds: a video, or a playlist,
                   which is linked to its own page with the picture of the
                   first video in it. */
                int ct = sj_find(s, obj, end, "contentType");
                char type[64];
                sj_str(s, ct, n, type, sizeof(type));
                if (ct < 0) continue;
                if (w_same(type, "LOCKUP_CONTENT_TYPE_PLAYLIST")) {
                    char raw[64];
                    sj_str(s, sj_find(s, obj, end, "contentId"), n, raw, sizeof(raw));
                    if (!site_ident(raw, list, sizeof(list), "-_") || w_len(list) != w_len(raw)) continue;
                    int at_pic = site_search(s, obj, end, "https://i.ytimg.com/vi/");
                    if (at_pic >= 0) site_ident(s + at_pic + 23, pic, sizeof(pic), "-_");
                    if (w_len(pic) != 11) pic[0] = 0;
                } else if (!w_same(type, "LOCKUP_CONTENT_TYPE_VIDEO")) {
                    continue;
                }
                if (!list[0]) sj_str(s, sj_find(s, obj, end, "contentId"), n, id, sizeof(id));
                int meta = sj_find(s, obj, end, "lockupMetadataViewModel");
                if (meta >= 0) {
                    int me = sj_skip(s, meta, n);
                    int t = sj_find(s, meta, me, "title");
                    if (t >= 0) sj_text(s, t, n, title, sizeof(title));
                    /* The rows under it: who, then how many and how long ago. */
                    int rows = sj_find(s, meta, me, "metadataRows");
                    if (rows >= 0) {
                        int re = sj_skip(s, rows, n), part = 0;
                        for (int q = rows; q < re && part < 3;) {
                            int c = sj_find(s, q, re, "content");
                            if (c < 0) break;
                            char *dst = part == 0 ? who : part == 1 ? views : age;
                            sj_str(s, c, n, dst, part == 0 ? (int)sizeof(who) : 128);
                            part++;
                            q = sj_skip(s, c, n);
                        }
                    }
                }
            } else {
                sj_str(s, sj_find(s, obj, end, "videoId"), n, id, sizeof(id));
                sj_field(s, obj, n, "title", title, sizeof(title));
                int by = sj_find(s, obj, end, "longBylineText");
                if (by < 0) by = sj_find(s, obj, end, "ownerText");
                if (by < 0) by = sj_find(s, obj, end, "shortBylineText");
                if (by >= 0) {
                    sj_text(s, by, n, who, sizeof(who));
                    yt_channel_href(s, by, n, href, sizeof(href));
                }
                if (!sj_field(s, obj, n, "viewCountText", views, sizeof(views)))
                    sj_field(s, obj, n, "shortViewCountText", views, sizeof(views));
                sj_field(s, obj, n, "publishedTimeText", age, sizeof(age));
                sj_field(s, obj, n, "lengthText", len, sizeof(len));
                int sn = sj_find(s, obj, end, "snippetText");
                if (sn >= 0) sj_text(s, sn, n, snip, sizeof(snip));
            }

            char clean[16];
            if (list[0]) {
                /* A playlist: its own address, and the first video's picture. */
                int o = 0;
                for (const char *q = "/playlist?list="; *q; q++) link[o++] = *q;
                for (const char *q = list; *q && o < (int)sizeof(link) - 1; q++) link[o++] = *q;
                link[o] = 0;
                if (!views[0]) w_copy(views, sizeof(views), "playlist", sizeof(views));
                yt_row(p, link, pic, title, who, href, views, age, len, snip);
                shown++;
                continue;
            }
            if (!site_ident(id, clean, sizeof(clean), "-_") || w_len(clean) != 11) continue;
            int dup = 0;
            for (int k = 0; k < nseen; k++) if (w_same(seen[k], clean)) dup = 1;
            if (dup) continue;
            if (nseen < 64) w_copy(seen[nseen++], 16, clean, 16);

            int o = 0;
            for (const char *q = "/watch?v="; *q; q++) link[o++] = *q;
            for (const char *q = clean; *q && o < (int)sizeof(link) - 1; q++) link[o++] = *q;
            link[o] = 0;
            yt_row(p, link, clean, title, who, href, views, age, len, snip);
            shown++;
        }
    }
    return shown;
}

/* A number of seconds as hours, minutes and seconds. */
static inline void yt_duration(site_page *p, long long secs) {
    long long h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
    if (h) { sp_num(p, h); sp_raw(p, ":"); }
    if (h && m < 10) sp_raw(p, "0");
    sp_num(p, m);
    sp_raw(p, ":");
    if (s < 10) sp_raw(p, "0");
    sp_num(p, s);
}

static inline long long site_atoll(const char *t) {
    long long v = 0;
    while (*t >= '0' && *t <= '9') v = v * 10 + (*t++ - '0');
    return v;
}

/* Frames from the video, which is as near to watching it as this comes.
 *
 * YouTube keeps storyboards for scrubbing along a video: sheets of small
 * frames taken at a fixed interval all the way through, as ordinary JPEGs
 * any page may fetch. The spec is a template address and a level per size:
 *
 *   https://i.ytimg.com/sb/ID/storyboard3_L$L/$N.jpg?sqp=...|w#h#count#cols#rows#ms#name#sig|...
 *
 * A sheet is the template with $L the level's number and $N its name with $M
 * the sheet's number in it, and &sigh= and the signature after it. The level
 * used is the one with the largest frames no wider than 160, and three
 * sheets at most are shown -- the first, the middle and the last -- each
 * captioned with the stretch of the video it covers. */
static inline void yt_frames(site_page *p, const char *s, int n) {
    static char spec[2048], base[1024], url[1400], f[8][96];
    int at = site_search(s, 0, n, "\"playerStoryboardSpecRenderer\":{");
    if (at < 0) return;
    at = sj_find(s, at, n, "playerStoryboardSpecRenderer");
    int sl = sj_field(s, at, n, "spec", spec, sizeof(spec));
    if (sl <= 0) return;

    /* The template, and the level to use. */
    int i = 0, bl = 0;
    while (i < sl && spec[i] != '|' && bl < (int)sizeof(base) - 1) base[bl++] = spec[i++];
    base[bl] = 0;
    if (!w_starts_fold(base, "https://i.ytimg.com/sb/")) return;
    int best = -1, best_w = 0, count = 0, cols = 0, rows = 0, ms = 0;
    static char name[96], sig[96];
    for (int level = 0; i < sl && spec[i] == '|'; level++) {
        i++;
        int k = 0, o = 0;
        for (int j = 0; j < 8; j++) f[j][0] = 0;
        while (i < sl && spec[i] != '|' && k < 8) {
            if (spec[i] == '#') { f[k][o] = 0; k++; o = 0; i++; continue; }
            if (o < 95) f[k][o++] = spec[i];
            i++;
        }
        if (k < 8) f[k][o] = 0;
        int w = (int)site_atoll(f[0]), iv = (int)site_atoll(f[5]);
        if (k < 7 || iv <= 0 || w <= 0 || w > 160 || w <= best_w) continue;
        best = level;
        best_w = w;
        count = (int)site_atoll(f[2]);
        cols = (int)site_atoll(f[3]);
        rows = (int)site_atoll(f[4]);
        ms = iv;
        w_copy(name, sizeof(name), f[6], sizeof(name));
        w_copy(sig, sizeof(sig), f[7], sizeof(sig));
    }
    if (best < 0 || count <= 0 || cols <= 0 || rows <= 0) return;
    int per = cols * rows, sheets = (count + per - 1) / per;
    int pick[3] = { 0, sheets / 2, sheets - 1 };

    sp_raw(p, "<h2>frames from the video</h2>\n<p><small>One every ");
    sp_num(p, ms / 1000);
    sp_raw(p, ms / 1000 == 1 ? " second" : " seconds");
    sp_raw(p, ", from YouTube's storyboards.</small></p>\n");
    for (int k = 0; k < 3; k++) {
        if (k && pick[k] == pick[k - 1]) continue;
        int sheet = pick[k];
        /* The address: the template with the level and the sheet put in. */
        int o = 0;
        char num[16];
        site_page np = { num, 0, (int)sizeof(num) };
        num[0] = 0;
        sp_digits(&np, sheet);
        for (const char *t = base; *t && o < (int)sizeof(url) - 1; t++) {
            if (t[0] == '$' && t[1] == 'L') {
                url[o++] = (char)('0' + best % 10);
                t++;
            } else if (t[0] == '$' && t[1] == 'N') {
                for (const char *m = name; *m && o < (int)sizeof(url) - 1; m++) {
                    if (m[0] == '$' && m[1] == 'M') {
                        for (const char *d = num; *d && o < (int)sizeof(url) - 1; d++) url[o++] = *d;
                        m++;
                    } else {
                        url[o++] = *m;
                    }
                }
                t++;
            } else {
                url[o++] = *t;
            }
        }
        for (const char *t = "&sigh="; *t && o < (int)sizeof(url) - 1; t++) url[o++] = *t;
        for (const char *t = sig; *t && o < (int)sizeof(url) - 1; t++) url[o++] = *t;
        url[o] = 0;

        int first = sheet * per, last = first + per - 1;
        if (last > count - 1) last = count - 1;
        sp_raw(p, "<p><img src=\"");
        sp_text(p, url);
        sp_raw(p, "\" alt=\"\"><br><small>");
        yt_duration(p, (long long)first * ms / 1000);
        sp_raw(p, " to ");
        yt_duration(p, (long long)last * ms / 1000);
        sp_raw(p, "</small></p>\n");
    }
}

/* The page, from YouTube's own. 0 when there is nothing in it to read. */
static inline int site_youtube(const url_t *u, const char *s, int n, char *out, int cap) {
    if (!site_is_youtube(u) || n <= 0) return 0;
    if (site_search(s, 0, n, "ytInitialData") < 0
        && site_search(s, 0, n, "ytInitialPlayerResponse") < 0) return 0;

    site_page p = { out, 0, cap };
    static char q[256], title[YT_FIELD], who[YT_FIELD], text[8192], num[64];
    site_param(u->path, "search_query", q, sizeof(q));

    int watch = w_starts_fold(u->path, "/watch");
    int vd = watch ? site_search(s, 0, n, "\"videoDetails\":{") : -1;
    if (vd >= 0) vd = sj_find(s, vd, n, "videoDetails");
    int meta = vd < 0 ? site_search(s, 0, n, "\"channelMetadataRenderer\":{") : -1;
    if (meta >= 0) meta = sj_find(s, meta, n, "channelMetadataRenderer");
    /* A playlist's page names itself the same way, in its own renderer. */
    if (vd < 0 && meta < 0) {
        meta = site_search(s, 0, n, "\"playlistMetadataRenderer\":{");
        if (meta >= 0) meta = sj_find(s, meta, n, "playlistMetadataRenderer");
    }

    title[0] = 0;
    if (vd >= 0) sj_field(s, vd, n, "title", title, sizeof(title));
    else if (meta >= 0) sj_field(s, meta, n, "title", title, sizeof(title));
    sp_raw(&p, "<html><head><title>");
    if (title[0] || q[0]) {
        sp_text(&p, title[0] ? title : q);
        sp_raw(&p, " - ");
    }
    sp_raw(&p, "YouTube</title></head><body>\n");

    sp_raw(&p, "<form action=\"/results\" method=\"get\"><b>YouTube</b> "
               "<input type=\"text\" name=\"search_query\" size=\"40\" value=\"");
    sp_text(&p, q);
    sp_raw(&p, "\"> <input type=\"submit\" value=\"search\"></form>\n"
               "<p><small>Read by zelr from the data in YouTube's page, because the page "
               "itself is an application this browser cannot run.</small></p>\n");

    if (vd >= 0) {
        char id[64], clean[16];
        sj_field(s, vd, n, "videoId", id, sizeof(id));
        site_ident(id, clean, sizeof(clean), "-_");
        sj_field(s, vd, n, "author", who, sizeof(who));
        /* The channel, by its id: the details carry no address for it. */
        char chan[64], href[80];
        sj_field(s, vd, n, "channelId", text, sizeof(text));
        href[0] = 0;
        if (site_ident(text, chan, sizeof(chan), "_-") == w_len(text) && chan[0] == 'U' && chan[1] == 'C') {
            int o = 0;
            for (const char *q = "/channel/"; *q; q++) href[o++] = *q;
            for (const char *q = chan; *q && o < (int)sizeof(href) - 1; q++) href[o++] = *q;
            href[o] = 0;
        }

        sp_raw(&p, "<h1>");
        sp_text(&p, title);
        sp_raw(&p, "</h1>\n<p><img src=\"https://i.ytimg.com/vi/");
        sp_raw(&p, clean);
        sp_raw(&p, "/hqdefault.jpg\" width=\"480\" height=\"360\" alt=\"\"></p>\n<p><b>");
        yt_who(&p, who, href);
        sp_raw(&p, "</b>");
        sj_field(s, vd, n, "viewCount", num, sizeof(num));
        if (num[0]) { sp_raw(&p, " &middot; "); sp_num(&p, site_atoll(num)); sp_raw(&p, " views"); }
        /* Live now, rather than a stream that has ended, whose length is
           real however long it ran. */
        sj_field(s, vd, n, "isLive", text, sizeof(text));
        int live = w_same(text, "true");
        sj_field(s, vd, n, "lengthSeconds", num, sizeof(num));
        if (!live && num[0] && site_atoll(num) > 0) { sp_raw(&p, " &middot; "); yt_duration(&p, site_atoll(num)); }
        if (live) sp_raw(&p, " &middot; live");
        sp_raw(&p, "</p>\n<p><small>The video itself is H.264, sent in pieces chosen by YouTube's "
                   "own player; this browser has no video decoder to play it.</small></p>\n");
        if (!live) yt_frames(&p, s, n);

        sj_field(s, vd, n, "shortDescription", text, sizeof(text));
        if (text[0]) { sp_raw(&p, "<p>"); sp_text(&p, text); sp_raw(&p, "</p>\n"); }

        sp_raw(&p, "<h2>more like it</h2>\n");
        if (!yt_videos(&p, s, n, 20)) sp_raw(&p, "<p>nothing else was listed</p>\n");
    } else {
        if (meta >= 0) {
            sj_field(s, meta, n, "description", text, sizeof(text));
            sp_raw(&p, "<h1>");
            sp_text(&p, title);
            sp_raw(&p, "</h1>\n");
            if (text[0]) { sp_raw(&p, "<p>"); sp_text(&p, text); sp_raw(&p, "</p>\n"); }
        } else if (q[0]) {
            sp_raw(&p, "<h2>");
            sp_text(&p, q);
            sp_raw(&p, "</h2>\n");
        }
        if (!yt_videos(&p, s, n, 24))
            sp_raw(&p, "<p>YouTube did not list any videos here. It shows nothing on its front page "
                       "to somebody who is not signed in: search for something above.</p>\n");
    }
    sp_raw(&p, "</body></html>\n");
    return p.n;
}

/* --- Twitch -------------------------------------------------------------------- */

static inline int site_is_twitch(const url_t *u) {
    static const char *const H[] = { "www.twitch.tv", "twitch.tv", "m.twitch.tv", 0 };
    return site_host_is(u, H);
}

/* The Client-Id Twitch's own web page sends. The API answers ordinary
   public questions -- who is live, what is being played -- to any client that
   gives one. */
#define TWITCH_CLIENT "Client-Id: kimne78kx3ncx6brgo4mv6wki5h1ko\r\n"
#define TWITCH_REPLY (128 * 1024)

/* What a Twitch address is a page of. */
enum { TW_NONE, TW_LIVE, TW_CATEGORIES, TW_CATEGORY, TW_CHANNEL, TW_VIDEO, TW_SEARCH };

/* The path without its query or a slash at the end. */
static inline void twitch_path(const url_t *u, char *path, int cap) {
    int pl = 0;
    for (const char *t = u->path; *t && *t != '?' && pl < cap - 1; t++) path[pl++] = *t;
    path[pl] = 0;
    while (pl > 1 && path[pl - 1] == '/') path[--pl] = 0;
}

/* A GraphQL string, with nothing in it that could end it early. */
static inline void twitch_quote(const char *in, char *out, int cap) {
    int o = 0;
    for (; *in && o < cap - 1; in++)
        if (*in != '"' && *in != '\\' && (u8)*in >= 32) out[o++] = *in;
    out[o] = 0;
}

/* Three pieces run together: the question around a name. */
static inline void twitch_join(char *out, int cap, const char *a, const char *b, const char *c) {
    int k = 0;
    for (const char *t = a; *t && k < cap - 1; t++) out[k++] = *t;
    for (const char *t = b; *t && k < cap - 1; t++) out[k++] = *t;
    for (const char *t = c; *t && k < cap - 1; t++) out[k++] = *t;
    out[k] = 0;
}

/* The question an address asks of the API, and what kind of page the answer
   makes; TW_NONE for an address that is not a page this knows. `name` is the
   category or the channel, as it will be shown. */
static inline int twitch_query(const url_t *u, char *query, int cap, char *name, int ncap) {
    static char path[URL_PATH], decoded[256];
    query[0] = 0;
    name[0] = 0;
    if (!site_is_twitch(u)) return TW_NONE;
    twitch_path(u, path, sizeof(path));
    int pl = w_len(path);

    if (pl <= 1) {
        twitch_join(query, cap, "query{streams(first:24){edges{node{title viewersCount broadcaster{login "
            "displayName} game{name} previewImageURL(width:320,height:180)}}}}", "", "");
        return TW_LIVE;
    }
    if (w_same_fold(path, "/directory")) {
        twitch_join(query, cap, "query{games(first:30){edges{node{name viewersCount "
            "boxArtURL(width:144,height:192)}}}}", "", "");
        return TW_CATEGORIES;
    }
    if (w_starts_fold(path, "/directory/game/") || w_starts_fold(path, "/directory/category/")) {
        const char *raw = path + (w_starts_fold(path, "/directory/game/") ? 16 : 20);
        site_unescape(raw, w_len(raw), decoded, sizeof(decoded), 0);
        twitch_quote(decoded, name, ncap);
        if (!name[0]) return TW_NONE;
        twitch_join(query, cap, "query{game(name:\"", name, "\"){displayName streams(first:24){edges{node{"
            "title viewersCount broadcaster{login displayName} game{name} "
            "previewImageURL(width:320,height:180)}}}}}");
        return TW_CATEGORY;
    }
    /* Twitch's own address for a search. With nothing to look for there is
       nothing to ask, and the page is only the box. */
    if (w_same_fold(path, "/search")) {
        static char term[256];
        site_param(u->path, "term", term, sizeof(term));
        twitch_quote(term, name, ncap);
        if (name[0])
            twitch_join(query, cap, "query{searchFor(userQuery:\"", name, "\",platform:\"web\"){channels{"
                "items{login displayName followers{totalCount} stream{viewersCount title game{name} "
                "previewImageURL(width:320,height:180)}}}}}");
        return TW_SEARCH;
    }
    /* A past broadcast, by its number. */
    if (w_starts_fold(path, "/videos/")) {
        char id[24];
        int il = w_len(path + 8);
        if (il <= 0 || il >= (int)sizeof(id) || site_ident(path + 8, id, sizeof(id), "") != il)
            return TW_NONE;
        w_copy(name, ncap, id, ncap);
        twitch_join(query, cap, "query{video(id:\"", id, "\"){title lengthSeconds viewCount publishedAt "
            "owner{login displayName} game{name} previewThumbnailURL(width:640,height:360) seekPreviewsURL}}");
        return TW_VIDEO;
    }
    /* A channel, and whatever page of it: its name is the first part. Only
       what a login is made of, so a name cannot close the quotes it is
       written into and ask something else. */
    if (!site_ident(path + 1, name, ncap, "_")) return TW_NONE;
    twitch_join(query, cap, "query{user(login:\"", name, "\"){displayName description stream{title "
        "viewersCount game{name} previewImageURL(width:640,height:360)} "
        "lastBroadcast{title} videos(first:10){edges{node{id title lengthSeconds viewCount publishedAt "
        "previewThumbnailURL(width:320,height:180) game{name}}}}}}");
    return TW_CHANNEL;
}

/* A path segment, with anything but what needs no escaping escaped. */
static inline void sp_path(site_page *p, const char *t) {
    static const char HEX[] = "0123456789ABCDEF";
    for (; *t; t++) {
        char c = *t;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '.' || c == '_') {
            char one[2] = { c, 0 };
            sp_raw(p, one);
        } else {
            char esc[4] = { '%', HEX[(u8)c >> 4], HEX[(u8)c & 15], 0 };
            sp_raw(p, esc);
        }
    }
}

/* A picture only from Twitch's own picture server: the address is written
   into the page, and it came from an answer. */
static inline void twitch_picture(site_page *p, const char *url, int w, int h) {
    if (!w_starts_fold(url, "https://static-cdn.jtvnw.net/")) return;
    sp_raw(p, "<img src=\"");
    sp_text(p, url);
    sp_raw(p, "\" width=\"");
    sp_num(p, w);
    sp_raw(p, "\" height=\"");
    sp_num(p, h);
    sp_raw(p, "\" alt=\"\">");
}

static inline int twitch_streams(site_page *p, const char *s, int n) {
    static char title[YT_FIELD], login[64], raw[64], name[128], game[128], pic[256], viewers[32];
    int shown = 0;
    for (int at = site_search(s, 0, n, "\"node\":{"); at >= 0;
         at = site_search(s, at + 1, n, "\"node\":{")) {
        int obj = sj_find(s, at, n, "node");
        int end = sj_skip(s, obj, n);
        int b = sj_find(s, obj, end, "broadcaster");
        if (b < 0 || s[b] != '{') continue;
        sj_field(s, b, n, "login", raw, sizeof(raw));
        if (!site_ident(raw, login, sizeof(login), "_")) continue;
        sj_field(s, b, n, "displayName", name, sizeof(name));
        sj_field(s, obj, n, "title", title, sizeof(title));
        sj_field(s, obj, n, "viewersCount", viewers, sizeof(viewers));
        game[0] = 0;
        int g = sj_find(s, obj, end, "game");
        if (g >= 0 && s[g] == '{') sj_field(s, g, n, "name", game, sizeof(game));
        sj_field(s, obj, n, "previewImageURL", pic, sizeof(pic));

        sp_row_open(p, 240);
        sp_raw(p, "<a href=\"/");
        sp_raw(p, login);
        sp_raw(p, "\">");
        twitch_picture(p, pic, 240, 135);
        sp_raw(p, "</a>");
        sp_row_words(p);
        sp_raw(p, "<a href=\"/");
        sp_raw(p, login);
        sp_raw(p, "\"><b>");
        sp_text(p, title[0] ? title : login);
        sp_raw(p, "</b></a><br><small>");
        sp_text(p, name[0] ? name : login);
        if (game[0]) { sp_raw(p, " &middot; "); sp_text(p, game); }
        if (viewers[0]) { sp_raw(p, "<br>"); sp_num(p, site_atoll(viewers)); sp_raw(p, " watching"); }
        sp_raw(p, "</small>");
        sp_row_close(p);
        shown++;
    }
    if (!shown) sp_raw(p, "<p>nobody is live here just now</p>\n");
    return shown;
}

/* The next object in an array, from `at` (just inside the bracket or just
   after the last one): its start, with `at` moved past it; -1 at the end. */
static inline int sj_next_item(const char *s, int *at, int n) {
    int i = *at;
    while (i < n && (s[i] == ' ' || s[i] == ',' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
    if (i >= n || s[i] != '{') return -1;
    *at = sj_skip(s, i, n);
    return i;
}

/* A date as its day, which is the part of 2026-09-27T18:24:59Z worth reading. */
static inline void sp_day(site_page *p, const char *iso) {
    char day[12];
    int k = 0;
    while (k < 10 && ((iso[k] >= '0' && iso[k] <= '9') || iso[k] == '-')) { day[k] = iso[k]; k++; }
    day[k] = 0;
    sp_raw(p, day);
}

/* A channel's past broadcasts, one row each, linked to their own pages. */
static inline int twitch_videos(site_page *p, const char *s, int from, int to) {
    static char id[32], raw[32], title[YT_FIELD], len[24], views[24], when[40], pic[256], game[128];
    int shown = 0;
    for (int at = site_search(s, from, to, "\"node\":{"); at >= 0 && at < to;
         at = site_search(s, at + 1, to, "\"node\":{")) {
        int obj = sj_find(s, at, to, "node");
        int end = sj_skip(s, obj, to);
        sj_field(s, obj, to, "id", raw, sizeof(raw));
        if (!site_ident(raw, id, sizeof(id), "") || w_len(id) != w_len(raw)) continue;
        sj_field(s, obj, to, "title", title, sizeof(title));
        sj_field(s, obj, to, "lengthSeconds", len, sizeof(len));
        sj_field(s, obj, to, "viewCount", views, sizeof(views));
        sj_field(s, obj, to, "publishedAt", when, sizeof(when));
        sj_field(s, obj, to, "previewThumbnailURL", pic, sizeof(pic));
        game[0] = 0;
        int g = sj_find(s, obj, end, "game");
        if (g >= 0 && s[g] == '{') sj_field(s, g, to, "name", game, sizeof(game));

        sp_row_open(p, 240);
        sp_raw(p, "<a href=\"/videos/");
        sp_raw(p, id);
        sp_raw(p, "\">");
        twitch_picture(p, pic, 240, 135);
        sp_raw(p, "</a>");
        sp_row_words(p);
        sp_raw(p, "<a href=\"/videos/");
        sp_raw(p, id);
        sp_raw(p, "\"><b>");
        sp_text(p, title[0] ? title : id);
        sp_raw(p, "</b></a><br><small>");
        if (game[0]) { sp_text(p, game); sp_raw(p, " &middot; "); }
        sp_num(p, site_atoll(views));
        sp_raw(p, " views &middot; ");
        yt_duration(p, site_atoll(len));
        if (when[0]) { sp_raw(p, " &middot; "); sp_day(p, when); }
        sp_raw(p, "</small>");
        sp_row_close(p);
        shown++;
    }
    return shown;
}

/* Frames from a past broadcast. Twitch keeps them for scrubbing too: its
   answer names a list (seekPreviewsURL) of sheets of frames beside it, each
   so many columns and rows of frames an interval apart. The list is fetched
   by site_twitch and handed in here as `info`; the sheets are named relative
   to it. Only a list on Twitch's own video servers is used, only sheet names
   made of what file names are made of, and three sheets at most -- the
   first, the middle and the last. */
static inline int twitch_seek_base(const char *url, char *base, int cap) {
    base[0] = 0;
    if (!w_starts_fold(url, "https://")) return 0;
    const char *host = url + 8, *slash = host;
    while (*slash && *slash != '/') slash++;
    int hl = (int)(slash - host);
    const char *tail = ".cloudfront.net";
    int tl = w_len(tail);
    if (hl <= tl || !w_starts_fold(host + hl - tl, tail)) return 0;
    int last = -1;
    for (int i = 0; url[i]; i++) if (url[i] == '/') last = i;
    if (last < 8 + hl || last + 1 >= cap) return 0;
    w_copy(base, cap, url, last + 1);           /* up to and with the slash */
    return last + 1;
}

static inline void twitch_frames(site_page *p, const char *base, const char *info, int n) {
    if (!info || n <= 0 || !base[0]) return;
    /* The list is one entry per quality; the smaller frames are wanted. */
    int at = 0;
    while (at < n && info[at] != '[') at++;
    at++;
    int pick = -1, first = -1;
    for (int obj; (obj = sj_next_item(info, &at, n)) >= 0;) {
        char q[16];
        if (first < 0) first = obj;
        sj_field(info, obj, n, "quality", q, sizeof(q));
        if (w_same(q, "low")) { pick = obj; break; }
    }
    if (pick < 0) pick = first;
    if (pick < 0) return;
    char num[24];
    sj_field(info, pick, n, "count", num, sizeof(num));
    int count = (int)site_atoll(num);
    sj_field(info, pick, n, "cols", num, sizeof(num));
    int cols = (int)site_atoll(num);
    sj_field(info, pick, n, "rows", num, sizeof(num));
    int rows = (int)site_atoll(num);
    sj_field(info, pick, n, "interval", num, sizeof(num));
    int every = (int)site_atoll(num);
    int end = sj_skip(info, pick, n);
    int imgs = sj_find(info, pick, end, "images");
    if (count <= 0 || cols <= 0 || rows <= 0 || every <= 0 || imgs < 0 || info[imgs] != '[') return;

    /* The sheet names, in order. */
    static char names[64][96];
    int nn = 0, ie = sj_skip(info, imgs, n);
    for (int q = imgs + 1; q < ie && nn < 64;) {
        while (q < ie && info[q] != '"') q++;
        if (q >= ie) break;
        char raw[96];
        int rl = sj_str(info, q, n, raw, sizeof(raw));
        if (rl > 0 && site_ident(raw, names[nn], 96, "-_.") == rl && raw[0] != '.') nn++;
        q = sj_skip(info, q, n);
    }
    if (!nn) return;
    int per = cols * rows;
    int pickn[3] = { 0, nn / 2, nn - 1 };
    sp_raw(p, "<h2>frames from the broadcast</h2>\n<p><small>One every ");
    sp_num(p, every);
    sp_raw(p, every == 1 ? " second" : " seconds");
    sp_raw(p, ", from Twitch's storyboards.</small></p>\n");
    for (int k = 0; k < 3; k++) {
        if (k && pickn[k] == pickn[k - 1]) continue;
        int sheet = pickn[k];
        int f0 = sheet * per, f1 = f0 + per - 1;
        if (f1 > count - 1) f1 = count - 1;
        sp_raw(p, "<p><img src=\"");
        sp_text(p, base);
        sp_raw(p, names[sheet]);
        sp_raw(p, "\" alt=\"\"><br><small>");
        yt_duration(p, (long long)f0 * every);
        sp_raw(p, " to ");
        yt_duration(p, (long long)f1 * every);
        sp_raw(p, "</small></p>\n");
    }
}

/* The page, from the API's answer to twitch_query's question, and for a past
   broadcast the list of its frames (`info`, or nothing). */
static inline int twitch_page(int kind, const char *name, const char *s, int n,
                              const char *info, int info_n, char *out, int cap) {
    site_page p = { out, 0, cap };
    const char *NOTE = "<p><small>Read by zelr from Twitch's public API, because Twitch's page is "
                       "an application this browser cannot run. A stream itself is H.264 video in "
                       "pieces (HLS), and this browser has no video decoder to play it.</small></p>\n";
    const char *NAV = "<form action=\"/search\" method=\"get\"><p><a href=\"/\">live now</a> &middot; "
                      "<a href=\"/directory\">categories</a> &middot; <input type=\"text\" name=\"term\" "
                      "size=\"24\"> <input type=\"submit\" value=\"search\"></p></form>\n";

    sp_raw(&p, "<html><head><title>");
    if (kind == TW_LIVE) sp_raw(&p, "Twitch");
    else if (kind == TW_CATEGORIES) sp_raw(&p, "Categories - Twitch");
    else if (kind == TW_SEARCH) { sp_text(&p, name[0] ? name : "search"); sp_raw(&p, " - Twitch"); }
    else if (kind != TW_VIDEO) { sp_text(&p, name); sp_raw(&p, " - Twitch"); }
    else {
        static char vt[YT_FIELD];
        int v = sj_find(s, 0, n, "video");
        vt[0] = 0;
        if (v >= 0 && s[v] == '{') sj_field(s, v, n, "title", vt, sizeof(vt));
        sp_text(&p, vt[0] ? vt : name);
        sp_raw(&p, " - Twitch");
    }
    sp_raw(&p, "</title></head><body>\n<h1>");
    if (kind == TW_LIVE) sp_raw(&p, "Twitch: live now");
    else if (kind == TW_CATEGORIES) sp_raw(&p, "Twitch: categories");
    else if (kind == TW_CATEGORY) sp_text(&p, name);

    if (kind == TW_SEARCH) {
        sp_raw(&p, "Twitch: search</h1>\n");
        sp_raw(&p, NAV);
        int items = n > 0 ? sj_find(s, 0, n, "items") : -1;
        int shown = 0;
        if (items >= 0 && s[items] == '[') {
            static char raw[64], login[64], who[128], fol[24], title[YT_FIELD], game[128], pic[256], viewers[24];
            int at = items + 1, ie = sj_skip(s, items, n);
            for (int obj; (obj = sj_next_item(s, &at, ie)) >= 0;) {
                int oe = sj_skip(s, obj, ie);
                sj_field(s, obj, oe, "login", raw, sizeof(raw));
                if (!site_ident(raw, login, sizeof(login), "_") || w_len(login) != w_len(raw)) continue;
                sj_field(s, obj, oe, "displayName", who, sizeof(who));
                int f = sj_find(s, obj, oe, "followers");
                fol[0] = 0;
                if (f >= 0 && s[f] == '{') sj_field(s, f, oe, "totalCount", fol, sizeof(fol));
                int st = sj_find(s, obj, oe, "stream");
                if (st >= 0 && s[st] == '{') {
                    int se = sj_skip(s, st, oe);
                    sj_field(s, st, se, "title", title, sizeof(title));
                    sj_field(s, st, se, "viewersCount", viewers, sizeof(viewers));
                    sj_field(s, st, se, "previewImageURL", pic, sizeof(pic));
                    game[0] = 0;
                    int g = sj_find(s, st, se, "game");
                    if (g >= 0 && s[g] == '{') sj_field(s, g, se, "name", game, sizeof(game));
                    sp_row_open(&p, 240);
                    sp_raw(&p, "<a href=\"/");
                    sp_raw(&p, login);
                    sp_raw(&p, "\">");
                    twitch_picture(&p, pic, 240, 135);
                    sp_raw(&p, "</a>");
                    sp_row_words(&p);
                    sp_raw(&p, "<a href=\"/");
                    sp_raw(&p, login);
                    sp_raw(&p, "\"><b>");
                    sp_text(&p, who[0] ? who : login);
                    sp_raw(&p, "</b></a><br><small>live: ");
                    sp_text(&p, title);
                    if (game[0]) { sp_raw(&p, " &middot; "); sp_text(&p, game); }
                    sp_raw(&p, "<br>");
                    sp_num(&p, site_atoll(viewers));
                    sp_raw(&p, " watching &middot; ");
                    sp_num(&p, site_atoll(fol));
                    sp_raw(&p, " followers</small>");
                    sp_row_close(&p);
                } else {
                    sp_raw(&p, "<p><a href=\"/");
                    sp_raw(&p, login);
                    sp_raw(&p, "\"><b>");
                    sp_text(&p, who[0] ? who : login);
                    sp_raw(&p, "</b></a> <small>offline &middot; ");
                    sp_num(&p, site_atoll(fol));
                    sp_raw(&p, " followers</small></p>\n");
                }
                shown++;
            }
        }
        if (!name[0]) sp_raw(&p, "<p>Type what to look for in the box above.</p>\n");
        else if (!shown) sp_raw(&p, "<p>Twitch found no channel by that name.</p>\n");
        sp_raw(&p, NOTE);
    } else if (kind == TW_VIDEO) {
        static char vt[YT_FIELD], raw[64], login[64], who[128], game[128], pic[256], len[24], views[24];
        static char when[40], seek[512], base[512];
        int v = sj_find(s, 0, n, "video");
        if (v < 0 || s[v] != '{') {
            sp_raw(&p, "a past broadcast</h1>\n");
            sp_raw(&p, NAV);
            sp_raw(&p, "<p>Twitch has no past broadcast by that number.</p>\n");
        } else {
            int ve = sj_skip(s, v, n);
            sj_field(s, v, ve, "title", vt, sizeof(vt));
            sj_field(s, v, ve, "lengthSeconds", len, sizeof(len));
            sj_field(s, v, ve, "viewCount", views, sizeof(views));
            sj_field(s, v, ve, "publishedAt", when, sizeof(when));
            sj_field(s, v, ve, "previewThumbnailURL", pic, sizeof(pic));
            sj_field(s, v, ve, "seekPreviewsURL", seek, sizeof(seek));
            login[0] = who[0] = game[0] = 0;
            int o = sj_find(s, v, ve, "owner");
            if (o >= 0 && s[o] == '{') {
                sj_field(s, o, ve, "login", raw, sizeof(raw));
                if (site_ident(raw, login, sizeof(login), "_") != w_len(raw)) login[0] = 0;
                sj_field(s, o, ve, "displayName", who, sizeof(who));
            }
            int g = sj_find(s, v, ve, "game");
            if (g >= 0 && s[g] == '{') sj_field(s, g, ve, "name", game, sizeof(game));
            sp_text(&p, vt[0] ? vt : name);
            sp_raw(&p, "</h1>\n");
            sp_raw(&p, NAV);
            sp_raw(&p, "<p>");
            twitch_picture(&p, pic, 640, 360);
            sp_raw(&p, "</p>\n<p>");
            if (login[0]) {
                sp_raw(&p, "<b><a href=\"/");
                sp_raw(&p, login);
                sp_raw(&p, "\">");
                sp_text(&p, who[0] ? who : login);
                sp_raw(&p, "</a></b> &middot; ");
            }
            if (game[0]) { sp_text(&p, game); sp_raw(&p, " &middot; "); }
            sp_num(&p, site_atoll(views));
            sp_raw(&p, " views &middot; ");
            yt_duration(&p, site_atoll(len));
            if (when[0]) { sp_raw(&p, " &middot; "); sp_day(&p, when); }
            sp_raw(&p, "</p>\n");
            sp_raw(&p, NOTE);
            if (twitch_seek_base(seek, base, sizeof(base))) twitch_frames(&p, base, info, info_n);
        }
    } else if (kind == TW_LIVE || kind == TW_CATEGORY) {
        sp_raw(&p, "</h1>\n");
        sp_raw(&p, NAV);
        sp_raw(&p, NOTE);
        /* A category Twitch has no record of answers null, which lists
           nobody. */
        twitch_streams(&p, s, n);
    } else if (kind == TW_CATEGORIES) {
        sp_raw(&p, "</h1>\n");
        sp_raw(&p, NAV);
        sp_raw(&p, NOTE);
        static char game[128], art[256], viewers[32];
        int shown = 0;
        for (int at = site_search(s, 0, n, "\"node\":{"); at >= 0;
             at = site_search(s, at + 1, n, "\"node\":{")) {
            int obj = sj_find(s, at, n, "node");
            sj_field(s, obj, n, "name", game, sizeof(game));
            if (!game[0]) continue;
            sj_field(s, obj, n, "viewersCount", viewers, sizeof(viewers));
            sj_field(s, obj, n, "boxArtURL", art, sizeof(art));
            sp_row_open(&p, 72);
            sp_raw(&p, "<a href=\"/directory/game/");
            sp_path(&p, game);
            sp_raw(&p, "\">");
            twitch_picture(&p, art, 72, 96);
            sp_raw(&p, "</a>");
            sp_row_words(&p);
            sp_raw(&p, "<a href=\"/directory/game/");
            sp_path(&p, game);
            sp_raw(&p, "\"><b>");
            sp_text(&p, game);
            sp_raw(&p, "</b></a><br><small>");
            sp_num(&p, site_atoll(viewers));
            sp_raw(&p, " watching</small>");
            sp_row_close(&p);
            shown++;
        }
        if (!shown) sp_raw(&p, "<p>Twitch listed no categories</p>\n");
    } else {
        static char shown_as[128], desc[1024], title[YT_FIELD], game[128], pic[256], viewers[32];
        int user = sj_find(s, 0, n, "user");
        if (user < 0 || s[user] != '{') {
            sp_text(&p, name);
            sp_raw(&p, "</h1>\n");
            sp_raw(&p, NAV);
            sp_raw(&p, "<p>Twitch knows nobody by that name.</p>\n");
        } else {
            sj_field(s, user, n, "displayName", shown_as, sizeof(shown_as));
            sj_field(s, user, n, "description", desc, sizeof(desc));
            sp_text(&p, shown_as[0] ? shown_as : name);
            sp_raw(&p, "</h1>\n");
            sp_raw(&p, NAV);
            int ue = sj_skip(s, user, n);
            int st = sj_find(s, user, ue, "stream");
            if (st >= 0 && s[st] == '{') {
                int se = sj_skip(s, st, n);
                sj_field(s, st, n, "title", title, sizeof(title));
                sj_field(s, st, n, "viewersCount", viewers, sizeof(viewers));
                sj_field(s, st, n, "previewImageURL", pic, sizeof(pic));
                game[0] = 0;
                int g = sj_find(s, st, se, "game");
                if (g >= 0 && s[g] == '{') sj_field(s, g, n, "name", game, sizeof(game));
                sp_raw(&p, "<p><b>live:</b> ");
                sp_text(&p, title);
                sp_raw(&p, "</p>\n<p>");
                twitch_picture(&p, pic, 640, 360);
                sp_raw(&p, "</p>\n<p>");
                if (game[0]) { sp_text(&p, game); sp_raw(&p, " &middot; "); }
                sp_num(&p, site_atoll(viewers));
                sp_raw(&p, " watching</p>\n");
            } else {
                int lb = sj_find(s, user, ue, "lastBroadcast");
                title[0] = 0;
                if (lb >= 0 && s[lb] == '{') sj_field(s, lb, n, "title", title, sizeof(title));
                sp_raw(&p, "<p>offline");
                if (title[0]) { sp_raw(&p, "; last streamed: "); sp_text(&p, title); }
                sp_raw(&p, "</p>\n");
            }
            if (desc[0]) { sp_raw(&p, "<p>"); sp_text(&p, desc); sp_raw(&p, "</p>\n"); }
            int vids = sj_find(s, user, ue, "videos");
            if (vids >= 0 && s[vids] == '{') {
                int at = p.n;
                sp_raw(&p, "<h2>past broadcasts</h2>\n");
                if (!twitch_videos(&p, s, vids, sj_skip(s, vids, n))) {
                    p.n = at;                       /* a heading over nothing says nothing */
                    p.out[p.n] = 0;
                }
            }
        }
        sp_raw(&p, NOTE);
    }
    sp_raw(&p, "</body></html>\n");
    return p.n;
}

/* The question, as the JSON body the API takes. */
static inline void twitch_body(const char *query, char *json, int cap) {
    int o = 0;
    for (const char *t = "{\"query\":\""; *t && o < cap - 1; t++) json[o++] = *t;
    for (const char *t = query; *t && o < cap - 4; t++) {
        if (*t == '"' || *t == '\\') json[o++] = '\\';
        json[o++] = *t;
    }
    json[o++] = '"';
    json[o++] = '}';
    json[o] = 0;
}

/* The page for a Twitch address, asked of the API: its length, 0 for an
   address that is not one of these pages, or a WEB_ERR_ when the API could
   not be asked. */
static inline int site_twitch(const url_t *u, char *out, int cap) {
    static char query[1024], json[2048], name[256];
    static char reply_buf[TWITCH_REPLY];
    int kind = twitch_query(u, query, sizeof(query), name, sizeof(name));
    if (kind == TW_NONE) return 0;
    if (!query[0]) return twitch_page(kind, name, "", 0, 0, 0, out, cap);
    twitch_body(query, json, sizeof(json));

    url_t api;
    if (!url_parse("https://gql.twitch.tv/gql", &api)) return WEB_ERR_SCHEME;
    response_t r;
    web_body_type = "text/plain;charset=UTF-8";
    web_extra = TWITCH_CLIENT;
    int rc = web_post(&api, json, reply_buf, TWITCH_REPLY, &r);
    web_body_type = 0;
    web_extra = 0;
    if (rc < 0) return rc;
    if (rc != 200) return WEB_ERR_EMPTY;

    /* A past broadcast's list of frames, which is a second question to a
       second server: asked only when the list is where it should be. */
    static char info_buf[32 * 1024], seek[512], base[512];
    const char *info = 0;
    int info_n = 0;
    if (kind == TW_VIDEO) {
        int v = sj_find(r.body, 0, r.len, "video");
        if (v >= 0 && r.body[v] == '{') {
            sj_field(r.body, v, r.len, "seekPreviewsURL", seek, sizeof(seek));
            url_t su;
            response_t sr;
            if (twitch_seek_base(seek, base, sizeof(base)) && url_parse(seek, &su)) {
                /* The answer so far is in reply_buf, which this fetch must
                   not touch: it goes into a buffer of its own. */
                int src = web_get(&su, info_buf, (int)sizeof(info_buf), &sr);
                if (src == 200) { info = sr.body; info_n = sr.len; }
            }
        }
    }
    return twitch_page(kind, name, r.body, r.len, info, info_n, out, cap);
}
