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
#include "power.h"
#include "sound.h"
#include "diskfs.h"
#include "fbcon.h"
#include "sched.h"
#include "net.h"
#include "netdev.h"
#include "wifi.h"
#include "io.h"

/* A bar that floats clear of the edges rather than one welded to them.
 *
 * It used to be flush to the bottom and the full width, and the reason
 * written down here was that a panel is part of the machine rather than a
 * card lying on the desktop. That was true of a panel made of the same grey
 * as a window. It is not true of one made of glass: a floating bar with the
 * wallpaper running underneath and past it on every side reads as a layer
 * above the desktop, which is what it is, and it gives the wallpaper its
 * corners back.
 *
 * Forty four pixels by default. It was thirty four, which was a strip, and
 * then fifty six, which was a slab: a floating bar is read as an object,
 * and an object a quarter the height of nothing on the screen is furniture
 * rather than a dock. Forty four clears a twenty pixel clock with air
 * around it and nothing more.
 *
 * By default, though, and not by decree. Every number on this bar is in
 * the settings file now, including whether it floats at all: a gap of
 * nought welds it back to the bottom edge, which is what it used to be and
 * what somebody may well want again.
 *
 * The names stay so the eighty places that use them do not change, and so
 * that none of them can hold a number the settings window has since moved.
 */
static int dock_h_now(void) {
    int v = theme()->dock_h;
    return v >= 20 ? v : 44;
}
static int dock_gap_now(void)  { int v = theme()->dock_gap;  return v >= 0 ? v : 14; }
static int dock_side_now(void) { int v = theme()->dock_side; return v >= 0 ? v : 16; }
static int dock_r_now(void)    { int v = theme()->dock_radius; return v >= 0 ? v : 14; }

#define TASKBAR_H   dock_h_now()
#define TASKBAR_GAP dock_gap_now()
#define DOCK_SIDE   dock_side_now()
#define DOCK_R      dock_r_now()
#define TASKBAR_R   0

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

/* What replaced the row of pinned icons.
 *
 * Six coloured letters in circles told you which six programs somebody had
 * chosen, and nothing else: to reach a seventh you opened the launcher
 * anyway. A field in the middle of the bar reaches all of them and is the
 * one thing on a desktop that is always worth having within one click. It
 * is also, unlike a row of badges, the same size whatever is installed.
 *
 * Clicking it opens the launcher with the field focused, and what is typed
 * from then on narrows what the launcher shows. See menu_matches below:
 * there is no index and no ranking, because thirteen labels is a loop.
 *
 * Its width is a setting and its height is not: a field in a bar wants to
 * be the height of the bar less its margins, and a person who set the two
 * independently would mostly be setting them wrong. */
static int dock_x(void);
static int dock_w(void);
static int taskbar_net_x(void);

static int dock_find_w(void) {
    int v = theme()->dock_search_w;
    if (v < 80) v = 80;

    /* Whatever is left after the name at one end and the tray at the
       other. The setting is what somebody asked for and this is what the
       bar can give them: a field seven hundred pixels wide in a bar six
       hundred wide is a field with the clock inside it. */
    int room = dock_w() - (theme()->dock_brand ? 110 : 20) - 170;
    if (room < 80) room = 80;
    if (v > room) v = room;
    return v;
}
static int dock_find_h(void) {
    int v = TASKBAR_H - 18;
    return v < 16 ? 16 : v;
}

#define DOCK_FIND_W dock_find_w()
#define DOCK_FIND_H dock_find_h()

static int dock_x(void) { return DOCK_SIDE; }
static int dock_w(void) { return (int)fb_width() - DOCK_SIDE * 2; }

static int dock_find_x(void) {
    return dock_x() + (dock_w() - DOCK_FIND_W) / 2;
}

static int taskbar_chips_x(void) {
    if (!theme()->dock_brand) return dock_x() + 16;
    return dock_x() + 16 + TASKBAR_BADGE_W + 18;
}

/* Where the row of window chips has to stop.
 *
 * At the tray, and at the field before it if there is one. The field is
 * centred in the bar and the chips grow from the left, which is fine while
 * the bar is wide and is a collision the moment it is not: with the dock
 * inset two hundred pixels a side, the first chip was drawn straight
 * through the middle of the field. Both the drawing and the hit test ask
 * this, so neither can be right about where a chip ends while the other is
 * wrong. */
static int taskbar_chips_end(void) {
    int end = taskbar_net_x() - 8;
    if (theme()->look == LOOK_MODERN && theme()->dock_search) {
        int before = dock_find_x() - 10;
        if (before < end) end = before;
    }
    return end;
}
/* The volume button, left of the clock, and the little panel it opens. */
#define VOL_W      30
#define VOLPOP_W   208
#define VOLPOP_H   40

/* And the network, left of that. */
#define NET_W      26
#define NETPOP_W   268
#define NETPOP_H   150

/* The launcher, in two columns.
 *
 * It was one: thirteen rows of one word each, which is a list rather than a
 * menu and which got taller every time the machine learned to do something
 * new. Thirteen is already most of the height of the screen and there is no
 * number of programs at which that stops being true.
 *
 * Two columns do not get taller. The left is what kind of thing you want,
 * the right is the things of that kind, and the panel is as tall as the
 * longest of those -- five rows today and five rows after another dozen
 * programs, because a dozen programs is two or three more categories at
 * most and a category costs one row.
 */
#define MENU_RAIL  132          /* the kinds, down the left */
#define MENU_PANE  152          /* the things of that kind */
#define MENU_PAD   10
#define MENU_W     (MENU_PAD * 2 + MENU_RAIL + MENU_PANE)
#define MENU_ITEM  32

/* An index into the panel that says which column it came from, so that one
   hover and one click handler can serve both and neither can mistake the
   third kind for the third program. */
#define MENU_RIGHT 100
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

/* How long each transition takes.
 *
 * Three constants in a fixed ratio to one another, moved together by one
 * setting: what a person means by "faster" is the whole desktop rather
 * than the launcher in particular, and three sliders would mostly be three
 * ways to make the desktop disagree with itself. A hundred and twenty is
 * what these were written at. */
static u32 anim_scale(u32 ms) {
    int k = theme()->anim_ms;
    if (k < 20) k = 120;
    return (ms * (u32)k) / 120;
}

#define MENU_MS    anim_scale(200)   /* the launcher arriving */
#define HOVER_MS   anim_scale(110)   /* a highlight under the pointer */

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
static bool net_open;
static bool volume_drag;
static int  volume_before_mute = 70;

/* The launcher. Open when someone clicks the desktop or the taskbar badge. */
static bool menu_open;
static int  menu_x, menu_y;
static int  menu_hover = -1;
static int  menu_left = -1;         /* the item the pointer has just left */
static u64  menu_since;             /* when it opened */
static u64  menu_hover_since;       /* when the highlight last changed */
static int  menu_cat;               /* which kind the right column shows */

/* The desktop's own menu, which is a different thing from the launcher.
 *
 * A launcher answers "what can this machine run"; a menu on the desktop
 * answers "what can I do here, now, to this". They were the same panel
 * because there was only one panel, and the result was that the right
 * button on the wallpaper offered thirteen programs and nothing at all
 * about the wallpaper. */
static bool ctx_open;
static int  ctx_x, ctx_y;
static int  ctx_hover = -1, ctx_left = -1;
static u64  ctx_since, ctx_hover_since;

#define CTX_W    204
#define CTX_ITEM 30
#define CTX_PAD  8

static const struct {
    const char *label;
    const char *program;
} CTX[] = {
    { "Open terminal",     "/bin/term" },
    { "Browse files",      "/bin/files" },
    { "Select all",        0 },
    { "Personalise",       "/bin/settings" },
    { "Close all windows", 0 },
    { "System info",       0 },
};
#define CTX_N ((int)(sizeof(CTX) / sizeof(CTX[0])))
#define CTX_H (CTX_N * CTX_ITEM + CTX_PAD * 2)

static u64 last_theme_check;

/* What the launcher offers. A null program means the kernel handles it. */
typedef struct {
    const char *label;
    const char *program;
} mitem_t;

static const mitem_t M_WORK[] = {
    { "Terminal",     "/bin/term" },
    { "Files",        "/bin/files" },
    { "Notes",        "/bin/notes" },
    { "Calculator",   "/bin/calc" },
};
static const mitem_t M_WEB[] = {
    { "Browser",      "/bin/browser" },
};
static const mitem_t M_MEDIA[] = {
    { "Paint",        "/bin/paint" },
    { "Music",        "/bin/music" },
};
static const mitem_t M_SYSTEM[] = {
    { "Settings",     "/bin/settings" },
    { "Monitor",      "/bin/monitor" },
    { "System info",  0 },
};
static const mitem_t M_SESSION[] = {
    { "Close all",    0 },
    { "Leave desktop", 0 },
    { "Shut down",    0 },
};

/* There is no Games here, and a category with nothing in it is a promise
   the machine cannot keep: a row that opens onto an empty column reads as
   something broken rather than as something not written yet. It goes in
   the day there is a game to put in it. */
#define CAT(a) a, ((int)(sizeof(a) / sizeof((a)[0])))
static const struct {
    const char *name;
    const mitem_t *items;
    int n;
} MCAT[] = {
    { "Productivity", CAT(M_WORK)    },
    { "Internet",     CAT(M_WEB)     },
    { "Media",        CAT(M_MEDIA)   },
    { "System",       CAT(M_SYSTEM)  },
    { "Session",      CAT(M_SESSION) },
};
#undef CAT

#define MCAT_N ((int)(sizeof(MCAT) / sizeof(MCAT[0])))

/* As tall as the taller column, worked out rather than written down: a
   constant here is one that is wrong the first time a category grows. */
static int menu_rows(void) {
    int rows = MCAT_N;
    for (int i = 0; i < MCAT_N; i++)
        if (MCAT[i].n > rows) rows = MCAT[i].n;
    return rows;
}

/* The height with nothing typed, which is also the most it can be: the
   panel is laid out upward from the dock, so this is the room reserved
   for it whatever it ends up showing. */
static int menu_full_h(void) { return MENU_PAD * 2 + menu_rows() * MENU_ITEM; }

/* --- what has been typed into the field on the dock -----------------------
 *
 * The field was drawn and did nothing. It looked like somewhere to type,
 * which is the whole reason it is the shape it is, and a field that looks
 * like somewhere to type and is not is worse than no field: every person
 * who tries it learns that this desktop's controls are decoration.
 *
 * What it does now is the ordinary thing. Anything typed while the
 * launcher is up goes in here, the panel shows what matches instead of the
 * kinds, and return runs the first of them. There is no index and no
 * ranking: thirteen labels is a loop.
 */
#define MENU_QMAX 24
#define MENU_HITS 4             /* the query takes the top row of the panel */

static char menu_q[MENU_QMAX + 1];
static int  menu_qn;

static char menu_low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* The query anywhere in the label, not only at the front: somebody after
   the browser may well type "web" or "brow", and one of those working
   while the other silently finds nothing is the kind of search that
   teaches people not to use it. */
static bool menu_matches(const char *label) {
    if (menu_qn <= 0) return true;
    for (int at = 0; label[at]; at++) {
        int i = 0;
        while (i < menu_qn && label[at + i]
               && menu_low(label[at + i]) == menu_low(menu_q[i])) i++;
        if (i == menu_qn) return true;
    }
    return false;
}

static int menu_hits(const mitem_t **out, int max) {
    int n = 0;
    for (int c = 0; c < MCAT_N; c++)
        for (int j = 0; j < MCAT[c].n; j++) {
            if (!menu_matches(MCAT[c].items[j].label)) continue;
            if (n < max) out[n] = &MCAT[c].items[j];
            n++;
        }
    return n < max ? n : max;
}

/* As tall as what is in it. A panel that stays five rows deep while it is
   showing one match is a box with a word at the top of it, and the empty
   four rows read as four things that failed to load. */
static int menu_h(void) {
    if (menu_qn <= 0) return menu_full_h();
    const mitem_t *hit[MENU_HITS];
    int n = menu_hits(hit, MENU_HITS);
    if (n < 1) n = 1;                 /* the row that says there are none */
    return MENU_PAD * 2 + (n + 1) * MENU_ITEM;
}

/* It shrinks upward, because it hangs from the dock. menu_y is where the
   full panel's top would be, and everything that draws or hits the panel
   goes through this so the two can never disagree about where it is. */
static int menu_top(void) { return menu_y + menu_full_h() - menu_h(); }

/* The two the rest of the kernel asks for through wm.h. Both fall back to
   what they used to be when nobody has said otherwise, which matters at
   boot: the self test builds windows before theme_init has run, and a
   title bar of nought pixels is a window with no title bar. */
