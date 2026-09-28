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
#include "jpegprog.h"

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

    /* --- progressive ------------------------------------------------------------
     *
     * Each picture twice from the same quantised coefficients, by an encoder
     * that is not this decoder (tools/genjpegprog.py): a progressive file is
     * the same numbers sent in another order, so it has to come out as the
     * same pixels as its baseline twin, byte for byte. Between them they carry
     * every kind of progressive scan -- DC first and refined, AC in bands, AC
     * refined, runs of empty blocks -- and a picture whose sides are not whole
     * blocks (tall), one with no subsampling (ramp), one halved across only
     * (rings), one component (grey), and restart markers (restarts). */
    {
        struct { const u8 *b; int bn; const u8 *p; int pn; int w, h; } pairs[] = {
            { JPB_QUARTERS, sizeof(JPB_QUARTERS), JPP_QUARTERS, sizeof(JPP_QUARTERS), JP_QUARTERS_W, JP_QUARTERS_H },
            { JPB_TALL, sizeof(JPB_TALL), JPP_TALL, sizeof(JPP_TALL), JP_TALL_W, JP_TALL_H },
            { JPB_RAMP, sizeof(JPB_RAMP), JPP_RAMP, sizeof(JPP_RAMP), JP_RAMP_W, JP_RAMP_H },
            { JPB_RINGS, sizeof(JPB_RINGS), JPP_RINGS, sizeof(JPP_RINGS), JP_RINGS_W, JP_RINGS_H },
            { JPB_GREY, sizeof(JPB_GREY), JPP_GREY, sizeof(JPP_GREY), JP_GREY_W, JP_GREY_H },
            { JPB_RESTARTS, sizeof(JPB_RESTARTS), JPP_RESTARTS, sizeof(JPP_RESTARTS), JP_RESTARTS_W, JP_RESTARTS_H },
        };
        int n = (int)(sizeof(pairs) / sizeof(pairs[0]));
        int decoded = 0, sized = 1, same = 1;
        for (int i = 0; i < n; i++) {
            picture b, pr;
            int rb = jpeg_decode(pairs[i].b, pairs[i].bn, &b);
            int rp = jpeg_decode(pairs[i].p, pairs[i].pn, &pr);
            if (rb == JPG_OK && rp == JPG_OK) {
                decoded++;
                if (b.w != pairs[i].w || b.h != pairs[i].h || pr.w != b.w || pr.h != b.h) sized = 0;
                else
                    for (int k = 0; k < b.w * b.h * 3; k++)
                        if (b.rgb[k] != pr.rgb[k]) { same = 0; break; }
            } else {
                same = 0;
            }
            if (rb == JPG_OK) picture_free(&b);
            if (rp == JPG_OK) picture_free(&pr);
        }
        okn("every progressive picture decodes, and its baseline twin", decoded == n, decoded);
        ok("and each is the size it said", sized);
        ok("and is the same pixels as the same picture sent baseline, byte for byte", same);

        picture q;
        int rc = jpeg_decode(JPP_QUARTERS, sizeof(JPP_QUARTERS), &q);
        if (rc == JPG_OK) {
            ok("and a progressive picture of four colours has them in their places",
               red(&q, 8, 8) > 200 && green(&q, 8, 8) < 60
               && green(&q, q.w - 8, 8) > 200 && red(&q, q.w - 8, 8) < 60
               && blue(&q, 8, q.h - 8) > 200 && red(&q, 8, q.h - 8) < 60
               && red(&q, q.w - 8, q.h - 8) > 200 && blue(&q, q.w - 8, q.h - 8) > 200);
            picture_free(&q);
        }

        /* A progressive picture is sent the way it is so that it can be shown
           before it has all arrived, and a page that stops loading part way
           through one is ordinary: what came is shown. */
        picture cut;
        int cn = (int)sizeof(JPP_RINGS) * 3 / 5;
        rc = jpeg_decode(JPP_RINGS, cn, &cut);
        okn("and one that stops part way is shown from what arrived",
            rc == JPG_OK && cut.w == JP_RINGS_W && cut.h == JP_RINGS_H, rc);
        if (rc == JPG_OK) picture_free(&cut);
    }

    /* --- and what it refuses --------------------------------------------------- */
    {
        picture p;
        static const u8 NOT_A_JPEG[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
        okn("something that is not a jpeg says so",
            jpeg_decode(NOT_A_JPEG, 8, &p) == JPG_NOT_JPEG, JPG_NOT_JPEG);

        okn("and one that stops part way says that instead",
            jpeg_decode(JPG_FLAT, 40, &p) < 0, 1);

        /* A baseline scan in a frame that says it is progressive: a DC scan
           that runs on to the sixty fourth frequency is no scan the format
           has, and is refused as broken rather than half decoded. */
        static u8 prog[sizeof(JPG_QUARTERS)];
        for (u32 i = 0; i < sizeof(JPG_QUARTERS); i++) prog[i] = JPG_QUARTERS[i];
        int found = 0;
        for (u32 i = 0; i + 1 < sizeof(prog); i++)
            if (prog[i] == 0xFF && prog[i + 1] == 0xC0) {
                prog[i + 1] = 0xC2;             /* baseline becomes progressive */
                found = 1;
                break;
            }
        okn("and a baseline scan labelled progressive is refused as broken",
            found && jpeg_decode(prog, (int)sizeof(prog), &p) == JPG_BAD, JPG_BAD);

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

    /* --- the transform with the zeroes left out ------------------------------
     *
     * jpg_idct skips the terms whose coefficient is zero, which is most of
     * them, and says every byte is what the whole formula gives. So it is
     * held to that: the whole formula, written out here as it was, against
     * the decoder's, on blocks as sparse as a photograph's and on full ones,
     * byte for byte. */
    {
        jpg_cos_init();
        u32 seed = 12345;
        int same = 1, blocks = 0;
        for (int b = 0; b < 400 && same; b++) {
            int coef[64];
            int keep = b % 5 == 4 ? 64 : 1 + b % 12;   /* mostly sparse, some full */
            for (int i = 0; i < 64; i++) coef[i] = 0;
            for (int i = 0; i < keep; i++) {
                seed = seed * 1103515245u + 12345u;
                int at = (int)((seed >> 16) % 64);
                seed = seed * 1103515245u + 12345u;
                coef[at] = (int)((seed >> 16) % 2001) - 1000;
            }
            u8 fast[64], whole[64];
            jpg_idct(coef, fast, 8);

            float tmp[64];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    float sum = 0;
                    for (int u = 0; u < 8; u++) sum += JPG_COS[u][x] * (float)coef[y * 8 + u];
                    tmp[y * 8 + x] = sum;
                }
            for (int x = 0; x < 8; x++)
                for (int y = 0; y < 8; y++) {
                    float sum = 0;
                    for (int v = 0; v < 8; v++) sum += JPG_COS[v][y] * tmp[v * 8 + x];
                    int val = (int)(sum + 128.5f);
                    if (val < 0) val = 0;
                    if (val > 255) val = 255;
                    whole[y * 8 + x] = (u8)val;
                }
            for (int i = 0; i < 64; i++) if (fast[i] != whole[i]) same = 0;
            blocks++;
        }
        okn("the transform without its zero terms is the whole formula, byte for byte",
            same && blocks == 400, blocks);
    }


    puts(failed ? "JPEGTEST_FAIL\n" : "JPEGTEST_PASS\n");
    return failed;
}
