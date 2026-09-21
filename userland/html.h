/* Turning a page into a list of things to lay out.
 *
 * Not a tree. A tree is what a page means, and a flat run of open, text and
 * close is what a page is, which is all the layout below needs: it walks the
 * list with a stack of styles and never looks at a node twice. Building a
 * tree first would mean deciding what to do about the half of the web whose
 * tags do not nest, and every answer to that is a guess.
 *
 * What it does have to be right about is which bytes are text. A page is
 * full of markup that is not content, entities that are not the characters
 * they spell, and script that looks exactly like prose to anything reading
 * for angle brackets.
 */
#pragma once
#include "zelr.h"
#include "web.h"

#define HTML_NODES  9000
#define HTML_ARENA  (192 * 1024)

enum { N_TEXT = 1, N_OPEN, N_CLOSE };

enum {
    T_OTHER = 0,
    T_HTML, T_HEAD, T_BODY, T_TITLE,
    T_H1, T_H2, T_H3, T_H4, T_H5, T_H6,
    T_P, T_BR, T_HR, T_DIV, T_SPAN, T_A,
    T_UL, T_OL, T_LI, T_DL, T_DT, T_DD,
    T_B, T_STRONG, T_I, T_EM, T_CODE, T_PRE, T_KBD, T_SAMP, T_TT,
    T_BLOCKQUOTE, T_TABLE, T_TR, T_TD, T_TH, T_CAPTION,
    T_SCRIPT, T_STYLE, T_IMG, T_FORM, T_INPUT, T_BUTTON, T_TEXTAREA,
    T_SELECT, T_OPTION, T_NAV, T_HEADER, T_FOOTER, T_SECTION, T_ARTICLE,
    T_MAIN, T_ASIDE, T_FIGURE, T_FIGCAPTION, T_SMALL,
    T_NOSCRIPT, T_IFRAME, T_SVG, T_LABEL, T_META, T_LINK, T_HGROUP,

    /* Obsolete since 1999 and on the front page of Google, which is
       how the web actually is. Without it the element is unknown, and
       an unknown element is inline and centres nothing -- which is the
       whole reason that page rendered against the left margin. */
    T_CENTER,
    T_COUNT
};

typedef struct {
    short kind;
    short tag;
    int   at, len;        /* text, into the arena */
    int   href;           /* an anchor's target, into the arena, or -1 */
    int   alt;            /* an image's words, into the arena, or -1 */
} hnode;

typedef struct {
    hnode nodes[HTML_NODES];
    int   count;
    char  arena[HTML_ARENA];
    int   used;
    int   title;                 /* into the arena, or -1 */
    int   overflowed;            /* the page did not fit */
} hdoc;

/* --- the tag names -------------------------------------------------------
 *
 * In the same order as the enum above, which is checked once at the bottom
 * of this file rather than trusted. */
static const char *const HTML_TAGS[T_COUNT] = {
    "", "html", "head", "body", "title",
    "h1", "h2", "h3", "h4", "h5", "h6",
    "p", "br", "hr", "div", "span", "a",
    "ul", "ol", "li", "dl", "dt", "dd",
    "b", "strong", "i", "em", "code", "pre", "kbd", "samp", "tt",
    "blockquote", "table", "tr", "td", "th", "caption",
    "script", "style", "img", "form", "input", "button", "textarea",
    "select", "option", "nav", "header", "footer", "section", "article",
    "main", "aside", "figure", "figcaption", "small",
    "noscript", "iframe", "svg", "label", "meta", "link", "hgroup",
    "center",
};

static inline int html_tag_of(const char *name, int len) {
    for (int t = 1; t < T_COUNT; t++) {
        const char *s = HTML_TAGS[t];
        int i = 0;
        for (; i < len && s[i]; i++)
            if (w_lower(name[i]) != s[i]) break;
        if (i == len && !s[i]) return t;
    }
    return T_OTHER;
}

/* Tags that never have an end, so a close for them is never waited for. */
static inline int html_void(int t) {
    return t == T_BR || t == T_HR || t == T_IMG || t == T_INPUT
        || t == T_META || t == T_LINK;
}

/* --- characters ----------------------------------------------------------
 *
 * The font on this machine is the printable half of ASCII, which is a
 * decision made long before there was a browser and the right one for a
 * system that draws its own letterforms. A page, meanwhile, is full of
 * curly quotes, dashes of three different lengths and accented letters.
 *
 * Dropping them would take the punctuation out of every quoted sentence on
 * the web. Drawing a box for each would be worse. So each one is written the
 * way it was written before there was anything but ASCII to write it in, and
 * a reader loses the shape of the mark rather than the sense of it. */
