/* Descriptors, across two processes.
 *
 * The kernel checks what one program can do with them on its own. What it
 * cannot check is the half that only exists once there are two: that a child
 * inherits what the parent had open, that the numbers are the child's own,
 * and that a pipe carries bytes from one process to another and then says
 * where it ends.
 *
 * Everything a shell does is in here somewhere. `cmd > file` is the
 * redirection test; `a | b` is the pipe test; both of them depend on the
 * inheritance test, because the whole trick is that the child arranges its
 * descriptors before the program it becomes ever starts.
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

static void okn(const char *what, int cond, int n) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    puts("  ");
    putn(n);
    putc('\n');
    if (!cond) failed++;
}

static int same(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int main(void) {
    puts("descriptors, across two processes\n");

    unlink("/fdt.txt");

    /* --- the numbers a program is born with ------------------------------ */
    {
        int fd = open("/fdt.txt", O_WRITE | O_CREATE);
        okn("the first file a program opens is three", fd == 3, fd);
        fwrite(fd, "one two three", 13);
        close(fd);
    }

    /* --- a child inherits what was open ---------------------------------- */
    {
        int fd = open("/fdt.txt", O_READ);
        if (fd < 0) { ok("a file to inherit", 0); return 1; }

        int pid = fork();
        if (pid == 0) {
            /* The same number, in a different process, still refers to the
               same open file — and to the same position in it. */
            char buf[8];
            int n = fread(fd, buf, 3);
            exit(n == 3 && same(buf, "one", 3) ? 31 : 1);
        }
        okn("a child can read the file its parent had open",
            wait_for(pid) == 31, 31);

        /* And the position moved for the parent too, because both sides are
           looking at one open file rather than two copies of one. */
        char buf[8];
        int n = fread(fd, buf, 4);
        ok("and the parent carries on from where the child got to",
           n == 4 && same(buf, " two", 4));
        close(fd);
    }

    /* --- the numbers are the child's own --------------------------------- */
    {
        int fd = open("/fdt.txt", O_READ);          /* 3, in the parent */
        int pid = fork();
        if (pid == 0) {
            /* Three is taken, inherited, so the next is four. Close the
               inherited one and three comes free again — in this process
               only, which is the point of a per process table. */
            int a = open("/fdt.txt", O_READ);
            close(fd);
            int b = open("/fdt.txt", O_READ);
            exit(a == 4 && b == 3 ? 32 : 1);
        }
        okn("a child's descriptor numbers are its own", wait_for(pid) == 32, 32);

        /* The child closed its 3. The parent's is untouched. */
        char buf[4];
        ok("and closing one in the child did not close the parent's",
           fread(fd, buf, 3) == 3 && same(buf, "one", 3));
        close(fd);
    }

    /* --- redirection ------------------------------------------------------
     *
     * This is `cmd > file`, written out. The child points its descriptor 1
     * at a file and then writes with `puts`, which knows nothing about any
     * of this: it writes to 1, as it always has. */
    {
        unlink("/fdt2.txt");
        int pid = fork();
        if (pid == 0) {
            int fd = open("/fdt2.txt", O_WRITE | O_CREATE | O_TRUNC);
            if (fd < 0) exit(1);
            dup2(fd, 1);
            close(fd);
            puts("redirected");
            exit(33);
        }
        okn("a child can send its output to a file", wait_for(pid) == 33, 33);

        char buf[32];
        int fd = open("/fdt2.txt", O_READ);
        int n = fd >= 0 ? fread(fd, buf, sizeof(buf)) : -1;
        close(fd);
        okn("and what it printed is in the file, not on the screen",
            n == 10 && same(buf, "redirected", 10), n);
        unlink("/fdt2.txt");
    }

    /* --- a pipe between two processes -------------------------------------
     *
     * The left hand side of `a | b`. Both ends exist in both processes after
     * the fork, so each side closes the one it is not using: if the parent
     * kept the writing end open, the reader would never see the end of the
     * file, because as far as the kernel is concerned there would still be
     * somebody who might write. That is the classic mistake, and the check
     * below is the one that catches it. */
    {
        int ends[2];
        ok("a pipe can be made", pipe(ends) == 0);

        int pid = fork();
        if (pid == 0) {
            close(ends[0]);
            fwrite(ends[1], "down the pipe", 13);
            close(ends[1]);
            exit(34);
        }
        close(ends[1]);                 /* the parent will not be writing */

        char buf[32];
        int n = fread(ends[0], buf, sizeof(buf));
        okn("and what a child writes into it", n == 13 && same(buf, "down the pipe", 13), n);
        okn("comes out in the parent", wait_for(pid) == 34, 34);

        /* Nobody is left to write, so this is the end of the file rather
           than a wait that never finishes. */
        okn("and then it says it has ended", fread(ends[0], buf, 4) == 0, 0);
        close(ends[0]);
    }

    /* --- a pipeline, the way a shell builds one ---------------------------
     *
     * Two children, neither of which knows the other exists: the first has
     * its output pointed at the writing end, the second its input pointed at
     * the reading end. Here both are this same program pretending, because
     * what is being checked is the plumbing rather than the programs. */
    {
        int ends[2];
        if (pipe(ends) != 0) { ok("a pipe for the pipeline", 0); return 1; }

        int left = fork();
        if (left == 0) {
            close(ends[0]);
            dup2(ends[1], 1);
            close(ends[1]);
            puts("carried");            /* straight into the pipe */
            exit(0);
        }

        int right = fork();
        if (right == 0) {
            close(ends[1]);
            dup2(ends[0], 0);
            close(ends[0]);
            char buf[32];
            int n = fread(0, buf, sizeof(buf));
            exit(n == 7 && same(buf, "carried", 7) ? 35 : 1);
        }

        /* The shell holds neither end once both sides have them. */
        close(ends[0]);
        close(ends[1]);
        wait_for(left);
        okn("one program's output becomes another's input",
            wait_for(right) == 35, 35);
    }

    /* --- what is refused --------------------------------------------------- */
    {
        char buf[4];
        ok("a number that was never opened cannot be read",
           fread(11, buf, 1) < 0);
        ok("nor written to", fwrite(11, "x", 1) < 0);
        ok("nor duplicated", dup(11) < 0);
        ok("and closing it says so rather than pretending", close(11) < 0);
    }

    unlink("/fdt.txt");
    puts(failed ? "FDTEST_FAIL\n" : "FDTEST_PASS\n");
    return failed;
}
