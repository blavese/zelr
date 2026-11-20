/* Help: every key the desktop and its programs take, and what it does.
 *
 * The keys were spread over every program's own corner of the README, and a
 * key nobody knows about is a feature nobody has. This is the one list,
 * narrowed as you type: "zip", "alt" or "music" leaves the rows with that in
 * where, keys or what (case aside). What is shown is said on the console,
 * how many of all of them and the first ("help: 1 of 70 shown; the first,
 * files ctrl+K: make a zip of what is chosen"), for helpcheck.py.
 *
 * The table is kept by hand beside the programs it describes; when a key
 * changes there, it changes here.
 */
#include "zelr.h"
#include "ui.h"

typedef struct { const char *where, *keys, *what; } row_t;

static const row_t ROWS[] = {
    { "desktop", "alt+Tab", "the window behind comes to the front" },
    { "desktop", "alt+D", "every window put away" },
    { "desktop", "alt+M", "the front window put away" },
    { "desktop", "alt+F", "the front window fills the screen, or goes back" },
    { "desktop", "alt+Q", "the front window closed" },
    { "desktop", "alt+Left, alt+Right", "the front window to that half of the screen" },
    { "desktop", "alt+L", "the screen locked: only the password opens it" },
    { "desktop", "alt+N", "the notices given lately, newest first" },
    { "desktop", "alt+Escape", "the desktop left for the console" },
    { "desktop", "ctrl+F", "a word looked for on the screen" },
    { "desktop", "Print Screen", "the screen saved as a PNG in /home/pictures" },
    { "desktop", "alt+Print Screen", "the front window alone saved as a PNG in /home/pictures" },
    { "terminal", "Tab", "a command or a path finished" },
    { "terminal", "Up, Down", "back and on through what was typed" },
    { "terminal", "ctrl+R", "back through what was typed for a word; Return runs it, ctrl+G lets go" },
    { "terminal", "Page Up, Page Down", "the scrollback" },
    { "terminal", "ctrl+C, ctrl+V", "what is selected copied, the clipboard pasted" },
    { "terminal", "ctrl+A", "everything on the screen selected" },
    { "terminal", "Escape", "the desktop left for the console" },
    { "files", "Return", "the file opened, or the folder gone into" },
    { "files", "Backspace", "up a folder, or out of an archive" },
    { "files", "Home, End", "the top or the bottom of the list" },
    { "files", "/", "the filter: type to narrow, Return looks in every folder below" },
    { "files", "Delete", "into the trash (twice, for good, on a stick or in the trash)" },
    { "files", "ctrl+Delete", "the trash emptied (pressed twice)" },
    { "files", "ctrl+Z", "the last thing put in the trash put back" },
    { "files", "ctrl+C, ctrl+X, ctrl+V", "copy, cut and paste" },
    { "files", "F2", "the name changed" },
    { "files", "ctrl+E", "an archive taken out into a folder beside it" },
    { "files", "ctrl+K", "make a zip of what is chosen" },
    { "files", "ctrl+P", "how big a folder is, or a file's size and date" },
    { "files", "ctrl+1, ctrl+2, ctrl+3", "sorted by name, by size, by date" },
    { "notes", "ctrl+S", "saved" },
    { "notes", "ctrl+Z", "the last change taken back" },
    { "notes", "ctrl+A", "everything selected" },
    { "notes", "ctrl+C, ctrl+X, ctrl+V", "copy, cut and paste" },
    { "notes", "ctrl+H", "find and replace; Return the next, ctrl+R every one" },
    { "notes", "F3", "the next match" },
    { "notes", "ctrl+G", "to a line by its number" },
    { "sheet", "a key, F2", "the cell typed into, or what is in it changed" },
    { "sheet", "Return, Tab", "kept, and down (back to where the Tabs began) or along" },
    { "sheet", "ctrl+G", "to a cell by its name, as B3" },
    { "sheet", "ctrl+T", "the rows from here down in order by this column; again, the other way" },
    { "sheet", "ctrl+S, ctrl+O", "saved as CSV, or another opened" },
    { "sheet", "Delete", "the cell emptied" },
    { "sheet", "ctrl+D, ctrl+R", "the cell above, or to the left, copied in, its references moved" },
    { "sheet", "ctrl+C, ctrl+V", "a cell copied and pasted, its references moved ($ keeps one)" },
    { "paint", "B, L, R, E, F", "the brush, a line, a rectangle, an ellipse, a fill" },
    { "paint", "T", "words, typed where the paper is clicked; Return ends them" },
    { "paint", "1, 2, 3, 4", "the brush's size" },
    { "paint", "[, ]", "the colour before or after" },
    { "paint", "C", "the paper cleared" },
    { "paint", "ctrl+S", "saved as a PNG" },
    { "paint", "ctrl+Z", "the last stroke or shape taken back" },
    { "pictures", "Left, Right, Space", "the picture before or after in the folder" },
    { "pictures", "Home, End", "the first or the last picture" },
    { "pictures", "+, -, 0", "twice or half the size, or fitted again" },
    { "pictures", "the arrows, a drag", "about a picture bigger than the window" },
    { "pictures", "R", "turned a quarter to the right" },
    { "pictures", "W", "made the wallpaper" },
    { "pictures", "S", "a slideshow, three seconds a picture; S or an arrow stops it" },
    { "music", "Up, Down, Home, End", "a song chosen" },
    { "music", "Return", "the song chosen played" },
    { "music", "Space", "paused, and on from the same place" },
    { "music", "Left, Right", "the song before or after" },
    { "music", "R", "the list repeated" },
    { "music", "S", "the list shuffled, each song once before any twice" },
    { "calendar", "the arrows", "a day or a week" },
    { "calendar", "Page Up, Page Down", "a month" },
    { "calendar", "Home", "back to today" },
    { "calendar", "Return", "the day's note written; Return again keeps it" },
    { "calendar", "Delete", "the day's note taken away" },
    { "clock", "Space", "the stopwatch started or stopped" },
    { "clock", "R", "the stopwatch put back" },
    { "browser", "ctrl+L", "the address bar" },
    { "browser", "ctrl+D", "the page bookmarked" },
    { "browser", "ctrl+N", "a new window" },
    { "browser", "ctrl+S", "the page kept in /home/downloads" },
    { "browser", "ctrl+click, the middle button", "a link opened in a new window" },
    { "browser", "the right button", "a menu for the link, the picture or the page" },
    { "browser", "double click, triple click", "a word or a paragraph selected" },
};
#define NROWS ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