static inline int html_fold_cp(unsigned cp, char *out) {
    static const char *const LATIN =
        "AAAAAAACEEEEIIIIDNOOOOOx0UUUUYPB"      /* c0..df */
        "aaaaaaaceeeeiiiionooooo/0uuuuypy";     /* e0..ff */

    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp >= 0xC0 && cp <= 0xFF) { out[0] = LATIN[cp - 0xC0]; return 1; }

    const char *s = 0;
    switch (cp) {
        case 0x00A0: s = " ";    break;         /* a space that does not break */
        case 0x00A9: s = "(c)";  break;
        case 0x00AB: s = "<<";   break;
        case 0x00AE: s = "(R)";  break;
        case 0x00B0: s = " deg"; break;
        case 0x00B7: s = "-";    break;
        case 0x00BB: s = ">>";   break;
        case 0x00BD: s = "1/2";  break;
        case 0x2010: case 0x2011: case 0x2012:
        case 0x2013: s = "-";    break;
        case 0x2014: case 0x2015: s = "--"; break;
        case 0x2018: case 0x2019: case 0x201B: s = "'"; break;
        case 0x201C: case 0x201D: case 0x201F: s = "\""; break;
        case 0x2022: case 0x25CF: case 0x25AA: s = "*"; break;
        case 0x2026: s = "...";  break;
        case 0x2039: s = "<";    break;
        case 0x203A: s = ">";    break;
        case 0x2122: s = "(TM)"; break;
        case 0x2190: s = "<-";   break;
        case 0x2192: s = "->";   break;
        case 0x2212: s = "-";    break;
        case 0x00D7: s = "x";    break;
        case 0x20AC: s = "EUR";  break;
        case 0x00A3: s = "GBP";  break;
        case 0x00A5: s = "JPY";  break;
        default: break;
    }
    if (!s) { out[0] = '?'; return 1; }

    int n = 0;
    while (s[n]) { out[n] = s[n]; n++; }
    return n;
}

/* One character out of the source, whatever it is written as. `used` comes
   back as how many bytes it took. */
static inline int html_char(const char *p, int left, int *used, char *out) {
    unsigned char c = (unsigned char)p[0];

    /* --- a numbered or named entity --------------------------------------- */
    if (c == '&' && left > 2) {
        int i = 1;
        unsigned cp = 0;
        if (p[1] == '#') {
            i = 2;
            int base = 10;
            if (i < left && (p[i] == 'x' || p[i] == 'X')) { base = 16; i++; }
            int digits = 0;
            while (i < left && digits < 8) {
                char d = p[i];
                int v;
                if (d >= '0' && d <= '9') v = d - '0';
                else if (base == 16 && d >= 'a' && d <= 'f') v = d - 'a' + 10;
                else if (base == 16 && d >= 'A' && d <= 'F') v = d - 'A' + 10;
                else break;
                cp = cp * (unsigned)base + (unsigned)v;
                i++;
                digits++;
            }
            if (!digits) { *used = 1; out[0] = '&'; return 1; }
        } else {
            static const struct { const char *name; unsigned cp; } NAMED[] = {
                { "amp",   '&' },  { "lt",    '<' },  { "gt",   '>' },
                { "quot",  '"' },  { "apos",  '\'' }, { "nbsp", 0x00A0 },
                { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 },
                { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
                { "ldquo", 0x201C }, { "rdquo", 0x201D },
                { "copy",  0x00A9 }, { "reg",  0x00AE }, { "trade", 0x2122 },
                { "bull",  0x2022 }, { "middot", 0x00B7 },
                { "laquo", 0x00AB }, { "raquo", 0x00BB },
                { "times", 0x00D7 }, { "deg",  0x00B0 },
                { "euro",  0x20AC }, { "pound", 0x00A3 }, { "yen", 0x00A5 },
                { "eacute", 0x00E9 }, { "egrave", 0x00E8 },
                { "agrave", 0x00E0 }, { "ccedil", 0x00E7 },
                { "uuml",  0x00FC }, { "ouml", 0x00F6 }, { "auml", 0x00E4 },
                { "szlig", 0x00DF }, { "ntilde", 0x00F1 },
                { "rarr",  0x2192 }, { "larr", 0x2190 },
                { 0, 0 }
            };
            int name_len = 0;
            while (1 + name_len < left && name_len < 12) {
                char d = p[1 + name_len];
                if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z')
                    || (d >= '0' && d <= '9')) name_len++;
                else break;
            }
            for (int k = 0; NAMED[k].name; k++) {
                int j = 0;
                while (j < name_len && NAMED[k].name[j]
                       && w_lower(p[1 + j]) == NAMED[k].name[j]) j++;
                if (j == name_len && !NAMED[k].name[j]) { cp = NAMED[k].cp; break; }
            }
            if (!cp) { *used = 1; out[0] = '&'; return 1; }
            i = 1 + name_len;
        }
        /* The semicolon is optional in practice and required in the spec, so
           it is taken when it is there and not insisted on. */
        if (i < left && p[i] == ';') i++;
        *used = i;
        return html_fold_cp(cp, out);
    }

    /* --- a character written as utf-8 ------------------------------------- */
    if (c >= 0x80) {
        unsigned cp = 0;
        int n = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; n = 4; }
        else { *used = 1; out[0] = '?'; return 1; }

        if (n > left) { *used = 1; out[0] = '?'; return 1; }
        for (int i = 1; i < n; i++) {
            unsigned char k = (unsigned char)p[i];
            if ((k & 0xC0) != 0x80) { *used = 1; out[0] = '?'; return 1; }
            cp = (cp << 6) | (k & 0x3Fu);
        }
        *used = n;
        return html_fold_cp(cp, out);
    }

    *used = 1;
    out[0] = (char)c;
    return 1;
}

