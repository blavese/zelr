#pragma once
#include "types.h"

/* Straight to the screen. */
void gfx_char(int x, int y, char c, u32 fg);
void gfx_text(int x, int y, const char *s, u32 fg);
void gfx_text_bg(int x, int y, const char *s, u32 fg, u32 bg);
int  gfx_text_width(const char *s);

/* Into an off-screen surface of w by h pixels. Everything clips. */
void surf_clear(u32 *px, int w, int h, u32 rgb);
void surf_rect(u32 *px, int w, int h, int x, int y, int rw, int rh, u32 rgb);
void surf_frame(u32 *px, int w, int h, int x, int y, int rw, int rh, u32 rgb);
void surf_disc(u32 *px, int w, int h, int cx, int cy, int r, u32 rgb);
void surf_line(u32 *px, int w, int h, int x0, int y0, int x1, int y1, int r, u32 rgb);
void surf_char(u32 *px, int w, int h, int x, int y, char c, u32 fg);
void surf_text(u32 *px, int w, int h, int x, int y, const char *s, u32 fg);
void surf_blit(const u32 *px, int w, int h, int dx, int dy);

/* Chrome drawing. These go straight to the screen, because the window
   manager composites there rather than into a surface of its own. */
void fb_round_rect(int x, int y, int w, int h, int r, u32 rgb);
void fb_round_frame(int x, int y, int w, int h, int r, u32 rgb);
void fb_shadow(int x, int y, int w, int h, int r, int spread);
void fb_vgradient(int x, int y, int w, int h, u32 top, u32 bottom);

/* --- surfaces that are material rather than built ------------------------
 *
 * A rounded rectangle with its corners measured rather than stepped, and
 * with an alpha, so a panel can let the wallpaper through it. A soft round
 * light, for a background that has depth in it. And the gloss along the top
 * of a pane, which is the one thing from that era of interfaces that still
 * reads as light. */
void fb_round_rect_aa(int x, int y, int w, int h, int r, u32 rgb, int alpha);
void fb_glow(int cx, int cy, int rx, int ry, u32 rgb, int strength);

/* Darkens toward the corners, which is what stops a gradient reading as a
   gradient. Every photograph has this and no drawing does, which is most of
   why a flat wash looks like a diagram of a background. */
void fb_vignette(int strength);
void fb_sheen(int x, int y, int w, int h, int r, int strength);
void fb_hgradient(int x, int y, int w, int h, u32 left, u32 right);
u32  gfx_mix(u32 under, u32 over, int alpha);

/* --- edges that catch the light -------------------------------------------
 *
 * The whole of the classic look is one idea: a surface is not a colour, it
 * is a plane with a light above and to the left of it. Everything raised
 * has a bright top and left and a dark bottom and right, everything sunk
 * has it the other way round, and a control says what it is by which way
 * its edges go rather than by being tinted.
 *
 * Two pixels rather than one, because one is a line and two is a bevel: the
 * outer pair carry the strong colours and the inner pair the soft ones, and
 * together they read as a chamfer at any size.
 *
 * Colours are passed in rather than read from the theme so that this stays
 * a drawing routine. The four are named going inward from the top left. */
void fb_bevel(int x, int y, int w, int h,
              u32 tl_outer, u32 tl_inner, u32 br_inner, u32 br_outer);

/* One pixel each way, for the places a full bevel is too heavy: a menu
   separator, the well around a text field, the groove between toolbars. */
void fb_bevel_thin(int x, int y, int w, int h, u32 tl, u32 br);

/* --- the anti-aliased face -----------------------------------------------
 *
 * The 8x16 bitmap above is still what the terminal draws with, because a
 * terminal is a grid and needs every character the same width. Everything
 * else -- window titles, the taskbar, menus -- uses this, which is an
 * outline face rasterised at build time into coverage bytes. See
 * tools/genface.py.
 *
 * A glyph is blended rather than stamped: each pixel says how much of it the
 * letter covers, and that fraction of the text colour goes over whatever was
 * already there. That is the whole difference between text that looks drawn
 * and text that looks printed. */

/* The scale, in the order genface.py emits it. Names rather than numbers
   because the order changes whenever a size is added, and a call site that
   said 1 would then quietly draw at a different size. */
#define FACE_SMALL      0   /* 13px, for things that sit under something */
#define FACE_BODY       1   /* 15px, the default for chrome */
#define FACE_HEAD       2   /* 20px */
#define FACE_TITLE      3   /* 26px */
#define FACE_BODY_BOLD  4   /* 15px, for emphasis at body size */
#define FACE_HEAD_BOLD  5   /* 20px, for a heading that has to lead */

void face_text(int x, int y, const char *s, u32 fg, int which);
int  face_width(const char *s, int which);
int  face_height(int which);

/* Into an off-screen surface, for anything composited rather than drawn
   straight to the screen. */
void face_surf_text(u32 *px, int w, int h, int x, int y,
                    const char *s, u32 fg, int which);
