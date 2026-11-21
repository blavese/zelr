/* A terminal, running in ring 3.
 *
 * This is a shell that is not part of the kernel. It has a window, a
 * scrollback buffer, a line editor and a command set, and every single thing
 * it does goes through int 0x80: listing a directory, reading a file, writing
 * one, starting another program, and fetching a page over TCP. The kernel's
 * own shell still exists on the serial console, but nothing here calls into
 * it.
 *
 * It is deliberately not a terminal emulator. There is no pty and no escape
 * sequence parser; a line carries its own colour, which is all the colour
 * anything here needs. Commands run inline and print into the buffer.
 *
 * The parts, in order: the scrollback, the line editor, the command table,
 * and the loop that feeds one to the other.
 */
#include "zelr.h"
#include "zones.h"
#include "draw.h"
#include "sha256.h"
#include "arcio.h"

#define COLS      160
#define MAX_LINES 400
#define PAD       8
#define MAX_HIST  64

/* What the kernel will accept, so a path built here can always be passed
   back to it. */
#define VFS_PATH  ZELR_PATH_MAX
#define VFS_NAME  32

/* Roughly one terminal's worth of file. Anything larger is printed in full
   but never held in one piece. */
#define IO_BUF 65536

#define HISTORY_FILE "/cfg/history"
#define THEME_FILE   "/cfg/term"

/* --- colours ------------------------------------------------------------

   Six colours is the whole palette: two for text, one for anything the
   terminal is saying about itself, one for a name that means something, one
   for a problem, and the background. Everything printed picks one of them,
   which is why there is no escape sequence parser here. */

typedef struct {
    const char *name;
    u32 bg, fg, dim, accent, warn, cursor;
} palette;

static const palette PALETTES[] = {
    { "slate",
      RGB(0x10, 0x14, 0x1A), RGB(0xC8, 0xD2, 0xDA), RGB(0x6B, 0x7A, 0x87),
      RGB(0x5E, 0xD1, 0xA0), RGB(0xE0, 0x7A, 0x6A), RGB(0x5E, 0xD1, 0xA0) },
    { "paper",
      RGB(0xF2, 0xEE, 0xE4), RGB(0x2A, 0x28, 0x24), RGB(0x8A, 0x84, 0x78),
      RGB(0x1F, 0x6F, 0x8B), RGB(0xB0, 0x3A, 0x2E), RGB(0x1F, 0x6F, 0x8B) },
    { "amber",
      RGB(0x14, 0x0E, 0x04), RGB(0xFF, 0xB0, 0x30), RGB(0x8A, 0x5E, 0x18),
      RGB(0xFF, 0xE0, 0x80), RGB(0xFF, 0x60, 0x40), RGB(0xFF, 0xB0, 0x30) },
    { "phosphor",
      RGB(0x02, 0x0A, 0x02), RGB(0x40, 0xE0, 0x50), RGB(0x1E, 0x70, 0x28),
      RGB(0xA0, 0xFF, 0xB0), RGB(0xFF, 0x70, 0x50), RGB(0x40, 0xE0, 0x50) },
    { "ink",
      RGB(0x0A, 0x0C, 0x18), RGB(0xCF, 0xD4, 0xEE), RGB(0x63, 0x6C, 0x92),
      RGB(0x9A, 0x86, 0xE8), RGB(0xE8, 0x7B, 0x9B), RGB(0x9A, 0x86, 0xE8) },
};
#define N_PALETTES ((int)(sizeof(PALETTES) / sizeof(PALETTES[0])))

static palette pal;

/* The colour a line was printed in is stored as which of the six it was, not
   as the value, so switching palette recolours everything already on screen
   rather than only what comes next. */
#define C_FG     0
#define C_DIM    1
#define C_ACCENT 2
#define C_WARN   3

static u32 colour_of(u8 slot) {
    switch (slot) {
        case C_DIM:    return pal.dim;
        case C_ACCENT: return pal.accent;
        case C_WARN:   return pal.warn;
        default:       return pal.fg;
    }
}

/* --- the scrollback ----------------------------------------------------- */

static char lines[MAX_LINES][COLS + 1];
static u8   slots[MAX_LINES];
static int  n_lines;            /* how many are in use, up to MAX_LINES */
static int  first;              /* index of the oldest line */
static int  view;               /* how many lines scrolled back from the end */

static surface scr;
static int win;
static int rows, cols;

/* --- selecting with the mouse ---------------------------------------------
 *
 * A terminal you cannot copy out of is a terminal you retype out of. The
 * selection is kept as two positions in the scrollback, each a line index
 * and a column, rather than as pixels: the window can be resized and the
 * view scrolled underneath a selection, and both of those move the pixels
 * without moving the text. */
static int sel_active;            /* there is a selection worth painting */
static int sel_dragging;
static int sel_l0, sel_c0;        /* where the drag started */
static int sel_l1, sel_c1;        /* where it is now */

/* Ordered, so the painter and the copier do not each work it out. */
static void sel_range(int *l0, int *c0, int *l1, int *c1) {
    if (sel_l0 < sel_l1 || (sel_l0 == sel_l1 && sel_c0 <= sel_c1)) {
        *l0 = sel_l0; *c0 = sel_c0; *l1 = sel_l1; *c1 = sel_c1;
    } else {
        *l0 = sel_l1; *c0 = sel_c1; *l1 = sel_l0; *c1 = sel_c0;
    }
}

static void sel_clear(void) { sel_active = 0; sel_dragging = 0; }

/* The line being typed, and where the cursor sits inside it. */
static char input[COLS + 1];
static int  in_off;                 /* the first column of it on screen */
static int  in_len, in_pos;

/* The cursor blinks half a second a phase, by the clock (ticks, a hundred a
   second) rather than by counting passes of the loop: the loop used to go
   round fifty times a second whether or not anything happened, and now it
   sleeps until something does or the cursor is due to change. A key starts
   the phase again, so the cursor is lit while somebody is typing. */
#define BLINK_TICKS 50
static int  blink_from;

static int cursor_lit(void) { return ((ticks() - blink_from) / BLINK_TICKS) % 2 == 0; }

/* What has been typed before. The newest is at the end; hist_at is where the
   up arrow has walked back to, counted from there. */
static char hist[MAX_HIST][COLS + 1];
static int  n_hist;
static int  hist_at;            /* 0 means the line being typed */
static char hist_stash[COLS + 1];   /* the line set aside while browsing */

/* ctrl+R: back through what has been typed for a line with some text in it,
   the newest first; ctrl+R again for the one before that. Return runs what
   was found, an arrow (or Home or End) takes it to the line to change, and
   ctrl+G, or Backspace with nothing to look for, lets it go. Escape is the
   desktop's, as the help says, so it is not the way out here. */
static int  hs_on;
static char hs_text[COLS + 1];
static int  hs_len;
static int  hs_found;           /* into hist: the line found, -1 for none */

static char *line_at(int i) { return lines[(first + i) % MAX_LINES]; }
static u8   *slot_at(int i) { return &slots[(first + i) % MAX_LINES]; }

static void push_line(const char *s, u8 slot) {
    int idx;
    if (n_lines < MAX_LINES) {
        idx = (first + n_lines) % MAX_LINES;
        n_lines++;
    } else {
        idx = first;
        first = (first + 1) % MAX_LINES;
    }
    strncpy(lines[idx], s, COLS + 1);
    slots[idx] = slot;
}

/* CMD > FILE: while one of the terminal's own commands runs with its output
   sent to a file, what it says is kept here instead of shown, a line each as
   it said it (unwrapped), and written when it is done. What goes wrong is
   still shown, as a shell keeps errors on the screen. */
static char cap_buf[65536];
static int  cap_n, capturing;

/* CMD | CMD: what a command before a | says is the input of the one after
   it, which reads it where it would have read a file (cat, head, tail, wc,
   grep and sort, given no file). There is one task, so each command's
   output is kept whole in cap_buf before the next one starts, and handed
   over here. */
static char pipe_buf[65536];
static int  pipe_n = -1;            /* -1: nothing is piped into this command */

/* Prints text, breaking it at newlines and wrapping anything too wide. */
static void print(const char *s, u8 slot) {
    if (capturing && slot != C_WARN) {
        for (int i = 0; s[i] && cap_n < (int)sizeof(cap_buf) - 1; i++) cap_buf[cap_n++] = s[i];
        if (cap_n < (int)sizeof(cap_buf) - 1) cap_buf[cap_n++] = '\n';
        return;
    }
    char buf[COLS + 1];
    int n = 0;
    for (int i = 0; ; i++) {
        char c = s[i];
        if (c == '\n' || c == 0 || n == cols) {
            buf[n] = 0;
            push_line(buf, slot);
            n = 0;
            if (c == 0) return;
            if (c == '\n') continue;
        }
        if (c == '\t') {
            while (n < cols && (n % 4)) buf[n++] = ' ';
            if (n < cols) buf[n++] = ' ';
            continue;
        }
        if (c == '\r') continue;
        if (c < 32 || c > 126) c = '.';
        buf[n++] = c;
    }
}

static void say(const char *s)  { print(s, C_FG); }
static void dim(const char *s)  { print(s, C_DIM); }
static void good(const char *s) { print(s, C_ACCENT); }
static void err(const char *s)  { print(s, C_WARN); }

/* Building a line out of pieces, which is most of what the commands do. */
static char work[COLS + 1];
static int  work_n;

static void w_reset(void) { work_n = 0; work[0] = 0; }
static void w_str(const char *s) {
    for (int i = 0; s[i] && work_n < COLS; i++) work[work_n++] = s[i];
    work[work_n] = 0;
}
static void w_ch(char c) { if (work_n < COLS) { work[work_n++] = c; work[work_n] = 0; } }
static void w_num(u32 v) {
    char tmp[12];
    utoa(v, tmp);
    w_str(tmp);
}
static void w_pad(int to) {
    while (work_n < to && work_n < COLS) work[work_n++] = ' ';
    work[work_n] = 0;
}
/* Right aligns a number in a column, which is what makes a listing readable. */
static void w_rnum(u32 v, int width) {
    char tmp[12];
    int n = utoa(v, tmp);
    for (int i = n; i < width; i++) w_ch(' ');
    w_str(tmp);
}

/* --- rendering ---------------------------------------------------------- */

/* Which line and column a point in the window lands on. The scrollback is
   drawn from `start`, so the row on screen is an offset from there rather
   than an index in its own right. */
static void point_to_cell(int x, int y, int *line, int *col) {
    int text_rows = rows - 1;
    int end = n_lines - view;
    if (end < 0) end = 0;
    int start = end - text_rows;
    if (start < 0) start = 0;

    int row = (y - PAD) / MONO_H;
    if (row < 0) row = 0;
    if (row >= text_rows) row = text_rows - 1;

    int c = (x - PAD) / MONO_W;
    if (c < 0) c = 0;
    if (c > cols) c = cols;

    int l = start + row;
    if (l < 0) l = 0;
    if (l > n_lines) l = n_lines;

    *line = l;
    *col = c;
}

/* Defined further down, next to the rest of the input line's handling. */
static void insert_char(char c);
static void dim(const char *s);

/* The selected text, flattened. Lines join with a newline, and trailing
   spaces go: a terminal pads its lines to the width of the window and
   nobody wants that padding in what they paste. */
static int selection_text(char *out, int cap) {
    if (!sel_active) return 0;
    int l0, c0, l1, c1;
    sel_range(&l0, &c0, &l1, &c1);

    int n = 0;
    for (int l = l0; l <= l1 && l < n_lines; l++) {
        const char *src = line_at(l);
        int len = strlen(src);
        int from = (l == l0) ? c0 : 0;
        int to   = (l == l1) ? c1 : len;
        if (to > len) to = len;
        if (from > len) from = len;

        int stop = to;
        while (stop > from && src[stop - 1] == ' ') stop--;

        for (int i = from; i < stop && n < cap - 1; i++) out[n++] = src[i];
        if (l != l1 && n < cap - 1) out[n++] = '\n';
    }
    out[n] = 0;
    return n;
}

static void copy_selection(void) {
    static char out[COLS * 40];
    int n = selection_text(out, sizeof(out));
    if (n <= 0) {
        /* Nothing selected means the line being typed, which is the other
           thing somebody reaches for copy to get. */
        if (in_len <= 0) { dim("nothing to copy"); return; }
        clip_set(input, in_len);
        dim("copied the input line");
        return;
    }
    clip_set(out, n);
    dim("copied");
}

static void paste_clipboard(void) {
    int n = clip_len();
    if (n <= 0) { dim("clipboard is empty"); return; }

    static char incoming[COLS + 1];
    n = clip_get(incoming, sizeof(incoming));

    /* A newline in the middle would submit half of it, so everything up to
       the first one is taken and the rest is dropped. A terminal input line
       holds one line by definition. */
    for (int i = 0; i < n; i++) {
        if (incoming[i] == '\n' || incoming[i] == '\r') break;
        insert_char(incoming[i]);
    }
    dim("pasted");
}

/* What is on the screen, in words, so the desktop's find can look through
   it. A window is a rectangle of pixels and there is no text in a picture
   of text, so a terminal that says nothing is a terminal nobody can search
   -- which for the most text there is on this desktop would be the wrong
   way round.

   The recent lines rather than the whole scrollback, because find answers
   about what can be seen and what has scrolled away cannot. */
static void publish_text(void) {
    static char buf[4096];
    int n = 0;
    int from = n_lines > 60 ? n_lines - 60 : 0;
    for (int i = from; i < n_lines && n < (int)sizeof(buf) - 2; i++) {
        const char *t = line_at(i);
        for (int k = 0; t[k] && n < (int)sizeof(buf) - 2; k++) buf[n++] = t[k];
        buf[n++] = '\n';
    }
    win_set_text(win, buf, n);
}

static int draw_prompt(void);

static void draw_all(void) {
    fill(&scr, pal.bg);
    publish_text();

    /* The last row belongs to the prompt, so the scrollback gets the rest. */
    int text_rows = rows - 1;
    int end = n_lines - view;
    if (end < 0) end = 0;
    int start = end - text_rows;
    if (start < 0) start = 0;

    int y = PAD;
    for (int i = start; i < end; i++) {
        if (sel_active) {
            int l0, c0, l1, c1;
            sel_range(&l0, &c0, &l1, &c1);
            if (i >= l0 && i <= l1) {
                int len = strlen(line_at(i));
                int from = (i == l0) ? c0 : 0;
                int to   = (i == l1) ? c1 : len;
                if (to > len) to = len;
                if (from > to) from = to;
                if (to > from)
                    rect(&scr, PAD + from * MONO_W, y,
                         (to - from) * MONO_W, MONO_H, mix(pal.bg, pal.accent, 120));
            }
        }
        face_draw(&scr, PAD, y, line_at(i), colour_of(*slot_at(i)),
                  UI_FACE_MONO);
        y += MONO_H;
    }

    /* The prompt sits on the bottom row, always visible. */
    int py = PAD + text_rows * MONO_H;
    rect(&scr, 0, py - 3, scr.w, 1, pal.dim);
    draw_prompt();

    if (view > 0) {
        w_reset();
        w_str(" "); w_num((u32)view); w_str(" lines back, End returns ");
        int tw = strlen(work) * MONO_W;
        rect(&scr, scr.w - tw - PAD, PAD - 2, tw, MONO_H + 2, pal.dim);
        face_draw(&scr, scr.w - tw - PAD, PAD, work, pal.bg, UI_FACE_MONO);
    }
}

/* The bottom row: where the shell is, the line being typed and the cursor.
 * Everything from the top of it to the bottom of the window belongs to it,
 * so a blink of the cursor draws only this and commits only this
 * (win_commit_rect), where it drew and sent the whole window twice a second.
 * Returns the row's top. */
static int hs_find(int before);

