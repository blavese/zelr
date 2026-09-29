#pragma once
/* URL and URLSearchParams, the way the URL standard reads an address.
 *
 * Included from jsdom.h. X, Python.org and LinkedIn stopped on URL, and
 * Reddit and Ars Technica on URLSearchParams: a page builds every address
 * it asks for with them, and reads its own with them.
 *
 * web.h's url_t is the browser's: a host and a path to send, made for
 * fetching. A script needs the rest -- the scheme as written, a user and a
 * password, the query and the fragment apart, an origin, addresses that are
 * not http at all -- and it needs them parsed by the standard's rules,
 * because a page that builds a URL and reads a part back expects exactly
 * the part another browser would give it. So this is its own reader, of the
 * standard's parser in the cases pages write: a whole address, one relative
 * to a base (//host, /path, path, ?query, #fragment), the special schemes
 * with their default ports, dots in paths, and the characters each part
 * escapes. Not here: international names turned into punycode (a host is
 * kept in lower case as written) and the number forms of an IPv4 address.
 *
 * An object holds its whole address as one string under a symbol, and each
 * part is read from it when asked; a part set writes the whole address
 * again. URLSearchParams keeps its pairs as an array, and one that belongs
 * to a URL writes the URL's query whenever it changes.
 */

#define JU_PART 4096

typedef struct {
    char scheme[40];
    char user[512], pass[512];
    char host[512];
    int  port;                   /* -1: none, or the scheme's own */
    char path[JU_PART];
    char query[JU_PART];
    char frag[JU_PART];
    u8   has_host, has_query, has_frag, special, opaque;
} jurl;

static int ju_default_port(const char *scheme) {
    if (w_same(scheme, "http") || w_same(scheme, "ws")) return 80;
    if (w_same(scheme, "https") || w_same(scheme, "wss")) return 443;
    if (w_same(scheme, "ftp")) return 21;
    return -1;
}

static int ju_is_special(const char *scheme) {
    return ju_default_port(scheme) > 0 || w_same(scheme, "file");
}

/* Which characters each part escapes: the standard's percent-encode sets,
   each a superset of the one before. */
enum { JU_C0 = 0, JU_FRAG, JU_QUERY, JU_SQUERY, JU_PATH, JU_USER };

static int ju_escapes(u8 c, int set) {
    if (c < 0x20 || c > 0x7E) return 1;
    switch (set) {
        case JU_USER:
            if (c == '/' || c == ':' || c == ';' || c == '=' || c == '@' || c == '['
                || c == '\\' || c == ']' || c == '^' || c == '|') return 1;
            /* fall through */
        case JU_PATH:
            if (c == '?' || c == '`' || c == '{' || c == '}') return 1;
            /* fall through */
        case JU_QUERY: case JU_SQUERY:
            if (c == ' ' || c == '"' || c == '#' || c == '<' || c == '>') return 1;
            if (set == JU_SQUERY && c == '\'') return 1;
            return 0;
        case JU_FRAG:
            return c == ' ' || c == '"' || c == '<' || c == '>' || c == '`';
        default:
            return 0;
    }
}

/* Appends s[0..n) to out (a part of cap bytes), escaping what the set says. */
static void ju_put(char *out, int cap, const char *s, int n, int set) {
    static const char HEX[] = "0123456789ABCDEF";
    int w = w_len(out);
    for (int i = 0; i < n && w < cap - 4; i++) {
        u8 c = (u8)s[i];
        if (ju_escapes(c, set)) { out[w++] = '%'; out[w++] = HEX[c >> 4]; out[w++] = HEX[c & 15]; }
        else out[w++] = (char)c;
    }
    out[w] = 0;
}

static void ju_set(char *out, int cap, const char *s, int n, int set) {
    out[0] = 0;
    ju_put(out, cap, s, n, set);
}

static int ju_hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A host: percent-decoded, lower case, and refused when it holds what no
   host may. An address in brackets is kept as written. */
static int ju_host(const char *s, int n, char *out, int cap, int special) {
    int w = 0;
    if (n > 0 && s[0] == '[') {
        if (s[n - 1] != ']') return 0;
        for (int i = 0; i < n && w < cap - 1; i++) out[w++] = w_lower(s[i]);
        out[w] = 0;
        return 1;
    }
    for (int i = 0; i < n && w < cap - 1; i++) {
        char c = s[i];
        if (c == '%' && i + 2 < n && ju_hexv(s[i + 1]) >= 0 && ju_hexv(s[i + 2]) >= 0) {
            c = (char)(ju_hexv(s[i + 1]) * 16 + ju_hexv(s[i + 2]));
            i += 2;
        }
        if (c == ' ' || c == '#' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?'
            || c == '@' || c == '[' || c == '\\' || c == ']' || c == '^' || c == '|'
            || (special && c == '%') || (u8)c < 0x20 || c == 0x7F) return 0;
        out[w++] = special ? w_lower(c) : c;
    }
    out[w] = 0;
    if (special && !w) return 0;
    return 1;
}

/* A path's pieces put together with the dots taken out: "." is here and
   ".." is up one, in any case and escaped as %2e. */
static int ju_dot(const char *s, int n) {
    if (n == 1 && s[0] == '.') return 1;
    if (n == 3 && s[0] == '%' && s[1] == '2' && w_lower(s[2]) == 'e') return 1;
    if (n == 2 && s[0] == '.' && s[1] == '.') return 2;
    if (n == 4 && ((s[0] == '.' && s[1] == '%') || (s[0] == '%' && s[3] == '.'))) {
        const char *e = s[0] == '.' ? s + 1 : s;
        if (e[0] == '%' && e[1] == '2' && w_lower(e[2]) == 'e') return 2;
    }
    if (n == 6 && s[0] == '%' && s[1] == '2' && w_lower(s[2]) == 'e' && s[3] == '%' && s[4] == '2'
        && w_lower(s[5]) == 'e') return 2;
    return 0;
}

