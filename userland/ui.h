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
#define UI_ROW       26      /* a list row, a menu item */
#define UI_BTN_H     30
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
    u32 soft;        /* accent at low strength, for a row that is selected */
    u32 line;        /* borders and separators */
    u32 warn;
} ui_theme;

/* Read once at startup, out of the same file the desktop reads.
 *
 * There was very nearly a second config file here, holding a palette just
 * for programs. That would have meant changing the accent in Settings
 * recoloured the window chrome and left every window's contents alone, which
 * is exactly the incoherence that makes a collection of programs not a
 * desktop environment. So this parses /zelr.cfg, the one the window manager
 * already re-reads four times a second, and derives everything from the same
 * choice.
 *
 * The file is "key value" lines, values in hex or decimal. Unknown keys are
 * skipped rather than refused, so a newer Settings cannot break an older
 * program. */

/* The same six the desktop offers, in the same order. Duplicated rather than
   fetched: a system call to read six constants would be a system call in
   every program's startup, and if they ever drift the cost is a swatch a
   shade off. */
#define UI_PRESETS 6
static const u32 UI_ACCENTS[UI_PRESETS] = {
    RGB(0x2C, 0xC7, 0xA0), RGB(0x6E, 0x8A, 0xE8), RGB(0xE0, 0xA0, 0x3C),
    RGB(0xE0, 0x6A, 0x8C), RGB(0x8A, 0x9B, 0xB0), RGB(0x9A, 0xD1, 0x4A),
};

