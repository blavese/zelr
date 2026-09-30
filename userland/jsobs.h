#pragma once
/* Watching the page: MutationObserver, IntersectionObserver and
 * ResizeObserver, each as much as is true of this browser.
 *
 * Included from jsdom.h, which tells this file of every change to the tree
 * (jd_record_children, jd_record_attr, jd_record_text).
 *
 * MutationObserver hears of each change as the standard describes it -- a
 * list of records handed to the callback once the script that made them
 * has finished, as a microtask -- for what it asked to watch: children,
 * attributes, text, under the node or on it alone, with the old values when
 * it asked for them.
 *
 * IntersectionObserver: this browser lays the whole page out at once and
 * draws all of it, pictures included, so everything observed that the
 * layout drew is reported as intersecting, once, on the pass after it is
 * observed (when the page has been laid out), and anything it did not draw
 * as not. That is true here, and it is what makes a page's lazy pictures
 * and infinite lists go on to the next step.
 *
 * ResizeObserver: an element's size where the layout drew it, reported on
 * the pass after it is observed and on the pass after any layout that
 * changes it (jsdom_laid_out, which the browser calls after each).
 */

/* --- MutationObserver ------------------------------------------------------------------ */

typedef struct {
    jobj *obs;                   /* the MutationObserver */
    int   target;                /* the node it watches; -1 for the document */
    u8    child, attrs, chars, subtree, attr_old, char_old;
    jobj *filter;                /* attribute names, or none */
} jmreg;

static jmreg *jd_mregs;
static int    jd_nmregs, jd_capmregs;
static jstr  *jd_k_records, *jd_k_mcb;
static jobj  *jd_p_mobs, *jd_p_record, *jd_mo_deliver_fn;
static int    jd_mo_queued;          /* a delivery is waiting to run */

static void jd_mo_deliver_all(jctx *J);

static jval nat_mo_deliver(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jd_mo_queued = 0;
    jd_mo_deliver_all(J);
    return js_undef();
}

/* Whether a registration covers a change at node. */
static int jd_mreg_covers(const jmreg *r, int node) {
    if (r->target == node) return 1;
    if (!r->subtree) return 0;
    if (r->target < 0) return jd_connected(node);
    return node >= 0 && dom_contains(jd_doc, r->target, node);
}

/* The nodes a record names. Most records have none on one side or both,
   and those share one empty list: each list is a slot in jd_lists, and a
   page that builds itself under an observer makes thousands of records. */
static jval jd_mo_none;

static jval jd_node_list(jctx *J, int node) {
    int at;
    if (node < 0 && jd_mo_none.t == JS_OBJ) return jd_mo_none;
    jval l = jd_static_list(J, &at);
    if (node < 0) jd_mo_none = l;
    if (l.t == JS_OBJ && node >= 0) jd_list_add(&jd_lists[at], node);
    return l;
}

enum { MO_CHILD, MO_ATTR, MO_TEXT };

/* Whether a registration asks to hear of this change. */
static int jd_mreg_wants(const jmreg *r, int kind, int target, const char *attr) {
    if (!r->obs) return 0;
    int want = kind == MO_CHILD ? r->child : kind == MO_ATTR ? r->attrs : r->chars;
    if (!want || !jd_mreg_covers(r, target)) return 0;
    if (kind == MO_ATTR && r->filter && attr) {
        for (u32 k = 0; k < r->filter->len; k++)
            if (r->filter->items[k].t == JS_STR && js_str_is(r->filter->items[k].str, attr)) return 1;
        return 0;
    }
    return 1;
}

/* A record, added to each observer that watches for it: one each, however
   many of its registrations cover the change, with the old value if any of
   them asked for it. */
