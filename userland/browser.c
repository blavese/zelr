/* A web browser.
 *
 * It fetches a page over a TCP connection this system implements, builds a
 * tree out of the HTML, reads the style sheets the page asks for, works out
 * what every element ends up looking like, lays that out against the width
 * of its own window, and draws it with letterforms this project drew.
 * Nothing in that sentence comes from anywhere else.
 *
 * It used to have no CSS at all, and drew pages the way pages were drawn
 * before there was any: headings bigger, links blue, one column. That is a
 * defensible answer for a document and the wrong one for the web as it is,
 * where the difference between a menu and a list of links, or between a
 * sidebar and the article, exists only in a style sheet. Without one a page
 * is not simplified. It is read in the wrong order, and the reader is not
 * told that is what is happening.
 *
 * https works, which took a certificate parser, a big integer library, two
 * key exchanges and a root store to say so. None of it is borrowed either.
 *
 * The status line says "encrypted" or "NOT encrypted" in words rather than
 * drawing a padlock, because a padlock is a picture people have learned to
 * read as a promise about the site. What this can promise is narrower and
 * worth being exact about: the bytes came from whoever holds the name that
 * was typed, proved by a signature chaining to an authority this machine
 * was built trusting. It says nothing about whether the site is honest.
 */
#include "zelr.h"
#include "draw.h"
#include "ui.h"
#include "web.h"
#include "fetch.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "jsdom.h"
#include "png.h"
#include "jpeg.h"
#include "svg.h"

/* --- how much room there is ----------------------------------------------
 *
 * These are fixed rather than grown. There is an allocator now — the
 * JavaScript engine below uses it, in quarter megabyte chunks — but the
 * buffers a page is read into are decided once and reused for every page,
 * because a browser that allocated per page would be a browser whose
 * failure to show one depended on which one it showed before. They are
 * sized for a real page: a few hundred kilobytes of source, a handful of
 * style sheets, a few thousand words on the screen. */
#define SRC_MAX    (320 * 1024)
#define CSS_MAX    (192 * 1024)
#define SHEETS_MAX 6
#define HIST_MAX   40

static char src[SRC_MAX];
static char cssbuf[CSS_MAX];

static ddoc   doc;
static csheet sheet;
static cindex index_;
static ldoc   page;
static cmatch match;
static cinline inl[DOM_NODES];

typedef struct { char text[URL_TEXT]; int scroll; } hist_t;
static hist_t hist[HIST_MAX];
static int    hist_n, hist_at = -1;

static url_t    here;
static response_t reply;

static char  title[160];
static char  status[URL_TEXT + 96];
static int   scroll;
static int   over_link = -1;
static int   hover_node = -1;

/* The size everything relative is relative to. A page that says 1.2em means
   twenty per cent more than this, and a page that says nothing gets it. */
static int   root_px = 16;

/* --- the pictures -------------------------------------------------------
 *
 * Fetched after the page is parsed and before it is laid out, because the
 * layout has to know how big each one is to leave room for it. One
 * connection at a time is all this kernel's TCP does, so they arrive one
 * after another and a page of many pictures is slow — visibly so, and
 * honestly so, rather than appearing to hang.
 *
 * A picture that will not decode is not an error: the element falls back to
 * its alt text, which is what that text is for. Only the count of what was
 * skipped is worth saying.
 */
#define PICS_MAX 24

typedef struct {
    int     node;                /* which img element */
    picture pic;                 /* its pixels, or nothing */
} shown;

static shown  pics[PICS_MAX];
static int    npics;
static limage pic_sizes[PICS_MAX];
static int    npic_sizes;
static int    pics_skipped;

static void pics_drop(void) {
    for (int i = 0; i < npics; i++) picture_free(&pics[i].pic);
    npics = 0;
    npic_sizes = 0;
    pics_skipped = 0;
}

static const picture *pic_of(int node) {
    for (int i = 0; i < npics; i++)
        if (pics[i].node == node && pics[i].pic.rgb) return &pics[i].pic;
    return 0;
}

/* --- what is being typed into ---------------------------------------------
 *
 * A control holds its value in the document, as the attribute a page would
 * have written it in, so that there is one answer to what is in a field and
 * everything reads it from the same place: the layout sizes the box, the
 * drawing writes the value into it, a script that asks gets what is on the
 * screen, and submitting sends exactly what can be seen.
 *
 * Keeping it anywhere else means two answers that agree until somebody
 * types.
 */
static int focus_node = -1;

/* The same editor the address bar uses, pointed at whichever control has the
   keyboard. One of them, rather than one per field: only one can be typed
   into, and the value is written back to the document on every keystroke, so
   there is nothing to keep for the others. */
static char     focus_buf[1024];
static ui_field focus_field = { focus_buf, sizeof(focus_buf), 0, 0, 0 };

static int field_checked(int el) {
    const char *v = dom_attr(&doc, el, "checked");
    return v && !w_same(v, "0");
}

/* A checkbox written with a bare `checked` has an empty value, which is not
   the same as not being there; unchecking one has to leave something
   behind, so it leaves a nought. */
static void field_set_checked(int el, int on) {
    dom_attr_set(&doc, el, "checked", on ? "1" : "0");
}

