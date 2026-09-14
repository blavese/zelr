/* The one window the kernel still draws itself.
 *
 * about reports on the machine it is running on, which means reaching into
 * the allocator, the scheduler and the clock. Everything else that appears
 * on the desktop is a ring 3 program talking to the window server; this is
 * here because a program in user space has no way to ask these questions,
 * and giving it one would be a bigger hole than the window is worth. */
#include "apps.h"
#include "wm.h"
#include "gfx.h"
#include "font.h"
#include "fb.h"
#include "pmm.h"
#include "heap.h"
#include "timer.h"
#include "sched.h"
#include "fs.h"
#include "string.h"
#include "printf.h"

/* --- about -------------------------------------------------------------- */

static void about_render(window_t *w) {
    surf_clear(w->canvas, w->cw, w->ch, RGB(0x16, 0x1B, 0x21));
    surf_rect(w->canvas, w->cw, w->ch, 0, 0, w->cw, 34, RGB(0x1E, 0x25, 0x2D));
    surf_text(w->canvas, w->cw, w->ch, 14, 9, "zelr " KERNEL_VERSION, RGB(0x4F, 0xD6, 0xA0));

    char line[80];
    int y = 48;
    const int step = 20;

    surf_text(w->canvas, w->cw, w->ch, 14, y, "an operating system from scratch",
              RGB(0xC4, 0xCE, 0xD6)); y += step + 6;

    u32 total = pmm_total_frames() * 4;
    u32 used  = pmm_used_frames() * 4;

    kformat(line, sizeof(line), "memory    %d of %d KiB used", used, total);
    surf_text(w->canvas, w->cw, w->ch, 14, y, line, RGB(0x93, 0xA1, 0xAC)); y += step;

    kformat(line, sizeof(line), "heap      %d of %d KiB", heap_used() / 1024, heap_total() / 1024);
    surf_text(w->canvas, w->cw, w->ch, 14, y, line, RGB(0x93, 0xA1, 0xAC)); y += step;

    kformat(line, sizeof(line), "display   %d x %d, 32 bpp", (u32)fb_width(), (u32)fb_height());
    surf_text(w->canvas, w->cw, w->ch, 14, y, line, RGB(0x93, 0xA1, 0xAC)); y += step;

    kformat(line, sizeof(line), "uptime    %d seconds", (u32)(timer_ticks() / timer_hz()));
    surf_text(w->canvas, w->cw, w->ch, 14, y, line, RGB(0x93, 0xA1, 0xAC)); y += step;

    kformat(line, sizeof(line), "tasks     %d", task_count());
    surf_text(w->canvas, w->cw, w->ch, 14, y, line, RGB(0x93, 0xA1, 0xAC)); y += step;

    kformat(line, sizeof(line), "files     %d", fs_count());
    surf_text(w->canvas, w->cw, w->ch, 14, y, line, RGB(0x93, 0xA1, 0xAC)); y += step + 8;

    surf_text(w->canvas, w->cw, w->ch, 14, y, "drag a title bar to move a window",
              RGB(0x6E, 0x7C, 0x88)); y += step;
    surf_text(w->canvas, w->cw, w->ch, 14, y, "click one to bring it to the front",
              RGB(0x6E, 0x7C, 0x88));
}

static window_t *about_win;

static void about_on_mouse(window_t *w, int x, int y, u8 buttons, bool just_pressed) {
    (void)x; (void)y; (void)buttons;
    if (just_pressed) { about_render(w); wm_invalidate(w); }   /* refresh the figures */
}

void app_about(void) {
    if (about_win) { wm_raise(about_win); return; }
    window_t *w = wm_create("about", 682, 120, 322, 300);
    if (!w) return;
    about_render(w);
    w->on_mouse = about_on_mouse;
    w->on_close = app_about_forget;
    about_win = w;
    wm_invalidate(w);
}

void app_about_forget(window_t *w) { if (about_win == w) about_win = 0; }
