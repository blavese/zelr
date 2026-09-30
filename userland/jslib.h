/* The objects a script finds already there.
 *
 * Written as natives rather than in JavaScript. A bootstrap written in the
 * language itself is elegant and it means every one of these costs a parse
 * and a tree walk on every page load, for methods that are a loop each.
 *
 * Each lives where the language says it lives -- a string's methods on
 * String.prototype, an array's on Array.prototype -- so a page can find them,
 * borrow them (Array.prototype.slice.call(arguments)), test for them before
 * it uses them, and add its own beside them, which is what every polyfill
 * does. They were looked up by name in tables before there were prototypes,
 * and a page that asked String.prototype for anything was told it was not
 * there. This file is included from jsrun.h, and jsarr.h and jsprom.h from
 * this one.
 */
#pragma once

/* --- making natives -------------------------------------------------------- */

static jobj *js_native(jctx *J, const char *name, jnative fn) {
    jobj *o = js_object(J, JO_NATIVE);
    if (!o) return 0;
    o->fn = fn;
    o->name = js_str(J, name);
    return o;
}

static jobj *js_native_n(jctx *J, const char *name, jnative fn, int arity) {
    jobj *o = js_native(J, name, fn);
    if (o) o->spare = (u16)arity;
    return o;
}

static jval js_arg(jval *argv, int argc, int i) {
    return i < argc ? argv[i] : js_undef();
}

/* A method, as the built-in ones are: not walked by for-in, and not a
   constructor. */
static jobj *js_method(jctx *J, jobj *on, const char *name, jnative fn, int arity) {
    jobj *f = js_native_n(J, name, fn, arity);
    if (!f || !on) return f;
    f->flags |= JOF_NOCTOR;
    js_put_prop_flags(J, on, js_str(J, name), js_from_obj(f), JP_WRITE | JP_CONF);
    return f;
}

static jobj *js_method_key(jctx *J, jobj *on, jstr *key, const char *name, jnative fn, int arity) {
    jobj *f = js_native_n(J, name, fn, arity);
    if (!f || !on) return f;
    f->flags |= JOF_NOCTOR;
    js_put_prop_flags(J, on, key, js_from_obj(f), JP_WRITE | JP_CONF);
    return f;
}

static void js_getter(jctx *J, jobj *on, const char *name, jnative fn) {
    jobj *g = js_native(J, name, fn);
    if (!g) return;
    g->flags |= JOF_NOCTOR;
    js_define_accessor(J, on, js_str(J, name), js_from_obj(g), js_undef(), JP_CONF);
}

static void js_const_prop(jctx *J, jobj *on, const char *name, jval v) {
    js_put_prop_flags(J, on, js_str(J, name), v, 0);
}

/* A constructor, its prototype, and the name it goes by in the global
   scope. */
static jobj *js_ctor(jctx *J, const char *name, jnative fn, int arity, jobj *proto) {
    jobj *c = js_native_n(J, name, fn, arity);
    if (!c) return 0;
    if (proto) {
        js_put_prop_flags(J, c, J->s_prototype, js_from_obj(proto), 0);
        js_put_prop_flags(J, proto, J->s_constructor, js_from_obj(c), JP_WRITE | JP_CONF);
    }
    js_declare_flags(J, J->global, js_str(J, name), js_from_obj(c), JP_WRITE | JP_CONF);
    return c;
}

static void js_tag(jctx *J, jobj *on, const char *tag) {
    js_put_prop_flags(J, on, J->sym_to_string_tag, js_from_str(js_str(J, tag)), JP_CONF);
}

static jval js_iter_result(jctx *J, jval value, int done) {
    jobj *r = js_object(J, JO_PLAIN);
    if (!r) return js_undef();
    js_put_prop(J, r, J->s_value, value);
    js_put_prop(J, r, J->s_done, js_bool(done));
    return js_from_obj(r);
}

/* A value as an object, as Object(v) and the methods that need one make it:
   a primitive in a box, and null or undefined refused. */
static jobj *js_to_object(jctx *J, jval v) {
    if (js_is_obj(v)) return v.obj;
    if (v.t == JS_NULL || v.t == JS_UNDEF) {
        js_throw(J, JS_ERR_TYPE, v.t == JS_NULL ? "null cannot be made into an object"
                                                 : "undefined cannot be made into an object",
                 J->error_line);
        return 0;
    }
    jobj *b = js_object_with(J, JO_BOXED, js_proto_of_value(J, v));
    if (b) b->ival = v;
    return b;
}

/* The primitive in a box, or the value itself. */
static jval js_unbox(jval v) {
    if (js_is_obj(v) && v.obj->kind == JO_BOXED) return v.obj->ival;
    return v;
}

static jstr *js_this_str(jctx *J, jval t, const char *method) {
    if (t.t == JS_STR) return t.str;
    if (t.t == JS_NULL || t.t == JS_UNDEF) {
        char msg[96];
        int n = 0;
        for (const char *p = method; *p && n < 60; p++) msg[n++] = *p;
        for (const char *p = " called on nothing"; *p; p++) msg[n++] = *p;
        msg[n] = 0;
        js_throw(J, JS_ERR_TYPE, msg, J->error_line);
        return js_str(J, "");
    }
    return js_to_str(J, t);
}

static void js_names(jctx *J) {
    J->s_arguments = js_str(J, "arguments");
    J->s_length = js_str(J, "length");
    J->s_prototype = js_str(J, "prototype");
    J->s_constructor = js_str(J, "constructor");
    J->s_name = js_str(J, "name");
    J->s_message = js_str(J, "message");
    J->s_next = js_str(J, "next");
    J->s_done = js_str(J, "done");
    J->s_value = js_str(J, "value");
    J->s_then = js_str(J, "then");
    J->s_return = js_str(J, "return");
    J->s_throw = js_str(J, "throw");
    J->s_get = js_str(J, "get");
    J->s_set = js_str(J, "set");
    J->s_lastIndex = js_str(J, "lastIndex");
    J->s_index = js_str(J, "index");
    J->s_input = js_str(J, "input");
    J->s_toString = js_str(J, "toString");
    J->s_valueOf = js_str(J, "valueOf");
    J->s_toJSON = js_str(J, "toJSON");
    /* Names no script can write, for what a call keeps in its scope for
       `this`, super and new.target: `this` is a keyword, and the others have
       characters a name cannot. */
    J->s_this = js_str(J, "this");
    J->s_home = js_str(J, "%home");
    J->s_fnself = js_str(J, "%fn");
    J->s_newtarget = js_str(J, "new.target");
    J->s_stack = js_str(J, "stack");
    J->s_callee = js_str(J, "callee");
    J->s_groups = js_str(J, "groups");
    J->s_enumerable = js_str(J, "enumerable");
    J->s_configurable = js_str(J, "configurable");
    J->s_writable = js_str(J, "writable");
    J->s_cause = js_str(J, "cause");
    J->s_proto = js_str(J, "__proto__");
    J->sym_iterator = js_sym_new(J, "Symbol.iterator", 15);
    J->sym_async_iterator = js_sym_new(J, "Symbol.asyncIterator", 20);
    J->sym_has_instance = js_sym_new(J, "Symbol.hasInstance", 18);
    J->sym_to_primitive = js_sym_new(J, "Symbol.toPrimitive", 18);
    J->sym_to_string_tag = js_sym_new(J, "Symbol.toStringTag", 18);
    J->sym_species = js_sym_new(J, "Symbol.species", 14);
    J->sym_unscopables = js_sym_new(J, "Symbol.unscopables", 18);
    J->sym_match_all = js_sym_new(J, "Symbol.matchAll", 15);
}

/* --- Object ------------------------------------------------------------------ */

static jval nat_obj_valueof(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *o = js_to_object(J, t);
    return o ? js_from_obj(o) : js_undef();
}

static const char *js_class_of(jobj *o) {
    switch (o->kind) {
        case JO_ARRAY: return "Array";
        case JO_FUNC: case JO_NATIVE: return "Function";
        case JO_ERROR: return "Error";
        case JO_ARGS: return "Arguments";
        case JO_DATE: return "Date";
        case JO_REGEX: return "RegExp";
        case JO_BOXED:
            return o->ival.t == JS_STR ? "String" : o->ival.t == JS_NUM ? "Number"
                 : o->ival.t == JS_BOOL ? "Boolean" : "Object";
        default: return "Object";
    }
}

static jval nat_obj_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (t.t == JS_UNDEF) return js_from_str(js_str(J, "[object Undefined]"));
    if (t.t == JS_NULL) return js_from_str(js_str(J, "[object Null]"));
    jobj *o = js_to_object(J, t);
    if (!o) return js_undef();
    jval tag = js_get(J, js_from_obj(o), J->sym_to_string_tag);
    if (J->sig != JS_OK) return js_undef();
    const char *cls = js_class_of(o);
    char buf[128];
    int w = 0;
    for (const char *p = "[object "; *p; p++) buf[w++] = *p;
    if (tag.t == JS_STR) for (u32 i = 0; i < tag.str->len && w < 120; i++) buf[w++] = tag.str->s[i];
    else for (const char *p = cls; *p; p++) buf[w++] = *p;
    buf[w++] = ']';
    return js_from_str(js_str_n(J, buf, (u32)w));
}

static jval nat_obj_tolocalestring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jval m = js_get(J, t, J->s_toString);
    if (J->sig != JS_OK) return js_undef();
    return js_call(J, m, t, 0, 0);
}

static int js_has_own(jctx *J, jval t, jstr *key) {
    if (t.t == JS_STR) {
        u32 idx;
        return js_str_eq(key, J->s_length) || (js_index_of(key, &idx) && idx < t.str->len);
    }
    if (!js_is_obj(t)) return 0;
    jval v;
    int fl;
    if (js_get_own(J, t.obj, key, &v, &fl)) return 1;
    if (t.obj->host >= 0 && J->host_get && !js_is_sym_key(key)) {
        char buf[64];
        u32 k = key->len < 63 ? key->len : 63;
        for (u32 i = 0; i < k; i++) buf[i] = key->s[i];
        buf[k] = 0;
        return J->host_get(J, t.obj, buf, &v);
    }
    return 0;
}

static jval nat_obj_hasown_proto(jctx *J, jval t, jval *a, int n) {
    jstr *key = js_to_key(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    if (t.t == JS_NULL || t.t == JS_UNDEF)
        return js_throw(J, JS_ERR_TYPE, "hasOwnProperty called on nothing", J->error_line);
    return js_bool(js_has_own(J, t, key));
}

static jval nat_obj_isprototypeof(jctx *J, jval t, jval *a, int n) {
    jval v = js_arg(a, n, 0);
    if (!js_is_obj(v) || !js_is_obj(t)) return js_bool(0);
    int depth = 0;
    for (jobj *o = v.obj->proto; o && depth < 10000; o = o->proto, depth++)
        if (o == t.obj) return js_bool(1);
    (void)J;
    return js_bool(0);
}

static jval nat_obj_propisenum(jctx *J, jval t, jval *a, int n) {
    jstr *key = js_to_key(J, js_arg(a, n, 0));
    if (!js_is_obj(t)) return js_bool(t.t == JS_STR && !js_str_eq(key, J->s_length) && js_has_own(J, t, key));
    jval v;
    int fl;
    if (!js_get_own(J, t.obj, key, &v, &fl)) return js_bool(0);
    return js_bool(fl & JP_ENUM);
}

static jval nat_obj_proto_get(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *o = js_proto_of_value(J, t);
    if (!o) return js_undef();
    if (t.t != JS_OBJ) return js_from_obj(o);
    return o->proto ? js_from_obj(o->proto) : js_null();
}

/* Whether making `proto` o's prototype would make a ring. */
static int js_proto_loops(jobj *o, jobj *proto) {
    for (int depth = 0; proto && depth < 10000; proto = proto->proto, depth++)
        if (proto == o) return 1;
    return 0;
}

static jval nat_obj_proto_set(jctx *J, jval t, jval *a, int n) {
    jval p = js_arg(a, n, 0);
    if (!js_is_obj(t) || (p.t != JS_NULL && !js_is_obj(p))) return js_undef();
    jobj *np = p.t == JS_NULL ? 0 : p.obj;
    if (js_proto_loops(t.obj, np))
        return js_throw(J, JS_ERR_TYPE, "that prototype would make a ring", J->error_line);
    if (!(t.obj->flags & JOF_NOEXT)) t.obj->proto = np;
    return js_undef();
}

static jval nat_obj_define_getter(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return js_undef();
    jstr *key = js_to_key(J, js_arg(a, n, 0));
    js_define_accessor(J, t.obj, key, js_arg(a, n, 1), js_undef(), JP_ENUM | JP_CONF);
    return js_undef();
}

static jval nat_obj_define_setter(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return js_undef();
    jstr *key = js_to_key(J, js_arg(a, n, 0));
    js_define_accessor(J, t.obj, key, js_undef(), js_arg(a, n, 1), JP_ENUM | JP_CONF);
    return js_undef();
}

static jval nat_obj_lookup_accessor(jctx *J, jval t, jval *a, int n, int setter) {
    jstr *key = js_to_key(J, js_arg(a, n, 0));
    for (jobj *o = js_proto_of_value(J, t); o; o = o->proto) {
        jprop *p = js_find(o, key);
        if (!p) continue;
        if (p->v.t != JS_ACC) return js_undef();
        return setter ? p->v.acc->set : p->v.acc->get;
    }
    return js_undef();
}
static jval nat_obj_lookup_getter(jctx *J, jval t, jval *a, int n) { return nat_obj_lookup_accessor(J, t, a, n, 0); }
static jval nat_obj_lookup_setter(jctx *J, jval t, jval *a, int n) { return nat_obj_lookup_accessor(J, t, a, n, 1); }

/* Object(v): v as an object, and a fresh one for nothing. */
static jval nat_object_make(jctx *J, jval t, jval *a, int n) {
    jval v = js_arg(a, n, 0);
    if (J->new_target.t != JS_UNDEF && J->new_target.obj != J->c_object && js_is_obj(t)) return t;
    if (v.t == JS_NULL || v.t == JS_UNDEF) return js_from_obj(js_object(J, JO_PLAIN));
    jobj *o = js_to_object(J, v);
    return o ? js_from_obj(o) : js_undef();
}

/* The own keys of anything, as Object.keys, values and entries walk it:
   an array's indices, a string's positions, and the enumerable names. */
static jobj *js_own_enum(jctx *J, jval v, int what) {
    jobj *out = js_array(J);
    if (!out) return 0;
    if (v.t == JS_NULL || v.t == JS_UNDEF) {
        js_throw(J, JS_ERR_TYPE, "undefined or null has no keys", J->error_line);
        return out;
    }
    jval src = v;
    if (v.t == JS_STR || (js_is_obj(v) && v.obj->kind == JO_BOXED && v.obj->ival.t == JS_STR)) {
        jstr *s = v.t == JS_STR ? v.str : v.obj->ival.str;
        for (u32 i = 0; i < s->len; i++) {
            jval k = js_from_str(js_to_key(J, js_num(i)));
            jval c = js_from_str(js_str_n(J, s->s + i, 1));
            if (what == 0) js_arr_push(J, out, k);
            else if (what == 1) js_arr_push(J, out, c);
            else {
                jobj *pair = js_array(J);
                js_arr_push(J, pair, k);
                js_arr_push(J, pair, c);
                js_arr_push(J, out, js_from_obj(pair));
            }
        }
        if (v.t == JS_STR) return out;
    }
    if (!js_is_obj(src)) return out;
    jobj *o = src.obj;
    if (o->kind == JO_ARRAY || o->kind == JO_ARGS || o->kind == JO_TYPED) {
        u32 len = o->kind == JO_TYPED ? js_ta_length(o) : o->len;
        for (u32 i = 0; i < len; i++) {
            jval k = js_from_str(js_to_key(J, js_num(i)));
            jval v = o->kind == JO_TYPED ? js_ta_get(J, o, i) : o->items[i];
            if (what == 0) js_arr_push(J, out, k);
            else if (what == 1) js_arr_push(J, out, v);
            else {
                jobj *pair = js_array(J);
                js_arr_push(J, pair, k);
                js_arr_push(J, pair, v);
                js_arr_push(J, out, js_from_obj(pair));
            }
        }
    }
    jprop **own;
    u32 nown = js_own_keys(J, o, &own);
    for (u32 i = 0; i < nown && J->sig == JS_OK; i++) {
        jval k = js_from_str(own[i]->key);
        if (what == 0) { js_arr_push(J, out, k); continue; }
        jval val = js_prop_read(J, own[i], src);
        if (what == 1) js_arr_push(J, out, val);
        else {
            jobj *pair = js_array(J);
            js_arr_push(J, pair, k);
            js_arr_push(J, pair, val);
            js_arr_push(J, out, js_from_obj(pair));
        }
    }
    return out;
}

static jval nat_obj_keys(jctx *J, jval t, jval *a, int n) { (void)t; return js_from_obj(js_own_enum(J, js_arg(a, n, 0), 0)); }
static jval nat_obj_values(jctx *J, jval t, jval *a, int n) { (void)t; return js_from_obj(js_own_enum(J, js_arg(a, n, 0), 1)); }
static jval nat_obj_entries(jctx *J, jval t, jval *a, int n) { (void)t; return js_from_obj(js_own_enum(J, js_arg(a, n, 0), 2)); }

static jval nat_obj_assign(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *to = js_to_object(J, js_arg(a, n, 0));
    if (!to) return js_undef();
    jval tv = js_from_obj(to);
    for (int i = 1; i < n && J->sig == JS_OK; i++) {
        jval from = a[i];
        if (from.t == JS_STR) {
            for (u32 k = 0; k < from.str->len; k++)
                js_put(J, tv, js_to_key(J, js_num(k)), js_from_str(js_str_n(J, from.str->s + k, 1)));
            continue;
        }
        if (!js_is_obj(from)) continue;
        jobj *src = from.obj;
        if (src->kind == JO_ARRAY || src->kind == JO_ARGS)
            for (u32 k = 0; k < src->len; k++) js_put(J, tv, js_to_key(J, js_num(k)), src->items[k]);
        jprop **own;
        u32 nown = js_keys_of(J, src, &own, JK_ENUM | JK_STR | JK_SYM);
        for (u32 k = 0; k < nown && J->sig == JS_OK; k++)
            js_put(J, tv, own[k]->key, js_prop_read(J, own[k], from));
    }
    return tv;
}

/* A function's prototype is made when something first asks for it
   (js_make_proto_for). What works on the properties an object already has
   makes it first: freezing a function left its prototype to be made later,
   writable; deleting it said yes and it came back; and a descriptor with no
   value, Babel's way of making every class's prototype read-only, left a
   class with no methods, whose prototype nothing had asked for, with none --
   so a class extending it failed in Object.create, and Stripe's script. */
static void js_proto_ready(jctx *J, jobj *o) {
    if (o && o->kind == JO_FUNC && !js_find(o, J->s_prototype)) js_make_proto_for(J, o);
}

static jval nat_obj_freeze(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval v = js_arg(a, n, 0);
    if (!js_is_obj(v)) return v;
    jobj *o = v.obj;
    js_proto_ready(J, o);
    for (jprop *p = o->ofirst; p; p = p->onext) {
        p->flags &= ~JP_CONF;
        if (p->v.t != JS_ACC) p->flags &= ~JP_WRITE;
    }
    o->flags |= JOF_NOEXT;
    if (o->kind == JO_ARRAY || o->kind == JO_ARGS) o->flags |= JOF_FROZEN;
    return v;
}

static jval nat_obj_seal(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval v = js_arg(a, n, 0);
    if (!js_is_obj(v)) return v;
    js_proto_ready(J, v.obj);
    for (jprop *p = v.obj->ofirst; p; p = p->onext) p->flags &= ~JP_CONF;
    v.obj->flags |= JOF_NOEXT;
    return v;
}

static jval nat_obj_prevent_ext(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval v = js_arg(a, n, 0);
    if (js_is_obj(v)) js_proto_ready(J, v.obj);
    if (js_is_obj(v)) v.obj->flags |= JOF_NOEXT;
    return v;
}

static jval nat_obj_isfrozen(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)J;
    jval v = js_arg(a, n, 0);
    if (!js_is_obj(v)) return js_bool(1);
    jobj *o = v.obj;
    if (!(o->flags & JOF_NOEXT)) return js_bool(0);
    if ((o->kind == JO_ARRAY || o->kind == JO_ARGS) && o->len && !(o->flags & JOF_FROZEN)) return js_bool(0);
    for (jprop *p = o->ofirst; p; p = p->onext)
        if ((p->flags & JP_CONF) || (p->v.t != JS_ACC && (p->flags & JP_WRITE))) return js_bool(0);
    return js_bool(1);
}

