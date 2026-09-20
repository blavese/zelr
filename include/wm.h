#pragma once
#include "types.h"

#define WM_MAX_WINDOWS 8

/* How tall the title bar is, and how wide the frame around a window.
 *
 * Both were constants. Twenty pixels was what a bar is when the buttons in
 * it are sixteen, and four pixels of frame was a band of window colour all
 * the way round whatever the program had drawn -- which is why every
 * window looked like a picture in a mount. The current answers are
 * thirty two and one, and the point is that they are answers rather than
 * facts: somebody may want a bar with more room in it or a hairline of a
 * different weight, and there is no reason a desktop should have one
 * person's taste compiled into it.
 *
 * They stay macros so the forty places that use them do not each have to
 * change, and so that none of them can hold a copy of a number the
 * settings window is about to move. */
int wm_title_h(void);
int wm_border(void);

#define WM_TITLE_H     wm_title_h()
#define WM_BORDER      wm_border()

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
/* Somebody is looking for something. What they are looking for does not fit
   in an event -- it is a word, and this carries four numbers -- so the
   window asks for it with wm_find_query when this arrives. `y` is which
   match to go to, counting from nought, so pressing return again walks
   through them. */
#define WM_EV_FIND   6

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

/* What a window is showing, as words.
 *
 * A window is a rectangle of pixels and nothing else, so the desktop cannot
 * read what is on one: there is no text in a picture of text. A program
 * that wants to be searchable therefore says what it is showing, and this
 * is where that is kept.
 *
 * Four kilobytes of it, which is a screenful of a terminal or the visible
 * part of a page rather than the whole of either. A find that looked
 * through everything a program holds would answer about things nobody can
 * see, and the question being asked is about what is on the screen. */
#define WM_TEXT_MAX 4096

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

    /* What this window is showing, as the program describes it. Empty for
       a window whose program has never said, which is most of them and is
       why find says which windows it looked at. */
    char text[WM_TEXT_MAX];
    int  textlen;

    /* Where it was before it was maximised or snapped, so it has somewhere
       to go back to. */
    bool minimized, maximized;
    int  restore_x, restore_y, restore_cw, restore_ch;
};

/* What a program says it is showing, for find to look through. */
void wm_set_text(window_t *w, const char *s, int len);

/* And what is being looked for, for a program that has just been told. */
int  wm_find_query(char *out, int cap);

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
