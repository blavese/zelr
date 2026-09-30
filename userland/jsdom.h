#pragma once
#include "dom.h"
#include "css.h"
#include "js.h"
#include "jsparse.h"
#include "jsrun.h"
#include "web.h"

/* The document, as a script sees it.
 *
 * The engine in js.h knows nothing about pages: it runs a language, and it
 * was written that way on purpose so it could be tested without one. This
 * file is the browser putting a page in front of it -- the tree, the window
 * and what hangs off the window -- in the shapes the standards give them,
 * because that is the shape every script on the web is written against.
 *
 * --- the shape ---------------------------------------------------------------
 *
 * Every node a script touches is one object, made the first time it is asked
 * for and kept, so two names for one element are the same object. The
 * object holds nothing but the node's number and a prototype: an element is
 * an HTMLDivElement or an HTMLAnchorElement, which is an HTMLElement, which
 * is an Element, a Node and an EventTarget, each of those a prototype with
 * the methods and accessors the standard puts there. Reading el.textContent
 * runs an accessor on Node.prototype that walks the document as it is now,
 * rather than a copy taken when the object was made; writing it changes the
 * document the layout is about to read.
 *
 * It used to be one plain object per node with ten methods of its own and a
 * table of names behind it. That could not answer `el instanceof Element`,
 * which is how libraries tell a node from anything else, nor let a page add
 * a method to Element.prototype, which is what every polyfill does, and it
 * cost three kilobytes an element.
 *
 * A few objects answer for names nobody can list in advance -- a style's
 * properties, a dataset, a live list's items, storage -- and those go through
 * the engine's host hook (js.h), asked before an object's own properties.
 *
 * --- honesty -------------------------------------------------------------------
 *
 * What is here is what this browser does, and nothing else. Every value a
 * script can read is true of it: the user agent is what it sends, a picture
 * has the size it was decoded at, a box is where the layout put it, and what
 * the browser does not have is absent rather than pretended. A page asks
 * whether a thing exists so that it can do without it, and a false yes sends
 * it down the path that needs the thing. A missing property is better than
 * one that returns something plausible forever and lets a page believe it
 * worked.
 *
 * --- the script world outlives the load ----------------------------------------
 *
 * The world is opened when the page is built and closed when the page is
 * left, and what is in between is a browser that can be clicked on: a
 * handler is by definition a piece of a program that runs after the program
 * finished. A page with no script and no handler attribute opens no world
 * and costs nothing, which is still the common case by a wide margin.
 */

/* The host numbers that are not nodes. A node's is its index, and DOM_NODES
   is far below the first of these; the hook only fires for zero or more. */
#define JD_DOCUMENT  0x1000000
#define JD_WINDOW    (JD_DOCUMENT + 1)       /* a listener's key, never a host */
#define JD_CLASSLIST 0x2000000               /* + node: its classList */
#define JD_STYLE     0x3000000               /* + node: its style */
#define JD_DATASET   0x4000000               /* + node: its dataset */
#define JD_ATTRS     0x5000000               /* + node: its attributes */
#define JD_LIST      0x6000000               /* + index: a list of nodes */
#define JD_STORAGE   0x7000000               /* localStorage; + 1 sessionStorage */
#define JD_COMPUTED  (JD_STORAGE + 2)        /* a getComputedStyle answer */
#define JD_RSTYLE    0x8000000               /* + n: a CSS rule's style (jscssom.h) */
#define JD_RLIST     0x9000000               /* + n: a live list of CSS rules (jscssom.h) */

static jobj  *jd_document_obj;
static ddoc  *jd_doc;            /* what these bindings are bound to */
static csheet *jd_sheet;         /* borrowed, for asking about selectors */
static jobj **jd_wrap;           /* one object per node, so identity holds */
static int    jd_dirty;          /* a script changed what layout must see */
static u32    jd_version;        /* moved on by every change to the tree */
static int    jd_parsed;         /* how many nodes the page arrived with */

/* The world itself, open for as long as the page is. */
static jctx   jd_J;
static int    jd_open;
static char   jd_err[128];       /* the first thing that went wrong in it */

/* Where the page is, as the browser shows it: what location, document.URL
   and every relative address a script writes are relative to. Set by the
   browser before the world is opened (jsdom_at). */
static char   jd_address[URL_TEXT + 256];

#define JD_WRAPS DOM_NODES

/* Told of every error, not only the first, for tools/host/scriptdump.c. */
static void (*jd_on_error)(const char *err, int line);

