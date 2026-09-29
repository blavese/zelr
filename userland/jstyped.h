/* ArrayBuffer, the typed arrays and DataView: bytes a script can hold.
 *
 * Included from jslib.h. A buffer is bytes in the page's region; a typed
 * array is a view of some of them as numbers of one kind -- eight bit, sixteen,
 * thirty two, a float or a double -- read and written in the machine's own
 * order, which on this machine is little end first, as it is everywhere a
 * page is likely to run. Hashing, decoding pictures and text, reading binary
 * formats, the loaders big sites ship: all of it is written with these, and a
 * page that met Uint8Array here stopped at its first line. And what turns
 * bytes into text and back: atob, btoa, TextEncoder and TextDecoder.
 *
 * What is not here: the BigInt kinds, a buffer that can be resized or handed
 * to another thread, Atomics, and text in encodings other than UTF-8,
 * windows-1252 and UTF-16LE.
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

/* --- text and base64 -----------------------------------------------------------
 *
 * atob and btoa, TextEncoder and TextDecoder. Nothing in them needs a
 * document, and the browsers and their workers all have them on the global
 * object, which is where pages look: Google's search page decodes the
 * program it runs with atob, and without it that program stopped where
 * nothing said so.
 *
 * This engine's strings are UTF-8 bytes, indexed by byte. A "binary string"
 * -- a character for each byte, which is what atob gives and btoa takes -- is
 * therefore kept as those bytes themselves: charCodeAt(i) over what atob gave
 * is then the i-th byte, and length is the count of bytes, which is what the
 * code that calls atob does with it. */

/* A thrown error with a DOMException's name, which is what these throw in a
   browser; there is no DOMException here, and code that catches one reads
   its name or nothing. */
static jval js_throw_dom(jctx *J, const char *name, const char *what) {
    jobj *e = js_error_with(J, J->p_error, js_str(J, name), js_str(J, what));
    if (!e) return js_undef();
    js_put_prop_flags(J, e, J->s_name, js_from_str(js_str(J, name)), JP_WRITE | JP_CONF);
    J->ret = js_from_obj(e);
    J->sig = JS_THROWN;
    js_note_thrown(J, J->ret, J->error_line);
    return js_undef();
}

/* One character of UTF-8 at p, n bytes there: its length and *cp when it is
   well formed, 0 when it is cut short by the end, and minus the number of
   bytes one replacement character stands for when it is not UTF-8 (the
   standard's "maximal subpart": the lead and whatever continued it). */
static int tx_char(const u8 *p, u32 n, u32 *cp) {
    u8 c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    int need;
    u8 lo = 0x80, hi = 0xBF;
    u32 v;
    if (c >= 0xC2 && c <= 0xDF) { need = 1; v = c & 0x1Fu; }
    else if (c >= 0xE0 && c <= 0xEF) {
        need = 2; v = c & 0x0Fu;
        if (c == 0xE0) lo = 0xA0;
        if (c == 0xED) hi = 0x9F;         /* not the halves of a pair */
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3; v = c & 0x07u;
        if (c == 0xF0) lo = 0x90;
        if (c == 0xF4) hi = 0x8F;
    } else return -1;
    for (int k = 1; k <= need; k++) {
        if ((u32)k >= n) return 0;
        u8 d = p[k];
        if (d < lo || d > hi) return -k;
        lo = 0x80; hi = 0xBF;
        v = (v << 6) | (d & 0x3Fu);
    }
    *cp = v;
    return need + 1;
}

