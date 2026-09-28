/* Running JavaScript: conversions, the evaluator, and the built-in methods.
 *
 * A tree walker. Each node is looked at and done; there is no bytecode and no
 * compilation step, which costs speed and buys the ability to read this file
 * and know what the machine does.
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

static jval js_eval(jctx *J, int node, jscope *sc, jval this_val);
static jsignal js_exec(jctx *J, int node, jscope *sc, jval this_val);

/* --- saying what went wrong ----------------------------------------------
 *
 * An error is an object with a name and a message, made by one of six
 * constructors, and `e instanceof TypeError` is how a page tells a mistake
 * it expected from one it did not. The engine threw strings, so e.message
 * was undefined, `instanceof Error` was false for everything it raised, and
 * there was no Error for a page to throw one of its own. */
enum { JS_ERR_ERROR, JS_ERR_TYPE, JS_ERR_RANGE, JS_ERR_REFERENCE, JS_ERR_SYNTAX, JS_ERR_EVAL };
static const char *const JS_ERR_NAMES[6] = {
    "Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "EvalError"
};

static jval nat_error_make(jctx *J, jval t, jval *a, int n);

static jobj *js_error_obj(jctx *J, jobj *ctor, jstr *name, jstr *message) {
    jobj *e = js_object(J, JO_PLAIN);
    if (!e) return 0;
    if (ctor) js_set_hidden(J, e, "__ctor__", js_from_obj(ctor));
    /* Kept out of a walk over its keys, where the standard keeps them. */
    js_set_hidden(J, e, "name", js_from_str(name));
    js_set_hidden(J, e, "message", js_from_str(message));
    return e;
}

/* Whether an object was made by one of the six. */
static int js_is_error(jctx *J, jobj *o) {
    jprop *p = o ? js_find(o, J->s_ctor) : 0;
    return p && p->v.t == JS_OBJ && p->v.obj && p->v.obj->kind == JO_NATIVE
        && p->v.obj->fn == nat_error_make;
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
    jobj *e = js_error_obj(J, J->err_ctor[kind], js_str(J, nm), js_str(J, what));
    J->ret = e ? js_from_obj(e) : js_from_str(js_str(J, what));
    return js_undef();
}

/* --- numbers as text -----------------------------------------------------
 *
 * The shortest decimal that reads back as the same double, which is what the
 * standard asks for: 0.1 + 0.2 is 0.30000000000000004, because that is the
 * number it is, and 0.3 is 0.3. The digits come from jsnum.h; this is only
 * where the point goes.
 *
 * It printed fifteen digits, rounded, which made 0.1 + 0.2 look like 0.3
 * to a script that was being told the two were different. Before that it
 * cut after ten places rather than rounding, and 0.57 came out as
 * 0.5699999999 on every page that showed a price.
 */
static u32 js_num_text(double d, char *out, u32 cap) {
    u32 w = 0;
    if (cap < 32) { if (cap) out[0] = 0; return 0; }

    /* Not a number and the infinities, which compare false against
       themselves and against everything else. */
    if (d != d) {
        const char *s = "NaN";
        while (*s) out[w++] = *s++;
        out[w] = 0;
        return w;
    }
    if ((d - d) != (d - d)) {                    /* infinite: inf - inf is NaN */
        const char *s = d < 0 ? "-Infinity" : "Infinity";
        while (*s) out[w++] = *s++;
        out[w] = 0;
        return w;
    }

    int neg = d < 0;
    if (neg) d = -d;

    /* Whole, and small enough that a double holds it exactly. */
    if (d < 9007199254740992.0 && d == (double)(long long)d) {
        long long v = (long long)d;
        char rev[24];
        int r = 0;
        if (!v) rev[r++] = '0';
        while (v) { rev[r++] = (char)('0' + (int)(v % 10)); v /= 10; }
        if (neg) out[w++] = '-';
        while (r) out[w++] = rev[--r];
        out[w] = 0;
        return w;
    }

    char dig[18];
    int e = 0;
    int n = js_shortest(d, dig, &e);

    if (neg) out[w++] = '-';

    /* Very large or very small, where a plain decimal would be mostly
       zeros: one digit, the rest after a point, and the power of ten. */
    if (e >= 21 || e < -6) {
        out[w++] = dig[0];
        if (n > 1) {
            out[w++] = '.';
            for (int i = 1; i < n; i++) out[w++] = dig[i];
        }
        out[w++] = 'e';
        if (e < 0) { out[w++] = '-'; e = -e; } else out[w++] = '+';
        char rev[8];
        int r = 0;
        if (!e) rev[r++] = '0';
        while (e) { rev[r++] = (char)('0' + e % 10); e /= 10; }
        while (r) out[w++] = rev[--r];
        out[w] = 0;
        return w;
    }

    if (e >= 0) {
        /* e + 1 digits before the point, padded with zeros if the number
           ran out of significant ones first. */
        for (int i = 0; i <= e; i++) out[w++] = i < n ? dig[i] : '0';
        if (n > e + 1) {
            out[w++] = '.';
            for (int i = e + 1; i < n; i++) out[w++] = dig[i];
        }
    } else {
        out[w++] = '0';
        out[w++] = '.';
        for (int i = 0; i < -e - 1; i++) out[w++] = '0';
        for (int i = 0; i < n; i++) out[w++] = dig[i];
    }
    out[w] = 0;
    return w;
}

/* --- conversions --------------------------------------------------------- */

static jstr *js_to_str(jctx *J, jval v);

static int js_to_bool(jval v) {
    switch (v.t) {
        case JS_UNDEF: case JS_NULL: return 0;
        case JS_BOOL: return v.b;
        /* NaN is false, and so is zero, and -0 is zero. */
        case JS_NUM:  return !(v.num == 0 || v.num != v.num);
        case JS_STR:  return v.str && v.str->len > 0;
        default: return 1;
    }
}

static int js_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

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
    if (i >= n) return 0;                       /* an empty string is zero */

    u32 k = 0;
    double v = js_signed_decimal(s + i, n - i, &k);
    i += k;
    while (i < n && js_blank(s[i])) i++;

    /* Trailing rubbish makes the whole thing not a number, which is the rule
       and is why "12px" is NaN while parseInt("12px") is 12. */
    if (!k || i != n) {
        double zero = 0.0;
        return zero / zero;
    }
    return v;
}

static double js_to_num(jctx *J, jval v) {
    switch (v.t) {
        case JS_NUM:  return v.num;
        case JS_BOOL: return v.b ? 1.0 : 0.0;
        case JS_NULL: return 0.0;
        case JS_STR:  return v.str ? js_str_to_num(v.str->s, v.str->len) : 0.0;
        case JS_OBJ: {
            /* An array of one number is that number, which is the rule that
               makes [5] * 2 come out as ten. Anything else is not a number. */
            if (v.obj && v.obj->kind == JO_ARRAY) {
                if (v.obj->len == 0) return 0.0;
                if (v.obj->len == 1) return js_to_num(J, v.obj->items[0]);
            }
            double zero = 0.0;
            return zero / zero;
        }
        default: {
            double zero = 0.0;
            return zero / zero;
        }
    }
}

/* The thirty-two bit truncation the bitwise operators use. Everything is a
   double until one of these appears, and then it is briefly an integer. */
static int js_to_i32(jctx *J, jval v) {
    double d = js_to_num(J, v);
    if (d != d || d > 1e18 || d < -1e18) return 0;
    long long t = (long long)d;
    return (int)(u32)(t & 0xFFFFFFFFll);
}

static u32 js_to_u32(jctx *J, jval v) { return (u32)js_to_i32(J, v); }

static jstr *js_concat(jctx *J, jstr *a, jstr *b);

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
        case JS_OBJ: {
            jobj *o = v.obj;
            if (!o) return js_str(J, "null");
            if (o->kind == JO_FUNC || o->kind == JO_NATIVE)
                return js_str(J, "function");
            if (js_is_error(J, o)) {
                /* "TypeError: message", or whichever half there is. */
                jstr *nm = js_to_str(J, js_get_prop(o, js_str(J, "name")));
                jstr *msg = js_to_str(J, js_get_prop(o, js_str(J, "message")));
                if (!msg || !msg->len) return nm;
                if (!nm || !nm->len) return msg;
                return js_concat(J, js_concat(J, nm, js_str(J, ": ")), msg);
            }
            if (o->kind == JO_ARRAY) {
                /* Joined with commas, which is what an array becomes when
                   something asks it for text.
                 *
                 * An array that holds itself, somewhere down, is written as
                 * nothing where it comes round again, which is what the
                 * language says; it recursed until the stack ran out, and
                 * the browser went with it. And each element is made into
                 * text once: it was twice, once to measure and once to copy,
                 * so each level of arrays in arrays doubled the work. */
                static jobj *joining[256];
                static int njoining;
                for (int k = 0; k < njoining; k++)
                    if (joining[k] == o) return js_str(J, "");
                if (njoining >= 256) return js_str(J, "");
                jstr **part = o->len ? (jstr **)js_alloc(J, o->len * (u32)sizeof(jstr *)) : 0;
                if (o->len && !part) return js_str(J, "");
                joining[njoining++] = o;
                u32 total = 0;
                for (u32 i = 0; i < o->len; i++) {
                    jval e = o->items[i];
                    part[i] = e.t == JS_UNDEF || e.t == JS_NULL ? 0 : js_to_str(J, e);
                    total += (part[i] ? part[i]->len : 0) + 1;
                }
                njoining--;
                jstr *out = (jstr *)js_alloc(J, (u32)sizeof(jstr) + total + 1);
                if (!out) return js_str(J, "");
                u32 w = 0;
                for (u32 i = 0; i < o->len; i++) {
                    if (i) out->s[w++] = ',';
                    for (u32 k = 0; part[i] && k < part[i]->len; k++) out->s[w++] = part[i]->s[k];
                }
                js_free(J, part, o->len * (u32)sizeof(jstr *));
                out->s[w] = 0;
                out->len = w;
                out->hash = js_hash(out->s, w);
                return out;
            }
            return js_str(J, "[object Object]");
        }
    }
    return js_str(J, "");
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

/* --- equality ------------------------------------------------------------
 *
 * Two of them, and the difference is the one thing every guide about this
 * language leads with. Strict compares kinds first and is what anybody
 * means; loose converts, and is here because pages use it. */
static int js_strict_eq(jval a, jval b) {
    if (a.t != b.t) return 0;
    switch (a.t) {
        case JS_UNDEF: case JS_NULL: return 1;
        case JS_BOOL: return a.b == b.b;
        case JS_NUM:  return a.num == b.num;      /* NaN fails, correctly */
        case JS_STR:  return js_str_eq(a.str, b.str);
        default:      return a.obj == b.obj;
    }
}

static int js_loose_eq(jctx *J, jval a, jval b) {
    if (a.t == b.t) return js_strict_eq(a, b);
    if ((a.t == JS_NULL && b.t == JS_UNDEF)
        || (a.t == JS_UNDEF && b.t == JS_NULL)) return 1;
    if (a.t == JS_NULL || a.t == JS_UNDEF
        || b.t == JS_NULL || b.t == JS_UNDEF) return 0;
    if (a.t == JS_OBJ || b.t == JS_OBJ) {
        jstr *sa = js_to_str(J, a), *sb = js_to_str(J, b);
        return js_str_eq(sa, sb);
    }
    double x = js_to_num(J, a), y = js_to_num(J, b);
    return x == y;
}

/* --- scopes -------------------------------------------------------------- */

static jscope *js_scope(jctx *J, jscope *parent) {
    jscope *s = (jscope *)js_alloc(J, (u32)sizeof(jscope));
    if (!s) return parent;
    s->vars = js_object(J, JO_PLAIN);
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
        js_free(J, v->buckets, (u32)sizeof(jprop *) * v->nbuckets);
        js_free(J, v, (u32)sizeof(jobj));
    }
    js_free(J, sc, (u32)sizeof(jscope));
}

static jprop *js_lookup(jscope *sc, const jstr *name) {
    for (jscope *s = sc; s; s = s->parent) {
        jprop *p = js_find(s->vars, name);
        if (p) return p;
    }
    return 0;
}

static void js_declare(jctx *J, jscope *sc, jstr *name, jval v) {
    js_set_prop(J, sc->vars, name, v);
}

/* Assigning to a name that was never declared puts it in the global scope,
   which is what this language does and what makes a typo into a new global
   rather than an error. */
static void js_assign_name(jctx *J, jscope *sc, jstr *name, jval v) {
    jprop *p = js_lookup(sc, name);
    if (p) { p->v = v; return; }
    js_set_prop(J, J->global->vars, name, v);
}

/* --- properties ---------------------------------------------------------- */

static jval js_length_of(jctx *J, jval target) {
    if (target.t == JS_STR) return js_num((double)(target.str ? target.str->len : 0));
    if (target.t == JS_OBJ && target.obj && target.obj->kind == JO_ARRAY)
        return js_num((double)target.obj->len);
    (void)J;
    return js_undef();
}

/* Turns a property name into an array index, or says it is not one. */
static int js_index_of(const jstr *key, u32 *out) {
    if (!key || !key->len || key->len > 10) return 0;
    u32 v = 0;
    for (u32 i = 0; i < key->len; i++) {
        if (key->s[i] < '0' || key->s[i] > '9') return 0;
        v = v * 10 + (u32)(key->s[i] - '0');
    }
    *out = v;
    return 1;
}

static jval js_string_method(jctx *J, jval target, jstr *name);
static jval js_array_method(jctx *J, jval target, jstr *name);
static jobj *js_native(jctx *J, const char *name, jnative fn);
static jval nat_fn_call(jctx *J, jval t, jval *a, int n);
static jval nat_fn_apply(jctx *J, jval t, jval *a, int n);
static jval nat_fn_bind(jctx *J, jval t, jval *a, int n);

