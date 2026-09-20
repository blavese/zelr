#pragma once
#include "zelr.h"
#include "alloc.h"
#include "dom.h"
#include "png.h"
#include "css.h"

/* SVG, which is not a picture format at all.
 *
 * A PNG and a JPEG are pixels, compressed two different ways, and decoding
 * one is undoing that. An SVG is a drawing: shapes, in coordinates, at
 * whatever size you care to draw them. So there is nothing to decode. There
 * is a renderer, which is a different and larger thing, and this is the
 * smaller half of one.
 *
 * It matters because it is what a logo is now. The reason the browser showed
 * the word "Google" and no Google is that the logo is an SVG, and no amount
 * of PNG decoding was ever going to show it.
 *
 * What is here:
 *
 *   paths, with every command the format has except arcs written as arcs —
 *     move, line, horizontal, vertical, cubic, quadratic, their smooth
 *     forms, close, and each of those in relative coordinates
 *   rect, circle, ellipse, line, polyline, polygon, which are paths with a
 *     shorter spelling
 *   transforms: translate, scale, rotate, matrix, nested
 *   viewBox, so a drawing meant for one size lands at another
 *   fill and stroke, in any colour spelling the style sheet understands,
 *     with both winding rules
 *
 * What is not, and is skipped rather than approximated: gradients, filters,
 * clipping, masks, patterns, and text. Each of those is a renderer of its
 * own — text alone means the font machinery, laid out along a path — and a
 * logo drawn with the shapes above and a flat fill is the overwhelming
 * majority of what a page actually carries. A gradient fill comes out as its
 * first stop rather than as nothing, because a shape in roughly the right
 * colour is closer than a hole.
 *
 * Curves are flattened to lines before anything is drawn. Filling is a
 * scanline: for every row, where does the outline cross it, and what is
 * inside. The edges are sampled four times a pixel each way, which is what
 * makes a diagonal a diagonal rather than a staircase.
 */

#define SVG_OK            0
#define SVG_NOT_SVG      -1
#define SVG_TOO_BIG      -2
#define SVG_NOTHING_IN_IT -3

#define SVG_MAX_PTS   16384      /* flattened points in one path */
#define SVG_MAX_SUB     256      /* subpaths in one path */
#define SVG_MAX_SIDE   2048
#define SVG_SUB           4      /* samples per pixel, each way */

typedef struct { float x, y; } svpt;

/* a c e
   b d f, the usual six of an affine transform. */
typedef struct { float a, b, c, d, e, f; } svmat;

typedef struct {
    svpt pts[SVG_MAX_PTS];
    int  n;
    int  start[SVG_MAX_SUB];     /* where each subpath begins */
    int  nsub;
} svpath;

/* --- transforms ----------------------------------------------------------- */

static inline svmat sv_identity(void) {
    svmat m = { 1, 0, 0, 1, 0, 0 };
    return m;
}

static inline svmat sv_mul(svmat x, svmat y) {
    svmat r;
    r.a = x.a * y.a + x.c * y.b;
    r.b = x.b * y.a + x.d * y.b;
    r.c = x.a * y.c + x.c * y.d;
    r.d = x.b * y.c + x.d * y.d;
    r.e = x.a * y.e + x.c * y.f + x.e;
    r.f = x.b * y.e + x.d * y.f + x.f;
    return r;
}

static inline svpt sv_apply(svmat m, float x, float y) {
    svpt p;
    p.x = m.a * x + m.c * y + m.e;
    p.y = m.b * x + m.d * y + m.f;
    return p;
}

/* --- reading numbers ------------------------------------------------------
 *
 * Path data is numbers with almost anything between them: commas, spaces, or
 * nothing at all when the sign makes the break. "10-20" is two numbers. */
static inline int sv_space(char c) {
    return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r';
}

