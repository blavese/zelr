#pragma once
/* The window, beyond the document: what the browser is, and what it keeps.
 *
 * Included from jsdom.h, whose world, hooks and helpers everything here uses.
 * Every answer is this browser's own. navigator.userAgent is exactly what it
 * sends (web.h, WEB_USER_AGENT); the platform is zelr; the number of
 * processors is read from the machine or not given at all; there are no
 * plugins, no touch, no PDF viewer and no automation, and each of those is
 * said the way the standard lets a browser say it has none. A page asks
 * these questions to decide what to do, and an answer borrowed from another
 * browser sends it down that browser's path.
 */

static jval nat_list_values_empty(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    return jd_array_iter(J, js_array(J), "values");
}

/* --- navigator -------------------------------------------------------------------------- */

static jval nat_nav_ua(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_str(WEB_USER_AGENT);
}

/* The standard's fixed answers, the same in every browser that follows it:
   they name nobody (HTML, "navigator.appCodeName" and its kin). */
static jval nat_nav_appcodename(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return jd_str("Mozilla"); }
static jval nat_nav_appname(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return jd_str("Netscape"); }
static jval nat_nav_appversion(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return jd_str("4.0"); }
static jval nat_nav_product(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return jd_str("Gecko"); }

static jval nat_nav_platform(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_str("zelr");
}

/* The language the machine speaks: its every word is British English, and
   there is no setting to make it another. */
#define JD_LANGUAGE "en-GB"

static jval nat_nav_language(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_str(JD_LANGUAGE);
}

static jobj *jd_languages;

static jval nat_nav_languages(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    if (!jd_languages) {
        jd_languages = js_array(J);
        if (jd_languages) {
            js_arr_push(J, jd_languages, jd_str(JD_LANGUAGE));
            jd_languages->flags |= JOF_FROZEN;
        }
    }
    return js_from_obj(jd_languages);
}

/* There is a jar (fetch.h), and a page reached over the network is online:
   it was fetched a moment ago. */
static jval nat_nav_true(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_bool(1); }
static jval nat_nav_false(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_bool(0); }
static jval nat_nav_zero(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_num(0); }

/* What the machine is, as the kernel tells a program (sdk: sysinfo): the
   processors running and the screen's size. Asked once. */
static zelr_sysinfo jd_sys;
static int jd_sys_asked, jd_sys_ok;

static const zelr_sysinfo *jd_machine(void) {
    if (!jd_sys_asked) {
        jd_sys_asked = 1;
        volatile u8 *z = (volatile u8 *)&jd_sys;
        for (u32 i = 0; i < sizeof(jd_sys); i++) z[i] = 0;
        jd_sys_ok = sysinfo(&jd_sys) >= 0;
    }
    return jd_sys_ok ? &jd_sys : 0;
}

/* The processors running, or -1 where the kernel does not say (the host
   build), in which case the property is not there at all rather than a
   number made up. */
static int jd_cpu_count(void) {
    const zelr_sysinfo *m = jd_machine();
    return m && m->cpus_started > 0 ? (int)m->cpus_started : -1;
}

static jval nat_nav_cpus(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_num(jd_cpu_count());
}

/* Plugins and the kinds they handle: none. Since there is no PDF viewer,
   the standard has both lists empty, which is the truth. */
static jval nat_empty_item(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_null();
}

static jobj *jd_empty_list(jctx *J, const char *iface) {
    jobj *p = jd_interface(J, iface, 0, 0, 0);
    if (!p) return 0;
    js_const_prop(J, p, "length", js_num(0));
    jd_method(J, p, "item", nat_empty_item, 1);
    jd_method(J, p, "namedItem", nat_empty_item, 1);
    jd_method(J, p, "refresh", nat_nothing_js, 0);
    jobj *o = js_object_with(J, JO_PLAIN, p);
    js_method_key(J, p, J->sym_iterator, "[Symbol.iterator]", nat_list_values_empty, 0);
    return o;
}

static jobj *jd_plugins, *jd_mimetypes;

static jval nat_nav_plugins(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_plugins ? js_from_obj(jd_plugins) : js_null();
}

static jval nat_nav_mimetypes(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_mimetypes ? js_from_obj(jd_mimetypes) : js_null();
}

/* sendBeacon: a small POST a page wants made whether or not it stays, made
   on the browser's next pass through the same door a request goes through
   (jd_do_request). True when it was taken, as the standard has it; this
   takes every one it can hold. */
static void jd_beacon_due(jval arg) {
    if (!js_is_obj(arg) || arg.obj->kind != JO_ARRAY || arg.obj->len < 3 || !jd_do_request) return;
    jval u = arg.obj->items[0], b = arg.obj->items[1], ty = arg.obj->items[2];
    if (u.t != JS_STR) return;
    jd_reply rp = { 0, 0, 0, 0, 0 };
    jd_do_request("POST", u.str->s, b.t == JS_STR ? b.str->s : "", ty.t == JS_STR ? ty.str->s : 0, &rp);
}

static jval nat_nav_beacon(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *url = jd_arg_str(J, a, n, 0);
    if (!url->len) return js_throw(J, JS_ERR_TYPE, "sendBeacon needs an address", J->error_line);
    /* Made whole against the page, as fetch makes its address. */
    {
        char out[URL_TEXT + 256];
        if (jd_resolve(url->s, out, (int)sizeof(out))) url = js_str(J, out);
    }
    jval data = js_arg(a, n, 1);
    jval body = jd_str(""), type = js_undef();
    if (data.t != JS_UNDEF && data.t != JS_NULL) {
        /* Whatever fetch would send it as (jsnet.h): a Blob as its bytes
           and its type, which is how Instagram sends every beacon. */
        const char *ty = 0;
        jstr *s = jd_body_of(J, data, &ty);
        if (!s) return js_undef();
        body = js_from_str(s);
        if (ty) type = jd_str(ty);
    }
    jobj *job = js_array(J);
    if (!job) return js_bool(0);
    js_arr_push(J, job, js_from_str(url));
    js_arr_push(J, job, body);
    js_arr_push(J, job, type);
    return js_bool(jd_later_native(jd_beacon_due, js_from_obj(job), 1) != 0);
}

static void jd_setup_navigator(jctx *J) {
    jobj *np = jd_interface(J, "Navigator", 0, 0, 0);
    if (!np) return;
    jd_accessor(J, np, "userAgent", nat_nav_ua, 0);
    jd_accessor(J, np, "appCodeName", nat_nav_appcodename, 0);
    jd_accessor(J, np, "appName", nat_nav_appname, 0);
    jd_accessor(J, np, "appVersion", nat_nav_appversion, 0);
    jd_accessor(J, np, "product", nat_nav_product, 0);
    jd_accessor(J, np, "vendor", nat_empty_str, 0);
    jd_accessor(J, np, "vendorSub", nat_empty_str, 0);
    jd_accessor(J, np, "platform", nat_nav_platform, 0);
    jd_accessor(J, np, "language", nat_nav_language, 0);
    jd_accessor(J, np, "languages", nat_nav_languages, 0);
    jd_accessor(J, np, "cookieEnabled", nat_nav_true, 0);
    jd_accessor(J, np, "onLine", nat_nav_true, 0);
    jd_accessor(J, np, "maxTouchPoints", nat_nav_zero, 0);
    jd_accessor(J, np, "webdriver", nat_nav_false, 0);
    jd_accessor(J, np, "pdfViewerEnabled", nat_nav_false, 0);
    if (jd_cpu_count() > 0) jd_accessor(J, np, "hardwareConcurrency", nat_nav_cpus, 0);
    jd_accessor(J, np, "plugins", nat_nav_plugins, 0);
    jd_accessor(J, np, "mimeTypes", nat_nav_mimetypes, 0);
    jd_method(J, np, "javaEnabled", nat_nav_false, 0);
    jd_method(J, np, "sendBeacon", nat_nav_beacon, 1);
    jd_plugins = jd_empty_list(J, "PluginArray");
    jd_mimetypes = jd_empty_list(J, "MimeTypeArray");
    jd_languages = 0;
    jobj *nav = js_object_with(J, JO_PLAIN, np);
    if (nav) js_declare(J, J->global, js_str(J, "navigator"), js_from_obj(nav));
}

/* --- location and history ----------------------------------------------------------------
 *
 * location is the page's address, read by the URL reader (jsurl.h): its
 * parts, and setting one of them, or href, or calling assign or replace,
 * sends the browser there on its next pass (jd_navigate). An address that
 * differs from the page's only after the # is the same page: the browser's
 * address changes, the page scrolls to what it names, and hashchange is
 * sent, but nothing is fetched.
 *
 * history.pushState and replaceState change the address the browser shows
 * and keeps, without loading anything (jd_address_changed), as the standard
 * has them; an address on another origin is refused. back, forward and go
 * are the browser's own buttons (jd_history_go), which load the page they
 * land on: a page that pushed states is loaded again at that address rather
 * than sent popstate, which is the one way this differs. */
static void (*jd_address_changed)(const char *url, int push);
static void (*jd_history_go)(int delta);
static int  (*jd_history_length)(void);
static jval jd_state;
static jobj *jd_location_obj;

void jsdom_address_with(void (*fn)(const char *, int)) { jd_address_changed = fn; }
void jsdom_history_with(void (*go)(int), int (*length)(void)) {
    jd_history_go = go;
    jd_history_length = length;
}

/* The page's address as a string without its fragment, to tell whether
   another is the same page. */
static int jd_same_but_fragment(const jurl *a, const jurl *b) {
    jtext ta = { 0, 0, 0, 0 }, tb = { 0, 0, 0, 0 };
    ju_text(a, &ta, 0);
    ju_text(b, &tb, 0);
    int same = ta.n == tb.n && (!ta.n || jd_same_n(ta.b, tb.b, (int)ta.n));
    free(ta.b);
    free(tb.b);
    return same;
}