static int draw_prompt(void) {
    int py = PAD + (rows - 1) * MONO_H;
    rect(&scr, 0, py, scr.w, scr.h - py, pal.bg);

    /* Looking back: what is looked for, and the line it finds. */
    if (hs_on) {
        char head[COLS + 16];
        int n = 0;
        for (const char *p = "(back) "; *p; p++) head[n++] = *p;
        for (int i = 0; hs_text[i] && n < COLS; i++) head[n++] = hs_text[i];
        head[n++] = ':';
        head[n++] = ' ';
        head[n] = 0;
        face_draw(&scr, PAD, py, head, pal.accent, UI_FACE_MONO);
        int x = PAD + n * MONO_W;
        char shown[COLS + 1];
        int room = cols - n;
        if (room < 1) room = 1;
        const char *found = hs_found >= 0 ? hist[hs_found] : "nothing typed before has that";
        int sn = 0;
        for (int i = 0; found[i] && sn < room && sn < COLS; i++) shown[sn++] = found[i];
        shown[sn] = 0;
        face_draw(&scr, x, py, shown, hs_found >= 0 ? pal.fg : pal.dim, UI_FACE_MONO);
        return py;
    }

    char cwd[128];
    if (getcwd(cwd, sizeof(cwd)) < 0) strcpy(cwd, "/");
    face_draw(&scr, PAD, py, cwd, pal.accent, UI_FACE_MONO);
    int x = PAD + (strlen(cwd) + 1) * MONO_W;
    face_draw(&scr, x, py, "> ", pal.dim, UI_FACE_MONO);
    x += 2 * MONO_W;

    /* The line can be longer than the row it is written on, so the row is a
       window onto it that follows the cursor. Scrolled by half a screen at
       a time rather than one column, because a window that moves on every
       keystroke makes the text slide about under what is being typed. */
    int room = cols - (int)strlen(cwd) - 3;
    if (room < 8) room = 8;
    if (in_off > in_len) in_off = 0;
    if (in_pos < in_off) {
        in_off = in_pos - room / 2;
        if (in_off < 0) in_off = 0;
    }
    if (in_pos > in_off + room - 1) in_off = in_pos - room + 1;

    char shown[COLS + 1];
    int sn = 0;
    for (int i = in_off; input[i] && sn < room; i++) shown[sn++] = input[i];
    shown[sn] = 0;
    face_draw(&scr, x, py, shown, pal.fg, UI_FACE_MONO);

    /* A block cursor sitting on the character it is in front of, so editing
       in the middle of a line is visible rather than guessed at. */
    if (view == 0 && cursor_lit()) {
        int cx = x + (in_pos - in_off) * MONO_W;
        if (in_pos < in_len) {
            rect(&scr, cx, py, MONO_W, MONO_H, pal.cursor);
            char one[2] = { input[in_pos], 0 };
            face_draw(&scr, cx, py, one, pal.bg, UI_FACE_MONO);
        } else {
            rect(&scr, cx, py, 2, MONO_H, pal.cursor);
        }
    }
    return py;
}

/* --- paths -------------------------------------------------------------- */

/* Joins a directory and a name the way the kernel would resolve them, which
   is needed anywhere a path is built up rather than typed. */
static void path_join(const char *dir, const char *name, char *out, int cap) {
    int o = 0;
    if (name[0] == '/') {
        for (int i = 0; name[i] && o < cap - 1; i++) out[o++] = name[i];
        out[o] = 0;
        return;
    }
    for (int i = 0; dir[i] && o < cap - 1; i++) out[o++] = dir[i];
    if (o && out[o - 1] != '/' && o < cap - 1) out[o++] = '/';
    for (int i = 0; name[i] && o < cap - 1; i++) out[o++] = name[i];
    out[o] = 0;
}

static int parse_num(const char *s, int fallback) {
    int v = 0;
    bool any = false;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return fallback;
        v = v * 10 + (s[i] - '0');
        any = true;
    }
    return any ? v : fallback;
}

/* --- commands -----------------------------------------------------------

   Each one takes the split command line. They are listed in a table at the
   bottom, which is what help prints and what tab completion searches, so
   there is exactly one place a command has to be added. */

typedef void (*handler)(int argc, char **argv);

static void run_line(char *cmdline);        /* used by time and by the loop */

/* --- wildcards ----------------------------------------------------------------
 *
 * A word with * or ? outside double quotes stands for the names in its folder
 * that it matches, in order of their bytes: * for any run of characters, ? for
 * any one, case aside as the disk has it. Only the last part of a path may
 * have them (*.txt, /home/a?.csv, or a folder's name, a slash and *.md); a
 * name beginning with a dot is matched only by a word that begins with one; a
 * word that matches nothing is left as it was typed, as other shells leave
 * it. The names are kept in a pool that each line typed starts afresh. */
#define GLOB_POOL 16384
static char glob_pool[GLOB_POOL];
static int  glob_n;

static bool glob_match(const char *p, const char *s) {
    const char *star = 0, *back = 0;
    while (*s) {
        char a = *p, b = *s;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (*p == '*') { star = ++p; back = s; continue; }
        if (*p && (*p == '?' || a == b)) { p++; s++; continue; }
        if (star) { p = star; s = ++back; continue; }
        return false;
    }
    while (*p == '*') p++;
    return !*p;
}

/* The word at argv[at] made the names it matches, sorted: the new count. */
static int glob_expand(char **argv, int at, int max) {
    const char *word = argv[at];
    int cut = -1;
    for (int i = 0; word[i]; i++) if (word[i] == '/') cut = i;
    char dir[VFS_PATH];
    if (cut < 0) { if (getcwd(dir, sizeof(dir)) < 0) strcpy(dir, "/"); }
    else if (cut == 0) strcpy(dir, "/");
    else if (cut < (int)sizeof(dir)) { memcpy(dir, word, cut); dir[cut] = 0; }
    else return at + 1;
    for (int i = 0; dir[i]; i++) if (dir[i] == '*' || dir[i] == '?') return at + 1;
    const char *pat = word + cut + 1;
    int first = at;
    zelr_stat st;
    for (int i = 0; readdir(dir, i, &st) == 1 && at < max; i++) {
        if (!strcmp(st.name, ".") || !strcmp(st.name, "..")) continue;
        if (st.name[0] == '.' && pat[0] != '.') continue;
        if (!glob_match(pat, st.name)) continue;
        int need = cut + 1 + (int)strlen(st.name) + 1;
        if (glob_n + need > GLOB_POOL) break;
        char *w = glob_pool + glob_n;
        for (int k = 0; k <= cut; k++) w[k] = word[k];
        strcpy(w + cut + 1, st.name);
        glob_n += need;
        int k = at;
        while (k > first && strcmp(argv[k - 1], w) > 0) { argv[k] = argv[k - 1]; k--; }
        argv[k] = w;
        at++;
    }
    return at > first ? at : first + 1;
}

/* Words run to a space outside double quotes, and the quotes go: "a b" is
   one word, a b, as other shells have it (a delimiter of one space for cut,
   two words for grep to find together). A word with a wildcard outside the
   quotes becomes the names it matches. */
static int split(char *s, char **argv, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        argv[n++] = s;
        char *out = s;
        int quoted = 0, wild = 0;
        while (*s && (quoted || *s != ' ')) {
            if (*s == '"') { quoted = !quoted; s++; continue; }
            if (!quoted && (*s == '*' || *s == '?')) wild = 1;
            *out++ = *s++;
        }
        int more = *s != 0;
        *out = 0;
        if (more) s++;
        if (wild) n = glob_expand(argv, n - 1, max);
    }
    return n;
}

static void join_from(char **argv, int argc, int from, char *out, int cap) {
    int o = 0;
    for (int i = from; i < argc && o < cap - 1; i++) {
        if (i > from && o < cap - 1) out[o++] = ' ';
        for (const char *p = argv[i]; *p && o < cap - 1; p++) out[o++] = *p;
    }
    out[o] = 0;
}

static void need(const char *usage) { w_reset(); w_str("usage: "); w_str(usage); err(work); }

/* --- looking around ----------------------------------------------------- */

static void cmd_shutdown(int argc, char **argv) {
    (void)argc; (void)argv;
    say("switching off");
    power_off();
    /* Only here when the firmware wanted something this kernel does not do,
       which is worth saying rather than looking like nothing happened. */
    err("this machine will not power off by itself");
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    say("restarting");
    power_reboot();
    err("this machine did not restart");
}

/* With -l, when a file was last written, between its size and its name:
   "2026-10-18 14:05  ", or blanks as wide where the disk keeps no time. */
static void w_when(const char *path) {
    zelr_times t;
    if (file_times(path, &t) != 0 || !t.year) { w_pad(work_n + 18); return; }
    w_num(t.year); w_ch('-');
    w_ch((char)('0' + t.month / 10)); w_ch((char)('0' + t.month % 10)); w_ch('-');
    w_ch((char)('0' + t.day / 10)); w_ch((char)('0' + t.day % 10)); w_ch(' ');
    w_ch((char)('0' + t.hour / 10)); w_ch((char)('0' + t.hour % 10)); w_ch(':');
    w_ch((char)('0' + t.minute / 10)); w_ch((char)('0' + t.minute % 10));
    w_str("  ");
}

static void cmd_ls(int argc, char **argv) {
    int times = argc > 1 && !strcmp(argv[1], "-l");
    const char *where = argc > 1 + times ? argv[1 + times] : ".";
    zelr_stat st;
    int files = 0, dirs = 0;
    u32 bytes = 0;

    /* A path that is a file rather than a directory should show that file,
       not nothing at all. */
    if (stat(where, &st) == 0 && !st.is_dir) {
        w_reset(); w_rnum(st.size, 10); w_str("  ");
        if (times) w_when(where);
        w_str(where); say(work);
        return;
    }

    for (int i = 0; ; i++) {
        if (readdir(where, i, &st) != 1) break;
        w_reset();
        char path[VFS_PATH];
        if (times) path_join(where, st.name, path, sizeof(path));
        if (st.is_dir) {
            w_str("     <dir>  ");
            if (times) w_when(path);
            w_str(st.name);
            w_ch('/');
            print(work, C_ACCENT);
            dirs++;
        } else {
            w_rnum(st.size, 10);
            w_str("  ");
            if (times) w_when(path);
            w_str(st.name);
            say(work);
            files++;
            bytes += st.size;
        }
    }

    if (!files && !dirs) { dim("(empty)"); return; }
    w_reset();
    w_num((u32)files); w_str(" file"); if (files != 1) w_ch('s');
    w_str(" in "); w_num(bytes); w_str(" bytes");
    if (dirs) { w_str(", "); w_num((u32)dirs); w_str(" director"); w_str(dirs == 1 ? "y" : "ies"); }
    dim(work);
}

/* The whole tree under a path, indented. Depth is capped because the
   filesystem allows nesting deeper than this is useful for. */
static void walk(const char *dir, int depth, int *files, int *dirs, u32 *bytes) {
    if (depth > 6) return;
    zelr_stat st;
    for (int i = 0; ; i++) {
        if (readdir(dir, i, &st) != 1) break;

        w_reset();
        for (int d = 0; d < depth; d++) w_str("  ");
        w_str(st.is_dir ? "" : "");
        w_str(st.name);
        if (st.is_dir) {
            w_ch('/');
            print(work, C_ACCENT);
            (*dirs)++;
            char sub[VFS_PATH];
            path_join(dir, st.name, sub, sizeof(sub));
            walk(sub, depth + 1, files, dirs, bytes);
        } else {
            w_pad(28);
            w_rnum(st.size, 8);
            say(work);
            (*files)++;
            *bytes += st.size;
        }
    }
}

static void cmd_tree(int argc, char **argv) {
    const char *where = argc > 1 ? argv[1] : ".";
    zelr_stat st;
    if (stat(where, &st) != 0) { err("no such path"); return; }
    if (!st.is_dir) { err("that is a file"); return; }

    print(where, C_ACCENT);
    int files = 0, dirs = 0;
    u32 bytes = 0;
    walk(where, 1, &files, &dirs, &bytes);

    w_reset();
    w_num((u32)files); w_str(" files, "); w_num((u32)dirs);
    w_str(" directories, "); w_num(bytes); w_str(" bytes");
    dim(work);
}

/* Searches the tree for names containing a string. */
static void hunt(const char *dir, const char *needle, int depth, int *found) {
    if (depth > 6 || *found >= 200) return;
    zelr_stat st;
    for (int i = 0; ; i++) {
        if (readdir(dir, i, &st) != 1) break;

        char full[VFS_PATH];
        path_join(dir, st.name, full, sizeof(full));

        bool hit = false;
        int nl = strlen(needle);
        for (int k = 0; st.name[k] && !hit; k++)
            if (strncmp(st.name + k, needle, nl) == 0) hit = true;

        if (hit) {
            (*found)++;
            print(full, st.is_dir ? C_ACCENT : C_FG);
        }
        if (st.is_dir) hunt(full, needle, depth + 1, found);
    }
}

static void cmd_find(int argc, char **argv) {
    if (argc < 2) { need("find NAME [WHERE]"); return; }
    const char *where = argc > 2 ? argv[2] : "/";
    int found = 0;
    hunt(where, argv[1], 0, &found);
    if (!found) { dim("nothing matched"); return; }
    w_reset(); w_num((u32)found); w_str(" match"); if (found != 1) w_str("es"); dim(work);
}

/* --- reading files ------------------------------------------------------ */

static char io[IO_BUF];

/* Reads a whole file, returning how much came back, or -1. */
static int read_all(const char *path) {
    zelr_stat st;
    if (stat(path, &st) != 0) { err("no such file"); return -1; }
    if (st.is_dir) { err("that is a directory"); return -1; }

    int fd = open(path, O_READ);
    if (fd < 0) { err("cannot open it"); return -1; }

    int total = 0;
    for (;;) {
        int n = fread(fd, io + total, (int)sizeof(io) - 1 - total);
        if (n <= 0) break;
        total += n;
        if (total >= (int)sizeof(io) - 1) break;
    }
    close(fd);
    io[total] = 0;
    return total;
}

/* The text a command works on, into io: the file, or with no file what was
   piped in. */
static int read_input(const char *path) {
    if (path) return read_all(path);
    int n = pipe_n < (int)sizeof(io) - 1 ? pipe_n : (int)sizeof(io) - 1;
    for (int i = 0; i < n; i++) io[i] = pipe_buf[i];
    io[n] = 0;
    return n;
}

/* A file's last newline ends its last line; it does not start another, or
   cat sent through sort would give an empty line first. */
static void print_text(int n) {
    if (n > 0 && io[n - 1] == '\n') io[n - 1] = 0;
    if (n > 0) print(io, C_FG);
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2 && pipe_n >= 0) { print_text(read_input(0)); return; }
    if (argc < 2) { need("cat FILE"); return; }
    for (int i = 1; i < argc; i++) {
        if (argc > 2) { w_reset(); w_str("== "); w_str(argv[i]); w_str(" =="); dim(work); }
        print_text(read_all(argv[i]));
    }
}

/* Prints the first or last few lines. Reading the whole file to do it is
   fine at these sizes and keeps one code path. */
static void head_tail(const char *path, int count, bool from_end) {
    int total = read_input(path);
    if (total <= 0) return;

    int nl = 0;
    for (int i = 0; i < total; i++) if (io[i] == '\n') nl++;
    if (io[total - 1] != '\n') nl++;

    int skip = from_end ? (nl > count ? nl - count : 0) : 0;
    int limit = from_end ? nl : count;

    int line = 0, start = 0;
    for (int i = 0; i <= total; i++) {
        if (i == total || io[i] == '\n') {
            if (line >= skip && line < limit) {
                char save = io[i];
                io[i] = 0;
                say(io + start);
                io[i] = save;
            }
            line++;
            start = i + 1;
            if (line >= limit) break;
        }
    }
}

/* After a |, head and tail read what was piped in, and the one word is the
   number of lines. */
static void cmd_head(int argc, char **argv) {
    if (pipe_n >= 0 && argc <= 2) { head_tail(0, argc > 1 ? parse_num(argv[1], 10) : 10, false); return; }
    if (argc < 2) { need("head FILE [LINES]"); return; }
    head_tail(argv[1], argc > 2 ? parse_num(argv[2], 10) : 10, false);
}

static void cmd_tail(int argc, char **argv) {
    if (pipe_n >= 0 && argc <= 2) { head_tail(0, argc > 1 ? parse_num(argv[1], 10) : 10, true); return; }
    if (argc < 2) { need("tail FILE [LINES]"); return; }
    head_tail(argv[1], argc > 2 ? parse_num(argv[2], 10) : 10, true);
}

static void cmd_wc(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : 0;
    if (!path && pipe_n < 0) { need("wc FILE"); return; }
    int total = read_input(path);
    if (total < 0) return;

    int nl = 0, words = 0;
    bool in_word = false;
    for (int i = 0; i < total; i++) {
        if (io[i] == '\n') nl++;
        bool space = io[i] == ' ' || io[i] == '\n' || io[i] == '\t' || io[i] == '\r';
        if (!space && !in_word) { words++; in_word = true; }
        if (space) in_word = false;
    }
    if (total && io[total - 1] != '\n') nl++;

    w_reset();
    w_rnum((u32)nl, 7); w_str(" lines");
    w_rnum((u32)words, 8); w_str(" words");
    w_rnum((u32)total, 9); w_str(" bytes");
    if (path) { w_str("  "); w_str(path); }
    say(work);
}

