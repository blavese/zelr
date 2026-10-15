#pragma once
/* A page's canvas: the 2D context (CanvasRenderingContext2D), its gradients,
 * patterns, paths (Path2D) and pictures (ImageData), and the canvas's own
 * toDataURL and toBlob. Included from jsdom.h.
 *
 * getContext('2d') gave null, which the standard allows and nothing on the
 * web expects: Lottie, the animation library on a large part of it, makes a
 * canvas as its module loads and sets fillStyle on what it got, and every
 * page carrying it (Vimeo's) stopped there.
 *
 * Each canvas is a bitmap of its width and height, four bytes a pixel --
 * red, green, blue and alpha, not premultiplied, as ImageData has them --
 * in a table of its own (jc_canvas), malloc'd, holding no script values.
 * A context's paints (gradients and patterns) are script objects, so they
 * are kept on the context object, where the collector looks (jsgc.h), one
 * place for each state a save() can make (jc_paint_slot).
 *
 * Shapes are filled by svg.h's scanline (sv_cover, sampled four times a pixel
 * each way) and painted a row at a time here; a stroke is made into shapes
 * first (each segment a quadrilateral, each join and cap its own shape, all
 * filled together so nothing is painted twice). Text is drawn in the
 * browser's own faces (tface.h), scaled past the largest.
 *
 * What it does not do, and says nothing about: shadows (their settings are
 * kept and given back, nothing is drawn), filters, text along a transform
 * that turns or skews it (it is placed by the transform and drawn upright),
 * and the compositing operations that change what lies outside the shape
 * (copy, source-in and destination-in change only what the shape covers).
 * WebGL is refused, as getContext('webgl') returning null says. */
#include "svg.h"
#include "tface.h"

#define JC_MAX      32                  /* canvases a page draws on at once */
#define JC_BYTES    (16 * 1024 * 1024)  /* one canvas's pixels at most */
#define JC_ALL      (48 * 1024 * 1024)  /* every canvas's together */
#define JC_STATES   32                  /* save()s deep */
#define JC_STOPS    32                  /* colour stops a gradient uses */

enum { JC_OVER, JC_COPY, JC_DST_OVER, JC_DST_OUT, JC_SRC_IN, JC_SRC_ATOP, JC_DST_IN,
       JC_DST_ATOP, JC_LIGHTER, JC_XOR, JC_OPS };
static const char *const JC_OP_NAMES[JC_OPS] = {
    "source-over", "copy", "destination-over", "destination-out", "source-in", "source-atop",
    "destination-in", "destination-atop", "lighter", "xor",
};
static const char *const JC_CAPS[] = { "butt", "round", "square", 0 };
static const char *const JC_JOINS[] = { "miter", "round", "bevel", 0 };
static const char *const JC_ALIGNS[] = { "start", "end", "left", "right", "center", 0 };
static const char *const JC_BASELINES[] = { "alphabetic", "top", "hanging", "middle", "ideographic", "bottom", 0 };

typedef struct {
    double a, b, c, d, e, f;          /* the transform: x' = a x + c y + e, y' = b x + d y + f */
    u32 fill, stroke;                 /* 0xAARRGGBB, when the paint is a colour */
    int fill_paint, stroke_paint;     /* 1 when the paint is the kept gradient or pattern */
    double line_width, miter, dash_off;
    double dash[16];
    int ndash;
    u8 cap, join, op, align, baseline, bold;
    double alpha;
    int font_px;
    char font[96];
    int clip;                         /* 1 + the slot of the clip's mask, 0 for none */
} jcstate;

/* A path as it is drawn: points on the canvas, the transform already applied
   when each was added (as the standard has it), curves flattened. */
typedef struct {
    svpt *pts;
    int n, cap;
    int *start;
    u8 *closed;
    int nsub, capsub;
    int open;                         /* the last subpath can still be added to */
    double ux, uy;                    /* the current point, as the page wrote it */
    double dx, dy;                    /* and where it is on the canvas */
    int has_cur;
} jcpath;

typedef struct {
    int node;                         /* the <canvas>, -1 for a free slot */
    int w, h;
    u8 *px;
    int reset;                        /* its width or height was set: cleared at next use */
    int drawn;                        /* changed since the browser last took it */
    u8 *rgb, *alpha;                  /* the browser's copy, three bytes and one */
    int copy_ok;
    jcstate st[JC_STATES];
    int depth;
    jcpath path;
    u8 *mask[JC_STATES + 1];          /* clip masks, a byte a pixel */
    int mask_refs[JC_STATES + 1];
} jcanvas;

static jcanvas jc_canvas[JC_MAX];
static int jc_drawn_any;
static long long jc_bytes;
static jstr *jd_k_jc, *jd_k_jcctx, *jd_k_jcpaints, *jd_k_grad, *jd_k_stops, *jd_k_pat, *jd_k_patpx;
static jstr *jd_k_p2d, *jd_k_idw, *jd_k_idh, *jd_k_iddata;
static jobj *jd_p_ctx2d, *jd_p_grad, *jd_p_pattern, *jd_p_imagedata, *jd_p_path2d, *jd_p_metrics;

/* The browser's pictures, for drawImage of an <img>: its decoded pixels,
   three bytes each, and their alpha or 0 (browser.c, image_pixels). */
static const u8 *(*jd_image_pixels)(int node, int *w, int *h, const u8 **alpha);
void jsdom_image_pixels_with(const u8 *(*fn)(int, int *, int *, const u8 **)) { jd_image_pixels = fn; }

static double jc_sin(double x) { return js_circle(x, 0); }
static double jc_cos(double x) { return js_circle(x, 1); }
static double jc_abs(double x) { return x < 0 ? -x : x; }
static int jc_finite(double x) { return x == x && x - x == 0; }

/* --- paths ------------------------------------------------------------------------------- */

static void jc_path_clear(jcpath *p) {
    p->n = 0;
    p->nsub = 0;
    p->open = 0;
    p->has_cur = 0;
}

static void jc_path_free(jcpath *p) {
    if (p->pts) free(p->pts);
    if (p->start) free(p->start);
    if (p->closed) free(p->closed);
    p->pts = 0; p->start = 0; p->closed = 0;
    p->cap = p->capsub = 0;
    jc_path_clear(p);
}

/* Grown by copying, as anything malloc'd here is; a path past a million
   points stops growing rather than taking the machine. */
static int jc_path_room(jcpath *p, int pts, int subs) {
    if (p->n + pts > p->cap) {
        int c = p->cap ? p->cap * 2 : 256;
        while (c < p->n + pts) c *= 2;
        if (c > 1 << 20) return 0;
        svpt *np = (svpt *)malloc((u64)c * sizeof(svpt));
        if (!np) return 0;
        for (int i = 0; i < p->n; i++) np[i] = p->pts[i];
        if (p->pts) free(p->pts);
        p->pts = np;
        p->cap = c;
    }
    if (p->nsub + subs > p->capsub) {
        int c = p->capsub ? p->capsub * 2 : 32;
        while (c < p->nsub + subs) c *= 2;
        if (c > 1 << 18) return 0;
        int *ns = (int *)malloc((u64)c * sizeof(int));
        u8 *nc = (u8 *)malloc((u64)c);
        if (!ns || !nc) { if (ns) free(ns); if (nc) free(nc); return 0; }
        for (int i = 0; i < p->nsub; i++) { ns[i] = p->start[i]; nc[i] = p->closed[i]; }
        if (p->start) free(p->start);
        if (p->closed) free(p->closed);
        p->start = ns;
        p->closed = nc;
        p->capsub = c;
    }
    return 1;
}

static void jc_xy(const jcstate *s, double x, double y, double *ox, double *oy) {
    *ox = s->a * x + s->c * y + s->e;
    *oy = s->b * x + s->d * y + s->f;
}

/* A point on the canvas back to the page's coordinates under s; 0 when the
   transform has no inverse (it flattens everything to a line). */
static int jc_unxy(const jcstate *s, double x, double y, double *ox, double *oy) {
    double det = s->a * s->d - s->b * s->c;
    if (det == 0 || !jc_finite(det)) return 0;
    x -= s->e;
    y -= s->f;
    *ox = (s->d * x - s->c * y) / det;
    *oy = (-s->b * x + s->a * y) / det;
    return 1;
}

/* How much the transform stretches a length: the square root of its area's. */
static double jc_scale(const jcstate *s) {
    double det = jc_abs(s->a * s->d - s->b * s->c);
    return det > 0 ? js_sqrt(det) : 0;
}

static void jc_path_new_sub(jcpath *p, double dx, double dy) {
    if (!jc_path_room(p, 1, 1)) return;
    p->start[p->nsub] = p->n;
    p->closed[p->nsub] = 0;
    p->nsub++;
    p->pts[p->n].x = (float)dx;
    p->pts[p->n].y = (float)dy;
    p->n++;
    p->open = 1;
}

static void jc_move(jcpath *p, const jcstate *s, double x, double y) {
    if (!jc_finite(x) || !jc_finite(y)) return;
    double dx, dy;
    jc_xy(s, x, y, &dx, &dy);
    jc_path_new_sub(p, dx, dy);
    p->ux = x; p->uy = y; p->dx = dx; p->dy = dy;
    p->has_cur = 1;
}

/* A point on the canvas added to the subpath being made, or the start of
   one when there is none (lineTo with no current point is a moveTo). */
static void jc_add_dev(jcpath *p, double dx, double dy) {
    if (!p->open) { jc_path_new_sub(p, p->has_cur ? p->dx : dx, p->has_cur ? p->dy : dy); }
    if (!jc_path_room(p, 1, 0)) return;
    p->pts[p->n].x = (float)dx;
    p->pts[p->n].y = (float)dy;
    p->n++;
    p->dx = dx; p->dy = dy;
    p->has_cur = 1;
}

static void jc_line(jcpath *p, const jcstate *s, double x, double y) {
    if (!jc_finite(x) || !jc_finite(y)) return;
    double dx, dy;
    jc_xy(s, x, y, &dx, &dy);
    if (!p->has_cur) { jc_move(p, s, x, y); return; }
    jc_add_dev(p, dx, dy);
    p->ux = x; p->uy = y;
}

static void jc_close(jcpath *p) {
    if (!p->nsub || !p->open) return;
    int at = p->start[p->nsub - 1];
    p->closed[p->nsub - 1] = 1;
    double sx = p->pts[at].x, sy = p->pts[at].y;
    p->open = 0;
    /* What follows starts where this one did. */
    p->dx = sx; p->dy = sy;
    p->has_cur = 1;
}

/* Where the current point is in the page's coordinates under s now: the
   transform may have changed since it was set. */
static void jc_cur_user(const jcpath *p, const jcstate *s, double *x, double *y) {
    if (!jc_unxy(s, p->dx, p->dy, x, y)) { *x = p->ux; *y = p->uy; }
}

/* Enough straight pieces that a curve looks like one on the canvas: by its
   length there, a piece every two pixels or so, at least four. */
static int jc_pieces(double len) {
    int k = (int)(len / 2) + 4;
    return k > 256 ? 256 : k;
}

static void jc_quad(jcpath *p, const jcstate *s, double cx, double cy, double x, double y) {
    if (!jc_finite(cx) || !jc_finite(cy) || !jc_finite(x) || !jc_finite(y)) return;
    double x0, y0;
    if (!p->has_cur) jc_move(p, s, cx, cy);
    jc_cur_user(p, s, &x0, &y0);
    double sc = jc_scale(s);
    int k = jc_pieces((jc_abs(cx - x0) + jc_abs(cy - y0) + jc_abs(x - cx) + jc_abs(y - cy)) * sc);
    for (int i = 1; i <= k; i++) {
        double t = (double)i / k, u = 1 - t;
        double px = u * u * x0 + 2 * u * t * cx + t * t * x, py = u * u * y0 + 2 * u * t * cy + t * t * y;
        double dx, dy;
        jc_xy(s, px, py, &dx, &dy);
        jc_add_dev(p, dx, dy);
    }
    p->ux = x; p->uy = y;
}

static void jc_cubic(jcpath *p, const jcstate *s, double c1x, double c1y, double c2x, double c2y,
                     double x, double y) {
    if (!jc_finite(c1x) || !jc_finite(c1y) || !jc_finite(c2x) || !jc_finite(c2y)
        || !jc_finite(x) || !jc_finite(y)) return;
    double x0, y0;
    if (!p->has_cur) jc_move(p, s, c1x, c1y);
    jc_cur_user(p, s, &x0, &y0);
    double sc = jc_scale(s);
    int k = jc_pieces((jc_abs(c1x - x0) + jc_abs(c1y - y0) + jc_abs(c2x - c1x) + jc_abs(c2y - c1y)
                       + jc_abs(x - c2x) + jc_abs(y - c2y)) * sc);
    for (int i = 1; i <= k; i++) {
        double t = (double)i / k, u = 1 - t;
        double px = u * u * u * x0 + 3 * u * u * t * c1x + 3 * u * t * t * c2x + t * t * t * x;
        double py = u * u * u * y0 + 3 * u * u * t * c1y + 3 * u * t * t * c2y + t * t * t * y;
        double dx, dy;
        jc_xy(s, px, py, &dx, &dy);
        jc_add_dev(p, dx, dy);
    }
    p->ux = x; p->uy = y;
}

#define JC_PI 3.14159265358979323846

/* An arc of an ellipse centred at (x, y), radii rx and ry, turned by rot,
   from angle a0 to a1 the way ccw says, joined to what came before by a
   line (as arc() is). */
static void jc_ellipse(jcpath *p, const jcstate *s, double x, double y, double rx, double ry,
                       double rot, double a0, double a1, int ccw) {
    if (!jc_finite(x) || !jc_finite(y) || !jc_finite(rx) || !jc_finite(ry)
        || !jc_finite(rot) || !jc_finite(a0) || !jc_finite(a1)) return;
    double sweep = a1 - a0;
    if (!ccw) {
        if (sweep >= 2 * JC_PI) sweep = 2 * JC_PI;
        else { while (sweep < 0) sweep += 2 * JC_PI; }
    } else {
        if (sweep <= -2 * JC_PI) sweep = -2 * JC_PI;
        else { while (sweep > 0) sweep -= 2 * JC_PI; }
    }
    double sc = jc_scale(s);
    int k = jc_pieces(jc_abs(sweep) * (rx > ry ? rx : ry) * sc);
    double cr = jc_cos(rot), sr = jc_sin(rot);
    for (int i = 0; i <= k; i++) {
        double t = a0 + sweep * i / k;
        double ex = rx * jc_cos(t), ey = ry * jc_sin(t);
        double px = x + ex * cr - ey * sr, py = y + ex * sr + ey * cr;
        double dx, dy;
        jc_xy(s, px, py, &dx, &dy);
        if (i == 0 && !p->has_cur) { jc_path_new_sub(p, dx, dy); p->has_cur = 1; }
        else jc_add_dev(p, dx, dy);
        p->ux = px; p->uy = py;
    }
}

/* arcTo: a line towards (x1, y1), turning along a circle of radius r to
   head for (x2, y2). */
static void jc_arc_to(jcpath *p, const jcstate *s, double x1, double y1, double x2, double y2, double r) {
    if (!jc_finite(x1) || !jc_finite(y1) || !jc_finite(x2) || !jc_finite(y2) || !jc_finite(r)) return;
    if (!p->has_cur) jc_move(p, s, x1, y1);
    double x0, y0;
    jc_cur_user(p, s, &x0, &y0);
    double ax = x0 - x1, ay = y0 - y1, bx = x2 - x1, by = y2 - y1;
    double la = js_sqrt(ax * ax + ay * ay), lb = js_sqrt(bx * bx + by * by);
    double cross = ax * by - ay * bx;
    if (r == 0 || la == 0 || lb == 0 || jc_abs(cross) < 1e-9 * la * lb) { jc_line(p, s, x1, y1); return; }
    ax /= la; ay /= la; bx /= lb; by /= lb;
    double cosang = ax * bx + ay * by;
    double half = js_atan2(js_sqrt(1 - cosang * cosang), cosang) / 2;   /* half the angle between them */
    double dist = r / (jc_sin(half) / jc_cos(half));                    /* from the corner to each tangent point */
    double t0x = x1 + ax * dist, t0y = y1 + ay * dist, t1x = x1 + bx * dist, t1y = y1 + by * dist;
    double bis_x = ax + bx, bis_y = ay + by, bl = js_sqrt(bis_x * bis_x + bis_y * bis_y);
    double cd = r / jc_sin(half);
    double cx = x1 + bis_x / bl * cd, cy = y1 + bis_y / bl * cd;
    jc_line(p, s, t0x, t0y);
    double a0 = js_atan2(t0y - cy, t0x - cx), a1 = js_atan2(t1y - cy, t1x - cx);
    jc_ellipse(p, s, cx, cy, r, r, 0, a0, a1, cross > 0);
}