static void jd_hashchange_due(jval arg) {
    if (!js_is_obj(arg) || arg.obj->kind != JO_ARRAY || arg.obj->len < 2 || !jd_J.global_obj) return;
    jobj *ev = jd_new_event(jd_evkind("HashChangeEvent"), "hashchange", 0, 0);
    if (!ev) return;
    js_set(&jd_J, ev, "isTrusted", js_bool(1));
    js_set(&jd_J, ev, "oldURL", arg.obj->items[0]);
    js_set(&jd_J, ev, "newURL", arg.obj->items[1]);
    jd_dispatch_to(ev, js_from_obj(jd_J.global_obj), js_from_obj(jd_J.global_obj));
}

/* The page going somewhere, by a script's hand: to another page, or to
   another place on this one. */
static void jd_location_go(jctx *J, const jurl *to, int replace) {
    jurl *here = (jurl *)malloc(sizeof(jurl));
    if (!here) return;
    int have = ju_page(here);
    jstr *href = ju_href(J, to);
    if (have && to->has_frag && jd_same_but_fragment(here, to)) {
        jval old = jd_str(jd_address);
        w_copy(jd_address, (int)sizeof(jd_address), href->s, (int)sizeof(jd_address));
        if (jd_address_changed) jd_address_changed(jd_address, !replace);
        /* What the fragment names, brought into view. */
        int el = to->frag[0] ? jd_by_id(to->frag) : -1;
        int bx, by, bw, bh;
        if (el >= 0 && jd_scroll_to && jd_box(el, &bx, &by, &bw, &bh)) jd_scroll_to(by + jd_scroll_y);
        if (!here->has_frag || !w_same(here->frag, to->frag)) {
            jobj *arg = js_array(J);
            if (arg) {
                js_arr_push(J, arg, old);
                js_arr_push(J, arg, js_from_str(href));
                jd_later_native(jd_hashchange_due, js_from_obj(arg), 1);
            }
        }
    } else if (jd_navigate) {
        jd_navigate(href->s, replace);
    }
    free(here);
}

static void jd_location_to_text(jctx *J, jstr *s, int replace) {
    jurl *base = (jurl *)malloc(sizeof(jurl));
    jurl *to = (jurl *)malloc(sizeof(jurl));
    if (base && to) {
        int have = ju_page(base);
        if (ju_parse(s->s, have ? base : 0, to)) jd_location_go(J, to, replace);
        else js_throw_dom(J, "SyntaxError", "that is not an address to go to");
    }
    free(base);
    free(to);
}

static jval nat_loc_get(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    int which = J->callee ? J->callee->spare : 0;
    jurl *u = (jurl *)malloc(sizeof(jurl));
    if (!u) return js_undef();
    jval r = ju_page(u) ? jd_url_part(J, u, which) : jd_str(which == JUP_HREF ? jd_address : "");
    free(u);
    return r;
}

static jval nat_loc_set(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int which = J->callee ? J->callee->spare : 0;
    jstr *v = js_to_str(J, js_arg(a, n, 0));
    if (!v) return js_undef();
    if (which == JUP_HREF) { jd_location_to_text(J, v, 0); return js_undef(); }
    jurl *u = (jurl *)malloc(sizeof(jurl));
    if (!u) return js_undef();
    if (ju_page(u) && jd_url_set_part(J, u, which, v)) jd_location_go(J, u, 0);
    free(u);
    return js_undef();
}

static jval nat_loc_assign(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *v = js_to_str(J, js_arg(a, n, 0));
    if (v) jd_location_to_text(J, v, 0);
    return js_undef();
}

static jval nat_loc_replace(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *v = js_to_str(J, js_arg(a, n, 0));
    if (v) jd_location_to_text(J, v, 1);
    return js_undef();
}

static jval nat_loc_reload(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    if (jd_navigate && jd_address[0]) jd_navigate(jd_address, 1);
    return js_undef();
}

static jval nat_loc_tostring(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_str(jd_address);
}

/* window.location and document.location: the one object, and assigning
   either is going somewhere. */
static jval nat_location(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_location_obj ? js_from_obj(jd_location_obj) : js_null();
}

static jval nat_set_location(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *v = js_to_str(J, js_arg(a, n, 0));
    if (v) jd_location_to_text(J, v, 0);
    return js_undef();
}

static jval jd_history_change(jctx *J, jval *a, int n, int push) {
    jval url = js_arg(a, n, 2);
    if (url.t != JS_UNDEF && url.t != JS_NULL) {
        jstr *s = js_to_str(J, url);
        if (!s) return js_undef();
        jurl *base = (jurl *)malloc(sizeof(jurl));
        jurl *to = (jurl *)malloc(sizeof(jurl));
        int ok = 0;
        if (base && to && ju_page(base) && ju_parse(s->s, base, to)) {
            jstr *o1 = ju_origin(J, base), *o2 = ju_origin(J, to);
            if (!js_str_eq(o1, o2)) {
                free(base);
                free(to);
                return js_throw_dom(J, "SecurityError", "a page cannot put another site's address in its history");
            }
            jstr *href = ju_href(J, to);
            w_copy(jd_address, (int)sizeof(jd_address), href->s, (int)sizeof(jd_address));
            ok = 1;
        }
        free(base);
        free(to);
        if (!ok) return js_throw_dom(J, "SyntaxError", "that is not an address");
    }
    jd_state = js_arg(a, n, 0);
    if (jd_address_changed) jd_address_changed(jd_address, push);
    return js_undef();
}

static jval nat_hist_push(jctx *J, jval t, jval *a, int n) { (void)t; return jd_history_change(J, a, n, 1); }
static jval nat_hist_replace(jctx *J, jval t, jval *a, int n) { (void)t; return jd_history_change(J, a, n, 0); }

static jval nat_hist_state(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_state.t == JS_UNDEF ? js_null() : jd_state;
}

static jval nat_hist_length(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_num(jd_history_length ? jd_history_length() : 1);
}

static jval nat_hist_go(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int d = n > 0 ? (int)js_to_num(J, a[0]) : 0;
    if (jd_history_go) jd_history_go(d);
    return js_undef();
}

static jval nat_hist_back(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    if (jd_history_go) jd_history_go(-1);
    return js_undef();
}

static jval nat_hist_forward(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    if (jd_history_go) jd_history_go(1);
    return js_undef();
}

static jval jd_scroll_restoration;

static jval nat_hist_restoration(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_scroll_restoration.t == JS_STR ? jd_scroll_restoration : jd_str("auto");
}

static jval nat_hist_set_restoration(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = jd_arg_str(J, a, n, 0);
    if (js_str_is(s, "auto") || js_str_is(s, "manual")) jd_scroll_restoration = js_from_str(s);
    return js_undef();
}

static void jd_setup_location(jctx *J) {
    jd_state = js_null();
    jd_scroll_restoration = js_undef();
    jobj *lp = jd_interface(J, "Location", 0, 0, 0);
    static const struct { const char *name; int which, set; } PARTS[] = {
        { "href", JUP_HREF, 1 }, { "origin", JUP_ORIGIN, 0 }, { "protocol", JUP_PROTOCOL, 1 },
        { "host", JUP_HOST, 1 }, { "hostname", JUP_HOSTNAME, 1 }, { "port", JUP_PORT, 1 },
        { "pathname", JUP_PATHNAME, 1 }, { "search", JUP_SEARCH, 1 }, { "hash", JUP_HASH, 1 },
        { 0, 0, 0 }
    };
    for (int i = 0; PARTS[i].name; i++)
        jd_url_accessor(J, lp, PARTS[i].name, PARTS[i].which, nat_loc_get, PARTS[i].set ? nat_loc_set : 0);
    jd_method(J, lp, "assign", nat_loc_assign, 1);
    jd_method(J, lp, "replace", nat_loc_replace, 1);
    jd_method(J, lp, "reload", nat_loc_reload, 0);
    jd_method(J, lp, "toString", nat_loc_tostring, 0);
    jd_location_obj = js_object_with(J, JO_PLAIN, lp);

    /* On the window as an accessor, so `location = '/x'` goes there. */
    jobj *lg = js_native(J, "location", nat_location);
    jobj *ls = js_native(J, "location", nat_set_location);
    if (lg && ls && J->global_obj) {
        lg->flags |= JOF_NOCTOR;
        ls->flags |= JOF_NOCTOR;
        js_define_accessor(J, J->global_obj, js_str(J, "location"), js_from_obj(lg), js_from_obj(ls),
                           JP_ENUM | JP_CONF);
    }
    jd_accessor(J, jd_p[JI_DOCUMENT], "location", nat_location, nat_set_location);

    jobj *hp = jd_interface(J, "History", 0, 0, 0);
    jd_method(J, hp, "pushState", nat_hist_push, 2);
    jd_method(J, hp, "replaceState", nat_hist_replace, 2);
    jd_method(J, hp, "go", nat_hist_go, 0);
    jd_method(J, hp, "back", nat_hist_back, 0);
    jd_method(J, hp, "forward", nat_hist_forward, 0);
    jd_accessor(J, hp, "state", nat_hist_state, 0);
    jd_accessor(J, hp, "length", nat_hist_length, 0);
    jd_accessor(J, hp, "scrollRestoration", nat_hist_restoration, nat_hist_set_restoration);
    jobj *hist = js_object_with(J, JO_PLAIN, hp);
    if (hist) js_declare(J, J->global, js_str(J, "history"), js_from_obj(hist));
}

/* --- cookies -----------------------------------------------------------------------------
 *
 * document.cookie is the browser's own jar (fetch.h), not a copy: what a
 * script reads is what the next request to this page's address will send,
 * less the cookies marked HttpOnly, which the standard keeps from every
 * script; and what a script writes is one Set-Cookie line, taken by the same
 * code that takes a server's, which will not let it make or change an
 * HttpOnly one. Reached through the browser (jsdom_cookies_with), which
 * knows the page's address. */
