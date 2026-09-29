/* ArrayBuffer, the typed arrays and DataView: bytes a script can hold.
 *
 * Included from jslib.h. A buffer is bytes in the page's region; a typed
 * array is a view of some of them as numbers of one kind -- eight bit, sixteen,
 * thirty two, a float or a double -- read and written in the machine's own
 * order, which on this machine is little end first, as it is everywhere a
 * page is likely to run. Hashing, decoding pictures and text, reading binary
 * formats, the loaders big sites ship: all of it is written with these, and a
 * page that met Uint8Array here stopped at its first line.
 *
 * What is not here: the BigInt kinds, a buffer that can be resized or handed
 * to another thread, and Atomics.
 */
#pragma once

enum { TA_I8 = 0, TA_U8, TA_U8C, TA_I16, TA_U16, TA_I32, TA_U32, TA_F32, TA_F64, TA_KINDS };
static const char *const TA_NAMES[TA_KINDS] = {
    "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
    "Int32Array", "Uint32Array", "Float32Array", "Float64Array"
};
static const u8 TA_SIZE[TA_KINDS] = { 1, 1, 1, 2, 2, 4, 4, 4, 8 };

/* What a typed array or a DataView looks at. */
typedef struct {
    jobj *buf;
    u32   off, len;               /* len in elements (bytes for a view) */
    u8    type;
} jtyped;


static u8 *ta_bytes(jobj *buf) { return buf ? (u8 *)buf->internal : 0; }
static u32 ta_buflen(jobj *buf) { return buf ? (u32)buf->ival.num : 0; }

static u32 js_ta_length(jobj *o) {
    jtyped *t = (jtyped *)o->internal;
    return t ? t->len : 0;
}

static jval js_ta_get(jctx *J, jobj *o, u32 i) {
    (void)J;
    jtyped *t = (jtyped *)o->internal;
    if (!t || i >= t->len) return js_undef();
    u8 *p = ta_bytes(t->buf) + t->off + i * TA_SIZE[t->type];
    switch (t->type) {
        case TA_I8:  return js_num((double)(signed char)p[0]);
        case TA_U8: case TA_U8C: return js_num((double)p[0]);
        case TA_I16: return js_num((double)(short)(p[0] | (p[1] << 8)));
        case TA_U16: return js_num((double)(u16)(p[0] | (p[1] << 8)));
        case TA_I32: return js_num((double)(int)((u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24)));
        case TA_U32: return js_num((double)((u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24)));
        case TA_F32: {
            union { u32 u; float f; } x;
            x.u = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
            return js_num((double)x.f);
        }
        default: {
            union { u64 u; double d; } x;
            x.u = 0;
            for (int k = 7; k >= 0; k--) x.u = (x.u << 8) | p[k];
            return js_num(x.d);
        }
    }
}

/* A number into bytes of one kind, the way the standard converts: modulo for
   the whole kinds, clamped and rounded to even for Uint8Clamped. */
static void ta_store(int type, u8 *p, double d) {
    if (type == TA_F32) {
        union { u32 u; float f; } x;
        x.f = (float)d;
        for (int k = 0; k < 4; k++) p[k] = (u8)(x.u >> (8 * k));
        return;
    }
    if (type == TA_F64) {
        union { u64 u; double d; } x;
        x.d = d;
        for (int k = 0; k < 8; k++) p[k] = (u8)(x.u >> (8 * k));
        return;
    }
    if (type == TA_U8C) {
        if (!(d > 0)) { p[0] = 0; return; }
        if (d >= 255) { p[0] = 255; return; }
        double f = js_floor(d);
        double r = d - f;
        u32 v = (u32)f;
        if (r > 0.5 || (r == 0.5 && (v & 1))) v++;
        p[0] = (u8)v;
        return;
    }
    u32 v = (u32)js_d_to_i32(d);
    for (int k = 0; k < TA_SIZE[type]; k++) p[k] = (u8)(v >> (8 * k));
}