static void jc_rect(jcpath *p, const jcstate *s, double x, double y, double w, double h) {
    if (!jc_finite(x) || !jc_finite(y) || !jc_finite(w) || !jc_finite(h)) return;
    jc_move(p, s, x, y);
    jc_line(p, s, x + w, y);
    jc_line(p, s, x + w, y + h);
    jc_line(p, s, x, y + h);
    jc_close(p);
    jc_move(p, s, x, y);
}

/* roundRect with up to four radii (all the same, or top left and right,
   bottom right and left as CSS gives them). */
static void jc_round_rect(jcpath *p, const jcstate *s, double x, double y, double w, double h,
                          const double *r4) {
    double r[4] = { r4[0], r4[1], r4[2], r4[3] };
    /* Too big for the sides, they are scaled down together. */
    double aw = jc_abs(w), ah = jc_abs(h), f = 1;
    double top = r[0] + r[1], bot = r[2] + r[3], left = r[0] + r[3], right = r[1] + r[2];
    if (top > aw && top > 0) f = aw / top < f ? aw / top : f;
    if (bot > aw && bot > 0) f = aw / bot < f ? aw / bot : f;
    if (left > ah && left > 0) f = ah / left < f ? ah / left : f;
    if (right > ah && right > 0) f = ah / right < f ? ah / right : f;
    for (int i = 0; i < 4; i++) r[i] *= f;
    jc_move(p, s, x + r[0], y);
    jc_line(p, s, x + w - r[1], y);
    if (r[1] > 0) jc_ellipse(p, s, x + w - r[1], y + r[1], r[1], r[1], 0, -JC_PI / 2, 0, 0);
    jc_line(p, s, x + w, y + h - r[2]);
    if (r[2] > 0) jc_ellipse(p, s, x + w - r[2], y + h - r[2], r[2], r[2], 0, 0, JC_PI / 2, 0);
    jc_line(p, s, x + r[3], y + h);
    if (r[3] > 0) jc_ellipse(p, s, x + r[3], y + h - r[3], r[3], r[3], 0, JC_PI / 2, JC_PI, 0);
    jc_line(p, s, x, y + r[0]);
    if (r[0] > 0) jc_ellipse(p, s, x + r[0], y + r[0], r[0], r[0], 0, JC_PI, JC_PI * 1.5, 0);
    jc_close(p);
    jc_move(p, s, x, y);
}

/* --- painting ------------------------------------------------------------------------------ */

enum { JP_COLOUR, JP_LINEAR, JP_RADIAL, JP_CONIC, JP_IMAGE };

/* What one fill, stroke, text or picture paints with, worked out once. */
typedef struct {
    int kind;
    u32 colour;                       /* 0xAARRGGBB */
    u32 lut[256];                     /* a gradient's colours along it */
    jcstate inv;                      /* only its transform: from the canvas back to the page */
    int has_inv;
    double x0, y0, r0, x1, y1, r1, angle;
    /* a picture or pattern: its pixels and how to step through them */
    const u8 *src;
    const u8 *src_alpha;              /* a separate plane, or 0 (with 4 bytes a pixel, the fourth) */
    int sw, sh, bpp;
    int rep_x, rep_y;
    int smooth;
    double ia, ib, ic, id, ie, iff;   /* the canvas to the picture's pixels */
} jcpaint;

typedef struct {
    jcanvas *cv;
    jcpaint *paint;
    double alpha;                     /* globalAlpha */
    int op;
    const u8 *mask;
} jcdraw;

static u8 jc_clamp8(double v) { return v <= 0 ? 0 : v >= 255 ? 255 : (u8)(v + 0.5); }

/* A colour as fillStyle gives it back: #rrggbb when it is opaque, else
   rgba(r, g, b, a) with the alpha as short a decimal as says it. */
static void jc_colour_text(u32 c, char *out, int cap) {
    int a = (int)(c >> 24), r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
    static const char HEX[] = "0123456789abcdef";
    int w = 0;
    if (a == 255) {
        if (cap < 8) return;
        out[w++] = '#';
        int v[3] = { r, g, b };
        for (int i = 0; i < 3; i++) { out[w++] = HEX[v[i] >> 4]; out[w++] = HEX[v[i] & 15]; }
        out[w] = 0;
        return;
    }
    char buf[64];
    int milli = (a * 1000 + 127) / 255;           /* alpha in thousandths, as browsers round it */
    char am[8];
    int k = 0;
    if (milli >= 1000) { am[k++] = '1'; }
    else {
        am[k++] = '0';
        if (milli) {
            am[k++] = '.';
            int d[3] = { milli / 100, (milli / 10) % 10, milli % 10 }, last = 2;
            while (last > 0 && !d[last]) last--;
            for (int i = 0; i <= last; i++) am[k++] = (char)('0' + d[i]);
        }
    }
    am[k] = 0;
    int n = 0;
    const char *pre = "rgba(";
    for (const char *q = pre; *q; q++) buf[n++] = *q;
    int v[3] = { r, g, b };
    for (int i = 0; i < 3; i++) {
        char num[4]; int m = 0, x = v[i];
        if (!x) num[m++] = '0';
        while (x) { num[m++] = (char)('0' + x % 10); x /= 10; }
        while (m) buf[n++] = num[--m];
        buf[n++] = ','; buf[n++] = ' ';
    }
    for (int i = 0; am[i]; i++) buf[n++] = am[i];
    buf[n++] = ')';
    buf[n] = 0;
    for (w = 0; w < n && w < cap - 1; w++) out[w] = buf[w];
    out[w] = 0;
}

/* A CSS colour, or 0 when it is not one. */
static int jc_parse_colour(const char *s, u32 *out) {
    u32 blended;
    if (!css_color(s, &blended)) return 0;
    *out = ((u32)css_last_alpha << 24) | (css_last_raw & 0xFFFFFF);
    return 1;
}

/* The gradient's colour at t (0 to 1) from its stops: those at the same
   offset meet sharply, and before the first and past the last the end
   colours run on. Interpolated as the standard says, premultiplied. */
static u32 jc_stop_colour(const double *off, const u32 *col, int n, double t) {
    if (n == 0) return 0;
    if (t <= off[0]) return col[0];
    if (t >= off[n - 1]) return col[n - 1];
    int i = 0;
    while (i + 1 < n && off[i + 1] < t) i++;
    if (i + 1 >= n) return col[n - 1];
    double span = off[i + 1] - off[i];
    double f = span > 0 ? (t - off[i]) / span : 1;
    u32 c0 = col[i], c1 = col[i + 1];
    double a0 = (double)(c0 >> 24) / 255, a1 = (double)(c1 >> 24) / 255;
    double a = a0 + (a1 - a0) * f;
    double rgb[3];
    for (int k = 0; k < 3; k++) {
        int sh = 16 - 8 * k;
        double v0 = (double)((c0 >> sh) & 255) * a0, v1 = (double)((c1 >> sh) & 255) * a1;
        double v = v0 + (v1 - v0) * f;
        rgb[k] = a > 0 ? v / a : 0;
    }
    return ((u32)jc_clamp8(a * 255) << 24) | ((u32)jc_clamp8(rgb[0]) << 16)
         | ((u32)jc_clamp8(rgb[1]) << 8) | jc_clamp8(rgb[2]);
}

/* t for a radial gradient at (x, y): the largest t whose circle, between
   the two circles, passes through the point with a radius not below 0. */
static int jc_radial_t(const jcpaint *p, double x, double y, double *t) {
    double cdx = p->x1 - p->x0, cdy = p->y1 - p->y0, dr = p->r1 - p->r0;
    double pdx = x - p->x0, pdy = y - p->y0;
    double a = cdx * cdx + cdy * cdy - dr * dr;
    double b = pdx * cdx + pdy * cdy + p->r0 * dr;
    double c = pdx * pdx + pdy * pdy - p->r0 * p->r0;
    if (jc_abs(a) < 1e-9) {
        if (b == 0) return 0;
        double s = c / (2 * b);
        if (p->r0 + s * dr < 0) return 0;
        *t = s;
        return 1;
    }
    double disc = b * b - a * c;
    if (disc < 0) return 0;
    double sq = js_sqrt(disc);
    double s1 = (b + sq) / a, s2 = (b - sq) / a;
    double hi = s1 > s2 ? s1 : s2, lo = s1 > s2 ? s2 : s1;
    if (p->r0 + hi * dr >= 0) { *t = hi; return 1; }
    if (p->r0 + lo * dr >= 0) { *t = lo; return 1; }
    return 0;
}

/* One sample of a picture at (u, v) in its pixels, with its alpha; 0 when
   it falls outside a side that does not repeat. */
static int jc_sample(const jcpaint *p, double u, double v, double *r, double *g, double *b, double *a) {
    if (p->sw <= 0 || p->sh <= 0) return 0;
    /* A side that does not repeat ends at the picture's edge; inside, the
       samples nearest it are the edge's own. */
    if (!p->rep_x && (u < 0 || u >= p->sw)) return 0;
    if (!p->rep_y && (v < 0 || v >= p->sh)) return 0;
    int smooth = p->smooth;
    double fu = smooth ? u - 0.5 : u, fv = smooth ? v - 0.5 : v;
    int iu = (int)js_floor(fu), iv = (int)js_floor(fv);
    double tx = smooth ? fu - iu : 0, ty = smooth ? fv - iv : 0;
    double acc[4] = { 0, 0, 0, 0 };
    double wsum = 0;
    for (int k = 0; k < (smooth ? 4 : 1); k++) {
        int x = iu + (k & 1), y = iv + (k >> 1);
        double wgt = smooth ? ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty) : 1;
        if (wgt <= 0) continue;
        if (p->rep_x) { x %= p->sw; if (x < 0) x += p->sw; }
        else x = x < 0 ? 0 : x >= p->sw ? p->sw - 1 : x;
        if (p->rep_y) { y %= p->sh; if (y < 0) y += p->sh; }
        else y = y < 0 ? 0 : y >= p->sh ? p->sh - 1 : y;
        const u8 *q = p->src + ((u64)y * (u64)p->sw + (u64)x) * (u64)p->bpp;
        double al = p->bpp == 4 ? q[3] : p->src_alpha ? p->src_alpha[(u64)y * (u64)p->sw + (u64)x] : 255;
        acc[0] += q[0] * al * wgt; acc[1] += q[1] * al * wgt; acc[2] += q[2] * al * wgt; acc[3] += al * wgt;
        wsum += wgt;
    }
    if (wsum <= 0 || acc[3] <= 0) return 0;
    *a = acc[3] / wsum;
    *r = acc[0] / acc[3]; *g = acc[1] / acc[3]; *b = acc[2] / acc[3];
    return 1;
}

/* The paint's colour at the centre of pixel (x, y) of the canvas. */
static int jc_paint_at(const jcpaint *p, int x, int y, double *r, double *g, double *b, double *a) {
    u32 c;
    if (p->kind == JP_COLOUR) c = p->colour;
    else if (p->kind == JP_IMAGE) {
        double px = x + 0.5, py = y + 0.5;
        double u = p->ia * px + p->ic * py + p->ie, v = p->ib * px + p->id * py + p->iff;
        return jc_sample(p, u, v, r, g, b, a);
    } else {
        if (!p->has_inv) return 0;
        double ux, uy;
        jc_unxy(&p->inv, x + 0.5, y + 0.5, &ux, &uy);
        double t;
        if (p->kind == JP_LINEAR) {
            double dx = p->x1 - p->x0, dy = p->y1 - p->y0, len2 = dx * dx + dy * dy;
            if (len2 == 0) return 0;
            t = ((ux - p->x0) * dx + (uy - p->y0) * dy) / len2;
        } else if (p->kind == JP_RADIAL) {
            if (!jc_radial_t(p, ux, uy, &t)) return 0;
        } else {
            double ang = js_atan2(uy - p->y0, ux - p->x0) - p->angle;
            t = ang / (2 * JC_PI);
            t -= js_floor(t);
        }
        int i = t <= 0 ? 0 : t >= 1 ? 255 : (int)(t * 255 + 0.5);
        c = p->lut[i];
    }
    *a = (double)(c >> 24);
    *r = (double)((c >> 16) & 255);
    *g = (double)((c >> 8) & 255);
    *b = (double)(c & 255);
    return 1;
}

/* Source over the canvas's pixel at (x, y) with coverage cov, by the
   drawing's operation, in non-premultiplied bytes. */
static void jc_put(jcdraw *dw, int x, int y, double cov) {
    jcanvas *cv = dw->cv;
    if (x < 0 || y < 0 || x >= cv->w || y >= cv->h || cov <= 0) return;
    if (dw->mask) {
        cov *= dw->mask[(u64)y * (u64)cv->w + (u64)x] / 255.0;
        if (cov <= 0) return;
    }
    if (cov > 1) cov = 1;
    double sr, sg, sb, sa;
    if (!jc_paint_at(dw->paint, x, y, &sr, &sg, &sb, &sa)) {
        if (dw->op != JC_COPY && dw->op != JC_SRC_IN && dw->op != JC_DST_IN) return;
        sr = sg = sb = sa = 0;
    }
    sa = sa / 255 * dw->alpha;
    u8 *d = cv->px + ((u64)y * (u64)cv->w + (u64)x) * 4;
    double da = d[3] / 255.0;
    double dr = d[0], dg = d[1], db = d[2];
    /* Premultiplied from here to the end. */
    double ps[3] = { sr * sa, sg * sa, sb * sa }, pd[3] = { dr * da, dg * da, db * da };
    double oa, po[3];
    int linear = dw->op != JC_COPY && dw->op != JC_SRC_IN && dw->op != JC_DST_IN && dw->op != JC_DST_ATOP;
    double s_a = linear ? sa * cov : sa;
    double ls[3];
    for (int k = 0; k < 3; k++) ls[k] = linear ? ps[k] * cov : ps[k];
    switch (dw->op) {
    case JC_COPY:
        oa = s_a;
        for (int k = 0; k < 3; k++) po[k] = ls[k];
        break;
    case JC_DST_OVER:
        oa = da + s_a * (1 - da);
        for (int k = 0; k < 3; k++) po[k] = pd[k] + ls[k] * (1 - da);
        break;
    case JC_DST_OUT:
        oa = da * (1 - s_a);
        for (int k = 0; k < 3; k++) po[k] = pd[k] * (1 - s_a);
        break;
    case JC_SRC_IN:
        oa = s_a * da;
        for (int k = 0; k < 3; k++) po[k] = ls[k] * da;
        break;
    case JC_DST_IN:
        oa = da * s_a;
        for (int k = 0; k < 3; k++) po[k] = pd[k] * s_a;
        break;
    case JC_SRC_ATOP:
        oa = da;
        for (int k = 0; k < 3; k++) po[k] = ls[k] * da + pd[k] * (1 - s_a);
        break;
    case JC_DST_ATOP:
        oa = s_a;
        for (int k = 0; k < 3; k++) po[k] = pd[k] * s_a + ls[k] * (1 - da);
        break;
    case JC_LIGHTER:
        oa = s_a + da;
        if (oa > 1) oa = 1;
        for (int k = 0; k < 3; k++) po[k] = ls[k] + pd[k];
        break;
    case JC_XOR:
        oa = s_a * (1 - da) + da * (1 - s_a);
        for (int k = 0; k < 3; k++) po[k] = ls[k] * (1 - da) + pd[k] * (1 - s_a);
        break;
    default:
        oa = s_a + da * (1 - s_a);
        for (int k = 0; k < 3; k++) po[k] = ls[k] + pd[k] * (1 - s_a);
        break;
    }
    if (!linear) {
        /* Partly covered, the result is laid over what was there by how much. */
        oa = oa * cov + da * (1 - cov);
        for (int k = 0; k < 3; k++) po[k] = po[k] * cov + pd[k] * (1 - cov);
    }
    if (oa <= 0) { d[0] = d[1] = d[2] = d[3] = 0; return; }
    d[0] = jc_clamp8(po[0] / oa);
    d[1] = jc_clamp8(po[1] / oa);
    d[2] = jc_clamp8(po[2] / oa);
    d[3] = jc_clamp8(oa * 255);
}

