/* A window manager.
 *
 * Windows are off-screen surfaces; the manager owns the chrome around them,
 * the stacking order and the pointer. Everything is composited into the
 * framebuffer's back buffer and pushed once per frame, so a window moving
 * over another never leaves a trail.
 *
 * Compositing writes rows with memcpy rather than going through fb_put per
 * pixel. At 1024x768 that is the difference between a desktop that drags
 * smoothly and one that does not. The chrome is the exception: rounded
 * corners and shadows read the framebuffer back, so they are drawn per pixel
 * and only around the edges of a window rather than across it.
 *
 * Nothing here picks its own colours. They all come from theme.c, which
 * reads a file a ring 3 program writes, which is how the settings window
 * changes the look of a desktop it cannot otherwise reach. */
#include "wm.h"
#include "fb.h"
#include "gfx.h"
#include "font.h"
#include "theme.h"
#include "rtc.h"
#include "mouse.h"
#include "keyboard.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "vfs.h"
#include "user.h"
#include "elf.h"
#include "apps.h"
#include "winsrv.h"
#include "io.h"

#define TASKBAR_H  34
#define MENU_W     210
#define MENU_ITEM  30
#define SHADOW     5

static window_t *stack[WM_MAX_WINDOWS];   /* index 0 is the bottom */
static int  nwin;
static bool running;

static window_t *dragging;
static int drag_off_x, drag_off_y;
static window_t *mouse_capture;           /* gets moves until the button lifts */

/* Resizing by the bottom right corner. The offset is from the corner rather
   than the origin, so the window does not jump when the grab is not exactly
   on the pixel the corner is at. */
static window_t *resizing;
static int resize_off_x, resize_off_y;
static int resize_cw, resize_ch;      /* the size the outline is showing */

/* Where a window would land if the drag ended now. Shown as an outline
   while the button is still down, which is the only way somebody can tell
   what is about to happen before it happens. */
typedef enum { SNAP_NONE = 0, SNAP_LEFT, SNAP_RIGHT, SNAP_FULL } snap_t;
static snap_t snap_preview;

/* Dragging a window quickly back and forth clears everything else out of
   the way. The positions are kept so the gesture can be recognised, which
   needs a few of them rather than just the last. */
#define SHAKE_SAMPLES 8
static int  shake_x[SHAKE_SAMPLES];
static u64  shake_t[SHAKE_SAMPLES];
static int  shake_n;

static u8  last_buttons;
static int last_mx, last_my;
static bool needs_composite = true;

/* The launcher. Open when someone clicks the desktop or the taskbar badge. */
static bool menu_open;
static int  menu_x, menu_y;
static int  menu_hover = -1;

static u64 last_theme_check;

/* What the launcher offers. A null program means the kernel handles it. */
static const struct {
    const char *label;
    const char *program;
} MENU[] = {
    { "Terminal",     "/bin/term" },
    { "Files",        "/bin/files" },
    { "Notes",        "/bin/notes" },
    { "Paint",        "/bin/paint" },
    { "Settings",     "/bin/settings" },
    { "System info",  0 },
    { "Close all",    0 },
    { "Leave desktop", 0 },
};

#define MENU_N ((int)(sizeof(MENU) / sizeof(MENU[0])))

bool wm_active(void) { return running; }

int wm_outer_w(const window_t *w) { return w->cw + WM_BORDER * 2; }
int wm_outer_h(const window_t *w) { return w->ch + WM_TITLE_H + WM_BORDER; }

void wm_invalidate(window_t *w) { if (w) w->dirty = true; needs_composite = true; }

window_t *wm_create(const char *title, int x, int y, int cw, int ch) {
    if (nwin >= WM_MAX_WINDOWS) return 0;

    window_t *w = (window_t *)kcalloc(sizeof(window_t));
    if (!w) return 0;
    w->canvas = (u32 *)kmalloc((u32)(cw * ch) * 4);
    if (!w->canvas) { kfree(w); return 0; }

    w->x = x; w->y = y; w->cw = cw; w->ch = ch;
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->open = true;
    w->dirty = true;
    surf_clear(w->canvas, cw, ch, theme()->surface);

    stack[nwin++] = w;                    /* new windows open on top */
    needs_composite = true;
    return w;
}

void wm_close(window_t *w) {
    if (!w) return;
    if (w->owned_by_user) {
        /* Tell the owner rather than pulling the surface out from under it. */
        wm_event_t ev = { WM_EV_CLOSE, 0, 0, 0, 0 };
        wm_push_event(w, &ev);
    }
    if (w->on_close) w->on_close(w);
    for (int i = 0; i < nwin; i++) {
        if (stack[i] != w) continue;
        for (int j = i; j < nwin - 1; j++) stack[j] = stack[j + 1];
        nwin--;
        break;
    }
    if (dragging == w) dragging = 0;
    if (mouse_capture == w) mouse_capture = 0;
    /* A surface the window server handed out is not ours to release: it was
       carved page aligned out of a larger allocation, so this pointer is not
       one kmalloc returned, and the server frees the real one when the owning
       program drops its handle. */
    if (w->canvas && !w->owned_by_user) kfree(w->canvas);
    kfree(w);
    needs_composite = true;
}

void wm_raise(window_t *w) {
    if (nwin == 0 || stack[nwin - 1] == w) return;
    for (int i = 0; i < nwin; i++) {
        if (stack[i] != w) continue;
        for (int j = i; j < nwin - 1; j++) stack[j] = stack[j + 1];
        stack[nwin - 1] = w;
        needs_composite = true;
        return;
    }
}

