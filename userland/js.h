/* JavaScript: the values, the reader, and the machine that runs it.
 *
 * Written here like everything else. There is no library underneath this and
 * no borrowed grammar; the lexer, the parser and the interpreter are all in
 * this file and the one that follows it.
 *
 * Two things this could not have been built on top of before now. A number
 * in JavaScript is a double, and until the floating point unit was turned on
 * this machine had no doubles at all — an engine written against integers
 * would have been wrong about every fraction and wrong quietly. And a script
 * allocates: objects, strings, arrays, closures, none of them of a size
 * anybody knows in advance, which is impossible in a program built out of
 * fixed arrays. Both of those are recent and both were built for this.
 *
 * --- what is here ---------------------------------------------------------
 *
 * var, let and const; functions as declarations, as expressions and as
 * values, with closures; if, while, do, for, for-in, break, continue,
 * return; objects and arrays with literals; the operators, including the
 * ternary, the logical ones with their short circuits, the bitwise ones with
 * their thirty-two bit truncation, typeof and delete; try, catch, throw and
 * finally; and the built-in methods of String, Array, Object, Math and JSON
 * that ordinary page scripts actually call.
 *
 * --- what is not ----------------------------------------------------------
 *
 * No prototype chain that a script can reach into: objects have one
 * internally, and Object.create and the prototype property are not exposed,
 * because half a prototype system is worse than none. No classes, no
 * generators, no async, no await, no Promise, no regular expressions, no
 * modules, no getters and setters, no Proxy, no symbols. No eval.
 *
 * Those are absent rather than approximated. An engine that accepts `class`
 * and does something slightly wrong with it is worse than one that says it
 * does not know the word, because the first produces a page that is subtly
 * broken and the second produces a message.
 *
 * --- memory ---------------------------------------------------------------
 *
 * Everything a script allocates comes from a region, and the whole region
 * goes when the script's world is torn down, which for a browser is when the
 * page is left. There is no garbage collector.
 *
 * That is a decision and not an omission. A collector needs to know every
 * live value, and in a tree-walking interpreter half of them are in local
 * variables of the C functions doing the walking; getting that wrong gives
 * you an object collected while something is still using it, which is the
 * worst class of bug there is and it appears at random. A region has none of
 * that: nothing is ever freed early because nothing is ever freed at all.
 *
 * What it costs is a script that allocates in a loop forever. There is a cap,
 * and reaching it stops the script and says so, rather than taking the
 * machine down with it.
 */
#pragma once
#include "zelr.h"
#include "alloc.h"

/* How much one page's scripts may have at once, and how long they may run.
   Both are refusals with a message rather than limits that corrupt. */
#define JS_MEM_CAP   (24u * 1024 * 1024)
#define JS_MEM_SPARE (1u * 1024 * 1024)
#define JS_STEP_CAP  40000000u
#define JS_DEPTH_CAP 160

/* --- values --------------------------------------------------------------
 *
 * Eight kinds, in a tagged union. A number is a double because that is what
 * a number is in this language: there is no integer type for a script to
 * ask for, and the whole of the arithmetic depends on 1/3 being a third. */

typedef enum {
    JS_UNDEF = 0, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ
} jtype;

typedef struct jstr jstr;
typedef struct jobj jobj;

typedef struct {
    jtype t;
    union {
        double num;
        int    b;
        jstr  *str;
        jobj  *obj;
    };
} jval;

/* A string: length, hash and the bytes, all in one allocation. Interned
   lengths mean comparing two strings is a length test and a memcmp, and the
   hash means a property lookup does not have to do even that most of the
   time. */
struct jstr {
    u32 len;
    u32 hash;
    char s[1];                /* len + 1, allocated with the header */
};

typedef struct jprop {
    jstr *key;
    jval  v;
    struct jprop *next;       /* in its bucket */
    struct jprop *onext;      /* in the order the properties were made */
    int   enumerable;
} jprop;

typedef enum {
    JO_PLAIN = 0, JO_ARRAY, JO_FUNC, JO_NATIVE, JO_REGEX
} jokind;

struct jctx;
typedef jval (*jnative)(struct jctx *J, jval this_val, jval *argv, int argc);

typedef struct jscope jscope;

struct jobj {
    jokind kind;
    jprop **buckets;
    u32     nbuckets, count;

