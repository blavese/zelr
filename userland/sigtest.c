/* sigtest — a program told about a signal that carries on afterwards.
 *
 * A handler is the kernel interrupting a program between two of its own
 * instructions, persuading it to call a function it never called, and then
 * putting it back as though nothing had happened. So what is worth checking
 * is not only that the function ran:
 *
 *   it ran, and with the number it was raised on
 *   the program carried on from where it was, not from where it would like
 *     to have been
 *   arithmetic in progress across the signal came out right, which is the
 *     only way to see that every register was put back
 *   a second one of the same kind waits for the first to finish rather than
 *     landing on top of it
 *   SIGKILL cannot be caught, and SIG_IGN still means ignored
 *   a child inherits handlers, and exec forgets them
 *
 * The register check is the one that matters. A handler runs the compiler's
 * code, which uses whatever registers it likes, and a kernel that saved
 * fifteen of the sixteen would pass every other check here and corrupt one
 * long-running calculation in a thousand.
 */
#include "zelr.h"

static int fails = 0;

static void ok(const char *what, int cond) {
    puts(cond ? "  ok   " : "  FAIL ");
    puts(what);
    putc('\n');
    if (!cond) fails++;
}

/* Touched by the handler and read by the program, so the compiler must not
   keep either in a register across the call it cannot see. */
static volatile int caught;
static volatile int caught_sig;
static volatile int depth;
static volatile int deepest;
static volatile int resend;

static void on_signal(int sig) {
    caught++;
    caught_sig = sig;

    depth++;
    if (depth > deepest) deepest = depth;

    /* Raised against itself from inside its own handler, once. A kernel
       that enters the handler again here rather than holding the signal
       until this one returns would show a depth of two. */
    if (resend) {
        resend = 0;
        send_signal(getpid(), SIGTERM);
        /* Long enough that a kernel willing to re-enter would have. */
        for (volatile int i = 0; i < 400000; i++) { }
    }
    depth--;
}

static void quiet(int sig) { (void)sig; caught++; }

/* A handler that does arithmetic, as any compiled handler may, and then
   leaves the vector registers and the rounding mode as it pleases. */
static volatile double spent;
static void fp_handler(int sig) {
    (void)sig;
    caught++;
    double x = 1.5;
    for (int i = 0; i < 100; i++) x = x * 1.0001 + 0.5;
    spent = x;
    unsigned mode = 0x7F80;                    /* round toward zero */
    __asm__ volatile ("ldmxcsr %0" :: "m"(mode));
    __asm__ volatile ("xorps %%xmm0, %%xmm0" ::: "xmm0");
}

static unsigned read_mxcsr(void) {
    unsigned m;
    __asm__ volatile ("stmxcsr %0" : "=m"(m));
    return m;
}

static void write_mxcsr(unsigned m) { __asm__ volatile ("ldmxcsr %0" :: "m"(m)); }

/* Run with "fpu-probe" by a child that set an odd rounding mode and then
   ran this program again: what a new program finds in the unit. */
static int fpu_probe(void) {
    unsigned short cw;
    __asm__ volatile ("fnstcw %0" : "=m"(cw));
    return read_mxcsr() == 0x1F80 && cw == 0x037F ? 0 : 1;
}

/* Arithmetic that uses enough registers at once that a kernel which lost
   one would get a different answer. Deliberately not a sum: a sum of a
   known run is a number that can be right by accident. */
