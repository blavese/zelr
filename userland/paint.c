/* paint, running in ring 3.
 *
 * This program has no access to the framebuffer, the window manager, or any
 * kernel memory. It asks for a window, gets back a block of pixels mapped
 * into its own address space, and draws into that. Every other interaction
 * with the outside world is a system call.
 *
 * Drawing primitives are here rather than shared with the kernel, because
 * nothing is shared with the kernel. */
#include "zelr.h"

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

/* --- drawing ------------------------------------------------------------ */

static void rect(int x, int y, int w, int h, u32 c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        u32 *row = px + (u32)(y + j) * W + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void frame(int x, int y, int w, int h, u32 c) {
    rect(x, y, w, 1, c);
    rect(x, y + h - 1, w, 1, c);
    rect(x, y, 1, h, c);
    rect(x + w - 1, y, 1, h, c);
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
static void line(int x0, int y0, int x1, int y1, int r, u32 c) {
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

/* --- the toolbar -------------------------------------------------------- */

static int clear_x(void) { return W - 44; }

static void draw_toolbar(void) {
    rect(0, 0, W, TOOLBAR_H, BAR_BG);
    rect(0, TOOLBAR_H - 1, W, 1, BAR_EDGE);

    for (int i = 0; i < 16; i++) {
        int x = 8 + i * (SWATCH + SWATCH_GAP);
        if (x + SWATCH > clear_x() - 8) break;
        rect(x, 6, SWATCH, SWATCH, PALETTE[i]);
        if (i == color) {
            frame(x - 2, 4, SWATCH + 4, SWATCH + 4, WHITE);
            frame(x - 3, 3, SWATCH + 6, SWATCH + 6, RGB(0x11, 0x16, 0x1C));
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
        if (i == brush) frame(cx - 15, cy - 13, 30, 26, RGB(0x4F, 0x5C, 0x68));
    }

    /* Clear, drawn as a cross rather than as a word: there is no font on
       this side of the ring boundary. */
    int bx = clear_x();
    rect(bx, 8, 34, 34, CLEAR_BG);
    frame(bx, 8, 34, 34, BAR_EDGE);
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
    rect(0, TOOLBAR_H, W, H - TOOLBAR_H, PAPER);
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
                return;
            }
        }
        for (int i = 0; i < 4; i++) {
            int cx = 20 + i * 34;
            if (x >= cx - 15 && x <= cx + 15 && y >= 31) {
                brush = i;
                draw_toolbar();
                return;
            }
        }
        if (x >= clear_x() && x < clear_x() + 34 && y >= 8 && y < 42) {
            clear_canvas();
            return;
        }
        return;
    }

    if (!left) { drawing = 0; return; }

    int r = BRUSHES[brush];
    if (!drawing || pressed) {
        dot(x, y, r, PALETTE[color]);
        drawing = 1;
    } else {
        line(last_x, last_y, x, y, r, PALETTE[color]);
    }
    last_x = x;
    last_y = y;
}

static void on_key(u32 key) {
    if (key >= '1' && key <= '4') { brush = (int)key - '1'; draw_toolbar(); }
    else if (key == 'c' || key == 'C') clear_canvas();
    else if (key == '[') { color = (color + 15) % 16; draw_toolbar(); }
    else if (key == ']') { color = (color + 1) % 16; draw_toolbar(); }
}

int main(void);

/* The kernel enters here with a fresh stack and nothing else set up. */
__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

int main(void) {
    int win = win_create("paint", 640, 420);
    if (win < 0) { puts("paint: no window\n"); return 1; }

    px = win_surface(win);
    if (!px) { puts("paint: no surface\n"); return 1; }

    W = win_width(win);
    H = win_height(win);
    if (W <= 0 || H <= 0) return 1;

    clear_canvas();
    draw_toolbar();
    win_commit(win);

    for (;;) {
        win_event ev;
        int changed = 0;

        while (win_poll(win, &ev) == 1) {
            if (ev.type == WIN_EV_CLOSE) {
                win_close(win);
                return 0;
            }
            if (ev.type == WIN_EV_MOUSE) { on_mouse(ev.x, ev.y, ev.buttons); changed = 1; }
            if (ev.type == WIN_EV_KEY)   { on_key(ev.key); changed = 1; }
        }

        if (changed) win_commit(win);
        sleep_ms(10);      /* the window manager runs while this one waits */
    }
}
