/* A web browser.
 *
 * It fetches a page over a TCP connection this system implements, parses the
 * HTML with the reader in html.h, lays the result out against the width of
 * its own window, and draws it with the letterforms in face.h. Nothing in
 * that sentence comes from anywhere else.
 *
 * What it is not is a rendering engine with a style sheet in it. There is no
 * CSS here, so a page is drawn the way a page was drawn before there was
 * any: headings are bigger, paragraphs have air around them, lists are
 * indented and bulleted, links are blue and underlined, and everything flows
 * down the page in the order it is written in. That is a real answer rather
 * than a stopgap. A page whose meaning is in its markup reads properly; a
 * page whose meaning is entirely in a style sheet reads as a long column,
 * which is what it is.
 *
 * The one thing it cannot do is https, and that is not a small footnote: it
 * is most of the web. TLS is a certificate parser, a big integer library and
 * two or three key exchanges, and all of that has to be written here too. It
 * is the next piece of work rather than a limitation being worked around.
 */
#include "zelr.h"
#include "draw.h"
#include "ui.h"
#include "web.h"
#include "fetch.h"
#include "html.h"

/* --- how much room there is ----------------------------------------------
 *
 * A program here has no allocator, so every one of these is a decision about
 * what the biggest page it can show looks like rather than a number that
 * grows when it needs to. They are sized for a real page: a couple of
 * hundred kilobytes of source, a few thousand words on the screen. */
#define SRC_MAX    (256 * 1024)
#define RUNS_MAX   14000
#define WORDS_MAX  (160 * 1024)
#define LINKS_MAX  900
#define HIST_MAX   40

static char   src[SRC_MAX];
static hdoc   doc;

enum { R_TEXT = 1, R_MONO, R_RULE, R_BULLET };

typedef struct {
    int x, y, w, h;               /* y is down the document, not the window */
    int at;                       /* a string in `words` */
    unsigned char kind, face, bold, under;
    u32 colour;
    int link;                     /* into `links`, or -1 */
} run_t;

static run_t runs[RUNS_MAX];
static int   nruns;

static char  words[WORDS_MAX];
static int   nwords;

typedef struct { int href; } link_t;
static link_t links[LINKS_MAX];
static int    nlinks;

/* Where a link points, as it was written on the page. It lives in the
   parser's arena rather than being copied, because a page with six hundred
   links on it is six hundred addresses and most of them are never used. */
static const char *link_href(int i) {
    if (i < 0 || i >= nlinks) return "";
    return doc.arena + links[i].href;
}

typedef struct { char text[URL_TEXT]; int scroll; } hist_t;
static hist_t hist[HIST_MAX];
static int    hist_n, hist_at = -1;

static url_t    here;
static response_t reply;

static char  title[160];
static char  status[URL_TEXT + 64];
static int   scroll;
static int   doc_h;
static int   truncated;
static int   over_link = -1;

/* --- putting words somewhere ---------------------------------------------- */

static int word_put(const char *s, int len) {
    if (nwords + len + 1 >= WORDS_MAX) return -1;
    int at = nwords;
    for (int i = 0; i < len; i++) words[nwords++] = s[i];
    words[nwords++] = 0;
    return at;
}

/* --- layout ---------------------------------------------------------------
 *
 * One pass down the node list with a stack of styles. Everything that is not
 * a block is put on the current line until the line is full, and everything
 * that is a block ends the line and leaves a gap.
 */

typedef struct {
    unsigned char face, bold, mono, pre;
    short tag;                    /* what opened this level */
    int link;
    u32 colour;
} style_t;

#define STACK_MAX 64
static style_t stack[STACK_MAX];
static int     depth;

static int content_w;             /* what the page is laid out against */
static int pen_x, line_y, line_h;
static int line_first;            /* the first run on the line being built */
static int indent;
static int pending_space;
static int list_depth;
static int list_count[8];
static int list_ordered[8];
static int skipping;              /* inside head, script, style */

static u32 col_text, col_dim, col_link, col_rule;

static style_t *top(void) { return &stack[depth]; }

static void style_push(void) {
    if (depth + 1 >= STACK_MAX) return;
    style_t *a = &stack[depth + 1], *b = &stack[depth];
    a->face = b->face; a->bold = b->bold; a->mono = b->mono; a->pre = b->pre;
    a->link = b->link; a->colour = b->colour; a->tag = b->tag;
    depth++;
}

