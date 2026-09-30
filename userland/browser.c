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
#include "sites.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "jsdom.h"
#include "png.h"
#include "jpeg.h"
#include "gif.h"
#include "webp.h"
#include "svg.h"

/* --- how much room there is ----------------------------------------------
 *
 * These are fixed rather than grown. There is an allocator now — the
 * JavaScript engine below uses it, in quarter megabyte chunks — but the
 * buffers a page is read into are decided once and reused for every page,
 * because a browser that allocated per page would be a browser whose
 * failure to show one depended on which one it showed before. They are
 * sized for a real page: a few hundred kilobytes of source, a handful of
 * style sheets, a few thousand words on the screen.
 *
 * "A real page" was measured rather than guessed, and it is larger than it
 * sounds. An encyclopaedia article is six hundred and seventy kilobytes of
 * html; an ordinary article on an ordinary site is two hundred and forty.
 * At three hundred and twenty the first of those did not fit, and what it
 * looked like was not a page cut short -- it was "the fetch failed",
 * because the page arrives compressed and a decompression that runs out of
 * room fails whole. So: a megabyte, and a decompression that runs out of
 * room now keeps what it has. Then three, and now eight: a streaming
 * service's front page is 3.2 megabytes of html once unpacked, most of it
 * the data its script draws from, and the buffer is mapped, so a page pays
 * only for what it uses (the comment above `src`). */
#define SRC_MAX    (8 * 1024 * 1024)
#define DOC_ARENA  (2 * SRC_MAX)
#define CSS_MAX    (1024 * 1024)
/* Style sheets a page may link. A site built in pieces links one per
   piece: The Verge links sixty-six, and the rule that keeps its menu drawer
   shut until it is opened was in one past the fortieth, so the drawer and
   the logo inside it were drawn across the page. */
#define SHEETS_MAX 96

/* A script the page did not bring with it. One buffer, reused: each is run
   the moment it arrives, so there is never more than one in hand. The limit
   on how many are followed is the same argument as for sheets -- every one
   is another round trip -- and it is said out loud when it is reached.
 *
 * It was 128 kilobytes and eight files, and a site's bundle is not that: The
 * Verge's two largest are 940 and 890 kilobytes, Instagram's is 3.9
 * megabytes, and fifty files is ordinary. A script cut off at the limit was
 * run anyway and stopped on a syntax error in its middle, which is what
 * Microsoft, Instagram, Yahoo, ESPN, IMDb and Spotify reported. Now the buffer
 * is mapped, so a page pays for the size of what it fetched, and one that
 * does not fit is not run at all. What running one costs is many times its
 * size again, in the engine's tree of it, which lives as long as the page:
 * whether the machine has that is asked before each one runs (jsdom.h,
 * jd_room_for), which on a 64 megabyte machine is about a quarter of a
 * megabyte of script and on a 256 megabyte one several; SCRIPTS_BYTES is a
 * ceiling on what a page's files come to together whatever the machine.
 *
 * A page of modules is many small files rather than a few large ones:
 * GitHub's home page asked for more than 64, and more than 6 megabytes of
 * them together, and the rest would not come. So a page may have 256 files
 * and 16 megabytes; whether the machine can run each one is still asked
 * before it runs (jd_room_for). */
#define SCRIPT_MAX    (4 * 1024 * 1024)
#define SCRIPTS_MAX   256
#define SCRIPTS_BYTES (16 * 1024 * 1024)

/* And what a script asks for while the page is up. Its own buffer, because
   a reply arriving must not write over the text of the script that asked
   for it -- which is exactly what sharing one would do. Mapped, and as large
   as a script file, because what a page's fetch asks for is as often a
   megabyte of JSON as a line of text, and a reply cut short is JSON that
   will not parse. */
#define REPLY_MAX   (4 * 1024 * 1024)

/* How many a page may make. A page in a loop asking forever is a page
   that holds the machine on the network rather than on the processor,
   and neither is somewhere to leave it. */
#define ASKS_MAX    64
#define HIST_MAX   40

/* The five big things -- the page's source, a sheet's, the tree, the style
   sheet and the laid out page, twenty megabytes between them -- are mapped
   when the browser starts rather than declared, because a declared array
   is memory the loader hands over whole before the first instruction, and a
   mapping is paid for a page at a time as it is used (map, sdk/zelr.h). A
   small page touches a small part of each. Declared, they grew past what
   a 64 megabyte machine had left with a terminal and a calculator open,
   and the browser did not start at all. */
static char *src;
static char *cssbuf;
static char *scriptbuf;              /* mapped, SCRIPT_MAX */
static char *replybuf;               /* mapped, REPLY_MAX */
static int  scripts_outside;
static int  scripts_bytes;           /* what they came to, for SCRIPTS_BYTES */
static int  asks_made;

static ddoc   *doc_mem;
static csheet *sheet_mem;
static cindex index_;
static ldoc   *page_mem;
#define doc   (*doc_mem)
#define sheet (*sheet_mem)
#define page  (*page_mem)
static cmatch match;
static cinline inl[DOM_NODES];

typedef struct { char text[URL_TEXT]; int scroll; } hist_t;
static hist_t hist[HIST_MAX];
static int    hist_n, hist_at = -1;

static url_t    here;
static response_t reply;

static char  title[160];
static char  status[URL_TEXT + 96];
static char  address[URL_TEXT];       /* what the bar says; the window's, below */
static int   scroll;
static int   over_link = -1;
static int   hover_node = -1;
static int   hover_reach[16], hover_n;   /* what :hover reaches there (css_hover_reach) */

/* The size everything relative is relative to. A page that says 1.2em means
   twenty per cent more than this, and a page that says nothing gets it. */
static int   root_px = 16;

/* --- the pictures -------------------------------------------------------
 *
 * Fetched after the page is parsed and before it is laid out, because the
 * layout has to know how big each one is to leave room for it. One
 * connection at a time is all this kernel's TCP does, so they arrive one
 * after another and a page of many pictures is slow — visibly so, and
 * honestly so, rather than appearing to hang.
 *
 * A picture that will not decode is not an error: the element falls back to
 * its alt text, which is what that text is for. Only the count of what was
 * skipped is worth saying.
 */
#define PICS_MAX 48

typedef struct {
    int     node;                /* which img element */
    picture pic;                 /* its pixels, or nothing: colour times alpha */
    u8     *alpha;               /* how much of it covers what is behind, or null
                                    for a picture with no clear parts (pic_merge) */
} shown;

static shown  pics[PICS_MAX];
static int    npics;
static limage pic_sizes[PICS_MAX];
static int    npic_sizes;
static int    pics_skipped;

/* Drawings written into the page, <svg> in the markup: made from the page's
 * own tree the first time each is shown, at the size the layout gave it and
 * on the backdrop behind it, in its colour (layout.h, lay_drawing; svg.h,
 * svg_render_tree). Kept until the page is laid out again. One that draws
 * nothing is kept as nothing, so it is not tried again every frame. */
#define DRAWINGS_MAX 256
#define DRAWINGS_BYTES (12 * 1024 * 1024)

typedef struct {
    int node, w, h;
    u32 bg, ink;
    picture pic;
} drawn;

static drawn drawings[DRAWINGS_MAX];
static int   ndrawings, drawings_bytes;

static void drawings_drop(void) {
    for (int i = 0; i < ndrawings; i++) picture_free(&drawings[i].pic);
    ndrawings = 0;
    drawings_bytes = 0;
}

static const picture *drawing_of(const litem *it) {
    for (int i = 0; i < ndrawings; i++) {
        const drawn *dr = &drawings[i];
        if (dr->node == it->node && dr->w == it->w && dr->h == it->h
            && dr->bg == it->bg && dr->ink == it->color)
            return dr->pic.rgb ? &dr->pic : 0;
    }
    int bytes = it->w * it->h * 3;
    if (ndrawings >= DRAWINGS_MAX || drawings_bytes + bytes > DRAWINGS_BYTES) return 0;
    drawn *dr = &drawings[ndrawings++];
    dr->node = it->node;
    dr->w = it->w;
    dr->h = it->h;
    dr->bg = it->bg;
    dr->ink = it->color;
    svg_render_tree(&doc, it->node, it->w, it->h, &dr->pic, it->bg, it->color);
    if (dr->pic.rgb) drawings_bytes += bytes;
    return dr->pic.rgb ? &dr->pic : 0;
}

/* --- background pictures ------------------------------------------------------
 *
 * A box's background-image, or the mask its colour is painted through
 * (layout.h, lbg): fetched once the page is laid out, since they take no
 * room, and drawn under the box's contents (draw_background). Each is decoded
 * twice, over black and over white, and what differs between the two is how
 * much of whatever is behind shows through: kept as the colour already
 * multiplied by that and the amount itself, a transparent icon lands on what
 * is behind it rather than on a white square. A drawing is kept as its markup
 * and made at the size it is drawn at, the last size kept. One that will not
 * decode is kept as a failure, so it is not fetched again every frame. */
#define BGPICS_MAX 32
#define BGPICS_BYTES (16 * 1024 * 1024)

typedef struct {
    u32 key;
    int keylen;                  /* the url as the page wrote it, hashed */
    int ok;
    int w, h;                    /* the picture's size, or the drawing's own */
    u8 *rgb, *a;                 /* colour times alpha, and alpha (null when opaque) */
    char *svg;                   /* a drawing's markup */
    int svglen, dw, dh;          /* and the size its pixels were made at */
} bgpic;

static bgpic bgpics[BGPICS_MAX];
static int nbgpics, bgpics_bytes;

static u32 bg_hash(const char *s, int *len) {
    u32 h = 2166136261u;
    int n = 0;
    for (; s[n]; n++) h = (h ^ (u8)s[n]) * 16777619u;
    *len = n;
    return h;
}

static bgpic *bg_find(const char *url) {
    int len;
    u32 k = bg_hash(url, &len);
    for (int i = 0; i < nbgpics; i++)
        if (bgpics[i].key == k && bgpics[i].keylen == len) return &bgpics[i];
    return 0;
}

static void bg_pixels_free(bgpic *b) {
    if (b->rgb) free(b->rgb);
    if (b->a) free(b->a);
    b->rgb = b->a = 0;
}

static void bgpics_drop(void) {
    for (int i = 0; i < nbgpics; i++) {
        bg_pixels_free(&bgpics[i]);
        if (bgpics[i].svg) free(bgpics[i].svg);
        bgpics[i].svg = 0;
    }
    nbgpics = 0;
    bgpics_bytes = 0;
}

/* The same picture over black and over white, made into colour times alpha
   (left in black) and alpha, how much of what is behind it shows: null when
   none does. What differs between the two is exactly what shows through.
   Takes white's pixels; on failure, black's too. */
static int pic_merge(picture *black, picture *white, u8 **alpha) {
    *alpha = 0;
    int ok = black->rgb && white->rgb && black->w == white->w && black->h == white->h;
    int n = ok ? black->w * black->h : 0;
    u8 *a = ok ? (u8 *)malloc((u32)n) : 0;
    if (!a) {
        picture_free(black);
        picture_free(white);
        return 0;
    }
    int opaque = 1;
    for (int i = 0; i < n; i++) {
        int al = 255 - ((int)white->rgb[i * 3 + 1] - (int)black->rgb[i * 3 + 1]);
        if (al < 0) al = 0;
        if (al > 255) al = 255;
        a[i] = (u8)al;
        if (al != 255) opaque = 0;
    }
    picture_free(white);
    if (opaque) free(a);
    else *alpha = a;
    return 1;
}

static int bg_merge(picture *black, picture *white, bgpic *b) {
    u8 *a;
    if (!pic_merge(black, white, &a)) return 0;
    b->rgb = black->rgb;
    black->rgb = 0;
    b->a = a;
    b->w = b->dw = black->w;
    b->h = b->dh = black->h;
    return 1;
}

/* Whether a picture's file says it has clear parts, so that only those are
   decoded the second time pic_merge needs. */
static int pic_may_be_clear(const u8 *b, int n) {
    if (n > 26 && b[0] == 137 && b[1] == 'P') {
        if (b[25] == 4 || b[25] == 6) return 1;            /* grey or colour with alpha */
        for (int i = 8; i + 8 < n; ) {                      /* or a tRNS before the pixels */
            u32 len = ((u32)b[i] << 24) | ((u32)b[i + 1] << 16) | ((u32)b[i + 2] << 8) | b[i + 3];
            if (b[i + 4] == 't' && b[i + 5] == 'R' && b[i + 6] == 'N' && b[i + 7] == 'S') return 1;
            if (b[i + 4] == 'I' && b[i + 5] == 'D' && b[i + 6] == 'A' && b[i + 7] == 'T') return 0;
            if (len > (u32)n) return 0;
            i += 12 + (int)len;
        }
        return 0;
    }
    if (n > 25 && b[0] == 'R' && b[8] == 'W') {
        if (b[12] == 'V' && b[13] == 'P' && b[14] == '8' && b[15] == 'X') return (b[20] & 0x10) != 0;
        if (b[12] == 'V' && b[13] == 'P' && b[14] == '8' && b[15] == 'L') {
            u32 bits = (u32)b[21] | ((u32)b[22] << 8) | ((u32)b[23] << 16) | ((u32)b[24] << 24);
            return (bits >> 28) & 1;
        }
        return 0;
    }
    return 1;                                           /* a GIF or a drawing: see */
}

/* A drawing's pixels at w by h, made now if they were made at another size. */
static int bg_svg_at(bgpic *b, int w, int h) {
    if (b->rgb && b->dw == w && b->dh == h) return 1;
    if ((long long)w * h > 1024 * 1024) return 0;
    int had = b->rgb ? b->dw * b->dh * 4 : 0;
    bg_pixels_free(b);
    bgpics_bytes -= had;
    if (bgpics_bytes + w * h * 4 > BGPICS_BYTES) return 0;
    picture k, wh;
    int ww = b->w, hh = b->h;
    b->ok = 0;                                  /* until it is made */
    if (svg_render(b->svg, b->svglen, w, h, &k, 0x000000) != SVG_OK) return 0;
    if (svg_render(b->svg, b->svglen, w, h, &wh, 0xFFFFFF) != SVG_OK) { picture_free(&k); return 0; }
    if (!bg_merge(&k, &wh, b)) return 0;
    b->ok = 1;
    b->dw = b->w;
    b->dh = b->h;
    b->w = ww;
    b->h = hh;
    bgpics_bytes += b->dw * b->dh * 4;
    return 1;
}