/* Path text, which may hold dots, added to the path so far: after an
   absolute path the caller has emptied it, and for a relative one cut it
   back to its directory, so it is empty or ends with a slash. It keeps
   ending with one while the pieces go on, and the last piece is added
   without. ".." takes the last piece off, "." does nothing. */
static void ju_path_append(jurl *u, const char *s, int n) {
    int i = 0;
    if (n > 0 && (s[0] == '/' || (u->special && s[0] == '\\'))) i = 1;
    if (!u->path[0]) {
        if (!u->special && !n) return;
        u->path[0] = '/';
        u->path[1] = 0;
    }
    for (;;) {
        int st = i;
        while (i < n && s[i] != '/' && !(u->special && s[i] == '\\')) i++;
        int seg = i - st, last = i >= n;
        int d = ju_dot(s + st, seg);
        if (d == 2) {
            int w = w_len(u->path);
            if (w > 1) {
                w--;                                   /* the slash it ends with */
                while (w > 0 && u->path[w - 1] != '/') w--;
                u->path[w] = 0;
            }
        } else if (d == 0) {
            ju_put(u->path, JU_PART, s + st, seg, JU_PATH);
            if (!last) ju_put(u->path, JU_PART, "/", 1, JU_C0);
        }
        if (last) break;
        i++;
    }
}

static int ju_scheme_char(char c, int first) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    return !first && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.');
}

static void ju_copy(jurl *d, const jurl *s) {
    volatile u8 *a = (volatile u8 *)d;
    const u8 *b = (const u8 *)s;
    for (u32 i = 0; i < sizeof(jurl); i++) a[i] = b[i];
}

/* The authority after "//": user and password, host, port. */
static int ju_authority(jurl *u, const char *s, int n) {
    int at = -1;
    for (int i = n - 1; i >= 0; i--) if (s[i] == '@') { at = i; break; }
    u->user[0] = u->pass[0] = 0;
    if (at >= 0) {
        int colon = -1;
        for (int i = 0; i < at; i++) if (s[i] == ':') { colon = i; break; }
        ju_set(u->user, (int)sizeof(u->user), s, colon >= 0 ? colon : at, JU_USER);
        if (colon >= 0) ju_set(u->pass, (int)sizeof(u->pass), s + colon + 1, at - colon - 1, JU_USER);
        s += at + 1;
        n -= at + 1;
    }
    int hn = n, colon = -1, inbr = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '[') inbr = 1;
        else if (s[i] == ']') inbr = 0;
        else if (s[i] == ':' && !inbr) { colon = i; }
    }
    if (colon >= 0) hn = colon;
    if (!ju_host(s, hn, u->host, (int)sizeof(u->host), u->special)) {
        if (!(w_same(u->scheme, "file") && hn == 0)) return 0;
        u->host[0] = 0;
    }
    u->has_host = 1;
    u->port = -1;
    if (colon >= 0 && colon + 1 < n) {
        long v = 0;
        for (int i = colon + 1; i < n; i++) {
            if (s[i] < '0' || s[i] > '9') return 0;
            v = v * 10 + (s[i] - '0');
            if (v > 65535) return 0;
        }
        if (v != ju_default_port(u->scheme)) u->port = (int)v;
    }
    return 1;
}

