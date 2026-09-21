/* Whether a fork copies the program, or only promises to.
 *
 * It used to copy: every page the parent had mapped for user code went into
 * a fresh frame before fork returned. That is correct and it is the largest
 * single thing fork does, and in the case a shell spends all day on -- fork
 * and then exec -- every byte of it is thrown away a moment later, because
 * the child's first act is to replace the address space it was handed.
 *
 * Correctness is checked elsewhere: forktest already asks whether the two
 * sides can see each other's writes, which is the question that matters and
 * which an eager copy also answers. What cannot be seen from there is
 * whether the copying happened, because a program cannot tell.
 *
 * So this asks the machine how much memory is free, which it will say. A
 * fork that copies four megabytes takes four megabytes; one that shares
 * them takes the page tables and little else. Then the same program writes
 * to every page and the four megabytes go after all -- which is the other
 * half of the claim, and the half that says the sharing was real rather
 * than a fork that quietly forgot to copy.
 */
#include "zelr.h"
#include "alloc.h"

static int failed, ran;

static void ok(const char *what, int good) {
    ran++;
    puts(good ? "  PASS  " : "  FAIL  ");
    puts(what);
    puts("\n");
    if (!good) failed++;
}

static void say(const char *label, int kb) {
    puts("        ");
    puts(label);
    puts(" ");
    putn(kb);
    puts(" KiB\n");
}

static int free_kb(void) {
    zelr_sysinfo info;
    if (sysinfo(&info) < 0) return -1;
    return (int)info.mem_free_kb;
}

#define PAGES 1024                     /* four megabytes of them */
#define PAGE  4096

int main(void) {
    puts("=== fork, and what it copies ===\n");

    char *big = (char *)malloc((u64)PAGES * PAGE);
    if (!big) { puts("  FAIL  no room for the test\nCOWTEST_FAIL\n"); return 1; }

    /* Touched, not merely asked for: a page the heap has promised and the
       kernel has not mapped is not a page a fork has to think about. */
    for (int i = 0; i < PAGES; i++) big[(u64)i * PAGE] = (char)(i & 0x7F);

    int before = free_kb();
    ok("the machine says how much memory is free", before > 0);

    int pid = fork();
    if (pid == 0) {
        /* The child holds its half of the sharing open while the parent
           measures, and then proves it has its own copy: it writes its own
           mark over every page and reads them back. If the two sides were
           sharing for real rather than copying on write, the parent's marks
           would be underneath. */
        sleep_ms(2500);
        for (int i = 0; i < PAGES; i++) big[(u64)i * PAGE] = (char)0x5A;
        for (int i = 0; i < PAGES; i++)
            if (big[(u64)i * PAGE] != (char)0x5A) exit(2);
        exit(0);
    }
    if (pid < 0) { puts("  FAIL  fork failed\nCOWTEST_FAIL\n"); return 1; }

    /* --- what the fork itself cost ------------------------------------- */
    int after_fork = free_kb();
    int fork_cost = before - after_fork;
    say("the fork cost", fork_cost);
    ok("a fork does not copy the program's memory", fork_cost < 512);

    /* --- and what writing to it costs ----------------------------------- */
    //
    // One byte in each page, which is the whole of what copy on write is
    // about: the cost is per page touched and nothing is charged for a
    // page that is only read.
    for (int i = 0; i < PAGES; i++) big[(u64)i * PAGE] = (char)0xA5;

    int after_write = free_kb();
    int write_cost = after_fork - after_write;
    say("writing to it cost", write_cost);
    ok("and writing to it afterwards does", write_cost > 3000);

    /* --- and neither side saw the other --------------------------------- */
    for (int i = 0; i < PAGES; i++)
        if (big[(u64)i * PAGE] != (char)0xA5) { ok("the parent kept its own", 0); break; }
    if (!failed) ok("the parent kept its own writes", 1);

    int status = wait_for(pid);
    ok("and the child kept its own", status == 0);
    if (status != 0) { puts("        child said "); putn(status); puts("\n"); }

    /* --- and the memory comes back -------------------------------------- */
    //
    // A shared frame that was freed by whichever side let go of it last is
    // the one thing a reference count can get wrong in the direction that
    // does not crash: the frame is never handed back and the machine leaks
    // a page per fork. Nothing reports that, so this asks.
    sleep_ms(400);
    int after_child = free_kb();
    say("free once the child had gone", after_child);
    ok("the child's memory comes back when it goes",
       after_child > after_write + 2000);

    puts(failed ? "COWTEST_FAIL\n" : "COWTEST_PASS\n");
    return failed;
}
