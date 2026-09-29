/* Turning a styled tree into a list of things to draw.
 *
 * Two kinds of box and one pass. A block box takes the full width it is
 * given, stacks its children down the page, and carries a background, a
 * border and a margin. Inline content flows into lines inside whichever
 * block contains it, and a line is as tall as the tallest thing on it.
 * That is the part of CSS layout that decides whether a page is readable.
 *
 * Around that, the arrangements that turn a page from one column into
 * several: flex rows (which wrap), grids of tracks and named areas, tables
 * with column widths, floats that lines flow round, inline-blocks placed
 * like words, and absolute and relative positioning -- each the short
 * honest version described where it is done, with what it leaves out.
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

/* One item per box and one per run of text on a line, and one entry per
   link. Set against what a real page costs, measured: an encyclopaedia
   article with sixteen hundred anchors in its markup came out of the
   layout as forty-six thousand items and six and a half thousand link
   entries, which is more than the markup holds and is a separate thing
   to look into -- but until it is looked into, a limit set to the
   markup's own size turns a page that renders into a page that says it
   does not fit.

   Running out is said out loud in the window rather than passed over,
   and a link past the limit is drawn as words: not in the accent, and
   not clickable. */
#define LAY_ITEMS  96000
#define LAY_TEXT   (1024 * 1024)
#define LAY_LINKS  12000
#define LAY_LINE   400
#define LAY_DEPTH  DOM_DEPTH

enum { LK_BOX = 1, LK_TEXT, LK_BULLET, LK_IMAGE, LK_FIELD };

/* --- a page that hides itself until its script has run --------------------
 *
 * A framework-built page arrives as
 * `<div style="visibility:hidden">` wrapped round the whole document, and
 * is made visible from script once the framework has rebuilt the page in
 * the browser. This one runs a page's own script; it does not run a
 * framework that rebuilds the page, so the div stays hidden -- and what is
 * on screen is nothing at all, for a document whose every word arrived in
 * the html and is sitting in the tree.
 *
 * So the page is laid out, and if that produced no words at all, it is laid
 * out again with this set and the window says why. Showing a page a moment
 * before its author meant to is a smaller wrong than showing an empty one.
 *
 * It is not on by default: `visibility:hidden` on a menu that is not open
 * means what it says, and a page that works is not improved by having its
 * closed menus drawn over it. */
static int lay_show_hidden;

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
    int   laid;          /* boxes laid out, trial ones included: the work done */
    int   matched;       /* elements run through selector matching */
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
/* An element's words, leaving out any drawing inside it: an icon's <title>
   ("Chevron Left", "Hamburger") is a name for a screen reader, and written
   as a button's label it read as the button's words. */
static inline void lay_words_of(const ddoc *d, int el, char *out, int cap) {
    int w = 0;
    for (int i = el; i >= 0 && w < cap - 1; ) {
        if (d->nodes[i].kind == DN_ELEMENT && d->nodes[i].tag == T_SVG && i != el) {
            int nx = -1;
            for (int up = i; up >= 0 && up != el; up = d->nodes[up].parent)
                if (d->nodes[up].next >= 0) { nx = d->nodes[up].next; break; }
            i = nx;
            continue;
        }
        if (d->nodes[i].kind == DN_TEXT && d->nodes[i].text >= 0) {
            const char *s = d->arena + d->nodes[i].text;
            int any = 0;
            for (const char *q = s; *q; q++) if (*q > ' ') any = 1;
            if (any) while (*s && w < cap - 1) out[w++] = *s++;
        }
        i = dom_next(d, i, el);
    }
    out[w] = 0;
}

