/* The collector: what nothing can reach any more goes back to the region.
 *
 * Nothing was ever given back but a call's own scope, and only when nothing
 * made during the call could reach it: a page's scripts kept every object,
 * string and scope they had ever made until the page was left. MSN's page
 * had used 121 of its 128 megabytes four seconds in, and its news never came.
 *
 * The engine is a tree walker, and what it is working on is in C: in the
 * locals of the calls that got it there, in natives' locals, in the host's.
 * There is no list of them to be had without rewriting all of it. So the
 * collector looks, as Boehm's does, at every word that could hold one --
 * the stack, the stacks of suspended functions (jsco.h), the program's
 * writable data (zelr.h), the context itself and the tree's strings -- and
 * takes any word that points into a block, anywhere in it, to mean that
 * block is in use; then every word of every block in use, the same way,
 * until there are no more. A number that happens to look like an address
 * keeps a block that could have gone, which costs memory and nothing else.
 * Blocks never move, so nothing that points at one has to be told.
 *
 * What is not in use goes back to the free lists, next to its free
 * neighbours made one block (a free block says so in itself, js_is_free),
 * and a chunk with nothing in use left is given back to the machine. The
 * host must keep what it holds of a context's where the collector looks: in
 * the region itself, reached from the context or from its own static data,
 * or on the stack. Nothing the engine keeps in memory from malloc may hold a
 * value, which is why the lists of listeners, timers, jobs, names and texts
 * are in the region.
 *
 * It runs when what is in use has doubled since it last ran (at the first
 * JS_GC_FIRST), and before a page is told it has run out. It never runs
 * inside itself, and it allocates nothing from the region. */
#pragma once
#include "js.h"
#include "jsco.h"

typedef struct { jchunk *c; u32 i; } jgcitem;

static jgcitem *js_gc_stack;
static u32 js_gc_n, js_gc_cap;
static int js_gc_lost;                 /* the mark stack could not grow: sweep nothing */
static u8 *js_gc_lo, *js_gc_hi;        /* the context's chunks, lowest and highest */

/* The first block starting at or after bit i of a chunk, or its end. */
static u32 js_next_start(jchunk *c, u32 i) {
    u32 nb = c->used / 16;
    u32 *st = js_starts(c);
    while (i < nb) {
        u32 w = st[i >> 5] >> (i & 31);
        if (w) {
            u32 k = i + (u32)__builtin_ctz(w);
            return k < nb ? k : nb;
        }
        i = (i | 31u) + 1;
    }
    return nb;
}

/* The block a bit is in: the last start at or before it. */
static int js_prev_start(jchunk *c, u32 i) {
    u32 *st = js_starts(c);
    for (;;) {
        u32 w = st[i >> 5] & (0xFFFFFFFFu >> (31 - (i & 31)));
        if (w) return (int)((i & ~31u) + 31u - (u32)__builtin_clz(w));
        if (i < 32) return -1;
        i = (i & ~31u) - 1;
    }
}

static void js_gc_push(jchunk *c, u32 i) {
    if (js_gc_n >= js_gc_cap) {
        u32 cap = js_gc_cap ? js_gc_cap * 2 : 4096;
        jgcitem *more = (jgcitem *)malloc((u64)cap * sizeof(jgcitem));
        if (!more) { js_gc_lost = 1; return; }
        for (u32 k = 0; k < js_gc_n; k++) more[k] = js_gc_stack[k];
        free(js_gc_stack);
        js_gc_stack = more;
        js_gc_cap = cap;
    }
    js_gc_stack[js_gc_n].c = c;
    js_gc_stack[js_gc_n].i = i;
    js_gc_n++;
}

/* A word that may point into a block: the block is in use. */
static void js_gc_try(jctx *J, u64 w) {
    const u8 *p = (const u8 *)w;
    if (p < js_gc_lo || p >= js_gc_hi) return;
    jchunk *c = js_chunk_of(J, p);
    if (!c) return;
    u32 off = (u32)(p - c->data);
    if (off >= c->used) return;
    int s = js_prev_start(c, off / 16);
    if (s < 0) return;
    u32 *m = js_marks(c);
    if (js_bit(m, (u32)s)) return;
    js_bit_set(m, (u32)s);
    if (js_is_free(c->data + (u32)s * 16)) return;
    js_gc_push(c, (u32)s);
}