static void style_pop(void) {
    if (depth > 0) depth--;
}

/* Finishes the line being built.
 *
 * The runs on it were emitted before the height of the line was known, which
 * is the only way to find that height: it is whatever the tallest thing on
 * it turned out to be. So they are moved down to sit on a common bottom edge
 * here, which is what stops a heading and the words beside it from being
 * drawn from different tops. */
static void line_end(void) {
    if (line_first < nruns) {
        for (int i = line_first; i < nruns; i++)
            runs[i].y = line_y + (line_h - runs[i].h);
        line_y += line_h;
    } else if (line_h) {
        line_y += line_h;
    }
    pen_x = indent;
    line_h = 0;
    line_first = nruns;
    pending_space = 0;
}

static void gap(int px) {
    line_end();
    line_y += px;
}

static run_t *run_new(void) {
    if (nruns >= RUNS_MAX) return 0;
    run_t *r = &runs[nruns++];
    r->x = 0; r->y = 0; r->w = 0; r->h = 0;
    r->at = -1; r->kind = R_TEXT; r->face = UI_FACE_BODY;
    r->bold = 0; r->under = 0; r->colour = col_text; r->link = -1;
    return r;
}

static int measure(const char *s, int face, int mono, int bold) {
    if (mono) {
        int n = 0;
        while (s[n]) n++;
        return n * FONT_W;
    }
    return face_w(s, face) + (bold ? 1 : 0);
}

static int height_of(int face, int mono) {
    return mono ? FONT_H : face_h(face);
}

/* One word onto the line, wrapping first if it will not fit. */
static void emit_word(const char *s, int len) {
    if (len <= 0) return;
    style_t *st = top();

    int at = word_put(s, len);
    if (at < 0) { truncated = 1; return; }

    int w = measure(words + at, st->face, st->mono, st->bold);
    int h = height_of(st->face, st->mono);
    int space = st->mono ? FONT_W : face_w(" ", st->face);

    int need = w + (pending_space && pen_x > indent ? space : 0);
    if (pen_x > indent && pen_x + need > content_w) {
        line_end();
        pending_space = 0;
    } else if (pending_space && pen_x > indent) {
        pen_x += space;
    }
    pending_space = 0;

    run_t *r = run_new();
    if (!r) { truncated = 1; return; }
    r->kind = st->mono ? R_MONO : R_TEXT;
    r->x = pen_x;
    r->w = w;
    r->h = h;
    r->at = at;
    r->face = st->face;
    r->bold = st->bold;
    r->under = st->link >= 0;
    r->colour = st->colour;
    r->link = st->link;

    pen_x += w;
    if (h > line_h) line_h = h;
}

/* A whole line of preformatted text, spaces and all. */
static void emit_pre_line(const char *s, int len) {
    line_end();
    style_t *st = top();
    if (len > 0) {
        int at = word_put(s, len);
        if (at < 0) { truncated = 1; return; }
        run_t *r = run_new();
        if (!r) { truncated = 1; return; }
        r->kind = R_MONO;
        r->x = indent;
        r->w = measure(words + at, st->face, 1, 0);
        r->h = FONT_H;
        r->at = at;
        r->colour = st->colour;
        r->link = st->link;
        r->under = st->link >= 0;
        line_h = FONT_H;
    } else {
        line_h = FONT_H;
    }
    line_end();
}

static void emit_rule(void) {
    line_end();
    run_t *r = run_new();
    if (!r) return;
    r->kind = R_RULE;
    r->x = indent;
    r->y = line_y + 4;
    r->w = content_w - indent;
    r->h = 2;
    r->colour = col_rule;
    line_first = nruns;            /* placed already; not part of the line */
    line_y += 10;
}

static void emit_bullet(void) {
    style_t *st = top();
    run_t *r = run_new();
    if (!r) return;
    r->kind = R_BULLET;
    r->x = indent - 14;
    r->w = 6;
    r->h = face_h(UI_FACE_BODY);
    r->colour = col_dim;
    r->face = st->face;
    if (r->h > line_h) line_h = r->h;
    pen_x = indent;
}