static int tx_b64(u8 c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* atob: the standard's forgiving decode -- spaces anywhere, the padding
   optional, and anything else outside the alphabet refused. */
static jval nat_atob(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (n < 1) return js_throw(J, JS_ERR_TYPE, "atob needs something to decode", J->error_line);
    jstr *s = js_to_str(J, a[0]);
    if (J->sig != JS_OK || !s) return js_undef();
    u8 *clean = (u8 *)malloc(s->len + 1);
    if (!clean) { js_out_of_memory(J); return js_undef(); }
    u32 m = 0;
    for (u32 i = 0; i < s->len; i++) {
        u8 c = (u8)s->s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 12) continue;
        clean[m++] = c;
    }
    if (m % 4 == 0 && m && clean[m - 1] == '=') { m--; if (clean[m - 1] == '=') m--; }
    int ok = m % 4 != 1;
    for (u32 i = 0; ok && i < m; i++) if (tx_b64(clean[i]) < 0) ok = 0;
    if (!ok) {
        free(clean);
        return js_throw_dom(J, "InvalidCharacterError", "that is not base64");
    }
    u32 bits = 0, have = 0, w = 0;
    for (u32 i = 0; i < m; i++) {
        bits = (bits << 6) | (u32)tx_b64(clean[i]);
        have += 6;
        if (have >= 8) { have -= 8; clean[w++] = (u8)(bits >> have); }
    }
    jstr *r = js_str_n(J, (const char *)clean, w);
    free(clean);
    return r ? js_from_str(r) : js_undef();
}

/* btoa. A string that is UTF-8 with every character at or under U+00FF is
   those characters, one byte each, as String.fromCharCode made them; one
   that is not UTF-8 at all is taken as the bytes it is, as atob gave them;
   a character past U+00FF cannot be said in a byte and is refused. */
static jval nat_btoa(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (n < 1) return js_throw(J, JS_ERR_TYPE, "btoa needs something to encode", J->error_line);
    jstr *s = js_to_str(J, a[0]);
    if (J->sig != JS_OK || !s) return js_undef();
    const u8 *p = (const u8 *)s->s;
    int whole = 1, wide = 0;
    for (u32 i = 0; i < s->len;) {
        u32 cp;
        int k = tx_char(p + i, s->len - i, &cp);
        if (k <= 0) { whole = 0; break; }
        if (cp > 0xFF) wide = 1;
        i += (u32)k;
    }
    if (whole && wide)
        return js_throw_dom(J, "InvalidCharacterError", "btoa takes characters up to U+00FF only");
    u8 *bytes = (u8 *)malloc(s->len + 1);
    char *out = (char *)malloc((s->len + 2) / 3 * 4 + 4);
    if (!bytes || !out) { free(bytes); free(out); js_out_of_memory(J); return js_undef(); }
    u32 nb = 0;
    if (whole) {
        for (u32 i = 0; i < s->len;) {
            u32 cp;
            i += (u32)tx_char(p + i, s->len - i, &cp);
            bytes[nb++] = (u8)cp;
        }
    } else {
        for (u32 i = 0; i < s->len; i++) bytes[nb++] = p[i];
    }
    static const char AL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    u32 w = 0;
    for (u32 i = 0; i < nb; i += 3) {
        u32 v = (u32)bytes[i] << 16;
        if (i + 1 < nb) v |= (u32)bytes[i + 1] << 8;
        if (i + 2 < nb) v |= bytes[i + 2];
        out[w++] = AL[(v >> 18) & 63];
        out[w++] = AL[(v >> 12) & 63];
        out[w++] = i + 1 < nb ? AL[(v >> 6) & 63] : '=';
        out[w++] = i + 2 < nb ? AL[v & 63] : '=';
    }
    jstr *r = js_str_n(J, out, w);
    free(bytes);
    free(out);
    return r ? js_from_str(r) : js_undef();
}

/* The bytes a buffer, a typed array or a DataView looks at; 0 when the value
   is none of them. */
static int tx_bytes_of(jval v, const u8 **p, u32 *n) {
    if (!js_is_obj(v)) return 0;
    jobj *o = v.obj;
    if (o->kind == JO_BUFFER) { *p = ta_bytes(o); *n = ta_buflen(o); return 1; }
    if ((o->kind == JO_TYPED || o->kind == JO_VIEW) && o->internal) {
        jtyped *x = (jtyped *)o->internal;
        *p = ta_bytes(x->buf) + x->off;
        *n = o->kind == JO_TYPED ? x->len * TA_SIZE[x->type] : x->len;
        return 1;
    }
    return 0;
}

static jval nat_textenc_make(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "a TextEncoder is made with new", J->error_line);
    return t;
}

static jval nat_utf8_name(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    return js_from_str(js_str(J, "utf-8"));
}