static void jd_note_error(void) {
    if (jd_on_error && jd_J.error[0]) jd_on_error(jd_J.error, jd_J.error_line);
    if (jd_err[0] || !jd_J.error[0]) return;
    int w = 0;
    for (const char *p = jd_J.error; *p && w < (int)sizeof(jd_err) - 1; p++)
        jd_err[w++] = *p;

    /* And where. A minified page is one line of a hundred thousand
       characters, so the number is not much on its own -- but it tells one
       script from another, which is the difference between "something on
       this page failed" and "the third one did". */
    if (jd_J.error_line > 0 && w < (int)sizeof(jd_err) - 16) {
        const char *at = " at line ";
        for (int i = 0; at[i]; i++) jd_err[w++] = at[i];
        char num[12];
        int n = 0, v = jd_J.error_line;
        if (v > 999999) v = 999999;
        do { num[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) jd_err[w++] = num[--n];
    }
    jd_err[w] = 0;
}

/* Whether the page's scripts have used all the memory they are allowed. Once
   they have, none of its handlers, timers or requests is run again: each one
   would stop at its first allocation anyway, having used a little more of the
   spare the one that ran out is getting out on (js.h, js_alloc). */
/* A page that has run out does not run again: what it holds is what ran it
   out, and a collection after (jsgc.h) gives back only what it had finished
   with. */
static int jd_spent(void) { return jd_J.ran_out || jd_J.allocated >= jd_J.mem_cap; }

/* A call from the browser into the page: a handler, a timer, a reply. What
   it throws is the page's to report and not the browser's to stop on. */
static jval jd_call(jval fn, jval self, jval *argv, int argc) {
    jd_J.sig = JS_OK;
    jd_J.steps = 0;
    jval r = js_call(&jd_J, fn, self, argv, argc);
    if (jd_J.sig != JS_OK) { jd_note_error(); jd_J.sig = JS_OK; return js_undef(); }
    return r;
}

/* --- small things ---------------------------------------------------------------- */

static jval jd_str(const char *s) { return js_from_str(js_str(&jd_J, s)); }
static jval jd_str_n(const char *s, int n) { return js_from_str(js_str_n(&jd_J, s, (u32)n)); }

static jval jd_illegal(jctx *J) {
    return js_throw(J, JS_ERR_TYPE, "Illegal invocation", J->error_line);
}

/* A string argument, or "" for one that is missing. */
static jstr *jd_arg_str(jctx *J, jval *a, int n, int i) {
    if (i >= n) return js_str(J, "");
    if (a[i].t == JS_STR) return a[i].str;
    jstr *s = js_to_str(J, a[i]);
    return s ? s : js_str(J, "");
}

static int jd_same_n(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* A growing buffer of text, turned into a string once, as the engine does
   (jsrun.h, jt_put): textContent and innerHTML of a whole page are
   hundreds of kilobytes, and a fixed buffer cut them short. */
static void jd_put(jtext *t, const char *s) { jt_put(&jd_J, t, s, (u32)w_len(s)); }
static void jd_putc(jtext *t, char c) { jt_put(&jd_J, t, &c, 1); }

__attribute__((unused)) static void jd_put_num(jtext *t, int v) {
    char d[12];
    int k = 0;
    u32 u = v < 0 ? (u32)-v : (u32)v;
    if (v < 0) jd_putc(t, '-');
    do { d[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (k) jd_putc(t, d[--k]);
}

/* --- what a node is ------------------------------------------------------------------
 *
 * The document here has three kinds of node, elements, text and comments
 * (dom.h), and the layout draws nothing for a comment. A document fragment
 * is an element with a name no page can write, "#document-fragment", and is
 * never in the tree to be drawn at all. */
enum { JN_ELEMENT = 1, JN_TEXT = 3, JN_COMMENT = 8, JN_DOCUMENT = 9, JN_FRAGMENT = 11 };

static int jd_kind(int n) {
    const dnode *x = &jd_doc->nodes[n];
    if (x->kind == DN_TEXT) return JN_TEXT;
    if (x->kind == DN_COMMENT) return JN_COMMENT;
    if (x->tag == T_OTHER && x->text >= 0 && jd_doc->arena[x->text] == '#') {
        /* "#comment", "#document-fragment", or "#document" (jd_new_document),
           which is nine letters long. */
        const char *nm = jd_doc->arena + x->text;
        if (nm[1] == 'c') return JN_COMMENT;
        return nm[9] == 0 ? JN_DOCUMENT : JN_FRAGMENT;
    }
    return JN_ELEMENT;
}

static int jd_is_element(int n) { return n >= 0 && jd_kind(n) == JN_ELEMENT; }

/* The node a value stands for, or -1 when it is not one of ours. */
static int jd_node_of(jval v) {
    if (v.t != JS_OBJ || !v.obj || !jd_doc) return -1;
    int h = v.obj->host;
    return (h >= 0 && h < jd_doc->count) ? h : -1;
}

static int jd_el_of(jval v) {
    int n = jd_node_of(v);
    return jd_is_element(n) ? n : -1;
}

static int jd_is_doc(jval v) { return v.t == JS_OBJ && v.obj && v.obj->host == JD_DOCUMENT; }

/* Whether a node is in the page, rather than made and not yet put anywhere,
   or taken out. A node taken out keeps its number (dom.h, dom_unlink), and
   a search that walked the numbers found it still: B17 in atlas 12. */
static int jd_connected(int n) {
    for (int k = 0; n >= 0 && k < DOM_NODES; k++) {
        if (n == jd_doc->root) return 1;
        n = jd_doc->nodes[n].parent;
    }
    return 0;
}

/* Each shadow root a script attached, with its element (attachShadow, below).
   What is in one is in the page while its element is, and the browser draws
   them (browser.c, page_drawn). jd_shadow_mark has a bit for every host and
   every root, so asking of a node that is neither costs nothing. */
#define JD_SHADOWS 1024
static int jd_shadow_host[JD_SHADOWS], jd_shadow_root[JD_SHADOWS], jd_nshadow;
static u8 jd_shadow_mark[DOM_NODES / 8];

/* A template's contents, which are a document of their own with no window:
   nothing made there is upgraded, as in a browser (jd_custom_created). */
static u8 jd_inert_mark[DOM_NODES / 8];

/* Which of them n is the root of (as_root) or the element of, or -1. */
static int jd_shadow_find(int n, int as_root) {
    if (n < 0 || n >= DOM_NODES || !(jd_shadow_mark[n >> 3] & (1 << (n & 7)))) return -1;
    for (int s = 0; s < jd_nshadow; s++)
        if ((as_root ? jd_shadow_root[s] : jd_shadow_host[s]) == n) return s;
    return -1;
}

/* Connected as the standard means it, through a shadow root to its element:
   isConnected, and custom elements' upgrades and callbacks. A component's
   own components are in its shadow root, and were never upgraded, so they
   drew nothing. The page's searches keep to the document (jd_connected). */
static int jd_connected_deep(int n) {
    for (int k = 0; n >= 0 && k < DOM_NODES; k++) {
        if (n == jd_doc->root) return 1;
        int p = jd_doc->nodes[n].parent;
        if (p < 0) {
            int s = jd_shadow_find(n, 1);
            p = s >= 0 ? jd_shadow_host[s] : -1;
        }
        n = p;
    }
    return 0;
}

/* The page's own <html> element, which is the document element to a
   script. The parser puts every page under a root html element of its own
   (dom.h, dom_parse), so a page that wrote <html lang="en" class="no-js">
   has that element as the root's child, with the attributes on it; a
   script reading documentElement.lang or changing its class got the root,
   which has neither. To a script the root is not there: its place is the
   document's. A page that wrote no <html> has only the root, which is then
   its document element. */
static int jd_top(void) {
    int r = jd_doc->root;
    if (r < 0) return r;
    for (int c = jd_doc->nodes[r].first; c >= 0; c = jd_doc->nodes[c].next)
        if (jd_doc->nodes[c].kind == DN_ELEMENT && jd_doc->nodes[c].tag == T_HTML) return c;
    return r;
}

/* A document that is not the page (jd_new_document), when this is one. */
static int jd_inert_of(jval t) {
    int x = jd_node_of(t);
    return x >= 0 && jd_kind(x) == JN_DOCUMENT ? x : -1;
}

/* Whether a node is where the document's own children are: the document
   element, or the root in its place. */
static int jd_is_top(int n) { return n >= 0 && (n == jd_doc->root || n == jd_top()); }

static int jd_has_ancestor_tag(int n, int tag, const char *name) {
    for (int p = jd_doc->nodes[n].parent; p >= 0; p = jd_doc->nodes[p].parent) {
        if (jd_doc->nodes[p].kind != DN_ELEMENT) continue;
        if (tag != T_OTHER ? jd_doc->nodes[p].tag == tag
                           : w_same(dom_tag_name(jd_doc, p), name)) return 1;
    }
    return 0;
}

/* Inside an <svg>, where elements are SVG's and keep the case of their
   names. Made with createElementNS for SVG counts too (jd_svg_made). */
static u8 jd_svg_made[DOM_NODES / 8];

static int jd_is_svg(int n) {
    if (jd_doc->nodes[n].kind != DN_ELEMENT) return 0;
    if (jd_doc->nodes[n].tag == T_SVG) return 1;
    if (jd_svg_made[n >> 3] & (1 << (n & 7))) return 1;
    return jd_has_ancestor_tag(n, T_SVG, 0);
}

/* An element's name as the DOM gives it: upper case for HTML, which is what
   `el.tagName === 'A'` is written against, and as written for SVG. */
static void jd_tag_name_into(int n, char *out, int cap, int upper) {
    const char *nm = dom_tag_name(jd_doc, n);
    int svg = upper && jd_is_svg(n);
    int i = 0;
    for (; nm[i] && i < cap - 1; i++) {
        char c = nm[i];
        if (upper && !svg && c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[i] = c;
    }
    out[i] = 0;
}

/* --- the interfaces ---------------------------------------------------------------
 *
 * One prototype for each name the standards give, chained the way they chain
 * them. The HTML ones are a table: which elements each is for, and which it
 * inherits from when that is not HTMLElement itself. */
enum {
    JI_EVENTTARGET, JI_NODE, JI_CHARDATA, JI_TEXT, JI_COMMENT, JI_ELEMENT, JI_HTMLELEMENT,
    JI_SVGELEMENT, JI_SVGSVG, JI_DOCUMENT, JI_HTMLDOCUMENT, JI_FRAGMENT,
    JI_NODELIST, JI_HTMLCOLLECTION, JI_TOKENLIST, JI_STYLEDECL, JI_STRINGMAP,
    JI_NAMEDNODEMAP, JI_ATTR, JI_WINDOW, JI_EVENT, JI_XHR, JI_DOMRECT,
    JI_COUNT
};
static jobj *jd_p[JI_COUNT];

typedef struct {
    const char *name;
    const char *tags;          /* space separated; 0 for one that is only inherited */
    const char *parent;        /* 0: HTMLElement */
} jd_iface_t;

static const jd_iface_t JD_HTML[] = {
    { "HTMLAnchorElement", "a", 0 },           { "HTMLAreaElement", "area", 0 },
    { "HTMLMediaElement", 0, 0 },
    { "HTMLAudioElement", "audio", "HTMLMediaElement" },
    { "HTMLVideoElement", "video", "HTMLMediaElement" },
    { "HTMLBRElement", "br", 0 },              { "HTMLBaseElement", "base", 0 },
    { "HTMLBodyElement", "body", 0 },          { "HTMLButtonElement", "button", 0 },
    { "HTMLCanvasElement", "canvas", 0 },      { "HTMLDListElement", "dl", 0 },
    { "HTMLDataElement", "data", 0 },          { "HTMLDataListElement", "datalist", 0 },
    { "HTMLDetailsElement", "details", 0 },    { "HTMLDialogElement", "dialog", 0 },
    { "HTMLDivElement", "div", 0 },            { "HTMLEmbedElement", "embed", 0 },
    { "HTMLFieldSetElement", "fieldset", 0 },  { "HTMLFormElement", "form", 0 },
    { "HTMLHRElement", "hr", 0 },              { "HTMLHeadElement", "head", 0 },
    { "HTMLHeadingElement", "h1 h2 h3 h4 h5 h6", 0 },
    { "HTMLHtmlElement", "html", 0 },          { "HTMLIFrameElement", "iframe", 0 },
    { "HTMLImageElement", "img", 0 },          { "HTMLInputElement", "input", 0 },
    { "HTMLLIElement", "li", 0 },              { "HTMLLabelElement", "label", 0 },
    { "HTMLLegendElement", "legend", 0 },      { "HTMLLinkElement", "link", 0 },
    { "HTMLMapElement", "map", 0 },            { "HTMLMenuElement", "menu", 0 },
    { "HTMLMetaElement", "meta", 0 },          { "HTMLMeterElement", "meter", 0 },
    { "HTMLModElement", "ins del", 0 },        { "HTMLOListElement", "ol", 0 },
    { "HTMLObjectElement", "object", 0 },      { "HTMLOptGroupElement", "optgroup", 0 },
    { "HTMLOptionElement", "option", 0 },      { "HTMLOutputElement", "output", 0 },
    { "HTMLParagraphElement", "p", 0 },        { "HTMLPictureElement", "picture", 0 },
    { "HTMLPreElement", "pre listing xmp", 0 },{ "HTMLProgressElement", "progress", 0 },
    { "HTMLQuoteElement", "blockquote q", 0 }, { "HTMLScriptElement", "script", 0 },
    { "HTMLSelectElement", "select", 0 },      { "HTMLSlotElement", "slot", 0 },
    { "HTMLSourceElement", "source", 0 },      { "HTMLSpanElement", "span", 0 },
    { "HTMLStyleElement", "style", 0 },        { "HTMLTableCaptionElement", "caption", 0 },
    { "HTMLTableCellElement", "td th", 0 },    { "HTMLTableColElement", "col colgroup", 0 },
    { "HTMLTableElement", "table", 0 },        { "HTMLTableRowElement", "tr", 0 },
    { "HTMLTableSectionElement", "thead tbody tfoot", 0 },
    { "HTMLTemplateElement", "template", 0 },  { "HTMLTextAreaElement", "textarea", 0 },
    { "HTMLTimeElement", "time", 0 },          { "HTMLTitleElement", "title", 0 },
    { "HTMLTrackElement", "track", 0 },        { "HTMLUListElement", "ul", 0 },
    { "HTMLUnknownElement", 0, 0 },
};
#define JD_HTML_N ((int)(sizeof(JD_HTML) / sizeof(JD_HTML[0])))
static jobj *jd_html_p[JD_HTML_N];

/* Elements that are plain HTMLElements: known to HTML, with no interface of
   their own. Any other name without a hyphen is an HTMLUnknownElement, and
   one with a hyphen is a custom element's, which is an HTMLElement until a
   page defines it (jd_custom_proto). */
static const char JD_PLAIN_HTML[] =
    " b strong i em code kbd samp tt nav header footer section article main aside figure"
    " figcaption small noscript hgroup center abbr address bdi bdo cite dfn dd dt mark rp rt"
    " ruby s sub sup u var wbr summary search acronym big strike nobr noembed noframes ";

static int jd_word_in(const char *list, const char *w) {
    int n = w_len(w);
    if (!n) return 0;
    for (const char *p = list; *p; ) {
        while (*p == ' ') p++;
        const char *s = p;
        while (*p && *p != ' ') p++;
        if (p - s == n && jd_same_n(s, w, n)) return 1;
    }
    return 0;
}

static int jd_html_index(const char *name) {
    for (int i = 0; i < JD_HTML_N; i++) if (w_same(JD_HTML[i].name, name)) return i;
    return -1;
}

static jobj *jd_iface(const char *name) {
    int i = jd_html_index(name);
    return i >= 0 ? jd_html_p[i] : 0;
}

/* Which prototype an element of this name gets, cached for the named tags. */
static jobj *jd_tag_proto[T_COUNT];

static jobj *jd_custom_proto(const char *name);

static jobj *jd_html_proto_by_name(const char *nm) {
    for (int i = 0; i < JD_HTML_N; i++)
        if (JD_HTML[i].tags && jd_word_in(JD_HTML[i].tags, nm)) return jd_html_p[i];
    if (jd_word_in(JD_PLAIN_HTML, nm)) return jd_p[JI_HTMLELEMENT];
    for (const char *p = nm; *p; p++) if (*p == '-') return jd_p[JI_HTMLELEMENT];
    return jd_iface("HTMLUnknownElement");
}

static jobj *jd_proto_for(int node) {
    switch (jd_kind(node)) {
        case JN_TEXT: return jd_p[JI_TEXT];
        case JN_COMMENT: return jd_p[JI_COMMENT];
        case JN_FRAGMENT: return jd_p[JI_FRAGMENT];
        case JN_DOCUMENT: return jd_p[JI_HTMLDOCUMENT];
        default: break;
    }
    const dnode *x = &jd_doc->nodes[node];
    if (jd_is_svg(node)) return x->tag == T_SVG ? jd_p[JI_SVGSVG] : jd_p[JI_SVGELEMENT];
    if (x->tag != T_OTHER) {
        if (!jd_tag_proto[x->tag]) jd_tag_proto[x->tag] = jd_html_proto_by_name(HTML_TAGS[x->tag]);
        return jd_tag_proto[x->tag];
    }
    const char *nm = dom_tag_name(jd_doc, node);
    jobj *c = jd_custom_proto(nm);
    return c ? c : jd_html_proto_by_name(nm);
}

/* --- the objects for nodes ------------------------------------------------------------ */

static jobj *jd_element(jctx *J, int node) {
    if (!jd_doc || node < 0 || node >= jd_doc->count) return 0;
    if (jd_wrap && jd_wrap[node]) return jd_wrap[node];
    jobj *o = js_object_with(J, JO_PLAIN, jd_proto_for(node));
    if (!o) return 0;
    o->host = node;
    if (jd_wrap) jd_wrap[node] = o;
    return o;
}

/* A node as a value: its object, or null for none. The root's parent is
   the document, which has no node of its own. */
static jval jd_el_value(jctx *J, int node) {
    jobj *o = jd_element(J, node);
    return o ? js_from_obj(o) : js_null();
}

static jval jd_node_or_doc(jctx *J, int node) {
    if (node < 0) return js_null();
    return jd_el_value(J, node);
}

/* Things kept on a node's object where no script will stumble on them: its
   classList, style and dataset, made once so that `el.style === el.style`,
   and a template's contents. Keyed by symbols made for this world. */
static jstr *jd_k_classlist, *jd_k_style, *jd_k_dataset, *jd_k_attrs, *jd_k_children,
            *jd_k_childnodes, *jd_k_content, *jd_k_evflags, *jd_k_evpath, *jd_k_signal,
            *jd_k_shadow, *jd_k_host, *jd_k_mode;
static jobj *jd_p_shadowroot;

static jval jd_kept(jobj *o, jstr *key) {
    jprop *p = o ? js_find(o, key) : 0;
    return p ? p->v : js_undef();
}

static void jd_keep(jobj *o, jstr *key, jval v) {
    if (!o) return;
    jprop *p = js_put_prop(&jd_J, o, key, v);
    if (p) p->flags = JP_WRITE;
}

/* --- changes ---------------------------------------------------------------------------
 *
 * Every change to the tree goes through here, so that three things that must
 * follow one always do: the layout is told (jd_dirty), live lists know to
 * look again (jd_version), and a MutationObserver hears about it. */
static void jd_record_children(int target, int added, int removed, int prev, int next);
static void jd_record_attr(int target, const char *name, const char *old);
static void jd_record_text(int target, const char *old);

/* And a custom element hears of its own (jd_custom_*, further down). */
static void jd_custom_connected(int top);
static void jd_custom_created(int top);
static int jd_inert(int x);
static void jd_slots_touch(int parent, int child);
static int jd_fire_simple(int node, const char *type, int bubbles, int cancelable);
static int jd_later_native(void (*fn)(jval arg), jval arg, int ticks_from_now);
static void jd_custom_disconnected(int top);
static void jd_custom_attr(int node, const char *name, const char *old, const char *now);
static int  jd_ncustom;

static void jd_touched(void) { jd_version++; jd_dirty = 1; }

/* An element whose style attribute changed, for the browser to read again
   before it lays the page out (jsdom_next_restyled). The style attributes
   were read once when the page was built, before any script ran, so a
   script's el.style.display = 'none' changed the attribute and nothing on
   the screen. */
static u8  jd_restyle[DOM_NODES / 8];
static int jd_restyle_any;

static void jd_mark_restyle(int el) {
    if (el < 0 || el >= DOM_NODES) return;
    jd_restyle[el >> 3] |= (u8)(1 << (el & 7));
    jd_restyle_any = 1;
}

/* --- attributes ---------------------------------------------------------------------- */

/* Names are folded to lower case on an HTML element, as the parser folds
   them: setAttribute('onClick') and getAttribute('ONCLICK') are the same
   attribute. */
static void jd_attr_name(int el, const jstr *s, char *out, int cap) {
    (void)el;
    int n = (int)s->len < cap - 1 ? (int)s->len : cap - 1;
    for (int i = 0; i < n; i++) out[i] = w_lower(s->s[i]);
    out[n] = 0;
}

static const char *jd_attr(int el, const char *name) {
    return dom_attr_fold(jd_doc, el, name);
}

static int jd_is_upgraded(int n);

static void jd_attr_set(int el, const char *name, const char *value) {
    const char *old = jd_attr(el, name);
    if (old && w_same(old, value)) return;
    jd_record_attr(el, name, old);
    /* The old value, kept for a custom element that watches this attribute:
       one that fits is written over where it lies (dom.h, dom_attr_set). */
    char *was = 0;
    if (jd_ncustom && jd_is_upgraded(el) && old) {
        int n = w_len(old);
        was = (char *)malloc((u64)n + 1);
        if (was) for (int i = 0; i <= n; i++) was[i] = old[i];
    }
    int had = old != 0;
    if (!dom_attr_set(jd_doc, el, name, value)) { if (was) free(was); return; }
    jd_touched();
    if (w_same(name, "style")) jd_mark_restyle(el);
    if (jd_ncustom && jd_is_upgraded(el)) jd_custom_attr(el, name, had ? (was ? was : "") : 0, value);
    if (was) free(was);
}

/* Out of the element's run of attributes. The runs sit end to end (dom.h),
   so the gap is closed from inside this run only, and the slot left at its
   end belongs to nobody. */
static int jd_attr_remove(int el, const char *name) {
    dnode *x = &jd_doc->nodes[el];
    for (int i = 0; i < x->attr_n; i++) {
        dattr *a = &jd_doc->attrs[x->attr_at + i];
        if (!w_same_fold(jd_doc->arena + a->name, name)) continue;
        const char *old = jd_doc->arena + a->value;
        jd_record_attr(el, name, old);
        for (int k = i; k + 1 < x->attr_n; k++)
            jd_doc->attrs[x->attr_at + k] = jd_doc->attrs[x->attr_at + k + 1];
        x->attr_n--;
        jd_touched();
        if (w_same_fold(name, "style")) jd_mark_restyle(el);
        /* The value's bytes are still in the arena where they were. */
        if (jd_ncustom && jd_is_upgraded(el)) jd_custom_attr(el, name, old, 0);
        return 1;
    }
    return 0;
}

/* --- making nodes ------------------------------------------------------------------- */

static int jd_new_text(const char *s, int len) {
    int t = dom_new(jd_doc, DN_TEXT, T_OTHER);
    if (t < 0) return -1;
    jd_doc->nodes[t].text = dom_str(jd_doc, s, len);
    return t;
}

static int jd_new_named(const char *name) {
    int el = dom_new(jd_doc, DN_ELEMENT, T_OTHER);
    if (el < 0) return -1;
    jd_doc->nodes[el].text = dom_str(jd_doc, name, w_len(name));
    return el;
}

static int jd_new_comment(const char *s, int len) {
    int c = dom_new(jd_doc, DN_COMMENT, T_OTHER);
    if (c < 0) return -1;
    jd_doc->nodes[c].text = dom_str(jd_doc, s, len);
    return c;
}

static int jd_new_fragment(void) { return jd_new_named("#document-fragment"); }

/* An element by name, lower-cased as the parser lowers the ones it reads,
   because a page's createElement('DIV') is a div. */
static int jd_new_element(const jstr *name) {
    char low[64];
    int n = (int)name->len < 63 ? (int)name->len : 63;
    for (int i = 0; i < n; i++) low[i] = w_lower(name->s[i]);
    low[n] = 0;
    return dom_create_element(jd_doc, low, n);
}

/* --- the text of things ------------------------------------------------------------- */

static const char *jd_text_of(int n) {
    const dnode *x = &jd_doc->nodes[n];
    if (x->kind == DN_TEXT || x->kind == DN_COMMENT)
        return x->text >= 0 ? jd_doc->arena + x->text : "";
    return "";
}

static void jd_text_content(int el, jtext *t) {
    for (int i = el; i >= 0; i = dom_next(jd_doc, i, el))
        if (jd_doc->nodes[i].kind == DN_TEXT && jd_doc->nodes[i].text >= 0)
            jd_put(t, jd_doc->arena + jd_doc->nodes[i].text);
}

static jval jd_text_value(int el) {
    jtext t = { 0, 0, 0, 0 };
    jd_text_content(el, &t);
    return js_from_str(jt_done(&jd_J, &t));
}

static void jd_set_data(int n, const char *s, int len) {
    jd_record_text(n, jd_text_of(n));
    if (jd_doc->nodes[n].kind == DN_TEXT || jd_doc->nodes[n].kind == DN_COMMENT)
        jd_doc->nodes[n].text = dom_str(jd_doc, s, len);
    jd_touched();
}

/* --- putting nodes in and taking them out -------------------------------------------- */

static void jd_scripts_inserted(int top);

/* Modules (jsmod.h). */
static void jd_mods_reset(void);
static void jd_importmap_read(void);
static int  jd_module_wanted(int i);
static int  jd_run_module_el(int node);
static void jd_inserted_module_due(jval arg);
static jval jd_import(jctx *J, jval spec, void *module);
static jval jd_import_meta(jctx *J, void *module);

/* document.doctype (jsform.h). */
static jval nat_doctype(jctx *J, jval t, jval *a, int n);

/* The moments of the page's navigation entry (jswin.h). */
enum { NV_INTERACTIVE, NV_DCL_START, NV_DCL_END, NV_COMPLETE, NV_LOAD_START, NV_LOAD_END, NV_COUNT };
static void jd_nav_mark(int which);

static void jd_remove(int child) {
    int parent = jd_doc->nodes[child].parent;
    if (parent < 0) return;
    int was_in = jd_ncustom && jd_connected_deep(child);
    int prev = jd_doc->nodes[child].prev, next = jd_doc->nodes[child].next;
    dom_unlink(jd_doc, child);
    jd_record_children(parent, -1, child, prev, next);
    jd_touched();
    jd_slots_touch(parent, child);
    if (was_in) jd_custom_disconnected(child);
}

static void jd_remove_children(int el) {
    while (jd_doc->nodes[el].first >= 0) jd_remove(jd_doc->nodes[el].first);
}

/* Whether a node may go under a parent: not inside itself, and never a
   document or a fragment as a child in its own right. */
static int jd_can_insert(jctx *J, int parent, int child) {
    if (parent < 0 || child < 0) {
        js_throw(J, JS_ERR_TYPE, "that is not a node", J->error_line);
        return 0;
    }
    if (jd_kind(parent) == JN_TEXT || jd_kind(parent) == JN_COMMENT || jd_kind(child) == JN_DOCUMENT
        || dom_contains(jd_doc, child, parent)) {
        js_throw_dom(J, "HierarchyRequestError", "a node cannot be put there");
        return 0;
    }
    return 1;
}

/* Puts child under parent before `before` (or at the end), moving it from
   wherever it was; a fragment gives up its children instead of going in. */
static void jd_insert(int parent, int child, int before) {
    if (before >= 0 && jd_doc->nodes[before].parent != parent) before = -1;
    if (jd_kind(child) == JN_FRAGMENT) {
        while (jd_doc->nodes[child].first >= 0) {
            int c = jd_doc->nodes[child].first;
            jd_remove(c);
            jd_insert(parent, c, before);
        }
        return;
    }
    if (child == before) return;
    if (jd_doc->nodes[child].parent >= 0) jd_remove(child);
    if (before >= 0) dom_insert_before(jd_doc, parent, child, before);
    else dom_append(jd_doc, parent, child);
    jd_record_children(parent, child, -1, jd_doc->nodes[child].prev, jd_doc->nodes[child].next);
    jd_touched();
    jd_slots_touch(parent, child);
    /* A script put into the page runs, which is how a page loads the rest
       of itself: a script element made, given a src and appended -- or made,
       appended, and then given its text. */
    if (jd_connected(child)) {
        jd_scripts_inserted(child);
        if (jd_doc->nodes[parent].tag == T_SCRIPT) jd_scripts_inserted(parent);
    }
    if (jd_connected_deep(child)) jd_custom_connected(child);
}

/* A value that may be a node or a string, as append() and before() take:
   the node, or a text node made of the string. */
static int jd_node_or_text(jctx *J, jval v) {
    int n = jd_node_of(v);
    if (n >= 0) return n;
    jstr *s = js_to_str(J, v);
    if (!s) return -1;
    return jd_new_text(s->s, (int)s->len);
}

/* Several of those as one fragment, which is how the standard has append,
   prepend, before, after and replaceWith take theirs. */
static int jd_nodes_arg(jctx *J, jval *a, int n) {
    if (n == 1) return jd_node_or_text(J, a[0]);
    int f = jd_new_fragment();
    if (f < 0) return -1;
    for (int i = 0; i < n; i++) {
        int c = jd_node_or_text(J, a[i]);
        if (J->sig != JS_OK || c < 0) return -1;
        if (!jd_can_insert(J, f, c)) return -1;
        jd_insert(f, c, -1);
    }
    return f;
}

/* --- markup out --------------------------------------------------------------------
 *
 * innerHTML and outerHTML, written the way the HTML standard serialises:
 * names in lower case, attribute values quoted, the characters that would
 * be read back as markup escaped, and the contents of script and style left
 * as they are, because they were never markup. */
static int jd_void(int el) {
    if (jd_doc->nodes[el].tag != T_OTHER) return html_void(jd_doc->nodes[el].tag);
    return jd_word_in("area base col embed param source track wbr keygen", dom_tag_name(jd_doc, el));
}

static int jd_raw_text(int el) {
    int t = jd_doc->nodes[el].tag;
    if (t == T_SCRIPT || t == T_STYLE || t == T_NOSCRIPT || t == T_IFRAME) return 1;
    return t == T_OTHER && jd_word_in("xmp noembed noframes plaintext", dom_tag_name(jd_doc, el));
}

static void jd_escape(jtext *t, const char *s, int attr) {
    for (; *s; s++) {
        if (*s == '&') jd_put(t, "&amp;");
        else if (*s == '<' && !attr) jd_put(t, "&lt;");
        else if (*s == '>' && !attr) jd_put(t, "&gt;");
        else if (*s == '"' && attr) jd_put(t, "&quot;");
        else jd_putc(t, *s);
    }
}

static void jd_serialise(int n, jtext *t, int self);

static void jd_serialise_children(int el, jtext *t) {
    for (int c = jd_doc->nodes[el].first; c >= 0; c = jd_doc->nodes[c].next)
        jd_serialise(c, t, 1);
}

static void jd_serialise(int n, jtext *t, int self) {
    int k = jd_kind(n);
    if (k == JN_TEXT) {
        int p = jd_doc->nodes[n].parent;
        if (p >= 0 && jd_raw_text(p)) jd_put(t, jd_text_of(n));
        else jd_escape(t, jd_text_of(n), 0);
        return;
    }
    if (k == JN_COMMENT) {
        jd_put(t, "<!--");
        jd_put(t, jd_text_of(n));
        jd_put(t, "-->");
        return;
    }
    if (k == JN_FRAGMENT || k == JN_DOCUMENT || !self) { jd_serialise_children(n, t); return; }
    const char *nm = dom_tag_name(jd_doc, n);
    jd_putc(t, '<');
    jd_put(t, nm);
    const dnode *x = &jd_doc->nodes[n];
    for (int i = 0; i < x->attr_n; i++) {
        const dattr *a = &jd_doc->attrs[x->attr_at + i];
        jd_putc(t, ' ');
        jd_put(t, jd_doc->arena + a->name);
        jd_put(t, "=\"");
        jd_escape(t, jd_doc->arena + a->value, 1);
        jd_putc(t, '"');
    }
    jd_putc(t, '>');
    if (jd_void(n)) return;
    jd_serialise_children(n, t);
    jd_put(t, "</");
    jd_put(t, nm);
    jd_putc(t, '>');
}

/* --- markup in ------------------------------------------------------------------------
 *
 * Read by the page's own reader (dom.h, dom_parse_into) into a fragment,
 * then put where it was asked for. A script in markup a script wrote does
 * not run, as the standard has it: innerHTML is how pages insert text they
 * were sent, and a script in that is the oldest way of running somebody
 * else's program on a page. */
static u8 jd_started[DOM_NODES / 8];      /* a script run, or never to be */

static void jd_mark_started(int n) {
    if (n >= 0 && n < DOM_NODES) jd_started[n >> 3] |= (u8)(1 << (n & 7));
}

static int jd_is_started(int n) {
    return n >= 0 && n < DOM_NODES && (jd_started[n >> 3] & (1 << (n & 7)));
}

static int jd_parse_fragment(const char *html, int len) {
    int f = jd_new_fragment();
    if (f < 0) return -1;
    int from = jd_doc->count;
    /* The parser takes the first head, body and title it meets for the
       page's (dom.h), and a fragment's are not: a page with no title whose
       script read a whole page into a DOMParser took that page's title. */
    int head = jd_doc->head, body = jd_doc->body, title = jd_doc->title, std = jd_doc->standards;
    dom_parse_into(jd_doc, html, len, f);
    jd_doc->head = head;
    jd_doc->body = body;
    jd_doc->title = title;
    jd_doc->standards = std;
    for (int i = from; i < jd_doc->count; i++) {
        if (jd_doc->nodes[i].kind != DN_ELEMENT) continue;
        if (jd_doc->nodes[i].tag == T_SCRIPT) jd_mark_started(i);
        if (dom_attr(jd_doc, i, "style")) jd_mark_restyle(i);
    }
    return f;
}

/* A template's contents are a fragment of their own (jd_template_content),
   which is where its markup is read and written. */
static int jd_template_content(int el);

static int jd_is_template(int el) {
    return jd_doc->nodes[el].tag == T_OTHER && w_same(dom_tag_name(jd_doc, el), "template");
}

static void jd_set_inner(int el, const char *html, int len) {
    if (jd_is_template(el)) el = jd_template_content(el);
    if (el < 0) return;
    jd_remove_children(el);

    /* Inside a script or a style sheet markup is not markup, and in a
       textarea or a title only its entities are: what is set is text, as the
       standard reads it there. Next.js sets the code of the scripts it makes
       this way, and a `<` in the code began a tag and cut the script off. */
    int tag = jd_doc->nodes[el].tag;
    if (jd_raw_text(el) || tag == T_TEXTAREA || tag == T_TITLE) {
        int t;
        if (tag == T_TEXTAREA || tag == T_TITLE) {
            /* A byte that is not part of a character comes out as three. */
            int cap = len * 3 + 8;
            char *out = (char *)malloc((u64)cap);
            if (!out) return;
            int w = 0;
            for (int i = 0; i < len; ) {
                char one[8];
                int used = 1, n = html_char(html + i, len - i, &used, one);
                for (int k = 0; k < n && w < cap; k++) out[w++] = one[k];
                i += used;
            }
            t = jd_new_text(out, w);
            free(out);
        } else {
            t = jd_new_text(html, len);
        }
        if (t >= 0) jd_insert(el, t, -1);
        return;
    }

    int f = jd_parse_fragment(html, len);
    if (f >= 0) jd_insert(el, f, -1);
    jd_custom_created(el);
}

/* --- copies ------------------------------------------------------------------------- */

static int jd_clone(int n, int deep) {
    int k = jd_kind(n);
    int c;
    if (k == JN_TEXT) return jd_new_text(jd_text_of(n), w_len(jd_text_of(n)));
    if (k == JN_COMMENT) return jd_new_comment(jd_text_of(n), w_len(jd_text_of(n)));
    const dnode *x = &jd_doc->nodes[n];
    c = dom_new(jd_doc, DN_ELEMENT, x->tag);
    if (c < 0) return -1;
    x = &jd_doc->nodes[n];
    jd_doc->nodes[c].text = x->text;              /* the name, shared: never written */
    /* Values copied rather than shared: a value that fits is rewritten where
       it lies (dom.h, dom_attr_set), and a shared one would change on both
       elements. Names are never written. */
    for (int i = 0; i < jd_doc->nodes[n].attr_n; i++) {
        const dattr *a = &jd_doc->attrs[jd_doc->nodes[n].attr_at + i];
        const char *v = jd_doc->arena + a->value;
        int nm = a->name;
        int vl = dom_str(jd_doc, v, w_len(v));
        dom_attr_add(jd_doc, c, nm, vl);
    }
    if (dom_attr(jd_doc, c, "style")) jd_mark_restyle(c);
    if (n < DOM_NODES && c < DOM_NODES && (jd_svg_made[n >> 3] & (1 << (n & 7))))
        jd_svg_made[c >> 3] |= (u8)(1 << (c & 7));
    if (!deep) return c;
    for (int ch = jd_doc->nodes[n].first; ch >= 0; ch = jd_doc->nodes[ch].next) {
        int cc = jd_clone(ch, 1);
        if (cc < 0) break;
        dom_append(jd_doc, c, cc);
    }
    return c;
}

/* --- walking the page ------------------------------------------------------------------
 *
 * In document order, under a node, leaving out what is inside a template:
 * a template's markup is a fragment of its own in the standard, not part of
 * the page (browser.c, in_template), and a search of the page does not find
 * it. The template element itself is found. */
static int jd_walk_next(int i, int top) {
    const dnode *x = &jd_doc->nodes[i];
    if (x->first >= 0 && !(x->kind == DN_ELEMENT && jd_is_template(i))) return x->first;
    while (i >= 0 && i != top) {
        if (jd_doc->nodes[i].next >= 0) return jd_doc->nodes[i].next;
        i = jd_doc->nodes[i].parent;
    }
    return -1;
}

/* The first node of a walk under top, or of the whole page for -1. */
static int jd_walk_first(int top) {
    return top < 0 ? jd_top() : jd_walk_next(top, top);
}

/* --- finding by selector ---------------------------------------------------------------
 *
 * The parser and the matcher are the ones the style sheets use, and that is
 * the whole point of doing it this way. A page's idea of what "nav >
 * a.current" picks out has to be the same whether it came from a sheet or
 * from a script; two implementations of that agree until they do not, and
 * the day they stop is the day a page styles one element and scripts
 * another.
 *
 * A selector arrives as text. It is parsed once on to the end of the
 * browser's own sheet, every element asked is matched against it, and the
 * sheet is rolled back -- nothing else runs in between, so the sheet
 * afterwards is the sheet before. It used to be parsed again for every
 * element asked, which for querySelectorAll over a whole page was ten
 * thousand parses of one selector. */
#define JD_SEL_PARTS 32

typedef struct {
    crule r[JD_SEL_PARTS];
    int   n;
    int   save_sels, save_used, save_over, save_negs;
} jd_selq;

static void jd_sel_open(jd_selq *q, const jstr *sel) {
    q->n = 0;
    if (!jd_sheet) return;
    q->save_sels = jd_sheet->nsels;
    q->save_used = jd_sheet->used;
    q->save_over = jd_sheet->overflowed;
    q->save_negs = jd_sheet->nnegs;
    int len = (int)sel->len, at = 0;
    while (at < len && q->n < JD_SEL_PARTS) {
        while (at < len && (sel->s[at] == ',' || css_space(sel->s[at]))) at++;
        if (at >= len) break;
        int sel_at = jd_sheet->nsels, spec = 0;
        int k = css_parse_selector(jd_sheet, sel->s, len, &at, &spec);
        /* A part this cannot read matches nothing, and neither does the
           rest of the list, whose end it cannot find. */
        if (k <= 0 || jd_sheet->overflowed) break;
        crule *r = &q->r[q->n++];
        r->sel_at = sel_at;
        r->sel_n = k;
        r->decl_at = r->decl_n = 0;
        r->spec = spec;
        r->order = 0;
        r->mq_lo = r->mq_hi = -1;
        r->layer = CSS_UNLAYERED;
    }
}

static int jd_sel_test(const jd_selq *q, int el) {
    if (jd_kind(el) != JN_ELEMENT) return 0;
    cmatch m;
    m.hover = -1;
    m.visited_links = 0;
    /* :root is the document element to a script (jd_top). The matcher is
       the style sheets' own (css.h), which knows the root by the document's
       record, so the record names the document element while a script asks
       and the root again after. */
    int root = jd_doc->root, hit = 0;
    jd_doc->root = jd_top();
    for (int i = 0; i < q->n && !hit; i++)
        if (css_matches(jd_sheet, jd_doc, el, &q->r[i], &m)) hit = 1;
    jd_doc->root = root;
    return hit;
}

static void jd_sel_close(jd_selq *q) {
    if (!jd_sheet) return;
    jd_sheet->nsels = q->save_sels;
    jd_sheet->used = q->save_used;
    jd_sheet->overflowed = q->save_over;
    jd_sheet->nnegs = q->save_negs;
}

static int jd_sel_matches(int el, const jstr *sel) {
    jd_selq q;
    jd_sel_open(&q, sel);
    int ok = jd_sel_test(&q, el);
    jd_sel_close(&q);
    return ok;
}

/* --- lists of nodes -----------------------------------------------------------------
 *
 * children, childNodes and getElementsByTagName give back lists that follow
 * the document: add a row and the list a page is holding has one more. So a
 * list is what it was asked for -- the node it is under and the question --
 * and its members are gathered again whenever the document has changed since
 * they were last (jd_version). querySelectorAll's is gathered once and kept,
 * as the standard has it.
 *
 * The records are the browser's memory, not the page's; the members are
 * numbers. A list's object carries its record's index in its host number. */
enum { JL_STATIC = 0, JL_CHILDREN, JL_CHILDNODES, JL_TAG, JL_CLASS, JL_NAME, JL_FORMS,
       JL_IMAGES, JL_LINKS, JL_SCRIPTS, JL_OPTIONS, JL_ELEMENTS, JL_DOCKIDS, JL_ROWS, JL_CELLS,
       JL_SELECTED, JL_LABELS };

typedef struct {
    u8    kind, html;          /* html: an HTMLCollection, else a NodeList */
    int   root;                /* the node it is under, -1 for the document */
    jstr *name;
    int  *nodes;
    u32   n, cap;
    u32   version;
    int   fresh;
    jobj *obj;
} jlist;

static jlist *jd_lists;
static int    jd_nlists, jd_caplists;

static void jd_list_add(jlist *L, int node) {
    if (L->n >= L->cap) {
        u32 cap = L->cap ? L->cap * 2 : 16;
        int *more = (int *)malloc((u64)cap * sizeof(int));
        if (!more) return;
        for (u32 i = 0; i < L->n; i++) more[i] = L->nodes[i];
        if (L->nodes) free(L->nodes);
        L->nodes = more;
        L->cap = cap;
    }
    L->nodes[L->n++] = node;
}

/* Whether an element carries every class in a space separated list. */
static int jd_has_classes(int el, const jstr *want) {
    int any = 0;
    for (u32 i = 0; i < want->len; ) {
        while (i < want->len && css_space(want->s[i])) i++;
        u32 s = i;
        while (i < want->len && !css_space(want->s[i])) i++;
        if (i == s) continue;
        any = 1;
        if (!dom_has_class(jd_doc, el, want->s + s, (int)(i - s))) return 0;
    }
    return any;
}

static int jd_is_form_control(int el) {
    int t = jd_doc->nodes[el].tag;
    if (t == T_INPUT || t == T_BUTTON || t == T_SELECT || t == T_TEXTAREA) return 1;
    return t == T_OTHER && jd_word_in("fieldset output object", dom_tag_name(jd_doc, el));
}

static int jd_list_wants(const jlist *L, int el) {
    if (L->kind == JL_CHILDNODES) return 1;
    if (jd_kind(el) != JN_ELEMENT) return 0;
    const dnode *x = &jd_doc->nodes[el];
    switch (L->kind) {
        case JL_CHILDREN: case JL_DOCKIDS: return 1;
        case JL_TAG:
            if (L->name->len == 1 && L->name->s[0] == '*') return 1;
            return w_same_fold(dom_tag_name(jd_doc, el), L->name->s);
        case JL_CLASS: return jd_has_classes(el, L->name);
        case JL_NAME: {
            const char *v = dom_attr(jd_doc, el, "name");
            return v && w_same(v, L->name->s);
        }
        case JL_FORMS: return x->tag == T_FORM;
        case JL_IMAGES: return x->tag == T_IMG;
        case JL_LINKS: return (x->tag == T_A || w_same(dom_tag_name(jd_doc, el), "area"))
                              && dom_attr(jd_doc, el, "href");
        case JL_SCRIPTS: return x->tag == T_SCRIPT;
        case JL_OPTIONS: return x->tag == T_OPTION;
        case JL_SELECTED: return x->tag == T_OPTION && dom_attr(jd_doc, el, "selected");
        case JL_ELEMENTS: return jd_is_form_control(el);
        case JL_ROWS: return x->tag == T_TR;
        case JL_CELLS: return x->tag == T_TD || x->tag == T_TH;
        case JL_LABELS: {
            if (x->tag != T_LABEL) return 0;
            const char *f = dom_attr(jd_doc, el, "for");
            return f && L->name && w_same(f, L->name->s);
        }
        default: return 0;
    }
}

static void jd_list_gather(jlist *L) {
    if (L->kind == JL_STATIC) return;
    if (L->fresh && L->version == jd_version) return;
    L->n = 0;
    L->fresh = 1;
    L->version = jd_version;
    if (L->kind == JL_DOCKIDS) { if (jd_top() >= 0) jd_list_add(L, jd_top()); return; }
    int top = L->root;
    if (L->kind == JL_CHILDREN || L->kind == JL_CHILDNODES) {
        if (top < 0) return;
        for (int c = jd_doc->nodes[top].first; c >= 0; c = jd_doc->nodes[c].next)
            if (jd_list_wants(L, c)) jd_list_add(L, c);
        return;
    }
    /* Under the node, not the node itself -- except the document's, which
       is under nothing and includes the root. */
    for (int i = jd_walk_first(top); i >= 0; i = jd_walk_next(i, top))
        if (jd_list_wants(L, i)) jd_list_add(L, i);
}

static jlist *jd_list_of(jval v) {
    if (v.t != JS_OBJ || !v.obj) return 0;
    int h = v.obj->host - JD_LIST;
    if (v.obj->host < JD_LIST || v.obj->host >= JD_STORAGE || h >= jd_nlists) return 0;
    return &jd_lists[h];
}

static int jd_list_new(int kind, int html, int root, jstr *name) {
    if (jd_nlists >= jd_caplists) {
        int cap = jd_caplists ? jd_caplists * 2 : 64;
        if (cap > (1 << 20)) return -1;
        /* In the region: a list holds its name and object (jsgc.h). */
        jlist *more = (jlist *)js_alloc(&jd_J, (u32)cap * (u32)sizeof(jlist));
        if (!more) return -1;
        volatile u8 *d = (volatile u8 *)more;
        const u8 *s = (const u8 *)jd_lists;
        for (u64 i = 0; i < (u64)jd_nlists * sizeof(jlist); i++) d[i] = s[i];
        if (jd_lists) js_free(&jd_J, jd_lists, (u32)jd_caplists * (u32)sizeof(jlist));
        jd_lists = more;
        jd_caplists = cap;
    }
    jlist *L = &jd_lists[jd_nlists];
    L->kind = (u8)kind;
    L->html = (u8)html;
    L->root = root;
    L->name = name;
    L->nodes = 0;
    L->n = L->cap = 0;
    L->version = 0;
    L->fresh = 0;
    L->obj = 0;
    return jd_nlists++;
}

static void jd_lists_free(void) {
    for (int i = 0; i < jd_nlists; i++) if (jd_lists[i].nodes) free(jd_lists[i].nodes);
    jd_lists = 0;                        /* the region's, gone with it */
    jd_nlists = jd_caplists = 0;
}

/* A live list, the same object for the same question, as browsers give
   back: getElementsByTagName('div') twice is one list. */
static jval jd_list_value(jctx *J, int kind, int html, int root, jstr *name) {
    for (int i = jd_nlists - 1, seen = 0; i >= 0 && seen < 256; i--, seen++) {
        jlist *L = &jd_lists[i];
        if (L->kind != kind || L->root != root || L->html != html || !L->obj) continue;
        if ((L->name == 0) != (name == 0)) continue;
        if (name && !js_str_eq(L->name, name)) continue;
        return js_from_obj(L->obj);
    }
    int at = jd_list_new(kind, html, root, name);
    if (at < 0) return js_null();
    jobj *o = js_object_with(J, JO_PLAIN, jd_p[html ? JI_HTMLCOLLECTION : JI_NODELIST]);
    if (!o) return js_null();
    o->host = JD_LIST + at;
    jd_lists[at].obj = o;
    return js_from_obj(o);
}

/* A list that does not follow the document, as querySelectorAll's. */
static jval jd_static_list(jctx *J, int *index) {
    int at = jd_list_new(JL_STATIC, 0, -1, 0);
    if (at < 0) return js_null();
    jobj *o = js_object_with(J, JO_PLAIN, jd_p[JI_NODELIST]);
    if (!o) return js_null();
    o->host = JD_LIST + at;
    jd_lists[at].obj = o;
    jd_lists[at].fresh = 1;
    *index = at;
    return js_from_obj(o);
}

/* The members now, as an array, for the methods that walk them. */
static jobj *jd_list_array(jctx *J, jlist *L) {
    jd_list_gather(L);
    jobj *arr = js_array(J);
    if (!arr) return 0;
    for (u32 i = 0; i < L->n; i++) js_arr_push(J, arr, jd_el_value(J, L->nodes[i]));
    return arr;
}

static jval nat_list_item(jctx *J, jval t, jval *a, int n) {
    jlist *L = jd_list_of(t);
    if (!L) return jd_illegal(J);
    jd_list_gather(L);
    double d = n > 0 ? js_to_num(J, a[0]) : 0;
    if (!(d >= 0) || d >= (double)L->n) return js_null();
    return jd_el_value(J, L->nodes[(u32)d]);
}

/* length, as an accessor on NodeList's and HTMLCollection's prototypes, where
   the standard has it and scripts take its getter from. */
static jval nat_list_length(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jlist *L = jd_list_of(t);
    if (!L) return jd_illegal(J);
    jd_list_gather(L);
    return js_num(L->n);
}

static int jd_list_find_name(jlist *L, const char *want) {
    jd_list_gather(L);
    for (u32 i = 0; i < L->n; i++) {
        const char *id = dom_attr(jd_doc, L->nodes[i], "id");
        const char *nm = dom_attr(jd_doc, L->nodes[i], "name");
        if ((id && w_same(want, id)) || (nm && w_same(want, nm))) return L->nodes[i];
    }
    return -1;
}

static jval nat_list_named(jctx *J, jval t, jval *a, int n) {
    jlist *L = jd_list_of(t);
    if (!L) return jd_illegal(J);
    jstr *want = jd_arg_str(J, a, n, 0);
    int at = jd_list_find_name(L, want->s);
    return at < 0 ? js_null() : jd_el_value(J, at);
}

/* forEach, entries, keys, values and iteration, over the members as they
   are when the walk starts. */
static jval nat_list_foreach(jctx *J, jval t, jval *a, int n) {
    jlist *L = jd_list_of(t);
    if (!L) return jd_illegal(J);
    jval fn = js_arg(a, n, 0);
    if (!js_callable(fn)) return js_throw(J, JS_ERR_TYPE, "forEach needs a function", J->error_line);
    jobj *arr = jd_list_array(J, L);
    if (!arr) return js_undef();
    for (u32 i = 0; i < arr->len && J->sig == JS_OK; i++) {
        jval args[3] = { arr->items[i], js_num(i), t };
        js_call(J, fn, js_arg(a, n, 1), args, 3);
    }
    return js_undef();
}

static jval jd_array_iter(jctx *J, jobj *arr, const char *which) {
    if (!arr) return js_undef();
    jval m = js_get(J, js_from_obj(J->p_array), js_str(J, which));
    return js_call(J, m, js_from_obj(arr), 0, 0);
}

static jval nat_list_values(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jlist *L = jd_list_of(t);
    return L ? jd_array_iter(J, jd_list_array(J, L), "values") : jd_illegal(J);
}

static jval nat_list_keys(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jlist *L = jd_list_of(t);
    return L ? jd_array_iter(J, jd_list_array(J, L), "keys") : jd_illegal(J);
}

static jval nat_list_entries(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jlist *L = jd_list_of(t);
    return L ? jd_array_iter(J, jd_list_array(J, L), "entries") : jd_illegal(J);
}

/* length and the members by number, asked through the host hook; for an
   HTMLCollection a member by its id or name too, but never over the
   prototype's own methods, which the standard's collections do not hide. */
static int jd_list_get(jctx *J, jobj *o, const char *name, jval *out) {
    jlist *L = jd_list_of(js_from_obj(o));
    if (!L) return 0;
    if (name[0] >= '0' && name[0] <= '9') {
        u32 i = 0;
        for (const char *p = name; *p; p++) {
            if (*p < '0' || *p > '9') return 0;
            if (i > 100000000) return 0;
            i = i * 10 + (u32)(*p - '0');
        }
        jd_list_gather(L);
        *out = i < L->n ? jd_el_value(J, L->nodes[i]) : js_undef();
        return 1;
    }
    if (w_same(name, "length")) {
        jd_list_gather(L);
        *out = js_num((double)L->n);
        return 1;
    }
    if (L->html) {
        for (jobj *p = o->proto; p; p = p->proto)
            if (js_find(p, js_str(J, name))) return 0;
        int at = jd_list_find_name(L, name);
        if (at >= 0) { *out = jd_el_value(J, at); return 1; }
    }
    return 0;
}

/* --- a class at a time ------------------------------------------------------------------
 *
 * classList is a list in name and a string underneath, and every page that
 * turns something on and off uses it. Its object remembers which element it
 * belongs to by the same number an element carries, moved out of the way of
 * one; the methods are on DOMTokenList.prototype. */
static int jd_token_node(jval t) {
    if (t.t != JS_OBJ || !t.obj) return -1;
    if (t.obj->host < JD_CLASSLIST || t.obj->host >= JD_STYLE) return -1;
    int h = t.obj->host - JD_CLASSLIST;
    return (!jd_doc || h >= jd_doc->count) ? -1 : h;
}

/* The nth word of a space separated attribute, or -1 past the end. */
static int jd_token_at(const char *v, int nth, int *len) {
    int k = 0;
    for (int i = 0; v && v[i]; ) {
        while (v[i] && css_space(v[i])) i++;
        int s = i;
        while (v[i] && !css_space(v[i])) i++;
        if (i == s) break;
        if (k++ == nth) { *len = i - s; return s; }
    }
    return -1;
}

static int jd_token_count(const char *v) {
    int n = 0, len;
    while (jd_token_at(v, n, &len) >= 0) n++;
    return n;
}

/* The class attribute with one name added or taken out, written whole: the
   attribute is one string and a class is a word inside it. A repeated word
   is written once, as the standard's writing of the set does. */
static void jd_token_write(int el, const jstr *want, int on) {
    const char *cur = jd_attr(el, "class");
    jtext t = { 0, 0, 0, 0 };
    int present = 0, len, at;
    for (int k = 0; (at = jd_token_at(cur, k, &len)) >= 0; k++) {
        int same = (u32)len == want->len && jd_same_n(cur + at, want->s, len);
        if (same) { present = 1; if (!on) continue; }
        int dup = 0, l2, j;
        for (int q = 0; !dup && q < k && (j = jd_token_at(cur, q, &l2)) >= 0; q++)
            if (l2 == len && jd_same_n(cur + j, cur + at, len)) dup = 1;
        if (dup) continue;
        if (t.n) jd_putc(&t, ' ');
        jt_put(&jd_J, &t, cur + at, (u32)len);
    }
    if (on && !present) {
        if (t.n) jd_putc(&t, ' ');
        jt_put(&jd_J, &t, want->s, want->len);
    }
    jd_putc(&t, 0);
    jd_attr_set(el, "class", t.b ? t.b : "");
    free(t.b);
}

static int jd_token_has(int el, const jstr *want) {
    return dom_has_class(jd_doc, el, want->s, (int)want->len);
}

/* A token with a space in it, or none, is refused as the standard refuses
   it, rather than written into the attribute as two classes. */
static int jd_token_ok(jctx *J, const jstr *s) {
    if (!s->len) { js_throw_dom(J, "SyntaxError", "a class cannot be empty"); return 0; }
    for (u32 i = 0; i < s->len; i++)
        if (css_space(s->s[i])) {
            js_throw_dom(J, "InvalidCharacterError", "a class cannot have a space in it");
            return 0;
        }
    return 1;
}

static jval nat_cls_add(jctx *J, jval t, jval *a, int n) {
    int el = jd_token_node(t);
    if (el < 0) return jd_illegal(J);
    for (int i = 0; i < n; i++) {
        jstr *s = js_to_str(J, a[i]);
        if (!s || !jd_token_ok(J, s)) return js_undef();
        if (!jd_token_has(el, s)) jd_token_write(el, s, 1);
    }
    return js_undef();
}

static jval nat_cls_remove(jctx *J, jval t, jval *a, int n) {
    int el = jd_token_node(t);
    if (el < 0) return jd_illegal(J);
    for (int i = 0; i < n; i++) {
        jstr *s = js_to_str(J, a[i]);
        if (!s || !jd_token_ok(J, s)) return js_undef();
        if (jd_token_has(el, s)) jd_token_write(el, s, 0);
    }
    return js_undef();
}

static jval nat_cls_contains(jctx *J, jval t, jval *a, int n) {
    int el = jd_token_node(t);
    if (el < 0) return jd_illegal(J);
    jstr *s = jd_arg_str(J, a, n, 0);
    return js_bool(s->len && jd_token_has(el, s));
}

static jval nat_cls_toggle(jctx *J, jval t, jval *a, int n) {
    int el = jd_token_node(t);
    if (el < 0) return jd_illegal(J);
    jstr *s = jd_arg_str(J, a, n, 0);
    if (!jd_token_ok(J, s)) return js_undef();
    int has = jd_token_has(el, s);
    int on = n > 1 && a[1].t != JS_UNDEF ? js_to_bool(a[1]) : !has;
    if (on != has) jd_token_write(el, s, on);
    return js_bool(on);
}

static jval nat_cls_replace(jctx *J, jval t, jval *a, int n) {
    int el = jd_token_node(t);
    if (el < 0) return jd_illegal(J);
    jstr *old = jd_arg_str(J, a, n, 0), *nw = jd_arg_str(J, a, n, 1);
    if (!jd_token_ok(J, old) || !jd_token_ok(J, nw)) return js_undef();
    if (!jd_token_has(el, old)) return js_bool(0);
    jd_token_write(el, old, 0);
    jd_token_write(el, nw, 1);
    return js_bool(1);
}

static jval nat_cls_item(jctx *J, jval t, jval *a, int n) {
    int el = jd_token_node(t);
    if (el < 0) return jd_illegal(J);
    double d = n > 0 ? js_to_num(J, a[0]) : 0;
    const char *v = jd_attr(el, "class");
    int len;
    int at = d >= 0 && d < 100000 ? jd_token_at(v, (int)d, &len) : -1;
    return at < 0 ? js_null() : jd_str_n(v + at, len);
}

static jval nat_cls_value(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n; (void)J;
    int el = jd_token_node(t);
    if (el < 0) return js_undef();
    const char *v = jd_attr(el, "class");
    return jd_str(v ? v : "");
}

static jval nat_cls_set_value(jctx *J, jval t, jval *a, int n) {
    int el = jd_token_node(t);
    if (el < 0) return js_undef();
    jd_attr_set(el, "class", jd_arg_str(J, a, n, 0)->s);
    return js_undef();
}

static jval nat_cls_length(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n; (void)J;
    int el = jd_token_node(t);
    return el < 0 ? js_undef() : js_num(jd_token_count(jd_attr(el, "class")));
}

static jval nat_cls_supports(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    return js_throw(J, JS_ERR_TYPE, "classList has no list of supported tokens", J->error_line);
}

/* The words as an array, for forEach and iteration. */
static jobj *jd_token_array(jctx *J, int el) {
    jobj *arr = js_array(J);
    const char *v = jd_attr(el, "class");
    int len, at;
    for (int k = 0; arr && (at = jd_token_at(v, k, &len)) >= 0; k++)
        js_arr_push(J, arr, js_from_str(js_str_n(J, v + at, (u32)len)));
    return arr;
}

static jval nat_cls_values(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int el = jd_token_node(t);
    return el < 0 ? jd_illegal(J) : jd_array_iter(J, jd_token_array(J, el), "values");
}

static jval nat_cls_foreach(jctx *J, jval t, jval *a, int n) {
    int el = jd_token_node(t);
    if (el < 0) return jd_illegal(J);
    jval fn = js_arg(a, n, 0);
    if (!js_callable(fn)) return js_throw(J, JS_ERR_TYPE, "forEach needs a function", J->error_line);
    jobj *arr = jd_token_array(J, el);
    for (u32 i = 0; arr && i < arr->len && J->sig == JS_OK; i++) {
        jval args[3] = { arr->items[i], js_num(i), t };
        js_call(J, fn, js_arg(a, n, 1), args, 3);
    }
    return js_undef();
}

static int jd_token_get(jctx *J, jobj *o, const char *name, jval *out) {
    int el = jd_token_node(js_from_obj(o));
    if (el < 0 || name[0] < '0' || name[0] > '9') return 0;
    int i = 0;
    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9' || i > 100000) return 0;
        i = i * 10 + (*p - '0');
    }
    const char *v = jd_attr(el, "class");
    int len, at = jd_token_at(v, i, &len);
    *out = at < 0 ? js_undef() : js_from_str(js_str_n(J, v + at, (u32)len));
    return 1;
}

static jobj *jd_classlist(jctx *J, int el) {
    jobj *w = jd_element(J, el);
    jval kept = jd_kept(w, jd_k_classlist);
    if (kept.t == JS_OBJ) return kept.obj;
    jobj *o = js_object_with(J, JO_PLAIN, jd_p[JI_TOKENLIST]);
    if (!o) return 0;
    o->host = JD_CLASSLIST + el;
    jd_keep(w, jd_k_classlist, js_from_obj(o));
    return o;
}

/* --- style, as an object that writes the style attribute ---------------------------------
 *
 * el.style.display = 'none' is the other thing every page does. The object
 * is a view of the attribute: reading a property finds its declaration in
 * it, writing one rewrites the attribute with that declaration changed, and
 * the browser reads the attribute again before it next lays the page out
 * (jd_mark_restyle).
 *
 * It answers for the properties this browser's style sheets know (css.h:
 * css_prop_of, and the shorthands css_declare splits), and custom
 * properties through setProperty. A property this browser does not lay out
 * is not one el.style has, which is the honest answer to a page asking
 * `'transition' in el.style` before it relies on one. */
static const char JD_SHORTHANDS[] =
    " margin padding border border-width border-top border-right border-bottom border-left"
    " font list-style background ";

/* camelCase to the property's name: backgroundColor is background-color,
   cssFloat is float, webkitTransform is -webkit-transform. */
static int jd_css_name(const char *in, char *out, int cap) {
    int w = 0;
    if (w_same(in, "cssFloat")) in = "float";
    if (in[0] == '-' && in[1] == '-') {
        for (; *in && w < cap - 1; in++) out[w++] = *in;
        out[w] = 0;
        return w;
    }
    if ((in[0] == 'w' && w_starts_fold(in, "webkit") && in[6] >= 'A' && in[6] <= 'Z')
        || (in[0] == 'm' && in[1] == 'o' && in[2] == 'z' && in[3] >= 'A' && in[3] <= 'Z'))
        out[w++] = '-';
    for (; *in && w < cap - 2; in++) {
        char c = *in;
        if (c >= 'A' && c <= 'Z') { out[w++] = '-'; out[w++] = (char)(c + 32); }
        else out[w++] = c;
    }
    out[w] = 0;
    return w;
}

static int jd_css_known(const char *prop) {
    if (prop[0] == '-' && prop[1] == '-') return 1;
    if (css_prop_of(prop) != P_NONE) return 1;
    return jd_word_in(JD_SHORTHANDS, prop);
}

/* The declarations of a style attribute, one at a time: where the name and
   the value are, and the value without !important. */
typedef struct { int ns, nl, vs, vl, imp; } jd_decl;

static int jd_decl_next(const char *st, int *at, jd_decl *d) {
    int p = *at, len = w_len(st);
    while (p < len) {
        while (p < len && (css_space(st[p]) || st[p] == ';')) p++;
        if (p >= len) break;
        d->ns = p;
        while (p < len && st[p] != ':' && st[p] != ';') p++;
        d->nl = p - d->ns;
        while (d->nl > 0 && css_space(st[d->ns + d->nl - 1])) d->nl--;
        if (p >= len || st[p] != ':') { while (p < len && st[p] != ';') p++; continue; }
        p++;
        while (p < len && css_space(st[p])) p++;
        d->vs = p;
        int depth = 0;
        char quote = 0;
        while (p < len) {
            char c = st[p];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '(') depth++;
            else if (c == ')') { if (depth) depth--; }
            else if (!depth && c == ';') break;
            p++;
        }
        d->vl = p - d->vs;
        while (d->vl > 0 && css_space(st[d->vs + d->vl - 1])) d->vl--;
        d->imp = 0;
        if (d->vl >= 10) {
            int b = d->vl;
            while (b > 0 && st[d->vs + b - 1] != '!') b--;
            if (b > 0 && w_starts_fold(st + d->vs + b, "important")) {
                d->imp = 1;
                d->vl = b - 1;
                while (d->vl > 0 && css_space(st[d->vs + d->vl - 1])) d->vl--;
            }
        }
        *at = p;
        return 1;
    }
    *at = p;
    return 0;
}

static int jd_decl_is(const char *st, const jd_decl *d, const char *prop) {
    int n = w_len(prop);
    if (d->nl != n) return 0;
    for (int i = 0; i < n; i++) if (w_lower(st[d->ns + i]) != prop[i]) return 0;
    return 1;
}

/* A block of declarations: an element's style attribute, or a style rule's
   declarations (jscssom.h), which one set of accessors reads and writes. */
typedef struct { int el; jobj *rule; } jd_decls;

static const char *jcs_rule_decls(jobj *rule);
static void jcs_rule_set_decls(jobj *rule, const char *text);
static jobj *jcs_style_rule(jval t);

static const char *jd_decls_text(const jd_decls *d) {
    return d->el >= 0 ? jd_attr(d->el, "style") : jcs_rule_decls(d->rule);
}

static void jd_decls_store(const jd_decls *d, const char *text) {
    if (d->el >= 0) jd_attr_set(d->el, "style", text);
    else jcs_rule_set_decls(d->rule, text);
}

/* A property's value, "" when it is not set, and whether it was important. */
static jval jd_style_value(const jd_decls *src, const char *prop, int *imp) {
    const char *st = jd_decls_text(src);
    if (imp) *imp = 0;
    if (!st) return jd_str("");
    jd_decl d;
    int at = 0, found = 0;
    jd_decl last;
    while (jd_decl_next(st, &at, &d))
        if (jd_decl_is(st, &d, prop)) { last = d; found = 1; }
    if (!found) return jd_str("");
    if (imp) *imp = last.imp;
    return jd_str_n(st + last.vs, last.vl);
}

/* The attribute again with one property set to a value, or taken out when
   the value is empty. */
static void jd_style_write(const jd_decls *src, const char *prop, const char *val, int imp) {
    const char *st = jd_decls_text(src);
    jtext t = { 0, 0, 0, 0 };
    jd_decl d;
    int at = 0, done = 0;
    while (st && jd_decl_next(st, &at, &d)) {
        if (jd_decl_is(st, &d, prop)) {
            if (done || !*val) continue;
            done = 1;
            if (t.n) jd_putc(&t, ' ');
            jd_put(&t, prop);
            jd_put(&t, ": ");
            jd_put(&t, val);
            if (imp) jd_put(&t, " !important");
            jd_putc(&t, ';');
            continue;
        }
        if (t.n) jd_putc(&t, ' ');
        jt_put(&jd_J, &t, st + d.ns, (u32)d.nl);
        jd_put(&t, ": ");
        jt_put(&jd_J, &t, st + d.vs, (u32)d.vl);
        if (d.imp) jd_put(&t, " !important");
        jd_putc(&t, ';');
    }
    if (!done && *val) {
        if (t.n) jd_putc(&t, ' ');
        jd_put(&t, prop);
        jd_put(&t, ": ");
        jd_put(&t, val);
        if (imp) jd_put(&t, " !important");
        jd_putc(&t, ';');
    }
    jd_putc(&t, 0);
    jd_decls_store(src, t.b ? t.b : "");
    free(t.b);
}

static int jd_style_node(jval t) {
    if (t.t != JS_OBJ || !t.obj) return -1;
    if (t.obj->host < JD_STYLE || t.obj->host >= JD_DATASET) return -1;
    int h = t.obj->host - JD_STYLE;
    return (!jd_doc || h >= jd_doc->count) ? -1 : h;
}

static int jd_decls_of(jval t, jd_decls *d) {
    d->el = jd_style_node(t);
    d->rule = d->el < 0 ? jcs_style_rule(t) : 0;
    return d->el >= 0 || d->rule;
}

/* A value as a style takes it: a number is a number, null is nothing. */
static jstr *jd_style_arg(jctx *J, jval v) {
    if (v.t == JS_NULL || v.t == JS_UNDEF) return js_str(J, "");
    return js_to_str(J, v);
}

static jval nat_style_get(jctx *J, jval t, jval *a, int n) {
    jd_decls src;
    if (!jd_decls_of(t, &src)) return jd_illegal(J);
    char prop[64];
    jstr *s = jd_arg_str(J, a, n, 0);
    int k = 0;
    for (; k < (int)s->len && k < 63; k++) prop[k] = s->s[0] == '-' && s->s[1] == '-' ? s->s[k] : w_lower(s->s[k]);
    prop[k] = 0;
    return jd_style_value(&src, prop, 0);
}

static jval nat_style_priority(jctx *J, jval t, jval *a, int n) {
    jd_decls src;
    if (!jd_decls_of(t, &src)) return jd_illegal(J);
    char prop[64];
    jstr *s = jd_arg_str(J, a, n, 0);
    int k = 0;
    for (; k < (int)s->len && k < 63; k++) prop[k] = w_lower(s->s[k]);
    prop[k] = 0;
    int imp;
    jd_style_value(&src, prop, &imp);
    return jd_str(imp ? "important" : "");
}

static jval nat_style_set(jctx *J, jval t, jval *a, int n) {
    jd_decls src;
    if (!jd_decls_of(t, &src)) return jd_illegal(J);
    jstr *s = jd_arg_str(J, a, n, 0);
    char prop[64];
    int k = 0, custom = s->len > 2 && s->s[0] == '-' && s->s[1] == '-';
    for (; k < (int)s->len && k < 63; k++) prop[k] = custom ? s->s[k] : w_lower(s->s[k]);
    prop[k] = 0;
    if (!jd_css_known(prop)) return js_undef();
    jstr *v = jd_style_arg(J, js_arg(a, n, 1));
    jstr *pri = n > 2 ? jd_arg_str(J, a, n, 2) : 0;
    if (J->sig != JS_OK) return js_undef();
    jd_style_write(&src, prop, v->s, pri && js_str_is(pri, "important"));
    return js_undef();
}

static jval nat_style_remove(jctx *J, jval t, jval *a, int n) {
    jd_decls src;
    if (!jd_decls_of(t, &src)) return jd_illegal(J);
    jstr *s = jd_arg_str(J, a, n, 0);
    char prop[64];
    int k = 0, custom = s->len > 2 && s->s[0] == '-' && s->s[1] == '-';
    for (; k < (int)s->len && k < 63; k++) prop[k] = custom ? s->s[k] : w_lower(s->s[k]);
    prop[k] = 0;
    jval old = jd_style_value(&src, prop, 0);
    jd_style_write(&src, prop, "", 0);
    return old;
}

static jval nat_style_csstext(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n; (void)J;
    jd_decls src;
    if (!jd_decls_of(t, &src)) return js_undef();
    const char *st = jd_decls_text(&src);
    return jd_str(st ? st : "");
}

static jval nat_style_set_csstext(jctx *J, jval t, jval *a, int n) {
    jd_decls src;
    if (!jd_decls_of(t, &src)) return js_undef();
    jd_decls_store(&src, jd_style_arg(J, js_arg(a, n, 0))->s);
    return js_undef();
}

static int jd_style_count(const jd_decls *src) {
    const char *st = jd_decls_text(src);
    jd_decl d;
    int at = 0, k = 0;
    while (st && jd_decl_next(st, &at, &d)) k++;
    return k;
}

static jval nat_style_length(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n; (void)J;
    jd_decls src;
    return !jd_decls_of(t, &src) ? js_undef() : js_num(jd_style_count(&src));
}

static jval jd_style_nth(const jd_decls *src, int i) {
    const char *st = jd_decls_text(src);
    jd_decl d;
    int at = 0, k = 0;
    while (st && jd_decl_next(st, &at, &d)) {
        if (k++ != i) continue;
        char low[64];
        int m = d.nl < 63 ? d.nl : 63;
        for (int q = 0; q < m; q++) low[q] = w_lower(st[d.ns + q]);
        low[m] = 0;
        return jd_str(low);
    }
    return jd_str("");
}

static jval nat_style_item(jctx *J, jval t, jval *a, int n) {
    jd_decls src;
    if (!jd_decls_of(t, &src)) return jd_illegal(J);
    double d = n > 0 ? js_to_num(J, a[0]) : 0;
    return d >= 0 && d < 10000 ? jd_style_nth(&src, (int)d) : jd_str("");
}

static int jd_style_get(jctx *J, jobj *o, const char *name, jval *out) {
    (void)J;
    jd_decls src;
    if (!jd_decls_of(js_from_obj(o), &src)) return 0;
    if (name[0] >= '0' && name[0] <= '9') {
        int i = 0;
        for (const char *p = name; *p; p++) {
            if (*p < '0' || *p > '9' || i > 100000) return 0;
            i = i * 10 + (*p - '0');
        }
        *out = i < jd_style_count(&src) ? jd_style_nth(&src, i) : js_undef();
        return 1;
    }
    char prop[64];
    jd_css_name(name, prop, (int)sizeof(prop));
    if (!jd_css_known(prop) || (prop[0] == '-' && prop[1] == '-')) return 0;
    *out = jd_style_value(&src, prop, 0);
    return 1;
}

static int jd_style_put(jctx *J, jobj *o, const char *name, jval v) {
    jd_decls src;
    if (!jd_decls_of(js_from_obj(o), &src)) return 0;
    char prop[64];
    jd_css_name(name, prop, (int)sizeof(prop));
    if (!jd_css_known(prop) || (prop[0] == '-' && prop[1] == '-')) return 0;
    jstr *s = jd_style_arg(J, v);
    if (s) jd_style_write(&src, prop, s->s, 0);
    return 1;
}

static jobj *jd_style_obj(jctx *J, int el) {
    jobj *w = jd_element(J, el);
    jval kept = jd_kept(w, jd_k_style);
    if (kept.t == JS_OBJ) return kept.obj;
    jobj *o = js_object_with(J, JO_PLAIN, jd_p[JI_STYLEDECL]);
    if (!o) return 0;
    o->host = JD_STYLE + el;
    jd_keep(w, jd_k_style, js_from_obj(o));
    return o;
}

/* --- dataset ----------------------------------------------------------------------------
 *
 * The data- attributes by a camelCase name: dataset.userId is data-user-id.
 * What is read is the attribute as it is; what is written writes it. */
static int jd_dataset_node(jobj *o) {
    if (o->host < JD_DATASET || o->host >= JD_ATTRS) return -1;
    int h = o->host - JD_DATASET;
    return (!jd_doc || h >= jd_doc->count) ? -1 : h;
}

static void jd_data_name(const char *camel, char *out, int cap) {
    int w = 0;
    const char *pre = "data-";
    for (int i = 0; pre[i]; i++) out[w++] = pre[i];
    for (; *camel && w < cap - 2; camel++) {
        char c = *camel;
        if (c >= 'A' && c <= 'Z') { out[w++] = '-'; out[w++] = (char)(c + 32); }
        else out[w++] = c;
    }
    out[w] = 0;
}

static int jd_dataset_get(jctx *J, jobj *o, const char *name, jval *out) {
    (void)J;
    int el = jd_dataset_node(o);
    if (el < 0) return 0;
    char attr[96];
    jd_data_name(name, attr, (int)sizeof(attr));
    const char *v = jd_attr(el, attr);
    if (!v) return 0;
    *out = jd_str(v);
    return 1;
}

static int jd_dataset_put(jctx *J, jobj *o, const char *name, jval v) {
    int el = jd_dataset_node(o);
    if (el < 0) return 0;
    char attr[96];
    jd_data_name(name, attr, (int)sizeof(attr));
    jstr *s = js_to_str(J, v);
    if (s) jd_attr_set(el, attr, s->s);
    return 1;
}

static jobj *jd_dataset_obj(jctx *J, int el) {
    jobj *w = jd_element(J, el);
    jval kept = jd_kept(w, jd_k_dataset);
    if (kept.t == JS_OBJ) return kept.obj;
    jobj *o = js_object_with(J, JO_PLAIN, jd_p[JI_STRINGMAP]);
    if (!o) return 0;
    o->host = JD_DATASET + el;
    jd_keep(w, jd_k_dataset, js_from_obj(o));
    return o;
}

/* --- attributes as a list -----------------------------------------------------------------
 *
 * el.attributes, which templating libraries walk to find what an element
 * was written with. Each is handed back as an Attr made when asked: its
 * name and value as they are then. */
static int jd_attrs_node(jval t) {
    if (t.t != JS_OBJ || !t.obj) return -1;
    if (t.obj->host < JD_ATTRS || t.obj->host >= JD_LIST) return -1;
    int h = t.obj->host - JD_ATTRS;
    return (!jd_doc || h >= jd_doc->count) ? -1 : h;
}

static jstr *jd_k_attr_value;

static jval jd_attr_obj(jctx *J, int el, int i) {
    const dnode *x = &jd_doc->nodes[el];
    if (i < 0 || i >= x->attr_n) return js_null();
    const dattr *a = &jd_doc->attrs[x->attr_at + i];
    jobj *o = js_object_with(J, JO_PLAIN, jd_p[JI_ATTR]);
    if (!o) return js_null();
    jval nm = jd_str(jd_doc->arena + a->name);
    js_set(J, o, "name", nm);
    js_set(J, o, "localName", nm);
    js_set(J, o, "nodeName", nm);
    /* The value is read through the element while it holds this (jsform.h,
       nat_attr_value); the one kept is for when it no longer does. */
    jd_keep(o, jd_k_attr_value, jd_str(jd_doc->arena + a->value));
    js_set(J, o, "nodeType", js_num(2));
    js_set(J, o, "specified", js_bool(1));
    js_set(J, o, "namespaceURI", js_null());
    js_set(J, o, "prefix", js_null());
    js_set(J, o, "ownerElement", jd_el_value(J, el));
    return js_from_obj(o);
}

static int jd_attr_index(int el, const char *name) {
    const dnode *x = &jd_doc->nodes[el];
    for (int i = 0; i < x->attr_n; i++)
        if (w_same_fold(jd_doc->arena + jd_doc->attrs[x->attr_at + i].name, name)) return i;
    return -1;
}

static jval nat_attrs_item(jctx *J, jval t, jval *a, int n) {
    int el = jd_attrs_node(t);
    if (el < 0) return jd_illegal(J);
    double d = n > 0 ? js_to_num(J, a[0]) : 0;
    return d >= 0 && d < 100000 ? jd_attr_obj(J, el, (int)d) : js_null();
}

static jval nat_attrs_named(jctx *J, jval t, jval *a, int n) {
    int el = jd_attrs_node(t);
    if (el < 0) return jd_illegal(J);
    return jd_attr_obj(J, el, jd_attr_index(el, jd_arg_str(J, a, n, 0)->s));
}

static jval nat_attrs_remove(jctx *J, jval t, jval *a, int n) {
    int el = jd_attrs_node(t);
    if (el < 0) return jd_illegal(J);
    jstr *s = jd_arg_str(J, a, n, 0);
    jval old = jd_attr_obj(J, el, jd_attr_index(el, s->s));
    if (old.t != JS_OBJ) return js_throw_dom(J, "NotFoundError", "there is no such attribute");
    jd_attr_remove(el, s->s);
    return old;
}

static jval nat_attrs_values(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int el = jd_attrs_node(t);
    if (el < 0) return jd_illegal(J);
    jobj *arr = js_array(J);
    for (int i = 0; arr && i < jd_doc->nodes[el].attr_n; i++) js_arr_push(J, arr, jd_attr_obj(J, el, i));
    return jd_array_iter(J, arr, "values");
}

static int jd_attrs_get(jctx *J, jobj *o, const char *name, jval *out) {
    int el = jd_attrs_node(js_from_obj(o));
    if (el < 0) return 0;
    if (w_same(name, "length")) { *out = js_num(jd_doc->nodes[el].attr_n); return 1; }
    if (name[0] >= '0' && name[0] <= '9') {
        int i = 0;
        for (const char *p = name; *p; p++) {
            if (*p < '0' || *p > '9' || i > 100000) return 0;
            i = i * 10 + (*p - '0');
        }
        *out = i < jd_doc->nodes[el].attr_n ? jd_attr_obj(J, el, i) : js_undef();
        return 1;
    }
    for (jobj *p = o->proto; p; p = p->proto)
        if (js_find(p, js_str(J, name))) return 0;
    int i = jd_attr_index(el, name);
    if (i < 0) return 0;
    *out = jd_attr_obj(J, el, i);
    return 1;
}

static jobj *jd_attrs_obj(jctx *J, int el) {
    jobj *w = jd_element(J, el);
    jval kept = jd_kept(w, jd_k_attrs);
    if (kept.t == JS_OBJ) return kept.obj;
    jobj *o = js_object_with(J, JO_PLAIN, jd_p[JI_NAMEDNODEMAP]);
    if (!o) return 0;
    o->host = JD_ATTRS + el;
    jd_keep(w, jd_k_attrs, js_from_obj(o));
    return o;
}

/* --- what the browser knows that the document does not -----------------------------------
 *
 * Where the layout put things, how large a picture was when decoded, and the
 * window: the browser's to say, through these, set with jsdom_boxes_with and
 * jsdom_view. Before the page is laid out a box is nowhere, and says so. */
static int (*jd_box_of)(int node, int *x, int *y, int *w, int *h);
static int (*jd_node_at)(int x, int y);  /* what the layout drew at a point of the page */
static int (*jd_picture_size)(int node, int *w, int *h);
static int jd_view_w = 0, jd_view_h = 0, jd_scroll_y = 0;

/* A box in the window's terms: the layout's are down the page. */
static int jd_box(int node, int *x, int *y, int *w, int *h) {
    *x = *y = *w = *h = 0;
    if (!jd_box_of || node < 0 || !jd_box_of(node, x, y, w, h)) return 0;
    *y -= jd_scroll_y;
    return 1;
}

/* An address the page wrote, made whole against where the page is. The
   URL module does this properly once it is there (jsurl.h); this is the
   browser's own rule for links, which is what a page's links go by. */
static int jd_resolve(const char *href, char *out, int cap);
static jstr *jd_resolve_str(jctx *J, const char *href);

/* --- nodes ------------------------------------------------------------------------------- */

static jval nat_node_type(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (jd_is_doc(t)) return js_num(JN_DOCUMENT);
    int x = jd_node_of(t);
    return x < 0 ? js_undef() : js_num(jd_kind(x));
}

static jval jd_name_of(int x) {
    switch (jd_kind(x)) {
        case JN_TEXT: return jd_str("#text");
        case JN_COMMENT: return jd_str("#comment");
        case JN_FRAGMENT: return jd_str("#document-fragment");
        case JN_DOCUMENT: return jd_str("#document");
        default: break;
    }
    char buf[64];
    jd_tag_name_into(x, buf, (int)sizeof(buf), 1);
    return jd_str(buf);
}

static jval nat_node_name(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (jd_is_doc(t)) return jd_str("#document");
    int x = jd_node_of(t);
    return x < 0 ? js_undef() : jd_name_of(x);
}

static jval nat_node_value(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0 || jd_kind(x) == JN_ELEMENT || jd_kind(x) == JN_FRAGMENT || jd_kind(x) == JN_DOCUMENT) return js_null();
    return jd_str(jd_text_of(x));
}

static jval nat_node_set_value(jctx *J, jval t, jval *a, int n) {
    int x = jd_node_of(t);
    if (x < 0 || jd_kind(x) == JN_ELEMENT || jd_kind(x) == JN_FRAGMENT || jd_kind(x) == JN_DOCUMENT) return js_undef();
    jval v = js_arg(a, n, 0);
    jstr *s = v.t == JS_NULL ? js_str(J, "") : js_to_str(J, v);
    if (s) jd_set_data(x, s->s, (int)s->len);
    return js_undef();
}

static jval nat_text_content(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0) return js_null();
    int k = jd_kind(x);
    if (k == JN_DOCUMENT) return js_null();
    if (k == JN_TEXT || k == JN_COMMENT) return jd_str(jd_text_of(x));
    return jd_text_value(x);
}

static void jd_replace_with_text(int x, const jstr *s) {
    jd_remove_children(x);
    if (s->len) {
        int tn = jd_new_text(s->s, (int)s->len);
        if (tn >= 0) jd_insert(x, tn, -1);
    }
    jd_touched();
}

static jval nat_set_text_content(jctx *J, jval t, jval *a, int n) {
    int x = jd_node_of(t);
    if (x < 0) return js_undef();
    jval v = js_arg(a, n, 0);
    jstr *s = v.t == JS_NULL || v.t == JS_UNDEF ? js_str(J, "") : js_to_str(J, v);
    if (!s) return js_undef();
    int k = jd_kind(x);
    if (k == JN_DOCUMENT) return js_undef();
    if (k == JN_TEXT || k == JN_COMMENT) jd_set_data(x, s->s, (int)s->len);
    else jd_replace_with_text(x, s);
    return js_undef();
}

static jval nat_parent_node(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0) return js_null();
    if (jd_is_top(x)) return jd_document_obj ? js_from_obj(jd_document_obj) : js_null();
    int p = jd_doc->nodes[x].parent;
    return p >= 0 ? jd_el_value(J, p) : js_null();
}

