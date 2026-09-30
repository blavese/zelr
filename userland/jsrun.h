/* Running JavaScript: conversions, the evaluator, and the way in.
 *
 * A tree walker. Each node is looked at and done; there is no bytecode and no
 * compilation step, which costs speed and buys the ability to read this file
 * and know what the machine does. The built-in objects are in jslib.h, and
 * the machinery for a function that stops half way (a generator, an async
 * function) is in jsco.h and at the end of this file.
 *
 * The rules that look strange in this language are mostly conversion rules,
 * and they are strange on purpose rather than by accident: `"5" - 2` is 3 and
 * `"5" + 2` is "52" because plus is the one operator that means concatenate
 * when either side is a string, and everything else is arithmetic. A page
 * relies on that far more often than anybody would like, so it is
 * implemented rather than tidied up.
 */
#pragma once
#include "js.h"
#include "jsnum.h"
#include "jsparse.h"
#include "jsregex.h"
#include "jsco.h"

#define JS_NOINLINE __attribute__((noinline))

static jval js_eval(jctx *J, int node, jscope *sc, jval this_val);
static jsignal js_exec(jctx *J, int node, jscope *sc, jval this_val);
static jval js_call(jctx *J, jval fn, jval this_val, jval *argv, int argc);
static jval js_construct(jctx *J, jval fn, jval *argv, int argc, jval new_target);
static jval js_get(jctx *J, jval target, jstr *key);
static jval js_getv(jctx *J, jval target, jstr *key, jval receiver);
static void js_put(jctx *J, jval target, jstr *key, jval v);
static jstr *js_to_str(jctx *J, jval v);
static double js_to_num(jctx *J, jval v);
static jobj *js_native(jctx *J, const char *name, jnative fn);
static void js_drain(jctx *J);
/* BigInt (jsbig.h, which comes after jslib.h). */
static jval js_from_big(jbint *b);
static jbint *jsb_literal(jctx *J, const jstr *s);
static jval jsb_negate(jctx *J, jval v);
static jval jsb_bitnot(jctx *J, jval v);
static jval jsb_step(jctx *J, jval v, int up);
static u32 jsb_hash(const jbint *a);
static double jsb_to_double(jctx *J, const jbint *a);
static jobj *js_promise_new(jctx *J);
static void js_promise_settle(jctx *J, jobj *p, int ok, jval v);
static void js_promise_resolve_with(jctx *J, jobj *p, jval v);
static jobj *js_promise_of(jctx *J, jval v);
static void js_promise_await(jctx *J, jobj *p, jco *co);

/* A Proxy's traps (jsproxy.h), where the ordinary paths meet one. */
static jval js_proxy_get(jctx *J, jobj *p, jstr *key, jval receiver);
static void js_proxy_set(jctx *J, jobj *p, jstr *key, jval v, jval receiver);
static int  js_proxy_set_inherited(jctx *J, jobj *p, jstr *key, jval v, jval receiver);
static int  js_proxy_has(jctx *J, jobj *p, jstr *key);
static int  js_proxy_delete(jctx *J, jobj *p, jstr *key);
static int  js_proxy_get_own(jctx *J, jobj *p, jstr *key, jval *v, int *flags);
static int  js_proxy_is_array(jctx *J, jobj *p);
static jval js_proxy_getproto(jctx *J, jobj *p);
static jval js_iter_result(jctx *J, jval value, int done);
static jobj *js_to_object(jctx *J, jval v);
static int js_array_join_fast(jctx *J, jval v, jstr **out);
static double js_pow(double a, double b);
static int js_tick(jctx *J);
static jobj *js_regex_new(jctx *J, const char *pat, u32 len, int flags);

/* --- saying what went wrong ----------------------------------------------
 *
 * An error is an object with a name and a message, made by one of the
 * constructors, and `e instanceof TypeError` is how a page tells a mistake
 * it expected from one it did not. */
enum { JS_ERR_ERROR, JS_ERR_TYPE, JS_ERR_RANGE, JS_ERR_REFERENCE, JS_ERR_SYNTAX,
       JS_ERR_EVAL, JS_ERR_URI, JS_ERR_AGGREGATE };
static const char *const JS_ERR_NAMES[8] = {
    "Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "EvalError",
    "URIError", "AggregateError"
};

/* An error object, of the kind whose prototype is given. `stack` is what a
   great many libraries read on every error they see, and it is here as the
   one line this engine can honestly give. */
static jobj *js_error_with(jctx *J, jobj *proto, jstr *name, jstr *message) {
    jobj *e = js_object_with(J, JO_ERROR, proto ? proto : J->p_error);
    if (!e) return 0;
    jprop *p = js_put_prop(J, e, J->s_message, js_from_str(message));
    if (p) p->flags = JP_WRITE | JP_CONF;
    jstr *st = name;
    if (message && message->len) {
        char buf[240];
        u32 w = 0;
        for (u32 i = 0; name && i < name->len && w < 100; i++) buf[w++] = name->s[i];
        buf[w++] = ':';
        buf[w++] = ' ';
        for (u32 i = 0; i < message->len && w < 200; i++) buf[w++] = message->s[i];
        const char *at = "\n    at <anonymous>";
        for (int i = 0; at[i]; i++) buf[w++] = at[i];
        st = js_str_n(J, buf, w);
    }
    p = js_put_prop(J, e, J->s_stack, js_from_str(st));
    if (p) p->flags = JP_WRITE | JP_CONF;
    return e;
}

static jval js_throw(jctx *J, int kind, const char *what, int line) {
    if (J->sig == JS_FAILED) return js_undef();
    J->sig = JS_THROWN;
    J->error_line = line;
    /* What the page's error line shows, if nothing catches it. */
    const char *nm = JS_ERR_NAMES[kind];
    int i = 0;
    for (; nm[i] && i < (int)sizeof(J->error) - 3; i++) J->error[i] = nm[i];
    J->error[i++] = ':';
    J->error[i++] = ' ';
    for (int k = 0; what[k] && i < (int)sizeof(J->error) - 1; k++) J->error[i++] = what[k];
    J->error[i] = 0;
    jobj *e = js_error_with(J, J->err_proto[kind], js_str(J, nm), js_str(J, what));
    J->ret = e ? js_from_obj(e) : js_from_str(js_str(J, what));
    return js_undef();
}

/* A message with a name in it: "x is not defined", "cannot read y of null". */
static jval js_throw_named(jctx *J, int kind, const char *before, jstr *name, const char *after) {
    char msg[160];
    int n = 0;
    for (const char *p = before; *p && n < 60; p++) msg[n++] = *p;
    for (u32 i = 0; name && !js_is_sym_key(name) && i < name->len && n < 110; i++) msg[n++] = name->s[i];
    if (name && js_is_sym_key(name)) for (const char *p = "a symbol"; *p; p++) msg[n++] = *p;
    for (const char *p = after; *p && n < 159; p++) msg[n++] = *p;
    msg[n] = 0;
    return js_throw(J, kind, msg, J->error_line);
}

/* --- conversions --------------------------------------------------------- */

static int js_to_bool(jval v) {
    switch (v.t) {
        case JS_UNDEF: case JS_NULL: return 0;
        case JS_BOOL: return v.b;
        /* NaN is false, and so is zero, and -0 is zero. */
        case JS_NUM:  return !(v.num == 0 || v.num != v.num);
        case JS_STR:  return v.str && v.str->len > 0;
        case JS_BIG:  return v.big->n != 0;
        default: return 1;
    }
}

static int js_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 11 || c == 12; }

static double js_nan(void) { double z = 0.0; return z / z; }

/* A sign, then Infinity or a decimal, at the start of s; how much of s that
   was in *used, nought if it was neither. */
static double js_signed_decimal(const char *s, u32 n, u32 *used) {
    u32 i = 0;
    int neg = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    double v;
    int inf = n - i >= 8;
    for (int k = 0; inf && k < 8; k++) if (s[i + k] != "Infinity"[k]) inf = 0;
    if (inf) {
        v = jn_bits(JN_INF);
        i += 8;
    } else {
        u32 k = 0;
        v = js_decimal(s + i, n - i, &k);
        if (!k) { *used = 0; return 0.0; }
        i += k;
    }
    *used = i;
    return neg ? -v : v;
}

static double js_str_to_num(const char *s, u32 n) {
    u32 i = 0;
    while (i < n && js_blank(s[i])) i++;
    while (n > i && js_blank(s[n - 1])) n--;
    if (i >= n) return 0;                       /* an empty string is zero */

    /* 0x, 0o and 0b, which Number() reads and parseFloat does not. */
    if (n - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X' || s[i + 1] == 'o'
                                     || s[i + 1] == 'O' || s[i + 1] == 'b' || s[i + 1] == 'B')) {
        int base = (s[i + 1] | 32) == 'x' ? 16 : (s[i + 1] | 32) == 'o' ? 8 : 2;
        double v = 0;
        for (u32 k = i + 2; k < n; k++) {
            int d = js_hexval(s[k]);
            if (d < 0 || d >= base) return js_nan();
            v = v * base + d;
        }
        return v;
    }

    u32 k = 0;
    double v = js_signed_decimal(s + i, n - i, &k);
    /* Trailing rubbish makes the whole thing not a number, which is the rule
       and is why "12px" is NaN while parseInt("12px") is 12. */
    if (!k || i + k != n) return js_nan();
    return v;
}

/* The value an object stands for when an operator needs a primitive: its
   Symbol.toPrimitive, or valueOf and toString in the order the hint says.
   `hint` is 0 for none, 1 for a number and 2 for a string. */
static jval nat_obj_valueof(jctx *J, jval t, jval *a, int n);
static jval js_to_primitive(jctx *J, jval v, int hint) {
    if (v.t != JS_OBJ || !v.obj) return v;
    jval ex = js_get(J, v, J->sym_to_primitive);
    if (J->sig != JS_OK) return js_undef();
    if (js_callable(ex)) {
        jval h = js_from_str(js_str(J, hint == 1 ? "number" : hint == 2 ? "string" : "default"));
        jval r = js_call(J, ex, v, &h, 1);
        if (J->sig != JS_OK) return js_undef();
        if (r.t == JS_OBJ) return js_throw(J, JS_ERR_TYPE, "Symbol.toPrimitive gave back an object", J->error_line);
        return r;
    }
    /* An array's text without a call, which is what nearly every use of an
       array as a primitive wants, and what it costs most to get the long way. */
    if (v.obj->kind == JO_ARRAY) {
        jstr *s;
        if (js_array_join_fast(J, v, &s)) return js_from_str(s);
    }
    for (int round = 0; round < 2; round++) {
        int str_first = hint == 2;
        jstr *name = (round == 0) == str_first ? J->s_toString : J->s_valueOf;
        jval m = js_get(J, v, name);
        if (J->sig != JS_OK) return js_undef();
        if (!js_callable(m)) continue;
        if (m.obj->kind == JO_NATIVE && m.obj->fn == nat_obj_valueof) continue;
        jval r = js_call(J, m, v, 0, 0);
        if (J->sig != JS_OK) return js_undef();
        if (r.t != JS_OBJ) return r;
    }
    return js_throw(J, JS_ERR_TYPE, "this object cannot be made into a primitive", J->error_line);
}

static double js_to_num(jctx *J, jval v) {
    switch (v.t) {
        case JS_NUM:  return v.num;
        case JS_BOOL: return v.b ? 1.0 : 0.0;
        case JS_NULL: return 0.0;
        case JS_STR:  return v.str ? js_str_to_num(v.str->s, v.str->len) : 0.0;
        case JS_SYM:
            js_throw(J, JS_ERR_TYPE, "a symbol cannot be made into a number", J->error_line);
            return js_nan();
        case JS_BIG:
            js_throw(J, JS_ERR_TYPE, "a BigInt is made into a number with Number()", J->error_line);
            return js_nan();
        case JS_OBJ: {
            jval p = js_to_primitive(J, v, 1);
            if (J->sig != JS_OK) return js_nan();
            return js_to_num(J, p);
        }
        default: return js_nan();
    }
}

/* The thirty-two bit truncation the bitwise operators use: the whole number
   modulo 2^32, which for a double far past 2^63 is found from its bits
   rather than through a cast that would have no answer to give. */
static int js_d_to_i32(double d) {
    if (d != d || d - d != 0) return 0;
    if (d < 9.2e18 && d > -9.2e18) {
        long long t = (long long)d;
        return (int)(u32)(t & 0xFFFFFFFFll);
    }
    union { double d; u64 u; } x;
    x.d = d;
    int e = (int)((x.u >> 52) & 0x7FF) - 1075;
    u64 m = (x.u & 0xFFFFFFFFFFFFFull) | (1ull << 52);
    u32 r = e >= 64 ? 0 : (u32)(m << e);
    if (x.u >> 63) r = (u32)(0u - r);
    return (int)r;
}

static int js_to_i32(jctx *J, jval v) {
    if (v.t == JS_NUM) return js_d_to_i32(v.num);
    return js_d_to_i32(js_to_num(J, v));
}

static u32 js_to_u32(jctx *J, jval v) { return (u32)js_to_i32(J, v); }

/* A whole number, as ToIntegerOrInfinity makes one, for positions. */
static double js_trunc(double d) {
    if (d != d) return 0;
    if (d - d != 0 || d >= 4503599627370496.0 || d <= -4503599627370496.0) return d;
    double t = (double)(long long)d;
    return t;
}

static jstr *js_concat(jctx *J, jstr *a, jstr *b);
static jstr *jsb_to_str(jctx *J, const jbint *a, int radix);

static jstr *js_to_str(jctx *J, jval v) {
    char buf[64];
    switch (v.t) {
        case JS_UNDEF: return js_str(J, "undefined");
        case JS_NULL:  return js_str(J, "null");
        case JS_BOOL:  return js_str(J, v.b ? "true" : "false");
        case JS_NUM: {
            u32 n = js_num_text(v.num, buf, sizeof(buf));
            return js_str_n(J, buf, n);
        }
        case JS_STR: return v.str ? v.str : js_str(J, "");
        case JS_BIG: return jsb_to_str(J, v.big, 10);
        case JS_SYM:
            js_throw(J, JS_ERR_TYPE, "a symbol cannot be made into text without String()",
                     J->error_line);
            return js_str(J, "");
        case JS_OBJ: {
            jval p = js_to_primitive(J, v, 2);
            if (J->sig != JS_OK) return js_str(J, "");
            if (p.t == JS_OBJ) return js_str(J, "[object Object]");
            return js_to_str(J, p);
        }
        default: return js_str(J, "");
    }
}

/* A property key from a value: a symbol is its own key, anything else its
   text. */
static jstr *js_to_key(jctx *J, jval v) {
    if (v.t == JS_SYM) return v.str;
    if (v.t == JS_STR) return v.str;
    if (v.t == JS_NUM) {
        char buf[40];
        double d = v.num;
        if (d >= 0 && d < 1e9 && d == (double)(int)d) {
            /* Small whole numbers are nearly every key a number becomes, and
               the smallest are made once: a loop over an object by index
               made a string for every turn, which a long loop turned into
               the whole of the page's memory. */
            int k = (int)d, w = 0;
            if (k < JS_INT_KEYS && J->int_keys[k]) return J->int_keys[k];
            char rev[12];
            do { rev[w++] = (char)('0' + k % 10); k /= 10; } while (k);
            for (int i = 0; i < w; i++) buf[i] = rev[w - 1 - i];
            jstr *s = js_str_n(J, buf, (u32)w);
            if ((int)d < JS_INT_KEYS) J->int_keys[(int)d] = s;
            return s;
        }
        u32 n = js_num_text(d, buf, sizeof(buf));
        return js_str_n(J, buf, n);
    }
    if (v.t == JS_OBJ) {
        jval p = js_to_primitive(J, v, 2);
        if (J->sig != JS_OK) return js_str(J, "");
        return js_to_key(J, p);
    }
    return js_to_str(J, v);
}

static jstr *js_concat(jctx *J, jstr *a, jstr *b) {
    u32 n = (a ? a->len : 0) + (b ? b->len : 0);
    jstr *out = (jstr *)js_alloc(J, (u32)sizeof(jstr) + n + 1);
    if (!out) return a;
    u32 w = 0;
    for (u32 i = 0; a && i < a->len; i++) out->s[w++] = a->s[i];
    for (u32 i = 0; b && i < b->len; i++) out->s[w++] = b->s[i];
    out->s[w] = 0;
    out->len = w;
    out->hash = js_hash(out->s, w);
    return out;
}

static const char *js_typeof_name(jval v) {
    switch (v.t) {
        case JS_UNDEF: return "undefined";
        case JS_NULL:  return "object";            /* the famous one */
        case JS_BOOL:  return "boolean";
        case JS_NUM:   return "number";
        case JS_STR:   return "string";
        case JS_SYM:   return "symbol";
        case JS_BIG:   return "bigint";
        case JS_OBJ:   return js_callable(v) ? "function" : "object";
        default:       return "undefined";
    }
}

/* --- equality ------------------------------------------------------------
 *
 * Several of them, and the difference is the one thing every guide about this
 * language leads with. Strict compares kinds first and is what anybody
 * means; loose converts, and is here because pages use it; SameValueZero is
 * what includes, Map and Set use, where NaN is itself. */
static int jsb_cmp(const jbint *a, const jbint *b);
static int jsb_loose_eq(jctx *J, jval big, jval other);

static int js_strict_eq(jval a, jval b) {
    if (a.t != b.t) return 0;
    switch (a.t) {
        case JS_BIG:  return jsb_cmp(a.big, b.big) == 0;
        case JS_UNDEF: case JS_NULL: return 1;
        case JS_BOOL: return a.b == b.b;
        case JS_NUM:  return a.num == b.num;      /* NaN fails, correctly */
        case JS_STR:  return js_str_eq(a.str, b.str);
        case JS_SYM:  return a.str == b.str;
        default:      return a.obj == b.obj;
    }
}

static int js_same_zero(jval a, jval b) {
    if (a.t == JS_NUM && b.t == JS_NUM && a.num != a.num && b.num != b.num) return 1;
    return js_strict_eq(a, b);
}

static int js_same_value(jval a, jval b) {
    if (a.t == JS_NUM && b.t == JS_NUM) {
        if (a.num != a.num) return b.num != b.num;
        if (a.num == 0 && b.num == 0) return (1 / a.num > 0) == (1 / b.num > 0);
    }
    return js_strict_eq(a, b);
}

static int js_loose_eq(jctx *J, jval a, jval b) {
    if (a.t == b.t) return js_strict_eq(a, b);
    if ((a.t == JS_NULL && b.t == JS_UNDEF)
        || (a.t == JS_UNDEF && b.t == JS_NULL)) return 1;
    if (a.t == JS_NULL || a.t == JS_UNDEF
        || b.t == JS_NULL || b.t == JS_UNDEF) return 0;
    if (a.t == JS_SYM || b.t == JS_SYM) {
        if (a.t == JS_OBJ) return js_loose_eq(J, js_to_primitive(J, a, 0), b);
        if (b.t == JS_OBJ) return js_loose_eq(J, a, js_to_primitive(J, b, 0));
        return 0;
    }
    /* An object against something that is not one is its primitive against
       that, compared by the rules for the two of them -- as numbers when the
       other is a number or true or false. */
    if (a.t == JS_OBJ || b.t == JS_OBJ) {
        jval pa = a.t == JS_OBJ ? js_to_primitive(J, a, 0) : a;
        if (J->sig != JS_OK) return 0;
        jval pb = b.t == JS_OBJ ? js_to_primitive(J, b, 0) : b;
        if (J->sig != JS_OK) return 0;
        return js_loose_eq(J, pa, pb);
    }
    if (a.t == JS_STR && b.t == JS_STR) return js_str_eq(a.str, b.str);
    if (a.t == JS_BIG) return jsb_loose_eq(J, a, b);
    if (b.t == JS_BIG) return jsb_loose_eq(J, b, a);
    double x = js_to_num(J, a), y = js_to_num(J, b);
    return x == y;
}

/* --- scopes -------------------------------------------------------------- */

static jscope *js_scope(jctx *J, jscope *parent) {
    jscope *s = (jscope *)js_alloc(J, (u32)sizeof(jscope));
    if (!s) return parent;
    s->vars = js_object_with(J, JO_PLAIN, 0);
    if (!s->vars) return parent;
    s->parent = parent;
    return s;
}

/* A scope nothing can reach any more, back to the free lists: its variables'
   properties, their table, the object holding them, and itself. What the
   variables held is not touched -- a value outlives the name it had. */
static void js_scope_free(jctx *J, jscope *sc) {
    jobj *v = sc->vars;
    if (v) {
        for (jprop *p = v->ofirst; p; ) {
            jprop *next = p->onext;
            js_free(J, p, (u32)sizeof(jprop));
            p = next;
        }
        if (v->buckets) js_free(J, v->buckets, (u32)sizeof(jprop *) * v->nbuckets);
        js_free(J, v, (u32)sizeof(jobj));
    }
    js_free(J, sc, (u32)sizeof(jscope));
}

/* A binding the engine keeps -- this, new.target, a class's private names
   -- or a variable, in the scopes from sc out. A with statement's object
   has none of the first kind, so its scope is passed over here; the
   variables a page names go through js_lookup_name. */