static void js_ta_set(jctx *J, jobj *o, u32 i, jval v) {
    jtyped *t = (jtyped *)o->internal;
    double d = js_to_num(J, v);
    if (J->sig != JS_OK || !t || i >= t->len) return;
    ta_store(t->type, ta_bytes(t->buf) + t->off + i * TA_SIZE[t->type], d);
}

/* A buffer of n bytes, all nought. */
static jobj *ta_new_buffer(jctx *J, jobj *proto, double n) {
    if (!(n >= 0) || n > (double)(JS_MEM_CAP / 2) || n != js_trunc(n)) {
        js_throw(J, JS_ERR_RANGE, "that is not a length a buffer can have", J->error_line);
        return 0;
    }
    jobj *b = js_object_with(J, JO_BUFFER, proto ? proto : J->p_buffer);
    if (!b) return 0;
    u8 *data = (u8 *)js_alloc(J, (u32)n ? (u32)n : 1);
    if (!data) return 0;
    b->internal = data;
    b->ival = js_num(n);
    return b;
}

static jobj *ta_make(jctx *J, int type, jobj *buf, u32 off, u32 len, jobj *proto) {
    jobj *o = js_object_with(J, JO_TYPED, proto ? proto : J->p_typed[type]);
    jtyped *t = (jtyped *)js_alloc(J, (u32)sizeof(jtyped));
    if (!o || !t) return 0;
    t->buf = buf;
    t->off = off;
    t->len = len;
    t->type = (u8)type;
    o->internal = t;
    o->spare = (u16)type;
    return o;
}

/* A fresh typed array of a kind and a length, with a buffer of its own. */
static jobj *ta_alloc(jctx *J, int type, u32 len) {
    jobj *buf = ta_new_buffer(J, 0, (double)len * TA_SIZE[type]);
    return buf ? ta_make(J, type, buf, 0, len, 0) : 0;
}

/* new Uint8Array(length | typedArray | arrayLike | iterable | buffer[, offset[, length]]) */
static jval nat_ta_make(jctx *J, jval t, jval *a, int n) {
    jobj *self = J->callee;
    if (J->new_target.t == JS_UNDEF)
        return js_throw(J, JS_ERR_TYPE, "a typed array is made with new", J->error_line);
    int type = self && self->data.t == JS_NUM ? (int)self->data.num : TA_U8;
    jobj *proto = js_is_obj(t) ? t.obj->proto : J->p_typed[type];
    jval src = js_arg(a, n, 0);
    int sz = TA_SIZE[type];
    if (!js_is_obj(src)) {
        double len = src.t == JS_UNDEF ? 0 : js_trunc(js_to_num(J, src));
        if (J->sig != JS_OK) return js_undef();
        if (len < 0 || len > (double)(JS_MEM_CAP / 2) / sz)
            return js_throw(J, JS_ERR_RANGE, "that is not a length a typed array can have", J->error_line);
        jobj *buf = ta_new_buffer(J, 0, len * sz);
        return buf ? js_from_obj(ta_make(J, type, buf, 0, (u32)len, proto)) : js_undef();
    }
    jobj *so = src.obj;
    if (so->kind == JO_BUFFER) {
        u32 blen = ta_buflen(so);
        double off = n > 1 ? js_trunc(js_to_num(J, a[1])) : 0;
        if (off < 0 || off > blen || ((u32)off % (u32)sz))
            return js_throw(J, JS_ERR_RANGE, "that offset does not fit the buffer", J->error_line);
        double len;
        if (n > 2 && a[2].t != JS_UNDEF) {
            len = js_trunc(js_to_num(J, a[2]));
            if (len < 0 || off + len * sz > blen)
                return js_throw(J, JS_ERR_RANGE, "that length does not fit the buffer", J->error_line);
        } else {
            if ((blen - (u32)off) % (u32)sz)
                return js_throw(J, JS_ERR_RANGE, "the buffer is not a whole number of elements", J->error_line);
            len = (blen - (u32)off) / sz;
        }
        return js_from_obj(ta_make(J, type, so, (u32)off, (u32)len, proto));
    }
    /* Anything else is copied: another typed array, an iterable, or
       something with a length. */
    jargs A;
    js_args_init(&A);
    jval m = js_get(J, src, J->sym_iterator);
    if (J->sig != JS_OK) return js_undef();
    if (so->kind == JO_TYPED) {
        u32 len = js_ta_length(so);
        for (u32 i = 0; i < len; i++) js_args_push(J, &A, js_ta_get(J, so, i));
    } else if (js_callable(m)) {
        if (!js_iter_collect(J, src, &A)) { js_args_free(&A); return js_undef(); }
    } else {
        u32 len = js_len_of(J, src);
        for (u32 i = 0; i < len && J->sig == JS_OK; i++) js_args_push(J, &A, js_get_index(J, src, i));
    }
    jobj *buf = ta_new_buffer(J, 0, (double)A.n * sz);
    jobj *o = buf ? ta_make(J, type, buf, 0, (u32)A.n, proto) : 0;
    for (int i = 0; o && i < A.n && J->sig == JS_OK; i++) js_ta_set(J, o, (u32)i, A.v[i]);
    js_args_free(&A);
    return J->sig == JS_OK ? js_from_obj(o) : js_undef();
}

