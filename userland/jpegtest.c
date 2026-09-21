/* The photograph decoder, against photographs it did not make.
 *
 * JPEG throws pixels away on purpose, so nothing here can ask for a pixel
 * back exactly. What it asks instead is that the picture is the picture:
 * a red square is red, an edge is where the edge was, a ramp ramps, and the
 * colours are the right way round.
 *
 * That last one is the check worth having. Brightness and the two colour
 * differences are easy to put back together swapped, and the result still
 * looks like a photograph — just the wrong one — so a person glancing at it
 * would pass it. A number will not.
 */
#include "zelr.h"
#include "alloc.h"
#include "jpeg.h"
#include "jpegdata.h"

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

static int red(const picture *p, int x, int y) {
    return p->rgb[(y * p->w + x) * 3];
}
static int green(const picture *p, int x, int y) {
    return p->rgb[(y * p->w + x) * 3 + 1];
}
static int blue(const picture *p, int x, int y) {
    return p->rgb[(y * p->w + x) * 3 + 2];
}

/* What the format loses. A flat area comes back within a few of itself; an
   edge is blurred over a pixel or two either side, which is why every
   sample below is taken well inside its own region. */
static int near(int got, int want, int slack) {
    int d = got - want;
    if (d < 0) d = -d;
    return d <= slack;
}

