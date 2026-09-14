/* Settings, running in ring 3.
 *
 * This program cannot reach into the window manager and has no way to ask it
 * for anything. What it can do is write a file. The window manager re-reads
 * that file four times a second, and every program built on ui.h reads it at
 * startup, so a choice made here shows up on the desktop behind this window
 * almost immediately without either side knowing anything about the other
 * beyond the format.
 *
 * The file is plain "key value" text on purpose: everything this window does
 * can also be done with the shell's write command, and a settings program
 * that is the only way to change a setting is a settings program you cannot
 * fix when it breaks. */
#include "zelr.h"
#include "ui.h"

#define CFG "/zelr.cfg"

#define SIDEBAR_W 150

/* What is being edited. These are the same names the kernel's theme.c
   parses; nothing else is shared between the two. */
static int preset = 0;
static int light = 0;
static int wallpaper = 3;
static int corner = 8;
static int shadows = 1;
static int animate = 1;
static int quirks = 1;

static int saved_at;            /* when, so the confirmation can fade */

static const char *const PRESET_NAMES[UI_PRESETS] = {
    "Teal", "Indigo", "Amber", "Rose", "Slate", "Lime"
};

#define N_WALLPAPERS 7
static const char *const WALLPAPERS[N_WALLPAPERS] = {
    "Plain", "Grid", "Dots", "Gradient", "Stars", "Waves", "Weave"
};

static const int CORNERS[4] = { 0, 4, 8, 14 };
static const char *const CORNER_NAMES[4] = { "Square", "Slight", "Round", "Very round" };

static const char *const PAGES[] = { "Appearance", "Desktop", "System", "About" };
#define N_PAGES 4
static int page = 0;

/* --- the config file ------------------------------------------------------ */