/* Reads `in` against `base` (which may be none). 1 when it is an address. */
static int ju_parse(const char *in, const jurl *base, jurl *u) {
    /* Leading and trailing spaces and controls go, and tabs and newlines
       anywhere in it, as the standard has it. */
    int len = w_len(in);
    while (len > 0 && (u8)in[len - 1] <= ' ') len--;
    while (len > 0 && (u8)in[0] <= ' ') { in++; len--; }
    char *buf = (char *)malloc((u64)len + 1);
    if (!buf) return 0;
    int n = 0;
    for (int i = 0; i < len; i++) if (in[i] != '\t' && in[i] != '\n' && in[i] != '\r') buf[n++] = in[i];
    buf[n] = 0;
    const char *s = buf;

    volatile u8 *z = (volatile u8 *)u;
    for (u32 i = 0; i < sizeof(jurl); i++) z[i] = 0;
    u->port = -1;

    int i = 0, ok = 1;
    while (i < n && ju_scheme_char(s[i], i == 0)) i++;
    int has_scheme = i > 0 && i < n && s[i] == ':';
    const char *rest;
    int rn;
    if (has_scheme) {
        int k = i < 39 ? i : 39;
        for (int j = 0; j < k; j++) u->scheme[j] = w_lower(s[j]);
        u->scheme[k] = 0;
        u->special = (u8)ju_is_special(u->scheme);
        rest = s + i + 1;
        rn = n - i - 1;
        /* "http:foo" against an http base is relative to it; with no base,
           or another scheme, the rest is read on its own. */
        if (u->special && base && w_same(base->scheme, u->scheme) && !(rn >= 1 && (rest[0] == '/' || rest[0] == '\\'))) {
            ju_copy(u, base);
            goto relative;
        }
    } else {
        if (!base) { ok = 0; goto done; }
        if (base->opaque) {
            if (n > 0 && s[0] == '#') {
                ju_copy(u, base);
                u->has_frag = 1;
                ju_set(u->frag, JU_PART, s + 1, n - 1, JU_FRAG);
                goto done;
            }
            ok = 0;
            goto done;
        }
        ju_copy(u, base);
        rest = s;
        rn = n;
        goto relative;
    }

    /* An address with its own scheme. */
    if (u->special) {
        /* The slashes after a special scheme may be any number of either
           kind, and a host must follow. */
        int k = 0;
        while (k < rn && (rest[k] == '/' || rest[k] == '\\')) k++;
        if (w_same(u->scheme, "file")) {
            if (k >= 2) {
                int e = k;
                while (e < rn && rest[e] != '/' && rest[e] != '\\' && rest[e] != '?' && rest[e] != '#') e++;
                if (!ju_authority(u, rest + k, e - k)) { ok = 0; goto done; }
                rest += e; rn -= e;
            } else {
                u->has_host = 1;
                u->host[0] = 0;
            }
        } else {
            int e = k;
            while (e < rn && rest[e] != '/' && rest[e] != '\\' && rest[e] != '?' && rest[e] != '#') e++;
            if (!ju_authority(u, rest + k, e - k)) { ok = 0; goto done; }
            rest += e; rn -= e;
        }
        u->path[0] = 0;
        goto path;
    }
    if (rn >= 2 && rest[0] == '/' && rest[1] == '/') {
        int e = 2;
        while (e < rn && rest[e] != '/' && rest[e] != '?' && rest[e] != '#') e++;
        if (!ju_authority(u, rest + 2, e - 2)) { ok = 0; goto done; }
        rest += e; rn -= e;
        u->path[0] = 0;
        goto path;
    }
    if (rn >= 1 && rest[0] == '/') { u->path[0] = 0; goto path; }
    /* mailto:, data:, javascript: -- a path that is not a path. */
    u->opaque = 1;
    {
        int e = 0;
        while (e < rn && rest[e] != '?' && rest[e] != '#') e++;
        ju_set(u->path, JU_PART, rest, e, JU_C0);
        rest += e; rn -= e;
    }
    goto tail;

relative:
    /* Against the base: what the input starts with says how much of the
       base it keeps. */
    if (rn >= 2 && (rest[0] == '/' || (u->special && rest[0] == '\\'))
        && (rest[1] == '/' || (u->special && rest[1] == '\\'))) {
        int e = 2;
        while (e < rn && rest[e] != '/' && !(u->special && rest[e] == '\\') && rest[e] != '?' && rest[e] != '#') e++;
        u->user[0] = u->pass[0] = 0;
        if (!ju_authority(u, rest + 2, e - 2)) { ok = 0; goto done; }
        rest += e; rn -= e;
        u->path[0] = 0;
        u->has_query = u->has_frag = 0;
        goto path;
    }
    if (rn >= 1 && (rest[0] == '/' || (u->special && rest[0] == '\\'))) {
        u->path[0] = 0;
        u->has_query = u->has_frag = 0;
        goto path;
    }
    if (rn >= 1 && rest[0] == '?') {
        u->has_query = u->has_frag = 0;
        goto tail;
    }
    if (rn >= 1 && rest[0] == '#') {
        u->has_frag = 0;
        goto tail;
    }
    if (rn == 0) { u->has_frag = 0; goto done; }
    /* A relative path: the base's directory, then this. */
    {
        int w = w_len(u->path);
        while (w > 0 && u->path[w - 1] != '/') w--;
        u->path[w] = 0;
        u->has_query = u->has_frag = 0;
    }

path:
    {
        int e = 0;
        while (e < rn && rest[e] != '?' && rest[e] != '#') e++;
        ju_path_append(u, rest, e);
        if (u->special && !u->path[0]) { u->path[0] = '/'; u->path[1] = 0; }
        rest += e; rn -= e;
    }

tail:
    if (rn >= 1 && rest[0] == '?') {
        int e = 1;
        while (e < rn && rest[e] != '#') e++;
        u->has_query = 1;
        ju_set(u->query, JU_PART, rest + 1, e - 1, u->special ? JU_SQUERY : JU_QUERY);
        rest += e; rn -= e;
    }
    if (rn >= 1 && rest[0] == '#') {
        u->has_frag = 1;
        ju_set(u->frag, JU_PART, rest + 1, rn - 1, JU_FRAG);
    }

done:
    free(buf);
    return ok;
}

/* The address as one string, as href gives it. */
static void ju_text(const jurl *u, jtext *t, int with_frag) {
    jd_put(t, u->scheme);
    jd_putc(t, ':');
    if (u->has_host) {
        jd_put(t, "//");
        if (u->user[0] || u->pass[0]) {
            jd_put(t, u->user);
            if (u->pass[0]) { jd_putc(t, ':'); jd_put(t, u->pass); }
            jd_putc(t, '@');
        }
        jd_put(t, u->host);
        if (u->port >= 0) { jd_putc(t, ':'); jd_put_num(t, u->port); }
    }
    jd_put(t, u->path);
    if (u->has_query) { jd_putc(t, '?'); jd_put(t, u->query); }
    if (with_frag && u->has_frag) { jd_putc(t, '#'); jd_put(t, u->frag); }
}

static jstr *ju_href(jctx *J, const jurl *u) {
    jtext t = { 0, 0, 0, 0 };
    ju_text(u, &t, 1);
    return jt_done(J, &t);
}

static jstr *ju_origin(jctx *J, const jurl *u) {
    if (!u->special || w_same(u->scheme, "file")) return js_str(J, "null");
    jtext t = { 0, 0, 0, 0 };
    jd_put(&t, u->scheme);
    jd_put(&t, "://");
    jd_put(&t, u->host);
    if (u->port >= 0) { jd_putc(&t, ':'); jd_put_num(&t, u->port); }
    return jt_done(J, &t);
}

/* The page's own address, as a jurl, or 0. */
static int ju_page(jurl *u) {
    return jd_address[0] && ju_parse(jd_address, 0, u);
}

/* An address a page wrote, whole: what href and src give back. */
static int jd_resolve(const char *href, char *out, int cap) {
    jurl *base = (jurl *)malloc(sizeof(jurl));
    jurl *u = (jurl *)malloc(sizeof(jurl));
    int ok = 0;
    if (base && u && ju_page(base) && ju_parse(href, base, u)) {
        jtext t = { 0, 0, 0, 0 };
        ju_text(u, &t, 1);
        int k = 0;
        for (; k < (int)t.n && k < cap - 1; k++) out[k] = t.b[k];
        out[k] = 0;
        free(t.b);
        ok = 1;
    }
    free(base);
    free(u);
    return ok;
}

/* --- form encoding -----------------------------------------------------------------------
 *
 * application/x-www-form-urlencoded: what a query string is, and what
 * URLSearchParams reads and writes. + is a space; everything but letters,
 * digits and *-._ is escaped. */
