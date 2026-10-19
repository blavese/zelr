#pragma once
/* A page asking the network, after it has been read: XMLHttpRequest and
 * fetch, their headers, requests and responses, a form's data as a body,
 * AbortController, and the queue both go through.
 *
 * Included from jsdom.h. Every request is made on the browser's next pass
 * rather than inside the call that asked for it, so the code after send()
 * or fetch() runs first, as the page is written to expect, and a reply
 * arrives the way it would anywhere: afterwards. Where the browser can (it
 * gives jsdom_asks_with), a request is started on one pass and its answer
 * taken on a later one, the page's timers, frames and input going on in
 * between; a browser that cannot (the host's tests) makes each one whole,
 * one a pass, which stalls the page while it comes.
 *
 * What goes out goes through the browser (jd_do_request; browser.c,
 * do_request), which sends the jar's cookies for the address as every
 * request does, the page's address as its Referer as far as the page's
 * policy allows, and Origin where the standard asks for it. The method is
 * any the standard allows (CONNECT, TRACE and TRACK are refused), the
 * headers the page set go with it but for those the standard keeps for the
 * browser (jd_header_forbidden), and a body is bytes, as long as it is. What
 * comes back carries every header the server sent but its cookies.
 */

/* One reply, as the browser hands it back. */
typedef struct {
    const char *body;
    int  len;
    int  status;
    const char *type;            /* the Content-Type it came with, or none */
    const char *url;             /* where it ended up, after redirects */
    const char *head;            /* the status line and headers as they came, or none */
    int  hlen;
} jd_reply;

/* How one is made. Set by the browser, for the same reason the script
   fetch is: the network and the page's address are its business. `body` is
   blen bytes; `type` is what it is, "" for nothing said; `headers` are the
   page's own, whole lines each ending CRLF. */
typedef int (*jd_request_fn)(const char *method, const char *url, const char *body, int blen,
                             const char *type, const char *headers, jd_reply *out);
static jd_request_fn jd_do_request;

void jsdom_request_with(jd_request_fn fn) { jd_do_request = fn; }

/* And the same without waiting: start one (an id, -1 when every way out is
   busy and it should be asked again on a later pass, or below -1 for a
   failure to send), ask how it is going (0 still coming, 1 there, below 0
   failed; *out good until it is ended), and end it (also one still coming).
   `max` is the most its answer may be. */
typedef int  (*jd_ask_start_fn)(const char *method, const char *url, const char *body, int blen,
                                const char *type, const char *headers, int max);
typedef int  (*jd_ask_poll_fn)(int ask, jd_reply *out);
typedef void (*jd_ask_end_fn)(int ask);
static jd_ask_start_fn jd_ask_start;
static jd_ask_poll_fn  jd_ask_poll;
static jd_ask_end_fn   jd_ask_end;

/* Set while a media element asks for a stretch of its file, which only the
   decoder reads. The browser lets that reach a file beside a page on the
   disk, and nothing else a script asks for (browser.c script_may_ask). */
static int jd_asking_media;

void jsdom_asks_with(jd_ask_start_fn start, jd_ask_poll_fn poll, jd_ask_end_fn end) {
    jd_ask_start = start;
    jd_ask_poll = poll;
    jd_ask_end = end;
}

#define JD_REPLY_MAX (4 * 1024 * 1024)

/* --- the queue ------------------------------------------------------------------------- */

#define JD_REQUESTS 32
enum { JQ_XHR = 1, JQ_FETCH, JQ_BEACON };

typedef struct {
    jobj *self;                  /* the request, which holds its own state */
    int   kind, waiting;
    int   ask;                   /* the browser's id for it while it is under way, plus one; 0 for none */
} jxhr;

static jxhr jd_req[JD_REQUESTS];
static int  jd_nreq;

/* --- the page's own site first ------------------------------------------------------------
 *
 * Three ways out are shared by everything a page asks for once it is up, and
 * on a page that carries advertising most of what it asks for is someone
 * else's: Imgur's gallery asked for its posts in its first seconds and the
 * request waited behind trackers and ad scripts until the twenty-fifth. A
 * browser keeps connections per host, so another host's queue never holds
 * up the page's own. Here what is waiting goes in two rounds, the page's own
 * site first.
 *
 * The site is the host's last two labels, or three when the last is a
 * two-letter country and the one before it is short (bbc.co.uk, abc.net.au).
 * That is a guess at the public suffix list, which is not here, and all it
 * decides is which request goes first. */
static const char *jd_site_part(const char *host) {
    int n = w_len(host);
    while (n > 0 && host[n - 1] == '.') n--;
    int dots[3] = { -1, -1, -1 }, nd = 0;
    for (int i = n - 1; i >= 0 && nd < 3; i--) if (host[i] == '.') dots[nd++] = i;
    if (nd < 1) return host;
    int last = n - dots[0] - 1;
    int second = nd >= 2 ? dots[0] - dots[1] - 1 : dots[0];
    if (last == 2 && second <= 3 && nd >= 2) return host + (nd >= 3 ? dots[2] + 1 : 0);
    return host + (nd >= 2 ? dots[1] + 1 : 0);
}

/* Whether an address is on the page's own site; one with no host of its own
   (a path) is. */
static int jd_same_site(const char *url) {
    if (!url || !*url) return 1;
    const char *p = url;
    while (*p && *p != ':' && *p != '/' && *p != '?' && *p != '#') p++;
    int bare = url[0] == '/' && url[1] == '/';
    if (!(p[0] == ':' && p[1] == '/' && p[2] == '/') && !bare) return 1;
    static char whole[URL_TEXT + 8];
    if (bare) {
        w_copy(whole, (int)sizeof(whole), "https:", (int)sizeof(whole));
        w_copy(whole + 6, (int)sizeof(whole) - 6, url, (int)sizeof(whole) - 6);
        url = whole;
    }
    url_t u, page;
    if (!url_parse(url, &u) || !url_parse(jd_address, &page)) return 0;
    return w_same_fold(jd_site_part(u.host), jd_site_part(page.host));
}

/* --- scripts a page puts in, fetched while it goes on -------------------------------------
 *
 * A script element a page's script inserts with a src was fetched on the
 * next pass, and the fetch waited: nothing else on the page happened until
 * the file had come. An ad loader inserts dozens, one after another, and on
 * Imgur the gallery's own requests waited behind them for most of a minute
 * (the program, sampled, was waiting on the network nine looks in ten).
 * Browsers fetch such a script without waiting and run it when it comes,
 * in whatever order they come in; so does this now, when the browser gives
 * it a way (jsdom_script_asks_with): the fetch is one of the page's asks,
 * started on one pass and run, then told load or error, on the pass its
 * answer is whole. Without one (the tests' stand-ins) the old way stands. */
typedef int  (*jd_sask_start_fn)(const char *src);
typedef int  (*jd_sask_poll_fn)(int ask, const char **text);
typedef void (*jd_sask_end_fn)(int ask);
static jd_sask_start_fn jd_sask_start;
static jd_sask_poll_fn  jd_sask_poll;
static jd_sask_end_fn   jd_sask_end;

/* start: an id, -1 when every way out is busy (asked again on a later
   pass), below -1 when it will not come; poll: 0 still coming, the length
   once it is whole (the text good until it is ended), below 0 when it
   failed; end: done with, whole or not. */
void jsdom_script_asks_with(jd_sask_start_fn start, jd_sask_poll_fn poll, jd_sask_end_fn end) {
    jd_sask_start = start;
    jd_sask_poll = poll;
    jd_sask_end = end;
}

#define JD_SCRIPT_ASKS 128
typedef struct {
    int node;                    /* the script element */
    int ask;                     /* the browser's id plus one while it is coming; 0 before it is asked */
} jsask;
static jsask jd_sq[JD_SCRIPT_ASKS];
static int   jd_nsq;

/* Queued to be fetched without waiting: 1, or 0 when there is no way to (or
   no room), and the caller fetches it the old way. */
static int jd_script_queue(int node) {
    if (!jd_sask_start || !jd_sask_poll || jd_nsq >= JD_SCRIPT_ASKS) return 0;
    jd_sq[jd_nsq].node = node;
    jd_sq[jd_nsq].ask = 0;
    jd_nsq++;
    return 1;
}

static void jd_script_unqueue(int k) {
    for (int m = k; m + 1 < jd_nsq; m++) jd_sq[m] = jd_sq[m + 1];
    jd_nsq--;
}

static void jd_scripts_drop(void) {
    for (int k = 0; k < jd_nsq; k++)
        if (jd_sq[k].ask && jd_sask_end) jd_sask_end(jd_sq[k].ask - 1);
    jd_nsq = 0;
}

static int jd_scripts_waiting(void) {
    for (int k = 0; k < jd_nsq; k++) if (!jd_sq[k].ask) return 1;
    return 0;
}

static int jd_requests_waiting(void) {
    for (int i = 0; i < jd_nreq; i++) if (jd_req[i].waiting) return 1;
    return jd_scripts_waiting();
}

