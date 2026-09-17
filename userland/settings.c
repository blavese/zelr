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
static int autodesktop = 1;
static int scr_w = 0, scr_h = 0;   /* 0 for whatever the machine gave us */

static int saved_at;            /* when, so the confirmation can fade */
static int reset_at;            /* when the reset button was first pressed */

/* --- the apps on the taskbar ---------------------------------------------
 *
 * The same file the window manager reads, in the same format: the program
 * first and the name to show after it, one to a line. Nothing is shared
 * between the two beyond that, exactly as with the theme. */
#define PINS_FILE "/zelr.pins"
#define PINS_MAX  8

typedef struct { char label[16]; char path[32]; } pinrec;
static pinrec pins[PINS_MAX];
static int npins;

static const struct { const char *label, *path; } APPS[] = {
    { "Terminal", "/bin/term" },
    { "Files",    "/bin/files" },
    { "Notes",    "/bin/notes" },
    { "Paint",    "/bin/paint" },
    { "Settings", "/bin/settings" },
    { "Monitor",  "/bin/monitor" },
    { "Music",    "/bin/music" },
    { "Calc",     "/bin/calc" },
};
#define N_APPS ((int)(sizeof(APPS) / sizeof(APPS[0])))

/* The sizes offered. Every one of them is only offered at all where the
   mode is this kernel's to set, and the card is asked rather than assumed:
   one it will not take leaves the screen exactly as it was. */
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

static void copy_into(char *dst, int cap, const char *src) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void pins_default(void) {
    npins = 0;
    for (int i = 0; i < N_APPS && npins < PINS_MAX; i++) {
        copy_into(pins[npins].label, 16, APPS[i].label);
        copy_into(pins[npins].path, 32, APPS[i].path);
        npins++;
    }
}

static void pins_load(void) {
    char buf[512];
    int n = slurp(PINS_FILE, buf, sizeof(buf) - 1);
    if (n <= 0) { pins_default(); return; }   /* never set: the ones it ships */
    buf[n] = 0;

    npins = 0;
    int i = 0;
    while (i < n && npins < PINS_MAX) {
        while (i < n && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r')) i++;
        if (i >= n) break;
        if (buf[i] == '#') { while (i < n && buf[i] != '\n') i++; continue; }

        int k = 0;
        while (i < n && buf[i] != ' ' && buf[i] != '\n' && buf[i] != '\r') {
            if (k < 31) pins[npins].path[k++] = buf[i];
            i++;
        }
        pins[npins].path[k] = 0;

        while (i < n && buf[i] == ' ') i++;
        k = 0;
        while (i < n && buf[i] != '\n' && buf[i] != '\r') {
            if (k < 15) pins[npins].label[k++] = buf[i];
            i++;
        }
        pins[npins].label[k] = 0;

        if (pins[npins].path[0]) npins++;
    }
}

static void pins_write(void) {
    char out[512];
    int n = 0;
    const char *head = "# zelr taskbar, one program a line\n";
    for (int i = 0; head[i]; i++) out[n++] = head[i];

    for (int i = 0; i < npins; i++) {
        for (int k = 0; pins[i].path[k]; k++) out[n++] = pins[i].path[k];
        out[n++] = ' ';
        for (int k = 0; pins[i].label[k]; k++) out[n++] = pins[i].label[k];
        out[n++] = '\n';
    }
    spit(PINS_FILE, out, n);
    saved_at = ticks();
}

static int pinned(const char *path) {
    for (int i = 0; i < npins; i++)
        if (!strcmp(pins[i].path, path)) return i;
    return -1;
}

static const char *const PRESET_NAMES[UI_PRESETS] = {
    "Teal", "Indigo", "Amber", "Rose", "Slate", "Lime"
};

#define N_WALLPAPERS 11
#define WALLPAPER_COLS 4
static const char *const WALLPAPERS[N_WALLPAPERS] = {
    "Plain", "Grid", "Dots", "Gradient", "Stars", "Waves", "Weave",
    "Aurora", "Rain", "Orbs", "Pulse"
};

static const int CORNERS[4] = { 0, 4, 8, 14 };
static const char *const CORNER_NAMES[4] = { "Square", "Slight", "Round", "Very round" };

static const char *const PAGES[] = {
    "Appearance", "Desktop", "Taskbar", "Screen", "System", "About"
};
#define N_PAGES 6
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
    autodesktop = ui_cfg_int(buf, "autodesktop", 1);
    scr_w     = ui_cfg_int(buf, "width", 0);
    scr_h     = ui_cfg_int(buf, "height", 0);

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
    n = put_kv(out, n, "autodesktop", autodesktop);
    n = put_kv(out, n, "width", scr_w);
    n = put_kv(out, n, "height", scr_h);
    out[n] = 0;
    spit(CFG, out, n);
    saved_at = ticks();
}

/* Everything back to how a machine that has never been touched arrives.
 *
 * The taskbar's file is deleted rather than written with the defaults in
 * it: no file at all is what a new machine has, and the window manager
 * reads it the same way, so the two cannot end up disagreeing about what
 * the defaults are. */
