/* A file manager.
 *
 * The shell could already do all of this, which is exactly why it is worth
 * having: a desktop where the only way to see what is on the disk is to type
 * `ls` is not a desktop, it is a terminal with wallpaper.
 *
 * Everything here goes through the ordinary file syscalls. There is no
 * privileged path and nothing it can do that a program written by anyone
 * else could not, which is the property worth keeping.
 *
 * Opening a file means starting the program that knows what to do with it,
 * and telling that program which file. Until there was one argument a task
 * could be started with, the second half of that was impossible and this
 * could only open folders.
 */
#include "zelr.h"
#include "ui.h"

#define MAX_ENTRIES 512
#define NAME_MAX    64          /* as wide as the kernel's own names */
#define PATH_MAX    256

/* What a file is, as far as opening it goes. */
#define KIND_DIR    0
#define KIND_TEXT   1
#define KIND_SOUND  2
#define KIND_PROG   3
#define KIND_OTHER  4

typedef struct {
    char name[NAME_MAX];
    u32  size;
    u32  is_dir;
    int  kind;
} entry;

static entry entries[MAX_ENTRIES];
static int   count;                     /* after the filter */
static int   total;                     /* everything in the directory */
static u32   total_bytes;

static char  cwd[PATH_MAX] = "/home";
static int   selected = -1;
static int   first_row;

static char  status[128];
static char  clip_path[PATH_MAX];
static int   clip_is_cut;

static char  filter_buf[32];
static ui_field filter_field;

/* --- the menu bar ---------------------------------------------------------
 *
 * Every command this program has, in the place every program puts them.
 * The toolbar keeps the four worth a button of their own; everything else
 * was previously reachable only by right clicking something, which is to
 * say it was not reachable by anyone who did not already know.
 *
 * The program owns which title is open rather than the widget, because a
 * menu bar with its own opinion about that cannot be driven by a keyboard
 * later on. */
static const char *const MENU_TITLES[] = { "File", "Edit", "View", "Help" };
#define N_MENUS 4

static const char *const FILE_ITEMS[] = {
    "Open", "New folder", "Rename", "Delete", "Close"
};
static const char *const EDIT_ITEMS[] = { "Copy", "Cut", "Paste" };
static const char *const VIEW_ITEMS[] = { "Refresh", "Home", "Root" };
static const char *const HELP_ITEMS[] = { "About Files" };

static const char *const *MENU_ITEMS[N_MENUS] = {
    FILE_ITEMS, EDIT_ITEMS, VIEW_ITEMS, HELP_ITEMS
};
static const int MENU_COUNTS[N_MENUS] = { 5, 3, 3, 1 };
static const int MENU_WIDTHS[N_MENUS] = { 130, 110, 110, 130 };

static int bar_open = -1;              /* which title is down, or none */
static int bar_x[N_MENUS];

/* --- paths ---------------------------------------------------------------- */

static void join(char *out, const char *dir, const char *name) {
    int i = 0;
    for (; dir[i] && i < PATH_MAX - 2; i++) out[i] = dir[i];
    if (i > 1 && out[i - 1] == '/') i--;          /* no "//" except at the root */
    out[i++] = '/';
    for (int j = 0; name[j] && i < PATH_MAX - 1; j++) out[i++] = name[j];
    out[i] = 0;
}

static void parent_of(char *path) {
    int n = strlen(path);
    if (n <= 1) return;
    if (path[n - 1] == '/') n--;
    while (n > 1 && path[n - 1] != '/') n--;
    if (n > 1) n--;
    path[n ? n : 1] = 0;
    if (!path[0]) { path[0] = '/'; path[1] = 0; }
}

