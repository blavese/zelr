/* JavaScript: the values, the reader, and the machine that runs it.
 *
 * Written here like everything else. There is no library underneath this and
 * no borrowed grammar; the lexer, the parser and the interpreter are all in
 * this file and the ones that follow it (jsparse.h, jsrun.h, jslib.h, jsco.h).
 *
 * Two things this could not have been built on top of before now. A number
 * in JavaScript is a double, and until the floating point unit was turned on
 * this machine had no doubles at all -- an engine written against integers
 * would have been wrong about every fraction and wrong quietly. And a script
 * allocates: objects, strings, arrays, closures, none of them of a size
 * anybody knows in advance, which is impossible in a program built out of
 * fixed arrays. Both of those are recent and both were built for this.
 *
 * --- what is here ---------------------------------------------------------
 *
 * The language as pages now write it, which is the language of 2021 and not
 * of 2009: let and const with a scope per block and per turn of a loop,
 * arrow functions, classes with fields, private names, getters, setters,
 * static members and super; template literals, tagged or not; destructuring,
 * spread and rest, default parameters; optional chaining and ??, and the
 * logical assignments; symbols and the iteration protocol, for-of, Map, Set
 * and their weak kinds; generators, promises, async functions and await;
 * prototypes that a script can reach, with Object.create and the rest of
 * Object, and property descriptors with accessors.
 *
 * Pages stopped at their first line without these. A minified bundle is
 * arrow functions and classes from one end to the other, and an engine that
 * refuses the first of them refuses the page.
 *
 * --- what is not ----------------------------------------------------------
 *
 * No modules (import and export are refused by name; a page's module scripts
 * are not run), no with, no BigInt arithmetic (a literal such as 10n is read
 * as the number it names), no Proxy, no typed arrays, no Intl. Strings are
 * bytes of UTF-8 rather than UTF-16 units, so length counts bytes.
 *
 * What is refused is refused by name. An engine that accepts a word and does
 * something slightly wrong with it is worse than one that says it does not
 * know the word, because the first produces a page that is subtly broken and
 * the second produces a message.
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
   All are refusals with a message rather than limits that corrupt. The
   memory is what every machine gets; a host with more to spare raises a
   context's own mem_cap (jsdom.h, jd_mem_cap). */
#define JS_MEM_CAP   (24u * 1024 * 1024)
#define JS_MEM_SPARE (1u * 1024 * 1024)
#define JS_STEP_CAP  40000000u
#define JS_DEPTH_CAP 800

/* How much of the machine's stack the scripts may use, below wherever the
   host first called in. A tree walker spends stack on every level of nesting
   in the text as well as on every call, and the only honest bound is the
   stack itself: counting calls alone let a deeply nested expression, or a
   parser reading one, run the program off the end of its stack.
 *
 * Half of zelr's megabyte, which is about six hundred levels of calls. The
 * Windows build (tools/host) gets twice the bytes for the same depth: that
 * calling convention hands every sixteen byte value back through memory, and
 * a level costs twice the stack. */
#if defined(_WIN32)
#define JS_STACK_BUDGET (1024u * 1024)
#else
#define JS_STACK_BUDGET (512u * 1024)
#endif

/* The tree, which is not in the region (it is read, never collected), has a
   ceiling of its own: a page that evaluates text in a loop must not be able
   to grow it until the machine has nothing left. */
#define JS_NODES_CAP (3 * 1024 * 1024)

#define JS_INT_KEYS 1024

/* --- values --------------------------------------------------------------
 *
 * Seven kinds a script can see, in a tagged union, and two only the engine
 * ever holds. A number is a double because that is what a number is in this
 * language: there is no integer type for a script to ask for, and the whole
 * of the arithmetic depends on 1/3 being a third. */

typedef enum {
    JS_UNDEF = 0, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ, JS_SYM,
    /* A let, const or class that has not reached its declaration yet. Reading
       one is an error, which is the whole difference between let and var. */
    JS_HOLE,
    /* An accessor: a getter and a setter where a value would be. Only ever
       inside a property, never handed to a script. */
    JS_ACC,
    /* A BigInt: a whole number of any size (jsbig.h). */
    JS_BIG
} jtype;

