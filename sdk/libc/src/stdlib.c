/* Memory, numbers out of text, sorting, and ending.
 *
 * Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The allocator is a first-fit free list over memory taken from the kernel
 * with sbrk, with adjacent free blocks joined back together. That is not the
 * fastest arrangement and it is the one whose failure modes are visible:
 * every block carries its size and a flag, free() checks the flag it is
 * about to clear, and a double free is caught rather than corrupting the
 * list quietly.
 *
 * It does not give memory back to the kernel. A program that frees
 * everything and then asks again reuses what it already has, which is the
 * common case; a program that frees everything and then exits gives it all
 * back at once, which is the other one.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

/* The build script sets both of these; they are here so that this
   file is also correct when compiled on its own. */
#ifndef ZELR_NO_SUGAR
#define ZELR_NO_SUGAR 1
#endif
#ifndef ZELR_NO_START
#define ZELR_NO_START 1
#endif
#include "zelr.h"

int errno;

/* --- the heap ------------------------------------------------------------ */

typedef struct block {
    size_t        size;          /* payload bytes, not counting this header */
    struct block *next;
    int           used;
} block;

#define HDR   ((size_t)sizeof(block))
#define ALIGN 16
#define CHUNK (64 * 1024)        /* what one trip to the kernel is worth */

static block *heap_head;

static size_t round_up(size_t n) {
    return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);
}

/* Splits a block that is comfortably larger than what was asked for, so a
   one byte request out of a sixty kilobyte block does not consume it. */
static void split(block *b, size_t want) {
    if (b->size < want + HDR + ALIGN) return;
    block *rest = (block *)((unsigned char *)b + HDR + want);
    rest->size = b->size - want - HDR;
    rest->used = 0;
    rest->next = b->next;
    b->size = want;
    b->next = rest;
}

static void join_forward(block *b) {
    while (b->next && !b->next->used
           && (unsigned char *)b + HDR + b->size == (unsigned char *)b->next) {
        b->size += HDR + b->next->size;
        b->next = b->next->next;
    }
}

static block *grow(size_t want) {
    size_t take = want + HDR;
    if (take < CHUNK) take = CHUNK;
    take = round_up(take);

    void *mem = sbrk((i64)take);
    if (!mem || mem == (void *)-1) { errno = ENOMEM; return NULL; }

    block *b = (block *)mem;
    b->size = take - HDR;
    b->used = 0;
    b->next = NULL;

    if (!heap_head) { heap_head = b; return b; }

    block *last = heap_head;
    while (last->next) last = last->next;
    last->next = b;

    /* The kernel hands back the next address up, so the new block usually
       sits directly on the end of the old one and the two are really one. */
    join_forward(last);
    return last->used ? b : last;
}

void *malloc(size_t n) {
    if (n == 0) n = 1;
    if (n > (size_t)-1 - HDR - ALIGN) { errno = ENOMEM; return NULL; }
    size_t want = round_up(n);

    for (block *b = heap_head; b; b = b->next) {
        if (b->used || b->size < want) continue;
        split(b, want);
        b->used = 1;
        return (unsigned char *)b + HDR;
    }

    block *b = grow(want);
    if (!b) return NULL;
    if (b->used || b->size < want) { errno = ENOMEM; return NULL; }
    split(b, want);
    b->used = 1;
    return (unsigned char *)b + HDR;
}

void free(void *p) {
    if (!p) return;
    block *b = (block *)((unsigned char *)p - HDR);

    /* A block that is already free is a program with a bug in it, and
       joining it into the list a second time would put the same memory in
       two places. Refusing is the answer that can be debugged. */
    if (!b->used) return;
    b->used = 0;

    join_forward(b);

    /* And backwards, which costs a walk because the list is one way. */
    for (block *q = heap_head; q; q = q->next)
        if (!q->used) join_forward(q);
}