static jstr *ju_form_decode(jctx *J, const char *s, int n) {
    jtext t = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '+') c = ' ';
        else if (c == '%' && i + 2 < n && ju_hexv(s[i + 1]) >= 0 && ju_hexv(s[i + 2]) >= 0) {
            c = (char)(ju_hexv(s[i + 1]) * 16 + ju_hexv(s[i + 2]));
            i += 2;
        }
        jd_putc(&t, c);
    }
    return jt_done(J, &t);
}

static void ju_form_encode(jtext *t, const jstr *s) {
    static const char HEX[] = "0123456789ABCDEF";
    for (u32 i = 0; i < s->len; i++) {
        u8 c = (u8)s->s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '*' || c == '-' || c == '.' || c == '_') jd_putc(t, (char)c);
        else if (c == ' ') jd_putc(t, '+');
        else { char e[3] = { '%', HEX[c >> 4], HEX[c & 15] }; jt_put(&jd_J, t, e, 3); }
    }
}

/* --- URLSearchParams ---------------------------------------------------------------------- */

static jobj *jd_p_params, *jd_p_url;
static jstr *jd_k_pairs, *jd_k_owner, *jd_k_href, *jd_k_params;

static int jd_is_search_params_obj(jval v) {
    return js_is_obj(v) && js_find(v.obj, jd_k_pairs) != 0;
}

static jobj *jd_pairs_of(jctx *J, jval t) {
    jval p = js_is_obj(t) ? jd_kept(t.obj, jd_k_pairs) : js_undef();
    if (p.t == JS_OBJ && p.obj->kind == JO_ARRAY) return p.obj;
    js_throw(J, JS_ERR_TYPE, "that is not a URLSearchParams", J->error_line);
    return 0;
}

static void jd_pairs_parse(jctx *J, jobj *pairs, const char *s, int n) {
    pairs->len = 0;
    if (n > 0 && s[0] == '?') { s++; n--; }
    int i = 0;
    while (i < n) {
        int st = i;
        while (i < n && s[i] != '&') i++;
        int e = i;
        if (e > st) {
            int eq = st;
            while (eq < e && s[eq] != '=') eq++;
            jobj *pair = js_array(J);
            if (!pair) return;
            js_arr_push(J, pair, js_from_str(ju_form_decode(J, s + st, eq - st)));
            js_arr_push(J, pair, js_from_str(eq < e ? ju_form_decode(J, s + eq + 1, e - eq - 1) : js_str(J, "")));
            js_arr_push(J, pairs, js_from_obj(pair));
        }
        i++;
    }
}

static jstr *jd_pairs_text(jctx *J, jobj *pairs) {
    jtext t = { 0, 0, 0, 0 };
    for (u32 i = 0; i < pairs->len; i++) {
        jval pv = pairs->items[i];
        if (!js_is_obj(pv) || pv.obj->len < 2) continue;
        if (t.n) jd_putc(&t, '&');
        ju_form_encode(&t, pv.obj->items[0].str);
        jd_putc(&t, '=');
        ju_form_encode(&t, pv.obj->items[1].str);
    }
    return jt_done(J, &t);
}

static void jd_url_set_query_from(jctx *J, jobj *url, jobj *pairs);

/* After any change: the URL it belongs to, if any, gets the new query. */
static void jd_params_changed(jctx *J, jval t, jobj *pairs) {
    jval owner = jd_kept(t.obj, jd_k_owner);
    if (owner.t == JS_OBJ) jd_url_set_query_from(J, owner.obj, pairs);
}

static jobj *jd_new_params(jctx *J, jobj *proto) {
    jobj *o = js_object_with(J, JO_PLAIN, proto ? proto : jd_p_params);
    jobj *pairs = js_array(J);
    if (!o || !pairs) return 0;
    jd_keep(o, jd_k_pairs, js_from_obj(pairs));
    return o;
}

static jval nat_params_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "URLSearchParams is made with new", J->error_line);
    jobj *pairs = js_array(J);
    if (!pairs) return js_undef();
    jd_keep(t.obj, jd_k_pairs, js_from_obj(pairs));
    jval init = js_arg(a, n, 0);
    if (init.t == JS_UNDEF || init.t == JS_NULL) return js_undef();
    if (jd_is_search_params_obj(init)) {
        jobj *from = jd_pairs_of(J, init);
        for (u32 i = 0; from && i < from->len; i++) {
            jobj *pair = js_array(J);
            js_arr_push(J, pair, from->items[i].obj->items[0]);
            js_arr_push(J, pair, from->items[i].obj->items[1]);
            js_arr_push(J, pairs, js_from_obj(pair));
        }
        return js_undef();
    }
    if (js_is_obj(init)) {
        jval it = js_get(J, init, J->sym_iterator);
        if (J->sig != JS_OK) return js_undef();
        if (js_callable(it)) {
            jargs A;
            js_args_init(&A);
            if (js_iter_collect(J, init, &A)) {
                for (int i = 0; i < A.n && J->sig == JS_OK; i++) {
                    jargs B;
                    js_args_init(&B);
                    if (js_iter_collect(J, A.v[i], &B) && B.n == 2) {
                        jobj *pair = js_array(J);
                        js_arr_push(J, pair, js_from_str(js_to_str(J, B.v[0])));
                        js_arr_push(J, pair, js_from_str(js_to_str(J, B.v[1])));
                        js_arr_push(J, pairs, js_from_obj(pair));
                    } else if (J->sig == JS_OK) {
                        js_throw(J, JS_ERR_TYPE, "each pair for URLSearchParams needs two things", J->error_line);
                    }
                    js_args_free(&B);
                }
            }
            js_args_free(&A);
            return js_undef();
        }
        /* A record: its own enumerable string keys. */
        jprop **keys;
        u32 nk = js_own_keys(J, init.obj, &keys);
        for (u32 i = 0; i < nk && J->sig == JS_OK; i++) {
            jobj *pair = js_array(J);
            js_arr_push(J, pair, js_from_str(keys[i]->key));
            js_arr_push(J, pair, js_from_str(js_to_str(J, js_get(J, init, keys[i]->key))));
            js_arr_push(J, pairs, js_from_obj(pair));
        }
        return js_undef();
    }
    jstr *s = js_to_str(J, init);
    if (s) jd_pairs_parse(J, pairs, s->s, (int)s->len);
    return js_undef();
}

