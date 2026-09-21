/* polltest — waiting on several descriptors at once.
 *
 * Every read on this system blocks. That is fine for a program doing one
 * thing and impossible for one doing two: reading either descriptor is a
 * commitment to it, and while that read is waiting the other one is not
 * being watched. A shell that wants to notice a keystroke or a pipe filling
 * up, whichever happens first, cannot be written without this.
 *
 * So the checks are about the three answers poll has to get right -- ready,
 * not ready, and gone -- and about the two that are easy to fake:
 *
 *   it returns early when something happens, rather than sleeping out the
 *     whole timeout and reporting what it finds at the end
 *   it returns late when nothing does, rather than returning at once and
 *     letting a caller spin
 *
 * Both of those are measured against the clock, because a poll that always
 * returned immediately and a poll that always waited the full time would
 * each pass every check that only looked at the bits.
 */
#include "zelr.h"

static int fails = 0;

static void ok(const char *what, int cond) {
    puts(cond ? "  ok   " : "  FAIL ");
    puts(what);
    putc('\n');
    if (!cond) fails++;
}

static void say_ms(const char *what, int ms) {
    puts("      ");
    puts(what);
    putn(ms);
    puts(" ms\n");
}

/* Ticks are hundredths here; sysinfo is not needed for a rough elapsed. */
static int ms_since(int began) { return (ticks() - began) * 10; }

int main(void) {
    puts("polltest\n");

    int ends[2];
    ok("a pipe can be made", pipe(ends) == 0);
    if (pipe(ends) != 0 && ends[0] < 0) { puts("POLLTEST_FAIL\n"); return 1; }

    /* --- nothing in it yet ---------------------------------------------- */
    pollfd_t w[2];
    w[0].fd = ends[0]; w[0].events = POLLIN; w[0].revents = 0;

    int began = ticks();
    int n = poll(w, 1, 200);
    int waited = ms_since(began);
    ok("an empty pipe is not ready", n == 0);
    ok("and asking waits for as long as it was told", waited >= 150);
    say_ms("waited ", waited);

    /* --- with something in it -------------------------------------------- */
    fwrite(ends[1], "x", 1);
    w[0].revents = 0;
    began = ticks();
    n = poll(w, 1, 2000);
    waited = ms_since(began);
    ok("a pipe with a byte in it is ready", n == 1 && (w[0].revents & POLLIN));
    ok("and that is answered at once", waited < 60);
    say_ms("answered in ", waited);

    /* Reading it empties the pipe again. */
    char got = 0;
    fread(ends[0], &got, 1);
    ok("and the byte is the one that was written", got == 'x');

    /* --- a file is always ready ------------------------------------------ */
    //
    // The way it is everywhere: a read that reaches the end returns
    // nothing, and nothing is an answer rather than a wait.
    int fd = open("/bin/hello", O_READ);
    if (fd >= 0) {
        w[0].fd = fd; w[0].events = POLLIN | POLLOUT; w[0].revents = 0;
        ok("a file is always ready", poll(w, 1, 0) == 1 &&
                                     (w[0].revents & POLLIN));
        close(fd);
    } else {
        ok("a file is always ready", 0);
    }

    /* --- something that is not a descriptor ------------------------------ */
    w[0].fd = 999; w[0].events = POLLIN; w[0].revents = 0;
    ok("a descriptor that is not one says so",
       poll(w, 1, 0) == 1 && (w[0].revents & POLLNVAL));

    /* --- returning early ------------------------------------------------- */
    //
    // The whole point. A child writes after a moment; the parent asks to
    // wait far longer than that and must come back when the write happens
    // rather than when the time runs out.
    int kid = fork();
    if (kid == 0) {
        close(ends[0]);
        sleep_ms(250);
        fwrite(ends[1], "y", 1);
        exit(0);
    }
    close(ends[1]);            /* so the only writer is the child */

    w[0].fd = ends[0]; w[0].events = POLLIN; w[0].revents = 0;
    began = ticks();
    n = poll(w, 1, 5000);
    waited = ms_since(began);
    ok("a wait ends when something happens", n == 1 &&
                                             (w[0].revents & POLLIN));
    ok("and not when the time runs out", waited >= 150 && waited < 2000);
    say_ms("came back after ", waited);

    fread(ends[0], &got, 1);
    wait_for(kid);

    /* --- and when the other end has gone --------------------------------- */
    //
    // The child has exited, so nothing can write to this pipe again. A read
    // end with no writers is at end of file, and has to say so: a program
    // waiting on a pipe nobody will ever write to again would otherwise
    // wait for ever.
    w[0].revents = 0;
    began = ticks();
    n = poll(w, 1, 2000);
    waited = ms_since(began);
    ok("a pipe whose writer has gone is ready rather than waiting",
       n == 1 && (w[0].revents & POLLHUP));
    ok("and says so at once", waited < 200);

    /* And a read on it really does return nothing, which is what the
       program that was woken will do next. */
    ok("and reading it is the end of the file",
       fread(ends[0], &got, 1) == 0);

    /* --- more than it will take ------------------------------------------ */
    pollfd_t many[24];
    for (int i = 0; i < 24; i++) { many[i].fd = 0; many[i].events = POLLOUT; many[i].revents = 0; }
    ok("more descriptors than it holds is refused, not trimmed",
       poll(many, 24, 0) == -1);

    close(ends[0]);
    puts(fails ? "POLLTEST_FAIL\n" : "POLLTEST_PASS\n");
    return fails ? 1 : 0;
}
