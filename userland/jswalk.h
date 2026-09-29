#pragma once
/* Walking the tree the standard's way: NodeFilter, TreeWalker and
 * NodeIterator, from document.createTreeWalker and createNodeIterator.
 *
 * Included from jsdom.h. The walks follow the DOM standard's steps, over
 * the tree as a script sees it: the page's document above its document
 * element, and the root the parser keeps every page under left out
 * (jd_top), a template's markup not among its children (it is a fragment
 * of its own), and a document of its own (jd_new_document) walked like any
 * node. Yahoo's advertising loader stopped on NodeFilter. */

#define JW_DOC (-2)                /* the page's document, which is no node */

static jstr *jd_k_wroot, *jd_k_wcur, *jd_k_wshow, *jd_k_wfilter, *jd_k_wbefore;
static jobj *jd_p_walker, *jd_p_iter;

static int jw_index(jval v) {
    if (jd_is_doc(v)) return JW_DOC;
    return jd_node_of(v);
}

static jval jw_value(jctx *J, int x) {
    if (x == JW_DOC) return jd_document_obj ? js_from_obj(jd_document_obj) : js_null();
    return x >= 0 ? jd_el_value(J, x) : js_null();
}

static int jw_type(int x) { return x == JW_DOC ? JN_DOCUMENT : jd_kind(x); }

static int jw_parent(int x) {
    if (x < 0) return -1;
    if (jd_is_top(x)) return JW_DOC;
    return jd_doc->nodes[x].parent;
}

static int jw_first(int x) {
    if (x == JW_DOC) return jd_top();
    if (x < 0 || (jd_doc->nodes[x].kind == DN_ELEMENT && jd_is_template(x))) return -1;
    return jd_doc->nodes[x].first;
}

static int jw_last(int x) {
    if (x == JW_DOC) return jd_top();
    if (x < 0 || (jd_doc->nodes[x].kind == DN_ELEMENT && jd_is_template(x))) return -1;
    return jd_doc->nodes[x].last;
}

static int jw_next(int x) {
    if (x < 0 || jd_is_top(x)) return -1;
    return jd_doc->nodes[x].next;
}

static int jw_prev(int x) {
    if (x < 0 || jd_is_top(x)) return -1;
    return jd_doc->nodes[x].prev;
}

/* One node through whatToShow and the page's filter: FILTER_ACCEPT (1),
   FILTER_REJECT (2) or FILTER_SKIP (3); 0 when the filter threw, which the
   walk then passes on as it is. */
static int jw_filter(jctx *J, jobj *w, int x) {
    double show = js_to_num(J, jd_kept(w, jd_k_wshow));
    u32 bits = (u32)(show < 0 ? 0 : show > 4294967295.0 ? 4294967295.0 : show);
    int type = jw_type(x);
    if (type < 1 || type > 32 || !(bits & (1u << (type - 1)))) return 3;
    jval f = jd_kept(w, jd_k_wfilter);
    if (f.t == JS_NULL || f.t == JS_UNDEF) return 1;
    jval arg = jw_value(J, x), got;
    if (js_callable(f)) {
        got = js_call(J, f, js_null(), &arg, 1);
    } else {
        jval m = js_get(J, f, js_str(J, "acceptNode"));
        if (J->sig != JS_OK) return 0;
        if (!js_callable(m)) {
            js_throw(J, JS_ERR_TYPE, "a NodeFilter needs acceptNode", J->error_line);
            return 0;
        }
        got = js_call(J, m, f, &arg, 1);
    }
    if (J->sig != JS_OK) return 0;
    int r = (int)js_to_num(J, got);
    return r == 1 || r == 2 ? r : 3;
}

static int jw_current(jobj *w) { return jw_index(jd_kept(w, jd_k_wcur)); }
static int jw_root(jobj *w) { return jw_index(jd_kept(w, jd_k_wroot)); }

static jval jw_accept(jctx *J, jobj *w, int x) {
    jval v = jw_value(J, x);
    jd_keep(w, jd_k_wcur, v);
    return v;
}

/* One of ours, and of the kind asked for: only an iterator keeps which side
   of its node it is on. */
static jobj *jw_this(jctx *J, jval t, jobj *proto) {
    int iter = proto == jd_p_iter;
    if (!js_is_obj(t) || jd_kept(t.obj, jd_k_wroot).t != JS_OBJ
        || (jd_kept(t.obj, jd_k_wbefore).t == JS_BOOL) != iter) {
        jd_illegal(J);
        return 0;
    }
    return t.obj;
}

