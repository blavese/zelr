/* echo — says what it was given.
 *
 * The smallest program there is, and the one that makes a pipeline testable:
 * it writes to descriptor 1 and nothing else, so wherever its output ends up
 * is entirely somebody else's doing.
 */
#include "zelr.h"

int main(int argc, char **argv) {
    /* All of them, separated by one space, which is what echo means
       everywhere. It used to be the single string a program was started
       on, so `echo  a   b` came out with its spacing intact; it comes out
       with one space between words now, because words are what arrive. */
    for (int i = 1; i < argc; i++) {
        if (i > 1) putc(' ');
        puts(argv[i]);
    }
    putc('\n');
    return 0;
}