static inline float sv_number(const char **at) {
    const char *p = *at;
    while (*p && sv_space(*p)) p++;

    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    float v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (float)(*p++ - '0');

    if (*p == '.') {
        p++;
        float scale = 0.1f;
        while (*p >= '0' && *p <= '9') {
            v += (float)(*p++ - '0') * scale;
            scale *= 0.1f;
        }
    }

    if (*p == 'e' || *p == 'E') {
        const char *save = p;
        p++;
        int eneg = 0;
        if (*p == '-') { eneg = 1; p++; }
        else if (*p == '+') p++;
        if (*p >= '0' && *p <= '9') {
            int ex = 0;
            while (*p >= '0' && *p <= '9') ex = ex * 10 + (*p++ - '0');
            if (ex > 30) ex = 30;
            for (int i = 0; i < ex; i++) v = eneg ? v * 0.1f : v * 10.0f;
        } else {
            p = save;
        }
    }

    *at = p;
    return neg ? -v : v;
}

static inline int sv_more(const char *p) {
    while (*p && sv_space(*p)) p++;
    return (*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.';
}

/* --- building a path ------------------------------------------------------ */

static inline void sv_begin(svpath *p) {
    if (p->nsub < SVG_MAX_SUB) p->start[p->nsub++] = p->n;
}

static inline void sv_add(svpath *p, svmat m, float x, float y) {
    if (p->n >= SVG_MAX_PTS) return;
    p->pts[p->n++] = sv_apply(m, x, y);
}

/* A cubic, as enough straight lines that the difference does not show. The
   number of them is fixed rather than worked out from how bent the curve is:
   sixteen is more than a letterform needs at the sizes a page draws one. */
static inline void sv_cubic(svpath *p, svmat m, float x0, float y0,
                            float x1, float y1, float x2, float y2,
                            float x3, float y3) {
    const int steps = 16;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps;
        float u = 1 - t;
        float x = u * u * u * x0 + 3 * u * u * t * x1
                + 3 * u * t * t * x2 + t * t * t * x3;
        float y = u * u * u * y0 + 3 * u * u * t * y1
                + 3 * u * t * t * y2 + t * t * t * y3;
        sv_add(p, m, x, y);
    }
}

/* --- path data ------------------------------------------------------------ */

