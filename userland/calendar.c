/* A calendar.
 *
 * A month at a time, weeks from Monday, today marked, and a line of notes
 * for any day: what a person opens a calendar to find out or to write
 * down. Today is the machine's own idea of it, in its time zone: the local
 * line of /sys/time (kernel/zone.c), read when the window opens and once a
 * minute after, so a calendar left open past midnight moves on.
 *
 * The notes are a text file, /home/calendar, a line a day:
 * "2026-11-20<tab>the dentist at ten". Plain text for the reason
 * /zelr.cfg is: the shell can read and write it, and a calendar whose
 * notes only it can read is a calendar whose notes are lost with it.
 *
 * Keys: the arrows move a day and a week, Page Up and Page Down a month,
 * Home goes back to today; Return (or a click on the day chosen) writes its
 * note, Return again keeps it and Escape leaves it as it was; Delete takes
 * the note away. What is shown is said on the console too
 * ("calendar: 2026-11-20, Friday, November 2026"), for calendarcheck.py.
 */
#include "zelr.h"
#include "ui.h"

#define NOTES_FILE "/home/calendar"
#define NOTES_MAX  (64 * 1024)
#define NOTE_MAX   96

static const char *const MONTHS[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"
};
static const char *const DAYS[7] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
static const char *const DAY_NAMES[7] = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
};

static int today_y = 2026, today_m = 1, today_d = 1;
static int view_y, view_m, sel_d;          /* the month shown, and the day chosen in it */

static char notes[NOTES_MAX + 1];          /* the file, as it is on the disk */
static int  notes_len;
static char note_buf[NOTE_MAX];
static ui_field note_field;
static char status[96];

/* --- days ------------------------------------------------------------------- */

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in(int y, int m) {
    static const int D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap(y) ? 29 : D[m - 1];
}