static jval nat_parent_element(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0) return js_null();
    int p = jd_doc->nodes[x].parent;
    return jd_is_element(p) && !jd_is_top(x) ? jd_el_value(J, p) : js_null();
}

static jval nat_child_nodes(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (jd_is_doc(t)) return jd_list_value(J, JL_DOCKIDS, 0, -1, 0);
    int x = jd_node_of(t);
    if (x < 0) return js_undef();
    jobj *w = jd_element(J, x);
    jval kept = jd_kept(w, jd_k_childnodes);
    if (kept.t == JS_OBJ) return kept;
    jval l = jd_list_value(J, JL_CHILDNODES, 0, x, 0);
    jd_keep(w, jd_k_childnodes, l);
    return l;
}

static jval nat_children(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (jd_is_doc(t)) return jd_list_value(J, JL_DOCKIDS, 1, -1, 0);
    int x = jd_node_of(t);
    if (x < 0) return js_undef();
    jobj *w = jd_element(J, x);
    jval kept = jd_kept(w, jd_k_children);
    if (kept.t == JS_OBJ) return kept;
    jval l = jd_list_value(J, JL_CHILDREN, 1, x, 0);
    jd_keep(w, jd_k_children, l);
    return l;
}

/* first, last, next, previous: a node's neighbours, and the same skipping
   everything that is not an element. Off the end is null, which is what a
   page tests for. */
static int jd_step(int x, int which, int elements) {
    const dnode *d = &jd_doc->nodes[x];
    int c = which == 0 ? d->first : which == 1 ? d->last : which == 2 ? d->next : d->prev;
    while (elements && c >= 0 && !jd_is_element(c))
        c = (which == 0 || which == 2) ? jd_doc->nodes[c].next : jd_doc->nodes[c].prev;
    return c;
}

static jval jd_neighbour(jctx *J, jval t, int which, int elements) {
    if (jd_is_doc(t)) return which < 2 ? jd_node_or_doc(J, jd_top()) : js_null();
    int x = jd_node_of(t);
    if (x < 0) return js_null();
    int c = jd_step(x, which, elements);
    return c >= 0 ? jd_el_value(J, c) : js_null();
}

static jval nat_first_child(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_neighbour(J, t, 0, 0); }
static jval nat_last_child(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_neighbour(J, t, 1, 0); }
static jval nat_next_sibling(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_neighbour(J, t, 2, 0); }
static jval nat_prev_sibling(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_neighbour(J, t, 3, 0); }
static jval nat_first_el(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_neighbour(J, t, 0, 1); }
static jval nat_last_el(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_neighbour(J, t, 1, 1); }
static jval nat_next_el(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_neighbour(J, t, 2, 1); }
static jval nat_prev_el(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_neighbour(J, t, 3, 1); }

static jval nat_child_el_count(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (jd_is_doc(t)) return js_num(jd_top() >= 0);
    int x = jd_node_of(t);
    if (x < 0) return js_undef();
    int k = 0;
    for (int c = jd_doc->nodes[x].first; c >= 0; c = jd_doc->nodes[c].next) if (jd_is_element(c)) k++;
    return js_num(k);
}

static jval nat_owner_document(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (jd_is_doc(t) || jd_inert_of(t) >= 0 || !jd_document_obj) return js_null();
    /* One inside a document of its own (jd_new_document) belongs to that. */
    int x = jd_node_of(t);
    for (int k = 0; x >= 0 && k < DOM_NODES; k++) {
        if (jd_kind(x) == JN_DOCUMENT) return jd_el_value(J, x);
        x = jd_doc->nodes[x].parent;
    }
    return js_from_obj(jd_document_obj);
}

static jval nat_is_connected(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (jd_is_doc(t)) return js_bool(1);
    int x = jd_node_of(t);
    return js_bool(x >= 0 && jd_connected_deep(x));
}

static jval nat_base_uri(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_str(jd_address);
}

/* The node a method is called on, as a parent: a node, or the document,
   which stands for its root when something is asked of what is under it. */
static int jd_parent_of_call(jval t) {
    if (jd_is_doc(t)) return -2;
    return jd_node_of(t);
}

static jval nat_append_child(jctx *J, jval t, jval *a, int n) {
    int p = jd_parent_of_call(t), c = jd_node_of(js_arg(a, n, 0));
    if (p == -2) return js_throw_dom(J, "HierarchyRequestError", "the document already has its element");
    if (!jd_can_insert(J, p, c)) return js_undef();
    jd_insert(p, c, -1);
    return js_arg(a, n, 0);
}

static jval nat_insert_before(jctx *J, jval t, jval *a, int n) {
    int p = jd_parent_of_call(t), c = jd_node_of(js_arg(a, n, 0));
    if (p == -2) return js_throw_dom(J, "HierarchyRequestError", "the document already has its element");
    if (!jd_can_insert(J, p, c)) return js_undef();
    jval refv = js_arg(a, n, 1);
    int ref = jd_node_of(refv);
    if (refv.t != JS_NULL && refv.t != JS_UNDEF && (ref < 0 || jd_doc->nodes[ref].parent != p))
        return js_throw_dom(J, "NotFoundError", "the node to put it before is not a child of this one");
    jd_insert(p, c, ref);
    return js_arg(a, n, 0);
}

static jval nat_remove_child(jctx *J, jval t, jval *a, int n) {
    int p = jd_node_of(t), c = jd_node_of(js_arg(a, n, 0));
    if (c < 0 || (p >= 0 && jd_doc->nodes[c].parent != p) || (p < 0 && !jd_is_doc(t)))
        return js_throw_dom(J, "NotFoundError", "that node is not a child of this one");
    if (jd_is_doc(t)) return js_throw_dom(J, "NotSupportedError", "the document's element stays");
    jd_remove(c);
    return js_arg(a, n, 0);
}

static jval nat_replace_child(jctx *J, jval t, jval *a, int n) {
    int p = jd_node_of(t), nw = jd_node_of(js_arg(a, n, 0)), old = jd_node_of(js_arg(a, n, 1));
    if (old < 0 || p < 0 || jd_doc->nodes[old].parent != p)
        return js_throw_dom(J, "NotFoundError", "the node to replace is not a child of this one");
    if (!jd_can_insert(J, p, nw)) return js_undef();
    if (nw == old) return js_arg(a, n, 1);
    int next = jd_doc->nodes[old].next;
    if (next == nw) next = jd_doc->nodes[nw].next;
    jd_remove(old);
    jd_insert(p, nw, next);
    return js_arg(a, n, 1);
}

static jval nat_clone_node(jctx *J, jval t, jval *a, int n) {
    if (jd_is_doc(t)) return js_throw_dom(J, "NotSupportedError", "the document is not copied here");
    int x = jd_node_of(t);
    if (x < 0) return jd_illegal(J);
    int deep = n > 0 && js_to_bool(a[0]);
    int c = jd_clone(x, deep);
    if (c >= 0 && deep && jd_is_template(x)) {
        /* A template's contents come with it, which is the one thing its
           copy is for. */
        int from = jd_template_content(x), to = jd_template_content(c);
        for (int ch = from >= 0 ? jd_doc->nodes[from].first : -1; ch >= 0 && to >= 0;
             ch = jd_doc->nodes[ch].next) {
            int cc = jd_clone(ch, 1);
            if (cc >= 0) dom_append(jd_doc, to, cc);
        }
    }
    /* A copy is made in the document its original is in. */
    if (c >= 0 && !jd_inert(x)) jd_custom_created(c);
    return c >= 0 ? jd_el_value(J, c) : js_null();
}

static jval nat_contains(jctx *J, jval t, jval *a, int n) {
    (void)J;
    int other = jd_node_of(js_arg(a, n, 0));
    if (jd_is_doc(t)) return js_bool(jd_is_doc(js_arg(a, n, 0)) || (other >= 0 && jd_connected(other)));
    int x = jd_node_of(t);
    if (x < 0 || other < 0) return js_bool(0);
    return js_bool(dom_contains(jd_doc, x, other));
}

static jval nat_has_child_nodes(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (jd_is_doc(t)) return js_bool(1);
    int x = jd_node_of(t);
    return js_bool(x >= 0 && jd_doc->nodes[x].first >= 0);
}

/* The top of the tree a node is in: the document, a fragment, or a shadow
   root -- and, asked for composed, past a shadow root to its element's. */
static jval nat_get_root_node(jctx *J, jval t, jval *a, int n) {
    if (jd_is_doc(t)) return t;
    int x = jd_node_of(t);
    if (x < 0) return jd_illegal(J);
    int composed = n > 0 && js_is_obj(a[0]) && js_to_bool(js_get(J, a[0], js_str(J, "composed")));
    if (J->sig != JS_OK) return js_undef();
    for (int hops = 0; hops < 64; hops++) {
        if (jd_connected(x) && jd_document_obj) return js_from_obj(jd_document_obj);
        while (jd_doc->nodes[x].parent >= 0) x = jd_doc->nodes[x].parent;
        jval root = jd_el_value(J, x);
        jval host = composed && js_is_obj(root) ? jd_kept(root.obj, jd_k_host) : js_undef();
        int h = host.t == JS_OBJ ? jd_node_of(host) : -1;
        if (h < 0) return root;
        x = h;
    }
    return js_null();
}

/* --- shadow roots a script makes ------------------------------------------------------
 *
 * attachShadow makes the shadow root a component fills: a fragment of its
 * own whose object is a ShadowRoot, kept on its element (shadowRoot, when it
 * is open) and knowing it (host). Scripts build and search it as they would
 * any fragment; there was no attachShadow, and Cloudflare's components
 * stopped on it. Each is kept in jd_shadow_host and jd_shadow_root: what is
 * in one is connected while its element is (jd_connected_deep), a composed
 * event goes on from it to its element (jd_dispatch_to), and the browser
 * lays out a copy of the page with each tree in place (dom.h, dom_flat). */
static int jd_shadow_host_ok(int x) {
    if (jd_doc->nodes[x].kind != DN_ELEMENT || jd_is_svg(x)) return 0;
    const char *nm = dom_tag_name(jd_doc, x);
    for (const char *p = nm; *p; p++) if (*p == '-') return 1;
    return jd_word_in("article aside blockquote body div footer h1 h2 h3 h4 h5 h6 header main nav p section span", nm);
}

static jval nat_attach_shadow(jctx *J, jval t, jval *a, int n) {
    int x = jd_node_of(t);
    if (x < 0 || !js_is_obj(t)) return jd_illegal(J);
    jval init = js_arg(a, n, 0);
    jval mode = js_is_obj(init) ? js_get(J, init, js_str(J, "mode")) : js_undef();
    if (J->sig != JS_OK) return js_undef();
    jstr *m = mode.t == JS_STR ? mode.str : 0;
    if (!m || !(js_str_is(m, "open") || js_str_is(m, "closed")))
        return js_throw(J, JS_ERR_TYPE, "attachShadow needs a mode, open or closed", J->error_line);
    if (!jd_shadow_host_ok(x)) return js_throw_dom(J, "NotSupportedError", "this element cannot have a shadow root");
    if (jd_kept(t.obj, jd_k_shadow).t == JS_OBJ)
        return js_throw_dom(J, "NotSupportedError", "this element has a shadow root already");
    int f = jd_new_fragment();
    jobj *r = f >= 0 ? jd_element(J, f) : 0;
    if (!r) return js_null();
    if (jd_p_shadowroot) r->proto = jd_p_shadowroot;
    if (jd_nshadow < JD_SHADOWS) {
        jd_shadow_host[jd_nshadow] = x;
        jd_shadow_root[jd_nshadow] = f;
        jd_nshadow++;
        jd_shadow_mark[x >> 3] |= (u8)(1 << (x & 7));
        jd_shadow_mark[f >> 3] |= (u8)(1 << (f & 7));
    }
    jd_keep(r, jd_k_host, t);
    jd_keep(r, jd_k_mode, js_from_str(m));
    jd_keep(t.obj, jd_k_shadow, js_from_obj(r));
    jd_touched();                    /* its element's own children are no longer drawn */
    return js_from_obj(r);
}

/* The i-th shadow root a script attached, and its element; 0 past the last. */
__attribute__((unused)) static int jsdom_shadow(int i, int *host, int *root) {
    if (!jd_open || !jd_doc || i < 0 || i >= jd_nshadow) return 0;
    *host = jd_shadow_host[i];
    *root = jd_shadow_root[i];
    return 1;
}

static jval nat_el_shadow_root(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    jval r = js_is_obj(t) ? jd_kept(t.obj, jd_k_shadow) : js_undef();
    if (r.t != JS_OBJ) return js_null();
    jval m = jd_kept(r.obj, jd_k_mode);
    return m.t == JS_STR && js_str_is(m.str, "open") ? r : js_null();
}

static jval nat_shadow_host(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jval h = js_is_obj(t) ? jd_kept(t.obj, jd_k_host) : js_undef();
    return h.t == JS_OBJ ? h : jd_illegal(J);
}

/* --- slots ------------------------------------------------------------------------------
 *
 * What a shadow tree's slot is given: its element's children whose slot
 * attribute is its name (the rest go to a slot with none), when it is the
 * first slot of that name in the tree -- as the browser draws them (dom.h,
 * dom_flat). FAST's buttons ask theirs whether they hold an icon, and every
 * one of MSN's stopped on assignedNodes. A slot is told by slotchange, as a
 * task, when a child of its element that it would be given comes or goes, and
 * when it arrives in a tree whose element has children for it. */
static u8 jd_slot_pending[DOM_NODES / 8];

static int jd_is_slot(int x) {
    return jd_doc->nodes[x].kind == DN_ELEMENT && jd_doc->nodes[x].tag == T_OTHER
           && w_same(dom_tag_name(jd_doc, x), "slot");
}

/* The shadow root a node is in, or -1. */
static int jd_slot_root(int x) {
    int top = x;
    for (int k = 0; top >= 0 && jd_doc->nodes[top].parent >= 0 && k < DOM_NODES; k++) top = jd_doc->nodes[top].parent;
    return top >= 0 && jd_shadow_find(top, 1) >= 0 ? top : -1;
}

static const char *jd_slot_name(int sl) {
    const char *v = dom_attr(jd_doc, sl, "name");
    return v ? v : "";
}

/* The name of the slot a child of a shadow tree's element goes to. */
static const char *jd_slottable_name(int c) {
    const char *v = jd_doc->nodes[c].kind == DN_ELEMENT ? dom_attr(jd_doc, c, "slot") : 0;
    return v ? v : "";
}

/* The first slot of a name in a tree, or -1. */
static int jd_slot_for(int root, const char *name) {
    for (int i = jd_doc->nodes[root].first; i >= 0; i = jd_walk_next(i, root))
        if (jd_is_slot(i) && w_same(jd_slot_name(i), name)) return i;
    return -1;
}

static int jd_slottable(int c) {
    int k = jd_doc->nodes[c].kind;
    return k == DN_ELEMENT || k == DN_TEXT;
}

static jval jd_assigned(jctx *J, jval t, jval *a, int n, int elements) {
    int sl = jd_el_of(t);
    if (sl < 0) return jd_illegal(J);
    int flatten = n > 0 && js_is_obj(a[0]) && js_to_bool(js_get(J, a[0], js_str(J, "flatten")));
    if (J->sig != JS_OK) return js_undef();
    jobj *arr = js_array(J);
    if (!arr) return js_null();
    int root = jd_slot_root(sl), got = 0;
    int s = root >= 0 ? jd_shadow_find(root, 1) : -1;
    if (s >= 0 && jd_slot_for(root, jd_slot_name(sl)) == sl)
        for (int c = jd_doc->nodes[jd_shadow_host[s]].first; c >= 0; c = jd_doc->nodes[c].next) {
            if (!jd_slottable(c) || !w_same(jd_slottable_name(c), jd_slot_name(sl))) continue;
            got = 1;
            if (!elements || jd_doc->nodes[c].kind == DN_ELEMENT) js_arr_push(J, arr, jd_el_value(J, c));
        }
    /* Flattened, a slot given nothing is what it holds itself. */
    if (flatten && !got)
        for (int c = jd_doc->nodes[sl].first; c >= 0; c = jd_doc->nodes[c].next)
            if (jd_slottable(c) && (!elements || jd_doc->nodes[c].kind == DN_ELEMENT))
                js_arr_push(J, arr, jd_el_value(J, c));
    return js_from_obj(arr);
}

static jval nat_slot_assigned_nodes(jctx *J, jval t, jval *a, int n) { return jd_assigned(J, t, a, n, 0); }
static jval nat_slot_assigned_elements(jctx *J, jval t, jval *a, int n) { return jd_assigned(J, t, a, n, 1); }

/* The slot an element or a text is given to, unless its tree is closed. */
static jval nat_assigned_slot(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_node_of(t);
    int p = x >= 0 ? jd_doc->nodes[x].parent : -1;
    int s = p >= 0 ? jd_shadow_find(p, 0) : -1;
    if (s < 0) return js_null();
    jobj *r = jd_element(J, jd_shadow_root[s]);
    jval m = r ? jd_kept(r, jd_k_mode) : js_undef();
    if (m.t != JS_STR || !js_str_is(m.str, "open")) return js_null();
    int sl = jd_slot_for(jd_shadow_root[s], jd_slottable_name(x));
    return sl >= 0 ? jd_el_value(J, sl) : js_null();
}

static void jd_slotchange_due(jval arg) {
    int sl = (int)arg.num;
    if (sl < 0 || sl >= DOM_NODES) return;
    jd_slot_pending[sl >> 3] &= (u8)~(1 << (sl & 7));
    if (sl < jd_doc->count) jd_fire_simple(sl, "slotchange", 1, 0);
}

static void jd_slot_signal(int sl) {
    if (sl < 0 || sl >= DOM_NODES || (jd_slot_pending[sl >> 3] & (1 << (sl & 7)))) return;
    jd_slot_pending[sl >> 3] |= (u8)(1 << (sl & 7));
    jd_later_native(jd_slotchange_due, js_num(sl), 0);
}

/* A child has come under parent or gone from it. */
static void jd_slots_touch(int parent, int child) {
    if (!jd_nshadow || parent < 0 || child < 0) return;
    int s = jd_shadow_find(parent, 0);
    if (s >= 0) {
        if (jd_slottable(child)) jd_slot_signal(jd_slot_for(jd_shadow_root[s], jd_slottable_name(child)));
        return;
    }
    /* Slots arriving in a tree whose element has children for them. */
    int root = jd_doc->nodes[child].parent >= 0 ? jd_slot_root(parent) : -1;
    int hs = root >= 0 ? jd_shadow_find(root, 1) : -1;
    if (hs < 0 || jd_doc->nodes[jd_shadow_host[hs]].first < 0) return;
    for (int i = child; i >= 0; i = jd_walk_next(i, child)) {
        if (!jd_is_slot(i) || jd_slot_for(root, jd_slot_name(i)) != i) continue;
        for (int c = jd_doc->nodes[jd_shadow_host[hs]].first; c >= 0; c = jd_doc->nodes[c].next)
            if (jd_slottable(c) && w_same(jd_slottable_name(c), jd_slot_name(i))) { jd_slot_signal(i); break; }
    }
}

static jval nat_shadow_mode(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jval m = js_is_obj(t) ? jd_kept(t.obj, jd_k_mode) : js_undef();
    return m.t == JS_STR ? m : jd_illegal(J);
}

/* Where one node is against another, in the standard's bits: 2 before,
   4 after, 8 contains, 16 contained by, 1 not in the same tree. */
static int jd_position_in(int a, int top) {
    int k = 0;
    for (int i = top; i >= 0; i = dom_next(jd_doc, i, top), k++) if (i == a) return k;
    return -1;
}

static jval nat_compare_position(jctx *J, jval t, jval *a, int n) {
    (void)J;
    int x = jd_node_of(t), y = jd_node_of(js_arg(a, n, 0));
    if (jd_is_doc(t)) x = jd_doc->root;
    if (jd_is_doc(js_arg(a, n, 0))) y = jd_doc->root;
    if (x < 0 || y < 0) return js_num(1);
    if (x == y) return js_num(0);
    int rx = x, ry = y;
    while (jd_doc->nodes[rx].parent >= 0) rx = jd_doc->nodes[rx].parent;
    while (jd_doc->nodes[ry].parent >= 0) ry = jd_doc->nodes[ry].parent;
    if (rx != ry) return js_num(1 | 32 | (x < y ? 4 : 2));
    if (dom_contains(jd_doc, y, x)) return js_num(8 | 2);
    if (dom_contains(jd_doc, x, y)) return js_num(16 | 4);
    return js_num(jd_position_in(y, rx) > jd_position_in(x, rx) ? 4 : 2);
}

static jval nat_is_same_node(jctx *J, jval t, jval *a, int n) {
    (void)J;
    jval o = js_arg(a, n, 0);
    return js_bool(o.t == JS_OBJ && t.t == JS_OBJ && o.obj == t.obj);
}

static int jd_equal_nodes(int x, int y) {
    if (jd_kind(x) != jd_kind(y)) return 0;
    if (jd_kind(x) == JN_TEXT || jd_kind(x) == JN_COMMENT) return w_same(jd_text_of(x), jd_text_of(y));
    if (!w_same(dom_tag_name(jd_doc, x), dom_tag_name(jd_doc, y))) return 0;
    const dnode *dx = &jd_doc->nodes[x], *dy = &jd_doc->nodes[y];
    if (dx->attr_n != dy->attr_n) return 0;
    for (int i = 0; i < dx->attr_n; i++) {
        const dattr *ax = &jd_doc->attrs[dx->attr_at + i];
        const char *v = dom_attr(jd_doc, y, jd_doc->arena + ax->name);
        if (!v || !w_same(v, jd_doc->arena + ax->value)) return 0;
    }
    int cx = dx->first, cy = dy->first;
    while (cx >= 0 && cy >= 0) {
        if (!jd_equal_nodes(cx, cy)) return 0;
        cx = jd_doc->nodes[cx].next;
        cy = jd_doc->nodes[cy].next;
    }
    return cx < 0 && cy < 0;
}

static jval nat_is_equal_node(jctx *J, jval t, jval *a, int n) {
    (void)J;
    int x = jd_node_of(t), y = jd_node_of(js_arg(a, n, 0));
    return js_bool(x >= 0 && y >= 0 && jd_equal_nodes(x, y));
}

/* Neighbouring text nodes run into one, and empty ones dropped. */
static jval nat_normalize(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0) return js_undef();
    for (int i = jd_walk_first(x); i >= 0; i = jd_walk_next(i, x)) {
        if (jd_doc->nodes[i].kind != DN_TEXT) continue;
        int nx;
        while ((nx = jd_doc->nodes[i].next) >= 0 && jd_doc->nodes[nx].kind == DN_TEXT) {
            jtext tx = { 0, 0, 0, 0 };
            jd_put(&tx, jd_text_of(i));
            jd_put(&tx, jd_text_of(nx));
            jd_putc(&tx, 0);
            jd_set_data(i, tx.b ? tx.b : "", tx.n ? (int)tx.n - 1 : 0);
            free(tx.b);
            jd_remove(nx);
        }
    }
    return js_undef();
}

/* --- ParentNode and ChildNode: the methods that take several nodes ---------------------- */

static jval nat_append(jctx *J, jval t, jval *a, int n) {
    int p = jd_node_of(t);
    if (jd_is_doc(t)) return js_throw_dom(J, "HierarchyRequestError", "the document already has its element");
    if (p < 0) return jd_illegal(J);
    if (!n) return js_undef();
    int c = jd_nodes_arg(J, a, n);
    if (c < 0 || !jd_can_insert(J, p, c)) return js_undef();
    jd_insert(p, c, -1);
    return js_undef();
}

static jval nat_prepend(jctx *J, jval t, jval *a, int n) {
    int p = jd_node_of(t);
    if (p < 0) return jd_illegal(J);
    if (!n) return js_undef();
    int c = jd_nodes_arg(J, a, n);
    if (c < 0 || !jd_can_insert(J, p, c)) return js_undef();
    jd_insert(p, c, jd_doc->nodes[p].first);
    return js_undef();
}

static jval nat_replace_children(jctx *J, jval t, jval *a, int n) {
    int p = jd_node_of(t);
    if (p < 0) return jd_illegal(J);
    int c = n ? jd_nodes_arg(J, a, n) : -1;
    if (n && (c < 0 || !jd_can_insert(J, p, c))) return js_undef();
    jd_remove_children(p);
    if (c >= 0) jd_insert(p, c, -1);
    return js_undef();
}

static jval nat_before(jctx *J, jval t, jval *a, int n) {
    int x = jd_node_of(t);
    if (x < 0) return jd_illegal(J);
    int p = jd_doc->nodes[x].parent;
    if (p < 0 || !n) return js_undef();
    int c = jd_nodes_arg(J, a, n);
    if (c < 0 || !jd_can_insert(J, p, c)) return js_undef();
    jd_insert(p, c, x);
    return js_undef();
}

static jval nat_after(jctx *J, jval t, jval *a, int n) {
    int x = jd_node_of(t);
    if (x < 0) return jd_illegal(J);
    int p = jd_doc->nodes[x].parent;
    if (p < 0 || !n) return js_undef();
    int c = jd_nodes_arg(J, a, n);
    if (c < 0 || !jd_can_insert(J, p, c)) return js_undef();
    jd_insert(p, c, jd_doc->nodes[x].next);
    return js_undef();
}

