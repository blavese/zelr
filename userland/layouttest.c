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

static void lay_again(int width);

/* The same three steps the browser takes, without the fetching. */
static void lay(const char *html, int width) {
    int n = 0;
    while (html[n]) n++;
    dom_parse(&doc, html, n);
    lay_again(width);
}

/* And the page as it now is laid out again, as after a script has changed
   it. */
static void lay_again(int width) {
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
    /* What ::before and ::after put in, as the browser makes them in the copy
       it lays out (here the page itself, which no script sees). */
    lay_generated(&doc, &sheet, &index_, &match, inl);
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

    /* --- text a script put in --------------------------------------------
     *
     * textContent puts a script's text in the tree as it was given, so the
     * layout meets what the parser never lets through: UTF-8 past ASCII.
     * Folded as the parser folds a page's own text, or each curly
     * apostrophe, dash and accented letter is a gap. */
    {
        lay("<body><p id=s>x</p><pre id=p>x</pre></body>", 600);
        static const char said[] = "you\xe2\x80\x99ll \xe2\x80\x9cquote\xe2\x80\x9d caf\xc3\xa9 dash\xe2\x80\xa6"
                                   " \xf0\x9f\x98\x80smile";
        static const char pre[] = "a\xe2\x80\x94" "b";
        dom_set_text(&doc, by_id("s"), said, (int)sizeof(said) - 1);
        dom_set_text(&doc, by_id("p"), pre, (int)sizeof(pre) - 1);
        lay_again(600);
        ok("a script's curly apostrophe is drawn as one the face has", word("you'll") != 0);
        ok("and its quotes, accents and ellipsis", word("\"quote\"") && word("cafe") && word("dash..."));
        ok("and an emoji is left out, keeping the word after it", word("smile") != 0);
        ok("and the same in preformatted text", word("a--b") != 0);
    }

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

    /* The border shorthand's colour. The whole value went on as the colour,
       which is no colour, and every border it set was drawn in the default
       grey. */
    {
        lay("<style>#s{border:4px solid #1D4ED8}#t { border: 2px dashed rgb(0, 128, 0) }</style>"
            "<div id=s>a</div><div id=t>b</div>", 600);
        const litem *a = box_of(by_id("s")), *b = box_of(by_id("t"));
        okn("a border's colour comes from the shorthand, written close or spaced out",
            a && b && a->border == 0x1D4ED8 && b->border == 0x008000, a ? (int)a->border : -1);
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

        /* A picture made a block keeps its own margins above and below,
           and auto ones at its sides centre it: the commonest way a page
           puts a picture in the middle with room around it. Both ways a
           block picture is laid out: a block's child, and a box of its own
           (here a flex item). With spaced CSS, as pages write it. */
        lay("<style>body { margin: 0 } p { margin: 0 } svg { display: block; margin: 12px auto 20px auto }</style>"
            "<div><p>before</p><svg id=s width=100 height=40></svg><p>after</p></div>", 400);
        s = image_of(by_id("s"));
        const litem *before = word("before"), *after = word("after");
        okn("a block picture keeps its top margin (its gap from the line above)",
            s && before && s->y - (before->y + before->h) >= 12 && s->y - (before->y + before->h) < 20,
            s && before ? s->y - (before->y + before->h) : -1);
        okn("and its bottom margin (the gap to the line below)",
            s && after && after->y - (s->y + s->h) >= 20 && after->y - (s->y + s->h) < 28,
            s && after ? after->y - (s->y + s->h) : -1);
        okn("and auto margins at its sides centre it", s && s->x == 150, s ? s->x : -1);
        lay("<style>body { margin: 0 } .f { display: flex; flex-direction: column } "
            ".f svg { margin: 10px auto 0 auto }</style>"
            "<div class=f><svg id=s width=60 height=30></svg></div>", 400);
        s = image_of(by_id("s"));
        okn("so does one that is a box of its own, a flex item (its x)", s && s->x == 170 && s->y >= 10, s ? s->x : -1);

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

    /* --- comments ----------------------------------------------------------
     *
     * In the document now, where scripts find them, and still nothing on the
     * page: no cell of a grid, no place or gap in a row, no line between
     * blocks, and not content to :empty or a child to :first-child. React's
     * streamed pages put them everywhere. */
    {
        lay("<style>.g{display:grid;grid-template-columns:100px 100px}.g div{background:#eeeeee}"
            ".f{display:flex;gap:10px}.f span{display:block;width:30px;height:10px;background:#dddddd}"
            "li:first-child{color:#ff0000}p{background:#cccccc}p:empty{height:7px}"
            ".s div{background:#bbbbbb}.ib{display:inline-block;width:50px;height:10px;background:#aaaaaa}</style>"
            "<div class=g><!--$--><div id=g1>a</div><!--/$--><div id=g2>b</div><!-- --><div id=g3>c</div></div>"
            "<div class=f><!--x--><span id=f1></span><!--y--><span id=f2></span></div>"
            "<ul><!--a--><li>first</li></ul><p id=e><!--nothing here--></p>"
            "<div class=s><div id=b1>x</div> <!--between--> <div id=b2>y</div></div>"
            "<div class=s><div id=c1>x</div> <div id=c2>y</div></div>"
            "<div><div class=ib id=i1></div> <!--x--> <div class=ib id=i2></div></div>"
            "<div><div class=ib id=j1></div> <div class=ib id=j2></div></div>", 600);
        const litem *g1 = box_of(by_id("g1")), *g2 = box_of(by_id("g2")), *g3 = box_of(by_id("g3"));
        ok("a comment in a grid takes no cell",
           g1 && g2 && g3 && g2->y == g1->y && g2->x == g1->x + 100 && g3->y > g1->y && g3->x == g1->x);
        const litem *f1 = box_of(by_id("f1")), *f2 = box_of(by_id("f2"));
        okn("nor a place in a row, nor a gap", f1 && f2 && f2->x - f1->x == 40, f1 && f2 ? f2->x - f1->x : -1);
        const litem *first = word("first");
        ok("a list's first item is still its first child", first && first->color == 0xFF0000);
        const litem *e = box_of(by_id("e"));
        okn("an element with only a comment in it is empty", e && e->h == 7, e ? e->h : -1);
        const litem *b1 = box_of(by_id("b1")), *b2 = box_of(by_id("b2"));
        const litem *c1 = box_of(by_id("c1")), *c2 = box_of(by_id("c2"));
        okn("and one between two blocks adds no line",
            b1 && b2 && c1 && c2 && b2->y - b1->y == c2->y - c1->y, b1 && b2 ? b2->y - b1->y : -1);
        const litem *i1 = box_of(by_id("i1")), *i2 = box_of(by_id("i2"));
        const litem *j1 = box_of(by_id("j1")), *j2 = box_of(by_id("j2"));
        okn("nor a space between two boxes on one line",
            i1 && i2 && j1 && j2 && i2->x - i1->x == j2->x - j1->x, i1 && i2 ? i2->x - i1->x : -1);
    }

    /* --- a grid's rows ----------------------------------------------------------
     *
     * Every row was as tall as what was in it, whatever grid-template-rows
     * and grid-auto-rows said. */
    {
        lay("<style>.g{display:grid;grid-template-columns:100px 100px;grid-template-rows:50px 120px;gap:10px}"
            ".h { display: grid; grid-template-columns: 100px; grid-template-rows: 1fr 2fr; height: 300px }"
            ".a{display:grid;grid-template-columns:100px;grid-auto-rows:40px}"
            ".m{display:grid;grid-template-columns:100px;grid-template-rows:minmax(80px,auto) repeat(2,30px)}.m div{background:#000}"
            "p{margin:0}</style>"
            "<div class=g><div id=g1>a</div><div id=g2>b</div><div id=g3>c</div></div><p id=after>x</p>"
            "<div class=h><div id=h1>a</div><div id=h2>b</div></div>"
            "<div class=a><div id=a1>a</div><div id=a2>b</div></div>"
            "<div class=m><div id=m1>a</div><div id=m2>b</div><div id=m3>c</div></div>", 600);
        const litem *g1 = word("a"), *after = word("x");
        int g1y = g1 ? g1->y : -1;
        const litem *w_c = 0, *w_b[3] = { 0, 0, 0 };
        int nb = 0;
        for (int i = 0; i < page.nitems; i++) {
            const litem *it = &page.items[i];
            if (it->kind != LK_TEXT || it->at < 0) continue;
            if (w_same(page.text + it->at, "c") && !w_c) w_c = it;
            if (w_same(page.text + it->at, "b") && nb < 3) w_b[nb++] = it;
        }
        okn("a row of a written length is that tall, and the grid holds every row it names",
            g1 && w_c && after && w_c->y - g1y == 60 && after->y - g1y >= 180, w_c && g1 ? w_c->y - g1y : -1);
        const litem *a[4] = { 0, 0, 0, 0 };
        int na = 0;
        for (int i = 0; i < page.nitems && na < 4; i++)
            if (page.items[i].kind == LK_TEXT && page.items[i].at >= 0 && w_same(page.text + page.items[i].at, "a")) a[na++] = &page.items[i];
        okn("shares of a grid's own height, rows of grid-auto-rows, and minmax and repeat",
            na == 4 && nb == 3 && w_b[1]->y - a[1]->y == 100 && w_b[2]->y - a[2]->y == 40 && w_b[0]->y - a[0]->y == 0,
            na == 4 && nb == 3 ? w_b[1]->y - a[1]->y : -1);
        const litem *m1 = box_of(by_id("m1")), *m2 = box_of(by_id("m2")), *m3 = box_of(by_id("m3"));
        okn("a minmax row is at least its least, and a repeat of lengths is each that tall",
            m1 && m2 && m3 && m2->y - m1->y == 80 && m3->y - m2->y == 30, m1 && m2 ? m2->y - m1->y : -1);
    }

    /* --- stretch ------------------------------------------------------------------
     *
     * A row's items are as tall as the row unless they or it say otherwise;
     * each card had been as tall as its own words. */
    {
        lay("<style>.r{display:flex}.r div{background:#000;width:100px}.s{align-self:flex-start}.t{height:20px}"
            ".k { display: grid; grid-template-columns: 100px 100px } .k div { background: #000 }</style>"
            "<div class=r><div id=f1>a</div><div id=f2>b<br>b<br>b</div><div id=f3 class=s>c</div><div id=f4 class=t>d</div></div>"
            "<div class=k><div id=k1>a</div><div id=k2>b<br>b<br>b</div></div>", 600);
        const litem *f1 = box_of(by_id("f1")), *f2 = box_of(by_id("f2")), *f3 = box_of(by_id("f3")), *f4 = box_of(by_id("f4"));
        const litem *k1 = box_of(by_id("k1")), *k2 = box_of(by_id("k2"));
        okn("a flex row's items stretch to its tallest, but not one that starts or has a height",
            f1 && f2 && f3 && f4 && f1->h == f2->h && f3->h < f2->h && f4->h < f2->h, f1 && f2 ? f2->h - f1->h : -1);
        okn("and so do a grid row's", k1 && k2 && k1->h == k2->h && k2->h > 30, k1 && k2 ? k2->h - k1->h : -1);
    }

    /* --- beside a float -------------------------------------------------------------
     *
     * A box that is its own formatting context sits beside a float, and below
     * it when it does not fit there; an ordinary block still runs under it.
     * The media object's box was drawn under its picture. */
    {
        lay("<style>.f{float:left;width:100px;height:50px;background:#000}.o{overflow:hidden;background:#000}"
            ".n{background:#000}.w { display: flow-root; width: 550px; background: #000 }</style>"
            "<div><div class=f id=bf></div><div class=o id=bo>beside</div><div class=n id=bn>under</div></div>"
            "<div><div class=f id=bg></div><div class=w id=bw>below</div></div>", 600);
        const litem *bf = box_of(by_id("bf")), *bo = box_of(by_id("bo")), *bn = box_of(by_id("bn"));
        const litem *bg = box_of(by_id("bg")), *bw = box_of(by_id("bw"));
        okn("a box that is its own formatting context sits beside a float, an ordinary one under it",
            bf && bo && bn && bo->x - bf->x == 100 && bo->x + bo->w == bn->x + bn->w && bn->x == bf->x,
            bf && bo ? bo->x - bf->x : -1);
        okn("and below the float when it is too wide for the room beside it",
            bg && bw && bw->y >= bg->y + 50, bg && bw ? bw->y - bg->y : -1);
    }

    /* --- collapsed borders ------------------------------------------------------------
     *
     * The cells of a collapsed table share their borders, and the table's own
     * border stands in for theirs at its edge; every inner line had been
     * drawn twice. */
    {
        lay("<style>.c{border-collapse:collapse}.c td{border:1px solid #000}.b{border:1px solid #000}"
            ".s td { border: 1px solid #000 }</style>"
            "<table class=c><tr><td id=c1>a</td><td id=c2>b</td></tr><tr><td id=c3>c</td><td id=c4>d</td></tr></table>"
            "<table class='c b'><tr><td id=d1>a</td><td id=d2>b</td></tr></table>"
            "<table class=s><tr><td id=s1>first</td><td id=s2>second</td></tr></table>", 600);
        const litem *c1 = box_of(by_id("c1")), *c2 = box_of(by_id("c2")), *c3 = box_of(by_id("c3")), *c4 = box_of(by_id("c4"));
        const litem *d1 = box_of(by_id("d1")), *d2 = box_of(by_id("d2"));
        const litem *s1 = box_of(by_id("s1")), *s2 = box_of(by_id("s2"));
        okn("a collapsed table's cells draw each inner line once",
            c1 && c2 && c3 && c4 && c1->bl == 1 && c1->bt == 1 && c2->bl == 0 && c2->br == 1 && c3->bt == 0 && c3->bb == 1
            && c4->bl == 0 && c4->bt == 0 && c4->br == 1 && c4->bb == 1, c2 ? c2->bl : -1);
        okn("a collapsed table's own border stands in for its cells' at its edge",
            d1 && d2 && d1->bl == 0 && d1->bt == 0 && d1->bb == 0 && d1->br == 1 && d2->br == 0 && d2->bl == 0,
            d1 ? d1->bl : -1);
        okn("and a table that keeps its borders apart draws them all",
            s1 && s2 && s2->bl == 1 && s1->br == 1 && s2->x > s1->x + s1->w, s2 ? s2->bl : -1);
    }

    /* --- noscript ---------------------------------------------------------------------
     *
     * What is in a <noscript> is for a browser that does not run scripts, and
     * is its text in one that does. DuckDuckGo's home page hides its body
     * there, and was blank. */
    {
        lay("<style>.v{background:#000}</style>"
            "<noscript><style>p { display: none }</style><div id=ns>no scripts</div></noscript>"
            "<p id=nw class=v>words</p>", 600);
        const litem *nw = box_of(by_id("nw"));
        ok("a style sheet inside noscript is not read", nw && nw->h > 0);
        int nsel = -1;
        for (int i = 0; i < doc.count; i++)
            if (doc.nodes[i].kind == DN_ELEMENT && doc.nodes[i].tag == T_NOSCRIPT) nsel = i;
        int kid = nsel >= 0 ? doc.nodes[nsel].first : -1;
        ok("and what is in it is its text, not elements",
           by_id("ns") < 0 && kid >= 0 && doc.nodes[kid].kind == DN_TEXT
           && doc.nodes[kid].text >= 0 && doc.arena[doc.nodes[kid].text] == '<');
        lay("<html><head><base href=\"/x/\"><title>t</title><link id=l1 rel=stylesheet href=a.css></head>"
            "<body><picture id=pc><source srcset=a.webp><img id=pi src=a.png></picture><p id=pw>one<wbr>two</p></body></html>", 600);
        int l1 = by_id("l1"), pi = by_id("pi"), pc = by_id("pc"), pw = by_id("pw");
        int last = pw >= 0 ? doc.nodes[pw].last : -1;
        ok("a base, a source and a wbr hold nothing: what follows each is beside it",
           l1 >= 0 && doc.nodes[l1].parent == doc.head && pi >= 0 && doc.nodes[pi].parent == pc
           && last >= 0 && doc.nodes[last].kind == DN_TEXT && w_same(doc.arena + doc.nodes[last].text, "two"));
        lay("<div id=cd><a id=ca href=#m>skip</a\n      ><header id=ch>head</header></div>", 600);
        int ca = by_id("ca"), ch = by_id("ch");
        ok("a close tag whose name is followed by a line break closes the element",
           ca >= 0 && ch >= 0 && doc.nodes[ch].parent == by_id("cd"));
        {
            /* A long article: twenty-five thousand paragraphs, fifty thousand
               nodes, read and laid out whole. Wikipedia's United States was
               cut off part way at forty thousand. */
            static char longpage[25000 * 9 + 64];
            int n = 0;
            for (int i = 0; i < 24999; i++)
                for (const char *h = "<p>x</p>"; *h; h++) longpage[n++] = *h;
            for (const char *h = "<p>last</p>"; *h; h++) longpage[n++] = *h;
            longpage[n] = 0;
            lay(longpage, 600);
            ok("a page of fifty thousand nodes is read and laid out whole", !doc.overflowed && !page.overflowed && word("last"));
        }
        lay("<style>.alert { display: block; padding: 8px }</style><noscript class=alert><h2>needs scripts</h2></noscript><p>shown</p>", 600);
        ok("and a noscript the page's rules give a display is still not drawn", !word("scripts</h2>") && !word("needs") && word("shown"));
    }

    /* --- white space kept for scripts, passed over by layout ---------------------------
     *
     * Every run of white space is in the tree as it was written, as in any
     * browser: Svelte's templates hold a space where each piece of text goes,
     * and with no node there crates.io drew nothing. Layout draws only what
     * sits between inline things, so an indented page and the same page
     * written tight come out alike. */
    {
        lay("<body><div id=t><p>one</p><p>two <b>x</b> <i>y</i></p><ul><li>item</li></ul></div><p>after</p></body>", 600);
        const litem *o1 = word("one"), *t1 = word("two"), *y1 = word("y"), *i1 = word("item"), *a1 = word("after");
        int tight[5] = { o1 ? o1->y : -1, t1 ? t1->y : -1, y1 ? y1->x : -1, i1 ? i1->y : -1, a1 ? a1->y : -1 };
        lay("<body>\n  <div id=t>\n    <p>one</p>\n    <p>two <b>x</b>\n      <i>y</i></p>\n    <ul>\n      <li>item</li>\n    </ul>\n"
            "  </div>\n  <!-- a comment -->\n  <p>after</p>\n</body>", 600);
        const litem *o2 = word("one"), *t2 = word("two"), *x2 = word("x"), *y2 = word("y"), *i2 = word("item"), *a2 = word("after");
        int kids = 0, first_kind = -1, t = by_id("t");
        for (int c = t >= 0 ? doc.nodes[t].first : -1; c >= 0; c = doc.nodes[c].next) kids++;
        if (t >= 0 && doc.nodes[t].first >= 0) first_kind = doc.nodes[doc.nodes[t].first].kind;
        ok("an indented page keeps its white space in the tree, as it was written",
           kids == 7 && first_kind == DN_TEXT && w_same(doc.arena + doc.nodes[doc.nodes[t].first].text, "\n    "));
        ok("and is laid out as the same page written tight",
           o2 && t2 && y2 && i2 && a2 && o2->y == tight[0] && t2->y == tight[1] && y2->x == tight[2]
           && i2->y == tight[3] && a2->y == tight[4]);
        ok("and the white space between two inline things is still a space", x2 && y2 && y2->x > x2->x + x2->w);

        /* Inside boxes sized by what they hold: an inline-block, a cell, a
           centred line. White space at their edges is nothing in a browser,
           and measured as a space it widened each and moved what follows. */
        static const char *const shapes[2] = {
            "<style>.ib{display:inline-block;background:#ff0000}.c{text-align:center}.blk{display:block}</style>"
            "<body><p><span class=ib id=ib><b>word</b></span>after</p>"
            "<table><tr><td><b>cell</b></td><td>next</td></tr></table><div class=c><b>mid</b></div>"
            "<table class=blk><tr><td>one</td><td>two</td></tr></table></body>",
            "<style>.ib{display:inline-block;background:#ff0000}.c{text-align:center}.blk{display:block}</style>"
            "<body>\n<p>\n  <span class=ib id=ib>\n    <b>word</b>\n  </span>after</p>\n"
            "<table>\n  <tr>\n    <td>\n      <b>cell</b>\n    </td>\n    <td>next</td>\n  </tr>\n</table>\n"
            "<div class=c>\n  <b>mid</b>\n</div>\n<table class=blk>\n  <tr>\n    <td>one</td>\n    <td>two</td>\n  </tr>\n</table>\n</body>" };
        int at[2][5];
        for (int v = 0; v < 2; v++) {
            lay(shapes[v], 600);
            const litem *ib = box_of(by_id("ib")), *af = word("after"), *nx = word("next"), *md = word("mid");
            at[v][0] = ib ? ib->w : -1;
            at[v][1] = af ? af->x : -1;
            at[v][2] = nx ? nx->x : -1;
            at[v][3] = md ? md->x : -1;
            const litem *tw = word("two");
            at[v][4] = tw ? tw->x : -1;
        }
        lay("<p><span>def</span><span> </span><span>fib</span> and <span>x</span><b> </b><i>y</i></p>"
            "<table><tr><td>a</td><td> </td><td>b</td></tr></table><table><tr><td>a</td><td></td><td>b</td></tr></table>", 600);
        const litem *d1 = word("def"), *f1 = word("fib"), *xx = word("x"), *yy = word("y");
        int bx[2] = { -1, -1 }, nb = 0;
        for (int i = 0; i < page.nitems && nb < 2; i++)
            if (page.items[i].kind == LK_TEXT && page.items[i].at >= 0 && w_same(page.text + page.items[i].at, "b"))
                bx[nb++] = page.items[i].x;
        ok("a span holding only a space is a word space between the words beside it, and a cell holding one is empty",
           d1 && f1 && f1->x > d1->x + d1->w && xx && yy && yy->x > xx->x + xx->w && nb == 2 && bx[0] == bx[1]);
        ok("and an indented inline-block, cell, centred line and table made a block come out as written tight",
           at[0][0] > 0 && at[0][0] == at[1][0] && at[0][1] == at[1][1] && at[0][2] == at[1][2] && at[0][3] == at[1][3]
           && at[0][4] > 0 && at[0][4] == at[1][4]);
        for (int v = 0; v < 2; v++) {
            puts(v ? ", indented" : "      tight");
            for (int k = 0; k < 5; k++) { puts(" "); putn(at[v][k]); }
        }
        puts("\n");
    }

    /* --- white-space: pre-wrap and pre-line, and a value not known ---------------------
     *
     * pre-wrap keeps the spaces and the line breaks and still wraps; pre-line
     * keeps the line breaks only. Both were read as pre, which never wraps. A
     * value this does not know is dropped, as in a browser: python.org's code
     * sample has pre-wrap followed by -moz-pre-wrap !important, which was read
     * as normal and ran the sample's lines together. */
    {
        lay("<style>.pw{white-space:pre-wrap;width:150px}.pl{white-space:pre-line;width:150px}"
            "pre{white-space:pre-wrap;white-space:-moz-pre-wrap !important}</style>"
            "<div class=pw>first line\n    indented and long enough to wrap round</div>"
            "<div class=pl>alpha     beta\ngamma</div><pre>one\ntwo</pre>", 600);
        const litem *fl = word("first"), *in = word("    indented"), *ro = word("wrap");
        ok("pre-wrap keeps a line break and the spaces that indent a line, and wraps what does not fit",
           fl && in && ro && in->y > fl->y && in->x == fl->x && ro->y > in->y && ro->x == fl->x);
        const litem *al = word("alpha"), *be = word("beta"), *ga = word("gamma");
        int one_space = al && be ? be->x - (al->x + al->w) : -1;
        ok("pre-line keeps the line break but not the run of spaces",
           al && be && ga && be->y == al->y && one_space > 0 && one_space < 12 && ga->y > al->y && ga->x == al->x);
        const litem *o1 = word("one"), *t2 = word("two");
        ok("and a white-space value not known is dropped, not read as normal", o1 && t2 && t2->y > o1->y);
    }

    /* --- inline-table -------------------------------------------------------------------
     *
     * On anything but a table it holds its children in a cell it makes, an
     * inline-block to everything here. CNN's ribbon of headlines is a row of
     * them, each holding the headline and a "|" after it; read as a table, the
     * bars went and the headlines ran together. */
    {
        lay("<style>.row{display:flex;list-style:none}.t{display:inline-table}.hl{display:inline-block}"
            ".it:not(:last-child) .t:after{content:\"|\";padding:0 8px;display:inline-block}</style>"
            "<ul class=row><li class=it><a href=#><div class=t><div class=hl><span>Alpha</span></div></div></a></li>"
            "<li class=it><a href=#><div class=t><div class=hl><span>Beta</span></div></div></a></li></ul>", 600);
        const litem *al = word("Alpha"), *be = word("Beta"), *bar = word("|");
        ok("an inline-table on a div is an inline-block: the bar after a headline sits on its line, before the next",
           al && be && bar && bar->y == al->y && bar->x > al->x + al->w && be->x > bar->x + bar->w && be->y == al->y);
    }
    {
        /* The font shorthand: a bare number is a weight. Forbes's `font: 400
           0.75rem/1.5 Graphik` was read as a size of 400 and drawn forty
           pixels high, written out and through var() alike. */
        lay("<style>:root{--w:400;--s:0.75rem;--lh:1.5;--f:Graphik, sans-serif;--b:700}"
            ".a{font:400 0.75rem/1.5 Graphik, \"Graphik Fallback\", sans-serif}"
            ".b{font:var(--w) var(--s)/var(--lh) var(--f)}"
            ".c{font:var(--b) 20px/1.2 serif}.d{font:400 20px/1.2 serif}.e{font:0/0 a}</style>"
            "<p class=a>written</p><p class=b>through</p><p class=c>heavy</p><p class=d>light</p><p class=e>hidden</p>", 600);
        const litem *wr = word("written"), *th = word("through"), *he = word("heavy"), *li = word("light");
        ok("a weight before the size in the font shorthand is a weight, written out or through var()",
           wr && th && wr->h == 12 && th->h == 12);
        ok("and 700 there is bold, and font: 0/0 hides the words",
           he && li && he->h == li->h && he->face != li->face && !word("hidden"));
    }
    {
        /* A carousel: a row four windows wide in a box that hides its
           overflow, one slide showing, moved on by a translate. Kept to the
           room, SoundCloud's four slides were squeezed side by side. */
        lay("<style>.wrap{overflow:hidden;width:400px}.car{display:flex;width:400%}.on{transform:translateX(-25%)}.s{width:25%}"
            ".open{width:400px}</style>"
            "<div class=wrap><div class=car><div class=s>one</div><div class=s>two</div><div class=s>three</div><div class=s>four</div></div></div>"
            "<div class=wrap><div class='car on'><div class=s>alpha</div><div class=s>beta</div><div class=s>gamma</div><div class=s>delta</div></div></div>"
            "<div class=open><div class=car><div class=s>red</div><div class=s>green</div></div></div>", 800);
        const litem *one = word("one"), *two = word("two"), *beta = word("beta"), *green = word("green");
        ok("a carousel wider than the box hiding its overflow shows its first slide and not the rest",
           one && one->x == 0 && !two && !word("three"));
        ok("and a translate moves the next one in, alone", beta && beta->x == 0 && !word("alpha") && !word("gamma"));
        ok("while a row in a box that shows its overflow is still kept to the room", green && green->x > 0 && green->x < 400);

        /* SoundCloud's: the row hides its own overflow, its parent shows it.
           Its slides wait past the window's edge; an old page's wider
           wrapper with its overflow hidden is still kept to the room. */
        lay("<style>.cont{width:800px}.car{display:flex;width:400%;overflow:hidden}.s{width:25%}"
            ".wrapper{width:960px;overflow:hidden}</style>"
            "<div class=cont><div class=car><div class=s>uno</div><div class=s>dos</div><div class=s>tres</div></div></div>"
            "<div class=wrapper><p>words in an old page's wrapper</p></div>", 800);
        const litem *uno = word("uno"), *dos = word("dos"), *wr = word("wrapper");
        ok("a row hiding its own overflow four windows wide puts its next slides past the window's edge",
           uno && uno->x == 0 && dos && dos->x >= 800);
        ok("and a wrapper a little wider than the window with its overflow hidden is kept to the room",
           wr && wr->x + wr->w <= 800);
        /* Debian's slideshow: floated slides in a box hiding its overflow, a
           column beside it. The floats are the box's own; reaching out, they
           pushed the next column's heading down past them. */
        lay("<style>.row{display:flex}.col{width:50%}.sw{overflow:hidden}.strip{width:600%}.sl{float:left;width:16%;height:120px}"
            "</style><div class=row><div class=col><div class=sw><div class=strip><div class=sl>s1</div><div class=sl>s2</div>"
            "<div class=sl>s3</div></div></div></div><div class=col><h2>first words of a heading that runs onto a second line here</h2></div></div>", 600);
        const litem *fw = word("first"), *ln = word("line");
        ok("and floats inside a box hiding its overflow do not push the column beside it down",
           fw && ln && fw->y < 60 && ln->y > fw->y && ln->y - fw->y < 80);
    }
    {
        /* The space between columns and the space between rows apart, as
           bbc.com's grid has them (column-gap 16, row-gap 24): they were one,
           so the row gap widened the columns' and they ran past the grid. */
        lay("<style>body{margin:0}p{margin:0;line-height:20px}"
            ".g{display:grid;grid-template-columns:repeat(4,1fr);column-gap:16px;row-gap:40px;width:448px}"
            ".h{display:grid;grid-template-columns:repeat(2,100px);gap:30px 10px}"
            ".f{display:flex;flex-wrap:wrap;width:250px;column-gap:10px;row-gap:50px}.f>p{width:100px}</style>"
            "<div class=g><p>g1</p><p>g2</p><p>g3</p><p>g4</p><p>g5</p></div>"
            "<div class=h><p>h1</p><p>h2</p><p>h3</p></div>"
            "<div class=f><p>f1</p><p>f2</p><p>f3</p></div>", 800);
        const litem *g1 = word("g1"), *g2 = word("g2"), *g5 = word("g5"), *h1 = word("h1"), *h2 = word("h2"),
                    *h3 = word("h3"), *f1 = word("f1"), *f2 = word("f2"), *f3 = word("f3");
        ok("column-gap, row-gap and gap's two values each go where they say",
           g1 && g2 && g5 && g2->x == 116 && g5->y - g1->y == 60
           && h1 && h2 && h3 && h2->x == 110 && h3->y - h1->y == 50
           && f1 && f2 && f3 && f2->x == 110 && f3->y - f1->y == 70);
    }
    {
        /* bbc.com's cards end in a row 32 high, centred: the time, a rule 1
           pixel wide and 16 high written `margin: auto 2px`, the section. The
           rule came out 16 wide over the section's first letters, and the row
           lined its items up at its top. Also a rule standing alone. */
        lay("<style>body{margin:0}.m{height:32px;align-items:center;display:flex;gap:8px}"
            ".s{background-color:#545658;margin:auto 2px;height:16px;width:1px}.t{font-size:12px;line-height:14px}"
            ".bar{background:#123456;width:3px;height:10px}</style>"
            "<div class=m><span class=t>ago</span><div class=s id=sep></div><span class=t>section</span></div>"
            "<div class=bar id=bar></div>", 800);
        const litem *ag = word("ago"), *se = word("section"), *sp = box_of(by_id("sep")), *bar = box_of(by_id("bar"));
        ok("a box the page makes 1 or 3 pixels wide is that wide",
           sp && sp->w == 1 && bar && bar->w == 3 && se && sp->x + sp->w < se->x);
        ok("and a row of one line 32 high centres its items, a rule with auto margins too",
           ag && ag->y >= 8 && ag->y <= 10 && se && se->y == ag->y && sp && sp->y == 8 && sp->h == 16);
        lay("<style>body{margin:0}.e{display:flex;height:60px;align-items:flex-end}"
            ".e div{width:10px;height:10px;background:#654321}.u{margin-bottom:auto}.v{margin:auto 0}"
            ".c{display:flex;min-height:100px;align-items:center}.c p{margin:0;line-height:20px}</style>"
            "<div class=e><div id=low></div><div class=u id=high></div><div class=v id=mid></div></div>"
            "<div class=c><p>middle</p></div>", 800);
        const litem *lo = box_of(by_id("low")), *hi = box_of(by_id("high")), *md = box_of(by_id("mid")),
                    *mi = word("middle");
        ok("the end of a row 60 high is its bottom, auto margins keep an item at the top or the middle, a least height centres",
           lo && lo->y == 50 && hi && hi->y == 0 && md && md->y == 25 && mi && mi->y >= 98 && mi->y <= 104);
    }
    {
        /* Grid items across their cells: every one was stretched across, so
           bbc.com's 32 pixel play icon in a grid was as wide as the grid. One
           with a width keeps it, at the start, the middle (justify-items) or
           the end (justify-self); place-items centres both ways in a grid
           whose one row is as tall as the grid; align-self puts one at the
           bottom of its row. */
        lay("<style>body{margin:0}p{margin:0;line-height:20px}.g{display:grid;width:300px}"
            ".i{width:32px;height:20px;background:#a00}.c{justify-items:center}.e{justify-self:end}"
            ".pc{place-items:center;height:60px}.r{display:grid;grid-template-columns:1fr 1fr;width:300px}"
            ".t{height:50px;background:#00a}.b{align-self:end}</style>"
            "<div class=g><div class=i id=ja></div></div><div class=\"g c\"><div class=i id=jb></div></div>"
            "<div class=g><div class=\"i e\" id=jc></div></div><div class=\"g pc\"><p>centred</p></div>"
            "<div class=r><div class=t></div><p class=b>low</p></div>", 800);
        const litem *ja = box_of(by_id("ja")), *jb = box_of(by_id("jb")), *jc = box_of(by_id("jc")),
                    *ce = word("centred"), *lw = word("low");
        ok("a grid item with a width keeps it, at its cell's start, middle or end",
           ja && ja->x == 0 && ja->w == 32 && jb && jb->x == 134 && jb->w == 32 && jc && jc->x == 268 && jc->w == 32);
        ok("and place-items: center centres a grid's one item both ways, align-self: end puts one at its row's foot",
           ce && ce->x == (300 - ce->w) / 2 && ce->y == 80 && lw && lw->y == 150);
        /* A percentage is of the cell, once: given its own width as the
           room, usa.gov's calc(100% - 50px) was taken off twice. */
        lay("<style>body{margin:0}.g{display:grid;width:300px}.p{width:calc(100% - 50px);justify-self:end;height:10px;"
            "background:#a00}.q{width:calc(100% - 50px);margin:auto;height:10px;background:#0a0}</style>"
            "<div class=g><div class=p id=pe></div><div class=q id=pq></div></div>", 800);
        const litem *pe = box_of(by_id("pe")), *pq = box_of(by_id("pq"));
        ok("and a grid item's percentage width is of its cell, at the end or centred by its margins",
           pe && pe->w == 250 && pe->x == 50 && pq && pq->w == 250 && pq->x == 25);
        /* A width that is not a length, as dev.to's sidebar has it through a
           custom property (2fr): auto, so the item fills its column. Read as
           2 pixels, the sidebar was a sliver. */
        lay("<style>body{margin:0}.g{display:grid;grid-template-columns:240px 1fr;width:600px;--w:2fr}"
            ".s{width:100%;width:var(--w);height:10px;background:#a00}</style>"
            "<div class=g><div class=s id=sw></div><div>main</div></div>", 800);
        const litem *sw = box_of(by_id("sw"));
        ok("and a width that is not a length (2fr) is auto, the item filling its column", sw && sw->w == 240);
        /* grid-template-rows: unset puts the rows back to none, as bbc.com
           does under a media query: kept as a list, it was one row 0 pixels
           high, every story card was 0 high and the next row lay over it. */
        lay("<style>body{margin:0}p{margin:0;line-height:20px}.g{display:grid;grid-template-rows:10px 10px;width:300px}"
            ".g{grid-template-rows:unset}.h{display:grid;grid-template-columns:50px 50px}.h{grid-template-columns:initial}"
            "</style><div class=g><p>one</p><p>two</p></div><div class=h><p>three</p><p>four</p></div><p>after</p>", 800);
        const litem *u1 = word("one"), *u2 = word("two"), *u3 = word("three"), *u4 = word("four"), *ua = word("after");
        ok("and grid-template-rows: unset or columns: initial are none, the rows as tall as what is in them",
           u1 && u2 && u2->y - u1->y == 20 && u3 && u4 && u4->y - u3->y == 20 && u4->x == u3->x && ua && ua->y == 80);
    }
    {
        /* Class names with escapes in them, as Tailwind writes every one with
           a colon, a bracket or a slash: read up to the backslash, .lg\:hidden
           was the class lg, so what a page shows only at other widths showed
           at every width (Hugging Face's wide picture over its headline). */
        lay("<style>body{margin:0}p{margin:0;line-height:20px}.max-xl\\:hidden{display:none}"
            "@media (width>=40rem){.md\\:ml-\\[50px\\]{margin-left:50px}}.w-1\\/2{margin-left:120px}"
            ".\\31 0x{margin-left:200px}</style>"
            "<p class=max-xl:hidden>gone</p><p class='md:ml-[50px]'>moved</p><p class=w-1/2>half</p>"
            "<p class=10x>digit</p>", 800);
        const litem *eg = word("gone"), *em = word("moved"), *eh = word("half"), *ed = word("digit");
        ok("a class named with escapes is matched by its name: .max-xl\\:hidden, .md\\:ml-\\[50px\\], .w-1\\/2, .\\31 0x",
           !eg && em && em->x == 50 && eh && eh->x == 120 && ed && ed->x == 200);
    }
    {
        /* Craigslist's header: a row of a place that grows and buttons that
           do not, each button a column of an icon over a word, centred, and
           at least 44 wide. Measured, a centred item reached halfway across
           the room it was measured in, so each button measured a third of
           the window and the place was squeezed to a sliver; and laid out at
           a word's width, the buttons sat 22 apart, each over the next. */
        lay("<style>body{margin:0}p{margin:0;line-height:20px}.h{display:flex;width:800px}.g{flex-grow:1;background:#eee}"
            ".m{display:flex;flex-shrink:0}.b{display:flex;min-width:44px;justify-content:center}"
            ".c{display:flex;flex-direction:column;align-items:center}.w{font-size:10px;line-height:12px}</style>"
            "<div class=h><div class=g id=place><p>somewhere</p></div><div class=m>"
            "<a class=b id=b1><span class=c><span class=w>post</span></span></a>"
            "<a class=b id=b2><span class=c><span class=w>acct</span></span></a></div></div>", 800);
        const litem *pl = box_of(by_id("place")), *bp = word("post"), *ba = word("acct");
        ok("a row's centred columns measure as wide as what is in them, and keep their min-width apart",
           pl && pl->w >= 700 && bp && ba && ba->x - bp->x == 44);
    }
    {
        /* A box that hides its overflow and is 0 high is hidden on purpose
           unless its padding makes it something: Debian's banner heading is
           0 high with 130 pixels of padding for its picture, and was never
           drawn. One a pixel square with no padding is still hidden. */
        lay("<style>body{margin:0}p{margin:0}.s{height:0;padding-top:50px;overflow:hidden;background:#123456}"
            ".sr{position:absolute;width:1px;height:1px;padding:0;overflow:hidden}</style>"
            "<div class=s id=splash>heading</div><p class=sr>unseen</p><p>after</p>", 800);
        const litem *sb = box_of(by_id("splash")), *us = word("unseen"), *af2 = word("after");
        ok("a box 0 high that hides its overflow is drawn when its padding gives it height, a pixel square is not",
           sb && sb->h == 50 && !us && af2 && af2->y == 50);
    }
    {
        /* Tailwind 4's palette is oklch(): unread, no colour from it reached
           the page. Its red-500, blue-600 and gray-900 as the specification
           works them out, and oklab's pure red, to within a step; and an
           opacity written both ways, #rrggbbaa and then color-mix() under
           @supports, keeps the first, since color-mix() is not read. */
        lay("<style>:root{--red:oklch(63.7% 0.237 25.331)}.r{color:var(--red)}.b{color:oklch(54.6% .245 262.881deg)}"
            ".g{color:oklch(0.21 0.034 264.665)}.o{color:oklab(0.628 0.2249 0.1258)}.m{--c:#00800080;color:var(--c)}"
            "@supports (color:color-mix(in lab, red, red)){.m{--c:color-mix(in oklab, #008000 50%, transparent)}}"
            ".q{color:#123456}@supports (background-image:linear-gradient(in lab, red, red)){.q{color:#654321}}"
            "</style><p class=r>tred</p><p class=b>tblue</p><p class=g>tgray</p><p class=o>tok</p><p class=m>tmix</p>"
            "<p class=q>tgrad</p>", 800);
        const litem *k1 = word("tred"), *k2 = word("tblue"), *k3 = word("tgray"), *k4 = word("tok"), *k5 = word("tmix"),
                    *k6 = word("tgrad");
        #define NEAR(c, want) ((c) >= 0 && (((int)((c) >> 16) & 255) - (((want) >> 16) & 255)) * (((int)((c) >> 16) & 255) - (((want) >> 16) & 255)) <= 1 \
            && (((int)((c) >> 8) & 255) - (((want) >> 8) & 255)) * (((int)((c) >> 8) & 255) - (((want) >> 8) & 255)) <= 1 \
            && (((int)(c) & 255) - ((want) & 255)) * (((int)(c) & 255) - ((want) & 255)) <= 1)
        ok("oklch() and oklab() colours come out as the specification has them, color-mix() and gradients in lab leave their fallbacks",
           k1 && NEAR(k1->color, 0xfb2c36) && k2 && NEAR(k2->color, 0x155dfc) && k3 && NEAR(k3->color, 0x101828)
           && k4 && NEAR(k4->color, 0xff0000) && k5 && NEAR(k5->color, 0x7fbf7f) && k6 && k6->color == 0x123456);
        /* color() in srgb, srgb-linear and display-p3, on a screen that is
           neither high dynamic range nor wide gamut: substack.com swaps its
           greys for display-p3 ones on such a screen, and with the swap taken
           and color() unread its buttons had no colour. */
        lay("<style>.a{color:color(srgb 0.2 0.4 0.6)}.b{color:color(srgb-linear 50% 50% 50%)}"
            ".c{color:color(display-p3 0.2 0.4 0.6)}.d{color:#123456}@media (dynamic-range:high){.d{color:#ff0000}}"
            ".e{color:#123456}@media (color-gamut:p3){.e{color:#ff0000}}"
            ".f{color:#123456}@media (color-gamut:srgb){.f{color:#00ff00}}</style>"
            "<p class=a>ka</p><p class=b>kb</p><p class=c>kc</p><p class=d>kd</p><p class=e>ke</p><p class=f>kf</p>", 800);
        const litem *ka = word("ka"), *kb = word("kb"), *kc = word("kc"), *kd = word("kd"), *ke = word("ke"),
                    *kf = word("kf");
        ok("color() in srgb, srgb-linear and display-p3 comes out as the specification has it",
           ka && NEAR(ka->color, 0x336699) && kb && NEAR(kb->color, 0xbcbcbc) && kc && NEAR(kc->color, 0x1b689d));
        ok("and the screen is not high dynamic range or wide gamut, but is sRGB",
           kd && kd->color == 0x123456 && ke && ke->color == 0x123456 && kf && kf->color == 0x00FF00);
        #undef NEAR
    }
    {
        /* padding-inline and -block, margin-inline and -block, and a side of
           one: Tailwind 4 writes px-4 as padding-inline: calc(var(--spacing)
           * 4), and none of them was read, so its buttons had no padding. */
        lay("<style>body{margin:0}p{margin:0;line-height:20px}:root{--spacing:4px}"
            ".a{padding-inline:20px;padding-block:5px}.b{margin-inline:30px}.c{padding-inline:calc(var(--spacing) * 4)}"
            ".d{padding-inline-start:12px}.e{margin-block:10px 0}.f{--tb:6px 30px;padding-block:var(--tb)}</style>"
            "<p class=a>wa</p><p class=b>wb</p><p class=c>wc</p><p class=d>wd</p><p class=e>we</p><p>wf</p>"
            "<p class=f>wg</p><p>wh</p>", 800);
        const litem *la = word("wa"), *lb = word("wb"), *lc = word("wc"), *ld = word("wd"), *le = word("we"),
                    *lf = word("wf"), *lg = word("wg"), *lh = word("wh");
        ok("padding-inline and -block, margin-inline and -block, a side alone and calc(var()) in them all take",
           la && la->x == 20 && la->y == 5 && lb && lb->x == 30 && lc && lc->x == 16 && ld && ld->x == 12
           && le && lf && lf->y - le->y == 20 && le->y == 30 + 20 + 20 + 20 + 10
           && lg && lh && lg->y - lf->y == 26 && lh->y - lg->y == 50);
    }
    {
        /* inline-size, block-size and their limits are width and height:
           w3.org's full-width hero is inline-size: 100vw between margins of
           -50vw and moved back by inset-inline: 50%, and without the size it
           was as wide as both margins. */
        lay("<style>body{margin:0}p{margin:0}.f{inline-size:300px;block-size:30px;background:#111111}"
            ".m{max-inline-size:100px;min-block-size:40px;background:#222222}.w{width:400px;margin:0 auto}"
            ".k{inline-size:100vw;inset-inline:50%;margin-inline:-50vw;position:relative;block-size:10px;background:#333333}"
            "</style><p class=f id=lf>sa</p><p class=m id=lm>sb</p><div class=w><div class=k id=lk></div></div>", 800);
        const litem *lf = box_of(by_id("lf")), *lm = box_of(by_id("lm")), *lk = box_of(by_id("lk"));
        ok("inline-size, block-size and their limits size a box, and a full-width box between -50vw margins fills the window",
           lf && lf->w == 300 && lf->h == 30 && lm && lm->w == 100 && lm->h == 40 && lk && lk->x == 0 && lk->w == 800);
    }
    {
        /* color: unset and inherit undo the browser's link colour: bbc.com's
           links say unset, and its logo, painted currentcolor, was blue. */
        lay("<style>a{color:#0000ff}div{color:#123456}.u{color:unset}.i{color:inherit}.n{color:initial}p{color:#00ff00}</style>"
            "<div><a href=x class=u>cu</a> <a href=x class=i>ci</a> <a href=x>ca</a><p class=n>cn</p></div><a href=/>cs</a>", 800);
        const litem *cu = word("cu"), *ci = word("ci"), *ca = word("ca"), *cn = word("cn"), *cs = word("cs");
        ok("color: unset and inherit take the parent's colour over a link's, initial the first colour",
           cu && cu->color == 0x123456 && ci && ci->color == 0x123456 && ca && ca->color != 0x123456
           && cn && cn->color == 0x1A1A1A);
        /* And an unquoted value keeps its slash: <a href=/> was closed on the
           spot, and its words were not a link. */
        ok("<a href=/> is a link to / holding what follows", cs && cs->color == 0x0000FF && cs->link >= 0);
    }
    {
        /* background: none and background-color: initial are transparent:
           every reset button kept the browser's grey, and Spotify's close
           button a white circle under its grey cross. A gradient that is not
           drawn keeps the colour before it, its only stand-in. */
        lay("<style>.g{background:#ff0000;height:10px}.n{background:none}.i{background-color:initial}"
            ".u{background:url(x.png) no-repeat}.r{background:radial-gradient(#fff,#000)}"
            ".v{--b:none;background:var(--b)}.w{--b:url(x.png) no-repeat 0 0 #00ff00;background:var(--b)}</style>"
            "<div class='g n' id=bn></div><div class='g i' id=bi></div><div class='g u' id=bu></div>"
            "<div class='g r' id=br></div><div class=g id=bg></div><div class='g v' id=bv></div>"
            "<div class='g w' id=bw></div>", 800);
        const litem *bn = box_of(by_id("bn")), *bi = box_of(by_id("bi")), *bu = box_of(by_id("bu")),
                    *br = box_of(by_id("br")), *bg = box_of(by_id("bg")), *bv = box_of(by_id("bv")),
                    *bw = box_of(by_id("bw"));
        ok("background: none, a picture alone and background-color: initial leave no colour, in a variable too",
           !(bn && bn->has_bg) && !(bi && bi->has_bg) && !(bu && bu->has_bg) && !(bv && bv->has_bg)
           && bg && bg->has_bg && bg->bg == 0xFF0000 && bw && bw->has_bg && bw->bg == 0x00FF00);
        ok("and a gradient that is not drawn keeps the colour before it", br && br->has_bg && br->bg == 0xFF0000);
        /* What is drawn inside is made on what is behind the box, not on the
           colour taken away: Spotify's cross was drawn on a white square. */
        lay("<style>.d{background:#123456}.n{background:none}.t{background-color:initial}</style>"
            "<div class=d><button class=n><svg id=sn width=10 height=10></svg></button>"
            "<button class=t><svg id=st width=10 height=10></svg></button></div>", 800);
        const litem *sn = image_of(by_id("sn")), *sv = image_of(by_id("st"));
        ok("and a drawing in a button whose background is taken away is made on what is behind it",
           sn && sn->bg == 0x123456 && sv && sv->bg == 0x123456);
    }
    {
        /* GitHub's repository page sends twenty-five thousand rules, and at
           sixteen thousand the rest were lost: its visually hidden headings
           and its closed menu's rules among them. The last of 25000 rules
           still takes. */
        static char big[25000 * 24 + 256];
        int w = 0;
        const char *head = "<style>body{margin:0}p{margin:0}";
        for (int i = 0; head[i]; i++) big[w++] = head[i];
        for (int r = 0; r < 25000; r++) {
            big[w++] = '.'; big[w++] = 'r';
            char num[8];
            int nn = 0, v = r;
            do { num[nn++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (nn) big[w++] = num[--nn];
            const char *body = "{margin-left:1px}";
            for (int i = 0; body[i]; i++) big[w++] = body[i];
        }
        const char *tail = ".last{margin-left:77px}</style><p class=last>tail</p>";
        for (int i = 0; tail[i]; i++) big[w++] = tail[i];
        big[w] = 0;
        lay(big, 800);
        const litem *tl = word("tail");
        okn("the last of twenty-five thousand rules is read", tl && tl->x == 77, tl ? tl->x : -1);
    }
    {
        /* An absolutely positioned box with no width is as wide as what is
           in it: a label at a picture's corner was a bar across the picture,
           one put at the right by right: 0 started at the left, and one
           centred by left: 50% and translate(-50%) was half off the box. */
        lay("<style>body{margin:0}p{margin:0;line-height:20px}.w{position:relative;width:400px;height:100px}"
            ".tag{position:absolute;left:0;bottom:0;background:#000;color:#fff;padding:4px}"
            ".cor{position:absolute;right:0;top:0;background:#0a0}"
            ".mid{position:absolute;left:50%;top:40px;transform:translateX(-50%);background:#aa0}</style>"
            "<div class=w><p class=tag id=tg>tag</p><p class=cor id=cr>corner</p><p class=mid id=md2>middle</p></div>", 800);
        const litem *tg = box_of(by_id("tg")), *tw = word("tag"), *cr = box_of(by_id("cr")),
                    *m2 = box_of(by_id("md2"));
        ok("an absolutely positioned box with no width is as wide as its words, at the corner or the side it is put",
           tg && tw && tg->w == tw->w + 8 && tg->x == 0 && tg->y == 100 - tg->h
           && cr && cr->x + cr->w == 400 && cr->w < 100 && m2 && m2->x + m2->w / 2 >= 199 && m2->x + m2->w / 2 <= 201);
        /* And it is no part of how wide what holds it is: a box sized by its
           contents with a button put at its right was as wide as its row
           (dev.to's search field), squeezing what was beside it. */
        lay("<style>body{margin:0}.row{display:flex;width:600px}.f{position:relative;background:#eee}"
            ".b{position:absolute;right:0;top:0;width:40px;height:10px;background:#000}.n{width:200px;height:10px;"
            "background:#a00}</style><div class=row><div class=f id=ff>field<span class=b></span></div>"
            "<div class=n id=nn></div></div>", 800);
        const litem *ff = box_of(by_id("ff")), *nn = box_of(by_id("nn")), *fw = word("field");
        ok("and a positioned box adds nothing to how wide its holder measures", ff && fw && ff->w < fw->w + 4
           && nn && nn->w == 200);
    }
    {
        /* A box whose height a padding gives it, and what fills it placed
           absolutely with height: 100% or inset: 0, as Forbes draws its cover
           and most sites their thumbnails: the filling had no height, and
           neither the background nor the picture was drawn. */
        static const char *aspect =
            "<style>body{margin:0}.r{position:relative;display:block;width:400px}"
            ".r:before{content:'';display:block;padding-bottom:50%}"
            ".w{position:absolute;top:0;left:0;width:100%;height:100%;overflow:hidden}"
            ".i{position:absolute;inset:0;background:#e2e2e2}"
            ".r img{position:absolute;top:0;left:0;width:100%;height:100%}</style>"
            "<a class=r><div class=w><div class=i id=ii></div></div></a>"
            "<div class=r><img id=ip src=x></div><p>after</p>";
        lay(aspect, 800);
        picture_at("ip", 640, 480);
        lay(aspect, 800);
        nfake = 0;
        const litem *ii = box_of(by_id("ii")), *ip = image_of(by_id("ip")), *af = word("after");
        ok("what fills a box by height: 100% or inset: 0 takes the height the box comes to",
           ii && ii->w == 400 && ii->h == 200 && ii->y == 0
           && ip && ip->w == 400 && ip->h == 200 && ip->y == 200 && af && af->y >= 400);
    }
    {
        /* :is() and :where() holding lists, as Forbes places its columns: the
           rule was dropped. Each choice counts, and :where weighs nothing, so
           a plainer rule before it still wins. */
        lay("<style>body{margin:0}p{margin:0}:is(.l,.r) :is(.big,.cols){margin-left:200px}"
            ".z p{margin-left:50px}:where(.q,.z) p{margin-left:300px}</style>"
            "<div class=r><p class=big>wide</p><p class=cols>column</p></div><div class=z><p>weightless</p></div>", 800);
        const litem *wi = word("wide"), *cl = word("column"), *wl = word("weightless");
        ok(":is() and :where() with lists match each choice, :where weighing nothing",
           wi && wi->x >= 200 && cl && cl->x >= 200 && wl && wl->x >= 50 && wl->x < 100);
    }
    {
        /* An absolutely positioned box with only a bottom goes that far from
           the bottom of its positioned box, once that box's height is known,
           whether it was given one or grew round what is in it; it stayed
           where the flow put it. */
        lay("<style>body{margin:0}p{margin:0;line-height:20px}.w{position:relative;height:300px}"
            ".b{position:absolute;bottom:20px;left:0}.v{position:relative;padding:10px}"
            ".c{position:absolute;bottom:0;right:0}</style>"
            "<div class=w><p class=b>floor</p></div>"
            "<div class=v><p>one</p><p>two</p><p>three</p><p class=c>corner</p></div>", 800);
        const litem *fl = word("floor"), *th = word("three"), *co = word("corner");
        ok("a positioned box with only a bottom goes that far from its positioned box's bottom",
           fl && fl->y >= 255 && fl->y <= 262 && th && co && co->y >= th->y + 8 && co->y <= th->y + 12);

        /* A positioned picture is placed by its offsets and as big as it
           says, as Apple's hero is (left: 50%, a translate, bottom: 0, a
           height): it was put on the line it would have been on, at its
           left, and squeezed into it. inset-inline-start is left. */
        static const char *hero =
            "<style>body{margin:0}.w{position:relative;height:300px}"
            ".w img{position:absolute;left:50%;bottom:0;height:100px;transform:translate(-50%)}"
            ".x{position:relative;height:300px}.x img{position:absolute;inset-inline-start:50%;top:0;height:300px;"
            "transform:translate(-50%)}</style>"
            "<div class=w><img id=hp src=x></div><div class=x><img id=hq src=x></div>";
        lay(hero, 800);
        picture_at("hp", 1600, 400);
        picture_at("hq", 1600, 400);
        lay(hero, 800);
        nfake = 0;
        const litem *hp = image_of(by_id("hp")), *hq = image_of(by_id("hq"));
        ok("a positioned picture is placed by its offsets and is as big as it says",
           hp && hp->w == 400 && hp->h == 100 && hp->x == 200 && hp->y == 200
           && hq && hq->w == 1200 && hq->x == -200 && hq->y == 300);
    }
    {
        /* A bracket and a brace inside quoted strings, as Tumblr's sheet has
           content:":(" half way down: the declaration ran on through every
           rule after it. */
        lay("<style>.s:after{content:\":(\";width:1em}.t{margin-left:200px}"
            ".u:before{content:'}'}.v{margin-left:300px}</style>"
            "<p class=t>lefted</p><p class=v>further</p>", 800);
        const litem *lf = word("lefted"), *fu = word("further");
        ok("a bracket or a brace inside a quoted string is not CSS", lf && lf->x >= 200 && fu && fu->x >= 300);
    }
    {
        /* hidden="until-found" under a sheet that makes the box a block, as
           the US government's banner is: the box stays, its contents do not,
           and content-visibility: hidden does the same from a sheet. A plain
           hidden box the sheet makes a block is drawn, as in a browser. */
        lay("<style>.b{display:block;padding:4px 0 30px}.cv{content-visibility:hidden;height:40px}</style>"
            "<div class=b hidden=until-found><p>secret words</p></div><p>after</p>"
            "<div class=cv><p>concealed</p></div><p>below</p><div class=b hidden><p>shown</p></div>", 800);
        const litem *af = word("after"), *bl = word("below");
        ok("hidden=until-found and content-visibility: hidden keep the box and hide what is in it",
           !word("secret") && !word("concealed") && af && af->y >= 34 && bl && bl->y - af->y >= 40 && word("shown"));
    }
    {
        /* A row line counted back from the last, as NPR's card pictures span
           the card (grid-row: 1 / -1): it took the first row alone, and the
           headline beside the picture went below it. */
        lay("<style>.c{display:grid;grid-template-columns:200px 200px;grid-template-rows:auto auto 1fr;width:400px}"
            ".c>p{grid-column:1;margin:0}.c>div{grid-column:2;grid-row:1 / -1;height:300px}</style>"
            "<div class=c><p>eyebrow</p><p>headline</p><div></div></div><p>after</p>", 800);
        const litem *eb = word("eyebrow"), *hl = word("headline"), *af = word("after");
        ok("a row line counted back from the last spans the rows written out",
           eb && hl && af && hl->y > eb->y && hl->y - eb->y < 100 && af->y >= 300);
    }
    {
        /* Two :not()s on one compound, as NPR places its cards' parts: the
           selector was dropped, so the parts took a column each. */
        lay("<style>.r{display:grid;grid-template-columns:repeat(4,100px);width:400px}"
            ".r>:not(.m):not(.l){grid-column:1/span 3}</style>"
            "<div class=r><p>wide words here</p><p class=m>media</p><p class=l>label</p></div>", 800);
        const litem *wd = word("wide"), *hr = word("here"), *md = word("media"), *lb = word("label");
        ok("a compound with two :not()s matches what is neither",
           wd && hr && md && lb && hr->y == wd->y && md->y == wd->y && md->x >= 300 && lb->y > wd->y && lb->x < 100);
    }
    {
        /* A grid item flowed into place with its span on the end alone, as
           National Geographic writes it: it took one column of twelve. */
        lay("<style>.g{display:grid;grid-template-columns:repeat(12,1fr);width:600px}.g>*{grid-column-end:span 12}"
            ".h{display:grid;grid-template-columns:repeat(12,1fr);width:600px}.h>*{grid-column-start:span 6}</style>"
            "<div class=g><p>whole width words that should run across the row</p><p>next</p></div>"
            "<div class=h><p>half</p><p>other</p></div>", 800);
        const litem *wh = word("row"), *nx = word("next"), *ha = word("half"), *ot = word("other");
        ok("a grid item flowed with its span on one end alone spans the columns it asks for",
           wh && nx && wh->y == word("whole")->y && nx->y > wh->y && ha && ot && ot->y == ha->y && ot->x >= 290 && ot->x < 320);
    }
    {
        /* flex-flow, a direction and a wrap together, was not read at all:
           `flex-flow: column` left a row. */
        lay("<style>.c{display:flex;flex-flow:column}.w{display:flex;flex-flow:row wrap;width:300px}.i{width:200px}"
            ".v{--d:column;display:flex;flex-flow:var(--d) nowrap}</style>"
            "<div class=c><p>top</p><p>bottom</p></div><div class=w><div class=i>left</div><div class=i>right</div></div>"
            "<div class=v><p>upper</p><p>lower</p></div>", 800);
        const litem *t = word("top"), *bo = word("bottom"), *l = word("left"), *r = word("right");
        const litem *up = word("upper"), *lo = word("lower");
        ok("flex-flow sets a column, a wrapping row, and a column through var()",
           t && bo && bo->y > t->y && bo->x == t->x && l && r && r->y > l->y && up && lo && lo->y > up->y && lo->x == up->x);
    }
    {
        /* A sheet with thousands of :not()s, as CNN's has: a rule after the
           first two thousand was dropped. */
        static char many[3000 * 32 + 256];
        int n = 0;
        for (const char *h = "<style>"; *h; h++) many[n++] = *h;
        for (int i = 0; i < 3000; i++) {
            char num[8];
            int k = 0, v = i;
            do { num[k++] = (char)('0' + v % 10); v /= 10; } while (v);
            for (const char *h = ".z"; *h; h++) many[n++] = *h;
            while (k) many[n++] = num[--k];
            for (const char *h = ":not(.q){color:red}"; *h; h++) many[n++] = *h;
        }
        for (const char *h = ".late:not(.q):after{content:\"MARK\"}</style><p class=late>text</p>"; *h; h++) many[n++] = *h;
        many[n] = 0;
        lay(many, 600);
        ok("a rule after three thousand :not()s in the sheet still applies", word("MARK") != 0);
    }

    /* --- a transform's share is of the box ------------------------------------------
     *
     * translate(100%) moves a box by its own width, though it draws nothing
     * of its own. Taken from what was in it, DuckDuckGo's closed menu moved
     * by a word's width and stood at the window's edge. */
    {
        lay("<style>.m{position:fixed;top:0;right:0;width:200px;transform:translate(100%)}"
            ".k { width: 50px; background: #000 }</style>"
            "<div class=m><div class=k id=mk>menu</div></div><p>page</p>", 600);
        const litem *mk = box_of(by_id("mk"));
        okn("a closed menu moved by its own width is out of the window",
            mk && mk->x >= 600, mk ? mk->x : -1);
    }

    /* --- a canvas ---------------------------------------------------------------------
     *
     * As big as its bitmap, which its width and height attributes make (300
     * by 150 without them), its shape kept when the page's rules give it one
     * side; and what is written inside it is for a browser that cannot draw
     * one. It had been an unknown element, its fallback words laid out and
     * no box for its picture. */
    {
        lay("<canvas width=50 height=20>fallback words</canvas>", 600);
        /* What is drawn: the edges of the boxes round it draw nothing. */
        int drawn = 0;
        const litem *pic = 0;
        for (int i = 0; i < page.nitems; i++)
            if (page.items[i].kind != LK_EDGE) { drawn++; pic = &page.items[i]; }
        int only = drawn == 1 && pic->kind == LK_IMAGE && pic->w == 50 && pic->h == 20;
        okn("a canvas is a box its attributes' size, and its fallback is not laid out", only, drawn);
        /* A box that draws nothing has its edges laid out all the same, and
           they go where its contents are moved to. */
        lay("<style>body { margin: 0 } .w { width: 300px } .f { display: flex; justify-content: center; width: 400px }"
            " .c { width: 100px }</style><div id=e1 class=w>x</div><div class=f><div id=e2 class=c>y</div></div>", 800);
        const litem *e1 = 0, *e2 = 0;
        for (int i = 0; i < page.nitems; i++) {
            if (page.items[i].kind != LK_EDGE) continue;
            if (page.items[i].node == by_id("e1")) e1 = &page.items[i];
            if (page.items[i].node == by_id("e2")) e2 = &page.items[i];
        }
        const litem *wy = word("y");
        okn("a box that draws nothing has its edges: a div 300 wide holding a word is 300 wide", e1 && e1->w == 300, e1 ? e1->w : -1);
        okn("and a centred flex item's edges are where its words went", e2 && e2->x == 150 && e2->w == 100 && wy && wy->x >= 150,
            e2 ? e2->x : -1);
        lay("<style>.w{width:200px}</style><canvas id=c1 width=100 height=50 class=w></canvas><canvas id=c2></canvas>", 600);
        int c1 = by_id("c1"), c2 = by_id("c2");
        const litem *i1 = 0, *i2 = 0;
        for (int i = 0; i < page.nitems; i++) {
            if (page.items[i].kind != LK_IMAGE) continue;
            if (page.items[i].node == c1) i1 = &page.items[i];
            if (page.items[i].node == c2) i2 = &page.items[i];
        }
        okn("given a width, its height keeps its shape", i1 && i1->w == 200 && i1->h == 100, i1 ? i1->h : -1);
        okn("and with no attributes it is 300 by 150", i2 && i2->w == 300 && i2->h == 150, i2 ? i2->w : -1);
    }

    /* --- a player from another site ----------------------------------------------------
     *
     * A frame whose address names an embed or a player is a box its width and
     * height say, 300 by 150 without them, the whole of it a link to the
     * frame's address; any other frame, and one sized nought, is not laid out.
     * Every frame had been left out, and an article had a gap where its film
     * was. */
    {
        lay("<p>a</p><iframe id=f1 width=560 height=315 src=\"https://www.youtube.com/embed/abcdefghijk\"></iframe>"
            "<iframe id=f2 src=\"https://ads.example.net/pagead/x\"></iframe>"
            "<iframe id=f3 width=0 height=0 src=\"https://player.vimeo.com/video/1\"></iframe>"
            "<iframe id=f4 src=\"/videos/embed/xyz\">words for a browser with no frames</iframe>", 800);
        const litem *f1 = image_of(by_id("f1")), *f2 = image_of(by_id("f2"));
        const litem *f3 = image_of(by_id("f3")), *f4 = image_of(by_id("f4"));
        okn("a player from another site is a box its attributes' size", f1 && f1->w == 560 && f1->h == 315, f1 ? f1->w : -1);
        ok("and the whole box opens the frame's address",
           f1 && f1->link >= 0 && w_same(page.text + page.links[f1->link].href, "https://www.youtube.com/embed/abcdefghijk"));
        ok("a frame that is not a player, or is sized nought, is not laid out", !f2 && !f3);
        okn("a player on the page's own site is 300 by 150 without a size", f4 && f4->w == 300 && f4->h == 150, f4 ? f4->w : -1);
        ok("and what is written inside a frame is not laid out", !word("words"));
        lay("<style>.w { width: 400px; height: 225px }</style><iframe id=f5 class=w src=\"https://player.twitch.tv/?channel=x\"></iframe>", 800);
        const litem *f5 = image_of(by_id("f5"));
        okn("the page's rules size it", f5 && f5->w == 400 && f5->h == 225, f5 ? f5->w : -1);
    }

    /* --- ::before and ::after --------------------------------------------------------
     *
     * Every rule naming one was thrown away, and a page's marks, labels and
     * clearing blocks were not there. */
    {
        lay("<style>.q::before{content:'[' attr(data-x) ']'}.q::after{content:\"\\21\"}"
            ".n::before{content:none}.v{background:#000}"
            ".cf::after { content: \"end\"; display: block; clear: both }"
            ".f{float:left;width:50px;height:50px;background:#000}</style>"
            "<p class=q data-x=hi>words</p><p class=n id=np>plain</p>"
            "<div class='cf v' id=box><div class=f></div></div><div class=v id=next>next</div>", 600);
        const litem *a = word("[hi]"), *b = word("words"), *c = word("!");
        okn("::before and ::after put their content round the element's, from strings, escapes and attr()",
            a && b && c && a->x < b->x && b->x < c->x && a->y == b->y, a ? a->x : -1);
        int np = by_id("np"), extra = 0;
        for (int i = 0; i < page.nitems; i++)
            if (page.items[i].kind == LK_TEXT && page.items[i].node != np && page.items[i].y == (word("plain") ? word("plain")->y : -1))
                extra++;
        okn("and content: none puts nothing", word("plain") && !extra, extra);
        /* (A box here holds its floats with or without a clearfix, so the
           ::after is a block of words that clears the float: only its own
           rules put it below.) */
        const litem *bx = box_of(by_id("box")), *end = word("end");
        okn("a ::after takes its own rules: a block that clears the float above it", bx && end && end->y >= bx->y + 50,
            end && bx ? end->y - bx->y : -1);
    }

    puts(failed ? "LAYOUTTEST_FAIL\n" : "LAYOUTTEST_PASS\n");
    return failed;
}
