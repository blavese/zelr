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

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

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

    puts(failed ? "SVGTEST_FAIL\n" : "SVGTEST_PASS\n");
    return failed;
}