    /* The same properties again, in the order they were made, which is the
       order a script walking them is owed. Walked by bucket they came out
       in hash order, so {x, y, z} listed its keys as y, z, x. */
    jprop  *ofirst, *olast;

    /* Arrays keep their elements out of the property table, because a page
       that walks a thousand element array through a hash of decimal strings
       is a page that takes a second to do nothing. */
    jval   *items;
    u32     len, cap;

    /* Functions. `body` and `params` are nodes in the tree; `closure` is the
       scope the function was made in, which is the whole of what a closure
       is. */
    int     body, params, nparams;
    int     uses_args;        /* its text names `arguments` */
    jscope *closure;
    jnative fn;
    jstr   *name;

    /* Anything the host wants to hang off this object: for the browser, the
       index of the element it stands for. */
    int     host;
};

/* --- the region ---------------------------------------------------------- */

typedef struct jchunk {
    struct jchunk *next;
    u32 used, size;
    u8  data[1];
} jchunk;

/* --- scopes --------------------------------------------------------------
 *
 * A chain of them. A name is looked for here, then in the parent, and so on
 * to the global one; a function carries the scope it was made in, so a
 * closure is not a special case but the ordinary rule applied to a scope
 * that outlived the call that made it. */
struct jscope {
    jobj   *vars;
    jscope *parent;

    /* Whether anything can reach this scope once the call that made it is
       over: set, on it and on every scope it sits in, when a function value
       is made inside it. One that is never set is given back at the end of
       its call. */
    int     escaped;
};

/* --- what the reader produces -------------------------------------------- */

typedef enum {
    N_NONE = 0,
    /* expressions */
    N_NUM, N_STR, N_REGEX, N_TRUE, N_FALSE, N_NULL, N_UNDEF, N_IDENT,
    N_ARRAY, N_OBJECT, N_FUNC, N_CALL, N_NEW, N_MEMBER, N_INDEX,
    N_UNARY, N_BINARY, N_LOGICAL, N_ASSIGN, N_COND, N_SEQ,
    N_PREINC, N_POSTINC, N_TYPEOF, N_DELETE, N_THIS,
    /* statements */
    N_VAR, N_BLOCK, N_IF, N_WHILE, N_DO, N_FOR, N_FORIN,
    N_RETURN, N_BREAK, N_CONTINUE, N_EXPRSTMT, N_FUNCDECL, N_EMPTY,
    N_THROW, N_TRY, N_SWITCH, N_CASE, N_LABEL
} ntype;

typedef struct {
    ntype kind;
    int   a, b, c, d;         /* children, or -1 */
    double num;
    jstr *str;
    int   op;                 /* for the operator nodes */
    int   line;
} jnode;

/* --- the machine --------------------------------------------------------- */

typedef enum {
    JS_OK = 0, JS_RETURN, JS_BREAK, JS_CONTINUE, JS_THROWN, JS_FAILED
} jsignal;

typedef struct jctx {
    /* the tree */
    jnode *nodes;
    int    nnodes, ncap;

    /* Where a break or continue is aimed, and the name about to be given to
       the statement being entered. A loop that is not the one named has to
       pass the signal on rather than swallowing it, which is the whole of
       what a label is for. */
    jstr  *label;
    jstr  *pending_label;

    /* the region, and what has been given back to it: a list for each size
       up to 256 bytes, in steps of sixteen */
    jchunk *chunks;
    u32     allocated;
    void   *free_list[16];

    jscope *global;
    jobj   *global_obj;

    /* what is happening */
    jsignal sig;
    jval    ret;              /* the value of a return, or what was thrown */
    u32     steps;
    int     depth;
    char    error[192];
    int     error_line;

    /* The native being called, for the length of the call into it. A native
       is handed `this` and its arguments and nothing that says which object
       it is, which is all most of them need; a bound function is the one
       that has to find what it was bound to, and that lives on itself. Read
       it first thing: any call the native makes changes it. */
    jobj   *callee;

    /* The error constructors as the engine made them, for the errors it
       raises itself: a script that assigns over TypeError does not change
       what reading a property of null throws. In the order of JS_ERR_*. */
    jobj   *err_ctor[6];

    /* Names the engine looks up on every call, made once. Each lookup used
       to make its own copy of the string, and a page that called a function
       in a loop ran out of memory on the copies. */
    jstr   *s_this, *s_fn, *s_ctor, *s_bound, *s_args, *s_arguments;

    /* The string and array methods, one native each, for a method called
       where it is fetched -- s.charAt(i) -- which gets its receiver from the
       call. Set while the call fetches it. */
    jobj   *str_methods[24], *arr_methods[16];
    int     for_call;

    /* the host's hook: how a property on a host object is read and written,
       and what happens when one is called. Null in a program that has no
       host objects, which is how this file is testable on its own. */
    int  (*host_get)(struct jctx *J, jobj *o, const char *name, jval *out);
    int  (*host_set)(struct jctx *J, jobj *o, const char *name, jval v);
    void *host_data;
} jctx;

