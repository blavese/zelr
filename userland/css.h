/* Style sheets: reading them, matching them, and working out what a given
 * element ends up looking like.
 *
 * The browser used to draw a page the way pages were drawn before there was
 * any CSS: headings bigger, links blue, everything in one column. That is a
 * defensible answer for a document, and it is the wrong answer for the web
 * as it is, where the difference between a menu and a list of links, or
 * between a sidebar and the article, exists only in a style sheet. Without
 * one, a page is not simplified. It is read in the wrong order.
 *
 * What is here is the part of CSS that decides what a page looks like:
 * selectors with the three combinators that matter, the cascade in
 * specificity and then source order, inheritance for the properties that
 * inherit, and the box model. What is not here is everything whose absence
 * changes where a box sits rather than whether the page makes sense:
 * floats, positioning, grid, transitions, media queries beyond width.
 *
 * Nothing is allocated, here or anywhere in this program.
 */
#pragma once
#include "zelr.h"
#include "web.h"
#include "dom.h"

#define CSS_RULES   4000
#define CSS_SELS    9000
#define CSS_DECLS   16000
#define CSS_TEXT    (160 * 1024)
#define CSS_MAXCLS  4

/* --- what can be said about a box ---------------------------------------- */

enum {
    P_NONE = 0,
    P_COLOR, P_BACKGROUND, P_DISPLAY, P_FONT_SIZE, P_FONT_WEIGHT,
    P_FONT_STYLE, P_FONT_FAMILY, P_TEXT_ALIGN, P_TEXT_DECORATION,
    P_MARGIN_T, P_MARGIN_R, P_MARGIN_B, P_MARGIN_L,
    P_PADDING_T, P_PADDING_R, P_PADDING_B, P_PADDING_L,
    P_BORDER_T, P_BORDER_R, P_BORDER_B, P_BORDER_L, P_BORDER_COLOR,
    P_WIDTH, P_MAX_WIDTH, P_HEIGHT, P_LINE_HEIGHT, P_WHITE_SPACE,
    P_LIST_STYLE, P_RADIUS, P_TEXT_INDENT, P_VISIBILITY, P_OPACITY,
    P_FLEX_DIR, P_JUSTIFY, P_ALIGN_ITEMS, P_FLEX_WRAP, P_GAP, P_FLEX_GROW,
    P_MIN_WIDTH, P_MIN_HEIGHT, P_MAX_HEIGHT, P_BOX_SIZING,
    P_POSITION, P_TOP, P_RIGHT, P_BOTTOM, P_LEFT,
    P_COUNT
};

/* Where an element sits.

   STATIC is the flow. RELATIVE is the flow, drawn somewhere else, and
   the space it would have taken is still taken. ABSOLUTE and FIXED are
   out of the flow entirely, measured from an ancestor and from the
   window respectively, and take no space at all. */
enum { POS_STATIC = 0, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED };

/* What an offset of `auto` is, which is not the same as zero: an
   absolute box with no top and no bottom stays where the flow put it. */
#define CSS_AUTO_OFF ((short)-32768)

enum { D_INLINE = 0, D_BLOCK, D_INLINE_BLOCK, D_LIST_ITEM, D_NONE,
       D_TABLE_CELL, D_FLEX };

/* A flex container's own settings, and a flex item's one of them.
 *
 * display:flex used to fall through to block, which is not a small
 * difference: every row on every modern page came out as a column. A menu
 * across the top of a site became the menu down the side of nothing, and no
 * amount of getting the pictures right was going to fix it. */
enum { FD_ROW = 0, FD_ROW_REVERSE, FD_COLUMN, FD_COLUMN_REVERSE };
enum { JC_START = 0, JC_CENTER, JC_END, JC_BETWEEN, JC_AROUND, JC_EVENLY };
enum { AI_STRETCH = 0, AI_START, AI_CENTER, AI_END, AI_BASELINE };
enum { A_LEFT = 0, A_CENTER, A_RIGHT, A_JUSTIFY };
enum { WS_NORMAL = 0, WS_PRE, WS_NOWRAP };
enum { LS_DISC = 0, LS_DECIMAL, LS_NONE, LS_CIRCLE, LS_SQUARE };

/* A length, kept as it was written and resolved when the thing it is
   relative to is known. Resolving at parse time means guessing the font size
   and the containing width, and a guess that is wrong is a layout that is
   wrong everywhere the guess was used. */
enum { U_PX = 0, U_EM, U_REM, U_PCT, U_AUTO };

typedef struct {
    short v;                  /* in the unit's own terms, times 1 */
    unsigned char unit;
} clen;

typedef struct {
    u32 color, background, border_color;
    short font_px;
    unsigned char bold, italic, mono;
    unsigned char underline, strike;
    unsigned char display, align, white, list, visible;
    unsigned char has_bg;
    short mt, mr, mb, ml;
    short pt, pr, pb, pl;
    short bt, br, bb, bl;
    short width, max_width, height;      /* -1 for auto */
    short min_width, min_height, max_height;

    /* Whether width means the content box or the whole box.

       border-box is what nearly every page written this decade sets on
       everything, because content-box makes a box with padding wider
       than the number you asked for. A browser that ignores it lays
       every such page out too wide, compounding at every nesting. */
    unsigned char border_box;

    /* Taken out of the flow, or offset from where it would have been.
       See lay_positioned in layout.h. */
    unsigned char position;
    short top, right_off, bottom, left;  /* -32768 for auto */
    short line_h;                        /* per cent of the font size */
    short radius, indent;

    /* Set on a flex container, and read by its children's layout rather
       than by their own style. */
    unsigned char flex_dir, justify, align_items, flex_wrap;
    short gap;
    short grow;                          /* this element's own flex-grow */
} cstyle;

/* --- the text of a sheet -------------------------------------------------
 *
 * Every string a sheet needs lives in one arena, so a rule is a handful of
 * ints and nothing has to be freed. */

typedef struct {
    short tag;                /* a T_* tag, or -1 for any */
    int   id;                 /* into the text, or -1 */
    int   cls[CSS_MAXCLS];
    short ncls;
    short combinator;         /* how it joins the part to its left */
    short pseudo;
} csel;

enum { CB_FIRST = 0, CB_DESC, CB_CHILD };
enum { PS_NONE = 0, PS_HOVER, PS_LINK, PS_VISITED, PS_FIRST_CHILD, PS_ROOT };

typedef struct {
    short prop;
    int   value;              /* into the text */
} cdecl;

typedef struct {
    int sel_at, sel_n;        /* the compound parts, leftmost first */
    int decl_at, decl_n;
    int spec;                 /* ids, then classes, then element names */
    int order;
} crule;

typedef struct {
    crule rules[CSS_RULES];
    int   nrules;
    csel  sels[CSS_SELS];
    int   nsels;
    cdecl decls[CSS_DECLS];
    int   ndecls;
    char  text[CSS_TEXT];
    int   used;
    int   overflowed;
} csheet;

/* Where an element's own style attribute landed.
 *
 * Parsed once when the page is read rather than every time it is laid out,
 * because laying out happens again on every resize and parsing the same
 * twenty declarations on every one of them is the kind of work that makes
 * dragging a window edge feel broken. Indexed by DOM node. */