/* One picture over a backdrop, by what its bytes say it is. */
static int bg_raster(const u8 *body, int len, u32 bg, picture *out) {
    if (len > 8 && body[0] == 137 && body[1] == 'P' && body[2] == 'N' && body[3] == 'G')
        return png_decode(body, len, out, bg) == PNG_OK;
    if (len > 6 && body[0] == 'G' && body[1] == 'I' && body[2] == 'F')
        return gif_decode(body, len, out, bg) == GIF_OK;
    if (len > 12 && body[0] == 'R' && body[1] == 'I' && body[2] == 'F' && body[3] == 'F'
        && body[8] == 'W' && body[9] == 'E' && body[10] == 'B' && body[11] == 'P')
        return webp_decode(body, len, out, bg) == WEBP_OK;
    return 0;
}

static int pic_data_uri(const char *src, char *out, int cap);

static void gather_backgrounds(void) {
    for (int i = 0; i < page.nitems && nbgpics < BGPICS_MAX; i++) {
        const litem *it = &page.items[i];
        if (it->kind != LK_BOX || it->bgi < 0 || it->w <= 0 || it->h <= 0) continue;
        if (page.bgs[it->bgi].url < 0) continue;              /* a gradient alone */
        const char *src = page.text + page.bgs[it->bgi].url;
        if (bg_find(src)) continue;
        bgpic *b = &bgpics[nbgpics++];
        b->key = bg_hash(src, &b->keylen);
        b->ok = 0;
        b->w = b->h = b->dw = b->dh = 0;
        b->rgb = b->a = 0;
        b->svg = 0;
        b->svglen = 0;

        response_t r;
        if (w_starts_fold(src, "data:")) {
            int dn = pic_data_uri(src, cssbuf, CSS_MAX);
            if (dn <= 0) continue;
            r.body = cssbuf;
            r.len = dn;
        } else {
            url_t u;
            if (!url_join(&here, src, &u)) continue;
            web_accept = "image/webp,image/png,image/jpeg,image/gif,image/svg+xml;q=0.9,*/*;q=0.1";
            int rc = web_get(&u, cssbuf, CSS_MAX, &r);
            web_accept = 0;
            if (rc < 0 || rc >= 400 || r.len <= 0) continue;
        }
        const u8 *body = (const u8 *)r.body;
        if (r.len > 3 && body[0] == 0xFF && body[1] == 0xD8) {
            picture p;
            if (jpeg_decode(body, r.len, &p) != JPG_OK) continue;
            b->rgb = p.rgb;
            b->w = b->dw = p.w;
            b->h = b->dh = p.h;
            b->ok = 1;
        } else {
            picture k, wh;
            if (bg_raster(body, r.len, 0x000000, &k)) {
                if (!bg_raster(body, r.len, 0xFFFFFF, &wh)) { picture_free(&k); continue; }
                b->ok = bg_merge(&k, &wh, b);
            } else {
                int at = 0;
                while (at < r.len && (body[at] == ' ' || body[at] == '\n' || body[at] == '\r'
                                      || body[at] == '\t')) at++;
                if (at >= r.len || body[at] != '<') continue;
                b->svg = (char *)malloc((u32)r.len + 1);
                if (!b->svg) continue;
                for (int c = 0; c < r.len; c++) b->svg[c] = r.body[c];
                b->svg[r.len] = 0;
                b->svglen = r.len;
                /* Made once at its own size, which is its size. */
                picture own;
                if (svg_render(b->svg, b->svglen, 0, 0, &own, 0xFFFFFF) != SVG_OK) continue;
                b->w = own.w;
                b->h = own.h;
                picture_free(&own);
                b->ok = b->w > 0 && b->h > 0;
            }
        }
        if (b->ok && !b->svg) {
            bgpics_bytes += b->dw * b->dh * 4;
            if (bgpics_bytes > BGPICS_BYTES) { bg_pixels_free(b); b->ok = 0; }
        }
    }
}

/* Sine of a whole number of degrees, in 65536ths: enough to point a
   gradient, with no library under this program to ask. */
static int sin65536(int deg) {
    static const int q[91] = {
        0, 1144, 2287, 3430, 4572, 5712, 6850, 7987, 9121, 10252, 11380, 12505, 13626, 14742,
        15855, 16962, 18064, 19161, 20252, 21336, 22415, 23486, 24550, 25607, 26656, 27697,
        28729, 29753, 30767, 31772, 32768, 33754, 34729, 35693, 36647, 37590, 38521, 39441,
        40348, 41243, 42126, 42995, 43852, 44695, 45525, 46341, 47143, 47930, 48703, 49461,
        50203, 50931, 51643, 52339, 53020, 53684, 54332, 54963, 55578, 56175, 56756, 57319,
        57865, 58393, 58903, 59396, 59870, 60326, 60764, 61183, 61584, 61966, 62328, 62672,
        62997, 63303, 63589, 63856, 64104, 64332, 64540, 64729, 64898, 65048, 65177, 65287,
        65376, 65446, 65496, 65526, 65536 };
    deg = ((deg % 360) + 360) % 360;
    if (deg <= 90) return q[deg];
    if (deg <= 180) return q[180 - deg];
    if (deg <= 270) return -q[deg - 180];
    return -q[360 - deg];
}

/* A linear gradient across a box: each pixel's place along the gradient's
   line (as long as the box is across in that direction, as CSS has it)
   picks a colour between the stops round it, colour and alpha interpolated
   together, laid over what is there. */
static void draw_gradient(surface *s, const litem *it, const lbg *g, int x, int sy, int ox, int oy, int vw, int vh) {
    int bw = it->w, bh = it->h;
    int sn = sin65536(g->angle), cs = sin65536(g->angle + 90);
    long long len = ((long long)bw * (sn < 0 ? -sn : sn) + (long long)bh * (cs < 0 ? -cs : cs)) >> 16;
    if (len < 1) len = 1;
    int n = g->nstops;
    for (int row = 0; row < bh; row++) {
        int dy = sy + row;
        if (dy < oy || dy >= oy + vh || dy < 0 || dy >= s->h) continue;
        long long yy = 2 * row + 1 - bh;              /* twice, from the centre */
        for (int col = 0; col < bw; col++) {
            int dx = x + col;
            if (dx < ox || dx >= ox + vw || dx < 0 || dx >= s->w) continue;
            long long xx = 2 * col + 1 - bw;
            /* along the direction, 0 at the start and 10000 at the end */
            long long along = (xx * sn - yy * cs) >> 16;      /* twice the distance */
            long long t = 5000 + along * 5000 / len;
            if (t < 0) t = 0;
            if (t > 10000) t = 10000;
            int k = 0;
            while (k < n - 1 && t > g->stop_p[k + 1] * 100) k++;
            int p0 = g->stop_p[k] * 100, p1 = k < n - 1 ? g->stop_p[k + 1] * 100 : p0;
            int f = p1 > p0 ? (int)((t - p0) * 256 / (p1 - p0)) : 0;
            if (f < 0) f = 0;
            if (f > 256) f = 256;
            int k1 = k < n - 1 ? k + 1 : k;
            u32 c0 = g->stop_c[k], c1 = g->stop_c[k1];
            int a0 = g->stop_a[k], a1 = g->stop_a[k1];
            int al = (a0 * (256 - f) + a1 * f) >> 8;
            if (!al) continue;
            /* premultiplied, so a stop that is clear adds no colour of its own */
            int pr = ((int)((c0 >> 16) & 255) * a0 * (256 - f) + (int)((c1 >> 16) & 255) * a1 * f) >> 8;
            int pg = ((int)((c0 >> 8) & 255) * a0 * (256 - f) + (int)((c1 >> 8) & 255) * a1 * f) >> 8;
            int pb = ((int)(c0 & 255) * a0 * (256 - f) + (int)(c1 & 255) * a1 * f) >> 8;
            u32 *d = &s->px[(u32)dy * s->w + dx];
            int back = 255 - al;
            int r2 = (pr + (int)((*d >> 16) & 255) * back) / 255;
            int g2 = (pg + (int)((*d >> 8) & 255) * back) / 255;
            int b2 = (pb + (int)(*d & 255) * back) / 255;
            *d = ((u32)(r2 > 255 ? 255 : r2) << 16) | ((u32)(g2 > 255 ? 255 : g2) << 8)
               | (u32)(b2 > 255 ? 255 : b2);
        }
    }
}

/* A box's background picture or mask, inside the box and the view: sized,
   placed and repeated as its sheet said. */
static void draw_picture_layer(surface *s, const litem *it, int x, int sy, int ox, int oy, int vw, int vh);

static void draw_background(surface *s, const litem *it, int x, int sy, int ox, int oy, int vw, int vh) {
    const lbg *g = &page.bgs[it->bgi];
    if (g->nstops && g->under) draw_gradient(s, it, g, x, sy, ox, oy, vw, vh);
    if (g->url >= 0) draw_picture_layer(s, it, x, sy, ox, oy, vw, vh);
    if (g->nstops && !g->under) draw_gradient(s, it, g, x, sy, ox, oy, vw, vh);
}

static void draw_picture_layer(surface *s, const litem *it, int x, int sy, int ox, int oy, int vw, int vh) {
    const lbg *g = &page.bgs[it->bgi];
    bgpic *b = bg_find(page.text + g->url);
    if (!b || !b->ok || b->w <= 0 || b->h <= 0) return;
    int bw = it->w, bh = it->h, nw = b->w, nh = b->h, tw, th;
    if (g->fit) {
        long long fx = (long long)bw * 65536 / nw, fy = (long long)bh * 65536 / nh;
        long long f = g->fit == 1 ? (fx > fy ? fx : fy) : (fx < fy ? fx : fy);
        tw = (int)((nw * f) >> 16);
        th = (int)((nh * f) >> 16);
    } else {
        int sw = g->sw < 0 ? -1 : g->swp ? bw * g->sw / 100 : g->sw;
        int sh = g->sh < 0 ? -1 : g->shp ? bh * g->sh / 100 : g->sh;
        if (sw < 0 && sh < 0) { tw = nw; th = nh; }
        else if (sh < 0) { tw = sw; th = (int)((long long)nh * sw / nw); }
        else if (sw < 0) { th = sh; tw = (int)((long long)nw * sh / nh); }
        else { tw = sw; th = sh; }
    }
    if (tw < 1 || th < 1) return;
    if (b->svg && !bg_svg_at(b, tw, th)) return;
    if (!b->rgb) return;
    int pw = b->dw, ph = b->dh;
    int x0 = g->ppx ? (bw - tw) * g->px / 100 : g->px;
    int y0 = g->ppy ? (bh - th) * g->py / 100 : g->py;
    int across = g->rep == 0 || g->rep == 2, down = g->rep == 0 || g->rep == 3;
    u32 c = it->bg;
    int cr = (int)((c >> 16) & 255), cg = (int)((c >> 8) & 255), cb = (int)(c & 255);
    for (int row = 0; row < bh; row++) {
        int dy = sy + row;
        if (dy < oy || dy >= oy + vh || dy < 0 || dy >= s->h) continue;
        int ty = row - y0;
        if (down) { ty %= th; if (ty < 0) ty += th; }
        else if (ty < 0 || ty >= th) continue;
        int src_y = ty * ph / th;
        for (int col = 0; col < bw; col++) {
            int dx = x + col;
            if (dx < ox || dx >= ox + vw || dx < 0 || dx >= s->w) continue;
            int tx = col - x0;
            if (across) { tx %= tw; if (tx < 0) tx += tw; }
            else if (tx < 0 || tx >= tw) continue;
            int at = src_y * pw + tx * pw / tw;
            int al = b->a ? b->a[at] : 255;
            if (!al) continue;
            u32 *d = &s->px[(u32)dy * s->w + dx];
            int dr = (int)((*d >> 16) & 255), dgn = (int)((*d >> 8) & 255), db = (int)(*d & 255);
            int r2, g2, b2;
            if (g->mask) {
                r2 = (cr * al + dr * (255 - al)) / 255;
                g2 = (cg * al + dgn * (255 - al)) / 255;
                b2 = (cb * al + db * (255 - al)) / 255;
            } else {
                const u8 *q = b->rgb + at * 3;
                r2 = q[0] + dr * (255 - al) / 255;
                g2 = q[1] + dgn * (255 - al) / 255;
                b2 = q[2] + db * (255 - al) / 255;
                if (r2 > 255) r2 = 255;
                if (g2 > 255) g2 = 255;
                if (b2 > 255) b2 = 255;
            }
            *d = ((u32)r2 << 16) | ((u32)g2 << 8) | (u32)b2;
        }
    }
}

static void pics_drop(void) {
    for (int i = 0; i < npics; i++) {
        picture_free(&pics[i].pic);
        if (pics[i].alpha) free(pics[i].alpha);
        pics[i].alpha = 0;
    }
    npics = 0;
    npic_sizes = 0;
    pics_skipped = 0;
    drawings_drop();
    bgpics_drop();
}

static const picture *pic_of(int node) {
    for (int i = 0; i < npics; i++)
        if (pics[i].node == node && pics[i].pic.rgb) return &pics[i].pic;
    return 0;
}

static const u8 *pic_alpha_of(int node) {
    for (int i = 0; i < npics; i++)
        if (pics[i].node == node && pics[i].pic.rgb) return pics[i].alpha;
    return 0;
}

/* --- what is being typed into ---------------------------------------------
 *
 * A control holds its value in the document, as the attribute a page would
 * have written it in, so that there is one answer to what is in a field and
 * everything reads it from the same place: the layout sizes the box, the
 * drawing writes the value into it, a script that asks gets what is on the
 * screen, and submitting sends exactly what can be seen.
 *
 * Keeping it anywhere else means two answers that agree until somebody
 * types.
 */
static int browser_win = -1;

static int focus_node = -1;

/* The same editor the address bar uses, pointed at whichever control has the
   keyboard. One of them, rather than one per field: only one can be typed
   into, and the value is written back to the document on every keystroke, so
   there is nothing to keep for the others. */
static char     focus_buf[1024];
static ui_field focus_field = { focus_buf, sizeof(focus_buf), 0, 0, 0 };

static int field_checked(int el) {
    const char *v = dom_attr(&doc, el, "checked");
    return v && !w_same(v, "0");
}

/* A checkbox written with a bare `checked` has an empty value, which is not
   the same as not being there; unchecking one has to leave something
   behind, so it leaves a nought. */
static void field_set_checked(int el, int on) {
    dom_attr_set(&doc, el, "checked", on ? "1" : "0");
}

static const char *field_value(int el, int kind) {
    return lay_control_label(&doc, el, kind);
}

static void field_set_value(int el, const char *v) {
    dom_attr_set(&doc, el, "value", v);
}

/* The form an element is in, or -1. Walked up rather than looked up: a page
   may name a form anywhere and nest one nowhere, and the enclosing element
   is what the markup actually says. */
static int form_of(int el) {
    while (el >= 0) {
        if (doc.nodes[el].kind == DN_ELEMENT && doc.nodes[el].tag == T_FORM)
            return el;
        el = doc.nodes[el].parent;
    }
    return -1;
}

