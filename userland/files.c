/* A file manager.
 *
 * The shell could already do all of this, which is exactly why it is worth
 * having: a desktop where the only way to see what is on the disk is to type
 * `ls` is not a desktop, it is a terminal with wallpaper.
 *
 * Everything here goes through the ordinary file syscalls. There is no
 * privileged path and nothing it can do that a program written by anyone
 * else could not, which is the property worth keeping. */
#include "nyx.h"
#include "ui.h"

#define MAX_ENTRIES 512
#define PATH_MAX    256

typedef struct {
    char name[32];
    u32  size;
    u32  is_dir;
} entry;

static entry entries[MAX_ENTRIES];
static int   count;
static char  cwd[PATH_MAX] = "/";
static int   selected = -1;
static int   first_row;                 /* the top of the scrolled view */

static char  status[128];
static char  clip_path[PATH_MAX];       /* what copy or cut remembered */
static int   clip_is_cut;

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

/* --- the listing ---------------------------------------------------------- */

/* Directories first, then files, each alphabetical. readdir gives whatever
   order the filesystem stores, which for FAT is creation order, and a
   directory listing that changes order as files are written is unusable. */
static void sort_entries(void) {
    for (int i = 1; i < count; i++) {
        entry key = entries[i];
        int j = i - 1;
        while (j >= 0) {
            entry *a = &entries[j];
            int after;
            if (a->is_dir != key.is_dir) after = !a->is_dir && key.is_dir;
            else                         after = strcmp(a->name, key.name) > 0;
            if (!after) break;
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

static void reload(void) {
    count = 0;
    selected = -1;
    first_row = 0;

    nyx_stat st;
    for (int i = 0; i < MAX_ENTRIES && readdir(cwd, i, &st) == 1; i++) {
        strncpy(entries[count].name, st.name, sizeof(entries[count].name) - 1);
        entries[count].name[sizeof(entries[count].name) - 1] = 0;
        entries[count].size = st.size;
        entries[count].is_dir = st.is_dir;
        count++;
    }
    sort_entries();
}

static void say(const char *msg) {
    strncpy(status, msg, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;
}

/* --- what the buttons do --------------------------------------------------- */

static void enter(int index) {
    if (index < 0 || index >= count) return;
    if (!entries[index].is_dir) return;
    char next[PATH_MAX];
    join(next, cwd, entries[index].name);
    strncpy(cwd, next, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = 0;
    reload();
}

static void go_up(void) {
    parent_of(cwd);
    reload();
}

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

    /* The name is whatever came after the last slash. */
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
#define CRUMB_H   26

static const char *const menu_items[] = { "Open", "Copy", "Cut", "Rename", "Delete" };
#define MENU_COUNT 5

void _start(void) {
    int win = win_create("Files", 720, 480);
    if (win < 0) exit(1);
    win_allow_resize(win);

    ui_theme t = ui_load_theme();
    ui_input in;
    memset(&in, 0, sizeof(in));

    reload();
    say("");

    /* Rename happens in place: the row turns into a field rather than a
       dialog appearing somewhere else on the screen. */
    int renaming = -1;
    char rename_buf[32];
    ui_field rename_field;
    memset(&rename_field, 0, sizeof(rename_field));
    rename_field.buf = rename_buf;
    rename_field.cap = sizeof(rename_buf);

    int menu_open = 0, menu_x = 0, menu_y = 0;

    for (;;) {
        int w = win_width(win), h = win_height(win);
        u32 *px = win_surface(win);
        if (!px || w <= 0 || h <= 0) break;
        surface s = { px, w, h };

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
        } else if (in.key) {
            if (in.key == KEY_UP && selected > 0) selected--;
            else if (in.key == KEY_DOWN && selected < count - 1) selected++;
            else if (in.key == '\n') enter(selected);
            else if (in.key == '\b') go_up();
            else if (in.key == KEY_DELETE) do_delete();
        }

        /* --- paint ------------------------------------------------------ */
        fill(&s, t.bg);
        ui_toolbar(&s, &t, w, TOOLBAR_H);

        int bx = UI_PAD;
        if (ui_button(&s, &in, &t, bx, (TOOLBAR_H - UI_BTN_H) / 2, 48, "Up")) go_up();
        bx += 48 + UI_GAP;
        if (ui_button(&s, &in, &t, bx, (TOOLBAR_H - UI_BTN_H) / 2, 64, "Home")) {
            strcpy(cwd, "/home");
            reload();
        }
        bx += 64 + UI_GAP;
        if (ui_button(&s, &in, &t, bx, (TOOLBAR_H - UI_BTN_H) / 2, 72, "New dir")) {
            char target[PATH_MAX];
            join(target, cwd, "new folder");
            say(mkdir(target) < 0 ? "could not create" : "created");
            reload();
        }
        bx += 72 + UI_GAP;
        if (ui_button(&s, &in, &t, bx, (TOOLBAR_H - UI_BTN_H) / 2, 64, "Paste")) do_paste();

        /* The path, as its own strip. */
        rect(&s, 0, TOOLBAR_H, w, CRUMB_H, mix(t.bg, 0, 30));
        text(&s, UI_PAD, TOOLBAR_H + (CRUMB_H - FONT_H) / 2, cwd, t.dim);

        int list_y = TOOLBAR_H + CRUMB_H;
        int list_h = h - list_y - UI_ROW;
        int shown = list_h / UI_ROW;

        /* Keep the selection on screen, whichever way it moved. */
        if (selected >= 0) {
            if (selected < first_row) first_row = selected;
            if (selected >= first_row + shown) first_row = selected - shown + 1;
        }
        if (first_row > count - shown) first_row = count - shown;
        if (first_row < 0) first_row = 0;

        if (count == 0) {
            text(&s, UI_PAD, list_y + UI_PAD, "(empty)", t.dim);
        }

        for (int i = 0; i < shown && first_row + i < count; i++) {
            int idx = first_row + i;
            int ry = list_y + i * UI_ROW;
            entry *e = &entries[idx];

            if (renaming == idx) {
                rect(&s, 0, ry, w, UI_ROW, t.bg);
                ui_field_draw(&s, &in, &t, UI_PAD, ry - 2, w - UI_PAD * 2 - UI_SCROLL_W,
                              &rename_field, "name");
                continue;
            }

            /* A folder is marked rather than iconified: one glyph reads at
               this size, a drawn icon does not. */
            char label[40];
            int li = 0;
            label[li++] = e->is_dir ? '[' : ' ';
            for (int k = 0; e->name[k] && li < (int)sizeof(label) - 2; k++)
                label[li++] = e->name[k];
            if (e->is_dir) label[li++] = ']';
            label[li] = 0;

            char size[24];
            if (e->is_dir) {
                strcpy(size, "folder");
            } else {
                int n = utoa(e->size, size);
                size[n++] = ' '; size[n++] = 'B'; size[n] = 0;
            }

            int hit = ui_row(&s, &in, &t, 0, ry, w - UI_SCROLL_W, label, size, idx == selected);
            if (hit == 1) {
                if (selected == idx && e->is_dir) enter(idx);
                else selected = idx;
            } else if (hit == 2) {
                selected = idx;
                menu_open = 1;
                menu_x = in.mx;
                menu_y = in.my;
            }
        }

        ui_scrollbar(&s, &t, w - UI_SCROLL_W, list_y, list_h, first_row, shown, count);

        char right[32];
        int n = utoa((u32)count, right);
        right[n++] = ' ';
        right[n++] = 'i'; right[n++] = 't'; right[n++] = 'e'; right[n++] = 'm';
        right[n++] = 's'; right[n] = 0;
        ui_statusbar(&s, &t, w, h, status[0] ? status : cwd, right);

        if (menu_open) {
            int pick = ui_menu(&s, &in, &t, menu_x, menu_y, 140, menu_items, MENU_COUNT);
            if (pick >= 0) {
                menu_open = 0;
                if (pick == 0) enter(selected);
                else if (pick == 1 && selected >= 0) {
                    join(clip_path, cwd, entries[selected].name);
                    clip_is_cut = 0;
                    clip_set(clip_path, strlen(clip_path));
                    say("copied a path");
                } else if (pick == 2 && selected >= 0) {
                    join(clip_path, cwd, entries[selected].name);
                    clip_is_cut = 1;
                    say("cut");
                } else if (pick == 3 && selected >= 0) {
                    renaming = selected;
                    strncpy(rename_buf, entries[selected].name, sizeof(rename_buf) - 1);
                    rename_buf[sizeof(rename_buf) - 1] = 0;
                    rename_field.len = strlen(rename_buf);
                    rename_field.cursor = rename_field.len;
                    rename_field.focused = 1;
                } else if (pick == 4) {
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
