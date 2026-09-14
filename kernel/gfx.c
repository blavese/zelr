/* Drawing helpers that sit on top of the framebuffer: text, and blitting
   between an off-screen surface and the screen. */
#include "gfx.h"
#include "fb.h"
#include "font.h"
#include "string.h"

void gfx_char(int x, int y, char c, u32 fg) {
    if (c < FONT_FIRST || c > FONT_LAST) return;
    const u8 *g = font8x16[(u8)c - FONT_FIRST];
    for (int row = 0; row < FONT_H; row++) {
        u8 bits = g[row];
        for (int col = 0; col < FONT_W; col++)
            if (bits & (0x80 >> col)) fb_put((u32)(x + col), (u32)(y + row), fg);
    }
}

void gfx_text(int x, int y, const char *s, u32 fg) {
    for (int i = 0; s[i]; i++) gfx_char(x + i * FONT_W, y, s[i], fg);
}

void gfx_text_bg(int x, int y, const char *s, u32 fg, u32 bg) {
    int w = (int)strlen(s) * FONT_W;
    fb_rect((u32)x, (u32)y, (u32)w, FONT_H, bg);
    gfx_text(x, y, s, fg);
}

int gfx_text_width(const char *s) { return (int)strlen(s) * FONT_W; }

/* --- off-screen surfaces ------------------------------------------------ */

void surf_clear(u32 *px, int w, int h, u32 rgb) {
    for (int i = 0; i < w * h; i++) px[i] = rgb;
}

void surf_rect(u32 *px, int w, int h, int x, int y, int rw, int rh, u32 rgb) {
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;
    if (rw <= 0 || rh <= 0) return;
    for (int j = 0; j < rh; j++) {
        u32 *row = px + (y + j) * w + x;
        for (int i = 0; i < rw; i++) row[i] = rgb;
    }
}

void surf_frame(u32 *px, int w, int h, int x, int y, int rw, int rh, u32 rgb) {
    surf_rect(px, w, h, x, y, rw, 1, rgb);
    surf_rect(px, w, h, x, y + rh - 1, rw, 1, rgb);
    surf_rect(px, w, h, x, y, 1, rh, rgb);
    surf_rect(px, w, h, x + rw - 1, y, 1, rh, rgb);
}

/* A filled circle, which is what a paint brush actually needs. */
void surf_disc(u32 *px, int w, int h, int cx, int cy, int r, u32 rgb) {
    if (r < 1) r = 1;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            px[y * w + x] = rgb;
        }
    }
}

/* Bresenham, so a fast drag does not leave gaps between sampled positions. */
void surf_line(u32 *px, int w, int h, int x0, int y0, int x1, int y1, int r, u32 rgb) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    int err = adx - ady;

    for (;;) {
        surf_disc(px, w, h, x0, y0, r, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -ady) { err -= ady; x0 += sx; }
        if (e2 < adx)  { err += adx; y0 += sy; }
    }
}

void surf_char(u32 *px, int w, int h, int x, int y, char c, u32 fg) {
    if (c < FONT_FIRST || c > FONT_LAST) return;
    const u8 *g = font8x16[(u8)c - FONT_FIRST];
    for (int row = 0; row < FONT_H; row++) {
        u8 bits = g[row];
        for (int col = 0; col < FONT_W; col++) {
            if (!(bits & (0x80 >> col))) continue;
            int px_x = x + col, px_y = y + row;
            if (px_x < 0 || px_y < 0 || px_x >= w || px_y >= h) continue;
            px[px_y * w + px_x] = fg;
        }
    }
}

void surf_text(u32 *px, int w, int h, int x, int y, const char *s, u32 fg) {
    for (int i = 0; s[i]; i++) surf_char(px, w, h, x + i * FONT_W, y, s[i], fg);
}

void surf_blit(const u32 *px, int w, int h, int dx, int dy) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            fb_put((u32)(dx + i), (u32)(dy + j), px[j * w + i]);
}

/* --- chrome --------------------------------------------------------------

   Rounded corners and shadows, drawn straight to the screen. A real
   rasteriser would antialias the curve; at these radii a stack of
   shortening rows is indistinguishable and costs one comparison per row. */

u32 gfx_mix(u32 under, u32 over, int alpha) {
    int ur = (int)((under >> 16) & 0xFF), ug = (int)((under >> 8) & 0xFF), ub = (int)(under & 0xFF);
    int orr = (int)((over >> 16) & 0xFF), og = (int)((over >> 8) & 0xFF), ob = (int)(over & 0xFF);
    int r = ur + (orr - ur) * alpha / 255;
    int g = ug + (og - ug) * alpha / 255;
    int b = ub + (ob - ub) * alpha / 255;
    return RGB(r, g, b);
}

/* How far in from the edge row `i` of a corner of radius `r` starts. */
static int corner_inset(int r, int i) {
    int dy = r - i;
    int dx = r;
    while (dx > 0 && dx * dx + dy * dy > r * r) dx--;
    return r - dx;
}