/* --- events for programs outside the kernel ----------------------------- */

void wm_push_event(window_t *w, const wm_event_t *ev) {
    if (!w) return;
    u32 next = (w->q_head + 1) % WM_EVENT_QUEUE;
    if (next == w->q_tail) return;          /* full: drop this one, keep the backlog */
    w->queue[w->q_head] = *ev;
    w->q_head = next;
}

bool wm_pop_event(window_t *w, wm_event_t *out) {
    if (!w || w->q_head == w->q_tail) return false;
    *out = w->queue[w->q_tail];
    w->q_tail = (w->q_tail + 1) % WM_EVENT_QUEUE;
    return true;
}

/* --- compositing -------------------------------------------------------- */

static void blit_surface(const u32 *px, int sw, int sh, int dx, int dy) {
    int sx = 0, sy = 0;
    int w = sw, h = sh;
    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > (int)fb_width())  w = (int)fb_width() - dx;
    if (dy + h > (int)fb_height()) h = (int)fb_height() - dy;
    if (w <= 0 || h <= 0) return;

    u8 *dst = fb_pixels();
    u32 pitch = fb_pitch();
    for (int j = 0; j < h; j++) {
        const u32 *srow = px + (sy + j) * sw + sx;
        u8 *drow = dst + (u32)(dy + j) * pitch + (u32)dx * 4;
        memcpy(drow, srow, (u32)w * 4);
    }
}

/* Lighter and darker versions of a colour, for edges and hovers. */
static u32 lighten(u32 c, int amount) { return gfx_mix(c, RGB(0xFF, 0xFF, 0xFF), amount); }
static u32 darken(u32 c, int amount)  { return gfx_mix(c, 0, amount); }

static void draw_wallpaper(void) {
    const theme_t *t = theme();
    int h = (int)fb_height() - TASKBAR_H;

    switch (t->wallpaper) {
    case WALLPAPER_GRADIENT:
        /* Lit from the top left, which is where a desktop usually is. */
        fb_vgradient(0, 0, (int)fb_width(), h, lighten(t->desktop, 22), t->desktop);
        break;

    case WALLPAPER_GRID: {
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);
        u32 linec = lighten(t->desktop, 14);
        for (int y = 0; y < h; y += 32) fb_rect(0, (u32)y, fb_width(), 1, linec);
        for (u32 x = 0; x < fb_width(); x += 32) fb_rect(x, 0, 1, (u32)h, linec);
        break;
    }

    case WALLPAPER_DOTS: {
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);
        u32 dot = lighten(t->desktop, 26);
        for (int y = 16; y < h; y += 28)
            for (u32 x = 16; x < fb_width(); x += 28) {
                fb_put(x, (u32)y, dot);
                fb_put(x + 1, (u32)y, dot);
                fb_put(x, (u32)y + 1, dot);
                fb_put(x + 1, (u32)y + 1, dot);
            }
        break;
    }

    case WALLPAPER_STARS: {
        /* The field is not stored anywhere. Each star's place comes from
           multiplying its number by a large odd constant and keeping the
           middle bits, which scatters them well enough to look unplanned
           and costs nothing to keep. Drift comes from the clock, so the
           whole thing is a function of the time and the star's number. */
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);
        u32 drift = (u32)(timer_ticks() / 3);

        for (u32 i = 0; i < 240; i++) {
            u32 hx = i * 2654435761u;
            u32 hy = i * 2246822519u;

            u32 speed = 1 + (i % 3);            /* nearer stars move faster */
            u32 x = ((hx >> 9) + drift * speed) % fb_width();
            u32 y = (hy >> 9) % (u32)h;

            u32 c = lighten(t->desktop, 25 + (int)(i % 3) * 45);
            fb_put(x, y, c);
            if (i % 9 == 0) {                   /* a few brighter ones */
                fb_put(x + 1, y, c);
                fb_put(x, y + 1, c);
                fb_put(x + 1, y + 1, c);
            }
        }
        break;
    }

    case WALLPAPER_WAVES: {
        /* Bands whose height follows a quarter of a sine, mirrored out to a
           full period. A table is smaller than the code to compute one and
           this is the only place anything here needs a curve. */
        static const u8 QUARTER[17] = {
            0, 24, 49, 73, 97, 120, 142, 163, 181, 198, 212, 224,
            233, 240, 245, 247, 248
        };
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);

        for (u32 x = 0; x < fb_width(); x++) {
            u32 phase = (x / 3) % 64;
            int v = (phase < 16) ? QUARTER[phase]
                  : (phase < 32) ? QUARTER[32 - phase]
                  : (phase < 48) ? -QUARTER[phase - 32]
                                 : -QUARTER[64 - phase];

            for (int band = 0; band < 5; band++) {
                int y = h * (band + 1) / 6 + v / 8;
                if (y < 0 || y >= h) continue;
                fb_rect(x, (u32)y, 1, 2, lighten(t->desktop, 10 + band * 4));
            }
        }
        break;
    }

    case WALLPAPER_WEAVE: {
        /* Diagonals both ways. Two passes rather than one so the crossings
           come out brighter, which is what makes it read as woven. */
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);
        u32 line = lighten(t->desktop, 12);
        int span = (int)fb_width() + h;

        for (int d = 0; d < span; d += 24)
            for (int k = 0; k < h; k++) {
                int x = d - k;
                if (x >= 0 && x < (int)fb_width()) fb_put((u32)x, (u32)k, line);
            }
        for (int d = -h; d < (int)fb_width(); d += 24)
            for (int k = 0; k < h; k++) {
                int x = d + k;
                if (x >= 0 && x < (int)fb_width()) fb_put((u32)x, (u32)k, line);
            }
        break;
    }

    default:
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);
        break;
    }
}