typedef struct jstr jstr;
typedef struct jobj jobj;
typedef struct jacc jacc;

/* A BigInt's sign and magnitude, 32-bit limbs least significant first,
   none of them zero at the top; made once and never changed (jsbig.h). */
typedef struct jbint { u32 n; u32 neg; u32 d[1]; } jbint;

typedef struct {
    jtype t;
    union {
        double num;
        int    b;
        jstr  *str;           /* JS_STR, and JS_SYM: the symbol's own key */
        jobj  *obj;
        jacc  *acc;
        jbint *big;
    };
} jval;

/* A string: length, hash and the bytes, all in one allocation. The hash
   means a property lookup compares a number before it compares any bytes.
 *
 * A symbol is a string too, of a kind no script can make: its hash has the
 * top bit set, which no string's ever does (js_hash clears it), and each
 * symbol has a hash of its own. Two keys are equal only when their hashes
 * are, so a symbol is equal to itself and to nothing else, and it can be a
 * property key without a second kind of property table. Its bytes are its
 * description. */
struct jstr {
    u32 len;
    u32 hash;
    char s[1];                /* len + 1, allocated with the header */
};

#define JS_SYM_BIT 0x80000000u
static inline int js_is_sym_key(const jstr *k) { return k && (k->hash & JS_SYM_BIT); }

/* What a property is allowed to have done to it. */
#define JP_ENUM  1
#define JP_WRITE 2
#define JP_CONF  4
#define JP_PLAIN (JP_ENUM | JP_WRITE | JP_CONF)

typedef struct jprop {
    jstr *key;
    jval  v;
    struct jprop *next;       /* in its bucket */
    struct jprop *onext;      /* in the order the properties were made */
    int   flags;              /* JP_* */
} jprop;

struct jacc {
    jval get, set;
};

typedef enum {
    JO_PLAIN = 0, JO_ARRAY, JO_FUNC, JO_NATIVE, JO_REGEX,
    JO_ARGS,                  /* an arguments object: elements like an array */
    JO_BOXED,                 /* new String(...), new Number(...) and the like */
    JO_ERROR, JO_DATE, JO_MAP, JO_SET, JO_WEAKMAP, JO_WEAKSET,
    JO_PROMISE, JO_GEN, JO_ITER, JO_BUFFER, JO_TYPED, JO_VIEW,
    JO_CODEC                  /* a TextDecoder, which remembers half a character */
} jokind;

/* What an object allows. */
#define JOF_NOEXT  1          /* no new properties (preventExtensions) */
#define JOF_FROZEN 2          /* an array whose elements cannot change */
#define JOF_LINKED 4          /* a promise resolver whose pair has been used */
#define JOF_NOCTOR 8          /* a native that `new` refuses */
#define JOF_PROXY  16         /* a Proxy: target in data, handler in data2 (jsproxy.h) */

struct jctx;
typedef jval (*jnative)(struct jctx *J, jval this_val, jval *argv, int argc);

typedef struct jscope jscope;

struct jobj {
    u8      kind;
    u8      flags;
    u16     spare;
    int     host;             /* the host's index for this object, or -1 */
    jobj   *proto;

    jprop **buckets;          /* none until the first property */
    u32     nbuckets, count;

    /* The same properties again, in the order they were made, which is the
       order a script walking them is owed. Walked by bucket they came out
       in hash order, so {x, y, z} listed its keys as y, z, x. */
    jprop  *ofirst, *olast;

    jstr   *name;             /* functions and natives */

    union {
        /* Arrays keep their elements out of the property table, because a
           page that walks a thousand element array through a hash of decimal
           strings is a page that takes a second to do nothing. */
        struct { jval *items; u32 len, cap; };

        /* Functions. `node` is the function in the tree; `closure` is the
           scope the function was made in, which is the whole of what a
           closure is. `home` is where super looks, for a method; `fields`
           is a class's list of what each new instance is given; and an
           arrow keeps the `this` it was written under. */
        struct { int node; int fpad; jscope *closure; jobj *home; jobj *fields; jval lex_this; };