int wm_title_h(void) {
    int v = theme()->title_h;
    return v >= 16 ? v : 32;
}

int wm_border(void) {
    int v = theme()->border;
    return (v >= 0 && v <= 8) ? v : 1;
}

bool wm_active(void) { return running; }

int wm_outer_w(const window_t *w) { return w->cw + WM_BORDER * 2; }
int wm_outer_h(const window_t *w) { return w->ch + WM_TOP + WM_BORDER; }

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

    /* A pointer that has only moved overwrites the last move rather than
       queueing behind it. Where the pointer is now is the only thing a
       program wants out of a move, and the walk across the screen that gets
       it somewhere is thirty of them.

       This is what a full queue used to be made of, and the queue dropped
       whatever arrived while it was full. The thing that arrived was the
       press, the release turned up once the backlog had drained, and a
       release with no press before it sets nothing: the program sees the
       button go up from a state where it was already up. So the click did
       not happen, three times over, with the pointer sitting on the button
       in the photograph. Nothing in the path reports a dropped event, which
       is why this looked like the button not working.

       What must never be folded away is the event that changed the buttons,
       and being a move is not enough to say an event did not: the release
       is a mouse event with no buttons in it, and so is the move that comes
       after it. Folding the second into the first moves the release to
       wherever the pointer went next, which for a button means it was let
       go somewhere else and the click is lost again. So an event is only
       overwritten when it carries the same buttons as this one and as the
       one before it, which is what makes it a move and nothing else. */
    if (ev->type == WM_EV_MOUSE && !(ev->buttons & 0x80)
        && w->q_head != w->q_tail) {
        u32 last = (w->q_head + WM_EVENT_QUEUE - 1) % WM_EVENT_QUEUE;
        wm_event_t *prev = &w->queue[last];
        if (prev->type == WM_EV_MOUSE && prev->buttons == ev->buttons
            && w->q_prev_buttons == ev->buttons) {
            *prev = *ev;
            return;
        }
    }

    u32 next = (w->q_head + 1) % WM_EVENT_QUEUE;
    if (next == w->q_tail) return;          /* full: drop this one, keep the backlog */
    w->queue[w->q_head] = *ev;
    w->q_head = next;

    if (ev->type == WM_EV_MOUSE) {
        w->q_prev_buttons = w->q_last_buttons;
        w->q_last_buttons = ev->buttons;
    }
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
#define PANEL_MS   anim_scale(170)   /* going away, and coming back */
#define PANEL_EDGE 3        /* how close to the bottom brings it back */

static bool panel_shown = true;
static u64  panel_since;

/* An icon being pressed, and dragged along the panel to reorder it. */

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
    int mode = theme()->dock_hide;
    if (mode <= 0) return panel_rest_y();     /* it never moves, so nor does this */
    if (mode >= 2) return (int)fb_height();   /* it is never there to be avoided */
    return panel_in_the_way() ? (int)fb_height() : panel_rest_y();
}

int wm_work_height(void) { return work_h(); }

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
    if (menu_open || ctx_open || dragging) return true;

    /* Never, when a window wants the room, or always: the middle one is
       what this did with no setting at all, and it is still the default.
       The other two are the two things people actually ask for. */
    int mode = theme()->dock_hide;
    if (mode <= 0) return true;
    if (mode == 1 && !panel_in_the_way()) return true;

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

/* --- the icons on the desktop ---------------------------------------------
 *
 * Drawn rather than stored, because a bitmap for each would be a file format,
 * a loader and a directory to keep them in before anything appeared on the
 * screen, and these are thirty two pixels square: at that size a drawn shape
 * and a stored one are the same picture.
 *
 * They open on a double click. Single click selects, which is what every
 * desktop that has ever had icons does, and the reason is that dragging one
 * has to start with putting the pointer on it.
 */
/* The pictogram is drawn in a thirty two unit square and scaled to
   whatever size the icons are set to, so that making them bigger moves the
   whole drawing rather than leaving a thirty two pixel picture in the
   middle of a sixty four pixel tile. IS() is that scaling, and every
   coordinate inside a pictogram goes through it. */
static int icon_tile(void) {
    int v = theme()->icon_size;
    return v >= 16 ? v : 32;
}
static int icon_gap(void) {
    int v = theme()->icon_gap;
    return v >= 8 ? v : 30;
}

#define ICON_TILE   icon_tile()
#define IS(v)       ((v) * ICON_TILE / 32)
#define ICON_CELL_W (ICON_TILE + icon_gap() * 2)
#define ICON_CELL_H (ICON_TILE + icon_gap() + 26)
#define ICON_LEFT  18
#define ICON_TOP   20

/* Where a window may start without landing on the icon column. With no
   icons on the desktop there is no column, and a window that still
   cascaded from behind one would be avoiding something that is not
   there. */
int wm_icons_right(void) {
    return theme()->desk_icons ? ICON_LEFT + ICON_CELL_W : ICON_LEFT;
}

static const struct {
    const char *label;
    const char *program;
    int kind;
} DESK[] = {
    { "Terminal", "/bin/term",     0 },
    { "Files",    "/bin/files",    1 },
    { "Notes",    "/bin/notes",    2 },
    { "Paint",    "/bin/paint",    3 },
    { "Settings", "/bin/settings", 4 },
};
#define DESK_N ((int)(sizeof(DESK) / sizeof(DESK[0])))

/* Which icons are picked out. A set rather than one of them, because a
   band drawn across the desktop can land on more than one and a desktop
   that can only hold one selected icon cannot say what a band caught. */
static u32 desk_sel;
static int desk_last_click = -1;
static u64 desk_last_tick;

static bool desk_is_sel(int i) { return (desk_sel >> i) & 1u; }

/* The band itself: where it started, where the pointer is, and which
   button is holding it down, because either of them can draw one. */
static bool band_on;
static int  band_ax, band_ay, band_bx, band_by;
static u8   band_button;

/* The five pictograms, flat, and drawn to whatever size the icons are.
 *
 * They were drawn with bevels, which is what a picture of a thing looked
 * like when a screen had a hundred and thirty thousand pixels and every
 * edge had to be argued for. On a desktop made of light they read as five
 * embossed stickers. Flat shapes with one rounded corner apiece say the
 * same thing and stop drawing attention to their own edges.
 *
 * Every coordinate in them goes through IS(), which is the same number
 * measured in a thirty two unit square and scaled to the tile. Without it
 * an icon size of sixty four is a sixty four pixel tile with a thirty two
 * pixel drawing in the corner of it, which is not a bigger icon, it is a
 * broken one. IS1() is for thicknesses, which round to nothing at small
 * sizes and then the line they were drawing is simply absent.
 */
#define IS1(v) (IS(v) < 1 ? 1 : IS(v))

static void icon_terminal(int x, int y) {
    bool modern = theme()->look == LOOK_MODERN;
    if (modern) {
        fb_round_rect_aa(x, y, ICON_TILE, ICON_TILE, IS(8),
                         RGB(0x15, 0x19, 0x22), 255);
        fb_round_rect_aa(x, y, ICON_TILE, ICON_TILE, IS(8),
                         RGB(0x3A, 0x44, 0x58), 120);
    } else {
        fb_rect((u32)x, (u32)y, (u32)ICON_TILE, (u32)ICON_TILE,
                RGB(0x1A, 0x1E, 0x24));
        fb_bevel(x, y, ICON_TILE, ICON_TILE, RGB(0x60, 0x66, 0x70),
                 RGB(0x40, 0x46, 0x50), RGB(0x10, 0x12, 0x16),
                 RGB(0x06, 0x07, 0x09));
    }
    u32 green = RGB(0x4C, 0xD9, 0x8A);

    /* The chevron is stepped a pixel at a time rather than scaled step by
       step: scaling the steps leaves gaps between them at any size where
       one unit is more than one pixel, and a dotted chevron is not a
       chevron. */
    int n = IS1(4), th = IS1(2);
    for (int i = 0; i < n; i++) {
        fb_rect((u32)(x + IS(7) + i), (u32)(y + IS(10) + i),
                (u32)th, (u32)th, green);
        fb_rect((u32)(x + IS(7) + i), (u32)(y + IS(18) - i),
                (u32)th, (u32)th, green);
    }
    fb_rect((u32)(x + IS(14)), (u32)(y + IS(20)), (u32)IS1(10), (u32)th, green);
}

static void icon_folder(int x, int y) {
    u32 body = RGB(0xE2, 0xB8, 0x4E), edge = RGB(0x9A, 0x77, 0x22);
    if (theme()->look == LOOK_MODERN) {
        /* Two tones and no outline: the back of the folder a shade darker
           than its front is the whole of what makes it read as a folder. */
        fb_round_rect_aa(x + IS(2), y + IS(6), IS1(13), IS1(8), IS(3),
                         RGB(0xC9, 0xA0, 0x3E), 255);
        fb_round_rect_aa(x + IS(2), y + IS(9), ICON_TILE - IS(4), IS1(18),
                         IS(4), body, 255);
        fb_round_rect_aa(x + IS(2), y + IS(9), ICON_TILE - IS(4), IS1(9),
                         IS(4), RGB(0xFF, 0xFF, 0xFF), 26);
        return;
    }
    fb_rect((u32)(x + IS(2)), (u32)(y + IS(7)), (u32)IS1(12), (u32)IS1(4), body);
    fb_rect((u32)(x + IS(2)), (u32)(y + IS(10)), (u32)(ICON_TILE - IS(4)),
            (u32)IS1(17), body);
    fb_rect((u32)(x + IS(2)), (u32)(y + IS(10)), (u32)(ICON_TILE - IS(4)), 1,
            RGB(0xF4, 0xD8, 0x8E));
    fb_bevel_thin(x + IS(2), y + IS(7), IS1(12), IS1(5),
                  RGB(0xF4, 0xD8, 0x8E), edge);
    fb_bevel_thin(x + IS(2), y + IS(10), ICON_TILE - IS(4), IS1(17),
                  RGB(0xF4, 0xD8, 0x8E), edge);
}

static void icon_page(int x, int y) {
    if (theme()->look == LOOK_MODERN) {
        fb_round_rect_aa(x + IS(5), y + IS(2), IS1(22), IS1(28), IS(4),
                         RGB(0xFC, 0xFC, 0xFF), 255);
        /* The lines in two weights, because a page of writing is not five
           identical rules and the difference is what makes it read as one. */
        for (int i = 0; i < 5; i++)
            fb_round_rect_aa(x + IS(9), y + IS(8 + i * 4),
                             i == 4 ? IS1(7) : IS1(14), IS1(2), IS(1),
                             i == 0 ? RGB(0x37, 0x3D, 0x4A)
                                    : RGB(0x8A, 0x92, 0xA4), 255);
        return;
    }
    fb_rect((u32)(x + IS(6)), (u32)(y + IS(3)), (u32)IS1(20), (u32)IS1(26),
            RGB(0xFA, 0xFA, 0xF6));
    fb_bevel_thin(x + IS(6), y + IS(3), IS1(20), IS1(26),
                  RGB(0xFF, 0xFF, 0xFF), RGB(0x7A, 0x78, 0x74));
    for (int i = 0; i < 5; i++)
        fb_rect((u32)(x + IS(10)), (u32)(y + IS(8 + i * 4)),
                (u32)(i == 4 ? IS1(7) : IS1(12)), 1, RGB(0x50, 0x56, 0x62));
}

static void icon_paint(int x, int y) {
    u32 ring = RGB(0xC8, 0x8A, 0x58);
    if (theme()->look == LOOK_MODERN) {
        /* Five wells of colour on a rounded palette, and no rim: the
           colours are the picture. */
        fb_round_rect_aa(x + IS(3), y + IS(7), IS1(26), IS1(18), IS(6),
                         RGB(0xF3, 0xE3, 0xCC), 255);
        static const u32 WELL[5] = {
            RGB(0xE5, 0x48, 0x43), RGB(0x3C, 0x74, 0xD0),
            RGB(0x3F, 0xB9, 0x63), RGB(0xEF, 0xC1, 0x3C),
            RGB(0x2B, 0x2E, 0x38) };
        for (int i = 0; i < 3; i++)
            fb_round_rect_aa(x + IS(7 + i * 6), y + IS(11), IS1(5), IS1(5),
                             IS(2), WELL[i], 255);
        for (int i = 0; i < 2; i++)
            fb_round_rect_aa(x + IS(10 + i * 6), y + IS(18), IS1(5), IS1(5),
                             IS(2), WELL[3 + i], 255);
        return;
    }
    fb_rect((u32)(x + IS(4)), (u32)(y + IS(8)), (u32)IS1(24), (u32)IS1(16),
            RGB(0xE8, 0xC8, 0xA0));
    fb_bevel_thin(x + IS(4), y + IS(8), IS1(24), IS1(16),
                  RGB(0xFA, 0xE8, 0xCC), ring);
    static const u32 DAB[5] = {
        RGB(0xD8, 0x44, 0x40), RGB(0x3C, 0x74, 0xD0), RGB(0x50, 0xB0, 0x60),
        RGB(0xE0, 0xC0, 0x40), RGB(0x30, 0x30, 0x38) };
    for (int i = 0; i < 3; i++)
        fb_rect((u32)(x + IS(8 + i * 6)), (u32)(y + IS(12)),
                (u32)IS1(4), (u32)IS1(4), DAB[i]);
    for (int i = 0; i < 2; i++)
        fb_rect((u32)(x + IS(11 + i * 6)), (u32)(y + IS(18)),
                (u32)IS1(4), (u32)IS1(4), DAB[3 + i]);
}