/* Every way out was busy on the last pass, so what waits is asked again
   on the next tick rather than at once. */
static int jd_asks_full;

/* Requests started and not answered yet, which want asking each tick. */
static int jd_requests_under_way(void) {
    for (int i = 0; i < jd_nreq; i++) if (jd_req[i].ask) return 1;
    for (int k = 0; k < jd_nsq; k++) if (jd_sq[k].ask) return 1;
    return 0;
}

/* One under way given up on: aborted, or the page gone. */
static void jd_request_drop(int i) {
    if (jd_req[i].ask && jd_ask_end) jd_ask_end(jd_req[i].ask - 1);
    jd_req[i].ask = 0;
}

static int jd_queue(jobj *self, int kind) {
    int slot = -1;
    for (int i = 0; i < jd_nreq; i++) if (!jd_req[i].waiting && !jd_req[i].ask) { slot = i; break; }
    if (slot < 0 && jd_nreq < JD_REQUESTS) slot = jd_nreq++;
    if (slot < 0) return 0;
    jd_req[slot].self = self;
    jd_req[slot].kind = kind;
    jd_req[slot].waiting = 1;
    return 1;
}

/* What a request carries, kept under symbols on its object. */
static jstr *jd_k_method, *jd_k_url, *jd_k_body, *jd_k_type, *jd_k_promise, *jd_k_rsig,
            *jd_k_hlist, *jd_k_rbody, *jd_k_rheaders;

static const char *jd_kept_str(jobj *o, jstr *key, const char *fallback) {
    jval v = jd_kept(o, key);
    return v.t == JS_STR ? v.str->s : fallback;
}

/* A method as the standard takes one (Fetch, "normalize"): DELETE, GET,
   HEAD, OPTIONS, POST and PUT in capitals whatever their case, any other
   token as written. 0 for one that is not a token; *forbidden for CONNECT,
   TRACE and TRACK, which a page may not send. */
static jstr *jd_method_norm(jctx *J, jstr *m, int *forbidden) {
    *forbidden = 0;
    if (!m || !m->len || m->len > 32) return 0;
    for (u32 i = 0; i < m->len; i++) {
        u8 c = (u8)m->s[i];
        if (c <= 0x20 || c >= 0x7F) return 0;
        for (const char *p = "()<>@,;:\\\"/[]?={}"; *p; p++) if (c == (u8)*p) return 0;
    }
    static const char *const UP[] = { "DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT", 0 };
    for (int i = 0; UP[i]; i++) if (w_len(UP[i]) == (int)m->len && w_starts_fold(m->s, UP[i])) return js_str(J, UP[i]);
    if (w_same_fold(m->s, "CONNECT") || w_same_fold(m->s, "TRACE") || w_same_fold(m->s, "TRACK")) *forbidden = 1;
    return m;
}

/* The headers the standard keeps for the browser to send (Fetch, "forbidden
   request-header"): a page setting one is ignored, as a browser ignores it. */
static int jd_header_forbidden(const char *name, int n) {
    static const char *const NO[] = { "accept-charset", "accept-encoding", "access-control-request-headers",
        "access-control-request-method", "connection", "content-length", "cookie", "cookie2", "date", "dnt",
        "expect", "host", "keep-alive", "origin", "referer", "set-cookie", "te", "trailer",
        "transfer-encoding", "upgrade", "via", "x-http-method", "x-http-method-override",
        "x-method-override", 0 };
    for (int i = 0; NO[i]; i++) if (w_len(NO[i]) == n && w_starts_fold(name, NO[i])) return 1;
    return (n >= 6 && w_starts_fold(name, "proxy-")) || (n >= 4 && w_starts_fold(name, "sec-"));
}

/* A request's headers as the lines that go out: each pair the page set,
   but the forbidden ones, Content-Type (sent as the body's type) and any
   whose name is not a token or whose value would end the line. */
static int jd_header_lines(jobj *list, char *out, int cap) {
    int w = 0;
    out[0] = 0;
    for (u32 i = 0; list && i < list->len; i++) {
        jval pv = list->items[i];
        if (!js_is_obj(pv) || pv.obj->len < 2 || pv.obj->items[0].t != JS_STR || pv.obj->items[1].t != JS_STR) continue;
        jstr *name = pv.obj->items[0].str, *value = pv.obj->items[1].str;
        if (!name->len || jd_header_forbidden(name->s, (int)name->len) || js_str_is(name, "content-type")) continue;
        int bad = 0;
        for (u32 k = 0; k < name->len && !bad; k++) {
            u8 c = (u8)name->s[k];
            if (c <= 0x20 || c >= 0x7F || c == ':') bad = 1;
        }
        for (u32 k = 0; k < value->len && !bad; k++) if (value->s[k] == '\r' || value->s[k] == '\n' || !value->s[k]) bad = 1;
        if (bad || w + (int)name->len + (int)value->len + 5 >= cap) continue;
        for (u32 k = 0; k < name->len; k++) out[w++] = name->s[k];
        out[w++] = ':';
        out[w++] = ' ';
        for (u32 k = 0; k < value->len; k++) out[w++] = value->s[k];
        out[w++] = '\r';
        out[w++] = '\n';
    }
    out[w] = 0;
    return w;
}

/* --- Headers ----------------------------------------------------------------------------
 *
 * Names in lower case, values as given, a name set twice kept twice and
 * read back joined, as the standard has it. */
static jobj *jd_p_headers, *jd_p_request, *jd_p_response, *jd_p_formdata;

static jobj *jd_hlist_of(jctx *J, jval t) {
    jval v = js_is_obj(t) ? jd_kept(t.obj, jd_k_hlist) : js_undef();
    if (v.t == JS_OBJ && v.obj->kind == JO_ARRAY) return v.obj;
    js_throw(J, JS_ERR_TYPE, "that is not a Headers", J->error_line);
    return 0;
}

static jstr *jd_lower_str(jctx *J, jstr *s) {
    char *b = (char *)malloc((u64)s->len + 1);
    if (!b) return s;
    for (u32 i = 0; i < s->len; i++) b[i] = w_lower(s->s[i]);
    jstr *r = js_str_n(J, b, s->len);
    free(b);
    return r ? r : s;
}

static void jd_hlist_add(jctx *J, jobj *list, jstr *name, jstr *value) {
    jobj *pair = js_array(J);
    if (!pair) return;
    js_arr_push(J, pair, js_from_str(jd_lower_str(J, name)));
    js_arr_push(J, pair, js_from_str(value));
    js_arr_push(J, list, js_from_obj(pair));
}

static jobj *jd_new_headers(jctx *J) {
    jobj *h = js_object_with(J, JO_PLAIN, jd_p_headers);
    jobj *list = js_array(J);
    if (!h || !list) return 0;
    jd_keep(h, jd_k_hlist, js_from_obj(list));
    return h;
}

/* Headers from whatever a page hands over: another Headers, pairs, or a
   record. */
static void jd_headers_fill(jctx *J, jobj *list, jval init) {
    if (!js_is_obj(init)) return;
    jval other = jd_kept(init.obj, jd_k_hlist);
    if (other.t == JS_OBJ) {
        for (u32 i = 0; i < other.obj->len; i++) {
            jobj *p = other.obj->items[i].obj;
            jd_hlist_add(J, list, p->items[0].str, p->items[1].str);
        }
        return;
    }
    jval it = js_get(J, init, J->sym_iterator);
    if (J->sig != JS_OK) return;
    if (js_callable(it)) {
        jargs A;
        js_args_init(&A);
        if (js_iter_collect(J, init, &A))
            for (int i = 0; i < A.n && J->sig == JS_OK; i++) {
                jargs B;
                js_args_init(&B);
                if (js_iter_collect(J, A.v[i], &B) && B.n == 2)
                    jd_hlist_add(J, list, js_to_str(J, B.v[0]), js_to_str(J, B.v[1]));
                js_args_free(&B);
            }
        js_args_free(&A);
        return;
    }
    jprop **keys;
    u32 nk = js_own_keys(J, init.obj, &keys);
    for (u32 i = 0; i < nk && J->sig == JS_OK; i++)
        jd_hlist_add(J, list, keys[i]->key, js_to_str(J, js_get(J, init, keys[i]->key)));
}

static jval nat_headers_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "Headers is made with new", J->error_line);
    jobj *list = js_array(J);
    if (!list) return js_undef();
    jd_keep(t.obj, jd_k_hlist, js_from_obj(list));
    jd_headers_fill(J, list, js_arg(a, n, 0));
    return js_undef();
}

static jval jd_headers_get(jctx *J, jobj *list, jstr *name) {
    jstr *low = jd_lower_str(J, name);
    jtext t = { 0, 0, 0, 0 };
    int any = 0;
    for (u32 i = 0; i < list->len; i++) {
        jobj *p = list->items[i].obj;
        if (!js_str_eq(p->items[0].str, low)) continue;
        if (any) jd_put(&t, ", ");
        jt_put(J, &t, p->items[1].str->s, p->items[1].str->len);
        any = 1;
    }
    if (!any) return js_null();
    return js_from_str(jt_done(J, &t));
}

