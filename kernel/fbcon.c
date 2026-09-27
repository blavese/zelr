/* A text console drawn into the framebuffer.
 *
 * Mirrors the VGA text console's interface so the shell does not know or care
 * which one it is talking to. Colours use the same 16 entry palette, so code
 * written for text mode keeps working. */
#include "fbcon.h"
#include "fb.h"
#include "font.h"
#include "string.h"
#include "mouse.h"

static const u32 PALETTE[16] = {
    RGB(0x14, 0x18, 0x1D),   /* black, lifted slightly so it is not a void */
    RGB(0x2A, 0x54, 0xA8),   /* blue */
    RGB(0x2E, 0x9E, 0x5B),   /* green */
    RGB(0x1F, 0x8D, 0x96),   /* cyan */
    RGB(0xB2, 0x3A, 0x30),   /* red */
    RGB(0x8E, 0x3C, 0x9E),   /* magenta */
    RGB(0x9A, 0x6B, 0x2A),   /* brown */
    RGB(0xC2, 0xC8, 0xCC),   /* light grey */
    RGB(0x55, 0x5F, 0x66),   /* dark grey */
    RGB(0x5A, 0x8F, 0xE8), /* light blue */
    RGB(0x5E, 0xD1, 0x8A),   /* light green */
    RGB(0x4F, 0xD6, 0xD6),   /* light cyan */
    RGB(0xE8, 0x6A, 0x5E),   /* light red */
    RGB(0xD1, 0x77, 0xE0),   /* light magenta */
    RGB(0xE8, 0xC8, 0x62),   /* yellow */
    RGB(0xF2, 0xF5, 0xF7),   /* white */
};

static u32 cols, rows;
static u32 cx, cy;
static u8  fg = 7, bg = 0;
static bool cursor_shown;
static u32 dcol0, drow0, dcol1, drow1;       /* cells to send; see touched() */

void fbcon_init(void) {
    cols = fb_width() / FONT_W;
    rows = fb_height() / FONT_H;
    cx = cy = 0;
    fg = 7; bg = 0;
    cursor_shown = false;
    dcol0 = dcol1 = 0;
    fb_clear(PALETTE[bg]);
    fb_flush();
}

u32 fbcon_cols(void) { return cols; }
u32 fbcon_rows(void) { return rows; }

void fbcon_set_color(u8 f, u8 b) { fg = f & 0x0F; bg = b & 0x0F; }

/* A row of the back buffer at a time. It was 128 calls to fb_put a
   character, each checking its pixel was on the screen, which a cell inside
   the grid always is. */
static void draw_cell(u32 col, u32 row, char ch, u8 f, u8 b) {
    u32 px = col * FONT_W, py = row * FONT_H;
    u32 fgc = PALETTE[f], bgc = PALETTE[b];
    const u8 *g = (ch >= FONT_FIRST && ch <= FONT_LAST) ? font8x16[(u8)ch - FONT_FIRST] : 0;

    for (u32 y = 0; y < FONT_H; y++) {
        u32 *r = fb_row((int)(py + y));
        if (!r) continue;
        r += px;
        u8 bits = g ? g[y] : 0;
        for (u32 x = 0; x < FONT_W; x++) r[x] = (bits & (0x80 >> x)) ? fgc : bgc;
    }
}

/* The cells a character touched -- the old cursor, the new one and the
   character itself -- sent once, together, when it is done. Each of them
   was sent on its own, three flushes a character. [dcol0, dcol1) by
   [drow0, drow1), empty when dcol0 is not below dcol1. */
static void touched(u32 col, u32 row) {
    if (dcol0 >= dcol1) { dcol0 = col; drow0 = row; dcol1 = col + 1; drow1 = row + 1; return; }
    if (col < dcol0) dcol0 = col;
    if (row < drow0) drow0 = row;
    if (col + 1 > dcol1) dcol1 = col + 1;
    if (row + 1 > drow1) drow1 = row + 1;
}

static void send_touched(void) {
    if (dcol0 >= dcol1) return;
    fb_flush_rect(dcol0 * FONT_W, drow0 * FONT_H, (dcol1 - dcol0) * FONT_W, (drow1 - drow0) * FONT_H);
    dcol0 = dcol1 = 0;
}

static void hide_cursor(void) {
    if (!cursor_shown) return;
    fb_rect(cx * FONT_W, cy * FONT_H + FONT_H - 2, FONT_W, 2, PALETTE[bg]);
    touched(cx, cy);
    cursor_shown = false;
}

static void show_cursor(void) {
    fb_rect(cx * FONT_W, cy * FONT_H + FONT_H - 2, FONT_W, 2, PALETTE[fg]);
    touched(cx, cy);
    cursor_shown = true;
}

/* A quarter of the screen at a time once the text reaches the bottom.
 *
 * A line at a time moved the whole back buffer up by one line and sent the
 * whole screen to the card, three megabytes each way at 1024x768, for every
 * line printed once the screen was full: a third of the self test's time went
 * on it. Jumping by a quarter does that once in twelve lines, and the lines
 * in between go into the space it left. */
static void scroll(void) {
    u32 by = rows / 4 ? rows / 4 : 1;
    u8 *p = fb_pixels();
    u32 line = FONT_H * fb_pitch();

    memmove(p, p + by * line, (rows - by) * line);
    fb_rect(0, (rows - by) * FONT_H, fb_width(), by * FONT_H, PALETTE[bg]);
    cy = rows - by;
    fb_flush();
    dcol0 = dcol1 = 0;                       /* all of it has been sent */
}

void fbcon_putc(char c) {
    /* The pointer saves the pixels underneath it, so it has to come
       off before anything writes there and go back on afterwards -- when
       this can write there: the row the cursor is on and the ones either
       side (a backspace goes up, a newline down), or everywhere when the
       text is about to scroll. Taking it off and putting it back for every
       character was two flushes and four hundred pixels each, wherever the
       pointer was. */
    bool wraps = c == '\n' || (c == '\t' ? ((cx + 4) & ~3u) >= cols
                                         : c != '\b' && c != '\r' && cx + 1 >= cols);
    u32 from = cy ? cy - 1 : 0;
    bool near = (wraps && cy + 1 >= rows) ||
                mouse_over(0, from * FONT_H, fb_width(), (cy + 2 - from) * FONT_H);
    if (near) mouse_hide();
    hide_cursor();

    switch (c) {
        case '\n': cx = 0; cy++; break;
        case '\r': cx = 0; break;
        case '\t': cx = (cx + 4) & ~3u; break;
        case '\b':
            if (cx > 0) cx--;
            else if (cy > 0) { cy--; cx = cols - 1; }
            draw_cell(cx, cy, ' ', fg, bg);
            touched(cx, cy);
            break;
        default:
            draw_cell(cx, cy, c, fg, bg);
            touched(cx, cy);
            cx++;
    }

    if (cx >= cols) { cx = 0; cy++; }
    if (cy >= rows) scroll();
    show_cursor();
    send_touched();
    mouse_show();          /* back if it came off; nothing if it is still there */
}

void fbcon_clear(void) {
    mouse_hide();
    cursor_shown = false;
    fb_clear(PALETTE[bg]);
    cx = cy = 0;
    fb_flush();
    dcol0 = dcol1 = 0;
    show_cursor();
    send_touched();
    mouse_show();
}