static void emit_number(int n) {
    char buf[12];
    int k = 0;
    if (!n) buf[k++] = '0';
    while (n > 0) { buf[k++] = (char)('0' + n % 10); n /= 10; }
    char out[14];
    int o = 0;
    while (k) out[o++] = buf[--k];
    out[o++] = '.';
    out[o] = 0;

    int at = word_put(out, o);
    if (at < 0) return;
    run_t *r = run_new();
    if (!r) return;
    r->kind = R_TEXT;
    r->x = indent - 26;
    r->w = face_w(out, UI_FACE_BODY);
    r->h = face_h(UI_FACE_BODY);
    r->at = at;
    r->colour = col_dim;
    if (r->h > line_h) line_h = r->h;
    pen_x = indent;
}

/* The text of one node, broken into words or kept as it is. */
static void lay_text(const char *s, int len) {
    if (skipping) return;
    style_t *st = top();

    if (st->pre) {
        int start = 0;
        for (int i = 0; i <= len; i++) {
            if (i == len || s[i] == '\n') {
                emit_pre_line(s + start, i - start);
                start = i + 1;
            }
        }
        return;
    }

    int i = 0;
    while (i < len) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
            pending_space = 1;
            i++;
            continue;
        }
        int start = i;
        while (i < len && s[i] != ' ' && s[i] != '\t' && s[i] != '\n'
               && s[i] != '\r') i++;
        emit_word(s + start, i - start);
    }
}

/* Which tags start a new block, and how much air goes above them. */
static int block_gap(int tag) {
    switch (tag) {
        case T_H1: return 16;
        case T_H2: return 14;
        case T_H3: case T_H4: case T_H5: case T_H6: return 12;
        case T_P: case T_BLOCKQUOTE: case T_PRE: case T_TABLE:
        case T_FIGURE: case T_FORM: return 10;
        case T_UL: case T_OL: case T_DL: return 8;
        case T_DIV: case T_SECTION: case T_ARTICLE: case T_MAIN:
        case T_HEADER: case T_FOOTER: case T_NAV: case T_ASIDE:
        case T_LI: case T_DT: case T_DD: case T_TR: case T_CAPTION:
        case T_FIGCAPTION: case T_HGROUP: return 2;
        default: return -1;                 /* not a block */
    }
}

/* Which open tag a newly opened one ends by itself.
 *
 * Almost nothing on the web closes a list item or a paragraph, and the page
 * that started all of this closes neither its terms nor its definitions. A
 * reader that waits for an end tag that is never coming keeps pushing onto
 * its stack until the stack stops taking them, and from that point on no
 * style on the page applies to anything. */
static int ends_previous(int opening, int open_now) {
    if (opening == T_P)  return open_now == T_P;
    if (opening == T_LI) return open_now == T_LI;
    if (opening == T_DT || opening == T_DD)
        return open_now == T_DT || open_now == T_DD;
    if (opening == T_TR)
        return open_now == T_TR || open_now == T_TD || open_now == T_TH;
    if (opening == T_TD || opening == T_TH)
        return open_now == T_TD || open_now == T_TH;
    if (opening == T_OPTION) return open_now == T_OPTION;
    return 0;
}

static void close_one(int tag);

