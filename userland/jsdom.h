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
 * What is here is what a page can actually do, and nothing else. A missing
 * property is better than one that returns undefined forever and lets a page
 * believe it worked.
 *
 * --- the script world outlives the load ------------------------------------
 *
 * It used to not. Scripts ran once, between the parse and the layout, and
 * then the context, the global object and every wrapper were torn down: a
 * page was a thing that happened to a document rather than something running
 * on top of it. That is enough for a script that rewrites a heading on the
 * way in and it is the wrong shape for everything else, because a handler is
 * by definition a piece of a program that runs after the program finished.
 *
 * So the world is opened when the page is built and closed when the page is
 * left, and what is in between is a browser that can be clicked on. The cost
 * is a quarter megabyte held for the life of a page that has any script in
 * it at all, which is the same quarter megabyte that used to be held for the
 * length of the load.
 *
 * A page with no script and no handler attribute opens no world and costs
 * nothing, which is still the common case by a wide margin.
 */

/* The host number that means the document itself rather than an element in
   it. Above any node index there could ever be: DOM_NODES is twelve
   thousand, and the hooks only fire for a host of zero or more. */
#define JD_DOCUMENT 0x1000000

static ddoc  *jd_doc;            /* what these bindings are bound to */
static jobj **jd_wrap;           /* one object per node, so identity holds */
static int    jd_dirty;          /* a script changed what layout must see */

/* The world itself, open for as long as the page is. */
static jctx   jd_J;
static int    jd_open;
static char   jd_err[128];       /* the first thing that went wrong in it */

/* Every element object is made lazily and kept, and the array is sized for
   the whole document rather than for the part of it that existed when the
   page was opened, because a script may add nodes past the end of it. */
#define JD_WRAPS DOM_NODES

static void jd_note_error(void) {
    if (jd_err[0] || !jd_J.error[0]) return;
    int w = 0;
    for (const char *p = jd_J.error; *p && w < (int)sizeof(jd_err) - 1; p++)
        jd_err[w++] = *p;
    jd_err[w] = 0;
}

/* --- elements -------------------------------------------------------------
 *
 * One object per node, kept for as long as the page is. Two calls to
 * getElementById for the same id have to come back as the same object or
 * `a === b` is false for two names for one element, which is a thing pages
 * check.
 */
static jval nat_el_getattr(jctx *J, jval t, jval *a, int n);
static jval nat_el_setattr(jctx *J, jval t, jval *a, int n);
static jval nat_add_listener(jctx *J, jval t, jval *a, int n);
static jval nat_remove_listener(jctx *J, jval t, jval *a, int n);
static jval nat_el_append(jctx *J, jval t, jval *a, int n);
static jval nat_el_insert_before(jctx *J, jval t, jval *a, int n);
static jval nat_el_remove_child(jctx *J, jval t, jval *a, int n);
static jval nat_el_remove(jctx *J, jval t, jval *a, int n);

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
    js_set(J, o, "addEventListener",
           js_from_obj(js_native(J, "addEventListener", nat_add_listener)));
    js_set(J, o, "removeEventListener",
           js_from_obj(js_native(J, "removeEventListener", nat_remove_listener)));
    js_set(J, o, "appendChild",
           js_from_obj(js_native(J, "appendChild", nat_el_append)));
    js_set(J, o, "insertBefore",
           js_from_obj(js_native(J, "insertBefore", nat_el_insert_before)));
    js_set(J, o, "removeChild",
           js_from_obj(js_native(J, "removeChild", nat_el_remove_child)));
    js_set(J, o, "remove",
           js_from_obj(js_native(J, "remove", nat_el_remove)));

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