static jprop *js_lookup(jscope *sc, const jstr *name) {
    for (jscope *s = sc; s; s = s->parent) {
        if (s->with) continue;
        jprop *p = js_find(s->vars, name);
        if (p) return p;
    }
    return 0;
}

static int js_has(jctx *J, jobj *o, jstr *key);

/* The module the code at sc was written in, the host's record of it, or 0. */
static void *js_module_of(jscope *sc) {
    for (jscope *s = sc; s; s = s->parent)
        if (s->module) return s->module;
    return 0;
}

/* A name a page wrote: the binding it is, or, when a with statement's object
   has it first, 0 with *with set to the object. The object was the scope's
   table, so only its own properties were names: `with (document) { title }`
   missed the title, and Alpine's scope, a proxy, had no names at all. */
static jprop *js_lookup_name(jctx *J, jscope *sc, jstr *name, jobj **with) {
    *with = 0;
    for (jscope *s = sc; s; s = s->parent) {
        if (s->with) {
            if (js_has(J, s->vars, name)) { *with = s->vars; return 0; }
            if (J->sig != JS_OK) return 0;
            continue;
        }
        jprop *p = js_find(s->vars, name);
        if (p) return p;
    }
    return 0;
}

static void js_declare(jctx *J, jscope *sc, jstr *name, jval v) {
    jprop *p = js_put_prop(J, sc->vars, name, v);
    if (p) p->flags = JP_PLAIN;
}

static void js_declare_flags(jctx *J, jscope *sc, jstr *name, jval v, int flags) {
    jprop *p = js_put_prop(J, sc->vars, name, v);
    if (p) p->flags = flags;
}

/* Assigning to a name. One that was never declared becomes a property of the
   global object, which is what this language does and what makes a typo
   into a new global rather than an error. A const, or a let before its line,
   refuses. */
static void js_assign_name(jctx *J, jscope *sc, jstr *name, jval v) {
    jobj *with;
    jprop *p = js_lookup_name(J, sc, name, &with);
    if (with) { js_put(J, js_from_obj(with), name, v); return; }
    if (J->sig != JS_OK) return;
    if (p) {
        if (p->v.t == JS_HOLE) {
            js_throw_named(J, JS_ERR_REFERENCE, "", name, " cannot be used before its declaration");
            return;
        }
        if (p->v.t == JS_ACC) {
            if (js_callable(p->v.acc->set)) js_call(J, p->v.acc->set, js_from_obj(J->global_obj), &v, 1);
            return;
        }
        if (!(p->flags & JP_WRITE)) {
            js_throw_named(J, JS_ERR_TYPE, "", name, " is a constant and cannot be assigned to");
            return;
        }
        p->v = v;
        return;
    }
    js_set_prop(J, J->global->vars, name, v);
}

/* A new scope with the same variables as another, for the next turn of a
   loop whose last turn made a closure over them: each closure keeps the
   values of its own turn, which is what let in a for loop is for. */
static jscope *js_scope_copy(jctx *J, jscope *from) {
    jscope *s = js_scope(J, from->parent);
    if (s == from->parent) return from;
    for (jprop *p = from->vars->ofirst; p; p = p->onext)
        js_declare_flags(J, s, p->key, p->v, p->flags);
    return s;
}

/* --- properties -----------------------------------------------------------
 *
 * A property is looked for on the object and then up its prototypes, as the
 * language has it; an accessor found on the way is called with the object
 * that was asked as `this`. A few kinds of object answer for some names
 * without a property: an array's elements and length, a string object's
 * characters, a function's name and length, and whatever the host says about
 * its own objects. */

/* Whether a name is an array index: digits, no leading zero, at most
   4294967294. "4294967296" wrapped round to 0 and "01" was 1, so either wrote
   over an element that was never named. */
static int js_index_of(const jstr *key, u32 *out) {
    if (!key || !key->len || key->len > 10 || js_is_sym_key(key)) return 0;
    if (key->s[0] < '0' || key->s[0] > '9') return 0;
    if (key->len > 1 && key->s[0] == '0') return 0;
    u64 v = 0;
    for (u32 i = 0; i < key->len; i++) {
        if (key->s[i] < '0' || key->s[i] > '9') return 0;
        v = v * 10 + (u64)(key->s[i] - '0');
    }
    if (v > 4294967294ull) return 0;
    *out = (u32)v;
    return 1;
}

static jobj *js_make_proto_for(jctx *J, jobj *f);
static jval js_ta_get(jctx *J, jobj *o, u32 i);
static void js_ta_set(jctx *J, jobj *o, u32 i, jval v);
static u32 js_ta_length(jobj *o);

/* The own properties some objects have without a property table entry. 1
   and the value in *out when o answers for key that way. */
static int js_exotic_get(jctx *J, jobj *o, jstr *key, jval *out) {
    u32 idx;
    switch (o->kind) {
        case JO_ARRAY: case JO_ARGS:
            if (js_index_of(key, &idx)) {
                if (idx < o->len) { *out = o->items[idx]; return 1; }
                return 0;
            }
            if (js_str_eq(key, J->s_length)) { *out = js_num((double)o->len); return 1; }
            return 0;
        case JO_BOXED:
            if (o->ival.t == JS_STR && o->ival.str) {
                jstr *s = o->ival.str;
                if (js_str_eq(key, J->s_length)) { *out = js_num((double)s->len); return 1; }
                if (js_index_of(key, &idx) && idx < s->len) {
                    *out = js_from_str(js_str_n(J, s->s + idx, 1));
                    return 1;
                }
            }
            return 0;
        case JO_TYPED:
            if (js_index_of(key, &idx)) {
                if (idx < js_ta_length(o)) { *out = js_ta_get(J, o, idx); return 1; }
                *out = js_undef();
                return 1;
            }
            return 0;
        case JO_FUNC: case JO_NATIVE:
            if (js_str_eq(key, J->s_name)) {
                *out = js_from_str(o->name ? o->name : js_str(J, ""));
                return 1;
            }
            if (js_str_eq(key, J->s_length)) {
                int n = o->kind == JO_FUNC && o->node >= 0 ? J->nodes[o->node].d : o->spare;
                *out = js_num(n < 0 ? 0 : n);
                return 1;
            }
            return 0;
        default:
            return 0;
    }
}

static jval js_prop_read(jctx *J, jprop *p, jval receiver) {
    if (p->v.t != JS_ACC) return p->v;
    jval g = p->v.acc->get;
    if (!js_callable(g)) return js_undef();
    return js_call(J, g, receiver, 0, 0);
}

/* Reaching into nothing.
 *
 * `a.b` where a is null or undefined is a mistake, and the language throws
 * rather than handing back undefined. A page that reads a property of
 * nothing has already gone wrong, and quietly answering undefined lets it
 * carry on and fail somewhere else, a long way from the line that was
 * wrong. */
static jval js_nothing(jctx *J, const char *verb, jstr *name, jval target) {
    return js_throw_named(J, JS_ERR_TYPE, verb, name,
                          target.t == JS_NULL ? " of null" : " of undefined");
}

/* The words of a name or a chain of them, a.b.c, into out; 0 when the
   expression is anything else or will not fit. */
static int js_chain_text(jctx *J, int node, char *out, int cap, int *n) {
    if (node < 0 || node >= J->nnodes) return 0;
    const jnode *x = &J->nodes[node];
    const char *s = 0;
    u32 len = 0;
    if (x->kind == N_THIS) { s = "this"; len = 4; }
    else if (x->kind == N_IDENT && x->str) { s = x->str->s; len = x->str->len; }
    else if (x->kind == N_MEMBER && x->str && !(x->flags & NF_PRIVATE)) {
        if (!js_chain_text(J, x->a, out, cap, n)) return 0;
        if (*n + 1 >= cap) return 0;
        out[(*n)++] = '.';
        s = x->str->s;
        len = x->str->len;
    } else if (x->kind == N_CALL) {
        /* What a call gave back: f(...), a.b(...). */
        if (!js_chain_text(J, x->a, out, cap, n)) return 0;
        s = "(...)";
        len = 5;
    } else return 0;
    if (*n + (int)len >= cap) return 0;
    for (u32 i = 0; i < len; i++) out[(*n)++] = s[i];
    out[*n] = 0;
    return 1;
}

/* The same error, saying which value it was when that is a name or a chain
   of them: "cannot read forEach of undefined (e.addedNodes)". Without it a
   bundle on one line said only "at line 1", which could be anywhere. */
static jval js_nothing_from(jctx *J, jstr *name, jval target, int obj_node) {
    char what[64];
    int n = 0;
    if (!js_chain_text(J, obj_node, what, (int)sizeof(what), &n)) return js_nothing(J, "cannot read ", name, target);
    char tail[96];
    const char *of = target.t == JS_NULL ? " of null (" : " of undefined (";
    int w = 0;
    for (const char *p = of; *p; p++) tail[w++] = *p;
    for (int i = 0; i < n && w < (int)sizeof(tail) - 2; i++) tail[w++] = what[i];
    tail[w++] = ')';
    tail[w] = 0;
    return js_throw_named(J, JS_ERR_TYPE, "cannot read ", name, tail);
}

/* The object whose properties a value has, for reading: an object itself,
   or the prototype of a primitive's kind. */
static jobj *js_proto_of_value(jctx *J, jval v) {
    switch (v.t) {
        case JS_OBJ: return v.obj;
        case JS_STR: return J->p_string;
        case JS_NUM: return J->p_number;
        case JS_BOOL: return J->p_boolean;
        case JS_SYM: return J->p_symbol;
        case JS_BIG: return J->p_bigint;
        default: return 0;
    }
}

/* An own property's descriptor, exotic ones included: 1 when there is one,
   with its value (or accessor) and flags. */
static int js_get_own(jctx *J, jobj *o, jstr *key, jval *v, int *flags) {
    if (o->flags & JOF_PROXY) return js_proxy_get_own(J, o, key, v, flags);
    jprop *p = js_find(o, key);
    if (p) { *v = p->v; *flags = p->flags; return 1; }
    if (js_exotic_get(J, o, key, v)) {
        u32 idx;
        int is_idx = js_index_of(key, &idx);
        *flags = (o->kind == JO_ARRAY || o->kind == JO_ARGS) && is_idx
               ? (o->flags & JOF_FROZEN ? JP_ENUM : JP_PLAIN)
               : o->kind == JO_TYPED && is_idx ? (JP_ENUM | JP_WRITE)
               : o->kind == JO_BOXED && is_idx ? JP_ENUM
               : o->kind == JO_ARRAY && !(o->flags & JOF_FROZEN) ? JP_WRITE
               : o->kind == JO_FUNC || o->kind == JO_NATIVE ? JP_CONF : 0;
        return 1;
    }
    if (o->kind == JO_FUNC && js_str_eq(key, J->s_prototype)) {
        jobj *pr = js_make_proto_for(J, o);
        if (pr) { *v = js_from_obj(pr); *flags = JP_WRITE; return 1; }
    }
    return 0;
}

static jval js_getv(jctx *J, jval target, jstr *key, jval receiver) {
    if (!key) return js_undef();
    if (target.t == JS_STR) {
        u32 idx;
        jstr *s = target.str;
        if (js_str_eq(key, J->s_length)) return js_num((double)(s ? s->len : 0));
        if (js_index_of(key, &idx)) {
            if (!s || idx >= s->len) return js_undef();
            return js_from_str(js_str_n(J, s->s + idx, 1));
        }
    }
    if (target.t == JS_NULL || target.t == JS_UNDEF)
        return js_nothing(J, "cannot read ", key, target);

    jobj *o = js_proto_of_value(J, target);
    int sym = js_is_sym_key(key);
    int depth = 0;
    for (; o; o = o->proto) {
        if (++depth > 10000) break;
        if (o->flags & JOF_PROXY) return js_proxy_get(J, o, key, receiver);
        if (o->kind != JO_PLAIN) {
            jval out;
            if (js_exotic_get(J, o, key, &out)) return out;
        }
        if (o->host >= 0 && J->host_get && !sym) {
            char buf[64];
            u32 n = key->len < 63 ? key->len : 63;
            for (u32 i = 0; i < n; i++) buf[i] = key->s[i];
            buf[n] = 0;
            jval out;
            if (J->host_get(J, o, buf, &out)) return out;
        }
        jprop *p = js_find(o, key);
        if (p) return js_prop_read(J, p, receiver);
        if (o->kind == JO_FUNC && js_str_eq(key, J->s_prototype)) {
            jobj *pr = js_make_proto_for(J, o);
            if (pr) return js_from_obj(pr);
        }
    }
    return js_undef();
}

static jval js_get(jctx *J, jval target, jstr *key) {
    return js_getv(J, target, key, target);
}

static jval js_get_str(jctx *J, jval target, const char *name) {
    return js_get(J, target, js_str(J, name));
}

/* The same read, from an index. */
static jval js_get_index(jctx *J, jval target, u32 i) {
    if (target.t == JS_OBJ && target.obj && (target.obj->kind == JO_ARRAY || target.obj->kind == JO_ARGS)
        && i < target.obj->len)
        return target.obj->items[i];
    return js_get(J, target, js_to_key(J, js_num((double)i)));
}

/* Whether a property is there, own or inherited, as `in` asks. */
static int js_has(jctx *J, jobj *o, jstr *key) {
    int sym = js_is_sym_key(key);
    for (int depth = 0; o && depth < 10000; o = o->proto, depth++) {
        if (o->flags & JOF_PROXY) return js_proxy_has(J, o, key);
        jval out;
        int fl;
        if (js_get_own(J, o, key, &out, &fl)) return 1;
        if (o->host >= 0 && J->host_get && !sym) {
            char buf[64];
            u32 n = key->len < 63 ? key->len : 63;
            for (u32 i = 0; i < n; i++) buf[i] = key->s[i];
            buf[n] = 0;
            if (J->host_get(J, o, buf, &out)) return 1;
        }
    }
    return 0;
}

/* An array's length, set: shorter drops the end, longer fills with nothing,
   and anything that is not a length is refused as the language refuses it. */
static void js_set_length(jctx *J, jobj *o, jval v) {
    double d = js_to_num(J, v);
    if (J->sig != JS_OK) return;
    if (!(d >= 0) || d > 4294967295.0 || (double)(u32)d != d) {
        js_throw(J, JS_ERR_RANGE, "that is not a length an array can have", J->error_line);
        return;
    }
    u32 want = (u32)d;
    if (want < o->len) o->len = want;
    else if (want > o->len) js_arr_set(J, o, want - 1, js_undef());
}

static void js_putv(jctx *J, jval target, jstr *key, jval v, jval receiver) {
    if (!key) return;
    if (target.t == JS_NULL || target.t == JS_UNDEF) {
        js_nothing(J, "cannot set ", key, target);
        return;
    }
    if (target.t != JS_OBJ || !target.obj) {
        /* A primitive has nowhere to keep a property; a setter on its
           prototype still runs. */
        jobj *o = js_proto_of_value(J, target);
        for (; o; o = o->proto) {
            jprop *p = js_find(o, key);
            if (p) {
                if (p->v.t == JS_ACC && js_callable(p->v.acc->set))
                    js_call(J, p->v.acc->set, receiver, &v, 1);
                return;
            }
        }
        return;
    }
    jobj *o = target.obj;
    if (o->flags & JOF_PROXY) { js_proxy_set(J, o, key, v, receiver); return; }

    if (o->kind == JO_TYPED) {
        /* An element past the end is dropped, as the standard has it; any
           other name is an ordinary property. */
        u32 idx;
        if (js_index_of(key, &idx)) { js_ta_set(J, o, idx, v); return; }
    }

    if (o->kind == JO_ARRAY || o->kind == JO_ARGS) {
        u32 idx;
        if (js_index_of(key, &idx)) {
            if (o->flags & JOF_FROZEN) return;
            if (o->kind == JO_ARGS && idx >= o->len) {
                /* past the end of an arguments object: an ordinary property */
            } else if (js_arr_set(J, o, idx, v)) return;
            /* Too far out to keep among the elements, it is kept as a named
               property: read back the same, and no length. */
        } else if (js_str_eq(key, J->s_length)) {
            if (o->flags & JOF_FROZEN) return;
            if (o->kind == JO_ARRAY) { js_set_length(J, o, v); return; }
        }
    }

    if (o->host >= 0 && J->host_set && !js_is_sym_key(key)) {
        char buf[64];
        u32 n = key->len < 63 ? key->len : 63;
        for (u32 i = 0; i < n; i++) buf[i] = key->s[i];
        buf[n] = 0;
        if (J->host_set(J, o, buf, v)) return;
    }

    jprop *p = js_find(o, key);
    if (p) {
        if (p->v.t == JS_ACC) {
            if (js_callable(p->v.acc->set)) js_call(J, p->v.acc->set, receiver, &v, 1);
            return;
        }
        if (!(p->flags & JP_WRITE)) return;          /* read only: left alone */
        p->v = v;
        return;
    }
    /* A function's name and length are read only; its prototype is made on
       the first write as on the first read. */
    if ((o->kind == JO_FUNC || o->kind == JO_NATIVE)
        && (js_str_eq(key, J->s_name) || js_str_eq(key, J->s_length)))
        return;
    int flags = JP_PLAIN;
    if (o->kind == JO_FUNC && js_str_eq(key, J->s_prototype)) flags = JP_WRITE;

    /* Inherited: a setter there runs, and a read only property there stops
       the write, as the language has it. */
    int depth = 0;
    for (jobj *q = o->proto; q && depth < 10000; q = q->proto, depth++) {
        if ((q->flags & JOF_PROXY) && js_proxy_set_inherited(J, q, key, v, receiver)) return;
        jprop *pp = js_find(q, key);
        if (!pp) continue;
        if (pp->v.t == JS_ACC) {
            if (js_callable(pp->v.acc->set)) js_call(J, pp->v.acc->set, receiver, &v, 1);
            return;
        }
        if (!(pp->flags & JP_WRITE)) return;
        break;
    }
    if (o->flags & JOF_NOEXT) return;
    js_put_prop_flags(J, o, key, v, flags);
}

static void js_put(jctx *J, jval target, jstr *key, jval v) {
    js_putv(J, target, key, v, target);
}

/* A property made as a literal or a definition makes one: on the object
   itself, whatever its prototypes have. */
static void js_define(jctx *J, jobj *o, jstr *key, jval v, int flags) {
    if (!o || !key) return;
    if (o->kind == JO_ARRAY) {
        u32 idx;
        if (js_index_of(key, &idx) && js_arr_set(J, o, idx, v)) return;
        if (js_str_eq(key, J->s_length)) { js_set_length(J, o, v); return; }
    }
    js_put_prop_flags(J, o, key, v, flags);
}

/* A getter or a setter, added to whatever the property already has of the
   other: `get x` and `set x` are written separately and make one property. */
static void js_define_accessor(jctx *J, jobj *o, jstr *key, jval get, jval set, int flags) {
    if (!o || !key) return;
    jprop *p = js_find(o, key);
    jacc *a;
    if (p && p->v.t == JS_ACC) a = p->v.acc;
    else {
        a = (jacc *)js_alloc(J, (u32)sizeof(jacc));
        if (!a) return;
        a->get = js_undef();
        a->set = js_undef();
        jval v;
        v.t = JS_ACC;
        v.acc = a;
        p = js_put_prop_flags(J, o, key, v, flags);
        if (!p) return;
    }
    if (get.t != JS_UNDEF) a->get = get;
    if (set.t != JS_UNDEF) a->set = set;
    p->flags = flags & ~JP_WRITE;
}

static int js_delete(jctx *J, jval target, jstr *key) {
    if (target.t != JS_OBJ || !target.obj) return 1;
    jobj *o = target.obj;
    if (o->flags & JOF_PROXY) return js_proxy_delete(J, o, key);
    if (o->kind == JO_TYPED) {
        u32 idx;
        if (js_index_of(key, &idx)) return idx >= js_ta_length(o);
    }
    if (o->kind == JO_ARRAY || o->kind == JO_ARGS) {
        u32 idx;
        if (js_index_of(key, &idx)) {
            if (o->flags & JOF_FROZEN) return 0;
            if (idx < o->len) {
                /* A hole, which this engine keeps as undefined: the length
                   stays, as it does everywhere. */
                o->items[idx] = js_undef();
                if (idx + 1 == o->len && o->kind == JO_ARRAY) { /* stays */ }
                return 1;
            }
        }
        if (js_str_eq(key, J->s_length)) return 0;
    }
    /* A function's prototype, made if nothing has asked for it yet, so that
       deleting it is refused rather than answered yes and made again. */
    if (o->kind == JO_FUNC && js_str_eq(key, J->s_prototype) && !js_find(o, key)) js_make_proto_for(J, o);
    jprop *p = js_find(o, key);
    if (!p) return 1;
    if (!(p->flags & JP_CONF)) return 0;
    return js_delete_prop(o, key);
}

/* --- calling --------------------------------------------------------------
 *
 * A call's arguments, as many as it has. There were at most twenty-four,
 * and the rest were neither worked out nor passed, which lost their side
 * effects and made Math.max.apply(null, big) wrong. */