/* The three buttons on a title bar, right to left: close, maximise,
   minimise. A window nobody said can be resized has no maximise button,
   because pressing it would do nothing. */
#define BTN_SIZE 14
#define BTN_STEP 20

typedef enum { BTN_NONE = 0, BTN_CLOSE, BTN_MAX, BTN_MIN } button_t;

static int button_box(const window_t *w, button_t which, int *bx, int *by) {
    int slot = (which == BTN_CLOSE) ? 0 : (which == BTN_MAX ? 1 : 2);
    *bx = w->x + wm_outer_w(w) - 26 - slot * BTN_STEP;
    *by = w->y + (WM_TITLE_H - BTN_SIZE) / 2;
    return BTN_SIZE;
}

static button_t button_at(const window_t *w, int mx, int my) {
    button_t order[3] = { BTN_CLOSE, BTN_MAX, BTN_MIN };
    for (int i = 0; i < 3; i++) {
        if (order[i] == BTN_MAX && !w->resizable) continue;
        int bx, by, bs = button_box(w, order[i], &bx, &by);
        if (mx >= bx && mx < bx + bs && my >= by && my < by + bs) return order[i];
    }
    return BTN_NONE;
}

/* The grip in the bottom right corner. Only there on a window that can
   actually be resized. */
#define GRIP 14

static bool on_grip(const window_t *w, int mx, int my) {
    if (!w->resizable || w->maximized) return false;
    int gx = w->x + wm_outer_w(w) - GRIP;
    int gy = w->y + wm_outer_h(w) - GRIP;
    return mx >= gx && mx < gx + GRIP && my >= gy && my < gy + GRIP;
}

/* Everything above the taskbar, which is where a window is allowed to be. */
static int work_h(void) { return (int)fb_height() - TASKBAR_H; }

static void draw_chrome(window_t *w, bool focused) {
    const theme_t *t = theme();
    int ow = wm_outer_w(w), oh = wm_outer_h(w);
    int r = t->corner;

    if (t->shadows) fb_shadow(w->x, w->y, ow, oh, r, SHADOW);

    /* The body, so the rounded bottom corners have something under them. */
    fb_round_rect(w->x, w->y, ow, oh, r, t->surface);

    /* The title bar is the top of that same rounded shape, which is why it
       is drawn as its own rounded rect and then squared off at the bottom. */
    u32 bar = focused ? t->accent : lighten(t->surface, 10);
    fb_round_rect(w->x, w->y, ow, WM_TITLE_H + r, r, bar);
    fb_rect((u32)w->x, (u32)(w->y + WM_TITLE_H - 1), (u32)ow, 1,
            focused ? darken(t->accent, 40) : darken(t->surface, 20));

    u32 title_fg = focused ? darken(t->accent, 170) : t->text_dim;
    gfx_text(w->x + 12, w->y + (WM_TITLE_H - FONT_H) / 2, w->title, title_fg);

    /* Dots rather than glyphs: at 14 pixels a drawn symbol is mostly noise,
       and the colour and position already say what each one does. The mark
       inside is what tells them apart at a glance. */
    button_t order[3] = { BTN_CLOSE, BTN_MAX, BTN_MIN };
    u32 tint[3] = { RGB(0xE0, 0x6A, 0x5A), RGB(0x5E, 0xC2, 0x7A), RGB(0xE0, 0xB0, 0x4A) };
    for (int i = 0; i < 3; i++) {
        if (order[i] == BTN_MAX && !w->resizable) continue;

        int bx, by, bs = button_box(w, order[i], &bx, &by);
        u32 dot = focused ? tint[i] : darken(t->surface, 30);
        fb_round_rect(bx, by, bs, bs, bs / 2, dot);
        if (!focused) continue;

        u32 mark = darken(dot, 130);
        switch (order[i]) {
        case BTN_CLOSE:                                     /* a bar */
            fb_rect((u32)(bx + 4), (u32)(by + 6), 6, 2, mark);
            break;
        case BTN_MAX:                                       /* a box */
            fb_round_frame(bx + 4, by + 4, 6, 6, 1, mark);
            break;
        default:                                            /* a floor */
            fb_rect((u32)(bx + 4), (u32)(by + 8), 6, 2, mark);
            break;
        }
    }

    fb_round_frame(w->x, w->y, ow, oh, r, darken(t->surface, 55));

    /* Three short strokes in the corner, which is how a grip has looked
       for long enough that nobody needs to be told. */
    if (w->resizable && !w->maximized) {
        u32 grip = darken(t->surface, 70);
        int gx = w->x + ow - 5, gy = w->y + oh - 5;
        for (int i = 0; i < 3; i++) {
            int d = i * 4;
            fb_rect((u32)(gx - d), (u32)(gy - 2), 3, 2, grip);
            fb_rect((u32)(gx - 2), (u32)(gy - d), 2, 3, grip);
        }
    }
}