static jobj *js_this_ta(jctx *J, jval t) {
    if (js_is_obj(t) && t.obj->kind == JO_TYPED && t.obj->internal) return t.obj;
    js_throw(J, JS_ERR_TYPE, "this is not a typed array", J->error_line);
    return 0;
}

static jval nat_ta_buffer(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *o = js_this_ta(J, t);
    return o ? js_from_obj(((jtyped *)o->internal)->buf) : js_undef();
}
static jval nat_ta_bytelength(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *o = js_this_ta(J, t);
    if (!o) return js_undef();
    jtyped *x = (jtyped *)o->internal;
    return js_num((double)x->len * TA_SIZE[x->type]);
}
static jval nat_ta_byteoffset(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *o = js_this_ta(J, t);
    return o ? js_num(((jtyped *)o->internal)->off) : js_undef();
}
static jval nat_ta_length(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *o = js_this_ta(J, t);
    return o ? js_num(js_ta_length(o)) : js_undef();
}

/* set(source, offset): another array's values copied in from offset on. */
static jval nat_ta_set(jctx *J, jval t, jval *a, int n) {
    jobj *o = js_this_ta(J, t);
    if (!o) return js_undef();
    jval src = js_arg(a, n, 0);
    double off = n > 1 ? js_trunc(js_to_num(J, a[1])) : 0;
    u32 len = js_is_obj(src) && src.obj->kind == JO_TYPED ? js_ta_length(src.obj) : js_len_of(J, src);
    if (off < 0 || off + len > js_ta_length(o))
        return js_throw(J, JS_ERR_RANGE, "that does not fit in the typed array", J->error_line);
    /* Read first, then write, in case the two share a buffer. */
    jargs A;
    js_args_init(&A);
    for (u32 i = 0; i < len && J->sig == JS_OK; i++)
        js_args_push(J, &A, src.obj && src.obj->kind == JO_TYPED ? js_ta_get(J, src.obj, i) : js_get_index(J, src, i));
    for (int i = 0; i < A.n && J->sig == JS_OK; i++) js_ta_set(J, o, (u32)off + (u32)i, A.v[i]);
    js_args_free(&A);
    return js_undef();
}

/* subarray: a view of the same bytes; slice: a copy of them. */
static jval js_ta_part(jctx *J, jval t, jval *a, int n, int copy) {
    jobj *o = js_this_ta(J, t);
    if (!o) return js_undef();
    jtyped *x = (jtyped *)o->internal;
    u32 len = x->len;
    u32 from = js_rel_index(J, js_arg(a, n, 0), len, 0);
    u32 to = js_rel_index(J, js_arg(a, n, 1), len, len);
    u32 count = to > from ? to - from : 0;
    if (!copy) return js_from_obj(ta_make(J, x->type, x->buf, x->off + from * TA_SIZE[x->type], count, o->proto));
    jobj *r = ta_alloc(J, x->type, count);
    if (!r) return js_undef();
    u8 *d = ta_bytes(((jtyped *)r->internal)->buf);
    u8 *s = ta_bytes(x->buf) + x->off + from * TA_SIZE[x->type];
    volatile u8 *vd = d;
    for (u32 i = 0; i < count * TA_SIZE[x->type]; i++) vd[i] = s[i];
    return js_from_obj(r);
}
static jval nat_ta_subarray(jctx *J, jval t, jval *a, int n) { return js_ta_part(J, t, a, n, 0); }
static jval nat_ta_slice(jctx *J, jval t, jval *a, int n) { return js_ta_part(J, t, a, n, 1); }

