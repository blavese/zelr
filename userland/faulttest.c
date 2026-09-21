/* faulttest — a program that does something it cannot do, and a machine
 * that is still there afterwards.
 *
 * Until this, every exception went to panic. A ring 3 program dereferencing
 * a null pointer, dividing by zero or jumping into nothing took the whole
 * machine with it: the desktop, every other program, and whatever the disk
 * was in the middle of. The account of it was a panic screen naming the
 * program, which is the right information attached to entirely the wrong
 * outcome.
 *
 * The check cannot be made from inside the program that faults, because
 * that program is meant to stop. So this forks, lets the child do the
 * forbidden thing, and asks two questions the parent is in a position to
 * answer: did the child end, and is this program -- and therefore the
 * machine -- still running.
 *
 * The second is the one that matters. A machine that panicked would never
 * reach the next line, and a check that never runs prints nothing, which is
 * why the last thing here is a marker and not a verdict.
 */
#include "zelr.h"

static int fails = 0;

static void ok(const char *what, int cond) {
    puts(cond ? "  ok   " : "  FAIL ");
    puts(what);
    putc('\n');
    if (!cond) fails++;
}

/* 139 is what a shell prints for a program killed by a memory fault,
   everywhere. There is no SIGSEGV to catch here, so the number is the
   convention rather than a signal that was delivered. */
#define FAULTED 139

/* Each of these is a different exception vector arriving at the same place.
   Written so the compiler cannot fold them away: it is entitled to assume a
   program never dereferences null, and an optimiser that acts on that
   assumption removes the very thing being tested. */
static volatile int *const nowhere = (volatile int *)0;
static volatile int zero;

/* Where the answer goes. Without somewhere observable to put it the
   compiler deletes the division: dividing by zero is undefined, so it is
   entitled to assume this never happens and to remove the instruction that
   would prove otherwise. It did, and the check reported a machine that
   let a divide by zero through when no division had been compiled. */
static volatile int sink;

/* The kernel's half, in a variable rather than a constant, for the same
   reason: a constant that cannot be a valid pointer is one the compiler is
   free to fold away. The first version of this faulted on address zero
   while claiming to have read the kernel's memory, which is a check that
   passes for a reason that is not the one it names. */
static volatile u64 far_away = 0xFFFF800000000000ull;

static int child_reads_null(void) { sink = nowhere[0]; return 0; }
static int child_writes_null(void) { nowhere[0] = 1; return 0; }
static int child_divides_by_zero(void) { int d = zero; sink = 7 / d; return 0; }

static int child_reads_the_kernel(void) {
    volatile u64 *theirs = (volatile u64 *)far_away;
    sink = (int)theirs[0];
    return 0;
}

static int faulting_child(int (*fn)(void)) {
    int kid = fork();
    if (kid == 0) {
        fn();
        /* Only here if the machine let it through, which is its own
           failure and a different one from the machine stopping. */
        exit(0);
    }
    if (kid < 0) return -1;
    return wait_for(kid);
}

int main(void) {
    puts("faulttest\n");

    ok("a program that reads a null pointer ends",
       faulting_child(child_reads_null) == FAULTED);
    ok("and the machine is still here", 1);

    ok("a program that writes one ends",
       faulting_child(child_writes_null) == FAULTED);

    ok("a program that divides by zero ends",
       faulting_child(child_divides_by_zero) != 0);

    ok("a program that reads the kernel's half of memory ends",
       faulting_child(child_reads_the_kernel) == FAULTED);

    /* --- and the machine still works, rather than merely still existing -- */
    //
    // Being alive is not the same as being well. A kernel that caught the
    // fault and leaked the frames, the descriptors or the window the child
    // held would pass everything above and run out of memory an hour later.
    // So this asks the machine for its own numbers, either side of a
    // hundred faults, and requires the free page count to come back.
    zelr_sysinfo before, after;
    sysinfo(&before);

    int ended = 0;
    for (int i = 0; i < 100; i++)
        if (faulting_child(child_writes_null) == FAULTED) ended++;
    ok("a hundred of them all end the same way", ended == 100);
    if (ended != 100) { puts("      "); putn(ended); puts(" of 100\n"); }

    sysinfo(&after);

    /* Some drift is ordinary: the shell, the console and this program are
       all doing things. What is not ordinary is a hundred programs' worth
       of memory never coming back. Each one is a fresh address space with
       a stack, an image and a heap in it. */
    int lost = (int)before.mem_free_kb - (int)after.mem_free_kb;
    ok("and the memory they held comes back", lost < 4096);
    puts("      memory moved by ");
    putn(lost);
    puts(" KiB across a hundred faults\n");

    puts(fails ? "FAULTTEST_FAIL\n" : "FAULTTEST_PASS\n");
    return fails ? 1 : 0;
}
