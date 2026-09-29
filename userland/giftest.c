/* The GIF decoder, against pictures it did not make.
 *
 * The files come from tools/gengif.ps1, which uses Windows' own encoder, so
 * they were compressed by something else: one small picture for the colour
 * table and the rows, one of noise for codes past nine bits and a table that
 * fills and is cleared, one with a transparent colour, and one interlaced.
 * Every pixel of each is compared with the index the script put there.
 */
#include "zelr.h"
#include "alloc.h"
#include "gif.h"
#include "gifdata.h"

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

/* The colour gengif.ps1 gave palette entry i. */
static u32 entry(int i) {
    return (u32)(((i * 37) % 256) << 16 | ((i * 91) % 256) << 8 | ((i * 53 + 17) % 256));
}

static u32 at(const picture *p, int x, int y) {
    const u8 *px = p->rgb + (y * p->w + x) * 3;
    return ((u32)px[0] << 16) | ((u32)px[1] << 8) | px[2];
}

/* How many pixels differ from what the index function says. */
static int wrong(const picture *p, int kind) {
    int bad = 0;
    for (int y = 0; y < p->h; y++)
        for (int x = 0; x < p->w; x++) {
            int i;
            if (kind == 0) i = (y * 5 + x) % 16;
            else if (kind == 1) i = (x * 7919 + y * 104729 + x * y * 31) & 0xFF;
            else i = (y * 3 + x) % 64;
            if (at(p, x, y) != entry(i)) bad++;
        }
    return bad;
}

int main(void) {
    puts("gif pictures\n");
    picture p;

    int rc = gif_decode(GIF_SMALL, (int)sizeof(GIF_SMALL), &p, 0xFFFFFF);
    ok("a small picture decodes", rc == GIF_OK);
    if (rc == GIF_OK) {
        ok("at the size it says", p.w == 5 && p.h == 4);
        okn("with every pixel the colour its index names", wrong(&p, 0) == 0, wrong(&p, 0));
        free(p.rgb);
    }

    rc = gif_decode(GIF_NOISY, (int)sizeof(GIF_NOISY), &p, 0xFFFFFF);
    ok("a picture of noise decodes", rc == GIF_OK);
    if (rc == GIF_OK) {
        ok("at its size", p.w == 160 && p.h == 120);
        okn("and every pixel is right, past nine-bit codes and a table cleared", wrong(&p, 1) == 0,
            wrong(&p, 1));
        free(p.rgb);
    }

    rc = gif_decode(GIF_HOLES, (int)sizeof(GIF_HOLES), &p, 0x123456);
    ok("a picture with a transparent colour decodes", rc == GIF_OK);
    if (rc == GIF_OK) {
        int shows = 0, others = 1;
        for (int y = 0; y < p.h; y++)
            for (int x = 0; x < p.w; x++) {
                int i = (y * 5 + x) % 16;
                if (i == 3) shows += at(&p, x, y) == 0x123456;
                else if (at(&p, x, y) != entry(i)) others = 0;
            }
        okn("and the page shows through where it is", shows == 2, shows);
        ok("and nowhere else", others);
        free(p.rgb);
    }

    rc = gif_decode(GIF_LACED, (int)sizeof(GIF_LACED), &p, 0xFFFFFF);
    ok("an interlaced picture decodes", rc == GIF_OK);
    if (rc == GIF_OK) {
        okn("with its rows back in order", wrong(&p, 2) == 0, wrong(&p, 2));
        free(p.rgb);
    }

    ok("a file that is not a GIF is refused", gif_decode((const u8 *)"GIF88a hello there", 18, &p, 0) == GIF_NOT_GIF);
    ok("and one cut short is refused, not read past", gif_decode(GIF_SMALL, 20, &p, 0) != GIF_OK);

    /* Cut off in the middle of its pixels: what arrived is shown, and the
       rest is the page -- the bytes after the cut are still there in this
       array, so a decoder that read past the end would draw them. */
    rc = gif_decode(GIF_NOISY, (int)sizeof(GIF_NOISY) - 4000, &p, 0x123456);
    ok("a picture cut off in its pixels still decodes", rc == GIF_OK);
    if (rc == GIF_OK) {
        okn("with what arrived drawn", at(&p, 0, 0) == entry(0), (int)at(&p, 0, 0));
        okn("and nothing past the cut", at(&p, p.w - 1, p.h - 1) == 0x123456, (int)at(&p, p.w - 1, p.h - 1));
        free(p.rgb);
    }

    puts(failed ? "GIFTEST_FAIL\n" : "GIFTEST_PASS\n");
    return failed;
}
