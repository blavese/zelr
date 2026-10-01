/* Arrays, strings and patterns: the methods on their prototypes.
 *
 * Included from jslib.h. Every array method works on anything with a length
 * and numbered properties, as the standard says, because pages borrow them:
 * Array.prototype.slice.call(arguments) and [].forEach.call(nodeList, f)
 * are on every other page. A real array takes the short way to its elements.
 */
#pragma once

/* --- things with a length ------------------------------------------------- */

static jobj *js_arraylike(jctx *J, jval t) {
    if (t.t == JS_NULL || t.t == JS_UNDEF) {
        js_throw(J, JS_ERR_TYPE, "an array method was called on nothing", J->error_line);
        return 0;
    }
    return js_is_obj(t) ? t.obj : 0;
}

static int js_is_dense(jval t) {
    return t.t == JS_OBJ && t.obj && (t.obj->kind == JO_ARRAY || t.obj->kind == JO_ARGS);
}

static u32 js_len_of(jctx *J, jval t) {
    if (js_is_dense(t)) return t.obj->len;
    if (t.t == JS_STR) return t.str->len;
    double d = js_to_num(J, js_get(J, t, J->s_length));
    if (!(d > 0)) return 0;
    if (d > 4294967295.0) return 4294967295u;
    return (u32)d;
}

static void js_put_index(jctx *J, jval t, u32 i, jval v) {
    if (t.t == JS_OBJ && t.obj && t.obj->kind == JO_ARRAY && !(t.obj->flags & JOF_FROZEN)
        && js_arr_set(J, t.obj, i, v))
        return;
    js_put(J, t, js_to_key(J, js_num(i)), v);
}

static int js_has_index(jctx *J, jval t, u32 i) {
    if (js_is_dense(t)) return i < t.obj->len && t.obj->items[i].t != JS_HOLE;
    if (t.t == JS_STR) return i < t.str->len;
    if (!js_is_obj(t)) return 0;
    return js_has(J, t.obj, js_to_key(J, js_num(i)));
}

static void js_put_length(jctx *J, jval t, u32 n) {
    if (t.t == JS_OBJ && t.obj && t.obj->kind == JO_ARRAY) {
        if (!(t.obj->flags & JOF_FROZEN)) js_set_length(J, t.obj, js_num(n));
        return;
    }
    js_put(J, t, J->s_length, js_num(n));
}

/* A position as the array methods read one: negative from the end, and held
   inside [0, len]. */
static u32 js_rel_index(jctx *J, jval v, u32 len, u32 dflt) {
    if (v.t == JS_UNDEF) return dflt;
    double d = js_trunc(js_to_num(J, v));
    if (d < 0) { d += len; if (d < 0) d = 0; }
    if (d > len) d = len;
    return (u32)d;
}

/* An array's elements joined with commas, as it becomes text: without a
   call when nobody has replaced how arrays become text. An array that holds
   itself is empty where it comes round again. */
static jval nat_arr_tostring(jctx *J, jval t, jval *a, int n);
static jval nat_arr_join(jctx *J, jval t, jval *a, int n);

static jstr *js_join(jctx *J, jval t, jstr *sep) {
    static jobj *joining[256];
    static int njoining;
    jobj *o = js_is_obj(t) ? t.obj : 0;
    for (int k = 0; o && k < njoining; k++) if (joining[k] == o) return js_str(J, "");
    if (njoining >= 256) return js_str(J, "");
    if (o) joining[njoining++] = o;
    u32 len = js_len_of(J, t);
    jtext out = { 0, 0, 0, 0 };
    for (u32 i = 0; i < len && J->sig == JS_OK; i++) {
        if (i && sep) jt_put(J, &out, sep->s, sep->len);
        jval e = js_get_index(J, t, i);
        if (e.t == JS_UNDEF || e.t == JS_NULL) continue;
        jstr *s = js_to_str(J, e);
        jt_put(J, &out, s->s, s->len);
        if (out.full) break;
    }
    if (o) njoining--;
    if (J->sig != JS_OK) { free(out.b); return js_str(J, ""); }
    return jt_done(J, &out);
}

static int js_array_join_fast(jctx *J, jval v, jstr **out) {
    jobj *o = v.obj;
    if (o->proto != J->p_array || js_find(o, J->s_toString) || js_find(o, J->s_valueOf)) return 0;
    if (!js_proto_value_is(J, J->p_array, J->s_toString, nat_arr_tostring)) return 0;
    if (!js_proto_value_is(J, J->p_object, J->s_valueOf, nat_obj_valueof)) return 0;
    if (js_find(J->p_array, J->s_valueOf)) return 0;
    *out = js_join(J, v, js_str(J, ","));
    return J->sig == JS_OK;
}

/* --- Array ------------------------------------------------------------------ */

/* Array(3) is three empty places and Array(1, 2) is two values, which is the
   one place this constructor is surprising and the one a page relies on. */
static jval nat_array_make(jctx *J, jval t, jval *a, int n) {
    jobj *o;
    if (J->new_target.t != JS_UNDEF && js_is_obj(t)) {
        o = t.obj;
        o->kind = JO_ARRAY;
    } else o = js_array(J);
    if (!o) return js_null();
    if (n == 1 && a[0].t == JS_NUM) {
        double want = a[0].num;
        if (!(want >= 0) || want != (double)(u32)want)
            return js_throw(J, JS_ERR_RANGE, "that is not a length an array can have", J->error_line);
        if (want > 0 && !js_arr_set(J, o, (u32)want - 1, js_hole()))
            return js_throw(J, JS_ERR_RANGE, "an array that long is more than a page may have", J->error_line);
        return js_from_obj(o);
    }
    js_arr_reserve(J, o, (u32)n);
    for (int i = 0; i < n; i++) js_arr_set(J, o, (u32)i, a[i]);
    return js_from_obj(o);
}

static jval nat_array_is(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval v = js_arg(a, n, 0);
    if (v.t == JS_OBJ && v.obj && (v.obj->flags & JOF_PROXY)) return js_bool(js_proxy_is_array(J, v.obj));
    return js_bool(v.t == JS_OBJ && v.obj && v.obj->kind == JO_ARRAY);
}

static jval nat_array_of(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *o = js_array(J);
    for (int i = 0; o && i < n; i++) js_arr_push(J, o, a[i]);
    return js_from_obj(o);
}

/* Array.from: an iterable, or anything with a length, with a function to
   map each value on the way. */
static jval nat_array_from(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval src = js_arg(a, n, 0), fn = js_arg(a, n, 1), self = js_arg(a, n, 2);
    if (fn.t != JS_UNDEF && !js_callable(fn))
        return js_throw(J, JS_ERR_TYPE, "Array.from's second argument has to be a function", J->error_line);
    jobj *o = js_array(J);
    if (!o) return js_undef();
    if (src.t == JS_NULL || src.t == JS_UNDEF)
        return js_throw(J, JS_ERR_TYPE, "Array.from needs something to make an array from", J->error_line);
    jval m = js_is_obj(src) || src.t == JS_STR ? js_get(J, src, J->sym_iterator) : js_undef();
    if (J->sig != JS_OK) return js_undef();
    if (js_callable(m)) {
        jiter it;
        if (!js_iter_open(J, src, &it)) return js_undef();
        for (u32 i = 0;; i++) {
            jval x;
            int r = js_iter_step(J, &it, &x);
            if (r < 0) return js_undef();
            if (r == 0) break;
            if (js_callable(fn)) {
                jval args[2] = { x, js_num(i) };
                x = js_call(J, fn, self, args, 2);
                if (J->sig != JS_OK) { js_iter_close(J, &it); return js_undef(); }
            }
            js_arr_push(J, o, x);
        }
        return js_from_obj(o);
    }
    u32 len = js_len_of(J, src);
    for (u32 i = 0; i < len && J->sig == JS_OK; i++) {
        jval x = js_get_index(J, src, i);
        if (js_callable(fn)) {
            jval args[2] = { x, js_num(i) };
            x = js_call(J, fn, self, args, 2);
        }
        js_arr_push(J, o, x);
    }
    return J->sig == JS_OK ? js_from_obj(o) : js_undef();
}

static jval nat_arr_push(jctx *J, jval t, jval *a, int n) {
    if (t.t == JS_OBJ && t.obj && t.obj->kind == JO_ARRAY && !(t.obj->flags & (JOF_FROZEN | JOF_NOEXT))) {
        jobj *o = t.obj;
        js_arr_reserve(J, o, o->len + (u32)n);
        for (int i = 0; i < n; i++) js_arr_push(J, o, a[i]);
        return js_num((double)o->len);
    }
    if (!js_arraylike(J, t)) return js_undef();
    if (t.t == JS_OBJ && (t.obj->flags & JOF_FROZEN))
        return js_throw(J, JS_ERR_TYPE, "a frozen array cannot be pushed to", J->error_line);
    u32 len = js_len_of(J, t);
    for (int i = 0; i < n; i++) js_put_index(J, t, len + (u32)i, a[i]);
    js_put_length(J, t, len + (u32)n);
    return js_num((double)(len + (u32)n));
}

static jval nat_arr_pop(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (t.t == JS_OBJ && t.obj && t.obj->kind == JO_ARRAY && !(t.obj->flags & JOF_FROZEN)) {
        if (!t.obj->len) return js_undef();
        return js_item(t.obj, --t.obj->len);
    }
    if (!js_arraylike(J, t)) return js_undef();
    u32 len = js_len_of(J, t);
    if (!len) { js_put_length(J, t, 0); return js_undef(); }
    jval v = js_get_index(J, t, len - 1);
    js_delete(J, t, js_to_key(J, js_num(len - 1)));
    js_put_length(J, t, len - 1);
    return v;
}

static jval nat_arr_shift(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (t.t == JS_OBJ && t.obj && t.obj->kind == JO_ARRAY && !(t.obj->flags & JOF_FROZEN)) {
        jobj *o = t.obj;
        if (!o->len) return js_undef();
        jval first = js_item(o, 0);
        for (u32 i = 1; i < o->len; i++) o->items[i - 1] = o->items[i];
        o->len--;
        return first;
    }
    if (!js_arraylike(J, t)) return js_undef();
    u32 len = js_len_of(J, t);
    if (!len) { js_put_length(J, t, 0); return js_undef(); }
    jval first = js_get_index(J, t, 0);
    for (u32 i = 1; i < len && J->sig == JS_OK; i++) js_put_index(J, t, i - 1, js_get_index(J, t, i));
    js_delete(J, t, js_to_key(J, js_num(len - 1)));
    js_put_length(J, t, len - 1);
    return first;
}