static void icon_sliders(int x, int y) {
    const theme_t *t = theme();
    if (t->look == LOOK_MODERN) {
        /* Three rails and three knobs, on nothing. A panel behind them
           would be a second rectangle inside the tile that is already
           behind them. */
        for (int i = 0; i < 3; i++) {
            int ly = y + IS(9 + i * 7);
            fb_round_rect_aa(x + IS(5), ly, IS1(22), IS1(3), IS(1),
                             RGB(0x8A, 0x92, 0xA4), 255);
            int knob = x + IS(5) + IS(i == 0 ? 14 : i == 1 ? 4 : 10);
            fb_round_rect_aa(knob, ly - IS(3), IS1(7), IS1(9), IS(3),
                             t->accent, 255);
        }
        return;
    }
    fb_rect((u32)(x + IS(3)), (u32)(y + IS(4)), (u32)IS1(26), (u32)IS1(24),
            t->surface);
    fb_bevel(x + IS(3), y + IS(4), IS1(26), IS1(24), t->edge_hi, t->edge_light,
             t->edge_shadow, t->edge_dark);
    for (int i = 0; i < 3; i++) {
        int ly = y + IS(10 + i * 6);
        fb_rect((u32)(x + IS(7)), (u32)ly, (u32)IS1(18), (u32)IS1(2),
                t->edge_shadow);
        fb_rect((u32)(x + IS(7)), (u32)(ly + IS1(1)), (u32)IS1(18), 1,
                t->edge_hi);
        int knob = x + IS(8) + IS(i == 1 ? 11 : i * 6);
        fb_rect((u32)knob, (u32)(ly - IS(2)), (u32)IS1(4), (u32)IS1(6),
                t->accent);
        fb_bevel_thin(knob, ly - IS(2), IS1(4), IS1(6), t->edge_hi,
                      t->edge_dark);
    }
}

static void draw_desk_icon(int i, int x, int y, bool selected) {
    const theme_t *t = theme();
    int tx = x + (ICON_CELL_W - ICON_TILE) / 2;

    /* A tile behind it.
     *
     * The pictograms are small and were drawn to sit on a grey desktop; on
     * a photograph of one they are five loose shapes floating on a
     * gradient. A rounded square of the wallpaper's own light behind each
     * gives them somewhere to sit, which is what every desktop that stopped
     * looking like 1998 did first — and it costs one rectangle.
     */
    if (t->look == LOOK_MODERN) {
        int pad = IS1(10), r = IS1(14);
        int bx = tx - pad, by = y - pad;
        int bw = ICON_TILE + pad * 2, bh = ICON_TILE + pad * 2;

        bool hot = !band_on
                && last_mx >= x && last_mx < x + ICON_CELL_W
                && last_my >= y - pad && last_my < y + ICON_TILE + pad;

        if (selected) {
            fb_round_rect_aa(bx, by, bw, bh, r, t->accent, 110);
            fb_round_rect_aa(bx, by, bw, bh, r, RGB(0xFF, 0xFF, 0xFF), 60);
        } else {
            fb_round_rect_aa(bx, by, bw, bh, r, RGB(0xFF, 0xFF, 0xFF),
                             hot ? 42 : 20);
            fb_round_rect_aa(bx, by, bw, bh, r, RGB(0xFF, 0xFF, 0xFF),
                             hot ? 55 : 28);
            fb_round_rect_aa(bx + 1, by + 1, bw - 2, bh - 2, r - 1,
                             RGB(0xFF, 0xFF, 0xFF), hot ? 34 : 14);
        }
    }

    switch (DESK[i].kind) {
        case 0: icon_terminal(tx, y); break;
        case 1: icon_folder(tx, y);   break;
        case 2: icon_page(tx, y);     break;
        case 3: icon_paint(tx, y);    break;
        default: icon_sliders(tx, y); break;
    }

    const char *label = DESK[i].label;
    int lw = face_width(label, FACE_BODY);
    int lx = x + (ICON_CELL_W - lw) / 2;
    int ly = y + ICON_TILE + (theme()->look == LOOK_MODERN ? 16 : 6);

    if (t->look == LOOK_MODERN) {
        /* White, with a soft dark copy underneath rather than a box behind.
           The wallpaper can be any colour at all, and one dark pixel offset
           by one is enough to keep the label readable on all of them
           without putting a panel on the desktop. */
        face_text(lx + 1, ly + 1, label, RGB(0x06, 0x09, 0x14), FACE_BODY);
        face_text(lx, ly, label,
                  selected ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xEC, 0xF0, 0xF8),
                  FACE_BODY);
        return;
    }

    if (selected) {
        fb_rect((u32)(lx - 4), (u32)(ly - 2), (u32)(lw + 8),
                (u32)(face_height(FACE_BODY) + 4), t->accent);
        face_text(lx, ly, label, t->accent_text, FACE_BODY);
        return;
    }

    /* A shadow under the label rather than a panel behind it. The wallpaper
       can be any colour at all, including one the text would vanish into,
       and one dark pixel offset by one is enough to stop that without
       putting a box on the desktop. */
    face_text(lx + 1, ly + 1, label, RGB(0x10, 0x14, 0x1A), FACE_BODY);
    face_text(lx, ly, label, RGB(0xF2, 0xF4, 0xF8), FACE_BODY);
}

static void draw_desk_icons(void) {
    if (!theme()->desk_icons) return;
    int y = ICON_TOP;
    for (int i = 0; i < DESK_N; i++) {
        draw_desk_icon(i, ICON_LEFT, y, desk_is_sel(i));
        y += ICON_CELL_H;
    }
}

/* Where icon i is, as a rectangle, so that drawing it, clicking it and
   sweeping a band over it all ask the same question. */
static void desk_icon_rect(int i, int *x, int *y, int *w, int *h) {
    *x = ICON_LEFT;
    *y = ICON_TOP + i * ICON_CELL_H;
    *w = ICON_CELL_W;
    *h = ICON_CELL_H - 6;
}

/* The band, in the order a rectangle wants rather than the order it was
   drawn in: it is dragged in whichever of the four directions the hand
   went, and everything downstream wants a top left and a size. */