static int  (*jd_cookie_get)(char *out, int cap);
static void (*jd_cookie_set)(const char *line);

void jsdom_cookies_with(int (*get)(char *, int), void (*set)(const char *)) {
    jd_cookie_get = get;
    jd_cookie_set = set;
}

#define JD_COOKIES_MAX (64 * 1024)

static jval nat_doc_cookie(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    /* A document of its own has no address, and so no cookies. */
    if (!jd_cookie_get || jd_inert_of(t) >= 0) return jd_str("");
    char *buf = (char *)malloc(JD_COOKIES_MAX);
    if (!buf) return jd_str("");
    int got = jd_cookie_get(buf, JD_COOKIES_MAX);
    jval v = js_from_str(js_str_n(J, buf, got > 0 ? (u32)got : 0));
    free(buf);
    return v;
}

static jval nat_doc_set_cookie(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (s && jd_cookie_set && jd_inert_of(t) < 0) jd_cookie_set(s->s);
    return js_undef();
}

/* --- storage ----------------------------------------------------------------------------------
 *
 * localStorage and sessionStorage: strings by name, kept for the page's
 * origin in the browser's memory -- not the page's region, which goes when
 * the page does -- for as long as the browser runs, and never written to
 * disk, where they would be something a reader had to be able to find and
 * delete. This browser has one window, so a session lasts as long as the
 * browser and the two differ only in being two stores. Each origin may keep
 * a megabyte in each, and a write past that is refused with the standard's
 * QuotaExceededError; the storage event goes to other windows, of which there
 * are none. MDN stopped on localStorage. */
#define JD_STORE_QUOTA (1024 * 1024)
#define JD_STORE_ALL   (8 * 1024 * 1024)

static jstr *jd_page_origin(jctx *J);

typedef struct {
    char *origin, *key, *value;
    u32   klen, vlen;
} jstore_item;

typedef struct {
    jstore_item *items;
    int n, cap;
    u32 bytes;
} jstore;

static jstore jd_stores[2];              /* local, session */

static int jd_store_find(jstore *s, const char *origin, const jstr *key) {
    for (int i = 0; i < s->n; i++)
        if (w_same(s->items[i].origin, origin) && s->items[i].klen == key->len
            && jd_same_n(s->items[i].key, key->s, (int)key->len)) return i;
    return -1;
}

static u32 jd_store_used(jstore *s, const char *origin) {
    u32 used = 0;
    for (int i = 0; i < s->n; i++)
        if (w_same(s->items[i].origin, origin)) used += s->items[i].klen + s->items[i].vlen;
    return used;
}

static char *jd_dup(const char *p, u32 n) {
    char *d = (char *)malloc((u64)n + 1);
    if (!d) return 0;
    for (u32 i = 0; i < n; i++) d[i] = p[i];
    d[n] = 0;
    return d;
}

static void jd_store_drop(jstore *s, int i) {
    s->bytes -= s->items[i].klen + s->items[i].vlen;
    free(s->items[i].origin);
    free(s->items[i].key);
    free(s->items[i].value);
    s->items[i] = s->items[--s->n];
}

/* Which store an object is, and the page's origin to keep it under; an
   opaque origin has no storage, as the standard has it. */
static jstore *jd_store_of(jctx *J, jval t, jstr **origin) {
    if (!js_is_obj(t) || (t.obj->host != JD_STORAGE && t.obj->host != JD_STORAGE + 1)) {
        jd_illegal(J);
        return 0;
    }
    *origin = jd_page_origin(J);
    if (js_str_is(*origin, "null")) {
        js_throw_dom(J, "SecurityError", "a page with no origin has no storage");
        return 0;
    }
    return &jd_stores[t.obj->host - JD_STORAGE];
}

static jval jd_store_get(jctx *J, jstore *s, const char *origin, const jstr *key) {
    int i = jd_store_find(s, origin, key);
    return i < 0 ? js_null() : js_from_str(js_str_n(J, s->items[i].value, s->items[i].vlen));
}

static void jd_store_set(jctx *J, jstore *s, const char *origin, const jstr *key, const jstr *val) {
    int i = jd_store_find(s, origin, key);
    u32 was = i >= 0 ? s->items[i].klen + s->items[i].vlen : 0;
    u32 want = key->len + val->len;
    if (jd_store_used(s, origin) - was + want > JD_STORE_QUOTA || s->bytes - was + want > JD_STORE_ALL) {
        js_throw_dom(J, "QuotaExceededError", "this site has used all the storage it is allowed");
        return;
    }
    char *v = jd_dup(val->s, val->len);
    if (!v) { js_throw_dom(J, "QuotaExceededError", "the browser has no room to keep that"); return; }
    if (i >= 0) {
        free(s->items[i].value);
        s->items[i].value = v;
        s->items[i].vlen = val->len;
        s->bytes = s->bytes - was + want;
        return;
    }
    if (s->n >= s->cap) {
        int cap = s->cap ? s->cap * 2 : 32;
        jstore_item *more = (jstore_item *)malloc((u64)cap * sizeof(jstore_item));
        if (!more) { free(v); js_throw_dom(J, "QuotaExceededError", "the browser has no room to keep that"); return; }
        volatile u8 *d = (volatile u8 *)more;
        const u8 *src = (const u8 *)s->items;
        for (u64 k = 0; k < (u64)s->n * sizeof(jstore_item); k++) d[k] = src[k];
        if (s->items) free(s->items);
        s->items = more;
        s->cap = cap;
    }
    jstore_item *it = &s->items[s->n];
    it->origin = jd_dup(origin, (u32)w_len(origin));
    it->key = jd_dup(key->s, key->len);
    it->value = v;
    it->klen = key->len;
    it->vlen = val->len;
    if (!it->origin || !it->key) { free(it->origin); free(it->key); free(v); return; }
    s->n++;
    s->bytes += want;
}

static jval nat_store_get(jctx *J, jval t, jval *a, int n) {
    jstr *origin;
    jstore *s = jd_store_of(J, t, &origin);
    if (!s) return js_undef();
    return jd_store_get(J, s, origin->s, jd_arg_str(J, a, n, 0));
}

static jval nat_store_set(jctx *J, jval t, jval *a, int n) {
    jstr *origin;
    jstore *s = jd_store_of(J, t, &origin);
    if (!s) return js_undef();
    jstr *k = jd_arg_str(J, a, n, 0), *v = js_to_str(J, js_arg(a, n, 1));
    if (v) jd_store_set(J, s, origin->s, k, v);
    return js_undef();
}

static jval nat_store_remove(jctx *J, jval t, jval *a, int n) {
    jstr *origin;
    jstore *s = jd_store_of(J, t, &origin);
    if (!s) return js_undef();
    int i = jd_store_find(s, origin->s, jd_arg_str(J, a, n, 0));
    if (i >= 0) jd_store_drop(s, i);
    return js_undef();
}

static jval nat_store_clear(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *origin;
    jstore *s = jd_store_of(J, t, &origin);
    if (!s) return js_undef();
    for (int i = s->n - 1; i >= 0; i--)
        if (w_same(s->items[i].origin, origin->s)) jd_store_drop(s, i);
    return js_undef();
}

/* The nth key of this origin's, in the order they were first kept. */
static int jd_store_nth(jstore *s, const char *origin, int nth) {
    for (int i = 0, k = 0; i < s->n; i++)
        if (w_same(s->items[i].origin, origin) && k++ == nth) return i;
    return -1;
}

static jval nat_store_key(jctx *J, jval t, jval *a, int n) {
    jstr *origin;
    jstore *s = jd_store_of(J, t, &origin);
    if (!s) return js_undef();
    double d = n > 0 ? js_to_num(J, a[0]) : 0;
    int i = d >= 0 && d < 1e9 ? jd_store_nth(s, origin->s, (int)d) : -1;
    return i < 0 ? js_null() : js_from_str(js_str_n(J, s->items[i].key, s->items[i].klen));
}

static jval nat_store_length(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *origin;
    jstore *s = jd_store_of(J, t, &origin);
    if (!s) return js_undef();
    int k = 0;
    for (int i = 0; i < s->n; i++) if (w_same(s->items[i].origin, origin->s)) k++;
    return js_num(k);
}

/* localStorage.name, read and written as though it were getItem and
   setItem, for any name the prototype does not have. */
static int jd_store_named(jctx *J, jobj *o, const char *name) {
    for (jobj *p = o->proto; p; p = p->proto)
        if (js_find(p, js_str(J, name))) return 0;
    return 1;
}

static int jd_storage_get(jctx *J, jobj *o, const char *name, jval *out) {
    if (!jd_store_named(J, o, name)) return 0;
    jstr *origin;
    jstore *s = jd_store_of(J, js_from_obj(o), &origin);
    if (!s) { J->sig = JS_OK; return 0; }
    jval v = jd_store_get(J, s, origin->s, js_str(J, name));
    if (v.t != JS_STR) return 0;
    *out = v;
    return 1;
}

static int jd_storage_put(jctx *J, jobj *o, const char *name, jval v) {
    if (!jd_store_named(J, o, name)) return 0;
    jstr *origin;
    jstore *s = jd_store_of(J, js_from_obj(o), &origin);
    if (!s) return 1;
    jstr *val = js_to_str(J, v);
    if (val) jd_store_set(J, s, origin->s, js_str(J, name), val);
    return 1;
}

static void jd_setup_storage(jctx *J) {
    jobj *sp = jd_interface(J, "Storage", 0, 0, 0);
    jd_method(J, sp, "getItem", nat_store_get, 1);
    jd_method(J, sp, "setItem", nat_store_set, 2);
    jd_method(J, sp, "removeItem", nat_store_remove, 1);
    jd_method(J, sp, "clear", nat_store_clear, 0);
    jd_method(J, sp, "key", nat_store_key, 1);
    jd_accessor(J, sp, "length", nat_store_length, 0);
    const char *names[2] = { "localStorage", "sessionStorage" };
    for (int k = 0; k < 2; k++) {
        jobj *o = js_object_with(J, JO_PLAIN, sp);
        if (!o) continue;
        o->host = JD_STORAGE + k;
        js_declare(J, J->global, js_str(J, names[k]), js_from_obj(o));
    }
}