        /* Natives, with room for what a bound function or a promise's
           resolver has to carry. */
        struct { jnative fn; jval data; jval data2; jobj *extra; };

        /* Everything else that has state a property cannot hold. */
        struct { void *internal; jval ival; };
    };
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
 * that outlived the call that made it. A block with a let in it has a scope
 * of its own, and so does each turn of a loop that declares one. */
struct jscope {
    jobj   *vars;
    jscope *parent;

    /* Whether anything can reach this scope once the call that made it is
       over: set, on it and on every scope it sits in, when a function value
       is made inside it. One that is never set is given back at the end of
       its call. */
    int     escaped;

    /* A with statement's: `vars` is the object itself, and a name is its
       when the object has it, inherited or through a proxy's has. */
    int     with;

    /* A module's own scope: the host's record of the module, which import()
       and import.meta written anywhere inside it are relative to. */
    void   *module;
};

/* --- what the reader produces -------------------------------------------- */

typedef enum {
    N_NONE = 0,
    /* expressions */
    N_NUM, N_STR, N_REGEX, N_TRUE, N_FALSE, N_NULL, N_UNDEF, N_IDENT,
    N_ARRAY, N_OBJECT, N_FUNC, N_CALL, N_NEW, N_MEMBER, N_INDEX,
    N_UNARY, N_BINARY, N_LOGICAL, N_ASSIGN, N_COND, N_SEQ,
    N_PREINC, N_POSTINC, N_TYPEOF, N_DELETE, N_THIS,
    N_TEMPLATE, N_TAGGED, N_SPREAD, N_CLASS, N_YIELD, N_AWAIT,
    N_OPTCHAIN, N_SUPERCALL, N_SUPERMEMBER, N_NEWTARGET, N_PRIVNAME,
    N_APAT, N_OPAT, N_IMPORT,
    /* statements */
    N_VAR, N_BLOCK, N_IF, N_WHILE, N_DO, N_FOR, N_FORIN, N_FOROF,
    N_RETURN, N_BREAK, N_CONTINUE, N_EXPRSTMT, N_FUNCDECL, N_EMPTY,
    N_THROW, N_TRY, N_SWITCH, N_CASE, N_LABEL, N_CLASSDECL, N_WITH,
    /* a module's: import and export declarations, one name in their lists,
       and import.meta */
    N_IMPORTDECL, N_IMPSPEC, N_EXPORTDECL, N_EXPSPEC, N_IMPORTMETA,
    /* a BigInt literal: its text, made a value when reached */
    N_BIGINT
} ntype;

/* Thirty two bytes a node. A big page's bundle is a million of them, and the
   tree lives outside the region for the life of the page. */
typedef struct {
    u8    kind;
    u8    flags;              /* NF_* */
    u16   op;                 /* an operator, or a function's FN_* */
    int   line;
    int   a, b, c, d;         /* children, or -1 */
    union {
        double num;
        jstr  *str;
    };
} jnode;

/* Node flags. */
#define NF_OPT     1          /* a ?. link in a chain */
#define NF_SCOPE   2          /* a block that needs a scope of its own */
#define NF_BODY    4          /* a function's own body: its scope is the call's */
#define NF_PRIVATE 8          /* a member named by #name */
#define NF_STATIC  16         /* a class member on the class itself */
#define NF_PAREN   32         /* written in brackets */
#define NF_TLA     64         /* a module's body that awaits at its top level */

/* Function flags, in the op of an N_FUNC. */
#define FN_ARROW     0x0001
#define FN_ASYNC     0x0002
#define FN_GEN       0x0004
#define FN_METHOD    0x0008   /* a method: no prototype, not a constructor */
#define FN_CTOR      0x0010   /* a class constructor: new only */
#define FN_DERIVED   0x0020   /* ...of a class that extends another */
#define FN_STRICT    0x0040
#define FN_ARGS      0x0080   /* its text names `arguments` */
#define FN_SIMPLE    0x0100   /* plain names for parameters, nothing else */
#define FN_SUPER     0x0200   /* its text says super */
#define FN_NEWTARGET 0x0400   /* ...or new.target */
#define FN_GETTER    0x0800
#define FN_SETTER    0x1000
#define FN_EXPR      0x2000   /* an arrow whose body is one expression */
#define FN_FIELD     0x4000   /* a class field's initialiser */
#define FN_SELFNAME  0x8000   /* a function expression written with a name, which it knows itself by */

/* What an object literal's or a class's entry is. */
enum { PK_INIT = 0, PK_SHORT, PK_METHOD, PK_GET, PK_SET, PK_SPREAD, PK_PROTO,
       PK_FIELD, PK_BLOCK };

/* What a declaration declares. */
enum { VK_VAR = 1, VK_LET = 2, VK_CONST = 3 };

/* --- the machine --------------------------------------------------------- */

typedef enum {
    JS_OK = 0, JS_RETURN, JS_BREAK, JS_CONTINUE, JS_THROWN, JS_FAILED
} jsignal;

/* A job for after the current script: a promise's reaction, a thenable to
   follow, a callback queued by queueMicrotask, or a suspended function to
   carry on with. */
typedef struct {
    int   kind;
    jval  a, b;
    jobj *o;
    void *co;
} jjob;

struct jco;

/* Where a function's text is: which kept source, and from where to where
   (Function.prototype.toString). */
typedef struct { int src; u32 start, end; } jspan;

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
    u32     mem_cap;          /* JS_MEM_CAP unless the host gave more */
    void   *free_list[16];

