#pragma once
#include "types.h"

#define WM_MAX_WINDOWS 8
#define WM_TITLE_H     20
#define WM_BORDER      4

/* How far the client area sits below the top of the window: the frame's
   edge and then the title bar. Named, because a dozen places need it and
   every one of them used to add the two up for itself, which is fine until
   the frame stops being one pixel. */
#define WM_TOP         (WM_BORDER + WM_TITLE_H)

typedef struct window window_t;

/* What a window hands to whoever owns it. Fixed layout: a ring 3 program
   reads these straight out of a buffer the kernel filled in. */
#define WM_EV_NONE   0
#define WM_EV_MOUSE  1
#define WM_EV_KEY    2
#define WM_EV_CLOSE  3
/* The window is a different size now. x and y carry the new content size,
   and whatever the program had drawn is gone. */
#define WM_EV_RESIZE 4
/* The wheel turned over this window. y carries the steps, positive
   downward; x and buttons are where the pointer was. */
#define WM_EV_SCROLL 5

typedef struct {
    u32 type;
    i32 x, y;
    u32 buttons;
    u32 key;
} wm_event_t;

#define WM_EVENT_QUEUE 32

/* Buttons are the raw PS/2 bitmask: bit 0 left, bit 1 right. */
typedef void (*wm_mouse_fn)(window_t *w, int x, int y, u8 buttons, bool just_pressed);
typedef void (*wm_key_fn)(window_t *w, char c);

struct window {
    int   x, y;               /* outer top-left, including the title bar */
    int   cw, ch;             /* content size */
    char  title[32];
    /* The program that opened it, so the taskbar can tell that the window
       in front is the app whose icon is pinned two inches to the left. A
       window the kernel opened for itself has none. */
    char  app[32];
    u32  *canvas;             /* cw * ch pixels, owned by the window */
    bool  open;
    bool  dirty;              /* content changed since the last composite */
    wm_mouse_fn on_mouse;
    wm_key_fn   on_key;
    void (*on_close)(window_t *w);   /* lets an app drop its handle */
    void *data;

    /* Events waiting for the program that owns this window. Only used when
       the owner is outside the kernel; in-kernel windows use the callbacks. */
    wm_event_t queue[WM_EVENT_QUEUE];
    u32  q_head, q_tail;

    /* The buttons carried by the last two mouse events pushed, which is how
       a pointer that only moved is told apart from one that also did
       something. See wm_push_event. */
    u32  q_last_buttons, q_prev_buttons;
    bool owned_by_user;

    /* Set by the owner. A window nobody has said can be resized is left
       alone by the edges, the corner grip and the maximise button. */
    bool resizable;

    /* A size the desktop has asked for but the program has not taken up.
       The swap cannot happen when the desktop decides it, because the
       program may be part way through drawing into the surface at that
       moment; it happens instead the next time the program calls in, when
       by definition it is not. Zero when there is nothing pending. */
    int  want_cw, want_ch;

    /* Where it was before it was maximised or snapped, so it has somewhere
       to go back to. */
    bool minimized, maximized;
    int  restore_x, restore_y, restore_cw, restore_ch;
};

window_t *wm_create(const char *title, int x, int y, int cw, int ch);
void wm_close(window_t *w);
void wm_invalidate(window_t *w);
void wm_raise(window_t *w);

/* Changes a window's size. Works for both kinds: the kernel's own windows
   own their pixels, and a program's are the window server's to replace. */
bool wm_resize(window_t *w, int cw, int ch);

/* Runs the desktop until the user leaves it. */
void wm_run(void);
void wm_quit(void);
bool wm_active(void);

void wm_push_event(window_t *w, const wm_event_t *ev);
bool wm_pop_event(window_t *w, wm_event_t *out);

int  wm_outer_w(const window_t *w);
int  wm_outer_h(const window_t *w);

/* Where the desktop's icon column ends, so a window opening for the first
   time does not land on top of it. The icons are the one thing on the
   desktop that is there to be looked at when nothing is running, and every
   window used to open over them. */
int  wm_icons_right(void);

/* The height a window has to live in: the screen, less the panel when the
   panel is resting on the bottom of it. */
int  wm_work_height(void);