static void jd_mo_record(int kind, int target, int added, int removed, int prev, int next,
                         const char *attr, const char *old) {
    if (!jd_nmregs || !jd_open || jd_spent()) return;
    jctx *J = &jd_J;
    static const char *const names[] = { "childList", "attributes", "characterData" };
    for (int i = 0; i < jd_nmregs; i++) {
        jmreg *r = &jd_mregs[i];
        if (!jd_mreg_wants(r, kind, target, attr)) continue;
        int keep_old = 0, again = 0;
        for (int k = 0; k < jd_nmregs; k++) {
            if (jd_mregs[k].obs != r->obs || !jd_mreg_wants(&jd_mregs[k], kind, target, attr)) continue;
            if (k < i) again = 1;
            if (kind == MO_ATTR && jd_mregs[k].attr_old) keep_old = 1;
            if (kind == MO_TEXT && jd_mregs[k].char_old) keep_old = 1;
        }
        if (again) continue;
        jobj *rec = js_object_with(J, JO_PLAIN, jd_p_record);
        if (!rec) return;
        js_set(J, rec, "type", jd_str(names[kind]));
        js_set(J, rec, "target", jd_el_value(J, target));
        js_set(J, rec, "addedNodes", jd_node_list(J, added));
        js_set(J, rec, "removedNodes", jd_node_list(J, removed));
        js_set(J, rec, "previousSibling", prev >= 0 ? jd_el_value(J, prev) : js_null());
        js_set(J, rec, "nextSibling", next >= 0 ? jd_el_value(J, next) : js_null());
        js_set(J, rec, "attributeName", attr ? jd_str(attr) : js_null());
        js_set(J, rec, "attributeNamespace", js_null());
        /* Copied now: a value that fits is written over where it lies
           (dom.h), and the record is made before that happens. */
        js_set(J, rec, "oldValue", keep_old && old ? jd_str(old) : js_null());
        jval list = jd_kept(r->obs, jd_k_records);
        if (list.t != JS_OBJ) {
            jobj *arr = js_array(J);
            if (!arr) return;
            list = js_from_obj(arr);
            jd_keep(r->obs, jd_k_records, list);
        }
        js_arr_push(J, list.obj, js_from_obj(rec));
        if (!jd_mo_queued && jd_mo_deliver_fn) {
            jd_mo_queued = 1;
            js_enqueue(J, JOB_CALL, js_from_obj(jd_mo_deliver_fn), js_undef(), 0, 0);
        }
    }
}

/* Each observer with records is handed them, in the order the observers
   were made to observe, as a microtask once the script that made the
   changes has finished. The budget of steps is the drain's, so an observer
   that changes what it watches every time runs out of it rather than
   holding the browser for ever. */
static void jd_mo_deliver_all(jctx *J) {
    for (int i = 0; i < jd_nmregs; i++) {
        jobj *obs = jd_mregs[i].obs;
        if (!obs) continue;
        int first = 1;
        for (int k = 0; k < i; k++) if (jd_mregs[k].obs == obs) first = 0;
        if (!first) continue;
        jval list = jd_kept(obs, jd_k_records);
        if (list.t != JS_OBJ || !list.obj->len) continue;
        jd_keep(obs, jd_k_records, js_undef());
        jval cb = jd_kept(obs, jd_k_mcb);
        if (!js_callable(cb)) continue;
        jval args[2] = { list, js_from_obj(obs) };
        js_call(J, cb, js_from_obj(obs), args, 2);
        if (J->sig == JS_FAILED) return;
        if (J->sig != JS_OK) { jd_note_error(); J->sig = JS_OK; }
    }
}

static void jd_record_children(int target, int added, int removed, int prev, int next) {
    if (jd_nmregs) jd_mo_record(MO_CHILD, target, added, removed, prev, next, 0, 0);
}

static void jd_record_attr(int target, const char *name, const char *old) {
    if (jd_nmregs) jd_mo_record(MO_ATTR, target, -1, -1, -1, -1, name, old);
}

static void jd_record_text(int target, const char *old) {
    if (jd_nmregs) jd_mo_record(MO_TEXT, target, -1, -1, -1, -1, 0, old);
}

static jval nat_mo_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "MutationObserver is made with new", J->error_line);
    jval cb = js_arg(a, n, 0);
    if (!js_callable(cb)) return js_throw(J, JS_ERR_TYPE, "a MutationObserver needs a function", J->error_line);
    jd_keep(t.obj, jd_k_mcb, cb);
    return js_undef();
}

