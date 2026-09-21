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
#include "draw.h"

#define COLS      160
#define MAX_LINES 400
#define PAD       8
#define MAX_HIST  64

/* What the kernel will accept, so a path built here can always be passed
   back to it. */
#define VFS_PATH  128
#define VFS_NAME  32

/* Roughly one terminal's worth of file. Anything larger is printed in full
   but never held in one piece. */
#define IO_BUF 16384

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
static int  in_len, in_pos;
static int  blink;

/* What has been typed before. The newest is at the end; hist_at is where the
   up arrow has walked back to, counted from there. */
static char hist[MAX_HIST][COLS + 1];
static int  n_hist;
static int  hist_at;            /* 0 means the line being typed */
static char hist_stash[COLS + 1];   /* the line set aside while browsing */

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

/* Prints text, breaking it at newlines and wrapping anything too wide. */
static void print(const char *s, u8 slot) {
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

    char cwd[128];
    if (getcwd(cwd, sizeof(cwd)) < 0) strcpy(cwd, "/");
    face_draw(&scr, PAD, py, cwd, pal.accent, UI_FACE_MONO);
    int x = PAD + (strlen(cwd) + 1) * MONO_W;
    face_draw(&scr, x, py, "> ", pal.dim, UI_FACE_MONO);
    x += 2 * MONO_W;
    face_draw(&scr, x, py, input, pal.fg, UI_FACE_MONO);

    /* A block cursor sitting on the character it is in front of, so editing
       in the middle of a line is visible rather than guessed at. */
    if (view == 0 && (blink / 12) % 2 == 0) {
        int cx = x + in_pos * MONO_W;
        if (in_pos < in_len) {
            rect(&scr, cx, py, MONO_W, MONO_H, pal.cursor);
            char one[2] = { input[in_pos], 0 };
            face_draw(&scr, cx, py, one, pal.bg, UI_FACE_MONO);
        } else {
            rect(&scr, cx, py, 2, MONO_H, pal.cursor);
        }
    }

    if (view > 0) {
        w_reset();
        w_str(" "); w_num((u32)view); w_str(" lines back, End returns ");
        int tw = strlen(work) * MONO_W;
        rect(&scr, scr.w - tw - PAD, PAD - 2, tw, MONO_H + 2, pal.dim);
        face_draw(&scr, scr.w - tw - PAD, PAD, work, pal.bg, UI_FACE_MONO);
    }
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

static int split(char *s, char **argv, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        argv[n++] = s;
        while (*s && *s != ' ') s++;
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

static void cmd_ls(int argc, char **argv) {
    const char *where = argc > 1 ? argv[1] : ".";
    zelr_stat st;
    int files = 0, dirs = 0;
    u32 bytes = 0;

    /* A path that is a file rather than a directory should show that file,
       not nothing at all. */
    if (stat(where, &st) == 0 && !st.is_dir) {
        w_reset(); w_rnum(st.size, 10); w_str("  "); w_str(where); say(work);
        return;
    }

    for (int i = 0; ; i++) {
        if (readdir(where, i, &st) != 1) break;
        w_reset();
        if (st.is_dir) {
            w_str("     <dir>  ");
            w_str(st.name);
            w_ch('/');
            print(work, C_ACCENT);
            dirs++;
        } else {
            w_rnum(st.size, 10);
            w_str("  ");
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

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) { need("cat FILE"); return; }
    for (int i = 1; i < argc; i++) {
        if (argc > 2) { w_reset(); w_str("== "); w_str(argv[i]); w_str(" =="); dim(work); }
        if (read_all(argv[i]) >= 0) print(io, C_FG);
    }
}

/* Prints the first or last few lines. Reading the whole file to do it is
   fine at these sizes and keeps one code path. */
static void head_tail(const char *path, int count, bool from_end) {
    int total = read_all(path);
    if (total < 0) return;

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

static void cmd_head(int argc, char **argv) {
    if (argc < 2) { need("head FILE [LINES]"); return; }
    head_tail(argv[1], argc > 2 ? parse_num(argv[2], 10) : 10, false);
}

static void cmd_tail(int argc, char **argv) {
    if (argc < 2) { need("tail FILE [LINES]"); return; }
    head_tail(argv[1], argc > 2 ? parse_num(argv[2], 10) : 10, true);
}

static void cmd_wc(int argc, char **argv) {
    if (argc < 2) { need("wc FILE"); return; }
    int total = read_all(argv[1]);
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
    w_rnum((u32)total, 9); w_str(" bytes  ");
    w_str(argv[1]);
    say(work);
}

/* Lines of a file containing a string. The one search that is worth having
   built in, because there is no pipe to build it out of. */
static void cmd_grep(int argc, char **argv) {
    if (argc < 3) { need("grep TEXT FILE..."); return; }
    const char *needle = argv[1];
    int nl = strlen(needle);
    int hits = 0;

    for (int f = 2; f < argc; f++) {
        int total = read_all(argv[f]);
        if (total < 0) continue;

        int line = 1, start = 0;
        for (int i = 0; i <= total; i++) {
            if (i != total && io[i] != '\n') continue;

            char save = io[i];
            io[i] = 0;
            for (int k = start; k + nl <= i; k++) {
                if (strncmp(io + k, needle, nl) != 0) continue;
                w_reset();
                if (argc > 3) { w_str(argv[f]); w_ch(':'); }
                w_num((u32)line); w_str(": ");
                w_str(io + start);
                say(work);
                hits++;
                break;
            }
            io[i] = save;
            line++;
            start = i + 1;
        }
    }
    if (!hits) dim("no matches");
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

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) { need("rm FILE..."); return; }
    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) == 0) { w_reset(); w_str("removed "); w_str(argv[i]); dim(work); }
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

/* Copy, and move built out of it. Whole file at a time, like everything
   else here. */
static bool copy_file(const char *src, const char *dst) {
    int total = read_all(src);
    if (total < 0) return false;

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

    if (spit(target, io, total) < 0) { err("cannot write it"); return false; }
    w_reset(); w_num((u32)total); w_str(" bytes to "); w_str(target); dim(work);
    return true;
}

static void cmd_cp(int argc, char **argv) {
    if (argc < 3) { need("cp SRC DST"); return; }
    copy_file(argv[1], argv[2]);
}

static void cmd_mv(int argc, char **argv) {
    if (argc < 3) { need("mv SRC DST"); return; }
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
    if (kill(pid) != 0) { err("no such task"); return; }
    w_reset(); w_str("killed "); w_num((u32)pid); dim(work);
}

/* Starts a program and waits for it, reporting what it returned. This is
   the whole point of having spawn: the terminal is a ring 3 program starting
   another ring 3 program, with the kernel only lending a hand. */
static void start_program_on(const char *path, const char *arg, bool background) {
    int pid = arg && arg[0] ? spawn_arg(path, arg) : spawn(path);
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

typedef struct {
    const char *name;
    handler     fn;
    const char *args;
    const char *what;
} command;

static const command COMMANDS[] = {
    { "help",    cmd_help,    "[COMMAND]",     "this, or detail on one command" },
    { "ls",      cmd_ls,      "[PATH]",        "list a directory" },
    { "shutdown", cmd_shutdown, "",            "turn the machine off" },
    { "reboot",  cmd_reboot,  "",              "start it again" },
    { "tree",    cmd_tree,    "[PATH]",        "everything below a directory" },
    { "cd",      cmd_cd,      "[PATH]",        "change directory, no argument goes home" },
    { "pwd",     cmd_pwd,     "",              "where you are" },
    { "cat",     cmd_cat,     "FILE...",       "print a file" },
    { "head",    cmd_head,    "FILE [LINES]",  "the first few lines" },
    { "tail",    cmd_tail,    "FILE [LINES]",  "the last few" },
    { "grep",    cmd_grep,    "TEXT FILE...",  "lines containing something" },
    { "find",    cmd_find,    "NAME [WHERE]",  "files whose name contains it" },
    { "hex",     cmd_hex,     "FILE [BYTES]",  "what a file really holds" },
    { "wc",      cmd_wc,      "FILE",          "lines, words and bytes" },
    { "stat",    cmd_stat,    "PATH",          "size, kind, and where it is kept" },
    { "write",   cmd_write,   "FILE TEXT",     "create or overwrite" },
    { "append",  cmd_append,  "FILE TEXT",     "add a line to the end" },
    { "cp",      cmd_cp,      "SRC DST",       "copy a file" },
    { "mv",      cmd_mv,      "SRC DST",       "move one" },
    { "rm",      cmd_rm,      "FILE...",       "delete" },
    { "mkdir",   cmd_mkdir,   "NAME",          "make a directory" },
    { "rmdir",   cmd_rmdir,   "NAME",          "remove an empty one" },
    { "run",     cmd_run,     "PROGRAM [&]",   "start a program, & to not wait" },
    { "ps",      cmd_ps,      "",              "what is running" },
    { "kill",    cmd_kill,    "PID",           "stop a task" },
    { "sys",     cmd_sys,     "[NAME]",        "the machine, as files" },
    { "watch",   cmd_watch,   "FILE [TIMES]",  "read it again and again" },
    { "time",    cmd_time,    "COMMAND...",    "how long something takes" },
    { "mem",     cmd_mem,     "",              "memory, with a bar" },
    { "uptime",  cmd_uptime,  "",              "how long this has been running" },
    { "net",     cmd_net,     "",              "the address, if there is one" },
    { "resolve", cmd_resolve, "HOST",          "look up a name" },
    { "get",     cmd_get,     "HOST [PATH] [FILE]", "fetch a page, https unless said" },
    { "theme",   cmd_theme,   "[NAME]",        "change the colours" },
    { "history", cmd_history, "",              "what you have typed" },
    { "echo",    cmd_echo,    "TEXT",          "print it back" },
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
    dim("Tab completes. Up and down walk through history.");
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
static void run_line(char *cmdline) {
    char *argv[16];
    int argc = split(cmdline, argv, 16);
    if (!argc) return;

    const command *c = command_named(argv[0]);
    if (c) { c->fn(argc, argv); return; }

    char path[VFS_PATH];
    if (find_program(argv[0], path, sizeof(path))) {
        bool bg = argc > 1 && argv[argc - 1][0] == '&';

        /* Anything after the name is handed to the program as the one thing
           it was started on, which for every program here is a file. */
        const char *arg = 0;
        if (argc > 1 && argv[1][0] != '&') arg = argv[1];
        start_program_on(path, arg, bg);
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
    for (int i = 0; i < len && n < cols - 2; i++) input[n++] = with[i];
    in_pos = n;
    for (int i = 0; tail[i] && n < cols - 2; i++) input[n++] = tail[i];
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
        if (n_cand == 1 && cand[0][shared - 1] != '/' && in_len < cols - 2) {
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

    in_len = in_pos = 0;
    input[0] = 0;
    hist_at = 0;
    view = 0;

    run_line(copy);
}

static void insert_char(char c) {
    if (in_len >= cols - 2) return;
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

static void on_key(u32 key) {
    /* Control chords first: they are not text and must not be typed. */
    if (KEY_CTRL(key)) {
        int c = key_ctrl_letter(key);
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
            run_line(line);
            return;
        }
    }

    if (key >= 32 && key <= 126) insert_char((char)key);
}

/* --- the loop ----------------------------------------------------------- */

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

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

    draw_all();
    win_commit(win);

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
            if (ev.type == WIN_EV_KEY) { on_key(ev.key); changed = 1; }
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

        blink++;
        if (blink % 12 == 0) changed = 1;      /* the cursor needs a repaint */

        if (changed) {
            draw_all();
            win_commit(win);
        }
        sleep_ms(20);
    }
}