static inline void sv_path_data(svpath *path, svmat m, const char *d) {
    float x = 0, y = 0;            /* where the pen is */
    float sx = 0, sy = 0;          /* where this subpath started */
    float cx = 0, cy = 0;          /* the last control point, for smooth forms */
    char last = 0;
    int have_control = 0;

    const char *p = d;
    while (*p) {
        while (*p && sv_space(*p)) p++;
        if (!*p) break;

        char cmd = *p;
        if ((cmd >= '0' && cmd <= '9') || cmd == '-' || cmd == '+' || cmd == '.') {
            /* A repeated command: another set of numbers means do it again,
               except that a repeated moveto is a lineto, which is a rule
               that exists to catch people out. */
            cmd = last == 'M' ? 'L' : last == 'm' ? 'l' : last;
            if (!cmd) break;
        } else {
            p++;
        }
        last = cmd;

        int rel = cmd >= 'a' && cmd <= 'z';
        char c = (char)(rel ? cmd - 32 : cmd);

        if (c == 'M') {
            float nx = sv_number(&p), ny = sv_number(&p);
            x = rel ? x + nx : nx;
            y = rel ? y + ny : ny;
            sx = x; sy = y;
            sv_begin(path);
            sv_add(path, m, x, y);
            have_control = 0;
        } else if (c == 'L') {
            float nx = sv_number(&p), ny = sv_number(&p);
            x = rel ? x + nx : nx;
            y = rel ? y + ny : ny;
            sv_add(path, m, x, y);
            have_control = 0;
        } else if (c == 'H') {
            float nx = sv_number(&p);
            x = rel ? x + nx : nx;
            sv_add(path, m, x, y);
            have_control = 0;
        } else if (c == 'V') {
            float ny = sv_number(&p);
            y = rel ? y + ny : ny;
            sv_add(path, m, x, y);
            have_control = 0;
        } else if (c == 'C' || c == 'S') {
            float x1, y1;
            if (c == 'C') {
                x1 = sv_number(&p); y1 = sv_number(&p);
                if (rel) { x1 += x; y1 += y; }
            } else {
                /* Smooth: the first control point is the last one mirrored,
                   which is what makes a run of curves continuous. */
                x1 = have_control ? 2 * x - cx : x;
                y1 = have_control ? 2 * y - cy : y;
            }
            float x2 = sv_number(&p), y2 = sv_number(&p);
            float x3 = sv_number(&p), y3 = sv_number(&p);
            if (rel) { x2 += x; y2 += y; x3 += x; y3 += y; }

            sv_cubic(path, m, x, y, x1, y1, x2, y2, x3, y3);
            cx = x2; cy = y2;
            have_control = 1;
            x = x3; y = y3;
        } else if (c == 'Q' || c == 'T') {
            float qx, qy;
            if (c == 'Q') {
                qx = sv_number(&p); qy = sv_number(&p);
                if (rel) { qx += x; qy += y; }
            } else {
                qx = have_control ? 2 * x - cx : x;
                qy = have_control ? 2 * y - cy : y;
            }
            float x2 = sv_number(&p), y2 = sv_number(&p);
            if (rel) { x2 += x; y2 += y; }

            /* A quadratic is a cubic whose control points are two thirds of
               the way to the single one. */
            sv_cubic(path, m, x, y,
                     x + 2.0f / 3.0f * (qx - x), y + 2.0f / 3.0f * (qy - y),
                     x2 + 2.0f / 3.0f * (qx - x2), y2 + 2.0f / 3.0f * (qy - y2),
                     x2, y2);
            cx = qx; cy = qy;
            have_control = 1;
            x = x2; y = y2;
        } else if (c == 'A') {
            /* An elliptical arc, as a line to where it ends.
             *
             * The parameters are an ellipse's radii, a rotation and two
             * flags, and turning that into a curve is a page of trigonometry
             * for something a logo uses to round a corner. Drawn as its
             * chord, which is wrong by the bulge of the arc and right about
             * where the shape goes next — and the alternative, skipping it,
             * leaves the outline open and fills the whole shape wrongly. */
            sv_number(&p); sv_number(&p); sv_number(&p);
            sv_number(&p); sv_number(&p);
            float nx = sv_number(&p), ny = sv_number(&p);
            x = rel ? x + nx : nx;
            y = rel ? y + ny : ny;
            sv_add(path, m, x, y);
            have_control = 0;
        } else if (c == 'Z') {
            sv_add(path, m, sx, sy);
            x = sx; y = sy;
            have_control = 0;
        } else {
            break;                          /* something this does not know */
        }
    }
}

/* --- filling ---------------------------------------------------------------
 *
 * A scanline fill with the edges sampled several times per pixel row, and
 * the crossings within a row turned into fractional coverage across it. That
 * is what makes an outline smooth: a pixel the edge passes through halfway
 * is half the colour, rather than in or out.
 */
typedef struct {
    u8 *rgb;
    int w, h;
} svcanvas;

static inline void sv_span(float *cov, int w, float x0, float x1) {
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
    if (x1 <= 0 || x0 >= (float)w) return;
    if (x0 < 0) x0 = 0;
    if (x1 > (float)w) x1 = (float)w;

    int a = (int)x0, b = (int)x1;
    if (a == b) {
        cov[a] += x1 - x0;
        return;
    }
    cov[a] += (float)(a + 1) - x0;
    for (int i = a + 1; i < b; i++) cov[i] += 1.0f;
    if (b < w) cov[b] += x1 - (float)b;
}

/* The fill takes the points and where the subpaths begin, rather than a
 * whole svpath.
 *
 * Because an svpath is a hundred and thirty kilobytes and a program's stack
 * is sixty four, so a function that wanted one of its own to draw a four
 * sided shape ran off the end of the stack into nothing. It is the same
 * mistake the kernel made with its own stacks and found by painting them;
 * ring 3 has no such paint, so the rule here is simply that nothing puts one
 * of these on the stack.
 */
