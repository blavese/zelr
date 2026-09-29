/* Collections, promises, generators' prototypes and dates.
 *
 * Included from jslib.h.
 */
#pragma once

/* --- Map and Set -------------------------------------------------------------
 *
 * A table of entries in the order they were added, with a hash over them.
 * Deleting marks an entry gone rather than moving the ones after it, so an
 * iterator part way through -- which is only a position in the list -- sees
 * what the standard says it sees: what was added since, and not what was
 * deleted. Only clear() empties the list. Any value is a key, compared as
 * SameValueZero: an object by what it is, NaN as itself. The weak kinds are
 * the same table and hold their keys strongly: with no collector, nothing
 * is ever let go of anyway. */

static u32 jm_hash(jval k) {
    switch (k.t) {
        case JS_STR: case JS_SYM: return k.str ? k.str->hash : 0;
        case JS_NUM: {
            double d = k.num;
            if (d == 0) d = 0;                      /* -0 is 0 */
            if (d != d) return 0x7FF80000u;
            union { double d; u64 u; } x;
            x.d = d;
            return (u32)(x.u ^ (x.u >> 32)) * 2654435761u;
        }
        case JS_BOOL: return k.b ? 3 : 5;
        case JS_OBJ: {
            u64 p = (u64)k.obj;
            return (u32)((p >> 4) ^ (p >> 20)) * 2654435761u;
        }
        default: return (u32)k.t * 7919u;
    }
}

static jmap *jm_new(jctx *J) {
    jmap *m = (jmap *)js_alloc(J, (u32)sizeof(jmap));
    return m;
}

static void jm_rehash(jctx *J, jmap *m, u32 want) {
    int *h = (int *)js_alloc(J, want * (u32)sizeof(int));
    if (!h) return;
    for (u32 i = 0; i < want; i++) h[i] = -1;
    for (u32 i = 0; i < m->n; i++) {
        if (m->e[i].gone) continue;
        u32 b = m->e[i].hash & (want - 1);
        m->e[i].next = h[b];
        h[b] = (int)i;
    }
    m->heads = h;
    m->nheads = want;
}

static int jm_find(jmap *m, jval k) {
    if (!m || !m->nheads) return -1;
    u32 h = jm_hash(k);
    for (int i = m->heads[h & (m->nheads - 1)]; i >= 0; i = m->e[i].next)
        if (!m->e[i].gone && m->e[i].hash == h && js_same_zero(m->e[i].k, k)) return i;
    return -1;
}

static void jm_set(jctx *J, jmap *m, jval k, jval v) {
    if (k.t == JS_NUM && k.num == 0) k.num = 0;
    int i = jm_find(m, k);
    if (i >= 0) { m->e[i].v = v; return; }
    if (m->n >= m->cap) {
        u32 cap = m->cap ? m->cap * 2 : 8;
        jmentry *e = (jmentry *)js_alloc(J, cap * (u32)sizeof(jmentry));
        if (!e) return;
        for (u32 q = 0; q < m->n; q++) e[q] = m->e[q];
        m->e = e;
        m->cap = cap;
    }
    if (m->n + 1 > m->nheads) jm_rehash(J, m, m->nheads ? m->nheads * 2 : 8);
    if (!m->nheads) return;
    jmentry *x = &m->e[m->n];
    x->k = k;
    x->v = v;
    x->hash = jm_hash(k);
    x->gone = 0;
    u32 b = x->hash & (m->nheads - 1);
    x->next = m->heads[b];
    m->heads[b] = (int)m->n;
    m->n++;
    m->live++;
}

static int jm_delete(jmap *m, jval k) {
    int i = jm_find(m, k);
    if (i < 0) return 0;
    m->e[i].gone = 1;
    m->e[i].k = js_undef();
    m->e[i].v = js_undef();
    m->live--;
    return 1;
}

static jmap *js_this_map(jctx *J, jval t, int kind, const char *what) {
    if (!js_is_obj(t) || t.obj->kind != kind || !t.obj->internal) {
        js_throw_named(J, JS_ERR_TYPE, what, 0, " called on the wrong kind of object");
        return 0;
    }
    return (jmap *)t.obj->internal;
}

/* new Map(entries), new Set(values) and the weak kinds: the fresh object
   that `new` made, turned into one, and filled from the iterable through
   the object's own set or add. */
static jval js_collection_make(jctx *J, jval t, jval *a, int n, int kind) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "a Map or Set is made with new", J->error_line);
    jobj *o = t.obj;
    o->kind = (u8)kind;
    o->internal = jm_new(J);
    if (!o->internal) return js_undef();
    jval src = js_arg(a, n, 0);
    if (src.t == JS_UNDEF || src.t == JS_NULL) return t;
    int is_map = kind == JO_MAP || kind == JO_WEAKMAP;
    jval adder = js_get(J, t, js_str(J, is_map ? "set" : "add"));
    if (J->sig != JS_OK) return js_undef();
    jiter it;
    if (!js_iter_open(J, src, &it)) return js_undef();
    for (;;) {
        jval e;
        int r = js_iter_step(J, &it, &e);
        if (r < 0) return js_undef();
        if (r == 0) break;
        jval args[2];
        int argc = 1;
        if (is_map) {
            if (!js_is_obj(e)) {
                js_iter_close(J, &it);
                return js_throw(J, JS_ERR_TYPE, "a Map's entries have to be [key, value]", J->error_line);
            }
            args[0] = js_get_index(J, e, 0);
            args[1] = js_get_index(J, e, 1);
            argc = 2;
        } else args[0] = e;
        js_call(J, adder, t, args, argc);
        if (J->sig != JS_OK) { js_iter_close(J, &it); return js_undef(); }
    }
    return t;
}

static jval nat_map_make(jctx *J, jval t, jval *a, int n) { return js_collection_make(J, t, a, n, JO_MAP); }
static jval nat_set_make(jctx *J, jval t, jval *a, int n) { return js_collection_make(J, t, a, n, JO_SET); }
static jval nat_weakmap_make(jctx *J, jval t, jval *a, int n) { return js_collection_make(J, t, a, n, JO_WEAKMAP); }
static jval nat_weakset_make(jctx *J, jval t, jval *a, int n) { return js_collection_make(J, t, a, n, JO_WEAKSET); }

static int js_weak_key_ok(jctx *J, jval k) {
    if (k.t == JS_OBJ || k.t == JS_SYM) return 1;
    js_throw(J, JS_ERR_TYPE, "a weak collection's key has to be an object", J->error_line);
    return 0;
}

static jval nat_map_get(jctx *J, jval t, jval *a, int n) {
    jmap *m = js_is_obj(t) && t.obj->kind == JO_WEAKMAP ? (jmap *)t.obj->internal
                                                          : js_this_map(J, t, JO_MAP, "Map.prototype.get");
    if (!m) return js_undef();
    int i = jm_find(m, js_arg(a, n, 0));
    return i >= 0 ? m->e[i].v : js_undef();
}

static jval nat_map_set(jctx *J, jval t, jval *a, int n) {
    int weak = js_is_obj(t) && t.obj->kind == JO_WEAKMAP;
    jmap *m = weak ? (jmap *)t.obj->internal : js_this_map(J, t, JO_MAP, "Map.prototype.set");
    if (!m) return js_undef();
    if (weak && !js_weak_key_ok(J, js_arg(a, n, 0))) return js_undef();
    jm_set(J, m, js_arg(a, n, 0), js_arg(a, n, 1));
    return t;
}

static jmap *js_any_map(jctx *J, jval t, const char *what) {
    if (js_is_obj(t) && (t.obj->kind == JO_MAP || t.obj->kind == JO_SET || t.obj->kind == JO_WEAKMAP
                         || t.obj->kind == JO_WEAKSET) && t.obj->internal)
        return (jmap *)t.obj->internal;
    js_throw_named(J, JS_ERR_TYPE, what, 0, " called on the wrong kind of object");
    return 0;
}

static jval nat_map_has(jctx *J, jval t, jval *a, int n) {
    jmap *m = js_any_map(J, t, "has");
    return m ? js_bool(jm_find(m, js_arg(a, n, 0)) >= 0) : js_undef();
}