static jval nat_headers_get(jctx *J, jval t, jval *a, int n) {
    jobj *list = jd_hlist_of(J, t);
    return list ? jd_headers_get(J, list, jd_arg_str(J, a, n, 0)) : js_undef();
}

static jval nat_headers_has(jctx *J, jval t, jval *a, int n) {
    jobj *list = jd_hlist_of(J, t);
    return list ? js_bool(jd_headers_get(J, list, jd_arg_str(J, a, n, 0)).t == JS_STR) : js_undef();
}

static void jd_headers_remove(jctx *J, jobj *list, jstr *name) {
    jstr *low = jd_lower_str(J, name);
    u32 w = 0;
    for (u32 i = 0; i < list->len; i++)
        if (!js_str_eq(list->items[i].obj->items[0].str, low)) list->items[w++] = list->items[i];
    list->len = w;
}

static jval nat_headers_append(jctx *J, jval t, jval *a, int n) {
    jobj *list = jd_hlist_of(J, t);
    if (list) jd_hlist_add(J, list, jd_arg_str(J, a, n, 0), js_to_str(J, js_arg(a, n, 1)));
    return js_undef();
}

static jval nat_headers_set(jctx *J, jval t, jval *a, int n) {
    jobj *list = jd_hlist_of(J, t);
    if (!list) return js_undef();
    jstr *name = jd_arg_str(J, a, n, 0);
    jd_headers_remove(J, list, name);
    jd_hlist_add(J, list, name, js_to_str(J, js_arg(a, n, 1)));
    return js_undef();
}

static jval nat_headers_delete(jctx *J, jval t, jval *a, int n) {
    jobj *list = jd_hlist_of(J, t);
    if (list) jd_headers_remove(J, list, jd_arg_str(J, a, n, 0));
    return js_undef();
}

/* Walked in name order, each name once with its values joined, as the
   standard walks them. */
static jobj *jd_headers_sorted(jctx *J, jobj *list) {
    jobj *out = js_array(J);
    for (u32 i = 0; out && i < list->len; i++) {
        jstr *name = list->items[i].obj->items[0].str;
        int seen = 0;
        for (u32 k = 0; k < out->len && !seen; k++) if (js_str_eq(out->items[k].obj->items[0].str, name)) seen = 1;
        if (seen) continue;
        jobj *pair = js_array(J);
        js_arr_push(J, pair, js_from_str(name));
        js_arr_push(J, pair, jd_headers_get(J, list, name));
        u32 at = out->len;
        js_arr_push(J, out, js_from_obj(pair));
        while (at > 0 && jd_str_before(name, out->items[at - 1].obj->items[0].str)) {
            jval tmp = out->items[at];
            out->items[at] = out->items[at - 1];
            out->items[at - 1] = tmp;
            at--;
        }
    }
    return out;
}

static jval jd_headers_iter(jctx *J, jval t, int which) {
    jobj *list = jd_hlist_of(J, t);
    if (!list) return js_undef();
    jobj *sorted = jd_headers_sorted(J, list);
    if (!sorted) return js_undef();
    if (which) for (u32 i = 0; i < sorted->len; i++) sorted->items[i] = sorted->items[i].obj->items[which == 1 ? 0 : 1];
    return jd_array_iter(J, sorted, "values");
}

static jval nat_headers_entries(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_headers_iter(J, t, 0); }
static jval nat_headers_keys(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_headers_iter(J, t, 1); }
static jval nat_headers_values(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_headers_iter(J, t, 2); }

static jval nat_headers_foreach(jctx *J, jval t, jval *a, int n) {
    jobj *list = jd_hlist_of(J, t);
    jval fn = js_arg(a, n, 0);
    if (!list) return js_undef();
    if (!js_callable(fn)) return js_throw(J, JS_ERR_TYPE, "forEach needs a function", J->error_line);
    jobj *sorted = jd_headers_sorted(J, list);
    for (u32 i = 0; sorted && i < sorted->len && J->sig == JS_OK; i++) {
        jval args[3] = { sorted->items[i].obj->items[1], sorted->items[i].obj->items[0], t };
        js_call(J, fn, js_arg(a, n, 1), args, 3);
    }
    return js_undef();
}

static jval nat_headers_setcookie(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    return js_from_obj(js_array(J));          /* a script is never shown a Set-Cookie */
}

/* --- a body -----------------------------------------------------------------------------
 *
 * What a page may send: text, URLSearchParams as a form, FormData as one
 * too, or bytes. Returned as the text to send and what it is. */
static jstr *jd_k_fpairs;

static int jd_is_formdata(jval v) { return js_is_obj(v) && js_find(v.obj, jd_k_fpairs) != 0; }

static int jd_blob_parts(jval v, jstr **bytes, jstr **type);     /* jswin.h */
static int jd_blob_lookup(const char *url, jstr **bytes, jstr **type);

static jstr *jd_body_of(jctx *J, jval b, const char **type) {
    *type = 0;
    if (b.t == JS_UNDEF || b.t == JS_NULL) return 0;
    jstr *bb, *bt;
    if (jd_blob_parts(b, &bb, &bt)) {
        *type = bt && bt->len ? bt->s : 0;
        return bb;
    }
    /* FormData before URLSearchParams: it keeps its pairs the same way. */
    if (!jd_is_formdata(b) && jd_is_search_params_obj(b)) {
        *type = "application/x-www-form-urlencoded;charset=UTF-8";
        return js_to_str(J, b);
    }
    if (jd_is_formdata(b)) {
        /* Sent as a form rather than as multipart parts: what it holds here
           is only ever text, and a form is what every server reads. */
        jval pairs = jd_kept(b.obj, jd_k_fpairs);
        *type = "application/x-www-form-urlencoded;charset=UTF-8";
        return jd_pairs_text(J, pairs.obj);
    }
    const u8 *bytes;
    u32 blen;
    if (js_is_obj(b) && tx_bytes_of(b, &bytes, &blen)) {
        *type = 0;                       /* bytes say nothing of what they are */
        return js_str_n(J, (const char *)bytes, blen);
    }
    *type = "text/plain;charset=UTF-8";
    return js_to_str(J, b);
}

/* --- FormData ------------------------------------------------------------------------------- */

/* A form's controls as its submission would send them: the named ones,
   boxes only when ticked, buttons not at all. */
static void jd_formdata_from(jctx *J, jobj *pairs, int form) {
    for (int i = jd_walk_first(form); i >= 0; i = jd_walk_next(i, form)) {
        if (!jd_is_element(i) || !jd_is_form_control(i)) continue;
        const char *name = jd_attr(i, "name");
        if (!name || !*name || jd_attr(i, "disabled")) continue;
        int tag = jd_doc->nodes[i].tag;
        const char *ty = jd_attr(i, "type");
        if (tag == T_BUTTON || (tag == T_INPUT && ty && (w_same_fold(ty, "submit") || w_same_fold(ty, "button")
                                                       || w_same_fold(ty, "reset") || w_same_fold(ty, "image")
                                                       || w_same_fold(ty, "file")))) continue;
        if (tag == T_INPUT && ty && (w_same_fold(ty, "checkbox") || w_same_fold(ty, "radio"))) {
            const char *c = jd_attr(i, "checked");
            if (!c || w_same(c, "0")) continue;
        }
        jobj *pair = js_array(J);
        js_arr_push(J, pair, jd_str(name));
        js_arr_push(J, pair, nat_value(J, jd_el_value(J, i), 0, 0));
        js_arr_push(J, pairs, js_from_obj(pair));
    }
}

static jval nat_formdata_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "FormData is made with new", J->error_line);
    jobj *pairs = js_array(J);
    if (!pairs) return js_undef();
    jd_keep(t.obj, jd_k_fpairs, js_from_obj(pairs));
    /* URLSearchParams' methods work on these pairs too. */
    jd_keep(t.obj, jd_k_pairs, js_from_obj(pairs));
    int form = jd_el_of(js_arg(a, n, 0));
    if (form >= 0 && jd_doc->nodes[form].tag == T_FORM) jd_formdata_from(J, pairs, form);
    return js_undef();
}

static void jd_setup_formdata(jctx *J) {
    jd_p_formdata = jd_interface(J, "FormData", 0, nat_formdata_ctor, 0);
    jd_method(J, jd_p_formdata, "append", nat_params_append, 2);
    jd_method(J, jd_p_formdata, "delete", nat_params_delete, 1);
    jd_method(J, jd_p_formdata, "get", nat_params_get, 1);
    jd_method(J, jd_p_formdata, "getAll", nat_params_getall, 1);
    jd_method(J, jd_p_formdata, "has", nat_params_has, 1);
    jd_method(J, jd_p_formdata, "set", nat_params_set, 2);
    jd_method(J, jd_p_formdata, "forEach", nat_params_foreach, 1);
    jd_method(J, jd_p_formdata, "entries", nat_params_entries, 0);
    jd_method(J, jd_p_formdata, "keys", nat_params_keys, 0);
    jd_method(J, jd_p_formdata, "values", nat_params_values, 0);
    js_method_key(J, jd_p_formdata, J->sym_iterator, "[Symbol.iterator]", nat_params_entries, 0);
}