static jval nat_mo_observe(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jval tv = js_arg(a, n, 0), opt = js_arg(a, n, 1);
    int target = jd_node_of(tv);
    if (target < 0 && !jd_is_doc(tv)) return js_throw(J, JS_ERR_TYPE, "observe needs a node", J->error_line);
    if (jd_is_doc(tv)) target = -1;
    jmreg r;
    r.obs = t.obj;
    r.target = target;
    r.child = js_is_obj(opt) && js_to_bool(js_get(J, opt, js_str(J, "childList")));
    jval at = js_is_obj(opt) ? js_get(J, opt, js_str(J, "attributes")) : js_undef();
    jval ch = js_is_obj(opt) ? js_get(J, opt, js_str(J, "characterData")) : js_undef();
    r.subtree = js_is_obj(opt) && js_to_bool(js_get(J, opt, js_str(J, "subtree")));
    r.attr_old = js_is_obj(opt) && js_to_bool(js_get(J, opt, js_str(J, "attributeOldValue")));
    r.char_old = js_is_obj(opt) && js_to_bool(js_get(J, opt, js_str(J, "characterDataOldValue")));
    jval filt = js_is_obj(opt) ? js_get(J, opt, js_str(J, "attributeFilter")) : js_undef();
    r.filter = 0;
    if (js_is_obj(filt)) {
        jargs A;
        js_args_init(&A);
        if (js_iter_collect(J, filt, &A)) {
            r.filter = js_array(J);
            for (int i = 0; r.filter && i < A.n; i++) js_arr_push(J, r.filter, js_from_str(jd_lower_str(J, js_to_str(J, A.v[i]))));
        }
        js_args_free(&A);
    }
    /* What the options imply, as the standard has them. */
    r.attrs = at.t != JS_UNDEF ? js_to_bool(at) : (r.attr_old || r.filter);
    r.chars = ch.t != JS_UNDEF ? js_to_bool(ch) : r.char_old;
    if (J->sig != JS_OK) return js_undef();
    if (!r.child && !r.attrs && !r.chars)
        return js_throw(J, JS_ERR_TYPE, "observe needs childList, attributes or characterData", J->error_line);
    /* Observing the same node again replaces what was asked. */
    for (int i = 0; i < jd_nmregs; i++)
        if (jd_mregs[i].obs == t.obj && jd_mregs[i].target == target) { jd_mregs[i] = r; return js_undef(); }
    /* A place a disconnected observer left, before a new one. */
    for (int i = 0; i < jd_nmregs; i++)
        if (!jd_mregs[i].obs) { jd_mregs[i] = r; return js_undef(); }
    if (jd_nmregs >= jd_capmregs) {
        int cap = jd_capmregs ? jd_capmregs * 2 : 16;
        jmreg *more = (jmreg *)malloc((u64)cap * sizeof(jmreg));
        if (!more) return js_undef();
        volatile u8 *d = (volatile u8 *)more;
        const u8 *s = (const u8 *)jd_mregs;
        for (u64 i = 0; i < (u64)jd_nmregs * sizeof(jmreg); i++) d[i] = s[i];
        if (jd_mregs) free(jd_mregs);
        jd_mregs = more;
        jd_capmregs = cap;
    }
    jd_mregs[jd_nmregs++] = r;
    return js_undef();
}

static jval nat_mo_disconnect(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (!js_is_obj(t)) return js_undef();
    for (int i = 0; i < jd_nmregs; i++) if (jd_mregs[i].obs == t.obj) jd_mregs[i].obs = 0;
    jd_keep(t.obj, jd_k_records, js_undef());
    return js_undef();
}

static jval nat_mo_take(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return jd_illegal(J);
    jval list = jd_kept(t.obj, jd_k_records);
    jd_keep(t.obj, jd_k_records, js_undef());
    return list.t == JS_OBJ ? list : js_from_obj(js_array(J));
}

/* --- IntersectionObserver and ResizeObserver ------------------------------------------------ */

static jobj *jd_p_iobs, *jd_p_robs;
static jstr *jd_k_ocb, *jd_k_targets, *jd_k_sizes;
static jobj *jd_robservers;            /* every ResizeObserver with a target */

/* The entries for an IntersectionObserver's new targets: every one drawn
   and intersecting, with where it is. */
static void jd_io_due(jval arg) {
    if (!js_is_obj(arg)) return;
    jctx *J = &jd_J;
    jobj *obs = arg.obj;
    jval pend = jd_kept(obs, jd_k_targets);
    if (pend.t != JS_OBJ || !pend.obj->len) return;
    jd_keep(obs, jd_k_targets, js_undef());
    jobj *entries = js_array(J);
    for (u32 i = 0; entries && i < pend.obj->len; i++) {
        int el = jd_node_of(pend.obj->items[i]);
        if (el < 0) continue;
        /* One the layout did not draw (display: none) is not in view. */
        int x = 0, y = 0, w = 0, h = 0;
        int drawn = jd_box(el, &x, &y, &w, &h);
        if (!drawn) x = y = w = h = 0;
        jobj *e = js_object(J, JO_PLAIN);
        if (!e) break;
        js_set(J, e, "target", pend.obj->items[i]);
        js_set(J, e, "isIntersecting", js_bool(drawn));
        js_set(J, e, "intersectionRatio", js_num(drawn ? 1 : 0));
        js_set(J, e, "time", js_num(jd_now_ms()));
        js_set(J, e, "boundingClientRect", jd_rect(J, x, y, w, h));
        js_set(J, e, "intersectionRect", jd_rect(J, x, y, w, h));
        js_set(J, e, "rootBounds", jd_rect(J, 0, 0, jd_view_w, jd_view_h));
        js_arr_push(J, entries, js_from_obj(e));
    }
    jval cb = jd_kept(obs, jd_k_ocb);
    if (entries && entries->len && js_callable(cb)) {
        jval args[2] = { js_from_obj(entries), js_from_obj(obs) };
        jd_call(cb, js_from_obj(obs), args, 2);
    }
}

