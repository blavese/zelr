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

/* The processors running, as the kernel reports them (/sys/cpu, "started").
   Read once, the first time a page asks; where the file cannot be read the
   property is not there at all rather than a number made up. */
static int jd_cpus = -2;

static int jd_cpu_count(void) {
    if (jd_cpus != -2) return jd_cpus;
    jd_cpus = -1;
    int fd = open("/sys/cpu", O_READ);
    if (fd < 0) return -1;
    char buf[256];
    int got = zelr_fread(fd, buf, (int)sizeof(buf) - 1);
    close(fd);
    if (got <= 0) return -1;
    buf[got] = 0;
    for (int i = 0; buf[i]; i++) {
        if ((i && buf[i - 1] != '\n') || !w_starts_fold(buf + i, "started")) continue;
        int k = i + 7, v = 0, any = 0;
        while (buf[k] == ' ') k++;
        while (buf[k] >= '0' && buf[k] <= '9') { v = v * 10 + (buf[k++] - '0'); any = 1; }
        if (any && v > 0) jd_cpus = v;
        break;
    }
    return jd_cpus;
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
    const char *out = 0;
    int status = 0;
    jd_do_request("POST", u.str->s, b.t == JS_STR ? b.str->s : "", ty.t == JS_STR ? ty.str->s : 0,
                  &out, &status);
}

static jval nat_nav_beacon(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *url = jd_arg_str(J, a, n, 0);
    if (!url->len) return js_throw(J, JS_ERR_TYPE, "sendBeacon needs an address", J->error_line);
    jval data = js_arg(a, n, 1);
    jval body = jd_str(""), type = js_undef();
    if (data.t != JS_UNDEF && data.t != JS_NULL) {
        const u8 *bytes;
        u32 blen;
        if (js_is_obj(data) && tx_bytes_of(data, &bytes, &blen)) {
            body = js_from_str(js_str_n(J, (const char *)bytes, blen));
            type = jd_str("application/octet-stream");
        } else {
            jstr *s = js_to_str(J, data);
            if (!s) return js_undef();
            body = js_from_str(s);
            type = jd_str(jd_is_search_params_obj(data) ?"application/x-www-form-urlencoded;charset=UTF-8"
                                                    : "text/plain;charset=UTF-8");
        }
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
    (void)t; (void)a; (void)n;
    if (!jd_cookie_get) return jd_str("");
    char *buf = (char *)malloc(JD_COOKIES_MAX);
    if (!buf) return jd_str("");
    int got = jd_cookie_get(buf, JD_COOKIES_MAX);
    jval v = js_from_str(js_str_n(J, buf, got > 0 ? (u32)got : 0));
    free(buf);
    return v;
}

static jval nat_doc_set_cookie(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (s && jd_cookie_set) jd_cookie_set(s->s);
    return js_undef();
}