/* --- AbortController and AbortSignal ------------------------------------------------------- */

static jobj *jd_p_signal;
static jstr *jd_k_aborted, *jd_k_reason;

static void jd_signal_abort(jctx *J, jobj *sig, jval reason);

static jobj *jd_new_signal(jctx *J) {
    jobj *s = js_object_with(J, JO_PLAIN, jd_p_signal);
    if (s) jd_keep(s, jd_k_aborted, js_bool(0));
    return s;
}

static int jd_signal_aborted(jobj *s) {
    jval v = s ? jd_kept(s, jd_k_aborted) : js_undef();
    return v.t == JS_BOOL && v.b;
}

static jval jd_abort_error(jctx *J, const char *name, const char *what) {
    jobj *e = js_domexc_new(J, 0, js_str(J, what), js_str(J, name));
    return e ? js_from_obj(e) : js_undef();
}

static void jd_fetch_aborted(jctx *J, jobj *sig);

static void jd_signal_abort(jctx *J, jobj *sig, jval reason) {
    if (!sig || jd_signal_aborted(sig)) return;
    if (reason.t == JS_UNDEF) reason = jd_abort_error(J, "AbortError", "the operation was aborted");
    jd_keep(sig, jd_k_aborted, js_bool(1));
    jd_keep(sig, jd_k_reason, reason);
    jd_listeners_aborted(sig);
    jd_fetch_aborted(J, sig);
    jobj *ev = jd_new_event(0, "abort", 0, 0);
    if (ev) {
        js_set(J, ev, "isTrusted", js_bool(1));
        jd_dispatch_to(ev, js_from_obj(sig), js_from_obj(sig));
    }
}

static jval nat_signal_aborted(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_bool(js_is_obj(t) && jd_signal_aborted(t.obj));
}

static jval nat_signal_reason(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_is_obj(t) ? jd_kept(t.obj, jd_k_reason) : js_undef();
}

static jval nat_signal_throw(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t) || !jd_signal_aborted(t.obj)) return js_undef();
    J->ret = jd_kept(t.obj, jd_k_reason);
    J->sig = JS_THROWN;
    js_note_thrown(J, J->ret, J->error_line);
    return js_undef();
}

static jval nat_signal_abort_static(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *s = jd_new_signal(J);
    if (s) jd_signal_abort(J, s, js_arg(a, n, 0));
    return js_from_obj(s);
}

static void jd_signal_timeout_due(jval arg) {
    if (js_is_obj(arg))
        jd_signal_abort(&jd_J, arg.obj, jd_abort_error(&jd_J, "TimeoutError", "the time given ran out"));
}

static jval nat_signal_timeout(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *s = jd_new_signal(J);
    if (s) jd_later_native(jd_signal_timeout_due, js_from_obj(s), jd_ticks_for(js_to_num(J, js_arg(a, n, 0))));
    return js_from_obj(s);
}

static jval nat_controller_ctor(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "AbortController is made with new", J->error_line);
    jobj *s = jd_new_signal(J);
    if (s) js_const_prop(J, t.obj, "signal", js_from_obj(s));
    return js_undef();
}

static jval nat_controller_abort(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jval s = js_get(J, t, js_str(J, "signal"));
    if (js_is_obj(s)) jd_signal_abort(J, s.obj, js_arg(a, n, 0));
    return js_undef();
}

/* --- Request and Response ------------------------------------------------------------------ */

static jobj *jd_make_request(jctx *J, jobj *proto, jval input, jval init) {
    jobj *r = js_object_with(J, JO_PLAIN, proto ? proto : jd_p_request);
    if (!r) return 0;
    jstr *url = 0, *method = js_str(J, "GET"), *body = 0;
    const char *type = 0;
    jobj *headers = jd_new_headers(J);
    jval signal = js_undef();
    if (js_is_obj(input) && js_find(input.obj, jd_k_url)) {
        /* Another Request: what it holds, before init says otherwise. */
        url = jd_kept(input.obj, jd_k_url).str;
        method = jd_kept(input.obj, jd_k_method).str;
        jval b = jd_kept(input.obj, jd_k_body);
        if (b.t == JS_STR) body = b.str;
        jval h = js_get(J, input, js_str(J, "headers"));
        if (headers && js_is_obj(h)) jd_headers_fill(J, jd_hlist_of(J, js_from_obj(headers)), h);
        signal = jd_kept(input.obj, jd_k_rsig);
    } else {
        jstr *s = js_to_str(J, input);
        if (!s) return 0;
        jstr *whole = jd_resolve_str(J, s->s);
        url = whole ? whole : s;
    }
    if (js_is_obj(init)) {
        jval m = js_get(J, init, js_str(J, "method"));
        if (m.t != JS_UNDEF) {
            int forbidden;
            jstr *ms = jd_method_norm(J, js_to_str(J, m), &forbidden);
            if (J->sig != JS_OK) return 0;
            if (!ms) { js_throw(J, JS_ERR_TYPE, "that is not a method", J->error_line); return 0; }
            if (forbidden) { js_throw(J, JS_ERR_TYPE, "that method may not be sent", J->error_line); return 0; }
            method = ms;
        }
        jval h = js_get(J, init, js_str(J, "headers"));
        if (headers && js_is_obj(h)) {
            jobj *list = jd_hlist_of(J, js_from_obj(headers));
            list->len = 0;
            jd_headers_fill(J, list, h);
        }
        jval b = js_get(J, init, js_str(J, "body"));
        if (b.t != JS_UNDEF && b.t != JS_NULL) body = jd_body_of(J, b, &type);
        jval sg = js_get(J, init, js_str(J, "signal"));
        if (sg.t != JS_UNDEF) signal = sg;
    }
    if (J->sig != JS_OK) return 0;
    /* A type the page set wins over the one the body implies. */
    if (headers && type) {
        jobj *list = jd_hlist_of(J, js_from_obj(headers));
        if (jd_headers_get(J, list, js_str(J, "content-type")).t != JS_STR)
            jd_hlist_add(J, list, js_str(J, "content-type"), js_str(J, type));
    }
    jd_keep(r, jd_k_url, js_from_str(url));
    jd_keep(r, jd_k_method, js_from_str(method));
    if (body) jd_keep(r, jd_k_body, js_from_str(body));
    if (js_is_obj(signal)) jd_keep(r, jd_k_rsig, signal);
    js_const_prop(J, r, "headers", js_from_obj(headers));
    return r;
}

static jval nat_request_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "Request is made with new", J->error_line);
    jobj *r = jd_make_request(J, t.obj->proto, js_arg(a, n, 0), js_arg(a, n, 1));
    return r ? js_from_obj(r) : js_undef();
}

static jval nat_request_url(jctx *J, jval t, jval *a, int n) { (void)J; (void)a; (void)n; return js_is_obj(t) ? jd_kept(t.obj, jd_k_url) : js_undef(); }
static jval nat_request_method(jctx *J, jval t, jval *a, int n) { (void)J; (void)a; (void)n; return js_is_obj(t) ? jd_kept(t.obj, jd_k_method) : js_undef(); }

static jval nat_request_signal(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return js_undef();
    jval s = jd_kept(t.obj, jd_k_rsig);
    if (js_is_obj(s)) return s;
    jobj *fresh = jd_new_signal(J);
    if (fresh) jd_keep(t.obj, jd_k_rsig, js_from_obj(fresh));
    return js_from_obj(fresh);
}

/* A response's facts, kept under keys no script can name and read through
   accessors on the prototype, where the standard puts them: Transcend's
   consent manager takes the getter of url off Response.prototype, and
   stopped on Vimeo finding none. */
static const char *const JD_RESP_FIELDS[] = { "status", "ok", "statusText", "headers", "url", "redirected", "type",
                                              "bodyUsed", 0 };
enum { JRF_STATUS, JRF_OK, JRF_STEXT, JRF_HEADERS, JRF_URL, JRF_REDIR, JRF_TYPE, JRF_USED, JRF_FIELDS };
static jstr *jd_k_resp[JRF_FIELDS];

static jval nat_response_field(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    return js_is_obj(t) ? jd_kept(t.obj, jd_k_resp[(int)J->callee->data.num]) : js_undef();
}

/* A response's body: text, parsed JSON, or bytes, read once. */
static jval jd_response_body(jctx *J, jval t) {
    if (!js_is_obj(t)) return js_undef();
    if (js_to_bool(jd_kept(t.obj, jd_k_resp[JRF_USED]))) {
        js_throw(J, JS_ERR_TYPE, "that response's body has been read already", J->error_line);
        return js_undef();
    }
    jd_keep(t.obj, jd_k_resp[JRF_USED], js_bool(1));
    jval b = jd_kept(t.obj, jd_k_rbody);
    return b.t == JS_STR ? b : jd_str("");
}

