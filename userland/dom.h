/* A page as a tree.
 *
 * The reader in html.h turns a page into a flat run of open, text and close,
 * which is all a layout needs if the layout is a single pass down the page.
 * It is not enough for anything else. A style sheet asks questions about
 * ancestors and siblings; a script asks for an element by name and then
 * changes it; both need every attribute rather than the three the flat
 * reader kept, and both need the contents of the script and style elements
 * that the flat reader threw away on sight.
 *
 * So this builds a tree, and it is the tree everything else now works
 * against.
 *
 * Building one means answering the question the flat reader avoided: what to
 * do when tags do not nest, which on the real web is most of the time. The
 * answer here is the one every browser converged on, which is less a guess
 * than a small set of rules about which elements end which others. A <p>
 * ends when a block starts. An <li> ends when the next <li> begins. A close
 * tag for something that is not open is dropped rather than allowed to close
 * whatever happens to be open instead, because that is how one stray
 * </div> in a footer swallows the rest of a document.
 *
 * Nothing is allocated. Every limit here is a decision about the biggest
 * page this can show, and a page past one of them says so rather than
 * quietly appearing short.
 */
#pragma once
#include "zelr.h"
#include "web.h"
#include "html.h"

#define DOM_NODES   12000
#define DOM_ATTRS   24000
#define DOM_ARENA   (640 * 1024)
#define DOM_DEPTH   64

enum { DN_ELEMENT = 1, DN_TEXT };

/* An attribute's name and value, both offsets into the arena. Names are
   folded to lower case on the way in, because HTML does not care and every
   comparison after this point would otherwise have to. */
typedef struct {
    int name, value;
} dattr;

typedef struct {
    short kind;
    short tag;
    int   text;                  /* DN_TEXT: the string, into the arena.
                                    DN_ELEMENT with an unknown tag: its name */
    int   attr_at, attr_n;
    int   parent, first, last, next, prev;
} dnode;

typedef struct {
    dnode nodes[DOM_NODES];
    int   count;
    dattr attrs[DOM_ATTRS];
    int   nattrs;
    char  arena[DOM_ARENA];
    int   used;

    int   root;                  /* the html element this always has */
    int   body;                  /* where content goes, for scripts to find */
    int   head;
    int   title;                 /* into the arena, or -1 */
    int   overflowed;
} ddoc;

/* --- the arena ----------------------------------------------------------- */

static inline void dom_put(ddoc *d, char c) {
    if (d->used < DOM_ARENA - 1) d->arena[d->used++] = c;
    else d->overflowed = 1;
}

static inline int dom_str(ddoc *d, const char *s, int len) {
    int at = d->used;
    for (int i = 0; i < len; i++) dom_put(d, s[i]);
    dom_put(d, 0);
    return at;
}

static inline const char *dom_get(const ddoc *d, int at) {
    return at >= 0 ? d->arena + at : "";
}

/* --- nodes --------------------------------------------------------------- */

static inline int dom_new(ddoc *d, int kind, int tag) {
    if (d->count >= DOM_NODES) { d->overflowed = 1; return -1; }
    int i = d->count++;
    dnode *n = &d->nodes[i];
    n->kind = (short)kind;
    n->tag = (short)tag;
    n->text = -1;
    n->attr_at = d->nattrs;
    n->attr_n = 0;
    n->parent = n->first = n->last = n->next = n->prev = -1;
    return i;
}

/* Taking a node out of wherever it is. The parser never needs this: it
   builds a tree once and only ever adds to the end of it. A script needs it
   for every one of appendChild, removeChild and insertBefore, because all
   three can be handed a node that is already somewhere.

   Nothing is freed. The node keeps its index, its text and its attributes,
   because a script that takes an element out and puts it back is ordinary,
   and because the arena all of it lives in is thrown away whole when the
   page is left. What it costs is that a script churning the tree runs the
   node count down rather than reusing it, and there are twelve thousand. */
static inline void dom_unlink(ddoc *d, int node) {
    if (node < 0 || node >= d->count) return;
    dnode *n = &d->nodes[node];
    if (n->parent >= 0) {
        dnode *p = &d->nodes[n->parent];
        if (p->first == node) p->first = n->next;
        if (p->last == node) p->last = n->prev;
    }
    if (n->prev >= 0) d->nodes[n->prev].next = n->next;
    if (n->next >= 0) d->nodes[n->next].prev = n->prev;
    n->parent = n->prev = n->next = -1;
}