/* --- time -----------------------------------------------------------------------------------
 *
 * performance.now() is milliseconds since the page began, from the kernel's
 * tick, which is ten milliseconds long: that is the resolution a page gets,
 * and it is the truth of this machine rather than a finer clock pretended.
 * timeOrigin is when that was, by the same wall clock Date reads. Marks and
 * measures are kept as a page makes them; the browser records no timings of
 * its own, so there are no other entries. */
static jobj *jd_perf_entries;
static double jd_time_origin;

static void jd_perf_observed(jobj *entry);        /* jsobs.h */

static jval nat_perf_now(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_num(jd_now_ms());
}

static jval nat_perf_origin(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_num(jd_time_origin);
}

static jobj *jd_perf_entry(jctx *J, jstr *name, const char *type, double start, double dur) {
    jobj *e = js_object(J, JO_PLAIN);
    if (!e) return 0;
    js_set(J, e, "name", js_from_str(name));
    js_set(J, e, "entryType", jd_str(type));
    js_set(J, e, "startTime", js_num(start));
    js_set(J, e, "duration", js_num(dur));
    js_set(J, e, "detail", js_null());
    if (jd_perf_entries) js_arr_push(J, jd_perf_entries, js_from_obj(e));
    jd_perf_observed(e);
    return e;
}

static double jd_perf_find(const jstr *name) {
    for (u32 i = jd_perf_entries ? jd_perf_entries->len : 0; i > 0; i--) {
        jval e = jd_perf_entries->items[i - 1];
        jval nm = jd_ev_get(e.obj, "name");
        if (nm.t == JS_STR && js_str_eq(nm.str, name)) return js_to_num(&jd_J, jd_ev_get(e.obj, "startTime"));
    }
    return -1;
}

static jval nat_perf_mark(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *e = jd_perf_entry(J, jd_arg_str(J, a, n, 0), "mark", jd_now_ms(), 0);
    return e ? js_from_obj(e) : js_undef();
}

static jval nat_perf_measure(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double start = 0, end = jd_now_ms();
    jval s = js_arg(a, n, 1), f = js_arg(a, n, 2);
    if (js_is_obj(s)) {
        jval st = js_get(J, s, js_str(J, "start")), en = js_get(J, s, js_str(J, "end"));
        if (st.t == JS_STR) start = jd_perf_find(st.str); else if (st.t == JS_NUM) start = st.num;
        if (en.t == JS_STR) end = jd_perf_find(en.str); else if (en.t == JS_NUM) end = en.num;
    } else {
        if (s.t == JS_STR) start = jd_perf_find(s.str);
        if (f.t == JS_STR) end = jd_perf_find(f.str);
    }
    if (start < 0 || end < 0) return js_throw_dom(J, "SyntaxError", "there is no mark of that name");
    jobj *e = jd_perf_entry(J, jd_arg_str(J, a, n, 0), "measure", start, end - start);
    return e ? js_from_obj(e) : js_undef();
}

static jval jd_perf_list(jctx *J, const jstr *name, const jstr *type) {
    jobj *out = js_array(J);
    for (u32 i = 0; out && jd_perf_entries && i < jd_perf_entries->len; i++) {
        jobj *e = jd_perf_entries->items[i].obj;
        jval nm = jd_ev_get(e, "name"), ty = jd_ev_get(e, "entryType");
        if (name && !(nm.t == JS_STR && js_str_eq(nm.str, name))) continue;
        if (type && !(ty.t == JS_STR && js_str_eq(ty.str, type))) continue;
        js_arr_push(J, out, js_from_obj(e));
    }
    return js_from_obj(out);
}

static jval nat_perf_entries(jctx *J, jval t, jval *a, int n) { (void)t; (void)a; (void)n; return jd_perf_list(J, 0, 0); }

static jval nat_perf_by_name(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return jd_perf_list(J, jd_arg_str(J, a, n, 0), n > 1 && a[1].t != JS_UNDEF ? jd_arg_str(J, a, n, 1) : 0);
}

static jval nat_perf_by_type(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return jd_perf_list(J, 0, jd_arg_str(J, a, n, 0));
}

static jval jd_perf_clear(jctx *J, jval *a, int n, const char *type) {
    if (!jd_perf_entries) return js_undef();
    jstr *name = n > 0 && a[0].t != JS_UNDEF ? js_to_str(J, a[0]) : 0;
    u32 w = 0;
    for (u32 i = 0; i < jd_perf_entries->len; i++) {
        jobj *e = jd_perf_entries->items[i].obj;
        jval nm = jd_ev_get(e, "name"), ty = jd_ev_get(e, "entryType");
        int gone = ty.t == JS_STR && js_str_is(ty.str, type) && (!name || (nm.t == JS_STR && js_str_eq(nm.str, name)));
        if (!gone) jd_perf_entries->items[w++] = jd_perf_entries->items[i];
    }
    jd_perf_entries->len = w;
    return js_undef();
}

static jval nat_perf_clear_marks(jctx *J, jval t, jval *a, int n) { (void)t; return jd_perf_clear(J, a, n, "mark"); }
static jval nat_perf_clear_measures(jctx *J, jval t, jval *a, int n) { (void)t; return jd_perf_clear(J, a, n, "measure"); }

static jval nat_perf_json(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jobj *o = js_object(J, JO_PLAIN);
    if (o) js_set(J, o, "timeOrigin", js_num(jd_time_origin));
    return js_from_obj(o);
}

static jval nat_win_event(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_window_event;
}

/* requestAnimationFrame: called back before the next frame, about sixty a
   second, with the time; frames are the same timers setTimeout makes, and
   cancelAnimationFrame is clearTimeout by another name. */
static jval nat_raf(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (!js_callable(js_arg(a, n, 0))) return js_throw(J, JS_ERR_TYPE, "requestAnimationFrame needs a function", J->error_line);
    return jd_add_timer(J, a, n, 0, 1);
}

/* --- the window's size, scroll and screen -------------------------------------------------- */

static int jd_win_w, jd_win_h;          /* the browser's whole window */

/* The browser's window, toolbar and all: what outerWidth is. */
__attribute__((unused)) static void jsdom_window(int w, int h) { jd_win_w = w; jd_win_h = h; }

static jval nat_win_inner_w(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_num(jd_view_w); }
static jval nat_win_inner_h(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_num(jd_view_h); }
static jval nat_win_outer_w(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_num(jd_win_w ? jd_win_w : jd_view_w); }
static jval nat_win_outer_h(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_num(jd_win_h ? jd_win_h : jd_view_h); }
static jval nat_win_scroll_y(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_num(jd_scroll_y); }
static jval nat_win_one(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_num(1); }

/* scrollTo(x, y), scrollTo({ top }), scrollBy: the page scrolls as a whole,
   downwards; across, nothing here scrolls. */
static jval jd_win_scroll(jctx *J, jval *a, int n, int by) {
    double y = 0;
    int have = 0;
    if (n > 0 && js_is_obj(a[0])) {
        jval top = js_get(J, a[0], js_str(J, "top"));
        if (top.t != JS_UNDEF) { y = js_to_num(J, top); have = 1; }
    } else if (n > 1) {
        y = js_to_num(J, a[1]);
        have = 1;
    }
    if (!have || !(y == y) || !jd_scroll_to) return js_undef();
    int to = (int)(by ? jd_scroll_y + y : y);
    if (to < 0) to = 0;
    jd_scroll_to(to);
    jd_scroll_y = to;
    return js_undef();
}

static jval nat_win_scroll_to(jctx *J, jval t, jval *a, int n) { (void)t; return jd_win_scroll(J, a, n, 0); }
static jval nat_win_scroll_by(jctx *J, jval t, jval *a, int n) { (void)t; return jd_win_scroll(J, a, n, 1); }

/* The screen, as the kernel reports its size (sysinfo); the window can take
   all of it, and it shows 24 bits of colour. Where the kernel does not say,
   the screen is the window, which the standard allows a browser to answer. */
static jval nat_screen_w(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    const zelr_sysinfo *m = jd_machine();
    return js_num(m && m->screen_w ? (double)m->screen_w : jd_view_w);
}

static jval nat_screen_h(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    const zelr_sysinfo *m = jd_machine();
    return js_num(m && m->screen_h ? (double)m->screen_h : jd_view_h);
}

static jval nat_screen_depth(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_num(24); }

/* --- media queries ----------------------------------------------------------------------------
 *
 * matchMedia answers with the style sheets' own reading of a query (css.h,
 * css_mq), against the window's width now, so a page's script and its
 * sheet cannot disagree about which layout is in force. A list a page is
 * listening to is asked again when the window's width changes, and hears
 * "change" when its answer did. */
static jstr *jd_k_media, *jd_k_was;
static jobj *jd_mqls;                   /* the lists with listeners, to ask again */

static int jd_mq_matches(const jstr *q) {
    int lo, hi;
    if (!css_mq(q->s, (int)q->len, &lo, &hi)) return 0;
    if (lo >= 0 && jd_view_w < lo) return 0;
    if (hi >= 0 && jd_view_w > hi) return 0;
    return 1;
}

static jval nat_mql_matches(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    jval q = js_is_obj(t) ? jd_kept(t.obj, jd_k_media) : js_undef();
    return q.t == JS_STR ? js_bool(jd_mq_matches(q.str)) : js_undef();
}

static jval nat_mql_media(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    jval q = js_is_obj(t) ? jd_kept(t.obj, jd_k_media) : js_undef();
    return q.t == JS_STR ? q : js_undef();
}