static jval nat_params_append(jctx *J, jval t, jval *a, int n) {
    jobj *pairs = jd_pairs_of(J, t);
    if (!pairs) return js_undef();
    jobj *pair = js_array(J);
    js_arr_push(J, pair, js_from_str(jd_arg_str(J, a, n, 0)));
    js_arr_push(J, pair, js_from_str(js_to_str(J, js_arg(a, n, 1))));
    js_arr_push(J, pairs, js_from_obj(pair));
    jd_params_changed(J, t, pairs);
    return js_undef();
}

static int jd_pair_is(jval pv, const jstr *name) {
    return js_is_obj(pv) && pv.obj->len >= 2 && pv.obj->items[0].t == JS_STR && js_str_eq(pv.obj->items[0].str, name);
}

static jval nat_params_delete(jctx *J, jval t, jval *a, int n) {
    jobj *pairs = jd_pairs_of(J, t);
    if (!pairs) return js_undef();
    jstr *name = jd_arg_str(J, a, n, 0);
    jval only = js_arg(a, n, 1);
    jstr *val = only.t != JS_UNDEF ? js_to_str(J, only) : 0;
    u32 w = 0;
    for (u32 i = 0; i < pairs->len; i++) {
        jval pv = pairs->items[i];
        if (jd_pair_is(pv, name) && (!val || js_str_eq(pv.obj->items[1].str, val))) continue;
        pairs->items[w++] = pv;
    }
    pairs->len = w;
    jd_params_changed(J, t, pairs);
    return js_undef();
}

static jval nat_params_get(jctx *J, jval t, jval *a, int n) {
    jobj *pairs = jd_pairs_of(J, t);
    if (!pairs) return js_undef();
    jstr *name = jd_arg_str(J, a, n, 0);
    for (u32 i = 0; i < pairs->len; i++)
        if (jd_pair_is(pairs->items[i], name)) return pairs->items[i].obj->items[1];
    return js_null();
}

static jval nat_params_getall(jctx *J, jval t, jval *a, int n) {
    jobj *pairs = jd_pairs_of(J, t);
    if (!pairs) return js_undef();
    jstr *name = jd_arg_str(J, a, n, 0);
    jobj *out = js_array(J);
    for (u32 i = 0; out && i < pairs->len; i++)
        if (jd_pair_is(pairs->items[i], name)) js_arr_push(J, out, pairs->items[i].obj->items[1]);
    return js_from_obj(out);
}

static jval nat_params_has(jctx *J, jval t, jval *a, int n) {
    jobj *pairs = jd_pairs_of(J, t);
    if (!pairs) return js_undef();
    jstr *name = jd_arg_str(J, a, n, 0);
    jval only = js_arg(a, n, 1);
    jstr *val = only.t != JS_UNDEF ? js_to_str(J, only) : 0;
    for (u32 i = 0; i < pairs->len; i++)
        if (jd_pair_is(pairs->items[i], name) && (!val || js_str_eq(pairs->items[i].obj->items[1].str, val)))
            return js_bool(1);
    return js_bool(0);
}

static jval nat_params_set(jctx *J, jval t, jval *a, int n) {
    jobj *pairs = jd_pairs_of(J, t);
    if (!pairs) return js_undef();
    jstr *name = jd_arg_str(J, a, n, 0);
    jstr *val = js_to_str(J, js_arg(a, n, 1));
    if (!val) return js_undef();
    u32 w = 0;
    int done = 0;
    for (u32 i = 0; i < pairs->len; i++) {
        jval pv = pairs->items[i];
        if (jd_pair_is(pv, name)) {
            if (done) continue;
            pv.obj->items[1] = js_from_str(val);
            done = 1;
        }
        pairs->items[w++] = pv;
    }
    pairs->len = w;
    if (!done) {
        jobj *pair = js_array(J);
        js_arr_push(J, pair, js_from_str(name));
        js_arr_push(J, pair, js_from_str(val));
        js_arr_push(J, pairs, js_from_obj(pair));
    }
    jd_params_changed(J, t, pairs);
    return js_undef();
}

/* By name, keeping the order of pairs with the same name: an insertion sort,
   which is stable, over what is always a short list. */
static int jd_str_before(const jstr *a, const jstr *b) {
    u32 n = a->len < b->len ? a->len : b->len;
    for (u32 i = 0; i < n; i++)
        if (a->s[i] != b->s[i]) return (u8)a->s[i] < (u8)b->s[i];
    return a->len < b->len;
}

static jval nat_params_sort(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *pairs = jd_pairs_of(J, t);
    if (!pairs) return js_undef();
    for (u32 i = 1; i < pairs->len; i++) {
        jval v = pairs->items[i];
        u32 k = i;
        while (k > 0 && jd_str_before(v.obj->items[0].str, pairs->items[k - 1].obj->items[0].str)) {
            pairs->items[k] = pairs->items[k - 1];
            k--;
        }
        pairs->items[k] = v;
    }
    jd_params_changed(J, t, pairs);
    return js_undef();
}

static jval nat_params_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *pairs = jd_pairs_of(J, t);
    return pairs ? js_from_str(jd_pairs_text(J, pairs)) : js_undef();
}

static jval nat_params_size(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *pairs = jd_pairs_of(J, t);
    return pairs ? js_num(pairs->len) : js_undef();
}