/* grep [-i] [-r] TEXT FILE...: lines of a file containing a string, each
   with its number, and with its file's name when there are several; after
   a |, the lines of what was piped in, as they are, so that a wc after it
   counts them. -i finds the string with case aside. -r looks in every file
   below a folder named, sixteen folders deep as du goes, each line with its
   file's path, and passes over a file with a nought byte in its first
   kilobyte (a picture, a program), as its lines are not lines. Finding
   none says so only on the screen: in a file or a pipe it is nothing. */
static int grep_fold, grep_hits;

static int grep_same(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (grep_fold) {
            if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
            if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        }
        if (x != y) return 0;
    }
    return 1;
}

/* The lines of what is in io that hold the needle. */
static void grep_lines(int total, const char *needle, const char *name, int numbered) {
    int nl = strlen(needle), line = 1, start = 0;
    for (int i = 0; i <= total; i++) {
        if (i != total && io[i] != '\n') continue;
        char save = io[i];
        io[i] = 0;
        for (int k = start; k + nl <= i; k++) {
            if (!grep_same(io + k, needle, nl)) continue;
            w_reset();
            if (name) { w_str(name); w_ch(':'); }
            if (numbered) { w_num((u32)line); w_str(": "); }
            w_str(io + start);
            say(work);
            grep_hits++;
            break;
        }
        io[i] = save;
        line++;
        start = i + 1;
    }
}

static void grep_walk(const char *dir, const char *needle, int depth) {
    zelr_stat st;
    for (int i = 0; readdir(dir, i, &st) == 1; i++) {
        if (!strcmp(st.name, ".") || !strcmp(st.name, "..")) continue;
        char sub[VFS_PATH];
        path_join(dir, st.name, sub, sizeof(sub));
        if (st.is_dir) {
            if (depth < 16) grep_walk(sub, needle, depth + 1);
            continue;
        }
        int total = read_all(sub);
        if (total < 0) continue;
        int binary = 0;
        for (int k = 0; k < total && k < 1024 && !binary; k++) binary = !io[k];
        if (!binary) grep_lines(total, needle, sub, 1);
    }
}

static void cmd_grep(int argc, char **argv) {
    int a = 1, walk = 0;
    grep_fold = 0;
    grep_hits = 0;
    while (a < argc && argv[a][0] == '-' && argv[a][1]) {
        for (const char *o = argv[a] + 1; *o; o++) {
            if (*o == 'i') grep_fold = 1;
            else if (*o == 'r') walk = 1;
            else { need("grep [-i] [-r] TEXT FILE..."); return; }
        }
        a++;
    }
    int files = argc - a - 1;
    bool from_pipe = files == 0 && pipe_n >= 0;
    if (files < 1 && !from_pipe) { need("grep [-i] [-r] TEXT FILE..."); return; }
    const char *needle = argv[a];
    if (from_pipe) {
        int total = read_input(0);
        if (total > 0) grep_lines(total, needle, 0, 0);
    }
    for (int f = a + 1; f < argc; f++) {
        zelr_stat st;
        if (walk && stat(argv[f], &st) == 0 && st.is_dir) { grep_walk(argv[f], needle, 0); continue; }
        int total = read_all(argv[f]);
        if (total < 0) continue;
        grep_lines(total, needle, files > 1 || walk ? argv[f] : 0, 1);
    }
    if (!grep_hits && !capturing) dim("no matches");
}

/* uniq [-c] [FILE]: lines alike one after another said once, with -c how
   many there were in front; of a file or of what was piped in. Only lines
   that follow each other: sort first to count them all. */
static void uniq_say(const char *line, int run, int counts) {
    if (!counts) { say(line); return; }
    w_reset();
    w_rnum((u32)run, 7);
    w_ch(' ');
    w_str(line);
    say(work);
}

static void cmd_uniq(int argc, char **argv) {
    int counts = argc > 1 && !strcmp(argv[1], "-c");
    const char *path = argc > 1 + counts ? argv[1 + counts] : 0;
    if (!path && pipe_n < 0) { need("uniq [-c] FILE"); return; }
    int total = read_input(path);
    if (total <= 0) return;
    char *prev = 0;
    int run = 0, start = 0;
    for (int i = 0; i <= total; i++) {
        if (i < total && io[i] != '\n') continue;
        if (i == total && start == total) break;     /* nothing after the last newline */
        io[i] = 0;
        char *line = io + start;
        if (prev && !strcmp(prev, line)) run++;
        else {
            if (prev) uniq_say(prev, run, counts);
            prev = line;
            run = 1;
        }
        start = i + 1;
    }
    if (prev) uniq_say(prev, run, counts);
}

/* diff A B: the fewest lines taken out of A and put in from B that make one
   the other, each said with its own file's line number ("- 3: ..." is A's
   third line gone, "+ 3: ..." B's third come). Worked out from the longest
   run of lines the two share, kept in a table of how much of it is left
   from each pair of places; the lines the two begin and end with alike are
   passed over first, so only what lies between is tabled, a thousand lines
   each at most. Saying nothing on the screen would look like a fault, so
   two files alike are said to be the same, there and only there. */
#define DIFF_LINES 4000
#define DIFF_MID   1000
static char diff_a[IO_BUF + 1], diff_b[IO_BUF + 1];
static char *diff_la[DIFF_LINES], *diff_lb[DIFF_LINES];
static u16 diff_lcs[(DIFF_MID + 1) * (DIFF_MID + 1)];

static int diff_lines(char *buf, int n, char **out) {
    int k = 0, start = 0;
    for (int i = 0; i <= n && k < DIFF_LINES; i++) {
        if (i < n && buf[i] != '\n') continue;
        if (i == n && start == n) break;
        buf[i] = 0;
        out[k++] = buf + start;
        start = i + 1;
    }
    return k;
}

static void diff_say(char sign, int num, const char *text) {
    w_reset();
    w_ch(sign);
    w_ch(' ');
    w_num((u32)num);
    w_str(": ");
    w_str(text);
    say(work);
}

static void cmd_diff(int argc, char **argv) {
    if (argc < 3) { need("diff FILE FILE"); return; }
    int na = read_all(argv[1]);
    if (na < 0) return;
    for (int i = 0; i < na; i++) diff_a[i] = io[i];
    diff_a[na] = 0;
    int nb = read_all(argv[2]);
    if (nb < 0) return;
    for (int i = 0; i < nb; i++) diff_b[i] = io[i];
    diff_b[nb] = 0;
    char **la = diff_la, **lb = diff_lb;
    int n = diff_lines(diff_a, na, la), m = diff_lines(diff_b, nb, lb);
    if (n == DIFF_LINES || m == DIFF_LINES) { err("too long to compare here: 4000 lines each at most"); return; }

    int pre = 0, suf = 0;
    while (pre < n && pre < m && !strcmp(la[pre], lb[pre])) pre++;
    while (suf < n - pre && suf < m - pre && !strcmp(la[n - 1 - suf], lb[m - 1 - suf])) suf++;
    int an = n - pre - suf, bn = m - pre - suf;
    if (an > DIFF_MID || bn > DIFF_MID) {
        err("too different to compare here: a thousand lines each between what they share");
        return;
    }
#define LCS(i, j) diff_lcs[(i) * (bn + 1) + (j)]
    for (int i = an; i >= 0; i--)
        for (int j = bn; j >= 0; j--) {
            if (i == an || j == bn) LCS(i, j) = 0;
            else if (!strcmp(la[pre + i], lb[pre + j])) LCS(i, j) = (u16)(LCS(i + 1, j + 1) + 1);
            else LCS(i, j) = LCS(i + 1, j) >= LCS(i, j + 1) ? LCS(i + 1, j) : LCS(i, j + 1);
        }
    int i = 0, j = 0, said = 0;
    while (i < an || j < bn) {
        if (i < an && j < bn && !strcmp(la[pre + i], lb[pre + j])) { i++; j++; continue; }
        if (j >= bn || (i < an && LCS(i + 1, j) >= LCS(i, j + 1))) { diff_say('-', pre + i + 1, la[pre + i]); i++; }
        else { diff_say('+', pre + j + 1, lb[pre + j]); j++; }
        said++;
    }
#undef LCS
    if (!said && !capturing) dim("the same");
}

/* sha256sum FILE...: each file's SHA-256 in hex, then its name, as other
   systems print it, so that a sum taken elsewhere can be compared by eye
   or by a program; with no file, what was piped in, named "-". A file is
   read a piece at a time, so its size does not matter. */
static void sum_say(const u8 d[32], const char *name) {
    const char *digits = "0123456789abcdef";
    w_reset();
    for (int k = 0; k < 32; k++) { w_ch(digits[d[k] >> 4]); w_ch(digits[d[k] & 15]); }
    w_str("  ");
    w_str(name);
    say(work);
}

static void not_a_file(const char *path, const char *why) {
    w_reset();
    w_str(path);
    w_str(why);
    err(work);
}

static void cmd_sha256sum(int argc, char **argv) {
    u8 d[32];
    jc_sha256_t s;
    if (argc < 2 && pipe_n >= 0) {
        jc_sha256_start(&s);
        jc_sha256_add(&s, (const u8 *)pipe_buf, (u32)pipe_n);
        jc_sha256_end(&s, d);
        sum_say(d, "-");
        return;
    }
    if (argc < 2) { need("sha256sum FILE..."); return; }
    for (int i = 1; i < argc; i++) {
        zelr_stat st;
        if (stat(argv[i], &st) != 0) { not_a_file(argv[i], ": no such file"); continue; }
        if (st.is_dir) { not_a_file(argv[i], ": a folder"); continue; }
        int fd = open(argv[i], O_READ);
        if (fd < 0) { not_a_file(argv[i], ": cannot open it"); continue; }
        /* Pieces one short of the buffer, as read_all reads: each ends
           mid-block, so the way a short read from a stick would leave the
           digest is the way every file goes. */
        jc_sha256_start(&s);
        for (;;) {
            int n = fread(fd, io, (int)sizeof(io) - 1);
            if (n <= 0) break;
            jc_sha256_add(&s, (const u8 *)io, (u32)n);
        }
        close(fd);
        jc_sha256_end(&s, d);
        sum_say(d, argv[i]);
    }
}

/* cmp FILE FILE: where two files first differ, by byte and line counted
   from 1 as other systems count them, or that one ends first, or that they
   are the same. Read a piece at a time, so their size does not matter. */
static void cmd_cmp(int argc, char **argv) {
    if (argc < 3) { need("cmp FILE FILE"); return; }
    for (int i = 1; i <= 2; i++) {
        zelr_stat st;
        if (stat(argv[i], &st) != 0) { not_a_file(argv[i], ": no such file"); return; }
        if (st.is_dir) { not_a_file(argv[i], ": a folder"); return; }
    }
    int fa = open(argv[1], O_READ);
    int fb = fa < 0 ? -1 : open(argv[2], O_READ);
    if (fa < 0 || fb < 0) { if (fa >= 0) close(fa); err("cannot open them"); return; }
    u32 at = 0, line = 1;
    for (;;) {
        /* As much of b as came of a, so the two pieces line up. */
        int na = fread(fa, diff_a, IO_BUF);
        if (na < 0) na = 0;
        int nb = 0;
        while (nb < na) {
            int n = fread(fb, diff_b + nb, na - nb);
            if (n <= 0) break;
            nb += n;
        }
        w_reset();
        if (na == 0) {
            /* a has ended: b too, or b is longer. */
            if (fread(fb, diff_b, 1) > 0) {
                w_str(argv[1]); w_str(" ends first, after byte "); w_num(at);
            } else {
                w_str(argv[1]); w_ch(' '); w_str(argv[2]); w_str(" are the same, ");
                w_num(at); w_str(at == 1 ? " byte" : " bytes");
            }
            say(work);
            break;
        }
        int k = 0;
        while (k < nb && diff_a[k] == diff_b[k]) { if (diff_a[k] == '\n') line++; k++; }
        if (k < nb) {
            w_str(argv[1]); w_ch(' '); w_str(argv[2]); w_str(" differ: byte "); w_num(at + (u32)k + 1);
            w_str(", line "); w_num(line);
            say(work);
            break;
        }
        if (nb < na) {
            w_str(argv[2]); w_str(" ends first, after byte "); w_num(at + (u32)nb);
            say(work);
            break;
        }
        at += (u32)na;
    }
    close(fa);
    close(fb);
}

/* cut -f LIST [-d C] [FILE], cut -c LIST [FILE]: parts of each line, of a
   file or of what was piped in. LIST is numbers from 1 and ranges (2-4,
   3-, -2) apart by commas; what is chosen comes in the line's order, once.
   Fields are apart by a tab unless -d says otherwise, and a line without
   one is given whole, as other systems give it. */
#define CUT_RANGES 16
static int cut_lo[CUT_RANGES], cut_hi[CUT_RANGES], cut_n;
static char cut_row[4096];

static bool cut_list(const char *s) {
    cut_n = 0;
    while (*s) {
        if (cut_n == CUT_RANGES) return false;
        int lo = 0, hi = 0, dash = 0;
        while (*s >= '0' && *s <= '9') lo = lo * 10 + (*s++ - '0');
        if (*s == '-') {
            dash = 1;
            s++;
            while (*s >= '0' && *s <= '9') hi = hi * 10 + (*s++ - '0');
        }
        if (*s && *s != ',') return false;
        if (*s == ',') s++;
        if (!dash) hi = lo;
        if (dash && !lo) lo = 1;
        if (!lo || (hi && hi < lo)) return false;
        cut_lo[cut_n] = lo;
        cut_hi[cut_n] = hi;               /* 0: to the end */
        cut_n++;
    }
    return cut_n > 0;
}

static bool cut_wanted(int k) {
    for (int i = 0; i < cut_n; i++)
        if (k >= cut_lo[i] && (!cut_hi[i] || k <= cut_hi[i])) return true;
    return false;
}

static void cut_line(const char *line, int len, bool chars, char delim) {
    int o = 0, cap = (int)sizeof(cut_row) - 1;
    if (chars) {
        for (int i = 0; i < len && o < cap; i++) if (cut_wanted(i + 1)) cut_row[o++] = line[i];
    } else {
        bool any = false;
        for (int i = 0; i < len; i++) if (line[i] == delim) { any = true; break; }
        if (!any) {
            for (int i = 0; i < len && o < cap; i++) cut_row[o++] = line[i];
        } else {
            int field = 1, first = 1;
            for (int i = 0; i <= len; i++) {
                int start = i;
                while (i < len && line[i] != delim) i++;
                if (cut_wanted(field)) {
                    if (!first && o < cap) cut_row[o++] = delim;
                    for (int k = start; k < i && o < cap; k++) cut_row[o++] = line[k];
                    first = 0;
                }
                field++;
            }
        }
    }
    cut_row[o] = 0;
    say(cut_row);
}

static void cmd_cut(int argc, char **argv) {
    const char *list = 0, *path = 0;
    char delim = '\t';
    bool chars = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && (a[1] == 'f' || a[1] == 'c' || a[1] == 'd') && a[2] == 0 && i + 1 < argc) {
            if (a[1] == 'd') delim = argv[++i][0];
            else { chars = a[1] == 'c'; list = argv[++i]; }
        } else if (a[0] == '-' && (a[1] == 'f' || a[1] == 'c')) {
            chars = a[1] == 'c';
            list = a + 2;
        } else if (a[0] == '-' && a[1] == 'd') {
            delim = a[2];
        } else {
            path = a;
        }
    }
    if (!list || !delim || (!path && pipe_n < 0)) { need("cut -f LIST [-d C] FILE, or cut -c LIST FILE"); return; }
    if (!cut_list(list)) { err("a list is numbers and ranges apart by commas: 1,3 or 2-4 or 3-"); return; }
    int total = read_input(path);
    if (total <= 0) return;
    for (int i = 0, start = 0; i <= total; i++) {
        if (i < total && io[i] != '\n') continue;
        if (i == total && start == total) break;     /* nothing after the last newline */
        cut_line(io + start, i - start, chars, delim);
        start = i + 1;
    }
}

/* tr SET1 SET2 [FILE], tr -d SET [FILE]: each byte of SET1 made the one in
   the same place in SET2 (the last of SET2 again when it is shorter), or
   with -d taken out. A set may hold ranges (a-z) and \n, \t and \\. */