static jval jd_promise_from(jctx *J, jval v) {
    jobj *p = js_promise_new(J);
    if (!p) return js_undef();
    if (J->sig == JS_THROWN) {
        jval e = J->ret;
        J->sig = JS_OK;
        js_promise_settle(J, p, 0, e);
    } else {
        js_promise_settle(J, p, 1, v);
    }
    return js_from_obj(p);
}

static jval nat_response_text(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    return jd_promise_from(J, jd_response_body(J, t));
}

static jval nat_response_json(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jval b = jd_response_body(J, t);
    if (J->sig == JS_OK) b = nat_json_parse(J, js_undef(), &b, 1);
    return jd_promise_from(J, b);
}

static jval nat_response_buffer(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jval b = jd_response_body(J, t);
    if (J->sig != JS_OK) return jd_promise_from(J, js_undef());
    jobj *buf = ta_new_buffer(J, J->p_buffer, b.t == JS_STR ? b.str->len : 0);
    if (buf && b.t == JS_STR) {
        u8 *d = ta_bytes(buf);
        for (u32 i = 0; i < b.str->len; i++) d[i] = (u8)b.str->s[i];
    }
    return jd_promise_from(J, js_from_obj(buf));
}

static jobj *jd_make_response(jctx *J, jobj *proto, jstr *body, int status, jstr *status_text,
                              jobj *headers, jstr *url, int redirected) {
    jobj *r = js_object_with(J, JO_PLAIN, proto ? proto : jd_p_response);
    if (!r) return 0;
    jd_keep(r, jd_k_rbody, body ? js_from_str(body) : jd_str(""));
    jd_keep(r, jd_k_resp[JRF_STATUS], js_num(status));
    jd_keep(r, jd_k_resp[JRF_OK], js_bool(status >= 200 && status < 300));
    jd_keep(r, jd_k_resp[JRF_STEXT], js_from_str(status_text ? status_text : js_str(J, "")));
    jd_keep(r, jd_k_resp[JRF_HEADERS], js_from_obj(headers ? headers : jd_new_headers(J)));
    jd_keep(r, jd_k_resp[JRF_URL], js_from_str(url ? url : js_str(J, "")));
    jd_keep(r, jd_k_resp[JRF_REDIR], js_bool(redirected));
    jd_keep(r, jd_k_resp[JRF_TYPE], jd_str(status ? "basic" : "error"));
    jd_keep(r, jd_k_resp[JRF_USED], js_bool(0));
    return r;
}

static jval nat_response_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "Response is made with new", J->error_line);
    const char *type;
    jstr *body = jd_body_of(J, js_arg(a, n, 0), &type);
    jval init = js_arg(a, n, 1);
    int status = 200;
    jstr *st = 0;
    jobj *headers = jd_new_headers(J);
    if (js_is_obj(init)) {
        jval s = js_get(J, init, js_str(J, "status"));
        if (s.t != JS_UNDEF) status = (int)js_to_num(J, s);
        jval x = js_get(J, init, js_str(J, "statusText"));
        if (x.t != JS_UNDEF) st = js_to_str(J, x);
        jval h = js_get(J, init, js_str(J, "headers"));
        if (headers && js_is_obj(h)) jd_headers_fill(J, jd_hlist_of(J, js_from_obj(headers)), h);
    }
    if (headers && type && jd_headers_get(J, jd_hlist_of(J, js_from_obj(headers)), js_str(J, "content-type")).t != JS_STR)
        jd_hlist_add(J, jd_hlist_of(J, js_from_obj(headers)), js_str(J, "content-type"), js_str(J, type));
    jobj *r = jd_make_response(J, t.obj->proto, body, status, st, headers, 0, 0);
    return r ? js_from_obj(r) : js_undef();
}

static jval nat_response_json_static(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval v = js_arg(a, n, 0);
    jval s = nat_json_stringify(J, js_undef(), &v, 1);
    if (J->sig != JS_OK) return js_undef();
    jobj *headers = jd_new_headers(J);
    if (headers) jd_hlist_add(J, jd_hlist_of(J, js_from_obj(headers)), js_str(J, "content-type"), js_str(J, "application/json"));
    jobj *r = jd_make_response(J, 0, s.t == JS_STR ? s.str : 0, 200, 0, headers, 0, 0);
    return js_from_obj(r);
}

static jval nat_response_clone(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return jd_illegal(J);
    jval b = jd_kept(t.obj, jd_k_rbody);
    jobj *r = jd_make_response(J, t.obj->proto, b.t == JS_STR ? b.str : 0,
                               (int)js_to_num(J, js_get(J, t, js_str(J, "status"))),
                               js_to_str(J, js_get(J, t, js_str(J, "statusText"))),
                               js_get(J, t, js_str(J, "headers")).obj,
                               js_to_str(J, js_get(J, t, js_str(J, "url"))),
                               js_to_bool(js_get(J, t, js_str(J, "redirected"))));
    return js_from_obj(r);
}

/* --- fetch --------------------------------------------------------------------------------- */

/* A promise refused with a TypeError, as a fetch that fails is. */
static void jd_refuse(jctx *J, jobj *p, const char *why) {
    js_throw(J, JS_ERR_TYPE, why, J->error_line);
    jval err = J->ret;
    J->sig = JS_OK;
    js_promise_settle(J, p, 0, err);
}

static jval nat_fetch(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *p = js_promise_new(J);
    if (!p) return js_undef();
    jobj *r = jd_make_request(J, 0, js_arg(a, n, 0), js_arg(a, n, 1));
    if (!r) {
        jval e = J->sig == JS_THROWN ? J->ret : js_undef();
        J->sig = JS_OK;
        js_promise_settle(J, p, 0, e);
        return js_from_obj(p);
    }
    jd_keep(r, jd_k_promise, js_from_obj(p));
    jval sig = jd_kept(r, jd_k_rsig);
    if (js_is_obj(sig) && jd_signal_aborted(sig.obj))
        js_promise_settle(J, p, 0, jd_kept(sig.obj, jd_k_reason));
    else if (!jd_queue(r, JQ_FETCH))
        jd_refuse(J, p, "too many requests at once");
    return js_from_obj(p);
}

/* A fetch whose signal was aborted before it was answered is refused now,
   and one under way given up. */
static void jd_fetch_aborted(jctx *J, jobj *sig) {
    for (int i = 0; i < jd_nreq; i++) {
        if ((!jd_req[i].waiting && !jd_req[i].ask) || jd_req[i].kind != JQ_FETCH) continue;
        jobj *r = jd_req[i].self;
        jval s = jd_kept(r, jd_k_rsig);
        if (!js_is_obj(s) || s.obj != sig) continue;
        jd_req[i].waiting = 0;
        jd_request_drop(i);
        jval p = jd_kept(r, jd_k_promise);
        if (js_is_obj(p)) js_promise_settle(J, p.obj, 0, jd_kept(sig, jd_k_reason));
    }
}

/* The reply's headers: every one the server sent but its cookies, which a
   page never sees (Fetch, "forbidden response-header"); for a reply made
   here (data:, blob:) its type. */
static jobj *jd_reply_headers(jctx *J, const jd_reply *rp) {
    jobj *h = jd_new_headers(J);
    if (!h) return 0;
    jobj *list = jd_hlist_of(J, js_from_obj(h));
    if (!rp->head || rp->hlen <= 0) {
        if (rp->type && rp->type[0]) jd_hlist_add(J, list, js_str(J, "content-type"), js_str(J, rp->type));
        return h;
    }
    const char *s = rp->head;
    int n = rp->hlen, at = 0;
    while (at < n && s[at] != '\n') at++;          /* past the status line */
    while (++at < n) {
        int ls = at;
        while (at < n && s[at] != '\n') at++;
        int le = at;
        if (le > ls && s[le - 1] == '\r') le--;
        int c = ls;
        while (c < le && s[c] != ':') c++;
        if (c == le || c == ls) continue;
        int vs = c + 1;
        while (vs < le && (s[vs] == ' ' || s[vs] == '\t')) vs++;
        int ve = le;
        while (ve > vs && (s[ve - 1] == ' ' || s[ve - 1] == '\t')) ve--;
        jstr *name = js_str_n(J, s + ls, (u32)(c - ls));
        if (!name || (c - ls == 10 && w_starts_fold(s + ls, "set-cookie")) ||
            (c - ls == 11 && w_starts_fold(s + ls, "set-cookie2"))) continue;
        jd_hlist_add(J, list, name, js_str_n(J, s + vs, (u32)(ve - vs)));
    }
    return h;
}

/* The reason the status line gives ("OK", "Not Found"), or nothing for a
   reply made here. */
static jstr *jd_status_text(jctx *J, const jd_reply *rp) {
    const char *h = rp->head;
    int n = rp->hlen, i = 0;
    if (!h || n <= 0) return js_str(J, "");
    while (i < n && h[i] != ' ' && h[i] != '\r' && h[i] != '\n') i++;     /* HTTP/1.1 */
    while (i < n && h[i] == ' ') i++;
    while (i < n && h[i] >= '0' && h[i] <= '9') i++;                       /* 200 */
    while (i < n && h[i] == ' ') i++;
    int at = i;
    while (i < n && h[i] != '\r' && h[i] != '\n') i++;
    return js_str_n(J, h + at, (u32)(i - at));
}