static void focus_control(int el) {
    focus_node = el;
    focus_field.focused = 0;
    focus_buf[0] = 0;
    focus_field.len = 0;
    focus_field.cursor = 0;
    if (el < 0) return;

    int ck = lay_control_kind(&doc, el);
    if (ck != CTL_TEXT && ck != CTL_PASSWORD && ck != CTL_AREA) return;

    w_copy(focus_buf, sizeof(focus_buf), field_value(el, ck),
           sizeof(focus_buf));
    focus_field.len = w_len(focus_buf);
    focus_field.cursor = focus_field.len;
    focus_field.focused = 1;
}

/* --- being searchable ------------------------------------------------------
 *
 * A window is a rectangle of pixels, and there is no text in a picture of
 * text, so the desktop cannot read a page off the screen. What it can read
 * is what the program says it is showing, and this says it.
 *
 * What is published is the words of the laid out page rather than the
 * source: what a reader can see is what they mean when they look for it,
 * and the source is full of markup and script that nobody is looking at.
 *
 * The match is found again here rather than being carried back, because the
 * desktop counts matches in a string and what this needs is the line of the
 * page it fell on. Counting twice over the same text in the same order
 * gives the same answer, which is the only thing the two sides have to
 * agree about.
 */
static int find_item = -1;        /* the run holding the match, or -1 */

static char fold_ch(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static void publish_text(void) {
    static char buf[4096];
    int n = 0;
    for (int i = 0; i < page.nitems && n < (int)sizeof(buf) - 2; i++) {
        const litem *it = &page.items[i];
        if (it->kind != LK_TEXT || it->at < 0) continue;
        for (const char *t = page.text + it->at;
             *t && n < (int)sizeof(buf) - 2; t++)
            buf[n++] = *t;
        buf[n++] = ' ';
    }
    win_set_text(browser_win, buf, n);
}

/* The nth match, scrolled to and remembered so it can be drawn lit. */
static void find_show(int which, int view_h) {
    find_item = -1;

    char q[64];
    int qn = win_find_query(q, sizeof(q));
    if (qn <= 0) return;

    int seen = 0;
    for (int i = 0; i < page.nitems; i++) {
        const litem *it = &page.items[i];
        if (it->kind != LK_TEXT || it->at < 0) continue;
        const char *t = page.text + it->at;
        for (int k = 0; t[k]; k++) {
            int j = 0;
            while (j < qn && t[k + j] && fold_ch(t[k + j]) == fold_ch(q[j])) j++;
            if (j < qn) continue;
            if (seen == which) {
                find_item = i;
                scroll = it->y - view_h / 3;
                if (scroll < 0) scroll = 0;
                return;
            }
            seen++;
            k += j - 1;
        }
    }
}

/* --- sending a form -------------------------------------------------------
 *
 * Everything a form holds, named and escaped, in document order, which is
 * the order a server is entitled to expect.
 *
 * A control with no name sends nothing, which is how a page marks the boxes
 * that are for the reader rather than for the server. A checkbox that is
 * not ticked sends nothing at all rather than sending "off": the absence is
 * the message, and a server reading a ticked box and an unticked one as two
 * different values of the same key is the one thing this must not do.
 */
static char go_to[URL_TEXT];
static int  load_post;

/* --- a page that asks to be replaced --------------------------------------
 *
 * <meta http-equiv="refresh" content="0;url=..."> is markup rather than
 * script, and it is how a great many sites send a browser somewhere else --
 * including, as it turns out, google when it does not think it is talking
 * to a browser at all. A page that carries one and is not followed is a
 * page that sits there saying "please click here if you are not
 * redirected", which is the sentence somebody writes for exactly this.
 *
 * Bounded, and not to itself. A page that refreshes to its own address is a
 * loop, and every browser that has ever existed has had to stop one.
 */
#define REFRESH_MAX 3
static int refreshes;
static int go_is_refresh;      /* this load was the page's idea, not a reader's */

/* Whether an element is inside <noscript>, which is for a browser that does
   not run scripts. This one does: Google's search page says there to go to
   a page asking for scripts to be turned on, and following it left the page
   the page's own script was about to work on. */
static int in_noscript(int el) {
    for (int p = doc.nodes[el].parent; p >= 0; p = doc.nodes[p].parent)
        if (doc.nodes[p].kind == DN_ELEMENT && doc.nodes[p].tag == T_NOSCRIPT) return 1;
    return 0;
}

static int meta_refresh(char *out, int cap) {
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT) continue;
        if (doc.nodes[i].tag != T_META) continue;
        if (in_noscript(i)) continue;

        const char *eq = dom_attr(&doc, i, "http-equiv");
        if (!eq || !lay_same_fold(eq, "refresh")) continue;

        const char *c = dom_attr(&doc, i, "content");
        if (!c) continue;

        /* "5" alone is this page again after five seconds, which is a thing
           status boards do and not something to follow. Only one that names
           somewhere else is worth acting on. */
        int k = 0;
        while (c[k] && c[k] != ';') k++;
        if (!c[k]) continue;
        k++;
        while (c[k] == ' ') k++;
        if (!(c[k] == 'u' || c[k] == 'U')) continue;
        while (c[k] && c[k] != '=') k++;
        if (!c[k]) continue;
        k++;
        while (c[k] == ' ' || c[k] == '"' || c[k] == '\'') k++;

        int w = 0;
        while (c[k] && c[k] != '"' && c[k] != '\'' && w < cap - 1)
            out[w++] = c[k++];
        out[w] = 0;
        return w > 0;
    }
    return 0;
}
static char post_body[4096];
static int  want_go;
static int  go_is_post;

static void url_encode_into(char *out, int cap, int *at, const char *v) {
    static const char *hex = "0123456789ABCDEF";
    for (const char *q = v; *q && *at < cap - 4; q++) {
        unsigned char c = (unsigned char)*q;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~')
            out[(*at)++] = (char)c;
        else if (c == ' ')
            out[(*at)++] = '+';
        else {
            out[(*at)++] = '%';
            out[(*at)++] = hex[c >> 4];
            out[(*at)++] = hex[c & 15];
        }
    }
    out[*at] = 0;
}

static int form_query(int form, char *out, int cap) {
    int at = 0;
    out[0] = 0;
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT) continue;

        int ck = lay_control_kind(&doc, i);
        if (ck == CTL_NONE || ck == CTL_BUTTON) continue;
        if (form_of(i) != form) continue;

        const char *name = dom_attr(&doc, i, "name");
        if (!name || !*name) continue;
        if ((ck == CTL_CHECK || ck == CTL_RADIO) && !field_checked(i)) continue;

        /* Copied out before anything else is read: the value of a textarea
           comes back in a buffer that the next read of any label reuses. */
        char keep[1024];
        if (ck == CTL_CHECK || ck == CTL_RADIO) {
            const char *vv = dom_attr(&doc, i, "value");
            w_copy(keep, sizeof(keep), vv && *vv ? vv : "on", sizeof(keep));
        } else {
            w_copy(keep, sizeof(keep), field_value(i, ck), sizeof(keep));
        }

        if (at && at < cap - 1) out[at++] = '&';
        url_encode_into(out, cap, &at, name);
        if (at < cap - 1) out[at++] = '=';
        url_encode_into(out, cap, &at, keep);
    }
    out[at] = 0;
    return at;
}

static void submit_form(int form) {
    if (form < 0) return;

    char query[sizeof(post_body)];
    form_query(form, query, (int)sizeof(query));

    const char *action = dom_attr(&doc, form, "action");
    const char *method = dom_attr(&doc, form, "method");
    int post = method && lay_same_fold(method, "post");

    /* A form with no action goes back to the page it is on, which is what
       the specification says and what a search box on a site relies on. */
    url_t target;
    if (action && *action) {
        if (!url_join(&here, action, &target)) return;
    } else {
        url_copy(&target, &here);
    }

    url_text(&target, go_to, sizeof(go_to));

    /* Whatever query the current address had is the previous answer's, not
       this form's, and carrying it would send both. */
    for (int i = 0; go_to[i]; i++)
        if (go_to[i] == '?') { go_to[i] = 0; break; }

    go_is_post = post;
    post_body[0] = 0;
    if (post) {
        w_copy(post_body, sizeof(post_body), query, sizeof(post_body));
    } else if (query[0]) {
        int n = w_len(go_to);
        if (n < (int)sizeof(go_to) - 2) {
            go_to[n++] = '?';
            go_to[n] = 0;
            w_copy(go_to + n, (int)sizeof(go_to) - n, query,
                   (int)sizeof(go_to) - n);
        }
    }
    want_go = 1;
}

/* What the page's own scripts did, for the status line to mention. */
static int   scripts_ran;
static int   scripts_changed;      /* one of them wrote to the document */
static char  script_err[128];
/* A handler that threw is worth saying once, and saying it on every click
   after that would bury whatever the status line was for. */
static int   said_script_err;

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
        case WEB_ERR_BUSY:    return "every connection this machine has "
                                     "is in use";
        case WEB_ERR_TLS:     return "the connection would not prove who it was";
        case WEB_ERR_CONNECT: return "could not connect to that host";
        case WEB_ERR_SEND:    return "the request could not be sent";
        case WEB_ERR_EMPTY:   return "the server said nothing";
        case WEB_ERR_HEADERS: return "the answer was not http";
        case WEB_ERR_ENCODING: return "the answer was compressed in a way "
                                      "this cannot undo";
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
static void inline_style_of(int i) {
    {
        inl[i].at = 0;
        inl[i].n = 0;
        if (doc.nodes[i].kind != DN_ELEMENT) return;
        const char *st = dom_attr(&doc, i, "style");
        if (!st || !*st) return;
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

static void gather_inline_styles(void) {
    for (int i = 0; i < doc.count; i++) inline_style_of(i);
}

/* The style attributes a script changed since the page was last laid out,
   read again (jsdom.h, jsdom_next_restyled). They were read once, before
   any script ran, so el.style.display = 'none' changed the attribute and
   nothing on the screen. Each reading adds to the sheet's declarations;
   a page that restyles forever runs them out and its later changes are
   not seen, which the sheet's own overflow already stands for. */
static void restyle_changed(void) {
    for (int i; (i = jsdom_next_restyled()) >= 0; )
        if (i < doc.count) inline_style_of(i);
}

/* What a link or style element's media attribute allows: 0 when the sheet
   is not for this screen at all (print, a dark scheme), else the window
   widths it is for. A sheet for dark mode was read as though it were the
   page's, and a documentation site came out white on black. */
static int sheet_media(int el, int *lo, int *hi) {
    *lo = *hi = -1;
    const char *m = dom_attr(&doc, el, "media");
    if (!m || !*m) return 1;
    return css_mq(m, w_len(m), lo, hi);
}

/* The sheets the page carries itself: every style element, in order. */
/* Whether an element is inside a <template>, which holds markup a script
   may stamp out later and is otherwise not part of the page: not drawn, and
   its style sheets, links and pictures not the page's. GitHub keeps whole
   menus and dialogs in them, style elements included, and those rules were
   applied to the page. */
static int in_template(int el) {
    for (int p = doc.nodes[el].parent; p >= 0; p = doc.nodes[p].parent)
        if (doc.nodes[p].kind == DN_ELEMENT && doc.nodes[p].tag == T_OTHER
            && w_same_fold(dom_tag_name(&doc, p), "template")) return 1;
    return 0;
}

/* Each style and link element the base read, and what it gave (below,
   sheets_follow). */
static void source_add(int node, int r0, int ok);
static int rel_is_sheet(const char *rel);

static void gather_inline_sheets(void) {
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_STYLE)
            continue;
        if (in_template(i)) continue;
        int lo, hi, r0 = sheet.nrules;
        if (!sheet_media(i, &lo, &hi)) { source_add(i, r0, 1); continue; }
        const char *scope = dom_attr(&doc, i, "data-zs");      /* dom_shadows */
        int t = doc.nodes[i].first;
        while (t >= 0) {
            if (doc.nodes[t].kind == DN_TEXT && doc.nodes[t].text >= 0) {
                const char *s = doc.arena + doc.nodes[t].text;
                css_parse_style(&sheet, s, w_len(s), lo, hi, scope);
            }
            t = doc.nodes[t].next;
        }
        source_add(i, r0, 1);
    }
}

/* And the sheets it links to, which on a modern page is nearly all of them.
 *
 * Fetched here rather than skipped, because a page whose entire appearance
 * is in one linked file and which is shown without it is not the page. Each
 * one is another round trip, so there is a limit on how many are followed
 * and the limit is said out loud when it is reached rather than leaving
 * somebody wondering why one part of a page is styled and the rest is not. */
/* A linked sheet's url()s are relative to the sheet, and the layout knows
   only the page: each is written out against the sheet's own address before
   the sheet is read, into out. Its length, or -1 when it will not fit. A
   picture's own data: address and a drawing's #fragment are left alone. */
static int css_urls_from(const url_t *base, const char *in, int n, char *out, int cap) {
    int w = 0, i = 0;
    while (i < n) {
        if (i + 4 <= n && (in[i] == 'u' || in[i] == 'U') && w_lower(in[i + 1]) == 'r'
            && w_lower(in[i + 2]) == 'l' && in[i + 3] == '(' && (i == 0 || !css_ident(in[i - 1]))) {
            for (int k = 0; k < 4; k++) { if (w >= cap - 1) return -1; out[w++] = in[i++]; }
            while (i < n && in[i] == ' ') { if (w >= cap - 1) return -1; out[w++] = in[i++]; }
            char q = 0;
            if (i < n && (in[i] == '"' || in[i] == '\'')) { q = in[i]; if (w >= cap - 1) return -1; out[w++] = in[i++]; }
            int s0 = i;
            while (i < n && (q ? in[i] != q : (in[i] != ')' && in[i] != ' '))) i++;
            int len = i - s0;
            char rel[URL_TEXT], whole[URL_TEXT];
            const char *put = in + s0;
            int plen = len;
            if (len > 0 && len < (int)sizeof(rel) && !w_starts_fold(in + s0, "data:") && in[s0] != '#') {
                for (int k = 0; k < len; k++) rel[k] = in[s0 + k];
                rel[len] = 0;
                url_t u;
                if (url_join(base, rel, &u)) {
                    url_text(&u, whole, (int)sizeof(whole));
                    put = whole;
                    plen = w_len(whole);
                }
            }
            if (w + plen >= cap - 1) return -1;
            for (int k = 0; k < plen; k++) out[w++] = put[k];
            continue;
        }
        if (w >= cap - 1) return -1;
        out[w++] = in[i++];
    }
    out[w] = 0;
    return w;
}

/* A sheet read with its urls made whole (css_urls_from), or as it is when
   there is no room to. */
