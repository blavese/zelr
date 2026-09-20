#pragma once
#include "dom.h"
#include "js.h"
#include "jsparse.h"
#include "jsrun.h"
#include "web.h"

/* The document, as a script sees it.
 *
 * The engine in js.h knows nothing about pages: it runs a language, and it
 * was written that way on purpose so it could be tested without one. What it
 * has instead is two hooks — how a property on a host object is read and
 * written — and this file is the browser filling them in.
 *
 * A host object is a plain JavaScript object with a number on it saying what
 * it stands for. For an element that number is its index in the document, so
 * reading `el.textContent` walks the document as it is now rather than a
 * copy taken when the object was made, and writing it changes the document
 * the layout is about to read.
 *
 * What is here is what a page can actually do, and nothing else. There is no
 * addEventListener, because nothing delivers events yet; no createElement,
 * because nothing puts one anywhere; no innerHTML, because that means running
 * the parser again over a fragment and the parser builds whole documents. A
 * missing property is better than one that returns undefined forever and
 * lets a page believe it worked.
 *
 * Scripts run once, after the document is parsed and before it is laid out.
 * There is no event loop, no timer and no order beyond the order they appear
 * in, which is also why `defer` and `async` are ignored rather than honoured
 * incorrectly.
 */

/* The host number that means the document itself rather than an element in
   it. Above any node index there could ever be: DOM_NODES is twelve
   thousand, and the hooks only fire for a host of zero or more. */
#define JD_DOCUMENT 0x1000000

static ddoc  *jd_doc;            /* what these bindings are bound to */
static jobj **jd_wrap;           /* one object per node, so identity holds */
static int    jd_dirty;          /* a script changed what layout must see */

/* --- elements -------------------------------------------------------------
 *
 * One object per node, kept for as long as the page is. Two calls to
 * getElementById for the same id have to come back as the same object or
 * `a === b` is false for two names for one element, which is a thing pages
 * check.
 */
static jval nat_el_getattr(jctx *J, jval t, jval *a, int n);
static jval nat_el_setattr(jctx *J, jval t, jval *a, int n);

static jobj *jd_element(jctx *J, int node) {
    if (!jd_doc || node < 0 || node >= jd_doc->count) return 0;
    if (jd_wrap && jd_wrap[node]) return jd_wrap[node];

    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return 0;
    o->host = node;

    /* The two that have to be methods rather than properties, because they
       take a name. Everything else an element answers to is a property and
       goes through the hook. */
    js_set(J, o, "getAttribute",
           js_from_obj(js_native(J, "getAttribute", nat_el_getattr)));
    js_set(J, o, "setAttribute",
           js_from_obj(js_native(J, "setAttribute", nat_el_setattr)));

    if (jd_wrap) jd_wrap[node] = o;
    return o;
}

static jval jd_el_value(jctx *J, int node) {
    jobj *o = jd_element(J, node);
    return o ? js_from_obj(o) : js_null();
}

/* --- what a script may ask for ------------------------------------------- */

static jval nat_doc_by_id(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (!jd_doc || n < 1) return js_null();
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s) return js_null();
    int el = dom_by_id(jd_doc, s->s);
    return el < 0 ? js_null() : jd_el_value(J, el);
}

static jval nat_doc_by_tag(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *out = js_array(J);
    if (!out || !jd_doc || n < 1) return out ? js_from_obj(out) : js_null();

    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s) return js_from_obj(out);
    int tag = html_tag_of(s->s, (int)s->len);

    u32 at = 0;
    for (int i = 0; i < jd_doc->count; i++) {
        if (jd_doc->nodes[i].kind != DN_ELEMENT) continue;
        if (jd_doc->nodes[i].tag != tag) continue;
        js_arr_set(J, out, at++, jd_el_value(J, i));
    }
    return js_from_obj(out);
}

static jval nat_el_getattr(jctx *J, jval t, jval *a, int n) {
    if (!jd_doc || t.t != JS_OBJ || !t.obj || t.obj->host < 0) return js_null();
    if (t.obj->host >= JD_DOCUMENT || n < 1) return js_null();

    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s) return js_null();
    const char *v = dom_attr(jd_doc, t.obj->host, s->s);
    return v ? js_from_str(js_str(J, v)) : js_null();
}

static jval nat_el_setattr(jctx *J, jval t, jval *a, int n) {
    if (!jd_doc || t.t != JS_OBJ || !t.obj || t.obj->host < 0) return js_undef();
    if (t.obj->host >= JD_DOCUMENT || n < 2) return js_undef();

    jstr *name = js_to_str(J, js_arg(a, n, 0));
    jstr *val = js_to_str(J, js_arg(a, n, 1));
    if (!name || !val) return js_undef();

    if (dom_attr_set(jd_doc, t.obj->host, name->s, val->s))
        jd_dirty = 1;
    return js_undef();
}

/* --- the hooks ------------------------------------------------------------
 *
 * Asked before the object's own property table, so what comes back is the
 * document as it stands rather than whatever was written there last.
 */
