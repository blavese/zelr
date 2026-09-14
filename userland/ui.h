/* Widgets, so the programs look like they came from the same place.
 *
 * Before this, every program drew its own buttons and picked its own
 * spacing, and it showed: two windows side by side agreed about nothing. A
 * desktop environment is not a window manager plus some programs, it is a
 * set of decisions about how things look and behave, applied everywhere.
 * This file is where those decisions live.
 *
 * It is immediate mode. There is no widget tree and nothing is retained
 * between frames: a program describes what it wants this frame and gets back
 * whether the user did anything. That suits a system where a window is a
 * block of pixels and an event queue, it keeps state in the program where
 * the program already has it, and it is about a tenth of the code of the
 * alternative.
 *
 * The palette comes from the theme file, so a program that uses these gets
 * the user's colours without knowing there is such a thing as a theme. */
#pragma once
#include "draw.h"

/* --- metrics -------------------------------------------------------------
 *
 * One scale, used everywhere. The numbers are multiples of 4 because the
 * font cell is 8x16 and anything else puts text on half a pixel. */
#define UI_PAD        8      /* inside a container, to its contents */
#define UI_GAP        6      /* between two things that belong together */
#define UI_ROW       24      /* a list row, a menu item */
#define UI_BTN_H     28
#define UI_TITLE_H   28      /* a section header inside a window */
#define UI_RADIUS     6
#define UI_SCROLL_W  10

typedef struct {
    u32 bg;          /* the window's ground */
    u32 panel;       /* a raised area: toolbars, sidebars */
    u32 fg;          /* body text */
    u32 dim;         /* secondary text */
    u32 accent;      /* selection, focus, the active thing */
    u32 accent_fg;   /* text on top of accent */
    u32 line;        /* borders and separators */
    u32 warn;
} ui_theme;

/* Read once at startup. The defaults are a dark grey that does not fight
   with anything; /cfg/theme overrides them. */
static inline ui_theme ui_load_theme(void) {
    ui_theme t;
    t.bg        = RGB(0x1e, 0x20, 0x24);
    t.panel     = RGB(0x2a, 0x2d, 0x33);
    t.fg        = RGB(0xe6, 0xe8, 0xea);
    t.dim       = RGB(0x9a, 0xa0, 0xa8);
    t.accent    = RGB(0x4a, 0x8c, 0xf0);
    t.accent_fg = RGB(0xff, 0xff, 0xff);
    t.line      = RGB(0x3a, 0x3e, 0x45);
    t.warn      = RGB(0xe0, 0x6c, 0x60);

    /* The theme file is a list of "name value" lines, value in hex. Unknown
       names are skipped rather than refused, so a newer settings program
       cannot break an older one. */
    char buf[1024];
    int n = slurp("/cfg/theme", buf, sizeof(buf) - 1);
    if (n <= 0) return t;
    buf[n] = 0;

    int i = 0;
    while (i < n) {
        int ls = i;
        while (i < n && buf[i] != '\n') i++;
        int le = i;
        if (i < n) i++;

        /* split the line on its first space */
        int sp = ls;
        while (sp < le && buf[sp] != ' ') sp++;
        if (sp >= le) continue;
        buf[sp] = 0;
        buf[le] = 0;

        u32 v = 0;
        for (const char *p = buf + sp + 1; *p; p++) {
            int d;
            if (*p >= '0' && *p <= '9') d = *p - '0';
            else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
            else continue;
            v = v * 16 + (u32)d;
        }

        const char *k = buf + ls;
        if      (!strcmp(k, "bg"))        t.bg = v;
        else if (!strcmp(k, "panel"))     t.panel = v;
        else if (!strcmp(k, "fg"))        t.fg = v;
        else if (!strcmp(k, "dim"))       t.dim = v;
        else if (!strcmp(k, "accent"))    t.accent = v;
        else if (!strcmp(k, "accentfg"))  t.accent_fg = v;
        else if (!strcmp(k, "line"))      t.line = v;
        else if (!strcmp(k, "warn"))      t.warn = v;
    }
    return t;
}