static void open_tag(const hnode *n) {
    int tag = n->tag;

    if (tag == T_HEAD || tag == T_SCRIPT || tag == T_STYLE
        || tag == T_NOSCRIPT || tag == T_TITLE || tag == T_SVG
        || tag == T_IFRAME || tag == T_META || tag == T_LINK) {
        skipping++;
        return;
    }
    if (skipping) return;

    while (depth > 0 && ends_previous(tag, stack[depth].tag))
        close_one(stack[depth].tag);

    int g = block_gap(tag);
    if (g >= 0) gap(g);

    style_push();
    style_t *st = top();
    st->tag = (short)tag;

    switch (tag) {
        case T_H1: st->face = UI_FACE_HEAD; st->bold = 1; break;
        case T_H2: st->face = UI_FACE_HEAD; break;
        case T_H3: case T_H4:
            st->face = UI_FACE_BOLD; st->bold = 1; break;
        case T_H5: case T_H6:
            st->face = UI_FACE_BOLD; break;
        case T_B: case T_STRONG:
            st->face = UI_FACE_BOLD; st->bold = 1; break;
        case T_SMALL: st->face = UI_FACE_SMALL; break;
        case T_CODE: case T_KBD: case T_SAMP: case T_TT:
            st->mono = 1; break;
        case T_PRE:
            st->mono = 1; st->pre = 1; break;
        case T_BLOCKQUOTE:
            indent += 24; pen_x = indent; st->colour = col_dim; break;
        case T_A:
            if (n->href >= 0 && nlinks < LINKS_MAX) {
                links[nlinks].href = n->href;
                st->link = nlinks++;
                st->colour = col_link;
            }
            break;
        case T_UL: case T_OL:
            if (list_depth < 8) {
                list_ordered[list_depth] = (tag == T_OL);
                list_count[list_depth] = 0;
                list_depth++;
            }
            indent += 28;
            pen_x = indent;
            break;
        case T_LI:
            if (list_depth > 0 && list_ordered[list_depth - 1])
                emit_number(++list_count[list_depth - 1]);
            else
                emit_bullet();
            break;
        case T_DD: indent += 24; pen_x = indent; break;
        case T_BR: line_end(); break;
        case T_HR: emit_rule(); break;
        case T_TH:
            /* A heading cell, which is bold and, like any cell, sits beside
               the one before it rather than under it. There is no table
               layout here: a row is a line and a cell is a word on it. */
            st->face = UI_FACE_BOLD;
            st->bold = 1;
            pending_space = 1;
            break;
        case T_TD:
            pending_space = 1;
            break;
        case T_IMG:
            if (n->alt >= 0 && doc.arena[n->alt]) {
                const char *alt = doc.arena + n->alt;
                emit_word("[", 1);
                pending_space = 0;
                lay_text(alt, w_len(alt));
                pending_space = 0;
                emit_word("]", 1);
            }
            break;
        case T_INPUT: case T_BUTTON:
            if (n->alt >= 0 && doc.arena[n->alt]) {
                const char *v = doc.arena + n->alt;
                emit_word("[", 1);
                pending_space = 0;
                lay_text(v, w_len(v));
                pending_space = 0;
                emit_word("]", 1);
            }
            break;
        default: break;
    }
}

/* Ends the tag on the top of the stack, whatever wrote it there. */
static void close_one(int tag) {
    switch (tag) {
        case T_UL: case T_OL:
            if (list_depth > 0) list_depth--;
            indent -= 28;
            break;
        case T_BLOCKQUOTE: indent -= 24; break;
        case T_DD: indent -= 24; break;
        default: break;
    }
    if (indent < 0) indent = 0;

    /* The pen goes back to the margin only for something that ended a line.
     *
     * It used to go back for every end tag, and most end tags on a page are
     * in the middle of a sentence: the end of a link put the next word back
     * at the left margin without ending the line, so it was drawn on top of
     * the words already there. A page with one link at the end of a
     * paragraph looked perfect and a page with links inside its sentences
     * came out as two or three lines of text in the same place. */
    int g = block_gap(tag);
    if (g >= 0) {
        line_end();
        if (tag == T_P || tag == T_H1 || tag == T_H2 || tag == T_H3
            || tag == T_PRE || tag == T_BLOCKQUOTE || tag == T_UL
            || tag == T_OL || tag == T_TABLE) line_y += g / 2;
        pen_x = indent;
    }
    style_pop();
}

static void close_tag(int tag) {
    if (tag == T_HEAD || tag == T_SCRIPT || tag == T_STYLE
        || tag == T_NOSCRIPT || tag == T_TITLE || tag == T_SVG
        || tag == T_IFRAME || tag == T_META || tag == T_LINK) {
        if (skipping) skipping--;
        return;
    }
    if (skipping) return;

    /* Find what is being closed on the stack. An end tag for something that
       was never opened closes nothing: taking the top of the stack instead
       would strip a style off whatever the page is really inside. */
    int at = -1;
    for (int d = depth; d > 0; d--)
        if (stack[d].tag == tag) { at = d; break; }

    if (at < 0) {
        if (block_gap(tag) >= 0) { line_end(); pen_x = indent; }
        return;
    }
    while (depth >= at) close_one(stack[depth].tag);
}

static void layout(int width) {
    ui_theme t = ui_load_theme();
    col_text = t.fg;
    col_dim  = t.dim;
    col_link = t.accent;
    col_rule = t.line;

    nruns = 0;
    nwords = 0;
    nlinks = 0;
    depth = 0;
    indent = 0;
    pen_x = 0;
    line_y = 0;
    line_h = 0;
    line_first = 0;
    pending_space = 0;
    list_depth = 0;
    skipping = 0;
    truncated = doc.overflowed;
    content_w = width;

    stack[0].face = UI_FACE_BODY;
    stack[0].bold = 0;
    stack[0].mono = 0;
    stack[0].pre = 0;
    stack[0].link = -1;
    stack[0].tag = T_OTHER;
    stack[0].colour = col_text;

    for (int i = 0; i < doc.count; i++) {
        const hnode *n = &doc.nodes[i];
        if (n->kind == N_TEXT) lay_text(doc.arena + n->at, n->len);
        else if (n->kind == N_OPEN) open_tag(n);
        else close_tag(n->tag);
    }
    line_end();
    doc_h = line_y + 12;
}

