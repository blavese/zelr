/* What the machine is doing, while it does it.
 *
 * Everything here comes from two system calls and one file. sysinfo says how
 * much memory is in use, how many processors were found and started, how long
 * the machine has been up and how many system calls it has answered; tasks
 * walks the scheduler's list. Nothing is privileged: a program written by
 * anyone else could show exactly this.
 *
 * The one number that takes any working out is how busy the processor is.
 * The scheduler keeps a slice count per task, which it increments on every
 * timer tick for whichever task it picked, and there is a tick count that
 * goes up whether anything is running or not. The share of ticks handed out
 * looks like the answer and is not: this kernel is round robin, so a task
 * sitting in a loop waiting for a key is picked every tick and spends the
 * slice halted. Measured that way an idle machine reads a hundred per cent,
 * which is what it did until this was looked at.
 *
 * So a waiting loop says so, with task_idle_wait, and the kernel counts the
 * ticks it slept through. Slices minus those is work, and an idle machine
 * reads nothing.
 */
#include "zelr.h"
#include "ui.h"

#define MAX_TASKS   32
#define HISTORY     120          /* samples in the graph */
#define SAMPLE_MS   250

typedef struct {
    u32  pid;
    u32  state;
    u32  slices;
    u32  idle;
    u32  user;
    char name[64];
    u32  was;                    /* slices at the previous sample */
    u32  was_idle;
    u32  share;                  /* per cent of the last interval */
} row_t;

static row_t rows[MAX_TASKS];
static int   nrows;

static u8  history[HISTORY];     /* per cent busy, oldest first */
static int history_n;

static int  selected = -1;
static int  last_tick;
static int  last_total;          /* slices across everything, last sample */

static zelr_sysinfo info;

/* --- the sample ----------------------------------------------------------- */

static row_t *find(u32 pid) {
    for (int i = 0; i < nrows; i++)
        if (rows[i].pid == pid) return &rows[i];
    return 0;
}

static void push_history(int busy) {
    if (history_n < HISTORY) {
        history[history_n++] = (u8)busy;
        return;
    }
    for (int i = 1; i < HISTORY; i++) history[i - 1] = history[i];
    history[HISTORY - 1] = (u8)busy;
}

/* Reads the scheduler's list into rows, keeping what each task's slice count
   was last time so the difference is what it did since. */
static void sample(void) {
    int now = ticks();
    int span = now - last_tick;
    if (span <= 0) span = 1;

    row_t fresh[MAX_TASKS];
    int n = 0;
    int total = 0;

    zelr_task t;
    for (int i = 0; i < MAX_TASKS && tasks(i, &t) == 1; i++) {
        row_t *old = find(t.pid);
        row_t *r = &fresh[n++];

        r->pid = t.pid;
        r->state = t.state;
        r->slices = t.slices;
        r->idle = t.idle;
        r->user = t.user;
        int k = 0;
        while (k < 63 && t.name[k]) { r->name[k] = t.name[k]; k++; }
        r->name[k] = 0;

        r->was = old ? old->slices : t.slices;
        r->was_idle = old ? old->idle : t.idle;

        /* A slice is a tick the scheduler gave this task, which is not the
           same as a tick it used: a loop waiting for a key is handed one
           every time round and spends it halted. What it did is the slices
           it was given less the ones it said it was only waiting through. */
        int did = (int)(r->slices - r->was) - (int)(r->idle - r->was_idle);
        if (did < 0) did = 0;
        r->share = (u32)(did * 100 / span);
        if (r->share > 100) r->share = 100;
        total += did;
    }

    for (int i = 0; i < n; i++) rows[i] = fresh[i];
    nrows = n;

    int busy = total * 100 / span;
    if (busy > 100) busy = 100;
    push_history(busy);

    last_total = total;
    last_tick = now;
    sysinfo(&info);
}

static int busy_now(void) {
    return history_n ? history[history_n - 1] : 0;
}

static const char *state_name(u32 s) {
    switch (s) {
        case TASK_READY:    return "ready";
        case TASK_RUNNING:  return "running";
        case TASK_SLEEPING: return "sleeping";
        case TASK_BLOCKED:  return "waiting";
        default:            return "dead";
    }
}

/* --- pieces of the window ------------------------------------------------- */

/* A number with a word under it, on a card of its own. Four of these across
   the top is the whole of what somebody wants at a glance. */