typedef struct {
    int at, n;
} cinline;

static inline int css_put(csheet *s, const char *p, int len) {
    int at = s->used;
    if (s->used + len + 1 >= CSS_TEXT) { s->overflowed = 1; return -1; }
    for (int i = 0; i < len; i++) s->text[s->used++] = p[i];
    s->text[s->used++] = 0;
    return at;
}

static inline int css_put_lower(csheet *s, const char *p, int len) {
    int at = s->used;
    if (s->used + len + 1 >= CSS_TEXT) { s->overflowed = 1; return -1; }
    for (int i = 0; i < len; i++) s->text[s->used++] = w_lower(p[i]);
    s->text[s->used++] = 0;
    return at;
}

/* --- colours -------------------------------------------------------------
 *
 * Hex in three or six digits, the rgb and rgba functions, and the named
 * colours that turn up in real pages. Transparency is composited against
 * white rather than kept, because there is no compositing in the layout
 * below and a half transparent background drawn opaque is closer to right
 * than one drawn solid. */

typedef struct { const char *name; u32 rgb; } cnamed;

static const cnamed CSS_NAMES[] = {
    { "black", 0x000000 },   { "white", 0xFFFFFF },  { "red", 0xFF0000 },
    { "green", 0x008000 },   { "blue", 0x0000FF },   { "gray", 0x808080 },
    { "grey", 0x808080 },    { "silver", 0xC0C0C0 }, { "maroon", 0x800000 },
    { "yellow", 0xFFFF00 },  { "olive", 0x808000 },  { "lime", 0x00FF00 },
    { "aqua", 0x00FFFF },    { "cyan", 0x00FFFF },   { "teal", 0x008080 },
    { "navy", 0x000080 },    { "fuchsia", 0xFF00FF },{ "magenta", 0xFF00FF },
    { "purple", 0x800080 },  { "orange", 0xFFA500 },
    { "transparent", 0xFFFFFF },
    { "lightgray", 0xD3D3D3 },   { "lightgrey", 0xD3D3D3 },
    { "darkgray", 0xA9A9A9 },    { "darkgrey", 0xA9A9A9 },
    { "whitesmoke", 0xF5F5F5 },  { "gainsboro", 0xDCDCDC },
    { "dimgray", 0x696969 },     { "dimgrey", 0x696969 },
    { "darkblue", 0x00008B },    { "darkred", 0x8B0000 },
    { "darkgreen", 0x006400 },   { "steelblue", 0x4682B4 },
    { "royalblue", 0x4169E1 },   { "dodgerblue", 0x1E90FF },
    { "crimson", 0xDC143C },     { "tomato", 0xFF6347 },
    { "gold", 0xFFD700 },        { "beige", 0xF5F5DC },
    { "ivory", 0xFFFFF0 },       { "linen", 0xFAF0E6 },
    { "salmon", 0xFA8072 },      { "khaki", 0xF0E68C },
    { "indigo", 0x4B0082 },      { "violet", 0xEE82EE },
    { "pink", 0xFFC0CB },        { "brown", 0xA52A0A },
    { "coral", 0xFF7F50 },       { "turquoise", 0x40E0D0 },
    { "slategray", 0x708090 },   { "slategrey", 0x708090 },
    { "midnightblue", 0x191970 },{ "rebeccapurple", 0x663399 },
    { 0, 0 }
};

static inline int css_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Returns 1 and writes the colour, or 0 when the text is not one. */
static inline int css_color(const char *s, u32 *out) {
    while (*s == ' ') s++;
    if (*s == '#') {
        s++;
        int d[8], n = 0;
        while (n < 8) {
            int v = css_hex(s[n]);
            if (v < 0) break;
            d[n++] = v;
        }
        if (n >= 6) {
            *out = (u32)((d[0] << 20) | (d[1] << 16) | (d[2] << 12)
                       | (d[3] << 8) | (d[4] << 4) | d[5]);
            return 1;
        }
        if (n >= 3) {
            *out = (u32)((d[0] << 20) | (d[0] << 16) | (d[1] << 12)
                       | (d[1] << 8) | (d[2] << 4) | d[2]);
            return 1;
        }
        return 0;
    }
    if (w_starts_fold(s, "rgb")) {
        while (*s && *s != '(') s++;
        if (*s) s++;
        int c[4] = { 0, 0, 0, 255 }, n = 0;
        while (*s && *s != ')' && n < 4) {
            while (*s == ' ' || *s == ',') s++;
            int v = 0, any = 0, frac = 0, fdiv = 1;
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
            if (*s == '.') {
                s++;
                while (*s >= '0' && *s <= '9') {
                    if (fdiv < 1000) { frac = frac * 10 + (*s - '0'); fdiv *= 10; }
                    s++;
                }
            }
            if (*s == '%') { v = v * 255 / 100; s++; }
            else if (n == 3) v = v * 255 + frac * 255 / fdiv;   /* 0..1 alpha */
            if (!any) break;
            c[n++] = v;
            while (*s == ' ' || *s == '/' ) s++;
        }
        if (n < 3) return 0;
        int a = c[3] < 0 ? 0 : (c[3] > 255 ? 255 : c[3]);
        /* Over white, because nothing below composites. */
        int r = (c[0] * a + 255 * (255 - a)) / 255;
        int g = (c[1] * a + 255 * (255 - a)) / 255;
        int b = (c[2] * a + 255 * (255 - a)) / 255;
        *out = (u32)((r << 16) | (g << 8) | b);
        return 1;
    }
    for (int i = 0; CSS_NAMES[i].name; i++)
        if (w_same_fold(s, CSS_NAMES[i].name)) { *out = CSS_NAMES[i].rgb; return 1; }
    return 0;
}

/* --- lengths ------------------------------------------------------------- */

static inline clen css_len(const char *s) {
    clen L; L.v = 0; L.unit = U_PX;
    while (*s == ' ') s++;
    if (w_starts_fold(s, "auto")) { L.unit = U_AUTO; return L; }
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    int v = 0, any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    int frac = 0;
    if (*s == '.') {
        s++;
        int scale = 10;
        while (*s >= '0' && *s <= '9') {
            if (scale <= 100) frac += (*s - '0') * (100 / scale);
            scale *= 10;
            s++;
        }
        any = 1;
    }
    if (!any) { L.unit = U_AUTO; return L; }
    /* Hundredths, so half an em survives. */
    L.v = (short)(sign * (v * 100 + frac));
    if (w_starts_fold(s, "em")) L.unit = U_EM;
    else if (w_starts_fold(s, "rem")) L.unit = U_REM;
    else if (*s == '%') L.unit = U_PCT;
    else if (w_starts_fold(s, "pt")) { L.v = (short)(L.v * 4 / 3); L.unit = U_PX; }
    else L.unit = U_PX;
    return L;
}

/* px, given what the relative units are relative to. */
static inline int css_px(clen L, int font_px, int root_px, int pct_of) {
    switch (L.unit) {
        case U_EM:  return L.v * font_px / 100;
        case U_REM: return L.v * root_px / 100;
        case U_PCT: return pct_of >= 0 ? L.v * pct_of / 10000 : 0;
        case U_AUTO: return -1;
        default:    return L.v / 100;
    }
}