static const char *field_value(int el, int kind) {
    return lay_control_label(&doc, el, kind);
}

static void field_set_value(int el, const char *v) {
    dom_attr_set(&doc, el, "value", v);
}

/* The form an element is in, or -1. Walked up rather than looked up: a page
   may name a form anywhere and nest one nowhere, and the enclosing element
   is what the markup actually says. */
static int form_of(int el) {
    while (el >= 0) {
        if (doc.nodes[el].kind == DN_ELEMENT && doc.nodes[el].tag == T_FORM)
            return el;
        el = doc.nodes[el].parent;
    }
    return -1;
}

static void focus_control(int el) {
    focus_node = el;
    focus_field.focused = 0;
    focus_buf[0] = 0;
    focus_field.len = 0;
    focus_field.cursor = 0;
    if (el < 0) return;

    int ck = lay_control_kind(&doc, el);
    if (ck != CTL_TEXT && ck != CTL_PASSWORD && ck != CTL_AREA) return;

    w_copy(focus_buf, sizeof(focus_buf), field_value(el, ck),
           sizeof(focus_buf));
    focus_field.len = w_len(focus_buf);
    focus_field.cursor = focus_field.len;
    focus_field.focused = 1;
}

/* --- sending a form -------------------------------------------------------
 *
 * Everything a form holds, named and escaped, in document order, which is
 * the order a server is entitled to expect.
 *
 * A control with no name sends nothing, which is how a page marks the boxes
 * that are for the reader rather than for the server. A checkbox that is
 * not ticked sends nothing at all rather than sending "off": the absence is
 * the message, and a server reading a ticked box and an unticked one as two
 * different values of the same key is the one thing this must not do.
 */
static char go_to[URL_TEXT];
static int  load_post;
static char post_body[4096];
static int  want_go;
static int  go_is_post;

static void url_encode_into(char *out, int cap, int *at, const char *v) {
    static const char *hex = "0123456789ABCDEF";
    for (const char *q = v; *q && *at < cap - 4; q++) {
        unsigned char c = (unsigned char)*q;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~')
            out[(*at)++] = (char)c;
        else if (c == ' ')
            out[(*at)++] = '+';
        else {
            out[(*at)++] = '%';
            out[(*at)++] = hex[c >> 4];
            out[(*at)++] = hex[c & 15];
        }
    }
    out[*at] = 0;
}

static int form_query(int form, char *out, int cap) {
    int at = 0;
    out[0] = 0;
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT) continue;

        int ck = lay_control_kind(&doc, i);
        if (ck == CTL_NONE || ck == CTL_BUTTON) continue;
        if (form_of(i) != form) continue;

        const char *name = dom_attr(&doc, i, "name");
        if (!name || !*name) continue;
        if ((ck == CTL_CHECK || ck == CTL_RADIO) && !field_checked(i)) continue;

        /* Copied out before anything else is read: the value of a textarea
           comes back in a buffer that the next read of any label reuses. */
        char keep[1024];
        if (ck == CTL_CHECK || ck == CTL_RADIO) {
            const char *vv = dom_attr(&doc, i, "value");
            w_copy(keep, sizeof(keep), vv && *vv ? vv : "on", sizeof(keep));
        } else {
            w_copy(keep, sizeof(keep), field_value(i, ck), sizeof(keep));
        }

        if (at && at < cap - 1) out[at++] = '&';
        url_encode_into(out, cap, &at, name);
        if (at < cap - 1) out[at++] = '=';
        url_encode_into(out, cap, &at, keep);
    }
    out[at] = 0;
    return at;
}

static void submit_form(int form) {
    if (form < 0) return;

    char query[sizeof(post_body)];
    form_query(form, query, (int)sizeof(query));

    const char *action = dom_attr(&doc, form, "action");
    const char *method = dom_attr(&doc, form, "method");
    int post = method && lay_same_fold(method, "post");

    /* A form with no action goes back to the page it is on, which is what
       the specification says and what a search box on a site relies on. */
    url_t target;
    if (action && *action) {
        if (!url_join(&here, action, &target)) return;
    } else {
        url_copy(&target, &here);
    }

    url_text(&target, go_to, sizeof(go_to));

    /* Whatever query the current address had is the previous answer's, not
       this form's, and carrying it would send both. */
    for (int i = 0; go_to[i]; i++)
        if (go_to[i] == '?') { go_to[i] = 0; break; }

    go_is_post = post;
    post_body[0] = 0;
    if (post) {
        w_copy(post_body, sizeof(post_body), query, sizeof(post_body));
    } else if (query[0]) {
        int n = w_len(go_to);
        if (n < (int)sizeof(go_to) - 2) {
            go_to[n++] = '?';
            go_to[n] = 0;
            w_copy(go_to + n, (int)sizeof(go_to) - n, query,
                   (int)sizeof(go_to) - n);
        }
    }
    want_go = 1;
}

/* What the page's own scripts did, for the status line to mention. */
static int   scripts_ran;
static int   scripts_changed;      /* one of them wrote to the document */
static char  script_err[128];
/* A handler that threw is worth saying once, and saying it on every click
   after that would bury whatever the status line was for. */
static int   said_script_err;

/* --- saying what happened ------------------------------------------------- */

