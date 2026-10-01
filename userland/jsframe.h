#pragma once
/* Frames: an <iframe>'s window and document, as a page's scripts reach them.
 *
 * Included from jsdom.h. No frame is loaded or drawn here, but scripts reach
 * into frames as a matter of course: a performance beacon or an ad loader
 * makes a blank iframe and writes into its document so that its own globals
 * stay out of the page's way, a page posts messages to an embedded player's
 * window, and code waits for an iframe's load. With contentWindow null all of
 * that stopped; NPR's mPulse snippet did, on `O.document.open()`.
 *
 * So an iframe in the page has a window: itself as window, self and frames,
 * the page's window as parent and top, its element as frameElement,
 * postMessage that goes nowhere (there is nobody in the frame to hear it),
 * listeners, and the page's timers, constructors and other globals by the
 * same names -- one realm, not two, so a library that takes a pristine Array
 * from a frame is given the page's. A frame whose src is empty, about:blank,
 * about:srcdoc or javascript:, or that has a srcdoc, is the page's own origin
 * and has a document: one of its own as createHTMLDocument makes it, written
 * to and read and never run or drawn, with the srcdoc's markup in it. A frame
 * from anywhere else has no document, as across origins. The frame and its
 * window are told load on the pass after it enters the page, or with the
 * page's own load for one the page was written with.
 *
 * Not done: a frame's own address, its scripts, its history, and a second
 * set of globals. */

static jstr *jd_k_fwin, *jd_k_fdoc, *jd_k_floaded;

/* The globals a frame's window answers with the page's own. */
static const char *const JD_FRAME_GLOBALS[] = {
    "Object", "Array", "Function", "String", "Number", "Boolean", "Symbol", "BigInt", "JSON", "Math",
    "Date", "RegExp", "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "Promise",
    "Map", "Set", "WeakMap", "WeakSet", "ArrayBuffer", "DataView", "Uint8Array", "Int32Array",
    "Float64Array", "Reflect", "Proxy", "Intl", "console", "navigator", "performance", "screen",
    "setTimeout", "clearTimeout", "setInterval", "clearInterval", "requestAnimationFrame",
    "cancelAnimationFrame", "queueMicrotask", "encodeURIComponent", "decodeURIComponent",
    "encodeURI", "decodeURI", "escape", "unescape", "parseInt", "parseFloat", "isNaN", "isFinite",
    "Node", "Element", "HTMLElement", "Document", "Event", "CustomEvent", "MutationObserver",
    "XMLHttpRequest", "fetch", "atob", "btoa", "getComputedStyle", "Image", "URL",
    "URLSearchParams", "TextEncoder", "TextDecoder", "Blob", "FileReader", "crypto", 0
};

/* Whether a frame is the page's own origin, by where it says it comes from. */
static int jd_frame_own(int x) {
    if (jd_attr(x, "srcdoc")) return 1;
    const char *src = jd_attr(x, "src");
    while (src && *src == ' ') src++;
    return !src || !*src || w_same_fold(src, "about:blank") || w_same_fold(src, "about:srcdoc")
           || w_starts_fold(src, "javascript:");
}

static jval nat_frame_post(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_undef();
}

/* The frame's document: made once, with the srcdoc's markup when it has one. */
static jval jd_frame_document(jctx *J, jobj *el, int x) {
    jval kept = jd_kept(el, jd_k_fdoc);
    if (kept.t == JS_OBJ) return kept;
    jval doc = js_null();
    const char *srcdoc = jd_attr(x, "srcdoc");
    if (srcdoc) {
        jval args[2] = { js_from_str(js_str(J, srcdoc)), js_from_str(js_str(J, "text/html")) };
        doc = nat_parser_parse(J, js_undef(), args, 2);
        if (J->sig != JS_OK) { J->sig = JS_OK; doc = js_null(); }
    } else {
        int d = jd_new_document("about:blank", 1);
        if (d >= 0) doc = jd_el_value(J, d);
    }
    if (doc.t == JS_OBJ) jd_keep(el, jd_k_fdoc, doc);
    return doc;
}