typedef struct {
    jval *v;
    int   n, cap;
    jval  local[4];
} jargs;

static void js_args_init(jargs *A) {
    A->v = A->local;
    A->n = 0;
    A->cap = 4;
}

static int js_args_push(jctx *J, jargs *A, jval x) {
    if (A->n >= A->cap) {
        if (A->cap >= 1 << 20) {
            js_throw(J, JS_ERR_RANGE, "too many arguments for one call", J->error_line);
            return 0;
        }
        int cap = A->cap * 2;
        jval *more = (jval *)malloc((u64)cap * sizeof(jval));
        if (!more) { js_out_of_memory(J); return 0; }
        for (int i = 0; i < A->n; i++) more[i] = A->v[i];
        if (A->v != A->local) free(A->v);
        A->v = more;
        A->cap = cap;
    }
    A->v[A->n++] = x;
    return 1;
}

static void js_args_free(jargs *A) {
    if (A->v != A->local) free(A->v);
    A->v = A->local;
}

static int js_stack_low(jctx *J) {
    char here;
    return J->stack_limit && &here < J->stack_limit;
}

static jval co_call_out(jctx *J, jval fn, jval this_val, jval *argv, int argc,
                        int construct, jval nt);
static jval js_call_function(jctx *J, jobj *f, jval this_val, jval *argv, int argc);
static jval js_run_function(jctx *J, jobj *f, jval this_val, jval *argv, int argc,
                            jval new_target, jval *this_out);

/* Where a call from the host starts: how deep the stack may go from here,
   and afterwards the jobs the call left behind, which the page is owed before
   anything else happens. */
static void js_enter(jctx *J) {
    if (J->nest++ == 0) {
        char here;
        J->stack_limit = &here - JS_STACK_BUDGET;
    }
}

static void js_leave(jctx *J) {
    if (--J->nest == 0 && !J->draining && J->jcount) {
        jsignal s = J->sig;
        jval r = J->ret;
        char err[sizeof(J->error)];
        int line = J->error_line;
        for (u32 i = 0; i < sizeof(err); i++) err[i] = J->error[i];
        J->sig = JS_OK;
        js_drain(J);
        if (J->sig != JS_FAILED) {
            J->sig = s;
            J->ret = r;
            J->error_line = line;
            for (u32 i = 0; i < sizeof(err); i++) J->error[i] = err[i];
        }
    }
}

static jval js_call(jctx *J, jval fn, jval this_val, jval *argv, int argc) {
    if (!js_callable(fn))
        return js_throw(J, JS_ERR_TYPE, "this is not a function", J->error_line);
    if (J->sig != JS_OK) return js_undef();
    /* On a suspended function's own stack, a call is made on the main one
       (jsco.h says why). */
    if (J->co_current) return co_call_out(J, fn, this_val, argv, argc, 0, js_undef());

    if (J->depth >= JS_DEPTH_CAP || js_stack_low(J))
        return js_throw(J, JS_ERR_RANGE, "too many nested calls", J->error_line);

    js_enter(J);
    jval r;
    jobj *f = fn.obj;
    if (f->kind == JO_NATIVE) {
        J->callee = f;
        J->new_target = js_undef();
        r = f->fn(J, this_val, argv, argc);
    } else {
        r = js_call_function(J, f, this_val, argv, argc);
    }
    js_leave(J);
    return r;
}

/* Whether something can be given to `new`. */
static int js_is_constructor(jctx *J, jval v) {
    if (!js_callable(v)) return 0;
    jobj *f = v.obj;
    if (f->kind == JO_NATIVE) return f->flags & JOF_NOCTOR ? 0 : 1;
    int fl = f->node >= 0 ? J->nodes[f->node].op : 0;
    return !(fl & (FN_ARROW | FN_METHOD | FN_ASYNC | FN_GEN));
}

/* The prototype a `new` of this constructor gives its object: its prototype
   property when that is an object, or the one for its kind. */
static jobj *js_proto_from(jctx *J, jval ctor, jobj *fallback) {
    if (!js_is_obj(ctor)) return fallback;
    jval p = js_get(J, ctor, J->s_prototype);
    if (J->sig != JS_OK) return fallback;
    return js_is_obj(p) ? p.obj : fallback;
}

static void js_init_fields(jctx *J, jobj *f, jval obj);

static jval js_construct(jctx *J, jval fn, jval *argv, int argc, jval new_target) {
    if (J->sig != JS_OK) return js_undef();
    if (!js_is_constructor(J, fn))
        return js_throw(J, JS_ERR_TYPE, "this is not a constructor", J->error_line);
    if (J->co_current) return co_call_out(J, fn, js_undef(), argv, argc, 1, new_target);
    if (J->depth >= JS_DEPTH_CAP || js_stack_low(J))
        return js_throw(J, JS_ERR_RANGE, "too many nested calls", J->error_line);

    js_enter(J);
    jval r = js_undef();
    jobj *f = fn.obj;
    if (f->kind == JO_NATIVE) {
        jobj *fresh = js_object_with(J, JO_PLAIN, js_proto_from(J, new_target, J->p_object));
        if (J->sig == JS_OK && fresh) {
            J->callee = f;
            J->new_target = new_target;
            jval t = js_from_obj(fresh);
            r = f->fn(J, t, argv, argc);
            J->new_target = js_undef();
            if (r.t != JS_OBJ) r = t;
        }
    } else {
        int fl = J->nodes[f->node].op;
        if (fl & FN_DERIVED) {
            if (J->nodes[f->node].flags & NF_STATIC) {
                /* The made-up constructor of a class that extends another:
                   the other's, with the same arguments. */
                jval parent = js_from_obj(f->proto);
                r = js_construct(J, parent, argv, argc, new_target);
                if (J->sig == JS_OK && js_is_obj(r)) js_init_fields(J, f, r);
            } else {
                jval self = js_hole();
                r = js_run_function(J, f, js_hole(), argv, argc, new_target, &self);
                if (J->sig == JS_OK && r.t != JS_OBJ) {
                    if (self.t == JS_HOLE)
                        js_throw(J, JS_ERR_REFERENCE,
                                 "a derived class's constructor has to call super()", J->error_line);
                    r = self;
                }
            }
        } else {
            jobj *obj = js_object_with(J, JO_PLAIN, js_proto_from(J, new_target, J->p_object));
            if (J->sig == JS_OK && obj) {
                jval t = js_from_obj(obj);
                if (f->fields) js_init_fields(J, f, t);
                if (J->sig == JS_OK) {
                    r = js_run_function(J, f, t, argv, argc, new_target, 0);
                    if (J->sig == JS_OK && r.t != JS_OBJ) r = t;
                }
            }
        }
    }
    js_leave(J);
    return J->sig == JS_OK ? r : js_undef();
}

/* --- making functions -------------------------------------------------------
 *
 * A function value is its node and the scope it was made in. Nothing more is
 * made until it is needed: the prototype object an ordinary function has is
 * made the first time something asks for it, because nearly every function
 * a page makes is a callback that is never used with `new`. */
static jobj *js_make_proto_for(jctx *J, jobj *f) {
    if (f->kind != JO_FUNC || f->node < 0) return 0;
    int fl = J->nodes[f->node].op;
    if (fl & (FN_ARROW | FN_METHOD)) return 0;
    if ((fl & FN_ASYNC) && !(fl & FN_GEN)) return 0;
    jobj *pr;
    if (fl & FN_GEN) {
        pr = js_object_with(J, JO_PLAIN, (fl & FN_ASYNC) ? J->p_async_generator : J->p_generator);
    } else {
        pr = js_object(J, JO_PLAIN);
        if (pr) {
            jprop *c = js_put_prop(J, pr, J->s_constructor, js_from_obj(f));
            if (c) c->flags = JP_WRITE | JP_CONF;
        }
    }
    if (!pr) return 0;
    js_put_prop_flags(J, f, J->s_prototype, js_from_obj(pr), JP_WRITE);
    return pr;
}

static jobj *js_make_function(jctx *J, int node, jscope *sc, jval this_val) {
    jobj *f = js_object(J, JO_FUNC);
    if (!f) return 0;
    int fl = J->nodes[node].op;
    f->node = node;
    /* A function expression written with a name (function u() { ... u() })
       is the one thing that name means inside it, unless its parameters or
       variables say otherwise: a scope of its own round the call's, holding
       only the name. It was not bound at all, so GSAP, and every library
       whose minifier writes its recursion that way, stopped at "u is not
       defined". An assignment to it changes it, where the standard would
       ignore it or throw. */
    if ((fl & FN_SELFNAME) && J->nodes[node].kind == N_FUNC && J->nodes[node].str) {
        jscope *own = js_scope(J, sc);
        if (own != sc) {
            js_declare(J, own, J->nodes[node].str, js_from_obj(f));
            sc = own;
        }
    }
    f->closure = sc;
    f->name = J->nodes[node].str;
    if ((fl & FN_ASYNC) && (fl & FN_GEN) && J->p_async_gen_function) f->proto = J->p_async_gen_function;
    else if ((fl & FN_GEN) && J->p_gen_function) f->proto = J->p_gen_function;
    else if ((fl & FN_ASYNC) && J->p_async_function) f->proto = J->p_async_function;
    /* The function can be called after the call it was made in is over, so
       that call's scope, and every scope around it, has to stay. */
    for (jscope *s = sc; s && !s->escaped; s = s->parent) s->escaped = 1;
    /* An arrow takes `this` from where it was written rather than from
       wherever it is later called, so it is caught here, at the moment the
       function value is made. */
    if (fl & FN_ARROW) f->lex_this = this_val;
    return f;
}

/* --- jobs for after the script ------------------------------------------- */

enum { JOB_REACT_OK = 1, JOB_REACT_ERR, JOB_THENABLE, JOB_CALL, JOB_RESUME_OK, JOB_RESUME_ERR };

static void js_enqueue(jctx *J, int kind, jval a, jval b, jobj *o, void *co) {
    if (J->jcount >= J->jcap) {
        u32 cap = J->jcap ? J->jcap * 2 : 64;
        if (cap > (1u << 22)) { js_out_of_memory(J); return; }
        jjob *more = (jjob *)malloc((u64)cap * sizeof(jjob));
        if (!more) { js_out_of_memory(J); return; }
        for (u32 i = 0; i < J->jcount; i++) more[i] = J->jobs[(J->jhead + i) % J->jcap];
        if (J->jobs) free(J->jobs);
        J->jobs = more;
        J->jhead = 0;
        J->jcap = cap;
    }
    jjob *j = &J->jobs[(J->jhead + J->jcount) % J->jcap];
    j->kind = kind;
    j->a = a;
    j->b = b;
    j->o = o;
    j->co = co;
    J->jcount++;
}

static void js_run_job(jctx *J, jjob *job);

/* Every job waiting, and every job those make, until there are none: the
   checkpoint a browser makes when a script or a handler has finished. A job
   that throws is the page's problem and not the next job's. */
static void js_drain(jctx *J) {
    if (J->draining || J->co_current) return;
    J->draining = 1;
    while (J->jcount && J->sig != JS_FAILED) {
        jjob job = J->jobs[J->jhead];
        J->jhead = (J->jhead + 1) % J->jcap;
        J->jcount--;
        J->sig = JS_OK;
        J->nest++;
        js_run_job(J, &job);
        J->nest--;
        if (J->sig == JS_THROWN || J->sig == JS_RETURN) J->sig = JS_OK;
        if (!js_tick(J)) break;
    }
    if (J->sig == JS_FAILED) J->jcount = 0;
    J->draining = 0;
}

static jval nat_fn_bound(jctx *J, jval t, jval *a, int n);

/* --- a call ------------------------------------------------------------------ */

static void js_bind_params(jctx *J, int params, jval *argv, int argc, jscope *sc, jval this_val);
static jval js_start_coroutine(jctx *J, jobj *f, jval this_val, jval *argv, int argc);
static void js_hoist_body(jctx *J, int block, jscope *var_sc, jscope *lex_sc);

static jval js_call_function(jctx *J, jobj *f, jval this_val, jval *argv, int argc) {
    int fl = J->nodes[f->node].op;
    if (fl & FN_CTOR)
        return js_throw(J, JS_ERR_TYPE, "a class constructor cannot be called without new",
                        J->error_line);
    if (fl & (FN_GEN | FN_ASYNC)) return js_start_coroutine(J, f, this_val, argv, argc);
    return js_run_function(J, f, this_val, argv, argc, js_undef(), 0);
}

/* A function's body, run: its scope made, its parameters bound, its vars and
   functions declared before the first line, and the body walked. */
static jval js_run_function(jctx *J, jobj *f, jval this_val, jval *argv, int argc,
                            jval new_target, jval *this_out) {
    int node = f->node;
    int fl = J->nodes[node].op;
    int body = J->nodes[node].a;
    int params = J->nodes[node].b;

    if (fl & FN_ARROW) this_val = f->lex_this;
    else if (!(fl & FN_STRICT) && (this_val.t == JS_UNDEF || this_val.t == JS_NULL))
        /* A plain call's `this` outside strict code is the global object,
           which older scripts lean on to find it. */
        this_val = js_from_obj(J->global_obj);

    jscope *sc = js_scope(J, f->closure);
    if (sc == f->closure) return js_undef();

    if (fl & FN_SIMPLE) {
        int i = 0;
        for (int p = params; p >= 0; p = J->nodes[p].b, i++)
            js_declare(J, sc, J->nodes[p].str, i < argc ? argv[i] : js_undef());
    }

    /* `arguments`, when the text asks for it. */
    if (fl & FN_ARGS) {
        jobj *a = js_object_with(J, JO_ARGS, J->p_object);
        if (a) {
            js_arr_reserve(J, a, (u32)argc);
            for (int k = 0; k < argc; k++) js_arr_push(J, a, argv[k]);
            js_declare(J, sc, J->s_arguments, js_from_obj(a));
        }
    }
    if (fl & (FN_SUPER | FN_CTOR)) js_declare(J, sc, J->s_home, js_from_obj(f->home));
    if (fl & (FN_NEWTARGET | FN_DERIVED | FN_CTOR)) js_declare(J, sc, J->s_newtarget, new_target);
    if (fl & FN_DERIVED) {
        js_declare(J, sc, J->s_this, js_hole());
        js_declare(J, sc, J->s_fnself, js_from_obj(f));
    }

    if (!(fl & FN_SIMPLE)) js_bind_params(J, params, argv, argc, sc, this_val);
    if (!(fl & FN_EXPR) && body >= 0) js_hoist_body(J, body, sc, sc);

    jval r = js_undef();
    if (J->sig == JS_OK) {
        J->depth++;
        if (fl & FN_EXPR) {
            r = js_eval(J, body, sc, this_val);
        } else {
            jsignal s = js_exec(J, body, sc, this_val);
            if (s == JS_RETURN) {
                J->sig = JS_OK;
                r = J->ret;
                J->ret = js_undef();
            } else if (s == JS_BREAK || s == JS_CONTINUE) {
                /* A break with nowhere to go ends the function rather than
                   escaping into the caller's loop. */
                J->sig = JS_OK;
                J->label = 0;
            }
        }
        J->depth--;
    }
    if (this_out && (fl & FN_DERIVED)) {
        jprop *tp = js_find(sc->vars, J->s_this);
        *this_out = tp ? tp->v : js_hole();
    }

    /* Nothing made during the call can reach its scope, so the scope is
       given back for the next call to use. It never was, and a page that
       called a small function in a loop ran out of memory in about forty
       thousand calls. */
    if (!sc->escaped) js_scope_free(J, sc);
    return J->sig == JS_OK ? r : js_undef();
}

/* --- iteration ----------------------------------------------------------------
 *
 * for-of, spread and destructuring all walk something one value at a time by
 * the same protocol: ask it for an iterator with Symbol.iterator, then call
 * next until it says done. An array, a string, an arguments object, a Map and
 * a Set are walked directly when nobody has replaced how they iterate,
 * because the protocol costs an object per value and those are what nearly
 * every loop walks. */
enum { IT_ARRAY = 1, IT_STRING, IT_MAP, IT_GENERIC };

typedef struct {
    int  kind;
    jval obj;                 /* what is walked, or the iterator */
    jval next;
    u32  at;
    int  entries;             /* a Map's pairs */
} jiter;

typedef struct jmentry { jval k, v; u32 hash; int next; int gone; } jmentry;
typedef struct { jmentry *e; u32 n, cap, live; int *heads; u32 nheads; } jmap;

static int js_proto_value_is(jctx *J, jobj *proto, jstr *key, jnative fn) {
    (void)J;
    jprop *p = js_find(proto, key);
    return p && p->v.t == JS_OBJ && p->v.obj && p->v.obj->kind == JO_NATIVE && p->v.obj->fn == fn;
}

static jval nat_arr_values(jctx *J, jval t, jval *a, int n);
static jval nat_str_iterator(jctx *J, jval t, jval *a, int n);
static jval nat_map_entries(jctx *J, jval t, jval *a, int n);
static jval nat_set_values(jctx *J, jval t, jval *a, int n);

static int js_iter_open(jctx *J, jval v, jiter *it) {
    it->at = 0;
    it->entries = 0;
    it->obj = v;
    it->next = js_undef();
    if (v.t == JS_OBJ && v.obj) {
        jobj *o = v.obj;
        if ((o->kind == JO_ARRAY && o->proto == J->p_array) || o->kind == JO_ARGS) {
            if (!js_find(o, J->sym_iterator)
                && (o->kind == JO_ARGS || js_proto_value_is(J, J->p_array, J->sym_iterator, nat_arr_values))) {
                it->kind = IT_ARRAY;
                return 1;
            }
        }
        if ((o->kind == JO_MAP && o->proto == J->p_map && !js_find(o, J->sym_iterator)
             && js_proto_value_is(J, J->p_map, J->sym_iterator, nat_map_entries))
            || (o->kind == JO_SET && o->proto == J->p_set && !js_find(o, J->sym_iterator)
                && js_proto_value_is(J, J->p_set, J->sym_iterator, nat_set_values))) {
            it->kind = IT_MAP;
            it->entries = o->kind == JO_MAP;
            return 1;
        }
    }
    if (v.t == JS_STR && js_proto_value_is(J, J->p_string, J->sym_iterator, nat_str_iterator)) {
        it->kind = IT_STRING;
        return 1;
    }
    if (v.t == JS_NULL || v.t == JS_UNDEF) {
        js_throw(J, JS_ERR_TYPE, "undefined or null cannot be walked with of or spread",
                 J->error_line);
        return 0;
    }
    jval m = js_get(J, v, J->sym_iterator);
    if (J->sig != JS_OK) return 0;
    if (!js_callable(m)) {
        js_throw(J, JS_ERR_TYPE, "this cannot be walked: it has no Symbol.iterator", J->error_line);
        return 0;
    }
    jval iter = js_call(J, m, v, 0, 0);
    if (J->sig != JS_OK) return 0;
    if (!js_is_obj(iter)) {
        js_throw(J, JS_ERR_TYPE, "Symbol.iterator gave back something that is not an iterator",
                 J->error_line);
        return 0;
    }
    it->kind = IT_GENERIC;
    it->obj = iter;
    it->next = js_get(J, iter, J->s_next);
    return J->sig == JS_OK;
}