static jval jd_observer_ctor(jctx *J, jval t, jval *a, int n, const char *what) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t)) {
        char msg[64];
        int w = 0;
        for (const char *p = what; *p; p++) msg[w++] = *p;
        for (const char *p = " is made with new"; *p; p++) msg[w++] = *p;
        msg[w] = 0;
        return js_throw(J, JS_ERR_TYPE, msg, J->error_line);
    }
    jval cb = js_arg(a, n, 0);
    if (!js_callable(cb)) return js_throw(J, JS_ERR_TYPE, "an observer needs a function", J->error_line);
    jd_keep(t.obj, jd_k_ocb, cb);
    return js_undef();
}

static jval nat_io_ctor(jctx *J, jval t, jval *a, int n) {
    jval r = jd_observer_ctor(J, t, a, n, "IntersectionObserver");
    if (J->sig != JS_OK) return r;
    jval opt = js_arg(a, n, 1);
    jval root = js_is_obj(opt) ? js_get(J, opt, js_str(J, "root")) : js_undef();
    jval margin = js_is_obj(opt) ? js_get(J, opt, js_str(J, "rootMargin")) : js_undef();
    js_set(J, t.obj, "root", root.t == JS_UNDEF ? js_null() : root);
    js_set(J, t.obj, "rootMargin", margin.t == JS_UNDEF ? jd_str("0px 0px 0px 0px") : js_from_str(js_to_str(J, margin)));
    /* The thresholds asked for, read back; everything drawn is wholly in
       view here, which crosses every one of them at once. */
    jobj *th = js_array(J);
    jval want = js_is_obj(opt) ? js_get(J, opt, js_str(J, "threshold")) : js_undef();
    if (th && js_is_obj(want) && want.obj->kind == JO_ARRAY) {
        for (u32 i = 0; i < want.obj->len; i++) js_arr_push(J, th, js_num(js_to_num(J, want.obj->items[i])));
    } else if (th) {
        js_arr_push(J, th, js_num(want.t == JS_UNDEF ? 0 : js_to_num(J, want)));
    }
    js_set(J, t.obj, "thresholds", js_from_obj(th));
    return js_undef();
}

static jval nat_io_observe(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jval el = js_arg(a, n, 0);
    if (jd_el_of(el) < 0) return js_throw(J, JS_ERR_TYPE, "observe needs an element", J->error_line);
    jval pend = jd_kept(t.obj, jd_k_targets);
    if (pend.t != JS_OBJ) {
        jobj *arr = js_array(J);
        if (!arr) return js_undef();
        pend = js_from_obj(arr);
        jd_keep(t.obj, jd_k_targets, pend);
        jd_later_native(jd_io_due, t, 1);
    }
    js_arr_push(J, pend.obj, el);
    return js_undef();
}

static void jd_drop_target(jobj *o, jstr *key, jval el) {
    jval pend = jd_kept(o, key);
    if (pend.t != JS_OBJ) return;
    u32 w = 0;
    for (u32 i = 0; i < pend.obj->len; i++)
        if (!(pend.obj->items[i].t == JS_OBJ && el.t == JS_OBJ && pend.obj->items[i].obj == el.obj))
            pend.obj->items[w++] = pend.obj->items[i];
    pend.obj->len = w;
}

static jval nat_io_unobserve(jctx *J, jval t, jval *a, int n) {
    (void)J;
    if (js_is_obj(t)) jd_drop_target(t.obj, jd_k_targets, js_arg(a, n, 0));
    return js_undef();
}

static jval nat_io_disconnect(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (js_is_obj(t)) jd_keep(t.obj, jd_k_targets, js_undef());
    return js_undef();
}

static jval nat_empty_array(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    return js_from_obj(js_array(J));
}

/* The box inside an element's padding and borders, from its style when the
   browser can say what that is (jsdom_styles_with), else the whole box. */
static void jd_content_box(int el, int w, int h, int *cx, int *cy, int *cw, int *ch) {
    *cx = *cy = 0;
    *cw = w;
    *ch = h;
    if (!jd_style_of || !jd_is_element(el) || (!w && !h)) return;
    cstyle *s = (cstyle *)malloc(sizeof(cstyle));
    if (!s) return;
    if (jd_style_of(el, s)) {
        *cx = s->pl;
        *cy = s->pt;
        *cw = w - s->pl - s->pr - s->bl - s->br;
        *ch = h - s->pt - s->pb - s->bt - s->bb;
        if (*cw < 0) *cw = 0;
        if (*ch < 0) *ch = 0;
    }
    free(s);
}