/* Copies a string's UTF-8 to d, whole characters only, as far as cap
   allows, with the lone half of a pair (kept here as the three bytes it
   would be) written as U+FFFD, as the standard's encoder does. *units is
   how many UTF-16 units were read, which encodeInto reports. */
static u32 tx_encode(const jstr *s, u8 *d, u32 cap, u32 *units) {
    u32 w = 0, i = 0, u = 0;
    const u8 *p = (const u8 *)s->s;
    while (i < s->len) {
        u32 k = js_utf8_len(p[i]);
        if (i + k > s->len) k = s->len - i;
        if (w + k > cap) break;
        if (k == 3 && p[i] == 0xED && i + 1 < s->len && p[i + 1] >= 0xA0) {
            d[w] = 0xEF; d[w + 1] = 0xBF; d[w + 2] = 0xBD;
        } else {
            for (u32 j = 0; j < k; j++) d[w + j] = p[i + j];
        }
        w += k;
        i += k;
        u += k == 4 ? 2 : 1;
    }
    if (units) *units = u;
    return w;
}

static jval nat_textenc_encode(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = n > 0 && a[0].t != JS_UNDEF ? js_to_str(J, a[0]) : js_str(J, "");
    if (J->sig != JS_OK || !s) return js_undef();
    jobj *r = ta_alloc(J, TA_U8, s->len);
    if (!r) return js_undef();
    tx_encode(s, ta_bytes(((jtyped *)r->internal)->buf), s->len, 0);
    return js_from_obj(r);
}

static jval nat_textenc_into(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (J->sig != JS_OK || !s) return js_undef();
    jval d = js_arg(a, n, 1);
    if (!js_is_obj(d) || d.obj->kind != JO_TYPED || d.obj->spare != TA_U8)
        return js_throw(J, JS_ERR_TYPE, "encodeInto writes into a Uint8Array", J->error_line);
    jtyped *x = (jtyped *)d.obj->internal;
    u32 units = 0;
    u32 w = tx_encode(s, ta_bytes(x->buf) + x->off, x->len, &units);
    jobj *r = js_object(J, JO_PLAIN);
    if (!r) return js_undef();
    js_set(J, r, "read", js_num(units));
    js_set(J, r, "written", js_num(w));
    return js_from_obj(r);
}

/* A decoder's settings, and the start of a character a streamed decode
   ended in the middle of. */
enum { TX_UTF8, TX_1252, TX_UTF16LE };
typedef struct {
    u8 kind, fatal, ignore_bom, started, npend;
    u8 pend[4];
} jcodec;

/* windows-1252's 0x80 to 0x9F, which is what "latin1" and "ascii" mean to a
   browser; the rest of its bytes are the code points they look like. */
static const u16 TX_1252_HIGH[32] = {
    0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039,
    0x152, 0x8D, 0x17D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178
};

