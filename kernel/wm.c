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
#include "pins.h"
#include "winsrv.h"
#include "power.h"
#include "sound.h"
#include "diskfs.h"
#include "fbcon.h"
#include "sched.h"
#include "io.h"

#define TASKBAR_H  34
/* The panel floats clear of the screen edge rather than being welded to it.
   A bar that runs edge to edge is a border of the display; one with air
   around it is an object lying on the desktop, and the desktop is then
   something with depth rather than a backdrop. */
#define TASKBAR_GAP 10
#define TASKBAR_R   10

/* Where the panel sits when it is out. Where it is actually drawn is
   somewhere between that and off the bottom of the screen, because it tucks
   itself away for a window that wants the room; taskbar_y below says where,
   and everything that draws it or asks what is under the pointer goes
   through that one function so the two can never disagree. */
static int panel_rest_y(void) {
    return (int)fb_height() - TASKBAR_H - TASKBAR_GAP;
}

/* The badge, then the pinned apps, then a chip for each remaining window. */
#define TASKBAR_BADGE_W 76
#define PIN_ICON  22
#define PIN_STEP  (PIN_ICON + 8)

static int taskbar_pins_x(void) {
    return TASKBAR_GAP + 8 + TASKBAR_BADGE_W + 12;
}

static int taskbar_chips_x(void) {
    return taskbar_pins_x() + pins_count() * PIN_STEP + 14;
}
/* The volume button, left of the clock, and the little panel it opens. */
#define VOL_W      30
#define VOLPOP_W   208
#define VOLPOP_H   40

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

/* What of the screen has to reach video memory this frame.
 *
 * Drawing is into ordinary memory and costs almost nothing. Copying the
 * result out is ninety two per cent of a frame, measured, because video
 * memory is uncached and every write goes to the device rather than to a
 * cache line. A full screen copy is three megabytes at this size and twenty
 * at a laptop's, so a frame that only moved a menu should not pay for one.
 *
 * Whole is the default and everything that has ever asked for a frame still
 * gets one. Only the animation asks for less, and only because it knows
 * exactly what it touched. */
static bool frame_is_whole = true;

/* The union of everything that changed, when it is not the whole screen.
   Empty when x1 is not past x0. */
static int dmg_x0, dmg_y0, dmg_x1, dmg_y1;

static void damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (dmg_x1 <= dmg_x0) {                 /* first of this frame */
        dmg_x0 = x; dmg_y0 = y; dmg_x1 = x + w; dmg_y1 = y + h;
        return;
    }
    if (x < dmg_x0) dmg_x0 = x;
    if (y < dmg_y0) dmg_y0 = y;
    if (x + w > dmg_x1) dmg_x1 = x + w;
    if (y + h > dmg_y1) dmg_y1 = y + h;
}

/* One rectangle rather than a list. Two windows redrawing in opposite
   corners therefore cost the whole screen, which is the price of not
   keeping a list, and the case that matters is one thing moving. */
static void need_frame(void) {
    needs_composite = true;
    frame_is_whole = true;
}

/* Another frame, but only this much of it has to be sent. */
static void need_frame_in(int x, int y, int w, int h) {
    needs_composite = true;
    damage(x, y, w, h);
}

/* --- things that move ----------------------------------------------------
 *
 * There is one way of doing this and everything uses it: whatever is moving
 * records the tick it started on, and asks here how far along it is. Nothing
 * owns a timer, nothing counts frames, and nothing has to be told to stop.
 * The compositor keeps painting while anything is still in flight and goes
 * back to sleep when nothing is.
 *
 * The clock runs at a hundred ticks a second, so the shortest transition
 * worth having is around ten of them. Faster than that and it arrives in a
 * frame or two, which is a jump with extra work.
 *
 * All of it is integer: this kernel is built with no floating point at all,
 * so the curve below is fixed point where 256 means finished.
 */
#define ANIM_FULL  256
#define MENU_MS    200       /* the launcher arriving */
#define HOVER_MS   110      /* a highlight coming up under the pointer */

static u64 anim_len(u32 ms) {
    u64 n = ((u64)ms * timer_hz()) / 1000u;
    return n ? n : 1;
}

/* Fast to begin with and settling at the end, which is what makes a
   transition read as something arriving rather than a number changing.
   Cubic, so 1 - (1-p)^3. */
static u32 ease_out(u32 p) {
    if (p >= ANIM_FULL) return ANIM_FULL;
    u32 inv = ANIM_FULL - p;
    inv = (inv * inv) / ANIM_FULL;
    inv = (inv * (ANIM_FULL - p)) / ANIM_FULL;
    return ANIM_FULL - inv;
}

/* How far into a transition that began on this tick, 0 to 256.
 *
 * With animation switched off everything is already finished, which is the
 * whole of what the toggle in the settings program does. Before this it was
 * read from the file, written back to it, and never once looked at. */
static u32 phase_of(u64 since, u32 ms) {
    if (!since || !theme()->animate) return ANIM_FULL;
    u64 len = anim_len(ms);
    u64 gone = timer_ticks() - since;
    if (gone >= len) return ANIM_FULL;
    return ease_out((u32)((gone * ANIM_FULL) / len));
}

static bool still_moving(u64 since, u32 ms) {
    return since && phase_of(since, ms) < ANIM_FULL;
}

/* The volume panel, open when someone clicks the speaker. */
static bool volume_open;
static bool volume_drag;
static int  volume_before_mute = 70;

/* The launcher. Open when someone clicks the desktop or the taskbar badge. */
static bool menu_open;
static int  menu_x, menu_y;
static int  menu_hover = -1;
static int  menu_left = -1;         /* the item the pointer has just left */
static u64  menu_since;             /* when it opened */
static u64  menu_hover_since;       /* when the highlight last changed */

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
    { "Monitor",      "/bin/monitor" },
    { "Music",        "/bin/music" },
    { "Calculator",   "/bin/calc" },
    { "System info",  0 },
    { "Close all",    0 },
    { "Leave desktop", 0 },
    { "Shut down",    0 },
};

#define MENU_N ((int)(sizeof(MENU) / sizeof(MENU[0])))

bool wm_active(void) { return running; }

int wm_outer_w(const window_t *w) { return w->cw + WM_BORDER * 2; }
int wm_outer_h(const window_t *w) { return w->ch + WM_TITLE_H + WM_BORDER; }

void wm_invalidate(window_t *w) { if (w) w->dirty = true; need_frame(); }

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
    need_frame();
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
    need_frame();
}