/* The length of one character of UTF-8 starting with byte c. */
static u32 js_utf8_len(u8 c) {
    return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

/* 1 with the next value, 0 at the end, -1 when something threw. */
static int js_iter_step(jctx *J, jiter *it, jval *out) {
    switch (it->kind) {
        case IT_ARRAY: {
            jobj *o = it->obj.obj;
            if (it->at >= o->len) return 0;
            *out = o->items[it->at++];
            return 1;
        }
        case IT_STRING: {
            jstr *s = it->obj.str;
            if (!s || it->at >= s->len) return 0;
            u32 k = js_utf8_len((u8)s->s[it->at]);
            if (it->at + k > s->len) k = s->len - it->at;
            *out = js_from_str(js_str_n(J, s->s + it->at, k));
            it->at += k;
            return 1;
        }
        case IT_MAP: {
            jmap *m = (jmap *)it->obj.obj->internal;
            while (m && it->at < m->n && m->e[it->at].gone) it->at++;
            if (!m || it->at >= m->n) return 0;
            jmentry *e = &m->e[it->at++];
            if (!it->entries) { *out = e->k; return 1; }
            jobj *pair = js_array(J);
            if (!pair) return -1;
            js_arr_push(J, pair, e->k);
            js_arr_push(J, pair, e->v);
            *out = js_from_obj(pair);
            return 1;
        }
        default: {
            jval r = js_call(J, it->next, it->obj, 0, 0);
            if (J->sig != JS_OK) return -1;
            if (!js_is_obj(r)) {
                js_throw(J, JS_ERR_TYPE, "an iterator's next gave back something that is not an object",
                         J->error_line);
                return -1;
            }
            jval d = js_get(J, r, J->s_done);
            if (J->sig != JS_OK) return -1;
            if (js_to_bool(d)) return 0;
            *out = js_get(J, r, J->s_value);
            return J->sig == JS_OK ? 1 : -1;
        }
    }
}

/* An iterator left before its end -- a break, a throw, a return out of the
   loop -- is told, so a generator's finally runs. Whatever was already
   happening carries on happening. */
static void js_iter_close(jctx *J, jiter *it) {
    if (it->kind != IT_GENERIC) return;
    jsignal s = J->sig;
    jval ret = J->ret;
    jstr *label = J->label;
    if (s == JS_FAILED) return;
    J->sig = JS_OK;
    jval m = js_get(J, it->obj, J->s_return);
    if (J->sig == JS_OK && js_callable(m)) js_call(J, m, it->obj, 0, 0);
    if (s != JS_OK || J->sig == JS_OK) {
        if (J->sig != JS_FAILED) {
            J->sig = s;
            J->ret = ret;
            J->label = label;
        }
    }
}

/* Every value of an iterable, into an array. */
static int js_iter_collect(jctx *J, jval v, jargs *A) {
    jiter it;
    if (!js_iter_open(J, v, &it)) return 0;
    for (;;) {
        jval x;
        int r = js_iter_step(J, &it, &x);
        if (r < 0) return 0;
        if (r == 0) return 1;
        if (!js_args_push(J, A, x)) { js_iter_close(J, &it); return 0; }
    }
}

/* --- destructuring ---------------------------------------------------------
 *
 * A pattern and a value: each name in the pattern given the part of the value
 * it stands for. `mode` is 0 for an assignment, which may put values into
 * any place, and otherwise how a declaration declares (VK_*), or 4 for a
 * parameter. */
#define BIND_PARAM 4

static jstr *js_private_key(jctx *J, jscope *sc, jstr *name);

static void js_bind(jctx *J, int target, jval v, jscope *sc, jval this_val, int mode);

static void js_bind_name(jctx *J, jstr *name, jval v, jscope *sc, int mode) {
    if (mode == 0 || mode == VK_VAR) js_assign_name(J, sc, name, v);
    else js_declare_flags(J, sc, name, v, mode == VK_CONST ? (JP_ENUM | JP_CONF) : JP_PLAIN);
}

static void js_bind(jctx *J, int target, jval v, jscope *sc, jval this_val, int mode) {
    if (target < 0 || J->sig != JS_OK) return;
    int kind = J->nodes[target].kind;
    J->error_line = J->nodes[target].line;
    if (kind == N_IDENT) {
        js_bind_name(J, J->nodes[target].str, v, sc, mode);
        return;
    }
    if (kind == N_MEMBER || kind == N_INDEX) {
        jval obj = js_eval(J, J->nodes[target].a, sc, this_val);
        if (J->sig != JS_OK) return;
        jstr *key;
        if (kind == N_MEMBER) {
            key = J->nodes[target].str;
            if (J->nodes[target].flags & NF_PRIVATE) key = js_private_key(J, sc, key);
        } else {
            key = js_to_key(J, js_eval(J, J->nodes[target].b, sc, this_val));
        }
        if (J->sig != JS_OK) return;
        js_put(J, obj, key, v);
        return;
    }
    if (kind == N_APAT) {
        jiter it;
        if (!js_iter_open(J, v, &it)) return;
        int done = 0;
        for (int cell = J->nodes[target].a; cell >= 0; cell = J->nodes[cell].b) {
            int t = J->nodes[cell].a;
            int dflt = J->nodes[cell].c;
            if (J->nodes[cell].op == 1) {
                /* ...rest: whatever is left, as an array. */
                jobj *rest = js_array(J);
                while (!done && rest) {
                    jval x;
                    int r = js_iter_step(J, &it, &x);
                    if (r < 0) return;
                    if (r == 0) { done = 1; break; }
                    js_arr_push(J, rest, x);
                }
                js_bind(J, t, js_from_obj(rest), sc, this_val, mode);
                if (J->sig != JS_OK) return;
                continue;
            }
            jval x = js_undef();
            if (!done) {
                int r = js_iter_step(J, &it, &x);
                if (r < 0) return;
                if (r == 0) { done = 1; x = js_undef(); }
            }
            if (t < 0) continue;                       /* a hole */
            if (x.t == JS_UNDEF && dflt >= 0) {
                x = js_eval(J, dflt, sc, this_val);
                if (J->sig != JS_OK) { if (!done) js_iter_close(J, &it); return; }
            }
            js_bind(J, t, x, sc, this_val, mode);
            if (J->sig != JS_OK) { if (!done) js_iter_close(J, &it); return; }
        }
        if (!done) js_iter_close(J, &it);
        return;
    }
    if (kind == N_OPAT) {
        if (v.t == JS_NULL || v.t == JS_UNDEF) {
            js_throw(J, JS_ERR_TYPE, v.t == JS_NULL ? "null cannot be destructured"
                                                    : "undefined cannot be destructured",
                     J->error_line);
            return;
        }
        /* The keys taken, for a ...rest to leave out. */
        jstr *taken[32];
        int ntaken = 0;
        for (int cell = J->nodes[target].a; cell >= 0; cell = J->nodes[cell].b) {
            int t = J->nodes[cell].a;
            if (J->nodes[cell].op == 1) {
                jobj *rest = js_object(J, JO_PLAIN);
                jobj *src = js_is_obj(v) ? v.obj : 0;
                if (rest && src) {
                    jprop **own;
                    u32 nown = js_keys_of(J, src, &own, JK_ENUM | JK_STR | JK_SYM);
                    for (u32 i = 0; i < nown; i++) {
                        int skip = 0;
                        for (int k = 0; k < ntaken; k++) if (js_str_eq(taken[k], own[i]->key)) skip = 1;
                        if (!skip) js_define(J, rest, own[i]->key, js_prop_read(J, own[i], v), JP_PLAIN);
                    }
                    if (src->kind == JO_ARRAY)
                        for (u32 i = 0; i < src->len; i++)
                            js_define(J, rest, js_to_key(J, js_num(i)), src->items[i], JP_PLAIN);
                }
                js_bind(J, t, js_from_obj(rest), sc, this_val, mode);
                if (J->sig != JS_OK) return;
                continue;
            }
            jstr *key = J->nodes[cell].str;
            int kn = J->nodes[cell].d;
            if (kn >= 0) {
                key = js_to_key(J, js_eval(J, kn, sc, this_val));
                if (J->sig != JS_OK) return;
            }
            if (ntaken < 32) taken[ntaken++] = key;
            jval x = js_get(J, v, key);
            if (J->sig != JS_OK) return;
            if (x.t == JS_UNDEF && J->nodes[cell].c >= 0) {
                x = js_eval(J, J->nodes[cell].c, sc, this_val);
                if (J->sig != JS_OK) return;
            }
            js_bind(J, t, x, sc, this_val, mode);
            if (J->sig != JS_OK) return;
        }
        return;
    }
    js_throw(J, JS_ERR_SYNTAX, "this cannot be assigned to", J->error_line);
}

static void js_bind_params(jctx *J, int params, jval *argv, int argc, jscope *sc, jval this_val) {
    int i = 0;
    for (int p = params; p >= 0 && J->sig == JS_OK; p = J->nodes[p].b, i++) {
        int t = J->nodes[p].a;
        if (J->nodes[p].op == 1) {
            jobj *rest = js_array(J);
            for (int k = i; k < argc && rest; k++) js_arr_push(J, rest, argv[k]);
            js_bind(J, t, js_from_obj(rest), sc, this_val, BIND_PARAM);
            return;
        }
        jval x = i < argc ? argv[i] : js_undef();
        if (x.t == JS_UNDEF && J->nodes[p].c >= 0) {
            x = js_eval(J, J->nodes[p].c, sc, this_val);
            if (J->sig != JS_OK) return;
        }
        js_bind(J, t, x, sc, this_val, BIND_PARAM);
    }
}

/* --- declarations, before the first line ----------------------------------
 *
 * A function's var names exist from the top of the function, undefined
 * until their line runs; its function declarations exist with their values;
 * and its let, const and class names exist but may not be touched until
 * their line. Without the first, a closure that assigned a var declared
 * further down made a global instead; without the last, let was var. */

static void js_note_lexical(jctx *J, int target, jscope *sc) {
    if (target < 0) return;
    int k = J->nodes[target].kind;
    if (k == N_IDENT) {
        js_declare_flags(J, sc, J->nodes[target].str, js_hole(), JP_PLAIN);
        return;
    }
    if (k == N_APAT || k == N_OPAT)
        for (int cell = J->nodes[target].a; cell >= 0; cell = J->nodes[cell].b)
            js_note_lexical(J, J->nodes[cell].a, sc);
}

/* The declarations in a list of statements: functions made in `fn_sc`,
   let, const and class marked in `lex_sc`. */
static void js_hoist_one(jctx *J, int st, jscope *fn_sc, jscope *lex_sc) {
    int k = J->nodes[st].kind;
    if (k == N_FUNCDECL) {
        jobj *f = js_make_function(J, st, lex_sc, js_undef());
        if (f && J->nodes[st].str) js_declare(J, fn_sc, J->nodes[st].str, js_from_obj(f));
    } else if (k == N_VAR && J->nodes[st].d != VK_VAR) {
        for (int c = J->nodes[st].a; c >= 0; c = J->nodes[c].b)
            js_note_lexical(J, J->nodes[c].c, lex_sc);
    } else if (k == N_CLASSDECL && J->nodes[st].str) {
        js_declare_flags(J, lex_sc, J->nodes[st].str, js_hole(), JP_PLAIN);
    } else if (k == N_EXPORTDECL) {
        /* What an export declares is declared, and a default is a binding
           of its own, *default*, which no page can name. */
        int d = J->nodes[st].a;
        if (J->nodes[st].op <= 1 && d >= 0) js_hoist_one(J, d, fn_sc, lex_sc);
        if (J->nodes[st].op == 1) {
            jstr *dk = js_intern(J, "*default*", 9);
            jprop *p = d >= 0 && J->nodes[d].kind == N_FUNCDECL && J->nodes[d].str
                     ? js_find(fn_sc->vars, J->nodes[d].str) : 0;
            js_declare_flags(J, lex_sc, dk, p ? p->v : js_hole(), JP_PLAIN);
        }
    }
}

static void js_hoist_list(jctx *J, int chain, jscope *fn_sc, jscope *lex_sc) {
    for (int cell = chain; cell >= 0; cell = J->nodes[cell].b) {
        int st = J->nodes[cell].a;
        if (st >= 0) js_hoist_one(J, st, fn_sc, lex_sc);
    }
}

/* A body: a function's or a whole script's. `var_sc` gets the vars and the
   functions; `lex_sc` the rest, which for a script is the scope shared by
   every script's top level. */
static void js_hoist_body(jctx *J, int block, jscope *var_sc, jscope *lex_sc) {
    for (int v = J->nodes[block].c; v >= 0; v = J->nodes[v].b) {
        jstr *name = J->nodes[v].str;
        if (name && !js_find(var_sc->vars, name)) js_declare(J, var_sc, name, js_undef());
    }
    js_hoist_list(J, J->nodes[block].a, var_sc, lex_sc);
}

/* --- the evaluator -------------------------------------------------------- */

static int js_tick(jctx *J) {
    if (++J->steps > JS_STEP_CAP) {
        if (J->sig == JS_OK || J->sig == JS_THROWN) {
            J->sig = JS_FAILED;
            const char *m = "this script ran for too long and was stopped";
            int i = 0;
            while (m[i] && i < (int)sizeof(J->error) - 1) { J->error[i] = m[i]; i++; }
            J->error[i] = 0;
        }
        return 0;
    }
    return J->sig == JS_OK;
}

/* Whether a break or continue that has arrived here was aimed at this
   loop. One with no name is for whichever loop catches it first; one with a
   name belongs to the statement of that name and nothing else. */
static int js_label_mine(jctx *J, jstr *mine) {
    if (!J->label) return 1;
    return mine && js_str_eq(J->label, mine);
}

/* The key a #name stands for in the class it was written in: a symbol made
   when the class was, so no other class's #name is the same property. */
static jstr *js_private_key(jctx *J, jscope *sc, jstr *name) {
    jprop *p = js_lookup(sc, name);
    if (p && p->v.t == JS_SYM) return p->v.str;
    js_throw_named(J, JS_ERR_SYNTAX, "", name, " is not a name this class has");
    return 0;
}

/* `this` inside a derived class's constructor, which is not there until
   super() has made it. */
static JS_NOINLINE jval js_this_binding(jctx *J, jscope *sc) {
    jprop *p = js_lookup(sc, J->s_this);
    if (!p) return js_undef();
    if (p->v.t == JS_HOLE)
        return js_throw(J, JS_ERR_REFERENCE, "this cannot be used before super() has been called",
                        J->error_line);
    return p->v;
}

static jval js_binary(jctx *J, jop op, jval l, jval r, int line);

/* Both sides of an assignment target: where to read it and where to put it
   back. Used by ++, --, the compound assignments and the logical ones, which
   need both. An element of an array or a typed array, by a whole number, is
   reached directly: a[i] = v in a loop made the text of every i. */
typedef struct {
    int    kind;              /* 0 name, 1 property, 2 nothing, 3 super, 4 element */
    u32    idx;
    jstr  *name;
    jval   obj;
    jval   self;
} jplace;

static jval js_ta_get(jctx *J, jobj *o, u32 i);
static void js_ta_set(jctx *J, jobj *o, u32 i, jval v);
static u32 js_ta_length(jobj *o);

static jplace js_place(jctx *J, int node, jscope *sc, jval this_val) {
    jplace p;
    p.kind = 2;
    p.idx = 0;
    p.name = 0;
    p.obj = js_undef();
    p.self = js_undef();
    if (node < 0) return p;

    int kind = J->nodes[node].kind;
    if (kind == N_IDENT) {
        p.kind = 0;
        p.name = J->nodes[node].str;
        return p;
    }
    if (kind == N_MEMBER) {
        jstr *name = J->nodes[node].str;
        int priv = J->nodes[node].flags & NF_PRIVATE;
        p.obj = js_eval(J, J->nodes[node].a, sc, this_val);
        p.name = priv ? js_private_key(J, sc, name) : name;
        p.kind = 1;
        return p;
    }
    if (kind == N_INDEX) {
        int b = J->nodes[node].b;
        p.obj = js_eval(J, J->nodes[node].a, sc, this_val);
        if (J->sig != JS_OK) return p;
        jval k = js_eval(J, b, sc, this_val);
        if (J->sig != JS_OK) return p;
        if (k.t == JS_NUM && p.obj.t == JS_OBJ && p.obj.obj
            && (p.obj.obj->kind == JO_ARRAY || p.obj.obj->kind == JO_TYPED)
            && k.num >= 0 && k.num < 4294967295.0 && k.num == (double)(u32)k.num) {
            p.kind = 4;
            p.idx = (u32)k.num;
            return p;
        }
        p.name = js_to_key(J, k);
        p.kind = 1;
        return p;
    }
    if (kind == N_SUPERMEMBER) {
        int b = J->nodes[node].b;
        jstr *name = J->nodes[node].str;
        jprop *h = js_lookup(sc, J->s_home);
        p.obj = h && js_is_obj(h->v) && h->v.obj->proto ? js_from_obj(h->v.obj->proto) : js_undef();
        p.self = this_val.t == JS_HOLE ? js_this_binding(J, sc) : this_val;
        p.name = b >= 0 ? js_to_key(J, js_eval(J, b, sc, this_val)) : name;
        p.kind = 3;
        return p;
    }
    return p;
}

static jval js_ident(jctx *J, jstr *name, jscope *sc);

static jval js_place_get(jctx *J, jplace *p, jscope *sc) {
    if (p->kind == 0) return js_ident(J, p->name, sc);
    if (p->kind == 1) return js_get(J, p->obj, p->name);
    if (p->kind == 3) return js_getv(J, p->obj, p->name, p->self);
    if (p->kind == 4) {
        jobj *o = p->obj.obj;
        if (o->kind == JO_TYPED) return p->idx < js_ta_length(o) ? js_ta_get(J, o, p->idx) : js_undef();
        if (p->idx < o->len) return o->items[p->idx];
        return js_get(J, p->obj, js_to_key(J, js_num(p->idx)));
    }
    return js_undef();
}

static void js_place_put(jctx *J, jplace *p, jscope *sc, jval v) {
    if (p->kind == 0) js_assign_name(J, sc, p->name, v);
    else if (p->kind == 1) js_put(J, p->obj, p->name, v);
    else if (p->kind == 3) js_putv(J, p->self, p->name, v, p->self);
    else if (p->kind == 4) {
        jobj *o = p->obj.obj;
        if (o->kind == JO_TYPED) { js_ta_set(J, o, p->idx, v); return; }
        if (!(o->flags & JOF_FROZEN) && p->idx < o->len) { o->items[p->idx] = v; return; }
        js_put(J, p->obj, js_to_key(J, js_num(p->idx)), v);
    }
}

/* A name's value. One that was never declared is a ReferenceError, as the
   language says -- it was undefined here, which let a page that used
   something this browser does not have carry on past the try that would
   have caught it, to fail somewhere with nothing round it. typeof asks
   without the error. */
static jval js_ident_in(jctx *J, jstr *name, jscope *sc, int *found, jobj **with) {
    jprop *p = js_lookup_name(J, sc, name, with);
    *found = 1;
    if (*with) return js_get(J, js_from_obj(*with), name);
    if (J->sig != JS_OK) return js_undef();
    if (p) {
        if (p->v.t == JS_HOLE)
            return js_throw_named(J, JS_ERR_REFERENCE, "", name, " cannot be used before its declaration");
        if (p->v.t == JS_ACC) return js_prop_read(J, p, js_from_obj(J->global_obj));
        return p->v;
    }
    /* The global object's own prototype: a bare toString is the window's. */
    for (jobj *o = J->global_obj ? J->global_obj->proto : 0; o; o = o->proto) {
        jprop *q = js_find(o, name);
        if (q) return js_prop_read(J, q, js_from_obj(J->global_obj));
    }
    *found = 0;
    return js_undef();
}

static jval js_ident_soft(jctx *J, jstr *name, jscope *sc, int *found) {
    jobj *with;
    return js_ident_in(J, name, sc, found, &with);
}

static JS_NOINLINE jval js_ident(jctx *J, jstr *name, jscope *sc) {
    int found;
    jval v = js_ident_soft(J, name, sc, &found);
    if (!found) return js_throw_named(J, JS_ERR_REFERENCE, "", name, " is not defined");
    return v;
}

static jval jsb_binary(jctx *J, jop op, jval l, jval r);
static jval jsb_relational(jctx *J, jop op, jval l, jval r);

/* The arithmetic and bitwise operators when either side is a BigInt or an
   object, which may stand for one: both made primitive (as numbers), then
   BigInt arithmetic when either is one. 0 when neither is, with l and r
   left primitive for the ordinary way. */
static int js_binary_big(jctx *J, jop op, jval *l, jval *r, jval *out) {
    if (l->t == JS_OBJ) { *l = js_to_primitive(J, *l, 1); if (J->sig != JS_OK) { *out = js_undef(); return 1; } }
    if (r->t == JS_OBJ) { *r = js_to_primitive(J, *r, 1); if (J->sig != JS_OK) { *out = js_undef(); return 1; } }
    if (l->t != JS_BIG && r->t != JS_BIG) return 0;
    *out = jsb_binary(J, op, *l, *r);
    return 1;
}

static jval js_binary(jctx *J, jop op, jval l, jval r, int line) {
    if ((l.t == JS_BIG || r.t == JS_BIG || l.t == JS_OBJ || r.t == JS_OBJ) && op != OP_ADD
        && op != OP_EQ && op != OP_NE && op != OP_SEQ && op != OP_SNE && op != OP_INSTANCEOF && op != OP_IN
        && op != OP_LT && op != OP_GT && op != OP_LE && op != OP_GE) {
        jval out;
        if (js_binary_big(J, op, &l, &r, &out)) return out;
    }
    switch (op) {
        case OP_ADD:
            if (l.t == JS_NUM && r.t == JS_NUM) return js_num(l.num + r.num);
            /* The one operator that is two operators. Objects become their
               primitive first; then if either side is a string the result is
               text, and otherwise it is arithmetic. */
            if (l.t == JS_OBJ) { l = js_to_primitive(J, l, 0); if (J->sig != JS_OK) return js_undef(); }
            if (r.t == JS_OBJ) { r = js_to_primitive(J, r, 0); if (J->sig != JS_OK) return js_undef(); }
            if (l.t == JS_STR || r.t == JS_STR) {
                jstr *a = js_to_str(J, l);
                jstr *b = js_to_str(J, r);
                if (J->sig != JS_OK) return js_undef();
                return js_from_str(js_concat(J, a, b));
            }
            if (l.t == JS_BIG || r.t == JS_BIG) return jsb_binary(J, op, l, r);
            return js_num(js_to_num(J, l) + js_to_num(J, r));

        case OP_SUB: return js_num(js_to_num(J, l) - js_to_num(J, r));
        case OP_MUL: return js_num(js_to_num(J, l) * js_to_num(J, r));
        case OP_DIV: return js_num(js_to_num(J, l) / js_to_num(J, r));
        case OP_MOD: {
            double a = js_to_num(J, l), b = js_to_num(J, r);
            if (b == 0 || a != a || b != b || a - a != 0) return js_num(js_nan());
            if (b - b != 0) return js_num(a);
            if (a < 9.2e18 && a > -9.2e18 && b < 9.2e18 && b > -9.2e18
                && a == (double)(long long)a && b == (double)(long long)b) {
                long long q = (long long)a % (long long)b;
                double res = (double)q;
                if (res == 0 && a < 0) res = -0.0;
                return js_num(res);
            }
            double q = a / b;
            double w = js_trunc(q);
            return js_num(a - w * b);
        }
        case OP_POW: {
            double a = js_to_num(J, l), b = js_to_num(J, r);
            return js_num(js_pow(a, b));
        }

        case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
            /* Two strings compare as text; anything else compares as
               numbers, which is why "10" < "9" and 10 > 9 are both true. */
            if (l.t == JS_OBJ) { l = js_to_primitive(J, l, 1); if (J->sig != JS_OK) return js_undef(); }
            if (r.t == JS_OBJ) { r = js_to_primitive(J, r, 1); if (J->sig != JS_OK) return js_undef(); }
            if (l.t == JS_STR && r.t == JS_STR) {
                jstr *a = l.str, *b = r.str;
                u32 m = a->len < b->len ? a->len : b->len;
                int c = 0;
                for (u32 i = 0; i < m && !c; i++) c = (u8)a->s[i] - (u8)b->s[i];
                if (!c) c = (int)a->len - (int)b->len;
                switch (op) {
                    case OP_LT: return js_bool(c < 0);
                    case OP_GT: return js_bool(c > 0);
                    case OP_LE: return js_bool(c <= 0);
                    default:    return js_bool(c >= 0);
                }
            }
            if (l.t == JS_BIG || r.t == JS_BIG) return jsb_relational(J, op, l, r);
            double a = js_to_num(J, l), b = js_to_num(J, r);
            switch (op) {
                case OP_LT: return js_bool(a < b);
                case OP_GT: return js_bool(a > b);
                case OP_LE: return js_bool(a <= b);
                default:    return js_bool(a >= b);
            }
        }

        case OP_EQ:  return js_bool(js_loose_eq(J, l, r));
        case OP_NE:  return js_bool(!js_loose_eq(J, l, r));
        case OP_SEQ: return js_bool(js_strict_eq(l, r));
        case OP_SNE: return js_bool(!js_strict_eq(l, r));

        case OP_BAND: return js_num((double)(js_to_i32(J, l) & js_to_i32(J, r)));
        case OP_BOR:  return js_num((double)(js_to_i32(J, l) | js_to_i32(J, r)));
        case OP_BXOR: return js_num((double)(js_to_i32(J, l) ^ js_to_i32(J, r)));
        case OP_SHL:  return js_num((double)(int)((u32)js_to_i32(J, l) << (js_to_u32(J, r) & 31)));
        case OP_SHR:  return js_num((double)(js_to_i32(J, l) >> (js_to_u32(J, r) & 31)));
        case OP_USHR: return js_num((double)(js_to_u32(J, l) >> (js_to_u32(J, r) & 31)));

        /* `x instanceof F`: whether F.prototype is anywhere up x's chain, or
           whatever F's Symbol.hasInstance says. */
        case OP_INSTANCEOF: {
            if (r.t != JS_OBJ || !r.obj)
                return js_throw(J, JS_ERR_TYPE, "the right of instanceof is not a constructor", line);
            jval hi = js_get(J, r, J->sym_has_instance);
            if (J->sig != JS_OK) return js_undef();
            if (js_callable(hi) && !(hi.obj->kind == JO_NATIVE && hi.obj->name
                                     && js_str_is(hi.obj->name, "[Symbol.hasInstance]"))) {
                jval res = js_call(J, hi, r, &l, 1);
                return js_bool(js_to_bool(res));
            }
            if (!js_callable(r))
                return js_throw(J, JS_ERR_TYPE, "the right of instanceof is not a constructor", line);
            /* A bound function answers for what it was bound to. */
            while (r.obj->kind == JO_NATIVE && r.obj->fn == nat_fn_bound && js_callable(r.obj->data))
                r = r.obj->data;
            if (l.t != JS_OBJ || !l.obj) return js_bool(0);
            jval pr = js_get(J, r, J->s_prototype);
            if (J->sig != JS_OK) return js_undef();
            if (!js_is_obj(pr)) return js_bool(0);
            int depth = 0;
            for (jobj *o = l.obj->proto; o && depth < 10000; o = o->proto, depth++)
                if (o == pr.obj) return js_bool(1);
            return js_bool(0);
        }

        case OP_IN: {
            if (r.t != JS_OBJ || !r.obj)
                return js_throw(J, JS_ERR_TYPE, "the right of in is not an object", line);
            jstr *k = js_to_key(J, l);
            if (J->sig != JS_OK) return js_undef();
            return js_bool(js_has(J, r.obj, k));
        }

        default:
            return js_throw(J, JS_ERR_SYNTAX, "this operator is not one this engine has", line);
    }
}