static jval nat_map_delete(jctx *J, jval t, jval *a, int n) {
    jmap *m = js_any_map(J, t, "delete");
    return m ? js_bool(jm_delete(m, js_arg(a, n, 0))) : js_undef();
}

static jval nat_map_clear(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jmap *m = js_any_map(J, t, "clear");
    if (!m) return js_undef();
    /* Marked gone rather than emptied, so an iterator part way through
       ends rather than walking what is added next from where it was. */
    for (u32 i = 0; i < m->n; i++) { m->e[i].gone = 1; m->e[i].k = js_undef(); m->e[i].v = js_undef(); }
    m->live = 0;
    return js_undef();
}

static jval nat_map_size(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (js_is_obj(t) && (t.obj == J->p_map || t.obj == J->p_set)) return js_num(0);
    jmap *m = js_any_map(J, t, "size");
    return m ? js_num((double)m->live) : js_undef();
}

static jval nat_map_foreach(jctx *J, jval t, jval *a, int n) {
    jmap *m = js_any_map(J, t, "forEach");
    if (!m) return js_undef();
    jval fn = js_arg(a, n, 0), self = js_arg(a, n, 1);
    if (!js_callable(fn)) return js_throw(J, JS_ERR_TYPE, "forEach needs a function", J->error_line);
    int is_set = t.obj->kind == JO_SET;
    for (u32 i = 0; i < m->n && J->sig == JS_OK; i++) {
        if (m->e[i].gone) continue;
        jval args[3] = { is_set ? m->e[i].k : m->e[i].v, m->e[i].k, t };
        js_call(J, fn, self, args, 3);
        m = (jmap *)t.obj->internal;
    }
    return js_undef();
}

static jval nat_set_add(jctx *J, jval t, jval *a, int n) {
    int weak = js_is_obj(t) && t.obj->kind == JO_WEAKSET;
    jmap *m = weak ? (jmap *)t.obj->internal : js_this_map(J, t, JO_SET, "Set.prototype.add");
    if (!m) return js_undef();
    jval k = js_arg(a, n, 0);
    if (weak && !js_weak_key_ok(J, k)) return js_undef();
    if (jm_find(m, k) < 0) jm_set(J, m, k, k);
    return t;
}

static jval js_map_iter(jctx *J, jval t, int kind, jobj *proto) {
    jmap *m = js_any_map(J, t, "an iterator");
    if (!m) return js_undef();
    return js_make_iter(J, proto, t, kind);
}

static jval nat_map_entries(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_map_iter(J, t, IK_ENTRIES, J->p_map_iter); }
static jval nat_map_keys(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_map_iter(J, t, IK_KEYS, J->p_map_iter); }
static jval nat_map_values(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_map_iter(J, t, IK_VALUES, J->p_map_iter); }
static jval nat_set_values(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_map_iter(J, t, IK_VALUES, J->p_set_iter); }
static jval nat_set_entries(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_map_iter(J, t, IK_ENTRIES, J->p_set_iter); }

static jval nat_mapiter_next(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t) || t.obj->kind != JO_ITER)
        return js_throw(J, JS_ERR_TYPE, "next called on something that is not an iterator", J->error_line);
    jobj *it = t.obj;
    if (!js_is_obj(it->ival)) return js_iter_result(J, js_undef(), 1);
    jmap *m = (jmap *)it->ival.obj->internal;
    u32 i = (u32)(u64)it->internal;
    while (m && i < m->n && m->e[i].gone) i++;
    if (!m || i >= m->n) {
        it->ival = js_undef();
        return js_iter_result(J, js_undef(), 1);
    }
    it->internal = (void *)(u64)(i + 1);
    jmentry *e = &m->e[i];
    if (it->spare == IK_KEYS) return js_iter_result(J, e->k, 0);
    if (it->spare == IK_VALUES) return js_iter_result(J, e->v, 0);
    jobj *pair = js_array(J);
    js_arr_push(J, pair, e->k);
    js_arr_push(J, pair, e->v);
    return js_iter_result(J, js_from_obj(pair), 0);
}

static jval nat_weakref_make(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "a WeakRef is made with new", J->error_line);
    jval target = js_arg(a, n, 0);
    if (!js_weak_key_ok(J, target)) return js_undef();
    t.obj->kind = JO_ITER;
    t.obj->ival = target;
    return t;
}

static jval nat_weakref_deref(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_is_obj(t) ? t.obj->ival : js_undef();
}

static jval nat_finreg_make(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "made with new", J->error_line);
    return t;
}

static void js_setup_collections(jctx *J) {
    J->p_map = js_object_with(J, JO_PLAIN, J->p_object);
    J->p_set = js_object_with(J, JO_PLAIN, J->p_object);
    J->p_weakmap = js_object_with(J, JO_PLAIN, J->p_object);
    J->p_weakset = js_object_with(J, JO_PLAIN, J->p_object);
    jobj *mp = J->p_map, *sp = J->p_set;
    js_ctor(J, "Map", nat_map_make, 0, mp);
    js_method(J, mp, "get", nat_map_get, 1);
    js_method(J, mp, "set", nat_map_set, 2);
    js_method(J, mp, "has", nat_map_has, 1);
    js_method(J, mp, "delete", nat_map_delete, 1);
    js_method(J, mp, "clear", nat_map_clear, 0);
    js_method(J, mp, "forEach", nat_map_foreach, 1);
    js_method(J, mp, "keys", nat_map_keys, 0);
    js_method(J, mp, "values", nat_map_values, 0);
    jobj *me = js_method(J, mp, "entries", nat_map_entries, 0);
    if (me) js_put_prop_flags(J, mp, J->sym_iterator, js_from_obj(me), JP_WRITE | JP_CONF);
    js_getter(J, mp, "size", nat_map_size);
    js_tag(J, mp, "Map");

    js_ctor(J, "Set", nat_set_make, 0, sp);
    js_method(J, sp, "add", nat_set_add, 1);
    js_method(J, sp, "has", nat_map_has, 1);
    js_method(J, sp, "delete", nat_map_delete, 1);
    js_method(J, sp, "clear", nat_map_clear, 0);
    js_method(J, sp, "forEach", nat_map_foreach, 1);
    js_method(J, sp, "entries", nat_set_entries, 0);
    jobj *sv = js_method(J, sp, "values", nat_set_values, 0);
    if (sv) {
        js_put_prop_flags(J, sp, js_str(J, "keys"), js_from_obj(sv), JP_WRITE | JP_CONF);
        js_put_prop_flags(J, sp, J->sym_iterator, js_from_obj(sv), JP_WRITE | JP_CONF);
    }
    js_getter(J, sp, "size", nat_map_size);
    js_tag(J, sp, "Set");

    js_ctor(J, "WeakMap", nat_weakmap_make, 0, J->p_weakmap);
    js_method(J, J->p_weakmap, "get", nat_map_get, 1);
    js_method(J, J->p_weakmap, "set", nat_map_set, 2);
    js_method(J, J->p_weakmap, "has", nat_map_has, 1);
    js_method(J, J->p_weakmap, "delete", nat_map_delete, 1);
    js_tag(J, J->p_weakmap, "WeakMap");
    js_ctor(J, "WeakSet", nat_weakset_make, 0, J->p_weakset);
    js_method(J, J->p_weakset, "add", nat_set_add, 1);
    js_method(J, J->p_weakset, "has", nat_map_has, 1);
    js_method(J, J->p_weakset, "delete", nat_map_delete, 1);
    js_tag(J, J->p_weakset, "WeakSet");

    J->p_map_iter = js_object_with(J, JO_PLAIN, J->p_iterator);
    js_method(J, J->p_map_iter, "next", nat_mapiter_next, 0);
    js_tag(J, J->p_map_iter, "Map Iterator");
    J->p_set_iter = js_object_with(J, JO_PLAIN, J->p_iterator);
    js_method(J, J->p_set_iter, "next", nat_mapiter_next, 0);
    js_tag(J, J->p_set_iter, "Set Iterator");

    jobj *wp = js_object_with(J, JO_PLAIN, J->p_object);
    js_ctor(J, "WeakRef", nat_weakref_make, 1, wp);
    js_method(J, wp, "deref", nat_weakref_deref, 0);
    js_tag(J, wp, "WeakRef");
    jobj *fp = js_object_with(J, JO_PLAIN, J->p_object);
    js_ctor(J, "FinalizationRegistry", nat_finreg_make, 1, fp);
    js_method(J, fp, "register", nat_nothing, 2);
    js_method(J, fp, "unregister", nat_nothing, 1);
    js_tag(J, fp, "FinalizationRegistry");
}