/* --- input for a frame ----------------------------------------------------
 *
 * A program collects events into one of these and hands it to every widget.
 * Widgets read it; only the click flags are consumed, so two overlapping
 * widgets cannot both claim the same press. */
typedef struct {
    int  mx, my;          /* where the pointer is */
    int  down;            /* a button is held */
    int  pressed;         /* it went down this frame */
    int  released;        /* it came up this frame */
    int  right_pressed;
    u32  key;             /* a key this frame, 0 for none */
    int  scroll;          /* wheel, in rows, positive is down */
} ui_input;

static inline void ui_begin(ui_input *in) {
    in->pressed = in->released = in->right_pressed = 0;
    in->key = 0;
    in->scroll = 0;
}

/* Folds one window event into the frame's input. */
static inline void ui_feed(ui_input *in, const win_event *ev) {
    if (ev->type == WIN_EV_MOUSE) {
        in->mx = ev->x;
        in->my = ev->y;
        int now = (ev->buttons & WIN_BTN_LEFT) != 0;
        if (now && !in->down) in->pressed = 1;
        if (!now && in->down) in->released = 1;
        in->down = now;
        if (ev->buttons & WIN_BTN_RIGHT) in->right_pressed = 1;
    } else if (ev->type == WIN_EV_KEY) {
        in->key = ev->key;
    }
}

static inline int ui_hit(const ui_input *in, int x, int y, int w, int h) {
    return in->mx >= x && in->mx < x + w && in->my >= y && in->my < y + h;
}

/* --- widgets -------------------------------------------------------------- */

/* Returns 1 on the frame the button is released inside itself, which is what
   a click is: pressing and dragging away has to be cancellable. */
static inline int ui_button(surface *s, ui_input *in, const ui_theme *t,
                            int x, int y, int w, const char *label) {
    int h = UI_BTN_H;
    int over = ui_hit(in, x, y, w, h);
    int held = over && in->down;

    u32 face = held ? mix(t->panel, t->accent, 90)
             : over ? mix(t->panel, t->fg, 24)
             : t->panel;
    round_rect(s, x, y, w, h, UI_RADIUS, face);
    text_centred(s, x, y, w, h, label, t->fg);

    if (over && in->released) { in->released = 0; return 1; }
    return 0;
}

/* A button that reads as the one to press. Same shape, accent coloured. */
static inline int ui_button_primary(surface *s, ui_input *in, const ui_theme *t,
                                    int x, int y, int w, const char *label) {
    int h = UI_BTN_H;
    int over = ui_hit(in, x, y, w, h);
    int held = over && in->down;

    u32 face = held ? mix(t->accent, 0, 60) : over ? mix(t->accent, 0xFFFFFF, 30) : t->accent;
    round_rect(s, x, y, w, h, UI_RADIUS, face);
    text_centred(s, x, y, w, h, label, t->accent_fg);

    if (over && in->released) { in->released = 0; return 1; }
    return 0;
}

/* A row that can be selected. Returns 1 on click, 2 on a right click, so a
   caller can raise a context menu without a second hit test. */
static inline int ui_row(surface *s, ui_input *in, const ui_theme *t,
                         int x, int y, int w, const char *label,
                         const char *right, int selected) {
    int h = UI_ROW;
    int over = ui_hit(in, x, y, w, h);

    if (selected)  rect(s, x, y, w, h, t->accent);
    else if (over) rect(s, x, y, w, h, mix(t->bg, t->fg, 18));

    u32 fg = selected ? t->accent_fg : t->fg;
    text(s, x + UI_PAD, y + (h - FONT_H) / 2, label, fg);
    if (right) {
        int rw = strlen(right) * FONT_W;
        text(s, x + w - UI_PAD - rw, y + (h - FONT_H) / 2, right,
             selected ? t->accent_fg : t->dim);
    }

    if (over && in->right_pressed) { in->right_pressed = 0; return 2; }
    if (over && in->released)      { in->released = 0; return 1; }
    return 0;
}

static inline void ui_label(surface *s, const ui_theme *t,
                            int x, int y, const char *str) {
    text(s, x, y, str, t->fg);
}