    /* The global scope, whose variables are the global object's properties,
       and inside it the scope every script's top level let, const and class
       go in: shared by all the page's scripts, as the standard has it, and
       not properties of the window. */
    jscope *global;
    jscope *global_lex;
    jobj   *global_obj;

    /* what is happening */
    jsignal sig;
    jval    ret;              /* the value of a return, or what was thrown */
    u32     steps;
    int     depth;
    char    error[192];
    int     error_line;
    int     with_used;        /* a with statement has run: calls look for its object */
    int     parse_module;     /* what is being read is a module: export is a word */

    /* The host's, for modules: import() and import.meta, given the module
       the code was written in (0 for a classic script). */
    jval  (*import_hook)(struct jctx *J, jval spec, void *module);
    jval  (*meta_hook)(struct jctx *J, void *module);

    /* The native being called, for the length of the call into it, and the
       constructor `new` was given when it was new that called it. Read them
       first thing: any call the native makes changes them. */
    jobj   *callee;
    jval    new_target;

    /* Prototypes, and the constructors the engine needs to find again
       whatever a script has assigned over their names. */
    jobj   *p_object, *p_function, *p_array, *p_string, *p_number, *p_boolean,
           *p_symbol, *p_bigint, *p_regexp, *p_error, *p_date, *p_map, *p_set, *p_weakmap,
           *p_weakset, *p_promise, *p_iterator, *p_array_iter, *p_map_iter,
           *p_set_iter, *p_string_iter, *p_generator, *p_async_generator,
           *p_async_iterator, *p_regexp_iter, *p_gen_function, *p_async_function,
           *p_async_gen_function, *p_buffer, *p_view, *p_typed[9], *p_domexc;
    jobj   *c_promise, *c_object, *c_array, *c_function;
    jobj   *err_ctor[8], *err_proto[8];
    jobj   *eval_fn;          /* the original eval, which is the direct one */
    jobj   *tagged;           /* each tagged template's pieces, made once */
    jstr   *int_keys[1024];   /* "0" to "1023" as keys, made once (JS_INT_KEYS) */
    jobj   *sym_registry;     /* Symbol.for's symbols, by name */
    jstr  **intern;           /* the names the parser has read, each once */
    u32     nintern, intern_cap;
    double  clock_base;       /* the wall clock at the first tick read, in ms */
    int     clock_tick0;
    jobj   *array_values;     /* Array.prototype.values, for the fast paths */

    /* The well known symbols, as their keys. */
    jstr   *sym_iterator, *sym_async_iterator, *sym_has_instance,
           *sym_to_primitive, *sym_to_string_tag, *sym_species, *sym_unscopables,
           *sym_match_all;
    u32     sym_serial;