/* --- property names ------------------------------------------------------ */

typedef struct { const char *name; short prop; } cprop;

static const cprop CSS_PROPS[] = {
    { "color", P_COLOR },
    { "background-color", P_BACKGROUND },
    { "background", P_BACKGROUND },
    { "display", P_DISPLAY },
    { "flex-direction", P_FLEX_DIR },
    { "justify-content", P_JUSTIFY },
    { "align-items", P_ALIGN_ITEMS },
    { "flex-wrap", P_FLEX_WRAP },
    { "gap", P_GAP },
    { "column-gap", P_GAP },
    { "row-gap", P_GAP },
    { "flex-grow", P_FLEX_GROW },
    { "flex", P_FLEX_GROW },
    { "font-size", P_FONT_SIZE },
    { "font-weight", P_FONT_WEIGHT },
    { "font-style", P_FONT_STYLE },
    { "font-family", P_FONT_FAMILY },
    { "text-align", P_TEXT_ALIGN },
    { "text-decoration", P_TEXT_DECORATION },
    { "text-decoration-line", P_TEXT_DECORATION },
    { "margin-top", P_MARGIN_T },     { "margin-right", P_MARGIN_R },
    { "margin-bottom", P_MARGIN_B },  { "margin-left", P_MARGIN_L },
    { "padding-top", P_PADDING_T },   { "padding-right", P_PADDING_R },
    { "padding-bottom", P_PADDING_B },{ "padding-left", P_PADDING_L },
    { "border-top-width", P_BORDER_T },
    { "border-right-width", P_BORDER_R },
    { "border-bottom-width", P_BORDER_B },
    { "border-left-width", P_BORDER_L },
    { "border-color", P_BORDER_COLOR },
    { "width", P_WIDTH },
    { "min-width", P_MIN_WIDTH },
    { "min-height", P_MIN_HEIGHT },
    { "max-height", P_MAX_HEIGHT },
    { "box-sizing", P_BOX_SIZING },
    { "position", P_POSITION },
    { "top", P_TOP },
    { "right", P_RIGHT },
    { "bottom", P_BOTTOM },
    { "left", P_LEFT },
    { "max-width", P_MAX_WIDTH },
    { "height", P_HEIGHT },
    { "line-height", P_LINE_HEIGHT },
    { "white-space", P_WHITE_SPACE },
    { "list-style-type", P_LIST_STYLE },
    { "border-radius", P_RADIUS },
    { "text-indent", P_TEXT_INDENT },
    { "visibility", P_VISIBILITY },
    { "opacity", P_OPACITY },
    { 0, 0 }
};

static inline int css_prop_of(const char *name) {
    for (int i = 0; CSS_PROPS[i].name; i++)
        if (w_same(name, CSS_PROPS[i].name)) return CSS_PROPS[i].prop;
    return P_NONE;
}

/* --- reading a sheet ----------------------------------------------------- */

static inline int css_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static inline int css_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static inline void css_init(csheet *s) {
    s->nrules = s->nsels = s->ndecls = s->used = 0;
    s->overflowed = 0;
}

/* Skips comments and whitespace together, because between two selectors
   they mean the same thing and a comment in the middle of a selector list
   is otherwise read as a descendant combinator. */
static inline int css_skip(const char *p, int len, int i) {
    for (;;) {
        while (i < len && css_space(p[i])) i++;
        if (i + 1 < len && p[i] == '/' && p[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(p[i] == '*' && p[i + 1] == '/')) i++;
            i = i + 2 < len ? i + 2 : len;
            continue;
        }
        return i;
    }
}

/* One selector, up to a comma or the brace. Returns how many compound parts
   it had, or 0 when it is something this does not understand, in which case
   the rule is dropped rather than applied to the wrong elements. */
static inline int css_parse_selector(csheet *s, const char *p, int len,
                                     int *at, int *spec_out) {
    int i = *at, n = 0, spec = 0;
    int combinator = CB_FIRST;
    int started = 0;

    for (;;) {
        i = css_skip(p, len, i);
        if (i >= len || p[i] == ',' || p[i] == '{') break;

        if (p[i] == '>') { combinator = CB_CHILD; i++; started = 1; continue; }
        if (p[i] == '+' || p[i] == '~') {
            /* Sibling combinators are not matched here, and a rule that uses
               one is dropped rather than applied as a descendant, which is
               how a style meant for the element after a heading ends up on
               everything inside one. */
            while (i < len && p[i] != ',' && p[i] != '{') i++;
            *at = i;
            return 0;
        }

        if (s->nsels >= CSS_SELS) { s->overflowed = 1; return 0; }
        csel *c = &s->sels[s->nsels];
        c->tag = -1; c->id = -1; c->ncls = 0; c->pseudo = PS_NONE;
        c->combinator = (short)(n == 0 ? CB_FIRST : combinator);
        combinator = CB_DESC;

        int any = 0;
        for (;;) {
            if (i >= len) break;
            char ch = p[i];
            if (ch == '*') { i++; any = 1; continue; }
            if (ch == '#' || ch == '.') {
                i++;
                int st = i;
                while (i < len && css_ident(p[i])) i++;
                if (i == st) break;
                int t = css_put(s, p + st, i - st);
                if (ch == '#') { c->id = t; spec += 1 << 20; }
                else if (c->ncls < CSS_MAXCLS) {
                    c->cls[c->ncls++] = t;
                    spec += 1 << 10;
                }
                any = 1;
                continue;
            }
            if (ch == ':') {
                i++;
                if (i < len && p[i] == ':') i++;        /* an element, not a
                                                           class: ignored */
                int st = i;
                while (i < len && css_ident(p[i])) i++;
                int l = i - st;
                if (l == 5 && w_lower(p[st]) == 'h') c->pseudo = PS_HOVER;
                else if (l == 4 && w_lower(p[st]) == 'l') c->pseudo = PS_LINK;
                else if (l == 7 && w_lower(p[st]) == 'v') c->pseudo = PS_VISITED;
                else if (l == 4 && w_lower(p[st]) == 'r') c->pseudo = PS_ROOT;
                else if (l == 11) c->pseudo = PS_FIRST_CHILD;
                /* Anything with an argument is more than this understands,
                   and a rule that is guessed at is worse than one dropped. */
                if (i < len && p[i] == '(') {
                    int d = 0;
                    while (i < len) {
                        if (p[i] == '(') d++;
                        else if (p[i] == ')') { d--; i++; if (!d) break; continue; }
                        i++;
                    }
                    while (i < len && p[i] != ',' && p[i] != '{') i++;
                    *at = i;
                    return 0;
                }
                spec += 1 << 10;
                any = 1;
                continue;
            }
            if (ch == '[') {
                /* Attribute selectors: presence only, which is the common
                   case, and the rest are dropped. */
                int st = ++i;
                while (i < len && p[i] != ']' && p[i] != '=' ) i++;
                if (i < len && p[i] == '=') {
                    while (i < len && p[i] != ']') i++;
                    if (i < len) i++;
                    while (i < len && p[i] != ',' && p[i] != '{') i++;
                    *at = i;
                    return 0;
                }
                int t = css_put(s, p + st, i - st);
                if (c->ncls < CSS_MAXCLS) {
                    /* Held as a class with a marker, so matching has one
                       loop rather than two lists. */
                    c->cls[c->ncls++] = -t - 2;
                    spec += 1 << 10;
                }
                if (i < len) i++;
                any = 1;
                continue;
            }
            if (css_ident(ch)) {
                int st = i;
                while (i < len && css_ident(p[i])) i++;
                int tg = html_tag_of(p + st, i - st);
                c->tag = (short)(tg == T_OTHER ? -1 : tg);
                if (tg != T_OTHER) spec += 1;
                else {
                    /* An element this does not know about cannot be matched
                       by name, and matching everything instead is worse. */
                    while (i < len && p[i] != ',' && p[i] != '{') i++;
                    *at = i;
                    return 0;
                }
                any = 1;
                continue;
            }
            break;
        }

        if (!any) break;
        s->nsels++;
        n++;
        started = 1;
    }

    (void)started;
    *at = i;
    *spec_out = spec;
    return n;
}