static inline void sv_fill_pts(svcanvas *cv, const svpt *pts, int n,
                               const int *start, int nsub, u32 colour,
                               int evenodd, float alpha) {
    if (n < 2 || nsub < 1) return;

    float miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].y < miny) miny = pts[i].y;
        if (pts[i].y > maxy) maxy = pts[i].y;
    }
    int y0 = (int)miny - 1, y1 = (int)maxy + 1;
    if (y0 < 0) y0 = 0;
    if (y1 > cv->h) y1 = cv->h;
    if (y0 >= y1) return;

    float *cov = (float *)malloc((u32)(cv->w * (int)sizeof(float)));
    if (!cov) return;

    /* Crossings of one sub-scanline: where, and which way the edge was
       going, which is what the nonzero rule needs. */
    float xs[256];
    int dirs[256];

    int r = (int)((colour >> 16) & 0xFF);
    int g = (int)((colour >> 8) & 0xFF);
    int b = (int)(colour & 0xFF);

    for (int y = y0; y < y1; y++) {
        for (int i = 0; i < cv->w; i++) cov[i] = 0;

        for (int sub = 0; sub < SVG_SUB; sub++) {
            float sy = (float)y + ((float)sub + 0.5f) / SVG_SUB;
            int crossings = 0;

            for (int s = 0; s < nsub && crossings < 250; s++) {
                int from = start[s];
                int to = (s + 1 < nsub) ? start[s + 1] : n;
                if (to - from < 2) continue;

                for (int i = from; i < to && crossings < 250; i++) {
                    /* The last point joins back to the first: a fill always
                       closes, whether the path said so or not. */
                    svpt p0 = pts[i];
                    svpt p1 = pts[i + 1 < to ? i + 1 : from];

                    if ((p0.y <= sy && p1.y > sy) || (p1.y <= sy && p0.y > sy)) {
                        float t = (sy - p0.y) / (p1.y - p0.y);
                        xs[crossings] = p0.x + t * (p1.x - p0.x);
                        dirs[crossings] = p1.y > p0.y ? 1 : -1;
                        crossings++;
                    }
                }
            }
            if (crossings < 2) continue;

            /* Sorted, because the spans between them are what is inside. */
            for (int i = 1; i < crossings; i++) {
                float kx = xs[i];
                int kd = dirs[i];
                int j = i - 1;
                while (j >= 0 && xs[j] > kx) {
                    xs[j + 1] = xs[j];
                    dirs[j + 1] = dirs[j];
                    j--;
                }
                xs[j + 1] = kx;
                dirs[j + 1] = kd;
            }

            if (evenodd) {
                for (int i = 0; i + 1 < crossings; i += 2)
                    sv_span(cov, cv->w, xs[i], xs[i + 1]);
            } else {
                int wind = 0;
                for (int i = 0; i + 1 < crossings; i++) {
                    wind += dirs[i];
                    if (wind != 0) sv_span(cov, cv->w, xs[i], xs[i + 1]);
                }
            }
        }

        for (int x = 0; x < cv->w; x++) {
            float a = cov[x] / SVG_SUB * alpha;
            if (a <= 0.002f) continue;
            if (a > 1) a = 1;

            u8 *px = cv->rgb + (y * cv->w + x) * 3;
            px[0] = (u8)(px[0] + (r - px[0]) * a);
            px[1] = (u8)(px[1] + (g - px[1]) * a);
            px[2] = (u8)(px[2] + (b - px[2]) * a);
        }
    }

    free(cov);
}

static inline void sv_fill(svcanvas *cv, const svpath *path, u32 colour,
                           int evenodd, float alpha) {
    sv_fill_pts(cv, path->pts, path->n, path->start, path->nsub,
                colour, evenodd, alpha);
}