/* Whether b is a or anything under it. Putting an element inside its own
   descendant makes a ring, and a ring is not a tree that terminates: the
   layout walks children until there are none and would not come back. A
   script can ask for it by accident in two lines, so it is refused here
   rather than discovered as a machine that has stopped. */
static inline int dom_contains(const ddoc *d, int a, int b) {
    while (b >= 0) {
        if (b == a) return 1;
        b = d->nodes[b].parent;
    }
    return 0;
}

static inline void dom_append(ddoc *d, int parent, int child) {
    if (parent < 0 || child < 0 || parent == child) return;
    if (dom_contains(d, child, parent)) return;
    /* A node being moved rather than added. The parser only ever passes
       fresh ones, for which this is nothing. */
    if (d->nodes[child].parent >= 0) dom_unlink(d, child);
    dnode *p = &d->nodes[parent], *c = &d->nodes[child];
    c->parent = parent;
    c->prev = p->last;
    c->next = -1;
    if (p->last >= 0) d->nodes[p->last].next = child;
    else p->first = child;
    p->last = child;
}

/* Before a sibling rather than at the end. A reference that is not actually
   a child of this parent means the end, which is what the DOM says. */
static inline void dom_insert_before(ddoc *d, int parent, int child, int ref) {
    if (parent < 0 || child < 0 || parent == child) return;
    if (ref < 0 || ref >= d->count || d->nodes[ref].parent != parent) {
        dom_append(d, parent, child);
        return;
    }
    if (dom_contains(d, child, parent)) return;
    if (d->nodes[child].parent >= 0) dom_unlink(d, child);
    dnode *c = &d->nodes[child], *r = &d->nodes[ref];
    c->parent = parent;
    c->prev = r->prev;
    c->next = ref;
    if (r->prev >= 0) d->nodes[r->prev].next = child;
    else d->nodes[parent].first = child;
    r->prev = child;
}

/* An element made by a script rather than read out of a page. An unknown
   name is kept as text, the same way the parser keeps one, so tagName
   answers with what was asked for rather than with nothing. */
static inline int dom_create_element(ddoc *d, const char *name, int len) {
    int tag = html_tag_of(name, len);
    int el = dom_new(d, DN_ELEMENT, tag);
    if (el < 0) return -1;
    if (tag == T_OTHER) d->nodes[el].text = dom_str(d, name, len);
    return el;
}

/* --- attributes ----------------------------------------------------------
 *
 * Kept as one run per element, which is why they are added while the element
 * is the newest node and never afterwards. Everything that reads them walks
 * the run, and a run is short: the longest in an ordinary page is a handful.
 */

static inline void dom_attr_add(ddoc *d, int el, int name, int value) {
    if (el < 0 || d->nattrs >= DOM_ATTRS) { d->overflowed = 1; return; }
    d->attrs[d->nattrs].name = name;
    d->attrs[d->nattrs].value = value;
    d->nattrs++;
    d->nodes[el].attr_n++;
}

static inline const char *dom_attr(const ddoc *d, int el, const char *name) {
    if (el < 0 || d->nodes[el].kind != DN_ELEMENT) return 0;
    const dnode *n = &d->nodes[el];
    for (int i = 0; i < n->attr_n; i++) {
        const dattr *a = &d->attrs[n->attr_at + i];
        if (w_same(d->arena + a->name, name)) return d->arena + a->value;
    }
    return 0;
}

/* The same, without caring about the case of the name.
 *
 * Attribute names are folded to lower case on the way in, a few lines
 * below, because HTML does not care about their case. SVG does: viewBox is
 * spelled with a capital B in every drawing ever written, and asking for it
 * by that spelling found nothing at all. What that produced was not an
 * error but a drawing at the wrong scale -- with no viewBox the renderer
 * falls back to a hundred by a hundred, so a wide drawing came out at the
 * ratio of the height it was asked for to a square it never had.
 *
 * Rather than have each caller remember which side of the fold it is on. */