/* The mark of the block p points into, as js_gc_try finds it: 1 for a
   pointer outside the region, which nothing here frees. */
static u32 *js_gc_mark_of(jctx *J, const void *ptr, u32 *bit) {
    const u8 *p = (const u8 *)ptr;
    if (!p || p < js_gc_lo || p >= js_gc_hi) return 0;
    jchunk *c = js_chunk_of(J, p);
    if (!c) return 0;
    u32 off = (u32)(p - c->data);
    if (off >= c->used) return 0;
    int s = js_prev_start(c, off / 16);
    if (s < 0) return 0;
    *bit = (u32)s;
    return js_marks(c);
}

static int js_gc_marked(jctx *J, const void *p) {
    u32 bit, *m = js_gc_mark_of(J, p, &bit);
    return !m || js_bit(m, bit);
}

/* Marked without being looked inside: a weak collection's own blocks. */
static void js_gc_mark_only(jctx *J, const void *p) {
    u32 bit, *m = js_gc_mark_of(J, p, &bit);
    if (m) js_bit_set(m, bit);
}

static void js_gc_unmark(jctx *J, const void *p) {
    u32 bit, *m = js_gc_mark_of(J, p, &bit);
    if (m) js_bit_clear(m, bit);
}

static void js_gc_range(jctx *J, const void *lo, const void *hi) {
    u64 a = ((u64)lo + 7) & ~7ull, b = (u64)hi & ~7ull;
    for (; a < b; a += 8) js_gc_try(J, *(const u64 *)a);
}

static void js_gc_drain(jctx *J) {
    while (js_gc_n) {
        jgcitem it = js_gc_stack[--js_gc_n];
        u32 end = js_next_start(it.c, it.i + 1);
        js_gc_range(J, it.c->data + it.i * 16, it.c->data + end * 16);
    }
}

/* --- WeakMap and WeakSet -------------------------------------------------------
 *
 * They were Maps under another name, and kept every key and value they were
 * ever given: reselect's memo, which keys a WeakMap by each state an app's
 * store has had, kept every one of them. So a weak collection's table, its
 * entries and its hash are marked before anything is looked at, and never
 * looked inside; once everything else that is reachable has been found, an
 * entry whose key was found has its value looked at too, over and over until
 * nothing new is found (a value may hold another entry's key), and an entry
 * whose key was not found is removed. A collection nothing reached has its
 * blocks unmarked, to go with it. */
static void js_gc_weak_hold(jctx *J) {
    for (u32 w = 0; w < J->nweak; w++) {
        jmap *m = (jmap *)J->weak[w]->internal;
        if (!m) continue;
        js_gc_mark_only(J, m);
        js_gc_mark_only(J, m->e);
        js_gc_mark_only(J, m->heads);
    }
}

static int js_gc_key_found(jctx *J, jval k) {
    if (k.t == JS_OBJ) return js_gc_marked(J, k.obj);
    if (k.t == JS_SYM) return js_gc_marked(J, k.str);
    return 1;
}

static void js_gc_weak_settle(jctx *J) {
    for (;;) {
        int more = 0;
        for (u32 w = 0; w < J->nweak; w++) {
            jobj *o = J->weak[w];
            jmap *m = (jmap *)o->internal;
            if (!m || o->kind != JO_WEAKMAP || !js_gc_marked(J, o)) continue;
            for (u32 i = 0; i < m->n; i++) {
                jmentry *e = &m->e[i];
                if (e->gone || !js_gc_key_found(J, e->k)) continue;
                if (e->v.t != JS_OBJ && e->v.t != JS_STR && e->v.t != JS_SYM && e->v.t != JS_BIG) continue;
                if (js_gc_marked(J, e->v.obj)) continue;
                js_gc_try(J, (u64)e->v.obj);
                more = 1;
            }
        }
        js_gc_drain(J);
        if (!more || js_gc_lost) break;
    }
    if (js_gc_lost) return;
    u32 keep = 0;
    for (u32 w = 0; w < J->nweak; w++) {
        jobj *o = J->weak[w];
        jmap *m = (jmap *)o->internal;
        if (!js_gc_marked(J, o)) {
            if (m) { js_gc_unmark(J, m->heads); js_gc_unmark(J, m->e); js_gc_unmark(J, m); }
            continue;
        }
        for (u32 i = 0; m && i < m->n; i++) {
            jmentry *e = &m->e[i];
            if (e->gone || js_gc_key_found(J, e->k)) continue;
            e->gone = 1;
            e->k = js_undef();
            e->v = js_undef();
            m->live--;
        }
        J->weak[keep++] = o;
    }
    J->nweak = keep;
}