/* --- plain text ----------------------------------------------------------
 *
 * A server that says text/plain means it, and running it through the markup
 * reader would eat anything in it that looked like a tag. */
static void layout_plain(const char *s, int len, int width) {
    doc.count = 0;
    doc.overflowed = 0;
    ui_theme t = ui_load_theme();
    col_text = t.fg;
    col_dim = t.dim;
    col_link = t.accent;
    col_rule = t.line;

    nruns = nwords = nlinks = 0;
    depth = 0; indent = 0; pen_x = 0; line_y = 0; line_h = 0;
    line_first = 0; pending_space = 0; list_depth = 0; skipping = 0;
    truncated = 0;
    content_w = width;

    stack[0].face = UI_FACE_BODY;
    stack[0].bold = 0;
    stack[0].mono = 1;
    stack[0].pre = 1;
    stack[0].link = -1;
    stack[0].tag = T_OTHER;
    stack[0].colour = col_text;

    lay_text(s, len);
    line_end();
    doc_h = line_y + 12;
}

/* --- fetching and showing ------------------------------------------------- */

static void say(const char *a, const char *b) {
    int n = 0;
    for (const char *p = a; *p && n < (int)sizeof(status) - 1; p++) status[n++] = *p;
    if (b) for (const char *p = b; *p && n < (int)sizeof(status) - 1; p++) status[n++] = *p;
    status[n] = 0;
}

static const char *why(int rc) {
    switch (rc) {
        case WEB_ERR_SCHEME:  return "this address is https, which needs TLS. "
                                     "Not built yet.";
        case WEB_ERR_CONNECT: return "could not connect to that host";
        case WEB_ERR_SEND:    return "the request could not be sent";
        case WEB_ERR_EMPTY:   return "the server said nothing";
        case WEB_ERR_HEADERS: return "the answer was not http";
        default:              return "the fetch failed";
    }
}

/* Builds a page of our own, for when there is nothing to show. Written as
   html and put through the same reader, so the one path that draws anything
   is the path that is used. */
static void show_message(const char *heading, const char *body, int width) {
    int n = 0;
    const char *bits[6] = { "<h1>", heading, "</h1><p>", body, "</p>", 0 };
    for (int i = 0; bits[i] && n < SRC_MAX - 1; i++)
        for (const char *p = bits[i]; *p && n < SRC_MAX - 1; p++) src[n++] = *p;
    src[n] = 0;
    html_parse(&doc, src, n);
    layout(width);
    scroll = 0;
}

static void load(const char *address, int width, int keep_scroll) {
    url_t u;
    if (!url_parse(address, &u)) {
        show_message("Not an address", "That is not something this can go to.",
                     width);
        say("nothing to go to", 0);
        return;
    }

    url_copy(&here, &u);
    say("fetching ", address);

    int rc = web_get(&here, src, SRC_MAX, &reply);
    if (rc < 0) {
        show_message("Cannot show this page", why(rc), width);
        say(why(rc), 0);
        title[0] = 0;
        return;
    }

    int plain = w_starts_fold(reply.ctype, "text/plain")
             || w_starts_fold(reply.ctype, "application/json");

    if (plain) {
        layout_plain(reply.body, reply.len, width);
        w_copy(title, sizeof(title), here.path, sizeof(title));
    } else {
        html_parse(&doc, reply.body, reply.len);
        layout(width);
        if (doc.title >= 0) w_copy(title, sizeof(title),
                                   doc.arena + doc.title, sizeof(title));
        else title[0] = 0;
    }

    if (!keep_scroll) scroll = 0;

    char n[16];
    int k = 0, v = nlinks;
    if (!v) n[k++] = '0';
    while (v) { n[k++] = (char)('0' + v % 10); v /= 10; }
    char shown[16];
    int o = 0;
    while (k) shown[o++] = n[--k];
    shown[o] = 0;

    if (rc >= 400) say("the server said this page is not there", 0);
    else if (reply.truncated || truncated)
        say("shown as far as it fits: the page is bigger than this can hold", 0);
    else say(shown, nlinks == 1 ? " link on this page" : " links on this page");
}

