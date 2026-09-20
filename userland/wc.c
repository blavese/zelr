/* wc — how many lines, words and bytes went past.
 *
 * The program a pipeline is usually tested with, because it says a number
 * and a number is easy to be sure about. With no names it counts its own
 * input, which is the only way `ls | wc` can mean anything.
 */
#include "zelr.h"
#include "args.h"

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

static char buf[4096];

typedef struct { int lines, words, bytes; } count_t;

static void count(int fd, count_t *c) {
    int in_word = 0;
    for (;;) {
        int n = fread(fd, buf, sizeof(buf));
        if (n <= 0) return;
        for (int i = 0; i < n; i++) {
            char ch = buf[i];
            c->bytes++;
            if (ch == '\n') c->lines++;
            if (ch == ' ' || ch == '\t' || ch == '\n') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                c->words++;
            }
        }
    }
}

static void report(const count_t *c, const char *name) {
    putn(c->lines); puts(" ");
    putn(c->words); puts(" ");
    putn(c->bytes);
    if (name) { puts(" "); puts(name); }
    putc('\n');
}

int main(void) {
    char raw[256];
    char *argv[16];
    int argc = args_of(raw, sizeof(raw), argv, 16);

    if (argc == 0) {
        count_t c = { 0, 0, 0 };
        count(0, &c);
        report(&c, 0);
        return 0;
    }

    count_t total = { 0, 0, 0 };
    int bad = 0;
    for (int i = 0; i < argc; i++) {
        int fd = open(argv[i], O_READ);
        if (fd < 0) {
            puts("wc: cannot read ");
            puts(argv[i]);
            putc('\n');
            bad = 1;
            continue;
        }
        count_t c = { 0, 0, 0 };
        count(fd, &c);
        close(fd);
        report(&c, argv[i]);
        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
    }
    if (argc > 1) report(&total, "total");
    return bad;
}
