/* hangtest — a program waiting on a server that never answers, and a program
 * trying to end the kernel's own tasks.
 *
 *   hangtest HOST SILENT_PORT LIVE_PORT
 *
 * SILENT_PORT takes a connection and then says nothing, which is what a hung
 * web server looks like from here. LIVE_PORT is an ordinary web server.
 *
 * The first half is about waiting. A network call used to spin inside the
 * kernel with interrupts off, and the clock it was timing itself by only
 * moves on an interrupt, so a peer that never answered stopped the machine
 * for good. So what is checked is that while one program waits:
 *
 *   another program still runs, and the clock still moves
 *   the waiting one can be stopped, in the middle of its call
 *   and afterwards the network still works and every socket came back
 *
 * On a machine with the old kernel this program never gets past the first of
 * those: the child's wait takes the processor and never gives it back, and
 * the harness that started it sees nothing more, ever.
 *
 * The second half is about who may end what. A program may end another
 * program. It may not end or signal the kernel's own tasks, which used to be
 * reachable by pid like anything else.
 */
#include "zelr.h"

static int fails = 0;

static void ok(const char *what, int cond) {
    puts(cond ? "  ok   " : "  FAIL ");
    puts(what);
    putc('\n');
    if (!cond) fails++;
}

static int number(const char *s) {
    int v = 0;
    if (!s || !*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

static int same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* A page off the live server, and whether it looked like one. */
static int fetch_live(const char *host, int port) {
    int s = connect(host, port);
    if (s < 0) return 0;
    const char req[] = "GET / HTTP/1.0\r\nHost: hangtest\r\n\r\n";
    int good = send(s, req, (int)sizeof(req) - 1) > 0;
    char buf[256];
    int got = 0;
    for (int tries = 0; good && tries < 8 && got < 5; tries++) {
        int n = recv(s, buf + got, (int)sizeof(buf) - 1 - got);
        if (n == NET_EOF) break;
        if (n > 0) got += n;
    }
    disconnect(s);
    return got >= 5 && buf[0] == 'H' && buf[1] == 'T' && buf[2] == 'T' && buf[3] == 'P';
}

int main(int argc, char **argv) {
    puts("hangtest\n");
    const char *host = argc > 1 ? argv[1] : "";
    int silent = argc > 2 ? number(argv[2]) : -1;
    int live   = argc > 3 ? number(argv[3]) : -1;
    if (!host[0] || silent <= 0 || live <= 0) {
        puts("usage: hangtest HOST SILENT_PORT LIVE_PORT\nHANGTEST_FAIL\n");
        return 2;
    }

    /* --- waiting on a server that never answers -------------------------- */
    int kid = fork();
    if (kid == 0) {
        /* Connects, which the silent server lets happen, and then reads for
           ever. Each read gives up after a few seconds with nothing and the
           loop asks again, so this child is inside a network call nearly all
           of the time it exists. */
        int s = connect(host, silent);
        if (s < 0) exit(3);
        char buf[64];
        for (;;) {
            int n = recv(s, buf, sizeof(buf));
            if (n == NET_EOF || n < -2) exit(4);
        }
    }
    ok("a child was started to wait on the silent server", kid > 0);

    int t0 = ticks();
    sleep_ms(1500);
    int t1 = ticks();
    ok("another program runs while one waits on a server that never answers", 1);
    ok("and the clock moves meanwhile", t1 - t0 >= 100);

    zelr_task t;
    int waiting = 0;
    for (int i = 0; tasks(i, &t) == 1; i++)
        if ((int)t.pid == kid && t.state != TASK_DEAD) waiting = 1;
    ok("the waiting child is still there, still waiting", waiting);

    ok("a program waiting on the network can be stopped", kill(kid) == 0);
    wait_for(kid);
    int gone = 1;
    for (int i = 0; tasks(i, &t) == 1; i++)
        if ((int)t.pid == kid && t.state != TASK_DEAD) gone = 0;
    ok("and it goes", gone);

    /* --- and the network afterwards --------------------------------------- */
    //
    // The child was stopped asleep in the middle of a read, holding a socket
    // and a connection underneath it. Neither would come back if ending it
    // forgot them, and six of each is all there is.
    ok("the network still works after a program was stopped mid-call",
       fetch_live(host, live));

    int socks[6];
    int opened = 0;
    for (int i = 0; i < 6; i++) {
        socks[i] = connect(host, live);
        if (socks[i] >= 0) opened++;
    }
    ok("every socket the stopped program held came back", opened == 6);
    if (opened != 6) { puts("      opened "); putn(opened); putc('\n'); }
    for (int i = 0; i < 6; i++) if (socks[i] >= 0) disconnect(socks[i]);

    /* --- the kernel's own tasks ------------------------------------------- */
    //
    // Found by asking, never by guessing a number: a pid that happens to be a
    // program on one boot is the network task on another.
    int kernel_seen = 0, refused = 0, survived = 0, unsignalled = 0;
    u32 targets[4];
    int nt = 0;
    for (int i = 0; tasks(i, &t) == 1 && nt < 4; i++) {
        if (t.user || t.state == TASK_DEAD) continue;
        if (same(t.name, "net") || same(t.name, "idle") || same(t.name, "usb"))
            targets[nt++] = t.pid;
    }
    for (int i = 0; i < nt; i++) {
        kernel_seen++;
        if (send_signal((int)targets[i], SIGTERM) != 0) unsignalled++;
        if (kill((int)targets[i]) != 0) refused++;
    }
    sleep_ms(50);
    for (int i = 0; i < nt; i++)
        for (int j = 0; tasks(j, &t) == 1; j++)
            if (t.pid == targets[i] && t.state != TASK_DEAD) survived++;

    ok("the kernel's own tasks can be found", kernel_seen >= 2);
    ok("and a program cannot signal them", unsignalled == kernel_seen);
    ok("nor end them", refused == kernel_seen);
    ok("and they are all still running", survived == kernel_seen);

    puts(fails ? "HANGTEST_FAIL\n" : "HANGTEST_PASS\n");
    return fails ? 1 : 0;
}
