/* A clock: the time here and in other places, a stopwatch, and a timer.
 *
 * The time comes from /sys/time: here is its local line, and the others
 * are worked out from its first line (UTC, as seconds since 1970) and the
 * table of zones the kernel and the browser share (zones.h), with each
 * place's summer by its own rule. The places are kept in /home/clocks, a
 * zone name a line, so the list is the same next time; London, New York
 * and Tokyo until one is added or taken away.
 *
 * The stopwatch and the timer count the machine's ticks, a hundred a
 * second. The timer, when it reaches nought, rings (three short notes, if
 * there is a sound card) and says so on the desktop as well.
 *
 * Keys: Tab goes between the two fields (a place to add, the timer's
 * length) and Escape leaves them; Space starts and stops the stopwatch and
 * R puts it back to nought. Return in the first field adds the place whose
 * name has what was typed in it; in the second it starts the timer, "3"
 * for three minutes or "1:30" for a minute and a half. What is shown is
 * said on the console as well ("clock: here 14:05; London 14:05; ..."),
 * for clockcheck.py.
 */
#include "zelr.h"
#include "ui.h"
#include "zones.h"

#define CITIES_FILE "/home/clocks"
#define CITIES_MAX  8

static int cities[CITIES_MAX];
static int ncities;

static char here_time[16] = "--:--";
static char here_zone[40] = "UTC";
static long long now_utc;               /* seconds since 1970, from /sys/time */

static int sw_running;
static int sw_from;                     /* the tick it was started at */
static int sw_kept;                     /* ticks counted before that */

static int tm_running;
static int tm_end;                      /* the tick it rings at */

static char city_buf[32], timer_buf[16];
static ui_field city_field, timer_field;

/* --- names and times ---------------------------------------------------------- */

/* "Europe/London" as "London", "America/New_York" as "New York". */
static void place_name(int z, char *out, int cap) {
    const char *n = ZONES[z].name, *leaf = n;
    for (const char *p = n; *p; p++) if (*p == '/') leaf = p + 1;
    int k = 0;
    for (const char *p = leaf; *p && k < cap - 1; p++) out[k++] = *p == '_' ? ' ' : *p;
    out[k] = 0;
}

