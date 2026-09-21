/* ls — what is in a directory, one name to a line.
 *
 * One to a line on purpose. A listing in columns is nicer to look at and
 * useless to everything downstream of it; a line each is what lets `ls | wc`
 * count files rather than count columns.
 */
#include "zelr.h"

static int list(const char *path) {
    zelr_stat st;
    int shown = 0;

    for (int i = 0; ; i++) {
        int rc = readdir(path, i, &st);
        if (rc < 0) {
            puts("ls: cannot read ");
            puts(path);
            putc('\n');
            return 1;
        }
        if (rc == 0) break;

        puts(st.name);
        /* A slash, so a listing says which of them can be gone into without
           anybody having to ask a second time. */
        if (st.is_dir) putc('/');
        putc('\n');
        shown++;
    }
    (void)shown;
    return 0;
}

int main(int ac, char **av) {
    /* The operands: everything after the program's own name, which is what
       the body below has always meant by argc and argv. */
    char **argv = av + 1;
    int argc = ac - 1;

    if (argc == 0) {
        char cwd[128];
        if (getcwd(cwd, sizeof(cwd)) < 0) strcpy(cwd, "/");
        return list(cwd);
    }

    int bad = 0;
    for (int i = 0; i < argc; i++) {
        /* A name each when there is more than one, because a listing of
           three directories run together says nothing about which is which. */
        if (argc > 1) { puts(argv[i]); puts(":\n"); }
        if (list(argv[i])) bad = 1;
        if (argc > 1 && i != argc - 1) putc('\n');
    }
    return bad;
}