static int tr_set(const char *s, u8 *out) {
    int n = 0;
    while (*s && n < 256) {
        int c = (u8)*s++;
        if (c == '\\' && *s) {
            char e = *s++;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : (u8)e;
        }
        if (*s == '-' && s[1]) {
            int to = (u8)s[1];
            s += 2;
            if (to == '\\' && *s) {
                char e = *s++;
                to = e == 'n' ? '\n' : e == 't' ? '\t' : (u8)e;
            }
            for (int k = c; k <= to && n < 256; k++) out[n++] = (u8)k;
            continue;
        }
        out[n++] = (u8)c;
    }
    return n;
}

static void cmd_tr(int argc, char **argv) {
    bool del = argc > 1 && !strcmp(argv[1], "-d");
    int sets = del ? 1 : 2, first = del ? 2 : 1;
    const char *path = argc > first + sets ? argv[first + sets] : 0;
    if (argc < first + sets || (!path && pipe_n < 0)) { need("tr SET1 SET2 FILE, or tr -d SET FILE"); return; }
    static u8 a[256], b[256];
    int na = tr_set(argv[first], a), nb = del ? 0 : tr_set(argv[first + 1], b);
    if (!na || (!del && !nb)) { err("a set cannot be empty"); return; }
    int map[256];
    for (int k = 0; k < 256; k++) map[k] = k;
    for (int k = 0; k < na; k++) map[a[k]] = del ? -1 : b[k < nb ? k : nb - 1];
    int total = read_input(path);
    if (total <= 0) return;
    int o = 0;
    for (int i = 0; i < total; i++) {
        int m = map[(u8)io[i]];
        if (m >= 0) io[o++] = (char)m;
    }
    io[o] = 0;
    print_text(o);
}

/* unzip [-l] ARCHIVE [FOLDER]: a zip, tar, .tar.gz, .tgz or .gz taken out
   into a folder (made when it is not there), or with no folder into one
   beside it named after it, as Files takes it out; -l lists what is in it,
   each entry's size and name. An entry whose name would write outside the
   folder is refused and counted. */
static void cmd_unzip(int argc, char **argv) {
    int list = argc > 1 && !strcmp(argv[1], "-l");
    const char *path = argc > 1 + list ? argv[1 + list] : 0;
    const char *into = argc > 2 + list ? argv[2 + list] : 0;
    if (!path) { need("unzip [-l] ARCHIVE [FOLDER]"); return; }
    u32 len = 0;
    u8 *data = arcio_read_whole(path, &len);
    if (!data) { err("it could not be read (64 MB at most)"); return; }
    zip_t z;
    int r = zip_open(&z, data, len);
    if (r != ZIP_OK) {
        w_reset(); w_str("not opened: "); w_str(zip_why(r)); err(work);
        free(data);
        return;
    }
    if (list) {
        u32 at = 0;
        zip_entry e;
        while (zip_next(&z, &at, &e) == 1) {
            w_reset();
            w_rnum(e.is_dir ? 0 : e.usize, 9);
            w_str("  ");
            for (int i = 0; i < e.name_len && work_n < COLS; i++) w_ch(e.name[i]);
            say(work);
        }
    } else {
        char dest[ARCIO_PATH];
        if (into) { strncpy(dest, into, sizeof(dest) - 1); dest[sizeof(dest) - 1] = 0; }
        else arcio_dest_for(path, dest);
        w_reset();
        if (z.kind == ZIP_KIND_GZ && !into) {
            /* A file gzipped comes out as that file beside it. */
            u32 at = 0;
            zip_entry one;
            int got = zip_next(&z, &at, &one) == 1 ? arcio_take_out(&z, &one, dest) : ZIP_BAD;
            if (got < 0) { w_str("not extracted: "); w_str(zip_why(got)); err(work); }
            else { w_str("extracted "); w_str(arcio_leaf(dest)); say(work); }
        } else {
            arcio_make_parents(dest);
            mkdir(dest);
            zelr_stat st;
            if (stat(dest, &st) != 0 || !st.is_dir) {
                err("could not make the folder to extract into");
            } else {
                arcio_out out;
                arcio_extract_into(&z, dest, &out);
                w_str("extracted "); w_num((u32)out.files);
                w_str(out.files == 1 ? " file into " : " files into "); w_str(arcio_leaf(dest));
                if (out.refused) {
                    w_str("; not "); w_num((u32)out.refused); w_str(": ");
                    w_str(out.why ? zip_why(out.why) : "a name that would write outside the folder");
                }
                say(work);
            }
        }
    }
    zip_close(&z);
    free(data);
}

/* zip ARCHIVE PATH...: files and folders, everything below them, into a new
   zip, each under its own name; a file is deflated when that makes it
   smaller. 1024 files or 48 MB at most, as Files makes one. */
static void cmd_zip(int argc, char **argv) {
    if (argc < 3) { need("zip ARCHIVE.zip FILE-OR-FOLDER..."); return; }
    for (int i = 2; i < argc; i++) {
        zelr_stat st;
        if (stat(argv[i], &st) != 0) { not_a_file(argv[i], ": no such file"); return; }
    }
    arcio_make_t mk;
    u8 *buf = 0;
    int n = arcio_make((const char *const *)(argv + 2), argc - 2, &buf, &mk);
    if (n < 0) {
        err(mk.over ? "too much for one zip made here: 1024 files or 48 MB"
            : mk.unread ? "no zip made: a file in it could not be read" : "no zip made: not enough memory for it");
        return;
    }
    int wrote = arcio_write_whole(argv[1], buf, n);
    free(buf);
    if (wrote < 0) { not_a_file(argv[1], ": could not be written"); return; }
    w_reset();
    w_str("made "); w_str(arcio_leaf(argv[1])); w_str(": ");
    w_num((u32)mk.files); w_str(mk.files == 1 ? " file, " : " files, ");
    w_num((u32)((n + 1023) / 1024)); w_str(" KB from "); w_num((mk.bytes + 1023) / 1024); w_str(" KB");
    say(work);
}

/* --- small tools ---------------------------------------------------------------
 *
 * seq, nl, tac, rev, basename, dirname and touch, which a script reaches for
 * more than a person does. nl, tac and rev read a file or what was piped in. */

/* seq LAST, seq FIRST LAST, seq FIRST STEP LAST: whole numbers, one a line,
   from FIRST (1) by STEP (1, or -1 when LAST is below FIRST), as far as LAST;
   ten thousand at most. */
static bool whole_number(const char *s, int *out) {
    int neg = *s == '-', v = 0, any = 0;
    if (neg || *s == '+') s++;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000) return false;
        v = v * 10 + (*s - '0');
        any = 1;
    }
    *out = neg ? -v : v;
    return any;
}

static void cmd_seq(int argc, char **argv) {
    int first = 1, step = 0, last;
    bool ok = argc >= 2 && argc <= 4 && whole_number(argv[argc - 1], &last);
    if (ok && argc >= 3) ok = whole_number(argv[1], &first);
    if (ok && argc == 4) ok = whole_number(argv[2], &step) && step != 0;
    if (!ok) { need("seq [FIRST [STEP]] LAST, whole numbers"); return; }
    if (!step) step = last < first ? -1 : 1;
    int n = 0;
    for (int v = first; step > 0 ? v <= last : v >= last; v += step) {
        if (++n > 10000) { err("ten thousand numbers at most"); return; }
        w_reset();
        if (v < 0) { w_ch('-'); w_num((u32)-v); } else w_num((u32)v);
        say(work);
    }
}

/* The lines of what read_input gave: their starts, ended at their newlines,
   none after the last newline. */
static char *small_line[8192];

static int small_lines(int total) {
    int n = 0;
    for (int i = 0, start = 0; i <= total && n < 8192; i++) {
        if (i != total && io[i] != '\n') continue;
        if (i == total && start == total) break;
        io[i] = 0;
        small_line[n++] = io + start;
        start = i + 1;
    }
    return n;
}

/* nl [FILE]: each line with its number before it, right in six places, and
   a tab, as other systems number with cat -n. */
static void cmd_nl(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : 0;
    if (!path && pipe_n < 0) { need("nl FILE"); return; }
    int total = read_input(path);
    if (total <= 0) return;
    int n = small_lines(total);
    for (int i = 0; i < n; i++) {
        w_reset();
        w_rnum((u32)(i + 1), 6);
        w_ch('\t');
        w_str(small_line[i]);
        say(work);
    }
}

/* tac [FILE]: the lines last first. */
static void cmd_tac(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : 0;
    if (!path && pipe_n < 0) { need("tac FILE"); return; }
    int total = read_input(path);
    if (total <= 0) return;
    for (int i = small_lines(total) - 1; i >= 0; i--) say(small_line[i]);
}

/* rev [FILE]: each line's characters the other way round. */
static void cmd_rev(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : 0;
    if (!path && pipe_n < 0) { need("rev FILE"); return; }
    int total = read_input(path);
    if (total <= 0) return;
    int n = small_lines(total);
    for (int i = 0; i < n; i++) {
        char *s = small_line[i];
        int len = strlen(s);
        for (int a = 0, b = len - 1; a < b; a++, b--) { char t = s[a]; s[a] = s[b]; s[b] = t; }
        say(s);
    }
}

/* basename PATH [ENDING]: the last part of a path, an ending such as .txt
   taken off it when given (and not all of it); dirname PATH: all but the
   last part, "." when there is none and "/" for one at the top. Neither
   looks at the disk. */
static void cmd_basename(int argc, char **argv) {
    if (argc < 2 || argc > 3) { need("basename PATH [ENDING]"); return; }
    char part[VFS_PATH];
    strncpy(part, argv[1], sizeof(part) - 1);
    part[sizeof(part) - 1] = 0;
    int n = strlen(part);
    while (n > 1 && part[n - 1] == '/') part[--n] = 0;
    const char *leaf = part;
    for (int i = 0; i < n - 1; i++) if (part[i] == '/') leaf = part + i + 1;
    char out[VFS_PATH];
    strcpy(out, leaf);
    if (argc == 3) {
        int m = strlen(out), e = strlen(argv[2]);
        if (e < m && !strcmp(out + m - e, argv[2])) out[m - e] = 0;
    }
    say(out);
}

static void cmd_dirname(int argc, char **argv) {
    if (argc != 2) { need("dirname PATH"); return; }
    char part[VFS_PATH];
    strncpy(part, argv[1], sizeof(part) - 1);
    part[sizeof(part) - 1] = 0;
    int n = strlen(part);
    while (n > 1 && part[n - 1] == '/') part[--n] = 0;
    int cut = -1;
    for (int i = 0; i < n; i++) if (part[i] == '/') cut = i;
    if (cut < 0) { say("."); return; }
    while (cut > 0 && part[cut - 1] == '/') cut--;
    if (cut == 0) { say("/"); return; }
    part[cut] = 0;
    say(part);
}

/* touch FILE...: a file made, empty, when it is not there; one that is has
   its first byte written again as it was, so the disk says it was written
   now. An empty file that is there is made again, as empty. */
static void cmd_touch(int argc, char **argv) {
    if (argc < 2) { need("touch FILE..."); return; }
    for (int i = 1; i < argc; i++) {
        zelr_stat st;
        if (stat(argv[i], &st) == 0 && st.is_dir) { not_a_file(argv[i], ": a folder"); continue; }
        bool there = stat(argv[i], &st) == 0 && st.size > 0;
        if (!there) {
            int fd = open(argv[i], O_WRITE | O_CREATE | O_TRUNC);
            if (fd < 0) { not_a_file(argv[i], ": cannot be written"); continue; }
            close(fd);
            continue;
        }
        char first;
        int rd = open(argv[i], O_READ);
        bool got = rd >= 0 && fread(rd, &first, 1) == 1;
        if (rd >= 0) close(rd);
        int fd = got ? open(argv[i], O_WRITE) : -1;
        bool again = fd >= 0 && fwrite(fd, &first, 1) == 1;
        if (fd >= 0) close(fd);
        if (!again) not_a_file(argv[i], ": could not be written again");
    }
}

/* sort [FILE]: the lines in byte order, of a file or of what was piped in. */
static void cmd_sort(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : 0;
    if (!path && pipe_n < 0) { need("sort FILE"); return; }
    int total = read_input(path);
    if (total <= 0) return;
    static char *line[8192];
    int n = 0;
    for (int i = 0, start = 0; i <= total && n < 8192; i++) {
        if (i != total && io[i] != '\n') continue;
        if (i == total && start == total) break;     /* nothing after the last newline */
        io[i] = 0;
        line[n++] = io + start;
        start = i + 1;
    }
    /* Shell's sort on the pointers: no memory to ask for, and a few thousand
       lines take nothing. */
    for (int gap = n / 2; gap > 0; gap /= 2)
        for (int i = gap; i < n; i++)
            for (int j = i; j >= gap && strcmp(line[j - gap], line[j]) > 0; j -= gap) {
                char *t = line[j];
                line[j] = line[j - gap];
                line[j - gap] = t;
            }
    for (int i = 0; i < n; i++) say(line[i]);
}

/* What a file actually contains, which matters when it is not text. */
static void cmd_hex(int argc, char **argv) {
    if (argc < 2) { need("hex FILE [BYTES]"); return; }
    int total = read_all(argv[1]);
    if (total < 0) return;

    int limit = argc > 2 ? parse_num(argv[2], 256) : 256;
    if (limit > total) limit = total;

    const char *digits = "0123456789abcdef";
    for (int off = 0; off < limit; off += 16) {
        w_reset();
        for (int s = 12; s >= 0; s -= 4) w_ch(digits[(off >> s) & 15]);
        w_str("  ");
        for (int i = 0; i < 16; i++) {
            if (off + i < limit) {
                u8 b = (u8)io[off + i];
                w_ch(digits[b >> 4]);
                w_ch(digits[b & 15]);
            } else {
                w_str("  ");
            }
            w_ch(' ');
            if (i == 7) w_ch(' ');
        }
        w_ch('|');
        for (int i = 0; i < 16 && off + i < limit; i++) {
            char c = io[off + i];
            w_ch(c >= 32 && c <= 126 ? c : '.');
        }
        w_ch('|');
        say(work);
    }
    if (limit < total) {
        w_reset(); w_num((u32)(total - limit)); w_str(" more bytes"); dim(work);
    }
}

/* --- changing files ----------------------------------------------------- */

static void write_or_append(int argc, char **argv, bool append) {
    if (argc < 3) { need(append ? "append FILE TEXT" : "write FILE TEXT"); return; }

    char body[COLS + 2];
    join_from(argv, argc, 2, body, COLS);
    int len = strlen(body);
    body[len++] = '\n';

    int flags = append ? (O_WRITE | O_CREATE | O_APPEND)
                       : (O_WRITE | O_CREATE | O_TRUNC);
    int fd = open(argv[1], flags);
    if (fd < 0) { err("cannot open it"); return; }
    int n = fwrite(fd, body, len);
    close(fd);

    if (n != len) { err("write failed"); return; }
    w_reset(); w_num((u32)len); w_str(" bytes to "); w_str(argv[1]); dim(work);
}

static void cmd_write(int argc, char **argv)  { write_or_append(argc, argv, false); }
static void cmd_append(int argc, char **argv) { write_or_append(argc, argv, true); }

/* A folder and everything in it, deleted, eight deep at most; or a file.
   The first entry each time, since each one deleted is gone from the list,
   and one that will not go stops it rather than going round for ever. */
static bool remove_tree(const char *path, int depth) {
    zelr_stat st;
    if (stat(path, &st) != 0) return false;
    if (!st.is_dir) return unlink(path) == 0;
    if (depth > 8) return false;
    zelr_stat e;
    while (readdir(path, 0, &e) == 1) {
        char child[VFS_PATH];
        path_join(path, e.name, child, sizeof child);
        if (!remove_tree(child, depth + 1)) return false;
    }
    return rmdir(path) == 0;
}

/* rm -r takes a folder and what is in it, as rmdir will not. */
static void cmd_rm(int argc, char **argv) {
    int first = 1;
    bool tree = argc > 1 && !strcmp(argv[1], "-r");
    if (tree) first = 2;
    if (argc <= first) { need("rm [-r] PATH..."); return; }
    for (int i = first; i < argc; i++) {
        bool gone = tree ? remove_tree(argv[i], 0) : unlink(argv[i]) == 0;
        if (gone) { w_reset(); w_str("removed "); w_str(argv[i]); dim(work); }
        else { w_reset(); w_str("rm: "); w_str(argv[i]); err(work); }
    }
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { need("mkdir NAME"); return; }
    if (mkdir(argv[1]) != 0) err("mkdir failed"); else dim("ok");
}