/* 0 for Monday to 6 for Sunday, by Sakamoto's method. */
static int weekday(int y, int m, int d) {
    static const int T[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    int w = (y + y / 4 - y / 100 + y / 400 + T[m - 1] + d) % 7;    /* 0 for Sunday */
    return (w + 6) % 7;
}

/* The chosen day moved by n days, across months and years. */
static void move_days(int n) {
    while (n > 0) {
        if (++sel_d > days_in(view_y, view_m)) {
            sel_d = 1;
            if (++view_m > 12) { view_m = 1; view_y++; }
        }
        n--;
    }
    while (n < 0) {
        if (--sel_d < 1) {
            if (--view_m < 1) { view_m = 12; view_y--; }
            sel_d = days_in(view_y, view_m);
        }
        n++;
    }
}

/* A month on, or back, keeping the day where the new month has it. */
static void move_months(int n) {
    view_m += n;
    while (view_m > 12) { view_m -= 12; view_y++; }
    while (view_m < 1)  { view_m += 12; view_y--; }
    if (sel_d > days_in(view_y, view_m)) sel_d = days_in(view_y, view_m);
}

static int num_at(const char *p, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

/* Today, from /sys/time's local line ("local     2026-11-20 13:05:00"),
   or its first (UTC) one where there is no zone. */
static void read_today(void) {
    char t[512];
    int n = slurp("/sys/time", t, sizeof(t) - 1);
    if (n <= 0) return;
    t[n] = 0;
    const char *at = 0;
    for (const char *p = t; *p; ) {
        if (!strncmp(p, "local ", 6) || (!at && !strncmp(p, "now ", 4))) at = p;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    if (!at) return;
    while (*at && *at != ' ') at++;
    while (*at == ' ') at++;
    int y = num_at(at, 4), m = num_at(at + 5, 2), d = num_at(at + 8, 2);
    if (y < 1900 || m < 1 || m > 12 || d < 1 || d > days_in(y, m)) return;
    today_y = y;
    today_m = m;
    today_d = d;
}

/* --- notes ------------------------------------------------------------------- */

static void date_text(char *out, int y, int m, int d) {
    out[0] = (char)('0' + y / 1000 % 10); out[1] = (char)('0' + y / 100 % 10);
    out[2] = (char)('0' + y / 10 % 10);   out[3] = (char)('0' + y % 10);
    out[4] = '-';
    out[5] = (char)('0' + m / 10); out[6] = (char)('0' + m % 10);
    out[7] = '-';
    out[8] = (char)('0' + d / 10); out[9] = (char)('0' + d % 10);
    out[10] = 0;
}

static void notes_load(void) {
    int n = slurp(NOTES_FILE, notes, NOTES_MAX);
    notes_len = n > 0 ? n : 0;
    notes[notes_len] = 0;
}

/* The note for a day, copied out; 0 when it has none. */
static int note_of(int y, int m, int d, char *out, int cap) {
    char key[11];
    date_text(key, y, m, d);
    for (int i = 0; i < notes_len; ) {
        int e = i;
        while (e < notes_len && notes[e] != '\n') e++;
        if (e - i > 11 && !strncmp(notes + i, key, 10) && notes[i + 10] == '\t') {
            int k = 0;
            for (int j = i + 11; j < e && k < cap - 1; j++) out[k++] = notes[j];
            out[k] = 0;
            return 1;
        }
        i = e + 1;
    }
    if (cap) out[0] = 0;
    return 0;
}

/* The day's line replaced (or taken out, for an empty note), the rest kept
   as they were, and the file written. */
static int note_set(int y, int m, int d, const char *text) {
    static char out[NOTES_MAX + 1];
    char key[11];
    date_text(key, y, m, d);
    int w = 0;
    for (int i = 0; i < notes_len; ) {
        int e = i;
        while (e < notes_len && notes[e] != '\n') e++;
        int mine = e - i >= 11 && !strncmp(notes + i, key, 10) && notes[i + 10] == '\t';
        if (!mine && e > i) {
            for (int j = i; j < e && w < NOTES_MAX - 1; j++) out[w++] = notes[j];
            out[w++] = '\n';
        }
        i = e + 1;
    }
    if (text[0]) {
        int tl = strlen(text);
        if (w + 12 + tl >= NOTES_MAX) return 0;
        for (int j = 0; j < 10; j++) out[w++] = key[j];
        out[w++] = '\t';
        for (int j = 0; j < tl; j++) out[w++] = text[j] == '\n' ? ' ' : text[j];
        out[w++] = '\n';
    }
    mkdir("/home");
    if (spit(NOTES_FILE, out, w) < 0) return 0;
    memcpy(notes, out, w);
    notes_len = w;
    notes[w] = 0;
    return 1;
}

/* --- what is said ---------------------------------------------------------- */

static void say_chosen(void) {
    char key[11], note[NOTE_MAX];
    date_text(key, view_y, view_m, sel_d);
    int w = 0;
    for (const char *p = key; *p; p++) status[w++] = *p;
    status[w++] = ',';
    status[w++] = ' ';
    for (const char *p = DAY_NAMES[weekday(view_y, view_m, sel_d)]; *p; p++) status[w++] = *p;
    status[w++] = ',';
    status[w++] = ' ';
    for (const char *p = MONTHS[view_m - 1]; *p; p++) status[w++] = *p;
    status[w++] = ' ';
    w += utoa((u32)view_y, status + w);
    if (note_of(view_y, view_m, sel_d, note, sizeof(note))) {
        status[w++] = ':';
        status[w++] = ' ';
        for (const char *p = note; *p && w < (int)sizeof(status) - 1; p++) status[w++] = *p;
    }
    status[w] = 0;
}

static void begin_note(void) {
    note_of(view_y, view_m, sel_d, note_buf, sizeof(note_buf));
    note_field.len = strlen(note_buf);
    note_field.cursor = note_field.len;
    note_field.focused = 1;
}

static void keep_note(void) {
    note_field.focused = 0;
    char key[11];
    date_text(key, view_y, view_m, sel_d);
    if (!note_set(view_y, view_m, sel_d, note_buf)) {
        puts("calendar: the note could not be written\n");
        return;
    }
    puts(note_buf[0] ? "calendar: kept a note for " : "calendar: took the note away from ");
    puts(key);
    puts("\n");
}

/* --- the window ----------------------------------------------------------- */

int main(void) {
    int win = win_create("Calendar", 460, 470);
    if (win < 0) exit(1);
    win_allow_resize(win);
    win_want_escape(win);

    memset(&note_field, 0, sizeof(note_field));
    note_field.buf = note_buf;
    note_field.cap = sizeof(note_buf);

    read_today();
    notes_load();
    view_y = today_y;
    view_m = today_m;
    sel_d = today_d;
    int read_at = ticks();

    ui_input in;
    memset(&in, 0, sizeof(in));
    static char said[96];

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

        /* Today again once a minute: a calendar left open overnight. */
        if (ticks() - read_at >= 6000) { read_today(); read_at = ticks(); }
        ui_due(read_at + 6000);

        for (int ki = 0; ki < in.nkeys; ki++) {
            u32 k = in.keys[ki];
            if (note_field.focused) {
                if (k == '\n') keep_note();
                else if (k == 27) note_field.focused = 0;          /* left as it was */
                else ui_field_key(&note_field, k);
                continue;
            }
            if (k == KEY_LEFT) move_days(-1);
            else if (k == KEY_RIGHT) move_days(1);
            else if (k == KEY_UP) move_days(-7);
            else if (k == KEY_DOWN) move_days(7);
            else if (k == KEY_PAGE_UP) move_months(-1);
            else if (k == KEY_PAGE_DOWN) move_months(1);
            else if (k == KEY_HOME) { view_y = today_y; view_m = today_m; sel_d = today_d; }
            else if (k == '\n') begin_note();
            else if (k == KEY_DELETE) {
                note_buf[0] = 0;
                keep_note();
            }
        }

        fill(&s, t.bg);

        /* --- the month, and the way to the next ------------------------- */
        int y = UI_PAD;
        if (ui_button(&s, &in, &t, UI_PAD, y, 40, "<")) move_months(-1);
        if (ui_button(&s, &in, &t, w - UI_PAD - 40, y, 40, ">")) move_months(1);
        if (ui_button(&s, &in, &t, w - UI_PAD - 40 - UI_GAP - 70, y, 70, "today")) {
            view_y = today_y; view_m = today_m; sel_d = today_d;
        }
        char title[32];
        int tn = 0;
        for (const char *p = MONTHS[view_m - 1]; *p; p++) title[tn++] = *p;
        title[tn++] = ' ';
        tn += utoa((u32)view_y, title + tn);
        title[tn] = 0;
        face_draw(&s, UI_PAD + 40 + UI_GAP * 2, y + (UI_BTN_H - face_h(UI_FACE_BOLD)) / 2, title, t.fg, UI_FACE_BOLD);
        y += UI_BTN_H + UI_PAD;

        /* --- the grid ------------------------------------------------------ */
        int gw = w - UI_PAD * 2;
        int cw = gw / 7;
        int ch = (h - y - 120) / 7;
        if (ch < 22) ch = 22;
        for (int c = 0; c < 7; c++)
            face_draw(&s, UI_PAD + c * cw + (cw - face_w(DAYS[c], UI_FACE_BODY)) / 2, y + 4, DAYS[c],
                      c >= 5 ? t.dim : t.fg, UI_FACE_BODY);
        y += ch;
        int first = weekday(view_y, view_m, 1), n = days_in(view_y, view_m);
        char note[NOTE_MAX];
        for (int d = 1; d <= n; d++) {
            int cell = first + d - 1;
            int cx = UI_PAD + (cell % 7) * cw, cy = y + (cell / 7) * ch;
            int is_today = view_y == today_y && view_m == today_m && d == today_d;
            int chosen = d == sel_d;
            if (chosen) round_rect(&s, cx + 2, cy + 2, cw - 4, ch - 4, 6, mix(t.bg, t.accent, 70));
            if (is_today) {
                int r = (ch < cw ? ch : cw) / 2 - 4;
                disc(&s, cx + cw / 2, cy + ch / 2, r, t.accent);
            }
            char num[4];
            num[utoa((u32)d, num)] = 0;
            face_draw(&s, cx + (cw - face_w(num, UI_FACE_BODY)) / 2, cy + (ch - face_h(UI_FACE_BODY)) / 2, num,
                      is_today ? RGB(0xff, 0xff, 0xff) : t.fg, UI_FACE_BODY);
            if (note_of(view_y, view_m, d, note, sizeof(note)))
                disc(&s, cx + cw / 2, cy + ch - 6, 2, is_today ? RGB(0xff, 0xff, 0xff) : t.accent);
            /* A click chooses a day; a click on the day chosen writes its note. */
            if (in.released && in.mx >= cx && in.mx < cx + cw && in.my >= cy && in.my < cy + ch) {
                in.released = 0;
                if (chosen) begin_note();
                else sel_d = d;
            }
        }
        y += 6 * ch + UI_GAP;

        /* --- the day chosen, and its note ---------------------------------- */
        char day[48];
        int dn = 0;
        dn += utoa((u32)sel_d, day + dn);
        day[dn++] = ' ';
        for (const char *p = MONTHS[view_m - 1]; *p; p++) day[dn++] = *p;
        day[dn++] = ' ';
        dn += utoa((u32)view_y, day + dn);
        day[dn] = 0;
        face_draw(&s, UI_PAD, y, day, t.fg, UI_FACE_BOLD);
        y += face_h(UI_FACE_BOLD) + UI_GAP;
        if (note_field.focused) {
            ui_field_draw(&s, &in, &t, UI_PAD, y, w - UI_PAD * 2, &note_field, "a note for this day");
        } else if (note_of(view_y, view_m, sel_d, note, sizeof(note))) {
            ui_label(&s, &t, UI_PAD, y + 6, note);
        } else {
            ui_dim_label(&s, &t, UI_PAD, y + 6, "No note. Return writes one.");
        }

        say_chosen();
        if (strcmp(status, said)) {
            strcpy(said, status);
            puts("calendar: ");
            puts(said);
            puts("\n");
        }

        win_commit(win);
        ui_wait(win);
    }

    win_close(win);
    exit(0);
}