static jval nat_replace_with(jctx *J, jval t, jval *a, int n) {
    int x = jd_node_of(t);
    if (x < 0) return jd_illegal(J);
    int p = jd_doc->nodes[x].parent;
    if (p < 0) return js_undef();
    int c = n ? jd_nodes_arg(J, a, n) : -1;
    if (n && (c < 0 || !jd_can_insert(J, p, c))) return js_undef();
    int next = jd_doc->nodes[x].next;
    jd_remove(x);
    if (c >= 0) jd_insert(p, c, next);
    return js_undef();
}

static jval nat_remove(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0) return jd_illegal(J);
    jd_remove(x);
    return js_undef();
}

/* --- character data -------------------------------------------------------------------- */

static jval nat_cd_data(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_node_of(t);
    return x < 0 ? js_undef() : jd_str(jd_text_of(x));
}

static jval nat_cd_length(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_node_of(t);
    return x < 0 ? js_undef() : js_num(w_len(jd_text_of(x)));
}

static jval nat_cd_append(jctx *J, jval t, jval *a, int n) {
    int x = jd_node_of(t);
    if (x < 0) return jd_illegal(J);
    jstr *s = jd_arg_str(J, a, n, 0);
    jtext tx = { 0, 0, 0, 0 };
    jd_put(&tx, jd_text_of(x));
    jt_put(J, &tx, s->s, s->len);
    jd_putc(&tx, 0);
    jd_set_data(x, tx.b, (int)tx.n - 1);
    free(tx.b);
    return js_undef();
}

static jval nat_text_whole(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0) return js_undef();
    while (jd_doc->nodes[x].prev >= 0 && jd_doc->nodes[jd_doc->nodes[x].prev].kind == DN_TEXT)
        x = jd_doc->nodes[x].prev;
    jtext tx = { 0, 0, 0, 0 };
    for (; x >= 0 && jd_doc->nodes[x].kind == DN_TEXT; x = jd_doc->nodes[x].next)
        jd_put(&tx, jd_text_of(x));
    return js_from_str(jt_done(J, &tx));
}

static jval nat_text_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "Text is made with new", J->error_line);
    jstr *s = jd_arg_str(J, a, n, 0);
    int x = jd_new_text(s->s, (int)s->len);
    return x < 0 ? js_undef() : jd_el_value(J, x);
}

static jval nat_comment_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "Comment is made with new", J->error_line);
    jstr *s = jd_arg_str(J, a, n, 0);
    int x = jd_new_comment(s->s, (int)s->len);
    return x < 0 ? js_undef() : jd_el_value(J, x);
}

static jval nat_fragment_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "DocumentFragment is made with new", J->error_line);
    int x = jd_new_fragment();
    return x < 0 ? js_undef() : jd_el_value(J, x);
}

/* The interfaces a page reaches for by name but never makes itself. */
static jval nat_illegal_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    return js_throw(J, JS_ERR_TYPE, "Illegal constructor", J->error_line);
}

/* --- elements --------------------------------------------------------------------------- */

static jval nat_tag_name(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    return x < 0 ? js_undef() : jd_name_of(x);
}

static jval nat_local_name(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    return x < 0 ? js_undef() : jd_str(dom_tag_name(jd_doc, x));
}

static jval nat_namespace(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    return jd_str(jd_is_svg(x) ? "http://www.w3.org/2000/svg" : "http://www.w3.org/1999/xhtml");
}

static jval nat_null_getter(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_null();
}

static jval nat_get_attr(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    char nm[128];
    jd_attr_name(x, jd_arg_str(J, a, n, 0), nm, (int)sizeof(nm));
    const char *v = jd_attr(x, nm);
    return v ? jd_str(v) : js_null();
}

static jval nat_set_attr(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    jstr *name = jd_arg_str(J, a, n, 0);
    if (!name->len) return js_throw_dom(J, "InvalidCharacterError", "an attribute needs a name");
    jstr *val = js_to_str(J, js_arg(a, n, 1));
    if (!val) return js_undef();
    char nm[128];
    jd_attr_name(x, name, nm, (int)sizeof(nm));
    jd_attr_set(x, nm, val->s);
    return js_undef();
}

static jval nat_remove_attr(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    char nm[128];
    jd_attr_name(x, jd_arg_str(J, a, n, 0), nm, (int)sizeof(nm));
    jd_attr_remove(x, nm);
    return js_undef();
}

static jval nat_has_attr(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    char nm[128];
    jd_attr_name(x, jd_arg_str(J, a, n, 0), nm, (int)sizeof(nm));
    return js_bool(jd_attr(x, nm) != 0);
}

static jval nat_has_attrs(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    return js_bool(jd_doc->nodes[x].attr_n > 0);
}

static jval nat_toggle_attr(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    char nm[128];
    jd_attr_name(x, jd_arg_str(J, a, n, 0), nm, (int)sizeof(nm));
    int has = jd_attr(x, nm) != 0;
    int want = n > 1 && a[1].t != JS_UNDEF ? js_to_bool(a[1]) : !has;
    if (want && !has) jd_attr_set(x, nm, "");
    if (!want && has) jd_attr_remove(x, nm);
    return js_bool(want);
}

static jval nat_attr_names(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    jobj *arr = js_array(J);
    const dnode *d = &jd_doc->nodes[x];
    for (int i = 0; arr && i < d->attr_n; i++)
        js_arr_push(J, arr, jd_str(jd_doc->arena + jd_doc->attrs[d->attr_at + i].name));
    return js_from_obj(arr);
}

/* The namespaced ones, for the few pages that use them: SVG's xlink:href
   mostly. The name is kept as written; a local name finds a prefixed one. */
static const char *jd_attr_ns(int x, const jstr *local) {
    const char *v = jd_attr(x, local->s);
    if (v) return v;
    const dnode *d = &jd_doc->nodes[x];
    for (int i = 0; i < d->attr_n; i++) {
        const char *nm = jd_doc->arena + jd_doc->attrs[d->attr_at + i].name;
        const char *colon = 0;
        for (const char *p = nm; *p; p++) if (*p == ':') colon = p;
        if (colon && w_same_fold(colon + 1, local->s)) return jd_doc->arena + jd_doc->attrs[d->attr_at + i].value;
    }
    return 0;
}

static jval nat_get_attr_ns(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    const char *v = jd_attr_ns(x, jd_arg_str(J, a, n, 1));
    return v ? jd_str(v) : js_null();
}

static jval nat_set_attr_ns(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    jstr *nm = jd_arg_str(J, a, n, 1);
    jstr *v = js_to_str(J, js_arg(a, n, 2));
    if (!v || !nm->len) return js_undef();
    char low[128];
    jd_attr_name(x, nm, low, (int)sizeof(low));
    jd_attr_set(x, low, v->s);
    return js_undef();
}

static jval nat_remove_attr_ns(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    jd_attr_remove(x, jd_arg_str(J, a, n, 1)->s);
    return js_undef();
}

static jval nat_has_attr_ns(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    return js_bool(jd_attr_ns(x, jd_arg_str(J, a, n, 1)) != 0);
}

static jval nat_el_id(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    const char *v = jd_attr(x, "id");
    return jd_str(v ? v : "");
}

static jval nat_el_set_id(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jd_attr_set(x, "id", jd_arg_str(J, a, n, 0)->s);
    return js_undef();
}

static jval nat_class_name(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    const char *v = jd_attr(x, "class");
    return jd_str(v ? v : "");
}

static jval nat_set_class_name(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jd_attr_set(x, "class", jd_arg_str(J, a, n, 0)->s);
    return js_undef();
}

static jval nat_class_list(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jobj *o = jd_classlist(J, x);
    return o ? js_from_obj(o) : js_null();
}

static jval nat_set_class_list(jctx *J, jval t, jval *a, int n) {
    return nat_set_class_name(J, t, a, n);
}

static jval nat_attributes(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jobj *o = jd_attrs_obj(J, x);
    return o ? js_from_obj(o) : js_null();
}

static jval nat_inner_html(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0) return js_undef();
    if (jd_is_template(x)) {
        int c = jd_template_content(x);
        if (c >= 0) x = c;
    }
    jtext tx = { 0, 0, 0, 0 };
    jd_serialise_children(x, &tx);
    return js_from_str(jt_done(J, &tx));
}

static jval nat_set_inner_html(jctx *J, jval t, jval *a, int n) {
    int x = jd_node_of(t);
    if (x < 0) return js_undef();
    jval v = js_arg(a, n, 0);
    jstr *s = v.t == JS_NULL ? js_str(J, "") : js_to_str(J, v);
    if (s) jd_set_inner(x, s->s, (int)s->len);
    return js_undef();
}

static jval nat_outer_html(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jtext tx = { 0, 0, 0, 0 };
    jd_serialise(x, &tx, 1);
    return js_from_str(jt_done(J, &tx));
}

static jval nat_set_outer_html(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    int p = jd_doc->nodes[x].parent;
    if (p < 0) return js_throw_dom(J, "NoModificationAllowedError", "an element with no parent cannot be replaced");
    jstr *s = jd_arg_str(J, a, n, 0);
    int f = jd_parse_fragment(s->s, (int)s->len);
    int next = jd_doc->nodes[x].next;
    jd_remove(x);
    if (f >= 0) jd_insert(p, f, next);
    jd_custom_created(p);
    return js_undef();
}

/* beforebegin, afterbegin, beforeend, afterend: where, as a parent and the
   node to go before. */
static int jd_adjacent(jctx *J, int x, const jstr *pos, int *parent, int *before) {
    char w[16];
    int k = 0;
    for (; k < (int)pos->len && k < 15; k++) w[k] = w_lower(pos->s[k]);
    w[k] = 0;
    if (w_same(w, "beforebegin")) { *parent = jd_doc->nodes[x].parent; *before = x; }
    else if (w_same(w, "afterbegin")) { *parent = x; *before = jd_doc->nodes[x].first; }
    else if (w_same(w, "beforeend")) { *parent = x; *before = -1; }
    else if (w_same(w, "afterend")) { *parent = jd_doc->nodes[x].parent; *before = jd_doc->nodes[x].next; }
    else {
        js_throw_dom(J, "SyntaxError", "that is not a place next to an element");
        return 0;
    }
    return *parent >= 0;
}

static jval nat_insert_adj_html(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    int p, before;
    if (!jd_adjacent(J, x, jd_arg_str(J, a, n, 0), &p, &before)) return js_undef();
    jstr *s = jd_arg_str(J, a, n, 1);
    int f = jd_parse_fragment(s->s, (int)s->len);
    if (f >= 0) jd_insert(p, f, before);
    jd_custom_created(p);
    return js_undef();
}

static jval nat_insert_adj_el(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    int p, before, c = jd_node_of(js_arg(a, n, 1));
    if (!jd_adjacent(J, x, jd_arg_str(J, a, n, 0), &p, &before)) return js_null();
    if (!jd_can_insert(J, p, c)) return js_undef();
    jd_insert(p, c, before);
    return js_arg(a, n, 1);
}

static jval nat_insert_adj_text(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    int p, before;
    if (!jd_adjacent(J, x, jd_arg_str(J, a, n, 0), &p, &before)) return js_undef();
    jstr *s = jd_arg_str(J, a, n, 1);
    int c = jd_new_text(s->s, (int)s->len);
    if (c >= 0) jd_insert(p, c, before);
    return js_undef();
}

/* querySelector and querySelectorAll, on the document (the whole page) and
   on an element or a fragment (what is under it, not itself: a page asking
   a row for its links does not mean the row). */
static int jd_query_root(jval t) {
    if (jd_is_doc(t)) return -1;
    return jd_node_of(t);
}

static jval nat_query(jctx *J, jval t, jval *a, int n) {
    int root = jd_query_root(t);
    if (root < 0 && !jd_is_doc(t)) return jd_illegal(J);
    jstr *sel = jd_arg_str(J, a, n, 0);
    jd_selq q;
    jd_sel_open(&q, sel);
    int found = -1;
    for (int i = jd_walk_first(root); i >= 0 && found < 0; i = jd_walk_next(i, root))
        if (jd_sel_test(&q, i)) found = i;
    jd_sel_close(&q);
    return found >= 0 ? jd_el_value(J, found) : js_null();
}

static jval nat_query_all(jctx *J, jval t, jval *a, int n) {
    int root = jd_query_root(t);
    if (root < 0 && !jd_is_doc(t)) return jd_illegal(J);
    jstr *sel = jd_arg_str(J, a, n, 0);
    int at;
    jval out = jd_static_list(J, &at);
    if (out.t != JS_OBJ) return out;
    jd_selq q;
    jd_sel_open(&q, sel);
    for (int i = jd_walk_first(root); i >= 0; i = jd_walk_next(i, root))
        if (jd_sel_test(&q, i)) jd_list_add(&jd_lists[at], i);
    jd_sel_close(&q);
    return out;
}

static jval nat_matches(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    return js_bool(jd_sel_matches(x, jd_arg_str(J, a, n, 0)));
}

static jval nat_closest(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    jd_selq q;
    jd_sel_open(&q, jd_arg_str(J, a, n, 0));
    int found = -1;
    for (int at = x; at >= 0 && found < 0; at = jd_doc->nodes[at].parent)
        if (jd_is_element(at) && jd_sel_test(&q, at)) found = at;
    jd_sel_close(&q);
    return found >= 0 ? jd_el_value(J, found) : js_null();
}

static jval nat_by_tag(jctx *J, jval t, jval *a, int n) {
    int root = jd_query_root(t);
    if (root < 0 && !jd_is_doc(t)) return jd_illegal(J);
    return jd_list_value(J, JL_TAG, 1, root, jd_arg_str(J, a, n, 0));
}

static jval nat_by_tag_ns(jctx *J, jval t, jval *a, int n) {
    int root = jd_query_root(t);
    if (root < 0 && !jd_is_doc(t)) return jd_illegal(J);
    return jd_list_value(J, JL_TAG, 1, root, jd_arg_str(J, a, n, 1));
}

static jval nat_by_class(jctx *J, jval t, jval *a, int n) {
    int root = jd_query_root(t);
    if (root < 0 && !jd_is_doc(t)) return jd_illegal(J);
    return jd_list_value(J, JL_CLASS, 1, root, jd_arg_str(J, a, n, 0));
}

/* Where the layout put an element, relative to the window, as a DOMRect: the
   boxes it drew for the element and everything in it (browser.c,
   box_of). Before the first layout, and for something not drawn, it is at
   nought and has no size, which is the answer for an element with no box. */
static jval jd_rect(jctx *J, int x, int y, int w, int h) {
    jobj *r = js_object_with(J, JO_PLAIN, jd_p[JI_DOMRECT]);
    if (!r) return js_null();
    js_set(J, r, "x", js_num(x));
    js_set(J, r, "y", js_num(y));
    js_set(J, r, "width", js_num(w));
    js_set(J, r, "height", js_num(h));
    js_set(J, r, "top", js_num(h < 0 ? y + h : y));
    js_set(J, r, "left", js_num(w < 0 ? x + w : x));
    js_set(J, r, "right", js_num(w < 0 ? x : x + w));
    js_set(J, r, "bottom", js_num(h < 0 ? y : y + h));
    return js_from_obj(r);
}

static jval nat_rect_json(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *o = js_object(J, JO_PLAIN);
    static const char *const K[] = { "x", "y", "width", "height", "top", "right", "bottom", "left", 0 };
    for (int i = 0; o && K[i]; i++) js_set(J, o, K[i], js_get(J, t, js_str(J, K[i])));
    return js_from_obj(o);
}

static jval nat_domrect_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "DOMRect is made with new", J->error_line);
    double v[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4 && i < n; i++) v[i] = js_to_num(J, a[i]);
    jval r = jd_rect(J, 0, 0, 0, 0);
    if (r.t != JS_OBJ) return r;
    js_set(J, r.obj, "x", js_num(v[0]));
    js_set(J, r.obj, "y", js_num(v[1]));
    js_set(J, r.obj, "width", js_num(v[2]));
    js_set(J, r.obj, "height", js_num(v[3]));
    js_set(J, r.obj, "top", js_num(v[1]));
    js_set(J, r.obj, "left", js_num(v[0]));
    js_set(J, r.obj, "right", js_num(v[0] + v[2]));
    js_set(J, r.obj, "bottom", js_num(v[1] + v[3]));
    return r;
}

static jval nat_bounding_rect(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0) return jd_illegal(J);
    int bx, by, bw, bh;
    jd_box(x, &bx, &by, &bw, &bh);
    return jd_rect(J, bx, by, bw, bh);
}

static jval nat_client_rects(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0) return jd_illegal(J);
    jobj *arr = js_array(J);
    int bx, by, bw, bh;
    if (arr && jd_box(x, &bx, &by, &bw, &bh)) js_arr_push(J, arr, jd_rect(J, bx, by, bw, bh));
    return js_from_obj(arr);
}

/* The sizes a page reads to measure things: a box's, from the layout, and
   for the document's element the window's, which is what
   documentElement.clientWidth is asked for. */
static int jd_is_root_box(int x) { return jd_is_top(x); }

static jval jd_measure(jval t, int what) {
    int x = jd_node_of(t);
    if (x < 0) return js_undef();
    int bx, by, bw, bh;
    if (jd_is_root_box(x) && (what == 2 || what == 3)) return js_num(what == 2 ? jd_view_w : jd_view_h);
    jd_box(x, &bx, &by, &bw, &bh);
    switch (what) {
        case 0: return js_num(bx);
        case 1: return js_num(by + jd_scroll_y);
        case 2: return js_num(bw);
        default: return js_num(bh);
    }
}

static jval nat_offset_left(jctx *J, jval t, jval *a, int n) { (void)J; (void)a; (void)n; return jd_measure(t, 0); }
static jval nat_offset_top(jctx *J, jval t, jval *a, int n) { (void)J; (void)a; (void)n; return jd_measure(t, 1); }
static jval nat_offset_width(jctx *J, jval t, jval *a, int n) { (void)J; (void)a; (void)n; return jd_measure(t, 2); }
static jval nat_offset_height(jctx *J, jval t, jval *a, int n) { (void)J; (void)a; (void)n; return jd_measure(t, 3); }

static jval nat_scroll_height(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_node_of(t);
    if (x < 0) return js_undef();
    int bx, by, bw, bh;
    jd_box(x, &bx, &by, &bw, &bh);
    if ((jd_is_root_box(x) || x == jd_doc->body) && bh < jd_view_h) bh = jd_view_h;
    return js_num(bh);
}

static jval nat_offset_parent(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0 || !jd_connected_deep(x) || x == jd_doc->body || jd_is_top(x)) return js_null();
    return jd_el_value(J, jd_doc->body);
}

/* Scrolled boxes are not something this browser has: the page scrolls as a
   whole, so an element's own scroll is always at nought, except the
   document's element and the body, which are the page's (jd_scroll_y). */
static jval nat_scroll_top(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    return js_num(jd_is_top(x) || x == jd_doc->body ? jd_scroll_y : 0);
}

static jval nat_zero(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_num(0);
}

static jval nat_nothing_js(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_undef();
}

/* Asking the page to show an element: the browser scrolls to where the
   layout put it (jsdom_scroll_with). */
static void (*jd_scroll_to)(int y);

static jval nat_scroll_into_view(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_node_of(t);
    int bx, by, bw, bh;
    if (x >= 0 && jd_scroll_to && jd_box(x, &bx, &by, &bw, &bh)) jd_scroll_to(by + jd_scroll_y);
    return js_undef();
}

/* --- HTMLElement --------------------------------------------------------------------------- */

static jval nat_hidden(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    return x < 0 ? js_undef() : js_bool(jd_attr(x, "hidden") != 0);
}

static jval nat_set_hidden(jctx *J, jval t, jval *a, int n) {
    (void)J;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    if (js_to_bool(js_arg(a, n, 0))) jd_attr_set(x, "hidden", "");
    else jd_attr_remove(x, "hidden");
    return js_undef();
}

static jval nat_style(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jobj *o = jd_style_obj(J, x);
    return o ? js_from_obj(o) : js_null();
}

static jval nat_set_style(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jd_attr_set(x, "style", jd_style_arg(J, js_arg(a, n, 0))->s);
    return js_undef();
}

static jval nat_dataset(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jobj *o = jd_dataset_obj(J, x);
    return o ? js_from_obj(o) : js_null();
}

/* innerText: the words as they read, which leaves out what is never drawn --
   script, style, a template, anything marked hidden -- and turns a line
   break into one. Not the layout's own reading of what is visible: a page's
   display: none is not consulted, which is the one way this differs. */
static void jd_inner_text(int el, jtext *t) {
    for (int i = el; i >= 0; ) {
        const dnode *x = &jd_doc->nodes[i];
        int skip = 0;
        if (x->kind == DN_ELEMENT && i != el) {
            int tg = x->tag;
            if (tg == T_SCRIPT || tg == T_STYLE || tg == T_NOSCRIPT || jd_is_template(i)
                || dom_attr(jd_doc, i, "hidden") || jd_kind(i) == JN_COMMENT) skip = 1;
            if (tg == T_BR) jd_putc(t, '\n');
        }
        if (x->kind == DN_TEXT && x->text >= 0) jd_put(t, jd_doc->arena + x->text);
        if (!skip && x->first >= 0) { i = x->first; continue; }
        while (i >= 0 && i != el && jd_doc->nodes[i].next < 0) i = jd_doc->nodes[i].parent;
        if (i < 0 || i == el) break;
        i = jd_doc->nodes[i].next;
    }
}

static jval nat_inner_text(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jtext tx = { 0, 0, 0, 0 };
    jd_inner_text(x, &tx);
    return js_from_str(jt_done(J, &tx));
}

static int jd_fire_simple(int node, const char *type, int bubbles, int cancelable);
static int jd_fire_click(int node, int trusted, int x, int y);
static int jd_active = -1;          /* the element with the keyboard, or -1 */

/* click(): a click the page asks for, which runs what a click would --
   handlers, then a link followed or a box ticked (jd_fire_click). */
static jval nat_click(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    if (jd_attr(x, "disabled")) return js_undef();
    jd_fire_click(x, 0, 0, 0);
    return js_undef();
}

static jval nat_focus(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    if (jd_active == x || !jd_connected_deep(x)) return js_undef();
    int old = jd_active;
    jd_active = x;
    if (old >= 0) { jd_fire_simple(old, "blur", 0, 0); jd_fire_simple(old, "focusout", 1, 0); }
    jd_fire_simple(x, "focus", 0, 0);
    jd_fire_simple(x, "focusin", 1, 0);
    return js_undef();
}

static jval nat_blur(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    if (jd_active != x) return js_undef();
    jd_active = -1;
    jd_fire_simple(x, "blur", 0, 0);
    jd_fire_simple(x, "focusout", 1, 0);
    return js_undef();
}

/* --- reflected attributes -----------------------------------------------------------------
 *
 * Most of what an HTML element has is an attribute by another name:
 * img.alt is the alt attribute, input.disabled is whether there is one. One
 * pair of natives does all of them, told which attribute and how to read it
 * by what is kept on the native itself (js.h: a native's data), which it
 * reads before anything it calls can change J->callee. */
enum { JR_STR = 1, JR_BOOL, JR_INT, JR_URL, JR_ENUM, JR_NULLSTR };

static jval nat_refl_get(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *me = J->callee;
    int x = jd_el_of(t);
    if (x < 0 || !me || me->data.t != JS_STR) return js_undef();
    const char *attr = me->data.str->s;
    int kind = me->spare;
    const char *v = jd_attr(x, attr);
    switch (kind) {
        case JR_BOOL: return js_bool(v != 0);
        case JR_INT: {
            if (!v) return me->data2;
            int neg = 0, got = 0;
            double d = 0;
            while (*v == ' ') v++;
            if (*v == '-') { neg = 1; v++; } else if (*v == '+') v++;
            while (*v >= '0' && *v <= '9') { d = d * 10 + (*v++ - '0'); got = 1; }
            return got ? js_num(neg ? -d : d) : me->data2;
        }
        case JR_URL: {
            if (!v) return jd_str("");
            jstr *s = jd_resolve_str(J, v);
            return s ? js_from_str(s) : jd_str(v);
        }
        case JR_ENUM: {
            if (!v || !*v) return me->data2;
            char low[64];
            int k = 0;
            for (; v[k] && k < 63; k++) low[k] = w_lower(v[k]);
            low[k] = 0;
            return jd_str(low);
        }
        case JR_NULLSTR: return v ? jd_str(v) : js_null();
        default: return jd_str(v ? v : "");
    }
}

static jval nat_refl_set(jctx *J, jval t, jval *a, int n) {
    jobj *me = J->callee;
    int x = jd_el_of(t);
    if (x < 0 || !me || me->data.t != JS_STR) return js_undef();
    jstr *attr = me->data.str;
    int kind = me->spare;
    jval v = js_arg(a, n, 0);
    if (kind == JR_BOOL) {
        if (js_to_bool(v)) jd_attr_set(x, attr->s, "");
        else jd_attr_remove(x, attr->s);
        return js_undef();
    }
    if (kind == JR_NULLSTR && v.t == JS_NULL) { jd_attr_remove(x, attr->s); return js_undef(); }
    if (kind == JR_INT) {
        double d = js_to_num(J, v);
        char buf[24];
        int w = 0;
        long long q = d == d ? (long long)d : 0;
        if (q < 0) { buf[w++] = '-'; q = -q; }
        char tmp[20];
        int k = 0;
        do { tmp[k++] = (char)('0' + q % 10); q /= 10; } while (q && k < 19);
        while (k) buf[w++] = tmp[--k];
        buf[w] = 0;
        jd_attr_set(x, attr->s, buf);
        return js_undef();
    }
    jstr *s = js_to_str(J, v);
    if (s) jd_attr_set(x, attr->s, s->s);
    return js_undef();
}

static void jd_accessor(jctx *J, jobj *on, const char *name, jnative get, jnative set) {
    if (!on) return;
    jobj *g = get ? js_native(J, name, get) : 0;
    jobj *s = set ? js_native(J, name, set) : 0;
    if (g) g->flags |= JOF_NOCTOR;
    if (s) s->flags |= JOF_NOCTOR;
    js_define_accessor(J, on, js_str(J, name), js_from_obj(g), js_from_obj(s), JP_ENUM | JP_CONF);
}

static void jd_method(jctx *J, jobj *on, const char *name, jnative fn, int arity) {
    if (!on) return;
    jobj *f = js_native_n(J, name, fn, arity);
    if (!f) return;
    f->flags |= JOF_NOCTOR;
    js_put_prop_flags(J, on, js_str(J, name), js_from_obj(f), JP_ENUM | JP_WRITE | JP_CONF);
}

static void jd_reflect_as(jctx *J, jobj *on, const char *prop, const char *attr, int kind, jval deflt) {
    if (!on) return;
    jobj *g = js_native(J, prop, nat_refl_get);
    jobj *s = js_native(J, prop, nat_refl_set);
    if (!g || !s) return;
    g->flags |= JOF_NOCTOR;
    s->flags |= JOF_NOCTOR;
    g->data = s->data = jd_str(attr);
    g->data2 = s->data2 = deflt;
    g->spare = s->spare = (u16)kind;
    js_define_accessor(J, on, js_str(J, prop), js_from_obj(g), js_from_obj(s), JP_ENUM | JP_CONF);
}

static void jd_reflect(jctx *J, jobj *on, const char *prop, const char *attr, int kind) {
    jd_reflect_as(J, on, prop, attr, kind, kind == JR_INT ? js_num(0) : jd_str(""));
}

/* A list of string attributes whose property is their own name. */
static void jd_reflect_strs(jctx *J, jobj *on, const char *names) {
    char one[40];
    for (const char *p = names; *p; ) {
        while (*p == ' ') p++;
        int k = 0;
        while (*p && *p != ' ' && k < 39) one[k++] = *p++;
        one[k] = 0;
        if (!k) continue;
        char attr[40];
        for (int i = 0; i <= k; i++) attr[i] = w_lower(one[i]);
        jd_reflect(J, on, one, attr, JR_STR);
    }
}

static void jd_reflect_bools(jctx *J, jobj *on, const char *names) {
    char one[40];
    for (const char *p = names; *p; ) {
        while (*p == ' ') p++;
        int k = 0;
        while (*p && *p != ' ' && k < 39) one[k++] = *p++;
        one[k] = 0;
        if (!k) continue;
        char attr[40];
        for (int i = 0; i <= k; i++) attr[i] = w_lower(one[i]);
        jd_reflect(J, on, one, attr, JR_BOOL);
    }
}

/* --- form controls ---------------------------------------------------------------------------
 *
 * What is in a control is kept in the document, as the attribute the browser
 * types into and the layout draws (browser.c, field_set_value), so a script
 * reading it gets what is on the screen and a script writing it puts
 * something there. A box written with a bare `checked` is ticked, and one
 * unticked leaves a nought behind (browser.c, field_set_checked). */
static jval jd_option_value(int opt) {
    const char *v = jd_attr(opt, "value");
    if (v) return jd_str(v);
    return jd_text_value(opt);
}

static int jd_selected_option(int sel) {
    int first = -1;
    for (int i = jd_walk_first(sel); i >= 0; i = jd_walk_next(i, sel)) {
        if (jd_doc->nodes[i].kind != DN_ELEMENT || jd_doc->nodes[i].tag != T_OPTION) continue;
        if (first < 0) first = i;
        if (jd_attr(i, "selected")) return i;
    }
    return first;
}

static jval nat_value(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    int tag = jd_doc->nodes[x].tag;
    if (tag == T_SELECT) {
        int o = jd_selected_option(x);
        return o >= 0 ? jd_option_value(o) : jd_str("");
    }
    if (tag == T_OPTION) return jd_option_value(x);
    const char *v = jd_attr(x, "value");
    if (v) return jd_str(v);
    if (tag == T_TEXTAREA) return jd_text_value(x);
    if (tag == T_INPUT) {
        const char *ty = jd_attr(x, "type");
        if (ty && (w_same_fold(ty, "checkbox") || w_same_fold(ty, "radio"))) return jd_str("on");
    }
    return jd_str("");
}

static jval nat_set_value(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jval v = js_arg(a, n, 0);
    jstr *s = v.t == JS_NULL ? js_str(J, "") : js_to_str(J, v);
    if (!s) return js_undef();
    if (jd_doc->nodes[x].tag == T_SELECT) {
        for (int i = jd_walk_first(x); i >= 0; i = jd_walk_next(i, x)) {
            if (jd_doc->nodes[i].kind != DN_ELEMENT || jd_doc->nodes[i].tag != T_OPTION) continue;
            jval ov = jd_option_value(i);
            if (ov.t == JS_STR && js_str_eq(ov.str, s)) jd_attr_set(i, "selected", "");
            else jd_attr_remove(i, "selected");
        }
        return js_undef();
    }
    jd_attr_set(x, "value", s->s);
    return js_undef();
}

static jval nat_checked(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    const char *v = jd_attr(x, "checked");
    return js_bool(v && !w_same(v, "0"));
}

static jval nat_set_checked(jctx *J, jval t, jval *a, int n) {
    (void)J;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jd_attr_set(x, "checked", js_to_bool(js_arg(a, n, 0)) ? "1" : "0");
    return js_undef();
}

static jval nat_selected_index(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    int k = 0, want = jd_selected_option(x);
    for (int i = jd_walk_first(x); i >= 0; i = jd_walk_next(i, x)) {
        if (jd_doc->nodes[i].kind != DN_ELEMENT || jd_doc->nodes[i].tag != T_OPTION) continue;
        if (i == want) return js_num(k);
        k++;
    }
    return js_num(-1);
}

static jval nat_set_selected_index(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    int want = (int)js_to_num(J, js_arg(a, n, 0)), k = 0;
    for (int i = jd_walk_first(x); i >= 0; i = jd_walk_next(i, x)) {
        if (jd_doc->nodes[i].kind != DN_ELEMENT || jd_doc->nodes[i].tag != T_OPTION) continue;
        if (k++ == want) jd_attr_set(i, "selected", "");
        else jd_attr_remove(i, "selected");
    }
    return js_undef();
}

static jval nat_options(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    return jd_list_value(J, JL_OPTIONS, 1, x, 0);
}

static jval nat_selected_options(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    return jd_list_value(J, JL_SELECTED, 1, x, 0);
}

static jval nat_select_length(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    int k = 0;
    for (int i = jd_walk_first(x); i >= 0; i = jd_walk_next(i, x))
        if (jd_doc->nodes[i].kind == DN_ELEMENT && jd_doc->nodes[i].tag == T_OPTION) k++;
    return js_num(k);
}

static jval nat_select_add(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t), o = jd_node_of(js_arg(a, n, 0));
    if (x < 0) return jd_illegal(J);
    if (!jd_can_insert(J, x, o)) return js_undef();
    int before = jd_node_of(js_arg(a, n, 1));
    jd_insert(x, o, before >= 0 && jd_doc->nodes[before].parent == x ? before : -1);
    return js_undef();
}

static jval nat_select_remove(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    if (!n) { jd_remove(x); return js_undef(); }
    int want = (int)js_to_num(J, a[0]), k = 0;
    for (int i = jd_walk_first(x); i >= 0; i = jd_walk_next(i, x)) {
        if (jd_doc->nodes[i].kind != DN_ELEMENT || jd_doc->nodes[i].tag != T_OPTION) continue;
        if (k++ == want) { jd_remove(i); break; }
    }
    return js_undef();
}

static jval nat_option_selected(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    int p = x;
    while (p >= 0 && jd_doc->nodes[p].tag != T_SELECT) p = jd_doc->nodes[p].parent;
    return js_bool(p >= 0 ? jd_selected_option(p) == x : jd_attr(x, "selected") != 0);
}

static jval nat_option_index(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    int p = x;
    while (p >= 0 && jd_doc->nodes[p].tag != T_SELECT) p = jd_doc->nodes[p].parent;
    if (p < 0) return js_num(0);
    int k = 0;
    for (int i = jd_walk_first(p); i >= 0; i = jd_walk_next(i, p)) {
        if (jd_doc->nodes[i].kind != DN_ELEMENT || jd_doc->nodes[i].tag != T_OPTION) continue;
        if (i == x) return js_num(k);
        k++;
    }
    return js_num(0);
}

static jval nat_option_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "Option is made with new", J->error_line);
    int o = dom_create_element(jd_doc, "option", 6);
    if (o < 0) return js_undef();
    if (n > 0 && a[0].t != JS_UNDEF) {
        jstr *s = js_to_str(J, a[0]);
        if (s && s->len) { int tn = jd_new_text(s->s, (int)s->len); if (tn >= 0) dom_append(jd_doc, o, tn); }
    }
    if (n > 1 && a[1].t != JS_UNDEF) { jstr *s = js_to_str(J, a[1]); if (s) dom_attr_set(jd_doc, o, "value", s->s); }
    if (n > 3 && js_to_bool(a[3])) dom_attr_set(jd_doc, o, "selected", "");
    return jd_el_value(J, o);
}

static jval nat_image_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "Image is made with new", J->error_line);
    int o = dom_create_element(jd_doc, "img", 3);
    if (o < 0) return js_undef();
    char num[16];
    for (int i = 0; i < 2 && i < n; i++) {
        if (a[i].t == JS_UNDEF) continue;
        int v = (int)js_to_num(J, a[i]), w = 0;
        char tmp[12];
        int k = 0;
        if (v < 0) v = 0;
        do { tmp[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k) num[w++] = tmp[--k];
        num[w] = 0;
        dom_attr_set(jd_doc, o, i ? "height" : "width", num);
    }
    return jd_el_value(J, o);
}

static jval nat_form_of(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    for (int p = jd_doc->nodes[x].parent; p >= 0; p = jd_doc->nodes[p].parent)
        if (jd_doc->nodes[p].tag == T_FORM) return jd_el_value(J, p);
    return js_null();
}

static jval nat_labels(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    const char *id = jd_attr(x, "id");
    return jd_list_value(J, JL_LABELS, 0, -1, js_str(J, id ? id : "\x01"));
}

static jval nat_true_fn(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_bool(1);
}

static jval nat_form_elements(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    return jd_list_value(J, JL_ELEMENTS, 1, x, 0);
}

static jval nat_form_length(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    int k = 0;
    for (int i = jd_walk_first(x); i >= 0; i = jd_walk_next(i, x))
        if (jd_is_element(i) && jd_is_form_control(i)) k++;
    return js_num(k);
}

/* A form sent by a script goes the way one sent by the reader does: the
   browser builds the request from what is in it (jsdom_submit_with). The
   submit event does not fire for submit(), as the standard has it;
   requestSubmit() fires it first and stops when it is cancelled. */
static void (*jd_submit)(int form);

static jval nat_form_submit(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    if (jd_submit) jd_submit(x);
    return js_undef();
}

static jval nat_form_request_submit(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    if (!jd_fire_simple(x, "submit", 1, 1) && jd_submit) jd_submit(x);
    return js_undef();
}

static jval nat_form_reset(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    jd_fire_simple(x, "reset", 1, 1);
    return js_undef();
}

static jval nat_form_action(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    const char *v = jd_attr(x, "action");
    if (!v || !*v) return jd_str(jd_address);
    jstr *s = jd_resolve_str(J, v);
    return s ? js_from_str(s) : jd_str(v);
}

static jval nat_label_control(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    const char *f = jd_attr(x, "for");
    if (f) {
        for (int i = jd_walk_first(-1); i >= 0; i = jd_walk_next(i, -1)) {
            const char *id = jd_is_element(i) ? jd_attr(i, "id") : 0;
            if (id && w_same(id, f)) return jd_el_value(J, i);
        }
        return js_null();
    }
    for (int i = jd_walk_first(x); i >= 0; i = jd_walk_next(i, x))
        if (jd_is_element(i) && jd_is_form_control(i)) return jd_el_value(J, i);
    return js_null();
}

static jval nat_select_text(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_undef();
}

static jval nat_selection_end(jctx *J, jval t, jval *a, int n) {
    jval v = nat_value(J, t, a, n);
    return v.t == JS_STR ? js_num(v.str->len) : js_num(0);
}

/* --- pictures and the rest ------------------------------------------------------------------ */

static jval nat_img_natural(jctx *J, jval t, int which) {
    (void)J;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    int w = 0, h = 0;
    if (jd_picture_size) jd_picture_size(x, &w, &h);
    return js_num(which ? h : w);
}

static jval nat_img_natural_w(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return nat_img_natural(J, t, 0); }
static jval nat_img_natural_h(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return nat_img_natural(J, t, 1); }

/* width and height: the attribute when there is one, else where the layout
   drew it, else the size it was decoded at. */
static jval nat_img_size(jctx *J, jval t, int which) {
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    const char *v = jd_attr(x, which ? "height" : "width");
    if (v && *v >= '0' && *v <= '9') {
        int k = 0;
        while (*v >= '0' && *v <= '9') k = k * 10 + (*v++ - '0');
        return js_num(k);
    }
    int bx, by, bw, bh;
    if (jd_box(x, &bx, &by, &bw, &bh) && (bw || bh)) return js_num(which ? bh : bw);
    return nat_img_natural(J, t, which);
}

