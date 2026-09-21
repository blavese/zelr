/* Whether a sleep from ring 3 sleeps.
 *
 * It did not. The scheduler walked its ring, found nothing else runnable,
 * and handed the processor back to the task that had just asked to sleep,
 * there being no idle task to give it to instead. So sleep_ms returned
 * immediately whenever nothing else wanted the machine, which on a machine
 * running one program is almost always.
 *
 * Nothing reports that. What it looks like from outside is a program that
 * spins, and the four things it actually caused were a browser that starved
 * every other task, typing that went lossy under load, a compositor that
 * caught every frame half drawn, and timers in page scripts that had to be
 * checked by polling the clock instead of by sleeping.
 *
 * Which is why this is checked against the clock rather than by asking the
 * kernel what it did. A sleep that returns and says it slept is exactly the
 * failure; the only witness that cannot be fooled is time passing.
 *
 * The margins are wide on purpose. This runs inside an emulator on a busy
 * host, so a sleep can be late by a lot and that is not a fault. What is a
 * fault is a sleep that is early, and early is what is measured.
 */
#include "zelr.h"

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

static int failed, ran;

static void ok(const char *what, int good) {
    ran++;
    puts(good ? "  PASS  " : "  FAIL  ");
    puts(what);
    puts("\n");
    if (!good) failed++;
}

/* The clock runs at a hundred hertz, so a tick is ten milliseconds. */
static int elapsed_ms(int from, int to) { return (to - from) * 10; }

int main(void) {
    puts("=== sleeping ===\n");

    /* Warm up: the first call after a program starts can be late for
       reasons that have nothing to do with this. */
    sleep_ms(20);

    /* --- one sleep ------------------------------------------------------ */
    {
        int a = ticks();
        sleep_ms(100);
        int took = elapsed_ms(a, ticks());
        ok("a hundred milliseconds takes at least ninety", took >= 90);
        if (took < 90) { puts("        took "); putn(took); puts(" ms\n"); }
        ok("and is not wildly longer than it asked for", took < 600);
    }

    /* --- a short one ----------------------------------------------------
     *
     * This is the one the arithmetic used to lose. A sleep of five
     * milliseconds is half a tick, and (ms * hz) / 1000 truncated it to no
     * ticks at all -- so the task was marked sleeping with a wake time
     * already in the past and was runnable again before the switch. Asking
     * to sleep is asking to give the processor away, and a request too
     * short to be counted should still do that much. */
    {
        int a = ticks();
        for (int i = 0; i < 20; i++) sleep_ms(5);
        int took = elapsed_ms(a, ticks());
        ok("twenty sleeps of five milliseconds are not free", took >= 100);
        if (took < 100) { puts("        took "); putn(took); puts(" ms\n"); }
    }

    /* --- many in a row --------------------------------------------------- */
    {
        int a = ticks();
        for (int i = 0; i < 25; i++) sleep_ms(20);
        int took = elapsed_ms(a, ticks());
        ok("twenty five sleeps of twenty add up", took >= 450);
        if (took < 450) { puts("        took "); putn(took); puts(" ms\n"); }
    }

    /* --- the frame pace a graphical program actually uses ---------------- */
    {
        int a = ticks();
        for (int i = 0; i < 30; i++) sleep_ms(16);
        int took = elapsed_ms(a, ticks());
        ok("thirty frames at sixteen milliseconds take real time",
           took >= 300);
        if (took < 300) { puts("        took "); putn(took); puts(" ms\n"); }
    }

    /* --- and zero is still zero ------------------------------------------
     *
     * Rounding up must not turn a request for nothing into a tick. A
     * program that sleeps for zero is asking to give up the rest of its
     * slice, not to wait. */
    {
        int a = ticks();
        for (int i = 0; i < 50; i++) sleep_ms(0);
        int took = elapsed_ms(a, ticks());
        ok("fifty sleeps of nothing do not add up to much", took < 200);
        if (took >= 200) { puts("        took "); putn(took); puts(" ms\n"); }
    }

    /* --- the clock itself ------------------------------------------------
     *
     * Everything above trusts ticks(), so the last check is that ticks()
     * moves on its own and never backwards. A frozen clock would make every
     * measurement above read zero and would look exactly like the fault
     * being checked for. */
    {
        int last = ticks(), backwards = 0, moved = 0;
        for (int i = 0; i < 40; i++) {
            sleep_ms(10);
            int now = ticks();
            if (now < last) backwards++;
            if (now > last) moved++;
            last = now;
        }
        ok("the clock never runs backwards", backwards == 0);
        ok("and it does move", moved > 20);
    }

    puts(failed ? "SLEEPTEST_FAIL\n" : "SLEEPTEST_PASS\n");
    return failed;
}
