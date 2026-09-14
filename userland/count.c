#include "zelr.h"

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

/* Sleeps between lines, so the shell stays usable while this runs and the
   preemption is visible rather than asserted. */
int main(void) {
    puts("counting in the background, pid ");
    putn(getpid());
    putc('\n');

    for (int i = 1; i <= 5; i++) {
        puts("  tick ");
        putn(i);
        puts(" at ");
        putn(ticks());
        puts(" ticks\n");
        sleep_ms(700);
    }
    puts("done\n");
    return 0;
}
