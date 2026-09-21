/* ps — what is running.
 *
 * The kernel's own shell has had this for a long time; a shell in ring 3 had
 * no way to ask until a program could. One line per task, the name last, so
 * that `ps | grep something` reads the way it does anywhere else.
 */
#include "zelr.h"

/* The same five the kernel uses, in the same order. */
static const char *STATE[] = { "ready", "running", "sleeping", "blocked", "dead" };

static void pad(int n, int width) {
    char digits[16];
    int len = 0;
    if (n == 0) digits[len++] = '0';
    while (n > 0) { digits[len++] = (char)('0' + n % 10); n /= 10; }
    for (int i = len; i < width; i++) putc(' ');
    while (len > 0) putc(digits[--len]);
}

static void column(const char *s, int width) {
    int n = 0;
    for (; s[n]; n++) putc(s[n]);
    for (; n < width; n++) putc(' ');
}

int main(void) {
    column("PID", 6);
    column("STATE", 10);
    column("SLICES", 9);
    puts("NAME\n");

    zelr_task t;
    for (int i = 0; tasks(i, &t) == 1; i++) {
        pad((int)t.pid, 4);
        puts("  ");
        column(t.state < 5 ? STATE[t.state] : "?", 10);
        pad((int)t.slices, 7);
        puts("  ");
        puts(t.name);
        putc('\n');
    }
    return 0;
}