/* --- TreeWalker ------------------------------------------------------------------------------ */

static jval nat_tw_parent(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *w = jw_this(J, t, jd_p_walker);
    if (!w) return js_undef();
    int root = jw_root(w), x = jw_current(w);
    while (x != -1 && x != root) {
        x = jw_parent(x);
        if (x == -1) break;
        int r = jw_filter(J, w, x);
        if (!r) return js_undef();
        if (r == 1) return jw_accept(J, w, x);
    }
    return js_null();
}

/* firstChild and lastChild, as the standard's "traverse children". */
static jval jw_children(jctx *J, jval t, int first) {
    jobj *w = jw_this(J, t, jd_p_walker);
    if (!w) return js_undef();
    int root = jw_root(w), cur = jw_current(w);
    int x = first ? jw_first(cur) : jw_last(cur);
    while (x != -1) {
        int r = jw_filter(J, w, x);
        if (!r) return js_undef();
        if (r == 1) return jw_accept(J, w, x);
        if (r == 3) {
            int c = first ? jw_first(x) : jw_last(x);
            if (c != -1) { x = c; continue; }
        }
        while (x != -1) {
            int s = first ? jw_next(x) : jw_prev(x);
            if (s != -1) { x = s; break; }
            int p = jw_parent(x);
            if (p == -1 || p == root || p == cur) return js_null();
            x = p;
        }
    }
    return js_null();
}

static jval nat_tw_first(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jw_children(J, t, 1); }
static jval nat_tw_last(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jw_children(J, t, 0); }

/* nextSibling and previousSibling, as the standard's "traverse siblings". */
static jval jw_siblings(jctx *J, jval t, int next) {
    jobj *w = jw_this(J, t, jd_p_walker);
    if (!w) return js_undef();
    int root = jw_root(w), x = jw_current(w);
    if (x == root) return js_null();
    for (;;) {
        int s = next ? jw_next(x) : jw_prev(x);
        while (s != -1) {
            x = s;
            int r = jw_filter(J, w, x);
            if (!r) return js_undef();
            if (r == 1) return jw_accept(J, w, x);
            s = next ? jw_first(x) : jw_last(x);
            if (r == 2 || s == -1) s = next ? jw_next(x) : jw_prev(x);
        }
        x = jw_parent(x);
        if (x == -1 || x == root) return js_null();
        int r = jw_filter(J, w, x);
        if (!r) return js_undef();
        if (r == 1) return js_null();
    }
}

static jval nat_tw_next_sibling(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jw_siblings(J, t, 1); }
static jval nat_tw_prev_sibling(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jw_siblings(J, t, 0); }

static jval nat_tw_prev_node(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *w = jw_this(J, t, jd_p_walker);
    if (!w) return js_undef();
    int root = jw_root(w), x = jw_current(w);
    while (x != -1 && x != root) {
        int s = jw_prev(x);
        while (s != -1) {
            x = s;
            int r = jw_filter(J, w, x);
            if (!r) return js_undef();
            while (r != 2 && jw_last(x) != -1) {
                x = jw_last(x);
                r = jw_filter(J, w, x);
                if (!r) return js_undef();
            }
            if (r == 1) return jw_accept(J, w, x);
            s = jw_prev(x);
        }
        if (x == root || jw_parent(x) == -1) return js_null();
        x = jw_parent(x);
        int r = jw_filter(J, w, x);
        if (!r) return js_undef();
        if (r == 1) return jw_accept(J, w, x);
    }
    return js_null();
}

static jval nat_tw_next_node(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *w = jw_this(J, t, jd_p_walker);
    if (!w) return js_undef();
    int root = jw_root(w), x = jw_current(w), r = 1;
    if (x == -1) return js_null();
    for (;;) {
        while (r != 2 && jw_first(x) != -1) {
            x = jw_first(x);
            r = jw_filter(J, w, x);
            if (!r) return js_undef();
            if (r == 1) return jw_accept(J, w, x);
        }
        int s = -1;
        for (int tmp = x; tmp != -1; tmp = jw_parent(tmp)) {
            if (tmp == root) return js_null();
            s = jw_next(tmp);
            if (s != -1) break;
        }
        if (s == -1) return js_null();
        x = s;
        r = jw_filter(J, w, x);
        if (!r) return js_undef();
        if (r == 1) return jw_accept(J, w, x);
    }
}