/* --- the heavier kinds of expression, out of the way of the rest -----------
 *
 * Each in a function of its own, so the evaluator's own frame -- which is on
 * the stack once for every level of nesting in every function being run --
 * stays small. */

/* The arguments of a call or a new, spread ones included. */
static int js_eval_args(jctx *J, int cell, jscope *sc, jval this_val, jargs *A) {
    for (; cell >= 0; cell = J->nodes[cell].b) {
        int e = J->nodes[cell].a;
        if (e >= 0 && J->nodes[e].kind == N_SPREAD) {
            jval v = js_eval(J, J->nodes[e].a, sc, this_val);
            if (J->sig != JS_OK) return 0;
            if (!js_iter_collect(J, v, A)) return 0;
            continue;
        }
        jval v = js_eval(J, e, sc, this_val);
        if (J->sig != JS_OK) return 0;
        if (!js_args_push(J, A, v)) return 0;
    }
    return 1;
}

static jval js_direct_eval(jctx *J, jval *argv, int argc, jscope *sc, jval this_val);

/* Say which name was not a function. "this is not a function" is true and
   useless: a page calls hundreds of them and the message names none. */
static jval js_not_function(jctx *J, int callee, int line) {
    int k = J->nodes[callee].kind;
    J->error_line = line;
    if ((k == N_MEMBER || k == N_IDENT) && J->nodes[callee].str)
        return js_throw_named(J, JS_ERR_TYPE, "", J->nodes[callee].str, " is not a function");
    if (k == N_INDEX && J->nodes[J->nodes[callee].b].kind == N_STR)
        return js_throw_named(J, JS_ERR_TYPE, "", J->nodes[J->nodes[callee].b].str, " is not a function");
    return js_throw(J, JS_ERR_TYPE, "this is not a function", line);
}

static JS_NOINLINE jval js_eval_call(jctx *J, int node, jscope *sc, jval this_val) {
    int callee = J->nodes[node].a;
    int args = J->nodes[node].b;
    int opt = J->nodes[node].flags & NF_OPT;
    int line = J->nodes[node].line;
    int ck = J->nodes[callee].kind;
    jval fn, self = js_undef();

    if (ck == N_MEMBER || ck == N_INDEX) {
        int inner = J->nodes[callee].a;
        int link_opt = J->nodes[callee].flags & NF_OPT;
        self = js_eval(J, inner, sc, this_val);
        if (J->chain_short || J->sig != JS_OK) return js_undef();
        if (link_opt && (self.t == JS_NULL || self.t == JS_UNDEF)) {
            J->chain_short = 1;
            return js_undef();
        }
        jstr *key;
        if (ck == N_MEMBER) {
            key = J->nodes[callee].str;
            if (J->nodes[callee].flags & NF_PRIVATE) key = js_private_key(J, sc, key);
        } else {
            key = js_to_key(J, js_eval(J, J->nodes[callee].b, sc, this_val));
        }
        if (J->sig != JS_OK) return js_undef();
        J->error_line = line;
        if (self.t == JS_NULL || self.t == JS_UNDEF)
            return js_nothing_from(J, key, self, inner);
        fn = js_getv(J, self, key, self);
    } else if (ck == N_SUPERMEMBER) {
        jplace p = js_place(J, callee, sc, this_val);
        if (J->sig != JS_OK) return js_undef();
        fn = js_place_get(J, &p, sc);
        self = p.self;
    } else if (ck == N_IDENT && J->with_used) {
        /* A function a with statement's object has is called on it. */
        int found;
        jobj *with = 0;
        fn = js_ident_in(J, J->nodes[callee].str, sc, &found, &with);
        if (J->sig == JS_OK && !found)
            return js_throw_named(J, JS_ERR_REFERENCE, "", J->nodes[callee].str, " is not defined");
        if (with) self = js_from_obj(with);
    } else {
        fn = js_eval(J, callee, sc, this_val);
        if (J->chain_short) return js_undef();
    }
    if (J->sig != JS_OK) return js_undef();
    if (opt && (fn.t == JS_NULL || fn.t == JS_UNDEF)) {
        J->chain_short = 1;
        return js_undef();
    }

    jargs A;
    js_args_init(&A);
    if (!js_eval_args(J, args, sc, this_val, &A)) { js_args_free(&A); return js_undef(); }

    jval r;
    if (!js_callable(fn)) r = js_not_function(J, callee, line);
    else if (ck == N_IDENT && fn.obj == J->eval_fn) r = js_direct_eval(J, A.v, A.n, sc, this_val);
    else {
        J->error_line = line;
        r = js_call(J, fn, self, A.v, A.n);
    }
    js_args_free(&A);
    return r;
}

static JS_NOINLINE jval js_eval_new(jctx *J, int node, jscope *sc, jval this_val) {
    int callee = J->nodes[node].a;
    int args = J->nodes[node].b;
    int line = J->nodes[node].line;
    jval fn = js_eval(J, callee, sc, this_val);
    if (J->sig != JS_OK) return js_undef();
    jargs A;
    js_args_init(&A);
    if (!js_eval_args(J, args, sc, this_val, &A)) { js_args_free(&A); return js_undef(); }
    jval r;
    J->error_line = line;
    if (!js_is_constructor(J, fn)) {
        int k = J->nodes[callee].kind;
        if ((k == N_IDENT || k == N_MEMBER) && J->nodes[callee].str)
            r = js_throw_named(J, JS_ERR_TYPE, "", J->nodes[callee].str, " is not a constructor");
        else r = js_throw(J, JS_ERR_TYPE, "this is not a constructor", line);
    } else {
        r = js_construct(J, fn, A.v, A.n, fn);
    }
    js_args_free(&A);
    return r;
}

static JS_NOINLINE jval js_eval_array(jctx *J, int node, jscope *sc, jval this_val) {
    jobj *a = js_array(J);
    if (!a) return js_undef();
    for (int cell = J->nodes[node].a; cell >= 0; cell = J->nodes[cell].b) {
        int e = J->nodes[cell].a;
        if (e < 0) { js_arr_push(J, a, js_undef()); continue; }
        if (J->nodes[e].kind == N_SPREAD) {
            jval v = js_eval(J, J->nodes[e].a, sc, this_val);
            if (J->sig != JS_OK) break;
            jiter it;
            if (!js_iter_open(J, v, &it)) break;
            for (;;) {
                jval x;
                int r = js_iter_step(J, &it, &x);
                if (r <= 0) break;
                js_arr_push(J, a, x);
            }
            if (J->sig != JS_OK) break;
            continue;
        }
        jval v = js_eval(J, e, sc, this_val);
        if (J->sig != JS_OK) break;
        js_arr_push(J, a, v);
    }
    return js_from_obj(a);
}

/* Everything an object has of its own and may be walked, copied onto
   another: {...x} and Object.assign. */
static void js_copy_props(jctx *J, jobj *to, jval from) {
    if (from.t == JS_STR) {
        for (u32 i = 0; from.str && i < from.str->len; i++)
            js_define(J, to, js_to_key(J, js_num(i)), js_from_str(js_str_n(J, from.str->s + i, 1)), JP_PLAIN);
        return;
    }
    if (!js_is_obj(from)) return;
    jobj *src = from.obj;
    if (src->kind == JO_ARRAY || src->kind == JO_ARGS)
        for (u32 i = 0; i < src->len; i++)
            js_define(J, to, js_to_key(J, js_num(i)), src->items[i], JP_PLAIN);
    jprop **own;
    u32 nown = js_keys_of(J, src, &own, JK_ENUM | JK_STR | JK_SYM);
    for (u32 i = 0; i < nown && J->sig == JS_OK; i++)
        js_define(J, to, own[i]->key, js_prop_read(J, own[i], from), JP_PLAIN);
}

static JS_NOINLINE jval js_eval_object(jctx *J, int node, jscope *sc, jval this_val) {
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    for (int cell = J->nodes[node].a; cell >= 0; cell = J->nodes[cell].b) {
        int kind = J->nodes[cell].op;
        int val = J->nodes[cell].a;
        if (kind == PK_SPREAD) {
            jval v = js_eval(J, val, sc, this_val);
            if (J->sig != JS_OK) break;
            js_copy_props(J, o, v);
            continue;
        }
        jstr *key = J->nodes[cell].str;
        int ck = J->nodes[cell].c;
        if (ck >= 0) {
            key = js_to_key(J, js_eval(J, ck, sc, this_val));
            if (J->sig != JS_OK) break;
        }
        if (kind == PK_SHORT && J->nodes[cell].d >= 0) {
            js_throw(J, JS_ERR_SYNTAX, "{a = b} only means something as a pattern", J->nodes[cell].line);
            break;
        }
        if (kind == PK_METHOD || kind == PK_GET || kind == PK_SET) {
            jobj *f = js_make_function(J, val, sc, this_val);
            if (!f) break;
            f->home = o;
            if (ck >= 0 && !f->name) f->name = key;
            if (kind == PK_METHOD) js_define(J, o, key, js_from_obj(f), JP_PLAIN);
            else js_define_accessor(J, o, key, kind == PK_GET ? js_from_obj(f) : js_undef(),
                                    kind == PK_SET ? js_from_obj(f) : js_undef(), JP_ENUM | JP_CONF);
            continue;
        }
        jval v = js_eval(J, val, sc, this_val);
        if (J->sig != JS_OK) break;
        if (kind == PK_PROTO) {
            if (v.t == JS_OBJ) o->proto = v.obj;
            else if (v.t == JS_NULL) o->proto = 0;
            continue;
        }
        js_define(J, o, key, v, JP_PLAIN);
    }
    return js_from_obj(o);
}

/* A growing buffer of text, made a string once at the end. */
typedef struct { char *b; u32 n, cap; int full; } jtext;

static void jt_put(jctx *J, jtext *t, const char *s, u32 n) {
    if (t->full) return;
    if (t->n + n + 1 > t->cap) {
        u32 cap = t->cap ? t->cap : 128;
        while (cap < t->n + n + 1) cap *= 2;
        if (cap > J->mem_cap) { t->full = 1; js_out_of_memory(J); return; }
        char *nb = (char *)malloc(cap);
        if (!nb) { t->full = 1; js_out_of_memory(J); return; }
        volatile char *d = nb;
        for (u32 i = 0; i < t->n; i++) d[i] = t->b[i];
        free(t->b);
        t->b = nb;
        t->cap = cap;
    }
    volatile char *d = t->b + t->n;
    for (u32 i = 0; i < n; i++) d[i] = s[i];
    t->n += n;
}

static jstr *jt_done(jctx *J, jtext *t) {
    jstr *s = js_str_n(J, t->b ? t->b : "", t->n);
    free(t->b);
    t->b = 0;
    return s;
}

static JS_NOINLINE jval js_eval_template(jctx *J, int node, jscope *sc, jval this_val) {
    jtext t = { 0, 0, 0, 0 };
    for (int cell = J->nodes[node].a; cell >= 0; cell = J->nodes[cell].b) {
        jstr *piece = J->nodes[cell].str;
        if (piece) jt_put(J, &t, piece->s, piece->len);
        int e = J->nodes[cell].a;
        if (e >= 0) {
            jval v = js_eval(J, e, sc, this_val);
            if (J->sig != JS_OK) { free(t.b); return js_undef(); }
            jstr *s = js_to_str(J, v);
            if (J->sig != JS_OK) { free(t.b); return js_undef(); }
            jt_put(J, &t, s->s, s->len);
        }
    }
    return js_from_str(jt_done(J, &t));
}

/* tag`a${b}c`: the tag called with the pieces -- the same array every time
   this place in the text runs, as the standard has it and as the libraries
   that cache by it expect -- and the values. */
static jval nat_obj_freeze(jctx *J, jval t, jval *a, int n);
static JS_NOINLINE jval js_eval_tagged(jctx *J, int node, jscope *sc, jval this_val) {
    int tagn = J->nodes[node].a;
    int tpl = J->nodes[node].b;
    jval fn, self = js_undef();
    int tk = J->nodes[tagn].kind;
    if (tk == N_MEMBER || tk == N_INDEX) {
        jplace p = js_place(J, tagn, sc, this_val);
        if (J->sig != JS_OK) return js_undef();
        self = p.obj;
        fn = js_place_get(J, &p, sc);
    } else fn = js_eval(J, tagn, sc, this_val);
    if (J->sig != JS_OK) return js_undef();

    jobj *strings = 0;
    int cached = J->nodes[node].c;
    if (cached >= 0 && J->tagged && (u32)cached < J->tagged->len)
        strings = J->tagged->items[cached].obj;
    if (!strings) {
        strings = js_array(J);
        jobj *raw = js_array(J);
        if (!strings || !raw) return js_undef();
        for (int cell = J->nodes[tpl].a; cell >= 0; cell = J->nodes[cell].b) {
            jstr *cooked = J->nodes[cell].str;
            js_arr_push(J, strings, cooked ? js_from_str(cooked) : js_undef());
            int rn = J->nodes[cell].d;
            js_arr_push(J, raw, rn >= 0 ? js_from_str(J->nodes[rn].str) : js_undef());
        }
        raw->flags |= JOF_FROZEN | JOF_NOEXT;
        js_put_prop_flags(J, strings, js_str(J, "raw"), js_from_obj(raw), 0);
        strings->flags |= JOF_FROZEN | JOF_NOEXT;
        /* Kept by the engine, indexed from the node. */
        if (!J->tagged) J->tagged = js_array(J);
        if (J->tagged) {
            J->nodes[node].c = (int)J->tagged->len;
            js_arr_push(J, J->tagged, js_from_obj(strings));
        }
    }
    jargs A;
    js_args_init(&A);
    js_args_push(J, &A, js_from_obj(strings));
    for (int cell = J->nodes[tpl].a; cell >= 0; cell = J->nodes[cell].b) {
        int e = J->nodes[cell].a;
        if (e < 0) continue;
        jval v = js_eval(J, e, sc, this_val);
        if (J->sig != JS_OK) { js_args_free(&A); return js_undef(); }
        js_args_push(J, &A, v);
    }
    jval r = js_callable(fn) ? js_call(J, fn, self, A.v, A.n) : js_not_function(J, tagn, J->nodes[node].line);
    js_args_free(&A);
    return r;
}

/* What each new instance of a class is given before its constructor's body
   runs: its fields, in order, each worked out with the instance as this. */
static void js_init_fields(jctx *J, jobj *f, jval obj) {
    jobj *list = f->fields;
    if (!list || !js_is_obj(obj)) return;
    for (u32 i = 0; i + 1 < list->len && J->sig == JS_OK; i += 2) {
        jval k = list->items[i];
        jval init = list->items[i + 1];
        jval v = js_callable(init) ? js_call(J, init, obj, 0, 0) : js_undef();
        if (J->sig != JS_OK) return;
        jstr *key = k.t == JS_SYM || k.t == JS_STR ? k.str : js_to_key(J, k);
        js_define(J, obj.obj, key, v, k.t == JS_SYM ? (JP_WRITE | JP_CONF) : JP_PLAIN);
    }
}

