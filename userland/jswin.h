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