static void card(surface *s, ui_theme *t, int x, int y, int w, int h,
                 const char *big, const char *small, u32 tint) {
    round_rect(s, x, y, w, h, UI_RADIUS, t->panel);
    rect(s, x, y, w, 2, tint);

    face_draw(s, x + UI_PAD, y + 10, big, t->fg, UI_FACE_HEAD);
    face_draw(s, x + UI_PAD, y + h - face_h(UI_FACE_SMALL) - 8, small,
              t->dim, UI_FACE_SMALL);
}

static void bar(surface *s, ui_theme *t, int x, int y, int w, int h,
                int part, int whole, u32 tint) {
    round_rect(s, x, y, w, h, h / 2, mix(t->panel, 0, 40));
    if (whole <= 0) return;
    int on = w * part / whole;
    if (on > w) on = w;
    if (on > 0) round_rect(s, x, y, on, h, h / 2, tint);
}

/* The graph. A column per sample, tall for busy, drawn from the right so the
   newest is always against the same edge and the eye can follow it. */
static void graph(surface *s, ui_theme *t, int x, int y, int w, int h) {
    round_rect(s, x, y, w, h, UI_RADIUS, t->panel);

    /* Lines at a quarter, a half and three quarters, so the height of a
       column means something without a scale down the side. */
    for (int i = 1; i < 4; i++)
        rect(s, x + 6, y + h * i / 4, w - 12, 1, mix(t->panel, t->fg, 18));

    int cols = w - 12;
    if (cols > HISTORY) cols = HISTORY;

    for (int i = 0; i < cols; i++) {
        int v = history[history_n - 1 - i];
        if (history_n - 1 - i < 0) break;

        int ch = (h - 12) * v / 100;
        if (ch < 1 && v) ch = 1;
        int cx = x + w - 6 - i;
        u32 c = mix(t->accent, t->warn, v > 80 ? (v - 80) * 5 : 0);
        if (ch) rect(s, cx, y + h - 6 - ch, 1, ch, c);
    }
}

/* --- the window ----------------------------------------------------------- */