static void jd_mql_watch(jctx *J, jval t) {
    if (!js_is_obj(t)) return;
    if (!jd_mqls) jd_mqls = js_array(J);
    for (u32 i = 0; jd_mqls && i < jd_mqls->len; i++) if (jd_mqls->items[i].obj == t.obj) return;
    if (jd_mqls) {
        js_arr_push(J, jd_mqls, t);
        jd_keep(t.obj, jd_k_was, js_bool(nat_mql_matches(J, t, 0, 0).b));
    }
}

/* addListener and removeListener, the old names, are addEventListener and
   removeEventListener for "change". */
static jval nat_mql_add(jctx *J, jval t, jval *a, int n) {
    jd_mql_watch(J, t);
    jval args[2] = { jd_str("change"), js_arg(a, n, 0) };
    return nat_add_listener(J, t, args, 2);
}

static jval nat_mql_remove(jctx *J, jval t, jval *a, int n) {
    jval args[2] = { jd_str("change"), js_arg(a, n, 0) };
    return nat_remove_listener(J, t, args, 2);
}

static jval nat_mql_add_event(jctx *J, jval t, jval *a, int n) {
    jd_mql_watch(J, t);
    return nat_add_listener(J, t, a, n);
}

static jobj *jd_p_mql;

static jval nat_match_media(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *q = jd_arg_str(J, a, n, 0);
    jobj *o = js_object_with(J, JO_PLAIN, jd_p_mql);
    if (!o) return js_undef();
    jd_keep(o, jd_k_media, js_from_str(q));
    return js_from_obj(o);
}

/* After the window's width changed: every watched list asked again. */
static void jd_mql_recheck(void) {
    if (!jd_open || !jd_mqls || jd_spent()) return;
    for (u32 i = 0; i < jd_mqls->len; i++) {
        jval m = jd_mqls->items[i];
        jval q = jd_kept(m.obj, jd_k_media);
        if (q.t != JS_STR) continue;
        int now = jd_mq_matches(q.str);
        jval was = jd_kept(m.obj, jd_k_was);
        if (was.t == JS_BOOL && was.b == now) continue;
        jd_keep(m.obj, jd_k_was, js_bool(now));
        jobj *ev = jd_new_event(0, "change", 0, 0);
        if (!ev) continue;
        js_set(&jd_J, ev, "isTrusted", js_bool(1));
        js_set(&jd_J, ev, "matches", js_bool(now));
        js_set(&jd_J, ev, "media", q);
        jd_dispatch_to(ev, m, m);
    }
}

/* --- CSS ------------------------------------------------------------------------------------ */

/* CSS.supports: what an @supports rule would be told (css.h,
   css_supports), for one property and value or a whole condition. */
static jval nat_css_supports(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jtext tx = { 0, 0, 0, 0 };
    if (n >= 2) {
        jd_putc(&tx, '(');
        jstr *p = jd_arg_str(J, a, n, 0), *v = jd_arg_str(J, a, n, 1);
        jt_put(J, &tx, p->s, p->len);
        jd_put(&tx, ": ");
        jt_put(J, &tx, v->s, v->len);
        jd_putc(&tx, ')');
    } else {
        jstr *c = jd_arg_str(J, a, n, 0);
        jt_put(J, &tx, c->s, c->len);
    }
    int yes = css_supports(tx.b ? tx.b : "", (int)tx.n);
    free(tx.b);
    return js_bool(yes);
}

/* CSS.escape: an identifier written so a selector reads it as one. */
static jval nat_css_escape(jctx *J, jval t, jval *a, int n) {
    (void)t;
    static const char HEX[] = "0123456789abcdef";
    jstr *s = jd_arg_str(J, a, n, 0);
    jtext tx = { 0, 0, 0, 0 };
    for (u32 i = 0; i < s->len; i++) {
        u8 c = (u8)s->s[i];
        int first = i == 0, dash_first = s->s[0] == '-';
        if (c == 0) { jd_put(&tx, "\xef\xbf\xbd"); continue; }
        if ((c >= 1 && c <= 0x1F) || c == 0x7F || (c >= '0' && c <= '9' && (first || (i == 1 && dash_first)))) {
            jd_putc(&tx, '\\');
            if (c >= 16) jd_putc(&tx, HEX[c >> 4]);
            jd_putc(&tx, HEX[c & 15]);
            jd_putc(&tx, ' ');
            continue;
        }
        if (first && c == '-' && s->len == 1) { jd_put(&tx, "\\-"); continue; }
        if (c >= 0x80 || c == '-' || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            jd_putc(&tx, (char)c);
        else { jd_putc(&tx, '\\'); jd_putc(&tx, (char)c); }
    }
    return js_from_str(jt_done(J, &tx));
}

/* --- what the window does for a page ---------------------------------------------------------
 *
 * There are no dialogs: alert returns at once, confirm answers no and prompt
 * nothing, which is what the standard has a browser do when it cannot show
 * them. There is one window, so open() gives null, as it does when a
 * browser blocks one. */
static jval nat_alert(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_undef();
}

static jval nat_confirm(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_bool(0);
}

/* postMessage: a message to this window, delivered afterwards as a message
   event from this window's own origin. */
static void jd_message_due(jval arg) {
    if (!js_is_obj(arg) || arg.obj->kind != JO_ARRAY || arg.obj->len < 3) return;
    jval target = arg.obj->items[0], data = arg.obj->items[1], origin = arg.obj->items[2];
    if (!js_is_obj(target)) return;
    jobj *ev = jd_new_event(jd_evkind("MessageEvent"), "message", 0, 0);
    if (!ev) return;
    js_set(&jd_J, ev, "isTrusted", js_bool(1));
    js_set(&jd_J, ev, "data", data);
    js_set(&jd_J, ev, "origin", origin);
    js_set(&jd_J, ev, "source", arg.obj->len > 3 ? arg.obj->items[3] : js_null());
    js_set(&jd_J, ev, "ports", js_from_obj(js_array(&jd_J)));
    jd_dispatch_to(ev, target, target);
}

static jstr *jd_page_origin(jctx *J) {
    jurl *u = (jurl *)malloc(sizeof(jurl));
    jstr *o = u && ju_page(u) ? ju_origin(J, u) : js_str(J, "null");
    free(u);
    return o;
}

static jval nat_post_message(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *job = js_array(J);
    if (!job) return js_undef();
    js_arr_push(J, job, js_from_obj(J->global_obj));
    js_arr_push(J, job, js_arg(a, n, 0));
    js_arr_push(J, job, js_from_str(jd_page_origin(J)));
    js_arr_push(J, job, js_from_obj(J->global_obj));
    jd_later_native(jd_message_due, js_from_obj(job), 1);
    return js_undef();
}

/* MessageChannel: two ports, each posting to the other, the message
   arriving afterwards. React's scheduler takes its turns this way when it
   can, which is why it is here. */
static jstr *jd_k_other;
static jobj *jd_p_port;

static jval nat_port_post(jctx *J, jval t, jval *a, int n) {
    jval other = js_is_obj(t) ? jd_kept(t.obj, jd_k_other) : js_undef();
    if (!js_is_obj(other)) return js_undef();
    jobj *job = js_array(J);
    if (!job) return js_undef();
    js_arr_push(J, job, other);
    js_arr_push(J, job, js_arg(a, n, 0));
    js_arr_push(J, job, jd_str(""));
    jd_later_native(jd_message_due, js_from_obj(job), 1);
    return js_undef();
}

static jval nat_port_close(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (js_is_obj(t)) jd_keep(t.obj, jd_k_other, js_undef());
    return js_undef();
}

static jval nat_channel_ctor(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "MessageChannel is made with new", J->error_line);
    jobj *p1 = js_object_with(J, JO_PLAIN, jd_p_port), *p2 = js_object_with(J, JO_PLAIN, jd_p_port);
    if (!p1 || !p2) return js_undef();
    jd_keep(p1, jd_k_other, js_from_obj(p2));
    jd_keep(p2, jd_k_other, js_from_obj(p1));
    js_const_prop(J, t.obj, "port1", js_from_obj(p1));
    js_const_prop(J, t.obj, "port2", js_from_obj(p2));
    return js_undef();
}

/* --- crypto ------------------------------------------------------------------------------
 *
 * crypto.getRandomValues and crypto.randomUUID, from the kernel's random
 * number generator (SYS_RANDOM), which is the one the TLS keys come from.
 * Nothing here makes up for it: when the kernel has found no source of
 * randomness yet it says so, and so does this, with an OperationError,
 * rather than hand a page numbers somebody could guess. crypto.subtle is
 * not here: this browser has no SubtleCrypto, and a page that looks for it
 * finds nothing rather than something that pretends. */

static int (*jd_random)(void *buf, int len);

/* Where the bytes come from, for a check that has to see what happens
   without them; left alone, the kernel. */
void jsdom_random_with(int (*fn)(void *buf, int len)) { jd_random = fn; }

static int jd_kernel_random(void *buf, int len) {
    u8 *p = (u8 *)buf;
    int done = 0;
    while (done < len) {
        long got = random_bytes(p + done, len - done);
        if (got <= 0) return -1;
        done += (int)got;
    }
    return done;
}

static int jd_random_fill(jctx *J, u8 *p, u32 n) {
    int got = jd_random ? jd_random(p, (int)n) : jd_kernel_random(p, (int)n);
    if (got != (int)n) {
        js_throw_dom(J, "OperationError", "this machine has no source of randomness yet");
        return 0;
    }
    return 1;
}