/* map and filter make an array of the same kind, not a plain one. */
static jval nat_ta_map(jctx *J, jval t, jval *a, int n) {
    jobj *o = js_this_ta(J, t);
    if (!o) return js_undef();
    jval fn = js_arg(a, n, 0);
    if (!js_callable(fn)) return js_throw(J, JS_ERR_TYPE, "the callback is not a function", J->error_line);
    u32 len = js_ta_length(o);
    jobj *r = ta_alloc(J, ((jtyped *)o->internal)->type, len);
    for (u32 i = 0; r && i < len && J->sig == JS_OK; i++) {
        jval args[3] = { js_ta_get(J, o, i), js_num(i), t };
        js_ta_set(J, r, i, js_call(J, fn, js_arg(a, n, 1), args, 3));
    }
    return J->sig == JS_OK ? js_from_obj(r) : js_undef();
}

static jval nat_ta_filter(jctx *J, jval t, jval *a, int n) {
    jobj *o = js_this_ta(J, t);
    if (!o) return js_undef();
    jval fn = js_arg(a, n, 0);
    if (!js_callable(fn)) return js_throw(J, JS_ERR_TYPE, "the callback is not a function", J->error_line);
    u32 len = js_ta_length(o);
    jargs A;
    js_args_init(&A);
    for (u32 i = 0; i < len && J->sig == JS_OK; i++) {
        jval x = js_ta_get(J, o, i);
        jval args[3] = { x, js_num(i), t };
        if (js_to_bool(js_call(J, fn, js_arg(a, n, 1), args, 3))) js_args_push(J, &A, x);
    }
    jobj *r = J->sig == JS_OK ? ta_alloc(J, ((jtyped *)o->internal)->type, (u32)A.n) : 0;
    for (int i = 0; r && i < A.n; i++) js_ta_set(J, r, (u32)i, A.v[i]);
    js_args_free(&A);
    return r ? js_from_obj(r) : js_undef();
}

/* sort, by number rather than by text as an ordinary array sorts. */
static jval nat_ta_sort(jctx *J, jval t, jval *a, int n) {
    jobj *o = js_this_ta(J, t);
    if (!o) return js_undef();
    jval fn = js_arg(a, n, 0);
    u32 len = js_ta_length(o);
    if (len < 2) return t;
    jval *v = (jval *)malloc((u64)len * 2 * sizeof(jval));
    if (!v) { js_out_of_memory(J); return t; }
    for (u32 i = 0; i < len; i++) v[i] = js_ta_get(J, o, i);
    if (js_callable(fn)) {
        jsortctx S = { J, fn, 0 };
        js_merge_sort(&S, v, v + len, len);
    } else {
        /* numbers, with NaN last */
        for (u32 i = 1; i < len; i++) {
            jval x = v[i];
            u32 k = i;
            while (k > 0 && (v[k - 1].num > x.num || (v[k - 1].num != v[k - 1].num && x.num == x.num))) {
                v[k] = v[k - 1];
                k--;
            }
            v[k] = x;
        }
    }
    if (J->sig == JS_OK) for (u32 i = 0; i < len; i++) js_ta_set(J, o, i, v[i]);
    free(v);
    return t;
}

static jval nat_ta_tostring_tag(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t) || t.obj->kind != JO_TYPED) return js_undef();
    return js_from_str(js_str(J, TA_NAMES[t.obj->spare < TA_KINDS ? t.obj->spare : 0]));
}

/* Uint8Array.from and .of. */
static jval nat_ta_from(jctx *J, jval t, jval *a, int n) {
    if (!js_is_constructor(J, t)) return js_throw(J, JS_ERR_TYPE, "from needs a typed array constructor", J->error_line);
    jval arr = nat_array_from(J, js_undef(), a, n);
    if (J->sig != JS_OK) return js_undef();
    return js_construct(J, t, &arr, 1, t);
}