static jval nat_params_foreach(jctx *J, jval t, jval *a, int n) {
    jobj *pairs = jd_pairs_of(J, t);
    if (!pairs) return js_undef();
    jval fn = js_arg(a, n, 0);
    if (!js_callable(fn)) return js_throw(J, JS_ERR_TYPE, "forEach needs a function", J->error_line);
    for (u32 i = 0; i < pairs->len && J->sig == JS_OK; i++) {
        jval args[3] = { pairs->items[i].obj->items[1], pairs->items[i].obj->items[0], t };
        js_call(J, fn, js_arg(a, n, 1), args, 3);
    }
    return js_undef();
}

/* entries, keys and values, over a copy of the pairs as they are now. */
static jval jd_params_iter(jctx *J, jval t, int which) {
    jobj *pairs = jd_pairs_of(J, t);
    if (!pairs) return js_undef();
    jobj *arr = js_array(J);
    for (u32 i = 0; arr && i < pairs->len; i++) {
        jobj *p = pairs->items[i].obj;
        if (which == 0) {
            jobj *pair = js_array(J);
            js_arr_push(J, pair, p->items[0]);
            js_arr_push(J, pair, p->items[1]);
            js_arr_push(J, arr, js_from_obj(pair));
        } else {
            js_arr_push(J, arr, p->items[which == 1 ? 0 : 1]);
        }
    }
    return jd_array_iter(J, arr, "values");
}

static jval nat_params_entries(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_params_iter(J, t, 0); }
static jval nat_params_keys(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_params_iter(J, t, 1); }
static jval nat_params_values(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_params_iter(J, t, 2); }

/* --- URL ---------------------------------------------------------------------------------- */

static int jd_url_of(jctx *J, jval t, jurl *u) {
    jval h = js_is_obj(t) ? jd_kept(t.obj, jd_k_href) : js_undef();
    if (h.t != JS_STR) { js_throw(J, JS_ERR_TYPE, "that is not a URL", J->error_line); return 0; }
    return ju_parse(h.str->s, 0, u);
}

/* The address written back after a part changed; its searchParams, when
   it has been asked for, read again from the new query. */
static void jd_url_store(jctx *J, jobj *o, const jurl *u, int sync_params) {
    jd_keep(o, jd_k_href, js_from_str(ju_href(J, u)));
    if (!sync_params) return;
    jval p = jd_kept(o, jd_k_params);
    if (p.t == JS_OBJ) {
        jobj *pairs = jd_pairs_of(J, p);
        if (pairs) jd_pairs_parse(J, pairs, u->has_query ? u->query : "", u->has_query ? w_len(u->query) : 0);
    }
}

static void jd_url_set_query_from(jctx *J, jobj *url, jobj *pairs) {
    jurl *u = (jurl *)malloc(sizeof(jurl));
    if (!u) return;
    if (jd_url_of(J, js_from_obj(url), u)) {
        jstr *q = jd_pairs_text(J, pairs);
        u->has_query = q && q->len > 0;
        u->query[0] = 0;
        if (q) ju_put(u->query, JU_PART, q->s, (int)q->len, JU_C0);
        jd_url_store(J, url, u, 0);
    }
    free(u);
}

static jval nat_url_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "URL is made with new", J->error_line);
    jurl *u = (jurl *)malloc(sizeof(jurl));
    jurl *base = (jurl *)malloc(sizeof(jurl));
    jval r = js_undef();
    if (!u || !base) goto out;
    jstr *in = jd_arg_str(J, a, n, 0);
    int have_base = 0;
    if (n > 1 && a[1].t != JS_UNDEF) {
        jstr *b = js_to_str(J, a[1]);
        if (!b || !ju_parse(b->s, 0, base)) { r = js_throw(J, JS_ERR_TYPE, "that base is not an address", J->error_line); goto out; }
        have_base = 1;
    }
    if (!ju_parse(in->s, have_base ? base : 0, u)) {
        char msg[160];
        int w = 0;
        for (const char *p = "that is not an address: "; *p; p++) msg[w++] = *p;
        for (u32 i = 0; i < in->len && w < 150; i++) msg[w++] = in->s[i];
        msg[w] = 0;
        r = js_throw(J, JS_ERR_TYPE, msg, J->error_line);
        goto out;
    }
    jd_url_store(J, t.obj, u, 0);
out:
    free(u);
    free(base);
    return r;
}

static jval nat_url_canparse(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jurl *u = (jurl *)malloc(sizeof(jurl));
    jurl *base = (jurl *)malloc(sizeof(jurl));
    int ok = 0;
    if (u && base) {
        jstr *in = jd_arg_str(J, a, n, 0);
        int bad = 0, have = 0;
        if (n > 1 && a[1].t != JS_UNDEF) {
            jstr *b = js_to_str(J, a[1]);
            if (!b || !ju_parse(b->s, 0, base)) bad = 1;
            else have = 1;
        }
        ok = !bad && ju_parse(in->s, have ? base : 0, u);
    }
    free(u);
    free(base);
    return js_bool(ok);
}

/* One getter for every part, told which by what is kept on it (as the
   reflected attributes are: jd_reflect_as). */
enum { JUP_HREF = 1, JUP_ORIGIN, JUP_PROTOCOL, JUP_USERNAME, JUP_PASSWORD, JUP_HOST, JUP_HOSTNAME,
       JUP_PORT, JUP_PATHNAME, JUP_SEARCH, JUP_HASH };

static jval jd_url_part(jctx *J, const jurl *u, int which) {
    jtext t = { 0, 0, 0, 0 };
    switch (which) {
        case JUP_HREF: return js_from_str(ju_href(J, u));
        case JUP_ORIGIN: return js_from_str(ju_origin(J, u));
        case JUP_PROTOCOL: jd_put(&t, u->scheme); jd_putc(&t, ':'); break;
        case JUP_USERNAME: jd_put(&t, u->user); break;
        case JUP_PASSWORD: jd_put(&t, u->pass); break;
        case JUP_HOST:
            jd_put(&t, u->host);
            if (u->port >= 0) { jd_putc(&t, ':'); jd_put_num(&t, u->port); }
            break;
        case JUP_HOSTNAME: jd_put(&t, u->host); break;
        case JUP_PORT: if (u->port >= 0) jd_put_num(&t, u->port); break;
        case JUP_PATHNAME: jd_put(&t, u->path); break;
        case JUP_SEARCH: if (u->has_query && u->query[0]) { jd_putc(&t, '?'); jd_put(&t, u->query); } break;
        case JUP_HASH: if (u->has_frag && u->frag[0]) { jd_putc(&t, '#'); jd_put(&t, u->frag); } break;
    }
    return js_from_str(jt_done(J, &t));
}