static inline const char *dom_attr_fold(const ddoc *d, int el,
                                        const char *name) {
    if (el < 0 || d->nodes[el].kind != DN_ELEMENT) return 0;
    const dnode *n = &d->nodes[el];
    for (int i = 0; i < n->attr_n; i++) {
        const dattr *a = &d->attrs[n->attr_at + i];
        if (w_same_fold(d->arena + a->name, name)) return d->arena + a->value;
    }
    return 0;
}

/* Sets one, adding it when it was not there.
 *
 * The runs are laid out end to end, so there is nowhere to put a new
 * attribute in the middle of one. An element that already has the attribute
 * gets its value rewritten, in place when the new one fits and into fresh
 * arena when it does not.
 *
 * An element that does not have it at all needs its run to grow, and only
 * the newest element has room after it. So the run is moved: copied to the
 * end, where there is room, with the old one left behind. That is the same
 * trade the arena makes — a document only grows and is thrown away whole
 * when the page changes.
 *
 * This used to be refused instead, on the grounds that a script adding an
 * attribute to an element built earlier was rare. It is not rare. It is
 * `el.className = "..."` on an element that had no class, which is close to
 * the most common thing a script does to a page, and refusing it meant the
 * style never applied and nothing said why. */
static inline int dom_attr_set(ddoc *d, int el, const char *name,
                               const char *value) {
    if (el < 0 || d->nodes[el].kind != DN_ELEMENT) return 0;
    dnode *n = &d->nodes[el];
    for (int i = 0; i < n->attr_n; i++) {
        dattr *a = &d->attrs[n->attr_at + i];
        if (!w_same(d->arena + a->name, name)) continue;
        int have = w_len(d->arena + a->value);
        int want = w_len(value);
        if (want <= have) {
            char *dst = d->arena + a->value;
            for (int k = 0; k <= want; k++) dst[k] = value[k];
            return 1;
        }
        a->value = dom_str(d, value, want);
        return 1;
    }
    /* Not the newest, so the run cannot grow where it lies. Move it. */
    if (n->attr_at + n->attr_n != d->nattrs) {
        if (d->nattrs + n->attr_n + 1 > DOM_ATTRS) { d->overflowed = 1; return 0; }
        int at = d->nattrs;
        for (int i = 0; i < n->attr_n; i++)
            d->attrs[at + i] = d->attrs[n->attr_at + i];
        d->nattrs += n->attr_n;
        n->attr_at = at;
    }
    int nm = dom_str(d, name, w_len(name));
    int vl = dom_str(d, value, w_len(value));
    dom_attr_add(d, el, nm, vl);
    return 1;
}

/* --- walking -------------------------------------------------------------
 *
 * Document order, which is the order a person reads in and the order in
 * which a later rule in a style sheet beats an earlier one. Everything that
 * traverses the tree uses this rather than recursion: nothing bounds the
 * depth of a real page, and a recursive walk on a fixed stack is how a
 * browser meets a page built out of nested divs and stops. */
static inline int dom_next(const ddoc *d, int i, int top) {
    if (i < 0) return -1;
    if (d->nodes[i].first >= 0) return d->nodes[i].first;
    while (i >= 0 && i != top) {
        if (d->nodes[i].next >= 0) return d->nodes[i].next;
        i = d->nodes[i].parent;
    }
    return -1;
}

/* The text of an element and everything under it, run together. */
static inline int dom_text_content(const ddoc *d, int el, char *out, int cap) {
    int w = 0;
    if (cap <= 0) return 0;
    if (el >= 0) {
        for (int i = el; i >= 0; i = dom_next(d, i, el)) {
            if (d->nodes[i].kind != DN_TEXT) continue;
            const char *s = d->arena + d->nodes[i].text;
            while (*s && w < cap - 1) out[w++] = *s++;
        }
    }
    out[w] = 0;
    return w;
}

/* Replaces everything inside an element with one piece of text.
 *
 * What a script does when it writes to textContent. The children that were
 * there are dropped rather than freed: this document is thrown away whole
 * when the page changes, and an arena that only grows is the trade the rest
 * of this file already makes.
 */