void wm_raise(window_t *w) {
    if (nwin == 0 || stack[nwin - 1] == w) return;
    for (int i = 0; i < nwin; i++) {
        if (stack[i] != w) continue;
        for (int j = i; j < nwin - 1; j++) stack[j] = stack[j + 1];
        stack[nwin - 1] = w;
        need_frame();
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

/* --- the panel that tucks itself away ------------------------------------
 *
 * A bar welded to the bottom of the screen means a maximised window is not
 * maximised: it stops short, and the last thirty pixels of the display are
 * spent on something that is only occasionally looked at. A bar that is
 * always hidden means reaching for it every time, which is worse.
 *
 * So it is neither. Nothing wants the room, it is out, floating clear of
 * the edge with the wallpaper showing around it. Something does want the
 * room, it slides away and the window has the whole screen. Put the pointer
 * at the bottom and it comes back over the window, and stays as long as the
 * pointer is on it. No setting, nothing to turn on: it is a consequence of
 * what is on screen.
 */
#define PANEL_MS   170      /* going away, and coming back */
#define PANEL_EDGE 3        /* how close to the bottom brings it back */

static bool panel_shown = true;
static u64  panel_since;

/* An icon being pressed, and dragged along the panel to reorder it. */
static int  pin_press = -1;
static int  pin_press_x;
static int  pin_at_x;
static bool pin_moved;

/* Where the panel is drawn this frame. */
static int taskbar_y(void) {
    int out = panel_rest_y();
    int away = (int)fb_height() + SHADOW;
    u32 p = phase_of(panel_since, PANEL_MS);
    int from = panel_shown ? away : out;
    int to   = panel_shown ? out  : away;
    return from + ((to - from) * (int)p) / ANIM_FULL;
}

/* True when something on screen wants the room the panel is in. Only a
   maximised window does: everything else leaves the panel where it is. */
static bool panel_in_the_way(void) {
    for (int i = 0; i < nwin; i++)
        if (!stack[i]->minimized && stack[i]->maximized) return true;
    return false;
}

/* Everything above the panel, which is where a window is allowed to be.
   With the panel tucked away, that is the screen. */
static int work_h(void) {
    return panel_in_the_way() ? (int)fb_height() : panel_rest_y();
}

/* The topmost window covering the whole screen, or -1.
 *
 * Nothing below one is drawn and neither is the wallpaper: all of it would
 * be painted over before the frame reached the screen. A maximised terminal
 * with a wallpaper that drifts was repainting the whole desktop twelve
 * times a second for nobody. */
static int covering_index(void) {
    for (int i = nwin - 1; i >= 0; i--) {
        window_t *w = stack[i];
        if (w->minimized || !w->maximized) continue;

        /* Measured rather than taken on trust. */
        if (w->x <= 0 && w->y <= 0
            && w->x + wm_outer_w(w) >= (int)fb_width()
            && w->y + wm_outer_h(w) >= (int)fb_height()) return i;
    }
    return -1;
}

/* A sine, as 64 steps of a period, running from -248 to 248. A table is
   smaller than the code to compute one, and this kernel has no floating
   point in it at all, so everything that curves curves through here. */
static const u8 SINE_Q[17] = {
    0, 24, 49, 73, 97, 120, 142, 163, 181, 198, 212, 224, 233, 240, 245,
    247, 248
};

static int sine64(u32 phase) {
    phase &= 63;
    if (phase < 16) return (int)SINE_Q[phase];
    if (phase < 32) return (int)SINE_Q[32 - phase];
    if (phase < 48) return -(int)SINE_Q[phase - 32];
    return -(int)SINE_Q[64 - phase];
}

/* A circle's outline, from one eighth of it mirrored eight ways. Two rows
   thick, or it disappears at the size these are drawn at. */
static void ring(int cx, int cy, int r, u32 c) {
    if (r <= 0) return;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        const int P[8][2] = {
            { x, y }, { y, x }, { -y, x }, { -x, y },
            { -x, -y }, { -y, -x }, { y, -x }, { x, -y }
        };
        for (int i = 0; i < 8; i++) {
            fb_put((u32)(cx + P[i][0]), (u32)(cy + P[i][1]), c);
            fb_put((u32)(cx + P[i][0]), (u32)(cy + P[i][1] + 1), c);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* Whether the panel should be out, given where the pointer is.
 *
 * Out whenever nothing needs the room. Tucked when something does, unless
 * the pointer is at the very bottom of the screen asking for it, and then
 * out for as long as the pointer is on it: a panel that goes away under the
 * hand reaching for it is worse than one that never moves. */
static bool panel_should_show(int my) {
    if (menu_open || dragging || pin_press >= 0) return true;
    if (!panel_in_the_way()) return true;
    if (panel_shown) return my >= panel_rest_y() - 6;
    return my >= (int)fb_height() - PANEL_EDGE;
}

/* The band the panel moves through. Everything is composited whatever
   changed, so this is about what has to be copied to the card, and the
   panel sliding does not oblige the other nine tenths of the screen to
   make that trip. */
static void panel_frame(void) {
    need_frame_in(0, panel_rest_y() - SHADOW - 38, (int)fb_width(),
                  TASKBAR_H + TASKBAR_GAP + SHADOW * 2 + 42);
}

static void panel_update(int my) {
    bool want = panel_should_show(my);
    if (want == panel_shown) return;
    panel_shown = want;
    panel_since = timer_ticks();
    panel_frame();
}

/* Lighter and darker versions of a colour, for edges and hovers. */
static u32 lighten(u32 c, int amount) { return gfx_mix(c, RGB(0xFF, 0xFF, 0xFF), amount); }
static u32 darken(u32 c, int amount)  { return gfx_mix(c, 0, amount); }

/* A mark on the desktop, whichever ground it is.
 *
 * Every wallpaper was written as the background lightened by a little, which
 * is a mark on a dark desktop and nothing at all on a near-white one. This
 * is the same distance in whichever direction is away from the ground. */
static u32 paper(int amount) {
    const theme_t *t = theme();
    return t->light ? darken(t->desktop, amount) : lighten(t->desktop, amount);
}

static void draw_wallpaper(void) {
    const theme_t *t = theme();
    /* All the way down, because the panel no longer covers the bottom of
       the screen and the wallpaper shows around it. */
    int h = (int)fb_height();

    switch (t->wallpaper) {
    case WALLPAPER_GRADIENT:
        /* Lit from the top left, which is where a desktop usually is. */
        fb_vgradient(0, 0, (int)fb_width(), h, paper(22), t->desktop);
        break;

    case WALLPAPER_GRID: {
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);
        u32 linec = paper(14);
        for (int y = 0; y < h; y += 32) fb_rect(0, (u32)y, fb_width(), 1, linec);
        for (u32 x = 0; x < fb_width(); x += 32) fb_rect(x, 0, 1, (u32)h, linec);
        break;
    }

    case WALLPAPER_DOTS: {
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);
        u32 dot = paper(26);
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

            u32 c = paper(25 + (int)(i % 3) * 45);
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
        /* Five lines whose height follows a sine. They travel: the phase
           carries the clock, so the whole set slides sideways rather than
           being a still picture of something that should be moving.
           A thousand columns of two pixels, five times, and nothing else. */
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);
        u32 drift = (u32)(timer_ticks() / 4);

        for (u32 x = 0; x < fb_width(); x++) {
            for (int band = 0; band < 5; band++) {
                int v = sine64(x / 3 + drift + (u32)band * 5);
                int y = h * (band + 1) / 6 + v / 8;
                if (y < 0 || y >= h) continue;
                fb_rect(x, (u32)y, 1, 2,
                        gfx_mix(t->desktop, t->accent, 22 + band * 12));
            }
        }
        break;
    }

    case WALLPAPER_WEAVE: {
        /* Diagonals both ways. Two passes rather than one so the crossings
           come out brighter, which is what makes it read as woven. */
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);
        u32 line = paper(12);
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

    case WALLPAPER_AURORA: {
        /* Three bands of light leaning across the screen and drifting, each
           soft at its edges. Built as columns: for every x, one short run
           of pixels per band. There is no per pixel work over the screen
           anywhere in it, which is what keeps it affordable. */
        fb_vgradient(0, 0, (int)fb_width(), h, paper(18),
                     t->desktop);
        u32 drift = (u32)(timer_ticks() / 6);
        const int REACH = 17;

        for (int band = 0; band < 3; band++) {
            u32 tint = band == 0 ? t->accent
                     : band == 1 ? lighten(t->accent, 70)
                                 : darken(t->accent, 50);
            int base = h / 3 + band * h / 8;
            u32 slow = 5 + (u32)band * 3;

            for (u32 x = 0; x < fb_width(); x++) {
                int y = base + sine64(x / slow + drift + (u32)band * 11) / 5;
                for (int dy = -REACH; dy <= REACH; dy++) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    int d = dy < 0 ? -dy : dy;
                    int a = (REACH - d) * 40 / REACH;
                    fb_put(x, (u32)yy,
                           gfx_mix(fb_get(x, (u32)yy), tint, a));
                }
            }
        }
        break;
    }

    case WALLPAPER_RAIN: {
        /* Streaks falling at three speeds, each fading out behind its head.
           Where a drop is comes from its number and the clock, so none of
           them is stored anywhere. */
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);
        u32 now = (u32)timer_ticks();

        for (u32 i = 0; i < 170; i++) {
            u32 hx = i * 2654435761u;
            u32 speed = 4 + (i % 5) * 3;
            u32 x = (hx >> 11) % fb_width();
            u32 y = ((hx >> 3) + now * speed) % (u32)(h + 60);
            int len = 5 + (int)(i % 4) * 4;

            for (int k = 0; k < len; k++) {
                int yy = (int)y - k;
                if (yy < 0 || yy >= h) continue;
                fb_put(x, (u32)yy,
                       gfx_mix(t->desktop, t->accent, 75 - k * 65 / len));
            }
        }
        break;
    }

    case WALLPAPER_ORBS: {
        /* Five soft discs wandering on paths that never quite repeat,
           because each one's two sines run at rates with nothing in common.
           Brightness falls off with the square of the distance from the
           middle, which needs no square root and is the reason they have no
           edge to them. */
        fb_vgradient(0, 0, (int)fb_width(), h, paper(12),
                     t->desktop);
        u32 now = (u32)timer_ticks();

        for (int i = 0; i < 5; i++) {
            int r = 44 + i * 9;
            int span_x = (int)fb_width() / 2 - r;
            int span_y = h / 2 - r;
            int cx = (int)fb_width() / 2
                     + sine64(now / (7 + (u32)i * 2) + (u32)i * 9) * span_x / 248;
            int cy = h / 2
                     + sine64(now / (11 + (u32)i * 3) + (u32)i * 21) * span_y / 248;
            u32 tint = i & 1 ? lighten(t->accent, 60) : t->accent;
            int r2 = r * r;

            for (int dy = -r; dy <= r; dy++) {
                int yy = cy + dy;
                if (yy < 0 || yy >= h) continue;
                for (int dx = -r; dx <= r; dx++) {
                    int d2 = dx * dx + dy * dy;
                    if (d2 >= r2) continue;
                    int xx = cx + dx;
                    if (xx < 0 || xx >= (int)fb_width()) continue;
                    int a = (r2 - d2) * 34 / r2;
                    fb_put((u32)xx, (u32)yy,
                           gfx_mix(fb_get((u32)xx, (u32)yy), tint, a));
                }
            }
        }
        break;
    }

    case WALLPAPER_PULSE: {
        /* Rings going out from the middle, fading as they widen, spaced so
           that one leaves as the next arrives. An outline is a few thousand
           points however wide it gets, so this costs the same whatever the
           screen is. */
        fb_rect(0, 0, fb_width(), (u32)h, t->desktop);
        int reach = ((int)fb_width() + h) / 2;
        u32 now = (u32)(timer_ticks() * 3);

        for (int k = 0; k < 7; k++) {
            int r = (int)((now + (u32)(k * reach / 7)) % (u32)reach);
            int fade = 90 - r * 80 / reach;
            if (fade < 6) continue;

            int cx = (int)fb_width() / 2, cy = h / 2;
            ring(cx, cy, r - 3, gfx_mix(t->desktop, t->accent, fade / 3));
            ring(cx, cy, r,     gfx_mix(t->desktop, t->accent, fade));
            ring(cx, cy, r + 3, gfx_mix(t->desktop, t->accent, fade / 2));
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

static void draw_chrome(window_t *w, bool focused) {
    const theme_t *t = theme();
    int ow = wm_outer_w(w), oh = wm_outer_h(w);

    /* Edge to edge, so there is nothing for a rounded corner to show
       through and nothing beside it for a shadow to fall on. Drawing them
       anyway costs a read of the screen back per pixel around the whole
       frame, for four notches of desktop colour in the corners. */
    int r = w->maximized ? 0 : t->corner;

    if (t->shadows && !w->maximized) fb_shadow(w->x, w->y, ow, oh, r, SHADOW);

    /* The body, so the rounded bottom corners have something under them. */
    fb_round_rect(w->x, w->y, ow, oh, r, t->surface);

    /* The title bar is the top of that same rounded shape, which is why it
       is drawn as its own rounded rect and then squared off at the bottom.
     *
     * It is a surface and not a slab of accent. Accent on the largest
     * element of every window spends the one loud colour on the thing that
     * needs it least, leaves nothing louder for what does, and is most of
     * why a desktop reads as old. Focus is said three quieter ways instead:
     * the bar lifts a layer, the title goes to full strength in the heavier
     * weight, and the window's own edge picks up the accent. */
    u32 bar = focused ? t->raised : t->surface;
    fb_round_rect(w->x, w->y, ow, WM_TITLE_H + r, r, bar);

    /* Lit from above: one brighter row along the top, which is what stops a
       flat fill reading as a sticker. */
    fb_rect((u32)(w->x + r), (u32)w->y, (u32)(ow - r * 2), 1, t->sheen);

    /* And the edge between the bar and the body. */
    fb_rect((u32)w->x, (u32)(w->y + WM_TITLE_H - 1), (u32)ow, 1, t->hairline);

    /* The window in front carries its title in the heavier weight. Colour
       alone was doing that job, which leaves nothing for a theme where the
       accent is close to the surface, and weight reads at a glance in a way
       a hue does not. */
    u32 title_fg = focused ? t->text : t->text_dim;
    int title_face = focused ? FACE_BODY_BOLD : FACE_BODY;
    face_text(w->x + 12, w->y + (WM_TITLE_H - face_height(title_face)) / 2,
              w->title, title_fg, title_face);

    /* Dots rather than glyphs: at 14 pixels a drawn symbol is mostly noise,
       and the colour and position already say what each one does. The mark
       inside is what tells them apart at a glance. */
    button_t order[3] = { BTN_CLOSE, BTN_MAX, BTN_MIN };
    u32 tint[3] = { RGB(0xE0, 0x6A, 0x5A), RGB(0x5E, 0xC2, 0x7A), RGB(0xE0, 0xB0, 0x4A) };
    for (int i = 0; i < 3; i++) {
        if (order[i] == BTN_MAX && !w->resizable) continue;

        int bx, by, bs = button_box(w, order[i], &bx, &by);
        u32 dot = focused ? tint[i] : t->hairline;
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

    /* The focused window is the one wearing the accent, one pixel of it.
       This is the whole of the accent's job in a window and it is enough:
       the eye finds a coloured outline in a field of grey immediately. */
    fb_round_frame(w->x, w->y, ow, oh, r,
                   focused ? t->accent : t->hairline);

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
    need_frame();
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
    need_frame();
}

/* The rectangle a snap zone corresponds to, in outer coordinates. */
static void snap_rect(snap_t zone, int *x, int *y, int *cw, int *ch) {
    int fw = (int)fb_width();

    /* A maximised window gets the whole screen, because the panel tucks
       itself away for one. A window snapped to a side does not: the panel
       stays out beside it, so it stops above it. */
    int fh = (zone == SNAP_FULL) ? (int)fb_height() : work_h();
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
    need_frame();
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

/* How strongly item i should be lit: rising for the one under the pointer,
   falling for the one it just left, nothing for the rest. */
static u32 item_light(int i) {
    u32 p = phase_of(menu_hover_since, HOVER_MS);
    if (i == menu_hover) return p;
    if (i == menu_left)  return ANIM_FULL - p;
    return 0;
}

static void draw_menu(void) {
    if (!menu_open) return;
    const theme_t *t = theme();
    int h = MENU_N * MENU_ITEM + 12;

    /* It comes up from below rather than appearing. Eight pixels is enough
       to be read as movement and short enough not to be waited for. */
    u32 p = phase_of(menu_since, MENU_MS);
    int rise = (int)(((ANIM_FULL - p) * 8) / ANIM_FULL);
    int mx = menu_x, my = menu_y + rise;

    if (t->shadows) fb_shadow(mx, my, MENU_W, h, 8, SHADOW);
    fb_round_rect(mx, my, MENU_W, h, 8, t->overlay);
    fb_rect((u32)(mx + 8), (u32)my, (u32)(MENU_W - 16), 1, t->sheen);
    fb_round_frame(mx, my, MENU_W, h, 8, t->hairline);

    for (int i = 0; i < MENU_N; i++) {
        int iy = my + 6 + i * MENU_ITEM;

        u32 lit = item_light(i);
        if (lit) {
            u32 hot = gfx_mix(t->raised, t->accent, 60);
            fb_round_rect(mx + 5, iy, MENU_W - 10, MENU_ITEM, 6,
                          gfx_mix(t->overlay, hot, (int)lit * 255 / ANIM_FULL));
        }

        /* The label comes up out of the panel with it, so the whole thing
           settles at once instead of the text landing first. */
        u32 rest = gfx_mix(t->text, t->text_dim, 120);
        u32 fg = gfx_mix(rest, t->text, (int)lit * 255 / ANIM_FULL);
        fg = gfx_mix(t->overlay, fg, (int)p * 255 / ANIM_FULL);
        face_text(mx + 38, iy + (MENU_ITEM - face_height(FACE_BODY)) / 2,
                  MENU[i].label, fg, FACE_BODY);

        /* A rounded square stands in for an icon. Accent for the things that
           launch a program, grey for the ones the desktop handles itself. */
        u32 pip = MENU[i].program ? t->accent : darken(t->text_dim, 60);
        fb_round_rect(mx + 16, iy + MENU_ITEM / 2 - 6, 12, 12, 3,
                      gfx_mix(t->overlay, pip, (int)p * 255 / ANIM_FULL));
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

/* --- the apps on the panel -----------------------------------------------
 *
 * A pinned app is an icon in the panel whether it is running or not, and a
 * running one whose program is pinned is shown by that icon rather than
 * twice. So the panel reads left to right as: what you keep, then what you
 * happen to have open.
 */

/* How bright a colour reads, which is not its average: the eye weighs green
   far more than blue. */
static u32 luma_of(u32 c) {
    return ((((c >> 16) & 0xFF) * 77) + (((c >> 8) & 0xFF) * 151)
            + ((c & 0xFF) * 28)) >> 8;
}

/* An app's own colour, from its path, so the same program is the same
   colour on every machine and adding one does not renumber the others.
   The palette is the theme's own presets: six colours already chosen to
   work together, rather than six invented here. */
static u32 pin_colour(const char *path) {
    u32 hash = 2166136261u;
    for (const char *p = path; *p; p++) hash = (hash ^ (u32)*p) * 16777619u;
    return theme_preset_accent((int)(hash % THEME_PRESETS));
}

/* The window a program has open, or nothing. The topmost, so clicking an
   icon twice does not walk backwards through a stack of them. */
static window_t *window_for_app(const char *path) {
    if (!path || !path[0]) return 0;
    for (int i = nwin - 1; i >= 0; i--)
        if (!strcmp(stack[i]->app, path)) return stack[i];
    return 0;
}

/* True for a window that already has an icon on the panel. */
static bool shown_as_pin(const window_t *w) {
    return w->app[0] && pins_find(w->app) >= 0;
}

static int taskbar_pin_at(int mx, int my) {
    int y = taskbar_y();
    if (my < y + 2 || my >= y + TASKBAR_H - 2) return -1;

    int x = taskbar_pins_x();
    for (int i = 0; i < pins_count(); i++, x += PIN_STEP) {
        if (mx >= x - 4 && mx < x + PIN_ICON + 4) return i;
    }
    return -1;
}

/* Which slot a drag is over, which is not the same question: anywhere past
   the last icon is the last slot rather than nowhere. */
static int pin_slot_at(int mx) {
    int rel = mx - taskbar_pins_x() + PIN_STEP / 2;
    if (rel < 0) return 0;
    int slot = rel / PIN_STEP;
    if (slot >= pins_count()) slot = pins_count() - 1;
    return slot;
}

/* There are no icon files anywhere in this project and inventing a format
   to hold five pictures would be worse than this: a rounded square in the
   app's colour with the first letter of its name in it, which tells them
   apart at a glance and costs nothing to carry. */
static void draw_pin_icon(int x, int y, const pin_t *p, bool hot, bool running) {
    const theme_t *t = theme();
    u32 c = pin_colour(p->path);

    if (hot) fb_round_rect(x - 4, y - 3, PIN_ICON + 8, PIN_ICON + 6, 7, t->raised);
    fb_round_rect(x, y, PIN_ICON, PIN_ICON, 6, c);
    fb_rect((u32)(x + 6), (u32)y, (u32)(PIN_ICON - 12), 1, lighten(c, 70));
    fb_round_frame(x, y, PIN_ICON, PIN_ICON, 6, darken(c, 60));

    char first[2] = { p->label[0], 0 };
    if (first[0] >= 'a' && first[0] <= 'z') first[0] = (char)(first[0] - 32);
    u32 ink = luma_of(c) > 140 ? darken(c, 200) : RGB(0xFF, 0xFF, 0xFF);
    face_text(x + (PIN_ICON - face_width(first, FACE_BODY_BOLD)) / 2,
              y + (PIN_ICON - face_height(FACE_BODY_BOLD)) / 2,
              first, ink, FACE_BODY_BOLD);

    /* A bar under it while the program is running, in the same place the
       chips put theirs, so one line across the panel says what is open. */
    if (running)
        fb_rect((u32)(x + PIN_ICON / 2 - 4), (u32)(y + PIN_ICON + 3), 8, 2,
                t->accent);
}

static void clock_text(char *out, u32 cap) {
    if (rtc_present()) { rtc_format_short(out, cap); return; }

    /* A machine with no usable CMOS clock counts from boot instead, which
       is what this had before there was a clock to read. */
    u32 secs = (u32)(timer_ticks() / timer_hz());
    kformat(out, cap, "up %d:%02d", secs / 60, secs % 60);
}

/* The right hand end of the panel: the clock against the edge, and the
   speaker to the left of it. Worked out in one place so that what is drawn
   and what a click lands on cannot drift apart. */
static int taskbar_volume_x(void) {
    char clock[24];
    clock_text(clock, sizeof(clock));
    return (int)fb_width() - TASKBAR_GAP - 16
           - face_width(clock, FACE_BODY) - 14 - VOL_W;
}

static bool on_volume_button(int mx, int my) {
    int y = taskbar_y();
    if (my < y + 5 || my >= y + TASKBAR_H - 5) return false;
    int x = taskbar_volume_x();
    return mx >= x && mx < x + VOL_W;
}

/* A speaker, drawn rather than stored: a box, a cone widening out of it, and
   one bar for quiet or two for loud. A cross instead when it is off. */
static void draw_speaker(int x, int y, u32 fg, int level) {
    fb_rect((u32)(x + 2), (u32)(y + 5), 3, 6, fg);
    for (int i = 0; i < 5; i++)
        fb_rect((u32)(x + 5 + i), (u32)(y + 5 - i), 1, (u32)(6 + i * 2), fg);

    if (level <= 0) {
        for (int i = 0; i < 6; i++) {
            fb_rect((u32)(x + 13 + i), (u32)(y + 5 + i), 2, 1, fg);
            fb_rect((u32)(x + 13 + i), (u32)(y + 10 - i), 2, 1, fg);
        }
        return;
    }
    fb_rect((u32)(x + 13), (u32)(y + 5), 2, 6, fg);
    if (level > 45) fb_rect((u32)(x + 17), (u32)(y + 2), 2, 12, fg);
}

/* Where the slider's track is, which the drawing and the dragging both
   need and neither should work out for itself. */
static void volume_track(int *x, int *y, int *w) {
    int px = taskbar_volume_x() + VOL_W / 2 - VOLPOP_W / 2;
    if (px < TASKBAR_GAP) px = TASKBAR_GAP;
    if (px + VOLPOP_W > (int)fb_width() - TASKBAR_GAP)
        px = (int)fb_width() - TASKBAR_GAP - VOLPOP_W;

    *x = px + 16;
    *y = taskbar_y() - VOLPOP_H - 8 + VOLPOP_H / 2 - 3;
    *w = VOLPOP_W - 32 - 40;
}

static void draw_volume_panel(void) {
    if (!volume_open) return;
    const theme_t *t = theme();

    int tx, ty, tw;
    volume_track(&tx, &ty, &tw);
    int px = tx - 16, py = taskbar_y() - VOLPOP_H - 8;

    if (t->shadows) fb_shadow(px, py, VOLPOP_W, VOLPOP_H, 10, SHADOW);
    fb_round_rect(px, py, VOLPOP_W, VOLPOP_H, 10, t->overlay);
    fb_rect((u32)(px + 10), (u32)py, (u32)(VOLPOP_W - 20), 1, t->sheen);
    fb_round_frame(px, py, VOLPOP_W, VOLPOP_H, 10, t->hairline);

    int level = theme()->volume;
    int on = tw * level / 100;

    fb_round_rect(tx, ty, tw, 6, 3, t->raised);
    if (on > 0) fb_round_rect(tx, ty, on, 6, 3, t->accent);
    fb_round_rect(tx + on - 6, ty - 4, 13, 13, 6, t->accent);
    fb_round_frame(tx + on - 6, ty - 4, 13, 13, 6, darken(t->accent, 60));

    char num[8];
    kformat(num, sizeof(num), "%d", level);
    face_text(tx + tw + 14, py + (VOLPOP_H - face_height(FACE_BODY)) / 2,
              num, t->text, FACE_BODY);
}

static void volume_from_pointer(int mx) {
    int tx, ty, tw;
    volume_track(&tx, &ty, &tw);
    if (tw <= 0) return;

    int v = (mx - tx) * 100 / tw;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    theme_set_volume(v);
    need_frame();
}

static void draw_taskbar(void) {
    const theme_t *t = theme();
    int y = taskbar_y();
    int px = TASKBAR_GAP;
    int pw = (int)fb_width() - TASKBAR_GAP * 2;

    if (t->shadows) fb_shadow(px, y, pw, TASKBAR_H, TASKBAR_R, SHADOW);
    fb_round_rect(px, y, pw, TASKBAR_H, TASKBAR_R, t->overlay);
    fb_rect((u32)(px + TASKBAR_R), (u32)y, (u32)(pw - TASKBAR_R * 2), 1, t->sheen);
    fb_round_frame(px, y, pw, TASKBAR_H, TASKBAR_R, t->hairline);

    /* The launcher badge, which is also what the desktop menu opens from. */
    bool badge_hot = menu_open;
    fb_round_rect(px + 8, y + 5, TASKBAR_BADGE_W, TASKBAR_H - 10, 6,
                  badge_hot ? t->accent : t->raised);
    face_text(px + 20, y + (TASKBAR_H - face_height(FACE_HEAD_BOLD)) / 2, "zelr",
              badge_hot ? t->accent_text : t->accent, FACE_HEAD_BOLD);

    /* The pinned apps. The one being dragged is left out and drawn under
       the pointer instead; the list has already been reordered around it,
       so what is on screen is the answer rather than a preview of it. */
    int hot = pin_moved ? -1 : taskbar_pin_at(last_mx, last_my);
    int px_pin = taskbar_pins_x();
    for (int i = 0; i < pins_count(); i++, px_pin += PIN_STEP) {
        const pin_t *p = pin_at(i);
        if (pin_moved && i == pin_press) continue;
        draw_pin_icon(px_pin, y + 4, p, i == hot, window_for_app(p->path) != 0);
    }
    if (pin_moved && pin_press >= 0) {
        const pin_t *p = pin_at(pin_press);
        if (p) draw_pin_icon(pin_at_x - PIN_ICON / 2, y + 2, p, true,
                             window_for_app(p->path) != 0);
    }

    int x = taskbar_chips_x();
    for (int i = 0; i < nwin; i++) {
        window_t *w = stack[i];
        if (shown_as_pin(w)) continue;          /* its icon already says so */
        bool focused = (i == nwin - 1) && !w->minimized;
        int chip_face = focused ? FACE_BODY_BOLD : FACE_BODY;
        int tw = face_width(w->title, chip_face) + 24;
        if (x + tw > (int)fb_width() - TASKBAR_GAP - 120) break;

        u32 chip = t->raised;
        if (focused)           chip = t->accent_soft;
        else if (w->minimized) chip = t->surface;

        fb_round_rect(x, y + 5, tw, TASKBAR_H - 10, 6, chip);
        face_text(x + 12, y + (TASKBAR_H - face_height(chip_face)) / 2, w->title,
                  focused ? t->text : t->text_dim, chip_face);

        /* A full underline for the window in front, a short stub for one
           that is only put away, so the taskbar says where everything is. */
        if (focused)
            fb_rect((u32)(x + 8), (u32)(y + TASKBAR_H - 6), (u32)(tw - 16), 2, t->accent);
        else if (w->minimized)
            fb_rect((u32)(x + tw / 2 - 5), (u32)(y + TASKBAR_H - 6), 10, 2, t->text_dim);

        x += tw + 6;
    }

    /* The time on the right, and the volume to the left of it. */
    char clock[24];
    clock_text(clock, sizeof(clock));
    face_text((int)fb_width() - TASKBAR_GAP - face_width(clock, FACE_BODY) - 16,
              y + (TASKBAR_H - face_height(FACE_BODY)) / 2, clock, t->text,
              FACE_BODY);

    {
        int vx = taskbar_volume_x();
        bool hot = volume_open || on_volume_button(last_mx, last_my);
        if (hot) fb_round_rect(vx - 2, y + 5, VOL_W + 4, TASKBAR_H - 10, 6,
                               t->raised);
        draw_speaker(vx + 4, y + 9, t->volume ? t->text : t->text_dim,
                     t->volume);
    }

    /* And the name of the icon under the pointer, above it. */
    if (hot >= 0) {
        const pin_t *p = pin_at(hot);
        int tw = face_width(p->label, FACE_BODY) + 18;
        int th = 22;
        int tx = taskbar_pins_x() + hot * PIN_STEP + PIN_ICON / 2 - tw / 2;
        int ty = y - th - 6;

        if (tx < TASKBAR_GAP) tx = TASKBAR_GAP;
        if (tx + tw > (int)fb_width() - TASKBAR_GAP)
            tx = (int)fb_width() - TASKBAR_GAP - tw;

        if (t->shadows) fb_shadow(tx, ty, tw, th, 6, SHADOW);
        fb_round_rect(tx, ty, tw, th, 6, t->overlay);
        fb_round_frame(tx, ty, tw, th, 6, t->hairline);
        face_text(tx + 9, ty + (th - face_height(FACE_BODY)) / 2,
                  p->label, t->text, FACE_BODY);
    }
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

    /* A window covering the screen means the wallpaper and everything
       under that window are drawn and then painted over. The screen is
       filled flat instead of skipped entirely, because chrome blends with
       what is beneath it and what is beneath it would otherwise be the
       last frame, blended again, every frame. */
    int cover = covering_index();
    if (cover >= 0) fb_rect(0, 0, fb_width(), fb_height(), theme()->desktop);
    else            draw_wallpaper();

    for (int i = cover > 0 ? cover : 0; i < nwin; i++) {
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

    for (int i = 0; i < cover; i++) stack[i]->dirty = false;

    draw_snap_preview();
    draw_resize_preview();
    draw_taskbar();
    draw_volume_panel();
    draw_menu();
    draw_cursor(last_mx, last_my);
    if (frame_is_whole || dmg_x1 <= dmg_x0) {
        fb_flush();
    } else {
        fb_flush_rect(dmg_x0, dmg_y0, dmg_x1 - dmg_x0, dmg_y1 - dmg_y0);
    }
    /* Cleared, not set: the next frame sends only what asks to be sent, and
       everything that changes the whole screen already calls need_frame. */
    frame_is_whole = false;
    dmg_x0 = dmg_y0 = dmg_x1 = dmg_y1 = 0;
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
    need_frame();
    if (i < 0 || i >= MENU_N) return;

    if (MENU[i].program) { launch(MENU[i].program); return; }

    if (!strcmp(MENU[i].label, "System info")) app_about();
    else if (!strcmp(MENU[i].label, "Close all")) { while (nwin > 0) wm_close(stack[nwin - 1]); }
    else if (!strcmp(MENU[i].label, "Leave desktop")) running = false;
    else if (!strcmp(MENU[i].label, "Shut down")) {
        /* Anything not yet on the disk goes first: a machine that is
           switched off does not come back to finish writing. */
        diskfs_flush();
        power_off();
        /* Only here when the firmware wanted something this kernel does
           not do. Leaving the desktop is then the useful thing, because
           the console says why. */
        running = false;
    }
}

/* What the launcher calls a program, so an app pinned from its own window
   is named the way the menu names it rather than however the program
   happened to title the window. */
static const char *label_for(const char *path, const char *fallback) {
    for (int i = 0; i < MENU_N; i++)
        if (MENU[i].program && !strcmp(MENU[i].program, path))
            return MENU[i].label;
    return fallback;
}

/* Clicking an icon: start the program, or if it is already running, do what
   clicking its chip would have done. */
static void pin_activate(int i) {
    const pin_t *p = pin_at(i);
    if (!p) return;

    window_t *w = window_for_app(p->path);
    if (!w) { launch(p->path); return; }

    if (w->minimized)              set_minimized(w, false);
    else if (w == stack[nwin - 1]) set_minimized(w, true);
    else                           wm_raise(w);
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
    if (y + h > panel_rest_y()) y = panel_rest_y() - h - 4;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    menu_x = x; menu_y = y;
    menu_open = true;
    menu_hover = -1;
    menu_left = -1;
    menu_since = timer_ticks();
    menu_hover_since = 0;
    need_frame();
}

/* --- the screen changing size -------------------------------------------
 *
 * Somebody picks a size in the settings window, which writes it into the
 * same file the colours live in, and this is what notices. Nothing else
 * about the desktop knows a resolution: everything is laid out from
 * fb_width and fb_height every frame, so the only work here is that the
 * windows do not know the ground moved under them.
 */
static void screen_changed(void) {
    for (int i = 0; i < nwin; i++) {
        window_t *w = stack[i];

        if (w->maximized) {
            /* Refitted rather than un-maximised and done again, so where it
               would go back to is still where it came from. */
            int x, y, cw, ch;
            snap_rect(SNAP_FULL, &x, &y, &cw, &ch);
            place(w, x, y, cw, ch);
        } else {
            int cw = w->cw, ch = w->ch;
            if (cw > (int)fb_width() - 8) cw = (int)fb_width() - 8;
            if (ch > work_h() - WM_TITLE_H - 8) ch = work_h() - WM_TITLE_H - 8;
            if (cw < 160) cw = 160;
            if (ch < 80) ch = 80;
            if (cw != w->cw || ch != w->ch) wm_resize(w, cw, ch);

            if (w->x + wm_outer_w(w) > (int)fb_width())
                w->x = (int)fb_width() - wm_outer_w(w);
            if (w->y + wm_outer_h(w) > work_h())
                w->y = work_h() - wm_outer_h(w);
            if (w->x < 0) w->x = 0;
            if (w->y < 0) w->y = 0;
        }

        /* Where it goes back to has to be somewhere it can be seen. */
        if (w->restore_x + w->restore_cw > (int)fb_width())
            w->restore_x = 0;
        if (w->restore_y + w->restore_ch > work_h())
            w->restore_y = 0;
    }
    need_frame();
}

/* The size asked for, if it is not the size we have and the card will take
   it. A framebuffer the firmware set up cannot be changed at all, so this
   quietly does nothing there rather than pretending. */
/* What the machine started with. That is what no setting at all means,
   and what putting everything back goes to. */
static u32 boot_w, boot_h;

static void apply_screen_size(void) {
    const theme_t *t = theme();
    u32 w = t->want_w ? (u32)t->want_w : boot_w;
    u32 h = t->want_h ? (u32)t->want_h : boot_h;

    if (!w || !h) return;
    if (w == fb_width() && h == fb_height()) return;
    if (!fb_mode_settable()) return;
    if (!fb_set_mode(w, h)) return;

    fbcon_init();          /* the console behind this is a different shape */
    screen_changed();
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
    int y = taskbar_y();
    if (my < y + 5 || my >= y + TASKBAR_H - 5) return -1;

    int x = taskbar_chips_x();
    for (int i = 0; i < nwin; i++) {
        if (shown_as_pin(stack[i])) continue;
        /* Measured in the face it is drawn in. The window in front carries
           a heavier title, so measuring every chip in the regular weight
           puts the edge of the widest one in the wrong place and a click
           near it lands on the neighbour. */
        bool focused = (i == nwin - 1) && !stack[i]->minimized;
        int face = focused ? FACE_BODY_BOLD : FACE_BODY;
        int tw = face_width(stack[i]->title, face) + 24;
        if (x + tw > (int)fb_width() - TASKBAR_GAP - 120) break;
        if (mx >= x && mx < x + tw) return i;
        x += tw + 6;
    }
    return -1;
}

static bool on_taskbar_badge(int mx, int my) {
    int y = taskbar_y();
    return my >= y + 5 && my < y + TASKBAR_H - 5
           && mx >= TASKBAR_GAP + 8
           && mx < TASKBAR_GAP + 8 + TASKBAR_BADGE_W;
}

static void handle_mouse(int mx, int my, u8 buttons) {
    bool pressed_now = (buttons & 1) && !(last_buttons & 1);
    bool right_now   = (buttons & 2) && !(last_buttons & 2);
    bool released    = !(buttons & 1) && (last_buttons & 1);

    if (menu_open) {
        int over = menu_item_at(mx, my);
        if (over != menu_hover) {
            menu_left = menu_hover;
            menu_hover = over;
            menu_hover_since = timer_ticks();
            need_frame();
        }
        /* The right button over an entry keeps it on the panel, or takes it
           off again if it is already there. */
        if (right_now && over >= 0 && MENU[over].program) {
            int at = pins_find(MENU[over].program);
            if (at >= 0) pins_remove(at);
            else         pins_add(MENU[over].label, MENU[over].program);
            menu_open = false;
            need_frame();
            return;
        }

        if (pressed_now) {
            if (over >= 0) { menu_choose(over); return; }
            /* A click anywhere else dismisses it, and does nothing more. */
            menu_open = false;
            need_frame();
            return;
        }
    }

    if (volume_drag && (buttons & 1)) { volume_from_pointer(mx); return; }

    if (volume_open && (pressed_now || right_now)) {
        int tx, ty, tw;
        volume_track(&tx, &ty, &tw);
        int px = tx - 16, py = taskbar_y() - VOLPOP_H - 8;
        if (mx >= px && mx < px + VOLPOP_W && my >= py && my < py + VOLPOP_H) {
            volume_drag = true;
            volume_from_pointer(mx);
            return;
        }
        if (!on_volume_button(mx, my)) {
            /* A click anywhere else puts it away and does nothing more,
               which is what the launcher does and what people expect of
               anything that opened over the top of something. */
            volume_open = false;
            need_frame();
            return;
        }
    }

    if (released) {
        /* An icon that was being held. Dragged, and the order it has been
           put into is written down; not dragged, and it was a click. */
        volume_drag = false;

        if (pin_press >= 0) {
            if (pin_moved) pins_save();
            else           pin_activate(pin_press);
            pin_press = -1;
            pin_moved = false;
            panel_frame();
        }

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
        need_frame();
    }

    if (resizing) {
        resize_from_pointer(mx, my, &resize_cw, &resize_ch);
        need_frame();
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
        if (zone != snap_preview) { snap_preview = zone; need_frame(); }

        shake_note(mx);
        if (theme()->quirks && shake_detected()) {
            for (int i = 0; i < nwin; i++)
                if (stack[i] != dragging) set_minimized(stack[i], true);
            shake_reset();
        }

        need_frame();
        return;
    }

    /* An icon being dragged along the panel.
     *
     * The list is reordered as the pointer crosses each slot rather than
     * when the button comes up, so the icons move out of the way while it
     * is happening. A few pixels of travel are allowed first: a click with
     * a steady hand still moves the mouse by one or two, and a click that
     * silently reordered the panel would be a mystery. */
    if (pin_press >= 0 && (buttons & 1)) {
        int moved = mx - pin_press_x;
        if (moved < 0) moved = -moved;
        if (moved > 5) pin_moved = true;

        if (pin_moved) {
            int slot = pin_slot_at(mx);
            if (slot != pin_press) { pins_move(pin_press, slot); pin_press = slot; }
            pin_at_x = mx;
            panel_frame();
        }
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
            if (menu_open) { menu_open = false; need_frame(); }
            else open_menu_at(TASKBAR_GAP,
                              panel_rest_y() - (MENU_N * MENU_ITEM + 12) - 8);
            return;
        }

        if (on_volume_button(mx, my)) {
            if (right_now) {
                /* The right button is mute, which is the volume it was at
                   kept somewhere so that unmuting is not a guess. */
                if (theme()->volume) {
                    volume_before_mute = theme()->volume;
                    theme_set_volume(0);
                } else {
                    theme_set_volume(volume_before_mute);
                }
            } else {
                volume_open = !volume_open;
            }
            need_frame();
            return;
        }

        int pin = taskbar_pin_at(mx, my);
        if (pin >= 0) {
            if (right_now) { pins_remove(pin); panel_frame(); return; }
            pin_press = pin;
            pin_press_x = mx;
            pin_at_x = mx;
            pin_moved = false;
            return;
        }

        int chip = taskbar_chip_at(mx, my);
        if (chip >= 0) {
            window_t *c = stack[chip];

            /* The right button keeps the program on the panel after the
               window it is running in has gone. */
            if (right_now) {
                if (c->app[0]) pins_add(label_for(c->app, c->title), c->app);
                panel_frame();
                return;
            }

            /* Clicking the window already in front puts it away; clicking
               anything else brings it back and raises it. */
            if (c->minimized)                 set_minimized(c, false);
            else if (chip == nwin - 1)        set_minimized(c, true);
            else                              wm_raise(c);
            return;
        }

        /* The panel itself, anywhere there is nothing on it. Worth saying
           out loud now that the work area can reach the bottom of the
           screen: without this, clicking an empty stretch of the panel
           opens the launcher underneath it. */
        int ty = taskbar_y();
        if (my >= ty && my < ty + TASKBAR_H) return;

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
        need_frame();
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
    pins_init();

    /* Before anything is applied, so there is something to go back to. */
    sound_set_volume((u32)theme()->volume);
    if (!boot_w) { boot_w = fb_width(); boot_h = fb_height(); }
    apply_screen_size();

    running = true;
    menu_open = false;
    mouse_set_autodraw(false);          /* the manager draws the pointer */
    last_mx = mouse_x();
    last_my = mouse_y();
    last_buttons = mouse_buttons();
    need_frame();
    last_theme_check = timer_ticks();

    while (running) {
        int mx = mouse_x(), my = mouse_y();
        u8 buttons = mouse_buttons();

        if (mx != last_mx || my != last_my || buttons != last_buttons) {
            last_mx = mx; last_my = my;
            handle_mouse(mx, my, buttons);
            last_buttons = buttons;
            need_frame();
        }

        /* Asked every pass rather than only when the mouse moves: what the
           panel should be doing also changes when a window is maximised,
           closed or put away, and none of those touch the pointer. */
        panel_update(last_my);

        /* The wheel goes to whatever is under the pointer rather than to
           whatever has focus. Pointing at a list and turning the wheel is
           one gesture, and every desktop that makes you click first is
           wrong about it. */
        i32 wheel = mouse_take_scroll();
        if (wheel && on_volume_button(last_mx, last_my)) {
            theme_set_volume(theme()->volume - wheel * 5);
            need_frame();
        } else if (wheel) {
            bool on_title = false;
            button_t btn = BTN_NONE;
            window_t *over = window_at(last_mx, last_my, &on_title, &btn);
            if (over && over->owned_by_user) {
                wm_event_t ev = { WM_EV_SCROLL,
                                  last_mx - (over->x + WM_BORDER),
                                  wheel, 0, 0 };
                wm_push_event(over, &ev);
            }
        }

        int c = kbd_trygetchar();
        if (c >= 0 && KEY_CODE(c) == 27) {         /* escape */
            if (menu_open) { menu_open = false; need_frame(); }
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
            if (theme_reload()) { apply_screen_size(); need_frame(); }

            /* The same arrangement for the taskbar's own list, which the
               settings window writes and this side has to be told about.
             *
             * Not while an icon is being dragged. The list is reordered as
             * the pointer crosses each slot and only written down when the
             * button comes up, so re-reading the file in the middle of that
             * puts the old order back under the hand moving it. Which is
             * exactly what it did, and the check for dragging an icon is
             * what said so. */
            if (pin_press < 0 && pins_reload()) panel_frame();
        }

        /* A window that has redrawn needs its own rectangle sent, not the
           whole screen. With a terminal on the desktop this fired on nearly
           every pass, which is why no frame was ever a partial one. */
        for (int i = 0; i < nwin; i++) {
            window_t *w = stack[i];
            if (!w->dirty) continue;
            if (w->minimized) { need_frame(); continue; }
            need_frame_in(w->x - SHADOW - 2, w->y - SHADOW - 2,
                          wm_outer_w(w) + SHADOW * 4,
                          wm_outer_h(w) + SHADOW * 4);
        }

        /* The taskbar clock ticks, so repaint at least once a second even
           when nothing else changed. A wallpaper that moves needs it far
           more often than that, but only while it is the one on. */
        static u64 last_tick;
        u64 every = (wallpaper_moves(theme()->wallpaper) && covering_index() < 0)
                    ? timer_hz() / 12 : timer_hz();
        if (!every) every = 1;
        if (timer_ticks() - last_tick >= every) {
            last_tick = timer_ticks();
            need_frame();
        }

        if (still_moving(panel_since, PANEL_MS)) panel_frame();

        /* Anything mid transition wants the next frame. This is the only
           thing driving an animation: no timer of its own, no frame count,
           and nothing to switch off when it finishes. */
        if (menu_open && (still_moving(menu_since, MENU_MS)
                          || still_moving(menu_hover_since, HOVER_MS))) {
            need_frame_in(menu_x - 14, menu_y - 14,
                          MENU_W + 28, MENU_N * MENU_ITEM + 12 + 28);
            need_frame_in(last_mx - 2, last_my - 2, 20, 28);
        }

        if (needs_composite) {
            composite();
            needs_composite = false;
        } else {
            /* Nothing to draw and nothing moved, so stop asking. This loop
               used to poll the mouse as fast as the processor would go,
               which on a laptop is a warm one for a desktop sitting still.
               A halt ends on the next interrupt, and every reason to do
               another pass is one: the timer, a key, the mouse. */
            task_idle_wait();
        }
    }

    while (nwin > 0) wm_close(stack[nwin - 1]);
    running = false;
    menu_open = false;
    dragging = resizing = mouse_capture = 0;
    pin_press = -1;
    pin_moved = false;
    volume_open = false;
    volume_drag = false;
    panel_shown = true;
    panel_since = 0;
    snap_preview = SNAP_NONE;
    shake_reset();
    mouse_set_autodraw(true);
}