static void band_rect(int *x, int *y, int *w, int *h) {
    int x0 = band_ax < band_bx ? band_ax : band_bx;
    int x1 = band_ax < band_bx ? band_bx : band_ax;
    int y0 = band_ay < band_by ? band_ay : band_by;
    int y1 = band_ay < band_by ? band_by : band_ay;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

static void band_select(void) {
    int bx, by, bw, bh;
    band_rect(&bx, &by, &bw, &bh);
    desk_sel = 0;
    for (int i = 0; i < DESK_N; i++) {
        int ix, iy, iw, ih;
        desk_icon_rect(i, &ix, &iy, &iw, &ih);
        if (bx < ix + iw && bx + bw > ix && by < iy + ih && by + bh > iy)
            desk_sel |= 1u << i;
    }
}

static void draw_band(void) {
    if (!band_on) return;
    const theme_t *t = theme();
    int x, y, w, h;
    band_rect(&x, &y, &w, &h);
    if (w < 1 || h < 1) return;

    if (t->look == LOOK_MODERN) {
        /* A wash with a line round it. The wash alone is a smudge on the
           wallpaper and the line alone is a frame with a hole in it; the
           two together are the only thing a person reads as "everything
           inside here". */
        fb_round_rect_aa(x, y, w, h, 3, t->accent, 46);
        fb_round_rect_aa(x, y, w, 1, 0, t->accent, 190);
        fb_round_rect_aa(x, y + h - 1, w, 1, 0, t->accent, 190);
        fb_round_rect_aa(x, y, 1, h, 0, t->accent, 190);
        fb_round_rect_aa(x + w - 1, y, 1, h, 0, t->accent, 190);
    } else {
        /* The dotted frame, which is what a machine with no alpha to
           spend drew instead and which still reads perfectly well. */
        for (int i = 0; i < w; i += 2) {
            fb_rect((u32)(x + i), (u32)y, 1, 1, t->text);
            fb_rect((u32)(x + i), (u32)(y + h - 1), 1, 1, t->text);
        }
        for (int i = 0; i < h; i += 2) {
            fb_rect((u32)x, (u32)(y + i), 1, 1, t->text);
            fb_rect((u32)(x + w - 1), (u32)(y + i), 1, 1, t->text);
        }
    }
}

static int desk_icon_at(int mx, int my) {
    if (!theme()->desk_icons) return -1;
    for (int i = 0; i < DESK_N; i++) {
        int x, y, w, h;
        desk_icon_rect(i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h) return i;
    }
    return -1;
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

    case WALLPAPER_BLOOM: {
        /* Depth, made of one gradient and four lights.
         *
         * A flat colour behind a desktop is honest and it is also the thing
         * that makes a screenshot look like a diagram of a desktop rather
         * than a desktop. What a background needs is somewhere for the eye
         * to rest that is not a window, and the cheapest way to have one is
         * light: a deep ground, a wash from a cooler top to a warmer
         * bottom, and a few soft sources sitting well off centre.
         *
         * Off centre on purpose. Lights placed symmetrically read as a
         * pattern; placed the way they are here they read as a photograph
         * of something, which is what every desktop background of the last
         * fifteen years has been trying to be. */
        int W = (int)fb_width();

        /* A deep ground that is not quite black and not quite blue, warming
           very slightly toward the bottom. The old one went from navy to a
           lighter teal and the lighter end is what made it read as a
           gradient: a background gets darker toward the bottom in every
           photograph ever taken, because that is where the ground is. */
        fb_vgradient(0, 0, W, h, RGB(0x0A, 0x0F, 0x2A), RGB(0x07, 0x0A, 0x1B));

        /* Two lights, not four. Four is a pattern; two is a time of day.
           One large and cool, high and off to the left, and one smaller and
           warmer well below it on the other side, so the eye travels
           between them rather than settling in the middle. */
        if (t->glows) {
        fb_glow((W * 3) / 10, h / 4, (W * 3) / 5, (h * 3) / 5,
                RGB(0x3A, 0x7B, 0xFF), 96);
        fb_glow((W * 4) / 5, (h * 7) / 10, (W * 2) / 5, (h * 2) / 5,
                RGB(0x9B, 0x4B, 0xE0), 64);

        /* A small bright core inside the first, which is what gives the
           larger wash somewhere to fall away from and stops it reading as
           flat colour with the edges blurred. */
        fb_glow((W * 3) / 10, h / 4, W / 9, h / 9, RGB(0xC8, 0xDE, 0xFF), 70);
        }

        /* And the corners pulled down, which is the difference between a
           background and a wash. */
        if (t->vignette > 0) fb_vignette(t->vignette);
        break;
    }

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
   because pressing it would do nothing.

   Wider than they are tall, and bevelled like every other button on the
   desktop, because that is what makes them read as things to press. The
   round tinted dots that were here before said what they did by colour
   alone, which is a convention rather than an affordance: they look the
   same whether the machine is listening or not. */
static int btn_w(void) { int v = theme()->button_w; return v >= 12 ? v : 30; }
static int btn_h(void) { int v = theme()->button_h; return v >= 10 ? v : 24; }

#define BTN_W  btn_w()
#define BTN_H  btn_h()
#define BTN_GAP  2

typedef enum { BTN_NONE = 0, BTN_CLOSE, BTN_MAX, BTN_MIN } button_t;

static void button_box(const window_t *w, button_t which, int *bx, int *by) {
    int slot = (which == BTN_CLOSE) ? 0 : (which == BTN_MAX ? 1 : 2);
    *bx = w->x + wm_outer_w(w) - WM_BORDER - 2 - BTN_W
          - slot * (BTN_W + BTN_GAP);
    *by = w->y + WM_BORDER + (WM_TITLE_H - BTN_H) / 2;
}

static button_t button_at(const window_t *w, int mx, int my) {
    button_t order[3] = { BTN_CLOSE, BTN_MAX, BTN_MIN };
    for (int i = 0; i < 3; i++) {
        if (order[i] == BTN_MAX && !w->resizable) continue;
        int bx, by;
        button_box(w, order[i], &bx, &by);
        if (mx >= bx && mx < bx + BTN_W && my >= by && my < by + BTN_H)
            return order[i];
    }
    return BTN_NONE;
}

/* The grip in the bottom right corner. Only there on a window that can
   actually be resized. */
#define GRIP 16

static bool on_grip(const window_t *w, int mx, int my) {
    if (!w->resizable || w->maximized) return false;
    int gx = w->x + wm_outer_w(w) - GRIP;
    int gy = w->y + wm_outer_h(w) - GRIP;
    return mx >= gx && mx < gx + GRIP && my >= gy && my < gy + GRIP;
}

/* --- the pieces a built surface is made of --------------------------------
 *
 * Raised and sunk are the same routine with the light coming from the other
 * side, which is the entire trick and the reason it is worth having in one
 * place: every button, well, panel and groove on this desktop is one of
 * these two calls, so they all catch the light from the same direction. */
static void raised(int x, int y, int w, int h) {
    const theme_t *t = theme();
    fb_bevel(x, y, w, h, t->edge_hi, t->edge_light, t->edge_shadow, t->edge_dark);
}

static void sunken(int x, int y, int w, int h) {
    const theme_t *t = theme();
    fb_bevel(x, y, w, h, t->edge_dark, t->edge_shadow, t->edge_light, t->edge_hi);
}

/* A button, with its face, that goes down when it is held. Pressed is not a
   colour change: the bevel inverts and the label moves a pixel down and
   right, so the thing genuinely looks depressed rather than highlighted. */
static void button_face(int x, int y, int w, int h, bool down, bool hot) {
    const theme_t *t = theme();

    /* Modern, a button at rest is not drawn at all. It appears under the
       pointer as a soft rounded patch and it deepens when it is pressed,
       which is how every desktop of the last decade does it: a row of
       buttons that are each a raised slab is a row of boxes, and what
       somebody is actually looking at is the labels. */
    if (t->look == LOOK_MODERN) {
        int r = h / 3 > 8 ? 8 : h / 3;
        if (down)      fb_round_rect_aa(x, y, w, h, r, t->accent_soft, 235);
        else if (hot)  fb_round_rect_aa(x, y, w, h, r, t->sheen, 110);
        return;
    }

    u32 face = hot && !down ? gfx_mix(t->surface, t->edge_hi, 60) : t->surface;
    fb_rect((u32)x, (u32)y, (u32)w, (u32)h, face);
    if (down) sunken(x, y, w, h);
    else      raised(x, y, w, h);
}

/* --- the marks on the title bar buttons ----------------------------------- */

/* The three marks, centred in whatever size the button is. They were drawn
 * at fixed offsets from the top left, which was the same thing as centred
 * while the button was sixteen by fourteen and stopped being it the moment
 * the button grew. */
#define MARK 10

static void mark_minimise(int x, int y, u32 c) {
    fb_rect((u32)(x + (BTN_W - MARK) / 2), (u32)(y + BTN_H / 2), MARK, 1, c);
}

static void mark_maximise(int x, int y, u32 c) {
    int bx = x + (BTN_W - MARK) / 2, by = y + (BTN_H - MARK) / 2;
    fb_rect((u32)bx, (u32)by, MARK, MARK, c);
    fb_rect((u32)(bx + 1), (u32)(by + 1), MARK - 2, MARK - 2, theme()->surface);
}

static void mark_restore(int x, int y, u32 c) {
    /* Two boxes, the back one peeking out, which is what a window that is
       already filling the screen offers to undo. */
    fb_rect((u32)(x + 6), (u32)(y + 2), 6, 6, c);
    fb_rect((u32)(x + 7), (u32)(y + 4), 4, 3, theme()->surface);
    fb_rect((u32)(x + 3), (u32)(y + 5), 6, 6, c);
    fb_rect((u32)(x + 4), (u32)(y + 7), 4, 3, theme()->surface);
}

static void mark_close(int x, int y, u32 c) {
    int bx = x + (BTN_W - MARK) / 2, by = y + (BTN_H - MARK) / 2;
    for (int i = 0; i < MARK; i++) {
        fb_rect((u32)(bx + i), (u32)(by + i), 1, 1, c);
        fb_rect((u32)(bx + i), (u32)(by + MARK - 1 - i), 1, 1, c);
    }
}

static void draw_chrome(window_t *w, bool focused) {
    const theme_t *t = theme();
    int ow = wm_outer_w(w), oh = wm_outer_h(w);

    /* Built, the frame is a raised plane with the window sunk into it, and
       it is square, because a bevel has to turn a corner to read as one and
       a rounded corner has nowhere to put the four edges.
     *
       Modern, it is a pane: one fill, one hairline around it, rounded, with
       a shadow underneath saying which window is in front. The shadow is
       the whole of that job here, which is why an unfocused window gets a
       shallower one rather than a drained title bar. */
    if (t->look == LOOK_MODERN) {
        int r = t->corner;
        if (r > 0 && t->shadows)
            fb_shadow(w->x, w->y, ow, oh, r, focused ? 7 : 3);
        fb_round_rect_aa(w->x, w->y, ow, oh, r, t->surface, 255);

        /* The hairline is what separates a light window from a light
           wallpaper, and there is no other edge doing it.
         *
           On the focused window it takes the accent. Without that, the only
           thing saying which window is in front is the depth of its shadow,
           and a shadow is not something anybody reads deliberately: two
           overlapping windows of the same colour with the same flat title
           bar are two windows, and which one the keyboard is talking to is
           a question the screen has to answer. */
        u32 edge = focused ? gfx_mix(t->stroke, t->accent, 190) : t->stroke;
        fb_round_rect_aa(w->x, w->y, ow, oh, r, edge, focused ? 235 : 110);
        fb_round_rect_aa(w->x + 1, w->y + 1, ow - 2, oh - 2, r > 0 ? r - 1 : 0,
                         t->surface, 255);
    } else {
        fb_rect((u32)w->x, (u32)w->y, (u32)ow, (u32)oh, t->surface);
        raised(w->x, w->y, ow, oh);
    }

    /* --- the title bar ---------------------------------------------------- */
    int tx = w->x + WM_BORDER;
    int ty = w->y + WM_BORDER;
    int tw = ow - WM_BORDER * 2;

    if (t->look == LOOK_MODERN) {
        /* No band of colour. The title bar is the same material as the
           window, and the thing that says a window is in front is the
           shadow under it and the weight of its title. A coloured bar is a
           second answer to a question already answered, and it is the
           loudest thing on a screen that is mostly somebody's document. */
        u32 fg = focused ? t->text : t->text_dim;
        face_text(tx + 10, ty + (WM_TITLE_H - face_height(FACE_BODY)) / 2,
                  w->title, fg, focused ? FACE_BODY_BOLD : FACE_BODY);
    } else {
        if (focused) fb_hgradient(tx, ty, tw, WM_TITLE_H, t->title_a, t->title_b);
        else         fb_hgradient(tx, ty, tw, WM_TITLE_H, t->title_off_a,
                                  t->title_off_b);

        u32 title_fg = focused ? t->title_fg : t->title_off_fg;
        face_text(tx + 6, ty + (WM_TITLE_H - face_height(FACE_BODY_BOLD)) / 2,
                  w->title, title_fg, FACE_BODY_BOLD);
    }

    button_t order[3] = { BTN_CLOSE, BTN_MAX, BTN_MIN };
    for (int i = 0; i < 3; i++) {
        if (order[i] == BTN_MAX && !w->resizable) continue;

        int bx, by;
        button_box(w, order[i], &bx, &by);
        bool hot = last_mx >= bx && last_mx < bx + BTN_W
                && last_my >= by && last_my < by + BTN_H;

        u32 mark = t->text;
        if (t->look == LOOK_MODERN) {
            /* Nothing at rest, a patch under the pointer, and red under the
               pointer on close. Red on the one button that cannot be undone
               and on no other is the clearest thing a title bar does. */
            if (hot && order[i] == BTN_CLOSE) {
                fb_round_rect_aa(bx, by, BTN_W, BTN_H, 5,
                                 RGB(0xD9, 0x3A, 0x3A), 240);
                mark = RGB(0xFF, 0xFF, 0xFF);
            } else if (hot) {
                fb_round_rect_aa(bx, by, BTN_W, BTN_H, 5, t->sheen, 130);
            }
        } else {
            button_face(bx, by, BTN_W, BTN_H, false, hot);
        }
        switch (order[i]) {
        case BTN_CLOSE: mark_close(bx, by, mark); break;
        case BTN_MAX:
            if (w->maximized) mark_restore(bx, by, mark);
            else              mark_maximise(bx, by, mark);
            break;
        default:        mark_minimise(bx, by, mark); break;
        }
    }

    /* --- and the client area, sunk into the frame ------------------------- */
    int cx = w->x + WM_BORDER;
    int cy = w->y + WM_TOP;
    if (t->look == LOOK_MODERN) {
        /* A hairline rather than a well. Sinking the page into the frame is
           what a built surface does to say "this part is the program"; with
           no bevels anywhere else it reads as a groove nobody asked for. */
        fb_frame((u32)(cx - 1), (u32)(cy - 1), (u32)(w->cw + 2),
                 (u32)(w->ch + 2), t->stroke);
    } else {
        sunken(cx - 1, cy - 1, w->cw + 2, w->ch + 2);
    }

    /* Three short strokes in the corner, which is how a grip has looked for
       long enough that nobody needs to be told. Bevelled rather than drawn
       in one colour, so it belongs to the same surface as everything else. */
    if (w->resizable && !w->maximized) {
        int gx = w->x + ow - 4, gy = w->y + oh - 4;
        for (int i = 0; i < 3; i++) {
            int d = i * 4;
            fb_rect((u32)(gx - d - 1), (u32)(gy - 2), 3, 1, t->edge_hi);
            fb_rect((u32)(gx - d - 1), (u32)(gy - 1), 3, 1, t->edge_dark);
            fb_rect((u32)(gx - 2), (u32)(gy - d - 1), 1, 3, t->edge_hi);
            fb_rect((u32)(gx - 1), (u32)(gy - d - 1), 1, 3, t->edge_dark);
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
    *ch = fh - WM_TOP - WM_BORDER;
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
    int ch = my + resize_off_y - resizing->y - WM_TOP - WM_BORDER;

    if (cw < 160) cw = 160;
    if (ch < 80)  ch = 80;
    if (resizing->x + cw + WM_BORDER * 2 > (int)fb_width())
        cw = (int)fb_width() - resizing->x - WM_BORDER * 2;
    if (resizing->y + ch + WM_TOP + WM_BORDER > work_h())
        ch = work_h() - resizing->y - WM_TOP - WM_BORDER;

    *cw_out = cw;
    *ch_out = ch;
}

/* Which edge of the screen the pointer is close enough to for a snap. */
static snap_t snap_zone_at(int mx, int my) {
    if (!theme()->snap) return SNAP_NONE;
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
    int h = menu_h();

    /* It comes up from below rather than appearing. Eight pixels is enough
       to be read as movement and short enough not to be waited for. */
    u32 p = phase_of(menu_since, MENU_MS);
    int rise = (int)(((ANIM_FULL - p) * 8) / ANIM_FULL);
    int mx = menu_x, my = menu_top() + rise;

    if (t->look == LOOK_MODERN) {
        /* A floating panel, so it gets the deepest shadow on the desktop:
           it is the one thing that is genuinely above everything else and
           it is gone again in a moment. */
        if (t->shadows) fb_shadow(mx, my, MENU_W, h, 16, 12);
        /* Flat, with no gloss on it. A menu is a list of words somebody is
           reading right now, and a sheen across the top of one puts a
           gradient behind the first two items and not behind the rest. The
           gloss belongs on the bar, which is furniture. */
        fb_round_rect_aa(mx, my, MENU_W, h, 16, t->overlay, 250);
        fb_round_rect_aa(mx, my, MENU_W, h, 16, t->stroke, 85);
        fb_round_rect_aa(mx + 1, my + 1, MENU_W - 2, h - 2, 15, t->overlay, 252);
    } else {
        fb_rect((u32)mx, (u32)my, MENU_W, (u32)h, t->surface);
        raised(mx, my, MENU_W, h);
    }

    /* There was a strip of colour down the left of this with the letters
       Z E L R stacked in it, and a coloured tile beside every entry holding
       the first letter of the word next to it. Thirteen of those is not
       thirteen icons, it is the same word twice, and the strip was the one
       thing on the screen that was decoration rather than information.
     *
       What is down the left now is the only thing that earns that column:
       which kind of thing you are after. */

    /* --- what was typed, and what matches it ------------------------------- */
    if (menu_qn > 0) {
        int qx = mx + MENU_PAD, qw = MENU_W - MENU_PAD * 2;
        int qy = my + MENU_PAD;

        face_text(qx + 12, qy + (MENU_ITEM - face_height(FACE_BODY)) / 2,
                  menu_q, t->text, FACE_BODY);

        /* A caret, steady rather than blinking. Nothing on this desktop
           blinks and adding the one thing that does would need a timer
           whose whole job is to make a line disappear. */
        int cx = qx + 14 + face_width(menu_q, FACE_BODY);
        fb_rect((u32)cx, (u32)(qy + 7), 1, (u32)(MENU_ITEM - 14), t->accent);

        if (t->look == LOOK_MODERN)
            fb_round_rect_aa(qx + 8, qy + MENU_ITEM - 1, qw - 16, 1, 0,
                             t->stroke, 130);
        else
            fb_bevel_thin(qx, qy + MENU_ITEM - 1, qw, 1,
                          t->edge_shadow, t->edge_hi);

        const mitem_t *hit[MENU_HITS];
        int n = menu_hits(hit, MENU_HITS);

        if (n == 0) {
            face_text(qx + 12,
                      qy + MENU_ITEM
                      + (MENU_ITEM - face_height(FACE_BODY)) / 2,
                      "nothing by that name", t->text_mute, FACE_BODY);
            return;
        }

        for (int j = 0; j < n; j++) {
            int iy = qy + (j + 1) * MENU_ITEM;
            bool on = item_light(MENU_RIGHT + j) > ANIM_FULL / 2;

            if (t->look == LOOK_MODERN) {
                if (on) fb_round_rect_aa(qx, iy + 1, qw, MENU_ITEM - 2, 8,
                                         t->accent, 42);
                face_text(qx + 12,
                          iy + (MENU_ITEM - face_height(FACE_BODY)) / 2,
                          hit[j]->label, t->text, FACE_BODY);
            } else {
                if (on) fb_rect((u32)qx, (u32)iy, (u32)qw, MENU_ITEM,
                                t->accent);
                face_text(qx + 8,
                          iy + (MENU_ITEM - face_height(FACE_BODY)) / 2,
                          hit[j]->label, on ? t->accent_text : t->text,
                          FACE_BODY);
            }
        }
        return;
    }

    /* --- the kinds, down the left ------------------------------------------ */
    int rx = mx + MENU_PAD;
    int rw = MENU_RAIL - 8;

    for (int i = 0; i < MCAT_N; i++) {
        int iy = my + MENU_PAD + i * MENU_ITEM;
        bool here = (i == menu_cat);
        bool lit  = item_light(i) > ANIM_FULL / 2;

        if (t->look == LOOK_MODERN) {
            if (here)     fb_round_rect_aa(rx, iy + 1, rw, MENU_ITEM - 2, 8,
                                           t->accent, 58);
            else if (lit) fb_round_rect_aa(rx, iy + 1, rw, MENU_ITEM - 2, 8,
                                           t->text, 20);

            /* A short bar against the left edge of the one that is open,
               because a fill alone cannot be told apart from a hover and
               this column has one of each on it at once. */
            if (here)
                fb_round_rect_aa(rx + 2, iy + 9, 3, MENU_ITEM - 18, 1,
                                 t->accent, 255);

            face_text(rx + 14, iy + (MENU_ITEM - face_height(FACE_BODY)) / 2,
                      MCAT[i].name, here ? t->text : t->text_dim, FACE_BODY);

            /* How many are in it, set small and against the far edge. A
               category is worth choosing partly for how much is behind it,
               and that is one number the column has room for. */
            char n[4];
            kformat(n, sizeof(n), "%d", MCAT[i].n);
            face_text(rx + rw - 10 - face_width(n, FACE_SMALL),
                      iy + (MENU_ITEM - face_height(FACE_SMALL)) / 2,
                      n, t->text_mute, FACE_SMALL);
        } else {
            if (here || lit)
                fb_rect((u32)rx, (u32)iy, (u32)rw, MENU_ITEM,
                        here ? t->accent : t->edge_hi);
            face_text(rx + 8, iy + (MENU_ITEM - face_height(FACE_BODY)) / 2,
                      MCAT[i].name, here ? t->accent_text : t->text,
                      FACE_BODY);
        }
    }

    /* --- the rule between the columns -------------------------------------- */
    int sx = mx + MENU_PAD + MENU_RAIL - 4;
    if (t->look == LOOK_MODERN)
        fb_round_rect_aa(sx, my + MENU_PAD + 6, 1, h - MENU_PAD * 2 - 12, 0,
                         t->stroke, 110);
    else
        fb_bevel_thin(sx, my + MENU_PAD, 1, h - MENU_PAD * 2,
                      t->edge_shadow, t->edge_hi);

    /* --- the things of that kind, down the right --------------------------- */
    int ix = mx + MENU_PAD + MENU_RAIL + 4;
    int iw = MENU_PANE - MENU_PAD - 4;

    for (int j = 0; j < MCAT[menu_cat].n; j++) {
        int iy = my + MENU_PAD + j * MENU_ITEM;
        bool on = item_light(MENU_RIGHT + j) > ANIM_FULL / 2;

        if (t->look == LOOK_MODERN) {
            /* A rounded patch rather than a bar to the edges, so the panel
               keeps its margin and the highlight reads as a thing being
               pointed at rather than a row being filled in. */
            if (on)
                fb_round_rect_aa(ix, iy + 1, iw, MENU_ITEM - 2, 8,
                                 t->accent, 42);

            /* Full strength, all of them: dimming the ones the kernel
               handles made four working commands look unavailable. */
            face_text(ix + 12,
                      iy + (MENU_ITEM - face_height(FACE_BODY)) / 2,
                      MCAT[menu_cat].items[j].label, t->text, FACE_BODY);
        } else {
            if (on) fb_rect((u32)ix, (u32)iy, (u32)iw, MENU_ITEM, t->accent);
            face_text(ix + 8, iy + (MENU_ITEM - face_height(FACE_BODY)) / 2,
                      MCAT[menu_cat].items[j].label,
                      on ? t->accent_text : t->text, FACE_BODY);
        }
    }
}

/* How strongly a row of the desktop menu should be lit. The same two
   variables and the same rise as the launcher, kept apart from it because
   the two can never be open at once but can both be halfway through
   fading when the other opens. */
static u32 ctx_light(int i) {
    u32 p = phase_of(ctx_hover_since, HOVER_MS);
    if (i == ctx_hover) return p;
    if (i == ctx_left)  return ANIM_FULL - p;
    return 0;
}

static void draw_ctx(void) {
    if (!ctx_open) return;
    const theme_t *t = theme();

    u32 p = phase_of(ctx_since, MENU_MS);
    int rise = (int)(((ANIM_FULL - p) * 6) / ANIM_FULL);
    int mx = ctx_x, my = ctx_y + rise;

    if (t->look == LOOK_MODERN) {
        if (t->shadows) fb_shadow(mx, my, CTX_W, CTX_H, 14, 12);
        fb_round_rect_aa(mx, my, CTX_W, CTX_H, 14, t->overlay, 250);
        fb_round_rect_aa(mx, my, CTX_W, CTX_H, 14, t->stroke, 85);
        fb_round_rect_aa(mx + 1, my + 1, CTX_W - 2, CTX_H - 2, 13,
                         t->overlay, 252);
    } else {
        fb_rect((u32)mx, (u32)my, CTX_W, CTX_H, t->surface);
        raised(mx, my, CTX_W, CTX_H);
    }

    int ix = mx + CTX_PAD;
    int iw = CTX_W - CTX_PAD * 2;

    for (int i = 0; i < CTX_N; i++) {
        int iy = my + CTX_PAD + i * CTX_ITEM;
        bool on = ctx_light(i) > ANIM_FULL / 2;

        if (t->look == LOOK_MODERN) {
            if (on) fb_round_rect_aa(ix, iy + 1, iw, CTX_ITEM - 2, 7,
                                     t->accent, 42);
            face_text(ix + 12, iy + (CTX_ITEM - face_height(FACE_BODY)) / 2,
                      CTX[i].label, t->text, FACE_BODY);
        } else {
            if (on) fb_rect((u32)ix, (u32)iy, (u32)iw, CTX_ITEM, t->accent);
            face_text(ix + 8, iy + (CTX_ITEM - face_height(FACE_BODY)) / 2,
                      CTX[i].label, on ? t->accent_text : t->text, FACE_BODY);
        }

        /* The rule between what opens something and what happens to what
           is already open. */
        if (i == 4 && t->look == LOOK_MODERN)
            fb_round_rect_aa(ix + 8, iy - 1, iw - 16, 1, 0, t->stroke, 120);
        else if (i == 4)
            fb_rect((u32)ix, (u32)iy, (u32)iw, 1, t->edge_shadow);
    }
}

/* The outline of where a dragged window would land. Drawn as a frame
   rather than a filled rectangle so what is underneath stays readable. */
static void draw_snap_preview(void) {
    if (snap_preview == SNAP_NONE) return;
    const theme_t *t = theme();

    int x, y, cw, ch;
    snap_rect(snap_preview, &x, &y, &cw, &ch);
    int ow = cw + WM_BORDER * 2, oh = ch + WM_TOP + WM_BORDER;

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
    int ow = resize_cw + WM_BORDER * 2, oh = resize_ch + WM_TOP + WM_BORDER;
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



static void clock_text(char *out, u32 cap) {
    if (rtc_present()) {
        rtc_format_short(out, cap);

        /* Twelve hour is the same string with the hour folded and two
           letters after it, rather than a second formatter in rtc.c: the
           clock is the desktop's, and the only thing the desktop wants
           from the hardware is the hour it actually is. */
        if (!theme()->clock_24 && cap >= 9
            && out[0] >= '0' && out[0] <= '9' && out[2] == ':') {
            int hh = (out[0] - '0') * 10 + (out[1] - '0');
            bool pm = hh >= 12;
            int h12 = hh % 12;
            if (!h12) h12 = 12;
            out[0] = (char)('0' + h12 / 10);
            out[1] = (char)('0' + h12 % 10);
            out[5] = ' ';
            out[6] = pm ? 'p' : 'a';
            out[7] = 'm';
            out[8] = 0;
            /* A leading nought on a twelve hour clock is the one thing
               nobody writes. */
            if (out[0] == '0') {
                for (int i = 0; i < 8; i++) out[i] = out[i + 1];
            }
        }
        return;
    }

    /* A machine with no usable CMOS clock counts from boot instead, which
       is what this had before there was a clock to read. */
    u32 secs = (u32)(timer_ticks() / timer_hz());
    kformat(out, cap, "up %d:%02d", secs / 60, secs % 60);
}

/* The right hand end of the panel: the clock against the edge, and the
   speaker to the left of it. Worked out in one place so that what is drawn
   and what a click lands on cannot drift apart. */
/* How much of the right hand end the clock takes, which is nothing at all
   when it is switched off: the tray then moves over into the room it was
   using rather than leaving a hole where it used to be. */
static int clock_slot_w(void) {
    if (!theme()->dock_clock) return 0;
    char clock[24];
    clock_text(clock, sizeof(clock));
    return face_width(clock, theme()->look == LOOK_MODERN ? FACE_HEAD
                                                          : FACE_BODY);
}

static int taskbar_volume_x(void) {
    /* Measured in from the dock's own right edge rather than the screen's,
       because the dock no longer reaches it. */
    return dock_x() + dock_w() - 20 - clock_slot_w() - 16 - VOL_W;
}

static bool on_volume_button(int mx, int my) {
    int y = taskbar_y();
    if (my < y + 5 || my >= y + TASKBAR_H - 5) return false;
    int x = taskbar_volume_x();
    return mx >= x && mx < x + VOL_W;
}

/* --- the glyphs in the tray ----------------------------------------------
 *
 * Placed pixel by pixel rather than assembled out of rectangles, which is
 * how the pointer a few hundred lines down has always been drawn and for
 * the same reason. Both of these were built out of shapes and both came
 * out wrong in the same way: the sound leaving a speaker was two straight
 * bars, which beside anything read as tally marks, and the network was a
 * rounded box with a nub above and another below, which at sixteen pixels
 * read as an insect. Arcs computed from a radius were no better -- at this
 * size the rounding decides the shape, and three concentric ones came out
 * as a lattice.
 *
 * Sixteen pixels is not a drawing of a thing. It is an arrangement of two
 * hundred and fifty six cells, and the only reliable way to arrange them
 * is to arrange them.
 *
 * The digit in a cell says which part of the glyph it belongs to, so one
 * bitmap serves the quiet speaker and the loud one, and one serves all
 * five strengths of a signal, instead of five bitmaps that could drift.
 */
#define GLYPH 16

/* Which parts to draw, as bits: part '1' is bit zero. */
static void glyph(int x, int y, const char *bits, const u32 *ink, int parts) {
    for (int j = 0; j < GLYPH; j++)
        for (int i = 0; i < GLYPH; i++) {
            char c = bits[j * GLYPH + i];
            if (c == ' ') continue;
            int k = c - '1';
            if (!((parts >> k) & 1)) continue;
            fb_put((u32)(x + i), (u32)(y + j), ink[k]);
        }
}

/* 1 the cone, 2 the near wave, 3 the far one. */
static const char G_SPEAKER[] =
    "                "
    "                "
    "       1        "
    "      11    3   "
    "     111     3  "
    "    1111 2    3 "
    "11111111  2   3 "
    "11111111  2   3 "
    "11111111  2   3 "
    "11111111  2   3 "
    "    1111 2    3 "
    "     111     3  "
    "      11    3   "
    "       1        "
    "                "
    "                ";

/* The same cone with a cross where the waves were, rather than the waves
   drawn faintly: silence is not a quiet sound. */
static const char G_MUTE[] =
    "                "
    "                "
    "       1        "
    "      11        "
    "     111        "
    "    1111 44  44 "
    "11111111  4444  "
    "11111111   44   "
    "11111111   44   "
    "11111111  4444  "
    "    1111 44  44 "
    "     111        "
    "      11        "
    "       1        "
    "                "
    "                ";

_Static_assert(sizeof(G_SPEAKER) == GLYPH * GLYPH + 1, "the speaker is not 16 by 16");
_Static_assert(sizeof(G_MUTE) == GLYPH * GLYPH + 1, "the mute speaker is not 16 by 16");

static void draw_speaker(int x, int y, u32 fg, int level) {
    const u32 ink[4] = { fg, fg, fg, fg };
    if (level <= 0) { glyph(x, y, G_MUTE, ink, 0x9); return; }
    glyph(x, y, G_SPEAKER, ink, level > 45 ? 0x7 : 0x3);
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

    if (t->look != LOOK_MODERN)
        fb_rect((u32)px, (u32)py, VOLPOP_W, VOLPOP_H, t->surface);
    if (t->look == LOOK_MODERN) {
        if (t->shadows) fb_shadow(px, py, VOLPOP_W, VOLPOP_H, 10, 7);
        fb_round_rect_aa(px, py, VOLPOP_W, VOLPOP_H, 10, t->overlay, 248);
        fb_round_rect_aa(px, py, VOLPOP_W, VOLPOP_H, 10, t->stroke, 85);
        fb_round_rect_aa(px + 1, py + 1, VOLPOP_W - 2, VOLPOP_H - 2, 9, t->overlay, 252);
    } else {
        raised(px, py, VOLPOP_W, VOLPOP_H);
    }

    int level = theme()->volume;
    int on = tw * level / 100;

    if (t->look == LOOK_MODERN) {
        fb_round_rect_aa(tx, ty, tw, 6, 3, t->well, 255);
        if (on > 0) fb_round_rect_aa(tx, ty, on, 6, 3, t->accent, 255);
        /* A disc, and a ring of the surface around it, so the knob stays
           visible wherever along the track it happens to sit. */
        fb_round_rect_aa(tx + on - 8, ty - 5, 16, 16, 8, t->surface, 255);
        fb_round_rect_aa(tx + on - 6, ty - 3, 12, 12, 6, t->accent, 255);
    } else {
        fb_rect((u32)tx, (u32)ty, (u32)tw, 6, t->well);
        if (on > 0) fb_rect((u32)tx, (u32)ty, (u32)on, 6, t->accent);
        sunken(tx, ty, tw, 6);
        button_face(tx + on - 5, ty - 5, 11, 16, false, false);
    }

    char num[8];
    kformat(num, sizeof(num), "%d", level);
    face_text(tx + tw + 14, py + (VOLPOP_H - face_height(FACE_BODY)) / 2,
              num, t->text, FACE_BODY);
}

/* --- the network, left of the speaker -------------------------------------
 *
 * The icon says three things apart: no link, a link with no address, and a
 * link that can reach somewhere. The middle one is the state people actually
 * get stuck in and the one an icon with two states cannot show, which is why
 * a cable plugged into a router with no DHCP on it looks, on most machines,
 * exactly like no cable at all.
 */
static int taskbar_net_x(void) {
    return taskbar_volume_x() - NET_W - 6;
}

static bool on_net_button(int mx, int my) {
    int y = taskbar_y();
    if (my < y + 5 || my >= y + TASKBAR_H - 5) return false;
    int x = taskbar_net_x();
    return mx >= x && mx < x + NET_W;
}

/* Three arcs over a dot. Flat across the top with the ends dropping away
   quickly, rather than an even curve: three even curves three rows apart
   have each one's falling end level with the next one's rise, and what
   that draws is a lattice.
 *
   The arcs above the strength are drawn faintly rather than left out, so
   the icon keeps its size and shape whatever it is saying. One that
   changes outline as the signal moves is one the eye has to find again
   every time it does. */
static const char G_SIGNAL[] =
    "                "
    "                "
    "   4444444444   "
    " 44          44 "
    "                "
    "     333333     "
    "   33      33   "
    "                "
    "      2222      "
    "    22    22    "
    "                "
    "       11       "
    "      1111      "
    "      1111      "
    "       11       "
    "                ";

/* A plug on the end of a lead, for a wired connection, because arcs would
   be a lie about something that has no signal strength. */
static const char G_PLUG[] =
    "                "
    "                "
    "       11       "
    "       11       "
    "  111111111111  "
    "  1          1  "
    "  1          1  "
    "  1          1  "
    "  1          1  "
    "  111111111111  "
    "    11 11 11    "
    "    11 11 11    "
    "    11 11 11    "
    "                "
    "                "
    "                ";

_Static_assert(sizeof(G_SIGNAL) == GLYPH * GLYPH + 1, "the signal is not 16 by 16");
_Static_assert(sizeof(G_PLUG) == GLYPH * GLYPH + 1, "the plug is not 16 by 16");

static void draw_signal(int x, int y, u32 on, u32 off, int bars) {
    const u32 ink[4] = { bars >= 1 ? on : off, bars >= 2 ? on : off,
                         bars >= 3 ? on : off, bars >= 4 ? on : off };
    glyph(x, y, G_SIGNAL, ink, 0xF);
}

static void draw_wired(int x, int y, u32 fg) {
    const u32 ink[4] = { fg, fg, fg, fg };
    glyph(x, y, G_PLUG, ink, 0x1);
}

static void net_panel_rect(int *px, int *py) {
    int x = taskbar_net_x() + NET_W / 2 - NETPOP_W / 2;
    if (x < TASKBAR_GAP) x = TASKBAR_GAP;
    if (x + NETPOP_W > (int)fb_width() - TASKBAR_GAP)
        x = (int)fb_width() - TASKBAR_GAP - NETPOP_W;
    *px = x;
    *py = taskbar_y() - NETPOP_H - 8;
}

/* The button in the panel that asks for an address. */
static void dhcp_button_rect(int *bx, int *by, int *bw, int *bh) {
    int px, py;
    net_panel_rect(&px, &py);
    *bw = NETPOP_W - 28;
    *bh = 26;
    *bx = px + 14;
    *by = py + NETPOP_H - 14 - *bh;
}

static void draw_net_panel(void) {
    if (!net_open) return;
    const theme_t *t = theme();

    int px, py;
    net_panel_rect(&px, &py);

    if (t->look != LOOK_MODERN)
        fb_rect((u32)px, (u32)py, NETPOP_W, NETPOP_H, t->surface);
    if (t->look == LOOK_MODERN) {
        if (t->shadows) fb_shadow(px, py, NETPOP_W, NETPOP_H, 10, 7);
        fb_round_rect_aa(px, py, NETPOP_W, NETPOP_H, 10, t->overlay, 248);
        fb_round_rect_aa(px, py, NETPOP_W, NETPOP_H, 10, t->stroke, 85);
        fb_round_rect_aa(px + 1, py + 1, NETPOP_W - 2, NETPOP_H - 2, 9, t->overlay, 252);
    } else {
        raised(px, py, NETPOP_W, NETPOP_H);
    }

    int line = face_height(FACE_BODY) + 6;
    int ty = py + 12;
    int tx = px + 14;

    /* net_up means the card is up, which is not the same as being able to
       reach anything: that needs an address, and the whole point of this
       panel is telling those two apart. */
    bool card = netdev_name()[0] != 0 && netdev_up();
    bool addressed = net_ip() != 0;
    char buf[64];

    /* What the wire is. */
    if (card) {
        kformat(buf, sizeof(buf), "%s", netdev_name());
        face_text(tx, ty, buf, t->text, FACE_BODY);
    } else {
        /* "No card" and "a card with no driver for it" look the same from
           here and are not the same problem, so say which it is. */
        u16 uv = 0, ud = 0;
        if (netdev_undriven(&uv, &ud)) {
            kformat(buf, sizeof(buf), "%04x:%04x, no driver", uv, ud);
            face_text(tx, ty, buf, t->text_dim, FACE_BODY);
        } else {
            face_text(tx, ty, "no wired card", t->text_dim, FACE_BODY);
        }
    }
    ty += line;

    /* And whether it has got anywhere. An address is the thing that decides
       whether anything works, so it is the line in the ordinary colour. */
    if (addressed) {
        char ip[20];
        net_format_ip(net_ip(), ip);
        kformat(buf, sizeof(buf), "address %s", ip);
        face_text(tx, ty, buf, t->text, FACE_BODY);
        ty += line;

        net_format_ip(net_gateway(), ip);
        kformat(buf, sizeof(buf), "router  %s", ip);
        face_text(tx, ty, buf, t->text_dim, FACE_BODY);
    } else {
        face_text(tx, ty, net_dhcp_busy() ? "asking for an address"
                                       : "no address", t->text_dim, FACE_BODY);
        ty += line;
        face_text(tx, ty, "nothing can be reached without one",
                  t->text_dim, FACE_SMALL);
    }
    ty += line + 4;

    /* And the wireless situation, which on most machines is the reason
       somebody opened this panel. */
    face_text(tx, ty, wifi_describe(), t->text_dim, FACE_SMALL);
    ty += face_height(FACE_SMALL) + 4;
    if (wifi_state() != WIFI_NONE) {
        kformat(buf, sizeof(buf), "%s %04x:%04x", wifi_maker(),
                wifi_vendor(), wifi_device());
        face_text(tx, ty, buf, t->text_dim, FACE_SMALL);
    }

    int bx, by, bw, bh;
    dhcp_button_rect(&bx, &by, &bw, &bh);
    bool over = last_mx >= bx && last_mx < bx + bw
             && last_my >= by && last_my < by + bh;
    button_face(bx, by, bw, bh, false, over && !net_dhcp_busy());

    const char *label = net_dhcp_busy() ? "asking..." : "ask for an address";
    u32 label_colour = net_dhcp_busy() ? t->text_dim : t->text;
    face_text(bx + bw / 2 - face_width(label, FACE_BODY) / 2,
              by + (bh - face_height(FACE_BODY)) / 2,
              label, label_colour, FACE_BODY);
}

/* Asking takes seconds and happens in a task of its own, because the
   compositor stopping for the length of a DHCP exchange is the desktop
   freezing every time somebody plugs a cable in. The task and the flag that
   says one is running belong to net.c, because the machine also asks once by
   itself at startup and the two must not overlap. */
static void start_dhcp(void) {
    net_dhcp_start();
    need_frame();
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
    int W = (int)fb_width();

    /* Flush to the bottom edge and the full width of it, because a panel is
       part of the machine rather than a card lying on the desktop. A
       floating rounded one was tried here once and read as an app.
     *
       What changes with the modern look is not the shape but the material:
       the same full width bar, tinted rather than filled, so the wallpaper
       is still there underneath it and the bar belongs to the screen rather
       than sitting on top of it. A hairline along the top instead of a
       bevel, because a bevel on a translucent surface is a bevel on
       nothing. */
    if (t->look == LOOK_MODERN) {
        int dx = dock_x(), dw = dock_w();

        /* A shadow first, so the bar sits above the wallpaper rather than
           being a hole cut in it. */
        if (t->shadows) fb_shadow(dx, y, dw, TASKBAR_H, DOCK_R, 10);

        /* Glass, twice: once for the tint and once for a hairline round the
           edge. A single pass leaves the corners looking chewed, because
           the antialiasing on the outside of a rounded rectangle is what
           tells the eye where the edge is. */
        fb_round_rect_aa(dx, y, dw, TASKBAR_H, DOCK_R, t->glass, 208);
        fb_round_rect_aa(dx, y, dw, TASKBAR_H, DOCK_R, t->stroke, 70);
        fb_round_rect_aa(dx + 1, y + 1, dw - 2, TASKBAR_H - 2, DOCK_R - 1,
                         t->glass, 232);

        /* And a light along the top edge, which is what every piece of
           glass in the world has and no flat rectangle does. */
        fb_sheen(dx, y, dw, TASKBAR_H, DOCK_R, 44);
    } else {
        fb_rect(0, (u32)y, (u32)W, TASKBAR_H, t->surface);
        fb_bevel_thin(0, y, W, TASKBAR_H + 2, t->edge_hi, t->edge_hi);
        fb_rect(0, (u32)y, (u32)W, 1, t->edge_hi);
    }

    /* --- the launcher ----------------------------------------------------- */
    /* The same arithmetic the hit test uses, rather than a number that
       happens to look right: a badge drawn somewhere its own click handler
       does not expect is a button that works everywhere except where it
       is. */
    int bx = dock_x() + 16, bh = TASKBAR_H - 14, by = y + 7;
    if (t->dock_brand) {
        bool badge_hot = menu_open || (last_my >= by && last_my < by + bh
                                       && last_mx >= bx
                                       && last_mx < bx + TASKBAR_BADGE_W);
        button_face(bx, by, TASKBAR_BADGE_W, bh, menu_open, badge_hot);
        face_text(bx + 12 + (menu_open ? 1 : 0),
                  by + (bh - face_height(FACE_HEAD_BOLD)) / 2
                  + (menu_open ? 1 : 0),
                  "zelr", t->accent, FACE_HEAD_BOLD);
    }

    /* --- the field that replaced the icons ---------------------------------
     *
     * Centred in the bar rather than beside the brand, because the middle
     * of a wide bar is the one place nothing else wants and the eye goes
     * there first. Sunk very slightly into the glass: the same trick a
     * search field has had since before any of this, which is the only
     * shape a person reads as "type here" without a label.
     */
    if (t->look == LOOK_MODERN && t->dock_search) {
        int fx = dock_find_x();
        int fy = y + (TASKBAR_H - DOCK_FIND_H) / 2;
        bool fhot = last_my >= fy && last_my < fy + DOCK_FIND_H
                 && last_mx >= fx && last_mx < fx + DOCK_FIND_W;

        fb_round_rect_aa(fx, fy, DOCK_FIND_W, DOCK_FIND_H, DOCK_FIND_H / 2,
                         t->text, (fhot || menu_open) ? 26 : 16);
        fb_round_rect_aa(fx, fy, DOCK_FIND_W, DOCK_FIND_H, DOCK_FIND_H / 2,
                         menu_open ? t->accent : t->stroke,
                         menu_open ? 150 : (fhot ? 90 : 55));

        /* A ring and a handle, which is a magnifier at any size that has
           room for a ring and a handle. */
        u32 ink = fhot ? t->text : t->text_dim;
        int gx = fx + 16, gy = fy + DOCK_FIND_H / 2;

        /* A ring, from one eighth of a circle mirrored eight ways. There is
           no sine here to ask and none needed: a glyph this small is a
           handful of pixels either way. */
        static const int RING[5][2] = { {5,0}, {4,2}, {4,3}, {3,4}, {0,5} };
        for (int k = 0; k < 5; k++) {
            int a = RING[k][0], b = RING[k][1];
            fb_put((u32)(gx + a), (u32)(gy + b), ink);
            fb_put((u32)(gx - a), (u32)(gy + b), ink);
            fb_put((u32)(gx + a), (u32)(gy - b), ink);
            fb_put((u32)(gx - a), (u32)(gy - b), ink);
            fb_put((u32)(gx + b), (u32)(gy + a), ink);
            fb_put((u32)(gx - b), (u32)(gy + a), ink);
            fb_put((u32)(gx + b), (u32)(gy - a), ink);
            fb_put((u32)(gx - b), (u32)(gy - a), ink);
        }
        /* and its handle */
        for (int k = 0; k < 4; k++) {
            fb_put((u32)(gx + 4 + k), (u32)(gy + 4 + k), ink);
            fb_put((u32)(gx + 5 + k), (u32)(gy + 4 + k), ink);
        }

        /* What has been typed, where it was typed. The panel above shows
           the matches; this shows the query, because the field is where
           the pointer was when the typing started and is where the eye
           is. */
        int ty = fy + (DOCK_FIND_H - face_height(FACE_BODY)) / 2;
        if (menu_qn > 0) {
            face_text(fx + 30, ty, menu_q, t->text, FACE_BODY);
            fb_rect((u32)(fx + 32 + face_width(menu_q, FACE_BODY)),
                    (u32)(fy + 6), 1, (u32)(DOCK_FIND_H - 12), t->accent);
        } else if (menu_open) {
            fb_rect((u32)(fx + 32), (u32)(fy + 6), 1,
                    (u32)(DOCK_FIND_H - 12), t->accent);
            face_text(fx + 38, ty, "type to find", t->text_mute, FACE_BODY);
        } else {
            face_text(fx + 30, ty, "Search", t->text_dim, FACE_BODY);
        }
    }

    /* --- one button a window ----------------------------------------------
     *
     * Bevelled and pressed in for whichever is in front, because that is the
     * same language the rest of the desktop is in: the window you are using
     * is the button that is down. */
    int x = taskbar_chips_x();
    int room_end = taskbar_chips_end();
    for (int i = 0; i < nwin; i++) {
        window_t *w = stack[i];
        bool focused = (i == nwin - 1) && !w->minimized;

        int tw = face_width(w->title, FACE_BODY) + 22;
        if (tw > 160) tw = 160;
        if (x + tw > room_end) break;

        bool over = last_mx >= x && last_mx < x + tw
                 && last_my >= by && last_my < by + bh;

        if (t->look == LOOK_MODERN) {
            /* A pill, filled in the accent for whichever window is in
               front and in nothing at all for the rest until the pointer
               arrives. The bar is quiet when nothing is happening on it,
               which is the whole difference between a dock and a toolbar. */
            if (focused)
                fb_round_rect_aa(x, by, tw, bh, bh / 2, t->accent, 46);
            else if (over)
                fb_round_rect_aa(x, by, tw, bh, bh / 2, t->text, 22);

            /* And a short line under the one in front, which is how a dock
               says "this one" without a border. */
            if (focused)
                fb_round_rect_aa(x + tw / 2 - 7, by + bh - 3, 14, 3, 1,
                                 t->accent, 235);

            face_text(x + 12, by + (bh - face_height(FACE_BODY)) / 2,
                      w->title, w->minimized ? t->text_dim : t->text,
                      FACE_BODY);
        } else {
            button_face(x, by, tw, bh, focused, over);
            int off = focused ? 1 : 0;
            face_text(x + 8 + off, by + (bh - face_height(FACE_BODY)) / 2 + off,
                      w->title, w->minimized ? t->text_dim : t->text, FACE_BODY);
        }
        x += tw + 6;
    }

    /* --- the tray ----------------------------------------------------------
     *
     * Sunk, so the things in it read as indicators rather than as more
     * buttons: a clock is not something to press and should not look like
     * it. */
    int tray_x = taskbar_net_x() - 6;
    int tray_w = W - tray_x - 2;
    if (t->look == LOOK_MODERN) {
        /* Nothing at all. A sunk well says "these are indicators rather
           than buttons", which is true and which the bar no longer needs
           said: with every button on it invisible until the pointer is over
           it, a clock that is simply text is already not a button. Drawing
           the well anyway leaves one bevelled box on an otherwise flat bar,
           which is the only thing on it that looks out of place. */
    } else {
        fb_rect((u32)tray_x, (u32)by, (u32)tray_w, (u32)bh, t->surface);
        sunken(tray_x, by, tray_w, bh);
    }

    char clock[24];
    clock_text(clock, sizeof(clock));
    if (!t->dock_clock) {
        /* Nothing, and nothing in its place. */
    } else if (t->look == LOOK_MODERN) {
        /* Set larger than everything else on the bar, because it is the one
           thing on it that gets read rather than clicked. */
        face_text(dock_x() + dock_w() - 20 - face_width(clock, FACE_HEAD),
                  y + (TASKBAR_H - face_height(FACE_HEAD)) / 2, clock,
                  t->text, FACE_HEAD);
    } else {
        face_text(W - 8 - face_width(clock, FACE_BODY),
                  y + (TASKBAR_H - face_height(FACE_BODY)) / 2, clock, t->text,
                  FACE_BODY);
    }

    /* Both glyphs are sixteen pixels square and both are centred in the
       bar, rather than each carrying an offset that happened to look right
       at whatever height the bar was when it was written. */
    int gy = y + (TASKBAR_H - 16) / 2;

    {
        int vx = taskbar_volume_x();
        bool vhot = volume_open || on_volume_button(last_mx, last_my);
        if (vhot) {
            if (t->look == LOOK_MODERN)
                fb_round_rect_aa(vx - 3, by, VOL_W + 6, bh, bh / 2,
                                 t->text, volume_open ? 34 : 22);
            else
                fb_rect((u32)(vx - 1), (u32)(by + 2), VOL_W + 2,
                        (u32)(bh - 4), gfx_mix(t->surface, t->edge_hi, 70));
        }
        draw_speaker(vx + (VOL_W - 16) / 2, gy,
                     t->volume ? t->text : t->text_dim, t->volume);
    }

    {
        int nx = taskbar_net_x();
        bool nhot = net_open || on_net_button(last_mx, last_my);
        if (nhot) {
            if (t->look == LOOK_MODERN)
                fb_round_rect_aa(nx - 3, by, NET_W + 6, bh, bh / 2,
                                 t->text, net_open ? 34 : 22);
            else
                fb_rect((u32)(nx - 1), (u32)(by + 2), NET_W + 2,
                        (u32)(bh - 4), gfx_mix(t->surface, t->edge_hi, 70));
        }

        /* Three states, not two. A link with no address is the one people
           get stuck in, and an icon that cannot show it sends them looking
           at the cable. */
        bool link = netdev_up();
        bool reachable = net_ip() != 0;
        u32 fg = reachable ? t->text : link ? t->text_dim : t->text_mute;

        if (link) draw_wired(nx + (NET_W - 16) / 2, gy, fg);
        else      draw_signal(nx + (NET_W - 16) / 2, gy, fg, t->text_mute, 0);
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
    if (cover >= 0) {
        fb_rect(0, 0, fb_width(), fb_height(), theme()->desktop);
    } else {
        draw_wallpaper();
        draw_desk_icons();      /* on the wallpaper, under every window */
        draw_band();            /* and the band over the icons it catches */
    }

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

        blit_surface(px, cw, ch, w->x + WM_BORDER, w->y + WM_TOP);
    }

    for (int i = 0; i < cover; i++) stack[i]->dirty = false;

    draw_snap_preview();
    draw_resize_preview();
    draw_taskbar();
    draw_volume_panel();
    draw_net_panel();
    draw_menu();
    draw_ctx();
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

static void menu_run(const mitem_t *it) {
    menu_open = false;
    menu_qn = 0;
    menu_q[0] = 0;
    need_frame();

    if (it->program) { launch(it->program); return; }

    if (!strcmp(it->label, "System info")) app_about();
    else if (!strcmp(it->label, "Close all")) { while (nwin > 0) wm_close(stack[nwin - 1]); }
    else if (!strcmp(it->label, "Leave desktop")) running = false;
    else if (!strcmp(it->label, "Shut down")) {
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

/* An index from either column, or from the list of matches when something
   has been typed. One from the left opens that kind and leaves the panel
   up, which is the whole point of the left column; anything else does the
   thing and puts the panel away. */
static void menu_choose(int i) {
    if (menu_qn > 0) {
        const mitem_t *hit[MENU_HITS];
        int n = menu_hits(hit, MENU_HITS);
        int j = i - MENU_RIGHT;
        if (j >= 0 && j < n) menu_run(hit[j]);
        return;
    }

    if (i >= 0 && i < MCAT_N) {
        if (i != menu_cat) { menu_cat = i; need_frame(); }
        return;
    }

    int j = i - MENU_RIGHT;
    if (j < 0 || j >= MCAT[menu_cat].n) return;
    menu_run(&MCAT[menu_cat].items[j]);
}

/* A key, while the launcher is up. True when the launcher took it, which
   is what keeps it out of whatever window is behind. */
static bool menu_key(int key) {
    int c = KEY_CODE(key);

    if (c == '\n' || c == '\r') {
        /* Whatever is under the pointer, or the first match if it is
           nowhere near the panel, which is where a hand that has just
           finished typing usually leaves it. */
        int want = (menu_hover >= MENU_RIGHT) ? menu_hover : MENU_RIGHT;
        menu_choose(want);
        return true;
    }

    if (c == '\b' || c == 127) {
        if (menu_qn > 0) {
            menu_q[--menu_qn] = 0;
            menu_hover = menu_left = -1;
            need_frame();
        }
        return true;
    }

    if (c >= 32 && c < 127 && menu_qn < MENU_QMAX) {
        menu_q[menu_qn++] = (char)c;
        menu_q[menu_qn] = 0;
        menu_hover = menu_left = -1;
        need_frame();
        return true;
    }
    return false;
}


/* Which row of which column, or -1. The two columns are not the same
   length, so a row level with the fourth kind is nothing at all when the
   kind that is open has three things in it. */
static int menu_item_at(int mx, int my) {
    if (!menu_open) return -1;
    int h = menu_h(), top = menu_top();
    if (mx < menu_x || mx >= menu_x + MENU_W) return -1;
    if (my < top + MENU_PAD || my >= top + h - MENU_PAD) return -1;

    int row = (my - top - MENU_PAD) / MENU_ITEM;

    /* While something has been typed the panel is one column, and its top
       row is the query rather than a result. */
    if (menu_qn > 0) {
        const mitem_t *hit[MENU_HITS];
        int n = menu_hits(hit, MENU_HITS);
        if (row < 1 || row - 1 >= n) return -1;
        return MENU_RIGHT + row - 1;
    }

    int split = menu_x + MENU_PAD + MENU_RAIL - 4;

    if (mx < split)
        return (row >= 0 && row < MCAT_N) ? row : -1;
    return (row >= 0 && row < MCAT[menu_cat].n) ? MENU_RIGHT + row : -1;
}

static int ctx_item_at(int mx, int my) {
    if (!ctx_open) return -1;
    if (mx < ctx_x || mx >= ctx_x + CTX_W) return -1;
    if (my < ctx_y + CTX_PAD || my >= ctx_y + CTX_H - CTX_PAD) return -1;
    int i = (my - ctx_y - CTX_PAD) / CTX_ITEM;
    return (i >= 0 && i < CTX_N) ? i : -1;
}

static void open_ctx_at(int x, int y) {
    if (x + CTX_W > (int)fb_width()) x = (int)fb_width() - CTX_W - 4;
    if (y + CTX_H > panel_rest_y()) y = panel_rest_y() - CTX_H - 4;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    ctx_x = x; ctx_y = y;
    ctx_open = true;
    menu_open = false;
    ctx_hover = -1;
    ctx_left = -1;
    ctx_since = timer_ticks();
    ctx_hover_since = 0;
    need_frame();
}

static void ctx_choose(int i) {
    ctx_open = false;
    need_frame();
    if (i < 0 || i >= CTX_N) return;

    if (CTX[i].program) { launch(CTX[i].program); return; }

    if (!strcmp(CTX[i].label, "Select all")) {
        desk_sel = (DESK_N >= 32) ? 0xFFFFFFFFu : (1u << DESK_N) - 1u;
    } else if (!strcmp(CTX[i].label, "Close all windows")) {
        while (nwin > 0) wm_close(stack[nwin - 1]);
    } else if (!strcmp(CTX[i].label, "System info")) {
        app_about();
    }
}

static void open_menu_at(int x, int y) {
    int h = menu_full_h();
    if (x + MENU_W > (int)fb_width()) x = (int)fb_width() - MENU_W - 4;
    if (y + h > panel_rest_y()) y = panel_rest_y() - h - 4;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    menu_x = x; menu_y = y;
    menu_open = true;
    ctx_open = false;
    menu_cat = 0;
    menu_qn = 0;
    menu_q[0] = 0;
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
        *on_title = (my >= w->y + WM_BORDER && my < w->y + WM_TOP);
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
    if (my < y + 7 || my >= y + TASKBAR_H - 7) return -1;

    int x = taskbar_chips_x();
    for (int i = 0; i < nwin; i++) {
        /* Measured in the face it is drawn in. The window in front carries
           a heavier title, so measuring every chip in the regular weight
           puts the edge of the widest one in the wrong place and a click
           near it lands on the neighbour. */
        bool focused = (i == nwin - 1) && !stack[i]->minimized;
        int face = focused ? FACE_BODY_BOLD : FACE_BODY;
        int tw = face_width(stack[i]->title, face) + 22;
        if (tw > 160) tw = 160;
        if (x + tw > taskbar_chips_end()) break;
        if (mx >= x && mx < x + tw) return i;
        x += tw + 6;
    }
    return -1;
}

/* Where the badge is drawn, not near it. This measured from TASKBAR_GAP,
   which is the gap under the dock and had nothing to do with the left of
   it: the button worked over a stretch ten pixels left of where it was
   painted, which is the sort of thing that reads as a slow machine. */
static bool on_taskbar_badge(int mx, int my) {
    if (!theme()->dock_brand) return false;
    int y = taskbar_y();
    return my >= y + 7 && my < y + TASKBAR_H - 7
           && mx >= dock_x() + 16
           && mx < dock_x() + 16 + TASKBAR_BADGE_W;
}

/* And the field in the middle, which says it opens the launcher. It did
   not: nothing tested for it, so a click on it landed on the stretch of
   bar that swallows clicks and the field was decoration. */
static bool on_dock_find(int mx, int my) {
    if (theme()->look != LOOK_MODERN || !theme()->dock_search) return false;
    int y = taskbar_y();
    int fy = y + (TASKBAR_H - DOCK_FIND_H) / 2;
    int fx = dock_find_x();
    return my >= fy && my < fy + DOCK_FIND_H
        && mx >= fx && mx < fx + DOCK_FIND_W;
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
        /* The pointer resting on a kind opens it. A click would work and
           would also mean two clicks to reach anything, which is one more
           than the list it replaced needed. */
        if (over >= 0 && over < MCAT_N && over != menu_cat) {
            menu_cat = over;
            need_frame();
        }

        if (right_now) {           /* nothing to pin to any more */
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

    if (ctx_open) {
        int over = ctx_item_at(mx, my);
        if (over != ctx_hover) {
            ctx_left = ctx_hover;
            ctx_hover = over;
            ctx_hover_since = timer_ticks();
            need_frame();
        }
        if (pressed_now || right_now) {
            if (over >= 0) { ctx_choose(over); return; }
            ctx_open = false;
            need_frame();
            return;
        }
    }

    if (volume_drag && (buttons & 1)) { volume_from_pointer(mx); return; }

    if (net_open && (pressed_now || right_now)) {
        int px, py;
        net_panel_rect(&px, &py);
        if (mx >= px && mx < px + NETPOP_W && my >= py && my < py + NETPOP_H) {
            int bx, by, bw, bh;
            dhcp_button_rect(&bx, &by, &bw, &bh);
            if (mx >= bx && mx < bx + bw && my >= by && my < by + bh)
                start_dhcp();
            return;
        }
        if (!on_net_button(mx, my)) {
            net_open = false;
            need_frame();
            return;
        }
    }

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

    /* A band being dragged out. Handled before anything that cares which
       button went down, because from here until it is let go the only
       thing that matters is where the pointer is. */
    if (band_on) {
        bool held = (band_button == 1) ? (buttons & 1) : (buttons & 2);
        if (held) {
            if (mx != band_bx || my != band_by) {
                band_bx = mx;
                band_by = my;
                band_select();
                need_frame();
            }
            return;
        }

        band_on = false;

        /* A press and a release in the same place is a click, and a click
           on the wallpaper is a menu: the left button asks what this
           machine can run and the right asks what can be done here. Four
           pixels of slack, because a hand on a real mouse moves one or two
           between pressing and letting go and a menu that fails to open
           for that is a menu that fails at random. */
        int dx = band_bx - band_ax, dy = band_by - band_ay;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx < 4 && dy < 4) {
            desk_sel = 0;
            if (band_button == 2) open_ctx_at(band_ax, band_ay);
            else                  open_menu_at(band_ax, band_ay);
        }
        need_frame();
        return;
    }

    if (released) {
        volume_drag = false;

        /* Hand the release to whoever was being drawn in, before dropping
           the capture: a program needs to know a stroke ended. */
        if (mouse_capture && mouse_capture->owned_by_user) {
            window_t *w = mouse_capture;
            wm_event_t ev = { WM_EV_MOUSE,
                              mx - (w->x + WM_BORDER),
                              my - (w->y + WM_TOP), 0, 0 };
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


    /* Once a drag starts inside a window's content it keeps receiving
       movement, even if the pointer strays outside. */
    if (mouse_capture) {
        window_t *w = mouse_capture;
        int lx = mx - (w->x + WM_BORDER), ly = my - (w->y + WM_TOP);
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
            else open_menu_at(dock_x(), panel_rest_y() - menu_full_h() - 8);
            return;
        }

        if (on_dock_find(mx, my)) {
            /* Centred under the field rather than against the left of the
               dock, because that is where the eye already is. */
            if (menu_open) { menu_open = false; need_frame(); }
            else open_menu_at(dock_find_x() + DOCK_FIND_W / 2 - MENU_W / 2,
                              panel_rest_y() - menu_full_h() - 8);
            return;
        }

        if (on_net_button(mx, my)) {
            net_open = !net_open;
            volume_open = false;      /* one thing open over the panel */
            need_frame();
            return;
        }

        if (on_volume_button(mx, my)) {
            net_open = false;
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

        int chip = taskbar_chip_at(mx, my);
        if (chip >= 0) {
            window_t *c = stack[chip];

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

        /* An icon first, then the desktop itself. A single click picks one
           out and a second within half a second opens it, which is the
           arrangement everything with icons has used since there were
           icons: dragging one has to begin by putting the pointer on it,
           so the first click cannot be the one that opens it. */
        int icon = desk_icon_at(mx, my);
        if (icon >= 0) {
            u64 now = timer_ticks();
            bool again = (icon == desk_last_click)
                      && (now - desk_last_tick)
                         < timer_hz() * (u64)theme()->dblclick_ms / 1000;
            desk_sel = 1u << icon;
            desk_last_click = icon;
            desk_last_tick = now;
            if (again) {
                launch(DESK[icon].program);
                desk_last_click = -1;
            }
            /* The right button on an icon is the desktop's menu too, with
               the icon picked out behind it. */
            if (right_now) open_ctx_at(mx, my);
            need_frame();
            return;
        }

        /* The wallpaper. Nothing is decided here: a press starts a band,
           and whether it turns out to be a band or a click is not known
           until the button comes up. Opening a menu on the press instead
           is what made it impossible to drag one out at all -- the menu
           was already up and eating the movement. */
        if (my < work_h()) {
            band_on = true;
            band_button = pressed_now ? 1 : 2;
            band_ax = band_bx = mx;
            band_ay = band_by = my;
            desk_sel = 0;
            need_frame();
        }
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
        int lx = mx - (w->x + WM_BORDER), ly = my - (w->y + WM_TOP);
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
        /* Every change of the buttons first, in the order they happened and
           at the position each happened at.
         *
         * Reading the state below is not enough on its own. One pass of this
           loop ends in compositing the whole screen, and a press and a
           release that both happen inside one of them read as no change at
           all: the buttons were up before and they are up now. That is a
           click on a busy desktop doing nothing, with nothing to say why,
           and it is what a swatch clicked three times running with no effect
           turned out to be. */
        mouse_edge_t edge;
        while (mouse_take_edge(&edge)) {
            last_mx = edge.x; last_my = edge.y;
            handle_mouse(edge.x, edge.y, edge.buttons);
            last_buttons = edge.buttons;
            need_frame();
        }

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
        } else if (c >= 0 && menu_open && menu_key(c)) {
            /* Typed into the field on the dock. Tried after the chords,
               so alt and tab still walks the stack with the launcher up,
               and before the windows, so a letter does not arrive in the
               terminal behind it. */
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
        if (ctx_open && (still_moving(ctx_since, MENU_MS)
                         || still_moving(ctx_hover_since, HOVER_MS))) {
            need_frame_in(ctx_x - 14, ctx_y - 14, CTX_W + 28, CTX_H + 28);
            need_frame_in(last_mx - 2, last_my - 2, 20, 28);
        }

        if (menu_open && (still_moving(menu_since, MENU_MS)
                          || still_moving(menu_hover_since, HOVER_MS))) {
            need_frame_in(menu_x - 14, menu_y - 14,
                          MENU_W + 28, menu_full_h() + 28);
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
    ctx_open = false;
    band_on = false;
    desk_sel = 0;
    dragging = resizing = mouse_capture = 0;
    volume_open = false;
    volume_drag = false;
    panel_shown = true;
    panel_since = 0;
    snap_preview = SNAP_NONE;
    shake_reset();
    mouse_set_autodraw(true);
}