static char *sheet_abs;
static void css_parse_sheet(const url_t *base, const char *css, int len, int lo, int hi) {
    if (!sheet_abs) sheet_abs = (char *)malloc(CSS_MAX + CSS_MAX / 4);
    int n = sheet_abs ? css_urls_from(base, css, len, sheet_abs, CSS_MAX + CSS_MAX / 4) : -1;
    if (n >= 0) css_parse_in(&sheet, sheet_abs, n, lo, hi);
    else css_parse_in(&sheet, css, len, lo, hi);
}

/* The sheets a sheet imports, fetched and read before it, which is where
   @import puts them in the cascade. One level: an import's own imports are
   not followed. They were skipped, and a site that keeps its whole style in
   one file imported by a small one had none. */
static void gather_imports(const url_t *base, const char *css, int len, int *fetched) {
    int at = 0, lo, hi, got;
    char href[URL_TEXT];
    char *buf = 0;
    while (*fetched < SHEETS_MAX && (got = css_next_import(css, len, &at, href, (int)sizeof(href), &lo, &hi))) {
        if (got < 0) continue;
        url_t u;
        if (!url_join(base, href, &u)) continue;
        if (!buf && !(buf = (char *)malloc(CSS_MAX))) return;
        response_t r;
        int rc = web_get(&u, buf, CSS_MAX, &r);
        if (rc < 200 || rc >= 300 || r.len <= 0) continue;
        css_parse_sheet(&u, r.body, r.len, lo, hi);
        (*fetched)++;
    }
    if (buf) free(buf);
}

/* Whether this address was already read for this page: a page that links
   the same sheet five times (one does) spent five of its slots on it. */
#define SHEETS_SEEN 128
static char sheets_seen[SHEETS_SEEN][URL_TEXT];
static int nsheets_seen;

static int sheet_seen(const char *url) {
    for (int k = 0; k < nsheets_seen; k++) if (w_same(sheets_seen[k], url)) return 1;
    if (nsheets_seen < SHEETS_SEEN) w_copy(sheets_seen[nsheets_seen++], URL_TEXT, url, URL_TEXT);
    return 0;
}

static int gather_linked_sheets(int *fetched, int *skipped) {
    *fetched = *skipped = 0;
    nsheets_seen = 0;
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_LINK)
            continue;
        if (in_template(i)) continue;
        const char *rel = dom_attr(&doc, i, "rel");
        const char *href = dom_attr(&doc, i, "href");
        if (!rel || !href || !*href) continue;

        /* rel can be a list, and "stylesheet alternate" is one this should
           not take: an alternate sheet is one the reader has not chosen. */
        if (!rel_is_sheet(rel)) continue;
        int lo, hi, r0 = sheet.nrules;
        /* One for another medium, or switched off, is fetched only if a
           script makes it one for the screen (sheets_follow). */
        if (!sheet_media(i, &lo, &hi) || dom_attr(&doc, i, "disabled")) { source_add(i, r0, -1); continue; }

        if (*fetched >= SHEETS_MAX) { (*skipped)++; source_add(i, r0, -2); continue; }

        url_t u;
        if (!url_join(&here, href, &u)) { (*skipped)++; source_add(i, r0, 0); continue; }
        char whole[URL_TEXT];
        url_text(&u, whole, (int)sizeof(whole));
        if (sheet_seen(whole)) { source_add(i, r0, 1); continue; }

        response_t r;
        int rc = web_get(&u, cssbuf, CSS_MAX, &r);
        /* Asked for again, once, when the answer was no answer, the server
           said it was busy or it stopped short: one sheet that failed on the
           way is a page drawn with none of its style (Wikipedia's, after a
           burst of fetching). */
        if (rc < 0 || rc == 429 || rc >= 500 || r.cut) rc = web_get(&u, cssbuf, CSS_MAX, &r);
        if (rc < 200 || rc >= 300 || r.len <= 0) { (*skipped)++; source_add(i, r0, 0); continue; }
        gather_imports(&u, r.body, r.len, fetched);
        css_parse_sheet(&u, r.body, r.len, lo, hi);
        (*fetched)++;
        source_add(i, r0, 1);
    }
    return *fetched;
}

/* --- the page's styles as its scripts change them --------------------------------------
 *
 * The sheet is made once, before any script runs (build), and what the
 * scripts did to the page's styles after that never reached it: a style
 * element added or written, a stylesheet link added, a rule put in through
 * the CSSOM (jscssom.h). React pages whose styles emotion or
 * styled-components make in the browser came out with none, and a sheet a
 * page loads the way most do now -- preloaded, made a stylesheet by its
 * onload, or linked for print until its onload says all -- never applied.
 *
 * So what the page's own sheets gave is kept (the base), with each style
 * and link element that gave some and the rules it gave, and whenever the
 * page's styles are no longer what the sheet was made from (styles_print),
 * everything after the base is thrown away and read again: an element that
 * has gone or changed has its base rules switched off, and every style
 * element, link and adopted sheet that is not in the base as it was is read
 * after it, in the page's order. A changed element's rules so come after all
 * the base, where a browser would keep its place among them: which rule wins
 * a tie can differ, which rules there are cannot.
 *
 * A link a script adds is fetched once (links_kept), as is a preloaded sheet
 * and one linked for another medium, and the page is told whether each
 * arrived (jsdom_link_loaded): webpack waits for that before it runs the
 * code the sheet came with. */
typedef struct { int nrules, nsels, ndecls, nnegs, used, nlayers, overflowed; } sheet_mark_t;
static sheet_mark_t base_mark;
static int base_decl_n[CSS_RULES];
#define SOURCES_MAX 512
/* ok: 1 fetched, 0 failed, -1 not asked for (another medium), -2 past the
   limit on how many sheets a page may have fetched, and never to be. */
static struct { int node, r0, r1, ok; unsigned hash; } sources[SOURCES_MAX];
static int nsources;
static unsigned styles_made;          /* styles_print when the sheet was last made */
static unsigned styles_dom_seen, styles_css_seen;   /* the tree's and the sheets' versions then */
static int styles_live;               /* the page has scripts, which can change them */

static unsigned mix_text(unsigned h, const char *s) {
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h * 31u + (s ? 1u : 0u);
}

/* What an element's contribution was read from: a style element's text and
   media, a link's rel, address, medium and whether it is switched off. */
static unsigned source_hash(int i) {
    unsigned h = doc.nodes[i].tag == T_STYLE ? dom_text_hash(&doc, i) : 2166136261u;
    h = mix_text(h, dom_attr(&doc, i, "media"));
    if (doc.nodes[i].tag == T_LINK) {
        h = mix_text(h, dom_attr(&doc, i, "rel"));
        h = mix_text(h, dom_attr(&doc, i, "href"));
        h = mix_text(h, dom_attr(&doc, i, "as"));
        if (dom_attr(&doc, i, "disabled")) h ^= 0x9E3779B9u;
    }
    return h;
}

/* rel="stylesheet", as one word of a list, but not an alternate sheet, which
   is one the reader has not chosen. */
static int rel_is_sheet(const char *rel) {
    int yes = 0, alt = 0;
    for (const char *p = rel; p && *p; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        const char *w = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        int n = (int)(p - w);
        if (n == 10 && w_starts_fold(w, "stylesheet")) yes = 1;
        if (n == 9 && w_starts_fold(w, "alternate")) alt = 1;
    }
    return yes && !alt;
}

static int source_of(int node) {
    for (int k = 0; k < nsources; k++) if (sources[k].node == node) return k;
    return -1;
}

static void source_add(int node, int r0, int ok) {
    if (nsources >= SOURCES_MAX) return;
    sources[nsources].node = node;
    sources[nsources].r0 = r0;
    sources[nsources].r1 = sheet.nrules;
    sources[nsources].ok = ok;
    sources[nsources].hash = source_hash(node);
    nsources++;
}

static int node_in_page(int i) {
    for (int k = 0; i >= 0 && k < DOM_NODES; k++) {
        if (i == doc.root) return 1;
        i = doc.nodes[i].parent;
    }
    return 0;
}

/* The next node in the page's order, leaving out what is inside a template. */
static int next_in_page(int i) {
    const dnode *x = &doc.nodes[i];
    if (x->first >= 0 && !(x->kind == DN_ELEMENT && x->tag == T_OTHER
                           && w_same_fold(dom_tag_name(&doc, i), "template"))) return x->first;
    while (i >= 0) {
        if (doc.nodes[i].next >= 0) return doc.nodes[i].next;
        i = doc.nodes[i].parent;
    }
    return -1;
}

/* Everything the page's styles are made from, in one number. */
static unsigned styles_print(void) {
    unsigned h = 2166136261u ^ jsdom_css_version();
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT) continue;
        int tag = doc.nodes[i].tag;
        if (tag != T_STYLE && tag != T_LINK) continue;
        int live = node_in_page(i) && !in_template(i);
        h = (h ^ (unsigned)i) * 16777619u;
        h = (h ^ (unsigned)live) * 16777619u;
        if (live) h = (h ^ source_hash(i)) * 16777619u;
    }
    return h;
}

/* Linked sheets fetched after the page was built, kept for the page: read
   again at every change to the page's styles, they would otherwise be asked
   for again each time. */
#define LINKS_KEPT 48
#define LINKS_BYTES (3 * 1024 * 1024)
static struct { char url[URL_TEXT]; char *text; int len; } links_kept[LINKS_KEPT];
static int nlinks_kept, links_kept_bytes;

static void links_forget(void) {
    for (int k = 0; k < nlinks_kept; k++) free(links_kept[k].text);
    nlinks_kept = 0;
    links_kept_bytes = 0;
}

/* A linked sheet's text, and its length, or -1 when it would not come. The
   text may be in cssbuf, and last only until that is used again. */
static int link_text(url_t *u, const char **text) {
    char whole[URL_TEXT];
    url_text(u, whole, (int)sizeof(whole));
    for (int k = 0; k < nlinks_kept; k++)
        if (w_same(links_kept[k].url, whole)) {
            *text = links_kept[k].text;
            return links_kept[k].text ? links_kept[k].len : -1;
        }
    response_t r;
    int rc = web_get(u, cssbuf, CSS_MAX, &r);
    if (rc < 0 || rc == 429 || rc >= 500 || r.cut) rc = web_get(u, cssbuf, CSS_MAX, &r);
    int ok = rc >= 200 && rc < 300 && r.len > 0;
    char *copy = 0;
    if (ok && links_kept_bytes + r.len <= LINKS_BYTES && (copy = (char *)malloc((u64)r.len + 1))) {
        for (int i = 0; i < r.len; i++) copy[i] = r.body[i];
        copy[r.len] = 0;
        links_kept_bytes += r.len;
    }
    if (nlinks_kept < LINKS_KEPT && (copy || !ok)) {
        w_copy(links_kept[nlinks_kept].url, URL_TEXT, whole, URL_TEXT);
        links_kept[nlinks_kept].text = copy;
        links_kept[nlinks_kept].len = ok ? r.len : 0;
        nlinks_kept++;
    } else if (copy) {
        free(copy);
        links_kept_bytes -= r.len;
        copy = 0;
    }
    if (!ok) return -1;
    *text = copy ? copy : r.body;
    return r.len;
}

/* For a script that reads a linked sheet's rules (jscssom.h, jcs_ready). */
static int sheet_text_for_script(const char *href, const char **text) {
    url_t u;
    if (!url_join(&here, href, &u)) return -1;
    return link_text(&u, text);
}

/* Each stylesheet link and preloaded sheet in the page, told once whether
   it arrived; one the base did not fetch is fetched for that. */
#define REPORTED_MAX 256
static struct { int node; unsigned href; } reported[REPORTED_MAX];
static int nreported;

static void links_report(void) {
    if (!styles_live || doc.root < 0) return;
    for (int i = doc.root; i >= 0; i = next_in_page(i)) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_LINK) continue;
        const char *rel = dom_attr(&doc, i, "rel"), *href = dom_attr(&doc, i, "href");
        const char *as = dom_attr(&doc, i, "as");
        if (!rel || !href || !*href || dom_attr(&doc, i, "disabled")) continue;
        int is_sheet = rel_is_sheet(rel);
        if (!is_sheet && !(w_same_fold(rel, "preload") && as && w_same_fold(as, "style"))) continue;
        unsigned h = mix_text(2166136261u, href);
        int k = 0;
        while (k < nreported && !(reported[k].node == i && reported[k].href == h)) k++;
        if (k < nreported) continue;
        int s = source_of(i);
        if (s >= 0 && sources[s].ok == -2) continue;
        if (nreported >= REPORTED_MAX) return;
        reported[nreported].node = i;
        reported[nreported].href = h;
        nreported++;
        int ok;
        if (is_sheet && s >= 0 && sources[s].ok >= 0) ok = sources[s].ok;
        else {
            url_t u;
            const char *t;
            ok = url_join(&here, href, &u) && link_text(&u, &t) >= 0;
        }
        jsdom_link_loaded(i, ok);
    }
}

/* What the page's own sheets gave, before any script ran. */
static void styles_base(void) {
    base_mark.nrules = sheet.nrules;
    base_mark.nsels = sheet.nsels;
    base_mark.ndecls = sheet.ndecls;
    base_mark.nnegs = sheet.nnegs;
    base_mark.used = sheet.used;
    base_mark.nlayers = sheet.nlayers;
    base_mark.overflowed = sheet.overflowed;
    for (int r = 0; r < sheet.nrules; r++) base_decl_n[r] = sheet.rules[r].decl_n;
    styles_made = styles_print();
}