static void jc_row(void *ctx, int y, const float *cov, int x0, int x1) {
    jcdraw *dw = (jcdraw *)ctx;
    for (int x = x0; x < x1; x++) if (cov[x] > 0.002f) jc_put(dw, x, y, cov[x]);
}

/* A path filled by the drawing's paint, by the nonzero rule or even-odd. */
static void jc_fill_path(jcdraw *dw, const jcpath *p, int evenodd) {
    if (!p->n || !p->nsub) return;
    sv_cover(dw->cv->w, dw->cv->h, p->pts, p->n, p->start, p->nsub, evenodd, jc_row, dw);
    dw->cv->drawn = 1;
    dw->cv->copy_ok = 0;
    jc_drawn_any = 1;
}

/* --- strokes ------------------------------------------------------------------------------ */

/* A shape added to a path, turned to go the same way round as every other,
   so that where they overlap the nonzero rule sees more inside, never a
   hole. */
static void jc_shape(jcpath *out, const double *xy, int k) {
    double area = 0;
    for (int i = 0; i < k; i++) {
        int j = (i + 1) % k;
        area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
    }
    if (area == 0 || !(area == area)) return;
    if (!jc_path_room(out, k, 1)) return;
    out->start[out->nsub] = out->n;
    out->closed[out->nsub] = 1;
    out->nsub++;
    for (int i = 0; i < k; i++) {
        int at = area > 0 ? i : k - 1 - i;
        out->pts[out->n].x = (float)xy[2 * at];
        out->pts[out->n].y = (float)xy[2 * at + 1];
        out->n++;
    }
}

static void jc_disc(jcpath *out, double cx, double cy, double r) {
    int k = (int)(r * 1.2) + 8;
    if (k > 64) k = 64;
    double xy[128];
    for (int i = 0; i < k; i++) {
        double t = 2 * JC_PI * i / k;
        xy[2 * i] = cx + r * jc_cos(t);
        xy[2 * i + 1] = cy + r * jc_sin(t);
    }
    jc_shape(out, xy, k);
}

/* The join at (bx, by) between a segment heading d0 and the next heading
   d1, on the outside of the turn. */
static void jc_join(jcpath *out, double bx, double by, double d0x, double d0y, double d1x, double d1y,
                    double hw, int join, double miter) {
    double cross = d0x * d1y - d0y * d1x, dot = d0x * d1x + d0y * d1y;
    if (jc_abs(cross) < 1e-9 && dot > 0) return;           /* straight on */
    if (join == 1) { jc_disc(out, bx, by, hw); return; }
    double s = cross > 0 ? -1 : 1;
    double n0x = -d0y * hw * s, n0y = d0x * hw * s, n1x = -d1y * hw * s, n1y = d1x * hw * s;
    if (join == 0) {
        double mx = n0x + n1x, my = n0y + n1y, ml = js_sqrt(mx * mx + my * my);
        if (ml > 0) {
            double cphi = (mx * n0x + my * n0y) / (ml * hw);
            /* The standard's limit: the tip's distance from the joint over
               half the width. */
            if (cphi > 0 && 1 / cphi <= miter) {
                double len = hw / cphi;
                double q[8] = { bx, by, bx + n0x, by + n0y, bx + mx / ml * len, by + my / ml * len,
                                bx + n1x, by + n1y };
                jc_shape(out, q, 4);
                return;
            }
        }
    }
    double tri[6] = { bx, by, bx + n0x, by + n0y, bx + n1x, by + n1y };
    jc_shape(out, tri, 3);
}

/* A run of points stroked: a quadrilateral a segment, joins between them,
   and caps at the ends of one that is not closed. */
static void jc_stroke_run(jcpath *out, const svpt *in, int k, int closed, double hw, int cap, int join,
                          double miter) {
    /* Points that repeat the one before them make no segment. */
    svpt *p = (svpt *)malloc((u64)(k > 0 ? k : 1) * sizeof(svpt));
    if (!p) return;
    int m = 0;
    for (int i = 0; i < k; i++)
        if (!m || in[i].x != p[m - 1].x || in[i].y != p[m - 1].y) p[m++] = in[i];
    if (closed && m > 1 && p[0].x == p[m - 1].x && p[0].y == p[m - 1].y) m--;
    if (m < 2) {
        /* A point with nothing to stroke draws its caps, if they have a shape. */
        if (m == 1 && !closed && cap == 1) jc_disc(out, p[0].x, p[0].y, hw);
        if (m == 1 && !closed && cap == 2) {
            double q[8] = { p[0].x - hw, p[0].y - hw, p[0].x + hw, p[0].y - hw, p[0].x + hw, p[0].y + hw,
                            p[0].x - hw, p[0].y + hw };
            jc_shape(out, q, 4);
        }
        free(p);
        return;
    }
    int segs = closed ? m : m - 1;
    for (int i = 0; i < segs; i++) {
        svpt a = p[i], b = p[(i + 1) % m];
        double dx = b.x - a.x, dy = b.y - a.y, len = js_sqrt(dx * dx + dy * dy);
        if (len <= 0) continue;
        dx /= len; dy /= len;
        double ax = a.x, ay = a.y, bx = b.x, by = b.y;
        if (!closed && cap == 2) {
            if (i == 0) { ax -= dx * hw; ay -= dy * hw; }
            if (i == segs - 1) { bx += dx * hw; by += dy * hw; }
        }
        double nx = -dy * hw, ny = dx * hw;
        double q[8] = { ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny };
        jc_shape(out, q, 4);
        /* The join at b, with the segment after it. */
        if (closed || i + 1 < segs) {
            svpt c = p[(i + 2) % m];
            double ex = c.x - b.x, ey = c.y - b.y, el = js_sqrt(ex * ex + ey * ey);
            if (el > 0) jc_join(out, b.x, b.y, dx, dy, ex / el, ey / el, hw, join, miter);
        }
    }
    if (!closed && cap == 1) {
        jc_disc(out, p[0].x, p[0].y, hw);
        jc_disc(out, p[m - 1].x, p[m - 1].y, hw);
    }
    free(p);
}

/* A path's stroke as shapes to fill, dashed when the state says so. Lengths
   are stretched by the transform's scale; under a transform that stretches
   one way more than another, a stroke is as wide as the average of them. */
static void jc_stroke_shapes(jcpath *out, const jcpath *in, const jcstate *s) {
    double sc = jc_scale(s);
    double hw = s->line_width * sc / 2;
    if (!(hw > 0)) return;
    if (hw < 0.125) hw = 0.125;                /* thinner than this misses the samples */
    double total = 0;
    for (int i = 0; i < s->ndash; i++) total += s->dash[i] * sc;
    svpt *piece = 0;
    int piece_cap = 0;
    for (int sub = 0; sub < in->nsub; sub++) {
        int from = in->start[sub], to = sub + 1 < in->nsub ? in->start[sub + 1] : in->n;
        int closed = in->closed[sub];
        if (to - from < 1) continue;
        if (!s->ndash || !(total > 0)) {
            jc_stroke_run(out, in->pts + from, to - from, closed, hw, s->cap, s->join, s->miter);
            continue;
        }
        /* Dashed: walk the run, on and off by the pattern, each piece that
           is on stroked as a run of its own. */
        if (piece_cap < to - from + 2) {
            if (piece) free(piece);
            piece_cap = to - from + 2;
            piece = (svpt *)malloc((u64)piece_cap * sizeof(svpt));
            if (!piece) return;
        }
        double off = s->dash_off * sc;
        off -= js_floor(off / total) * total;
        int di = 0;
        double left = s->dash[0] * sc;
        while (off > 0) {
            if (off >= left) { off -= left; di = (di + 1) % s->ndash; left = s->dash[di] * sc; }
            else { left -= off; off = 0; }
        }
        int np = 0;
        int on = !(di & 1);
        int segs = closed ? to - from : to - from - 1;
        if (on) piece[np++] = in->pts[from];
        for (int i = 0; i < segs; i++) {
            svpt a = in->pts[from + i], b = in->pts[from + (i + 1) % (to - from)];
            double dx = b.x - a.x, dy = b.y - a.y, len = js_sqrt(dx * dx + dy * dy), t = 0;
            while (len > 0 && t < len) {
                double step = left < len - t ? left : len - t;
                t += step;
                left -= step;
                svpt at;
                at.x = (float)(a.x + dx * t / len);
                at.y = (float)(a.y + dy * t / len);
                if (on && np < piece_cap) piece[np++] = at;
                if (left <= 1e-9) {
                    if (on && np > 1) jc_stroke_run(out, piece, np, 0, hw, s->cap, s->join, s->miter);
                    np = 0;
                    di = (di + 1) % s->ndash;
                    left = s->dash[di] * sc;
                    on = !(di & 1);
                    if (on) piece[np++] = at;
                }
            }
        }
        if (on && np > 1) jc_stroke_run(out, piece, np, 0, hw, s->cap, s->join, s->miter);
    }
    if (piece) free(piece);
}

/* --- text ------------------------------------------------------------------------------------ */

/* A script's text as the faces can draw it: each character as html.h writes
   it in ASCII, the way the page's own words are. */
static int jc_ascii(const jstr *s, char *out, int cap) {
    int w = 0;
    for (u32 i = 0; s && i < s->len && w < cap - 8; ) {
        unsigned c = (u8)s->s[i];
        unsigned cp = c;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) {
            cp = c & (0x3F >> extra);
            for (int k = 1; k <= extra && i + (u32)k < s->len; k++) cp = (cp << 6) | ((u8)s->s[i + (u32)k] & 0x3F);
        }
        i += (u32)extra + 1;
        if (cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f') cp = ' ';
        char tmp[8];
        int n = html_fold_cp(cp, tmp);
        for (int k = 0; k < n && w < cap - 1; k++) out[w++] = tmp[k];
    }
    out[w] = 0;
    return w;
}

/* The face for text px tall on the canvas, and how much its letters are
   scaled: not at all inside the faces' sizes (the nearest is drawn), and
   from the largest or smallest past them. */
static int jc_face_for(double px, int bold, double *scale) {
    const face_t *big = tface_of(face_pick(1000, bold, 0)), *small = tface_of(face_pick(0, bold, 0));
    int want = (int)(px + 0.5);
    int f = face_pick(want, bold, 0);
    *scale = 1;
    if (px > big->size * 1.1 || px < small->size * 0.9) *scale = px / tface_of(f)->size;
    return f;
}

typedef struct { int face; double scale, dev_px, width_dev, width_user; } jctext;

static void jc_text_measure(const jcstate *s, const char *txt, int n, jctext *m) {
    double sc = jc_scale(s);
    if (!(sc > 0)) sc = 1;
    m->dev_px = s->font_px * sc;
    m->face = jc_face_for(m->dev_px, s->bold, &m->scale);
    m->width_dev = tface_wn(txt, n, m->face) * m->scale;
    m->width_user = m->width_dev / sc;
}

/* Text drawn with its baseline's start at (x, y) on the canvas. */
static void jc_text_draw(jcdraw *dw, const char *txt, int n, double x, double y, const jctext *m) {
    const face_t *f = tface_of(m->face);
    double sc = m->scale, pen = x;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)txt[i];
        if (c < FACE_FIRST || c > FACE_LAST) c = ' ';
        const face_glyph *g = &f->glyphs[c - FACE_FIRST];
        const unsigned char *px = f->pixels + g->at;
        if (sc == 1) {
            int ox = (int)js_floor(pen + 0.5) + g->left, oy = (int)js_floor(y + 0.5) - g->top;
            for (int gy = 0; gy < g->h; gy++)
                for (int gx = 0; gx < g->w; gx++) {
                    unsigned char a = px[gy * g->w + gx];
                    if (a) jc_put(dw, ox + gx, oy + gy, a / 255.0);
                }
        } else {
            /* Scaled, each pixel of the letter's box on the canvas samples
               the face's four nearest. */
            double left = pen + g->left * sc, top = y - g->top * sc;
            int x0 = (int)js_floor(left), y0 = (int)js_floor(top);
            int x1 = (int)(left + g->w * sc) + 1, y1 = (int)(top + g->h * sc) + 1;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    double u = (xx + 0.5 - left) / sc - 0.5, v = (yy + 0.5 - top) / sc - 0.5;
                    int iu = (int)js_floor(u), iv = (int)js_floor(v);
                    double tx = u - iu, ty = v - iv, acc = 0;
                    for (int k = 0; k < 4; k++) {
                        int gx = iu + (k & 1), gy = iv + (k >> 1);
                        if (gx < 0 || gy < 0 || gx >= g->w || gy >= g->h) continue;
                        acc += px[gy * g->w + gx] * ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
                    }
                    if (acc > 0.5) jc_put(dw, xx, yy, acc / 255.0);
                }
        }
        pen += g->advance * sc;
    }
    dw->cv->drawn = 1;
    dw->cv->copy_ok = 0;
    jc_drawn_any = 1;
}

/* --- the canvases ------------------------------------------------------------------------- */

static void jc_bytes_copy(void *dst, const void *src, u32 n) {
    volatile u8 *d = (volatile u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)src;
    for (u32 i = 0; i < n; i++) d[i] = s[i];
}

static void jc_state_init(jcstate *s) {
    s->a = 1; s->b = 0; s->c = 0; s->d = 1; s->e = 0; s->f = 0;
    s->fill = s->stroke = 0xFF000000;
    s->fill_paint = s->stroke_paint = 0;
    s->line_width = 1;
    s->miter = 10;
    s->dash_off = 0;
    s->ndash = 0;
    s->cap = s->join = 0;
    s->op = JC_OVER;
    s->align = s->baseline = 0;
    s->bold = 0;
    s->alpha = 1;
    s->font_px = 10;
    const char *f = "10px sans-serif";
    int i = 0;
    for (; f[i]; i++) s->font[i] = f[i];
    s->font[i] = 0;
    s->clip = 0;
}

/* A width or height attribute read as the standard reads one: a
   non-negative integer, or the default when it is not one. */
static int jc_dim(int node, const char *name, int dflt) {
    const char *v = jd_attr(node, name);
    if (!v) return dflt;
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r' || *v == '\f') v++;
    if (*v == '+') v++;
    if (*v < '0' || *v > '9') return dflt;
    long long n = 0;
    while (*v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); if (n > 1000000) n = 1000000; v++; }
    return (int)n;
}

static void jc_mask_release(jcanvas *cv, int clip) {
    if (clip <= 0) return;
    int m = clip - 1;
    if (--cv->mask_refs[m] <= 0) {
        cv->mask_refs[m] = 0;
        if (cv->mask[m]) free(cv->mask[m]);
        cv->mask[m] = 0;
    }
}

static void jc_forget(jcanvas *cv) {
    for (int m = 0; m <= JC_STATES; m++) {
        if (cv->mask[m]) free(cv->mask[m]);
        cv->mask[m] = 0;
        cv->mask_refs[m] = 0;
    }
    jc_path_clear(&cv->path);
    cv->depth = 0;
    jc_state_init(&cv->st[0]);
}

static int jc_slot_of(int node) {
    for (int i = 0; i < JC_MAX; i++) if (jc_canvas[i].node == node && node >= 0) return i;
    return -1;
}

/* The bitmap made to match the element's width and height, cleared, and
   everything about drawing on it forgotten, when either was set or they no
   longer match. Too big a canvas has no bitmap, and drawing on it does
   nothing, as one a browser could not find the memory for. */
static void jc_sync(int i) {
    jcanvas *cv = &jc_canvas[i];
    int w = jc_dim(cv->node, "width", 300), h = jc_dim(cv->node, "height", 150);
    if (!cv->reset && w == cv->w && h == cv->h && (cv->px || !w || !h)) return;
    if (cv->px) {
        jc_bytes -= (long long)cv->w * cv->h * 4;
        free(cv->px);
        cv->px = 0;
    }
    if (cv->rgb) { free(cv->rgb); cv->rgb = 0; }
    if (cv->alpha) { free(cv->alpha); cv->alpha = 0; }
    cv->copy_ok = 0;
    cv->w = w;
    cv->h = h;
    long long need = (long long)w * h * 4;
    if (w > 0 && h > 0 && need <= JC_BYTES && jc_bytes + need <= JC_ALL) {
        cv->px = (u8 *)malloc((u64)need);
        if (cv->px) {
            for (long long k = 0; k < need; k++) cv->px[k] = 0;
            jc_bytes += need;
        }
    }
    cv->reset = 0;
    jc_forget(cv);
    cv->drawn = 1;
    jc_drawn_any = 1;
}