static void push_history(const char *address) {
    if (hist_at >= 0 && hist_at < hist_n)
        hist[hist_at].scroll = scroll;
    if (hist_n >= HIST_MAX) {
        for (int i = 1; i < HIST_MAX; i++) {
            for (int k = 0; k < URL_TEXT; k++)
                hist[i - 1].text[k] = hist[i].text[k];
            hist[i - 1].scroll = hist[i].scroll;
        }
        hist_n--;
        if (hist_at > 0) hist_at--;
    }
    hist_at++;
    w_copy(hist[hist_at].text, URL_TEXT, address, URL_TEXT);
    hist[hist_at].scroll = 0;
    hist_n = hist_at + 1;
}

/* --- drawing -------------------------------------------------------------- */

static void draw_runs(surface *s, const ui_theme *t, int ox, int oy,
                      int vw, int vh) {
    (void)t;
    for (int i = 0; i < nruns; i++) {
        run_t *r = &runs[i];
        int y = r->y - scroll;
        if (y + r->h < 0 || y > vh) continue;

        if (r->kind == R_RULE) {
            rect(s, ox + r->x, oy + y, r->w < vw ? r->w : vw, 2, r->colour);
            continue;
        }
        if (r->kind == R_BULLET) {
            disc(s, ox + r->x + 3, oy + y + r->h / 2, 3, r->colour);
            continue;
        }
        if (r->at < 0) continue;

        const char *str = words + r->at;
        if (r->kind == R_MONO) {
            text(s, ox + r->x, oy + y, str, r->colour);
        } else {
            face_draw(s, ox + r->x, oy + y, str, r->colour, r->face);
            /* Bold without a bold face at every size: the same word again,
               a pixel to the right. It is what a printer did before there
               were two cuts of a typeface, and at this size it is the
               difference between a heading and a line of body text. */
            if (r->bold) face_draw(s, ox + r->x + 1, oy + y, str, r->colour,
                                   r->face);
        }
        if (r->under) {
            /* Carried across the space to the next word when that word is
               the same link. A link underlined word by word looks like
               several links, which is what it looked like. */
            int uw = r->w;
            if (i + 1 < nruns && runs[i + 1].link == r->link
                && runs[i + 1].y == r->y && runs[i + 1].x > r->x)
                uw = runs[i + 1].x - r->x;
            rect(s, ox + r->x, oy + y + r->h - 1, uw, 1, r->colour);
        }
    }
}

static int run_at(int dx, int dy) {
    for (int i = 0; i < nruns; i++) {
        run_t *r = &runs[i];
        if (r->link < 0) continue;
        if (dx >= r->x && dx < r->x + r->w && dy >= r->y && dy < r->y + r->h)
            return i;
    }
    return -1;
}

/* --- the window ----------------------------------------------------------- */

#define TOOLBAR_H (UI_PAD + UI_BTN_H + UI_PAD)

static char address[URL_TEXT];
static ui_field bar = { address, sizeof(address), 0, 0, 0 };

/* Set when the bar has just been clicked into, and cleared by the first
   thing typed after that.
 *
 * Clicking an address bar and typing replaces what was there, which is what
 * every browser does and the only reason anybody can change an address
 * without reaching for the backspace key sixty times. It is a selection in
 * everything but drawing: there is nothing here that can show one yet. */
static int bar_fresh;
static int bar_was_focused;

static void set_address(const char *s) {
    w_copy(address, sizeof(address), s, sizeof(address));
    bar.len = w_len(address);
    bar.cursor = bar.len;
}

