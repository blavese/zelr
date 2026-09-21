/* maptest — memory that costs what it is used rather than what it is asked
 * for.
 *
 * A mapping is a promise. Nothing is allocated when one is made; a page
 * arrives the first time the program touches one. The whole point of that
 * is a number, so this measures the number: it asks the machine how much
 * memory is free, maps sixty-four megabytes, and asks again. A kernel that
 * allocated on the promise loses sixty-four megabytes at that line. This one
 * should lose nothing worth measuring.
 *
 * Then it touches the pages one at a time and watches memory go down in step
 * with what it touched, which is the same claim from the other side.
 *
 * The rest is the things a mapping has to be, and each of them is a way to
 * get it wrong that looks fine from inside:
 *
 *   the pages are zeroed, because a page handed over with the last
 *     program's secrets in it is worse than no mapping at all
 *   what is written stays written, so the fault handler is not quietly
 *     handing over a fresh page every time
 *   unmapping gives the memory back
 *   an address just past the end is not the program's, and reaching for it
 *     ends the program rather than growing the mapping
 *   a child inherits the promise, and what it writes is its own
 */
#include "zelr.h"

static int fails = 0;

static void ok(const char *what, int cond) {
    puts(cond ? "  ok   " : "  FAIL ");
    puts(what);
    putc('\n');
    if (!cond) fails++;
}

#define MEG (1024u * 1024u)
#define BIG (64u * MEG)

static u32 free_kb(void) {
    zelr_sysinfo s;
    if (sysinfo(&s) != 0) return 0;
    return s.mem_free_kb;
}

/* 139 is what a program killed by a memory fault exits with. */
#define FAULTED 139

int main(void) {
    puts("maptest\n");

    /* --- the promise costs nothing ------------------------------------- */
    u32 before = free_kb();
    char *p = (char *)map(BIG, PROT_READ | PROT_WRITE);
    ok("sixty-four megabytes can be asked for", p != 0);
    if (!p) { puts("MAPTEST_FAIL\n"); return 1; }

    u32 after_map = free_kb();
    int cost_kb = (int)before - (int)after_map;
    ok("and asking costs almost nothing", cost_kb < 256);
    puts("      asking for 65536 KiB moved memory by ");
    putn(cost_kb);
    puts(" KiB\n");

    /* --- and touching costs what it touched ---------------------------- */
    //
    // A page at a time, a megabyte apart, so each one is its own fault
    // rather than the same page written twice.
    for (u32 i = 0; i < 16; i++) p[i * MEG] = (char)(i + 1);
    u32 after_touch = free_kb();
    int touched_kb = (int)after_map - (int)after_touch;
    ok("and touching sixteen pages costs about sixteen pages",
       touched_kb >= 64 && touched_kb < 512);
    puts("      touching 16 pages moved memory by ");
    putn(touched_kb);
    puts(" KiB\n");

    /* --- what is in them ------------------------------------------------ */
    int zeroed = 1;
    for (u32 i = 0; i < 16; i++)
        for (u32 j = 1; j < 64; j++)
            if (p[i * MEG + j] != 0) zeroed = 0;
    ok("a page arrives zeroed", zeroed);

    int kept = 1;
    for (u32 i = 0; i < 16; i++)
        if (p[i * MEG] != (char)(i + 1)) kept = 0;
    ok("and what was written to it stays written", kept);

    /* --- the end of it is the end of it --------------------------------- */
    //
    // One page past the mapping. A kernel that answers every fault by
    // handing over a page would grow the mapping forever and never say no,
    // which is a program that cannot be wrong about its own memory.
    int kid = fork();
    if (kid == 0) {
        volatile char *past = (volatile char *)(p + BIG);
        past[0] = 1;
        exit(0);
    }
    ok("and a page past the end is not the program's",
       kid > 0 && wait_for(kid) == FAULTED);

    /* --- a child inherits the promise ----------------------------------- */
    int heir = fork();
    if (heir == 0) {
        /* Not touched by the parent, so this page has never existed. */
        p[40 * MEG] = 42;
        exit(p[40 * MEG] == 42 ? 0 : 1);
    }
    ok("a child can use what its parent mapped",
       heir > 0 && wait_for(heir) == 0);

    /* What the child wrote is the child's. The page was never in this
       program's copy of the address space, so it is still not. */
    ok("and what the child wrote is the child's", p[40 * MEG] == 0);

    /* --- giving it back -------------------------------------------------- */
    ok("a mapping can be given back", unmap(p, BIG) == 0);
    u32 after_unmap = free_kb();
    ok("and the pages that arrived come back",
       (int)after_unmap - (int)after_touch >= 48);
    puts("      giving it back moved memory by ");
    putn((int)after_unmap - (int)after_touch);
    puts(" KiB\n");

    /* And the range stops being the program's. */
    int gone = fork();
    if (gone == 0) { p[0] = 1; exit(0); }
    ok("and it is not the program's any more",
       gone > 0 && wait_for(gone) == FAULTED);

    /* --- more than the kernel will hold ---------------------------------- */
    int held = 0;
    for (int i = 0; i < 32; i++)
        if (map(4096, PROT_READ | PROT_WRITE)) held++;
    ok("more mappings than there are slots is refused, not invented",
       held > 0 && held <= 16);
    puts("      ");
    putn(held);
    puts(" of 32 were given\n");

    puts(fails ? "MAPTEST_FAIL\n" : "MAPTEST_PASS\n");
    return fails ? 1 : 0;
}