    /* Names the engine looks up all the time, made once. Each lookup used to
       make its own copy of the string, and a page that called a function in
       a loop ran out of memory on the copies. */
    jstr   *s_arguments, *s_length, *s_prototype, *s_constructor, *s_name,
           *s_message, *s_next, *s_done, *s_value, *s_then, *s_return,
           *s_throw, *s_get, *s_set, *s_lastIndex, *s_index, *s_input,
           *s_toString, *s_valueOf, *s_toJSON, *s_this, *s_home, *s_fnself,
           *s_newtarget, *s_stack, *s_callee, *s_groups, *s_enumerable,
           *s_configurable, *s_writable, *s_cause, *s_proto;

    /* A ?. chain that met null or undefined, on its way out to the end of
       the chain. */
    int     chain_short;

    /* The jobs waiting for the current script to finish, as a ring. */
    jjob   *jobs;
    u32     jhead, jcount, jcap;
    int     nest;             /* how deep in calls from the host this is */
    int     draining;

    /* The suspended function whose own stack is in use, if one is, and the
       lowest address the stack in use may reach. */
    struct jco *co_current;
    char   *stack_limit;
    u32     co_live;          /* stacks held by suspended functions */
    struct jco *co_all;       /* and a list of them */

    /* the host's hook: how a property on a host object is read and written,
       and what happens when one is called. Null in a program that has no
       host objects, which is how this file is testable on its own. */
    int  (*host_get)(struct jctx *J, jobj *o, const char *name, jval *out);
    int  (*host_set)(struct jctx *J, jobj *o, const char *name, jval v);
    void *host_data;

    jstr *k_intl;             /* where an Intl object keeps its state (jsintl.h), one for each context */

    /* The text of every script read, kept for the page's life so that a
       function can give its own back (toString): a function node's c is its
       span's index in spans. */
    jstr  **srcs;
    int     nsrcs, capsrcs, cur_src;
    jspan  *spans;
    int     nspans, capspans;
} jctx;

/* --- allocation ---------------------------------------------------------- */

#define JS_CHUNK (256u * 1024u)

