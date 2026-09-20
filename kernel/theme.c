/* The desktop's appearance, and the file it is kept in.
 *
 * The settings program runs in ring 3 and cannot reach into the window
 * manager, so the two communicate the way two programs normally would: one
 * writes a file and the other reads it. The window manager re-reads it a few
 * times a second, which is cheap for a file this size and means a change
 * shows up while the settings window is still open.
 *
 * The format is deliberately plain text. Someone who does not like any of
 * the presets can set a colour with the shell's `write` command. */
#include "theme.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "fb.h"
#include "sound.h"

static theme_t current;

/* Six presets, differing only in accent and background temperature. The rest
   of the palette is derived, so a new accent never produces unreadable text. */
static const struct {
    const char *name;
    u32 accent, desktop, surface;
} PRESETS[THEME_PRESETS] = {
    { "teal",   RGB(0x2C, 0xC7, 0xA0), RGB(0x12, 0x18, 0x1E), RGB(0x1A, 0x22, 0x2A) },
    { "indigo", RGB(0x6E, 0x8A, 0xE8), RGB(0x14, 0x16, 0x22), RGB(0x1C, 0x1F, 0x2E) },
    { "amber",  RGB(0xE0, 0xA0, 0x3C), RGB(0x1A, 0x16, 0x12), RGB(0x25, 0x20, 0x1A) },
    { "rose",   RGB(0xE0, 0x6A, 0x8C), RGB(0x1B, 0x13, 0x18), RGB(0x27, 0x1C, 0x22) },
    { "slate",  RGB(0x8A, 0x9B, 0xB0), RGB(0x14, 0x17, 0x1B), RGB(0x1E, 0x23, 0x29) },
    { "lime",   RGB(0x9A, 0xD1, 0x4A), RGB(0x14, 0x1A, 0x14), RGB(0x1D, 0x25, 0x1D) },
};

/* Which ones have to be redrawn to look right. Everything else is painted
   once and left alone, which costs nothing at all to have on. */
/* The light ground. One set of four rather than one per preset: what makes
   the presets different is the accent and the temperature of the dark
   background, and a light background that shifted temperature with the
   accent would be six nearly identical off-whites nobody asked for. */
/* A grey, and not an off-white. Chrome that is built out of edges needs a
   surface with room above it and below it: a near-white one has nowhere to
   put a highlight, so every bevel reads as a single grey line and the whole
   desktop goes flat. This is a warm neutral a couple of steps lighter and
   cooler than the grey these desktops used to be, which is the whole of the
   modernising: the construction is the old one, the colour is not. */
/* --- the modern ground ---------------------------------------------------
 *
 * A near white rather than the warm grey above, because the warm grey was
 * chosen to have room above and below it for a bevel and there are no
 * bevels here. What a modern surface needs instead is to be quiet enough
 * that a hairline shows on it. */
#define MODERN_L_SURFACE RGB(0xF4, 0xF4, 0xF7)
#define MODERN_L_TEXT    RGB(0x17, 0x18, 0x1C)
#define MODERN_L_DIM     RGB(0x5D, 0x60, 0x6A)
#define MODERN_D_SURFACE RGB(0x22, 0x24, 0x2B)

/* And the ground behind everything: a deep blue the wallpaper builds its
   lights on top of, rather than a flat slab. */
#define MODERN_DESKTOP   RGB(0x10, 0x25, 0x4A)

#define LIGHT_DESKTOP RGB(0x33, 0x44, 0x52)
#define LIGHT_SURFACE RGB(0xD6, 0xD3, 0xCD)
#define LIGHT_TEXT    RGB(0x12, 0x12, 0x14)
#define LIGHT_DIM     RGB(0x5C, 0x5A, 0x57)

#define DARK_TEXT     RGB(0xE2, 0xE9, 0xEE)
#define DARK_DIM      RGB(0x77, 0x86, 0x93)

/* --- every setting that is a plain number --------------------------------
 *
 * The parser walked a chain of strcmp and the writer walked a list of
 * fields and the settings window had a control per setting, and all three
 * were maintained by hand. The comments in userland/settings.c are a
 * record of what that costs: a key the window wrote that the kernel did
 * not parse did nothing, and a key the kernel had that the window did not
 * was deleted every time the window saved.
 *
 * So there is one list, and it is this one. Adding a setting is adding a
 * line here and a field in theme.h; the file format, the parser, the
 * writer and the settings window all follow without being told.
 *
 * Bounds are part of the entry because a settings file is a file a person
 * edits, and a person who sets a dock two thousand pixels tall should get
 * a very tall dock rather than a desktop with nothing on it.
 */
