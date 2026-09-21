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
 * So this writes the file alternately full of A and full of B, forever, and
 * tools/crashcheck.py kills the machine at a moment it does not choose.
 * Whatever survives has to be one of those two, whole. A file that is half
 * A and half B is the promise broken; a file that is missing is worse.
 *
 *   crashwrite          rewrite it, over and over, until the power goes
 *   crashwrite A        write it once, full of A, and stop
 *   crashwrite check    say what is on the disk
 */
#include "zelr.h"

#define PATH "/home/crash.dat"
#define SIZE (64 * 1024)

/* Big enough to span many clusters, so the window in which the power can go
   mid-write is a real one rather than a coincidence. */
static char buf[SIZE];

static int write_all(char fill) {
    for (int i = 0; i < SIZE; i++) buf[i] = fill;

    int fd = open(PATH, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) return 0;

    int done = 0;
    while (done < SIZE) {
        int n = fwrite(fd, buf + done, SIZE - done);
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

static int check(void) {
    int fd = open(PATH, O_READ);
    if (fd < 0) {
        /* The one outcome the promise rules out. */
        puts("CRASH_MISSING\n");
        return 1;
    }

    int n = 0;
    while (n < SIZE) {
        int got = fread(fd, buf + n, SIZE - n);
        if (got <= 0) break;
        n += got;
    }
    close(fd);

    if (n != SIZE) {
        puts("CRASH_SHORT ");
        putn(n);
        putc('\n');
        return 1;
    }

    char first = buf[0];
    for (int i = 1; i < SIZE; i++) {
        if (buf[i] == first) continue;
        puts("CRASH_TORN at ");
        putn(i);
        puts(", ");
        putc(first);
        puts(" then ");
        putc(buf[i]);
        putc('\n');
        return 1;
    }

    if (first != 'A' && first != 'B') {
        puts("CRASH_WRONG ");
        putc(first);
        putc('\n');
        return 1;
    }

    puts("CRASH_WHOLE ");
    putc(first);
    putc('\n');
    return 0;
}

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "";

    if (what[0] == 'c') return check();

    if (what[0] == 'A' || what[0] == 'B') {
        if (!write_all(what[0])) { puts("CRASH_NOWRITE\n"); return 1; }
        puts("CRASH_WROTE ");
        putc(what[0]);
        putc('\n');
        return 0;
    }

    /* Round and round until somebody stops the machine. The marker after
       each one is how the check outside knows writing has really started
       before it starts counting down to the kill. */
    for (;;) {
        if (!write_all('B')) { puts("CRASH_NOWRITE\n"); return 1; }
        puts("CRASH_TURN\n");
        if (!write_all('A')) { puts("CRASH_NOWRITE\n"); return 1; }
        puts("CRASH_TURN\n");
    }
}