static inline void dom_set_text(ddoc *d, int el, const char *s, int len) {
    if (el < 0 || el >= d->count) return;
    int at = dom_str(d, s, len);
    int t = dom_new(d, DN_TEXT, 0);
    if (t < 0) return;
    d->nodes[t].text = at;
    d->nodes[el].first = -1;
    d->nodes[el].last = -1;
    dom_append(d, el, t);
}

/* --- class lists ---------------------------------------------------------
 *
 * Space separated, any amount of space, and the whole word has to match:
 * "nav" must not match class="navbar", which is the difference between a
 * style sheet applying to a menu and applying to half the page. */
static inline int dom_has_class(const ddoc *d, int el, const char *want,
                                int want_len) {
    const char *cl = dom_attr(d, el, "class");
    if (!cl || want_len <= 0) return 0;
    int i = 0;
    while (cl[i]) {
        while (cl[i] == ' ' || cl[i] == '\t' || cl[i] == '\n'
               || cl[i] == '\r') i++;
        if (!cl[i]) break;
        int start = i;
        while (cl[i] && cl[i] != ' ' && cl[i] != '\t' && cl[i] != '\n'
               && cl[i] != '\r') i++;
        if (i - start == want_len) {
            int k = 0;
            while (k < want_len && cl[start + k] == want[k]) k++;
            if (k == want_len) return 1;
        }
    }
    return 0;
}

/* --- which elements end which --------------------------------------------
 *
 * The part of parsing HTML that is not tokenising. These are not stylistic
 * preferences: a page that opens a p, then a div, and closes neither is not
 * unusual, it is ordinary, and a parser that nests the div inside the
 * paragraph gets a different tree from every other browser and lays the page
 * out differently for the rest of the document. */
static inline int dom_is_block(int t) {
    switch (t) {
        case T_P: case T_DIV: case T_UL: case T_OL: case T_LI: case T_DL:
        case T_DT: case T_DD: case T_H1: case T_H2: case T_H3: case T_H4:
        case T_H5: case T_H6: case T_HR: case T_BLOCKQUOTE: case T_TABLE:
        case T_TR: case T_TD: case T_TH: case T_FORM: case T_NAV:
        case T_HEADER: case T_FOOTER: case T_SECTION: case T_ARTICLE:
        case T_MAIN: case T_ASIDE: case T_FIGURE: case T_FIGCAPTION:
        case T_PRE: case T_CAPTION: case T_HGROUP: case T_BODY:
        case T_CENTER:
            return 1;
        default: return 0;
    }
}

/* True when opening one implicitly closes the other. */
static inline int dom_closes(int open, int now) {
    if (open == T_P)  return dom_is_block(now);
    if (open == T_LI) return now == T_LI;
    if (open == T_DT || open == T_DD) return now == T_DT || now == T_DD;
    if (open == T_TD || open == T_TH)
        return now == T_TD || now == T_TH || now == T_TR;
    if (open == T_TR) return now == T_TR;
    if (open == T_OPTION) return now == T_OPTION;
    return 0;
}

/* Elements whose contents are not markup. Reading them as markup is how a
   script containing a less-than sign becomes a document with a tag in it
   that nobody wrote. */
static inline int dom_raw(int t) {
    return t == T_SCRIPT || t == T_STYLE || t == T_TEXTAREA || t == T_TITLE;
}

/* --- the parse ----------------------------------------------------------- */

typedef struct {
    ddoc *d;
    int stack[DOM_DEPTH];
    int depth;
} dparse;

static inline int dp_top(dparse *z) {
    return z->depth > 0 ? z->stack[z->depth - 1] : z->d->root;
}

static inline int dp_tag_top(dparse *z) {
    int t = dp_top(z);
    return t >= 0 ? z->d->nodes[t].tag : T_OTHER;
}

static inline void dp_push(dparse *z, int el) {
    if (z->depth < DOM_DEPTH) z->stack[z->depth++] = el;
    else z->d->overflowed = 1;
}

/* Closes back to a matching open element, and does nothing at all when there
   is none. Closing whatever happens to be open instead is how one stray end
   tag ends the document. */
static inline void dp_pop_to(dparse *z, int tag) {
    for (int k = z->depth - 1; k >= 0; k--)
        if (z->d->nodes[z->stack[k]].tag == tag) { z->depth = k; return; }
}