static jval nat_arr_unshift(jctx *J, jval t, jval *a, int n) {
    if (t.t == JS_OBJ && t.obj && t.obj->kind == JO_ARRAY && !(t.obj->flags & JOF_FROZEN)) {
        jobj *o = t.obj;
        u32 len = o->len;
        if (n && !js_arr_set(J, o, len + (u32)n - 1, js_undef())) return js_undef();
        for (u32 i = len; i > 0; i--) o->items[i - 1 + (u32)n] = o->items[i - 1];
        for (int i = 0; i < n; i++) o->items[i] = a[i];
        return js_num((double)o->len);
    }
    if (!js_arraylike(J, t)) return js_undef();
    u32 len = js_len_of(J, t);
    for (u32 i = len; i > 0 && J->sig == JS_OK; i--) js_put_index(J, t, i - 1 + (u32)n, js_get_index(J, t, i - 1));
    for (int i = 0; i < n; i++) js_put_index(J, t, (u32)i, a[i]);
    js_put_length(J, t, len + (u32)n);
    return js_num((double)(len + (u32)n));
}

static jval nat_arr_slice(jctx *J, jval t, jval *a, int n) {
    jobj *out = js_array(J);
    if (!out || (!js_is_obj(t) && t.t != JS_STR)) {
        if (t.t == JS_NULL || t.t == JS_UNDEF) js_arraylike(J, t);
        return js_from_obj(out);
    }
    u32 len = js_len_of(J, t);
    u32 from = js_rel_index(J, js_arg(a, n, 0), len, 0);
    u32 to = js_rel_index(J, js_arg(a, n, 1), len, len);
    if (to > from) js_arr_reserve(J, out, to - from);
    for (u32 i = from; i < to && J->sig == JS_OK; i++) js_arr_push(J, out, js_get_index(J, t, i));
    return js_from_obj(out);
}

static jval nat_arr_splice(jctx *J, jval t, jval *a, int n) {
    if (!js_arraylike(J, t)) return js_undef();
    if (t.obj->flags & JOF_FROZEN)
        return js_throw(J, JS_ERR_TYPE, "a frozen array cannot be spliced", J->error_line);
    u32 len = js_len_of(J, t);
    u32 start = js_rel_index(J, js_arg(a, n, 0), len, 0);
    u32 del;
    if (n == 0) del = 0;
    else if (n == 1) del = len - start;
    else {
        double d = js_trunc(js_to_num(J, a[1]));
        if (d < 0) d = 0;
        if (d > len - start) d = len - start;
        del = (u32)d;
    }
    u32 add = n > 2 ? (u32)(n - 2) : 0;
    jobj *removed = js_array(J);
    if (!removed) return js_undef();
    for (u32 i = 0; i < del; i++) js_arr_push(J, removed, js_get_index(J, t, start + i));
    if (t.obj->kind == JO_ARRAY) {
        jobj *o = t.obj;
        u32 nlen = len - del + add;
        if (add > del && !js_arr_set(J, o, nlen - 1, js_undef())) return js_undef();
        if (add != del) {
            if (add > del) for (u32 i = len; i > start + del; i--) o->items[i - 1 + add - del] = o->items[i - 1];
            else for (u32 i = start + del; i < len; i++) o->items[i - del + add] = o->items[i];
        }
        for (u32 i = 0; i < add; i++) o->items[start + i] = a[2 + i];
        o->len = nlen;
        return js_from_obj(removed);
    }
    /* Generic: move the tail, then write the new ones. */
    if (add < del) {
        for (u32 i = start; i < len - del && J->sig == JS_OK; i++)
            js_put_index(J, t, i + add, js_get_index(J, t, i + del));
        for (u32 i = len; i > len - del + add; i--) js_delete(J, t, js_to_key(J, js_num(i - 1)));
    } else if (add > del) {
        for (u32 i = len - del; i > start && J->sig == JS_OK; i--)
            js_put_index(J, t, i + add - 1, js_get_index(J, t, i + del - 1));
    }
    for (u32 i = 0; i < add; i++) js_put_index(J, t, start + i, a[2 + i]);
    js_put_length(J, t, len - del + add);
    return js_from_obj(removed);
}

static int js_is_spreadable(jctx *J, jval v) {
    (void)J;
    return v.t == JS_OBJ && v.obj && v.obj->kind == JO_ARRAY;
}

static jval nat_arr_concat(jctx *J, jval t, jval *a, int n) {
    jobj *out = js_array(J);
    if (!out) return js_undef();
    for (int k = -1; k < n && J->sig == JS_OK; k++) {
        jval v = k < 0 ? t : a[k];
        if (js_is_spreadable(J, v)) {
            jobj *o = v.obj;
            js_arr_reserve(J, out, out->len + o->len);
            for (u32 i = 0; i < o->len; i++) js_arr_push(J, out, o->items[i]);
            /* Far indices kept as properties come along too. */
        } else js_arr_push(J, out, v);
    }
    return js_from_obj(out);
}

static jval nat_arr_join(jctx *J, jval t, jval *a, int n) {
    if (t.t == JS_NULL || t.t == JS_UNDEF) { js_arraylike(J, t); return js_undef(); }
    jstr *sep = n > 0 && a[0].t != JS_UNDEF ? js_to_str(J, a[0]) : js_str(J, ",");
    return js_from_str(js_join(J, t, sep));
}

static jval nat_arr_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return nat_obj_tostring(J, t, 0, 0);
    jval j = js_get(J, t, js_str(J, "join"));
    if (J->sig != JS_OK) return js_undef();
    if (js_callable(j)) return js_call(J, j, t, 0, 0);
    return nat_obj_tostring(J, t, 0, 0);
}

static jval nat_arr_reverse(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_arraylike(J, t)) return t;
    if (t.obj->kind == JO_ARRAY) {
        jobj *o = t.obj;
        for (u32 i = 0, k = o->len; i + 1 < k; i++, k--) {
            jval tmp = o->items[i];
            o->items[i] = o->items[k - 1];
            o->items[k - 1] = tmp;
        }
        return t;
    }
    u32 len = js_len_of(J, t);
    for (u32 i = 0, k = len; i + 1 < k && J->sig == JS_OK; i++, k--) {
        jval x = js_get_index(J, t, i), y = js_get_index(J, t, k - 1);
        js_put_index(J, t, i, y);
        js_put_index(J, t, k - 1, x);
    }
    return t;
}

static jval nat_arr_indexof(jctx *J, jval t, jval *a, int n) {
    if (!js_arraylike(J, t) && t.t != JS_STR) return js_num(-1);
    u32 len = js_len_of(J, t);
    jval want = js_arg(a, n, 0);
    u32 from = js_rel_index(J, js_arg(a, n, 1), len, 0);
    for (u32 i = from; i < len; i++) {
        if (!js_has_index(J, t, i)) continue;                 /* a hole is no element */
        if (js_strict_eq(js_get_index(J, t, i), want)) return js_num((double)i);
    }
    return js_num(-1);
}

static jval nat_arr_lastindexof(jctx *J, jval t, jval *a, int n) {
    if (!js_arraylike(J, t)) return js_num(-1);
    u32 len = js_len_of(J, t);
    if (!len) return js_num(-1);
    jval want = js_arg(a, n, 0);
    double from = n > 1 ? js_trunc(js_to_num(J, a[1])) : (double)len - 1;
    if (from < 0) from += len;
    if (from >= len) from = (double)len - 1;
    for (double i = from; i >= 0; i--)
        if (js_has_index(J, t, (u32)i) && js_strict_eq(js_get_index(J, t, (u32)i), want)) return js_num(i);
    return js_num(-1);
}

static jval nat_arr_includes(jctx *J, jval t, jval *a, int n) {
    if (!js_arraylike(J, t)) return js_bool(0);
    u32 len = js_len_of(J, t);
    jval want = js_arg(a, n, 0);
    u32 from = js_rel_index(J, js_arg(a, n, 1), len, 0);
    for (u32 i = from; i < len; i++)
        if (js_same_zero(js_get_index(J, t, i), want)) return js_bool(1);
    return js_bool(0);
}

/* The shape of forEach, map, filter, some, every, find and their kind: a
   callback with the value, the index and the array, and `this` for it. */
enum { AI_EACH, AI_MAP, AI_FILTER, AI_SOME, AI_EVERY, AI_FIND, AI_FINDI, AI_FINDLAST, AI_FINDLASTI };

static jval js_arr_iterate(jctx *J, jval t, jval *a, int n, int kind) {
    if (!js_arraylike(J, t) && t.t != JS_STR) return js_undef();
    jval fn = js_arg(a, n, 0), self = js_arg(a, n, 1);
    if (!js_callable(fn)) return js_throw(J, JS_ERR_TYPE, "the callback is not a function", J->error_line);
    u32 len = js_len_of(J, t);
    jobj *out = kind == AI_MAP || kind == AI_FILTER ? js_array(J) : 0;
    if (kind == AI_MAP && out) js_arr_reserve(J, out, len);
    int backwards = kind == AI_FINDLAST || kind == AI_FINDLASTI;
    for (u32 k = 0; k < len; k++) {
        u32 i = backwards ? len - 1 - k : k;
        int skip_holes = kind != AI_FIND && kind != AI_FINDI && !backwards;
        if (skip_holes && !js_has_index(J, t, i)) {
            if (js_is_dense(t) && i >= t.obj->len) break;
            if (kind == AI_MAP) js_arr_set(J, out, i, js_hole());   /* map keeps the hole */
            continue;
        }
        if (js_is_dense(t) && i >= t.obj->len) break;
        jval x = js_get_index(J, t, i);
        jval args[3] = { x, js_num((double)i), t };
        jval r = js_call(J, fn, self, args, 3);
        if (J->sig != JS_OK) return js_undef();
        switch (kind) {
            case AI_MAP: js_arr_push(J, out, r); break;
            case AI_FILTER: if (js_to_bool(r)) js_arr_push(J, out, x); break;
            case AI_SOME: if (js_to_bool(r)) return js_bool(1); break;
            case AI_EVERY: if (!js_to_bool(r)) return js_bool(0); break;
            case AI_FIND: case AI_FINDLAST: if (js_to_bool(r)) return x; break;
            case AI_FINDI: case AI_FINDLASTI: if (js_to_bool(r)) return js_num((double)i); break;
            default: break;
        }
    }
    switch (kind) {
        case AI_MAP: case AI_FILTER: return js_from_obj(out);
        case AI_SOME: return js_bool(0);
        case AI_EVERY: return js_bool(1);
        case AI_FINDI: case AI_FINDLASTI: return js_num(-1);
        default: return js_undef();
    }
}