static jval jd_box_size(jctx *J, int w, int h) {
    jobj *box = js_object(J, JO_PLAIN), *list = js_array(J);
    if (!box || !list) return js_null();
    js_set(J, box, "inlineSize", js_num(w));
    js_set(J, box, "blockSize", js_num(h));
    js_arr_push(J, list, js_from_obj(box));
    return js_from_obj(list);
}

/* A ResizeObserver keeps each target with the size last reported for it
   (-1 before the first), and reports the ones whose size a layout
   changed. The size is where the layout drew the element, so one that is
   not drawn (display: none, or nothing laid out yet) is 0 by 0, as the
   standard has it. */
static void jd_ro_check(jobj *obs) {
    jctx *J = &jd_J;
    jval tg = jd_kept(obs, jd_k_targets), sz = jd_kept(obs, jd_k_sizes);
    if (tg.t != JS_OBJ || sz.t != JS_OBJ) return;
    jobj *entries = 0;
    for (u32 i = 0; i < tg.obj->len && i < sz.obj->len; i++) {
        int el = jd_node_of(tg.obj->items[i]);
        if (el < 0) continue;
        int x, y, w = 0, h = 0;
        if (!jd_box(el, &x, &y, &w, &h)) w = h = 0;
        double key = (double)w * 100000.0 + (double)h;
        if (sz.obj->items[i].t == JS_NUM && sz.obj->items[i].num == key) continue;
        sz.obj->items[i] = js_num(key);
        if (!entries) entries = js_array(J);
        jobj *e = js_object(J, JO_PLAIN);
        if (!e || !entries) break;
        int cx, cy, cw, ch;
        jd_content_box(el, w, h, &cx, &cy, &cw, &ch);
        js_set(J, e, "target", tg.obj->items[i]);
        js_set(J, e, "contentRect", jd_rect(J, cx, cy, cw, ch));
        js_set(J, e, "borderBoxSize", jd_box_size(J, w, h));
        js_set(J, e, "contentBoxSize", jd_box_size(J, cw, ch));
        /* One CSS pixel is one pixel of the screen here (devicePixelRatio 1). */
        js_set(J, e, "devicePixelContentBoxSize", jd_box_size(J, cw, ch));
        js_arr_push(J, entries, js_from_obj(e));
    }
    jval cb = jd_kept(obs, jd_k_ocb);
    if (entries && entries->len && js_callable(cb)) {
        jval args[2] = { js_from_obj(entries), js_from_obj(obs) };
        jd_call(cb, js_from_obj(obs), args, 2);
    }
}

static void jd_ro_due(jval arg) { if (js_is_obj(arg)) jd_ro_check(arg.obj); }

static jval nat_ro_ctor(jctx *J, jval t, jval *a, int n) { return jd_observer_ctor(J, t, a, n, "ResizeObserver"); }

static jval nat_ro_observe(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jval el = js_arg(a, n, 0);
    if (jd_el_of(el) < 0) return js_throw(J, JS_ERR_TYPE, "observe needs an element", J->error_line);
    jval tg = jd_kept(t.obj, jd_k_targets), sz = jd_kept(t.obj, jd_k_sizes);
    if (tg.t != JS_OBJ) {
        jobj *a1 = js_array(J), *a2 = js_array(J);
        if (!a1 || !a2) return js_undef();
        tg = js_from_obj(a1);
        sz = js_from_obj(a2);
        jd_keep(t.obj, jd_k_targets, tg);
        jd_keep(t.obj, jd_k_sizes, sz);
        if (!jd_robservers) jd_robservers = js_array(J);
        if (jd_robservers) js_arr_push(J, jd_robservers, t);
    }
    for (u32 i = 0; i < tg.obj->len; i++) if (tg.obj->items[i].obj == el.obj) return js_undef();
    js_arr_push(J, tg.obj, el);
    js_arr_push(J, sz.obj, js_num(-1));
    jd_later_native(jd_ro_due, t, 1);
    return js_undef();
}