static void cmd_rmdir(int argc, char **argv) {
    if (argc < 2) { need("rmdir NAME"); return; }
    if (rmdir(argv[1]) != 0) err("not empty, or not a directory"); else dim("ok");
}

/* Copy, and move built out of it.
 *
 * A buffer's worth at a time, however big the file is. This used to read
 * the whole file into the sixteen kilobytes cat uses and write that back,
 * so anything larger arrived cut off at 16 KiB with nothing said -- and mv
 * then deleted the original, which was the only whole copy there was. What
 * reached the target is now checked against the size of the source before
 * the copy is called done, and mv deletes nothing until it is. */
static bool copy_file(const char *src, const char *dst) {
    zelr_stat from;
    if (stat(src, &from) != 0) { err("no such file"); return false; }
    if (from.is_dir) { err("that is a directory"); return false; }

    /* A directory as the target means the same name inside it. */
    char target[VFS_PATH];
    zelr_stat st;
    if (stat(dst, &st) == 0 && st.is_dir) {
        const char *base = src;
        for (const char *p = src; *p; p++) if (*p == '/') base = p + 1;
        path_join(dst, base, target, sizeof(target));
    } else {
        strncpy(target, dst, sizeof(target) - 1);
        target[sizeof(target) - 1] = 0;
    }

    /* Opening the target empties it, so a file copied onto itself would be
       emptied before a byte of it was read. */
    if (strcmp(src, target) == 0) { err("that is the same file"); return false; }

    int in = open(src, O_READ);
    if (in < 0) { err("cannot open it"); return false; }
    int out = open(target, O_WRITE | O_CREATE | O_TRUNC);
    if (out < 0) { close(in); err("cannot write it"); return false; }

    u32 total = 0;
    bool broke = false;
    for (;;) {
        int n = fread(in, io, (int)sizeof(io));
        if (n < 0) { broke = true; break; }
        if (n == 0) break;
        if (fwrite(out, io, n) != n) { broke = true; break; }
        total += (u32)n;
    }
    close(in);
    close(out);

    zelr_stat got;
    if (broke || stat(target, &got) != 0 || got.size != from.size || total != from.size) {
        err("the copy did not arrive whole");
        return false;
    }
    w_reset(); w_num(total); w_str(" bytes to "); w_str(target); dim(work);
    return true;
}

/* A folder and everything in it: its folders made and its files copied,
   eight deep at most, stopping at the first that does not arrive. */
static bool copy_tree(const char *src, const char *dst, int depth) {
    zelr_stat st;
    if (stat(src, &st) != 0) { err("no such path"); return false; }
    if (!st.is_dir) return copy_file(src, dst);
    if (depth > 8) { err("too deep"); return false; }
    zelr_stat there;
    if (mkdir(dst) != 0 && (stat(dst, &there) != 0 || !there.is_dir)) { err("cannot make the folder"); return false; }
    zelr_stat e;
    for (int i = 0; readdir(src, i, &e) == 1; i++) {
        char a[VFS_PATH], b[VFS_PATH];
        path_join(src, e.name, a, sizeof a);
        path_join(dst, e.name, b, sizeof b);
        if (!copy_tree(a, b, depth + 1)) return false;
    }
    return true;
}

/* cp -r copies a folder, into DST when DST is a folder already, as DST when
   it is not there; never into itself, which would copy the copy. */
static void cmd_cp(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "-r")) {
        char target[VFS_PATH];
        zelr_stat st;
        if (stat(argv[3], &st) == 0 && st.is_dir) {
            const char *base = argv[2];
            for (const char *p = argv[2]; *p; p++) if (*p == '/') base = p + 1;
            path_join(argv[3], base, target, sizeof target);
        } else {
            strncpy(target, argv[3], sizeof target - 1);
            target[sizeof target - 1] = 0;
        }
        int n = strlen(argv[2]);
        if (!strncmp(target, argv[2], n) && (target[n] == 0 || target[n] == '/')) { err("not into itself"); return; }
        if (copy_tree(argv[2], target, 0)) { w_reset(); w_str("copied to "); w_str(target); dim(work); }
        return;
    }
    if (argc < 3) { need("cp [-r] SRC DST"); return; }
    copy_file(argv[1], argv[2]);
}

/* A rename where the file system can do one, which moves nothing and so
   cannot lose anything -- into another directory too, and a directory with
   everything in it, on a disk this system made; a copy, checked, and then
   the delete otherwise (a file only). Into a directory is under the same
   name there. */
static void cmd_mv(int argc, char **argv) {
    if (argc < 3) { need("mv SRC DST"); return; }
    zelr_stat st;
    char target[VFS_PATH];
    if (stat(argv[2], &st) == 0 && st.is_dir) {
        const char *base = argv[1];
        for (const char *p = argv[1]; *p; p++) if (*p == '/') base = p + 1;
        path_join(argv[2], base, target, sizeof(target));
    } else {
        strncpy(target, argv[2], sizeof(target) - 1);
        target[sizeof(target) - 1] = 0;
    }
    if (zelr_rename(argv[1], target) == 0) {
        w_reset(); w_str("moved to "); w_str(target); dim(work);
        return;
    }
    if (copy_file(argv[1], argv[2])) unlink(argv[1]);
}

static void cmd_stat(int argc, char **argv) {
    if (argc < 2) { need("stat PATH"); return; }
    zelr_stat st;
    if (stat(argv[1], &st) != 0) { err("no such path"); return; }

    w_reset(); w_str("path      "); w_str(argv[1]); say(work);
    w_reset(); w_str("kind      "); w_str(st.is_dir ? "directory" : "file"); say(work);
    if (st.is_dir) {
        int n = 0;
        zelr_stat e;
        while (readdir(argv[1], n, &e) == 1) n++;
        w_reset(); w_str("entries   "); w_num((u32)n); say(work);
    } else {
        w_reset(); w_str("size      "); w_num(st.size); w_str(" bytes"); say(work);
    }

    /* Where it lives is worth saying, because two of the directories here
       are not on any disk. */
    const char *kept = "on the disk";
    if (strncmp(argv[1], "/sys", 4) == 0) kept = "generated when read, stored nowhere";
    else if (strncmp(argv[1], "/bin", 4) == 0) kept = "inside the kernel image";
    else if (strncmp(argv[1], "/tmp", 4) == 0) kept = "on the disk, cleared at boot";
    w_reset(); w_str("kept      "); w_str(kept); dim(work);
}

/* --- the machine --------------------------------------------------------

   These are the commands that only make sense on this system. /sys is a
   directory of files that do not exist until they are read, so most of what
   used to be a command is now a file, and what is left is the things that
   act rather than report. */

static const char *state_word(u32 s) {
    switch (s) {
        case TASK_READY:    return "ready";
        case TASK_RUNNING:  return "running";
        case TASK_SLEEPING: return "sleeping";
        case TASK_BLOCKED:  return "blocked";
        default:            return "dead";
    }
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    dim(" pid  ring  state      slices  name");
    zelr_task t;
    int me = getpid();
    for (int i = 0; tasks(i, &t) == 1; i++) {
        w_reset();
        w_rnum(t.pid, 4);
        w_str(t.user ? "    3  " : "    0  ");
        w_str(state_word(t.state));
        w_pad(24);
        w_rnum(t.slices, 7);
        w_str("  ");
        w_str(t.name);
        if ((int)t.pid == me) w_str("   <- this terminal");
        print(work, (int)t.pid == me ? C_ACCENT : C_FG);
    }
}

static void cmd_kill(int argc, char **argv) {
    if (argc < 2) { need("kill PID"); return; }
    int pid = parse_num(argv[1], -1);
    if (pid < 0) { err("that is not a pid"); return; }
    if (pid == getpid()) { err("not this one"); return; }
    if (kill(pid) != 0) { err("no program with that pid (the kernel's own tasks cannot be stopped)"); return; }
    w_reset(); w_str("killed "); w_num((u32)pid); dim(work);
}

/* Starts a program and waits for it, reporting what it returned. This is
   the whole point of having spawn: the terminal is a ring 3 program starting
   another ring 3 program, with the kernel only lending a hand. */
static void start_program_on(const char *path, char *const *argv,
                             bool background) {
    int pid = argv ? spawnv(path, argv) : spawn(path);
    if (pid < 0) { w_reset(); w_str("cannot start "); w_str(path); err(work); return; }

    if (background) {
        w_reset(); w_str(path); w_str(" started as pid "); w_num((u32)pid); dim(work);
        return;
    }

    int began = ticks();
    int status = wait_for(pid);
    int elapsed = ticks() - began;

    w_reset();
    w_str("pid "); w_num((u32)pid);
    w_str(status == 0 ? " finished" : " exited with ");
    if (status != 0) w_num((u32)status);
    w_str(", "); w_num((u32)elapsed); w_str(" ticks");
    print(work, status == 0 ? C_DIM : C_WARN);
}

static void start_program(const char *path, bool background) {
    start_program_on(path, 0, background);
}

static void cmd_run(int argc, char **argv) {
    if (argc < 2) { need("run PROGRAM [&]"); return; }
    bool bg = argc > 2 && argv[2][0] == '&';

    /* A bare name means one of the built-in programs, which is where a
       program with no path could sensibly be. */
    char path[VFS_PATH];
    if (argv[1][0] == '/') { strncpy(path, argv[1], sizeof(path) - 1); path[sizeof(path) - 1] = 0; }
    else path_join("/bin", argv[1], path, sizeof(path));

    start_program(path, bg);
}

/* Shorthand for reading the live tree, and with no argument, a listing of
   what is in it. */
static void cmd_sys(int argc, char **argv) {
    if (argc < 2) {
        dim("the machine, as files");
        zelr_stat st;
        for (int i = 0; readdir("/sys", i, &st) == 1; i++) {
            w_reset(); w_str("  /sys/"); w_str(st.name); say(work);
        }
        dim("none of these are stored; reading one works out the answer");
        return;
    }

    char path[VFS_PATH];
    path_join("/sys", argv[1], path, sizeof(path));
    if (read_all(path) >= 0) print(io, C_FG);
}

/* Reads a file repeatedly. Pointed at /sys this shows a number moving,
   which is the clearest demonstration that nothing there is cached. */
static void cmd_watch(int argc, char **argv) {
    if (argc < 2) { need("watch FILE [TIMES]"); return; }
    int times = argc > 2 ? parse_num(argv[2], 5) : 5;
    if (times > 20) times = 20;

    for (int i = 0; i < times; i++) {
        int total = read_all(argv[1]);
        if (total < 0) return;

        w_reset(); w_str("-- read "); w_num((u32)(i + 1)); w_str(" --"); dim(work);
        print(io, C_FG);
        if (i + 1 < times) {
            draw_all();
            win_commit(win);
            sleep_ms(500);
        }
    }
}

/* How long something takes, in ticks. The timer runs at 100 Hz, so this
   measures in hundredths of a second and says so. */
static void cmd_time(int argc, char **argv) {
    if (argc < 2) { need("time COMMAND..."); return; }

    char rest[COLS + 1];
    join_from(argv, argc, 1, rest, COLS);

    int began = ticks();
    run_line(rest);
    int elapsed = ticks() - began;

    w_reset();
    w_str("took "); w_num((u32)elapsed); w_str(" ticks, ");
    w_num((u32)(elapsed * 10)); w_str(" ms");
    good(work);
}

/* The value after a key at the start of a line of /sys/time, or 0. */
static const char *time_line(const char *t, const char *key) {
    int kl = strlen(key);
    for (const char *p = t; *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == ' ') {
            p += kl;
            while (*p == ' ') p++;
            return p;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return 0;
}

static int digits(const char *p, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) v = v * 10 + (p[i] - '0');
    return v;
}

static void w_two(int v) { w_ch((char)('0' + v / 10 % 10)); w_ch((char)('0' + v % 10)); }

/* The date and time where the machine is, as the kernel shell's date says
   it, from /sys/time: "Sunday 18 October 2026, 21:05:33 in Australia/Sydney
   (UTC+11)". */
/* A number typed as a word, or -1. */
static int word_num(const char *s) {
    int v = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000) return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

/* A line said without what it ends with in spaces. */
static void say_trimmed(void) {
    while (work_n && work[work_n - 1] == ' ') work[--work_n] = 0;
    say(work);
}

/* cal, or cal MONTH YEAR: the month, Monday first, laid out as cal always
   has, twenty columns wide with the title in the middle of them. */
static void cmd_cal(int argc, char **argv) {
    static const char *const MONTHS[12] = { "January", "February", "March", "April", "May", "June", "July",
                                            "August", "September", "October", "November", "December" };
    int y = 0, m = 0;
    if (argc >= 3) {
        m = word_num(argv[1]);
        y = word_num(argv[2]);
    } else if (argc == 2) {
        err("cal, or cal MONTH YEAR, as cal 2 2028");
        return;
    } else {
        static char t[512];
        int n = slurp("/sys/time", t, (int)sizeof(t) - 1);
        const char *local = 0;
        if (n > 0) { t[n] = 0; local = time_line(t, "local"); }
        if (!local || strlen(local) < 10) { err("cannot read the clock"); return; }
        y = digits(local, 4);
        m = digits(local + 5, 2);
    }
    if (m < 1 || m > 12 || y < 1 || y > 9999) { err("a month is 1 to 12, and a year 1 to 9999"); return; }
    static const int DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int days = DAYS[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days = 29;

    char title[32];
    int tn = 0;
    for (const char *p = MONTHS[m - 1]; *p; p++) title[tn++] = *p;
    title[tn++] = ' ';
    tn += utoa((u32)y, title + tn);
    title[tn] = 0;
    w_reset();
    for (int i = 0; i < (20 - tn) / 2; i++) w_ch(' ');
    w_str(title);
    say_trimmed();
    say("Mo Tu We Th Fr Sa Su");

    int col = (zone_weekday(zone_days_from_civil(y, m, 1)) + 6) % 7;   /* Monday is 0 */
    w_reset();
    for (int c = 0; c < col; c++) w_str(c ? "   " : "  ");
    for (int d = 1; d <= days; d++) {
        if (col) w_ch(' ');
        w_ch(d < 10 ? ' ' : (char)('0' + d / 10));
        w_ch((char)('0' + d % 10));
        if (++col == 7) { say_trimmed(); w_reset(); col = 0; }
    }
    if (col) say_trimmed();
}

/* df: how much of the disk and of a stick is used, as the kernel counts it
   (/sys/storage). */
static void size_words(u32 kib) {
    u32 tenths = (u32)((u64)kib * 10 / 1024);
    w_num(tenths / 10);
    w_ch('.');
    w_ch((char)('0' + tenths % 10));
    w_str(" MB");
}

static void cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    static char t[512];
    int n = slurp("/sys/storage", t, (int)sizeof(t) - 1);
    if (n <= 0) { err("cannot read /sys/storage"); return; }
    t[n] = 0;
    static const char *const WHICH[2] = { "disk", "stick" };
    for (int k = 0; k < 2; k++) {
        const char *l = time_line(t, WHICH[k]);
        w_reset();
        w_str(WHICH[k]);
        w_str(": ");
        u32 total = 0, freek = 0;
        const char *p = l;
        while (p && *p == ' ') p++;
        if (!p || *p < '0' || *p > '9') { w_str("none"); say(work); continue; }
        while (*p >= '0' && *p <= '9') total = total * 10 + (u32)(*p++ - '0');
        while (*p && (*p < '0' || *p > '9') && *p != '\n') p++;
        while (*p >= '0' && *p <= '9') freek = freek * 10 + (u32)(*p++ - '0');
        size_words(total);
        w_str(", ");
        size_words(total - freek);
        w_str(" used, ");
        size_words(freek);
        w_str(" free");
        say(work);
    }
}

/* du [PATH]: what is below a folder, all the way down. */
static u32 du_files, du_dirs;
static u64 du_bytes;

static void du_walk(const char *dir, int depth) {
    zelr_stat st;
    for (int i = 0; readdir(dir, i, &st) == 1; i++) {
        if (!strcmp(st.name, ".") || !strcmp(st.name, "..")) continue;
        char sub[VFS_PATH];
        path_join(dir, st.name, sub, sizeof(sub));
        if (st.is_dir) {
            du_dirs++;
            if (depth < 16) du_walk(sub, depth + 1);
        } else {
            du_files++;
            du_bytes += st.size;
        }
    }
}