/* A stroke, as a filled quadrilateral per segment plus a square at each
   joint. Not what a real stroker does — that offsets the outline and solves
   the joins properly — and indistinguishable from it at the widths a logo
   uses. */
static inline void sv_stroke(svcanvas *cv, const svpath *path, u32 colour,
                             float width, float alpha) {
    if (width < 0.6f) width = 0.6f;
    float half = width / 2;

    for (int s = 0; s < path->nsub; s++) {
        int from = path->start[s];
        int to = (s + 1 < path->nsub) ? path->start[s + 1] : path->n;

        for (int i = from; i + 1 < to; i++) {
            svpt p0 = path->pts[i], p1 = path->pts[i + 1];
            float dx = p1.x - p0.x, dy = p1.y - p0.y;

            float len2 = dx * dx + dy * dy;
            if (len2 < 0.0001f) continue;

            /* One over the length, by Newton's method: there is no square
               root to call here. Three rounds from a halfway guess is more
               than a coordinate needs. */
            float guess = len2 > 1 ? len2 * 0.5f : 1.0f;
            for (int k = 0; k < 12; k++) guess = 0.5f * (guess + len2 / guess);
            float nx = -dy / guess * half, ny = dx / guess * half;

            /* Four points and one subpath, on the stack, which is
               thirty six bytes rather than the hundred and thirty
               kilobytes a whole path would be. */
            svpt quad[4];
            int one[1] = { 0 };
            quad[0].x = p0.x + nx; quad[0].y = p0.y + ny;
            quad[1].x = p1.x + nx; quad[1].y = p1.y + ny;
            quad[2].x = p1.x - nx; quad[2].y = p1.y - ny;
            quad[3].x = p0.x - nx; quad[3].y = p0.y - ny;
            sv_fill_pts(cv, quad, 4, one, 1, colour, 0, alpha);
        }
    }
}

/* --- the drawing, as a document ------------------------------------------
 *
 * An SVG is markup, so the same parser that reads a page reads this: the
 * shapes are elements and everything about them is an attribute. What is
 * below walks that, keeping a transform as it descends, and draws whatever
 * it recognises.
 */

/* A colour, from an attribute or from the style on the same element.
 * Returns whether there is one to draw at all: "none" is a real answer and
 * a different one from "not said", which inherits. */
static inline int sv_paint(const ddoc *d, int el, const char *name, u32 *out) {
    const char *v = dom_attr(d, el, name);

    if (!v || !*v) {
        /* Also look in a style attribute, where a drawing program is at
           least as likely to have put it. */
        const char *st = dom_attr(d, el, "style");
        if (!st) return 0;

        int nlen = 0;
        while (name[nlen]) nlen++;
        for (const char *q = st; *q; q++) {
            if (w_lower(*q) != w_lower(name[0])) continue;
            int i = 0;
            while (i < nlen && q[i] && w_lower(q[i]) == w_lower(name[i])) i++;
            if (i != nlen) continue;
            const char *r = q + nlen;
            while (*r == ' ') r++;
            if (*r != ':') continue;
            r++;
            while (*r == ' ') r++;
            v = r;
            break;
        }
        if (!v || !*v) return 0;
    }

    while (*v == ' ') v++;
    if (w_starts_fold(v, "none")) return 0;

    /* A gradient or a pattern is a reference to something this does not
       draw. Rather than leaving a hole where the shape was, it is filled
       flat in a middle grey: a shape in roughly the wrong colour reads as
       part of the drawing, and a hole reads as a broken page. */
    if (w_starts_fold(v, "url(")) { *out = 0x9A9A9A; return 1; }

    return css_color(v, out) ? 1 : 0;
}

static inline float sv_attr_num(const ddoc *d, int el, const char *name,
                                float dflt) {
    const char *v = dom_attr(d, el, name);
    if (!v || !*v) return dflt;
    const char *at = v;
    return sv_number(&at);
}