static jval nat_ta_of(jctx *J, jval t, jval *a, int n) {
    jval arr = nat_array_of(J, js_undef(), a, n);
    if (J->sig != JS_OK) return js_undef();
    return js_construct(J, t, &arr, 1, t);
}

/* --- ArrayBuffer ------------------------------------------------------------ */

static jval nat_buffer_make(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "an ArrayBuffer is made with new", J->error_line);
    double len = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (J->sig != JS_OK) return js_undef();
    jobj *b = ta_new_buffer(J, js_is_obj(t) ? t.obj->proto : J->p_buffer, len);
    return b ? js_from_obj(b) : js_undef();
}

static jobj *js_this_buffer(jctx *J, jval t) {
    if (js_is_obj(t) && t.obj->kind == JO_BUFFER) return t.obj;
    js_throw(J, JS_ERR_TYPE, "this is not an ArrayBuffer", J->error_line);
    return 0;
}

static jval nat_buffer_bytelength(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *b = js_this_buffer(J, t);
    return b ? js_num(ta_buflen(b)) : js_undef();
}

static jval nat_buffer_slice(jctx *J, jval t, jval *a, int n) {
    jobj *b = js_this_buffer(J, t);
    if (!b) return js_undef();
    u32 len = ta_buflen(b);
    u32 from = js_rel_index(J, js_arg(a, n, 0), len, 0);
    u32 to = js_rel_index(J, js_arg(a, n, 1), len, len);
    u32 count = to > from ? to - from : 0;
    jobj *r = ta_new_buffer(J, 0, count);
    if (!r) return js_undef();
    volatile u8 *d = ta_bytes(r);
    u8 *s = ta_bytes(b) + from;
    for (u32 i = 0; i < count; i++) d[i] = s[i];
    return js_from_obj(r);
}

static jval nat_buffer_isview(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t;
    jval v = js_arg(a, n, 0);
    return js_bool(js_is_obj(v) && (v.obj->kind == JO_TYPED || v.obj->kind == JO_VIEW));
}

/* --- DataView ------------------------------------------------------------------ */

static jval nat_view_make(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "a DataView is made with new", J->error_line);
    jval b = js_arg(a, n, 0);
    if (!js_is_obj(b) || b.obj->kind != JO_BUFFER)
        return js_throw(J, JS_ERR_TYPE, "a DataView looks at an ArrayBuffer", J->error_line);
    u32 blen = ta_buflen(b.obj);
    double off = n > 1 ? js_trunc(js_to_num(J, a[1])) : 0;
    double len = n > 2 && a[2].t != JS_UNDEF ? js_trunc(js_to_num(J, a[2])) : blen - off;
    if (off < 0 || len < 0 || off + len > blen)
        return js_throw(J, JS_ERR_RANGE, "that does not fit the buffer", J->error_line);
    jtyped *x = (jtyped *)js_alloc(J, (u32)sizeof(jtyped));
    if (!x || !js_is_obj(t)) return js_undef();
    x->buf = b.obj;
    x->off = (u32)off;
    x->len = (u32)len;
    x->type = TA_U8;
    t.obj->kind = JO_VIEW;
    t.obj->internal = x;
    return t;
}

/* get and set of every kind, with the byte order a DataView is asked for
   (big end first unless told little). `which` is the kind; `set` whether
   it writes. */
