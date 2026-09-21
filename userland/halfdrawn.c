/* A program that spends most of its time half drawn, on purpose.
 *
 * The window server used to hand the compositor the same pixels the program
 * was writing. win_commit marked the window dirty and swapped nothing, so
 * there was no moment at which a frame became finished, and a program whose
 * frame begins by painting over everything genuinely contained a background
 * and nothing else for as long as that took. The compositor is a task like
 * any other and it ran in that gap.
 *
 * That is impossible to check from a screenshot of a real program, because
 * the gap is short and whether a sample lands in it is luck. So this makes
 * the gap enormous and the two states impossible to confuse: it paints the
 * whole window red, waits a fifth of a second without committing, paints it
 * green and commits.
 *
 * Red is therefore a colour this program never publishes. A desktop that
 * ever shows red is a desktop reading a frame that was not finished, and
 * one that shows only green is a desktop reading a frame that was.
 *
 * The green alternates between two shades so that a frozen window and a
 * working one can be told apart: a picture that never changes would pass a
 * check that only looked for the absence of red.
 */
#include "zelr.h"

#define HALF_RED   0xC81E1E
#define HALF_GREEN 0x1E9632
#define HALF_MINT  0x1EC864

int main(void) {
    int win = win_create("Half drawn", 360, 260);
    if (win < 0) exit(1);

    int shade = 0;
    for (;;) {
        int w = win_width(win), h = win_height(win);
        u32 *px = win_surface(win);
        if (!px || w <= 0 || h <= 0) break;

        win_event ev;
        int closing = 0;
        while (win_poll(win, &ev)) if (ev.type == WIN_EV_CLOSE) closing = 1;
        if (closing) break;

        /* The half drawn state. Not committed, so nothing should ever see
           it -- which is the whole claim. */
        for (int i = 0; i < w * h; i++) px[i] = HALF_RED;
        sleep_ms(200);

        u32 done = shade ? HALF_MINT : HALF_GREEN;
        shade = !shade;
        for (int i = 0; i < w * h; i++) px[i] = done;

        win_commit(win);
        sleep_ms(200);
    }

    win_close(win);
    exit(0);
}
