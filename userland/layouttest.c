/* Where things end up on the page, with no page.
 *
 * The layout turns a styled tree into a list of boxes with coordinates. That
 * list is the whole answer: if the boxes are in the right places the page is
 * right, and whether they then reached the glass is the drawing's business
 * and the browser check's question.
 *
 * So this asks about the coordinates directly, which is both far stricter
 * and far quicker than a screenshot. A row of three boxes is either three
 * boxes at the same height and increasing x, or it is not, and no amount of
 * counting coloured pixels says it as plainly as that.
 *
 * It exists because display:flex used to fall through to block, and every
 * row on every modern page came out as a column.
 */
#include "zelr.h"
#include "alloc.h"
#include "dom.h"
#include "css.h"
#include "layout.h"

static int failed;

static void ok(const char *what, int cond) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    putc('\n');
    if (!cond) failed++;
}

static void okn(const char *what, int cond, int n) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    puts("  ");
    putn(n);
    putc('\n');
    if (!cond) failed++;
}

static ddoc   doc;
static csheet sheet;
static cindex index_;
static cmatch match;
static cinline inl[DOM_NODES];
static ldoc   page;

/* A picture the page is told it has, so the img branch runs without
   anything being fetched. The browser hands the layout a list of these
   after it has decoded what the page pointed at; what the layout does with
   the width and height on the tag is the same either way. */
static limage fake[4];
static int nfake;

static void picture_at(const char *id, int w, int h);
static const char *hover_id;     /* the element under the pointer, for :hover */

/* The same three steps the browser takes, without the fetching. */
static void lay(const char *html, int width) {
    int n = 0;
    while (html[n]) n++;
    dom_parse(&doc, html, n);
    dom_shadows(&doc);

    css_init(&sheet);
    css_parse(&sheet, CSS_UA, (int)sizeof(CSS_UA) - 1);
    sheet.ua_rules = sheet.nrules;

    /* Style sheets written in the page itself. */
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_STYLE)
            continue;
        int child = doc.nodes[i].first;
        if (child >= 0 && doc.nodes[child].kind == DN_TEXT
            && doc.nodes[child].text >= 0) {
            const char *t = doc.arena + doc.nodes[child].text;
            css_parse_style(&sheet, t, w_len(t), -1, -1, dom_attr(&doc, i, "data-zs"));
        }
    }

    for (int i = 0; i < doc.count; i++) { inl[i].at = 0; inl[i].n = 0; }
    css_index(&sheet, &index_);
    match.hover = hover_id ? dom_by_id(&doc, hover_id) : -1;
    match.visited_links = 0;
    lay_run(&page, &doc, &sheet, &index_, &match, inl, fake, nfake,
            width, 16);
}

static void picture_at(const char *id, int w, int h) {
    int node = dom_by_id(&doc, id);
    if (node < 0 || nfake >= 4) return;
    fake[nfake].node = node;
    fake[nfake].w = w;
    fake[nfake].h = h;
    nfake++;
}

/* The box a given element produced, which is the one item that carries its
   node and has a size. */
static const litem *box_of(int node) {
    for (int i = 0; i < page.nitems; i++)
        if (page.items[i].node == node && page.items[i].kind == LK_BOX)
            return &page.items[i];
    return 0;
}

static int by_id(const char *id) { return dom_by_id(&doc, id); }

/* The picture an element was laid out as: an <img>, or a drawing. */
static const litem *image_of(int node) {
    for (int i = 0; i < page.nitems; i++)
        if (page.items[i].node == node && page.items[i].kind == LK_IMAGE)
            return &page.items[i];
    return 0;
}

/* The first run of text that is exactly this word. */
static const litem *word(const char *w) {
    for (int i = 0; i < page.nitems; i++) {
        const litem *it = &page.items[i];
        if (it->kind == LK_TEXT && it->at >= 0 && w_same(page.text + it->at, w)) return it;
    }
    return 0;
}