static void sheets_remake(void) {
    sheet.nrules = base_mark.nrules;
    sheet.nsels = base_mark.nsels;
    sheet.ndecls = base_mark.ndecls;
    sheet.nnegs = base_mark.nnegs;
    sheet.used = base_mark.used;
    sheet.nlayers = base_mark.nlayers;
    sheet.overflowed = base_mark.overflowed;
    for (int r = 0; r < base_mark.nrules; r++) sheet.rules[r].decl_n = base_decl_n[r];

    /* The base's elements that no longer give what they gave. */
    static unsigned char intact[SOURCES_MAX];
    for (int k = 0; k < nsources; k++) {
        int i = sources[k].node;
        intact[k] = node_in_page(i) && !in_template(i) && source_hash(i) == sources[k].hash
                    && !jsdom_sheet_override(i, 0, 0);
        if (!intact[k])
            for (int r = sources[k].r0; r < sources[k].r1; r++) sheet.rules[r].decl_n = 0;
    }

    /* And everything else, in the page's order. */
    for (int i = doc.root; i >= 0; i = next_in_page(i)) {
        if (doc.nodes[i].kind != DN_ELEMENT) continue;
        int tag = doc.nodes[i].tag;
        if (tag != T_STYLE && tag != T_LINK) continue;
        int k = source_of(i);
        if (k >= 0 && intact[k]) continue;
        int lo, hi;
        if (!sheet_media(i, &lo, &hi)) continue;
        const char *ov = 0;
        int ovn = 0, has_ov = jsdom_sheet_override(i, &ov, &ovn);
        if (tag == T_STYLE) {
            const char *scope = dom_attr(&doc, i, "data-zs");
            if (has_ov) { css_parse_style(&sheet, ov, ovn, lo, hi, scope); continue; }
            for (int t = doc.nodes[i].first; t >= 0; t = doc.nodes[t].next)
                if (doc.nodes[t].kind == DN_TEXT && doc.nodes[t].text >= 0) {
                    const char *s = doc.arena + doc.nodes[t].text;
                    css_parse_style(&sheet, s, w_len(s), lo, hi, scope);
                }
            continue;
        }
        const char *rel = dom_attr(&doc, i, "rel"), *href = dom_attr(&doc, i, "href");
        if (!rel || !href || !*href || !rel_is_sheet(rel) || dom_attr(&doc, i, "disabled")) continue;
        if (k >= 0 && sources[k].ok == -2) continue;
        url_t u;
        if (!url_join(&here, href, &u)) continue;
        if (has_ov) { css_parse_sheet(&u, ov, ovn, lo, hi); continue; }
        const char *text;
        int n = link_text(&u, &text);
        if (n > 0) css_parse_sheet(&u, text, n, lo, hi);
    }
    const char *t;
    int n;
    for (int k = 0; jsdom_adopted(k, &t, &n); k++) if (n > 0) css_parse_in(&sheet, t, n, -1, -1);

    /* Style attributes read since the base, whose declarations went with
       everything else after it. */
    for (int i = 0; i < doc.count; i++)
        if (inl[i].n > 0 && inl[i].at >= base_mark.ndecls) inline_style_of(i);
    css_index(&sheet, &index_);
    links_report();
}

/* The sheet made again when the page's styles have changed since. */
static void sheets_follow(void) {
    if (!styles_live) return;
    styles_dom_seen = jsdom_dom_version();
    styles_css_seen = jsdom_css_version();
    unsigned now = styles_print();
    if (now == styles_made) return;
    styles_made = now;
    sheets_remake();
}

/* The same for getComputedStyle, which a script may ask in a loop a thousand
   times over: styles_print walks every node, so it is worked out only when
   the tree or a sheet has changed since it last was. */
static void sheets_follow_if_changed(void) {
    if (!styles_live) return;
    if (jsdom_dom_version() == styles_dom_seen && jsdom_css_version() == styles_css_seen) return;
    sheets_follow();
}


/* A script with a src, fetched. Handed to jsdom.h, which knows how to run
   one and deliberately knows nothing about where it came from.

   Relative to the page, the way every other address on it is: a page at
   /a/b.html asking for c.js means /a/c.js, and resolving that is url_join's
   job and not this one's. */
/* How much memory the machine has free, for jsdom.h to ask before it runs a
   script (jd_room_for): running one costs many times its size, and a
   program that touches a page the machine does not have is ended (a one
   megabyte script did that to the browser on a 64 megabyte machine). The
   host build's kernel cannot say. */
static long long free_memory(void) {
    zelr_sysinfo si;
    if (sysinfo(&si) < 0 || si.mem_total_kb == 0) return -1;
    return (long long)si.mem_free_kb * 1024;
}

static int fetch_script(const char *src, const char **out) {
    if (scripts_outside >= SCRIPTS_MAX || scripts_bytes >= SCRIPTS_BYTES || !scriptbuf) return 0;

    url_t u;
    if (!url_join(&here, src, &u)) return 0;

    response_t r;
    int rc = web_get(&u, scriptbuf, SCRIPT_MAX, &r);
    if (rc < 200 || rc >= 300 || r.len <= 0) return 0;
    /* Half a script is a syntax error somewhere in its middle, and the page
       is better told the file would not come. */
    if (r.truncated || r.cut || scripts_bytes + r.len > SCRIPTS_BYTES) return 0;

    scripts_outside++;
    scripts_bytes += r.len;
    *out = r.body;
    return r.len;
}

/* What a script asked the network for.
 *
 * Same origin is not enforced. The request carries the jar's cookies for
 * the address it goes to, as every request this browser makes does
 * (fetch.h), so a page can ask another site for what that site would show
 * the reader anyway, and read the answer -- which a browser with CORS
 * refuses. This one has no CORS: it is said here rather than pretended, and
 * it is a gap, not a boundary.
 *
 * The address is resolved against the page, so a script may ask for a path
 * the way it would write one in a link. */
static int do_request(const char *method, const char *url, const char *body,
                      const char *type, jd_reply *out) {
    static char landed[URL_TEXT];
    static char ctype[64];
    out->body = 0;
    out->len = 0;
    out->status = 0;
    out->type = 0;
    out->url = 0;
    if (asks_made >= ASKS_MAX || !replybuf) return 0;

    url_t u;
    if (!url_join(&here, url, &u)) return 0;

    response_t r;
    int post = method && (method[0] == 'P' || method[0] == 'p');
    web_body_type = type;
    int rc = post ? web_post(&u, body ? body : "", replybuf, REPLY_MAX, &r)
                  : web_get(&u, replybuf, REPLY_MAX, &r);
    web_body_type = 0;

    asks_made++;
    out->status = rc;
    if (rc <= 0) return 0;
    /* Where it ended up, after any redirect (web_send follows them in u),
       and what the server said it was. */
    url_text(&u, landed, sizeof(landed));
    w_copy(ctype, sizeof(ctype), r.ctype, sizeof(ctype));
    out->url = landed;
    out->type = ctype;
    if (r.len <= 0) return 0;
    out->body = r.body;
    out->len = r.len;
    return r.len;
}

/* --- what a page's scripts may ask the browser -----------------------------
 *
 * Where the layout put an element, which is every box it drew for the
 * element or for anything inside it, run together: what
 * getBoundingClientRect answers with. In the page's own terms, down the
 * document; jsdom.h takes the scroll off. An element nothing was drawn for
 * has no box, and says so. */
static u8 box_mark[DOM_NODES / 8];

/* What the layout drew at a point of the page, for elementFromPoint. */
static int node_at_point(int x, int y) { return lay_node_at(&page, x, y); }

static int box_of(int node, int *x, int *y, int *w, int *h) {
    if (node < 0 || node >= doc.count) return 0;
    for (int i = node; i >= 0; i = dom_next(&doc, i, node)) box_mark[i >> 3] |= (u8)(1 << (i & 7));
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, any = 0;
    for (int k = 0; k < page.nitems; k++) {
        const litem *it = &page.items[k];
        int n = it->node;
        if (n < 0 || n >= doc.count || !(box_mark[n >> 3] & (1 << (n & 7)))) continue;
        if (!any || it->x < x0) x0 = it->x;
        if (!any || it->y < y0) y0 = it->y;
        if (!any || it->x + it->w > x1) x1 = it->x + it->w;
        if (!any || it->y + it->h > y1) y1 = it->y + it->h;
        any = 1;
    }
    for (int i = node; i >= 0; i = dom_next(&doc, i, node)) box_mark[i >> 3] &= (u8)~(1 << (i & 7));
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return any;
}

/* An element's style as the layout works it out: the browser's rules, the
   page's, the style attribute, and what it inherits, from the root down to
   it, with the layout's own lay_style (getComputedStyle, jsdom.h). The
   layout's per-element caches are for the page as it was last laid out,
   which a script may have changed since, so they are started again first;
   the next layout starts them again anyway. A percentage is taken of the
   window's width, where the layout would take it of the containing
   block's: the one way this differs. */
static void restyle_changed(void);

static int computed_style(int node, cstyle *out) {
    if (node < 0 || node >= doc.count || doc.nodes[node].kind != DN_ELEMENT) return 0;
    int chain[256], n = 0;
    for (int p = node; p >= 0; p = doc.nodes[p].parent) {
        if (n >= (int)(sizeof(chain) / sizeof(chain[0]))) return 0;
        chain[n++] = p;
    }
    if (chain[n - 1] != doc.root) return 0;             /* not in the page */
    sheets_follow_if_changed();
    restyle_changed();
    lay_gen++;
    lay_hit_used = 0;
    lay_var_used = 0;
    lay_arena_used = 0;

    static lctx L;
    volatile u8 *z = (volatile u8 *)&L;
    for (u32 i = 0; i < sizeof(L); i++) z[i] = 0;
    L.d = &doc; L.s = &sheet; L.x = &index_; L.m = &match; L.inl = inl;
    L.imgs = pic_sizes; L.nimgs = npic_sizes;
    L.out = &page; L.root_px = root_px;
    L.cur_link = -1; L.flex_sized = -1; L.floating = -1;
    int width = css_view_w > 0 ? css_view_w : 800;
    L.line_width = width; L.pos_w = width; L.cont_width = width;

    static cstyle a, b;
    css_default_style(&a, root_px);
    cstyle *cur = &a, *next = &b;
    for (int i = n - 1; i >= 0; i--) {
        lay_style(&L, chain[i], cur, next, width);
        cstyle *t = cur; cur = next; next = t;
    }
    lay_cs(out, cur);
    return 1;
}

/* How big a picture was when it was decoded. */
static int picture_size(int node, int *w, int *h) {
    for (int i = 0; i < npics; i++)
        if (pics[i].node == node && pics[i].pic.rgb) { *w = pics[i].pic.w; *h = pics[i].pic.h; return 1; }
    *w = *h = 0;
    return 0;
}

static void script_scroll(int y) { scroll = y < 0 ? 0 : y; }

/* document.cookie, from the jar and into it, for the page's address: what a
   request there would send, less what is HttpOnly (fetch.h). */
static int script_cookies(char *out, int cap) { return ck_cookies_for(&here, out, cap, 1); }
static void script_set_cookie(const char *line) { ck_take_line(&here, line, 1); }

/* A page sending the browser somewhere: a link a script clicked, and
   location once it is here. Taken on the loop's next pass, as a form's
   address is. `replace` takes the place of the page in the history rather
   than adding to it. */
static int go_replace;

static void script_navigate(const char *url, int replace) {
    w_copy(go_to, sizeof(go_to), url, sizeof(go_to));
    go_is_post = 0;
    post_body[0] = 0;
    go_replace = replace;
    want_go = 1;
}

/* The address the page says it is at now, without loading anything:
   history.pushState and replaceState, and a move to another place on the
   same page (jsdom.h, jd_location_go). The bar shows it, the history keeps
   it, and relative addresses on the page are taken against it, as they are
   in any browser once a page has moved itself. `push` adds it to the
   history rather than taking the place of the entry there. */
static int address_moved;             /* the bar is owed a redraw */

static void push_history(const char *address);
static void set_address(const char *s);

static void script_address(const char *url, int push) {
    url_t u;
    if (!url_parse(url, &u) || !w_same_fold(u.host, here.host) || u.secure != here.secure) return;
    url_copy(&here, &u);
    set_address(url);
    if (push) push_history(url);
    else if (hist_at >= 0) w_copy(hist[hist_at].text, URL_TEXT, url, URL_TEXT);
    address_moved = 1;
}

/* history.back, forward and go: the browser's own buttons, pressed on the
   next pass (want_hist). */
static int want_hist;                 /* how far to move, when hist_go is set */
static int hist_go;

static void script_history_go(int delta) { want_hist = delta; hist_go = 1; }
static int script_history_length(void) { return hist_n > 0 ? hist_n : 1; }

static int page_unhidden;      /* it was laid out a second time, shown anyway */

static void relayout(int width) {
    sheets_follow();
    restyle_changed();
    /* And the title, which a script may have written. */
    if (doc.title >= 0) w_copy(title, sizeof(title), doc.arena + doc.title, sizeof(title));
    drawings_drop();                 /* made for the sizes of the last one */
    match.hover = hover_node;
    match.visited_links = 0;
    lay_show_hidden = 0;
    lay_run(&page, &doc, &sheet, &index_, &match, inl, pic_sizes, npic_sizes,
            width, root_px);

    /* A page that hides its whole self until its script has rebuilt it.
       Measured rather than guessed at: not a word came out of a document
       with words in it, which is what `<div style="visibility:hidden">`
       around everything does to a browser that is not going to run the
       framework that takes it off again. */
    page_unhidden = 0;
    if (lay_words(&page) == 0 && dom_has_words(&doc)) {
        lay_show_hidden = 1;
        lay_run(&page, &doc, &sheet, &index_, &match, inl, pic_sizes,
                npic_sizes, width, root_px);
        lay_show_hidden = 0;
        page_unhidden = lay_words(&page) > 0;
    }
    /* Sizes may have changed, which a page's ResizeObserver hears of on the
       next pass (jsdom.h). */
    jsdom_laid_out();
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

/* Every img in the document, fetched and decoded in the order they appear.
 *
 * The buffer is the one the style sheets use: by this point every sheet has
 * been parsed into the cascade and what is in it is no longer needed, and a
 * second buffer of this size is a megabyte that is idle on every page
 * without a picture on it. */
/* Which address a picture is really at.
 *
 * A page that loads its pictures as they scroll into view puts a
 * placeholder in src -- nothing, or a one-pixel image written into the
 * address itself -- and the real one in data-src or one of its spellings,
 * for its script to swap in. A responsive one may have only a srcset. */
static const char *pic_source(int el) {
    static char pick[URL_TEXT];
    const char *src = dom_attr(&doc, el, "src");
    int placeholder = !src || !*src || w_starts_fold(src, "data:");
    static const char *const LAZY[] = { "data-src", "data-lazy-src", "data-original",
                                        "data-url", "data-hi-res-src", 0 };
    if (placeholder) {
        for (int k = 0; LAZY[k]; k++) {
            const char *v = dom_attr(&doc, el, LAZY[k]);
            if (v && *v && !w_starts_fold(v, "data:")) { src = v; placeholder = 0; break; }
        }
    }
    if (placeholder) {
        /* The first candidate of a srcset: up to its first space or comma. */
        const char *ss = dom_attr(&doc, el, "srcset");
        if (!ss) ss = dom_attr(&doc, el, "data-srcset");
        if (ss) {
            while (*ss == ' ') ss++;
            int k = 0;
            while (ss[k] && ss[k] != ' ' && ss[k] != ',' && k < URL_TEXT - 1) { pick[k] = ss[k]; k++; }
            pick[k] = 0;
            if (k) { src = pick; placeholder = 0; }
        }
    }
    if (placeholder && (!src || !*src)) return 0;
    if (w_starts_fold(src, "data:")) return src;       /* decoded where it is */
    int n = w_len(src);
    if (src != pick) {
        if (n >= URL_TEXT) return 0;
        w_copy(pick, sizeof(pick), src, sizeof(pick));
    }
    return pick;
}

/* A picture written into its own address: data:image/png;base64,... or an
   SVG with its markup escaped. Decoded into `out`; its length, or 0. */
static int pic_data_uri(const char *src, char *out, int cap) {
    const char *comma = src;
    while (*comma && *comma != ',') comma++;
    if (!*comma) return 0;
    int b64 = 0;
    for (const char *q = src; q < comma; q++)
        if (w_starts_fold(q, ";base64")) b64 = 1;
    const char *p = comma + 1;
    int n = 0;
    if (!b64) {
        while (*p && n < cap - 1) {
            if (p[0] == '%' && p[1] && p[2]) {
                int hi = p[1] <= '9' ? p[1] - '0' : (w_lower(p[1]) - 'a' + 10);
                int lo = p[2] <= '9' ? p[2] - '0' : (w_lower(p[2]) - 'a' + 10);
                out[n++] = (char)(hi * 16 + lo);
                p += 3;
            } else {
                out[n++] = *p++;
            }
        }
        out[n] = 0;
        return n;
    }
    unsigned acc = 0;
    int bits = 0;
    for (; *p && n < cap - 1; p++) {
        char c = *p;
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[n++] = (char)((acc >> bits) & 0xFF); }
    }
    out[n] = 0;
    return n;
}