static jval nat_crypto_values(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval v = js_arg(a, n, 0);
    if (!js_is_obj(v) || v.obj->kind != JO_TYPED || !v.obj->internal)
        return js_throw(J, JS_ERR_TYPE, "getRandomValues needs an integer typed array", J->error_line);
    jtyped *x = (jtyped *)v.obj->internal;
    if (x->type == TA_F32 || x->type == TA_F64)
        return js_throw_dom(J, "TypeMismatchError", "getRandomValues fills integers, not floating point");
    u32 bytes = x->len * TA_SIZE[x->type];
    if (bytes > 65536)
        return js_throw_dom(J, "QuotaExceededError", "getRandomValues gives at most 65536 bytes at a time");
    if (bytes && !jd_random_fill(J, ta_bytes(x->buf) + x->off, bytes)) return js_undef();
    return v;
}

static jval nat_crypto_uuid(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    static const char hex[] = "0123456789abcdef";
    u8 b[16];
    if (!jd_random_fill(J, b, sizeof(b))) return js_undef();
    b[6] = (u8)((b[6] & 0x0F) | 0x40);            /* version 4: random */
    b[8] = (u8)((b[8] & 0x3F) | 0x80);            /* the standard's variant */
    char s[37];
    int w = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s[w++] = '-';
        s[w++] = hex[b[i] >> 4];
        s[w++] = hex[b[i] & 15];
    }
    s[w] = 0;
    return jd_str(s);
}

static void jd_setup_crypto(jctx *J, jscope *g, int secure) {
    jobj *cp = jd_interface(J, "Crypto", 0, 0, 0);
    jd_method(J, cp, "getRandomValues", nat_crypto_values, 1);
    /* Only on a page that came over an encrypted connection, as the
       standard has it; getRandomValues is for every page. */
    if (secure) jd_method(J, cp, "randomUUID", nat_crypto_uuid, 0);
    jobj *c = js_object_with(J, JO_PLAIN, cp);
    if (c) js_declare(J, g, js_str(J, "crypto"), js_from_obj(c));
}

/* --- Blob, File and blob: addresses ------------------------------------------------------------
 *
 * Bytes a page puts together and hands to something that takes a body or an
 * address: Instagram makes a Blob for every beacon it sends. A Blob keeps
 * its bytes as a string of them (strings here are bytes) and its type;
 * a File is a Blob with a name and a time. URL.createObjectURL gives one an
 * address, blob:<origin>/<uuid>, that a fetch, an XMLHttpRequest or a
 * script's src is answered from until the address is revoked or the page
 * is left. */

static jstr *jd_k_bbytes, *jd_k_btype;
static jobj *jd_p_blob, *jd_p_file;
static jobj *jd_blob_urls;                 /* [address, blob, address, blob, ...] */
static int jd_blob_count;

/* A Blob's bytes and type, when v is one. */
static int jd_blob_parts(jval v, jstr **bytes, jstr **type) {
    if (!js_is_obj(v)) return 0;
    jval b = jd_kept(v.obj, jd_k_bbytes);
    if (b.t != JS_STR) return 0;
    *bytes = b.str;
    jval t = jd_kept(v.obj, jd_k_btype);
    *type = t.t == JS_STR ? t.str : 0;
    return 1;
}

/* A type as the standard keeps it: lower case, and nothing at all if it
   has a character outside printable ASCII. */
static jstr *jd_blob_type(jctx *J, jval v) {
    if (v.t == JS_UNDEF) return js_str(J, "");
    jstr *s = js_to_str(J, v);
    if (!s) return js_str(J, "");
    for (u32 i = 0; i < s->len; i++) if ((u8)s->s[i] < 0x20 || (u8)s->s[i] > 0x7E) return js_str(J, "");
    return jd_lower_str(J, s);
}

static jobj *jd_blob_make(jctx *J, jobj *proto, jstr *bytes, jstr *type) {
    jobj *b = js_object_with(J, JO_PLAIN, proto ? proto : jd_p_blob);
    if (!b) return 0;
    jd_keep(b, jd_k_bbytes, js_from_str(bytes ? bytes : js_str(J, "")));
    jd_keep(b, jd_k_btype, js_from_str(type ? type : js_str(J, "")));
    return b;
}

/* The parts a Blob is made of, one after another: strings as their UTF-8,
   buffers and views as their bytes, Blobs as theirs. */
static jstr *jd_blob_join(jctx *J, jval parts) {
    jtext t = { 0, 0, 0, 0 };
    if (parts.t != JS_UNDEF && parts.t != JS_NULL) {
        if (!js_is_obj(parts)) {
            js_throw(J, JS_ERR_TYPE, "a Blob is made from a list of parts", J->error_line);
            return 0;
        }
        jargs A;
        js_args_init(&A);
        if (js_iter_collect(J, parts, &A)) {
            for (int i = 0; i < A.n && J->sig == JS_OK; i++) {
                const u8 *p;
                u32 n;
                jstr *bb, *bt;
                if (jd_blob_parts(A.v[i], &bb, &bt)) jt_put(J, &t, bb->s, bb->len);
                else if (js_is_obj(A.v[i]) && tx_bytes_of(A.v[i], &p, &n)) jt_put(J, &t, (const char *)p, n);
                else {
                    jstr *s = js_to_str(J, A.v[i]);
                    if (s) jt_put(J, &t, s->s, s->len);
                }
            }
        }
        js_args_free(&A);
    }
    if (J->sig != JS_OK) { free(t.b); return 0; }
    return jt_done(J, &t);
}

static jval nat_blob_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "Blob is made with new", J->error_line);
    jstr *bytes = jd_blob_join(J, js_arg(a, n, 0));
    if (!bytes) return js_undef();
    jval opt = js_arg(a, n, 1);
    jstr *type = jd_blob_type(J, js_is_obj(opt) ? js_get(J, opt, js_str(J, "type")) : js_undef());
    jd_keep(t.obj, jd_k_bbytes, js_from_str(bytes));
    jd_keep(t.obj, jd_k_btype, js_from_str(type));
    return js_undef();
}

static jval nat_file_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "File is made with new", J->error_line);
    if (n < 2) return js_throw(J, JS_ERR_TYPE, "a File needs its parts and a name", J->error_line);
    jstr *bytes = jd_blob_join(J, a[0]);
    if (!bytes) return js_undef();
    jstr *name = js_to_str(J, a[1]);
    jval opt = js_arg(a, n, 2);
    jstr *type = jd_blob_type(J, js_is_obj(opt) ? js_get(J, opt, js_str(J, "type")) : js_undef());
    jval when = js_is_obj(opt) ? js_get(J, opt, js_str(J, "lastModified")) : js_undef();
    jd_keep(t.obj, jd_k_bbytes, js_from_str(bytes));
    jd_keep(t.obj, jd_k_btype, js_from_str(type));
    js_define(J, t.obj, js_str(J, "name"), js_from_str(name ? name : js_str(J, "")), JP_ENUM);
    js_define(J, t.obj, js_str(J, "lastModified"),
              js_num(when.t == JS_UNDEF ? js_trunc(js_now(J)) : js_to_num(J, when)), JP_ENUM);
    return js_undef();
}

static jval nat_blob_size(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *b, *ty;
    if (!jd_blob_parts(t, &b, &ty)) return jd_illegal(J);
    return js_num(b->len);
}

static jval nat_blob_type_get(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *b, *ty;
    if (!jd_blob_parts(t, &b, &ty)) return jd_illegal(J);
    return js_from_str(ty ? ty : js_str(J, ""));
}

/* A position as slice takes one: from the end when negative, and inside. */
static u32 jd_blob_at(jctx *J, jval v, u32 len, u32 dflt) {
    if (v.t == JS_UNDEF) return dflt;
    double d = js_trunc(js_to_num(J, v));
    if (d != d) d = 0;
    if (d < 0) d = d + len < 0 ? 0 : d + len;
    return d > len ? len : (u32)d;
}

static jval nat_blob_slice(jctx *J, jval t, jval *a, int n) {
    jstr *b, *ty;
    if (!jd_blob_parts(t, &b, &ty)) return jd_illegal(J);
    u32 from = jd_blob_at(J, js_arg(a, n, 0), b->len, 0);
    u32 to = jd_blob_at(J, js_arg(a, n, 1), b->len, b->len);
    if (to < from) to = from;
    jstr *type = jd_blob_type(J, js_arg(a, n, 2));
    jobj *o = jd_blob_make(J, jd_p_blob, js_str_n(J, b->s + from, to - from), type);
    return o ? js_from_obj(o) : js_null();
}

static jval nat_blob_text(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *b, *ty;
    if (!jd_blob_parts(t, &b, &ty)) return jd_illegal(J);
    return jd_promise_from(J, js_from_str(b));
}

static jval nat_blob_buffer(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *b, *ty;
    if (!jd_blob_parts(t, &b, &ty)) return jd_illegal(J);
    jobj *buf = ta_new_buffer(J, J->p_buffer, b->len);
    if (buf) {
        u8 *d = ta_bytes(buf);
        for (u32 i = 0; i < b->len; i++) d[i] = (u8)b->s[i];
    }
    return jd_promise_from(J, js_from_obj(buf));
}

/* --- blob: addresses --- */

static jval nat_url_create_object(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval v = js_arg(a, n, 0);
    jstr *b, *ty;
    if (!jd_blob_parts(v, &b, &ty)) return js_throw(J, JS_ERR_TYPE, "createObjectURL needs a Blob", J->error_line);
    /* A UUID from the kernel's generator when it has one; the address is a
       name, not a secret, so a counter in the same form does when not. */
    static const char hex[] = "0123456789abcdef";
    u8 r[16];
    int got = jd_random ? jd_random(r, 16) : jd_kernel_random(r, 16);
    if (got != 16) {
        for (int i = 0; i < 16; i++) r[i] = 0;
        int c = ++jd_blob_count;
        for (int i = 15; i >= 12 && c; i--, c >>= 8) r[i] = (u8)c;
    }
    r[6] = (u8)((r[6] & 0x0F) | 0x40);
    r[8] = (u8)((r[8] & 0x3F) | 0x80);
    char id[37];
    int w = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) id[w++] = '-';
        id[w++] = hex[r[i] >> 4];
        id[w++] = hex[r[i] & 15];
    }
    id[w] = 0;
    jtext tx = { 0, 0, 0, 0 };
    jd_put(&tx, "blob:");
    jstr *origin = jd_page_origin(J);
    if (origin && !js_str_is(origin, "null")) jt_put(J, &tx, origin->s, origin->len);
    else jd_put(&tx, "null");
    jd_putc(&tx, '/');
    jd_put(&tx, id);
    jstr *url = jt_done(J, &tx);
    if (!jd_blob_urls) jd_blob_urls = js_array(J);
    if (jd_blob_urls && url) {
        js_arr_push(J, jd_blob_urls, js_from_str(url));
        js_arr_push(J, jd_blob_urls, v);
    }
    return url ? js_from_str(url) : js_undef();
}