static jval nat_arr_foreach(jctx *J, jval t, jval *a, int n) { return js_arr_iterate(J, t, a, n, AI_EACH); }
static jval nat_arr_map(jctx *J, jval t, jval *a, int n) { return js_arr_iterate(J, t, a, n, AI_MAP); }
static jval nat_arr_filter(jctx *J, jval t, jval *a, int n) { return js_arr_iterate(J, t, a, n, AI_FILTER); }
static jval nat_arr_some(jctx *J, jval t, jval *a, int n) { return js_arr_iterate(J, t, a, n, AI_SOME); }
static jval nat_arr_every(jctx *J, jval t, jval *a, int n) { return js_arr_iterate(J, t, a, n, AI_EVERY); }
static jval nat_arr_find(jctx *J, jval t, jval *a, int n) { return js_arr_iterate(J, t, a, n, AI_FIND); }
static jval nat_arr_findindex(jctx *J, jval t, jval *a, int n) { return js_arr_iterate(J, t, a, n, AI_FINDI); }
static jval nat_arr_findlast(jctx *J, jval t, jval *a, int n) { return js_arr_iterate(J, t, a, n, AI_FINDLAST); }
static jval nat_arr_findlastindex(jctx *J, jval t, jval *a, int n) { return js_arr_iterate(J, t, a, n, AI_FINDLASTI); }

static jval js_arr_reduce(jctx *J, jval t, jval *a, int n, int right) {
    if (!js_arraylike(J, t)) return js_undef();
    jval fn = js_arg(a, n, 0);
    if (!js_callable(fn)) return js_throw(J, JS_ERR_TYPE, "the callback is not a function", J->error_line);
    u32 len = js_len_of(J, t);
    u32 k = 0;
    jval acc;
    if (n > 1) acc = a[1];
    else {
        int found = 0;
        while (k < len && !found) {
            u32 i = right ? len - 1 - k : k;
            if (js_is_dense(t) || js_has_index(J, t, i)) { acc = js_get_index(J, t, i); found = 1; }
            k++;
        }
        if (!found) return js_throw(J, JS_ERR_TYPE, "reduce of an empty array with no first value", J->error_line);
    }
    for (; k < len; k++) {
        u32 i = right ? len - 1 - k : k;
        if (!js_is_dense(t) && !js_has_index(J, t, i)) continue;
        if (js_is_dense(t) && i >= t.obj->len) continue;
        jval args[4] = { acc, js_get_index(J, t, i), js_num((double)i), t };
        acc = js_call(J, fn, js_undef(), args, 4);
        if (J->sig != JS_OK) return js_undef();
    }
    return acc;
}

static jval nat_arr_reduce(jctx *J, jval t, jval *a, int n) { return js_arr_reduce(J, t, a, n, 0); }
static jval nat_arr_reduceright(jctx *J, jval t, jval *a, int n) { return js_arr_reduce(J, t, a, n, 1); }

static jval nat_arr_fill(jctx *J, jval t, jval *a, int n) {
    if (!js_arraylike(J, t)) return t;
    u32 len = js_len_of(J, t);
    u32 from = js_rel_index(J, js_arg(a, n, 1), len, 0);
    u32 to = js_rel_index(J, js_arg(a, n, 2), len, len);
    jval v = js_arg(a, n, 0);
    for (u32 i = from; i < to && J->sig == JS_OK; i++) js_put_index(J, t, i, v);
    return t;
}

/* An index made a hole: in an array the element is gone and the length
   stays; anything else that is array-like loses the property. */
static void js_make_hole(jctx *J, jval t, u32 i) {
    if (js_is_dense(t)) {
        if (i < t.obj->len && !(t.obj->flags & JOF_FROZEN)) t.obj->items[i] = js_hole();
        return;
    }
    js_delete(J, t, js_to_key(J, js_num((double)i)));
}

/* One element copied, or the hole it was. */
static void js_copy_index(jctx *J, jval t, u32 to, u32 from) {
    if (js_has_index(J, t, from)) js_put_index(J, t, to, js_get_index(J, t, from));
    else js_make_hole(J, t, to);
}

static jval nat_arr_copywithin(jctx *J, jval t, jval *a, int n) {
    if (!js_arraylike(J, t)) return t;
    u32 len = js_len_of(J, t);
    u32 to = js_rel_index(J, js_arg(a, n, 0), len, 0);
    u32 from = js_rel_index(J, js_arg(a, n, 1), len, 0);
    u32 end = js_rel_index(J, js_arg(a, n, 2), len, len);
    u32 count = end > from ? end - from : 0;
    if (count > len - to) count = len - to;
    if (from < to && to < from + count) {
        for (u32 i = count; i > 0; i--) js_copy_index(J, t, to + i - 1, from + i - 1);
    } else {
        for (u32 i = 0; i < count; i++) js_copy_index(J, t, to + i, from + i);
    }
    return t;
}

static void js_flatten(jctx *J, jobj *out, jval src, double depth) {
    u32 len = js_len_of(J, src);
    for (u32 i = 0; i < len && J->sig == JS_OK; i++) {
        if (!js_is_dense(src) && !js_has_index(J, src, i)) continue;
        jval x = js_get_index(J, src, i);
        if (depth >= 1 && js_is_obj(x) && x.obj->kind == JO_ARRAY) js_flatten(J, out, x, depth - 1);
        else js_arr_push(J, out, x);
    }
}

static jval nat_arr_flat(jctx *J, jval t, jval *a, int n) {
    if (!js_arraylike(J, t)) return js_undef();
    double depth = js_arg(a, n, 0).t == JS_UNDEF ? 1 : js_trunc(js_to_num(J, a[0]));
    if (depth > 64) depth = 64;
    jobj *out = js_array(J);
    if (out) js_flatten(J, out, t, depth);
    return js_from_obj(out);
}

static jval nat_arr_flatmap(jctx *J, jval t, jval *a, int n) {
    jval mapped = js_arr_iterate(J, t, a, n, AI_MAP);
    if (J->sig != JS_OK) return js_undef();
    jobj *out = js_array(J);
    if (out) js_flatten(J, out, mapped, 1);
    return js_from_obj(out);
}

static jval nat_arr_at(jctx *J, jval t, jval *a, int n) {
    if (!js_arraylike(J, t) && t.t != JS_STR) return js_undef();
    u32 len = js_len_of(J, t);
    double d = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (d < 0) d += len;
    if (d < 0 || d >= len) return js_undef();
    return js_get_index(J, t, (u32)d);
}

/* A stable merge sort, by the comparison function when one is given and by
   text when one is not, which is what this language does by default and the
   reason [1, 10, 2] sorts into that order. undefined goes to the end. */
typedef struct { jctx *J; jval fn; int failed; } jsortctx;

