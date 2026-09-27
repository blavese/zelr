/* First-fit free list with boundary tags and coalescing on free. */
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "smp.h"

typedef struct block {
    u32 size;              /* payload bytes */
    bool free;
    struct block *next, *prev;
} block_t;

#define HDR sizeof(block_t)
#define MIN_SPLIT 16

static block_t *head;
static u32 total, used;
static u64 heap_start;

void (*heap_test_probe)(void);

/* Interrupts off and a lock of its own for the length of every change to
 * the list.
 *
 * Nothing in here gives the processor away, so the only way into the middle
 * of a change was an interrupt: the timer preempting a kernel task on the
 * boot processor for another kernel task, or the collector's kfree on the
 * next tick, or -- with that task put down and the kernel lock let go -- a
 * system call arriving from another processor. Any of them found a block
 * the first one had chosen and not yet marked, and both went away with it.
 * Interrupts off keeps this processor's own out. The lock is for the
 * processors that reach the heap without the kernel lock: one coming up at
 * boot, before anybody holds it, and work handed out with smp_run. */
static spinlock_t heap_lock;

static inline bool enter(void) { return spin_lock_irqsave(&heap_lock); }
static inline void leave(bool on) { spin_unlock_irqrestore(&heap_lock, on); }

void heap_init(u64 start, u64 size) {
    heap_start = start;
    head = (block_t *)start;
    head->size = size - HDR;
    head->free = true;
    head->next = head->prev = 0;
    total = size;
    used = 0;
}

static void split(block_t *b, size_t n) {
    if (b->size < n + HDR + MIN_SPLIT) return;
    block_t *nb = (block_t *)((u8 *)b + HDR + n);
    nb->size = b->size - n - HDR;
    nb->free = true;
    nb->next = b->next;
    nb->prev = b;
    if (b->next) b->next->prev = nb;
    b->next = nb;
    b->size = (u32)n;
}

void *kmalloc(size_t n) {
    /* Block sizes are 32 bits. Rounding with ~7u, which is 32 bits too, used
       to mask anything from 4 GiB up down to its low bits -- possibly to
       nothing -- and hand back a small block, which kcalloc then cleared for
       the whole of the size asked for. */
    if (n == 0 || n > 0xFFFFFF00u) return 0;
    n = (n + 7) & ~(size_t)7;                 /* 8-byte align */
    bool on = enter();
    for (block_t *b = head; b; b = b->next) {
        if (b->free && b->size >= n) {
            /* The moment between choosing a block and claiming it, which
               is where a tick used to let somebody else choose it too. */
            if (heap_test_probe) heap_test_probe();
            split(b, n);
            b->free = false;
            used += b->size + HDR;
            leave(on);
            return (u8 *)b + HDR;
        }
    }
    leave(on);
    return 0;
}

void *kcalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    bool on = enter();
    block_t *b = (block_t *)((u8 *)p - HDR);
    if (b->free) { leave(on); return; }
    b->free = true;
    used -= b->size + HDR;

    if (b->next && b->next->free) {           /* merge forward */
        b->size += HDR + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free) {           /* merge backward */
        b->prev->size += HDR + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
    leave(on);
}

bool heap_check(void) {
    bool on = enter();
    bool good = head == (block_t *)heap_start && head->prev == 0;
    u64 end = heap_start + total, sum = 0, in_use = 0;
    block_t *b = head;
    for (u32 guard = 0; good && b; guard++) {
        u64 at = (u64)b;
        u64 after = at + HDR + b->size;
        if (guard > (1u << 24) || at < heap_start || after > end) { good = false; break; }
        sum += HDR + b->size;
        if (!b->free) in_use += HDR + b->size;
        if (b->next) {
            /* Next in the list is next in memory, points back, and is not a
               second free block that merging should have absorbed. */
            if ((u64)b->next != after || b->next->prev != b ||
                (b->free && b->next->free)) good = false;
        } else if (after != end) {
            good = false;
        }
        b = b->next;
    }
    if (good && (sum != total || in_use != used)) good = false;
    leave(on);
    return good;
}

u32 heap_used(void)  { return used; }
u32 heap_total(void) { return total; }
