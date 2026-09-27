/* cputest — a process's memory stays its own, on whichever processor it runs.
 *
 * Three faults in the kernel, each of which let one process's memory be
 * handled through somebody else's idea of it:
 *
 *   the kernel wrote its answers straight through copy on write pages. After
 *     a fork both processes share every page read-only, and a system call
 *     writing into the child's buffer changed the parent's copy, because
 *     nothing had told the processor that ring 0 writes obey read-only
 *
 *   a copy on write fault, once resolved, went back to the program still
 *     holding the kernel lock, keeping every other processor out of the
 *     kernel until the next interrupt happened along
 *
 *   the kernel kept one record of "the address space in use" for the whole
 *     machine, so a program on one processor had its pointers checked against
 *     whichever space another processor had switched to last
 *
 * The first two show on any machine. The third needs more than one
 * processor, so smpcheck runs this with four as well as ring3check with one.
 */
#include "zelr.h"

static int fails = 0;

static void ok(const char *what, int cond) {
    puts(cond ? "  ok   " : "  FAIL ");
    puts(what);
    putc('\n');
    if (!cond) fails++;
}

/* A page of its own, touched by nobody after the fork except through the
   one system call that is the point of the check. */
static char shared[4096] __attribute__((aligned(4096)));

/* Pages for the children to write to after the fork, one copy on write
   fault each. */
#define CHURN_PAGES 64
static unsigned int churn[CHURN_PAGES * 1024] __attribute__((aligned(4096)));

#define WORKERS 4
#define ROUNDS  3000

/* The sum of every processor's "kept" in /sys/cpu. */
static int kept_now(void) {
    static char buf[4096];
    int fd = open("/sys/cpu", O_READ);
    if (fd < 0) return -1;
    int n = zelr_fread(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    int total = 0;
    for (int i = 0; i + 5 <= n; i++) {
        if (buf[i] != ' ' || buf[i + 1] != 'k' || buf[i + 2] != 'e' ||
            buf[i + 3] != 'p' || buf[i + 4] != 't') continue;
        int j = i - 1, v = 0, scale = 1;
        while (j >= 0 && buf[j] >= '0' && buf[j] <= '9') {
            v += (buf[j] - '0') * scale;
            scale *= 10;
            j--;
        }
        total += v;
    }
    return total;
}

/* System calls that write into the caller's memory, over and over, with a
   copy on write fault in the middle of each lap. Answers how many of them
   were refused or came back wrong. */
static int work(int who) {
    int cwd_bad = 0, info_bad = 0, page_bad = 0;
    for (int i = 0; i < ROUNDS; i++) {
        /* Two cheap calls that both write into this program's memory.
           Not sysinfo, which counts the disk's free space every time and
           made a run of this take a hundred seconds. */
        char cwd[64];
        zelr_netinfo ni;
        cwd[0] = 0;
        if (getcwd(cwd, sizeof(cwd)) <= 0 || cwd[0] != '/') cwd_bad++;
        if (netinfo(&ni) != 0) info_bad++;
        if (i < CHURN_PAGES) churn[i * 1024] = (unsigned int)(who * 100000 + i);
    }
    /* What it wrote to its own copies is still there. */
    for (int i = 0; i < CHURN_PAGES; i++)
        if (churn[i * 1024] != (unsigned int)(who * 100000 + i)) page_bad++;

    /* Said by the worker, since an exit status carries one number. */
    if (cwd_bad || info_bad || page_bad) {
        puts("      worker "); putn(who);
        puts(": getcwd refused "); putn(cwd_bad);
        puts(", netinfo refused "); putn(info_bad);
        puts(", pages wrong "); putn(page_bad); putc('\n');
    }
    return cwd_bad + info_bad + page_bad;
}

int main(void) {
    puts("cputest\n");

    /* --- a system call writing into a page shared after a fork ------------ */
    for (int i = 0; i < 4096; i++) shared[i] = 'P';
    int kid = fork();
    if (kid == 0) {
        /* The kernel writes the directory into `shared`. From ring 3 this
           page has not been touched since the fork, so the only thing that
           can give the child its own copy is the kernel's write faulting. */
        int n = getcwd(shared, 64);
        exit(n > 0 && shared[0] == '/' ? 0 : 1);
    }
    int status = wait_for(kid);
    ok("the child's system call got its answer", status == 0);
    ok("and it went into the child's copy, not the parent's", shared[0] == 'P');

    /* --- copy on write faults, and many calls, on several processors ------ */
    for (int i = 0; i < CHURN_PAGES; i++) churn[i * 1024] = 7;
    int kept_before = kept_now();
    ok("the processors' counts can be read", kept_before >= 0);

    /* Each worker says how it went down a pipe as its last act. Its exit
       status is not enough on a busy machine: a finished task's record is
       kept ten seconds for whoever will wait on it, and with the processors
       full the parent can reach the last worker later than that. */
    int ends[2];
    ok("a pipe for the workers to answer down", pipe(ends) == 0);
    int pids[WORKERS];
    for (int w = 0; w < WORKERS; w++) {
        pids[w] = fork();
        if (pids[w] == 0) {
            close(ends[0]);
            char verdict = work(w + 1) ? 'B' : 'G';
            zelr_fwrite(ends[1], &verdict, 1);
            exit(0);
        }
    }
    close(ends[1]);

    /* A worker ended by a fault never writes, so the pipe runs dry short.
       A read also gives up after ten quiet seconds (pipe.c) and answers
       nothing, the same as the end of the pipe, so it is asked again a
       bounded number of times; at the real end each ask returns at once. */
    int clean = 0, heard = 0;
    char got[WORKERS];
    for (int tries = 0; heard < WORKERS && tries < 30; tries++) {
        int n = zelr_fread(ends[0], got + heard, WORKERS - heard);
        if (n < 0) break;
        heard += n;
    }
    close(ends[0]);
    for (int i = 0; i < heard; i++) if (got[i] == 'G') clean++;
    int died = 0;
    for (int w = 0; w < WORKERS; w++) if (pids[w] > 0 && wait_for(pids[w]) == 139) died++;

    ok("every worker lived to say how it went", heard == WORKERS && died == 0);
    ok("every call from every worker found the caller's memory", clean == WORKERS);

    int kept_after = kept_now();
    ok("no processor went back to a program still holding the kernel lock",
       kept_after == kept_before);
    if (kept_after != kept_before) {
        puts("      kept went from "); putn(kept_before);
        puts(" to "); putn(kept_after); putc('\n');
    }

    /* And the parent's pages are still the parent's. */
    int mine = 1;
    for (int i = 0; i < CHURN_PAGES; i++) if (churn[i * 1024] != 7) mine = 0;
    ok("the parent's pages kept what the parent wrote", mine);

    puts(fails ? "CPUTEST_FAIL\n" : "CPUTEST_PASS\n");
    return fails ? 1 : 0;
}