#define K(field) ((u16)__builtin_offsetof(theme_t, field))

static const theme_knob KNOBS[] = {
    /* --- the dock ------------------------------------------------------ */
    { "dock_h",       "Height",              K(dock_h),        0,  20, 120,  44 },
    { "dock_gap",     "Gap below it",        K(dock_gap),      0,   0,  80,  14 },
    { "dock_side",    "Gap at the sides",    K(dock_side),     0,   0, 400,  16 },
    { "dock_radius",  "Corner",              K(dock_radius),   0,   0,  40,  14 },
    { "dock_brand",   "Show the name",       K(dock_brand),    1,   0,   1,   1 },
    { "dock_search",  "Show the field",      K(dock_search),   1,   0,   1,   1 },
    { "dock_search_w", "Field width",        K(dock_search_w), 0,  80, 700, 260 },
    { "dock_clock",   "Show the clock",      K(dock_clock),    1,   0,   1,   1 },
    { "clock_24",     "Twenty four hour",    K(clock_24),      1,   0,   1,   1 },
    { "dock_hide",    "Tuck away",           K(dock_hide),     0,   0,   2,   1 },

    /* --- windows ------------------------------------------------------- */
    { "title_h",      "Title bar",           K(title_h),       0,  16,  64,  32 },
    { "border",       "Frame",               K(border),        0,   0,   8,   1 },
    { "button_w",     "Button width",        K(button_w),      0,  12,  60,  30 },
    { "button_h",     "Button height",       K(button_h),      0,  10,  48,  24 },
    { "corner",       "Window corner",       K(corner),        0,   0,  24,  12 },
    { "shadows",      "Shadows",             K(shadows),       1,   0,   1,   1 },
    { "snap",         "Snap to the edges",   K(snap),          1,   0,   1,   1 },

    /* --- the desktop --------------------------------------------------- */
    { "desk_icons",   "Icons on it",         K(desk_icons),    1,   0,   1,   1 },
    { "icon_size",    "Icon size",           K(icon_size),     0,  16,  64,  32 },
    { "icon_gap",     "Room around one",     K(icon_gap),      0,   8,  60,  30 },
    { "vignette",     "Darken at the edges", K(vignette),      0,   0, 255, 150 },
    { "glows",        "Lights in it",        K(glows),         1,   0,   1,   1 },
    { "wallpaper",    "Wallpaper",           K(wallpaper),     0,   0,
                                             WALLPAPER_COUNT - 1,
                                             WALLPAPER_BLOOM },

    /* --- behaviour ----------------------------------------------------- */
    { "animate",      "Fade rather than snap", K(animate),     1,   0,   1,   1 },
    { "anim_ms",      "How long a fade takes", K(anim_ms),     0,  20, 600, 120 },
    { "dblclick_ms",  "Double click within",   K(dblclick_ms), 0, 150, 1200, 500 },
    { "quirks",       "Shake to clear",        K(quirks),      1,   0,   1,   1 },
    { "autodesktop",  "Desktop at boot",       K(autodesktop), 1,   0,   1,   1 },

    /* --- the machine --------------------------------------------------- */
    { "volume",       "Volume",              K(volume),        0,   0, 100,  70 },
    { "width",        "Screen width",        K(want_w),        0,   0, 4096,  0 },
    { "height",       "Screen height",       K(want_h),        0,   0, 4096,  0 },
};
#undef K

#define N_KNOBS ((int)(sizeof(KNOBS) / sizeof(KNOBS[0])))

/* The wallpaper is an enum and the table reads it as an int. That is true
   of every enum in C and would stop being true the day one of them is
   given an explicit narrow type, which is the kind of change that breaks
   quietly. */
_Static_assert(sizeof(wallpaper_t) == sizeof(int),
               "the knob table reads the wallpaper as an int");

int theme_knob_count(void) { return N_KNOBS; }

const theme_knob *theme_knob_at(int i) {
    return (i < 0 || i >= N_KNOBS) ? 0 : &KNOBS[i];
}

/* The field, whatever its type. A bool is one byte and an int is four, and
   an enum is an int as far as this is concerned: wallpaper_t is in the
   table above because a wallpaper is a number a person may set and there
   is no reason it should be the one number they cannot. */
static int knob_get(const theme_knob *k) {
    const u8 *base = (const u8 *)&current;
    if (k->is_bool) return *(const bool *)(base + k->at) ? 1 : 0;
    return *(const int *)(base + k->at);
}

static void knob_set(const theme_knob *k, int v) {
    if (v < k->lo) v = k->lo;
    if (v > k->hi) v = k->hi;
    u8 *base = (u8 *)&current;
    if (k->is_bool) *(bool *)(base + k->at) = v != 0;
    else            *(int *)(base + k->at) = v;
}