static int js_sort_cmp(jsortctx *S, jval x, jval y) {
    jctx *J = S->J;
    if (x.t == JS_UNDEF) return y.t == JS_UNDEF ? 0 : 1;
    if (y.t == JS_UNDEF) return -1;
    if (js_callable(S->fn)) {
        jval args[2] = { x, y };
        double r = js_to_num(J, js_call(J, S->fn, js_undef(), args, 2));
        if (J->sig != JS_OK) { S->failed = 1; return 0; }
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    if (x.t == JS_NUM && y.t == JS_NUM && x.num == (double)(int)x.num && y.num == (double)(int)y.num
        && x.num >= 0 && y.num >= 0 && x.num < 10 && y.num < 10)
        return x.num < y.num ? -1 : x.num > y.num ? 1 : 0;
    jstr *a = js_to_str(J, x), *b = js_to_str(J, y);
    if (J->sig != JS_OK) { S->failed = 1; return 0; }
    u32 m = a->len < b->len ? a->len : b->len;
    for (u32 i = 0; i < m; i++)
        if (a->s[i] != b->s[i]) return (u8)a->s[i] < (u8)b->s[i] ? -1 : 1;
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

static void js_merge_sort(jsortctx *S, jval *v, jval *tmp, u32 n) {
    if (n < 2 || S->failed) return;
    if (n <= 8) {
        for (u32 i = 1; i < n && !S->failed; i++) {
            jval x = v[i];
            u32 k = i;
            while (k > 0 && js_sort_cmp(S, v[k - 1], x) > 0) { v[k] = v[k - 1]; k--; }
            v[k] = x;
        }
        return;
    }
    u32 mid = n / 2;
    js_merge_sort(S, v, tmp, mid);
    js_merge_sort(S, v + mid, tmp, n - mid);
    if (S->failed) return;
    u32 i = 0, j = mid, k = 0;
    while (i < mid && j < n) tmp[k++] = js_sort_cmp(S, v[j], v[i]) < 0 ? v[j++] : v[i++];
    while (i < mid) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    for (u32 q = 0; q < n; q++) v[q] = tmp[q];
}

static jval nat_arr_sort(jctx *J, jval t, jval *a, int n) {
    jval fn = js_arg(a, n, 0);
    if (fn.t != JS_UNDEF && !js_callable(fn))
        return js_throw(J, JS_ERR_TYPE, "sort's argument has to be a function", J->error_line);
    if (!js_arraylike(J, t)) return t;
    u32 len = js_len_of(J, t);
    if (len < 2) return t;
    /* In the region: the comparator runs scripts, and the collector must see
       what is being sorted. */
    if ((u64)len * 2 * sizeof(jval) > 0xFFFFFFF0u) { js_out_of_memory(J); return t; }
    jval *v = (jval *)js_alloc(J, len * 2 * (u32)sizeof(jval));
    if (!v) { js_out_of_memory(J); return t; }
    /* The elements there are, sorted, then the holes: a hole is no element,
       and goes to the end, after even undefined. */
    u32 m = 0;
    for (u32 i = 0; i < len; i++) if (js_has_index(J, t, i)) v[m++] = js_get_index(J, t, i);
    jsortctx S = { J, fn, 0 };
    js_merge_sort(&S, v, v + m, m);
    if (!S.failed && J->sig == JS_OK) {
        for (u32 i = 0; i < m; i++) js_put_index(J, t, i, v[i]);
        for (u32 i = m; i < len; i++) js_make_hole(J, t, i);
    }
    js_free(J, v, len * 2 * (u32)sizeof(jval));
    return t;
}

static jval js_copy_array(jctx *J, jval t) {
    jobj *out = js_array(J);
    if (!out) return js_undef();
    u32 len = js_len_of(J, t);
    js_arr_reserve(J, out, len);
    for (u32 i = 0; i < len && J->sig == JS_OK; i++) js_arr_push(J, out, js_get_index(J, t, i));
    return js_from_obj(out);
}

static jval nat_arr_toreversed(jctx *J, jval t, jval *a, int n) {
    jval c = js_copy_array(J, t);
    return J->sig == JS_OK ? nat_arr_reverse(J, c, a, n) : js_undef();
}
static jval nat_arr_tosorted(jctx *J, jval t, jval *a, int n) {
    jval c = js_copy_array(J, t);
    return J->sig == JS_OK ? nat_arr_sort(J, c, a, n) : js_undef();
}
static jval nat_arr_tospliced(jctx *J, jval t, jval *a, int n) {
    jval c = js_copy_array(J, t);
    if (J->sig != JS_OK) return js_undef();
    nat_arr_splice(J, c, a, n);
    return c;
}
static jval nat_arr_with(jctx *J, jval t, jval *a, int n) {
    u32 len = js_len_of(J, t);
    double d = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (d < 0) d += len;
    if (d < 0 || d >= len) return js_throw(J, JS_ERR_RANGE, "that index is not in the array", J->error_line);
    jval c = js_copy_array(J, t);
    if (J->sig == JS_OK) js_put_index(J, c, (u32)d, js_arg(a, n, 1));
    return c;
}

/* --- iterators over arrays, and the prototype every iterator has ----------- */

enum { IK_KEYS = 1, IK_VALUES, IK_ENTRIES };

static jval js_make_iter(jctx *J, jobj *proto, jval target, int kind) {
    jobj *it = js_object_with(J, JO_ITER, proto);
    if (!it) return js_undef();
    it->ival = target;
    it->internal = 0;
    it->spare = (u16)kind;
    return js_from_obj(it);
}

static jval nat_arr_keys(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_arraylike(J, t) && t.t != JS_STR) return js_undef();
    return js_make_iter(J, J->p_array_iter, t, IK_KEYS);
}
static jval nat_arr_values(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_arraylike(J, t) && t.t != JS_STR) return js_undef();
    return js_make_iter(J, J->p_array_iter, t, IK_VALUES);
}
static jval nat_arr_entries(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_arraylike(J, t) && t.t != JS_STR) return js_undef();
    return js_make_iter(J, J->p_array_iter, t, IK_ENTRIES);
}

static jval nat_arrayiter_next(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t) || t.obj->kind != JO_ITER)
        return js_throw(J, JS_ERR_TYPE, "next called on something that is not an iterator", J->error_line);
    jobj *it = t.obj;
    u32 i = (u32)(u64)it->internal;
    jval target = it->ival;
    if (target.t == JS_UNDEF) return js_iter_result(J, js_undef(), 1);
    if (i >= js_len_of(J, target)) {
        it->ival = js_undef();
        return js_iter_result(J, js_undef(), 1);
    }
    it->internal = (void *)(u64)(i + 1);
    if (it->spare == IK_KEYS) return js_iter_result(J, js_num((double)i), 0);
    jval v = js_get_index(J, target, i);
    if (it->spare == IK_VALUES) return js_iter_result(J, v, 0);
    jobj *pair = js_array(J);
    js_arr_push(J, pair, js_num((double)i));
    js_arr_push(J, pair, v);
    return js_iter_result(J, js_from_obj(pair), 0);
}

static jval nat_iter_self(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return t;
}

static void js_setup_arrays(jctx *J) {
    jobj *ap = J->p_array;
    jobj *array = js_ctor(J, "Array", nat_array_make, 1, ap);
    J->c_array = array;
    js_method(J, array, "isArray", nat_array_is, 1);
    js_method(J, array, "from", nat_array_from, 1);
    js_method(J, array, "of", nat_array_of, 0);
    struct { const char *n; jnative f; int k; } M[] = {
        { "push", nat_arr_push, 1 }, { "pop", nat_arr_pop, 0 }, { "shift", nat_arr_shift, 0 },
        { "unshift", nat_arr_unshift, 1 }, { "slice", nat_arr_slice, 2 }, { "splice", nat_arr_splice, 2 },
        { "concat", nat_arr_concat, 1 }, { "join", nat_arr_join, 1 }, { "reverse", nat_arr_reverse, 0 },
        { "indexOf", nat_arr_indexof, 1 }, { "lastIndexOf", nat_arr_lastindexof, 1 },
        { "includes", nat_arr_includes, 1 }, { "forEach", nat_arr_foreach, 1 },
        { "map", nat_arr_map, 1 }, { "filter", nat_arr_filter, 1 }, { "some", nat_arr_some, 1 },
        { "every", nat_arr_every, 1 }, { "find", nat_arr_find, 1 }, { "findIndex", nat_arr_findindex, 1 },
        { "findLast", nat_arr_findlast, 1 }, { "findLastIndex", nat_arr_findlastindex, 1 },
        { "reduce", nat_arr_reduce, 1 }, { "reduceRight", nat_arr_reduceright, 1 },
        { "fill", nat_arr_fill, 1 }, { "copyWithin", nat_arr_copywithin, 2 }, { "flat", nat_arr_flat, 0 },
        { "flatMap", nat_arr_flatmap, 1 }, { "at", nat_arr_at, 1 }, { "sort", nat_arr_sort, 1 },
        { "keys", nat_arr_keys, 0 }, { "entries", nat_arr_entries, 0 },
        { "toString", nat_arr_tostring, 0 }, { "toLocaleString", nat_arr_tostring, 0 },
        { "toReversed", nat_arr_toreversed, 0 }, { "toSorted", nat_arr_tosorted, 1 },
        { "toSpliced", nat_arr_tospliced, 2 }, { "with", nat_arr_with, 2 },
        { 0, 0, 0 }
    };
    for (int i = 0; M[i].n; i++) js_method(J, ap, M[i].n, M[i].f, M[i].k);
    jobj *values = js_method(J, ap, "values", nat_arr_values, 0);
    J->array_values = values;
    if (values) js_put_prop_flags(J, ap, J->sym_iterator, js_from_obj(values), JP_WRITE | JP_CONF);

    /* Every built-in iterator's prototype has one of its own, and that has
       Symbol.iterator returning the iterator itself, so an iterator can be
       walked by for-of as well as stepped by next. */
    js_method_key(J, J->p_iterator, J->sym_iterator, "[Symbol.iterator]", nat_iter_self, 0);
    js_method_key(J, J->p_async_iterator, J->sym_async_iterator, "[Symbol.asyncIterator]", nat_iter_self, 0);
    J->p_array_iter = js_object_with(J, JO_PLAIN, J->p_iterator);
    js_method(J, J->p_array_iter, "next", nat_arrayiter_next, 0);
    js_tag(J, J->p_array_iter, "Array Iterator");
}

/* --- String ------------------------------------------------------------------- */

static jval nat_str_ctor(jctx *J, jval t, jval *a, int n) {
    jval v;
    if (n == 0) v = js_from_str(js_str(J, ""));
    else if (a[0].t == JS_SYM && J->new_target.t == JS_UNDEF) v = nat_symbol_tostring(J, a[0], 0, 0);
    else v = js_from_str(js_to_str(J, a[0]));
    if (J->new_target.t != JS_UNDEF && js_is_obj(t)) {
        t.obj->kind = JO_BOXED;
        t.obj->ival = v;
        return t;
    }
    return v;
}

static jval nat_str_fromcharcode(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jtext o = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        u32 cu = (u32)js_to_i32(J, a[i]) & 0xFFFF;
        /* Two halves in a row are one character. */
        if (cu >= 0xD800 && cu < 0xDC00 && i + 1 < n) {
            u32 lo = (u32)js_to_i32(J, a[i + 1]) & 0xFFFF;
            if (lo >= 0xDC00 && lo < 0xE000) {
                cu = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        char buf[4];
        u32 k = js_utf8(cu, buf);
        jt_put(J, &o, buf, k);
    }
    return js_from_str(jt_done(J, &o));
}

static jval nat_str_fromcodepoint(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jtext o = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        double d = js_to_num(J, a[i]);
        if (d != js_trunc(d) || d < 0 || d > 0x10FFFF) {
            free(o.b);
            return js_throw(J, JS_ERR_RANGE, "that is not a code point", J->error_line);
        }
        char buf[4];
        u32 k = js_utf8((u32)d, buf);
        jt_put(J, &o, buf, k);
    }
    return js_from_str(jt_done(J, &o));
}

/* String.raw`...`: the pieces as they were written, with the values between. */
static jval nat_str_raw(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval strings = js_arg(a, n, 0);
    jval raw = js_get(J, strings, js_str(J, "raw"));
    if (J->sig != JS_OK) return js_undef();
    u32 len = js_len_of(J, raw);
    jtext o = { 0, 0, 0, 0 };
    for (u32 i = 0; i < len && J->sig == JS_OK; i++) {
        jstr *s = js_to_str(J, js_get_index(J, raw, i));
        jt_put(J, &o, s->s, s->len);
        if (i + 1 < len && (int)i + 1 < n) {
            jstr *v = js_to_str(J, a[i + 1]);
            jt_put(J, &o, v->s, v->len);
        }
    }
    if (J->sig != JS_OK) { free(o.b); return js_undef(); }
    return js_from_str(jt_done(J, &o));
}

static jval nat_str_valueof(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jval v = js_unbox(t);
    if (v.t != JS_STR) return js_throw(J, JS_ERR_TYPE, "this is not a string", J->error_line);
    return v;
}

