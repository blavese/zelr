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

/* The same three steps the browser takes, without the fetching. */
static void lay(const char *html, int width) {
    int n = 0;
    while (html[n]) n++;
    dom_parse(&doc, html, n);

    css_init(&sheet);
    css_parse(&sheet, CSS_UA, (int)sizeof(CSS_UA) - 1);

    /* Style sheets written in the page itself. */
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_STYLE)
            continue;
        int child = doc.nodes[i].first;
        if (child >= 0 && doc.nodes[child].kind == DN_TEXT
            && doc.nodes[child].text >= 0) {
            const char *t = doc.arena + doc.nodes[child].text;
            css_parse(&sheet, t, w_len(t));
        }
    }

    for (int i = 0; i < doc.count; i++) { inl[i].at = 0; inl[i].n = 0; }
    css_index(&sheet, &index_);
    match.hover = -1;
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

    puts(failed ? "LAYOUTTEST_FAIL\n" : "LAYOUTTEST_PASS\n");
    return failed;
}