static jval nat_img_width(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return nat_img_size(J, t, 0); }
static jval nat_img_height(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return nat_img_size(J, t, 1); }

static jval nat_img_set_width(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (s) jd_attr_set(x, "width", s->s);
    return js_undef();
}

static jval nat_img_set_height(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (s) jd_attr_set(x, "height", s->s);
    return js_undef();
}

static jval jd_resolved_promise(jctx *J, jval v) {
    jobj *p = js_promise_new(J);
    if (!p) return js_undef();
    js_promise_settle(J, p, 1, v);
    return js_from_obj(p);
}

static jval jd_rejected_dom(jctx *J, const char *name, const char *what) {
    jobj *p = js_promise_new(J);
    jobj *e = js_domexc_new(J, 0, js_str(J, what), js_str(J, name));
    if (!p || !e) return js_undef();
    js_promise_settle(J, p, 0, js_from_obj(e));
    return js_from_obj(p);
}

static jval nat_img_decode(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    return jd_resolved_promise(J, js_undef());
}

/* A canvas this browser has no drawing context for, and says so the way
   the standard lets it: getContext gives null. */
static jval nat_canvas_context(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_null();
}

static jval nat_canvas_data_url(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_str("data:,");
}

/* Sound and video: nothing here plays either, so a page asking whether one
   can be played is told no, and a play() is refused. */
static jval nat_media_can_play(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_str("");
}

static jval nat_media_play(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    return jd_rejected_dom(J, "NotSupportedError", "this browser plays no sound or video");
}

static jval nat_true_getter(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_bool(1);
}

static jval nat_false_getter(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_bool(0);
}

static jval nat_nan_getter(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_num(js_nan());
}

static jval nat_one_getter(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_num(1);
}

static jval nat_three_getter(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_num(3);
}

static jval nat_empty_str(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_str("");
}

/* A dialog opened and closed: the open attribute, which the browser's sheet
   draws or hides. */
static jval nat_dialog_show(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    jd_attr_set(x, "open", "");
    return js_undef();
}

static jval nat_dialog_close(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    if (n > 0 && a[0].t != JS_UNDEF) js_set(J, t.obj, "returnValue", js_from_str(js_to_str(J, a[0])));
    if (!jd_attr(x, "open")) return js_undef();
    jd_attr_remove(x, "open");
    jd_fire_simple(x, "close", 0, 0);
    return js_undef();
}

static jval nat_script_text(jctx *J, jval t, jval *a, int n) { return nat_text_content(J, t, a, n); }
static jval nat_script_set_text(jctx *J, jval t, jval *a, int n) { return nat_set_text_content(J, t, a, n); }

/* HTMLScriptElement.supports: a classic script, a module and an import map,
   yes (jsmod.h); speculation rules and the rest, no. */
static jval nat_script_supports(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *k = jd_arg_str(J, a, n, 0);
    return js_bool(js_str_is(k, "classic") || js_str_is(k, "module") || js_str_is(k, "importmap"));
}

/* --- template -------------------------------------------------------------------------------
 *
 * A template's markup is a fragment of its own, not part of the page. The
 * parser puts it under the template; the first time content is asked for,
 * it is moved into a fragment kept on the template's object, which is where
 * the standard has it and where a script cloning it looks. */
static int jd_template_content(int el) {
    jobj *w = jd_element(&jd_J, el);
    jval kept = jd_kept(w, jd_k_content);
    int f = jd_node_of(kept);
    if (f >= 0) return f;
    f = jd_new_fragment();
    if (f < 0) return -1;
    while (jd_doc->nodes[el].first >= 0) {
        int c = jd_doc->nodes[el].first;
        dom_unlink(jd_doc, c);
        dom_append(jd_doc, f, c);
    }
    jd_keep(w, jd_k_content, jd_el_value(&jd_J, f));
    jd_inert_mark[f >> 3] |= (u8)(1 << (f & 7));
    return f;
}

static jval nat_template_content(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    int f = jd_template_content(x);
    return f >= 0 ? jd_el_value(J, f) : js_null();
}

/* --- custom elements ------------------------------------------------------------------------
 *
 * customElements.define(name, class) makes every element of that name an
 * instance of the class: those already in the page are upgraded then, those
 * made later as they are made or put in the page. The class's constructor
 * runs through HTMLElement's, which hands back the element being upgraded
 * or makes one of the defined name (nat_html_element_ctor), and the
 * callbacks run as the standard names them: connected, disconnected, and
 * attributeChanged for the attributes it says it observes. */
#define JD_CUSTOM 256

typedef struct {
    jstr *name;
    jobj *ctor, *proto;
    jobj *observed;              /* an array of names, or none */
    jobj *waiting;               /* a promise whenDefined handed out, or none */
} jcustom;

static jcustom jd_custom[JD_CUSTOM];
static int     jd_ncustom;
static jobj   *jd_pending_defs[JD_CUSTOM];      /* whenDefined for names not yet defined */
static jstr   *jd_pending_names[JD_CUSTOM];
static int     jd_npending;
static int     jd_upgrading = -1;
static u8      jd_upgraded[DOM_NODES / 8];

static jcustom *jd_custom_by_name(const char *name) {
    for (int i = 0; i < jd_ncustom; i++) if (js_str_is(jd_custom[i].name, name)) return &jd_custom[i];
    return 0;
}

static jobj *jd_custom_proto(const char *name) {
    jcustom *c = jd_ncustom ? jd_custom_by_name(name) : 0;
    return c ? c->proto : 0;
}

static int jd_is_upgraded(int n) { return n >= 0 && n < DOM_NODES && (jd_upgraded[n >> 3] & (1 << (n & 7))); }

static void jd_custom_callback(int node, const char *which, jval *args, int nargs) {
    if (!jd_is_upgraded(node)) return;
    jobj *w = jd_element(&jd_J, node);
    if (!w) return;
    jval fn = js_get(&jd_J, js_from_obj(w), js_str(&jd_J, which));
    if (jd_J.sig != JS_OK) { jd_note_error(); jd_J.sig = JS_OK; return; }
    if (!js_callable(fn)) return;
    js_call(&jd_J, fn, js_from_obj(w), args, nargs);
    if (jd_J.sig != JS_OK) { jd_note_error(); jd_J.sig = JS_OK; }
}

static void jd_upgrade(int node) {
    if (jd_is_upgraded(node) || !jd_is_element(node)) return;
    jcustom *c = jd_custom_by_name(dom_tag_name(jd_doc, node));
    if (!c) return;
    jobj *w = jd_element(&jd_J, node);
    if (!w) return;
    w->proto = c->proto;
    jd_upgraded[node >> 3] |= (u8)(1 << (node & 7));
    int was = jd_upgrading;
    jd_upgrading = node;
    jsignal before = jd_J.sig;
    jd_J.sig = JS_OK;
    js_construct(&jd_J, js_from_obj(c->ctor), 0, 0, js_from_obj(c->ctor));
    if (jd_J.sig != JS_OK) { jd_note_error(); jd_J.sig = JS_OK; }
    else jd_J.sig = before;
    jd_upgrading = was;
    if (c->observed)
        for (u32 i = 0; i < c->observed->len; i++) {
            jval nm = c->observed->items[i];
            if (nm.t != JS_STR) continue;
            const char *v = jd_attr(node, nm.str->s);
            if (!v) continue;
            jval args[3] = { nm, js_null(), jd_str(v) };
            jd_custom_callback(node, "attributeChangedCallback", args, 3);
        }
    if (jd_connected_deep(node)) jd_custom_callback(node, "connectedCallback", 0, 0);
}

/* The elements that may be custom in a subtree, in the order the standard
   takes them: each, then what is in its shadow root, then its children. */
typedef struct { int *at; int n, cap; } jd_nodelist;

static void jd_custom_gather(int top, jd_nodelist *L) {
    for (int i = top; i >= 0; i = jd_walk_next(i, top)) {
        if (!jd_is_element(i)) continue;
        if (jd_doc->nodes[i].tag == T_OTHER) {
            if (L->n >= L->cap) {
                int cap = L->cap ? L->cap * 2 : 64;
                int *more = (int *)malloc((u64)cap * sizeof(int));
                if (!more) return;
                for (int k = 0; k < L->n; k++) more[k] = L->at[k];
                free(L->at);
                L->at = more;
                L->cap = cap;
            }
            L->at[L->n++] = i;
        }
        int s = jd_shadow_find(i, 0);
        if (s >= 0)
            for (int c = jd_doc->nodes[jd_shadow_root[s]].first; c >= 0; c = jd_doc->nodes[c].next)
                jd_custom_gather(c, L);
    }
}

/* Whether a node is in a document with no window: a template's contents,
   or one DOMParser made. */
static int jd_inert(int x) {
    int top = x;
    for (int k = 0; top >= 0 && jd_doc->nodes[top].parent >= 0 && k < DOM_NODES; k++) top = jd_doc->nodes[top].parent;
    if (top < 0) return 0;
    if (top < DOM_NODES && (jd_inert_mark[top >> 3] & (1 << (top & 7)))) return 1;
    return top != jd_doc->root && jd_kind(top) == JN_DOCUMENT;
}

/* Elements of defined names just made by the page's document, in or out of
   the page: upgraded then, as a browser upgrades what markup, cloneNode and
   importNode make before handing it over. They were upgraded only once put
   in the page, and a component's property set in between (FAST binds its
   views' properties before they go in) reached a class not yet made: MSN's
   header read a field its constructor had not yet set, and stopped. */
static void jd_custom_created(int top) {
    if (!jd_ncustom || top < 0 || jd_inert(top)) return;
    jd_nodelist L = { 0, 0, 0 };
    jd_custom_gather(top, &L);
    for (int k = 0; k < L.n; k++) if (!jd_is_upgraded(L.at[k])) jd_upgrade(L.at[k]);
    free(L.at);
}

/* Elements of defined names in a subtree just put in the page, and in the
   shadow roots in it: upgraded if they were not, told they are connected if
   they were. Taken from a list made first, as the standard queues them: an
   element a callback adds is told by its own insertion, and was told twice
   when the walk then reached it. */
static void jd_custom_connected(int top) {
    if (!jd_ncustom) return;
    jd_nodelist L = { 0, 0, 0 };
    jd_custom_gather(top, &L);
    for (int k = 0; k < L.n; k++) {
        int i = L.at[k];
        if (!jd_connected_deep(i)) continue;          /* taken out again by a callback */
        if (jd_is_upgraded(i)) jd_custom_callback(i, "connectedCallback", 0, 0);
        else jd_upgrade(i);
    }
    free(L.at);
}

static void jd_custom_disconnected(int top) {
    if (!jd_ncustom) return;
    jd_nodelist L = { 0, 0, 0 };
    jd_custom_gather(top, &L);
    for (int k = 0; k < L.n; k++)
        if (jd_is_upgraded(L.at[k])) jd_custom_callback(L.at[k], "disconnectedCallback", 0, 0);
    free(L.at);
}

static void jd_custom_attr(int node, const char *name, const char *old, const char *now) {
    if (!jd_ncustom || !jd_is_upgraded(node)) return;
    jcustom *c = jd_custom_by_name(dom_tag_name(jd_doc, node));
    if (!c || !c->observed) return;
    for (u32 i = 0; i < c->observed->len; i++) {
        jval nm = c->observed->items[i];
        if (nm.t != JS_STR || !js_str_is(nm.str, name)) continue;
        jval args[3] = { nm, old ? jd_str(old) : js_null(), now ? jd_str(now) : js_null() };
        jd_custom_callback(node, "attributeChangedCallback", args, 3);
        return;
    }
}

/* new MyElement() and super() in one: during an upgrade, the element being
   upgraded; otherwise a new element of the name the class was defined
   with. HTMLElement on its own, or a class nobody defined, cannot be made. */
static jval nat_html_element_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jval nt = J->new_target;
    if (nt.t != JS_OBJ) return nat_illegal_ctor(J, t, a, n);
    jcustom *c = 0;
    for (int i = 0; i < jd_ncustom; i++) if (jd_custom[i].ctor == nt.obj) c = &jd_custom[i];
    if (!c) return nat_illegal_ctor(J, t, a, n);
    if (jd_upgrading >= 0 && w_same(dom_tag_name(jd_doc, jd_upgrading), c->name->s)) {
        jobj *w = jd_element(J, jd_upgrading);
        return w ? js_from_obj(w) : js_undef();
    }
    int el = dom_create_element(jd_doc, c->name->s, (int)c->name->len);
    if (el < 0) return js_throw_dom(J, "NotSupportedError", "the document is full");
    jd_upgraded[el >> 3] |= (u8)(1 << (el & 7));
    jobj *w = jd_element(J, el);
    if (w) w->proto = js_proto_from(J, nt, c->proto);
    return w ? js_from_obj(w) : js_undef();
}

static int jd_valid_custom_name(const jstr *s) {
    if (!s->len || s->s[0] < 'a' || s->s[0] > 'z') return 0;
    int dash = 0;
    for (u32 i = 0; i < s->len; i++) {
        char c = s->s[i];
        if (c == '-') dash = 1;
        else if (c >= 'A' && c <= 'Z') return 0;
        else if (c == ' ' || c == '>' || c == '<' || c == '/') return 0;
    }
    return dash;
}

static jval nat_ce_define(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *name = jd_arg_str(J, a, n, 0);
    jval cls = js_arg(a, n, 1);
    if (!jd_valid_custom_name(name))
        return js_throw_dom(J, "SyntaxError", "a custom element's name needs a hyphen and lower case");
    if (!js_is_constructor(J, cls)) return js_throw(J, JS_ERR_TYPE, "a custom element needs a class", J->error_line);
    if (jd_custom_by_name(name->s)) return js_throw_dom(J, "NotSupportedError", "that name is already defined");
    if (jd_ncustom >= JD_CUSTOM) return js_throw_dom(J, "NotSupportedError", "too many custom elements");
    jval proto = js_get(J, cls, J->s_prototype);
    if (J->sig != JS_OK) return js_undef();
    jval obs = js_get(J, cls, js_str(J, "observedAttributes"));
    if (J->sig != JS_OK) return js_undef();
    jcustom *c = &jd_custom[jd_ncustom++];
    c->name = name;
    c->ctor = cls.obj;
    c->proto = js_is_obj(proto) ? proto.obj : jd_p[JI_HTMLELEMENT];
    c->observed = 0;
    c->waiting = 0;
    if (js_is_obj(obs)) {
        jargs A;
        js_args_init(&A);
        if (js_iter_collect(J, obs, &A)) {
            jobj *arr = js_array(J);
            for (int i = 0; arr && i < A.n; i++) js_arr_push(J, arr, js_from_str(js_to_str(J, A.v[i])));
            c->observed = arr;
        }
        js_args_free(&A);
        J->sig = JS_OK;
    }
    /* Everything of that name already here, in document order -- but not in
       a template, whose contents are no document's until copied out. */
    for (int i = 0; i < jd_doc->count; i++)
        if (jd_is_element(i) && jd_doc->nodes[i].tag == T_OTHER && jd_connected_deep(i)
            && js_str_is(name, dom_tag_name(jd_doc, i)) && !jd_has_ancestor_tag(i, T_OTHER, "template"))
            jd_upgrade(i);
    for (int i = 0; i < jd_npending; i++)
        if (jd_pending_defs[i] && js_str_eq(jd_pending_names[i], name)) {
            js_promise_settle(J, jd_pending_defs[i], 1, cls);
            jd_pending_defs[i] = 0;
        }
    return js_undef();
}

static jval nat_ce_get(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jcustom *c = jd_custom_by_name(jd_arg_str(J, a, n, 0)->s);
    return c ? js_from_obj(c->ctor) : js_undef();
}

static jval nat_ce_when(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *name = jd_arg_str(J, a, n, 0);
    jcustom *c = jd_custom_by_name(name->s);
    if (c) return jd_resolved_promise(J, js_from_obj(c->ctor));
    for (int i = 0; i < jd_npending; i++)
        if (jd_pending_defs[i] && js_str_eq(jd_pending_names[i], name)) return js_from_obj(jd_pending_defs[i]);
    jobj *p = js_promise_new(J);
    if (p && jd_npending < JD_CUSTOM) {
        jd_pending_defs[jd_npending] = p;
        jd_pending_names[jd_npending++] = name;
    }
    return js_from_obj(p);
}

static jval nat_ce_upgrade(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t;
    int x = jd_node_of(js_arg(a, n, 0));
    if (x < 0) return js_undef();
    for (int i = x; i >= 0; i = jd_walk_next(i, x)) jd_upgrade(i);
    return js_undef();
}

/* --- events ----------------------------------------------------------------------------------
 *
 * A listener is a function and what it is on, kept in a table of the
 * browser's rather than on the object, so that what a page can see of an
 * element stays what the DOM says an element has. What it is on is an
 * object, not a node: the window, the document, an element, and anything
 * else a page listens to -- a request, an AbortSignal, an EventTarget of its
 * own making.
 *
 * Delivery is the standard's three phases: down from the window to the
 * target's parent for the listeners that asked to capture, at the target,
 * and back up for the rest if the event bubbles. A handler written in an
 * attribute or set as a property (onclick) runs where a listener would, at
 * the target and on the way up, before the listeners on the same node. */
typedef struct {
    jobj *on;
    jstr *type;
    jval  fn;                    /* a function, or an object with handleEvent */
    jobj *signal;                /* an AbortSignal that takes it away, or none */
    u8    capture, once, passive, gone;
} jlisten;

static jlisten *jd_listen;
static int      jd_nlisten, jd_caplisten;

/* An event's state the page does not see: stopped, stopped at once, inside a
   passive listener. Kept under a symbol on the event, so that one event
   dispatched inside another's handler keeps its own. */
#define JE_STOP      1
#define JE_STOPNOW   2
#define JE_PASSIVE   4
#define JE_BUSY      8
#define JE_INITED    16

static int jd_ev_flags(jobj *ev) {
    jprop *p = js_find(ev, jd_k_evflags);
    return p && p->v.t == JS_NUM ? (int)p->v.num : 0;
}

static void jd_ev_set_flags(jobj *ev, int f) { jd_keep(ev, jd_k_evflags, js_num(f)); }

static jval jd_ev_get(jobj *ev, const char *name) {
    jprop *p = js_find(ev, js_str(&jd_J, name));
    return p && p->v.t != JS_ACC ? p->v : js_undef();
}

/* Who a listener is on: an object, or the window for a call with nothing
   before the dot. */
static jobj *jd_listener_on(jctx *J, jval t) {
    if (t.t != JS_OBJ || !t.obj) return J->global_obj;
    return t.obj;
}

static int jd_listen_same(const jlisten *L, jobj *on, const jstr *type, jval fn, int capture) {
    return !L->gone && L->on == on && L->capture == capture && js_str_eq(L->type, type)
        && L->fn.t == JS_OBJ && fn.t == JS_OBJ && L->fn.obj == fn.obj;
}

static jval nat_add_listener(jctx *J, jval t, jval *a, int n) {
    jval fn = js_arg(a, n, 1);
    if (fn.t != JS_OBJ || !fn.obj) return js_undef();
    jstr *ty = js_to_str(J, js_arg(a, n, 0));
    if (!ty) return js_undef();
    int capture = 0, once = 0, passive = 0;
    jobj *signal = 0;
    jval opt = js_arg(a, n, 2);
    if (js_is_obj(opt)) {
        capture = js_to_bool(js_get(J, opt, js_str(J, "capture")));
        once = js_to_bool(js_get(J, opt, js_str(J, "once")));
        passive = js_to_bool(js_get(J, opt, js_str(J, "passive")));
        jval s = js_get(J, opt, js_str(J, "signal"));
        if (js_is_obj(s)) {
            signal = s.obj;
            if (js_to_bool(js_get(J, s, js_str(J, "aborted")))) return js_undef();
        }
    } else {
        capture = js_to_bool(opt);
    }
    if (J->sig != JS_OK) return js_undef();
    jobj *on = jd_listener_on(J, t);
    for (int i = 0; i < jd_nlisten; i++)
        if (jd_listen_same(&jd_listen[i], on, ty, fn, capture)) return js_undef();
    /* A place freed by one removed, before growing the table. */
    int slot = -1;
    if (jd_nlisten >= jd_caplisten) {
        for (int i = 0; i < jd_nlisten; i++) if (jd_listen[i].gone) { slot = i; break; }
        if (slot < 0) {
            int cap = jd_caplisten ? jd_caplisten * 2 : 256;
            if (cap > (1 << 18)) return js_undef();
            /* In the region, where the collector sees the listeners (jsgc.h). */
            jlisten *more = (jlisten *)js_alloc(&jd_J, (u32)cap * (u32)sizeof(jlisten));
            if (!more) return js_undef();
            volatile u8 *d = (volatile u8 *)more;
            const u8 *s = (const u8 *)jd_listen;
            for (u64 i = 0; i < (u64)jd_nlisten * sizeof(jlisten); i++) d[i] = s[i];
            if (jd_listen) js_free(&jd_J, jd_listen, (u32)jd_caplisten * (u32)sizeof(jlisten));
            jd_listen = more;
            jd_caplisten = cap;
        }
    }
    if (slot < 0) slot = jd_nlisten++;
    jlisten *L = &jd_listen[slot];
    L->on = on;
    L->type = ty;
    L->fn = fn;
    L->signal = signal;
    L->capture = (u8)capture;
    L->once = (u8)once;
    L->passive = (u8)passive;
    L->gone = 0;
    return js_undef();
}

static jval nat_remove_listener(jctx *J, jval t, jval *a, int n) {
    jstr *ty = js_to_str(J, js_arg(a, n, 0));
    jval fn = js_arg(a, n, 1);
    if (!ty || fn.t != JS_OBJ) return js_undef();
    jval opt = js_arg(a, n, 2);
    int capture = js_is_obj(opt) ? js_to_bool(js_get(J, opt, js_str(J, "capture"))) : js_to_bool(opt);
    jobj *on = jd_listener_on(J, t);
    for (int i = 0; i < jd_nlisten; i++)
        if (jd_listen_same(&jd_listen[i], on, ty, fn, capture)) jd_listen[i].gone = 1;
    return js_undef();
}

/* Everything an AbortSignal took away, when it is aborted. */
__attribute__((unused)) static void jd_listeners_aborted(jobj *signal) {
    for (int i = 0; i < jd_nlisten; i++)
        if (jd_listen[i].signal == signal) jd_listen[i].gone = 1;
}

/* --- event objects ---------------------------------------------------------------------------
 *
 * One constructor native for every kind, told which by what is kept on it,
 * with the names each kind takes from its init dictionary and what each is
 * when the dictionary leaves it out: n a number, b false, s "", o null. */
typedef struct { const char *name, *parent, *keys; } jd_evkind_t;

static const jd_evkind_t JD_EVENTS[] = {
    { "Event", 0, "" },
    { "CustomEvent", "Event", "odetail" },
    { "UIEvent", "Event", "oview ndetail" },
    { "MouseEvent", "UIEvent", "nscreenX nscreenY nclientX nclientY nbutton nbuttons orelatedTarget"
                               " bctrlKey bshiftKey baltKey bmetaKey nmovementX nmovementY" },
    { "WheelEvent", "MouseEvent", "ndeltaX ndeltaY ndeltaZ ndeltaMode" },
    { "KeyboardEvent", "UIEvent", "skey scode nlocation bctrlKey bshiftKey baltKey bmetaKey brepeat"
                                  " bisComposing ncharCode nkeyCode nwhich" },
    { "FocusEvent", "UIEvent", "orelatedTarget" },
    { "InputEvent", "UIEvent", "odata sinputType bisComposing" },
    { "CompositionEvent", "UIEvent", "sdata" },
    { "ErrorEvent", "Event", "smessage sfilename nlineno ncolno oerror" },
    { "PromiseRejectionEvent", "Event", "opromise oreason" },
    { "PopStateEvent", "Event", "ostate" },
    { "HashChangeEvent", "Event", "soldURL snewURL" },
    { "MessageEvent", "Event", "odata sorigin slastEventId osource oports" },
    { "ProgressEvent", "Event", "blengthComputable nloaded ntotal" },
    { "PageTransitionEvent", "Event", "bpersisted" },
    { "StorageEvent", "Event", "okey ooldValue onewValue surl ostorageArea" },
    { "SubmitEvent", "Event", "osubmitter" },
    /* The rest a page makes or takes the prototype of: Mozilla's consent
       manager makes a SecurityPolicyViolationEvent to learn its isTrusted. */
    { "PointerEvent", "MouseEvent", "npointerId nwidth nheight npressure ntangentialPressure ntiltX ntiltY ntwist"
                                    " spointerType bisPrimary" },
    { "DragEvent", "MouseEvent", "odataTransfer" },
    { "TouchEvent", "UIEvent", "otouches otargetTouches ochangedTouches bctrlKey bshiftKey baltKey bmetaKey" },
    { "AnimationEvent", "Event", "sanimationName nelapsedTime spseudoElement" },
    { "TransitionEvent", "Event", "spropertyName nelapsedTime spseudoElement" },
    { "ClipboardEvent", "Event", "oclipboardData" },
    { "SecurityPolicyViolationEvent", "Event", "sdocumentURI sreferrer sblockedURI sviolatedDirective seffectiveDirective"
                                              " soriginalPolicy sdisposition ssourceFile nstatusCode nlineNumber ncolumnNumber ssample" },
    { "FormDataEvent", "Event", "oformData" },
    { "ToggleEvent", "Event", "soldState snewState" },
};
#define JD_EVKINDS ((int)(sizeof(JD_EVENTS) / sizeof(JD_EVENTS[0])))
static jobj *jd_ev_proto[JD_EVKINDS];

static int jd_evkind(const char *name) {
    for (int i = 0; i < JD_EVKINDS; i++) if (w_same(JD_EVENTS[i].name, name)) return i;
    return 0;
}

static double jd_now_ms(void);

/* The fields every event has, and the ones its kind adds, from an init
   dictionary or their defaults. */
static void jd_ev_init(jctx *J, jobj *e, jstr *type, jval init, int kind) {
    js_set(J, e, "type", js_from_str(type));
    js_set(J, e, "bubbles", js_bool(js_is_obj(init) && js_to_bool(js_get(J, init, js_str(J, "bubbles")))));
    js_set(J, e, "cancelable", js_bool(js_is_obj(init) && js_to_bool(js_get(J, init, js_str(J, "cancelable")))));
    js_set(J, e, "composed", js_bool(js_is_obj(init) && js_to_bool(js_get(J, init, js_str(J, "composed")))));
    js_set(J, e, "defaultPrevented", js_bool(0));
    js_set(J, e, "isTrusted", js_bool(0));
    js_set(J, e, "timeStamp", js_num(jd_now_ms()));
    js_set(J, e, "target", js_null());
    js_set(J, e, "currentTarget", js_null());
    js_set(J, e, "srcElement", js_null());
    js_set(J, e, "eventPhase", js_num(0));
    js_set(J, e, "returnValue", js_bool(1));
    js_set(J, e, "cancelBubble", js_bool(0));
    /* The kind's own, and its parents', from the most basic kind down. */
    int chain[8], nc = 0;
    for (int k = kind; k > 0 && nc < 8; ) {
        chain[nc++] = k;
        const char *par = JD_EVENTS[k].parent;
        k = par ? jd_evkind(par) : 0;
    }
    while (nc > 0) {
        const char *p = JD_EVENTS[chain[--nc]].keys;
        char key[32];
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            char ty = *p++;
            int k = 0;
            while (*p && *p != ' ' && k < 31) key[k++] = *p++;
            key[k] = 0;
            jval v = js_is_obj(init) ? js_get(J, init, js_str(J, key)) : js_undef();
            if (v.t == JS_UNDEF)
                v = ty == 'n' ? js_num(0) : ty == 'b' ? js_bool(0) : ty == 's' ? jd_str("") : js_null();
            js_set(J, e, key, v);
        }
    }
    if (J->sig == JS_OK) jd_ev_set_flags(e, JE_INITED);
}

static jval nat_event_ctor(jctx *J, jval t, jval *a, int n) {
    int kind = J->callee && J->callee->data.t == JS_NUM ? (int)J->callee->data.num : 0;
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "an event is made with new", J->error_line);
    if (n < 1) return js_throw(J, JS_ERR_TYPE, "an event needs a type", J->error_line);
    jstr *type = js_to_str(J, a[0]);
    if (!type) return js_undef();
    jd_ev_init(J, t.obj, type, js_arg(a, n, 1), kind);
    return js_undef();
}

/* A new event of a kind, for the browser's own: a click, a load. */
static jobj *jd_new_event(int kind, const char *type, int bubbles, int cancelable) {
    jobj *e = js_object_with(&jd_J, JO_PLAIN, jd_ev_proto[kind]);
    if (!e) return 0;
    jd_ev_init(&jd_J, e, js_str(&jd_J, type), js_undef(), kind);
    js_set(&jd_J, e, "bubbles", js_bool(bubbles));
    js_set(&jd_J, e, "cancelable", js_bool(cancelable));
    return e;
}

static jval nat_ev_prevent(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return js_undef();
    if (!js_to_bool(jd_ev_get(t.obj, "cancelable"))) return js_undef();
    if (jd_ev_flags(t.obj) & JE_PASSIVE) return js_undef();
    js_set(J, t.obj, "defaultPrevented", js_bool(1));
    js_set(J, t.obj, "returnValue", js_bool(0));
    return js_undef();
}

static jval nat_ev_stop(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (js_is_obj(t)) jd_ev_set_flags(t.obj, jd_ev_flags(t.obj) | JE_STOP);
    return js_undef();
}

static jval nat_ev_stop_now(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (js_is_obj(t)) jd_ev_set_flags(t.obj, jd_ev_flags(t.obj) | JE_STOP | JE_STOPNOW);
    return js_undef();
}

static jval nat_ev_path(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return js_undef();
    jval p = jd_kept(t.obj, jd_k_evpath);
    if (p.t == JS_OBJ) return p;
    jobj *arr = js_array(J);
    return js_from_obj(arr);
}

/* document.createEvent and initEvent, which pages written before the
   constructors still use to make an event and send it. */
static jval nat_ev_init_event(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return js_undef();
    if (jd_ev_flags(t.obj) & JE_BUSY) return js_undef();
    js_set(J, t.obj, "type", js_from_str(jd_arg_str(J, a, n, 0)));
    js_set(J, t.obj, "bubbles", js_bool(js_to_bool(js_arg(a, n, 1))));
    js_set(J, t.obj, "cancelable", js_bool(js_to_bool(js_arg(a, n, 2))));
    js_set(J, t.obj, "defaultPrevented", js_bool(0));
    jd_ev_set_flags(t.obj, JE_INITED);
    return js_undef();
}

static jval nat_ev_init_custom(jctx *J, jval t, jval *a, int n) {
    nat_ev_init_event(J, t, a, n);
    if (js_is_obj(t)) js_set(J, t.obj, "detail", js_arg(a, n, 3));
    return js_undef();
}

static jval nat_ev_modifier(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return js_bool(0);
    jstr *k = jd_arg_str(J, a, n, 0);
    const char *prop = js_str_is(k, "Control") ? "ctrlKey" : js_str_is(k, "Shift") ? "shiftKey"
                     : js_str_is(k, "Alt") ? "altKey" : js_str_is(k, "Meta") ? "metaKey" : 0;
    return js_bool(prop && js_to_bool(jd_ev_get(t.obj, prop)));
}

static jval nat_create_event(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *which = jd_arg_str(J, a, n, 0);
    char nm[40];
    int k = 0;
    for (; k < (int)which->len && k < 39; k++) nm[k] = which->s[k];
    nm[k] = 0;
    if (k > 1 && nm[k - 1] == 's' && !w_same(nm, "HTMLEvents")) nm[--k] = 0;     /* MouseEvents */
    int kind = w_same(nm, "HTMLEvents") || w_same(nm, "Event") ? 0 : -1;
    for (int i = 0; i < JD_EVKINDS && kind < 0; i++) if (w_same_fold(JD_EVENTS[i].name, nm)) kind = i;
    if (kind < 0) return js_throw_dom(J, "NotSupportedError", "that kind of event cannot be made here");
    jobj *e = js_object_with(J, JO_PLAIN, jd_ev_proto[kind]);
    if (!e) return js_undef();
    jd_ev_init(J, e, js_str(J, ""), js_undef(), kind);
    jd_ev_set_flags(e, 0);
    return js_from_obj(e);
}

/* --- delivering one ---------------------------------------------------------------------------- */

/* The name of the handler for an event: onclick for click. */
static void jd_handler_name(const jstr *type, char *out, int cap) {
    int w = 0;
    out[w++] = 'o';
    out[w++] = 'n';
    for (u32 i = 0; i < type->len && w < cap - 1; i++) out[w++] = type->s[i];
    out[w] = 0;
}

/* A handler written in an attribute. The text is a function body and not an
   expression, so it is wrapped in a function before being compiled -- which
   also gives it a scope of its own, so a `var` in an attribute does not
   land among the page's globals. Compiled when it fires rather than when it
   is read, because the great majority of them never fire at all. */
static jval jd_attr_handler(int node, const char *name) {
    const char *body = dom_attr(jd_doc, node, name);
    if (!body || !*body) return js_undef();
    jtext t = { 0, 0, 0, 0 };
    jd_put(&t, "(function(event){");
    jd_put(&t, body);
    jd_put(&t, "\n})");
    jval fn;
    int ok = js_eval_text(&jd_J, t.b ? t.b : "", t.n, &fn);
    free(t.b);
    if (!ok) { jd_note_error(); jd_J.sig = JS_OK; return js_undef(); }
    return fn;
}

/* The window's handlers that a page writes on its body element. */
static int jd_body_handler(const jstr *type) {
    static const char *const W[] = { "load", "unload", "beforeunload", "hashchange", "popstate",
                                     "message", "online", "offline", "pageshow", "pagehide",
                                     "resize", "storage", "error", "focus", "blur", 0 };
    for (int i = 0; W[i]; i++) if (js_str_is(type, W[i])) return 1;
    return 0;
}

/* Calls one listener or handler. A handler that returns false has refused
   the default, which is how pages said so before there was
   preventDefault, and still do. */
static jval jd_window_event;             /* window.event, while a listener runs */

static void jd_invoke(jval fn, jval self, jobj *ev, int is_handler) {
    jval arg = js_from_obj(ev);
    jval got;
    jval was = jd_window_event;
    jd_window_event = arg;
    jd_J.sig = JS_OK;
    jd_J.steps = 0;
    if (js_callable(fn)) {
        got = js_call(&jd_J, fn, self, &arg, 1);
    } else {
        jval m = js_get(&jd_J, fn, js_str(&jd_J, "handleEvent"));
        if (jd_J.sig == JS_OK && js_callable(m)) got = js_call(&jd_J, m, fn, &arg, 1);
        else got = js_undef();
    }
    jd_window_event = was;
    if (jd_J.sig != JS_OK) { jd_note_error(); jd_J.sig = JS_OK; return; }
    if (is_handler && got.t == JS_BOOL && !got.b && js_to_bool(jd_ev_get(ev, "cancelable"))) {
        js_set(&jd_J, ev, "defaultPrevented", js_bool(1));
        js_set(&jd_J, ev, "returnValue", js_bool(0));
    }
}

/* One stop on the path: its handler if the phase runs handlers, then its
   listeners of the phase, in the order they were added. The count is taken
   once: a listener added while this runs has not asked to be in it. */
static void jd_deliver(jobj *on, int node, jobj *ev, const jstr *type, int phase, int pass) {
    js_set(&jd_J, ev, "currentTarget", js_from_obj(on));
    js_set(&jd_J, ev, "eventPhase", js_num(phase));
    jval self = js_from_obj(on);

    if (pass == 3) {
        char name[48];
        jd_handler_name(type, name, (int)sizeof(name));
        jprop *pr = js_find(on, js_str(&jd_J, name));
        jval h = pr && pr->v.t != JS_ACC ? pr->v : js_undef();
        if (!js_callable(h) && node >= 0 && jd_is_element(node)) h = jd_attr_handler(node, name);
        if (!js_callable(h) && on == jd_J.global_obj && jd_doc->body >= 0 && jd_body_handler(type))
            h = jd_attr_handler(jd_doc->body, name);
        if (js_callable(h)) {
            jd_invoke(h, self, ev, 1);
            if (jd_ev_flags(ev) & JE_STOPNOW) return;
        }
    }

    int upto = jd_nlisten;
    for (int i = 0; i < upto; i++) {
        jlisten *L = &jd_listen[i];
        if (L->gone || L->on != on || !js_str_eq(L->type, type)) continue;
        if (pass == 1 && !L->capture) continue;
        if (pass == 3 && L->capture) continue;
        if (L->once) L->gone = 1;
        jval fn = L->fn;
        int f = jd_ev_flags(ev);
        if (L->passive) jd_ev_set_flags(ev, f | JE_PASSIVE);
        jd_invoke(fn, self, ev, 0);
        jd_ev_set_flags(ev, jd_ev_flags(ev) & ~JE_PASSIVE);
        if (jd_ev_flags(ev) & JE_STOPNOW) return;
    }
}

#define JD_PATH_MAX 1024

/* The target a stop on the path sees (jd_dispatch_to's seen). */
static void jd_seen_as(jobj *ev, int host, jval given) {
    jval v = host >= 0 ? jd_el_value(&jd_J, host) : given;
    js_set(&jd_J, ev, "target", v);
    js_set(&jd_J, ev, "srcElement", v);
}

/* An event sent to a target: a node's object, the document, the window, or
   anything else. `as_target` is what event.target says, which for the
   window's load is the document. Returns whether the default was
   refused. */