int main(void) {
    puts("photographs\n");

    /* --- a solid colour -----------------------------------------------------
     *
     * The simplest picture there is, and the one that fails outright when
     * the colour conversion is wrong rather than subtly. */
    {
        picture p;
        int rc = jpeg_decode(JPG_FLAT, (int)sizeof(JPG_FLAT), &p);
        okn("a photograph decodes", rc == JPG_OK, rc);

        if (rc == JPG_OK) {
            okn("and it is the size it said it was",
                p.w == JPG_FLAT_W && p.h == JPG_FLAT_H, p.w);

            okn("and a solid colour comes back as that colour",
                near(red(&p, 16, 16), 200, 12)
                && near(green(&p, 16, 16), 30, 12)
                && near(blue(&p, 16, 16), 40, 12), red(&p, 16, 16));

            /* The same everywhere, because a flat picture that is flat in
               one corner and not another is an inverse transform that is
               nearly right. */
            ok("and it is that colour in every corner",
               near(red(&p, 2, 2), 200, 14) && near(red(&p, 29, 2), 200, 14)
               && near(red(&p, 2, 29), 200, 14) && near(red(&p, 29, 29), 200, 14));
            picture_free(&p);
        }
    }

    /* --- four colours, one per quarter --------------------------------------
     *
     * Red, green, blue and white, clockwise from the top left. Getting the
     * two colour planes the wrong way round swaps red with blue and nothing
     * else, which is exactly the failure a picture cannot show you. */
    {
        picture p;
        int rc = jpeg_decode(JPG_QUARTERS, (int)sizeof(JPG_QUARTERS), &p);
        okn("a photograph of four colours decodes", rc == JPG_OK, rc);

        if (rc == JPG_OK) {
            int q = p.w / 4, h = p.h / 4;

            okn("the top left quarter is red",
                red(&p, q, h) > 180 && green(&p, q, h) < 80
                && blue(&p, q, h) < 80, red(&p, q, h));

            okn("the top right is green",
                green(&p, q * 3, h) > 180 && red(&p, q * 3, h) < 100
                && blue(&p, q * 3, h) < 100, green(&p, q * 3, h));

            okn("the bottom left is blue, and not red",
                blue(&p, q, h * 3) > 180 && red(&p, q, h * 3) < 80,
                blue(&p, q, h * 3));

            ok("and the bottom right is white",
               red(&p, q * 3, h * 3) > 200 && green(&p, q * 3, h * 3) > 200
               && blue(&p, q * 3, h * 3) > 200);
            picture_free(&p);
        }
    }

    /* --- a ramp --------------------------------------------------------------
     *
     * What the cosine transform is actually for. A wrong inverse turns a
     * smooth ramp into eight pixel steps or into noise, and either shows up
     * as the middle not being in the middle. */
    {
        picture p;
        int rc = jpeg_decode(JPG_GRADIENT, (int)sizeof(JPG_GRADIENT), &p);
        okn("a ramp decodes", rc == JPG_OK, rc);

        if (rc == JPG_OK) {
            ok("it is dark at the dark end", red(&p, 1, 12) < 40);
            ok("and bright at the bright end", red(&p, p.w - 2, 12) > 215);
            okn("and half way along it is half way up",
                near(red(&p, p.w / 2, 12), 128, 24), red(&p, p.w / 2, 12));

            /* It climbs the whole way, block by block.
             *
             * Not pixel by pixel: the format works in blocks of eight and
             * quantising the slope leaves a small step at each boundary, so
             * a ramp really does go backwards by ten or so eight times
             * along. That is the format, not the decoder, and a check that
             * forbade it would be a check that forbade JPEG.
             *
             * What a wrong transform does is different in kind — blocks in
             * the wrong order, or flat, or noise — and comparing the
             * average of each block against the one before it catches that
             * while ignoring the ringing at their edges. */
            int blocks = p.w / 8;
            int rising = 0, last_avg = -1;
            for (int b = 0; b < blocks; b++) {
                int sum = 0;
                for (int x = b * 8; x < b * 8 + 8; x++) sum += red(&p, x, 12);
                int avg = sum / 8;
                if (last_avg >= 0 && avg > last_avg) rising++;
                last_avg = avg;
            }
            okn("and it climbs the whole way rather than only at the ends",
                rising == blocks - 1, rising);

            /* And no single step is a cliff, which is what a block decoded
               out of order looks like. */
            int worst = 0;
            for (int x = 4; x < p.w - 4; x++) {
                int step = red(&p, x - 1, 12) - red(&p, x, 12);
                if (step > worst) worst = step;
            }
            okn("and nothing along it falls off a cliff", worst < 40, worst);
            picture_free(&p);
        }
    }

    /* --- a size that is not a multiple of eight -------------------------------
     *
     * The blocks at the right and bottom edges run off the picture. They are
     * still in the stream and still have to be decoded, and a decoder that
     * skips them loses its place in the bits and everything after is noise. */
    {
        picture p;
        int rc = jpeg_decode(JPG_TALL, (int)sizeof(JPG_TALL), &p);
        okn("a picture whose size is not a multiple of eight decodes",
            rc == JPG_OK, rc);

        if (rc == JPG_OK) {
            okn("and comes out the odd size it is",
                p.w == JPG_TALL_W && p.h == JPG_TALL_H, p.h);

            /* The bottom right quarter is white, and it is the furthest
               thing in the stream from the start: if the edge blocks lost
               the decoder's place, this is where it shows. */
            ok("and the far corner is still the right colour",
               red(&p, p.w - 3, p.h - 3) > 190
               && blue(&p, p.w - 3, p.h - 3) > 190);
            picture_free(&p);
        }
    }

    /* --- and what it refuses --------------------------------------------------- */
    {
        picture p;
        static const u8 NOT_A_JPEG[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
        okn("something that is not a jpeg says so",
            jpeg_decode(NOT_A_JPEG, 8, &p) == JPG_NOT_JPEG, JPG_NOT_JPEG);

        okn("and one that stops part way says that instead",
            jpeg_decode(JPG_FLAT, 40, &p) < 0, 1);

        /* A progressive file is a different decoder, not a variation on
           this one, and is refused by name. */
        static u8 prog[sizeof(JPG_QUARTERS)];
        for (u32 i = 0; i < sizeof(JPG_QUARTERS); i++) prog[i] = JPG_QUARTERS[i];
        int found = 0;
        for (u32 i = 0; i + 1 < sizeof(prog); i++)
            if (prog[i] == 0xFF && prog[i + 1] == 0xC0) {
                prog[i + 1] = 0xC2;             /* baseline becomes progressive */
                found = 1;
                break;
            }
        okn("and a progressive one is refused rather than half decoded",
            !found || jpeg_decode(prog, (int)sizeof(prog), &p) == JPG_PROGRESSIVE,
            JPG_PROGRESSIVE);

        ok("and a refusal leaves nothing to free", p.rgb == 0);
    }

    /* --- nothing was left behind ------------------------------------------- */
    {
        u32 before = heap_live();
        picture p;
        for (int i = 0; i < 20; i++)
            if (jpeg_decode(JPG_QUARTERS, (int)sizeof(JPG_QUARTERS), &p) == JPG_OK)
                picture_free(&p);
        okn("twenty decoded and freed leave the heap where it was",
            heap_live() <= before, (int)(heap_live() - before));
    }

    puts(failed ? "JPEGTEST_FAIL\n" : "JPEGTEST_PASS\n");
    return failed;
}
