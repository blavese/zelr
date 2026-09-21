/* Settings, running in ring 3.
 *
 * This program cannot reach into the window manager and has no way to ask
 * it for anything. What it can do is write a file. The window manager
 * re-reads that file four times a second, and every program built on ui.h
 * reads it at startup, so a choice made here shows up on the desktop behind
 * this window almost immediately without either side knowing anything about
 * the other beyond the format.
 *
 * The file is plain "key value" text on purpose: everything this window does
 * can also be done with the shell's write command, and a settings program
 * that is the only way to change a setting is a settings program you cannot
 * fix when it breaks.
 *
 * --- and it no longer knows what the settings are -------------------------
 *
 * It used to. There was a variable here per setting, a default here per
 * setting, a line in save() per setting and a control per setting, and the
 * comments this file grew are the record of what that cost: a default
 * written twice where the second copy was the one that ran; a key the
 * kernel knew about and this did not, which this deleted from the machine
 * every time it saved; a control that wrote a key nothing parsed.
 *
 * The list lives in kernel/theme.c now and arrives through /sys/settings
 * with a range and a default for each entry. The pages below arrange them
 * and give some of them a better control than a slider, and the Everything
 * page offers all of them whether anybody has arranged them or not. A
 * setting added to the kernel appears in this window without this file
 * being touched.
 */
#include "zelr.h"
#include "ui.h"

#define CFG "/zelr.cfg"
#define SIDEBAR_W 160

/* --- the list of settings, as the kernel describes it --------------------- */

#define MAX_KNOBS 48

typedef struct {
    char key[20];
    char label[26];
    int  value, lo, hi, def;
} knob;

static knob knobs[MAX_KNOBS];
static int  nknobs;

/* The palette, which is not a list of numbers with ranges and so is not in
   that list. `custom` says how much of it this window is responsible for:
   nothing, the accent alone, or the whole thing. */
static u32 accent, ground, surface_c, ink;
static int light = 1, look = 0, preset = 1, custom = 0;

static int saved_at;
static int reset_at;
static int dirty, last_write;

static const char *const PRESET_NAMES[UI_PRESETS] = {
    "Teal", "Indigo", "Amber", "Rose", "Slate", "Lime"
};

#define N_WALLPAPERS 12
#define WALLPAPER_COLS 4
static const char *const WALLPAPERS[N_WALLPAPERS] = {
    "Plain", "Grid", "Dots", "Gradient", "Stars", "Waves", "Weave",
    "Aurora", "Rain", "Orbs", "Pulse", "Bloom"
};

static const char *const HIDE_NAMES[3] = {
    "Never", "When a window needs the room", "Always"
};

/* The sizes offered. Every one is only offered where the mode is this
   kernel's to set, and the card is asked rather than assumed: one it will
   not take leaves the screen exactly as it was. */
static const struct { int w, h; const char *name; } MODES[] = {
    {  800,  600, "800x600" },
    { 1024,  768, "1024x768" },
    { 1280,  720, "1280x720" },
    { 1280, 1024, "1280x1024" },
    { 1440,  900, "1440x900" },
    { 1600,  900, "1600x900" },
    { 1920, 1080, "1920x1080" },
};
#define N_MODES ((int)(sizeof(MODES) / sizeof(MODES[0])))
#define MODE_COLS 3

static const char *const PAGES[] = {
    "Colours", "Dock", "Windows", "Desktop", "Behaviour",
    "Screen", "Everything", "The file", "System", "About"
};
#define N_PAGES 10
static int page = 0;
static int scroll;              /* the Everything page, which is long */

/* --- reading and writing -------------------------------------------------- */