/* --- events ---------------------------------------------------------------
 *
 * A handler is a function and the node it was attached to, kept in a table
 * of its own rather than on the element object, so that what a page can see
 * of an element stays what the DOM says an element has, with none of the
 * browser's bookkeeping showing through.
 *
 * There are two ways a page attaches one, and both are here because real
 * pages use both. addEventListener is the table below. The other is an
 * attribute -- onclick="..." -- which is a piece of program written where a
 * value goes; it is compiled when it fires rather than when it is read,
 * because the great majority of them never fire at all.
 *
 * Delivery is the bubbling half of what a browser does: the node that was
 * hit, then its parent, and so on up to the document. The capture half is
 * not here. Very little uses it, and a listener registered for capture and
 * run at bubble time is worse than one not run: a handler that fires in the
 * wrong order produces a page that is subtly wrong, and a handler that does
 * not fire produces a page that is plainly wrong, which is the one somebody
 * can actually account for.
 */
#define JD_LISTENERS 256
#define JD_TYPE_MAX  24

typedef struct {
    int   node;                  /* a node index, or JD_DOCUMENT */
    char  type[JD_TYPE_MAX];
    jobj *fn;                    /* zeroed when it is removed */
} jlisten;

static jlisten jd_listen[JD_LISTENERS];
static int     jd_nlisten;

/* The event on its way. One at a time: nothing here lets a handler start
   another, so a second set of these would have nothing to hold. */
static int jd_prevented;
static int jd_stopped;

static jval nat_ev_prevent(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    jd_prevented = 1;
    return js_undef();
}

static jval nat_ev_stop(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    jd_stopped = 1;
    return js_undef();
}

static jval nat_add_listener(jctx *J, jval t, jval *a, int n) {
    if (n < 2 || t.t != JS_OBJ || !t.obj) return js_undef();

    jval fn = js_arg(a, n, 1);
    if (fn.t != JS_OBJ || !fn.obj) return js_undef();
    if (fn.obj->kind != JO_FUNC && fn.obj->kind != JO_NATIVE) return js_undef();

    jstr *ty = js_to_str(J, js_arg(a, n, 0));
    if (!ty || jd_nlisten >= JD_LISTENERS) return js_undef();

    jlisten *L = &jd_listen[jd_nlisten++];
    L->node = t.obj->host;
    L->fn = fn.obj;
    u32 w = 0;
    while (w < ty->len && w < JD_TYPE_MAX - 1) { L->type[w] = ty->s[w]; w++; }
    L->type[w] = 0;
    return js_undef();
}

static jval nat_remove_listener(jctx *J, jval t, jval *a, int n) {
    if (n < 2 || t.t != JS_OBJ || !t.obj) return js_undef();

    jstr *ty = js_to_str(J, js_arg(a, n, 0));
    jval fn = js_arg(a, n, 1);
    if (!ty || fn.t != JS_OBJ) return js_undef();

    for (int i = 0; i < jd_nlisten; i++)
        if (jd_listen[i].fn == fn.obj && jd_listen[i].node == t.obj->host
            && js_str_is(ty, jd_listen[i].type))
            jd_listen[i].fn = 0;
    return js_undef();
}

/* What a handler is handed. Enough of an event for a page to tell what
   happened and to say the ordinary thing should not follow. */
static jobj *jd_event_object(const char *type, int target) {
    jobj *e = js_object(&jd_J, JO_PLAIN);
    if (!e) return 0;
    js_set(&jd_J, e, "type", js_from_str(js_str(&jd_J, type)));
    js_set(&jd_J, e, "target",
           target >= 0 ? jd_el_value(&jd_J, target) : js_null());
    js_set(&jd_J, e, "preventDefault",
           js_from_obj(js_native(&jd_J, "preventDefault", nat_ev_prevent)));
    js_set(&jd_J, e, "stopPropagation",
           js_from_obj(js_native(&jd_J, "stopPropagation", nat_ev_stop)));
    return e;
}

/* A handler written in an attribute. The text is a function body and not an
   expression, so it is wrapped in a function before being compiled -- which
   also gives it a scope of its own, so a `var` in an attribute does not
   land among the page's globals. */