static char filter_buf[48];
static ui_field filter;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int has(const char *s, const char *want) {
    if (!want[0]) return 1;
    for (int i = 0; s[i]; i++) {
        int k = 0;
        while (want[k] && lower(s[i + k]) == lower(want[k])) k++;
        if (!want[k]) return 1;
    }
    return 0;
}

static int shown(const row_t *r) {
    return has(r->where, filter_buf) || has(r->keys, filter_buf) || has(r->what, filter_buf);
}

int main(void) {
    int win = win_create("Help", 640, 520);
    if (win < 0) exit(1);
    win_allow_resize(win);

    memset(&filter, 0, sizeof(filter));
    filter.buf = filter_buf;
    filter.cap = sizeof(filter_buf);
    filter.focused = 1;

    ui_input in;
    memset(&in, 0, sizeof(in));
    static char said[200];
    int scroll = 0;

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

        int before = filter.len;
        for (int ki = 0; ki < in.nkeys; ki++) {
            u32 k = in.keys[ki];
            if (k == KEY_PAGE_DOWN) scroll += 8;
            else if (k == KEY_PAGE_UP) scroll -= 8;
            else if (k == KEY_DOWN) scroll++;
            else if (k == KEY_UP) scroll--;
            else ui_field_key(&filter, k);
        }
        if (filter.len != before) scroll = 0;
        if (in.scroll) scroll += in.scroll * 3;

        fill(&s, t.bg);
        int y = UI_PAD;
        ui_field_draw(&s, &in, &t, UI_PAD, y, w - UI_PAD * 2, &filter, "type to look for a key, a program or a word");
        y += UI_BTN_H + UI_PAD;

        int line = face_h(UI_FACE_BODY) + 8;
        int rows = (h - y - UI_ROW - 8) / line;
        if (rows < 1) rows = 1;
        int n = 0, first = -1;
        for (int i = 0; i < NROWS; i++) if (shown(&ROWS[i])) { if (first < 0) first = i; n++; }
        if (scroll > n - rows) scroll = n - rows;
        if (scroll < 0) scroll = 0;

        int seen = 0, drawn = 0;
        for (int i = 0; i < NROWS && drawn < rows; i++) {
            if (!shown(&ROWS[i])) continue;
            if (seen++ < scroll) continue;
            face_draw(&s, UI_PAD, y, ROWS[i].where, t.dim, UI_FACE_BODY);
            face_draw(&s, UI_PAD + 80, y, ROWS[i].keys, t.accent, UI_FACE_BODY);
            int kx = UI_PAD + 80 + face_w(ROWS[i].keys, UI_FACE_BODY) + 12;
            if (kx < UI_PAD + 250) kx = UI_PAD + 250;
            face_draw(&s, kx, y, ROWS[i].what, t.fg, UI_FACE_BODY);
            y += line;
            drawn++;
        }
        if (!n) face_draw(&s, UI_PAD, y, "Nothing has that in it.", t.dim, UI_FACE_BODY);

        /* What is shown, on the line and the console. */
        char line_text[200];
        int k = 0;
        k += utoa((u32)n, line_text + k);
        for (const char *p = " of "; *p; p++) line_text[k++] = *p;
        k += utoa((u32)NROWS, line_text + k);
        for (const char *p = " shown"; *p; p++) line_text[k++] = *p;
        if (first >= 0) {
            for (const char *p = "; the first, "; *p; p++) line_text[k++] = *p;
            for (const char *p = ROWS[first].where; *p; p++) line_text[k++] = *p;
            line_text[k++] = ' ';
            for (const char *p = ROWS[first].keys; *p && k < 150; p++) line_text[k++] = *p;
            line_text[k++] = ':';
            line_text[k++] = ' ';
            for (const char *p = ROWS[first].what; *p && k < 196; p++) line_text[k++] = *p;
        }
        line_text[k] = 0;
        ui_statusbar(&s, &t, w, h, line_text, "help");
        if (strcmp(line_text, said)) {
            strcpy(said, line_text);
            puts("help: ");
            puts(said);
            puts("\n");
        }

        win_commit(win);
        ui_wait(win);
    }

    win_close(win);
    exit(0);
}
