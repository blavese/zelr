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
 * checks here are as much about the refusals as the successes -- a rename
 * that quietly did the unsafe thing would pass a check that only looked at
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

    /* --- and what it will not do -------------------------------------------- */
    //
    // Both of these need a second directory entry, and this filesystem has
    // nowhere to record that two entries are one rename in progress. A
    // power cut between them would leave two names for one chain of
    // clusters, and deleting either would then corrupt the other. Refusing
    // is the honest answer and these check that it really refuses rather
    // than doing it and hoping.
    ok("renaming into another directory is refused",
       rename(B, DEEP) != 0);
    ok("and the file is still where it was", says(B, "replaced"));

    ok("renaming to a name that does not fit is refused",
       rename(B, DIR "/a-name-far-too-long-for-eight-three.text") != 0);
    ok("and the file is still where it was after that too",
       says(B, "replaced"));

    ok("renaming something that is not there is refused",
       rename(DIR "/dur-nothing.txt", DIR "/dur-x.txt") != 0);

    /* Renaming a file to its own name is nothing to do rather than an
       error, and must not lose the file. */
    ok("renaming a file to its own name is allowed", rename(B, B) == 0);
    ok("and leaves it alone", says(B, "replaced"));

    unlink(B);
    puts(fails ? "DURTEST_FAIL\n" : "DURTEST_PASS\n");
    return fails ? 1 : 0;
}