void *calloc(size_t count, size_t size) {
    if (count && size > (size_t)-1 / count) { errno = ENOMEM; return NULL; }
    size_t n = count * size;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }

    block *b = (block *)((unsigned char *)p - HDR);
    if (b->size >= round_up(n)) return p;

    void *out = malloc(n);
    if (!out) return NULL;
    memcpy(out, p, b->size);
    free(p);
    return out;
}

/* --- ending -------------------------------------------------------------- */

#define ATEXIT_MAX 32
static void (*at_exit_fns[ATEXIT_MAX])(void);
static int n_at_exit;

int atexit(void (*fn)(void)) {
    if (!fn || n_at_exit >= ATEXIT_MAX) return -1;
    at_exit_fns[n_at_exit++] = fn;
    return 0;
}

/* Defined in stdio.c: anything written and not yet sent goes out before the
   program stops, which is what makes printf without a newline appear. */
void _zflush_all(void);

_Noreturn void exit(int status) {
    while (n_at_exit) at_exit_fns[--n_at_exit]();
    _zflush_all();
    zelr_exit(status);
}

_Noreturn void abort(void) {
    _zflush_all();
    zelr_exit(134);              /* what a shell prints for a killed program */
}

void _zassert(const char *what, const char *file, int line) {
    extern int fprintf(void *f, const char *fmt, ...);
    extern void *stderr;
    fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, what);
    abort();
}

/* --- numbers out of text -------------------------------------------------- */

static int digit_of(int c, int base) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    else return -1;
    return v < base ? v : -1;
}

static unsigned long long scan_uint(const char *s, char **end, int base,
                                    int *negative) {
    const char *start = s;
    while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;

    *negative = 0;
    if (*s == '+') s++;
    else if (*s == '-') { *negative = 1; s++; }

    if ((base == 0 || base == 16) && s[0] == '0'
        && (s[1] == 'x' || s[1] == 'X') && digit_of(s[2], 16) >= 0) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0' && digit_of(s[1], 8) >= 0) ? 8 : 10;
    }

    unsigned long long v = 0;
    int any = 0;
    for (;;) {
        int d = digit_of((unsigned char)*s, base);
        if (d < 0) break;
        v = v * (unsigned)base + (unsigned)d;
        any = 1;
        s++;
    }

    /* Nothing was a number, so nothing was consumed: the standard asks for
       the original pointer back rather than wherever the scan stopped. */
    if (end) *end = (char *)(any ? s : start);
    return v;
}

long strtol(const char *s, char **end, int base) {
    int neg;
    unsigned long long v = scan_uint(s, end, base, &neg);
    if (neg) return v > 9223372036854775807ULL ? LONG_MIN : -(long)v;
    return v > 9223372036854775807ULL ? LONG_MAX : (long)v;
}

unsigned long strtoul(const char *s, char **end, int base) {
    int neg;
    unsigned long long v = scan_uint(s, end, base, &neg);
    return neg ? (unsigned long)(-(long long)v) : (unsigned long)v;
}

long long strtoll(const char *s, char **end, int base) {
    return (long long)strtol(s, end, base);
}

unsigned long long strtoull(const char *s, char **end, int base) {
    return (unsigned long long)strtoul(s, end, base);
}

int       atoi(const char *s)  { return (int)strtol(s, NULL, 10); }
long      atol(const char *s)  { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

/* A decimal number with an exponent. Built by multiplying rather than by
   any table of powers, which loses a little at the far ends of the range
   and is exact for everything a program is likely to have written down. */
double strtod(const char *s, char **end) {
    const char *start = s;
    while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;

    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }

    double v = 0;
    int any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }

    if (*s == '.') {
        s++;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (*s - '0') * scale;
            scale /= 10;
            s++;
            any = 1;
        }
    }

    if (any && (*s == 'e' || *s == 'E')) {
        const char *was = s;
        s++;
        int eneg = 0;
        if (*s == '+') s++;
        else if (*s == '-') { eneg = 1; s++; }
        if (*s >= '0' && *s <= '9') {
            int e = 0;
            while (*s >= '0' && *s <= '9') { e = e * 10 + (*s - '0'); s++; }
            for (int i = 0; i < e; i++) v = eneg ? v / 10 : v * 10;
        } else {
            s = was;              /* an e with no exponent is not part of it */
        }
    }

    if (end) *end = (char *)(any ? s : start);
    return neg ? -v : v;
}