static int ends_with(const char *name, const char *ext) {
    int n = strlen(name), e = strlen(ext);
    if (n <= e) return 0;
    for (int i = 0; i < e; i++) {
        char a = name[n - e + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Which program should have this, decided from the name and where it is.
   Nothing here reads the file to find out: a name is what a person sees and
   a guess that disagrees with the name is a guess nobody can correct. */
static int kind_of(const char *dir, const char *name, int is_dir) {
    if (is_dir) return KIND_DIR;
    if (!strncmp(dir, "/bin", 4)) return KIND_PROG;
    if (ends_with(name, ".wav")) return KIND_SOUND;
    if (ends_with(name, ".txt") || ends_with(name, ".md")
        || ends_with(name, ".cfg") || ends_with(name, ".log")
        || ends_with(name, ".c") || ends_with(name, ".h")
        || ends_with(name, ".sh")) return KIND_TEXT;
    return KIND_OTHER;
}

static const char *kind_name(int k) {
    switch (k) {
        case KIND_DIR:   return "folder";
        case KIND_TEXT:  return "text";
        case KIND_SOUND: return "sound";
        case KIND_PROG:  return "program";
        default:         return "file";
    }
}

/* --- the listing ---------------------------------------------------------- */

static void say(const char *msg) {
    strncpy(status, msg, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;
}

/* Directories first, then files, each alphabetical. readdir gives whatever
   order the filesystem stores, which for FAT is creation order, and a
   directory listing that changes order as files are written is unusable. */
static void sort_entries(void) {
    for (int i = 1; i < count; i++) {
        entry key = entries[i];
        int j = i - 1;
        while (j >= 0) {
            entry *a = &entries[j];
            int after = (a->is_dir != key.is_dir)
                        ? (!a->is_dir && key.is_dir)
                        : (strcmp(a->name, key.name) > 0);
            if (!after) break;
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

/* True when a name passes whatever has been typed into the filter. Case is
   ignored, because nobody typing three letters into a box means them in a
   particular case. */
static int matches(const char *name) {
    if (!filter_buf[0]) return 1;
    for (int i = 0; name[i]; i++) {
        int k = 0;
        while (filter_buf[k]) {
            char a = name[i + k], b = filter_buf[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            k++;
        }
        if (!filter_buf[k]) return 1;
    }
    return 0;
}

static void reload(void) {
    count = 0;
    total = 0;
    total_bytes = 0;
    selected = -1;
    first_row = 0;

    zelr_stat st;
    for (int i = 0; i < MAX_ENTRIES && readdir(cwd, i, &st) == 1; i++) {
        total++;
        if (!st.is_dir) total_bytes += st.size;
        if (!matches(st.name)) continue;

        entry *e = &entries[count++];
        strncpy(e->name, st.name, NAME_MAX - 1);
        e->name[NAME_MAX - 1] = 0;
        e->size = st.size;
        e->is_dir = st.is_dir;
        e->kind = kind_of(cwd, e->name, (int)st.is_dir);
    }
    sort_entries();
}

static void go_to(const char *where) {
    strncpy(cwd, where, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = 0;
    filter_buf[0] = 0;
    filter_field.len = 0;
    filter_field.cursor = 0;
    reload();
}

static void go_up(void) {
    parent_of(cwd);
    reload();
}

/* --- opening things -------------------------------------------------------
 *
 * The whole point of the argument a task can now be started with. Each kind
 * goes to the program that knows about it, and that program is told which
 * file rather than having to guess or ask.
 */
static void open_with(int index, const char *program) {
    if (index < 0 || index >= count) return;
    char target[PATH_MAX];
    join(target, cwd, entries[index].name);

    if (spawn_arg(program, target) < 0) say("could not start it");
    else say("opened");
}

static void open_entry(int index) {
    if (index < 0 || index >= count) return;
    entry *e = &entries[index];

    if (e->is_dir) {
        char next[PATH_MAX];
        join(next, cwd, e->name);
        go_to(next);
        return;
    }

    char target[PATH_MAX];
    join(target, cwd, e->name);

    switch (e->kind) {
        case KIND_SOUND: open_with(index, "/bin/music"); break;
        case KIND_PROG:
            if (spawn(target) < 0) say("would not run");
            else say("started");
            break;
        default: open_with(index, "/bin/notes"); break;
    }
}

/* --- moving files around --------------------------------------------------- */

/* Copying is a read and a write, because there is no syscall that does it in
   one and adding one would put a loop in the kernel that belongs here. */
static int copy_file(const char *from, const char *to) {
    static char buf[65536];
    int n = slurp(from, buf, sizeof(buf));
    if (n < 0) return -1;
    return spit(to, buf, n) < 0 ? -1 : 0;
}

static void do_paste(void) {
    if (!clip_path[0]) { say("nothing copied"); return; }

    const char *leaf = clip_path;
    for (const char *p = clip_path; *p; p++) if (*p == '/') leaf = p + 1;

    char dest[PATH_MAX];
    join(dest, cwd, leaf);
    if (!strcmp(dest, clip_path)) { say("already here"); return; }

    if (copy_file(clip_path, dest) < 0) { say("copy failed"); return; }
    if (clip_is_cut) {
        unlink(clip_path);
        clip_path[0] = 0;
    }
    say(clip_is_cut ? "moved" : "copied");
    reload();
}

static void do_delete(void) {
    if (selected < 0) { say("nothing selected"); return; }
    char target[PATH_MAX];
    join(target, cwd, entries[selected].name);

    int rc = entries[selected].is_dir ? rmdir(target) : unlink(target);
    say(rc < 0 ? (entries[selected].is_dir ? "directory is not empty" : "delete failed")
               : "deleted");
    reload();
}

/* --- the window ----------------------------------------------------------- */

#define TOOLBAR_H 40
#define CRUMB_H   28
#define SIDE_W    124

static const char *const menu_items[] = {
    "Open", "Edit", "Play", "Copy", "Cut", "Rename", "Delete"
};
#define MENU_COUNT 7

/* The places worth one click. /usb is listed whether or not a stick is in,
   because a list that changes shape under the pointer is worse than a row
   that says there is nothing there. */
static const struct { const char *label, *path; } PLACES[] = {
    { "Home",      "/home" },
    { "Documents", "/doc" },
    { "Programs",  "/bin" },
    { "System",    "/sys" },
    { "Temp",      "/tmp" },
    { "Stick",     "/usb" },
    { "Root",      "/" },
};
#define N_PLACES ((int)(sizeof(PLACES) / sizeof(PLACES[0])))

/* A square in the colour of whatever kind of thing this is. Five colours is
   enough to tell a folder from a program from a sound at a glance, which is
   the whole job an icon does in a list this size. */
static u32 kind_tint(const ui_theme *t, int kind) {
    switch (kind) {
        case KIND_DIR:   return t->accent;
        case KIND_TEXT:  return mix(t->fg, t->accent, 60);
        case KIND_SOUND: return RGB(0x9A, 0x86, 0xE8);
        case KIND_PROG:  return RGB(0xE0, 0xA0, 0x3C);
        default:         return t->dim;
    }
}

void _start(void) {
    int win = win_create("Files", 780, 520);
    if (win < 0) exit(1);
    win_allow_resize(win);

    ui_input in;
    memset(&in, 0, sizeof(in));

    memset(&filter_field, 0, sizeof(filter_field));
    filter_field.buf = filter_buf;
    filter_field.cap = sizeof(filter_buf);

    /* Started on a folder: show that one. */
    char wanted[PATH_MAX];
    if (getarg(wanted, sizeof(wanted)) > 0 && wanted[0]) go_to(wanted);
    else reload();
    say("");

    /* Rename happens in place: the row turns into a field rather than a
       dialog appearing somewhere else on the screen. */
    int renaming = -1;
    char rename_buf[NAME_MAX];
    ui_field rename_field;
    memset(&rename_field, 0, sizeof(rename_field));
    rename_field.buf = rename_buf;
    rename_field.cap = sizeof(rename_buf);

    int menu_open = 0, menu_x = 0, menu_y = 0;
    int last_filter_len = 0;

    for (;;) {
        int w = win_width(win), h = win_height(win);
        u32 *px = win_surface(win);
        if (!px || w <= 0 || h <= 0) break;
        surface s = { px, w, h };
        ui_theme t = ui_load_theme();

        ui_begin(&in);
        win_event ev;
        int closing = 0;
        while (win_poll(win, &ev)) {
            if (ev.type == WIN_EV_CLOSE) { closing = 1; break; }
            ui_feed(&in, &ev);
        }
        if (closing) break;

        /* --- keys ------------------------------------------------------- */
        if (renaming >= 0) {
            if (in.key == '\n') {
                char from[PATH_MAX], to[PATH_MAX];
                join(from, cwd, entries[renaming].name);
                join(to, cwd, rename_buf);
                if (rename_buf[0] && strcmp(from, to)) {
                    if (copy_file(from, to) == 0) { unlink(from); say("renamed"); }
                    else say("rename failed");
                }
                renaming = -1;
                reload();
            } else if (in.key == 27) {
                renaming = -1;
            } else {
                ui_field_key(&rename_field, in.key);
            }
            in.key = 0;
        } else if (filter_field.focused && in.key) {
            if (in.key == 27) { filter_buf[0] = 0; filter_field.len = 0;
                                filter_field.cursor = 0; filter_field.focused = 0; }
            else ui_field_key(&filter_field, in.key);
            in.key = 0;
        } else if (in.key) {
            if (in.key == KEY_UP && selected > 0) selected--;
            else if (in.key == KEY_DOWN && selected < count - 1) selected++;
            else if (in.key == '\n') open_entry(selected);
            else if (in.key == '\b') go_up();
            else if (in.key == KEY_DELETE) do_delete();
        }

        /* Typing in the filter changes what is listed, as it is typed. */
        if (filter_field.len != last_filter_len) {
            last_filter_len = filter_field.len;
            reload();
        }

        /* --- paint ------------------------------------------------------ */
        fill(&s, t.bg);

        /* The menu bar owns the top of the window, and everything else is
           measured from under it. */
        int top = UI_MENUBAR_H;
        int hot_title = ui_menubar(&s, &in, &t, w, MENU_TITLES, N_MENUS,
                                   bar_open, bar_x);

        /* Once one is open, sliding along the bar moves to the next, which
           is what a menu bar has always done and what makes it browsable. */
        if (bar_open >= 0 && hot_title >= 0 && hot_title != bar_open)
            bar_open = hot_title;

        if (in.released && hot_title >= 0) {
            in.released = 0;
            bar_open = (bar_open == hot_title) ? -1 : hot_title;
        }

        int tool_y = top;

        /* The places, down the left. */
        rect(&s, 0, top, SIDE_W, h - top, t.panel);
        face_draw(&s, UI_PAD, tool_y + TOOLBAR_H + UI_PAD, "PLACES",
                  t.dim, UI_FACE_SMALL);

        for (int i = 0; i < N_PLACES; i++) {
            int iy = tool_y + TOOLBAR_H + UI_PAD + 20 + i * (UI_ROW + 2);
            int here = !strcmp(cwd, PLACES[i].path);
            if (ui_row(&s, &in, &t, 2, iy, SIDE_W - 6, PLACES[i].label, 0, here) == 1)
                go_to(PLACES[i].path);
        }

        int cx = SIDE_W;
        int cw = w - SIDE_W;

        rect(&s, 0, tool_y, w, TOOLBAR_H, t.panel);
        ui_groove(&s, &t, 0, tool_y + TOOLBAR_H - 2, w, 2);
        ui_toolbar_gap(&s, &t, SIDE_W - 3, tool_y + 3, TOOLBAR_H - 8);

        int btn_y = tool_y + (TOOLBAR_H - UI_BTN_H) / 2;
        int bx = cx + UI_PAD;
        if (ui_button(&s, &in, &t, bx, btn_y, 48, "Up")) go_up();
        bx += 48 + UI_GAP;
        if (ui_button(&s, &in, &t, bx, btn_y, 60, "Open"))
            open_entry(selected);
        bx += 60 + UI_GAP;
        if (ui_button(&s, &in, &t, bx, btn_y, 86, "New folder")) {
            char target[PATH_MAX];
            join(target, cwd, "new folder");
            say(mkdir(target) < 0 ? "could not create" : "created");
            reload();
        }
        bx += 86 + UI_GAP;
        if (ui_button(&s, &in, &t, bx, btn_y, 60, "Paste"))
            do_paste();

        /* The filter, against the right hand end of the toolbar. */
        int fw = 150;
        if (cw > 460)
            ui_field_draw(&s, &in, &t, w - UI_PAD - fw, btn_y,
                          fw, &filter_field, "filter");

        /* The path, in a well of its own: it is something being read, not
           something to press. */
        int crumb_y = tool_y + TOOLBAR_H;
        rect(&s, cx, crumb_y, cw, CRUMB_H, t.bg);
        ui_well(&s, &t, cx + 4, crumb_y + 3, cw - 8, CRUMB_H - 6, 0, 0, 0, 0);
        face_draw(&s, cx + 10, crumb_y + (CRUMB_H - face_h(UI_FACE_BODY)) / 2,
                  cwd, t.fg, UI_FACE_BODY);

        int list_y = crumb_y + CRUMB_H;
        int list_h = h - list_y - UI_ROW;

        /* Sunk, with paper at the bottom of it. What is in a window is
           either something to press or something to look at, and this is
           the shape that says which. */
        ui_well(&s, &t, cx + 4, list_y, cw - 8, list_h - 4, 0, 0, 0, 0);
        list_y += 2;
        list_h -= 8;

        int shown = list_h / UI_ROW;
        if (shown < 1) shown = 1;

        if (selected >= 0) {
            if (selected < first_row) first_row = selected;
            if (selected >= first_row + shown) first_row = selected - shown + 1;
        }
        if (in.scroll) first_row += in.scroll * 3;
        if (first_row > count - shown) first_row = count - shown;
        if (first_row < 0) first_row = 0;

        if (count == 0)
            face_draw(&s, cx + UI_PAD, list_y + UI_PAD,
                      filter_buf[0] ? "nothing matches" : "(empty)",
                      t.dim, UI_FACE_BODY);

        for (int i = 0; i < shown && first_row + i < count; i++) {
            int idx = first_row + i;
            int ry = list_y + i * UI_ROW;
            entry *e = &entries[idx];

            if (renaming == idx) {
                rect(&s, cx, ry, cw, UI_ROW, t.bg);
                ui_field_draw(&s, &in, &t, cx + UI_PAD, ry - 2,
                              cw - UI_PAD * 2 - UI_SCROLL_W, &rename_field, "name");
                continue;
            }

            int over = ui_hit(&in, cx, ry, cw - UI_SCROLL_W, UI_ROW);
            if (idx == selected)
                rect(&s, cx, ry, cw - UI_SCROLL_W, UI_ROW, t.soft);
            else if (over)
                rect(&s, cx, ry, cw - UI_SCROLL_W, UI_ROW, mix(t.bg, t.fg, 14));

            /* The tag, then the name, then what it is and how big. */
            round_rect(&s, cx + UI_PAD, ry + (UI_ROW - 12) / 2, 10, 12, 3,
                       kind_tint(&t, e->kind));

            int ty = ry + (UI_ROW - face_h(UI_FACE_BODY)) / 2;
            face_draw(&s, cx + UI_PAD + 20, ty, e->name,
                      idx == selected ? t.fg : mix(t.fg, t.dim, 90),
                      UI_FACE_BODY);

            if (cw > 380)
                face_draw(&s, w - UI_SCROLL_W - 190, ty, kind_name(e->kind),
                          t.dim, UI_FACE_SMALL);

            if (!e->is_dir) {
                char size[24];
                int n;
                if (e->size >= 1024) {
                    n = utoa(e->size / 1024, size);
                    size[n++] = ' '; size[n++] = 'K';
                } else {
                    n = utoa(e->size, size);
                    size[n++] = ' '; size[n++] = 'B';
                }
                size[n] = 0;
                face_draw(&s, w - UI_SCROLL_W - UI_PAD - face_w(size, UI_FACE_SMALL),
                          ty, size, t.dim, UI_FACE_SMALL);
            }

            if (over && in.released) {
                in.released = 0;
                if (selected == idx) open_entry(idx);
                else selected = idx;
            }
            if (over && in.right_pressed) {
                in.right_pressed = 0;
                selected = idx;
                menu_open = 1;
                menu_x = in.mx;
                menu_y = in.my;
            }
        }

        ui_scrollbar(&s, &t, w - UI_SCROLL_W, list_y, list_h, first_row, shown, count);

        /* --- what is down there ------------------------------------------- */
        char right[48];
        int n = utoa((u32)total, right);
        const char *items = total == 1 ? " item, " : " items, ";
        for (int i = 0; items[i]; i++) right[n++] = items[i];
        n += utoa(total_bytes / 1024, right + n);
        right[n++] = ' '; right[n++] = 'K'; right[n] = 0;

        const char *left = status[0] ? status : cwd;
        char detail[160];
        if (!status[0] && selected >= 0 && selected < count) {
            int d = 0;
            for (int i = 0; entries[selected].name[i] && d < 90; i++)
                detail[d++] = entries[selected].name[i];
            detail[d++] = ' '; detail[d++] = '-'; detail[d++] = ' ';
            const char *k = kind_name(entries[selected].kind);
            for (int i = 0; k[i]; i++) detail[d++] = k[i];
            detail[d] = 0;
            left = detail;
        }
        ui_statusbar(&s, &t, w, h, left, right);

        /* --- the menu that is down, over everything else ------------------
         *
         * Drawn last so it sits over the window rather than under whatever
         * is painted after it, and every one of its entries does the same
         * thing the toolbar button or the context menu does. A command
         * reachable two ways is not duplication: it is the difference
         * between a program you can learn and one you have to be told
         * about. */
        if (bar_open >= 0) {
            int pick = ui_menu(&s, &in, &t, bar_x[bar_open], UI_MENUBAR_H,
                               MENU_WIDTHS[bar_open],
                               MENU_ITEMS[bar_open], MENU_COUNTS[bar_open]);
            if (pick >= 0) {
                int which = bar_open;
                bar_open = -1;

                if (which == 0) {                       /* File */
                    if (pick == 0) open_entry(selected);
                    else if (pick == 1) {
                        char target[PATH_MAX];
                        join(target, cwd, "new folder");
                        say(mkdir(target) < 0 ? "could not create" : "created");
                        reload();
                    } else if (pick == 2 && selected >= 0) {
                        renaming = selected;
                        strncpy(rename_buf, entries[selected].name,
                                sizeof(rename_buf) - 1);
                        rename_buf[sizeof(rename_buf) - 1] = 0;
                        rename_field.len = strlen(rename_buf);
                        rename_field.cursor = rename_field.len;
                        rename_field.focused = 1;
                    } else if (pick == 3 && selected >= 0) {
                        char target[PATH_MAX];
                        join(target, cwd, entries[selected].name);
                        say(unlink(target) < 0 ? "could not delete" : "deleted");
                        reload();
                    } else if (pick == 4) closing = 1;
                } else if (which == 1) {                /* Edit */
                    if (pick == 0 && selected >= 0) {
                        join(clip_path, cwd, entries[selected].name);
                        clip_is_cut = 0;
                        say("copied");
                    } else if (pick == 1 && selected >= 0) {
                        join(clip_path, cwd, entries[selected].name);
                        clip_is_cut = 1;
                        say("cut");
                    } else if (pick == 2) do_paste();
                } else if (which == 2) {                /* View */
                    if (pick == 0) reload();
                    else if (pick == 1) go_to("/home");
                    else if (pick == 2) go_to("/");
                } else {                                /* Help */
                    say("Files, a ring 3 program");
                }
            } else if (in.released) {
                in.released = 0;
                bar_open = -1;             /* a click anywhere else shuts it */
            }
        }

        if (menu_open) {
            int pick = ui_menu(&s, &in, &t, menu_x, menu_y, 150,
                               menu_items, MENU_COUNT);
            if (pick >= 0) {
                menu_open = 0;
                if (pick == 0) open_entry(selected);
                else if (pick == 1) open_with(selected, "/bin/notes");
                else if (pick == 2) open_with(selected, "/bin/music");
                else if (pick == 3 && selected >= 0) {
                    join(clip_path, cwd, entries[selected].name);
                    clip_is_cut = 0;
                    clip_set(clip_path, strlen(clip_path));
                    say("copied a path");
                } else if (pick == 4 && selected >= 0) {
                    join(clip_path, cwd, entries[selected].name);
                    clip_is_cut = 1;
                    say("cut");
                } else if (pick == 5 && selected >= 0) {
                    renaming = selected;
                    strncpy(rename_buf, entries[selected].name, sizeof(rename_buf) - 1);
                    rename_buf[sizeof(rename_buf) - 1] = 0;
                    rename_field.len = strlen(rename_buf);
                    rename_field.cursor = rename_field.len;
                    rename_field.focused = 1;
                } else if (pick == 6) {
                    do_delete();
                }
            } else if (in.pressed) {
                menu_open = 0;
            }
        }

        win_commit(win);
        sleep_ms(16);
    }

    win_close(win);
    exit(0);
}