static int num_at(const char *p, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

/* The value after a word at the start of a line of /sys/time. */
static const char *field_of(const char *t, const char *word) {
    int n = strlen(word);
    for (const char *p = t; *p; ) {
        if (!strncmp(p, word, n) && p[n] == ' ') {
            p += n;
            while (*p == ' ') p++;
            return p;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return 0;
}

static void read_time(void) {
    char t[512];
    int n = slurp("/sys/time", t, sizeof(t) - 1);
    if (n <= 0) return;
    t[n] = 0;
    const char *now = field_of(t, "now");
    if (now) {
        int y = num_at(now, 4), mo = num_at(now + 5, 2), d = num_at(now + 8, 2);
        int h = num_at(now + 11, 2), mi = num_at(now + 14, 2), s = num_at(now + 17, 2);
        if (y > 1900 && mo > 0 && d > 0 && h >= 0 && mi >= 0 && s >= 0)
            now_utc = zone_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    }
    const char *local = field_of(t, "local");
    if (local) {
        for (int i = 0; i < 5; i++) here_time[i] = local[11 + i];
        here_time[5] = 0;
    } else if (now) {
        for (int i = 0; i < 5; i++) here_time[i] = now[11 + i];
        here_time[5] = 0;
    }
    const char *zone = field_of(t, "zone");
    if (zone) {
        int k = 0;
        while (zone[k] && zone[k] != '\n' && k < (int)sizeof(here_zone) - 1) { here_zone[k] = zone[k]; k++; }
        here_zone[k] = 0;
    }
}

/* "23:05" for a zone at this moment, by its own rule for summer. */
static void time_in(int z, char *out) {
    long long t = now_utc + (long long)zone_offset_at(&ZONES[z], now_utc) * 60;
    long long s = t % 86400;
    if (s < 0) s += 86400;
    int h = (int)(s / 3600), m = (int)(s / 60 % 60);
    out[0] = (char)('0' + h / 10); out[1] = (char)('0' + h % 10);
    out[2] = ':';
    out[3] = (char)('0' + m / 10); out[4] = (char)('0' + m % 10);
    out[5] = 0;
}

/* --- the places ---------------------------------------------------------------- */

static void cities_save(void) {
    char out[CITIES_MAX * 40];
    int w = 0;
    for (int i = 0; i < ncities; i++) {
        for (const char *p = ZONES[cities[i]].name; *p; p++) out[w++] = *p;
        out[w++] = '\n';
    }
    mkdir("/home");
    spit(CITIES_FILE, out, w);
}

static void cities_load(void) {
    char t[CITIES_MAX * 48];
    int n = slurp(CITIES_FILE, t, sizeof(t) - 1);
    ncities = 0;
    if (n > 0) {
        t[n] = 0;
        for (char *p = t; *p && ncities < CITIES_MAX; ) {
            char *e = p;
            while (*e && *e != '\n') e++;
            char was = *e;
            *e = 0;
            int z = zone_find(p);
            if (z >= 0) cities[ncities++] = z;
            *e = was;
            p = *e ? e + 1 : e;
        }
        return;
    }
    static const char *const START[] = { "Europe/London", "America/New_York", "Asia/Tokyo" };
    for (int i = 0; i < 3; i++) {
        int z = zone_find(START[i]);
        if (z >= 0) cities[ncities++] = z;
    }
}

static int contains_fold(const char *hay, const char *needle) {
    int n = strlen(needle);
    if (!n) return 0;
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (k < n && hay[i + k] && zone_low(hay[i + k]) == zone_low(needle[k])) k++;
        if (k == n) return 1;
    }
    return 0;
}

static void add_city(void) {
    if (!city_buf[0]) return;
    char name[40];
    for (int z = 0; ZONES[z].name; z++) {
        place_name(z, name, sizeof(name));
        if (!contains_fold(name, city_buf) && !contains_fold(ZONES[z].name, city_buf)) continue;
        for (int i = 0; i < ncities; i++) if (cities[i] == z) { puts("clock: already shown\n"); return; }
        if (ncities >= CITIES_MAX) { puts("clock: no room for another place\n"); return; }
        cities[ncities++] = z;
        cities_save();
        puts("clock: added ");
        puts(name);
        puts("\n");
        city_buf[0] = 0;
        city_field.len = city_field.cursor = 0;
        return;
    }
    puts("clock: no place of that name\n");
}

static void take_city(int i) {
    char name[40];
    place_name(cities[i], name, sizeof(name));
    for (int k = i; k + 1 < ncities; k++) cities[k] = cities[k + 1];
    ncities--;
    cities_save();
    puts("clock: took away ");
    puts(name);
    puts("\n");
}

/* --- the stopwatch and the timer -------------------------------------------------- */

static int sw_ticks(void) { return sw_kept + (sw_running ? ticks() - sw_from : 0); }

/* "1:02.3", minutes, seconds and tenths. */
static void sw_text(int t, char *out) {
    int tenths = t / 10, s = tenths / 10, m = s / 60;
    int k = utoa((u32)m, out);
    out[k++] = ':';
    out[k++] = (char)('0' + s % 60 / 10);
    out[k++] = (char)('0' + s % 10);
    out[k++] = '.';
    out[k++] = (char)('0' + tenths % 10);
    out[k] = 0;
}

static void sw_toggle(void) {
    char t[16];
    if (sw_running) {
        sw_kept = sw_ticks();
        sw_running = 0;
        sw_text(sw_kept, t);
        puts("clock: stopwatch stopped at ");
        puts(t);
        puts("\n");
    } else {
        sw_from = ticks();
        sw_running = 1;
        puts("clock: stopwatch started\n");
    }
}

static void sw_reset(void) {
    sw_running = 0;
    sw_kept = 0;
    puts("clock: stopwatch back to nought\n");
}

/* "3" is three minutes, "1:30" a minute and a half; 0 for nothing. */
static int timer_seconds(void) {
    int m = 0, s = 0, colon = 0;
    for (const char *p = timer_buf; *p; p++) {
        if (*p == ':') { colon = 1; continue; }
        if (*p < '0' || *p > '9') return 0;
        if (colon) s = s * 10 + (*p - '0');
        else m = m * 10 + (*p - '0');
    }
    return m * 60 + s;
}

static void timer_start(void) {
    int secs = timer_seconds();
    if (secs <= 0 || secs > 24 * 3600) { puts("clock: a timer is minutes, or minutes:seconds\n"); return; }
    tm_end = ticks() + secs * 100;
    tm_running = 1;
    timer_field.focused = 0;
    char t[16];
    int k = utoa((u32)secs, t);
    t[k] = 0;
    puts("clock: the timer started, ");
    puts(t);
    puts(" seconds\n");
}

/* Three short notes, if there is a card to play them. */
static void ring(void) {
    zelr_sound si;
    if (sound_info(&si) < 0 || !si.present || !si.rate) return;
    static short frames[2 * 4800];
    int ch = si.channels ? (int)si.channels : 2;
    if (ch > 2) ch = 2;
    int n = (int)si.rate / 10;                  /* a tenth of a second */
    if (n > 4800) n = 4800;
    int half = (int)si.rate / 880 / 2;
    if (half < 1) half = 1;
    for (int note = 0; note < 3; note++) {
        for (int i = 0; i < n; i++) {
            short v = (short)(((i / half) & 1) ? 5000 : -5000);
            for (int c = 0; c < ch; c++) frames[i * ch + c] = note < 3 ? v : 0;
        }
        sound_write(frames, n);
        for (int i = 0; i < n * ch; i++) frames[i] = 0;
        sound_write(frames, n);
    }
}

static void timer_done(void) {
    tm_running = 0;
    puts("clock: the timer is done\n");
    desktop_notice("the timer is done");
    ring();
}

/* --- the window ------------------------------------------------------------------- */

int main(void) {
    int win = win_create("Clock", 440, 500);
    if (win < 0) exit(1);
    win_allow_resize(win);
    win_want_escape(win);

    memset(&city_field, 0, sizeof(city_field));
    city_field.buf = city_buf;
    city_field.cap = sizeof(city_buf);
    memset(&timer_field, 0, sizeof(timer_field));
    timer_field.buf = timer_buf;
    timer_field.cap = sizeof(timer_buf);

    cities_load();
    read_time();
    int read_at = ticks();

    ui_input in;
    memset(&in, 0, sizeof(in));
    static char said[400];

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

        if (ticks() - read_at >= 100) { read_time(); read_at = ticks(); }
        if (tm_running && ticks() >= tm_end) timer_done();

        for (int ki = 0; ki < in.nkeys; ki++) {
            u32 k = in.keys[ki];
            if (k == '\t') {
                int was_city = city_field.focused;
                city_field.focused = !was_city;
                timer_field.focused = was_city;
                continue;
            }
            if (city_field.focused || timer_field.focused) {
                ui_field *f = city_field.focused ? &city_field : &timer_field;
                if (k == 27) city_field.focused = timer_field.focused = 0;
                else if (k == '\n') { if (f == &city_field) add_city(); else timer_start(); }
                else ui_field_key(f, k);
                continue;
            }
            if (k == ' ') sw_toggle();
            else if (k == 'r' || k == 'R') sw_reset();
        }

        fill(&s, t.bg);
        int y = UI_PAD;

        /* --- here ----------------------------------------------------------- */
        face_draw(&s, UI_PAD, y, here_time, t.fg, UI_FACE_HEAD);
        face_draw(&s, UI_PAD + face_w(here_time, UI_FACE_HEAD) + UI_GAP * 2, y + 5, here_zone, t.dim, UI_FACE_BODY);
        y += face_h(UI_FACE_HEAD) + UI_PAD;

        /* --- other places ------------------------------------------------------ */
        y = ui_section(&s, &t, UI_PAD, y, w - UI_PAD * 2, "Other places");
        char line[400];
        int ln = 0;
        for (const char *p = "here "; *p; p++) line[ln++] = *p;
        for (const char *p = here_time; *p; p++) line[ln++] = *p;
        for (int i = 0; i < ncities; i++) {
            char name[40], when[8];
            place_name(cities[i], name, sizeof(name));
            time_in(cities[i], when);
            /* A row as tall as its button and a little, so the buttons do
               not touch (clockcheck.py clicks them at this height). */
            int ty = y + (UI_BTN_H - face_h(UI_FACE_BODY)) / 2;
            ui_label(&s, &t, UI_PAD, ty, name);
            face_draw(&s, w - UI_PAD - 34 - UI_GAP - face_w(when, UI_FACE_BOLD), ty, when, t.fg, UI_FACE_BOLD);
            if (ui_button(&s, &in, &t, w - UI_PAD - 34, y, 34, "x")) { take_city(i); break; }
            y += UI_BTN_H + 4;
            line[ln++] = ';';
            line[ln++] = ' ';
            for (const char *p = name; *p && ln < (int)sizeof(line) - 10; p++) line[ln++] = *p;
            line[ln++] = ' ';
            for (const char *p = when; *p; p++) line[ln++] = *p;
        }
        line[ln] = 0;
        ui_field_draw(&s, &in, &t, UI_PAD, y + 2, w - UI_PAD * 3 - 70, &city_field, "add a place");
        if (ui_button(&s, &in, &t, w - UI_PAD - 70, y + 2, 70, "add")) add_city();
        y += UI_BTN_H + UI_PAD * 2;

        /* --- the stopwatch ----------------------------------------------------- */
        y = ui_section(&s, &t, UI_PAD, y, w - UI_PAD * 2, "Stopwatch");
        char sw[16];
        sw_text(sw_ticks(), sw);
        face_draw(&s, UI_PAD, y + 2, sw, t.fg, UI_FACE_HEAD);
        int bx = w - UI_PAD - 80;
        if (ui_button(&s, &in, &t, bx, y, 80, "reset")) sw_reset();
        bx -= 80 + UI_GAP;
        if (ui_button_primary(&s, &in, &t, bx, y, 80, sw_running ? "stop" : "start")) sw_toggle();
        y += UI_BTN_H + UI_PAD * 2;

        /* --- the timer -------------------------------------------------------- */
        y = ui_section(&s, &t, UI_PAD, y, w - UI_PAD * 2, "Timer");
        if (tm_running) {
            int left = (tm_end - ticks() + 99) / 100;
            if (left < 0) left = 0;
            char tl[16];
            int k = utoa((u32)(left / 60), tl);
            tl[k++] = ':';
            tl[k++] = (char)('0' + left % 60 / 10);
            tl[k++] = (char)('0' + left % 10);
            tl[k] = 0;
            face_draw(&s, UI_PAD, y + 2, tl, t.fg, UI_FACE_HEAD);
            if (ui_button(&s, &in, &t, w - UI_PAD - 80, y, 80, "stop")) {
                tm_running = 0;
                puts("clock: the timer stopped\n");
            }
        } else {
            ui_field_draw(&s, &in, &t, UI_PAD, y, 120, &timer_field, "minutes");
            if (ui_button_primary(&s, &in, &t, UI_PAD + 120 + UI_GAP, y, 80, "start")) timer_start();
            ui_dim_label(&s, &t, UI_PAD + 210, y + 7, "3, or 1:30");
        }

        if (strcmp(line, said)) {
            strcpy(said, line);
            puts("clock: ");
            puts(said);
            puts("\n");
        }

        /* Redrawn as often as what is shown changes: tenths while the
           stopwatch runs, seconds while the timer does, else when the
           clock is next read. */
        if (sw_running) ui_due(ticks() + 10);
        else if (tm_running) ui_due(tm_end < ticks() + 50 ? tm_end : ticks() + 50);
        else ui_due(read_at + 100);

        win_commit(win);
        ui_wait(win);
    }

    win_close(win);
    exit(0);
}
