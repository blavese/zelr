/* fork and exec, which are the two calls a process model is made of.
 *
 * The thing to prove is not that fork returns a number. It is that there are
 * genuinely two processes afterwards and that they do not share memory: a
 * child that writes to a variable must not be writing to the parent's copy
 * of it, or the whole model is a thread pretending.
 *
 * So most of what is below writes to something on both sides and then looks.
 * A shared address space passes a test that only checks the return value and
 * fails every one of these.
 */
#include "zelr.h"
#include "alloc.h"

static int failed;

static void ok(const char *what, int cond) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    putc('\n');
    if (!cond) failed++;
}

static void okn(const char *what, int cond, int n) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    puts("  ");
    putn(n);
    putc('\n');
    if (!cond) failed++;
}

/* Something in the data segment, so both sides start out agreeing about it
   and can then disagree. */
static int shared_counter = 100;
static char shared_text[64] = "untouched";

int main(void) {
    puts("fork and exec, in ring 3\n");

    /* --- it returns twice ------------------------------------------------ */
    {
        int pid = fork();
        if (pid == 0) {
            /* The child. It exits with a number the parent checks, which is
               the only way it can say anything the parent will believe. */
            shared_counter = 7;
            exit(shared_counter == 7 ? 42 : 1);
        }
        ok("fork returns a pid to the parent", pid > 0);
        int status = wait_for(pid);
        okn("and the child ran and exited with what it meant to",
            status == 42, status);
        okn("and the parent's own copy was not touched",
            shared_counter == 100, shared_counter);
    }

    /* --- memory really is separate --------------------------------------- */
    {
        /* A heap block, written by the child, checked by the parent. If the
           address space were shared this would come back changed. */
        char *buf = (char *)malloc(64);
        if (!buf) { ok("a heap block to fork over", 0); return 1; }
        buf[0] = 'a';
        buf[1] = 0;

        int pid = fork();
        if (pid == 0) {
            buf[0] = 'z';
            for (int i = 0; i < 64; i++) shared_text[i] = 0;
            shared_text[0] = 'c';
            exit(buf[0] == 'z' && shared_text[0] == 'c' ? 17 : 1);
        }
        int status = wait_for(pid);
        okn("the child could write to its own copy of the heap",
            status == 17, status);
        ok("and the parent's heap block still says what it said",
           buf[0] == 'a');
        ok("and so does the parent's static text",
           shared_text[0] == 'u');
        free(buf);
    }

    /* --- the child knows who its parent is -------------------------------- */
    {
        int me = getpid();
        int pid = fork();
        if (pid == 0) exit(getppid() == me ? 55 : 1);
        okn("a child can name its parent", wait_for(pid) == 55, me);
    }

    /* --- several children at once ----------------------------------------- */
    {
        int pids[4];
        int made = 0;
        for (int i = 0; i < 4; i++) {
            int pid = fork();
            if (pid == 0) exit(i + 1);
            if (pid > 0) pids[made++] = pid;
        }
        okn("four children were made", made == 4, made);

        int sum = 0;
        for (int i = 0; i < made; i++) {
            int st = wait_for(pids[i]);
            if (st > 0) sum += st;
        }
        okn("and each exited with its own number, summing to ten",
            sum == 10, sum);
    }

    /* --- the heap keeps working on both sides ------------------------------ */
    {
        int pid = fork();
        if (pid == 0) {
            /* A child allocating after the fork must not be handed the
               parent's addresses to write over; it has its own break and
               its own pages behind it. */
            int good = 1;
            for (int i = 0; i < 200; i++) {
                char *p = (char *)malloc(300);
                if (!p) { good = 0; break; }
                for (int k = 0; k < 300; k++) p[k] = (char)(i + k);
                free(p);
            }
            exit(good ? 23 : 1);
        }
        okn("a child can allocate after the fork", wait_for(pid) == 23, 23);

        char *mine = (char *)malloc(256);
        ok("and so can the parent, afterwards", mine != 0);
        free(mine);
    }

    /* --- exec -------------------------------------------------------------- */
    {
        /* A child that becomes another program entirely. `count` is the
           smallest thing in /bin and exits with a known number. */
        int pid = fork();
        if (pid == 0) {
            exec("/bin/count", 0);
            /* Only reached when exec failed, which is the whole reason it
               has a return value at all. */
            exit(99);
        }
        int status = wait_for(pid);
        okn("a forked child can become another program", status != 99, status);
    }

    {
        int pid = fork();
        if (pid == 0) {
            int rc = exec("/bin/there-is-no-such-thing", 0);
            exit(rc == 0 ? 1 : 64);
        }
        okn("and exec says no rather than losing the process",
            wait_for(pid) == 64, 64);
    }

    puts(failed ? "FORKTEST_FAIL\n" : "FORKTEST_PASS\n");
    return failed;
}
