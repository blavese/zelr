/* An allocator, for programs.
 *
 * Every program here used to be a set of fixed arrays. The browser decided
 * at compile time how large a page it could ever show; the editor decided
 * how long a file could be. That is not a style: it is a ceiling written
 * into the binary, and the only way past it was to edit a header and
 * rebuild the system.
 *
 * What makes this possible is one system call. SYS_SBRK moves the program's
 * heap break and maps pages behind it, and everything below is arithmetic
 * on top of that.
 *
 * The shape is a segregated free list with boundary tags. A block carries
 * its size at both ends, so the block before it can be found by reading
 * backwards and the two can be merged when both are free; free blocks are
 * filed in lists by size class, so finding one is looking in the right list
 * rather than walking the heap.
 *
 * What it is not is thread safe. There are no threads inside a program
 * here: a task is a program, and two programs do not share a heap.
 */
#pragma once
#include "zelr.h"

/* Sizes are rounded to this. Sixteen rather than eight because a double has
   to be aligned to eight and anything vector aligned wants sixteen, and the
   header is already sixteen wide. */
#define AL_ALIGN 16

/* How much to ask the kernel for at a time. Asking per allocation would be
   a system call per malloc; asking for this means one call per sixty-four
   kilobytes, and a program that allocates nothing still pays nothing. */
#define AL_CHUNK (64 * 1024)

/* Size classes. Small blocks are much more common than large ones and the
   difference between finding one immediately and walking a list of
   thousands is the difference between an allocator and a problem. */
#define AL_BINS 16

typedef struct al_block {
    u64 size;                 /* payload bytes, with the low bit as "in use" */
    struct al_block *next;    /* while free: the bin's list */
    struct al_block *prev;
    u64 pad;                  /* to sixteen, so payloads land aligned */
} al_block;

#define AL_HDR   ((u64)sizeof(al_block))
#define AL_INUSE 1ull
#define AL_SIZE(b) ((b)->size & ~AL_INUSE)

/* The footer is sixteen bytes and only eight of them say anything.
 *
 * That is not waste, it is the alignment. A block is the header, the
 * payload and the footer, and the next block begins where this one ends; if
 * the footer were the eight bytes it needs, every block after a split would
 * sit eight bytes off a sixteen byte boundary and so would its payload.
 *
 * With the vector instructions off that was survivable and invisible. With
 * them on the compiler uses aligned moves on anything it believes is
 * aligned, and an aligned move to an address that is not raises a general
 * protection fault rather than doing something slow. The first version of
 * this had an eight byte footer and the churn test below faulted in ring 3
 * a few hundred allocations in. */
#define AL_FOOT  16ull

static inline u64 *al_footer(al_block *b) {
    return (u64 *)((u8 *)b + AL_HDR + AL_SIZE(b));
}

static al_block *al_bins[AL_BINS];
static u64 al_top;             /* one past the last byte the heap owns */
static u64 al_bottom;
static u64 al_live;            /* payload bytes currently handed out */

/* Which bin a size belongs in: roughly its logarithm, so each bin holds
   sizes within a factor of two of each other. */
static inline int al_bin_of(u64 n) {
    int b = 0;
    n >>= 5;
    while (n && b < AL_BINS - 1) { n >>= 1; b++; }
    return b;
}

static inline void al_unlink(al_block *b) {
    int i = al_bin_of(AL_SIZE(b));
    if (b->prev) b->prev->next = b->next;
    else al_bins[i] = b->next;
    if (b->next) b->next->prev = b->prev;
    b->next = b->prev = 0;
}

static inline void al_link(al_block *b) {
    int i = al_bin_of(AL_SIZE(b));
    b->prev = 0;
    b->next = al_bins[i];
    if (b->next) b->next->prev = b;
    al_bins[i] = b;
    *al_footer(b) = b->size;
}

