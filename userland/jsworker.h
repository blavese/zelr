/* Web Workers.
 *
 * MSN's page and OpenStreetMap's stopped at their first script with "Worker
 * is not defined", and drew nothing. A worker is a JavaScript context of its
 * own (js_init), beside the page's, with a worker's globals: postMessage,
 * onmessage and addEventListener, importScripts, close, timers, fetch,
 * location, navigator, performance and crypto. There is one processor's
 * worth of time, so it runs on the page's: its script, each message to it,
 * each of its timers and each answer to its fetches is a task in the page's
 * queue (jd_later_native), which is where a browser's worker would have been
 * doing the same work at the same moments as far as the page can tell.
 *
 * A message crosses by a copy made in the other context (jsc_across), as the
 * standard's structured clone does between threads; nothing is shared, so a
 * worker that is ended is given back whole (js_done). Each task names its
 * worker's slot with the slot's generation, so a slot used again never runs
 * what was queued for the worker that had it.
 *
 * The worker's own event machinery -- listeners, onmessage, MessageEvent,
 * the Response a fetch gives -- is a few lines of JavaScript run in its
 * context when it is made (JW_PRELUDE), given the natives it needs as
 * arguments so none of them is a global a script could find.
 *
 * Not done: module workers (type "module" is run as a classic script, and
 * one with import in it fails with an error event), SharedWorker, transfer
 * (what the list names is copied, not moved), XMLHttpRequest inside a worker,
 * and a fetch's headers beyond its content type. */
#pragma once

#define JW_MAX 8
#define JW_TIMERS 256
#define JW_FETCHES 32

typedef struct {
    jval  fn;                 /* in the worker's context: a function, or code as a string */
    jobj *args;
    int   every;              /* ticks, 0 for once */
    u32   id;                 /* 0 when the entry is free */
} jwtimer;

typedef struct {
    jctx *J;                  /* the worker's context, 0 when the slot is free */
    jobj *worker;             /* the Worker object, in the page's context */
    jval  deliver, response;  /* the prelude's (JW_PRELUDE) */
    jobj *inbox;              /* messages to it, already in its context */
    char *source;             /* its script, until it has run */
    int   len;
    u32   gen;
    int   closed;
    char  url[URL_TEXT];
    jwtimer timers[JW_TIMERS];
    u32   next_timer;
} jworker;

typedef struct {
    int   used, slot;
    u32   gen;
    jobj *promise;            /* in the worker's context */
    char  url[URL_TEXT];
    char  method[8];
    char *body;
} jwfetch;

static jworker jw[JW_MAX];
static u32 jw_ended[JW_MAX];  /* each slot's last generation the page terminated */
static jwfetch jw_fetches[JW_FETCHES];
static u32 jw_gens;
static jstr *jw_k_slot;       /* on a Worker object: its slot's task name (jw_name) */

/* A task's name for a slot as it is now, and the slot it names while that
   is still so: the slot in the low three bits, its generation above. */
static double jw_name(int slot) { return (double)((u64)jw[slot].gen * JW_MAX + (u64)slot); }

static int jw_named(double v) {
    u64 x = (u64)v;
    int slot = (int)(x % JW_MAX);
    return jw[slot].J && !jw[slot].closed && jw[slot].gen == (u32)(x / JW_MAX) ? slot : -1;
}

static int jw_slot_of(jctx *J) {
    for (int i = 0; i < JW_MAX; i++) if (jw[i].J == J) return i;
    return -1;
}

/* --- ending one ------------------------------------------------------------------------- */

static void jw_free_slot(int slot) {
    jworker *w = &jw[slot];
    for (int i = 0; i < JW_FETCHES; i++)
        if (jw_fetches[i].used && jw_fetches[i].slot == slot) { free(jw_fetches[i].body); jw_fetches[i].used = 0; }
    if (w->J) { js_done(w->J); free(w->J); }
    free(w->source);
    u32 gen = w->gen;
    volatile u8 *z = (volatile u8 *)w;
    for (u32 i = 0; i < sizeof(*w); i++) z[i] = 0;
    w->gen = gen;
}

/* close() is called from inside the worker, whose context cannot be given
   back under it: it is marked, and given back as a task of its own. */
static void jw_free_due(jval arg) {
    int slot = (int)arg.num;
    if (slot >= 0 && slot < JW_MAX && jw[slot].J && jw[slot].closed) jw_free_slot(slot);
}

static void jw_close_all(void) {
    for (int i = 0; i < JW_MAX; i++) if (jw[i].J) jw_free_slot(i);
    for (int i = 0; i < JW_FETCHES; i++) { free(jw_fetches[i].body); jw_fetches[i].used = 0; }
}

/* --- to the page ------------------------------------------------------------------------ */

/* An error the worker did not catch: an error event at its Worker object,
   and the line on the page's console a browser would show. */
static void jw_error_due(jval arg) {
    if (!js_is_obj(arg) || arg.obj->kind != JO_ARRAY || arg.obj->len < 3) return;
    jval target = arg.obj->items[0], msg = arg.obj->items[1], line = arg.obj->items[2];
    if (!js_is_obj(target)) return;
    jobj *ev = jd_new_event(jd_evkind("ErrorEvent"), "error", 0, 1);
    if (!ev) return;
    js_set(&jd_J, ev, "isTrusted", js_bool(1));
    js_set(&jd_J, ev, "message", msg);
    js_set(&jd_J, ev, "lineno", line);
    js_set(&jd_J, ev, "colno", js_num(0));
    jd_dispatch_to(ev, target, target);
}

static void jw_report(int slot, const char *msg, int line) {
    if (js_print_hook) {
        js_print_hook("Uncaught in a worker: ", 22);
        js_print_hook(msg, (u32)w_len(msg));
        js_print_hook("\n", 1);
    }
    jobj *job = js_array(&jd_J);
    if (!job || !jw[slot].worker) return;
    js_arr_push(&jd_J, job, js_from_obj(jw[slot].worker));
    js_arr_push(&jd_J, job, jd_str(msg));
    js_arr_push(&jd_J, job, js_num(line));
    jd_later_native(jw_error_due, js_from_obj(job), 0);
}