static jval nat_str_charat(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "charAt");
    double d = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (d < 0 || d >= s->len) return js_from_str(js_str(J, ""));
    return js_from_str(js_str_n(J, s->s + (u32)d, 1));
}

static jval nat_str_charcode(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "charCodeAt");
    double d = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (d < 0 || d >= s->len) return js_num(js_nan());
    return js_num((double)(u8)s->s[(u32)d]);
}

/* The code point of the UTF-8 character that starts at a byte. */
static jval nat_str_codepoint(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "codePointAt");
    double d = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (d < 0 || d >= s->len) return js_undef();
    u32 i = (u32)d;
    u8 c = (u8)s->s[i];
    u32 k = js_utf8_len(c);
    if (c < 0x80 || (c & 0xC0) == 0x80 || i + k > s->len) return js_num(c);
    u32 cp = c & (k == 2 ? 0x1F : k == 3 ? 0x0F : 0x07);
    for (u32 q = 1; q < k; q++) cp = (cp << 6) | ((u8)s->s[i + q] & 0x3F);
    return js_num(cp);
}

static int js_find_sub(const jstr *h, const jstr *nd, u32 from) {
    if (!h || !nd) return -1;
    if (nd->len == 0) return from <= h->len ? (int)from : (int)h->len;
    if (nd->len > h->len) return -1;
    for (u32 i = from; i + nd->len <= h->len; i++) {
        if (h->s[i] != nd->s[0]) continue;
        u32 k = 1;
        while (k < nd->len && h->s[i + k] == nd->s[k]) k++;
        if (k == nd->len) return (int)i;
    }
    return -1;
}

/* A position in a string, as the methods that take one read it: missing is
   `dflt`, not a number is 0, and anything else is held inside the string. */
static int js_str_pos(jctx *J, jval *a, int n, int i, int len, int dflt) {
    if (i >= n || a[i].t == JS_UNDEF) return dflt;
    double d = js_to_num(J, a[i]);
    if (d != d || d < 0) return 0;
    if (d > (double)len) return len;
    return (int)d;
}

static jval nat_str_indexof(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_this_str(J, t, "indexOf"), *nd = js_to_str(J, js_arg(a, n, 0));
    return js_num((double)js_find_sub(h, nd, (u32)js_str_pos(J, a, n, 1, (int)h->len, 0)));
}

static jval nat_str_lastindexof(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_this_str(J, t, "lastIndexOf"), *nd = js_to_str(J, js_arg(a, n, 0));
    if (nd->len > h->len) return js_num(-1);
    int from = js_str_pos(J, a, n, 1, (int)h->len, (int)h->len);
    if (from > (int)(h->len - nd->len)) from = (int)(h->len - nd->len);
    for (int i = from; i >= 0; i--) {
        u32 k = 0;
        while (k < nd->len && h->s[i + k] == nd->s[k]) k++;
        if (k == nd->len) return js_num((double)i);
    }
    return js_num(-1);
}

static int js_is_regex(jval v) { return v.t == JS_OBJ && v.obj && v.obj->kind == JO_REGEX; }

static jval nat_str_includes(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_this_str(J, t, "includes");
    if (js_is_regex(js_arg(a, n, 0)))
        return js_throw(J, JS_ERR_TYPE, "includes takes text, not a pattern", J->error_line);
    jstr *nd = js_to_str(J, js_arg(a, n, 0));
    return js_bool(js_find_sub(h, nd, (u32)js_str_pos(J, a, n, 1, (int)h->len, 0)) >= 0);
}

static jval nat_str_startswith(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_this_str(J, t, "startsWith");
    if (js_is_regex(js_arg(a, n, 0)))
        return js_throw(J, JS_ERR_TYPE, "startsWith takes text, not a pattern", J->error_line);
    jstr *nd = js_to_str(J, js_arg(a, n, 0));
    u32 at = (u32)js_str_pos(J, a, n, 1, (int)h->len, 0);
    if (nd->len > h->len - at) return js_bool(0);
    for (u32 i = 0; i < nd->len; i++) if (h->s[at + i] != nd->s[i]) return js_bool(0);
    return js_bool(1);
}

/* Ends with, where the end is the second argument when there is one. */
static jval nat_str_endswith(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_this_str(J, t, "endsWith");
    if (js_is_regex(js_arg(a, n, 0)))
        return js_throw(J, JS_ERR_TYPE, "endsWith takes text, not a pattern", J->error_line);
    jstr *nd = js_to_str(J, js_arg(a, n, 0));
    u32 end = (u32)js_str_pos(J, a, n, 1, (int)h->len, (int)h->len);
    if (nd->len > end) return js_bool(0);
    for (u32 i = 0; i < nd->len; i++)
        if (h->s[end - nd->len + i] != nd->s[i]) return js_bool(0);
    return js_bool(1);
}

static jval nat_str_slice(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "slice");
    u32 len = s->len;
    u32 from = js_rel_index(J, js_arg(a, n, 0), len, 0);
    u32 to = js_rel_index(J, js_arg(a, n, 1), len, len);
    if (to <= from) return js_from_str(js_str(J, ""));
    return js_from_str(js_str_n(J, s->s + from, to - from));
}

/* Not slice. substring clamps a negative to nothing rather than counting it
   from the end, and takes its two ends in either order; substr is a start
   and a length. */
static jval nat_str_substring(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "substring");
    int len = (int)s->len;
    int from = js_str_pos(J, a, n, 0, len, 0);
    int to = js_str_pos(J, a, n, 1, len, len);
    if (from > to) { int k = from; from = to; to = k; }
    return js_from_str(js_str_n(J, s->s + from, (u32)(to - from)));
}

static jval nat_str_substr(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "substr");
    int len = (int)s->len;
    double d = n > 0 ? js_trunc(js_to_num(J, a[0])) : 0;
    int from = d < 0 ? (d < -len ? 0 : len + (int)d) : (d > len ? len : (int)d);
    int want = len - from;
    if (n > 1 && a[1].t != JS_UNDEF) {
        double w = js_trunc(js_to_num(J, a[1]));
        want = w < 0 ? 0 : (w > want ? want : (int)w);
    }
    return js_from_str(js_str_n(J, s->s + from, (u32)want));
}

/* One character, counted from the end when the index is negative. */
static jval nat_str_at(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "at");
    double d = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (d < 0) d += s->len;
    if (d < 0 || d >= s->len) return js_undef();
    return js_from_str(js_str_n(J, s->s + (u32)d, 1));
}

static jval nat_str_concat(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "concat");
    jtext o = { 0, 0, 0, 0 };
    jt_put(J, &o, s->s, s->len);
    for (int i = 0; i < n && J->sig == JS_OK; i++) {
        jstr *x = js_to_str(J, a[i]);
        jt_put(J, &o, x->s, x->len);
    }
    return js_from_str(jt_done(J, &o));
}

/* Padded to a length with a filler, at the front or the back. */
static jval js_str_pad(jctx *J, jval t, jval *a, int n, int front) {
    jstr *s = js_this_str(J, t, front ? "padStart" : "padEnd");
    double w = n > 0 ? js_trunc(js_to_num(J, a[0])) : 0;
    jstr *fill = n > 1 && a[1].t != JS_UNDEF ? js_to_str(J, a[1]) : js_str(J, " ");
    if (w <= (double)s->len || !fill->len) return js_from_str(s);
    if (w > (1 << 24)) return js_throw(J, JS_ERR_RANGE, "that padding is longer than a page may have", J->error_line);
    u32 need = (u32)w - s->len;
    jtext o = { 0, 0, 0, 0 };
    if (!front) jt_put(J, &o, s->s, s->len);
    for (u32 i = 0; i < need; i++) jt_put(J, &o, fill->s + i % fill->len, 1);
    if (front) jt_put(J, &o, s->s, s->len);
    return js_from_str(jt_done(J, &o));
}

static jval nat_str_padstart(jctx *J, jval t, jval *a, int n) { return js_str_pad(J, t, a, n, 1); }
static jval nat_str_padend(jctx *J, jval t, jval *a, int n) { return js_str_pad(J, t, a, n, 0); }

static jval js_str_case(jctx *J, jval t, int upper) {
    jstr *s = js_this_str(J, t, upper ? "toUpperCase" : "toLowerCase");
    jstr *o = js_str_n(J, s->s, s->len);
    if (!o) return js_undef();
    for (u32 i = 0; i < o->len; i++) {
        char c = o->s[i];
        if (upper && c >= 'a' && c <= 'z') o->s[i] = (char)(c - 32);
        else if (!upper && c >= 'A' && c <= 'Z') o->s[i] = (char)(c + 32);
        else if ((u8)c == 0xC3 && i + 1 < o->len) {
            /* The accented Latin letters, which are the ones pages meet. */
            u8 d = (u8)o->s[i + 1];
            if (upper && d >= 0xA0 && d <= 0xBE && d != 0xB7) o->s[i + 1] = (char)(d - 0x20);
            else if (!upper && d >= 0x80 && d <= 0x9E && d != 0x97) o->s[i + 1] = (char)(d + 0x20);
            i++;
        }
    }
    o->hash = js_hash(o->s, o->len);
    return js_from_str(o);
}

static jval nat_str_upper(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_str_case(J, t, 1); }
static jval nat_str_lower(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_str_case(J, t, 0); }

static int js_str_blank(const char *s, u32 left, u32 *k) {
    char c = s[0];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') { *k = 1; return 1; }
    int nl;
    int w = (u8)c >= 0x80 ? js_wide_space(s, left, &nl) : 0;
    if (w) { *k = (u32)w; return 1; }
    return 0;
}

/* Blanks off both ends, or one: `which` is 1 for the front, 2 the back. */
static jval js_str_trim_ends(jctx *J, jval t, int which) {
    jstr *s = js_this_str(J, t, "trim");
    u32 from = 0, to = s->len, k;
    if (which & 1) while (from < to && js_str_blank(s->s + from, to - from, &k)) from += k;
    if (which & 2) {
        for (;;) {
            if (to > from && js_str_blank(s->s + to - 1, 1, &k)) { to--; continue; }
            if (to >= from + 2) {
                int nl;
                if (to >= from + 3 && js_wide_space(s->s + to - 3, 3, &nl) == 3) { to -= 3; continue; }
                if (js_wide_space(s->s + to - 2, 2, &nl) == 2) { to -= 2; continue; }
            }
            break;
        }
    }
    return js_from_str(js_str_n(J, s->s + from, to - from));
}

