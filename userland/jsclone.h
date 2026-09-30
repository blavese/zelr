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

static jval jsc_clone(jctx *J, jval v, jmap *memo, int depth);

/* The own enumerable string-keyed properties of from, cloned onto to. */
static int jsc_props(jctx *J, jobj *from, jobj *to, jmap *memo, int depth) {
    jprop **own;
    u32 n = js_keys_of(J, from, &own, JK_STR);
    for (u32 i = 0; i < n && J->sig == JS_OK; i++) {
        jprop *p = own[i];
        if (!(p->flags & JP_ENUM)) continue;
        jval val = p->v.t == JS_ACC ? js_prop_read(J, p, js_from_obj(from)) : p->v;
        if (J->sig != JS_OK) return 0;
        jval c = jsc_clone(J, val, memo, depth + 1);
        if (J->sig != JS_OK) return 0;
        js_put_prop(J, to, p->key, c);
    }
    return J->sig == JS_OK;
}

static jval jsc_clone(jctx *J, jval v, jmap *memo, int depth) {
    if (v.t == JS_SYM) return jsc_fail(J, "a symbol cannot be cloned");
    if (v.t != JS_OBJ) return v;
    if (depth > JSC_DEPTH) return jsc_fail(J, "that is nested too deeply to clone");
    int seen = jm_find(memo, v);
    if (seen >= 0) return memo->e[seen].v;
    jobj *o = v.obj, *c = 0;
    if (o->flags & JOF_PROXY) return jsc_fail(J, "a proxy cannot be cloned");
    if (js_callable(v)) return jsc_fail(J, "a function cannot be cloned");
    switch (o->kind) {
        case JO_PLAIN: case JO_ARGS:
            if (o->host != -1) return jsc_fail(J, "a platform object cannot be cloned");
            c = js_object_with(J, JO_PLAIN, J->p_object);
            if (!c) return js_undef();
            jm_set(J, memo, v, js_from_obj(c));
            if (!jsc_props(J, o, c, memo, depth)) return js_undef();
            return js_from_obj(c);
        case JO_ARRAY: {
            c = js_array(J);
            if (!c) return js_undef();
            jm_set(J, memo, v, js_from_obj(c));
            js_arr_reserve(J, c, o->len);
            for (u32 i = 0; i < o->len && J->sig == JS_OK; i++) {
                jval e = o->items[i];
                if (e.t == JS_HOLE) { js_arr_set(J, c, i, e); continue; }
                jval ce = jsc_clone(J, e, memo, depth + 1);
                if (J->sig != JS_OK) return js_undef();
                js_arr_set(J, c, i, ce);
            }
            if (!jsc_props(J, o, c, memo, depth)) return js_undef();
            return js_from_obj(c);
        }
        case JO_BOXED:
            if (o->ival.t == JS_SYM) return jsc_fail(J, "a symbol cannot be cloned");
            c = js_to_object(J, o->ival);
            break;
        case JO_DATE:
            c = js_object_with(J, JO_DATE, J->p_date);
            if (c) c->ival = o->ival;
            break;
        case JO_REGEX: {
            jstr *src = o->name;
            c = js_regex_new(J, src ? src->s : "", src ? src->len : 0, o->spare);
            break;
        }
        case JO_MAP: case JO_SET: {
            c = js_object_with(J, o->kind, o->kind == JO_MAP ? J->p_map : J->p_set);
            jmap *m = jm_new(J), *src = (jmap *)o->internal;
            if (!c || !m) return js_undef();
            c->internal = m;
            jm_set(J, memo, v, js_from_obj(c));
            for (u32 i = 0; src && i < src->n && J->sig == JS_OK; i++) {
                if (src->e[i].gone) continue;
                jval k = jsc_clone(J, src->e[i].k, memo, depth + 1);
                if (J->sig != JS_OK) return js_undef();
                jval val = o->kind == JO_MAP ? jsc_clone(J, src->e[i].v, memo, depth + 1) : k;
                if (J->sig != JS_OK) return js_undef();
                jm_set(J, m, k, val);
            }
            return js_from_obj(c);
        }
        case JO_ERROR: {
            /* The kind it was when that is one of the standard's, else Error. */
            jobj *proto = J->err_proto[0];
            int found = 0;
            for (jobj *q = o->proto; q && !found; q = q->proto)
                for (int k = 0; k < 8; k++)
                    if (J->err_proto[k] && q == J->err_proto[k]) { proto = q; found = 1; break; }
            c = js_object_with(J, JO_ERROR, proto);
            if (!c) return js_undef();
            jm_set(J, memo, v, js_from_obj(c));
            static const char *const KEEP[] = { "message", "name", "stack", "cause", 0 };
            for (int k = 0; KEEP[k]; k++) {
                jprop *p = js_find(o, js_str(J, KEEP[k]));
                if (!p || p->v.t == JS_ACC) continue;
                jval cv = jsc_clone(J, p->v, memo, depth + 1);
                if (J->sig != JS_OK) return js_undef();
                js_put_prop_flags(J, c, p->key, cv, JP_WRITE | JP_CONF);
            }
            return js_from_obj(c);
        }
        case JO_BUFFER: {
            u32 n = ta_buflen(o);
            c = ta_new_buffer(J, J->p_buffer, (double)n);
            if (!c) return js_undef();
            u8 *d = ta_bytes(c), *s = ta_bytes(o);
            for (u32 i = 0; i < n; i++) d[i] = s[i];
            break;
        }
        case JO_TYPED: case JO_VIEW: {
            jtyped *t = (jtyped *)o->internal;
            if (!t) return jsc_fail(J, "that view has nothing to clone");
            jval nb = jsc_clone(J, js_from_obj(t->buf), memo, depth + 1);
            if (J->sig != JS_OK || !js_is_obj(nb)) return js_undef();
            if (o->kind == JO_TYPED) c = ta_make(J, t->type, nb.obj, t->off, t->len, J->p_typed[t->type]);
            else {
                c = js_object_with(J, JO_VIEW, J->p_view);
                jtyped *x = (jtyped *)js_alloc(J, (u32)sizeof(jtyped));
                if (!c || !x) return js_undef();
                *x = *t;
                x->buf = nb.obj;
                c->internal = x;
            }
            break;
        }
        default:
            return jsc_fail(J, "that kind of object cannot be cloned");
    }
    if (!c) return js_undef();
    jm_set(J, memo, v, js_from_obj(c));
    return js_from_obj(c);
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