/* Text, with entities and utf-8 turned into something this machine can
   draw. Undecoded for a script or a style sheet, where an ampersand is an
   operator rather than the start of an entity. */
static inline int dp_text(dparse *z, const char *p, int len, int decode) {
    ddoc *d = z->d;
    int at = d->used;
    for (int i = 0; i < len; ) {
        if (!decode) { dom_put(d, p[i]); i++; continue; }
        char out[8];
        int used = 1;
        int n = html_char(p + i, len - i, &used, out);
        for (int k = 0; k < n; k++) dom_put(d, out[k]);
        i += used;
    }
    dom_put(d, 0);
    return at;
}

static inline int dp_all_space(const char *s) {
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') return 0;
    return 1;
}

static inline void dom_parse(ddoc *d, const char *p, int len) {
    d->count = 0; d->nattrs = 0; d->used = 0;
    d->title = -1; d->overflowed = 0;
    d->root = d->body = d->head = -1;
    d->root = dom_new(d, DN_ELEMENT, T_HTML);

    dparse z;
    z.d = d; z.depth = 0;

    int i = 0;
    while (i < len) {
        if (p[i] != '<') {
            int start = i;
            while (i < len && p[i] != '<') i++;
            int at = dp_text(&z, p + start, i - start, 1);

            /* Whitespace on its own is content or it is not, and which one
               depends entirely on what is beside it.
               *
               * Between two inline things it is a word separator:
               * <a>Gmail</a> <a>Images</a> is two words, and throwing it away
               * spells GmailImages. This file used to throw all of it away
               * and that is what every page looked like — every link on
               * google.com run together into one.
               *
               * It is discardable in two places, and only two. At the start
               * of a container there is nothing for it to separate. After a
               * block element the line has already ended, so a space would
               * sit at the start of the next line, where the line breaker
               * drops it anyway.
               *
               * What is kept is one space rather than what was written,
               * because an indented page is mostly newlines and the run that
               * separates two words means exactly as much as a single space
               * does. The line breaker collapses runs for the same reason. */
            if (dp_all_space(d->arena + at)) {
                d->used = at;

                int parent = dp_top(&z);
                if (parent < 0) continue;
                int last = d->nodes[parent].last;
                if (last < 0) continue;
                if (d->nodes[last].kind == DN_ELEMENT
                    && dom_is_block(d->nodes[last].tag)) continue;

                int sp = dom_new(d, DN_TEXT, T_OTHER);
                if (sp < 0) return;
                d->nodes[sp].text = dom_str(d, " ", 1);
                dom_append(d, parent, sp);
                continue;
            }
            int t = dom_new(d, DN_TEXT, T_OTHER);
            if (t < 0) return;
            d->nodes[t].text = at;
            dom_append(d, dp_top(&z), t);
            continue;
        }

        /* --- comments and doctypes --------------------------------------- */
        if (i + 3 < len && p[i + 1] == '!' && p[i + 2] == '-'
            && p[i + 3] == '-') {
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
            int at = i + 2, start = at;
            while (at < len && p[at] != '>' && p[at] != ' ') at++;
            int tag = html_tag_of(p + start, at - start);
            while (at < len && p[at] != '>') at++;
            i = at + 1;
            if (tag != T_OTHER) dp_pop_to(&z, tag);
            continue;
        }

        /* --- an open tag -------------------------------------------------- */
        {
            int at = i + 1, start = at;
            while (at < len && p[at] != '>' && p[at] != ' ' && p[at] != '\t'
                   && p[at] != '\n' && p[at] != '\r' && p[at] != '/') at++;
            int tag = html_tag_of(p + start, at - start);

            /* An unknown element still has a name, because a style sheet can
               select on one and a script can ask for it. */
            int name_at = -1;
            if (tag == T_OTHER && at > start) {
                name_at = d->used;
                for (int k = start; k < at; k++) dom_put(d, w_lower(p[k]));
                dom_put(d, 0);
            }

            /* What this ends before it begins. */
            while (z.depth > 0 && dom_closes(dp_tag_top(&z), tag)) z.depth--;

            int el = dom_new(d, DN_ELEMENT, tag);
            if (el < 0) return;
            d->nodes[el].text = name_at;

            /* Every attribute, not a chosen few: a style sheet selects on any
               of them and a script can read all of them. */
            while (at < len && p[at] != '>') {
                while (at < len && (p[at] == ' ' || p[at] == '\t'
                                    || p[at] == '\n' || p[at] == '\r')) at++;
                if (at >= len || p[at] == '>') break;
                if (p[at] == '/') { at++; continue; }

                int nstart = at;
                while (at < len && p[at] != '=' && p[at] != '>' && p[at] != ' '
                       && p[at] != '\t' && p[at] != '\n' && p[at] != '\r') at++;
                int nlen = at - nstart;
                if (nlen <= 0) { at++; continue; }

                int nm = d->used;
                for (int k = 0; k < nlen; k++) dom_put(d, w_lower(p[nstart + k]));
                dom_put(d, 0);

                int val;
                int j = at;
                while (j < len && (p[j] == ' ' || p[j] == '\t')) j++;
                if (j >= len || p[j] != '=') {
                    /* Present with no value, which is how checked and
                       disabled and hidden are written. */
                    val = d->used;
                    dom_put(d, 0);
                    at = j;
                } else {
                    j++;
                    while (j < len && (p[j] == ' ' || p[j] == '\t')) j++;
                    char quote = 0;
                    if (j < len && (p[j] == '"' || p[j] == '\'')) quote = p[j++];
                    int vstart = j;
                    while (j < len) {
                        char c = p[j];
                        if (quote ? (c == quote)
                                  : (c == ' ' || c == '>' || c == '\t'
                                     || c == '\n' || c == '\r')) break;
                        j++;
                    }
                    val = dp_text(&z, p + vstart, j - vstart, 1);
                    if (quote && j < len) j++;
                    at = j;
                }
                dom_attr_add(d, el, nm, val);
            }

            int self_closed = at > i && p[at - 1] == '/';
            i = at + 1;

            dom_append(d, dp_top(&z), el);
            if (tag == T_BODY && d->body < 0) d->body = el;
            if (tag == T_HEAD && d->head < 0) d->head = el;

            if (dom_raw(tag)) {
                const char *name = HTML_TAGS[tag];
                int end = html_raw_end(p, len, i, name);
                int decode = (tag == T_TITLE || tag == T_TEXTAREA);
                if (end > i) {
                    int txt = dp_text(&z, p + i, end - i, decode);
                    int t = dom_new(d, DN_TEXT, T_OTHER);
                    if (t < 0) return;
                    d->nodes[t].text = txt;
                    dom_append(d, el, t);
                    if (tag == T_TITLE && d->title < 0) d->title = txt;
                }
                i = end;
                while (i < len && p[i] != '>') i++;
                i++;
            } else if (!html_void(tag) && !self_closed) {
                dp_push(&z, el);
            }
        }
    }

    /* A page with no body element still has content, and everything that
       looks for one has to find something. */
    if (d->body < 0) d->body = d->root;
}

/* --- finding things ------------------------------------------------------ */

static inline int dom_by_id(const ddoc *d, const char *id) {
    for (int i = 0; i < d->count; i++) {
        if (d->nodes[i].kind != DN_ELEMENT) continue;
        const char *v = dom_attr(d, i, "id");
        if (v && w_same(v, id)) return i;
    }
    return -1;
}

/* The nth element with this tag, in document order, or -1 when there is no
   such thing, which is also how a caller counts them. */
static inline int dom_by_tag(const ddoc *d, int tag, int nth) {
    for (int i = 0; i < d->count; i++)
        if (d->nodes[i].kind == DN_ELEMENT && d->nodes[i].tag == tag)
            if (nth-- == 0) return i;
    return -1;
}

/* The name of an element, known tag or not. */
static inline const char *dom_tag_name(const ddoc *d, int el) {
    if (el < 0 || d->nodes[el].kind != DN_ELEMENT) return "";
    if (d->nodes[el].tag != T_OTHER) return HTML_TAGS[d->nodes[el].tag];
    return d->nodes[el].text >= 0 ? d->arena + d->nodes[el].text : "";
}