static int jd_dispatch_to(jobj *ev, jval target, jval as_target) {
    if (!jd_open || !jd_doc || jd_spent() || !ev || !js_is_obj(target)) return 0;
    jobj *path[JD_PATH_MAX];
    int nodes[JD_PATH_MAX];
    /* What each stop sees as the target: the one given (-1), or past a shadow
       root the element whose root it is, as the standard retargets it. */
    int seen[JD_PATH_MAX];
    int np = 0, crossed = 0;
    int tn = jd_node_of(target);
    if (tn >= 0) {
        int at = tn, as = -1;
        int composed = js_to_bool(jd_ev_get(ev, "composed"));
        while (at >= 0 && np < JD_PATH_MAX - 2) {
            jobj *o = jd_element(&jd_J, at);
            if (!o) break;
            path[np] = o;
            seen[np] = as;
            nodes[np++] = at;
            if (jd_is_top(at)) break;
            int p = jd_doc->nodes[at].parent;
            if (p < 0 && composed) {
                int s = jd_shadow_find(at, 1);
                if (s >= 0) as = p = jd_shadow_host[s], crossed = 1;
            }
            at = p;
        }
        if (at >= 0 && jd_is_top(at) && jd_document_obj) {
            path[np] = jd_document_obj; seen[np] = as; nodes[np++] = -1;
            if (jd_J.global_obj && !js_str_is(js_to_str(&jd_J, jd_ev_get(ev, "type")), "load")) {
                path[np] = jd_J.global_obj; seen[np] = as; nodes[np++] = -1;
            }
        }
    } else if (jd_is_doc(target)) {
        path[np] = target.obj; seen[np] = -1; nodes[np++] = -1;
        if (jd_J.global_obj) { path[np] = jd_J.global_obj; seen[np] = -1; nodes[np++] = -1; }
    } else {
        path[np] = target.obj; seen[np] = -1; nodes[np++] = -1;
    }

    jval tyv = jd_ev_get(ev, "type");
    if (tyv.t != JS_STR) return 0;
    jstr *type = tyv.str;
    int flags = (jd_ev_flags(ev) & JE_INITED) | JE_BUSY;
    jd_ev_set_flags(ev, flags);
    js_set(&jd_J, ev, "target", as_target);
    js_set(&jd_J, ev, "srcElement", as_target);
    js_set(&jd_J, ev, "defaultPrevented", js_bool(0));
    jobj *arr = js_array(&jd_J);
    for (int i = 0; arr && i < np; i++) js_arr_push(&jd_J, arr, js_from_obj(path[i]));
    jd_keep(ev, jd_k_evpath, js_from_obj(arr));

    int bubbles = js_to_bool(jd_ev_get(ev, "bubbles"));
    for (int i = np - 1; i > 0 && !(jd_ev_flags(ev) & JE_STOP); i--) {
        if (crossed) jd_seen_as(ev, seen[i], as_target);
        jd_deliver(path[i], nodes[i], ev, type, 1, 1);
    }
    if (!(jd_ev_flags(ev) & JE_STOP)) {
        /* At the target the capture listeners run first, then the rest. */
        if (crossed) jd_seen_as(ev, seen[0], as_target);
        jd_deliver(path[0], nodes[0], ev, type, 2, 1);
        if (!(jd_ev_flags(ev) & JE_STOPNOW)) jd_deliver(path[0], nodes[0], ev, type, 2, 3);
    }
    for (int i = 1; bubbles && i < np && !(jd_ev_flags(ev) & JE_STOP); i++) {
        if (js_to_bool(jd_ev_get(ev, "cancelBubble"))) break;
        if (crossed) jd_seen_as(ev, seen[i], as_target);
        jd_deliver(path[i], nodes[i], ev, type, 3, 3);
    }

    js_set(&jd_J, ev, "currentTarget", js_null());
    js_set(&jd_J, ev, "eventPhase", js_num(0));
    jd_ev_set_flags(ev, jd_ev_flags(ev) & JE_INITED);
    jd_keep(ev, jd_k_evpath, js_undef());
    return js_to_bool(jd_ev_get(ev, "defaultPrevented"));
}

static jval nat_dispatch_event(jctx *J, jval t, jval *a, int n) {
    jval ev = js_arg(a, n, 0);
    if (!js_is_obj(ev) || jd_ev_get(ev.obj, "type").t != JS_STR)
        return js_throw(J, JS_ERR_TYPE, "dispatchEvent needs an event", J->error_line);
    int f = jd_ev_flags(ev.obj);
    if (f & JE_BUSY) return js_throw_dom(J, "InvalidStateError", "that event is already being sent");
    if (!(f & JE_INITED)) return js_throw_dom(J, "InvalidStateError", "that event was never given a type");
    jval target = t.t == JS_OBJ && t.obj ? t : js_from_obj(J->global_obj);
    js_set(J, ev.obj, "isTrusted", js_bool(0));
    int prevented = jd_dispatch_to(ev.obj, target, target);
    return js_bool(!prevented);
}

static jval nat_event_target_ctor(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "EventTarget is made with new", J->error_line);
    return t;
}

/* The browser's own events, which a page cannot fake: isTrusted is true
   only for these. */
static int jd_fire_simple(int node, const char *type, int bubbles, int cancelable) {
    if (!jd_open || node < 0 || jd_spent()) return 0;
    jobj *ev = jd_new_event(0, type, bubbles, cancelable);
    if (!ev) return 0;
    js_set(&jd_J, ev, "isTrusted", js_bool(1));
    jval tv = jd_el_value(&jd_J, node);
    return jd_dispatch_to(ev, tv, tv);
}

static void (*jd_navigate)(const char *url, int replace);

/* A click: from the browser (trusted, and the browser does what a click
   does itself afterwards: follows the link, ticks the box) or from a
   script's click(), after which this does it. */
static int jd_fire_click(int node, int trusted, int x, int y) {
    if (!jd_open || node < 0 || jd_spent()) return 0;
    jobj *ev = jd_new_event(jd_evkind("MouseEvent"), "click", 1, 1);
    if (!ev) return 0;
    js_set(&jd_J, ev, "isTrusted", js_bool(trusted));
    js_set(&jd_J, ev, "composed", js_bool(1));
    js_set(&jd_J, ev, "detail", js_num(1));
    js_set(&jd_J, ev, "view", js_from_obj(jd_J.global_obj));
    js_set(&jd_J, ev, "clientX", js_num(x));
    js_set(&jd_J, ev, "clientY", js_num(y));
    js_set(&jd_J, ev, "screenX", js_num(x));
    js_set(&jd_J, ev, "screenY", js_num(y));
    js_set(&jd_J, ev, "pageX", js_num(x));
    js_set(&jd_J, ev, "pageY", js_num(y + jd_scroll_y));
    js_set(&jd_J, ev, "x", js_num(x));
    js_set(&jd_J, ev, "y", js_num(y));
    int bx, by, bw, bh;
    jd_box(node, &bx, &by, &bw, &bh);
    js_set(&jd_J, ev, "offsetX", js_num(x - bx));
    js_set(&jd_J, ev, "offsetY", js_num(y - by));
    js_set(&jd_J, ev, "which", js_num(1));
    jval tv = jd_el_value(&jd_J, node);
    int prevented = jd_dispatch_to(ev, tv, tv);
    if (trusted || prevented) return prevented;

    /* What a click does, for one a script asked for. */
    for (int at = node; at >= 0; at = jd_doc->nodes[at].parent) {
        if (!jd_is_element(at)) continue;
        int tag = jd_doc->nodes[at].tag;
        if (tag == T_A) {
            const char *href = jd_attr(at, "href");
            char out[URL_TEXT + 256];
            if (href && jd_navigate && jd_resolve(href, out, (int)sizeof(out))) jd_navigate(out, 0);
            break;
        }
        if (tag == T_INPUT) {
            const char *ty = jd_attr(at, "type");
            if (ty && (w_same_fold(ty, "checkbox") || w_same_fold(ty, "radio"))) {
                const char *c = jd_attr(at, "checked");
                int on = !(c && !w_same(c, "0"));
                if (w_same_fold(ty, "radio")) on = 1;
                jd_attr_set(at, "checked", on ? "1" : "0");
                jd_fire_simple(at, "input", 1, 0);
                jd_fire_simple(at, "change", 1, 0);
                break;
            }
        }
        if (tag == T_INPUT || tag == T_BUTTON) {
            const char *ty = jd_attr(at, "type");
            int submits = tag == T_BUTTON ? !(ty && (w_same_fold(ty, "button") || w_same_fold(ty, "reset")))
                                          : ty && (w_same_fold(ty, "submit") || w_same_fold(ty, "image"));
            if (submits) {
                int f = at;
                while (f >= 0 && jd_doc->nodes[f].tag != T_FORM) f = jd_doc->nodes[f].parent;
                if (f >= 0 && !jd_fire_simple(f, "submit", 1, 1) && jd_submit) jd_submit(f);
            }
            break;
        }
    }
    return 0;
}

/* --- later ------------------------------------------------------------------------------------
 *
 * setTimeout is the only way a page can arrange for something to happen that
 * nobody asked for, and a great deal of ordinary behaviour is written with
 * it: a menu that closes itself, a message that goes away, anything that
 * waits a moment before it does the expensive thing. A page that calls it
 * and is never called back is not slow, it is stopped part way through.
 *
 * The clock is the kernel's tick counter, which runs at the rate in
 * kernel/timer.c. Getting that number wrong here would make every timer on
 * every page run at the wrong speed rather than not at all, which is the
 * harder kind of wrong to notice, so it is named rather than assumed.
 *
 * The table grows: a page sets as many as it likes. The browser's own
 * deferred work -- a script a page inserted, fetched on the next pass -- goes
 * in the same table as a native with its argument, so everything that runs
 * later runs in the order it was asked for. */
#define JD_HZ 100                /* kernel/timer.c: frequency */

typedef struct {
    jval  fn;                    /* a function, or text to run */
    jobj *args;                  /* what it is called with, or none */
    void (*native)(jval arg);    /* the browser's own, run with arg */
    jval  arg;
    int   due;                   /* in ticks */
    int   every;                 /* ticks between repeats, or 0 for once */
    int   id;
    u8    used, frame;           /* frame: an animation frame, given the time */
} jtimer;

static jtimer *jd_timer;
static int     jd_ntimer, jd_captimer, jd_timer_id;
static int     jd_tick0;         /* when the page's clock started */

/* performance.now(): milliseconds since the page began, from the same tick
   counter, which is as fine a clock as this machine has -- ten milliseconds
   a tick, and a page is told no finer. */
static double jd_now_ms(void) { return (double)(ticks() - jd_tick0) * (1000.0 / JD_HZ); }

static int jd_ticks_for(double ms) {
    if (!(ms > 0)) ms = 0;
    if (ms > 3600000) ms = 3600000;          /* an hour is long enough */
    int t = (int)((ms * JD_HZ + 999) / 1000);
    return t < 1 ? 1 : t;                    /* never zero: a timer that is
                                                due the instant it is set
                                                would run inside the call
                                                that set it */
}

static jtimer *jd_timer_slot(void) {
    for (int i = 0; i < jd_ntimer; i++) if (!jd_timer[i].used) return &jd_timer[i];
    if (jd_ntimer >= jd_captimer) {
        int cap = jd_captimer ? jd_captimer * 2 : 64;
        if (cap > (1 << 16)) return 0;
        /* In the region, where the collector sees what each will call. */
        jtimer *more = (jtimer *)js_alloc(&jd_J, (u32)cap * (u32)sizeof(jtimer));
        if (!more) return 0;
        volatile u8 *d = (volatile u8 *)more;
        const u8 *s = (const u8 *)jd_timer;
        for (u64 i = 0; i < (u64)jd_ntimer * sizeof(jtimer); i++) d[i] = s[i];
        if (jd_timer) js_free(&jd_J, jd_timer, (u32)jd_captimer * (u32)sizeof(jtimer));
        jd_timer = more;
        jd_captimer = cap;
    }
    jtimer *t = &jd_timer[jd_ntimer++];
    t->used = 0;
    return t;
}

static int jd_later_native(void (*fn)(jval arg), jval arg, int ticks_from_now) {
    jtimer *t = jd_timer_slot();
    if (!t) return 0;
    t->fn = js_undef();
    t->args = 0;
    t->native = fn;
    t->arg = arg;
    t->due = ticks() + (ticks_from_now < 1 ? 1 : ticks_from_now);
    t->every = 0;
    t->id = ++jd_timer_id;
    t->used = 1;
    t->frame = 0;
    return t->id;
}

static jval jd_add_timer(jctx *J, jval *a, int n, int repeat, int frame) {
    jval fn = js_arg(a, n, 0);
    if (!js_callable(fn)) {
        /* Text to run, which the oldest pages still hand setTimeout. */
        if (fn.t != JS_STR) return js_num(0);
    }
    jtimer *t = jd_timer_slot();
    if (!t) return js_num(0);
    int ticks_wait = frame ? jd_ticks_for(16) : jd_ticks_for(n > 1 ? js_to_num(J, a[1]) : 0);
    t->fn = fn;
    t->args = 0;
    if (!frame && n > 2) {
        t->args = js_array(J);
        for (int i = 2; t->args && i < n; i++) js_arr_push(J, t->args, a[i]);
    }
    t->native = 0;
    t->arg = js_undef();
    t->due = ticks() + ticks_wait;
    t->every = repeat ? ticks_wait : 0;
    t->id = ++jd_timer_id;
    t->used = 1;
    t->frame = (u8)frame;
    return js_num(t->id);
}

static jval nat_set_timeout(jctx *J, jval t, jval *a, int n) { (void)t; return jd_add_timer(J, a, n, 0, 0); }
static jval nat_set_interval(jctx *J, jval t, jval *a, int n) { (void)t; return jd_add_timer(J, a, n, 1, 0); }

static jval nat_clear_timer(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (n < 1) return js_undef();
    int id = (int)js_to_num(J, a[0]);
    for (int i = 0; i < jd_ntimer; i++)
        if (jd_timer[i].used && jd_timer[i].id == id && !jd_timer[i].native) jd_timer[i].used = 0;
    return js_undef();
}

/* How long until the page has something to do, in ticks: 0 when something
   is already due, the time to the earliest otherwise, and -1 when it has
   asked for nothing. The browser sleeps that long rather than looking sixty
   times a second. */
static int jd_requests_waiting(void);

static int jsdom_next_due(void) {
    if (!jd_open || jd_spent()) return -1;
    if (jd_requests_waiting()) return 0;
    int now = ticks(), best = -1;
    for (int i = 0; i < jd_ntimer; i++) {
        if (!jd_timer[i].used) continue;
        int left = jd_timer[i].due - now;
        if (left < 0) left = 0;
        if (best < 0 || left < best) best = left;
    }
    return best;
}

static void jd_run_timer(jtimer *t) {
    if (t->native) {
        void (*fn)(jval) = t->native;
        jval arg = t->arg;
        t->used = 0;
        fn(arg);
        /* The browser's own work may settle a promise from outside any call
           into the page, and its reactions are owed now. */
        js_drain(&jd_J);
        return;
    }
    jval fn = t->fn;
    jobj *args = t->args;
    int frame = t->frame;
    if (t->every > 0) t->due = ticks() + t->every;
    else t->used = 0;
    if (fn.t == JS_STR) {
        jd_J.sig = JS_OK;
        if (!js_run(&jd_J, fn.str->s, fn.str->len)) { jd_note_error(); jd_J.sig = JS_OK; }
        return;
    }
    if (frame) {
        jval now = js_num(jd_now_ms());
        jd_call(fn, js_undef(), &now, 1);
    } else {
        jd_call(fn, js_undef(), args ? args->items : 0, args ? (int)args->len : 0);
    }
}

/* Whatever is due, run once, earliest first and in the order they were set
   among those due together. The answer is how many ran, so the browser
   knows whether to ask whether the document changed.
 *
 * A repeating timer is booked forward from now rather than from when it was
 * due, so a page whose interval is shorter than the work in it falls behind
 * rather than accumulating a backlog it can never run down. */
static int jsdom_timers(void) {
    if (!jd_open || jd_spent()) return 0;
    int now = ticks();
    int ran = 0;
    int limit = jd_timer_id;             /* not the ones these set */
    for (;;) {
        int pick = -1;
        for (int i = 0; i < jd_ntimer; i++) {
            jtimer *t = &jd_timer[i];
            if (!t->used || now - t->due < 0 || t->id > limit) continue;
            if (pick < 0 || t->due - jd_timer[pick].due < 0
                || (t->due == jd_timer[pick].due && t->id < jd_timer[pick].id)) pick = i;
        }
        if (pick < 0) break;
        /* A repeat is booked forward before it runs, past `now`, so it is
           not picked again in this pass. */
        jd_run_timer(&jd_timer[pick]);
        ran++;
        if (jd_spent()) break;
    }
    return ran;
}

/* --- document ----------------------------------------------------------------------------------- */

static int jd_by_id(const char *id) {
    for (int i = jd_walk_first(-1); i >= 0; i = jd_walk_next(i, -1)) {
        if (jd_doc->nodes[i].kind != DN_ELEMENT) continue;
        const char *v = dom_attr(jd_doc, i, "id");
        if (v && w_same(v, id)) return i;
    }
    return -1;
}

static jval nat_doc_by_id(jctx *J, jval t, jval *a, int n) {
    if (n < 1) return js_null();
    jstr *s = js_to_str(J, a[0]);
    if (!s) return js_null();
    int root = jd_node_of(t);
    if (root >= 0) {
        /* A fragment's getElementById: what is under it. */
        for (int i = jd_walk_first(root); i >= 0; i = jd_walk_next(i, root)) {
            const char *v = jd_is_element(i) ? dom_attr(jd_doc, i, "id") : 0;
            if (v && w_same(v, s->s)) return jd_el_value(J, i);
        }
        return js_null();
    }
    int el = jd_by_id(s->s);
    return el < 0 ? js_null() : jd_el_value(J, el);
}

static jval nat_doc_by_name(jctx *J, jval t, jval *a, int n) {
    return jd_list_value(J, JL_NAME, 0, jd_inert_of(t), jd_arg_str(J, a, n, 0));
}

static jval nat_doc_create_el(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (n < 1) return js_throw_dom(J, "InvalidCharacterError", "an element needs a name");
    jstr *s = js_to_str(J, a[0]);
    if (!s || !s->len) return js_throw_dom(J, "InvalidCharacterError", "an element needs a name");
    int el = jd_new_element(s);
    if (el < 0) return js_throw_dom(J, "NotSupportedError", "the document is full");
    /* Not dirty yet: an element nothing has been told about is not on the
       page, and laying the document out again for it would be a pass over
       everything to draw exactly what is already there. A defined custom
       element is made through its class. */
    if (jd_ncustom && jd_custom_by_name(dom_tag_name(jd_doc, el))) jd_upgrade(el);
    return jd_el_value(J, el);
}

static jval nat_doc_create_el_ns(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *ns = jd_arg_str(J, a, n, 0);
    jstr *s = jd_arg_str(J, a, n, 1);
    if (!s->len) return js_throw_dom(J, "InvalidCharacterError", "an element needs a name");
    int svg = js_str_is(ns, "http://www.w3.org/2000/svg");
    int el = svg ? dom_create_element(jd_doc, s->s, (int)s->len) : jd_new_element(s);
    if (el < 0) return js_throw_dom(J, "NotSupportedError", "the document is full");
    if (svg && el < DOM_NODES) jd_svg_made[el >> 3] |= (u8)(1 << (el & 7));
    return jd_el_value(J, el);
}

static jval nat_doc_create_text(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = jd_arg_str(J, a, n, 0);
    int node = jd_new_text(s->s, (int)s->len);
    return node < 0 ? js_null() : jd_el_value(J, node);
}

static jval nat_doc_create_comment(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = jd_arg_str(J, a, n, 0);
    int node = jd_new_comment(s->s, (int)s->len);
    return node < 0 ? js_null() : jd_el_value(J, node);
}

static jval nat_doc_create_fragment(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    int node = jd_new_fragment();
    return node < 0 ? js_null() : jd_el_value(J, node);
}

static jval nat_doc_import(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int x = jd_node_of(js_arg(a, n, 0));
    if (x < 0) return js_throw_dom(J, "NotSupportedError", "only a node can be imported");
    int c = jd_clone(x, n > 1 && js_to_bool(a[1]));
    jd_custom_created(c);
    return c < 0 ? js_null() : jd_el_value(J, c);
}

static jval nat_doc_adopt(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t;
    int x = jd_node_of(js_arg(a, n, 0));
    if (x >= 0) jd_remove(x);
    /* A template's contents adopted are the page's document's, and what is
       copied from them is upgraded: FAST makes its views so. */
    if (x >= 0 && x < DOM_NODES) jd_inert_mark[x >> 3] &= (u8)~(1 << (x & 7));
    return js_arg(a, n, 0);
}

static int jd_inert_element(int d);
static int jd_inert_part(int d, int tag);
static jval jd_inert_title(jctx *J, int d);
static void jd_inert_set_title(int d, jstr *s);

static jval nat_doc_element(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int d = jd_inert_of(t);
    if (d >= 0) return jd_node_or_doc(J, jd_inert_element(d));
    return jd_node_or_doc(J, jd_top());
}

/* The head is always there to a script, which appends its styles and
   scripts to it without asking; a page that wrote none gets one. */
static jval nat_doc_head(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int d = jd_inert_of(t);
    if (d >= 0) return jd_node_or_doc(J, jd_inert_part(d, T_HEAD));
    if (jd_doc->head < 0 && jd_top() >= 0) {
        int h = dom_create_element(jd_doc, "head", 4);
        if (h >= 0) {
            dom_insert_before(jd_doc, jd_top(), h, jd_doc->nodes[jd_top()].first);
            jd_doc->head = h;
        }
    }
    return jd_node_or_doc(J, jd_doc->head);
}

static jval nat_doc_body(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int d = jd_inert_of(t);
    if (d >= 0) return jd_node_or_doc(J, jd_inert_part(d, T_BODY));
    return jd_doc->body >= 0 ? jd_el_value(J, jd_doc->body) : js_null();
}

static jval nat_doc_title(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int d = jd_inert_of(t);
    if (d >= 0) return jd_inert_title(J, d);
    return jd_str(jd_doc->title >= 0 ? jd_doc->arena + jd_doc->title : "");
}

static jval nat_doc_set_title(jctx *J, jval t, jval *a, int n) {
    jstr *s = jd_arg_str(J, a, n, 0);
    int d = jd_inert_of(t);
    if (d >= 0) { jd_inert_set_title(d, s); return js_undef(); }
    jd_doc->title = dom_str(jd_doc, s->s, (int)s->len);
    jd_touched();
    return js_undef();
}

static jval nat_doc_url(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int d = jd_inert_of(t);
    if (d >= 0) {
        const char *u = dom_attr(jd_doc, d, "url");
        return jd_str(u ? u : "about:blank");
    }
    return jd_str(jd_address);
}

static jval nat_doc_domain(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    url_t u;
    if (!url_parse(jd_address, &u)) return jd_str("");
    return jd_str(u.host);
}

/* The browser sends no Referer (fetch.h), so there is none to report. */
static jval nat_doc_referrer(jctx *J, jval t, jval *a, int n) { return nat_empty_str(J, t, a, n); }

static jval nat_doc_charset(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_str("UTF-8");
}

static jval nat_doc_content_type(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_str("text/html");
}

static jval nat_doc_compat(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (jd_inert_of(t) >= 0) return jd_str("CSS1Compat");
    return jd_str(jd_doc->standards ? "CSS1Compat" : "BackCompat");
}

static int jd_ready;                     /* 0 loading, 1 interactive, 2 complete */

static jval nat_doc_ready(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (jd_inert_of(t) >= 0) return jd_str("complete");
    return jd_str(jd_ready == 0 ? "loading" : jd_ready == 1 ? "interactive" : "complete");
}

/* The page is on the screen whenever it is loaded: the browser has one page
   and draws it. A document of its own is never drawn. */
static jval nat_doc_visibility(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return jd_str(jd_inert_of(t) >= 0 ? "hidden" : "visible");
}

static jval nat_doc_hidden(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_bool(jd_inert_of(t) >= 0);
}

static jval nat_doc_view(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (jd_inert_of(t) >= 0) return js_null();
    return js_from_obj(J->global_obj);
}

static jval nat_doc_active(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int d = jd_inert_of(t);
    if (d >= 0) return jd_node_or_doc(J, jd_inert_part(d, T_BODY));
    if (jd_active >= 0 && jd_connected(jd_active)) return jd_el_value(J, jd_active);
    return jd_doc->body >= 0 ? jd_el_value(J, jd_doc->body) : js_null();
}

static jval nat_doc_forms(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_list_value(J, JL_FORMS, 1, jd_inert_of(t), 0); }
static jval nat_doc_images(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_list_value(J, JL_IMAGES, 1, jd_inert_of(t), 0); }
static jval nat_doc_links(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_list_value(J, JL_LINKS, 1, jd_inert_of(t), 0); }
static jval nat_doc_scripts(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return jd_list_value(J, JL_SCRIPTS, 1, jd_inert_of(t), 0); }

static int jd_current_script = -1;

static jval nat_doc_current_script(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (jd_inert_of(t) >= 0) return js_null();
    return jd_current_script >= 0 ? jd_el_value(J, jd_current_script) : js_null();
}

/* document.write, while the page is being read: the markup goes in after
   the script that wrote it, and a script in it runs as soon as the writer
   has finished, as a parser would reach it. Afterwards it would replace the
   whole page, which is never what a page that calls it late meant; it is
   left alone. */
#define JD_WRITTEN 64
static int jd_written[JD_WRITTEN];
static int jd_nwritten;

static jval jd_doc_write(jctx *J, jval *a, int n, int newline) {
    if (jd_ready != 0 || jd_current_script < 0) return js_undef();
    jtext tx = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        jstr *s = js_to_str(J, a[i]);
        if (s) jt_put(J, &tx, s->s, s->len);
    }
    if (newline) jd_putc(&tx, '\n');
    int from = jd_doc->count;
    int f = jd_parse_fragment(tx.b ? tx.b : "", (int)tx.n);
    free(tx.b);
    int cs = jd_current_script, p = jd_doc->nodes[cs].parent;
    if (f < 0 || p < 0) return js_undef();
    /* Before whatever followed the script, which puts a second write after
       the first. The scripts in it were marked as never to run by the
       reading (jd_parse_fragment); these are run when the writer is done. */
    for (int i = from; i < jd_doc->count; i++)
        if (jd_doc->nodes[i].kind == DN_ELEMENT && jd_doc->nodes[i].tag == T_SCRIPT
            && jd_nwritten < JD_WRITTEN)
            jd_written[jd_nwritten++] = i;
    jd_insert(p, f, jd_doc->nodes[cs].next);
    return js_undef();
}

/* Into the page only: a document of its own is written to by nobody here,
   and writing its markup after the running script would put it in the
   page instead. */
static jval nat_doc_write(jctx *J, jval t, jval *a, int n) {
    return jd_inert_of(t) >= 0 ? js_undef() : jd_doc_write(J, a, n, 0);
}
static jval nat_doc_writeln(jctx *J, jval t, jval *a, int n) {
    return jd_inert_of(t) >= 0 ? js_undef() : jd_doc_write(J, a, n, 1);
}

/* elementFromPoint: the element the layout drew at a point of the window
   (a word is its element's), the document element where it drew nothing,
   and nothing outside the window; elementsFromPoint, that element and
   everything it is inside. A document of its own is drawn nowhere. */
static int jd_element_at(jctx *J, jval *a, int n) {
    double x = js_to_num(J, js_arg(a, n, 0)), y = js_to_num(J, js_arg(a, n, 1));
    if (!(x >= 0 && y >= 0 && x < jd_view_w && y < jd_view_h)) return -1;
    int el = jd_node_at ? jd_node_at((int)x, (int)y + jd_scroll_y) : -1;
    while (el >= 0 && !jd_is_element(el)) el = jd_doc->nodes[el].parent;
    /* One in a shadow tree is its element, to the document, as a browser
       answers: what is in the tree is the component's own. */
    for (int hops = 0; el >= 0 && !jd_connected(el) && hops < 64; hops++) {
        int top = el;
        while (jd_doc->nodes[top].parent >= 0) top = jd_doc->nodes[top].parent;
        int s = jd_shadow_find(top, 1);
        el = s >= 0 ? jd_shadow_host[s] : -1;
    }
    if (el < 0 || !jd_connected(el) || el == jd_doc->root) el = jd_top();
    return el;
}

static jval nat_doc_element_from_point(jctx *J, jval t, jval *a, int n) {
    if (jd_inert_of(t) >= 0) return js_null();
    int el = jd_element_at(J, a, n);
    return el >= 0 ? jd_el_value(J, el) : js_null();
}

static jval nat_doc_elements_from_point(jctx *J, jval t, jval *a, int n) {
    jobj *out = js_array(J);
    if (!out || jd_inert_of(t) >= 0) return js_from_obj(out);
    for (int el = jd_element_at(J, a, n); el >= 0; el = jd_doc->nodes[el].parent) {
        if (jd_is_element(el)) js_arr_push(J, out, jd_el_value(J, el));
        /* The root the parser keeps above the page's own element is not
           there to a script (jd_top). */
        if (el == jd_top()) break;
    }
    return js_from_obj(out);
}

static jval nat_doc_has_focus(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_bool(jd_inert_of(t) < 0);
}

/* --- documents that are not the page ------------------------------------------------------
 *
 * document.implementation.createHTMLDocument and DOMParser make documents
 * of their own: jQuery makes one to read markup into before it will load
 * at all, and a page reads the parts of a fetched page out of one. Here
 * such a document is a node of the page's tree that is in no part of the
 * page -- an element named "#document", as a fragment is one named
 * "#document-fragment" -- so every search, walk and change works under it
 * as it does under any node, and nothing in it is drawn, run or connected.
 * The Document methods above answer for it (jd_inert_of): its own element,
 * head, body and title, no cookies, no window, and writes that go nowhere. */

static jobj *jd_interface(jctx *J, const char *name, jobj *parent_proto, jnative ctor, int arity);

static int jd_inert_element(int d) {
    for (int c = jd_doc->nodes[d].first; c >= 0; c = jd_doc->nodes[c].next)
        if (jd_is_element(c)) return c;
    return -1;
}

/* The document element's head or body. */
static int jd_inert_part(int d, int tag) {
    int html = jd_inert_element(d);
    if (html < 0) return -1;
    for (int c = jd_doc->nodes[html].first; c >= 0; c = jd_doc->nodes[c].next)
        if (jd_doc->nodes[c].kind == DN_ELEMENT && jd_doc->nodes[c].tag == tag) return c;
    return -1;
}

static int jd_inert_title_el(int d) {
    for (int i = jd_walk_first(d); i >= 0; i = jd_walk_next(i, d))
        if (jd_doc->nodes[i].kind == DN_ELEMENT && jd_doc->nodes[i].tag == T_TITLE) return i;
    return -1;
}

/* The first title's text, with its white space run together and trimmed,
   as the standard reads a title. */
static jval jd_inert_title(jctx *J, int d) {
    int ti = jd_inert_title_el(d);
    if (ti < 0) return jd_str("");
    jtext raw = { 0, 0, 0, 0 };
    jd_text_content(ti, &raw);
    jtext out = { 0, 0, 0, 0 };
    int gap = 0;
    for (u32 i = 0; i < raw.n; i++) {
        char c = raw.b[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') { gap = out.n > 0; continue; }
        if (gap) jd_putc(&out, ' ');
        gap = 0;
        jd_putc(&out, c);
    }
    free(raw.b);
    return js_from_str(jt_done(J, &out));
}

static void jd_inert_set_title(int d, jstr *s) {
    int ti = jd_inert_title_el(d);
    if (ti < 0) {
        int head = jd_inert_part(d, T_HEAD);
        if (head < 0) return;
        ti = dom_create_element(jd_doc, "title", 5);
        if (ti < 0) return;
        jd_insert(head, ti, -1);
    }
    jd_replace_with_text(ti, s);
}

/* A document of its own: html, with a head and a body, under a node that
   stands for the document. The address, when it has one, is kept on that
   node, where nothing reads it but document.URL. */
static int jd_new_document(const char *url, int with_parts) {
    int d = jd_new_named("#document");
    if (d < 0) return -1;
    if (url) dom_attr_set(jd_doc, d, "url", url);
    if (!with_parts) return d;
    int html = dom_create_element(jd_doc, "html", 4);
    int head = dom_create_element(jd_doc, "head", 4);
    int body = dom_create_element(jd_doc, "body", 4);
    if (html < 0 || head < 0 || body < 0) return -1;
    dom_append(jd_doc, d, html);
    dom_append(jd_doc, html, head);
    dom_append(jd_doc, html, body);
    return d;
}

static jval nat_impl_create_html(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int d = jd_new_document(0, 1);
    if (d < 0) return js_throw_dom(J, "NotSupportedError", "the document is full");
    if (n > 0 && a[0].t != JS_UNDEF) {
        jstr *s = js_to_str(J, a[0]);
        int head = jd_inert_part(d, T_HEAD);
        int ti = dom_create_element(jd_doc, "title", 5);
        int tx = s ? jd_new_text(s->s, (int)s->len) : -1;
        if (head >= 0 && ti >= 0 && tx >= 0) {
            dom_append(jd_doc, ti, tx);
            dom_append(jd_doc, head, ti);
        }
    }
    return jd_el_value(J, d);
}

/* The standard now says this is true whatever is asked. */
static jval nat_impl_has_feature(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_bool(1);
}

/* What goes in a head when a page's markup came without one: the parts a
   head holds, up to the first thing that is content. */
static int jd_head_part(int c) {
    if (jd_doc->nodes[c].kind != DN_ELEMENT) return 0;
    int tag = jd_doc->nodes[c].tag;
    if (tag == T_TITLE || tag == T_META || tag == T_LINK || tag == T_STYLE || tag == T_SCRIPT) return 1;
    return tag == T_OTHER && w_same(dom_tag_name(jd_doc, c), "base");
}

static int jd_blank_text(int c) {
    if (jd_doc->nodes[c].kind != DN_TEXT) return 0;
    for (const char *p = jd_text_of(c); *p; p++)
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') return 0;
    return 1;
}

static jval nat_parser_ctor(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "DOMParser is made with new", J->error_line);
    return js_undef();
}

/* HTML read the way the page was, into a document of its own with the
   page's address: the markup's html, head and body where it wrote them,
   and made where it did not -- a head's parts before any content go in the
   head, and the rest in the body, in the order written. The scripts in it
   never run (jd_parse_fragment). XML is not read: this browser has no XML
   parser, and reading it as HTML would change what it says. */
static jval nat_parser_parse(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *src = jd_arg_str(J, a, n, 0), *type = jd_arg_str(J, a, n, 1);
    if (!js_str_is(type, "text/html")) {
        if (js_str_is(type, "text/xml") || js_str_is(type, "application/xml")
            || js_str_is(type, "application/xhtml+xml") || js_str_is(type, "image/svg+xml"))
            return js_throw_dom(J, "NotSupportedError", "this browser reads HTML, and has no XML parser");
        return js_throw(J, JS_ERR_TYPE, "parseFromString needs text/html or an XML type", J->error_line);
    }
    int d = jd_new_document(jd_address, 0);
    int f = d >= 0 ? jd_parse_fragment(src->s, (int)src->len) : -1;
    if (f < 0) return js_throw_dom(J, "NotSupportedError", "the document is full");

    int html = -1;
    for (int c = jd_doc->nodes[f].first; c >= 0 && html < 0; c = jd_doc->nodes[c].next)
        if (jd_doc->nodes[c].kind == DN_ELEMENT && jd_doc->nodes[c].tag == T_HTML) html = c;
    if (html >= 0) dom_unlink(jd_doc, html);
    else if ((html = dom_create_element(jd_doc, "html", 4)) < 0) return js_null();
    dom_append(jd_doc, d, html);
    /* Anything written outside <html> goes inside it, in order. */
    while (jd_doc->nodes[f].first >= 0) {
        int c = jd_doc->nodes[f].first;
        dom_unlink(jd_doc, c);
        dom_append(jd_doc, html, c);
    }
    int head = -1, body = -1;
    for (int c = jd_doc->nodes[html].first; c >= 0; c = jd_doc->nodes[c].next) {
        if (jd_doc->nodes[c].kind != DN_ELEMENT) continue;
        if (jd_doc->nodes[c].tag == T_HEAD && head < 0) head = c;
        if (jd_doc->nodes[c].tag == T_BODY && body < 0) body = c;
    }
    if (head < 0) {
        if ((head = dom_create_element(jd_doc, "head", 4)) < 0) return js_null();
        dom_insert_before(jd_doc, html, head, jd_doc->nodes[html].first);
    }
    if (body < 0) {
        if ((body = dom_create_element(jd_doc, "body", 4)) < 0) return js_null();
        dom_append(jd_doc, html, body);
    }
    /* What is left beside them: before the body, into the start of it (or
       the head, while it is a head's part); after it, onto its end. */
    int before = jd_doc->nodes[body].first, past_body = 0, content = 0;
    for (int c = jd_doc->nodes[html].first, next; c >= 0; c = next) {
        next = jd_doc->nodes[c].next;
        if (c == head) continue;
        if (c == body) { past_body = 1; continue; }
        dom_unlink(jd_doc, c);
        if (!content && !past_body && jd_blank_text(c)) continue;
        if (!content && !past_body && jd_head_part(c)) { dom_append(jd_doc, head, c); continue; }
        content = 1;
        if (!past_body && before >= 0) dom_insert_before(jd_doc, body, c, before);
        else dom_append(jd_doc, body, c);
    }
    return jd_el_value(J, d);
}

static jobj *jd_implementation;

static jval nat_doc_implementation(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_implementation ? js_from_obj(jd_implementation) : js_null();
}

static jval nat_impl_create_doc(jctx *J, jval t, jval *a, int n);
static jval nat_impl_create_doctype(jctx *J, jval t, jval *a, int n);

static void jd_setup_documents(jctx *J, jobj *docproto) {
    jobj *ip = jd_interface(J, "DOMImplementation", 0, 0, 0);
    jd_method(J, ip, "createHTMLDocument", nat_impl_create_html, 0);
    jd_method(J, ip, "createDocument", nat_impl_create_doc, 2);
    jd_method(J, ip, "createDocumentType", nat_impl_create_doctype, 3);
    jd_method(J, ip, "hasFeature", nat_impl_has_feature, 0);
    jd_implementation = js_object_with(J, JO_PLAIN, ip);
    jd_accessor(J, docproto, "implementation", nat_doc_implementation, 0);
    jobj *pp = jd_interface(J, "DOMParser", 0, nat_parser_ctor, 0);
    jd_method(J, pp, "parseFromString", nat_parser_parse, 2);
}


/* --- running scripts ---------------------------------------------------------------------------
 *
 * One context for the whole document rather than one per script, because a
 * function declared in the first script and called from the second is
 * ordinary and would otherwise not be found. */

/* How a script with a src gets its text. Set by the browser, because
   fetching it needs the network and the address of the page it is relative
   to, and neither belongs down here. The text it hands back stays valid
   until the next call, which is all it has to be -- it is run
   immediately. */
static int (*jd_get_script)(const char *src, const char **out);

/* Whether a script element is a script. A page carries data in them too --
   application/ld+json describing the article, text/template, importmap --
   and every one was run: the data did not parse, and its error became the
   only thing the page said. Nothing, or one of the names JavaScript goes by,
   runs as a classic script; a module runs as one (jsmod.h, jd_module_wanted). */
static int jd_script_type_runs(const char *type) {
    if (!type) return 1;
    while (*type == ' ') type++;
    if (!*type) return 1;
    static const char *const RUN[] = { "text/javascript", "application/javascript",
                                       "text/ecmascript", "application/ecmascript",
                                       "application/x-javascript", "text/jscript", 0 };
    for (int i = 0; RUN[i]; i++) {
        const char *r = RUN[i];
        int k = 0;
        while (r[k] && w_lower(type[k]) == r[k]) k++;
        /* The name, and then nothing, or a parameter after a semicolon. */
        if (!r[k] && (!type[k] || type[k] == ';' || type[k] == ' ')) return 1;
    }
    return 0;
}

