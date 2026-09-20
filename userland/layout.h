/* Turning a styled tree into a list of things to draw.
 *
 * Two kinds of box and one pass. A block box takes the full width it is
 * given, stacks its children down the page, and carries a background, a
 * border and a margin. Inline content flows into lines inside whichever
 * block contains it, and a line is as tall as the tallest thing on it.
 * That is the part of CSS layout that decides whether a page is readable.
 *
 * What is deliberately not here is the part that decides whether a page is
 * pixel for pixel what its author saw: floats, absolute and fixed
 * positioning, flex and grid tracks, and tables with column widths. Every
 * one of those turns a page from one column into several, and a browser
 * that does half of them puts things in places nobody chose. Elements that
 * ask for them are laid out as ordinary blocks, which is the arrangement
 * their markup describes, and it reads top to bottom in the order it was
 * written.
 *
 * The output is a flat display list in document coordinates. Drawing is
 * somebody else's problem and so is scrolling: both are a subtraction.
 */
#pragma once
#include "zelr.h"
#include "web.h"
#include "dom.h"
#include "css.h"
#include "facetext.h"

#define LAY_ITEMS  20000
#define LAY_TEXT   (192 * 1024)
#define LAY_LINKS  1400
#define LAY_LINE   400
#define LAY_DEPTH  DOM_DEPTH

enum { LK_BOX = 1, LK_TEXT, LK_BULLET, LK_IMAGE, LK_FIELD };

typedef struct {
    int x, y, w, h;               /* y is down the document, not the window */
    int at;                       /* LK_TEXT: the string */
    u32 color, bg, border;
    short face;
    int  node;                    /* which element this came from */
    int  link;                    /* into links, or -1 */
    unsigned char kind, under, strike, radius;
    unsigned char bt, br, bb, bl;
    unsigned char has_bg;
} litem;

typedef struct {
    int node;                     /* the anchor */
    int href;                     /* into the text arena */
} llink;

/* A picture that has arrived, as far as the layout is concerned: which
   element it belongs to and how big it is. Not the pixels — those belong to
   whoever fetched it, and the layout has no business decoding anything. */
typedef struct {
    int node;
    int w, h;
} limage;

typedef struct {
    litem items[LAY_ITEMS];
    int   nitems;
    char  text[LAY_TEXT];
    int   used;
    llink links[LAY_LINKS];
    int   nlinks;
    int   height;
    int   overflowed;
} ldoc;

/* --- picking a face ------------------------------------------------------
 *
 * A page asks for a size in whatever unit it likes and gets the nearest one
 * that exists, at the weight and spacing asked for. Matching the weight
 * matters more than matching the size: bold text one pixel off reads as
 * bold, and regular text at the right size does not read as bold at all. */
static inline int face_pick(int px, int bold, int mono) {
    int best = 0, best_d = 1 << 20;
    for (int i = 0; i < TFACE_SIZES; i++) {
        const face_t *f = &tface_faces[i];
        if ((f->bold != 0) != (bold != 0)) continue;
        if ((f->mono != 0) != (mono != 0)) continue;
        int d = f->size - px;
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best_d == (1 << 20)) {
        /* Nothing at that weight and spacing, so fall back on size alone
           rather than on face zero, which is the smallest there is. */
        for (int i = 0; i < TFACE_SIZES; i++) {
            int d = tface_faces[i].size - px;
            if (d < 0) d = -d;
            if (d < best_d) { best_d = d; best = i; }
        }
    }
    return best;
}

static inline const face_t *tface_of(int which) {
    if (which < 0 || which >= TFACE_SIZES) which = 0;
    return &tface_faces[which];
}

static inline int tface_h(int which) { return tface_of(which)->size; }

static inline int tface_wn(const char *s, int n, int which) {
    const face_t *f = tface_of(which);
    int w = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < FACE_FIRST || c > FACE_LAST) c = ' ';
        w += f->glyphs[c - FACE_FIRST].advance;
    }
    return w;
}

static inline int tface_w(const char *s, int which) {
    int n = 0;
    while (s[n]) n++;
    return tface_wn(s, n, which);
}

/* --- form controls --------------------------------------------------------
 *
 * A control is a box whose contents are a value rather than markup. The
 * layout decides how big one is; what is written inside it is read out of
 * the document at the moment of drawing, because a value changes without
 * the page being laid out again, and somebody typing into a field would
 * otherwise cost a reflow per keystroke.
 *
 * The kinds are named here rather than in the browser because both have to
 * agree about them: the layout decides how much room a checkbox takes and
 * the browser decides what a checkbox looks like, and those are the same
 * checkbox.
 */
enum { CTL_NONE = 0, CTL_TEXT, CTL_PASSWORD, CTL_BUTTON,
       CTL_CHECK, CTL_RADIO, CTL_AREA, CTL_SELECT, CTL_HIDDEN };

