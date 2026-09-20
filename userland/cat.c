/* cat — copies what it is given to its output.
 *
 * With names, it copies those files. With nothing, it copies its own input,
 * which is the half that matters: `cat` with no arguments is what turns a
 * pipe into something a person can see, and a program that only knew how to
 * open files could not do it.
 */
#include "zelr.h"
#include "args.h"

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

static char buf[4096];

/* Everything from one descriptor to another, until there is no more. A
   short read is not the end — only a zero is. */
static int drain(int from) {
    for (;;) {
        int n = fread(from, buf, sizeof(buf));
        if (n < 0) return -1;
        if (n == 0) return 0;
        int done = 0;
        while (done < n) {
            int w = fwrite(1, buf + done, n - done);
            if (w <= 0) return -1;
            done += w;
        }
    }
}

int main(void) {
    char raw[256];
    char *argv[16];
    int argc = args_of(raw, sizeof(raw), argv, 16);

    if (argc == 0) return drain(0) < 0 ? 1 : 0;

    int bad = 0;
    for (int i = 0; i < argc; i++) {
        int fd = open(argv[i], O_READ);
        if (fd < 0) {
            puts("cat: cannot read ");
            puts(argv[i]);
            putc('\n');
            bad = 1;
            continue;
        }
        if (drain(fd) < 0) bad = 1;
        close(fd);
    }
    return bad;
}