static jval nat_obj_issealed(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)J;
    jval v = js_arg(a, n, 0);
    if (!js_is_obj(v)) return js_bool(1);
    if (!(v.obj->flags & JOF_NOEXT)) return js_bool(0);
    for (jprop *p = v.obj->ofirst; p; p = p->onext) if (p->flags & JP_CONF) return js_bool(0);
    return js_bool(1);
}

static jval nat_obj_isextensible(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)J;
    jval v = js_arg(a, n, 0);
    return js_bool(js_is_obj(v) && !(v.obj->flags & JOF_NOEXT));
}

static jval nat_obj_getproto(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval v = js_arg(a, n, 0);
    if (v.t == JS_NULL || v.t == JS_UNDEF)
        return js_throw(J, JS_ERR_TYPE, "undefined or null has no prototype", J->error_line);
    if (!js_is_obj(v)) return js_from_obj(js_proto_of_value(J, v));
    if (v.obj->flags & JOF_PROXY) return js_proxy_getproto(J, v.obj);
    return v.obj->proto ? js_from_obj(v.obj->proto) : js_null();
}

static jval nat_obj_setproto(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval v = js_arg(a, n, 0), p = js_arg(a, n, 1);
    if (p.t != JS_NULL && !js_is_obj(p))
        return js_throw(J, JS_ERR_TYPE, "a prototype is an object or null", J->error_line);
    if (!js_is_obj(v)) return v;
    jobj *np = p.t == JS_NULL ? 0 : p.obj;
    if (js_proto_loops(v.obj, np))
        return js_throw(J, JS_ERR_TYPE, "that prototype would make a ring", J->error_line);
    v.obj->proto = np;
    return v;
}

static int js_proxy_define(jctx *J, jobj *p, jstr *key, jval desc);

/* Object.defineProperty's descriptor, applied: a value or an accessor, and
   what is allowed, with anything left out kept from what was there. */
static int js_define_from_desc(jctx *J, jobj *o, jstr *key, jval desc) {
    if (o->flags & JOF_PROXY) return js_proxy_define(J, o, key, desc);
    if (!js_is_obj(desc)) {
        js_throw(J, JS_ERR_TYPE, "a property descriptor has to be an object", J->error_line);
        return 0;
    }
    jobj *d = desc.obj;
    int has_value = js_has(J, d, J->s_value), has_get = js_has(J, d, J->s_get);
    int has_set = js_has(J, d, J->s_set), has_w = js_has(J, d, J->s_writable);
    int has_e = js_has(J, d, J->s_enumerable), has_c = js_has(J, d, J->s_configurable);
    jval value = has_value ? js_get(J, desc, J->s_value) : js_undef();
    jval get = has_get ? js_get(J, desc, J->s_get) : js_undef();
    jval set = has_set ? js_get(J, desc, J->s_set) : js_undef();
    int w = has_w && js_to_bool(js_get(J, desc, J->s_writable));
    int e = has_e && js_to_bool(js_get(J, desc, J->s_enumerable));
    int c = has_c && js_to_bool(js_get(J, desc, J->s_configurable));
    if (J->sig != JS_OK) return 0;
    if ((has_get && get.t != JS_UNDEF && !js_callable(get)) || (has_set && set.t != JS_UNDEF && !js_callable(set))) {
        js_throw(J, JS_ERR_TYPE, "a getter or setter has to be a function", J->error_line);
        return 0;
    }

    if (js_str_eq(key, J->s_prototype)) js_proto_ready(J, o);
    jprop *p = js_find(o, key);
    if (p && !(p->flags & JP_CONF)) {
        /* Not configurable: only a writable value may still change. */
        if (p->v.t != JS_ACC && (p->flags & JP_WRITE)) {
            if (has_value) p->v = value;
            if (has_w && !w) p->flags &= ~JP_WRITE;
            return 1;
        }
        if (has_value && p->v.t != JS_ACC && js_same_value(p->v, value)) return 1;
        if (!has_value && !has_get && !has_set) return 1;
        js_throw_named(J, JS_ERR_TYPE, "", key, " cannot be redefined");
        return 0;
    }
    int flags = p ? p->flags : 0;
    if (has_e) flags = e ? (flags | JP_ENUM) : (flags & ~JP_ENUM);
    if (has_c) flags = c ? (flags | JP_CONF) : (flags & ~JP_CONF);

    if (has_get || has_set) {
        if (p && p->v.t != JS_ACC) js_delete_prop(o, key);
        js_define_accessor(J, o, key, get, set, flags);
        if (has_get && get.t == JS_UNDEF) { jprop *q = js_find(o, key); if (q && q->v.t == JS_ACC) q->v.acc->get = js_undef(); }
        if (has_set && set.t == JS_UNDEF) { jprop *q = js_find(o, key); if (q && q->v.t == JS_ACC) q->v.acc->set = js_undef(); }
        return 1;
    }
    if (has_w) flags = w ? (flags | JP_WRITE) : (flags & ~JP_WRITE);
    if (o->kind == JO_ARRAY || o->kind == JO_ARGS) {
        u32 idx;
        if (js_index_of(key, &idx)) {
            if (has_value) js_arr_set(J, o, idx, value);
            else if (idx >= o->len) js_arr_set(J, o, idx, js_undef());
            return 1;
        }
        if (js_str_eq(key, J->s_length)) {
            if (has_value) js_set_length(J, o, value);
            if (has_w && !w) o->flags |= JOF_FROZEN;
            return 1;
        }
    }
    if (p && p->v.t == JS_ACC) { js_delete_prop(o, key); p = 0; }
    if (p) {
        if (has_value) p->v = value;
        p->flags = flags;
        return 1;
    }
    if (o->flags & JOF_NOEXT) {
        js_throw_named(J, JS_ERR_TYPE, "", key, " cannot be added: the object is not extensible");
        return 0;
    }
    js_put_prop_flags(J, o, key, value, flags);
    return 1;
}

static jval nat_obj_defineprop(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval o = js_arg(a, n, 0);
    if (!js_is_obj(o)) return js_throw(J, JS_ERR_TYPE, "Object.defineProperty needs an object", J->error_line);
    jstr *key = js_to_key(J, js_arg(a, n, 1));
    if (J->sig != JS_OK) return js_undef();
    js_define_from_desc(J, o.obj, key, js_arg(a, n, 2));
    return o;
}

static jval nat_obj_defineprops(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval o = js_arg(a, n, 0), props = js_arg(a, n, 1);
    if (!js_is_obj(o)) return js_throw(J, JS_ERR_TYPE, "Object.defineProperties needs an object", J->error_line);
    if (!js_is_obj(props)) return o;
    jprop **own;
    u32 nown = js_keys_of(J, props.obj, &own, JK_ENUM | JK_STR | JK_SYM);
    for (u32 i = 0; i < nown && J->sig == JS_OK; i++)
        js_define_from_desc(J, o.obj, own[i]->key, js_prop_read(J, own[i], props));
    return o;
}

static jval nat_obj_create(jctx *J, jval t, jval *a, int n) {
    jval p = js_arg(a, n, 0);
    if (p.t != JS_NULL && !js_is_obj(p))
        return js_throw(J, JS_ERR_TYPE, "Object.create needs an object or null", J->error_line);
    jobj *o = js_object_with(J, JO_PLAIN, p.t == JS_NULL ? 0 : p.obj);
    if (!o) return js_undef();
    if (n > 1 && a[1].t != JS_UNDEF) {
        jval args[2] = { js_from_obj(o), a[1] };
        nat_obj_defineprops(J, t, args, 2);
    }
    return js_from_obj(o);
}

static jobj *js_descriptor(jctx *J, jval v, int flags) {
    jobj *d = js_object(J, JO_PLAIN);
    if (!d) return 0;
    if (v.t == JS_ACC) {
        js_put_prop(J, d, J->s_get, v.acc->get);
        js_put_prop(J, d, J->s_set, v.acc->set);
    } else {
        js_put_prop(J, d, J->s_value, v);
        js_put_prop(J, d, J->s_writable, js_bool(flags & JP_WRITE));
    }
    js_put_prop(J, d, J->s_enumerable, js_bool(flags & JP_ENUM));
    js_put_prop(J, d, J->s_configurable, js_bool(flags & JP_CONF));
    return d;
}

static jval nat_obj_getownpropdesc(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval o = js_arg(a, n, 0);
    jstr *key = js_to_key(J, js_arg(a, n, 1));
    if (J->sig != JS_OK) return js_undef();
    if (o.t == JS_STR) {
        u32 idx;
        if (js_index_of(key, &idx) && idx < o.str->len)
            return js_from_obj(js_descriptor(J, js_from_str(js_str_n(J, o.str->s + idx, 1)), JP_ENUM));
        if (js_str_eq(key, J->s_length))
            return js_from_obj(js_descriptor(J, js_num(o.str->len), 0));
        return js_undef();
    }
    if (!js_is_obj(o)) return js_undef();
    jval v;
    int fl;
    if (!js_get_own(J, o.obj, key, &v, &fl)) return js_undef();
    return js_from_obj(js_descriptor(J, v, fl));
}

static jval nat_obj_getownpropdescs(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval o = js_arg(a, n, 0);
    jobj *out = js_object(J, JO_PLAIN);
    if (!js_is_obj(o) || !out) return js_from_obj(out);
    if (o.obj->kind == JO_ARRAY)
        for (u32 i = 0; i < o.obj->len; i++)
            js_put_prop(J, out, js_to_key(J, js_num(i)), js_from_obj(js_descriptor(J, o.obj->items[i], JP_PLAIN)));
    jprop **own;
    u32 nown = js_keys_of(J, o.obj, &own, JK_STR | JK_SYM);
    for (u32 i = 0; i < nown; i++)
        js_put_prop(J, out, own[i]->key, js_from_obj(js_descriptor(J, own[i]->v, own[i]->flags)));
    return js_from_obj(out);
}