/* --- promises ------------------------------------------------------------------
 *
 * A promise is a value that is not there yet, and the reactions waiting for
 * it. When it settles, each reaction becomes a job, and the jobs run after the
 * script that is running now has finished -- never inside it, which is the
 * one promise every page is written against: code after .then() runs before
 * the callback does.
 *
 * XMLHttpRequest was the only way a page here could ask for anything later,
 * because there was nothing a promise could be built on. A page written in
 * the last ten years is written with them, and with async functions, which
 * are promises underneath. */

enum { PR_PENDING = 0, PR_OK, PR_ERR };

typedef struct jreact {
    jval on_ok, on_err;
    jobj *derived;            /* the promise then() handed back, or 0 */
    jco *co;                  /* an await, which carries on instead */
    struct jreact *next;
} jreact;

typedef struct {
    int state;
    jval value;
    jreact *first, *last;
    int handled;
} jpromise;

static jobj *js_promise_new(jctx *J) {
    jobj *p = js_object_with(J, JO_PROMISE, J->p_promise);
    if (!p) return 0;
    p->internal = js_alloc(J, (u32)sizeof(jpromise));
    if (!p->internal) return 0;
    return p;
}

static void js_react_job(jctx *J, jreact *r, int ok, jval v) {
    if (r->co) js_enqueue(J, ok ? JOB_RESUME_OK : JOB_RESUME_ERR, js_undef(), v, 0, r->co);
    else js_enqueue(J, ok ? JOB_REACT_OK : JOB_REACT_ERR, ok ? r->on_ok : r->on_err, v, r->derived, 0);
}

/* A promise settled: fulfilled or rejected with v, once. */
static void js_promise_settle(jctx *J, jobj *p, int ok, jval v) {
    if (!p || p->kind != JO_PROMISE || !p->internal) return;
    jpromise *pr = (jpromise *)p->internal;
    if (pr->state != PR_PENDING) return;
    pr->state = ok ? PR_OK : PR_ERR;
    pr->value = v;
    for (jreact *r = pr->first; r; r = r->next) js_react_job(J, r, ok, v);
    pr->first = pr->last = 0;
}

/* A promise resolved with a value: a thenable is followed, in a job of its
   own; anything else fulfils it. */
static void js_promise_resolve_with(jctx *J, jobj *p, jval v) {
    if (!p || p->kind != JO_PROMISE) return;
    if (js_is_obj(v) && v.obj == p) {
        jobj *e = js_error_with(J, J->err_proto[JS_ERR_TYPE], js_str(J, "TypeError"),
                                js_str(J, "a promise cannot be resolved with itself"));
        js_promise_settle(J, p, 0, js_from_obj(e));
        return;
    }
    if (js_is_obj(v)) {
        jsignal before = J->sig;
        jval then = js_get(J, v, J->s_then);
        if (J->sig == JS_THROWN) {
            jval e = J->ret;
            J->sig = before;
            js_promise_settle(J, p, 0, e);
            return;
        }
        if (J->sig != JS_OK) return;
        if (js_callable(then)) {
            js_enqueue(J, JOB_THENABLE, v, then, p, 0);
            return;
        }
    }
    js_promise_settle(J, p, 1, v);
}

/* The resolving functions a promise's executor is handed. The pair shares
   one "already used" mark: whichever is called first decides, and calling
   either again does nothing. */
static jval nat_promise_resolver(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *self = J->callee;
    if (!self || (self->flags & JOF_LINKED)) return js_undef();
    self->flags |= JOF_LINKED;
    if (js_is_obj(self->data2)) self->data2.obj->flags |= JOF_LINKED;
    jobj *p = self->data.obj;
    if (self->spare == 1) js_promise_resolve_with(J, p, js_arg(a, n, 0));
    else js_promise_settle(J, p, 0, js_arg(a, n, 0));
    return js_undef();
}

static void js_resolvers(jctx *J, jobj *p, jval *res, jval *rej) {
    jobj *r1 = js_native_n(J, "resolve", nat_promise_resolver, 1);
    jobj *r2 = js_native_n(J, "reject", nat_promise_resolver, 1);
    if (!r1 || !r2) { *res = *rej = js_undef(); return; }
    r1->data = js_from_obj(p);
    r2->data = js_from_obj(p);
    r1->spare = 1;
    r2->spare = 0;
    r1->data2 = js_from_obj(r2);
    r2->data2 = js_from_obj(r1);
    r1->flags |= JOF_NOCTOR;
    r2->flags |= JOF_NOCTOR;
    *res = js_from_obj(r1);
    *rej = js_from_obj(r2);
}

static void js_promise_react(jctx *J, jobj *p, jreact *r) {
    jpromise *pr = (jpromise *)p->internal;
    pr->handled = 1;
    if (pr->state == PR_PENDING) {
        if (pr->last) pr->last->next = r;
        else pr->first = r;
        pr->last = r;
        return;
    }
    js_react_job(J, r, pr->state == PR_OK, pr->value);
}

/* p.then(ok, err) on a promise this engine made, from C. */
static jobj *js_promise_then(jctx *J, jobj *p, jval ok, jval err) {
    jobj *d = js_promise_new(J);
    jreact *r = (jreact *)js_alloc(J, (u32)sizeof(jreact));
    if (!d || !r) return 0;
    r->on_ok = ok;
    r->on_err = err;
    r->derived = d;
    js_promise_react(J, p, r);
    return d;
}

/* What await does with its value: a promise of this engine's is used as it
   is, and anything else is put in one. */
static jobj *js_promise_of(jctx *J, jval v) {
    if (js_is_obj(v) && v.obj->kind == JO_PROMISE && v.obj->proto == J->p_promise) return v.obj;
    jobj *p = js_promise_new(J);
    if (p) js_promise_resolve_with(J, p, v);
    return p;
}

static void js_promise_await(jctx *J, jobj *p, jco *co) {
    jreact *r = (jreact *)js_alloc(J, (u32)sizeof(jreact));
    if (!r) return;
    r->co = co;
    js_promise_react(J, p, r);
}

static void co_after(jctx *J, jco *co);
static void co_run(jctx *J, jco *co);

/* One job from the queue (jsrun.h, js_drain). */
static void js_run_job(jctx *J, jjob *job) {
    switch (job->kind) {
        case JOB_REACT_OK: case JOB_REACT_ERR: {
            int ok = job->kind == JOB_REACT_OK;
            jobj *d = job->o;
            if (!js_callable(job->a)) {
                /* No handler for this outcome: it passes through. */
                if (d) {
                    if (ok) js_promise_resolve_with(J, d, job->b);
                    else js_promise_settle(J, d, 0, job->b);
                }
                return;
            }
            jval r = js_call(J, job->a, js_undef(), &job->b, 1);
            if (J->sig == JS_THROWN) {
                jval e = J->ret;
                J->sig = JS_OK;
                if (d) js_promise_settle(J, d, 0, e);
                return;
            }
            if (J->sig == JS_OK && d) js_promise_resolve_with(J, d, r);
            return;
        }
        case JOB_THENABLE: {
            jobj *p = job->o;
            jval res, rej;
            js_resolvers(J, p, &res, &rej);
            jval args[2] = { res, rej };
            js_call(J, job->b, job->a, args, 2);
            if (J->sig == JS_THROWN) {
                jval e = J->ret;
                J->sig = JS_OK;
                if (!(res.obj->flags & JOF_LINKED)) {
                    res.obj->flags |= JOF_LINKED;
                    rej.obj->flags |= JOF_LINKED;
                    js_promise_settle(J, p, 0, e);
                }
            }
            return;
        }
        case JOB_CALL:
            js_call(J, job->a, js_undef(), 0, 0);
            return;
        case JOB_RESUME_OK: case JOB_RESUME_ERR: {
            jco *co = (jco *)job->co;
            if (!co || co->state != CO_SUSPENDED) return;
            co->mode = job->kind == JOB_RESUME_OK ? CO_NEXT : CO_THROW;
            co->value = job->b;
            co_run(J, co);
            co_after(J, co);
            return;
        }
        default:
            return;
    }
}