static int jc_open_slot(int node) {
    int i = jc_slot_of(node);
    if (i >= 0) return i;
    for (i = 0; i < JC_MAX; i++) {
        if (jc_canvas[i].node >= 0) continue;
        jcanvas *cv = &jc_canvas[i];
        cv->node = node;
        cv->w = cv->h = -1;
        cv->reset = 1;
        jc_sync(i);
        return i;
    }
    return -1;
}

static void jd_canvas_reset(void) {
    for (int i = 0; i < JC_MAX; i++) {
        jcanvas *cv = &jc_canvas[i];
        if (cv->px) free(cv->px);
        if (cv->rgb) free(cv->rgb);
        if (cv->alpha) free(cv->alpha);
        cv->px = cv->rgb = cv->alpha = 0;
        jc_forget(cv);
        jc_path_free(&cv->path);
        cv->node = -1;
        cv->w = cv->h = 0;
    }
    jc_bytes = 0;
    jc_drawn_any = 0;
}

/* The canvas a context draws on, brought up to date with its element. */
static jcanvas *jc_this(jval t) {
    if (!js_is_obj(t)) return 0;
    jval k = jd_kept(t.obj, jd_k_jc);
    if (k.t != JS_NUM) return 0;
    int i = (int)k.num;
    if (i < 0 || i >= JC_MAX || jc_canvas[i].node < 0) return 0;
    jc_sync(i);
    return &jc_canvas[i];
}

static jcstate *jc_st(jcanvas *cv) { return &cv->st[cv->depth]; }

/* --- the context's paints, kept where the collector looks ------------------------------ */

static jobj *jc_paints(jctx *J, jobj *ctx) {
    jval v = jd_kept(ctx, jd_k_jcpaints);
    if (v.t == JS_OBJ && v.obj->kind == JO_ARRAY) return v.obj;
    jobj *arr = js_array(J);
    if (arr) jd_keep(ctx, jd_k_jcpaints, js_from_obj(arr));
    return arr;
}

/* Where a state's fill (0) or stroke (1) gradient or pattern is kept. */
static int jc_paint_slot(const jcanvas *cv, int which) { return cv->depth * 2 + which; }

static jval jc_paint_get(jctx *J, jobj *ctx, const jcanvas *cv, int which) {
    jobj *arr = jc_paints(J, ctx);
    u32 at = (u32)jc_paint_slot(cv, which);
    if (!arr || at >= arr->len) return js_undef();
    return arr->items[at];
}

static void jc_paint_put(jctx *J, jobj *ctx, const jcanvas *cv, int which, jval v) {
    jobj *arr = jc_paints(J, ctx);
    if (arr) js_arr_set(J, arr, (u32)jc_paint_slot(cv, which), v);
}

static double jc_item_num(jobj *arr, u32 i) {
    if (!arr || i >= arr->len || arr->items[i].t != JS_NUM) return 0;
    return arr->items[i].num;
}

/* The paint for a fill or stroke worked out from the state: its colour, or
   its gradient's colours along it and where it lies, or its pattern's
   pixels. */
static int jc_paint_of(jctx *J, jobj *ctx, jcanvas *cv, int which, jcpaint *pt) {
    jcstate *s = jc_st(cv);
    pt->kind = JP_COLOUR;
    pt->colour = which ? s->stroke : s->fill;
    pt->inv.a = s->a; pt->inv.b = s->b; pt->inv.c = s->c; pt->inv.d = s->d; pt->inv.e = s->e; pt->inv.f = s->f;
    double det = s->a * s->d - s->b * s->c;
    pt->has_inv = det != 0 && jc_finite(det);
    if (!(which ? s->stroke_paint : s->fill_paint)) return 1;
    jval v = jc_paint_get(J, ctx, cv, which);
    if (!js_is_obj(v)) return 1;
    jval g = jd_kept(v.obj, jd_k_grad);
    if (g.t == JS_OBJ && g.obj->kind == JO_ARRAY) {
        pt->kind = (int)jc_item_num(g.obj, 0);
        pt->x0 = jc_item_num(g.obj, 1); pt->y0 = jc_item_num(g.obj, 2); pt->r0 = jc_item_num(g.obj, 3);
        pt->x1 = jc_item_num(g.obj, 4); pt->y1 = jc_item_num(g.obj, 5); pt->r1 = jc_item_num(g.obj, 6);
        pt->angle = jc_item_num(g.obj, 7);
        double off[JC_STOPS];
        u32 col[JC_STOPS];
        int n = 0;
        jval sv = jd_kept(v.obj, jd_k_stops);
        if (sv.t == JS_OBJ && sv.obj->kind == JO_ARRAY)
            for (u32 i = 0; i + 1 < sv.obj->len && n < JC_STOPS; i += 2) {
                /* In order of offset, those at the same one kept in the order added. */
                double o = jc_item_num(sv.obj, i);
                u32 c = (u32)jc_item_num(sv.obj, i + 1);
                int k = n++;
                while (k > 0 && off[k - 1] > o) { off[k] = off[k - 1]; col[k] = col[k - 1]; k--; }
                off[k] = o;
                col[k] = c;
            }
        if (!n) { pt->kind = JP_COLOUR; pt->colour = 0; return 1; }
        for (int i = 0; i < 256; i++) pt->lut[i] = jc_stop_colour(off, col, n, i / 255.0);
        return 1;
    }
    jval pv = jd_kept(v.obj, jd_k_pat);
    jval px = jd_kept(v.obj, jd_k_patpx);
    if (pv.t == JS_OBJ && pv.obj->kind == JO_ARRAY && px.t == JS_STR) {
        pt->kind = JP_IMAGE;
        pt->sw = (int)jc_item_num(pv.obj, 0);
        pt->sh = (int)jc_item_num(pv.obj, 1);
        pt->rep_x = (int)jc_item_num(pv.obj, 2);
        pt->rep_y = (int)jc_item_num(pv.obj, 3);
        pt->src = (const u8 *)px.str->s;
        pt->src_alpha = 0;
        pt->bpp = 4;
        pt->smooth = 0;
        if ((u64)px.str->len < (u64)pt->sw * (u64)pt->sh * 4 || !pt->has_inv) { pt->kind = JP_COLOUR; pt->colour = 0; return 1; }
        /* The pattern lies in the page's coordinates under the transform as it is now. */
        pt->ia = s->d / det; pt->ic = -s->c / det; pt->ie = (s->c * s->f - s->d * s->e) / det;
        pt->ib = -s->b / det; pt->id = s->a / det; pt->iff = (s->b * s->e - s->a * s->f) / det;
        return 1;
    }
    return 1;
}

static void jc_draw_setup(jcanvas *cv, jcdraw *dw, jcpaint *pt) {
    jcstate *s = jc_st(cv);
    dw->cv = cv;
    dw->paint = pt;
    dw->alpha = s->alpha;
    dw->op = s->op < JC_OPS ? s->op : JC_OVER;
    dw->mask = s->clip ? cv->mask[s->clip - 1] : 0;
}

/* --- Path2D ------------------------------------------------------------------------------- */

/* A Path2D keeps what was said to it, in the page's coordinates, as numbers:
   each command's code and then its arguments, replayed into a path under
   the transform of the context that draws it. An SVG path's text is kept
   beside them and read by svg.h when it is replayed. */
enum { P2_MOVE = 1, P2_LINE, P2_CLOSE, P2_QUAD, P2_CUBIC, P2_ARCTO, P2_RECT, P2_RRECT, P2_ARC, P2_ELLIPSE,
       P2_SVG, P2_PUSH, P2_POP };
static const int P2_ARGS[] = { 0, 2, 2, 0, 4, 6, 5, 4, 8, 6, 8, 1, 6, 0 };

static svpath *jc_svg_scratch;

static void jc_replay(jctx *J, jobj *ops, jobj *texts, jcpath *out, const jcstate *base) {
    jcstate stack[8];
    int sp = 0;
    jcstate s;
    s.a = base->a; s.b = base->b; s.c = base->c; s.d = base->d; s.e = base->e; s.f = base->f;
    u32 i = 0;
    (void)J;
    while (ops && i < ops->len) {
        int op = (int)jc_item_num(ops, i++);
        if (op < 1 || op > P2_POP) break;
        double v[8];
        for (int k = 0; k < P2_ARGS[op]; k++) v[k] = jc_item_num(ops, i++);
        switch (op) {
        case P2_MOVE: jc_move(out, &s, v[0], v[1]); break;
        case P2_LINE: jc_line(out, &s, v[0], v[1]); break;
        case P2_CLOSE: jc_close(out); break;
        case P2_QUAD: jc_quad(out, &s, v[0], v[1], v[2], v[3]); break;
        case P2_CUBIC: jc_cubic(out, &s, v[0], v[1], v[2], v[3], v[4], v[5]); break;
        case P2_ARCTO: jc_arc_to(out, &s, v[0], v[1], v[2], v[3], v[4]); break;
        case P2_RECT: jc_rect(out, &s, v[0], v[1], v[2], v[3]); break;
        case P2_RRECT: jc_round_rect(out, &s, v[0], v[1], v[2], v[3], v + 4); break;
        case P2_ARC: jc_ellipse(out, &s, v[0], v[1], v[2], v[2], 0, v[3], v[4], v[5] != 0); break;
        case P2_ELLIPSE: jc_ellipse(out, &s, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7] != 0); break;
        case P2_SVG: {
            u32 t = (u32)v[0];
            if (!texts || t >= texts->len || texts->items[t].t != JS_STR) break;
            if (!jc_svg_scratch) jc_svg_scratch = (svpath *)malloc(sizeof(svpath));
            if (!jc_svg_scratch) break;
            svpath *sp2 = jc_svg_scratch;
            sp2->n = 0;
            sp2->nsub = 0;
            svmat m;
            m.a = (float)s.a; m.b = (float)s.b; m.c = (float)s.c; m.d = (float)s.d; m.e = (float)s.e; m.f = (float)s.f;
            sv_path_data(sp2, m, texts->items[t].str->s);
            for (int k = 0; k < sp2->nsub; k++) {
                int from = sp2->start[k], to = k + 1 < sp2->nsub ? sp2->start[k + 1] : sp2->n;
                if (to <= from) continue;
                jc_path_new_sub(out, sp2->pts[from].x, sp2->pts[from].y);
                for (int q = from + 1; q < to; q++) jc_add_dev(out, sp2->pts[q].x, sp2->pts[q].y);
                if (to - from > 2 && sp2->pts[from].x == sp2->pts[to - 1].x && sp2->pts[from].y == sp2->pts[to - 1].y)
                    jc_close(out);
            }
            break;
        }
        case P2_PUSH:
            if (sp < 8) {
                stack[sp].a = s.a; stack[sp].b = s.b; stack[sp].c = s.c;
                stack[sp].d = s.d; stack[sp].e = s.e; stack[sp].f = s.f;
                sp++;
                /* What was added with a transform is drawn under it, within
                   the context's. */
                double a = s.a * v[0] + s.c * v[1], b = s.b * v[0] + s.d * v[1];
                double c = s.a * v[2] + s.c * v[3], d = s.b * v[2] + s.d * v[3];
                double e = s.a * v[4] + s.c * v[5] + s.e, f = s.b * v[4] + s.d * v[5] + s.f;
                s.a = a; s.b = b; s.c = c; s.d = d; s.e = e; s.f = f;
            }
            break;
        case P2_POP:
            if (sp > 0) {
                sp--;
                s.a = stack[sp].a; s.b = stack[sp].b; s.c = stack[sp].c;
                s.d = stack[sp].d; s.e = stack[sp].e; s.f = stack[sp].f;
            }
            break;
        }
    }
}

static int jc_is_path2d(jval v) {
    if (!js_is_obj(v)) return 0;
    jval k = jd_kept(v.obj, jd_k_p2d);
    return k.t == JS_OBJ && k.obj->kind == JO_ARRAY;
}

static jobj *jc_p2d_ops(jval v) { return jd_kept(v.obj, jd_k_p2d).obj; }

static jobj *jc_p2d_texts(jctx *J, jval v, int make) {
    jval k = jd_kept(v.obj, jd_k_patpx);
    if (k.t == JS_OBJ && k.obj->kind == JO_ARRAY) return k.obj;
    if (!make) return 0;
    jobj *arr = js_array(J);
    if (arr) jd_keep(v.obj, jd_k_patpx, js_from_obj(arr));
    return arr;
}

static jcpath jc_tmp, jc_tmp2;

/* The path a fill, stroke or clip is of: a Path2D's, made under the
   context's transform, or the context's own. */
static const jcpath *jc_path_arg(jctx *J, jcanvas *cv, jval *a, int n, int *argi) {
    if (n > 0 && jc_is_path2d(a[0])) {
        jc_path_clear(&jc_tmp);
        jc_replay(J, jc_p2d_ops(a[0]), jc_p2d_texts(J, a[0], 0), &jc_tmp, jc_st(cv));
        *argi = 1;
        return &jc_tmp;
    }
    *argi = 0;
    return &cv->path;
}

static int jc_rule_arg(jctx *J, jval *a, int n, int i) {
    if (i >= n || a[i].t != JS_STR) return 0;
    (void)J;
    return js_str_is(a[i].str, "evenodd");
}

/* --- numbers and words from the page ------------------------------------------------------ */

static double jc_num(jctx *J, jval *a, int n, int i) { return js_to_num(J, js_arg(a, n, i)); }

static int jc_word(const jstr *s, const char *const *words) {
    for (int i = 0; s && words[i]; i++) if (js_str_is(s, words[i])) return i;
    return -1;
}

static const char *const JC_BLENDS[] = {
    "multiply", "screen", "overlay", "darken", "lighten", "color-dodge", "color-burn", "hard-light",
    "soft-light", "difference", "exclusion", "hue", "saturation", "color", "luminosity", 0
};

/* A CSS font for the canvas: its size in pixels and whether it is bold, or
   0 when it is not a font (and the setting is left as it was). */
static int jc_font_parse(const char *f, int *px, int *bold) {
    *bold = 0;
    const char *p = f;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *w = p;
        while (*p && *p != ' ') p++;
        int len = (int)(p - w);
        if ((len == 4 && w_starts_fold(w, "bold")) || (len == 6 && w_starts_fold(w, "bolder"))
            || (len == 3 && w[0] >= '6' && w[0] <= '9' && w[1] == '0' && w[2] == '0')) { *bold = 1; continue; }
        if (*w >= '0' && *w <= '9') {
            double v = 0, frac = 0, div = 1;
            const char *q = w;
            while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
            if (*q == '.') { q++; while (*q >= '0' && *q <= '9') { frac = frac * 10 + (*q - '0'); div *= 10; q++; } }
            v += frac / div;
            if (w_starts_fold(q, "px")) *px = (int)(v + 0.5);
            else if (w_starts_fold(q, "pt")) *px = (int)(v * 4 / 3 + 0.5);
            else if (w_starts_fold(q, "rem") || w_starts_fold(q, "em")) *px = (int)(v * 16 + 0.5);
            else if (*q == '%') *px = (int)(v * 16 / 100 + 0.5);
            else continue;                          /* a weight such as 400 */
            if (*px < 1) *px = 1;
            if (*px > 1000) *px = 1000;
            /* The family has to follow the size. */
            while (*p == ' ') p++;
            return *p != 0;
        }
    }
    return 0;
}

/* --- the context's methods ---------------------------------------------------------------- */

#define JC_THIS(cv) jcanvas *cv = jc_this(t); if (!cv) return jd_illegal(J)

static jval nat_jc_save(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    JC_THIS(cv);
    if (cv->depth + 1 >= JC_STATES) return js_undef();
    jc_bytes_copy(&cv->st[cv->depth + 1], &cv->st[cv->depth], (u32)sizeof(jcstate));
    for (int w = 0; w < 2; w++) {
        jval pv = jc_paint_get(J, t.obj, cv, w);
        cv->depth++;
        jc_paint_put(J, t.obj, cv, w, pv);
        cv->depth--;
    }
    cv->depth++;
    if (jc_st(cv)->clip) cv->mask_refs[jc_st(cv)->clip - 1]++;
    return js_undef();
}

