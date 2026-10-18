/* durtest — saying "now" about a file, and giving one a different name.
 *
 * Two calls a ported program expects and this system did not have.
 *
 * fsync, because a file is held in memory until its last descriptor closes.
 * That is fine for a program that writes something and stops and no use at
 * all to one that keeps a file open -- a log, a database, anything long
 * running -- for which everything written since it opened is in memory and
 * on the disk there is nothing. The checks below open a file, write to it,
 * and read it back through a *different* descriptor, which is the only way
 * from in here to ask what is really on the disk rather than what this
 * program remembers writing.
 *
 * rename, because that is how a program replaces a file without a moment
 * where it is half written: write a temporary, then swing the name. What
 * this filesystem can and cannot do about that is in kernel/fat.c, and the
 * checks here are about the refusals as well as the successes -- a rename
 * that quietly did the wrong thing would pass a check that only looked at
 * the cases that work.
 */
#include "zelr.h"

static int fails = 0;

static void ok(const char *what, int cond) {
    puts(cond ? "  ok   " : "  FAIL ");
    puts(what);
    putc('\n');
    if (!cond) fails++;
}

#define DIR  "/home"
#define A    DIR "/dur-a.txt"
#define B    DIR "/dur-b.txt"
#define TMP  DIR "/dur-t.txt"
#define DEEP "/cfg/dur-a.txt"
#define LONG DIR "/a-name-far-too-long-for-eight-three.text"
#define SUB  DIR "/dur-d"

static char buf[256];

/* What is on the disk under this name, read fresh. Returns the length, or
   -1 when there is nothing there. */