static jval nat_str_trim(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_str_trim_ends(J, t, 3); }
static jval nat_str_trimstart(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_str_trim_ends(J, t, 1); }
static jval nat_str_trimend(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_str_trim_ends(J, t, 2); }

static jval nat_str_repeat(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "repeat");
    double times = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (times < 0 || times - times != 0)
        return js_throw(J, JS_ERR_RANGE, "repeat takes a count that is not negative", J->error_line);
    if (times * s->len > (double)J->mem_cap)
        return js_throw(J, JS_ERR_RANGE, "that would be longer than a page may have", J->error_line);
    jtext o = { 0, 0, 0, 0 };
    for (u32 i = 0; i < (u32)times && !o.full; i++) jt_put(J, &o, s->s, s->len);
    return js_from_str(jt_done(J, &o));
}

static jval intl_locale_compare(jctx *J, jstr *x, jstr *y, jval locales, jval opts);

/* As an Intl.Collator of its options compares (jsintl.h): letters before
   their accents before their case, lower case first. */
static jval nat_str_localecompare(jctx *J, jval t, jval *a, int n) {
    jstr *x = js_this_str(J, t, "localeCompare");
    if (!x || J->sig != JS_OK) return js_undef();
    jstr *y = js_to_str(J, js_arg(a, n, 0));
    if (!y || J->sig != JS_OK) return js_undef();
    return intl_locale_compare(J, x, y, js_arg(a, n, 1), js_arg(a, n, 2));
}

static jval nat_str_normalize(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    return js_from_str(js_this_str(J, t, "normalize"));
}

static jval nat_str_iterator(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *s = js_this_str(J, t, "[Symbol.iterator]");
    return js_make_iter(J, J->p_string_iter, js_from_str(s), IK_VALUES);
}

static jval nat_striter_next(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t) || t.obj->kind != JO_ITER)
        return js_throw(J, JS_ERR_TYPE, "next called on something that is not an iterator", J->error_line);
    jobj *it = t.obj;
    jstr *s = it->ival.str;
    u32 i = (u32)(u64)it->internal;
    if (!s || i >= s->len) return js_iter_result(J, js_undef(), 1);
    u32 k = js_utf8_len((u8)s->s[i]);
    if (i + k > s->len) k = s->len - i;
    it->internal = (void *)(u64)(i + k);
    return js_iter_result(J, js_from_str(js_str_n(J, s->s + i, k)), 0);
}

/* --- patterns --------------------------------------------------------------
 *
 * A pattern object keeps its source and flags on itself and is compiled
 * fresh for each call. Compiling is a walk over a string that is almost
 * always under thirty characters; keeping a compiled copy would mean a page
 * that builds a pattern in a loop keeps every one.
 *
 * One scratch engine, because nothing here matches two patterns at once:
 * every use below compiles, matches and is finished before the next.
 */
static rx js_rx;

static int js_rx_load(jctx *J, jval v) {
    if (!js_is_regex(v)) return 0;
    jobj *o = v.obj;
    char flags[10];
    int w = 0;
    if (o->spare & RXF_G) flags[w++] = 'g';
    if (o->spare & RXF_I) flags[w++] = 'i';
    if (o->spare & RXF_M) flags[w++] = 'm';
    if (o->spare & RXF_S) flags[w++] = 's';
    if (o->spare & RXF_Y) flags[w++] = 'y';
    if (o->spare & (RXF_U | RXF_V)) flags[w++] = 'u';
    flags[w] = 0;
    jstr *src = o->name;
    if (!rx_compile(&js_rx, src ? src->s : "", src ? (int)src->len : 0, flags)) {
        js_throw(J, JS_ERR_SYNTAX, js_rx.why[0] ? js_rx.why : "a pattern this cannot read",
                 J->error_line);
        return 0;
    }
    return 1;
}

/* Where a match is looked for: from `from`, and with y only there. */
static int js_rx_search(jobj *re, jstr *s, int from) {
    int at = rx_search(&js_rx, s->s, (int)s->len, from);
    if (at >= 0 && (re->spare & RXF_Y) && at != from) return -1;
    return at;
}

static jval js_rx_cap(jctx *J, jstr *s, int g) {
    if (js_rx.cap_start[g] < 0 || js_rx.cap_end[g] < js_rx.cap_start[g]) return js_undef();
    return js_from_str(js_str_n(J, s->s + js_rx.cap_start[g], (u32)(js_rx.cap_end[g] - js_rx.cap_start[g])));
}

/* The named groups' values, as the object a match's `groups` is; undefined
   when the pattern names none. */
static jval js_rx_groups(jctx *J, jstr *s) {
    if (!js_rx.named) return js_undef();
    jobj *g = js_object_with(J, JO_PLAIN, 0);
    if (!g) return js_undef();
    for (int i = 1; i < js_rx.ncaps; i++)
        if (js_rx.names[i][0]) js_put_prop(J, g, js_str(J, js_rx.names[i]), js_rx_cap(J, s, i));
    return js_from_obj(g);
}

/* The array exec and match hand back: the whole match at nought, then each
   group, with where it was found and what it was found in. */
static jval js_rx_result(jctx *J, jstr *s) {
    jobj *out = js_array(J);
    if (!out) return js_null();
    for (int i = 0; i < js_rx.ncaps; i++) js_arr_set(J, out, (u32)i, js_rx_cap(J, s, i));
    js_put_prop(J, out, J->s_index, js_num((double)js_rx.cap_start[0]));
    js_put_prop(J, out, J->s_input, js_from_str(s));
    js_put_prop(J, out, J->s_groups, js_rx_groups(J, s));
    return js_from_obj(out);
}

/* How far an empty match moves on from at: one character, read the way the
   matcher reads them, so that the next try does not start inside one. */
static int js_rx_step(const jstr *s, int at) {
    if (at >= (int)s->len) return 1;
    int k;
    rx_utf8(s->s + at, (int)s->len - at, &k);
    return k;
}

static double js_last_index(jctx *J, jobj *re) {
    jval li = js_get(J, js_from_obj(re), J->s_lastIndex);
    double d = js_trunc(js_to_num(J, li));
    return d < 0 ? 0 : d;
}

static void js_set_last_index(jctx *J, jobj *re, double v) {
    js_put(J, js_from_obj(re), J->s_lastIndex, js_num(v));
}

/* exec walks a global or sticky pattern through its subject one call at a
   time, which is what lastIndex is for. */
static jval js_rx_exec(jctx *J, jval t, jstr *s) {
    if (!js_rx_load(J, t)) return js_null();
    jobj *re = t.obj;
    int walks = (re->spare & (RXF_G | RXF_Y)) != 0;
    int from = 0;
    if (walks) {
        double li = js_last_index(J, re);
        if (li > s->len) { js_set_last_index(J, re, 0); return js_null(); }
        from = (int)li;
    }
    if (js_rx_search(re, s, from) < 0) {
        if (walks) js_set_last_index(J, re, 0);
        return js_null();
    }
    if (walks) js_set_last_index(J, re, js_rx.cap_end[0]);
    return js_rx_result(J, s);
}

static jval nat_re_exec(jctx *J, jval t, jval *a, int n) {
    if (!js_is_regex(t)) return js_throw(J, JS_ERR_TYPE, "exec called on something that is not a pattern", J->error_line);
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    return js_rx_exec(J, t, s);
}

static jval nat_re_test(jctx *J, jval t, jval *a, int n) {
    if (!js_is_regex(t)) return js_throw(J, JS_ERR_TYPE, "test called on something that is not a pattern", J->error_line);
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    jobj *re = t.obj;
    if (!(re->spare & (RXF_G | RXF_Y))) {
        if (!js_rx_load(J, t)) return js_bool(0);
        return js_bool(js_rx_search(re, s, 0) >= 0);
    }
    return js_bool(js_rx_exec(J, t, s).t != JS_NULL);
}

static void js_flags_text(int flags, char *f) {
    int w = 0;
    if (flags & RXF_D) f[w++] = 'd';
    if (flags & RXF_G) f[w++] = 'g';
    if (flags & RXF_I) f[w++] = 'i';
    if (flags & RXF_M) f[w++] = 'm';
    if (flags & RXF_S) f[w++] = 's';
    if (flags & RXF_U) f[w++] = 'u';
    if (flags & RXF_V) f[w++] = 'v';
    if (flags & RXF_Y) f[w++] = 'y';
    f[w] = 0;
}

static jobj *js_regex_new(jctx *J, const char *pat, u32 len, int flags) {
    jobj *o = js_object(J, JO_REGEX);
    if (!o) return 0;
    o->name = js_str_n(J, pat, len);
    o->spare = (u16)flags;
    js_put_prop_flags(J, o, J->s_lastIndex, js_num(0), JP_WRITE);
    return o;
}

static int js_parse_flags(jctx *J, jstr *flg, int *out) {
    int flags = 0;
    for (u32 i = 0; flg && i < flg->len; i++) {
        char c = flg->s[i];
        int bit = c == 'g' ? RXF_G : c == 'i' ? RXF_I : c == 'm' ? RXF_M : c == 's' ? RXF_S
                : c == 'u' ? RXF_U : c == 'y' ? RXF_Y : c == 'd' ? RXF_D : c == 'v' ? RXF_V : 0;
        if (!bit || (flags & bit)) {
            js_throw(J, JS_ERR_SYNTAX, "a pattern flag that does not exist, or one given twice", J->error_line);
            return 0;
        }
        flags |= bit;
    }
    *out = flags;
    return 1;
}

/* RegExp("a.b", "i"), for a pattern that is not known until it is built.
   With or without `new`. */
static jval nat_regexp_make(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval first = js_arg(a, n, 0);
    int flags = 0;
    jstr *pat;
    if (js_is_regex(first)) {
        pat = first.obj->name;
        flags = first.obj->spare;
        if (n < 2 || a[1].t == JS_UNDEF) {
            if (J->new_target.t == JS_UNDEF) return first;
        }
    } else pat = first.t == JS_UNDEF ? js_str(J, "(?:)") : js_to_str(J, first);
    if (n > 1 && a[1].t != JS_UNDEF && !js_parse_flags(J, js_to_str(J, a[1]), &flags)) return js_undef();
    if (J->sig != JS_OK || !pat) return js_undef();
    jobj *o = js_regex_new(J, pat->s, pat->len, flags);
    if (!o) return js_null();
    /* Compiled now, so a pattern that will never match anything is refused
       where it is made. */
    if (!js_rx_load(J, js_from_obj(o))) return js_undef();
    if (J->new_target.t != JS_UNDEF && js_is_obj(J->new_target) && J->new_target.obj != J->callee) {
        jobj *p = js_proto_from(J, J->new_target, J->p_regexp);
        if (p) o->proto = p;
    }
    return js_from_obj(o);
}