/* The declarations between the braces. */
static inline void css_parse_block(csheet *s, const char *p, int len, int *at,
                                   int *decl_at, int *decl_n);

/* Expands the shorthands that matter, because a page that writes
   `margin: 0 auto` and nothing else is a page whose whole layout is in a
   shorthand. */
static inline void css_add(csheet *s, int prop, const char *v, int vlen) {
    if (s->ndecls >= CSS_DECLS) { s->overflowed = 1; return; }
    int t = css_put(s, v, vlen);
    if (t < 0) return;
    s->decls[s->ndecls].prop = (short)prop;
    s->decls[s->ndecls].value = t;
    s->ndecls++;
}

/* Splits a value into up to four space separated pieces. */
static inline int css_parts(const char *v, int vlen, int *st, int *ln, int max) {
    int n = 0, i = 0;
    while (i < vlen && n < max) {
        while (i < vlen && css_space(v[i])) i++;
        if (i >= vlen) break;
        int a = i, depth = 0;
        while (i < vlen && (depth || !css_space(v[i]))) {
            if (v[i] == '(') depth++;
            else if (v[i] == ')') depth--;
            i++;
        }
        st[n] = a; ln[n] = i - a; n++;
    }
    return n;
}

static inline void css_shorthand4(csheet *s, const char *v, int vlen,
                                  int pt, int pr, int pb, int pl) {
    int st[4], ln[4];
    int n = css_parts(v, vlen, st, ln, 4);
    if (n == 0) return;
    int top = 0, right = 0, bottom = 0, left = 0;
    if (n == 1) { top = right = bottom = left = 0; }
    else if (n == 2) { top = bottom = 0; right = left = 1; }
    else if (n == 3) { top = 0; right = left = 1; bottom = 2; }
    else { top = 0; right = 1; bottom = 2; left = 3; }
    css_add(s, pt, v + st[top], ln[top]);
    css_add(s, pr, v + st[right], ln[right]);
    css_add(s, pb, v + st[bottom], ln[bottom]);
    css_add(s, pl, v + st[left], ln[left]);
}

static inline void css_declare(csheet *s, const char *name, int nlen,
                               const char *v, int vlen) {
    /* Trim, so a value with a trailing !important or space compares. */
    while (vlen > 0 && css_space(v[vlen - 1])) vlen--;
    if (vlen > 10) {
        int k = vlen - 10;
        if (v[k] == '!' || (k > 0 && v[k - 1] == '!')) {
            int b = vlen;
            while (b > 0 && v[b - 1] != '!') b--;
            if (b > 0 && w_starts_fold(v + b, "important")) {
                vlen = b - 1;
                while (vlen > 0 && css_space(v[vlen - 1])) vlen--;
            }
        }
    }
    if (vlen <= 0) return;

    char lower[40];
    int n = nlen < 39 ? nlen : 39;
    for (int i = 0; i < n; i++) lower[i] = w_lower(name[i]);
    lower[n] = 0;

    if (w_same(lower, "margin")) {
        css_shorthand4(s, v, vlen, P_MARGIN_T, P_MARGIN_R, P_MARGIN_B, P_MARGIN_L);
        return;
    }
    if (w_same(lower, "padding")) {
        css_shorthand4(s, v, vlen, P_PADDING_T, P_PADDING_R, P_PADDING_B, P_PADDING_L);
        return;
    }
    if (w_same(lower, "border") || w_same(lower, "border-width")) {
        /* A border is a width, a style and a colour in any order. The style
           is not drawn, so what is taken is the width and the colour, and a
           style of none means there is no border whatever the width says. */
        int st[4], ln[4];
        int np = css_parts(v, vlen, st, ln, 4);
        int wpx = -1;
        u32 col = 0;
        int have_col = 0, none = 0;
        for (int i = 0; i < np; i++) {
            const char *pp = v + st[i];
            if (w_starts_fold(pp, "none") || w_starts_fold(pp, "hidden")) none = 1;
            else if (css_color(pp, &col)) have_col = 1;
            else {
                clen L = css_len(pp);
                if (L.unit != U_AUTO) wpx = L.v / 100;
                else if (w_starts_fold(pp, "thin")) wpx = 1;
                else if (w_starts_fold(pp, "medium")) wpx = 2;
                else if (w_starts_fold(pp, "thick")) wpx = 4;
            }
        }
        if (none) wpx = 0;
        else if (wpx < 0) wpx = 1;                /* a border with no width */
        char num[8];
        int w = 0, q = wpx;
        if (q == 0) num[w++] = '0';
        else { char tmp[8]; int t = 0; while (q) { tmp[t++] = (char)('0' + q % 10); q /= 10; }
               while (t) num[w++] = tmp[--t]; }
        num[w] = 0;
        css_add(s, P_BORDER_T, num, w);
        css_add(s, P_BORDER_R, num, w);
        css_add(s, P_BORDER_B, num, w);
        css_add(s, P_BORDER_L, num, w);
        if (have_col) css_add(s, P_BORDER_COLOR, v, vlen);
        return;
    }
    if (w_same(lower, "border-top")) { css_add(s, P_BORDER_T, v, vlen); return; }
    if (w_same(lower, "border-bottom")) { css_add(s, P_BORDER_B, v, vlen); return; }
    if (w_same(lower, "border-left")) { css_add(s, P_BORDER_L, v, vlen); return; }
    if (w_same(lower, "border-right")) { css_add(s, P_BORDER_R, v, vlen); return; }
    if (w_same(lower, "font")) {
        /* The shorthand's one part worth having is the size. */
        int st[6], ln[6];
        int np = css_parts(v, vlen, st, ln, 6);
        for (int i = 0; i < np; i++) {
            clen L = css_len(v + st[i]);
            if (L.unit != U_AUTO && L.v > 0) {
                css_add(s, P_FONT_SIZE, v + st[i], ln[i]);
                break;
            }
            if (w_starts_fold(v + st[i], "bold")) css_add(s, P_FONT_WEIGHT, "bold", 4);
            if (w_starts_fold(v + st[i], "italic")) css_add(s, P_FONT_STYLE, "italic", 6);
        }
        return;
    }
    if (w_same(lower, "list-style")) {
        css_add(s, P_LIST_STYLE, v, vlen);
        return;
    }

    int prop = css_prop_of(lower);
    if (prop == P_NONE) return;

    /* `background` is a whole pile of things of which only a colour is
       drawn here. A gradient or an image is not one, and taking the first
       word of it as a colour paints panels black. */
    if (prop == P_BACKGROUND && !w_same(lower, "background-color")) {
        u32 c;
        int st[6], ln[6];
        int np = css_parts(v, vlen, st, ln, 6);
        int found = -1;
        for (int i = 0; i < np; i++)
            if (css_color(v + st[i], &c)) { found = i; break; }
        if (found < 0) return;
        css_add(s, P_BACKGROUND, v + st[found], ln[found]);
        return;
    }

    css_add(s, prop, v, vlen);
}