/* transform="translate(4 8) scale(2) rotate(30)", applied left to right. */
static inline svmat sv_transform(const ddoc *d, int el, svmat parent) {
    const char *v = dom_attr(d, el, "transform");
    if (!v || !*v) return parent;

    svmat m = parent;
    const char *p = v;
    while (*p) {
        while (*p && (sv_space(*p) || *p == ')')) p++;
        if (!*p) break;

        const char *name = p;
        while (*p && *p != '(') p++;
        if (!*p) break;
        p++;

        float a[6];
        int n = 0;
        while (n < 6 && sv_more(p)) a[n++] = sv_number(&p);

        svmat t = sv_identity();
        if (w_starts_fold(name, "translate")) {
            t.e = a[0];
            t.f = n > 1 ? a[1] : 0;
        } else if (w_starts_fold(name, "scale")) {
            t.a = a[0];
            t.d = n > 1 ? a[1] : a[0];
        } else if (w_starts_fold(name, "matrix") && n >= 6) {
            t.a = a[0]; t.b = a[1]; t.c = a[2];
            t.d = a[3]; t.e = a[4]; t.f = a[5];
        } else if (w_starts_fold(name, "rotate") && n >= 1) {
            /* Degrees, and there is no sine here to ask for either, so the
               series again. Reduced into a turn first, because the series
               is only quick near zero. */
            float deg = a[0];
            while (deg > 180) deg -= 360;
            while (deg < -180) deg += 360;
            float r = deg * 3.14159265358979f / 180.0f;

            float sn = r, term = r;
            for (int k = 1; k < 10; k++) {
                term *= -r * r / (float)((2 * k) * (2 * k + 1));
                sn += term;
            }
            float cs = 1, ct = 1;
            for (int k = 1; k < 10; k++) {
                ct *= -r * r / (float)((2 * k - 1) * (2 * k));
                cs += ct;
            }
            t.a = cs; t.b = sn; t.c = -sn; t.d = cs;

            /* rotate(angle cx cy) turns about a point rather than the
               origin, which is how a drawing program writes one. */
            if (n >= 3) {
                svmat to = sv_identity(); to.e = a[1]; to.f = a[2];
                svmat back = sv_identity(); back.e = -a[1]; back.f = -a[2];
                t = sv_mul(to, sv_mul(t, back));
            }
        }
        m = sv_mul(m, t);

        while (*p && *p != ')') p++;
        if (*p) p++;
    }
    return m;
}

/* One shape, whichever kind it is, as a path. */
static inline int sv_shape(const ddoc *d, int el, svmat m, svpath *path) {
    const char *name = dom_tag_name(d, el);
    path->n = 0;
    path->nsub = 0;

    if (w_same_fold(name, "path")) {
        const char *dd = dom_attr(d, el, "d");
        if (!dd || !*dd) return 0;
        sv_path_data(path, m, dd);
        return path->n > 1;
    }

    if (w_same_fold(name, "rect")) {
        float x = sv_attr_num(d, el, "x", 0), y = sv_attr_num(d, el, "y", 0);
        float w = sv_attr_num(d, el, "width", 0);
        float h = sv_attr_num(d, el, "height", 0);
        if (w <= 0 || h <= 0) return 0;
        sv_begin(path);
        sv_add(path, m, x, y);
        sv_add(path, m, x + w, y);
        sv_add(path, m, x + w, y + h);
        sv_add(path, m, x, y + h);
        return 1;
    }

    if (w_same_fold(name, "circle") || w_same_fold(name, "ellipse")) {
        float cx = sv_attr_num(d, el, "cx", 0), cy = sv_attr_num(d, el, "cy", 0);
        float rx, ry;
        if (w_same_fold(name, "circle")) {
            rx = ry = sv_attr_num(d, el, "r", 0);
        } else {
            rx = sv_attr_num(d, el, "rx", 0);
            ry = sv_attr_num(d, el, "ry", 0);
        }
        if (rx <= 0 || ry <= 0) return 0;

        /* Four cubics, which is how everything draws a circle: the magic
           number is what makes the curve touch the quarter points. */
        const float k = 0.5522847f;
        sv_begin(path);
        sv_add(path, m, cx + rx, cy);
        sv_cubic(path, m, cx + rx, cy, cx + rx, cy + ry * k,
                 cx + rx * k, cy + ry, cx, cy + ry);
        sv_cubic(path, m, cx, cy + ry, cx - rx * k, cy + ry,
                 cx - rx, cy + ry * k, cx - rx, cy);
        sv_cubic(path, m, cx - rx, cy, cx - rx, cy - ry * k,
                 cx - rx * k, cy - ry, cx, cy - ry);
        sv_cubic(path, m, cx, cy - ry, cx + rx * k, cy - ry,
                 cx + rx, cy - ry * k, cx + rx, cy);
        return 1;
    }

    if (w_same_fold(name, "line")) {
        sv_begin(path);
        sv_add(path, m, sv_attr_num(d, el, "x1", 0), sv_attr_num(d, el, "y1", 0));
        sv_add(path, m, sv_attr_num(d, el, "x2", 0), sv_attr_num(d, el, "y2", 0));
        return 1;
    }

    if (w_same_fold(name, "polygon") || w_same_fold(name, "polyline")) {
        const char *pts = dom_attr(d, el, "points");
        if (!pts || !*pts) return 0;
        sv_begin(path);
        while (sv_more(pts)) {
            float x = sv_number(&pts);
            float y = sv_number(&pts);
            sv_add(path, m, x, y);
        }
        return path->n > 1;
    }

    return 0;
}

