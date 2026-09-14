/* Proves the window server actually crosses the ring boundary.
 *
 * Everything this program reports it learned from ring 3, through int 0x80.
 * If the surface were not really mapped into this address space, the write
 * below would fault instead of reading back. */
#include "zelr.h"

static void hex(u64 v) {
    const char *d = "0123456789abcdef";
    puts("0x");
    /* All sixteen digits: printing eight of them is what made a truncated
       pointer look like a correct one. */
    for (int i = 60; i >= 0; i -= 4) putc(d[(v >> i) & 0xF]);
}

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

int main(void) {
    int win = win_create("wintest", 64, 48);
    if (win < 0) { puts("wintest: create failed\n"); return 1; }
    puts("wintest: handle "); putn(win); putc('\n');

    int w = win_width(win), h = win_height(win);
    puts("wintest: size "); putn(w); putc('x'); putn(h); putc('\n');
    if (w != 64 || h != 48) { puts("wintest: wrong size\n"); return 1; }

    u32 *px = win_surface(win);
    if (!px) { puts("wintest: no surface\n"); return 1; }
    puts("wintest: surface at "); hex((u64)px); putc('\n');

    /* Write the whole surface and read it back. A partial mapping would
       fault somewhere in the middle of this. */
    for (int i = 0; i < w * h; i++) px[i] = (u32)(i * 7 + 1);
    int bad = 0;
    for (int i = 0; i < w * h; i++) if (px[i] != (u32)(i * 7 + 1)) bad++;
    if (bad) { puts("wintest: readback failed\n"); return 1; }
    puts("wintest: wrote and read back "); putn(w * h); puts(" pixels\n");

    if (win_commit(win) != 0) { puts("wintest: commit failed\n"); return 1; }
    puts("wintest: committed\n");

    if (win_close(win) != 0) { puts("wintest: close failed\n"); return 1; }
    if (win_surface(win) != 0) { puts("wintest: handle outlived the window\n"); return 1; }
    puts("wintest: closed, handle is dead\n");

    /* A handle this program never owned must not resolve. */
    if (win_width(5) != -1) { puts("wintest: reached a foreign window\n"); return 1; }
    puts("wintest: ok\n");
    return 0;
}