int theme_knob_get(const theme_knob *k) { return k ? knob_get(k) : 0; }

bool wallpaper_moves(wallpaper_t w) {
    return w == WALLPAPER_STARS  || w == WALLPAPER_WAVES
        || w == WALLPAPER_AURORA || w == WALLPAPER_RAIN
        || w == WALLPAPER_ORBS   || w == WALLPAPER_PULSE;
}

const char *theme_preset_name(int i) {
    if (i < 0 || i >= THEME_PRESETS) return "";
    return PRESETS[i].name;
}

static void derive(void);
static void theme_defaults(void);

u32 theme_preset_accent(int i) {
    if (i < 0 || i >= THEME_PRESETS) return 0;
    return PRESETS[i].accent;
}

void theme_apply_preset(int i) {
    if (i < 0 || i >= THEME_PRESETS) return;
    current.accent = PRESETS[i].accent;

    bool modern = current.look == LOOK_MODERN;

    if (current.light) {
        current.desktop  = modern ? MODERN_DESKTOP : LIGHT_DESKTOP;
        current.surface  = modern ? MODERN_L_SURFACE : LIGHT_SURFACE;
        current.text     = modern ? MODERN_L_TEXT : LIGHT_TEXT;
        current.text_dim = modern ? MODERN_L_DIM : LIGHT_DIM;
    } else {
        current.desktop  = modern ? MODERN_DESKTOP : PRESETS[i].desktop;
        current.surface  = modern ? MODERN_D_SURFACE : PRESETS[i].surface;
        current.text = DARK_TEXT;
        current.text_dim = DARK_DIM;
    }
    derive();
}

int theme_current_preset(void) {
    for (int i = 0; i < THEME_PRESETS; i++)
        if (PRESETS[i].accent == current.accent) return i;
    return -1;                            /* a colour someone set by hand */
}

/* --- the recipe ---------------------------------------------------------
 *
 * A theme names three colours. Everything else on screen is worked out from
 * them here, once, so that a layer looks like the same layer wherever it is
 * drawn and a changed accent moves everything that depends on it together.
 */
static u32 mix(u32 a, u32 b, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    u32 ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    u32 br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    return RGB((ar * (255 - t) + br * t) / 255,
               (ag * (255 - t) + bg * t) / 255,
               (ab * (255 - t) + bb * t) / 255);
}

/* How bright a colour reads, which is not its average: the eye weighs green
   far more than blue. */
static u32 luma(u32 c) {
    return ((((c >> 16) & 0xFF) * 77) + (((c >> 8) & 0xFF) * 151)
            + ((c & 0xFF) * 28)) >> 8;
}

static void derive(void) {
    bool dark = luma(current.surface) < 128;
    const u32 W = RGB(0xFF, 0xFF, 0xFF);
    const u32 K = RGB(0x00, 0x00, 0x00);

    /* Layers lift toward the light in both modes; a light theme just needs
       much less of it before the step is visible. */
    current.raised  = mix(current.surface, W, dark ? 26 : 12);
    current.overlay = mix(current.surface, W, dark ? 42 : 18);
    current.sheen   = mix(current.surface, W, dark ? 92 : 48);

    /* An edge has to go the other way in a light theme or it disappears. */
    current.hairline = dark ? mix(current.surface, W, 44)
                            : mix(current.surface, K, 28);

    current.accent_soft = mix(current.surface, current.accent, dark ? 48 : 40);

    /* Text on the accent has to survive whatever accent somebody picked. */
    current.accent_text = luma(current.accent) > 140
                          ? mix(current.accent, K, 205)
                          : W;

    current.text_mute = mix(current.text_dim, current.surface, 95);

    /* --- the edges ---------------------------------------------------
     *
     * Both modes light from the same direction, so both lift the top left
     * and drop the bottom right. A dark theme needs far more of the lift
     * to show it and far less of the drop before it turns to a hole, and
     * a light one is the other way about. */
    current.edge_hi     = mix(current.surface, W, dark ? 120 : 255);
    current.edge_light  = mix(current.surface, W, dark ? 56  : 96);
    current.edge_shadow = mix(current.surface, K, dark ? 78  : 96);
    current.edge_dark   = mix(current.surface, K, dark ? 150 : 190);

    /* What is inside something sunk. Paper in a light theme, and a hole in
       a dark one: the same relationship to the surface either way. */
    current.well = dark ? mix(current.surface, K, 66)
                        : mix(current.surface, W, 210);

    /* --- material ----------------------------------------------------
     *
     * What a floating pane is tinted with before the wallpaper shows
     * through it, and the hairline that separates it from what is behind.
     * In a dark theme the tint is the surface itself; in a light one it is
     * lifted, because a light panel over a dark wallpaper needs to stay
     * light when a third of the wallpaper is coming through it. */
    current.glass  = dark ? mix(current.surface, W, 10)
                          : mix(current.surface, W, 80);
    current.stroke = dark ? mix(current.surface, W, 60)
                          : mix(current.surface, K, 26);

    /* A title bar. Built, it is a gradient along the bar rather than down
       it, which is the way every desktop that did this well did it: down a
       22 pixel bar a vertical gradient has nowhere to go and reads as a
       smudge. Modern, it is not a coloured bar at all. It is the same
       material as the window under it, and what says which window is in
       front is the text and the shadow rather than a band of accent. */
    if (current.look == LOOK_MODERN) {
        current.title_a  = current.raised;
        current.title_b  = current.raised;
        current.title_fg = current.text;
    } else {
        current.title_a  = current.accent;
        current.title_b  = mix(current.accent, W, 58);
        current.title_fg = current.accent_text;
    }

    /* An unfocused bar is the same shape drained of the colour, so the one
       in front is obvious without either being hard to read. */
    /* Drained rather than darkened. A window that is merely not in front is
       still a window somebody is reading, and a title bar that goes nearly
       black to say so is louder about being unfocused than the focused one
       is about being focused. */
    if (current.look == LOOK_MODERN) {
        current.title_off_a  = current.surface;
        current.title_off_b  = current.surface;
        current.title_off_fg = current.text_dim;
    } else {
        current.title_off_a  = mix(current.surface, dark ? W : K, dark ? 10 : 34);
        current.title_off_b  = mix(current.surface, W, dark ? 30 : 70);
        current.title_off_fg = current.text_dim;
    }
}