/* A part set: the standard's setters, in the forms pages use. */
static int jd_url_set_part(jctx *J, jurl *u, int which, jstr *v) {
    const char *s = v->s;
    int n = (int)v->len;
    switch (which) {
        case JUP_HREF: {
            jurl *nu = (jurl *)malloc(sizeof(jurl));
            if (!nu) return 0;
            int ok = ju_parse(s, 0, nu);
            if (ok) ju_copy(u, nu);
            free(nu);
            if (!ok) js_throw(J, JS_ERR_TYPE, "that is not an address", J->error_line);
            return ok;
        }
        case JUP_PROTOCOL: {
            char sc[40];
            int k = 0;
            while (k < n && k < 39 && s[k] != ':' && ju_scheme_char(s[k], k == 0)) { sc[k] = w_lower(s[k]); k++; }
            sc[k] = 0;
            if (!k || ju_is_special(sc) != u->special) return 0;
            w_copy(u->scheme, (int)sizeof(u->scheme), sc, (int)sizeof(u->scheme));
            if (u->port == ju_default_port(u->scheme)) u->port = -1;
            return 1;
        }
        case JUP_USERNAME: if (u->has_host && u->host[0]) ju_set(u->user, (int)sizeof(u->user), s, n, JU_USER); return 1;
        case JUP_PASSWORD: if (u->has_host && u->host[0]) ju_set(u->pass, (int)sizeof(u->pass), s, n, JU_USER); return 1;
        case JUP_HOST: case JUP_HOSTNAME: {
            if (u->opaque) return 0;
            int e = 0;
            while (e < n && s[e] != '/' && s[e] != '?' && s[e] != '#' && s[e] != '\\'
                   && !(which == JUP_HOSTNAME && s[e] == ':')) e++;
            int colon = -1;
            if (which == JUP_HOST) for (int i = 0; i < e; i++) if (s[i] == ':') colon = i;
            char host[512];
            if (!ju_host(s, colon >= 0 ? colon : e, host, (int)sizeof(host), u->special)) return 0;
            w_copy(u->host, (int)sizeof(u->host), host, (int)sizeof(u->host));
            u->has_host = 1;
            if (colon >= 0) {
                int v2 = 0, any = 0;
                for (int i = colon + 1; i < e && s[i] >= '0' && s[i] <= '9'; i++) { v2 = v2 * 10 + (s[i] - '0'); any = 1; }
                if (any && v2 <= 65535) u->port = v2 == ju_default_port(u->scheme) ? -1 : v2;
            }
            return 1;
        }
        case JUP_PORT: {
            if (!u->has_host || !u->host[0] || w_same(u->scheme, "file")) return 0;
            if (!n) { u->port = -1; return 1; }
            int v2 = 0, any = 0;
            for (int i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) { v2 = v2 * 10 + (s[i] - '0'); any = 1; if (v2 > 65535) return 0; }
            if (any) u->port = v2 == ju_default_port(u->scheme) ? -1 : v2;
            return 1;
        }
        case JUP_PATHNAME: {
            if (u->opaque) return 0;
            u->path[0] = 0;
            ju_path_append(u, s, n);
            if (u->special && !u->path[0]) { u->path[0] = '/'; u->path[1] = 0; }
            return 1;
        }
        case JUP_SEARCH: {
            if (n && s[0] == '?') { s++; n--; }
            u->has_query = n > 0;
            ju_set(u->query, JU_PART, s, n, u->special ? JU_SQUERY : JU_QUERY);
            return 1;
        }
        case JUP_HASH: {
            if (n && s[0] == '#') { s++; n--; }
            u->has_frag = n > 0;
            ju_set(u->frag, JU_PART, s, n, JU_FRAG);
            return 1;
        }
    }
    return 0;
}

static jval nat_url_get(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int which = J->callee ? J->callee->spare : 0;
    jurl *u = (jurl *)malloc(sizeof(jurl));
    if (!u) return js_undef();
    jval r = jd_url_of(J, t, u) ? jd_url_part(J, u, which) : js_undef();
    free(u);
    return r;
}

static jval nat_url_set(jctx *J, jval t, jval *a, int n) {
    int which = J->callee ? J->callee->spare : 0;
    jstr *v = js_to_str(J, js_arg(a, n, 0));
    if (!v) return js_undef();
    jurl *u = (jurl *)malloc(sizeof(jurl));
    if (!u) return js_undef();
    if (jd_url_of(J, t, u) && jd_url_set_part(J, u, which, v))
        jd_url_store(J, t.obj, u, which == JUP_HREF || which == JUP_SEARCH);
    free(u);
    return js_undef();
}

static jval nat_url_params(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return js_undef();
    jval kept = jd_kept(t.obj, jd_k_params);
    if (kept.t == JS_OBJ) return kept;
    jurl *u = (jurl *)malloc(sizeof(jurl));
    if (!u) return js_undef();
    jval r = js_undef();
    if (jd_url_of(J, t, u)) {
        jobj *p = jd_new_params(J, 0);
        if (p) {
            jobj *pairs = jd_pairs_of(J, js_from_obj(p));
            if (pairs && u->has_query) jd_pairs_parse(J, pairs, u->query, w_len(u->query));
            jd_keep(p, jd_k_owner, t);
            jd_keep(t.obj, jd_k_params, js_from_obj(p));
            r = js_from_obj(p);
        }
    }
    free(u);
    return r;
}