static jval nat_jc_restore(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    JC_THIS(cv);
    if (cv->depth <= 0) return js_undef();
    jc_mask_release(cv, jc_st(cv)->clip);
    jc_paint_put(J, t.obj, cv, 0, js_undef());
    jc_paint_put(J, t.obj, cv, 1, js_undef());
    cv->depth--;
    return js_undef();
}

/* reset(): the bitmap cleared and the context as it was made. */
static jval nat_jc_reset(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    JC_THIS(cv);
    cv->reset = 1;
    jc_sync((int)(cv - jc_canvas));
    return js_undef();
}

static void jc_mul(jcstate *s, double a, double b, double c, double d, double e, double f) {
    double na = s->a * a + s->c * b, nb = s->b * a + s->d * b;
    double nc = s->a * c + s->c * d, nd = s->b * c + s->d * d;
    double ne = s->a * e + s->c * f + s->e, nf = s->b * e + s->d * f + s->f;
    s->a = na; s->b = nb; s->c = nc; s->d = nd; s->e = ne; s->f = nf;
}

static jval nat_jc_translate(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    double x = jc_num(J, a, n, 0), y = jc_num(J, a, n, 1);
    if (jc_finite(x) && jc_finite(y)) jc_mul(jc_st(cv), 1, 0, 0, 1, x, y);
    return js_undef();
}

static jval nat_jc_scale(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    double x = jc_num(J, a, n, 0), y = jc_num(J, a, n, 1);
    if (jc_finite(x) && jc_finite(y)) jc_mul(jc_st(cv), x, 0, 0, y, 0, 0);
    return js_undef();
}

static jval nat_jc_rotate(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    double r = jc_num(J, a, n, 0);
    if (jc_finite(r)) { double c = jc_cos(r), s = jc_sin(r); jc_mul(jc_st(cv), c, s, -s, c, 0, 0); }
    return js_undef();
}

static int jc_six(jctx *J, jval *a, int n, double *v) {
    for (int i = 0; i < 6; i++) { v[i] = jc_num(J, a, n, i); if (!jc_finite(v[i])) return 0; }
    return 1;
}

static jval nat_jc_transform(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    double v[6];
    if (jc_six(J, a, n, v)) jc_mul(jc_st(cv), v[0], v[1], v[2], v[3], v[4], v[5]);
    return js_undef();
}

static jval nat_jc_set_transform(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    double v[6];
    if (n >= 6) {
        if (!jc_six(J, a, n, v)) return js_undef();
    } else {
        /* A matrix, or a dictionary of one's numbers, or nothing (none at all). */
        dmat m;
        if (!dmx_from_value(J, js_arg(a, n, 0), &m)) return js_throw(J, JS_ERR_TYPE, "that is not a matrix", J->error_line);
        v[0] = m.m[0]; v[1] = m.m[1]; v[2] = m.m[4]; v[3] = m.m[5]; v[4] = m.m[12]; v[5] = m.m[13];
        for (int i = 0; i < 6; i++) if (!jc_finite(v[i])) return js_undef();
    }
    s->a = v[0]; s->b = v[1]; s->c = v[2]; s->d = v[3]; s->e = v[4]; s->f = v[5];
    return js_undef();
}

static jval nat_jc_reset_transform(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    s->a = 1; s->b = 0; s->c = 0; s->d = 1; s->e = 0; s->f = 0;
    return js_undef();
}

static jval nat_jc_get_transform(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    dmat m;
    dmx_identity(&m);
    m.m[0] = s->a; m.m[1] = s->b; m.m[4] = s->c; m.m[5] = s->d; m.m[12] = s->e; m.m[13] = s->f;
    return dmx_new(J, &m, 0);
}

/* fillStyle and strokeStyle: a colour, given back as the standard writes
   one, or the gradient or pattern that was given. */
static jval jc_style_get(jctx *J, jval t, int which) {
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    if (which ? s->stroke_paint : s->fill_paint) return jc_paint_get(J, t.obj, cv, which);
    char buf[48];
    jc_colour_text(which ? s->stroke : s->fill, buf, sizeof(buf));
    return jd_str(buf);
}

static jval jc_style_set(jctx *J, jval t, jval v, int which) {
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    if (js_is_obj(v)) {
        jval g = jd_kept(v.obj, jd_k_grad), p = jd_kept(v.obj, jd_k_pat);
        if (g.t != JS_OBJ && p.t != JS_OBJ) return js_undef();
        jc_paint_put(J, t.obj, cv, which, v);
        if (which) s->stroke_paint = 1; else s->fill_paint = 1;
        return js_undef();
    }
    jstr *str = js_to_str(J, v);
    u32 c;
    if (!str || !jc_parse_colour(str->s, &c)) return js_undef();
    if (which) { s->stroke = c; s->stroke_paint = 0; } else { s->fill = c; s->fill_paint = 0; }
    jc_paint_put(J, t.obj, cv, which, js_undef());
    return js_undef();
}

static jval nat_jc_fill_get(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jc_style_get(J, t, 0); }
static jval nat_jc_fill_set(jctx *J, jval t, jval *a, int n) { return jc_style_set(J, t, js_arg(a, n, 0), 0); }
static jval nat_jc_stroke_get(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jc_style_get(J, t, 1); }
static jval nat_jc_stroke_set(jctx *J, jval t, jval *a, int n) { return jc_style_set(J, t, js_arg(a, n, 0), 1); }

/* The numbers and words of the state, each refusing what the standard
   says to leave as it was. */
enum { JF_WIDTH, JF_MITER, JF_ALPHA, JF_DASHOFF, JF_CAP, JF_JOIN, JF_OP, JF_ALIGN, JF_BASELINE, JF_FONT };

static jval nat_jc_field(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    switch ((int)J->callee->data.num) {
    case JF_WIDTH: return js_num(s->line_width);
    case JF_MITER: return js_num(s->miter);
    case JF_ALPHA: return js_num(s->alpha);
    case JF_DASHOFF: return js_num(s->dash_off);
    case JF_CAP: return jd_str(JC_CAPS[s->cap]);
    case JF_JOIN: return jd_str(JC_JOINS[s->join]);
    case JF_OP: return jd_str(s->op < JC_OPS ? JC_OP_NAMES[s->op] : JC_BLENDS[s->op - JC_OPS]);
    case JF_ALIGN: return jd_str(JC_ALIGNS[s->align]);
    case JF_BASELINE: return jd_str(JC_BASELINES[s->baseline]);
    case JF_FONT: return jd_str(s->font);
    }
    return js_undef();
}

static jval nat_jc_field_set(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    int f = (int)J->callee->data.num;
    jval v = js_arg(a, n, 0);
    if (f <= JF_DASHOFF) {
        double x = js_to_num(J, v);
        if (!jc_finite(x)) return js_undef();
        if (f == JF_WIDTH && x > 0) s->line_width = x;
        if (f == JF_MITER && x > 0) s->miter = x;
        if (f == JF_ALPHA && x >= 0 && x <= 1) s->alpha = x;
        if (f == JF_DASHOFF) s->dash_off = x;
        return js_undef();
    }
    jstr *w = js_to_str(J, v);
    if (!w) return js_undef();
    int k;
    switch (f) {
    case JF_CAP: if ((k = jc_word(w, JC_CAPS)) >= 0) s->cap = (u8)k; break;
    case JF_JOIN: if ((k = jc_word(w, JC_JOINS)) >= 0) s->join = (u8)k; break;
    case JF_OP:
        if ((k = jc_word(w, JC_OP_NAMES)) >= 0) s->op = (u8)k;
        else if ((k = jc_word(w, JC_BLENDS)) >= 0) s->op = (u8)(JC_OPS + k);   /* drawn as source-over */
        break;
    case JF_ALIGN: if ((k = jc_word(w, JC_ALIGNS)) >= 0) s->align = (u8)k; break;
    case JF_BASELINE: if ((k = jc_word(w, JC_BASELINES)) >= 0) s->baseline = (u8)k; break;
    case JF_FONT: {
        int px, bold;
        if (w->len >= sizeof(s->font) || !jc_font_parse(w->s, &px, &bold)) break;
        s->font_px = px;
        s->bold = (u8)bold;
        u32 i = 0;
        const char *src = w->s;
        while (*src == ' ') src++;
        for (; src[i] && i < sizeof(s->font) - 1; i++) s->font[i] = src[i];
        while (i > 0 && s->font[i - 1] == ' ') i--;
        s->font[i] = 0;
        break;
    }
    }
    return js_undef();
}

static jval nat_jc_set_dash(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    jval v = js_arg(a, n, 0);
    if (!js_is_obj(v)) return js_throw(J, JS_ERR_TYPE, "setLineDash takes a list", J->error_line);
    double d[16];
    int k = 0;
    jval len = js_get(J, v, js_str(J, "length"));
    int count = (int)js_to_num(J, len);
    if (count < 0 || count > 8) count = count < 0 ? 0 : 8;
    for (int i = 0; i < count; i++) {
        char key[8];
        int m = 0, x = i;
        if (!x) key[m++] = '0';
        char rev[8]; int r = 0;
        while (x) { rev[r++] = (char)('0' + x % 10); x /= 10; }
        while (r) key[m++] = rev[--r];
        key[m] = 0;
        double num = js_to_num(J, js_get(J, v, js_str(J, key)));
        if (!jc_finite(num) || num < 0) return js_undef();
        d[k++] = num;
    }
    /* An odd number of lengths is said twice. */
    int total = k & 1 ? k * 2 : k;
    for (int i = 0; i < total; i++) s->dash[i] = d[i % k];
    s->ndash = total;
    return js_undef();
}

static jval nat_jc_get_dash(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    jobj *arr = js_array(J);
    if (!arr) return js_undef();
    for (int i = 0; i < s->ndash; i++) js_arr_push(J, arr, js_num(s->dash[i]));
    return js_from_obj(arr);
}

/* --- making paths ----------------------------------------------------------------------- */

/* The same commands for a context's own path and for a Path2D's: a context
   draws as it goes, a Path2D writes them down. */
static jval jc_path_cmd(jctx *J, jval t, jval *a, int n, int op) {
    double v[8];
    int need = P2_ARGS[op];
    for (int i = 0; i < need && i < 8; i++) v[i] = jc_num(J, a, n, i);
    if (op == P2_ARC) {
        v[5] = js_to_bool(js_arg(a, n, 5));
        if (jc_finite(v[2]) && v[2] < 0) return js_throw_dom(J, "IndexSizeError", "an arc's radius is not below 0");
    }
    if (op == P2_ELLIPSE) {
        v[7] = js_to_bool(js_arg(a, n, 7));
        if ((jc_finite(v[2]) && v[2] < 0) || (jc_finite(v[3]) && v[3] < 0))
            return js_throw_dom(J, "IndexSizeError", "an ellipse's radii are not below 0");
    }
    if (op == P2_ARCTO && jc_finite(v[4]) && v[4] < 0) return js_throw_dom(J, "IndexSizeError", "arcTo's radius is not below 0");
    if (op == P2_RRECT) {
        /* Radii: a number, or a list of one to four. */
        jval r = js_arg(a, n, 4);
        double rr[4] = { 0, 0, 0, 0 };
        if (js_is_obj(r) && r.obj->kind == JO_ARRAY) {
            int k = (int)r.obj->len;
            double q[4];
            for (int i = 0; i < k && i < 4; i++) q[i] = js_to_num(J, r.obj->items[i]);
            if (k == 1) rr[0] = rr[1] = rr[2] = rr[3] = q[0];
            else if (k == 2) { rr[0] = rr[2] = q[0]; rr[1] = rr[3] = q[1]; }
            else if (k == 3) { rr[0] = q[0]; rr[1] = rr[3] = q[1]; rr[2] = q[2]; }
            else if (k >= 4) { rr[0] = q[0]; rr[1] = q[1]; rr[2] = q[2]; rr[3] = q[3]; }
        } else if (r.t != JS_UNDEF) {
            rr[0] = rr[1] = rr[2] = rr[3] = js_to_num(J, r);
        }
        for (int i = 0; i < 4; i++) {
            if (!jc_finite(rr[i])) return js_undef();
            if (rr[i] < 0) return js_throw(J, JS_ERR_RANGE, "a corner's radius is not below 0", J->error_line);
            v[4 + i] = rr[i];
        }
    }
    if (jc_is_path2d(t)) {
        jobj *ops = jc_p2d_ops(t);
        js_arr_push(J, ops, js_num(op));
        for (int i = 0; i < need; i++) js_arr_push(J, ops, js_num(v[i]));
        return js_undef();
    }
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    jcpath *p = &cv->path;
    switch (op) {
    case P2_MOVE: jc_move(p, s, v[0], v[1]); break;
    case P2_LINE: jc_line(p, s, v[0], v[1]); break;
    case P2_CLOSE: jc_close(p); break;
    case P2_QUAD: jc_quad(p, s, v[0], v[1], v[2], v[3]); break;
    case P2_CUBIC: jc_cubic(p, s, v[0], v[1], v[2], v[3], v[4], v[5]); break;
    case P2_ARCTO: jc_arc_to(p, s, v[0], v[1], v[2], v[3], v[4]); break;
    case P2_RECT: jc_rect(p, s, v[0], v[1], v[2], v[3]); break;
    case P2_RRECT: jc_round_rect(p, s, v[0], v[1], v[2], v[3], v + 4); break;
    case P2_ARC: jc_ellipse(p, s, v[0], v[1], v[2], v[2], 0, v[3], v[4], v[5] != 0); break;
    case P2_ELLIPSE: jc_ellipse(p, s, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7] != 0); break;
    }
    return js_undef();
}

static jval nat_jc_move(jctx *J, jval t, jval *a, int n) { return jc_path_cmd(J, t, a, n, P2_MOVE); }
static jval nat_jc_line(jctx *J, jval t, jval *a, int n) { return jc_path_cmd(J, t, a, n, P2_LINE); }
static jval nat_jc_closepath(jctx *J, jval t, jval *a, int n) { return jc_path_cmd(J, t, a, n, P2_CLOSE); }
static jval nat_jc_quad(jctx *J, jval t, jval *a, int n) { return jc_path_cmd(J, t, a, n, P2_QUAD); }
static jval nat_jc_cubic(jctx *J, jval t, jval *a, int n) { return jc_path_cmd(J, t, a, n, P2_CUBIC); }
static jval nat_jc_arcto(jctx *J, jval t, jval *a, int n) { return jc_path_cmd(J, t, a, n, P2_ARCTO); }
static jval nat_jc_rect(jctx *J, jval t, jval *a, int n) { return jc_path_cmd(J, t, a, n, P2_RECT); }
static jval nat_jc_rrect(jctx *J, jval t, jval *a, int n) { return jc_path_cmd(J, t, a, n, P2_RRECT); }
static jval nat_jc_arc(jctx *J, jval t, jval *a, int n) { return jc_path_cmd(J, t, a, n, P2_ARC); }
static jval nat_jc_ellipse(jctx *J, jval t, jval *a, int n) { return jc_path_cmd(J, t, a, n, P2_ELLIPSE); }

static jval nat_jc_begin(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    JC_THIS(cv);
    jc_path_clear(&cv->path);
    return js_undef();
}

static jval nat_path2d_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t)) return js_throw(J, JS_ERR_TYPE, "Path2D is made with new", J->error_line);
    jobj *ops = js_array(J);
    if (!ops) return js_undef();
    jd_keep(t.obj, jd_k_p2d, js_from_obj(ops));
    jval from = js_arg(a, n, 0);
    if (jc_is_path2d(from)) {
        jobj *o = jc_p2d_ops(from);
        for (u32 i = 0; i < o->len; i++) js_arr_push(J, ops, o->items[i]);
        jobj *tx = jc_p2d_texts(J, from, 0);
        if (tx) {
            jobj *mine = jc_p2d_texts(J, t, 1);
            for (u32 i = 0; mine && i < tx->len; i++) js_arr_push(J, mine, tx->items[i]);
        }
    } else if (from.t == JS_STR) {
        jobj *texts = jc_p2d_texts(J, t, 1);
        if (texts) {
            js_arr_push(J, ops, js_num(P2_SVG));
            js_arr_push(J, ops, js_num(texts->len));
            js_arr_push(J, texts, from);
        }
    }
    return js_undef();
}

