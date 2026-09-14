/* Proves a program can start another program, wait for it, and learn what it
   returned. Until spawn and wait existed only the kernel could do any of
   that, which is why anything that composed had to live inside it. */
#include "zelr.h"

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

int main(void) {
    puts("spawntest: starting hello.elf from ring 3\n");

    int pid = spawn("/bin/hello");
    if (pid < 0) { puts("spawntest: spawn failed\n"); return 1; }
    puts("spawntest: pid "); putn(pid); putc('\n');

    int status = wait_for(pid);
    puts("spawntest: it exited with "); putn(status); putc('\n');
    if (status != 0) { puts("spawntest: wrong status\n"); return 1; }

    /* count.elf runs for a while, so waiting for it has to actually block
       rather than return early. */
    int t0 = ticks();
    int pid2 = spawn("/bin/count");
    if (pid2 < 0) { puts("spawntest: second spawn failed\n"); return 1; }
    int st2 = wait_for(pid2);
    int elapsed = ticks() - t0;
    puts("spawntest: waited "); putn(elapsed); puts(" ticks for a slow one\n");
    if (elapsed < 5) { puts("spawntest: wait did not block\n"); return 1; }
    (void)st2;

    /* Waiting for something that was never there. */
    if (wait_for(9999) != -1) { puts("spawntest: waited on a ghost\n"); return 1; }

    /* The task list, which needed a system call of its own. */
    zelr_task t;
    int n = 0;
    for (int i = 0; tasks(i, &t) == 1; i++) n++;
    puts("spawntest: "); putn(n); puts(" tasks visible from ring 3\n");
    if (n < 1) { puts("spawntest: no tasks\n"); return 1; }

    puts("spawntest: ok\n");
    return 0;
}