void _start(void) {
    int win = win_create("Monitor", 640, 560);
    if (win < 0) exit(1);
    win_allow_resize(win);

    last_tick = ticks();
    sample();

    ui_input in;
    memset(&in, 0, sizeof(in));
    int next = ticks();
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

        if (ticks() - next >= SAMPLE_MS / 10) {
            next = ticks();
            sample();
        }

        fill(&s, t.bg);

        /* --- the four numbers --------------------------------------------- */
        int pad = UI_PAD * 2;
        int cw = (w - pad * 2 - UI_GAP * 3) / 4;
        int cy = pad;
        int chh = 62;
        char big[32], small[64];

        int n = utoa((u32)busy_now(), big);
        big[n++] = '%'; big[n] = 0;
        card(&s, &t, pad, cy, cw, chh, big, "processor", t.accent);

        int used_mb = (int)(info.mem_used_kb / 1024);
        int total_mb = (int)(info.mem_total_kb / 1024);
        n = utoa((u32)used_mb, big);
        big[n++] = ' '; big[n++] = 'M'; big[n++] = 'B'; big[n] = 0;
        n = 0;
        const char *of = "of ";
        for (int i = 0; of[i]; i++) small[n++] = of[i];
        n += utoa((u32)total_mb, small + n);
        small[n++] = ' '; small[n++] = 'M'; small[n++] = 'B'; small[n] = 0;
        card(&s, &t, pad + (cw + UI_GAP), cy, cw, chh, big, small,
             mix(t.accent, t.warn, used_mb * 255 / (total_mb ? total_mb : 1)));

        n = utoa((u32)nrows, big);
        big[n] = 0;
        card(&s, &t, pad + (cw + UI_GAP) * 2, cy, cw, chh, big, "tasks",
             t.accent);

        u32 up = info.uptime_seconds;
        n = utoa(up / 60, big);
        big[n++] = 'm'; big[n] = 0;
        n = utoa(up % 60, small);
        small[n++] = 's'; small[n++] = ' ';
        small[n++] = 'u'; small[n++] = 'p'; small[n] = 0;
        card(&s, &t, pad + (cw + UI_GAP) * 3, cy, cw, chh, big, small,
             t.accent);

        /* --- the graph ----------------------------------------------------- */
        int gy = cy + chh + UI_GAP * 2;
        int gh = 110;
        graph(&s, &t, pad, gy, w - pad * 2, gh);
        face_draw(&s, pad + 10, gy + 8, "processor, last thirty seconds",
                  t.dim, UI_FACE_SMALL);

        /* --- memory -------------------------------------------------------- */
        int my = gy + gh + UI_GAP * 2;
        face_draw(&s, pad, my, "memory", t.dim, UI_FACE_SMALL);
        bar(&s, &t, pad, my + 18, w - pad * 2, 10,
            (int)info.mem_used_kb, (int)info.mem_total_kb, t.accent);

        n = 0;
        n += utoa(info.heap_total_kb ? info.heap_total_kb : 0, small + n);
        small[n] = 0;
        char line[96];
        int k = 0;
        const char *lbl = "heap ";
        for (int i = 0; lbl[i]; i++) line[k++] = lbl[i];
        k += utoa(info.heap_total_kb, line + k);
        const char *kib = " KiB, ";
        for (int i = 0; kib[i]; i++) line[k++] = kib[i];
        k += utoa(info.cpus_started, line + k);
        const char *cpus = " of ";
        for (int i = 0; cpus[i]; i++) line[k++] = cpus[i];
        k += utoa(info.cpus_found, line + k);
        const char *run = " processors running, ";
        for (int i = 0; run[i]; i++) line[k++] = run[i];
        k += utoa(info.syscalls, line + k);
        const char *sc = " system calls";
        for (int i = 0; sc[i]; i++) line[k++] = sc[i];
        line[k] = 0;
        face_draw(&s, pad, my + 34, line, t.dim, UI_FACE_SMALL);

        /* --- the tasks ------------------------------------------------------ */
        int ly = my + 34 + face_h(UI_FACE_SMALL) + UI_GAP * 2;
        face_draw(&s, pad, ly, "what is running", t.dim, UI_FACE_SMALL);
        ly += face_h(UI_FACE_SMALL) + UI_GAP;

        int list_h = h - ly - UI_ROW - UI_PAD;
        int shown = list_h / UI_ROW;
        if (shown < 1) shown = 1;

        if (in.scroll) scroll += in.scroll;
        if (scroll > nrows - shown) scroll = nrows - shown;
        if (scroll < 0) scroll = 0;

        for (int i = 0; i < shown && scroll + i < nrows; i++) {
            row_t *r = &rows[scroll + i];
            int ry = ly + i * UI_ROW;
            int over = ui_hit(&in, pad, ry, w - pad * 2, UI_ROW);

            if (scroll + i == selected)
                round_rect(&s, pad, ry, w - pad * 2, UI_ROW, 4, t.soft);
            else if (over)
                round_rect(&s, pad, ry, w - pad * 2, UI_ROW, 4,
                           mix(t.bg, t.fg, 14));

            if (over && in.released) { in.released = 0; selected = scroll + i; }

            int ty = ry + (UI_ROW - face_h(UI_FACE_BODY)) / 2;
            char num[16];
            utoa(r->pid, num);
            face_draw(&s, pad + 8, ty, num, t.dim, UI_FACE_BODY);
            face_draw(&s, pad + 44, ty, r->name, t.fg, UI_FACE_BODY);
            face_draw(&s, pad + 300, ty, state_name(r->state),
                      r->state == TASK_RUNNING ? t.accent : t.dim,
                      UI_FACE_BODY);

            /* What share of the last interval this one had, as a bar rather
               than a number, because the shape of it reads faster. */
            int bx = w - pad - 130;
            bar(&s, &t, bx, ry + UI_ROW / 2 - 3, 90, 6,
                (int)r->share, 100, r->user ? t.accent : t.dim);

            int pn = utoa(r->share, num);
            num[pn++] = '%'; num[pn] = 0;
            face_draw(&s, w - pad - 32, ty, num, t.dim, UI_FACE_SMALL);
        }

        /* --- and the one thing it can do ------------------------------------ */
        if (selected >= 0 && selected < nrows) {
            int by = h - UI_ROW - UI_PAD + 2;
            char label[48];
            int ln = 0;
            const char *stop = "Stop ";
            for (int i = 0; stop[i]; i++) label[ln++] = stop[i];
            for (int i = 0; rows[selected].name[i] && ln < 40; i++)
                label[ln++] = rows[selected].name[i];
            label[ln] = 0;

            if (ui_button(&s, &in, &t, pad, by - UI_BTN_H + UI_ROW, 200, label)
                && rows[selected].pid != (u32)getpid()) {
                kill((int)rows[selected].pid);
                selected = -1;
                sample();
            }
        }

        ui_statusbar(&s, &t, w, h, "updated four times a second", "Monitor");
        win_commit(win);
        sleep_ms(60);
    }

    win_close(win);
    exit(0);
}
