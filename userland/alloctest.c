/* The allocator, worked hard enough to break it.
 *
 * Allocating a few things and freeing them proves nothing: the first fit in
 * an empty heap always works. What breaks an allocator is reuse — a
 * thousand blocks of mixed sizes, freed in an order nothing planned for,
 * with the space having to come back each time. If coalescing is wrong the
 * heap grows without bound; if the boundary tags are wrong the walk
 * backwards lands in the middle of a block and the whole thing comes apart.
 *
 * So the strong check here is not "did it return a pointer". It is that a
 * million bytes handed out and given back a thousand times over does not
 * make the heap a thousand times larger, and that every block written to
 * still contains what was written when it comes back.
 */
#include "zelr.h"
#include "alloc.h"

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

static int failed;

static void ok(const char *what, int cond) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    putc('\n');
    if (!cond) failed++;
}

static void okn(const char *what, int cond, u64 n) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    puts("  ");
    putn((int)n);
    putc('\n');
    if (!cond) failed++;
}

/* A cheap repeatable sequence, so the pattern of sizes and the order of
   frees is the same on every run and a failure can be looked at twice. */
static u32 seed = 0x5A4C5200u;
static u32 next_rand(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

#define N 400
static u8 *ptr[N];
static u64 len[N];

/* Every byte of a block set to something derived from its address, so a
   block that overlaps another is caught by the contents rather than by
   luck. */
static void fill(u8 *p, u64 n, u8 tag) {
    for (u64 i = 0; i < n; i++) p[i] = (u8)(tag + (u8)i);
}

static int intact(const u8 *p, u64 n, u8 tag) {
    for (u64 i = 0; i < n; i++)
        if (p[i] != (u8)(tag + (u8)i)) return 0;
    return 1;
}

int main(void) {
    puts("the allocator, in ring 3\n");

    ok("a program starts with no heap at all", heap_size() == 0);

    /* --- the simple things ---------------------------------------------- */
    {
        u8 *a = (u8 *)malloc(100);
        ok("a hundred bytes comes back", a != 0);
        fill(a, 100, 1);
        ok("and holds what was written to it", intact(a, 100, 1));

        u8 *b = (u8 *)malloc(100);
        ok("a second block is somewhere else", b != 0 && b != a);
        ok("and the first is still intact", intact(a, 100, 1));

        free(a);
        free(b);
        ok("freeing twice is not a crash", (free(a), 1));
    }

    ok("asking for nothing gives nothing", malloc(0) == 0);
    ok("freeing nothing is allowed", (free(0), 1));

    /* --- alignment ------------------------------------------------------ */
    {
        /* Held all at once rather than freed each time round.
         *
           Allocating and freeing in a loop reuses the same block and every
           one of them comes back at the same aligned address, so the first
           version of this passed while every block produced by splitting a
           larger one was eight bytes out. Holding them forces the splits,
           which is where the misalignment was. */
        void *held[64];
        int aligned = 1;
        for (int i = 0; i < 64; i++) {
            held[i] = malloc(1 + (u64)i * 7);
            if (!held[i] || ((u64)held[i] & (AL_ALIGN - 1))) aligned = 0;
        }
        ok("every block is aligned, including the ones split off others",
           aligned);
        for (int i = 0; i < 64; i++) free(held[i]);
    }

    /* --- the real test: churn ------------------------------------------- */
    for (int i = 0; i < N; i++) { ptr[i] = 0; len[i] = 0; }

    u64 after_first = 0;
    int contents_ok = 1;

    for (int round = 0; round < 1200; round++) {
        int i = (int)(next_rand() % N);

        if (ptr[i]) {
            if (!intact(ptr[i], len[i], (u8)(i + 1))) contents_ok = 0;
            free(ptr[i]);
            ptr[i] = 0;
            continue;
        }

        /* Mostly small, occasionally large, which is what real use looks
           like and what a bin-per-size-class is for. */
        u64 n = 8 + (next_rand() % 200);
        if ((next_rand() & 31) == 0) n = 2000 + (next_rand() % 9000);

        ptr[i] = (u8 *)malloc(n);
        if (!ptr[i]) { ok("the heap did not run out during churn", 0); break; }
        len[i] = n;
        fill(ptr[i], n, (u8)(i + 1));

        if (round == 100) after_first = heap_size();
    }

    ok("every block still held what was written to it", contents_ok);

    /* The point of the whole exercise. Twelve hundred rounds of allocate
       and free, and the heap must not have grown anything like that much:
       the space has to be coming back. */
    {
        u64 grew = heap_size();
        okn("the heap after 1200 rounds, in kb", grew < 1024 * 1024,
            grew / 1024);
        okn("and it had this much after 100 rounds, in kb",
            after_first > 0, after_first / 1024);
    }

    /* Everything back, and then the heap should be one big free block
       again: a single allocation of most of it has to succeed. */
    {
        for (int i = 0; i < N; i++) if (ptr[i]) { free(ptr[i]); ptr[i] = 0; }
        okn("nothing is still handed out", heap_live() == 0, heap_live());

        u64 whole = heap_size() / 2;
        void *big = malloc(whole);
        ok("and half the heap can be taken in one block", big != 0);
        free(big);
    }

    /* --- growing past what is already mapped ---------------------------- */
    {
        u64 before = heap_size();
        u8 *big = (u8 *)malloc(400 * 1024);
        ok("a block larger than the heap makes the heap larger", big != 0);
        fill(big, 400 * 1024, 7);
        ok("and all four hundred kilobytes of it are writable",
           intact(big, 400 * 1024, 7));
        okn("the heap grew, in kb", heap_size() > before,
            (heap_size() - before) / 1024);
        free(big);
    }

    /* --- realloc -------------------------------------------------------- */
    {
        u8 *p = (u8 *)malloc(64);
        fill(p, 64, 9);
        p = (u8 *)realloc(p, 4096);
        ok("a block that grew kept its contents", p && intact(p, 64, 9));
        free(p);
    }

    puts(failed ? "ALLOCTEST_FAIL\n" : "ALLOCTEST_PASS\n");
    return failed;
}
