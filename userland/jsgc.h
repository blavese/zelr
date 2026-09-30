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
    js_gc_roots(J);
    js_gc_drain(J);
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