static void say(const char *a, const char *b) {
    int n = 0;
    for (const char *p = a; *p && n < (int)sizeof(status) - 1; p++) status[n++] = *p;
    if (b) for (const char *p = b; *p && n < (int)sizeof(status) - 1; p++) status[n++] = *p;
    status[n] = 0;
}

static void say_more(const char *s) {
    int n = 0;
    while (status[n]) n++;
    for (const char *p = s; *p && n < (int)sizeof(status) - 1; p++) status[n++] = *p;
    status[n] = 0;
}

static const char *why(int rc) {
    switch (rc) {
        case WEB_ERR_SCHEME:  return "that is not an address this can fetch";
        case WEB_ERR_DOWN:    return "this machine has no address. It asks for "
                                     "one at startup, so either there is no "
                                     "card or nothing answered; the network "
                                     "panel next to the clock will ask again.";
        case WEB_ERR_RESOLVE: return "that name did not turn into an address. "
                                     "Either it does not exist, or the name "
                                     "server is not answering.";
        case WEB_ERR_BUSY:    return "something else is using the one "
                                     "connection this machine has";
        case WEB_ERR_TLS:     return "the connection would not prove who it was";
        case WEB_ERR_CONNECT: return "could not connect to that host";
        case WEB_ERR_SEND:    return "the request could not be sent";
        case WEB_ERR_EMPTY:   return "the server said nothing";
        case WEB_ERR_HEADERS: return "the answer was not http";
        default:              return "the fetch failed";
    }
}

/* The heading above the reason. A refused certificate and a machine that
   never got as far as sending a packet are different enough that giving them
   the same heading is how somebody ends up looking at the wrong thing. */
static const char *why_heading(int rc) {
    switch (rc) {
        case WEB_ERR_DOWN:    return "No network address";
        case WEB_ERR_RESOLVE: return "That name did not resolve";
        case WEB_ERR_TLS:     return "This connection was refused";
        default:              return "Cannot show this page";
    }
}

/* --- building the page ---------------------------------------------------
 *
 * Four passes over one tree, in this order because each needs the last: the
 * tree, then every style sheet that applies to it, then an index of those
 * rules, then the layout.
 */

/* Every declaration an element carries in its own style attribute, parsed
   once here instead of on every layout. A resize lays the page out again,
   and re-parsing the same declarations on every frame of a window drag is
   the difference between a reflow and a stutter. */
static void gather_inline_styles(void) {
    for (int i = 0; i < doc.count; i++) {
        inl[i].at = 0;
        inl[i].n = 0;
        if (doc.nodes[i].kind != DN_ELEMENT) continue;
        const char *st = dom_attr(&doc, i, "style");
        if (!st || !*st) continue;
        int at = sheet.ndecls;
        /* The same reader as a block between braces, given a run with no
           braces around it. */
        int p = 0, len = w_len(st);
        while (p < len) {
            while (p < len && css_space(st[p])) p++;
            int ns = p;
            while (p < len && st[p] != ':' && st[p] != ';') p++;
            int nl = p - ns;
            while (nl > 0 && css_space(st[ns + nl - 1])) nl--;
            if (p >= len || st[p] != ':') {
                while (p < len && st[p] != ';') p++;
                if (p < len) p++;
                continue;
            }
            p++;
            int vs = p, depth = 0;
            while (p < len) {
                if (st[p] == '(') depth++;
                else if (st[p] == ')') { if (depth) depth--; }
                else if (!depth && st[p] == ';') break;
                p++;
            }
            css_declare(&sheet, st + ns, nl, st + vs, p - vs);
            if (p < len) p++;
        }
        inl[i].at = at;
        inl[i].n = sheet.ndecls - at;
    }
}

/* The sheets the page carries itself: every style element, in order. */
static void gather_inline_sheets(void) {
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_STYLE)
            continue;
        int t = doc.nodes[i].first;
        while (t >= 0) {
            if (doc.nodes[t].kind == DN_TEXT && doc.nodes[t].text >= 0) {
                const char *s = doc.arena + doc.nodes[t].text;
                css_parse(&sheet, s, w_len(s));
            }
            t = doc.nodes[t].next;
        }
    }
}

/* And the sheets it links to, which on a modern page is nearly all of them.
 *
 * Fetched here rather than skipped, because a page whose entire appearance
 * is in one linked file and which is shown without it is not the page. Each
 * one is another round trip, so there is a limit on how many are followed
 * and the limit is said out loud when it is reached rather than leaving
 * somebody wondering why one part of a page is styled and the rest is not. */
static int gather_linked_sheets(int *fetched, int *skipped) {
    *fetched = *skipped = 0;
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_LINK)
            continue;
        const char *rel = dom_attr(&doc, i, "rel");
        const char *href = dom_attr(&doc, i, "href");
        if (!rel || !href || !*href) continue;

        /* rel can be a list, and "stylesheet alternate" is one this should
           not take: an alternate sheet is one the reader has not chosen. */
        if (!w_same_fold(rel, "stylesheet")) continue;

        if (*fetched >= SHEETS_MAX) { (*skipped)++; continue; }

        url_t u;
        if (!url_join(&here, href, &u)) { (*skipped)++; continue; }

        response_t r;
        int rc = web_get(&u, cssbuf, CSS_MAX, &r);
        if (rc < 200 || rc >= 300 || r.len <= 0) { (*skipped)++; continue; }
        css_parse(&sheet, r.body, r.len);
        (*fetched)++;
    }
    return *fetched;
}