static void jd_run_attr(int node, const char *type, jobj *ev) {
    char name[JD_TYPE_MAX + 4];
    int w = 0;
    name[w++] = 'o';
    name[w++] = 'n';
    for (int i = 0; type[i] && w < (int)sizeof(name) - 1; i++) name[w++] = type[i];
    name[w] = 0;

    const char *body = dom_attr(jd_doc, node, name);
    if (!body || !*body) return;

    static char wrapped[4096];
    const char *head = "(function(event){";
    const char *tail = "\n})";
    int n = 0;
    for (const char *p = head; *p; p++) wrapped[n++] = *p;
    for (const char *p = body; *p && n < (int)sizeof(wrapped) - 8; p++)
        wrapped[n++] = *p;
    for (const char *p = tail; *p && n < (int)sizeof(wrapped) - 1; p++)
        wrapped[n++] = *p;
    wrapped[n] = 0;

    jval fn;
    if (!js_eval_text(&jd_J, wrapped, (u32)n, &fn)) { jd_note_error(); return; }

    jval arg = ev ? js_from_obj(ev) : js_undef();
    js_call(&jd_J, fn, jd_el_value(&jd_J, node), &arg, 1);
    if (jd_J.sig != JS_OK) { jd_note_error(); jd_J.sig = JS_OK; }
}

/* Everything registered against one node, for one kind of event. The count
   is taken once: a handler that adds another handler for the same event on
   the same node has not asked for it to run inside this one. */
static void jd_fire(int host, const char *type, jobj *ev) {
    int upto = jd_nlisten;
    for (int i = 0; i < upto && !jd_stopped; i++) {
        if (!jd_listen[i].fn || jd_listen[i].node != host) continue;
        if (!w_same(jd_listen[i].type, type)) continue;

        jd_J.steps = 0;
        jval arg = ev ? js_from_obj(ev) : js_undef();
        jval self = host >= 0 && host < JD_DOCUMENT
                  ? jd_el_value(&jd_J, host) : js_undef();
        js_call(&jd_J, js_from_obj(jd_listen[i].fn), self, &arg, 1);
        if (jd_J.sig != JS_OK) { jd_note_error(); jd_J.sig = JS_OK; }
    }
}

/* Up the tree from whatever was hit, and then the document. Returns whether
   the page asked for the ordinary consequence not to follow. */
static int jd_dispatch(int node, const char *type) {
    if (!jd_open || !jd_doc) return 0;

    jd_prevented = 0;
    jd_stopped = 0;
    jd_J.sig = JS_OK;
    jd_J.steps = 0;

    jobj *ev = jd_event_object(type, node);

    int at = node;
    while (at >= 0 && !jd_stopped) {
        jd_run_attr(at, type, ev);
        if (jd_stopped) break;
        jd_fire(at, type, ev);
        at = jd_doc->nodes[at].parent;
    }
    if (!jd_stopped) jd_fire(JD_DOCUMENT, type, ev);

    return jd_prevented;
}

/* --- changing the tree ----------------------------------------------------
 *
 * A handler that can read a document and not change its shape can do about
 * a third of what pages ask for: it can rewrite the words in a box, which
 * textContent already did, and it cannot add a row, take one out, or move
 * one. These are the four the DOM is actually built on.
 *
 * Nodes are made in the document's own arena rather than the script's,
 * because the layout reads them and the layout outlives any one script; and
 * a node taken out of the tree is unlinked rather than freed, because a
 * script that removes something and puts it back is ordinary.
 */
static jval nat_doc_create_el(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (!jd_doc || n < 1) return js_null();
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s) return js_null();

    int el = dom_create_element(jd_doc, s->s, (int)s->len);
    if (el < 0) return js_null();
    /* Not dirty yet: an element nothing has been told about is not on the
       page, and laying the document out again for it would be a pass over
       everything to draw exactly what is already there. */
    return jd_el_value(J, el);
}