static jval nat_url_revoke_object(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *url = jd_arg_str(J, a, n, 0);
    for (u32 i = 0; jd_blob_urls && i + 1 < jd_blob_urls->len; i += 2)
        if (jd_blob_urls->items[i].t == JS_STR && js_str_eq(jd_blob_urls->items[i].str, url)) {
            jd_blob_urls->items[i] = js_undef();
            jd_blob_urls->items[i + 1] = js_undef();
        }
    return js_undef();
}

static int jd_is_blob_url(const char *s) {
    return s && w_lower(s[0]) == 'b' && w_lower(s[1]) == 'l' && w_lower(s[2]) == 'o'
           && w_lower(s[3]) == 'b' && s[4] == ':';
}

/* What a blob: address was made for, while it has not been revoked; the
   fragment is no part of it. */
static int jd_blob_lookup(const char *url, jstr **bytes, jstr **type) {
    if (!jd_blob_urls || !jd_is_blob_url(url)) return 0;
    int len = 0;
    while (url[len] && url[len] != '#') len++;
    for (u32 i = 0; i + 1 < jd_blob_urls->len; i += 2) {
        jval u = jd_blob_urls->items[i];
        if (u.t != JS_STR || (int)u.str->len != len) continue;
        int same = 1;
        for (int k = 0; k < len && same; k++) if (u.str->s[k] != url[k]) same = 0;
        if (same) return jd_blob_parts(jd_blob_urls->items[i + 1], bytes, type);
    }
    return 0;
}

static void jd_setup_blobs(jctx *J, jscope *g) {
    (void)g;
    jd_k_bbytes = js_sym_new(J, "bytes", 5);
    jd_k_btype = js_sym_new(J, "type", 4);
    jd_blob_urls = 0;
    jd_blob_count = 0;
    jd_p_blob = jd_interface(J, "Blob", 0, nat_blob_ctor, 0);
    jd_accessor(J, jd_p_blob, "size", nat_blob_size, 0);
    jd_accessor(J, jd_p_blob, "type", nat_blob_type_get, 0);
    jd_method(J, jd_p_blob, "slice", nat_blob_slice, 0);
    jd_method(J, jd_p_blob, "text", nat_blob_text, 0);
    jd_method(J, jd_p_blob, "arrayBuffer", nat_blob_buffer, 0);
    jd_p_file = jd_interface(J, "File", jd_p_blob, nat_file_ctor, 2);
    jobj *url = jd_ctor_of(jd_p_url);
    if (url) {
        js_method(J, url, "createObjectURL", nat_url_create_object, 1);
        js_method(J, url, "revokeObjectURL", nat_url_revoke_object, 1);
    }
}

/* --- the computed style ------------------------------------------------------------------------
 *
 * getComputedStyle, from the style the layout works out for the element --
 * the browser's rules, the page's, the style attribute, and what the element
 * inherits -- reached through the browser, which holds the sheet
 * (jsdom_styles_with; browser.c, computed_style). Answered for the
 * properties the layout knows; one it does not is "", which is what a
 * browser answers for a property it does not have. Width and height are
 * where the layout drew the element when it was drawn. */
static int (*jd_style_of)(int node, cstyle *out);

void jsdom_styles_with(int (*fn)(int, cstyle *)) { jd_style_of = fn; }

static void jd_css_colour(jtext *t, u32 c) {
    jd_put(t, "rgb(");
    jd_put_num(t, (int)((c >> 16) & 255));
    jd_put(t, ", ");
    jd_put_num(t, (int)((c >> 8) & 255));
    jd_put(t, ", ");
    jd_put_num(t, (int)(c & 255));
    jd_putc(t, ')');
}

static void jd_css_px(jtext *t, int v) { jd_put_num(t, v); jd_put(t, "px"); }

static int jd_computed_value(int node, const cstyle *s, const char *prop, jtext *t) {
    static const char *const DISPLAY[] = { "inline", "block", "inline-block", "list-item", "none",
        "table-cell", "flex", "table", "table-row", "table-row-group", "inline-flex", "grid", "contents" };
    static const char *const POSITION[] = { "static", "relative", "absolute", "fixed" };
    static const char *const ALIGN[] = { "left", "center", "right", "justify" };
    static const char *const WHITE[] = { "normal", "pre", "nowrap" };
    static const char *const FLEXDIR[] = { "row", "row-reverse", "column", "column-reverse" };
    static const char *const JUSTIFY[] = { "flex-start", "center", "flex-end", "space-between", "space-around", "space-evenly" };
    static const char *const ALIGNITEMS[] = { "stretch", "flex-start", "center", "flex-end", "baseline" };
    int bx, by, bw, bh;
    if (w_same(prop, "display")) {
        jd_put(t, s->display < sizeof(DISPLAY) / sizeof(DISPLAY[0]) ? DISPLAY[s->display] : "block");
    } else if (w_same(prop, "visibility")) jd_put(t, s->visible ? "visible" : "hidden");
    else if (w_same(prop, "color")) jd_css_colour(t, s->color);
    else if (w_same(prop, "background-color")) {
        if (s->has_bg) jd_css_colour(t, s->background);
        else jd_put(t, "rgba(0, 0, 0, 0)");
    } else if (w_same(prop, "border-color") || w_same(prop, "border-top-color")) jd_css_colour(t, s->border_color);
    else if (w_same(prop, "font-size")) jd_css_px(t, s->font_px);
    else if (w_same(prop, "font-weight")) jd_put(t, s->bold ? "700" : "400");
    else if (w_same(prop, "font-style")) jd_put(t, s->italic ? "italic" : "normal");
    else if (w_same(prop, "text-decoration") || w_same(prop, "text-decoration-line"))
        jd_put(t, s->underline ? "underline" : s->strike ? "line-through" : "none");
    else if (w_same(prop, "text-align")) jd_put(t, s->align < 4 ? ALIGN[s->align] : "left");
    else if (w_same(prop, "white-space")) jd_put(t, s->white < 3 ? WHITE[s->white] : "normal");
    else if (w_same(prop, "position")) jd_put(t, s->position < 4 ? POSITION[s->position] : "static");
    else if (w_same(prop, "float")) jd_put(t, s->floated == 1 ? "left" : s->floated == 2 ? "right" : "none");
    else if (w_same(prop, "box-sizing")) jd_put(t, s->border_box ? "border-box" : "content-box");
    else if (w_same(prop, "flex-direction")) jd_put(t, s->flex_dir < 4 ? FLEXDIR[s->flex_dir] : "row");
    else if (w_same(prop, "flex-wrap")) jd_put(t, s->flex_wrap ? "wrap" : "nowrap");
    else if (w_same(prop, "justify-content")) jd_put(t, s->justify < 6 ? JUSTIFY[s->justify] : "normal");
    else if (w_same(prop, "align-items")) jd_put(t, s->align_items < 5 ? ALIGNITEMS[s->align_items] : "normal");
    else if (w_same(prop, "margin-top")) jd_css_px(t, s->mt);
    else if (w_same(prop, "margin-right")) jd_css_px(t, s->mr);
    else if (w_same(prop, "margin-bottom")) jd_css_px(t, s->mb);
    else if (w_same(prop, "margin-left")) jd_css_px(t, s->ml);
    else if (w_same(prop, "padding-top")) jd_css_px(t, s->pt);
    else if (w_same(prop, "padding-right")) jd_css_px(t, s->pr);
    else if (w_same(prop, "padding-bottom")) jd_css_px(t, s->pb);
    else if (w_same(prop, "padding-left")) jd_css_px(t, s->pl);
    else if (w_same(prop, "border-top-width")) jd_css_px(t, s->bt);
    else if (w_same(prop, "border-right-width")) jd_css_px(t, s->br);
    else if (w_same(prop, "border-bottom-width")) jd_css_px(t, s->bb);
    else if (w_same(prop, "border-left-width")) jd_css_px(t, s->bl);
    else if (w_same(prop, "width") || w_same(prop, "height")) {
        int wide = prop[0] == 'w';
        if (jd_box(node, &bx, &by, &bw, &bh)) jd_css_px(t, wide ? bw : bh);
        else if ((wide ? s->width : s->height) >= 0) jd_css_px(t, wide ? s->width : s->height);
        else jd_put(t, "auto");
    } else if (w_same(prop, "line-height")) {
        if (s->line_h > 0) jd_css_px(t, s->font_px * s->line_h / 100);
        else jd_put(t, "normal");
    } else return 0;
    return 1;
}

static jstr *jd_k_computed;
static jobj *jd_p_computed;

static int jd_computed_node(jobj *o) {
    jval v = jd_kept(o, jd_k_computed);
    return v.t == JS_NUM ? (int)v.num : -1;
}

static jval jd_computed_get_value(jctx *J, int node, const char *prop) {
    if (node < 0 || !jd_style_of || !jd_is_element(node)) return jd_str("");
    cstyle *s = (cstyle *)malloc(sizeof(cstyle));
    if (!s) return jd_str("");
    jval r = jd_str("");
    if (jd_style_of(node, s)) {
        jtext t = { 0, 0, 0, 0 };
        if (jd_computed_value(node, s, prop, &t)) r = js_from_str(jt_done(J, &t));
        else free(t.b);
    }
    free(s);
    return r;
}