static void relayout(int width) {
    match.hover = hover_node;
    match.visited_links = 0;
    lay_run(&page, &doc, &sheet, &index_, &match, inl, pic_sizes, npic_sizes, width, root_px);
}

/* What this system thinks a link looks like, which is the accent the rest of
   the desktop uses.
 *
 * Parsed after the browser's own defaults and before anything the page says,
 * so it beats the default and loses to the page. A site that has chosen its
 * link colour has chosen it; a site that has not gets the one colour this
 * machine uses everywhere else for the thing you can press. */
static void accent_sheet(void) {
    ui_theme t = ui_load_theme();
    char rule[64];
    static const char hex[] = "0123456789abcdef";
    int n = 0;
    const char *head = "a{color:#";
    for (const char *p = head; *p; p++) rule[n++] = *p;
    u32 c = t.accent;
    for (int shift = 20; shift >= 0; shift -= 4)
        rule[n++] = hex[(c >> shift) & 0xF];
    const char *tail = "}";
    for (const char *p = tail; *p; p++) rule[n++] = *p;
    rule[n] = 0;
    css_parse(&sheet, rule, n);
}

/* Every img in the document, fetched and decoded in the order they appear.
 *
 * The buffer is the one the style sheets use: by this point every sheet has
 * been parsed into the cascade and what is in it is no longer needed, and a
 * second buffer of this size is a megabyte that is idle on every page
 * without a picture on it. */
static void gather_pictures(void) {
    for (int i = 0; i < doc.count && npics < PICS_MAX; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_IMG)
            continue;

        const char *src = dom_attr(&doc, i, "src");
        if (!src || !*src) continue;

        url_t u;
        if (!url_join(&here, src, &u)) { pics_skipped++; continue; }

        response_t r;
        int rc = web_get(&u, cssbuf, CSS_MAX, &r);
        if (rc < 0 || rc >= 400 || r.len <= 0) { pics_skipped++; continue; }

        shown *s = &pics[npics];
        s->node = i;

        /* Which of the three it is, from the bytes rather than from what the
           server said it was. A server that labels a PNG as an octet stream
           is common; a PNG that does not start with the PNG signature is
           not, so the bytes are the better authority. */
        const u8 *body = (const u8 *)r.body;
        int ok = 0;

        if (r.len > 8 && body[0] == 137 && body[1] == 'P'
            && body[2] == 'N' && body[3] == 'G') {
            ok = png_decode(body, r.len, &s->pic, 0xFFFFFF) == PNG_OK;
        } else if (r.len > 3 && body[0] == 0xFF && body[1] == 0xD8) {
            ok = jpeg_decode(body, r.len, &s->pic) == JPG_OK;
        } else {
            /* A drawing, which is markup and so can start with an XML
               declaration, a comment, or the element itself. */
            int at = 0;
            while (at < r.len && (body[at] == ' ' || body[at] == '\n'
                                  || body[at] == '\r' || body[at] == '\t')) at++;
            if (at < r.len && body[at] == '<') {
                /* What the page asked for, so a drawing lands at the size
                   the layout is about to leave for it rather than at
                   whatever size it happens to describe. */
                const char *aw = dom_attr(&doc, i, "width");
                const char *ah = dom_attr(&doc, i, "height");
                int want_w = aw ? lay_number(aw) : 0;
                int want_h = ah ? lay_number(ah) : 0;
                ok = svg_render((const char *)body, r.len, want_w, want_h,
                                &s->pic, 0xFFFFFF) == SVG_OK;
            }
        }

        if (!ok) { pics_skipped++; continue; }

        pic_sizes[npic_sizes].node = i;
        pic_sizes[npic_sizes].w = s->pic.w;
        pic_sizes[npic_sizes].h = s->pic.h;
        npic_sizes++;
        npics++;
    }
}

static void build(const char *html, int len, int width, int want_sheets,
                  int *fetched, int *skipped) {
    /* Before the tree it is bound to is taken apart under it. */
    jsdom_close();
    dom_parse(&doc, html, len);

    css_init(&sheet);
    css_parse(&sheet, CSS_UA, (int)sizeof(CSS_UA) - 1);
    accent_sheet();
    gather_inline_sheets();
    if (want_sheets) gather_linked_sheets(fetched, skipped);
    else { *fetched = 0; *skipped = 0; }
    gather_inline_styles();
    css_index(&sheet, &index_);

    /* The pictures, before the layout so it can leave room for them. */
    pics_drop();
    if (want_sheets) gather_pictures();

    /* And then whatever the page brought with it, before any of it is laid
       out: a script that writes to an element is writing to the document
       the layout is about to read, so running them afterwards would show
       the page as it was and correct it a frame later. */
    focus_control(-1);
    script_err[0] = 0;
    said_script_err = 0;
    scripts_ran = 0;
    scripts_changed = 0;
    if (jsdom_open(&doc)) {
        scripts_ran = jsdom_scripts(script_err, (int)sizeof(script_err));
        jsdom_loaded();
        scripts_changed = jsdom_changed();
    }

    hover_node = -1;
    relayout(width);

    if (doc.title >= 0) w_copy(title, sizeof(title), doc.arena + doc.title,
                               sizeof(title));
    else title[0] = 0;
}

