#pragma once
/* A page asking the network, after it has been read: XMLHttpRequest and
 * fetch, their headers, requests and responses, a form's data as a body,
 * AbortController, and the queue both go through.
 *
 * Included from jsdom.h. Every request is made on the browser's next pass
 * rather than inside the call that asked for it, so the code after send()
 * or fetch() runs first, as the page is written to expect, and a reply
 * arrives the way it would anywhere: afterwards. The fetch itself blocks the
 * browser while it happens, which is a stall rather than a lie -- nothing is
 * told it finished before it did.
 *
 * What goes out goes through the browser (jd_do_request; browser.c,
 * do_request), which sends the jar's cookies for the address as every
 * request does and knows two methods, GET and POST. A request for another
 * is refused, as a failed fetch, rather than sent as one of those.
 */

/* One reply, as the browser hands it back. */
typedef struct {
    const char *body;
    int  len;
    int  status;
    const char *type;            /* the Content-Type it came with, or none */
    const char *url;             /* where it ended up, after redirects */
} jd_reply;

/* How one is made. Set by the browser, for the same reason the script
   fetch is: the network and the page's address are its business. `type` is
   what the body is, when there is one and it is not a form. */
static int (*jd_do_request)(const char *method, const char *url, const char *body,
                            const char *type, jd_reply *out);

void jsdom_request_with(int (*fn)(const char *, const char *, const char *, const char *, jd_reply *)) {
    jd_do_request = fn;
}

/* --- the queue ------------------------------------------------------------------------- */

#define JD_REQUESTS 32
enum { JQ_XHR = 1, JQ_FETCH, JQ_BEACON };

typedef struct {
    jobj *self;                  /* the request, which holds its own state */
    int   kind, waiting;
} jxhr;

static jxhr jd_req[JD_REQUESTS];
static int  jd_nreq;

static int jd_requests_waiting(void) {
    for (int i = 0; i < jd_nreq; i++) if (jd_req[i].waiting) return 1;
    return 0;
}