static jobj *js_this_promise(jctx *J, jval t) {
    if (js_is_obj(t) && t.obj->kind == JO_PROMISE && t.obj->internal) return t.obj;
    js_throw(J, JS_ERR_TYPE, "this is not a promise", J->error_line);
    return 0;
}

/* new Promise(executor): the fresh object made into a promise, and the
   executor called with the pair that settles it. */
static jval nat_promise_make(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "a Promise is made with new", J->error_line);
    jval ex = js_arg(a, n, 0);
    if (!js_callable(ex)) return js_throw(J, JS_ERR_TYPE, "a Promise needs a function to run", J->error_line);
    jobj *p = t.obj;
    p->kind = JO_PROMISE;
    p->internal = js_alloc(J, (u32)sizeof(jpromise));
    if (!p->internal) return js_undef();
    jval res, rej;
    js_resolvers(J, p, &res, &rej);
    jval args[2] = { res, rej };
    js_call(J, ex, js_undef(), args, 2);
    if (J->sig == JS_THROWN) {
        jval e = J->ret;
        J->sig = JS_OK;
        if (!(res.obj->flags & JOF_LINKED)) {
            res.obj->flags |= JOF_LINKED;
            rej.obj->flags |= JOF_LINKED;
            js_promise_settle(J, p, 0, e);
        }
    }
    return t;
}

static jval nat_promise_then(jctx *J, jval t, jval *a, int n) {
    jobj *p = js_this_promise(J, t);
    if (!p) return js_undef();
    jval ok = js_arg(a, n, 0), err = js_arg(a, n, 1);
    jobj *d = js_promise_then(J, p, js_callable(ok) ? ok : js_undef(), js_callable(err) ? err : js_undef());
    return js_from_obj(d);
}

static jval nat_promise_catch(jctx *J, jval t, jval *a, int n) {
    jval then = js_get(J, t, J->s_then);
    if (J->sig != JS_OK) return js_undef();
    jval args[2] = { js_undef(), js_arg(a, n, 0) };
    return js_call(J, then, t, args, 2);
}

/* finally: the callback, whose own promise is waited for, and then the
   value or the reason that was there before, unchanged. */
static jval nat_finally_value(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jobj *self = J->callee;
    if (self->spare) {
        J->sig = JS_THROWN;
        J->ret = self->data;
        js_note_thrown(J, self->data, J->error_line);
        return js_undef();
    }
    return self->data;
}

static jval nat_finally_step(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *self = J->callee;
    jval fn = self->data;
    int rejected = self->spare;
    jval v = js_arg(a, n, 0);
    jval r = js_call(J, fn, js_undef(), 0, 0);
    if (J->sig != JS_OK) return js_undef();
    jobj *p = js_promise_of(J, r);
    jobj *k = js_native(J, "", nat_finally_value);
    if (!p || !k) return js_undef();
    k->data = v;
    k->spare = (u16)rejected;
    return js_from_obj(js_promise_then(J, p, js_from_obj(k), js_undef()));
}

static jval nat_promise_finally(jctx *J, jval t, jval *a, int n) {
    jval fn = js_arg(a, n, 0);
    jval then = js_get(J, t, J->s_then);
    if (J->sig != JS_OK) return js_undef();
    if (!js_callable(fn)) {
        jval args[2] = { fn, fn };
        return js_call(J, then, t, args, 2);
    }
    jobj *ok = js_native(J, "", nat_finally_step);
    jobj *err = js_native(J, "", nat_finally_step);
    if (!ok || !err) return js_undef();
    ok->data = fn;
    err->data = fn;
    err->spare = 1;
    jval args[2] = { js_from_obj(ok), js_from_obj(err) };
    return js_call(J, then, t, args, 2);
}

static jval nat_promise_resolve(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return js_from_obj(js_promise_of(J, js_arg(a, n, 0)));
}

static jval nat_promise_reject(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *p = js_promise_new(J);
    if (p) js_promise_settle(J, p, 0, js_arg(a, n, 0));
    return js_from_obj(p);
}

static jval nat_promise_withresolvers(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jobj *p = js_promise_new(J);
    jobj *o = js_object(J, JO_PLAIN);
    if (!p || !o) return js_undef();
    jval res, rej;
    js_resolvers(J, p, &res, &rej);
    js_set(J, o, "promise", js_from_obj(p));
    js_set(J, o, "resolve", res);
    js_set(J, o, "reject", rej);
    return js_from_obj(o);
}

/* all, allSettled and any: a count of what is still out, the values in
   their places, and one promise for the lot. The state is kept on a
   plain-looking object only the element functions can reach. */
typedef struct { int remaining; jobj *values; jobj *promise; int kind; } jall;
enum { AK_ALL, AK_SETTLED, AK_ANY };

static jval nat_all_element(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *self = J->callee;
    if (self->flags & JOF_LINKED) return js_undef();
    self->flags |= JOF_LINKED;
    jall *st = (jall *)self->data.obj->internal;
    u32 idx = (u32)self->data2.num;
    int rejected = self->spare;
    jval v = js_arg(a, n, 0);
    if (st->kind == AK_SETTLED) {
        jobj *rec = js_object(J, JO_PLAIN);
        if (!rec) return js_undef();
        js_set(J, rec, "status", js_from_str(js_str(J, rejected ? "rejected" : "fulfilled")));
        js_set(J, rec, rejected ? "reason" : "value", v);
        v = js_from_obj(rec);
    }
    js_arr_set(J, st->values, idx, v);
    if (--st->remaining == 0) {
        if (st->kind == AK_ANY) {
            jobj *e = js_error_with(J, J->err_proto[JS_ERR_AGGREGATE], js_str(J, "AggregateError"),
                                    js_str(J, "every promise was rejected"));
            if (e) {
                jprop *p = js_put_prop(J, e, js_str(J, "errors"), js_from_obj(st->values));
                if (p) p->flags = JP_WRITE | JP_CONF;
            }
            js_promise_settle(J, st->promise, 0, js_from_obj(e));
        } else js_promise_resolve_with(J, st->promise, js_from_obj(st->values));
    }
    return js_undef();
}

static jval js_promise_combine(jctx *J, jval *a, int n, int kind) {
    jobj *result = js_promise_new(J);
    jobj *holder = js_object(J, JO_ITER);
    jall *st = (jall *)js_alloc(J, (u32)sizeof(jall));
    if (!result || !holder || !st) return js_undef();
    holder->internal = st;
    st->values = js_array(J);
    st->promise = result;
    st->kind = kind;
    st->remaining = 1;
    jval res_fn, rej_fn;
    js_resolvers(J, result, &res_fn, &rej_fn);
    jiter it;
    if (!js_iter_open(J, js_arg(a, n, 0), &it)) {
        jval e = J->ret;
        J->sig = JS_OK;
        js_promise_settle(J, result, 0, e);
        return js_from_obj(result);
    }
    for (u32 i = 0;; i++) {
        jval x;
        int r = js_iter_step(J, &it, &x);
        if (r < 0) {
            jval e = J->ret;
            J->sig = JS_OK;
            js_promise_settle(J, result, 0, e);
            return js_from_obj(result);
        }
        if (r == 0) break;
        jobj *p = js_promise_of(J, x);
        if (!p) break;
        js_arr_set(J, st->values, i, js_undef());
        st->remaining++;
        jobj *ok = js_native_n(J, "", nat_all_element, 1);
        jobj *err = js_native_n(J, "", nat_all_element, 1);
        if (!ok || !err) break;
        ok->data = err->data = js_from_obj(holder);
        ok->data2 = err->data2 = js_num(i);
        ok->spare = 0;
        err->spare = 1;
        jval on_ok = kind == AK_ANY ? res_fn : js_from_obj(ok);
        jval on_err = kind == AK_ALL ? rej_fn : js_from_obj(err);
        if (kind == AK_ANY) { err->spare = 1; on_err = js_from_obj(err); }
        js_promise_then(J, p, on_ok, on_err);
    }
    if (--st->remaining == 0) {
        if (kind == AK_ANY) {
            jobj *e = js_error_with(J, J->err_proto[JS_ERR_AGGREGATE], js_str(J, "AggregateError"),
                                    js_str(J, "every promise was rejected"));
            js_promise_settle(J, result, 0, js_from_obj(e));
        } else js_promise_resolve_with(J, result, js_from_obj(st->values));
    }
    return js_from_obj(result);
}