/* --- allocation ---------------------------------------------------------- */

#define JS_CHUNK (256u * 1024u)

static void *js_alloc(jctx *J, u32 n) {
    n = (n + 15u) & ~15u;
    if (n && n <= 256) {
        void **slot = &J->free_list[n / 16 - 1];
        if (*slot) {
            void *p = *slot;
            *slot = *(void **)p;
            memset(p, 0, (int)n);
            return p;
        }
    }
    if (J->allocated + n > JS_MEM_CAP) {
        if (J->sig != JS_FAILED) {
            J->sig = JS_FAILED;
            const char *m = "this script asked for more memory than a page is "
                            "allowed";
            int i = 0;
            while (m[i] && i < (int)sizeof(J->error) - 1) {
                J->error[i] = m[i];
                i++;
            }
            J->error[i] = 0;
        }
        /* The script is stopped from here, but the C already on its way
           through a statement is not: it asked for a string or a property and
           a great many places use what they were given without asking whether
           they were given anything, and a NULL there took the whole browser
           down. So what is already running is given a little more, from a
           spare that nothing reaches but this, to get back out; the page's
           handlers and timers do not run again (jsdom.h, jd_spent). */
        if (J->allocated + n > JS_MEM_CAP + JS_MEM_SPARE) return 0;
    }

    jchunk *c = J->chunks;
    if (!c || c->used + n > c->size) {
        u32 want = n > JS_CHUNK ? n : JS_CHUNK;
        jchunk *fresh = (jchunk *)malloc(sizeof(jchunk) + want);
        if (!fresh) {
            J->sig = JS_FAILED;
            return 0;
        }
        fresh->next = J->chunks;
        fresh->used = 0;
        fresh->size = want;
        J->chunks = fresh;
        c = fresh;
    }
    void *p = c->data + c->used;
    c->used += n;
    J->allocated += n;
    memset(p, 0, (int)n);
    return p;
}

/* Gives a block back, for the next js_alloc of the same size. Only for a
   block nothing can reach any more: the region never had to know, and what
   is given back here is used again. */
static void js_free(jctx *J, void *p, u32 n) {
    n = (n + 15u) & ~15u;
    if (!p || !n || n > 256) return;
    void **slot = &J->free_list[n / 16 - 1];
    *(void **)p = *slot;
    *slot = p;
}

static void js_free_all(jctx *J) {
    jchunk *c = J->chunks;
    while (c) {
        jchunk *nxt = c->next;
        free(c);
        c = nxt;
    }
    J->chunks = 0;
    J->allocated = 0;
    for (int i = 0; i < 16; i++) J->free_list[i] = 0;
}

/* --- strings ------------------------------------------------------------- */

static u32 js_hash(const char *s, u32 n) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 16777619u; }
    return h;
}

static jstr *js_str_n(jctx *J, const char *s, u32 n) {
    jstr *r = (jstr *)js_alloc(J, (u32)sizeof(jstr) + n + 1);
    if (!r) return 0;
    r->len = n;
    for (u32 i = 0; i < n; i++) r->s[i] = s[i];
    r->s[n] = 0;
    r->hash = js_hash(s, n);
    return r;
}

static jstr *js_str(jctx *J, const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    return js_str_n(J, s, n);
}

static int js_str_eq(const jstr *a, const jstr *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->len != b->len || a->hash != b->hash) return 0;
    for (u32 i = 0; i < a->len; i++) if (a->s[i] != b->s[i]) return 0;
    return 1;
}

static int js_str_is(const jstr *a, const char *b) {
    if (!a) return 0;
    u32 i = 0;
    for (; i < a->len; i++) if (a->s[i] != b[i] || !b[i]) return 0;
    return b[i] == 0;
}