static void js_out_of_memory(jctx *J) {
    if (J->sig == JS_FAILED) return;
    J->sig = JS_FAILED;
    const char *m = "this script asked for more memory than a page is allowed";
    int i = 0;
    while (m[i] && i < (int)sizeof(J->error) - 1) {
        J->error[i] = m[i];
        i++;
    }
    J->error[i] = 0;
}

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
    if (J->allocated + n > J->mem_cap) {
        js_out_of_memory(J);
        /* The script is stopped from here, but the C already on its way
           through a statement is not: it asked for a string or a property and
           a great many places use what they were given without asking whether
           they were given anything, and a NULL there took the whole browser
           down. So what is already running is given a little more, from a
           spare that nothing reaches but this, to get back out; the page's
           handlers and timers do not run again (jsdom.h, jd_spent). */
        if (J->allocated + n > J->mem_cap + JS_MEM_SPARE) return 0;
    }

    jchunk *c = J->chunks;
    if (!c || c->used + n > c->size) {
        u32 want = n > JS_CHUNK ? n : JS_CHUNK;
        jchunk *fresh = (jchunk *)malloc(sizeof(jchunk) + want);
        if (!fresh) {
            js_out_of_memory(J);
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
    return h & ~JS_SYM_BIT;
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

/* A new symbol, whose key is equal to nothing that exists. */
static jstr *js_sym_new(jctx *J, const char *desc, u32 n) {
    jstr *r = js_str_n(J, desc, n);
    if (!r) return 0;
    r->hash = JS_SYM_BIT | (++J->sym_serial & ~JS_SYM_BIT);
    return r;
}

static int js_str_eq(const jstr *a, const jstr *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->len != b->len || a->hash != b->hash) return 0;
    for (u32 i = 0; i < a->len; i++) if (a->s[i] != b->s[i]) return 0;
    return 1;
}

static int js_str_is(const jstr *a, const char *b) {
    if (!a || js_is_sym_key(a)) return 0;
    u32 i = 0;
    for (; i < a->len; i++) if (a->s[i] != b[i] || !b[i]) return 0;
    return b[i] == 0;
}

/* --- making values ------------------------------------------------------- */

static inline jval js_undef(void) { jval v; v.t = JS_UNDEF; v.num = 0; return v; }
static inline jval js_null(void)  { jval v; v.t = JS_NULL;  v.num = 0; return v; }
static inline jval js_bool(int b) { jval v; v.t = JS_BOOL;  v.num = 0; v.b = !!b; return v; }
static inline jval js_num(double d) { jval v; v.t = JS_NUM; v.num = d; return v; }
static inline jval js_hole(void)  { jval v; v.t = JS_HOLE;  v.num = 0; return v; }

static inline jval js_from_str(jstr *s) {
    jval v;
    v.t = s ? JS_STR : JS_UNDEF;
    v.str = s;
    return v;
}

static inline jval js_from_obj(jobj *o) {
    jval v;
    v.t = o ? JS_OBJ : JS_UNDEF;
    v.obj = o;
    return v;
}

static inline jval js_from_sym(jstr *k) {
    jval v;
    v.t = k ? JS_SYM : JS_UNDEF;
    v.str = k;
    return v;
}

static inline int js_is_obj(jval v) { return v.t == JS_OBJ && v.obj; }

static inline int js_callable(jval v) {
    return v.t == JS_OBJ && v.obj && (v.obj->kind == JO_FUNC || v.obj->kind == JO_NATIVE);
}

/* --- objects ------------------------------------------------------------- */

#define JS_BUCKETS 4

/* The prototype an object of each kind is made with, until js_init has made
   them all; set by it. */
static jobj *js_default_proto(jctx *J, jokind kind) {
    switch (kind) {
        case JO_ARRAY:  return J->p_array;
        case JO_FUNC: case JO_NATIVE: return J->p_function;
        case JO_REGEX:  return J->p_regexp;
        case JO_ERROR:  return J->p_error;
        case JO_DATE:   return J->p_date;
        case JO_MAP:    return J->p_map;
        case JO_SET:    return J->p_set;
        case JO_WEAKMAP: return J->p_weakmap;
        case JO_WEAKSET: return J->p_weakset;
        case JO_PROMISE: return J->p_promise;
        case JO_BUFFER: return J->p_buffer;
        default:        return J->p_object;
    }
}

static jobj *js_object(jctx *J, jokind kind) {
    jobj *o = (jobj *)js_alloc(J, (u32)sizeof(jobj));
    if (!o) return 0;
    o->kind = (u8)kind;
    o->host = -1;
    o->proto = js_default_proto(J, kind);
    if (kind == JO_FUNC) o->node = -1;
    return o;
}

static jobj *js_object_with(jctx *J, jokind kind, jobj *proto) {
    jobj *o = js_object(J, kind);
    if (o) o->proto = proto;
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
    u32 want = o->nbuckets ? o->nbuckets * 2 : JS_BUCKETS;
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
    if (o->buckets) js_free(J, o->buckets, (u32)sizeof(jprop *) * o->nbuckets);
    o->buckets = fresh;
    o->nbuckets = want;
}

/* An own property made or changed, with what is allowed to happen to it.
   Nothing here asks whether it should be allowed: that is the caller's to
   decide, and the declarations and literals that use this directly are
   never refused. */
static jprop *js_put_prop_flags(jctx *J, jobj *o, jstr *key, jval v, int flags) {
    if (!o || !key) return 0;
    jprop *p = js_find(o, key);
    if (p) { p->v = v; return p; }

    if (!o->buckets || o->count + 1 > (o->nbuckets * 3) / 4) js_rehash(J, o);
    if (!o->buckets) return 0;

    p = (jprop *)js_alloc(J, (u32)sizeof(jprop));
    if (!p) return 0;
    p->key = key;
    p->v = v;
    p->flags = flags;
    u32 i = key->hash & (o->nbuckets - 1);
    p->next = o->buckets[i];
    o->buckets[i] = p;
    if (o->olast) o->olast->onext = p;
    else o->ofirst = p;
    o->olast = p;
    o->count++;
    return p;
}

static jprop *js_put_prop(jctx *J, jobj *o, jstr *key, jval v) {
    return js_put_prop_flags(J, o, key, v, JP_PLAIN);
}

static void js_set_prop(jctx *J, jobj *o, jstr *key, jval v) {
    js_put_prop(J, o, key, v);
}

static void js_set(jctx *J, jobj *o, const char *name, jval v) {
    js_set_prop(J, o, js_str(J, name), v);
}

/* A property the engine keeps for itself, or one the standard keeps out of a
   walk over the keys: a built-in method, a function's prototype. A script can
   still read it by name, but walking the object's keys does not turn it up,
   and neither does JSON. */
__attribute__((unused)) static void js_set_hidden(jctx *J, jobj *o, const char *name, jval v) {
    jprop *p = js_put_prop(J, o, js_str(J, name), v);
    if (p) p->flags = JP_WRITE | JP_CONF;
}

__attribute__((unused)) static jval js_get_prop(jobj *o, const jstr *key) {
    jprop *p = js_find(o, key);
    return p && p->v.t != JS_ACC ? p->v : js_undef();
}

static int js_delete_prop(jobj *o, const jstr *key) {
    if (!o || !o->buckets || !key) return 0;
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
    if (!k || !k->len || k->len > 10 || js_is_sym_key(k)) return 0;
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

/* An object's own properties, in the order the standard gives them: those
   named by an array index first, smallest first, then the rest in the order
   they were made. `want` says which: bit 0 the enumerable ones only, bit 1
   strings, bit 2 symbols. How many, with the list in *out; it lives as long
   as everything else the script made. */
#define JK_ENUM 1
#define JK_STR  2
#define JK_SYM  4

static u32 js_proxy_keys_of(jctx *J, jobj *p, jprop ***out, int want);

static u32 js_keys_of(jctx *J, jobj *o, jprop ***out, int want) {
    *out = 0;
    if (!o) return 0;
    if (o->flags & JOF_PROXY) return js_proxy_keys_of(J, o, out, want);
    u32 n = 0;
    for (jprop *p = o->ofirst; p; p = p->onext) {
        if ((want & JK_ENUM) && !(p->flags & JP_ENUM)) continue;
        if (js_is_sym_key(p->key) ? !(want & JK_SYM) : !(want & JK_STR)) continue;
        n++;
    }
    if (!n) return 0;
    jprop **list = (jprop **)js_alloc(J, (u32)sizeof(jprop *) * n);
    if (!list) return 0;

    /* The indices, each put in its place from the end, so a run of them made
       in order -- which is nearly always how they are made -- costs one
       comparison each. */
    u32 at = 0, v, w;
    for (jprop *p = o->ofirst; p; p = p->onext) {
        if ((want & JK_ENUM) && !(p->flags & JP_ENUM)) continue;
        if (!(want & JK_STR) || !js_index_key(p->key, &v)) continue;
        u32 k = at++;
        while (k > 0 && js_index_key(list[k - 1]->key, &w) && w > v) {
            list[k] = list[k - 1];
            k--;
        }
        list[k] = p;
    }
    for (jprop *p = o->ofirst; p; p = p->onext) {
        if ((want & JK_ENUM) && !(p->flags & JP_ENUM)) continue;
        if (js_is_sym_key(p->key)) continue;
        if ((want & JK_STR) && !js_index_key(p->key, 0)) list[at++] = p;
    }
    if (want & JK_SYM)
        for (jprop *p = o->ofirst; p; p = p->onext) {
            if ((want & JK_ENUM) && !(p->flags & JP_ENUM)) continue;
            if (js_is_sym_key(p->key)) list[at++] = p;
        }
    *out = list;
    return at;
}

static u32 js_own_keys(jctx *J, jobj *o, jprop ***out) {
    return js_keys_of(J, o, out, JK_ENUM | JK_STR);
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
    if (a->items) js_free(J, a->items, (u32)sizeof(jval) * a->cap);
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