static void cmd_du(int argc, char **argv) {
    char where[VFS_PATH];
    if (argc > 1) strncpy(where, argv[1], sizeof(where) - 1);
    else if (getcwd(where, sizeof(where)) < 0) strcpy(where, "/");
    where[sizeof(where) - 1] = 0;
    zelr_stat st;
    if (stat(where, &st) < 0) { err("there is nothing there"); return; }
    du_files = du_dirs = 0;
    du_bytes = 0;
    if (st.is_dir) du_walk(where, 0);
    else { du_files = 1; du_bytes = st.size; }
    w_reset();
    w_num((u32)du_bytes);
    w_str(du_bytes == 1 ? " byte in " : " bytes in ");
    w_num(du_files);
    w_str(du_files == 1 ? " file and " : " files and ");
    w_num(du_dirs);
    w_str(du_dirs == 1 ? " folder: " : " folders: ");
    w_str(where);
    say(work);
}

static void cmd_date(int argc, char **argv) {
    (void)argc; (void)argv;
    static const char *const DAYS[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const MONTHS[12] = { "January", "February", "March", "April", "May", "June", "July",
                                            "August", "September", "October", "November", "December" };
    static char t[512];
    int n = slurp("/sys/time", t, (int)sizeof(t) - 1);
    if (n <= 0) { err("cannot read the clock"); return; }
    t[n] = 0;
    const char *local = time_line(t, "local"), *zone = time_line(t, "zone"), *off = time_line(t, "offset");
    if (!local || strlen(local) < 19) { dim("no clock"); return; }
    int y = digits(local, 4), mo = digits(local + 5, 2), d = digits(local + 8, 2);
    if (mo < 1 || mo > 12) { dim("no clock"); return; }
    int offset = 0, neg = off && *off == '-';
    if (off) for (const char *p = off + neg; *p >= '0' && *p <= '9'; p++) offset = offset * 10 + (*p - '0');
    w_reset();
    w_str(DAYS[zone_weekday(zone_days_from_civil(y, mo, d))]); w_ch(' ');
    w_num((u32)d); w_ch(' '); w_str(MONTHS[mo - 1]); w_ch(' '); w_num((u32)y); w_str(", ");
    w_two(digits(local + 11, 2)); w_ch(':'); w_two(digits(local + 14, 2)); w_ch(':'); w_two(digits(local + 17, 2));
    w_str(" in ");
    for (const char *p = zone; p && *p && *p != '\n'; p++) w_ch(*p);
    w_str(" (UTC");
    if (offset) {
        w_ch(neg ? '-' : '+');
        w_num((u32)(offset / 60));
        if (offset % 60) { w_ch(':'); w_two(offset % 60); }
    }
    w_ch(')');
    say(work);
}

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    zelr_sysinfo si;
    if (sysinfo(&si) != 0) { err("cannot read the machine"); return; }
    u32 s = si.uptime_seconds;

    w_reset();
    w_str("up ");
    if (s >= 3600) { w_num(s / 3600); w_str("h "); }
    if (s >= 60)   { w_num((s / 60) % 60); w_str("m "); }
    w_num(s % 60); w_str("s, ");
    w_num(si.tasks); w_str(" tasks, ");
    w_num(si.syscalls); w_str(" system calls served");
    say(work);
}

static void cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    zelr_sysinfo si;
    if (sysinfo(&si) != 0) { err("cannot read the machine"); return; }

    /* A bar, because a ratio is easier to see than to read. */
    u32 used = si.mem_used_kb, total = si.mem_total_kb ? si.mem_total_kb : 1;
    int width = 40;
    int filled = (int)((u64)used * width / total);

    w_reset();
    w_str("physical  ");
    w_ch('[');
    for (int i = 0; i < width; i++) w_ch(i < filled ? '#' : '.');
    w_ch(']');
    say(work);

    w_reset();
    w_str("          "); w_num(used); w_str(" of "); w_num(total);
    w_str(" KiB used, "); w_num(total - used); w_str(" free");
    dim(work);

    w_reset(); w_str("heap      "); w_num(si.heap_total_kb); w_str(" KiB"); say(work);
    w_reset(); w_str("disk      "); w_num(si.disk_kb_free); w_str(" KiB free"); say(work);
}

/* --- the network -------------------------------------------------------- */

static void w_ip(u32 v) {
    for (int b = 3; b >= 0; b--) { w_num((v >> (b * 8)) & 0xFF); if (b) w_ch('.'); }
}

static void cmd_net(int argc, char **argv) {
    (void)argc; (void)argv;
    zelr_netinfo info;
    if (netinfo(&info) != 0 || !info.up) { err("no network"); return; }

    static const char *labels[4] = { "address  ", "gateway  ", "netmask  ", "resolver " };
    u32 values[4] = { info.ip, info.gateway, info.netmask, info.dns };
    for (int i = 0; i < 4; i++) {
        w_reset(); w_str(labels[i]); w_ip(values[i]); say(work);
    }

    w_reset();
    w_str("mac      ");
    for (int i = 0; i < 6; i++) {
        const char *hex = "0123456789abcdef";
        w_ch(hex[info.mac[i] >> 4]);
        w_ch(hex[info.mac[i] & 15]);
        if (i < 5) w_ch(':');
    }
    say(work);
}

static void cmd_resolve(int argc, char **argv) {
    if (argc < 2) { need("resolve HOST"); return; }
    u32 ip = 0;
    if (resolve(argv[1], &ip) != 0 || !ip) { err("could not look it up"); return; }
    w_reset(); w_str(argv[1]); w_str(" is "); w_ip(ip); good(work);
}

/* An HTTP GET, done entirely from user space through the socket calls. */
/* Whether a typed address starts with this scheme, ignoring case. */
static int starts_scheme(const char *s, const char *want) {
    for (; *want; s++, want++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *want) return 0;
    }
    return 1;
}

static void cmd_get(int argc, char **argv) {
    if (argc < 2) { need("get HOST [PATH] [FILE]"); return; }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";
    const char *save = argc > 3 ? argv[3] : 0;

    /* https unless the address says otherwise, for the same reason the
       browser does it: the plain one sends the request where anybody can
       read it, and defaulting to the safe one costs a name that has to be
       typed in full when a site really does only speak http. */
    int secure = 1;
    if (starts_scheme(host, "https://")) host += 8;
    else if (starts_scheme(host, "http://")) { secure = 0; host += 7; }

    w_reset(); w_str("connecting to "); w_str(host); dim(work);
    draw_all();
    win_commit(win);

    int sock;
    if (secure) {
        sock = connect_tls(host, 443);
        if (sock < 0) {
            static char reason[128];
            tls_why(reason, sizeof(reason));
            err(reason[0] ? reason : "could not connect");
            return;
        }
        static char what[64];
        tls_what(what, sizeof(what));
        w_reset(); w_str("secure: "); w_str(what); dim(work);
    } else {
        sock = connect(host, 80);
        if (sock < 0) { err("could not connect"); return; }
    }

    static char req[512];
    int n = 0;
    /* 1.0 keeps the answer in one piece: 1.1 lets a server chunk it, and
       nothing here puts chunks back together. */
    const char *parts[5] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                             "\r\nConnection: close\r\nUser-Agent: zelr-term\r\n\r\n" };
    for (int i = 0; i < 5; i++)
        for (const char *p = parts[i]; *p && n < (int)sizeof(req); p++) req[n++] = *p;

    if (send(sock, req, n) < 0) { err("send failed"); disconnect(sock); return; }

    int total = 0;
    int quiet = 0;
    for (;;) {
        int got = recv(sock, io + total, (int)sizeof(io) - 1 - total);
        if (got == NET_EOF) break;
        if (got < 0) break;
        if (got == 0) { if (++quiet >= 3) break; continue; }
        quiet = 0;
        total += got;
        if (total >= (int)sizeof(io) - 1) break;
    }
    disconnect(sock);
    io[total] = 0;

    if (!total) { err("nothing came back"); return; }
    w_reset(); w_num((u32)total); w_str(" bytes received"); dim(work);

    /* Split the headers off, so what gets saved is the page itself. */
    int start = 0;
    for (int i = 0; i + 3 < total; i++) {
        if (io[i] == '\r' && io[i + 1] == '\n' &&
            io[i + 2] == '\r' && io[i + 3] == '\n') { start = i + 4; break; }
    }

    char status[64];
    int sn = 0;
    while (sn < 63 && io[sn] && io[sn] != '\r' && io[sn] != '\n') { status[sn] = io[sn]; sn++; }
    status[sn] = 0;
    say(status);

    if (save) {
        if (spit(save, io + start, total - start) < 0) err("could not save it");
        else {
            w_reset(); w_str("saved "); w_num((u32)(total - start));
            w_str(" bytes to "); w_str(save);
            good(work);
        }
    } else {
        print(io + start, C_FG);
    }
}

/* --- the terminal itself ------------------------------------------------ */

static void apply_palette(int index) {
    if (index < 0 || index >= N_PALETTES) index = 0;
    pal = PALETTES[index];
}

static void save_theme(const char *name) { spit(THEME_FILE, name, strlen(name)); }

/* Which palette a terminal nobody has chosen one for should be.
 *
 * The desktop's own light mode is a system setting and this program is the
 * one window on it that does not read the system palette: a terminal has
 * its own idea of colour and always has. So it follows the setting only
 * when it has not been told otherwise, and `theme` still wins for good.
 *
 * With no settings file at all it is slate, which is not the desktop's
 * default and is meant not to be: a machine nobody has set up yet still
 * gets a dark terminal, because that is what a terminal is. */
static int default_palette(void) {
    char cfg[512];
    int n = slurp("/zelr.cfg", cfg, sizeof(cfg) - 1);
    if (n < 0) n = 0;
    cfg[n] = 0;
    /* One key, looked for at the start of a line, which is all this needs
       and less than pulling the whole settings header in for it. */
    for (int i = 0; cfg[i]; i++) {
        if (i && cfg[i - 1] != '\n') continue;
        if (strncmp(cfg + i, "light ", 6)) continue;
        return cfg[i + 6] == '0' ? 0 : 1;           /* slate, or paper */
    }
    return 0;
}

static void load_theme(void) {
    char buf[32];
    int n = slurp(THEME_FILE, buf, sizeof(buf) - 1);
    if (n <= 0) { apply_palette(default_palette()); return; }
    buf[n] = 0;
    for (int i = 0; buf[i]; i++) if (buf[i] == '\n' || buf[i] == '\r') { buf[i] = 0; break; }

    for (int i = 0; i < N_PALETTES; i++)
        if (strcmp(PALETTES[i].name, buf) == 0) { apply_palette(i); return; }
    apply_palette(default_palette());
}

static void cmd_theme(int argc, char **argv) {
    if (argc < 2) {
        dim("colour schemes");
        for (int i = 0; i < N_PALETTES; i++) {
            w_reset();
            w_str("  ");
            w_str(PALETTES[i].name);
            if (strcmp(PALETTES[i].name, pal.name) == 0) w_str("   <- in use");
            print(work, strcmp(PALETTES[i].name, pal.name) == 0 ? C_ACCENT : C_FG);
        }
        dim("theme NAME to change it; it is remembered in /cfg/term");
        return;
    }

    for (int i = 0; i < N_PALETTES; i++) {
        if (strcmp(PALETTES[i].name, argv[1]) != 0) continue;
        apply_palette(i);
        save_theme(pal.name);
        w_reset(); w_str("theme is now "); w_str(pal.name); good(work);
        return;
    }
    err("no such theme, try theme with no argument");
}

static void cmd_history(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!n_hist) { dim("nothing yet"); return; }
    for (int i = 0; i < n_hist; i++) {
        w_reset(); w_rnum((u32)(i + 1), 4); w_str("  "); w_str(hist[i]); say(work);
    }
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv; n_lines = 0; first = 0; view = 0; }

static void cmd_echo(int argc, char **argv) {
    char body[COLS + 1];
    join_from(argv, argc, 1, body, COLS);
    say(body);
}

/* Alarms: /home/alarms, "07:30 wake up" a line, which the desktop watches
   (it says them at their minute, and rings). `alarm` lists them, `alarm
   HH:MM TEXT` adds one, `alarm clear` takes them all away. */
static void cmd_alarm(int argc, char **argv) {
    static char buf[4096];
    if (argc < 2) {
        int n = slurp("/home/alarms", buf, sizeof(buf) - 1);
        if (n <= 0) { dim("no alarms"); return; }
        buf[n] = 0;
        for (char *l = buf; *l; ) {
            char *e = l;
            while (*e && *e != '\n') e++;
            char keep = *e;
            *e = 0;
            if (*l) say(l);
            if (!keep) break;
            l = e + 1;
        }
        return;
    }
    if (!strcmp(argv[1], "clear")) {
        unlink("/home/alarms");
        dim("no alarms now");
        return;
    }
    const char *t = argv[1];
    int good = strlen(t) == 5 && t[2] == ':' && t[0] >= '0' && t[0] <= '2' && t[1] >= '0' && t[1] <= '9'
               && t[3] >= '0' && t[3] <= '5' && t[4] >= '0' && t[4] <= '9' && (t[0] - '0') * 10 + (t[1] - '0') < 24;
    if (!good) { err("a time is HH:MM, as 07:30"); return; }
    char what[COLS + 1];
    join_from(argv, argc, 2, what, COLS - 8);
    int n = slurp("/home/alarms", buf, sizeof(buf) - COLS - 16);
    if (n < 0) n = 0;
    if (n && buf[n - 1] != '\n') buf[n++] = '\n';
    for (int i = 0; i < 5; i++) buf[n++] = t[i];
    if (what[0]) buf[n++] = ' ';
    for (int i = 0; what[i]; i++) buf[n++] = what[i];
    buf[n++] = '\n';
    if (spit("/home/alarms", buf, n) != n) { err("could not write /home/alarms"); return; }
    char line[COLS + 16];
    int k = 0;
    for (const char *p = "an alarm at "; *p; p++) line[k++] = *p;
    for (int i = 0; i < 5; i++) line[k++] = t[i];
    line[k] = 0;
    say(line);
}

/* A notice on the desktop, above the dock, and kept with the others there
(alt+N): a script's way of saying it has finished. */
static void cmd_notice(int argc, char **argv) {
    char body[112];
    join_from(argv, argc, 1, body, sizeof(body) - 1);
    if (!body[0]) { err("notice what?"); return; }
    if (desktop_notice(body) < 0) err("there is no desktop to show it");
}

static void cmd_cd(int argc, char **argv) {
    const char *where = argc > 1 ? argv[1] : "/home";
    if (chdir(where) != 0) { err("not a directory"); return; }
    char cwd[VFS_PATH];
    if (getcwd(cwd, sizeof(cwd)) >= 0) dim(cwd);
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char cwd[VFS_PATH];
    if (getcwd(cwd, sizeof(cwd)) >= 0) say(cwd);
}

static void cmd_about(int argc, char **argv) {
    (void)argc; (void)argv;
    good("zelr terminal");
    say("A shell that is not part of the kernel.");
    say("");
    w_reset(); w_str("pid       "); w_num((u32)getpid()); say(work);
    say("ring      3");
    say("access    system calls only");
    say("");
    dim("Listing a directory, reading a file, starting another");
    dim("program and fetching a page over TCP all cross the ring");
    dim("boundary through int 0x80. Nothing here is privileged.");
}

/* --- the command table --------------------------------------------------

   One table, read by three things: the dispatcher, help, and tab completion.
   Adding a command here is the whole job of adding a command. */

static void cmd_help(int argc, char **argv);
static void cmd_source(int argc, char **argv);      /* with run_line, below the table */
static void cmd_set(int argc, char **argv);

typedef struct {
    const char *name;
    handler     fn;
    const char *args;
    const char *what;
} command;

