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

/* Measured against what a real site sends rather than guessed. One
   encyclopaedia article's two sheets are two hundred and seventeen
   kilobytes and eleven hundred rules between them; the rule and selector
   counts had room for that and the text did not, and a sheet whose text
   ran out halfway is a page styled by the first half of its stylesheet --
   which looks like a layout bug and is not one. */
#define CSS_RULES   16000
#define CSS_SELS    40000
#define CSS_DECLS   64000
#define CSS_TEXT    (2048 * 1024)
#define CSS_MAXCLS  4
#define CSS_NEGS    2048

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
    P_VALIGN, P_SPACING, P_COLLAPSE, P_OVERFLOW, P_CLIP, P_FLOAT, P_CLEAR, P_TRANSFORM,
    P_GRID_COLS, P_GRID_COLUMN, P_GRID_AREAS, P_GRID_AREA, P_GRID_FLOW, P_GRID_AUTO_COLS,
    P_MASK,
    P_CUSTOM,        /* --name: value, kept as the text "--name:value" */
    P_DEFER,         /* a shorthand whose value has var() in it: "name:value" */
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
       D_TABLE_CELL, D_FLEX, D_TABLE, D_TABLE_ROW, D_TABLE_GROUP,
       D_INLINE_FLEX, D_GRID, D_CONTENTS };

/* Where a table cell's contents sit when the row is taller than they are.
   Baseline is what nothing asked for, and a cell treats it as the middle,
   which is what a row does for its cells unless told otherwise. */
enum { VA_BASELINE = 0, VA_TOP, VA_MIDDLE, VA_BOTTOM };

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
enum { U_PX = 0, U_EM, U_REM, U_PCT, U_VW, U_VH, U_AUTO };

/* The window, for the units that are a fraction of it. Set by the layout
   before it runs, because the page is measured against the window it is
   being laid out in and nothing here can ask.

   Without these, `width:100vw` is a hundred pixels: the unit is dropped and
   the number is taken as it stands. An overlay meant to cover the window
   comes out an inch wide, and everything the page put inside it is folded
   into a column one word across. */
static int css_view_w;
static int css_view_h;

/* What a percentage height is a percentage of: the parent's content height
   when the parent was given a height of its own, -1 when it was not. The
   layout sets it before an element's rules are applied (layout.h,
   lay_style), since nothing here can see the parent. */
static int css_parent_h = -1;

typedef struct {
    /* Hundredths of the unit, and an int rather than a short because a
       short holds 327.67 of anything. Pages are full of `width:960px` and
       `max-width:1140px`, which at a hundredth each are 96000 and 114000
       and do not fit: the first came back as 304 and the second as a
       negative number, so one box was a third of the width it asked for
       and the other had no ceiling at all. Both on a page whose markup and
       stylesheet were perfectly ordinary. */
    int v;
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

    /* A table's: the room between its cells (-1 when nothing said, which is
       two pixels, or cellspacing), inherited the way border-spacing is; and
       a cell's own vertical-align. */
    short spacing;
    unsigned char valign;

    /* Something written to be read aloud and not seen: clipped to nothing,
       or a box a pixel across with its overflow hidden. Screen readers get
       "skip to content" and the rest of a page's labels from these, and
       drawn they are words scattered over the top of every page. */
    unsigned char clip, gone;

    /* Floated to one side (1 left, 2 right) and cleared past floats (1 left,
       2 right, 3 both). See layout.h, lay_float. */
    unsigned char floated, clear;

    /* Painted through a mask this cannot draw: an icon is often a square of
       the text colour with the icon's shape as its mask, and drawn without
       the mask it is a black square. Its background is left out. */
    unsigned char masked;

    /* Text in a colour that cannot be seen: laid out, not drawn. Inherited. */
    unsigned char ink_none;

    /* A transform's translation: pixels, plus a percentage of the box's own
       width or height (layout.h, lay_translate). Nothing else a transform
       does is drawn. */
    short tx_px, ty_px, tx_pct, ty_pct;

    /* A grid's columns, as the text of grid-template-columns (an offset into
       the sheet's text, -1 for none), read by lay_grid; and how many columns
       an item spans (0 one, -1 all of them). */
    const char *grid_cols, *grid_areas, *garea;  /* the texts, or null; the layout keeps them alive */

    /* grid-auto-columns, the size of a column the items make for
       themselves (null: auto); and whether grid-auto-flow is column, which
       lays the items out along one row, a new column each. */
    const char *grid_auto;
    unsigned char gflow_col;

    /* Whether width was a percentage, which a box being measured for how
       wide its contents want to be cannot resolve: it is auto there, as the
       rules have it (layout.h, lay_drawing). */
    unsigned char width_pct;

    /* The custom properties in force, as the head of a chain the layout
       keeps (layout.h, lay_var_*); inherited, as custom properties are.
       -1 for none. */
    int vars;
    short gspan;
} cstyle;

/* --- the text of a sheet -------------------------------------------------
 *
 * Every string a sheet needs lives in one arena, so a rule is a handful of
 * ints and nothing has to be freed. */

typedef struct {
    short tag;                /* a T_* tag, or -1 for any */
    int   tname;              /* an element this has no T_ for, by name
                                 (lower case, into the text), or -1 */
    int   id;                 /* into the text, or -1 */
    int   cls[CSS_MAXCLS];
    short ncls;
    short combinator;         /* how it joins the part to its left */
    short pseudo;
    short nth_a, nth_b;       /* :nth-child(An+B) and its kin */
    int   neg;                /* a compound it must not match (:not), or -1 */
} csel;

enum { CB_FIRST = 0, CB_DESC, CB_CHILD, CB_NEXT, CB_LATER };
enum { PS_NONE = 0, PS_HOVER, PS_LINK, PS_VISITED, PS_FIRST_CHILD, PS_ROOT,
       PS_LAST_CHILD, PS_ONLY_CHILD, PS_FIRST_OF_TYPE, PS_LAST_OF_TYPE, PS_ONLY_OF_TYPE,
       PS_EMPTY, PS_CHECKED, PS_DISABLED, PS_ENABLED, PS_NEVER,
       PS_NTH_CHILD, PS_NTH_OF_TYPE, PS_NTH_LAST_CHILD };

typedef struct {
    short prop;
    int   value;              /* into the text */
} cdecl;

typedef struct {
    int sel_at, sel_n;        /* the compound parts, leftmost first */
    int decl_at, decl_n;
    int spec;                 /* ids, then classes, then element names */
    int order;
    /* The window widths the rule is for, from the media queries round it:
       at least mq_lo and at most mq_hi pixels, -1 for no bound. */
    short mq_lo, mq_hi;
} crule;

