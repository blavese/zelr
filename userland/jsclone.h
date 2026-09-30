/* structuredClone: a deep copy of a value, as a message between windows
 * carries one. Libraries copy state with it now rather than with
 * JSON.parse(JSON.stringify(...)), which loses dates, maps and anything
 * shared or cyclic.
 *
 * Primitives come back as they are; plain objects and arrays with their own
 * enumerable properties; Dates, regular expressions, Maps, Sets, errors,
 * boxed values, ArrayBuffers, typed arrays and DataViews as new ones of the
 * same kind. An object reached twice is copied once, so what was shared is
 * shared in the copy and a cycle is a cycle (the memo is a Map from each
 * original to its copy). A function, a symbol, a promise, a weak collection,
 * a proxy or a platform object (a node, a window) is a DataCloneError, as
 * the standard has it.
 *
 * Not done: transfer -- the list is accepted, and what it names is copied
 * rather than moved. */
#pragma once

#define JSC_DEPTH 2000

static jval jsc_fail(jctx *J, const char *what) {
    js_throw_dom(J, "DataCloneError", what);
    return js_undef();
}

static jval jsc_clone2(jctx *JF, jctx *JT, jval v, jmap *memo, int depth);

/* A string as the copy's context holds it: the same one in one context, its
   bytes copied into the other when the clone crosses to a worker's. */
static jstr *jsc_str(jctx *JF, jctx *JT, jstr *s) {
    return JF == JT || !s ? s : js_str_n(JT, s->s, s->len);
}

/* The own enumerable string-keyed properties of from, cloned onto to. */
static int jsc_props(jctx *JF, jctx *JT, jobj *from, jobj *to, jmap *memo, int depth) {
    jprop **own;
    u32 n = js_keys_of(JF, from, &own, JK_STR);
    for (u32 i = 0; i < n && JF->sig == JS_OK && JT->sig == JS_OK; i++) {
        jprop *p = own[i];
        if (!(p->flags & JP_ENUM)) continue;
        jval val = p->v.t == JS_ACC ? js_prop_read(JF, p, js_from_obj(from)) : p->v;
        if (JF->sig != JS_OK) return 0;
        jval c = jsc_clone2(JF, JT, val, memo, depth + 1);
        if (JF->sig != JS_OK || JT->sig != JS_OK) return 0;
        js_put_prop(JT, to, jsc_str(JF, JT, p->key), c);
    }
    return JF->sig == JS_OK && JT->sig == JS_OK;
}

/* Reads in JF and makes in JT, which are the same context for
   structuredClone and differ for a message to or from a worker. A refusal is
   thrown in JF, where the value was offered. */