static int jd_queue(jobj *self, int kind) {
    int slot = -1;
    for (int i = 0; i < jd_nreq; i++) if (!jd_req[i].waiting) { slot = i; break; }
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

static int jd_method_ok(const char *m) {
    return w_same_fold(m, "GET") || w_same_fold(m, "POST");
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

static jstr *jd_body_of(jctx *J, jval b, const char **type) {
    *type = 0;
    if (b.t == JS_UNDEF || b.t == JS_NULL) return 0;
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
        *type = "application/octet-stream";
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
        char out[URL_TEXT + 256];
        url = jd_resolve(s->s, out, (int)sizeof(out)) ? js_str(J, out) : s;
    }
    if (js_is_obj(init)) {
        jval m = js_get(J, init, js_str(J, "method"));
        if (m.t != JS_UNDEF) {
            jstr *ms = js_to_str(J, m);
            char up[16];
            int k = 0;
            for (; ms && k < (int)ms->len && k < 15; k++) up[k] = (ms->s[k] >= 'a' && ms->s[k] <= 'z') ? (char)(ms->s[k] - 32) : ms->s[k];
            up[k] = 0;
            method = js_str(J, up);
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

/* A response's body: text, parsed JSON, or bytes, read once. */
static jval jd_response_body(jctx *J, jval t) {
    if (!js_is_obj(t)) return js_undef();
    jval used = js_get(J, t, js_str(J, "bodyUsed"));
    if (js_to_bool(used)) {
        js_throw(J, JS_ERR_TYPE, "that response's body has been read already", J->error_line);
        return js_undef();
    }
    js_define(J, t.obj, js_str(J, "bodyUsed"), js_bool(1), JP_ENUM | JP_CONF);
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
    js_define(J, r, js_str(J, "status"), js_num(status), JP_ENUM | JP_CONF);
    js_define(J, r, js_str(J, "ok"), js_bool(status >= 200 && status < 300), JP_ENUM | JP_CONF);
    js_define(J, r, js_str(J, "statusText"), js_from_str(status_text ? status_text : js_str(J, "")), JP_ENUM | JP_CONF);
    js_define(J, r, js_str(J, "headers"), js_from_obj(headers ? headers : jd_new_headers(J)), JP_ENUM | JP_CONF);
    js_define(J, r, js_str(J, "url"), js_from_str(url ? url : js_str(J, "")), JP_ENUM | JP_CONF);
    js_define(J, r, js_str(J, "redirected"), js_bool(redirected), JP_ENUM | JP_CONF);
    js_define(J, r, js_str(J, "type"), jd_str(status ? "basic" : "error"), JP_ENUM | JP_CONF);
    js_define(J, r, js_str(J, "bodyUsed"), js_bool(0), JP_ENUM | JP_CONF);
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
    else if (!jd_method_ok(jd_kept_str(r, jd_k_method, "GET")))
        jd_refuse(J, p, "this browser sends only GET and POST");
    else if (!jd_queue(r, JQ_FETCH))
        jd_refuse(J, p, "too many requests at once");
    return js_from_obj(p);
}

/* A fetch whose signal was aborted before it was made is refused now. */
static void jd_fetch_aborted(jctx *J, jobj *sig) {
    for (int i = 0; i < jd_nreq; i++) {
        if (!jd_req[i].waiting || jd_req[i].kind != JQ_FETCH) continue;
        jobj *r = jd_req[i].self;
        jval s = jd_kept(r, jd_k_rsig);
        if (!js_is_obj(s) || s.obj != sig) continue;
        jd_req[i].waiting = 0;
        jval p = jd_kept(r, jd_k_promise);
        if (js_is_obj(p)) js_promise_settle(J, p.obj, 0, jd_kept(sig, jd_k_reason));
    }
}

/* The reply's headers, as far as the browser keeps them: its type. */
static jobj *jd_reply_headers(jctx *J, const jd_reply *rp) {
    jobj *h = jd_new_headers(J);
    if (h && rp->type && rp->type[0])
        jd_hlist_add(J, jd_hlist_of(J, js_from_obj(h)), js_str(J, "content-type"), js_str(J, rp->type));
    return h;
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
                                  rp->status, 0, jd_reply_headers(J, rp), url, redirected);
    js_promise_settle(J, p.obj, 1, js_from_obj(resp));
}

/* --- XMLHttpRequest ------------------------------------------------------------------------ */

static jval nat_xhr_ctor(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "XMLHttpRequest is made with new", J->error_line);
    jobj *o = t.obj;
    js_set(J, o, "readyState", js_num(0));
    js_set(J, o, "status", js_num(0));
    js_set(J, o, "statusText", jd_str(""));
    js_set(J, o, "responseText", jd_str(""));
    js_set(J, o, "response", jd_str(""));
    js_set(J, o, "responseType", jd_str(""));
    js_set(J, o, "responseURL", jd_str(""));
    js_set(J, o, "responseXML", js_null());
    js_set(J, o, "timeout", js_num(0));
    js_set(J, o, "withCredentials", js_bool(0));
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

static jval nat_xhr_open(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jstr *m = jd_arg_str(J, a, n, 0);
    jstr *u = jd_arg_str(J, a, n, 1);
    char out[URL_TEXT + 256];
    jd_keep(t.obj, jd_k_method, js_from_str(m));
    jd_keep(t.obj, jd_k_url, js_from_str(jd_resolve(u->s, out, (int)sizeof(out)) ? js_str(J, out) : u));
    jobj *list = js_array(J);
    if (list) jd_keep(t.obj, jd_k_hlist, js_from_obj(list));
    js_set(J, t.obj, "readyState", js_num(1));
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
    return js_undef();
}

static jval nat_xhr_abort(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (!js_is_obj(t)) return js_undef();
    int was = 0;
    for (int i = 0; i < jd_nreq; i++)
        if (jd_req[i].waiting && jd_req[i].self == t.obj) { jd_req[i].waiting = 0; was = 1; }
    if (was) {
        js_set(J, t.obj, "readyState", js_num(4));
        jd_xhr_event(t.obj, "readystatechange");
        jd_xhr_event(t.obj, "abort");
        jd_xhr_event(t.obj, "loadend");
        js_set(J, t.obj, "readyState", js_num(0));
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
    jobj *headers = jd_reply_headers(J, rp);
    if (headers) jd_keep(o, jd_k_rheaders, js_from_obj(headers));
    js_set(J, o, "status", js_num(rp->status > 0 ? rp->status : 0));
    js_set(J, o, "readyState", js_num(4));
    js_set(J, o, "responseText", text);
    js_set(J, o, "response", resp);
    js_set(J, o, "responseURL", jd_str(rp->url && rp->url[0] ? rp->url : jd_kept_str(o, jd_k_url, "")));
    jd_xhr_event(o, "readystatechange");
    /* load either way when the server answered: a page that asked for
       something it did not get is entitled to find out, and status is where
       it looks. No answer at all is an error. */
    jd_xhr_event(o, rp->status > 0 ? "load" : "error");
    jd_xhr_event(o, "loadend");
}

/* Whatever was sent, made. One per pass, because each one blocks the
   browser while it happens and a page that sent six would otherwise stop
   for all six at once. */
__attribute__((unused)) static int jsdom_requests(void) {
    if (!jd_open || jd_spent()) return 0;
    for (int i = 0; i < jd_nreq; i++) {
        if (!jd_req[i].waiting) continue;
        jobj *o = jd_req[i].self;
        int kind = jd_req[i].kind;
        jd_req[i].waiting = 0;
        if (!o) continue;
        const char *method = jd_kept_str(o, jd_k_method, "GET");
        const char *url = jd_kept_str(o, jd_k_url, "");
        const char *body = jd_kept_str(o, jd_k_body, 0);
        const char *type = jd_kept_str(o, jd_k_type, 0);
        if (kind == JQ_FETCH) {
            jval h = js_get(&jd_J, js_from_obj(o), js_str(&jd_J, "headers"));
            jd_J.sig = JS_OK;
            if (js_is_obj(h)) {
                jobj *list = jd_hlist_of(&jd_J, h);
                jval ct = list ? jd_headers_get(&jd_J, list, js_str(&jd_J, "content-type")) : js_undef();
                if (ct.t == JS_STR) type = ct.str->s;
            }
        }
        jd_reply rp = { 0, 0, 0, 0, 0 };
        if (url[0] && jd_do_request && (kind != JQ_XHR || jd_method_ok(method)))
            jd_do_request(method, url, body, type, &rp);
        if (kind == JQ_FETCH) jd_fetch_done(o, &rp);
        else jd_xhr_done(o, &rp);
        /* A promise settled from here, outside any call into the page, has
           its reactions run now, as they would be at the end of a task. */
        js_drain(&jd_J);
        return 1;
    }
    return 0;
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