void jsdom_fetch_with(int (*fn)(const char *, const char **)) {
    jd_get_script = fn;
}

/* How many external ones were fetched and how many were not, for the line
   the browser prints about what a page did. */
static int jd_outside, jd_outside_failed;

__attribute__((unused)) static int jsdom_outside(void) { return jd_outside; }
__attribute__((unused)) static int jsdom_outside_failed(void) { return jd_outside_failed; }

/* Told after each script has run, or failed to arrive (len 0), with what
   stopped it or nothing: for tools/host/scriptdump.c, which says which of a
   page's scripts stopped where. Nothing in the browser listens. */
static void (*jd_script_done)(int index, const char *src, u32 len, const char *err);

/* Whether a script element should run at all: a script, of a kind that
   runs, in the page, and not inside a template, whose markup is not the
   page's until a script stamps it out. */
static int jd_script_wanted(int i) {
    if (jd_doc->nodes[i].kind != DN_ELEMENT || jd_doc->nodes[i].tag != T_SCRIPT) return 0;
    if (jd_is_started(i)) return 0;
    if (!jd_script_type_runs(dom_attr(jd_doc, i, "type"))) return 0;
    /* What a page gives a browser without modules; this one has them. */
    if (dom_attr(jd_doc, i, "nomodule")) return 0;
    if (!jd_connected(i)) return 0;
    if (jd_has_ancestor_tag(i, T_OTHER, "template")) return 0;
    return 1;
}

/* One script's text, run. 1 when it finished. */
static int jd_run_text(int node, const char *src, const char *text, u32 len) {
    int was = jd_current_script;
    jd_current_script = node;
    jsignal outer = jd_J.sig;
    jval outer_ret = jd_J.ret;
    jd_J.sig = JS_OK;
    int good = js_run(&jd_J, text, len);
    if (!good) jd_note_error();
    if (jd_script_done) jd_script_done(node, src, len, good ? "" : jd_J.error);
    jd_J.sig = outer;
    jd_J.ret = outer_ret;
    jd_current_script = was;
    return good;
}

/* --- data: addresses ------------------------------------------------------------------------
 *
 * An address that carries what it names: data:[type][;base64],text, read
 * by the fetch standard's steps -- the text's percent escapes undone, then
 * base64 read (forgivingly, as atob reads it) when the type ends in
 * ";base64". Nothing goes to the network. Instagram writes nearly every
 * script it has as one, and a page may fetch one. */

static int jd_is_data_url(const char *s) {
    return s && w_lower(s[0]) == 'd' && w_lower(s[1]) == 'a' && w_lower(s[2]) == 't'
           && w_lower(s[3]) == 'a' && s[4] == ':';
}

static int jd_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The bytes, in memory of their own that the caller frees (*out), and how
   many; the type into mime when asked for. 0 with *out left null when the
   address has no comma or its base64 is not base64. */
static u32 jd_data_url(const char *src, char **out, char *mime, int mcap) {
    *out = 0;
    const char *p = src + 5, *comma = p;
    while (*comma && *comma != ',') comma++;
    if (!*comma) return 0;
    const char *te = comma;
    while (te > p && (te[-1] == ' ' || te[-1] == '\t')) te--;
    int b64 = 0;
    if (te - p >= 7) {
        const char *q = te - 7, *want = ";base64";
        b64 = 1;
        for (int k = 0; k < 7; k++) if (w_lower(q[k]) != want[k]) b64 = 0;
        if (b64) te = q;
    }
    if (mime && mcap > 0) {
        while (p < te && *p == ' ') p++;
        int m = 0;
        if (p < te && *p != ';') {
            for (const char *q = p; q < te && m < mcap - 1; q++) mime[m++] = *q;
        } else {
            const char *dflt = "text/plain;charset=US-ASCII";
            for (; dflt[m] && m < mcap - 1; m++) mime[m] = dflt[m];
        }
        mime[m] = 0;
    }
    const char *body = comma + 1;
    u32 len = (u32)w_len(body);
    char *buf = (char *)malloc(len + 1);
    if (!buf) return 0;
    u32 n = 0;
    for (u32 i = 0; i < len; i++) {
        int h1, h2;
        if (body[i] == '%' && i + 2 < len && (h1 = jd_hex_digit(body[i + 1])) >= 0
            && (h2 = jd_hex_digit(body[i + 2])) >= 0) {
            buf[n++] = (char)(h1 * 16 + h2);
            i += 2;
        } else {
            buf[n++] = body[i];
        }
    }
    if (b64) {
        u32 m = 0;
        for (u32 i = 0; i < n; i++) {
            char c = buf[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 12) continue;
            buf[m++] = c;
        }
        if (m % 4 == 0 && m && buf[m - 1] == '=') { m--; if (buf[m - 1] == '=') m--; }
        int ok = m % 4 != 1;
        for (u32 i = 0; ok && i < m; i++) if (tx_b64((u8)buf[i]) < 0) ok = 0;
        if (!ok) { free(buf); return 0; }
        u32 bits = 0, have = 0, w = 0;
        for (u32 i = 0; i < m; i++) {
            bits = (bits << 6) | (u32)tx_b64((u8)buf[i]);
            have += 6;
            if (have >= 8) { have -= 8; buf[w++] = (char)(bits >> have); }
        }
        n = w;
    }
    buf[n] = 0;
    *out = buf;
    return n;
}

static int jd_is_blob_url(const char *s);                          /* jswin.h */
static int jd_blob_lookup(const char *url, jstr **bytes, jstr **type);

/* A script element's text, from its src or from inside it. 0 for none. An
   inline one is gathered into memory of its own, which *owned says the
   caller frees once it has run: the text of a script inserted while another
   runs must not be the other's. */
static u32 jd_script_text(int i, const char **text, const char **src, char **owned) {
    *text = 0;
    *owned = 0;
    *src = dom_attr(jd_doc, i, "src");
    if (*src && **src) {
        /* A script element with a src ignores anything written inside it,
           which is what every browser does. */
        if (jd_is_data_url(*src)) {
            char *buf;
            u32 n = jd_data_url(*src, &buf, 0, 0);
            if (!buf) {
                jd_outside_failed++;
                if (jd_script_done) jd_script_done(i, *src, 0, "its data: address does not decode");
                return 0;
            }
            jd_outside++;
            *owned = buf;
            *text = buf;
            return n;
        }
        if (jd_is_blob_url(*src)) {
            jstr *bb, *bt;
            if (!jd_blob_lookup(*src, &bb, &bt)) {
                jd_outside_failed++;
                if (jd_script_done) jd_script_done(i, *src, 0, "its blob: address names nothing");
                return 0;
            }
            jd_outside++;
            *text = bb->s;
            return bb->len;
        }
        if (!jd_get_script) { jd_outside_failed++; return 0; }
        int n = jd_get_script(*src, text);
        if (n <= 0 || !*text) {
            jd_outside_failed++;
            if (jd_script_done) jd_script_done(i, *src, 0, "it would not come");
            return 0;
        }
        jd_outside++;
        return (u32)n;
    }
    *src = 0;
    jtext tx = { 0, 0, 0, 0 };
    jd_text_content(i, &tx);
    *owned = tx.b;
    *text = tx.b ? tx.b : "";
    return tx.n;
}

/* Whether the machine has the memory to run a script this long, from what
 * the browser says is free (jsdom_memory_with; -1 when the kernel does not
 * say, as on the host build). A machine without the memory ends the browser
 * for want of a page rather than saying no, so this says no first.
 *
 * What running one costs, measured: the engine's tree of it (jsparse.h,
 * js_node) is about 0.4 nodes of 32 bytes for each byte of a bundle, and it
 * grows by doubling, so the doubling that makes room holds a new array the
 * size of the whole tree while the old one is still there; what the script
 * makes when it runs was about twice its size again; and the page still has
 * to be laid out after it. A 64 megabyte machine has about nine and a half
 * megabytes free with the browser up: a 256 kilobyte bundle ran there and a
 * 512 kilobyte one ended the browser. */
#define JD_ROOM_RESERVE (4 * 1024 * 1024)

static long long (*jd_free_memory)(void);

void jsdom_memory_with(long long (*fn)(void)) { jd_free_memory = fn; }

/* How much a page's scripts may have, from what the machine has free as the
 * page opens: a quarter of it, never less than the 24 megabytes every machine
 * gets and never more than 128.
 *
 * With nothing ever given back until the page is left, 24 was where the
 * scripts of big sites stopped -- The Verge, Yahoo, Microsoft, Instagram and
 * Ars Technica all ended on "more memory than a page is allowed" -- on a
 * machine with hundreds of megabytes doing nothing. A quarter leaves the
 * layout, the pictures and every other program the rest. */
#define JD_MEM_CAP_MAX (128u * 1024 * 1024)

static u32 jd_mem_cap(void) {
    long long free_b = jd_free_memory ? jd_free_memory() : -1;
    if (free_b < 0) return JS_MEM_CAP;
    long long c = free_b / 4;
    if (c < (long long)JS_MEM_CAP) c = JS_MEM_CAP;
    if (c > (long long)JD_MEM_CAP_MAX) c = JD_MEM_CAP_MAX;
    return (u32)c;
}

static int jd_room_for(u32 len) {
    long long free_b = jd_free_memory ? jd_free_memory() : -1;
    if (free_b < 0) return 1;
    long long nodes = (long long)jd_J.nnodes + (long long)len * 2 / 5 + 1024;
    long long cap = jd_J.ncap ? jd_J.ncap : 1024, peak = 0;
    while (cap < nodes) { cap *= 2; peak = cap * (long long)sizeof(jnode); }
    return peak + (long long)len * 2 + JD_ROOM_RESERVE <= free_b;
}

/* A message of the browser's own on the page's error line, as the first
   thing that went wrong when nothing went wrong before it. */
static void jd_note_text(const char *msg) {
    if (jd_on_error) jd_on_error(msg, 0);
    if (jd_err[0]) return;
    w_copy(jd_err, (int)sizeof(jd_err), msg, (int)sizeof(jd_err));
}

/* A script element run, from wherever its text comes. 0 when there was no
   text to run. */
static int jd_run_script_el(int i) {
    const char *text, *src;
    char *owned;
    u32 len = jd_script_text(i, &text, &src, &owned);
    if (len && !jd_room_for(len)) {
        jd_note_text("a script too large for this machine's memory was not run");
        if (jd_script_done) jd_script_done(i, src, 0, "too large for this machine's memory");
        len = 0;
    }
    if (len) jd_run_text(i, src, text, len);
    if (owned) free(owned);
    return len != 0;
}

/* Scripts document.write put in, run in the order they were written, as
   soon as the script that wrote them has finished. */
static void jd_run_written(void) {
    for (int k = 0; k < jd_nwritten; k++) {
        int i = jd_written[k];
        if (!jd_connected(i) || !jd_script_type_runs(dom_attr(jd_doc, i, "type"))) continue;
        jd_run_script_el(i);
    }
    jd_nwritten = 0;
}

/* A script a page put in the page itself, from a src: fetched and run on
   the next pass, which is when a browser would have it, then told so with
   load, or error when it would not come. */
static void jd_inserted_script_due(jval arg) {
    int i = jd_node_of(arg);
    if (i < 0) return;
    int ran = jd_run_script_el(i);
    jd_fire_simple(i, ran ? "load" : "error", 0, 0);
}

static void jd_scripts_inserted(int top) {
    for (int i = top; i >= 0; i = jd_walk_next(i, top)) {
        if (jd_module_wanted(i)) {
            const char *msrc = dom_attr(jd_doc, i, "src");
            if (!(msrc && *msrc) && jd_doc->nodes[i].first < 0) continue;
            jd_mark_started(i);
            jd_later_native(jd_inserted_module_due, jd_el_value(&jd_J, i), 1);
            continue;
        }
        if (!jd_script_wanted(i)) continue;
        /* One with no src and nothing in it yet waits: a page that makes a
           script, appends it and then sets its text runs it when it has
           some, which is when setting the text on a connected one does. */
        const char *src = dom_attr(jd_doc, i, "src");
        if (!(src && *src) && jd_doc->nodes[i].first < 0) continue;
        jd_mark_started(i);
        if (src && *src) jd_later_native(jd_inserted_script_due, jd_el_value(&jd_J, i), 1);
        else jd_run_script_el(i);
    }
}

/* Returns how many scripts ran. `err` gets the first failure, because a page
   with a broken script should say so somewhere rather than silently doing
   nothing -- and the second failure is usually the first one's fault. */
/* When a script the parser met runs, as a browser that has all of the page at
   once runs it: now, where it stands (JR_NOW); after the document is read, in
   the order written (JR_AFTER: defer on a script with a src, and every module
   that is not async); or when it arrives (JR_ARRIVES: async), for which after
   the document is a time it could have come. defer and async mean nothing to
   a classic script without a src. */
enum { JR_NOW, JR_AFTER, JR_ARRIVES };

static int jd_script_when(int i, int module) {
    int async = dom_attr(jd_doc, i, "async") != 0;
    if (module) return async ? JR_ARRIVES : JR_AFTER;
    if (!dom_attr(jd_doc, i, "src")) return JR_NOW;
    if (async) return JR_ARRIVES;
    return dom_attr(jd_doc, i, "defer") ? JR_AFTER : JR_NOW;
}

static void jd_ready_change(int to);

static int jsdom_scripts(char *err, int errcap) {
    if (err && errcap) err[0] = 0;
    if (!jd_open || !jd_doc) return 0;
    int ran = 0;
    jd_outside = jd_outside_failed = 0;
    jd_ready = 0;

    /* Again here, where the browser has handed over everything it lends:
       it says what is free after opening the page, and the first page used
       to keep the 24 megabytes of a machine that had not said. */
    u32 cap = jd_mem_cap();
    if (cap > jd_J.mem_cap) jd_J.mem_cap = cap;
    jd_importmap_read();

    /* The page's own, each in its turn (jd_script_when), and in each turn in
       the order they were written -- which for what the parser made is the
       order of the numbers. A file that defines something and an inline
       script below it that uses it is the commonest shape on the web. So is
       the other way round with defer: every script ran in the order written,
       and NHS's deferred main.js ran before the inline script below it that
       sets up the settings it reads. Between the parser's turn and the rest,
       the document is read: it is interactive, and a script run after that
       cannot write into it, as in a browser. */
    for (int turn = JR_NOW; turn <= JR_ARRIVES && !jd_spent(); turn++) {
        if (turn == JR_AFTER) {
            jd_nav_mark(NV_INTERACTIVE);
            jd_ready_change(1);
        }
        for (int i = 0; i < jd_parsed && i < jd_doc->count && !jd_spent(); i++) {
            int module = jd_module_wanted(i);
            if (!module && !jd_script_wanted(i)) continue;
            if (jd_script_when(i, module) != turn) continue;
            jd_mark_started(i);
            if (module) {
                if (jd_run_module_el(i)) ran++;
                continue;
            }
            if (!jd_run_script_el(i)) continue;
            ran++;
            jd_run_written();
        }
    }

    if (err && errcap) {
        int w = 0;
        for (const char *p = jd_err; *p && w < errcap - 1; p++) err[w++] = *p;
        err[w] = 0;
    }
    return ran;
}

/* The two a page waits on before it will do anything, and the ready state
   moving with them: DOMContentLoaded to the document, which bubbles to the
   window, then load to the window. */
static void jd_ready_change(int to) {
    jd_ready = to;
    if (!jd_document_obj) return;
    jobj *ev = jd_new_event(0, "readystatechange", 0, 0);
    if (!ev) return;
    js_set(&jd_J, ev, "isTrusted", js_bool(1));
    jd_dispatch_to(ev, js_from_obj(jd_document_obj), js_from_obj(jd_document_obj));
}

static void jsdom_loaded(void) {
    if (!jd_open) return;
    if (jd_ready < 1) {
        jd_nav_mark(NV_INTERACTIVE);
        jd_ready_change(1);
    }
    jobj *ev = jd_new_event(0, "DOMContentLoaded", 1, 0);
    jd_nav_mark(NV_DCL_START);
    if (ev && jd_document_obj) {
        js_set(&jd_J, ev, "isTrusted", js_bool(1));
        jd_dispatch_to(ev, js_from_obj(jd_document_obj), js_from_obj(jd_document_obj));
    }
    jd_nav_mark(NV_DCL_END);
    jd_nav_mark(NV_COMPLETE);
    jd_ready_change(2);
    ev = jd_new_event(0, "load", 0, 0);
    jd_nav_mark(NV_LOAD_START);
    if (ev && jd_J.global_obj) {
        js_set(&jd_J, ev, "isTrusted", js_bool(1));
        jd_dispatch_to(ev, js_from_obj(jd_J.global_obj),
                       jd_document_obj ? js_from_obj(jd_document_obj) : js_null());
    }
    jd_nav_mark(NV_LOAD_END);
    ev = jd_new_event(jd_evkind("PageTransitionEvent"), "pageshow", 0, 0);
    if (ev && jd_J.global_obj) {
        js_set(&jd_J, ev, "isTrusted", js_bool(1));
        jd_dispatch_to(ev, js_from_obj(jd_J.global_obj),
                       jd_document_obj ? js_from_obj(jd_document_obj) : js_null());
    }
}

/* A click on an element, where in the page's view it landed, and whether
   the page asked for the ordinary consequence -- following a link -- not to
   follow. */
__attribute__((unused)) static int jsdom_click_at(int node, int x, int y) { return jd_fire_click(node, 1, x, y); }
__attribute__((unused)) static int jsdom_click(int node) { return jd_fire_click(node, 1, 0, 0); }

/* What the reader did to a form, told to the page as it happens: typing
   (input), ticking a box (input and change), and sending it (submit, which
   the page may cancel to send it its own way -- the browser does not send
   it then). */
static int jd_fire_kind(int node, int kind, const char *type, int bubbles, int cancelable) {
    if (!jd_open || node < 0 || jd_spent()) return 0;
    jobj *ev = jd_new_event(kind, type, bubbles, cancelable);
    if (!ev) return 0;
    js_set(&jd_J, ev, "isTrusted", js_bool(1));
    jval tv = jd_el_value(&jd_J, node);
    return jd_dispatch_to(ev, tv, tv);
}

__attribute__((unused)) static int jsdom_submitting(int form) {
    return form >= 0 ? jd_fire_kind(form, jd_evkind("SubmitEvent"), "submit", 1, 1) : 0;
}

__attribute__((unused)) static void jsdom_typed(int node) {
    jd_fire_kind(node, jd_evkind("InputEvent"), "input", 1, 0);
}

__attribute__((unused)) static void jsdom_toggled(int node) {
    jd_fire_kind(node, jd_evkind("InputEvent"), "input", 1, 0);
    jd_fire_kind(node, 0, "change", 1, 0);
}

static int jsdom_live(void) { return jd_open; }

/* Whether anything has changed the document since this was last asked, which
   is the question the browser asks before deciding to lay the page out
   again. Asking clears it. */
static int jsdom_changed(void) {
    int v = jd_dirty;
    jd_dirty = 0;
    return v;
}

static const char *jsdom_error(void) { return jd_err; }

/* The next element whose style attribute a script changed since the browser
   last asked, or -1: it reads each again before laying the page out. */
__attribute__((unused)) static int jsdom_next_restyled(void) {
    if (!jd_restyle_any || !jd_doc) return -1;
    int top = jd_doc->count < DOM_NODES ? jd_doc->count : DOM_NODES;
    for (int i = 0; i < top; i++)
        if (jd_restyle[i >> 3] & (1 << (i & 7))) {
            jd_restyle[i >> 3] &= (u8)~(1 << (i & 7));
            return i;
        }
    jd_restyle_any = 0;
    return -1;
}

void jsdom_boxes_with(int (*fn)(int, int *, int *, int *, int *)) { jd_box_of = fn; }
void jsdom_points_with(int (*fn)(int x, int y)) { jd_node_at = fn; }
void jsdom_pictures_with(int (*fn)(int, int *, int *)) { jd_picture_size = fn; }
void jsdom_scroll_with(void (*fn)(int)) { jd_scroll_to = fn; }
void jsdom_navigate_with(void (*fn)(const char *, int)) { jd_navigate = fn; }
void jsdom_submit_with(void (*fn)(int)) { jd_submit = fn; }

/* The window as it is now: how wide and tall the page's view is and how far
   it is scrolled. Told before anything runs that might ask; a new width
   asks the page's media query lists again (jswin.h). */
static void jd_mql_recheck(void);

__attribute__((unused)) static void jsdom_view(int w, int h, int scroll) {
    int wider = jd_view_w != w;
    jd_view_w = w;
    jd_view_h = h;
    jd_scroll_y = scroll;
    if (wider && jd_open) jd_mql_recheck();
}

/* Where the page is, before it is opened. */
__attribute__((unused)) static void jsdom_at(const char *address) {
    w_copy(jd_address, (int)sizeof(jd_address), address ? address : "", (int)sizeof(jd_address));
}

static jobj *jd_interface(jctx *J, const char *name, jobj *parent_proto, jnative ctor, int arity);
static jobj *jd_ctor_of(jobj *proto);
static void jd_consts(jctx *J, jobj *on, const char *const *names, int from);

#include "jsurl.h"
#include "jsnet.h"
#include "jswin.h"
#include "jsobs.h"
#include "jsmatrix.h"
#include "jsform.h"
#include "jscssom.h"
#include "jswalk.h"
#include "jsmod.h"
#include "jsworker.h"

/* --- the hooks -----------------------------------------------------------------------------------
 *
 * Asked before an object's own properties (js.h), for the objects whose
 * names nobody can list in advance. A node answers for nothing here -- its
 * properties are accessors on its prototypes -- except a form, whose
 * controls are properties of it by their names, as they are in every
 * browser: form.q.value is how pages written before getElementById read a
 * search box. */
static int jd_form_get(jctx *J, int form, const char *name, jval *out) {
    if (!name[0]) return 0;
    if (name[0] >= '0' && name[0] <= '9') {
        int want = 0;
        for (const char *p = name; *p; p++) {
            if (*p < '0' || *p > '9' || want > 100000) return 0;
            want = want * 10 + (*p - '0');
        }
        int k = 0;
        for (int i = jd_walk_first(form); i >= 0; i = jd_walk_next(i, form))
            if (jd_is_element(i) && jd_is_form_control(i) && k++ == want) { *out = jd_el_value(J, i); return 1; }
        *out = js_undef();
        return 1;
    }
    for (int i = jd_walk_first(form); i >= 0; i = jd_walk_next(i, form)) {
        if (!jd_is_element(i) || !jd_is_form_control(i)) continue;
        const char *id = dom_attr(jd_doc, i, "id"), *nm = dom_attr(jd_doc, i, "name");
        if ((nm && w_same(nm, name)) || (id && w_same(id, name))) { *out = jd_el_value(J, i); return 1; }
    }
    return 0;
}

static jval nat_option_set_selected(jctx *J, jval t, jval *a, int n) {
    (void)J;
    int x = jd_el_of(t);
    if (x < 0) return js_undef();
    if (js_to_bool(js_arg(a, n, 0))) jd_attr_set(x, "selected", "");
    else jd_attr_remove(x, "selected");
    return js_undef();
}

static jval nat_table_rows(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    return x < 0 ? js_undef() : jd_list_value(J, JL_ROWS, 1, x, 0);
}

static jval nat_row_cells(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    return x < 0 ? js_undef() : jd_list_value(J, JL_CELLS, 1, x, 0);
}

static int jd_host_get(jctx *J, jobj *o, const char *name, jval *out) {
    if (!jd_doc) return 0;
    int h = o->host;
    if (h < JD_DOCUMENT) {
        if (h < jd_doc->count && jd_doc->nodes[h].kind == DN_ELEMENT && jd_doc->nodes[h].tag == T_FORM)
            return jd_form_get(J, h, name, out);
        return 0;
    }
    if (h < JD_CLASSLIST) return 0;
    if (h < JD_STYLE) return jd_token_get(J, o, name, out);
    if (h < JD_DATASET) return jd_style_get(J, o, name, out);
    if (h < JD_ATTRS) return jd_dataset_get(J, o, name, out);
    if (h < JD_LIST) return jd_attrs_get(J, o, name, out);
    if (h < JD_STORAGE) return jd_list_get(J, o, name, out);
    if (h == JD_COMPUTED) return jd_computed_host(J, o, name, out);
    if (h == JD_STORAGE || h == JD_STORAGE + 1) return jd_storage_get(J, o, name, out);
    if (h >= JD_RSTYLE && h < JD_RLIST) return jd_style_get(J, o, name, out);
    if (h >= JD_RLIST) return jcs_list_get(J, o, name, out);
    return 0;
}

static int jd_host_set(jctx *J, jobj *o, const char *name, jval v) {
    if (!jd_doc) return 0;
    int h = o->host;
    if (h < JD_STYLE) return 0;
    if (h < JD_DATASET) return jd_style_put(J, o, name, v);
    if (h < JD_ATTRS) return jd_dataset_put(J, o, name, v);
    if (h == JD_STORAGE || h == JD_STORAGE + 1) return jd_storage_put(J, o, name, v);
    if (h >= JD_RSTYLE && h < JD_RLIST) return jd_style_put(J, o, name, v);
    return 0;
}

/* --- building the interfaces --------------------------------------------------------------------- */

/* An interface: its prototype, chained to its parent's, and its
   constructor on the window, whose own prototype chain follows the same
   line so a class extending it inherits its statics as well. */
static jobj *jd_interface(jctx *J, const char *name, jobj *parent_proto, jnative ctor, int arity) {
    jobj *p = js_object_with(J, JO_PLAIN, parent_proto ? parent_proto : J->p_object);
    if (!p) return 0;
    jobj *c = js_ctor(J, name, ctor ? ctor : nat_illegal_ctor, arity, p);
    if (c && parent_proto) {
        jprop *pc = js_find(parent_proto, J->s_constructor);
        if (pc && pc->v.t == JS_OBJ) c->proto = pc->v.obj;
    }
    js_tag(J, p, name);
    return p;
}

static jobj *jd_ctor_of(jobj *proto) {
    jprop *pc = proto ? js_find(proto, jd_J.s_constructor) : 0;
    return pc && pc->v.t == JS_OBJ ? pc->v.obj : 0;
}

static void jd_consts(jctx *J, jobj *on, const char *const *names, int from) {
    for (int i = 0; names[i]; i++) js_const_prop(J, on, names[i], js_num(from + i));
}

static void jd_setup_node(jctx *J) {
    jobj *et = jd_p[JI_EVENTTARGET];
    jd_method(J, et, "addEventListener", nat_add_listener, 2);
    jd_method(J, et, "removeEventListener", nat_remove_listener, 2);
    jd_method(J, et, "dispatchEvent", nat_dispatch_event, 1);

    jobj *nd = jd_p[JI_NODE];
    static const char *const TYPES[] = { "ELEMENT_NODE", "ATTRIBUTE_NODE", "TEXT_NODE",
        "CDATA_SECTION_NODE", "ENTITY_REFERENCE_NODE", "ENTITY_NODE", "PROCESSING_INSTRUCTION_NODE",
        "COMMENT_NODE", "DOCUMENT_NODE", "DOCUMENT_TYPE_NODE", "DOCUMENT_FRAGMENT_NODE",
        "NOTATION_NODE", 0 };
    jd_consts(J, nd, TYPES, 1);
    jd_consts(J, jd_ctor_of(nd), TYPES, 1);
    static const char *const POS[] = { "DOCUMENT_POSITION_DISCONNECTED", "DOCUMENT_POSITION_PRECEDING",
        "DOCUMENT_POSITION_FOLLOWING", "DOCUMENT_POSITION_CONTAINS", "DOCUMENT_POSITION_CONTAINED_BY",
        "DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC", 0 };
    for (int i = 0; POS[i]; i++) {
        js_const_prop(J, nd, POS[i], js_num(1 << i));
        js_const_prop(J, jd_ctor_of(nd), POS[i], js_num(1 << i));
    }
    jd_accessor(J, nd, "nodeType", nat_node_type, 0);
    jd_accessor(J, nd, "nodeName", nat_node_name, 0);
    jd_accessor(J, nd, "nodeValue", nat_node_value, nat_node_set_value);
    jd_accessor(J, nd, "textContent", nat_text_content, nat_set_text_content);
    jd_accessor(J, nd, "parentNode", nat_parent_node, 0);
    jd_accessor(J, nd, "parentElement", nat_parent_element, 0);
    jd_accessor(J, nd, "childNodes", nat_child_nodes, 0);
    jd_accessor(J, nd, "firstChild", nat_first_child, 0);
    jd_accessor(J, nd, "lastChild", nat_last_child, 0);
    jd_accessor(J, nd, "nextSibling", nat_next_sibling, 0);
    jd_accessor(J, nd, "previousSibling", nat_prev_sibling, 0);
    jd_accessor(J, nd, "ownerDocument", nat_owner_document, 0);
    jd_accessor(J, nd, "isConnected", nat_is_connected, 0);
    jd_accessor(J, nd, "baseURI", nat_base_uri, 0);
    jd_method(J, nd, "appendChild", nat_append_child, 1);
    jd_method(J, nd, "insertBefore", nat_insert_before, 2);
    jd_method(J, nd, "removeChild", nat_remove_child, 1);
    jd_method(J, nd, "replaceChild", nat_replace_child, 2);
    jd_method(J, nd, "cloneNode", nat_clone_node, 0);
    jd_method(J, nd, "contains", nat_contains, 1);
    jd_method(J, nd, "hasChildNodes", nat_has_child_nodes, 0);
    jd_method(J, nd, "getRootNode", nat_get_root_node, 0);
    jd_method(J, nd, "compareDocumentPosition", nat_compare_position, 1);
    jd_method(J, nd, "isSameNode", nat_is_same_node, 1);
    jd_method(J, nd, "isEqualNode", nat_is_equal_node, 1);
    jd_method(J, nd, "normalize", nat_normalize, 0);

    /* ChildNode and NonDocumentTypeChildNode, on characters and elements. */
    jobj *both[2] = { jd_p[JI_CHARDATA], jd_p[JI_ELEMENT] };
    for (int i = 0; i < 2; i++) {
        jd_method(J, both[i], "before", nat_before, 0);
        jd_method(J, both[i], "after", nat_after, 0);
        jd_method(J, both[i], "replaceWith", nat_replace_with, 0);
        jd_method(J, both[i], "remove", nat_remove, 0);
        jd_accessor(J, both[i], "nextElementSibling", nat_next_el, 0);
        jd_accessor(J, both[i], "previousElementSibling", nat_prev_el, 0);
    }
    /* ParentNode, on elements, the document and fragments. */
    jobj *par[3] = { jd_p[JI_ELEMENT], jd_p[JI_DOCUMENT], jd_p[JI_FRAGMENT] };
    for (int i = 0; i < 3; i++) {
        jd_accessor(J, par[i], "children", nat_children, 0);
        jd_accessor(J, par[i], "firstElementChild", nat_first_el, 0);
        jd_accessor(J, par[i], "lastElementChild", nat_last_el, 0);
        jd_accessor(J, par[i], "childElementCount", nat_child_el_count, 0);
        jd_method(J, par[i], "append", nat_append, 0);
        jd_method(J, par[i], "prepend", nat_prepend, 0);
        jd_method(J, par[i], "replaceChildren", nat_replace_children, 0);
        jd_method(J, par[i], "querySelector", nat_query, 1);
        jd_method(J, par[i], "querySelectorAll", nat_query_all, 1);
    }
    jd_method(J, jd_p[JI_FRAGMENT], "getElementById", nat_doc_by_id, 1);

    jobj *cd = jd_p[JI_CHARDATA];
    jd_accessor(J, cd, "data", nat_cd_data, nat_node_set_value);
    jd_accessor(J, cd, "length", nat_cd_length, 0);
    jd_method(J, cd, "appendData", nat_cd_append, 1);
    jd_accessor(J, jd_p[JI_TEXT], "wholeText", nat_text_whole, 0);

    jobj *el = jd_p[JI_ELEMENT];
    jd_accessor(J, el, "tagName", nat_tag_name, 0);
    jd_accessor(J, el, "localName", nat_local_name, 0);
    jd_accessor(J, el, "namespaceURI", nat_namespace, 0);
    jd_accessor(J, el, "prefix", nat_null_getter, 0);
    jd_accessor(J, el, "shadowRoot", nat_el_shadow_root, 0);
    jd_method(J, el, "attachShadow", nat_attach_shadow, 1);
    jd_accessor(J, el, "id", nat_el_id, nat_el_set_id);
    jd_accessor(J, el, "className", nat_class_name, nat_set_class_name);
    jd_accessor(J, el, "classList", nat_class_list, nat_set_class_list);
    jd_accessor(J, el, "attributes", nat_attributes, 0);
    jd_accessor(J, el, "innerHTML", nat_inner_html, nat_set_inner_html);
    jd_accessor(J, el, "outerHTML", nat_outer_html, nat_set_outer_html);
    jd_reflect(J, el, "slot", "slot", JR_STR);
    jd_accessor(J, el, "scrollTop", nat_scroll_top, nat_nothing_js);
    jd_accessor(J, el, "scrollLeft", nat_zero, nat_nothing_js);
    jd_accessor(J, el, "scrollWidth", nat_offset_width, 0);
    jd_accessor(J, el, "scrollHeight", nat_scroll_height, 0);
    jd_accessor(J, el, "clientWidth", nat_offset_width, 0);
    jd_accessor(J, el, "clientHeight", nat_offset_height, 0);
    jd_accessor(J, el, "clientTop", nat_zero, 0);
    jd_accessor(J, el, "clientLeft", nat_zero, 0);
    jd_method(J, el, "getAttribute", nat_get_attr, 1);
    jd_method(J, el, "setAttribute", nat_set_attr, 2);
    jd_method(J, el, "removeAttribute", nat_remove_attr, 1);
    jd_method(J, el, "hasAttribute", nat_has_attr, 1);
    jd_method(J, el, "hasAttributes", nat_has_attrs, 0);
    jd_method(J, el, "toggleAttribute", nat_toggle_attr, 1);
    jd_method(J, el, "getAttributeNames", nat_attr_names, 0);
    jd_method(J, el, "getAttributeNS", nat_get_attr_ns, 2);
    jd_method(J, el, "setAttributeNS", nat_set_attr_ns, 3);
    jd_method(J, el, "removeAttributeNS", nat_remove_attr_ns, 2);
    jd_method(J, el, "hasAttributeNS", nat_has_attr_ns, 2);
    jd_method(J, el, "getElementsByTagName", nat_by_tag, 1);
    jd_method(J, el, "getElementsByTagNameNS", nat_by_tag_ns, 2);
    jd_method(J, el, "getElementsByClassName", nat_by_class, 1);
    jd_method(J, el, "closest", nat_closest, 1);
    jd_method(J, el, "matches", nat_matches, 1);
    jd_method(J, el, "webkitMatchesSelector", nat_matches, 1);
    jd_method(J, el, "insertAdjacentHTML", nat_insert_adj_html, 2);
    jd_method(J, el, "insertAdjacentElement", nat_insert_adj_el, 2);
    jd_method(J, el, "insertAdjacentText", nat_insert_adj_text, 2);
    jd_method(J, el, "getBoundingClientRect", nat_bounding_rect, 0);
    jd_method(J, el, "getClientRects", nat_client_rects, 0);
    jd_method(J, el, "scrollIntoView", nat_scroll_into_view, 0);
    jd_method(J, el, "scroll", nat_nothing_js, 0);
    jd_method(J, el, "scrollTo", nat_nothing_js, 0);
    jd_method(J, el, "scrollBy", nat_nothing_js, 0);

    jobj *he = jd_p[JI_HTMLELEMENT];
    jd_accessor(J, he, "hidden", nat_hidden, nat_set_hidden);
    jd_accessor(J, he, "style", nat_style, nat_set_style);
    jd_accessor(J, he, "dataset", nat_dataset, 0);
    jd_accessor(J, he, "innerText", nat_inner_text, nat_set_text_content);
    jd_accessor(J, he, "outerText", nat_inner_text, 0);
    jd_accessor(J, he, "offsetLeft", nat_offset_left, 0);
    jd_accessor(J, he, "offsetTop", nat_offset_top, 0);
    jd_accessor(J, he, "offsetWidth", nat_offset_width, 0);
    jd_accessor(J, he, "offsetHeight", nat_offset_height, 0);
    jd_accessor(J, he, "offsetParent", nat_offset_parent, 0);
    jd_accessor(J, he, "isContentEditable", nat_false_getter, 0);
    jd_reflect_strs(J, he, "title lang dir accessKey nonce autocapitalize enterKeyHint inputMode");
    jd_reflect_bools(J, he, "draggable spellcheck inert autofocus");
    jd_reflect_as(J, he, "tabIndex", "tabindex", JR_INT, js_num(-1));
    jd_reflect_as(J, he, "contentEditable", "contenteditable", JR_ENUM, jd_str("inherit"));
    jd_method(J, he, "click", nat_click, 0);
    jd_method(J, he, "focus", nat_focus, 0);
    jd_method(J, he, "blur", nat_blur, 0);
    jd_accessor(J, jd_p[JI_SVGELEMENT], "style", nat_style, nat_set_style);
    jd_accessor(J, jd_p[JI_SVGELEMENT], "dataset", nat_dataset, 0);
    jd_method(J, jd_p[JI_SVGELEMENT], "focus", nat_focus, 0);
    jd_method(J, jd_p[JI_SVGELEMENT], "blur", nat_blur, 0);

    /* The lists. */
    jobj *nl = jd_p[JI_NODELIST], *hc = jd_p[JI_HTMLCOLLECTION];
    jd_method(J, nl, "item", nat_list_item, 1);
    jd_method(J, nl, "forEach", nat_list_foreach, 1);
    jd_method(J, nl, "entries", nat_list_entries, 0);
    jd_method(J, nl, "keys", nat_list_keys, 0);
    jd_method(J, nl, "values", nat_list_values, 0);
    js_method_key(J, nl, J->sym_iterator, "[Symbol.iterator]", nat_list_values, 0);
    jd_method(J, hc, "item", nat_list_item, 1);
    jd_method(J, hc, "namedItem", nat_list_named, 1);
    jd_accessor(J, nl, "length", nat_list_length, 0);
    jd_accessor(J, hc, "length", nat_list_length, 0);
    js_method_key(J, hc, J->sym_iterator, "[Symbol.iterator]", nat_list_values, 0);

    jobj *tl = jd_p[JI_TOKENLIST];
    jd_method(J, tl, "add", nat_cls_add, 0);
    jd_method(J, tl, "remove", nat_cls_remove, 0);
    jd_method(J, tl, "contains", nat_cls_contains, 1);
    jd_method(J, tl, "toggle", nat_cls_toggle, 1);
    jd_method(J, tl, "replace", nat_cls_replace, 2);
    jd_method(J, tl, "item", nat_cls_item, 1);
    jd_method(J, tl, "supports", nat_cls_supports, 1);
    jd_method(J, tl, "forEach", nat_cls_foreach, 1);
    jd_method(J, tl, "values", nat_cls_values, 0);
    jd_method(J, tl, "toString", nat_cls_value, 0);
    js_method_key(J, tl, J->sym_iterator, "[Symbol.iterator]", nat_cls_values, 0);
    jd_accessor(J, tl, "value", nat_cls_value, nat_cls_set_value);
    jd_accessor(J, tl, "length", nat_cls_length, 0);

    jobj *sd = jd_p[JI_STYLEDECL];
    jd_method(J, sd, "getPropertyValue", nat_style_get, 1);
    jd_method(J, sd, "getPropertyPriority", nat_style_priority, 1);
    jd_method(J, sd, "setProperty", nat_style_set, 2);
    jd_method(J, sd, "removeProperty", nat_style_remove, 1);
    jd_method(J, sd, "item", nat_style_item, 1);
    jd_accessor(J, sd, "cssText", nat_style_csstext, nat_style_set_csstext);
    jd_accessor(J, sd, "length", nat_style_length, 0);
    jd_accessor(J, sd, "parentRule", nat_null_getter, 0);

    jobj *nm = jd_p[JI_NAMEDNODEMAP];
    jd_method(J, nm, "item", nat_attrs_item, 1);
    jd_method(J, nm, "getNamedItem", nat_attrs_named, 1);
    jd_method(J, nm, "removeNamedItem", nat_attrs_remove, 1);
    js_method_key(J, nm, J->sym_iterator, "[Symbol.iterator]", nat_attrs_values, 0);

    jd_method(J, jd_p[JI_DOMRECT], "toJSON", nat_rect_json, 0);
}