static inline void css_parse_block(csheet *s, const char *p, int len, int *at,
                                   int *decl_at, int *decl_n) {
    int i = *at;
    *decl_at = s->ndecls;
    while (i < len && p[i] != '{') i++;
    if (i < len) i++;
    for (;;) {
        i = css_skip(p, len, i);
        if (i >= len || p[i] == '}') { if (i < len) i++; break; }
        int nstart = i;
        while (i < len && p[i] != ':' && p[i] != ';' && p[i] != '}') i++;
        int nlen = i - nstart;
        while (nlen > 0 && css_space(p[nstart + nlen - 1])) nlen--;
        if (i >= len || p[i] != ':') {
            while (i < len && p[i] != ';' && p[i] != '}') i++;
            if (i < len && p[i] == ';') i++;
            continue;
        }
        i++;
        int vstart = i, depth = 0;
        while (i < len) {
            if (p[i] == '(') depth++;
            else if (p[i] == ')') { if (depth) depth--; }
            else if (!depth && (p[i] == ';' || p[i] == '}')) break;
            i++;
        }
        css_declare(s, p + nstart, nlen, p + vstart, i - vstart);
        if (i < len && p[i] == ';') i++;
    }
    *decl_n = s->ndecls - *decl_at;
    *at = i;
}

/* Skips an at-rule. Media queries are opened rather than skipped when they
   are the kind that applies to a screen, because most of a modern page's
   layout is inside one and dropping them drops the page. */
static inline int css_at_rule(const char *p, int len, int i, int *open_body) {
    int start = i;
    i++;
    int nstart = i;
    while (i < len && css_ident(p[i])) i++;
    int nlen = i - nstart;
    int media = (nlen == 5 && w_lower(p[nstart]) == 'm');
    int supports = (nlen == 8 && w_lower(p[nstart]) == 's');

    int qstart = i;
    while (i < len && p[i] != '{' && p[i] != ';') i++;
    if (i < len && p[i] == ';') { *open_body = 0; return i + 1; }

    if (media || supports) {
        /* A query for print is not for this screen; one for a minimum width
           this window may not have is still opened, because a page whose
           desktop layout is behind min-width and whose mobile layout is the
           default reads better wide than narrow. What is refused is print. */
        int is_print = 0;
        for (int k = qstart; k + 4 < i; k++)
            if (w_lower(p[k]) == 'p' && w_lower(p[k + 1]) == 'r'
                && w_lower(p[k + 2]) == 'i' && w_lower(p[k + 3]) == 'n'
                && w_lower(p[k + 4]) == 't') { is_print = 1; break; }
        if (!is_print) { *open_body = 1; return i + 1; }
    }

    /* Everything else with a body is skipped whole: keyframes, font faces,
       and the rest, none of which this draws. */
    int depth = 0;
    i = start;
    while (i < len) {
        if (p[i] == '{') depth++;
        else if (p[i] == '}') { depth--; i++; if (depth <= 0) break; continue; }
        i++;
    }
    *open_body = 0;
    return i;
}

static inline void css_parse(csheet *s, const char *p, int len) {
    int i = 0;
    int nested = 0;

    while (i < len) {
        i = css_skip(p, len, i);
        if (i >= len) break;

        if (p[i] == '}') { if (nested > 0) nested--; i++; continue; }
        if (p[i] == '@') {
            int open_body = 0;
            i = css_at_rule(p, len, i, &open_body);
            if (open_body) nested++;
            continue;
        }

        /* A selector list, then one block shared by all of them. */
        int first_rule = s->nrules;
        int sel_starts[32], sel_counts[32], specs[32], nsel_lists = 0;
        for (;;) {
            int spec = 0;
            int start = s->nsels;
            int n = css_parse_selector(s, p, len, &i, &spec);
            if (n > 0 && nsel_lists < 32) {
                sel_starts[nsel_lists] = start;
                sel_counts[nsel_lists] = n;
                specs[nsel_lists] = spec;
                nsel_lists++;
            } else if (n == 0) {
                s->nsels = start;         /* a selector this cannot match */
            }
            i = css_skip(p, len, i);
            if (i < len && p[i] == ',') { i++; continue; }
            break;
        }

        if (i >= len || p[i] != '{') {
            while (i < len && p[i] != '{' && p[i] != '}') i++;
            if (i < len && p[i] == '{') { int d0 = 0, d1 = 0; css_parse_block(s, p, len, &i, &d0, &d1); }
            continue;
        }

        int decl_at = 0, decl_n = 0;
        css_parse_block(s, p, len, &i, &decl_at, &decl_n);
        if (decl_n <= 0) continue;

        for (int k = 0; k < nsel_lists; k++) {
            if (s->nrules >= CSS_RULES) { s->overflowed = 1; return; }
            crule *r = &s->rules[s->nrules];
            r->sel_at = sel_starts[k];
            r->sel_n = sel_counts[k];
            r->decl_at = decl_at;
            r->decl_n = decl_n;
            r->spec = specs[k];
            r->order = s->nrules;
            s->nrules++;
        }
        (void)first_rule;
    }
}

/* --- matching ------------------------------------------------------------
 *
 * One compound part against one element, then the parts right to left up the
 * tree. Right to left because the rightmost part fails for almost every
 * element and the walk then stops immediately, where left to right would
 * search the subtree of every ancestor that matched. */

typedef struct {
    int hover;                /* the element the pointer is over, or -1 */
    int visited_links;        /* whether :visited should match at all */
} cmatch;

static inline int css_part_matches(const csheet *s, const ddoc *d, int el,
                                   const csel *c, const cmatch *m) {
    const dnode *n = &d->nodes[el];
    if (n->kind != DN_ELEMENT) return 0;
    if (c->tag >= 0 && n->tag != c->tag) return 0;
    if (c->id >= 0) {
        const char *v = dom_attr(d, el, "id");
        if (!v || !w_same(v, s->text + c->id)) return 0;
    }
    for (int i = 0; i < c->ncls; i++) {
        int t = c->cls[i];
        if (t < 0) {
            if (!dom_attr(d, el, s->text + (-t - 2))) return 0;
        } else {
            const char *want = s->text + t;
            if (!dom_has_class(d, el, want, w_len(want))) return 0;
        }
    }
    switch (c->pseudo) {
        case PS_HOVER: if (!m || m->hover != el) return 0; break;
        case PS_LINK:
        case PS_VISITED: if (n->tag != T_A || !dom_attr(d, el, "href")) return 0;
                         break;
        case PS_FIRST_CHILD: {
            int p = n->parent;
            if (p < 0) return 0;
            int f = d->nodes[p].first;
            while (f >= 0 && d->nodes[f].kind != DN_ELEMENT) f = d->nodes[f].next;
            if (f != el) return 0;
            break;
        }
        case PS_ROOT: if (el != d->root) return 0; break;
        default: break;
    }
    return 1;
}