static inline const char *lay_control_label(const ddoc *d, int el, int kind) {
    static char buf[256];
    if (kind == CTL_BUTTON) {
        const char *v = dom_attr(d, el, "value");
        if (v && *v) return v;
        if (d->nodes[el].tag == T_BUTTON) {
            lay_words_of(d, el, buf, (int)sizeof(buf));
            if (buf[0]) return buf;
            /* A button that is only an icon has its name in aria-label or
               title, which is what a screen reader says for it; with neither
               it is an empty button rather than one that says "Button". */
            v = dom_attr(d, el, "aria-label");
            if (!v || !*v) v = dom_attr(d, el, "title");
            return v ? v : "";
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

    /* What an absolutely positioned box is measured from.
     *
       Its nearest ancestor that is itself positioned, or the page when
       there is none -- which is the rule, and is why `position: relative`
       with no offsets is the commonest declaration on the web: it does
       nothing to the element and makes it the thing its children are
       placed against. */
    int pos_x, pos_y, pos_w;

    /* Inside lay_measure, how deep, and the furthest right that a flex row
       being measured rather than laid out would have reached (see lay_flex). */
    int measuring;
    int measure_right;

    /* The flex item being laid out at the width its row settled on, which
       is its width whatever its own width says: that was only where the row
       started from (lay_flex). -1 when there is none. */
    int flex_sized;

    /* Boxes sitting on the line being built as one piece -- an inline-block,
       laid out as a block and placed like a word -- which the end of the line
       moves together rather than dropping each thing inside onto the
       baseline on its own. Items grp_first[i] up to grp_end[i]; a pinned one
       (something absolutely positioned inside a run of text) is not moved at
       all. The line's own are the last ones: those at or past line_at. */
#define LAY_GROUPS 256
    int grp_first[LAY_GROUPS], grp_end[LAY_GROUPS], grp_h[LAY_GROUPS];
    unsigned char grp_pin[LAY_GROUPS];
    int ngroups;

    /* The room a line was offered before any float took some of it, which is
       what the next line starts from (lay_line_start). */
    int cont_left, cont_width;

    /* Floats placed so far, the last LAY_FLOATS of them, in page
       coordinates: a line that starts beside one is shortened by it. */
#define LAY_FLOATS 64
    int fl_x[LAY_FLOATS], fl_y[LAY_FLOATS], fl_w[LAY_FLOATS], fl_h[LAY_FLOATS];
    unsigned char fl_side[LAY_FLOATS];
    int fl_seq;                   /* how many ever placed */
    int floating;                 /* the float being laid out, not floated again */
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
    /* Being measured, a line is as wide as its words wherever they would
       sit: centred, it reached halfway across whatever it was measured in,
       and a table column holding a centred heading asked for the whole page. */
    if (L->measuring) shift = 0;
    else if (L->align == A_CENTER) shift = slack / 2;
    else if (L->align == A_RIGHT) shift = slack;

    int g0 = L->ngroups;
    while (g0 > 0 && L->grp_first[g0 - 1] >= L->line_at) g0--;
    int gi = g0;
    for (int i = L->line_at; i < o->nitems; i++) {
        litem *it = &o->items[i];
        while (gi < L->ngroups && L->grp_end[gi] <= i) gi++;
        if (gi < L->ngroups && i >= L->grp_first[gi]) {
            /* One piece: laid out from the top of the line, and moved down
               together so that its bottom sits on the baseline. */
            if (L->grp_pin[gi]) continue;
            it->x += shift;
            int dy = L->line_base - L->grp_h[gi];
            if (dy > 0) it->y += dy;
            continue;
        }
        it->x += shift;
        /* Sit on the baseline rather than on the top of the line. */
        if (it->kind == LK_TEXT)
            it->y = L->line_top + L->line_base - tface_h(it->face);
        else
            it->y = L->line_top + L->line_base - it->h;
        if (it->y < L->line_top) it->y = L->line_top;
    }
    L->ngroups = g0;

    *y = L->line_top + L->line_h;
    L->line_started = 0;
    L->line_n = 0;
    L->pending_space = 0;
}

/* The room at height y inside left..left+width once the floats beside it
   have taken theirs: moved in from the side each float is on. */
static inline void lay_float_room(const lctx *L, int y, int *l, int *r) {
    int first = L->fl_seq > LAY_FLOATS ? L->fl_seq - LAY_FLOATS : 0;
    for (int s = first; s < L->fl_seq; s++) {
        int k = s % LAY_FLOATS;
        if (y < L->fl_y[k] || y >= L->fl_y[k] + L->fl_h[k]) continue;
        int fx = L->fl_x[k], fr = L->fl_x[k] + L->fl_w[k];
        if (fr <= *l || fx >= *r) continue;
        if (L->fl_side[k] == 1) { if (fr > *l) *l = fr; }
        else { if (fx < *r) *r = fx; }
    }
}

/* The lowest bottom above which a float still stands at height y, or -1. */
static inline int lay_float_next(const lctx *L, int y, int l, int r) {
    int best = -1;
    int first = L->fl_seq > LAY_FLOATS ? L->fl_seq - LAY_FLOATS : 0;
    for (int s = first; s < L->fl_seq; s++) {
        int k = s % LAY_FLOATS;
        int b = L->fl_y[k] + L->fl_h[k];
        if (y < L->fl_y[k] || y >= b) continue;
        if (L->fl_x[k] + L->fl_w[k] <= l || L->fl_x[k] >= r) continue;
        if (best < 0 || b < best) best = b;
    }
    return best;
}

static inline void lay_line_start(lctx *L, int y, int left, int width,
                                  int align) {
    L->cont_left = left;
    L->cont_width = width;
    /* Beside a float the line is shorter; and where what is left is too
       narrow for a word, the line goes below the float instead. */
    if (L->fl_seq) {
        for (int tries = 0; tries < 8; tries++) {
            int l = left, r = left + width;
            lay_float_room(L, y, &l, &r);
            if (r - l >= 48 || (l == left && r == left + width)) {
                left = l;
                width = r - l;
                break;
            }
            int nb = lay_float_next(L, y, left, left + width);
            if (nb < 0) break;
            y = nb;
        }
    }
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
        int left = L->cont_left, width = L->cont_width, al = L->align;
        lay_line_end(L, y);
        lay_line_start(L, *y, left, width, al);
        sp = 0;
    }
    L->pen += sp;

    litem *it = lay_item(L);
    if (!it) return;
    it->kind = LK_TEXT;
    it->at = st->ink_none ? -1 : lay_put(L, s, n);
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
    if (st->font_px <= 0) return;                 /* font-size: 0 */
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
            int left = L->cont_left, width = L->cont_width, al = L->align;
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

/* Which rules match each element, worked out once a layout.
 *
 * An element's style is asked for two or three times in one layout -- to ask
 * whether it is a block, to lay it out, once more by its box -- and a flex
 * row asks again while it measures. Each time went through the whole of
 * selector matching, and matching was nine tenths of the time a layout took:
 * every rule whose last part named one of the element's classes walked up its
 * ancestors looking for the rest. Which rules match cannot change inside one
 * layout (the document, the sheets and the hover are all fixed for it), so
 * the answer is kept, by rule number in the order they apply, stamped with
 * the layout it belongs to. When the pool is full the rest are worked out
 * every time, as they always were. */
#define LAY_HIT_POOL 131072
static int            lay_hit_pool[LAY_HIT_POOL];
static int            lay_hit_used;
static int            lay_hit_at[DOM_NODES];
static unsigned short lay_hit_n[DOM_NODES];
static unsigned       lay_hit_gen[DOM_NODES];
static unsigned       lay_gen;             /* moved on by every lay_run */

/* Each element's ancestor bits (css.h, cbloom), worked out once a layout from
   its parent's: the parent's own bits and the parent's keys. */
static cbloom   lay_anc[DOM_NODES];
static unsigned lay_anc_gen[DOM_NODES];

/* The keys an element puts into its children's bits: its tag, its id and
   each of its classes, exactly as css_part_matches compares them. */
static inline void lay_keys_of(const ddoc *d, int el, cbloom *b) {
    if (d->nodes[el].kind != DN_ELEMENT) return;
    css_bloom_add(b, css_key_tag(d->nodes[el].tag));
    const char *id = dom_attr(d, el, "id");
    if (id && *id) css_bloom_add(b, css_key_id(id, w_len(id)));
    const char *cl = dom_attr(d, el, "class");
    if (!cl) return;
    for (int i = 0; cl[i]; ) {
        while (cl[i] == ' ' || cl[i] == '\t' || cl[i] == '\n' || cl[i] == '\r') i++;
        if (!cl[i]) break;
        int start = i;
        while (cl[i] && cl[i] != ' ' && cl[i] != '\t' && cl[i] != '\n' && cl[i] != '\r') i++;
        css_bloom_add(b, css_key_cls(cl + start, i - start));
    }
}

/* Up to the nearest ancestor already worked out, then back down filling in.
   Null past a depth this will not follow, which only means no filtering. */
static inline const cbloom *lay_ancestors(const ddoc *d, int el) {
    if (el < 0 || el >= DOM_NODES) return 0;
    if (lay_anc_gen[el] == lay_gen) return &lay_anc[el];
    int chain[96], n = 0;
    for (int at = el; at >= 0 && lay_anc_gen[at] != lay_gen; at = d->nodes[at].parent) {
        if (n == 96) return 0;
        chain[n++] = at;
    }
    while (n--) {
        int node = chain[n], p = d->nodes[node].parent;
        cbloom *b = &lay_anc[node];
        b->w[0] = b->w[1] = 0;
        if (p >= 0) {
            b->w[0] = lay_anc[p].w[0];
            b->w[1] = lay_anc[p].w[1];
            lay_keys_of(d, p, b);
        }
        lay_anc_gen[node] = lay_gen;
    }
    return &lay_anc[el];
}


/* --- custom properties ------------------------------------------------------
 *
 * `--brand: #0969da` on :root and `color: var(--brand)` everywhere below is
 * how most sites written this decade say every colour, space and size they
 * use. None of it was read: a var() was a value nothing understood, so a
 * page lost its colours to the browser's defaults and its spacing to
 * nothing.
 *
 * Custom properties are inherited, so each element's are a chain: its own,
 * then its parent's, down to :root's (cstyle.vars is the chain's head). They
 * are gathered before anything else about the element is applied, because
 * var() refers to the element's final custom properties whatever order they
 * were written in; then each declaration that uses var() has the values put
 * in (lay_var_subst) and is applied as though it had been written that way.
 * Resolved text lives for the whole layout (lay_arena), since a grid's
 * template is read long after it was applied. A cycle, or nesting past
 * eight deep, resolves to nothing. */
#define LAY_VARS 65536
static const char *lay_var_text[LAY_VARS];   /* "--name:value" */
static int lay_var_nlen[LAY_VARS], lay_var_next[LAY_VARS];
static int lay_var_used;
static int lay_var_in[DOM_NODES], lay_var_out[DOM_NODES];
static unsigned lay_var_gen[DOM_NODES];
#define LAY_ARENA (1024 * 1024)
static char *lay_arena;               /* mapped on first use (map, sdk/zelr.h) */
static int lay_arena_used;

static inline int lay_var_push(int head, const char *txt) {
    if (lay_var_used >= LAY_VARS) return head;
    int n = 0;
    while (txt[n] && txt[n] != ':') n++;
    int k = lay_var_used++;
    lay_var_text[k] = txt;
    lay_var_nlen[k] = n;
    lay_var_next[k] = head;
    return k;
}

static inline const char *lay_var_find(int head, const char *name, int n) {
    for (int k = head; k >= 0; k = lay_var_next[k]) {
        if (lay_var_nlen[k] != n) continue;
        const char *t = lay_var_text[k];
        int i = 0;
        while (i < n && t[i] == name[i]) i++;
        if (i == n) return t + n + 1;
    }
    return 0;
}

/* v with every var() replaced, into out; how long it came to. */
static int lay_var_subst(int head, const char *v, int vlen, char *out, int cap, int depth) {
    int o = 0;
    for (int i = 0; i < vlen && o < cap - 1; ) {
        if (!(v[i] == 'v' && i + 3 < vlen && v[i + 1] == 'a' && v[i + 2] == 'r' && v[i + 3] == '(')) {
            out[o++] = v[i++];
            continue;
        }
        int j = i + 4;
        while (j < vlen && v[j] == ' ') j++;
        int ns = j;
        while (j < vlen && v[j] != ',' && v[j] != ')' && v[j] != ' ') j++;
        int nlen = j - ns;
        while (j < vlen && v[j] == ' ') j++;
        int fb = -1, fe = -1;
        if (j < vlen && v[j] == ',') {
            fb = ++j;
            int d = 0;
            while (j < vlen && (d || v[j] != ')')) {
                if (v[j] == '(') d++;
                else if (v[j] == ')') d--;
                j++;
            }
            fe = j;
        } else {
            while (j < vlen && v[j] != ')') j++;
        }
        if (j < vlen) j++;                            /* past the ')' */
        const char *val = depth < 8 ? lay_var_find(head, v + ns, nlen) : 0;
        if (val) o += lay_var_subst(head, val, w_len(val), out + o, cap - o, depth + 1);
        else if (fb >= 0 && depth < 8) o += lay_var_subst(head, v + fb, fe - fb, out + o, cap - o, depth + 1);
        i = j;
    }
    out[o] = 0;
    return o;
}

/* Resolved text that lasts the whole layout. */
static inline const char *lay_var_keep(int head, const char *v) {
    static char spill[4096];
    if (!lay_arena) lay_arena = (char *)map(LAY_ARENA, PROT_READ | PROT_WRITE);
    char *dst = lay_arena && LAY_ARENA - lay_arena_used > 4096 ? lay_arena + lay_arena_used : spill;
    int n = lay_var_subst(head, v, w_len(v), dst, 4096, 0);
    if (dst != spill) lay_arena_used += n + 1;
    return dst;
}

/* A shorthand whose value had var() in it, split now that it is known. */
static void lay_apply_short(lctx *L, cstyle *st, const char *nv, int pct_of) {
    char name[48];
    int k = 0;
    while (nv[k] && nv[k] != ':' && k < 47) { name[k] = nv[k]; k++; }
    name[k] = 0;
    const char *v = nv[k] == ':' ? nv + k + 1 : "";
    int ps[4], pl[4];
    int n = css_parts(v, w_len(v), ps, pl, 4);
    char part[4][96];
    for (int i = 0; i < n; i++) {
        int m = pl[i] < 95 ? pl[i] : 95;
        for (int c = 0; c < m; c++) part[i][c] = v[ps[i] + c];
        part[i][m] = 0;
    }
    int four = -1;
    if (lay_same_fold(name, "margin")) four = P_MARGIN_T;
    else if (lay_same_fold(name, "padding")) four = P_PADDING_T;
    else if (lay_same_fold(name, "border-width")) four = P_BORDER_T;
    else if (lay_same_fold(name, "inset")) four = P_TOP;
    if (four >= 0 && n > 0) {
        /* top, right, bottom, left, from one to four values */
        const char *t = part[0], *r = n > 1 ? part[1] : part[0];
        const char *b = n > 2 ? part[2] : part[0], *l = n > 3 ? part[3] : r;
        const char *side[4] = { t, r, b, l };
        for (int i = 0; i < 4; i++) css_apply_v(four + i, side[i], st, L->root_px, pct_of);
        return;
    }
    if (lay_same_fold(name, "background")) {
        u32 c;
        for (int i = 0; i < n; i++)
            if (css_color(part[i], &c)) { css_apply_v(P_BACKGROUND, part[i], st, L->root_px, pct_of); break; }
        return;
    }
    if (w_starts_fold(name, "border")) {
        int sides[4] = { P_BORDER_T, P_BORDER_R, P_BORDER_B, P_BORDER_L }, first = 0, count = 4;
        if (lay_same_fold(name, "border-top")) count = 1;
        else if (lay_same_fold(name, "border-right")) { first = 1; count = 1; }
        else if (lay_same_fold(name, "border-bottom")) { first = 2; count = 1; }
        else if (lay_same_fold(name, "border-left")) { first = 3; count = 1; }
        else if (lay_same_fold(name, "border-radius")) { if (n) css_apply_v(P_RADIUS, part[0], st, L->root_px, pct_of); return; }
        for (int s2 = first; s2 < first + count; s2++) css_apply_v(sides[s2], v, st, L->root_px, pct_of);
        return;
    }
    if (lay_same_fold(name, "font")) {
        for (int i = 0; i < n; i++) {
            clen F = css_len(part[i]);
            if (F.unit != U_AUTO && F.v > 0 && F.unit != U_PX) { css_apply_v(P_FONT_SIZE, part[i], st, L->root_px, pct_of); break; }
            if (F.unit == U_PX && F.v > 100 * 4) { css_apply_v(P_FONT_SIZE, part[i], st, L->root_px, pct_of); break; }
            if (w_starts_fold(part[i], "bold")) css_apply_v(P_FONT_WEIGHT, "bold", st, L->root_px, pct_of);
            if (w_starts_fold(part[i], "italic")) css_apply_v(P_FONT_STYLE, "italic", st, L->root_px, pct_of);
        }
        return;
    }
    if (lay_same_fold(name, "flex")) { css_apply_v(P_FLEX_GROW, v, st, L->root_px, pct_of); return; }
    if (lay_same_fold(name, "list-style")) { css_apply_v(P_LIST_STYLE, v, st, L->root_px, pct_of); return; }
}

/* One declaration, with its variables put in first when it has any. */
static inline void lay_apply_decl(lctx *L, const cdecl *dc, cstyle *out, int pct_of) {
    int prop = dc->prop & ~CSS_HAS_VAR;
    if (prop == P_CUSTOM) return;                 /* gathered already */
    const char *v = L->s->text + dc->value;
    if (dc->prop & CSS_HAS_VAR) v = lay_var_keep(out->vars, v);
    if (prop == P_DEFER) { lay_apply_short(L, out, v, pct_of); return; }
    css_apply_v(prop, v, out, L->root_px, pct_of);
}

static inline void lay_apply_rule(lctx *L, int rule, cstyle *out, int pct_of) {
    const crule *r = &L->s->rules[rule];
    for (int k = 0; k < r->decl_n; k++)
        lay_apply_decl(L, &L->s->decls[r->decl_at + k], out, pct_of);
}

/* --- what the markup says about its own look -------------------------------
 *
 * Before style sheets there were attributes: bgcolor, width, align, valign,
 * cellpadding, border, and <font>. Half the old web and some of the biggest
 * sites' plainest pages still say everything that way -- a news aggregator's
 * orange bar is a bgcolor on a table cell, and its centred column is a table
 * with width="85%" inside a <center>. None of it was read.
 *
 * They count for less than any style sheet a page has and for more than the
 * browser's own, which is where lay_style puts them. */

/* An element this has no T_ for, by name: tbody and font are among them. */
static inline int lay_named(const ddoc *d, int el, const char *name) {
    const dnode *n = &d->nodes[el];
    if (n->kind != DN_ELEMENT || n->tag != T_OTHER || n->text < 0) return 0;
    return lay_same_fold(d->arena + n->text, name);
}

/* A colour as old markup writes one, which is with or without its #. */
static inline int lay_attr_color(const char *v, u32 *out) {
    if (!v) return 0;
    while (*v == ' ') v++;
    if (css_color(v, out)) return 1;
    char with[16];
    int k = 0;
    with[k++] = '#';
    for (; v[k - 1] && k < 15; k++) with[k] = v[k - 1];
    with[k] = 0;
    return css_color(with, out);
}

/* A width or height attribute: pixels, or a percentage of `pct_of`. */
static inline int lay_attr_len(const char *v, int pct_of) {
    if (!v) return -1;
    int n = lay_number(v);
    const char *q = v;
    while (*q == ' ') q++;
    while (*q >= '0' && *q <= '9') q++;
    if (*q == '.') { q++; while (*q >= '0' && *q <= '9') q++; }
    if (*q == '%') return pct_of > 0 ? n * pct_of / 100 : -1;
    if (q == v) return -1;
    return n;
}

static inline int lay_attr_align(const char *v) {
    if (!v) return -1;
    if (lay_same_fold(v, "center") || lay_same_fold(v, "middle")) return A_CENTER;
    if (lay_same_fold(v, "right")) return A_RIGHT;
    if (lay_same_fold(v, "left")) return A_LEFT;
    if (lay_same_fold(v, "justify")) return A_JUSTIFY;
    return -1;
}

/* The table a cell, row or group belongs to, within a few levels. */
static inline int lay_table_of(const ddoc *d, int el) {
    int at = d->nodes[el].parent;
    for (int k = 0; k < 4 && at >= 0; k++, at = d->nodes[at].parent)
        if (d->nodes[at].kind == DN_ELEMENT && d->nodes[at].tag == T_TABLE) return at;
    return -1;
}

static inline void lay_hints(lctx *L, int el, cstyle *st, int pct_of) {
    const ddoc *d = L->d;
    if (el < 0 || d->nodes[el].kind != DN_ELEMENT) return;
    if (d->nodes[el].attr_n == 0) {
        if (d->nodes[el].tag == T_TABLE && !d->standards) st->align = A_LEFT;
        if (lay_named(d, el, "tbody") || lay_named(d, el, "thead") || lay_named(d, el, "tfoot"))
            st->display = D_TABLE_GROUP;
        return;
    }
    int tag = d->nodes[el].tag;
    u32 c;
    const char *v;

    if (tag == T_BODY || tag == T_TABLE || tag == T_TR || tag == T_TD || tag == T_TH) {
        if (lay_attr_color(dom_attr(d, el, "bgcolor"), &c)) { st->background = c; st->has_bg = 1; }
    }
    if (tag == T_BODY && lay_attr_color(dom_attr(d, el, "text"), &c)) st->color = c;

    if (tag == T_TABLE || tag == T_TD || tag == T_TH || tag == T_HR) {
        int w = lay_attr_len(dom_attr(d, el, "width"), pct_of);
        if (w > 0) st->width = (short)(w > 4000 ? 4000 : w);
    }
    if (tag == T_TABLE || tag == T_TD || tag == T_TH || tag == T_TR) {
        int h = lay_attr_len(dom_attr(d, el, "height"), -1);
        if (h > 0) st->height = (short)(h > 4000 ? 4000 : h);
    }

    if (tag == T_IMG || tag == T_TABLE) {
        /* align="left" on a picture or a table is a float. */
        v = dom_attr(d, el, "align");
        if (v && lay_same_fold(v, "left")) st->floated = 1;
        else if (v && lay_same_fold(v, "right")) st->floated = 2;
    }

    if (tag == T_TABLE) {
        /* Without a doctype a table starts its text from the left whatever
           it sits in -- the quirk a centred page of tables relies on to have
           its rows read from the left. */
        if (!d->standards) st->align = A_LEFT;
        v = dom_attr(d, el, "align");
        if (v && lay_same_fold(v, "center")) st->ml = st->mr = CSS_AUTO_OFF;
        v = dom_attr(d, el, "cellspacing");
        if (v) { int n = lay_number(v); st->spacing = (short)(n > 64 ? 64 : n); }
        v = dom_attr(d, el, "border");
        if (v) {
            int n = *v ? lay_number(v) : 1;
            if (n > 8) n = 8;
            if (n > 0) {
                st->bt = st->br = st->bb = st->bl = (short)n;
                st->border_color = 0x808080;
            }
        }
    }

    if (tag == T_TD || tag == T_TH) {
        int t = lay_table_of(d, el);
        if (t >= 0) {
            v = dom_attr(d, t, "cellpadding");
            if (v) {
                int n = lay_number(v);
                if (n > 64) n = 64;
                st->pt = st->pr = st->pb = st->pl = (short)n;
            }
            v = dom_attr(d, t, "border");
            if (v && (!*v || lay_number(v) > 0)) {
                st->bt = st->br = st->bb = st->bl = 1;
                st->border_color = 0x808080;
            }
        }
        if (dom_attr(d, el, "nowrap")) st->white = WS_NOWRAP;
    }

    if (tag == T_TD || tag == T_TH || tag == T_TR) {
        v = dom_attr(d, el, "valign");
        if (v) {
            if (lay_same_fold(v, "top")) st->valign = VA_TOP;
            else if (lay_same_fold(v, "bottom")) st->valign = VA_BOTTOM;
            else if (lay_same_fold(v, "middle") || lay_same_fold(v, "center")) st->valign = VA_MIDDLE;
        }
    }

    if (tag == T_TD || tag == T_TH || tag == T_TR || tag == T_DIV || tag == T_P
        || (tag >= T_H1 && tag <= T_H6) || tag == T_CAPTION) {
        int a = lay_attr_align(dom_attr(d, el, "align"));
        if (a >= 0) st->align = (unsigned char)a;
    }

    if (lay_named(d, el, "tbody") || lay_named(d, el, "thead") || lay_named(d, el, "tfoot"))
        st->display = D_TABLE_GROUP;

    if (lay_named(d, el, "font")) {
        if (lay_attr_color(dom_attr(d, el, "color"), &c)) st->color = c;
        v = dom_attr(d, el, "size");
        if (v) {
            /* One to seven, or a step from three written with a sign. */
            static const short PX[8] = { 0, 10, 13, 16, 18, 24, 32, 48 };
            const char *q = v;
            while (*q == ' ') q++;
            int step = *q == '+' || *q == '-';
            int n = lay_number(step ? q + 1 : q);
            if (step) n = *q == '+' ? 3 + n : 3 - n;
            if (n < 1) n = 1;
            if (n > 7) n = 7;
            st->font_px = PX[n];
        }
        v = dom_attr(d, el, "face");
        if (v) {
            for (const char *q = v; q[0] && q[1] && q[2] && q[3]; q++) {
                char a = w_lower(q[0]), b = w_lower(q[1]), e = w_lower(q[2]), f = w_lower(q[3]);
                if ((a == 'm' && b == 'o' && e == 'n' && f == 'o')
                    || (a == 'c' && b == 'o' && e == 'u' && f == 'r')) { st->mono = 1; break; }
            }
        }
    }
}

/* A whole style copied. Written out, because a struct assignment of this
   size is a call to memcpy and a ring 3 program has no memcpy to call: the
   compiler is within its rights and the linker says so. */
static inline void lay_cs(cstyle *d, const cstyle *s) {
    volatile unsigned long long *a = (volatile unsigned long long *)(void *)d;
    const unsigned long long *b = (const unsigned long long *)(const void *)s;
    for (unsigned k = 0; k < sizeof(cstyle) / 8; k++) a[k] = b[k];
    volatile unsigned char *ac = (volatile unsigned char *)(void *)d;
    const unsigned char *bc = (const unsigned char *)(const void *)s;
    for (unsigned k = sizeof(cstyle) / 8 * 8; k < sizeof(cstyle); k++) ac[k] = bc[k];
}

static inline void lay_style(lctx *L, int el, const cstyle *parent,
                             cstyle *out, int pct_of) {
    css_inherit(out, parent);
    /* The browser's own rules, then what the markup says about itself, then
       the page's sheets: an author's rule beats the browser's whatever
       either's specificity, which is the order the cascade has always had
       and which a sorted list of both had lost. */
    int ua = L->s->ua_rules;
    const int *rules;
    int n;
    chit hits[CSS_HITS];
    if (el >= 0 && el < DOM_NODES && lay_hit_gen[el] == lay_gen) {
        rules = lay_hit_pool + lay_hit_at[el];
        n = lay_hit_n[el];
    } else {
        n = css_collect(L->s, L->x, L->d, el, L->m, lay_ancestors(L->d, el), hits);
        L->out->matched++;
        if (el >= 0 && el < DOM_NODES && lay_hit_used + n <= LAY_HIT_POOL) {
            lay_hit_at[el] = lay_hit_used;
            lay_hit_n[el] = (unsigned short)n;
            lay_hit_gen[el] = lay_gen;
            for (int i = 0; i < n; i++) lay_hit_pool[lay_hit_used++] = hits[i].rule;
            rules = lay_hit_pool + lay_hit_at[el];
        } else {
            static int loose[CSS_HITS];
            for (int i = 0; i < n; i++) loose[i] = hits[i].rule;
            rules = loose;
        }
    }
    /* The custom properties first, in the order the cascade applies them,
       worked out once per element for the chain it inherits. */
    int head = out->vars;
    if (el >= 0 && el < DOM_NODES && lay_var_gen[el] == lay_gen && lay_var_in[el] == head) {
        head = lay_var_out[el];
    } else {
        int h = head;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < n; i++) {
                if ((rules[i] < ua) != (pass == 0)) continue;
                const crule *r = &L->s->rules[rules[i]];
                for (int k = 0; k < r->decl_n; k++) {
                    const cdecl *dc = &L->s->decls[r->decl_at + k];
                    if ((dc->prop & ~CSS_HAS_VAR) == P_CUSTOM) h = lay_var_push(h, L->s->text + dc->value);
                }
            }
        if (L->inl && el >= 0 && L->inl[el].n > 0)
            for (int k = 0; k < L->inl[el].n; k++) {
                const cdecl *dc = &L->s->decls[L->inl[el].at + k];
                if ((dc->prop & ~CSS_HAS_VAR) == P_CUSTOM) h = lay_var_push(h, L->s->text + dc->value);
            }
        if (el >= 0 && el < DOM_NODES) {
            lay_var_gen[el] = lay_gen;
            lay_var_in[el] = head;
            lay_var_out[el] = h;
        }
        head = h;
    }
    out->vars = head;

    for (int i = 0; i < n; i++) if (rules[i] < ua) lay_apply_rule(L, rules[i], out, pct_of);
    lay_hints(L, el, out, pct_of);
    for (int i = 0; i < n; i++) if (rules[i] >= ua) lay_apply_rule(L, rules[i], out, pct_of);

    /* And last, what the element says about itself. A style attribute beats
       every rule in every sheet no matter how specific, which is the one
       part of the cascade that needs no comparison to decide. */
    if (L->inl && L->inl[el].n > 0)
        for (int k = 0; k < L->inl[el].n; k++)
            lay_apply_decl(L, &L->s->decls[L->inl[el].at + k], out, pct_of);
}

static void lay_block(lctx *L, int node, const cstyle *parent, int x,
                      int avail, int *y);
static int lay_measure(lctx *L, int node, const cstyle *parent, int avail,
                       int *height);

/* Something written for a screen reader and not for the eye: clipped to
   nothing, or a box a pixel or two across whose overflow is hidden. */
static inline int lay_unseen(const cstyle *st) {
    if (st->gone) return 1;
    return st->clip && ((st->width >= 0 && st->width <= 2)
                        || (st->height >= 0 && st->height <= 2));
}

/* An inline-block, or something absolutely positioned inside a run of
 * text, laid out as a block of its own from the top of the line it lands
 * on and placed on the line as one piece (lctx grp_*).
 *
 * Both used to be walked as though they were inline: an inline-block's
 * width, padding and background were ignored and whatever blocks were in it
 * were run into the words around it, which is how a row of buttons or tabs
 * became one long sentence; and an absolutely positioned label was written
 * into the text it was meant to float over. */
static void lay_inline_piece(lctx *L, int at, const cstyle *parent, int pin, int *y) {
    cstyle st;
    lay_style(L, at, parent, &st, L->line_width);
    int w = L->line_width;
    if (!pin) {
        int frame = st.pl + st.pr + st.bl + st.br;
        int mw = (st.ml > 0 ? st.ml : 0) + (st.mr > 0 ? st.mr : 0);
        if (st.width >= 0) {
            w = st.width + (st.border_box ? 0 : frame) + mw;
        } else if (L->measuring) {
            /* Inside a measurement already, and a box being measured comes
               out as wide as what is in it (lay_block_placed): laying it out
               once in all the room left says how wide it is. Measuring it
               first and then laying it out, at every level of nesting, was
               twice the work per level -- a page header of inline-blocks
               fifteen deep took minutes. */
            w = L->line_left + L->line_width - L->pen;
            if (w < 1) w = 1;
            int first = L->out->nitems, s_mr = L->measure_right, yy = L->line_top;
            int s_line_at = L->line_at, s_line_n = L->line_n, s_pen = L->pen;
            int s_top = L->line_top, s_h = L->line_h, s_base = L->line_base;
            int s_cl = L->cont_left, s_cw = L->cont_width;
            int s_left = L->line_left, s_width = L->line_width, s_align = L->align;
            int s_link = L->cur_link, s_space = L->pending_space;
            L->measure_right = 0;
            L->line_started = 0;
            L->flex_sized = at;
            lay_block(L, at, parent, L->pen, w, &yy);
            L->flex_sized = -1;
            int right = L->measure_right;
            for (int i = first; i < L->out->nitems; i++) {
                int r = L->out->items[i].x + L->out->items[i].w;
                if (r > right) right = r;
            }
            L->measure_right = s_mr > right ? s_mr : right;
            L->line_at = s_line_at; L->line_n = s_line_n; L->pen = s_pen;
            L->line_top = s_top; L->line_h = s_h; L->line_base = s_base;
            L->line_left = s_left; L->line_width = s_width;
    L->cont_left = s_cl; L->cont_width = s_cw; L->align = s_align;
            L->cur_link = s_link; L->pending_space = s_space;
            L->line_started = 1;
            int pw = right - s_pen;
            if (pw < 1) pw = 1;
            int h = yy - s_top;
            if (L->ngroups < LAY_GROUPS && L->out->nitems > first) {
                L->grp_first[L->ngroups] = first;
                L->grp_end[L->ngroups] = L->out->nitems;
                L->grp_h[L->ngroups] = h;
                L->grp_pin[L->ngroups] = 0;
                L->ngroups++;
            }
            L->pen += pw;
            L->line_n++;
            lay_line_fit(L, h > 0 ? h : 1, 100);
            L->pending_space = 0;
            return;
        } else {
            int h0;
            w = lay_measure(L, at, parent, L->line_width, &h0);
        }
        if (w > L->line_width) w = L->line_width;
        if (w < 1) w = 1;

        int sp = 0;
        if (L->pending_space && L->pen > L->line_left)
            sp = tface_wn(" ", 1, face_pick(parent->font_px, parent->bold, parent->mono));
        if (L->pen + sp + w > L->line_left + L->line_width && L->pen > L->line_left) {
            int left = L->cont_left, width = L->cont_width, al = L->align;
            lay_line_end(L, y);
            lay_line_start(L, *y, left, width, al);
            sp = 0;
        }
        L->pen += sp;
    }

    int s_line_at = L->line_at, s_line_n = L->line_n;
    int s_pen = L->pen, s_top = L->line_top;
    int s_h = L->line_h, s_base = L->line_base;
    int s_cl = L->cont_left, s_cw = L->cont_width;
            int s_left = L->line_left, s_width = L->line_width;
    int s_align = L->align, s_link = L->cur_link, s_space = L->pending_space;

    int first = L->out->nitems;
    int yy = L->line_top;
    L->line_started = 0;
    if (!pin) L->flex_sized = at;          /* its width was settled here */
    lay_block(L, at, parent, pin ? L->line_left : L->pen, w, &yy);
    L->flex_sized = -1;

    L->line_at = s_line_at; L->line_n = s_line_n;
    L->pen = s_pen; L->line_top = s_top;
    L->line_h = s_h; L->line_base = s_base;
    L->line_left = s_left; L->line_width = s_width;
    L->cont_left = s_cl; L->cont_width = s_cw;
    L->align = s_align; L->cur_link = s_link;
    L->pending_space = s_space;
    L->line_started = 1;

    int h = yy - s_top;
    if (L->out->nitems > first && L->ngroups < LAY_GROUPS) {
        L->grp_first[L->ngroups] = first;
        L->grp_end[L->ngroups] = L->out->nitems;
        L->grp_h[L->ngroups] = h;
        L->grp_pin[L->ngroups] = (unsigned char)pin;
        L->ngroups++;
    }
    if (pin) return;
    L->pen += w;
    L->line_n++;
    lay_line_fit(L, h > 0 ? h : 1, 100);
    L->pending_space = 0;
}

/* --- floats ---------------------------------------------------------------
 *
 * A float goes to one side of what it is in and the lines beside it are
 * shortened to leave it room (lay_line_start): a sidebar, a picture with the
 * text round it, the box of facts at the top of an encyclopaedia article.
 * They were laid out as blocks in the flow, so every sidebar came above the
 * page it was beside and every picture sat alone on a line.
 *
 * What is here: a float is as wide as it was told or as its contents want,
 * goes as far to its side as the floats already there allow, and down past
 * them when there is no room; lines beside it are shorter, and a block that
 * clears goes below it. What is not: blocks do not move aside for floats
 * (only their lines do, which is what a block that is not its own
 * formatting context does anyway), and a block always grows to hold the
 * floats inside it, as though every one were cleared at its end -- the
 * clearfix nearly every page applies, which this cannot see because it is
 * written as an ::after box. */
static void lay_inline(lctx *L, int node, const cstyle *parent, int *y);

static void lay_float(lctx *L, int node, const cstyle *parent, int cleft, int cwidth, int y0) {
    const ddoc *d = L->d;
    cstyle st;
    lay_style(L, node, parent, &st, cwidth);
    int tag = d->nodes[node].tag;
    int replaced = tag == T_IMG || lay_control_kind(d, node) != CTL_NONE;
    int ml = st.ml == CSS_AUTO_OFF ? 0 : st.ml, mr = st.mr == CSS_AUTO_OFF ? 0 : st.mr;

    /* The box's own width, margins apart. */
    int w;
    if (st.width >= 0) {
        w = st.width + (st.border_box ? 0 : st.pl + st.pr + st.bl + st.br);
    } else if (tag == T_IMG) {
        const limage *pic = lay_image_of(L, node);
        const char *aw = dom_attr(d, node, "width");
        w = aw ? lay_number(aw) : pic ? pic->w : 0;
        if (w <= 0) return;                  /* nothing arrived and no size */
    } else {
        /* Measured as itself, not as a float: measuring lays it out, and
           laid out it would be floated again, and measured again. */
        int h, was = L->floating;
        L->floating = node;
        w = lay_measure(L, node, parent, cwidth, &h) - (ml > 0 ? ml : 0) - (mr > 0 ? mr : 0);
        L->floating = was;
    }
    if (w > cwidth) w = cwidth;
    if (w < 1) w = 1;

    /* The room it needs is its margin box, which a negative margin makes
       smaller than the box -- to nothing, for a sidebar pulled back across
       the column floated before it with margin-left: -100%. */
    int need = w + ml + mr;
    if (need < 0) need = 0;

    /* As far to its side as it goes, and down past floats when there is not
       the room beside them. */
    int y = y0, l = cleft, r = cleft + cwidth;
    for (int tries = 0; tries < 16; tries++) {
        l = cleft; r = cleft + cwidth;
        lay_float_room(L, y, &l, &r);
        if (r - l >= need) break;
        int nb = lay_float_next(L, y, cleft, cleft + cwidth);
        if (nb < 0) { l = cleft; r = cleft + cwidth; break; }
        y = nb;
    }
    int mx = st.floated == 2 ? r - need : l;       /* the margin box */
    int x = mx + ml;                              /* the box */

    int s_line_at = L->line_at, s_line_n = L->line_n, s_pen = L->pen;
    int s_top = L->line_top, s_h = L->line_h, s_base = L->line_base;
    int s_left = L->line_left, s_width = L->line_width, s_align = L->align;
    int s_cl = L->cont_left, s_cw = L->cont_width, s_started = L->line_started;
    int s_link = L->cur_link, s_space = L->pending_space, s_groups = L->ngroups;

    int yy = y, was = L->floating;
    L->line_started = 0;
    L->floating = node;
    if (replaced) {
        lay_line_start(L, yy, x, w, A_LEFT);
        lay_inline(L, node, parent, &yy);
        lay_line_end(L, &yy);
    } else {
        /* Given room that its own margins take back off to leave exactly
           the box decided here. */
        L->flex_sized = node;
        lay_block(L, node, parent, x - ml, w + ml + mr, &yy);
        L->flex_sized = -1;
    }
    L->floating = was;

    L->line_at = s_line_at; L->line_n = s_line_n; L->pen = s_pen;
    L->line_top = s_top; L->line_h = s_h; L->line_base = s_base;
    L->line_left = s_left; L->line_width = s_width; L->align = s_align;
    L->cont_left = s_cl; L->cont_width = s_cw; L->line_started = s_started;
    L->cur_link = s_link; L->pending_space = s_space; L->ngroups = s_groups;

    int k = L->fl_seq % LAY_FLOATS;
    L->fl_x[k] = mx;
    L->fl_y[k] = y;
    L->fl_w[k] = need;
    L->fl_h[k] = yy - y + (st.mb > 0 && st.mb != CSS_AUTO_OFF ? st.mb : 0);
    L->fl_side[k] = st.floated;
    L->fl_seq++;
}

/* Below every float on the side a box clears, within its width. */
static int lay_cleared(const lctx *L, int y, int clear, int x, int w) {
    int first = L->fl_seq > LAY_FLOATS ? L->fl_seq - LAY_FLOATS : 0;
    for (int s = first; s < L->fl_seq; s++) {
        int k = s % LAY_FLOATS;
        if (!(clear & L->fl_side[k])) continue;
        if (L->fl_x[k] + L->fl_w[k] <= x || L->fl_x[k] >= x + w) continue;
        int b = L->fl_y[k] + L->fl_h[k];
        if (b > y) y = b;
    }
    return y;
}

/* Inline content, which is everything between two blocks. Walked with an
   explicit style stack rather than by recursion, because an inline run can
   be nested as deep as the page is and this is the hot path. */
/* The left of an inline box: its margin, border and padding, walked past
   before any of its words are laid down. The background's slot is taken
   here and filled in when the box closes, so that it lands in the display
   list behind what is written on top of it. */
static inline void lay_inline_open(lctx *L, int node, const cstyle *st,
                                   int *x0, int *top, int *slot) {
    *x0 = L->pen;
    *top = L->line_top;
    *slot = -1;

    if (st->has_bg || st->bt || st->br || st->bb || st->bl) {
        litem *bg = lay_item(L);
        if (bg) {
            *slot = L->out->nitems - 1;
            bg->kind = LK_BOX;
            bg->node = node;
            bg->w = 0;
            bg->h = 0;
        }
    }
    L->pen += (st->ml > 0 ? st->ml : 0) + st->bl + st->pl;
}

/* And the right of it. */
static inline void lay_inline_close(lctx *L, const cstyle *st, int x0,
                                    int top, int slot) {
    L->pen += st->pr + st->br + (st->mr > 0 ? st->mr : 0);

    if (slot < 0) return;
    litem *bg = &L->out->items[slot];
    if (top != L->line_top) return;          /* it wrapped: draw nothing */

    bg->x = x0 + (st->ml > 0 ? st->ml : 0);
    bg->y = top;
    bg->w = L->pen - bg->x - (st->mr > 0 ? st->mr : 0);
    bg->h = L->line_h > 0 ? L->line_h : st->font_px;
    if (bg->w < 0) bg->w = 0;
    bg->bg = st->background;
    bg->has_bg = st->has_bg;
    bg->border = st->border_color;
    bg->bt = (unsigned char)(st->bt > 255 ? 255 : st->bt);
    bg->br = (unsigned char)(st->br > 255 ? 255 : st->br);
    bg->bb = (unsigned char)(st->bb > 255 ? 255 : st->bb);
    bg->bl = (unsigned char)(st->bl > 255 ? 255 : st->bl);
    bg->radius = (unsigned char)(st->radius > 40 ? 40 : st->radius);
}

/* --- the edges of an inline box -------------------------------------------
 *
 * An inline element has a left and a right: padding, a border and a margin,
 * and a background behind the words. None of it was applied, which is why
 * two links written one after another with no space between them in the
 * markup came out as one word -- on a real page the space between them is
 * padding and nothing else, and a page whose navigation reads
 * "GmailImages" is not a page anybody can use.
 *
 * The top and the bottom are deliberately not applied. Padding above and
 * below an inline box does not move the line it is on, it overflows it, and
 * a layout that pushed the line down instead would space every paragraph
 * containing a styled word differently from one without.
 *
 * A box that started on one line and ended on another is drawn as nothing.
 * Splitting it into a piece per line is what a browser does; drawing one
 * rectangle from where it started to where it ended would be a band across
 * everything in between, which is worse than the gap.
 */
/* Where the walk goes after skipping everything under `at`: the next thing
   after it in the document, but never past the end of the run. `at` can be
   the run itself -- a form field or a hidden element laid out on its own --
   and when it was the last thing in its parent, climbing to find what came
   next went up out of the run and on into the rest of the document, which
   was then laid out twice: once flowed on to the line after the field, and
   again where it belonged. Every search form ending in a button did it. */
static inline int lay_past(const ddoc *d, int at, int node) {
    for (int s = at; s >= 0 && s != node; s = d->nodes[s].parent)
        if (d->nodes[s].next >= 0) return d->nodes[s].next;
    return -1;
}

static inline void lay_inline(lctx *L, int node, const cstyle *parent, int *y) {
    const ddoc *d = L->d;
    cstyle stack[LAY_DEPTH];
    int stack_node[LAY_DEPTH];
    int stack_x[LAY_DEPTH];        /* where the box began */
    int stack_top[LAY_DEPTH];      /* and on which line */
    int stack_slot[LAY_DEPTH];     /* its background, taken now, filled later */
    int sp = 0;
    lay_cs(&stack[0], parent);
    stack_node[0] = -1;
    stack_x[0] = 0;
    stack_top[0] = 0;
    stack_slot[0] = -1;

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
            lay_inline_close(L, &stack[sp], stack_x[sp], stack_top[sp],
                             stack_slot[sp]);
            sp--;
        }

        const dnode *n = &d->nodes[at];
        if (n->kind == DN_TEXT) {
            if ((stack[sp].visible || lay_show_hidden) && n->text >= 0)
                lay_text_run(L, d->arena + n->text, &stack[sp], y);
        } else {
            cstyle st;
            lay_style(L, at, &stack[sp], &st, L->line_width);
            if (st.display == D_NONE || lay_unseen(&st)) {
                /* Skip the subtree entirely. */
                at = lay_past(d, at, node);
                continue;
            }
            if (st.floated && at != L->floating && st.position != POS_ABSOLUTE
                && st.position != POS_FIXED) {
                /* To the side, from the line it came in on; a line with nothing
                   on it yet is started again beside it. */
                lay_float(L, at, &stack[sp], L->cont_left, L->cont_width, L->line_top);
                if (L->line_n == 0)
                    lay_line_start(L, L->line_top, L->cont_left, L->cont_width, L->align);
                at = lay_past(d, at, node);
                continue;
            }
            int blockish = st.display == D_BLOCK || st.display == D_FLEX
                        || st.display == D_LIST_ITEM || st.display == D_TABLE
                        || st.display == D_TABLE_ROW || st.display == D_TABLE_GROUP
                        || st.display == D_GRID;
            if (n->tag != T_BR && n->tag != T_IMG && lay_control_kind(d, at) == CTL_NONE) {
                if (st.position == POS_ABSOLUTE || st.position == POS_FIXED) {
                    lay_inline_piece(L, at, &stack[sp], 1, y);
                    at = lay_past(d, at, node);
                    continue;
                }
                if (blockish && at != node) {
                    /* A block inside something inline -- a card that is a
                       link round a div, a div inside a span -- ends the line,
                       takes the whole width as a block does, and the words
                       after it start a line of their own. It was run into
                       the text instead, so a card's heading, its picture and
                       its summary came out as one sentence. */
                    int left = L->cont_left, width = L->cont_width, al = L->align;
                    lay_line_end(L, y);
                    lay_block(L, at, &stack[sp], left, width, y);
                    lay_line_start(L, *y, left, width, al);
                    at = lay_past(d, at, node);
                    continue;
                }
                if (st.display == D_INLINE_BLOCK || st.display == D_INLINE_FLEX
                    || st.display == D_TABLE_CELL) {
                    lay_inline_piece(L, at, &stack[sp], 0, y);
                    at = lay_past(d, at, node);
                    continue;
                }
            }
            if (n->tag == T_BR) {
                int left = L->cont_left, width = L->cont_width, al = L->align;
                lay_line_fit(L, st.font_px, st.line_h);
                lay_line_end(L, y);
                /* <br clear="all">, the old way of going on below a picture. */
                const char *cl = dom_attr(d, at, "clear");
                if (cl) {
                    int side = lay_same_fold(cl, "left") ? 1 : lay_same_fold(cl, "right") ? 2
                             : lay_same_fold(cl, "none") ? 0 : 3;
                    if (side) *y = lay_cleared(L, *y, side, left, width);
                }
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
                        int left = L->cont_left, width = L->cont_width;
                        int al = L->align;
                        lay_line_end(L, y);
                        lay_line_start(L, *y, left, width, al);
                    }

                    litem *it = (st.visible || lay_show_hidden) ? lay_item(L) : 0;
                    if (!it) {
                        L->pen += iw;
                        lay_line_fit(L, ih, 100);
                    }
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
                        /* A hundred per cent of its own height. The second
                           argument is a percentage of the first, so passing
                           the height twice asked for a line of ih*ih/100 --
                           right only for a picture a hundred pixels tall,
                           and too short for every smaller one, which is
                           what put the next line through the bottom of it. */
                        lay_line_fit(L, ih, 100);
                        L->line_started = 1;
                        L->pending_space = 0;
                    }
                } else {
                    /* Nothing arrived. A picture whose size the page gave
                       keeps that room, as an empty frame: its alt text poured
                       into a column the width of a thumbnail was a stack of
                       single words down the page. One whose size is not known
                       is the words it came with, which is what alt text is
                       for and a great deal more use than a gap. */
                    const char *aw = dom_attr(d, at, "width");
                    const char *ah = dom_attr(d, at, "height");
                    int fw = st.width >= 0 ? st.width : aw ? lay_number(aw) : 0;
                    int fh = st.height >= 0 ? st.height : ah ? lay_number(ah) : 0;
                    if (fw > L->line_width) { if (fh > 0) fh = fh * L->line_width / fw; fw = L->line_width; }
                    const char *alt = dom_attr(d, at, "alt");
                    if (fw >= 24 && fh >= 16) {
                        if (L->pen + fw > L->line_left + L->line_width && L->pen > L->line_left) {
                            int left = L->cont_left, width = L->cont_width, al = L->align;
                            lay_line_end(L, y);
                            lay_line_start(L, *y, left, width, al);
                        }
                        litem *it = lay_item(L);
                        if (it) {
                            it->kind = LK_BOX;
                            it->node = at;
                            it->x = L->pen;
                            it->y = L->line_top;
                            it->w = fw;
                            it->h = fh;
                            it->bt = it->br = it->bb = it->bl = 1;
                            it->border = 0xDDDDDD;
                            it->link = L->cur_link;
                            L->pen += fw;
                            lay_line_fit(L, fh, 100);
                            L->line_started = 1;
                            L->pending_space = 0;
                        }
                    } else if (alt && *alt) {
                        cstyle s2;
                        lay_cs(&s2, &st);
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
                /* An invisible control -- a checkbox made transparent so that
                   a label drawn over it can be clicked instead -- takes no
                   room either: it is always positioned out of the way too. */
                if (!st.visible && !lay_show_hidden) ck = CTL_HIDDEN;
                if (ck != CTL_HIDDEN) {
                    if (L->pen + fw > L->line_left + L->line_width
                        && L->pen > L->line_left) {
                        int left = L->cont_left, width = L->cont_width;
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

                        /* A page that rounds its search box gets a rounded
                           search box. Carried through here because a field
                           is drawn by the browser rather than by the block
                           code, so nothing else would ever look at it. */
                        it->radius = (unsigned char)(st.radius > 255 ? 255
                                                     : (st.radius < 0 ? 0
                                                        : st.radius));
                        it->border = st.border_color;
                        it->bt = (unsigned char)(st.bt > 8 ? 8 : st.bt);
                        it->br = (unsigned char)(st.br > 8 ? 8 : st.br);
                        it->bb = (unsigned char)(st.bb > 8 ? 8 : st.bb);
                        it->bl = (unsigned char)(st.bl > 8 ? 8 : st.bl);

                        L->pen += fw + 2;
                        lay_line_fit(L, fh, 100);
                        L->line_started = 1;
                        L->pending_space = 0;
                    }
                }

                /* What is inside a control belongs to the control and not
                   to the page: the text in a button is its label and the
                   text in a textarea is its value, and the box draws both
                   rather than letting them flow out after it. */
                at = lay_past(d, at, node);
                continue;
            } else if (sp + 1 < LAY_DEPTH) {
                sp++;
                lay_cs(&stack[sp], &st);
                stack_node[sp] = at;
                lay_inline_open(L, at, &st, &stack_x[sp], &stack_top[sp],
                                &stack_slot[sp]);
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

    /* Whatever is still open. The walk stops as soon as there is nowhere
       left to go rather than on the way back up, so the right edge of the
       last box would otherwise never be added and its background never
       filled in. */
    while (sp > 0) {
        lay_inline_close(L, &stack[sp], stack_x[sp], stack_top[sp],
                         stack_slot[sp]);
        sp--;
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
        || st.display == D_FLEX || st.display == D_TABLE
        || st.display == D_TABLE_ROW || st.display == D_TABLE_GROUP
        || st.display == D_GRID;
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

#define LAY_FLEX_MAX 128

/* Lays a node out and forgets it, returning how wide its content came out
   and how tall. Everything the layout was in the middle of is put back. */
/* What a measurement came to, kept for the rest of the layout: which
   element, at what width, and whether as a flex item or a table cell whose
   own width does not count. The answer cannot change inside one layout, and
   a table inside a table inside a table measured each cell again at every
   level -- three times over per level -- where once is enough. */
#define LAY_MCACHE 8192
typedef struct { int node, avail, right, h; unsigned gen; unsigned char sized; } lmcache;
static lmcache lay_mcache[LAY_MCACHE];

static int lay_measure(lctx *L, int node, const cstyle *parent, int avail,
                       int *height) {
    unsigned char sized = (unsigned char)(L->flex_sized == node);
    unsigned slot = ((unsigned)node * 2654435761u ^ (unsigned)avail * 40503u ^ sized) % LAY_MCACHE;
    lmcache *mc = &lay_mcache[slot];
    if (mc->gen == lay_gen && mc->node == node && mc->avail == avail && mc->sized == sized) {
        if (sized) L->flex_sized = -1;
        *height = mc->h;
        return mc->right;
    }

    /* Field by field rather than by copying the whole context.
     *
     * A struct assignment of something this size is a call to memcpy, and
     * there is no memcpy to call: this is a program with no library under
     * it. The compiler is within its rights and the linker says so. */
    int s_line_at = L->line_at, s_line_n = L->line_n;
    int s_pen = L->pen, s_top = L->line_top;
    int s_h = L->line_h, s_base = L->line_base;
    int s_cl = L->cont_left, s_cw = L->cont_width;
            int s_left = L->line_left, s_width = L->line_width;
    int s_align = L->align, s_space = L->pending_space;
    int s_started = L->line_started, s_link = L->cur_link;
    int s_depth = L->list_depth;
    int s_count[LAY_DEPTH];
    for (int i = 0; i < LAY_DEPTH; i++) s_count[i] = L->list_count[i];
    int s_groups = L->ngroups, s_floats = L->fl_seq;

    int items = L->out->nitems, used = L->out->used, links = L->out->nlinks;

    /* A trial that ran out of room is not a page that ran out of room. The
       items this makes are thrown away a few lines down, so the mark saying
       the page did not fit is thrown away with them -- otherwise a flex row
       measuring a wide child reports the whole document as too big for a
       layout that then fits perfectly well. */
    int spilled = L->out->overflowed;

    int s_mright = L->measure_right;
    L->measure_right = 0;
    L->measuring++;
    int y = 0;
    lay_block(L, node, parent, 0, avail, &y);
    L->measuring--;

    int right = L->measure_right;
    for (int i = items; i < L->out->nitems; i++) {
        int r = L->out->items[i].x + L->out->items[i].w;
        if (r > right) right = r;
    }
    L->measure_right = s_mright;

    *height = y;

    L->line_at = s_line_at; L->line_n = s_line_n;
    L->pen = s_pen; L->line_top = s_top;
    L->line_h = s_h; L->line_base = s_base;
    L->line_left = s_left; L->line_width = s_width;
    L->cont_left = s_cl; L->cont_width = s_cw;
    L->align = s_align; L->pending_space = s_space;
    L->line_started = s_started; L->cur_link = s_link;
    L->list_depth = s_depth;
    for (int i = 0; i < LAY_DEPTH; i++) L->list_count[i] = s_count[i];

    L->out->nitems = items;
    L->out->used = used;
    L->out->nlinks = links;
    L->out->overflowed = spilled;
    L->ngroups = s_groups;
    L->fl_seq = s_floats;

    mc->gen = lay_gen; mc->node = node; mc->avail = avail; mc->sized = sized;
    mc->right = right; mc->h = *height;
    return right;
}

/* A flex container. `cx` and `cw` are inside its own padding and border, and
   `y` is where its contents start and where they are finished. */
/* One line of flex items along the row: kid[0..n), with what each would
   like (want, which this changes to what each gets), the narrowest each can
   go (low), its measured height and how much it grows. */
static void lay_flex_line(lctx *L, const cstyle *st, const int *kid, int n,
                          const int *meas, int *want, const int *low,
                          const int *high, const int *grow, int cx, int cw,
                          int gap, int reverse, int *y) {
    int total = 0, grows = 0;
    for (int i = 0; i < n; i++) { total += want[i]; grows += grow[i]; }
    total += gap * (n - 1);

    /* Too wide: everything shrinks in proportion to its size, but nothing
       below the narrowest it can go -- its longest word, its picture. Down
       to eight pixels, as it was, set a row of links in overlapping letters
       wherever the row was longer than the window. What will not fit then
       runs over the end, as it does anywhere else. */
    if (total > cw && total > 0) {
        int over = total - cw;
        for (int round = 0; round < 3 && over > 0; round++) {
            int give = 0;
            for (int i = 0; i < n; i++) if (want[i] > low[i]) give += want[i];
            if (give <= 0) break;
            int took = 0;
            for (int i = 0; i < n; i++) {
                if (want[i] <= low[i]) continue;
                int cut = (int)((long long)over * want[i] / give);
                if (cut > want[i] - low[i]) cut = want[i] - low[i];
                want[i] -= cut;
                took += cut;
            }
            over -= took;
            if (took == 0) break;
        }
        total = gap * (n - 1);
        for (int i = 0; i < n; i++) total += want[i];
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

    int top = *y;

    /* Being measured rather than laid out, the row stops here.
     *
     * Its height is the tallest child's measured height either way, and what
     * a measurement wants besides is how far right the row reaches, which is
     * each child's place plus the narrower of what it measured and what it
     * was given. Laying the children out again to find that out is what made
     * nesting cost twice as much at every level: a row measured each child by
     * laying it out and then laid it out again, and a row inside it did the
     * same inside both of those, so ten rows deep was a thousand times the
     * work of one. Measured, a row only measures, and the one real layout at
     * the end is still the full one at the widths it settled on. The reach is
     * exact unless a child centres its content or sizes it by percentage,
     * where a measurement was already an estimate. */
    if (L->measuring) {
        int tallest = 0;
        for (int i = 0; i < n; i++) if (high[i] > tallest) tallest = high[i];
        for (int idx = 0; idx < n; idx++) {
            int i = reverse ? n - 1 - idx : idx;
            int reach = pen + (meas[i] < want[i] ? meas[i] : want[i]);
            if (reach > L->measure_right) L->measure_right = reach;
            pen += want[i] + between;
        }
        *y = top + tallest;
        return;
    }

    /* Laid out at the widths they settled on, and then lined up against the
       tallest as it actually came out: the height a child measured at the
       width it would have liked is not its height at the one it got, and
       aligning to the first put text that wrapped once more through the row
       under it. */
    int first[LAY_FLEX_MAX + 1], got[LAY_FLEX_MAX];
    int tallest = 0;
    for (int idx = 0; idx < n; idx++) {
        int i = reverse ? n - 1 - idx : idx;
        first[idx] = L->out->nitems;
        int child_y = top;
        L->flex_sized = kid[i];
        lay_block(L, kid[i], st, pen, want[i], &child_y);
        L->flex_sized = -1;
        got[idx] = child_y - top;
        if (got[idx] > tallest) tallest = got[idx];
        pen += want[i] + between;
    }
    first[n] = L->out->nitems;

    /* Slid down the cross axis afterwards, which is cheaper than laying it
       out somewhere else and gives the same answer. Stretch is left where it
       is: making a child taller means laying it out again with a height it
       did not ask for, and a box at the top of its row is what stretch looks
       like when everything in it is the same height anyway. */
    for (int idx = 0; idx < n; idx++) {
        int dy = 0;
        if (st->align_items == AI_CENTER) dy = (tallest - got[idx]) / 2;
        else if (st->align_items == AI_END) dy = tallest - got[idx];
        if (dy > 0)
            for (int k = first[idx]; k < first[idx + 1]; k++)
                L->out->items[k].y += dy;
    }

    *y = top + tallest;
}

static void lay_flex(lctx *L, int node, const cstyle *st, int cx, int cw,
                     int *y) {
    const ddoc *d = L->d;

    /* Past this many the rest are laid out as a column under the row, which
       is where they would have wrapped to on a page that let them. They used
       to be dropped: a list of fifty links in a flex row showed thirty-two. */
    int kid[LAY_FLEX_MAX];
    int n = 0, extra = -1;
    for (int c = d->nodes[node].first; c >= 0; c = d->nodes[c].next) {
        if (d->nodes[c].kind != DN_ELEMENT) continue;
        if (n < LAY_FLEX_MAX) kid[n++] = c;
        else { extra = c; break; }
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
    } else {
        /* --- along the line ------------------------------------------------ */
        int want[LAY_FLEX_MAX], high[LAY_FLEX_MAX], grow[LAY_FLEX_MAX];
        int meas[LAY_FLEX_MAX], low[LAY_FLEX_MAX];

        for (int i = 0; i < n; i++) {
            int h = 0, h2 = 0;
            /* Measured with room to spare, so the answer is how wide the
               child would like to be rather than how wide it was squeezed
               into; and as narrow as it will go, which is as far as it
               shrinks. */
            int w = lay_measure(L, kid[i], st, cw > 0 ? cw : 2000, &h);
            if (w < 1) w = 1;
            if (w > cw && cw > 0) w = cw;

            cstyle own;
            lay_style(L, kid[i], st, &own, cw);
            if (own.width >= 0)
                w = own.width + (own.border_box ? 0 : own.pl + own.pr + own.bl + own.br)
                  + (own.ml > 0 ? own.ml : 0) + (own.mr > 0 ? own.mr : 0);

            /* A flex item's own width is not a floor, as a grid item's is:
               it shrinks to the narrowest its contents allow, so it is
               measured with its own width set aside (and its children's
               kept: a card of 280 pixels inside it cannot go narrower). */
            int lo;
            if (own.min_width >= 0) lo = own.min_width;
            else if (own.clip) lo = 8;
            else { L->flex_sized = kid[i]; lo = lay_measure(L, kid[i], st, 1, &h2); L->flex_sized = -1; }
            if (lo > w) lo = w;
            if (lo < 1) lo = 1;

            meas[i] = w;
            want[i] = w;
            low[i] = lo;
            high[i] = h;
            grow[i] = own.grow > 0 ? own.grow : 0;
        }

        /* A row that would scroll sideways -- a shelf of cards, a strip of
           albums -- cannot be scrolled inside a page here, and squeezed onto
           one line its cards were written over each other. It wraps. */
        if (!st->flex_wrap && st->clip != 2) {
            lay_flex_line(L, st, kid, n, meas, want, low, high, grow, cx, cw, gap, reverse, y);
        } else {
            /* Wrapping: as many as fit on each line, each line a row of its
               own. Card grids are built this way, and one row of every card
               squeezed to a sliver was the same page with nothing readable
               in it. */
            int i0 = 0;
            while (i0 < n) {
                int i1 = i0, used = 0;
                while (i1 < n) {
                    int wi = want[i1] > low[i1] ? want[i1] : low[i1];
                    int add = wi + (i1 > i0 ? gap : 0);
                    if (i1 > i0 && used + add > cw) break;
                    used += add;
                    i1++;
                }
                if (i0) *y += gap;
                lay_flex_line(L, st, kid + i0, i1 - i0, meas + i0, want + i0, low + i0,
                              high + i0, grow + i0, cx, cw, gap, reverse, y);
                i0 = i1;
            }
        }
    }

    for (int c = extra; c >= 0; c = d->nodes[c].next)
        if (d->nodes[c].kind == DN_ELEMENT) lay_block(L, c, st, cx, cw, y);
}

/* --- tables ---------------------------------------------------------------
 *
 * Rows of cells in columns, each column as wide as the widest thing in it
 * needs and the table as wide as its columns, or as wide as it was told.
 * Tables were laid out as blocks with their cells run together as words,
 * which is readable for one row and wrong for everything else: a table
 * inside a cell -- which is how the old web and some of the biggest sites'
 * plainest pages are built -- ran every row of it into one paragraph.
 *
 * This is the automatic layout, shortened:
 *
 *   each cell is measured twice, as narrow as it will go (its longest word)
 *   and as wide as it would like (nothing wrapped), and each column takes
 *   the most any of its cells asks for; a cell across several columns
 *   shares what it asks for out among them
 *   a table given a width fills it; one that was not is as wide as its
 *   columns would like, or as wide as there is room for when that is less
 *   between the two, the room over the narrowest is handed out in
 *   proportion to how much more each column would like
 *   then each row is laid out cell by cell at those widths, is as tall as
 *   its tallest cell, and each cell's background is stretched to the row
 *
 * Cells spanning rows keep their column clear below them, and a row grows
 * to fit one that ends in it. What is left out: fixed layout (it is treated
 * as automatic), borders collapsing into one line (they are drawn per cell
 * with no room between), and captions anywhere but the top. */
#define LAY_TCOLS 64
#define LAY_TSPANS 128

typedef struct {
    int ncols;
    int col_w[LAY_TCOLS];
    int width;                  /* all the columns and the room around them */
    int spacing;
} ltable;

enum { TR_ROW = 0, TR_CELLS, TR_BLOCK };

/* How a child of a table is taken: its display, asked with the table's
   style for a parent (display is not inherited, so that is enough). */
static int lay_display_of(lctx *L, int el, const cstyle *parent) {
    cstyle st;
    lay_style(L, el, parent, &st, L->line_width);
    return st.display;
}

/* Whether an element directly holds rows or cells, which is what makes a
   form or a div between a table and its rows something to look through. */
static int lay_holds_rows(const ddoc *d, int el) {
    for (int c = d->nodes[el].first; c >= 0; c = d->nodes[c].next) {
        int t = d->nodes[c].tag;
        if (d->nodes[c].kind == DN_ELEMENT && (t == T_TR || t == T_TD || t == T_TH
            || lay_named(d, c, "tbody") || lay_named(d, c, "thead") || lay_named(d, c, "tfoot")))
            return 1;
    }
    return 0;
}

/* The row after `at` (-1 for the first) and what kind it is: a tr, a run of
   cells written with no tr round them, or something that is neither, laid
   out across the whole table. Groups and anything holding rows are looked
   through; captions are laid out before the rows and skipped here. */
static int lay_table_next(lctx *L, int table, const cstyle *tst, int at, int *kind) {
    const ddoc *d = L->d;
    int n;
    if (at < 0) {
        n = d->nodes[table].first;
    } else if (*kind == TR_CELLS) {
        n = d->nodes[at].next;
        while (n >= 0 && (d->nodes[n].kind != DN_ELEMENT
                          || lay_display_of(L, n, tst) == D_TABLE_CELL))
            n = d->nodes[n].next;
        if (n < 0) {
            /* The run was the last thing in its parent: on past the parent. */
            int p = d->nodes[at].parent;
            n = p == table ? -1 : lay_past(d, p, table);
        }
    } else {
        n = lay_past(d, at, table);
    }
    while (n >= 0) {
        if (d->nodes[n].kind != DN_ELEMENT) { n = lay_past(d, n, table); continue; }
        if (d->nodes[n].tag == T_CAPTION) { n = lay_past(d, n, table); continue; }
        int disp = lay_display_of(L, n, tst);
        if (disp == D_NONE) { n = lay_past(d, n, table); continue; }
        if (disp == D_TABLE_ROW) { *kind = TR_ROW; return n; }
        if (disp == D_TABLE_CELL) { *kind = TR_CELLS; return n; }
        if ((disp == D_TABLE_GROUP || lay_holds_rows(d, n)) && d->nodes[n].first >= 0) {
            n = d->nodes[n].first;
            continue;
        }
        if (disp == D_TABLE_GROUP) { n = lay_past(d, n, table); continue; }
        *kind = TR_BLOCK;
        return n;
    }
    return -1;
}

/* The cells of a row, in order: the first after `at` (-1 for the first). */
static int lay_row_cell(lctx *L, int row, int kind, const cstyle *rst, int at) {
    const ddoc *d = L->d;
    if (kind == TR_BLOCK) return at < 0 ? row : -1;
    int n;
    if (kind == TR_CELLS) n = at < 0 ? row : d->nodes[at].next;
    else n = at < 0 ? d->nodes[row].first : d->nodes[at].next;
    for (; n >= 0; n = d->nodes[n].next) {
        if (d->nodes[n].kind != DN_ELEMENT) continue;
        int disp = lay_display_of(L, n, rst);
        if (disp == D_NONE) continue;
        if (kind == TR_CELLS && disp != D_TABLE_CELL) return -1;
        return n;
    }
    return -1;
}

static int lay_span_attr(const ddoc *d, int el, const char *name, int most) {
    const char *v = dom_attr(d, el, name);
    int n = v ? lay_number(v) : 1;
    if (n < 1) n = 1;
    if (n > most) n = most;
    return n;
}

/* A row's style, from the table's through whatever lies between. */
static void lay_row_style(lctx *L, int table, const cstyle *tst, int row, cstyle *out) {
    const ddoc *d = L->d;
    int chain[6], n = 0;
    for (int at = row; at >= 0 && at != table && n < 6; at = d->nodes[at].parent) chain[n++] = at;
    cstyle up, st;
    lay_cs(&up, tst);
    while (n--) {
        lay_style(L, chain[n], &up, &st, L->line_width);
        lay_cs(&up, &st);
    }
    lay_cs(out, &up);
}

/* The columns: how many, and how wide. `avail` is the room inside the
   table's own padding and border; `fill` says the table was given a width
   and fills it rather than shrinking to its columns. */
static void lay_table_plan(lctx *L, int table, const cstyle *tst, int avail, int fill,
                           ltable *T) {
    const ddoc *d = L->d;
    int spacing = tst->spacing >= 0 ? tst->spacing : 2;
    T->spacing = spacing;
    T->ncols = 0;
    T->width = 0;

    int mn[LAY_TCOLS], mx[LAY_TCOLS], fixed[LAY_TCOLS], busy[LAY_TCOLS];
    for (int c = 0; c < LAY_TCOLS; c++) { mn[c] = mx[c] = busy[c] = 0; fixed[c] = -1; }
    struct { int col, span, lo, hi; } sp[LAY_TSPANS];
    int nsp = 0, ncols = 0, whole = 0;

    /* Past this many cells the rest are laid out at the widths the first
       ones settled, which is what a long listing wants anyway. */
    int budget = 1500;
    int kind = 0;
    for (int row = lay_table_next(L, table, tst, -1, &kind); row >= 0 && budget > 0;
         row = lay_table_next(L, table, tst, row, &kind)) {
        cstyle rst;
        if (kind == TR_ROW) lay_row_style(L, table, tst, row, &rst);
        else lay_row_style(L, table, tst, d->nodes[row].parent, &rst);
        int col = 0;
        for (int cell = lay_row_cell(L, row, kind, &rst, -1); cell >= 0;
             cell = lay_row_cell(L, row, kind, &rst, cell)) {
            while (col < LAY_TCOLS && busy[col]) col++;
            if (col >= LAY_TCOLS) break;
            int span = kind == TR_BLOCK ? LAY_TCOLS : lay_span_attr(d, cell, "colspan", LAY_TCOLS);
            int rows = kind == TR_BLOCK ? 1 : lay_span_attr(d, cell, "rowspan", 1000);
            if (kind == TR_BLOCK) col = 0;
            if (col + span > LAY_TCOLS) span = LAY_TCOLS - col;

            cstyle cst;
            lay_style(L, cell, &rst, &cst, avail);
            int h;
            L->flex_sized = cell;
            int lo = lay_measure(L, cell, &rst, 1, &h);
            L->flex_sized = cell;
            int hi = lay_measure(L, cell, &rst, avail, &h);
            L->flex_sized = -1;
            if (hi < lo) hi = lo;
            budget--;
            int want = -1;
            if (cst.width >= 0 && kind != TR_BLOCK)
                want = cst.width + (cst.border_box ? 0 : cst.pl + cst.pr + cst.bl + cst.br);

            if (kind == TR_BLOCK) {
                whole = 1;
                if (nsp < LAY_TSPANS) {
                    sp[nsp].col = 0; sp[nsp].span = LAY_TCOLS; sp[nsp].lo = lo; sp[nsp].hi = hi;
                    nsp++;
                }
                break;
            }
            if (span == 1) {
                if (lo > mn[col]) mn[col] = lo;
                if (hi > mx[col]) mx[col] = hi;
                if (want > fixed[col]) fixed[col] = want;
            } else if (nsp < LAY_TSPANS) {
                sp[nsp].col = col; sp[nsp].span = span; sp[nsp].lo = lo; sp[nsp].hi = hi;
                nsp++;
            }
            for (int k = col; k < col + span && k < LAY_TCOLS; k++) busy[k] = rows;
            col += span;
            if (col > ncols) ncols = col;
        }
        for (int k = 0; k < LAY_TCOLS; k++) if (busy[k] > 0) busy[k]--;
    }
    /* Nothing but rows laid across the whole table: one column. */
    if (ncols == 0 && whole) ncols = 1;
    if (ncols == 0) return;

    /* A width asked for is also what the column would like. */
    for (int c = 0; c < ncols; c++) {
        if (fixed[c] >= 0) mx[c] = fixed[c] > mn[c] ? fixed[c] : mn[c];
        if (mx[c] < mn[c]) mx[c] = mn[c];
    }
    /* Cells across columns: whatever they need beyond what the columns
       already give is shared out evenly. */
    for (int i = 0; i < nsp; i++) {
        int c0 = sp[i].col, span = sp[i].span;
        if (c0 + span > ncols) span = ncols - c0;
        if (span <= 0) continue;
        int slo = spacing * (span - 1);
        for (int c = c0; c < c0 + span; c++) slo += mn[c];
        if (sp[i].lo > slo) {
            int more = sp[i].lo - slo;
            for (int c = c0; c < c0 + span; c++) {
                int add = more / (c0 + span - c);
                mn[c] += add; more -= add;
                if (mx[c] < mn[c]) mx[c] = mn[c];
            }
        }
        int shi = spacing * (span - 1);
        for (int c = c0; c < c0 + span; c++) shi += mx[c];
        if (sp[i].hi > shi) {
            int more = sp[i].hi - shi;
            for (int c = c0; c < c0 + span; c++) {
                int add = more / (c0 + span - c);
                mx[c] += add; more -= add;
            }
        }
    }

    int room = avail - spacing * (ncols + 1);
    if (room < ncols) room = ncols;
    int summin = 0, summax = 0;
    for (int c = 0; c < ncols; c++) { summin += mn[c]; summax += mx[c]; }
    int target = fill ? room : (summax < room ? summax : room);
    if (target < summin) target = summin;

    if (target >= summax) {
        /* Room over: to the columns nobody gave a width, in proportion. */
        int extra = target - summax, share = 0;
        for (int c = 0; c < ncols; c++) if (fixed[c] < 0) share += mx[c] > 0 ? mx[c] : 1;
        int all = share == 0;
        if (all) for (int c = 0; c < ncols; c++) share += mx[c] > 0 ? mx[c] : 1;
        int left = extra;
        for (int c = 0; c < ncols; c++) {
            int wgt = mx[c] > 0 ? mx[c] : 1;
            int add = (all || fixed[c] < 0) && share > 0 ? (int)((long long)extra * wgt / share) : 0;
            T->col_w[c] = mx[c] + add;
            left -= add;
        }
        for (int c = ncols - 1; c >= 0 && left > 0; c--)
            if (all || fixed[c] < 0) { T->col_w[c] += left; left = 0; }
    } else {
        int over = target - summin, want = summax - summin;
        for (int c = 0; c < ncols; c++)
            T->col_w[c] = mn[c] + (want > 0 ? (int)((long long)over * (mx[c] - mn[c]) / want) : 0);
    }
    T->ncols = ncols;
    int w = spacing * (ncols + 1);
    for (int c = 0; c < ncols; c++) w += T->col_w[c];
    T->width = w;
}

/* Where a cell's contents sit in a row taller than they are. */
static int lay_cell_valign(lctx *L, int cell, const cstyle *rst) {
    cstyle cst;
    lay_style(L, cell, rst, &cst, L->line_width);
    if (cst.valign != VA_BASELINE) return cst.valign;
    if (rst->valign != VA_BASELINE) return rst->valign;
    return VA_MIDDLE;
}

/* A cell laid out, then moved down inside its row and its background made
   as tall as the row. */
static void lay_cell_fit(lctx *L, int cell, int first, int end, int h, int rowh, int va) {
    int dy = va == VA_MIDDLE ? (rowh - h) / 2 : va == VA_BOTTOM ? rowh - h : 0;
    for (int i = first; i < end; i++) {
        litem *it = &L->out->items[i];
        if (i == first && it->kind == LK_BOX && it->node == cell) {
            it->h = rowh;
            continue;
        }
        if (dy > 0) it->y += dy;
    }
}

static void lay_table_rows(lctx *L, int table, const cstyle *tst, int cx, const ltable *T,
                           int *y) {
    const ddoc *d = L->d;
    int spacing = T->spacing;
    int colx[LAY_TCOLS + 1];
    colx[0] = cx + spacing;
    for (int c = 0; c < T->ncols; c++) colx[c + 1] = colx[c] + T->col_w[c] + spacing;

    for (int c = d->nodes[table].first; c >= 0; c = d->nodes[c].next)
        if (d->nodes[c].kind == DN_ELEMENT && d->nodes[c].tag == T_CAPTION)
            lay_block(L, c, tst, cx, T->width, y);
    if (T->ncols == 0) return;
    *y += spacing;

    int busy[LAY_TCOLS];
    for (int c = 0; c < LAY_TCOLS; c++) busy[c] = 0;
    /* Cells still spanning down into rows not laid out yet. */
    struct { int cell, first, end, top, rows, h, va; } tall[LAY_TCOLS];
    int ntall = 0;

    int kind = 0;
    for (int row = lay_table_next(L, table, tst, -1, &kind); row >= 0;
         row = lay_table_next(L, table, tst, row, &kind)) {
        cstyle rst;
        if (kind == TR_ROW) lay_row_style(L, table, tst, row, &rst);
        else lay_row_style(L, table, tst, d->nodes[row].parent, &rst);
        int top = *y;

        int row_slot = -1;
        if (kind == TR_ROW && rst.has_bg && !L->measuring) {
            litem *bg = lay_item(L);
            if (bg) { row_slot = L->out->nitems - 1; bg->kind = LK_BOX; bg->node = row; }
        }

        struct { int cell, first, end, h; } got[LAY_TCOLS];
        int ngot = 0, rowh = 0, col = 0;
        for (int cell = lay_row_cell(L, row, kind, &rst, -1); cell >= 0;
             cell = lay_row_cell(L, row, kind, &rst, cell)) {
            while (col < T->ncols && busy[col]) col++;
            int span, rows;
            if (kind == TR_BLOCK) { col = 0; span = T->ncols; rows = 1; }
            else {
                span = lay_span_attr(d, cell, "colspan", LAY_TCOLS);
                rows = lay_span_attr(d, cell, "rowspan", 1000);
            }
            /* A row with more cells than the table has columns (past the
               cells that were measured) puts the rest in the last one. */
            if (col >= T->ncols) col = T->ncols - 1;
            if (col + span > T->ncols) span = T->ncols - col;
            int x = colx[col];
            int w = colx[col + span] - spacing - x;

            int first = L->out->nitems;
            int cy = top;
            L->flex_sized = cell;
            lay_block(L, cell, &rst, x, w, &cy);
            L->flex_sized = -1;
            int h = cy - top;

            if (rows > 1 && ntall < LAY_TCOLS) {
                tall[ntall].cell = cell; tall[ntall].first = first;
                tall[ntall].end = L->out->nitems; tall[ntall].top = top;
                tall[ntall].rows = rows; tall[ntall].h = h;
                tall[ntall].va = lay_cell_valign(L, cell, &rst);
                ntall++;
            } else if (ngot < LAY_TCOLS) {
                got[ngot].cell = cell; got[ngot].first = first;
                got[ngot].end = L->out->nitems; got[ngot].h = h;
                ngot++;
                if (h > rowh) rowh = h;
            }
            for (int k = col; k < col + span && k < LAY_TCOLS; k++) busy[k] = rows;
            col += span;
            if (kind == TR_BLOCK) break;
        }
        if (rst.height > rowh && kind == TR_ROW) rowh = rst.height;

        /* A cell spanning down to this row makes it as tall as it needs. */
        for (int i = 0; i < ntall; i++) {
            if (tall[i].rows != 1) continue;
            int have = top + rowh - tall[i].top;
            if (tall[i].h > have) rowh += tall[i].h - have;
        }

        for (int i = 0; i < ngot; i++)
            lay_cell_fit(L, got[i].cell, got[i].first, got[i].end, got[i].h, rowh,
                         lay_cell_valign(L, got[i].cell, &rst));
        for (int i = 0; i < ntall; ) {
            if (--tall[i].rows > 0) { i++; continue; }
            lay_cell_fit(L, tall[i].cell, tall[i].first, tall[i].end, tall[i].h,
                         top + rowh - tall[i].top, tall[i].va);
            tall[i] = tall[--ntall];
        }

        if (row_slot >= 0) {
            litem *bg = &L->out->items[row_slot];
            bg->x = colx[0];
            bg->y = top;
            bg->w = colx[T->ncols] - spacing - colx[0];
            bg->h = rowh;
            bg->bg = rst.background;
            bg->has_bg = 1;
        }
        *y = top + rowh + spacing;
        for (int k = 0; k < LAY_TCOLS; k++) if (busy[k] > 0) busy[k]--;
    }
}

/* Old markup centres a table by what is round it: <center>, or align on
   the element it is in. Neither centres a block; both centre a table. */
static int lay_centres_table(const ddoc *d, int table) {
    int p = d->nodes[table].parent;
    if (p < 0 || d->nodes[p].kind != DN_ELEMENT) return 0;
    if (d->nodes[p].tag == T_CENTER) return 1;
    const char *a = dom_attr(d, p, "align");
    return a && lay_same_fold(a, "center");
}

/* --- grid -----------------------------------------------------------------
 *
 * Columns from grid-template-columns, and the items poured into them a row
 * at a time in the order they are written. Grid used to be laid out as a
 * block, so a page of cards in three columns came out as one card a row.
 *
 * The columns understood: pixels and the other lengths, percentages, fr,
 * auto (taken as 1fr), minmax(a, b) (as b, or its a when b is fr and the
 * room is short), and repeat(N, ...) including repeat(auto-fill, ...) and
 * auto-fit, which make as many as fit at their smallest. An item spans more
 * than one column with grid-column: span N, or 1 / -1 for the whole row.
 * Rows are as tall as their tallest item, with row gap and column gap the
 * one gap there is. What is not here: named areas and placement by line
 * number (items go where the next free place is), rows sized by
 * grid-template-rows, and dense packing. */
#define LAY_GRID_COLS 32

enum { GT_PX = 0, GT_PCT, GT_FR };

/* One track, from `p`: its kind and amount, and the smallest it may be
   (for minmax); where it ended. */
static const char *lay_grid_track(const char *p, const cstyle *st, int root_px, int cw,
                                  int *kind, int *v, int *min) {
    while (*p == ' ' || *p == ',') p++;
    *min = 0;
    if (w_starts_fold(p, "minmax(")) {
        int k2, v2, m2;
        p = lay_grid_track(p + 7, st, root_px, cw, &k2, &v2, &m2);
        *min = k2 == GT_PX ? v2 : k2 == GT_PCT ? v2 * cw / 100 : 0;
        p = lay_grid_track(p, st, root_px, cw, kind, v, &m2);
        while (*p && *p != ')') p++;
        if (*p) p++;
        return p;
    }
    if (w_starts_fold(p, "fit-content(")) {
        p += 12;
        *kind = GT_FR;
        *v = 100;
        while (*p && *p != ')') p++;
        if (*p) p++;
        return p;
    }
    char tok[32];
    int n = 0;
    while (*p && *p != ' ' && *p != ',' && *p != ')' && n < 31) tok[n++] = *p++;
    tok[n] = 0;
    int tl = n;
    if (tl > 2 && w_lower(tok[tl - 2]) == 'f' && w_lower(tok[tl - 1]) == 'r') {
        clen L = css_len(tok);                      /* 1fr reads as 1 of no unit */
        *kind = GT_FR;
        *v = L.v > 0 ? L.v : 100;                   /* hundredths */
        return p;
    }
    if (lay_same_fold(tok, "auto") || lay_same_fold(tok, "min-content")
        || lay_same_fold(tok, "max-content")) {
        *kind = GT_FR;
        *v = 100;
        return p;
    }
    clen L = css_len(tok);
    if (L.unit == U_PCT) { *kind = GT_PCT; *v = L.v / 100; return p; }
    *kind = GT_PX;
    *v = css_px(L, st->font_px, root_px, cw);
    if (*v < 0) *v = 0;
    return p;
}

/* A grid's tracks, from grid-template-columns: each one's kind, amount and
   least width; how many there are. With grid-auto-flow: column, `items`
   items make a column each past the ones written, at grid-auto-columns. */
static int lay_grid_tracks(lctx *L, const cstyle *st, int cw, int gap, int items,
                           int *kind, int *val, int *mins) {
    int n = 0;
    const char *p = st->grid_cols ? st->grid_cols : "";
    while (*p && n < LAY_GRID_COLS) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (w_starts_fold(p, "repeat(")) {
            p += 7;
            while (*p == ' ') p++;
            int count = 0, fill = 0;
            if (w_starts_fold(p, "auto-fill") || w_starts_fold(p, "auto-fit")) {
                fill = 1;
                while (*p && *p != ',') p++;
            } else {
                while (*p >= '0' && *p <= '9') count = count * 10 + (*p++ - '0');
                while (*p && *p != ',') p++;
            }
            if (*p == ',') p++;
            /* The tracks inside, once. */
            int rk[8], rv[8], rm[8], rn = 0;
            while (*p && *p != ')' && rn < 8) {
                while (*p == ' ') p++;
                if (*p == ')') break;
                p = lay_grid_track(p, st, L->root_px, cw, &rk[rn], &rv[rn], &rm[rn]);
                rn++;
            }
            if (*p == ')') p++;
            if (!rn) continue;
            if (fill) {
                /* As many as fit, each at the least it may be. */
                int one = 0;
                for (int i = 0; i < rn; i++)
                    one += rk[i] == GT_PX ? rv[i] : rk[i] == GT_PCT ? rv[i] * cw / 100 : (rm[i] > 0 ? rm[i] : 0);
                if (one <= 0) one = 1;
                count = (cw + gap) / (one + gap * rn);
                if (count < 1) count = 1;
            }
            for (int c = 0; c < count && n < LAY_GRID_COLS; c++)
                for (int i = 0; i < rn && n < LAY_GRID_COLS; i++) {
                    kind[n] = rk[i]; val[n] = rv[i]; mins[n] = rm[i]; n++;
                }
            continue;
        }
        const char *was = p;
        p = lay_grid_track(p, st, L->root_px, cw, &kind[n], &val[n], &mins[n]);
        n++;
        if (p == was) p++;
    }
    if (st->gflow_col && items > n) {
        int ak = GT_FR, av = 100, am = 0;
        if (st->grid_auto) lay_grid_track(st->grid_auto, st, L->root_px, cw, &ak, &av, &am);
        while (n < items && n < LAY_GRID_COLS) { kind[n] = ak; val[n] = av; mins[n] = am; n++; }
    }
    return n;
}

/* The widths of `n` tracks in `cw`, gaps between them. A flexible track
 * (fr, auto) shares what the fixed ones leave, but never comes out narrower
 * than `base`, the least its items can be drawn in: that is what 1fr means,
 * minmax(auto, 1fr), and a strip of 280 pixel cards in six 1fr columns is
 * six columns of 280 that overflow, not six slivers with the cards
 * squeezed into them. A track held at its least leaves the sharing, and the
 * others share again. */
static void lay_grid_share(int cw, int gap, int n, const int *kind, const int *val,
                           const int *base, int *width) {
    int fixed = gap * (n - 1);
    unsigned char held[LAY_GRID_COLS];
    for (int i = 0; i < n; i++) {
        held[i] = 0;
        if (kind[i] == GT_PX) fixed += val[i];
        else if (kind[i] == GT_PCT) fixed += val[i] * cw / 100;
    }
    int left = 0, frs = 0;
    for (int pass = 0; pass <= n; pass++) {
        left = cw - fixed;
        frs = 0;
        for (int i = 0; i < n; i++) {
            if (kind[i] != GT_FR) continue;
            if (held[i]) left -= base[i];
            else frs += val[i];
        }
        int again = 0;
        for (int i = 0; i < n && frs > 0; i++) {
            if (kind[i] != GT_FR || held[i]) continue;
            long long share = left > 0 ? (long long)left * val[i] / frs : 0;
            if (share < base[i]) { held[i] = 1; again = 1; }
        }
        if (!again) break;
    }
    for (int i = 0; i < n; i++) {
        if (kind[i] == GT_PX) width[i] = val[i];
        else if (kind[i] == GT_PCT) width[i] = val[i] * cw / 100;
        else if (held[i] || frs <= 0) width[i] = base[i];
        else {
            width[i] = left > 0 ? (int)((long long)left * val[i] / frs) : 0;
            if (width[i] < base[i]) width[i] = base[i];
        }
        if (width[i] < 1) width[i] = 1;
    }
}

/* The column widths for a grid `cw` wide, from what is written alone; how
   many there are. */
static int lay_grid_columns(lctx *L, const cstyle *st, int cw, int gap, int *width) {
    int kind[LAY_GRID_COLS], val[LAY_GRID_COLS], mins[LAY_GRID_COLS];
    int n = lay_grid_tracks(L, st, cw, gap, 0, kind, val, mins);
    if (n == 0) { width[0] = cw; return 1; }
    lay_grid_share(cw, gap, n, kind, val, mins, width);
    return n;
}

/* The least an item can be drawn in, as a grid track sees it: the width it
   asks for, else its min-width, else its narrowest content (measured at no
   room at all); nothing for one that hides what overflows, which is what
   the rules say such an item may be squeezed to. Margins count. */
static int lay_grid_least(lctx *L, int k, const cstyle *st, const cstyle *own, int cw) {
    int m;
    if (own->clip) m = 0;
    else if (own->width >= 0)
        m = own->width + (own->border_box ? 0 : own->pl + own->pr + own->bl + own->br);
    else if (own->min_width >= 0)
        m = own->min_width + (own->border_box ? 0 : own->pl + own->pr + own->bl + own->br);
    else {
        int h;
        m = lay_measure(L, k, st, 1, &h);
    }
    m += (own->ml > 0 ? own->ml : 0) + (own->mr > 0 ? own->mr : 0);
    (void)cw;
    return m > 0 ? m : 0;
}


/* --- named areas ---------------------------------------------------------
 *
 * grid-template-areas draws the page as words, a row of names per string:
 * "head head" "side main" "foot foot". Each name is the rectangle its cells
 * make, and an item with that grid-area goes there, whatever order it was
 * written in. It is how a page's header, sidebar, article and footer are put
 * in their places, and poured into columns one each instead, an article
 * found itself in a column the width of a date. Items that name no area
 * follow on rows of their own, across the whole grid. */
#define LAY_AREAS 24
#define LAY_AREA_ROWS 16

typedef struct { char name[24]; int r0, r1, c0, c1; } larea;

/* The areas, from their text; how many there are, and the rows and columns
   the text has. */
static int lay_grid_areas(const char *p, larea *a, int *rows, int *cols) {
    int n = 0, r = 0;
    *rows = *cols = 0;
    while (*p && r < LAY_AREA_ROWS) {
        while (*p && *p != '"' && *p != '\'') p++;
        if (!*p) break;
        char q = *p++;
        int c = 0;
        while (*p && *p != q) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p || *p == q) break;
            char name[24];
            int k = 0;
            while (*p && *p != q && *p != ' ' && *p != '\t') { if (k < 23) name[k++] = *p; p++; }
            name[k] = 0;
            if (c < LAY_GRID_COLS && !(k == 1 && name[0] == '.') && name[0] != '.') {
                int f = -1;
                for (int i = 0; i < n; i++) if (w_same(a[i].name, name)) f = i;
                if (f < 0 && n < LAY_AREAS) {
                    f = n++;
                    w_copy(a[f].name, 24, name, 24);
                    a[f].r0 = a[f].r1 = r;
                    a[f].c0 = a[f].c1 = c;
                }
                if (f >= 0) {
                    if (r < a[f].r0) a[f].r0 = r;
                    if (r > a[f].r1) a[f].r1 = r;
                    if (c < a[f].c0) a[f].c0 = c;
                    if (c > a[f].c1) a[f].c1 = c;
                }
            }
            c++;
        }
        if (*p) p++;
        if (c > *cols) *cols = c;
        r++;
    }
    *rows = r;
    return n;
}

static void lay_grid_named(lctx *L, int node, const cstyle *st, int cx, int cw, int *y) {
    const ddoc *d = L->d;
    larea a[LAY_AREAS];
    int rows, cols;
    int na = lay_grid_areas(st->grid_areas, a, &rows, &cols);
    int gap = st->gap > 0 ? st->gap : 0;
    int width[LAY_GRID_COLS];
    int n = lay_grid_columns(L, st, cw, gap, width);
    /* Fewer columns given than the areas draw: the rest share what is left. */
    if (n < cols) {
        int used = gap * (cols - 1);
        for (int c = 0; c < n; c++) used += width[c];
        if (!st->grid_cols) { used = gap * (cols - 1); n = 0; }
        int each = (cw - used) / (cols - n);
        for (int c = n; c < cols; c++) width[c] = each > 1 ? each : 1;
        n = cols;
    }
    int colx[LAY_GRID_COLS + 1];
    colx[0] = cx;
    for (int c = 0; c < n; c++) colx[c + 1] = colx[c] + width[c] + gap;

    int rowtop[LAY_AREA_ROWS + 1], endbot[LAY_AREA_ROWS];
    rowtop[0] = *y;
    for (int r = 0; r < rows; r++) endbot[r] = -1;
    for (int r = 0; r < rows; r++) {
        for (int k = d->nodes[node].first; k >= 0; k = d->nodes[k].next) {
            if (d->nodes[k].kind != DN_ELEMENT) continue;
            cstyle own;
            lay_style(L, k, st, &own, cw);
            if (own.display == D_NONE || !own.garea) continue;
            char name[24];
            int q = 0;
            for (const char *t = own.garea; *t && *t != ' ' && *t != '/' && q < 23; t++)
                name[q++] = *t;
            name[q] = 0;
            int f = -1;
            for (int i = 0; i < na; i++) if (w_same(a[i].name, name)) f = i;
            if (f < 0 || a[f].r0 != r) continue;
            /* Laid out on the row it starts on; it holds up the row it ends on. */
            int c1 = a[f].c1 < n ? a[f].c1 : n - 1;
            int x = colx[a[f].c0], w = colx[c1 + 1] - gap - x, cy = rowtop[r];
            L->flex_sized = k;
            lay_block(L, k, st, x, w, &cy);
            L->flex_sized = -1;
            if (cy > endbot[a[f].r1]) endbot[a[f].r1] = cy;
        }
        rowtop[r + 1] = endbot[r] > rowtop[r] ? endbot[r] + gap : rowtop[r];
    }
    int yy = rowtop[rows];
    if (yy > *y) yy -= gap;

    /* What names no area, on rows of its own below. */
    for (int k = d->nodes[node].first; k >= 0; k = d->nodes[k].next) {
        if (d->nodes[k].kind != DN_ELEMENT) continue;
        cstyle own;
        lay_style(L, k, st, &own, cw);
        if (own.display == D_NONE) continue;
        int f = -1;
        if (own.garea) {
            char name[24];
            int q = 0;
            for (const char *t = own.garea; *t && *t != ' ' && *t != '/' && q < 23; t++)
                name[q++] = *t;
            name[q] = 0;
            for (int i = 0; i < na; i++) if (w_same(a[i].name, name)) f = i;
        }
        if (f >= 0) continue;
        if (yy > *y) yy += gap;
        lay_block(L, k, st, cx, cw, &yy);
    }
    *y = yy;
}

static void lay_grid(lctx *L, int node, const cstyle *st, int cx, int cw, int *y) {
    if (st->grid_areas) { lay_grid_named(L, node, st, cx, cw, y); return; }
    const ddoc *d = L->d;
    int gap = st->gap > 0 ? st->gap : 0;
    int width[LAY_GRID_COLS];

    /* The items, counted, for a grid that makes a column for each; then
       the least each column's items can be drawn in. */
    int nitems = 0;
    for (int k = d->nodes[node].first; k >= 0; k = d->nodes[k].next) {
        if (d->nodes[k].kind != DN_ELEMENT) continue;
        cstyle own;
        lay_style(L, k, st, &own, cw);
        if (own.display != D_NONE) nitems++;
    }
    int kind[LAY_GRID_COLS], val[LAY_GRID_COLS], base[LAY_GRID_COLS];
    int ncols = lay_grid_tracks(L, st, cw, gap, nitems, kind, val, base);
    if (ncols == 0) { ncols = 1; kind[0] = GT_PX; val[0] = cw; base[0] = 0; }
    if (ncols > 1) {
        int col = 0, most = 0;
        for (int k = d->nodes[node].first; k >= 0; k = d->nodes[k].next) {
            if (d->nodes[k].kind != DN_ELEMENT) continue;
            cstyle own;
            lay_style(L, k, st, &own, cw);
            if (own.display == D_NONE) continue;
            int span = own.gspan < 0 ? ncols : own.gspan > 0 ? own.gspan : 1;
            if (span > ncols) span = ncols;
            if (col + span > ncols) col = 0;
            if (span == 1) {
                int m = lay_grid_least(L, k, st, &own, cw);
                if (kind[col] == GT_FR && m > base[col]) base[col] = m;
                if (m > most) most = m;
            }
            col += span;
        }
        lay_grid_share(cw, gap, ncols, kind, val, base, width);

        /* One that would scroll sideways cannot be scrolled inside a page
           here, so, like a scrolling flex row, it wraps: as many columns
           as fit, each at least as wide as its widest item, and the rest
           of the items on the rows below. */
        if (st->clip == 2) {
            int k = ncols;
            while (k > 1) {
                int t = gap * (k - 1);
                for (int c = 0; c < k; c++) t += width[c];
                if (t <= cw) break;
                k--;
            }
            if (k < ncols) {
                for (int c = 0; c < k; c++) if (kind[c] == GT_FR && base[c] < most) base[c] = most;
                ncols = k;
                lay_grid_share(cw, gap, ncols, kind, val, base, width);
            }
        }
    } else {
        lay_grid_share(cw, gap, ncols, kind, val, base, width);
    }
    int colx[LAY_GRID_COLS + 1];
    colx[0] = cx;
    for (int c = 0; c < ncols; c++) colx[c + 1] = colx[c] + width[c] + gap;

    struct { int first, end, h; } row[LAY_GRID_COLS];
    int nrow = 0, col = 0, top = *y, rowh = 0, any = 0;
    for (int k = d->nodes[node].first; ; k = d->nodes[k].next) {
        int last = k < 0;
        int span = 1;
        cstyle own;
        if (!last) {
            if (d->nodes[k].kind != DN_ELEMENT) continue;
            lay_style(L, k, st, &own, cw);
            if (own.display == D_NONE) continue;
            span = own.gspan < 0 ? ncols : own.gspan > 0 ? own.gspan : 1;
            if (span > ncols) span = ncols;
        }
        /* A row is finished by the item that will not fit on it, or by the
           end: every item on it is then lined up against the tallest. */
        if (nrow && (last || col + span > ncols)) {
            for (int i = 0; i < nrow; i++) {
                int dy = st->align_items == AI_CENTER ? (rowh - row[i].h) / 2
                       : st->align_items == AI_END ? rowh - row[i].h : 0;
                if (dy > 0)
                    for (int j = row[i].first; j < row[i].end; j++) L->out->items[j].y += dy;
            }
            top += rowh + gap;
            rowh = 0;
            nrow = 0;
            col = 0;
        }
        if (last) break;
        int x = colx[col];
        int w = colx[col + span] - gap - x;
        int first = L->out->nitems, cy = top;
        L->flex_sized = k;
        lay_block(L, k, st, x, w, &cy);
        L->flex_sized = -1;
        if (nrow < LAY_GRID_COLS) {
            row[nrow].first = first;
            row[nrow].end = L->out->nitems;
            row[nrow].h = cy - top;
            nrow++;
        }
        if (cy - top > rowh) rowh = cy - top;
        col += span;
        any = 1;
    }
    *y = any ? top - gap : top;
}

/* The body: a box laid out where it was told, in the flow. */
static void lay_block_placed(lctx *L, int node, const cstyle *parent, int x,
                             int avail, int *y);

/* --- where a box actually goes -------------------------------------------
 *
 * Three answers, and the difference between them is what half the layouts on
 * the web are built out of.
 *
 * static is the flow, and is everything below this function.
 *
 * relative is the flow, drawn somewhere else. The space it would have taken
 * is still taken -- nothing moves up to fill it -- so this lays it out
 * normally and then shifts the items it produced. That is also why
 * `position: relative` with no offsets is the commonest declaration on the
 * web: it changes nothing about the element and makes it the thing its
 * absolutely positioned children are measured from.
 *
 * absolute is out of the flow entirely: measured from the nearest positioned
 * ancestor, and taking no space where it was written, so whatever follows it
 * closes up as though it were not there.
 *
 * fixed is measured from the page rather than the window, which is not what
 * fixed means -- a fixed banner should stay put while the page scrolls under
 * it, and this one scrolls away with everything else. It is the closer of
 * the two wrong answers: the other is to leave it in the flow, which puts a
 * navigation bar in the middle of the text it was meant to sit above.
 */
/* A transform's translation, done once the box is laid out and its size
 * is known: its items moved together, by pixels and by a share of its own
 * width and height. It is how a dialog is centred (left: 50% and then back
 * by half itself) and how a menu that slides in is kept out of sight
 * (translateX(-100%)); read past, the dialog sat half off its place and the
 * closed menu lay over the page. */
static void lay_translate(lctx *L, int node, const cstyle *st, int first) {
    if (!st->tx_px && !st->ty_px && !st->tx_pct && !st->ty_pct) return;
    int w = 0, h = 0;
    if (first < L->out->nitems && L->out->items[first].node == node
        && L->out->items[first].kind == LK_BOX) {
        w = L->out->items[first].w;
        h = L->out->items[first].h;
    } else {
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
        for (int i = first; i < L->out->nitems; i++) {
            const litem *it = &L->out->items[i];
            if (it->x < x0) x0 = it->x;
            if (it->y < y0) y0 = it->y;
            if (it->x + it->w > x1) x1 = it->x + it->w;
            if (it->y + it->h > y1) y1 = it->y + it->h;
        }
        if (x1 > x0) { w = x1 - x0; h = y1 - y0; }
    }
    int dx = st->tx_px + st->tx_pct * w / 100;
    int dy = st->ty_px + st->ty_pct * h / 100;
    if (!dx && !dy) return;
    for (int i = first; i < L->out->nitems; i++) {
        L->out->items[i].x += dx;
        L->out->items[i].y += dy;
    }
}

static void lay_block(lctx *L, int node, const cstyle *parent, int x,
                      int avail, int *y) {
    L->out->laid++;
    cstyle probe;
    lay_style(L, node, parent, &probe, avail);
    if (probe.display == D_NONE) return;
    if (!probe.visible && !lay_show_hidden) return;
    if (lay_unseen(&probe)) return;
    int moved_from = L->out->nitems;

    if (probe.floated && L->floating != node && probe.position != POS_ABSOLUTE
        && probe.position != POS_FIXED) {
        lay_float(L, node, parent, x, avail, *y);
        return;
    }

    if (probe.position == POS_ABSOLUTE || probe.position == POS_FIXED) {
        int keep = *y;
        int aw = L->pos_w > 16 ? L->pos_w : avail;
        int bw = probe.width >= 0 ? probe.width : aw;

        int ax = *y >= 0 ? x : x;              /* where the flow left it */
        if (probe.left != CSS_AUTO_OFF) ax = L->pos_x + probe.left;
        else if (probe.right_off != CSS_AUTO_OFF)
            ax = L->pos_x + aw - probe.right_off - bw;

        /* A top is an offset from the containing block's top, which is
           known. A bottom is an offset from its bottom, which is not: this
           is one pass and the height of the thing being measured from is
           not settled until everything inside it has been laid out --
           including this box.

           So a box with only a bottom stays where the flow put it. That is
           wrong, and it is the least wrong answer available: computing it
           from the top instead puts a footer pinned to the bottom of the
           page somewhere above the top of it, which is visibly worse than
           leaving it in place. */
        int ay = *y;
        if (probe.top != CSS_AUTO_OFF) ay = L->pos_y + probe.top;

        int room = probe.width >= 0 ? probe.width : (L->pos_x + aw) - ax;
        if (room < 16) room = 16;

        int sub = ay;
        lay_block_placed(L, node, parent, ax, room, &sub);
        *y = keep;                             /* it took no space */
        if (!L->measuring) lay_translate(L, node, &probe, moved_from);
        return;
    }

    if (probe.position == POS_RELATIVE) {
        int first = L->out->nitems;
        lay_block_placed(L, node, parent, x, avail, y);

        int dx = probe.left != CSS_AUTO_OFF ? probe.left
               : (probe.right_off != CSS_AUTO_OFF ? -probe.right_off : 0);
        int dy = probe.top != CSS_AUTO_OFF ? probe.top
               : (probe.bottom != CSS_AUTO_OFF ? -probe.bottom : 0);
        if (dx || dy)
            for (int i = first; i < L->out->nitems; i++) {
                L->out->items[i].x += dx;
                L->out->items[i].y += dy;
            }
        if (!L->measuring) lay_translate(L, node, &probe, moved_from);
        return;
    }

    lay_block_placed(L, node, parent, x, avail, y);
    if (!L->measuring) lay_translate(L, node, &probe, moved_from);
}

static void lay_block_placed(lctx *L, int node, const cstyle *parent, int x,
                             int avail, int *y) {
    const ddoc *d = L->d;
    cstyle st;
    lay_style(L, node, parent, &st, avail);
    if (st.display == D_NONE) return;
    if (!st.visible && !lay_show_hidden) return;

    if (st.clear) *y = lay_cleared(L, *y, st.clear, x, avail);

    int ml = st.ml == CSS_AUTO_OFF ? 0 : st.ml;
    int mr = st.mr == CSS_AUTO_OFF ? 0 : st.mr;
    int box_w = avail - ml - mr;

    /* What a width means.
     *
       With border-box it is the whole box, so the padding and borders come
       out of it; with content-box, the default nobody wants, it is what is
       left after them. A browser that ignores box-sizing lays out every
       page written this decade too wide, and the error compounds at every
       level of nesting because each child is given its parent's wrong
       width to work from. */
    /* box_w below is the border box -- the whole thing, padding and borders
       included -- so border-box needs no adjusting and content-box is the
       one that has to grow by its frame. Written the other way round first,
       which made a content-box div come out exactly its frame too narrow
       and a border-box one exactly its frame too wide. */
    int frame = st.pl + st.pr + st.bl + st.br;
    int want = st.width;
    /* A flex item's width is where its row began, not where it ended: the
       row grew it or shrank it from there and hands over the result as
       avail. Taking the width again undid that -- an item that asked to
       grow stayed its own size, and one shrunk to fit ran over the next --
       and a percentage was a percentage of the share it had been given. */
    if (L->flex_sized == node) {
        want = -1;
        L->flex_sized = -1;
    }
    if (want >= 0 && !st.border_box) want += frame;
    int cap = st.max_width;
    if (cap >= 0 && !st.border_box) cap += frame;
    int floor_w = st.min_width;
    if (floor_w >= 0 && !st.border_box) floor_w += frame;

    /* Centring is where a box sits, not how wide it is, so a box being
       measured is not centred: it reached halfway across whatever it was
       measured in, and that was taken for its width. */
    int centre = st.ml == CSS_AUTO_OFF && st.mr == CSS_AUTO_OFF && !L->measuring;
    /* Laid out, a box is kept to the room it is given, so nothing runs off
       the side of a page that cannot be scrolled sideways. Measured, it is
       as wide as it asks to be, up to the page: the narrowest a card of 280
       pixels can be drawn in is 280, and measured as the room it was given
       it came out as narrow as that, and the grid holding it with it. */
    if (want >= 0 && (want < box_w || (L->measuring && want <= css_view_w))) {
        /* A width with auto margins is centred, which is how most pages put
           their content in the middle of a wide window. */
        if (centre) ml += (box_w - want) / 2;
        box_w = want;
    }
    if (cap >= 0 && cap < box_w) {
        if (centre) ml += (box_w - cap) / 2;
        box_w = cap;
    }
    /* A floor beats a ceiling, which is what every implementation does and
       what a page relies on when it sets both. */
    if (floor_w >= 0 && box_w < floor_w) box_w = floor_w;
    if (box_w < 16) box_w = 16;

    /* A table not given a width is as wide as its columns, so its columns
       are worked out before anything else; and centred the way old markup
       centres one (lay_centres_table). */
    ltable T;
    int is_table = st.display == D_TABLE;
    if (is_table) {
        int fr = st.bl + st.br + st.pl + st.pr;
        lay_table_plan(L, node, &st, box_w - fr, want >= 0, &T);
        if (want < 0 && T.width + fr < box_w) {
            int spare = box_w - (T.width + fr);
            if (!L->measuring && ((st.ml == CSS_AUTO_OFF && st.mr == CSS_AUTO_OFF)
                                  || lay_centres_table(d, node)))
                ml += spare / 2;
            box_w = T.width + fr;
        }
    }

    *y += st.mt == CSS_AUTO_OFF ? 0 : st.mt;
    int box_top = *y;

    /* The background goes in the list before the contents and its size is
       not known until they are laid out, so its slot is taken now and
       filled in at the end. */
    int slot = -1;
    if (st.masked) st.has_bg = 0;
    if (st.has_bg || st.bt || st.br || st.bb || st.bl) {
        litem *bg = lay_item(L);
        if (bg) {
            slot = L->out->nitems - 1;
            bg->kind = LK_BOX;
            bg->node = node;
        }
    }
    int inside_at = L->out->nitems;
    int floats_at = L->fl_seq;
    int held_right = L->measure_right;
    if (L->measuring) L->measure_right = 0;

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

    /* A positioned box is what its positioned descendants are measured
       from. Saved and put back, because this is a walk and the box two
       levels up is still the right answer for the box after this one. */
    int held_x = L->pos_x, held_y = L->pos_y, held_w = L->pos_w;
    if (st.position != POS_STATIC) {
        L->pos_x = cx;
        L->pos_y = box_top;
        L->pos_w = cw;
    }

    /* A flex container lays its children along a line rather than down
       the page, so it does not use the walk below at all. */
    if (st.display == D_FLEX || st.display == D_INLINE_FLEX) {
        lay_flex(L, node, &st, cx, cw, y);
    } else if (st.display == D_GRID) {
        lay_grid(L, node, &st, cx, cw, y);
    } else if (is_table) {
        lay_table_rows(L, node, &st, cx, &T, y);
    } else {

    /* The children: consecutive inline ones share a line, each block one
       starts on its own. */
    int child = d->nodes[node].first;
    int inline_open = 0;
    while (child >= 0) {
        int next = d->nodes[child].next;
        int is_block = lay_is_block_node(L, child, &st);
        if (is_block && (d->nodes[child].tag == T_IMG || lay_control_kind(d, child) != CTL_NONE)) {
            /* A picture or a field made a block is still a picture or a
               field: on a line of its own, drawn by the code that draws
               them, which is the inline code. As a block it had no children
               and came out as nothing. */
            if (inline_open) lay_line_end(L, y);
            lay_line_start(L, *y, cx, cw, st.align);
            lay_inline(L, child, &st, y);
            lay_line_end(L, y);
            inline_open = 0;
        } else if (is_block) {
            if (inline_open) { lay_line_end(L, y); inline_open = 0; }
            lay_block(L, child, &st, cx, cw, y);
        } else {
            if (!inline_open) {
                /* The indent is the first line's only, and a negative one
                   -- -9999px, the way text is hidden behind a logo drawn in
                   its place -- takes the words off the page. */
                lay_line_start(L, *y, cx, cw, st.align);
                if (st.indent != CSS_AUTO_OFF) L->pen += st.indent;
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

    L->pos_x = held_x;
    L->pos_y = held_y;
    L->pos_w = held_w;

    /* Tall enough for the floats inside it (lay_float). */
    for (int s2 = floats_at > L->fl_seq - LAY_FLOATS ? floats_at : L->fl_seq - LAY_FLOATS;
         s2 < L->fl_seq; s2++) {
        int k = s2 % LAY_FLOATS;
        if (s2 < 0) continue;
        if (L->fl_y[k] + L->fl_h[k] > *y) *y = L->fl_y[k] + L->fl_h[k];
    }

    /* Measured, a box that was not given a width is as wide as what is in
       it and its own padding and border, not as wide as the room it was
       measured in: that is what a table column or a flex item or an
       inline-block holding it wants to know. */
    int shrunk = -1;
    if (L->measuring) {
        int right = L->measure_right;
        for (int i = inside_at; i < L->out->nitems; i++) {
            int r = L->out->items[i].x + L->out->items[i].w;
            if (r > right) right = r;
        }
        int edge = want >= 0 || is_table ? x + ml + box_w : right + st.pr + st.br;
        if (edge > x + ml + box_w) edge = x + ml + box_w;
        if (edge < cx) edge = cx + st.pr + st.br;
        shrunk = edge - (x + ml);
        int reach = edge + (st.mr > 0 ? st.mr : 0);
        L->measure_right = held_right > reach ? held_right : reach;
    }

    *y += st.pb + st.bb;
    int box_h = *y - box_top;

    /* The same question as width, and the same answer: with border-box the
       number includes the padding and the borders. */
    /* And the same way round: box_h is the border box too. */
    int vframe = st.pt + st.pb + st.bt + st.bb;
    int want_h = st.height, floor_h = st.min_height, cap_h = st.max_height;
    if (!st.border_box) {
        if (want_h >= 0)  want_h  += vframe;
        if (floor_h >= 0) floor_h += vframe;
        if (cap_h >= 0)   cap_h   += vframe;
    }

    if (want_h >= 0 && want_h > box_h) { *y += want_h - box_h; box_h = want_h; }
    if (floor_h >= 0 && floor_h > box_h) { *y += floor_h - box_h; box_h = floor_h; }

    /* A ceiling on height cuts the box rather than the words in it: nothing
       here clips, so the box stops and whatever was under it moves up. That
       is what max-height does on a page that uses it to cap a banner, and it
       is not what it does on one that uses it with overflow. */
    /* Unless the box hides what overflows it, what is in it is shown whole
       and what follows comes after it. A box that scrolls cannot be
       scrolled here, and one whose overflow shows would have it drawn over
       whatever came next: a sidebar capped at the window's height put an
       encyclopaedia's contents on top of the article's title. Hidden, the
       box is cut and what was below the cut is not drawn. */
    if (cap_h >= 0 && cap_h < box_h && st.clip == 1) {
        int cut = box_top + cap_h;
        for (int i = inside_at; i < L->out->nitems; i++)
            if (L->out->items[i].y >= cut) L->out->items[i].kind = 0;
        *y -= box_h - cap_h;
        box_h = cap_h;
    }

    /* A horizontal rule is a border on a box with nothing in it, and a box
       with nothing in it is no height at all. */
    if (tag == T_HR && box_h <= 0) box_h = st.bt ? st.bt : 1;

#ifdef LAY_TRACE
    /* Every box as it came out, drawn or not: the host's laydump defines
       this to find which box made a page the shape it is. */
    LAY_TRACE(L, node, x + ml, box_top, shrunk >= 0 ? shrunk : box_w, box_h);
#endif

    if (slot >= 0) {
        litem *bg = &L->out->items[slot];
        bg->x = x + ml;
        bg->y = box_top;
        bg->w = shrunk >= 0 ? shrunk : box_w;
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

    *y += st.mb == CSS_AUTO_OFF ? 0 : st.mb;
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
    out->laid = 0;
    out->matched = 0;

    /* A new layout, so no element's rules are known yet. */
    lay_gen++;
    lay_hit_used = 0;

    css_view_w = width;              /* what vw is a hundredth of */

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

    /* With nothing positioned above it, an absolutely positioned box is
       measured from the page. */
    L.pos_x = 0; L.pos_y = 0; L.pos_w = width;
    L.measuring = 0; L.measure_right = 0;
    L.flex_sized = -1;
    L.ngroups = 0;
    L.cont_left = 0; L.cont_width = width;
    L.fl_seq = 0;
    L.floating = -1;

    lay_var_used = 0;
    lay_arena_used = 0;

    /* The <html> element's style, which the body inherits from: :root is
       where a page keeps its custom properties and often its colours and
       base size, and the layout started at the body with the browser's
       defaults as its parent, as though :root said nothing. */
    cstyle base, root;
    css_default_style(&base, root_px);
    if (d->root >= 0 && d->body >= 0) lay_style(&L, d->root, &base, &root, width);
    else lay_cs(&root, &base);
    root.display = D_BLOCK;

    int y = 0;
    int start = d->body >= 0 ? d->body : d->root;
    if (start >= 0) lay_block(&L, start, &root, 0, width, &y);
    out->height = y;
    if (d->overflowed) out->overflowed = 1;
}

/* How many words came out of it. Zero from a document with words in it is
   the page above: hidden by a style its script was going to undo. */
static inline int lay_words(const ldoc *o) {
    int n = 0;
    for (int i = 0; i < o->nitems; i++)
        if (o->items[i].kind == LK_TEXT) n++;
    return n;
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