static int tx_label(const jstr *s) {
    char b[24];
    u32 w = 0, i = 0, e = s->len;
    while (i < e && (s->s[i] == ' ' || s->s[i] == '\t' || s->s[i] == '\n')) i++;
    while (e > i && (s->s[e - 1] == ' ' || s->s[e - 1] == '\t' || s->s[e - 1] == '\n')) e--;
    for (; i < e && w < sizeof(b) - 1; i++) {
        char c = s->s[i];
        b[w++] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    b[w] = 0;
    static const char *const U8[] = { "utf-8", "utf8", "unicode-1-1-utf-8", "unicode11utf8",
                                      "unicode20utf8", "x-unicode20utf8", 0 };
    static const char *const L1[] = { "windows-1252", "latin1", "iso-8859-1", "iso8859-1",
                                      "iso_8859-1", "l1", "ascii", "us-ascii", "cp1252",
                                      "x-cp1252", "cp819", "ibm819", "iso-ir-100", 0 };
    static const char *const U16[] = { "utf-16le", "utf-16", "ucs-2", "unicode", "csunicode",
                                       "iso-10646-ucs-2", "unicodefeff", 0 };
    for (int k = 0; U8[k]; k++) if (!strcmp(b, U8[k])) return TX_UTF8;
    for (int k = 0; L1[k]; k++) if (!strcmp(b, L1[k])) return TX_1252;
    for (int k = 0; U16[k]; k++) if (!strcmp(b, U16[k])) return TX_UTF16LE;
    return -1;
}

static jval nat_textdec_make(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "a TextDecoder is made with new", J->error_line);
    int kind = TX_UTF8;
    if (n > 0 && a[0].t != JS_UNDEF) {
        jstr *label = js_to_str(J, a[0]);
        if (J->sig != JS_OK || !label) return js_undef();
        kind = tx_label(label);
        if (kind < 0) return js_throw_named(J, JS_ERR_RANGE, "", label, " is not an encoding this browser reads");
    }
    jcodec *c = (jcodec *)js_alloc(J, (u32)sizeof(jcodec));
    if (!c || !js_is_obj(t)) return js_undef();
    c->kind = (u8)kind;
    jval opts = js_arg(a, n, 1);
    if (js_is_obj(opts)) {
        c->fatal = (u8)js_to_bool(js_get(J, opts, js_str(J, "fatal")));
        c->ignore_bom = (u8)js_to_bool(js_get(J, opts, js_str(J, "ignoreBOM")));
    }
    t.obj->kind = JO_CODEC;
    t.obj->internal = c;
    return t;
}

static jcodec *tx_this_codec(jctx *J, jval t) {
    if (js_is_obj(t) && t.obj->kind == JO_CODEC && t.obj->internal) return (jcodec *)t.obj->internal;
    js_throw(J, JS_ERR_TYPE, "this is not a TextDecoder", J->error_line);
    return 0;
}

static jval nat_textdec_encoding(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jcodec *c = tx_this_codec(J, t);
    static const char *const NAMES[] = { "utf-8", "windows-1252", "utf-16le" };
    return c ? js_from_str(js_str(J, NAMES[c->kind])) : js_undef();
}
static jval nat_textdec_fatal(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jcodec *c = tx_this_codec(J, t);
    return c ? js_bool(c->fatal) : js_undef();
}
static jval nat_textdec_ignorebom(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jcodec *c = tx_this_codec(J, t);
    return c ? js_bool(c->ignore_bom) : js_undef();
}

/* decode(bytes, { stream }): the bytes as text, a malformed one as U+FFFD
   (or a TypeError when the decoder was made fatal), and, while streaming,
   an unfinished character kept for the next call rather than replaced. */