static void copy_into(char *dst, int cap, const char *src, int n) {
    int i = 0;
    for (; i < n && i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int word(const char *b, int i, int n, char *out, int cap) {
    while (i < n && (b[i] == ' ' || b[i] == '\t')) i++;
    int s = i;
    while (i < n && b[i] != ' ' && b[i] != '\n' && b[i] != '\r' && b[i] != '\t') i++;
    copy_into(out, cap, b + s, i - s);
    return i;
}

static int num(const char *s) {
    int v = 0, neg = 0, i = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    for (; s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
    return neg ? -v : v;
}

/* key value low high default label, a line each. The label is the rest of
   the line, because it has spaces in it and nothing after it does. */
static void knobs_load(void) {
    char buf[2048];
    int n = slurp("/sys/settings", buf, sizeof(buf) - 1);
    if (n <= 0) { nknobs = 0; return; }
    buf[n] = 0;

    nknobs = 0;
    int i = 0;
    while (i < n && nknobs < MAX_KNOBS) {
        while (i < n && (buf[i] == '\n' || buf[i] == '\r')) i++;
        if (i >= n) break;
        if (buf[i] == '#') { while (i < n && buf[i] != '\n') i++; continue; }

        knob k;
        char v[12];
        i = word(buf, i, n, k.key, sizeof(k.key));
        i = word(buf, i, n, v, sizeof(v));  k.value = num(v);
        i = word(buf, i, n, v, sizeof(v));  k.lo    = num(v);
        i = word(buf, i, n, v, sizeof(v));  k.hi    = num(v);
        i = word(buf, i, n, v, sizeof(v));  k.def   = num(v);

        while (i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
        int s = i;
        while (i < n && buf[i] != '\n' && buf[i] != '\r') i++;
        copy_into(k.label, sizeof(k.label), buf + s, i - s);

        if (k.key[0]) knobs[nknobs++] = k;
    }
}

static knob *K(const char *key) {
    for (int i = 0; i < nknobs; i++)
        if (!strcmp(knobs[i].key, key)) return &knobs[i];
    return 0;
}

static int put_num(char *out, int at, int v) {
    if (v < 0) { out[at++] = '-'; v = -v; }
    return at + utoa((u32)v, out + at);
}

static int put_hex6(char *out, int at, u32 v) {
    const char *hex = "0123456789abcdef";
    for (int i = 5; i >= 0; i--) out[at++] = hex[(v >> (i * 4)) & 0xF];
    return at;
}

static int line_num(char *out, int at, const char *key, int v) {
    for (int i = 0; key[i]; i++) out[at++] = key[i];
    out[at++] = ' ';
    at = put_num(out, at, v);
    out[at++] = '\n';
    return at;
}

/* With the prefix, so the readers on both sides know it is hex without
   being told. See the note in kernel/theme.c. */
static int line_hex(char *out, int at, const char *key, u32 v) {
    for (int i = 0; key[i]; i++) out[at++] = key[i];
    out[at++] = ' ';
    out[at++] = '0';
    out[at++] = 'x';
    at = put_hex6(out, at, v);
    out[at++] = '\n';
    return at;
}

/* Written whole every time rather than edited in place. The file is a
   kilobyte and a partial rewrite is a way to end up with two values for one
   key.
 *
 * The order matters exactly once: look and light rebuild the palette when
 * the kernel reads them, so anything said about a colour has to come after
 * them or it would be overwritten by the rebuild. */
static void save(void) {
    char out[2048];
    int n = 0;

    const char *header = "# zelr desktop settings\n";
    for (const char *p = header; *p; p++) out[n++] = *p;

    n = line_num(out, n, "look", look);
    n = line_num(out, n, "light", light);

    if (custom == 0) {
        n = line_num(out, n, "preset", preset);
    } else {
        n = line_hex(out, n, "accent", accent);
        if (custom == 2) {
            n = line_hex(out, n, "desktop", ground);
            n = line_hex(out, n, "surface", surface_c);
            n = line_hex(out, n, "text", ink);
        }
    }

    for (int i = 0; i < nknobs; i++)
        n = line_num(out, n, knobs[i].key, knobs[i].value);

    out[n] = 0;
    spit(CFG, out, n);
    saved_at = ticks();
    last_write = ticks();
}

/* A slider dragged across its range is sixty frames, and sixty writes of a
   file is sixty trips to the disk for fifty nine answers nobody sees. So
   the write is held back a few hundredths and then made: fast enough that
   the desktop behind this window follows the slider, slow enough that it is
   not one write a frame. */
static void touch(void) { dirty = 1; }

static void flush(int held) {
    if (!dirty) return;
    if (held && ticks() - last_write < 8) return;
    save();
    dirty = 0;
}

/* The palette as the kernel has it now, rather than as this program would
   work it out: with a colour set by hand the two are different, and the one
   that is right is the kernel's. */
static void colours_load(void) {
    char buf[256];
    int n = slurp("/sys/theme", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        accent    = (u32)ui_cfg_int(buf, "accent", (int)UI_ACCENTS[1]);
        ground    = (u32)ui_cfg_int(buf, "desktop", 0x102542);
        surface_c = (u32)ui_cfg_int(buf, "surface", 0xf4f4f7);
        ink       = (u32)ui_cfg_int(buf, "text", 0x17181c);
        light     = ui_cfg_int(buf, "light", 1);
        look      = ui_cfg_int(buf, "look", 0);
        preset    = ui_cfg_int(buf, "preset", 1);
    }

    /* What this window is responsible for is decided by what is in the
       file, not kept in a variable of its own: a machine rebooted into a
       hand written palette has to come back to this window with the
       palette still in it. */
    char cfg[2048];
    int c = slurp(CFG, cfg, sizeof(cfg) - 1);
    custom = 0;
    if (c > 0) {
        cfg[c] = 0;
        if (ui_cfg_int(cfg, "accent", -1) >= 0)  custom = 1;
        if (ui_cfg_int(cfg, "surface", -1) >= 0) custom = 2;
    }
    if (preset < 0 || preset >= UI_PRESETS) preset = 1;
}

/* Everything back to how a machine that has never been touched arrives.
   The defaults come out of the same table the settings themselves do, so
   there is no second copy of them here to drift from the first. */
static void reset_everything(void) {
    for (int i = 0; i < nknobs; i++) knobs[i].value = knobs[i].def;
    preset = 1;
    light = 1;
    look = 0;
    custom = 0;
    save();
    colours_load();
}

/* --- the two widgets only this program needs ------------------------------ */

static int swatch(surface *s, ui_input *in, int x, int y, u32 colour, int chosen) {
    int d = 34;
    int over = ui_hit(in, x, y, d, d);

    if (chosen || over)
        round_rect(s, x - 3, y - 3, d + 6, d + 6, UI_RADIUS + 2,
                   chosen ? colour : mix(colour, 0, 140));
    round_rect(s, x, y, d, d, UI_RADIUS, colour);

    if (over && in->released) { in->released = 0; return 1; }
    return 0;
}

#define ROW_H  30
#define NAME_W 172

/* A colour, as three sliders and the number they add up to. Not a wheel:
   a wheel needs a saturation and a value beside it and three numbers people
   can already read is the same information in less room. */
static int colour_row(surface *s, ui_input *in, ui_theme *t,
                      int x, int y, int w, const char *label, u32 *c) {
    ui_label(s, t, x, y + 6, label);
    round_rect(s, x + NAME_W - 46, y + 1, 30, 22, UI_RADIUS, *c);

    int part[3] = { (int)((*c >> 16) & 0xFF), (int)((*c >> 8) & 0xFF),
                    (int)(*c & 0xFF) };

    int sw = (w - NAME_W - 76) / 3;
    if (sw > 104) sw = 104;
    if (sw < 40) sw = 40;
    for (int i = 0; i < 3; i++)
        part[i] = ui_slider(s, in, t, x + NAME_W + i * (sw + 8), y + 3, sw,
                            part[i], 0, 255);

    char hex[10];
    hex[0] = '#';
    put_hex6(hex, 1, (u32)((part[0] << 16) | (part[1] << 8) | part[2]));
    hex[7] = 0;
    ui_dim_label(s, t, x + NAME_W + 3 * (sw + 8) + 4, y + 6, hex);

    u32 now = RGB(part[0], part[1], part[2]);
    if (now != *c) { *c = now; return 1; }
    return 0;
}

/* One setting from the list, with the control its range asks for: two
   values is a switch and anything else is a slider. The number is shown
   beside it because a slider with no number is a control you cannot set to
   the same place twice. */
static int knob_row(surface *s, ui_input *in, ui_theme *t,
                    int x, int y, int w, knob *k) {
    if (!k) return y;

    ui_label(s, t, x, y + (ROW_H - FONT_H) / 2, k->label);

    if (k->lo == 0 && k->hi == 1) {
        int v = ui_toggle(s, in, t, x + NAME_W, y + (ROW_H - 20) / 2, 0,
                          k->value);
        if (v != k->value) { k->value = v; touch(); }
        return y + ROW_H;
    }

    char n[12];
    n[put_num(n, 0, k->value)] = 0;
    ui_dim_label(s, t, x + NAME_W, y + (ROW_H - FONT_H) / 2, n);

    int sx = x + NAME_W + 42;
    int sw = w - (NAME_W + 42);
    if (sw > 280) sw = 280;
    if (sw < 60) sw = 60;

    int v = ui_slider(s, in, t, sx, y + (ROW_H - 18) / 2, sw, k->value,
                      k->lo, k->hi);
    if (v != k->value) { k->value = v; touch(); }
    return y + ROW_H;
}

static int row(surface *s, ui_input *in, ui_theme *t,
               int x, int y, int w, const char *key) {
    return knob_row(s, in, t, x, y, w, K(key));
}

/* --- pages ---------------------------------------------------------------- */

static int page_colours(surface *s, ui_input *in, ui_theme *t,
                        int x, int y, int w) {
    y = ui_section(s, t, x, y, w, "Accent");
    for (int i = 0; i < UI_PRESETS; i++) {
        if (swatch(s, in, x + i * 46, y, UI_ACCENTS[i], !custom && i == preset)) {
            preset = i;
            custom = 0;
            accent = UI_ACCENTS[i];
            save();
        }
    }
    ui_dim_label(s, t, x + UI_PRESETS * 46 + UI_GAP, y + 10,
                 custom ? "set by hand" : PRESET_NAMES[preset]);
    y += 34 + UI_GAP;

    if (colour_row(s, in, t, x, y, w, "Any colour at all", &accent)) {
        if (!custom) custom = 1;
        touch();
    }
    y += 30 + UI_PAD;

    y = ui_section(s, t, x, y, w, "Ground");
    {
        int was = light;
        light = ui_toggle(s, in, t, x, y, light ? "Light" : "Dark", light);
        if (light != was) save();
        y += 26;

        static const char *const LOOKS[2] = { "Modern", "Built" };
        for (int i = 0; i < 2; i++) {
            if (ui_button(s, in, t, x + i * 108, y, 102, LOOKS[i])) {
                look = i;
                /* The two looks want different corners and different
                   shadows, and leaving the old ones behind gives a modern
                   desktop with square windows or a built one with a drop
                   shadow on top of every bevel. Someone who wants square
                   modern windows can still say so afterwards; what this
                   avoids is landing there without asking. */
                knob *c = K("corner");
                knob *sh = K("shadows");
                if (c) c->value = i ? 0 : 12;
                if (sh) sh->value = i ? 0 : 1;
                save();
            }
            if (i == look) rect(s, x + i * 108, y + UI_BTN_H - 2, 102, 2,
                                t->accent);
        }
        y += UI_BTN_H + UI_PAD;
    }

    y = ui_section(s, t, x, y, w, "The rest of the palette");
    if (custom < 2) {
        ui_dim_label(s, t, x, y + 4,
                     "Worked out from the accent and the ground above.");
        y += 24;
        if (ui_button(s, in, t, x, y, 190, "Set them by hand")) {
            custom = 2;
            save();
        }
        y += UI_BTN_H + UI_GAP;
        return y;
    }

    int moved = 0;
    moved |= colour_row(s, in, t, x, y, w, "Behind everything", &ground);
    y += 30;
    moved |= colour_row(s, in, t, x, y, w, "Window and dock", &surface_c);
    y += 30;
    moved |= colour_row(s, in, t, x, y, w, "Text", &ink);
    y += 30 + UI_GAP;
    if (moved) touch();

    if (ui_button(s, in, t, x, y, 190, "Work them out again")) {
        custom = accent == UI_ACCENTS[preset] ? 0 : 1;
        save();
        colours_load();
    }
    y += UI_BTN_H + UI_GAP;
    ui_dim_label(s, t, x, y,
                 "These reach the desktop. A window's own colours follow the");
    y += 20;
    ui_dim_label(s, t, x, y, "accent and the ground, not these.");
    return y + 24;
}

static int page_dock(surface *s, ui_input *in, ui_theme *t,
                     int x, int y, int w) {
    y = ui_section(s, t, x, y, w, "Size and place");
    y = row(s, in, t, x, y, w, "dock_h");
    y = row(s, in, t, x, y, w, "dock_gap");
    y = row(s, in, t, x, y, w, "dock_side");
    y = row(s, in, t, x, y, w, "dock_radius");
    ui_dim_label(s, t, x, y + 2,
                 "A gap of nothing welds it to the bottom of the screen.");
    y += 22 + UI_GAP;

    y = ui_section(s, t, x, y, w, "What is on it");
    y = row(s, in, t, x, y, w, "dock_brand");
    y = row(s, in, t, x, y, w, "dock_search");
    y = row(s, in, t, x, y, w, "dock_search_w");
    y = row(s, in, t, x, y, w, "dock_clock");
    y = row(s, in, t, x, y, w, "clock_24");
    y += UI_GAP;

    y = ui_section(s, t, x, y, w, "Getting out of the way");
    {
        knob *k = K("dock_hide");
        int at = k ? k->value : 1;
        for (int i = 0; i < 3; i++) {
            int bw = (w > 520 ? 160 : 132);
            if (ui_button(s, in, t, x + i * (bw + UI_GAP), y, bw,
                          i == 0 ? "Never" : i == 1 ? "When needed" : "Always")
                && k) {
                k->value = i;
                save();
            }
            if (i == at) rect(s, x + i * (bw + UI_GAP), y + UI_BTN_H - 2, bw, 2,
                              t->accent);
        }
        y += UI_BTN_H + UI_GAP;
        ui_dim_label(s, t, x, y, HIDE_NAMES[at < 0 || at > 2 ? 1 : at]);
        y += 24;
    }
    return y;
}

static int page_windows(surface *s, ui_input *in, ui_theme *t,
                        int x, int y, int w) {
    y = ui_section(s, t, x, y, w, "Chrome");
    y = row(s, in, t, x, y, w, "title_h");
    y = row(s, in, t, x, y, w, "border");
    y = row(s, in, t, x, y, w, "corner");
    y = row(s, in, t, x, y, w, "button_w");
    y = row(s, in, t, x, y, w, "button_h");
    y += UI_GAP;

    y = ui_section(s, t, x, y, w, "Behaviour");
    y = row(s, in, t, x, y, w, "shadows");
    y = row(s, in, t, x, y, w, "snap");
    y += UI_GAP;

    ui_dim_label(s, t, x, y, "Alt and an arrow snaps the window in front.");
    y += 20;
    ui_dim_label(s, t, x, y, "Alt and tab walks the stack; alt and d clears it.");
    return y + 24;
}

static int page_desktop(surface *s, ui_input *in, ui_theme *t,
                        int x, int y, int w) {
    y = ui_section(s, t, x, y, w, "Wallpaper");
    {
        knob *k = K("wallpaper");
        int at = k ? k->value : 11;
        for (int i = 0; i < N_WALLPAPERS; i++) {
            int col = i % WALLPAPER_COLS, r = i / WALLPAPER_COLS;
            int bx = x + col * 96, by = y + r * (UI_BTN_H + UI_GAP);
            if (ui_button(s, in, t, bx, by, 90, WALLPAPERS[i]) && k) {
                k->value = i;
                save();
            }
            if (i == at) rect(s, bx, by + UI_BTN_H - 2, 90, 2, t->accent);
        }
        y += (UI_BTN_H + UI_GAP)
             * ((N_WALLPAPERS + WALLPAPER_COLS - 1) / WALLPAPER_COLS) + UI_GAP;
    }

    y = row(s, in, t, x, y, w, "vignette");
    y = row(s, in, t, x, y, w, "glows");
    y += UI_GAP;

    y = ui_section(s, t, x, y, w, "Icons");
    y = row(s, in, t, x, y, w, "desk_icons");
    y = row(s, in, t, x, y, w, "icon_size");
    y = row(s, in, t, x, y, w, "icon_gap");
    return y + UI_GAP;
}

static int page_behaviour(surface *s, ui_input *in, ui_theme *t,
                          int x, int y, int w) {
    y = ui_section(s, t, x, y, w, "Movement");
    y = row(s, in, t, x, y, w, "animate");
    y = row(s, in, t, x, y, w, "anim_ms");
    y = row(s, in, t, x, y, w, "dblclick_ms");
    y += UI_GAP;

    y = ui_section(s, t, x, y, w, "The machine");
    y = row(s, in, t, x, y, w, "volume");
    y = row(s, in, t, x, y, w, "quirks");
    y = row(s, in, t, x, y, w, "autodesktop");
    y += UI_GAP;

    ui_dim_label(s, t, x, y,
                 "Escape leaves the desktop, which is how the console is reached.");
    return y + 26;
}

static int page_screen(surface *s, ui_input *in, ui_theme *t,
                       int x, int y, int w) {
    y = ui_section(s, t, x, y, w, "Screen");
    {
        char screen[192];
        int n = slurp("/sys/screen", screen, (int)sizeof(screen) - 1);
        if (n < 0) n = 0;
        screen[n] = 0;

        int now_w = ui_cfg_int(screen, "width", 0);
        int now_h = ui_cfg_int(screen, "height", 0);
        int settable = ui_cfg_int(screen, "settable", 0);

        char label[48];
        int k = utoa((u32)now_w, label);
        label[k++] = ' '; label[k++] = 'b'; label[k++] = 'y'; label[k++] = ' ';
        k += utoa((u32)now_h, label + k);
        label[k] = 0;
        ui_label(s, t, x, y + 4, label);
        y += 24;

        if (!settable) {
            ui_dim_label(s, t, x, y + 4,
                         "The firmware chose this one and it cannot be changed.");
            y += 26;
        } else {
            knob *kw = K("width"), *kh = K("height");
            for (int i = 0; i < N_MODES; i++) {
                int col = i % MODE_COLS, r = i / MODE_COLS;
                int bx = x + col * 106, by = y + r * (UI_BTN_H + UI_GAP);
                if (ui_button(s, in, t, bx, by, 100, MODES[i].name)
                    && kw && kh) {
                    kw->value = MODES[i].w;
                    kh->value = MODES[i].h;
                    save();
                }
                if (MODES[i].w == now_w && MODES[i].h == now_h)
                    rect(s, bx, by + UI_BTN_H - 2, 100, 2, t->accent);
            }
            y += (UI_BTN_H + UI_GAP)
                 * ((N_MODES + MODE_COLS - 1) / MODE_COLS) + UI_GAP;
            ui_dim_label(s, t, x, y + 4,
                         "A size the card will not take leaves the screen alone.");
            y += 26;
        }
    }

    y = ui_section(s, t, x, y, w, "Start again");
    {
        /* Two presses, because one press that throws away every choice
           somebody has made is a button they will hit once by accident and
           never trust again. */
        int armed = reset_at && ticks() - reset_at < 300;
        if (ui_button(s, in, t, x, y, 210,
                      armed ? "Yes, put it all back" : "Reset everything")) {
            if (armed) { reset_everything(); reset_at = 0; }
            else       { reset_at = ticks(); }
        }
        y += UI_BTN_H + UI_GAP;
        ui_dim_label(s, t, x, y,
                     armed ? "every setting, the colours and the screen"
                           : "every setting back to how the machine arrived");
        y += 26;
    }
    return y;
}

/* Every setting there is, in the order the kernel lists them, whether or
   not a page above has arranged it. This is the page that makes the answer
   to "can I change that" yes rather than "if somebody wrote a control". */
static int page_everything(surface *s, ui_input *in, ui_theme *t,
                           int x, int y, int w, int h) {
    int top = y;
    /* Room for the two lines under the list and for the status bar below
       them, rather than as many rows as the window is tall: the note was
       drawn over the last row and then clipped by the bar. */
    int shown = (h - top - 86) / ROW_H;
    if (shown < 1) shown = 1;
    if (shown > nknobs) shown = nknobs;

    if (scroll > nknobs - shown) scroll = nknobs - shown;
    if (scroll < 0) scroll = 0;

    for (int i = 0; i < shown; i++)
        y = knob_row(s, in, t, x, y, w - UI_SCROLL_W - UI_GAP,
                     &knobs[scroll + i]);

    ui_scrollbar(s, t, x + w - UI_SCROLL_W, top, shown * ROW_H,
                 scroll, shown, nknobs);

    y += UI_GAP;
    ui_dim_label(s, t, x, y,
                 "Every one of these is a line in /zelr.cfg, and the shell's");
    y += 20;
    ui_dim_label(s, t, x, y, "write command can set any of them too.");
    return y + 24;
}

/* Reads the live tree rather than keeping its own numbers, so what is shown
   is what the kernel says now. */
static int show_file(surface *s, ui_theme *t, int x, int y, int w,
                     const char *path, int bottom) {
    char buf[2048];
    int n = slurp(path, buf, sizeof(buf) - 1);
    if (n <= 0) {
        ui_dim_label(s, t, x, y, "(nothing there)");
        return y + FONT_H + UI_GAP;
    }
    buf[n] = 0;

    int line_start = 0;
    for (int i = 0; i <= n; i++) {
        if (buf[i] != '\n' && buf[i] != 0) continue;
        char was = buf[i];
        buf[i] = 0;
        if (buf[line_start]) {
            text(s, x, y, buf + line_start, t->fg);
            y += FONT_H + 2;
        }
        buf[i] = was;
        line_start = i + 1;
        if (y > bottom) break;
    }
    (void)w;
    return y + UI_GAP;
}

/* The file itself, in two columns because it is longer than the window.
 * Shown because this is a machine whose settings are a text file, and a
 * settings window that hides the file is a settings window pretending the
 * file is not the real thing. */
static int page_file(surface *s, ui_input *in, ui_theme *t,
                     int x, int y, int w, int h) {
    (void)in;
    y = ui_section(s, t, x, y, w, CFG);

    char buf[2048];
    int n = slurp(CFG, buf, sizeof(buf) - 1);
    if (n <= 0) {
        ui_dim_label(s, t, x, y, "Not written yet: this machine is as it came.");
        return y + 26;
    }
    buf[n] = 0;

    int cx = x, cy = y;
    int bottom = h - 46;
    int line_start = 0;
    for (int i = 0; i <= n; i++) {
        if (buf[i] != '\n' && buf[i] != 0) continue;
        char was = buf[i];
        buf[i] = 0;
        if (buf[line_start]) {
            text(s, cx, cy, buf + line_start,
                 buf[line_start] == '#' ? t->dim : t->fg);
            cy += FONT_H + 2;
            if (cy > bottom) { cy = y; cx += (w / 2); }
        }
        buf[i] = was;
        line_start = i + 1;
    }
    return h - 40;
}

static int page_system(surface *s, ui_input *in, ui_theme *t,
                       int x, int y, int w, int h) {
    (void)in;
    y = ui_section(s, t, x, y, w, "Memory");
    y = show_file(s, t, x, y, w, "/sys/memory", h - 60);
    y = ui_section(s, t, x, y, w, "Processors");
    y = show_file(s, t, x, y, w, "/sys/cpu", h - 60);
    y = ui_section(s, t, x, y, w, "Devices");
    y = show_file(s, t, x, y, w, "/sys/devices", h - 60);
    return y;
}

static int page_about(surface *s, ui_input *in, ui_theme *t,
                      int x, int y, int w, int h) {
    (void)in;
    y = ui_section(s, t, x, y, w, "This system");
    y = show_file(s, t, x, y, w, "/sys/version", h - 60);
    y = ui_section(s, t, x, y, w, "Uptime");
    y = show_file(s, t, x, y, w, "/sys/uptime", h - 60);
    y = ui_section(s, t, x, y, w, "Network");
    y = show_file(s, t, x, y, w, "/sys/net", h - 60);
    return y;
}

/* --- the window ----------------------------------------------------------- */

int main(void) {
    int win = win_create("Settings", 700, 580);
    if (win < 0) exit(1);
    win_allow_resize(win);

    knobs_load();
    colours_load();

    ui_input in;
    memset(&in, 0, sizeof(in));
    int since_reload = 0;

    for (;;) {
        int w = win_width(win), h = win_height(win);
        u32 *px = win_surface(win);
        if (!px || w <= 0 || h <= 0) break;
        surface s = { px, w, h };

        /* Re-read every frame, so the window recolours itself the moment a
           choice is made rather than on the next launch. */
        ui_theme t = ui_load_theme();

        ui_begin(&in);
        win_event ev;
        int closing = 0;
        while (win_poll(win, &ev)) {
            if (ev.type == WIN_EV_CLOSE) { closing = 1; break; }
            ui_feed(&in, &ev);
        }
        if (closing) break;

        if (in.key == KEY_UP && page > 0) page--;
        if (in.key == KEY_DOWN && page < N_PAGES - 1) page++;
        if (in.scroll) scroll += in.scroll;

        /* The volume can change from the dock while this window is open,
           and a knob table read once at startup would then be showing a
           number the machine has moved on from. Four times a second, which
           is what the window manager gives the file. */
        if (++since_reload > 15 && !dirty && !in.down) {
            since_reload = 0;
            knobs_load();
        }

        fill(&s, t.bg);

        /* The sidebar, which is what makes this a settings application
           rather than one long column of controls. */
        rect(&s, 0, 0, SIDEBAR_W, h, t.panel);
        rect(&s, SIDEBAR_W - 1, 0, 1, h, t.line);
        for (int i = 0; i < N_PAGES; i++) {
            int iy = UI_PAD + i * (UI_ROW + 2);
            if (ui_row(&s, &in, &t, 0, iy, SIDEBAR_W - 1, PAGES[i], 0,
                       i == page) == 1)
                page = i;
        }

        int x = SIDEBAR_W + UI_PAD * 2;
        int cw = w - x - UI_PAD * 2;
        int y = UI_PAD;

        switch (page) {
        case 0: y = page_colours(&s, &in, &t, x, y, cw); break;
        case 1: y = page_dock(&s, &in, &t, x, y, cw); break;
        case 2: y = page_windows(&s, &in, &t, x, y, cw); break;
        case 3: y = page_desktop(&s, &in, &t, x, y, cw); break;
        case 4: y = page_behaviour(&s, &in, &t, x, y, cw); break;
        case 5: y = page_screen(&s, &in, &t, x, y, cw); break;
        case 6: y = page_everything(&s, &in, &t, x, y, cw, h); break;
        case 7: y = page_file(&s, &in, &t, x, y, cw, h); break;
        case 8: y = page_system(&s, &in, &t, x, y, cw, h); break;
        default: y = page_about(&s, &in, &t, x, y, cw, h); break;
        }

        flush(in.down);

        const char *msg = "changes apply as you make them";
        if (saved_at && ticks() - saved_at < 90) msg = "saved to /zelr.cfg";
        ui_statusbar(&s, &t, w, h, msg, PAGES[page]);

        win_commit(win);
        sleep_ms(16);
    }

    win_close(win);
    exit(0);
}