static JS_NOINLINE jval js_eval_class(jctx *J, int node, jscope *sc, jval this_val) {
    jstr *name = J->nodes[node].str;
    int heritage = J->nodes[node].a;
    int members = J->nodes[node].b;
    int ctor_node = J->nodes[node].c;

    jobj *proto_parent = J->p_object;
    jobj *ctor_parent = J->p_function;
    if (heritage >= 0) {
        jval parent = js_eval(J, heritage, sc, this_val);
        if (J->sig != JS_OK) return js_undef();
        if (parent.t == JS_NULL) {
            proto_parent = 0;
        } else {
            if (!js_is_constructor(J, parent))
                return js_throw(J, JS_ERR_TYPE, "a class can only extend a constructor or null",
                                J->nodes[node].line);
            jval pp = js_get(J, parent, J->s_prototype);
            if (J->sig != JS_OK) return js_undef();
            if (pp.t != JS_NULL && !js_is_obj(pp))
                return js_throw(J, JS_ERR_TYPE, "what a class extends has no prototype object",
                                J->nodes[node].line);
            proto_parent = pp.t == JS_NULL ? 0 : pp.obj;
            ctor_parent = parent.obj;
        }
    }

    /* The class's own scope: its name, for use inside it, and each #name. */
    jscope *csc = js_scope(J, sc);
    for (int cell = members; cell >= 0; cell = J->nodes[cell].b) {
        if (!(J->nodes[cell].flags & NF_PRIVATE)) continue;
        jstr *pn = J->nodes[cell].str;
        if (!js_find(csc->vars, pn))
            js_declare_flags(J, csc, pn, js_from_sym(js_sym_new(J, pn->s, pn->len)), 0);
    }
    if (name) js_declare_flags(J, csc, name, js_hole(), JP_ENUM | JP_CONF);

    jobj *proto = js_object_with(J, JO_PLAIN, proto_parent);
    jobj *F = js_make_function(J, ctor_node, csc, this_val);
    if (!proto || !F) return js_undef();
    F->proto = ctor_parent;
    F->home = proto;
    if (!F->name) F->name = name;
    js_put_prop_flags(J, F, J->s_prototype, js_from_obj(proto), 0);
    js_put_prop_flags(J, proto, J->s_constructor, js_from_obj(F), JP_WRITE | JP_CONF);

    jobj *statics = 0;
    for (int cell = members; cell >= 0 && J->sig == JS_OK; cell = J->nodes[cell].b) {
        int kind = J->nodes[cell].op;
        int is_static = J->nodes[cell].flags & NF_STATIC;
        int priv = J->nodes[cell].flags & NF_PRIVATE;
        int val = J->nodes[cell].a;
        jobj *target = is_static ? F : proto;

        if (kind == PK_BLOCK) {
            if (!statics) statics = js_array(J);
            jobj *f = js_make_function(J, val, csc, this_val);
            if (f) f->home = F;
            if (statics) {
                js_arr_push(J, statics, js_undef());
                js_arr_push(J, statics, js_from_obj(f));
            }
            continue;
        }
        jval keyv;
        jstr *key = J->nodes[cell].str;
        if (priv) {
            key = js_private_key(J, csc, key);
            keyv = js_from_sym(key);
        } else if (J->nodes[cell].c >= 0) {
            key = js_to_key(J, js_eval(J, J->nodes[cell].c, csc, this_val));
            if (J->sig != JS_OK) break;
            keyv = js_is_sym_key(key) ? js_from_sym(key) : js_from_str(key);
        } else {
            keyv = js_from_str(key);
        }

        if (kind == PK_FIELD) {
            jobj *init = val >= 0 ? js_make_function(J, val, csc, this_val) : 0;
            if (init) init->home = target;
            jobj **list = is_static ? &statics : &F->fields;
            if (!*list) *list = js_array(J);
            if (*list) {
                js_arr_push(J, *list, keyv);
                js_arr_push(J, *list, init ? js_from_obj(init) : js_undef());
            }
            continue;
        }
        jobj *f = js_make_function(J, val, csc, this_val);
        if (!f) break;
        f->home = target;
        if (!f->name) f->name = key;
        int fl = JP_WRITE | JP_CONF;
        if (kind == PK_METHOD) js_define(J, target, key, js_from_obj(f), fl);
        else js_define_accessor(J, target, key, kind == PK_GET ? js_from_obj(f) : js_undef(),
                                kind == PK_SET ? js_from_obj(f) : js_undef(), JP_CONF);
    }
    if (J->sig != JS_OK) return js_undef();
    if (name) js_declare_flags(J, csc, name, js_from_obj(F), JP_ENUM | JP_CONF);

    /* Static fields and blocks, in the order written, with the class as
       `this`. */
    if (statics) {
        jval Fv = js_from_obj(F);
        for (u32 i = 0; i + 1 < statics->len && J->sig == JS_OK; i += 2) {
            jval k = statics->items[i], init = statics->items[i + 1];
            if (k.t == JS_UNDEF) {                     /* a static block */
                if (js_callable(init)) js_call(J, init, Fv, 0, 0);
                continue;
            }
            jval v = js_callable(init) ? js_call(J, init, Fv, 0, 0) : js_undef();
            if (J->sig != JS_OK) break;
            js_define(J, F, k.str, v, k.t == JS_SYM ? (JP_WRITE | JP_CONF) : JP_PLAIN);
        }
    }
    return J->sig == JS_OK ? js_from_obj(F) : js_undef();
}

static JS_NOINLINE jval js_eval_super_call(jctx *J, int node, jscope *sc, jval this_val) {
    (void)this_val;
    jprop *fp = js_lookup(sc, J->s_fnself);
    jprop *nt = js_lookup(sc, J->s_newtarget);
    jprop *tp = js_lookup(sc, J->s_this);
    if (!fp || !js_is_obj(fp->v) || !tp)
        return js_throw(J, JS_ERR_SYNTAX, "super() is only for a derived class's constructor",
                        J->nodes[node].line);
    jobj *f = fp->v.obj;
    jval parent = f->proto ? js_from_obj(f->proto) : js_undef();
    jargs A;
    js_args_init(&A);
    if (!js_eval_args(J, J->nodes[node].b, sc, this_val, &A)) { js_args_free(&A); return js_undef(); }
    J->error_line = J->nodes[node].line;
    jval r = js_is_constructor(J, parent)
           ? js_construct(J, parent, A.v, A.n, nt ? nt->v : js_undef())
           : js_throw(J, JS_ERR_TYPE, "what this class extends cannot be constructed", J->error_line);
    js_args_free(&A);
    if (J->sig != JS_OK) return js_undef();
    if (tp->v.t != JS_HOLE)
        return js_throw(J, JS_ERR_REFERENCE, "super() has already been called", J->error_line);
    tp->v = r;
    js_init_fields(J, f, r);
    return r;
}

static jval co_yield(jctx *J, jval v);
static jval co_yield_star(jctx *J, jval v);
static jval co_await(jctx *J, jval v);

static JS_NOINLINE jval js_eval_assign(jctx *J, int node, jscope *sc, jval this_val) {
    int target = J->nodes[node].a;
    int value = J->nodes[node].b;
    int op = J->nodes[node].op;
    int line = J->nodes[node].line;
    int tk = J->nodes[target].kind;

    if (tk == N_APAT || tk == N_OPAT) {
        jval r = js_eval(J, value, sc, this_val);
        if (J->sig != JS_OK) return js_undef();
        js_bind(J, target, r, sc, this_val, 0);
        return r;
    }
    /* A plain name, the commonest assignment there is, without the place. */
    if (op == OP_ASSIGN && tk == N_IDENT) {
        jval r = js_eval(J, value, sc, this_val);
        if (J->sig != JS_OK) return js_undef();
        js_assign_name(J, sc, J->nodes[target].str, r);
        return r;
    }
    jplace p = js_place(J, target, sc, this_val);
    if (J->sig != JS_OK) return js_undef();
    if (p.kind == 2) return js_throw(J, JS_ERR_SYNTAX, "this cannot be assigned to", line);
    if ((p.kind == 1) && (p.obj.t == JS_NULL || p.obj.t == JS_UNDEF))
        return js_nothing(J, "cannot set ", p.name, p.obj);

    if (op == OP_LANDEQ || op == OP_LOREQ || op == OP_NULLEQ) {
        /* a ||= b assigns only when it has to, and b is not even worked
           out otherwise. */
        jval cur = js_place_get(J, &p, sc);
        if (J->sig != JS_OK) return js_undef();
        int keep = op == OP_LANDEQ ? !js_to_bool(cur)
                 : op == OP_LOREQ ? js_to_bool(cur)
                 : !(cur.t == JS_NULL || cur.t == JS_UNDEF);
        if (keep) return cur;
        jval r = js_eval(J, value, sc, this_val);
        if (J->sig != JS_OK) return js_undef();
        js_place_put(J, &p, sc, r);
        return r;
    }

    jval r;
    if (op != OP_ASSIGN) {
        jval cur = js_place_get(J, &p, sc);
        if (J->sig != JS_OK) return js_undef();
        r = js_eval(J, value, sc, this_val);
        if (J->sig != JS_OK) return js_undef();
        jop base = OP_ADD;
        switch (op) {
            case OP_ADDEQ: base = OP_ADD; break;
            case OP_SUBEQ: base = OP_SUB; break;
            case OP_MULEQ: base = OP_MUL; break;
            case OP_DIVEQ: base = OP_DIV; break;
            case OP_MODEQ: base = OP_MOD; break;
            case OP_OREQ:  base = OP_BOR; break;
            case OP_ANDEQ: base = OP_BAND; break;
            case OP_XOREQ: base = OP_BXOR; break;
            case OP_SHLEQ: base = OP_SHL; break;
            case OP_SHREQ: base = OP_SHR; break;
            case OP_USHREQ: base = OP_USHR; break;
            case OP_POWEQ: base = OP_POW; break;
            default: break;
        }
        r = js_binary(J, base, cur, r, line);
        if (J->sig != JS_OK) return js_undef();
    } else {
        r = js_eval(J, value, sc, this_val);
        if (J->sig != JS_OK) return js_undef();
    }
    js_place_put(J, &p, sc, r);
    return r;
}

