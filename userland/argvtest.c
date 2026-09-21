/* argvtest — what a program is told it was started on.
 *
 * A program's arguments are not something it asks for. They are on its stack
 * when it begins, in the order and the shape System V says, and a program
 * built by a toolchain that has never heard of this kernel finds them there
 * because that is where its startup code already looks.
 *
 * So what is worth checking is not "did a string arrive" but the shape:
 *
 *   argv[0] is the program's own name, even when nobody gave it one
 *   words stay apart, and a word with a space in it stays together
 *   the vector ends in a null, which is what makes counting it possible
 *   argc and that null agree
 *   the whole lot survives fork and exec, and spawn, which build it twice
 *     in two different places
 *
 * It runs itself as its own child, which is the only way to check the two
 * calls that hand a vector over: it is the program on both ends of them.
 *
 *   /bin/argvtest              the parent, which starts the others
 *   /bin/argvtest child A B    the half that reports what it was given
 *
 * Which half this is arrives in the vector, so a machine that loses the
 * vector runs the parent twice -- and that parent starts two more, and so
 * on, which is a fork bomb rather than a failed test. It was one, the first
 * time this was run against a kernel broken on purpose. So the parent leaves
 * a note before it starts anything, and a program that finds the note and no
 * vector knows exactly what it is and says so.
 */
#include "zelr.h"

#define SELF "/bin/argvtest"
#define MARK "/home/argvtest.run"

static int fails = 0;

static void ok(const char *what, int cond) {
    puts(cond ? "  ok   " : "  FAIL ");
    puts(what);
    putc('\n');
    if (!cond) fails++;
}

static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* Whether a path ends in a name. argv[0] is a path when a shell found the
   program by looking, and a bare name when somebody typed one. */
static int named(const char *path, const char *leaf) {
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    return streq(base, leaf);
}

static int exists(const char *path) {
    zelr_stat st;
    return stat(path, &st) == 0;
}

/* The words the two halves agree on. The middle one has a space in it on
   purpose: a vector that was ever glued into one string and taken apart
   again arrives here as two words, and the count says so. */
static char *const CHILD_ARGV[] = {
    (char *)SELF, (char *)"child", (char *)"two words", (char *)"four", 0
};

static int be_the_child(int argc, char **argv) {
    ok("a vector survives the call that carried it", argc == 4);
    if (argc != 4) return 1;
    ok("argv[0] is still the program", named(argv[0], "argvtest"));
    ok("a word with a space in it is one word", streq(argv[2], "two words"));
    ok("and the word after it is the word after it", streq(argv[3], "four"));
    ok("and the vector still ends in a null", argv[4] == 0);
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && streq(argv[1], "child")) return be_the_child(argc, argv);

    /* No "child" in the vector, and a note on the disk: this is a child
       whose arguments did not arrive. Saying so and stopping is the whole
       point of the note. */
    if (exists(MARK)) {
        puts("  FAIL a child was started on a vector and got none of it\n");
        puts("ARGVTEST_FAIL\n");
        return 1;
    }

    puts("argvtest\n");

    /* --- what a program gets when nobody said anything ------------------ */
    ok("a program started on nothing still has a name", argc >= 1 && argv[0][0]);
    ok("and that name is its own", named(argv[0], "argvtest"));
    ok("and the vector ends in a null", argv[argc] == 0);

    /* argc is a count and the null is a sentinel, and a program may use
       either, so they have to agree. Counting to the null is what argv_len
       does and what every startup file written elsewhere does. */
    ok("and argc counts up to it", argv_len(argv) == argc);

    int note = open(MARK, O_WRITE | O_CREATE | O_TRUNC);
    if (note < 0) {
        puts("  FAIL could not leave the note that makes this safe to run\n");
        puts("ARGVTEST_FAIL\n");
        return 1;
    }
    fwrite(note, "x", 1);
    close(note);

    /* --- across exec ---------------------------------------------------- */
    int kid = fork();
    if (kid == 0) {
        execv(SELF, CHILD_ARGV);
        puts("  FAIL exec did not replace this program\n");
        exit(70);
    }
    ok("a forked child can exec onto a vector", kid > 0 && wait_for(kid) == 0);

    /* --- and across spawn, which builds the stack somewhere else -------- */
    int other = spawnv(SELF, CHILD_ARGV);
    ok("and spawn hands over the same one", other > 0 && wait_for(other) == 0);

    /* --- more words than the kernel will take --------------------------- */
    /*
     * The block is built inside one page of the new program's stack, so
     * there is a limit, and what matters is that reaching it is a refusal
     * rather than a program started on something truncated. A truncated
     * vector is the failure nobody sees: the program runs, and does its
     * work to the wrong file.
     */
    static char big[3000];
    for (int i = 0; i < (int)sizeof(big) - 1; i++) big[i] = 'x';
    big[sizeof(big) - 1] = 0;
    char *const too_much[] = { (char *)SELF, (char *)"child", big, 0 };
    ok("more words than fit is refused, not trimmed",
       spawnv(SELF, too_much) < 0);

    unlink(MARK);
    puts(fails ? "ARGVTEST_FAIL\n" : "ARGVTEST_PASS\n");
    return fails ? 1 : 0;
}