static jval jsc_clone2(jctx *JF, jctx *JT, jval v, jmap *memo, int depth) {
    if (v.t == JS_SYM) return jsc_fail(JF, "a symbol cannot be cloned");
    if (v.t == JS_STR) return js_from_str(jsc_str(JF, JT, v.str));
    if (v.t == JS_BIG && JF != JT) {
        u32 bytes = (u32)sizeof(jbint) + (v.big->n ? v.big->n - 1 : 0) * 4;
        jbint *b = (jbint *)js_alloc(JT, bytes);
        if (!b) return js_undef();
        u8 *d = (u8 *)b, *s = (u8 *)v.big;
        for (u32 i = 0; i < bytes; i++) d[i] = s[i];
        jval r = v;
        r.big = b;
        return r;
    }
    if (v.t != JS_OBJ) return v;
    if (depth > JSC_DEPTH) return jsc_fail(JF, "that is nested too deeply to clone");
    int seen = jm_find(memo, v);
    if (seen >= 0) return memo->e[seen].v;
    jobj *o = v.obj, *c = 0;
    if (o->flags & JOF_PROXY) return jsc_fail(JF, "a proxy cannot be cloned");
    if (js_callable(v)) return jsc_fail(JF, "a function cannot be cloned");
    switch (o->kind) {
        case JO_PLAIN: case JO_ARGS:
            if (o->host != -1) return jsc_fail(JF, "a platform object cannot be cloned");
            c = js_object_with(JT, JO_PLAIN, JT->p_object);
            if (!c) return js_undef();
            jm_set(JT, memo, v, js_from_obj(c));
            if (!jsc_props(JF, JT, o, c, memo, depth)) return js_undef();
            return js_from_obj(c);
        case JO_ARRAY: {
            c = js_array(JT);
            if (!c) return js_undef();
            jm_set(JT, memo, v, js_from_obj(c));
            js_arr_reserve(JT, c, o->len);
            for (u32 i = 0; i < o->len && JF->sig == JS_OK; i++) {
                jval e = o->items[i];
                if (e.t == JS_HOLE) { js_arr_set(JT, c, i, e); continue; }
                jval ce = jsc_clone2(JF, JT, e, memo, depth + 1);
                if (JF->sig != JS_OK || JT->sig != JS_OK) return js_undef();
                js_arr_set(JT, c, i, ce);
            }
            if (!jsc_props(JF, JT, o, c, memo, depth)) return js_undef();
            return js_from_obj(c);
        }
        case JO_BOXED:
            if (o->ival.t == JS_SYM) return jsc_fail(JF, "a symbol cannot be cloned");
            c = js_to_object(JT, jsc_clone2(JF, JT, o->ival, memo, depth + 1));
            break;
        case JO_DATE:
            c = js_object_with(JT, JO_DATE, JT->p_date);
            if (c) c->ival = o->ival;
            break;
        case JO_REGEX: {
            jstr *src = o->name;
            c = js_regex_new(JT, src ? src->s : "", src ? src->len : 0, o->spare);
            break;
        }
        case JO_MAP: case JO_SET: {
            c = js_object_with(JT, o->kind, o->kind == JO_MAP ? JT->p_map : JT->p_set);
            jmap *m = jm_new(JT), *src = (jmap *)o->internal;
            if (!c || !m) return js_undef();
            c->internal = m;
            jm_set(JT, memo, v, js_from_obj(c));
            for (u32 i = 0; src && i < src->n && JF->sig == JS_OK; i++) {
                if (src->e[i].gone) continue;
                jval k = jsc_clone2(JF, JT, src->e[i].k, memo, depth + 1);
                if (JF->sig != JS_OK || JT->sig != JS_OK) return js_undef();
                jval val = o->kind == JO_MAP ? jsc_clone2(JF, JT, src->e[i].v, memo, depth + 1) : k;
                if (JF->sig != JS_OK || JT->sig != JS_OK) return js_undef();
                jm_set(JT, m, k, val);
            }
            return js_from_obj(c);
        }
        case JO_ERROR: {
            /* The kind it was when that is one of the standard's, else Error:
               found by its place among JF's, and made as JT's of that place. */
            int kind = 0, found = 0;
            for (jobj *q = o->proto; q && !found; q = q->proto)
                for (int k = 0; k < 8; k++)
                    if (JF->err_proto[k] && q == JF->err_proto[k]) { kind = k; found = 1; break; }
            c = js_object_with(JT, JO_ERROR, JT->err_proto[kind] ? JT->err_proto[kind] : JT->err_proto[0]);
            if (!c) return js_undef();
            jm_set(JT, memo, v, js_from_obj(c));
            static const char *const KEEP[] = { "message", "name", "stack", "cause", 0 };
            for (int k = 0; KEEP[k]; k++) {
                jprop *p = js_find(o, js_str(JF, KEEP[k]));
                if (!p || p->v.t == JS_ACC) continue;
                jval cv = jsc_clone2(JF, JT, p->v, memo, depth + 1);
                if (JF->sig != JS_OK || JT->sig != JS_OK) return js_undef();
                js_put_prop_flags(JT, c, jsc_str(JF, JT, p->key), cv, JP_WRITE | JP_CONF);
            }
            return js_from_obj(c);
        }
        case JO_BUFFER: {
            u32 n = ta_buflen(o);
            c = ta_new_buffer(JT, JT->p_buffer, (double)n);
            if (!c) return js_undef();
            u8 *d = ta_bytes(c), *s = ta_bytes(o);
            for (u32 i = 0; i < n; i++) d[i] = s[i];
            break;
        }
        case JO_TYPED: case JO_VIEW: {
            jtyped *t = (jtyped *)o->internal;
            if (!t) return jsc_fail(JF, "that view has nothing to clone");
            jval nb = jsc_clone2(JF, JT, js_from_obj(t->buf), memo, depth + 1);
            if (JF->sig != JS_OK || JT->sig != JS_OK || !js_is_obj(nb)) return js_undef();
            if (o->kind == JO_TYPED) c = ta_make(JT, t->type, nb.obj, t->off, t->len, JT->p_typed[t->type]);
            else {
                c = js_object_with(JT, JO_VIEW, JT->p_view);
                jtyped *x = (jtyped *)js_alloc(JT, (u32)sizeof(jtyped));
                if (!c || !x) return js_undef();
                *x = *t;
                x->buf = nb.obj;
                c->internal = x;
            }
            break;
        }
        default:
            return jsc_fail(JF, "that kind of object cannot be cloned");
    }
    if (!c) return js_undef();
    jm_set(JT, memo, v, js_from_obj(c));
    return js_from_obj(c);
}

static jval jsc_clone(jctx *J, jval v, jmap *memo, int depth) { return jsc_clone2(J, J, v, memo, depth); }

/* A value from one context made in another, for a message to or from a
   worker: undefined, and JF's error set, when it cannot be copied. */
__attribute__((unused)) static jval jsc_across(jctx *JF, jctx *JT, jval v) {
    jmap *memo = jm_new(JT);
    if (!memo) return js_undef();
    return jsc_clone2(JF, JT, v, memo, 0);
}

static jval nat_structured_clone(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (n < 1) return js_throw(J, JS_ERR_TYPE, "structuredClone needs a value", J->error_line);
    jmap *memo = jm_new(J);
    if (!memo) return js_undef();
    return jsc_clone(J, a[0], memo, 0);
}

static void js_setup_clone(jctx *J) {
    jobj *f = js_method(J, 0, "structuredClone", nat_structured_clone, 1);
    if (f) js_declare_flags(J, J->global, js_str(J, "structuredClone"), js_from_obj(f), JP_WRITE | JP_CONF);
}