static JS_NOINLINE jval js_eval_unaryish(jctx *J, int node, jscope *sc, jval this_val) {
    int kind = J->nodes[node].kind;
    int a = J->nodes[node].a;
    int op = J->nodes[node].op;
    if (kind == N_TYPEOF) {
        /* typeof on a name that does not exist is "undefined" rather than
           an error, which is the whole reason it is used. */
        jval v;
        if (a >= 0 && J->nodes[a].kind == N_IDENT) {
            int found;
            v = js_ident_soft(J, J->nodes[a].str, sc, &found);
        } else v = js_eval(J, a, sc, this_val);
        if (J->sig != JS_OK) return js_undef();
        return js_from_str(js_str(J, js_typeof_name(v)));
    }
    if (kind == N_DELETE) {
        int k = a >= 0 ? J->nodes[a].kind : -1;
        if (k == N_OPTCHAIN) {
            int inner = J->nodes[a].a;
            jplace p = js_place(J, inner, sc, this_val);
            int shorted = J->chain_short;
            J->chain_short = 0;
            if (shorted || J->sig != JS_OK) return js_bool(1);
            if (p.kind == 4) { p.kind = 1; p.name = js_to_key(J, js_num(p.idx)); }
            if (p.kind == 1) return js_bool(js_delete(J, p.obj, p.name));
            return js_bool(1);
        }
        if (k == N_MEMBER || k == N_INDEX) {
            jplace p = js_place(J, a, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            if (p.kind == 4) { p.kind = 1; p.name = js_to_key(J, js_num(p.idx)); }
            if (p.obj.t == JS_NULL || p.obj.t == JS_UNDEF)
                return js_nothing(J, "cannot delete ", p.name, p.obj);
            return js_bool(js_delete(J, p.obj, p.name));
        }
        if (k == N_IDENT) return js_bool(0);
        js_eval(J, a, sc, this_val);
        return js_bool(1);
    }
    if (kind == N_PREINC || kind == N_POSTINC) {
        jplace p = js_place(J, a, sc, this_val);
        if (J->sig != JS_OK) return js_undef();
        if (p.kind == 2) return js_throw(J, JS_ERR_SYNTAX, "this cannot be incremented", J->nodes[node].line);
        jval old = js_place_get(J, &p, sc);
        if (J->sig != JS_OK) return js_undef();
        if (old.t == JS_OBJ) { old = js_to_primitive(J, old, 1); if (J->sig != JS_OK) return js_undef(); }
        if (old.t == JS_BIG) {
            jval nb = jsb_step(J, old, op == OP_INC);
            if (J->sig != JS_OK) return js_undef();
            js_place_put(J, &p, sc, nb);
            return kind == N_PREINC ? nb : old;
        }
        double cur = js_to_num(J, old);
        if (J->sig != JS_OK) return js_undef();
        double next = op == OP_INC ? cur + 1 : cur - 1;
        js_place_put(J, &p, sc, js_num(next));
        return js_num(kind == N_PREINC ? next : cur);
    }
    /* N_UNARY */
    jval v = js_eval(J, a, sc, this_val);
    if (J->sig != JS_OK) return js_undef();
    if ((op == OP_NEG || op == OP_BNOT) && (v.t == JS_BIG || v.t == JS_OBJ)) {
        if (v.t == JS_OBJ) { v = js_to_primitive(J, v, 1); if (J->sig != JS_OK) return js_undef(); }
        if (v.t == JS_BIG) return op == OP_NEG ? jsb_negate(J, v) : jsb_bitnot(J, v);
    }
    switch (op) {
        case OP_NOT:  return js_bool(!js_to_bool(v));
        case OP_NEG:  return js_num(-js_to_num(J, v));
        case OP_POS:  return js_num(js_to_num(J, v));
        case OP_BNOT: return js_num((double)(~js_to_i32(J, v)));
        default:      return js_undef();      /* void */
    }
}

/* A member of something, x.y, x[y] or x.#y. */
static JS_NOINLINE jval js_eval_member(jctx *J, int node, jscope *sc, jval this_val) {
    int kind = J->nodes[node].kind;
    int a = J->nodes[node].a;
    int flags = J->nodes[node].flags;
    jval target = js_eval(J, a, sc, this_val);
    if (J->chain_short || J->sig != JS_OK) return js_undef();
    if ((flags & NF_OPT) && (target.t == JS_NULL || target.t == JS_UNDEF)) {
        J->chain_short = 1;
        return js_undef();
    }
    jstr *key;
    if (kind == N_MEMBER) {
        key = J->nodes[node].str;
        if (flags & NF_PRIVATE) {
            key = js_private_key(J, sc, key);
            if (!key) return js_undef();
        }
    } else {
        int b = J->nodes[node].b;
        jval idx = js_eval(J, b, sc, this_val);
        if (J->sig != JS_OK) return js_undef();
        if (target.t == JS_OBJ && target.obj && idx.t == JS_NUM) {
            double d = idx.num;
            jobj *o = target.obj;
            if ((o->kind == JO_ARRAY || o->kind == JO_ARGS)
                && d >= 0 && d < (double)o->len && d == (double)(u32)d)
                return o->items[(u32)d];
            if (o->kind == JO_TYPED && d >= 0 && d == (double)(u32)d)
                return js_ta_get(J, o, (u32)d);
        }
        if (target.t == JS_STR && idx.t == JS_NUM && target.str) {
            double d = idx.num;
            if (d >= 0 && d < (double)target.str->len && d == (double)(u32)d)
                return js_from_str(js_str_n(J, target.str->s + (u32)d, 1));
        }
        key = js_to_key(J, idx);
        if (J->sig != JS_OK) return js_undef();
    }
    J->error_line = J->nodes[node].line;
    if (target.t == JS_UNDEF || target.t == JS_NULL) return js_nothing_from(J, key, target, a);
    return js_getv(J, target, key, target);
}

static JS_NOINLINE jval js_eval_misc(jctx *J, int node, jscope *sc, jval this_val) {
    int kind = J->nodes[node].kind;
    switch (kind) {
        case N_REGEX: {
            jstr *s = J->nodes[node].str;
            /* A fresh object each time the literal is reached, because a
               global pattern carries a lastIndex and two loops sharing one
               would each start where the other left off. */
            jobj *o = js_regex_new(J, s ? s->s : "", s ? s->len : 0, J->nodes[node].op);
            return o ? js_from_obj(o) : js_undef();
        }
        case N_NEWTARGET: {
            jprop *p = js_lookup(sc, J->s_newtarget);
            return p ? p->v : js_undef();
        }
        case N_SUPERMEMBER: {
            jplace p = js_place(J, node, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            return js_place_get(J, &p, sc);
        }
        case N_YIELD: {
            int a = J->nodes[node].a;
            int star = J->nodes[node].op;
            jval v = a >= 0 ? js_eval(J, a, sc, this_val) : js_undef();
            if (J->sig != JS_OK) return js_undef();
            return star ? co_yield_star(J, v) : co_yield(J, v);
        }
        case N_AWAIT: {
            jval v = js_eval(J, J->nodes[node].a, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            return co_await(J, v);
        }
        case N_IMPORT: {
            jval spec = js_eval(J, J->nodes[node].a, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            if (J->import_hook) return J->import_hook(J, spec, js_module_of(sc));
            jobj *p = js_promise_new(J);
            jobj *e = js_error_with(J, J->err_proto[JS_ERR_TYPE], js_str(J, "TypeError"),
                                    js_str(J, "this engine does not have modules"));
            if (p) js_promise_settle(J, p, 0, js_from_obj(e));
            return js_from_obj(p);
        }
        case N_IMPORTMETA:
            return J->meta_hook ? J->meta_hook(J, js_module_of(sc)) : js_undef();
        case N_PRIVNAME:
            return js_throw(J, JS_ERR_SYNTAX, "a #name on its own is only for `in`", J->nodes[node].line);
        case N_SPREAD:
            return js_throw(J, JS_ERR_SYNTAX, "... only means something in a list", J->nodes[node].line);
        case N_APAT: case N_OPAT:
            return js_throw(J, JS_ERR_SYNTAX, "a pattern is not a value", J->nodes[node].line);
        default:
            return js_undef();
    }
}

/* #x in o: whether o has the class's #x. */
static JS_NOINLINE jval js_eval_private_in(jctx *J, int node, jscope *sc, jval this_val) {
    int a = J->nodes[node].a, b = J->nodes[node].b, line = J->nodes[node].line;
    jstr *key = js_private_key(J, sc, J->nodes[a].str);
    jval r = js_eval(J, b, sc, this_val);
    if (J->sig != JS_OK || !key) return js_undef();
    if (!js_is_obj(r)) return js_throw(J, JS_ERR_TYPE, "the right of in is not an object", line);
    return js_bool(js_has(J, r.obj, key));
}

static jval js_eval(jctx *J, int node, jscope *sc, jval this_val) {
    if (node < 0) return js_undef();
    if (!js_tick(J)) return js_undef();
    if ((char *)&node < J->stack_limit)
        return js_throw(J, JS_ERR_RANGE, "too many nested calls", J->error_line);
    int kind = J->nodes[node].kind;
    J->error_line = J->nodes[node].line;

    switch (kind) {
        case N_NUM:   return js_num(J->nodes[node].num);
        case N_BIGINT: {
            jbint *b = jsb_literal(J, J->nodes[node].str);
            return b ? js_from_big(b) : js_undef();
        }
        case N_STR:   return js_from_str(J->nodes[node].str);
        case N_TRUE:  return js_bool(1);
        case N_FALSE: return js_bool(0);
        case N_NULL:  return js_null();
        case N_UNDEF: return js_undef();
        case N_THIS:  return this_val.t == JS_HOLE ? js_this_binding(J, sc) : this_val;
        case N_IDENT: return js_ident(J, J->nodes[node].str, sc);

        case N_MEMBER: case N_INDEX:
            return js_eval_member(J, node, sc, this_val);
        case N_CALL:   return js_eval_call(J, node, sc, this_val);
        case N_NEW:    return js_eval_new(J, node, sc, this_val);
        case N_ARRAY:  return js_eval_array(J, node, sc, this_val);
        case N_OBJECT: return js_eval_object(J, node, sc, this_val);
        case N_TEMPLATE: return js_eval_template(J, node, sc, this_val);
        case N_TAGGED: return js_eval_tagged(J, node, sc, this_val);
        case N_CLASS:  return js_eval_class(J, node, sc, this_val);
        case N_SUPERCALL: return js_eval_super_call(J, node, sc, this_val);
        case N_ASSIGN: return js_eval_assign(J, node, sc, this_val);
        case N_UNARY: case N_TYPEOF: case N_DELETE: case N_PREINC: case N_POSTINC:
            return js_eval_unaryish(J, node, sc, this_val);

        case N_FUNC: {
            jobj *f = js_make_function(J, node, sc, this_val);
            return js_from_obj(f);
        }

        case N_OPTCHAIN: {
            jval v = js_eval(J, J->nodes[node].a, sc, this_val);
            J->chain_short = 0;
            return v;
        }

        case N_BINARY: {
            int a = J->nodes[node].a, b = J->nodes[node].b, op = J->nodes[node].op;
            int line = J->nodes[node].line;
            if (op == OP_IN && a >= 0 && J->nodes[a].kind == N_PRIVNAME)
                return js_eval_private_in(J, node, sc, this_val);
            jval l = js_eval(J, a, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            jval r = js_eval(J, b, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            return js_binary(J, (jop)op, l, r, line);
        }

        case N_LOGICAL: {
            /* Short circuit, and the value of the side that decided it
               rather than a boolean: `a || b` is b when a is falsy. */
            int a = J->nodes[node].a, b = J->nodes[node].b, op = J->nodes[node].op;
            jval l = js_eval(J, a, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            if (op == OP_AND) return js_to_bool(l) ? js_eval(J, b, sc, this_val) : l;
            if (op == OP_OR) return js_to_bool(l) ? l : js_eval(J, b, sc, this_val);
            return l.t == JS_NULL || l.t == JS_UNDEF ? js_eval(J, b, sc, this_val) : l;
        }

        case N_COND: {
            int a = J->nodes[node].a, b = J->nodes[node].b, c = J->nodes[node].c;
            jval t = js_eval(J, a, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            return js_eval(J, js_to_bool(t) ? b : c, sc, this_val);
        }

        case N_SEQ: {
            int a = J->nodes[node].a, c = J->nodes[node].c;
            jval v = js_eval(J, a, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            if (c >= 0) return js_eval(J, c, sc, this_val);
            return v;
        }

        default:
            return js_eval_misc(J, node, sc, this_val);
    }
}

/* --- statements ------------------------------------------------------------ */

/* Whether a loop is done with a signal from its body: 1 to stop the loop
   and return `*out`, 0 to carry on with the next turn. */
static int js_loop_signal(jctx *J, jsignal s, jstr *mine, jsignal *out) {
    if (s == JS_BREAK) {
        if (!js_label_mine(J, mine)) { *out = s; return 1; }
        J->label = 0; J->sig = JS_OK;
        *out = JS_OK;
        return 1;
    }
    if (s == JS_CONTINUE) {
        if (!js_label_mine(J, mine)) { *out = s; return 1; }
        J->label = 0; J->sig = JS_OK;
        return 0;
    }
    if (s != JS_OK) { *out = s; return 1; }
    return 0;
}

/* The keys a for-in walks: the object's own enumerable ones and then those
   of its prototypes, each name once. */
static jobj *js_forin_keys(jctx *J, jobj *o) {
    jobj *keys = js_array(J);
    if (!keys) return 0;
    int depth = 0;
    for (jobj *q = o; q && depth < 64; q = q->proto, depth++) {
        if (q->kind == JO_ARRAY || q->kind == JO_ARGS || q->kind == JO_TYPED) {
            u32 len = q->kind == JO_TYPED ? js_ta_length(q) : q->len;
            for (u32 i = 0; i < len; i++) js_arr_push(J, keys, js_from_str(js_to_key(J, js_num(i))));
        }
        if (q->kind == JO_BOXED && q->ival.t == JS_STR)
            for (u32 i = 0; i < q->ival.str->len; i++)
                js_arr_push(J, keys, js_from_str(js_to_key(J, js_num(i))));
        jprop **own;
        u32 nown = js_keys_of(J, q, &own, JK_STR | (depth ? 0 : 0));
        for (u32 i = 0; i < nown; i++) {
            if (!(own[i]->flags & JP_ENUM)) continue;
            if (depth) {
                /* Shadowed further down, or already there: once only. */
                int seen = 0;
                for (u32 k = 0; k < keys->len && !seen; k++)
                    if (js_str_eq(keys->items[k].str, own[i]->key)) seen = 1;
                if (seen) continue;
            }
            js_arr_push(J, keys, js_from_str(own[i]->key));
        }
    }
    return keys;
}

static JS_NOINLINE jsignal js_exec_forin(jctx *J, int node, jscope *sc, jval this_val) {
    /* A label on it is its own, as a label on any loop is. */
    jstr *mine = J->pending_label;
    J->pending_label = 0;
    int a = J->nodes[node].a, body = J->nodes[node].b, target = J->nodes[node].c;
    int decl = J->nodes[node].d;
    jval obj = js_eval(J, a, sc, this_val);
    if (J->sig != JS_OK) return J->sig;
    if (obj.t == JS_STR) {
        jobj *b = js_object_with(J, JO_BOXED, J->p_string);
        if (!b) return J->sig;
        b->ival = obj;
        obj = js_from_obj(b);
    }
    if (!js_is_obj(obj)) return JS_OK;

    /* The keys are collected first. A body that adds or removes a property
       while the loop is running would otherwise walk a table that moved
       under it. */
    jobj *keys = js_forin_keys(J, obj.obj);
    if (!keys) return J->sig;
    jscope *isc = sc;
    jsignal out = JS_OK;
    for (u32 i = 0; i < keys->len; i++) {
        jval k = keys->items[i];
        /* A key deleted by an earlier turn is not walked. */
        if (obj.obj->kind == JO_PLAIN && !js_has(J, obj.obj, k.str)) continue;
        if (decl == VK_LET || decl == VK_CONST) {
            if (isc == sc || isc->escaped) isc = js_scope(J, sc);
            js_bind(J, target, k, isc, this_val, decl);
        } else {
            js_bind(J, target, k, sc, this_val, decl == VK_VAR ? VK_VAR : 0);
        }
        if (J->sig != JS_OK) return J->sig;
        jsignal s = js_exec(J, body, isc, this_val);
        if (js_loop_signal(J, s, mine, &out)) return out;
        if (!js_tick(J)) return J->sig;
    }
    return J->sig;
}

static jval co_await(jctx *J, jval v);

static JS_NOINLINE jsignal js_exec_forof(jctx *J, int node, jscope *sc, jval this_val) {
    jstr *mine = J->pending_label;
    J->pending_label = 0;
    int a = J->nodes[node].a, body = J->nodes[node].b, target = J->nodes[node].c;
    int decl = J->nodes[node].d, is_await = J->nodes[node].op;
    jval obj = js_eval(J, a, sc, this_val);
    if (J->sig != JS_OK) return J->sig;

    jiter it;
    if (is_await) {
        /* for await: the async iterator if there is one, or the ordinary one
           with each value awaited. */
        jval m = js_is_obj(obj) ? js_get(J, obj, J->sym_async_iterator) : js_undef();
        if (J->sig != JS_OK) return J->sig;
        if (js_callable(m)) {
            jval iter = js_call(J, m, obj, 0, 0);
            if (J->sig != JS_OK) return J->sig;
            it.kind = IT_GENERIC;
            it.obj = iter;
            it.next = js_get(J, iter, J->s_next);
            it.at = 0;
            it.entries = 2;                          /* 2: results are promises */
        } else if (!js_iter_open(J, obj, &it)) return J->sig;
    } else if (!js_iter_open(J, obj, &it)) return J->sig;

    jscope *isc = sc;
    jsignal out = JS_OK;
    for (;;) {
        jval x;
        int r;
        if (is_await && it.kind == IT_GENERIC && it.entries == 2) {
            jval res = js_call(J, it.next, it.obj, 0, 0);
            if (J->sig != JS_OK) return J->sig;
            res = co_await(J, res);
            if (J->sig != JS_OK) return J->sig;
            if (!js_is_obj(res)) return js_throw(J, JS_ERR_TYPE, "an async iterator gave back something that is not an object", J->error_line), J->sig;
            jval d = js_get(J, res, J->s_done);
            if (J->sig != JS_OK) return J->sig;
            if (js_to_bool(d)) break;
            x = js_get(J, res, J->s_value);
            r = J->sig == JS_OK ? 1 : -1;
        } else {
            r = js_iter_step(J, &it, &x);
            if (r > 0 && is_await) x = co_await(J, x);
        }
        if (r < 0 || J->sig != JS_OK) return J->sig;
        if (r == 0) break;
        if (decl == VK_LET || decl == VK_CONST) {
            if (isc == sc || isc->escaped) isc = js_scope(J, sc);
            js_bind(J, target, x, isc, this_val, decl);
        } else {
            js_bind(J, target, x, sc, this_val, decl == VK_VAR ? VK_VAR : 0);
        }
        if (J->sig != JS_OK) { js_iter_close(J, &it); return J->sig; }
        jsignal s = js_exec(J, body, isc, this_val);
        if (js_loop_signal(J, s, mine, &out)) {
            js_iter_close(J, &it);
            return out;
        }
        if (!js_tick(J)) return J->sig;
    }
    return J->sig;
}

static JS_NOINLINE jsignal js_exec_for(jctx *J, int node, jscope *sc, jval this_val) {
    jstr *mine = J->pending_label;
    J->pending_label = 0;
    int init = J->nodes[node].a, test = J->nodes[node].b, step = J->nodes[node].c;
    int body = J->nodes[node].d;
    int scoped = J->nodes[node].flags & NF_SCOPE;
    jscope *ls = scoped ? js_scope(J, sc) : sc;
    if (init >= 0) {
        jsignal s = js_exec(J, init, ls, this_val);
        if (s != JS_OK) return s;
    }
    jsignal out = JS_OK;
    for (;;) {
        if (test >= 0) {
            jval t = js_eval(J, test, ls, this_val);
            if (J->sig != JS_OK) return J->sig;
            if (!js_to_bool(t)) break;
        }
        jsignal s = js_exec(J, body, ls, this_val);
        if (js_loop_signal(J, s, mine, &out)) {
            if (out != JS_OK || s == JS_BREAK) return out;
        }
        /* A closure made in this turn keeps this turn's variables; the next
           turn gets copies. */
        if (scoped && ls->escaped) ls = js_scope_copy(J, ls);
        if (step >= 0) {
            js_eval(J, step, ls, this_val);
            if (J->sig != JS_OK) return J->sig;
        }
        if (!js_tick(J)) return J->sig;
    }
    if (scoped && !ls->escaped && ls != sc) js_scope_free(J, ls);
    return J->sig;
}

static JS_NOINLINE jsignal js_exec_switch(jctx *J, int node, jscope *sc, jval this_val) {
    int a = J->nodes[node].a, first = J->nodes[node].b;
    jval subject = js_eval(J, a, sc, this_val);
    if (J->sig != JS_OK) return J->sig;
    jscope *ssc = sc;
    if (J->nodes[node].flags & NF_SCOPE) {
        ssc = js_scope(J, sc);
        for (int arm = first; arm >= 0; arm = J->nodes[arm].c)
            js_hoist_list(J, J->nodes[arm].b, ssc, ssc);
    }

    int started = 0;
    /* Two passes: the matching arm, then everything from there on,
       because falling through is the behaviour and not a bug. */
    for (int pass = 0; pass < 2 && !started; pass++) {
        for (int arm = first; arm >= 0; arm = J->nodes[arm].c) {
            if (!started) {
                int test = J->nodes[arm].a;
                if (pass == 0) {
                    if (test < 0) continue;      /* default, later */
                    jval v = js_eval(J, test, ssc, this_val);
                    if (J->sig != JS_OK) return J->sig;
                    if (!js_strict_eq(subject, v)) continue;
                } else {
                    if (test >= 0) continue;     /* only the default */
                }
                started = 1;
            }
            for (int cell = J->nodes[arm].b; cell >= 0; cell = J->nodes[cell].b) {
                jsignal s = js_exec(J, J->nodes[cell].a, ssc, this_val);
                /* A break with a label is for something outside the switch. */
                if (s == JS_BREAK && !J->label) { J->sig = JS_OK; return JS_OK; }
                if (s != JS_OK) return s;
            }
        }
    }
    return J->sig;
}

static JS_NOINLINE jsignal js_exec_try(jctx *J, int node, jscope *sc, jval this_val) {
    int block = J->nodes[node].a, handler = J->nodes[node].b, fin = J->nodes[node].c;
    int param = J->nodes[node].d;
    jsignal s = js_exec(J, block, sc, this_val);
    if (s == JS_THROWN && handler >= 0) {
        jval thrown = J->ret;
        J->sig = JS_OK;
        J->ret = js_undef();
        jscope *cs = js_scope(J, sc);
        if (param >= 0) js_bind(J, param, thrown, cs, this_val, VK_LET);
        s = J->sig == JS_OK ? js_exec(J, handler, cs, this_val) : J->sig;
        /* Given back like a call's, and for the same reason. */
        if (cs != sc && !cs->escaped) js_scope_free(J, cs);
    }
    if (fin >= 0 && s != JS_FAILED) {
        jsignal keep = s;
        jval kept = J->ret;
        jstr *label = J->label;
        J->sig = JS_OK;
        jsignal f = js_exec(J, fin, sc, this_val);
        /* A finally that itself leaves early wins; otherwise whatever the
           body or the catch decided stands. */
        if (f != JS_OK) return f;
        J->sig = keep;
        J->ret = kept;
        J->label = label;
        s = keep;
    }
    return s;
}

/* Text for the page's error line from a thrown value, without running any
   of the page's code to get it. */
static void js_note_thrown(jctx *J, jval v, int line) {
    char *e = J->error;
    int cap = (int)sizeof(J->error) - 1, i = 0;
    jstr *s = 0;
    if (v.t == JS_STR) s = v.str;
    else if (js_is_obj(v)) {
        jval nm = js_undef(), msg = js_undef();
        for (jobj *o = v.obj; o; o = o->proto) {
            jprop *p = js_find(o, J->s_name);
            if (p && p->v.t == JS_STR && nm.t == JS_UNDEF) nm = p->v;
            p = js_find(o, J->s_message);
            if (p && p->v.t == JS_STR && msg.t == JS_UNDEF) msg = p->v;
        }
        if (nm.t == JS_STR) {
            for (u32 k = 0; k < nm.str->len && i < cap; k++) e[i++] = nm.str->s[k];
            if (msg.t == JS_STR && msg.str->len && i + 2 < cap) { e[i++] = ':'; e[i++] = ' '; }
        }
        if (msg.t == JS_STR) s = msg.str;
        else if (nm.t != JS_STR) s = js_str(J, "an object was thrown");
    } else if (v.t != JS_SYM) s = js_to_str(J, v);
    for (u32 k = 0; s && k < s->len && i < cap; k++) e[i++] = s->s[k];
    e[i] = 0;
    J->error_line = line;
}

/* The statements that are not on the way into every call: out of the way of
   js_exec's own frame, which is on the stack once for every block and every
   return in every function being run. */
static JS_NOINLINE jsignal js_exec_misc(jctx *J, int node, jscope *sc, jval this_val) {
    int kind = J->nodes[node].kind;
    switch (kind) {
        case N_FUNCDECL:
            /* Already made when the block was entered. One in a block is
               also the function's var, as the old rule has it, from here. */
            if ((J->nodes[node].flags & NF_PAREN) && J->nodes[node].str) {
                jprop *p = js_find(sc->vars, J->nodes[node].str);
                if (p && sc->parent) js_assign_name(J, sc->parent, J->nodes[node].str, p->v);
            }
            return J->sig;

        case N_VAR: {
            int kind_d = J->nodes[node].d;
            for (int cell = J->nodes[node].a; cell >= 0; cell = J->nodes[cell].b) {
                int init = J->nodes[cell].a;
                int target = J->nodes[cell].c;
                jstr *name = J->nodes[cell].str;
                if (kind_d == VK_VAR && init < 0) continue;     /* declared already */
                jval v = init >= 0 ? js_eval(J, init, sc, this_val) : js_undef();
                if (J->sig != JS_OK) return J->sig;
                if (name) {
                    if (kind_d == VK_VAR) js_assign_name(J, sc, name, v);
                    else js_declare_flags(J, sc, name, v,
                                          kind_d == VK_CONST ? (JP_ENUM | JP_CONF) : JP_PLAIN);
                } else {
                    js_bind(J, target, v, sc, this_val, kind_d);
                }
                if (J->sig != JS_OK) return J->sig;
            }
            return JS_OK;
        }

        case N_CLASSDECL: {
            jval c = js_eval_class(J, node, sc, this_val);
            if (J->sig != JS_OK) return J->sig;
            if (J->nodes[node].str) js_declare(J, sc, J->nodes[node].str, c);
            return JS_OK;
        }

        case N_WHILE: {
            jstr *mine = J->pending_label;
            J->pending_label = 0;
            int a = J->nodes[node].a, b = J->nodes[node].b;
            jsignal out = JS_OK;
            for (;;) {
                jval t = js_eval(J, a, sc, this_val);
                if (J->sig != JS_OK) return J->sig;
                if (!js_to_bool(t)) break;
                jsignal s = js_exec(J, b, sc, this_val);
                if (js_loop_signal(J, s, mine, &out)) {
                    if (out != JS_OK || s == JS_BREAK) return out;
                }
                if (!js_tick(J)) return J->sig;
            }
            return J->sig;
        }

        case N_DO: {
            jstr *mine = J->pending_label;
            J->pending_label = 0;
            int a = J->nodes[node].a, b = J->nodes[node].b;
            jsignal out = JS_OK;
            for (;;) {
                jsignal s = js_exec(J, b, sc, this_val);
                if (js_loop_signal(J, s, mine, &out)) {
                    if (out != JS_OK || s == JS_BREAK) return out;
                }
                jval t = js_eval(J, a, sc, this_val);
                if (J->sig != JS_OK) return J->sig;
                if (!js_to_bool(t)) break;
                if (!js_tick(J)) return J->sig;
            }
            return J->sig;
        }

        /* The name is handed to the statement about to run, so that a loop
           can tell a break meant for it from one meant for something it is
           inside. A labelled thing that is not a loop -- a block, which is
           the other common one -- catches its own break here. */
        case N_LABEL: {
            int a = J->nodes[node].a;
            jstr *name = J->nodes[node].str;
            int k = a >= 0 ? J->nodes[a].kind : -1;
            J->pending_label = k == N_WHILE || k == N_DO || k == N_FOR || k == N_FORIN || k == N_FOROF
                             ? name : 0;
            jsignal s = js_exec(J, a, sc, this_val);
            J->pending_label = 0;
            if (s == JS_BREAK && J->label && js_str_eq(J->label, name)) {
                J->label = 0;
                J->sig = JS_OK;
                return JS_OK;
            }
            return s;
        }

        case N_THROW: {
            int line = J->nodes[node].line;
            jval v = js_eval(J, J->nodes[node].a, sc, this_val);
            if (J->sig != JS_OK) return J->sig;
            J->ret = v;
            J->sig = JS_THROWN;
            js_note_thrown(J, v, line);
            return JS_THROWN;
        }

        case N_IMPORTDECL:
            return J->sig;                   /* linked before the module ran */
        case N_EXPORTDECL: {
            int d = J->nodes[node].a, op = J->nodes[node].op;
            if (op == 0) return js_exec(J, d, sc, this_val);
            if (op != 1) return J->sig;
            /* export default: the binding *default* given its value, which a
               function declaration already has. */
            jval v = js_undef();
            if (d >= 0 && J->nodes[d].kind == N_FUNCDECL) return J->sig;
            if (d >= 0 && J->nodes[d].kind == N_CLASSDECL) {
                js_exec(J, d, sc, this_val);
                if (J->sig != JS_OK) return J->sig;
                jprop *cp = js_find(sc->vars, J->nodes[d].str);
                v = cp ? cp->v : js_undef();
            } else {
                v = js_eval(J, d, sc, this_val);
                if (J->sig != JS_OK) return J->sig;
            }
            jprop *p = js_find(sc->vars, js_intern(J, "*default*", 9));
            if (p) p->v = v;
            return J->sig;
        }

        case N_WITH: {
            /* A scope whose variables are the object's own properties. It is
               never given back: its table is the object's. */
            int a = J->nodes[node].a, b = J->nodes[node].b;
            jval o = js_eval(J, a, sc, this_val);
            if (J->sig != JS_OK) return J->sig;
            jobj *ob = js_to_object(J, o);
            if (!ob) return J->sig;
            jscope *ws = (jscope *)js_alloc(J, (u32)sizeof(jscope));
            if (!ws) return J->sig;
            ws->vars = ob;
            ws->parent = sc;
            ws->escaped = 1;
            ws->with = 1;
            J->with_used = 1;
            return js_exec(J, b, ws, this_val);
        }

        default:
            js_eval(J, node, sc, this_val);
            return J->sig;
    }
}

static JS_NOINLINE jsignal js_exec_scoped_block(jctx *J, int node, jscope *sc, jval this_val) {
    jscope *bsc = js_scope(J, sc);
    js_hoist_list(J, J->nodes[node].a, bsc, bsc);
    jsignal s = JS_OK;
    for (int cell = J->nodes[node].a; cell >= 0; cell = J->nodes[cell].b) {
        s = js_exec(J, J->nodes[cell].a, bsc, this_val);
        if (s != JS_OK) break;
    }
    if (bsc != sc && !bsc->escaped) js_scope_free(J, bsc);
    return s;
}

static jsignal js_exec(jctx *J, int node, jscope *sc, jval this_val) {
    if (node < 0) return J->sig;
    if (!js_tick(J)) return J->sig;
    if ((char *)&node < J->stack_limit) {
        js_throw(J, JS_ERR_RANGE, "too many nested calls", J->error_line);
        return J->sig;
    }
    /* The fields are read before anything below can run code: running code
       can read more text (eval), which grows the node array and moves it. */
    int kind = J->nodes[node].kind;
    J->error_line = J->nodes[node].line;

    switch (kind) {
        case N_BLOCK: {
            if (J->nodes[node].flags & NF_SCOPE) return js_exec_scoped_block(J, node, sc, this_val);
            for (int cell = J->nodes[node].a; cell >= 0; cell = J->nodes[cell].b) {
                jsignal s = js_exec(J, J->nodes[cell].a, sc, this_val);
                if (s != JS_OK) return s;
            }
            return JS_OK;
        }

        case N_EMPTY: return JS_OK;

        case N_EXPRSTMT: {
            jval v = js_eval(J, J->nodes[node].a, sc, this_val);
            if (J->sig == JS_OK) J->ret = v;       /* what eval() hands back */
            return J->sig;
        }

        case N_IF: {
            int b = J->nodes[node].b, c = J->nodes[node].c;
            jval t = js_eval(J, J->nodes[node].a, sc, this_val);
            if (J->sig != JS_OK) return J->sig;
            if (js_to_bool(t)) return js_exec(J, b, sc, this_val);
            if (c >= 0) return js_exec(J, c, sc, this_val);
            return J->sig;
        }

        case N_RETURN: {
            /* Into a local first: when the value throws, J->ret is what was
               thrown, and writing the failed evaluation's undefined over it
               made every error that passed through `return f()` arrive in
               the catch as undefined. */
            int a = J->nodes[node].a;
            jval v = a >= 0 ? js_eval(J, a, sc, this_val) : js_undef();
            if (J->sig != JS_OK) return J->sig;
            J->ret = v;
            J->sig = JS_RETURN;
            return JS_RETURN;
        }

        case N_BREAK:
            J->label = J->nodes[node].str;
            J->sig = JS_BREAK;
            return JS_BREAK;
        case N_CONTINUE:
            J->label = J->nodes[node].str;
            J->sig = JS_CONTINUE;
            return JS_CONTINUE;

        case N_FOR:    return js_exec_for(J, node, sc, this_val);
        case N_FORIN:  return js_exec_forin(J, node, sc, this_val);
        case N_FOROF:  return js_exec_forof(J, node, sc, this_val);
        case N_TRY:    return js_exec_try(J, node, sc, this_val);
        case N_SWITCH: return js_exec_switch(J, node, sc, this_val);

        default:
            return js_exec_misc(J, node, sc, this_val);
    }
}

/* --- eval ------------------------------------------------------------------
 *
 * eval(text), called by that name, runs the text where it is written, with
 * the variables round it; called any other way it runs at the top level. Its
 * value is the value of the last expression statement in it. Old pages use
 * it to read JSON, and a great many libraries find the global object with
 * Function('return this')(); both were refused here, by name, and the
 * scripts that used them stopped at the line. */
static jval js_eval_source(jctx *J, jstr *src, jscope *var_sc, jscope *sc, jval this_val) {
    int saved_line = J->error_line;
    jsignal before = J->sig;
    int prog = js_parse(J, src->s, src->len);
    if (prog < 0) {
        /* A parse failure in text handed to eval is the page's SyntaxError
           to catch, not the end of the page's script. */
        char why[sizeof(J->error)];
        for (u32 i = 0; i < sizeof(why); i++) why[i] = J->error[i];
        J->sig = before;
        J->error_line = saved_line;
        return js_throw(J, JS_ERR_SYNTAX, why, saved_line);
    }
    jscope *esc = js_scope(J, sc);
    js_hoist_body(J, prog, var_sc, esc);
    J->ret = js_undef();
    jsignal s = js_exec(J, prog, esc, this_val);
    jval r = J->ret;
    if (s == JS_OK || s == JS_RETURN) {
        J->sig = JS_OK;
        J->ret = js_undef();
        return r;
    }
    return js_undef();
}

static jval js_direct_eval(jctx *J, jval *argv, int argc, jscope *sc, jval this_val) {
    if (argc < 1) return js_undef();
    if (argv[0].t != JS_STR) return argv[0];
    /* The vars go to the function round the call, which is the nearest
       scope that is not a block's; the scope chain does not say which is
       which, so the scope the call is in stands in for it. */
    return js_eval_source(J, argv[0].str, sc, sc, this_val);
}

/* --- functions that stop half way -----------------------------------------
 *
 * (jsco.h says how.) A generator's body, or an async function's, runs on a
 * stack of its own; everything it calls is handed to the main stack; and a
 * yield or an await gives the stack back until the generator's next() or the
 * awaited promise's job picks it up again. */

static jval co_call_out(jctx *J, jval fn, jval this_val, jval *argv, int argc,
                        int construct, jval nt) {
    jco *co = J->co_current;
    co->c_fn = fn;
    co->c_this = this_val;
    co->c_argv = argv;
    co->c_argc = argc;
    co->c_construct = construct;
    co->c_newtarget = nt;
    co->c_result = js_undef();
    co_switch_out(co, CO_WHY_CALL);
    return co->c_result;
}

/* Runs it until it yields, awaits or ends, making on the main stack every
   call it asks for on the way. */
static void co_run(jctx *J, jco *co) {
    for (;;) {
        co_switch_in(J, co);
        if (co->why != CO_WHY_CALL) return;
        co->c_result = co->c_construct
            ? js_construct(J, co->c_fn, co->c_argv, co->c_argc, co->c_newtarget)
            : js_call(J, co->c_fn, co->c_this, co->c_argv, co->c_argc);
    }
}

static void co_entry(void *arg) {
    jco *co = (jco *)arg;
    jctx *J = co->J;
    if (co->is_module) {
        js_exec(J, co->prog, co->msc, js_undef());
        if (J->sig == JS_RETURN || J->sig == JS_BREAK || J->sig == JS_CONTINUE) J->sig = JS_OK;
        co->value = js_undef();
        co_switch_out(co, CO_WHY_END);
        return;
    }
    jval r = js_run_function(J, co->fn, co->this_val, co->args, co->argc, js_undef(), 0);
    co->value = r;
    co_switch_out(co, CO_WHY_END);
}

static jco *co_new(jctx *J, jobj *f, jval this_val, jval *argv, int argc, int kind) {
    jco *co = (jco *)js_alloc(J, (u32)sizeof(jco));
    if (!co) return 0;
    co->J = J;
    co->fn = f;
    co->this_val = this_val;
    co->kind = (u8)kind;
    co->state = CO_START;
    if (argc > 0) {
        co->args = (jval *)js_alloc(J, (u32)argc * (u32)sizeof(jval));
        if (!co->args) return 0;
        for (int i = 0; i < argc; i++) co->args[i] = argv[i];
    }
    co->argc = argc;
    return co;
}

static void agen_run(jctx *J, jco *co);
static void agen_after(jctx *J, jco *co);

/* What happens once an async function has given the stack back: nothing if
   it is waiting, and its promise settled if it has finished. */
static void co_after(jctx *J, jco *co) {
    if (co->kind == CO_ASYNCGEN) { agen_after(J, co); return; }
    if (co->kind != CO_ASYNC || co->why != CO_WHY_END) return;
    co_stack_free(J, co);
    if (J->sig == JS_THROWN) {
        jval e = J->ret;
        J->sig = JS_OK;
        J->ret = js_undef();
        js_promise_settle(J, co->promise, 0, e);
    } else if (J->sig == JS_OK) {
        js_promise_resolve_with(J, co->promise, co->value);
    }
}

static JS_NOINLINE jval js_start_coroutine(jctx *J, jobj *f, jval this_val, jval *argv, int argc) {
    int fl = J->nodes[f->node].op;
    if (fl & FN_GEN) {
        int kind = (fl & FN_ASYNC) ? CO_ASYNCGEN : CO_GEN;
        jobj *proto = js_proto_from(J, js_from_obj(f),
                                    kind == CO_GEN ? J->p_generator : J->p_async_generator);
        jobj *g = js_object_with(J, JO_GEN, proto);
        jco *co = co_new(J, f, this_val, argv, argc, kind);
        if (!g || !co) return js_undef();
        g->internal = co;
        g->spare = (u16)kind;
        return js_from_obj(g);
    }
    jobj *p = js_promise_new(J);
    jco *co = co_new(J, f, this_val, argv, argc, CO_ASYNC);
    if (!p || !co) return js_undef();
    co->promise = p;
    if (!co_stack_new(J, co)) return js_from_obj(p);
    co_run(J, co);
    co_after(J, co);
    return js_from_obj(p);
}

/* A module's body that awaits at its top level, run as an async function's
   body is, on a stack of its own: the promise it gives back settles when the
   body has finished, fulfilled, or rejected with what it threw. */
__attribute__((unused)) static jobj *js_run_module_async(jctx *J, int prog, jscope *msc) {
    jobj *p = js_promise_new(J);
    jco *co = co_new(J, 0, js_undef(), 0, 0, CO_ASYNC);
    if (!p || !co) return 0;
    co->is_module = 1;
    co->prog = prog;
    co->msc = msc;
    co->promise = p;
    if (J->nest == 0) {
        J->sig = JS_OK;
        J->steps = 0;
        J->error[0] = 0;
    }
    js_enter(J);
    if (co_stack_new(J, co)) {
        co_run(J, co);
        co_after(J, co);
    }
    js_leave(J);
    return p;
}

/* A value arriving in a function that was suspended, as the thing it asked
   for, or as a throw or a return from where it stopped. */
static jval co_resumed(jctx *J, jco *co) {
    jval v = co->value;
    if (co->mode == CO_THROW) {
        J->sig = JS_THROWN;
        J->ret = v;
        js_note_thrown(J, v, J->error_line);
        return js_undef();
    }
    if (co->mode == CO_RETURN) {
        if (co->kind == CO_ASYNCGEN) {
            v = co_await(J, v);
            if (J->sig != JS_OK) return js_undef();
        }
        J->ret = v;
        J->sig = JS_RETURN;
        return js_undef();
    }
    return v;
}

static jval co_await(jctx *J, jval v) {
    jco *co = J->co_current;
    if (!co || co->kind == CO_GEN)
        return js_throw(J, JS_ERR_SYNTAX, "await is only for an async function", J->error_line);
    jobj *p = js_promise_of(J, v);
    if (J->sig != JS_OK || !p) return js_undef();
    js_promise_await(J, p, co);
    co_switch_out(co, CO_WHY_AWAIT);
    jval r = co->value;
    if (co->mode == CO_THROW) {
        J->sig = JS_THROWN;
        J->ret = r;
        js_note_thrown(J, r, J->error_line);
        return js_undef();
    }
    return r;
}

static jval co_yield(jctx *J, jval v) {
    jco *co = J->co_current;
    if (!co || co->kind == CO_ASYNC)
        return js_throw(J, JS_ERR_SYNTAX, "yield is only for a generator", J->error_line);
    if (co->kind == CO_ASYNCGEN) {
        v = co_await(J, v);
        if (J->sig != JS_OK) return js_undef();
    }
    co->value = v;
    co_switch_out(co, CO_WHY_YIELD);
    return co_resumed(J, co);
}

/* yield*: every value of another iterator, with whatever the caller sends
   in -- a value, a throw, a return -- passed along to it. */
static jval co_yield_star(jctx *J, jval v) {
    jco *co = J->co_current;
    if (!co || co->kind == CO_ASYNC)
        return js_throw(J, JS_ERR_SYNTAX, "yield is only for a generator", J->error_line);
    jiter it;
    if (co->kind == CO_ASYNCGEN && js_is_obj(v)) {
        jval m = js_get(J, v, J->sym_async_iterator);
        if (J->sig != JS_OK) return js_undef();
        if (js_callable(m)) {
            jval iter = js_call(J, m, v, 0, 0);
            if (J->sig != JS_OK) return js_undef();
            it.kind = IT_GENERIC;
            it.obj = iter;
            it.next = js_get(J, iter, J->s_next);
        } else if (!js_iter_open(J, v, &it)) return js_undef();
    } else if (!js_iter_open(J, v, &it)) return js_undef();

    if (it.kind != IT_GENERIC) {
        for (;;) {
            jval x;
            int r = js_iter_step(J, &it, &x);
            if (r <= 0) return js_undef();
            co_yield(J, x);
            if (J->sig != JS_OK) return js_undef();
        }
    }
    jval received = js_undef();
    int mode = CO_NEXT;
    for (;;) {
        jval r;
        if (mode == CO_NEXT) {
            r = js_call(J, it.next, it.obj, &received, 1);
        } else {
            jval m = js_get(J, it.obj, mode == CO_THROW ? J->s_throw : J->s_return);
            if (J->sig != JS_OK) return js_undef();
            if (!js_callable(m)) {
                if (mode == CO_RETURN) {
                    J->ret = received;
                    J->sig = JS_RETURN;
                    return js_undef();
                }
                js_iter_close(J, &it);
                return js_throw(J, JS_ERR_TYPE, "the iterator yield* is walking has no throw",
                                J->error_line);
            }
            r = js_call(J, m, it.obj, &received, 1);
        }
        if (J->sig != JS_OK) return js_undef();
        if (co->kind == CO_ASYNCGEN) {
            r = co_await(J, r);
            if (J->sig != JS_OK) return js_undef();
        }
        if (!js_is_obj(r))
            return js_throw(J, JS_ERR_TYPE, "an iterator gave back something that is not an object",
                            J->error_line);
        jval done = js_get(J, r, J->s_done);
        if (J->sig != JS_OK) return js_undef();
        jval val = js_get(J, r, J->s_value);
        if (J->sig != JS_OK) return js_undef();
        if (js_to_bool(done)) {
            if (mode == CO_RETURN) {
                J->ret = val;
                J->sig = JS_RETURN;
                return js_undef();
            }
            return val;
        }
        co->value = val;
        co_switch_out(co, CO_WHY_YIELD);
        mode = co->mode;
        received = co->value;
    }
}

/* A generator's next, throw or return. */
static jval co_gen_resume(jctx *J, jobj *g, int mode, jval v) {
    jco *co = (jco *)g->internal;
    if (!co) return js_throw(J, JS_ERR_TYPE, "this is not a generator", J->error_line);
    if (co->state == CO_RUNNING)
        return js_throw(J, JS_ERR_TYPE, "a generator cannot be resumed while it is running",
                        J->error_line);
    if (co->state == CO_START && mode != CO_NEXT) co->state = CO_DONE;
    if (co->state == CO_DONE) {
        if (mode == CO_THROW) {
            J->sig = JS_THROWN;
            J->ret = v;
            js_note_thrown(J, v, J->error_line);
            return js_undef();
        }
        return js_iter_result(J, mode == CO_RETURN ? v : js_undef(), 1);
    }
    if (co->state == CO_START && !co_stack_new(J, co)) return js_undef();
    co->mode = (u8)mode;
    co->value = v;
    co_run(J, co);
    if (co->why == CO_WHY_YIELD) return js_iter_result(J, co->value, 0);
    co_stack_free(J, co);
    if (J->sig != JS_OK) return js_undef();
    return js_iter_result(J, co->value, 1);
}

/* An async generator's next, throw or return: a promise, and a request that
   is dealt with when the generator gets to it. */
static jval co_agen_request(jctx *J, jobj *g, int mode, jval v) {
    jco *co = (jco *)g->internal;
    jobj *p = js_promise_new(J);
    if (!p) return js_undef();
    if (!co) {
        js_promise_settle(J, p, 0, js_from_obj(js_error_with(J, J->err_proto[JS_ERR_TYPE],
                          js_str(J, "TypeError"), js_str(J, "this is not an async generator"))));
        return js_from_obj(p);
    }
    jareq *rq = (jareq *)js_alloc(J, (u32)sizeof(jareq));
    if (!rq) return js_undef();
    rq->mode = mode;
    rq->value = v;
    rq->promise = p;
    if (co->queue_tail) co->queue_tail->next = rq;
    else co->queue = rq;
    co->queue_tail = rq;
    agen_run(J, co);
    return js_from_obj(p);
}

static void agen_settle_head(jctx *J, jco *co) {
    jareq *rq = co->queue;
    if (!rq) return;
    co->queue = rq->next;
    if (!co->queue) co->queue_tail = 0;
    if (co->why == CO_WHY_YIELD) {
        js_promise_resolve_with(J, rq->promise, js_iter_result(J, co->value, 0));
        return;
    }
    co_stack_free(J, co);
    if (J->sig == JS_THROWN) {
        jval e = J->ret;
        J->sig = JS_OK;
        J->ret = js_undef();
        js_promise_settle(J, rq->promise, 0, e);
    } else if (J->sig == JS_OK) {
        js_promise_resolve_with(J, rq->promise, js_iter_result(J, co->value, 1));
    }
}

static void agen_run(jctx *J, jco *co) {
    while (co->queue && J->sig == JS_OK) {
        if (co->state == CO_RUNNING) return;
        if (co->state == CO_SUSPENDED && co->why == CO_WHY_AWAIT) return;
        jareq *rq = co->queue;
        if (co->state == CO_START && rq->mode != CO_NEXT) co->state = CO_DONE;
        if (co->state == CO_DONE) {
            co->queue = rq->next;
            if (!co->queue) co->queue_tail = 0;
            if (rq->mode == CO_THROW) js_promise_settle(J, rq->promise, 0, rq->value);
            else js_promise_resolve_with(J, rq->promise,
                     js_iter_result(J, rq->mode == CO_RETURN ? rq->value : js_undef(), 1));
            continue;
        }
        if (co->state == CO_START && !co_stack_new(J, co)) return;
        co->mode = (u8)rq->mode;
        co->value = rq->value;
        co_run(J, co);
        if (co->why == CO_WHY_AWAIT) return;
        agen_settle_head(J, co);
    }
}

static void agen_after(jctx *J, jco *co) {
    if (co->why == CO_WHY_AWAIT) return;
    agen_settle_head(J, co);
    agen_run(J, co);
}

/* The built-in objects. */
#include "jslib.h"

/* --- the way in ---------------------------------------------------------- */

static void js_init(jctx *J) {
    memset(J, 0, (int)sizeof(*J));
    J->sig = JS_OK;
    J->mem_cap = JS_MEM_CAP;
    js_names(J);
    J->global = js_scope(J, 0);
    if (!J->global) return;
    J->global->escaped = 1;                   /* never given back */
    J->global_obj = J->global->vars;
    J->global_lex = js_scope(J, J->global);
    J->global_lex->escaped = 1;
    js_globals(J);
    J->global_obj->proto = J->p_object;
}

static void js_done(jctx *J) {
    if (J->nodes) free(J->nodes);
    J->nodes = 0;
    J->nnodes = J->ncap = 0;
    if (J->jobs) free(J->jobs);
    J->jobs = 0;
    J->jcount = J->jcap = J->jhead = 0;
    if (J->intern) free(J->intern);
    J->intern = 0;
    J->nintern = J->intern_cap = 0;
    /* The stacks of whatever was still suspended when the page was left: a
       generator nobody finished, an async function waiting on a promise that
       never settled. They are not in the region. */
    while (J->co_all) co_stack_free(J, J->co_all);
    js_free_all(J);
}

/* Runs a script. Returns 1 when it finished, 0 when it did not, with the
   reason in J->error and the line in J->error_line either way. The jobs it
   left behind -- promise reactions, an async function's next step -- run
   before this returns, which is when a browser runs them. */
static int js_run(jctx *J, const char *src, u32 len) {
    J->sig = JS_OK;
    J->steps = 0;
    J->error[0] = 0;

    js_enter(J);
    int prog = js_parse(J, src, len);
    if (prog < 0 || J->sig == JS_FAILED) {
        J->sig = JS_FAILED;
        J->nest--;
        return 0;
    }

    /* At the top of a script `this` is the global object. It was undefined,
       so the wrapper nearly every library ships in -- (function(root){
       root.lib = ...; })(this) -- stopped at its first line with "cannot set
       lib of undefined". */
    js_hoist_body(J, prog, J->global, J->global_lex);
    if (J->sig == JS_OK) js_exec(J, prog, J->global_lex, js_from_obj(J->global_obj));
    if (J->sig == JS_RETURN || J->sig == JS_BREAK || J->sig == JS_CONTINUE) J->sig = JS_OK;
    js_leave(J);

    if (J->sig == JS_THROWN) return 0;
    if (J->sig == JS_FAILED) return 0;
    J->sig = JS_OK;
    return 1;
}

/* A module's body, read with js_parse_module and hoisted into its scope by
   the host, run there: `this` is undefined at a module's top level. Run from
   inside another script -- an import() -- it leaves the outer script's state
   as it was and says what happened by its answer. 1 when it finished. */
__attribute__((unused)) static int js_run_module(jctx *J, int prog, jscope *msc) {
    int outer = J->nest > 0;
    if (!outer) {
        J->sig = JS_OK;
        J->steps = 0;
        J->error[0] = 0;
    }
    js_enter(J);
    js_exec(J, prog, msc, js_undef());
    if (J->sig == JS_RETURN || J->sig == JS_BREAK || J->sig == JS_CONTINUE) J->sig = JS_OK;
    js_leave(J);
    if (J->sig == JS_THROWN || J->sig == JS_FAILED) return 0;
    J->sig = JS_OK;
    return 1;
}

/* And evaluates one expression, for anything that wants a value back: an
   event handler written in an attribute, or a test. */
__attribute__((unused)) static int js_eval_text(jctx *J, const char *src, u32 len, jval *out) {
    J->sig = JS_OK;
    J->steps = 0;
    J->error[0] = 0;

    js_enter(J);
    jparse P;
    jfnctx fc = { -1, -1, 0, 0, 0 };
    js_parse_begin(&P, J, src, len, &fc);
    int e = js_parse_expr(&P);
    if (!P.L.failed && P.L.tok.type != T_EOF && !js_at_punct(&P, ';'))
        js_parse_fail(&P, P.L.tok.line, "expected the end, not ", P.L.tok.text, P.L.tok.len);
    if (e < 0 || P.L.failed || J->sig == JS_FAILED) {
        J->sig = JS_FAILED;
        J->nest--;
        return 0;
    }
    jval v = js_eval(J, e, J->global_lex, js_from_obj(J->global_obj));
    js_leave(J);
    if (J->sig == JS_THROWN || J->sig == JS_FAILED) return 0;
    J->sig = JS_OK;
    if (out) *out = v;
    return 1;
}