static jval nat_path2d_add(jctx *J, jval t, jval *a, int n) {
    if (!jc_is_path2d(t)) return jd_illegal(J);
    jval from = js_arg(a, n, 0);
    if (!jc_is_path2d(from)) return js_throw(J, JS_ERR_TYPE, "addPath takes a Path2D", J->error_line);
    jobj *ops = jc_p2d_ops(t), *o = jc_p2d_ops(from);
    dmat m;
    if (!dmx_from_value(J, js_arg(a, n, 1), &m)) return js_throw(J, JS_ERR_TYPE, "that is not a matrix", J->error_line);
    double six[6] = { m.m[0], m.m[1], m.m[4], m.m[5], m.m[12], m.m[13] };
    js_arr_push(J, ops, js_num(P2_PUSH));
    for (int i = 0; i < 6; i++) js_arr_push(J, ops, js_num(six[i]));
    /* The other's SVG texts are carried across, renumbered. */
    jobj *tx = jc_p2d_texts(J, from, 0), *mine = tx ? jc_p2d_texts(J, t, 1) : 0;
    u32 base = mine ? mine->len : 0;
    for (u32 i = 0; mine && i < tx->len; i++) js_arr_push(J, mine, tx->items[i]);
    u32 i = 0;
    while (i < o->len) {
        int op = (int)jc_item_num(o, i);
        if (op < 1 || op > P2_POP) break;
        js_arr_push(J, ops, o->items[i++]);
        for (int k = 0; k < P2_ARGS[op] && i < o->len; k++, i++)
            js_arr_push(J, ops, op == P2_SVG ? js_num(jc_item_num(o, i) + base) : o->items[i]);
    }
    js_arr_push(J, ops, js_num(P2_POP));
    return js_undef();
}

/* --- drawing ---------------------------------------------------------------------------- */

static jval nat_jc_fill(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    if (!cv->px) return js_undef();
    int argi;
    const jcpath *p = jc_path_arg(J, cv, a, n, &argi);
    jcpaint pt;
    jcdraw dw;
    jc_paint_of(J, t.obj, cv, 0, &pt);
    jc_draw_setup(cv, &dw, &pt);
    jc_fill_path(&dw, p, jc_rule_arg(J, a, n, argi));
    return js_undef();
}

static jval nat_jc_stroke(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    if (!cv->px) return js_undef();
    int argi;
    const jcpath *p = jc_path_arg(J, cv, a, n, &argi);
    jc_path_clear(&jc_tmp2);
    jc_stroke_shapes(&jc_tmp2, p, jc_st(cv));
    jcpaint pt;
    jcdraw dw;
    jc_paint_of(J, t.obj, cv, 1, &pt);
    jc_draw_setup(cv, &dw, &pt);
    jc_fill_path(&dw, &jc_tmp2, 0);
    return js_undef();
}

static int jc_rect_args(jctx *J, jval *a, int n, double *v) {
    for (int i = 0; i < 4; i++) { v[i] = jc_num(J, a, n, i); if (!jc_finite(v[i])) return 0; }
    return 1;
}

static jval nat_jc_fill_rect(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    double v[4];
    if (!cv->px || !jc_rect_args(J, a, n, v) || !v[2] || !v[3]) return js_undef();
    jc_path_clear(&jc_tmp);
    jc_rect(&jc_tmp, jc_st(cv), v[0], v[1], v[2], v[3]);
    jcpaint pt;
    jcdraw dw;
    jc_paint_of(J, t.obj, cv, 0, &pt);
    jc_draw_setup(cv, &dw, &pt);
    jc_fill_path(&dw, &jc_tmp, 0);
    return js_undef();
}

static jval nat_jc_stroke_rect(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    double v[4];
    if (!cv->px || !jc_rect_args(J, a, n, v)) return js_undef();
    jc_path_clear(&jc_tmp);
    jc_rect(&jc_tmp, jc_st(cv), v[0], v[1], v[2], v[3]);
    jc_path_clear(&jc_tmp2);
    jc_stroke_shapes(&jc_tmp2, &jc_tmp, jc_st(cv));
    jcpaint pt;
    jcdraw dw;
    jc_paint_of(J, t.obj, cv, 1, &pt);
    jc_draw_setup(cv, &dw, &pt);
    jc_fill_path(&dw, &jc_tmp2, 0);
    return js_undef();
}

/* clearRect: transparent black through the clip, whatever the alpha and the
   operation say. */
static jval nat_jc_clear_rect(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    double v[4];
    if (!cv->px || !jc_rect_args(J, a, n, v) || !v[2] || !v[3]) return js_undef();
    jc_path_clear(&jc_tmp);
    jc_rect(&jc_tmp, jc_st(cv), v[0], v[1], v[2], v[3]);
    jcpaint pt;
    jcdraw dw;
    pt.kind = JP_COLOUR;
    pt.colour = 0;
    jc_draw_setup(cv, &dw, &pt);
    dw.alpha = 1;
    dw.op = JC_COPY;
    jc_fill_path(&dw, &jc_tmp, 0);
    return js_undef();
}

typedef struct { u8 *mask; const u8 *old; int w; } jcclip;

static void jc_clip_row(void *ctx, int y, const float *cov, int x0, int x1) {
    jcclip *c = (jcclip *)ctx;
    for (int x = x0; x < x1; x++) {
        double v = cov[x] > 1 ? 1 : cov[x];
        int old = c->old ? c->old[(u64)y * (u64)c->w + (u64)x] : 255;
        c->mask[(u64)y * (u64)c->w + (u64)x] = (u8)(old * v + 0.5);
    }
}

static jval nat_jc_clip(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    if (!cv->px) return js_undef();
    int argi;
    const jcpath *p = jc_path_arg(J, cv, a, n, &argi);
    jcstate *s = jc_st(cv);
    int slot = -1;
    for (int m = 0; m <= JC_STATES; m++) if (!cv->mask[m]) { slot = m; break; }
    if (slot < 0) return js_undef();
    u64 bytes = (u64)cv->w * (u64)cv->h;
    u8 *mask = (u8 *)malloc(bytes);
    if (!mask) return js_undef();
    for (u64 k = 0; k < bytes; k++) mask[k] = 0;
    jcclip c;
    c.mask = mask;
    c.old = s->clip ? cv->mask[s->clip - 1] : 0;
    c.w = cv->w;
    if (p->n && p->nsub)
        sv_cover(cv->w, cv->h, p->pts, p->n, p->start, p->nsub, jc_rule_arg(J, a, n, argi), jc_clip_row, &c);
    jc_mask_release(cv, s->clip);
    cv->mask[slot] = mask;
    cv->mask_refs[slot] = 1;
    s->clip = slot + 1;
    return js_undef();
}

/* Whether (x, y) on the canvas is inside a path by a rule: the winding of
   its outline round the point's centre. */
static int jc_inside(const jcpath *p, double x, double y, int evenodd) {
    int wind = 0, cross = 0;
    for (int s = 0; s < p->nsub; s++) {
        int from = p->start[s], to = s + 1 < p->nsub ? p->start[s + 1] : p->n;
        for (int i = from; i < to; i++) {
            svpt p0 = p->pts[i], p1 = p->pts[i + 1 < to ? i + 1 : from];
            if ((p0.y <= y && p1.y > y) || (p1.y <= y && p0.y > y)) {
                double xc = p0.x + (y - p0.y) / (p1.y - p0.y) * (p1.x - p0.x);
                if (xc > x) { wind += p1.y > p0.y ? 1 : -1; cross++; }
            }
        }
    }
    return evenodd ? (cross & 1) : wind != 0;
}

static jval nat_jc_in_path(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    int argi;
    const jcpath *p = jc_path_arg(J, cv, a, n, &argi);
    double x = jc_num(J, a, n, argi), y = jc_num(J, a, n, argi + 1);
    if (!jc_finite(x) || !jc_finite(y)) return js_bool(0);
    return js_bool(jc_inside(p, x, y, jc_rule_arg(J, a, n, argi + 2)));
}

static jval nat_jc_in_stroke(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    int argi;
    const jcpath *p = jc_path_arg(J, cv, a, n, &argi);
    double x = jc_num(J, a, n, argi), y = jc_num(J, a, n, argi + 1);
    if (!jc_finite(x) || !jc_finite(y)) return js_bool(0);
    jc_path_clear(&jc_tmp2);
    jc_stroke_shapes(&jc_tmp2, p, jc_st(cv));
    return js_bool(jc_inside(&jc_tmp2, x, y, 0));
}

/* --- text ---------------------------------------------------------------------------------- */

static jval jc_text(jctx *J, jval t, jval *a, int n, int which) {
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    jstr *str = js_to_str(J, js_arg(a, n, 0));
    double x = jc_num(J, a, n, 1), y = jc_num(J, a, n, 2);
    if (!cv->px || !str || !jc_finite(x) || !jc_finite(y)) return js_undef();
    char txt[1024];
    int len = jc_ascii(str, txt, sizeof(txt));
    jctext m;
    jc_text_measure(s, txt, len, &m);
    /* maxWidth undefined is maxWidth not given (Chart.js passes it so). */
    int has_max = n > 3 && a[3].t != JS_UNDEF;
    double maxw = has_max ? jc_num(J, a, n, 3) : -1;
    if (has_max && (!jc_finite(maxw) || maxw <= 0)) return js_undef();
    int keep_px = s->font_px;
    if (maxw > 0 && m.width_user > maxw) {
        /* Too wide for its room: smaller until it fits. */
        int px = (int)(s->font_px * maxw / m.width_user);
        s->font_px = px < 1 ? 1 : px;
        jc_text_measure(s, txt, len, &m);
        s->font_px = keep_px;
    }
    double em = s->font_px;
    double dx = 0, dy = 0;
    int al = s->align;
    if (al == 1 || al == 3) dx = -m.width_user;
    else if (al == 4) dx = -m.width_user / 2;
    switch (s->baseline) {
    case 1: dy = em * 0.8; break;            /* top */
    case 2: dy = em * 0.64; break;           /* hanging */
    case 3: dy = em * 0.3; break;            /* middle */
    case 4: case 5: dy = -em * 0.2; break;   /* ideographic, bottom */
    }
    double px, py;
    jc_xy(s, x + dx, y + dy, &px, &py);
    jcpaint pt;
    jcdraw dw;
    jc_paint_of(J, t.obj, cv, which, &pt);
    jc_draw_setup(cv, &dw, &pt);
    jc_text_draw(&dw, txt, len, px, py, &m);
    return js_undef();
}

static jval nat_jc_fill_text(jctx *J, jval t, jval *a, int n) { return jc_text(J, t, a, n, 0); }
/* Outlined text is drawn filled, in the stroke's paint. */
static jval nat_jc_stroke_text(jctx *J, jval t, jval *a, int n) { return jc_text(J, t, a, n, 1); }

static jval nat_jc_measure(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    jcstate *s = jc_st(cv);
    jstr *str = js_to_str(J, js_arg(a, n, 0));
    char txt[1024];
    int len = jc_ascii(str, txt, sizeof(txt));
    jctext m;
    jc_text_measure(s, txt, len, &m);
    jobj *o = js_object_with(J, JO_PLAIN, jd_p_metrics);
    if (!o) return js_undef();
    double em = s->font_px, w = m.width_user;
    double left = s->align == 1 || s->align == 3 ? w : s->align == 4 ? w / 2 : 0;
    js_set(J, o, "width", js_num(w));
    js_set(J, o, "actualBoundingBoxLeft", js_num(left));
    js_set(J, o, "actualBoundingBoxRight", js_num(w - left));
    js_set(J, o, "actualBoundingBoxAscent", js_num(em * 0.72));
    js_set(J, o, "actualBoundingBoxDescent", js_num(em * 0.18));
    js_set(J, o, "fontBoundingBoxAscent", js_num(em * 0.8));
    js_set(J, o, "fontBoundingBoxDescent", js_num(em * 0.2));
    js_set(J, o, "emHeightAscent", js_num(em * 0.8));
    js_set(J, o, "emHeightDescent", js_num(em * 0.2));
    js_set(J, o, "hangingBaseline", js_num(em * 0.64));
    js_set(J, o, "alphabeticBaseline", js_num(0));
    js_set(J, o, "ideographicBaseline", js_num(-em * 0.2));
    return js_from_obj(o);
}

/* --- pictures ----------------------------------------------------------------------------- */

/* What a picture drawn from v is: an <img> the browser has decoded, a
   canvas, or a <video>'s latest frame. 0 for one that is not ready, and a
   TypeError for anything that is not a picture at all. */
static int jc_source(jctx *J, jval v, jcpaint *pt) {
    int el = jd_el_of(v);
    if (el < 0) {
        if (js_is_obj(v)) {
            jval k = jd_kept(v.obj, jd_k_iddata);
            if (k.t == JS_OBJ) {
                js_throw(J, JS_ERR_TYPE, "an ImageData is put, not drawn", J->error_line);
                return 0;
            }
        }
        js_throw(J, JS_ERR_TYPE, "that cannot be drawn on a canvas", J->error_line);
        return 0;
    }
    int tag = jd_doc->nodes[el].tag;
    pt->src_alpha = 0;
    if (tag == T_CANVAS) {
        int i = jc_open_slot(el);
        if (i < 0) return 0;
        jc_sync(i);
        if (!jc_canvas[i].px) return 0;
        pt->src = jc_canvas[i].px;
        pt->sw = jc_canvas[i].w;
        pt->sh = jc_canvas[i].h;
        pt->bpp = 4;
        return 1;
    }
    if (tag == T_IMG) {
        if (!jd_image_pixels) return 0;
        const u8 *al = 0;
        const u8 *px = jd_image_pixels(el, &pt->sw, &pt->sh, &al);
        if (!px) return 0;
        pt->src = px;
        pt->src_alpha = al;
        pt->bpp = 3;
        return 1;
    }
    if (tag == T_VIDEO) {
        const u8 *px = jsdom_media_picture(el, &pt->sw, &pt->sh);
        if (!px) return 0;
        pt->src = px;
        pt->bpp = 3;
        return 1;
    }
    js_throw(J, JS_ERR_TYPE, "that element cannot be drawn on a canvas", J->error_line);
    return 0;
}

static jval nat_jc_draw_image(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    if (n < 3) return js_throw(J, JS_ERR_TYPE, "drawImage takes a picture and where it goes", J->error_line);
    jcpaint pt;
    pt.kind = JP_IMAGE;
    pt.rep_x = pt.rep_y = 0;
    if (!jc_source(J, a[0], &pt)) return js_undef();
    if (!cv->px || pt.sw <= 0 || pt.sh <= 0) return js_undef();
    /* A picture is measured in its natural size, which may be more than the
       pixels it is kept at (browser.c, pic_shrink). */
    int el = jd_el_of(a[0]), nw = pt.sw, nh = pt.sh;
    if (el >= 0 && jd_doc->nodes[el].tag == T_IMG && jd_picture_size) {
        int w = 0, h = 0;
        jd_picture_size(el, &w, &h);
        if (w > 0 && h > 0) { nw = w; nh = h; }
    }
    double sx = 0, sy = 0, sw = nw, sh = nh, dx, dy, dw, dh;
    if (n >= 9) {
        sx = jc_num(J, a, n, 1); sy = jc_num(J, a, n, 2); sw = jc_num(J, a, n, 3); sh = jc_num(J, a, n, 4);
        dx = jc_num(J, a, n, 5); dy = jc_num(J, a, n, 6); dw = jc_num(J, a, n, 7); dh = jc_num(J, a, n, 8);
    } else if (n >= 5) {
        dx = jc_num(J, a, n, 1); dy = jc_num(J, a, n, 2); dw = jc_num(J, a, n, 3); dh = jc_num(J, a, n, 4);
    } else {
        dx = jc_num(J, a, n, 1); dy = jc_num(J, a, n, 2); dw = sw; dh = sh;
    }
    double all[8] = { sx, sy, sw, sh, dx, dy, dw, dh };
    for (int i = 0; i < 8; i++) if (!jc_finite(all[i])) return js_undef();
    if (!sw || !sh || !dw || !dh) return js_undef();
    jcstate *s = jc_st(cv);
    double det = s->a * s->d - s->b * s->c;
    if (det == 0 || !jc_finite(det)) return js_undef();
    /* From a pixel of the canvas back to the page's coordinates, then from
       the destination to the source. */
    double kx = sw / dw, ky = sh / dh;
    double ua = s->d / det, uc = -s->c / det, ue = (s->c * s->f - s->d * s->e) / det;
    double ub = -s->b / det, ud = s->a / det, uf = (s->b * s->e - s->a * s->f) / det;
    pt.ia = ua * kx; pt.ic = uc * kx; pt.ie = (ue - dx) * kx + sx;
    pt.ib = ub * ky; pt.id = ud * ky; pt.iff = (uf - dy) * ky + sy;
    if (nw != pt.sw || nh != pt.sh) {
        double px = (double)pt.sw / nw, py = (double)pt.sh / nh;
        pt.ia *= px; pt.ic *= px; pt.ie *= px;
        pt.ib *= py; pt.id *= py; pt.iff *= py;
    }
    jval sm = js_get(J, t, js_str(J, "imageSmoothingEnabled"));
    pt.smooth = sm.t == JS_UNDEF ? 1 : js_to_bool(sm);
    jc_path_clear(&jc_tmp);
    jc_rect(&jc_tmp, s, dx, dy, dw, dh);
    jcdraw dwr;
    jc_draw_setup(cv, &dwr, &pt);
    jc_fill_path(&dwr, &jc_tmp, 0);
    return js_undef();
}