static inline void ui_dim_label(surface *s, const ui_theme *t,
                                int x, int y, const char *str) {
    text(s, x, y, str, t->dim);
}

/* A heading with a rule under it, which is how a window says "new section". */
static inline int ui_section(surface *s, const ui_theme *t,
                             int x, int y, int w, const char *title) {
    text(s, x, y + 6, title, t->dim);
    rect(s, x, y + UI_TITLE_H - 1, w, 1, t->line);
    return y + UI_TITLE_H + UI_GAP;
}

static inline int ui_toggle(surface *s, ui_input *in, const ui_theme *t,
                            int x, int y, const char *label, int value) {
    int tw = 40, th = 20;
    int over = ui_hit(in, x, y, tw, th);

    round_rect(s, x, y, tw, th, th / 2, value ? t->accent : mix(t->panel, t->fg, 20));
    int knob = value ? x + tw - th + 2 : x + 2;
    disc(s, knob + (th - 4) / 2, y + th / 2, (th - 4) / 2, RGB(0xff, 0xff, 0xff));
    if (label) text(s, x + tw + UI_GAP, y + (th - FONT_H) / 2, label, t->fg);

    if (over && in->released) { in->released = 0; return !value; }
    return value;
}

/* Returns the new value, so a caller writes `v = ui_slider(...)`. */
static inline int ui_slider(surface *s, ui_input *in, const ui_theme *t,
                            int x, int y, int w, int value, int lo, int hi) {
    int h = 18;
    int over = ui_hit(in, x, y - 6, w, h + 12);

    rect(s, x, y + h / 2 - 2, w, 4, mix(t->panel, t->fg, 20));
    if (hi <= lo) hi = lo + 1;
    if (value < lo) value = lo;
    if (value > hi) value = hi;

    int span = w - 16;
    int at = x + 8 + (value - lo) * span / (hi - lo);
    rect(s, x, y + h / 2 - 2, at - x, 4, t->accent);
    disc(s, at, y + h / 2, 8, t->accent);
    disc(s, at, y + h / 2, 5, RGB(0xff, 0xff, 0xff));

    if (over && in->down) {
        int rel = in->mx - (x + 8);
        if (rel < 0) rel = 0;
        if (rel > span) rel = span;
        return lo + rel * (hi - lo) / (span ? span : 1);
    }
    return value;
}

/* The bar beside a list. Drawn only when there is more than fits, because a
   scrollbar on a list that does not scroll is a lie about the content. */
static inline void ui_scrollbar(surface *s, const ui_theme *t,
                                int x, int y, int h, int first, int shown, int total) {
    if (total <= shown) return;
    rect(s, x, y, UI_SCROLL_W, h, mix(t->bg, 0, 40));
    int th = h * shown / total;
    if (th < 20) th = 20;
    int ty = y + (h - th) * first / (total - shown);
    round_rect(s, x + 2, ty, UI_SCROLL_W - 4, th, (UI_SCROLL_W - 4) / 2,
               mix(t->panel, t->fg, 60));
}

/* --- a single line of editable text ---------------------------------------
 *
 * Keeps no state of its own: the caller owns the buffer and the cursor, so
 * two fields in one window do not need a widget identity to tell apart. */
typedef struct {
    char *buf;
    int   cap;
    int   len;
    int   cursor;
    int   focused;
} ui_field;

static inline void ui_field_key(ui_field *f, u32 key) {
    if (!f->focused || !key) return;

    if (key == '\b') {
        if (f->cursor > 0) {
            for (int i = f->cursor - 1; i < f->len - 1; i++) f->buf[i] = f->buf[i + 1];
            f->len--; f->cursor--;
            f->buf[f->len] = 0;
        }
        return;
    }
    if (key == KEY_DELETE) {
        if (f->cursor < f->len) {
            for (int i = f->cursor; i < f->len - 1; i++) f->buf[i] = f->buf[i + 1];
            f->len--;
            f->buf[f->len] = 0;
        }
        return;
    }
    if (key == KEY_LEFT)  { if (f->cursor > 0) f->cursor--; return; }
    if (key == KEY_RIGHT) { if (f->cursor < f->len) f->cursor++; return; }
    if (key == KEY_HOME)  { f->cursor = 0; return; }
    if (key == KEY_END)   { f->cursor = f->len; return; }
    if (KEY_IS_SPECIAL(key)) return;
    if (key < 32) return;

    if (f->len + 1 >= f->cap) return;
    for (int i = f->len; i > f->cursor; i--) f->buf[i] = f->buf[i - 1];
    f->buf[f->cursor++] = (char)key;
    f->len++;
    f->buf[f->len] = 0;
}