static jval js_view_access(jctx *J, jval t, jval *a, int n, int type, int set) {
    if (!js_is_obj(t) || t.obj->kind != JO_VIEW)
        return js_throw(J, JS_ERR_TYPE, "this is not a DataView", J->error_line);
    jtyped *x = (jtyped *)t.obj->internal;
    double at = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    int sz = TA_SIZE[type];
    if (at < 0 || at + sz > x->len) return js_throw(J, JS_ERR_RANGE, "that is past the end of the view", J->error_line);
    int little = js_to_bool(js_arg(a, n, set ? 2 : 1));
    u8 *p = ta_bytes(x->buf) + x->off + (u32)at;
    u8 tmp[8];
    if (set) {
        ta_store(type, tmp, js_to_num(J, js_arg(a, n, 1)));
        for (int k = 0; k < sz; k++) p[k] = little ? tmp[k] : tmp[sz - 1 - k];
        return js_undef();
    }
    for (int k = 0; k < sz; k++) tmp[k] = little ? p[k] : p[sz - 1 - k];
    /* Read back through a typed array of one element over the copy. */
    jtyped view = { 0, 0, 1, (u8)type };
    jobj fake;
    jobj holder;
    memset(&holder, 0, (int)sizeof(holder));
    holder.internal = tmp;
    holder.ival = js_num(8);
    view.buf = &holder;
    memset(&fake, 0, (int)sizeof(fake));
    fake.internal = &view;
    return js_ta_get(J, &fake, 0);
}

#define JS_VIEW(nm, ty) \
    static jval nat_view_get##nm(jctx *J, jval t, jval *a, int n) { return js_view_access(J, t, a, n, ty, 0); } \
    static jval nat_view_set##nm(jctx *J, jval t, jval *a, int n) { return js_view_access(J, t, a, n, ty, 1); }
JS_VIEW(int8, TA_I8) JS_VIEW(uint8, TA_U8) JS_VIEW(int16, TA_I16) JS_VIEW(uint16, TA_U16)
JS_VIEW(int32, TA_I32) JS_VIEW(uint32, TA_U32) JS_VIEW(float32, TA_F32) JS_VIEW(float64, TA_F64)

static jval nat_view_buffer(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t) || t.obj->kind != JO_VIEW) return js_throw(J, JS_ERR_TYPE, "this is not a DataView", J->error_line);
    return js_from_obj(((jtyped *)t.obj->internal)->buf);
}
static jval nat_view_bytelength(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t) || t.obj->kind != JO_VIEW) return js_throw(J, JS_ERR_TYPE, "this is not a DataView", J->error_line);
    return js_num(((jtyped *)t.obj->internal)->len);
}
static jval nat_view_byteoffset(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t) || t.obj->kind != JO_VIEW) return js_throw(J, JS_ERR_TYPE, "this is not a DataView", J->error_line);
    return js_num(((jtyped *)t.obj->internal)->off);
}