/* --- the whole drawing ----------------------------------------------------
 *
 * `want_w` and `want_h` are what the page asked for, or zero to take the
 * drawing's own size. The viewBox is what makes that possible: it says what
 * range of coordinates the shapes use, and everything else is a scale.
 */
static inline int svg_render(const char *xml, int len, int want_w, int want_h,
                             picture *out, u32 bg) {
    out->w = out->h = 0;
    out->rgb = 0;

    ddoc *doc = (ddoc *)malloc((u32)sizeof(ddoc));
    if (!doc) return SVG_TOO_BIG;
    dom_parse(doc, xml, len);

    int root = -1;
    for (int i = 0; i < doc->count; i++)
        if (doc->nodes[i].kind == DN_ELEMENT
            && w_same_fold(dom_tag_name(doc, i), "svg")) { root = i; break; }
    if (root < 0) { free(doc); return SVG_NOT_SVG; }

    /* What the drawing thinks its own coordinates are. */
    float vx = 0, vy = 0, vw = 0, vh = 0;
    const char *vb = dom_attr_fold(doc, root, "viewBox");
    if (vb && *vb) {
        vx = sv_number(&vb); vy = sv_number(&vb);
        vw = sv_number(&vb); vh = sv_number(&vb);
    }
    float own_w = sv_attr_num(doc, root, "width", 0);
    float own_h = sv_attr_num(doc, root, "height", 0);
    if (vw <= 0 || vh <= 0) { vw = own_w; vh = own_h; }
    if (vw <= 0 || vh <= 0) { vw = 100; vh = 100; }

    int w = want_w > 0 ? want_w : (own_w > 0 ? (int)own_w : (int)vw);
    int h = want_h > 0 ? want_h : (own_h > 0 ? (int)own_h : (int)vh);
    if (w <= 0) w = (int)vw;
    if (h <= 0) h = (int)vh;
    if (w > SVG_MAX_SIDE) w = SVG_MAX_SIDE;
    if (h > SVG_MAX_SIDE) h = SVG_MAX_SIDE;
    if (w < 1 || h < 1) { free(doc); return SVG_TOO_BIG; }

    u8 *rgb = (u8 *)malloc((u32)(w * h * 3));
    if (!rgb) { free(doc); return SVG_TOO_BIG; }
    for (int i = 0; i < w * h; i++) {
        rgb[i * 3] = (u8)((bg >> 16) & 0xFF);
        rgb[i * 3 + 1] = (u8)((bg >> 8) & 0xFF);
        rgb[i * 3 + 2] = (u8)(bg & 0xFF);
    }

    svcanvas cv;
    cv.rgb = rgb; cv.w = w; cv.h = h;

    /* The one transform every shape starts from: the viewBox, scaled to
       what was asked for, keeping the drawing's proportions and centring
       what is left over — which is what preserveAspectRatio means by
       default, and what a logo squashed to a square looks wrong without. */
    float sx = (float)w / vw, sy = (float)h / vh;
    float sc = sx < sy ? sx : sy;
    svmat base = sv_identity();
    base.a = sc; base.d = sc;
    base.e = -vx * sc + ((float)w - vw * sc) / 2;
    base.f = -vy * sc + ((float)h - vh * sc) / 2;

    svpath *path = (svpath *)malloc((u32)sizeof(svpath));
    if (!path) { free(doc); free(rgb); return SVG_TOO_BIG; }

    /* Depth first, in document order, which is also paint order: what comes
       later in the file is drawn on top, and that is the whole of what an
       SVG says about stacking. */
    int drawn = 0;
    svmat stack[32];
    int depth = 0;
    stack[0] = base;

    for (int el = root; el >= 0; el = dom_next(doc, el, root)) {
        if (doc->nodes[el].kind != DN_ELEMENT) continue;

        /* The transform of an element is its parent's with its own after
           it, so the chain is walked from this element up rather than
           carried down — the walk is not a recursion and has no stack of
           its own to hang it on. */
        svmat m = base;
        int chain[32];
        int n = 0;
        for (int a = el; a > root && n < 32; a = doc->nodes[a].parent)
            chain[n++] = a;
        for (int i = n - 1; i >= 0; i--) m = sv_transform(doc, chain[i], m);

        if (!sv_shape(doc, el, m, path)) continue;

        u32 fill = 0x000000;
        int has_fill = sv_paint(doc, el, "fill", &fill);
        /* Unsaid means black, which is what the format says and is why a
           path with no fill attribute at all still draws. */
        const char *fv = dom_attr(doc, el, "fill");
        if (!has_fill && (!fv || !*fv)
            && !w_same_fold(dom_tag_name(doc, el), "polyline")
            && !w_same_fold(dom_tag_name(doc, el), "line")) {
            has_fill = 1;
            fill = 0x000000;
        }

        float opacity = sv_attr_num(doc, el, "opacity", 1.0f);
        float fo = sv_attr_num(doc, el, "fill-opacity", 1.0f) * opacity;
        if (fo < 0) fo = 0;
        if (fo > 1) fo = 1;

        if (has_fill && fo > 0.004f) {
            const char *rule = dom_attr(doc, el, "fill-rule");
            int evenodd = rule && w_starts_fold(rule, "evenodd");
            sv_fill(&cv, path, fill, evenodd, fo);
            drawn++;
        }

        u32 stroke = 0;
        if (sv_paint(doc, el, "stroke", &stroke)) {
            float sw = sv_attr_num(doc, el, "stroke-width", 1.0f) * sc;
            float so = sv_attr_num(doc, el, "stroke-opacity", 1.0f) * opacity;
            if (so > 0.004f) {
                sv_stroke(&cv, path, stroke, sw, so > 1 ? 1 : so);
                drawn++;
            }
        }
    }
    (void)depth;
    (void)stack;

    free(path);
    free(doc);

    if (!drawn) { free(rgb); return SVG_NOTHING_IN_IT; }

    out->w = w;
    out->h = h;
    out->rgb = rgb;
    return SVG_OK;
}

static inline const char *svg_why(int rc) {
    switch (rc) {
    case SVG_NOT_SVG:       return "not a drawing";
    case SVG_TOO_BIG:       return "too big to hold";
    case SVG_NOTHING_IN_IT: return "nothing in it this knows how to draw";
    default:                return "it does not draw";
    }
}