static jval nat_ro_unobserve(jctx *J, jval t, jval *a, int n) {
    (void)J;
    if (!js_is_obj(t)) return js_undef();
    jval tg = jd_kept(t.obj, jd_k_targets), sz = jd_kept(t.obj, jd_k_sizes);
    jval el = js_arg(a, n, 0);
    if (tg.t != JS_OBJ || sz.t != JS_OBJ) return js_undef();
    u32 w = 0;
    for (u32 i = 0; i < tg.obj->len; i++) {
        if (tg.obj->items[i].t == JS_OBJ && el.t == JS_OBJ && tg.obj->items[i].obj == el.obj) continue;
        tg.obj->items[w] = tg.obj->items[i];
        if (i < sz.obj->len) sz.obj->items[w] = sz.obj->items[i];
        w++;
    }
    tg.obj->len = w;
    sz.obj->len = w;
    return js_undef();
}

static jval nat_ro_disconnect(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (!js_is_obj(t)) return js_undef();
    jval tg = jd_kept(t.obj, jd_k_targets), sz = jd_kept(t.obj, jd_k_sizes);
    if (tg.t == JS_OBJ) tg.obj->len = 0;
    if (sz.t == JS_OBJ) sz.obj->len = 0;
    return js_undef();
}

static int jd_ro_pending;

static void jd_ro_all(jval arg) {
    (void)arg;
    jd_ro_pending = 0;
    if (!jd_robservers || jd_spent()) return;
    for (u32 i = 0; i < jd_robservers->len; i++)
        if (js_is_obj(jd_robservers->items[i])) jd_ro_check(jd_robservers->items[i].obj);
}

/* After every layout: each ResizeObserver asked, on the next pass, whether
   its targets changed size. The browser calls this once the page is laid
   out again. Not asked here and now, because the page's callbacks would
   then run inside the layout, and one that changes a size would ask for
   another layout from inside this one. */
__attribute__((unused)) static void jsdom_laid_out(void) {
    if (!jd_open || !jd_robservers || jd_ro_pending || jd_spent()) return;
    if (jd_later_native(jd_ro_all, js_undef(), 1)) jd_ro_pending = 1;
}

static void jd_setup_perf_observer(jctx *J);

static void jd_setup_observers(jctx *J) {
    jd_k_records = js_sym_new(J, "records", 7);
    jd_k_mcb = js_sym_new(J, "callback", 8);
    jd_k_ocb = js_sym_new(J, "callback", 8);
    jd_k_targets = js_sym_new(J, "targets", 7);
    jd_k_sizes = js_sym_new(J, "sizes", 5);
    jd_nmregs = 0;
    jd_mo_queued = 0;
    jd_mo_none = js_undef();
    jd_robservers = 0;
    jd_ro_pending = 0;
    jd_mo_deliver_fn = js_native(J, "deliver", nat_mo_deliver);

    jd_p_mobs = jd_interface(J, "MutationObserver", 0, nat_mo_ctor, 1);
    jd_method(J, jd_p_mobs, "observe", nat_mo_observe, 1);
    jd_method(J, jd_p_mobs, "disconnect", nat_mo_disconnect, 0);
    jd_method(J, jd_p_mobs, "takeRecords", nat_mo_take, 0);
    jd_p_record = jd_interface(J, "MutationRecord", 0, 0, 0);

    jd_p_iobs = jd_interface(J, "IntersectionObserver", 0, nat_io_ctor, 1);
    jd_method(J, jd_p_iobs, "observe", nat_io_observe, 1);
    jd_method(J, jd_p_iobs, "unobserve", nat_io_unobserve, 1);
    jd_method(J, jd_p_iobs, "disconnect", nat_io_disconnect, 0);
    jd_method(J, jd_p_iobs, "takeRecords", nat_empty_array, 0);
    jd_interface(J, "IntersectionObserverEntry", 0, 0, 0);

    jd_p_robs = jd_interface(J, "ResizeObserver", 0, nat_ro_ctor, 1);
    jd_method(J, jd_p_robs, "observe", nat_ro_observe, 1);
    jd_method(J, jd_p_robs, "unobserve", nat_ro_unobserve, 1);
    jd_method(J, jd_p_robs, "disconnect", nat_ro_disconnect, 0);
    jd_interface(J, "ResizeObserverEntry", 0, 0, 0);
    jd_setup_perf_observer(J);
}

/* --- PerformanceObserver --------------------------------------------------------------------
 *
 * The only performance entries this browser has are the marks and measures
 * a page makes itself and the page's navigation entry (jswin.h), so those are
 * the entry types it supports (PerformanceObserver.supportedEntryTypes), and
 * an observer of them hears
 * of each new one on the next pass, with the ones already made if it asked
 * for them (buffered). One that asks for another type -- long tasks, paint,
 * resources, layout shifts, none of which this browser records -- is told
 * nothing, as the standard says a type the browser does not support is
 * ignored. LinkedIn stopped on the name. */