static jval nat_tw_get_current(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *w = jw_this(J, t, jd_p_walker);
    return w ? jd_kept(w, jd_k_wcur) : js_undef();
}

static jval nat_tw_set_current(jctx *J, jval t, jval *a, int n) {
    jobj *w = jw_this(J, t, jd_p_walker);
    if (!w) return js_undef();
    jval v = js_arg(a, n, 0);
    if (jw_index(v) == -1) return js_throw(J, JS_ERR_TYPE, "currentNode must be a node", J->error_line);
    jd_keep(w, jd_k_wcur, v);
    return js_undef();
}

/* --- NodeIterator ------------------------------------------------------------------------------ */

/* The node after x in tree order, inside root; -1 past its end. */
static int jw_following(int x, int root) {
    int c = jw_first(x);
    if (c != -1) return c;
    for (int tmp = x; tmp != -1 && tmp != root; tmp = jw_parent(tmp)) {
        int s = jw_next(tmp);
        if (s != -1) return s;
    }
    return -1;
}

/* The node before x in tree order, inside root; -1 before root. */
static int jw_preceding(int x, int root) {
    if (x == root) return -1;
    int s = jw_prev(x);
    if (s == -1) return jw_parent(x);
    while (jw_last(s) != -1) s = jw_last(s);
    return s;
}

static jval jw_iterate(jctx *J, jval t, int next) {
    jobj *w = jw_this(J, t, jd_p_iter);
    if (!w) return js_undef();
    int root = jw_root(w), x = jw_current(w);
    int before = js_to_bool(jd_kept(w, jd_k_wbefore));
    if (x == -1) return js_null();
    for (;;) {
        if (next) {
            if (!before) { x = jw_following(x, root); if (x == -1) return js_null(); }
            else before = 0;
        } else {
            if (before) { x = jw_preceding(x, root); if (x == -1) return js_null(); }
            else before = 1;
        }
        int r = jw_filter(J, w, x);
        if (!r) return js_undef();
        if (r == 1) break;
    }
    jd_keep(w, jd_k_wbefore, js_bool(before));
    return jw_accept(J, w, x);
}

static jval nat_ni_next(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jw_iterate(J, t, 1); }
static jval nat_ni_prev(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jw_iterate(J, t, 0); }

static jval nat_ni_reference(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *w = jw_this(J, t, jd_p_iter);
    return w ? jd_kept(w, jd_k_wcur) : js_undef();
}

static jval nat_ni_before(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *w = jw_this(J, t, jd_p_iter);
    return w ? js_bool(js_to_bool(jd_kept(w, jd_k_wbefore))) : js_undef();
}

/* --- both ---------------------------------------------------------------------------------------- */

static jval nat_w_root(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_is_obj(t) ? jd_kept(t.obj, jd_k_wroot) : js_undef();
}

static jval nat_w_show(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_is_obj(t) ? jd_kept(t.obj, jd_k_wshow) : js_undef();
}

static jval nat_w_filter(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (!js_is_obj(t)) return js_undef();
    jval f = jd_kept(t.obj, jd_k_wfilter);
    return f.t == JS_UNDEF ? js_null() : f;
}

static jval jw_make(jctx *J, jval *a, int n, jobj *proto, int iterator) {
    jval root = js_arg(a, n, 0);
    if (jw_index(root) == -1) return js_throw(J, JS_ERR_TYPE, "a walk needs a node to start from", J->error_line);
    jval show = js_arg(a, n, 1);
    double bits = show.t == JS_UNDEF ? 4294967295.0 : js_to_num(J, show);
    if (bits != bits) bits = 0;
    /* whatToShow is an unsigned long: -1 is every bit. */
    if (bits < 0) bits += 4294967296.0;
    jobj *w = js_object_with(J, JO_PLAIN, proto);
    if (!w) return js_null();
    jval f = js_arg(a, n, 2);
    jd_keep(w, jd_k_wroot, root);
    jd_keep(w, jd_k_wcur, root);
    jd_keep(w, jd_k_wshow, js_num(bits));
    jd_keep(w, jd_k_wfilter, f.t == JS_UNDEF ? js_null() : f);
    if (iterator) jd_keep(w, jd_k_wbefore, js_bool(1));
    return js_from_obj(w);
}