static jval nat_url_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jval h = js_is_obj(t) ? jd_kept(t.obj, jd_k_href) : js_undef();
    if (h.t != JS_STR) return js_throw(J, JS_ERR_TYPE, "that is not a URL", J->error_line);
    return h;
}

static jval nat_link_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int el = jd_el_of(t);
    if (el < 0) return jd_illegal(J);
    const char *h = jd_attr(el, "href");
    char out[URL_TEXT + 256];
    if (!h) return jd_str("");
    return jd_resolve(h, out, (int)sizeof(out)) ? jd_str(out) : jd_str(h);
}

static void jd_url_accessor(jctx *J, jobj *on, const char *name, int which, jnative get, jnative set) {
    jobj *g = js_native(J, name, get);
    jobj *s = set ? js_native(J, name, set) : 0;
    if (!g) return;
    g->flags |= JOF_NOCTOR;
    g->spare = (u16)which;
    if (s) { s->flags |= JOF_NOCTOR; s->spare = (u16)which; }
    js_define_accessor(J, on, js_str(J, name), js_from_obj(g), js_from_obj(s), JP_ENUM | JP_CONF);
}

/* The same parts on a link (a, area): read from its href made whole, and a
   part set writes the href attribute again. */
static int jd_link_url(int el, jurl *u) {
    const char *h = jd_attr(el, "href");
    if (!h) return 0;
    jurl *base = (jurl *)malloc(sizeof(jurl));
    if (!base) return 0;
    int ok = ju_page(base) && ju_parse(h, base, u);
    free(base);
    return ok;
}

static jval nat_link_get(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int which = J->callee ? J->callee->spare : 0;
    int el = jd_el_of(t);
    if (el < 0) return js_undef();
    jurl *u = (jurl *)malloc(sizeof(jurl));
    if (!u) return js_undef();
    jval r;
    if (jd_link_url(el, u)) r = jd_url_part(J, u, which);
    else if (which == JUP_HREF) { const char *h = jd_attr(el, "href"); r = jd_str(h ? h : ""); }
    else r = jd_str("");
    free(u);
    return r;
}

static jval nat_link_set(jctx *J, jval t, jval *a, int n) {
    int which = J->callee ? J->callee->spare : 0;
    int el = jd_el_of(t);
    jstr *v = js_to_str(J, js_arg(a, n, 0));
    if (el < 0 || !v) return js_undef();
    if (which == JUP_HREF) { jd_attr_set(el, "href", v->s); return js_undef(); }
    jurl *u = (jurl *)malloc(sizeof(jurl));
    if (!u) return js_undef();
    if (jd_link_url(el, u) && jd_url_set_part(J, u, which, v)) {
        jstr *h = ju_href(J, u);
        if (h) jd_attr_set(el, "href", h->s);
    }
    free(u);
    return js_undef();
}

static void jd_setup_url(jctx *J) {
    jd_k_pairs = js_sym_new(J, "pairs", 5);
    jd_k_owner = js_sym_new(J, "owner", 5);
    jd_k_href = js_sym_new(J, "href", 4);
    jd_k_params = js_sym_new(J, "searchParams", 12);

    jobj *pp = jd_interface(J, "URLSearchParams", 0, nat_params_ctor, 0);
    jd_p_params = pp;
    jd_method(J, pp, "append", nat_params_append, 2);
    jd_method(J, pp, "delete", nat_params_delete, 1);
    jd_method(J, pp, "get", nat_params_get, 1);
    jd_method(J, pp, "getAll", nat_params_getall, 1);
    jd_method(J, pp, "has", nat_params_has, 1);
    jd_method(J, pp, "set", nat_params_set, 2);
    jd_method(J, pp, "sort", nat_params_sort, 0);
    jd_method(J, pp, "toString", nat_params_tostring, 0);
    jd_method(J, pp, "forEach", nat_params_foreach, 1);
    jd_method(J, pp, "entries", nat_params_entries, 0);
    jd_method(J, pp, "keys", nat_params_keys, 0);
    jd_method(J, pp, "values", nat_params_values, 0);
    js_method_key(J, pp, J->sym_iterator, "[Symbol.iterator]", nat_params_entries, 0);
    jd_accessor(J, pp, "size", nat_params_size, 0);

    jobj *up = jd_interface(J, "URL", 0, nat_url_ctor, 1);
    jd_p_url = up;
    static const struct { const char *name; int which, set; } PARTS[] = {
        { "href", JUP_HREF, 1 }, { "origin", JUP_ORIGIN, 0 }, { "protocol", JUP_PROTOCOL, 1 },
        { "username", JUP_USERNAME, 1 }, { "password", JUP_PASSWORD, 1 }, { "host", JUP_HOST, 1 },
        { "hostname", JUP_HOSTNAME, 1 }, { "port", JUP_PORT, 1 }, { "pathname", JUP_PATHNAME, 1 },
        { "search", JUP_SEARCH, 1 }, { "hash", JUP_HASH, 1 }, { 0, 0, 0 }
    };
    for (int i = 0; PARTS[i].name; i++)
        jd_url_accessor(J, up, PARTS[i].name, PARTS[i].which, nat_url_get, PARTS[i].set ? nat_url_set : 0);
    jd_accessor(J, up, "searchParams", nat_url_params, 0);
    jd_method(J, up, "toString", nat_url_tostring, 0);
    jd_method(J, up, "toJSON", nat_url_tostring, 0);
    jobj *uc = jd_ctor_of(up);
    if (uc) js_method(J, uc, "canParse", nat_url_canparse, 1);

    /* The same parts on links, where href is already the reflected one. */
    const char *links[2] = { "HTMLAnchorElement", "HTMLAreaElement" };
    for (int k = 0; k < 2; k++) {
        jobj *lp = jd_iface(links[k]);
        if (!lp) continue;
        for (int i = 0; PARTS[i].name; i++)
            jd_url_accessor(J, lp, PARTS[i].name, PARTS[i].which, nat_link_get, PARTS[i].set ? nat_link_set : 0);
        jd_method(J, lp, "toString", nat_link_tostring, 0);
    }
}