static jval nat_textdec_decode(jctx *J, jval t, jval *a, int n) {
    jcodec *c = tx_this_codec(J, t);
    if (!c) return js_undef();
    const u8 *src = 0;
    u32 len = 0;
    jval in = js_arg(a, n, 0);
    if (in.t != JS_UNDEF && !tx_bytes_of(in, &src, &len))
        return js_throw(J, JS_ERR_TYPE, "decode reads an ArrayBuffer, a typed array or a DataView", J->error_line);
    jval opts = js_arg(a, n, 1);
    int stream = js_is_obj(opts) && js_to_bool(js_get(J, opts, js_str(J, "stream")));
    if (J->sig != JS_OK) return js_undef();

    /* What was held back, then the new bytes, in one run. */
    u32 total = c->npend + len;
    u8 *p = (u8 *)malloc(total + 1);
    char *out = (char *)malloc((u64)total * 3 + 8);
    if (!p || !out) { free(p); free(out); js_out_of_memory(J); return js_undef(); }
    for (u32 i = 0; i < c->npend; i++) p[i] = c->pend[i];
    for (u32 i = 0; i < len; i++) p[c->npend + i] = src[i];
    c->npend = 0;

    u32 i = 0, w = 0;
    int bad = 0;
    if (!c->started && !c->ignore_bom) {
        if (c->kind == TX_UTF8 && total >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) i = 3;
        if (c->kind == TX_UTF16LE && total >= 2 && p[0] == 0xFF && p[1] == 0xFE) i = 2;
    }
    /* A BOM cut in half by the stream is waited for, not decoded. */
    int held = 0;
    if (!c->started && !c->ignore_bom && stream && i == 0 && total < 3
        && ((c->kind == TX_UTF8 && total && p[0] == 0xEF && (total < 2 || p[1] == 0xBB))
            || (c->kind == TX_UTF16LE && total == 1 && p[0] == 0xFF))) {
        for (u32 k = 0; k < total; k++) c->pend[k] = p[k];
        c->npend = (u8)total;
        held = 1;
        i = total;
    }
    if (!held && total) c->started = 1;

    while (i < total) {
        u32 cp;
        if (c->kind == TX_1252) {
            u8 b = p[i++];
            cp = b >= 0x80 && b < 0xA0 ? TX_1252_HIGH[b - 0x80] : b;
        } else if (c->kind == TX_UTF16LE) {
            if (i + 1 >= total) {
                if (stream) { c->pend[0] = p[i]; c->npend = 1; break; }
                i++;
                bad = 1;
                cp = 0xFFFD;
            } else {
                u32 unit = (u32)p[i] | ((u32)p[i + 1] << 8);
                if (unit >= 0xD800 && unit < 0xDC00) {
                    if (i + 3 >= total) {
                        if (stream) {
                            for (u32 k = i; k < total; k++) c->pend[c->npend++] = p[k];
                            break;
                        }
                        i = total;
                        bad = 1;
                        cp = 0xFFFD;
                    } else {
                        u32 lo = (u32)p[i + 2] | ((u32)p[i + 3] << 8);
                        if (lo >= 0xDC00 && lo < 0xE000) {
                            cp = 0x10000 + ((unit - 0xD800) << 10) + (lo - 0xDC00);
                            i += 4;
                        } else { cp = 0xFFFD; bad = 1; i += 2; }
                    }
                } else if (unit >= 0xDC00 && unit < 0xE000) { cp = 0xFFFD; bad = 1; i += 2; }
                else { cp = unit; i += 2; }
            }
        } else {
            int k = tx_char(p + i, total - i, &cp);
            if (k > 0) i += (u32)k;
            else if (k == 0) {
                if (stream) {
                    for (u32 q = i; q < total; q++) c->pend[c->npend++] = p[q];
                    break;
                }
                i = total;
                cp = 0xFFFD;
                bad = 1;
            } else { i += (u32)-k; cp = 0xFFFD; bad = 1; }
        }
        if (bad && c->fatal) break;
        w += js_utf8(cp, out + w);
    }
    free(p);
    if (!stream && !c->npend) c->started = 0;
    if (bad && c->fatal) {
        free(out);
        c->npend = 0;
        c->started = 0;
        return js_throw(J, JS_ERR_TYPE, "the data is not well-formed text in that encoding", J->error_line);
    }
    jstr *r = js_str_n(J, out, w);
    free(out);
    return r ? js_from_str(r) : js_undef();
}

static void js_setup_text(jctx *J) {
    struct { const char *n; jnative f; } G[] = { { "atob", nat_atob }, { "btoa", nat_btoa }, { 0, 0 } };
    for (int i = 0; G[i].n; i++) {
        jobj *f = js_method(J, 0, G[i].n, G[i].f, 1);
        js_declare_flags(J, J->global, js_str(J, G[i].n), js_from_obj(f), JP_WRITE | JP_CONF);
    }

    jobj *ep = js_object_with(J, JO_PLAIN, J->p_object);
    js_ctor(J, "TextEncoder", nat_textenc_make, 0, ep);
    js_getter(J, ep, "encoding", nat_utf8_name);
    js_method(J, ep, "encode", nat_textenc_encode, 0);
    js_method(J, ep, "encodeInto", nat_textenc_into, 2);
    js_tag(J, ep, "TextEncoder");

    jobj *dp = js_object_with(J, JO_PLAIN, J->p_object);
    js_ctor(J, "TextDecoder", nat_textdec_make, 0, dp);
    js_getter(J, dp, "encoding", nat_textdec_encoding);
    js_getter(J, dp, "fatal", nat_textdec_fatal);
    js_getter(J, dp, "ignoreBOM", nat_textdec_ignorebom);
    js_method(J, dp, "decode", nat_textdec_decode, 0);
    js_tag(J, dp, "TextDecoder");
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
