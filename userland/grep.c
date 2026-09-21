/* grep — the lines with something in them.
 *
 * Plain text, not a pattern language. There is no regular expression engine
 * in this system and pretending otherwise by supporting a `*` that means
 * something slightly different from what anyone expects would be worse than
 * saying plainly: this looks for the letters you typed.
 */
#include "zelr.h"

#define LINE 1024

static char line[LINE];
static char chunk[2048];

static int contains(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (needle[k] && hay[i + k] == needle[k]) k++;
        if (!needle[k]) return 1;
    }
    return 0;
}

/* A line at a time out of a descriptor that knows nothing about lines. The
   leftovers between reads are why this holds a buffer of its own: a line can
   straddle any number of them. */
static int scan(int fd, const char *needle) {
    int len = 0;
    int found = 0;

    for (;;) {
        int n = fread(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            char ch = chunk[i];
            if (ch == '\n' || len == LINE - 1) {
                line[len] = 0;
                if (contains(line, needle)) {
                    puts(line);
                    putc('\n');
                    found++;
                }
                len = 0;
            } else {
                line[len++] = ch;
            }
        }
    }

    /* A last line with no newline after it is still a line. */
    if (len) {
        line[len] = 0;
        if (contains(line, needle)) { puts(line); putc('\n'); found++; }
    }
    return found;
}

int main(int ac, char **av) {
    /* The operands: everything after the program's own name, which is what
       the body below has always meant by argc and argv. */
    char **argv = av + 1;
    int argc = ac - 1;

    if (argc == 0) {
        puts("usage: grep WHAT [FILE ...]\n");
        return 2;
    }

    const char *needle = argv[0];
    int found = 0;

    if (argc == 1) {
        found = scan(0, needle);
    } else {
        for (int i = 1; i < argc; i++) {
            int fd = open(argv[i], O_READ);
            if (fd < 0) {
                puts("grep: cannot read ");
                puts(argv[i]);
                putc('\n');
                continue;
            }
            found += scan(fd, needle);
            close(fd);
        }
    }

    /* Nothing found is not an error, but it is worth saying apart from
       something found, the way every other grep does. */
    return found ? 0 : 1;
}
