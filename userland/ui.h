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

    /* --- the edges -------------------------------------------------------
     *
     * The same idea the window chrome is built from, so a button inside a
     * window and a button on the title bar catch the light the same way. A
     * surface here is a plane lit from the top left: raised things are
     * bright there and dark at the bottom right, sunk things are the other
     * way round, and a control says what it is by its edges rather than by
     * being a different colour.
     *
     * Four, because a bevel is two pixels: the outer pair carry the strong
     * colours and the inner pair the soft ones. */
    u32 edge_hi;
    u32 edge_light;
    u32 edge_shadow;
    u32 edge_dark;
    u32 well;        /* the ground inside something sunk: a field, a list */

    /* --- which century ---------------------------------------------------
     *
     * The kernel's chrome can be built out of bevels or out of material, and
     * a program's own controls have to be whichever the window around them
     * is. A modern title bar with a bevelled toolbar underneath it is the
     * worst of both and reads as a program that was not updated. */
    int modern;
    u32 stroke;      /* the hairline that replaces a bevel */
    u32 raised;      /* one layer up from bg, for a toolbar or a card */
} ui_theme;

/* --- surfaces that catch the light ---------------------------------------
 *
 * Every raised and sunk thing in every program goes through these two, so
 * that they all agree about where the light is. Getting that wrong in one
 * place is the difference between a desktop and a pile of programs. */
static inline void ui_bevel(surface *s, int x, int y, int w, int h,
                            u32 tl_out, u32 tl_in, u32 br_in, u32 br_out) {
    if (w <= 0 || h <= 0) return;
    /* The top row whole and the left column starting below it, so the two
       do not both claim the corner and leave a notch. */
    rect(s, x, y, w, 1, tl_out);
    rect(s, x, y + 1, 1, h - 1, tl_out);
    rect(s, x, y + h - 1, w, 1, br_out);
    rect(s, x + w - 1, y, 1, h - 1, br_out);
    if (w <= 2 || h <= 2) return;
    rect(s, x + 1, y + 1, w - 2, 1, tl_in);
    rect(s, x + 1, y + 2, 1, h - 3, tl_in);
    rect(s, x + 1, y + h - 2, w - 2, 1, br_in);
    rect(s, x + w - 2, y + 1, 1, h - 2, br_in);
}

/* How much of one pixel falls inside a corner circle, as 0 to 255. The
   kernel has the same routine for the same reason: sixteen samples rather
   than one yes-or-no is the whole difference between a corner that reads as
   a curve and one that reads as a staircase. */
static inline int ui_corner_cover(int px, int cy8, int r8) {
    int inside = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int dx = (px * 8 + sx * 2 + 1) - r8;
            int dy = (sy * 2 + 1) - cy8;
            if (dx * dx + dy * dy <= r8 * r8) inside++;
        }
    return inside * 255 / 16;
}

static inline void ui_round(surface *s, int x, int y, int w, int h, int r,
                            u32 c, int alpha) {
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;

    for (int j = 0; j < h; j++) {
        int from_edge = j < h - 1 - j ? j : h - 1 - j;
        int py = y + j;
        if (py < 0 || py >= s->h) continue;

        if (from_edge >= r) {
            if (alpha >= 255) rect(s, x, py, w, 1, c);
            else for (int px = x; px < x + w; px++) {
                if (px < 0 || px >= s->w) continue;
                u32 *slot = &s->px[(u32)py * s->w + px];
                *slot = mix(*slot, c, alpha);
            }
            continue;
        }

        int cy8 = (r - from_edge) * 8;
        for (int i = 0; i < r; i++) {
            int cov = ui_corner_cover(i, cy8, r * 8);
            if (!cov) continue;
            int a = alpha >= 255 ? cov : cov * alpha / 255;
            int left = x + i, right = x + w - 1 - i;
            if (left >= 0 && left < s->w) {
                u32 *sl = &s->px[(u32)py * s->w + left];
                *sl = mix(*sl, c, a);
            }
            if (right != left && right >= 0 && right < s->w) {
                u32 *sl = &s->px[(u32)py * s->w + right];
                *sl = mix(*sl, c, a);
            }
        }
        if (alpha >= 255) rect(s, x + r, py, w - r * 2, 1, c);
        else for (int px = x + r; px < x + w - r; px++) {
            if (px < 0 || px >= s->w) continue;
            u32 *slot = &s->px[(u32)py * s->w + px];
            *slot = mix(*slot, c, alpha);
        }
    }
}