static jval nat_promise_all(jctx *J, jval t, jval *a, int n) { (void)t; return js_promise_combine(J, a, n, AK_ALL); }
static jval nat_promise_allsettled(jctx *J, jval t, jval *a, int n) { (void)t; return js_promise_combine(J, a, n, AK_SETTLED); }
static jval nat_promise_any(jctx *J, jval t, jval *a, int n) { (void)t; return js_promise_combine(J, a, n, AK_ANY); }

static jval nat_promise_race(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *result = js_promise_new(J);
    if (!result) return js_undef();
    jval res, rej;
    js_resolvers(J, result, &res, &rej);
    jiter it;
    if (!js_iter_open(J, js_arg(a, n, 0), &it)) {
        jval e = J->ret;
        J->sig = JS_OK;
        js_promise_settle(J, result, 0, e);
        return js_from_obj(result);
    }
    for (;;) {
        jval x;
        int r = js_iter_step(J, &it, &x);
        if (r <= 0) break;
        jobj *p = js_promise_of(J, x);
        if (p) js_promise_then(J, p, res, rej);
    }
    return js_from_obj(result);
}

static void js_setup_promises(jctx *J) {
    J->p_promise = js_object_with(J, JO_PLAIN, J->p_object);
    jobj *pp = J->p_promise;
    jobj *promise = js_ctor(J, "Promise", nat_promise_make, 1, pp);
    J->c_promise = promise;
    js_method(J, pp, "then", nat_promise_then, 2);
    js_method(J, pp, "catch", nat_promise_catch, 1);
    js_method(J, pp, "finally", nat_promise_finally, 1);
    js_tag(J, pp, "Promise");
    js_method(J, promise, "resolve", nat_promise_resolve, 1);
    js_method(J, promise, "reject", nat_promise_reject, 1);
    js_method(J, promise, "all", nat_promise_all, 1);
    js_method(J, promise, "allSettled", nat_promise_allsettled, 1);
    js_method(J, promise, "any", nat_promise_any, 1);
    js_method(J, promise, "race", nat_promise_race, 1);
    js_method(J, promise, "withResolvers", nat_promise_withresolvers, 0);
}

/* --- generators' prototypes --------------------------------------------------- */

static jobj *js_this_gen(jctx *J, jval t, int kind) {
    if (js_is_obj(t) && t.obj->kind == JO_GEN && t.obj->spare == kind) return t.obj;
    js_throw(J, JS_ERR_TYPE, "this is not a generator of that kind", J->error_line);
    return 0;
}

static jval nat_gen_next(jctx *J, jval t, jval *a, int n) {
    jobj *g = js_this_gen(J, t, CO_GEN);
    return g ? co_gen_resume(J, g, CO_NEXT, js_arg(a, n, 0)) : js_undef();
}
static jval nat_gen_return(jctx *J, jval t, jval *a, int n) {
    jobj *g = js_this_gen(J, t, CO_GEN);
    return g ? co_gen_resume(J, g, CO_RETURN, js_arg(a, n, 0)) : js_undef();
}
static jval nat_gen_throw(jctx *J, jval t, jval *a, int n) {
    jobj *g = js_this_gen(J, t, CO_GEN);
    return g ? co_gen_resume(J, g, CO_THROW, js_arg(a, n, 0)) : js_undef();
}
static jval nat_agen_next(jctx *J, jval t, jval *a, int n) {
    jobj *g = js_is_obj(t) && t.obj->kind == JO_GEN ? t.obj : 0;
    return co_agen_request(J, g ? g : js_object(J, JO_GEN), CO_NEXT, js_arg(a, n, 0));
}
static jval nat_agen_return(jctx *J, jval t, jval *a, int n) {
    jobj *g = js_is_obj(t) && t.obj->kind == JO_GEN ? t.obj : 0;
    return co_agen_request(J, g ? g : js_object(J, JO_GEN), CO_RETURN, js_arg(a, n, 0));
}
static jval nat_agen_throw(jctx *J, jval t, jval *a, int n) {
    jobj *g = js_is_obj(t) && t.obj->kind == JO_GEN ? t.obj : 0;
    return co_agen_request(J, g ? g : js_object(J, JO_GEN), CO_THROW, js_arg(a, n, 0));
}

static void js_setup_generators(jctx *J) {
    /* %GeneratorFunction.prototype% and its prototype object, which every
       generator's own prototype inherits from. */
    J->p_generator = js_object_with(J, JO_PLAIN, J->p_iterator);
    js_method(J, J->p_generator, "next", nat_gen_next, 1);
    js_method(J, J->p_generator, "return", nat_gen_return, 1);
    js_method(J, J->p_generator, "throw", nat_gen_throw, 1);
    js_tag(J, J->p_generator, "Generator");
    J->p_async_generator = js_object_with(J, JO_PLAIN, J->p_async_iterator);
    js_method(J, J->p_async_generator, "next", nat_agen_next, 1);
    js_method(J, J->p_async_generator, "return", nat_agen_return, 1);
    js_method(J, J->p_async_generator, "throw", nat_agen_throw, 1);
    js_tag(J, J->p_async_generator, "AsyncGenerator");

    J->p_gen_function = js_object_with(J, JO_PLAIN, J->p_function);
    js_put_prop_flags(J, J->p_gen_function, J->s_prototype, js_from_obj(J->p_generator), JP_CONF);
    js_tag(J, J->p_gen_function, "GeneratorFunction");
    J->p_async_function = js_object_with(J, JO_PLAIN, J->p_function);
    js_tag(J, J->p_async_function, "AsyncFunction");
    J->p_async_gen_function = js_object_with(J, JO_PLAIN, J->p_function);
    js_put_prop_flags(J, J->p_async_gen_function, J->s_prototype, js_from_obj(J->p_async_generator), JP_CONF);
    js_tag(J, J->p_async_gen_function, "AsyncGeneratorFunction");
}

/* --- Date ------------------------------------------------------------------------
 *
 * A moment is milliseconds since the start of 1970 in UTC, and there is no
 * other time zone here: the machine's clock has no idea where it is, so
 * local time is UTC and getTimezoneOffset() is 0. The clock itself is the
 * machine's (/sys/time), read once and carried forward by the tick counter;
 * where there is no clock -- the host build, which reaches no files -- it is
 * the start of 2026 plus however long the program has run, which is a clock
 * nobody set and not a claim about what time it is. */

static double js_days_from_civil(double y, double m, double d) {
    /* Howard Hinnant's arithmetic, for any year. */
    y -= m <= 2;
    double era = js_floor((y >= 0 ? y : y - 399) / 400);
    double yoe = y - era * 400;
    double mp = m > 2 ? m - 3 : m + 9;
    double doy = js_floor((153 * mp + 2) / 5) + d - 1;
    double doe = yoe * 365 + js_floor(yoe / 4) - js_floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}