static inline void ui_field_draw(surface *s, ui_input *in, const ui_theme *t,
                                 int x, int y, int w, ui_field *f,
                                 const char *placeholder) {
    int h = UI_BTN_H;
    int over = ui_hit(in, x, y, w, h);
    if (over && in->released) { in->released = 0; f->focused = 1; }
    else if (in->released && !over) f->focused = 0;

    round_rect(s, x, y, w, h, UI_RADIUS, mix(t->bg, 0, 50));
    if (f->focused) {
        /* A ring rather than a fill, so the text stays as legible as it was. */
        round_rect(s, x, y, w, 2, 1, t->accent);
        round_rect(s, x, y + h - 2, w, 2, 1, t->accent);
    }

    int ty = y + (h - FONT_H) / 2;
    if (f->len == 0 && placeholder) {
        text(s, x + UI_PAD, ty, placeholder, t->dim);
    } else {
        /* Scroll so the cursor is always in view. */
        int fit = (w - UI_PAD * 2) / FONT_W;
        int from = f->cursor > fit ? f->cursor - fit : 0;
        text(s, x + UI_PAD, ty, f->buf + from, t->fg);
        if (f->focused && (ticks() / 30) % 2 == 0)
            rect(s, x + UI_PAD + (f->cursor - from) * FONT_W, ty, 2, FONT_H, t->accent);
    }
}

/* --- chrome --------------------------------------------------------------- */

/* The strip across the top of a window that holds its controls. */
static inline void ui_toolbar(surface *s, const ui_theme *t, int w, int h) {
    rect(s, 0, 0, w, h, t->panel);
    rect(s, 0, h - 1, w, 1, t->line);
}

static inline void ui_statusbar(surface *s, const ui_theme *t,
                                int w, int h, const char *left, const char *right) {
    int y = h - UI_ROW;
    rect(s, 0, y, w, UI_ROW, t->panel);
    rect(s, 0, y, w, 1, t->line);
    if (left)  text(s, UI_PAD, y + (UI_ROW - FONT_H) / 2, left, t->dim);
    if (right) {
        int rw = strlen(right) * FONT_W;
        text(s, w - UI_PAD - rw, y + (UI_ROW - FONT_H) / 2, right, t->dim);
    }
}

/* --- a menu that pops up where it was asked for ---------------------------
 *
 * Returns the index of the chosen item, or -1. The caller decides whether it
 * is open, so a right click can raise one and a click anywhere else closes
 * it without the menu having to know about the rest of the window. */
static inline int ui_menu(surface *s, ui_input *in, const ui_theme *t,
                          int x, int y, int w,
                          const char *const *items, int count) {
    int h = count * UI_ROW + UI_GAP * 2;

    if (x + w > s->w) x = s->w - w;
    if (y + h > s->h) y = s->h - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    /* Something under it, so it reads as floating rather than painted on. */
    for (int i = 3; i > 0; i--)
        round_rect(s, x - i, y - i + 2, w + i * 2, h + i * 2, UI_RADIUS + i,
                   mix(t->bg, 0, 30 / i));

    round_rect(s, x, y, w, h, UI_RADIUS, t->panel);

    int chosen = -1;
    for (int i = 0; i < count; i++) {
        int iy = y + UI_GAP + i * UI_ROW;
        int over = ui_hit(in, x, iy, w, UI_ROW);
        if (over) rect(s, x + 2, iy, w - 4, UI_ROW, mix(t->panel, t->accent, 120));
        text(s, x + UI_PAD, iy + (UI_ROW - FONT_H) / 2, items[i], t->fg);
        if (over && in->released) { in->released = 0; chosen = i; }
    }
    return chosen;
}