static int on_disk(const char *path) {
    int fd = open(path, O_READ);
    if (fd < 0) return -1;
    int n = fread(fd, buf, (int)sizeof(buf) - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

/* /sys/time's local time, "local     2026-10-18 14:05:33", as a day number
   and seconds into it; 0 when there is none. */
static int local_now(int *day, int *secs) {
    char t[512];
    int n = slurp("/sys/time", t, (int)sizeof(t) - 1);
    if (n <= 0) return 0;
    t[n] = 0;
    for (int i = 0; i + 30 < n; i++) {
        if (strncmp(t + i, "local", 5) || (i && t[i - 1] != '\n')) continue;
        const char *p = t + i + 5;
        while (*p == ' ') p++;
        int v[6] = { 0 }, at = 0;
        for (int k = 0; k < 6; k++) {
            while (p[at] >= '0' && p[at] <= '9') v[k] = v[k] * 10 + (p[at++] - '0');
            at++;
        }
        *day = v[0] * 372 + v[1] * 31 + v[2];
        *secs = v[3] * 3600 + v[4] * 60 + v[5];
        return 1;
    }
    return 0;
}

static int says(const char *path, const char *want) {
    if (on_disk(path) < 0) return 0;
    return strcmp(buf, want) == 0;
}

int main(void) {
    puts("durtest\n");

    unlink(A);
    unlink(B);
    unlink(TMP);

    /* --- what is on the disk before anything asks ----------------------- */
    int fd = open(A, O_WRITE | O_CREATE | O_TRUNC);
    ok("a file can be opened for writing", fd >= 0);
    if (fd < 0) { puts("DURTEST_FAIL\n"); return 1; }

    fwrite(fd, "first", 5);

    /* Still open, and nothing has asked for it to be written. */
    ok("what is written is not on the disk until somebody says so",
       on_disk(A) <= 0);

    /* --- and after -------------------------------------------------------- */
    ok("fsync says so", fsync(fd) == 0);
    ok("and then it is on the disk", says(A, "first"));

    /* Written again through the same open descriptor, so this is a second
       sync on a file that already exists rather than a first one. */
    seek(fd, 0, 0);
    fwrite(fd, "second", 6);
    ok("a second sync writes the new contents", fsync(fd) == 0 &&
                                                says(A, "second"));
    close(fd);
    ok("and closing after a sync leaves them alone", says(A, "second"));

    /* --- a different name -------------------------------------------------- */
    ok("a file can be renamed", rename(A, B) == 0);
    ok("and is there under the new one", says(B, "second"));
    ok("and gone from the old one", on_disk(A) < 0);

    /* --- replacing --------------------------------------------------------- */
    //
    // The reason rename is worth having: write a temporary and swing the
    // name, so nobody ever sees the file half written.
    fd = open(TMP, O_WRITE | O_CREATE | O_TRUNC);
    fwrite(fd, "replaced", 8);
    close(fd);
    ok("a rename over an existing file replaces it", rename(TMP, B) == 0);
    ok("and the new contents are what is there", says(B, "replaced"));
    ok("and the temporary is gone", on_disk(TMP) < 0);

    /* --- another directory, a long name ------------------------------------- */
    //
    // Both need a second directory entry, and between writing it and
    // deleting the first, two names share one chain of clusters: deleting
    // either then would corrupt the other. The new entry carries a mark
    // until the old one is gone, and the sweep a volume made here gets when
    // it is mounted finishes a rename the power cut short (kernel/fat.c), so
    // on such a volume both are done. They were refused before there was a
    // mark.
    ok("a file can be moved into another directory", rename(B, DEEP) == 0);
    ok("and is there, and gone from where it was", says(DEEP, "replaced") && on_disk(B) < 0);
    ok("and renamed to a name that does not fit eight and three", rename(DEEP, LONG) == 0);
    ok("and is there under it, and gone from where it was", says(LONG, "replaced") && on_disk(DEEP) < 0);
    ok("and back again", rename(LONG, B) == 0 && says(B, "replaced") && on_disk(LONG) < 0);

    /* --- and what it will not do -------------------------------------------- */
    mkdir(SUB);
    ok("a directory is not moved inside itself", rename(SUB, SUB "/inner") != 0);
    rmdir(SUB);

    ok("renaming something that is not there is refused",
       rename(DIR "/dur-nothing.txt", DIR "/dur-x.txt") != 0);

    /* Renaming a file to its own name is nothing to do rather than an
       error, and must not lose the file. */
    ok("renaming a file to its own name is allowed", rename(B, B) == 0);
    ok("and leaves it alone", says(B, "replaced"));

    /* --- when ------------------------------------------------------------- */
    //
    // Every entry was stamped the first of January 2025 whatever the clock
    // said. A file written now says now, as /sys/time's local time has it
    // (to the two seconds the disk keeps), was made then too, and keeps both
    // through a rename; the files the kernel makes up keep none.
    int day0 = 0, secs0 = 0;
    int clock = local_now(&day0, &secs0);
    fd = open(TMP, O_WRITE | O_CREATE | O_TRUNC);
    fwrite(fd, "when", 4);
    close(fd);
    zelr_times t, t2;
    memset(&t, 0, sizeof(t));
    int got = file_times(TMP, &t) == 0;
    int day = t.year * 372 + t.month * 31 + t.day, secs = t.hour * 3600 + t.minute * 60 + t.second;
    ok("a file written now is stamped now, in local time",
       got && (!clock || (day == day0 && secs - secs0 >= -2 && secs - secs0 <= 6)));
    ok("and was made at the same moment",
       got && t.made_year == t.year && t.made_month == t.month && t.made_day == t.day
       && t.made_hour == t.hour && t.made_minute == t.minute && t.made_second == t.second);
    ok("and a rename keeps both",
       rename(TMP, A) == 0 && file_times(A, &t2) == 0 && t2.year == t.year && t2.month == t.month
       && t2.day == t.day && t2.hour == t.hour && t2.minute == t.minute && t2.second == t.second
       && t2.made_second == t.made_second);
    unlink(A);
    ok("the files the kernel makes up keep no time", file_times("/sys/version", &t) != 0);

    unlink(B);
    puts(fails ? "DURTEST_FAIL\n" : "DURTEST_PASS\n");
    return fails ? 1 : 0;
}
