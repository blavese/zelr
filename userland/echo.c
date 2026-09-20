/* echo — says what it was given.
 *
 * The smallest program there is, and the one that makes a pipeline testable:
 * it writes to descriptor 1 and nothing else, so wherever its output ends up
 * is entirely somebody else's doing.
 */
#include "zelr.h"

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

int main(void) {
    char arg[256];
    arg[0] = 0;
    getarg(arg, sizeof(arg));
    puts(arg);
    putc('\n');
    return 0;
}