void fb_round_rect(int x, int y, int w, int h, int r, u32 rgb) {
    if (w <= 0 || h <= 0) return;
    if (r <= 0) { fb_rect((u32)x, (u32)y, (u32)w, (u32)h, rgb); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    fb_rect((u32)x, (u32)(y + r), (u32)w, (u32)(h - r * 2), rgb);
    for (int i = 0; i < r; i++) {
        int inset = corner_inset(r, i);
        fb_rect((u32)(x + inset), (u32)(y + i), (u32)(w - inset * 2), 1, rgb);
        fb_rect((u32)(x + inset), (u32)(y + h - 1 - i), (u32)(w - inset * 2), 1, rgb);
    }
}

void fb_round_frame(int x, int y, int w, int h, int r, u32 rgb) {
    if (w <= 0 || h <= 0) return;
    if (r <= 0) { fb_frame((u32)x, (u32)y, (u32)w, (u32)h, rgb); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    fb_rect((u32)x, (u32)(y + r), 1, (u32)(h - r * 2), rgb);
    fb_rect((u32)(x + w - 1), (u32)(y + r), 1, (u32)(h - r * 2), rgb);
    for (int i = 0; i < r; i++) {
        int inset = corner_inset(r, i);
        fb_put((u32)(x + inset), (u32)(y + i), rgb);
        fb_put((u32)(x + w - 1 - inset), (u32)(y + i), rgb);
        fb_put((u32)(x + inset), (u32)(y + h - 1 - i), rgb);
        fb_put((u32)(x + w - 1 - inset), (u32)(y + h - 1 - i), rgb);
    }
    fb_rect((u32)(x + r), (u32)y, (u32)(w - r * 2), 1, rgb);
    fb_rect((u32)(x + r), (u32)(y + h - 1), (u32)(w - r * 2), 1, rgb);
}

/* A soft edge under a window, drawn by darkening what is already there in
   rings that fade outward. Reading the framebuffer back is what makes this
   work over any wallpaper without knowing what it is. */
void fb_shadow(int x, int y, int w, int h, int r, int spread) {
    for (int s = spread; s >= 1; s--) {
        int alpha = 70 / (s + 1);
        int rx = x - s, ry = y - s + 2, rw = w + s * 2, rh = h + s * 2;
        int rr = r + s;

        for (int j = 0; j < rh; j++) {
            int py = ry + j;
            if (py < 0 || py >= (int)fb_height()) continue;

            int inset = 0;
            if (j < rr) inset = corner_inset(rr, j);
            else if (j >= rh - rr) inset = corner_inset(rr, rh - 1 - j);

            for (int i = inset; i < rw - inset; i++) {
                /* Only the ring matters; the inside is covered anyway. */
                bool edge = (i < inset + 2) || (i >= rw - inset - 2) ||
                            (j < 2) || (j >= rh - 2);
                if (!edge) continue;
                int px = rx + i;
                if (px < 0 || px >= (int)fb_width()) continue;
                fb_put((u32)px, (u32)py, gfx_mix(fb_get((u32)px, (u32)py), 0, alpha));
            }
        }
    }
}

void fb_vgradient(int x, int y, int w, int h, u32 top, u32 bottom) {
    if (h <= 0) return;
    for (int j = 0; j < h; j++) {
        u32 c = gfx_mix(top, bottom, j * 255 / (h > 1 ? h - 1 : 1));
        fb_rect((u32)x, (u32)(y + j), (u32)w, 1, c);
    }
}

/* --- the anti-aliased face ----------------------------------------------- */

#include "face.h"

static const face_t *face_for(int which) {
    if (which < 0 || which >= FACE_SIZES) which = 0;
    return &face_faces[which];
}

int face_height(int which) { return face_for(which)->size; }

int face_width(const char *s, int which) {
    const face_t *f = face_for(which);
    int w = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < FACE_FIRST || c > FACE_LAST) c = ' ';
        w += f->glyphs[c - FACE_FIRST].advance;
    }
    return w;
}

/* `y` is the top of the line, not the baseline: every caller here thinks in
   boxes, and making them each subtract an ascent would be one more thing to
   get wrong per call site. */
void face_text(int x, int y, const char *s, u32 fg, int which) {
    const face_t *f = face_for(which);
    int baseline = y + (f->size * 4) / 5;

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < FACE_FIRST || c > FACE_LAST) c = ' ';
        const face_glyph *g = &f->glyphs[c - FACE_FIRST];
        const unsigned char *px = f->pixels + g->at;

        for (int gy = 0; gy < g->h; gy++) {
            int sy = baseline - g->top + gy;
            if (sy < 0 || sy >= (int)fb_height()) continue;
            for (int gx = 0; gx < g->w; gx++) {
                unsigned char a = px[gy * g->w + gx];
                if (!a) continue;
                int sx = x + g->left + gx;
                if (sx < 0 || sx >= (int)fb_width()) continue;
                fb_put((u32)sx, (u32)sy,
                       a == 255 ? fg : gfx_mix(fb_get((u32)sx, (u32)sy), fg, a));
            }
        }
        x += g->advance;
    }
}

void face_surf_text(u32 *dst, int w, int h, int x, int y,
                    const char *s, u32 fg, int which) {
    const face_t *f = face_for(which);
    int baseline = y + (f->size * 4) / 5;

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < FACE_FIRST || c > FACE_LAST) c = ' ';
        const face_glyph *g = &f->glyphs[c - FACE_FIRST];
        const unsigned char *px = f->pixels + g->at;

        for (int gy = 0; gy < g->h; gy++) {
            int sy = baseline - g->top + gy;
            if (sy < 0 || sy >= h) continue;
            for (int gx = 0; gx < g->w; gx++) {
                unsigned char a = px[gy * g->w + gx];
                if (!a) continue;
                int sx = x + g->left + gx;
                if (sx < 0 || sx >= w) continue;
                u32 *slot = &dst[(u32)sy * w + sx];
                *slot = (a == 255) ? fg : gfx_mix(*slot, fg, a);
            }
        }
        x += g->advance;
    }
}