/* A hairline around a rounded shape: the shape in the line colour with the
   shape one pixel smaller punched back out of it in the fill colour. */
static inline void ui_round_outline(surface *s, int x, int y, int w, int h,
                                    int r, u32 fill, u32 line) {
    ui_round(s, x, y, w, h, r, line, 255);
    ui_round(s, x + 1, y + 1, w - 2, h - 2, r > 0 ? r - 1 : 0, fill, 255);
}

static inline void ui_raised(surface *s, const ui_theme *t,
                             int x, int y, int w, int h) {
    ui_bevel(s, x, y, w, h, t->edge_hi, t->edge_light,
             t->edge_shadow, t->edge_dark);
}

static inline void ui_sunken(surface *s, const ui_theme *t,
                             int x, int y, int w, int h) {
    ui_bevel(s, x, y, w, h, t->edge_dark, t->edge_shadow,
             t->edge_light, t->edge_hi);
}

/* A groove: one pixel each way, for a separator or the line under a menu
   bar, where a full bevel would be too much weight for what it is saying. */
static inline void ui_groove(surface *s, const ui_theme *t,
                             int x, int y, int w, int h) {
    rect(s, x, y, w, 1, t->edge_shadow);
    rect(s, x, y + 1, w, 1, t->edge_hi);
    (void)h;
}

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

    int preset = ui_cfg_int(cfg, "preset", 1);
    if (preset < 0 || preset >= UI_PRESETS) preset = 0;
    int light = ui_cfg_int(cfg, "light", 1);

    /* Zero is modern, which is what a machine with no settings file gets.
       The same default as the kernel's, and it has to stay the same one:
       a window whose chrome disagrees with its contents about which look
       it is reads as broken rather than as either. */
    int built = ui_cfg_int(cfg, "look", 0);

    ui_theme t;
    t.modern = !built;
    t.accent = (u32)ui_cfg_int(cfg, "accent", (int)UI_ACCENTS[preset]);

    /* And then the accent the desktop is actually using, if the kernel is
       offering it.
     *
       Everything above is this program working the palette out for itself
       from preset, light and look, which is right while those three are
       all that decides a colour. It stops being right the moment somebody
       sets one by hand: the window manager would draw the dock in that
       colour and every window in front of it would draw its buttons in
       whatever this guessed. Reading /sys/theme is asking rather than
       guessing, and a machine that is not offering it falls through to the
       guess, which is what this always did. */
    {
        char live[192];
        int ln = slurp("/sys/theme", live, sizeof(live) - 1);
        if (ln > 0) {
            live[ln] = 0;
            t.accent = (u32)ui_cfg_int(live, "accent", (int)t.accent);
        }
    }

    if (t.modern) {
        /* Near white, because nothing here is a bevel that needs room above
           and below it. What a modern surface needs is to be quiet enough
           that a one pixel line shows on it. */
        if (light) {
            t.bg        = RGB(0xf4, 0xf4, 0xf7);
            t.panel     = RGB(0xfa, 0xfa, 0xfc);
            t.fg        = RGB(0x17, 0x18, 0x1c);
            t.dim       = RGB(0x5d, 0x60, 0x6a);
            t.line      = RGB(0xdd, 0xdd, 0xe3);
            t.stroke    = RGB(0xd2, 0xd3, 0xda);
            t.raised    = RGB(0xff, 0xff, 0xff);
            t.well      = RGB(0xff, 0xff, 0xff);
        } else {
            t.bg        = RGB(0x22, 0x24, 0x2b);
            t.panel     = RGB(0x2a, 0x2d, 0x35);
            t.fg        = RGB(0xe6, 0xe8, 0xea);
            t.dim       = RGB(0x9a, 0xa0, 0xa8);
            t.line      = RGB(0x35, 0x38, 0x41);
            t.stroke    = RGB(0x3c, 0x40, 0x4a);
            t.raised    = RGB(0x2f, 0x32, 0x3b);
            t.well      = RGB(0x1b, 0x1d, 0x23);
        }
        t.accent_fg   = RGB(0xff, 0xff, 0xff);
        /* Kept so that anything still asking for an edge gets something
           harmless rather than a colour from the other palette. */
        t.edge_hi     = t.raised;
        t.edge_light  = t.raised;
        t.edge_shadow = t.stroke;
        t.edge_dark   = t.stroke;
        t.soft = mix(t.bg, t.accent, light ? 40 : 48);
        t.warn = RGB(0xe0, 0x6c, 0x60);
        return t;
    }

    t.stroke = 0;
    t.raised = 0;

    if (light) {
        /* The same grey the chrome is made of. A near-white surface has
           nowhere to put a highlight, so every bevel on it collapses to one
           grey line and the whole window goes flat. */
        t.bg        = RGB(0xd6, 0xd3, 0xcd);
        t.panel     = RGB(0xd6, 0xd3, 0xcd);
        t.fg        = RGB(0x12, 0x12, 0x14);
        t.dim       = RGB(0x5c, 0x5a, 0x57);
        t.line      = RGB(0x8e, 0x8b, 0x86);
        t.accent_fg = RGB(0xff, 0xff, 0xff);
        t.edge_hi     = RGB(0xff, 0xff, 0xff);
        t.edge_light  = RGB(0xe8, 0xe6, 0xe1);
        t.edge_shadow = RGB(0x86, 0x84, 0x80);
        t.edge_dark   = RGB(0x3c, 0x3b, 0x39);
        t.well        = RGB(0xff, 0xff, 0xff);
    } else {
        t.bg        = RGB(0x2a, 0x2d, 0x33);
        t.panel     = RGB(0x2a, 0x2d, 0x33);
        t.fg        = RGB(0xe6, 0xe8, 0xea);
        t.dim       = RGB(0x9a, 0xa0, 0xa8);
        t.line      = RGB(0x18, 0x1a, 0x1e);
        t.accent_fg = RGB(0xff, 0xff, 0xff);
        t.edge_hi     = RGB(0x5a, 0x5f, 0x68);
        t.edge_light  = RGB(0x3c, 0x40, 0x48);
        t.edge_shadow = RGB(0x1c, 0x1e, 0x22);
        t.edge_dark   = RGB(0x0c, 0x0d, 0x10);
        t.well        = RGB(0x1a, 0x1c, 0x20);
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
/* How many keys one frame can carry.
 *
 * It used to carry one, and each new one overwrote the last. A frame is
 * sixteen milliseconds and somebody typing quickly puts two or three keys
 * inside one, so letters went missing out of the middle of whatever was
 * being typed: a browser given an address got most of it, and the editor
 * dropped characters out of sentences. Nothing reported it, because from
 * inside the program the key simply never arrived. */
#define UI_KEYS 16

typedef struct {
    int  mx, my;          /* where the pointer is */
    int  down;            /* a button is held */
    int  pressed;         /* it went down this frame */
    int  released;        /* it came up this frame */
    int  right_pressed;
    u32  key;             /* the first key this frame, 0 for none */
    u32  keys[UI_KEYS];   /* and all of them, in the order they arrived */
    int  nkeys;
    int  scroll;          /* wheel, in rows, positive is down */
} ui_input;

static inline void ui_begin(ui_input *in) {
    in->pressed = in->released = in->right_pressed = 0;
    in->key = 0;
    in->nkeys = 0;
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
        /* The first one, not the last. A program that reads in->key and
           nothing else then acts on the key that arrived first, which is the
           one a person pressed first. */
        if (!in->nkeys) in->key = ev->key;
        if (in->nkeys < UI_KEYS) in->keys[in->nkeys++] = ev->key;
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

    /* Held is not a colour. The bevel turns over and the label moves a
       pixel down and right, so the button is genuinely depressed: that
       reads as a press at any size and in any palette, which a tint does
       not. */
    if (t->modern) {
        /* A card with a hairline, lifting under the pointer and settling
           when pressed. The press is a colour change rather than a bevel
           turning over, because there is no bevel to turn. */
        u32 face = held ? mix(t->raised, t->accent, 40)
                        : (over ? mix(t->raised, t->accent, 14) : t->raised);
        ui_round_outline(s, x, y, w, h, 7, face,
                         held || over ? t->accent : t->stroke);
        face_centred(s, x, y + (held ? 1 : 0), w, h, label, t->fg,
                     UI_FACE_BODY);
        if (over && in->released) { in->released = 0; return 1; }
        return 0;
    }

    u32 face = over && !held ? mix(t->panel, t->edge_hi, 70) : t->panel;
    rect(s, x, y, w, h, face);
    if (held) ui_sunken(s, t, x, y, w, h);
    else      ui_raised(s, t, x, y, w, h);
    face_centred(s, x + (held ? 1 : 0), y + (held ? 1 : 0), w, h,
                 label, t->fg, UI_FACE_BODY);

    if (over && in->released) { in->released = 0; return 1; }
    return 0;
}

/* A button that reads as the one to press. Same shape, accent coloured. */
static inline int ui_button_primary(surface *s, ui_input *in, const ui_theme *t,
                                    int x, int y, int w, const char *label) {
    int h = UI_BTN_H;
    int over = ui_hit(in, x, y, w, h);
    int held = over && in->down;

    if (t->modern) {
        u32 face = held ? mix(t->accent, 0, 40)
                        : (over ? mix(t->accent, 0xFFFFFF, 26) : t->accent);
        ui_round(s, x, y, w, h, 7, face, 255);
        face_centred(s, x, y + (held ? 1 : 0), w, h, label, t->accent_fg,
                     UI_FACE_BODY);
        if (over && in->released) { in->released = 0; return 1; }
        return 0;
    }

    u32 face = over && !held ? mix(t->accent, 0xFFFFFF, 30) : t->accent;
    rect(s, x, y, w, h, face);
    if (held) ui_sunken(s, t, x, y, w, h);
    else      ui_raised(s, t, x, y, w, h);
    face_centred(s, x + (held ? 1 : 0), y + (held ? 1 : 0), w, h,
                 label, t->accent_fg, UI_FACE_BODY);
    /* And a ring just inside the bevel, which is how the default button in
       a dialogue said it was the default long before anything glowed. */
    if (!held) ui_bevel(s, x + 2, y + 2, w - 4, h - 4,
                        mix(t->accent, 0xFFFFFF, 60), face, face,
                        mix(t->accent, 0, 70));

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

/* Returns 1 on the frame it was clicked into, which is not the same as the
   frame it became focused on: clicking a field that already has the
   keyboard is a thing people do constantly and the caller cannot see it
   otherwise, because the release is consumed here. */
static inline int ui_field_draw(surface *s, ui_input *in, const ui_theme *t,
                                int x, int y, int w, ui_field *f,
                                const char *placeholder) {
    int h = UI_BTN_H;
    int over = ui_hit(in, x, y, w, h);
    int clicked = 0;
    if (over && in->released) {
        in->released = 0;
        f->focused = 1;
        clicked = 1;
    } else if (in->released && !over) f->focused = 0;

    /* Sunk, and paper coloured inside. Somewhere to type is a hole in the
       surface with something white at the bottom of it, which is the one
       shape that has always meant "this accepts text" without a label. */
    if (t->modern) {
        /* A rounded well with a hairline, and the line goes accent coloured
           when it has the keyboard. A ring rather than a bar under the
           text: the bar was the two pixels a bevel left spare, and with no
           bevel there is nowhere for it to sit. */
        ui_round_outline(s, x, y, w, h, 7, t->well,
                         f->focused ? t->accent : t->stroke);
    } else {
        rect(s, x, y, w, h, t->well);
        ui_sunken(s, t, x, y, w, h);
        if (f->focused) rect(s, x + 2, y + 2, w - 4, 1, t->accent);
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
        face_draw_clip(s, x + UI_PAD - shift, ty, x + 2, x + w - 2,
                       f->buf, t->fg, UI_FACE_BODY);
        if (f->focused && (ticks() / 30) % 2 == 0)
            rect(s, x + UI_PAD + upto - shift, ty, 2,
                 face_h(UI_FACE_BODY), t->accent);
    }

    return clicked;
}

/* A well: somewhere content lives, sunk into the window with paper at the
   bottom of it. Lists, text areas, canvases, anything being looked at rather
   than pressed. Returns the inside, so a caller draws into the hole rather
   than working out the two pixel inset for itself. */
static inline void ui_well(surface *s, const ui_theme *t,
                           int x, int y, int w, int h,
                           int *ix, int *iy, int *iw, int *ih) {
    if (t->modern) ui_round_outline(s, x, y, w, h, 8, t->well, t->stroke);
    else {
        rect(s, x, y, w, h, t->well);
        ui_sunken(s, t, x, y, w, h);
    }
    if (ix) *ix = x + 2;
    if (iy) *iy = y + 2;
    if (iw) *iw = w - 4;
    if (ih) *ih = h - 4;
}

/* --- chrome --------------------------------------------------------------- */

/* The strip across the top of a window that holds its controls. */
static inline void ui_toolbar(surface *s, const ui_theme *t, int w, int h) {
    rect(s, 0, 0, w, h, t->panel);
    if (t->modern) {
        /* One line, not a groove. A groove is two pixels catching light
           from a source that is not lighting anything else here. */
        rect(s, 0, h - 1, w, 1, t->line);
    } else {
        ui_groove(s, t, 0, h - 2, w, 2);
    }
}

/* A separator between groups of buttons on a toolbar. Vertical twin of the
   groove, and the reason a row of a dozen icons reads as three groups. */
static inline void ui_toolbar_gap(surface *s, const ui_theme *t,
                                  int x, int y, int h) {
    if (t->modern) { rect(s, x, y + 2, 1, h - 4, t->line); return; }
    rect(s, x, y, 1, h, t->edge_shadow);
    rect(s, x + 1, y, 1, h, t->edge_hi);
}

/* --- a menu bar -----------------------------------------------------------
 *
 * File, Edit, View, Help. The thing most missing from this desktop: every
 * window had its commands hidden behind whichever button the program's
 * author found room for, which means no two programs put anything in the
 * same place and nothing is discoverable.
 *
 * Draws the strip and returns which title the pointer is over, or -1. The
 * program owns which one is open, because a menu bar with an opinion about
 * that cannot be driven by the keyboard. */
#define UI_MENUBAR_H 20

static inline int ui_menubar(surface *s, ui_input *in, const ui_theme *t,
                             int w, const char *const *titles, int count,
                             int open, int *x_out) {
    rect(s, 0, 0, w, UI_MENUBAR_H, t->panel);

    int x = 2, hot = -1;
    for (int i = 0; i < count; i++) {
        int tw = face_w(titles[i], UI_FACE_BODY) + 16;
        int over = ui_hit(in, x, 0, tw, UI_MENUBAR_H);

        if (i == open) {
            rect(s, x, 0, tw, UI_MENUBAR_H, t->accent);
            face_centred(s, x, 0, tw, UI_MENUBAR_H, titles[i],
                         t->accent_fg, UI_FACE_BODY);
        } else {
            if (over) ui_raised(s, t, x, 0, tw, UI_MENUBAR_H);
            face_centred(s, x, 0, tw, UI_MENUBAR_H, titles[i],
                         t->fg, UI_FACE_BODY);
        }

        if (over) hot = i;
        if (x_out) x_out[i] = x;
        x += tw;
    }
    return hot;
}

static inline void ui_statusbar(surface *s, const ui_theme *t,
                                int w, int h, const char *left, const char *right) {
    int y = h - UI_ROW;
    rect(s, 0, y, w, UI_ROW, t->panel);

    /* Two sunk panels rather than a strip of text, which is what a status
       bar has always been: the text sits in something, so an empty one
       still looks like part of the window instead of a gap. */
    int ty = y + (UI_ROW - face_h(UI_FACE_BODY)) / 2;
    int split = right ? w - 140 : w - 3;
    if (split < 60) split = w - 3;

    if (t->modern) {
        /* One line above it and nothing else. The two sunk panels are the
           shape a status bar has had for thirty years and they are two more
           boxes on a window that no longer has any: what the text needs to
           read as a status bar is to be quiet and at the bottom, which a
           line above it already says. */
        rect(s, 0, y, w, 1, t->line);
        if (left) face_draw(s, 10, ty, left, t->dim, UI_FACE_BODY);
        if (right) {
            int rw = face_w(right, UI_FACE_BODY);
            face_draw(s, w - 10 - rw, ty, right, t->dim, UI_FACE_BODY);
        }
        return;
    }

    ui_sunken(s, t, 2, y + 2, split - 4, UI_ROW - 4);
    if (left) face_draw(s, 7, ty, left, t->fg, UI_FACE_BODY);

    if (right) {
        ui_sunken(s, t, split, y + 2, w - split - 2, UI_ROW - 4);
        int rw = face_w(right, UI_FACE_BODY);
        face_draw(s, w - 6 - rw, ty, right, t->fg, UI_FACE_BODY);
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