/* --- the arena ----------------------------------------------------------- */

static inline int ha_start(hdoc *d) { return d->used; }

static inline void ha_put(hdoc *d, char c) {
    if (d->used < HTML_ARENA - 1) d->arena[d->used++] = c;
    else d->overflowed = 1;
}

static inline void ha_end(hdoc *d) { ha_put(d, 0); }

static inline hnode *ha_node(hdoc *d) {
    if (d->count >= HTML_NODES) { d->overflowed = 1; return 0; }
    hnode *n = &d->nodes[d->count++];
    n->kind = 0; n->tag = T_OTHER; n->at = 0; n->len = 0;
    n->href = -1; n->alt = -1;
    return n;
}

/* --- the parse ----------------------------------------------------------- */

/* Reads one attribute's value into the arena and returns where it starts.
   Quoted or not, because plenty of pages leave the quotes off. */
static inline int html_attr_value(hdoc *d, const char *p, int len, int *at) {
    int i = *at;
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n'
                       || p[i] == '\r')) i++;
    if (i >= len || p[i] != '=') { *at = i; return -1; }
    i++;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;

    char quote = 0;
    if (i < len && (p[i] == '"' || p[i] == '\'')) quote = p[i++];

    int start = ha_start(d);
    while (i < len) {
        char c = p[i];
        if (quote ? (c == quote) : (c == ' ' || c == '>' || c == '\t'
                                    || c == '\n' || c == '\r')) break;
        char out[8];
        int used = 1;
        int n = html_char(p + i, len - i, &used, out);
        for (int k = 0; k < n; k++) ha_put(d, out[k]);
        i += used;
    }
    if (quote && i < len) i++;
    ha_end(d);
    *at = i;
    return start;
}

/* Where the matching close tag for a raw element starts, or len. Used for
   script and style, whose contents are not markup and must not be read as
   any. A page with `if (a < b)` inside a script is otherwise a page with a
   tag called `b)` in it. */
static inline int html_raw_end(const char *p, int len, int at, const char *name) {
    int nlen = w_len(name);
    for (int i = at; i + nlen + 2 < len; i++) {
        if (p[i] != '<' || p[i + 1] != '/') continue;
        int j = 0;
        while (j < nlen && w_lower(p[i + 2 + j]) == name[j]) j++;
        if (j == nlen) return i;
    }
    return len;
}