static inline int css_matches(const csheet *s, const ddoc *d, int el,
                              const crule *r, const cmatch *m) {
    int k = r->sel_n - 1;
    const csel *c = &s->sels[r->sel_at + k];
    if (!css_part_matches(s, d, el, c, m)) return 0;

    int at = el;
    for (k--; k >= 0; k--) {
        const csel *want = &s->sels[r->sel_at + k];
        int join = s->sels[r->sel_at + k + 1].combinator;
        if (join == CB_CHILD) {
            at = d->nodes[at].parent;
            if (at < 0 || !css_part_matches(s, d, at, want, m)) return 0;
        } else {
            int p = d->nodes[at].parent;
            while (p >= 0 && !css_part_matches(s, d, p, want, m))
                p = d->nodes[p].parent;
            if (p < 0) return 0;
            at = p;
        }
    }
    return 1;
}

/* --- applying ------------------------------------------------------------ */

static inline void css_default_style(cstyle *st, int root_px) {
    st->color = 0x1A1A1A;
    st->background = 0xFFFFFF;
    st->border_color = 0xD0D0D0;
    st->font_px = (short)root_px;
    st->bold = st->italic = st->mono = 0;
    st->underline = st->strike = 0;
    st->display = D_INLINE;
    st->flex_dir = FD_ROW;
    st->justify = JC_START;
    st->align_items = AI_STRETCH;
    st->flex_wrap = 0;
    st->gap = 0;
    st->grow = 0;
    st->align = A_LEFT;
    st->white = WS_NORMAL;
    st->list = LS_DISC;
    st->visible = 1;
    st->has_bg = 0;
    st->mt = st->mr = st->mb = st->ml = 0;
    st->pt = st->pr = st->pb = st->pl = 0;
    st->bt = st->br = st->bb = st->bl = 0;
    st->width = st->max_width = st->height = -1;
    st->min_width = st->min_height = st->max_height = -1;
    st->position = POS_STATIC;
    st->top = st->right_off = st->bottom = st->left = CSS_AUTO_OFF;
    st->line_h = 145;
    st->radius = 0;
    st->indent = 0;
}

/* What passes from a parent to a child, which is a short list and not the
   whole style: a child does not inherit its parent's margins, and a browser
   that lets it produces a page of ever growing indents. */
static inline void css_inherit(cstyle *child, const cstyle *parent) {
    css_default_style(child, parent->font_px);
    child->color = parent->color;
    child->font_px = parent->font_px;
    child->bold = parent->bold;
    child->italic = parent->italic;
    child->mono = parent->mono;
    child->align = parent->align;
    child->white = parent->white;
    child->list = parent->list;
    child->line_h = parent->line_h;
    child->visible = parent->visible;
    child->underline = parent->underline;
    child->background = parent->background;
    child->has_bg = 0;
}

