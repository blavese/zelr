/* A text editor.
 *
 * The terminal can already write a file in one go with `write`, which is
 * fine for a line and useless for anything you want to come back to. This
 * is the program that makes the disk worth having.
 *
 * The buffer is one flat array of characters with newlines in it, rather
 * than an array of lines. That makes insertion O(n) and it does not matter:
 * n is a text file, the machine does this between frames, and the flat form
 * means saving is one write and loading is one read with nothing in between
 * that can disagree about what a line is.
 */
#include "zelr.h"
#include "ui.h"

#define BUF_MAX   65536
#define PATH_MAX  256

static char buf[BUF_MAX];
static int  len;
static int  cursor;
static int  dirty;
static char path[PATH_MAX];
static char status[128];

static int  scroll_line;          /* first line drawn */
static int  sel_anchor = -1;      /* where a selection started, -1 for none */

/* --- the buffer ----------------------------------------------------------- */

static void say(const char *m) {
    strncpy(status, m, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;
}

static void insert_text(const char *text, int n) {
    if (len + n >= BUF_MAX) { say("file is full"); return; }
    for (int i = len; i >= cursor; i--) buf[i + n] = buf[i];
    for (int i = 0; i < n; i++) buf[cursor + i] = text[i];
    len += n;
    cursor += n;
    buf[len] = 0;
    dirty = 1;
}

static void insert_char(char c) { insert_text(&c, 1); }

static void delete_range(int from, int to) {
    if (from < 0) from = 0;
    if (to > len) to = len;
    if (to <= from) return;
    int n = to - from;
    for (int i = from; i < len - n; i++) buf[i] = buf[i + n];
    len -= n;
    buf[len] = 0;
    cursor = from;
    dirty = 1;
}

static void backspace(void) {
    if (cursor > 0) delete_range(cursor - 1, cursor);
}

/* --- positions ------------------------------------------------------------ */

static int line_start(int at) {
    while (at > 0 && buf[at - 1] != '\n') at--;
    return at;
}

static int line_end(int at) {
    while (at < len && buf[at] != '\n') at++;
    return at;
}

static int line_of(int at) {
    int n = 0;
    for (int i = 0; i < at && i < len; i++) if (buf[i] == '\n') n++;
    return n;
}

static int column_of(int at) { return at - line_start(at); }

static int start_of_line(int line) {
    int at = 0, n = 0;
    while (at < len && n < line) if (buf[at++] == '\n') n++;
    return at;
}

static int total_lines(void) {
    int n = 1;
    for (int i = 0; i < len; i++) if (buf[i] == '\n') n++;
    return n;
}

/* Moving up or down keeps the column where it can. */
static void move_line(int delta) {
    int col = column_of(cursor);
    int line = line_of(cursor) + delta;
    if (line < 0) line = 0;
    int total = total_lines();
    if (line >= total) line = total - 1;

    int at = start_of_line(line);
    int end = line_end(at);
    cursor = at + col;
    if (cursor > end) cursor = end;
}

/* --- files ---------------------------------------------------------------- */

static void load(const char *from) {
    int n = slurp(from, buf, BUF_MAX - 1);
    if (n < 0) {
        len = 0; buf[0] = 0;
        say("new file");
    } else {
        len = n;
        buf[len] = 0;
        say("opened");
    }
    cursor = 0;
    scroll_line = 0;
    sel_anchor = -1;
    dirty = 0;
    strncpy(path, from, PATH_MAX - 1);
    path[PATH_MAX - 1] = 0;
}

static void save(void) {
    if (!path[0]) { say("no filename"); return; }
    if (spit(path, buf, len) < 0) { say("could not save"); return; }
    dirty = 0;
    say("saved");
}

/* --- selection and the clipboard ------------------------------------------ */

static int sel_low(void)  { return sel_anchor < cursor ? sel_anchor : cursor; }
static int sel_high(void) { return sel_anchor < cursor ? cursor : sel_anchor; }
static int has_selection(void) { return sel_anchor >= 0 && sel_anchor != cursor; }

static void copy_selection(int cut) {
    if (!has_selection()) { say("nothing selected"); return; }
    int a = sel_low(), b = sel_high();
    clip_set(buf + a, b - a);
    if (cut) { delete_range(a, b); sel_anchor = -1; say("cut"); }
    else say("copied");
}

static void paste(void) {
    int n = clip_len();
    if (n <= 0) { say("clipboard is empty"); return; }
    if (n > BUF_MAX - len - 1) { say("too much to paste"); return; }

    static char incoming[BUF_MAX];
    n = clip_get(incoming, sizeof(incoming));
    if (n <= 0) return;
    if (has_selection()) { delete_range(sel_low(), sel_high()); sel_anchor = -1; }
    insert_text(incoming, n);
    say("pasted");
}

/* --- the window ----------------------------------------------------------- */

#define TOOLBAR_H 40
#define GUTTER_W  (5 * MONO_W)

void _start(void) {
    int win = win_create("Notes", 700, 520);
    if (win < 0) exit(1);
    win_allow_resize(win);

    ui_theme t = ui_load_theme();
    ui_input in;
    memset(&in, 0, sizeof(in));

    /* Whatever this was started on, which is how the file manager's open
       reaches the editor. The file left in /cfg is the older way of saying
       the same thing and still works. */
    char wanted[PATH_MAX];
    if (getarg(wanted, sizeof(wanted)) > 0 && wanted[0]) {
        load(wanted);
    } else if (slurp("/cfg/notes-open", wanted, sizeof(wanted) - 1) > 0) {
        int n = strlen(wanted);
        while (n > 0 && (wanted[n - 1] == '\n' || wanted[n - 1] == ' ')) wanted[--n] = 0;
        load(wanted);
        unlink("/cfg/notes-open");
    } else {
        strcpy(path, "/home/untitled.txt");
        load(path);
    }

    char name_buf[PATH_MAX];
    strncpy(name_buf, path, sizeof(name_buf) - 1);
    ui_field name_field;
    memset(&name_field, 0, sizeof(name_field));
    name_field.buf = name_buf;
    name_field.cap = sizeof(name_buf);
    name_field.len = strlen(name_buf);
    name_field.cursor = name_field.len;

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

        int text_y = TOOLBAR_H;
        int text_h = h - text_y - UI_ROW;
        int rows = text_h / MONO_H;
        int cols = (w - GUTTER_W - UI_PAD) / MONO_W;

        /* --- keys ------------------------------------------------------- */
        u32 k = in.key;
        if (k && !name_field.focused) {
            int shifted = 0;                /* selection extends while shift-like keys move */

            if (k == KEY_LEFT)       { if (cursor > 0) cursor--; }
            else if (k == KEY_RIGHT) { if (cursor < len) cursor++; }
            else if (k == KEY_UP)    move_line(-1);
            else if (k == KEY_DOWN)  move_line(1);
            else if (k == KEY_HOME)  cursor = line_start(cursor);
            else if (k == KEY_END)   cursor = line_end(cursor);
            else if (k == KEY_PAGE_UP)   move_line(-rows);
            else if (k == KEY_PAGE_DOWN) move_line(rows);
            else if (k == KEY_DELETE) {
                if (has_selection()) { delete_range(sel_low(), sel_high()); sel_anchor = -1; }
                else if (cursor < len) delete_range(cursor, cursor + 1);
            }
            else if (k == '\b') {
                if (has_selection()) { delete_range(sel_low(), sel_high()); sel_anchor = -1; }
                else backspace();
            }
            else if (k == '\n' || k == '\t' || (k >= 32 && !KEY_IS_SPECIAL(k))) {
                if (has_selection()) { delete_range(sel_low(), sel_high()); sel_anchor = -1; }
                insert_char(k == '\t' ? ' ' : (char)k);
                if (k == '\t') { insert_char(' '); insert_char(' '); insert_char(' '); }
            }
            (void)shifted;
        }

        /* --- paint ------------------------------------------------------ */
        fill(&s, t.bg);
        ui_toolbar(&s, &t, w, TOOLBAR_H);

        int bx = UI_PAD;
        int by = (TOOLBAR_H - UI_BTN_H) / 2;
        if (ui_button_primary(&s, &in, &t, bx, by, 56, "Save")) save();
        bx += 56 + UI_GAP;
        if (ui_button(&s, &in, &t, bx, by, 56, "Open")) { load(name_buf); }
        bx += 56 + UI_GAP;
        if (ui_button(&s, &in, &t, bx, by, 56, "Copy")) copy_selection(0);
        bx += 56 + UI_GAP;
        if (ui_button(&s, &in, &t, bx, by, 48, "Cut"))  copy_selection(1);
        bx += 48 + UI_GAP;
        if (ui_button(&s, &in, &t, bx, by, 64, "Paste")) paste();
        bx += 64 + UI_GAP;

        ui_field_draw(&s, &in, &t, bx, by, w - bx - UI_PAD, &name_field, "path");
        if (name_field.focused && in.key) {
            if (in.key == '\n') { load(name_buf); name_field.focused = 0; }
            else ui_field_key(&name_field, in.key);
        }

        /* Keep the cursor's line on screen. */
        int cur_line = line_of(cursor);
        /* The wheel moves the page without moving the cursor, which is
           what makes it reading rather than editing. The page only follows
           the cursor on a frame where the wheel did not move it, or the
           two fight and the cursor always wins. */
        if (in.scroll) {
            scroll_line += in.scroll * 3;
            int last = line_of(len);
            if (scroll_line > last) scroll_line = last;
        } else {
            if (cur_line < scroll_line) scroll_line = cur_line;
            if (cur_line >= scroll_line + rows) scroll_line = cur_line - rows + 1;
        }
        if (scroll_line < 0) scroll_line = 0;

        rect(&s, 0, text_y, GUTTER_W, text_h, mix(t.bg, 0, 30));

        int at = start_of_line(scroll_line);
        for (int row = 0; row < rows && at <= len; row++) {
            int ry = text_y + row * MONO_H;
            int end = line_end(at);

            char num[8];
            int nn = utoa((u32)(scroll_line + row + 1), num);
            num[nn] = 0;
            int nw = (int)strlen(num) * MONO_W;
            face_draw(&s, GUTTER_W - nw - 4, ry, num, t.dim, UI_FACE_MONO);

            /* The selection is painted under the glyphs rather than over. */
            if (has_selection()) {
                int a = sel_low(), b = sel_high();
                int from = a > at ? a : at;
                int to = b < end ? b : end;
                if (to > from)
                    rect(&s, GUTTER_W + (from - at) * MONO_W, ry,
                         (to - from) * MONO_W, MONO_H, mix(t.bg, t.accent, 140));
            }

            for (int i = 0; at + i < end && i < cols; i++)
                mono_char(&s, GUTTER_W + i * MONO_W, ry, buf[at + i], t.fg);

            if (cursor >= at && cursor <= end && (ticks() / 30) % 2 == 0) {
                int cx = GUTTER_W + (cursor - at) * MONO_W;
                rect(&s, cx, ry, 2, MONO_H, t.accent);
            }

            if (end >= len) break;
            at = end + 1;
        }

        /* Clicking puts the cursor where it was clicked; dragging selects. */
        if (in.my >= text_y && in.my < text_y + text_h && in.mx >= GUTTER_W) {
            int row = (in.my - text_y) / MONO_H;
            int col = (in.mx - GUTTER_W) / MONO_W;
            int at2 = start_of_line(scroll_line + row);
            int end2 = line_end(at2);
            int want = at2 + col;
            if (want > end2) want = end2;

            if (in.pressed) { cursor = want; sel_anchor = want; }
            else if (in.down) cursor = want;
        }

        char left[192];
        int li = 0;
        if (dirty) { left[li++] = '*'; left[li++] = ' '; }
        for (int i = 0; path[i] && li < (int)sizeof(left) - 1; i++) left[li++] = path[i];
        left[li] = 0;

        char right[48];
        int ri = utoa((u32)(line_of(cursor) + 1), right);
        right[ri++] = ':';
        ri += utoa((u32)(column_of(cursor) + 1), right + ri);
        right[ri++] = ' '; right[ri++] = ' ';
        for (const char *m = status; *m && ri < (int)sizeof(right) - 1; m++) right[ri++] = *m;
        right[ri] = 0;

        ui_statusbar(&s, &t, w, h, left, right);

        win_commit(win);
        sleep_ms(16);
    }

    win_close(win);
    exit(0);
}