static jval js_own_names(jctx *J, jval o, int want) {
    jobj *out = js_array(J);
    if (!out) return js_undef();
    if (o.t == JS_STR && (want & JK_STR)) {
        for (u32 i = 0; i < o.str->len; i++) js_arr_push(J, out, js_from_str(js_to_key(J, js_num(i))));
        js_arr_push(J, out, js_from_str(J->s_length));
        return js_from_obj(out);
    }
    if (!js_is_obj(o)) return js_from_obj(out);
    jobj *ob = o.obj;
    if ((want & JK_STR) && (ob->kind == JO_ARRAY || ob->kind == JO_ARGS)) {
        for (u32 i = 0; i < ob->len; i++) js_arr_push(J, out, js_from_str(js_to_key(J, js_num(i))));
        js_arr_push(J, out, js_from_str(J->s_length));
    }
    /* A function's length and name are read from the function itself
       unless a script has given it its own (js_exotic_get), and they are
       its own properties all the same, before its prototype. */
    if ((want & JK_STR) && (ob->kind == JO_FUNC || ob->kind == JO_NATIVE)) {
        if (!js_find(ob, J->s_length)) js_arr_push(J, out, js_from_str(J->s_length));
        if (!js_find(ob, J->s_name)) js_arr_push(J, out, js_from_str(J->s_name));
        js_proto_ready(J, ob);
    }
    jprop **own;
    u32 nown = js_keys_of(J, ob, &own, want);
    for (u32 i = 0; i < nown; i++)
        js_arr_push(J, out, js_is_sym_key(own[i]->key) ? js_from_sym(own[i]->key) : js_from_str(own[i]->key));
    return js_from_obj(out);
}

static jval nat_obj_getownnames(jctx *J, jval t, jval *a, int n) { (void)t; return js_own_names(J, js_arg(a, n, 0), JK_STR); }
static jval nat_obj_getownsyms(jctx *J, jval t, jval *a, int n) { (void)t; return js_own_names(J, js_arg(a, n, 0), JK_SYM); }

static jval nat_obj_fromentries(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *out = js_object(J, JO_PLAIN);
    jiter it;
    if (!out || !js_iter_open(J, js_arg(a, n, 0), &it)) return js_from_obj(out);
    for (;;) {
        jval e;
        int r = js_iter_step(J, &it, &e);
        if (r <= 0) break;
        jval k = js_get_index(J, e, 0), v = js_get_index(J, e, 1);
        if (J->sig != JS_OK) { js_iter_close(J, &it); break; }
        js_define(J, out, js_to_key(J, k), v, JP_PLAIN);
    }
    return js_from_obj(out);
}

static jval nat_obj_is(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t;
    return js_bool(js_same_value(js_arg(a, n, 0), js_arg(a, n, 1)));
}

static jval nat_obj_hasown(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval o = js_arg(a, n, 0);
    if (o.t == JS_NULL || o.t == JS_UNDEF)
        return js_throw(J, JS_ERR_TYPE, "Object.hasOwn needs an object", J->error_line);
    return js_bool(js_has_own(J, o, js_to_key(J, js_arg(a, n, 1))));
}

static jval nat_obj_groupby(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *out = js_object_with(J, JO_PLAIN, 0);
    jval fn = js_arg(a, n, 1);
    jiter it;
    if (!out || !js_iter_open(J, js_arg(a, n, 0), &it)) return js_from_obj(out);
    for (u32 i = 0;; i++) {
        jval e;
        int r = js_iter_step(J, &it, &e);
        if (r <= 0) break;
        jval args[2] = { e, js_num(i) };
        jstr *k = js_to_key(J, js_call(J, fn, js_undef(), args, 2));
        if (J->sig != JS_OK) { js_iter_close(J, &it); break; }
        jprop *p = js_find(out, k);
        if (!p) p = js_put_prop(J, out, k, js_from_obj(js_array(J)));
        if (p && js_is_obj(p->v)) js_arr_push(J, p->v.obj, e);
    }
    return js_from_obj(out);
}

/* --- Function ---------------------------------------------------------------
 *
 * In `f.call(x, 1)` the function to run is `this`: the method was fetched
 * from f, so f is what the call hands over as the receiver, exactly as it
 * would for any other method. */
static jval nat_fn_call(jctx *J, jval t, jval *a, int n) {
    if (!js_callable(t)) return js_throw(J, JS_ERR_TYPE, "call needs a function", J->error_line);
    return js_call(J, t, n > 0 ? a[0] : js_undef(), n > 1 ? a + 1 : 0, n > 1 ? n - 1 : 0);
}

/* An array-like, as apply and Reflect take their arguments. */
static int js_list_from(jctx *J, jval v, jargs *A) {
    if (v.t == JS_NULL || v.t == JS_UNDEF) return 1;
    if (!js_is_obj(v)) {
        js_throw(J, JS_ERR_TYPE, "the arguments have to be an array or something like one", J->error_line);
        return 0;
    }
    jobj *o = v.obj;
    if (o->kind == JO_ARRAY || o->kind == JO_ARGS) {
        for (u32 i = 0; i < o->len; i++) if (!js_args_push(J, A, o->items[i])) return 0;
        return 1;
    }
    double len = js_to_num(J, js_get(J, v, J->s_length));
    if (J->sig != JS_OK) return 0;
    if (!(len > 0)) return 1;
    if (len > (1 << 20)) len = 1 << 20;
    for (u32 i = 0; i < (u32)len; i++) {
        if (!js_args_push(J, A, js_get_index(J, v, i))) return 0;
        if (J->sig != JS_OK) return 0;
    }
    return 1;
}

static jval nat_fn_apply(jctx *J, jval t, jval *a, int n) {
    if (!js_callable(t)) return js_throw(J, JS_ERR_TYPE, "apply needs a function", J->error_line);
    jargs A;
    js_args_init(&A);
    if (!js_list_from(J, js_arg(a, n, 1), &A)) { js_args_free(&A); return js_undef(); }
    jval r = js_call(J, t, js_arg(a, n, 0), A.v, A.n);
    js_args_free(&A);
    return r;
}

/* A bound function being called: what it holds is on itself, which only
   J->callee can say -- `this` here is whatever the caller supplied, and is
   exactly what a bound function ignores. With new, it constructs what it was
   bound to. */
static jval nat_fn_bound(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *self = J->callee;
    jval nt = J->new_target;
    if (!self) return js_throw(J, JS_ERR_TYPE, "this is not a function", J->error_line);
    jval fn = self->data;
    jargs A;
    js_args_init(&A);
    jobj *pre = self->extra;
    if (pre) for (u32 i = 0; i < pre->len; i++) js_args_push(J, &A, pre->items[i]);
    for (int i = 0; i < n; i++) js_args_push(J, &A, a[i]);
    jval r;
    if (nt.t != JS_UNDEF) r = js_construct(J, fn, A.v, A.n, js_is_obj(nt) && nt.obj == self ? fn : nt);
    else r = js_call(J, fn, self->data2, A.v, A.n);
    js_args_free(&A);
    return r;
}

static jval nat_fn_bind(jctx *J, jval t, jval *a, int n) {
    if (!js_callable(t)) return js_throw(J, JS_ERR_TYPE, "bind needs a function", J->error_line);
    jobj *out = js_native(J, "bound", nat_fn_bound);
    if (!out) return js_undef();
    out->data = t;
    out->data2 = n > 0 ? a[0] : js_undef();
    if (n > 1) {
        jobj *pre = js_array(J);
        if (pre) for (int i = 1; i < n; i++) js_arr_push(J, pre, a[i]);
        out->extra = pre;
    }
    /* Named after what it is bound to, as "bound f". */
    jstr *nm = t.obj->name;
    char buf[80];
    int w = 0;
    for (const char *p = "bound "; *p; p++) buf[w++] = *p;
    for (u32 i = 0; nm && i < nm->len && w < 79; i++) buf[w++] = nm->s[i];
    out->name = js_str_n(J, buf, (u32)w);
    int len = (int)js_to_num(J, js_get(J, t, J->s_length)) - (n > 1 ? n - 1 : 0);
    out->spare = (u16)(len > 0 ? len : 0);
    if (!js_is_constructor(J, t)) out->flags |= JOF_NOCTOR;
    return js_from_obj(out);
}

static jval nat_fn_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_callable(t)) return js_throw(J, JS_ERR_TYPE, "Function.prototype.toString needs a function", J->error_line);
    char buf[160];
    int w = 0;
    int native = t.obj->kind == JO_NATIVE;
    /* The function's own text, as the standard asks and as MapLibre needs:
       it makes its worker from its functions' text, and got "{ ... }". */
    if (!native && t.obj->node >= 0 && t.obj->node < J->nnodes) {
        int sp = J->nodes[t.obj->node].c;
        int k = J->nodes[t.obj->node].kind;
        if ((k == N_FUNC || k == N_FUNCDECL) && sp >= 0 && sp < J->nspans) {
            jspan *s = &J->spans[sp];
            jstr *src = s->src >= 0 && s->src < J->nsrcs ? J->srcs[s->src] : 0;
            if (src && s->end <= src->len && s->start < s->end)
                return js_from_str(js_str_n(J, src->s + s->start, s->end - s->start));
        }
    }
    int fl = native || t.obj->node < 0 ? 0 : J->nodes[t.obj->node].op;
    const char *head = (fl & FN_CTOR) ? "class " : (fl & FN_ASYNC) ? "async function " : "function ";
    if (fl & FN_ARROW) head = "";
    for (const char *p = head; *p; p++) buf[w++] = *p;
    if (!(fl & FN_ARROW)) {
        jstr *nm = t.obj->name;
        for (u32 i = 0; nm && !js_is_sym_key(nm) && i < nm->len && w < 100; i++) buf[w++] = nm->s[i];
    }
    const char *tail = native ? "() { [native code] }" : (fl & FN_ARROW) ? "() => { ... }"
                     : (fl & FN_CTOR) ? " { ... }" : "() { ... }";
    for (const char *p = tail; *p; p++) buf[w++] = *p;
    return js_from_str(js_str_n(J, buf, (u32)w));
}

static jval nat_fn_hasinstance(jctx *J, jval t, jval *a, int n) {
    return js_binary(J, OP_INSTANCEOF, js_arg(a, n, 0), t, J->error_line);
}

static jval js_eval_source(jctx *J, jstr *src, jscope *var_sc, jscope *sc, jval this_val);

/* Function(a, b, body): a function made from text, at the top level. */
/* A function made from its source at run time, of the kind `head` begins. */
static jval js_function_from(jctx *J, const char *head, u32 hlen, jval *a, int n) {
    jtext tx = { 0, 0, 0, 0 };
    jt_put(J, &tx, head, hlen);
    for (int i = 0; i + 1 < n; i++) {
        jstr *p = js_to_str(J, a[i]);
        if (i) jt_put(J, &tx, ",", 1);
        jt_put(J, &tx, p->s, p->len);
    }
    jt_put(J, &tx, "\n) {\n", 5);
    if (n > 0) {
        jstr *body = js_to_str(J, a[n - 1]);
        jt_put(J, &tx, body->s, body->len);
    }
    jt_put(J, &tx, "\n})", 3);
    jstr *src = jt_done(J, &tx);
    if (J->sig != JS_OK || !src) return js_undef();
    return js_eval_source(J, src, J->global, J->global_lex, js_from_obj(J->global_obj));
}

static jval nat_function_make(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return js_function_from(J, "(function anonymous(", 20, a, n);
}

/* The same for the other kinds of function, whose constructors are reached
   only as the constructor of a function of the kind. Alpine makes every
   expression a page writes in its attributes into an async function this
   way, and with Function's answer those gave back a value and no promise:
   its `.catch` on the result stopped every one. */
static jval nat_async_function_make(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return js_function_from(J, "(async function anonymous(", 26, a, n);
}

static jval nat_gen_function_make(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return js_function_from(J, "(function* anonymous(", 21, a, n);
}

static jval nat_async_gen_function_make(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return js_function_from(J, "(async function* anonymous(", 27, a, n);
}

static jval nat_fn_proto(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_undef();
}

/* --- Symbol ------------------------------------------------------------------ */

static jval nat_symbol_make(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (J->new_target.t != JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "Symbol is not made with new", J->error_line);
    jval d = js_arg(a, n, 0);
    jstr *desc = d.t == JS_UNDEF ? js_str(J, "") : js_to_str(J, d);
    return js_from_sym(js_sym_new(J, desc->s, desc->len));
}

static jval nat_symbol_for(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *k = js_to_str(J, js_arg(a, n, 0));
    if (!J->sym_registry) J->sym_registry = js_object_with(J, JO_PLAIN, 0);
    jprop *p = js_find(J->sym_registry, k);
    if (p) return p->v;
    jval s = js_from_sym(js_sym_new(J, k->s, k->len));
    js_put_prop(J, J->sym_registry, k, s);
    return s;
}

static jval nat_symbol_keyfor(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval s = js_arg(a, n, 0);
    if (s.t != JS_SYM) return js_throw(J, JS_ERR_TYPE, "Symbol.keyFor needs a symbol", J->error_line);
    if (!J->sym_registry) return js_undef();
    for (jprop *p = J->sym_registry->ofirst; p; p = p->onext)
        if (p->v.str == s.str) return js_from_str(p->key);
    return js_undef();
}

static jstr *js_this_sym(jctx *J, jval t) {
    t = js_unbox(t);
    if (t.t != JS_SYM) { js_throw(J, JS_ERR_TYPE, "this is not a symbol", J->error_line); return 0; }
    return t.str;
}

static jval nat_symbol_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *k = js_this_sym(J, t);
    if (!k) return js_undef();
    jtext tx = { 0, 0, 0, 0 };
    jt_put(J, &tx, "Symbol(", 7);
    jt_put(J, &tx, k->s, k->len);
    jt_put(J, &tx, ")", 1);
    return js_from_str(jt_done(J, &tx));
}

static jval nat_symbol_valueof(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *k = js_this_sym(J, t);
    return k ? js_from_sym(k) : js_undef();
}

static jval nat_symbol_desc(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *k = js_this_sym(J, t);
    return k ? js_from_str(js_str_n(J, k->s, k->len)) : js_undef();
}

/* --- errors ------------------------------------------------------------------ */

/* Error and its kinds, called with or without new: which of them is being
   called is in the native's own data. */
static jval nat_error_make(jctx *J, jval t, jval *a, int n) {
    jobj *ctor = J->callee;
    jval nt = J->new_target;
    int kind = ctor && ctor->data.t == JS_NUM ? (int)ctor->data.num : JS_ERR_ERROR;
    int ai = kind == JS_ERR_AGGREGATE ? 1 : 0;
    jval m = js_arg(a, n, ai);
    jstr *msg = m.t == JS_UNDEF ? 0 : js_to_str(J, m);
    if (J->sig != JS_OK) return js_undef();
    jobj *proto = nt.t != JS_UNDEF && js_is_obj(t) ? t.obj->proto : J->err_proto[kind];
    jobj *e = js_error_with(J, proto, js_str(J, JS_ERR_NAMES[kind]), msg ? msg : js_str(J, ""));
    if (!e) return js_undef();
    if (!msg) js_delete_prop(e, J->s_message);
    jval opts = js_arg(a, n, ai + 1);
    if (js_is_obj(opts) && js_has(J, opts.obj, J->s_cause)) {
        jprop *p = js_put_prop(J, e, J->s_cause, js_get(J, opts, J->s_cause));
        if (p) p->flags = JP_WRITE | JP_CONF;
    }
    if (kind == JS_ERR_AGGREGATE) {
        jargs A;
        js_args_init(&A);
        js_iter_collect(J, js_arg(a, n, 0), &A);
        jobj *errs = js_array(J);
        for (int i = 0; i < A.n && errs; i++) js_arr_push(J, errs, A.v[i]);
        js_args_free(&A);
        jprop *p = js_put_prop(J, e, js_str(J, "errors"), js_from_obj(errs));
        if (p) p->flags = JP_WRITE | JP_CONF;
    }
    return js_from_obj(e);
}