/* Changes a window's size, whoever owns it.
 *
 * A window the kernel drew into owns its own pixels and they are simply
 * replaced. One a program draws into is the window server's business,
 * because the same memory is mapped into that program's address space and
 * the program has to be told. */
bool wm_resize(window_t *w, int cw, int ch) {
    if (!w || cw < 120 || ch < 60) return false;
    if (w->cw == cw && w->ch == ch) return true;

    if (w->owned_by_user) return winsrv_resize_window(w, cw, ch);

    u32 *fresh = (u32 *)kmalloc((u32)(cw * ch) * 4);
    if (!fresh) return false;
    surf_clear(fresh, cw, ch, theme()->surface);
    kfree(w->canvas);
    w->canvas = fresh;
    w->cw = cw;
    w->ch = ch;
    w->dirty = true;
    needs_composite = true;
    return true;
}

/* Remembers where a window was, so there is something to go back to. Only
   the first of a run of these counts: snapping a maximised window and then
   restoring it should give back where it was before any of it. */
static void remember_place(window_t *w) {
    if (w->maximized) return;
    w->restore_x = w->x;
    w->restore_y = w->y;
    w->restore_cw = w->cw;
    w->restore_ch = w->ch;
}

static void place(window_t *w, int x, int y, int cw, int ch) {
    if (!wm_resize(w, cw, ch)) return;
    w->x = x;
    w->y = y;
    needs_composite = true;
}

/* The rectangle a snap zone corresponds to, in outer coordinates. */
static void snap_rect(snap_t zone, int *x, int *y, int *cw, int *ch) {
    int fw = (int)fb_width(), fh = work_h();
    switch (zone) {
    case SNAP_LEFT:  *x = 0;      *y = 0; *cw = fw / 2 - WM_BORDER * 2; break;
    case SNAP_RIGHT: *x = fw / 2; *y = 0; *cw = fw / 2 - WM_BORDER * 2; break;
    default:         *x = 0;      *y = 0; *cw = fw - WM_BORDER * 2;     break;
    }
    *ch = fh - WM_TITLE_H - WM_BORDER;
}

static void apply_snap(window_t *w, snap_t zone) {
    if (!w->resizable || zone == SNAP_NONE) return;
    remember_place(w);
    int x, y, cw, ch;
    snap_rect(zone, &x, &y, &cw, &ch);
    place(w, x, y, cw, ch);
    w->maximized = (zone == SNAP_FULL);
}

static void toggle_maximize(window_t *w) {
    if (!w->resizable) return;
    if (w->maximized) {
        w->maximized = false;
        place(w, w->restore_x, w->restore_y, w->restore_cw, w->restore_ch);
    } else {
        apply_snap(w, SNAP_FULL);
    }
}

static void set_minimized(window_t *w, bool yes) {
    if (w->minimized == yes) return;
    w->minimized = yes;
    if (!yes) wm_raise(w);
    needs_composite = true;
}

/* The size a corner drag is currently asking for, clamped so a window
   cannot be dragged smaller than something usable or off the work area. */
static void resize_from_pointer(int mx, int my, int *cw_out, int *ch_out) {
    if (!resizing) return;
    int cw = mx + resize_off_x - resizing->x - WM_BORDER * 2;
    int ch = my + resize_off_y - resizing->y - WM_TITLE_H - WM_BORDER;

    if (cw < 160) cw = 160;
    if (ch < 80)  ch = 80;
    if (resizing->x + cw + WM_BORDER * 2 > (int)fb_width())
        cw = (int)fb_width() - resizing->x - WM_BORDER * 2;
    if (resizing->y + ch + WM_TITLE_H + WM_BORDER > work_h())
        ch = work_h() - resizing->y - WM_TITLE_H - WM_BORDER;

    *cw_out = cw;
    *ch_out = ch;
}

/* Which edge of the screen the pointer is close enough to for a snap. */
static snap_t snap_zone_at(int mx, int my) {
    const int EDGE = 12;
    if (my <= EDGE) return SNAP_FULL;
    if (mx <= EDGE) return SNAP_LEFT;
    if (mx >= (int)fb_width() - EDGE) return SNAP_RIGHT;
    return SNAP_NONE;
}

/* Dragging a window quickly back and forth means "get everything else out
   of my way". Recognised as several changes of direction inside a second,
   which a person aiming at something never does by accident. */
static void shake_note(int mx) {
    u64 now = timer_ticks();
    if (shake_n >= SHAKE_SAMPLES) {
        for (int i = 1; i < SHAKE_SAMPLES; i++) {
            shake_x[i - 1] = shake_x[i];
            shake_t[i - 1] = shake_t[i];
        }
        shake_n = SHAKE_SAMPLES - 1;
    }
    shake_x[shake_n] = mx;
    shake_t[shake_n] = now;
    shake_n++;
}

static bool shake_detected(void) {
    if (shake_n < SHAKE_SAMPLES) return false;
    if (shake_t[SHAKE_SAMPLES - 1] - shake_t[0] > timer_hz()) return false;

    int turns = 0;
    int last_dir = 0;
    for (int i = 1; i < SHAKE_SAMPLES; i++) {
        int d = shake_x[i] - shake_x[i - 1];
        if (d > -24 && d < 24) continue;          /* too small to count */
        int dir = d > 0 ? 1 : -1;
        if (last_dir && dir != last_dir) turns++;
        last_dir = dir;
    }
    return turns >= 3;
}

static void shake_reset(void) { shake_n = 0; }