static void jd_fetch_done(jobj *r, const jd_reply *rp) {
    jctx *J = &jd_J;
    jval p = jd_kept(r, jd_k_promise);
    if (!js_is_obj(p)) return;
    if (rp->status <= 0) {
        js_throw(J, JS_ERR_TYPE, "Failed to fetch", J->error_line);
        jval e = J->ret;
        J->sig = JS_OK;
        js_promise_settle(J, p.obj, 0, e);
        return;
    }
    jstr *url = rp->url && rp->url[0] ? js_str(J, rp->url) : jd_kept(r, jd_k_url).str;
    int redirected = url && !js_str_eq(url, jd_kept(r, jd_k_url).str);
    jobj *resp = jd_make_response(J, 0, rp->len > 0 && rp->body ? js_str_n(J, rp->body, (u32)rp->len) : 0,
                                  rp->status, jd_status_text(J, rp), jd_reply_headers(J, rp), url, redirected);
    js_promise_settle(J, p.obj, 1, js_from_obj(resp));
}

/* --- XMLHttpRequest ------------------------------------------------------------------------ */

/* Its state kept under keys no script can name, and read through accessors
   on the prototype, where the standard puts them: Mozilla's consent manager
   takes the getters of readyState, response and responseText off
   XMLHttpRequest.prototype, and found none. */
static const char *const JD_XHR_FIELDS[] = { "readyState", "status", "statusText", "responseText", "response",
                                             "responseType", "responseURL", 0 };
static jstr *jd_k_xhr[7];

static void jd_xhr_put(jobj *o, int k, jval v) { jd_keep(o, jd_k_xhr[k], v); }

/* What a page sets, kept and given back, on the prototype as the standard
   has them (the consent manager above wraps withCredentials): a time limit,
   not kept to, since a request here ends when the connection's own wait
   does; and whether to send credentials, which does not change how the
   request is made. */
static jstr *jd_k_xhr_timeout, *jd_k_xhr_creds;

static jval nat_xhr_timeout(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    jval v = js_is_obj(t) ? jd_kept(t.obj, jd_k_xhr_timeout) : js_undef();
    return v.t == JS_NUM ? v : js_num(0);
}

static jval nat_xhr_set_timeout(jctx *J, jval t, jval *a, int n) {
    double ms = js_to_num(J, js_arg(a, n, 0));
    if (js_is_obj(t) && ms >= 0) jd_keep(t.obj, jd_k_xhr_timeout, js_num((double)(u32)ms));
    return js_undef();
}

static jval nat_xhr_creds(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_bool(js_is_obj(t) && js_to_bool(jd_kept(t.obj, jd_k_xhr_creds)));
}

static jval nat_xhr_set_creds(jctx *J, jval t, jval *a, int n) {
    (void)J;
    if (js_is_obj(t)) jd_keep(t.obj, jd_k_xhr_creds, js_bool(js_to_bool(js_arg(a, n, 0))));
    return js_undef();
}

static jval nat_xhr_field(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int k = (int)J->callee->data.num;
    jval v = js_is_obj(t) ? jd_kept(t.obj, jd_k_xhr[k]) : js_undef();
    if (v.t != JS_UNDEF) return v;
    return k <= 1 ? js_num(0) : jd_str("");
}

/* responseType: one of the kinds, or left as it was. */
static jval nat_xhr_set_type(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return js_undef();
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    static const char *const KINDS = "arraybuffer blob document json text";
    if (s && (!s->len || jd_word_in(KINDS, s->s))) jd_xhr_put(t.obj, 5, js_from_str(s));
    return js_undef();
}

static jval nat_xhr_ctor(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "XMLHttpRequest is made with new", J->error_line);
    jobj *o = t.obj;
    jd_xhr_put(o, 0, js_num(0));
    jd_xhr_put(o, 1, js_num(0));
    jd_xhr_put(o, 2, jd_str(""));
    jd_xhr_put(o, 3, jd_str(""));
    jd_xhr_put(o, 4, jd_str(""));
    jd_xhr_put(o, 5, jd_str(""));
    jd_xhr_put(o, 6, jd_str(""));
    jd_keep(o, jd_k_xhr_timeout, js_num(0));
    jd_keep(o, jd_k_xhr_creds, js_bool(0));
    jobj *up = js_object_with(J, JO_PLAIN, jd_p[JI_EVENTTARGET]);
    js_set(J, o, "upload", js_from_obj(up));
    jobj *headers = jd_new_headers(J);
    if (headers) jd_keep(o, jd_k_hlist, jd_kept(headers, jd_k_hlist));
    return js_undef();
}

static void jd_xhr_event(jobj *o, const char *type) {
    jobj *ev = jd_new_event(jd_evkind("ProgressEvent"), type, 0, 0);
    if (ev) jd_dispatch_to(ev, js_from_obj(o), js_from_obj(o));
}

/* One that says how much has come: the whole answer, here, since it is
   handed over at once. */
static void jd_xhr_progress(jobj *o, const char *type, int loaded) {
    jobj *ev = jd_new_event(jd_evkind("ProgressEvent"), type, 0, 0);
    if (!ev) return;
    js_set(&jd_J, ev, "lengthComputable", js_bool(loaded > 0));
    js_set(&jd_J, ev, "loaded", js_num(loaded));
    js_set(&jd_J, ev, "total", js_num(loaded));
    jd_dispatch_to(ev, js_from_obj(o), js_from_obj(o));
}

/* The request moved to a state and told so. Whether it is still in that
   state: a handler may have aborted it or opened it again. */
static int jd_xhr_step(jobj *o, int state) {
    jd_xhr_put(o, 0, js_num(state));
    jd_xhr_event(o, "readystatechange");
    jval rs = jd_kept(o, jd_k_xhr[0]);
    return rs.t == JS_NUM && rs.num == state;
}

static jval nat_xhr_open(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    int forbidden;
    jstr *m = jd_method_norm(J, jd_arg_str(J, a, n, 0), &forbidden);
    if (!m) return js_throw_dom(J, "SyntaxError", "that is not a method");
    if (forbidden) return js_throw_dom(J, "SecurityError", "that method may not be sent");
    jstr *u = jd_arg_str(J, a, n, 1);
    jstr *whole = jd_resolve_str(J, u->s);
    jd_keep(t.obj, jd_k_method, js_from_str(m));
    jd_keep(t.obj, jd_k_url, js_from_str(whole ? whole : u));
    jobj *list = js_array(J);
    if (list) jd_keep(t.obj, jd_k_hlist, js_from_obj(list));
    jd_xhr_put(t.obj, 0, js_num(1));
    jd_xhr_event(t.obj, "readystatechange");
    return js_undef();
}

/* Kept, and the one this browser can send -- what the body is -- sent. */
static jval nat_xhr_header(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jval list = jd_kept(t.obj, jd_k_hlist);
    if (list.t == JS_OBJ) jd_hlist_add(J, list.obj, jd_arg_str(J, a, n, 0), js_to_str(J, js_arg(a, n, 1)));
    return js_undef();
}

static jval nat_xhr_send(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    if (n > 0 && a[0].t != JS_UNDEF && a[0].t != JS_NULL) {
        const char *type;
        jstr *body = jd_body_of(J, a[0], &type);
        if (body) jd_keep(t.obj, jd_k_body, js_from_str(body));
        if (type) jd_keep(t.obj, jd_k_type, jd_str(type));
    }
    jval list = jd_kept(t.obj, jd_k_hlist);
    if (list.t == JS_OBJ) {
        jval ct = jd_headers_get(J, list.obj, js_str(J, "content-type"));
        if (ct.t == JS_STR) jd_keep(t.obj, jd_k_type, ct);
    }
    jd_queue(t.obj, JQ_XHR);
    jd_xhr_event(t.obj, "loadstart");
    return js_undef();
}

static jval nat_xhr_abort(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (!js_is_obj(t)) return js_undef();
    int was = 0;
    for (int i = 0; i < jd_nreq; i++)
        if ((jd_req[i].waiting || jd_req[i].ask) && jd_req[i].self == t.obj) {
            jd_req[i].waiting = 0;
            jd_request_drop(i);
            was = 1;
        }
    /* Asked from a handler while the answer is being told (state 2 or 3):
       the request is no longer waiting, but it has not ended. */
    jval rs = jd_kept(t.obj, jd_k_xhr[0]);
    if (rs.t == JS_NUM && (rs.num == 2 || rs.num == 3)) was = 1;
    if (was) {
        jd_xhr_put(t.obj, 0, js_num(4));
        jd_xhr_event(t.obj, "readystatechange");
        jd_xhr_event(t.obj, "abort");
        jd_xhr_event(t.obj, "loadend");
        jd_xhr_put(t.obj, 0, js_num(0));
    }
    return js_undef();
}