static const command COMMANDS[] = {
    { "help",    cmd_help,    "[COMMAND]",     "this, or detail on one command" },
    { "ls",      cmd_ls,      "[-l] [PATH]",   "list a directory, -l with when each was written" },
    { "shutdown", cmd_shutdown, "",            "turn the machine off" },
    { "reboot",  cmd_reboot,  "",              "start it again" },
    { "tree",    cmd_tree,    "[PATH]",        "everything below a directory" },
    { "cd",      cmd_cd,      "[PATH]",        "change directory, no argument goes home" },
    { "pwd",     cmd_pwd,     "",              "where you are" },
    { "cat",     cmd_cat,     "FILE...",       "print a file" },
    { "head",    cmd_head,    "FILE [LINES]",  "the first few lines" },
    { "tail",    cmd_tail,    "FILE [LINES]",  "the last few" },
    { "grep",    cmd_grep,    "[-ir] TEXT FILE...", "lines containing something, -i case aside, -r in folders" },
    { "sort",    cmd_sort,    "[FILE]",        "lines in order, of a file or after a |" },
    { "uniq",    cmd_uniq,    "[-c] [FILE]",   "lines alike in a row said once, -c counted" },
    { "diff",    cmd_diff,    "FILE FILE",     "the lines two files differ by" },
    { "find",    cmd_find,    "NAME [WHERE]",  "files whose name contains it" },
    { "hex",     cmd_hex,     "FILE [BYTES]",  "what a file really holds" },
    { "seq",     cmd_seq,     "[FIRST [STEP]] LAST", "whole numbers, one a line" },
    { "nl",      cmd_nl,      "[FILE]",        "the lines numbered" },
    { "tac",     cmd_tac,     "[FILE]",        "the lines last first" },
    { "rev",     cmd_rev,     "[FILE]",        "each line the other way round" },
    { "basename", cmd_basename, "PATH [ENDING]", "the last part of a path" },
    { "dirname", cmd_dirname, "PATH",          "all but the last part of a path" },
    { "touch",   cmd_touch,   "FILE...",       "made when it is not there, or written now" },
    { "source",  cmd_source,  "FILE [WORDS...]", "a file's lines run as if typed; $1 to $9 are the words" },
    { "set",     cmd_set,     "[NAME [TEXT]]", "a name for $NAME on any line, or the names given" },
    { "cut",     cmd_cut,     "-f LIST [-d C] FILE", "fields of each line, or -c LIST its characters" },
    { "tr",      cmd_tr,      "SET1 SET2 FILE", "bytes made others, or -d taken out" },
    { "sha256sum", cmd_sha256sum, "FILE...",   "each file's SHA-256, to compare with a sum taken elsewhere" },
    { "cmp",     cmd_cmp,     "FILE FILE",     "the first byte two files differ at" },
    { "unzip",   cmd_unzip,   "[-l] ARCHIVE [FOLDER]", "a zip, tar or .gz taken out, or -l listed" },
    { "zip",     cmd_zip,     "ARCHIVE.zip PATH...", "files and folders into a new zip" },
    { "wc",      cmd_wc,      "FILE",          "lines, words and bytes" },
    { "stat",    cmd_stat,    "PATH",          "size, kind, and where it is kept" },
    { "write",   cmd_write,   "FILE TEXT",     "create or overwrite" },
    { "append",  cmd_append,  "FILE TEXT",     "add a line to the end" },
    { "cp",      cmd_cp,      "[-r] SRC DST",  "copy a file, or a folder" },
    { "mv",      cmd_mv,      "SRC DST",       "move one" },
    { "rm",      cmd_rm,      "[-r] PATH...",  "delete, -r a folder too" },
    { "mkdir",   cmd_mkdir,   "NAME",          "make a directory" },
    { "rmdir",   cmd_rmdir,   "NAME",          "remove an empty one" },
    { "run",     cmd_run,     "PROGRAM [&]",   "start a program, & to not wait" },
    { "ps",      cmd_ps,      "",              "what is running" },
    { "kill",    cmd_kill,    "PID",           "stop a program" },
    { "sys",     cmd_sys,     "[NAME]",        "the machine, as files" },
    { "watch",   cmd_watch,   "FILE [TIMES]",  "read it again and again" },
    { "time",    cmd_time,    "COMMAND...",    "how long something takes" },
    { "mem",     cmd_mem,     "",              "memory, with a bar" },
    { "uptime",  cmd_uptime,  "",              "how long this has been running" },
    { "date",    cmd_date,    "",              "the date and time where the machine is" },
    { "cal",     cmd_cal,     "[MONTH YEAR]",  "a month, Monday first" },
    { "df",      cmd_df,      "",              "how much of the disk and a stick is used" },
    { "du",      cmd_du,      "[PATH]",        "the bytes, files and folders below a folder" },
    { "net",     cmd_net,     "",              "the address, if there is one" },
    { "resolve", cmd_resolve, "HOST",          "look up a name" },
    { "get",     cmd_get,     "HOST [PATH] [FILE]", "fetch a page, https unless said" },
    { "theme",   cmd_theme,   "[NAME]",        "change the colours" },
    { "history", cmd_history, "",              "what you have typed" },
    { "echo",    cmd_echo,    "TEXT",          "print it back" },
    { "notice",  cmd_notice,  "TEXT",          "say it on the desktop, kept under alt+N" },
    { "alarm",   cmd_alarm,   "[HH:MM TEXT]",  "list the alarms, or set one; alarm clear" },
    { "clear",   cmd_clear,   "",              "empty the scrollback" },
    { "about",   cmd_about,   "",              "what this program is" },
};
#define N_COMMANDS ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

static const command *command_named(const char *name) {
    for (int i = 0; i < N_COMMANDS; i++)
        if (strcmp(COMMANDS[i].name, name) == 0) return &COMMANDS[i];
    return 0;
}

static void cmd_help(int argc, char **argv) {
    if (argc > 1) {
        const command *c = command_named(argv[1]);
        if (!c) { err("no such command"); return; }
        w_reset(); w_str(c->name); w_ch(' '); w_str(c->args); good(work);
        w_reset(); w_str("  "); w_str(c->what); say(work);
        return;
    }

    dim("commands");
    for (int i = 0; i < N_COMMANDS; i++) {
        w_reset();
        w_str("  ");
        w_str(COMMANDS[i].name);
        w_ch(' ');
        w_str(COMMANDS[i].args);
        w_pad(28);
        w_str(COMMANDS[i].what);
        say(work);
    }
    say("");
    dim("A name on its own runs that program: here, then /usb, then /bin.");
    dim("CMD | CMD hands what one says to the next; > FILE writes it, >> adds.");
    dim("Tab completes. Up and down walk through history; ctrl+R looks in it.");
    dim("PageUp and PageDown scroll. Escape leaves the desktop.");
}

/* Where a name is looked for, in order.
 *
 * It used to be /bin and nothing else, which is the directory of programs
 * pasted into the kernel image: a program had to be built into the machine
 * to be run by typing its name. The kernel could always load an ELF off the
 * disk -- spawn reads through the same VFS as everything else -- so what
 * was missing was not the loading, it was the looking.
 *
 * The working directory comes first, because a program somebody has just
 * downloaded or copied is the one they mean. /bin is last, so a name that
 * exists in both runs the one in front of you rather than the one that
 * shipped, which is the way round that makes a program replaceable. */
static const char *const PROG_PATH[] = { 0, "/usb", "/bin" };
#define PROG_PATH_N ((int)(sizeof(PROG_PATH) / sizeof(PROG_PATH[0])))

/* Fills `out` with the first of those that holds `name`. */
static bool find_program(const char *name, char *out, int cap) {
    zelr_stat st;

    /* A name with a slash in it is a path already and is not searched for:
       "./mine" and "/home/mine" mean what they say. */
    for (const char *p = name; *p; p++)
        if (*p == '/') {
            int n = 0;
            while (name[n] && n < cap - 1) { out[n] = name[n]; n++; }
            out[n] = 0;
            return stat(out, &st) == 0 && !st.is_dir;
        }

    for (int i = 0; i < PROG_PATH_N; i++) {
        const char *dir = PROG_PATH[i];
        if (!dir) {
            char here[VFS_PATH];
            if (getcwd(here, sizeof(here)) < 0) continue;
            path_join(here, name, out, cap);
        } else {
            path_join(dir, name, out, cap);
        }
        if (stat(out, &st) == 0 && !st.is_dir) return true;
    }
    return false;
}

/* Runs one command line. Anything that is not a command is looked for as a
   program, so one can be started by typing its name. */
/* The output of a command of the terminal's own into a file: "> FILE"
   writes it, ">> FILE" adds to what is there. The first > outside double
   quotes is the one. A program from /bin prints to the console, not here,
   so it cannot be sent anywhere from this line, and says so. */
#define STAGES 8
static char *st_argv[STAGES][16];
static int st_argc[STAGES];
static const command *st_cmd[STAGES];

/* Cuts a line at its |s outside double quotes, and each piece into words:
   the number of commands, or 0 once it has said what is wrong. */
static int stages_of(char *line, const char *empty) {
    char *piece[STAGES];
    int n = 0, quoted = 0;
    piece[n++] = line;
    for (char *p = line; *p; p++) {
        if (*p == '"') quoted = !quoted;
        if (*p == '|' && !quoted) {
            if (n == STAGES) { err("eight commands to a line at most"); return 0; }
            *p = 0;
            piece[n++] = p + 1;
        }
    }
    for (int i = 0; i < n; i++) {
        st_argc[i] = split(piece[i], st_argv[i], 16);
        if (!st_argc[i]) { err(n > 1 ? "a | needs a command on each side" : empty); return 0; }
        st_cmd[i] = command_named(st_argv[i][0]);
    }
    return n;
}

/* Runs a line's commands in turn, each one's output kept and handed to the
   next as its input; the last one's is said, or kept in cap_buf when it
   goes to a file. A program from /bin prints to the console, so it can
   take no part, and nothing runs. 0 when refused. */
static int run_stages(int n, int keep) {
    for (int i = 0; i < n; i++)
        if (!st_cmd[i]) {
            err(n > 1 ? "only the terminal's own commands can be piped"
                      : "only the terminal's own commands can be sent to a file");
            return 0;
        }
    pipe_n = -1;
    for (int i = 0; i < n; i++) {
        if (keep || i < n - 1) { capturing = 1; cap_n = 0; }
        st_cmd[i]->fn(st_argc[i], st_argv[i]);
        capturing = 0;
        if (i < n - 1) {
            for (int k = 0; k < cap_n; k++) pipe_buf[k] = cap_buf[k];
            pipe_n = cap_n;
        }
    }
    pipe_n = -1;
    return 1;
}

static int run_into(char *cmdline, char *to, int append) {
    while (*to == ' ') to++;
    char *e = to + strlen(to);
    while (e > to && e[-1] == ' ') *--e = 0;
    if (!*to) { err("to a file: COMMAND > FILE, or >> to add to it"); return 1; }
    for (const char *q = to; *q; q++)
        if (*q == '|') { err("the file goes last: CMD | CMD > FILE"); return 1; }
    int stages = stages_of(cmdline, "nothing to send to a file");
    if (!stages) return 1;

    static char before[65536];
    int had = 0;
    if (append) {
        had = slurp(to, before, sizeof(before));
        if (had < 0) had = 0;
    }
    if (!run_stages(stages, 1)) return 1;

    /* What was there first, then what was said: one write either way. */
    static char out[131072];
    int n = 0;
    for (int i = 0; i < had; i++) out[n++] = before[i];
    for (int i = 0; i < cap_n; i++) out[n++] = cap_buf[i];
    if (spit(to, out, n) != n) { err("could not write that file"); return 1; }
    w_reset();
    w_num((u32)cap_n);
    w_str(append ? " bytes added to " : " bytes written to ");
    w_str(to);
    dim(work);
    return 1;
}

/* --- scripts and names ------------------------------------------------------
 *
 * source FILE [WORDS...] runs a file's lines one after another as if each
 * were typed, passing over empty lines and those that begin with #. In them
 * $1 to $9 are the words given after the file, $0 the file, $# how many and
 * $* all of them; on any line, typed or in a script, $NAME is what `set NAME
 * TEXT` gave it (nothing when nothing did, as other shells have it). One
 * script at a time: one that sources another, itself included, is told it
 * cannot, rather than going round for ever. */
#define VARS 32
static char var_name[VARS][16], var_text[VARS][COLS + 1];
static int  nvars;
static const char *script_word[10];
static int  script_words, in_script;
static char script_text[IO_BUF];

static int var_find(const char *name, int n) {
    for (int i = 0; i < nvars; i++)
        if ((int)strlen(var_name[i]) == n && !strncmp(var_name[i], name, n)) return i;
    return -1;
}

static bool name_char(char c, bool first) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (!first && c >= '0' && c <= '9');
}

/* A line with its $s put in, cut at cap. */
static void expand(const char *in, char *out, int cap) {
    int o = 0;
    for (int i = 0; in[i] && o < cap - 1; ) {
        if (in[i] != '$') { out[o++] = in[i++]; continue; }
        const char *put = 0;
        char num[12];
        int take = 1;
        char c = in[i + 1];
        if (c >= '0' && c <= '9') {
            int k = c - '0';
            put = in_script && k < script_words ? script_word[k] : "";
            take = 2;
        } else if (c == '#') {
            num[utoa((u32)(in_script && script_words ? script_words - 1 : 0), num)] = 0;
            put = num;
            take = 2;
        } else if (c == '*') {
            for (int k = 1; in_script && k < script_words; k++) {
                if (k > 1 && o < cap - 1) out[o++] = ' ';
                for (const char *p = script_word[k]; *p && o < cap - 1; p++) out[o++] = *p;
            }
            i += 2;
            continue;
        } else if (name_char(c, true)) {
            int n = 1;
            while (name_char(in[i + 1 + n], false)) n++;
            int v = var_find(in + i + 1, n);
            put = v >= 0 ? var_text[v] : "";
            take = 1 + n;
        } else {
            out[o++] = in[i++];             /* a $ that names nothing is a $ */
            continue;
        }
        for (const char *p = put; *p && o < cap - 1; p++) out[o++] = *p;
        i += take;
    }
    out[o] = 0;
}

/* set NAME TEXT gives a name its text, set NAME takes it away, and set
   alone lists them, NAME=TEXT a line. */
static void cmd_set(int argc, char **argv) {
    if (argc < 2) {
        for (int i = 0; i < nvars; i++) {
            w_reset(); w_str(var_name[i]); w_ch('='); w_str(var_text[i]); say(work);
        }
        return;
    }
    const char *name = argv[1];
    int n = strlen(name);
    bool good = n > 0 && n < (int)sizeof(var_name[0]) && name_char(name[0], true);
    for (int i = 1; good && i < n; i++) good = name_char(name[i], false);
    if (!good) { err("a name is letters, digits and _, fifteen at most, not starting with a digit"); return; }
    int v = var_find(name, n);
    if (argc < 3) {
        if (v >= 0) {
            for (int i = v; i < nvars - 1; i++) {
                strcpy(var_name[i], var_name[i + 1]);
                strcpy(var_text[i], var_text[i + 1]);
            }
            nvars--;
        }
        return;
    }
    if (v < 0) {
        if (nvars == VARS) { err("thirty-two names at most"); return; }
        v = nvars++;
        strcpy(var_name[v], name);
    }
    join_from(argv, argc, 2, var_text[v], (int)sizeof(var_text[v]));
}

static void cmd_source(int argc, char **argv) {
    if (argc < 2) { need("source FILE [WORDS...]"); return; }
    if (in_script) { err("a script cannot run another here"); return; }
    int total = read_all(argv[1]);
    if (total < 0) return;
    for (int i = 0; i < total; i++) script_text[i] = io[i];
    script_text[total] = 0;
    script_words = argc - 1 < 10 ? argc - 1 : 10;
    for (int k = 0; k < script_words; k++) script_word[k] = argv[1 + k];
    in_script = 1;
    int names_kept = glob_n;                /* the words given may be in the pool */
    for (int i = 0, start = 0; i <= total; i++) {
        if (i < total && script_text[i] != '\n') continue;
        int n = i - start;
        if (n > 0 && script_text[start + n - 1] == '\r') n--;
        char line[COLS + 1];
        if (n > COLS) n = COLS;
        for (int k = 0; k < n; k++) line[k] = script_text[start + k];
        line[n] = 0;
        start = i + 1;
        int lead = 0;
        while (line[lead] == ' ' || line[lead] == '\t') lead++;
        if (!line[lead] || line[lead] == '#') continue;
        glob_n = names_kept;
        run_line(line + lead);
    }
    in_script = 0;
    script_words = 0;
}