static inline int lay_same_fold(const char *a, const char *b) {
    for (int i = 0;; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static inline int lay_control_kind(const ddoc *d, int el) {
    if (el < 0 || el >= d->count || d->nodes[el].kind != DN_ELEMENT)
        return CTL_NONE;
    int tag = d->nodes[el].tag;
    if (tag == T_TEXTAREA) return CTL_AREA;
    if (tag == T_SELECT)   return CTL_SELECT;
    if (tag == T_BUTTON)   return CTL_BUTTON;
    if (tag != T_INPUT)    return CTL_NONE;

    /* A type nobody wrote is text, and a type nothing here knows is text
       too: that is what a browser does with the ones invented after it was
       written, and it is the answer that leaves the field usable. */
    const char *t = dom_attr(d, el, "type");
    if (!t || !*t) return CTL_TEXT;
    if (lay_same_fold(t, "hidden"))   return CTL_HIDDEN;
    if (lay_same_fold(t, "password")) return CTL_PASSWORD;
    if (lay_same_fold(t, "checkbox")) return CTL_CHECK;
    if (lay_same_fold(t, "radio"))    return CTL_RADIO;
    if (lay_same_fold(t, "submit") || lay_same_fold(t, "button")
        || lay_same_fold(t, "reset") || lay_same_fold(t, "image"))
        return CTL_BUTTON;
    return CTL_TEXT;
}

/* The words on a control, which are not what it submits: a button carries a
   label and sends a value, and a password shows none of what it holds. */
static inline const char *lay_control_label(const ddoc *d, int el, int kind) {
    static char buf[256];
    if (kind == CTL_BUTTON) {
        const char *v = dom_attr(d, el, "value");
        if (v && *v) return v;
        if (d->nodes[el].tag == T_BUTTON) {
            dom_text_content(d, el, buf, (int)sizeof(buf));
            return buf[0] ? buf : "Button";
        }
        const char *t = dom_attr(d, el, "type");
        if (t && lay_same_fold(t, "reset")) return "Reset";
        return "Submit";
    }
    if (kind == CTL_SELECT) {
        /* Whatever the first option says, because nothing here opens one.
           A list that cannot be opened, showing its first entry, is at
           least the value it would send. */
        for (int c = d->nodes[el].first; c >= 0; c = d->nodes[c].next)
            if (d->nodes[c].kind == DN_ELEMENT && d->nodes[c].tag == T_OPTION) {
                dom_text_content(d, c, buf, (int)sizeof(buf));
                return buf;
            }
        return "";
    }
    if (kind == CTL_AREA) {
        const char *v = dom_attr(d, el, "value");
        if (v) return v;
        dom_text_content(d, el, buf, (int)sizeof(buf));
        return buf;
    }
    const char *v = dom_attr(d, el, "value");
    return v ? v : "";
}

/* --- the run of the layout ----------------------------------------------- */

typedef struct {
    const ddoc   *d;
    const csheet *s;
    const cindex *x;
    const cmatch *m;
    const cinline *inl;           /* one per DOM node, or null */

    /* The pictures that arrived, if any. A page laid out before they have
       is laid out with their alt text, which is what happens on the first
       pass and is corrected on the second. */
    const limage *imgs;
    int           nimgs;
    ldoc   *out;
    int     root_px;

    /* the line being built */
    int line_at, line_n;          /* items[line_at ..] are this line's */
    int pen, line_top, line_h, line_base;
    int line_left, line_width;
    int align;
    int pending_space;
    int line_started;

    int cur_link;                 /* into links, or -1 */
    int list_depth;
    int list_count[LAY_DEPTH];
} lctx;

static inline int lay_put(lctx *L, const char *s, int n) {
    ldoc *o = L->out;
    if (o->used + n + 1 >= LAY_TEXT) { o->overflowed = 1; return -1; }
    int at = o->used;
    for (int i = 0; i < n; i++) o->text[o->used++] = s[i];
    o->text[o->used++] = 0;
    return at;
}

/* A number out of an attribute. width="200" is written without a unit,
   and what follows the digits is ignored the way a browser ignores it. */
static inline int lay_number(const char *s) {
    int v = 0, any = 0;
    while (*s == ' ' || *s == '	') s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
        any = 1;
        if (v > 100000) return 100000;
    }
    return any ? v : 0;
}

static inline const limage *lay_image_of(const lctx *L, int node) {
    for (int i = 0; i < L->nimgs; i++)
        if (L->imgs[i].node == node) return &L->imgs[i];
    return 0;
}

static inline litem *lay_item(lctx *L) {
    ldoc *o = L->out;
    if (o->nitems >= LAY_ITEMS) { o->overflowed = 1; return 0; }
    litem *it = &o->items[o->nitems++];
    it->x = it->y = it->w = it->h = 0;
    it->at = -1;
    it->color = 0x1A1A1A; it->bg = 0xFFFFFF; it->border = 0xD0D0D0;
    it->face = 0; it->node = -1; it->link = -1;
    it->kind = LK_TEXT; it->under = it->strike = it->radius = 0;
    it->bt = it->br = it->bb = it->bl = 0;
    it->has_bg = 0;
    return it;
}

/* --- lines ---------------------------------------------------------------
 *
 * A line is finished when the next word will not fit, when a block starts,
 * or when the text says so. Finishing it is where alignment happens, because
 * alignment is the only thing that cannot be decided until the width of the
 * line is known, and it is also where every item on the line is dropped onto
 * a common baseline. Text of two sizes on one line sits on one baseline;
 * lining up their tops instead is the thing that makes mixed type look like
 * a ransom note. */
static inline void lay_line_end(lctx *L, int *y) {
    if (!L->line_started) return;
    ldoc *o = L->out;

    int used = L->pen - L->line_left;
    int slack = L->line_width - used;
    if (slack < 0) slack = 0;
    int shift = 0;
    if (L->align == A_CENTER) shift = slack / 2;
    else if (L->align == A_RIGHT) shift = slack;

    for (int i = L->line_at; i < o->nitems; i++) {
        litem *it = &o->items[i];
        it->x += shift;
        /* Sit on the baseline rather than on the top of the line. */
        if (it->kind == LK_TEXT)
            it->y = L->line_top + L->line_base - tface_h(it->face);
        else
            it->y = L->line_top + L->line_base - it->h;
        if (it->y < L->line_top) it->y = L->line_top;
    }

    *y = L->line_top + L->line_h;
    L->line_started = 0;
    L->line_n = 0;
    L->pending_space = 0;
}

static inline void lay_line_start(lctx *L, int y, int left, int width,
                                  int align) {
    L->line_at = L->out->nitems;
    L->line_n = 0;
    L->line_top = y;
    L->line_h = 0;
    L->line_base = 0;
    L->pen = left;
    L->line_left = left;
    L->line_width = width;
    L->align = align;
    L->line_started = 1;
}

/* Every item on a line contributes its own height, and the line is as tall
   as the tallest. The baseline sits at four fifths of the tallest, which is
   about where the baseline of a face sits inside its own line box. */
static inline void lay_line_fit(lctx *L, int h, int line_pct) {
    int box = h * line_pct / 100;
    if (box > L->line_h) L->line_h = box;
    int base = h * 4 / 5 + (box - h) / 2;
    if (base > L->line_base) L->line_base = base;
}

/* --- text ---------------------------------------------------------------- */

static inline int lay_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static inline void lay_word(lctx *L, const char *s, int n, const cstyle *st,
                            int *y, int face) {
    if (n <= 0) return;
    int w = tface_wn(s, n, face);
    int sp = L->pending_space && L->pen > L->line_left
             ? tface_wn(" ", 1, face) : 0;

    if (L->pen + sp + w > L->line_left + L->line_width
        && L->pen > L->line_left && st->white != WS_NOWRAP) {
        int left = L->line_left, width = L->line_width, al = L->align;
        lay_line_end(L, y);
        lay_line_start(L, *y, left, width, al);
        sp = 0;
    }
    L->pen += sp;

    litem *it = lay_item(L);
    if (!it) return;
    it->kind = LK_TEXT;
    it->at = lay_put(L, s, n);
    it->x = L->pen;
    it->w = w;
    it->h = tface_h(face);
    it->face = (short)face;
    it->color = st->color;
    it->under = st->underline;
    it->strike = st->strike;
    it->link = L->cur_link;
    L->pen += w;
    L->line_n++;
    lay_line_fit(L, tface_h(face), st->line_h);
    L->pending_space = 0;
}

static inline void lay_text_run(lctx *L, const char *s, const cstyle *st,
                                int *y) {
    int face = face_pick(st->font_px, st->bold, st->mono);
    if (st->white == WS_PRE) {
        int i = 0;
        for (;;) {
            int start = i;
            while (s[i] && s[i] != '\n') i++;
            if (i > start) {
                /* One item per line, so the spacing inside it is kept. */
                int w = tface_wn(s + start, i - start, face);
                litem *it = lay_item(L);
                if (!it) return;
                it->kind = LK_TEXT;
                it->at = lay_put(L, s + start, i - start);
                it->x = L->pen;
                it->w = w;
                it->h = tface_h(face);
                it->face = (short)face;
                it->color = st->color;
                it->link = L->cur_link;
                L->pen += w;
                L->line_n++;
                lay_line_fit(L, tface_h(face), st->line_h);
            } else {
                lay_line_fit(L, tface_h(face), st->line_h);
            }
            if (!s[i]) break;
            i++;
            int left = L->line_left, width = L->line_width, al = L->align;
            lay_line_end(L, y);
            lay_line_start(L, *y, left, width, al);
        }
        return;
    }

    int i = 0;
    while (s[i]) {
        if (lay_space(s[i])) { L->pending_space = 1; i++; continue; }
        int start = i;
        while (s[i] && !lay_space(s[i])) i++;
        lay_word(L, s + start, i - start, st, y, face);
    }
}

/* --- the walk ------------------------------------------------------------ */

static inline void lay_style(lctx *L, int el, const cstyle *parent,
                             cstyle *out, int pct_of) {
    css_inherit(out, parent);
    chit hits[CSS_HITS];
    int n = css_collect(L->s, L->x, L->d, el, L->m, hits);
    for (int i = 0; i < n; i++) {
        const crule *r = &L->s->rules[hits[i].rule];
        for (int k = 0; k < r->decl_n; k++)
            css_apply(L->s, &L->s->decls[r->decl_at + k], out, L->root_px,
                      pct_of);
    }

    /* And last, what the element says about itself. A style attribute beats
       every rule in every sheet no matter how specific, which is the one
       part of the cascade that needs no comparison to decide. */
    if (L->inl && L->inl[el].n > 0)
        for (int k = 0; k < L->inl[el].n; k++)
            css_apply(L->s, &L->s->decls[L->inl[el].at + k], out, L->root_px,
                      pct_of);
}

static void lay_block(lctx *L, int node, const cstyle *parent, int x,
                      int avail, int *y);

/* Inline content, which is everything between two blocks. Walked with an
   explicit style stack rather than by recursion, because an inline run can
   be nested as deep as the page is and this is the hot path. */
static inline void lay_inline(lctx *L, int node, const cstyle *parent, int *y) {
    const ddoc *d = L->d;
    cstyle stack[LAY_DEPTH];
    int stack_node[LAY_DEPTH];
    int sp = 0;
    stack[0] = *parent;
    stack_node[0] = -1;

    /* One subtree, in document order, popping styles on the way back up. */
    int at = node;
    while (at >= 0) {
        while (sp > 0 && stack_node[sp] >= 0) {
            /* Pop any style whose element we have left. */
            int owner = stack_node[sp];
            int still_inside = 0;
            for (int p = at; p >= 0; p = d->nodes[p].parent)
                if (p == owner) { still_inside = 1; break; }
            if (still_inside) break;
            if (d->nodes[owner].tag == T_A) L->cur_link = -1;
            sp--;
        }

        const dnode *n = &d->nodes[at];
        if (n->kind == DN_TEXT) {
            if (stack[sp].visible && n->text >= 0)
                lay_text_run(L, d->arena + n->text, &stack[sp], y);
        } else {
            cstyle st;
            lay_style(L, at, &stack[sp], &st, L->line_width);
            if (st.display == D_NONE) {
                /* Skip the subtree entirely. */
                int skip = at;
                while (skip >= 0 && d->nodes[skip].next < 0
                       && d->nodes[skip].parent != node)
                    skip = d->nodes[skip].parent;
                at = skip >= 0 ? d->nodes[skip].next : -1;
                if (skip == node) at = -1;
                continue;
            }
            if (n->tag == T_BR) {
                int left = L->line_left, width = L->line_width, al = L->align;
                lay_line_fit(L, st.font_px, st.line_h);
                lay_line_end(L, y);
                lay_line_start(L, *y, left, width, al);
            } else if (n->tag == T_IMG) {
                const limage *pic = lay_image_of(L, at);

                if (pic && pic->w > 0 && pic->h > 0) {
                    /* What the page asked for beats what the file is, and
                       what there is room for beats both: a picture wider
                       than the column would push everything else off it. */
                    int iw = pic->w, ih = pic->h;
                    const char *aw = dom_attr(d, at, "width");
                    const char *ah = dom_attr(d, at, "height");
                    int want_w = aw ? lay_number(aw) : 0;
                    int want_h = ah ? lay_number(ah) : 0;

                    if (want_w > 0 && want_h > 0) { iw = want_w; ih = want_h; }
                    else if (want_w > 0) { ih = ih * want_w / iw; iw = want_w; }
                    else if (want_h > 0) { iw = iw * want_h / ih; ih = want_h; }

                    if (iw > L->line_width && iw > 0) {
                        ih = ih * L->line_width / iw;
                        iw = L->line_width;
                    }
                    if (ih < 1) ih = 1;
                    if (iw < 1) iw = 1;

                    /* It sits on the line like a very tall word, so text
                       beside it flows the way text beside a picture does. */
                    if (L->pen + iw > L->line_left + L->line_width
                        && L->pen > L->line_left) {
                        int left = L->line_left, width = L->line_width;
                        int al = L->align;
                        lay_line_end(L, y);
                        lay_line_start(L, *y, left, width, al);
                    }

                    litem *it = lay_item(L);
                    if (it) {
                        it->kind = LK_IMAGE;
                        it->x = L->pen;
                        it->y = L->line_top;
                        it->w = iw;
                        it->h = ih;
                        it->node = at;
                        it->at = -1;
                        it->link = L->cur_link;
                        L->pen += iw;
                        lay_line_fit(L, ih, ih);
                        L->line_started = 1;
                        L->pending_space = 0;
                    }
                } else {
                    /* Nothing arrived, so it is the words it came with,
                       which is what alt text is for and a great deal more
                       use than a gap. */
                    const char *alt = dom_attr(d, at, "alt");
                    if (alt && *alt) {
                        cstyle s2 = st;
                        s2.color = 0x6B6B6B;
                        s2.italic = 1;
                        lay_text_run(L, alt, &s2, y);
                    }
                }
            } else if (lay_control_kind(d, at) != CTL_NONE) {
                int ck = lay_control_kind(d, at);
                int face = face_pick(st.font_px, st.bold, st.mono);
                int fw = 0, fh = tface_h(face) + 10;

                if (ck == CTL_CHECK || ck == CTL_RADIO) {
                    fw = fh = tface_h(face) + 2;
                } else if (ck == CTL_BUTTON) {
                    fw = tface_w(lay_control_label(d, at, ck), face) + 20;
                } else if (ck == CTL_AREA) {
                    const char *cols = dom_attr(d, at, "cols");
                    const char *rows = dom_attr(d, at, "rows");
                    int nc = cols ? lay_number(cols) : 0;
                    int nr = rows ? lay_number(rows) : 0;
                    if (nc < 1) nc = 28;
                    if (nr < 1) nr = 3;
                    fw = tface_wn("0", 1, face) * nc + 10;
                    fh = tface_h(face) * nr + nr * 4 + 8;
                } else {
                    const char *size = dom_attr(d, at, "size");
                    int nc = size ? lay_number(size) : 0;
                    if (nc < 1) nc = 20;
                    fw = tface_wn("0", 1, face) * nc + 10;
                }

                if (fw > L->line_width) fw = L->line_width;
                if (fw < 8) fw = 8;

                /* Hidden carries a value and takes no room, which is the
                   whole point of it. Drawn as nothing rather than as an
                   empty box, because an empty box is a field somebody will
                   try to type into. */
                if (ck != CTL_HIDDEN) {
                    if (L->pen + fw > L->line_left + L->line_width
                        && L->pen > L->line_left) {
                        int left = L->line_left, width = L->line_width;
                        int al = L->align;
                        lay_line_end(L, y);
                        lay_line_start(L, *y, left, width, al);
                    }

                    litem *it = lay_item(L);
                    if (it) {
                        it->kind = LK_FIELD;
                        it->x = L->pen;
                        it->y = L->line_top;
                        it->w = fw;
                        it->h = fh;
                        it->node = at;
                        it->at = -1;
                        it->face = (short)face;
                        it->color = st.color;
                        it->bg = st.background;
                        it->has_bg = st.has_bg;
                        it->link = L->cur_link;
                        L->pen += fw + 2;
                        lay_line_fit(L, fh, fh);
                        L->line_started = 1;
                        L->pending_space = 0;
                    }
                }

                /* What is inside a control belongs to the control and not
                   to the page: the text in a button is its label and the
                   text in a textarea is its value, and the box draws both
                   rather than letting them flow out after it. */
                int skip = at;
                while (skip >= 0 && d->nodes[skip].next < 0
                       && d->nodes[skip].parent != node)
                    skip = d->nodes[skip].parent;
                at = skip >= 0 ? d->nodes[skip].next : -1;
                if (skip == node) at = -1;
                continue;
            } else if (sp + 1 < LAY_DEPTH) {
                sp++;
                stack[sp] = st;
                stack_node[sp] = at;
                if (n->tag == T_A) {
                    const char *href = dom_attr(d, at, "href");
                    if (href && *href && L->out->nlinks < LAY_LINKS) {
                        L->out->links[L->out->nlinks].node = at;
                        L->out->links[L->out->nlinks].href =
                            lay_put(L, href, w_len(href));
                        L->cur_link = L->out->nlinks++;
                    }
                }
            }
        }

        /* Next in document order, staying inside the run. */
        if (d->nodes[at].first >= 0) { at = d->nodes[at].first; continue; }
        for (;;) {
            if (at == node) { at = -1; break; }
            if (d->nodes[at].next >= 0) { at = d->nodes[at].next; break; }
            at = d->nodes[at].parent;
            if (at < 0) break;
            if (at == node) { at = -1; break; }
        }
    }
    L->cur_link = -1;
}

/* True when a node starts a new block rather than flowing into a line. */
static inline int lay_is_block_node(lctx *L, int n, const cstyle *parent) {
    if (L->d->nodes[n].kind != DN_ELEMENT) return 0;
    cstyle st;
    lay_style(L, n, parent, &st, L->line_width);
    /* A flex container is a block: it takes the width it is given and
       starts on its own line. What is different about it is only what it
       does with its children, and that is decided inside lay_block. Left
       out of here, it was treated as inline and never reached the code
       that knows what a row is. */
    return st.display == D_BLOCK || st.display == D_LIST_ITEM
        || st.display == D_FLEX;
}


/* --- laying things out in a row ------------------------------------------
 *
 * Everything above lays out downward: a block starts where the last one
 * ended and takes the whole width. A flex container does not. Its children
 * go along an axis, share out whatever room is spare, and line up against
 * each other on the other axis — and since display:flex used to fall through
 * to block, every row on every modern page came out as a column.
 *
 * The algorithm here is the honest short version of the real one:
 *
 *   measure each child, by laying it out and throwing that away
 *   add up what they want; if it is more than there is, shrink them all in
 *     proportion; if it is less, hand the spare room to whoever asked to
 *     grow, and then position the rest according to justify-content
 *   lay each child out again, for real, at the place and width it ended up
 *     with, and slide it down the cross axis for align-items
 *
 * Measuring by laying out and throwing it away is not how a fast engine does
 * this — a fast one keeps a separate cheap pass that computes intrinsic
 * widths without emitting anything. It is, though, exactly right, because
 * the thing being measured is the thing that will be drawn rather than a
 * second implementation of it that can disagree.
 */

#define LAY_FLEX_MAX 32

/* Lays a node out and forgets it, returning how wide its content came out
   and how tall. Everything the layout was in the middle of is put back. */
static int lay_measure(lctx *L, int node, const cstyle *parent, int avail,
                       int *height) {
    /* Field by field rather than by copying the whole context.
     *
     * A struct assignment of something this size is a call to memcpy, and
     * there is no memcpy to call: this is a program with no library under
     * it. The compiler is within its rights and the linker says so. */
    int s_line_at = L->line_at, s_line_n = L->line_n;
    int s_pen = L->pen, s_top = L->line_top;
    int s_h = L->line_h, s_base = L->line_base;
    int s_left = L->line_left, s_width = L->line_width;
    int s_align = L->align, s_space = L->pending_space;
    int s_started = L->line_started, s_link = L->cur_link;
    int s_depth = L->list_depth;
    int s_count[LAY_DEPTH];
    for (int i = 0; i < LAY_DEPTH; i++) s_count[i] = L->list_count[i];

    int items = L->out->nitems, used = L->out->used, links = L->out->nlinks;

    int y = 0;
    lay_block(L, node, parent, 0, avail, &y);

    int right = 0;
    for (int i = items; i < L->out->nitems; i++) {
        int r = L->out->items[i].x + L->out->items[i].w;
        if (r > right) right = r;
    }

    *height = y;

    L->line_at = s_line_at; L->line_n = s_line_n;
    L->pen = s_pen; L->line_top = s_top;
    L->line_h = s_h; L->line_base = s_base;
    L->line_left = s_left; L->line_width = s_width;
    L->align = s_align; L->pending_space = s_space;
    L->line_started = s_started; L->cur_link = s_link;
    L->list_depth = s_depth;
    for (int i = 0; i < LAY_DEPTH; i++) L->list_count[i] = s_count[i];

    L->out->nitems = items;
    L->out->used = used;
    L->out->nlinks = links;
    return right;
}

/* A flex container. `cx` and `cw` are inside its own padding and border, and
   `y` is where its contents start and where they are finished. */
static void lay_flex(lctx *L, int node, const cstyle *st, int cx, int cw,
                     int *y) {
    const ddoc *d = L->d;

    int kid[LAY_FLEX_MAX];
    int n = 0;
    for (int c = d->nodes[node].first; c >= 0 && n < LAY_FLEX_MAX;
         c = d->nodes[c].next) {
        if (d->nodes[c].kind == DN_ELEMENT) kid[n++] = c;
    }
    if (n == 0) return;

    int column = st->flex_dir == FD_COLUMN || st->flex_dir == FD_COLUMN_REVERSE;
    int reverse = st->flex_dir == FD_ROW_REVERSE
               || st->flex_dir == FD_COLUMN_REVERSE;
    int gap = st->gap > 0 ? st->gap : 0;

    /* --- down the page is nearly what already happens ---------------------
     *
     * A column of flex items is a stack of blocks with a gap between them
     * and a chance to be reversed. The one thing worth doing properly is
     * the gap, because a page that asked for one and did not get it has
     * everything touching. */
    if (column) {
        for (int i = 0; i < n; i++) {
            int k = kid[reverse ? n - 1 - i : i];
            if (i) *y += gap;
            lay_block(L, k, st, cx, cw, y);
        }
        return;
    }

    /* --- along the line ---------------------------------------------------- */
    int want[LAY_FLEX_MAX], high[LAY_FLEX_MAX], grow[LAY_FLEX_MAX];
    int total = 0, grows = 0;

    for (int i = 0; i < n; i++) {
        int h = 0;
        /* Measured with room to spare, so the answer is how wide the child
           would like to be rather than how wide it was squeezed into. */
        int w = lay_measure(L, kid[i], st, cw > 0 ? cw * 3 : 2000, &h);
        if (w < 1) w = 1;
        if (w > cw && cw > 0) w = cw;

        cstyle own;
        lay_style(L, kid[i], st, &own, cw);
        if (own.width >= 0) w = own.width;

        want[i] = w;
        high[i] = h;
        grow[i] = own.grow > 0 ? own.grow : 0;
        grows += grow[i];
        total += w;
    }
    total += gap * (n - 1);

    /* Too wide: everything shrinks in proportion, down to a floor, because
       a row that overflows takes the page with it. */
    if (total > cw && total > 0) {
        int room = cw - gap * (n - 1);
        if (room < n) room = n;
        int sum = 0;
        for (int i = 0; i < n; i++) sum += want[i];
        for (int i = 0; i < n; i++) {
            want[i] = sum > 0 ? want[i] * room / sum : room / n;
            if (want[i] < 8) want[i] = 8;
        }
        total = 0;
        for (int i = 0; i < n; i++) total += want[i];
        total += gap * (n - 1);
    }

    int spare = cw - total;
    if (spare < 0) spare = 0;

    /* Anything that asked to grow takes the spare room first, and then
       there is none left to justify with — which is what `flex: 1` is for
       and why a page that uses it does not also use space-between. */
    if (grows > 0 && spare > 0) {
        int left = spare;
        for (int i = 0; i < n; i++) {
            if (!grow[i]) continue;
            int add = spare * grow[i] / grows;
            if (add > left) add = left;
            want[i] += add;
            left -= add;
        }
        if (left > 0) {
            for (int i = n - 1; i >= 0; i--)
                if (grow[i]) { want[i] += left; break; }
        }
        spare = 0;
    }

    /* Where the first one starts and what goes between them. */
    int pen = cx;
    int between = gap;
    if (spare > 0) {
        if (st->justify == JC_CENTER) pen += spare / 2;
        else if (st->justify == JC_END) pen += spare;
        else if (st->justify == JC_BETWEEN && n > 1) between += spare / (n - 1);
        else if (st->justify == JC_AROUND) {
            between += spare / n;
            pen += spare / (n * 2);
        } else if (st->justify == JC_EVENLY) {
            between += spare / (n + 1);
            pen += spare / (n + 1);
        }
    }

    /* The row is as tall as its tallest child, which is what the ones that
       are shorter are aligned within. */
    int tallest = 0;
    for (int i = 0; i < n; i++) if (high[i] > tallest) tallest = high[i];

    int top = *y;
    for (int idx = 0; idx < n; idx++) {
        int i = reverse ? n - 1 - idx : idx;

        int before = L->out->nitems;
        int child_y = top;
        lay_block(L, kid[i], st, pen, want[i], &child_y);

        /* Slid down the cross axis afterwards, which is cheaper than laying
           it out somewhere else and gives the same answer. Stretch is left
           where it is: making a child taller means laying it out again with
           a height it did not ask for, and a box at the top of its row is
           what stretch looks like when everything in it is the same height
           anyway. */
        int mine = high[i] > 0 ? high[i] : child_y - top;
        int dy = 0;
        if (st->align_items == AI_CENTER) dy = (tallest - mine) / 2;
        else if (st->align_items == AI_END) dy = tallest - mine;
        if (dy > 0)
            for (int k = before; k < L->out->nitems; k++)
                L->out->items[k].y += dy;

        pen += want[i] + between;
    }

    *y = top + tallest;
}

static void lay_block(lctx *L, int node, const cstyle *parent, int x,
                      int avail, int *y) {
    const ddoc *d = L->d;
    cstyle st;
    lay_style(L, node, parent, &st, avail);
    if (st.display == D_NONE || !st.visible) return;

    int ml = st.ml < 0 ? 0 : st.ml;
    int mr = st.mr < 0 ? 0 : st.mr;
    int box_w = avail - ml - mr;
    if (st.width >= 0 && st.width < box_w) {
        /* A width with auto margins is centred, which is how most pages put
           their content in the middle of a wide window. */
        if (st.ml < 0 && st.mr < 0) ml += (box_w - st.width) / 2;
        box_w = st.width;
    }
    if (st.max_width >= 0 && st.max_width < box_w) {
        if (st.ml < 0 && st.mr < 0) ml += (box_w - st.max_width) / 2;
        box_w = st.max_width;
    }
    if (box_w < 16) box_w = 16;

    *y += st.mt < 0 ? 0 : st.mt;
    int box_top = *y;

    /* The background goes in the list before the contents and its size is
       not known until they are laid out, so its slot is taken now and
       filled in at the end. */
    int slot = -1;
    if (st.has_bg || st.bt || st.br || st.bb || st.bl) {
        litem *bg = lay_item(L);
        if (bg) {
            slot = L->out->nitems - 1;
            bg->kind = LK_BOX;
            bg->node = node;
        }
    }

    int cx = x + ml + st.bl + st.pl;
    int cw = box_w - st.bl - st.br - st.pl - st.pr;
    if (cw < 16) cw = 16;
    *y += st.bt + st.pt;

    /* A list item's marker sits in the padding its list reserved. */
    if (st.display == D_LIST_ITEM && st.list != LS_NONE) {
        litem *b = lay_item(L);
        if (b) {
            b->kind = LK_BULLET;
            b->node = node;
            b->color = st.color;
            b->face = (short)face_pick(st.font_px, 0, 0);
            b->x = cx - 18;
            b->y = *y;
            b->w = 14;
            b->h = st.font_px;
            if (st.list == LS_DECIMAL) {
                int idx = ++L->list_count[L->list_depth < LAY_DEPTH
                                         ? L->list_depth : 0];
                char num[12];
                int w = 0, q = idx;
                char tmp[12]; int t = 0;
                if (!q) tmp[t++] = '0';
                while (q) { tmp[t++] = (char)('0' + q % 10); q /= 10; }
                while (t) num[w++] = tmp[--t];
                num[w++] = '.';
                b->at = lay_put(L, num, w);
                b->w = tface_wn(num, w, b->face);
                b->x = cx - b->w - 6;
            } else {
                b->at = -1;
            }
        }
    }
    if (st.display == D_LIST_ITEM && L->list_depth < LAY_DEPTH)
        L->list_count[L->list_depth] = L->list_count[L->list_depth];

    /* A list resets the numbering of the items directly inside it. */
    int tag = d->nodes[node].tag;
    int pushed = 0;
    if (tag == T_UL || tag == T_OL) {
        if (L->list_depth + 1 < LAY_DEPTH) {
            L->list_depth++;
            L->list_count[L->list_depth] = 0;
            pushed = 1;
        }
    }

    /* A flex container lays its children along a line rather than down
       the page, so it does not use the walk below at all. */
    if (st.display == D_FLEX) {
        lay_flex(L, node, &st, cx, cw, y);
    } else {

    /* The children: consecutive inline ones share a line, each block one
       starts on its own. */
    int child = d->nodes[node].first;
    int inline_open = 0;
    while (child >= 0) {
        int next = d->nodes[child].next;
        int is_block = lay_is_block_node(L, child, &st);
        if (is_block) {
            if (inline_open) { lay_line_end(L, y); inline_open = 0; }
            lay_block(L, child, &st, cx, cw, y);
        } else {
            if (!inline_open) {
                lay_line_start(L, *y, cx + (st.indent > 0 ? st.indent : 0),
                               cw, st.align);
                inline_open = 1;
            }
            if (d->nodes[child].kind == DN_TEXT) {
                if (d->nodes[child].text >= 0)
                    lay_text_run(L, d->arena + d->nodes[child].text, &st, y);
            } else {
                lay_inline(L, child, &st, y);
            }
        }
        child = next;
    }
    if (inline_open) lay_line_end(L, y);
    }
    if (pushed) L->list_depth--;

    *y += st.pb + st.bb;
    int box_h = *y - box_top;
    if (st.height >= 0 && st.height > box_h) {
        *y += st.height - box_h;
        box_h = st.height;
    }

    /* A horizontal rule is a border on a box with nothing in it, and a box
       with nothing in it is no height at all. */
    if (tag == T_HR && box_h <= 0) box_h = st.bt ? st.bt : 1;

    if (slot >= 0) {
        litem *bg = &L->out->items[slot];
        bg->x = x + ml;
        bg->y = box_top;
        bg->w = box_w;
        bg->h = box_h;
        bg->bg = st.background;
        bg->has_bg = st.has_bg;
        bg->border = st.border_color;
        bg->bt = (unsigned char)(st.bt > 255 ? 255 : st.bt);
        bg->br = (unsigned char)(st.br > 255 ? 255 : st.br);
        bg->bb = (unsigned char)(st.bb > 255 ? 255 : st.bb);
        bg->bl = (unsigned char)(st.bl > 255 ? 255 : st.bl);
        bg->radius = (unsigned char)(st.radius > 40 ? 40 : st.radius);
    }

    *y += st.mb < 0 ? 0 : st.mb;
}

/* --- the whole page ------------------------------------------------------ */

static inline void lay_run(ldoc *out, const ddoc *d, const csheet *s,
                           const cindex *x, const cmatch *m,
                           const cinline *inl, const limage *imgs, int nimgs,
                           int width, int root_px) {
    out->nitems = 0;
    out->used = 0;
    out->nlinks = 0;
    out->overflowed = 0;

    lctx L;
    L.d = d; L.s = s; L.x = x; L.m = m; L.inl = inl;
    L.imgs = imgs; L.nimgs = nimgs;
    L.out = out; L.root_px = root_px;
    L.line_started = 0; L.pending_space = 0; L.cur_link = -1;
    L.line_at = 0; L.line_n = 0; L.pen = 0;
    L.line_top = 0; L.line_h = 0; L.line_base = 0;
    L.line_left = 0; L.line_width = width; L.align = A_LEFT;
    L.list_depth = 0;
    for (int i = 0; i < LAY_DEPTH; i++) L.list_count[i] = 0;

    cstyle root;
    css_default_style(&root, root_px);

    int y = 0;
    int start = d->body >= 0 ? d->body : d->root;
    if (start >= 0) lay_block(&L, start, &root, 0, width, &y);
    out->height = y;
    if (d->overflowed) out->overflowed = 1;
}

/* --- hit testing --------------------------------------------------------- */

/* The link under a point, or -1. Walked backwards so the thing drawn last,
   which is the thing on top, is the thing found. */
static inline int lay_link_at(const ldoc *o, int x, int y) {
    for (int i = o->nitems - 1; i >= 0; i--) {
        const litem *it = &o->items[i];
        if (it->link < 0 || it->kind != LK_TEXT) continue;
        if (x >= it->x && x < it->x + it->w
            && y >= it->y - 2 && y < it->y + it->h + 2) return it->link;
    }
    return -1;
}

/* And the element, for anything that wants to know what was clicked. */
static inline int lay_node_at(const ldoc *o, int x, int y) {
    for (int i = o->nitems - 1; i >= 0; i--) {
        const litem *it = &o->items[i];
        if (it->node < 0) continue;
        if (x >= it->x && x < it->x + it->w
            && y >= it->y && y < it->y + it->h) return it->node;
    }
    return -1;
}
