/* The smallest zelr program, and the one this SDK is checked with.
 *
 * It uses nothing from this repository except zelr.h and zelr.ld, and
 * tools/sdkcheck.py builds it in a directory outside the repository with
 * only those two files copied in -- because an SDK that works when the rest
 * of the tree is next to it has not been shown to work at all.
 *
 *   bash build.sh hello.c
 *
 * puts hello.elf beside it. Copy that onto a zelr disk and type its name.
 */
#include "zelr.h"

int main(int argc, char **argv) {
    puts("hello from outside the tree.\n");

    puts("  pid     ");
    putn(getpid());
    putc('\n');

    puts("  argv    ");
    for (int i = 0; i < argc; i++) {
        if (i) putc(' ');
        putc('[');
        puts(argv[i]);
        putc(']');
    }
    putc('\n');

    /* Nothing here is linked in: this loop is the whole of the arithmetic
       and puts is four instructions around int 0x80. */
    int sum = 0;
    for (int i = 1; i <= 100; i++) sum += i;
    puts("  1..100  ");
    putn(sum);
    putc('\n');

    return 0;
}