static void js_civil_from_days(double z, int *yy, int *mm, int *dd) {
    z += 719468;
    double era = js_floor((z >= 0 ? z : z - 146096) / 146097);
    double doe = z - era * 146097;
    double yoe = js_floor((doe - js_floor(doe / 1460) + js_floor(doe / 36524) - js_floor(doe / 146096)) / 365);
    double y = yoe + era * 400;
    double doy = doe - (365 * yoe + js_floor(yoe / 4) - js_floor(yoe / 100));
    double mp = js_floor((5 * doy + 2) / 153);
    double d = doy - js_floor((153 * mp + 2) / 5) + 1;
    double m = mp < 10 ? mp + 3 : mp - 9;
    *yy = (int)(y + (m <= 2));
    *mm = (int)m;
    *dd = (int)d;
}

static double js_make_time(double y, double mon, double d, double h, double mi, double s, double ms) {
    if (!js_isfin(y) || !js_isfin(mon) || !js_isfin(d) || !js_isfin(h) || !js_isfin(mi)
        || !js_isfin(s) || !js_isfin(ms)) return js_nan();
    y = js_trunc(y); mon = js_trunc(mon);
    double ym = y + js_floor(mon / 12);
    double mn = mon - js_floor(mon / 12) * 12;
    double days = js_days_from_civil(ym, mn + 1, 1) + js_trunc(d) - 1;
    double t = days * 86400000.0 + js_trunc(h) * 3600000.0 + js_trunc(mi) * 60000.0
             + js_trunc(s) * 1000.0 + js_trunc(ms);
    if (js_fabs(t) > 8.64e15) return js_nan();
    return t;
}

static double js_now(jctx *J) {
    if (!J->clock_base) {
        double base = 1767225600000.0;             /* 2026-01-01, a clock nobody set */
        char buf[128];
        int n = slurp("/sys/time", buf, (int)sizeof(buf) - 1);
        if (n > 20) {
            buf[n] = 0;
            /* "now       YYYY-MM-DD HH:MM:SS" */
            for (int i = 0; i + 19 <= n; i++) {
                if (!(js_digit(buf[i]) && buf[i + 4] == '-' && buf[i + 7] == '-')) continue;
                int v[6] = { 0 }, lens[6] = { 4, 2, 2, 2, 2, 2 }, at = i, ok = 1;
                for (int k = 0; k < 6 && ok; k++) {
                    for (int q = 0; q < lens[k]; q++) {
                        if (!js_digit(buf[at])) { ok = 0; break; }
                        v[k] = v[k] * 10 + (buf[at++] - '0');
                    }
                    at++;
                }
                if (ok && v[0] > 1970) base = js_make_time(v[0], v[1] - 1, v[2], v[3], v[4], v[5], 0);
                break;
            }
        }
        J->clock_base = base;
        J->clock_tick0 = ticks();
    }
    return J->clock_base + (double)(ticks() - J->clock_tick0) * 10.0;
}

/* The parts of a moment. */
typedef struct { int y, mon, d, h, mi, s, ms, wd; } jdate;

static void js_date_parts(double t, jdate *p) {
    double days = js_floor(t / 86400000.0);
    double rem = t - days * 86400000.0;
    js_civil_from_days(days, &p->y, &p->mon, &p->d);
    p->mon -= 1;
    p->h = (int)(rem / 3600000.0);
    rem -= p->h * 3600000.0;
    p->mi = (int)(rem / 60000.0);
    rem -= p->mi * 60000.0;
    p->s = (int)(rem / 1000.0);
    p->ms = (int)(rem - p->s * 1000.0);
    double wd = js_floor(days + 4) - js_floor((days + 4) / 7) * 7;
    p->wd = (int)wd;
}

/* The date formats pages hand to Date: ISO 8601 (what toISOString writes,
   and what JSON carries), and the English ones mail and HTTP use. */
static int js_word_is(const char *s, const char *w) {
    for (int i = 0; w[i]; i++) if ((s[i] | 32) != w[i]) return 0;
    return 1;
}

static double js_date_parse(const char *s, u32 n) {
    u32 i = 0;
    while (i < n && s[i] == ' ') i++;
    /* ISO: [+-]YYYY[YY]-MM-DD[THH:MM[:SS[.sss]]][Z|+HH:MM] */
    if (i + 4 <= n && (js_digit(s[i]) || s[i] == '+' || s[i] == '-')) {
        u32 k = i;
        int sign = 1;
        if (s[k] == '+' || s[k] == '-') { sign = s[k] == '-' ? -1 : 1; k++; }
        int digits = 0;
        double y = 0;
        while (k < n && js_digit(s[k])) { y = y * 10 + (s[k++] - '0'); digits++; }
        if (digits >= 4 && (k >= n || s[k] == '-' || s[k] == 'T')) {
            y *= sign;
            double mon = 1, d = 1, h = 0, mi = 0, sec = 0, ms = 0, off = 0;
            int has_time = 0, zone = 0;
            if (k < n && s[k] == '-') {
                k++;
                mon = 0;
                for (int q = 0; q < 2 && k < n && js_digit(s[k]); q++) mon = mon * 10 + (s[k++] - '0');
                if (k < n && s[k] == '-') {
                    k++;
                    d = 0;
                    for (int q = 0; q < 2 && k < n && js_digit(s[k]); q++) d = d * 10 + (s[k++] - '0');
                }
            }
            if (k < n && (s[k] == 'T' || s[k] == 't' || s[k] == ' ')) {
                k++;
                has_time = 1;
                for (int q = 0; q < 2 && k < n && js_digit(s[k]); q++) h = h * 10 + (s[k++] - '0');
                if (k < n && s[k] == ':') k++;
                for (int q = 0; q < 2 && k < n && js_digit(s[k]); q++) mi = mi * 10 + (s[k++] - '0');
                if (k < n && s[k] == ':') {
                    k++;
                    for (int q = 0; q < 2 && k < n && js_digit(s[k]); q++) sec = sec * 10 + (s[k++] - '0');
                }
                if (k < n && (s[k] == '.' || s[k] == ',')) {
                    k++;
                    double scale = 100;
                    while (k < n && js_digit(s[k])) { ms += (s[k++] - '0') * scale; scale /= 10; }
                    ms = js_trunc(ms);
                }
            }
            if (k < n && (s[k] == 'Z' || s[k] == 'z')) { k++; zone = 1; }
            else if (k < n && (s[k] == '+' || s[k] == '-')) {
                int zs = s[k] == '-' ? -1 : 1;
                k++;
                double oh = 0, om = 0;
                for (int q = 0; q < 2 && k < n && js_digit(s[k]); q++) oh = oh * 10 + (s[k++] - '0');
                if (k < n && s[k] == ':') k++;
                for (int q = 0; q < 2 && k < n && js_digit(s[k]); q++) om = om * 10 + (s[k++] - '0');
                off = zs * (oh * 60 + om);
                zone = 1;
            }
            (void)has_time;
            (void)zone;
            if (k == n && mon >= 1 && mon <= 12 && d >= 1 && d <= 31 && h <= 24 && mi < 60 && sec < 60)
                return js_make_time(y, mon - 1, d, h, mi, sec, ms) - off * 60000.0;
        }
    }
    /* "Tue, 01 Jan 2024 10:00:00 GMT", "January 1, 2024 10:00", "1 Jan 2024". */
    static const char *const MON[12] = { "jan", "feb", "mar", "apr", "may", "jun",
                                         "jul", "aug", "sep", "oct", "nov", "dec" };
    int mon = -1;
    double nums[6];
    int nn = 0;
    double off = 0;
    int pm = 0, am = 0;
    for (u32 k = i; k < n;) {
        char c = s[k];
        if (js_digit(c)) {
            double v = 0;
            while (k < n && js_digit(s[k])) v = v * 10 + (s[k++] - '0');
            if (nn < 6) nums[nn++] = v;
            if (k < n && s[k] == ':') k++;
            continue;
        }
        if ((c == '+' || c == '-') && k + 4 < n && js_digit(s[k + 1]) && nn >= 3) {
            int zs = c == '-' ? -1 : 1;
            double hh = (s[k + 1] - '0') * 10 + (s[k + 2] - '0');
            u32 q = k + 3;
            if (q < n && s[q] == ':') q++;
            double mm = q + 1 < n ? (s[q] - '0') * 10 + (s[q + 1] - '0') : 0;
            off = zs * (hh * 60 + mm);
            k = q + 2;
            continue;
        }
        if ((c | 32) >= 'a' && (c | 32) <= 'z') {
            u32 start = k;
            while (k < n && (s[k] | 32) >= 'a' && (s[k] | 32) <= 'z') k++;
            if (k - start >= 3)
                for (int q = 0; q < 12; q++) if (js_word_is(s + start, MON[q])) mon = q;
            if (k - start == 2 && js_word_is(s + start, "pm")) pm = 1;
            if (k - start == 2 && js_word_is(s + start, "am")) am = 1;
            continue;
        }
        k++;
    }
    if (mon < 0 || nn < 2) return js_nan();
    double d, y;
    if (nums[0] > 31) { y = nums[0]; d = nums[1]; }
    else { d = nums[0]; y = nums[1]; }
    double h = nn > 2 ? nums[2] : 0, mi = nn > 3 ? nums[3] : 0, sec = nn > 4 ? nums[4] : 0;
    if (pm && h < 12) h += 12;
    if (am && h == 12) h = 0;
    if (y < 100) y += y < 50 ? 2000 : 1900;
    return js_make_time(y, mon, d, h, mi, sec, 0) - off * 60000.0;
}