static jobj *jd_pobservers;
static jstr *jd_k_ptypes, *jd_k_pqueue, *jd_k_plist;
static jobj *jd_p_pobs, *jd_p_plist;
static int jd_po_pending;

static int jd_perf_type_known(const jstr *s) {
    return js_str_is(s, "mark") || js_str_is(s, "measure") || js_str_is(s, "navigation");
}

static int jd_po_watches(jobj *obs, const jstr *type) {
    jval types = jd_kept(obs, jd_k_ptypes);
    if (types.t != JS_OBJ) return 0;
    for (u32 i = 0; i < types.obj->len; i++)
        if (types.obj->items[i].t == JS_STR && js_str_eq(types.obj->items[i].str, type)) return 1;
    return 0;
}

static void jd_po_queue(jobj *obs, jval entry) {
    jval q = jd_kept(obs, jd_k_pqueue);
    if (q.t != JS_OBJ) {
        jobj *arr = js_array(&jd_J);
        if (!arr) return;
        q = js_from_obj(arr);
        jd_keep(obs, jd_k_pqueue, q);
    }
    js_arr_push(&jd_J, q.obj, entry);
}

static jval jd_plist_entries(jctx *J, jval t, const jstr *name, const jstr *type) {
    jobj *out = js_array(J);
    jval all = js_is_obj(t) ? jd_kept(t.obj, jd_k_plist) : js_undef();
    for (u32 i = 0; out && all.t == JS_OBJ && i < all.obj->len; i++) {
        jobj *e = all.obj->items[i].obj;
        jval nm = jd_pe(e, PE_NAME), ty = jd_pe(e, PE_TYPE);
        if (name && !(nm.t == JS_STR && js_str_eq(nm.str, name))) continue;
        if (type && !(ty.t == JS_STR && js_str_eq(ty.str, type))) continue;
        js_arr_push(J, out, all.obj->items[i]);
    }
    return js_from_obj(out);
}

static jval nat_plist_all(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_plist_entries(J, t, 0, 0); }
static jval nat_plist_by_type(jctx *J, jval t, jval *a, int n) { return jd_plist_entries(J, t, 0, jd_arg_str(J, a, n, 0)); }
static jval nat_plist_by_name(jctx *J, jval t, jval *a, int n) {
    return jd_plist_entries(J, t, jd_arg_str(J, a, n, 0), n > 1 && a[1].t != JS_UNDEF ? jd_arg_str(J, a, n, 1) : 0);
}

static void jd_po_deliver(jval arg) {
    (void)arg;
    jd_po_pending = 0;
    if (!jd_pobservers || jd_spent()) return;
    jctx *J = &jd_J;
    for (u32 i = 0; i < jd_pobservers->len; i++) {
        if (!js_is_obj(jd_pobservers->items[i])) continue;
        jobj *obs = jd_pobservers->items[i].obj;
        jval q = jd_kept(obs, jd_k_pqueue);
        if (q.t != JS_OBJ || !q.obj->len) continue;
        jd_keep(obs, jd_k_pqueue, js_undef());
        jobj *list = js_object_with(J, JO_PLAIN, jd_p_plist);
        jval cb = jd_kept(obs, jd_k_ocb);
        if (!list || !js_callable(cb)) continue;
        jd_keep(list, jd_k_plist, q);
        jval args[2] = { js_from_obj(list), js_from_obj(obs) };
        jd_call(cb, js_from_obj(obs), args, 2);
    }
}

static void jd_po_schedule(void) {
    if (!jd_po_pending && jd_later_native(jd_po_deliver, js_undef(), 1)) jd_po_pending = 1;
}

/* Every new mark and measure (jswin.h, jd_perf_entry), to the observers of
   its type. */
static void jd_perf_observed(jobj *entry) {
    if (!jd_pobservers || !entry) return;
    jval ty = jd_pe(entry, PE_TYPE);
    if (ty.t != JS_STR) return;
    int any = 0;
    for (u32 i = 0; i < jd_pobservers->len; i++)
        if (js_is_obj(jd_pobservers->items[i]) && jd_po_watches(jd_pobservers->items[i].obj, ty.str)) {
            jd_po_queue(jd_pobservers->items[i].obj, js_from_obj(entry));
            any = 1;
        }
    if (any) jd_po_schedule();
}

static jval nat_po_ctor(jctx *J, jval t, jval *a, int n) { return jd_observer_ctor(J, t, a, n, "PerformanceObserver"); }