static int jd_host_get(jctx *J, jobj *o, const char *name, jval *out) {
    if (!jd_doc) return 0;

    if (o->host == JD_DOCUMENT) {
        if (w_same(name, "title")) {
            *out = js_from_str(js_str(J, jd_doc->title >= 0
                                      ? jd_doc->arena + jd_doc->title : ""));
            return 1;
        }
        if (w_same(name, "body")) {
            *out = jd_doc->body >= 0 ? jd_el_value(J, jd_doc->body) : js_null();
            return 1;
        }
        return 0;                       /* its methods are ordinary properties */
    }

    int el = o->host;
    if (el < 0 || el >= jd_doc->count) return 0;

    if (w_same(name, "textContent")) {
        /* Out of the document every time it is asked for. A copy kept on the
           object would go stale the moment anything else wrote to it. */
        static char buf[4096];
        dom_text_content(jd_doc, el, buf, (int)sizeof(buf));
        *out = js_from_str(js_str(J, buf));
        return 1;
    }
    if (w_same(name, "tagName")) {
        *out = js_from_str(js_str(J, dom_tag_name(jd_doc, el)));
        return 1;
    }
    if (w_same(name, "id")) {
        const char *v = dom_attr(jd_doc, el, "id");
        *out = js_from_str(js_str(J, v ? v : ""));
        return 1;
    }
    if (w_same(name, "className")) {
        const char *v = dom_attr(jd_doc, el, "class");
        *out = js_from_str(js_str(J, v ? v : ""));
        return 1;
    }
    return 0;
}

static int jd_host_set(jctx *J, jobj *o, const char *name, jval v) {
    if (!jd_doc) return 0;

    if (o->host == JD_DOCUMENT) {
        if (w_same(name, "title")) {
            jstr *s = js_to_str(J, v);
            if (s) {
                jd_doc->title = dom_str(jd_doc, s->s, (int)s->len);
                jd_dirty = 1;
            }
            return 1;
        }
        return 0;
    }

    int el = o->host;
    if (el < 0 || el >= jd_doc->count) return 0;

    if (w_same(name, "textContent")) {
        jstr *s = js_to_str(J, v);
        if (s) {
            dom_set_text(jd_doc, el, s->s, (int)s->len);
            jd_dirty = 1;
        }
        return 1;
    }
    if (w_same(name, "className")) {
        jstr *s = js_to_str(J, v);
        if (s && dom_attr_set(jd_doc, el, "class", s->s))
            jd_dirty = 1;
        return 1;
    }
    return 0;
}

/* --- running what is on the page ------------------------------------------
 *
 * One context for the whole document rather than one per script, because a
 * function declared in the first script and called from the second is
 * ordinary and would otherwise not be found.
 *
 * Returns how many scripts ran. `err` gets the first failure, because a page
 * with a broken script should say so somewhere rather than silently doing
 * nothing — and the second failure is usually the first one's fault.
 */
static int jsdom_run(ddoc *d, char *err, int errcap, int *changed) {
    if (err && errcap) err[0] = 0;
    if (changed) *changed = 0;
    if (!d) return 0;

    /* Nothing to run is the common case by a wide margin, and it should not
       cost a context, a global object or a quarter megabyte. */
    int any = 0;
    for (int i = 0; i < d->count; i++)
        if (d->nodes[i].kind == DN_ELEMENT && d->nodes[i].tag == T_SCRIPT) {
            any = 1;
            break;
        }
    if (!any) return 0;

    static jctx J;
    js_init(&J);
    J.host_get = jd_host_get;
    J.host_set = jd_host_set;

    jd_doc = d;
    jd_dirty = 0;
    jd_wrap = (jobj **)js_alloc(&J, (u32)sizeof(jobj *) * (u32)d->count);
    if (jd_wrap)
        for (int i = 0; i < d->count; i++) jd_wrap[i] = 0;

    jobj *document = js_object(&J, JO_PLAIN);
    if (document) {
        document->host = JD_DOCUMENT;
        js_set(&J, document, "getElementById",
               js_from_obj(js_native(&J, "getElementById", nat_doc_by_id)));
        js_set(&J, document, "getElementsByTagName",
               js_from_obj(js_native(&J, "getElementsByTagName", nat_doc_by_tag)));
        js_declare(&J, J.global, js_str(&J, "document"), js_from_obj(document));
    }

    int ran = 0;
    for (int i = 0; i < d->count; i++) {
        if (d->nodes[i].kind != DN_ELEMENT || d->nodes[i].tag != T_SCRIPT)
            continue;

        /* A script with a src is somebody else's file, and nothing here
           fetches one yet. Skipped rather than half run. */
        if (dom_attr(d, i, "src")) continue;

        int child = d->nodes[i].first;
        if (child < 0 || d->nodes[child].kind != DN_TEXT) continue;
        const char *text = d->arena + d->nodes[child].text;

        u32 len = 0;
        while (text[len]) len++;
        if (!len) continue;

        ran++;
        if (!js_run(&J, text, len) && err && errcap && !err[0]) {
            int w = 0;
            for (const char *p = J.error; *p && w < errcap - 1; p++)
                err[w++] = *p;
            err[w] = 0;
        }
    }

    /* Every element object, every string and every scope goes here. The
       document keeps whatever the scripts wrote into it, because that lives
       in the document's own arena rather than this one. */
    js_done(&J);
    jd_doc = 0;
    jd_wrap = 0;
    if (changed) *changed = jd_dirty;
    return ran;
}