static jval nat_computed_get(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jstr *p = jd_arg_str(J, a, n, 0);
    char low[64];
    int k = 0;
    for (; k < (int)p->len && k < 63; k++) low[k] = w_lower(p->s[k]);
    low[k] = 0;
    return jd_computed_get_value(J, jd_computed_node(t.obj), low);
}

/* The properties by their camelCase names, as a page reads them. */
static int jd_computed_host(jctx *J, jobj *o, const char *name, jval *out) {
    int node = jd_computed_node(o);
    if (node < 0 || name[0] < 'a' || name[0] > 'z') return 0;
    char prop[64];
    jd_css_name(name, prop, (int)sizeof(prop));
    if (!jd_css_known(prop) && !w_same(prop, "visibility") && !w_same(prop, "border-top-color")) return 0;
    *out = jd_computed_get_value(J, node, prop);
    return 1;
}

static jval nat_get_computed_style(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int x = jd_el_of(js_arg(a, n, 0));
    if (x < 0) return js_throw(J, JS_ERR_TYPE, "getComputedStyle needs an element", J->error_line);
    jobj *o = js_object_with(J, JO_PLAIN, jd_p_computed);
    if (!o) return js_undef();
    o->host = JD_COMPUTED;
    jd_keep(o, jd_k_computed, js_num(x));
    return js_from_obj(o);
}

static void jd_setup_window_more(jctx *J) {
    jscope *g = J->global;
    jobj *w = J->global_obj;

    /* performance */
    jd_perf_entries = js_array(J);
    jd_time_origin = js_now(J) - jd_now_ms();
    jobj *pp = jd_interface(J, "Performance", jd_p[JI_EVENTTARGET], 0, 0);
    jd_method(J, pp, "now", nat_perf_now, 0);
    jd_accessor(J, pp, "timeOrigin", nat_perf_origin, 0);
    jd_method(J, pp, "mark", nat_perf_mark, 1);
    jd_method(J, pp, "measure", nat_perf_measure, 1);
    jd_method(J, pp, "getEntries", nat_perf_entries, 0);
    jd_method(J, pp, "getEntriesByName", nat_perf_by_name, 1);
    jd_method(J, pp, "getEntriesByType", nat_perf_by_type, 1);
    jd_method(J, pp, "clearMarks", nat_perf_clear_marks, 0);
    jd_method(J, pp, "clearMeasures", nat_perf_clear_measures, 0);
    jd_method(J, pp, "toJSON", nat_perf_json, 0);
    jobj *perf = js_object_with(J, JO_PLAIN, pp);
    if (perf) js_declare(J, g, js_str(J, "performance"), js_from_obj(perf));

    js_declare(J, g, js_str(J, "requestAnimationFrame"), js_from_obj(js_native_n(J, "requestAnimationFrame", nat_raf, 1)));
    js_declare(J, g, js_str(J, "cancelAnimationFrame"), js_from_obj(js_native_n(J, "cancelAnimationFrame", nat_clear_timer, 1)));

    /* sizes and scrolling, as the window's own accessors */
    static const struct { const char *name; jnative get; } SIZES[] = {
        { "innerWidth", nat_win_inner_w }, { "innerHeight", nat_win_inner_h },
        { "outerWidth", nat_win_outer_w }, { "outerHeight", nat_win_outer_h },
        { "scrollX", nat_zero }, { "scrollY", nat_win_scroll_y },
        { "pageXOffset", nat_zero }, { "pageYOffset", nat_win_scroll_y },
        { "screenX", nat_zero }, { "screenY", nat_zero }, { "screenLeft", nat_zero }, { "screenTop", nat_zero },
        { "devicePixelRatio", nat_win_one }, { 0, 0 }
    };
    for (int i = 0; SIZES[i].name && w; i++) jd_accessor(J, w, SIZES[i].name, SIZES[i].get, 0);
    /* window.event: the event being handled while a listener runs, and
       nothing otherwise (jd_invoke). Old pages read it bare, `event`. */
    jd_window_event = js_undef();
    if (w) jd_accessor(J, w, "event", nat_win_event, 0);
    js_declare(J, g, js_str(J, "scrollTo"), js_from_obj(js_native_n(J, "scrollTo", nat_win_scroll_to, 2)));
    js_declare(J, g, js_str(J, "scroll"), js_from_obj(js_native_n(J, "scroll", nat_win_scroll_to, 2)));
    js_declare(J, g, js_str(J, "scrollBy"), js_from_obj(js_native_n(J, "scrollBy", nat_win_scroll_by, 2)));

    jobj *sp = jd_interface(J, "Screen", jd_p[JI_EVENTTARGET], 0, 0);
    jd_accessor(J, sp, "width", nat_screen_w, 0);
    jd_accessor(J, sp, "height", nat_screen_h, 0);
    jd_accessor(J, sp, "availWidth", nat_screen_w, 0);
    jd_accessor(J, sp, "availHeight", nat_screen_h, 0);
    jd_accessor(J, sp, "availLeft", nat_zero, 0);
    jd_accessor(J, sp, "availTop", nat_zero, 0);
    jd_accessor(J, sp, "colorDepth", nat_screen_depth, 0);
    jd_accessor(J, sp, "pixelDepth", nat_screen_depth, 0);
    jobj *scr = js_object_with(J, JO_PLAIN, sp);
    if (scr) js_declare(J, g, js_str(J, "screen"), js_from_obj(scr));

    /* matchMedia */
    jd_k_media = js_sym_new(J, "media", 5);
    jd_k_was = js_sym_new(J, "matched", 7);
    jd_mqls = 0;
    jd_p_mql = jd_interface(J, "MediaQueryList", jd_p[JI_EVENTTARGET], 0, 0);
    jd_accessor(J, jd_p_mql, "matches", nat_mql_matches, 0);
    jd_accessor(J, jd_p_mql, "media", nat_mql_media, 0);
    jd_method(J, jd_p_mql, "addListener", nat_mql_add, 1);
    jd_method(J, jd_p_mql, "removeListener", nat_mql_remove, 1);
    jd_method(J, jd_p_mql, "addEventListener", nat_mql_add_event, 2);
    js_declare(J, g, js_str(J, "matchMedia"), js_from_obj(js_native_n(J, "matchMedia", nat_match_media, 1)));

    /* CSS */
    jobj *css = js_object(J, JO_PLAIN);
    if (css) {
        js_method(J, css, "supports", nat_css_supports, 1);
        js_method(J, css, "escape", nat_css_escape, 1);
        js_declare(J, g, js_str(J, "CSS"), js_from_obj(css));
    }

    /* dialogs, windows, messages */
    js_declare(J, g, js_str(J, "alert"), js_from_obj(js_native_n(J, "alert", nat_alert, 0)));
    js_declare(J, g, js_str(J, "confirm"), js_from_obj(js_native_n(J, "confirm", nat_confirm, 0)));
    js_declare(J, g, js_str(J, "prompt"), js_from_obj(js_native_n(J, "prompt", nat_null_getter, 0)));
    js_declare(J, g, js_str(J, "open"), js_from_obj(js_native_n(J, "open", nat_null_getter, 0)));
    static const char *const QUIET[] = { "close", "focus", "blur", "print", "stop", 0 };
    for (int i = 0; QUIET[i]; i++) js_declare(J, g, js_str(J, QUIET[i]), js_from_obj(js_native_n(J, QUIET[i], nat_nothing_js, 0)));
    js_declare(J, g, js_str(J, "postMessage"), js_from_obj(js_native_n(J, "postMessage", nat_post_message, 1)));
    jd_k_other = js_sym_new(J, "other", 5);
    jd_p_port = jd_interface(J, "MessagePort", jd_p[JI_EVENTTARGET], 0, 0);
    jd_method(J, jd_p_port, "postMessage", nat_port_post, 1);
    jd_method(J, jd_p_port, "start", nat_nothing_js, 0);
    jd_method(J, jd_p_port, "close", nat_port_close, 0);
    jd_interface(J, "MessageChannel", 0, nat_channel_ctor, 0);

    /* The window's own family: it is its own frames, parent and top, and
       nothing opened it. */
    if (w) {
        js_declare(J, g, js_str(J, "frames"), js_from_obj(w));
        js_declare(J, g, js_str(J, "parent"), js_from_obj(w));
        js_declare(J, g, js_str(J, "top"), js_from_obj(w));
    }
    js_declare(J, g, js_str(J, "opener"), js_null());
    js_declare(J, g, js_str(J, "frameElement"), js_null());
    js_declare(J, g, js_str(J, "closed"), js_bool(0));
    js_declare(J, g, js_str(J, "length"), js_num(0));
    js_declare(J, g, js_str(J, "name"), jd_str(""));
    js_declare(J, g, js_str(J, "status"), jd_str(""));
    {
        jurl *u = (jurl *)malloc(sizeof(jurl));
        int secure = u && ju_page(u) && w_same(u->scheme, "https");
        free(u);
        js_declare(J, g, js_str(J, "isSecureContext"), js_bool(secure));
        js_declare(J, g, js_str(J, "origin"), js_from_str(jd_page_origin(J)));
        jd_setup_crypto(J, g, secure);
        jd_setup_blobs(J, g);
    }

    /* getComputedStyle */
    jd_k_computed = js_sym_new(J, "computed", 8);
    jd_p_computed = js_object_with(J, JO_PLAIN, jd_p[JI_STYLEDECL]);
    if (jd_p_computed) {
        jd_method(J, jd_p_computed, "getPropertyValue", nat_computed_get, 1);
        js_tag(J, jd_p_computed, "CSSStyleDeclaration");
    }
    js_declare(J, g, js_str(J, "getComputedStyle"), js_from_obj(js_native_n(J, "getComputedStyle", nat_get_computed_style, 1)));
}