/* --- gradients and patterns ----------------------------------------------------------------- */

static jval jc_gradient(jctx *J, int kind, double *v, int nv) {
    for (int i = 0; i < nv; i++) if (!jc_finite(v[i])) return js_throw(J, JS_ERR_TYPE, "a gradient's numbers are finite", J->error_line);
    if (kind == JP_RADIAL && (v[2] < 0 || v[5] < 0)) return js_throw_dom(J, "IndexSizeError", "a gradient's radii are not below 0");
    jobj *g = js_object_with(J, JO_PLAIN, jd_p_grad);
    jobj *arr = js_array(J), *stops = js_array(J);
    if (!g || !arr || !stops) return js_undef();
    double all[8] = { (double)kind, 0, 0, 0, 0, 0, 0, 0 };
    if (kind == JP_LINEAR) { all[1] = v[0]; all[2] = v[1]; all[4] = v[2]; all[5] = v[3]; }
    else if (kind == JP_RADIAL) { all[1] = v[0]; all[2] = v[1]; all[3] = v[2]; all[4] = v[3]; all[5] = v[4]; all[6] = v[5]; }
    else { all[7] = v[0]; all[1] = v[1]; all[2] = v[2]; }
    for (int i = 0; i < 8; i++) js_arr_push(J, arr, js_num(all[i]));
    jd_keep(g, jd_k_grad, js_from_obj(arr));
    jd_keep(g, jd_k_stops, js_from_obj(stops));
    return js_from_obj(g);
}

static jval nat_jc_linear(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double v[4];
    for (int i = 0; i < 4; i++) v[i] = jc_num(J, a, n, i);
    return jc_gradient(J, JP_LINEAR, v, 4);
}

static jval nat_jc_radial(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double v[6];
    for (int i = 0; i < 6; i++) v[i] = jc_num(J, a, n, i);
    return jc_gradient(J, JP_RADIAL, v, 6);
}

static jval nat_jc_conic(jctx *J, jval t, jval *a, int n) {
    (void)t;
    double v[3];
    for (int i = 0; i < 3; i++) v[i] = jc_num(J, a, n, i);
    return jc_gradient(J, JP_CONIC, v, 3);
}

static jval nat_grad_stop(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jval sv = jd_kept(t.obj, jd_k_stops);
    if (sv.t != JS_OBJ) return jd_illegal(J);
    double off = jc_num(J, a, n, 0);
    if (!jc_finite(off) || off < 0 || off > 1) return js_throw_dom(J, "IndexSizeError", "a stop's offset is from 0 to 1");
    jstr *c = js_to_str(J, js_arg(a, n, 1));
    u32 col;
    if (!c || !jc_parse_colour(c->s, &col)) return js_throw_dom(J, "SyntaxError", "that is not a colour");
    js_arr_push(J, sv.obj, js_num(off));
    js_arr_push(J, sv.obj, js_num((double)col));
    return js_undef();
}

static jval nat_jc_pattern(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jcpaint src;
    if (!jc_source(J, js_arg(a, n, 0), &src)) return J->sig == JS_OK ? js_null() : js_undef();
    jval rv = js_arg(a, n, 1);
    int rx = 1, ry = 1;
    if (rv.t != JS_NULL && rv.t != JS_UNDEF) {
        jstr *r = js_to_str(J, rv);
        if (!r) return js_undef();
        if (js_str_is(r, "repeat-x")) ry = 0;
        else if (js_str_is(r, "repeat-y")) rx = 0;
        else if (js_str_is(r, "no-repeat")) rx = ry = 0;
        else if (r->len && !js_str_is(r, "repeat")) return js_throw_dom(J, "SyntaxError", "that is not a way to repeat");
    }
    if (src.sw <= 0 || src.sh <= 0) return js_null();
    /* What the picture is now, copied: a pattern does not follow its source. */
    u64 bytes = (u64)src.sw * (u64)src.sh * 4;
    if (bytes > JC_BYTES) return js_null();
    char *buf = (char *)malloc(bytes);
    if (!buf) return js_null();
    for (u64 i = 0; i < (u64)src.sw * (u64)src.sh; i++) {
        const u8 *q = src.src + i * (u64)src.bpp;
        buf[i * 4] = (char)q[0];
        buf[i * 4 + 1] = (char)q[1];
        buf[i * 4 + 2] = (char)q[2];
        buf[i * 4 + 3] = (char)(src.bpp == 4 ? q[3] : src.src_alpha ? src.src_alpha[i] : 255);
    }
    jstr *px = js_str_n(J, buf, (u32)bytes);
    free(buf);
    jobj *p = js_object_with(J, JO_PLAIN, jd_p_pattern), *arr = js_array(J);
    if (!p || !arr || !px) return js_null();
    js_arr_push(J, arr, js_num(src.sw));
    js_arr_push(J, arr, js_num(src.sh));
    js_arr_push(J, arr, js_num(rx));
    js_arr_push(J, arr, js_num(ry));
    jd_keep(p, jd_k_pat, js_from_obj(arr));
    jd_keep(p, jd_k_patpx, js_from_str(px));
    return js_from_obj(p);
}

/* --- ImageData ------------------------------------------------------------------------------- */

static u8 *jc_typed_bytes(jval v, u32 *len) {
    if (!js_is_obj(v) || v.obj->kind != JO_TYPED) return 0;
    jtyped *tp = (jtyped *)v.obj->internal;
    if (!tp || tp->type != TA_U8C) return 0;
    *len = tp->len;
    return ta_bytes(tp->buf) + tp->off;
}

static jval jc_image_data_new(jctx *J, int w, int h, jval data) {
    if (w <= 0 || h <= 0) return js_throw_dom(J, "IndexSizeError", "a picture is at least one pixel each way");
    if ((u64)w * (u64)h * 4 > JC_BYTES) return js_throw(J, JS_ERR_RANGE, "that picture is too big", J->error_line);
    jobj *o = js_object_with(J, JO_PLAIN, jd_p_imagedata);
    if (!o) return js_undef();
    if (data.t == JS_UNDEF) {
        jobj *arr = ta_alloc(J, TA_U8C, (u32)(w * h * 4));
        if (!arr) return js_undef();
        data = js_from_obj(arr);
    }
    jd_keep(o, jd_k_idw, js_num(w));
    jd_keep(o, jd_k_idh, js_num(h));
    jd_keep(o, jd_k_iddata, data);
    return js_from_obj(o);
}

static jval nat_imagedata_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "ImageData is made with new", J->error_line);
    jval first = js_arg(a, n, 0);
    u32 len;
    u8 *bytes = jc_typed_bytes(first, &len);
    if (bytes) {
        double w = jc_num(J, a, n, 1);
        if (!jc_finite(w) || w <= 0 || !len || len % 4 || (len / 4) % (u32)w)
            return js_throw_dom(J, "IndexSizeError", "the bytes are not a whole number of rows that wide");
        double h = n > 2 ? jc_num(J, a, n, 2) : (double)(len / 4 / (u32)w);
        if (!jc_finite(h) || (u32)h * (u32)w * 4 != len) return js_throw_dom(J, "IndexSizeError", "the bytes are not that many rows");
        return jc_image_data_new(J, (int)w, (int)h, first);
    }
    double w = jc_num(J, a, n, 0), h = jc_num(J, a, n, 1);
    if (!jc_finite(w) || !jc_finite(h)) return js_throw_dom(J, "IndexSizeError", "a picture's size is finite");
    return jc_image_data_new(J, (int)w, (int)h, js_undef());
}

enum { JI_W, JI_H, JI_DATA, JI_SPACE };

static jval nat_imagedata_field(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t) || jd_kept(t.obj, jd_k_iddata).t != JS_OBJ) return jd_illegal(J);
    switch ((int)J->callee->data.num) {
    case JI_W: return jd_kept(t.obj, jd_k_idw);
    case JI_H: return jd_kept(t.obj, jd_k_idh);
    case JI_DATA: return jd_kept(t.obj, jd_k_iddata);
    }
    return jd_str("srgb");
}

static jval nat_jc_create_data(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval first = js_arg(a, n, 0);
    if (js_is_obj(first) && jd_kept(first.obj, jd_k_iddata).t == JS_OBJ)
        return jc_image_data_new(J, (int)js_to_num(J, jd_kept(first.obj, jd_k_idw)),
                                 (int)js_to_num(J, jd_kept(first.obj, jd_k_idh)), js_undef());
    double w = jc_num(J, a, n, 0), h = jc_num(J, a, n, 1);
    if (!jc_finite(w) || !jc_finite(h)) return js_throw(J, JS_ERR_TYPE, "a picture's size is finite", J->error_line);
    if (w < 0) w = -w;
    if (h < 0) h = -h;
    return jc_image_data_new(J, (int)w, (int)h, js_undef());
}

static jval nat_jc_get_data(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    double v[4];
    if (!jc_rect_args(J, a, n, v)) return js_throw(J, JS_ERR_TYPE, "getImageData takes a finite rectangle", J->error_line);
    if (!v[2] || !v[3]) return js_throw_dom(J, "IndexSizeError", "a rectangle with nothing in it");
    if (v[2] < 0) { v[0] += v[2]; v[2] = -v[2]; }
    if (v[3] < 0) { v[1] += v[3]; v[3] = -v[3]; }
    int x0 = (int)js_floor(v[0]), y0 = (int)js_floor(v[1]), w = (int)v[2], h = (int)v[3];
    jval id = jc_image_data_new(J, w, h, js_undef());
    if (J->sig != JS_OK || !js_is_obj(id)) return id;
    u32 len;
    u8 *out = jc_typed_bytes(jd_kept(id.obj, jd_k_iddata), &len);
    if (!out || !cv->px) return id;
    for (int y = 0; y < h; y++) {
        int sy = y0 + y;
        if (sy < 0 || sy >= cv->h) continue;
        for (int x = 0; x < w; x++) {
            int sx = x0 + x;
            if (sx < 0 || sx >= cv->w) continue;
            const u8 *q = cv->px + ((u64)sy * (u64)cv->w + (u64)sx) * 4;
            u8 *d = out + ((u64)y * (u64)w + (u64)x) * 4;
            d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = q[3];
        }
    }
    return id;
}

/* putImageData: the bytes written as they are, whatever the transform, the
   alpha, the operation and the clip say. */
static jval nat_jc_put_data(jctx *J, jval t, jval *a, int n) {
    JC_THIS(cv);
    jval id = js_arg(a, n, 0);
    if (!js_is_obj(id) || jd_kept(id.obj, jd_k_iddata).t != JS_OBJ)
        return js_throw(J, JS_ERR_TYPE, "putImageData takes an ImageData", J->error_line);
    int w = (int)js_to_num(J, jd_kept(id.obj, jd_k_idw)), h = (int)js_to_num(J, jd_kept(id.obj, jd_k_idh));
    u32 len;
    const u8 *src = jc_typed_bytes(jd_kept(id.obj, jd_k_iddata), &len);
    double dx = jc_num(J, a, n, 1), dy = jc_num(J, a, n, 2);
    if (!src || !jc_finite(dx) || !jc_finite(dy) || (u64)len < (u64)w * (u64)h * 4) return js_undef();
    double rx = 0, ry = 0, rw = w, rh = h;
    if (n >= 7) {
        rx = jc_num(J, a, n, 3); ry = jc_num(J, a, n, 4); rw = jc_num(J, a, n, 5); rh = jc_num(J, a, n, 6);
        if (!jc_finite(rx) || !jc_finite(ry) || !jc_finite(rw) || !jc_finite(rh)) return js_undef();
        if (rw < 0) { rx += rw; rw = -rw; }
        if (rh < 0) { ry += rh; rh = -rh; }
    }
    if (!cv->px) return js_undef();
    int ix0 = (int)js_floor(dx), iy0 = (int)js_floor(dy);
    for (int y = (int)ry; y < (int)(ry + rh); y++) {
        if (y < 0 || y >= h) continue;
        int ty = iy0 + y;
        if (ty < 0 || ty >= cv->h) continue;
        for (int x = (int)rx; x < (int)(rx + rw); x++) {
            if (x < 0 || x >= w) continue;
            int tx = ix0 + x;
            if (tx < 0 || tx >= cv->w) continue;
            const u8 *q = src + ((u64)y * (u64)w + (u64)x) * 4;
            u8 *d = cv->px + ((u64)ty * (u64)cv->w + (u64)tx) * 4;
            d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = q[3];
        }
    }
    cv->drawn = 1;
    cv->copy_ok = 0;
    jc_drawn_any = 1;
    return js_undef();
}

static jval nat_jc_attrs(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    JC_THIS(cv);
    (void)cv;
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    js_set(J, o, "alpha", js_bool(1));
    js_set(J, o, "colorSpace", jd_str("srgb"));
    js_set(J, o, "desynchronized", js_bool(0));
    js_set(J, o, "willReadFrequently", js_bool(0));
    return js_from_obj(o);
}

/* --- the element ------------------------------------------------------------------------------ */

/* getContext('2d'): one context an element, the same each time. Anything
   else is a context this browser has none of. */
static jval nat_canvas_context(jctx *J, jval t, jval *a, int n) {
    int el = jd_el_of(t);
    if (el < 0) return jd_illegal(J);
    jstr *kind = js_to_str(J, js_arg(a, n, 0));
    if (!kind || !js_str_is(kind, "2d")) return js_null();
    jval have = jd_kept(t.obj, jd_k_jcctx);
    if (js_is_obj(have)) return have;
    int i = jc_open_slot(el);
    if (i < 0) return js_null();
    jobj *ctx = js_object_with(J, JO_PLAIN, jd_p_ctx2d);
    if (!ctx) return js_null();
    jd_keep(ctx, jd_k_jc, js_num(i));
    js_define(J, ctx, js_str(J, "canvas"), t, JP_ENUM | JP_CONF);
    jd_keep(t.obj, jd_k_jcctx, js_from_obj(ctx));
    return js_from_obj(ctx);
}

/* width and height: setting either clears the canvas, the same number or
   not (canvas.width = canvas.width is how a page wipes one). */
static jval nat_canvas_dim(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int el = jd_el_of(t);
    if (el < 0) return jd_illegal(J);
    int h = (int)J->callee->data.num;
    return js_num(jc_dim(el, h ? "height" : "width", h ? 150 : 300));
}

static jval nat_canvas_dim_set(jctx *J, jval t, jval *a, int n) {
    int el = jd_el_of(t);
    if (el < 0) return jd_illegal(J);
    int h = (int)J->callee->data.num;
    double v = js_to_num(J, js_arg(a, n, 0));
    long long k = jc_finite(v) && v > 0 ? (long long)v : 0;
    if (k > 1000000) k = 1000000;
    char buf[16];
    int m = 0;
    char rev[16];
    int r = 0;
    if (!k) rev[r++] = '0';
    while (k) { rev[r++] = (char)('0' + k % 10); k /= 10; }
    while (r) buf[m++] = rev[--r];
    buf[m] = 0;
    jd_attr_set(el, h ? "height" : "width", buf);
    int i = jc_slot_of(el);
    if (i >= 0) jc_canvas[i].reset = 1;
    return js_undef();
}