static jval nat_xhr_get_header(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jval kept = jd_kept(t.obj, jd_k_rheaders);
    if (!js_is_obj(kept)) return js_null();
    jobj *list = jd_hlist_of(J, kept);
    return list ? jd_headers_get(J, list, jd_arg_str(J, a, n, 0)) : js_null();
}

static jval nat_xhr_all_headers(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return jd_illegal(J);
    jval kept = jd_kept(t.obj, jd_k_rheaders);
    if (!js_is_obj(kept)) return jd_str("");
    jobj *list = jd_hlist_of(J, kept);
    jtext tx = { 0, 0, 0, 0 };
    for (u32 i = 0; list && i < list->len; i++) {
        jobj *p = list->items[i].obj;
        jt_put(J, &tx, p->items[0].str->s, p->items[0].str->len);
        jd_put(&tx, ": ");
        jt_put(J, &tx, p->items[1].str->s, p->items[1].str->len);
        jd_put(&tx, "\r\n");
    }
    return js_from_str(jt_done(J, &tx));
}

static void jd_xhr_done(jobj *o, const jd_reply *rp) {
    jctx *J = &jd_J;
    jval text = rp->len > 0 && rp->body ? js_from_str(js_str_n(J, rp->body, (u32)rp->len)) : jd_str("");
    jval resp = text;
    jstr *rt = js_to_str(J, js_get(J, js_from_obj(o), js_str(J, "responseType")));
    if (rt && js_str_is(rt, "json")) {
        resp = nat_json_parse(J, js_undef(), &text, 1);
        if (J->sig != JS_OK) { J->sig = JS_OK; resp = js_null(); }
    } else if (rt && js_str_is(rt, "arraybuffer")) {
        jobj *buf = ta_new_buffer(J, J->p_buffer, text.str->len);
        if (buf) for (u32 i = 0; i < text.str->len; i++) ta_bytes(buf)[i] = (u8)text.str->s[i];
        resp = js_from_obj(buf);
    }
    /* Everything taken from the reply before a handler runs: one may abort
       the request, which lets the reply go. */
    jobj *headers = jd_reply_headers(J, rp);
    jval stext = js_from_str(jd_status_text(J, rp));
    jval where = jd_str(rp->url && rp->url[0] ? rp->url : jd_kept_str(o, jd_k_url, ""));
    int loaded = rp->len > 0 ? rp->len : 0;
    if (headers) jd_keep(o, jd_k_rheaders, js_from_obj(headers));
    jd_xhr_put(o, 1, js_num(rp->status > 0 ? rp->status : 0));
    jd_xhr_put(o, 2, stext);
    jd_xhr_put(o, 6, where);
    /* The states an answer passes through, each told: headers, then the
       body coming, then done. Disney's player builds its response at state
       2 (on National Geographic, whose page waits for it), and waited for
       ever when this went from 1 straight to 4. */
    if (rp->status > 0) {
        if (!jd_xhr_step(o, 2)) return;
        jstr *rt3 = js_to_str(J, js_get(J, js_from_obj(o), js_str(J, "responseType")));
        if (!rt3 || !rt3->len || js_str_is(rt3, "text")) jd_xhr_put(o, 3, text);
        if (!jd_xhr_step(o, 3)) return;
        jd_xhr_progress(o, "progress", loaded);
        jval rs = jd_kept(o, jd_k_xhr[0]);
        if (rs.t != JS_NUM || rs.num != 3) return;
    }
    jd_xhr_put(o, 0, js_num(4));
    jd_xhr_put(o, 3, text);
    jd_xhr_put(o, 4, resp);
    jd_xhr_event(o, "readystatechange");
    /* load either way when the server answered: a page that asked for
       something it did not get is entitled to find out, and status is where
       it looks. No answer at all is an error. */
    jd_xhr_progress(o, rp->status > 0 ? "load" : "error", rp->status > 0 ? loaded : 0);
    jd_xhr_progress(o, "loadend", rp->status > 0 ? loaded : 0);
}

/* An answer handed to the request it is for. */
static void jd_request_answered(jobj *o, int kind, const jd_reply *rp) {
    if (kind == JQ_FETCH) jd_fetch_done(o, rp);
    else jd_xhr_done(o, rp);
    /* A promise settled from here, outside any call into the page, has its
       reactions run now, as they would be at the end of a task. */
    js_drain(&jd_J);
}

/* Whatever was sent, made. Started without waiting where the browser can,
   and the answers that have come handed over; otherwise one made whole a
   pass, because each blocks the browser while it happens and a page that
   sent six would otherwise stop for all six at once. */
/* The scripts queued above: each asked for when a way out is free, and run,
   then told load or error, once its text has come, in the order they come.
   How many were run or failed. */
static int jd_scripts_pump(void) {
    int told = 0;
    /* What waits is asked for, the page's own site's first. */
    for (int round = 0; round < 2 && !jd_asks_full; round++) {
        for (int k = 0; k < jd_nsq && !jd_spent(); ) {
            int i = jd_sq[k].node;
            const char *src = dom_attr(jd_doc, i, "src");
            if (jd_sq[k].ask || jd_same_site(src) != !round) { k++; continue; }
            int id = src && *src ? jd_sask_start(src) : -2;
            if (id == -1) { jd_asks_full = 1; break; }
            if (id >= 0) { jd_sq[k].ask = id + 1; k++; continue; }
            jd_script_unqueue(k);
            jd_outside_failed++;
            if (jd_script_done) jd_script_done(i, src, 0, "it would not come");
            jd_fire_simple(i, "error", 0, 0);
            told++;
        }
    }
    /* And what has come is run, in the order it came. */
    for (int k = 0; k < jd_nsq && !jd_spent(); ) {
        int i = jd_sq[k].node;
        const char *src = dom_attr(jd_doc, i, "src");
        if (!jd_sq[k].ask) { k++; continue; }
        const char *text = 0;
        int ask = jd_sq[k].ask - 1;
        int n = jd_sask_poll(ask, &text);
        if (n == 0) { k++; continue; }
        /* Out of the queue before it runs: what it inserts joins the end. */
        jd_script_unqueue(k);
        int ran = 0;
        if (n > 0 && text) {
            jd_outside++;
            if (!jd_room_for((u32)n)) {
                jd_note_text("a script too large for this machine's memory was not run");
                if (jd_script_done) jd_script_done(i, src, 0, "too large for this machine's memory");
            } else {
                jd_run_text(i, src, text, (u32)n);
                ran = 1;
            }
        } else {
            jd_outside_failed++;
            if (jd_script_done) jd_script_done(i, src, 0, "it would not come");
        }
        /* Ended after it ran: its text is the ask's until then. */
        if (jd_sask_end) jd_sask_end(ask);
        jd_fire_simple(i, ran ? "load" : "error", 0, 0);
        told++;
    }
    return told;
}