static void draw_menu(void) {
    if (!menu_open) return;
    const theme_t *t = theme();
    int h = MENU_N * MENU_ITEM + 12;

    if (t->shadows) fb_shadow(menu_x, menu_y, MENU_W, h, 8, SHADOW);
    fb_round_rect(menu_x, menu_y, MENU_W, h, 8, lighten(t->surface, 6));
    fb_round_frame(menu_x, menu_y, MENU_W, h, 8, darken(t->surface, 40));

    for (int i = 0; i < MENU_N; i++) {
        int iy = menu_y + 6 + i * MENU_ITEM;
        if (i == menu_hover)
            fb_round_rect(menu_x + 5, iy, MENU_W - 10, MENU_ITEM, 6,
                          gfx_mix(lighten(t->surface, 6), t->accent, 60));

        u32 fg = (i == menu_hover) ? t->text : gfx_mix(t->text, t->text_dim, 120);
        gfx_text(menu_x + 38, iy + (MENU_ITEM - FONT_H) / 2, MENU[i].label, fg);

        /* A rounded square stands in for an icon. Accent for the things that
           launch a program, grey for the ones the desktop handles itself. */
        fb_round_rect(menu_x + 16, iy + MENU_ITEM / 2 - 6, 12, 12, 3,
                      MENU[i].program ? t->accent : darken(t->text_dim, 60));
    }
}

/* The outline of where a dragged window would land. Drawn as a frame
   rather than a filled rectangle so what is underneath stays readable. */
static void draw_snap_preview(void) {
    if (snap_preview == SNAP_NONE) return;
    const theme_t *t = theme();

    int x, y, cw, ch;
    snap_rect(snap_preview, &x, &y, &cw, &ch);
    int ow = cw + WM_BORDER * 2, oh = ch + WM_TITLE_H + WM_BORDER;

    for (int i = 0; i < 3; i++)
        fb_round_frame(x + i, y + i, ow - i * 2, oh - i * 2, t->corner, t->accent);
    fb_rect((u32)(x + 3), (u32)(y + 3), (u32)(ow - 6), WM_TITLE_H - 3,
            gfx_mix(t->desktop, t->accent, 90));
}

/* While a corner is being dragged, the frame stays where it is and this
   shows the size being asked for. The surface itself is swapped once, when
   the button comes up. */
static void draw_resize_preview(void) {
    if (!resizing) return;
    const theme_t *t = theme();
    int ow = resize_cw + WM_BORDER * 2, oh = resize_ch + WM_TITLE_H + WM_BORDER;
    for (int i = 0; i < 2; i++)
        fb_round_frame(resizing->x + i, resizing->y + i,
                       ow - i * 2, oh - i * 2, t->corner, t->accent);
}

static void draw_taskbar(void) {
    const theme_t *t = theme();
    int y = (int)fb_height() - TASKBAR_H;

    fb_rect(0, (u32)y, fb_width(), TASKBAR_H, darken(t->surface, 40));
    fb_rect(0, (u32)y, fb_width(), 1, lighten(t->surface, 12));

    /* The launcher badge, which is also what the desktop menu opens from. */
    bool badge_hot = menu_open;
    fb_round_rect(8, y + 5, 76, TASKBAR_H - 10, 6,
                  badge_hot ? t->accent : lighten(t->surface, 4));
    gfx_text(20, y + (TASKBAR_H - FONT_H) / 2, "nyx",
             badge_hot ? darken(t->accent, 170) : t->accent);

    int x = 96;
    for (int i = 0; i < nwin; i++) {
        window_t *w = stack[i];
        bool focused = (i == nwin - 1) && !w->minimized;
        int tw = gfx_text_width(w->title) + 24;
        if (x + tw > (int)fb_width() - 120) break;

        u32 chip = lighten(t->surface, 4);
        if (focused)        chip = gfx_mix(chip, t->accent, 70);
        else if (w->minimized) chip = darken(t->surface, 20);

        fb_round_rect(x, y + 5, tw, TASKBAR_H - 10, 6, chip);
        gfx_text(x + 12, y + (TASKBAR_H - FONT_H) / 2, w->title,
                 focused ? t->text : t->text_dim);

        /* A full underline for the window in front, a short stub for one
           that is only put away, so the taskbar says where everything is. */
        if (focused)
            fb_rect((u32)(x + 8), (u32)(y + TASKBAR_H - 6), (u32)(tw - 16), 2, t->accent);
        else if (w->minimized)
            fb_rect((u32)(x + tw / 2 - 5), (u32)(y + TASKBAR_H - 6), 10, 2, t->text_dim);

        x += tw + 6;
    }

    /* The time on the right, with uptime under it when there is room. A
       machine with no usable CMOS clock falls back to counting from boot,
       which is what this had before there was a clock to read. */
    char clock[24];
    if (rtc_present()) rtc_format_short(clock, sizeof(clock));
    else {
        u32 secs = (u32)(timer_ticks() / timer_hz());
        kformat(clock, sizeof(clock), "up %d:%02d", secs / 60, secs % 60);
    }
    gfx_text((int)fb_width() - gfx_text_width(clock) - 16,
             y + (TASKBAR_H - FONT_H) / 2, clock, t->text);
}

static const u8 CURSOR[19][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0}, {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0}, {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0}, {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0}, {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0}, {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0}, {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0}, {1,2,2,1,1,2,2,1,0,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0}, {1,1,0,0,0,1,2,2,1,0,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0}, {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
};