static jval nat_doc_create_text(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (!jd_doc || n < 1) return js_null();
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s) return js_null();

    int node = dom_new(jd_doc, DN_TEXT, T_OTHER);
    if (node < 0) return js_null();
    jd_doc->nodes[node].text = dom_str(jd_doc, s->s, (int)s->len);
    return jd_el_value(J, node);
}

/* The node an argument stands for, or -1 if it is not one of ours. */
static int jd_node_arg(jval v) {
    if (v.t != JS_OBJ || !v.obj) return -1;
    int h = v.obj->host;
    if (h < 0 || h >= JD_DOCUMENT || !jd_doc || h >= jd_doc->count) return -1;
    return h;
}

static jval nat_el_append(jctx *J, jval t, jval *a, int n) {
    (void)J;
    int parent = jd_node_arg(t), child = jd_node_arg(js_arg(a, n, 0));
    if (parent < 0 || child < 0) return js_undef();
    dom_append(jd_doc, parent, child);
    jd_dirty = 1;
    return js_arg(a, n, 0);
}

static jval nat_el_insert_before(jctx *J, jval t, jval *a, int n) {
    (void)J;
    int parent = jd_node_arg(t), child = jd_node_arg(js_arg(a, n, 0));
    if (parent < 0 || child < 0) return js_undef();
    dom_insert_before(jd_doc, parent, child, jd_node_arg(js_arg(a, n, 1)));
    jd_dirty = 1;
    return js_arg(a, n, 0);
}

static jval nat_el_remove_child(jctx *J, jval t, jval *a, int n) {
    (void)J;
    int parent = jd_node_arg(t), child = jd_node_arg(js_arg(a, n, 0));
    if (parent < 0 || child < 0) return js_undef();
    if (jd_doc->nodes[child].parent != parent) return js_undef();
    dom_unlink(jd_doc, child);
    jd_dirty = 1;
    return js_arg(a, n, 0);
}

static jval nat_el_remove(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int el = jd_node_arg(t);
    if (el < 0) return js_undef();
    dom_unlink(jd_doc, el);
    jd_dirty = 1;
    return js_undef();
}

/* --- a class at a time ----------------------------------------------------
 *
 * classList is the one piece of the DOM that is a list in name and a string
 * underneath, and every page that turns something on and off uses it. It is
 * an object of its own so that the four methods have somewhere to live, and
 * it remembers which element it belongs to by the same number an element
 * object carries, moved out of the way of one.
 */
#define JD_CLASSLIST 0x2000000

static int jd_class_node(jval t) {
    if (t.t != JS_OBJ || !t.obj) return -1;
    int h = t.obj->host;
    if (h < JD_CLASSLIST) return -1;
    h -= JD_CLASSLIST;
    return (!jd_doc || h < 0 || h >= jd_doc->count) ? -1 : h;
}

/* Rewrites the attribute with one name added or taken out. Written whole
   rather than edited in place, because the attribute is one string and a
   class is a word inside it. */
static void jd_class_write(int el, const jstr *want, int on) {
    const char *cur = dom_attr(jd_doc, el, "class");
    static char out[512];
    int w = 0;
    int present = 0;

    for (const char *p = cur ? cur : ""; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        int len = (int)(p - s);
        if (!len) continue;

        int same = (u32)len == want->len;
        for (int i = 0; same && i < len; i++) if (s[i] != want->s[i]) same = 0;
        if (same) { present = 1; if (!on) continue; }

        if (w && w < (int)sizeof(out) - 1) out[w++] = ' ';
        for (int i = 0; i < len && w < (int)sizeof(out) - 1; i++) out[w++] = s[i];
    }

    if (on && !present) {
        if (w && w < (int)sizeof(out) - 1) out[w++] = ' ';
        for (u32 i = 0; i < want->len && w < (int)sizeof(out) - 1; i++)
            out[w++] = want->s[i];
    }
    out[w] = 0;

    if (dom_attr_set(jd_doc, el, "class", out)) jd_dirty = 1;
}