static inline void css_apply(const csheet *s, const cdecl *dcl, cstyle *st,
                             int root_px, int pct_of) {
    const char *v = s->text + dcl->value;
    switch (dcl->prop) {
        case P_COLOR: css_color(v, &st->color); break;
        case P_BACKGROUND:
            if (css_color(v, &st->background)) st->has_bg = 1;
            break;
        case P_BORDER_COLOR: css_color(v, &st->border_color); break;
        case P_DISPLAY:
            if (w_starts_fold(v, "none")) st->display = D_NONE;
            else if (w_starts_fold(v, "inline-block")) st->display = D_INLINE_BLOCK;
            else if (w_starts_fold(v, "inline-flex")) st->display = D_INLINE_BLOCK;
            else if (w_starts_fold(v, "inline")) st->display = D_INLINE;
            else if (w_starts_fold(v, "list-item")) st->display = D_LIST_ITEM;
            else if (w_starts_fold(v, "table-cell")) st->display = D_TABLE_CELL;
            else if (w_starts_fold(v, "flex")) st->display = D_FLEX;
            else st->display = D_BLOCK;   /* block, grid, table: a box */
            break;

        case P_FLEX_DIR:
            if (w_starts_fold(v, "row-reverse")) st->flex_dir = FD_ROW_REVERSE;
            else if (w_starts_fold(v, "column-reverse")) st->flex_dir = FD_COLUMN_REVERSE;
            else if (w_starts_fold(v, "column")) st->flex_dir = FD_COLUMN;
            else st->flex_dir = FD_ROW;
            break;

        case P_JUSTIFY:
            if (w_starts_fold(v, "space-between")) st->justify = JC_BETWEEN;
            else if (w_starts_fold(v, "space-around")) st->justify = JC_AROUND;
            else if (w_starts_fold(v, "space-evenly")) st->justify = JC_EVENLY;
            else if (w_starts_fold(v, "center")) st->justify = JC_CENTER;
            else if (w_starts_fold(v, "flex-end") || w_starts_fold(v, "end")
                     || w_starts_fold(v, "right")) st->justify = JC_END;
            else st->justify = JC_START;
            break;

        case P_ALIGN_ITEMS:
            if (w_starts_fold(v, "center")) st->align_items = AI_CENTER;
            else if (w_starts_fold(v, "flex-end") || w_starts_fold(v, "end"))
                st->align_items = AI_END;
            else if (w_starts_fold(v, "flex-start") || w_starts_fold(v, "start"))
                st->align_items = AI_START;
            else if (w_starts_fold(v, "baseline")) st->align_items = AI_BASELINE;
            else st->align_items = AI_STRETCH;
            break;

        case P_FLEX_WRAP:
            st->flex_wrap = w_starts_fold(v, "wrap") ? 1 : 0;
            break;

        case P_GAP: {
            clen L = css_len(v);
            st->gap = (short)css_px(L, st->font_px, root_px, pct_of);
            if (st->gap < 0) st->gap = 0;
            break;
        }

        case P_FLEX_GROW: {
            /* `flex: 1` and `flex-grow: 1` mean the same thing here. The
               shorthand's other two parts — how it shrinks and what it
               starts from — are read past: an item that grows is the whole
               of what a page uses this for. */
            const char *p = v;
            while (*p == ' ') p++;
            if (w_starts_fold(p, "none")) { st->grow = 0; break; }
            if (w_starts_fold(p, "auto")) { st->grow = 1; break; }
            int n = 0, any = 0;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p++ - '0'); any = 1; }
            st->grow = (short)(any ? n : 0);
            break;
        }
        case P_FONT_SIZE: {
            if (w_starts_fold(v, "smaller")) { st->font_px = (short)(st->font_px * 5 / 6); break; }
            if (w_starts_fold(v, "larger")) { st->font_px = (short)(st->font_px * 6 / 5); break; }
            clen L = css_len(v);
            int px = css_px(L, st->font_px, root_px, st->font_px);
            if (px > 0) st->font_px = (short)(px > 96 ? 96 : (px < 7 ? 7 : px));
            break;
        }
        case P_FONT_WEIGHT: {
            if (w_starts_fold(v, "bold")) st->bold = 1;
            else if (w_starts_fold(v, "normal")) st->bold = 0;
            else if (v[0] >= '1' && v[0] <= '9') st->bold = (v[0] >= '6');
            break;
        }
        case P_FONT_STYLE:
            st->italic = (unsigned char)(w_starts_fold(v, "italic")
                                      || w_starts_fold(v, "oblique"));
            break;
        case P_FONT_FAMILY:
            st->mono = 0;
            for (const char *q = v; *q; q++)
                if (w_lower(q[0]) == 'm' && w_lower(q[1]) == 'o'
                    && w_lower(q[2]) == 'n' && w_lower(q[3]) == 'o') { st->mono = 1; break; }
            break;
        case P_TEXT_ALIGN:
            if (w_starts_fold(v, "center")) st->align = A_CENTER;
            else if (w_starts_fold(v, "right")) st->align = A_RIGHT;
            else if (w_starts_fold(v, "justify")) st->align = A_JUSTIFY;
            else st->align = A_LEFT;
            break;
        case P_TEXT_DECORATION:
            st->underline = (unsigned char)(w_starts_fold(v, "underline") != 0);
            st->strike = (unsigned char)(w_starts_fold(v, "line-through") != 0);
            if (w_starts_fold(v, "none")) { st->underline = 0; st->strike = 0; }
            break;
        case P_WHITE_SPACE:
            if (w_starts_fold(v, "pre")) st->white = WS_PRE;
            else if (w_starts_fold(v, "nowrap")) st->white = WS_NOWRAP;
            else st->white = WS_NORMAL;
            break;
        case P_LIST_STYLE:
            if (w_starts_fold(v, "none")) st->list = LS_NONE;
            else if (w_starts_fold(v, "decimal")) st->list = LS_DECIMAL;
            else if (w_starts_fold(v, "circle")) st->list = LS_CIRCLE;
            else if (w_starts_fold(v, "square")) st->list = LS_SQUARE;
            else st->list = LS_DISC;
            break;
        case P_BOX_SIZING:
            /* content-box is the default and nobody wants it. */
            st->border_box = (unsigned char)w_starts_fold(v, "border-box");
            break;

        case P_POSITION:
            if (w_starts_fold(v, "absolute")) st->position = POS_ABSOLUTE;
            else if (w_starts_fold(v, "fixed")) st->position = POS_FIXED;
            else if (w_starts_fold(v, "relative")) st->position = POS_RELATIVE;
            else if (w_starts_fold(v, "sticky")) st->position = POS_RELATIVE;
            else st->position = POS_STATIC;
            break;

        case P_VISIBILITY:
            st->visible = (unsigned char)(!w_starts_fold(v, "hidden"));
            break;
        case P_OPACITY:
            /* Nothing composites, so anything close to invisible is treated
               as hidden and everything else as drawn. */
            if (v[0] == '0' && (v[1] == 0 || (v[1] == '.' && v[2] <= '1')))
                st->visible = 0;
            break;
        case P_LINE_HEIGHT: {
            clen L = css_len(v);
            if (L.unit == U_PCT) st->line_h = (short)(L.v / 100);
            else if (L.unit == U_PX && L.v > 400)
                st->line_h = (short)(L.v * 100 / (st->font_px * 100));
            else if (L.unit != U_AUTO) st->line_h = (short)(L.v);   /* a number */
            if (st->line_h < 90) st->line_h = 90;
            if (st->line_h > 300) st->line_h = 300;
            break;
        }
        default: {
            clen L = css_len(v);
            int px = css_px(L, st->font_px, root_px, pct_of);
            short *slot = 0;
            switch (dcl->prop) {
                case P_MARGIN_T: slot = &st->mt; break;
                case P_MARGIN_R: slot = &st->mr; break;
                case P_MARGIN_B: slot = &st->mb; break;
                case P_MARGIN_L: slot = &st->ml; break;
                case P_PADDING_T: slot = &st->pt; break;
                case P_PADDING_R: slot = &st->pr; break;
                case P_PADDING_B: slot = &st->pb; break;
                case P_PADDING_L: slot = &st->pl; break;
                case P_BORDER_T: slot = &st->bt; break;
                case P_BORDER_R: slot = &st->br; break;
                case P_BORDER_B: slot = &st->bb; break;
                case P_BORDER_L: slot = &st->bl; break;
                case P_WIDTH: slot = &st->width; break;
                case P_MAX_WIDTH: slot = &st->max_width; break;
                case P_MIN_WIDTH: slot = &st->min_width; break;
                case P_MIN_HEIGHT: slot = &st->min_height; break;
                case P_MAX_HEIGHT: slot = &st->max_height; break;
                case P_TOP: slot = &st->top; break;
                case P_RIGHT: slot = &st->right_off; break;
                case P_BOTTOM: slot = &st->bottom; break;
                case P_LEFT: slot = &st->left; break;
                case P_HEIGHT: slot = &st->height; break;
                case P_RADIUS: slot = &st->radius; break;
                case P_TEXT_INDENT: slot = &st->indent; break;
                default: break;
            }
            if (!slot) break;
            if (dcl->prop >= P_BORDER_T && dcl->prop <= P_BORDER_L) {
                /* border-top and friends carry a style and a colour too. */
                if (w_starts_fold(v, "none") || w_starts_fold(v, "hidden")) px = 0;
                else if (px < 0) px = 1;
                u32 c;
                int st2[4], ln2[4];
                int np = css_parts(v, w_len(v), st2, ln2, 4);
                for (int q = 0; q < np; q++)
                    if (css_color(v + st2[q], &c)) { st->border_color = c; break; }
            }
            /* The four offsets are not lengths like the others.
             *
               `auto` is not zero for them -- an absolute box with neither a
               top nor a bottom stays where the flow would have put it --
               and a negative one is ordinary rather than a mistake, because
               half the centring on the web is `left: 50%` with a negative
               margin to match. So they keep their sign and their auto,
               where every other length here clamps both away. */
            if (dcl->prop >= P_TOP && dcl->prop <= P_LEFT) {
                if (L.unit == U_AUTO) { *slot = CSS_AUTO_OFF; break; }
                if (px < -4000) px = -4000;
                if (px > 4000) px = 4000;
                *slot = (short)px;
                break;
            }

            if (px < -1) px = -1;
            if (px > 4000) px = 4000;
            *slot = (short)px;
            break;
        }
    }
}

/* --- finding the rules that could match ----------------------------------
 *
 * Walking every rule for every element is the obvious way and it is far too
 * slow: a real page is ten thousand nodes and a real sheet is a few thousand
 * rules, which is tens of millions of compound comparisons for one layout,
 * and the layout runs again on every resize.
 *
 * So the rules are filed under the one thing their rightmost part insists
 * on: an id, or a class, or an element name. An element then only looks at
 * the rules filed under its own id, its own classes and its own name, plus
 * the few that insist on nothing. That is the whole of the trick and it
 * turns the tens of millions into tens. */