static void js_setup_typed(jctx *J) {
    J->p_buffer = js_object_with(J, JO_PLAIN, J->p_object);
    jobj *bufctor = js_ctor(J, "ArrayBuffer", nat_buffer_make, 1, J->p_buffer);
    js_getter(J, J->p_buffer, "byteLength", nat_buffer_bytelength);
    js_method(J, J->p_buffer, "slice", nat_buffer_slice, 2);
    js_method(J, bufctor, "isView", nat_buffer_isview, 1);
    js_tag(J, J->p_buffer, "ArrayBuffer");

    /* %TypedArray%: the prototype every kind shares, with the array methods
       that work on anything with a length, and its own where a copy has to
       be of the same kind. */
    jobj *tp = js_object_with(J, JO_PLAIN, J->p_object);
    jobj *tctor = js_native_n(J, "TypedArray", nat_ta_make, 0);
    if (tctor) {
        js_put_prop_flags(J, tctor, J->s_prototype, js_from_obj(tp), 0);
        js_put_prop_flags(J, tp, J->s_constructor, js_from_obj(tctor), JP_WRITE | JP_CONF);
        js_method(J, tctor, "from", nat_ta_from, 1);
        js_method(J, tctor, "of", nat_ta_of, 0);
    }
    js_getter(J, tp, "buffer", nat_ta_buffer);
    js_getter(J, tp, "byteLength", nat_ta_bytelength);
    js_getter(J, tp, "byteOffset", nat_ta_byteoffset);
    js_getter(J, tp, "length", nat_ta_length);
    {
        jobj *g = js_native(J, "get [Symbol.toStringTag]", nat_ta_tostring_tag);
        if (g) js_define_accessor(J, tp, J->sym_to_string_tag, js_from_obj(g), js_undef(), JP_CONF);
    }
    js_method(J, tp, "set", nat_ta_set, 1);
    js_method(J, tp, "subarray", nat_ta_subarray, 2);
    js_method(J, tp, "slice", nat_ta_slice, 2);
    js_method(J, tp, "map", nat_ta_map, 1);
    js_method(J, tp, "filter", nat_ta_filter, 1);
    js_method(J, tp, "sort", nat_ta_sort, 1);
    struct { const char *n; jnative f; int k; } M[] = {
        { "forEach", nat_arr_foreach, 1 }, { "indexOf", nat_arr_indexof, 1 },
        { "lastIndexOf", nat_arr_lastindexof, 1 }, { "includes", nat_arr_includes, 1 },
        { "join", nat_arr_join, 1 }, { "reverse", nat_arr_reverse, 0 }, { "some", nat_arr_some, 1 },
        { "every", nat_arr_every, 1 }, { "find", nat_arr_find, 1 }, { "findIndex", nat_arr_findindex, 1 },
        { "findLast", nat_arr_findlast, 1 }, { "findLastIndex", nat_arr_findlastindex, 1 },
        { "reduce", nat_arr_reduce, 1 }, { "reduceRight", nat_arr_reduceright, 1 },
        { "fill", nat_arr_fill, 1 }, { "copyWithin", nat_arr_copywithin, 2 }, { "at", nat_arr_at, 1 },
        { "keys", nat_arr_keys, 0 }, { "entries", nat_arr_entries, 0 },
        { "toString", nat_arr_tostring, 0 }, { "toLocaleString", nat_arr_tostring, 0 },
        { 0, 0, 0 }
    };
    for (int i = 0; M[i].n; i++) js_method(J, tp, M[i].n, M[i].f, M[i].k);
    jobj *values = js_method(J, tp, "values", nat_arr_values, 0);
    if (values) js_put_prop_flags(J, tp, J->sym_iterator, js_from_obj(values), JP_WRITE | JP_CONF);

    for (int k = 0; k < TA_KINDS; k++) {
        jobj *p = js_object_with(J, JO_PLAIN, tp);
        J->p_typed[k] = p;
        jobj *c = js_ctor(J, TA_NAMES[k], nat_ta_make, 3, p);
        if (!c) continue;
        c->data = js_num(k);
        if (tctor) c->proto = tctor;
        js_const_prop(J, c, "BYTES_PER_ELEMENT", js_num(TA_SIZE[k]));
        js_const_prop(J, p, "BYTES_PER_ELEMENT", js_num(TA_SIZE[k]));
    }

    J->p_view = js_object_with(J, JO_PLAIN, J->p_object);
    jobj *vp = J->p_view;
    js_ctor(J, "DataView", nat_view_make, 1, vp);
    struct { const char *n; jnative f; int k; } V[] = {
        { "getInt8", nat_view_getint8, 1 }, { "setInt8", nat_view_setint8, 2 },
        { "getUint8", nat_view_getuint8, 1 }, { "setUint8", nat_view_setuint8, 2 },
        { "getInt16", nat_view_getint16, 1 }, { "setInt16", nat_view_setint16, 2 },
        { "getUint16", nat_view_getuint16, 1 }, { "setUint16", nat_view_setuint16, 2 },
        { "getInt32", nat_view_getint32, 1 }, { "setInt32", nat_view_setint32, 2 },
        { "getUint32", nat_view_getuint32, 1 }, { "setUint32", nat_view_setuint32, 2 },
        { "getFloat32", nat_view_getfloat32, 1 }, { "setFloat32", nat_view_setfloat32, 2 },
        { "getFloat64", nat_view_getfloat64, 1 }, { "setFloat64", nat_view_setfloat64, 2 },
        { 0, 0, 0 }
    };
    for (int i = 0; V[i].n; i++) js_method(J, vp, V[i].n, V[i].f, V[i].k);
    js_getter(J, vp, "buffer", nat_view_buffer);
    js_getter(J, vp, "byteLength", nat_view_bytelength);
    js_getter(J, vp, "byteOffset", nat_view_byteoffset);
    js_tag(J, vp, "DataView");
}