static void draw_cursor(int mx, int my) {
    for (int y = 0; y < 19; y++)
        for (int x = 0; x < 12; x++) {
            u8 v = CURSOR[y][x];
            if (!v) continue;
            fb_put((u32)(mx + x), (u32)(my + y),
                   v == 1 ? RGB(0x08, 0x0C, 0x10) : RGB(0xF4, 0xF7, 0xF9));
        }
}

static void composite(void) {
    /* Surfaces replaced since the last frame go back now, before anything
       here takes a pointer into one. */
    winsrv_reap_retired();

    draw_wallpaper();

    for (int i = 0; i < nwin; i++) {
        window_t *w = stack[i];
        w->dirty = false;
        if (w->minimized) continue;         /* still a window, just not here */
        draw_chrome(w, i == nwin - 1);

        /* The surface and its dimensions are taken together. A program
           swapping its own surface changes all three at once, and reading
           them one at a time could pair a new, smaller buffer with the size
           of the old one and run off the end of it. */
        bool were_on = interrupts_enabled();
        cli();
        const u32 *px = w->canvas;
        int cw = w->cw, ch = w->ch;
        if (were_on) sti();

        blit_surface(px, cw, ch, w->x + WM_BORDER, w->y + WM_TITLE_H);
    }

    draw_snap_preview();
    draw_resize_preview();
    draw_taskbar();
    draw_menu();
    draw_cursor(last_mx, last_my);
    fb_flush();
}

/* --- launching ---------------------------------------------------------- */

static void launch(const char *path) {
    u32 size = 0;
    u8 *img = vfs_slurp(path, &size);
    if (!img) return;
    user_spawn_elf(path, img, size);
    kfree(img);
}

static void menu_choose(int i) {
    menu_open = false;
    needs_composite = true;
    if (i < 0 || i >= MENU_N) return;

    if (MENU[i].program) { launch(MENU[i].program); return; }

    if (!strcmp(MENU[i].label, "System info")) app_about();
    else if (!strcmp(MENU[i].label, "Close all")) { while (nwin > 0) wm_close(stack[nwin - 1]); }
    else if (!strcmp(MENU[i].label, "Leave desktop")) running = false;
}

static int menu_item_at(int mx, int my) {
    if (!menu_open) return -1;
    int h = MENU_N * MENU_ITEM + 12;
    if (mx < menu_x || mx >= menu_x + MENU_W) return -1;
    if (my < menu_y + 6 || my >= menu_y + h - 6) return -1;
    int i = (my - menu_y - 6) / MENU_ITEM;
    return (i >= 0 && i < MENU_N) ? i : -1;
}

static void open_menu_at(int x, int y) {
    int h = MENU_N * MENU_ITEM + 12;
    if (x + MENU_W > (int)fb_width()) x = (int)fb_width() - MENU_W - 4;
    if (y + h > (int)fb_height() - TASKBAR_H) y = (int)fb_height() - TASKBAR_H - h - 4;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    menu_x = x; menu_y = y;
    menu_open = true;
    menu_hover = -1;
    needs_composite = true;
}

/* --- input -------------------------------------------------------------- */

static window_t *window_at(int mx, int my, bool *on_title, button_t *button) {
    for (int i = nwin - 1; i >= 0; i--) {
        window_t *w = stack[i];
        if (w->minimized) continue;         /* not on screen, not clickable */

        int ow = wm_outer_w(w), oh = wm_outer_h(w);
        if (mx < w->x || my < w->y || mx >= w->x + ow || my >= w->y + oh) continue;

        *button = button_at(w, mx, my);
        *on_title = (my < w->y + WM_TITLE_H);
        return w;
    }
    *on_title = false;
    *button = BTN_NONE;
    return 0;
}

/* Which taskbar chip is under the pointer, or -1. The widths have to be
   worked out the same way the drawing does, so this walks the same list. */
static int taskbar_chip_at(int mx, int my) {
    int y = (int)fb_height() - TASKBAR_H;
    if (my < y + 5 || my >= y + TASKBAR_H - 5) return -1;

    int x = 96;
    for (int i = 0; i < nwin; i++) {
        int tw = gfx_text_width(stack[i]->title) + 24;
        if (x + tw > (int)fb_width() - 120) break;
        if (mx >= x && mx < x + tw) return i;
        x += tw + 6;
    }
    return -1;
}

static bool on_taskbar_badge(int mx, int my) {
    int y = (int)fb_height() - TASKBAR_H;
    return my >= y + 5 && my < y + TASKBAR_H - 5 && mx >= 8 && mx < 84;
}