static jval js_re_getter(jctx *J, jval t, int bit) {
    if (!js_is_regex(t)) {
        if (js_is_obj(t) && t.obj == J->p_regexp) return js_undef();
        return js_throw(J, JS_ERR_TYPE, "this is not a pattern", J->error_line);
    }
    return js_bool(t.obj->spare & bit);
}
static jval nat_re_global(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_re_getter(J, t, RXF_G); }
static jval nat_re_icase(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_re_getter(J, t, RXF_I); }
static jval nat_re_multiline(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_re_getter(J, t, RXF_M); }
static jval nat_re_sticky(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_re_getter(J, t, RXF_Y); }
static jval nat_re_unicode(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_re_getter(J, t, RXF_U); }
static jval nat_re_dotall(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_re_getter(J, t, RXF_S); }
static jval nat_re_hasindices(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return js_re_getter(J, t, RXF_D); }

static jval nat_re_source(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_regex(t)) return js_from_str(js_str(J, "(?:)"));
    return js_from_str(t.obj->name && t.obj->name->len ? t.obj->name : js_str(J, "(?:)"));
}

static jval nat_re_flags(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_regex(t)) return js_from_str(js_str(J, ""));
    char f[10];
    js_flags_text(t.obj->spare, f);
    return js_from_str(js_str(J, f));
}

static jval nat_re_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *src = js_to_str(J, js_get_str(J, t, "source"));
    jstr *fl = js_to_str(J, js_get_str(J, t, "flags"));
    if (J->sig != JS_OK) return js_undef();
    jtext o = { 0, 0, 0, 0 };
    jt_put(J, &o, "/", 1);
    jt_put(J, &o, src->s, src->len);
    jt_put(J, &o, "/", 1);
    jt_put(J, &o, fl->s, fl->len);
    return js_from_str(jt_done(J, &o));
}

/* Building the replacement, with $1 and friends standing for what the
   groups caught. */
static void js_rx_expand(jctx *J, jstr *with, jstr *s, jtext *out) {
    for (u32 i = 0; with && i < with->len; i++) {
        char c = with->s[i];
        if (c == '$' && i + 1 < with->len) {
            char d = with->s[i + 1];
            if (d == '$') { jt_put(J, out, "$", 1); i++; continue; }
            if (d == '&') {
                jt_put(J, out, s->s + js_rx.cap_start[0], (u32)(js_rx.cap_end[0] - js_rx.cap_start[0]));
                i++;
                continue;
            }
            if (d == '`') { jt_put(J, out, s->s, (u32)js_rx.cap_start[0]); i++; continue; }
            if (d == '<' && js_rx.named) {
                /* $<name>: what the named group caught. */
                u32 k = i + 2;
                while (k < with->len && with->s[k] != '>') k++;
                if (k < with->len) {
                    int g = rx_name_index(&js_rx, with->s + i + 2, (int)(k - i - 2));
                    if (g > 0 && js_rx.cap_start[g] >= 0)
                        jt_put(J, out, s->s + js_rx.cap_start[g], (u32)(js_rx.cap_end[g] - js_rx.cap_start[g]));
                    i = k;
                    continue;
                }
            }
            if (d == '\'') {
                jt_put(J, out, s->s + js_rx.cap_end[0], s->len - (u32)js_rx.cap_end[0]);
                i++;
                continue;
            }
            if (d >= '0' && d <= '9') {
                int g = d - '0';
                u32 used = 1;
                if (i + 2 < with->len && with->s[i + 2] >= '0' && with->s[i + 2] <= '9'
                    && (g * 10 + (with->s[i + 2] - '0')) < js_rx.ncaps) {
                    g = g * 10 + (with->s[i + 2] - '0');
                    used = 2;
                }
                if (g > 0 && g < js_rx.ncaps) {
                    if (js_rx.cap_start[g] >= 0)
                        jt_put(J, out, s->s + js_rx.cap_start[g], (u32)(js_rx.cap_end[g] - js_rx.cap_start[g]));
                    i += used;
                    continue;
                }
            }
        }
        jt_put(J, out, &c, 1);
    }
}

/* replace and replaceAll with a pattern. The output grows as it needs to:
   it was a sixteen kilobyte static buffer, which cut long results short and
   was overwritten by a replacement function that itself replaced. */
static jval js_str_replace_re(jctx *J, jstr *s, jval re, jval rep) {
    if (!js_rx_load(J, re)) return js_from_str(s);
    jobj *ro = re.obj;
    int every = (ro->spare & RXF_G) != 0;
    int call = js_callable(rep);
    jstr *with = call ? 0 : js_to_str(J, rep);
    jtext out = { 0, 0, 0, 0 };
    int from = every ? 0 : ((ro->spare & RXF_Y) ? (int)js_last_index(J, ro) : 0);
    int done_any = 0;
    u32 copied = 0;
    while (from <= (int)s->len) {
        if (js_rx_search(ro, s, from) < 0) break;
        done_any = 1;
        int start = js_rx.cap_start[0], end = js_rx.cap_end[0];
        jt_put(J, &out, s->s + copied, (u32)start - copied);
        if (call) {
            jargs A;
            js_args_init(&A);
            int ncaps = js_rx.ncaps;
            int cs[RX_CAPS], ce[RX_CAPS];
            for (int g = 0; g < ncaps; g++) { cs[g] = js_rx.cap_start[g]; ce[g] = js_rx.cap_end[g]; }
            for (int g = 0; g < ncaps; g++)
                js_args_push(J, &A, cs[g] < 0 ? js_undef()
                             : js_from_str(js_str_n(J, s->s + cs[g], (u32)(ce[g] - cs[g]))));
            js_args_push(J, &A, js_num((double)start));
            js_args_push(J, &A, js_from_str(s));
            if (js_rx.named) js_args_push(J, &A, js_rx_groups(J, s));
            jval got = js_call(J, rep, js_undef(), A.v, A.n);
            js_args_free(&A);
            if (J->sig != JS_OK) { free(out.b); return js_undef(); }
            jstr *gs = js_to_str(J, got);
            jt_put(J, &out, gs->s, gs->len);
            /* The engine is scratch and the call may have used it. */
            if (!js_rx_load(J, re)) { free(out.b); return js_undef(); }
        } else {
            js_rx_expand(J, with, s, &out);
        }
        copied = (u32)end;
        int next = end;
        if (next == start) {
            if (next < (int)s->len) {
                u32 k = (u32)js_rx_step(s, next);
                jt_put(J, &out, s->s + next, k);
                copied = (u32)next + k;
                next += (int)k;
            } else next++;
        }
        from = next;
        if (!every) break;
    }
    if (!every && (ro->spare & RXF_Y)) js_set_last_index(J, ro, done_any ? js_rx.cap_end[0] : 0);
    if (copied < s->len) jt_put(J, &out, s->s + copied, s->len - copied);
    if (every) js_set_last_index(J, ro, 0);
    return js_from_str(jt_done(J, &out));
}

/* A string pattern, replaced once or everywhere. */
static jval js_str_replace_text(jctx *J, jstr *s, jstr *find, jval rep, int every) {
    int call = js_callable(rep);
    jstr *with = call ? 0 : js_to_str(J, rep);
    jtext out = { 0, 0, 0, 0 };
    u32 at = 0;
    for (;;) {
        int hit = js_find_sub(s, find, at);
        if (hit < 0) break;
        jt_put(J, &out, s->s + at, (u32)hit - at);
        if (call) {
            jval args[3] = { js_from_str(js_str_n(J, s->s + hit, find->len)),
                             js_num((double)hit), js_from_str(s) };
            jval got = js_call(J, rep, js_undef(), args, 3);
            if (J->sig != JS_OK) { free(out.b); return js_undef(); }
            jstr *gs = js_to_str(J, got);
            jt_put(J, &out, gs->s, gs->len);
        } else {
            for (u32 i = 0; with && i < with->len; i++) {
                char c = with->s[i], d = i + 1 < with->len ? with->s[i + 1] : 0;
                if (c == '$' && d == '$') { jt_put(J, &out, "$", 1); i++; }
                else if (c == '$' && d == '&') { jt_put(J, &out, find->s, find->len); i++; }
                else if (c == '$' && d == '`') { jt_put(J, &out, s->s, (u32)hit); i++; }
                else if (c == '$' && d == '\'') {
                    jt_put(J, &out, s->s + hit + find->len, s->len - (u32)hit - find->len);
                    i++;
                } else jt_put(J, &out, &c, 1);
            }
        }
        at = (u32)hit + find->len;
        if (!every) break;
        if (find->len == 0) {
            /* An empty pattern matches between every character. */
            if (at >= s->len) break;
            jt_put(J, &out, s->s + at, 1);
            at++;
        }
    }
    if (at < s->len) jt_put(J, &out, s->s + at, s->len - at);
    return js_from_str(jt_done(J, &out));
}

static jval nat_str_replace(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "replace");
    if (J->sig != JS_OK) return js_undef();
    jval pat = js_arg(a, n, 0);
    if (js_is_regex(pat)) return js_str_replace_re(J, s, pat, js_arg(a, n, 1));
    jstr *find = js_to_str(J, pat);
    return js_str_replace_text(J, s, find, js_arg(a, n, 1), 0);
}

static jval nat_str_replaceall(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "replaceAll");
    if (J->sig != JS_OK) return js_undef();
    jval pat = js_arg(a, n, 0);
    if (js_is_regex(pat)) {
        if (!(pat.obj->spare & RXF_G))
            return js_throw(J, JS_ERR_TYPE, "replaceAll with a pattern needs the g flag", J->error_line);
        return js_str_replace_re(J, s, pat, js_arg(a, n, 1));
    }
    return js_str_replace_text(J, s, js_to_str(J, pat), js_arg(a, n, 1), 1);
}

