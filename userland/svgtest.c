/* The drawing renderer.
 *
 * Unlike the other two picture tests there is no file from elsewhere to
 * compare against: an SVG is markup, so the input is written here in full
 * and what is checked is where the ink landed. That is the right check
 * anyway — the question is not "did it parse" but "is the shape in the right
 * place, the right colour, and the right way up".
 *
 * Coordinates go down the page, which is the mistake worth catching: a
 * drawing rendered upside down still looks like a drawing.
 */
#include "zelr.h"
#include "alloc.h"
#include "svg.h"

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

static picture p;

static int draw(const char *xml, int w, int h) {
    int n = 0;
    while (xml[n]) n++;
    return svg_render(xml, n, w, h, &p, 0xFFFFFF);
}

static int at(int x, int y) {
    if (!p.rgb || x < 0 || y < 0 || x >= p.w || y >= p.h) return -1;
    const u8 *q = p.rgb + (y * p.w + x) * 3;
    return (q[0] << 16) | (q[1] << 8) | q[2];
}

static int isred(int c)   { return c >= 0 && ((c >> 16) & 0xFF) > 180
                                   && ((c >> 8) & 0xFF) < 70 && (c & 0xFF) < 70; }
static int iswhite(int c) { return c == 0xFFFFFF; }

int main(void) {
    puts("drawings\n");

    /* --- a rectangle, where it was put ------------------------------------- */
    {
        int rc = draw("<svg width='40' height='40'>"
                      "<rect x='10' y='5' width='20' height='10' fill='red'/>"
                      "</svg>", 40, 40);
        okn("a drawing renders", rc == SVG_OK, rc);

        if (rc == SVG_OK) {
            ok("and a rectangle is where it was put", isred(at(20, 10)));
            ok("and not above it", iswhite(at(20, 2)));
            ok("and not below it", iswhite(at(20, 30)));
            ok("and not beside it", iswhite(at(3, 10)));

            /* Down the page, not up it. A drawing upside down still looks
               like a drawing, which is why this is asked directly. */
            ok("and y counts downward, not upward", iswhite(at(20, 35)));
            picture_free(&p);
        }
    }

    /* --- a viewBox, which is the whole point of the format ------------------
     *
     * The shapes are written in one set of coordinates and drawn at
     * whatever size is asked for. Ten units becoming a hundred pixels is
     * what lets a logo be a logo at any size. */
    {
        int rc = draw("<svg viewBox='0 0 10 10' width='10' height='10'>"
                      "<rect x='0' y='0' width='5' height='10' fill='red'/>"
                      "</svg>", 100, 100);
        okn("a drawing scales to the size it is asked for", rc == SVG_OK, rc);

        if (rc == SVG_OK) {
            okn("and comes out at that size", p.w == 100 && p.h == 100, p.w);
            ok("the half that was filled is filled", isred(at(25, 50)));
            ok("and the half that was not is not", iswhite(at(75, 50)));
            picture_free(&p);
        }
    }

    /* --- a path ------------------------------------------------------------- */
    {
        int rc = draw("<svg width='40' height='40'>"
                      "<path d='M 5 5 L 35 5 L 35 35 Z' fill='red'/>"
                      "</svg>", 40, 40);
        okn("a path renders", rc == SVG_OK, rc);

        if (rc == SVG_OK) {
            /* The triangle has its right angle at the bottom right, so
               inside is below the diagonal and near the right. */
            ok("the inside of a triangle is filled", isred(at(30, 25)));
            ok("and the outside of it is not", iswhite(at(8, 30)));
            picture_free(&p);
        }
    }

    /* --- relative commands and a closed shape --------------------------------
     *
     * Lower case means relative, and a path written entirely in relatives is
     * what a drawing program emits. Getting it wrong puts everything after
     * the first command in the wrong place. */
    {
        int rc = draw("<svg width='40' height='40'>"
                      "<path d='m 10 10 h 20 v 20 h -20 z' fill='red'/>"
                      "</svg>", 40, 40);
        okn("a path in relative commands renders", rc == SVG_OK, rc);
        if (rc == SVG_OK) {
            ok("and the square it draws is where it should be",
               isred(at(20, 20)));
            ok("and its corner is outside it", iswhite(at(5, 5)));
            ok("and so is the other one", iswhite(at(35, 35)));
            picture_free(&p);
        }
    }

    /* --- a transform ---------------------------------------------------------- */
    {
        int rc = draw("<svg width='40' height='40'>"
                      "<g transform='translate(20 0)'>"
                      "<rect x='0' y='10' width='10' height='10' fill='red'/>"
                      "</g></svg>", 40, 40);
        okn("a transform moves what is inside it", rc == SVG_OK, rc);
        if (rc == SVG_OK) {
            ok("the shape is where the transform put it", isred(at(25, 15)));
            ok("and not where it was written", iswhite(at(5, 15)));
            picture_free(&p);
        }
    }

    /* --- a circle --------------------------------------------------------------
     *
     * Four cubics, so this asks the one thing that tells a circle from the
     * square that contains it: the corners are outside. */
    {
        int rc = draw("<svg width='40' height='40'>"
                      "<circle cx='20' cy='20' r='15' fill='red'/></svg>", 40, 40);
        okn("a circle renders", rc == SVG_OK, rc);
        if (rc == SVG_OK) {
            ok("its middle is filled", isred(at(20, 20)));
            ok("and the edge of it is too", isred(at(20, 7)));
            ok("but its corners are not, which is what makes it a circle",
               iswhite(at(7, 7)) && iswhite(at(33, 33)));
            picture_free(&p);
        }
    }

    /* --- what is not filled ------------------------------------------------- */
    {
        int rc = draw("<svg width='40' height='40'>"
                      "<rect x='5' y='5' width='30' height='30' fill='none'/>"
                      "</svg>", 40, 40);
        /* Nothing was drawn at all, which is a real answer rather than a
           failure, and is different from a drawing this cannot read. */
        okn("a shape filled with nothing draws nothing",
            rc == SVG_NOTHING_IN_IT, rc);
    }

    /* --- both winding rules --------------------------------------------------
     *
     * Two squares, one inside the other, drawn the same way round. Under the
     * nonzero rule the middle is filled; under even-odd it is a hole. Pages
     * rely on both and a renderer that only has one draws rings as discs. */
    {
        const char *two = "<svg width='60' height='60'>"
                          "<path d='M5 5 H55 V55 H5 Z M20 20 H40 V40 H20 Z' "
                          "fill='red' fill-rule='evenodd'/></svg>";
        int rc = draw(two, 60, 60);
        okn("a shape with a hole in it renders", rc == SVG_OK, rc);
        if (rc == SVG_OK) {
            ok("the ring around the hole is filled", isred(at(10, 30)));
            ok("and under even-odd the middle is a hole", iswhite(at(30, 30)));
            picture_free(&p);
        }
    }

    /* --- a stroke -------------------------------------------------------------- */
    {
        int rc = draw("<svg width='40' height='40'>"
                      "<line x1='5' y1='20' x2='35' y2='20' "
                      "stroke='red' stroke-width='4'/></svg>", 40, 40);
        okn("a stroked line renders", rc == SVG_OK, rc);
        if (rc == SVG_OK) {
            ok("there is ink along the line", isred(at(20, 20)));
            ok("and none well away from it", iswhite(at(20, 5)));
            picture_free(&p);
        }
    }

    /* --- paint said once, for everything in a group -------------------------
     *
     * Fill and stroke are inherited: said on a <g>, or on the <svg> itself,
     * they apply to every shape inside that says nothing of its own. They
     * were read off each shape alone, so a logo drawn as one filled group
     * came out as a black silhouette. */
    {
        int rc = draw("<svg width='40' height='40'><g fill='red'>"
                      "<rect x='10' y='10' width='20' height='20'/>"
                      "</g></svg>", 40, 40);
        okn("a group's fill renders", rc == SVG_OK, rc);
        if (rc == SVG_OK) {
            ok("and colours the shape inside it", isred(at(20, 20)));
            picture_free(&p);
        }

        rc = draw("<svg width='40' height='40' fill='none' stroke='red' stroke-width='4'>"
                  "<rect x='10' y='10' width='20' height='20'/></svg>", 40, 40);
        if (rc == SVG_OK) {
            ok("a stroke said on the drawing itself outlines the shapes in it",
               isred(at(10, 20)));
            ok("and its fill of none leaves them hollow", iswhite(at(20, 20)));
            picture_free(&p);
        } else {
            okn("a drawing with its paint on the root renders", 0, rc);
        }

        rc = draw("<svg width='40' height='40'><g style='fill:red'>"
                  "<g><circle cx='20' cy='20' r='10'/></g></g></svg>", 40, 40);
        if (rc == SVG_OK) {
            ok("a fill in a group's style reaches a shape two groups down",
               isred(at(20, 20)));
            picture_free(&p);
        } else {
            okn("a drawing with nested groups renders", 0, rc);
        }

        rc = draw("<svg width='40' height='40'><g fill='red'>"
                  "<rect x='10' y='10' width='20' height='20' fill='none' stroke='red'/>"
                  "</g></svg>", 40, 40);
        if (rc == SVG_OK) {
            ok("and a shape's own paint still wins over its group's",
               iswhite(at(20, 20)));
            picture_free(&p);
        } else {
            okn("a shape overriding its group renders", 0, rc);
        }
    }

    /* --- what is there to be referred to, and paint that is not there -------- */
    {
        int rc = draw("<svg width='40' height='40'><defs>"
                      "<rect x='0' y='0' width='40' height='40' fill='red'/></defs>"
                      "<clipPath id='c'><rect x='0' y='0' width='40' height='40'/></clipPath>"
                      "<circle cx='20' cy='20' r='6' fill='red'/></svg>", 40, 40);
        if (rc == SVG_OK) {
            ok("a shape inside <defs> or a clip path is not drawn where it stands",
               iswhite(at(3, 3)) && isred(at(20, 20)));
            picture_free(&p);
        } else {
            okn("a drawing with <defs> in it renders", 0, rc);
        }

        /* Two groups side by side: the second is not inside the first, so
           it takes neither its paint nor its move. The parser used to let
           an end tag it had no name for close nothing. */
        rc = draw("<svg width='40' height='40'>"
                  "<g fill='red' transform='translate(0 0)'><rect x='0' y='0' width='20' height='40'/></g>"
                  "<g transform='translate(20 0)'><rect x='0' y='0' width='20' height='40'/></g>"
                  "</svg>", 40, 40);
        if (rc == SVG_OK) {
            ok("a group's paint stops at its end tag", isred(at(10, 20)) && at(30, 20) == 0);
            picture_free(&p);
        } else {
            okn("two groups side by side render", 0, rc);
        }

        rc = draw("<svg width='40' height='40'>"
                  "<rect x='0' y='0' width='40' height='40' fill='red'/>"
                  "<rect x='10' y='10' width='20' height='20' fill='transparent'/></svg>", 40, 40);
        if (rc == SVG_OK) {
            ok("a transparent fill leaves what is under it showing", isred(at(20, 20)));
            picture_free(&p);
        } else {
            okn("a drawing with a transparent fill renders", 0, rc);
        }

        rc = draw("<svg width='40' height='40'><polygon points='10,10 30,10 30,30 10,30' "
                  "fill='none' stroke='red' stroke-width='4'/></svg>", 40, 40);
        if (rc == SVG_OK) {
            ok("a stroked polygon is closed, its last corner joined to its first",
               isred(at(10, 20)));
            picture_free(&p);
        } else {
            okn("a stroked polygon renders", 0, rc);
        }
    }

    /* --- and what it refuses ---------------------------------------------------- */
    {
        okn("something that is not a drawing says so",
            draw("<html><body>not a drawing</body></html>", 40, 40)
            == SVG_NOT_SVG, SVG_NOT_SVG);
        ok("and a refusal leaves nothing to free", p.rgb == 0);
    }

    /* --- nothing was left behind -------------------------------------------- */
    {
        u32 before = heap_live();
        for (int i = 0; i < 20; i++) {
            if (draw("<svg width='40' height='40'>"
                     "<circle cx='20' cy='20' r='15' fill='red'/></svg>",
                     40, 40) == SVG_OK)
                picture_free(&p);
        }
        okn("twenty drawings rendered and freed leave the heap where it was",
            heap_live() <= before, (int)(heap_live() - before));
    }

    /* --- a viewBox, spelled the way drawings spell it -------------------
     *
     * With a capital B, which is the whole of this check. Attribute names
     * are folded to lower case by the parser, because HTML does not care
     * about their case, and this asked for the folded name by its
     * unfolded spelling and never found it. Nothing reported anything:
     * with no viewBox the fallback is a hundred by a hundred, so a wide
     * drawing asked for at two hundred by one hundred and twenty came out
     * at a hundred and twenty by seventy two -- the ratio of the height
     * to a square it never had. It looked like a drawing that worked.
     */
    {
        static const char VB[] =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 60\">"
            "<rect x=\"0\" y=\"0\" width=\"100\" height=\"60\" fill=\"#e11d48\"/></svg>";

        picture p;
        int rc = svg_render(VB, (int)sizeof(VB) - 1, 200, 120, &p,
                            0xFFFFFF);
        okn("a drawing with a viewBox renders", rc == SVG_OK, rc);

        if (rc == SVG_OK) {
            okn("at the size it was asked for", p.w == 200 && p.h == 120,
                p.w);

            /* The rectangle is the whole of the viewBox, so scaled into
               the surface it is the whole of the surface. Counting the
               corners is the same question as "was the viewBox read", and
               it is the question a wrong scale answers no to. */
            int inked = 0;
            for (int i = 0; i < p.w * p.h; i++)
                if (p.rgb[i * 3] == 0xE1 && p.rgb[i * 3 + 1] == 0x1D
                    && p.rgb[i * 3 + 2] == 0x48) inked++;
            okn("and the drawing fills it rather than a corner of it",
                inked > 200 * 120 * 9 / 10, inked);
            picture_free(&p);
        }
    }

    /* --- a drawing written into a page ------------------------------------
     *
     * Drawn from the page's own tree rather than from markup of its own, on
     * the backdrop behind it (the picture is opaque), with currentColor the
     * colour of the words around it: an icon in a link is the link's colour,
     * and on a dark header a white logo drawn on white was a white box. */
    {
        static const char PAGE[] =
            "<html><body><p>before <svg width=20 height=20 viewBox=\"0 0 20 20\">"
            "<rect x=0 y=0 width=10 height=20 fill=\"currentColor\"/></svg> after</p></body></html>";
        ddoc *d = (ddoc *)malloc((u32)sizeof(ddoc));
        int root = -1;
        if (d) {
            dom_parse(d, PAGE, (int)sizeof(PAGE) - 1);
            for (int i = 0; i < d->count; i++)
                if (d->nodes[i].kind == DN_ELEMENT && d->nodes[i].tag == T_SVG) { root = i; break; }
        }
        int rc = root >= 0 ? svg_render_tree(d, root, 20, 20, &p, 0x123456, 0xFF0000) : -99;
        okn("an svg in a page is drawn from the page's tree", rc == SVG_OK && p.w == 20 && p.h == 20, rc);
        okn("currentColor is the colour it was given", isred(at(5, 10)), at(5, 10));
        okn("and the rest is the backdrop behind it", at(15, 10) == 0x123456, at(15, 10));
        picture_free(&p);

        /* One that arrived as a file of its own has no words around it:
           currentColor is black, as it was. */
        rc = draw("<svg width=\"20\" height=\"20\"><rect width=\"20\" height=\"20\" fill=\"currentColor\"/></svg>",
                  20, 20);
        okn("in a file of its own currentColor is black", rc == SVG_OK && at(10, 10) == 0x000000, at(10, 10));
        picture_free(&p);
        free(d);
    }

    puts(failed ? "SVGTEST_FAIL\n" : "SVGTEST_PASS\n");
    return failed;
}