typedef struct {
    crule rules[CSS_RULES];
    int   nrules;
    int   ua_rules;           /* how many of them are the browser's own */
    csel  sels[CSS_SELS];
    int   nsels;
    cdecl decls[CSS_DECLS];
    int   ndecls;
    csel  negs[CSS_NEGS];     /* the insides of :not(...) */
    int   nnegs;
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
/* How opaque the last colour read was, 0 to 255: a background that is
   (nearly) transparent is no background, and text in a transparent colour
   is text nobody is meant to see, where both used to be drawn white. */
static int css_last_alpha = 255;

/* hsl(h, s%, l%) as RGB, in whole numbers: h in degrees, s and l in
   hundredths of one. */
static inline u32 css_hsl(int h, int s, int l) {
    h %= 360;
    if (h < 0) h += 360;
    int c = (100 - (2 * l - 100 < 0 ? 100 - 2 * l : 2 * l - 100)) * s / 100;   /* chroma, % */
    int hp = h * 100 / 60;                                                   /* h' in hundredths */
    int m2 = hp % 200 - 100;
    int x = c * (100 - (m2 < 0 ? -m2 : m2)) / 100;
    int r = 0, g = 0, b = 0;
    if (hp < 100) { r = c; g = x; }
    else if (hp < 200) { r = x; g = c; }
    else if (hp < 300) { g = c; b = x; }
    else if (hp < 400) { g = x; b = c; }
    else if (hp < 500) { r = x; b = c; }
    else { r = c; b = x; }
    int m = l - c / 2;
    r = (r + m) * 255 / 100; g = (g + m) * 255 / 100; b = (b + m) * 255 / 100;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (u32)((r << 16) | (g << 8) | b);
}

static inline int css_color(const char *s, u32 *out) {
    while (*s == ' ') s++;
    css_last_alpha = 255;
    if (w_starts_fold(s, "hsl")) {
        while (*s && *s != '(') s++;
        if (*s) s++;
        int c[4] = { 0, 0, 0, 255 }, n = 0;
        while (*s && *s != ')' && n < 4) {
            while (*s == ' ' || *s == ',' || *s == '/') s++;
            int v = 0, any = 0, frac = 0, fdiv = 1, neg = 0;
            if (*s == '-') { neg = 1; s++; }
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
            if (*s == '.') {
                s++;
                while (*s >= '0' && *s <= '9') {
                    if (fdiv < 1000) { frac = frac * 10 + (*s - '0'); fdiv *= 10; }
                    s++; any = 1;
                }
            }
            if (!any) break;
            if (n == 3) v = *s == '%' ? v * 255 / 100 : v * 255 + frac * 255 / fdiv;
            if (*s == '%') s++;
            while (*s && *s != ' ' && *s != ',' && *s != '/' && *s != ')') s++;   /* deg */
            c[n++] = neg ? -v : v;
        }
        if (n < 3) return 0;
        css_last_alpha = c[3] < 0 ? 0 : (c[3] > 255 ? 255 : c[3]);
        u32 rgb = css_hsl(c[0], c[1], c[2]);
        int a = css_last_alpha;
        int r = (int)((rgb >> 16) & 255), g = (int)((rgb >> 8) & 255), b = (int)(rgb & 255);
        r = (r * a + 255 * (255 - a)) / 255;
        g = (g * a + 255 * (255 - a)) / 255;
        b = (b * a + 255 * (255 - a)) / 255;
        *out = (u32)((r << 16) | (g << 8) | b);
        return 1;
    }
    if (w_same_fold(s, "transparent")) { *out = 0xFFFFFF; css_last_alpha = 0; return 1; }
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
            if (n == 8) css_last_alpha = d[6] * 16 + d[7];
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
            /* `.5` with no nought in front is how most sheets write a half:
               read as no number, the alpha was lost and a faint shadow
               colour was drawn solid black. */
            if (*s == '.') {
                s++;
                while (*s >= '0' && *s <= '9') {
                    if (fdiv < 1000) { frac = frac * 10 + (*s - '0'); fdiv *= 10; }
                    s++;
                    any = 1;
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
        css_last_alpha = a;
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
    while (*s >= '0' && *s <= '9') {
        if (v < 1000000) v = v * 10 + (*s - '0');
        s++;
        any = 1;
    }
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
    /* Hundredths, so half an em survives. Ten thousand of any unit is far
       past anything a page means and keeps the multiplications below in
       range. */
    if (v > 10000) v = 10000;
    L.v = sign * (v * 100 + frac);
    if (w_starts_fold(s, "em")) L.unit = U_EM;
    else if (w_starts_fold(s, "rem")) L.unit = U_REM;
    else if (w_starts_fold(s, "vw")) L.unit = U_VW;
    else if (w_starts_fold(s, "vh")) L.unit = U_VH;
    else if (*s == '%') L.unit = U_PCT;
    else if (w_starts_fold(s, "pt")) { L.v = L.v * 4 / 3; L.unit = U_PX; }
    else L.unit = U_PX;
    return L;
}

/* px, given what the relative units are relative to. */
static inline int css_px(clen L, int font_px, int root_px, int pct_of) {
    switch (L.unit) {
        case U_EM:  return L.v * font_px / 100;
        case U_REM: return L.v * root_px / 100;
        case U_PCT: return pct_of >= 0 ? L.v * pct_of / 10000 : 0;
        case U_VW:  return css_view_w > 0 ? L.v * css_view_w / 10000 : -1;
        case U_VH:  return css_view_h > 0 ? L.v * css_view_h / 10000 : -1;
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
    { "float", P_FLOAT },
    { "clear", P_CLEAR },
    { "transform", P_TRANSFORM },
    { "grid-template-columns", P_GRID_COLS },
    { "grid-column", P_GRID_COLUMN },
    { "grid-template-areas", P_GRID_AREAS },
    { "grid-area", P_GRID_AREA },
    { "grid-auto-flow", P_GRID_FLOW },
    { "grid-auto-columns", P_GRID_AUTO_COLS },
    { "mask", P_MASK },
    { "mask-image", P_MASK },
    { "-webkit-mask", P_MASK },
    { "-webkit-mask-image", P_MASK },
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
    { "vertical-align", P_VALIGN },
    { "border-spacing", P_SPACING },
    { "border-collapse", P_COLLAPSE },
    { "overflow", P_OVERFLOW },
    { "overflow-x", P_OVERFLOW },
    { "clip", P_CLIP },
    { "clip-path", P_CLIP },
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
    s->nnegs = 0;
    s->ua_rules = 0;
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
/* Whether a run of `n` characters is this name, whatever the case. Used on
   selector text, which is not terminated: it is a slice of the sheet. */
static inline int css_named(const char *p, int n, const char *name) {
    int i = 0;
    for (; i < n; i++) {
        if (!name[i]) return 0;
        if (w_lower(p[i]) != w_lower(name[i])) return 0;
    }
    return name[i] == 0;
}

/* An attribute selector's test, written after its name in the text as one
   character and the value: '?' is only that the attribute is there. */
static inline int css_attr_op(const char *p, int len, int i, char *op) {
    *op = '?';
    if (i < len && p[i] == '=') { *op = '='; return i + 1; }
    if (i + 1 < len && p[i + 1] == '=' && (p[i] == '~' || p[i] == '|' || p[i] == '^'
                                           || p[i] == '$' || p[i] == '*')) {
        *op = p[i];
        return i + 2;
    }
    return i;
}

/* An+B from the inside of :nth-child(...): odd, even, 3, 2n+1, -n+3, n. */
static inline int css_nth(const char *p, int len, int i, short *a, short *b) {
    while (i < len && css_space(p[i])) i++;
    if (css_named(p + i, (i + 3 <= len ? 3 : len - i), "odd")) { *a = 2; *b = 1; return 1; }
    if (css_named(p + i, (i + 4 <= len ? 4 : len - i), "even")) { *a = 2; *b = 0; return 1; }
    int sign = 1, num = 0, have = 0;
    if (i < len && (p[i] == '-' || p[i] == '+')) { if (p[i] == '-') sign = -1; i++; }
    while (i < len && p[i] >= '0' && p[i] <= '9') { num = num * 10 + (p[i++] - '0'); have = 1; }
    if (i < len && (p[i] == 'n' || p[i] == 'N')) {
        *a = (short)(sign * (have ? num : 1));
        i++;
        while (i < len && css_space(p[i])) i++;
        int bs = 1, bn = 0;
        if (i < len && (p[i] == '+' || p[i] == '-')) {
            if (p[i] == '-') bs = -1;
            i++;
            while (i < len && css_space(p[i])) i++;
            while (i < len && p[i] >= '0' && p[i] <= '9') bn = bn * 10 + (p[i++] - '0');
        }
        *b = (short)(bs * bn);
    } else {
        if (!have) return 0;
        *a = 0;
        *b = (short)(sign * num);
    }
    while (i < len && css_space(p[i])) i++;
    return i < len && p[i] == ')';
}

/* One compound -- a tag, classes, an id, attributes and pseudo-classes run
   together -- into `c`. Returns whether it had anything in it, or -1 when it
   uses something that cannot be matched here and the selector must go. */
static inline int css_parse_compound(csheet *s, const char *p, int len, int *at,
                                     csel *c, int *spec, int inner) {
    int i = *at, any = 0;
    c->tag = -1; c->tname = -1; c->id = -1; c->ncls = 0; c->pseudo = PS_NONE;
    c->neg = -1; c->nth_a = c->nth_b = 0;
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
            if (ch == '#') { c->id = t; *spec += 1 << 20; }
            else if (c->ncls < CSS_MAXCLS) {
                c->cls[c->ncls++] = t;
                *spec += 1 << 10;
            }
            any = 1;
            continue;
        }
        if (ch == ':') {
            i++;
            int elem = 0;
            if (i < len && p[i] == ':') { i++; elem = 1; }
            int st = i;
            while (i < len && css_ident(p[i])) i++;
            int l = i - st;
            const char *nm = p + st;

            /* The four that are written with one colon as often as two. */
            if (!elem && (css_named(nm, l, "before") || css_named(nm, l, "after")
                          || css_named(nm, l, "first-line") || css_named(nm, l, "first-letter")))
                elem = 1;

            /* A pseudo-element is not an element in the tree. Nothing here
               draws a scrollbar or a ::before box, and a selector that names
               one must not quietly become a selector for the element it
               hangs off -- still less for everything.

               `::-webkit-scrollbar{width:6px}` was exactly that: after the
               two colons there is no tag, no class and no id left, which is
               the shape of `*`. Every box on the page came out six pixels
               wide, and a whole site's article was a column of one word per
               line. */
            if (elem) return -1;

            if (i < len && p[i] == '(') {
                int open = i + 1, d = 0;
                while (i < len) {
                    if (p[i] == '(') d++;
                    else if (p[i] == ')') { d--; i++; if (!d) break; continue; }
                    i++;
                }
                int close = i - 1;                    /* at the ')' */
                if (css_named(nm, l, "nth-child") || css_named(nm, l, "nth-of-type")
                    || css_named(nm, l, "nth-last-child")) {
                    if (c->pseudo != PS_NONE) return -1;
                    if (!css_nth(p, close + 1, open, &c->nth_a, &c->nth_b)) return -1;
                    c->pseudo = css_named(nm, l, "nth-child") ? PS_NTH_CHILD
                              : css_named(nm, l, "nth-of-type") ? PS_NTH_OF_TYPE : PS_NTH_LAST_CHILD;
                } else if (css_named(nm, l, "not") && !inner && c->neg < 0) {
                    /* One simple compound inside: :not(.open), :not(:last-child),
                       :not([hidden]). A list or anything more is not guessed at. */
                    if (s->nnegs >= CSS_NEGS) return -1;
                    csel *ng = &s->negs[s->nnegs];
                    int q = open, sp2 = 0;
                    while (q < close && css_space(p[q])) q++;
                    int got = css_parse_compound(s, p, close, &q, ng, &sp2, 1);
                    while (q < close && css_space(p[q])) q++;
                    if (got <= 0 || q != close) return -1;
                    c->neg = s->nnegs++;
                    *spec += sp2;
                } else if ((css_named(nm, l, "is") || css_named(nm, l, "where")) && !inner) {
                    /* :is(X) and :where(X) with one compound are that compound. */
                    int q = open, sp2 = 0;
                    csel tmp;
                    while (q < close && css_space(p[q])) q++;
                    int got = css_parse_compound(s, p, close, &q, &tmp, &sp2, 1);
                    while (q < close && css_space(p[q])) q++;
                    if (got <= 0 || q != close) return -1;
                    if (tmp.tag >= 0) { if (c->tag >= 0 && c->tag != tmp.tag) return -1; c->tag = tmp.tag; }
                    if (tmp.tname >= 0) c->tname = tmp.tname;
                    if (tmp.id >= 0) c->id = tmp.id;
                    for (int k = 0; k < tmp.ncls && c->ncls < CSS_MAXCLS; k++) c->cls[c->ncls++] = tmp.cls[k];
                    if (tmp.pseudo != PS_NONE) {
                        if (c->pseudo != PS_NONE) return -1;
                        c->pseudo = tmp.pseudo; c->nth_a = tmp.nth_a; c->nth_b = tmp.nth_b;
                    }
                    if (css_named(nm, l, "is")) *spec += sp2;
                } else {
                    return -1;
                }
                any = 1;
                continue;
            }

            int ps = -1;
            if (css_named(nm, l, "hover")) ps = PS_HOVER;
            else if (css_named(nm, l, "link") || css_named(nm, l, "any-link")) ps = PS_LINK;
            else if (css_named(nm, l, "visited")) ps = PS_VISITED;
            else if (css_named(nm, l, "root")) ps = PS_ROOT;
            else if (css_named(nm, l, "first-child")) ps = PS_FIRST_CHILD;
            else if (css_named(nm, l, "last-child")) ps = PS_LAST_CHILD;
            else if (css_named(nm, l, "only-child")) ps = PS_ONLY_CHILD;
            else if (css_named(nm, l, "first-of-type")) ps = PS_FIRST_OF_TYPE;
            else if (css_named(nm, l, "last-of-type")) ps = PS_LAST_OF_TYPE;
            else if (css_named(nm, l, "only-of-type")) ps = PS_ONLY_OF_TYPE;
            else if (css_named(nm, l, "empty")) ps = PS_EMPTY;
            else if (css_named(nm, l, "checked")) ps = PS_CHECKED;
            else if (css_named(nm, l, "disabled")) ps = PS_DISABLED;
            else if (css_named(nm, l, "enabled")) ps = PS_ENABLED;
            else if (css_named(nm, l, "defined") || css_named(nm, l, "scope")) ps = PS_NONE;
            /* Everything else is a state nothing is in when a page is first
               drawn -- focused, pressed, the target of the address, invalid,
               open, full screen -- or one this cannot know. They were read
               past as if they were not there, so `:focus` outlines and
               `:checked ~ .menu {display:block}` applied all the time. */
            else ps = PS_NEVER;
            if (ps != PS_NONE) {
                if (c->pseudo != PS_NONE && c->pseudo != ps) {
                    /* Two states on one compound: the second is only kept
                       when it can never be true, which decides the matter. */
                    if (ps == PS_NEVER) c->pseudo = PS_NEVER;
                    else if (c->pseudo != PS_NEVER) return -1;
                } else {
                    c->pseudo = (short)ps;
                }
            }
            *spec += 1 << 10;
            any = 1;
            continue;
        }
        if (ch == '[') {
            /* [name], [name=value], [name~=value] and the rest, with the value
               quoted or not and an i after it for case not mattering. The
               name and the test go into the text one after the other. */
            i++;
            while (i < len && css_space(p[i])) i++;
            int st = i;
            while (i < len && (css_ident(p[i]) || p[i] == ':')) i++;
            int nl = i - st;
            while (i < len && css_space(p[i])) i++;
            char op;
            i = css_attr_op(p, len, i, &op);
            char val[256];
            int vl = 0, fold = 0;
            if (op != '?') {
                while (i < len && css_space(p[i])) i++;
                if (i < len && (p[i] == '"' || p[i] == '\'')) {
                    char q = p[i++];
                    while (i < len && p[i] != q) {
                        if (p[i] == '\\' && i + 1 < len) i++;
                        if (vl < 255) val[vl++] = p[i];
                        i++;
                    }
                    if (i < len) i++;
                } else {
                    while (i < len && p[i] != ']' && !css_space(p[i])) {
                        if (vl < 255) val[vl++] = p[i];
                        i++;
                    }
                }
                while (i < len && css_space(p[i])) i++;
                if (i < len && (p[i] == 'i' || p[i] == 'I')) { fold = 1; i++; }
                else if (i < len && (p[i] == 's' || p[i] == 'S')) i++;
                while (i < len && css_space(p[i])) i++;
            }
            if (i >= len || p[i] != ']' || nl == 0) return -1;
            i++;
            int t = css_put_lower(s, p + st, nl);
            char tv[258];
            tv[0] = op;
            tv[1] = fold ? 'i' : 's';
            for (int k = 0; k < vl; k++) tv[2 + k] = val[k];
            if (t < 0 || css_put(s, tv, 2 + vl) < 0) return -1;
            if (c->ncls < CSS_MAXCLS) {
                /* Held as a class with a marker, so matching has one loop
                   rather than two lists. */
                c->cls[c->ncls++] = -t - 2;
                *spec += 1 << 10;
            }
            any = 1;
            continue;
        }
        if (css_ident(ch)) {
            int st = i;
            while (i < len && css_ident(p[i])) i++;
            int tg = html_tag_of(p + st, i - st);
            c->tag = (short)(tg == T_OTHER ? -1 : tg);
            /* An element this has no number for -- tbody, sup, a page's own
               custom elements -- is matched by its name. These rules used to
               be dropped whole, so `tbody tr` and every rule for a custom
               element never applied. */
            if (tg == T_OTHER) c->tname = css_put_lower(s, p + st, i - st);
            *spec += 1;
            any = 1;
            continue;
        }
        break;
    }
    *at = i;
    return any;
}

static inline int css_parse_selector(csheet *s, const char *p, int len,
                                     int *at, int *spec_out) {
    int i = *at, n = 0, spec = 0;
    int combinator = CB_FIRST;

    for (;;) {
        i = css_skip(p, len, i);
        if (i >= len || p[i] == ',' || p[i] == '{') break;

        if (p[i] == '>') { combinator = CB_CHILD; i++; continue; }
        /* The element straight after (+) and any after (~): how a page
           hides a menu until the box before it is ticked, and styles the
           paragraph after a heading. They used to drop the rule. */
        if (p[i] == '+') { combinator = CB_NEXT; i++; continue; }
        if (p[i] == '~') { combinator = CB_LATER; i++; continue; }

        if (s->nsels >= CSS_SELS) { s->overflowed = 1; return 0; }
        csel *c = &s->sels[s->nsels];
        int got = css_parse_compound(s, p, len, &i, c, &spec, 0);
        if (got < 0) {
            while (i < len && p[i] != ',' && p[i] != '{') i++;
            *at = i;
            return 0;
        }
        c->combinator = (short)(n == 0 ? CB_FIRST : combinator);
        combinator = CB_DESC;
        if (!got) break;
        s->nsels++;
        n++;
    }

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
/* Set on a declaration whose value uses var(), so the layout knows to put
   the variables in before applying it without looking at every value. */
#define CSS_HAS_VAR 0x4000

static inline void css_add(csheet *s, int prop, const char *v, int vlen) {
    if (s->ndecls >= CSS_DECLS) { s->overflowed = 1; return; }
    int t = css_put(s, v, vlen);
    if (t < 0) return;
    for (int i = 0; i + 3 < vlen; i++)
        if (v[i] == 'v' && v[i + 1] == 'a' && v[i + 2] == 'r' && v[i + 3] == '(') { prop |= CSS_HAS_VAR; break; }
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
    /* Trim, so a value with a trailing !important or space compares -- and
       the space after the colon, which is how nearly every sheet written
       by hand puts it (`display: none`). Kept, every keyword read by its
       first letters missed: display, position, float, text-align and the
       rest were ignored on any sheet that was not minified, and a page's
       closed menus, fixed bars and floats were laid out as plain blocks. */
    while (vlen > 0 && css_space(v[0])) { v++; vlen--; }
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

    /* A custom property, kept whole as "--name:value" for the layout to
       gather (custom properties are case-sensitive, so the name as written). */
    if (nlen > 2 && name[0] == '-' && name[1] == '-') {
        char both[2048];
        int k = 0;
        for (int i = 0; i < nlen && k < 200; i++) both[k++] = name[i];
        both[k++] = ':';
        for (int i = 0; i < vlen && k < (int)sizeof(both) - 1; i++) both[k++] = v[i];
        css_add(s, P_CUSTOM, both, k);
        return;
    }
    /* A shorthand whose parts are in a variable cannot be split until the
       variable is known, which is when the page is laid out. */
    int has_var = 0;
    for (int i = 0; i + 3 < vlen; i++)
        if (v[i] == 'v' && v[i + 1] == 'a' && v[i + 2] == 'r' && v[i + 3] == '(') { has_var = 1; break; }
    if (has_var && (w_same(lower, "margin") || w_same(lower, "padding") || w_same(lower, "border")
                    || w_same(lower, "border-width") || w_same(lower, "border-top")
                    || w_same(lower, "border-right") || w_same(lower, "border-bottom")
                    || w_same(lower, "border-left") || w_same(lower, "background")
                    || w_same(lower, "font") || w_same(lower, "inset") || w_same(lower, "flex")
                    || w_same(lower, "border-radius") || w_same(lower, "list-style"))) {
        char both[2048];
        int k = 0;
        for (int i = 0; lower[i] && k < 40; i++) both[k++] = lower[i];
        both[k++] = ':';
        for (int i = 0; i < vlen && k < (int)sizeof(both) - 1; i++) both[k++] = v[i];
        css_add(s, P_DEFER, both, k);
        return;
    }

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

/* --- media queries -------------------------------------------------------
 *
 * Every query for a screen used to be opened whatever it asked, so a page's
 * rules for a phone, a tablet and a wide desktop all applied at once and the
 * last one written won -- usually the desktop's, at widths this window does
 * not have, with the phone's rules for hiding things mixed in. Now the widths
 * are kept with each rule and compared with the window's when the page is
 * laid out (css_collect_chain), so a page gets the layout written for its
 * size, and a resize changes which that is.
 *
 * What a query can ask besides width is answered once, here, as this machine
 * would: a screen, not print; light, not dark; a mouse that hovers; one pixel
 * to a pixel; wider than tall. Anything not known applies. */
#define CSS_MQ_BIG 30000

static inline int css_mq_px(const char *v, int n) {
    char tmp[32];
    int k = 0;
    while (n > 0 && css_space(*v)) { v++; n--; }
    for (; k < n && k < 31; k++) tmp[k] = v[k];
    tmp[k] = 0;
    clen L = css_len(tmp);
    if (L.unit == U_EM || L.unit == U_REM) return L.v * 16 / 100;
    if (L.unit == U_AUTO) return -1;
    return L.v / 100;
}

static inline int css_mq_word(const char *p, int n, const char *w) {
    for (int k = 0; w[k]; k++) if (k >= n || w_lower(p[k]) != w[k]) return 0;
    return 1;
}

/* One comma-separated part: whether it can apply at all, and its bounds. */
static inline int css_mq_part(const char *p, int n, int *lo, int *hi) {
    *lo = -1; *hi = -1;
    int never = 0, negate = 0, first = 1;
    int i = 0;
    while (i < n) {
        while (i < n && css_space(p[i])) i++;
        if (i >= n) break;
        if (p[i] == '(') {
            int depth = 1, st = ++i;
            while (i < n && depth) { if (p[i] == '(') depth++; else if (p[i] == ')') depth--; i++; }
            int en = depth ? i : i - 1;                  /* the closing bracket */
            const char *f = p + st;
            int fl = en - st;
            int colon = -1, cmp = -1;
            for (int k = 0; k < fl; k++) {
                if (f[k] == ':' && colon < 0) colon = k;
                if ((f[k] == '<' || f[k] == '>') && cmp < 0) cmp = k;
            }
            if (colon >= 0) {
                const char *v = f + colon + 1;
                int vl = fl - colon - 1;
                int px = css_mq_px(v, vl);
                while (vl > 0 && css_space(*v)) { v++; vl--; }
                if (css_mq_word(f, colon, "min-width") || css_mq_word(f, colon, "min-device-width")) {
                    if (px > *lo) *lo = px;
                } else if (css_mq_word(f, colon, "max-width") || css_mq_word(f, colon, "max-device-width")) {
                    if (px >= 0 && (*hi < 0 || px < *hi)) *hi = px;
                } else if (css_mq_word(f, colon, "prefers-color-scheme")) {
                    if (css_mq_word(v, vl, "dark")) never = 1;
                } else if (css_mq_word(f, colon, "hover") || css_mq_word(f, colon, "any-hover")) {
                    if (css_mq_word(v, vl, "none")) never = 1;
                } else if (css_mq_word(f, colon, "pointer") || css_mq_word(f, colon, "any-pointer")) {
                    if (css_mq_word(v, vl, "coarse") || css_mq_word(v, vl, "none")) never = 1;
                } else if (css_mq_word(f, colon, "orientation")) {
                    if (css_mq_word(v, vl, "portrait")) never = 1;
                } else if (css_mq_word(f, colon, "min-resolution")
                           || css_mq_word(f, colon, "-webkit-min-device-pixel-ratio")
                           || css_mq_word(f, colon, "min--moz-device-pixel-ratio")) {
                    /* 2dppx, 192dpi, 1.5: anything past one pixel a pixel. */
                    int whole = 0, k = 0;
                    while (k < vl && v[k] >= '0' && v[k] <= '9') whole = whole * 10 + (v[k++] - '0');
                    int frac = k + 1 < vl && v[k] == '.' && v[k + 1] > '0';
                    int dpi = 0;
                    for (int q = 0; q + 3 <= vl; q++) if (css_mq_word(v + q, 3, "dpi")) dpi = 1;
                    if (dpi) { if (whole > 96) never = 1; }
                    else if (whole > 1 || (whole == 1 && frac)) never = 1;
                } else if (css_mq_word(f, colon, "display-mode")) {
                    if (!css_mq_word(v, vl, "browser")) never = 1;
                } else if (css_mq_word(f, colon, "forced-colors") || css_mq_word(f, colon, "prefers-contrast")) {
                    if (css_mq_word(v, vl, "active") || css_mq_word(v, vl, "more")) never = 1;
                }
            } else if (cmp >= 0) {
                /* width >= 600px, width < 40em, 600px <= width */
                int wat = -1;
                for (int k = 0; k + 5 <= fl; k++)
                    if (css_mq_word(f + k, 5, "width") && (k == 0 || (!css_ident(f[k - 1]) && f[k - 1] != '-'))) {
                        wat = k;
                        break;
                    }
                if (wat >= 0) {
                    int gt = f[cmp] == '>', eq = cmp + 1 < fl && f[cmp + 1] == '=';
                    int left = wat < cmp;
                    const char *v = left ? f + cmp + 1 + eq : f;
                    int vl = left ? fl - (cmp + 1 + eq) : cmp;
                    int px = css_mq_px(v, vl);
                    if (px >= 0) {
                        /* width > v is a floor; v > width is a ceiling. */
                        int floor = left ? gt : !gt;
                        if (floor) { int b = px + (eq ? 0 : 1); if (b > *lo) *lo = b; }
                        else { int b = px - (eq ? 0 : 1); if (*hi < 0 || b < *hi) *hi = b; }
                    }
                }
            }
            first = 0;
            continue;
        }
        int st = i;
        while (i < n && !css_space(p[i]) && p[i] != '(') i++;
        const char *w = p + st;
        int wl = i - st;
        if (first && wl == 3 && css_mq_word(w, 3, "not")) negate = 1;
        else if (wl == 3 && css_mq_word(w, 3, "and")) { }
        else if (wl == 4 && css_mq_word(w, 4, "only")) { }
        else if ((wl == 6 && css_mq_word(w, 6, "screen")) || (wl == 3 && css_mq_word(w, 3, "all"))) { }
        else if (wl > 0 && css_ident(w[0])) never = 1;    /* print, speech, tv... */
        first = 0;
    }
    if (negate) {
        if (never) { *lo = *hi = -1; return 1; }
        if (*lo >= 0 && *hi < 0) { *hi = *lo - 1; *lo = -1; return 1; }
        if (*hi >= 0 && *lo < 0) { *lo = *hi + 1; *hi = -1; return 1; }
        return 0;
    }
    return !never;
}

/* A whole query: 0 when it can never apply, else the widths it covers. A
   list of several is taken as the span of them all, which is exact for the
   lists pages write (a phone query and a narrow-window query together). */
static inline int css_mq(const char *p, int n, int *lo, int *hi) {
    int any = 0, unbounded = 0, ulo = CSS_MQ_BIG, uhi = -1;
    int i = 0;
    while (i <= n) {
        int st = i, depth = 0;
        while (i < n && (p[i] != ',' || depth)) {
            if (p[i] == '(') depth++;
            else if (p[i] == ')') depth--;
            i++;
        }
        int plo, phi;
        if (css_mq_part(p + st, i - st, &plo, &phi)) {
            any = 1;
            if (plo < 0 && phi < 0) unbounded = 1;
            if ((plo < 0 ? 0 : plo) < ulo) ulo = plo < 0 ? 0 : plo;
            if ((phi < 0 ? CSS_MQ_BIG : phi) > uhi) uhi = phi < 0 ? CSS_MQ_BIG : phi;
        }
        i++;
    }
    if (!any) return 0;
    if (unbounded) { *lo = *hi = -1; return 1; }
    *lo = ulo > 0 ? ulo : -1;
    *hi = uhi < CSS_MQ_BIG ? uhi : -1;
    return 1;
}

/* Whether @supports would say yes here. Nearly everything a page asks
   about is read or harmlessly ignored, so the answer is yes -- except what
   this browser lays out differently or not at all: grid (and subgrid),
   container queries and :has(). A page asks precisely so
   that it can do something else where the answer is no, and opening the
   block anyway gave an encyclopaedia's grid layout, whose columns are never
   made, instead of the one it writes for a browser without grid. A leading
   `not` turns the answer round; `and` and `or` are taken as a whole. */
static inline int css_supports(const char *p, int n) {
    int i = 0;
    while (i < n && css_space(p[i])) i++;
    int negate = i + 3 < n && css_named(p + i, 3, "not") && (css_space(p[i + 3]) || p[i + 3] == '(');
    int no = 0;
    for (int k = 0; k < n; k++) {
        if (css_named(p + k, (k + 4 <= n ? 4 : n - k), "grid")) no = 1;
        if (css_named(p + k, (k + 9 <= n ? 9 : n - k), "container")) no = 1;
        if (k + 4 <= n && p[k] == ':' && css_named(p + k + 1, 3, "has")) no = 1;
    }
    return negate ? no : !no;
}

/* Skips an at-rule. Media queries are opened rather than skipped when they
   can apply to this screen, and the widths they are for are handed back
   (css_mq); @supports is opened when it would say yes (css_supports). */
static inline int css_at_rule(const char *p, int len, int i, int *open_body,
                              int *lo, int *hi) {
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

    *lo = *hi = -1;
    if (supports && css_supports(p + qstart, i - qstart)) { *open_body = 1; return i + 1; }
    if (media && css_mq(p + qstart, i - qstart, lo, hi)) { *open_body = 1; return i + 1; }

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

/* A sheet, all of it for the window widths lo to hi (-1 for no bound):
   what the media attribute of the link or style element that carried it
   said, since a sheet can be meant for print or for a phone as a whole. */
static inline void css_parse_in(csheet *s, const char *p, int len, int lo, int hi) {
    int i = 0;
    int nested = 0;
    /* The widths each open query allows, the outer ones included. */
    short mlo[17], mhi[17];
    mlo[0] = (short)lo;
    mhi[0] = (short)hi;

    while (i < len) {
        i = css_skip(p, len, i);
        if (i >= len) break;

        if (p[i] == '}') { if (nested > 0) nested--; i++; continue; }
        if (p[i] == '@') {
            int open_body = 0, lo = -1, hi = -1;
            i = css_at_rule(p, len, i, &open_body, &lo, &hi);
            if (open_body) {
                int up = nested < 16 ? nested : 16;
                nested++;
                int at = nested < 16 ? nested : 16;
                short plo = mlo[up], phi = mhi[up];
                if (lo > plo) plo = (short)lo;
                if (hi >= 0 && (phi < 0 || hi < phi)) phi = (short)hi;
                mlo[at] = plo;
                mhi[at] = phi;
            }
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
            r->mq_lo = mlo[nested < 16 ? nested : 16];
            r->mq_hi = mhi[nested < 16 ? nested : 16];
            s->nrules++;
        }
        (void)first_rule;
    }
}

static inline void css_parse(csheet *s, const char *p, int len) {
    css_parse_in(s, p, len, -1, -1);
}

/* The next @import at the head of a sheet, from *at: its address into href
   and the window widths its media query allows; 0 when the imports are
   over (they can only come first, after @charset and comments), -1 for one
   whose media can never apply, which the caller skips. */
static inline int css_next_import(const char *css, int len, int *at, char *href, int cap,
                                  int *lo, int *hi) {
    int i = *at;
    for (;;) {
        while (i < len && css_space(css[i])) i++;
        if (i + 1 < len && css[i] == '/' && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(css[i] == '*' && css[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        if (i + 8 < len && w_starts_fold(css + i, "@charset")) {
            while (i < len && css[i] != ';') i++;
            i++;
            continue;
        }
        break;
    }
    if (!(i + 7 < len && w_starts_fold(css + i, "@import"))) { *at = i; return 0; }
    i += 7;
    while (i < len && css_space(css[i])) i++;
    int wrapped = 0;
    if (i + 4 < len && w_starts_fold(css + i, "url(")) { i += 4; wrapped = 1; }
    while (i < len && css_space(css[i])) i++;
    char q = (i < len && (css[i] == '"' || css[i] == '\'')) ? css[i++] : 0;
    int n = 0;
    while (i < len && n < cap - 1 && css[i] != ';'
           && (q ? css[i] != q : (css[i] != ')' && !css_space(css[i]))))
        href[n++] = css[i++];
    href[n] = 0;
    if (q && i < len && css[i] == q) i++;
    while (i < len && css_space(css[i])) i++;
    if (wrapped && i < len && css[i] == ')') i++;
    int st = i;
    while (i < len && css[i] != ';') i++;
    *lo = *hi = -1;
    int keep = 1;
    int m = st;
    while (m < i && css_space(css[m])) m++;
    if (m < i) keep = css_mq(css + m, i - m, lo, hi);
    *at = i < len ? i + 1 : i;
    if (!n) return css_next_import(css, len, at, href, cap, lo, hi);
    return keep ? 1 : -1;
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

/* How many element siblings come before this one (or after it), of its
   own type only when asked. */
static inline int css_sibling_count(const ddoc *d, int el, int same_type, int after) {
    int n = 0;
    const dnode *me = &d->nodes[el];
    for (int k = after ? me->next : me->prev; k >= 0; k = after ? d->nodes[k].next : d->nodes[k].prev) {
        const dnode *o = &d->nodes[k];
        if (o->kind != DN_ELEMENT) continue;
        if (same_type) {
            if (o->tag != me->tag) continue;
            if (me->tag == T_OTHER && (o->text < 0 || me->text < 0
                || !w_same(d->arena + o->text, d->arena + me->text))) continue;
        }
        n++;
    }
    return n;
}

/* An attribute's value against a selector's test (css_parse_compound). */
static inline int css_attr_test(const char *have, const char *tv) {
    char op = tv[0];
    int fold = tv[1] == 'i';
    const char *want = tv + 2;
    if (op == '?') return 1;
    int hl = w_len(have), wl = w_len(want);
#define CSS_EQ(a, b) (fold ? w_lower(a) == w_lower(b) : (a) == (b))
    if (op == '=' || op == '|') {
        int k = 0;
        while (k < wl && k < hl && CSS_EQ(have[k], want[k])) k++;
        if (k != wl) return 0;
        return hl == wl || (op == '|' && have[wl] == '-');
    }
    if (!wl) return 0;
    if (op == '^' || op == '$') {
        if (hl < wl) return 0;
        const char *h = op == '^' ? have : have + hl - wl;
        for (int k = 0; k < wl; k++) if (!CSS_EQ(h[k], want[k])) return 0;
        return 1;
    }
    for (int st = 0; st + wl <= hl; st++) {
        if (op == '~' && st > 0 && !css_space(have[st - 1])) continue;
        int k = 0;
        while (k < wl && CSS_EQ(have[st + k], want[k])) k++;
        if (k == wl && (op != '~' || st + wl == hl || css_space(have[st + wl]))) return 1;
    }
    return 0;
#undef CSS_EQ
}

static inline int css_part_matches(const csheet *s, const ddoc *d, int el,
                                   const csel *c, const cmatch *m) {
    const dnode *n = &d->nodes[el];
    if (n->kind != DN_ELEMENT) return 0;
    if (c->tag >= 0 && n->tag != c->tag) return 0;
    if (c->tname >= 0) {
        if (n->tag != T_OTHER || n->text < 0) return 0;
        const char *have = d->arena + n->text, *want = s->text + c->tname;
        int k = 0;
        for (; have[k] && want[k]; k++) if (w_lower(have[k]) != want[k]) return 0;
        if (have[k] || want[k]) return 0;
    }
    if (c->id >= 0) {
        const char *v = dom_attr(d, el, "id");
        if (!v || !w_same(v, s->text + c->id)) return 0;
    }
    for (int i = 0; i < c->ncls; i++) {
        int t = c->cls[i];
        if (t < 0) {
            const char *name = s->text + (-t - 2);
            const char *have = dom_attr(d, el, name);
            if (!have) return 0;
            if (!css_attr_test(have, name + w_len(name) + 1)) return 0;
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
        case PS_LAST_CHILD: if (css_sibling_count(d, el, 0, 1) != 0) return 0; break;
        case PS_ONLY_CHILD:
            if (css_sibling_count(d, el, 0, 0) || css_sibling_count(d, el, 0, 1)) return 0;
            break;
        case PS_FIRST_OF_TYPE: if (css_sibling_count(d, el, 1, 0) != 0) return 0; break;
        case PS_LAST_OF_TYPE: if (css_sibling_count(d, el, 1, 1) != 0) return 0; break;
        case PS_ONLY_OF_TYPE:
            if (css_sibling_count(d, el, 1, 0) || css_sibling_count(d, el, 1, 1)) return 0;
            break;
        case PS_EMPTY:
            for (int k = n->first; k >= 0; k = d->nodes[k].next) {
                if (d->nodes[k].kind == DN_ELEMENT) return 0;
                if (d->nodes[k].text >= 0 && d->arena[d->nodes[k].text]) return 0;
            }
            break;
        case PS_CHECKED:
            if (!dom_attr(d, el, "checked") && !dom_attr(d, el, "selected")) return 0;
            break;
        case PS_DISABLED: if (!dom_attr(d, el, "disabled")) return 0; break;
        case PS_ENABLED:
            if (dom_attr(d, el, "disabled")) return 0;
            if (n->tag != T_INPUT && n->tag != T_BUTTON && n->tag != T_SELECT
                && n->tag != T_TEXTAREA && n->tag != T_OPTION) return 0;
            break;
        case PS_NEVER: return 0;
        case PS_NTH_CHILD: case PS_NTH_OF_TYPE: case PS_NTH_LAST_CHILD: {
            int pos = 1 + css_sibling_count(d, el, c->pseudo == PS_NTH_OF_TYPE,
                                            c->pseudo == PS_NTH_LAST_CHILD);
            int a = c->nth_a, b = c->nth_b;
            if (a == 0) { if (pos != b) return 0; }
            else { int k = pos - b; if (k % a != 0 || k / a < 0) return 0; }
            break;
        }
        default: break;
    }
    if (c->neg >= 0 && css_part_matches(s, d, el, &s->negs[c->neg], m)) return 0;
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
        } else if (join == CB_NEXT || join == CB_LATER) {
            int q = d->nodes[at].prev;
            for (;;) {
                while (q >= 0 && d->nodes[q].kind != DN_ELEMENT) q = d->nodes[q].prev;
                if (q < 0) return 0;
                if (css_part_matches(s, d, q, want, m)) break;
                if (join == CB_NEXT) return 0;
                q = d->nodes[q].prev;
            }
            at = q;
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
    st->spacing = -1;
    st->valign = VA_BASELINE;
    st->clip = st->gone = 0;
    st->floated = st->clear = 0;
    st->masked = 0;
    st->ink_none = 0;
    st->tx_px = st->ty_px = st->tx_pct = st->ty_pct = 0;
    st->grid_cols = st->grid_areas = st->garea = st->grid_auto = 0;
    st->gflow_col = 0;
    st->width_pct = 0;
    st->vars = -1;
    st->gspan = 0;
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
    child->spacing = parent->spacing;
    child->vars = parent->vars;
    child->ink_none = parent->ink_none;
}

/* One length of a translation: pixels and a percentage of the box, from a
   length, a percentage, or calc() of a sum of them. */
static inline const char *css_tlen(const char *v, int font_px, int root_px, int *px, int *pct) {
    *px = *pct = 0;
    while (*v == ' ') v++;
    int calc = w_starts_fold(v, "calc(");
    if (calc) v += 5;
    int sign = 1;
    for (;;) {
        while (*v == ' ') v++;
        if (*v == '-' && (v[1] < '0' || v[1] > '9') && v[1] != '.') { sign = -sign; v++; continue; }
        char tmp[32];
        int k = 0;
        while (*v && *v != ',' && *v != ')' && *v != ' ' && k < 31) tmp[k++] = *v++;
        tmp[k] = 0;
        clen L = css_len(tmp);
        if (L.unit == U_PCT) *pct += sign * L.v / 100;
        else if (L.unit != U_AUTO) *px += sign * css_px(L, font_px, root_px, 0);
        while (*v == ' ') v++;
        if (!calc) break;
        if (*v == '+') { sign = 1; v++; continue; }
        if (*v == '-') { sign = -1; v++; continue; }
        if (*v == ')') v++;
        break;
    }
    return v;
}

/* translate, translateX, translateY and translate3d, anywhere in the list. */
static inline void css_translate(const char *v, cstyle *st, int root_px) {
    for (const char *q = v; *q; q++) {
        if (!w_starts_fold(q, "translate")) continue;
        const char *a = q + 9;
        int which = 0;                                 /* both, x only, y only */
        if (w_lower(*a) == 'x') { which = 1; a++; }
        else if (w_lower(*a) == 'y') { which = 2; a++; }
        else if (w_starts_fold(a, "3d")) a += 2;
        if (*a != '(') continue;
        a++;
        int px, pct;
        a = css_tlen(a, st->font_px, root_px, &px, &pct);
        if (which == 2) { st->ty_px = (short)px; st->ty_pct = (short)pct; }
        else { st->tx_px = (short)px; st->tx_pct = (short)pct; }
        if (which == 0) {
            while (*a == ' ') a++;
            if (*a == ',') {
                a = css_tlen(a + 1, st->font_px, root_px, &px, &pct);
                st->ty_px = (short)px;
                st->ty_pct = (short)pct;
            }
        }
        q = a;
        if (!*q) break;
    }
}

/* --- calc() and its kin ---------------------------------------------------
 *
 * A length worked out when the page is laid out: calc() of sums and
 * products of lengths and numbers, and min(), max() and clamp(), which is
 * how a modern page says "as wide as the window but no wider than 1200px"
 * and "this big, growing with the window between these two". Each read as
 * nothing at all before, so the box took its default. */
typedef struct { const char *p; int font_px, root_px, pct_of, bad; } ccalc;

static inline int css_calc_sum(ccalc *c, int *is_num);

static inline void css_calc_space(ccalc *c) { while (*c->p == ' ') c->p++; }

/* One operand; its value in pixels (hundredths of one), or a plain number
   (hundredths) when *is_num. */
static inline int css_calc_atom(ccalc *c, int *is_num) {
    css_calc_space(c);
    *is_num = 0;
    int fn = w_starts_fold(c->p, "calc(") ? 5 : w_starts_fold(c->p, "min(") ? 4
           : w_starts_fold(c->p, "max(") ? 4 : w_starts_fold(c->p, "clamp(") ? 6 : 0;
    if (fn || *c->p == '(') {
        int kind = fn == 5 || !fn ? 0 : w_lower(c->p[1]) == 'i' ? 1 : w_lower(c->p[1]) == 'a' ? 2 : 3;
        c->p += fn ? fn : 1;
        int n = 0, vals[3] = { 0, 0, 0 }, nums = 1;
        for (;;) {
            int in;
            int v = css_calc_sum(c, &in);
            if (n < 3) vals[n] = v;
            n++;
            if (!in) nums = 0;
            css_calc_space(c);
            if (*c->p == ',') { c->p++; continue; }
            break;
        }
        if (*c->p == ')') c->p++; else c->bad = 1;
        *is_num = nums;
        if (kind == 0) return vals[0];
        if (kind == 3) {                           /* clamp(lo, want, hi) */
            int v = vals[1];
            if (v > vals[2]) v = vals[2];
            if (v < vals[0]) v = vals[0];
            return v;
        }
        int m = vals[0];
        for (int i = 1; i < n && i < 3; i++) m = kind == 1 ? (vals[i] < m ? vals[i] : m) : (vals[i] > m ? vals[i] : m);
        return m;
    }
    char tok[32];
    int k = 0;
    if (*c->p == '-' || *c->p == '+') tok[k++] = *c->p++;
    while (*c->p && k < 31 && ((*c->p >= '0' && *c->p <= '9') || *c->p == '.'
                               || (*c->p >= 'a' && *c->p <= 'z') || (*c->p >= 'A' && *c->p <= 'Z')
                               || *c->p == '%'))
        tok[k++] = *c->p++;
    tok[k] = 0;
    if (!k) { c->bad = 1; return 0; }
    clen L = css_len(tok);
    int unitless = 1;
    for (int i = 0; tok[i]; i++) if ((tok[i] >= 'a' && tok[i] <= 'z') || (tok[i] >= 'A' && tok[i] <= 'Z') || tok[i] == '%') unitless = 0;
    if (unitless) { *is_num = 1; return L.v; }
    if (L.unit == U_PCT) return c->pct_of >= 0 ? (int)((long long)L.v * c->pct_of / 100) : 0;
    if (L.unit == U_AUTO) { c->bad = 1; return 0; }
    if (L.unit == U_PX) return L.v;
    return css_px(L, c->font_px, c->root_px, c->pct_of) * 100;
}

static inline int css_calc_product(ccalc *c, int *is_num) {
    int v = css_calc_atom(c, is_num);
    for (;;) {
        css_calc_space(c);
        char op = *c->p;
        if (op != '*' && op != '/') return v;
        c->p++;
        int n2;
        int w = css_calc_atom(c, &n2);
        if (op == '*') {
            if (n2) v = (int)((long long)v * w / 100);
            else if (*is_num) { v = (int)((long long)w * v / 100); *is_num = 0; }
            else c->bad = 1;
        } else {
            if (n2 && w) v = (int)((long long)v * 100 / w);
            else c->bad = 1;
        }
    }
}

static inline int css_calc_sum(ccalc *c, int *is_num) {
    int v = css_calc_product(c, is_num);
    for (;;) {
        css_calc_space(c);
        char op = *c->p;
        if (op != '+' && op != '-') return v;
        c->p++;
        int n2;
        int w = css_calc_product(c, &n2);
        v = op == '+' ? v + w : v - w;
    }
}

/* A length, with calc(), min(), max() and clamp() worked out: as a clen in
   pixels, or whatever css_len made of anything else. */
static inline clen css_len_at(const char *v, int font_px, int root_px, int pct_of) {
    while (*v == ' ') v++;
    if (w_starts_fold(v, "calc(") || w_starts_fold(v, "min(") || w_starts_fold(v, "max(")
        || w_starts_fold(v, "clamp(")) {
        ccalc c = { v, font_px, root_px, pct_of, 0 };
        int is_num;
        int px = css_calc_atom(&c, &is_num);
        clen L;
        if (c.bad) { L.v = 0; L.unit = U_AUTO; return L; }
        L.v = px;
        L.unit = U_PX;
        return L;
    }
    return css_len(v);
}

static inline void css_apply_v(int prop, const char *v, cstyle *st, int root_px, int pct_of);

static inline void css_apply(const csheet *s, const cdecl *dcl, cstyle *st,
                             int root_px, int pct_of) {
    css_apply_v(dcl->prop & ~CSS_HAS_VAR, s->text + dcl->value, st, root_px, pct_of);
}

static inline void css_apply_v(int prop, const char *v, cstyle *st, int root_px, int pct_of) {
    switch (prop) {
        case P_COLOR:
            if (w_same_fold(v, "currentcolor") || w_same_fold(v, "inherit")) break;
            if (css_color(v, &st->color)) st->ink_none = css_last_alpha < 13;
            break;
        case P_BACKGROUND: {
            u32 c;
            if (w_same_fold(v, "currentcolor")) { st->background = st->color; st->has_bg = 1; break; }
            if (!css_color(v, &c)) break;
            /* (Nearly) transparent is no background at all. */
            if (css_last_alpha < 13) { st->has_bg = 0; break; }
            st->background = c;
            st->has_bg = 1;
            break;
        }
        case P_BORDER_COLOR: css_color(v, &st->border_color); break;
        case P_DISPLAY:
            if (w_starts_fold(v, "none")) st->display = D_NONE;
            else if (w_starts_fold(v, "inline-block")) st->display = D_INLINE_BLOCK;
            else if (w_starts_fold(v, "inline-flex")) st->display = D_INLINE_FLEX;
            else if (w_starts_fold(v, "inline-table")) st->display = D_TABLE;
            else if (w_starts_fold(v, "inline-grid")) st->display = D_INLINE_BLOCK;
            else if (w_starts_fold(v, "inline")) st->display = D_INLINE;
            else if (w_starts_fold(v, "list-item")) st->display = D_LIST_ITEM;
            else if (w_starts_fold(v, "table-cell")) st->display = D_TABLE_CELL;
            else if (w_starts_fold(v, "table-row-group")
                     || w_starts_fold(v, "table-header-group")
                     || w_starts_fold(v, "table-footer-group")) st->display = D_TABLE_GROUP;
            else if (w_starts_fold(v, "table-row")) st->display = D_TABLE_ROW;
            else if (w_starts_fold(v, "table-column")) st->display = D_NONE;
            else if (w_starts_fold(v, "table-caption")) st->display = D_BLOCK;
            else if (w_starts_fold(v, "table") || w_starts_fold(v, "inline-table"))
                st->display = D_TABLE;
            else if (w_starts_fold(v, "flex")) st->display = D_FLEX;
            else if (w_starts_fold(v, "grid")) st->display = D_GRID;
            else if (w_starts_fold(v, "contents")) st->display = D_CONTENTS;
            else st->display = D_BLOCK;
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
            clen L = css_len_at(v, st->font_px, root_px, pct_of);
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
            clen L = css_len_at(v, st->font_px, root_px, st->font_px);
            int px = css_px(L, st->font_px, root_px, st->font_px);
            if (px > 0) st->font_px = (short)(px > 96 ? 96 : (px < 7 ? 7 : px));
            /* Nought is how a page hides the words in something it draws
               another way, and the gaps between inline-blocks. */
            else if (px == 0 && L.unit != U_AUTO) st->font_px = 0;
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

        case P_FLOAT:
            if (w_starts_fold(v, "left") || w_starts_fold(v, "inline-start")) st->floated = 1;
            else if (w_starts_fold(v, "right") || w_starts_fold(v, "inline-end")) st->floated = 2;
            else st->floated = 0;
            break;
        case P_CLEAR:
            if (w_starts_fold(v, "both")) st->clear = 3;
            else if (w_starts_fold(v, "left") || w_starts_fold(v, "inline-start")) st->clear = 1;
            else if (w_starts_fold(v, "right") || w_starts_fold(v, "inline-end")) st->clear = 2;
            else st->clear = 0;
            break;
        case P_MASK:
            st->masked = !w_starts_fold(v, "none");
            break;
        case P_GRID_AREAS:
            st->grid_areas = w_starts_fold(v, "none") ? 0 : v;
            break;
        case P_GRID_AREA:
            /* A name; a placement by line numbers is left to the flow. */
            st->garea = (v[0] >= '0' && v[0] <= '9') || w_starts_fold(v, "auto") ? 0 : v;
            break;
        case P_GRID_COLS:
            st->grid_cols = w_starts_fold(v, "none") ? 0 : v;
            break;
        case P_GRID_FLOW:
            st->gflow_col = (unsigned char)w_starts_fold(v, "column");
            break;
        case P_GRID_AUTO_COLS:
            st->grid_auto = w_starts_fold(v, "auto") ? 0 : v;
            break;
        case P_GRID_COLUMN: {
            /* span N, or 1 / -1 for the whole row; a line number alone is
               one column, placed where the next free one is. */
            st->gspan = 0;
            const char *q = v;
            while (*q == ' ') q++;
            if (w_starts_fold(q, "span")) {
                q += 4;
                while (*q == ' ') q++;
                int k = 0;
                while (*q >= '0' && *q <= '9') k = k * 10 + (*q++ - '0');
                st->gspan = (short)(k > 1 ? (k > 64 ? 64 : k) : 0);
                break;
            }
            for (const char *r = q; *r; r++) {
                if (*r != '/') continue;
                r++;
                while (*r == ' ') r++;
                if (r[0] == '-' && r[1] == '1') st->gspan = -1;
                else if (w_starts_fold(r, "span")) {
                    r += 4;
                    while (*r == ' ') r++;
                    int k = 0;
                    while (*r >= '0' && *r <= '9') k = k * 10 + (*r++ - '0');
                    st->gspan = (short)(k > 1 ? (k > 64 ? 64 : k) : 0);
                } else {
                    int a = 0, b = 0;
                    while (*q >= '0' && *q <= '9') a = a * 10 + (*q++ - '0');
                    while (*r >= '0' && *r <= '9') b = b * 10 + (*r++ - '0');
                    if (a > 0 && b > a + 1) st->gspan = (short)(b - a > 64 ? 64 : b - a);
                }
                break;
            }
            break;
        }
        case P_TRANSFORM:
            st->tx_px = st->ty_px = st->tx_pct = st->ty_pct = 0;
            css_translate(v, st, root_px);
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
        case P_VALIGN:
            if (w_starts_fold(v, "top") || w_starts_fold(v, "text-top")) st->valign = VA_TOP;
            else if (w_starts_fold(v, "middle")) st->valign = VA_MIDDLE;
            else if (w_starts_fold(v, "bottom") || w_starts_fold(v, "text-bottom"))
                st->valign = VA_BOTTOM;
            else st->valign = VA_BASELINE;
            break;
        case P_SPACING: {
            clen L = css_len(v);
            int px = css_px(L, st->font_px, root_px, pct_of);
            st->spacing = (short)(px < 0 ? 0 : (px > 64 ? 64 : px));
            break;
        }
        case P_COLLAPSE:
            /* Collapsed borders share the line between two cells, which is
               no room between them; separate is the default's two pixels. */
            if (w_starts_fold(v, "collapse")) st->spacing = 0;
            break;
        case P_OVERFLOW:
            /* 1 hides what overflows; 2 would scroll it, which nothing
               inside a page does here (layout.h wraps a scrolling row). */
            st->clip = (unsigned char)(w_starts_fold(v, "hidden") || w_starts_fold(v, "clip") ? 1
                     : w_starts_fold(v, "auto") || w_starts_fold(v, "scroll") ? 2 : 0);
            break;
        case P_CLIP: {
            /* clip: rect(0 0 0 0), rect(1px, 1px, 1px, 1px) and clip-path:
               inset(50%) are the three spellings of the visually hidden
               pattern; anything else is a shape this does not draw anyway. */
            const char *q = v;
            while (*q == ' ') q++;
            if (w_starts_fold(q, "inset(50%") || w_starts_fold(q, "inset(100%")) st->gone = 1;
            if (w_starts_fold(q, "rect(")) {
                int big = 0;
                for (const char *r = q + 5; *r && *r != ')'; r++) {
                    if (*r < '0' || *r > '9') continue;
                    int num = 0;
                    while (*r >= '0' && *r <= '9') { if (num < 1000) num = num * 10 + (*r - '0'); r++; }
                    if (num > 1) big = 1;
                    r--;
                }
                if (!big) st->gone = 1;
            }
            break;
        }
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
                st->line_h = (short)(st->font_px > 0 ? L.v * 100 / (st->font_px * 100) : 145);
            else if (L.unit != U_AUTO) st->line_h = (short)(L.v);   /* a number */
            if (st->line_h < 90) st->line_h = 90;
            if (st->line_h > 300) st->line_h = 300;
            break;
        }
        default: {
            clen L = css_len_at(v, st->font_px, root_px, pct_of);
            int px = css_px(L, st->font_px, root_px, pct_of);
            short *slot = 0;
            switch (prop) {
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
            if (prop == P_WIDTH) st->width_pct = (unsigned char)(L.unit == U_PCT);

            /* A percentage height is a percentage of the containing
               block's height, and in one pass down the tree that height is
               not known -- the box being measured is part of what decides
               it. CSS says such a height is auto, and auto is also the only
               answer available here.

               Resolved against the width instead, which is what this did,
               `.mw-logo{height:100%}` came out as tall as the column was
               wide. The logo is at the top of the page, so everything after
               it -- the whole article -- was laid out below the bottom of
               the window, and what was on screen was a blank page with one
               link at the top of it.

               Except where the parent was given a height of its own, which
               is then known before anything inside it is laid out: a logo
               at 100% of a 26 pixel heading is 26 pixels, and as auto it was
               drawn as wide as the page and as tall as its proportions made
               that. */
            if (L.unit == U_PCT) {
                if (prop == P_HEIGHT || prop == P_MIN_HEIGHT || prop == P_MAX_HEIGHT) {
                    *slot = css_parent_h > 0 ? (short)(L.v / 100 * css_parent_h / 100) : -1;
                    break;
                }
                if (prop == P_TOP || prop == P_BOTTOM) {
                    *slot = CSS_AUTO_OFF;
                    break;
                }
            }

            if (prop >= P_BORDER_T && prop <= P_BORDER_L) {
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
            /* And margins the same way: a negative margin is how a row
               reaches into its container's padding and how a sidebar is
               pulled back over the column it floated after, and clamping it
               to nothing put both somewhere else. Auto is kept apart from
               every number, which is what centring needs. */
            if ((prop >= P_TOP && prop <= P_LEFT) || prop == P_TEXT_INDENT
                || (prop >= P_MARGIN_T && prop <= P_MARGIN_L)) {
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

/* What an element's ancestors are, as a hundred and twenty eight bits: two
 * for each tag, id and class above it.
 *
 * The index finds the rules whose last part could be this element, and for
 * most pages that is enough. It is not enough for a sheet whose rules share
 * a last part and differ in what they want above it -- forty rules of the
 * form ".section-n .item .link" all filed under "link" -- because every link
 * then walked up its ancestors forty times to find out which section it was
 * in. Each rule carries the bits of everything its other parts insist on, and
 * a rule wanting a bit the element's ancestors do not have cannot match and is
 * not walked. Bits can collide, so a rule that passes is still walked; one
 * that fails is certain to have failed. */
typedef struct { unsigned long long w[2]; } cbloom;

static inline unsigned css_fnv(const char *p, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 16777619u; }
    return h;
}

static inline void css_bloom_add(cbloom *b, unsigned h) {
    unsigned a = h & 127, c = (h >> 7) & 127;
    b->w[a >> 6] |= 1ull << (a & 63);
    b->w[c >> 6] |= 1ull << (c & 63);
}

/* The keys, kept apart so a class named like a tag number is not the tag. */
static inline unsigned css_key_tag(int tag)               { return (unsigned)(tag + 1) * 2654435761u; }
static inline unsigned css_key_id(const char *p, int n)   { return css_fnv(p, n) ^ 0x5BD1E995u; }
static inline unsigned css_key_cls(const char *p, int n)  { return css_fnv(p, n); }

typedef struct {
    int by_tag[T_COUNT];
    int by_key[CSS_BUCKETS];
    int universal;
    int next[CSS_RULES];
    cbloom need[CSS_RULES];   /* what a rule's other parts want above it */
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

        /* Every part but the last has to be matched by an ancestor, by a
           child or a descendant join alike. */
        cbloom *b = &x->need[r];
        b->w[0] = b->w[1] = 0;
        for (int k = 0; k < rule->sel_n - 1; k++) {
            const csel *p = &s->sels[rule->sel_at + k];
            /* Only a part that is an ancestor of the element: one with a
               child or descendant join somewhere to its right. A sibling is
               not one, and requiring it among the ancestors refused every
               element the rule was for. */
            int anc = 0;
            for (int j = k + 1; j < rule->sel_n; j++) {
                int cb = s->sels[rule->sel_at + j].combinator;
                if (cb == CB_DESC || cb == CB_CHILD) anc = 1;
            }
            if (!anc) continue;
            if (p->tag >= 0) css_bloom_add(b, css_key_tag(p->tag));
            if (p->id >= 0) {
                const char *v = s->text + p->id;
                css_bloom_add(b, css_key_id(v, w_len(v)));
            }
            for (int i = 0; i < p->ncls; i++) {
                if (p->cls[i] < 0) continue;          /* an attribute, not a class */
                const char *v = s->text + p->cls[i];
                css_bloom_add(b, css_key_cls(v, w_len(v)));
            }
        }
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
                                     const cmatch *m, const cbloom *anc,
                                     chit *hits, int *n) {
    for (int r = head; r >= 0; r = x->next[r]) {
        if (*n >= CSS_HITS) return;
        if (anc && ((x->need[r].w[0] & ~anc->w[0]) | (x->need[r].w[1] & ~anc->w[1])))
            continue;                          /* wants an ancestor there is not */
        const crule *rule = &s->rules[r];
        if (rule->mq_lo >= 0 && css_view_w > 0 && css_view_w < rule->mq_lo) continue;
        if (rule->mq_hi >= 0 && css_view_w > 0 && css_view_w > rule->mq_hi) continue;
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
                              int el, const cmatch *m, const cbloom *anc,
                              chit *hits) {
    int n = 0;
    css_collect_chain(s, x, d, el, x->universal, m, anc, hits, &n);
    int tag = d->nodes[el].tag;
    if (tag > 0 && tag < T_COUNT)
        css_collect_chain(s, x, d, el, x->by_tag[tag], m, anc, hits, &n);

    const char *id = dom_attr(d, el, "id");
    if (id && *id)
        css_collect_chain(s, x, d, el, x->by_key[css_hash(id)], m, anc, hits, &n);

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
                                     m, anc, hits, &n);
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
    "table{display:table}"
    "tr{display:table-row}"
    "thead,tbody,tfoot{display:table-row-group}"
    "col,colgroup{display:none}"
    "td,th{display:table-cell;padding:1px}"
    "th{font-weight:bold;text-align:center}"
    "caption{display:block;text-align:center}"
    "[hidden]{display:none}"
    "sup,sub{font-size:0.8em}"
    "u,ins{text-decoration:underline}"
    "s,del,strike{text-decoration:line-through}"
    "cite,dfn,var,address{font-style:italic}"
    "mark{background:#fff2a8}"
    "details,summary{display:block}"
    "head,script,style,title,meta,link,noscript,template{display:none}"
    "button{display:inline-block;padding:5px 12px;background:#f2f3f5;"
        "border:1px #c9ccd1;border-radius:6px}"
    "input,textarea,select{display:inline-block;padding:4px 8px;"
        "background:#ffffff;border:1px #c9ccd1;border-radius:6px}"
    "img{display:inline-block}"
    /* Old markup that is still on the front of some very large sites.
       center is a block that centres what is in it, and it is how the
       plain version of more than one homepage is laid out to this day. */
    "center{display:block;text-align:center}"
    "iframe{display:none}";