static double js_this_time(jctx *J, jval t, int *ok) {
    if (js_is_obj(t) && t.obj->kind == JO_DATE) { *ok = 1; return t.obj->ival.num; }
    js_throw(J, JS_ERR_TYPE, "this is not a Date", J->error_line);
    *ok = 0;
    return 0;
}

static double js_time_clip(double t) {
    if (!js_isfin(t) || js_fabs(t) > 8.64e15) return js_nan();
    return js_trunc(t) + 0;
}

static jval nat_date_tostring(jctx *J, jval t, jval *a, int n);

static jval nat_date_make(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return nat_date_tostring(J, js_undef(), 0, 0);
    double tv;
    if (n == 0) tv = js_now(J);
    else if (n == 1) {
        jval v = a[0];
        if (js_is_obj(v) && v.obj->kind == JO_DATE) tv = v.obj->ival.num;
        else {
            v = js_to_primitive(J, v, 0);
            if (J->sig != JS_OK) return js_undef();
            tv = v.t == JS_STR ? js_date_parse(v.str->s, v.str->len) : js_to_num(J, v);
        }
    } else {
        double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
        for (int i = 0; i < n && i < 7; i++) f[i] = js_to_num(J, a[i]);
        if (f[0] == f[0] && f[0] >= 0 && f[0] <= 99 && js_trunc(f[0]) == f[0]) f[0] += 1900;
        tv = js_make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    }
    if (!js_is_obj(t)) return js_undef();
    t.obj->kind = JO_DATE;
    t.obj->ival = js_num(js_time_clip(tv));
    return t;
}

static jval nat_date_now(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    return js_num(js_floor(js_now(J)));
}

static jval nat_date_parse(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    return js_num(js_date_parse(s->s, s->len));
}

static jval nat_date_utc(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double f[7] = { js_nan(), 0, 1, 0, 0, 0, 0 };
    for (int i = 0; i < n && i < 7; i++) f[i] = js_to_num(J, a[i]);
    if (f[0] == f[0] && f[0] >= 0 && f[0] <= 99 && js_trunc(f[0]) == f[0]) f[0] += 1900;
    return js_num(js_time_clip(js_make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6])));
}

/* Every getter, by which part: 0 year, 1 month, 2 day of the month, 3 hours,
   4 minutes, 5 seconds, 6 milliseconds, 7 day of the week, 8 the time. */
static jval js_date_get(jctx *J, jval t, int which) {
    int ok;
    double tv = js_this_time(J, t, &ok);
    if (!ok) return js_undef();
    if (tv != tv) return js_num(js_nan());
    if (which == 8) return js_num(tv);
    jdate p;
    js_date_parts(tv, &p);
    int v[8] = { p.y, p.mon, p.d, p.h, p.mi, p.s, p.ms, p.wd };
    return js_num(v[which]);
}

#define JS_DATE_GET(nm, k) static jval nat_date_##nm(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_date_get(J, t, k); }
JS_DATE_GET(getfullyear, 0) JS_DATE_GET(getmonth, 1) JS_DATE_GET(getdate, 2)
JS_DATE_GET(gethours, 3) JS_DATE_GET(getminutes, 4) JS_DATE_GET(getseconds, 5)
JS_DATE_GET(getms, 6) JS_DATE_GET(getday, 7) JS_DATE_GET(gettime, 8)

static jval nat_date_getyear(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jval y = js_date_get(J, t, 0);
    return y.t == JS_NUM ? js_num(y.num - 1900) : y;
}

static jval nat_date_tzoffset(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int ok;
    double tv = js_this_time(J, t, &ok);
    if (!ok) return js_undef();
    return js_num(tv != tv ? js_nan() : 0);
}

/* The setters: which parts are given, from `first`, and the rest kept. */
static jval js_date_set(jctx *J, jval t, jval *a, int n, int first) {
    int ok;
    double tv = js_this_time(J, t, &ok);
    if (!ok) return js_undef();
    if (first == 8) {
        double v = js_time_clip(js_to_num(J, js_arg(a, n, 0)));
        t.obj->ival = js_num(v);
        return js_num(v);
    }
    double f[7];
    if (tv != tv) {
        if (first != 0) return js_num(js_nan());
        f[0] = 1970; f[1] = 0; f[2] = 1; f[3] = f[4] = f[5] = f[6] = 0;
    } else {
        jdate p;
        js_date_parts(tv, &p);
        f[0] = p.y; f[1] = p.mon; f[2] = p.d; f[3] = p.h; f[4] = p.mi; f[5] = p.s; f[6] = p.ms;
    }
    int most = first == 0 ? 3 : first == 1 ? 2 : first == 2 ? 1 : first == 3 ? 4 : first == 4 ? 3 : first == 5 ? 2 : 1;
    for (int i = 0; i < n && i < most; i++) f[first + i] = js_to_num(J, a[i]);
    double v = js_time_clip(js_make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]));
    t.obj->ival = js_num(v);
    return js_num(v);
}

#define JS_DATE_SET(nm, k) static jval nat_date_##nm(jctx *J, jval t, jval *a, int n) { return js_date_set(J, t, a, n, k); }
JS_DATE_SET(setfullyear, 0) JS_DATE_SET(setmonth, 1) JS_DATE_SET(setdate, 2)
JS_DATE_SET(sethours, 3) JS_DATE_SET(setminutes, 4) JS_DATE_SET(setseconds, 5)
JS_DATE_SET(setms, 6) JS_DATE_SET(settime, 8)

