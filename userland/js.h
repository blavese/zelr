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
    struct jprop *next;
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

    /* Arrays keep their elements out of the property table, because a page
       that walks a thousand element array through a hash of decimal strings
       is a page that takes a second to do nothing. */
    jval   *items;
    u32     len, cap;

    /* Functions. `body` and `params` are nodes in the tree; `closure` is the
       scope the function was made in, which is the whole of what a closure
       is. */
    int     body, params, nparams;
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
    N_THROW, N_TRY, N_SWITCH, N_CASE
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

    /* the region */
    jchunk *chunks;
    u32     allocated;

    jscope *global;
    jobj   *global_obj;

    /* what is happening */
    jsignal sig;
    jval    ret;              /* the value of a return, or what was thrown */
    u32     steps;
    int     depth;
    char    error[192];
    int     error_line;

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
        return 0;
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

static void js_free_all(jctx *J) {
    jchunk *c = J->chunks;
    while (c) {
        jchunk *nxt = c->next;
        free(c);
        c = nxt;
    }
    J->chunks = 0;
    J->allocated = 0;
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
    if (!o || !o->buckets) return 0;
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
    o->buckets = fresh;
    o->nbuckets = want;
}

static void js_set_prop(jctx *J, jobj *o, jstr *key, jval v) {
    if (!o || !key) return;
    jprop *p = js_find(o, key);
    if (p) { p->v = v; return; }

    if (o->count + 1 > (o->nbuckets * 3) / 4) js_rehash(J, o);

    p = (jprop *)js_alloc(J, (u32)sizeof(jprop));
    if (!p) return;
    p->key = key;
    p->v = v;
    p->enumerable = 1;
    u32 i = key->hash & (o->nbuckets - 1);
    p->next = o->buckets[i];
    o->buckets[i] = p;
    o->count++;
}

static void js_set(jctx *J, jobj *o, const char *name, jval v) {
    js_set_prop(J, o, js_str(J, name), v);
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
            *link = (*link)->next;
            o->count--;
            return 1;
        }
        link = &(*link)->next;
    }
    return 0;
}

/* --- arrays -------------------------------------------------------------- */

static void js_arr_reserve(jctx *J, jobj *a, u32 want) {
    if (want <= a->cap) return;
    u32 cap = a->cap ? a->cap * 2 : 8;
    while (cap < want) cap *= 2;
    jval *fresh = (jval *)js_alloc(J, (u32)sizeof(jval) * cap);
    if (!fresh) return;
    for (u32 i = 0; i < a->len; i++) fresh[i] = a->items[i];
    a->items = fresh;
    a->cap = cap;
}

static void js_arr_set(jctx *J, jobj *a, u32 i, jval v) {
    js_arr_reserve(J, a, i + 1);
    if (i + 1 > a->cap) return;
    for (u32 k = a->len; k < i; k++) a->items[k] = js_undef();
    a->items[i] = v;
    if (i + 1 > a->len) a->len = i + 1;
}

static void js_arr_push(jctx *J, jobj *a, jval v) {
    js_arr_set(J, a, a->len, v);
}

static jobj *js_array(jctx *J) { return js_object(J, JO_ARRAY); }