/* --- PNG, for toDataURL and toBlob ------------------------------------------------------------ */

static u32 jc_crc_table[256];

static u32 jc_crc(u32 c, const u8 *p, u32 n) {
    if (!jc_crc_table[1]) {
        for (u32 i = 0; i < 256; i++) {
            u32 v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            jc_crc_table[i] = v;
        }
    }
    c = ~c;
    for (u32 i = 0; i < n; i++) c = jc_crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}

static void jc_be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* The canvas as a PNG: eight bits of each of red, green, blue and alpha, the
   rows unfiltered and deflate's stored blocks, which every decoder reads and
   which take no compressor to write. */
static u8 *jc_png(const jcanvas *cv, u32 *out_len) {
    u64 row = 1 + (u64)cv->w * 4, raw = row * (u64)cv->h;
    u64 blocks = (raw + 65534) / 65535;
    u64 zlen = 2 + raw + blocks * 5 + 4;
    u64 total = 8 + 25 + 12 + zlen + 12;
    if (total > 0x7FFFFFFF) return 0;
    u8 *png = (u8 *)malloc(total);
    if (!png) return 0;
    static const u8 SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    u64 at = 0;
    for (int i = 0; i < 8; i++) png[at++] = SIG[i];
    jc_be32(png + at, 13); at += 4;
    u8 *ihdr = png + at;
    ihdr[0] = 'I'; ihdr[1] = 'H'; ihdr[2] = 'D'; ihdr[3] = 'R';
    jc_be32(ihdr + 4, (u32)cv->w);
    jc_be32(ihdr + 8, (u32)cv->h);
    ihdr[12] = 8; ihdr[13] = 6; ihdr[14] = 0; ihdr[15] = 0; ihdr[16] = 0;
    jc_be32(ihdr + 17, jc_crc(0, ihdr, 17));
    at += 21;
    jc_be32(png + at, (u32)zlen); at += 4;
    u8 *idat = png + at;
    idat[0] = 'I'; idat[1] = 'D'; idat[2] = 'A'; idat[3] = 'T';
    u64 z = 4;
    idat[z++] = 0x78; idat[z++] = 0x01;
    u32 s1 = 1, s2 = 0;
    u64 left = raw, y = 0, x = 0;           /* x counts bytes into the current row */
    while (left > 0) {
        u32 take = left > 65535 ? 65535 : (u32)left;
        left -= take;
        idat[z++] = left ? 0 : 1;
        idat[z++] = (u8)take; idat[z++] = (u8)(take >> 8);
        idat[z++] = (u8)~take; idat[z++] = (u8)(~take >> 8);
        for (u32 k = 0; k < take; k++) {
            u8 b = x == 0 ? 0 : cv->px[(y * (u64)cv->w * 4) + (x - 1)];
            idat[z++] = b;
            s1 = (s1 + b) % 65521;
            s2 = (s2 + s1) % 65521;
            if (++x == row) { x = 0; y++; }
        }
    }
    jc_be32(idat + z, (s2 << 16) | s1); z += 4;
    jc_be32(idat + z, jc_crc(0, idat, (u32)z)); z += 4;
    at += z;
    jc_be32(png + at, 0); at += 4;
    png[at] = 'I'; png[at + 1] = 'E'; png[at + 2] = 'N'; png[at + 3] = 'D';
    jc_be32(png + at + 4, jc_crc(0, png + at, 4));
    at += 8;
    *out_len = (u32)at;
    return png;
}

static jval nat_canvas_data_url(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int el = jd_el_of(t);
    if (el < 0) return jd_illegal(J);
    int i = jc_open_slot(el);
    if (i < 0) return jd_str("data:,");
    jc_sync(i);
    jcanvas *cv = &jc_canvas[i];
    if (!cv->px || cv->w <= 0 || cv->h <= 0) return jd_str("data:,");
    u32 len;
    u8 *png = jc_png(cv, &len);
    if (!png) return jd_str("data:,");
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *pre = "data:image/png;base64,";
    u32 plen = 0;
    while (pre[plen]) plen++;
    u64 olen = plen + ((u64)len + 2) / 3 * 4;
    char *out = (char *)malloc(olen + 1);
    if (!out) { free(png); return jd_str("data:,"); }
    u64 w = 0;
    for (u32 k = 0; k < plen; k++) out[w++] = pre[k];
    for (u32 k = 0; k < len; k += 3) {
        u32 v = (u32)png[k] << 16 | (k + 1 < len ? (u32)png[k + 1] << 8 : 0) | (k + 2 < len ? png[k + 2] : 0);
        out[w++] = B64[(v >> 18) & 63];
        out[w++] = B64[(v >> 12) & 63];
        out[w++] = k + 1 < len ? B64[(v >> 6) & 63] : '=';
        out[w++] = k + 2 < len ? B64[v & 63] : '=';
    }
    out[w] = 0;
    jstr *s = js_str_n(J, out, (u32)w);
    free(out);
    free(png);
    return s ? js_from_str(s) : jd_str("data:,");
}

/* toBlob's callback is called later, as a task, with a PNG Blob (or null for
   a canvas with nothing to it). */
static void jc_blob_later(jval arg) {
    jctx *J = &jd_J;
    if (!js_is_obj(arg) || arg.obj->kind != JO_ARRAY || arg.obj->len < 2) return;
    jval fn = arg.obj->items[0], blob = arg.obj->items[1];
    js_call(J, fn, js_undef(), &blob, 1);
    if (J->sig != JS_OK) J->sig = JS_OK;
}

static jval nat_canvas_to_blob(jctx *J, jval t, jval *a, int n) {
    int el = jd_el_of(t);
    if (el < 0) return jd_illegal(J);
    jval fn = js_arg(a, n, 0);
    if (!js_is_obj(fn) || !js_callable(fn)) return js_throw(J, JS_ERR_TYPE, "toBlob takes a function", J->error_line);
    int i = jc_open_slot(el);
    jval blob = js_null();
    if (i >= 0) {
        jc_sync(i);
        jcanvas *cv = &jc_canvas[i];
        u32 len;
        u8 *png = cv->px && cv->w > 0 && cv->h > 0 ? jc_png(cv, &len) : 0;
        if (png) {
            jstr *bytes = js_str_n(J, (const char *)png, len);
            free(png);
            jobj *b = bytes ? jd_blob_make(J, 0, bytes, js_str(J, "image/png")) : 0;
            if (b) blob = js_from_obj(b);
        }
    }
    jobj *pair = js_array(J);
    if (!pair) return js_undef();
    js_arr_push(J, pair, fn);
    js_arr_push(J, pair, blob);
    jd_later_native(jc_blob_later, js_from_obj(pair), 0);
    return js_undef();
}

/* --- for the browser ---------------------------------------------------------------------------- */

/* A canvas's pixels as the browser draws pictures: three bytes each and a
   plane of alpha, made again only after it has been drawn on. 0 for an
   element with no bitmap. */
const u8 *jsdom_canvas_picture(int node, int *w, int *h, const u8 **alpha) {
    int i = jc_slot_of(node);
    if (i < 0) return 0;
    jcanvas *cv = &jc_canvas[i];
    if (!cv->px || cv->w <= 0 || cv->h <= 0) return 0;
    u64 np = (u64)cv->w * (u64)cv->h;
    if (!cv->copy_ok) {
        if (!cv->rgb) cv->rgb = (u8 *)malloc(np * 3);
        if (!cv->alpha) cv->alpha = (u8 *)malloc(np);
        if (!cv->rgb || !cv->alpha) return 0;
        for (u64 k = 0; k < np; k++) {
            cv->rgb[k * 3] = cv->px[k * 4];
            cv->rgb[k * 3 + 1] = cv->px[k * 4 + 1];
            cv->rgb[k * 3 + 2] = cv->px[k * 4 + 2];
            cv->alpha[k] = cv->px[k * 4 + 3];
        }
        cv->copy_ok = 1;
    }
    *w = cv->w;
    *h = cv->h;
    *alpha = cv->alpha;
    return cv->rgb;
}

/* Whether any canvas has been drawn on since this was last asked, and so
   the page has to be drawn again; answered once. */
int jsdom_canvas_drawn(void) {
    int d = jc_drawn_any;
    jc_drawn_any = 0;
    return d;
}

/* --- setting it up ------------------------------------------------------------------------------ */

static void jd_setup_canvas(jctx *J) {
    jd_k_jc = js_sym_new(J, "canvas", 6);
    jd_k_jcctx = js_sym_new(J, "context", 7);
    jd_k_jcpaints = js_sym_new(J, "paints", 6);
    jd_k_grad = js_sym_new(J, "gradient", 8);
    jd_k_stops = js_sym_new(J, "stops", 5);
    jd_k_pat = js_sym_new(J, "pattern", 7);
    jd_k_patpx = js_sym_new(J, "pixels", 6);
    jd_k_p2d = js_sym_new(J, "path", 4);
    jd_k_idw = js_sym_new(J, "width", 5);
    jd_k_idh = js_sym_new(J, "height", 6);
    jd_k_iddata = js_sym_new(J, "data", 4);
    jd_canvas_reset();

    jobj *p = jd_p_ctx2d = jd_interface(J, "CanvasRenderingContext2D", 0, 0, 0);
    static const struct { const char *name; jnative fn; int arity; } M[] = {
        { "save", nat_jc_save, 0 }, { "restore", nat_jc_restore, 0 }, { "reset", nat_jc_reset, 0 },
        { "translate", nat_jc_translate, 2 }, { "scale", nat_jc_scale, 2 }, { "rotate", nat_jc_rotate, 1 },
        { "transform", nat_jc_transform, 6 }, { "setTransform", nat_jc_set_transform, 0 },
        { "resetTransform", nat_jc_reset_transform, 0 }, { "getTransform", nat_jc_get_transform, 0 },
        { "setLineDash", nat_jc_set_dash, 1 }, { "getLineDash", nat_jc_get_dash, 0 },
        { "beginPath", nat_jc_begin, 0 }, { "moveTo", nat_jc_move, 2 }, { "lineTo", nat_jc_line, 2 },
        { "closePath", nat_jc_closepath, 0 }, { "quadraticCurveTo", nat_jc_quad, 4 },
        { "bezierCurveTo", nat_jc_cubic, 6 }, { "arcTo", nat_jc_arcto, 5 }, { "rect", nat_jc_rect, 4 },
        { "roundRect", nat_jc_rrect, 4 }, { "arc", nat_jc_arc, 5 }, { "ellipse", nat_jc_ellipse, 7 },
        { "fill", nat_jc_fill, 0 }, { "stroke", nat_jc_stroke, 0 }, { "clip", nat_jc_clip, 0 },
        { "isPointInPath", nat_jc_in_path, 2 }, { "isPointInStroke", nat_jc_in_stroke, 2 },
        { "fillRect", nat_jc_fill_rect, 4 }, { "strokeRect", nat_jc_stroke_rect, 4 },
        { "clearRect", nat_jc_clear_rect, 4 }, { "fillText", nat_jc_fill_text, 3 },
        { "strokeText", nat_jc_stroke_text, 3 }, { "measureText", nat_jc_measure, 1 },
        { "drawImage", nat_jc_draw_image, 3 }, { "createLinearGradient", nat_jc_linear, 4 },
        { "createRadialGradient", nat_jc_radial, 6 }, { "createConicGradient", nat_jc_conic, 3 },
        { "createPattern", nat_jc_pattern, 2 }, { "createImageData", nat_jc_create_data, 1 },
        { "getImageData", nat_jc_get_data, 4 }, { "putImageData", nat_jc_put_data, 3 },
        { "getContextAttributes", nat_jc_attrs, 0 }, { "isContextLost", nat_false_getter, 0 },
        { "drawFocusIfNeeded", nat_nothing_js, 1 },
    };
    for (int i = 0; i < (int)(sizeof(M) / sizeof(M[0])); i++) jd_method(J, p, M[i].name, M[i].fn, M[i].arity);
    jd_accessor(J, p, "fillStyle", nat_jc_fill_get, nat_jc_fill_set);
    jd_accessor(J, p, "strokeStyle", nat_jc_stroke_get, nat_jc_stroke_set);
    static const char *const FIELDS[] = { "lineWidth", "miterLimit", "globalAlpha", "lineDashOffset", "lineCap",
                                          "lineJoin", "globalCompositeOperation", "textAlign", "textBaseline",
                                          "font", 0 };
    for (int i = 0; FIELDS[i]; i++) {
        jobj *g = js_native(J, FIELDS[i], nat_jc_field), *s = js_native(J, FIELDS[i], nat_jc_field_set);
        if (!g || !s) continue;
        g->flags |= JOF_NOCTOR;
        s->flags |= JOF_NOCTOR;
        g->data = s->data = js_num(i);
        js_define_accessor(J, p, js_str(J, FIELDS[i]), js_from_obj(g), js_from_obj(s), JP_ENUM | JP_CONF);
    }
    /* Kept and given back; nothing is drawn from them (see the top). */
    js_set(J, p, "shadowBlur", js_num(0));
    js_set(J, p, "shadowColor", jd_str("rgba(0, 0, 0, 0)"));
    js_set(J, p, "shadowOffsetX", js_num(0));
    js_set(J, p, "shadowOffsetY", js_num(0));
    js_set(J, p, "imageSmoothingEnabled", js_bool(1));
    js_set(J, p, "imageSmoothingQuality", jd_str("low"));
    js_set(J, p, "filter", jd_str("none"));
    js_set(J, p, "direction", jd_str("inherit"));
    js_set(J, p, "fontKerning", jd_str("auto"));
    js_set(J, p, "letterSpacing", jd_str("0px"));
    js_set(J, p, "wordSpacing", jd_str("0px"));
    js_set(J, p, "textRendering", jd_str("auto"));

    jd_p_grad = jd_interface(J, "CanvasGradient", 0, 0, 0);
    jd_method(J, jd_p_grad, "addColorStop", nat_grad_stop, 2);
    jd_p_pattern = jd_interface(J, "CanvasPattern", 0, 0, 0);
    jd_method(J, jd_p_pattern, "setTransform", nat_nothing_js, 0);
    jd_p_metrics = jd_interface(J, "TextMetrics", 0, 0, 0);

    jd_p_imagedata = jd_interface(J, "ImageData", 0, nat_imagedata_ctor, 2);
    static const char *const ID[] = { "width", "height", "data", "colorSpace", 0 };
    for (int i = 0; ID[i]; i++) jd_getter_data(J, jd_p_imagedata, ID[i], nat_imagedata_field, i);

    jd_p_path2d = jd_interface(J, "Path2D", 0, nat_path2d_ctor, 0);
    static const struct { const char *name; jnative fn; int arity; } P[] = {
        { "moveTo", nat_jc_move, 2 }, { "lineTo", nat_jc_line, 2 }, { "closePath", nat_jc_closepath, 0 },
        { "quadraticCurveTo", nat_jc_quad, 4 }, { "bezierCurveTo", nat_jc_cubic, 6 },
        { "arcTo", nat_jc_arcto, 5 }, { "rect", nat_jc_rect, 4 }, { "roundRect", nat_jc_rrect, 4 },
        { "arc", nat_jc_arc, 5 }, { "ellipse", nat_jc_ellipse, 7 }, { "addPath", nat_path2d_add, 1 },
    };
    for (int i = 0; i < (int)(sizeof(P) / sizeof(P[0])); i++) jd_method(J, jd_p_path2d, P[i].name, P[i].fn, P[i].arity);

    jobj *cp = jd_iface("HTMLCanvasElement");
    if (cp) {
        for (int h = 0; h < 2; h++) {
            const char *nm = h ? "height" : "width";
            jobj *g = js_native(J, nm, nat_canvas_dim), *st = js_native(J, nm, nat_canvas_dim_set);
            if (!g || !st) continue;
            g->flags |= JOF_NOCTOR;
            st->flags |= JOF_NOCTOR;
            g->data = st->data = js_num(h);
            js_define_accessor(J, cp, js_str(J, nm), js_from_obj(g), js_from_obj(st), JP_ENUM | JP_CONF);
        }
        jd_method(J, cp, "getContext", nat_canvas_context, 1);
        jd_method(J, cp, "toDataURL", nat_canvas_data_url, 0);
        jd_method(J, cp, "toBlob", nat_canvas_to_blob, 1);
    }
}