/* Reaching into nothing.
 *
 * `a.b` where a is null or undefined is a mistake, and the language throws
 * rather than handing back undefined. Copying that is not about fidelity: a
 * page that reads a property of nothing has already gone wrong, and quietly
 * answering undefined lets it carry on and fail somewhere else, a long way
 * from the line that was wrong. The browser found this the hard way — a
 * script that named an element which was not on the page changed nothing,
 * reported nothing, and left a page that looked like a page whose script had
 * simply done nothing at all.
 */
static jval js_nothing(jctx *J, const char *verb, jstr *name, jval target) {
    char msg[96];
    int n = 0;
    for (const char *p = verb; *p && n < 40; p++) msg[n++] = *p;
    if (name)
        for (u32 i = 0; i < name->len && n < 70; i++) msg[n++] = name->s[i];
    const char *tail = target.t == JS_NULL ? " of null" : " of undefined";
    for (const char *p = tail; *p && n < 95; p++) msg[n++] = *p;
    msg[n] = 0;
    return js_throw(J, JS_ERR_TYPE, msg, J->error_line);
}

static jval js_get(jctx *J, jval target, jstr *name) {
    if (js_str_is(name, "length")) {
        jval l = js_length_of(J, target);
        if (l.t != JS_UNDEF) return l;
    }

    if (target.t == JS_STR) {
        u32 idx;
        if (js_index_of(name, &idx)) {
            if (!target.str || idx >= target.str->len) return js_undef();
            return js_from_str(js_str_n(J, target.str->s + idx, 1));
        }
        return js_string_method(J, target, name);
    }

    if (target.t == JS_NULL || target.t == JS_UNDEF)
        return js_nothing(J, "cannot read ", name, target);

    /* A number or a boolean is not nothing: it has no properties of its own
       and says so with undefined, which is what the language does. */
    if (target.t != JS_OBJ || !target.obj) {
        return js_undef();
    }

    jobj *o = target.obj;

    if (o->kind == JO_ARRAY) {
        u32 idx;
        if (js_index_of(name, &idx))
            return idx < o->len ? o->items[idx] : js_undef();
        jval m = js_array_method(J, target, name);
        if (m.t != JS_UNDEF) return m;
    }

    /* --- what you can do to a function --------------------------------
     *
     * call, apply and bind: three ways of saying which object a function
     * should treat as `this`. They are not decoration. A minified script
     * uses them constantly -- it is how anything written as a method gets
     * borrowed, and how every library shim on the web starts -- and a page
     * that calls one and gets "not a function" stops there, part way
     * through whatever it was setting up.
     *
     * Google's front page stops on `call`, which is how this was found:
     * the error used to say "this is not a function" and name nothing.
     */
    if (o->kind == JO_FUNC || o->kind == JO_NATIVE) {
        if (js_str_is(name, "call") || js_str_is(name, "apply")
            || js_str_is(name, "bind")) {
            jobj *m = js_native(J, name->s,
                                js_str_is(name, "call")  ? nat_fn_call :
                                js_str_is(name, "apply") ? nat_fn_apply
                                                         : nat_fn_bind);
            if (m) js_set_hidden(J, m, "__fn__", target);
            return js_from_obj(m);
        }
    }

    /* Anything the host owns gets asked before the property table, so a
       page reading element.textContent reaches the document rather than a
       stale copy of it. */
    if (o->host >= 0 && J->host_get) {
        char buf[64];
        u32 n = name->len < 63 ? name->len : 63;
        for (u32 i = 0; i < n; i++) buf[i] = name->s[i];
        buf[n] = 0;
        jval out;
        if (J->host_get(J, o, buf, &out)) return out;
    }

    return js_get_prop(o, name);
}

static void js_put(jctx *J, jval target, jstr *name, jval v) {
    if (target.t == JS_NULL || target.t == JS_UNDEF) {
        js_nothing(J, "cannot set ", name, target);
        return;
    }
    if (target.t != JS_OBJ || !target.obj) return;
    jobj *o = target.obj;

    if (o->kind == JO_ARRAY) {
        u32 idx;
        if (js_index_of(name, &idx)) { js_arr_set(J, o, idx, v); return; }
        if (js_str_is(name, "length")) {
            u32 want = (u32)js_to_num(J, v);
            if (want < o->len) o->len = want;
            else js_arr_reserve(J, o, want);
            return;
        }
    }

    if (o->host >= 0 && J->host_set) {
        char buf[64];
        u32 n = name->len < 63 ? name->len : 63;
        for (u32 i = 0; i < n; i++) buf[i] = name->s[i];
        buf[n] = 0;
        if (J->host_set(J, o, buf, v)) return;
    }

    js_set_prop(J, o, name, v);
}

/* --- calling ------------------------------------------------------------- */

#define JS_ARGS_MAX 24

static jval js_call(jctx *J, jval fn, jval this_val, jval *argv, int argc) {
    if (fn.t != JS_OBJ || !fn.obj
        || (fn.obj->kind != JO_FUNC && fn.obj->kind != JO_NATIVE))
        return js_throw(J, JS_ERR_TYPE, "this is not a function", J->error_line);

    if (J->depth >= JS_DEPTH_CAP)
        return js_throw(J, JS_ERR_RANGE, "too many nested calls", J->error_line);

    jobj *f = fn.obj;
    if (f->kind == JO_NATIVE) {
        J->callee = f;
        return f->fn(J, this_val, argv, argc);
    }

    /* An arrow function carries the receiver it was written under. */
    {
        jprop *lex = js_find(f, J->s_this);
        if (lex) this_val = lex->v;
    }

    jscope *sc = js_scope(J, f->closure);
    if (!sc) return js_undef();

    /* The parameters, in order, with anything not passed left undefined. */
    int p = f->params;
    int i = 0;
    while (p >= 0) {
        js_declare(J, sc, J->nodes[p].str, i < argc ? argv[i] : js_undef());
        p = J->nodes[p].b;
        i++;
    }

    /* `arguments`, as an array, because pages use it -- when this one does. */
    if (f->uses_args) {
        jobj *a = js_array(J);
        for (int k = 0; k < argc; k++) js_arr_push(J, a, argv[k]);
        js_declare(J, sc, J->s_arguments, js_from_obj(a));
    }

    J->depth++;
    jsignal s = js_exec(J, f->body, sc, this_val);
    J->depth--;

    /* Nothing made during the call can reach its scope, so the scope is
       given back for the next call to use. It never was, and a page that
       called a small function in a loop ran out of memory in about forty
       thousand calls. */
    if (!sc->escaped) js_scope_free(J, sc);

    if (s == JS_RETURN) {
        J->sig = JS_OK;
        jval r = J->ret;
        J->ret = js_undef();
        return r;
    }
    if (s == JS_THROWN || s == JS_FAILED) return js_undef();
    return js_undef();
}

/* --- what you can do to a function ----------------------------------------
 *
 * In `f.call(x, 1)` the function to run is `this`: the method was fetched
 * from f, so f is what the call hands over as the receiver, exactly as it
 * would for any other method. The object to treat as `this` inside f is the
 * first argument, and the rest are the arguments.
 *
 * All four of these used to look for the function on `this` under __fn__,
 * which is where js_get puts it on the wrapper -- and `this` is not the
 * wrapper, it is f, which has no __fn__. So every call, apply and bind threw
 * "this is not a function", on every page that used them, Google's front
 * page included. The wrapper's own __fn__ is still consulted, through
 * J->callee, when the method was taken off the function and called on its
 * own, which is lenient rather than standard.
 *
 * bind returns a fresh native holding the function, the receiver and any
 * arguments given after it, so calling it later runs the original with the
 * receiver it was bound to and those arguments in front of its own.
 */
static jval fn_held(jctx *J, jval self) {
    if (self.t != JS_OBJ || !self.obj) return js_undef();
    jprop *p = js_find(self.obj, J->s_fn);
    return p ? p->v : js_undef();
}

/* The function a call, apply or bind is about: `this`, when it is one. */
static jval fn_target(jctx *J, jval t, jobj *wrapper) {
    if (t.t == JS_OBJ && t.obj && (t.obj->kind == JO_FUNC || t.obj->kind == JO_NATIVE))
        return t;
    return wrapper ? fn_held(J, js_from_obj(wrapper)) : js_undef();
}

static jval nat_fn_call(jctx *J, jval t, jval *a, int n) {
    jval fn = fn_target(J, t, J->callee);
    jval who = n > 0 ? a[0] : js_undef();
    jval rest[JS_ARGS_MAX];
    int m = 0;
    for (int i = 1; i < n && m < JS_ARGS_MAX; i++) rest[m++] = a[i];
    return js_call(J, fn, who, rest, m);
}

static jval nat_fn_apply(jctx *J, jval t, jval *a, int n) {
    jval fn = fn_target(J, t, J->callee);
    jval who = n > 0 ? a[0] : js_undef();

    /* The second argument is an array of them, which is the whole
       difference between apply and call. Anything else is no arguments,
       the way it is everywhere. */
    jval rest[JS_ARGS_MAX];
    int m = 0;
    if (n > 1 && a[1].t == JS_OBJ && a[1].obj && a[1].obj->kind == JO_ARRAY) {
        jobj *arr = a[1].obj;
        for (u32 i = 0; i < arr->len && m < JS_ARGS_MAX; i++)
            rest[m++] = arr->items[i];
    }
    return js_call(J, fn, who, rest, m);
}

/* A bound function being called: what it holds is on itself, which only
   J->callee can say -- `this` here is whatever the caller supplied, and is
   exactly what a bound function ignores. */
static jval nat_fn_bound(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *self = J->callee;
    if (!self) return js_throw(J, JS_ERR_TYPE, "this is not a function", J->error_line);
    jval fn = fn_held(J, js_from_obj(self));
    jprop *p = js_find(self, J->s_bound);
    jval who = p ? p->v : js_undef();

    /* The arguments given to bind, then the ones given now. */
    jval all[JS_ARGS_MAX];
    int m = 0;
    jprop *pre = js_find(self, J->s_args);
    if (pre && pre->v.t == JS_OBJ && pre->v.obj && pre->v.obj->kind == JO_ARRAY) {
        jobj *arr = pre->v.obj;
        for (u32 i = 0; i < arr->len && m < JS_ARGS_MAX; i++) all[m++] = arr->items[i];
    }
    for (int i = 0; i < n && m < JS_ARGS_MAX; i++) all[m++] = a[i];
    return js_call(J, fn, who, all, m);
}

static jval nat_fn_bind(jctx *J, jval t, jval *a, int n) {
    jval fn = fn_target(J, t, J->callee);
    if (fn.t != JS_OBJ) return js_throw(J, JS_ERR_TYPE, "bind needs a function", J->error_line);
    jobj *out = js_native(J, "bound", nat_fn_bound);
    if (!out) return js_undef();
    js_set_hidden(J, out, "__fn__", fn);
    js_set_hidden(J, out, "__bound__", n > 0 ? a[0] : js_undef());
    if (n > 1) {
        jobj *pre = js_array(J);
        if (pre) {
            for (int i = 1; i < n; i++) js_arr_push(J, pre, a[i]);
            js_set_hidden(J, out, "__args__", js_from_obj(pre));
        }
    }
    return js_from_obj(out);
}

/* --- the built-in methods -------------------------------------------------
 *
 * Written as natives rather than in JavaScript. A bootstrap written in the
 * language itself is elegant and it means every one of these costs a parse
 * and a tree walk on every page load, for methods that are a loop each.
 */

static jobj *js_native(jctx *J, const char *name, jnative fn) {
    jobj *o = js_object(J, JO_NATIVE);
    if (!o) return 0;
    o->fn = fn;
    o->name = js_str(J, name);
    return o;
}

static jval js_arg(jval *argv, int argc, int i) {
    return i < argc ? argv[i] : js_undef();
}

/* --- regular expressions --------------------------------------------------
 *
 * A pattern object keeps its source and its flags as ordinary properties,
 * which is what a script expects to be able to read, and is compiled fresh
 * for each call. Compiling is a walk over a string that is almost always
 * under thirty characters; keeping a compiled copy would mean somewhere to
 * put it on an object that has no room for one, and a page that builds a
 * pattern in a loop would leak them.
 *
 * One scratch engine, because nothing here matches two patterns at once:
 * every use below compiles, matches and is finished before the next.
 */
static rx js_rx;

static int js_is_regex(jval v) {
    return v.t == JS_OBJ && v.obj && v.obj->kind == JO_REGEX;
}

/* Loads the pattern on an object into the scratch engine. Returns 0 and
   leaves a thrown error when the pattern is one this cannot read, because a
   page told its pattern is not understood can say so, and a page quietly
   matching nothing cannot. */
static int js_rx_load(jctx *J, jval v, int line) {
    if (!js_is_regex(v)) return 0;
    jval src = js_get_prop(v.obj, js_str(J, "source"));
    jval flg = js_get_prop(v.obj, js_str(J, "flags"));
    jstr *ss = src.t == JS_STR ? src.str : 0;
    jstr *sf = flg.t == JS_STR ? flg.str : 0;
    if (!ss) return 0;

    char flags[8];
    u32 i = 0;
    for (; sf && i < sf->len && i < sizeof(flags) - 1; i++) flags[i] = sf->s[i];
    flags[i] = 0;

    if (!rx_compile(&js_rx, ss->s, (int)ss->len, flags)) {
        js_throw(J, JS_ERR_SYNTAX, js_rx.why[0] ? js_rx.why : "a pattern this cannot read",
                 line);
        return 0;
    }
    return 1;
}

static jobj *js_regex_new(jctx *J, const char *pat, u32 len, int flags);

/* The array exec and match hand back: the whole match at nought, then each
   group, with where it was found and what it was found in. */
static jval js_rx_result(jctx *J, jstr *s) {
    jobj *out = js_array(J);
    if (!out) return js_null();
    for (int i = 0; i < js_rx.ncaps; i++) {
        if (js_rx.cap_start[i] < 0 || js_rx.cap_end[i] < js_rx.cap_start[i])
            js_arr_set(J, out, (u32)i, js_undef());
        else
            js_arr_set(J, out, (u32)i,
                       js_from_str(js_str_n(J, s->s + js_rx.cap_start[i],
                                            (u32)(js_rx.cap_end[i]
                                                  - js_rx.cap_start[i]))));
    }
    js_set(J, out, "index", js_num((double)js_rx.cap_start[0]));
    js_set(J, out, "input", js_from_str(s));
    return js_from_obj(out);
}

