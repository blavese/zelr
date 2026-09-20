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
    WALLPAPER_AURORA,       /* bands of light leaning across the screen */
    WALLPAPER_RAIN,
    WALLPAPER_ORBS,
    WALLPAPER_PULSE,
    WALLPAPER_BLOOM,        /* deep colour with soft lights in it */
    WALLPAPER_COUNT
} wallpaper_t;

/* Two ways of saying the same things about a surface.
 *
 * BUILT is chrome made out of edges: a lit side, a shaded side, and a hard
 * line where one surface meets the next. It is how a desktop said "this is
 * raised" when it could not afford to say it any other way, and it is exact
 * and legible and this project drew all of it.
 *
 * MODERN is chrome made out of material: a soft corner, a hairline, a pane
 * that lets what is behind it through, and light as a sheen across the top
 * rather than a line down one side.
 *
 * Neither is more correct. They are different centuries, and both are here
 * because somebody should be able to have the one they want. */
typedef enum {
    LOOK_MODERN = 0,
    LOOK_BUILT
} look_t;

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

    /* --- the edges ---------------------------------------------------
     *
     * A second idea of colour, for chrome that is built rather than
     * tinted. A surface here is a plane with a light above and to the
     * left of it, and the four below are what an edge facing the light
     * and an edge facing away from it look like, twice over: strong on
     * the outside of a bevel and soft on the inside.
     *
     * Derived from the surface like everything else, so a theme still
     * only names a few colours and the chrome follows. */
    u32 edge_hi;         /* the outer lit edge, nearly white */
    u32 edge_light;      /* the inner lit edge */
    u32 edge_shadow;     /* the inner shaded edge */
    u32 edge_dark;       /* the outer shaded edge, nearly black */
    u32 well;            /* the ground inside something sunk: a field, a list */

    /* --- material --------------------------------------------------
     *
     * What a pane is made of when the look is MODERN. `glass` is what a
     * floating panel is tinted with before the wallpaper is let through
     * it; `stroke` is the hairline around such a panel, which is what
     * separates it from what is behind it now that no bevel does. */
    u32 glass;
    u32 stroke;

    /* A title bar is a gradient rather than a fill, which is most of what
     * separates chrome that looks built from chrome that looks printed. */
    u32 title_a, title_b, title_fg;
    u32 title_off_a, title_off_b, title_off_fg;

    /* A light ground rather than a dark one. The presets name an accent
       and the temperature of the dark palette; the light one is the same
       accent over near-white, and everything derived below already knows
       which of the two it is looking at. */
    bool light;

    look_t look;
    wallpaper_t wallpaper;
    int  corner;         /* window corner radius, 0 for square */
    bool shadows;
    bool animate;        /* menus and highlights fade rather than snap */

    /* The playful behaviour: shaking a window to clear the others away, and
       anything else that is delightful the first time and in the way the
       twentieth. Off is a real setting, not a hidden one. */
    bool quirks;

    /* Whether the desktop opens by itself when the machine starts. Escape
       leaves it and lands at the console, which is how a machine set this
       way is still a machine somebody can type at. */
    bool autodesktop;

    /* How loud, nought to a hundred. Kept here because this is the file
       the desktop's settings live in and the control is on the panel, not
       because it has anything to do with how things look. */
    int  volume;

    /* The screen size to ask for. Zero for whatever the machine gave us,
       which is the only answer on one whose firmware set the mode. */
    int  want_w, want_h;

    /* --- the rest of the desktop, which used to be constants -------------
     *
     * Every number below was a #define in wm.c, which is to say it was
     * somebody's taste compiled in. None of them is a number there is a
     * right answer to: a dock can reasonably be shorter, or against the
     * top, or welded to the edge rather than floating; a title bar can
     * reasonably have more room in it; icons can reasonably be bigger.
     *
     * They are here because this is the file the desktop reads, and they
     * are plain numbers in it because the whole point of a plain file is
     * that `write` can set the ones no window happens to offer.
     */
    int  dock_h;              /* how tall the dock is */
    int  dock_gap;            /* clear of the screen edge; 0 welds it on */
    int  dock_side;           /* and clear of the sides */
    int  dock_radius;         /* the corner it is cut with */
    bool dock_brand;          /* the name badge at its left */
    bool dock_search;         /* the field in the middle */
    int  dock_search_w;
    bool dock_clock;
    bool clock_24;            /* or twelve hour, with am and pm */
    int  dock_hide;           /* 0 never, 1 when a window wants the room,
                                 2 always, out only when reached for */

    int  title_h;             /* the bar at the top of a window */
    int  border;              /* the frame around one */
    int  button_w, button_h;  /* the three buttons in that bar */
    bool snap;                /* dragging to an edge snaps the window */

    bool desk_icons;          /* the icons on the wallpaper */
    int  icon_size;           /* how big the pictogram is */
    int  icon_gap;            /* the room around it inside its cell */
    int  vignette;            /* how much the wallpaper darkens at the edge */
    bool glows;               /* and whether it has lights in it at all */

    int  anim_ms;             /* how long a fade takes */
    int  dblclick_ms;         /* how close two clicks have to be to be two */
} theme_t;

void theme_init(void);
const theme_t *theme(void);

/* Reads THEME_FILE if it exists. Returns true if anything changed, so the
   window manager knows to repaint. */
bool theme_reload(void);

/* Writes the current theme back out, which is how the settings program's
   choices survive a reboot. */
bool theme_save(void);

/* Changing how loud, and remembering it. The taskbar's control calls this
   rather than writing the file itself, so there is one place that knows
   what the file looks like. */
void theme_set_volume(int percent);

/* The same, without writing the file. Returns whether anything changed, so
   that whoever is dragging knows whether it has something to save. */
bool theme_set_volume_live(int percent);

/* --- every setting that is a plain number --------------------------------
 *
 * One table, rather than a parser arm and a writer line and a control, all
 * three of which had to be remembered separately and one of which was
 * always forgotten. A key the settings window wrote and the kernel did not
 * parse was a setting that silently did nothing, and a key the kernel knew
 * about and the window did not was a setting the window deleted every time
 * it saved. Both of those have happened here.
 *
 * Now the parser walks this, the writer walks this, and a settings program
 * reads it through /sys/settings and can offer a control for every entry
 * without being told what the entries are. */
typedef struct {
    const char *key;
    const char *label;    /* for a program that has to show it to a person */
    u16  at;              /* where it lives in theme_t */
    u8   is_bool;
    int  lo, hi;
    int  def;             /* what a machine nobody has touched arrives with */
} theme_knob;

int               theme_knob_count(void);
const theme_knob *theme_knob_at(int i);
int               theme_knob_get(const theme_knob *k);

/* The named presets a settings program offers. */
#define THEME_PRESETS 6
const char *theme_preset_name(int i);
u32         theme_preset_accent(int i);
void        theme_apply_preset(int i);
int         theme_current_preset(void);
