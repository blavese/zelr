/* spin — a program that does not stop on its own.
 *
 * Every other program in /bin finishes. This one is here so there is
 * something to interrupt: until signals existed, starting this was a way to
 * lose the machine, which is the whole reason they now do.
 *
 * It says it has started and then says nothing, because a program printing
 * while somebody is trying to type ctrl-C at it is a program whose output
 * is the thing being tested rather than the interruption.
 */
#include "zelr.h"

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

int main(void) {
    puts("spinning\n");

    /* Deliberately without sleeping. A program that sleeps gives the
       processor up and could be interrupted at the point it asks for it;
       one that does not is the harder case, and the one that used to be
       impossible to stop. */
    volatile unsigned long n = 0;
    for (;;) n++;

    return 0;
}