static jval nat_re_test(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s || !js_rx_load(J, t, J->error_line)) return js_bool(0);
    return js_bool(rx_search(&js_rx, s->s, (int)s->len, 0) >= 0);
}

/* exec walks a global pattern through its subject one call at a time, which
   is what lastIndex is for and the only reason a page calls it in a loop. */
static jval nat_re_exec(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s || !js_rx_load(J, t, J->error_line)) return js_null();

    int from = 0;
    if (js_rx.global) {
        jval li = js_get_prop(t.obj, js_str(J, "lastIndex"));
        from = li.t == JS_NUM ? (int)li.num : 0;
        if (from < 0 || from > (int)s->len) {
            js_set(J, t.obj, "lastIndex", js_num(0));
            return js_null();
        }
    }

    if (rx_search(&js_rx, s->s, (int)s->len, from) < 0) {
        if (js_rx.global) js_set(J, t.obj, "lastIndex", js_num(0));
        return js_null();
    }
    if (js_rx.global) {
        /* An empty match would otherwise stand still for ever. */
        int next = js_rx.cap_end[0];
        if (next == js_rx.cap_start[0]) next++;
        js_set(J, t.obj, "lastIndex", js_num((double)next));
    }
    return js_rx_result(J, s);
}

static jobj *js_regex_new(jctx *J, const char *pat, u32 len, int flags) {
    jobj *o = js_object(J, JO_REGEX);
    if (!o) return 0;

    char f[4];
    int w = 0;
    if (flags & RXF_G) f[w++] = 'g';
    if (flags & RXF_I) f[w++] = 'i';
    if (flags & RXF_M) f[w++] = 'm';
    f[w] = 0;

    js_set(J, o, "source", js_from_str(js_str_n(J, pat, len)));
    js_set(J, o, "flags", js_from_str(js_str(J, f)));
    js_set(J, o, "global", js_bool((flags & RXF_G) != 0));
    js_set(J, o, "ignoreCase", js_bool((flags & RXF_I) != 0));
    js_set(J, o, "multiline", js_bool((flags & RXF_M) != 0));
    js_set(J, o, "lastIndex", js_num(0));
    js_set(J, o, "test", js_from_obj(js_native(J, "test", nat_re_test)));
    js_set(J, o, "exec", js_from_obj(js_native(J, "exec", nat_re_exec)));
    return o;
}

/* RegExp("a.b", "i"), for a pattern that is not known until it is built.
   With or without `new`: both are written, and an engine that takes only
   one of them refuses half the pages that use it. */
static jval nat_regexp_make(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval first = js_arg(a, n, 0);
    jstr *pat = js_is_regex(first)
              ? js_get_prop(first.obj, js_str(J, "source")).str
              : js_to_str(J, first);
    jstr *flg = n > 1 ? js_to_str(J, js_arg(a, n, 1)) : 0;

    int flags = 0;
    for (u32 i = 0; flg && i < flg->len; i++) {
        if (flg->s[i] == 'g') flags |= RXF_G;
        else if (flg->s[i] == 'i') flags |= RXF_I;
        else if (flg->s[i] == 'm') flags |= RXF_M;
    }
    if (!pat) return js_null();
    jobj *o = js_regex_new(J, pat->s, pat->len, flags);
    return o ? js_from_obj(o) : js_null();
}

static jval nat_str_charat(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    int i = (int)js_to_num(J, js_arg(a, n, 0));
    if (!s || i < 0 || (u32)i >= s->len) return js_from_str(js_str(J, ""));
    return js_from_str(js_str_n(J, s->s + i, 1));
}

static jval nat_str_charcode(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    int i = (int)js_to_num(J, js_arg(a, n, 0));
    if (!s || i < 0 || (u32)i >= s->len) {
        double z = 0.0;
        return js_num(z / z);
    }
    return js_num((double)(u8)s->s[i]);
}

static int js_find_sub(const jstr *h, const jstr *n, u32 from) {
    if (!h || !n) return -1;
    if (n->len == 0) return (int)from;
    if (n->len > h->len) return -1;
    for (u32 i = from; i + n->len <= h->len; i++) {
        u32 k = 0;
        while (k < n->len && h->s[i + k] == n->s[k]) k++;
        if (k == n->len) return (int)i;
    }
    return -1;
}

/* A position in a string, as the methods that take one read it: missing is
   `dflt`, not a number is 0, and anything else is held inside the string.
   Every one of them used to ignore it, so indexOf(x, from) found the first x
   wherever from said to start, and a loop walking a string by its matches
   went round the first one for ever. */
static int js_str_pos(jctx *J, jval *a, int n, int i, int len, int dflt) {
    if (i >= n || a[i].t == JS_UNDEF) return dflt;
    double d = js_to_num(J, a[i]);
    if (d != d || d < 0) return 0;
    if (d > (double)len) return len;
    return (int)d;
}

static jval nat_str_indexof(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_to_str(J, t), *nd = js_to_str(J, js_arg(a, n, 0));
    if (!h) return js_num(-1);
    return js_num((double)js_find_sub(h, nd, (u32)js_str_pos(J, a, n, 1, (int)h->len, 0)));
}

static jval nat_str_lastindexof(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_to_str(J, t), *nd = js_to_str(J, js_arg(a, n, 0));
    if (!h || !nd || nd->len > h->len) return js_num(-1);
    int from = js_str_pos(J, a, n, 1, (int)h->len, (int)h->len);
    if (from > (int)(h->len - nd->len)) from = (int)(h->len - nd->len);
    for (int i = from; i >= 0; i--) {
        u32 k = 0;
        while (k < nd->len && h->s[i + k] == nd->s[k]) k++;
        if (k == nd->len) return js_num((double)i);
    }
    return js_num(-1);
}

static jval nat_str_includes(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_to_str(J, t), *nd = js_to_str(J, js_arg(a, n, 0));
    if (!h) return js_bool(0);
    return js_bool(js_find_sub(h, nd, (u32)js_str_pos(J, a, n, 1, (int)h->len, 0)) >= 0);
}

static jval nat_str_startswith(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_to_str(J, t), *nd = js_to_str(J, js_arg(a, n, 0));
    if (!h || !nd) return js_bool(0);
    u32 at = (u32)js_str_pos(J, a, n, 1, (int)h->len, 0);
    if (nd->len > h->len - at) return js_bool(0);
    for (u32 i = 0; i < nd->len; i++) if (h->s[at + i] != nd->s[i]) return js_bool(0);
    return js_bool(1);
}

/* Ends with, where the end is the second argument when there is one. */
static jval nat_str_endswith(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_to_str(J, t), *nd = js_to_str(J, js_arg(a, n, 0));
    if (!h || !nd) return js_bool(0);
    u32 end = (u32)js_str_pos(J, a, n, 1, (int)h->len, (int)h->len);
    if (nd->len > end) return js_bool(0);
    for (u32 i = 0; i < nd->len; i++)
        if (h->s[end - nd->len + i] != nd->s[i]) return js_bool(0);
    return js_bool(1);
}

static jval nat_str_slice(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    if (!s) return js_from_str(js_str(J, ""));
    int len = (int)s->len;
    int from = n > 0 ? (int)js_to_num(J, a[0]) : 0;
    int to = n > 1 && a[1].t != JS_UNDEF ? (int)js_to_num(J, a[1]) : len;
    if (from < 0) from += len;
    if (to < 0) to += len;
    if (from < 0) from = 0;
    if (to > len) to = len;
    if (to <= from) return js_from_str(js_str(J, ""));
    return js_from_str(js_str_n(J, s->s + from, (u32)(to - from)));
}

/* Not slice. substring clamps a negative to nothing rather than counting it
   from the end, and takes its two ends in either order; substr is a start
   and a length. All three were slice, so "hello".substring(3, 1) was empty
   and substr(1, 3) was two letters. */
static jval nat_str_substring(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    if (!s) return js_from_str(js_str(J, ""));
    int len = (int)s->len;
    int from = js_str_pos(J, a, n, 0, len, 0);
    int to = js_str_pos(J, a, n, 1, len, len);
    if (from > to) { int k = from; from = to; to = k; }
    return js_from_str(js_str_n(J, s->s + from, (u32)(to - from)));
}

static jval nat_str_substr(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    if (!s) return js_from_str(js_str(J, ""));
    int len = (int)s->len;
    double d = n > 0 ? js_to_num(J, a[0]) : 0;
    int from = d != d ? 0 : (d < 0 ? (d < -len ? 0 : len + (int)d) : (d > len ? len : (int)d));
    int want = len - from;
    if (n > 1 && a[1].t != JS_UNDEF) {
        double w = js_to_num(J, a[1]);
        want = w != w || w < 0 ? 0 : (w > want ? want : (int)w);
    }
    return js_from_str(js_str_n(J, s->s + from, (u32)want));
}

/* One character, counted from the end when the index is negative. */
static jval nat_str_at(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    double d = n > 0 ? js_to_num(J, a[0]) : 0;
    if (!s || d != d) d = 0;
    int i = (int)d;
    if (i < 0) i += s ? (int)s->len : 0;
    if (!s || i < 0 || i >= (int)s->len) return js_undef();
    return js_from_str(js_str_n(J, s->s + i, 1));
}

static jval nat_str_concat(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    for (int i = 0; i < n && s; i++) s = js_concat(J, s, js_to_str(J, a[i]));
    return js_from_str(s ? s : js_str(J, ""));
}

/* Padded to a length with a filler, at the front or the back. */
static jval js_str_pad(jctx *J, jval t, jval *a, int n, int front) {
    jstr *s = js_to_str(J, t);
    if (!s) return js_from_str(js_str(J, ""));
    double w = n > 0 ? js_to_num(J, a[0]) : 0;
    jstr *fill = n > 1 && a[1].t != JS_UNDEF ? js_to_str(J, a[1]) : js_str(J, " ");
    if (w != w || w <= (double)s->len || !fill || !fill->len || w > 65536) return js_from_str(s);
    u32 need = (u32)w - s->len;
    char *buf = (char *)js_alloc(J, need + 1);
    if (!buf) return js_from_str(s);
    for (u32 i = 0; i < need; i++) buf[i] = fill->s[i % fill->len];
    jstr *pad = js_str_n(J, buf, need);
    return js_from_str(front ? js_concat(J, pad, s) : js_concat(J, s, pad));
}

static jval nat_str_padstart(jctx *J, jval t, jval *a, int n) { return js_str_pad(J, t, a, n, 1); }
static jval nat_str_padend(jctx *J, jval t, jval *a, int n) { return js_str_pad(J, t, a, n, 0); }

static jval nat_str_upper(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *s = js_to_str(J, t);
    if (!s) return js_from_str(js_str(J, ""));
    jstr *o = js_str_n(J, s->s, s->len);
    for (u32 i = 0; o && i < o->len; i++)
        if (o->s[i] >= 'a' && o->s[i] <= 'z') o->s[i] = (char)(o->s[i] - 32);
    if (o) o->hash = js_hash(o->s, o->len);
    return js_from_str(o);
}

static jval nat_str_lower(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *s = js_to_str(J, t);
    if (!s) return js_from_str(js_str(J, ""));
    jstr *o = js_str_n(J, s->s, s->len);
    for (u32 i = 0; o && i < o->len; i++)
        if (o->s[i] >= 'A' && o->s[i] <= 'Z') o->s[i] = (char)(o->s[i] + 32);
    if (o) o->hash = js_hash(o->s, o->len);
    return js_from_str(o);
}