static jval nat_doc_tree_walker(jctx *J, jval t, jval *a, int n) { (void)t; return jw_make(J, a, n, jd_p_walker, 0); }
static jval nat_doc_node_iterator(jctx *J, jval t, jval *a, int n) { (void)t; return jw_make(J, a, n, jd_p_iter, 1); }
static jval nat_nothing_walk(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_undef(); }

static void jd_setup_walks(jctx *J) {
    jd_k_wroot = js_sym_new(J, "root", 4);
    jd_k_wcur = js_sym_new(J, "current", 7);
    jd_k_wshow = js_sym_new(J, "whatToShow", 10);
    jd_k_wfilter = js_sym_new(J, "filter", 6);
    jd_k_wbefore = js_sym_new(J, "before", 6);

    jobj *nf = jd_interface(J, "NodeFilter", 0, 0, 0);
    static const struct { const char *name; double v; } K[] = {
        { "FILTER_ACCEPT", 1 }, { "FILTER_REJECT", 2 }, { "FILTER_SKIP", 3 },
        { "SHOW_ALL", 4294967295.0 }, { "SHOW_ELEMENT", 0x1 }, { "SHOW_ATTRIBUTE", 0x2 },
        { "SHOW_TEXT", 0x4 }, { "SHOW_CDATA_SECTION", 0x8 }, { "SHOW_ENTITY_REFERENCE", 0x10 },
        { "SHOW_ENTITY", 0x20 }, { "SHOW_PROCESSING_INSTRUCTION", 0x40 }, { "SHOW_COMMENT", 0x80 },
        { "SHOW_DOCUMENT", 0x100 }, { "SHOW_DOCUMENT_TYPE", 0x200 }, { "SHOW_DOCUMENT_FRAGMENT", 0x400 },
        { "SHOW_NOTATION", 0x800 }, { 0, 0 }
    };
    jobj *nfc = jd_ctor_of(nf);
    for (int i = 0; K[i].name; i++) {
        if (nfc) js_define(J, nfc, js_str(J, K[i].name), js_num(K[i].v), JP_ENUM);
        js_define(J, nf, js_str(J, K[i].name), js_num(K[i].v), JP_ENUM);
    }

    jd_p_walker = jd_interface(J, "TreeWalker", 0, 0, 0);
    jd_accessor(J, jd_p_walker, "root", nat_w_root, 0);
    jd_accessor(J, jd_p_walker, "whatToShow", nat_w_show, 0);
    jd_accessor(J, jd_p_walker, "filter", nat_w_filter, 0);
    jd_accessor(J, jd_p_walker, "currentNode", nat_tw_get_current, nat_tw_set_current);
    jd_method(J, jd_p_walker, "parentNode", nat_tw_parent, 0);
    jd_method(J, jd_p_walker, "firstChild", nat_tw_first, 0);
    jd_method(J, jd_p_walker, "lastChild", nat_tw_last, 0);
    jd_method(J, jd_p_walker, "nextSibling", nat_tw_next_sibling, 0);
    jd_method(J, jd_p_walker, "previousSibling", nat_tw_prev_sibling, 0);
    jd_method(J, jd_p_walker, "nextNode", nat_tw_next_node, 0);
    jd_method(J, jd_p_walker, "previousNode", nat_tw_prev_node, 0);

    jd_p_iter = jd_interface(J, "NodeIterator", 0, 0, 0);
    jd_accessor(J, jd_p_iter, "root", nat_w_root, 0);
    jd_accessor(J, jd_p_iter, "whatToShow", nat_w_show, 0);
    jd_accessor(J, jd_p_iter, "filter", nat_w_filter, 0);
    jd_accessor(J, jd_p_iter, "referenceNode", nat_ni_reference, 0);
    jd_accessor(J, jd_p_iter, "pointerBeforeReferenceNode", nat_ni_before, 0);
    jd_method(J, jd_p_iter, "nextNode", nat_ni_next, 0);
    jd_method(J, jd_p_iter, "previousNode", nat_ni_prev, 0);
    jd_method(J, jd_p_iter, "detach", nat_nothing_walk, 0);

    jd_method(J, jd_p[JI_DOCUMENT], "createTreeWalker", nat_doc_tree_walker, 1);
    jd_method(J, jd_p[JI_DOCUMENT], "createNodeIterator", nat_doc_node_iterator, 1);
}