static jval jd_frame_window(jctx *J, jobj *el, int x) {
    jval kept = jd_kept(el, jd_k_fwin);
    if (kept.t == JS_OBJ) return kept;
    jobj *w = js_object_with(J, JO_PLAIN, jd_p[JI_EVENTTARGET]);
    if (!w) return js_null();
    jval me = js_from_obj(w), page = jd_J.global_obj ? js_from_obj(jd_J.global_obj) : js_null();
    int own = jd_frame_own(x);
    js_set(J, w, "window", me);
    js_set(J, w, "self", me);
    js_set(J, w, "frames", me);
    js_set(J, w, "parent", page);
    js_set(J, w, "top", page);
    js_set(J, w, "opener", js_null());
    js_set(J, w, "closed", js_bool(0));
    js_set(J, w, "length", js_num(0));
    js_set(J, w, "frameElement", own ? js_from_obj(el) : js_null());
    const char *name = jd_attr(x, "name");
    js_set(J, w, "name", js_from_str(js_str(J, name ? name : "")));
    jobj *postm = js_native(J, "postMessage", nat_frame_post);
    if (postm) js_set(J, w, "postMessage", js_from_obj(postm));
    if (own) {
        jobj *loc = js_object_with(J, JO_PLAIN, 0);
        if (loc) {
            js_set(J, loc, "href", js_from_str(js_str(J, jd_attr(x, "srcdoc") ? "about:srcdoc" : "about:blank")));
            js_set(J, w, "location", js_from_obj(loc));
        }
        for (int i = 0; JD_FRAME_GLOBALS[i]; i++) {
            jval v = page.t == JS_OBJ ? js_get(J, page, js_str(J, JD_FRAME_GLOBALS[i])) : js_undef();
            if (J->sig != JS_OK) { J->sig = JS_OK; continue; }
            if (v.t != JS_UNDEF) js_set(J, w, JD_FRAME_GLOBALS[i], v);
        }
        js_set(J, w, "document", jd_frame_document(J, el, x));
    }
    jd_keep(el, jd_k_fwin, me);
    return me;
}

/* Only an iframe in the page has a window or a document, as in a browser. */
static jval nat_frame_window_get(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0 || !js_is_obj(t) || !jd_connected(x)) return js_null();
    return jd_frame_window(J, t.obj, x);
}

static jval nat_frame_document_get(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0 || !js_is_obj(t) || !jd_connected(x) || !jd_frame_own(x)) return js_null();
    jd_frame_window(J, t.obj, x);
    return jd_frame_document(J, t.obj, x);
}

/* load, to the frame's window and then to the element, once. */
static void jd_frame_load(int x) {
    jval el = jd_el_value(&jd_J, x);
    if (!js_is_obj(el) || jd_kept(el.obj, jd_k_floaded).t != JS_UNDEF) return;
    jd_keep(el.obj, jd_k_floaded, js_bool(1));
    jval w = jd_frame_window(&jd_J, el.obj, x);
    jobj *ev = jd_new_event(0, "load", 0, 0);
    if (ev && w.t == JS_OBJ) {
        js_set(&jd_J, ev, "isTrusted", js_bool(1));
        jd_dispatch_to(ev, w, w);
    }
    jd_fire_simple(x, "load", 0, 0);
}

static void jd_frame_load_due(jval arg) {
    int x = jd_node_of(arg);
    if (x >= 0 && jd_connected(x)) jd_frame_load(x);
}

/* Each iframe in what was just put in the page, told load on the next pass. */
static void jd_frames_inserted(int top) {
    for (int i = top; i >= 0; i = jd_walk_next(i, top))
        if (jd_doc->nodes[i].kind == DN_ELEMENT && jd_doc->nodes[i].tag == T_IFRAME)
            jd_later_native(jd_frame_load_due, jd_el_value(&jd_J, i), 1);
}

/* And the ones the page was written with, before the page's own load. */
static void jd_frames_loaded(void) {
    int root = jd_doc->root;
    for (int i = root; i >= 0; i = jd_walk_next(i, root))
        if (jd_doc->nodes[i].kind == DN_ELEMENT && jd_doc->nodes[i].tag == T_IFRAME && jd_connected(i))
            jd_frame_load(i);
}

static void jd_setup_frames(jctx *J) {
    jd_k_fwin = js_sym_new(J, "frame window", 12);
    jd_k_fdoc = js_sym_new(J, "frame document", 14);
    jd_k_floaded = js_sym_new(J, "frame loaded", 12);
}
