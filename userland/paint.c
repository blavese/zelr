/* paint, running in ring 3.
 *
 * This program has no access to the framebuffer, the window manager, or any
 * kernel memory. It asks for a window, gets back a block of pixels mapped
 * into its own address space, and draws into that. Every other interaction
 * with the outside world is a system call.
 *
 * Drawing primitives are here rather than shared with the kernel, because
 * nothing is shared with the kernel.
 *
 * A picture is kept as a PNG: ctrl+S writes what is under the toolbar to
 * the file this was started on, or /home/picture.png (pngw.h), and started
 * on a PNG ("paint FILE.png") it opens with that picture on the paper
 * (png.h, the browser's reader). ctrl+Z takes back the last stroke or
 * clear. What it did is said on the console ("paint: saved PATH, WxH").
 *
 * Tools, by key or from the icons beside the brush sizes: B the brush, L a
 * line, R a rectangle, E an ellipse (each drawn from where the button went
 * down to where it is, shown as it is dragged), F a fill of the colour
 * clicked on, as far as it reaches, and T words, typed where the paper is
 * clicked in the desktop's own typeface, the brush's size choosing which.
 * While words are being typed every key is theirs: Return or a click
 * elsewhere ends them ("paint: words TEXT, WxH at X,Y").
 *
 * ctrl+O opens another PNG, chosen from a list (ui.h's chooser); what is
 * drawn here is named pr_ and flood so as not to be the toolkit's. */
#include "zelr.h"
#include "ui.h"
#include "alloc.h"
#include "png.h"
#include "pngw.h"
#include "face.h"

#define TOOLBAR_H  58
#define SWATCH     24
#define SWATCH_GAP 4

static const u32 PALETTE[16] = {
    RGB(0x18, 0x1D, 0x23), RGB(0xF2, 0xF5, 0xF7), RGB(0xC7, 0x4A, 0x3C),
    RGB(0xE0, 0x8A, 0x3C), RGB(0xE8, 0xC8, 0x62), RGB(0x5E, 0xD1, 0x8A),
    RGB(0x2E, 0x9E, 0x5B), RGB(0x4F, 0xD6, 0xD6), RGB(0x3C, 0x8F, 0xD1),
    RGB(0x2A, 0x54, 0xA8), RGB(0x8E, 0x6B, 0xE0), RGB(0xB0, 0x6A, 0xD6),
    RGB(0xE0, 0x74, 0xB8), RGB(0x8A, 0x5A, 0x3A), RGB(0x7A, 0x86, 0x92),
    RGB(0x3A, 0x44, 0x50),
};

static const int BRUSHES[4] = { 1, 3, 6, 12 };

#define BAR_BG     RGB(0x22, 0x29, 0x31)
#define BAR_EDGE   RGB(0x39, 0x44, 0x4F)
#define PAPER      RGB(0xF2, 0xF5, 0xF7)
#define WHITE      RGB(0xFF, 0xFF, 0xFF)
#define CLEAR_BG   RGB(0x33, 0x3D, 0x48)
#define CLEAR_MARK RGB(0xC7, 0x4A, 0x3C)

static u32 *px;
static int  W, H;

static int color = 2;
static int brush = 1;
static int drawing;
static int last_x, last_y;

enum { TOOL_BRUSH, TOOL_LINE, TOOL_RECT, TOOL_ELLIPSE, TOOL_FILL, TOOL_WORDS, TOOLS };
static const char *const TOOL_NAMES[TOOLS] = { "the brush", "a line", "a rectangle", "an ellipse", "a fill",
                                               "words" };
static int tool;
static int anchor_x, anchor_y;          /* where a shape began */

static char path[256] = "/home/picture.png";
static u32 *undo_px;                    /* the paper as it was before the last stroke or clear */
static int  undo_ok;
/* Changed since it was opened or saved, and whether ctrl+O has said so
   once: a second ctrl+O opens the list anyway, as Notes' does. */
static int  unsaved;
static int  open_armed;

/* --- drawing ------------------------------------------------------------ */