/* What the worker threw, said: the error's own message when it has one. */
static void jw_report_thrown(int slot) {
    jctx *W = jw[slot].J;
    jval e = W->ret;
    W->sig = JS_OK;
    char buf[200];
    jstr *s = 0;
    if (js_is_obj(e)) {
        jval m = js_get(W, e, js_str(W, "message"));
        if (W->sig == JS_OK && m.t == JS_STR) s = m.str;
        W->sig = JS_OK;
    }
    if (!s) s = js_to_str(W, e);
    W->sig = JS_OK;
    int k = 0;
    for (; s && k < (int)s->len && k < (int)sizeof(buf) - 1; k++) buf[k] = s->s[k];
    buf[k] = 0;
    jw_report(slot, buf, W->error_line);
}

/* A worker's work done: what it left for its microtask queue, and what it
   threw said. */
static void jw_after(int slot) {
    jctx *W = jw[slot].J;
    if (!W) return;
    if (W->sig == JS_THROWN) jw_report_thrown(slot);
    W->sig = JS_OK;
    js_drain(W);
    if (W->sig == JS_THROWN) jw_report_thrown(slot);
    W->sig = JS_OK;
}

static void jw_outbox_due(jval arg) {
    if (!js_is_obj(arg) || arg.obj->kind != JO_ARRAY || arg.obj->len < 2) return;
    /* What a worker posted before it closed itself still arrives, as the
       standard has it; nothing does from one the page terminated since, or
       once its slot is another worker's. */
    u64 x = (u64)arg.obj->items[0].num;
    int slot = (int)(x % JW_MAX);
    u32 gen = (u32)(x / JW_MAX);
    if (jw[slot].gen != gen || jw_ended[slot] == gen) return;
    jobj *job = js_array(&jd_J);
    if (!job) return;
    for (u32 i = 1; i < arg.obj->len; i++) js_arr_push(&jd_J, job, arg.obj->items[i]);
    jd_message_due(js_from_obj(job));
}

/* --- the worker's globals --------------------------------------------------------------- */