/* --- making values ------------------------------------------------------- */

static inline jval js_undef(void) { jval v; v.t = JS_UNDEF; v.num = 0; return v; }
static inline jval js_null(void)  { jval v; v.t = JS_NULL;  v.num = 0; return v; }
static inline jval js_bool(int b) { jval v; v.t = JS_BOOL;  v.b = !!b; return v; }
static inline jval js_num(double d) { jval v; v.t = JS_NUM; v.num = d; return v; }

static inline jval js_from_str(jstr *s) {
    jval v;
    v.t = JS_STR;
    v.str = s;
    return v;
}

static inline jval js_from_obj(jobj *o) {
    jval v;
    v.t = JS_OBJ;
    v.obj = o;
    return v;
}

/* --- objects ------------------------------------------------------------- */

#define JS_BUCKETS 8

static jobj *js_object(jctx *J, jokind kind) {
    jobj *o = (jobj *)js_alloc(J, (u32)sizeof(jobj));
    if (!o) return 0;
    o->kind = kind;
    o->nbuckets = JS_BUCKETS;
    o->buckets = (jprop **)js_alloc(J, (u32)sizeof(jprop *) * JS_BUCKETS);
    if (!o->buckets) return 0;
    o->host = -1;
    o->body = o->params = -1;
    return o;
}

static jprop *js_find(jobj *o, const jstr *key) {
    if (!o || !o->buckets || !key) return 0;
    u32 i = key->hash & (o->nbuckets - 1);
    for (jprop *p = o->buckets[i]; p; p = p->next)
        if (js_str_eq(p->key, key)) return p;
    return 0;
}

/* Grows the table when it is more than three quarters full. A hash with
   every key in one bucket is a list, and a page with a few hundred
   properties on one object is ordinary. */
static void js_rehash(jctx *J, jobj *o) {
    u32 want = o->nbuckets * 2;
    jprop **fresh = (jprop **)js_alloc(J, (u32)sizeof(jprop *) * want);
    if (!fresh) return;
    for (u32 i = 0; i < o->nbuckets; i++) {
        jprop *p = o->buckets[i];
        while (p) {
            jprop *nxt = p->next;
            u32 k = p->key->hash & (want - 1);
            p->next = fresh[k];
            fresh[k] = p;
            p = nxt;
        }
    }
    js_free(J, o->buckets, (u32)sizeof(jprop *) * o->nbuckets);
    o->buckets = fresh;
    o->nbuckets = want;
}

static jprop *js_put_prop(jctx *J, jobj *o, jstr *key, jval v) {
    if (!o || !key) return 0;
    jprop *p = js_find(o, key);
    if (p) { p->v = v; return p; }

    if (o->count + 1 > (o->nbuckets * 3) / 4) js_rehash(J, o);

    p = (jprop *)js_alloc(J, (u32)sizeof(jprop));
    if (!p) return 0;
    p->key = key;
    p->v = v;
    p->enumerable = 1;
    u32 i = key->hash & (o->nbuckets - 1);
    p->next = o->buckets[i];
    o->buckets[i] = p;
    if (o->olast) o->olast->onext = p;
    else o->ofirst = p;
    o->olast = p;
    o->count++;
    return p;
}

static void js_set_prop(jctx *J, jobj *o, jstr *key, jval v) {
    js_put_prop(J, o, key, v);
}

static void js_set(jctx *J, jobj *o, const char *name, jval v) {
    js_set_prop(J, o, js_str(J, name), v);
}

/* A property the engine keeps for itself: what made an object, what a bound
   function is bound to. A script can still read it by name, but walking the
   object's keys does not turn it up, and neither does JSON. They were
   ordinary properties, and Object.keys(new Thing()) listed __ctor__. */
static void js_set_hidden(jctx *J, jobj *o, const char *name, jval v) {
    jprop *p = js_put_prop(J, o, js_str(J, name), v);
    if (p) p->enumerable = 0;
}

static jval js_get_prop(jobj *o, const jstr *key) {
    jprop *p = js_find(o, key);
    return p ? p->v : js_undef();
}