/* A value as the pattern match and search use: a pattern as it is, anything
   else as the text of a new one. */
static jval js_as_regex(jctx *J, jval v, int flags) {
    if (js_is_regex(v)) return v;
    jstr *src = v.t == JS_UNDEF ? js_str(J, "(?:)") : js_to_str(J, v);
    jobj *o = js_regex_new(J, src->s, src->len, flags);
    return js_from_obj(o);
}

static jval nat_str_split(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "split");
    jobj *out = js_array(J);
    if (!out || J->sig != JS_OK) return js_from_obj(out);
    u32 limit = n > 1 && a[1].t != JS_UNDEF ? js_to_u32(J, a[1]) : 0xFFFFFFFFu;
    if (!limit) return js_from_obj(out);

    if (n == 0 || a[0].t == JS_UNDEF) {
        js_arr_push(J, out, js_from_str(s));
        return js_from_obj(out);
    }
    if (js_is_regex(a[0])) {
        /* The standard's walk: p is where the last piece ended and q where
           a match is looked for; an empty match where a piece would be
           empty moves q on by one character instead. */
        if (!js_rx_load(J, a[0])) return js_from_obj(out);
        js_rx.sticky = 0;
        int len = (int)s->len;
        if (!len) {
            if (rx_search(&js_rx, s->s, 0, 0) < 0) js_arr_push(J, out, js_from_str(s));
            return js_from_obj(out);
        }
        int p = 0, q = 0;
        while (q < len && out->len < limit) {
            int hit = rx_search(&js_rx, s->s, len, q);
            if (hit < 0 || hit >= len) break;
            int e = js_rx.cap_end[0];
            if (e == p) {
                q = hit + js_rx_step(s, hit);
                continue;
            }
            js_arr_push(J, out, js_from_str(js_str_n(J, s->s + p, (u32)(hit - p))));
            /* And whatever the groups caught, in between. */
            for (int g = 1; g < js_rx.ncaps && out->len < limit; g++)
                js_arr_push(J, out, js_rx_cap(J, s, g));
            p = q = e;
        }
        if (out->len < limit) js_arr_push(J, out, js_from_str(js_str_n(J, s->s + p, s->len - (u32)p)));
        return js_from_obj(out);
    }

    jstr *sep = js_to_str(J, a[0]);
    if (sep->len == 0) {
        for (u32 i = 0; i < s->len && out->len < limit; ) {
            u32 k = js_utf8_len((u8)s->s[i]);
            if (i + k > s->len) k = s->len - i;
            js_arr_push(J, out, js_from_str(js_str_n(J, s->s + i, k)));
            i += k;
        }
        return js_from_obj(out);
    }
    u32 at = 0;
    for (;;) {
        if (out->len >= limit) return js_from_obj(out);
        int hit = js_find_sub(s, sep, at);
        if (hit < 0) break;
        js_arr_push(J, out, js_from_str(js_str_n(J, s->s + at, (u32)hit - at)));
        at = (u32)hit + sep->len;
    }
    js_arr_push(J, out, js_from_str(js_str_n(J, s->s + at, s->len - at)));
    return js_from_obj(out);
}

/* Every match of a global pattern, as strings; with a plain one, the same
   array exec gives, which is what a page destructures for its groups. */
static jval nat_str_match(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "match");
    jval re = js_as_regex(J, js_arg(a, n, 0), 0);
    if (J->sig != JS_OK || !js_rx_load(J, re)) return js_null();
    if (!(re.obj->spare & RXF_G)) return js_rx_exec(J, re, s);

    jobj *out = js_array(J);
    if (!out) return js_null();
    int from = 0;
    while (from <= (int)s->len) {
        if (rx_search(&js_rx, s->s, (int)s->len, from) < 0) break;
        js_arr_push(J, out, js_from_str(js_str_n(J, s->s + js_rx.cap_start[0],
                                                 (u32)(js_rx.cap_end[0] - js_rx.cap_start[0]))));
        from = js_rx.cap_end[0];
        if (from == js_rx.cap_start[0]) from += js_rx_step(s, from);
    }
    js_set_last_index(J, re.obj, 0);
    return out->len ? js_from_obj(out) : js_null();
}

static jval nat_str_search(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "search");
    jval re = js_as_regex(J, js_arg(a, n, 0), 0);
    if (J->sig != JS_OK || !js_rx_load(J, re)) return js_num(-1);
    return js_num((double)rx_search(&js_rx, s->s, (int)s->len, 0));
}

/* matchAll: an iterator over every match, each the array exec gives. It
   walks a copy of the pattern, so the page's own lastIndex is left alone. */
static jval nat_str_matchall(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_this_str(J, t, "matchAll");
    jval re = js_arg(a, n, 0);
    if (js_is_regex(re) && !(re.obj->spare & RXF_G))
        return js_throw(J, JS_ERR_TYPE, "matchAll with a pattern needs the g flag", J->error_line);
    re = js_as_regex(J, re, RXF_G);
    if (J->sig != JS_OK) return js_undef();
    jobj *copy = js_regex_new(J, re.obj->name->s, re.obj->name->len, re.obj->spare | RXF_G);
    if (!copy) return js_undef();
    js_set_last_index(J, copy, js_last_index(J, re.obj));
    jobj *it = js_object_with(J, JO_ITER, J->p_regexp_iter);
    if (!it) return js_undef();
    it->ival = js_from_obj(copy);
    it->internal = s;
    return js_from_obj(it);
}

static jval nat_regexpiter_next(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t) || t.obj->kind != JO_ITER)
        return js_throw(J, JS_ERR_TYPE, "next called on something that is not an iterator", J->error_line);
    jobj *it = t.obj;
    if (it->ival.t != JS_OBJ) return js_iter_result(J, js_undef(), 1);
    jstr *s = (jstr *)it->internal;
    jobj *re = it->ival.obj;
    double before = js_last_index(J, re);
    jval m = js_rx_exec(J, it->ival, s);
    if (J->sig != JS_OK) return js_undef();
    if (m.t == JS_NULL) {
        it->ival = js_undef();
        return js_iter_result(J, js_undef(), 1);
    }
    /* An empty match would otherwise stand still for ever. */
    if (js_last_index(J, re) == before) js_set_last_index(J, re, before + js_rx_step(s, (int)before));
    return js_iter_result(J, m, 0);
}

static void js_setup_strings(jctx *J) {
    jobj *sp = J->p_string;
    jobj *string = js_ctor(J, "String", nat_str_ctor, 1, sp);
    js_method(J, string, "fromCharCode", nat_str_fromcharcode, 1);
    js_method(J, string, "fromCodePoint", nat_str_fromcodepoint, 1);
    js_method(J, string, "raw", nat_str_raw, 1);
    struct { const char *n; jnative f; int k; } M[] = {
        { "charAt", nat_str_charat, 1 }, { "charCodeAt", nat_str_charcode, 1 },
        { "codePointAt", nat_str_codepoint, 1 }, { "indexOf", nat_str_indexof, 1 },
        { "lastIndexOf", nat_str_lastindexof, 1 }, { "includes", nat_str_includes, 1 },
        { "startsWith", nat_str_startswith, 1 }, { "endsWith", nat_str_endswith, 1 },
        { "slice", nat_str_slice, 2 }, { "substring", nat_str_substring, 2 },
        { "substr", nat_str_substr, 2 }, { "at", nat_str_at, 1 }, { "concat", nat_str_concat, 1 },
        { "padStart", nat_str_padstart, 1 }, { "padEnd", nat_str_padend, 1 },
        { "toUpperCase", nat_str_upper, 0 }, { "toLowerCase", nat_str_lower, 0 },
        { "toLocaleUpperCase", nat_str_upper, 0 }, { "toLocaleLowerCase", nat_str_lower, 0 },
        { "trim", nat_str_trim, 0 }, { "trimStart", nat_str_trimstart, 0 },
        { "trimEnd", nat_str_trimend, 0 }, { "trimLeft", nat_str_trimstart, 0 },
        { "trimRight", nat_str_trimend, 0 }, { "split", nat_str_split, 2 },
        { "replace", nat_str_replace, 2 }, { "replaceAll", nat_str_replaceall, 2 },
        { "repeat", nat_str_repeat, 1 }, { "match", nat_str_match, 1 },
        { "matchAll", nat_str_matchall, 1 }, { "search", nat_str_search, 1 },
        { "localeCompare", nat_str_localecompare, 1 }, { "normalize", nat_str_normalize, 0 },
        { "toString", nat_str_valueof, 0 }, { "valueOf", nat_str_valueof, 0 },
        { 0, 0, 0 }
    };
    for (int i = 0; M[i].n; i++) js_method(J, sp, M[i].n, M[i].f, M[i].k);
    js_method_key(J, sp, J->sym_iterator, "[Symbol.iterator]", nat_str_iterator, 0);
    J->p_string_iter = js_object_with(J, JO_PLAIN, J->p_iterator);
    js_method(J, J->p_string_iter, "next", nat_striter_next, 0);
    js_tag(J, J->p_string_iter, "String Iterator");
}

static void js_setup_regexps(jctx *J) {
    J->p_regexp = js_object_with(J, JO_PLAIN, J->p_object);
    jobj *rp = J->p_regexp;
    js_ctor(J, "RegExp", nat_regexp_make, 2, rp);
    js_method(J, rp, "exec", nat_re_exec, 1);
    js_method(J, rp, "test", nat_re_test, 1);
    js_method(J, rp, "toString", nat_re_tostring, 0);
    js_getter(J, rp, "global", nat_re_global);
    js_getter(J, rp, "ignoreCase", nat_re_icase);
    js_getter(J, rp, "multiline", nat_re_multiline);
    js_getter(J, rp, "sticky", nat_re_sticky);
    js_getter(J, rp, "unicode", nat_re_unicode);
    js_getter(J, rp, "dotAll", nat_re_dotall);
    js_getter(J, rp, "hasIndices", nat_re_hasindices);
    js_getter(J, rp, "source", nat_re_source);
    js_getter(J, rp, "flags", nat_re_flags);
    J->p_regexp_iter = js_object_with(J, JO_PLAIN, J->p_iterator);
    js_method(J, J->p_regexp_iter, "next", nat_regexpiter_next, 0);
    js_tag(J, J->p_regexp_iter, "RegExp String Iterator");
}