static unsigned churn(unsigned rounds) {
    unsigned a = 1, b = 2, c = 3, d = 5, e = 7, f = 11, g = 13, h = 17;
    for (unsigned i = 0; i < rounds; i++) {
        a += b ^ i;   b += c + (a >> 3);  c += d ^ (b >> 2);  d += e + (c >> 1);
        e += f ^ (d >> 4); f += g + (e >> 2); g += h ^ (f >> 3); h += a + (g >> 1);
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h;
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == 'f' && argv[1][1] == 'p' && argv[1][2] == 'u')
        return fpu_probe();

    puts("sigtest\n");

    /* --- the same answer, with nothing interrupting it ------------------ */
    unsigned quiet_answer = churn(60000);

    /* --- caught, and carried on ----------------------------------------- */
    ok("a handler can be installed", signal(SIGTERM, on_signal) == 0);

    caught = caught_sig = 0;
    resend = 0;
    send_signal(getpid(), SIGTERM);

    /* Delivered on the way out of a system call, so by here it has run. */
    ok("and it runs", caught == 1);
    ok("and is told which signal it was", caught_sig == SIGTERM);
    ok("and the program is still running afterwards", 1);

    /* --- one at a time --------------------------------------------------- */
    caught = 0;
    depth = deepest = 0;
    resend = 1;
    send_signal(getpid(), SIGTERM);
    /* The second one arrives after the first handler returns, on the way
       out of some later call. Nothing here needs it to be quick. */
    for (int i = 0; i < 40 && caught < 2; i++) sleep_ms(20);
    ok("a second one waits rather than landing on top of the first",
       deepest == 1);
    ok("and arrives once the first has returned", caught == 2);
    if (caught != 2) { puts("      caught "); putn(caught); putc('\n'); }

    /* --- what cannot be caught ------------------------------------------ */
    ok("SIGKILL cannot be caught", signal(SIGKILL, quiet) != 0);
    ok("and cannot be ignored", signal(SIGKILL, SIG_IGN) != 0);

    /* --- ignored still means ignored ------------------------------------ */
    ok("a signal can still be ignored", signal(SIGTERM, SIG_IGN) == 0);
    caught = 0;
    send_signal(getpid(), SIGTERM);
    sleep_ms(60);
    ok("and then nothing runs", caught == 0);

    /* --- registers, across a signal that lands in the middle ------------- */
    //
    // The child sends it while this is inside churn, which only the timer
    // can interrupt: no system call is made in there, so the signal arrives
    // on the way back from a tick rather than from a call. That is the path
    // where a register left behind would be left behind.
    ok("a handler can be installed again", signal(SIGTERM, quiet) == 0);
    caught = 0;

    int me = getpid();
    int kid = fork();
    if (kid == 0) {
        sleep_ms(30);
        send_signal(me, SIGTERM);
        exit(0);
    }

    unsigned interrupted_answer = churn(60000);
    wait_for(kid);

    ok("a signal arrives in the middle of a running program", caught >= 1);
    ok("and the arithmetic it interrupted still comes out right",
       interrupted_answer == quiet_answer);
    if (interrupted_answer != quiet_answer) {
        puts("      quiet "); putn((int)quiet_answer);
        puts(" interrupted "); putn((int)interrupted_answer); putc('\n');
    }

    /* --- and the floating point registers across one ------------------- */
    //
    // Set, then the system call that raises the signal, then read back, all
    // in one piece of assembly so the compiler has no chance to move either
    // register in between. The handler runs on the way out of that call and
    // leaves xmm0 zero and the rounding mode changed; nothing used to put
    // them back.
    ok("a handler can be installed for the vector registers",
       signal(SIGTERM, fp_handler) == 0);
    caught = 0;
    static unsigned long long pattern[2] = { 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull };
    static unsigned long long after[2];
    static unsigned mode_in = 0x3F80, mode_out;   /* round down */
    long rax = SYS_SIGSEND;
    __asm__ volatile (
        "movdqu (%[pat]), %%xmm0\n\t"
        "ldmxcsr (%[min])\n\t"
        "int $0x80\n\t"
        "stmxcsr (%[mout])\n\t"
        "movdqu %%xmm0, (%[out])\n\t"
        : "+a"(rax)
        : "b"((long)getpid()), "c"((long)SIGTERM), "d"(0L),
          [pat]"r"(pattern), [min]"r"(&mode_in), [mout]"r"(&mode_out), [out]"r"(after)
        : "xmm0", "memory");
    write_mxcsr(0x1F80);
    ok("the handler ran", caught == 1);
    ok("and the vector registers are the program's again afterwards",
       after[0] == pattern[0] && after[1] == pattern[1]);
    ok("and so is its rounding mode", mode_out == 0x3F80);
    signal(SIGTERM, quiet);

    /* --- a child is the same program ------------------------------------- */
    //
    // Inherited across fork, because the child is running the same code at
    // the same addresses. The child reports by its exit status, because a
    // variable it sets is a variable in its own copy of this memory.
    int heir = fork();
    if (heir == 0) {
        caught = 0;
        send_signal(getpid(), SIGTERM);
        sleep_ms(40);
        exit(caught == 1 ? 0 : 1);
    }
    ok("a child inherits what its parent wanted done with a signal",
       heir > 0 && wait_for(heir) == 0);

    /* --- and exec is a different program ---------------------------------- */
    //
    // A handler address belonged to the program being replaced, so exec
    // forgets it. If it did not, a signal arriving afterwards would jump
    // into whatever the new program happens to have at that address, which
    // is the kind of fault that never reproduces. /bin/count takes no
    // interest in signals, so the default applies and it dies of one:
    // 128 + 15, which is what every shell has reported since the seventies.
    int gone = fork();
    if (gone == 0) {
        char *const v[] = { (char *)"/bin/count", 0 };
        execv("/bin/count", v);
        exit(70);
    }
    sleep_ms(120);
    send_signal(gone, SIGTERM);
    int status = wait_for(gone);
    ok("exec forgets them, so the default applies to the new program",
       status == 128 + SIGTERM);
    if (status != 128 + SIGTERM) {
        puts("      it exited with "); putn(status); putc('\n');
    }

    /* --- and exec starts the unit afresh --------------------------------- */
    //
    // The child rounds down, then becomes this program again with "fpu-probe",
    // which exits 0 only if it finds the state every new program should:
    // MXCSR 0x1F80 and the x87 control word 0x037F. exec used to carry the
    // old program's rounding and masks straight into the new one.
    int fresh = fork();
    if (fresh == 0) {
        write_mxcsr(0x3F80);
        char *const v[] = { (char *)"/bin/sigtest", (char *)"fpu-probe", 0 };
        execv("/bin/sigtest", v);
        exit(70);
    }
    int probed = wait_for(fresh);
    ok("exec gives the new program a fresh floating point state", probed == 0);
    if (probed != 0) { puts("      the probe exited with "); putn(probed); putc('\n'); }

    /* --- delivered onto a stack page not yet touched ---------------------- */
    //
    // The frame goes below the stack pointer, so a signal that arrives with
    // the stack at the foot of its last touched page needs the next one
    // down, which the program was promised but has not reached. It was
    // refused as no stack at all, and the program ended. The child moves
    // its stack to the top of a fresh mapping, where nothing is touched,
    // and raises one against itself from there.
    int low = fork();
    if (low == 0) {
        signal(SIGTERM, on_signal);
        caught = 0;
        resend = 0;
        char *m = (char *)map(64 * 1024, PROT_READ | PROT_WRITE);
        if (!m) exit(71);
        zelr_word top = (zelr_word)(m + 64 * 1024);
        zelr_word r = SYS_SIGSEND;
        __asm__ volatile ("mov %%rsp, %%r12\n"
                          "mov %[top], %%rsp\n"
                          "int $0x80\n"
                          "mov %%r12, %%rsp\n"
                          : "+a"(r)
                          : "b"((zelr_word)getpid()), "c"((zelr_word)SIGTERM), [top] "d"(top)
                          : "r12", "memory");
        exit(caught == 1 ? 0 : 72);
    }
    int lowst = wait_for(low);
    ok("a signal can be delivered onto a stack page not yet touched", lowst == 0);
    if (lowst != 0) { puts("      the child exited with "); putn(lowst); putc('\n'); }

    puts(fails ? "SIGTEST_FAIL\n" : "SIGTEST_PASS\n");
    return fails ? 1 : 0;
}
