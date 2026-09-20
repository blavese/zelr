/* The picture decoder, against pictures it did not make.
 *
 * The files come from tools/genpng.py, which uses the host's zlib to build
 * them. That is the point: a decoder checked against its own encoder agrees
 * with itself and proves nothing. These were made by something else, one per
 * thing that can go wrong — every row filter, every colour kind, a stored
 * deflate block and a compressed one.
 *
 * Headless, because none of this is about the screen. Whether the picture
 * reaches the glass is the browser check's question; whether the bytes are
 * the right bytes is this one's, and it answers in about a second.
 */
#include "zelr.h"
#include "alloc.h"
#include "png.h"
#include "pngdata.h"

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

/* The colour at a point, as one number, which is easier to read in a
   failure than three. */
static int at(const picture *p, int x, int y) {
    const u8 *px = p->rgb + (y * p->w + x) * 3;
    return (px[0] << 16) | (px[1] << 8) | px[2];
}

int main(void) {
    puts("pictures\n");

    /* --- every filter, undone in order ------------------------------------
     *
     * Five rows, five filters, and each row after the first is relative to
     * the one above it. Getting one wrong slides everything below it, so
     * comparing the whole picture is the check rather than a corner of it. */
    {
        picture p;
        int rc = png_decode(PNG_FILTERS, (int)sizeof(PNG_FILTERS), &p, 0xFFFFFF);
        okn("a truecolour picture decodes", rc == PNG_OK, rc);

        if (rc == PNG_OK) {
            okn("and it is the size it said it was", p.w == 8 && p.h == 5, p.w);

            int wrong = -1;
            for (int i = 0; i < p.w * p.h * 3; i++)
                if (p.rgb[i] != PNG_WANT[i]) { wrong = i; break; }
            okn("and every pixel of it came out right, through all five filters",
                wrong < 0, wrong);
            picture_free(&p);
        }
    }

    /* --- the same picture, not compressed ----------------------------------
     *
     * A stored block is a different path: no Huffman at all, whole bytes,
     * and it is what a very small picture usually turns out to be. */
    {
        picture p;
        int rc = png_decode(PNG_STORED, (int)sizeof(PNG_STORED), &p, 0xFFFFFF);
        okn("a picture stored rather than compressed decodes", rc == PNG_OK, rc);
        if (rc == PNG_OK) {
            int wrong = -1;
            for (int i = 0; i < p.w * p.h * 3; i++)
                if (p.rgb[i] != PNG_WANT[i]) { wrong = i; break; }
            ok("and comes out as the same picture", wrong < 0);
            picture_free(&p);
        }
    }

    /* --- a palette, and one colour of it transparent ------------------------ */
    {
        picture p;
        int rc = png_decode(PNG_PALETTE, (int)sizeof(PNG_PALETTE), &p, 0x00FF00);
        okn("a palette picture decodes", rc == PNG_OK, rc);

        if (rc == PNG_OK) {
            /* The palette is red, green, blue, white; the rows are
               0 1 2 3 / 3 2 1 0 / 1 1 2 2 / 0 3 0 3. */
            ok("a colour out of the palette is that colour",
               at(&p, 1, 0) == 0x00FF00);
            ok("and so is one further along it", at(&p, 2, 0) == 0x0000FF);
            ok("and the last of them", at(&p, 3, 0) == 0xFFFFFF);
            ok("and the order is right the other way along",
               at(&p, 0, 1) == 0xFFFFFF);

            /* Entry zero is transparent, so it should be the background
               that was handed in rather than the red in the palette. */
            ok("a transparent entry comes out as what is behind it",
               at(&p, 0, 0) == 0x00FF00);
            picture_free(&p);
        }
    }

    /* --- one bit a pixel ----------------------------------------------------
     *
     * Packed high bit first, which is the way round that is easy to get
     * wrong and looks like a mirrored picture when it is. */
    {
        picture p;
        int rc = png_decode(PNG_GREY1, (int)sizeof(PNG_GREY1), &p, 0xFFFFFF);
        okn("a one bit greyscale picture decodes", rc == PNG_OK, rc);

        if (rc == PNG_OK) {
            /* 0b10110001: white, black, white, white, black, black, black, white */
            ok("the first pixel is the high bit", at(&p, 0, 0) == 0xFFFFFF);
            ok("and the second is the one after it", at(&p, 1, 0) == 0x000000);
            ok("and the last is the low bit", at(&p, 7, 0) == 0xFFFFFF);
            ok("and the row below is its own", at(&p, 0, 1) == 0x000000);
            picture_free(&p);
        }
    }

    /* --- alpha, laid over a background --------------------------------------
     *
     * The result has no alpha of its own, so a half transparent pixel has to
     * come out half way between its colour and what is behind it. */
    {
        picture p;
        int rc = png_decode(PNG_ALPHA, (int)sizeof(PNG_ALPHA), &p, 0x000000);
        okn("a picture with alpha decodes", rc == PNG_OK, rc);

        if (rc == PNG_OK) {
            ok("an opaque pixel is its own colour", at(&p, 0, 0) == 0xFF0000);

            /* Full green at an alpha of 128, over black, is half of full
               green: 255 * 128 / 255 is 128. This check first asked for 64,
               which is what halving the alpha would give rather than
               halving the colour, and the decoder was right. */
            int mid = at(&p, 1, 0);
            okn("a half transparent one is half way to the background",
                ((mid >> 8) & 0xFF) > 120 && ((mid >> 8) & 0xFF) < 136
                && (mid & 0xFF) == 0 && ((mid >> 16) & 0xFF) == 0,
                (mid >> 8) & 0xFF);

            ok("and a fully transparent one is the background",
               at(&p, 0, 1) == 0x000000);
            picture_free(&p);
        }
    }

    /* --- and what it refuses ------------------------------------------------ */
    {
        picture p;
        static const u8 NOT_A_PNG[16] = { 'G', 'I', 'F', '8', '9', 'a', 1, 0,
                                          1, 0, 0, 0, 0, 0, 0, 0 };
        okn("something that is not a png says so",
            png_decode(NOT_A_PNG, 16, &p, 0) == PNG_NOT_PNG, PNG_NOT_PNG);

        okn("and one that stops part way says that instead",
            png_decode(PNG_FILTERS, 20, &p, 0) == PNG_TRUNCATED, PNG_TRUNCATED);

        /* An interlaced file is refused rather than drawn wrongly: it
           arrives in seven passes and this reads one. */
        static u8 lace[sizeof(PNG_FILTERS)];
        for (u32 i = 0; i < sizeof(PNG_FILTERS); i++) lace[i] = PNG_FILTERS[i];
        lace[8 + 8 + 12] = 1;                  /* the interlace byte in IHDR */
        okn("and an interlaced one is refused rather than half drawn",
            png_decode(lace, (int)sizeof(lace), &p, 0) == PNG_UNSUPPORTED,
            PNG_UNSUPPORTED);

        ok("and a refusal leaves nothing to free", p.rgb == 0);
    }

    /* --- nothing was left behind --------------------------------------------
     *
     * Every picture above was freed. A browser decodes one per image on
     * every page it shows, so a decoder that keeps a little of each is a
     * browser that cannot stay open. */
    {
        u32 before = heap_live();
        picture p;
        for (int i = 0; i < 50; i++) {
            if (png_decode(PNG_FILTERS, (int)sizeof(PNG_FILTERS), &p,
                           0xFFFFFF) == PNG_OK)
                picture_free(&p);
        }
        okn("fifty pictures decoded and freed leave the heap where it was",
            heap_live() <= before, (int)(heap_live() - before));
    }

    puts(failed ? "PNGTEST_FAIL\n" : "PNGTEST_PASS\n");
    return failed;
}
