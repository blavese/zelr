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

/* What the page's own scripts did, for the status line to mention. */
static int   scripts_ran;
static int   scripts_changed;      /* one of them wrote to the document */
static char  script_err[128];

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
    lay_run(&page, &doc, &sheet, &index_, &match, inl, width, root_px);
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

static void build(const char *html, int len, int width, int want_sheets,
                  int *fetched, int *skipped) {
    dom_parse(&doc, html, len);

    css_init(&sheet);
    css_parse(&sheet, CSS_UA, (int)sizeof(CSS_UA) - 1);
    accent_sheet();
    gather_inline_sheets();
    if (want_sheets) gather_linked_sheets(fetched, skipped);
    else { *fetched = 0; *skipped = 0; }
    gather_inline_styles();
    css_index(&sheet, &index_);

    /* And then whatever the page brought with it, before any of it is laid
       out: a script that writes to an element is writing to the document
       the layout is about to read, so running them afterwards would show
       the page as it was and correct it a frame later. */
    script_err[0] = 0;
    scripts_ran = jsdom_run(&doc, script_err, (int)sizeof(script_err),
                            &scripts_changed);

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

    int rc = web_get(&here, src, SRC_MAX, &reply);
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
