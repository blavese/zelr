/* crashwrite — a file rewritten over and over, so the machine can be
 * switched off in the middle of it.
 *
 * kernel/fat.c makes a specific promise about what happens if the power
 * goes while a file is being written:
 *
 *   the new copy goes into clusters nothing points at yet
 *   the data is flushed to the drive before anything points at it
 *   one sector write swings the directory entry from the old chain to the
 *     new one, and that is the commit
 *   the old chain is only released afterwards
 *
 *   "before it the old file is live, after it the new one is, and there is
 *    no moment where neither is"
 *
 * That is a strong claim and nothing tested it. A comment asserting crash
 * safety is worth nothing: the failure it describes happens once, on
 * somebody's real disk, months later.
 *
 * So this rewrites the file forever, each copy full of its own generation
 * number, one more than the last, and tools/crashcheck.py kills the machine
 * at a moment it does not choose. Whatever survives has to be one copy,
 * whole, and no older than the last one this said it had finished. A file
 * that is half one generation and half another is the promise broken; a file
 * that is missing is worse; and one older than a write that was reported
 * done is a disk that was never written.
 *
 * It was A and B in turn, and the check outside wanted to see both across six
 * power cuts to know the writes were landing. Once a write took milliseconds
 * that was a coin tossed six times, and it came up the same all six about
 * once in thirty two runs.
 *
 *   crashwrite          rewrite it, over and over, until the power goes
 *   crashwrite start    write generation 0 once, and stop
 *   crashwrite check    say what is on the disk
 */
#include "zelr.h"

#define PATH "/home/crash.dat"
#define SIZE (64 * 1024)

/* Big enough to span many clusters, so the window in which the power can go
   mid-write is a real one rather than a coincidence. */
static unsigned buf[SIZE / 4];

static int write_all(unsigned gen) {
    for (int i = 0; i < SIZE / 4; i++) buf[i] = gen;

    int fd = open(PATH, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) return 0;

    int done = 0;
    while (done < SIZE) {
        int n = fwrite(fd, (char *)buf + done, SIZE - done);
        if (n <= 0) { close(fd); return 0; }
        done += n;
    }

    /* The close is what puts it on the disk: a file is held in memory until
       the last descriptor goes. So the whole of the interesting window --
       the data, the table, the directory entry, the flushes between them --
       is inside this call. */
    /* close returns 0 for success, the way every call like it does. */
    return close(fd) == 0;
}

/* What is on the disk: 0 and the generation when it is one whole copy, or 1
   with the reason already said when it is not. */
static int read_back(unsigned *gen, int say) {
    int fd = open(PATH, O_READ);
    if (fd < 0) {
        /* The one outcome the promise rules out. */
        if (say) puts("CRASH_MISSING\n");
        return 1;
    }

    int n = 0;
    while (n < SIZE) {
        int got = fread(fd, (char *)buf + n, SIZE - n);
        if (got <= 0) break;
        n += got;
    }
    close(fd);

    if (n != SIZE) {
        if (say) { puts("CRASH_SHORT "); putn(n); putc('\n'); }
        return 1;
    }

    unsigned first = buf[0];
    for (int i = 1; i < SIZE / 4; i++) {
        if (buf[i] == first) continue;
        if (say) {
            puts("CRASH_TORN at ");
            putn(i * 4);
            puts(", ");
            putn((int)first);
            puts(" then ");
            putn((int)buf[i]);
            putc('\n');
        }
        return 1;
    }
    *gen = first;
    return 0;
}

static int check(void) {
    unsigned gen;
    if (read_back(&gen, 1)) return 1;
    puts("CRASH_WHOLE ");
    putn((int)gen);
    putc('\n');
    return 0;
}

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "";

    if (what[0] == 'c') return check();

    if (what[0] == 's') {
        if (!write_all(0)) { puts("CRASH_NOWRITE\n"); return 1; }
        puts("CRASH_WROTE 0\n");
        return 0;
    }

    /* Round and round until somebody stops the machine, carrying on from
       whatever the last power cut left, so the numbers only ever go up. The
       marker after each one says which has been finished: the check outside
       waits for the first before it starts counting down to the kill, and
       holds what survives to the last it saw. */
    unsigned gen = 0;
    if (read_back(&gen, 0)) gen = 0;
    for (;;) {
        gen++;
        if (!write_all(gen)) { puts("CRASH_NOWRITE\n"); return 1; }
        puts("CRASH_TURN ");
        putn((int)gen);
        putc('\n');
    }
}