static void load(void) {
    char buf[1024];
    int n = slurp(CFG, buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = 0;

    preset    = ui_cfg_int(buf, "preset", 0);
    light     = ui_cfg_int(buf, "light", 0);
    wallpaper = ui_cfg_int(buf, "wallpaper", 3);
    corner    = ui_cfg_int(buf, "corner", 8);
    shadows   = ui_cfg_int(buf, "shadows", 1);
    animate   = ui_cfg_int(buf, "animate", 1);
    quirks    = ui_cfg_int(buf, "quirks", 1);

    if (preset < 0 || preset >= UI_PRESETS) preset = 0;
    if (wallpaper < 0 || wallpaper >= N_WALLPAPERS) wallpaper = 3;
}

static int put_kv(char *out, int at, const char *key, int value) {
    for (int i = 0; key[i]; i++) out[at++] = key[i];
    out[at++] = ' ';
    at += utoa((u32)value, out + at);
    out[at++] = '\n';
    return at;
}

/* Written whole every time rather than edited in place. The file is a few
   hundred bytes and a partial rewrite is a way to end up with two values for
   one key. */
static void save(void) {
    char out[512];
    int n = 0;
    n = put_kv(out, n, "preset", preset);
    n = put_kv(out, n, "light", light);
    n = put_kv(out, n, "wallpaper", wallpaper);
    n = put_kv(out, n, "corner", corner);
    n = put_kv(out, n, "shadows", shadows);
    n = put_kv(out, n, "animate", animate);
    n = put_kv(out, n, "quirks", quirks);
    out[n] = 0;
    spit(CFG, out, n);
    saved_at = ticks();
}

/* --- a swatch, which is the one widget only this program needs ------------ */

static int swatch(surface *s, ui_input *in, int x, int y, u32 colour, int chosen) {
    int d = 34;
    int over = ui_hit(in, x, y, d, d);

    if (chosen || over) {
        round_rect(s, x - 3, y - 3, d + 6, d + 6, UI_RADIUS + 2,
                   chosen ? colour : mix(colour, 0, 140));
    }
    round_rect(s, x, y, d, d, UI_RADIUS, colour);

    if (over && in->released) { in->released = 0; return 1; }
    return 0;
}

/* --- pages ---------------------------------------------------------------- */

static int page_appearance(surface *s, ui_input *in, ui_theme *t, int x, int y, int w) {
    y = ui_section(s, t, x, y, w, "Accent");
    for (int i = 0; i < UI_PRESETS; i++) {
        if (swatch(s, in, x + i * 46, y, UI_ACCENTS[i], i == preset)) {
            preset = i;
            save();
        }
    }
    ui_dim_label(s, t, x + UI_PRESETS * 46 + UI_GAP, y + 10, PRESET_NAMES[preset]);
    y += 34 + UI_PAD * 2;

    y = ui_section(s, t, x, y, w, "Mode");
    int was = light;
    light = ui_toggle(s, in, t, x, y, light ? "Light" : "Dark", light);
    if (light != was) save();
    y += 20 + UI_PAD * 2;

    y = ui_section(s, t, x, y, w, "Window corners");
    for (int i = 0; i < 4; i++) {
        if (ui_button(s, in, t, x + i * 96, y, 90, CORNER_NAMES[i])) {
            corner = CORNERS[i];
            save();
        }
        if (CORNERS[i] == corner)
            rect(s, x + i * 96, y + UI_BTN_H - 2, 90, 2, t->accent);
    }
    y += UI_BTN_H + UI_PAD * 2;

    y = ui_section(s, t, x, y, w, "Effects");
    was = shadows;
    shadows = ui_toggle(s, in, t, x, y, "Shadows under windows", shadows);
    if (shadows != was) save();
    y += 26;

    was = animate;
    animate = ui_toggle(s, in, t, x, y, "Animate menus and highlights", animate);
    if (animate != was) save();
    y += 26;

    return y;
}

static int page_desktop(surface *s, ui_input *in, ui_theme *t, int x, int y, int w) {
    y = ui_section(s, t, x, y, w, "Wallpaper");
    for (int i = 0; i < N_WALLPAPERS; i++) {
        int col = i % 4, row = i / 4;
        if (ui_button(s, in, t, x + col * 96, y + row * (UI_BTN_H + UI_GAP),
                      90, WALLPAPERS[i])) {
            wallpaper = i;
            save();
        }
        if (i == wallpaper)
            rect(s, x + col * 96, y + row * (UI_BTN_H + UI_GAP) + UI_BTN_H - 2,
                 90, 2, t->accent);
    }
    y += (UI_BTN_H + UI_GAP) * 2 + UI_PAD;

    y = ui_section(s, t, x, y, w, "Behaviour");
    int was = quirks;
    quirks = ui_toggle(s, in, t, x, y, "Shake a window to clear the others", quirks);
    if (quirks != was) save();
    y += 26;

    ui_dim_label(s, t, x, y + 4, "Alt and a number picks a window.");
    y += 20;
    ui_dim_label(s, t, x, y + 4, "Drag a window to an edge to snap it there.");
    y += 26;
    return y;
}

/* Reads the live tree rather than keeping its own numbers, so what is shown
   is what the kernel says now. */
static int show_file(surface *s, ui_theme *t, int x, int y, int w, const char *path) {
    char buf[1024];
    int n = slurp(path, buf, sizeof(buf) - 1);
    if (n <= 0) {
        ui_dim_label(s, t, x, y, "(nothing there)");
        return y + FONT_H + UI_GAP;
    }
    buf[n] = 0;

    int line_start = 0;
    for (int i = 0; i <= n; i++) {
        if (buf[i] != '\n' && buf[i] != 0) continue;
        char save_c = buf[i];
        buf[i] = 0;
        if (buf[line_start]) {
            text(s, x, y, buf + line_start, t->fg);
            y += FONT_H + 2;
        }
        buf[i] = save_c;
        line_start = i + 1;
        if (y > 600) break;
    }
    (void)w;
    return y + UI_GAP;
}

static int page_system(surface *s, ui_input *in, ui_theme *t, int x, int y, int w) {
    (void)in;
    y = ui_section(s, t, x, y, w, "Memory");
    y = show_file(s, t, x, y, w, "/sys/memory");

    y = ui_section(s, t, x, y, w, "Processors");
    y = show_file(s, t, x, y, w, "/sys/cpu");

    y = ui_section(s, t, x, y, w, "Devices");
    y = show_file(s, t, x, y, w, "/sys/devices");
    return y;
}

static int page_about(surface *s, ui_input *in, ui_theme *t, int x, int y, int w) {
    (void)in;
    y = ui_section(s, t, x, y, w, "This system");
    y = show_file(s, t, x, y, w, "/sys/version");

    y = ui_section(s, t, x, y, w, "Uptime");
    y = show_file(s, t, x, y, w, "/sys/uptime");

    y = ui_section(s, t, x, y, w, "Network");
    y = show_file(s, t, x, y, w, "/sys/net");
    return y;
}

/* --- the window ----------------------------------------------------------- */

void _start(void) {
    int win = win_create("Settings", 620, 520);
    if (win < 0) exit(1);
    win_allow_resize(win);

    load();
    ui_input in;
    memset(&in, 0, sizeof(in));

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

        fill(&s, t.bg);

        /* The sidebar, which is what makes this a settings application
           rather than one long column of controls. */
        rect(&s, 0, 0, SIDEBAR_W, h, t.panel);
        rect(&s, SIDEBAR_W - 1, 0, 1, h, t.line);
        for (int i = 0; i < N_PAGES; i++) {
            int iy = UI_PAD + i * (UI_ROW + 2);
            if (ui_row(&s, &in, &t, 0, iy, SIDEBAR_W - 1, PAGES[i], 0, i == page) == 1)
                page = i;
        }

        int x = SIDEBAR_W + UI_PAD * 2;
        int cw = w - x - UI_PAD * 2;
        int y = UI_PAD;

        if (page == 0)      y = page_appearance(&s, &in, &t, x, y, cw);
        else if (page == 1) y = page_desktop(&s, &in, &t, x, y, cw);
        else if (page == 2) y = page_system(&s, &in, &t, x, y, cw);
        else                y = page_about(&s, &in, &t, x, y, cw);

        const char *msg = "changes apply as you make them";
        if (saved_at && ticks() - saved_at < 90) msg = "saved to /zelr.cfg";
        ui_statusbar(&s, &t, w, h, msg, PAGES[page]);

        win_commit(win);
        sleep_ms(16);
    }

    win_close(win);
    exit(0);
}