void _start(void) {
    int win = win_create("Browser", 860, 620);
    if (win < 0) exit(1);
    win_allow_resize(win);

    ui_input in;
    memset(&in, 0, sizeof(in));

    char arg[URL_TEXT];
    int have_arg = getarg(arg, sizeof(arg)) > 0 && arg[0];

    int laid_for = 0;
    int want_load = 1;
    int want_width = 0;
    int last_mx = -1, last_my = -1, last_scroll = -1;
    int dirty = 1;              /* something changed and a frame is owed */

    if (have_arg) set_address(arg);
    else set_address("http://example.com/");
    push_history(address);

    for (;;) {
        int w = win_width(win), h = win_height(win);
        u32 *px = win_surface(win);
        if (!px || w <= 0 || h <= 0) break;
        surface s = { px, w, h };

        int view_x = 3;
        int view_y = TOOLBAR_H + 3;
        int view_w = w - 6 - UI_SCROLL_W;
        int view_h = h - view_y - UI_ROW - 3;
        if (view_w < 80) view_w = 80;
        if (view_h < 40) view_h = 40;

        /* The page is laid out against the width it is shown at, so making
           the window wider reflows it rather than revealing more margin. */
        if (view_w != laid_for && !want_load) want_width = 1;

        ui_begin(&in);
        win_event ev;
        int closing = 0;
        int scrolled = 0;
        while (win_poll(win, &ev)) {
            if (ev.type == WIN_EV_CLOSE) { closing = 1; break; }
            if (ev.type == WIN_EV_SCROLL) scrolled += ev.y;
            ui_feed(&in, &ev);
        }
        if (closing) break;

        /* A reflow costs a pass over the whole page, so it happens once the
           dragging has stopped rather than on every frame of it. */
        if (want_width && !in.down) {
            layout(view_w - UI_PAD * 2);
            laid_for = view_w;
            want_width = 0;
            dirty = 1;
        }

        /* --- what the keyboard does ----------------------------------------
         *
         * Every key that arrived this frame, not just the first. Somebody
         * typing an address puts two or three into one frame and an address
         * with letters missing out of it goes to the wrong place, or to
         * nowhere, and looks like the network failing. */
        for (int ki = 0; ki < in.nkeys; ki++) {
            u32 raw = in.keys[ki];
            u32 k = KEY_CODE(raw);
            if (bar.focused) {
                if (k == '\n') {
                    bar.focused = 0;
                    bar_fresh = 0;
                    push_history(address);
                    want_load = 1;
                } else if (k == 27) {
                    bar.focused = 0;
                    bar_fresh = 0;
                    set_address(hist_at >= 0 ? hist[hist_at].text : address);
                } else {
                    if (bar_fresh && !KEY_IS_SPECIAL(k) && k >= ' ') {
                        address[0] = 0;
                        bar.len = 0;
                        bar.cursor = 0;
                    }
                    bar_fresh = 0;
                    ui_field_key(&bar, raw);
                }
            } else {
                int page = view_h - 40;
                if (k == KEY_DOWN)       scroll += 40;
                else if (k == KEY_UP)    scroll -= 40;
                else if (k == KEY_PAGE_DOWN) scroll += page;
                else if (k == KEY_PAGE_UP)   scroll -= page;
                else if (k == KEY_HOME)  scroll = 0;
                else if (k == KEY_END)   scroll = doc_h;
                else if (k == ' ')       scroll += page;
                else if (k == KEY_LEFT && hist_at > 0) {
                    hist[hist_at].scroll = scroll;
                    hist_at--;
                    set_address(hist[hist_at].text);
                    want_load = 1;
                } else if (k == KEY_RIGHT && hist_at + 1 < hist_n) {
                    hist[hist_at].scroll = scroll;
                    hist_at++;
                    set_address(hist[hist_at].text);
                    want_load = 1;
                }
            }
        }

        scroll -= scrolled * 48;

        int limit = doc_h - view_h;
        if (limit < 0) limit = 0;
        if (scroll > limit) scroll = limit;
        if (scroll < 0) scroll = 0;

        /* --- is there anything to redraw ------------------------------------
         *
         * Drawing a page of a few thousand words takes long enough that the
         * window manager can composite the window halfway through one, and
         * what it puts on the screen is then a page with no toolbar over it.
         * Every frame was a chance at that, sixty times a second, for a
         * window nobody was touching.
         *
         * So a frame is drawn when something happened and not otherwise, and
         * the chrome is drawn before the page rather than after it, so that
         * the half a frame anybody can catch is a window with its toolbar and
         * part of a page rather than a page with no window around it. */
        if (in.mx != last_mx || in.my != last_my) dirty = 1;
        if (in.nkeys || in.pressed || in.released || scrolled) dirty = 1;
        if (scroll != last_scroll || want_load) dirty = 1;
        last_mx = in.mx;
        last_my = in.my;
        last_scroll = scroll;

        if (!dirty) { sleep_ms(16); continue; }
        dirty = 0;

        /* The theme comes off the disk, so it is read on a frame that is
           being drawn rather than on every pass of this loop. It used to be
           every pass: sixty file reads a second, on a window nobody was
           touching, for an answer that had not changed. */
        ui_theme t = ui_load_theme();

        if (want_load) {
            want_load = 0;
            fill(&s, t.bg);
            ui_toolbar(&s, &t, w, TOOLBAR_H);
            ui_label(&s, &t, UI_PAD, TOOLBAR_H + 20, "fetching...");
            win_commit(win);

            load(address, view_w - UI_PAD * 2, 0);

            /* Where it landed, which is not always where it was sent: a
               redirect is a different address and the bar has to say so, or
               every relative link on the page is resolved against a page
               nobody is looking at. */
            char landed[URL_TEXT];
            url_text(&here, landed, sizeof(landed));
            set_address(landed);
            if (hist_at >= 0) w_copy(hist[hist_at].text, URL_TEXT, landed,
                                     URL_TEXT);
            laid_for = view_w;
            want_width = 0;
        }

        fill(&s, t.bg);

        int dx = in.mx - (view_x + UI_PAD);
        int dy = in.my - view_y + scroll;
        over_link = -1;
        if (in.my > view_y && in.my < view_y + view_h) {
            int hit = run_at(dx, dy);
            if (hit >= 0) over_link = runs[hit].link;
        }

        if (over_link >= 0 && in.released) {
            in.released = 0;
            url_t next;
            if (url_join(&here, link_href(over_link), &next)) {
                char text_of[URL_TEXT];
                url_text(&next, text_of, sizeof(text_of));
                set_address(text_of);
                push_history(text_of);
                want_load = 1;
            }
        }

        /* --- the toolbar --------------------------------------------------- */
        ui_toolbar(&s, &t, w, TOOLBAR_H);
        int bx = UI_PAD;
        if (ui_button(&s, &in, &t, bx, UI_PAD, 34, "<") && hist_at > 0) {
            hist[hist_at].scroll = scroll;
            hist_at--;
            set_address(hist[hist_at].text);
            want_load = 1;
        }
        bx += 34 + 4;
        if (ui_button(&s, &in, &t, bx, UI_PAD, 34, ">") && hist_at + 1 < hist_n) {
            hist[hist_at].scroll = scroll;
            hist_at++;
            set_address(hist[hist_at].text);
            want_load = 1;
        }
        bx += 34 + UI_GAP;
        if (ui_button(&s, &in, &t, bx, UI_PAD, 62, "Reload")) want_load = 1;
        bx += 62 + UI_GAP;

        int go_w = 40;
        int field_w = w - bx - go_w - UI_GAP - UI_PAD;
        if (field_w < 80) field_w = 80;
        ui_field_draw(&s, &in, &t, bx, UI_PAD, field_w, &bar,
                      "type an address");
        if (bar.focused && !bar_was_focused) bar_fresh = 1;
        bar_was_focused = bar.focused;
        if (ui_button_primary(&s, &in, &t, bx + field_w + UI_GAP, UI_PAD,
                              go_w, "Go")) {
            push_history(address);
            want_load = 1;
        }

        /* --- and what it is doing ------------------------------------------ */
        {
            const char *left = status;
            char hover[URL_TEXT];
            if (over_link >= 0) {
                url_t u;
                if (url_join(&here, link_href(over_link), &u)) {
                    url_text(&u, hover, sizeof(hover));
                    left = hover;
                }
            }
            ui_statusbar(&s, &t, w, h, left, title[0] ? title : "zelr");
        }

        /* --- and the page under it ------------------------------------------ */
        ui_well(&s, &t, view_x, view_y, view_w + UI_SCROLL_W, view_h, 0, 0, 0, 0);
        {
            /* Clipped at the bottom by drawing into a surface that stops
               where the well does, and at the top by putting the bevel back
               afterwards. */
            surface page = { px, w, view_y + view_h - 2 };
            draw_runs(&page, &t, view_x + UI_PAD, view_y + 2, view_w, view_h - 4);
        }
        ui_sunken(&s, &t, view_x, view_y, view_w + UI_SCROLL_W, view_h);
        ui_scrollbar(&s, &t, view_x + view_w + 2, view_y + 2, view_h - 4,
                     scroll, view_h, doc_h < view_h ? view_h : doc_h);

        win_commit(win);
        sleep_ms(16);
    }

    win_close(win);
    exit(0);
}