static void handle_mouse(int mx, int my, u8 buttons) {
    bool pressed_now = (buttons & 1) && !(last_buttons & 1);
    bool right_now   = (buttons & 2) && !(last_buttons & 2);
    bool released    = !(buttons & 1) && (last_buttons & 1);

    if (menu_open) {
        int over = menu_item_at(mx, my);
        if (over != menu_hover) { menu_hover = over; needs_composite = true; }
        if (pressed_now) {
            if (over >= 0) { menu_choose(over); return; }
            /* A click anywhere else dismisses it, and does nothing more. */
            menu_open = false;
            needs_composite = true;
            return;
        }
    }

    if (released) {
        /* Hand the release to whoever was being drawn in, before dropping
           the capture: a program needs to know a stroke ended. */
        if (mouse_capture && mouse_capture->owned_by_user) {
            window_t *w = mouse_capture;
            wm_event_t ev = { WM_EV_MOUSE,
                              mx - (w->x + WM_BORDER),
                              my - (w->y + WM_TITLE_H), 0, 0 };
            wm_push_event(w, &ev);
        }

        /* Letting go over an edge is what commits a snap. Doing it on the
           press instead would snap a window somebody was only dragging
           past the edge on the way somewhere else. */
        if (dragging && snap_preview != SNAP_NONE) apply_snap(dragging, snap_preview);
        snap_preview = SNAP_NONE;

        /* A resize is one swap, on release. Following the pointer would
           reallocate the surface on every mouse packet, which is both waste
           and a great deal more chances to catch the program mid-draw. */
        if (resizing) wm_resize(resizing, resize_cw, resize_ch);
        shake_reset();

        dragging = 0;
        resizing = 0;
        mouse_capture = 0;
        needs_composite = true;
    }

    if (resizing) {
        resize_from_pointer(mx, my, &resize_cw, &resize_ch);
        needs_composite = true;
        return;
    }

    if (dragging) {
        /* Dragging a maximised window pulls it back to its own size, under
           the pointer, which is the only thing that can sensibly happen. */
        if (dragging->maximized) {
            dragging->maximized = false;
            place(dragging, dragging->restore_x, dragging->restore_y,
                  dragging->restore_cw, dragging->restore_ch);
            drag_off_x = dragging->cw / 2;
            drag_off_y = WM_TITLE_H / 2;
        }

        dragging->x = mx - drag_off_x;
        dragging->y = my - drag_off_y;
        if (dragging->y < 0) dragging->y = 0;
        if (dragging->x < -(dragging->cw - 60)) dragging->x = -(dragging->cw - 60);
        if (dragging->x > (int)fb_width() - 60) dragging->x = (int)fb_width() - 60;
        if (dragging->y > work_h() - WM_TITLE_H)
            dragging->y = work_h() - WM_TITLE_H;

        snap_t zone = dragging->resizable ? snap_zone_at(mx, my) : SNAP_NONE;
        if (zone != snap_preview) { snap_preview = zone; needs_composite = true; }

        shake_note(mx);
        if (theme()->quirks && shake_detected()) {
            for (int i = 0; i < nwin; i++)
                if (stack[i] != dragging) set_minimized(stack[i], true);
            shake_reset();
        }

        needs_composite = true;
        return;
    }

    /* Once a drag starts inside a window's content it keeps receiving
       movement, even if the pointer strays outside. */
    if (mouse_capture) {
        window_t *w = mouse_capture;
        int lx = mx - (w->x + WM_BORDER), ly = my - (w->y + WM_TITLE_H);
        if (w->owned_by_user) {
            wm_event_t ev = { WM_EV_MOUSE, lx, ly, buttons, 0 };
            wm_push_event(w, &ev);
        } else if (w->on_mouse) {
            w->on_mouse(w, lx, ly, buttons, false);
        }
        return;
    }

    if (!pressed_now && !right_now) return;

    bool on_title = false;
    button_t button = BTN_NONE;
    window_t *w = window_at(mx, my, &on_title, &button);

    if (!w) {
        /* Nothing under the pointer: the taskbar, or the desktop itself. */
        if (on_taskbar_badge(mx, my)) {
            if (menu_open) { menu_open = false; needs_composite = true; }
            else open_menu_at(8, (int)fb_height() - TASKBAR_H - (MENU_N * MENU_ITEM + 12) - 6);
            return;
        }

        int chip = taskbar_chip_at(mx, my);
        if (chip >= 0) {
            /* Clicking the window already in front puts it away; clicking
               anything else brings it back and raises it. */
            window_t *c = stack[chip];
            if (c->minimized)                 set_minimized(c, false);
            else if (chip == nwin - 1)        set_minimized(c, true);
            else                              wm_raise(c);
            return;
        }

        if (my < work_h()) open_menu_at(mx, my);
        return;
    }

    wm_raise(w);

    if (button == BTN_CLOSE) { wm_close(w); return; }
    if (button == BTN_MAX)   { toggle_maximize(w); return; }
    if (button == BTN_MIN)   { set_minimized(w, true); return; }

    if (on_grip(w, mx, my)) {
        resizing = w;
        resize_off_x = (w->x + wm_outer_w(w)) - mx;
        resize_off_y = (w->y + wm_outer_h(w)) - my;
        resize_cw = w->cw;
        resize_ch = w->ch;
        return;
    }

    if (on_title) {
        /* A second click on a title bar is the usual way to maximise. */
        if (right_now) { toggle_maximize(w); return; }

        dragging = w;
        drag_off_x = mx - w->x;
        drag_off_y = my - w->y;
        shake_reset();
        return;
    }

    mouse_capture = w;
    {
        int lx = mx - (w->x + WM_BORDER), ly = my - (w->y + WM_TITLE_H);
        if (w->owned_by_user) {
            wm_event_t ev = { WM_EV_MOUSE, lx, ly, buttons | 0x80, 0 };
            wm_push_event(w, &ev);          /* 0x80 marks the initial press */
        } else if (w->on_mouse) {
            w->on_mouse(w, lx, ly, buttons, true);
        }
    }
}

/* --- shortcuts ----------------------------------------------------------

   A chord is not a character, so none of these can arrive through the
   keyboard buffer; the modifier is read directly at the moment the key
   comes out. Anything not claimed here goes on to the focused window. */