static void pr_rect(int x, int y, int w, int h, u32 c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        u32 *row = px + (u32)(y + j) * W + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void pr_frame(int x, int y, int w, int h, u32 c) {
    pr_rect(x, y, w, 1, c);
    pr_rect(x, y + h - 1, w, 1, c);
    pr_rect(x, y, 1, h, c);
    pr_rect(x + w - 1, y, 1, h, c);
}

/* A filled disc, clipped to the canvas area below the toolbar. */
static void dot(int cx, int cy, int r, u32 c) {
    if (r <= 1) {
        if (cx >= 0 && cy >= TOOLBAR_H && cx < W && cy < H)
            px[(u32)cy * W + cx] = c;
        return;
    }
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < TOOLBAR_H || y >= H) continue;
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= W) continue;
            if (dx * dx + dy * dy > r * r) continue;
            px[(u32)y * W + x] = c;
        }
    }
}

/* Bresenham, so a fast drag leaves a stroke instead of a dotted trail. */
static void pr_line(int x0, int y0, int x1, int y1, int r, u32 c) {
    int dx = x1 - x0, dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        dot(x0, y0, r, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* --- shapes --------------------------------------------------------------- */

static int isqrt(long long v) {
    if (v <= 0) return 0;
    unsigned long long n = (unsigned long long)v, r = 0, bit = 1ull << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (int)r;
}

static void rectangle(int x0, int y0, int x1, int y1, int r, u32 c) {
    pr_line(x0, y0, x1, y0, r, c);
    pr_line(x1, y0, x1, y1, r, c);
    pr_line(x1, y1, x0, y1, r, c);
    pr_line(x0, y1, x0, y0, r, c);
}

/* The ellipse in the box from (x0, y0) to (x1, y1): walked both ways, by x
   and by y, so a steep side has no gaps, and each offset the floor of the
   true one: the root is taken after the scaling. Taken before it (b times
   the root over a), offsets moved by two where the scale is two, and a fill
   inside a pixel-wide ellipse got out through the corners that left. */
static void ellipse(int x0, int y0, int x1, int y1, int r, u32 c) {
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    int a = (x1 - x0) / 2, b = (y1 - y0) / 2, cx = x0 + a, cy = y0 + b;
    if (a < 1 || b < 1) { pr_line(x0, y0, x1, y1, r, c); return; }
    for (int dx = -a; dx <= a; dx++) {
        int dy = isqrt((long long)b * b * (a * a - dx * dx) / ((long long)a * a));
        dot(cx + dx, cy - dy, r, c);
        dot(cx + dx, cy + dy, r, c);
    }
    for (int dy = -b; dy <= b; dy++) {
        int dx = isqrt((long long)a * a * (b * b - dy * dy) / ((long long)b * b));
        dot(cx - dx, cy + dy, r, c);
        dot(cx + dx, cy + dy, r, c);
    }
}

/* Everything of the colour clicked on that touches it, a row at a time: a
   stretch of a row is filled and the rows above and below looked along it. A
   stack of stretches rather than of pixels, kept in the heap and grown. */
static void flood(int x, int y, u32 c) {
    if (y < TOOLBAR_H || y >= H || x < 0 || x >= W) return;
    u32 from = px[(u32)y * W + x];
    if (from == c) return;
    int cap = 1024, n = 0;
    int *st = (int *)malloc((u64)cap * 2 * sizeof(int));
    if (!st) return;
    st[n * 2] = x; st[n * 2 + 1] = y; n++;
    while (n) {
        n--;
        int sx = st[n * 2], sy = st[n * 2 + 1];
        u32 *row = px + (u32)sy * W;
        if (row[sx] != from) continue;
        int l = sx, rr = sx;
        while (l > 0 && row[l - 1] == from) l--;
        while (rr < W - 1 && row[rr + 1] == from) rr++;
        for (int i = l; i <= rr; i++) row[i] = c;
        for (int dy = -1; dy <= 1; dy += 2) {
            int ny = sy + dy;
            if (ny < TOOLBAR_H || ny >= H) continue;
            u32 *nrow = px + (u32)ny * W;
            for (int i = l; i <= rr; i++) {
                if (nrow[i] != from || (i > l && nrow[i - 1] == from)) continue;
                if (n == cap) {
                    int *more = (int *)malloc((u64)cap * 4 * sizeof(int));
                    if (!more) { free(st); return; }
                    memcpy(more, st, (u64)cap * 2 * sizeof(int));
                    free(st);
                    st = more;
                    cap *= 2;
                }
                st[n * 2] = i; st[n * 2 + 1] = ny; n++;
            }
        }
    }
    free(st);
}

/* The paper as it was when the shape began, and the shape drawn on it to
   where the pointer is: dragged, it follows. */
static void shape_to(int x, int y) {
    if (!undo_ok) return;
    memcpy(px + (u32)TOOLBAR_H * W, undo_px, W * (H - TOOLBAR_H) * 4);
    int r = BRUSHES[brush];
    u32 c = PALETTE[color];
    if (tool == TOOL_LINE) pr_line(anchor_x, anchor_y, x, y, r, c);
    else if (tool == TOOL_RECT) rectangle(anchor_x, anchor_y, x, y, r, c);
    else if (tool == TOOL_ELLIPSE) ellipse(anchor_x, anchor_y, x, y, r, c);
}

/* --- the toolbar -------------------------------------------------------- */

static int tool_x(int i) { return 160 + i * 30; }

static void draw_tools(void) {
    for (int i = 0; i < TOOLS; i++) {
        int x = tool_x(i), y = 32;
        u32 ink = i == tool ? WHITE : RGB(0x8A, 0x9B, 0xA6);
        pr_rect(x, y, 24, 22, i == tool ? RGB(0x39, 0x44, 0x4F) : BAR_BG);
        if (i == TOOL_BRUSH) {
            for (int dy = -4; dy <= 4; dy++)
                for (int dx = -4; dx <= 4; dx++)
                    if (dx * dx + dy * dy <= 16) px[(u32)(y + 11 + dy) * W + x + 12 + dx] = ink;
        } else if (i == TOOL_LINE) {
            for (int k = 0; k < 14; k++) { px[(u32)(y + 18 - k) * W + x + 5 + k] = ink; px[(u32)(y + 18 - k) * W + x + 6 + k] = ink; }
        } else if (i == TOOL_RECT) {
            pr_frame(x + 5, y + 5, 14, 12, ink);
        } else if (i == TOOL_WORDS) {
            pr_rect(x + 6, y + 4, 12, 2, ink);
            pr_rect(x + 11, y + 4, 2, 14, ink);
        } else if (i == TOOL_ELLIPSE) {
            for (int dx = -7; dx <= 7; dx++) {
                int dy = isqrt(25 * (49 - dx * dx) / 49);
                px[(u32)(y + 11 - dy) * W + x + 12 + dx] = ink;
                px[(u32)(y + 11 + dy) * W + x + 12 + dx] = ink;
            }
        } else {
            pr_rect(x + 6, y + 6, 12, 10, ink);
        }
    }
}

static void choose_tool(int t) {
    tool = t;
    puts("paint: ");
    puts(TOOL_NAMES[t]);
    puts("\n");
}


static int clear_x(void) { return W - 44; }

static void draw_toolbar(void) {
    pr_rect(0, 0, W, TOOLBAR_H, BAR_BG);
    pr_rect(0, TOOLBAR_H - 1, W, 1, BAR_EDGE);

    for (int i = 0; i < 16; i++) {
        int x = 8 + i * (SWATCH + SWATCH_GAP);
        if (x + SWATCH > clear_x() - 8) break;
        pr_rect(x, 6, SWATCH, SWATCH, PALETTE[i]);
        if (i == color) {
            pr_frame(x - 2, 4, SWATCH + 4, SWATCH + 4, WHITE);
            pr_frame(x - 3, 3, SWATCH + 6, SWATCH + 6, RGB(0x11, 0x16, 0x1C));
        }
    }

    /* Brush sizes, drawn at the size they actually paint. */
    for (int i = 0; i < 4; i++) {
        int cx = 20 + i * 34, cy = 44;
        int r = BRUSHES[i];
        u32 c = (i == brush) ? WHITE : RGB(0x8A, 0x9B, 0xA6);
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx * dx + dy * dy <= r * r) {
                    int x = cx + dx, y = cy + dy;
                    if (x >= 0 && x < W && y >= 0 && y < TOOLBAR_H - 1)
                        px[(u32)y * W + x] = c;
                }
        if (i == brush) pr_frame(cx - 15, cy - 13, 30, 26, RGB(0x4F, 0x5C, 0x68));
    }
    draw_tools();

    /* Clear, drawn as a cross rather than as a word: there is no font on
       this side of the ring boundary. */
    int bx = clear_x();
    pr_rect(bx, 8, 34, 34, CLEAR_BG);
    pr_frame(bx, 8, 34, 34, BAR_EDGE);
    for (int i = 8; i < 26; i++) {
        for (int t = 0; t < 3; t++) {
            int y1 = 8 + i, x1 = bx + i + t;
            if (x1 < bx + 34 && y1 < 42) px[(u32)y1 * W + x1] = CLEAR_MARK;
            int x2 = bx + 33 - i + t;
            if (x2 > bx && x2 < bx + 34) px[(u32)y1 * W + x2] = CLEAR_MARK;
        }
    }
}

static void clear_canvas(void) {
    pr_rect(0, TOOLBAR_H, W, H - TOOLBAR_H, PAPER);
}

/* --- keeping it ------------------------------------------------------------ */

static void say(const char *what, const char *more) {
    puts("paint: ");
    puts(what);
    if (more) puts(more);
    puts("\n");
}

static void say_size(const char *what) {
    char line[320];
    int n = 0;
    for (const char *p = what; *p; p++) line[n++] = *p;
    for (const char *p = path; *p && n < 280; p++) line[n++] = *p;
    line[n++] = ',';
    line[n++] = ' ';
    n += utoa((u32)W, line + n);
    line[n++] = 'x';
    n += utoa((u32)(H - TOOLBAR_H), line + n);
    line[n] = 0;
    say(line, 0);
}

/* The paper, before a change that ctrl+Z can take back. */
static void keep_for_undo(void) {
    unsaved = 1;                        /* every change to the paper starts here */
    if (!undo_px) undo_px = (u32 *)malloc((u64)W * (u64)(H - TOOLBAR_H) * 4);
    if (!undo_px) return;
    memcpy(undo_px, px + (u32)TOOLBAR_H * W, W * (H - TOOLBAR_H) * 4);
    undo_ok = 1;
}

static void undo(void) {
    if (!undo_ok) { say("nothing to undo", 0); return; }
    memcpy(px + (u32)TOOLBAR_H * W, undo_px, W * (H - TOOLBAR_H) * 4);
    undo_ok = 0;
    say("undone", 0);
}

/* --- words ---------------------------------------------------------------
 *
 * Drawn as the desktop draws its own (draw.h's face_draw: the same faces, the
 * same baseline four fifths of the way down, edges blended by coverage), so
 * a picture's words read like the window's. The brush's size picks the face:
 * 13 pixels, 15 bold, 20, and 20 drawn at twice its size. While they are
 * typed the paper they cover is the one kept for ctrl+Z, put back and the
 * words drawn again at each key, with a mark where the next letter goes. */
static const int WORD_FACE[4] = { 0, 3, 2, 2 };
static const int WORD_SCALE[4] = { 1, 1, 1, 2 };

static char words[120];
static int nwords, words_on, words_x, words_y;

static u32 blend(u32 under, u32 over, int a) {
    int ur = (under >> 16) & 0xFF, ug = (under >> 8) & 0xFF, ub = under & 0xFF;
    int orr = (over >> 16) & 0xFF, og = (over >> 8) & 0xFF, ob = over & 0xFF;
    return RGB(ur + (orr - ur) * a / 255, ug + (og - ug) * a / 255, ub + (ob - ub) * a / 255);
}

static int words_width(void) {
    const face_t *f = &face_faces[WORD_FACE[brush]];
    int w = 0;
    for (int i = 0; i < nwords; i++) {
        unsigned char ch = (unsigned char)words[i];
        if (ch < FACE_FIRST || ch > FACE_LAST) ch = ' ';
        w += f->glyphs[ch - FACE_FIRST].advance;
    }
    return w * WORD_SCALE[brush];
}

static void words_draw(int mark) {
    const face_t *f = &face_faces[WORD_FACE[brush]];
    int s = WORD_SCALE[brush];
    int base = words_y + (f->size * 4) / 5 * s;
    int x = words_x;
    u32 c = PALETTE[color];
    for (int i = 0; i < nwords; i++) {
        unsigned char ch = (unsigned char)words[i];
        if (ch < FACE_FIRST || ch > FACE_LAST) ch = ' ';
        const face_glyph *g = &f->glyphs[ch - FACE_FIRST];
        const unsigned char *gp = f->pixels + g->at;
        for (int gy = 0; gy < g->h; gy++)
            for (int gx = 0; gx < g->w; gx++) {
                int a = gp[gy * g->w + gx];
                if (!a) continue;
                for (int sy = 0; sy < s; sy++)
                    for (int sx = 0; sx < s; sx++) {
                        int py = base + (gy - g->top) * s + sy, qx = x + (g->left + gx) * s + sx;
                        if (py < TOOLBAR_H || py >= H || qx < 0 || qx >= W) continue;
                        u32 *slot = &px[(u32)py * W + qx];
                        *slot = a == 255 ? c : blend(*slot, c, a);
                    }
            }
        x += g->advance * s;
    }
    if (mark) {
        int mh = f->size * s;
        if (words_y + mh > H) mh = H - words_y;
        if (x < W && mh > 0) pr_rect(x, words_y, s, mh, c);
    }
}

/* The paper as it was before the words, and the words on it again. */
static void words_show(void) {
    if (undo_ok) memcpy(px + (u32)TOOLBAR_H * W, undo_px, W * (H - TOOLBAR_H) * 4);
    words_draw(1);
}

static void words_begin(int x, int y) {
    keep_for_undo();
    words_on = 1;
    nwords = 0;
    words_x = x;
    words_y = y;
    words_show();
}

static void words_end(void) {
    if (!words_on) return;
    words_on = 0;
    if (undo_ok) memcpy(px + (u32)TOOLBAR_H * W, undo_px, W * (H - TOOLBAR_H) * 4);
    words_draw(0);
    if (!nwords) return;
    char line[220];
    int n = 0;
    for (const char *p = "words "; *p; p++) line[n++] = *p;
    for (int i = 0; i < nwords; i++) line[n++] = words[i];
    line[n++] = ',';
    line[n++] = ' ';
    n += utoa((u32)words_width(), line + n);
    line[n++] = 'x';
    n += utoa((u32)(face_faces[WORD_FACE[brush]].size * WORD_SCALE[brush]), line + n);
    for (const char *p = " at "; *p; p++) line[n++] = *p;
    n += utoa((u32)words_x, line + n);
    line[n++] = ',';
    n += utoa((u32)(words_y - TOOLBAR_H), line + n);
    line[n] = 0;
    say(line, 0);
}

/* --- another picture ------------------------------------------------------
 *
 * The list is drawn where the paper is, and the paper is the picture, so
 * the paper is kept aside while the list is open and put back when it is
 * let go; a picture chosen is opened on clean paper, and becomes what
 * ctrl+S saves to. */
static ui_chooser chooser;
static ui_input pick_in;
static u32 *behind;

static void open_picture(void);

static void choose_open(void) {
    words_end();
    if (!behind) behind = (u32 *)malloc((u64)W * (u64)(H - TOOLBAR_H) * 4);
    if (!behind) { say("no memory to keep the paper while choosing", 0); return; }
    memcpy(behind, px + (u32)TOOLBAR_H * W, W * (H - TOOLBAR_H) * 4);
    memset(&pick_in, 0, sizeof(pick_in));
    ui_chooser_open_on(&chooser, path, ".png", "paint");
}

static void choose_done(int picked) {
    memcpy(px + (u32)TOOLBAR_H * W, behind, W * (H - TOOLBAR_H) * 4);
    if (picked != 1) return;
    strncpy(path, chooser.picked, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    clear_canvas();
    open_picture();
    undo_ok = 0;                        /* the last picture's strokes are not this one's */
    unsaved = 0;
}

static void choose_draw(void) {
    surface s = { px, W, H };
    ui_theme t = ui_load_theme();
    if (ui_chooser_draw(&s, &pick_in, &t, &chooser, 0, TOOLBAR_H, W, H - TOOLBAR_H) == 1) choose_done(1);
}

static void save(void) {
    int ch = H - TOOLBAR_H;
    u32 size = png_write_size(W, ch);
    u8 *out = (u8 *)malloc(size);
    if (!out) { say("not saved: no memory for it", 0); return; }
    int n = png_write(px + (u32)TOOLBAR_H * W, W, W, ch, out, size);
    int ok = n > 0 && spit(path, out, n) == n;
    free(out);
    if (ok) { say_size("saved "); unsaved = 0; }
    else say("not saved: ", path);
}

/* A PNG on the paper, its top left at the paper's; what does not fit is
   left out, and paper shows where it does not reach. */
static void open_picture(void) {
    zelr_stat st;
    if (stat(path, &st) < 0 || st.is_dir) { say("a new picture, to be saved as ", path); return; }
    u8 *data = (u8 *)malloc(st.size + 1);
    int n = data ? slurp(path, (char *)data, (int)st.size + 1) : -1;
    picture pic;
    int r = n > 0 ? png_decode(data, n, &pic, PAPER) : -1;
    if (data) free(data);
    if (r != PNG_OK) { say("could not open ", path); return; }
    for (int y = 0; y < pic.h && TOOLBAR_H + y < H; y++)
        for (int x = 0; x < pic.w && x < W; x++) {
            const u8 *c = pic.rgb + ((u32)y * pic.w + x) * 3;
            px[(u32)(TOOLBAR_H + y) * W + x] = RGB(c[0], c[1], c[2]);
        }
    free(pic.rgb);
    char line[300];
    int k = 0;
    for (const char *p = "opened "; *p; p++) line[k++] = *p;
    for (const char *p = path; *p && k < 260; p++) line[k++] = *p;
    line[k++] = ',';
    line[k++] = ' ';
    k += utoa((u32)pic.w, line + k);
    line[k++] = 'x';
    k += utoa((u32)pic.h, line + k);
    line[k] = 0;
    say(line, 0);
}

/* --- input -------------------------------------------------------------- */

static void on_mouse(int x, int y, u32 buttons) {
    int pressed = (buttons & WIN_BTN_DOWN) != 0;
    int left = (buttons & WIN_BTN_LEFT) != 0;

    if (y < TOOLBAR_H) {
        if (!pressed) return;

        for (int i = 0; i < 16; i++) {
            int sx = 8 + i * (SWATCH + SWATCH_GAP);
            if (sx + SWATCH > clear_x() - 8) break;
            if (x >= sx && x < sx + SWATCH && y >= 6 && y < 6 + SWATCH) {
                color = i;
                draw_toolbar();
                if (words_on) words_show();
                return;
            }
        }
        for (int i = 0; i < 4; i++) {
            int cx = 20 + i * 34;
            if (x >= cx - 15 && x <= cx + 15 && y >= 31) {
                brush = i;
                draw_toolbar();
                if (words_on) words_show();
                return;
            }
        }
        for (int i = 0; i < TOOLS; i++) {
            if (x >= tool_x(i) && x < tool_x(i) + 24 && y >= 32 && y < 54) {
                words_end();
                choose_tool(i);
                draw_toolbar();
                return;
            }
        }
        if (x >= clear_x() && x < clear_x() + 34 && y >= 8 && y < 42) {
            words_end();
            keep_for_undo();
            clear_canvas();
            return;
        }
        return;
    }

    if (!left) { drawing = 0; return; }

    if (tool == TOOL_WORDS) {
        if (pressed) {
            words_end();
            words_begin(x, y);
        }
        return;
    }
    if (tool == TOOL_FILL) {
        if (pressed) { keep_for_undo(); flood(x, y, PALETTE[color]); }
        return;
    }
    if (tool != TOOL_BRUSH) {
        if (!drawing || pressed) {
            keep_for_undo();            /* the paper the shape is drawn over */
            anchor_x = x;
            anchor_y = y;
            drawing = 1;
        }
        shape_to(x, y);
        return;
    }

    int r = BRUSHES[brush];
    if (!drawing || pressed) {
        keep_for_undo();                /* a stroke begins */
        dot(x, y, r, PALETTE[color]);
        drawing = 1;
    } else {
        pr_line(last_x, last_y, x, y, r, PALETTE[color]);
    }
    last_x = x;
    last_y = y;
}

static void on_key(u32 key) {
    /* ctrl+O, also as the control character a serial line sends. */
    int open_key = (KEY_CTRL(key) && key_ctrl_letter(key) == 'o') || key == 15;
    if (open_key) {
        if (unsaved && !open_armed) {
            open_armed = 1;
            say("not saved: ctrl+S keeps it, ctrl+O again opens another anyway", 0);
        } else {
            open_armed = 0;
            choose_open();
        }
        return;
    }
    open_armed = 0;
    if (KEY_CTRL(key)) {
        int l = key_ctrl_letter(key);
        if (l == 's') { words_end(); save(); }
        else if (l == 'z') { words_on = 0; undo(); }
        return;
    }
    /* Words being typed take every key: the letters that choose tools are
       letters in them. */
    if (words_on) {
        if (key == '\n') words_end();
        else if (key == '\b') { if (nwords) nwords--; words_show(); }
        else if (key >= 32 && key < 127 && nwords < (int)sizeof(words) - 1) {
            words[nwords++] = (char)key;
            words_show();
        }
        return;
    }
    if (key >= '1' && key <= '4') { brush = (int)key - '1'; draw_toolbar(); }
    else if (key == 'c' || key == 'C') { keep_for_undo(); clear_canvas(); }
    else if (key == '[') { color = (color + 15) % 16; draw_toolbar(); }
    else if (key == ']') { color = (color + 1) % 16; draw_toolbar(); }
    else if (key == 'b' || key == 'B') { choose_tool(TOOL_BRUSH); draw_toolbar(); }
    else if (key == 'l' || key == 'L') { choose_tool(TOOL_LINE); draw_toolbar(); }
    else if (key == 'r' || key == 'R') { choose_tool(TOOL_RECT); draw_toolbar(); }
    else if (key == 'e' || key == 'E') { choose_tool(TOOL_ELLIPSE); draw_toolbar(); }
    else if (key == 'f' || key == 'F') { choose_tool(TOOL_FILL); draw_toolbar(); }
    else if (key == 't' || key == 'T') { choose_tool(TOOL_WORDS); draw_toolbar(); }
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0]) {
        strncpy(path, argv[1], sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    }
    int win = win_create("paint", 640, 420);
    if (win < 0) { puts("paint: no window\n"); return 1; }

    px = win_surface(win);
    if (!px) { puts("paint: no surface\n"); return 1; }

    W = win_width(win);
    H = win_height(win);
    if (W <= 0 || H <= 0) return 1;

    clear_canvas();
    if (argc > 1 && argv[1][0]) open_picture();
    draw_toolbar();
    win_commit(win);

    for (;;) {
        win_event ev;
        int changed = 0;

        if (chooser.open) ui_begin(&pick_in);
        while (win_poll(win, &ev) == 1) {
            if (ev.type == WIN_EV_CLOSE) {
                win_close(win);
                return 0;
            }
            if (chooser.open) {
                if (ev.type == WIN_EV_KEY) {
                    int r = ui_chooser_key(&chooser, ev.key);
                    if (r) choose_done(r);
                } else {
                    ui_feed(&pick_in, &ev);
                }
                changed = 1;
                continue;
            }
            if (ev.type == WIN_EV_MOUSE) {
                if (ev.buttons & WIN_BTN_DOWN) open_armed = 0;
                on_mouse(ev.x, ev.y, ev.buttons);
                changed = 1;
            }
            if (ev.type == WIN_EV_KEY)   { on_key(ev.key); changed = 1; }
        }

        if (chooser.open && changed) choose_draw();
        if (changed) win_commit(win);

        /* Nothing here changes by itself, so asleep until the pointer or a
           key: it looked a hundred times a second. The window manager runs
           while this one waits, as it did while it slept. */
        win_wait(win, 1000);
    }
}