static const char *const JS_DAYS[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const JS_MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static int js_put_num(char *out, int v, int width) {
    char rev[12];
    int r = 0, w = 0;
    int neg = v < 0;
    if (neg) v = -v;
    do { rev[r++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (r < width) rev[r++] = '0';
    if (neg) out[w++] = '-';
    while (r) out[w++] = rev[--r];
    return w;
}

/* The text of a moment in one of the shapes: 0 toString, 1 toDateString,
   2 toTimeString, 3 toUTCString, 4 toISOString, 5 toLocaleDateString,
   6 toLocaleTimeString, 7 toLocaleString. */
static jval js_date_text(jctx *J, jval t, int shape) {
    int ok;
    double tv = js_this_time(J, t, &ok);
    if (!ok) return js_undef();
    if (tv != tv) {
        if (shape == 4) return js_throw(J, JS_ERR_RANGE, "an invalid date has no ISO text", J->error_line);
        return js_from_str(js_str(J, "Invalid Date"));
    }
    jdate p;
    js_date_parts(tv, &p);
    char out[80];
    int w = 0;
    #define S(str) do { for (const char *q_ = (str); *q_; q_++) out[w++] = *q_; } while (0)
    switch (shape) {
        case 4:
            if (p.y < 0 || p.y > 9999) { out[w++] = p.y < 0 ? '-' : '+'; w += js_put_num(out + w, p.y < 0 ? -p.y : p.y, 6); }
            else w += js_put_num(out + w, p.y, 4);
            out[w++] = '-'; w += js_put_num(out + w, p.mon + 1, 2);
            out[w++] = '-'; w += js_put_num(out + w, p.d, 2);
            out[w++] = 'T'; w += js_put_num(out + w, p.h, 2);
            out[w++] = ':'; w += js_put_num(out + w, p.mi, 2);
            out[w++] = ':'; w += js_put_num(out + w, p.s, 2);
            out[w++] = '.'; w += js_put_num(out + w, p.ms, 3);
            out[w++] = 'Z';
            break;
        case 3:
            S(JS_DAYS[p.wd]); S(", "); w += js_put_num(out + w, p.d, 2); out[w++] = ' ';
            S(JS_MONTHS[p.mon]); out[w++] = ' '; w += js_put_num(out + w, p.y, 4); out[w++] = ' ';
            w += js_put_num(out + w, p.h, 2); out[w++] = ':'; w += js_put_num(out + w, p.mi, 2);
            out[w++] = ':'; w += js_put_num(out + w, p.s, 2); S(" GMT");
            break;
        case 5: case 6: case 7:
            if (shape != 6) {
                w += js_put_num(out + w, p.mon + 1, 1); out[w++] = '/';
                w += js_put_num(out + w, p.d, 1); out[w++] = '/';
                w += js_put_num(out + w, p.y, 4);
            }
            if (shape == 7) S(", ");
            if (shape != 5) {
                int h12 = p.h % 12 ? p.h % 12 : 12;
                w += js_put_num(out + w, h12, 1); out[w++] = ':';
                w += js_put_num(out + w, p.mi, 2); out[w++] = ':';
                w += js_put_num(out + w, p.s, 2);
                S(p.h < 12 ? " AM" : " PM");
            }
            break;
        default:
            if (shape != 2) {
                S(JS_DAYS[p.wd]); out[w++] = ' '; S(JS_MONTHS[p.mon]); out[w++] = ' ';
                w += js_put_num(out + w, p.d, 2); out[w++] = ' ';
                w += js_put_num(out + w, p.y, 4);
            }
            if (shape == 0) out[w++] = ' ';
            if (shape != 1) {
                w += js_put_num(out + w, p.h, 2); out[w++] = ':';
                w += js_put_num(out + w, p.mi, 2); out[w++] = ':';
                w += js_put_num(out + w, p.s, 2);
                S(" GMT+0000 (Coordinated Universal Time)");
            }
            break;
    }
    #undef S
    return js_from_str(js_str_n(J, out, (u32)w));
}

static jval nat_date_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (t.t == JS_UNDEF) {
        jobj *d = js_object(J, JO_DATE);
        if (!d) return js_undef();
        d->ival = js_num(js_now(J));
        t = js_from_obj(d);
    }
    return js_date_text(J, t, 0);
}

#define JS_DATE_TEXT(nm, k) static jval nat_date_##nm(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_date_text(J, t, k); }
JS_DATE_TEXT(todatestring, 1) JS_DATE_TEXT(totimestring, 2) JS_DATE_TEXT(toutcstring, 3)
JS_DATE_TEXT(toisostring, 4) JS_DATE_TEXT(tolocaledate, 5) JS_DATE_TEXT(tolocaletime, 6)
JS_DATE_TEXT(tolocale, 7)

static jval nat_date_tojson(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jval tv = js_to_primitive(J, t, 1);
    if (J->sig != JS_OK) return js_undef();
    if (tv.t == JS_NUM && !js_isfin(tv.num)) return js_null();
    jval m = js_get_str(J, t, "toISOString");
    if (J->sig != JS_OK) return js_undef();
    return js_call(J, m, t, 0, 0);
}

static jval nat_date_toprim(jctx *J, jval t, jval *a, int n) {
    jval h = js_arg(a, n, 0);
    int string = !(h.t == JS_STR && js_str_is(h.str, "number"));
    if (!js_is_obj(t)) return js_throw(J, JS_ERR_TYPE, "Date's Symbol.toPrimitive needs an object", J->error_line);
    jstr *order[2] = { string ? J->s_toString : J->s_valueOf, string ? J->s_valueOf : J->s_toString };
    for (int i = 0; i < 2; i++) {
        jval m = js_get(J, t, order[i]);
        if (J->sig != JS_OK) return js_undef();
        if (!js_callable(m)) continue;
        jval r = js_call(J, m, t, 0, 0);
        if (J->sig != JS_OK) return js_undef();
        if (r.t != JS_OBJ) return r;
    }
    return js_throw(J, JS_ERR_TYPE, "this date cannot be made into a primitive", J->error_line);
}

static void js_setup_dates(jctx *J) {
    J->p_date = js_object_with(J, JO_PLAIN, J->p_object);
    jobj *dp = J->p_date;
    jobj *date = js_ctor(J, "Date", nat_date_make, 7, dp);
    js_method(J, date, "now", nat_date_now, 0);
    js_method(J, date, "parse", nat_date_parse, 1);
    js_method(J, date, "UTC", nat_date_utc, 7);
    struct { const char *n; jnative f; int k; } M[] = {
        { "getFullYear", nat_date_getfullyear, 0 }, { "getUTCFullYear", nat_date_getfullyear, 0 },
        { "getMonth", nat_date_getmonth, 0 }, { "getUTCMonth", nat_date_getmonth, 0 },
        { "getDate", nat_date_getdate, 0 }, { "getUTCDate", nat_date_getdate, 0 },
        { "getHours", nat_date_gethours, 0 }, { "getUTCHours", nat_date_gethours, 0 },
        { "getMinutes", nat_date_getminutes, 0 }, { "getUTCMinutes", nat_date_getminutes, 0 },
        { "getSeconds", nat_date_getseconds, 0 }, { "getUTCSeconds", nat_date_getseconds, 0 },
        { "getMilliseconds", nat_date_getms, 0 }, { "getUTCMilliseconds", nat_date_getms, 0 },
        { "getDay", nat_date_getday, 0 }, { "getUTCDay", nat_date_getday, 0 },
        { "getTime", nat_date_gettime, 0 }, { "valueOf", nat_date_gettime, 0 },
        { "getYear", nat_date_getyear, 0 }, { "getTimezoneOffset", nat_date_tzoffset, 0 },
        { "setFullYear", nat_date_setfullyear, 3 }, { "setUTCFullYear", nat_date_setfullyear, 3 },
        { "setMonth", nat_date_setmonth, 2 }, { "setUTCMonth", nat_date_setmonth, 2 },
        { "setDate", nat_date_setdate, 1 }, { "setUTCDate", nat_date_setdate, 1 },
        { "setHours", nat_date_sethours, 4 }, { "setUTCHours", nat_date_sethours, 4 },
        { "setMinutes", nat_date_setminutes, 3 }, { "setUTCMinutes", nat_date_setminutes, 3 },
        { "setSeconds", nat_date_setseconds, 2 }, { "setUTCSeconds", nat_date_setseconds, 2 },
        { "setMilliseconds", nat_date_setms, 1 }, { "setUTCMilliseconds", nat_date_setms, 1 },
        { "setTime", nat_date_settime, 1 },
        { "toString", nat_date_tostring, 0 }, { "toDateString", nat_date_todatestring, 0 },
        { "toTimeString", nat_date_totimestring, 0 }, { "toUTCString", nat_date_toutcstring, 0 },
        { "toGMTString", nat_date_toutcstring, 0 }, { "toISOString", nat_date_toisostring, 0 },
        { "toLocaleDateString", nat_date_tolocaledate, 0 },
        { "toLocaleTimeString", nat_date_tolocaletime, 0 },
        { "toLocaleString", nat_date_tolocale, 0 }, { "toJSON", nat_date_tojson, 1 },
        { 0, 0, 0 }
    };
    for (int i = 0; M[i].n; i++) js_method(J, dp, M[i].n, M[i].f, M[i].k);
    js_method_key(J, dp, J->sym_to_primitive, "[Symbol.toPrimitive]", nat_date_toprim, 1);
}