/* What each kind of HTML element adds to HTMLElement. */
static void jd_setup_html(jctx *J) {
    jobj *p;
    jval text = jd_str("text");
    if ((p = jd_iface("HTMLAnchorElement"))) {
        jd_reflect(J, p, "href", "href", JR_URL);
        jd_reflect_strs(J, p, "target download rel hreflang type referrerPolicy ping");
        jd_accessor(J, p, "text", nat_text_content, nat_set_text_content);
    }
    if ((p = jd_iface("HTMLAreaElement"))) {
        jd_reflect(J, p, "href", "href", JR_URL);
        jd_reflect_strs(J, p, "alt coords shape target download rel referrerPolicy");
    }
    if ((p = jd_iface("HTMLMediaElement"))) {
        jd_reflect(J, p, "src", "src", JR_URL);
        jd_reflect(J, p, "currentSrc", "src", JR_URL);
        jd_reflect_bools(J, p, "autoplay loop controls muted defaultMuted");
        jd_reflect_strs(J, p, "preload");
        jd_reflect_as(J, p, "crossOrigin", "crossorigin", JR_NULLSTR, js_null());
        jd_accessor(J, p, "paused", nat_true_getter, 0);
        jd_accessor(J, p, "ended", nat_false_getter, 0);
        jd_accessor(J, p, "seeking", nat_false_getter, 0);
        jd_accessor(J, p, "currentTime", nat_zero, nat_nothing_js);
        jd_accessor(J, p, "duration", nat_nan_getter, 0);
        jd_accessor(J, p, "volume", nat_one_getter, nat_nothing_js);
        jd_accessor(J, p, "playbackRate", nat_one_getter, nat_nothing_js);
        jd_accessor(J, p, "readyState", nat_zero, 0);
        jd_accessor(J, p, "networkState", nat_three_getter, 0);
        jd_accessor(J, p, "error", nat_null_getter, 0);
        jd_method(J, p, "canPlayType", nat_media_can_play, 1);
        jd_method(J, p, "play", nat_media_play, 0);
        jd_method(J, p, "pause", nat_nothing_js, 0);
        jd_method(J, p, "load", nat_nothing_js, 0);
    }
    if ((p = jd_iface("HTMLVideoElement"))) {
        jd_reflect(J, p, "poster", "poster", JR_URL);
        jd_reflect(J, p, "width", "width", JR_INT);
        jd_reflect(J, p, "height", "height", JR_INT);
        jd_reflect_bools(J, p, "playsInline");
        jd_accessor(J, p, "videoWidth", nat_zero, 0);
        jd_accessor(J, p, "videoHeight", nat_zero, 0);
    }
    if ((p = jd_iface("HTMLBaseElement"))) {
        jd_reflect(J, p, "href", "href", JR_URL);
        jd_reflect_strs(J, p, "target");
    }
    if ((p = jd_iface("HTMLButtonElement"))) {
        jd_reflect_as(J, p, "type", "type", JR_ENUM, jd_str("submit"));
        jd_reflect_strs(J, p, "name formAction formMethod formTarget");
        jd_reflect_bools(J, p, "disabled formNoValidate");
        jd_accessor(J, p, "value", nat_value, nat_set_value);
        jd_accessor(J, p, "form", nat_form_of, 0);
        jd_accessor(J, p, "labels", nat_labels, 0);
        jd_method(J, p, "checkValidity", nat_true_fn, 0);
        jd_method(J, p, "reportValidity", nat_true_fn, 0);
        jd_method(J, p, "setCustomValidity", nat_nothing_js, 1);
    }
    if ((p = jd_iface("HTMLCanvasElement"))) {
        jd_reflect_as(J, p, "width", "width", JR_INT, js_num(300));
        jd_reflect_as(J, p, "height", "height", JR_INT, js_num(150));
        jd_method(J, p, "getContext", nat_canvas_context, 1);
        jd_method(J, p, "toDataURL", nat_canvas_data_url, 0);
    }
    if ((p = jd_iface("HTMLDataElement"))) jd_reflect_strs(J, p, "value");
    if ((p = jd_iface("HTMLDetailsElement"))) jd_reflect_bools(J, p, "open");
    if ((p = jd_iface("HTMLDialogElement"))) {
        jd_reflect_bools(J, p, "open");
        jd_method(J, p, "show", nat_dialog_show, 0);
        jd_method(J, p, "showModal", nat_dialog_show, 0);
        jd_method(J, p, "close", nat_dialog_close, 0);
    }
    if ((p = jd_iface("HTMLEmbedElement"))) {
        jd_reflect(J, p, "src", "src", JR_URL);
        jd_reflect_strs(J, p, "type width height");
    }
    if ((p = jd_iface("HTMLFieldSetElement"))) {
        jd_reflect_strs(J, p, "name");
        jd_reflect_bools(J, p, "disabled");
        jd_accessor(J, p, "elements", nat_form_elements, 0);
        jd_accessor(J, p, "form", nat_form_of, 0);
    }
    if ((p = jd_iface("HTMLFormElement"))) {
        jd_accessor(J, p, "action", nat_form_action, 0);
        jd_reflect_as(J, p, "method", "method", JR_ENUM, jd_str("get"));
        jd_reflect_as(J, p, "enctype", "enctype", JR_ENUM, jd_str("application/x-www-form-urlencoded"));
        jd_reflect_strs(J, p, "name target acceptCharset autocomplete rel");
        jd_reflect_bools(J, p, "noValidate");
        jd_accessor(J, p, "elements", nat_form_elements, 0);
        jd_accessor(J, p, "length", nat_form_length, 0);
        jd_method(J, p, "submit", nat_form_submit, 0);
        jd_method(J, p, "requestSubmit", nat_form_request_submit, 0);
        jd_method(J, p, "reset", nat_form_reset, 0);
        jd_method(J, p, "checkValidity", nat_true_fn, 0);
        jd_method(J, p, "reportValidity", nat_true_fn, 0);
    }
    if ((p = jd_iface("HTMLIFrameElement"))) {
        jd_reflect(J, p, "src", "src", JR_URL);
        jd_reflect_strs(J, p, "srcdoc name width height allow loading referrerPolicy");
        jd_reflect_bools(J, p, "allowFullscreen");
        /* No frame is loaded, so there is no window or document in it. */
        jd_accessor(J, p, "contentWindow", nat_null_getter, 0);
        jd_accessor(J, p, "contentDocument", nat_null_getter, 0);
    }
    if ((p = jd_iface("HTMLImageElement"))) {
        jd_reflect(J, p, "src", "src", JR_URL);
        jd_reflect(J, p, "currentSrc", "src", JR_URL);
        jd_reflect_strs(J, p, "alt srcset sizes useMap loading decoding referrerPolicy fetchPriority");
        jd_reflect_bools(J, p, "isMap");
        jd_reflect_as(J, p, "crossOrigin", "crossorigin", JR_NULLSTR, js_null());
        jd_accessor(J, p, "width", nat_img_width, nat_img_set_width);
        jd_accessor(J, p, "height", nat_img_height, nat_img_set_height);
        jd_accessor(J, p, "naturalWidth", nat_img_natural_w, 0);
        jd_accessor(J, p, "naturalHeight", nat_img_natural_h, 0);
        /* Every picture the browser will fetch is fetched before a script
           runs, and one that will not come is broken, which is complete
           too as the standard counts it. */
        jd_accessor(J, p, "complete", nat_true_getter, 0);
        jd_method(J, p, "decode", nat_img_decode, 0);
    }
    if ((p = jd_iface("HTMLInputElement"))) {
        jd_reflect_as(J, p, "type", "type", JR_ENUM, text);
        jd_reflect_strs(J, p, "name placeholder min max step pattern accept autocomplete alt dirName formAction formMethod formTarget");
        jd_reflect_bools(J, p, "disabled readOnly required multiple formNoValidate indeterminate");
        jd_reflect(J, p, "src", "src", JR_URL);
        jd_reflect_as(J, p, "maxLength", "maxlength", JR_INT, js_num(-1));
        jd_reflect_as(J, p, "minLength", "minlength", JR_INT, js_num(-1));
        jd_reflect_as(J, p, "size", "size", JR_INT, js_num(20));
        jd_reflect(J, p, "defaultValue", "value", JR_STR);
        jd_reflect(J, p, "defaultChecked", "checked", JR_BOOL);
        jd_accessor(J, p, "value", nat_value, nat_set_value);
        jd_accessor(J, p, "checked", nat_checked, nat_set_checked);
        jd_accessor(J, p, "form", nat_form_of, 0);
        jd_accessor(J, p, "files", nat_null_getter, 0);
        jd_accessor(J, p, "list", nat_null_getter, 0);
        jd_accessor(J, p, "labels", nat_labels, 0);
        jd_accessor(J, p, "selectionStart", nat_selection_end, nat_nothing_js);
        jd_accessor(J, p, "selectionEnd", nat_selection_end, nat_nothing_js);
        jd_method(J, p, "select", nat_select_text, 0);
        jd_method(J, p, "setSelectionRange", nat_nothing_js, 2);
        jd_method(J, p, "checkValidity", nat_true_fn, 0);
        jd_method(J, p, "reportValidity", nat_true_fn, 0);
        jd_method(J, p, "setCustomValidity", nat_nothing_js, 1);
    }
    if ((p = jd_iface("HTMLLIElement"))) jd_reflect(J, p, "value", "value", JR_INT);
    if ((p = jd_iface("HTMLLabelElement"))) {
        jd_reflect(J, p, "htmlFor", "for", JR_STR);
        jd_accessor(J, p, "control", nat_label_control, 0);
        jd_accessor(J, p, "form", nat_form_of, 0);
    }
    if ((p = jd_iface("HTMLLinkElement"))) {
        jd_reflect(J, p, "href", "href", JR_URL);
        jd_reflect_strs(J, p, "rel as media type hreflang sizes integrity referrerPolicy imageSrcset imageSizes fetchPriority");
        jd_reflect_as(J, p, "crossOrigin", "crossorigin", JR_NULLSTR, js_null());
        jd_reflect_bools(J, p, "disabled");
        jd_accessor(J, p, "sheet", nat_cssom_sheet, 0);
    }
    if ((p = jd_iface("HTMLMapElement"))) jd_reflect_strs(J, p, "name");
    if ((p = jd_iface("HTMLMetaElement"))) {
        jd_reflect_strs(J, p, "name content media scheme");
        jd_reflect(J, p, "httpEquiv", "http-equiv", JR_STR);
    }
    if ((p = jd_iface("HTMLMeterElement"))) {
        jd_reflect(J, p, "value", "value", JR_INT);
        jd_reflect(J, p, "min", "min", JR_INT);
        jd_reflect_as(J, p, "max", "max", JR_INT, js_num(1));
        jd_reflect(J, p, "low", "low", JR_INT);
        jd_reflect(J, p, "high", "high", JR_INT);
        jd_reflect(J, p, "optimum", "optimum", JR_INT);
    }
    if ((p = jd_iface("HTMLModElement"))) {
        jd_reflect(J, p, "cite", "cite", JR_URL);
        jd_reflect(J, p, "dateTime", "datetime", JR_STR);
    }
    if ((p = jd_iface("HTMLOListElement"))) {
        jd_reflect_as(J, p, "start", "start", JR_INT, js_num(1));
        jd_reflect_bools(J, p, "reversed");
        jd_reflect_strs(J, p, "type");
    }
    if ((p = jd_iface("HTMLObjectElement"))) {
        jd_reflect(J, p, "data", "data", JR_URL);
        jd_reflect_strs(J, p, "type name width height useMap");
        jd_accessor(J, p, "contentDocument", nat_null_getter, 0);
        jd_accessor(J, p, "contentWindow", nat_null_getter, 0);
    }
    if ((p = jd_iface("HTMLOptGroupElement"))) {
        jd_reflect_strs(J, p, "label");
        jd_reflect_bools(J, p, "disabled");
    }
    if ((p = jd_iface("HTMLOptionElement"))) {
        jd_accessor(J, p, "value", nat_value, nat_set_value);
        jd_accessor(J, p, "text", nat_text_content, nat_set_text_content);
        jd_accessor(J, p, "selected", nat_option_selected, nat_option_set_selected);
        jd_reflect(J, p, "defaultSelected", "selected", JR_BOOL);
        jd_reflect_strs(J, p, "label");
        jd_reflect_bools(J, p, "disabled");
        jd_accessor(J, p, "index", nat_option_index, 0);
        jd_accessor(J, p, "form", nat_form_of, 0);
    }
    if ((p = jd_iface("HTMLOutputElement"))) {
        jd_reflect_strs(J, p, "name");
        jd_accessor(J, p, "value", nat_text_content, nat_set_text_content);
        jd_accessor(J, p, "form", nat_form_of, 0);
    }
    if ((p = jd_iface("HTMLProgressElement"))) {
        jd_reflect(J, p, "value", "value", JR_INT);
        jd_reflect_as(J, p, "max", "max", JR_INT, js_num(1));
    }
    if ((p = jd_iface("HTMLQuoteElement"))) jd_reflect(J, p, "cite", "cite", JR_URL);
    if ((p = jd_iface("HTMLScriptElement"))) {
        jd_reflect(J, p, "src", "src", JR_URL);
        jd_reflect_strs(J, p, "type charset integrity referrerPolicy nonce event fetchPriority");
        jd_reflect(J, p, "htmlFor", "for", JR_STR);
        jd_reflect_bools(J, p, "async defer");
        jd_reflect_as(J, p, "crossOrigin", "crossorigin", JR_NULLSTR, js_null());
        jd_accessor(J, p, "text", nat_script_text, nat_script_set_text);
        jobj *c = jd_ctor_of(p);
        if (c) js_method(J, c, "supports", nat_script_supports, 1);
    }
    if ((p = jd_iface("HTMLSelectElement"))) {
        jd_reflect_strs(J, p, "name autocomplete");
        jd_reflect_bools(J, p, "disabled multiple required");
        jd_reflect(J, p, "size", "size", JR_INT);
        jd_accessor(J, p, "value", nat_value, nat_set_value);
        jd_accessor(J, p, "selectedIndex", nat_selected_index, nat_set_selected_index);
        jd_accessor(J, p, "options", nat_options, 0);
        jd_accessor(J, p, "selectedOptions", nat_selected_options, 0);
        jd_accessor(J, p, "length", nat_select_length, 0);
        jd_accessor(J, p, "form", nat_form_of, 0);
        jd_accessor(J, p, "labels", nat_labels, 0);
        jd_accessor(J, p, "type", nat_empty_str, 0);
        jd_method(J, p, "add", nat_select_add, 1);
        jd_method(J, p, "remove", nat_select_remove, 0);
        jd_method(J, p, "checkValidity", nat_true_fn, 0);
        jd_method(J, p, "reportValidity", nat_true_fn, 0);
        jd_method(J, p, "setCustomValidity", nat_nothing_js, 1);
    }
    if ((p = jd_iface("HTMLSlotElement"))) {
        jd_reflect_strs(J, p, "name");
        jd_method(J, p, "assignedNodes", nat_slot_assigned_nodes, 0);
        jd_method(J, p, "assignedElements", nat_slot_assigned_elements, 0);
    }
    jd_accessor(J, jd_p[JI_ELEMENT], "assignedSlot", nat_assigned_slot, 0);
    jd_accessor(J, jd_p[JI_TEXT], "assignedSlot", nat_assigned_slot, 0);
    if ((p = jd_iface("HTMLSourceElement"))) {
        jd_reflect(J, p, "src", "src", JR_URL);
        jd_reflect_strs(J, p, "srcset sizes media type");
    }
    if ((p = jd_iface("HTMLStyleElement"))) {
        jd_reflect_strs(J, p, "media type nonce");
        jd_reflect_bools(J, p, "disabled");
        jd_accessor(J, p, "sheet", nat_cssom_sheet, 0);
    }
    if ((p = jd_iface("HTMLTableCellElement"))) {
        jd_reflect_as(J, p, "colSpan", "colspan", JR_INT, js_num(1));
        jd_reflect_as(J, p, "rowSpan", "rowspan", JR_INT, js_num(1));
        jd_reflect_strs(J, p, "headers abbr scope");
    }
    if ((p = jd_iface("HTMLTableColElement"))) jd_reflect_as(J, p, "span", "span", JR_INT, js_num(1));
    if ((p = jd_iface("HTMLTableElement"))) jd_accessor(J, p, "rows", nat_table_rows, 0);
    if ((p = jd_iface("HTMLTableSectionElement"))) jd_accessor(J, p, "rows", nat_table_rows, 0);
    if ((p = jd_iface("HTMLTableRowElement"))) jd_accessor(J, p, "cells", nat_row_cells, 0);
    if ((p = jd_iface("HTMLTemplateElement"))) jd_accessor(J, p, "content", nat_template_content, 0);
    if ((p = jd_iface("HTMLTextAreaElement"))) {
        jd_reflect_strs(J, p, "name placeholder wrap autocomplete dirName");
        jd_reflect_bools(J, p, "disabled readOnly required");
        jd_reflect_as(J, p, "rows", "rows", JR_INT, js_num(2));
        jd_reflect_as(J, p, "cols", "cols", JR_INT, js_num(20));
        jd_reflect_as(J, p, "maxLength", "maxlength", JR_INT, js_num(-1));
        jd_accessor(J, p, "value", nat_value, nat_set_value);
        jd_accessor(J, p, "defaultValue", nat_text_content, nat_set_text_content);
        jd_accessor(J, p, "form", nat_form_of, 0);
        jd_accessor(J, p, "labels", nat_labels, 0);
        jd_accessor(J, p, "type", nat_empty_str, 0);
        jd_accessor(J, p, "selectionStart", nat_selection_end, nat_nothing_js);
        jd_accessor(J, p, "selectionEnd", nat_selection_end, nat_nothing_js);
        jd_method(J, p, "select", nat_select_text, 0);
        jd_method(J, p, "setSelectionRange", nat_nothing_js, 2);
        jd_method(J, p, "checkValidity", nat_true_fn, 0);
        jd_method(J, p, "reportValidity", nat_true_fn, 0);
    }
    if ((p = jd_iface("HTMLTimeElement"))) jd_reflect(J, p, "dateTime", "datetime", JR_STR);
    if ((p = jd_iface("HTMLTitleElement"))) jd_accessor(J, p, "text", nat_text_content, nat_set_text_content);
    if ((p = jd_iface("HTMLTrackElement"))) {
        jd_reflect(J, p, "src", "src", JR_URL);
        jd_reflect_strs(J, p, "kind label srclang");
        jd_reflect_bools(J, p, "default");
    }
    if ((p = jd_iface("HTMLUListElement"))) jd_reflect_strs(J, p, "type");
}

static void jd_setup_document(jctx *J, jobj *document) {
    jobj *d = jd_p[JI_DOCUMENT];
    jd_method(J, d, "getElementById", nat_doc_by_id, 1);
    jd_method(J, d, "getElementsByTagName", nat_by_tag, 1);
    jd_method(J, d, "getElementsByTagNameNS", nat_by_tag_ns, 2);
    jd_method(J, d, "getElementsByClassName", nat_by_class, 1);
    jd_method(J, d, "getElementsByName", nat_doc_by_name, 1);
    jd_method(J, d, "createElement", nat_doc_create_el, 1);
    jd_method(J, d, "createElementNS", nat_doc_create_el_ns, 2);
    jd_method(J, d, "createTextNode", nat_doc_create_text, 1);
    jd_method(J, d, "createComment", nat_doc_create_comment, 1);
    jd_method(J, d, "createDocumentFragment", nat_doc_create_fragment, 0);
    jd_method(J, d, "createEvent", nat_create_event, 1);
    jd_method(J, d, "importNode", nat_doc_import, 1);
    jd_method(J, d, "adoptNode", nat_doc_adopt, 1);
    jd_method(J, d, "write", nat_doc_write, 0);
    jd_method(J, d, "writeln", nat_doc_writeln, 0);
    jd_method(J, d, "open", nat_nothing_js, 0);
    jd_method(J, d, "close", nat_nothing_js, 0);
    jd_method(J, d, "hasFocus", nat_doc_has_focus, 0);
    jd_method(J, d, "elementFromPoint", nat_doc_element_from_point, 2);
    jd_method(J, d, "elementsFromPoint", nat_doc_elements_from_point, 2);
    jd_accessor(J, d, "documentElement", nat_doc_element, 0);
    jd_accessor(J, d, "head", nat_doc_head, 0);
    jd_accessor(J, d, "body", nat_doc_body, 0);
    jd_accessor(J, d, "title", nat_doc_title, nat_doc_set_title);
    jd_accessor(J, d, "cookie", nat_doc_cookie, nat_doc_set_cookie);
    jd_accessor(J, d, "URL", nat_doc_url, 0);
    jd_accessor(J, d, "documentURI", nat_doc_url, 0);
    jd_accessor(J, d, "domain", nat_doc_domain, 0);
    jd_accessor(J, d, "referrer", nat_doc_referrer, 0);
    jd_accessor(J, d, "characterSet", nat_doc_charset, 0);
    jd_accessor(J, d, "charset", nat_doc_charset, 0);
    jd_accessor(J, d, "inputEncoding", nat_doc_charset, 0);
    jd_accessor(J, d, "contentType", nat_doc_content_type, 0);
    jd_accessor(J, d, "compatMode", nat_doc_compat, 0);
    jd_accessor(J, d, "readyState", nat_doc_ready, 0);
    jd_accessor(J, d, "visibilityState", nat_doc_visibility, 0);
    jd_accessor(J, d, "hidden", nat_doc_hidden, 0);
    jd_accessor(J, d, "defaultView", nat_doc_view, 0);
    jd_setup_documents(J, d);
    jd_accessor(J, d, "activeElement", nat_doc_active, 0);
    jd_accessor(J, d, "forms", nat_doc_forms, 0);
    jd_accessor(J, d, "images", nat_doc_images, 0);
    jd_accessor(J, d, "links", nat_doc_links, 0);
    jd_accessor(J, d, "scripts", nat_doc_scripts, 0);
    jd_accessor(J, d, "currentScript", nat_doc_current_script, 0);
    jd_accessor(J, d, "doctype", nat_doctype, 0);
    jd_accessor(J, d, "fullscreenElement", nat_null_getter, 0);
    jd_accessor(J, d, "scrollingElement", nat_doc_element, 0);
    (void)document;
}

/* The window's own: the constructors and the few functions that are not
   anybody's methods. */
static void jd_setup_window(jctx *J) {
    jscope *g = J->global;
    js_declare(J, g, js_str(J, "setTimeout"), js_from_obj(js_native_n(J, "setTimeout", nat_set_timeout, 1)));
    js_declare(J, g, js_str(J, "setInterval"), js_from_obj(js_native_n(J, "setInterval", nat_set_interval, 1)));
    js_declare(J, g, js_str(J, "clearTimeout"), js_from_obj(js_native_n(J, "clearTimeout", nat_clear_timer, 1)));
    js_declare(J, g, js_str(J, "clearInterval"), js_from_obj(js_native_n(J, "clearInterval", nat_clear_timer, 1)));
    /* `window` is the global object, which is what it is in a browser, so a
       page that writes window.x and reads x gets the same thing. And the
       window listens, as the element and the document do: the page's first
       line is often window.addEventListener('load', start). */
    js_declare(J, g, js_str(J, "window"), js_from_obj(J->global_obj));
    js_declare(J, g, js_str(J, "addEventListener"), js_from_obj(js_native_n(J, "addEventListener", nat_add_listener, 2)));
    js_declare(J, g, js_str(J, "removeEventListener"), js_from_obj(js_native_n(J, "removeEventListener", nat_remove_listener, 2)));
    js_declare(J, g, js_str(J, "dispatchEvent"), js_from_obj(js_native_n(J, "dispatchEvent", nat_dispatch_event, 1)));

    /* customElements */
    jobj *ce = js_object(J, JO_PLAIN);
    if (ce) {
        js_method(J, ce, "define", nat_ce_define, 2);
        js_method(J, ce, "get", nat_ce_get, 1);
        js_method(J, ce, "whenDefined", nat_ce_when, 1);
        js_method(J, ce, "upgrade", nat_ce_upgrade, 1);
        js_tag(J, ce, "CustomElementRegistry");
        js_declare(J, g, js_str(J, "customElements"), js_from_obj(ce));
    }
}

/* Every interface, in the order each needs its parent. */
static void jd_setup(jctx *J) {
    jd_p[JI_EVENTTARGET] = jd_interface(J, "EventTarget", 0, nat_event_target_ctor, 0);
    jd_p[JI_NODE] = jd_interface(J, "Node", jd_p[JI_EVENTTARGET], 0, 0);
    jd_p[JI_CHARDATA] = jd_interface(J, "CharacterData", jd_p[JI_NODE], 0, 0);
    jd_p[JI_TEXT] = jd_interface(J, "Text", jd_p[JI_CHARDATA], nat_text_ctor, 0);
    jd_p[JI_COMMENT] = jd_interface(J, "Comment", jd_p[JI_CHARDATA], nat_comment_ctor, 0);
    jd_p[JI_ELEMENT] = jd_interface(J, "Element", jd_p[JI_NODE], 0, 0);
    jd_p[JI_HTMLELEMENT] = jd_interface(J, "HTMLElement", jd_p[JI_ELEMENT], nat_html_element_ctor, 0);
    jd_p[JI_SVGELEMENT] = jd_interface(J, "SVGElement", jd_p[JI_ELEMENT], 0, 0);
    jd_p[JI_SVGSVG] = jd_interface(J, "SVGSVGElement", jd_p[JI_SVGELEMENT], 0, 0);
    jd_p[JI_DOCUMENT] = jd_interface(J, "Document", jd_p[JI_NODE], 0, 0);
    jd_p[JI_HTMLDOCUMENT] = jd_interface(J, "HTMLDocument", jd_p[JI_DOCUMENT], 0, 0);
    jd_p[JI_FRAGMENT] = jd_interface(J, "DocumentFragment", jd_p[JI_NODE], nat_fragment_ctor, 0);
    /* Alpine.js asks `parentNode instanceof ShadowRoot` of every element it
       walks, and Ars Technica stopped on the name; attachShadow makes them. */
    jd_p_shadowroot = jd_interface(J, "ShadowRoot", jd_p[JI_FRAGMENT], 0, 0);
    jd_accessor(J, jd_p_shadowroot, "host", nat_shadow_host, 0);
    jd_accessor(J, jd_p_shadowroot, "mode", nat_shadow_mode, 0);
    jd_accessor(J, jd_p_shadowroot, "innerHTML", nat_inner_html, nat_set_inner_html);
    jd_accessor(J, jd_p_shadowroot, "activeElement", nat_null_getter, 0);
    if (jd_p_shadowroot) {
        js_set(J, jd_p_shadowroot, "delegatesFocus", js_bool(0));
        js_set(J, jd_p_shadowroot, "slotAssignment", jd_str("named"));
    }
    jd_p[JI_NODELIST] = jd_interface(J, "NodeList", 0, 0, 0);
    jd_p[JI_HTMLCOLLECTION] = jd_interface(J, "HTMLCollection", 0, 0, 0);
    jd_p[JI_TOKENLIST] = jd_interface(J, "DOMTokenList", 0, 0, 0);
    jd_p[JI_STYLEDECL] = jd_interface(J, "CSSStyleDeclaration", 0, 0, 0);
    jd_p[JI_STRINGMAP] = jd_interface(J, "DOMStringMap", 0, 0, 0);
    jd_p[JI_NAMEDNODEMAP] = jd_interface(J, "NamedNodeMap", 0, 0, 0);
    jd_p[JI_ATTR] = jd_interface(J, "Attr", jd_p[JI_NODE], 0, 0);
    jd_p[JI_WINDOW] = jd_interface(J, "Window", jd_p[JI_EVENTTARGET], 0, 0);
    jd_p[JI_DOMRECT] = jd_interface(J, "DOMRectReadOnly", 0, nat_domrect_ctor, 0);
    jd_p[JI_DOMRECT] = jd_interface(J, "DOMRect", jd_p[JI_DOMRECT], nat_domrect_ctor, 0);

    /* The HTML element interfaces, parents first: the table lists each
       parent before the ones that name it. */
    for (int i = 0; i < JD_HTML_N; i++) {
        jobj *parent = jd_p[JI_HTMLELEMENT];
        if (JD_HTML[i].parent) {
            int k = jd_html_index(JD_HTML[i].parent);
            if (k >= 0 && jd_html_p[k]) parent = jd_html_p[k];
        }
        jd_html_p[i] = jd_interface(J, JD_HTML[i].name, parent, 0, 0);
    }
    for (int i = 0; i < T_COUNT; i++) jd_tag_proto[i] = 0;

    /* The events, each a constructor that works. */
    for (int i = 0; i < JD_EVKINDS; i++) {
        jobj *parent = JD_EVENTS[i].parent ? jd_ev_proto[jd_evkind(JD_EVENTS[i].parent)] : 0;
        jd_ev_proto[i] = jd_interface(J, JD_EVENTS[i].name, parent, nat_event_ctor, 1);
        jobj *c = jd_ctor_of(jd_ev_proto[i]);
        if (c) c->data = js_num(i);
    }
    jd_p[JI_EVENT] = jd_ev_proto[0];
    jobj *ev = jd_p[JI_EVENT];
    static const char *const PHASES[] = { "NONE", "CAPTURING_PHASE", "AT_TARGET", "BUBBLING_PHASE", 0 };
    jd_consts(J, ev, PHASES, 0);
    jd_consts(J, jd_ctor_of(ev), PHASES, 0);
    jd_method(J, ev, "preventDefault", nat_ev_prevent, 0);
    jd_method(J, ev, "stopPropagation", nat_ev_stop, 0);
    jd_method(J, ev, "stopImmediatePropagation", nat_ev_stop_now, 0);
    jd_method(J, ev, "composedPath", nat_ev_path, 0);
    jd_method(J, ev, "initEvent", nat_ev_init_event, 3);
    jd_method(J, jd_ev_proto[jd_evkind("CustomEvent")], "initCustomEvent", nat_ev_init_custom, 4);
    jd_method(J, jd_ev_proto[jd_evkind("MouseEvent")], "getModifierState", nat_ev_modifier, 1);
    jd_method(J, jd_ev_proto[jd_evkind("KeyboardEvent")], "getModifierState", nat_ev_modifier, 1);

    jd_setup_node(J);
    jd_setup_html(J);


    /* The two element constructors a page may call itself. */
    jobj *img = js_native_n(J, "Image", nat_image_ctor, 0);
    if (img) {
        js_put_prop_flags(J, img, J->s_prototype, js_from_obj(jd_iface("HTMLImageElement")), 0);
        js_declare_flags(J, J->global, js_str(J, "Image"), js_from_obj(img), JP_WRITE | JP_CONF);
    }
    jobj *opt = js_native_n(J, "Option", nat_option_ctor, 0);
    if (opt) {
        js_put_prop_flags(J, opt, J->s_prototype, js_from_obj(jd_iface("HTMLOptionElement")), 0);
        js_declare_flags(J, J->global, js_str(J, "Option"), js_from_obj(opt), JP_WRITE | JP_CONF);
    }

    jd_setup_window(J);
    jd_setup_url(J);
    jd_setup_net(J);
    jd_setup_navigator(J);
    jd_setup_location(J);
    jd_setup_window_more(J);
    jd_setup_matrix(J);
    jd_setup_form(J);
    jd_setup_cssom(J);
    jd_setup_workers(J);
    jd_setup_storage(J);
    jd_setup_observers(J);
    jd_setup_walks(J);
}

/* --- opening and closing the world -----------------------------------------------------------
 *
 * One context for the whole life of the page, because a handler is a piece
 * of a program that runs after the program has finished, and there has to
 * be something left for it to run in. */

/* Whether this page has anything to run at all. A script element is the
   obvious half; the other is a handler attribute, and a page can carry
   those without a script element anywhere in it. Neither costs a context,
   a global object or a quarter megabyte, which is still what the great
   majority of pages should cost. */
static int jd_page_scripts(const ddoc *d) {
    for (int i = 0; i < d->count; i++) {
        if (d->nodes[i].kind != DN_ELEMENT) continue;
        if (d->nodes[i].tag == T_SCRIPT) return 1;
        const dnode *n = &d->nodes[i];
        for (int a = n->attr_at; a < n->attr_at + n->attr_n; a++) {
            const char *nm = d->arena + d->attrs[a].name;
            if (nm[0] == 'o' && nm[1] == 'n' && nm[2]) return 1;
        }
    }
    return 0;
}

static void jd_zero(void *p, int n) {
    volatile u8 *b = (volatile u8 *)p;
    for (int i = 0; i < n; i++) b[i] = 0;
}

/* Every element object, every string and every scope goes here. The document
   keeps whatever the scripts wrote into it, because that lives in the
   document's own arena rather than this one. */
static void jsdom_close(void) {
    jd_mods_reset();
    jw_close_all();
    for (int i = 0; i < jd_nreq; i++) { jd_req[i].waiting = 0; jd_req[i].self = 0; }
    jd_nreq = 0;
    if (!jd_open) return;
    js_done(&jd_J);
    jd_open = 0;
    jd_doc = 0;
    jd_sheet = 0;
    jd_wrap = 0;
    jd_document_obj = 0;
    jd_listen = 0;                       /* the region's, gone with it */
    jd_nlisten = jd_caplisten = 0;
    jd_timer = 0;
    jd_ntimer = jd_captimer = 0;
    jd_lists_free();
    jd_ncustom = 0;
    jd_npending = 0;
}

static int jsdom_open(ddoc *d, csheet *sheet) {
    jsdom_close();
    if (!d || !jd_page_scripts(d)) return 0;
    jd_sheet = sheet;

    js_init(&jd_J);
    jd_J.mem_cap = jd_mem_cap();
    jd_J.host_get = jd_host_get;
    jd_J.host_set = jd_host_set;
    jd_J.import_hook = jd_import;
    jd_J.meta_hook = jd_import_meta;

    jd_doc = d;
    jd_dirty = 0;
    jd_version = 1;
    jd_parsed = d->count;
    jd_err[0] = 0;
    jd_nlisten = 0;
    jd_ntimer = 0;
    jd_timer_id = 0;
    jd_tick0 = ticks();
    jd_ready = 0;
    jd_active = -1;
    jd_current_script = -1;
    jd_nwritten = 0;
    jd_upgrading = -1;
    jd_restyle_any = 0;
    jd_zero(jd_started, (int)sizeof(jd_started));
    jd_zero(jd_upgraded, (int)sizeof(jd_upgraded));
    jd_zero(jd_svg_made, (int)sizeof(jd_svg_made));
    jd_zero(jd_restyle, (int)sizeof(jd_restyle));
    jd_zero(jd_shadow_mark, (int)sizeof(jd_shadow_mark));
    jd_zero(jd_inert_mark, (int)sizeof(jd_inert_mark));
    jd_nshadow = 0;
    jd_zero(jd_slot_pending, (int)sizeof(jd_slot_pending));
    jd_open = 1;

    /* Sized for the whole document rather than for the part of it that
       exists now: a script may add nodes past the end of what was parsed,
       and an object for one of those has to land somewhere. */
    jd_wrap = (jobj **)js_alloc(&jd_J, (u32)sizeof(jobj *) * (u32)JD_WRAPS);

    jd_k_classlist = js_sym_new(&jd_J, "classList", 9);
    jd_k_style = js_sym_new(&jd_J, "style", 5);
    jd_k_dataset = js_sym_new(&jd_J, "dataset", 7);
    jd_k_attrs = js_sym_new(&jd_J, "attributes", 10);
    jd_k_children = js_sym_new(&jd_J, "children", 8);
    jd_k_childnodes = js_sym_new(&jd_J, "childNodes", 10);
    jd_k_content = js_sym_new(&jd_J, "content", 7);
    jd_k_evflags = js_sym_new(&jd_J, "event", 5);
    jd_k_evpath = js_sym_new(&jd_J, "path", 4);
    jd_k_signal = js_sym_new(&jd_J, "signal", 6);
    jd_k_shadow = js_sym_new(&jd_J, "shadow", 6);
    jd_k_host = js_sym_new(&jd_J, "host", 4);
    jd_k_mode = js_sym_new(&jd_J, "mode", 4);

    jd_setup(&jd_J);

    /* The window is an instance of Window, and so an EventTarget. */
    if (jd_J.global_obj && jd_p[JI_WINDOW]) jd_J.global_obj->proto = jd_p[JI_WINDOW];

    jobj *document = js_object_with(&jd_J, JO_PLAIN, jd_p[JI_HTMLDOCUMENT]);
    jd_document_obj = document;
    if (document) {
        document->host = JD_DOCUMENT;
        jd_setup_document(&jd_J, document);
        js_declare(&jd_J, jd_J.global, js_str(&jd_J, "document"), js_from_obj(document));
    }

    /* A template's children are its content, a fragment of its own, from
       the start. Made on the first read of .content, they were the
       template's children until then: Alpine's walk of the page went into
       them, queued what their attributes said, and ran it after the
       template's x-teleport had moved them out from under their scope --
       "isCategoryExcluded is not defined", 28 times on Ars Technica. */
    for (int i = 0; i < jd_parsed; i++)
        if (d->nodes[i].kind == DN_ELEMENT && d->nodes[i].first >= 0 && jd_is_template(i))
            jd_template_content(i);
    return 1;
}