static void reset_everything(void) {
    preset = 0;
    light = 0;
    wallpaper = 3;
    corner = 8;
    shadows = 1;
    animate = 1;
    quirks = 1;
    autodesktop = 1;
    scr_w = 0;
    scr_h = 0;
    save();

    unlink(PINS_FILE);
    pins_default();
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
        int col = i % WALLPAPER_COLS, row = i / WALLPAPER_COLS;
        if (ui_button(s, in, t, x + col * 96, y + row * (UI_BTN_H + UI_GAP),
                      90, WALLPAPERS[i])) {
            wallpaper = i;
            save();
        }
        if (i == wallpaper)
            rect(s, x + col * 96, y + row * (UI_BTN_H + UI_GAP) + UI_BTN_H - 2,
                 90, 2, t->accent);
    }
    y += (UI_BTN_H + UI_GAP)
         * ((N_WALLPAPERS + WALLPAPER_COLS - 1) / WALLPAPER_COLS) + UI_PAD;

    y = ui_section(s, t, x, y, w, "Behaviour");
    int was = quirks;
    quirks = ui_toggle(s, in, t, x, y, "Shake a window to clear the others", quirks);
    if (quirks != was) save();
    y += 26;

    was = autodesktop;
    autodesktop = ui_toggle(s, in, t, x, y, "Open the desktop when the machine starts",
                            autodesktop);
    if (autodesktop != was) save();
    y += 26;

    ui_dim_label(s, t, x, y + 4, "Escape leaves it, which is how the console is reached.");
    y += 26;

    ui_dim_label(s, t, x, y + 4, "Alt and a number picks a window.");
    y += 20;
    ui_dim_label(s, t, x, y + 4, "Drag a window to an edge to snap it there.");
    y += 26;
    return y;
}

static int page_taskbar(surface *s, ui_input *in, ui_theme *t, int x, int y, int w) {
    y = ui_section(s, t, x, y, w, "On the taskbar");

    if (!npins) {
        ui_dim_label(s, t, x, y + 6, "nothing, which is allowed");
        y += 28;
    }

    for (int i = 0; i < npins; i++) {
        int bx = x + w - 192;
        ui_label(s, t, x, y + (UI_BTN_H - FONT_H) / 2, pins[i].label);

        /* Drawn whether or not they can do anything, because a row of
           buttons that changes width depending on which row it is in is
           harder to use than one that occasionally does nothing. */
        int up   = ui_button(s, in, t, bx, y, 58, "Up");
        int down = ui_button(s, in, t, bx + 64, y, 64, "Down");
        int off  = ui_button(s, in, t, bx + 134, y, 58, "Off");

        if (up && i > 0) {
            pinrec tmp = pins[i]; pins[i] = pins[i - 1]; pins[i - 1] = tmp;
            pins_write();
        } else if (down && i < npins - 1) {
            pinrec tmp = pins[i]; pins[i] = pins[i + 1]; pins[i + 1] = tmp;
            pins_write();
        } else if (off) {
            for (int k = i; k < npins - 1; k++) pins[k] = pins[k + 1];
            npins--;
            pins_write();
            break;                      /* the list under the loop moved */
        }
        y += UI_BTN_H + UI_GAP;
    }

    y += UI_GAP;
    y = ui_section(s, t, x, y, w, "Add");

    int col = 0, any = 0;
    for (int i = 0; i < N_APPS; i++) {
        if (pinned(APPS[i].path) >= 0) continue;
        any = 1;
        if (ui_button(s, in, t, x + col * 106, y, 100, APPS[i].label)
            && npins < PINS_MAX) {
            copy_into(pins[npins].label, 16, APPS[i].label);
            copy_into(pins[npins].path, 32, APPS[i].path);
            npins++;
            pins_write();
        }
        if (++col == 4) { col = 0; y += UI_BTN_H + UI_GAP; }
    }
    if (!any) ui_dim_label(s, t, x, y + 6, "everything is already on it");
    y += UI_BTN_H + UI_GAP * 2;

    ui_dim_label(s, t, x, y, "Dragging an icon along the taskbar does the same,");
    y += 20;
    ui_dim_label(s, t, x, y, "and right clicking one takes it off.");
    return y + 26;
}

static int page_screen(surface *s, ui_input *in, ui_theme *t, int x, int y, int w) {
    /* --- the screen ------------------------------------------------------ */
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
        label[k++] = ' ';
        label[k++] = 'b';
        label[k++] = 'y';
        label[k++] = ' ';
        k += utoa((u32)now_h, label + k);
        label[k] = 0;
        ui_label(s, t, x, y + 4, label);
        y += 24;

        if (!settable) {
            ui_dim_label(s, t, x, y + 4,
                         "The firmware chose this one and it cannot be changed.");
            y += 26;
        } else {
            for (int i = 0; i < N_MODES; i++) {
                int col = i % MODE_COLS, row = i / MODE_COLS;
                int bx = x + col * 106, by = y + row * (UI_BTN_H + UI_GAP);
                if (ui_button(s, in, t, bx, by, 100, MODES[i].name)) {
                    scr_w = MODES[i].w;
                    scr_h = MODES[i].h;
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

    /* --- and the way back ------------------------------------------------ */
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
                     armed ? "colours, wallpaper, the taskbar and the screen"
                           : "every setting back to how the machine arrived");
        y += 26;
    }

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
    int win = win_create("Settings", 640, 560);
    if (win < 0) exit(1);
    win_allow_resize(win);

    load();
    pins_load();
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
        else if (page == 2) y = page_taskbar(&s, &in, &t, x, y, cw);
        else if (page == 3) y = page_screen(&s, &in, &t, x, y, cw);
        else if (page == 4) y = page_system(&s, &in, &t, x, y, cw);
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