static int jd_class_has(int el, const jstr *want) {
    const char *cur = dom_attr(jd_doc, el, "class");
    for (const char *p = cur ? cur : ""; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        int len = (int)(p - s);
        if (!len) continue;
        int same = (u32)len == want->len;
        for (int i = 0; same && i < len; i++) if (s[i] != want->s[i]) same = 0;
        if (same) return 1;
    }
    return 0;
}

static jval nat_cls_add(jctx *J, jval t, jval *a, int n) {
    int el = jd_class_node(t);
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (el >= 0 && s && s->len) jd_class_write(el, s, 1);
    return js_undef();
}

static jval nat_cls_remove(jctx *J, jval t, jval *a, int n) {
    int el = jd_class_node(t);
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (el >= 0 && s && s->len) jd_class_write(el, s, 0);
    return js_undef();
}

static jval nat_cls_contains(jctx *J, jval t, jval *a, int n) {
    int el = jd_class_node(t);
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (el < 0 || !s) return js_bool(0);
    return js_bool(jd_class_has(el, s));
}

static jval nat_cls_toggle(jctx *J, jval t, jval *a, int n) {
    int el = jd_class_node(t);
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (el < 0 || !s || !s->len) return js_bool(0);
    int on = !jd_class_has(el, s);
    jd_class_write(el, s, on);
    return js_bool(on);
}

static jobj *jd_classlist(jctx *J, int el) {
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return 0;
    o->host = JD_CLASSLIST + el;
    js_set(J, o, "add", js_from_obj(js_native(J, "add", nat_cls_add)));
    js_set(J, o, "remove", js_from_obj(js_native(J, "remove", nat_cls_remove)));
    js_set(J, o, "contains",
           js_from_obj(js_native(J, "contains", nat_cls_contains)));
    js_set(J, o, "toggle", js_from_obj(js_native(J, "toggle", nat_cls_toggle)));
    return o;
}

/* --- later ----------------------------------------------------------------
 *
 * setTimeout is the only way a page can arrange for something to happen that
 * nobody asked for, and a great deal of ordinary behaviour is written with
 * it: a menu that closes itself, a message that goes away, anything that
 * waits a moment before it does the expensive thing. A page that calls it
 * and is never called back is not slow, it is stopped part way through.
 *
 * The clock is the kernel's tick counter, which runs at the rate in
 * kernel/timer.c. Getting that number wrong here would make every timer on
 * every page run at the wrong speed rather than not at all, which is the
 * harder kind of wrong to notice, so it is named rather than assumed.
 */
#define JD_TIMERS 64
#define JD_HZ     100            /* kernel/timer.c: frequency */

typedef struct {
    jobj *fn;                    /* zeroed when it is cancelled or spent */
    int   due;                   /* in ticks */
    int   every;                 /* ticks between repeats, or 0 for once */
    int   id;
} jtimer;

static jtimer jd_timer[JD_TIMERS];
static int    jd_ntimer;
static int    jd_timer_id;

static int jd_ticks_for(double ms) {
    if (!(ms > 0)) ms = 0;
    if (ms > 3600000) ms = 3600000;          /* an hour is long enough */
    int t = (int)((ms * JD_HZ + 999) / 1000);
    return t < 1 ? 1 : t;                    /* never zero: a timer that is
                                                due the instant it is set
                                                would run inside the call
                                                that set it */
}

static jval jd_add_timer(jctx *J, jval *a, int n, int repeat) {
    jval fn = js_arg(a, n, 0);
    if (fn.t != JS_OBJ || !fn.obj) return js_num(0);
    if (fn.obj->kind != JO_FUNC && fn.obj->kind != JO_NATIVE) return js_num(0);

    int slot = -1;
    for (int i = 0; i < jd_ntimer; i++) if (!jd_timer[i].fn) { slot = i; break; }
    if (slot < 0 && jd_ntimer < JD_TIMERS) slot = jd_ntimer++;
    if (slot < 0) return js_num(0);

    int t = jd_ticks_for(n > 1 ? js_to_num(J, js_arg(a, n, 1)) : 0);
    jd_timer[slot].fn = fn.obj;
    jd_timer[slot].due = ticks() + t;
    jd_timer[slot].every = repeat ? t : 0;
    jd_timer[slot].id = ++jd_timer_id;
    return js_num(jd_timer[slot].id);
}