/* Builds a page of our own, for when there is nothing to show. Written as
   html and put through the same reader, so the one path that draws anything
   is the path that is used. */
static void show_message(const char *heading, const char *body, int width) {
    int n = 0;
    const char *bits[8] = {
        "<style>body{padding:28px 32px;max-width:640px}"
        "h1{font-size:1.7em;color:#333}p{color:#555;line-height:1.55}</style>"
        "<h1>", heading, "</h1><p>", body, "</p>", 0 };
    for (int i = 0; bits[i] && n < SRC_MAX - 1; i++)
        for (const char *p = bits[i]; *p && n < SRC_MAX - 1; p++) src[n++] = *p;
    src[n] = 0;
    int f, sk;
    build(src, n, width, 0, &f, &sk);
    scroll = 0;
}

/* Plain text, shown as plain text: one preformatted block, which is what it
   is, rather than run through a markup reader that would eat the indentation
   and every angle bracket in it. */
static void show_plain(const char *body, int len, int width) {
    int n = 0;
    const char *head = "<style>body{padding:16px}"
                       "pre{font-size:14px;line-height:1.45}</style><pre>";
    for (const char *p = head; *p && n < SRC_MAX - 1; p++) src[n++] = *p;
    for (int i = 0; i < len && n < SRC_MAX - 8; i++) {
        char c = body[i];
        if (c == '<') { const char *e = "&lt;"; while (*e) src[n++] = *e++; }
        else if (c == '&') { const char *e = "&amp;"; while (*e) src[n++] = *e++; }
        else src[n++] = c;
    }
    const char *tail = "</pre>";
    for (const char *p = tail; *p && n < SRC_MAX - 1; p++) src[n++] = *p;
    src[n] = 0;
    int f, sk;
    build(src, n, width, 0, &f, &sk);
}

static void number_into(char *out, int v) {
    char tmp[16];
    int t = 0;
    if (!v) tmp[t++] = '0';
    while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    int w = 0;
    while (t) out[w++] = tmp[--t];
    out[w] = 0;
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

    /* A form sent with POST is the one fetch that carries something, and
       it is spent once: going back to it afterwards asks again with GET
       rather than sending the form a second time. */
    int rc;
    if (load_post) {
        load_post = 0;
        rc = web_post(&here, post_body, src, SRC_MAX, &reply);
    } else {
        rc = web_get(&here, src, SRC_MAX, &reply);
    }
    if (rc < 0) {
        /* A refused certificate has a reason worth reading, and it is the
           one kind of failure where the difference between "expired" and
           "for a different site" is the whole story. Anything that failed
           before the handshake has no such reason, and printing the empty
           one said "refused: no error" on a machine that had no address. */
        if (rc == WEB_ERR_TLS && reply.how[0]) {
            show_message(why_heading(rc), reply.how, width);
            say("refused: ", reply.how);
        } else {
            show_message(why_heading(rc), why(rc), width);
            say(why(rc), 0);
        }
        title[0] = 0;
        return;
    }

    int plain = w_starts_fold(reply.ctype, "text/plain")
             || w_starts_fold(reply.ctype, "application/json");

    int fetched = 0, skipped = 0;
    if (plain) {
        show_plain(reply.body, reply.len, width);
        w_copy(title, sizeof(title), here.path, sizeof(title));
    } else {
        build(reply.body, reply.len, width, 1, &fetched, &skipped);
    }

    if (!keep_scroll) scroll = 0;

    char shown[16];
    number_into(shown, page.nlinks);

    if (rc >= 400) say("the server said this page is not there", 0);
    else if (reply.truncated || page.overflowed || doc.overflowed)
        say("shown as far as it fits: the page is bigger than this can hold", 0);
    else say(shown, page.nlinks == 1 ? " link on this page"
                                     : " links on this page");

    if (fetched) {
        char n[16];
        number_into(n, fetched);
        say_more(", ");
        say_more(n);
        say_more(fetched == 1 ? " style sheet" : " style sheets");
    }
    if (skipped) say_more(" (more were not read)");

    /* And what the page's own scripts did. A script that threw is worth
       saying out loud: the page will look like the one it was before it
       ran, and without this there is nothing to tell the two apart. */
    if (npics) {
        char n[16];
        number_into(n, npics);
        say_more(", ");
        say_more(n);
        say_more(npics == 1 ? " picture" : " pictures");
        if (pics_skipped) say_more(" (more would not show)");
    } else if (pics_skipped) {
        say_more(", no picture on it would show");
    }

    if (script_err[0]) {
        say_more(", a script stopped: ");
        say_more(script_err);
    } else if (scripts_ran) {
        char n[16];
        number_into(n, scripts_ran);
        say_more(", ");
        say_more(n);
        say_more(scripts_ran == 1 ? " script ran" : " scripts ran");
    }

    /* Whether anybody in between could have read it, said either way.
       Marking only the encrypted case trains people to read a missing mark
       as nothing in particular, and the case worth noticing is the other
       one. */
    say_more(reply.secure ? ", encrypted" : ", NOT encrypted");
}