static void run_line(char *given) {
    char cmdline[COLS * 2 + 1];
    expand(given, cmdline, (int)sizeof(cmdline));
    int quoted = 0, piped = 0;
    for (char *p = cmdline; *p; p++) {
        if (*p == '"') quoted = !quoted;
        if (*p == '|' && !quoted) piped = 1;
        if (*p == '>' && !quoted) {
            int append = p[1] == '>';
            *p = 0;
            run_into(cmdline, p + 1 + append, append);
            return;
        }
    }
    /* A line with no | runs as it always has, so that time's line inside a
       pipe keeps the input handed to it. */
    if (piped) {
        int n = stages_of(cmdline, "nothing to run");
        if (n) run_stages(n, 0);
        return;
    }

    char *argv[16];
    int argc = split(cmdline, argv, 16);
    if (!argc) return;

    const command *c = command_named(argv[0]);
    if (c) { c->fn(argc, argv); return; }

    char path[VFS_PATH];
    if (find_program(argv[0], path, sizeof(path))) {
        bool bg = argc > 1 && argv[argc - 1][0] == '&';

        /* Every word, the name first, which is the line as typed. The
           ampersand is this terminal's and not the program's, so it is
           dropped; the vector is ended with a null because that is what
           spawnv counts up to. */
        int n = bg ? argc - 1 : argc;
        if (n > 15) n = 15;
        argv[n] = 0;
        start_program_on(path, argv, bg);
        return;
    }

    w_reset(); w_str(argv[0]);
    w_str(": not a command, and no program of that name here, on a stick or in /bin");
    err(work);
}

/* --- history ------------------------------------------------------------ */

static void history_add(const char *line) {
    if (!line[0]) return;
    if (n_hist && strcmp(hist[n_hist - 1], line) == 0) return;  /* no repeats */

    if (n_hist == MAX_HIST) {
        for (int i = 1; i < MAX_HIST; i++) strncpy(hist[i - 1], hist[i], COLS + 1);
        n_hist--;
    }
    strncpy(hist[n_hist++], line, COLS + 1);
}

/* History outlives the window, which is the point of keeping it in a file.
   It is rewritten whole each time, which at sixty lines is nothing. */
static void history_save(void) {
    int n = 0;
    for (int i = 0; i < n_hist && n < (int)sizeof(io) - 2; i++) {
        for (const char *p = hist[i]; *p && n < (int)sizeof(io) - 2; p++) io[n++] = *p;
        io[n++] = '\n';
    }
    if (n) spit(HISTORY_FILE, io, n);
}

static void history_load(void) {
    int total = slurp(HISTORY_FILE, io, sizeof(io) - 1);
    if (total <= 0) return;
    io[total] = 0;

    int start = 0;
    for (int i = 0; i <= total; i++) {
        if (i != total && io[i] != '\n') continue;
        char save = io[i];
        io[i] = 0;
        if (i > start) history_add(io + start);
        io[i] = save;
        start = i + 1;
    }
}

/* --- tab completion -----------------------------------------------------

   Completing the first word means a command name or a program in /bin;
   completing a later one means a path. Either way the rule is the same:
   fill in as far as every candidate agrees, and if that adds nothing, show
   what the candidates are. */

static char cand[32][VFS_NAME];
static int  n_cand;

static void cand_add(const char *name) {
    if (n_cand >= 32) return;
    strncpy(cand[n_cand], name, VFS_NAME - 1);
    cand[n_cand][VFS_NAME - 1] = 0;
    n_cand++;
}

/* How much of the front all the candidates share. */
static int common_prefix(void) {
    if (!n_cand) return 0;
    int k = 0;
    for (;; k++) {
        char c = cand[0][k];
        if (!c) return k;
        for (int i = 1; i < n_cand; i++)
            if (cand[i][k] != c) return k;
    }
}

/* Splits the word the cursor is inside out of the line. */
static int word_start(void) {
    int s = in_pos;
    while (s > 0 && input[s - 1] != ' ') s--;
    return s;
}

static bool first_word(int start) {
    for (int i = 0; i < start; i++) if (input[i] != ' ') return false;
    return true;
}

static void replace_word(int start, const char *with, int len) {
    /* Everything after the cursor is kept, so completing in the middle of a
       line does not eat the rest of it. */
    char tail[COLS + 1];
    strncpy(tail, input + in_pos, COLS + 1);

    int n = start;
    for (int i = 0; i < len && n < COLS; i++) input[n++] = with[i];
    in_pos = n;
    for (int i = 0; tail[i] && n < COLS; i++) input[n++] = tail[i];
    input[n] = 0;
    in_len = n;
}

static void complete(void) {
    int start = word_start();
    const char *stem = input + start;
    int stem_len = in_pos - start;
    n_cand = 0;

    if (first_word(start)) {
        for (int i = 0; i < N_COMMANDS; i++)
            if (strncmp(COMMANDS[i].name, stem, stem_len) == 0) cand_add(COMMANDS[i].name);

        zelr_stat st;
        for (int i = 0; readdir("/bin", i, &st) == 1; i++)
            if (strncmp(st.name, stem, stem_len) == 0) cand_add(st.name);
    } else {
        /* A path: everything up to the last slash names the directory to
           look in, and what follows is what is being completed. */
        int slash = -1;
        for (int i = 0; i < stem_len; i++) if (stem[i] == '/') slash = i;

        char dir[VFS_PATH];
        if (slash < 0) {
            strcpy(dir, ".");
        } else if (slash == 0) {
            strcpy(dir, "/");
        } else {
            int n = slash < (int)sizeof(dir) - 1 ? slash : (int)sizeof(dir) - 1;
            for (int i = 0; i < n; i++) dir[i] = stem[i];
            dir[n] = 0;
        }

        const char *leaf = stem + slash + 1;
        int leaf_len = stem_len - slash - 1;

        zelr_stat st;
        for (int i = 0; readdir(dir, i, &st) == 1; i++) {
            if (strncmp(st.name, leaf, leaf_len) != 0) continue;
            /* A directory completes with its slash, so the next tab
               carries straight on into it. */
            char with[VFS_NAME];
            strncpy(with, st.name, VFS_NAME - 2);
            with[VFS_NAME - 2] = 0;
            if (st.is_dir) {
                int n = strlen(with);
                with[n] = '/';
                with[n + 1] = 0;
            }
            cand_add(with);
        }
        start += slash + 1;
        stem_len = leaf_len;
    }

    if (!n_cand) return;

    int shared = common_prefix();
    if (shared > stem_len) {
        replace_word(start, cand[0], shared);
        /* Exactly one match, and it is a whole word: a space after it saves
           a keystroke and is what everyone expects. */
        if (n_cand == 1 && cand[0][shared - 1] != '/' && in_len < COLS) {
            input[in_len++] = ' ';
            input[in_len] = 0;
            in_pos = in_len;
        }
        return;
    }

    if (n_cand == 1) return;

    /* Nothing more is shared, so show what the choices are. */
    w_reset();
    for (int i = 0; i < n_cand; i++) {
        if (work_n + (int)strlen(cand[i]) + 2 >= cols) { dim(work); w_reset(); }
        w_str(cand[i]);
        w_str("  ");
    }
    if (work_n) dim(work);
}

/* --- the line editor ---------------------------------------------------- */

static void set_input(const char *s) {
    strncpy(input, s, COLS + 1);
    in_len = strlen(input);
    in_pos = in_len;
}

static void submit(void) {
    /* The prompt line is echoed into the scrollback so the transcript reads
       as a conversation rather than a pile of output. */
    char cwd[VFS_PATH];
    if (getcwd(cwd, sizeof(cwd)) < 0) strcpy(cwd, "/");
    w_reset(); w_str(cwd); w_str(" > "); w_str(input);
    print(work, C_DIM);

    char copy[COLS + 1];
    strncpy(copy, input, COLS + 1);
    history_add(copy);

    in_len = in_pos = in_off = 0;
    input[0] = 0;
    hist_at = 0;
    view = 0;

    glob_n = 0;
    run_line(copy);
}

static void insert_char(char c) {
    /* The whole line, not the part of it that fits on screen.
     *
       This used to stop at the window's width, silently: a command longer
       than the row simply lost its end, with nothing to say so. What that
       looked like was `browser https://.../booting-and-dual-booting-of-
       operating-system/` starting on a page that did not exist, because
       everything past column eighty had never been typed. An address is
       the ordinary case of a long line, so the line scrolls instead. */
    if (in_len >= COLS) return;
    for (int i = in_len; i > in_pos; i--) input[i] = input[i - 1];
    input[in_pos++] = c;
    in_len++;
    input[in_len] = 0;
}

static void delete_at(int at) {
    if (at < 0 || at >= in_len) return;
    for (int i = at; i < in_len; i++) input[i] = input[i + 1];
    in_len--;
}

/* Up and down walk back through what has been typed. The line in progress is
   set aside on the way up and put back when you come all the way down, so
   browsing history never loses what you were writing. */
static void history_step(int delta) {
    if (!n_hist) return;

    if (hist_at == 0 && delta > 0) strncpy(hist_stash, input, COLS + 1);

    int at = hist_at + delta;
    if (at < 0) at = 0;
    if (at > n_hist) at = n_hist;
    if (at == hist_at) return;
    hist_at = at;

    if (hist_at == 0) set_input(hist_stash);
    else               set_input(hist[n_hist - hist_at]);
}

/* The newest line before `before` with the text in it, or -1. */
static int hs_find(int before) {
    for (int i = before - 1; i >= 0; i--) {
        const char *h = hist[i];
        for (int j = 0; h[j]; j++) {
            int k = 0;
            while (k < hs_len && h[j + k] == hs_text[k]) k++;
            if (k == hs_len) return i;
        }
        if (!hs_len) return i;
    }
    return -1;
}

static void hs_key(u32 key) {
    if (KEY_CTRL(key)) {
        int c = key_ctrl_letter(key);
        if (c == 'r' && hs_found > 0) {
            int f = hs_find(hs_found);
            if (f >= 0) hs_found = f;
        } else if (c == 'g') {
            hs_on = 0;
        }
        return;
    }
    key = KEY_CODE(key);
    if (key == '\n') {
        hs_on = 0;
        if (hs_found >= 0) { set_input(hist[hs_found]); submit(); }
    } else if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN
               || key == KEY_HOME || key == KEY_END) {
        hs_on = 0;
        if (hs_found >= 0) set_input(hist[hs_found]);
    } else if (key == '\b') {
        if (!hs_len) { hs_on = 0; return; }
        hs_text[--hs_len] = 0;
        hs_found = hs_find(n_hist);
    } else if (key >= ' ' && key < 127 && hs_len < COLS) {
        hs_text[hs_len++] = (char)key;
        hs_text[hs_len] = 0;
        hs_found = hs_find(n_hist);
    }
}

static void on_key(u32 key) {
    if (hs_on) { hs_key(key); return; }
    /* Control chords first: they are not text and must not be typed. */
    if (KEY_CTRL(key)) {
        int c = key_ctrl_letter(key);
        if (c == 'r') {
            hs_on = 1;
            hs_len = 0;
            hs_text[0] = 0;
            hs_found = hs_find(n_hist);
            return;
        }
        if (c == 'c') { copy_selection(); return; }
        if (c == 'v') { paste_clipboard(); return; }
        if (c == 'a') {                        /* select everything on screen */
            if (n_lines > 0) {
                sel_active = 1;
                sel_l0 = 0; sel_c0 = 0;
                sel_l1 = n_lines - 1;
                sel_c1 = strlen(line_at(n_lines - 1));
            }
            return;
        }
        if (c == 'l') { sel_clear(); return; }
        return;
    }
    key = KEY_CODE(key);

    /* Typing replaces a selection's reason to exist. */
    if (sel_active && key != KEY_PAGE_UP && key != KEY_PAGE_DOWN) sel_clear();

    /* Anything typed leaves the scrollback and comes back to the prompt,
       which is less surprising than typing into a view you cannot see. */
    if (view && key != KEY_PAGE_UP && key != KEY_PAGE_DOWN) view = 0;

    switch (key) {
        case '\n': submit(); return;
        case '\t': complete(); return;
        case '\b': if (in_pos > 0) { delete_at(in_pos - 1); in_pos--; } return;

        case KEY_LEFT:  if (in_pos > 0) in_pos--; return;
        case KEY_RIGHT: if (in_pos < in_len) in_pos++; return;
        case KEY_HOME:  in_pos = 0; return;
        case KEY_END:   in_pos = in_len; return;
        case KEY_DELETE: delete_at(in_pos); return;

        case KEY_UP:   history_step(1); return;
        case KEY_DOWN: history_step(-1); return;

        case KEY_PAGE_UP: {
            int max = n_lines - (rows - 1);
            if (max < 0) max = 0;
            view += rows / 2;
            if (view > max) view = max;
            return;
        }
        case KEY_PAGE_DOWN:
            view -= rows / 2;
            if (view < 0) view = 0;
            return;

        /* F1 is help, because somebody who has never seen this before will
           press it before they read anything. */
        case KEY_F1: {
            char line[8];
            strcpy(line, "help");
            glob_n = 0;
            run_line(line);
            return;
        }
    }

    if (key >= 32 && key <= 126) insert_char((char)key);
}

/* --- the loop ----------------------------------------------------------- */

/* Works out how much text the window holds now. Called at startup and
   again every time the desktop hands over a different size. */
static void fit_to_window(void) {
    cols = (scr.w - PAD * 2) / MONO_W;
    rows = (scr.h - PAD * 2) / MONO_H;
    if (cols > COLS) cols = COLS;
    if (cols < 8) cols = 8;
    if (rows < 2) rows = 2;
}

int main(void) {
    win = win_create("terminal", 760, 480);
    if (win < 0) { puts("term: no window\n"); return 1; }

    scr.px = win_surface(win);
    if (!scr.px) { puts("term: no surface\n"); return 1; }
    scr.w = win_width(win);
    scr.h = win_height(win);
    if (scr.w <= 0 || scr.h <= 0) return 1;

    fit_to_window();

    /* The desktop will not resize a window whose program has not said it
       can cope. This one can: the scrollback is stored as text, so a new
       size is only a different number of rows to lay it out in. */
    win_allow_resize(win);

    load_theme();
    history_load();

    good("zelr terminal");
    dim("A shell running in ring 3. Type help, or press F1.");
    dim("Tab completes, up and down are history, PageUp scrolls.");
    say("");

    blink_from = ticks();
    draw_all();
    win_commit(win);
    int lit = cursor_lit();

    for (;;) {
        win_event ev;
        int changed = 0;

        while (win_poll(win, &ev) == 1) {
            if (ev.type == WIN_EV_CLOSE) {
                history_save();
                win_close(win);
                return 0;
            }
            if (ev.type == WIN_EV_RESIZE) {
                /* The surface was replaced. Its address is the same, but
                   everything drawn into it is gone and the shape changed. */
                scr.w = ev.x;
                scr.h = ev.y;
                fit_to_window();
                view = 0;
                changed = 1;
            }
            if (ev.type == WIN_EV_SCROLL) {
                /* Three lines a step, which is what everything that has
                   ever had a wheel does. view counts backwards from the
                   end, so turning the wheel down means less of it. */
                int max = n_lines - (rows - 1);
                if (max < 0) max = 0;
                view -= ev.y * 3;
                if (view < 0) view = 0;
                if (view > max) view = max;
                changed = 1;
            }
            if (ev.type == WIN_EV_KEY) { on_key(ev.key); changed = 1; blink_from = ticks(); }
            if (ev.type == WIN_EV_MOUSE) {
                int line, col;
                point_to_cell(ev.x, ev.y, &line, &col);

                if (ev.buttons & WIN_BTN_DOWN) {
                    /* A press starts a selection rather than clearing one,
                       so a click and a drag are the same gesture and only
                       the release decides which it was. */
                    sel_dragging = 1;
                    sel_active = 0;
                    sel_l0 = sel_l1 = line;
                    sel_c0 = sel_c1 = col;
                    changed = 1;
                } else if (sel_dragging && (ev.buttons & WIN_BTN_LEFT)) {
                    sel_l1 = line;
                    sel_c1 = col;
                    sel_active = (sel_l0 != sel_l1 || sel_c0 != sel_c1);
                    changed = 1;
                } else if (sel_dragging) {
                    sel_dragging = 0;
                    changed = 1;
                }
            }
        }

        /* Only the cursor due to change: its row alone is drawn and sent,
           and nothing at all while the view is scrolled back, where no
           cursor is shown. */
        int blink = !changed && cursor_lit() != lit;
        if (cursor_lit() != lit) changed = 1;  /* the cursor needs a repaint */

        if (blink) {
            if (view == 0) {
                int py = draw_prompt();
                win_commit_rect(win, 0, py, scr.w, scr.h - py);
            }
            lit = cursor_lit();
        } else if (changed) {
            draw_all();
            win_commit(win);
            lit = cursor_lit();
        }

        /* Asleep until something arrives or the cursor is due to change. */
        int into = (ticks() - blink_from) % BLINK_TICKS;
        win_wait(win, (BLINK_TICKS - into) * 10 + 5);
    }
}