__attribute__((unused)) static int jsdom_requests(void) {
    if (!jd_open || jd_spent()) return 0;
    int told = 0;
    jd_asks_full = 0;
    for (int i = 0; i < jd_nreq && jd_ask_poll; i++) {
        if (!jd_req[i].ask) continue;
        jd_reply rp = { 0, 0, 0, 0, 0, 0, 0 };
        int st = jd_ask_poll(jd_req[i].ask - 1, &rp);
        if (st == 0) continue;
        if (st < 0) rp.status = st;
        jobj *o = jd_req[i].self;
        int kind = jd_req[i].kind;
        if (o) jd_request_answered(o, kind, &rp);
        jd_request_drop(i);
        told++;
        if (jd_spent()) return told;
    }
    /* What waits goes in two rounds, the page's own site's first. Scripts a
       page put in go before its requests, since most requests need what
       those define. */
    told += jd_scripts_pump();
    if (jd_spent() || jd_asks_full) return told;
    int order[JD_REQUESTS], no = 0;
    for (int round = 0; round < 2; round++)
        for (int i = 0; i < jd_nreq; i++) {
            if (!jd_req[i].waiting) continue;
            const char *u = jd_req[i].self ? jd_kept_str(jd_req[i].self, jd_k_url, "") : "";
            if (jd_same_site(u) == !round) order[no++] = i;
        }
    for (int oi = 0; oi < no; oi++) {
        int i = order[oi];
        if (!jd_req[i].waiting) continue;
        jobj *o = jd_req[i].self;
        int kind = jd_req[i].kind;
        if (!o) { jd_req[i].waiting = 0; continue; }
        const char *method = jd_kept_str(o, jd_k_method, "GET");
        const char *url = jd_kept_str(o, jd_k_url, "");
        jval bv = jd_kept(o, jd_k_body);
        const char *body = bv.t == JS_STR ? bv.str->s : 0;
        int blen = bv.t == JS_STR ? (int)bv.str->len : 0;
        const char *type = jd_kept_str(o, jd_k_type, 0);
        static char lines[4096];
        lines[0] = 0;
        jobj *hl = 0;
        if (kind == JQ_FETCH) {
            jval h = js_get(&jd_J, js_from_obj(o), js_str(&jd_J, "headers"));
            jd_J.sig = JS_OK;
            if (js_is_obj(h)) hl = jd_hlist_of(&jd_J, h);
            jd_J.sig = JS_OK;
        } else {
            jval h = jd_kept(o, jd_k_hlist);
            if (h.t == JS_OBJ && h.obj->kind == JO_ARRAY) hl = h.obj;
        }
        if (hl) {
            jval ct = jd_headers_get(&jd_J, hl, js_str(&jd_J, "content-type"));
            if (ct.t == JS_STR) type = ct.str->s;
            jd_header_lines(hl, lines, (int)sizeof(lines));
        }
        jd_reply rp = { 0, 0, 0, 0, 0, 0, 0 };
        char *data = 0, mime[96];
        if (url[0] && !jd_is_data_url(url) && !jd_is_blob_url(url) && jd_ask_start) {
            int id = jd_ask_start(method, url, body, blen, body ? (type ? type : "") : 0, lines, JD_REPLY_MAX);
            if (id == -1) { jd_asks_full = 1; break; }   /* every way out busy: a later pass */
            jd_req[i].waiting = 0;
            if (id >= 0) { jd_req[i].ask = id + 1; continue; }
            rp.status = id;
            jd_request_answered(o, kind, &rp);
            return told + 1;
        }
        jd_req[i].waiting = 0;
        if (jd_is_data_url(url)) {
            /* Answered here, from the address itself; one that does not
               decode is a failure to fetch, as the standard has it. */
            u32 n = jd_data_url(url, &data, mime, (int)sizeof(mime));
            if (data) {
                rp.body = data;
                rp.len = (int)n;
                rp.status = 200;
                rp.type = mime;
                rp.url = url;
            }
        } else if (jd_is_blob_url(url)) {
            /* From what the address was made for, or a failure to fetch
               once it has been revoked. */
            jstr *bb, *bt;
            if (jd_blob_lookup(url, &bb, &bt)) {
                rp.body = bb->s;
                rp.len = (int)bb->len;
                rp.status = 200;
                rp.type = bt ? bt->s : "";
                rp.url = url;
            }
        } else if (url[0] && jd_do_request) {
            int asked = ticks();
            jd_do_request(method, url, body, blen, body ? (type ? type : "") : 0, lines, &rp);
            jd_net_waited(asked);
        }
        jd_request_answered(o, kind, &rp);
        free(data);
        return told + 1;
    }
    return told;
}

static void jd_setup_net(jctx *J) {
    jd_k_method = js_sym_new(J, "method", 6);
    jd_k_url = js_sym_new(J, "url", 3);
    jd_k_body = js_sym_new(J, "body", 4);
    jd_k_type = js_sym_new(J, "type", 4);
    jd_k_promise = js_sym_new(J, "promise", 7);
    jd_k_rsig = js_sym_new(J, "signal", 6);
    jd_k_hlist = js_sym_new(J, "headers", 7);
    jd_k_rbody = js_sym_new(J, "body", 4);
    jd_k_rheaders = js_sym_new(J, "reply", 5);
    jd_k_fpairs = js_sym_new(J, "form", 4);
    jd_k_aborted = js_sym_new(J, "aborted", 7);
    jd_k_reason = js_sym_new(J, "reason", 6);

    jobj *xp = jd_interface(J, "XMLHttpRequestEventTarget", jd_p[JI_EVENTTARGET], 0, 0);
    xp = jd_interface(J, "XMLHttpRequest", xp, nat_xhr_ctor, 0);
    jd_p[JI_XHR] = xp;
    for (int k = 0; JD_XHR_FIELDS[k]; k++) {
        jd_k_xhr[k] = js_sym_new(J, JD_XHR_FIELDS[k], (u32)w_len(JD_XHR_FIELDS[k]));
        jobj *g = js_native(J, JD_XHR_FIELDS[k], nat_xhr_field);
        jobj *st = k == 5 ? js_native(J, JD_XHR_FIELDS[k], nat_xhr_set_type) : 0;
        if (!g) continue;
        g->flags |= JOF_NOCTOR;
        g->data = js_num(k);
        if (st) st->flags |= JOF_NOCTOR;
        js_define_accessor(J, xp, js_str(J, JD_XHR_FIELDS[k]), js_from_obj(g), st ? js_from_obj(st) : js_undef(), JP_ENUM | JP_CONF);
    }
    jd_k_xhr_timeout = js_sym_new(J, "timeout", 7);
    jd_k_xhr_creds = js_sym_new(J, "withCredentials", 15);
    jd_accessor(J, xp, "timeout", nat_xhr_timeout, nat_xhr_set_timeout);
    jd_accessor(J, xp, "withCredentials", nat_xhr_creds, nat_xhr_set_creds);
    jd_accessor(J, xp, "responseXML", nat_null_getter, 0);
    jd_method(J, xp, "open", nat_xhr_open, 2);
    jd_method(J, xp, "send", nat_xhr_send, 0);
    jd_method(J, xp, "setRequestHeader", nat_xhr_header, 2);
    jd_method(J, xp, "abort", nat_xhr_abort, 0);
    jd_method(J, xp, "getResponseHeader", nat_xhr_get_header, 1);
    jd_method(J, xp, "getAllResponseHeaders", nat_xhr_all_headers, 0);
    jd_method(J, xp, "overrideMimeType", nat_nothing_js, 1);
    static const char *const XS[] = { "UNSENT", "OPENED", "HEADERS_RECEIVED", "LOADING", "DONE", 0 };
    jd_consts(J, xp, XS, 0);
    jd_consts(J, jd_ctor_of(xp), XS, 0);

    jd_p_headers = jd_interface(J, "Headers", 0, nat_headers_ctor, 0);
    jd_method(J, jd_p_headers, "append", nat_headers_append, 2);
    jd_method(J, jd_p_headers, "set", nat_headers_set, 2);
    jd_method(J, jd_p_headers, "get", nat_headers_get, 1);
    jd_method(J, jd_p_headers, "has", nat_headers_has, 1);
    jd_method(J, jd_p_headers, "delete", nat_headers_delete, 1);
    jd_method(J, jd_p_headers, "forEach", nat_headers_foreach, 1);
    jd_method(J, jd_p_headers, "entries", nat_headers_entries, 0);
    jd_method(J, jd_p_headers, "keys", nat_headers_keys, 0);
    jd_method(J, jd_p_headers, "values", nat_headers_values, 0);
    jd_method(J, jd_p_headers, "getSetCookie", nat_headers_setcookie, 0);
    js_method_key(J, jd_p_headers, J->sym_iterator, "[Symbol.iterator]", nat_headers_entries, 0);

    jd_p_request = jd_interface(J, "Request", 0, nat_request_ctor, 1);
    jd_accessor(J, jd_p_request, "url", nat_request_url, 0);
    jd_accessor(J, jd_p_request, "method", nat_request_method, 0);
    jd_accessor(J, jd_p_request, "signal", nat_request_signal, 0);

    jd_p_response = jd_interface(J, "Response", 0, nat_response_ctor, 0);
    for (int k = 0; k < JRF_FIELDS; k++) {
        jd_k_resp[k] = js_sym_new(J, JD_RESP_FIELDS[k], (u32)w_len(JD_RESP_FIELDS[k]));
        jobj *g = js_native(J, JD_RESP_FIELDS[k], nat_response_field);
        if (!g) continue;
        g->flags |= JOF_NOCTOR;
        g->data = js_num(k);
        js_define_accessor(J, jd_p_response, js_str(J, JD_RESP_FIELDS[k]), js_from_obj(g), js_undef(), JP_ENUM | JP_CONF);
    }
    jd_method(J, jd_p_response, "text", nat_response_text, 0);
    jd_method(J, jd_p_response, "json", nat_response_json, 0);
    jd_method(J, jd_p_response, "arrayBuffer", nat_response_buffer, 0);
    jd_method(J, jd_p_response, "clone", nat_response_clone, 0);
    jobj *rc = jd_ctor_of(jd_p_response);
    if (rc) js_method(J, rc, "json", nat_response_json_static, 1);

    js_declare(J, J->global, js_str(J, "fetch"), js_from_obj(js_native_n(J, "fetch", nat_fetch, 1)));

    jd_p_signal = jd_interface(J, "AbortSignal", jd_p[JI_EVENTTARGET], 0, 0);
    jd_accessor(J, jd_p_signal, "aborted", nat_signal_aborted, 0);
    jd_accessor(J, jd_p_signal, "reason", nat_signal_reason, 0);
    jd_method(J, jd_p_signal, "throwIfAborted", nat_signal_throw, 0);
    jobj *sc = jd_ctor_of(jd_p_signal);
    if (sc) {
        js_method(J, sc, "abort", nat_signal_abort_static, 0);
        js_method(J, sc, "timeout", nat_signal_timeout, 1);
    }
    jobj *cp = jd_interface(J, "AbortController", 0, nat_controller_ctor, 0);
    jd_method(J, cp, "abort", nat_controller_abort, 0);

    jd_setup_formdata(J);
}