/* Brings the window behind the front one forward, so pressing it
   repeatedly walks the stack. */
static void cycle_windows(void) {
    if (nwin < 2) return;

    /* Skip past anything put away: cycling to a window that is not on
       screen looks like nothing happened. */
    for (int i = nwin - 2; i >= 0; i--) {
        if (stack[i]->minimized) continue;
        wm_raise(stack[i]);
        needs_composite = true;
        return;
    }
    /* Everything else is minimised, so bring the nearest one back. */
    set_minimized(stack[nwin - 2], false);
}

static void minimize_all(void) {
    for (int i = 0; i < nwin; i++) set_minimized(stack[i], true);
}

/* True when the key was a shortcut and should not reach a window.
 *
 * The modifier comes off the key rather than out of kbd_alt(), because by
 * the time a key is read the chord that produced it has usually been let go
 * again. */
static bool handle_shortcut(int key) {
    if (!(key & KEY_MOD_ALT)) return false;
    int c = KEY_CODE(key);

    switch (c) {
    case '\t': cycle_windows(); return true;      /* alt+tab */
    case 'd': case 'D':                            /* alt+d, show the desktop */
        minimize_all();
        return true;
    case 'm': case 'M':
        if (nwin) set_minimized(stack[nwin - 1], true);
        return true;
    case 'f': case 'F':
        if (nwin) toggle_maximize(stack[nwin - 1]);
        return true;
    case 'q': case 'Q':
        if (nwin) wm_close(stack[nwin - 1]);
        return true;
    }

    /* Alt with an arrow snaps the front window to that side, which is the
       same thing dragging it there does. */
    if (nwin) {
        window_t *top = stack[nwin - 1];
        if (c == KEY_LEFT)  { apply_snap(top, SNAP_LEFT);  return true; }
        if (c == KEY_RIGHT) { apply_snap(top, SNAP_RIGHT); return true; }
        if (c == KEY_UP)    { apply_snap(top, SNAP_FULL);  return true; }
        if (c == KEY_DOWN) {
            if (top->maximized) toggle_maximize(top);
            else                set_minimized(top, true);
            return true;
        }
    }
    return false;
}

void wm_quit(void) { running = false; }

void wm_run(void) {
    if (!fb_active()) { kprintf("the desktop needs a framebuffer\n"); return; }

    theme_init();

    running = true;
    menu_open = false;
    mouse_set_autodraw(false);          /* the manager draws the pointer */
    last_mx = mouse_x();
    last_my = mouse_y();
    last_buttons = mouse_buttons();
    needs_composite = true;
    last_theme_check = timer_ticks();

    while (running) {
        int mx = mouse_x(), my = mouse_y();
        u8 buttons = mouse_buttons();

        if (mx != last_mx || my != last_my || buttons != last_buttons) {
            last_mx = mx; last_my = my;
            handle_mouse(mx, my, buttons);
            last_buttons = buttons;
            needs_composite = true;
        }

        int c = kbd_trygetchar();
        if (c >= 0 && KEY_CODE(c) == 27) {         /* escape */
            if (menu_open) { menu_open = false; needs_composite = true; }
            else break;
        } else if (c >= 0 && handle_shortcut(c)) {
            /* Claimed by the desktop. */
        } else if (c >= 0 && nwin > 0) {
            /* A window that is put away is not the one being typed at, so
               find the front one that is actually on screen. */
            window_t *top = 0;
            for (int i = nwin - 1; i >= 0 && !top; i--)
                if (!stack[i]->minimized) top = stack[i];

            /* Programs get the key, plus the control bit and nothing else.
               Shift is already folded into the character, so passing it on
               would mean every capital letter arrived as a chord and no
               program's comparison against a letter would match. Alt is the
               desktop's own and a chord using it never reaches here. What is
               left is control, which is what a program needs to tell copy
               from the letter c. */
            u32 out = (u32)KEY_CODE(c) | (u32)(c & KEY_MOD_CTRL);
            if (top && top->owned_by_user) {
                wm_event_t ev = { WM_EV_KEY, 0, 0, 0, out };
                wm_push_event(top, &ev);
            } else if (top && top->on_key) {
                top->on_key(top, (char)KEY_CODE(c));
            }
        }

        /* The settings window writes a file; this is what notices. Four
           times a second is well under what a person can perceive as lag
           and is a 512 byte read. */
        if (timer_ticks() - last_theme_check > timer_hz() / 4) {
            last_theme_check = timer_ticks();
            if (theme_reload()) needs_composite = true;
        }

        for (int i = 0; i < nwin; i++)
            if (stack[i]->dirty) needs_composite = true;

        /* The taskbar clock ticks, so repaint at least once a second even
           when nothing else changed. A wallpaper that moves needs it far
           more often than that, but only while it is the one on. */
        static u64 last_tick;
        u64 every = wallpaper_moves(theme()->wallpaper) ? timer_hz() / 12
                                                        : timer_hz();
        if (!every) every = 1;
        if (timer_ticks() - last_tick >= every) {
            last_tick = timer_ticks();
            needs_composite = true;
        }

        if (needs_composite) {
            composite();
            needs_composite = false;
        }
    }

    while (nwin > 0) wm_close(stack[nwin - 1]);
    running = false;
    menu_open = false;
    dragging = resizing = mouse_capture = 0;
    snap_preview = SNAP_NONE;
    shake_reset();
    mouse_set_autodraw(true);
}