/* Everything that may hold a context's values from outside its region.
   Not inlined, so the registers js_gc saved are above the word it starts
   from. */
__attribute__((noinline)) static void js_gc_roots(jctx *J) {
    volatile char here = 0;
    const char *sp = (const char *)&here;
    jco *run = js_thread_co;
    if (run && run->stack) {
        /* On a suspended function's stack: it, and the main one below where
           it was switched from. */
        js_gc_range(J, sp, run->stack + JS_CO_STACK);
        js_gc_range(J, run->back_sp, zelr_stack_top);
    } else {
        js_gc_range(J, sp, zelr_stack_top);
    }
    for (jco *co = J->co_all; co; co = co->all_next)
        if (co != run && co->stack && co->sp) js_gc_range(J, co->sp, co->stack + JS_CO_STACK);
    js_gc_range(J, zelr_data_lo(), zelr_data_hi());
    js_gc_range(J, J, J + 1);
    for (int i = 0; i < J->nnodes; i++) js_gc_try(J, (u64)J->nodes[i].str);
}

/* What was not found goes back, runs of free blocks made one; a chunk with
   nothing in use is given back to the machine, and the end of the one being
   filled to its room. */
static void js_gc_sweep(jctx *J) {
    for (int k = 0; k < JS_FREE_CLASSES; k++) J->free_list[k] = 0;
    J->free_large = 0;
    u32 before = J->allocated;
    jchunk **link = &J->chunks;
    for (jchunk *c = J->chunks; c; ) {
        jchunk *next = c->next;
        u32 nb = c->used / 16;
        u32 *st = js_starts(c), *mk = js_marks(c);
        int live = 0;
        for (u32 i = 0; i < nb && !live; i = js_next_start(c, i + 1))
            live = js_bit(mk, i) && !js_is_free(c->data + i * 16);
        if (!live && c != J->chunks) {
            for (u32 i = 0; i < nb; ) {
                u32 j = js_next_start(c, i + 1);
                if (!js_is_free(c->data + i * 16)) J->allocated -= (j - i) * 16;
                i = j;
            }
            *link = next;
            js_map_remove(J, c);
#ifdef JS_ALLOC_PROFILE
            free((void *)c->spare);
#endif
            free(c);
            c = next;
            continue;
        }
        int run = -1;
        for (u32 i = 0; i < nb; ) {
            u32 j = js_next_start(c, i + 1);
            int is_free = js_is_free(c->data + i * 16), marked = js_bit(mk, i);
            js_bit_clear(mk, i);
            if (marked && !is_free) {
                if (run >= 0) {
                    js_free_push(J, c->data + (u32)run * 16, (i - (u32)run) * 16);
                    run = -1;
                }
            } else {
                if (!is_free) J->allocated -= (j - i) * 16;
                if (run < 0) run = (int)i;
                else js_bit_clear(st, i);
            }
            i = j;
        }
        if (run >= 0) {
            if (c == J->chunks) {
                js_bit_clear(st, (u32)run);
                c->used = (u32)run * 16;
            } else {
                js_free_push(J, c->data + (u32)run * 16, (nb - (u32)run) * 16);
            }
        }
        link = &c->next;
        c = next;
    }
    J->gc_freed = before - J->allocated;
}