static jval nat_error_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return js_throw(J, JS_ERR_TYPE, "Error.prototype.toString needs an object", J->error_line);
    jval nm = js_get(J, t, J->s_name);
    jval msg = js_get(J, t, J->s_message);
    if (J->sig != JS_OK) return js_undef();
    jstr *ns = nm.t == JS_UNDEF ? js_str(J, "Error") : js_to_str(J, nm);
    jstr *ms = msg.t == JS_UNDEF ? js_str(J, "") : js_to_str(J, msg);
    if (!ns->len) return js_from_str(ms);
    if (!ms->len) return js_from_str(ns);
    return js_from_str(js_concat(J, js_concat(J, ns, js_str(J, ": ")), ms));
}

/* V8's way for a library to give its own error a stack, which a great many
   of them call unconditionally. */
static jval nat_error_capture(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval o = js_arg(a, n, 0);
    if (!js_is_obj(o)) return js_undef();
    jval s = nat_error_tostring(J, o, 0, 0);
    if (J->sig != JS_OK) { J->sig = JS_OK; s = js_from_str(js_str(J, "Error")); }
    jstr *st = js_concat(J, js_to_str(J, s), js_str(J, "\n    at <anonymous>"));
    jprop *p = js_put_prop(J, o.obj, J->s_stack, js_from_str(st));
    if (p) p->flags = JP_WRITE | JP_CONF;
    return js_undef();
}

/* --- Boolean ----------------------------------------------------------------- */

static jval nat_bool_ctor(jctx *J, jval t, jval *a, int n) {
    jval v = js_bool(js_to_bool(js_arg(a, n, 0)));
    if (J->new_target.t != JS_UNDEF && js_is_obj(t)) {
        t.obj->kind = JO_BOXED;
        t.obj->ival = v;
        return t;
    }
    return v;
}

static jval nat_bool_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    t = js_unbox(t);
    if (t.t != JS_BOOL) return js_throw(J, JS_ERR_TYPE, "this is not a boolean", J->error_line);
    return js_from_str(js_str(J, t.b ? "true" : "false"));
}

static jval nat_bool_valueof(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    t = js_unbox(t);
    if (t.t != JS_BOOL) return js_throw(J, JS_ERR_TYPE, "this is not a boolean", J->error_line);
    return t;
}

/* --- numbers ---------------------------------------------------------------- */

static jval nat_num_ctor(jctx *J, jval t, jval *a, int n) {
    jval x = n ? js_unbox(a[0]) : js_num(0);
    if (x.t == JS_OBJ) { x = js_to_primitive(J, x, 1); if (J->sig != JS_OK) return js_undef(); }
    jval v = x.t == JS_BIG ? js_num(jsb_to_double(J, x.big)) : js_num(js_to_num(J, x));
    if (J->new_target.t != JS_UNDEF && js_is_obj(t)) {
        t.obj->kind = JO_BOXED;
        t.obj->ival = v;
        return t;
    }
    return v;
}

static int js_this_num(jctx *J, jval t, double *out) {
    t = js_unbox(t);
    if (t.t != JS_NUM) { js_throw(J, JS_ERR_TYPE, "this is not a number", J->error_line); return 0; }
    *out = t.num;
    return 1;
}

/* Every decimal digit of a finite, positive double, exactly: how many, and
   the power of ten of the first. A double is a whole number times a power of
   two, which has an exact decimal of at most seven hundred and seventy odd
   digits, and toFixed and toPrecision round that, not an approximation. */
static int js_exact_digits(double d, char *all, int cap, int *first) {
    union { double d; u64 u; } x;
    x.d = d;
    int ef = (int)((x.u >> 52) & 0x7FF);
    u64 m = x.u & 0xFFFFFFFFFFFFFull;
    int e2;
    if (ef) { m |= 1ull << 52; e2 = ef - 1075; }
    else e2 = -1074;
    if (!m) { all[0] = '0'; *first = 0; return 1; }
    jbig M;
    jb_set(&M, m);
    int last;
    if (e2 >= 0) { jb_shl(&M, e2); last = 0; }
    else { jb_mul_pow(&M, 5, -e2); last = e2; }
    char rev[800];
    int len = 0;
    while (M.n && len + 9 <= (int)sizeof(rev)) {
        u32 r = jb_div_small(&M, 1000000000u);
        for (int k = 0; k < 9; k++) { rev[len++] = (char)('0' + r % 10); r /= 10; }
    }
    while (len > 1 && rev[len - 1] == '0') len--;
    int trailing = 0;
    while (trailing < len - 1 && rev[trailing] == '0') trailing++;
    int n = len - trailing;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) all[i] = rev[len - 1 - i];
    *first = last + len - 1;
    return n;
}

/* Digits rounded to `keep` of them, half up as the standard says for these
   methods. Returns 1 when rounding carried into a new first digit. */
static int js_round_digits(char *dg, int *nd, int keep) {
    if (keep < 0) { *nd = 0; return 0; }
    if (*nd <= keep) {
        for (int i = *nd; i < keep; i++) dg[i] = '0';
        *nd = keep;
        return 0;
    }
    int up = dg[keep] >= '5';
    *nd = keep;
    if (!up) return 0;
    for (int i = keep - 1; i >= 0; i--) {
        if (dg[i] < '9') { dg[i]++; return 0; }
        dg[i] = '0';
    }
    for (int i = keep; i > 0; i--) dg[i] = dg[i - 1];
    dg[0] = '1';
    (*nd)++;
    return 1;
}

static jval nat_num_tofixed(jctx *J, jval t, jval *a, int n) {
    double x;
    if (!js_this_num(J, t, &x)) return js_undef();
    double fd = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (fd < 0 || fd > 100) return js_throw(J, JS_ERR_RANGE, "toFixed takes 0 to 100 digits", J->error_line);
    int f = (int)fd;
    if (x != x || x - x != 0 || x >= 1e21 || x <= -1e21) return js_from_str(js_to_str(J, js_num(x)));
    char out[240];
    int w = 0;
    if (x < 0) { out[w++] = '-'; x = -x; }
    char dg[800];
    int first;
    int nd = x == 0 ? 1 : js_exact_digits(x, dg, 790, &first);
    if (x == 0) { dg[0] = '0'; first = 0; }
    /* Keep the digits down to 10^-f. */
    int keep = first + 1 + f;
    if (keep < 0) { nd = 0; keep = 0; }
    int carried = js_round_digits(dg, &nd, keep);
    if (carried) first++;
    if (nd == 0) { dg[0] = '0'; nd = 1; first = -1 - f + 1; first = 0; for (int i = 0; i <= f; i++) dg[i] = '0'; nd = f + 1; first = 0; }
    /* Whole part. */
    if (first < 0) {
        out[w++] = '0';
        if (f) {
            out[w++] = '.';
            int zeros = -first - 1;
            int k = 0;
            for (int i = 0; i < f; i++) out[w++] = i < zeros ? '0' : (k < nd ? dg[k++] : '0');
        }
    } else {
        int k = 0;
        for (int i = 0; i <= first; i++) out[w++] = k < nd ? dg[k++] : '0';
        if (f) {
            out[w++] = '.';
            for (int i = 0; i < f; i++) out[w++] = k < nd ? dg[k++] : '0';
        }
    }
    if (w == 2 && out[0] == '-' && out[1] == '0') w = 1, out[0] = '0';
    /* -0.00 is written without its sign, as other engines do only when the
       value itself is -0; a negative that rounds to nought keeps it. */
    return js_from_str(js_str_n(J, out, (u32)w));
}

static jval nat_num_toprecision(jctx *J, jval t, jval *a, int n) {
    double x;
    if (!js_this_num(J, t, &x)) return js_undef();
    if (js_arg(a, n, 0).t == JS_UNDEF) return js_from_str(js_to_str(J, js_num(x)));
    double pd = js_trunc(js_to_num(J, a[0]));
    if (x != x || x - x != 0) return js_from_str(js_to_str(J, js_num(x)));
    if (pd < 1 || pd > 100) return js_throw(J, JS_ERR_RANGE, "toPrecision takes 1 to 100 digits", J->error_line);
    int p = (int)pd;
    char out[240];
    int w = 0;
    if (x < 0) { out[w++] = '-'; x = -x; }
    char dg[800];
    int e;
    int nd;
    if (x == 0) { nd = 1; dg[0] = '0'; e = 0; }
    else nd = js_exact_digits(x, dg, 790, &e);
    if (js_round_digits(dg, &nd, p)) e++;
    if (x == 0) { for (int i = 0; i < p; i++) dg[i] = '0'; nd = p; }
    if (e < -6 || e >= p) {
        out[w++] = dg[0];
        if (p > 1) { out[w++] = '.'; for (int i = 1; i < p; i++) out[w++] = dg[i]; }
        out[w++] = 'e';
        out[w++] = e < 0 ? '-' : '+';
        int ae = e < 0 ? -e : e;
        char rev[8];
        int r = 0;
        do { rev[r++] = (char)('0' + ae % 10); ae /= 10; } while (ae);
        while (r) out[w++] = rev[--r];
    } else if (e >= 0) {
        for (int i = 0; i <= e; i++) out[w++] = dg[i];
        if (p > e + 1) { out[w++] = '.'; for (int i = e + 1; i < p; i++) out[w++] = dg[i]; }
    } else {
        out[w++] = '0';
        out[w++] = '.';
        for (int i = 0; i < -e - 1; i++) out[w++] = '0';
        for (int i = 0; i < p; i++) out[w++] = dg[i];
    }
    return js_from_str(js_str_n(J, out, (u32)w));
}

static jval nat_num_toexponential(jctx *J, jval t, jval *a, int n) {
    double x;
    if (!js_this_num(J, t, &x)) return js_undef();
    if (x != x || x - x != 0) return js_from_str(js_to_str(J, js_num(x)));
    int given = js_arg(a, n, 0).t != JS_UNDEF;
    double fd = given ? js_trunc(js_to_num(J, a[0])) : 0;
    if (fd < 0 || fd > 100) return js_throw(J, JS_ERR_RANGE, "toExponential takes 0 to 100 digits", J->error_line);
    char out[240];
    int w = 0;
    if (x < 0) { out[w++] = '-'; x = -x; }
    char dg[800];
    int e, nd;
    if (x == 0) { nd = 1; dg[0] = '0'; e = 0; }
    else if (!given) {
        nd = js_shortest(x, dg, &e);
    } else nd = js_exact_digits(x, dg, 790, &e);
    int f = given ? (int)fd : nd - 1;
    if (js_round_digits(dg, &nd, f + 1)) e++;
    out[w++] = dg[0];
    if (f > 0) { out[w++] = '.'; for (int i = 1; i <= f; i++) out[w++] = i < nd ? dg[i] : '0'; }
    out[w++] = 'e';
    out[w++] = e < 0 ? '-' : '+';
    int ae = e < 0 ? -e : e;
    char rev[8];
    int r = 0;
    do { rev[r++] = (char)('0' + ae % 10); ae /= 10; } while (ae);
    while (r) out[w++] = rev[--r];
    return js_from_str(js_str_n(J, out, (u32)w));
}

/* A number in another base, for (255).toString(16) and the random ids pages
   make with toString(36). */
static jval nat_num_tostring(jctx *J, jval t, jval *a, int n) {
    double x;
    if (!js_this_num(J, t, &x)) return js_undef();
    int radix = js_arg(a, n, 0).t == JS_UNDEF ? 10 : (int)js_to_num(J, a[0]);
    if (radix < 2 || radix > 36) return js_throw(J, JS_ERR_RANGE, "a radix is from 2 to 36", J->error_line);
    if (radix == 10 || x != x || x - x != 0) return js_from_str(js_to_str(J, js_num(x)));
    static const char D[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char out[1200];
    int w = 0;
    int neg = x < 0;
    if (neg) x = -x;
    double ip = x < 4503599627370496.0 ? (double)(u64)x : x;
    if (ip > x) ip -= 1;
    double fp = x - ip;
    char rev[1100];
    int r = 0;
    if (ip < 18446744073709551616.0) {
        u64 v = (u64)ip;
        do { rev[r++] = D[v % (u64)radix]; v /= (u64)radix; } while (v && r < 1090);
    } else {
        /* Past 2^64, digit by digit from the double. */
        double v = ip;
        while (v >= 1 && r < 1090) {
            double q = js_trunc(v / radix);
            int d = (int)(v - q * radix);
            if (d < 0) d = 0;
            if (d >= radix) d = radix - 1;
            rev[r++] = D[d];
            v = q;
        }
    }
    if (neg) out[w++] = '-';
    while (r) out[w++] = rev[--r];
    if (fp > 0) {
        out[w++] = '.';
        /* As many digits as the fraction's 52 bits are worth. */
        int most = 1;
        for (double s = 1; s > 1e-16 * x && most < 1100 - w - 1 && most < 52; s /= radix) most++;
        for (int i = 0; i < most && fp > 0 && w < 1190; i++) {
            fp *= radix;
            int d = (int)fp;
            out[w++] = D[d];
            fp -= d;
        }
    }
    return js_from_str(js_str_n(J, out, (u32)w));
}

static jval nat_num_valueof(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    double x;
    if (!js_this_num(J, t, &x)) return js_undef();
    return js_num(x);
}

static jval intl_number_text(jctx *J, double x, jval locales, jval opts);

/* Through a NumberFormat of its options (jsintl.h): the way en-US writes a
   number, commas between thousands and at most three places by default. */
static jval nat_num_tolocalestring(jctx *J, jval t, jval *a, int n) {
    double x;
    if (!js_this_num(J, t, &x)) return js_undef();
    return intl_number_text(J, x, js_arg(a, n, 0), js_arg(a, n, 1));
}

static int js_is_int(double d) { return d == d && d - d == 0 && js_trunc(d) == d; }

static jval nat_number_isinteger(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t;
    jval v = js_arg(a, n, 0);
    return js_bool(v.t == JS_NUM && js_is_int(v.num));
}
static jval nat_number_issafe(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t;
    jval v = js_arg(a, n, 0);
    return js_bool(v.t == JS_NUM && js_is_int(v.num) && v.num <= 9007199254740991.0 && v.num >= -9007199254740991.0);
}
static jval nat_number_isfinite(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t;
    jval v = js_arg(a, n, 0);
    return js_bool(v.t == JS_NUM && v.num - v.num == 0);
}
static jval nat_number_isnan(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t;
    jval v = js_arg(a, n, 0);
    return js_bool(v.t == JS_NUM && v.num != v.num);
}

static jval nat_parseint(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    int radix = n > 1 ? js_to_i32(J, a[1]) : 0;
    u32 i = 0;
    while (i < s->len && js_blank(s->s[i])) i++;
    int neg = 0;
    if (i < s->len && (s->s[i] == '-' || s->s[i] == '+')) neg = s->s[i++] == '-';
    if (radix != 0 && (radix < 2 || radix > 36)) return js_num(js_nan());
    if ((radix == 0 || radix == 16) && i + 1 < s->len && s->s[i] == '0'
        && (s->s[i + 1] == 'x' || s->s[i + 1] == 'X')) {
        i += 2;
        radix = 16;
    }
    if (radix == 0) radix = 10;
    double v = 0;
    int any = 0;
    while (i < s->len) {
        char c = s->s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= radix) break;
        v = v * radix + d;
        any = 1;
        i++;
    }
    /* Unlike a plain conversion this stops at the first thing it cannot use
       rather than refusing the lot, which is why parseInt("12px") is 12. */
    if (!any) return js_num(js_nan());
    return js_num(neg ? -v : v);
}