static void gather_pictures(void) {
    for (int i = 0; i < doc.count && npics < PICS_MAX; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_IMG)
            continue;
        if (in_template(i)) continue;          /* a slot for one that is drawn */

        const char *src = pic_source(i);
        if (!src) continue;

        response_t r;
        if (w_starts_fold(src, "data:")) {
            /* A one-pixel placeholder is not worth a slot; anything else is
               decoded where it lies, with no fetch at all. */
            int dn = pic_data_uri(src, cssbuf, CSS_MAX);
            if (dn < 100) continue;
            r.body = cssbuf;
            r.len = dn;
        } else {
            url_t u;
            if (!url_join(&here, src, &u)) { pics_skipped++; continue; }
            web_accept = "image/webp,image/png,image/jpeg,image/gif,image/svg+xml;q=0.9,*/*;q=0.1";
            int rc = web_get(&u, cssbuf, CSS_MAX, &r);
            web_accept = 0;
            if (rc < 0 || rc >= 400 || r.len <= 0) { pics_skipped++; continue; }
        }

        shown *s = &pics[npics];
        s->node = i;
        s->alpha = 0;

        /* Which kind it is, from the bytes rather than from what the
           server said it was. A server that labels a PNG as an octet stream
           is common; a PNG that does not start with the PNG signature is
           not, so the bytes are the better authority. One whose file says
           it may have clear parts is decoded twice, over black and over
           white, so they show what is behind them (pic_merge): flattened
           onto white, a logo on a dark header sat in a white box. */
        const u8 *body = (const u8 *)r.body;
        int ok = 0;
        int clear = pic_may_be_clear(body, r.len);
        u32 under = clear ? 0x000000 : 0xFFFFFF;
        picture white;
        white.rgb = 0;

        if (r.len > 8 && body[0] == 137 && body[1] == 'P'
            && body[2] == 'N' && body[3] == 'G') {
            ok = png_decode(body, r.len, &s->pic, under) == PNG_OK;
            if (ok && clear) ok = png_decode(body, r.len, &white, 0xFFFFFF) == PNG_OK;
        } else if (r.len > 3 && body[0] == 0xFF && body[1] == 0xD8) {
            ok = jpeg_decode(body, r.len, &s->pic) == JPG_OK;
            clear = 0;
        } else if (r.len > 6 && body[0] == 'G' && body[1] == 'I' && body[2] == 'F') {
            ok = gif_decode(body, r.len, &s->pic, under) == GIF_OK;
            if (ok && clear) ok = gif_decode(body, r.len, &white, 0xFFFFFF) == GIF_OK;
        } else if (r.len > 12 && body[0] == 'R' && body[1] == 'I' && body[2] == 'F'
                   && body[3] == 'F' && body[8] == 'W' && body[9] == 'E'
                   && body[10] == 'B' && body[11] == 'P') {
            /* What a server sends a browser that says it takes WebP, and
               what some send whatever the browser says. */
            ok = webp_decode(body, r.len, &s->pic, under) == WEBP_OK;
            if (ok && clear) ok = webp_decode(body, r.len, &white, 0xFFFFFF) == WEBP_OK;
        } else {
            /* A drawing, which is markup and so can start with an XML
               declaration, a comment, or the element itself. */
            int at = 0;
            while (at < r.len && (body[at] == ' ' || body[at] == '\n'
                                  || body[at] == '\r' || body[at] == '\t')) at++;
            if (at < r.len && body[at] == '<') {
                /* What the page asked for, so a drawing lands at the size
                   the layout is about to leave for it rather than at
                   whatever size it happens to describe. */
                const char *aw = dom_attr(&doc, i, "width");
                const char *ah = dom_attr(&doc, i, "height");
                int want_w = aw ? lay_number(aw) : 0;
                int want_h = ah ? lay_number(ah) : 0;
                ok = svg_render((const char *)body, r.len, want_w, want_h,
                                &s->pic, 0x000000) == SVG_OK
                  && svg_render((const char *)body, r.len, want_w, want_h,
                                &white, 0xFFFFFF) == SVG_OK;
                clear = 1;
            }
        }

        if (ok && clear && white.rgb) ok = pic_merge(&s->pic, &white, &s->alpha);
        else if (white.rgb) picture_free(&white);
        if (!ok) { picture_free(&s->pic); pics_skipped++; continue; }

        pic_sizes[npic_sizes].node = i;
        pic_sizes[npic_sizes].w = s->pic.w;
        pic_sizes[npic_sizes].h = s->pic.h;
        npic_sizes++;
        npics++;
    }
}

