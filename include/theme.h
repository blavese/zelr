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
    u32 accent;          /* title bars, highlights, the pointer's own colour */
    u32 desktop;         /* the background behind everything */
    u32 surface;         /* window chrome and the taskbar */
    u32 text;
    u32 text_dim;

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