static void js_gc_clear_marks(jctx *J) {
    for (u32 k = 0; k < J->gc_nmap; k++) {
        jchunk *c = J->gc_map[k];
        u32 words = js_bits_words(c->size);
        for (u32 w = 0; w < words; w++) js_marks(c)[w] = 0;
    }
}

static void js_gc(jctx *J) {
    if (J->gc_busy || !zelr_stack_top || !J->gc_nmap) return;
    /* Every register the caller kept a value in, written into this frame,
       where js_gc_roots will find it. */
    __builtin_unwind_init();
    J->gc_busy = 1;
    js_gc_lo = J->gc_map[0]->data;
    jchunk *last = J->gc_map[J->gc_nmap - 1];
    js_gc_hi = last->data + last->size;
    js_gc_n = 0;
    js_gc_lost = 0;
    js_gc_weak_hold(J);
    js_gc_roots(J);
    js_gc_drain(J);
    js_gc_weak_settle(J);
    if (js_gc_lost) js_gc_clear_marks(J);
    else js_gc_sweep(J);
    J->gc_runs++;
    u32 live = J->allocated;
    u32 next = live < JS_GC_FIRST / 2 ? JS_GC_FIRST : live * 2;
    J->gc_next = next > J->mem_cap ? J->mem_cap : next;
    /* Near the cap with little given back, running again at every block
       would only put off being told: the next time is past the cap. */
    if (J->gc_freed < J->mem_cap / 16 && live + J->mem_cap / 16 > J->mem_cap) J->gc_next = J->mem_cap + JS_MEM_SPARE;
    J->gc_busy = 0;
}

#ifdef JS_ALLOC_PROFILE
/* What is still held, by the site that made it: a collection first, then
   every block in use summed under the stack it was made at (scriptdump URL
   SECONDS live, in a build with HOST_CFLAGS=-DJS_ALLOC_PROFILE). */
static u64 js_prof_held[JS_PROF_SLOTS + 1];
static u32 js_prof_held_n[JS_PROF_SLOTS + 1];

__attribute__((unused)) static void js_prof_live(jctx *J, int most) {
    js_gc(J);
    for (int s = 0; s <= JS_PROF_SLOTS; s++) { js_prof_held[s] = 0; js_prof_held_n[s] = 0; }
    u64 total = 0;
    for (jchunk *c = J->chunks; c; c = c->next) {
        u32 nb = c->used / 16;
        unsigned short *sites = (unsigned short *)c->spare;
        for (u32 i = 0; i < nb; ) {
            u32 j = js_next_start(c, i + 1);
            if (!js_is_free(c->data + i * 16)) {
                int s = sites ? sites[i] : 0;
                js_prof_held[s] += (u64)(j - i) * 16;
                js_prof_held_n[s]++;
                total += (u64)(j - i) * 16;
            }
            i = j;
        }
    }
    char line[1200];
    snprintf(line, sizeof(line), "held %llu KB after a collection\n", (unsigned long long)(total / 1024));
    puts(line);
    static u8 shown[JS_PROF_SLOTS + 1];
    for (int r = 0; r < most; r++) {
        int best = -1;
        for (int s = 0; s <= JS_PROF_SLOTS; s++)
            if (!shown[s] && js_prof_held[s] && (best < 0 || js_prof_held[s] > js_prof_held[best])) best = s;
        if (best < 0) break;
        shown[best] = 1;
        int at = snprintf(line, sizeof(line), "%7llu KB %8u  ", (unsigned long long)(js_prof_held[best] / 1024), js_prof_held_n[best]);
        if (best == 0) at += snprintf(line + at, sizeof(line) - at, "(made before profiling knew its chunk)");
        for (int i = 0; best > 0 && i < 4 && js_prof[best - 1].at[i]; i++) {
            if (i) at += snprintf(line + at, sizeof(line) - at, " < ");
            at += host_symbol(js_prof[best - 1].at[i], line + at, (int)sizeof(line) - at - 4);
            if (at > (int)sizeof(line) - 300) break;
        }
        snprintf(line + at, sizeof(line) - at, "\n");
        puts(line);
    }
}
#endif
