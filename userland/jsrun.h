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
#include "jsparse.h"
#include "jsregex.h"

static jval js_eval(jctx *J, int node, jscope *sc, jval this_val);
static jsignal js_exec(jctx *J, int node, jscope *sc, jval this_val);

/* --- saying what went wrong ---------------------------------------------- */

static jval js_throw(jctx *J, const char *what, int line) {
    if (J->sig == JS_FAILED) return js_undef();
    J->sig = JS_THROWN;
    J->error_line = line;
    int i = 0;
    while (what[i] && i < (int)sizeof(J->error) - 1) { J->error[i] = what[i]; i++; }
    J->error[i] = 0;
    J->ret = js_from_str(js_str(J, what));
    return js_undef();
}

/* --- numbers as text -----------------------------------------------------
 *
 * The part of this language that is hardest to do exactly. The standard asks
 * for the shortest decimal that reads back as the same double, which needs
 * arbitrary precision arithmetic to get right in every case.
 *
 * What is here is the practical version: whole numbers inside the range a
 * double represents exactly are printed as integers, and everything else
 * gets up to ten significant places with the trailing zeros removed. That is
 * exact for every integer a page is likely to hold and close for the rest,
 * and where it differs it differs in the last place rather than the first.
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
    if (d > 1.7e308 || d < -1.7e308) {
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

    /* Very large or very small, where a plain decimal would be all zeros. */
    if (d >= 1e21 || (d > 0 && d < 1e-6)) {
        int e = 0;
        while (d >= 10.0) { d /= 10.0; e++; }
        while (d < 1.0)   { d *= 10.0; e--; }
        if (neg) out[w++] = '-';
        long long lead = (long long)d;
        out[w++] = (char)('0' + (int)lead);
        d -= (double)lead;
        if (d > 1e-10) {
            out[w++] = '.';
            for (int i = 0; i < 9 && d > 1e-10; i++) {
                d *= 10.0;
                int dig = (int)d;
                out[w++] = (char)('0' + dig);
                d -= (double)dig;
            }
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

    if (neg) out[w++] = '-';
    long long whole = (long long)d;
    double frac = d - (double)whole;
    char rev[24];
    int r = 0;
    if (!whole) rev[r++] = '0';
    long long t = whole;
    while (t) { rev[r++] = (char)('0' + (int)(t % 10)); t /= 10; }
    while (r) out[w++] = rev[--r];

    if (frac > 0) {
        u32 dot = w;
        out[w++] = '.';
        for (int i = 0; i < 10; i++) {
            frac *= 10.0;
            int dig = (int)frac;
            out[w++] = (char)('0' + dig);
            frac -= (double)dig;
        }
        while (w > dot + 1 && out[w - 1] == '0') w--;
        if (w == dot + 1) w = dot;
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

static double js_str_to_num(const char *s, u32 n) {
    u32 i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i++;
    if (i >= n) return 0;                       /* an empty string is zero */

    int neg = 0;
    if (s[i] == '-') { neg = 1; i++; }
    else if (s[i] == '+') i++;

    int any = 0;
    double v = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        v = v * 10.0 + (double)(s[i] - '0');
        i++;
        any = 1;
    }
    if (i < n && s[i] == '.') {
        i++;
        double scale = 0.1;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            v += (double)(s[i] - '0') * scale;
            scale *= 0.1;
            i++;
            any = 1;
        }
    }
    if (any && i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        int eneg = 0;
        if (i < n && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
        int e = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') e = e * 10 + (s[i++] - '0');
        double p = 1.0;
        for (int k = 0; k < e; k++) p *= 10.0;
        v = eneg ? v / p : v * p;
    }
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i++;

    /* Trailing rubbish makes the whole thing not a number, which is the rule
       and is why "12px" is NaN while parseInt("12px") is 12. */
    if (!any || i != n) {
        double zero = 0.0;
        return zero / zero;
    }
    return neg ? -v : v;
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
            if (o->kind == JO_ARRAY) {
                /* Joined with commas, which is what an array becomes when
                   something asks it for text. */
                u32 total = 0;
                for (u32 i = 0; i < o->len; i++) {
                    jstr *p = js_to_str(J, o->items[i]);
                    total += (p ? p->len : 0) + 1;
                }
                jstr *out = (jstr *)js_alloc(J, (u32)sizeof(jstr) + total + 1);
                if (!out) return js_str(J, "");
                u32 w = 0;
                for (u32 i = 0; i < o->len; i++) {
                    if (i) out->s[w++] = ',';
                    jval e = o->items[i];
                    if (e.t == JS_UNDEF || e.t == JS_NULL) continue;
                    jstr *p = js_to_str(J, e);
                    for (u32 k = 0; p && k < p->len; k++) out->s[w++] = p->s[k];
                }
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
    return js_throw(J, msg, J->error_line);
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
        return js_throw(J, "this is not a function", J->error_line);

    if (J->depth >= JS_DEPTH_CAP)
        return js_throw(J, "too many nested calls", J->error_line);

    jobj *f = fn.obj;
    if (f->kind == JO_NATIVE) return f->fn(J, this_val, argv, argc);

    /* An arrow function carries the receiver it was written under. */
    {
        jprop *lex = js_find(f, js_str(J, "__this__"));
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

    /* `arguments`, as an array, because pages use it. */
    {
        jobj *a = js_array(J);
        for (int k = 0; k < argc; k++) js_arr_push(J, a, argv[k]);
        js_declare(J, sc, js_str(J, "arguments"), js_from_obj(a));
    }

    J->depth++;
    jsignal s = js_exec(J, f->body, sc, this_val);
    J->depth--;

    if (s == JS_RETURN) {
        J->sig = JS_OK;
        jval r = J->ret;
        J->ret = js_undef();
        return r;
    }
    if (s == JS_THROWN || s == JS_FAILED) return js_undef();
    return js_undef();
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

/* The receiver of a string method is carried on the native object, because
   a method fetched off a value has to remember which value it came from. */
static jval js_bound_this;

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
        js_throw(J, js_rx.why[0] ? js_rx.why : "a pattern this cannot read",
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

static jval nat_str_indexof(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_to_str(J, t), *nd = js_to_str(J, js_arg(a, n, 0));
    return js_num((double)js_find_sub(h, nd, 0));
}

static jval nat_str_includes(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_to_str(J, t), *nd = js_to_str(J, js_arg(a, n, 0));
    return js_bool(js_find_sub(h, nd, 0) >= 0);
}

static jval nat_str_startswith(jctx *J, jval t, jval *a, int n) {
    jstr *h = js_to_str(J, t), *nd = js_to_str(J, js_arg(a, n, 0));
    if (!h || !nd || nd->len > h->len) return js_bool(0);
    for (u32 i = 0; i < nd->len; i++) if (h->s[i] != nd->s[i]) return js_bool(0);
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

static jval nat_str_substring(jctx *J, jval t, jval *a, int n) {
    return nat_str_slice(J, t, a, n);
}

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

static jval nat_str_trim(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jstr *s = js_to_str(J, t);
    if (!s) return js_from_str(js_str(J, ""));
    u32 from = 0, to = s->len;
    while (from < to && (s->s[from] == ' ' || s->s[from] == '\t'
                         || s->s[from] == '\n' || s->s[from] == '\r')) from++;
    while (to > from && (s->s[to - 1] == ' ' || s->s[to - 1] == '\t'
                         || s->s[to - 1] == '\n' || s->s[to - 1] == '\r')) to--;
    return js_from_str(js_str_n(J, s->s + from, to - from));
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

static jval nat_str_replace(jctx *J, jval t, jval *a, int n) {
    if (js_is_regex(js_arg(a, n, 0))) return nat_str_replace_re(J, t, a, n);
    jstr *s = js_to_str(J, t);
    jstr *find = js_to_str(J, js_arg(a, n, 0));
    jstr *with = js_to_str(J, js_arg(a, n, 1));
    int hit = js_find_sub(s, find, 0);
    if (hit < 0 || !s) return js_from_str(s);
    jstr *head = js_str_n(J, s->s, (u32)hit);
    jstr *tail = js_str_n(J, s->s + hit + find->len,
                          s->len - (u32)hit - find->len);
    return js_from_str(js_concat(J, js_concat(J, head, with), tail));
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
    js_bound_this = target;
    struct { const char *n; jnative f; } M[] = {
        { "charAt", nat_str_charat }, { "charCodeAt", nat_str_charcode },
        { "indexOf", nat_str_indexof }, { "includes", nat_str_includes },
        { "startsWith", nat_str_startswith },
        { "slice", nat_str_slice }, { "substring", nat_str_substring },
        { "substr", nat_str_slice },
        { "toUpperCase", nat_str_upper }, { "toLowerCase", nat_str_lower },
        { "trim", nat_str_trim }, { "split", nat_str_split },
        { "replace", nat_str_replace }, { "replaceAll", nat_str_replace },
        { "repeat", nat_str_repeat },
        { "match", nat_str_match }, { "search", nat_str_search },
        { 0, 0 }
    };
    for (int i = 0; M[i].n; i++) {
        if (!js_str_is(name, M[i].n)) continue;
        jobj *o = js_native(J, M[i].n, M[i].f);
        if (!o) return js_undef();
        /* The receiver travels with the method. Fetching one and calling it
           later on something else is not something a page does, and doing it
           this way costs one field instead of a bound-function object. */
        js_set(J, o, "__this__", target);
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
        jobj *o = js_native(J, M[i].n, M[i].f);
        if (!o) return js_undef();
        js_set(J, o, "__this__", target);
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
                return js_throw(J, "the right of instanceof is not a "
                                   "constructor", line);

            jprop *made_by = js_find(l.obj, js_str(J, "__ctor__"));
            if (made_by && made_by->v.t == JS_OBJ && made_by->v.obj == r.obj)
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
            return js_throw(J, "this operator is not one this engine has", line);
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
            f->closure = sc;
            f->name = n->str;
            /* An arrow takes `this` from where it was written rather than
               from wherever it is later called, so it is caught here, at
               the moment the function value is made. */
            if (n->op) js_set(J, f, "__this__", this_val);
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
                return js_throw(J, msg, n->line);
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
                    return js_throw(J, "cannot call a method on nothing", n->line);
                fn = js_get(J, self, name);
            } else {
                fn = js_eval(J, callee, sc, this_val);
            }
            if (J->sig != JS_OK) return js_undef();

            /* A method carries the value it was fetched from, so a native
               that was bound gets its receiver back. */
            if (fn.t == JS_OBJ && fn.obj && fn.obj->kind == JO_NATIVE) {
                jprop *b = js_find(fn.obj, js_str(J, "__this__"));
                if (b) self = b->v;
            }

            jval argv[JS_ARGS_MAX];
            int argc = 0;
            for (int cell = n->b; cell >= 0 && argc < JS_ARGS_MAX;
                 cell = J->nodes[cell].b) {
                argv[argc++] = js_eval(J, J->nodes[cell].a, sc, this_val);
                if (J->sig != JS_OK) return js_undef();
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
            if (fn.t == JS_OBJ && fn.obj) js_set(J, fresh, "__ctor__", fn);
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
            if (p.kind == 2) return js_throw(J, "this cannot be assigned to",
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
            for (u32 b = 0; b < o->nbuckets; b++)
                for (jprop *p = o->buckets[b]; p; p = p->next)
                    if (p->enumerable) js_arr_push(J, keys, js_from_str(p->key));

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

        case N_RETURN:
            J->ret = n->a >= 0 ? js_eval(J, n->a, sc, this_val) : js_undef();
            if (J->sig != JS_OK) return J->sig;
            J->sig = JS_RETURN;
            return JS_RETURN;

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
    u32 end = 0;
    while (end < s->len) {
        char c = s->s[end];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+'
            || c == 'e' || c == 'E') end++;
        else break;
    }
    if (!end) { double z = 0.0; return js_num(z / z); }
    return js_num(js_str_to_num(s->s, end));
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

/* --- JSON ---------------------------------------------------------------- */

static void js_json_write(jctx *J, jval v, jstr **out) {
    switch (v.t) {
        case JS_STR: {
            *out = js_concat(J, *out, js_str(J, "\""));
            jstr *s = v.str;
            for (u32 i = 0; s && i < s->len; i++) {
                char c = s->s[i];
                if (c == '"' || c == '\\') {
                    char esc[3];
                    esc[0] = '\\'; esc[1] = c; esc[2] = 0;
                    *out = js_concat(J, *out, js_str(J, esc));
                } else if (c == '\n') *out = js_concat(J, *out, js_str(J, "\\n"));
                else if (c == '\t') *out = js_concat(J, *out, js_str(J, "\\t"));
                else *out = js_concat(J, *out, js_str_n(J, &c, 1));
            }
            *out = js_concat(J, *out, js_str(J, "\""));
            return;
        }
        case JS_OBJ: {
            jobj *o = v.obj;
            if (!o) { *out = js_concat(J, *out, js_str(J, "null")); return; }
            if (o->kind == JO_FUNC || o->kind == JO_NATIVE) {
                *out = js_concat(J, *out, js_str(J, "null"));
                return;
            }
            if (o->kind == JO_ARRAY) {
                *out = js_concat(J, *out, js_str(J, "["));
                for (u32 i = 0; i < o->len; i++) {
                    if (i) *out = js_concat(J, *out, js_str(J, ","));
                    js_json_write(J, o->items[i], out);
                }
                *out = js_concat(J, *out, js_str(J, "]"));
                return;
            }
            *out = js_concat(J, *out, js_str(J, "{"));
            int first = 1;
            for (u32 b = 0; b < o->nbuckets; b++) {
                for (jprop *p = o->buckets[b]; p; p = p->next) {
                    if (!first) *out = js_concat(J, *out, js_str(J, ","));
                    first = 0;
                    js_json_write(J, js_from_str(p->key), out);
                    *out = js_concat(J, *out, js_str(J, ":"));
                    js_json_write(J, p->v, out);
                }
            }
            *out = js_concat(J, *out, js_str(J, "}"));
            return;
        }
        default:
            *out = js_concat(J, *out, js_to_str(J, v));
            return;
    }
}

static jval nat_json_stringify(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *out = js_str(J, "");
    js_json_write(J, js_arg(a, n, 0), &out);
    return js_from_str(out);
}

static jval js_json_read(jctx *J, const char *s, u32 len, u32 *at);

static void js_json_space(const char *s, u32 len, u32 *at) {
    while (*at < len && (s[*at] == ' ' || s[*at] == '\t' || s[*at] == '\n'
                         || s[*at] == '\r')) (*at)++;
}

static jval js_json_read(jctx *J, const char *s, u32 len, u32 *at) {
    js_json_space(s, len, at);
    if (*at >= len) return js_undef();
    char c = s[*at];

    if (c == '{') {
        (*at)++;
        jobj *o = js_object(J, JO_PLAIN);
        js_json_space(s, len, at);
        if (*at < len && s[*at] == '}') { (*at)++; return js_from_obj(o); }
        for (;;) {
            js_json_space(s, len, at);
            jval k = js_json_read(J, s, len, at);
            js_json_space(s, len, at);
            if (*at < len && s[*at] == ':') (*at)++;
            jval v = js_json_read(J, s, len, at);
            js_set_prop(J, o, js_to_str(J, k), v);
            js_json_space(s, len, at);
            if (*at < len && s[*at] == ',') { (*at)++; continue; }
            if (*at < len && s[*at] == '}') (*at)++;
            break;
        }
        return js_from_obj(o);
    }

    if (c == '[') {
        (*at)++;
        jobj *a = js_array(J);
        js_json_space(s, len, at);
        if (*at < len && s[*at] == ']') { (*at)++; return js_from_obj(a); }
        for (;;) {
            js_arr_push(J, a, js_json_read(J, s, len, at));
            js_json_space(s, len, at);
            if (*at < len && s[*at] == ',') { (*at)++; continue; }
            if (*at < len && s[*at] == ']') (*at)++;
            break;
        }
        return js_from_obj(a);
    }

    if (c == '"') {
        (*at)++;
        u32 start = *at;
        u32 count = 0;
        u32 scan = *at;
        while (scan < len && s[scan] != '"') {
            if (s[scan] == '\\') scan++;
            scan++;
            count++;
        }
        char *buf = (char *)js_alloc(J, count + 1);
        if (!buf) return js_undef();
        u32 w = 0, i = start;
        while (i < len && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < len) {
                i++;
                switch (s[i]) {
                    case 'n': buf[w++] = '\n'; break;
                    case 't': buf[w++] = '\t'; break;
                    case 'r': buf[w++] = '\r'; break;
                    default:  buf[w++] = s[i]; break;
                }
                i++;
            } else buf[w++] = s[i++];
        }
        buf[w] = 0;
        *at = i + 1;
        return js_from_str(js_str_n(J, buf, w));
    }

    if (c == 't' && *at + 3 < len) { *at += 4; return js_bool(1); }
    if (c == 'f' && *at + 4 < len) { *at += 5; return js_bool(0); }
    if (c == 'n' && *at + 3 < len) { *at += 4; return js_null(); }

    u32 start = *at;
    while (*at < len) {
        char d = s[*at];
        if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.'
            || d == 'e' || d == 'E') (*at)++;
        else break;
    }
    if (*at == start) { (*at)++; return js_undef(); }
    return js_num(js_str_to_num(s + start, *at - start));
}

static jval nat_json_parse(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s) return js_undef();
    u32 at = 0;
    return js_json_read(J, s->s, s->len, &at);
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
    for (u32 b = 0; b < v.obj->nbuckets; b++)
        for (jprop *p = v.obj->buckets[b]; p; p = p->next)
            js_arr_push(J, out, js_from_str(p->key));
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
    for (u32 b = 0; b < v.obj->nbuckets; b++)
        for (jprop *p = v.obj->buckets[b]; p; p = p->next)
            js_arr_push(J, out, p->v);
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
    return js_throw(J, "a function built out of text is not here",
                    J->error_line);
}

static void js_globals(jctx *J) {
    jscope *g = J->global;

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
    J->global = js_scope(J, 0);
    if (!J->global) return;
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

    js_exec(J, prog, J->global, js_undef());

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