static void push_history(const char *address) {
    if (hist_at >= 0 && hist_at < hist_n) hist[hist_at].scroll = scroll;
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

/* The same blitter as draw.h's, against the browser's own larger set of
   faces. Written out rather than shared because draw.h's table is the one
   every other program carries and this one is thirty three faces. */
static void tface_draw(surface *s, int x, int y, const char *str, u32 fg,
                       int which) {
    const face_t *f = tface_of(which);
    int baseline = y + (f->size * 4) / 5;
    for (; *str; str++) {
        unsigned char c = (unsigned char)*str;
        if (c < FACE_FIRST || c > FACE_LAST) c = ' ';
        const face_glyph *g = &f->glyphs[c - FACE_FIRST];
        const unsigned char *px = f->pixels + g->at;
        for (int gy = 0; gy < g->h; gy++) {
            int sy = baseline - g->top + gy;
            if (sy < 0 || sy >= s->h) continue;
            for (int gx = 0; gx < g->w; gx++) {
                unsigned char a = px[gy * g->w + gx];
                if (!a) continue;
                int sx = x + g->left + gx;
                if (sx < 0 || sx >= s->w) continue;
                u32 *slot = &s->px[(u32)sy * s->w + sx];
                *slot = (a == 255) ? fg : mix(*slot, fg, a);
            }
        }
        x += g->advance;
    }
}

static void draw_page(surface *s, int ox, int oy, int vw, int vh) {
    for (int i = 0; i < page.nitems; i++) {
        const litem *it = &page.items[i];
        int y = it->y - scroll;
        if (y + it->h < -8 || y > vh + 8) continue;
        int x = ox + it->x;
        int sy = oy + y;

        if (it->kind == LK_BOX) {
            int w = it->w, h = it->h;
            if (w > vw) w = vw;
            if (it->has_bg) {
                if (it->radius) round_rect(s, x, sy, w, h, it->radius, it->bg);
                else rect(s, x, sy, w, h, it->bg);
            }
            if (it->bt) rect(s, x, sy, w, it->bt, it->border);
            if (it->bb) rect(s, x, sy + h - it->bb, w, it->bb, it->border);
            if (it->bl) rect(s, x, sy, it->bl, h, it->border);
            if (it->br) rect(s, x + w - it->br, sy, it->br, h, it->border);
            continue;
        }

        if (it->kind == LK_IMAGE) {
            const picture *p = pic_of(it->node);
            if (p && p->rgb && it->w > 0 && it->h > 0) {
                /* Nearest neighbour, chosen rather than settled for. A
                   picture on a page is usually drawn at or near its own
                   size, where every filter agrees; where it is not, the
                   difference is a page that draws now against one that
                   draws in a moment. */
                for (int row = 0; row < it->h; row++) {
                    int dy = sy + row;
                    if (dy < oy || dy >= oy + vh) continue;
                    int src_y = row * p->h / it->h;
                    for (int col = 0; col < it->w; col++) {
                        int dx = x + col;
                        if (dx < ox || dx >= ox + vw) continue;
                        if (dx < 0 || dx >= s->w) continue;
                        const u8 *q = p->rgb + ((src_y * p->w)
                                                + (col * p->w / it->w)) * 3;
                        s->px[(u32)dy * s->w + dx] =
                            ((u32)q[0] << 16) | ((u32)q[1] << 8) | q[2];
                    }
                }
            }
            continue;
        }

        if (it->kind == LK_FIELD) {
            int ck = lay_control_kind(&doc, it->node);
            int focused = (it->node == focus_node);
            u32 edge = focused ? 0x3B6FD6 : 0xA9A9A9;
            int w = it->w, h = it->h;

            /* A page that styles its fields gets the fields it styled.
               Without a colour of its own a text box is paper and a button
               is the colour of a button. */
            u32 inside = it->has_bg ? it->bg
                       : (ck == CTL_BUTTON ? 0xE6E6EA : 0xFFFFFF);

            if (ck == CTL_CHECK || ck == CTL_RADIO) {
                rect(s, x, sy, w, h, inside);
                rect(s, x, sy, w, 1, edge);
                rect(s, x, sy + h - 1, w, 1, edge);
                rect(s, x, sy, 1, h, edge);
                rect(s, x + w - 1, sy, 1, h, edge);
                if (field_checked(it->node)) {
                    if (ck == CTL_RADIO) disc(s, x + w / 2, sy + h / 2,
                                              w / 4, 0x1A1A1A);
                    else rect(s, x + 3, sy + 3, w - 6, h - 6, 0x1A1A1A);
                }
                continue;
            }

            rect(s, x, sy, w, h, inside);
            rect(s, x, sy, w, 1, edge);
            rect(s, x, sy + h - 1, w, 1, edge);
            rect(s, x, sy, 1, h, edge);
            rect(s, x + w - 1, sy, 1, h, edge);

            /* Read out of the document now rather than held from when the
               page was laid out, because typing changes it and typing does
               not lay the page out again. */
            const char *val = field_value(it->node, ck);
            char shown[192];
            int n = 0;
            if (ck == CTL_PASSWORD) {
                for (const char *q = val; *q && n < (int)sizeof(shown) - 1; q++)
                    shown[n++] = '*';
            } else {
                for (const char *q = val; *q && n < (int)sizeof(shown) - 1; q++)
                    shown[n++] = (*q == '\n' || *q == '\r') ? ' ' : *q;
            }
            shown[n] = 0;

            /* The end of it rather than the start: somebody typing wants to
               see what they are typing, and a field that shows the first
               twenty characters of what they wrote is one they cannot use. */
            int room = w - 10;
            int from = 0;
            while (from < n && tface_wn(shown + from, n - from, it->face) > room)
                from++;

            int th = tface_h(it->face);
            int ty = sy + (h - th) / 2;
            if (ck == CTL_AREA) ty = sy + 4;
            int tx = x + 5;
            if (ck == CTL_BUTTON)
                tx = x + (w - tface_wn(shown + from, n - from, it->face)) / 2;

            tface_draw(s, tx, ty, shown + from, 0x1A1A1A, it->face);

            if (focused && ck != CTL_BUTTON) {
                int cx = tx + tface_wn(shown + from, n - from, it->face);
                if (cx > x + w - 3) cx = x + w - 3;
                rect(s, cx + 1, ty, 1, th, 0x1A1A1A);
            }
            continue;
        }

        if (it->kind == LK_BULLET) {
            if (it->at >= 0)
                tface_draw(s, x, sy, page.text + it->at, it->color, it->face);
            else
                disc(s, x + 6, sy + it->h / 2, 3, it->color);
            continue;
        }

        if (it->at < 0) continue;
        const char *str = page.text + it->at;

        u32 col = it->color;
        /* The link under the pointer, and only that one. A whole page of
           links changing colour at once is what happens when the hover is
           tracked by href rather than by which run it is. */
        if (it->link >= 0 && it->link == over_link) col = 0x0842A0;

        tface_draw(s, x, sy, str, col, it->face);

        if (it->under || (it->link >= 0 && it->link == over_link)) {
            /* Carried across the space to the next word when that word is
               the same link. A link underlined word by word looks like
               several links, which is what it looked like. */
            int uw = it->w;
            if (i + 1 < page.nitems && page.items[i + 1].link == it->link
                && it->link >= 0 && page.items[i + 1].y == it->y
                && page.items[i + 1].x > it->x)
                uw = page.items[i + 1].x - it->x;
            rect(s, x, sy + it->h - 1, uw, 1, col);
        }
        if (it->strike)
            rect(s, x, sy + it->h / 2, it->w, 1, col);
    }
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
 * everything but drawing: there is nothing here that can show one yet.
 *
 * It used to be set when the bar became focused, which is not the same
 * thing and is wrong in the ordinary case: the bar keeps the keyboard after
 * an address is entered, so the second address somebody types is clicked
 * into a field that already has it, no transition happens, and what they
 * type is appended to what was there. Two addresses run together into one
 * and the page does not change. */
static int bar_fresh;

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
    int last_hover = -2;
    int dirty = 1;              /* something changed and a frame is owed */

    if (have_arg) set_address(arg);
    else set_address("https://example.com/");
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

            /* One frame holds sixteen keys and this loop drains the whole
               queue, so everything past the sixteenth used to be read out
               of the queue and thrown away. Nothing reports that: what it
               looks like is an address bar that loses most of a long
               address, which reads as the keyboard or the network rather
               than as this.
             *
             * Left in the queue instead. There is a frame owed already --
             * keys arrived -- and the next pass takes the next sixteen. */
            if (in.nkeys >= UI_KEYS) break;
        }
        if (closing) break;

        /* A reflow costs a pass over the whole page, so it happens once the
           dragging has stopped rather than on every frame of it. */
        if (want_width && !in.down) {
            relayout(view_w - UI_PAD * 2);
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
            } else if (focus_node >= 0) {
                int ck = lay_control_kind(&doc, focus_node);
                if (k == 27) {
                    focus_control(-1);
                } else if (k == '\n' && ck != CTL_AREA) {
                    /* Return in a field sends the form it is in, which is
                       how a search box has always worked and the only way
                       to use one that has no button beside it. */
                    int f = form_of(focus_node);
                    focus_control(-1);
                    submit_form(f);
                } else if (ck == CTL_CHECK || ck == CTL_RADIO) {
                    if (k == ' ')
                        field_set_checked(focus_node,
                                          !field_checked(focus_node));
                } else {
                    ui_field_key(&focus_field, raw);
                    field_set_value(focus_node, focus_buf);
                }
                dirty = 1;
            } else {
                int pg = view_h - 40;
                if (k == KEY_DOWN)       scroll += 40;
                else if (k == KEY_UP)    scroll -= 40;
                else if (k == KEY_PAGE_DOWN) scroll += pg;
                else if (k == KEY_PAGE_UP)   scroll -= pg;
                else if (k == KEY_HOME)  scroll = 0;
                else if (k == KEY_END)   scroll = page.height;
                else if (k == ' ')       scroll += pg;
                else if (k == '+' || k == '=') {
                    if (root_px < 28) { root_px += 2; want_width = 1; laid_for = -1; }
                } else if (k == '-') {
                    if (root_px > 10) { root_px -= 2; want_width = 1; laid_for = -1; }
                } else if (k == KEY_LEFT && hist_at > 0) {
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

        /* Anything the page asked to have done later. A page that calls
           setTimeout and is never called back is not slow: it is stopped
           part of the way through whatever it was doing. */
        if (jsdom_live() && jsdom_timers() && jsdom_changed()) {
            relayout(view_w - UI_PAD * 2);
            dirty = 1;
        }

        /* Where a form asked to go, once the click or the key that sent
           it has been dealt with. */
        if (want_go) {
            want_go = 0;
            load_post = go_is_post;
            set_address(go_to);
            push_history(go_to);
            want_load = 1;
            dirty = 1;
        }

        scroll -= scrolled * 48;

        int limit = page.height - view_h;
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
           being drawn rather than on every pass of this loop. */
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
        int node_under = -1;
        if (in.my > view_y && in.my < view_y + view_h) {
            over_link = lay_link_at(&page, dx, dy);
            node_under = lay_node_at(&page, dx, dy);
        }

        /* :hover is a style, so an element coming under the pointer changes
           what the page looks like and the page has to be laid out again.
           Only when the element actually changed: doing it per frame lays
           out a whole document sixty times a second for a pointer that has
           not left the word it was on. */
        if (node_under != last_hover) {
            last_hover = node_under;
            hover_node = node_under;
            relayout(view_w - UI_PAD * 2);
            over_link = lay_link_at(&page, dx, dy);
        }

        /* A click goes to the page before it goes to the browser.
         *
         * Which order that happens in is the whole behaviour of a menu, a
         * tab strip and every link that is really a button: the page gets
         * to say the ordinary consequence should not follow, and if it says
         * so, the link under the pointer is not followed. A browser that
         * navigated first would run the handler on a page that was already
         * leaving. */
        if (in.released && node_under >= 0 && jsdom_live()) {
            int stop = jsdom_click(node_under);

            /* A handler that changed the document changed what is on the
               screen, and nothing else in this loop would notice: the
               layout is rebuilt on a resize, a hover or a load, and a click
               is none of those. */
            if (jsdom_changed()) {
                relayout(view_w - UI_PAD * 2);
                over_link = lay_link_at(&page, dx, dy);
                dirty = 1;
            }
            if (jsdom_error()[0] && !said_script_err) {
                said_script_err = 1;
                say("a script stopped: ", jsdom_error());
            }
            if (stop) in.released = 0;
        }

        /* A control takes the click before a link does. It comes after the
           page has had it, so a handler that says the ordinary thing should
           not happen has already cleared the release and neither the field
           nor the link sees it. */
        if (in.released && node_under >= 0) {
            int ck = lay_control_kind(&doc, node_under);
            if (ck != CTL_NONE && ck != CTL_HIDDEN) {
                in.released = 0;
                bar.focused = 0;
                dirty = 1;

                if (ck == CTL_CHECK) {
                    field_set_checked(node_under, !field_checked(node_under));
                    focus_control(node_under);
                } else if (ck == CTL_RADIO) {
                    /* One of a name at a time, which is the only thing that
                       makes a radio button different from a checkbox. */
                    const char *nm = dom_attr(&doc, node_under, "name");
                    int mine = form_of(node_under);
                    if (nm && *nm)
                        for (int i = 0; i < doc.count; i++) {
                            if (lay_control_kind(&doc, i) != CTL_RADIO) continue;
                            if (form_of(i) != mine) continue;
                            const char *o = dom_attr(&doc, i, "name");
                            if (o && w_same(o, nm)) field_set_checked(i, 0);
                        }
                    field_set_checked(node_under, 1);
                    focus_control(node_under);
                } else if (ck == CTL_BUTTON) {
                    const char *t = dom_attr(&doc, node_under, "type");
                    focus_control(-1);
                    if (!(t && lay_same_fold(t, "reset")))
                        submit_form(form_of(node_under));
                } else {
                    focus_control(node_under);
                }
            }
        }

        /* Clicking anywhere else puts the field down, so that what is typed
           next scrolls the page rather than going into a box nobody is
           looking at. */
        if (in.released && focus_node >= 0
            && (node_under < 0
                || lay_control_kind(&doc, node_under) == CTL_NONE))
            focus_control(-1);

        if (over_link >= 0 && in.released) {
            in.released = 0;
            url_t next;
            if (url_join(&here, page.text + page.links[over_link].href, &next)) {
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
        if (ui_field_draw(&s, &in, &t, bx, UI_PAD, field_w, &bar,
                          "type an address"))
            bar_fresh = 1;
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
                if (url_join(&here, page.text + page.links[over_link].href, &u)) {
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
            surface pg = { px, w, view_y + view_h - 2 };
            draw_page(&pg, view_x + UI_PAD, view_y + 2, view_w, view_h - 4);
        }
        ui_sunken(&s, &t, view_x, view_y, view_w + UI_SCROLL_W, view_h);
        ui_scrollbar(&s, &t, view_x + view_w + 2, view_y + 2, view_h - 4,
                     scroll, view_h,
                     page.height < view_h ? view_h : page.height);

        win_commit(win);
        sleep_ms(16);
    }

    win_close(win);
    exit(0);
}