static inline void html_parse(hdoc *d, const char *p, int len) {
    d->count = 0;
    d->used = 0;
    d->title = -1;
    d->overflowed = 0;

    int i = 0;
    int in_title = 0;

    while (i < len) {
        if (p[i] != '<') {
            /* --- text, up to the next tag --------------------------------- */
            int start = ha_start(d);
            int any = 0;
            while (i < len && p[i] != '<') {
                char out[8];
                int used = 1;
                int n = html_char(p + i, len - i, &used, out);
                for (int k = 0; k < n; k++) { ha_put(d, out[k]); any = 1; }
                i += used;
            }
            ha_end(d);
            if (!any) continue;

            hnode *n = ha_node(d);
            if (!n) return;
            n->kind = N_TEXT;
            n->at = start;
            n->len = d->used - start - 1;
            if (in_title && d->title < 0) d->title = start;
            continue;
        }

        /* --- a comment, or a doctype ------------------------------------- */
        if (i + 3 < len && p[i + 1] == '!' && p[i + 2] == '-' && p[i + 3] == '-') {
            i += 4;
            while (i + 2 < len && !(p[i] == '-' && p[i + 1] == '-'
                                    && p[i + 2] == '>')) i++;
            i = i + 3 < len ? i + 3 : len;
            continue;
        }
        if (i + 1 < len && (p[i + 1] == '!' || p[i + 1] == '?')) {
            while (i < len && p[i] != '>') i++;
            i++;
            continue;
        }

        /* --- a close tag -------------------------------------------------- */
        if (i + 1 < len && p[i + 1] == '/') {
            int at = i + 2;
            int start = at;
            while (at < len && p[at] != '>' && p[at] != ' ') at++;
            int tag = html_tag_of(p + start, at - start);
            while (at < len && p[at] != '>') at++;
            i = at + 1;

            if (tag == T_TITLE) in_title = 0;
            hnode *n = ha_node(d);
            if (!n) return;
            n->kind = N_CLOSE;
            n->tag = tag;
            continue;
        }

        /* --- an open tag -------------------------------------------------- */
        {
            int at = i + 1;
            int start = at;
            while (at < len && p[at] != '>' && p[at] != ' ' && p[at] != '\t'
                   && p[at] != '\n' && p[at] != '\r' && p[at] != '/') at++;
            int tag = html_tag_of(p + start, at - start);

            int href = -1, alt = -1;

            /* The attributes, of which two are wanted and the rest are read
               past. Read past rather than skipped to the next '>', because a
               value is allowed to contain one. */
            while (at < len && p[at] != '>') {
                while (at < len && (p[at] == ' ' || p[at] == '\t'
                                    || p[at] == '\n' || p[at] == '\r')) at++;
                if (at >= len || p[at] == '>') break;
                if (p[at] == '/') { at++; continue; }

                int nstart = at;
                while (at < len && p[at] != '=' && p[at] != '>' && p[at] != ' '
                       && p[at] != '\t' && p[at] != '\n' && p[at] != '\r') at++;
                int nlen = at - nstart;

                int which = 0;                 /* 1 href, 2 alt, 3 value */
                if (nlen == 4 && w_lower(p[nstart]) == 'h'
                    && w_lower(p[nstart + 1]) == 'r') which = 1;
                else if (nlen == 3 && w_lower(p[nstart]) == 'a'
                         && w_lower(p[nstart + 1]) == 'l') which = 2;
                else if (nlen == 5 && w_lower(p[nstart]) == 'v'
                         && w_lower(p[nstart + 1]) == 'a') which = 3;

                int before = at;
                int v = html_attr_value(d, p, len, &at);
                if (at == before && v < 0) at++;      /* an attribute with no value */

                if (v >= 0 && which == 1 && tag == T_A && href < 0) href = v;
                if (v >= 0 && which == 2 && tag == T_IMG && alt < 0) alt = v;
                if (v >= 0 && which == 3 && tag == T_INPUT && alt < 0) alt = v;
            }
            i = at + 1;

            if (tag == T_TITLE) in_title = 1;

            hnode *n = ha_node(d);
            if (!n) return;
            n->kind = N_OPEN;
            n->tag = tag;
            n->href = href;
            n->alt = alt;

            /* Script and style hold something that is not markup. */
            if (tag == T_SCRIPT || tag == T_STYLE || tag == T_SVG
                || tag == T_IFRAME || tag == T_TEXTAREA) {
                const char *name = HTML_TAGS[tag];
                int end = html_raw_end(p, len, i, name);
                i = end;
                hnode *c = ha_node(d);
                if (!c) return;
                c->kind = N_CLOSE;
                c->tag = tag;
                /* And step over the close tag itself. */
                while (i < len && p[i] != '>') i++;
                i++;
            } else if (html_void(tag)) {
                hnode *c = ha_node(d);
                if (!c) return;
                c->kind = N_CLOSE;
                c->tag = tag;
            }
        }
    }
}