int main(void) {
    puts("where things end up\n");

    /* --- a row -------------------------------------------------------------
     *
     * Three boxes in a flex container. The question is not whether they
     * exist but whether they are beside each other: same top, increasing
     * left. Stacked, they would have the same left and increasing top,
     * which is exactly what this used to do. */
    {
        lay("<style>"
            ".row{display:flex}"
            ".box{background:#ff0000;padding:4px;width:60px}"
            "</style>"
            "<div class=row>"
            "<div class=box id=a>one</div>"
            "<div class=box id=b>two</div>"
            "<div class=box id=c>three</div>"
            "</div>", 600);

        const litem *a = box_of(by_id("a"));
        const litem *b = box_of(by_id("b"));
        const litem *c = box_of(by_id("c"));

        ok("three boxes in a row all exist", a && b && c);
        if (a && b && c) {
            okn("the second is to the right of the first", b->x > a->x, b->x);
            okn("and the third to the right of the second", c->x > b->x, c->x);
            okn("and all three are at the same height",
                a->y == b->y && b->y == c->y, b->y - a->y);
        }
    }

    /* --- and a column is still a column -------------------------------------
     *
     * The same markup without display:flex, so this says the check above is
     * measuring flex rather than measuring anything at all. */
    {
        lay("<style>.box{background:#ff0000;padding:4px;width:60px}</style>"
            "<div>"
            "<div class=box id=a>one</div>"
            "<div class=box id=b>two</div>"
            "</div>", 600);

        const litem *a = box_of(by_id("a"));
        const litem *b = box_of(by_id("b"));
        ok("without it they are still stacked", a && b);
        if (a && b) {
            okn("the second is below the first", b->y > a->y, b->y);
            okn("and at the same left edge", a->x == b->x, b->x - a->x);
        }
    }

    /* --- a gap between them -------------------------------------------------- */
    {
        lay("<style>.row{display:flex;gap:20px}"
            ".box{background:#ff0000;width:50px}</style>"
            "<div class=row><div class=box id=a>x</div>"
            "<div class=box id=b>y</div></div>", 600);

        const litem *a = box_of(by_id("a"));
        const litem *b = box_of(by_id("b"));
        ok("a row with a gap lays out", a && b);
        if (a && b)
            okn("and the gap is between them",
                b->x - (a->x + a->w) >= 18 && b->x - (a->x + a->w) <= 22,
                b->x - (a->x + a->w));
    }

    /* --- pushed to the far end ------------------------------------------------
     *
     * space-between is what puts a logo at one end of a bar and a menu at
     * the other, which is most of what a page uses flex for at all. */
    {
        lay("<style>.row{display:flex;justify-content:space-between}"
            ".box{background:#ff0000;width:50px}</style>"
            "<div class=row><div class=box id=a>x</div>"
            "<div class=box id=b>y</div></div>", 600);

        const litem *a = box_of(by_id("a"));
        const litem *b = box_of(by_id("b"));
        ok("a row that spreads itself out lays out", a && b);
        if (a && b) {
            okn("the first is at the near end", a->x < 40, a->x);
            okn("and the second at the far one", b->x > 400, b->x);
        }
    }

    /* --- and pulled to the middle --------------------------------------------- */
    {
        lay("<style>.row{display:flex;justify-content:center}"
            ".box{background:#ff0000;width:50px}</style>"
            "<div class=row><div class=box id=a>x</div></div>", 600);

        const litem *a = box_of(by_id("a"));
        ok("a centred row lays out", a != 0);
        if (a) okn("and its one box is near the middle",
                   a->x > 200 && a->x < 340, a->x);
    }

    /* --- one that takes what is left -------------------------------------------
     *
     * flex:1 is how a page says "this one fills the bar". Without it the
     * box is as wide as its words and the bar has a hole in it. */
    {
        lay("<style>.row{display:flex}"
            ".fixed{background:#ff0000;width:50px}"
            ".grow{background:#00ff00;flex:1}</style>"
            "<div class=row><div class=fixed id=a>x</div>"
            "<div class=grow id=b>y</div></div>", 600);

        const litem *a = box_of(by_id("a"));
        const litem *b = box_of(by_id("b"));
        ok("a row with something that grows lays out", a && b);
        if (a && b)
            okn("and the thing that grows took the rest of the room",
                b->w > 400, b->w);
    }

    /* --- a column direction ----------------------------------------------------- */
    {
        lay("<style>.col{display:flex;flex-direction:column;gap:10px}"
            ".box{background:#ff0000;width:50px}</style>"
            "<div class=col><div class=box id=a>x</div>"
            "<div class=box id=b>y</div></div>", 600);

        const litem *a = box_of(by_id("a"));
        const litem *b = box_of(by_id("b"));
        ok("a column lays out", a && b);
        if (a && b) {
            okn("its second box is below the first", b->y > a->y, b->y);
            okn("and at the same left edge", a->x == b->x, b->x - a->x);
        }
    }

    /* --- a row that does not fit ------------------------------------------------
     *
     * Five boxes wanting a hundred each in three hundred pixels. They have
     * to be made to fit, because a row that overflows takes the rest of the
     * page sideways with it. */
    {
        lay("<style>.row{display:flex}"
            ".box{background:#ff0000;width:100px}</style>"
            "<div class=row>"
            "<div class=box id=a>1</div><div class=box id=b>2</div>"
            "<div class=box id=c>3</div><div class=box id=d>4</div>"
            "<div class=box id=e>5</div></div>", 300);

        const litem *a = box_of(by_id("a"));
        const litem *e = box_of(by_id("e"));
        ok("a row that does not fit still lays out", a && e);
        if (a && e)
            okn("and the last of it is still on the page",
                e->x + e->w <= 320, e->x + e->w);
    }

    /* --- a row inside a row -------------------------------------------------
     *
     * The inner row is measured before it is placed, and a row being measured
     * no longer lays its children out a second time: it reports how far they
     * reach instead. What comes after it has to start where it really ends. */
    {
        lay("<style>.row{display:flex}"
            ".box{background:#ff0000;width:50px}"
            ".wide{background:#00ff00;width:60px}</style>"
            "<div class=row>"
            "<div class=row><div class=box id=a>1</div><div class=box id=b>2</div></div>"
            "<div class=wide id=c>3</div></div>", 600);

        const litem *a = box_of(by_id("a"));
        const litem *b = box_of(by_id("b"));
        const litem *c = box_of(by_id("c"));
        ok("a row inside a row lays out", a && b && c);
        if (a && b && c) {
            okn("with its boxes side by side", b->x == a->x + 50, b->x - a->x);
            okn("and the next box where the inner row ends", c->x == a->x + 100, c->x - a->x);
        }
    }

    /* --- and twelve of them -------------------------------------------------
     *
     * Each row measured its child by laying it out, then laid it out again,
     * and every row inside did the same inside both: the work doubled at
     * every level, and a page nested ten deep, which is an ordinary modern
     * page, laid itself out a thousand times over. Counted rather than timed,
     * because a count says the same thing on any machine. */
    {
        lay("<style>.r{display:flex}</style>"
            "<div class=r><div class=r><div class=r><div class=r>"
            "<div class=r><div class=r><div class=r><div class=r>"
            "<div class=r><div class=r><div class=r><div class=r>deep"
            "</div></div></div></div></div></div></div></div></div></div></div></div>",
            600);
        okn("rows nested twelve deep lay out in work that grows with the depth, not doubles",
            page.laid > 12 && page.laid <= 400, page.laid);

        /* And each element's rules are worked out once, however many times
           its style is asked for: it was two or three times a layout, and
           matching was nine tenths of what a layout cost. */
        int elements = 0;
        for (int i = 0; i < doc.count; i++) if (doc.nodes[i].kind == DN_ELEMENT) elements++;
        okn("and each element is matched against the rules once", page.matched > 0 && page.matched <= elements,
            page.matched);
    }

    /* --- rules that want something above the element ------------------------
     *
     * A rule is passed over without being walked when its other parts want an
     * ancestor the element does not have, which is decided from a few bits per
     * ancestor. So the ones that do match through ids, classes, element names
     * and child joins have to go on matching, and one wanting an ancestor that
     * is not there, written after them, must not paint over them. */
    {
        lay("<style>#top .mid div{background:#ff0000}"
            "ul > li.c2 > div{background:#0000ff}"
            ".other div{background:#00ff00}"
            "ol div{background:#00ff00}</style>"
            "<div id=top><div class=mid><div id=s>x</div></div></div>"
            "<ul><li class=c2><div id=u>y</div></li></ul>", 600);
        const litem *s = box_of(by_id("s"));
        const litem *u = box_of(by_id("u"));
        okn("a rule wanting an id and a class above matches through them",
            s && s->has_bg && s->bg == 0xFF0000, s ? s->bg : -1);
        okn("and one wanting names and a class by child joins",
            u && u->has_bg && u->bg == 0x0000FF, u ? u->bg : -1);
    }

    /* --- a picture at the size the page asked for ---------------------------
     *
     * An img carries a width and a height, and what the page says beats
     * what the file is: a drawing described in its own coordinates has to
     * land in the box the markup left for it. This is two passes, because
     * the layout only gives an img a box once the picture behind it has
     * arrived, and the list of pictures is built from the parsed document
     * -- so the document has to exist before the picture can be attached
     * to a node in it, and then be laid out again with it attached.
     */
    {
        /* Exactly what tools/webserver.py serves at /drawn, quotes, alt
           text and all: the browser check draws this page, and a test
           that lays out a tidied version of it is testing a page nobody
           sends. */
        const char *page_src =
            "<!doctype html>\n"
            "<html><head><title>a drawing</title>\n"
            "<style>body { font-family: sans-serif; padding: 24px }</style>"
            "</head>\n<body>\n"
            "<h1>a drawing</h1>\n"
            "<p>below this line there should be one</p>\n"
            "<img id=\"d\" src=\"/logo.svg\" width=\"200\" "
            "height=\"120\" alt=\"a drawing that did not draw\">\n"
            "</body></html>\n";

        nfake = 0;
        lay(page_src, 840);
        picture_at("d", 100, 60);       /* what the drawing says it is */
        lay(page_src, 840);

        const litem *img = 0;
        int node = dom_by_id(&doc, "d");
        for (int i = 0; i < page.nitems; i++)
            if (page.items[i].node == node && page.items[i].kind == LK_IMAGE)
                img = &page.items[i];

        ok("a picture on a page gets a box", img != 0);
        if (img) {
            okn("as wide as the page asked for, not as wide as the file",
                img->w == 200, img->w);
            okn("and as tall", img->h == 120, img->h);
        }
    }

    /* --- and one with no size of its own on the tag ------------------------- */
    {
        const char *page_src =
            "<style>body{padding:24px}</style>"
            "<p>x</p><img id=d src=/logo.svg>";

        nfake = 0;
        lay(page_src, 840);
        picture_at("d", 100, 60);
        lay(page_src, 840);

        const litem *img = 0;
        int node = dom_by_id(&doc, "d");
        for (int i = 0; i < page.nitems; i++)
            if (page.items[i].node == node && page.items[i].kind == LK_IMAGE)
                img = &page.items[i];

        ok("a picture with nothing said about it gets a box", img != 0);
        if (img) okn("at the size the file is", img->w == 100 && img->h == 60,
                     img->w);
    }

    /* --- what a width means -----------------------------------------------
     *
     * border-box is what nearly every page written this decade sets on
     * everything, because content-box makes a box with padding wider than
     * the number asked for. A browser that ignores it lays every such page
     * out too wide, and the error compounds at each level of nesting
     * because every child is handed its parent's wrong width. */
    {
        lay("<style>"
            "#c{box-sizing:content-box;width:200px;padding:20px;border:5px solid #000}"
            "#b{box-sizing:border-box;width:200px;padding:20px;border:5px solid #000}"
            "</style>"
            "<div id=c>content</div><div id=b>border</div>", 600);

        const litem *c = box_of(by_id("c"));
        const litem *b = box_of(by_id("b"));
        ok("both boxes are laid out", c != 0 && b != 0);
        /* content-box: 200 of content plus 50 of frame. border-box: 200
           altogether. What matters is that they differ, and by the frame. */
        if (c) okn("content-box is the width plus its frame", c->w == 250, c->w);
        if (b) okn("and border-box is the width", b->w == 200, b->w);
    }

    /* --- a floor and a ceiling --------------------------------------------- */
    {
        lay("<style>"
            "#n{background:#eee;width:50px;min-width:180px}"
            "#x{background:#eee;width:500px;max-width:120px}"
            "#both{background:#eee;width:10px;min-width:200px;max-width:150px}"
            "</style>"
            "<div id=n>narrow</div><div id=x>wide</div><div id=both>both</div>", 600);

        const litem *n = box_of(by_id("n"));
        const litem *x = box_of(by_id("x"));
        const litem *w = box_of(by_id("both"));
        if (n) okn("min-width raises a narrow box", n->w == 180, n->w);
        if (x) okn("max-width lowers a wide one", x->w == 120, x->w);
        /* When they disagree the floor wins, which is what every engine
           does and what a page that sets both relies on. */
        if (w) okn("and a floor beats a ceiling", w->w == 200, w->w);
    }

    /* --- out of the flow ---------------------------------------------------
     *
     * An absolutely positioned box is measured from its nearest positioned
     * ancestor and takes no space where it was written, so what follows it
     * closes up as though it were not there. Both halves matter: a browser
     * that placed it correctly and still reserved its space would push the
     * rest of the page down by the height of something that is not there. */
    {
        lay("<style>"
            "#outer{background:#eee;position:relative;margin:0;padding:0}"
            "#spacer{background:#ddd;height:40px}"
            "#abs{background:#ccc;position:absolute;left:30px;top:10px;width:50px;height:20px}"
            "#after{background:#bbb;height:15px}"
            "</style>"
            "<div id=outer>"
            "<div id=spacer>s</div>"
            "<div id=abs>a</div>"
            "<div id=after>b</div>"
            "</div>", 600);

        const litem *outer = box_of(by_id("outer"));
        const litem *abs = box_of(by_id("abs"));
        const litem *after = box_of(by_id("after"));

        ok("an absolute box is laid out", abs != 0);
        if (abs && outer) {
            okn("and sits where its ancestor plus its offsets put it",
                abs->x == outer->x + 30, abs->x);
            okn("on the other axis too", abs->y == outer->y + 10, abs->y);
        }
        /* The spacer is 40 tall and the absolute box claims 10..30. If it
           took space, what follows would start below it rather than at 40. */
        if (after && outer)
            okn("and takes no space, so what follows closes up",
                after->y == outer->y + 40, after->y - outer->y);
    }

    /* --- moved, but still counted ------------------------------------------
     *
     * relative is the flow, drawn somewhere else: the element moves and the
     * space it would have taken is still taken. That is the difference
     * between it and absolute and the whole reason both exist. */
    {
        lay("<style>"
            "#r{background:#eee;position:relative;left:25px;top:12px;height:30px}"
            "#next{background:#ddd;height:10px}"
            "</style>"
            "<div id=r>moved</div><div id=next>next</div>", 600);

        const litem *r = box_of(by_id("r"));
        const litem *next = box_of(by_id("next"));
        if (r) okn("a relative box is moved by its offsets", r->x == 25, r->x);
        if (next) okn("and the space it left is still taken",
                      next->y == 30, next->y);
    }

    /* --- the tag that is older than the standard which removed it ----------
     *
     * <center> was obsolete in 1999 and is on the front page of Google,
     * which is how the web is rather than how it is described. Without it
     * the element is unknown, an unknown element is inline, and the whole
     * page renders against the left margin -- which is exactly what this
     * browser did to google.com until it was added.
     */
    {
        /* A style block, not a style attribute: lay() above parses
           sheets and deliberately zeroes the inline ones, the way
           the browser gathers them separately. */
        lay("<style>#c{background:#eee;width:100px;display:inline-block}</style><center><div id=c>mid</div></center>", 600);
        const litem *c = box_of(by_id("c"));
        ok("a centre block lays out", c != 0);
        /* What <center> does is centre the inline content, which is what
           text-align:center means -- and it is exactly what Google's front
           page needs, because the logo, the search box and the buttons are
           all inline content inside one. A block child with a width of its
           own is not centred by it, here or anywhere else. */
        if (c) okn("and the inline content in it is centred rather than left",
                   c->x > 200 && c->x < 300, c->x);
    }

    /* --- a selector for something that is not in the tree ------------------
     *
     * `::-webkit-scrollbar` names a part of a scrollbar. There is no
     * scrollbar in the tree and nothing here draws one, so the rule is for
     * nobody -- but after the two colons are skipped there is no tag, no
     * class and no id left in it, which is the shape of `*`. It used to
     * match every element on the page, and `width:6px` with it.
     *
     * Found on a real article, which rendered as a column of one word per
     * line under a stylesheet whose seventh rule styles a scrollbar.
     */
    {
        lay("<style>::-webkit-scrollbar{width:6px}"
            "#a{background:#eee}</style>"
            "<div id=a>one two three four five</div>", 600);
        const litem *a = box_of(by_id("a"));
        ok("a box under a scrollbar rule lays out", a != 0);
        if (a) okn("and is not six pixels wide", a->w > 500, a->w);
    }

    /* The one-colon spelling of a pseudo element is the same thing. */
    {
        lay("<style>div:before{width:6px}"
            "#b{background:#eee}</style><div id=b>words here</div>", 600);
        const litem *b = box_of(by_id("b"));
        if (b) okn("nor under the old spelling of one", b->w > 500, b->w);
    }

    /* --- a length larger than a short ---------------------------------------
     *
     * Lengths are kept in hundredths, so 960px is 96000 and does not fit in
     * the sixteen bits they used to be kept in. It came back as 304.
     */
    {
        lay("<style>#w{width:960px;background:#eee}</style>"
            "<div id=w>x</div>", 1200);
        const litem *w = box_of(by_id("w"));
        if (w) okn("a width past a short's reach is the width", w->w == 960, w->w);
    }

    /* --- a fraction of the window ------------------------------------------ */
    {
        lay("<style>#v{width:50vw;background:#eee}</style>"
            "<div id=v>x</div>", 600);
        const litem *v = box_of(by_id("v"));
        if (v) okn("half the window wide is half the window", v->w == 300, v->w);
    }

    /* --- a percentage of a height nobody knows yet --------------------------
     *
     * `height:100%` with no height above it is auto, and resolving it
     * against the width instead made a logo as tall as the column was wide
     * and pushed a whole encyclopaedia article off the bottom of the
     * window.
     */
    {
        lay("<style>#t{height:100%;background:#eee}</style>"
            "<div id=t>one line</div>", 600);
        const litem *t = box_of(by_id("t"));
        if (t) okn("a percentage height with nothing to measure from is auto",
                   t->h > 0 && t->h < 100, t->h);
    }

    /* --- an element the parser has never heard of ----------------------------
     *
     * Its end tag closes it, like any other. It used to close nothing, so
     * everything after a custom element went inside it, and a page built of
     * them was one element nested a hundred deep. */
    {
        lay("<my-card id=c>inside</my-card><p id=after>after</p>", 600);
        int c = by_id("c"), after = by_id("after");
        ok("the end tag of an unknown element closes it",
           c >= 0 && after >= 0 && doc.nodes[after].parent == doc.nodes[c].parent);
    }

    /* --- pictures in the text -------------------------------------------------
     *
     * An emoji, a symbol with the mark that asks for it in colour, and a
     * joiner: none has an ASCII spelling, and each used to be a question
     * mark. They are left out; a letter the font lacks is still one. */
    {
        lay("<p id=t>a\xF0\x9F\x98\x80" "b\xE2\x98\x95\xEF\xB8\x8F" "c\xE2\x80\x8D" "d"
            "&#x1F600;e\xE4\xB8\x80</p>", 600);
        int t = by_id("t");
        int c = t >= 0 ? doc.nodes[t].first : -1;
        const char *s = c >= 0 && doc.nodes[c].text >= 0 ? doc.arena + doc.nodes[c].text : "";
        ok("pictures and the marks that join them are left out of the text, other letters are not",
           s[0] == 'a' && s[1] == 'b' && s[2] == 'c' && s[3] == 'd' && s[4] == 'e' && s[5] == '?'
           && s[6] == 0);
    }

    /* --- a flex item's width is where it starts --------------------------------
     *
     * The row grows or shrinks each item from its width and lays it out at
     * the result. Laying it out took the width again: an item that asked to
     * grow stayed the size it started, and items shrunk to fit ran over each
     * other, each drawn at its full width from where the row put it. */
    {
        lay("<style>.row{display:flex}#a{width:100px;background:#f00}"
            "#b{width:50%;flex:1;background:#0f0}</style>"
            "<div class=row><div id=a>x</div><div id=b>y</div></div>", 600);
        const litem *a = box_of(by_id("a"));
        const litem *b = box_of(by_id("b"));
        ok("a row of a fixed item and a growing one lays out", a && b);
        if (a && b) {
            okn("the fixed one keeps its width", a->w == 100, a->w);
            okn("and the growing one takes the rest of the row", b->w == 500, b->w);
        }
        lay("<style>.row{display:flex}#a{width:400px;background:#f00}"
            "#b{width:400px;background:#0f0}</style>"
            "<div class=row><div id=a>x</div><div id=b>y</div></div>", 600);
        a = box_of(by_id("a"));
        b = box_of(by_id("b"));
        if (a && b)
            okn("two too wide for the row share it, neither over the other",
                a->w == 300 && b->w == 300 && b->x >= a->x + a->w, a->w);
    }

    /* --- what follows a field that ends its parent ---------------------------
     *
     * A form field, or an element nobody is to see, laid out on its own and
     * last in its parent: skipping past it climbed out of the parent and
     * went on laying out the rest of the page on the field's line, and then
     * the page laid it out again where it belonged. Every search form that
     * ends in a button showed its whole page twice. */
    {
        static const char *const PAGES[] = {
            "<form><b>find</b> <input type=text name=q> <input type=submit value=go></form>"
            "<p id=after>after the form</p>",
            "<style>.gone{display:none}</style>"
            "<div>words <span class=gone>hidden</span></div><p id=after>after the div</p>",
        };
        static const char *const SAID[] = {
            "the words after a form that ends in a button are laid out once",
            "and the words after a hidden element that ends its parent",
        };
        for (int k = 0; k < 2; k++) {
            lay(PAGES[k], 600);
            int words = 0;
            for (int i = 0; i < page.nitems; i++) {
                const litem *it = &page.items[i];
                if (it->kind != LK_TEXT || it->at < 0) continue;
                const char *s = page.text + it->at;
                if (s[0] == 'a' && s[1] == 'f' && s[2] == 't') words++;
            }
            okn(SAID[k], words == 1, words);
        }
    }

    nfake = 0;

    /* --- tables ------------------------------------------------------------
     *
     * Rows under rows and cells in columns. They were blocks with the cells
     * run together as words, which reads for one row and for nothing else:
     * a table inside a cell ran all its rows into one paragraph. */
    {
        lay("<table><tr><td>short</td><td>x</td></tr>"
            "<tr><td>a much longer cell</td><td>y</td></tr></table>", 600);
        const litem *x = word("x"), *y = word("y"), *cell = word("cell");
        ok("a table's cells are laid out", x && y && cell);
        if (x && y && cell) {
            okn("the second row is below the first", y->y > x->y, y->y - x->y);
            okn("and its cells are in the same columns", y->x == x->x, y->x - x->x);
            okn("a column is as wide as the widest thing in it", x->x >= cell->x + cell->w,
                x->x - (cell->x + cell->w));
        }

        lay("<table><tr><td><table><tr><td>n1</td></tr><tr><td>n2</td></tr></table>"
            "</td><td>side</td></tr></table>", 600);
        const litem *n1 = word("n1"), *n2 = word("n2"), *side = word("side");
        ok("a table inside a cell keeps its rows", n1 && n2 && side && n2->y > n1->y && n2->x == n1->x);
        if (n1 && side) okn("and the cell beside it is beside it", side->x > n1->x, side->x - n1->x);

        lay("<style>table{background:#eeeeee}#uc{background:#dddddd}</style>"
            "<table id=t><tr><td>a</td><td>b</td></tr></table>"
            "<table id=u width=\"100%\"><tr><td id=uc>a</td></tr></table>", 600);
        const litem *t = box_of(by_id("t")), *u = box_of(by_id("u")), *uc = box_of(by_id("uc"));
        ok("tables with backgrounds have boxes", t && u && uc);
        if (t && u && uc) {
            okn("a table not given a width is as wide as its columns", t->w < 100, t->w);
            okn("and one given width=100% fills the line with its cells", uc->w >= 590, uc->w);
        }

        lay("<table><tr><td id=a bgcolor=ff6600>a</td><td>b<br>b<br>b</td></tr>"
            "<tr><td colspan=2 id=wide>both</td></tr></table><style>#wide{background:#00ff00}</style>", 600);
        const litem *a = box_of(by_id("a")), *w = box_of(by_id("wide")), *b = word("b");
        ok("a cell with bgcolor has a box of that colour", a && a->has_bg && a->bg == 0xFF6600);
        if (a) okn("stretched to the height of its row", a->h >= 3 * 16, a->h);
        const litem *aw = word("a");
        if (aw && a) okn("and its words are in the middle of it", aw->y > a->y + 8, aw->y - a->y);
        if (w && a && b) okn("a cell across two columns is as wide as both", w->w >= b->x + b->w - a->x - 4, w->w);

        lay("<table><tr><td rowspan=2>tall</td><td>r1</td></tr><tr><td>r2</td></tr></table>", 600);
        const litem *r1 = word("r1"), *r2 = word("r2");
        ok("a cell spanning two rows keeps its column clear below it",
           r1 && r2 && r1->x == r2->x && r2->y > r1->y);

        lay("<style>#p{background:#eeeeee}</style><table cellpadding=10><tr><td id=p>pad</td></tr></table>", 600);
        const litem *pb = box_of(by_id("p")), *pw = word("pad");
        ok("cellpadding pads every cell", pb && pw && pw->x - pb->x >= 10 && pw->y - pb->y >= 10);
    }

    /* --- a page with no doctype -------------------------------------------
     *
     * A table's text starts from the left whatever centres it, which is the
     * quirk a centred page of tables is written for; with a doctype the
     * centring carries in. */
    {
        lay("<center><table><tr><td>left</td></tr>"
            "<tr><td>a longer line of words</td></tr></table></center>", 600);
        const litem *l1 = word("left"), *l2 = word("a");
        ok("without a doctype a table's rows read from the left", l1 && l2 && l1->x == l2->x);
        lay("<!DOCTYPE html><center><table><tr><td>left</td></tr>"
            "<tr><td>a longer line of words</td></tr></table></center>", 600);
        l1 = word("left");
        l2 = word("a");
        ok("and with one they are centred", l1 && l2 && l1->x > l2->x);
    }

    /* --- inline-blocks and blocks inside inline things ---------------------- */
    {
        lay("<style>#ib{display:inline-block;width:100px;background:#00ff00}</style>"
            "<p>before <span id=ib>in</span> after</p>", 600);
        const litem *ib = box_of(by_id("ib")), *af = word("after");
        ok("an inline-block has its own box", ib != 0);
        if (ib) okn("as wide as it asked", ib->w == 100, ib->w);
        if (ib && af) okn("and what follows starts after it", af->x >= ib->x + 100, af->x - ib->x);

        lay("<a href=#><div>top</div><div>bottom</div></a>", 600);
        const litem *tp = word("top"), *bt = word("bottom");
        ok("blocks inside a link are still stacked", tp && bt && bt->y > tp->y && bt->x == tp->x);
    }

    /* --- measuring does not take centring for width ------------------------- */
    {
        lay("<style>#t{background:#eeeeee}td{text-align:center}</style>"
            "<table id=t><tr><td>mid</td></tr></table>", 600);
        const litem *t = box_of(by_id("t"));
        ok("a centred cell does not make its table as wide as the page", t && t->w < 100);
    }

    /* --- media queries ------------------------------------------------------ */
    {
        int lo, hi;
        ok("a query for a wide window is kept as a floor",
           css_mq("(min-width: 2000px)", 19, &lo, &hi) && lo == 2000 && hi == -1);
        ok("one in ems is in pixels", css_mq("screen and (max-width: 40em)", 28, &lo, &hi) && hi == 640);
        ok("a range is kept too", css_mq("(width >= 600px)", 16, &lo, &hi) && lo == 600);
        ok("print never applies", !css_mq("print", 5, &lo, &hi));
        ok("nor a dark scheme", !css_mq("(prefers-color-scheme: dark)", 28, &lo, &hi));
        ok("nor a second pixel to the pixel", !css_mq("(min-resolution: 2dppx)", 23, &lo, &hi));

        lay("<style>@media (min-width:2000px){#m{color:#ff0000}}"
            "@media (max-width:2000px){#n{color:#ff0000}}</style>"
            "<p id=m>wide</p><p id=n>narrow</p>", 600);
        const litem *m = word("wide"), *n = word("narrow");
        ok("rules for a window wider than this one do not apply", m && m->color != 0xFF0000);
        ok("and rules for this one do", n && n->color == 0xFF0000);
    }

    /* --- selectors ----------------------------------------------------------- */
    {
        lay("<style>[data-x=a]{color:#ff0000}[title^=pre]{color:#00ff00}"
            "[title$=end]{color:#0000ff}[class~=two]{color:#ff00ff}</style>"
            "<p data-x=a>aa</p><p data-x=b>bb</p><p title=prefix>pp</p>"
            "<p title=theend>ee</p><p class=\"one two\">ww</p><p class=twofold>ff</p>", 600);
        const litem *aa = word("aa"), *bb = word("bb"), *pp = word("pp"), *ee = word("ee");
        const litem *ww = word("ww"), *ff = word("ff");
        ok("an attribute selector with a value matches that value", aa && aa->color == 0xFF0000);
        ok("and not another", bb && bb->color != 0xFF0000);
        ok("^= and $= match the start and the end", pp && pp->color == 0x00FF00 && ee && ee->color == 0x0000FF);
        ok("~= matches a whole word only", ww && ww->color == 0xFF00FF && ff && ff->color != 0xFF00FF);

        lay("<style>p:focus{color:#ff0000}li:last-child{color:#00ff00}"
            "li:nth-child(2n){color:#0000ff}p:not(.x){color:#ff00ff}</style>"
            "<p class=x>focus</p><ul><li>l1</li><li>l2</li><li>l3</li></ul><p>other</p>", 600);
        const litem *fo = word("focus"), *l1 = word("l1"), *l2 = word("l2"), *l3 = word("l3");
        const litem *ot = word("other");
        ok("a state nothing is in when a page is drawn does not apply", fo && fo->color != 0xFF0000);
        ok(":last-child is the last", l3 && l3->color == 0x00FF00 && l1 && l1->color != 0x00FF00);
        ok(":nth-child(2n) is every second", l2 && l2->color == 0x0000FF && l1->color != 0x0000FF);
        ok(":not() refuses what it names", ot && ot->color == 0xFF00FF && fo->color != 0xFF00FF);

        lay("<style>tbody td{color:#ff0000}my-el{display:block}h2+p{color:#00ff00}"
            "h2~p{font-weight:bold}</style>"
            "<table><tbody><tr><td>cell</td></tr></tbody></table>"
            "<span>one</span><my-el>two</my-el>"
            "<h2>h</h2><p>next</p><p>later</p>", 600);
        const litem *ce = word("cell"), *o1 = word("one"), *t2 = word("two");
        const litem *nx = word("next"), *lt = word("later");
        ok("a selector naming an element with no number of its own applies", ce && ce->color == 0xFF0000);
        ok("and a page's own element can be a block", o1 && t2 && t2->y > o1->y);
        ok("+ takes the element straight after", nx && nx->color == 0x00FF00 && lt && lt->color != 0x00FF00);
        ok("~ takes every one after", nx && lt && nx->face == lt->face && tface_of(lt->face)->bold);
    }

    /* --- the browser's own rules lose to the page's ------------------------- */
    {
        lay("<style>*{margin:0}#d{background:#eeeeee}</style><div id=d><p>first</p></div>", 600);
        const litem *d = box_of(by_id("d")), *f = word("first");
        ok("a page's rule beats the browser's own, however specific", d && f && f->y - d->y < 6);
    }

    /* --- what old markup says about itself ---------------------------------- */
    {
        lay("<font color=red size=6>big</font> <span>small</span>", 600);
        const litem *bg = word("big"), *sm = word("small");
        ok("<font> gives its colour and its size", bg && sm && bg->color == 0xFF0000
           && tface_h(bg->face) > tface_h(sm->face));
        lay("<p hidden>gone</p><p>kept</p>", 600);
        ok("the hidden attribute hides", !word("gone") && word("kept"));
    }

    /* --- flex rows that wrap, and that cannot shrink past their words ------- */
    {
        lay("<style>.r{display:flex;flex-wrap:wrap}.r div{width:150px;background:#eeeeee}</style>"
            "<div class=r><div id=a>1</div><div>2</div><div>3</div><div id=d>4</div></div>", 500);
        const litem *a = box_of(by_id("a")), *d = box_of(by_id("d"));
        ok("a wrapping row puts what does not fit on the next line", a && d && d->y > a->y && d->x == a->x);

        lay("<style>.f{display:flex}</style><div class=f><div>abcdefghijklmnopqrstu</div>"
            "<div>vwxyzabcdefghijklmnop</div><div>qrstuvwxyzabcdefghij</div></div>", 200);
        const litem *w1 = word("abcdefghijklmnopqrstu"), *w2 = word("vwxyzabcdefghijklmnop");
        ok("a row too narrow for its words does not write them over each other",
           w1 && w2 && w2->x >= w1->x + w1->w);

        static char many[4096];
        int n = 0;
        const char *head = "<style>.f{display:flex;flex-wrap:wrap}</style><div class=f>";
        for (const char *p = head; *p; p++) many[n++] = *p;
        for (int i = 0; i < 150; i++) {
            const char *cell = "<div>k</div>";
            for (const char *p = cell; *p; p++) many[n++] = *p;
        }
        const char *tail = "<div>last</div></div>";
        for (const char *p = tail; *p; p++) many[n++] = *p;
        many[n] = 0;
        lay(many, 600);
        ok("a flex row of more than a hundred and fifty keeps every one", word("last") != 0);
    }

    /* --- floats ---------------------------------------------------------------- */
    {
        lay("<style>#f{float:right;width:100px;background:#0000ff}#c{clear:both;background:#eeeeee}</style>"
            "<div id=f>F</div>"
            "<p>one two three four five six seven eight nine ten eleven twelve thirteen "
            "fourteen fifteen sixteen seventeen eighteen nineteen twenty</p>"
            "<div id=c>below</div>", 400);
        const litem *f0 = box_of(by_id("f"));
        lay("<style>#f{float:right;width:100px;height:200px;background:#0000ff}"
            "#c{clear:both;background:#eeeeee}</style>"
            "<div id=f>F</div><p>short</p><div id=c>below</div>", 400);
        const litem *fc = box_of(by_id("f")), *cc = box_of(by_id("c"));
        ok("a box that clears goes below the float", fc && cc && cc->y >= fc->y + fc->h);
        lay("<style>#f{float:right;width:100px;background:#0000ff}#c{clear:both;background:#eeeeee}</style>"
            "<div id=f>F</div>"
            "<p>one two three four five six seven eight nine ten eleven twelve thirteen "
            "fourteen fifteen sixteen seventeen eighteen nineteen twenty</p>"
            "<div id=c>below</div>", 400);
        (void)f0;
        const litem *f = box_of(by_id("f")), *one = word("one"), *c = box_of(by_id("c"));
        ok("a float to the right is at the right", f && f->x == 300 && f->w == 100);
        int beside = 1;
        for (int i = 0; f && i < page.nitems; i++) {
            const litem *it = &page.items[i];
            if (it->kind == LK_TEXT && it->y < f->y + f->h && it->x + it->w > f->x && it->node != by_id("f")
                && page.text[it->at] != 'F')
                beside = 0;
        }
        ok("and the words beside it keep out of its way", one && beside);
        ok("and one after a long paragraph is below it too", f && c && c->y >= f->y + f->h);

        lay("<style>.o{padding:0 40px}#n{margin:0 -20px;background:#ff0000}</style>"
            "<div class=o><div id=n>x</div></div>", 400);
        const litem *nb = box_of(by_id("n"));
        ok("a negative margin reaches into its container", nb && nb->x == 20 && nb->w == 360);
    }

    /* --- words for a screen reader and not for the eye ---------------------- */
    {
        lay("<style>.sr{position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0,0,0,0)}</style>"
            "<p>shown <span class=sr>unseen</span></p>", 600);
        ok("text clipped to nothing is not drawn", word("shown") && !word("unseen"));
        lay("<style>.cap{max-height:20px}</style><div class=cap><p>p1</p><p>p2</p><p>p3</p></div><p>after</p>", 600);
        const litem *p3 = word("p3"), *af = word("after");
        ok("a box capped in height but not hiding its overflow does not put what follows over it",
           p3 && af && af->y > p3->y);
    }

    /* --- a picture made a block is still a picture ------------------------- */
    {
        lay("<style>#pic{display:block}</style><div><img id=pic src=x></div>", 600);
        picture_at("pic", 40, 30);
        lay("<style>#pic{display:block}</style><div><img id=pic src=x></div>", 600);
        int found = 0;
        for (int i = 0; i < page.nitems; i++)
            if (page.items[i].kind == LK_IMAGE) found = 1;
        nfake = 0;
        ok("a picture with display:block is drawn", found);
    }

    /* --- a button that is only an icon ------------------------------------- */
    {
        lay("<button id=b aria-label=\"search\"><svg></svg></button>", 600);
        int b = by_id("b");
        const char *label = b >= 0 ? lay_control_label(&doc, b, CTL_BUTTON) : "";
        ok("a button with no words is named by its label, not called Button", w_same(label, "search"));
    }

    /* --- a transform's translation ------------------------------------------
     *
     * Moved by pixels and by a share of the box's own size, once the box is
     * laid out: the dialog centred by going back half itself, the menu kept
     * out of sight by going back all of itself. */
    {
        lay("<style>#m{position:absolute;left:0;top:0;width:200px;transform:translateX(-100%);"
            "background:#ff0000}#d{position:absolute;left:300px;top:100px;width:100px;height:40px;"
            "transform:translate(-50%, -50%);background:#00ff00}"
            "#c{position:absolute;left:0;top:300px;width:200px;"
            "transform:translateX(calc(-100% - 10px));background:#0000ff}</style>"
            "<div id=m>menu</div><div id=d>dialog</div><div id=c>calc</div>", 600);
        const litem *m = box_of(by_id("m")), *dg = box_of(by_id("d")), *c = box_of(by_id("c"));
        ok("translated boxes are laid out", m && dg && c);
        if (m) okn("translateX(-100%) moves a box back its own width", m->x == -200, m->x);
        if (dg) okn("translate(-50%, -50%) centres a box on its point", dg->x == 250 && dg->y == 80, dg->x);
        if (c) okn("and calc() of a share and pixels is both", c->x == -210, c->x);
    }

    /* --- @supports, a row that would scroll, and an icon's name ------------- */
    {
        lay("<style>@supports (display:grid){#g{color:#ff0000}}"
            "@supports not (display:grid){#n{color:#00ff00}}"
            "@supports (display:flex){#f{color:#0000ff}}</style>"
            "<p id=g>grid</p><p id=n>nogrid</p><p id=f>flex</p>", 600);
        const litem *g = word("grid"), *n = word("nogrid"), *f = word("flex");
        ok("@supports for grid is refused, since grid is laid out as blocks", g && g->color != 0xFF0000);
        ok("and @supports not (grid) is taken", n && n->color == 0x00FF00);
        ok("and @supports for anything read is taken", f && f->color == 0x0000FF);

        lay("<style>.shelf{display:flex;overflow-x:auto}.shelf div{width:150px;flex-shrink:0;"
            "background:#eeeeee}</style>"
            "<div class=shelf><div id=a>1</div><div>2</div><div>3</div><div id=d>4</div></div>", 500);
        const litem *a = box_of(by_id("a")), *d = box_of(by_id("d"));
        ok("a row that would scroll sideways wraps instead", a && d && d->y > a->y && d->x == a->x);

        lay("<button id=b><svg><title>Chevron Left</title></svg> Back</button>", 600);
        int b = by_id("b");
        const char *label = b >= 0 ? lay_control_label(&doc, b, CTL_BUTTON) : "";
        ok("an icon's own title is not taken for the button's words", w_same(label, " Back"));
    }

    /* --- grid -----------------------------------------------------------------
     *
     * Columns from grid-template-columns and the items poured into them a
     * row at a time. It was a block, so three columns of cards were one card
     * a row. */
    {
        lay("<style>.g{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}"
            ".g div{background:#eeeeee}</style>"
            "<div class=g><div id=a>1</div><div id=b>2</div><div id=c>3</div><div id=d>4</div></div>", 620);
        const litem *a = box_of(by_id("a")), *b = box_of(by_id("b")), *c = box_of(by_id("c"));
        const litem *d = box_of(by_id("d"));
        ok("a grid's items are laid out", a && b && c && d);
        if (a && b && c && d) {
            okn("three to a row, side by side", a->y == b->y && b->y == c->y && b->x > a->x && c->x > b->x, c->x);
            okn("each a third of the row, less the gaps", a->w == 200, a->w);
            okn("with the gap between them", b->x - (a->x + a->w) == 10, b->x - (a->x + a->w));
            okn("and the fourth starts the next row", d->y > a->y && d->x == a->x, d->y - a->y);
        }

        lay("<style>.g{display:grid;grid-template-columns:200px 1fr}.g div{background:#eeeeee}"
            "#w{grid-column:1 / -1}</style>"
            "<div class=g><div id=a>side</div><div id=b>main</div><div id=w>wide</div></div>", 600);
        a = box_of(by_id("a"));
        b = box_of(by_id("b"));
        const litem *w = box_of(by_id("w"));
        ok("a fixed column and one taking the rest", a && b && a->w == 200 && b->w == 400 && b->x == a->x + 200);
        ok("and an item spanning the whole row", w && a && w->w == 600 && w->y > a->y);

        lay("<style>.g{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr))}"
            ".g div{background:#eeeeee}</style>"
            "<div class=g><div id=a>1</div><div id=b>2</div><div id=c>3</div><div id=d>4</div></div>", 480);
        a = box_of(by_id("a"));
        c = box_of(by_id("c"));
        d = box_of(by_id("d"));
        ok("auto-fill makes as many columns as fit", a && c && d && c->y == a->y && d->y > a->y && a->w == 160);

        /* A 1fr column is never narrower than what is in it: 1fr is
           minmax(auto, 1fr). Six of them holding 280 pixel cards were six
           slivers with the cards squeezed into them. */
        lay("<style>.g{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;width:400px}"
            ".g div{width:200px;background:#eeeeee}</style>"
            "<div class=g><div id=a>1</div><div id=b>2</div><div id=c>3</div></div>", 600);
        a = box_of(by_id("a"));
        b = box_of(by_id("b"));
        okn("a 1fr column is as wide as the item in it", a && b && b->x - a->x == 210,
            a && b ? b->x - a->x : -1);

        lay("<style>.g{display:grid;grid-template-columns:1fr 1fr;width:400px}"
            ".g div{background:#eeeeee}#a{width:300px}</style>"
            "<div class=g><div id=a>1</div><div id=b>2</div></div>", 600);
        a = box_of(by_id("a"));
        b = box_of(by_id("b"));
        okn("and the other column has what that one leaves", a && b && b->x - a->x == 300 && b->w == 100,
            b ? b->w : -1);

        /* And one that would scroll sideways, which nothing inside a page
           does here, wraps to the columns that fit instead. */
        lay("<style>.g{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;width:400px;"
            "overflow-x:auto}.g div{width:150px;background:#eeeeee}</style>"
            "<div class=g><div id=a>1</div><div id=b>2</div><div id=c>3</div></div>", 600);
        a = box_of(by_id("a"));
        b = box_of(by_id("b"));
        c = box_of(by_id("c"));
        ok("a grid that would scroll sideways wraps to the columns that fit",
           a && b && c && b->y == a->y && b->x > a->x && c->y > a->y && c->x == a->x);

        /* grid-auto-flow: column, the other way a strip of cards is made:
           each item a column of its own along one row. It was one column,
           the items stacked. */
        lay("<style>.g{display:grid;grid-auto-flow:column;gap:10px}.g div{background:#eeeeee}</style>"
            "<div class=g><div id=a>one</div><div id=b>two</div><div id=c>three</div></div>", 600);
        a = box_of(by_id("a"));
        b = box_of(by_id("b"));
        c = box_of(by_id("c"));
        ok("grid-auto-flow: column lays the items along one row",
           a && b && c && a->y == b->y && b->y == c->y && b->x > a->x && c->x > b->x);

        lay("<style>.g{display:grid;grid-auto-flow:column;grid-auto-columns:120px;gap:10px}"
            ".g div{background:#eeeeee}</style>"
            "<div class=g><div id=a>one</div><div id=b>two</div></div>", 600);
        a = box_of(by_id("a"));
        b = box_of(by_id("b"));
        okn("each as wide as grid-auto-columns says", a && b && a->w == 120 && b->x - a->x == 130,
            a ? a->w : -1);
    }

    /* --- a grid of named areas ------------------------------------------------ */
    {
        lay("<style>.g{display:grid;grid-template-columns:150px 1fr;"
            "grid-template-areas:\"head head\" \"side main\" \"foot foot\"}"
            ".g div{background:#eeeeee}#h{grid-area:head}#s{grid-area:side}#m{grid-area:main}"
            "#f{grid-area:foot}</style>"
            "<div class=g><div id=f>foot</div><div id=m>main</div><div id=s>side</div>"
            "<div id=h>head</div></div>", 600);
        const litem *h = box_of(by_id("h")), *sd = box_of(by_id("s")), *m = box_of(by_id("m"));
        const litem *f = box_of(by_id("f"));
        ok("a grid of named areas lays out", h && sd && m && f);
        if (h && sd && m && f) {
            ok("each item where its area is, whatever order it was written in",
               h->y < sd->y && sd->y == m->y && f->y > m->y);
            okn("an area across both columns is as wide as both", h->w == 600 && f->w == 600, h->w);
            okn("and the side and the main have their columns", sd->w == 150 && m->x == 150 && m->w == 450, m->w);
        }
    }

    /* --- custom properties, calc(), masks and @import --------------------------- */
    {
        lay("<style>:root{--brand:#ff0000;--pad:12px}.box{--brand:#00ff00}"
            "#a{color:var(--brand)}#b{color:var(--brand)}#c{color:var(--missing, #0000ff)}"
            "#d{background:#eeeeee;padding-left:var(--pad)}"
            "#e{--x:var(--brand);color:var(--x)}</style>"
            "<body><p id=a>root</p><div class=box><p id=b>inner</p><p id=e>chain</p></div>"
            "<p id=c>fallback</p><p id=d>padded</p></body>", 600);
        const litem *a = word("root"), *b = word("inner"), *c = word("fallback"), *e = word("chain");
        const litem *d = box_of(by_id("d")), *dw = word("padded");
        ok("a custom property on :root reaches the page", a && a->color == 0xFF0000);
        ok("and the nearest one wins, inherited from an ancestor", b && b->color == 0x00FF00);
        ok("var() with nothing set takes its fallback", c && c->color == 0x0000FF);
        ok("a length from a custom property is a length", d && dw && dw->x - d->x == 12);
        ok("and a custom property made of another resolves through it", e && e->color == 0x00FF00);

        lay("<style>#w{width:calc(100% - 100px);background:#eeeeee}"
            "#m{width:min(90%, 300px);background:#eeeeee}"
            "#cl{width:clamp(100px, 10%, 200px);background:#eeeeee}"
            "#x{width:calc(2 * 50px + 10%);background:#eeeeee}</style>"
            "<div id=w>w</div><div id=m>m</div><div id=cl>c</div><div id=x>x</div>", 600);
        const litem *w = box_of(by_id("w")), *m = box_of(by_id("m")), *cl = box_of(by_id("cl"));
        const litem *x = box_of(by_id("x"));
        ok("calc() of a percentage less pixels", w && w->w == 500);
        ok("min() takes the smaller", m && m->w == 300);
        ok("clamp() keeps a value between its bounds", cl && cl->w == 100);
        ok("calc() multiplies and adds", x && x->w == 160);

        lay("<style>#i{mask-image:url(x.svg);background:#000000;width:20px;height:20px}"
            "#k{mask-image:linear-gradient(#000,#0000);background:#000000;width:20px;height:20px}"
            "#j{background:#000000;width:20px;height:20px}</style><div id=i></div><div id=k></div><div id=j></div>", 600);
        const litem *ib = box_of(by_id("i")), *jb = box_of(by_id("j")), *kb = box_of(by_id("k"));
        ok("a box painted through a mask it cannot draw does not draw its background",
           !(kb && kb->has_bg) && jb && jb->has_bg);
        ok("and one painted through a picture keeps its colour for the picture's shape",
           ib && ib->has_bg && ib->bgi >= 0 && page.bgs[ib->bgi].mask);

        const char *imp = "@charset \"utf-8\"; /* a note */ @import url(\"base.css\");"
                          "@import 'print.css' print; @import url(wide.css) (min-width: 900px); p{color:red}";
        int at = 0, lo, hi, n = 0, got;
        char href[256], first[256] = "", third[256] = "";
        int third_lo = 0, skipped = 0;
        while ((got = css_next_import(imp, w_len(imp), &at, href, (int)sizeof(href), &lo, &hi))) {
            if (got < 0) { skipped++; continue; }
            if (n == 0) w_copy(first, sizeof(first), href, sizeof(first));
            if (n == 1) { w_copy(third, sizeof(third), href, sizeof(third)); third_lo = lo; }
            n++;
        }
        ok("@import is read past @charset and comments", w_same(first, "base.css"));
        ok("one for print is left out", skipped == 1);
        ok("and one for wide windows keeps its width", w_same(third, "wide.css") && third_lo == 900 && n == 2);
    }

    /* --- a picture that did not arrive ----------------------------------------- */
    {
        nfake = 0;
        lay("<p><img id=f src=x width=200 height=100 alt=\"astronauts on a runway at dawn\"> after</p>"
            "<p><img src=y alt=\"words instead\"></p>", 600);
        const litem *f = box_of(by_id("f"));
        ok("a missing picture with a size keeps its room, as a frame", f && f->w == 200 && f->h == 100);
        ok("and its alt text is not poured into it", !word("astronauts") && word("after"));
        ok("one with no size is still its words", word("words") != 0);
    }

    /* --- colours that cannot be seen, and text put out of sight ------------------ */
    {
        lay("<style>#t{background:transparent}#h{background:rgba(0,0,0,.1)}#z{background:rgba(0,0,0,0)}"
            "#l{color:hsl(0, 100%, 50%)}#i{color:transparent}#f{font-size:0}"
            "#x{text-indent:-9999px}#t,#h,#z{width:50px;height:10px}</style>"
            "<div id=t></div><div id=h></div><div id=z></div><p id=l>hue</p><p id=i>ghost</p>"
            "<p id=f>hidden</p><p id=x>offpage</p><p>shown</p>", 600);
        const litem *t = box_of(by_id("t")), *h = box_of(by_id("h")), *z = box_of(by_id("z"));
        const litem *l = word("hue"), *x = word("offpage");
        ok("a transparent background is no background", !(t && t->has_bg) && !(z && z->has_bg));
        ok("and a faint one written .1 is faint, not black", h && h->has_bg && h->bg == 0xE6E6E6);
        ok("hsl() is a colour", l && l->color == 0xFF0000);
        ok("text in a transparent colour is not drawn", !word("ghost") && word("shown"));
        ok("nor text at font-size 0", !word("hidden"));
        ok("and text indented by -9999px is off the page", x && x->x + x->w < 0);
    }

    /* --- drawings written into the page ------------------------------------------
     *
     * An <svg> in the markup is a picture the browser draws from the tree,
     * at the size the page gives it. Laid out as boxes instead, its shapes
     * each took a line, a logo took its header's whole width and the row
     * holding it wrapped, and most were hidden by the browser's own sheet. */
    {
        nfake = 0;
        lay("<p>a <svg id=s width=40 height=20 viewBox=\"0 0 4 2\"><text x=0 y=1>words</text>"
            "<rect width=4 height=2 fill=red /></svg> b</p>", 600);
        const litem *s = image_of(by_id("s"));
        okn("an svg in the page is a picture of the size it says", s && s->w == 40 && s->h == 20,
            s ? s->w : -1);
        ok("on the line with the words around it", s && word("a") && word("b")
           && word("a")->x < s->x && s->x < word("b")->x);
        ok("and what is inside it is not laid out as the page", !word("words"));

        lay("<style>.i{width:24px;height:24px}</style>"
            "<svg id=s class=i width=100 height=100 viewBox=\"0 0 10 10\"></svg>", 600);
        s = image_of(by_id("s"));
        okn("the page's rules beat what it says", s && s->w == 24 && s->h == 24, s ? s->w : -1);

        lay("<svg id=s height=30 viewBox=\"0 0 324 60\"></svg>", 600);
        s = image_of(by_id("s"));
        okn("a height alone and its viewBox make the width", s && s->w == 162 && s->h == 30,
            s ? s->w : -1);

        lay("<style>p{font-size:20px}</style><p><svg id=s width=1em height=1em></svg></p>", 600);
        s = image_of(by_id("s"));
        okn("and 1em is the size of the text around it", s && s->w == 20 && s->h == 20,
            s ? s->w : -1);

        lay("<style>.h{height:26px}.s{width:auto;height:100%}</style>"
            "<div class=h><svg id=s class=s viewBox=\"0 0 104 27\"></svg></div>", 600);
        s = image_of(by_id("s"));
        okn("a height of 100% in a parent 26 pixels tall is 26 pixels", s && s->h == 26 && s->w == 100,
            s ? s->h : -1);

        /* A logo beside a menu in a row: measured as the whole row, it
           squeezed the menu and pushed the rest onto a second line. */
        lay("<style>.row{display:flex;flex-wrap:wrap}.logo{flex-grow:1}.lt{display:inline-block}"
            ".link{display:inline-block;padding:20px;background:#eeeeee}</style>"
            "<div class=row><div class=logo><a href=/><svg id=s class=lt width=162 height=30><title>home</title>"
            "<circle cx=20 cy=17 r=3></circle><path d=\"M0 0h9v9z\"></path></svg></a></div>"
            "<div><a class=link href=/m>menu</a></div><div><a class=link id=f href=/f>find</a></div></div>", 600);
        s = image_of(by_id("s"));
        const litem *m = word("menu"), *f = word("find");
        ok("a row holding a drawing does not wrap",
           s && m && f && m->y == f->y && m->y < s->y + s->h && f->x > m->x);

        /* width="100%" in something sized to its contents cannot be
           resolved there, and is the 300 pixels every browser gives a
           picture with no size. */
        lay("<style>.row{display:flex}</style><div class=row><a href=/ id=a>"
            "<svg id=s width=100% height=100% viewBox=\"0 0 309 70\"></svg></a><span>x</span></div>", 600);
        s = image_of(by_id("s"));
        okn("a drawing 100% wide in a row is 300 pixels, not the row", s && s->w == 300,
            s ? s->w : -1);

        /* A picture given a box of its own, a flex item, was a box with
           nothing in it. */
        lay("<style>.row{display:flex}</style><div class=row><img id=p src=x><span>y</span></div>", 600);
        picture_at("p", 40, 30);
        lay("<style>.row{display:flex}</style><div class=row><img id=p src=x><span>y</span></div>", 600);
        const litem *p = image_of(by_id("p"));
        okn("a picture that is a flex item is drawn", p && p->w == 40 && p->h == 30, p ? p->w : -1);
        nfake = 0;
    }

    /* --- what a row leaves out ------------------------------------------------
     *
     * A closed menu (hidden, width: 100%) and something positioned over the
     * row are not items of it: counted in, the menu took a line of its own
     * and pushed the search button under GOV.UK's logo. */
    {
        lay("<style>.row{display:flex;flex-wrap:wrap}.dd{width:100%}</style>"
            "<div class=row><div>one</div><div class=dd hidden>gone</div><div>two</div></div>", 600);
        const litem *a = word("one"), *b = word("two");
        ok("a hidden child takes no room in a row", a && b && a->y == b->y && !word("gone"));

        lay("<style>.row{display:flex;flex-wrap:wrap;position:relative}"
            ".ab{position:absolute;top:0;left:0;width:100%}</style>"
            "<div class=row><div>alpha</div><div class=ab>over</div><div>beta</div></div>", 600);
        a = word("alpha");
        b = word("beta");
        const litem *o = word("over");
        ok("nor does one positioned over it, which is still drawn", a && b && o && a->y == b->y && b->x < 200);
    }

    /* --- a template is not the page ---------------------------------------------
     *
     * Markup kept for a script to stamp out later. GitHub keeps menus and
     * dialogs in <template>, and they were drawn down the page. */
    {
        lay("<p>shown</p><template><p>inert</p><div><a href=/x>stamped</a></div></template><p>after</p>", 600);
        ok("what is in a template is not drawn", word("shown") && word("after") && !word("inert")
           && !word("stamped"));
    }

    /* --- display: contents ------------------------------------------------------
     *
     * An element with no box: in a row or a grid its children are the
     * items. Read as a block, a wrapper was one item with its children
     * stacked inside it, and a strip of cards was a card and a column. */
    {
        lay("<style>.row{display:flex}.w{display:contents;color:#ff0000;background:#0000ff;padding:20px}"
            ".i{width:100px;background:#eeeeee}</style>"
            "<div class=row><div class=w id=w><div class=i id=a>one</div><div class=i id=b>two</div></div>"
            "<div class=i id=c>three</div></div>", 600);
        const litem *a = box_of(by_id("a")), *b = box_of(by_id("b")), *c = box_of(by_id("c"));
        ok("a contents element's children are the row's items",
           a && b && c && a->y == b->y && b->y == c->y && b->x >= a->x + 100 && c->x >= b->x + 100);
        const litem *one = word("one");
        ok("and still take what it passes down", one && one->color == 0xFF0000);

        lay("<style>.w{display:contents;background:#0000ff;padding:20px}</style>"
            "<div><div class=w id=w>inside</div></div>", 600);
        const litem *w = box_of(by_id("w")), *in = word("inside");
        ok("while it draws no box of its own", !(w && w->has_bg) && in && in->x < 10);

        lay("<style>.g{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.w{display:contents}"
            ".g div{background:#eeeeee}</style>"
            "<div class=g><div class=w><div id=a>1</div><div id=b>2</div></div><div id=c>3</div></div>", 620);
        a = box_of(by_id("a"));
        b = box_of(by_id("b"));
        c = box_of(by_id("c"));
        ok("and a grid's", a && b && c && a->y == b->y && b->y == c->y && a->w == 200 && b->x > a->x
           && c->x > b->x);

        lay("<style>@supports (display:contents){#y{color:#00ff00}}</style><p id=y>asked</p>", 600);
        const litem *y = word("asked");
        ok("and @supports says so", y && y->color == 0x00FF00);
    }

    /* --- a sheet written by hand ------------------------------------------------
     *
     * With a space after each colon, as nearly every sheet not put through a
     * minifier is written. Every check above is written without one, and so
     * none of them saw that the space was kept and every keyword after it
     * missed. */
    {
        lay("<style>\n.a { display: none; }\n.c { float: right; width: 100px; }\n"
            ".d { text-align: center; }\n.e { font-weight: bold; }\n</style>"
            "<p class=a>gone</p><div class=c>floated</div><p class=d>centred</p>"
            "<p class=e>strong</p><p>plain</p>", 600);
        const litem *f = word("floated"), *c = word("centred"), *e = word("strong"), *pl = word("plain");
        ok("display: none with a space after the colon hides", !word("gone") && pl);
        ok("and float: right floats", f && f->x > 400);
        ok("and text-align: center centres", c && c->x > 200);
        ok("and font-weight: bold is bold", e && pl && e->face != pl->face);
    }

    /* --- placing an item by line -----------------------------------------------
     *
     * A page built on a grid with named lines, [content-start] ...
     * [content-end], puts every section in the content column by name.
     * Poured into the first column instead -- a margin a few pixels wide --
     * a whole front page was a column one word across. */
    {
        lay("<style>.g{display:grid;grid-template-columns:[full-start] 1fr [content-start] 200px "
            "[content-end] 1fr [full-end]}.g>*{grid-column:content;background:#eeeeee}</style>"
            "<div class=g><div id=a>one</div><div id=b>two</div></div>", 600);
        const litem *a = box_of(by_id("a")), *b = box_of(by_id("b"));
        okn("an item placed by a named line is in that column", a && a->x == 200 && a->w == 200,
            a ? a->x : -1);
        ok("and the next one placed there starts a row of its own", b && a && b->x == 200 && b->y > a->y);

        lay("<style>.g{display:grid;grid-template-columns:repeat(4,100px)}.g div{background:#eeeeee}"
            "#a{grid-column:2 / 4}#b{grid-column:-2}#c{grid-column-start:1;grid-column-end:span 2}</style>"
            "<div class=g><div id=a>a</div><div id=b>b</div><div id=c>c</div></div>", 600);
        a = box_of(by_id("a"));
        b = box_of(by_id("b"));
        const litem *c = box_of(by_id("c"));
        okn("by numbers, 2 / 4 is the second and third columns", a && a->x == 100 && a->w == 200,
            a ? a->w : -1);
        okn("and -2 the last", b && b->x == 300 && b->y == a->y, b ? b->x : -1);
        okn("and the longhands, a start and a span", c && c->x == 0 && c->w == 200 && c->y > a->y,
            c ? c->w : -1);

        /* minmax(0, 300px) between two 1fr margins: as wide as there is
           room for, up to 300, before the margins share the rest. */
        lay("<style>.g{display:grid;grid-template-columns:1fr minmax(0,300px) 1fr}"
            ".g div{grid-column:2;background:#eeeeee}</style><div class=g><div id=a>mid</div></div>", 600);
        a = box_of(by_id("a"));
        okn("a capped track grows to its cap", a && a->x == 150 && a->w == 300, a ? a->w : -1);
        lay("<style>.g{display:grid;grid-template-columns:1fr minmax(0,300px) 1fr}"
            ".g div{grid-column:2;background:#eeeeee}</style><div class=g><div id=a>mid</div></div>", 250);
        a = box_of(by_id("a"));
        okn("and no further than the room there is", a && a->w == 250, a ? a->w : -1);
    }

    /* --- an item that names its row ----------------------------------------------
     *
     * The BBC's lead story puts its picture on row 1 at column 9 and its
     * words, which name no row, in columns 1 to 8 beside it. Placed row by
     * row in the order written, the words went under the picture. */
    {
        lay("<style>.g{display:grid;grid-template-columns:repeat(3,100px)}.g div{background:#eeeeee}"
            "#p{grid-column:2 / span 2;grid-row:1}#t{grid-column:1}</style>"
            "<div class=g><div id=p>picture</div><div id=t>words</div></div>", 600);
        const litem *p = box_of(by_id("p")), *t = box_of(by_id("t"));
        okn("an item on a named row is placed first, the rest beside it",
            p && t && p->x == 100 && p->w == 200 && t->x == 0 && t->y == p->y, t ? t->y - (p ? p->y : 0) : -1);

        lay("<style>.g{display:grid;grid-template-columns:repeat(3,100px)}.g div{background:#eeeeee}"
            "#q{grid-area:2 / 1 / 3 / 3}</style>"
            "<div class=g><div id=a>a</div><div id=q>q</div></div>", 600);
        const litem *a = box_of(by_id("a")), *q = box_of(by_id("q"));
        okn("grid-area written as lines", a && q && q->x == 0 && q->w == 200 && q->y > a->y, q ? q->w : -1);

        lay("<style>.g{display:grid;grid-template-columns:100px 100px}.g div{background:#eeeeee}"
            "#tall{grid-column:1;grid-row:1 / span 2}</style>"
            "<div class=g><div id=tall>tall</div><div id=b>b</div><div id=c>c</div></div>", 600);
        const litem *tl = box_of(by_id("tall")), *b = box_of(by_id("b")), *c = box_of(by_id("c"));
        ok("an item two rows tall leaves both rows' other column free",
           tl && b && c && b->x == 100 && c->x == 100 && b->y == tl->y && c->y > b->y);
    }

    /* --- pictures sized by the page's rules -------------------------------------- */
    {
        nfake = 0;
        lay("<style>.c{width:300px}.c img{width:100%}</style><div class=c><img id=p src=x></div>", 600);
        picture_at("p", 100, 50);
        lay("<style>.c{width:300px}.c img{width:100%}</style><div class=c><img id=p src=x></div>", 600);
        const litem *p = image_of(by_id("p"));
        okn("a picture 100% wide is as wide as its box, and in proportion", p && p->w == 300 && p->h == 150,
            p ? p->w : -1);

        nfake = 0;
        lay("<style>img{height:40px}</style><p><img id=p src=x></p>", 600);
        picture_at("p", 200, 100);
        lay("<style>img{height:40px}</style><p><img id=p src=x></p>", 600);
        p = image_of(by_id("p"));
        okn("a height alone makes the width from the picture", p && p->h == 40 && p->w == 80, p ? p->w : -1);

        nfake = 0;
        lay("<style>img{max-width:100px}</style><p><img id=p src=x></p>", 600);
        picture_at("p", 400, 200);
        lay("<style>img{max-width:100px}</style><p><img id=p src=x></p>", 600);
        p = image_of(by_id("p"));
        okn("and max-width holds one back", p && p->w == 100 && p->h == 50, p ? p->w : -1);
        nfake = 0;
    }

    /* --- a shadow tree written into the page ------------------------------------
     *
     * The element is drawn from the template's tree, its own children in the
     * tree's slots, and the tree's sheet reaches the tree alone. MDN closes
     * its menus with a rule inside one; without the tree the menus were
     * drawn open over the page. */
    {
        lay("<style>p{color:#00ff00}</style>"
            "<div id=h><template shadowrootmode=open><style>p{background:#1d4ed8}:host{background:#ff0000}</style>"
            "<p id=sp>shadow</p><slot name=top></slot><slot name=none>fallback</slot><slot>unused</slot>"
            "</template><span slot=top>first</span><b>light</b></div><p id=out>outside</p>", 600);
        const litem *sh = word("shadow"), *fi = word("first"), *li = word("light");
        ok("a shadow tree written into the page is drawn", sh != 0);
        ok("with the element's children in its slots, by name, in the tree's order",
           sh && fi && li && fi->y > sh->y && (li->y > fi->y || li->x > fi->x));
        ok("a slot's own contents only where nothing was put into it", word("fallback") && !word("unused"));
        const litem *sp = box_of(by_id("sp")), *out = box_of(by_id("out")), *h = box_of(by_id("h"));
        ok("the tree's sheet reaches the tree", sp && sp->has_bg && sp->bg == 0x1D4ED8);
        ok("and nothing outside it", !(out && out->has_bg));
        ok("and :host is the element the tree belongs to", h && h->has_bg && h->bg == 0xFF0000);
    }

    /* --- :not() with a list --------------------------------------------------- */
    {
        lay("<style>.h:not([loaded],.open) .s{display:none}</style>"
            "<div class=h><p class=s>shut</p></div><div class='h open'><p class=s>ajar</p></div>"
            "<div class=h loaded><p class=s>loaded</p></div>", 600);
        ok(":not() with a list matches none of them", !word("shut") && word("ajar") && word("loaded"));
    }

    /* --- text-transform, inset, order, align-self, flex basis, aspect-ratio ---- */
    {
        lay("<style>.u{text-transform:uppercase}.c{text-transform:capitalize}</style>"
            "<p class=u>shout <span>this</span></p><p class=c>each word</p>", 600);
        ok("text-transform: uppercase, and its children with it", word("SHOUT") && word("THIS") && !word("shout"));
        ok("and capitalize", word("Each") && word("Word"));

        lay("<style>.p{position:relative;width:400px;height:100px}"
            ".a{position:absolute;inset:10px 20px;background:#eeeeee}</style>"
            "<div class=p id=p><div class=a id=a>x</div></div>", 600);
        const litem *a = box_of(by_id("a"));
        okn("inset gives all four sides, and a box between them", a && a->x == 20 && a->y == 10 && a->w == 360,
            a ? a->w : -1);

        lay("<style>.row{display:flex}#x{order:2}#y{order:1}</style>"
            "<div class=row><div id=x>ex</div><div id=y>why</div><div id=z>zed</div></div>", 600);
        const litem *ex = word("ex"), *wy = word("why"), *ze = word("zed");
        ok("order puts a row's items in the order they ask for", ex && wy && ze && ze->x < wy->x && wy->x < ex->x);

        lay("<style>.row{display:flex;align-items:flex-start}.t{height:100px;background:#eeeeee}"
            ".s{align-self:flex-end;background:#dddddd}</style>"
            "<div class=row><div class=t id=t>tall</div><div class=s id=s>short</div></div>", 600);
        const litem *tb = box_of(by_id("t")), *sb = box_of(by_id("s"));
        ok("align-self puts one item at the end of the row", tb && sb && sb->y + sb->h == tb->y + tb->h && sb->y > tb->y);

        lay("<style>.row{display:flex;width:600px}.row div{flex:1;background:#eeeeee}</style>"
            "<div class=row><div id=f>a</div><div id=g>a much longer piece of text than the other</div></div>", 600);
        const litem *f = box_of(by_id("f")), *g = box_of(by_id("g"));
        okn("flex: 1 makes columns of one width, whatever is in them", f && g && f->w == g->w && f->w == 300,
            f ? f->w : -1);

        lay("<style>.row{display:flex;width:300px}.row div{width:200px;background:#eeeeee}"
            "#k{flex-shrink:0}</style><div class=row><div id=k>keep</div><div id=o>give</div></div>", 600);
        const litem *kb = box_of(by_id("k")), *ob = box_of(by_id("o"));
        okn("flex-shrink: 0 keeps its width, and the other gives way", kb && ob && kb->w == 200 && ob->w == 100,
            ob ? ob->w : -1);

        lay("<style>.b{width:200px;aspect-ratio:2 / 1;background:#eeeeee}.v{width:160px;aspect-ratio:16/9;"
            "background:#eeeeee}</style><div class=b id=b></div><div class=v id=v></div>", 600);
        const litem *bb = box_of(by_id("b")), *vb = box_of(by_id("v"));
        okn("aspect-ratio makes a box as tall as its width says", bb && vb && bb->h == 100 && vb->h == 90,
            bb ? bb->h : -1);
    }

    /* --- a basis below the contents ------------------------------------------ */
    {
        lay("<style>.r{display:flex;justify-content:space-between}.a{flex:1 1 0}"
            ".b{flex:0 1 auto;display:flex;background:#000}.k{width:200px;height:10px;background:#000}.m{width:150px;height:10px}</style>"
            "<div class=r><div class=a id=a1><div id=k class=k></div></div>"
            "<div class=b id=b1><div class=m></div><div class=m></div><div class=m></div><div class=m></div><div class=m></div></div></div>", 800);
        const litem *k = box_of(by_id("k")), *b1 = box_of(by_id("b1"));
        okn("a basis of 0 still keeps its contents' width", k && k->w == 200 && b1 && b1->x >= 200 && b1->x + b1->w <= 800, b1 ? b1->x * 1000 + b1->w : -1);
        lay("<style>.r{display:flex}.a{flex:1 1 0;background:#000}.w{width:500px;height:10px;background:#fff}</style>"
            "<div class=r><div class=a><div id=w1 class=w></div></div><div class=a id=e2>x</div></div>", 800);
        const litem *e = box_of(by_id("e2"));
        okn("the rest of the room goes to the others when one is held at its floor",
            e && e->x >= 500 && e->x + e->w <= 800 && e->w >= 250, e ? e->x * 1000 + e->w : -1);
    }

    /* --- a row pushed to its end, measured ------------------------------------ */
    {
        lay("<style>.ib{display:inline-block;background:#000}.row{display:flex;justify-content:flex-end;gap:10px}"
            ".s{width:50px;height:10px;background:#fff}</style>"
            "<div class=ib id=ib><div class=row><div class=s></div><div class=s></div></div></div>", 800);
        const litem *ib = box_of(by_id("ib"));
        okn("a row pushed to its end is as wide as its items when measured", ib && ib->w == 110, ib ? ib->w : -1);
    }

    /* --- a percentage of a width worked out from the contents --------------- */
    {
        lay("<style>.bar{display:flex}.host{display:inline-flex}.b{width:100%;padding:4px;background:#000}"
            ".i{display:block;width:20px;height:20px;background:#fff}.rest{flex:1;background:#888;height:10px}</style>"
            "<div class=bar><div><span class=host><span class=b id=pb><span class=i></span></span></span></div>"
            "<div class=rest id=pr></div></div>", 800);
        const litem *pb = box_of(by_id("pb")), *pr = box_of(by_id("pr"));
        okn("width: 100% inside a box sized by its contents counts as auto",
            pb && pb->w == 28 && pr && pr->x < 100, pb ? pb->w : -1);
        lay("<style>.w{width:400px}.p{width:150%;height:10px;background:#000}.ib{display:inline-block;background:#888}</style>"
            "<div class=ib id=qb><div class=w><div class=p id=pq></div></div></div>", 800);
        const litem *pq = box_of(by_id("pq")), *qb = box_of(by_id("qb"));
        okn("a percentage inside a width of its own still counts", pq && qb && qb->w == 600,
            qb ? qb->w : -1);
    }

    /* --- fields sized by the page ---------------------------------------------- */
    {
        lay("<style>.f{display:flex;width:600px}.f input{width:100%}.f .g{width:80px;flex-shrink:0;height:20px;background:#000}</style>"
            "<form class=f><input id=q><div class=g></div></form>", 800);
        const litem *q = 0;
        for (int i = 0; i < page.nitems; i++)
            if (page.items[i].node == by_id("q") && page.items[i].kind == LK_FIELD) q = &page.items[i];
        okn("a field in a flex row is a field, as wide as the row lets it be", q && q->w == 520, q ? q->w : -1);
        lay("<style>.f{display:flex;width:600px}.f input{width:100px;flex:1}.f .g{width:80px;flex-shrink:0;height:20px;background:#000}</style>"
            "<form class=f><input id=q2><div class=g></div></form>", 800);
        const litem *q2 = 0;
        for (int i = 0; i < page.nitems; i++)
            if (page.items[i].node == by_id("q2") && page.items[i].kind == LK_FIELD) q2 = &page.items[i];
        okn("a field that grows in its row is as wide as it grew", q2 && q2->w == 520, q2 ? q2->w : -1);
        lay("<style>input{width:300px;height:30px;box-sizing:border-box}</style><p><input id=r></p>", 800);
        const litem *r = 0;
        for (int i = 0; i < page.nitems; i++)
            if (page.items[i].node == by_id("r") && page.items[i].kind == LK_FIELD) r = &page.items[i];
        okn("a field takes the width and height its rules give it", r && r->w == 300 && r->h == 30,
            r ? r->w * 1000 + r->h : -1);
    }

    /* --- a box squeezed keeps its padding ---------------------------------------- */
    {
        lay("<style>.r{display:flex;width:300px}.w{width:400px;height:10px;background:#000}"
            ".b{padding:6px;background:#888}.g{display:block;width:24px;height:24px;background:#fff}</style>"
            "<div class=r><div class=w></div><div class=b id=pb2><span class=g></span></div></div>", 800);
        const litem *pb2 = box_of(by_id("pb2"));
        okn("squeezed, a box keeps its padding round what is in it", pb2 && pb2->w == 36, pb2 ? pb2->w : -1);
    }

    /* --- a button with things in it -------------------------------------------- */
    {
        lay("<style>.i{display:block;width:24px;height:24px;background:#000}</style>"
            "<div><button id=bt><span class=i id=ic></span><span>find</span></button></div>", 800);
        const litem *bt = box_of(by_id("bt")), *ic = box_of(by_id("ic"));
        okn("a button with an icon in it is a box with the icon drawn",
            bt && ic && ic->w == 24 && ic->x >= bt->x && ic->x + ic->w <= bt->x + bt->w && word("find") != 0,
            bt ? bt->w : -1);
    }

    /* --- layers --------------------------------------------------------------- */
    {
        lay("<style>@layer base{.a{display:none}}</style><p class=a>hidden</p><p>shown</p>", 600);
        ok("a rule inside @layer applies", !word("hidden") && word("shown"));
        lay("<style>@layer x{#b{display:none}}p{display:block}</style><p id=b>kept</p>", 600);
        ok("a rule in no layer beats a more specific one in a layer", word("kept") != 0);
        lay("<style>@layer one, two;@layer two{.c{display:block}}@layer one{p.c.d{display:none}}</style>"
            "<p class='c d'>later</p>", 600);
        ok("a layer named later beats one named first", word("later") != 0);
        lay("<style>@layer{.e{display:none}}@media (min-width:1px){@layer m{.f{display:none}}}</style>"
            "<p class=e>anon</p><p class=f>inner</p><p>after</p>", 600);
        ok("layers with no name and inside a query apply too", !word("anon") && !word("inner") && word("after"));
    }

    /* --- hex colours with an alpha ---------------------------------------------- */
    {
        u32 c = 0;
        int got = css_color("#0000", &c);
        okn("four hex digits are a colour and its alpha", got && css_last_alpha == 0, (int)css_last_alpha);
        got = css_color("#ff000080", &c);
        okn("eight hex digits are composited over white as rgba() is", got && css_last_alpha == 128 && c == 0xFF7F7F,
            (int)c);
        lay("<style>a{background-color:#0000}</style><p><a id=ln href=x>link</a></p>", 600);
        ok("a link with a transparent short hex background has no box", box_of(by_id("ln")) == 0);
    }

    /* --- words straight inside a row ------------------------------------------ */
    {
        lay("<style>.l{display:flex;align-items:center}.g{display:grid}.k{width:10px;height:10px;background:#000}</style>"
            "<a class=l href=x>Standards <span class=k id=ar></span></a><div class=l>alone</div>"
            "<div class=g>gridded</div>", 600);
        const litem *st1 = word("Standards"), *ar = box_of(by_id("ar"));
        ok("words straight inside a flex row are an item", st1 && ar && ar->x >= st1->x + st1->w);
        ok("and a row of nothing but words shows them", word("alone") != 0);
        ok("and so does a grid", word("gridded") != 0);
    }

    /* --- a percentage basis in a row sized by its contents ---------------------- */
    {
        lay("<style>.row{display:flex}.card{display:inline-flex;background:#888}.main{display:flex}"
            ".col{flex-basis:100%;display:flex}.pic{width:50px;height:50px;background:#000}</style>"
            "<div class=row><div class=card id=cd><div class=main><div class=col><div class=pic></div></div></div></div>"
            "<div class=card id=cd2><div class=main><div class=col><div class=pic></div></div></div></div></div>", 800);
        const litem *cd = box_of(by_id("cd")), *cd2 = box_of(by_id("cd2"));
        okn("a basis of 100% inside a card as wide as its contents is its contents",
            cd && cd2 && cd->w == 50 && cd2->x == cd->x + 50, cd ? cd->w : -1);
    }

    /* --- links that are blocks, items and pictures ------------------------------ */
    {
        lay("<style>a.b{display:block}.r{display:flex}</style><a class=b href=x>blocky</a>"
            "<div class=r><a href=y>flexed</a><a href=z>second</a></div><p>plain</p>", 600);
        const litem *bl = word("blocky"), *fl = word("flexed"), *sc = word("second"), *pl = word("plain");
        ok("a link that is a block is a link", bl && bl->link >= 0);
        ok("and so is one that is a flex item, each its own",
           fl && sc && fl->link >= 0 && sc->link >= 0 && fl->link != sc->link);
        ok("and what comes after them is not", pl && pl->link < 0);
        lay("<style>a.b{display:block}</style><a class=b href=x>one <b>two</b> three</a>", 600);
        const litem *w3 = word("three"), *w1 = word("one");
        ok("words after something inline in a block link are still the link",
           w1 && w3 && w1->link >= 0 && w3->link == w1->link);
        picture_at("lg", 80, 40);
        lay("<style>a.c{display:block;background:#eee;padding:10px}</style>"
            "<a class=c href=home><img id=lg src=x></a><p><a href=q>in <b>line</b></a> after</p>", 600);
        picture_at("lg", 80, 40);
        lay("<style>a.c{display:block;background:#eee;padding:10px}</style>"
            "<a class=c href=home><img id=lg src=x></a><p><a href=q>in <b>line</b></a> after</p>", 600);
        nfake = 0;
        const litem *lg = image_of(by_id("lg")), *af = word("after"), *ln = word("line");
        ok("a picture in a link can be clicked", lg && lay_link_at(&page, lg->x + 5, lg->y + 5) >= 0);
        ok("and so can the box round it", lg && lay_link_at(&page, lg->x + lg->w + 20, lg->y + 5) >= 0);
        ok("a link inside a line ends where it ends", ln && af && ln->link >= 0 && af->link < 0);
    }

    /* --- words belong to their element, and :hover to what is round it ---------- */
    {
        lay("<div id=wd><span id=sp>inner</span> outer</div>", 600);
        const litem *inr = word("inner"), *out = word("outer");
        okn("a word's element is the one it is in",
            inr && out && inr->node == by_id("sp") && out->node == by_id("wd")
            && lay_node_at(&page, inr->x + 2, inr->y + 2) == by_id("sp"),
            inr && out ? inr->node * 1000 + out->node : -1);
        hover_id = "deep";
        lay("<style>.card:hover .t{display:none}</style><div class=card><p class=t>gone</p><p><b id=deep>here</b></p></div>", 600);
        hover_id = 0;
        ok(":hover reaches an element with the pointer on something inside it", !word("gone") && word("here"));
        int reach[8];
        int nr = css_hover_reach(&sheet, &doc, by_id("deep"), reach, 8);
        ok("and what it reaches is worked out", nr == 1 && doc.nodes[reach[0]].tag == T_DIV);
    }

    /* --- auto margins in a row, and a column lined up -------------------------- */
    {
        lay("<style>.r{display:flex;width:600px}.a{width:100px;height:10px;background:#000}"
            ".b{width:100px;height:10px;background:#888;margin-left:auto}</style>"
            "<div class=r><div class=a id=ra></div><div class=b id=rb></div></div>", 800);
        const litem *ra = box_of(by_id("ra")), *rb = box_of(by_id("rb"));
        okn("margin-left: auto pushes an item to the far end of its row",
            ra && rb && rb->x - ra->x == 500, ra && rb ? rb->x - ra->x : -1);
        lay("<style>.r{display:flex;width:600px;background:#eee}.m{width:100px;height:10px;background:#000;margin:0 auto}</style>"
            "<div class=r id=rr><div class=m id=rm></div></div>", 800);
        const litem *rm = box_of(by_id("rm")), *rr = box_of(by_id("rr"));
        okn("and margin: auto centres one", rm && rr && rm->x - rr->x == 250, rm && rr ? rm->x - rr->x : -1);
        lay("<style>.c{display:flex;flex-direction:column;align-items:center;width:600px;background:#eee}"
            ".t{background:#000}.e{align-self:flex-end;background:#888}</style>"
            "<div class=c id=cc><p class=t id=ct>short</p><p class=e id=ce>end</p></div>", 800);
        const litem *ct = box_of(by_id("ct")), *ce = box_of(by_id("ce")), *cc = box_of(by_id("cc"));
        int mid = ct && cc ? (ct->x - cc->x) * 2 + ct->w - cc->w : 99;
        okn("a centred column centres what is in it, as wide as it is",
            ct && cc && ct->w < 200 && mid >= -2 && mid <= 2, ct && cc ? (ct->x - cc->x) * 1000 + ct->w : -1);
        okn("and align-self: flex-end puts one at the end", ce && cc && ce->x + ce->w == cc->x + cc->w && ce->w < 200,
            ce && cc ? ce->x * 1000 + ce->w : -1);
    }

    /* --- background pictures and masks ------------------------------------------ */
    {
        lay("<style>.h{height:40px;background:#123 url('/img/hero.png') no-repeat center / cover}"
            ".i{width:20px;height:20px;background-color:#f00;mask:url(icon.svg) no-repeat 0 100% / 16px auto}"
            ".p{height:10px;background-image:url(a.png);background-position:right 5px;background-size:50%}"
            ".n{height:10px;background:url(x.png);background:#fff}</style>"
            "<div class=h id=bh></div><div class=i id=bi></div><div class=p id=bp></div><div class=n id=bn></div>", 600);
        const litem *bh = box_of(by_id("bh")), *bi = box_of(by_id("bi")), *bp = box_of(by_id("bp"));
        const litem *bn = box_of(by_id("bn"));
        const lbg *h = bh && bh->bgi >= 0 ? &page.bgs[bh->bgi] : 0;
        const lbg *i = bi && bi->bgi >= 0 ? &page.bgs[bi->bgi] : 0;
        const lbg *p = bp && bp->bgi >= 0 ? &page.bgs[bp->bgi] : 0;
        ok("a background picture is kept with its box, placed and sized",
           h && w_same(page.text + h->url, "/img/hero.png") && h->fit == 1 && h->rep == 1
           && h->px == 50 && h->ppx && h->py == 50 && !h->mask && bh->has_bg);
        ok("a mask is a picture the colour is painted through",
           i && i->mask && w_same(page.text + i->url, "icon.svg") && i->px == 0 && i->py == 100
           && i->sw == 16 && !i->swp && i->sh == -1 && bi->has_bg && bi->bg == 0xFF0000);
        ok("and the longhands are read", p && w_same(page.text + p->url, "a.png") && p->px == 100 && p->ppx
           && p->py == 5 && !p->ppy && p->sw == 50 && p->swp);
        ok("a background shorthand with no picture takes the picture away", bn && bn->bgi < 0);

        lay("<style>.g{height:10px;background:linear-gradient(to right,#f00,rgba(0,0,255,.5) 30%,transparent)}"
            ".o{height:10px;background:linear-gradient(rgba(0,0,0,.5),rgba(0,0,0,.5)),url(h.jpg) center/cover}"
            ".w{height:10px;background-image:-webkit-linear-gradient(left,#000,#fff)}</style>"
            "<div class=g id=gg></div><div class=o id=go></div><div class=w id=gw></div>", 600);
        const litem *gg = box_of(by_id("gg")), *go = box_of(by_id("go")), *gw = box_of(by_id("gw"));
        const lbg *g1 = gg && gg->bgi >= 0 ? &page.bgs[gg->bgi] : 0;
        const lbg *g2 = go && go->bgi >= 0 ? &page.bgs[go->bgi] : 0;
        const lbg *g3 = gw && gw->bgi >= 0 ? &page.bgs[gw->bgi] : 0;
        ok("a linear gradient is kept with its direction and its stops",
           g1 && g1->url < 0 && g1->nstops == 3 && g1->angle == 90 && g1->stop_p[0] == 0
           && g1->stop_p[1] == 30 && g1->stop_p[2] == 100 && g1->stop_c[1] == 0x0000FF
           && g1->stop_a[1] > 120 && g1->stop_a[1] < 135 && g1->stop_a[2] == 0);
        ok("and one over a picture keeps both, the gradient on top",
           g2 && g2->nstops == 2 && g2->angle == 180 && !g2->under && g2->url >= 0
           && w_same(page.text + g2->url, "h.jpg") && g2->fit == 1);
        ok("and the prefixed form names where it starts", g3 && g3->nstops == 2 && g3->angle == 90);
    }

    /* --- a picture's fit and corners ------------------------------------------- */
    {
        lay("<style>img{width:90px;height:90px;object-fit:cover;border-radius:12px}</style><p><img id=of src=x></p>", 600);
        picture_at("of", 160, 90);
        lay("<style>img{width:90px;height:90px;object-fit:cover;border-radius:12px}</style><p><img id=of src=x></p>", 600);
        nfake = 0;
        const litem *of = image_of(by_id("of"));
        ok("a picture keeps its object-fit and its corners for the drawing",
           of && of->w == 90 && of->h == 90 && of->ofit == 2 && of->radius == 12);
    }

    /* --- the page's own background ---------------------------------------------- */
    {
        lay("<html><head><style>html{background:#123456}</style></head><body><p>x</p></body></html>", 600);
        ok("a background on html is the page's", page.has_canvas && page.canvas == 0x123456);
        lay("<html><head><style>body{background:#202020}</style></head><body><p>x</p></body></html>", 600);
        ok("and one on the body is, when html has none", page.has_canvas && page.canvas == 0x202020);
        lay("<p>plain</p>", 600);
        ok("and a page with neither has none", !page.has_canvas);
    }

    puts(failed ? "LAYOUTTEST_FAIL\n" : "LAYOUTTEST_PASS\n");
    return failed;
}