static void build(const char *html, int len, int width, int want_sheets,
                  int *fetched, int *skipped) {
    /* Before the tree it is bound to is taken apart under it. */
    jsdom_close();
    nsources = 0;
    nreported = 0;
    styles_live = 0;
    styles_dom_seen = 0xFFFFFFFFu;
    links_forget();
    dom_parse(&doc, html, len);
    dom_shadows(&doc);

    css_init(&sheet);
    css_parse(&sheet, CSS_UA, (int)sizeof(CSS_UA) - 1);
    accent_sheet();
    sheet.ua_rules = sheet.nrules;        /* the page's own count for more */
    gather_inline_sheets();
    if (want_sheets) gather_linked_sheets(fetched, skipped);
    else { *fetched = 0; *skipped = 0; }
    gather_inline_styles();
    styles_base();
    css_index(&sheet, &index_);

    /* The pictures, before the layout so it can leave room for them. */
    pics_drop();
    if (want_sheets) gather_pictures();

    /* And then whatever the page brought with it, before any of it is laid
       out: a script that writes to an element is writing to the document
       the layout is about to read, so running them afterwards would show
       the page as it was and correct it a frame later. */
    focus_control(-1);
    script_err[0] = 0;
    said_script_err = 0;
    scripts_ran = 0;
    scripts_changed = 0;
    scripts_outside = 0;
    scripts_bytes = 0;
    asks_made = 0;
    /* The page's address for its scripts, with the fragment the reader asked
       for, which is never sent and so is not in `here`. */
    char at[URL_TEXT];
    url_text(&here, at, sizeof(at));
    {
        int h = 0, n = w_len(at);
        while (address[h] && address[h] != '#') h++;
        if (address[h] == '#' && n + w_len(address + h) < (int)sizeof(at))
            w_copy(at + n, (int)sizeof(at) - n, address + h, (int)sizeof(at) - n);
    }
    jsdom_at(at);
    jsdom_view(width, css_view_h, 0);
    if (jsdom_open(&doc, &sheet)) {
        jsdom_fetch_with(fetch_script);
        jsdom_request_with(do_request);
        jsdom_boxes_with(box_of);
        jsdom_points_with(node_at_point);
        jsdom_pictures_with(picture_size);
        jsdom_scroll_with(script_scroll);
        jsdom_navigate_with(script_navigate);
        jsdom_submit_with(submit_form);
        jsdom_cookies_with(script_cookies, script_set_cookie);
        jsdom_address_with(script_address);
        jsdom_history_with(script_history_go, script_history_length);
        jsdom_styles_with(computed_style);
        jsdom_memory_with(free_memory);
        jsdom_sheet_text_with(sheet_text_for_script);
        styles_live = want_sheets;
        links_report();
        scripts_ran = jsdom_scripts(script_err, (int)sizeof(script_err));
        jsdom_loaded();
        scripts_changed = jsdom_changed();
    }

    hover_node = -1;
    hover_n = 0;
    relayout(width);
    if (want_sheets) gather_backgrounds();
    find_item = -1;
    publish_text();

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

/* --- looking something up ------------------------------------------------
 *
 * Where a search goes, and why it is not Google.
 *
 * Google's results are not in the page it sends. Asked for
 * /search?q=anything it returns ninety kilobytes with no result in it at
 * all -- no headings, no outbound links -- and builds the page from script
 * afterwards. Measured across three user agents and four sets of
 * parameters, including the ones that used to mean "no script": every one
 * came back the same way. It is not a check on what this browser is, it is
 * that the answer is not sent.
 *
 * So a search here goes somewhere that sends its answers. DuckDuckGo's lite
 * endpoint is HTML -- a table of links and snippets, nothing to run to read
 * it -- and it renders in this browser as it stands.
 *
 * That is a limitation stated rather than a preference. A browser that
 * cannot run a search engine's application cannot use that search engine,
 * and naming the one it can use is more useful than failing at the one it
 * cannot. A search asked of Google -- from its own home page, whose form
 * still works, or typed as an address -- is sent here too, and the page says
 * so at the top (sites.h, site_google_search).
 */
#define SEARCH_PREFIX "https://lite.duckduckgo.com/lite/?q="

/* --- where it starts ------------------------------------------------------
 *
 * A page of its own. The browser used to open on example.com, which is a page
 * about being an example, and on a machine with no network it opened on an
 * error saying so. This one needs nothing from outside: a search box, and the
 * sites this browser does well, each said plainly -- YouTube and Twitch are
 * listed and not played, and that is written next to them rather than found
 * out. */
#define START_ADDRESS "about:start"

static void show_start(int width) {
    static const char PAGE_START[] =
        "<html><head><title>start</title><style>"
        "body{padding:22px 30px;max-width:760px}"
        "h1{font-size:1.7em;color:#333;margin-bottom:0.5em}"
        "h2{font-size:1.15em;color:#444;margin-top:1.4em}"
        "li{margin:0.45em 0}small{color:#666}"
        "</style></head><body>"
        "<h1>where to?</h1>"
        "<form action=\"https://lite.duckduckgo.com/lite/\" method=\"get\">"
        "<input type=\"text\" name=\"q\" size=\"44\"> <input type=\"submit\" value=\"search\"></form>"
        "<p><small>or type an address, or words to look for, into the bar above</small></p>"
        "<h2>sites</h2><ul>"
        "<li><a href=\"https://www.youtube.com/results?search_query=music\">YouTube</a> "
        "<small>searches, videos and channels, read from YouTube's own data; videos are "
        "listed with frames from them, not played</small></li>"
        "<li><a href=\"https://www.twitch.tv/\">Twitch</a> "
        "<small>who is live, the categories and the channels, from Twitch's API; streams "
        "are listed, not played</small></li>"
        "<li><a href=\"https://en.wikipedia.org/wiki/Special:Random\">Wikipedia</a> "
        "<small>an article at random</small></li>"
        "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
        "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo</a> "
        "<small>where searches go, Google's included</small></li>"
        "<li><a href=\"https://example.com/\">example.com</a></li>"
        "</ul></body></html>";
    int fetched, skipped;
    build(PAGE_START, (int)sizeof(PAGE_START) - 1, width, 0, &fetched, &skipped);
    scroll = 0;
}

/* What a page read another way (sites.h) is built from: YouTube's pages are
   a megabyte and a half of data around a few kilobytes of page, more than
   src holds, and they are the only pages that big worth reading whole, so
   the room for them is asked for only when one is fetched. */
#define SITE_SRC_MAX (4 * 1024 * 1024)

/* A page with a line of our own at the top, saying what was done to get it.
   Written into a buffer of its own, because the page is in src. */
static void build_noted(const char *note, const char *html, int len, int width,
                        int *fetched, int *skipped) {
    int nl = w_len(note);
    char *both = (char *)malloc((u64)(nl + len + 1));
    if (!both) {
        build(html, len, width, 1, fetched, skipped);
        return;
    }
    /* Just inside the page's body. Put in front of everything, it came before
       the page's <html>, and the reader put it in the head, which is never
       shown: the page was right and the line saying why was not there. */
    int at = 0;
    for (int i = 0; i + 5 < len; i++) {
        if (html[i] == '<' && w_lower(html[i + 1]) == 'b' && w_lower(html[i + 2]) == 'o'
            && w_lower(html[i + 3]) == 'd' && w_lower(html[i + 4]) == 'y'
            && (html[i + 5] == '>' || html[i + 5] == ' ' || html[i + 5] == '\t' || html[i + 5] == '\n')) {
            int k = i + 5;
            while (k < len && html[k] != '>') k++;
            if (k < len) at = k + 1;
            break;
        }
    }
    volatile char *d = both;
    for (int i = 0; i < at; i++) d[i] = html[i];
    for (int i = 0; i < nl; i++) d[at + i] = note[i];
    for (int i = at; i < len; i++) d[nl + i] = html[i];
    d[nl + len] = 0;
    build(both, nl + len, width, 1, fetched, skipped);
    free(both);
}

static void load(const char *address, int width, int keep_scroll) {
    if (w_same_fold(address, START_ADDRESS)) {
        show_start(width);
        char shown[16];
        number_into(shown, page.nlinks);
        say("the start page, which needs no network: ", shown);
        say_more(" links");
        return;
    }

    url_t u;
    if (!url_parse(address, &u)) {
        show_message("Not an address", "That is not something this can go to.",
                     width);
        say("nothing to go to", 0);
        return;
    }

    /* A search asked of Google is asked where the answer comes as a page
       (sites.h). The address shown stays the one that was asked for. */
    static char words[URL_TEXT];
    int from_google = 0;
    if (!load_post && site_google_search(&u, words, (int)sizeof(words))) {
        char q[URL_TEXT], full[URL_TEXT];
        url_escape(words, q, (int)sizeof(q));
        int w = 0;
        for (const char *p = SEARCH_PREFIX; *p && w < (int)sizeof(full) - 1; p++) full[w++] = *p;
        for (const char *p = q; *p && w < (int)sizeof(full) - 1; p++) full[w++] = *p;
        full[w] = 0;
        if (url_parse(full, &u)) from_google = 1;
    }

    url_copy(&here, &u);
    say("fetching ", address);

    /* Twitch sends no page worth fetching, only its application; what it
       would show comes from its API, and is written as a page here. */
    if (!load_post && site_is_twitch(&here)) {
        say("asking Twitch who is live", 0);
        int n = site_twitch(&here, src, SRC_MAX);
        if (n > 0) {
            int fetched, skipped;
            build(src, n, width, 1, &fetched, &skipped);
            if (!keep_scroll) scroll = 0;
            say("read from Twitch's API: it cannot play the streams", 0);
            return;
        }
        if (n < 0) {
            show_message(why_heading(n), why(n), width);
            say(why(n), 0);
            title[0] = 0;
            return;
        }
    }

    /* A YouTube page is fetched into room of its own (SITE_SRC_MAX), freed
       once it has been read. */
    char *into = src;
    int room = SRC_MAX;
    char *big = 0;
    if (!load_post && site_is_youtube(&here)) {
        big = (char *)malloc(SITE_SRC_MAX);
        if (big) { into = big; room = SITE_SRC_MAX; }
    }

    /* A form sent with POST is the one fetch that carries something, and
       it is spent once: going back to it afterwards asks again with GET
       rather than sending the form a second time. */
    int rc;
    if (load_post) {
        load_post = 0;
        rc = web_post(&here, post_body, into, room, &reply);
    } else {
        rc = web_get(&here, into, room, &reply);
    }
    if (rc < 0) {
        if (big) free(big);
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
    int read_site = 0;
    if (plain) {
        show_plain(reply.body, reply.len, width);
        w_copy(title, sizeof(title), here.path, sizeof(title));
    } else if (big && rc < 400
               && (read_site = site_youtube(&here, reply.body, reply.len, src, SRC_MAX)) > 0) {
        /* A video's comments are a second question, asked with the token the
           page carries for them, while the page is still in hand; and a
           channel whose page listed none of its videos has them in its feed. */
        static char extra[64 * 1024];
        if (w_starts_fold(here.path, "/watch")) {
            int cn = site_youtube_comments(reply.body, reply.len, extra, (int)sizeof(extra));
            if (cn > 0) read_site = site_append(src, read_site, SRC_MAX, extra, cn);
        } else if (site_youtube_feed_for[0]) {
            int fn = site_youtube_feed(site_youtube_feed_for, extra, (int)sizeof(extra));
            if (fn > 0) read_site = site_append(src, read_site, SRC_MAX, extra, fn);
        }
        build(src, read_site, width, 1, &fetched, &skipped);
    } else if (from_google) {
        build_noted("<p style=\"background:#fff4d6;padding:6px 10px\">Google shows its results "
                    "only to its own script, which this browser cannot run, so these are "
                    "DuckDuckGo's for the same words.</p>",
                    reply.body, reply.len, width, &fetched, &skipped);
    } else {
        build(reply.body, reply.len, width, 1, &fetched, &skipped);
    }
    if (big) free(big);
    big = 0;

    if (!keep_scroll) scroll = 0;

    char shown[16];
    number_into(shown, page.nlinks);

    if (rc >= 400) say("the server said this page is not there", 0);
    else if (read_site > 0)
        say("read from the data in YouTube's page: it cannot play the videos", 0);
    else if (page_unhidden)
        say("this page hides itself until its own script rebuilds it; "
            "shown as it arrived", 0);
    /* Three different things, said differently: which one it was decides
       what could be done about it, and one sentence for all three told
       nobody anything -- including the person writing this, who spent an
       afternoon working out which limit a page had reached. */
    else if (reply.truncated)
        say("shown as far as it fits: more page arrived than this can hold", 0);
    else if (reply.cut)
        say("shown as far as it came: the server stopped before the end of the page", 0);
    else if (doc.overflowed)
        say("shown as far as it fits: more markup than this can hold", 0);
    else if (page.overflowed)
        say("shown as far as it fits: more on the page than this can lay out", 0);
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
    if (from_google) say_more(", DuckDuckGo's answer to a Google search");

    /* And what the page's own scripts did. A script that threw is worth
       saying out loud: the page will look like the one it was before it
       ran, and without this there is nothing to tell the two apart. */
    if (npics) {
        char n[16];
        number_into(n, npics);
        say_more(", ");
        say_more(n);
        say_more(npics == 1 ? " picture" : " pictures");
        if (pics_skipped) say_more(" (more would not show)");
    } else if (pics_skipped) {
        say_more(", no picture on it would show");
    }

    if (script_err[0]) {
        say_more(", a script stopped: ");
        say_more(script_err);
    } else if (scripts_ran) {
        char n[16];
        number_into(n, scripts_ran);
        say_more(", ");
        say_more(n);
        say_more(scripts_ran == 1 ? " script ran" : " scripts ran");

        /* Which of them were files of their own, because a page whose work
           is in one file it links to and which runs without it is not the
           page -- the same argument as for a style sheet, and the same
           silence to avoid. */
        if (jsdom_outside()) {
            number_into(n, jsdom_outside());
            say_more(" (");
            say_more(n);
            say_more(jsdom_outside() == 1 ? " from a file)" : " from files)");
        }
        if (jsdom_outside_failed()) say_more(", one would not come");
    }

    /* And whether it asked to be somewhere else. */
    if (rc >= 200 && rc < 400 && refreshes < REFRESH_MAX) {
        char where[URL_TEXT];
        if (meta_refresh(where, sizeof(where))) {
            url_t next;
            if (url_join(&here, where, &next)) {
                char text_of[URL_TEXT];
                url_text(&next, text_of, sizeof(text_of));
                char now[URL_TEXT];
                url_text(&here, now, sizeof(now));
                if (!w_same(text_of, now)) {
                    refreshes++;
                    go_is_refresh = 1;
                    w_copy(go_to, sizeof(go_to), text_of, sizeof(go_to));
                    go_is_post = 0;
                    post_body[0] = 0;
                    want_go = 1;
                    say_more(", following the page's own redirect");
                }
            }
        }
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
            /* A mask's colour is painted only through its picture. */
            int masked = it->bgi >= 0 && page.bgs[it->bgi].mask;
            if (it->has_bg && !masked) {
                if (it->radius) round_rect(s, x, sy, w, h, it->radius, it->bg);
                else rect(s, x, sy, w, h, it->bg);
            }
            if (it->bgi >= 0) draw_background(s, it, x, sy, ox, oy, vw, vh);
            if (it->bt) rect(s, x, sy, w, it->bt, it->border);
            if (it->bb) rect(s, x, sy + h - it->bb, w, it->bb, it->border);
            if (it->bl) rect(s, x, sy, it->bl, h, it->border);
            if (it->br) rect(s, x + w - it->br, sy, it->br, h, it->border);
            continue;
        }

        if (it->kind == LK_IMAGE) {
            const picture *p = pic_of(it->node);
            const u8 *alpha = p ? pic_alpha_of(it->node) : 0;
            if (!p && it->node >= 0 && it->node < doc.count && doc.nodes[it->node].tag == T_SVG)
                p = drawing_of(it);
            if (p && p->rgb && it->w > 0 && it->h > 0) {
                /* Nearest neighbour, chosen rather than settled for. A
                   picture on a page is usually drawn at or near its own
                   size, where every filter agrees; where it is not, the
                   difference is a page that draws now against one that
                   draws in a moment.

                   Drawn at tw by th inside the box, centred, as object-fit
                   says: stretched to it; its shape kept and all of it
                   showing (contain) or all of the box covered (cover); its
                   own size; or the smaller of that and contain. Stretched,
                   every card's thumbnail was a squashed picture. */
                int tw = it->w, th = it->h;
                if (it->ofit && p->w > 0 && p->h > 0) {
                    long long fx = (long long)it->w * 65536 / p->w, fy = (long long)it->h * 65536 / p->h;
                    long long f = it->ofit == 2 ? (fx > fy ? fx : fy) : (fx < fy ? fx : fy);
                    if (it->ofit == 3 || (it->ofit == 4 && f > 65536)) f = 65536;
                    tw = (int)((p->w * f) >> 16);
                    th = (int)((p->h * f) >> 16);
                    if (tw < 1) tw = 1;
                    if (th < 1) th = 1;
                }
                int x0 = (it->w - tw) / 2, y0 = (it->h - th) / 2;
                int rad = it->radius;
                if (rad * 2 > it->w) rad = it->w / 2;
                if (rad * 2 > it->h) rad = it->h / 2;
                for (int row = 0; row < it->h; row++) {
                    int dy = sy + row;
                    if (dy < oy || dy >= oy + vh) continue;
                    int ty = row - y0;
                    if (ty < 0 || ty >= th) continue;
                    int src_y = ty * p->h / th;
                    for (int col = 0; col < it->w; col++) {
                        int dx = x + col;
                        if (dx < ox || dx >= ox + vw) continue;
                        if (dx < 0 || dx >= s->w) continue;
                        int tx = col - x0;
                        if (tx < 0 || tx >= tw) continue;
                        if (rad > 0) {
                            /* Outside a rounded corner is not the picture. */
                            int cx = col < rad ? rad - col : col >= it->w - rad ? col - (it->w - rad - 1) : 0;
                            int cy = row < rad ? rad - row : row >= it->h - rad ? row - (it->h - rad - 1) : 0;
                            if (cx && cy && cx * cx + cy * cy > rad * rad) continue;
                        }
                        int at = src_y * p->w + tx * p->w / tw;
                        const u8 *q = p->rgb + at * 3;
                        u32 *d = &s->px[(u32)dy * s->w + dx];
                        int al = alpha ? alpha[at] : 255;
                        if (al == 255) {
                            *d = ((u32)q[0] << 16) | ((u32)q[1] << 8) | q[2];
                        } else if (al) {
                            /* Colour times alpha, and what is behind
                               through what it does not cover. */
                            int back = 255 - al;
                            int r2 = q[0] + (int)((*d >> 16) & 255) * back / 255;
                            int g2 = q[1] + (int)((*d >> 8) & 255) * back / 255;
                            int b2 = q[2] + (int)(*d & 255) * back / 255;
                            *d = ((u32)(r2 > 255 ? 255 : r2) << 16)
                               | ((u32)(g2 > 255 ? 255 : g2) << 8) | (u32)(b2 > 255 ? 255 : b2);
                        }
                    }
                }
            }
            continue;
        }

        if (it->kind == LK_FIELD) {
            int ck = lay_control_kind(&doc, it->node);
            int focused = (it->node == focus_node);
            u32 edge = focused ? 0x3B6FD6 : 0xA9A9A9;
            int w = it->w, h = it->h;

            /* A page that styles its fields gets the fields it styled.
               Without a colour of its own a text box is paper and a button
               is the colour of a button. */
            u32 inside = it->has_bg ? it->bg
                       : (ck == CTL_BUTTON ? 0xE6E6EA : 0xFFFFFF);

            if (ck == CTL_CHECK || ck == CTL_RADIO) {
                rect(s, x, sy, w, h, inside);
                rect(s, x, sy, w, 1, edge);
                rect(s, x, sy + h - 1, w, 1, edge);
                rect(s, x, sy, 1, h, edge);
                rect(s, x + w - 1, sy, 1, h, edge);
                if (field_checked(it->node)) {
                    if (ck == CTL_RADIO) disc(s, x + w / 2, sy + h / 2,
                                              w / 4, 0x1A1A1A);
                    else rect(s, x + 3, sy + 3, w - 6, h - 6, 0x1A1A1A);
                }
                continue;
            }

            /* Rounded when the page asked for it, which the search box on
               every site written this decade does. A field drawn square on
               a page that rounded it does not look like a slightly wrong
               field, it looks like a different era. */
            int rad = it->radius;
            if (rad * 2 > h) rad = h / 2;
            if (rad > 0) {
                /* The edge is the shape in the line colour with the inside
                   drawn on top of it a pixel in, which is how everything
                   else here draws a hairline round a curve. */
                u32 line = (it->bt || it->br || it->bb || it->bl) ? it->border
                                                                  : edge;
                ui_round(s, x, sy, w, h, rad, line, 255);
                ui_round(s, x + 1, sy + 1, w - 2, h - 2, rad - 1, inside, 255);
            } else {
                u32 line = (it->bt || it->br || it->bb || it->bl) ? it->border
                                                                  : edge;
                rect(s, x, sy, w, h, inside);
                rect(s, x, sy, w, 1, line);
                rect(s, x, sy + h - 1, w, 1, line);
                rect(s, x, sy, 1, h, line);
                rect(s, x + w - 1, sy, 1, h, line);
            }

            /* Read out of the document now rather than held from when the
               page was laid out, because typing changes it and typing does
               not lay the page out again. */
            const char *val = field_value(it->node, ck);
            char shown[192];
            int n = 0;
            if (ck == CTL_PASSWORD) {
                for (const char *q = val; *q && n < (int)sizeof(shown) - 1; q++)
                    shown[n++] = '*';
            } else {
                for (const char *q = val; *q && n < (int)sizeof(shown) - 1; q++)
                    shown[n++] = (*q == '\n' || *q == '\r') ? ' ' : *q;
            }
            shown[n] = 0;

            /* The end of it rather than the start: somebody typing wants to
               see what they are typing, and a field that shows the first
               twenty characters of what they wrote is one they cannot use. */
            int room = w - 10;
            int from = 0;
            while (from < n && tface_wn(shown + from, n - from, it->face) > room)
                from++;

            int th = tface_h(it->face);
            int ty = sy + (h - th) / 2;
            if (ck == CTL_AREA) ty = sy + 4;
            int tx = x + 5;
            if (ck == CTL_BUTTON)
                tx = x + (w - tface_wn(shown + from, n - from, it->face)) / 2;

            tface_draw(s, tx, ty, shown + from, 0x1A1A1A, it->face);

            if (focused && ck != CTL_BUTTON) {
                int cx = tx + tface_wn(shown + from, n - from, it->face);
                if (cx > x + w - 3) cx = x + w - 3;
                rect(s, cx + 1, ty, 1, th, 0x1A1A1A);
            }
            continue;
        }

        if (it->kind == LK_BULLET) {
            if (it->at >= 0)
                tface_draw(s, x, sy, page.text + it->at, it->color, it->face);
            else
                disc(s, x + 6, sy + it->h / 2, 3, it->color);
            continue;
        }

        if (i == find_item) {
            /* Behind the words rather than over them, so they stay
               readable: a find that hides what it found is a find that
               makes somebody scroll back to it. */
            rect(s, x - 2, sy - 1, tface_w(page.text + it->at, it->face) + 4,
                 tface_h(it->face) + 3, 0xFFE58F);
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
 * everything but drawing: there is nothing here that can show one yet.
 *
 * It used to be set when the bar became focused, which is not the same
 * thing and is wrong in the ordinary case: the bar keeps the keyboard after
 * an address is entered, so the second address somebody types is clicked
 * into a field that already has it, no transition happens, and what they
 * type is appended to what was there. Two addresses run together into one
 * and the page does not change. */
static int bar_fresh;

static void set_address(const char *s) {
    w_copy(address, sizeof(address), s, sizeof(address));
    bar.len = w_len(address);
    bar.cursor = bar.len;
}

static void set_search(const char *what) {
    char q[URL_TEXT];
    url_escape(what, q, (int)sizeof(q));

    char full[URL_TEXT];
    int w = 0;
    for (const char *p = SEARCH_PREFIX; *p && w < (int)sizeof(full) - 1; p++)
        full[w++] = *p;
    for (const char *p = q; *p && w < (int)sizeof(full) - 1; p++)
        full[w++] = *p;
    full[w] = 0;
    set_address(full);
}

/* What was typed: somewhere to go, or something to look for. */
static void go_or_search(const char *typed) {
    if (w_same_fold(typed, START_ADDRESS)) set_address(START_ADDRESS);
    else if (url_looks_like_address(typed)) set_address(typed);
    else set_search(typed);
}

/* Asleep until something arrives for the window, the page has a timer due or
   a request waiting, or something drawn is due to change -- rather than
   looking sixty times a second at a page nobody is touching. */
static void browser_wait(int win) {
    /* A tick at the least, even for a timer already due: a page that sets
       a zero timeout from inside one would otherwise have the processor to
       itself, where the old sixteen milliseconds held it to sixty a second. */
    int due = jsdom_next_due();
    if (due >= 0) ui_due(ticks() + (due > 0 ? due : 1));
    ui_wait(win);
}

int main(int argc, char **argv) {
    src = (char *)map(SRC_MAX, PROT_READ | PROT_WRITE);
    cssbuf = (char *)map(CSS_MAX, PROT_READ | PROT_WRITE);
    scriptbuf = (char *)map(SCRIPT_MAX, PROT_READ | PROT_WRITE);
    replybuf = (char *)map(REPLY_MAX, PROT_READ | PROT_WRITE);
    doc_mem = (ddoc *)map(sizeof(ddoc), PROT_READ | PROT_WRITE);
    /* Twice the largest page this reads, since every string of it goes here;
       mapped, so a page costs only the part it uses (dom_use_arena). */
    char *arena = (char *)map(DOC_ARENA, PROT_READ | PROT_WRITE);
    if (doc_mem) dom_use_arena(doc_mem, arena, DOC_ARENA);
    sheet_mem = (csheet *)map(sizeof(csheet), PROT_READ | PROT_WRITE);
    page_mem = (ldoc *)map(sizeof(ldoc), PROT_READ | PROT_WRITE);
    if (!src || !cssbuf || !doc_mem || !sheet_mem || !page_mem) exit(1);

    int win = win_create("Browser", 860, 620);
    if (win < 0) exit(1);
    browser_win = win;
    win_allow_resize(win);
    win_want_escape(win);                   /* it has a use for Escape */

    ui_input in;
    memset(&in, 0, sizeof(in));

    const char *arg = argc > 1 ? argv[1] : "";
    int have_arg = arg[0] != 0;

    int laid_for = 0;
    int want_load = 1;
    css_view_h = 600;                    /* until the window has been drawn */
    int want_width = 0;
    int last_mx = -1, last_my = -1, last_scroll = -1;
    int last_hover = -2;
    int dirty = 1;              /* something changed and a frame is owed */

    /* Started on something, which may equally be an address or a thing to
       look for: `browser an operating system` should search for one. */
    if (have_arg) {
        char joined[URL_TEXT];
        int w = 0;
        for (int i = 1; i < argc && w < (int)sizeof(joined) - 1; i++) {
            if (i > 1 && w < (int)sizeof(joined) - 1) joined[w++] = ' ';
            for (const char *p = argv[i]; *p && w < (int)sizeof(joined) - 1; p++)
                joined[w++] = *p;
        }
        joined[w] = 0;
        go_or_search(joined);
    } else {
        set_address(START_ADDRESS);
    }
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
        css_view_h = view_h;             /* what vh is a hundredth of */

        /* The page is laid out against the width it is shown at, so making
           the window wider reflows it rather than revealing more margin. */
        if (view_w != laid_for && !want_load) want_width = 1;

        ui_begin(&in);
        win_event ev;
        int closing = 0;
        int scrolled = 0;
        while (win_poll(win, &ev)) {
            if (ev.type == WIN_EV_CLOSE) { closing = 1; break; }
            if (ev.type == WIN_EV_FIND) {
                find_show(ev.y, view_h);
                dirty = 1;
                continue;
            }
            if (ev.type == WIN_EV_SCROLL) scrolled += ev.y;
            ui_feed(&in, &ev);

            /* One frame holds sixteen keys and this loop drains the whole
               queue, so everything past the sixteenth used to be read out
               of the queue and thrown away. Nothing reports that: what it
               looks like is an address bar that loses most of a long
               address, which reads as the keyboard or the network rather
               than as this.
             *
             * Left in the queue instead. There is a frame owed already --
             * keys arrived -- and the next pass takes the next sixteen. */
            if (in.nkeys >= UI_KEYS) break;
        }
        if (closing) break;

        /* What a script asking about the window is told this pass. */
        jsdom_view(view_w - UI_PAD * 2, view_h, scroll);
        jsdom_window(w, h);

        /* A reflow costs a pass over the whole page, so it happens once the
           dragging has stopped rather than on every frame of it. */
        if (want_width && !in.down) {
            relayout(view_w - UI_PAD * 2);
            laid_for = view_w;
            want_width = 0;
            publish_text();
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
                    /* A sentence rather than an address is something to
                       look for, which is what an address bar has meant for
                       twenty years. */
                    go_or_search(address);
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
            } else if (focus_node >= 0) {
                int ck = lay_control_kind(&doc, focus_node);
                if (k == 27) {
                    focus_control(-1);
                } else if (k == '\n' && ck != CTL_AREA) {
                    /* Return in a field sends the form it is in, which is
                       how a search box has always worked and the only way
                       to use one that has no button beside it. */
                    int f = form_of(focus_node);
                    focus_control(-1);
                    /* The page hears of it first, and may send it its own
                       way instead (jsdom.h, jsdom_submitting). */
                    if (!jsdom_submitting(f)) submit_form(f);
                } else if (ck == CTL_CHECK || ck == CTL_RADIO) {
                    if (k == ' ') {
                        field_set_checked(focus_node,
                                          !field_checked(focus_node));
                        jsdom_toggled(focus_node);
                    }
                } else {
                    ui_field_key(&focus_field, raw);
                    field_set_value(focus_node, focus_buf);
                    jsdom_typed(focus_node);
                }
                dirty = 1;
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

        /* A handler for what was typed may have changed the page. */
        if (jsdom_live() && jsdom_changed()) {
            relayout(view_w - UI_PAD * 2);
            dirty = 1;
        }

        /* Anything the page asked to have done later. A page that calls
           setTimeout and is never called back is not slow: it is stopped
           part of the way through whatever it was doing. */
        if (jsdom_live() && jsdom_timers() && jsdom_changed()) {
            relayout(view_w - UI_PAD * 2);
            dirty = 1;
        }

        /* And anything it asked the network for. One per pass: each blocks
           this loop while it happens, and a page that sent six would
           otherwise stop for all six before drawing anything. */
        if (jsdom_live() && jsdom_requests() && jsdom_changed()) {
            relayout(view_w - UI_PAD * 2);
            dirty = 1;
        }

        /* Where a form asked to go, once the click or the key that sent
           it has been dealt with. */
        /* The history buttons, pressed by a script. */
        if (hist_go) {
            hist_go = 0;
            int to = hist_at + want_hist;
            if (want_hist == 0) {
                want_load = 1;
            } else if (to >= 0 && to < hist_n) {
                hist[hist_at].scroll = scroll;
                hist_at = to;
                set_address(hist[hist_at].text);
                want_load = 1;
            }
            dirty = 1;
        }
        if (address_moved) { address_moved = 0; dirty = 1; }

        if (want_go) {
            want_go = 0;
            load_post = go_is_post;
            set_address(go_to);
            if (go_replace && hist_at >= 0) w_copy(hist[hist_at].text, URL_TEXT, go_to, URL_TEXT);
            else push_history(go_to);
            go_replace = 0;
            want_load = 1;
            dirty = 1;
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

        if (!dirty) { browser_wait(win); continue; }
        dirty = 0;

        /* The theme comes off the disk, so it is read on a frame that is
           being drawn rather than on every pass of this loop. */
        ui_theme t = ui_load_theme();

        if (want_load) {
            want_load = 0;
            /* A reader asking for an address starts the count again; a page
               asking on their behalf does not, or a pair of pages pointing
               at each other would go round for ever. */
            if (!go_is_refresh) refreshes = 0;
            go_is_refresh = 0;
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
            if (w_same_fold(address, START_ADDRESS))
                w_copy(landed, sizeof(landed), START_ADDRESS, sizeof(landed));
            else
                url_text(&here, landed, sizeof(landed));
            set_address(landed);
            if (hist_at >= 0) w_copy(hist[hist_at].text, URL_TEXT, landed,
                                     URL_TEXT);

            /* And what it came to, on the console, where nobody on the
               desktop sees it: a machine driven over its serial line has no
               other way to know what a page turned into (sitecheck.py). */
            puts("browser: ");
            puts(landed);
            puts(" -- ");
            puts(title);
            puts(" -- ");
            puts(status);
            putc('\n');
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
            int reach[16], nr = css_hover_reach(&sheet, &doc, node_under, reach, 16);
            int same = nr == hover_n;
            for (int k = 0; same && k < nr; k++) same = reach[k] == hover_reach[k];
            if (!same) {
                hover_n = nr;
                for (int k = 0; k < nr; k++) hover_reach[k] = reach[k];
                relayout(view_w - UI_PAD * 2);
                over_link = lay_link_at(&page, dx, dy);
            }
        }

        /* A click goes to the page before it goes to the browser.
         *
         * Which order that happens in is the whole behaviour of a menu, a
         * tab strip and every link that is really a button: the page gets
         * to say the ordinary consequence should not follow, and if it says
         * so, the link under the pointer is not followed. A browser that
         * navigated first would run the handler on a page that was already
         * leaving. */
        if (in.released && node_under >= 0 && jsdom_live()) {
            int stop = jsdom_click_at(node_under, dx, in.my - view_y);

            /* A handler that changed the document changed what is on the
               screen, and nothing else in this loop would notice: the
               layout is rebuilt on a resize, a hover or a load, and a click
               is none of those. */
            if (jsdom_changed()) {
                relayout(view_w - UI_PAD * 2);
                over_link = lay_link_at(&page, dx, dy);
                dirty = 1;
            }
            if (jsdom_error()[0] && !said_script_err) {
                said_script_err = 1;
                say("a script stopped: ", jsdom_error());
            }
            if (stop) in.released = 0;
        }

        /* A control takes the click before a link does. It comes after the
           page has had it, so a handler that says the ordinary thing should
           not happen has already cleared the release and neither the field
           nor the link sees it. */
        /* What is drawn inside a button -- its icon, its words -- is the
           button as far as a click goes. */
        for (int up = node_under; up >= 0; up = doc.nodes[up].parent)
            if (lay_button_box(&doc, up)) { node_under = up; break; }
        if (in.released && node_under >= 0) {
            int ck = lay_control_kind(&doc, node_under);
            if (ck != CTL_NONE && ck != CTL_HIDDEN) {
                in.released = 0;
                bar.focused = 0;
                dirty = 1;

                if (ck == CTL_CHECK) {
                    field_set_checked(node_under, !field_checked(node_under));
                    focus_control(node_under);
                    jsdom_toggled(node_under);
                } else if (ck == CTL_RADIO) {
                    /* One of a name at a time, which is the only thing that
                       makes a radio button different from a checkbox. */
                    const char *nm = dom_attr(&doc, node_under, "name");
                    int mine = form_of(node_under);
                    if (nm && *nm)
                        for (int i = 0; i < doc.count; i++) {
                            if (lay_control_kind(&doc, i) != CTL_RADIO) continue;
                            if (form_of(i) != mine) continue;
                            const char *o = dom_attr(&doc, i, "name");
                            if (o && w_same(o, nm)) field_set_checked(i, 0);
                        }
                    field_set_checked(node_under, 1);
                    focus_control(node_under);
                    jsdom_toggled(node_under);
                } else if (ck == CTL_BUTTON) {
                    const char *t = dom_attr(&doc, node_under, "type");
                    focus_control(-1);
                    /* A plain button is the page's; only a submit button
                       sends its form, once the page has heard of it. */
                    int f = form_of(node_under);
                    if (!(t && (lay_same_fold(t, "reset") || lay_same_fold(t, "button")))
                        && !jsdom_submitting(f))
                        submit_form(f);
                } else {
                    focus_control(node_under);
                }
                if (jsdom_live() && jsdom_changed()) {
                    relayout(view_w - UI_PAD * 2);
                    over_link = lay_link_at(&page, dx, dy);
                }
            }
        }

        /* Clicking anywhere else puts the field down, so that what is typed
           next scrolls the page rather than going into a box nobody is
           looking at. */
        if (in.released && focus_node >= 0
            && (node_under < 0
                || lay_control_kind(&doc, node_under) == CTL_NONE))
            focus_control(-1);

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
        if (ui_field_draw(&s, &in, &t, bx, UI_PAD, field_w, &bar,
                          "type an address"))
            bar_fresh = 1;
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
        if (page.has_canvas) {
            /* The page's own background, behind everything on it, inside
               the well's edge. */
            if (t.modern) ui_round(&s, view_x + 1, view_y + 1, view_w + UI_SCROLL_W - 2, view_h - 2, 7,
                                   page.canvas, 255);
            else rect(&s, view_x + 2, view_y + 2, view_w + UI_SCROLL_W - 4, view_h - 4, page.canvas);
        }
        {
            /* Drawn into a surface that is only the rows inside the well, so
               everything is clipped at both edges by the surface's own. It
               used to start at the top of the window and stop at the bottom
               of the well, with the bevel put back over the top afterwards --
               but a line or a box half scrolled off the top, which is always
               drawn, landed on the toolbar above the bevel. */
            surface pg = { px + (u32)(view_y + 2) * (u32)w, w, view_h - 4 };
            draw_page(&pg, view_x + UI_PAD, 0, view_w, view_h - 4);
        }
        ui_sunken(&s, &t, view_x, view_y, view_w + UI_SCROLL_W, view_h);
        ui_scrollbar(&s, &t, view_x + view_w + 2, view_y + 2, view_h - 4,
                     scroll, view_h,
                     page.height < view_h ? view_h : page.height);

        win_commit(win);
        browser_wait(win);
    }

    win_close(win);
    exit(0);
}