static jval nat_parsefloat(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    u32 i = 0, used = 0;
    while (i < s->len && js_blank(s->s[i])) i++;
    double v = js_signed_decimal(s->s + i, s->len - i, &used);
    if (!used) return js_num(js_nan());
    return js_num(v);
}

static jval nat_isnan(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double d = js_to_num(J, js_arg(a, n, 0));
    return js_bool(d != d);
}

static jval nat_isfinite(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double d = js_to_num(J, js_arg(a, n, 0));
    return js_bool(d - d == 0);
}

/* --- Math -------------------------------------------------------------------
 *
 * Worked out here, to the last place or next to it. The machine's own square
 * root is one instruction and is exact; the rest are reduced into a small
 * range with the constant split in two, so the reduction does not lose the
 * digits the series is then asked for. */

static double js_fmodsafe(double q);
static double js_fabs(double x) { return x < 0 ? -x : (x == 0 ? 0.0 : x); }
static int js_isfin(double x) { return x - x == 0; }

static double js_sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static double js_floor(double x) {
    if (!js_isfin(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    double t = (double)(long long)x;
    if (t > x) t -= 1;
    if (t == 0 && x < 0) return -0.0 + t;
    return t;
}

static double js_ceil(double x) {
    if (!js_isfin(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    double t = (double)(long long)x;
    if (t < x) t += 1;
    if (t == 0 && x < 0) return -0.0;
    return t;
}

static double js_ldexp(double x, int e) {
    union { double d; u64 u; } b;
    while (e > 1000) { x *= 1.0715086071862673e301; e -= 1000; }       /* 2^1000 */
    while (e < -1000) { x *= 9.332636185032189e-302; e += 1000; }       /* 2^-1000 */
    /* 2^e as a double, made from its bits. */
    if (e >= -1022) {
        b.u = (u64)(e + 1023) << 52;
        return x * b.d;
    }
    b.u = (u64)(e + 1023 + 500) << 52;
    return x * b.d * 3.054936363499605e-151;                            /* 2^-500 */
}

static double js_exp(double x) {
    if (x != x) return x;
    if (x > 709.782712893384) return 1e308 * 10;
    if (x < -745.1332191019412) return 0;
    const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10;
    double k = js_floor(x * 1.4426950408889634 + 0.5);
    double r = (x - k * ln2_hi) - k * ln2_lo;
    /* e^r = 1 + r + r^2 q, with q = 1/2 + r/6 + ... worked out smallest
       terms first, and 1 + r added with its rounding error kept and put back:
       a sum from the front lost the last place. */
    double p = 1;
    for (int i = 17; i >= 3; i--) p = 1 + p * r / i;
    double q = 0.5 * p;
    double s1 = 1 + r;
    double err = (1 - s1) + r;
    return js_ldexp(s1 + (err + r * r * q), (int)k);
}

/* The mantissa of x in [sqrt(1/2), sqrt(2)) and its power of two. */
static double js_split2(double x, int *e) {
    union { double d; u64 u; } b;
    b.d = x;
    int ex = (int)((b.u >> 52) & 0x7FF);
    if (!ex) {                                   /* subnormal: scale up first */
        b.d = x * 18014398509481984.0;           /* 2^54 */
        ex = (int)((b.u >> 52) & 0x7FF) - 54;
    }
    ex -= 1023;
    b.u = (b.u & 0xFFFFFFFFFFFFFull) | (1023ull << 52);
    double m = b.d;
    if (m > 1.4142135623730951) { m /= 2; ex++; }
    *e = ex;
    return m;
}

/* log(1 + f) for f in [sqrt(1/2) - 1, sqrt(2) - 1), as f less a small
   correction, which keeps f's own digits: 2 atanh(f / (2 + f)) rearranged. */
static double js_log1f(double f) {
    double s = f / (2 + f);
    double z = s * s;
    double R = 0;
    for (int k = 14; k >= 1; k--) R = z * (2.0 / (2 * k + 1) + R);
    double hfsq = 0.5 * f * f;
    return f - (hfsq - s * (hfsq + R));
}

static double js_log(double x) {
    if (x != x || x < 0) return js_nan();
    if (x == 0) return -1e308 * 10;
    if (!js_isfin(x)) return x;
    int e;
    double m = js_split2(x, &e);
    double f = m - 1;
    const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10;
    double s = f / (2 + f);
    double z = s * s;
    double R = 0;
    for (int k = 14; k >= 1; k--) R = z * (2.0 / (2 * k + 1) + R);
    double hfsq = 0.5 * f * f;
    return e * ln2_hi + (f - (hfsq - (s * (hfsq + R) + e * ln2_lo)));
}

/* log2 and log10 from the mantissa's logarithm and the power of two, so a
   power of two comes out whole. */
static double js_log2(double x) {
    if (x != x || x < 0) return js_nan();
    if (x == 0) return -1e308 * 10;
    if (!js_isfin(x)) return x;
    int e;
    double m = js_split2(x, &e);
    return e + js_log1f(m - 1) * 1.4426950408889634;
}

static double js_pow(double a, double b) {
    if (b != b) return js_nan();
    if (b == 0) return 1;
    if (a != a) return js_nan();
    if (js_fabs(a) == 1 && !js_isfin(b)) return js_nan();
    if (!js_isfin(b)) {
        double m = js_fabs(a);
        if (b > 0) return m > 1 ? b : 0;
        return m < 1 ? -b : 0;
    }
    int bint = js_trunc(b) == b;
    /* A square root is one instruction and exact; through a logarithm it lost
       its last place. */
    if (b == 0.5 && a >= 0 && js_isfin(a)) return js_sqrt(a) + 0.0;
    if (b == -0.5 && a > 0 && js_isfin(a)) return 1 / js_sqrt(a);
    if (a == 0) {
        int odd = bint && js_fabs(b) < 9007199254740992.0 && ((long long)b & 1);
        if (b > 0) return odd ? a : 0;
        return odd && (1 / a < 0) ? -1e308 * 10 : 1e308 * 10;
    }
    if (!js_isfin(a)) {
        int odd = bint && js_fabs(b) < 9007199254740992.0 && ((long long)b & 1);
        if (a > 0) return b > 0 ? a : 0;
        if (b > 0) return odd ? a : -a;
        return odd ? -0.0 : 0;
    }
    /* A whole exponent is repeated multiplication, which is exact where the
       logarithm would not be: 10 ** 3 has to be 1000 and not 999.9999. */
    if (bint && js_fabs(b) <= 1024) {
        double r = 1, x = a;
        long long k = (long long)(b < 0 ? -b : b);
        while (k) {
            if (k & 1) r *= x;
            x *= x;
            k >>= 1;
        }
        return b < 0 ? 1 / r : r;
    }
    if (a < 0) {
        if (!bint) return js_nan();
        double r = js_exp(b * js_log(-a));
        return ((long long)js_fmodsafe(b)) ? -r : r;
    }
    return js_exp(b * js_log(a));
}

static double js_sin_small(double x) {
    double x2 = x * x, term = x, sum = x;
    for (int i = 1; i < 12; i++) {
        term *= -x2 / ((2.0 * i) * (2.0 * i + 1));
        sum += term;
    }
    return sum;
}

static double js_cos_small(double x) {
    double x2 = x * x, term = 1, sum = 1;
    for (int i = 1; i < 12; i++) {
        term *= -x2 / ((2.0 * i - 1) * (2.0 * i));
        sum += term;
    }
    return sum;
}

/* x reduced by quarter turns, with pi/2 in three pieces so the reduction
   keeps its digits well past a thousand turns. */
static double js_circle(double x, int want_cos) {
    if (!js_isfin(x)) return js_nan();
    const double p1 = 1.57079632673412561417e+00, p2 = 6.07710050650619224932e-11,
                 p3 = 2.02226624879595063154e-21;
    double q = js_floor(x * 0.6366197723675814 + 0.5);
    double r = ((x - q * p1) - q * p2) - q * p3;
    long long k = (long long)js_fmodsafe(q);
    long long quarter = (k + (want_cos ? 1 : 0)) & 3;
    switch (quarter) {
        case 0:  return js_sin_small(r);
        case 1:  return js_cos_small(r);
        case 2:  return -js_sin_small(r);
        default: return -js_cos_small(r);
    }
}

static double js_atan(double x) {
    if (x != x) return x;
    int neg = x < 0;
    if (neg) x = -x;
    double add = 0;
    int inv = 0;
    if (x > 1) { x = 1 / x; inv = 1; }
    /* Past tan(pi/8), turn by a quarter of pi: atan x = pi/4 + atan((x-1)/(x+1)),
       which leaves an argument below 0.18 and a series that needs a dozen
       terms. */
    if (x > 0.41421356237309503) {
        x = (x - 1) / (x + 1);
        add = 0.7853981633974483;
    }
    double z = x * x, sum = 0;
    for (int k = 41; k >= 1; k -= 2) sum = 1.0 / k - z * sum;
    sum = add + x * sum;
    if (inv) sum = 1.5707963267948966 - sum;
    return neg ? -sum : sum;
}

static double js_atan2(double y, double x) {
    if (x != x || y != y) return js_nan();
    if (x == 0 && y == 0) {
        int xneg = 1 / x < 0, yneg = 1 / y < 0;
        if (!xneg) return y;
        return yneg ? -3.141592653589793 : 3.141592653589793;
    }
    if (!js_isfin(x) || !js_isfin(y)) {
        if (!js_isfin(x) && !js_isfin(y)) {
            double a = x > 0 ? 0.7853981633974483 : 2.356194490192345;
            return y > 0 ? a : -a;
        }
        if (!js_isfin(y)) return y > 0 ? 1.5707963267948966 : -1.5707963267948966;
        if (x > 0) return y >= 0 ? 0.0 : -0.0;
        return y >= 0 ? 3.141592653589793 : -3.141592653589793;
    }
    if (x == 0) return y > 0 ? 1.5707963267948966 : -1.5707963267948966;
    double a = js_atan(y / x);
    if (x > 0) return a;
    return y >= 0 ? a + 3.141592653589793 : a - 3.141592653589793;
}

static double js_fmodsafe(double q) {
    /* q modulo 2^52, as a whole number a long long holds. */
    if (q >= 4503599627370496.0 || q <= -4503599627370496.0) return 0;
    return q;
}

static jval js_math1(jctx *J, jval *a, int n, int which) {
    double x = js_to_num(J, js_arg(a, n, 0));
    switch (which) {
        case 0: return js_num(js_floor(x));
        case 1: return js_num(js_ceil(x));
        case 2: {                                            /* round */
            if (!js_isfin(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) return js_num(x);
            double f = js_floor(x);
            double r = x - f >= 0.5 ? f + 1 : f;
            if (r == 0 && (x < 0 || (x == 0 && 1 / x < 0))) r = -0.0;
            return js_num(r);
        }
        case 3: return js_num(js_trunc(x) == 0 && x < 0 ? -0.0 : js_trunc(x));
        case 4: return js_num(x < 0 ? -x : (x == 0 ? 0.0 : x));
        case 5: return js_num(x < 0 ? js_nan() : js_sqrt(x));
        case 6: return js_num(js_circle(x, 0));
        case 7: return js_num(js_circle(x, 1));
        case 8: {
            double c = js_circle(x, 1);
            return js_num(js_circle(x, 0) / c);
        }
        case 9: return js_num(js_atan(x));
        case 10: {                                           /* asin */
            if (x > 1 || x < -1) return js_num(js_nan());
            return js_num(js_atan2(x, js_sqrt(1 - x * x)));
        }
        case 11: {                                           /* acos */
            if (x > 1 || x < -1) return js_num(js_nan());
            return js_num(js_atan2(js_sqrt(1 - x * x), x));
        }
        case 12: return js_num(js_exp(x));
        case 13: return js_num(js_log(x));
        case 14: return js_num(js_log2(x));
        case 15: {                                           /* log10 */
            double r = js_log2(x) * 0.30102999566398120;
            double k = js_floor(r + 0.5);
            if (js_fabs(r - k) < 1e-12 && js_pow(10, k) == x) return js_num(k);
            return js_num(r);
        }
        case 16: return js_num(x > 0 ? 1 : x < 0 ? -1 : x);             /* sign */
        case 17: {                                           /* cbrt */
            if (x == 0 || !js_isfin(x)) return js_num(x);
            double m = js_fabs(x);
            double g = js_exp(js_log(m) / 3);
            for (int i = 0; i < 3; i++) g = g - (g * g * g - m) / (3 * g * g);
            return js_num(x < 0 ? -g : g);
        }
        case 18: {                                           /* expm1 */
            if (js_fabs(x) < 1e-5) return js_num(x + x * x / 2 + x * x * x / 6);
            return js_num(js_exp(x) - 1);
        }
        case 19: {                                           /* log1p */
            if (js_fabs(x) < 1e-4) return js_num(x - x * x / 2 + x * x * x / 3);
            return js_num(js_log(1 + x));
        }
        case 20: return js_num((js_exp(x) - js_exp(-x)) / 2);            /* sinh */
        case 21: return js_num((js_exp(x) + js_exp(-x)) / 2);            /* cosh */
        case 22: {                                           /* tanh */
            if (x > 20) return js_num(1);
            if (x < -20) return js_num(-1);
            double e2 = js_exp(2 * x);
            return js_num((e2 - 1) / (e2 + 1));
        }
        case 23: return js_num(x == 0 ? x : (x < 0 ? -1 : 1) * js_log(js_fabs(x) + js_sqrt(x * x + 1)));
        case 24: return js_num(x < 1 ? js_nan() : js_log(x + js_sqrt(x * x - 1)));
        case 25: return js_num(js_fabs(x) > 1 ? js_nan() : 0.5 * js_log((1 + x) / (1 - x)));
        case 26: return js_num((double)(float)x);                        /* fround */
        case 27: {                                           /* clz32 */
            u32 v = (u32)js_d_to_i32(x);
            int c = 0;
            if (!v) return js_num(32);
            while (!(v & 0x80000000u)) { v <<= 1; c++; }
            return js_num(c);
        }
        default: return js_num(js_nan());
    }
}

#define JS_MATH1(nm, k) static jval nat_m_##nm(jctx *J, jval t, jval *a, int n) { (void)t; return js_math1(J, a, n, k); }
JS_MATH1(floor, 0) JS_MATH1(ceil, 1) JS_MATH1(round, 2) JS_MATH1(trunc, 3) JS_MATH1(abs, 4)
JS_MATH1(sqrt, 5) JS_MATH1(sin, 6) JS_MATH1(cos, 7) JS_MATH1(tan, 8) JS_MATH1(atan, 9)
JS_MATH1(asin, 10) JS_MATH1(acos, 11) JS_MATH1(exp, 12) JS_MATH1(log, 13) JS_MATH1(log2, 14)
JS_MATH1(log10, 15) JS_MATH1(sign, 16) JS_MATH1(cbrt, 17) JS_MATH1(expm1, 18) JS_MATH1(log1p, 19)
JS_MATH1(sinh, 20) JS_MATH1(cosh, 21) JS_MATH1(tanh, 22) JS_MATH1(asinh, 23) JS_MATH1(acosh, 24)
JS_MATH1(atanh, 25) JS_MATH1(fround, 26) JS_MATH1(clz32, 27)

static jval nat_m_min(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double best = 1e308 * 10;
    int nan = 0;
    for (int i = 0; i < n; i++) {
        double v = js_to_num(J, a[i]);
        if (v != v) nan = 1;
        else if (v < best || (v == 0 && best == 0 && 1 / v < 0)) best = v;
    }
    return js_num(nan ? js_nan() : best);
}

static jval nat_m_max(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double best = -1e308 * 10;
    int nan = 0;
    for (int i = 0; i < n; i++) {
        double v = js_to_num(J, a[i]);
        if (v != v) nan = 1;
        else if (v > best || (v == 0 && best == 0 && 1 / v > 0)) best = v;
    }
    return js_num(nan ? js_nan() : best);
}

static jval nat_m_pow(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return js_num(js_pow(js_to_num(J, js_arg(a, n, 0)), js_to_num(J, js_arg(a, n, 1))));
}

static jval nat_m_atan2(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return js_num(js_atan2(js_to_num(J, js_arg(a, n, 0)), js_to_num(J, js_arg(a, n, 1))));
}

static jval nat_m_hypot(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double big = 0;
    int inf = 0, nan = 0;
    for (int i = 0; i < n; i++) {
        double v = js_fabs(js_to_num(J, a[i]));
        if (!js_isfin(v)) { if (v == v) inf = 1; else nan = 1; }
        else if (v > big) big = v;
    }
    if (inf) return js_num(1e308 * 10);
    if (nan) return js_num(js_nan());
    if (big == 0) return js_num(0);
    double sum = 0;
    for (int i = 0; i < n; i++) {
        double v = js_to_num(J, a[i]) / big;
        sum += v * v;
    }
    return js_num(big * js_sqrt(sum));
}

static jval nat_m_imul(jctx *J, jval t, jval *a, int n) {
    (void)t;
    u32 x = (u32)js_to_i32(J, js_arg(a, n, 0)), y = (u32)js_to_i32(J, js_arg(a, n, 1));
    return js_num((double)(int)(x * y));
}

/* A sequence that is different every time a page is loaded and good enough
   for what pages use it for: ids, shuffles, jitter. Seeded from the clock, so
   two pages on one machine do not make the same ids. */
static u64 js_rand_state;
static jval nat_m_random(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    if (!js_rand_state) js_rand_state = 0x5A4C52004A53ull ^ ((u64)ticks() * 0x9E3779B97F4A7C15ull);
    js_rand_state ^= js_rand_state << 13;
    js_rand_state ^= js_rand_state >> 7;
    js_rand_state ^= js_rand_state << 17;
    return js_num((double)(js_rand_state >> 11) / 9007199254740992.0);
}

/* --- JSON ----------------------------------------------------------------------
 *
 * Written into a buffer that grows and made a string once at the end. It was
 * built a character at a time by concatenation, and each concatenation
 * copied everything so far into the page's memory, which never gives any
 * back: seven kilobytes of output used up the whole of it. */

/* A string, quoted, with everything JSON does not allow raw escaped. The
   half of a surrogate pair with no other half (kept here as the three bytes
   it would be on its own) is written as its escape, as the standard has
   had it since 2019: core-js tests for exactly that, and finding it missing
   put its own JSON.stringify in place, whose pattern this engine then read
   wrongly. Two halves that do make a pair are written as the character. */
static void js_json_quote(jctx *J, jtext *o, const jstr *s) {
    static const char HEX[] = "0123456789abcdef";
    jt_put(J, o, "\"", 1);
    u32 from = 0;
    for (u32 i = 0; s && i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        if (c == 0xED && i + 2 < s->len && (u8)s->s[i + 1] >= 0xA0) {
            u32 hi = 0xD000u | (((u32)s->s[i + 1] & 0x3F) << 6) | ((u32)s->s[i + 2] & 0x3F);
            jt_put(J, o, s->s + from, i - from);
            if (hi < 0xDC00 && i + 5 < s->len && (u8)s->s[i + 3] == 0xED && (u8)s->s[i + 4] >= 0xB0) {
                u32 lo = 0xD000u | (((u32)s->s[i + 4] & 0x3F) << 6) | ((u32)s->s[i + 5] & 0x3F);
                u32 cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
                char b[4] = { (char)(0xF0 | (cp >> 18)), (char)(0x80 | ((cp >> 12) & 63)),
                              (char)(0x80 | ((cp >> 6) & 63)), (char)(0x80 | (cp & 63)) };
                jt_put(J, o, b, 4);
                i += 5;
            } else {
                char u[6] = { '\\', 'u', HEX[hi >> 12], HEX[(hi >> 8) & 15], HEX[(hi >> 4) & 15], HEX[hi & 15] };
                jt_put(J, o, u, 6);
                i += 2;
            }
            from = i + 1;
            continue;
        }
        if (c != '"' && c != '\\' && c >= 32) continue;
        jt_put(J, o, s->s + from, i - from);
        from = i + 1;
        if (c == '"') jt_put(J, o, "\\\"", 2);
        else if (c == '\\') jt_put(J, o, "\\\\", 2);
        else if (c == '\n') jt_put(J, o, "\\n", 2);
        else if (c == '\r') jt_put(J, o, "\\r", 2);
        else if (c == '\t') jt_put(J, o, "\\t", 2);
        else if (c == 8) jt_put(J, o, "\\b", 2);
        else if (c == 12) jt_put(J, o, "\\f", 2);
        else {
            char u[6] = { '\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 15] };
            jt_put(J, o, u, 6);
        }
    }
    if (s) jt_put(J, o, s->s + from, s->len - from);
    jt_put(J, o, "\"", 1);
}

#define JS_JSON_DEPTH 128

typedef struct {
    jtext out;
    jval replacer;            /* a function, or undefined */
    jobj *allow;              /* an array of the keys to write, or 0 */
    char gap[12];
    int ngap;
    jobj *open[JS_JSON_DEPTH];
    int depth;
} jjson;

static int js_json_value(jctx *J, jjson *S, jval holder, jstr *key, jval v);

static void js_json_newline(jctx *J, jjson *S) {
    if (!S->ngap) return;
    jt_put(J, &S->out, "\n", 1);
    for (int i = 0; i < S->depth; i++) jt_put(J, &S->out, S->gap, (u32)S->ngap);
}

/* Writes a value; 0 when it wrote nothing (undefined, a function, a symbol),
   -1 when something threw. */
static int js_json_value(jctx *J, jjson *S, jval holder, jstr *key, jval v) {
    if (js_is_obj(v) || v.t == JS_STR || v.t == JS_NUM || v.t == JS_BIG) {
        jval tj = js_is_obj(v) || v.t == JS_BIG ? js_get(J, v, J->s_toJSON) : js_undef();
        if (J->sig != JS_OK) return -1;
        if (js_callable(tj)) {
            jval k = js_from_str(key);
            v = js_call(J, tj, v, &k, 1);
            if (J->sig != JS_OK) return -1;
        }
    }
    if (js_callable(S->replacer)) {
        jval args[2] = { js_from_str(key), v };
        v = js_call(J, S->replacer, holder, args, 2);
        if (J->sig != JS_OK) return -1;
    }
    /* A boxed number, string or boolean is written as what it holds; a boxed
       symbol is an object like any other, and is written as one. */
    if (js_is_obj(v) && v.obj->kind == JO_BOXED && v.obj->ival.t != JS_SYM) v = v.obj->ival;
    if (v.t == JS_BIG) {
        js_throw(J, JS_ERR_TYPE, "JSON cannot write a BigInt", J->error_line);
        return -1;
    }
    switch (v.t) {
        case JS_NULL: jt_put(J, &S->out, "null", 4); return 1;
        case JS_BOOL: jt_put(J, &S->out, v.b ? "true" : "false", v.b ? 4 : 5); return 1;
        case JS_STR: js_json_quote(J, &S->out, v.str); return 1;
        case JS_NUM: {
            if (!js_isfin(v.num)) { jt_put(J, &S->out, "null", 4); return 1; }
            char buf[40];
            u32 k = js_num_text(v.num, buf, sizeof(buf));
            jt_put(J, &S->out, buf, k);
            return 1;
        }
        case JS_OBJ: break;
        default: return 0;
    }
    jobj *ob = v.obj;
    if (js_callable(v)) return 0;
    for (int k = 0; k < S->depth; k++)
        if (S->open[k] == ob) {
            js_throw(J, JS_ERR_TYPE, "JSON cannot write an object that contains itself", J->error_line);
            return -1;
        }
    if (S->depth >= JS_JSON_DEPTH) {
        js_throw(J, JS_ERR_RANGE, "JSON nested too deeply to write", J->error_line);
        return -1;
    }
    S->open[S->depth++] = ob;
    if (ob->kind == JO_ARRAY) {
        jt_put(J, &S->out, "[", 1);
        for (u32 i = 0; i < ob->len; i++) {
            if (i) jt_put(J, &S->out, ",", 1);
            js_json_newline(J, S);
            int r = js_json_value(J, S, v, js_to_key(J, js_num(i)), ob->items[i]);
            if (r < 0) return -1;
            if (r == 0) jt_put(J, &S->out, "null", 4);
        }
        S->depth--;
        if (ob->len) js_json_newline(J, S);
        jt_put(J, &S->out, "]", 1);
        return 1;
    }
    jt_put(J, &S->out, "{", 1);
    int first = 1;
    jobj *keys = S->allow ? S->allow : js_own_enum(J, v, 0);
    if (J->sig != JS_OK) return -1;
    for (u32 i = 0; keys && i < keys->len; i++) {
        jstr *k = js_to_key(J, keys->items[i]);
        jval pv = js_get(J, v, k);
        if (J->sig != JS_OK) return -1;
        u32 mark = S->out.n;
        if (!first) jt_put(J, &S->out, ",", 1);
        js_json_newline(J, S);
        js_json_quote(J, &S->out, k);
        jt_put(J, &S->out, S->ngap ? ": " : ":", S->ngap ? 2 : 1);
        int r = js_json_value(J, S, v, k, pv);
        if (r < 0) return -1;
        if (r == 0) { S->out.n = mark; continue; }
        first = 0;
    }
    S->depth--;
    if (!first) js_json_newline(J, S);
    jt_put(J, &S->out, "}", 1);
    return 1;
}

static jval nat_json_stringify(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jjson *S = (jjson *)malloc(sizeof(jjson));
    if (!S) { js_out_of_memory(J); return js_undef(); }
    memset(S, 0, (int)sizeof(jjson));
    S->replacer = js_undef();
    jval rep = js_arg(a, n, 1);
    if (js_callable(rep)) S->replacer = rep;
    else if (js_is_obj(rep) && rep.obj->kind == JO_ARRAY) {
        S->allow = js_array(J);
        for (u32 i = 0; i < rep.obj->len; i++) {
            jval e = js_unbox(rep.obj->items[i]);
            if (e.t == JS_STR || e.t == JS_NUM) js_arr_push(J, S->allow, js_from_str(js_to_key(J, e)));
        }
    }
    jval sp = js_unbox(js_arg(a, n, 2));
    if (sp.t == JS_NUM) {
        int k = sp.num > 10 ? 10 : (int)sp.num;
        for (int i = 0; i < k; i++) S->gap[S->ngap++] = ' ';
    } else if (sp.t == JS_STR) {
        for (u32 i = 0; i < sp.str->len && i < 10; i++) S->gap[S->ngap++] = sp.str->s[i];
    }
    jobj *wrap = js_object(J, JO_PLAIN);
    jval v = js_arg(a, n, 0);
    if (wrap) js_put_prop(J, wrap, js_str(J, ""), v);
    int r = js_json_value(J, S, js_from_obj(wrap), js_str(J, ""), v);
    jval out = js_undef();
    if (r > 0 && S->out.full) js_throw(J, JS_ERR_RANGE, "JSON too long to write", J->error_line);
    else if (r > 0) out = js_from_str(js_str_n(J, S->out.b ? S->out.b : "", S->out.n));
    free(S->out.b);
    free(S);
    return out;
}

/* --- and read ------------------------------------------------------------------
 *
 * Strictly: anything that is not JSON is a SyntaxError, as it is everywhere
 * else, because a page that tries JSON.parse inside try to find out whether
 * it has JSON has to be told when it has not. */

typedef struct {
    const char *s;
    u32 len, at;
    int bad;
    int depth;
} jread;

static void jr_space(jread *r) {
    while (r->at < r->len && (r->s[r->at] == ' ' || r->s[r->at] == '\t'
                              || r->s[r->at] == '\n' || r->s[r->at] == '\r')) r->at++;
}

static int jr_hex4(jread *r, u32 at) {
    if (at + 4 > r->len) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int d = js_hexval(r->s[at + i]);
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

static jstr *jr_string(jctx *J, jread *r) {
    r->at++;                                   /* the opening quote */
    u32 scan = r->at;
    int plain = 1;
    while (scan < r->len && r->s[scan] != '"') {
        if (r->s[scan] == '\\') { scan++; plain = 0; }
        else if ((unsigned char)r->s[scan] < 32) { r->bad = 1; return 0; }
        scan++;
    }
    if (scan >= r->len) { r->bad = 1; return 0; }
    if (plain) {
        jstr *s = js_str_n(J, r->s + r->at, scan - r->at);
        r->at = scan + 1;
        return s;
    }
    /* No escape makes more bytes than it was written in. */
    char *buf = (char *)malloc(scan - r->at + 1);
    if (!buf) { r->bad = 1; return 0; }
    u32 w = 0, i = r->at;
    while (i < scan) {
        if (r->s[i] != '\\') { buf[w++] = r->s[i++]; continue; }
        char e = r->s[i + 1];
        i += 2;
        switch (e) {
            case '"': buf[w++] = '"'; break;
            case '\\': buf[w++] = '\\'; break;
            case '/': buf[w++] = '/'; break;
            case 'b': buf[w++] = 8; break;
            case 'f': buf[w++] = 12; break;
            case 'n': buf[w++] = '\n'; break;
            case 'r': buf[w++] = '\r'; break;
            case 't': buf[w++] = '\t'; break;
            case 'u': {
                int h = jr_hex4(r, i);
                if (h < 0) { r->bad = 1; free(buf); return 0; }
                i += 4;
                u32 cp = (u32)h;
                /* A character past the first sixty five thousand comes as two. */
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= scan && r->s[i] == '\\' && r->s[i + 1] == 'u') {
                    int lo = jr_hex4(r, i + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (u32)(lo - 0xDC00);
                        i += 6;
                    }
                }
                w += js_utf8(cp, buf + w);
                break;
            }
            default: r->bad = 1; free(buf); return 0;
        }
    }
    r->at = scan + 1;
    jstr *s = js_str_n(J, buf, w);
    free(buf);
    return s;
}

static jval jr_value(jctx *J, jread *r) {
    jr_space(r);
    if (r->at >= r->len || r->bad) { r->bad = 1; return js_undef(); }
    char c = r->s[r->at];

    if (c == '{' || c == '[') {
        if (++r->depth > 512) { r->bad = 1; return js_undef(); }
        int obj = c == '{';
        r->at++;
        jobj *o = obj ? js_object(J, JO_PLAIN) : js_array(J);
        if (!o) { r->bad = 1; return js_undef(); }
        jr_space(r);
        char end = obj ? '}' : ']';
        if (r->at < r->len && r->s[r->at] == end) { r->at++; r->depth--; return js_from_obj(o); }
        for (;;) {
            jr_space(r);
            if (obj) {
                if (r->at >= r->len || r->s[r->at] != '"') { r->bad = 1; break; }
                jstr *k = jr_string(J, r);
                jr_space(r);
                if (r->bad || r->at >= r->len || r->s[r->at] != ':') { r->bad = 1; break; }
                r->at++;
                jval v = jr_value(J, r);
                if (r->bad) break;
                js_define(J, o, k, v, JP_PLAIN);
            } else {
                jval v = jr_value(J, r);
                if (r->bad) break;
                js_arr_push(J, o, v);
            }
            jr_space(r);
            if (r->at < r->len && r->s[r->at] == ',') { r->at++; continue; }
            if (r->at < r->len && r->s[r->at] == end) { r->at++; break; }
            r->bad = 1;
            break;
        }
        r->depth--;
        return js_from_obj(o);
    }
    if (c == '"') {
        jstr *s = jr_string(J, r);
        return s ? js_from_str(s) : js_undef();
    }

    /* The three words, whole. */
    static const struct { const char *w; int n; } WORDS[] = { { "true", 4 }, { "false", 5 }, { "null", 4 } };
    for (int k = 0; k < 3; k++) {
        int m = 0;
        while (m < WORDS[k].n && r->at + (u32)m < r->len && r->s[r->at + m] == WORDS[k].w[m]) m++;
        if (m == WORDS[k].n) {
            r->at += (u32)m;
            return k == 2 ? js_null() : js_bool(k == 0);
        }
    }

    /* A number, in JSON's own shape: a minus, digits with no leading zero,
       then a fraction and an exponent if they are there. */
    u32 start = r->at;
    if (r->at < r->len && r->s[r->at] == '-') r->at++;
    u32 digits = r->at;
    while (r->at < r->len && r->s[r->at] >= '0' && r->s[r->at] <= '9') r->at++;
    if (r->at == digits || (r->s[digits] == '0' && r->at - digits > 1)) { r->bad = 1; return js_undef(); }
    if (r->at < r->len && r->s[r->at] == '.') {
        u32 f = ++r->at;
        while (r->at < r->len && r->s[r->at] >= '0' && r->s[r->at] <= '9') r->at++;
        if (r->at == f) { r->bad = 1; return js_undef(); }
    }
    if (r->at < r->len && (r->s[r->at] == 'e' || r->s[r->at] == 'E')) {
        r->at++;
        if (r->at < r->len && (r->s[r->at] == '+' || r->s[r->at] == '-')) r->at++;
        u32 e = r->at;
        while (r->at < r->len && r->s[r->at] >= '0' && r->s[r->at] <= '9') r->at++;
        if (r->at == e) { r->bad = 1; return js_undef(); }
    }
    return js_num(js_str_to_num(r->s + start, r->at - start));
}

/* JSON.parse's reviver, from the inside out, as the standard walks it. */
static jval js_json_revive(jctx *J, jval holder, jstr *key, jval reviver, int depth) {
    jval v = js_get(J, holder, key);
    if (J->sig != JS_OK || depth > 512) return js_undef();
    if (js_is_obj(v)) {
        jobj *o = v.obj;
        if (o->kind == JO_ARRAY) {
            for (u32 i = 0; i < o->len; i++) {
                jstr *k = js_to_key(J, js_num(i));
                jval nv = js_json_revive(J, v, k, reviver, depth + 1);
                if (J->sig != JS_OK) return js_undef();
                if (nv.t == JS_UNDEF) o->items[i] = js_undef();
                else o->items[i] = nv;
            }
        } else {
            jobj *keys = js_own_enum(J, v, 0);
            for (u32 i = 0; keys && i < keys->len; i++) {
                jstr *k = keys->items[i].str;
                jval nv = js_json_revive(J, v, k, reviver, depth + 1);
                if (J->sig != JS_OK) return js_undef();
                if (nv.t == JS_UNDEF) js_delete_prop(o, k);
                else js_define(J, o, k, nv, JP_PLAIN);
            }
        }
    }
    jval args[2] = { js_from_str(key), v };
    return js_call(J, reviver, holder, args, 2);
}

static jval nat_json_parse(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    jread r = { s->s, s->len, 0, 0, 0 };
    jval v = jr_value(J, &r);
    jr_space(&r);
    if (r.bad || r.at != r.len)
        return js_throw(J, JS_ERR_SYNTAX, "JSON.parse was given text that is not JSON", J->error_line);
    jval reviver = js_arg(a, n, 1);
    if (js_callable(reviver)) {
        jobj *root = js_object(J, JO_PLAIN);
        jstr *empty = js_str(J, "");
        js_put_prop(J, root, empty, v);
        return js_json_revive(J, js_from_obj(root), empty, reviver, 0);
    }
    return v;
}

/* --- Reflect ------------------------------------------------------------------- */

static jval nat_reflect_apply(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jargs A;
    js_args_init(&A);
    if (!js_list_from(J, js_arg(a, n, 2), &A)) { js_args_free(&A); return js_undef(); }
    jval r = js_call(J, js_arg(a, n, 0), js_arg(a, n, 1), A.v, A.n);
    js_args_free(&A);
    return r;
}

static jval nat_reflect_construct(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval f = js_arg(a, n, 0);
    jargs A;
    js_args_init(&A);
    if (!js_list_from(J, js_arg(a, n, 1), &A)) { js_args_free(&A); return js_undef(); }
    jval nt = n > 2 ? a[2] : f;
    jval r = js_construct(J, f, A.v, A.n, nt);
    js_args_free(&A);
    return r;
}

static jval nat_reflect_defineprop(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval o = js_arg(a, n, 0);
    if (!js_is_obj(o)) return js_throw(J, JS_ERR_TYPE, "Reflect.defineProperty needs an object", J->error_line);
    jstr *key = js_to_key(J, js_arg(a, n, 1));
    int ok = js_define_from_desc(J, o.obj, key, js_arg(a, n, 2));
    if (J->sig == JS_THROWN && !ok) { J->sig = JS_OK; return js_bool(0); }
    return js_bool(ok);
}

static jval nat_reflect_deleteprop(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return js_bool(js_delete(J, js_arg(a, n, 0), js_to_key(J, js_arg(a, n, 1))));
}

static jval nat_reflect_get(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval o = js_arg(a, n, 0);
    if (!js_is_obj(o)) return js_throw(J, JS_ERR_TYPE, "Reflect.get needs an object", J->error_line);
    return js_getv(J, o, js_to_key(J, js_arg(a, n, 1)), n > 2 ? a[2] : o);
}

/* Reflect.set, as the standard's OrdinarySet has it: the property is looked
   for from the target up; a setter found there runs with the receiver as
   this; a writable data property found there, or none at all, is written on
   the receiver, not where it was found; and the answer is whether it was
   done. It wrote on the target: core-js asks Reflect.set(proto, "a", 1, obj)
   to test for an old browser's bug, and that put an enumerable "a" on
   Object.prototype, which every for-in on the page then walked. */
static jval nat_reflect_set(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval o = js_arg(a, n, 0);
    if (!js_is_obj(o)) return js_throw(J, JS_ERR_TYPE, "Reflect.set needs an object", J->error_line);
    jstr *key = js_to_key(J, js_arg(a, n, 1));
    if (J->sig != JS_OK || !key) return js_undef();
    jval v = js_arg(a, n, 2);
    jval recv = n > 3 ? a[3] : o;

    jval found = js_undef();
    int fl = 0, have = 0, depth = 0;
    for (jobj *q = o.obj; q && !have && depth < 10000; q = q->proto, depth++)
        if (js_get_own(J, q, key, &found, &fl)) have = 1;
    if (have && found.t == JS_ACC) {
        if (!js_callable(found.acc->set)) return js_bool(0);
        js_call(J, found.acc->set, recv, &v, 1);
        return J->sig == JS_OK ? js_bool(1) : js_undef();
    }
    if (have && !(fl & JP_WRITE)) return js_bool(0);
    if (!js_is_obj(recv)) return js_bool(0);

    jobj *r = recv.obj;
    jval mine;
    int mfl;
    if (js_get_own(J, r, key, &mine, &mfl)) {
        if (mine.t == JS_ACC || !(mfl & JP_WRITE)) return js_bool(0);
        /* On a proxy the value is defined, as the standard has it, not set
           through it: its set trap is what very often called this -- Vue's
           does, Reflect.set(target, key, value, receiver) -- and setting
           asked the trap again, for ever. */
        if (r->flags & JOF_PROXY) {
            jobj *d = js_object(J, JO_PLAIN);
            if (!d) return js_undef();
            js_put_prop(J, d, J->s_value, v);
            return js_bool(js_proxy_define(J, r, key, js_from_obj(d)));
        }
        js_putv(J, recv, key, v, recv);
    } else {
        if (r->flags & JOF_NOEXT) return js_bool(0);
        if (r->flags & JOF_PROXY) {
            jobj *d = js_object(J, JO_PLAIN);
            if (!d) return js_undef();
            js_put_prop(J, d, J->s_value, v);
            js_put_prop(J, d, J->s_writable, js_bool(1));
            js_put_prop(J, d, J->s_enumerable, js_bool(1));
            js_put_prop(J, d, J->s_configurable, js_bool(1));
            return js_bool(js_proxy_define(J, r, key, js_from_obj(d)));
        }
        if (r == o.obj) js_putv(J, recv, key, v, recv);
        else js_define(J, r, key, v, JP_PLAIN);
    }
    return J->sig == JS_OK ? js_bool(1) : js_undef();
}

static jval nat_reflect_has(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval o = js_arg(a, n, 0);
    if (!js_is_obj(o)) return js_throw(J, JS_ERR_TYPE, "Reflect.has needs an object", J->error_line);
    return js_bool(js_has(J, o.obj, js_to_key(J, js_arg(a, n, 1))));
}

static jval nat_reflect_ownkeys(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return js_own_names(J, js_arg(a, n, 0), JK_STR | JK_SYM);
}

static jval nat_reflect_prevent(jctx *J, jval t, jval *a, int n) {
    nat_obj_prevent_ext(J, t, a, n);
    return js_bool(1);
}

static jval nat_reflect_setproto(jctx *J, jval t, jval *a, int n) {
    nat_obj_setproto(J, t, a, n);
    if (J->sig == JS_THROWN) { J->sig = JS_OK; return js_bool(0); }
    return js_bool(1);
}

/* --- the global functions ------------------------------------------------------ */

/* Where console.log goes. A program sets this; with nothing set the output
   is dropped, which is what a browser tab with no console open does. */
static void (*js_print_hook)(const char *s, u32 n);

static jval nat_log(jctx *J, jval t, jval *a, int n) {
    (void)t;
    for (int i = 0; i < n; i++) {
        if (i && js_print_hook) js_print_hook(" ", 1);
        jstr *s = a[i].t == JS_SYM ? js_str(J, "Symbol()") : js_to_str(J, a[i]);
        if (J->sig != JS_OK) { J->sig = JS_OK; s = js_str(J, "[object]"); }
        if (s && js_print_hook) js_print_hook(s->s, s->len);
    }
    if (js_print_hook) js_print_hook("\n", 1);
    return js_undef();
}

static jval nat_nothing(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_undef();
}

static jval nat_eval(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (n < 1 || a[0].t != JS_STR) return js_arg(a, n, 0);
    return js_eval_source(J, a[0].str, J->global, J->global_lex, js_from_obj(J->global_obj));
}

static jval nat_queue_microtask(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval fn = js_arg(a, n, 0);
    if (!js_callable(fn)) return js_throw(J, JS_ERR_TYPE, "queueMicrotask needs a function", J->error_line);
    js_enqueue(J, JOB_CALL, fn, js_undef(), 0, 0);
    return js_undef();
}

/* %XX for every byte that is not one of `keep`, as encodeURIComponent and
   encodeURI write a string: in UTF-8, which is what the string already is. */
static jval js_uri_encode(jctx *J, jval *a, int n, const char *keep) {
    static const char HEX[] = "0123456789ABCDEF";
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    jtext o = { 0, 0, 0, 0 };
    for (u32 i = 0; i < s->len; i++) {
        u8 c = (u8)s->s[i];
        int plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        for (const char *k = keep; !plain && *k; k++) if (c == (u8)*k) plain = 1;
        if (plain) { char ch = (char)c; jt_put(J, &o, &ch, 1); continue; }
        char e[3] = { '%', HEX[c >> 4], HEX[c & 15] };
        jt_put(J, &o, e, 3);
    }
    return js_from_str(jt_done(J, &o));
}

static jval nat_encode_uri_component(jctx *J, jval t, jval *a, int n) { (void)t; return js_uri_encode(J, a, n, "-_.!~*'()"); }
static jval nat_encode_uri(jctx *J, jval t, jval *a, int n) { (void)t; return js_uri_encode(J, a, n, "-_.!~*'();/?:@&=+$,#"); }

static jval js_uri_decode(jctx *J, jval *a, int n, const char *reserved) {
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    jtext o = { 0, 0, 0, 0 };
    for (u32 i = 0; i < s->len; i++) {
        char c = s->s[i];
        if (c != '%') { jt_put(J, &o, &c, 1); continue; }
        int h1 = i + 1 < s->len ? js_hexval(s->s[i + 1]) : -1;
        int h2 = i + 2 < s->len ? js_hexval(s->s[i + 2]) : -1;
        if (h1 < 0 || h2 < 0) {
            free(o.b);
            return js_throw(J, JS_ERR_URI, "a % in the text is not followed by two hex digits", J->error_line);
        }
        char b = (char)(h1 * 16 + h2);
        int keep = 0;
        for (const char *r = reserved; *r; r++) if (b == *r) keep = 1;
        if (keep) jt_put(J, &o, s->s + i, 3);
        else jt_put(J, &o, &b, 1);
        i += 2;
    }
    return js_from_str(jt_done(J, &o));
}

static jval nat_decode_uri_component(jctx *J, jval t, jval *a, int n) { (void)t; return js_uri_decode(J, a, n, ""); }
static jval nat_decode_uri(jctx *J, jval t, jval *a, int n) { (void)t; return js_uri_decode(J, a, n, ";/?:@&=+$,#"); }

static jval nat_escape(jctx *J, jval t, jval *a, int n) { (void)t; return js_uri_encode(J, a, n, "@*_+-./"); }
static jval nat_unescape(jctx *J, jval t, jval *a, int n) { (void)t; return js_uri_decode(J, a, n, ""); }

#include "jsarr.h"
#include "jsprom.h"
#include "jstyped.h"
#include "jsproxy.h"
#include "jsbig.h"
#include "jsclone.h"
#include "jsintl.h"

/* --- setting it all up ------------------------------------------------------ */

static void js_globals(jctx *J) {
    jscope *g = J->global;

    /* The prototypes first, everything else is made with them. */
    J->p_object = js_object_with(J, JO_PLAIN, 0);
    J->p_function = js_object_with(J, JO_NATIVE, J->p_object);
    if (J->p_function) {
        J->p_function->fn = nat_fn_proto;
        J->p_function->name = js_str(J, "");
    }
    J->p_array = js_object_with(J, JO_ARRAY, J->p_object);
    J->p_string = js_object_with(J, JO_BOXED, J->p_object);
    if (J->p_string) J->p_string->ival = js_from_str(js_str(J, ""));
    J->p_number = js_object_with(J, JO_BOXED, J->p_object);
    if (J->p_number) J->p_number->ival = js_num(0);
    J->p_boolean = js_object_with(J, JO_BOXED, J->p_object);
    if (J->p_boolean) J->p_boolean->ival = js_bool(0);
    J->p_symbol = js_object_with(J, JO_PLAIN, J->p_object);
    J->p_error = js_object_with(J, JO_PLAIN, J->p_object);
    J->p_iterator = js_object_with(J, JO_PLAIN, J->p_object);
    J->p_async_iterator = js_object_with(J, JO_PLAIN, J->p_object);

    /* Object */
    jobj *op = J->p_object;
    jobj *object = js_ctor(J, "Object", nat_object_make, 1, op);
    J->c_object = object;
    js_method(J, op, "hasOwnProperty", nat_obj_hasown_proto, 1);
    js_method(J, op, "isPrototypeOf", nat_obj_isprototypeof, 1);
    js_method(J, op, "propertyIsEnumerable", nat_obj_propisenum, 1);
    js_method(J, op, "toString", nat_obj_tostring, 0);
    js_method(J, op, "toLocaleString", nat_obj_tolocalestring, 0);
    js_method(J, op, "valueOf", nat_obj_valueof, 0);
    js_method(J, op, "__defineGetter__", nat_obj_define_getter, 2);
    js_method(J, op, "__defineSetter__", nat_obj_define_setter, 2);
    js_method(J, op, "__lookupGetter__", nat_obj_lookup_getter, 1);
    js_method(J, op, "__lookupSetter__", nat_obj_lookup_setter, 1);
    {
        jobj *pg = js_native(J, "get __proto__", nat_obj_proto_get);
        jobj *ps = js_native(J, "set __proto__", nat_obj_proto_set);
        js_define_accessor(J, op, J->s_proto, js_from_obj(pg), js_from_obj(ps), JP_CONF);
    }
    js_method(J, object, "keys", nat_obj_keys, 1);
    js_method(J, object, "values", nat_obj_values, 1);
    js_method(J, object, "entries", nat_obj_entries, 1);
    js_method(J, object, "assign", nat_obj_assign, 2);
    js_method(J, object, "freeze", nat_obj_freeze, 1);
    js_method(J, object, "isFrozen", nat_obj_isfrozen, 1);
    js_method(J, object, "seal", nat_obj_seal, 1);
    js_method(J, object, "isSealed", nat_obj_issealed, 1);
    js_method(J, object, "preventExtensions", nat_obj_prevent_ext, 1);
    js_method(J, object, "isExtensible", nat_obj_isextensible, 1);
    js_method(J, object, "getPrototypeOf", nat_obj_getproto, 1);
    js_method(J, object, "setPrototypeOf", nat_obj_setproto, 2);
    js_method(J, object, "create", nat_obj_create, 2);
    js_method(J, object, "defineProperty", nat_obj_defineprop, 3);
    js_method(J, object, "defineProperties", nat_obj_defineprops, 2);
    js_method(J, object, "getOwnPropertyNames", nat_obj_getownnames, 1);
    js_method(J, object, "getOwnPropertySymbols", nat_obj_getownsyms, 1);
    js_method(J, object, "getOwnPropertyDescriptor", nat_obj_getownpropdesc, 2);
    js_method(J, object, "getOwnPropertyDescriptors", nat_obj_getownpropdescs, 1);
    js_method(J, object, "fromEntries", nat_obj_fromentries, 1);
    js_method(J, object, "is", nat_obj_is, 2);
    js_method(J, object, "hasOwn", nat_obj_hasown, 2);
    js_method(J, object, "groupBy", nat_obj_groupby, 2);

    /* Function */
    jobj *fp = J->p_function;
    jobj *function = js_ctor(J, "Function", nat_function_make, 1, fp);
    J->c_function = function;
    js_method(J, fp, "call", nat_fn_call, 1);
    js_method(J, fp, "apply", nat_fn_apply, 2);
    js_method(J, fp, "bind", nat_fn_bind, 1);
    js_method(J, fp, "toString", nat_fn_tostring, 0);
    {
        jobj *hi = js_native_n(J, "[Symbol.hasInstance]", nat_fn_hasinstance, 1);
        if (hi) js_put_prop_flags(J, fp, J->sym_has_instance, js_from_obj(hi), 0);
    }

    /* Symbol */
    jobj *sp = J->p_symbol;
    jobj *symbol = js_ctor(J, "Symbol", nat_symbol_make, 0, sp);
    js_method(J, symbol, "for", nat_symbol_for, 1);
    js_method(J, symbol, "keyFor", nat_symbol_keyfor, 1);
    js_const_prop(J, symbol, "iterator", js_from_sym(J->sym_iterator));
    js_const_prop(J, symbol, "asyncIterator", js_from_sym(J->sym_async_iterator));
    js_const_prop(J, symbol, "hasInstance", js_from_sym(J->sym_has_instance));
    js_const_prop(J, symbol, "toPrimitive", js_from_sym(J->sym_to_primitive));
    js_const_prop(J, symbol, "toStringTag", js_from_sym(J->sym_to_string_tag));
    js_const_prop(J, symbol, "species", js_from_sym(J->sym_species));
    js_const_prop(J, symbol, "unscopables", js_from_sym(J->sym_unscopables));
    js_const_prop(J, symbol, "matchAll", js_from_sym(J->sym_match_all));
    js_method(J, sp, "toString", nat_symbol_tostring, 0);
    js_method(J, sp, "valueOf", nat_symbol_valueof, 0);
    js_getter(J, sp, "description", nat_symbol_desc);
    js_method_key(J, sp, J->sym_to_primitive, "[Symbol.toPrimitive]", nat_symbol_valueof, 1);
    js_tag(J, sp, "Symbol");

    /* Errors */
    for (int k = 0; k < 8; k++) {
        jobj *proto = k == 0 ? J->p_error : js_object_with(J, JO_PLAIN, J->p_error);
        J->err_proto[k] = proto;
        jobj *c = js_ctor(J, JS_ERR_NAMES[k], nat_error_make, k == JS_ERR_AGGREGATE ? 2 : 1, proto);
        J->err_ctor[k] = c;
        if (c) c->data = js_num(k);
        if (c && k) c->proto = J->err_ctor[0];
        js_put_prop_flags(J, proto, J->s_name, js_from_str(js_str(J, JS_ERR_NAMES[k])), JP_WRITE | JP_CONF);
        js_put_prop_flags(J, proto, J->s_message, js_from_str(js_str(J, "")), JP_WRITE | JP_CONF);
    }
    js_method(J, J->p_error, "toString", nat_error_tostring, 0);
    js_method(J, J->err_ctor[0], "captureStackTrace", nat_error_capture, 2);
    js_put_prop(J, J->err_ctor[0], js_str(J, "stackTraceLimit"), js_num(10));

    /* Boolean */
    jobj *bp = J->p_boolean;
    js_ctor(J, "Boolean", nat_bool_ctor, 1, bp);
    js_method(J, bp, "toString", nat_bool_tostring, 0);
    js_method(J, bp, "valueOf", nat_bool_valueof, 0);

    /* Number */
    jobj *np = J->p_number;
    jobj *number = js_ctor(J, "Number", nat_num_ctor, 1, np);
    js_method(J, np, "toString", nat_num_tostring, 1);
    js_method(J, np, "toFixed", nat_num_tofixed, 1);
    js_method(J, np, "toPrecision", nat_num_toprecision, 1);
    js_method(J, np, "toExponential", nat_num_toexponential, 1);
    js_method(J, np, "valueOf", nat_num_valueof, 0);
    js_method(J, np, "toLocaleString", nat_num_tolocalestring, 0);
    js_method(J, number, "isInteger", nat_number_isinteger, 1);
    js_method(J, number, "isSafeInteger", nat_number_issafe, 1);
    js_method(J, number, "isFinite", nat_number_isfinite, 1);
    js_method(J, number, "isNaN", nat_number_isnan, 1);
    jobj *pi = js_method(J, number, "parseInt", nat_parseint, 2);
    jobj *pf = js_method(J, number, "parseFloat", nat_parsefloat, 1);
    js_const_prop(J, number, "EPSILON", js_num(2.220446049250313e-16));
    js_const_prop(J, number, "MAX_SAFE_INTEGER", js_num(9007199254740991.0));
    js_const_prop(J, number, "MIN_SAFE_INTEGER", js_num(-9007199254740991.0));
    js_const_prop(J, number, "MAX_VALUE", js_num(1.7976931348623157e308));
    js_const_prop(J, number, "MIN_VALUE", js_num(5e-324));
    js_const_prop(J, number, "POSITIVE_INFINITY", js_num(1e308 * 10));
    js_const_prop(J, number, "NEGATIVE_INFINITY", js_num(-1e308 * 10));
    js_const_prop(J, number, "NaN", js_num(js_nan()));

    /* Math */
    jobj *math = js_object(J, JO_PLAIN);
    struct { const char *n; jnative f; int k; } M[] = {
        { "floor", nat_m_floor, 1 }, { "ceil", nat_m_ceil, 1 }, { "round", nat_m_round, 1 },
        { "trunc", nat_m_trunc, 1 }, { "abs", nat_m_abs, 1 }, { "sqrt", nat_m_sqrt, 1 },
        { "sin", nat_m_sin, 1 }, { "cos", nat_m_cos, 1 }, { "tan", nat_m_tan, 1 },
        { "atan", nat_m_atan, 1 }, { "asin", nat_m_asin, 1 }, { "acos", nat_m_acos, 1 },
        { "exp", nat_m_exp, 1 }, { "log", nat_m_log, 1 }, { "log2", nat_m_log2, 1 },
        { "log10", nat_m_log10, 1 }, { "sign", nat_m_sign, 1 }, { "cbrt", nat_m_cbrt, 1 },
        { "expm1", nat_m_expm1, 1 }, { "log1p", nat_m_log1p, 1 }, { "sinh", nat_m_sinh, 1 },
        { "cosh", nat_m_cosh, 1 }, { "tanh", nat_m_tanh, 1 }, { "asinh", nat_m_asinh, 1 },
        { "acosh", nat_m_acosh, 1 }, { "atanh", nat_m_atanh, 1 }, { "fround", nat_m_fround, 1 },
        { "clz32", nat_m_clz32, 1 }, { "min", nat_m_min, 2 }, { "max", nat_m_max, 2 },
        { "pow", nat_m_pow, 2 }, { "atan2", nat_m_atan2, 2 }, { "hypot", nat_m_hypot, 2 },
        { "imul", nat_m_imul, 2 }, { "random", nat_m_random, 0 }, { 0, 0, 0 }
    };
    for (int i = 0; M[i].n; i++) js_method(J, math, M[i].n, M[i].f, M[i].k);
    js_const_prop(J, math, "PI", js_num(3.141592653589793));
    js_const_prop(J, math, "E", js_num(2.718281828459045));
    js_const_prop(J, math, "LN2", js_num(0.6931471805599453));
    js_const_prop(J, math, "LN10", js_num(2.302585092994046));
    js_const_prop(J, math, "LOG2E", js_num(1.4426950408889634));
    js_const_prop(J, math, "LOG10E", js_num(0.4342944819032518));
    js_const_prop(J, math, "SQRT2", js_num(1.4142135623730951));
    js_const_prop(J, math, "SQRT1_2", js_num(0.7071067811865476));
    js_tag(J, math, "Math");
    js_declare_flags(J, g, js_str(J, "Math"), js_from_obj(math), JP_WRITE | JP_CONF);

    /* JSON */
    jobj *json = js_object(J, JO_PLAIN);
    js_method(J, json, "stringify", nat_json_stringify, 3);
    js_method(J, json, "parse", nat_json_parse, 2);
    js_tag(J, json, "JSON");
    js_declare_flags(J, g, js_str(J, "JSON"), js_from_obj(json), JP_WRITE | JP_CONF);

    /* Reflect */
    jobj *reflect = js_object(J, JO_PLAIN);
    js_method(J, reflect, "apply", nat_reflect_apply, 3);
    js_method(J, reflect, "construct", nat_reflect_construct, 2);
    js_method(J, reflect, "defineProperty", nat_reflect_defineprop, 3);
    js_method(J, reflect, "deleteProperty", nat_reflect_deleteprop, 2);
    js_method(J, reflect, "get", nat_reflect_get, 2);
    js_method(J, reflect, "set", nat_reflect_set, 3);
    js_method(J, reflect, "has", nat_reflect_has, 2);
    js_method(J, reflect, "ownKeys", nat_reflect_ownkeys, 1);
    js_method(J, reflect, "getPrototypeOf", nat_obj_getproto, 1);
    js_method(J, reflect, "setPrototypeOf", nat_reflect_setproto, 2);
    js_method(J, reflect, "getOwnPropertyDescriptor", nat_obj_getownpropdesc, 2);
    js_method(J, reflect, "isExtensible", nat_obj_isextensible, 1);
    js_method(J, reflect, "preventExtensions", nat_reflect_prevent, 1);
    js_tag(J, reflect, "Reflect");
    js_declare_flags(J, g, js_str(J, "Reflect"), js_from_obj(reflect), JP_WRITE | JP_CONF);

    js_proxy_init(J);

    /* console */
    jobj *console = js_object(J, JO_PLAIN);
    static const char *const LOGS[] = { "log", "info", "warn", "error", "debug", "trace", "dir", 0 };
    for (int i = 0; LOGS[i]; i++) js_method(J, console, LOGS[i], nat_log, 0);
    static const char *const QUIET[] = { "table", "group", "groupCollapsed", "groupEnd", "time",
                                         "timeEnd", "timeLog", "assert", "count", "countReset",
                                         "clear", "profile", "profileEnd", 0 };
    for (int i = 0; QUIET[i]; i++) js_method(J, console, QUIET[i], nat_nothing, 0);
    js_declare(J, g, js_str(J, "console"), js_from_obj(console));

    /* The functions that are not anybody's methods. */
    jobj *ev = js_method(J, 0, "eval", nat_eval, 1);
    J->eval_fn = ev;
    js_declare_flags(J, g, js_str(J, "eval"), js_from_obj(ev), JP_WRITE | JP_CONF);
    if (pi) js_declare_flags(J, g, js_str(J, "parseInt"), js_from_obj(pi), JP_WRITE | JP_CONF);
    if (pf) js_declare_flags(J, g, js_str(J, "parseFloat"), js_from_obj(pf), JP_WRITE | JP_CONF);
    struct { const char *n; jnative f; int k; } G[] = {
        { "isNaN", nat_isnan, 1 }, { "isFinite", nat_isfinite, 1 },
        { "encodeURIComponent", nat_encode_uri_component, 1 }, { "encodeURI", nat_encode_uri, 1 },
        { "decodeURIComponent", nat_decode_uri_component, 1 }, { "decodeURI", nat_decode_uri, 1 },
        { "escape", nat_escape, 1 }, { "unescape", nat_unescape, 1 },
        { "queueMicrotask", nat_queue_microtask, 1 }, { 0, 0, 0 }
    };
    for (int i = 0; G[i].n; i++) {
        jobj *f = js_method(J, 0, G[i].n, G[i].f, G[i].k);
        js_declare_flags(J, g, js_str(J, G[i].n), js_from_obj(f), JP_WRITE | JP_CONF);
    }

    js_setup_arrays(J);
    js_setup_strings(J);
    js_setup_regexps(J);
    js_setup_collections(J);
    js_setup_promises(J);
    js_setup_generators(J);
    js_setup_dates(J);
    js_setup_typed(J);
    js_setup_text(J);
    js_setup_bigint(J);
    js_setup_clone(J);
    js_setup_intl(J);

    js_declare_flags(J, g, js_str(J, "NaN"), js_num(js_nan()), 0);
    js_declare_flags(J, g, js_str(J, "Infinity"), js_num(1e308 * 10), 0);
    js_declare_flags(J, g, js_str(J, "undefined"), js_undef(), 0);
    js_declare_flags(J, g, js_str(J, "globalThis"), js_from_obj(J->global_obj), JP_WRITE | JP_CONF);
    /* self is the global object in a window and in a worker alike, and the
       bundles that must run in both reach it that way first: the BBC's
       stopped at its first line without it. */
    js_declare_flags(J, g, js_str(J, "self"), js_from_obj(J->global_obj), JP_WRITE | JP_CONF);
}