static int js_str_blank(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* Blanks off both ends, or one: `which` is 1 for the front, 2 the back. */
static jval js_str_trim_ends(jctx *J, jval t, int which) {
    jstr *s = js_to_str(J, t);
    if (!s) return js_from_str(js_str(J, ""));
    u32 from = 0, to = s->len;
    if (which & 1) while (from < to && js_str_blank(s->s[from])) from++;
    if (which & 2) while (to > from && js_str_blank(s->s[to - 1])) to--;
    return js_from_str(js_str_n(J, s->s + from, to - from));
}

static jval nat_str_trim(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    return js_str_trim_ends(J, t, 3);
}

static jval nat_str_trimstart(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    return js_str_trim_ends(J, t, 1);
}

static jval nat_str_trimend(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    return js_str_trim_ends(J, t, 2);
}

static jval nat_str_split(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    jobj *out = js_array(J);
    if (!s || !out) return js_from_obj(out);

    if (n == 0 || a[0].t == JS_UNDEF) {
        js_arr_push(J, out, js_from_str(s));
        return js_from_obj(out);
    }
    if (js_is_regex(a[0])) {
        if (!js_rx_load(J, a[0], J->error_line)) return js_from_obj(out);
        int from = 0, at = 0;
        while (from <= (int)s->len) {
            if (rx_search(&js_rx, s->s, (int)s->len, from) < 0) break;
            if (js_rx.cap_end[0] == js_rx.cap_start[0]) {
                /* A pattern that matches nothing splits between every
                   character rather than standing still. */
                if (js_rx.cap_start[0] >= (int)s->len) break;
                from = js_rx.cap_start[0] + 1;
                js_arr_push(J, out,
                            js_from_str(js_str_n(J, s->s + at,
                                                 (u32)(from - at))));
                at = from;
                continue;
            }
            js_arr_push(J, out,
                        js_from_str(js_str_n(J, s->s + at,
                                             (u32)(js_rx.cap_start[0] - at))));
            at = from = js_rx.cap_end[0];
        }
        js_arr_push(J, out, js_from_str(js_str_n(J, s->s + at,
                                                 s->len - (u32)at)));
        return js_from_obj(out);
    }

    jstr *sep = js_to_str(J, a[0]);
    if (sep && sep->len == 0) {
        for (u32 i = 0; i < s->len; i++)
            js_arr_push(J, out, js_from_str(js_str_n(J, s->s + i, 1)));
        return js_from_obj(out);
    }

    u32 at = 0;
    for (;;) {
        int hit = js_find_sub(s, sep, at);
        if (hit < 0) break;
        js_arr_push(J, out, js_from_str(js_str_n(J, s->s + at, (u32)hit - at)));
        at = (u32)hit + sep->len;
    }
    js_arr_push(J, out, js_from_str(js_str_n(J, s->s + at, s->len - at)));
    return js_from_obj(out);
}

/* Building the replacement, with $1 and friends standing for what the
   groups caught. A page writing $1 and getting the two characters back is
   the commonest way a rewrite silently produces nonsense. */
static void js_rx_expand(jctx *J, jstr *with, jstr *s, char *out, int cap,
                         int *w) {
    for (u32 i = 0; with && i < with->len && *w < cap - 1; i++) {
        if (with->s[i] == '$' && i + 1 < with->len) {
            char d = with->s[i + 1];
            if (d == '$') { out[(*w)++] = '$'; i++; continue; }
            if (d == '&') {
                for (int k = js_rx.cap_start[0];
                     k < js_rx.cap_end[0] && *w < cap - 1; k++)
                    out[(*w)++] = s->s[k];
                i++;
                continue;
            }
            if (d >= '0' && d <= '9') {
                int g = d - '0';
                i++;
                if (i + 1 < with->len && with->s[i + 1] >= '0'
                    && with->s[i + 1] <= '9'
                    && (g * 10 + (with->s[i + 1] - '0')) < js_rx.ncaps) {
                    g = g * 10 + (with->s[i + 1] - '0');
                    i++;
                }
                if (g > 0 && g < js_rx.ncaps && js_rx.cap_start[g] >= 0)
                    for (int k = js_rx.cap_start[g];
                         k < js_rx.cap_end[g] && *w < cap - 1; k++)
                        out[(*w)++] = s->s[k];
                continue;
            }
        }
        out[(*w)++] = with->s[i];
    }
    (void)J;
}

static jval nat_str_replace_re(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    jval re = js_arg(a, n, 0);
    jval rep = js_arg(a, n, 1);
    if (!s || !js_rx_load(J, re, J->error_line)) return js_from_str(s);

    int every = js_rx.global;
    static char out[16384];
    int w = 0, from = 0;

    for (;;) {
        if (rx_search(&js_rx, s->s, (int)s->len, from) < 0) break;

        for (int k = from; k < js_rx.cap_start[0] && w < (int)sizeof(out) - 1; k++)
            out[w++] = s->s[k];

        if (rep.t == JS_OBJ && rep.obj
            && (rep.obj->kind == JO_FUNC || rep.obj->kind == JO_NATIVE)) {
            /* A function is handed the match and its groups, the way it is
               everywhere else, and what it returns goes in. */
            jval args[RX_CAPS + 2];
            int argc = 0;
            for (int g = 0; g < js_rx.ncaps && argc < RX_CAPS; g++)
                args[argc++] = js_rx.cap_start[g] < 0 ? js_undef()
                    : js_from_str(js_str_n(J, s->s + js_rx.cap_start[g],
                                           (u32)(js_rx.cap_end[g]
                                                 - js_rx.cap_start[g])));
            args[argc++] = js_num((double)js_rx.cap_start[0]);
            args[argc++] = js_from_str(s);

            int start = js_rx.cap_start[0], end = js_rx.cap_end[0];
            jval got = js_call(J, rep, js_undef(), args, argc);
            if (J->sig != JS_OK) return js_from_str(s);
            jstr *gs = js_to_str(J, got);
            for (u32 k = 0; gs && k < gs->len && w < (int)sizeof(out) - 1; k++)
                out[w++] = gs->s[k];
            /* The engine is scratch and the call may have used it. */
            if (!js_rx_load(J, re, J->error_line)) return js_from_str(s);
            js_rx.cap_start[0] = start;
            js_rx.cap_end[0] = end;
        } else {
            jstr *with = js_to_str(J, rep);
            js_rx_expand(J, with, s, out, (int)sizeof(out), &w);
        }

        int next = js_rx.cap_end[0];
        if (next == js_rx.cap_start[0]) {
            if (next < (int)s->len && w < (int)sizeof(out) - 1)
                out[w++] = s->s[next];
            next++;
        }
        from = next;
        if (!every || from > (int)s->len) break;
    }

    for (int k = from; k < (int)s->len && w < (int)sizeof(out) - 1; k++)
        out[w++] = s->s[k];
    out[w] = 0;
    return js_from_str(js_str_n(J, out, (u32)w));
}

/* A string pattern, replaced once or everywhere. The replacement is what it
   is with a regular expression: a function is called with the match, where
   it was and the whole string, and in text $& is the match, $` and $' what
   came before and after it, and $$ a dollar. Written as the text
   "function" and with the dollars left in, a page's rewrite came out as
   nonsense; and replaceAll was replace, so it replaced the first one. */
static jval js_str_replace_text(jctx *J, jval t, jval *a, int n, int every) {
    jstr *s = js_to_str(J, t);
    jstr *find = js_to_str(J, js_arg(a, n, 0));
    jval rep = js_arg(a, n, 1);
    int call = rep.t == JS_OBJ && rep.obj
             && (rep.obj->kind == JO_FUNC || rep.obj->kind == JO_NATIVE);
    jstr *with = call ? 0 : js_to_str(J, rep);
    if (!s || !find) return js_from_str(s);

    jstr *out = js_str(J, "");
    u32 at = 0;
    for (;;) {
        int hit = js_find_sub(s, find, at);
        if (hit < 0 || !out) break;
        out = js_concat(J, out, js_str_n(J, s->s + at, (u32)hit - at));
        if (call) {
            jval args[3] = { js_from_str(js_str_n(J, s->s + hit, find->len)),
                             js_num((double)hit), js_from_str(s) };
            jval got = js_call(J, rep, js_undef(), args, 3);
            if (J->sig != JS_OK) return js_from_str(s);
            out = js_concat(J, out, js_to_str(J, got));
        } else {
            for (u32 i = 0; with && i < with->len && out; i++) {
                char c = with->s[i], d = i + 1 < with->len ? with->s[i + 1] : 0;
                if (c == '$' && d == '$') { out = js_concat(J, out, js_str(J, "$")); i++; }
                else if (c == '$' && d == '&') {
                    out = js_concat(J, out, find);
                    i++;
                } else if (c == '$' && d == '`') {
                    out = js_concat(J, out, js_str_n(J, s->s, (u32)hit));
                    i++;
                } else if (c == '$' && d == '\'') {
                    out = js_concat(J, out, js_str_n(J, s->s + hit + find->len,
                                                     s->len - (u32)hit - find->len));
                    i++;
                } else {
                    out = js_concat(J, out, js_str_n(J, with->s + i, 1));
                }
            }
        }
        at = (u32)hit + find->len;
        if (!every) break;
        if (find->len == 0) {
            /* An empty pattern matches between every character. */
            if (at >= s->len) break;
            out = js_concat(J, out, js_str_n(J, s->s + at, 1));
            at++;
        }
    }
    if (out) out = js_concat(J, out, js_str_n(J, s->s + at, s->len - at));
    return js_from_str(out ? out : s);
}

static jval nat_str_replace(jctx *J, jval t, jval *a, int n) {
    if (js_is_regex(js_arg(a, n, 0))) return nat_str_replace_re(J, t, a, n);
    return js_str_replace_text(J, t, a, n, 0);
}

static jval nat_str_replaceall(jctx *J, jval t, jval *a, int n) {
    if (js_is_regex(js_arg(a, n, 0))) return nat_str_replace_re(J, t, a, n);
    return js_str_replace_text(J, t, a, n, 1);
}

static jval nat_str_repeat(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    int times = (int)js_to_num(J, js_arg(a, n, 0));
    if (times < 0) times = 0;
    jstr *out = js_str(J, "");
    for (int i = 0; i < times && i < 4096; i++) out = js_concat(J, out, s);
    return js_from_str(out);
}

/* Every match of a global pattern, as strings; with a plain one, the same
   array exec gives, which is what a page destructures for its groups. */
static jval nat_str_match(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    jval re = js_arg(a, n, 0);
    if (!s || !js_is_regex(re)) return js_null();
    if (!js_rx_load(J, re, J->error_line)) return js_null();

    if (!js_rx.global) {
        if (rx_search(&js_rx, s->s, (int)s->len, 0) < 0) return js_null();
        return js_rx_result(J, s);
    }

    jobj *out = js_array(J);
    if (!out) return js_null();
    int from = 0;
    u32 got = 0;
    while (from <= (int)s->len) {
        if (rx_search(&js_rx, s->s, (int)s->len, from) < 0) break;
        js_arr_set(J, out, got++,
                   js_from_str(js_str_n(J, s->s + js_rx.cap_start[0],
                                        (u32)(js_rx.cap_end[0]
                                              - js_rx.cap_start[0]))));
        from = js_rx.cap_end[0];
        if (from == js_rx.cap_start[0]) from++;
    }
    return got ? js_from_obj(out) : js_null();
}

static jval nat_str_search(jctx *J, jval t, jval *a, int n) {
    jstr *s = js_to_str(J, t);
    jval re = js_arg(a, n, 0);
    if (!s || !js_is_regex(re)) return js_num(-1);
    if (!js_rx_load(J, re, J->error_line)) return js_num(-1);
    return js_num((double)rx_search(&js_rx, s->s, (int)s->len, 0));
}

static jval js_string_method(jctx *J, jval target, jstr *name) {
    /* Static, because a table this size built on the stack at every call is
       a copy the compiler makes with memcpy, and there is none to call. */
    static const struct { const char *n; jnative f; } M[] = {
        { "charAt", nat_str_charat }, { "charCodeAt", nat_str_charcode },
        { "indexOf", nat_str_indexof }, { "includes", nat_str_includes },
        { "lastIndexOf", nat_str_lastindexof },
        { "startsWith", nat_str_startswith }, { "endsWith", nat_str_endswith },
        { "slice", nat_str_slice }, { "substring", nat_str_substring },
        { "substr", nat_str_substr }, { "at", nat_str_at }, { "concat", nat_str_concat },
        { "padStart", nat_str_padstart }, { "padEnd", nat_str_padend },
        { "toUpperCase", nat_str_upper }, { "toLowerCase", nat_str_lower },
        { "trim", nat_str_trim }, { "split", nat_str_split },
        { "trimStart", nat_str_trimstart }, { "trimEnd", nat_str_trimend },
        { "trimLeft", nat_str_trimstart }, { "trimRight", nat_str_trimend },
        { "replace", nat_str_replace }, { "replaceAll", nat_str_replaceall },
        { "repeat", nat_str_repeat },
        { "match", nat_str_match }, { "search", nat_str_search },
        { 0, 0 }
    };
    for (int i = 0; M[i].n; i++) {
        if (!js_str_is(name, M[i].n)) continue;
        /* Called where it is fetched, the call supplies the receiver and one
           native for each method will do. Every s.charAt(i) in a loop used
           to make a native of its own, three hundred bytes that were never
           given back. */
        if (J->for_call && i < 24) {
            if (!J->str_methods[i]) J->str_methods[i] = js_native(J, M[i].n, M[i].f);
            return J->str_methods[i] ? js_from_obj(J->str_methods[i]) : js_undef();
        }
        jobj *o = js_native(J, M[i].n, M[i].f);
        if (!o) return js_undef();
        /* Taken away to be called later, the receiver travels with it. */
        js_set_hidden(J, o, "__this__", target);
        return js_from_obj(o);
    }
    return js_undef();
}

/* --- arrays -------------------------------------------------------------- */

static jval nat_arr_push(jctx *J, jval t, jval *a, int n) {
    if (t.t != JS_OBJ || !t.obj) return js_num(0);
    for (int i = 0; i < n; i++) js_arr_push(J, t.obj, a[i]);
    return js_num((double)t.obj->len);
}

static jval nat_arr_pop(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (t.t != JS_OBJ || !t.obj || !t.obj->len) return js_undef();
    return t.obj->items[--t.obj->len];
}

static jval nat_arr_shift(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (t.t != JS_OBJ || !t.obj || !t.obj->len) return js_undef();
    jval first = t.obj->items[0];
    for (u32 i = 1; i < t.obj->len; i++) t.obj->items[i - 1] = t.obj->items[i];
    t.obj->len--;
    return first;
}

static jval nat_arr_join(jctx *J, jval t, jval *a, int n) {
    if (t.t != JS_OBJ || !t.obj) return js_from_str(js_str(J, ""));
    jstr *sep = n > 0 ? js_to_str(J, a[0]) : js_str(J, ",");
    jstr *out = js_str(J, "");
    for (u32 i = 0; i < t.obj->len; i++) {
        if (i) out = js_concat(J, out, sep);
        jval e = t.obj->items[i];
        if (e.t == JS_UNDEF || e.t == JS_NULL) continue;
        out = js_concat(J, out, js_to_str(J, e));
    }
    return js_from_str(out);
}

static jval nat_arr_indexof(jctx *J, jval t, jval *a, int n) {
    if (t.t != JS_OBJ || !t.obj) return js_num(-1);
    jval want = js_arg(a, n, 0);
    for (u32 i = 0; i < t.obj->len; i++)
        if (js_strict_eq(t.obj->items[i], want)) return js_num((double)i);
    (void)J;
    return js_num(-1);
}

static jval nat_arr_slice(jctx *J, jval t, jval *a, int n) {
    jobj *out = js_array(J);
    if (t.t != JS_OBJ || !t.obj || !out) return js_from_obj(out);
    int len = (int)t.obj->len;
    int from = n > 0 ? (int)js_to_num(J, a[0]) : 0;
    int to = n > 1 && a[1].t != JS_UNDEF ? (int)js_to_num(J, a[1]) : len;
    if (from < 0) from += len;
    if (to < 0) to += len;
    if (from < 0) from = 0;
    if (to > len) to = len;
    for (int i = from; i < to; i++) js_arr_push(J, out, t.obj->items[i]);
    return js_from_obj(out);
}

static jval nat_arr_foreach(jctx *J, jval t, jval *a, int n) {
    if (t.t != JS_OBJ || !t.obj || n < 1) return js_undef();
    for (u32 i = 0; i < t.obj->len; i++) {
        jval args[2];
        args[0] = t.obj->items[i];
        args[1] = js_num((double)i);
        js_call(J, a[0], js_undef(), args, 2);
        if (J->sig != JS_OK) break;
    }
    return js_undef();
}

static jval nat_arr_map(jctx *J, jval t, jval *a, int n) {
    jobj *out = js_array(J);
    if (t.t != JS_OBJ || !t.obj || n < 1 || !out) return js_from_obj(out);
    for (u32 i = 0; i < t.obj->len; i++) {
        jval args[2];
        args[0] = t.obj->items[i];
        args[1] = js_num((double)i);
        js_arr_push(J, out, js_call(J, a[0], js_undef(), args, 2));
        if (J->sig != JS_OK) break;
    }
    return js_from_obj(out);
}

static jval nat_arr_filter(jctx *J, jval t, jval *a, int n) {
    jobj *out = js_array(J);
    if (t.t != JS_OBJ || !t.obj || n < 1 || !out) return js_from_obj(out);
    for (u32 i = 0; i < t.obj->len; i++) {
        jval args[2];
        args[0] = t.obj->items[i];
        args[1] = js_num((double)i);
        if (js_to_bool(js_call(J, a[0], js_undef(), args, 2)))
            js_arr_push(J, out, t.obj->items[i]);
        if (J->sig != JS_OK) break;
    }
    return js_from_obj(out);
}

static jval nat_arr_reverse(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (t.t != JS_OBJ || !t.obj) return t;
    for (u32 i = 0, k = t.obj->len; i + 1 < k; i++, k--) {
        jval tmp = t.obj->items[i];
        t.obj->items[i] = t.obj->items[k - 1];
        t.obj->items[k - 1] = tmp;
    }
    return t;
}

/* An insertion sort, by the comparison function when one is given and by
   text when one is not, which is what this language does by default and the
   reason [1, 10, 2] sorts into that order. */
static jval nat_arr_sort(jctx *J, jval t, jval *a, int n) {
    if (t.t != JS_OBJ || !t.obj) return t;
    jobj *o = t.obj;
    for (u32 i = 1; i < o->len; i++) {
        jval v = o->items[i];
        u32 k = i;
        while (k > 0) {
            int swap;
            if (n > 0 && a[0].t == JS_OBJ) {
                jval args[2];
                args[0] = o->items[k - 1];
                args[1] = v;
                swap = js_to_num(J, js_call(J, a[0], js_undef(), args, 2)) > 0;
                if (J->sig != JS_OK) return t;
            } else {
                jstr *x = js_to_str(J, o->items[k - 1]);
                jstr *y = js_to_str(J, v);
                u32 m = x->len < y->len ? x->len : y->len;
                int cmp = 0;
                for (u32 q = 0; q < m && !cmp; q++)
                    cmp = (u8)x->s[q] - (u8)y->s[q];
                if (!cmp) cmp = (int)x->len - (int)y->len;
                swap = cmp > 0;
            }
            if (!swap) break;
            o->items[k] = o->items[k - 1];
            k--;
        }
        o->items[k] = v;
    }
    return t;
}

static jval js_array_method(jctx *J, jval target, jstr *name) {
    struct { const char *n; jnative f; } M[] = {
        { "push", nat_arr_push }, { "pop", nat_arr_pop },
        { "shift", nat_arr_shift }, { "join", nat_arr_join },
        { "indexOf", nat_arr_indexof }, { "slice", nat_arr_slice },
        { "forEach", nat_arr_foreach }, { "map", nat_arr_map },
        { "filter", nat_arr_filter }, { "reverse", nat_arr_reverse },
        { "sort", nat_arr_sort },
        { 0, 0 }
    };
    for (int i = 0; M[i].n; i++) {
        if (!js_str_is(name, M[i].n)) continue;
        if (J->for_call && i < 16) {             /* as for strings, above */
            if (!J->arr_methods[i]) J->arr_methods[i] = js_native(J, M[i].n, M[i].f);
            return J->arr_methods[i] ? js_from_obj(J->arr_methods[i]) : js_undef();
        }
        jobj *o = js_native(J, M[i].n, M[i].f);
        if (!o) return js_undef();
        js_set_hidden(J, o, "__this__", target);
        return js_from_obj(o);
    }
    return js_undef();
}

/* --- the evaluator -------------------------------------------------------- */

static int js_tick(jctx *J) {
    if (++J->steps > JS_STEP_CAP) {
        if (J->sig == JS_OK) {
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

/* Both sides of an assignment target: where to read it and where to put it
   back. Used by ++, -- and the compound assignments, which need both. */
typedef struct {
    int    kind;              /* 0 name, 1 property, 2 nothing */
    jstr  *name;
    jval   obj;
} jplace;

static jplace js_place(jctx *J, int node, jscope *sc, jval this_val) {
    jplace p;
    p.kind = 2;
    p.name = 0;
    p.obj = js_undef();
    if (node < 0) return p;

    jnode *n = &J->nodes[node];
    if (n->kind == N_IDENT) {
        p.kind = 0;
        p.name = n->str;
        return p;
    }
    if (n->kind == N_MEMBER) {
        p.kind = 1;
        p.obj = js_eval(J, n->a, sc, this_val);
        p.name = n->str;
        return p;
    }
    if (n->kind == N_INDEX) {
        p.kind = 1;
        p.obj = js_eval(J, n->a, sc, this_val);
        p.name = js_to_str(J, js_eval(J, n->b, sc, this_val));
        return p;
    }
    return p;
}

/* Whether a break or continue that has arrived here was aimed at this
   loop. One with no name is for whichever loop catches it first; one with a
   name belongs to the statement of that name and nothing else. */
static int js_label_mine(jctx *J, jstr *mine) {
    if (!J->label) return 1;
    return mine && js_str_eq(J->label, mine);
}

static jval js_place_get(jctx *J, jplace *p, jscope *sc) {
    if (p->kind == 0) {
        jprop *v = js_lookup(sc, p->name);
        return v ? v->v : js_undef();
    }
    if (p->kind == 1) return js_get(J, p->obj, p->name);
    return js_undef();
}

static void js_place_put(jctx *J, jplace *p, jscope *sc, jval v) {
    if (p->kind == 0) js_assign_name(J, sc, p->name, v);
    else if (p->kind == 1) js_put(J, p->obj, p->name, v);
}

static jval js_binary(jctx *J, jop op, jval l, jval r, int line) {
    switch (op) {
        case OP_ADD:
            /* The one operator that is two operators. If either side is a
               string the result is text; otherwise it is arithmetic. */
            if (l.t == JS_STR || r.t == JS_STR
                || (l.t == JS_OBJ && l.obj && l.obj->kind != JO_ARRAY)
                || (r.t == JS_OBJ && r.obj && r.obj->kind != JO_ARRAY))
                return js_from_str(js_concat(J, js_to_str(J, l), js_to_str(J, r)));
            return js_num(js_to_num(J, l) + js_to_num(J, r));

        case OP_SUB: return js_num(js_to_num(J, l) - js_to_num(J, r));
        case OP_MUL: return js_num(js_to_num(J, l) * js_to_num(J, r));
        case OP_DIV: return js_num(js_to_num(J, l) / js_to_num(J, r));
        case OP_MOD: {
            double a = js_to_num(J, l), b = js_to_num(J, r);
            if (b == 0) { double z = 0.0; return js_num(z / z); }
            double q = a / b;
            double w = q < 0 ? -(double)(long long)(-q) : (double)(long long)q;
            return js_num(a - w * b);
        }

        case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
            /* Two strings compare as text; anything else compares as
               numbers, which is why "10" < "9" and 10 > 9 are both true. */
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
        case OP_SHL:  return js_num((double)(js_to_i32(J, l) << (js_to_u32(J, r) & 31)));
        case OP_SHR:  return js_num((double)(js_to_i32(J, l) >> (js_to_u32(J, r) & 31)));
        case OP_USHR: return js_num((double)(js_to_u32(J, l) >> (js_to_u32(J, r) & 31)));

        /* `x instanceof F`
         *
         * There is no prototype chain a script can reach into here, so this
         * cannot be the walk up one that it is in a bigger engine. What it
         * is instead is exact about the thing it can be exact about: every
         * object made with `new` remembers what made it, and that is the
         * question being asked in nearly every use of this operator.
         *
         * What is therefore missing is inheritance -- an object made by one
         * constructor is not an instance of another that its maker was set
         * up from. That is said here rather than discovered: a false where
         * a page expected true is a branch not taken, and the page will
         * look like it decided something rather than like it broke.
         *
         * The built-in names are answered by what the object actually is,
         * because an array is an array whether or not anybody said new. */
        case OP_INSTANCEOF: {
            if (l.t != JS_OBJ || !l.obj) return js_bool(0);
            if (r.t != JS_OBJ || !r.obj)
                return js_throw(J, JS_ERR_TYPE, "the right of instanceof is not a "
                                   "constructor", line);

            jprop *made_by = js_find(l.obj, J->s_ctor);
            if (made_by && made_by->v.t == JS_OBJ && made_by->v.obj == r.obj)
                return js_bool(1);

            /* Every error is an Error, whichever of the six made it: the
               one piece of inheritance a page leans on. */
            if (r.obj == J->err_ctor[JS_ERR_ERROR] && js_is_error(J, l.obj))
                return js_bool(1);

            jstr *nm = r.obj->name;
            if (nm) {
                if (js_str_is(nm, "Array"))
                    return js_bool(l.obj->kind == JO_ARRAY);
                if (js_str_is(nm, "RegExp"))
                    return js_bool(l.obj->kind == JO_REGEX);
                if (js_str_is(nm, "Function"))
                    return js_bool(l.obj->kind == JO_FUNC
                                   || l.obj->kind == JO_NATIVE);
                if (js_str_is(nm, "Object")) return js_bool(1);
            }
            return js_bool(0);
        }

        case OP_IN: {
            if (r.t != JS_OBJ || !r.obj) return js_bool(0);
            jstr *k = js_to_str(J, l);
            if (r.obj->kind == JO_ARRAY) {
                u32 idx;
                if (js_index_of(k, &idx)) return js_bool(idx < r.obj->len);
            }
            return js_bool(js_find(r.obj, k) != 0);
        }

        default:
            return js_throw(J, JS_ERR_SYNTAX, "this operator is not one this engine has", line);
    }
}

static jval js_eval(jctx *J, int node, jscope *sc, jval this_val) {
    if (node < 0 || !js_tick(J)) return js_undef();
    jnode *n = &J->nodes[node];
    J->error_line = n->line;

    switch (n->kind) {
        case N_NUM:   return js_num(n->num);
        case N_STR:   return js_from_str(n->str);
        case N_REGEX: {
            /* A fresh object each time the literal is reached, because a
               global pattern carries a lastIndex and two loops sharing one
               would each start where the other left off. */
            jobj *o = js_regex_new(J, n->str ? n->str->s : "",
                                   n->str ? n->str->len : 0, n->op);
            return o ? js_from_obj(o) : js_undef();
        }
        case N_TRUE:  return js_bool(1);
        case N_FALSE: return js_bool(0);
        case N_NULL:  return js_null();
        case N_UNDEF: return js_undef();
        case N_THIS:  return this_val;

        case N_IDENT: {
            jprop *p = js_lookup(sc, n->str);
            if (p) return p->v;
            /* A name that was never given a value is undefined rather than
               an error, which is this language's rule and is how a page
               tests for a feature by naming it. */
            return js_undef();
        }

        case N_ARRAY: {
            jobj *a = js_array(J);
            if (!a) return js_undef();
            for (int cell = n->a; cell >= 0; cell = J->nodes[cell].b) {
                js_arr_push(J, a, js_eval(J, J->nodes[cell].a, sc, this_val));
                if (J->sig != JS_OK) break;
            }
            return js_from_obj(a);
        }

        case N_OBJECT: {
            jobj *o = js_object(J, JO_PLAIN);
            if (!o) return js_undef();
            for (int cell = n->a; cell >= 0; cell = J->nodes[cell].b) {
                jval v = js_eval(J, J->nodes[cell].a, sc, this_val);
                js_set_prop(J, o, J->nodes[cell].str, v);
                if (J->sig != JS_OK) break;
            }
            return js_from_obj(o);
        }

        case N_FUNC: case N_FUNCDECL: {
            jobj *f = js_object(J, JO_FUNC);
            if (!f) return js_undef();
            f->body = n->a;
            f->params = n->b;
            f->nparams = n->c;
            f->uses_args = n->d > 0;
            f->closure = sc;
            f->name = n->str;
            /* The function can be called after the call it was made in is
               over, so that call's scope, and every scope around it, has to
               stay. */
            for (jscope *s = sc; s && !s->escaped; s = s->parent) s->escaped = 1;
            /* An arrow takes `this` from where it was written rather than
               from wherever it is later called, so it is caught here, at
               the moment the function value is made. */
            if (n->op) js_set_hidden(J, f, "__this__", this_val);
            return js_from_obj(f);
        }

        case N_MEMBER: {
            jval target = js_eval(J, n->a, sc, this_val);
            if (target.t == JS_UNDEF || target.t == JS_NULL) {
                char msg[96];
                int w = 0;
                const char *a = "cannot read ";
                while (*a) msg[w++] = *a++;
                for (u32 i = 0; i < n->str->len && w < 80; i++)
                    msg[w++] = n->str->s[i];
                const char *b = " of nothing";
                while (*b) msg[w++] = *b++;
                msg[w] = 0;
                return js_throw(J, JS_ERR_TYPE, msg, n->line);
            }
            return js_get(J, target, n->str);
        }

        case N_INDEX: {
            jval target = js_eval(J, n->a, sc, this_val);
            jval idx = js_eval(J, n->b, sc, this_val);
            if (target.t == JS_OBJ && target.obj
                && target.obj->kind == JO_ARRAY && idx.t == JS_NUM) {
                double d = idx.num;
                if (d >= 0 && d < (double)target.obj->len && d == (double)(int)d)
                    return target.obj->items[(u32)d];
                return js_undef();
            }
            return js_get(J, target, js_to_str(J, idx));
        }

        case N_CALL: {
            jval fn, self = js_undef();
            int callee = n->a;
            if (callee >= 0 && (J->nodes[callee].kind == N_MEMBER
                                || J->nodes[callee].kind == N_INDEX)) {
                self = js_eval(J, J->nodes[callee].a, sc, this_val);
                jstr *name = J->nodes[callee].kind == N_MEMBER
                    ? J->nodes[callee].str
                    : js_to_str(J, js_eval(J, J->nodes[callee].b, sc, this_val));
                if (self.t == JS_UNDEF || self.t == JS_NULL)
                    return js_throw(J, JS_ERR_TYPE, "cannot call a method on nothing", n->line);
                int was = J->for_call;
                J->for_call = 1;
                fn = js_get(J, self, name);
                J->for_call = was;
            } else {
                fn = js_eval(J, callee, sc, this_val);
            }
            if (J->sig != JS_OK) return js_undef();

            /* A method carries the value it was fetched from, so a native
               that was bound gets its receiver back. */
            if (fn.t == JS_OBJ && fn.obj && fn.obj->kind == JO_NATIVE) {
                jprop *b = js_find(fn.obj, J->s_this);
                if (b) self = b->v;
            }

            jval argv[JS_ARGS_MAX];
            int argc = 0;
            for (int cell = n->b; cell >= 0 && argc < JS_ARGS_MAX;
                 cell = J->nodes[cell].b) {
                argv[argc++] = js_eval(J, J->nodes[cell].a, sc, this_val);
                if (J->sig != JS_OK) return js_undef();
            }

            /* Say which name was not a function.
             *
               "this is not a function" is true and useless: a page calls
               hundreds of them and the message names none. The name is
               right here at the call site, and knowing it is the difference
               between a report and a thing somebody can act on -- it is how
               the missing DOM methods below were found rather than
               guessed. */
            if (fn.t != JS_OBJ || !fn.obj
                || (fn.obj->kind != JO_FUNC && fn.obj->kind != JO_NATIVE)) {
                if (callee >= 0 && J->nodes[callee].str
                    && (J->nodes[callee].kind == N_MEMBER
                        || J->nodes[callee].kind == N_IDENT)) {
                    char said[96];
                    int w = 0;
                    const char *nm = J->nodes[callee].str->s;
                    for (int i = 0; nm[i] && w < (int)sizeof(said) - 24; i++)
                        said[w++] = nm[i];
                    const char *tail = " is not a function";
                    for (int i = 0; tail[i] && w < (int)sizeof(said) - 1; i++)
                        said[w++] = tail[i];
                    said[w] = 0;
                    return js_throw(J, JS_ERR_TYPE, said, n->line);
                }
            }
            return js_call(J, fn, self, argv, argc);
        }

        case N_NEW: {
            jval fn = js_eval(J, n->a, sc, this_val);
            jval argv[JS_ARGS_MAX];
            int argc = 0;
            for (int cell = n->b; cell >= 0 && argc < JS_ARGS_MAX;
                 cell = J->nodes[cell].b) {
                argv[argc++] = js_eval(J, J->nodes[cell].a, sc, this_val);
                if (J->sig != JS_OK) return js_undef();
            }
            jobj *fresh = js_object(J, JO_PLAIN);
            if (!fresh) return js_undef();
            /* So that instanceof has something exact to answer with. */
            if (fn.t == JS_OBJ && fn.obj) js_set_hidden(J, fresh, "__ctor__", fn);
            jval self = js_from_obj(fresh);
            jval out = js_call(J, fn, self, argv, argc);
            /* A constructor that returns an object returns that; one that
               returns anything else returns the object that was made. */
            return out.t == JS_OBJ ? out : self;
        }

        case N_UNARY: {
            jval v = js_eval(J, n->a, sc, this_val);
            switch (n->op) {
                case OP_NOT:  return js_bool(!js_to_bool(v));
                case OP_NEG:  return js_num(-js_to_num(J, v));
                case OP_POS:  return js_num(js_to_num(J, v));
                case OP_BNOT: return js_num((double)(~js_to_i32(J, v)));
                default:      return js_undef();      /* void */
            }
        }

        case N_TYPEOF: {
            /* typeof on a name that does not exist is "undefined" rather
               than an error, which is the whole reason it is used. */
            jval v = js_eval(J, n->a, sc, this_val);
            const char *t = "undefined";
            switch (v.t) {
                case JS_UNDEF: t = "undefined"; break;
                case JS_NULL:  t = "object"; break;     /* the famous one */
                case JS_BOOL:  t = "boolean"; break;
                case JS_NUM:   t = "number"; break;
                case JS_STR:   t = "string"; break;
                case JS_OBJ:
                    t = (v.obj && (v.obj->kind == JO_FUNC
                                   || v.obj->kind == JO_NATIVE))
                        ? "function" : "object";
                    break;
            }
            return js_from_str(js_str(J, t));
        }

        case N_DELETE: {
            jplace p = js_place(J, n->a, sc, this_val);
            if (p.kind == 1 && p.obj.t == JS_OBJ && p.obj.obj)
                return js_bool(js_delete_prop(p.obj.obj, p.name));
            return js_bool(0);
        }

        case N_BINARY: {
            jval l = js_eval(J, n->a, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            jval r = js_eval(J, n->b, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            return js_binary(J, (jop)n->op, l, r, n->line);
        }

        case N_LOGICAL: {
            /* Short circuit, and the value of the side that decided it
               rather than a boolean: `a || b` is b when a is falsy. */
            jval l = js_eval(J, n->a, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            if (n->op == OP_AND) return js_to_bool(l) ? js_eval(J, n->b, sc, this_val) : l;
            return js_to_bool(l) ? l : js_eval(J, n->b, sc, this_val);
        }

        case N_COND:
            return js_to_bool(js_eval(J, n->a, sc, this_val))
                 ? js_eval(J, n->b, sc, this_val)
                 : js_eval(J, n->c, sc, this_val);

        case N_ASSIGN: {
            jplace p = js_place(J, n->a, sc, this_val);
            if (J->sig != JS_OK) return js_undef();
            if (p.kind == 2) return js_throw(J, JS_ERR_SYNTAX, "this cannot be assigned to",
                                             n->line);
            jval r = js_eval(J, n->b, sc, this_val);
            if (J->sig != JS_OK) return js_undef();

            if (n->op != OP_ASSIGN) {
                jval cur = js_place_get(J, &p, sc);
                jop base = OP_ADD;
                switch (n->op) {
                    case OP_ADDEQ: base = OP_ADD; break;
                    case OP_SUBEQ: base = OP_SUB; break;
                    case OP_MULEQ: base = OP_MUL; break;
                    case OP_DIVEQ: base = OP_DIV; break;
                    case OP_MODEQ: base = OP_MOD; break;
                    case OP_OREQ:  base = OP_BOR; break;
                    case OP_ANDEQ: base = OP_BAND; break;
                    default: break;
                }
                r = js_binary(J, base, cur, r, n->line);
            }
            js_place_put(J, &p, sc, r);
            return r;
        }

        case N_PREINC: case N_POSTINC: {
            jplace p = js_place(J, n->a, sc, this_val);
            if (p.kind == 2) return js_undef();
            double cur = js_to_num(J, js_place_get(J, &p, sc));
            double next = n->op == OP_INC ? cur + 1 : cur - 1;
            js_place_put(J, &p, sc, js_num(next));
            return js_num(n->kind == N_PREINC ? next : cur);
        }

        case N_SEQ: {
            jval a = js_eval(J, n->a, sc, this_val);
            if (n->c >= 0) return js_eval(J, n->c, sc, this_val);
            return a;
        }

        default:
            return js_undef();
    }
}

/* --- statements ----------------------------------------------------------- */

/* Function declarations are visible from the top of the scope they are in,
   before the line that declares them has run. A page relies on this every
   time it calls a function defined at the bottom of a script. */
static void js_hoist(jctx *J, int block, jscope *sc) {
    if (block < 0) return;
    for (int cell = J->nodes[block].a; cell >= 0; cell = J->nodes[cell].b) {
        int st = J->nodes[cell].a;
        if (st < 0 || J->nodes[st].kind != N_FUNCDECL) continue;
        jval f = js_eval(J, st, sc, js_undef());
        if (J->nodes[st].str) js_declare(J, sc, J->nodes[st].str, f);
    }
}

static jsignal js_exec(jctx *J, int node, jscope *sc, jval this_val) {
    if (node < 0 || !js_tick(J)) return J->sig;
    jnode *n = &J->nodes[node];
    J->error_line = n->line;

    switch (n->kind) {
        case N_BLOCK:
            js_hoist(J, node, sc);
            for (int cell = n->a; cell >= 0; cell = J->nodes[cell].b) {
                jsignal s = js_exec(J, J->nodes[cell].a, sc, this_val);
                if (s != JS_OK) return s;
            }
            return JS_OK;

        case N_EMPTY: case N_FUNCDECL:
            return JS_OK;                   /* already hoisted */

        case N_EXPRSTMT:
            js_eval(J, n->a, sc, this_val);
            return J->sig;

        case N_VAR:
            for (int cell = n->a; cell >= 0; cell = J->nodes[cell].b) {
                /* `var x;` declares x and does nothing to a value it already
                   has. It used to set it to undefined, and minified code
                   declares the same name again all the time -- a counter
                   declared at the top of each loop went back to nothing. */
                if (n->d && J->nodes[cell].a < 0 && js_find(sc->vars, J->nodes[cell].str))
                    continue;
                jval v = J->nodes[cell].a >= 0
                    ? js_eval(J, J->nodes[cell].a, sc, this_val) : js_undef();
                if (J->sig != JS_OK) return J->sig;
                js_declare(J, sc, J->nodes[cell].str, v);
            }
            return JS_OK;

        case N_IF:
            if (js_to_bool(js_eval(J, n->a, sc, this_val)))
                return js_exec(J, n->b, sc, this_val);
            if (n->c >= 0) return js_exec(J, n->c, sc, this_val);
            return J->sig;

        case N_WHILE: {
            jstr *mine = J->pending_label;
            J->pending_label = 0;
            while (js_to_bool(js_eval(J, n->a, sc, this_val))) {
                if (J->sig != JS_OK) return J->sig;
                jsignal s = js_exec(J, n->b, sc, this_val);
                if (s == JS_BREAK) {
                    if (!js_label_mine(J, mine)) return s;
                    J->label = 0; J->sig = JS_OK; break;
                }
                if (s == JS_CONTINUE) {
                    if (!js_label_mine(J, mine)) return s;
                    J->label = 0; J->sig = JS_OK; continue;
                }
                if (s != JS_OK) return s;
                if (!js_tick(J)) return J->sig;
            }
            return J->sig;
        }

        case N_DO: {
            jstr *mine = J->pending_label;
            J->pending_label = 0;
            for (;;) {
                jsignal s = js_exec(J, n->b, sc, this_val);
                if (s == JS_BREAK) {
                    if (!js_label_mine(J, mine)) return s;
                    J->label = 0; J->sig = JS_OK; break;
                }
                if (s == JS_CONTINUE) {
                    if (!js_label_mine(J, mine)) return s;
                    J->label = 0; J->sig = JS_OK;
                } else if (s != JS_OK) return s;
                if (!js_to_bool(js_eval(J, n->a, sc, this_val))) break;
                if (!js_tick(J)) return J->sig;
            }
            return J->sig;
        }

        case N_FOR: {
            jstr *mine = J->pending_label;
            J->pending_label = 0;
            if (n->a >= 0) {
                jsignal s = js_exec(J, n->a, sc, this_val);
                if (s != JS_OK) return s;
            }
            for (;;) {
                if (n->b >= 0 && !js_to_bool(js_eval(J, n->b, sc, this_val)))
                    break;
                if (J->sig != JS_OK) return J->sig;
                jsignal s = js_exec(J, n->d, sc, this_val);
                if (s == JS_BREAK) {
                    if (!js_label_mine(J, mine)) return s;
                    J->label = 0; J->sig = JS_OK; break;
                }
                if (s == JS_CONTINUE && !js_label_mine(J, mine)) return s;
                if (s != JS_OK && s != JS_CONTINUE) return s;
                if (s == JS_CONTINUE) { J->label = 0; J->sig = JS_OK; }
                if (n->c >= 0) js_eval(J, n->c, sc, this_val);
                if (!js_tick(J)) return J->sig;
            }
            return J->sig;
        }

        case N_FORIN: {
            jval target = js_eval(J, n->a, sc, this_val);
            if (J->sig != JS_OK) return J->sig;
            if (target.t != JS_OBJ || !target.obj) return JS_OK;
            jobj *o = target.obj;

            /* The keys are collected first. A body that adds or removes a
               property while the loop is running would otherwise walk a
               table that moved under it. */
            jobj *keys = js_array(J);
            if (!keys) return J->sig;
            if (o->kind == JO_ARRAY) {
                for (u32 i = 0; i < o->len; i++) {
                    /* Thirty-two, because js_num_text refuses anything
                       smaller and writes an empty string instead; with
                       sixteen here every index came out as "". */
                    char tmp[32];
                    u32 w = js_num_text((double)i, tmp, sizeof(tmp));
                    js_arr_push(J, keys, js_from_str(js_str_n(J, tmp, w)));
                }
            }
            jprop **own;
            u32 nown = js_own_keys(J, o, &own);
            for (u32 i = 0; i < nown; i++) js_arr_push(J, keys, js_from_str(own[i]->key));

            for (u32 i = 0; i < keys->len; i++) {
                if (n->d) js_declare(J, sc, n->str, keys->items[i]);
                else if (n->str) js_assign_name(J, sc, n->str, keys->items[i]);
                else if (n->c >= 0) {
                    jplace p = js_place(J, n->c, sc, this_val);
                    js_place_put(J, &p, sc, keys->items[i]);
                }
                jsignal s = js_exec(J, n->b, sc, this_val);
                if (s == JS_BREAK) { J->sig = JS_OK; break; }
                if (s == JS_CONTINUE) { J->sig = JS_OK; continue; }
                if (s != JS_OK) return s;
            }
            return J->sig;
        }

        case N_RETURN: {
            /* Into a local first: when the value throws, J->ret is what was
               thrown, and writing the failed evaluation's undefined over it
               made every error that passed through `return f()` arrive in
               the catch as undefined. */
            jval v = n->a >= 0 ? js_eval(J, n->a, sc, this_val) : js_undef();
            if (J->sig != JS_OK) return J->sig;
            J->ret = v;
            J->sig = JS_RETURN;
            return JS_RETURN;
        }

        case N_BREAK:
            J->label = n->str;
            J->sig = JS_BREAK;
            return JS_BREAK;
        case N_CONTINUE:
            J->label = n->str;
            J->sig = JS_CONTINUE;
            return JS_CONTINUE;

        /* The name is handed to the statement about to run, so that a loop
           can tell a break meant for it from one meant for something it is
           inside. A labelled thing that is not a loop -- a block, which is
           the other common one -- catches its own break here. */
        case N_LABEL: {
            J->pending_label = n->str;
            jsignal s = js_exec(J, n->a, sc, this_val);
            J->pending_label = 0;
            if (s == JS_BREAK && J->label && js_str_eq(J->label, n->str)) {
                J->label = 0;
                J->sig = JS_OK;
                return JS_OK;
            }
            return s;
        }

        case N_THROW: {
            jval v = js_eval(J, n->a, sc, this_val);
            if (J->sig != JS_OK) return J->sig;
            J->ret = v;
            J->sig = JS_THROWN;
            jstr *s = js_to_str(J, v);
            int i = 0;
            for (; s && i < (int)s->len && i < (int)sizeof(J->error) - 1; i++)
                J->error[i] = s->s[i];
            J->error[i] = 0;
            J->error_line = n->line;
            return JS_THROWN;
        }

        case N_TRY: {
            jsignal s = js_exec(J, n->a, sc, this_val);
            if (s == JS_THROWN && n->b >= 0) {
                jval thrown = J->ret;
                J->sig = JS_OK;
                J->ret = js_undef();
                jscope *cs = js_scope(J, sc);
                if (n->str) js_declare(J, cs, n->str, thrown);
                s = js_exec(J, n->b, cs, this_val);
                /* Given back like a call's, and for the same reason. */
                if (cs != sc && !cs->escaped) js_scope_free(J, cs);
            }
            if (n->c >= 0) {
                jsignal keep = s;
                jval kept = J->ret;
                J->sig = JS_OK;
                jsignal f = js_exec(J, n->c, sc, this_val);
                /* A finally that itself leaves early wins; otherwise
                   whatever the body or the catch decided stands. */
                if (f != JS_OK) return f;
                J->sig = keep;
                J->ret = kept;
                s = keep;
            }
            return s;
        }

        case N_SWITCH: {
            jval subject = js_eval(J, n->a, sc, this_val);
            if (J->sig != JS_OK) return J->sig;

            int started = 0;
            /* Two passes: the matching arm, then everything from there on,
               because falling through is the behaviour and not a bug. */
            for (int pass = 0; pass < 2 && !started; pass++) {
                for (int arm = n->b; arm >= 0; arm = J->nodes[arm].c) {
                    if (!started) {
                        int test = J->nodes[arm].a;
                        if (pass == 0) {
                            if (test < 0) continue;      /* default, later */
                            jval v = js_eval(J, test, sc, this_val);
                            if (!js_strict_eq(subject, v)) continue;
                        } else {
                            if (test >= 0) continue;     /* only the default */
                        }
                        started = 1;
                    }
                    for (int cell = J->nodes[arm].b; cell >= 0;
                         cell = J->nodes[cell].b) {
                        jsignal s = js_exec(J, J->nodes[cell].a, sc, this_val);
                        if (s == JS_BREAK) { J->sig = JS_OK; return JS_OK; }
                        if (s != JS_OK) return s;
                    }
                }
            }
            return J->sig;
        }

        default:
            js_eval(J, node, sc, this_val);
            return J->sig;
    }
}

/* --- the global objects ---------------------------------------------------
 *
 * What a script finds already there. Deliberately short: this is what pages
 * actually reach for, and every name here is one somebody could otherwise
 * have used for their own.
 */

/* Where console.log goes. A program sets this; with nothing set the output
   is dropped, which is what a browser tab with no console open does. */
static void (*js_print_hook)(const char *s, u32 n);

static jval nat_log(jctx *J, jval t, jval *a, int n) {
    (void)t;
    for (int i = 0; i < n; i++) {
        if (i && js_print_hook) js_print_hook(" ", 1);
        jstr *s = js_to_str(J, a[i]);
        if (s && js_print_hook) js_print_hook(s->s, s->len);
    }
    if (js_print_hook) js_print_hook("\n", 1);
    return js_undef();
}

static jval nat_parseint(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s) { double z = 0.0; return js_num(z / z); }
    int radix = n > 1 ? (int)js_to_num(J, a[1]) : 10;
    if (radix < 2 || radix > 36) radix = 10;

    u32 i = 0;
    while (i < s->len && (s->s[i] == ' ' || s->s[i] == '\t')) i++;
    int neg = 0;
    if (i < s->len && (s->s[i] == '-' || s->s[i] == '+'))
        neg = s->s[i++] == '-';
    if (radix == 16 && i + 1 < s->len && s->s[i] == '0'
        && (s->s[i + 1] == 'x' || s->s[i + 1] == 'X')) i += 2;

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
    if (!any) { double z = 0.0; return js_num(z / z); }
    return js_num(neg ? -v : v);
}

static jval nat_parsefloat(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s) { double z = 0.0; return js_num(z / z); }
    /* The longest decimal at the front, after any spaces. It took every
       character that could be in a number and then asked whether they made
       one, so "1-2" was not 1 but nothing at all. */
    u32 i = 0, used = 0;
    while (i < s->len && js_blank(s->s[i])) i++;
    double v = js_signed_decimal(s->s + i, s->len - i, &used);
    if (!used) { double z = 0.0; return js_num(z / z); }
    return js_num(v);
}

static jval nat_isnan(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double d = js_to_num(J, js_arg(a, n, 0));
    return js_bool(d != d);
}

static jval nat_str_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return js_from_str(js_to_str(J, js_arg(a, n, 0)));
}

static jval nat_num_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t;
    return js_num(js_to_num(J, js_arg(a, n, 0)));
}

static jval nat_bool_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)J;
    return js_bool(js_to_bool(js_arg(a, n, 0)));
}

/* --- Math ---------------------------------------------------------------- */

static double js_fabs(double x) { return x < 0 ? -x : x; }

static double js_floor(double x) {
    double t = (double)(long long)x;
    return (x < 0 && t != x) ? t - 1 : t;
}

static double js_ceil(double x) {
    double t = (double)(long long)x;
    return (x > 0 && t != x) ? t + 1 : t;
}

/* Newton's method, which needs nothing but division and converges in a
   handful of steps once the first guess is halfway sensible. */
static double js_sqrt(double x) {
    if (x < 0) { double z = 0.0; return z / z; }
    if (x == 0) return 0;
    double g = x > 1 ? x : 1.0;
    for (int i = 0; i < 60; i++) {
        double nx = (g + x / g) * 0.5;
        if (js_fabs(nx - g) < 1e-15 * (g < 0 ? -g : g)) { g = nx; break; }
        g = nx;
    }
    return g;
}

static jval nat_m_floor(jctx *J, jval t, jval *a, int n) {
    (void)t; return js_num(js_floor(js_to_num(J, js_arg(a, n, 0))));
}
static jval nat_m_ceil(jctx *J, jval t, jval *a, int n) {
    (void)t; return js_num(js_ceil(js_to_num(J, js_arg(a, n, 0))));
}
static jval nat_m_round(jctx *J, jval t, jval *a, int n) {
    (void)t;
    /* Halves go up, including negative ones, which is this language's rule
       and differs from the one most other languages use. */
    return js_num(js_floor(js_to_num(J, js_arg(a, n, 0)) + 0.5));
}
static jval nat_m_abs(jctx *J, jval t, jval *a, int n) {
    (void)t; return js_num(js_fabs(js_to_num(J, js_arg(a, n, 0))));
}
static jval nat_m_sqrt(jctx *J, jval t, jval *a, int n) {
    (void)t; return js_num(js_sqrt(js_to_num(J, js_arg(a, n, 0))));
}
static jval nat_m_min(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (!n) return js_num(1.0 / 0.0 * 0.0 + 1e308 * 10);
    double best = js_to_num(J, a[0]);
    for (int i = 1; i < n; i++) {
        double v = js_to_num(J, a[i]);
        if (v < best) best = v;
    }
    return js_num(best);
}
static jval nat_m_max(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (!n) return js_num(-1e308 * 10);
    double best = js_to_num(J, a[0]);
    for (int i = 1; i < n; i++) {
        double v = js_to_num(J, a[i]);
        if (v > best) best = v;
    }
    return js_num(best);
}
static jval nat_m_pow(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double b = js_to_num(J, js_arg(a, n, 0));
    double e = js_to_num(J, js_arg(a, n, 1));
    /* Whole exponents by repeated multiplication, which is exact; anything
       else needs a logarithm this does not have and is refused as NaN
       rather than approximated badly. */
    if (e != js_floor(e) || js_fabs(e) > 1024) {
        if (e == 0.5) return js_num(js_sqrt(b));
        double z = 0.0;
        return js_num(z / z);
    }
    int neg = e < 0;
    long long k = (long long)(neg ? -e : e);
    double out = 1.0;
    while (k--) out *= b;
    return js_num(neg ? 1.0 / out : out);
}

/* A repeatable sequence rather than a real one. Nothing in a page needs
   unpredictability and this machine's real source of it is in the kernel;
   what a page needs is different numbers each time it asks. */
static u32 js_rand_state = 0x5A4C5200u;
static jval nat_m_random(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    js_rand_state ^= js_rand_state << 13;
    js_rand_state ^= js_rand_state >> 17;
    js_rand_state ^= js_rand_state << 5;
    return js_num((double)(js_rand_state >> 8) / 16777216.0);
}

/* --- JSON ----------------------------------------------------------------
 *
 * Written into a buffer that grows and made a string once at the end. It was
 * built a character at a time by concatenation, and each concatenation
 * copied everything so far into the page's memory, which never gives any
 * back: seven kilobytes of output used up the whole of it. */

typedef struct {
    char *b;
    u32 n, cap;
    int full;
} jbuf;

static void jb_put(jbuf *o, const char *s, u32 n) {
    if (o->full) return;
    if (o->n + n + 1 > o->cap) {
        u32 cap = o->cap ? o->cap : 256;
        while (cap < o->n + n + 1) cap *= 2;
        /* The same ceiling a page's memory has: past it, the answer is
           refused rather than the machine's heap taken. */
        if (cap > JS_MEM_CAP) { o->full = 1; return; }
        char *nb = (char *)malloc(cap);
        if (!nb) { o->full = 1; return; }
        volatile char *d = nb;
        for (u32 i = 0; i < o->n; i++) d[i] = o->b[i];
        free(o->b);
        o->b = nb;
        o->cap = cap;
    }
    volatile char *d = o->b + o->n;
    for (u32 i = 0; i < n; i++) d[i] = s[i];
    o->n += n;
}

static void jb_str(jbuf *o, const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    jb_put(o, s, n);
}

/* A string, quoted, with everything JSON does not allow raw escaped: the
   quote, the backslash, and every control character, which used to go out
   as they were and made text JSON.parse elsewhere refuses. */
static void js_json_quote(jbuf *o, const jstr *s) {
    static const char HEX[] = "0123456789abcdef";
    jb_put(o, "\"", 1);
    for (u32 i = 0; s && i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        if (c == '"') jb_put(o, "\\\"", 2);
        else if (c == '\\') jb_put(o, "\\\\", 2);
        else if (c == '\n') jb_put(o, "\\n", 2);
        else if (c == '\r') jb_put(o, "\\r", 2);
        else if (c == '\t') jb_put(o, "\\t", 2);
        else if (c == 8) jb_put(o, "\\b", 2);
        else if (c == 12) jb_put(o, "\\f", 2);
        else if (c < 32) {
            char u[6] = { '\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 15] };
            jb_put(o, u, 6);
        } else jb_put(o, (const char *)&s->s[i], 1);
    }
    jb_put(o, "\"", 1);
}

/* What JSON has no way to write, and leaves out of an object or writes as
   null in an array: undefined and functions. */
static int js_json_unwritable(jval v) {
    return v.t == JS_UNDEF
        || (v.t == JS_OBJ && v.obj && (v.obj->kind == JO_FUNC || v.obj->kind == JO_NATIVE));
}

#define JS_JSON_DEPTH 128

/* Writes v; 0 when it cannot be written (a cycle, or nested too deep), with
   the reason thrown. `open` is the objects being written round this one. */
static int js_json_write(jctx *J, jval v, jbuf *o, jobj **open, int depth) {
    switch (v.t) {
        case JS_STR:
            js_json_quote(o, v.str);
            return 1;
        case JS_NUM: {
            /* NaN and the infinities are not numbers JSON has. */
            if (v.num != v.num || v.num - v.num != 0) { jb_str(o, "null"); return 1; }
            jstr *t = js_to_str(J, v);
            if (t) jb_put(o, t->s, t->len);
            return 1;
        }
        case JS_BOOL:
            jb_str(o, v.b ? "true" : "false");
            return 1;
        case JS_OBJ: {
            jobj *ob = v.obj;
            if (!ob) { jb_str(o, "null"); return 1; }
            if (ob->kind == JO_FUNC || ob->kind == JO_NATIVE) { jb_str(o, "null"); return 1; }
            /* An object inside itself has no end to write, and writing it
               anyway recursed until the stack ran out. */
            for (int k = 0; k < depth; k++)
                if (open[k] == ob) {
                    js_throw(J, JS_ERR_TYPE, "JSON cannot write an object that contains itself",
                             J->error_line);
                    return 0;
                }
            if (depth >= JS_JSON_DEPTH) {
                js_throw(J, JS_ERR_RANGE, "JSON nested too deeply to write", J->error_line);
                return 0;
            }
            open[depth] = ob;
            if (ob->kind == JO_ARRAY) {
                jb_put(o, "[", 1);
                for (u32 i = 0; i < ob->len; i++) {
                    if (i) jb_put(o, ",", 1);
                    jval e = ob->items[i];
                    if (js_json_unwritable(e)) jb_str(o, "null");
                    else if (!js_json_write(J, e, o, open, depth + 1)) return 0;
                }
                jb_put(o, "]", 1);
                return 1;
            }
            jb_put(o, "{", 1);
            int first = 1;
            jprop **own;
            u32 nown = js_own_keys(J, ob, &own);
            for (u32 i = 0; i < nown; i++) {
                jprop *p = own[i];
                if (js_json_unwritable(p->v)) continue;
                if (!first) jb_put(o, ",", 1);
                first = 0;
                js_json_quote(o, p->key);
                jb_put(o, ":", 1);
                if (!js_json_write(J, p->v, o, open, depth + 1)) return 0;
            }
            jb_put(o, "}", 1);
            return 1;
        }
        default:
            jb_str(o, "null");
            return 1;
    }
}

static jval nat_json_stringify(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval v = js_arg(a, n, 0);
    if (js_json_unwritable(v)) return js_undef();
    static jobj *open[JS_JSON_DEPTH];
    jbuf o = { 0, 0, 0, 0 };
    int ok = js_json_write(J, v, &o, open, 0);
    jval out = js_undef();
    if (ok && o.full) js_throw(J, JS_ERR_RANGE, "JSON too long to write", J->error_line);
    else if (ok) out = js_from_str(js_str_n(J, o.b ? o.b : "", o.n));
    free(o.b);
    return out;
}

/* --- and read ---------------------------------------------------------------
 *
 * Strictly. It used to take any word beginning with t, f or n for true, false
 * or null, leave \u escapes undone, and answer something for text that was
 * not JSON at all; a page that tries JSON.parse inside try to find out
 * whether it has JSON was told it always had. Now anything that is not JSON
 * is a SyntaxError, as it is everywhere else. */

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
        char c = r->s[at + i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

/* A code point as UTF-8, which is what this engine's strings hold. */
static u32 jr_utf8(u32 cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static jstr *jr_string(jctx *J, jread *r) {
    r->at++;                                   /* the opening quote */
    u32 scan = r->at;
    while (scan < r->len && r->s[scan] != '"') {
        if (r->s[scan] == '\\') scan++;
        else if ((unsigned char)r->s[scan] < 32) { r->bad = 1; return 0; }
        scan++;
    }
    if (scan >= r->len) { r->bad = 1; return 0; }
    /* No escape makes more bytes than it was written in. */
    char *buf = (char *)js_alloc(J, scan - r->at + 1);
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
                if (h < 0) { r->bad = 1; return 0; }
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
                w += jr_utf8(cp, buf + w);
                break;
            }
            default: r->bad = 1; return 0;
        }
    }
    r->at = scan + 1;
    return js_str_n(J, buf, w);
}

static jval jr_value(jctx *J, jread *r) {
    jr_space(r);
    if (r->at >= r->len || r->bad) { r->bad = 1; return js_undef(); }
    char c = r->s[r->at];

    if (c == '{' || c == '[') {
        if (++r->depth > JS_JSON_DEPTH) { r->bad = 1; return js_undef(); }
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
                js_set_prop(J, o, k, v);
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

static jval nat_json_parse(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s) return js_undef();
    jread r = { s->s, s->len, 0, 0, 0 };
    jval v = jr_value(J, &r);
    jr_space(&r);
    if (r.bad || r.at != r.len)
        return js_throw(J, JS_ERR_SYNTAX, "JSON.parse was given text that is not JSON", J->error_line);
    return v;
}

/* --- Object -------------------------------------------------------------- */

static jval nat_obj_keys(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *out = js_array(J);
    jval v = js_arg(a, n, 0);
    if (v.t != JS_OBJ || !v.obj || !out) return js_from_obj(out);
    if (v.obj->kind == JO_ARRAY) {
        char tmp[32];
        for (u32 i = 0; i < v.obj->len; i++) {
            u32 w = js_num_text((double)i, tmp, sizeof(tmp));
            js_arr_push(J, out, js_from_str(js_str_n(J, tmp, w)));
        }
        return js_from_obj(out);
    }
    jprop **own;
    u32 nown = js_own_keys(J, v.obj, &own);
    for (u32 i = 0; i < nown; i++) js_arr_push(J, out, js_from_str(own[i]->key));
    return js_from_obj(out);
}

static jval nat_obj_values(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *out = js_array(J);
    jval v = js_arg(a, n, 0);
    if (v.t != JS_OBJ || !v.obj || !out) return js_from_obj(out);
    if (v.obj->kind == JO_ARRAY) {
        for (u32 i = 0; i < v.obj->len; i++) js_arr_push(J, out, v.obj->items[i]);
        return js_from_obj(out);
    }
    jprop **own;
    u32 nown = js_own_keys(J, v.obj, &own);
    for (u32 i = 0; i < nown; i++) js_arr_push(J, out, own[i]->v);
    return js_from_obj(out);
}

/* --- setting it all up ---------------------------------------------------- */

/* Array, as something a page can name. Array(3) is three empty places and
   Array(1, 2) is two values, which is the one place this constructor is
   surprising and the one a page relies on. */
static jval nat_array_make(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *o = js_array(J);
    if (!o) return js_null();
    if (n == 1 && a[0].t == JS_NUM) {
        double want = a[0].num;
        if (!(want > 0)) return js_from_obj(o);
        if (want > 100000) want = 100000;
        for (u32 i = 0; i < (u32)want; i++) js_arr_set(J, o, i, js_undef());
        return js_from_obj(o);
    }
    for (int i = 0; i < n; i++) js_arr_set(J, o, (u32)i, a[i]);
    return js_from_obj(o);
}

static jval nat_array_is(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t;
    jval v = js_arg(a, n, 0);
    return js_bool(v.t == JS_OBJ && v.obj && v.obj->kind == JO_ARRAY);
}

/* Function exists to be named -- `x instanceof Function` is ordinary -- and
   not to be called: building one out of text is eval by another spelling,
   and that is refused here by name like the rest of it. */
static jval nat_function_make(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    return js_throw(J, JS_ERR_EVAL, "a function built out of text is not here",
                    J->error_line);
}

/* Error and its five kinds, called with or without new: whichever of them
   is being called is the name the error gets. */
static jval nat_error_make(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *ctor = J->callee;
    jstr *name = ctor && ctor->name ? ctor->name : js_str(J, "Error");
    jval m = js_arg(a, n, 0);
    jstr *msg = m.t == JS_UNDEF ? js_str(J, "") : js_to_str(J, m);
    jobj *e = js_error_obj(J, ctor, name, msg);
    return e ? js_from_obj(e) : js_undef();
}

static void js_globals(jctx *J) {
    jscope *g = J->global;

    for (int k = 0; k < 6; k++) {
        J->err_ctor[k] = js_native(J, JS_ERR_NAMES[k], nat_error_make);
        js_declare(J, g, js_str(J, JS_ERR_NAMES[k]), js_from_obj(J->err_ctor[k]));
    }

    jobj *console = js_object(J, JO_PLAIN);
    js_set(J, console, "log", js_from_obj(js_native(J, "log", nat_log)));
    js_set(J, console, "warn", js_from_obj(js_native(J, "warn", nat_log)));
    js_set(J, console, "error", js_from_obj(js_native(J, "error", nat_log)));
    js_declare(J, g, js_str(J, "console"), js_from_obj(console));

    /* For a pattern that is not known until it is built. */
    js_declare(J, g, js_str(J, "RegExp"),
               js_from_obj(js_native(J, "RegExp", nat_regexp_make)));

    jobj *math = js_object(J, JO_PLAIN);
    js_set(J, math, "floor", js_from_obj(js_native(J, "floor", nat_m_floor)));
    js_set(J, math, "ceil",  js_from_obj(js_native(J, "ceil", nat_m_ceil)));
    js_set(J, math, "round", js_from_obj(js_native(J, "round", nat_m_round)));
    js_set(J, math, "abs",   js_from_obj(js_native(J, "abs", nat_m_abs)));
    js_set(J, math, "sqrt",  js_from_obj(js_native(J, "sqrt", nat_m_sqrt)));
    js_set(J, math, "min",   js_from_obj(js_native(J, "min", nat_m_min)));
    js_set(J, math, "max",   js_from_obj(js_native(J, "max", nat_m_max)));
    js_set(J, math, "pow",   js_from_obj(js_native(J, "pow", nat_m_pow)));
    js_set(J, math, "random", js_from_obj(js_native(J, "random", nat_m_random)));
    js_set(J, math, "PI", js_num(3.141592653589793));
    js_set(J, math, "E",  js_num(2.718281828459045));
    js_declare(J, g, js_str(J, "Math"), js_from_obj(math));

    jobj *json = js_object(J, JO_PLAIN);
    js_set(J, json, "stringify",
           js_from_obj(js_native(J, "stringify", nat_json_stringify)));
    js_set(J, json, "parse",
           js_from_obj(js_native(J, "parse", nat_json_parse)));
    js_declare(J, g, js_str(J, "JSON"), js_from_obj(json));

    jobj *object = js_object(J, JO_PLAIN);
    js_set(J, object, "keys", js_from_obj(js_native(J, "keys", nat_obj_keys)));
    js_set(J, object, "values",
           js_from_obj(js_native(J, "values", nat_obj_values)));
    /* Named, so that `instanceof Object` has something to compare with.
       It is an object rather than a function here because nothing calls it. */
    object->name = js_str(J, "Object");
    js_declare(J, g, js_str(J, "Object"), js_from_obj(object));

    jobj *array = js_native(J, "Array", nat_array_make);
    js_set(J, array, "isArray", js_from_obj(js_native(J, "isArray",
                                                      nat_array_is)));
    js_declare(J, g, js_str(J, "Array"), js_from_obj(array));
    js_declare(J, g, js_str(J, "Function"),
               js_from_obj(js_native(J, "Function", nat_function_make)));

    js_declare(J, g, js_str(J, "parseInt"),
               js_from_obj(js_native(J, "parseInt", nat_parseint)));
    js_declare(J, g, js_str(J, "parseFloat"),
               js_from_obj(js_native(J, "parseFloat", nat_parsefloat)));
    js_declare(J, g, js_str(J, "isNaN"),
               js_from_obj(js_native(J, "isNaN", nat_isnan)));
    js_declare(J, g, js_str(J, "String"),
               js_from_obj(js_native(J, "String", nat_str_ctor)));
    js_declare(J, g, js_str(J, "Number"),
               js_from_obj(js_native(J, "Number", nat_num_ctor)));
    js_declare(J, g, js_str(J, "Boolean"),
               js_from_obj(js_native(J, "Boolean", nat_bool_ctor)));

    {
        double z = 0.0;
        js_declare(J, g, js_str(J, "NaN"), js_num(z / z));
        js_declare(J, g, js_str(J, "Infinity"), js_num(1e308 * 10));
    }
}

/* --- the way in ---------------------------------------------------------- */

static void js_init(jctx *J) {
    memset(J, 0, (int)sizeof(*J));
    J->sig = JS_OK;
    J->s_this = js_str(J, "__this__");
    J->s_fn = js_str(J, "__fn__");
    J->s_ctor = js_str(J, "__ctor__");
    J->s_bound = js_str(J, "__bound__");
    J->s_args = js_str(J, "__args__");
    J->s_arguments = js_str(J, "arguments");
    J->global = js_scope(J, 0);
    if (!J->global) return;
    J->global->escaped = 1;                   /* never given back */
    J->global_obj = J->global->vars;
    js_globals(J);
}

static void js_done(jctx *J) {
    if (J->nodes) free(J->nodes);
    J->nodes = 0;
    J->nnodes = J->ncap = 0;
    js_free_all(J);
}

/* Runs a script. Returns 1 when it finished, 0 when it did not, with the
   reason in J->error and the line in J->error_line either way. */
static int js_run(jctx *J, const char *src, u32 len) {
    J->sig = JS_OK;
    J->steps = 0;
    J->error[0] = 0;

    int prog = js_parse(J, src, len);
    if (prog < 0 || J->sig == JS_FAILED) return 0;

    /* At the top of a script `this` is the global object. It was undefined,
       so the wrapper nearly every library ships in -- (function(root){
       root.lib = ...; })(this) -- stopped at its first line with "cannot set
       lib of undefined". */
    js_exec(J, prog, J->global, js_from_obj(J->global->vars));

    if (J->sig == JS_THROWN) return 0;
    if (J->sig == JS_FAILED) return 0;
    J->sig = JS_OK;
    return 1;
}

/* And evaluates one expression, for anything that wants a value back: an
   event handler written in an attribute, or a test. */
static int js_eval_text(jctx *J, const char *src, u32 len, jval *out) {
    J->sig = JS_OK;
    J->steps = 0;
    J->error[0] = 0;

    jparse P;
    P.J = J;
    P.no_in = 0;
    P.L.J = J;
    P.L.src = src;
    P.L.n = len;
    P.L.at = 0;
    P.L.line = 1;
    P.L.failed = 0;
    js_next(&P.L);

    int e = js_parse_expr(&P);
    if (e < 0 || P.L.failed || J->sig == JS_FAILED) return 0;
    jval v = js_eval(J, e, J->global, js_undef());
    if (J->sig == JS_THROWN || J->sig == JS_FAILED) return 0;
    J->sig = JS_OK;
    if (out) *out = v;
    return 1;
}