/* Asks the kernel for more and turns it into one free block. */
static inline int al_grow(u64 need) {
    u64 want = need + AL_HDR + AL_FOOT;
    if (want < AL_CHUNK) want = AL_CHUNK;
    want = (want + 4095) & ~(u64)4095;

    u64 got = (u64)sbrk((i64)want);
    if (!got) return 0;
    if (!al_bottom) al_bottom = got;

    /* A block that ends where the heap ends. The trailing word is the
       footer, which is why the request was that much larger. */
    al_block *b = (al_block *)got;
    b->size = want - AL_HDR - AL_FOOT;
    b->next = b->prev = 0;
    al_link(b);
    al_top = got + want;
    return 1;
}

/* Splits a block when the tail is worth having on its own. */
static inline void al_split(al_block *b, u64 want) {
    u64 have = AL_SIZE(b);
    if (have < want + AL_HDR + AL_FOOT + AL_ALIGN) return;

    al_block *tail = (al_block *)((u8 *)b + AL_HDR + want + AL_FOOT);
    tail->size = have - want - AL_HDR - AL_FOOT;
    tail->next = tail->prev = 0;

    b->size = want | (b->size & AL_INUSE);
    *al_footer(b) = b->size;
    al_link(tail);
}

static inline void *malloc(u64 n) {
    if (!n) return 0;
    u64 want = (n + AL_ALIGN - 1) & ~(u64)(AL_ALIGN - 1);

    for (int i = al_bin_of(want); i < AL_BINS; i++) {
        for (al_block *b = al_bins[i]; b; b = b->next) {
            if (AL_SIZE(b) < want) continue;
            al_unlink(b);
            al_split(b, want);
            b->size |= AL_INUSE;
            *al_footer(b) = b->size;
            al_live += AL_SIZE(b);
            return (u8 *)b + AL_HDR;
        }
    }

    if (!al_grow(want)) return 0;
    return malloc(n);
}

static inline void free(void *p) {
    if (!p) return;
    al_block *b = (al_block *)((u8 *)p - AL_HDR);
    if (!(b->size & AL_INUSE)) return;          /* freed twice */

    al_live -= AL_SIZE(b);
    b->size &= ~AL_INUSE;

    /* Merge forward. The next block starts just past this one's footer, and
       only exists if that address is still inside the heap. */
    u64 after = (u64)((u8 *)b + AL_HDR + AL_SIZE(b) + AL_FOOT);
    if (after < al_top) {
        al_block *nxt = (al_block *)after;
        if (!(nxt->size & AL_INUSE)) {
            al_unlink(nxt);
            b->size = AL_SIZE(b) + AL_HDR + AL_FOOT + AL_SIZE(nxt);
        }
    }

    /* And backwards, using the previous block's footer to find its head. */
    if ((u64)b > al_bottom) {
        u64 *prev_foot = (u64 *)((u8 *)b - AL_FOOT);
        if (!(*prev_foot & AL_INUSE)) {
            u64 psize = *prev_foot & ~AL_INUSE;
            al_block *prev = (al_block *)((u8 *)b - AL_FOOT - psize - AL_HDR);
            if ((u64)prev >= al_bottom) {
                al_unlink(prev);
                prev->size = psize + AL_HDR + AL_FOOT + AL_SIZE(b);
                b = prev;
            }
        }
    }

    al_link(b);
}

static inline void *calloc(u64 n) {
    void *p = malloc(n);
    if (p) memset(p, 0, (int)n);
    return p;
}

static inline void *realloc(void *p, u64 n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }

    al_block *b = (al_block *)((u8 *)p - AL_HDR);
    u64 have = AL_SIZE(b);
    u64 want = (n + AL_ALIGN - 1) & ~(u64)(AL_ALIGN - 1);
    if (want <= have) return p;

    void *fresh = malloc(n);
    if (!fresh) return 0;
    memcpy(fresh, p, (int)have);
    free(p);
    return fresh;
}

/* What the program is holding, and what it has taken from the kernel. For
   anything that wants to report on itself. */
static inline u64 heap_live(void) { return al_live; }
static inline u64 heap_size(void) { return al_top - al_bottom; }
