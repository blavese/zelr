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

int main(void) {
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

    puts(fails ? "SIGTEST_FAIL\n" : "SIGTEST_PASS\n");
    return fails ? 1 : 0;
}