#define CSS_BUCKETS 512

typedef struct {
    int by_tag[T_COUNT];
    int by_key[CSS_BUCKETS];
    int universal;
    int next[CSS_RULES];
} cindex;

static inline unsigned css_hash(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h & (CSS_BUCKETS - 1);
}

static inline void css_index(const csheet *s, cindex *x) {
    for (int i = 0; i < T_COUNT; i++) x->by_tag[i] = -1;
    for (int i = 0; i < CSS_BUCKETS; i++) x->by_key[i] = -1;
    x->universal = -1;

    /* Backwards, so each chain comes out in source order and the cascade
       below sees the rules in the order they were written. */
    for (int r = s->nrules - 1; r >= 0; r--) {
        const crule *rule = &s->rules[r];
        const csel *c = &s->sels[rule->sel_at + rule->sel_n - 1];
        int *head;
        if (c->id >= 0) head = &x->by_key[css_hash(s->text + c->id)];
        else if (c->ncls > 0 && c->cls[0] >= 0)
            head = &x->by_key[css_hash(s->text + c->cls[0])];
        else if (c->tag >= 0) head = &x->by_tag[c->tag];
        else head = &x->universal;
        x->next[r] = *head;
        *head = r;
    }
}

/* --- the cascade --------------------------------------------------------- */

#define CSS_HITS 192

typedef struct {
    int rule;
    int spec;
    int order;
} chit;

static inline void css_collect_chain(const csheet *s, const cindex *x,
                                     const ddoc *d, int el, int head,
                                     const cmatch *m, chit *hits, int *n) {
    for (int r = head; r >= 0; r = x->next[r]) {
        if (*n >= CSS_HITS) return;
        const crule *rule = &s->rules[r];
        if (!css_matches(s, d, el, rule, m)) continue;
        hits[*n].rule = r;
        hits[*n].spec = rule->spec;
        hits[*n].order = rule->order;
        (*n)++;
    }
}

/* Every rule that applies to this element, in the order they should be
   applied: weaker specificity first, and among equals the one written
   later wins, which is what "later in the sheet" means. */
static inline int css_collect(const csheet *s, const cindex *x, const ddoc *d,
                              int el, const cmatch *m, chit *hits) {
    int n = 0;
    css_collect_chain(s, x, d, el, x->universal, m, hits, &n);
    int tag = d->nodes[el].tag;
    if (tag > 0 && tag < T_COUNT)
        css_collect_chain(s, x, d, el, x->by_tag[tag], m, hits, &n);

    const char *id = dom_attr(d, el, "id");
    if (id && *id)
        css_collect_chain(s, x, d, el, x->by_key[css_hash(id)], m, hits, &n);

    const char *cl = dom_attr(d, el, "class");
    if (cl) {
        char one[64];
        int i = 0;
        while (cl[i]) {
            while (cl[i] == ' ' || cl[i] == '\t' || cl[i] == '\n'
                   || cl[i] == '\r') i++;
            if (!cl[i]) break;
            int w = 0;
            while (cl[i] && cl[i] != ' ' && cl[i] != '\t' && cl[i] != '\n'
                   && cl[i] != '\r') {
                if (w < 63) one[w++] = cl[i];
                i++;
            }
            one[w] = 0;
            if (w) css_collect_chain(s, x, d, el, x->by_key[css_hash(one)],
                                     m, hits, &n);
        }
    }

    /* Insertion sort: n is small, and it is stable, which matters because
       two rules of equal specificity have to stay in source order. */
    for (int i = 1; i < n; i++) {
        chit h = hits[i];
        int j = i - 1;
        while (j >= 0 && (hits[j].spec > h.spec
                          || (hits[j].spec == h.spec && hits[j].order > h.order))) {
            hits[j + 1] = hits[j];
            j--;
        }
        hits[j + 1] = h;
    }
    return n;
}

/* --- what the browser itself says about each element ----------------------
 *
 * Written as a style sheet rather than as a switch, because it is one, and
 * because a page's own rules then beat it by the ordinary cascade rather
 * than by a special case. A user agent sheet loses every tie by being
 * parsed first, which is exactly right: a page that says its headings are
 * small means it. */
static const char CSS_UA[] =
    "html,body{display:block;margin:0;padding:0;color:#1a1a1a}"
    "body{padding:0 2px}"
    "div,section,article,main,aside,nav,header,footer,figure,figcaption,"
    "hgroup,form,dl,dt,fieldset,address{display:block}"
    "p{display:block;margin:0.85em 0}"
    /* Obsolete since 1999 and on the front page of Google. It is a
       block that centres what is in it, and the centring is inherited
       by everything inside, which is the whole of what it does. */
    "center{display:block;text-align:center}"
    "h1{display:block;font-size:2em;font-weight:bold;margin:0.55em 0 0.4em}"
    "h2{display:block;font-size:1.5em;font-weight:bold;margin:0.7em 0 0.35em}"
    "h3{display:block;font-size:1.22em;font-weight:bold;margin:0.8em 0 0.3em}"
    "h4{display:block;font-size:1.05em;font-weight:bold;margin:0.9em 0 0.3em}"
    "h5,h6{display:block;font-weight:bold;margin:1em 0 0.3em}"
    "h6{font-size:0.9em}"
    "ul,ol{display:block;margin:0.7em 0;padding-left:1.8em}"
    "li{display:list-item;margin:0.25em 0}"
    "ol{list-style-type:decimal}"
    "dd{display:block;margin-left:1.8em}"
    "blockquote{display:block;margin:0.9em 0;padding-left:1em;"
        "border-left:3px #d6d6d6;color:#555}"
    "pre{display:block;white-space:pre;font-family:monospace;margin:0.9em 0;"
        "padding:10px 12px;background:#f6f7f9;border-radius:6px;color:#222}"
    "code,kbd,samp,tt{font-family:monospace;font-size:0.93em}"
    "b,strong{font-weight:bold}"
    "i,em{font-style:italic}"
    "small{font-size:0.86em}"
    "a{color:#0b5ed7;text-decoration:underline}"
    "hr{display:block;margin:1.1em 0;border-top:1px #dcdcdc}"
    "table{display:block;margin:0.8em 0}"
    "tr{display:block}"
    "td,th{display:table-cell;padding:4px 10px 4px 0}"
    "th{font-weight:bold;text-align:left}"
    "caption{display:block;font-style:italic;margin:0.3em 0}"
    "head,script,style,title,meta,link,noscript{display:none}"
    "button{display:inline-block;padding:5px 12px;background:#f2f3f5;"
        "border:1px #c9ccd1;border-radius:6px}"
    "input,textarea,select{display:inline-block;padding:4px 8px;"
        "background:#ffffff;border:1px #c9ccd1;border-radius:6px}"
    "img{display:inline-block}"
    /* Old markup that is still on the front of some very large sites.
       center is a block that centres what is in it, and it is how the
       plain version of more than one homepage is laid out to this day. */
    "center{display:block;text-align:center}"
    "iframe,svg{display:none}";