static inline int ui_cfg_int(const char *text, const char *key, int fallback) {
    int klen = strlen(key);
    for (int i = 0; text[i]; i++) {
        if (i && text[i - 1] != '\n') continue;
        if (strncmp(text + i, key, klen)) continue;
        if (text[i + klen] != ' ') continue;

        const char *v = text + i + klen + 1;
        int hex = (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'));
        if (hex) v += 2;

        int got = 0, n = 0;
        for (; *v && *v != '\n'; v++) {
            int d;
            if (*v >= '0' && *v <= '9') d = *v - '0';
            else if (hex && *v >= 'a' && *v <= 'f') d = *v - 'a' + 10;
            else if (hex && *v >= 'A' && *v <= 'F') d = *v - 'A' + 10;
            else break;
            n = n * (hex ? 16 : 10) + d;
            got = 1;
        }
        return got ? n : fallback;
    }
    return fallback;
}

static inline ui_theme ui_load_theme(void) {
    char cfg[1024];
    int n = slurp("/zelr.cfg", cfg, sizeof(cfg) - 1);
    if (n < 0) n = 0;
    cfg[n] = 0;

    int preset = ui_cfg_int(cfg, "preset", 0);
    if (preset < 0 || preset >= UI_PRESETS) preset = 0;
    int light = ui_cfg_int(cfg, "light", 0);

    ui_theme t;
    t.accent = (u32)ui_cfg_int(cfg, "accent", (int)UI_ACCENTS[preset]);

    if (light) {
        t.bg        = RGB(0xf4, 0xf5, 0xf7);
        t.panel     = RGB(0xe7, 0xe9, 0xec);
        t.fg        = RGB(0x1c, 0x1f, 0x24);
        t.dim       = RGB(0x6a, 0x71, 0x7a);
        t.line      = RGB(0xd2, 0xd6, 0xdb);
        t.accent_fg = RGB(0xff, 0xff, 0xff);
    } else {
        t.bg        = RGB(0x1e, 0x20, 0x24);
        t.panel     = RGB(0x2a, 0x2d, 0x33);
        t.fg        = RGB(0xe6, 0xe8, 0xea);
        t.dim       = RGB(0x9a, 0xa0, 0xa8);
        t.line      = RGB(0x3a, 0x3e, 0x45);
        t.accent_fg = RGB(0xff, 0xff, 0xff);
    }
    t.soft = mix(t.bg, t.accent, light ? 40 : 48);
    t.warn = RGB(0xe0, 0x6c, 0x60);
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
    } else if (ev->type == WIN_EV_SCROLL) {
        in->scroll += ev->y;
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
    face_centred(s, x, y, w, h, label, t->fg, UI_FACE_BODY);

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
    face_centred(s, x, y, w, h, label, t->accent_fg, UI_FACE_BODY);

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

    /* A selected row is a tint of the accent with a bar down its leading
       edge, not a block of the accent itself. Filling the row solid makes
       the sidebar the loudest thing on screen, louder than whatever the
       selection is pointing at, and it fights every other use of the
       colour. The tint says the same thing at a tenth of the volume. */
    if (selected) {
        rect(s, x, y, w, h, t->soft);
        rect(s, x, y, 3, h, t->accent);
    } else if (over) {
        rect(s, x, y, w, h, mix(t->bg, t->fg, 18));
    }

    u32 fg = selected ? t->accent : t->fg;
    int ty = y + (h - face_h(UI_FACE_BODY)) / 2;
    face_draw(s, x + UI_PAD, ty, label, fg, UI_FACE_BODY);
    if (right) {
        int rw = face_w(right, UI_FACE_BODY);
        face_draw(s, x + w - UI_PAD - rw, ty, right,
                  selected ? t->accent : t->dim, UI_FACE_BODY);
    }

    if (over && in->right_pressed) { in->right_pressed = 0; return 2; }
    if (over && in->released)      { in->released = 0; return 1; }
    return 0;
}

static inline void ui_label(surface *s, const ui_theme *t,
                            int x, int y, const char *str) {
    face_draw(s, x, y, str, t->fg, UI_FACE_BODY);
}

static inline void ui_dim_label(surface *s, const ui_theme *t,
                                int x, int y, const char *str) {
    face_draw(s, x, y, str, t->dim, UI_FACE_BODY);
}

/* A heading with a rule under it, which is how a window says "new section". */
static inline int ui_section(surface *s, const ui_theme *t,
                             int x, int y, int w, const char *title) {
    face_draw(s, x, y + 4, title, t->dim, UI_FACE_BOLD);
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
    if (label) face_draw(s, x + tw + UI_GAP, y + (th - face_h(UI_FACE_BODY)) / 2,
                         label, t->fg, UI_FACE_BODY);

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

    int ty = y + (h - face_h(UI_FACE_BODY)) / 2;
    if (f->len == 0 && placeholder) {
        face_draw(s, x + UI_PAD, ty, placeholder, t->dim, UI_FACE_BODY);
    } else {
        /* Scroll so the cursor is always in view. The face is not fixed
           width, so where the cursor sits has to be measured rather than
           counted: the prefix before it is drawn into nothing and its
           width taken. */
        char keep = f->buf[f->cursor];
        f->buf[f->cursor] = 0;
        int upto = face_w(f->buf, UI_FACE_BODY);
        f->buf[f->cursor] = keep;

        int room = w - UI_PAD * 2;
        int shift = upto > room ? upto - room : 0;
        face_draw(s, x + UI_PAD - shift, ty, f->buf, t->fg, UI_FACE_BODY);
        if (f->focused && (ticks() / 30) % 2 == 0)
            rect(s, x + UI_PAD + upto - shift, ty, 2,
                 face_h(UI_FACE_BODY), t->accent);
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
    int ty = y + (UI_ROW - face_h(UI_FACE_BODY)) / 2;
    if (left)  face_draw(s, UI_PAD, ty, left, t->dim, UI_FACE_BODY);
    if (right) {
        int rw = face_w(right, UI_FACE_BODY);
        face_draw(s, w - UI_PAD - rw, ty, right, t->dim, UI_FACE_BODY);
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
        face_draw(s, x + UI_PAD, iy + (UI_ROW - face_h(UI_FACE_BODY)) / 2,
                  items[i], t->fg, UI_FACE_BODY);
        if (over && in->released) { in->released = 0; chosen = i; }
    }
    return chosen;
}