static jval nat_jw_post(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int slot = jw_slot_of(J);
    if (slot < 0 || jw[slot].closed) return js_undef();
    jval data = jsc_across(J, &jd_J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    jd_J.sig = JS_OK;
    jobj *job = js_array(&jd_J);
    if (!job) return js_undef();
    js_arr_push(&jd_J, job, js_num(jw_name(slot)));
    js_arr_push(&jd_J, job, js_from_obj(jw[slot].worker));
    js_arr_push(&jd_J, job, data);
    js_arr_push(&jd_J, job, jd_str(""));
    js_arr_push(&jd_J, job, js_null());
    jd_later_native(jw_outbox_due, js_from_obj(job), 0);
    return js_undef();
}

static jval nat_jw_close(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    int slot = jw_slot_of(J);
    if (slot >= 0 && !jw[slot].closed) {
        jw[slot].closed = 1;
        jd_later_native(jw_free_due, js_num(slot), 0);
    }
    return js_undef();
}

/* An address the worker wrote, whole, against its own address when that is
   one a path can be resolved against, else against the page's. */
static int jw_resolve(int slot, const char *href, char *out, int cap) {
    if (jd_is_data_url(href) || jd_is_blob_url(href)) { w_copy(out, cap, href, cap); return 1; }
    jurl *base = (jurl *)malloc(sizeof(jurl)), *u = (jurl *)malloc(sizeof(jurl));
    int ok = 0;
    if (base && u) {
        int have = !jd_is_data_url(jw[slot].url) && !jd_is_blob_url(jw[slot].url) && ju_parse(jw[slot].url, 0, base);
        if ((have || ju_page(base)) && ju_parse(href, base, u)) {
            jtext tx = { 0, 0, 0, 0 };
            ju_text(u, &tx, 1);
            int k = 0;
            for (; k < (int)tx.n && k < cap - 1; k++) out[k] = tx.b[k];
            out[k] = 0;
            free(tx.b);
            ok = 1;
        }
    }
    free(base);
    free(u);
    return ok;
}

/* A script's text from an address: data: from itself, blob: from its Blob,
   the rest from the network the way the page's scripts come. Copied, since
   the buffer a fetched script arrives in is the next one's too. */
static char *jw_script_text(const char *url, int *len) {
    const char *text = 0;
    char *owned = 0;
    int n = 0;
    if (jd_is_data_url(url)) { n = (int)jd_data_url(url, &owned, 0, 0); text = owned; }
    else if (jd_is_blob_url(url)) {
        jstr *bb, *bt;
        if (jd_blob_lookup(url, &bb, &bt)) { text = bb->s; n = (int)bb->len; }
    } else if (jd_get_script) n = jd_get_script(url, &text);
    char *copy = n > 0 && text ? (char *)malloc((u64)n + 1) : 0;
    for (int i = 0; copy && i < n; i++) copy[i] = text[i];
    if (copy) copy[n] = 0;
    free(owned);
    *len = copy ? n : 0;
    return copy;
}

/* importScripts: each fetched and run in the worker's global scope, in
   order, before this returns, as the standard has it. */
static jval nat_jw_import(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int slot = jw_slot_of(J);
    if (slot < 0) return js_undef();
    for (int i = 0; i < n && J->sig == JS_OK; i++) {
        jstr *s = js_to_str(J, a[i]);
        char url[URL_TEXT];
        int len = 0;
        char *text = s && jw_resolve(slot, s->s, url, (int)sizeof(url)) ? jw_script_text(url, &len) : 0;
        if (!text) return js_throw_dom(J, "NetworkError", "a script importScripts asked for would not come");
        jstr *src = js_str_n(J, text, (u32)len);
        free(text);
        if (!src) return js_undef();
        js_eval_source(J, src, J->global, J->global_lex, js_from_obj(J->global_obj));
    }
    return js_undef();
}

/* --- the worker's timers ---------------------------------------------------------------- */

static double jw_timer_name(int slot, u32 id) { return (double)id * 65536.0 * JW_MAX + jw_name(slot); }

static void jw_timer_due(jval arg) {
    u64 x = (u64)arg.num;
    int slot = jw_named((double)(x % (65536ull * JW_MAX)));
    u32 id = (u32)(x / (65536ull * JW_MAX));
    if (slot < 0) return;
    jworker *w = &jw[slot];
    jwtimer *tm = 0;
    for (int i = 0; i < JW_TIMERS && !tm; i++) if (w->timers[i].id == id) tm = &w->timers[i];
    if (!tm) return;
    jctx *W = w->J;
    jval fn = tm->fn;
    jobj *args = tm->args;
    if (tm->every > 0) jd_later_native(jw_timer_due, arg, tm->every);
    else tm->id = 0;
    W->sig = JS_OK;
    if (fn.t == JS_STR) js_eval_source(W, fn.str, W->global, W->global_lex, js_from_obj(W->global_obj));
    else js_call(W, fn, js_from_obj(W->global_obj), args ? args->items : 0, args ? (int)args->len : 0);
    jw_after(slot);
}

static jval jw_add_timer(jctx *J, jval *a, int n, int repeat) {
    int slot = jw_slot_of(J);
    if (slot < 0) return js_num(0);
    jworker *w = &jw[slot];
    jval fn = js_arg(a, n, 0);
    if (!js_callable(fn)) {
        jstr *code = js_to_str(J, fn);
        if (!code) return js_num(0);
        fn = js_from_str(code);
    }
    double ms = n > 1 ? js_to_num(J, a[1]) : 0;
    if (!(ms >= 0)) ms = 0;
    if (ms > 2147483647.0) ms = 2147483647.0;
    int ticks = (int)((ms * JD_HZ + 999) / 1000);
    if (repeat && ticks < 1) ticks = 1;
    jwtimer *tm = 0;
    for (int i = 0; i < JW_TIMERS && !tm; i++) if (!w->timers[i].id) tm = &w->timers[i];
    if (!tm) return js_num(0);
    tm->id = ++w->next_timer;
    tm->fn = fn;
    tm->args = 0;
    if (n > 2) {
        tm->args = js_array(J);
        for (int i = 2; tm->args && i < n; i++) js_arr_push(J, tm->args, a[i]);
    }
    tm->every = repeat ? ticks : 0;
    jd_later_native(jw_timer_due, js_num(jw_timer_name(slot, tm->id)), ticks);
    return js_num(tm->id);
}

static jval nat_jw_set_timeout(jctx *J, jval t, jval *a, int n) { (void)t; return jw_add_timer(J, a, n, 0); }
static jval nat_jw_set_interval(jctx *J, jval t, jval *a, int n) { (void)t; return jw_add_timer(J, a, n, 1); }

static jval nat_jw_clear_timer(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int slot = jw_slot_of(J);
    double id = js_to_num(J, js_arg(a, n, 0));
    for (int i = 0; slot >= 0 && i < JW_TIMERS; i++)
        if (jw[slot].timers[i].id && (double)jw[slot].timers[i].id == id) jw[slot].timers[i].id = 0;
    return js_undef();
}

/* --- the worker's fetch ------------------------------------------------------------------ */

static void jw_fetch_due(jval arg) {
    int k = (int)arg.num;
    if (k < 0 || k >= JW_FETCHES || !jw_fetches[k].used) return;
    jwfetch *f = &jw_fetches[k];
    f->used = 0;
    int slot = jw[f->slot].J && jw[f->slot].gen == f->gen && !jw[f->slot].closed ? f->slot : -1;
    if (slot < 0) { free(f->body); return; }
    jctx *W = jw[slot].J;
    jd_reply rp = { 0, 0, 0, 0, 0 };
    char *owned = 0;
    if (jd_is_data_url(f->url)) {
        rp.len = (int)jd_data_url(f->url, &owned, 0, 0);
        rp.body = owned;
        rp.status = owned ? 200 : 0;
        rp.url = f->url;
    } else if (jd_is_blob_url(f->url)) {
        jstr *bb, *bt;
        if (jd_blob_lookup(f->url, &bb, &bt)) {
            rp.body = bb->s; rp.len = (int)bb->len; rp.status = 200; rp.type = bt ? bt->s : ""; rp.url = f->url;
        }
    } else if (jd_do_request) jd_do_request(f->method, f->url, f->body, 0, &rp);
    free(f->body);
    W->sig = JS_OK;
    if (rp.status <= 0) {
        jval err = js_throw(W, JS_ERR_TYPE, "Failed to fetch", 0);
        (void)err;
        jval e = W->ret;
        W->sig = JS_OK;
        js_promise_settle(W, f->promise, 0, e);
    } else {
        jval args[4] = { js_num(rp.status), js_from_str(js_str(W, rp.url ? rp.url : f->url)),
                         js_from_str(js_str(W, rp.type ? rp.type : "")),
                         js_from_str(js_str_n(W, rp.body ? rp.body : "", (u32)(rp.len > 0 ? rp.len : 0))) };
        jval r = js_call(W, jw[slot].response, js_undef(), args, 4);
        if (W->sig == JS_OK) js_promise_settle(W, f->promise, 1, r);
    }
    free(owned);
    jw_after(slot);
}

/* fetch(address or { url }, { method, body }): a GET or a POST with a text
   body, answered as a task of its own. */
static jval nat_jw_fetch(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int slot = jw_slot_of(J);
    jobj *p = js_promise_new(J);
    if (!p || slot < 0) return p ? js_from_obj(p) : js_undef();
    jval target = js_arg(a, n, 0), init = js_arg(a, n, 1);
    if (js_is_obj(target)) target = js_get(J, target, js_str(J, "url"));
    jstr *href = js_to_str(J, target);
    jval method = js_is_obj(init) ? js_get(J, init, js_str(J, "method")) : js_undef();
    jval body = js_is_obj(init) ? js_get(J, init, js_str(J, "body")) : js_undef();
    if (J->sig != JS_OK || !href) return js_undef();
    int k = 0;
    while (k < JW_FETCHES && jw_fetches[k].used) k++;
    jwfetch *f = k < JW_FETCHES ? &jw_fetches[k] : 0;
    if (!f || !jw_resolve(slot, href->s, f->url, (int)sizeof(f->url))) {
        js_throw(J, JS_ERR_TYPE, "Failed to fetch", J->error_line);
        jval e = J->ret;
        J->sig = JS_OK;
        js_promise_settle(J, p, 0, e);
        return js_from_obj(p);
    }
    f->used = 1;
    f->slot = slot;
    f->gen = jw[slot].gen;
    f->promise = p;
    jstr *m = method.t == JS_UNDEF ? 0 : js_to_str(J, method);
    w_copy(f->method, (int)sizeof(f->method), m && m->len ? m->s : "GET", (int)sizeof(f->method));
    for (char *q = f->method; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
    f->body = 0;
    if (body.t != JS_UNDEF && body.t != JS_NULL) {
        jstr *b = js_to_str(J, body);
        f->body = b ? (char *)malloc((u64)b->len + 1) : 0;
        for (u32 i = 0; f->body && i <= b->len; i++) f->body[i] = b->s[i];
    }
    jd_later_native(jw_fetch_due, js_num(k), 0);
    return js_from_obj(p);
}

/* The bytes of a string as an ArrayBuffer, for the prelude's
   Response.arrayBuffer. */
static jval nat_jw_to_buffer(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    jobj *b = s ? ta_new_buffer(J, J->p_buffer, (double)s->len) : 0;
    if (!b) return js_undef();
    u8 *d = ta_bytes(b);
    for (u32 i = 0; i < s->len; i++) d[i] = (u8)s->s[i];
    return js_from_obj(b);
}

static jobj *jw_location(jctx *W, const char *url);

/* parseURL(href, base) for the prelude's URL: the page's own parser, the
   parts as an object, or null for an address that does not parse. */
static jval nat_jw_parse_url(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *href = js_to_str(J, js_arg(a, n, 0));
    jval bv = js_arg(a, n, 1);
    jstr *bs = bv.t == JS_UNDEF ? 0 : js_to_str(J, bv);
    if (!href || J->sig != JS_OK) return js_null();
    jurl *base = (jurl *)malloc(sizeof(jurl)), *u = (jurl *)malloc(sizeof(jurl));
    jval r = js_null();
    int ok = base && u && (!bs || ju_parse(bs->s, 0, base)) && ju_parse(href->s, bs ? base : 0, u);
    if (ok) {
        jobj *o = js_object(J, JO_PLAIN);
        if (o) {
            jtext tx = { 0, 0, 0, 0 };
            ju_text(u, &tx, 1);
            js_set(J, o, "href", js_from_str(jt_done(J, &tx)));
            jobj *loc = jw_location(J, js_get(J, js_from_obj(o), js_str(J, "href")).str->s);
            static const char *const PARTS[] = { "protocol", "host", "hostname", "port", "pathname", "search", "hash", "origin", 0 };
            for (int i = 0; loc && PARTS[i]; i++) js_set(J, o, PARTS[i], js_get(J, js_from_obj(loc), js_str(J, PARTS[i])));
            js_set(J, o, "username", js_from_str(js_str(J, u->user)));
            js_set(J, o, "password", js_from_str(js_str(J, u->pass)));
            r = js_from_obj(o);
        }
    }
    free(base);
    free(u);
    return r;
}

/* What the prelude hands a listener's error to. */
static jval nat_jw_report(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int slot = jw_slot_of(J);
    if (slot < 0) return js_undef();
    J->ret = js_arg(a, n, 0);
    jw_report_thrown(slot);
    return js_undef();
}

/* The worker's event machinery and the rest of its globals that are plain
   JavaScript, run in its context once (jw_new). It is given the natives it
   uses as arguments and hands back the two functions the kernel of this
   file calls: deliver, which gives the worker a message, and response,
   which makes what its fetch resolves to. */
static const char JW_PRELUDE[] =
    "(function (g, report, toBuffer, parseURL, loc, nav) {\n"
    "  var L = {};\n"
    "  function listeners(t) {\n"
    "    if (t === g) return L;\n"
    "    if (!Object.prototype.hasOwnProperty.call(t, '__listeners'))\n"
    "      Object.defineProperty(t, '__listeners', { value: {}, enumerable: false });\n"
    "    return t.__listeners;\n"
    "  }\n"
    "  function Event(type, init) { init = init || {}; this.type = String(type); this.bubbles = !!init.bubbles;\n"
    "    this.cancelable = !!init.cancelable; this.defaultPrevented = false; this.isTrusted = false;\n"
    "    this.target = null; this.currentTarget = null; this.timeStamp = g.performance.now(); }\n"
    "  Event.prototype.preventDefault = function () { if (this.cancelable) this.defaultPrevented = true; };\n"
    "  Event.prototype.stopPropagation = function () {};\n"
    "  Event.prototype.stopImmediatePropagation = function () { this.stopped = true; };\n"
    "  function MessageEvent(type, init) { Event.call(this, type, init); init = init || {};\n"
    "    this.data = init.data === undefined ? null : init.data; this.origin = init.origin || '';\n"
    "    this.lastEventId = init.lastEventId || ''; this.source = init.source || null; this.ports = init.ports || []; }\n"
    "  MessageEvent.prototype = Object.create(Event.prototype); MessageEvent.prototype.constructor = MessageEvent;\n"
    "  function ErrorEvent(type, init) { Event.call(this, type, init); init = init || {};\n"
    "    this.message = init.message || ''; this.filename = init.filename || ''; this.lineno = init.lineno || 0;\n"
    "    this.colno = init.colno || 0; this.error = init.error === undefined ? null : init.error; }\n"
    "  ErrorEvent.prototype = Object.create(Event.prototype); ErrorEvent.prototype.constructor = ErrorEvent;\n"
    "  function EventTarget() {}\n"
    "  EventTarget.prototype.addEventListener = function (type, fn) {\n"
    "    if (typeof fn !== 'function' && !(fn && typeof fn.handleEvent === 'function')) return;\n"
    "    var m = listeners(this), a = m[type] || (m[type] = []); if (a.indexOf(fn) < 0) a.push(fn); };\n"
    "  EventTarget.prototype.removeEventListener = function (type, fn) {\n"
    "    var a = listeners(this)[type]; if (!a) return; var i = a.indexOf(fn); if (i >= 0) a.splice(i, 1); };\n"
    "  EventTarget.prototype.dispatchEvent = function (e) {\n"
    "    var t = this; e.target = e.currentTarget = t; var h = t['on' + e.type];\n"
    "    if (typeof h === 'function') { try { h.call(t, e); } catch (x) { report(x); } }\n"
    "    var a = (listeners(t)[e.type] || []).slice();\n"
    "    for (var i = 0; i < a.length && !e.stopped; i++) {\n"
    "      try { if (typeof a[i] === 'function') a[i].call(t, e); else a[i].handleEvent(e); } catch (x) { report(x); } }\n"
    "    return !e.defaultPrevented; };\n"
    "  function Illegal() { throw new TypeError('Illegal constructor'); }\n"
    "  var WorkerGlobalScope = function WorkerGlobalScope() { Illegal(); };\n"
    "  WorkerGlobalScope.prototype = Object.create(EventTarget.prototype);\n"
    "  var DedicatedWorkerGlobalScope = function DedicatedWorkerGlobalScope() { Illegal(); };\n"
    "  DedicatedWorkerGlobalScope.prototype = Object.create(WorkerGlobalScope.prototype);\n"
    "  var WorkerLocation = function WorkerLocation() { Illegal(); };\n"
    "  WorkerLocation.prototype.toString = function () { return this.href; };\n"
    "  var WorkerNavigator = function WorkerNavigator() { Illegal(); };\n"
    "  Object.setPrototypeOf(loc, WorkerLocation.prototype); Object.setPrototypeOf(nav, WorkerNavigator.prototype);\n"
    "\n"
    "  /* MessageChannel: a message to the other port, as a task of its own. */\n"
    "  function MessagePort() { this.onmessage = null; Object.defineProperty(this, '__other', { value: null, writable: true }); Object.defineProperty(this, '__closed', { value: false, writable: true }); }\n"
    "  MessagePort.prototype = Object.create(EventTarget.prototype); MessagePort.prototype.constructor = MessagePort;\n"
    "  MessagePort.prototype.postMessage = function (data) {\n"
    "    var o = this.__other; if (!o || this.__closed) return; var d = structuredClone(data);\n"
    "    g.setTimeout(function () { if (!o.__closed) o.dispatchEvent(new MessageEvent('message', { data: d })); }, 0); };\n"
    "  MessagePort.prototype.start = function () {};\n"
    "  MessagePort.prototype.close = function () { this.__closed = true; };\n"
    "  function MessageChannel() { this.port1 = new MessagePort(); this.port2 = new MessagePort();\n"
    "    this.port1.__other = this.port2; this.port2.__other = this.port1; }\n"
    "\n"
    "  /* AbortController and AbortSignal. */\n"
    "  function AbortSignal() { this.aborted = false; this.reason = undefined; this.onabort = null; }\n"
    "  AbortSignal.prototype = Object.create(EventTarget.prototype); AbortSignal.prototype.constructor = AbortSignal;\n"
    "  AbortSignal.prototype.throwIfAborted = function () { if (this.aborted) throw this.reason; };\n"
    "  function abortReason(r) { return r === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : r; }\n"
    "  AbortSignal.abort = function (r) { var s = new AbortSignal(); s.aborted = true; s.reason = abortReason(r); return s; };\n"
    "  AbortSignal.timeout = function (ms) { var s = new AbortSignal();\n"
    "    g.setTimeout(function () { if (!s.aborted) { s.aborted = true; s.reason = new DOMException('signal timed out', 'TimeoutError');\n"
    "      s.dispatchEvent(new Event('abort')); } }, ms); return s; };\n"
    "  function AbortController() { this.signal = new AbortSignal(); }\n"
    "  AbortController.prototype.abort = function (r) { var s = this.signal; if (s.aborted) return;\n"
    "    s.aborted = true; s.reason = abortReason(r); s.dispatchEvent(new Event('abort')); };\n"
    "\n"
    "  /* URLSearchParams and URL, parsed by the page's own parser (parseURL). */\n"
    "  function dec(s) { try { return decodeURIComponent(s.replace(/\\+/g, ' ')); } catch (e) { return s; } }\n"
    "  function enc(s) { return encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, function (c) {\n"
    "    return '%' + c.charCodeAt(0).toString(16).toUpperCase(); }); }\n"
    "  function URLSearchParams(init) {\n"
    "    Object.defineProperty(this, '__list', { value: [], writable: true });\n"
    "    Object.defineProperty(this, '__url', { value: null, writable: true });\n"
    "    if (init == null) return;\n"
    "    if (init instanceof URLSearchParams) { this.__list = init.__list.map(function (p) { return [p[0], p[1]]; }); return; }\n"
    "    if (typeof init === 'object') {\n"
    "      if (typeof init[Symbol.iterator] === 'function') { for (var p of init) this.__list.push([String(p[0]), String(p[1])]); }\n"
    "      else for (var k in init) if (Object.prototype.hasOwnProperty.call(init, k)) this.__list.push([k, String(init[k])]);\n"
    "      return;\n"
    "    }\n"
    "    var s = String(init); if (s[0] === '?') s = s.slice(1);\n"
    "    var parts = s.split('&');\n"
    "    for (var i = 0; i < parts.length; i++) { if (!parts[i]) continue; var e = parts[i].indexOf('=');\n"
    "      this.__list.push(e < 0 ? [dec(parts[i]), ''] : [dec(parts[i].slice(0, e)), dec(parts[i].slice(e + 1))]); }\n"
    "  }\n"
    "  function touched(sp) { if (sp.__url) { var s = sp.toString(); sp.__url.__p.search = s ? '?' + s : ''; } }\n"
    "  URLSearchParams.prototype.append = function (k, v) { this.__list.push([String(k), String(v)]); touched(this); };\n"
    "  URLSearchParams.prototype.delete = function (k) { k = String(k); this.__list = this.__list.filter(function (p) { return p[0] !== k; }); touched(this); };\n"
    "  URLSearchParams.prototype.get = function (k) { k = String(k); for (var i = 0; i < this.__list.length; i++) if (this.__list[i][0] === k) return this.__list[i][1]; return null; };\n"
    "  URLSearchParams.prototype.getAll = function (k) { k = String(k); return this.__list.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); };\n"
    "  URLSearchParams.prototype.has = function (k) { return this.get(k) !== null; };\n"
    "  URLSearchParams.prototype.set = function (k, v) { k = String(k); v = String(v); var done = false;\n"
    "    this.__list = this.__list.filter(function (p) { if (p[0] !== k) return true; if (done) return false; p[1] = v; done = true; return true; });\n"
    "    if (!done) this.__list.push([k, v]); touched(this); };\n"
    "  URLSearchParams.prototype.sort = function () { this.__list = this.__list.map(function (p, i) { return [p, i]; })\n"
    "    .sort(function (a, b) { return a[0][0] < b[0][0] ? -1 : a[0][0] > b[0][0] ? 1 : a[1] - b[1]; }).map(function (x) { return x[0]; }); touched(this); };\n"
    "  URLSearchParams.prototype.toString = function () { return this.__list.map(function (p) { return enc(p[0]) + '=' + enc(p[1]); }).join('&'); };\n"
    "  URLSearchParams.prototype.forEach = function (f, t) { for (var i = 0; i < this.__list.length; i++) f.call(t, this.__list[i][1], this.__list[i][0], this); };\n"
    "  URLSearchParams.prototype.entries = function () { return this.__list.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); };\n"
    "  URLSearchParams.prototype.keys = function () { return this.__list.map(function (p) { return p[0]; })[Symbol.iterator](); };\n"
    "  URLSearchParams.prototype.values = function () { return this.__list.map(function (p) { return p[1]; })[Symbol.iterator](); };\n"
    "  URLSearchParams.prototype[Symbol.iterator] = URLSearchParams.prototype.entries;\n"
    "  Object.defineProperty(URLSearchParams.prototype, 'size', { get: function () { return this.__list.length; } });\n"
    "\n"
    "  function URL(url, base) {\n"
    "    var p = parseURL(String(url), base === undefined ? undefined : String(base));\n"
    "    if (!p) throw new TypeError(\"Failed to construct 'URL': Invalid URL\");\n"
    "    Object.defineProperty(this, '__p', { value: p, writable: true });\n"
    "    var sp = new URLSearchParams(p.search); sp.__url = this;\n"
    "    Object.defineProperty(this, '__sp', { value: sp, writable: true });\n"
    "  }\n"
    "  function href(p) { return p.protocol + (p.host || p.protocol === 'file:' ? '//' : '') +\n"
    "    (p.username ? p.username + (p.password ? ':' + p.password : '') + '@' : '') + p.host + p.pathname + p.search + p.hash; }\n"
    "  ['protocol', 'username', 'password', 'hostname', 'port', 'pathname', 'hash'].forEach(function (k) {\n"
    "    Object.defineProperty(URL.prototype, k, { get: function () { return this.__p[k]; },\n"
    "      set: function (v) { this.__p[k] = String(v); if (k === 'hostname' || k === 'port')\n"
    "        this.__p.host = this.__p.hostname + (this.__p.port ? ':' + this.__p.port : ''); }, enumerable: true }); });\n"
    "  Object.defineProperty(URL.prototype, 'host', { get: function () { return this.__p.host; }, enumerable: true });\n"
    "  Object.defineProperty(URL.prototype, 'origin', { get: function () { return this.__p.origin; }, enumerable: true });\n"
    "  Object.defineProperty(URL.prototype, 'search', { get: function () { return this.__p.search; },\n"
    "    set: function (v) { v = String(v); this.__p.search = v && v[0] !== '?' ? '?' + v : v === '?' ? '' : v;\n"
    "      var sp = new URLSearchParams(this.__p.search); sp.__url = this; this.__sp = sp; }, enumerable: true });\n"
    "  Object.defineProperty(URL.prototype, 'searchParams', { get: function () { return this.__sp; }, enumerable: true });\n"
    "  Object.defineProperty(URL.prototype, 'href', { get: function () { return href(this.__p); },\n"
    "    set: function (v) { var p = parseURL(String(v)); if (!p) throw new TypeError('Invalid URL'); this.__p = p;\n"
    "      var sp = new URLSearchParams(p.search); sp.__url = this; this.__sp = sp; }, enumerable: true });\n"
    "  URL.prototype.toString = URL.prototype.toJSON = function () { return href(this.__p); };\n"
    "  URL.canParse = function (u, b) { return !!parseURL(String(u), b === undefined ? undefined : String(b)); };\n"
    "  URL.parse = function (u, b) { try { return new URL(u, b); } catch (e) { return null; } };\n"
    "\n"
    "  function Headers(init) { this.map = {}; if (init) for (var k in init) this.map[String(k).toLowerCase()] = String(init[k]); }\n"
    "  Headers.prototype.get = function (n) { var v = this.map[String(n).toLowerCase()]; return v === undefined ? null : v; };\n"
    "  Headers.prototype.has = function (n) { return String(n).toLowerCase() in this.map; };\n"
    "  Headers.prototype.set = function (n, v) { this.map[String(n).toLowerCase()] = String(v); };\n"
    "  Headers.prototype.forEach = function (f, t) { for (var k in this.map) f.call(t, this.map[k], k, this); };\n"
    "  function Response(body, init) { init = init || {}; this.body = null; this.bodyText = body == null ? '' : String(body);\n"
    "    this.status = init.status === undefined ? 200 : init.status; this.statusText = init.statusText || '';\n"
    "    this.ok = this.status >= 200 && this.status < 300; this.url = init.url || ''; this.redirected = false;\n"
    "    this.type = 'basic'; this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);\n"
    "    this.bodyUsed = false; }\n"
    "  Response.prototype.text = function () { this.bodyUsed = true; return Promise.resolve(this.bodyText); };\n"
    "  Response.prototype.json = function () { var t = this.bodyText; this.bodyUsed = true;\n"
    "    return new Promise(function (ok) { ok(JSON.parse(t)); }); };\n"
    "  Response.prototype.arrayBuffer = function () { this.bodyUsed = true; return Promise.resolve(toBuffer(this.bodyText)); };\n"
    "  Response.prototype.clone = function () { return new Response(this.bodyText, this); };\n"
    "\n"
    "  g.addEventListener = EventTarget.prototype.addEventListener;\n"
    "  g.removeEventListener = EventTarget.prototype.removeEventListener;\n"
    "  g.dispatchEvent = EventTarget.prototype.dispatchEvent;\n"
    "  g.onmessage = null; g.onmessageerror = null; g.onerror = null;\n"
    "  g.Event = Event; g.MessageEvent = MessageEvent; g.ErrorEvent = ErrorEvent; g.EventTarget = EventTarget;\n"
    "  g.WorkerGlobalScope = WorkerGlobalScope; g.DedicatedWorkerGlobalScope = DedicatedWorkerGlobalScope;\n"
    "  g.WorkerLocation = WorkerLocation; g.WorkerNavigator = WorkerNavigator;\n"
    "  g.MessageChannel = MessageChannel; g.MessagePort = MessagePort;\n"
    "  g.AbortController = AbortController; g.AbortSignal = AbortSignal;\n"
    "  g.URL = URL; g.URLSearchParams = URLSearchParams;\n"
    "  g.Headers = Headers; g.Response = Response; g.location = loc; g.navigator = nav;\n"
    "  Object.setPrototypeOf(g, DedicatedWorkerGlobalScope.prototype);\n"
    "  return {\n"
    "    deliver: function (type, data) { var e = new MessageEvent(type, { data: data }); e.isTrusted = true; g.dispatchEvent(e); },\n"
    "    response: function (status, url, type, body) { var h = new Headers(); if (type) h.set('content-type', type);\n"
    "      return new Response(body, { status: status, url: url, headers: h }); }\n"
    "  };\n"
    "})\n";

static void jw_put(jctx *W, jobj *o, const char *name, jval v) {
    if (o) js_set(W, o, name, v);
    else js_declare(W, W->global, js_str(W, name), v);
}

static void jw_native(jctx *W, jobj *o, const char *name, jnative fn, int arity) {
    jobj *f = js_native_n(W, name, fn, arity);
    if (f) jw_put(W, o, name, js_from_obj(f));
}

/* location, from the worker's own address. */
static jobj *jw_location(jctx *W, const char *url) {
    jobj *loc = js_object(W, JO_PLAIN);
    jurl *u = (jurl *)malloc(sizeof(jurl));
    if (!loc) { free(u); return 0; }
    js_set(W, loc, "href", js_from_str(js_str(W, url)));
    if (u && ju_parse(url, 0, u)) {
        char buf[URL_TEXT];
        int k = w_len(u->scheme);
        w_copy(buf, (int)sizeof(buf), u->scheme, (int)sizeof(buf));
        if (k < (int)sizeof(buf) - 1) { buf[k] = ':'; buf[k + 1] = 0; }
        js_set(W, loc, "protocol", js_from_str(js_str(W, buf)));
        js_set(W, loc, "hostname", js_from_str(js_str(W, u->host)));
        js_set(W, loc, "origin", js_from_str(ju_origin(W, u)));
        jtext t = { 0, 0, 0, 0 };
        jt_put(W, &t, u->host, (u32)w_len(u->host));
        if (u->port >= 0 && u->port != ju_default_port(u->scheme)) { jd_putc(&t, ':'); jd_put_num(&t, u->port); }
        js_set(W, loc, "host", js_from_str(jt_done(W, &t)));
        char port[12] = "";
        if (u->port >= 0 && u->port != ju_default_port(u->scheme)) {
            int p = u->port, w = 0;
            char rev[12];
            do { rev[w++] = (char)('0' + p % 10); p /= 10; } while (p && w < 11);
            for (int i = 0; i < w; i++) port[i] = rev[w - 1 - i];
            port[w] = 0;
        }
        js_set(W, loc, "port", js_from_str(js_str(W, port)));
        js_set(W, loc, "pathname", js_from_str(js_str(W, u->path)));
        jtext q = { 0, 0, 0, 0 };
        if (u->has_query && u->query[0]) { jd_putc(&q, '?'); jt_put(W, &q, u->query, (u32)w_len(u->query)); }
        js_set(W, loc, "search", js_from_str(jt_done(W, &q)));
        jtext h = { 0, 0, 0, 0 };
        if (u->has_frag && u->frag[0]) { jd_putc(&h, '#'); jt_put(W, &h, u->frag, (u32)w_len(u->frag)); }
        js_set(W, loc, "hash", js_from_str(jt_done(W, &h)));
    } else {
        static const char *const NONE[] = { "protocol", "host", "hostname", "port", "pathname", "search", "hash", 0 };
        for (int i = 0; NONE[i]; i++) js_set(W, loc, NONE[i], js_from_str(js_str(W, "")));
        js_set(W, loc, "origin", js_from_str(js_str(W, "null")));
    }
    free(u);
    return loc;
}

/* navigator, what the page's says. */
static jobj *jw_navigator(jctx *W) {
    jobj *nav = js_object(W, JO_PLAIN);
    if (!nav) return 0;
    static const char *const SAME[] = { "userAgent", "language", "platform", "appName", "appVersion", "vendor",
                                        "product", "hardwareConcurrency", "onLine", 0 };
    jval page_nav = js_get(&jd_J, js_from_obj(jd_J.global_obj), js_str(&jd_J, "navigator"));
    for (int i = 0; SAME[i] && js_is_obj(page_nav); i++) {
        jval v = js_get(&jd_J, page_nav, js_str(&jd_J, SAME[i]));
        if (jd_J.sig != JS_OK) { jd_J.sig = JS_OK; continue; }
        js_set(W, nav, SAME[i], jsc_across(&jd_J, W, v));
        jd_J.sig = JS_OK;
        W->sig = JS_OK;
    }
    jobj *langs = js_array(W);
    if (langs) {
        js_arr_push(W, langs, js_from_str(js_str(W, "en-US")));
        js_arr_push(W, langs, js_from_str(js_str(W, "en")));
        js_set(W, nav, "languages", js_from_obj(langs));
    }
    return nav;
}

/* A worker's context and globals, in a free slot: the slot, or -1. */
static int jw_new(const char *url, const char *name) {
    int slot = -1;
    for (int i = 0; i < JW_MAX && slot < 0; i++) if (!jw[i].J) slot = i;
    if (slot < 0) return -1;
    /* Out of the same free memory as the page's own share (jd_mem_cap), and
       none at all when there is too little left for one to run in. */
    long long free_b = jd_free_memory ? jd_free_memory() : -1;
    if (free_b >= 0 && free_b < 24ll * 1024 * 1024) return -1;
    jctx *W = (jctx *)malloc(sizeof(jctx));
    if (!W) return -1;
    js_init(W);
    if (!W->global) { js_done(W); free(W); return -1; }
    /* Half what a page is given now. */
    W->mem_cap = jd_mem_cap() / 2;
    jworker *w = &jw[slot];
    w->J = W;
    w->gen = ++jw_gens;
    w->closed = 0;
    w->next_timer = 0;
    w_copy(w->url, (int)sizeof(w->url), url, (int)sizeof(w->url));
    w->inbox = js_array(W);

    jw_native(W, 0, "postMessage", nat_jw_post, 1);
    jw_native(W, 0, "close", nat_jw_close, 0);
    jw_native(W, 0, "importScripts", nat_jw_import, 0);
    jw_native(W, 0, "setTimeout", nat_jw_set_timeout, 1);
    jw_native(W, 0, "setInterval", nat_jw_set_interval, 1);
    jw_native(W, 0, "clearTimeout", nat_jw_clear_timer, 1);
    jw_native(W, 0, "clearInterval", nat_jw_clear_timer, 1);
    jw_native(W, 0, "fetch", nat_jw_fetch, 1);
    jw_put(W, 0, "name", js_from_str(js_str(W, name ? name : "")));
    jw_put(W, 0, "isSecureContext", js_bool(jd_page_secure()));
    jobj *perf = js_object(W, JO_PLAIN);
    jw_native(W, perf, "now", nat_perf_now, 0);
    if (perf) js_set(W, perf, "timeOrigin", js_num(jd_time_origin));
    jw_put(W, 0, "performance", js_from_obj(perf));
    jobj *cr = js_object(W, JO_PLAIN);
    jw_native(W, cr, "getRandomValues", nat_crypto_values, 1);
    if (jd_page_secure()) jw_native(W, cr, "randomUUID", nat_crypto_uuid, 0);
    jw_put(W, 0, "crypto", js_from_obj(cr));

    jval prelude = js_eval_source(W, js_str(W, JW_PRELUDE), W->global, W->global_lex, js_undef());
    jobj *rep = js_native_n(W, "report", nat_jw_report, 1), *tb = js_native_n(W, "toBuffer", nat_jw_to_buffer, 1);
    jobj *pu = js_native_n(W, "parseURL", nat_jw_parse_url, 2);
    jval args[6] = { js_from_obj(W->global_obj), js_from_obj(rep), js_from_obj(tb), js_from_obj(pu),
                     js_from_obj(jw_location(W, url)), js_from_obj(jw_navigator(W)) };
    jval got = W->sig == JS_OK && js_callable(prelude) ? js_call(W, prelude, js_undef(), args, 6) : js_undef();
    if (W->sig != JS_OK || !js_is_obj(got)) { jw_free_slot(slot); return -1; }
    w->deliver = js_get(W, got, js_str(W, "deliver"));
    w->response = js_get(W, got, js_str(W, "response"));
    return slot;
}

/* --- the page's side ---------------------------------------------------------------------- */

static void jw_start_due(jval arg) {
    int slot = jw_named(arg.num);
    if (slot < 0) return;
    jworker *w = &jw[slot];
    jctx *W = w->J;
    char *src = w->source;
    int len = w->len;
    w->source = 0;
    if (!src) return;
    W->sig = JS_OK;
    int ok = js_run(W, src, (u32)len);
    free(src);
    if (!ok && W->sig != JS_THROWN) {
        char buf[200];
        w_copy(buf, (int)sizeof(buf), W->error[0] ? W->error : "the worker's script did not run", (int)sizeof(buf));
        W->sig = JS_OK;
        jw_report(slot, buf, W->error_line);
    }
    jw_after(slot);
}

static void jw_inbox_due(jval arg) {
    int slot = jw_named(arg.num);
    if (slot < 0) return;
    jworker *w = &jw[slot];
    jctx *W = w->J;
    if (!w->inbox || !w->inbox->len) return;
    jval data = w->inbox->items[0];
    for (u32 i = 0; i + 1 < w->inbox->len; i++) w->inbox->items[i] = w->inbox->items[i + 1];
    w->inbox->len--;
    jval args[2] = { js_from_str(js_str(W, "message")), data };
    W->sig = JS_OK;
    js_call(W, w->deliver, js_undef(), args, 2);
    jw_after(slot);
}

/* The error event for a worker that could not be made or whose script would
   not come, as a task, since the constructor itself does not fail. */
static void jw_fail_later(jobj *worker, const char *why) {
    if (js_print_hook) {
        js_print_hook("a worker did not start: ", 24);
        js_print_hook(why, (u32)w_len(why));
        js_print_hook("\n", 1);
    }
    jobj *job = js_array(&jd_J);
    if (!job) return;
    js_arr_push(&jd_J, job, js_from_obj(worker));
    js_arr_push(&jd_J, job, jd_str(why));
    js_arr_push(&jd_J, job, js_num(0));
    jd_later_native(jw_error_due, js_from_obj(job), 0);
}

static int jw_of(jval t) {
    jval v = js_is_obj(t) ? jd_kept(t.obj, jw_k_slot) : js_undef();
    return v.t == JS_NUM ? jw_named(v.num) : -1;
}

static jval nat_worker_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "Worker is made with new", J->error_line);
    jstr *href = js_to_str(J, js_arg(a, n, 0));
    jval opt = js_arg(a, n, 1);
    jval nm = js_is_obj(opt) ? js_get(J, opt, js_str(J, "name")) : js_undef();
    if (J->sig != JS_OK || !href) return js_undef();
    jstr *url = jd_resolve_str(J, href->s);
    if (!url) return js_throw_dom(J, "SyntaxError", "a worker's address that does not parse");
    jstr *name = nm.t == JS_UNDEF ? 0 : js_to_str(J, nm);
    int len = 0;
    char *src = jw_script_text(url->s, &len);
    if (!src) { jw_fail_later(t.obj, "its script would not come"); return t; }
    int slot = jw_new(url->s, name ? name->s : "");
    if (slot < 0) { free(src); jw_fail_later(t.obj, "no room for another worker"); return t; }
    jw[slot].worker = t.obj;
    jw[slot].source = src;
    jw[slot].len = len;
    jd_keep(t.obj, jw_k_slot, js_num(jw_name(slot)));
    jd_later_native(jw_start_due, js_num(jw_name(slot)), 0);
    return t;
}

static jval nat_worker_post(jctx *J, jval t, jval *a, int n) {
    int slot = jw_of(t);
    if (slot < 0) return js_undef();
    jworker *w = &jw[slot];
    jval data = jsc_across(J, w->J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    w->J->sig = JS_OK;
    js_arr_push(w->J, w->inbox, data);
    jd_later_native(jw_inbox_due, js_num(jw_name(slot)), 0);
    return js_undef();
}

static jval nat_worker_terminate(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int slot = jw_of(t);
    if (slot >= 0) { jw_ended[slot] = jw[slot].gen; jw_free_slot(slot); }
    return js_undef();
}

static void jd_setup_workers(jctx *J) {
    jw_close_all();
    jw_k_slot = js_sym_new(J, "worker", 6);
    jobj *wp = jd_interface(J, "Worker", jd_p[JI_EVENTTARGET], nat_worker_ctor, 1);
    jd_method(J, wp, "postMessage", nat_worker_post, 1);
    jd_method(J, wp, "terminate", nat_worker_terminate, 0);
}