static int js_delete_prop(jobj *o, const jstr *key) {
    if (!o || !o->buckets) return 0;
    u32 i = key->hash & (o->nbuckets - 1);
    jprop **link = &o->buckets[i];
    while (*link) {
        if (js_str_eq((*link)->key, key)) {
            jprop *gone = *link;
            *link = gone->next;
            /* And out of the order, which has no way back, so it is walked:
               a delete is rare next to everything else done to a property. */
            jprop *prev = 0;
            for (jprop *q = o->ofirst; q; prev = q, q = q->onext) {
                if (q != gone) continue;
                if (prev) prev->onext = q->onext;
                else o->ofirst = q->onext;
                if (o->olast == q) o->olast = prev;
                break;
            }
            o->count--;
            return 1;
        }
        link = &(*link)->next;
    }
    return 0;
}

/* Whether a key names an array index, the digits of a whole number below
   2^32 - 1 with no nought in front, and which. */
static int js_index_key(const jstr *k, u32 *out) {
    if (!k || !k->len || k->len > 10) return 0;
    if (k->len > 1 && k->s[0] == '0') return 0;
    u64 v = 0;
    for (u32 i = 0; i < k->len; i++) {
        if (k->s[i] < '0' || k->s[i] > '9') return 0;
        v = v * 10 + (u64)(k->s[i] - '0');
    }
    if (v >= 0xFFFFFFFFull) return 0;
    if (out) *out = (u32)v;
    return 1;
}

/* An object's own enumerable properties, in the order the standard gives
   them: those named by an array index first, smallest first, then the rest
   in the order they were made. How many, with the list in *out; it lives as
   long as everything else the script made. */
static u32 js_own_keys(jctx *J, jobj *o, jprop ***out) {
    *out = 0;
    if (!o) return 0;
    u32 n = 0;
    for (jprop *p = o->ofirst; p; p = p->onext) if (p->enumerable) n++;
    if (!n) return 0;
    jprop **list = (jprop **)js_alloc(J, (u32)sizeof(jprop *) * n);
    if (!list) return 0;

    /* The indices, each put in its place from the end, so a run of them made
       in order -- which is nearly always how they are made -- costs one
       comparison each. */
    u32 at = 0, v, w;
    for (jprop *p = o->ofirst; p; p = p->onext) {
        if (!p->enumerable || !js_index_key(p->key, &v)) continue;
        u32 k = at++;
        while (k > 0 && js_index_key(list[k - 1]->key, &w) && w > v) {
            list[k] = list[k - 1];
            k--;
        }
        list[k] = p;
    }
    for (jprop *p = o->ofirst; p; p = p->onext)
        if (p->enumerable && !js_index_key(p->key, 0)) list[at++] = p;
    *out = list;
    return n;
}

/* --- arrays -------------------------------------------------------------- */

/* The most elements an array keeps side by side. Past it the size in bytes
   wrapped round: an index of 150 million asked for sixteen times that in a
   32-bit sum, got a block of nothing, and the fill wrote past it; past two
   billion the doubling reached zero and went round for ever. A page's memory
   runs out long before this, so it costs nothing to say. */
#define JS_ARR_MAX (1u << 22)

static void js_arr_reserve(jctx *J, jobj *a, u32 want) {
    if (want <= a->cap || want > JS_ARR_MAX) return;
    u32 cap = a->cap ? a->cap * 2 : 8;
    while (cap < want) cap *= 2;
    jval *fresh = (jval *)js_alloc(J, (u32)sizeof(jval) * cap);
    if (!fresh) return;
    for (u32 i = 0; i < a->len; i++) fresh[i] = a->items[i];
    a->items = fresh;
    a->cap = cap;
}

/* 1 when it was kept among the elements; 0 when it is too far out for that,
   and the caller keeps it some other way or not at all. */
static int js_arr_set(jctx *J, jobj *a, u32 i, jval v) {
    if ((u64)i + 1 > JS_ARR_MAX) return 0;
    js_arr_reserve(J, a, i + 1);
    if ((u64)i + 1 > a->cap) return 0;
    for (u32 k = a->len; k < i; k++) a->items[k] = js_undef();
    a->items[i] = v;
    if (i + 1 > a->len) a->len = i + 1;
    return 1;
}

static void js_arr_push(jctx *J, jobj *a, jval v) {
    js_arr_set(J, a, a->len, v);
}

static jobj *js_array(jctx *J) { return js_object(J, JO_ARRAY); }