static jval nat_po_observe(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t) || !js_callable(jd_kept(t.obj, jd_k_ocb))) return jd_illegal(J);
    jval opt = js_arg(a, n, 0);
    jval list = js_is_obj(opt) ? js_get(J, opt, js_str(J, "entryTypes")) : js_undef();
    jval one = js_is_obj(opt) ? js_get(J, opt, js_str(J, "type")) : js_undef();
    if (J->sig != JS_OK) return js_undef();
    if ((list.t == JS_UNDEF) == (one.t == JS_UNDEF))
        return js_throw(J, JS_ERR_TYPE, "observe needs entryTypes or type, and not both", J->error_line);
    jval types = jd_kept(t.obj, jd_k_ptypes);
    if (types.t != JS_OBJ || list.t != JS_UNDEF) {
        jobj *arr = js_array(J);
        if (!arr) return js_undef();
        types = js_from_obj(arr);
        jd_keep(t.obj, jd_k_ptypes, types);
    }
    int buffered = one.t != JS_UNDEF && js_to_bool(js_get(J, opt, js_str(J, "buffered")));
    jargs A;
    js_args_init(&A);
    if (list.t != JS_UNDEF) js_iter_collect(J, list, &A);
    else js_args_push(J, &A, one);
    for (int i = 0; i < A.n; i++) {
        jstr *s = js_to_str(J, A.v[i]);
        if (!s || !jd_perf_type_known(s) || jd_po_watches(t.obj, s)) continue;
        js_arr_push(J, types.obj, js_from_str(s));
        if (buffered && jd_perf_entries) {
            for (u32 k = 0; k < jd_perf_entries->len; k++) {
                jval ty = jd_pe(jd_perf_entries->items[k].obj, PE_TYPE);
                if (ty.t == JS_STR && js_str_eq(ty.str, s)) jd_po_queue(t.obj, jd_perf_entries->items[k]);
            }
            jd_po_schedule();
        }
    }
    js_args_free(&A);
    if (!jd_pobservers) jd_pobservers = js_array(J);
    if (jd_pobservers) {
        int in = 0;
        for (u32 i = 0; i < jd_pobservers->len; i++)
            if (js_is_obj(jd_pobservers->items[i]) && jd_pobservers->items[i].obj == t.obj) in = 1;
        if (!in) js_arr_push(J, jd_pobservers, t);
    }
    return js_undef();
}

static jval nat_po_disconnect(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (!js_is_obj(t) || !jd_pobservers) return js_undef();
    for (u32 i = 0; i < jd_pobservers->len; i++)
        if (js_is_obj(jd_pobservers->items[i]) && jd_pobservers->items[i].obj == t.obj) jd_pobservers->items[i] = js_undef();
    jd_keep(t.obj, jd_k_pqueue, js_undef());
    jd_keep(t.obj, jd_k_ptypes, js_undef());
    return js_undef();
}

static jval nat_po_take(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return jd_illegal(J);
    jval q = jd_kept(t.obj, jd_k_pqueue);
    jd_keep(t.obj, jd_k_pqueue, js_undef());
    return q.t == JS_OBJ ? q : js_from_obj(js_array(J));
}

static jval nat_po_supported(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jobj *arr = js_array(J);
    if (arr) {
        js_arr_push(J, arr, jd_str("mark"));
        js_arr_push(J, arr, jd_str("measure"));
        js_arr_push(J, arr, jd_str("navigation"));
    }
    return js_from_obj(arr);
}

static void jd_setup_perf_observer(jctx *J) {
    jd_k_ptypes = js_sym_new(J, "types", 5);
    jd_k_pqueue = js_sym_new(J, "queue", 5);
    jd_k_plist = js_sym_new(J, "entries", 7);
    jd_pobservers = 0;
    jd_po_pending = 0;
    jd_p_pobs = jd_interface(J, "PerformanceObserver", 0, nat_po_ctor, 1);
    jd_method(J, jd_p_pobs, "observe", nat_po_observe, 1);
    jd_method(J, jd_p_pobs, "disconnect", nat_po_disconnect, 0);
    jd_method(J, jd_p_pobs, "takeRecords", nat_po_take, 0);
    jobj *c = jd_ctor_of(jd_p_pobs);
    jobj *get = js_native(J, "supportedEntryTypes", nat_po_supported);
    if (c && get) js_define_accessor(J, c, js_str(J, "supportedEntryTypes"), js_from_obj(get), js_undef(), JP_CONF);
    jd_p_plist = jd_interface(J, "PerformanceObserverEntryList", 0, 0, 0);
    jd_method(J, jd_p_plist, "getEntries", nat_plist_all, 0);
    jd_method(J, jd_p_plist, "getEntriesByType", nat_plist_by_type, 1);
    jd_method(J, jd_p_plist, "getEntriesByName", nat_plist_by_name, 1);
}