static jval nat_set_timeout(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return jd_add_timer(J, a, n, 0);
}

static jval nat_set_interval(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return jd_add_timer(J, a, n, 1);
}

static jval nat_clear_timer(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (n < 1) return js_undef();
    int id = (int)js_to_num(J, js_arg(a, n, 0));
    for (int i = 0; i < jd_ntimer; i++)
        if (jd_timer[i].id == id) jd_timer[i].fn = 0;
    return js_undef();
}

/* Whatever is due, run once. The browser calls this on every pass of its
   loop; the answer is how many ran, so it knows whether to ask whether the
   document changed.
 *
 * A repeating timer is booked forward from now rather than from when it was
 * due, so a page whose interval is shorter than the work in it falls behind
 * rather than accumulating a backlog it can never run down. */
static int jsdom_timers(void) {
    if (!jd_open) return 0;

    int now = ticks();
    int ran = 0;
    for (int i = 0; i < jd_ntimer; i++) {
        if (!jd_timer[i].fn) continue;
        if (now - jd_timer[i].due < 0) continue;

        jobj *fn = jd_timer[i].fn;
        if (jd_timer[i].every > 0) jd_timer[i].due = now + jd_timer[i].every;
        else jd_timer[i].fn = 0;

        jd_J.sig = JS_OK;
        jd_J.steps = 0;
        js_call(&jd_J, js_from_obj(fn), js_undef(), 0, 0);
        if (jd_J.sig != JS_OK) { jd_note_error(); jd_J.sig = JS_OK; }
        ran++;
    }
    return ran;
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
    /* What is in a control, which is the attribute the browser types into
       and the layout draws, so a script reading it gets what is on the
       screen and a script writing it puts something there.

       Only for the elements that have one. Answering "" for every element
       in the document would let a page read a value off a paragraph and
       believe it. */
    if (w_same(name, "value")) {
        int tag = jd_doc->nodes[el].tag;
        const char *v = dom_attr(jd_doc, el, "value");
        if (v) { *out = js_from_str(js_str(J, v)); return 1; }
        if (tag == T_TEXTAREA) {
            static char buf[4096];
            dom_text_content(jd_doc, el, buf, (int)sizeof(buf));
            *out = js_from_str(js_str(J, buf));
            return 1;
        }
        if (tag == T_INPUT || tag == T_SELECT || tag == T_BUTTON) {
            *out = js_from_str(js_str(J, ""));
            return 1;
        }
        return 0;
    }
    /* A box written with a bare `checked` has an empty value, and one that
       has been unticked has to leave something behind saying so. */
    if (w_same(name, "checked")) {
        const char *v = dom_attr(jd_doc, el, "checked");
        *out = js_bool(v && !w_same(v, "0"));
        return 1;
    }
    if (w_same(name, "classList")) {
        jobj *cl = jd_classlist(J, el);
        *out = cl ? js_from_obj(cl) : js_null();
        return 1;
    }

    /* Where this sits, which is how a handler gets from the thing that was
       clicked to the row it is in. Text nodes are part of the answer: a
       script walking children and skipping them would never find the words.
       Anything off the end of the tree is null rather than undefined,
       because null is what a page tests for. */
    if (w_same(name, "parentNode") || w_same(name, "parentElement")) {
        int p = jd_doc->nodes[el].parent;
        *out = p >= 0 ? jd_el_value(J, p) : js_null();
        return 1;
    }
    if (w_same(name, "firstChild")) {
        int c = jd_doc->nodes[el].first;
        *out = c >= 0 ? jd_el_value(J, c) : js_null();
        return 1;
    }
    if (w_same(name, "lastChild")) {
        int c = jd_doc->nodes[el].last;
        *out = c >= 0 ? jd_el_value(J, c) : js_null();
        return 1;
    }
    if (w_same(name, "nextSibling")) {
        int c = jd_doc->nodes[el].next;
        *out = c >= 0 ? jd_el_value(J, c) : js_null();
        return 1;
    }
    if (w_same(name, "previousSibling")) {
        int c = jd_doc->nodes[el].prev;
        *out = c >= 0 ? jd_el_value(J, c) : js_null();
        return 1;
    }
    /* A snapshot rather than the live list a browser hands back. A page that
       holds one and expects it to follow the document would be wrong here,
       and a page that walks it once -- which is nearly all of them -- is
       not. Elements only, which is what this name means. */
    if (w_same(name, "children")) {
        jobj *arr = js_array(J);
        if (!arr) return 0;
        u32 at = 0;
        for (int c = jd_doc->nodes[el].first; c >= 0;
             c = jd_doc->nodes[c].next)
            if (jd_doc->nodes[c].kind == DN_ELEMENT)
                js_arr_set(J, arr, at++, jd_el_value(J, c));
        *out = js_from_obj(arr);
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
    if (w_same(name, "value")) {
        jstr *s = js_to_str(J, v);
        if (s) {
            dom_attr_set(jd_doc, el, "value", s->s);
            jd_dirty = 1;
        }
        return 1;
    }
    if (w_same(name, "checked")) {
        dom_attr_set(jd_doc, el, "checked", js_to_bool(v) ? "1" : "0");
        jd_dirty = 1;
        return 1;
    }
    return 0;
}

/* --- opening and closing the world ----------------------------------------
 *
 * One context for the whole document rather than one per script, because a
 * function declared in the first script and called from the second is
 * ordinary and would otherwise not be found. It is now also one context for
 * the whole life of the page, because a handler is a piece of a program that
 * runs after the program has finished, and there has to be something left
 * for it to run in.
 */

/* Whether this page has anything to run at all. A script element is the
   obvious half; the other is a handler attribute, and a page can carry
   those without a script element anywhere in it. Neither costs a context,
   a global object or a quarter megabyte, which is still what the great
   majority of pages should cost. */
static int jd_page_scripts(const ddoc *d) {
    for (int i = 0; i < d->count; i++) {
        if (d->nodes[i].kind != DN_ELEMENT) continue;
        if (d->nodes[i].tag == T_SCRIPT) return 1;
        const dnode *n = &d->nodes[i];
        for (int a = n->attr_at; a < n->attr_at + n->attr_n; a++) {
            const char *nm = d->arena + d->attrs[a].name;
            if (nm[0] == 'o' && nm[1] == 'n' && nm[2]) return 1;
        }
    }
    return 0;
}

/* Every element object, every string and every scope goes here. The document
   keeps whatever the scripts wrote into it, because that lives in the
   document's own arena rather than this one. */
static void jsdom_close(void) {
    if (!jd_open) return;
    js_done(&jd_J);
    jd_open = 0;
    jd_doc = 0;
    jd_wrap = 0;
    jd_nlisten = 0;
    jd_ntimer = 0;
}

static int jsdom_open(ddoc *d) {
    jsdom_close();
    if (!d || !jd_page_scripts(d)) return 0;

    js_init(&jd_J);
    jd_J.host_get = jd_host_get;
    jd_J.host_set = jd_host_set;

    jd_doc = d;
    jd_dirty = 0;
    jd_err[0] = 0;
    jd_nlisten = 0;
    jd_ntimer = 0;
    jd_open = 1;

    /* Sized for the whole document rather than for the part of it that
       exists now: a script may add nodes past the end of what was parsed,
       and a wrapper for one of those has to land somewhere. */
    jd_wrap = (jobj **)js_alloc(&jd_J, (u32)sizeof(jobj *) * (u32)JD_WRAPS);
    if (jd_wrap)
        for (int i = 0; i < JD_WRAPS; i++) jd_wrap[i] = 0;

    jobj *document = js_object(&jd_J, JO_PLAIN);
    if (document) {
        document->host = JD_DOCUMENT;
        js_set(&jd_J, document, "getElementById",
               js_from_obj(js_native(&jd_J, "getElementById", nat_doc_by_id)));
        js_set(&jd_J, document, "getElementsByTagName",
               js_from_obj(js_native(&jd_J, "getElementsByTagName",
                                     nat_doc_by_tag)));
        js_set(&jd_J, document, "addEventListener",
               js_from_obj(js_native(&jd_J, "addEventListener",
                                     nat_add_listener)));
        js_set(&jd_J, document, "removeEventListener",
               js_from_obj(js_native(&jd_J, "removeEventListener",
                                     nat_remove_listener)));
        js_set(&jd_J, document, "createElement",
               js_from_obj(js_native(&jd_J, "createElement",
                                     nat_doc_create_el)));
        js_set(&jd_J, document, "createTextNode",
               js_from_obj(js_native(&jd_J, "createTextNode",
                                     nat_doc_create_text)));
        js_declare(&jd_J, jd_J.global, js_str(&jd_J, "document"),
                   js_from_obj(document));
    }

    /* Set on the global object, which is where a page looks for them,
       whether it writes setTimeout or window.setTimeout. */
    js_declare(&jd_J, jd_J.global, js_str(&jd_J, "setTimeout"),
               js_from_obj(js_native(&jd_J, "setTimeout", nat_set_timeout)));
    js_declare(&jd_J, jd_J.global, js_str(&jd_J, "setInterval"),
               js_from_obj(js_native(&jd_J, "setInterval", nat_set_interval)));
    js_declare(&jd_J, jd_J.global, js_str(&jd_J, "clearTimeout"),
               js_from_obj(js_native(&jd_J, "clearTimeout", nat_clear_timer)));
    js_declare(&jd_J, jd_J.global, js_str(&jd_J, "clearInterval"),
               js_from_obj(js_native(&jd_J, "clearInterval", nat_clear_timer)));

    /* `window` is the global object, which is what it is in a browser, so a
       page that writes window.x and reads x gets the same thing. */
    if (jd_J.global_obj)
        js_declare(&jd_J, jd_J.global, js_str(&jd_J, "window"),
                   js_from_obj(jd_J.global_obj));
    return 1;
}

/* Returns how many scripts ran. `err` gets the first failure, because a page
 * with a broken script should say so somewhere rather than silently doing
 * nothing — and the second failure is usually the first one's fault. */
static int jsdom_scripts(char *err, int errcap) {
    if (err && errcap) err[0] = 0;
    if (!jd_open || !jd_doc) return 0;

    ddoc *d = jd_doc;
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
        if (!js_run(&jd_J, text, len)) jd_note_error();
    }

    if (err && errcap) {
        int w = 0;
        for (const char *p = jd_err; *p && w < errcap - 1; p++) err[w++] = *p;
        err[w] = 0;
    }
    return ran;
}

/* The two a page waits on before it will do anything. Both are delivered to
   the document, which is where a page that wants them listens. */
static void jsdom_loaded(void) {
    if (!jd_open) return;
    jd_dispatch(-1, "DOMContentLoaded");
    jd_dispatch(-1, "load");
}

/* A click on an element, and whether the page asked for the ordinary
   consequence -- following a link -- not to follow. */
static int jsdom_click(int node) { return jd_dispatch(node, "click"); }

static int jsdom_live(void) { return jd_open; }

/* Whether anything has changed the document since this was last asked, which
   is the question the browser asks before deciding to lay the page out
   again. Asking clears it. */
static int jsdom_changed(void) {
    int v = jd_dirty;
    jd_dirty = 0;
    return v;
}

static const char *jsdom_error(void) { return jd_err; }