const theme_t *theme(void) { return &current; }

void theme_init(void) {
    /* The modern look is what a machine with no settings file gets.
     *
       It was the built one, and the note here used to explain why: a bevel
       already says which way is up, and a drop shadow on top of one is two
       answers to the same question. That reasoning is still right about
       bevels. It is the bevels that are the choice, and having made it once
       the whole desktop was locked to a particular decade.

       So the look is a setting now, and both sides of it are complete. A
       machine set to BUILT gets exactly what this used to be, down to the
       square corners and the absence of a shadow. */
    current.look = LOOK_MODERN;
    current.light = true;
    theme_apply_preset(1);
    theme_defaults();
    derive();
    theme_reload();
}

/* --- the file ----------------------------------------------------------- */

/* What a machine that has never been touched arrives with. Written out
   here rather than beside each field, because the settings window's Reset
   has to produce exactly this and used to do it from its own copy of the
   list: the two drifted, and resetting a machine left it on a desktop
   neither side thought was the default. */
static void theme_defaults(void) {
    for (int i = 0; i < N_KNOBS; i++) knob_set(&KNOBS[i], KNOBS[i].def);
}

static u32 parse_hex(const char *s) {
    u32 v = 0;
    /* With or without the prefix: a file written before colours carried
       one still has to load. */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        u32 d;
        if (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else break;
        v = v * 16 + d;
    }
    return v;
}