double atof(const char *s) { return strtod(s, NULL); }

int  abs(int v)   { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

div_t div(int num, int den) {
    div_t r;
    r.quot = num / den;
    r.rem = num % den;
    return r;
}

ldiv_t ldiv(long num, long den) {
    ldiv_t r;
    r.quot = num / den;
    r.rem = num % den;
    return r;
}

/* --- sorting and searching ------------------------------------------------ */

static void swap_bytes(unsigned char *a, unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

/* Insertion sort below a size where anything cleverer costs more than it
   saves, and a median-of-three quicksort above it. The median matters: a
   sorted array is the input a program is most likely to hand this, and it
   is exactly the one that makes a first-element pivot quadratic. */
static void sort_range(unsigned char *base, size_t n, size_t size,
                       int (*cmp)(const void *, const void *)) {
    while (n > 12) {
        unsigned char *lo = base;
        unsigned char *hi = base + (n - 1) * size;
        unsigned char *mid = base + (n / 2) * size;

        if (cmp(mid, lo) < 0) swap_bytes(mid, lo, size);
        if (cmp(hi, mid) < 0) {
            swap_bytes(hi, mid, size);
            if (cmp(mid, lo) < 0) swap_bytes(mid, lo, size);
        }
        swap_bytes(mid, hi - size, size);      /* the pivot, parked */
        unsigned char *pivot = hi - size;

        unsigned char *i = lo;
        unsigned char *j = pivot;
        for (;;) {
            do { i += size; } while (cmp(i, pivot) < 0);
            do { j -= size; } while (j > lo && cmp(j, pivot) > 0);
            if (i >= j) break;
            swap_bytes(i, j, size);
        }
        swap_bytes(i, pivot, size);

        size_t left = (size_t)(i - lo) / size;
        size_t right = n - left - 1;

        /* Recurse into the smaller half and loop on the larger, so the
           depth is logarithmic whatever the input looks like. */
        if (left < right) {
            sort_range(lo, left, size, cmp);
            base = i + size;
            n = right;
        } else {
            sort_range(i + size, right, size, cmp);
            n = left;
        }
    }

    for (size_t k = 1; k < n; k++) {
        for (size_t m = k; m > 0; m--) {
            unsigned char *a = base + (m - 1) * size;
            unsigned char *b = base + m * size;
            if (cmp(a, b) <= 0) break;
            swap_bytes(a, b, size);
        }
    }
}

void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *)) {
    if (n < 2 || size == 0) return;
    sort_range((unsigned char *)base, n, size, cmp);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
    const unsigned char *b = (const unsigned char *)base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, b + mid * size);
        if (r == 0) return (void *)(b + mid * size);
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

/* --- numbers that look unpredictable and are not -------------------------- */

/* The constants are the ones Park and Miller published, and the sequence is
   the same every run unless srand says otherwise -- which is what the
   standard requires and what makes a program repeatable. Anything that
   needs unpredictable bytes should ask the kernel, which has a real source
   and says when it has none. */
static unsigned long long rand_state = 1;

int rand(void) {
    rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((rand_state >> 33) & 0x7FFFFFFF);
}

void srand(unsigned seed) { rand_state = seed; }

/* --- the two that need something this machine has not got ----------------- */

/* There is no environment: nothing sets one and nothing passes one on, so
   every name is absent rather than empty. Saying so is better than
   returning "" and having a program act on a setting nobody made. */
char *getenv(const char *name) { (void)name; return NULL; }

int system(const char *command) {
    (void)command;
    errno = ENOSYS;
    return -1;
}
