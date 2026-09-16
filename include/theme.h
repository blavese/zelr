#pragma once
#include "types.h"

/* How the desktop looks.
 *
 * Everything the window manager draws takes its colours from here rather
 * than from constants, so a settings program in ring 3 can change the look
 * by writing a file. The file is plain "key value" lines, because it has to
 * be editable with the `write` command and readable by a person. */

#define THEME_FILE "/zelr.cfg"

typedef enum {
    WALLPAPER_PLAIN = 0,
    WALLPAPER_GRID,
    WALLPAPER_DOTS,
    WALLPAPER_GRADIENT,
    WALLPAPER_STARS,        /* drifts, so the desktop is never quite still */
    WALLPAPER_WAVES,
    WALLPAPER_WEAVE,
    WALLPAPER_COUNT
} wallpaper_t;

/* True for a wallpaper that has to be redrawn to look right, which is what
   tells the window manager to keep painting when nothing else has changed. */
bool wallpaper_moves(wallpaper_t w);

typedef struct {
    u32 accent;          /* the one loud colour, spent only where it counts */
    u32 desktop;         /* the background behind everything */
    u32 surface;         /* window chrome and the taskbar */
    u32 text;
    u32 text_dim;

    /* Derived, and deliberately not in the file.
     *
     * Three colours cannot describe a surface sitting on top of another
     * surface, so every place that needed one was mixing its own out of
     * lighten() and darken() with a number chosen on the spot. Twenty of
     * those and nothing lines up: the same conceptual layer comes out a
     * different colour in the taskbar than in a menu, and changing the
     * theme moves them by different amounts.
     *
     * These are one step, two steps, an edge and a highlight, worked out
     * once from the colours that are in the file, so the whole desktop
     * stays coherent when somebody changes one of them. */
    u32 raised;          /* one layer up: a title bar, a card, a well */
    u32 overlay;         /* two layers up: menus and anything floating */
    u32 hairline;        /* the edge that separates a layer from the next */
    u32 sheen;           /* a lighter top edge, as if lit from above */
    u32 accent_soft;     /* accent at low strength, for a selected row */
    u32 accent_text;     /* text that has to sit on the accent itself */
    u32 text_mute;       /* one level quieter than text_dim */

    wallpaper_t wallpaper;
    int  corner;         /* window corner radius, 0 for square */
    bool shadows;
    bool animate;        /* menus and highlights fade rather than snap */

    /* The playful behaviour: shaking a window to clear the others away, and
       anything else that is delightful the first time and in the way the
       twentieth. Off is a real setting, not a hidden one. */
    bool quirks;
} theme_t;

void theme_init(void);
const theme_t *theme(void);

/* Reads THEME_FILE if it exists. Returns true if anything changed, so the
   window manager knows to repaint. */
bool theme_reload(void);

/* Writes the current theme back out, which is how the settings program's
   choices survive a reboot. */
bool theme_save(void);

/* The named presets a settings program offers. */
#define THEME_PRESETS 6
const char *theme_preset_name(int i);
u32         theme_preset_accent(int i);
void        theme_apply_preset(int i);
int         theme_current_preset(void);