static u32 parse_dec(const char *s) {
    u32 v = 0;
    for (int i = 0; s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (u32)(s[i] - '0');
    return v;
}

static void apply(const char *key, const char *value) {
    /* Anything in the table is a number with bounds, and that is the whole
       of what reading it takes. */
    for (int i = 0; i < N_KNOBS; i++)
        if (!strcmp(key, KNOBS[i].key)) {
            knob_set(&KNOBS[i], (int)parse_dec(value));
            return;
        }

    if (!strcmp(key, "accent"))      current.accent = parse_hex(value);
    else if (!strcmp(key, "desktop")) current.desktop = parse_hex(value);
    else if (!strcmp(key, "surface")) current.surface = parse_hex(value);
    else if (!strcmp(key, "text"))    current.text = parse_hex(value);
    else if (!strcmp(key, "text_dim")) current.text_dim = parse_hex(value);
    else if (!strcmp(key, "look")) {
        /* The look changes what the palette is derived from, so the preset
           has to be applied again rather than only the flag being set. */
        current.look = parse_dec(value) ? LOOK_BUILT : LOOK_MODERN;
        int at = theme_current_preset();
        theme_apply_preset(at >= 0 ? at : 1);
    }
    else if (!strcmp(key, "preset"))  theme_apply_preset((int)parse_dec(value));
    else if (!strcmp(key, "light")) {
        /* The whole palette follows from this, so whichever order the file
           happens to be in, the ground is rebuilt when it is read. */
        current.light = parse_dec(value) != 0;
        int at = theme_current_preset();
        theme_apply_preset(at >= 0 ? at : 0);
    }
}

bool theme_reload(void) {
    /* The file is a key a line and there are now some thirty of them, so
       this is a kilobyte and a half rather than the half kilobyte it was.
       A short read here is not an error anywhere: it simply stops part way
       down the file, and what it stops before is whatever happens to be at
       the bottom of it. */
    char buf[2048];
    int n = vfs_read(THEME_FILE, buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    buf[n] = 0;

    theme_t before = current;

    /* "key value" a line at a time. A line the parser does not recognise is
       skipped rather than treated as an error, so a config written by a
       later version still loads. */
    int i = 0;
    while (i < n) {
        char key[32], value[32];
        int k = 0, v = 0;

        while (i < n && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r')) i++;
        if (i >= n) break;
        if (buf[i] == '#') { while (i < n && buf[i] != '\n') i++; continue; }

        while (i < n && buf[i] != ' ' && buf[i] != '\n' && buf[i] != '\r') {
            if (k < (int)sizeof(key) - 1) key[k++] = buf[i];
            i++;
        }
        key[k] = 0;
        while (i < n && buf[i] == ' ') i++;
        while (i < n && buf[i] != '\n' && buf[i] != '\r') {
            if (v < (int)sizeof(value) - 1) value[v++] = buf[i];
            i++;
        }
        value[v] = 0;

        if (k) apply(key, value);
    }

    /* The file names three colours; the rest of the palette follows from
       them, so it is rebuilt here rather than left over from before. */
    derive();

    return memcmp(&before, &current, sizeof(theme_t)) != 0;
}

/* The level, applied but not written down.
 *
 * Dragging a slider changes it once for every pixel the hand travels, and
 * writing it down is a write to the disk. Two hundred of those inside one
 * gesture is a slider that lags behind the pointer for a reason that has
 * nothing to do with sound. Whoever is dragging saves once, at the end. */
bool theme_set_volume_live(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == current.volume) return false;
    current.volume = percent;
    sound_set_volume((u32)percent);
    return true;
}

void theme_set_volume(int percent) {
    if (theme_set_volume_live(percent)) theme_save();
}

static int put_hex(char *out, u32 v) {
    const char *hex = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 5; i >= 0; i--) out[2 + 5 - i] = hex[(v >> (i * 4)) & 0xF];
    return 8;
}

static int put_num(char *out, u32 v) {
    char tmp[12];
    int n = 0;
    if (!v) { out[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static int put_key(char *out, int n, const char *key) {
    for (const char *p = key; *p; p++) out[n++] = *p;
    out[n++] = ' ';
    return n;
}

bool theme_save(void) {
    char out[1536];
    int n = 0;

    const char *header =
        "# zelr desktop settings\n"
        "# colours are 0xRRGGBB; everything else is a plain number\n"
        "# a line this kernel does not know is kept as a comment would be:\n"
        "# skipped, so a file from a later version still loads here\n";
    for (const char *p = header; *p; p++) out[n++] = *p;

    /* The palette first, because look and light rebuild it and a colour
       set by hand has to be able to win. Whichever way round the file is
       read the last word on a colour is the colour line itself. */
    int at = theme_current_preset();
    n = put_key(out, n, "look");
    n += put_num(out + n, (u32)(current.look == LOOK_BUILT ? 1 : 0));
    out[n++] = '\n';
    n = put_key(out, n, "light");
    n += put_num(out + n, current.light ? 1u : 0u);
    out[n++] = '\n';

    if (at >= 0) {
        n = put_key(out, n, "preset");
        n += put_num(out + n, (u32)at);
        out[n++] = '\n';
    } else {
        /* A palette nobody can name still has to survive a reboot, and all
           four of it: writing the accent and the ground and leaving the
           surface behind was a palette that came back half itself. */
        struct { const char *key; u32 value; } c[] = {
            { "accent",  current.accent },
            { "desktop", current.desktop },
            { "surface", current.surface },
            { "text",    current.text },
            { "text_dim", current.text_dim },
        };
        for (u32 i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
            n = put_key(out, n, c[i].key);
            n += put_hex(out + n, c[i].value);
            out[n++] = '\n';
        }
    }

    for (int i = 0; i < N_KNOBS; i++) {
        n = put_key(out, n, KNOBS[i].key);
        n += put_num(out + n, (u32)knob_get(&KNOBS[i]));
        out[n++] = '\n';
    }

    return vfs_write(THEME_FILE, out, (u32)n);
}
