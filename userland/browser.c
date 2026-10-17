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
/* A sheet may be 4 MB: Ubuntu's is 1.1 MB, and cut at a megabyte the rule
   that hides its six notices was never read, so all six were drawn. The
   buffer is mapped, and the copy with its addresses made whole is as big
   as the biggest sheet read (sheet_abs), so a page pays for what it reads. */
#define CSS_MAX    (4 * 1024 * 1024)
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
 * them together, and the rest would not come. So a page may have 1024 files
 * and 16 megabytes -- X's is past 300 modules, and its imports past the
 * 256th were refused as names that could not be resolved -- and whether the
 * machine can run each one is still asked before it runs (jd_room_for). */
#define SCRIPT_MAX    (16 * 1024 * 1024)
#define SCRIPTS_MAX   1024
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
/* Where the page's own addresses start from: its first <base href> when it
   has one, resolved against here, else here. PeerTube writes its scripts as
   main-7RR2Q2IV.js under <base href="/client/en-US/">, and they were asked
   for at the site's root and came back as its page. Here stays the page's
   own address (what it is, what it tells a server it came from). */
static url_t    base;
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
 *
 * A page had room for forty-eight, and a news site's front page has a
 * hundred, most of them sent far bigger than they are drawn: kept as
 * decoded, three bytes a pixel, forty-eight of those were what the memory
 * would stand. So a picture is kept at no more than PIC_KEEP along either
 * side (pic_shrink), which is still sharp drawn the width of a wide window,
 * and its natural size is what the layout, the page's script and a canvas
 * are told.
 */
#define PICS_MAX 128
#define PIC_KEEP 1280

typedef struct {
    int     node;                /* which img element */
    picture pic;                 /* its pixels, or nothing: colour times alpha */
    u8     *alpha;               /* how much of it covers what is behind, or null
                                    for a picture with no clear parts (pic_merge) */
    u32     key;                 /* the address it came from, hashed */
    int     nat_w, nat_h;        /* its size as the file has it, before pic_shrink */
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

static const ddoc *drawn_from(void);
static int style_in_drawn(const ddoc *d, int node, cstyle *out);

/* A paint the sheet said (cstyle.fill_said), as svg.h reads one. */
static void paint_said(int said, u32 c, char *out) {
    static const char HEX[] = "0123456789abcdef";
    if (said == 2) { w_copy(out, 24, "none", 5); return; }
    if (said == 3) { w_copy(out, 24, "currentcolor", 13); return; }
    if (said != 1) { out[0] = 0; return; }
    out[0] = '#';
    for (int k = 0; k < 6; k++) out[1 + k] = HEX[(c >> (20 - 4 * k)) & 15];
    out[7] = 0;
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
    /* From what was laid out, where an icon sprite from another file has
       been put in (sprites_into) and a shadow tree is in its place. */
    const ddoc *from = drawn_from();
    /* And with the paint the page's sheet gives its root, which beats the
       root's own attributes. */
    const ddoc *src = it->node < from->count ? from : &doc;
    char fill[24] = "", stroke[24] = "";
    cstyle *st = (cstyle *)malloc(sizeof(cstyle));
    if (st && style_in_drawn(src, it->node, st)) {
        paint_said(st->fill_said, st->fill, fill);
        paint_said(st->stroke_said, st->stroke, stroke);
    }
    if (st) free(st);
    svg_root_paint(fill, stroke);
    svg_render_tree(src, it->node, it->w, it->h, &dr->pic, it->bg, it->color);
    svg_root_paint(0, 0);
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

/* Pictures asked for and not yet come. The page used to wait for every one
   before it was shown, a server at a time; it is shown first now and each
   picture drawn as it arrives (pictures_pump), a picture element's laid out
   again, a background's only drawn. `bg` is the background's slot, or -1
   for the element `node`; `ask` the request under way, plus one. */
#define PIC_WAIT (PICS_MAX + BGPICS_MAX)
#define PIC_ACCEPT "Accept: image/webp,image/png,image/jpeg,image/gif,image/svg+xml;q=0.9,*/*;q=0.1\r\n"

typedef struct {
    int  node, bg, ask;
    u32  key;                    /* the address, hashed (pic_key) */
    char url[URL_TEXT];
} pic_wait_t;

static pic_wait_t pic_wait[PIC_WAIT];
static int npic_wait;

/* Icon sprites in files of their own: <svg><use href="/icons.svg#name"/>,
 * how GitLab and a great many design systems draw every icon. A <use> could
 * name only an element of the page, and each icon was an empty box. The
 * file is fetched as a picture is (a waiting picture whose bg is -2 less
 * its slot here), kept as markup, and put into the copy of the page that is
 * laid out (sprites_into, from page_drawn): read in under <head>, its ids
 * given the slot's prefix so they cannot meet the page's, and each <use>
 * that names it pointed at its prefixed id, where svg.h's sv_use finds it. */
#define SPRITES_MAX 8
static struct { u32 key; char *text; int len; int state; } sprites[SPRITES_MAX];   /* state 0 asked, 1 here, 2 not */
static int nsprites, sprites_here;

static void sprites_drop(void) {
    for (int i = 0; i < nsprites; i++) if (sprites[i].text) free(sprites[i].text);
    nsprites = sprites_here = 0;
}

/* A sprite's file come: kept as it is, to be read into each copy. */
static int sprite_take(int slot, const u8 *body, int len) {
    if (slot < 0 || slot >= nsprites || len <= 0) return 0;
    char *t = (char *)malloc((u32)len + 1);
    if (!t) { sprites[slot].state = 2; return 0; }
    for (int i = 0; i < len; i++) t[i] = (char)body[i];
    t[len] = 0;
    sprites[slot].text = t;
    sprites[slot].len = len;
    sprites[slot].state = 1;
    sprites_here++;
    return 1;
}

/* An address hashed, to tell a picture's source from the one before it. */
static u32 pic_key(const char *s) {
    u32 h = 2166136261u;
    for (; *s; s++) h = (h ^ (u8)*s) * 16777619u;
    return h;
}

static void pic_wait_add(int node, int bg, const url_t *u) {
    if (npic_wait >= PIC_WAIT) return;
    pic_wait_t *w = &pic_wait[npic_wait++];
    w->node = node;
    w->bg = bg;
    w->ask = 0;
    url_text(u, w->url, sizeof(w->url));
    w->key = pic_key(w->url);
}

/* Elements whose picture would not come or not decode, and from where: not
   asked for again every time the page is laid out, unless their source
   changes. */
#define PIC_TRIED 64
static struct { int node; u32 key; } pic_tried[PIC_TRIED];
static int npic_tried;
static int pics_follow_on;               /* this page's pictures are fetched (build's want_sheets) */
static int pics_moved;                   /* one went to the element that took its old one's place */

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

/* A background's bytes made into what is drawn: 1 when it could be. */
static int bg_take(bgpic *b, const u8 *body, int len) {
    if (len > 3 && body[0] == 0xFF && body[1] == 0xD8) {
        picture p;
        if (jpeg_decode(body, len, &p) != JPG_OK) return 0;
        b->rgb = p.rgb;
        b->w = b->dw = p.w;
        b->h = b->dh = p.h;
        b->ok = 1;
    } else {
        picture k, wh;
        if (bg_raster(body, len, 0x000000, &k)) {
            if (!bg_raster(body, len, 0xFFFFFF, &wh)) { picture_free(&k); return 0; }
            b->ok = bg_merge(&k, &wh, b);
        } else {
            int at = 0;
            while (at < len && (body[at] == ' ' || body[at] == '\n' || body[at] == '\r'
                                || body[at] == '\t')) at++;
            if (at >= len || body[at] != '<') return 0;
            b->svg = (char *)malloc((u32)len + 1);
            if (!b->svg) return 0;
            for (int c = 0; c < len; c++) b->svg[c] = (char)body[c];
            b->svg[len] = 0;
            b->svglen = len;
            /* Made once at its own size, which is its size. */
            picture own;
            if (svg_render(b->svg, b->svglen, 0, 0, &own, 0xFFFFFF) != SVG_OK) return 0;
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
    return b->ok;
}

/* Every background the layout drew a box for: one written into the page
   made now, one at an address asked for (pictures_pump) and drawn when it
   comes, its slot kept as nothing until then. */
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

        if (w_starts_fold(src, "data:")) {
            int dn = pic_data_uri(src, cssbuf, CSS_MAX);
            if (dn > 0) bg_take(b, (const u8 *)cssbuf, dn);
        } else {
            url_t u;
            if (url_join(&base, src, &u)) pic_wait_add(-1, (int)(b - bgpics), &u);
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

static void ask_end(int id);

/* What was still coming for the last page, given up. */
static void pic_wait_drop(void) {
    for (int k = 0; k < npic_wait; k++)
        if (pic_wait[k].ask) ask_end(pic_wait[k].ask - 1);
    npic_wait = 0;
}

static void pics_drop(void) {
    pic_wait_drop();
    for (int i = 0; i < npics; i++) {
        picture_free(&pics[i].pic);
        if (pics[i].alpha) free(pics[i].alpha);
        pics[i].alpha = 0;
    }
    npics = 0;
    npic_sizes = 0;
    pics_skipped = 0;
    npic_tried = 0;
    pics_follow_on = 0;
    drawings_drop();
    bgpics_drop();
    sprites_drop();
}

static const picture *pic_of(int node) {
    for (int i = 0; i < npics; i++)
        if (pics[i].node == node && pics[i].pic.rgb) return &pics[i].pic;
    return 0;
}

/* A <video>'s latest frame, lent by the page's media (jsmedia.h) until the
   next pass. */
static const picture *video_of(int node) {
    static picture v;
    int w, h;
    const u8 *rgb = jsdom_media_picture(node, &w, &h);
    if (!rgb) return 0;
    v.w = w;
    v.h = h;
    v.rgb = (u8 *)rgb;
    return &v;
}

/* A canvas's bitmap and its alpha, lent by the page (jscanvas.h) until it is
   drawn on again. */
static const picture *canvas_of(int node, const u8 **alpha) {
    static picture c;
    int w, h;
    const u8 *rgb = jsdom_canvas_picture(node, &w, &h, alpha);
    if (!rgb) return 0;
    c.w = w;
    c.h = h;
    c.rgb = (u8 *)rgb;
    return &c;
}

/* The sizes the layout is given: the pictures', and each <video>'s once its
   first frame has said what it is. */
static limage lay_sizes[PICS_MAX + 8];

static int sizes_for_layout(void) {
    int n = 0;
    for (int i = 0; i < npic_sizes; i++) lay_sizes[n++] = pic_sizes[i];
    int node, w, h;
    for (int i = 0; n < PICS_MAX + 8 && jsdom_media_size(i, &node, &w, &h); i++) {
        lay_sizes[n].node = node;
        lay_sizes[n].w = w;
        lay_sizes[n].h = h;
        n++;
    }
    return n;
}

static const u8 *pic_alpha_of(int node) {
    for (int i = 0; i < npics; i++)
        if (pics[i].node == node && pics[i].pic.rgb) return pics[i].alpha;
    return 0;
}

/* A picture's own size, as its file has it, whatever it is kept at; the
   kept size for anything that is not a fetched picture. */
static void pic_natural(int node, const picture *p, int *w, int *h) {
    *w = p->w;
    *h = p->h;
    for (int i = 0; i < npics; i++)
        if (pics[i].node == node && &pics[i].pic == p) { *w = pics[i].nat_w; *h = pics[i].nat_h; return; }
}

/* A picture made f times smaller each way, each f by f block averaged, its
   alpha with it (its colours are already times their alpha, so the
   averages stay right). Left as it was when there is no memory for it. */
static void pic_shrink(picture *p, u8 **alpha, int f) {
    int w = (p->w + f - 1) / f, h = (p->h + f - 1) / f;
    u8 *rgb = (u8 *)malloc((u64)w * (u64)h * 3);
    u8 *a = *alpha ? (u8 *)malloc((u64)w * (u64)h) : 0;
    if (!rgb || (*alpha && !a)) {
        if (rgb) free(rgb);
        if (a) free(a);
        return;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 r = 0, g = 0, b = 0, al = 0, n = 0;
            for (int yy = y * f; yy < y * f + f && yy < p->h; yy++)
                for (int xx = x * f; xx < x * f + f && xx < p->w; xx++) {
                    const u8 *q = p->rgb + ((u64)yy * (u64)p->w + (u64)xx) * 3;
                    r += q[0];
                    g += q[1];
                    b += q[2];
                    if (a) al += (*alpha)[(u64)yy * (u64)p->w + (u64)xx];
                    n++;
                }
            u8 *o = rgb + ((u64)y * (u64)w + (u64)x) * 3;
            o[0] = (u8)(r / n);
            o[1] = (u8)(g / n);
            o[2] = (u8)(b / n);
            if (a) a[(u64)y * (u64)w + (u64)x] = (u8)(al / n);
        }
    free(p->rgb);
    p->rgb = rgb;
    p->w = w;
    p->h = h;
    if (a) { free(*alpha); *alpha = a; }
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
static int page_active = -1;      /* what the page was last known to give the keyboard to */

/* The same editor the address bar uses, pointed at whichever control has the
   keyboard. One of them, rather than one per field: only one can be typed
   into, and the value is written back to the document on every keystroke, so
   there is nothing to keep for the others. */
static char     focus_buf[1024];
static ui_field focus_field = { focus_buf, sizeof(focus_buf), 0, 0, 0, 0 };

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
    focus_field.secret = ck == CTL_PASSWORD;
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

/* <meta name="referrer">: the policy the page's own requests go under, from
   here on (fetch.h, web_policy_of). */
static void meta_referrer(void) {
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_META) continue;
        const char *nm = dom_attr(&doc, i, "name");
        if (!nm || !lay_same_fold(nm, "referrer")) continue;
        int p = web_policy_of(dom_attr(&doc, i, "content"));
        if (p >= 0) web_referrer_policy = p;
    }
}

static int meta_refresh(char *out, int cap) {
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT) continue;
        if (doc.nodes[i].tag != T_META) continue;

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

/* Who sent the browser to the next page, for its Referer (fetch.h): the page
   it was on when a link, a form or the page's own script did, under that
   page's policy; nothing for an address typed or the history buttons. What
   was sent for the page on the screen is what its document.referrer says. */
static char nav_from[URL_TEXT];
static int  nav_policy;
static char page_referrer[URL_TEXT];

static void nav_from_here(void) {
    w_copy(nav_from, sizeof(nav_from), web_referrer_from, sizeof(nav_from));
    nav_policy = web_referrer_policy;
}
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
        if (!url_join(&base, action, &target)) return;
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

/* The status in two halves around what it says of the pictures, which
   changes as they come. */
static char status_head[sizeof(status)], status_tail[sizeof(status)];
static int  pics_said;                  /* the status has the halves to compose */

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

/* Whether an answer is worth asking for again, once: no answer, a server
   too busy to give one, or one cut short. One that said it was too busy is
   asked again after the pause it named (Retry-After), half a second if it
   named none and never more than two: the reader is waiting. */
static int ask_again(int rc, const response_t *r) {
    if (!(rc < 0 || rc == 429 || rc >= 500 || r->cut)) return 0;
    if (rc != 429 && rc != 503) return 1;
    int ms = 500;
    const char *h = r->head;
    for (int i = 0; h && i + 12 < r->hlen; i++) {
        if ((i == 0 || h[i - 1] == '\n') && w_starts_fold(h + i, "Retry-After:")) {
            const char *v = h + i + 12;
            while (*v == ' ') v++;
            if (*v >= '0' && *v <= '9') {
                int s = 0;
                while (*v >= '0' && *v <= '9' && s < 100) s = s * 10 + (*v++ - '0');
                ms = s * 1000;
            }
            break;
        }
    }
    if (ms > 2000) ms = 2000;
    sleep_ms(ms);
    return 1;
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
static int sheet_abs_cap;
static void css_parse_sheet_in(const url_t *base, const char *css, int len, int lo, int hi, const char *scope) {
    int need = len + len / 4 + 4096;
    if (need > sheet_abs_cap) {
        free(sheet_abs);
        sheet_abs = (char *)malloc((u64)need);
        sheet_abs_cap = sheet_abs ? need : 0;
    }
    int n = sheet_abs ? css_urls_from(base, css, len, sheet_abs, sheet_abs_cap) : -1;
    if (n >= 0) css_parse_style(&sheet, sheet_abs, n, lo, hi, scope);
    else css_parse_style(&sheet, css, len, lo, hi, scope);
}

static void css_parse_sheet(const url_t *base, const char *css, int len, int lo, int hi) {
    css_parse_sheet_in(base, css, len, lo, hi, 0);
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

static int ahead_take(const url_t *u, const char **out);

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
        if (!url_join(&base, href, &u)) { (*skipped)++; source_add(i, r0, 0); continue; }
        char whole[URL_TEXT];
        url_text(&u, whole, (int)sizeof(whole));
        if (sheet_seen(whole)) { source_add(i, r0, 1); continue; }

        /* Asked for already, with the page's other files (files_ahead). */
        const char *text = 0;
        int len = ahead_take(&u, &text);
        if (len <= 0) {
            response_t r;
            int rc = web_get(&u, cssbuf, CSS_MAX, &r);
            /* Asked for again, once, when the answer was no answer, the server
               said it was busy or it stopped short: one sheet that failed on the
               way is a page drawn with none of its style (Wikipedia's, after a
               burst of fetching). */
            if (ask_again(rc, &r)) rc = web_get(&u, cssbuf, CSS_MAX, &r);
            /* An empty sheet is a sheet with no rules in it: Mastodon serves
               its custom one so, and it was told as one not read. */
            if (rc < 200 || rc >= 300 || r.cut) { (*skipped)++; source_add(i, r0, 0); continue; }
            text = r.body;
            len = r.len;
        }
        gather_imports(&u, text, len, fetched);
        css_parse_sheet(&u, text, len, lo, hi);
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

/* --- shadow trees a script attached -------------------------------------------------
 *
 * The page is laid out from a copy with each in place (dom.h, dom_flat;
 * page_drawn), and each tree's style elements and adopted sheets reach that
 * tree alone, scoped to a name made from what they are: every copy of one
 * component adopts the same sheets, so the copies share a name and one
 * reading of them, where a name each would have read the component's sheets
 * again for every copy of it on the page. A tree with none has no name.
 * As many as jsdom keeps (JD_SHADOWS): with room for 1024, Rotten Tomatoes'
 * header, attached after a thousand other components, was not drawn as a
 * tree, and the red bar its shadow sheet gives it was not there. */
#define TREES_MAX 8192
static int trees_n;
static int tree_host[TREES_MAX], tree_root[TREES_MAX];
static unsigned tree_key[TREES_MAX];          /* what its sheets are, in one number; 0 for none */
static char tree_scope[TREES_MAX][DOM_SCOPE];
static unsigned trees_dom_seen, trees_css_seen;
static int trees_known;

static unsigned tree_sources(int s, int root) {
    unsigned h = 2166136261u;
    int any = 0;
    for (int i = doc.nodes[root].first; i >= 0; i = dom_next(&doc, i, root)) {
        if (doc.nodes[i].kind != DN_ELEMENT) continue;
        int tag = doc.nodes[i].tag;
        if (tag != T_STYLE && tag != T_LINK) continue;
        h = (h ^ source_hash(i)) * 16777619u;
        /* One a script has changed is this tree's alone. */
        if (jsdom_sheet_override(i, 0, 0)) h = (h ^ (unsigned)i ^ 0x5bd1e995u) * 16777619u;
        any = 1;
    }
    const char *t;
    int n;
    u32 id;
    for (int k = 0; jsdom_shadow_adopted(s, k, &t, &n, &id); k++) {
        h = (h ^ id) * 16777619u;
        any = 1;
    }
    return any ? (h ? h : 1) : 0;
}

/* The trees as the scripts have left them, worked out again only when the
   tree or a sheet has changed since. */
static void trees_gather(void) {
    unsigned dv = jsdom_dom_version(), cv = jsdom_css_version();
    if (trees_known && dv == trees_dom_seen && cv == trees_css_seen) return;
    trees_known = 1;
    trees_dom_seen = dv;
    trees_css_seen = cv;
    trees_n = 0;
    int h, r;
    for (int s = 0; trees_n < TREES_MAX && jsdom_shadow(s, &h, &r); s++) {
        unsigned key = tree_sources(s, r);
        tree_host[trees_n] = h;
        tree_root[trees_n] = r;
        tree_key[trees_n] = key;
        char *sc = tree_scope[trees_n];
        sc[0] = 0;
        if (key) {
            static const char hex[] = "0123456789abcdef";
            sc[0] = 'f';
            for (int k = 0; k < 8; k++) sc[1 + k] = hex[(key >> (28 - 4 * k)) & 15];
            sc[9] = 0;
        }
        trees_n++;
    }
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
    trees_gather();
    for (int t = 0; t < trees_n; t++) h = (h ^ tree_key[t]) * 16777619u;
    return h * 31u + (unsigned)trees_n;
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
    if (ask_again(rc, &r)) rc = web_get(u, cssbuf, CSS_MAX, &r);
    /* Empty is a sheet with no rules, not one that failed: its link is told
       load, not error. */
    int ok = rc >= 200 && rc < 300 && r.len >= 0 && !r.cut;
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
    if (!url_join(&base, href, &u)) return -1;
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
            ok = url_join(&base, href, &u) && link_text(&u, &t) >= 0;
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

/* What one style or link element gives, read into the sheet: for the
   document, or for the shadow tree `scope` names. k is its place in the
   base's sources, or -1. */
static void sheet_read(int i, int k, const char *scope) {
    int lo, hi;
    if (!sheet_media(i, &lo, &hi)) return;
    const char *ov = 0;
    int ovn = 0, has_ov = jsdom_sheet_override(i, &ov, &ovn);
    if (doc.nodes[i].tag == T_STYLE) {
        if (has_ov) { css_parse_style(&sheet, ov, ovn, lo, hi, scope); return; }
        for (int t = doc.nodes[i].first; t >= 0; t = doc.nodes[t].next)
            if (doc.nodes[t].kind == DN_TEXT && doc.nodes[t].text >= 0) {
                const char *s = doc.arena + doc.nodes[t].text;
                css_parse_style(&sheet, s, w_len(s), lo, hi, scope);
            }
        return;
    }
    const char *rel = dom_attr(&doc, i, "rel"), *href = dom_attr(&doc, i, "href");
    if (!rel || !href || !*href || !rel_is_sheet(rel) || dom_attr(&doc, i, "disabled")) return;
    if (k >= 0 && sources[k].ok == -2) return;
    url_t u;
    if (!url_join(&base, href, &u)) return;
    if (has_ov) { css_parse_sheet_in(&u, ov, ovn, lo, hi, scope); return; }
    const char *text;
    int n = link_text(&u, &text);
    if (n > 0) css_parse_sheet_in(&u, text, n, lo, hi, scope);
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
        sheet_read(i, k, dom_attr(&doc, i, "data-zs"));
    }
    const char *t;
    int n;
    for (int k = 0; jsdom_adopted(k, &t, &n); k++) if (n > 0) css_parse_in(&sheet, t, n, -1, -1);

    /* Each shadow tree's own, for it alone, read once for the trees that
       share them. */
    trees_gather();
    for (int tr = 0; tr < trees_n; tr++) {
        if (!tree_key[tr]) continue;
        int again = 0;
        for (int u = 0; u < tr && !again; u++) again = tree_key[u] == tree_key[tr];
        if (again) continue;
        int r = tree_root[tr];
        for (int i = doc.nodes[r].first; i >= 0; i = dom_next(&doc, i, r))
            if (doc.nodes[i].kind == DN_ELEMENT && (doc.nodes[i].tag == T_STYLE || doc.nodes[i].tag == T_LINK))
                sheet_read(i, -1, tree_scope[tr]);
        for (int k = 0; jsdom_shadow_adopted(tr, k, &t, &n, 0); k++)
            if (n > 0) css_parse_style(&sheet, t, n, -1, -1, tree_scope[tr]);
    }

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

static int ahead_take(const url_t *u, const char **out);

static int fetch_script(const char *src, const char **out) {
    if (scripts_outside >= SCRIPTS_MAX || scripts_bytes >= SCRIPTS_BYTES || !scriptbuf) return 0;

    url_t u;
    if (!url_join(&base, src, &u)) return 0;

    /* Asked for already, with the page's other files (scripts_ahead). */
    int got = ahead_take(&u, out);
    if (got > 0) {
        if (scripts_bytes + got > SCRIPTS_BYTES) return 0;
        scripts_outside++;
        scripts_bytes += got;
        return got;
    }

    response_t r;
    int rc = web_get(&u, scriptbuf, SCRIPT_MAX, &r);
    /* Asked for again, once, as a sheet is: PeerTube's app is seventy-odd
       modules, every one of which must come, and one dropped by a busy
       server was the whole page not drawn. */
    if (ask_again(rc, &r)) rc = web_get(&u, scriptbuf, SCRIPT_MAX, &r);
    if (rc >= 200 && rc < 300 && r.len == 0 && !r.cut && scripts_outside < SCRIPTS_MAX) {
        /* An empty file is a script that does nothing, and a module with
           nothing in it a module; nought is what a file that would not come
           is told by, so it is handed over as one space. */
        scripts_outside++;
        *out = " ";
        return 1;
    }
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
/* The page's own lines, and Origin, which the standard sends with a
   request to another site and with any that is not a GET or a HEAD. */
#define REQUEST_LINES (4096 + URL_TEXT)
static void request_lines(const url_t *u, const char *method, const char *headers, char *extra) {
    int sizeof_extra = REQUEST_LINES;
    w_copy(extra, sizeof_extra, headers ? headers : "", sizeof_extra);
    int cross = u->secure != here.secure || u->port != here.port || !w_same_fold(u->host, here.host);
    int plain = !method || w_same_fold(method, "GET") || w_same_fold(method, "HEAD");
    if (cross || !plain) {
        char origin[URL_TEXT];
        int w = 0;
        const char *from = web_referrer_from;
        if (w_starts_fold(from, "http://") || w_starts_fold(from, "https://")) {
            int slashes = 0;
            for (; from[w] && w < (int)sizeof(origin) - 1; w++) {
                if (from[w] == '/' && ++slashes == 3) break;
                if (from[w] == '?' || from[w] == '#') break;
                origin[w] = from[w];
            }
        } else {
            w_copy(origin, sizeof(origin), "null", sizeof(origin));
            w = 4;
        }
        origin[w] = 0;
        int e = w_len(extra);
        if (e + w + 12 < sizeof_extra) {
            w_copy(extra + e, sizeof_extra - e, "Origin: ", sizeof_extra - e);
            e += 8;
            w_copy(extra + e, sizeof_extra - e, origin, sizeof_extra - e);
            e += w;
            w_copy(extra + e, sizeof_extra - e, "\r\n", sizeof_extra - e);
        }
    }
}

/* Each request a page's scripts make, as it starts (a status of nought) and
   as it ends, for a host tool to hear (scriptdump's "net"); on the machine it
   is nothing. */
#ifndef ASK_SEEN
#define ASK_SEEN(method, url, status) ((void)0)
#endif

static int do_request(const char *method, const char *url, const char *body, int blen,
                      const char *type, const char *headers, jd_reply *out) {
    static char landed[URL_TEXT];
    static char ctype[64];
    out->body = 0;
    out->len = 0;
    out->status = 0;
    out->type = 0;
    out->url = 0;
    out->head = 0;
    out->hlen = 0;
    if (asks_made >= ASKS_MAX || !replybuf) return 0;

    url_t u;
    if (!url_join(&base, url, &u)) return 0;
    static char extra[REQUEST_LINES];
    request_lines(&u, method, headers, extra);

    response_t r;
    web_method = method && !w_same_fold(method, "GET") ? method : 0;
    web_body_type = type;
    web_body_len = body ? blen : -1;
    web_extra = extra;
    web_accept = "*/*";
    int rc = body ? web_post(&u, body, replybuf, REPLY_MAX, &r)
                  : web_get(&u, replybuf, REPLY_MAX, &r);
    web_method = 0;
    web_body_type = 0;
    web_body_len = -1;
    web_extra = 0;
    web_accept = 0;

    asks_made++;
    out->status = rc;
    ASK_SEEN(method ? method : "GET", url, rc);
    if (rc <= 0) return 0;
    /* Where it ended up, after any redirect (web_send follows them in u),
       and what the server said it was. */
    url_text(&u, landed, sizeof(landed));
    w_copy(ctype, sizeof(ctype), r.ctype, sizeof(ctype));
    out->url = landed;
    out->type = ctype;
    out->head = r.head;
    out->hlen = r.hlen;
    if (r.len <= 0) return 0;
    out->body = r.body;
    out->len = r.len;
    return r.len;
}

/* --- a page's requests, answered while it goes on --------------------------
 *
 * The same requests, each started on one pass and asked on the next ones
 * for what has come (fetch.h, web_job), so the page's timers, its frames,
 * its video and the reader's input go on while an answer arrives: done the
 * way above, a video's next stretch or a script's fetch() held the whole
 * browser for as long as it took. A few at a time, each slot keeping its
 * connection between requests, since the machine has six sockets in all;
 * one asked for while every slot is busy waits for a later pass. A slot
 * whose connection is to where a new request goes is chosen first. */
#define ASK_SLOTS 3

typedef struct {
    int     used;                       /* a request is under way, or answered and not yet ended */
    web_job job;
    char    method[16], type[96];
    char    extra[REQUEST_LINES];
    char   *body;                       /* a copy: the page's string may go before a retry needs it */
    char    landed[URL_TEXT], ctype[64];
    int     idle_since;                 /* the tick it was last ended */
} ask_t;

static ask_t asks[ASK_SLOTS];
static int asks_ready;

static void asks_init(void) {
    if (asks_ready) return;
    for (int i = 0; i < ASK_SLOTS; i++) web_job_init(&asks[i].job);
    asks_ready = 1;
}

/* A connection kept by a slot nobody is using, hung up once it has been
   idle for five seconds -- or now, when a connection is wanted and every
   socket is taken. The machine's six are for everything on it: a page's
   WebSockets, the next page, another program. */
#define ASK_IDLE 500
static void asks_hang_up_idle(int now_too) {
    if (!asks_ready) return;
    int now = ticks();
    for (int i = 0; i < ASK_SLOTS; i++)
        if (!asks[i].used && asks[i].job.sock >= 0 && (now_too || now - asks[i].idle_since > ASK_IDLE))
            web_job_hangup(&asks[i].job);
}

static void asks_tidy(void) { asks_hang_up_idle(0); }

static int ask_begin(const char *method, const char *url, const char *body, int blen,
                     const char *type, const char *headers, int max, int counted) {
    asks_init();
    if (counted && asks_made >= ASKS_MAX) return -2;
    url_t u;
    if (!url_join(&base, url, &u)) return WEB_ERR_SCHEME;
    int pick = -1;
    for (int i = 0; i < ASK_SLOTS; i++) {
        if (asks[i].used) continue;
        if (web_job_at(&asks[i].job, &u)) { pick = i; break; }
        if (pick < 0 || (asks[pick].job.sock >= 0 && asks[i].job.sock < 0)) pick = i;
    }
    if (pick < 0) return -1;
    ask_t *a = &asks[pick];
    w_copy(a->method, sizeof(a->method), method ? method : "GET", sizeof(a->method));
    w_copy(a->type, sizeof(a->type), type ? type : "", sizeof(a->type));
    request_lines(&u, method, headers, a->extra);
    if (a->body) free(a->body);
    a->body = 0;
    if (body) {
        a->body = (char *)malloc((u64)blen + 1);
        if (!a->body) return WEB_ERR_BUSY;
        for (int i = 0; i < blen; i++) a->body[i] = body[i];
        a->body[blen] = 0;
    }
    if (counted) asks_made++;
    a->used = 1;
    int rc = web_job_start(&a->job, &u, a->method, a->body, blen, type ? a->type : 0, a->extra, "*/*", max);
    if (rc == WEB_ERR_BUSY) {
        asks_hang_up_idle(1);
        rc = web_job_start(&a->job, &u, a->method, a->body, blen, type ? a->type : 0, a->extra, "*/*", max);
    }
    if (rc < 0) {
        a->used = 0;
        return rc < -1 ? rc : -2;
    }
    ASK_SEEN(a->method, url, 0);
    return pick;
}

/* A script element's file, which a page's script put in (jsnet.h,
   jsdom_script_asks_with): counted as the scripts the page was written with
   are, against SCRIPTS_MAX and SCRIPTS_BYTES, and not as a request. */
static int script_ask_start(const char *src) {
    if (scripts_outside >= SCRIPTS_MAX || scripts_bytes >= SCRIPTS_BYTES) return -2;
    return ask_begin("GET", src, 0, 0, 0, 0, SCRIPT_MAX, 0);
}

static int script_ask_poll(int id, const char **text) {
    if (id < 0 || id >= ASK_SLOTS || !asks[id].used) return WEB_ERR_EMPTY;
    ask_t *a = &asks[id];
    int st = web_job_step(&a->job);
    if (st < 0) {
        url_text(&a->job.u, a->landed, sizeof(a->landed));
        ASK_SEEN(a->method, a->landed, st);
    }
    if (st <= 0) return st;
    response_t *r = &a->job.r;
    url_text(&a->job.u, a->landed, sizeof(a->landed));
    ASK_SEEN(a->method, a->landed, r->status);
    /* Half a script is a syntax error somewhere in its middle, as for the
       page's own (fetch_script). */
    if (r->status < 200 || r->status >= 300 || r->len <= 0 || r->truncated || r->cut
        || scripts_bytes + r->len > SCRIPTS_BYTES)
        return WEB_ERR_EMPTY;
    scripts_outside++;
    scripts_bytes += r->len;
    *text = r->body;
    return r->len;
}

/* A page's script's request, counted against what a page may ask. */
static int ask_start(const char *method, const char *url, const char *body, int blen,
                     const char *type, const char *headers, int max) {
    return ask_begin(method, url, body, blen, type, headers, max, 1);
}

static int ask_poll(int id, jd_reply *out) {
    if (id < 0 || id >= ASK_SLOTS || !asks[id].used) return WEB_ERR_EMPTY;
    ask_t *a = &asks[id];
    int st = web_job_step(&a->job);
    if (st < 0) {
        url_text(&a->job.u, a->landed, sizeof(a->landed));
        ASK_SEEN(a->method, a->landed, st);
    }
    if (st <= 0) return st;
    response_t *r = &a->job.r;
    url_text(&a->job.u, a->landed, sizeof(a->landed));
    ASK_SEEN(a->method, a->landed, r->status);
    w_copy(a->ctype, sizeof(a->ctype), r->ctype, sizeof(a->ctype));
    out->status = r->status;
    out->url = a->landed;
    out->type = a->ctype;
    out->head = r->head;
    out->hlen = r->hlen;
    out->body = r->len > 0 ? r->body : 0;
    out->len = r->len > 0 ? r->len : 0;
    return 1;
}

static void ask_end(int id) {
    if (id < 0 || id >= ASK_SLOTS || !asks[id].used) return;
    web_job_stop(&asks[id].job);
    asks[id].used = 0;
    asks[id].idle_since = ticks();
}

/* A page left: whatever it had under way given up, and the connections
   with it, which were the old page's to keep. */
static void asks_drop_all(void) {
    if (!asks_ready) return;
    for (int i = 0; i < ASK_SLOTS; i++) {
        web_job_hangup(&asks[i].job);
        web_job_stop(&asks[i].job);
        asks[i].used = 0;
    }
}

/* --- a page's script files, asked for together --------------------------------
 *
 * The scripts a page is written with run one after another in the order
 * written, and each one's file was asked for when its turn came and waited
 * for: Imgur's forty-five took seven seconds on the host, one round trip
 * after another. A browser reads ahead and asks for them together; so does
 * this now. Before the first runs, every script element with a src that
 * will run is put in a list, and each pass asks for as many as there are
 * ways out (the asks above, which keep their connections, so a site's files
 * come over one or two); a script whose turn comes takes its file from the
 * list (fetch_script), waiting only for its own, and the rest go on coming
 * meanwhile. What has not been taken when the scripts are done is let go.
 * A file that would not come this way is asked for the old way, once. */
#define AHEAD_MAX 384

typedef struct {
    char  url[URL_TEXT];
    int   state;                        /* 0 to ask, 1 asked (slot + 1 in ask), 2 here, 3 would not come, 4 taken */
    int   ask, max;                     /* and the most its answer may be: a script's, or a sheet's */
    char *text;
    int   len;
} ahead_t;

static ahead_t ahead[AHEAD_MAX];
static int nahead;
static char *ahead_given;              /* the text fetch_script last handed out, freed at the next */

static void ahead_drop(void) {
    for (int i = 0; i < nahead; i++) {
        if (ahead[i].state == 1) ask_end(ahead[i].ask - 1);
        if (ahead[i].text) free(ahead[i].text);
        ahead[i].text = 0;
    }
    nahead = 0;
    if (ahead_given) free(ahead_given);
    ahead_given = 0;
}

/* One pass: what has come taken out of its slot, and what waits asked for
   while a slot is free, in the order written. */
static void ahead_pump(void) {
    for (int i = 0; i < nahead; i++) {
        ahead_t *a = &ahead[i];
        if (a->state != 1) continue;
        ask_t *k = &asks[a->ask - 1];
        int st = web_job_step(&k->job);
        if (st == 0) continue;
        response_t *r = &k->job.r;
        ASK_SEEN("GET", a->url, st > 0 ? r->status : st);
        a->state = 3;
        if (st > 0 && r->status >= 200 && r->status < 300 && r->len > 0 && !r->truncated && !r->cut) {
            a->text = (char *)malloc((u64)r->len + 1);
            if (a->text) {
                for (int b = 0; b < r->len; b++) a->text[b] = r->body[b];
                a->text[r->len] = 0;
                a->len = r->len;
                a->state = 2;
            }
        }
        ask_end(a->ask - 1);
        a->ask = 0;
    }
    for (int i = 0; i < nahead; i++) {
        if (ahead[i].state != 0) continue;
        int id = ask_begin("GET", ahead[i].url, 0, 0, 0, 0, ahead[i].max, 0);
        if (id == -1) break;
        if (id < 0) { ahead[i].state = 3; continue; }
        ahead[i].state = 1;
        ahead[i].ask = id + 1;
    }
}

static void ahead_add(const url_t *u, int max) {
    if (nahead >= AHEAD_MAX) return;
    url_text(u, ahead[nahead].url, sizeof(ahead[nahead].url));
    ahead[nahead].state = 0;
    ahead[nahead].ask = 0;
    ahead[nahead].max = max;
    ahead[nahead].text = 0;
    ahead[nahead].len = 0;
    nahead++;
}

/* The page's files listed as soon as it is read, and the first ones asked
   for, so that what comes next comes while the sheets are read: its style
   sheets (when they are wanted), as gather_linked_sheets will want them,
   then the scripts the parser made that will run, classic or a module's
   first file, with a src that is an address (a data: or blob: one carries
   its own). */
static void files_ahead(int want_sheets) {
    ahead_drop();
    for (int i = 0; want_sheets && i < doc.count && nahead < AHEAD_MAX; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_LINK || in_template(i)) continue;
        const char *rel = dom_attr(&doc, i, "rel");
        const char *href = dom_attr(&doc, i, "href");
        int lo, hi;
        if (!rel || !href || !*href || !rel_is_sheet(rel)) continue;
        if (!sheet_media(i, &lo, &hi) || dom_attr(&doc, i, "disabled")) continue;
        url_t u;
        if (url_join(&base, href, &u)) ahead_add(&u, CSS_MAX);
    }
    for (int i = 0; i < doc.count && nahead < AHEAD_MAX; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_SCRIPT) continue;
        const char *s = dom_attr(&doc, i, "src");
        if (!s || !*s || w_starts_fold(s, "data:") || w_starts_fold(s, "blob:")) continue;
        if (dom_attr(&doc, i, "nomodule")) continue;
        const char *type = dom_attr(&doc, i, "type");
        if (type && *type && !w_same_fold(type, "module") && !w_starts_fold(type, "text/javascript")
            && !w_starts_fold(type, "application/javascript") && !w_same_fold(type, "text/ecmascript"))
            continue;
        url_t u;
        if (url_join(&base, s, &u)) ahead_add(&u, SCRIPT_MAX);
    }
    /* And the modules the page says it will import (modulepreload, or a
       script it preloads): a module's own imports are known only once it
       has been read, and these are what it will ask for first. */
    for (int i = 0; i < doc.count && nahead < AHEAD_MAX; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_LINK || in_template(i)) continue;
        const char *rel = dom_attr(&doc, i, "rel"), *href = dom_attr(&doc, i, "href"), *as = dom_attr(&doc, i, "as");
        if (!rel || !href || !*href) continue;
        if (!w_same_fold(rel, "modulepreload") && !(w_same_fold(rel, "preload") && as && w_same_fold(as, "script")))
            continue;
        url_t u;
        if (url_join(&base, href, &u)) ahead_add(&u, SCRIPT_MAX);
    }
    ahead_pump();
}

/* A file a module imports (jsmod.h, jd_want_script), put on the list as
   soon as the module naming it has been read, and asked for while the
   modules before it are still coming. */
static void script_want(const char *src) {
    if (scripts_outside >= SCRIPTS_MAX || scripts_bytes >= SCRIPTS_BYTES || nahead >= AHEAD_MAX) return;
    url_t u;
    if (!url_join(&base, src, &u)) return;
    char t[URL_TEXT];
    url_text(&u, t, sizeof(t));
    for (int i = 0; i < nahead; i++) if (w_same(ahead[i].url, t)) return;
    ahead_add(&u, SCRIPT_MAX);
    ahead_pump();
}

/* A file for fetch_script from the list, waiting for it if it is still
   coming: its length, or 0 when it is not in the list or would not come
   this way. The text is good until the next call. */
static int ahead_take(const url_t *u, const char **out) {
    if (!nahead) return 0;
    char want[URL_TEXT];
    url_text(u, want, sizeof(want));
    int i = 0;
    for (; i < nahead; i++) if (ahead[i].state != 4 && w_same(ahead[i].url, want)) break;
    if (i == nahead) return 0;
    int since = ticks();
    while (ahead[i].state == 0 || ahead[i].state == 1) {
        ahead_pump();
        /* One not yet asked for while none of the list's are on their way,
           with no way out free for a fifth of a second, is fetched the old
           way: the asks are all the page's own requests then, which are taken
           back only between passes, and a script waiting here inside import()
           is in the middle of one. Mastodon's columns, each an import(),
           waited for ever. Behind the list's own, it waits its turn. */
        if (ahead[i].state == 0 && ticks() - since > 20) {
            int going = 0;
            for (int k = 0; k < nahead && !going; k++) going = ahead[k].state == 1;
            if (!going) { ahead[i].state = 4; return 0; }
        }
        if (ahead[i].state == 0 || ahead[i].state == 1) sleep_ms(5);
    }
    if (ahead[i].state != 2) { ahead[i].state = 4; return 0; }
    if (ahead_given) free(ahead_given);
    ahead_given = ahead[i].text;
    ahead[i].text = 0;
    ahead[i].state = 4;
    *out = ahead_given;
    return ahead[i].len;
}

/* The page as it is drawn: itself, or when a script has attached shadow
   trees or the sheet has rules for ::before and ::after, a copy with the
   trees in place (dom.h, dom_flat) and a box for each ::before and ::after
   that has content (layout.h, lay_generated). The copy shares the page's
   strings and writes its own where the page's next string will go, so it
   is made again once the page has changed or added a string; `fresh`
   makes it again anyway, as every layout does (a hover can change which
   rules apply). */
static ddoc *flat_mem;
static unsigned flat_dom_seen;
static int flat_used = -1, flat_count;
static const ddoc *laid;             /* what the last layout walked */

static const ddoc *drawn_from(void) { return laid ? laid : &doc; }

static void sprites_into(ddoc *d);

static const ddoc *page_drawn(int fresh) {
    trees_gather();
    if (!trees_n && !index_.npe && !sprites_here) return &doc;
    if (!flat_mem) flat_mem = (ddoc *)map(sizeof(ddoc), PROT_READ | PROT_WRITE);
    if (!flat_mem) return &doc;
    if (fresh || flat_used != doc.used || flat_count != doc.count || flat_dom_seen != jsdom_dom_version()) {
        dom_flat(flat_mem, &doc, tree_host, tree_root, (const char (*)[DOM_SCOPE])tree_scope, trees_n);
        if (sprites_here) sprites_into(flat_mem);
        lay_generated(flat_mem, &sheet, &index_, &match, inl);
        flat_used = doc.used;
        flat_count = doc.count;
        flat_dom_seen = jsdom_dom_version();
    }
    return flat_mem;
}

/* --- what a page's scripts may ask the browser -----------------------------
 *
 * Where the layout put an element, which is every box it drew for the
 * element or for anything inside it, run together: what
 * getBoundingClientRect answers with. In the page's own terms, down the
 * document; jsdom.h takes the scroll off. An element nothing was drawn for
 * has no box, and says so. */
static u8 box_mark[DOM_NODES / 8];

/* A node of what was laid out as the page's own: a box made for a ::before
   or ::after (layout.h, lay_generated), or the words in one, is its host's,
   since the page has no such element for a script or a click to be told of. */
static int page_node(int n) {
    while (n >= doc.count && laid && n < laid->count) n = laid->nodes[n].parent;
    return n < doc.count ? n : -1;
}

/* What the layout drew at a point of the page, for elementFromPoint. */
static int node_at_point(int x, int y) { return page_node(lay_node_at(&page, x, y)); }

static int box_of(int node, int *x, int *y, int *w, int *h) {
    if (node < 0 || node >= doc.count) return 0;
    /* A component's box holds what its shadow tree drew. */
    const ddoc *d = laid && node < laid->count ? laid : &doc;
    for (int i = node; i >= 0; i = dom_next(d, i, node)) box_mark[i >> 3] |= (u8)(1 << (i & 7));
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
    for (int i = node; i >= 0; i = dom_next(d, i, node)) box_mark[i >> 3] &= (u8)~(1 << (i & 7));
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

static int style_in_drawn(const ddoc *d, int node, cstyle *out);

static int computed_style(int node, cstyle *out) {
    if (node < 0 || node >= doc.count || doc.nodes[node].kind != DN_ELEMENT) return 0;
    /* From the page as it is drawn, so that one in a shadow tree has its
       tree's rules and inherits from where it is shown. */
    sheets_follow_if_changed();
    return style_in_drawn(page_drawn(0), node, out);
}

/* An element's ::before (pe 1) or ::after (2), for getComputedStyle with a
   pseudo-element: the style of the box made for it (layout.h,
   lay_generated) and what its content says, as the sheet wrote it. 0 when
   no box was made, which is content none. */
static int pseudo_style(int node, int pe, cstyle *out, char *content, int cap) {
    if (node < 0 || node >= doc.count || doc.nodes[node].kind != DN_ELEMENT || cap < 1) return 0;
    sheets_follow_if_changed();
    const ddoc *d = page_drawn(0);
    int g = pe == 1 ? d->nodes[node].first : d->nodes[node].last;
    if (g < 0 || css_pseudo_node(d, g) != pe) return 0;
    chit hits[CSS_HITS];
    int nh = css_collect_pe(&sheet, &index_, d, node, pe, &match, 0, hits);
    const char *val = "";
    for (int i = 0; i < nh; i++) {
        const crule *r = &sheet.rules[hits[i].rule];
        for (int k = 0; k < r->decl_n; k++) {
            const cdecl *dc = &sheet.decls[r->decl_at + k];
            if (CSS_PROP(dc->prop) == P_CONTENT) val = sheet.text + dc->value;
        }
    }
    w_copy(content, cap, val, w_len(val) + 1);
    return style_in_drawn(d, g, out);
}

/* An element's custom property for getComputedStyle (jswin.h, jd_custom_of):
   found in the chain computed_style leaves in the layout's tables, var() in
   it put in. */
static int custom_style(int node, const char *name, char *out, int cap) {
    cstyle *s = (cstyle *)malloc(sizeof(cstyle));
    if (!s) return 0;
    const char *v = computed_style(node, s) ? lay_var_find(s->vars, name, w_len(name)) : 0;
    if (v) lay_var_subst(s->vars, v, w_len(v), out, cap, 0);
    free(s);
    return v != 0;
}

static int style_in_drawn(const ddoc *d, int node, cstyle *out) {
    int chain[256], n = 0;
    for (int p = node; p >= 0; p = d->nodes[p].parent) {
        if (n >= (int)(sizeof(chain) / sizeof(chain[0]))) return 0;
        chain[n++] = p;
    }
    if (chain[n - 1] != d->root) return 0;              /* not in the page */
    restyle_changed();
    lay_gen++;
    lay_hits_forget();
    lay_var_used = 0;
    lay_arena_used = 0;

    static lctx L;
    volatile u8 *z = (volatile u8 *)&L;
    for (u32 i = 0; i < sizeof(L); i++) z[i] = 0;
    L.d = d; L.s = &sheet; L.x = &index_; L.m = &match; L.inl = inl;
    L.imgs = lay_sizes; L.nimgs = sizes_for_layout();
    L.out = &page; L.root_px = root_px;
    L.cur_link = -1; L.flex_sized = -1; L.floating = -1; L.pct_node = -1;
    int width = css_view_w > 0 ? css_view_w : 800;
    L.line_width = width; L.pos_w = width; L.cont_width = width;

    static cstyle a, b;
    css_default_style(&a, root_px);
    cstyle *cur = &a, *next = &b;
    for (int i = n - 1; i >= 0; i--) {
        lay_style(&L, chain[i], cur, next, width);
        if (i == n - 1 && next->font_px > 0) L.root_px = next->font_px;   /* a rem, as lay_run has it */
        cstyle *t = cur; cur = next; next = t;
    }
    lay_cs(out, cur);
    return 1;
}

/* How big a picture is, as its file has it. */
static int picture_size(int node, int *w, int *h) {
    for (int i = 0; i < npics; i++)
        if (pics[i].node == node && pics[i].pic.rgb) { *w = pics[i].nat_w; *h = pics[i].nat_h; return 1; }
    *w = *h = 0;
    return 0;
}

/* A decoded picture's pixels for a page's canvas to draw (jscanvas.h):
   three bytes each, and their alpha or 0. */
static const u8 *image_pixels(int node, int *w, int *h, const u8 **alpha) {
    const picture *p = pic_of(node);
    if (!p) return 0;
    *w = p->w;
    *h = p->h;
    *alpha = pic_alpha_of(node);
    return p->rgb;
}

static void script_scroll(int y) { scroll = y < 0 ? 0 : y; }

/* document.cookie, from the jar and into it, for the page's address: what a
   request there would send, less what is HttpOnly (fetch.h). */
static int script_cookies(char *out, int cap) { return ck_cookies_for(&here, out, cap, 1); }

/* The cookies a WebSocket's upgrade carries: the jar's for its address, as
   for any request there (HttpOnly ones too: no script reads them here). */
static int socket_cookies(const char *address, char *out, int cap) {
    char http[URL_TEXT];
    int n = 0, secure = w_starts_fold(address, "wss://");
    const char *rest = address + (secure ? 6 : w_starts_fold(address, "ws://") ? 5 : 0);
    for (const char *p = secure ? "https://" : "http://"; *p && n < URL_TEXT - 1; p++) http[n++] = *p;
    for (const char *p = rest; *p && n < URL_TEXT - 1; p++) http[n++] = *p;
    http[n] = 0;
    url_t u;
    out[0] = 0;
    if (!url_parse(http, &u)) return 0;
    return ck_cookies_for(&u, out, cap, 0);
}
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

static void pictures_follow(void);
static void sprites_follow(void);

/* The page's title, as the status line and the console show it: folded as
   a word is (layout.h, lay_fold), since a script's title arrives as it
   wrote it and "Café — news" was shown as "Caf   news". The tree keeps
   what the page said. */
static void title_take(void) {
    if (doc.title < 0) { title[0] = 0; return; }
    const char *t = doc.arena + doc.title;
    int n = w_len(t);
    char folded[sizeof(title) * 4];
    int nf = n < (int)sizeof(folded) - 8 ? lay_fold(t, n, folded, (int)sizeof(folded)) : 0;
    if (nf) { folded[nf] = 0; t = folded; }
    dom_collapse(t, title, (int)sizeof(title));
}

static unsigned page_loads;      /* moved on by every page built (build) */

static void relayout(int width) {
    sheets_follow();
    restyle_changed();
    /* And the title, which a script may have written. */
    title_take();
    drawings_drop();                 /* made for the sizes of the last one */
    match.hover = hover_node;
    match.visited_links = 0;
    lay_show_hidden = 0;
    const ddoc *d = page_drawn(1);
    laid = d;
    int nsizes = sizes_for_layout();
    /* The rules that reach each element are kept from the last layout when
       nothing they are matched against has changed (layout.h, lay_hits_keep):
       the same page and tree, the same sheet, width and hover. */
    static unsigned hit_print;
    unsigned hp = 2166136261u;
    unsigned parts[] = { page_loads, jsdom_live() ? jsdom_dom_version() : 0, jsdom_live() ? jsdom_css_version() : 0,
                         styles_made, (unsigned)sheet.nrules, (unsigned)sheet.ndecls, (unsigned)d->count,
                         (unsigned)(u64)d, (unsigned)width, (unsigned)match.hover, (unsigned)match.visited_links };
    for (unsigned k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) hp = (hp ^ parts[k]) * 16777619u;
    lay_hits_keep = hp == hit_print;
    hit_print = hp;
    lay_run(&page, d, &sheet, &index_, &match, inl, lay_sizes, nsizes,
            width, root_px);

    /* A page that hides its whole self until its script has rebuilt it.
       Measured rather than guessed at: not a word came out of a document
       with words in it, which is what `<div style="visibility:hidden">`
       around everything does to a browser that is not going to run the
       framework that takes it off again. */
    page_unhidden = 0;
    if (lay_words(&page) == 0 && dom_has_words(d)) {
        lay_show_hidden = 1;
        lay_run(&page, d, &sheet, &index_, &match, inl, lay_sizes,
                nsizes, width, root_px);
        lay_show_hidden = 0;
        page_unhidden = lay_words(&page) > 0;
    }
    /* Sizes may have changed, which a page's ResizeObserver hears of on the
       next pass (jsdom.h). */
    jsdom_laid_out();
    /* And pictures a script has added or moved since. */
    if (jsdom_live()) pictures_follow();
    sprites_follow();
    /* And the background pictures of boxes laid out for the first time: they
       were gathered once, after the first layout, so a background a script
       set, or a box an app drew later, never had its picture fetched
       (SoundCloud's carousel slides). Those already asked for are passed
       over. */
    if (pics_follow_on) gather_backgrounds();
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
/* The first candidate of a srcset that is for one pixel a pixel: the one
   with no descriptor or 1x, else the first. Into out; its length. */
static int srcset_pick(const char *ss, char *out, int cap) {
    const char *pick = 0;
    int pick_n = 0;
    for (const char *p = ss; *p; ) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') p++;
        if (!*p) break;
        /* An address runs to white space; a comma at its end ends the
           candidate (commas inside it are a resizing service's options). */
        const char *u = p;
        while (*p && *p != ' ' && *p != '\n' && *p != '\t') p++;
        int un = (int)(p - u), bare = 0;
        if (un > 0 && u[un - 1] == ',') { un--; bare = 1; }
        const char *dsc = p;
        if (!bare) {
            while (*dsc == ' ' || *dsc == '\t' || *dsc == '\n') dsc++;
            p = dsc;
            while (*p && *p != ',') p++;
        }
        int one = bare || dsc == p || (dsc[0] == '1' && (dsc[1] == 'x' || dsc[1] == '.'));
        if (!pick || one) { pick = u; pick_n = un; }
        if (one) break;
    }
    if (!pick || pick_n <= 0 || pick_n >= cap) return 0;
    for (int k = 0; k < pick_n; k++) out[k] = pick[k];
    out[pick_n] = 0;
    return pick_n;
}

/* A picture element's source for this window: the first <source> before the
   img whose media query holds and whose type can be drawn, and its srcset's
   candidate for one pixel a pixel. Apple's hero has a source for each width
   of window and an img of the smallest, which was drawn blown up to fill
   its place. 0 when none is chosen. */
static const char *pic_from_sources(int el, char *out, int cap) {
    int par = doc.nodes[el].parent;
    if (par < 0 || !w_same_fold(dom_tag_name(&doc, par), "picture")) return 0;
    for (int c = doc.nodes[par].first; c >= 0 && c != el; c = doc.nodes[c].next) {
        if (doc.nodes[c].kind != DN_ELEMENT || !w_same_fold(dom_tag_name(&doc, c), "source")) continue;
        const char *type = dom_attr(&doc, c, "type");
        if (type && *type && !(w_starts_fold(type, "image/jpeg") || w_starts_fold(type, "image/jpg")
                               || w_starts_fold(type, "image/png") || w_starts_fold(type, "image/gif")
                               || w_starts_fold(type, "image/webp") || w_starts_fold(type, "image/svg")))
            continue;
        const char *media = dom_attr(&doc, c, "media");
        if (media && *media) {
            int lo, hi, w = css_view_w + css_mq_extra;
            if (!css_mq(media, w_len(media), &lo, &hi)) continue;
            if ((lo >= 0 && w < lo) || (hi >= 0 && w > hi)) continue;
        }
        const char *ss = dom_attr(&doc, c, "srcset");
        if (!ss) ss = dom_attr(&doc, c, "data-srcset");
        if (ss && srcset_pick(ss, out, cap) > 0 && !w_starts_fold(out, "data:")) return out;
    }
    return 0;
}

static const char *pic_source(int el) {
    static char pick[URL_TEXT];
    if (pic_from_sources(el, pick, (int)sizeof(pick))) return pick;
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

/* The k'th picture let go, and the size the layout was told for it. */
static void pic_drop(int k) {
    int node = pics[k].node;
    picture_free(&pics[k].pic);
    if (pics[k].alpha) free(pics[k].alpha);
    for (int m = k + 1; m < npics; m++) {
        pics[m - 1].node = pics[m].node;
        pics[m - 1].pic = pics[m].pic;
        pics[m - 1].alpha = pics[m].alpha;
        pics[m - 1].key = pics[m].key;
        pics[m - 1].nat_w = pics[m].nat_w;
        pics[m - 1].nat_h = pics[m].nat_h;
    }
    npics--;
    for (int m = 0; m < npic_sizes; m++)
        if (pic_sizes[m].node == node) {
            for (int j = m + 1; j < npic_sizes; j++) {
                pic_sizes[j - 1].node = pic_sizes[j].node;
                pic_sizes[j - 1].w = pic_sizes[j].w;
                pic_sizes[j - 1].h = pic_sizes[j].h;
            }
            npic_sizes--;
            break;
        }
}

/* A picture's bytes made into what is drawn for the element: 1 when they
   could be. */
static int pic_take(int i, const u8 *body, int len) {
    /* A source changed by a script: the new picture in the old one's place. */
    for (int k = 0; k < npics; k++)
        if (pics[k].node == i) { pic_drop(k); break; }
    if (npics >= PICS_MAX) return 0;
    shown *s = &pics[npics];
    s->key = 0;
    s->node = i;
    s->alpha = 0;

    /* Which kind it is, from the bytes rather than from what the
       server said it was. A server that labels a PNG as an octet stream
       is common; a PNG that does not start with the PNG signature is
       not, so the bytes are the better authority. One whose file says
       it may have clear parts is decoded twice, over black and over
       white, so they show what is behind them (pic_merge): flattened
       onto white, a logo on a dark header sat in a white box. */
    int ok = 0;
    int clear = pic_may_be_clear(body, len);
    u32 under = clear ? 0x000000 : 0xFFFFFF;
    picture white;
    white.rgb = 0;

    if (len > 8 && body[0] == 137 && body[1] == 'P'
        && body[2] == 'N' && body[3] == 'G') {
        ok = png_decode(body, len, &s->pic, under) == PNG_OK;
        if (ok && clear) ok = png_decode(body, len, &white, 0xFFFFFF) == PNG_OK;
    } else if (len > 3 && body[0] == 0xFF && body[1] == 0xD8) {
        ok = jpeg_decode(body, len, &s->pic) == JPG_OK;
        clear = 0;
    } else if (len > 6 && body[0] == 'G' && body[1] == 'I' && body[2] == 'F') {
        ok = gif_decode(body, len, &s->pic, under) == GIF_OK;
        if (ok && clear) ok = gif_decode(body, len, &white, 0xFFFFFF) == GIF_OK;
    } else if (len > 12 && body[0] == 'R' && body[1] == 'I' && body[2] == 'F'
               && body[3] == 'F' && body[8] == 'W' && body[9] == 'E'
               && body[10] == 'B' && body[11] == 'P') {
        /* What a server sends a browser that says it takes WebP, and
           what some send whatever the browser says. */
        ok = webp_decode(body, len, &s->pic, under) == WEBP_OK;
        if (ok && clear) ok = webp_decode(body, len, &white, 0xFFFFFF) == WEBP_OK;
    } else {
        /* A drawing, which is markup and so can start with an XML
           declaration, a comment, or the element itself. */
        int at = 0;
        while (at < len && (body[at] == ' ' || body[at] == '\n'
                            || body[at] == '\r' || body[at] == '\t')) at++;
        if (at < len && body[at] == '<') {
            /* What the page asked for, so a drawing lands at the size
               the layout is about to leave for it rather than at
               whatever size it happens to describe. */
            const char *aw = dom_attr(&doc, i, "width");
            const char *ah = dom_attr(&doc, i, "height");
            int want_w = aw ? lay_number(aw) : 0;
            int want_h = ah ? lay_number(ah) : 0;
            ok = svg_render((const char *)body, len, want_w, want_h,
                            &s->pic, 0x000000) == SVG_OK
              && svg_render((const char *)body, len, want_w, want_h,
                            &white, 0xFFFFFF) == SVG_OK;
            clear = 1;
        }
    }

    if (ok && clear && white.rgb) ok = pic_merge(&s->pic, &white, &s->alpha);
    else if (white.rgb) picture_free(&white);
    if (!ok) { picture_free(&s->pic); pics_skipped++; return 0; }
    s->nat_w = s->pic.w;
    s->nat_h = s->pic.h;
    int side = s->pic.w > s->pic.h ? s->pic.w : s->pic.h;
    if (side > PIC_KEEP) pic_shrink(&s->pic, &s->alpha, (side + PIC_KEEP - 1) / PIC_KEEP);

    pic_sizes[npic_sizes].node = i;
    pic_sizes[npic_sizes].w = s->nat_w;
    pic_sizes[npic_sizes].h = s->nat_h;
    npic_sizes++;
    npics++;
    return 1;
}

/* Whether an element is in the page, its shadow trees included. A script
   that rebuilds the page leaves what it took out in the tree. */
static int pic_in_page(int el) {
    if (jd_doc == &doc) return jd_connected_deep(el);
    for (int k = 0; el >= 0 && k < DOM_NODES; k++) {
        if (el == doc.root) return 1;
        el = doc.nodes[el].parent;
    }
    return 0;
}

/* Every picture element: one written into the page made now, one at an
   address asked for (pictures_pump) and laid out when it comes. */
static void gather_pictures(void) {
    for (int i = 0; i < doc.count && npics + npic_wait < PICS_MAX; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_IMG)
            continue;
        if (in_template(i)) continue;          /* a slot for one that is drawn */
        if (!pic_in_page(i)) continue;

        const char *src = pic_source(i);
        if (!src) continue;

        if (w_starts_fold(src, "data:")) {
            /* A one-pixel placeholder is not worth a slot; anything else is
               decoded where it lies, with no fetch at all. */
            int dn = pic_data_uri(src, cssbuf, CSS_MAX);
            if (dn < 100) continue;
            if (pic_take(i, (const u8 *)cssbuf, dn)) pics[npics - 1].key = pic_key(src);
        } else {
            url_t u;
            if (!url_join(&base, src, &u)) { pics_skipped++; continue; }
            pic_wait_add(i, -1, &u);
        }
    }
}

/* Whether an element's picture from this address is drawn, coming, or has
   been tried and would not come. */
static int pic_known(int node, u32 key) {
    for (int k = 0; k < npics; k++) if (pics[k].node == node && pics[k].key == key) return 1;
    for (int k = 0; k < npic_wait; k++) if (pic_wait[k].node == node && pic_wait[k].key == key) return 1;
    for (int k = 0; k < npic_tried; k++) if (pic_tried[k].node == node && pic_tried[k].key == key) return 1;
    return 0;
}

/* Pictures a page's script added, or pointed at another address, after the
   page was shown -- a picture loaded as it comes into view starts with no
   source, or a stand-in, and its script gives it the real one later. Asked
   for as the page's first ones were, after every layout a script's change
   brings. Only gathering at build saw pictures before. */
/* The address and the name a <use> points at in another file: 0 for one
   that names an element of the page. */
static int sprite_ref(const ddoc *d, int el, url_t *u, const char **frag) {
    const char *href = dom_attr(d, el, "href");
    if (!href) href = dom_attr(d, el, "xlink:href");
    if (!href || href[0] == '#' || !href[0]) return 0;
    int h = 0;
    while (href[h] && href[h] != '#') h++;
    if (!href[h] || !href[h + 1] || h >= URL_TEXT) return 0;
    char addr[URL_TEXT];
    for (int i = 0; i < h; i++) addr[i] = href[i];
    addr[h] = 0;
    if (!url_join(&base, addr, u)) return 0;
    *frag = href + h + 1;
    return 1;
}

static int sprite_slot(u32 key) {
    for (int i = 0; i < nsprites; i++) if (sprites[i].key == key) return i;
    return -1;
}

static void sprites_follow(void) {
    if (!pics_follow_on) return;
    for (int i = 0; i < doc.count && nsprites < SPRITES_MAX && npic_wait < PIC_WAIT; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_OTHER) continue;
        if (!w_same_fold(dom_tag_name(&doc, i), "use")) continue;
        url_t u;
        const char *frag;
        if (!sprite_ref(&doc, i, &u, &frag)) continue;
        char text[URL_TEXT];
        url_text(&u, text, sizeof(text));
        u32 key = pic_key(text);
        if (sprite_slot(key) >= 0) continue;
        sprites[nsprites].key = key;
        sprites[nsprites].text = 0;
        sprites[nsprites].len = 0;
        sprites[nsprites].state = 0;
        pic_wait_add(-1, -2 - nsprites, &u);
        nsprites++;
    }
}

/* Each sprite a <use> of the copy names, read in under <head> once, and the
   use pointed at it. */
static void sprites_into(ddoc *d) {
    int holder = -1, count = d->count;
    u8 in[SPRITES_MAX] = { 0 };
    for (int i = 0; i < count; i++) {
        if (d->nodes[i].kind != DN_ELEMENT || d->nodes[i].tag != T_OTHER) continue;
        if (!w_same_fold(dom_tag_name(d, i), "use")) continue;
        url_t u;
        const char *frag;
        if (!sprite_ref(d, i, &u, &frag)) continue;
        char text[URL_TEXT];
        url_text(&u, text, sizeof(text));
        int s = sprite_slot(pic_key(text));
        if (s < 0 || sprites[s].state != 1) continue;
        char prefix[8] = { 'z', 's', (char)('0' + s), '-', 0 };
        if (!in[s]) {
            if (holder < 0) {
                int at = d->head >= 0 ? d->head : d->root;
                holder = dom_create_element(d, "svg", 3);
                if (holder < 0) return;
                dom_append(d, at, holder);
            }
            int first = d->count;
            dom_parse_into(d, sprites[s].text, sprites[s].len, holder);
            for (int k = first; k < d->count; k++) {
                if (d->nodes[k].kind != DN_ELEMENT) continue;
                const char *id = dom_attr(d, k, "id");
                if (!id || !id[0]) continue;
                char named[160];
                int w = 0;
                for (const char *q = prefix; *q; q++) named[w++] = *q;
                for (const char *q = id; *q && w < (int)sizeof(named) - 1; q++) named[w++] = *q;
                named[w] = 0;
                dom_flat_mark(d, k, "id", named);
            }
            in[s] = 1;
        }
        char to[160];
        int w = 0;
        to[w++] = '#';
        for (const char *q = prefix; *q; q++) to[w++] = *q;
        for (const char *q = frag; *q && w < (int)sizeof(to) - 1; q++) to[w++] = *q;
        to[w] = 0;
        dom_flat_mark(d, i, "href", to);
    }
}

/* The picture an img element in the page wants, hashed: 0 for none. */
static u32 pic_wanted(int i, url_t *u) {
    if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_IMG) return 0;
    if (in_template(i) || !pic_in_page(i)) return 0;
    const char *src = pic_source(i);
    if (!src) return 0;
    if (w_starts_fold(src, "data:")) return pic_key(src);
    if (!url_join(&base, src, u)) return 0;
    char text[URL_TEXT];
    url_text(u, text, sizeof(text));
    return pic_key(text);
}

/* Pictures and asks whose elements a script took out of the page: handed to
   an element put in with the same address, which a framework that renders
   the page again on top of the server's copy does with every picture
   (bbc.com: 70 img elements in the page, 210 taken out), and let go
   otherwise. They kept every slot, so the elements drawn got none, and
   were drawn as their alt text. */
static void pics_rehome(void) {
    u8 out[PICS_MAX];
    int any = 0;
    for (int k = 0; k < npics; k++) {
        out[k] = !pic_in_page(pics[k].node);
        any |= out[k];
    }
    for (int k = 0; k < npic_wait; k++)
        if (pic_wait[k].bg == -1 && !pic_in_page(pic_wait[k].node)) any = 1;
    if (!any) return;
    for (int i = 0; i < doc.count; i++) {
        url_t u;
        u32 key = pic_wanted(i, &u);
        if (!key || pic_known(i, key)) continue;
        int k;
        for (k = 0; k < npics; k++)
            if (out[k] && pics[k].key == key) {
                for (int m = 0; m < npic_sizes; m++)
                    if (pic_sizes[m].node == pics[k].node) pic_sizes[m].node = i;
                pics[k].node = i;
                out[k] = 0;
                pics_moved = 1;
                break;
            }
        if (k < npics) continue;
        for (k = 0; k < npic_wait; k++)
            if (pic_wait[k].bg == -1 && pic_wait[k].key == key && !pic_in_page(pic_wait[k].node)) {
                pic_wait[k].node = i;
                break;
            }
    }
    for (int k = npics - 1; k >= 0; k--)
        if (out[k]) pic_drop(k);
    /* An ask not yet started for an element gone is not started. */
    for (int k = 0; k < npic_wait; k++) {
        pic_wait_t *w = &pic_wait[k];
        if (w->bg != -1 || w->ask || pic_in_page(w->node)) continue;
        for (int m = k + 1; m < npic_wait; m++) {
            pic_wait[m - 1].node = pic_wait[m].node;
            pic_wait[m - 1].bg = pic_wait[m].bg;
            pic_wait[m - 1].ask = pic_wait[m].ask;
            pic_wait[m - 1].key = pic_wait[m].key;
            w_copy(pic_wait[m - 1].url, URL_TEXT, pic_wait[m].url, URL_TEXT);
        }
        npic_wait--;
        k--;
    }
}

static void pictures_follow(void) {
    if (!pics_follow_on) return;
    pics_rehome();
    for (int i = 0; i < doc.count && npic_wait < PIC_WAIT; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_IMG) continue;
        if (in_template(i) || !pic_in_page(i)) continue;
        const char *src = pic_source(i);
        if (!src) continue;
        if (w_starts_fold(src, "data:")) {
            u32 key = pic_key(src);
            if (pic_known(i, key)) continue;
            int dn = pic_data_uri(src, cssbuf, CSS_MAX);
            if (dn >= 100 && pic_take(i, (const u8 *)cssbuf, dn)) pics[npics - 1].key = key;
            else if (npic_tried < PIC_TRIED) { pic_tried[npic_tried].node = i; pic_tried[npic_tried++].key = key; }
            continue;
        }
        url_t u;
        if (!url_join(&base, src, &u)) continue;
        char text[URL_TEXT];
        url_text(&u, text, sizeof(text));
        if (pic_known(i, pic_key(text))) continue;
        if (npics + npic_wait >= PICS_MAX) {
            /* No room unless it replaces the element's own picture. */
            int had = 0;
            for (int k = 0; k < npics; k++) if (pics[k].node == i) had = 1;
            if (!had) continue;
        }
        pic_wait_add(i, -1, &u);
    }
}

/* The pictures waiting, a pass at a time: each started as a way out comes
   free, each that has come made. Bit 1 when an element's came (the page is
   laid out again), 2 when a background's did (only drawn), 4 when any was
   settled either way (the status says so). */
static int pictures_pump(void) {
    int what = 0;
    for (int k = 0; k < npic_wait; k++) {
        pic_wait_t *w = &pic_wait[k];
        int done = 0, ok = 0;
        if (!w->ask) {
            int id = ask_begin("GET", w->url, 0, 0, 0, PIC_ACCEPT, CSS_MAX, 0);
            if (id == -1) break;                 /* every way out busy: a later pass */
            if (id >= 0) { w->ask = id + 1; continue; }
            done = 1;
        } else {
            jd_reply rp = { 0, 0, 0, 0, 0, 0, 0 };
            int st = ask_poll(w->ask - 1, &rp);
            if (st == 0) continue;
            done = 1;
            if (st > 0 && rp.status < 400 && rp.len > 0) {
                if (w->bg >= 0) ok = bg_take(&bgpics[w->bg], (const u8 *)rp.body, rp.len);
                else if (w->bg <= -2) ok = sprite_take(-2 - w->bg, (const u8 *)rp.body, rp.len);
                else if ((ok = pic_take(w->node, (const u8 *)rp.body, rp.len)) != 0) pics[npics - 1].key = w->key;
            }
            ask_end(w->ask - 1);
        }
        if (!done) continue;
        if (w->bg <= -2) {
            /* A sprite: laid out again when it came, to be drawn from. */
            if (!ok && -2 - w->bg < nsprites) sprites[-2 - w->bg].state = 2;
            if (ok) what |= 1;
            what |= 4;
        } else if (!ok && w->bg < 0) {
            pics_skipped++;
            if (npic_tried < PIC_TRIED) { pic_tried[npic_tried].node = w->node; pic_tried[npic_tried++].key = w->key; }
        }
        if (w->bg > -2) {
            if (ok) what |= w->bg >= 0 ? 2 : 1;
            what |= 4;
        }
        /* Field by field: assigning the whole would be a memcpy, which a
           program here does not have. */
        for (int m = k + 1; m < npic_wait; m++) {
            pic_wait[m - 1].node = pic_wait[m].node;
            pic_wait[m - 1].bg = pic_wait[m].bg;
            pic_wait[m - 1].ask = pic_wait[m].ask;
            pic_wait[m - 1].key = pic_wait[m].key;
            w_copy(pic_wait[m - 1].url, URL_TEXT, pic_wait[m].url, URL_TEXT);
        }
        npic_wait--;
        k--;
    }
    return what;
}

/* The address a link goes to: its own, or for a player from another site
   (a frame, layout.h lay_frame_player) the page it is read from, when the
   site's reader knows one (sites.h site_frame_page). */
static int link_target(int li, url_t *u) {
    if (!url_join(&base, page.text + page.links[li].href, u)) return 0;
    int node = page_node(page.links[li].node);
    if (node >= 0 && doc.nodes[node].tag == T_IFRAME) {
        char to[URL_TEXT];
        url_t v;
        if (site_frame_page(u, to, sizeof(to)) && url_parse(to, &v)) url_copy(u, &v);
    }
    return 1;
}

/* The page's base, from its first <base> element with an href. */
static void base_take(void) {
    url_copy(&base, &here);
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_OTHER) continue;
        if (!w_same(dom_tag_name(&doc, i), "base")) continue;
        const char *h = dom_attr(&doc, i, "href");
        if (!h) continue;
        url_t b;
        if (url_join(&here, h, &b)) url_copy(&base, &b);
        return;
    }
}

static void build(const char *html, int len, int width, int want_sheets,
                  int *fetched, int *skipped) {
    /* Before the tree it is bound to is taken apart under it. */
    jsdom_close();
    page_loads++;
    nsources = 0;
    nreported = 0;
    styles_live = 0;
    styles_dom_seen = 0xFFFFFFFFu;
    links_forget();
    trees_known = 0;
    trees_n = 0;
    laid = 0;
    flat_used = -1;
    dom_parse(&doc, html, len);
    dom_shadows(&doc);
    meta_referrer();
    base_take();
    /* The old page's pictures let go before its asks are, since each holds
       the number of a slot that is about to be another file's. */
    pics_drop();
    asks_made = 0;
    asks_drop_all();
    files_ahead(want_sheets);

    css_init(&sheet);
    css_parse(&sheet, CSS_UA, (int)sizeof(CSS_UA) - 1);
    sheet.plain_at = sheet.nrules;
    css_parse(&sheet, CSS_UA_PLAIN, (int)sizeof(CSS_UA_PLAIN) - 1);
    sheet.plain_end = sheet.nrules;
    accent_sheet();
    sheet.ua_rules = sheet.nrules;        /* the page's own count for more */
    gather_inline_sheets();
    if (want_sheets) gather_linked_sheets(fetched, skipped);
    else { *fetched = 0; *skipped = 0; }
    gather_inline_styles();
    styles_base();
    css_index(&sheet, &index_);

    /* The pictures, before the layout so it can leave room for them. */
    if (want_sheets) gather_pictures();
    pics_follow_on = want_sheets;
    sprites_follow();                    /* and the icon sprites the markup names */

    /* And then whatever the page brought with it, before any of it is laid
       out: a script that writes to an element is writing to the document
       the layout is about to read, so running them afterwards would show
       the page as it was and correct it a frame later. */
    focus_control(-1);
    page_active = -1;
    script_err[0] = 0;
    said_script_err = 0;
    scripts_ran = 0;
    scripts_changed = 0;
    scripts_outside = 0;
    scripts_bytes = 0;
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
    jsdom_referrer(page_referrer);
    jsdom_view(width, css_view_h, 0);
    if (jsdom_open(&doc, &sheet)) {
        jsdom_fetch_with(fetch_script);
        jsdom_want_with(script_want);
        jsdom_request_with(do_request);
        jsdom_asks_with(ask_start, ask_poll, ask_end);
        jsdom_script_asks_with(script_ask_start, script_ask_poll, ask_end);
        jsdom_boxes_with(box_of);
        jsdom_points_with(node_at_point);
        jsdom_pictures_with(picture_size);
        jsdom_image_pixels_with(image_pixels);
        jsdom_scroll_with(script_scroll);
        jsdom_navigate_with(script_navigate);
        jsdom_submit_with(submit_form);
        jsdom_cookies_with(script_cookies, script_set_cookie);
        jsdom_websockets_with(socket_cookies);
        jsdom_address_with(script_address);
        jsdom_history_with(script_history_go, script_history_length);
        jsdom_styles_with(computed_style);
        jsdom_pseudo_with(pseudo_style);
        jsdom_custom_with(custom_style);
        jsdom_memory_with(free_memory);
        jsdom_sheet_text_with(sheet_text_for_script);
        styles_live = want_sheets;
        links_report();
        scripts_ran = jsdom_scripts(script_err, (int)sizeof(script_err));
        ahead_drop();
        jsdom_loaded();
        scripts_changed = jsdom_changed();
    }

    hover_node = -1;
    hover_n = 0;
    relayout(width);
    if (want_sheets) gather_backgrounds();
    find_item = -1;
    publish_text();

    title_take();
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

/* --- a file and not a page ------------------------------------------------
 *
 * A link to an archive, a document or a program brings something that is
 * not a page, and read as one it was a screen of garbage. It is saved in
 * /home/downloads, under the name the server gives it or the last part of
 * its address, and read back before the page says it was: a disk that is
 * full says so only by holding less. A picture is shown on a page of its
 * own and an MP4 played on one (media.h's file mode); text, JSON and pages
 * are what they were. Anything the server calls an attachment is saved
 * whatever it is. */
#define DOWNLOADS "/home/downloads"

enum { BODY_PAGE, BODY_FILE, BODY_PICTURE, BODY_VIDEO };

static int body_kind(const response_t *r) {
    char cd[128];
    if (r->head && r->hlen > 0 && wh_header(r->head, r->hlen, "content-disposition", cd, sizeof(cd))
        && w_starts_fold(cd, "attachment"))
        return BODY_FILE;
    const char *t = r->ctype;
    if (!t[0]) return BODY_PAGE;
    if (w_starts_fold(t, "image/")) return BODY_PICTURE;
    if (w_starts_fold(t, "video/mp4") || w_starts_fold(t, "audio/mp4")) return BODY_VIDEO;
    if (w_starts_fold(t, "text/") || w_starts_fold(t, "application/json") || w_starts_fold(t, "application/ld+json")
        || w_starts_fold(t, "application/xhtml") || w_starts_fold(t, "application/xml")
        || w_starts_fold(t, "application/rss") || w_starts_fold(t, "application/atom")
        || w_starts_fold(t, "application/javascript") || w_starts_fold(t, "application/ecmascript"))
        return BODY_PAGE;
    return BODY_FILE;
}

/* A name to keep it under: the one in Content-Disposition, else the last
   part of the address with its escapes undone, with nothing in it that a
   path or the page saying where it went would read as something else. */
static void download_name(const response_t *r, const url_t *u, char *out, int cap) {
    char cd[256], raw[256];
    int n = 0;
    raw[0] = 0;
    if (r->head && r->hlen > 0 && wh_header(r->head, r->hlen, "content-disposition", cd, sizeof(cd))) {
        for (const char *p = cd; *p; p++) {
            if (!w_starts_fold(p, "filename=")) continue;
            p += 9;
            char q = *p == '"' ? *p++ : 0;
            while (*p && n < (int)sizeof(raw) - 1 && (q ? *p != q : (*p != ';' && *p != ' '))) raw[n++] = *p++;
            raw[n] = 0;
            break;
        }
    }
    if (!raw[0]) {
        const char *path = u->path, *last = path;
        for (const char *p = path; *p && *p != '?' && *p != '#'; p++) if (*p == '/') last = p + 1;
        n = 0;
        for (const char *p = last; *p && *p != '?' && *p != '#' && n < (int)sizeof(raw) - 1; p++) {
            if (p[0] == '%' && p[1] && p[2]) {
                int hi = p[1] <= '9' ? p[1] - '0' : (w_lower(p[1]) - 'a' + 10);
                int lo = p[2] <= '9' ? p[2] - '0' : (w_lower(p[2]) - 'a' + 10);
                raw[n++] = (char)(hi * 16 + lo);
                p += 2;
            } else {
                raw[n++] = *p;
            }
        }
        raw[n] = 0;
    }
    int w = 0;
    for (int i = 0; raw[i] && w < cap - 1 && w < 80; i++) {
        char c = raw[i];
        if ((u8)c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"'
            || c == '<' || c == '>' || c == '|' || c == '&')
            c = '_';
        out[w++] = c;
    }
    while (w > 0 && (out[w - 1] == '.' || out[w - 1] == ' ')) w--;
    out[w] = 0;
    if (!w) w_copy(out, cap, "download", cap);
}

/* Saved under a name nothing in the folder has yet ("name (2).ext"), and
   read back; how many bytes went, or -1. */
static int save_download(const char *body, int len, const char *name, char *path, int cap) {
    mkdir("/home");
    mkdir(DOWNLOADS);
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot || dot == name) dot = name + w_len(name);
    for (int k = 1; k < 100; k++) {
        int w = 0;
        for (const char *p = DOWNLOADS "/"; *p && w < cap - 1; p++) path[w++] = *p;
        for (const char *p = name; p < dot && w < cap - 1; p++) path[w++] = *p;
        if (k > 1) {
            char num[16];
            number_into(num, k);
            for (const char *p = " ("; *p && w < cap - 1; p++) path[w++] = *p;
            for (const char *p = num; *p && w < cap - 1; p++) path[w++] = *p;
            if (w < cap - 1) path[w++] = ')';
        }
        for (const char *p = dot; *p && w < cap - 1; p++) path[w++] = *p;
        path[w] = 0;
        zelr_stat st;
        if (stat(path, &st) < 0) break;
    }
    int fd = open(path, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) return -1;
    int done = 0;
    while (done < len) {
        int got = zelr_fwrite(fd, body + done, len - done);
        if (got <= 0) break;
        done += got;
    }
    close(fd);
    if (done != len) return -1;
    fd = open(path, O_READ);
    if (fd < 0) return -1;
    static char back[64 * 1024];
    int at = 0, same = 1;
    while (at < len && same) {
        int want = len - at < (int)sizeof(back) ? len - at : (int)sizeof(back);
        int got = zelr_fread(fd, back, want);
        if (got <= 0) { same = 0; break; }
        for (int i = 0; i < got; i++) if (back[i] != body[at + i]) { same = 0; break; }
        at += got;
    }
    close(fd);
    return same && at == len ? len : -1;
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

/* --- bookmarks ------------------------------------------------------------
 *
 * ctrl+D keeps the page in /home/bookmarks, or lets it go if it was kept
 * already, and the start page lists what is kept above everything else. A
 * line each, the address and the title with a tab between, so the file reads
 * as what it is in Notes or the terminal. Kept on the disk, they outlast the
 * browser and the machine being turned off. */
#define BOOKMARKS "/home/bookmarks"
#define BOOKMARKS_MAX (64 * 1024)

static char bm_text[BOOKMARKS_MAX + 1];
static int  bm_len;

static void bm_read(void) {
    bm_len = 0;
    int fd = open(BOOKMARKS, O_READ);
    if (fd >= 0) {
        int got;
        while (bm_len < BOOKMARKS_MAX && (got = zelr_fread(fd, bm_text + bm_len, BOOKMARKS_MAX - bm_len)) > 0)
            bm_len += got;
        close(fd);
    }
    bm_text[bm_len] = 0;
}

static int bm_write(void) {
    mkdir("/home");
    int fd = open(BOOKMARKS, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) return 0;
    int done = 0;
    while (done < bm_len) {
        int got = zelr_fwrite(fd, bm_text + done, bm_len - done);
        if (got <= 0) break;
        done += got;
    }
    close(fd);
    return done == bm_len;
}

/* Where the line for an address starts, or -1. */
static int bm_find(const char *addr) {
    int n = w_len(addr);
    for (int i = 0; i < bm_len; ) {
        int e = i;
        while (e < bm_len && bm_text[e] != '\n') e++;
        if (e - i > n && bm_text[i + n] == '\t') {
            int k = 0;
            while (k < n && bm_text[i + k] == addr[k]) k++;
            if (k == n) return i;
        }
        i = e + 1;
    }
    return -1;
}

static int bm_count(void) {
    int n = 0;
    for (int i = 0; i < bm_len; i++) if (bm_text[i] == '\n') n++;
    return n;
}

/* Kept, or let go if it was kept: 1 kept, 0 let go, -1 not written. */
static int bm_toggle(const char *addr, const char *name) {
    bm_read();
    int at = bm_find(addr);
    if (at >= 0) {
        int e = at;
        while (e < bm_len && bm_text[e] != '\n') e++;
        if (e < bm_len) e++;
        for (int i = e; i <= bm_len; i++) bm_text[at + i - e] = bm_text[i];
        bm_len -= e - at;
        return bm_write() ? 0 : -1;
    }
    int need = w_len(addr) + w_len(name) + 2;
    if (bm_len + need > BOOKMARKS_MAX) return -1;
    for (const char *p = addr; *p; p++) bm_text[bm_len++] = *p;
    bm_text[bm_len++] = '\t';
    for (const char *p = name; *p; p++) bm_text[bm_len++] = *p == '\n' || *p == '\t' ? ' ' : *p;
    bm_text[bm_len++] = '\n';
    bm_text[bm_len] = 0;
    return bm_write() ? 1 : -1;
}

/* Text put into the start page's markup with nothing in it read as markup. */
static int bm_put(char *out, int w, int cap, const char *s, int n) {
    for (int i = 0; i < n && w < cap - 8; i++) {
        char c = s[i];
        if (c == '<') { out[w++] = '&'; out[w++] = 'l'; out[w++] = 't'; out[w++] = ';'; }
        else if (c == '&') { out[w++] = '&'; out[w++] = 'a'; out[w++] = 'm'; out[w++] = 'p'; out[w++] = ';'; }
        else if (c == '"') { out[w++] = '&'; out[w++] = '#'; out[w++] = '3'; out[w++] = '4'; out[w++] = ';'; }
        else out[w++] = c;
    }
    return w;
}

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
        "<!--bookmarks-->"
        "<h2>sites</h2><ul>"
        "<li><a href=\"https://www.youtube.com/results?search_query=music\">YouTube</a> "
        "<small>searches, videos and channels, read from YouTube's own data; videos are "
        "listed with frames from them, not played</small></li>"
        "<li><a href=\"https://www.twitch.tv/\">Twitch</a> "
        "<small>who is live, the categories and the channels, from Twitch's API; a live "
        "channel plays from its page's watch link</small></li>"
        "<li><a href=\"https://en.wikipedia.org/wiki/Special:Random\">Wikipedia</a> "
        "<small>an article at random</small></li>"
        "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
        "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo</a> "
        "<small>where searches go, Google's included</small></li>"
        "<li><a href=\"https://example.com/\">example.com</a></li>"
        "</ul></body></html>";
    /* The bookmarks go where the page leaves room for them. */
    static char page_with[sizeof(PAGE_START) + BOOKMARKS_MAX * 3];
    bm_read();
    const char *mark = "<!--bookmarks-->";
    int w = 0, i = 0, ml = w_len(mark);
    while (PAGE_START[i] && !w_starts_fold(PAGE_START + i, mark)) page_with[w++] = PAGE_START[i++];
    if (bm_len > 0) {
        const char *head = "<h2>bookmarks</h2><ul>";
        for (const char *p = head; *p; p++) page_with[w++] = *p;
        int cap = (int)sizeof(page_with) - 512;
        for (int a = 0; a < bm_len && w < cap; ) {
            int e = a;
            while (e < bm_len && bm_text[e] != '\n') e++;
            int t = a;
            while (t < e && bm_text[t] != '\t') t++;
            const char *open_ = "<li><a href=\"";
            for (const char *p = open_; *p; p++) page_with[w++] = *p;
            w = bm_put(page_with, w, cap, bm_text + a, t - a);
            page_with[w++] = '"';
            page_with[w++] = '>';
            int has_name = t + 1 < e;
            w = has_name ? bm_put(page_with, w, cap, bm_text + t + 1, e - t - 1) : bm_put(page_with, w, cap, bm_text + a, t - a);
            const char *mid = "</a> <small>";
            for (const char *p = mid; *p; p++) page_with[w++] = *p;
            w = bm_put(page_with, w, cap, bm_text + a, t - a);
            const char *close_ = "</small></li>";
            for (const char *p = close_; *p; p++) page_with[w++] = *p;
            a = e + 1;
        }
        const char *tail = "</ul><p><small>ctrl+D keeps a page here, or lets it go</small></p>";
        for (const char *p = tail; *p; p++) page_with[w++] = *p;
    } else {
        const char *none = "<p><small>ctrl+D keeps the page you are on here</small></p>";
        for (const char *p = none; *p; p++) page_with[w++] = *p;
    }
    if (PAGE_START[i]) i += ml;
    while (PAGE_START[i]) page_with[w++] = PAGE_START[i++];
    page_with[w] = 0;
    int fetched, skipped;
    build(page_with, w, width, 0, &fetched, &skipped);
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

/* The site's own page instead of what sites.h reads out of it, for the
   next load only: YouTube's or Twitch's application, run as it is. */
static int site_readers_off;

/* The pictures, as many as have come and how many are still coming,
   between the halves load said. */
static void status_compose(void) {
    if (!pics_said) return;
    w_copy(status, sizeof(status), status_head, sizeof(status));
    int coming = 0;
    for (int k = 0; k < npic_wait; k++) if (pic_wait[k].bg < 0) coming++;
    char n[16];
    if (npics || coming) {
        number_into(n, npics);
        say_more(", ");
        say_more(n);
        say_more(npics == 1 ? " picture" : " pictures");
        if (coming) {
            number_into(n, coming);
            say_more(" (");
            say_more(n);
            say_more(" more coming)");
        }
        if (pics_skipped) say_more(" (more would not show)");
    } else if (pics_skipped) {
        say_more(", no picture on it would show");
    }
    say_more(status_tail);
}

static void load(const char *address, int width, int keep_scroll) {
    int readers = !site_readers_off;
    site_readers_off = 0;
    /* What was still coming for the last page is not this one's. */
    pic_wait_drop();
    pics_said = 0;
    if (w_same_fold(address, START_ADDRESS)) {
        show_start(width);
        char shown[16];
        number_into(shown, page.nlinks);
        say("the start page, which needs no network: ", shown);
        say_more(" links");
        int nb = bm_count();
        if (nb) {
            number_into(shown, nb);
            say_more(", ");
            say_more(shown);
            say_more(nb == 1 ? " bookmark" : " bookmarks");
        }
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

    /* Asked for from the page that sent the browser here, if one did; then
       everything the page asks for is asked from the page itself, under the
       default policy until it says another. */
    w_copy(web_referrer_from, sizeof(web_referrer_from), nav_from, sizeof(web_referrer_from));
    web_referrer_policy = nav_policy;
    web_referrer_for(&here, page_referrer, sizeof(page_referrer));
    nav_from[0] = 0;
    nav_policy = REF_STRICT_CROSS;

    /* Twitch sends no page worth fetching, only its application; what it
       would show comes from its API, and is written as a page here. */
    if (!load_post && readers && site_is_twitch(&here)) {
        say("asking Twitch who is live", 0);
        int n = site_twitch(&here, src, SRC_MAX);
        url_text(&here, web_referrer_from, sizeof(web_referrer_from));
        web_referrer_policy = REF_STRICT_CROSS;
        if (n > 0) {
            int fetched, skipped;
            build(src, n, width, 1, &fetched, &skipped);
            if (!keep_scroll) scroll = 0;
            say("read from Twitch's API; a live channel plays from its watch link", 0);
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
        web_referrer_from[0] = 0;
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

    /* From here on the page is the one asking, under the policy it gave
       if it gave one. */
    url_text(&here, web_referrer_from, sizeof(web_referrer_from));
    web_referrer_policy = REF_STRICT_CROSS;
    {
        int pol = web_policy_of(reply.refpol);
        if (pol >= 0) web_referrer_policy = pol;
    }

    int kind = rc < 400 ? body_kind(&reply) : BODY_PAGE;
    if (kind == BODY_FILE) {
        char name[96], path[160], n[16];
        download_name(&reply, &here, name, (int)sizeof(name));
        if (reply.truncated || reply.cut) {
            show_message("Not saved", reply.truncated
                         ? "It is more than this can hold at once (8 megabytes), and only part of it would be saved."
                         : "The server stopped before the end of it, and only part of it would be saved.", width);
            say("not saved: ", name);
        } else {
            int got = save_download(reply.body, reply.len, name, path, (int)sizeof(path));
            if (got < 0) {
                show_message("Not saved", "It could not be written to " DOWNLOADS ": the disk is full, or "
                             "there is no disk.", width);
                say("not saved: ", name);
            } else {
                number_into(n, got);
                static char said[320];
                int w = 0;
                for (const char *p = path; *p && w < 200; p++) said[w++] = *p;
                for (const char *p = ", "; *p; p++) said[w++] = *p;
                for (const char *p = n; *p; p++) said[w++] = *p;
                for (const char *p = " bytes, read back the same"; *p; p++) said[w++] = *p;
                said[w] = 0;
                show_message("Saved", said, width);
                say("saved ", said);
            }
        }
        w_copy(title, sizeof(title), name, sizeof(title));
        if (big) free(big);
        big = 0;
        return;
    }
    if (kind == BODY_PICTURE || kind == BODY_VIDEO) {
        /* On a page of its own, asked for again by the page's own picture
           or video; the address escaped for the attribute it goes in. */
        int w = 0;
        const char *base_name = here.path;
        for (const char *p = here.path; *p && *p != '?'; p++) if (*p == '/') base_name = p + 1;
        for (const char *p = "<title>"; *p; p++) src[w++] = *p;
        for (const char *p = base_name; *p && *p != '?' && *p != '#' && w < 300; p++)
            src[w++] = *p == '<' || *p == '&' ? '_' : *p;
        for (const char *p = "</title>"; *p; p++) src[w++] = *p;
        const char *open_ = kind == BODY_PICTURE
            ? "<style>body{margin:0;background:#202124}img{display:block;margin:0 auto;max-width:100%}</style><img src=\""
            : "<style>body{margin:0;background:#000}video{display:block;width:100%}</style><video autoplay src=\"";
        for (const char *p = open_; *p && w < SRC_MAX - 64; p++) src[w++] = *p;
        for (const char *p = address; *p && w < SRC_MAX - 64; p++) {
            if (*p == '"') { src[w++] = '%'; src[w++] = '2'; src[w++] = '2'; }
            else if (*p == '<') { src[w++] = '%'; src[w++] = '3'; src[w++] = 'C'; }
            else src[w++] = *p;
        }
        for (const char *p = kind == BODY_PICTURE ? "\" alt=\"\">" : "\"></video>"; *p; p++) src[w++] = *p;
        src[w] = 0;
        int f2, s2;
        build(src, w, width, 1, &f2, &s2);
        if (!keep_scroll) scroll = 0;
        say(kind == BODY_PICTURE ? "a picture: " : "a video: ", reply.ctype);
        if (big) free(big);
        big = 0;
        return;
    }

    int plain = w_starts_fold(reply.ctype, "text/plain")
             || w_starts_fold(reply.ctype, "application/json");

    int fetched = 0, skipped = 0;
    int read_site = 0;
    if (plain) {
        show_plain(reply.body, reply.len, width);
        w_copy(title, sizeof(title), here.path, sizeof(title));
    } else if (big && readers && rc < 400
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

    /* The pictures are said in between, as they come (status_compose). */
    w_copy(status_head, sizeof(status_head), status, sizeof(status_head));
    status[0] = 0;

    /* And what the page's own scripts did. A script that threw is worth
       saying out loud: the page will look like the one it was before it
       ran, and without this there is nothing to tell the two apart. */
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
            if (url_join(&base, where, &next)) {
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
    w_copy(status_tail, sizeof(status_tail), status, sizeof(status_tail));
    pics_said = 1;
    status_compose();
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
static void tface_draw_sp(surface *s, int x, int y, const char *str, u32 fg, int which, int spacing);
static void tface_draw(surface *s, int x, int y, const char *str, u32 fg,
                       int which) {
    tface_draw_sp(s, x, y, str, fg, which, 0);
}

/* With letter-spacing: that many pixels after each letter. */
static void tface_draw_sp(surface *s, int x, int y, const char *str, u32 fg, int which, int spacing) {
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
        x += g->advance + spacing;
    }
}

/* A player from another site, where the frame would be: dark, a play mark,
   and where it is from; the whole box is a link (layout.h lay_video) that
   opens it. What a frame holds is a page of its own, and a second page
   inside the first is more memory than a small machine has to give; opened,
   the player is the page. */
static void draw_frame_box(surface *s, const litem *it, int x, int sy, int vw) {
    int w = it->w > vw ? vw : it->w, h = it->h;
    rect(s, x, sy, w, h, 0x202124);
    int r = (w < h ? w : h) / 6;
    if (r > 30) r = 30;
    if (r < 6) return;
    int face = face_pick(14, 0, 0);
    int words = h >= 2 * r + tface_h(face) + 24;
    int cx = x + w / 2, cy = sy + h / 2 - (words ? (tface_h(face) + 8) / 2 : 0);
    disc(s, cx, cy, r, 0xE8E8EA);
    /* A triangle pointing right, a third of the circle each side of the
       middle: rows narrowing from its left edge. */
    int t = r / 2;
    for (int row = -t; row <= t; row++) {
        int len = (t - (row < 0 ? -row : row)) * 7 / 4;
        if (len > 0) rect(s, cx - t * 2 / 3, cy + row, len, 1, 0x202124);
    }
    if (!words) return;
    url_t u;
    char line[URL_HOST + 32];
    line[0] = 0;
    const char *src = dom_attr(&doc, it->node, "src");
    w_copy(line, sizeof(line), "open the player", 16);
    if (src && url_join(&base, src, &u) && u.host[0]) {
        int n = w_len(line);
        w_copy(line + n, (int)sizeof(line) - n, " from ", 7);
        n = w_len(line);
        w_copy(line + n, (int)sizeof(line) - n, u.host, URL_HOST);
    }
    if (tface_w(line, face) > w - 8) line[15] = 0;          /* just "open the player" */
    if (tface_w(line, face) > w - 8) return;
    tface_draw(s, x + (w - tface_w(line, face)) / 2, cy + r + 8, line, 0xE8E8EA, face);
}

/* Items wholly left or right of these (in the surface's columns) are not
   drawn: a video's frame redraws only what crosses its box. */
static int draw_x0 = -0x7FFFFFFF, draw_x1 = 0x7FFFFFFF;

static void draw_page(surface *s, int ox, int oy, int vw, int vh) {
    /* In the order the layout worked out from z-indexes (lay_z_order). */
    for (int k = 0; k < page.nitems; k++) {
        int i = page.zsorted ? page.zorder[k] : k;
        const litem *it = &page.items[i];
        /* What the layout put away (kind 0: cut off by a box that hides its
           overflow, or the first layout of a box laid out again) is not
           drawn. A word was drawn whatever its kind, at the end of this
           loop, so Debian's hidden banner word showed under the banner and
           a box laid out again drew its words twice. */
        if (!it->kind) continue;
        int y = it->y - scroll;
        if (y + it->h < -8 || y > vh + 8) continue;
        int x = ox + it->x;
        int sy = oy + y;
        if (x + it->w < draw_x0 || x > draw_x1) continue;

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

        if (it->kind == LK_IMAGE && it->node >= 0 && it->node < doc.count && doc.nodes[it->node].tag == T_IFRAME) {
            draw_frame_box(s, it, x, sy, vw);
            continue;
        }

        if (it->kind == LK_IMAGE) {
            const picture *p = pic_of(it->node);
            const u8 *alpha = p ? pic_alpha_of(it->node) : 0;
            if (!p && it->node >= 0 && it->node < doc.count && doc.nodes[it->node].tag == T_SVG)
                p = drawing_of(it);
            if (!p && it->node >= 0 && it->node < doc.count && doc.nodes[it->node].tag == T_VIDEO)
                p = video_of(it->node);
            if (!p && it->node >= 0 && it->node < doc.count && doc.nodes[it->node].tag == T_CANVAS)
                p = canvas_of(it->node, &alpha);
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
                int tw = it->w, th = it->h, nw, nh;
                pic_natural(it->node, p, &nw, &nh);
                if (it->ofit && nw > 0 && nh > 0) {
                    long long fx = (long long)it->w * 65536 / nw, fy = (long long)it->h * 65536 / nh;
                    long long f = it->ofit == 2 ? (fx > fy ? fx : fy) : (fx < fy ? fx : fy);
                    if (it->ofit == 3 || (it->ofit == 4 && f > 65536)) f = 65536;
                    tw = (int)((nw * f) >> 16);
                    th = (int)((nh * f) >> 16);
                    if (tw < 1) tw = 1;
                    if (th < 1) th = 1;
                }
                int x0 = (it->w - tw) / 2, y0 = (it->h - th) / 2;
                int rad = it->radius;
                if (rad * 2 > it->w) rad = it->w / 2;
                if (rad * 2 > it->h) rad = it->h / 2;
                for (int row = 0; row < it->h; row++) {
                    int dy = sy + row;
                    if (dy < oy || dy >= oy + vh || dy < 0 || dy >= s->h) continue;
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

        tface_draw_sp(s, x, sy, str, col, it->face, it->ls);

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
static ui_field bar = { address, sizeof(address), 0, 0, 0, 0 };

/* The keyboard given to a control, or taken away (-1), by the reader: the
   page hears of it as blur and focus (jsdom.h, jsdom_focused). */
static void give_focus(int el) {
    focus_control(el);
    jsdom_focused(el);
    page_active = el;
}

/* Tab: the keyboard to the next field drawn on the page after the one that
   has it -- something to type in or tick -- from the top when none has it
   or the last is passed. Where it is drawn, so the page can be scrolled to
   it, or -1 when the page has none. Buttons and lists are passed over:
   nothing here presses one from the keyboard yet. */
static int tab_next(void) {
    int from = focus_node >= 0 ? focus_node : page_active;
    for (int pass = 0; pass < 2; pass++) {
        int start = pass ? 0 : from + 1, stop = pass ? from + 1 : doc.count;
        for (int i = start < 0 ? 0 : start; i < stop && i < doc.count; i++) {
            int ck = lay_control_kind(&doc, i);
            if (ck != CTL_TEXT && ck != CTL_PASSWORD && ck != CTL_AREA && ck != CTL_CHECK && ck != CTL_RADIO)
                continue;
            if (dom_attr(&doc, i, "disabled")) continue;
            for (int k = 0; k < page.nitems; k++)
                if (page.items[k].node == i) {
                    give_focus(i);
                    return page.items[k].y;
                }
        }
    }
    return -1;
}

/* The page moved the keyboard itself (el.focus(), el.blur()): a field it
   gave it to takes what is typed next, and one it took it from gives it
   up. Never while the address bar is being typed in. Whether it moved. */
static int follow_page_focus(void) {
    if (bar.focused || !jsdom_live()) return 0;
    int a = jsdom_active();
    if (a == page_active) return 0;
    int was = page_active;
    page_active = a;
    int ck = a >= 0 && a < doc.count ? lay_control_kind(&doc, a) : CTL_NONE;
    if (ck == CTL_TEXT || ck == CTL_PASSWORD || ck == CTL_AREA) focus_control(a);
    else if (focus_node >= 0 && focus_node == was) focus_control(-1);
    return 1;
}

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

/* --- a video's frame, alone -----------------------------------------------
 *
 * A new frame changes nothing but the pixels in its video's box, and the
 * whole page was drawn for it: on a page of any size, most of what a frame
 * cost. So when a frame is all that changed, each video's box is drawn on
 * its own, into a surface just that size -- the page's background, then
 * every item that crosses the box, in order, so what lies over the video (its
 * controls, a caption) stays over it -- and only that box is committed
 * (win_commit_rect). A box that meets the well's rounded corners or its
 * edges, or a page laid out again, is drawn whole as before. */
static u32 *frame_buf;
static int frame_cap;
static int frames_whole, frames_alone;        /* for the console, per page */

static int draw_video_frames(surface *win_s, int win, int view_x, int view_y, int view_w, int view_h, u32 well) {
    int drawn = 0;
    int ix0 = view_x + 2, ix1 = view_x + view_w + UI_SCROLL_W - 2;
    int iy0 = view_y + 2, iy1 = view_y + view_h - 2;
    for (int i = 0; i < page.nitems; i++) {
        const litem *it = &page.items[i];
        if (it->kind != LK_IMAGE || it->node < 0 || it->node >= doc.count || !lay_painted(doc.nodes[it->node].tag)
            || doc.nodes[it->node].tag == T_IFRAME) continue;
        int rx = view_x + UI_PAD + it->x, ry = view_y + 2 + it->y - scroll, rw = it->w, rh = it->h;
        if (rx < ix0) { rw -= ix0 - rx; rx = ix0; }
        if (ry < iy0) { rh -= iy0 - ry; ry = iy0; }
        if (rx + rw > ix1) rw = ix1 - rx;
        if (ry + rh > iy1) rh = iy1 - ry;
        if (rw <= 0 || rh <= 0) continue;                  /* scrolled out of sight */
        /* Not where the well's corners are rounded. */
        if ((ry < iy0 + 8 || ry + rh > iy1 - 8) && (rx < ix0 + 8 || rx + rw > ix1 - 8)) return -1;
        if (rw * rh > frame_cap) {
            if (frame_buf) free(frame_buf);
            frame_buf = (u32 *)malloc((u64)rw * rh * 4);
            frame_cap = frame_buf ? rw * rh : 0;
            if (!frame_buf) return -1;
        }
        surface fs = { frame_buf, rw, rh };
        rect(&fs, 0, 0, rw, rh, page.has_canvas ? page.canvas : well);
        /* The box's top made the surface's: the page as drawn, moved up. */
        int keep = scroll;
        scroll += ry - (view_y + 2);
        draw_x0 = 0;
        draw_x1 = rw;
        draw_page(&fs, view_x + UI_PAD - rx, 0, view_w + rx - (view_x + UI_PAD), rh);
        draw_x0 = -0x7FFFFFFF;
        draw_x1 = 0x7FFFFFFF;
        scroll = keep;
        for (int y = 0; y < rh; y++) {
            u32 *d = win_s->px + (u32)(ry + y) * win_s->w + rx;
            for (int x = 0; x < rw; x++) d[x] = frame_buf[y * rw + x];
        }
        win_commit_rect(win, rx, ry, rw, rh);
        drawn++;
    }
    return drawn;
}

/* Asleep until something arrives for the window, the page has a timer due or
   a request waiting, or something drawn is due to change -- rather than
   looking sixty times a second at a page nobody is touching. */
/* What the console is told of a page once it is all there: where it
   landed, its title and the status. */
static char announce_at[URL_TEXT];
static int  announce_owed, pics_relayout, pics_laid_at;

static void announce_page(void) {
    announce_owed = 0;
    puts("browser: ");
    puts(announce_at);
    puts(" -- ");
    puts(title);
    puts(" -- ");
    puts(status);
    putc('\n');
}

static void browser_wait(int win) {
    /* A tick at the least, even for a timer already due: a page that sets
       a zero timeout from inside one would otherwise have the processor to
       itself, where the old sixteen milliseconds held it to sixty a second. */
    int due = jsdom_next_due();
    if (due >= 0) ui_due(ticks() + (due > 0 ? due : 1));
    /* Pictures still coming are asked after every tick. */
    if (npic_wait) ui_due(ticks() + 1);
    /* And when a kept connection is due to be let go. */
    for (int i = 0; asks_ready && i < ASK_SLOTS; i++)
        if (!asks[i].used && asks[i].job.sock >= 0) ui_due(asks[i].idle_since + ASK_IDLE + 1);
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
    css_mq_extra = UI_SCROLL_W;          /* the page's scroll bar, which a viewport counts */
    int want_width = 0;
    int last_mx = -1, last_my = -1, last_scroll = -1;
    int last_hover = -2;
    int dirty = 1;              /* something changed and a frame is owed */
    int video_due = 0;          /* a video has a new frame (draw_video_frames) */

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

        /* A field the page gave the keyboard to since (a search box opened
           by a script) is typed into from now on. */
        if (follow_page_focus()) dirty = 1;

        /* --- what the keyboard does ----------------------------------------
         *
         * Every key that arrived this frame, not just the first. Somebody
         * typing an address puts two or three into one frame and an address
         * with letters missing out of it goes to the wrong place, or to
         * nowhere, and looks like the network failing. */
        for (int ki = 0; ki < in.nkeys; ki++) {
            u32 raw = in.keys[ki];
            u32 k = KEY_CODE(raw);
            /* ctrl+L is the address bar's, whatever has the keyboard: what
               is typed next replaces the address, as after a click. */
            if (KEY_CTRL(raw) && key_ctrl_letter(raw) == 'l') {
                if (focus_node >= 0) give_focus(-1);
                bar.focused = 1;
                bar_fresh = 1;
                dirty = 1;
                continue;
            }
            /* ctrl+D keeps the page in the bookmarks, or lets it go, and says
               so on the status line and the console. */
            if (KEY_CTRL(raw) && key_ctrl_letter(raw) == 'd' && hist_at >= 0
                && !w_same_fold(hist[hist_at].text, START_ADDRESS)) {
                int kept = bm_toggle(hist[hist_at].text, title[0] ? title : hist[hist_at].text);
                say(kept > 0 ? "kept in the bookmarks: " : kept == 0 ? "let go from the bookmarks: "
                                                                     : "the bookmarks could not be written: ",
                    title[0] ? title : hist[hist_at].text);
                announce_page();
                dirty = 1;
                continue;
            }
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
                    int pasting = KEY_CTRL(raw) && key_ctrl_letter(raw) == 'v';
                    if (bar_fresh && ((!KEY_IS_SPECIAL(k) && k >= ' ' && !KEY_CTRL(raw)) || pasting)) {
                        address[0] = 0;
                        bar.len = 0;
                        bar.cursor = 0;
                    }
                    bar_fresh = 0;
                    ui_field_key(&bar, raw);
                }
                continue;
            }
            /* The page hears the key first and may keep it (jsdom.h,
               jsdom_key_down); a handler may give one of its fields the
               keyboard, which then takes the key, as in a browser. */
            int kept = jsdom_live() && jsdom_key_down(raw);
            if (follow_page_focus()) dirty = 1;
            if (kept) {
                dirty = 1;
            } else if (k == '\t') {
                int at = tab_next();
                if (at >= 0 && (at < scroll || at > scroll + view_h - 40)) scroll = at - view_h / 3;
                dirty = 1;
            } else if (focus_node >= 0) {
                int ck = lay_control_kind(&doc, focus_node);
                if (k == 27) {
                    give_focus(-1);
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
            if (jsdom_live()) jsdom_key_up(raw);
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

        /* A video's new frame is drawn, on its own when nothing else
           changed; one whose size changed is laid out again first
           (jsmedia.h). A canvas drawn on is drawn the same way. */
        if (jsdom_live()) {
            int resized = 0;
            if (jsdom_media_frames(&resized)) video_due = 1;
            if (jsdom_canvas_drawn()) video_due = 1;
            if (resized) { relayout(view_w - UI_PAD * 2); dirty = 1; }
        }

        /* And anything it asked the network for: started, and the answers
           that have come handed over (asks, below). */
        if (jsdom_live() && jsdom_requests() && jsdom_changed()) {
            relayout(view_w - UI_PAD * 2);
            dirty = 1;
        }
        asks_tidy();

        /* And the page's pictures, drawn as they come: laid out again at
           most four times a second while more are coming, and once when the
           last has. */
        if (pics_moved) { pics_moved = 0; pics_relayout = 1; }
        if (npic_wait) {
            int came = pictures_pump();
            if (came & 1) pics_relayout = 1;
            if (came & 2) dirty = 1;
            if (came & 4) { status_compose(); dirty = 1; }
        }
        if (pics_relayout && (!npic_wait || ticks() - pics_laid_at >= 25)) {
            pics_relayout = 0;
            pics_laid_at = ticks();
            relayout(view_w - UI_PAD * 2);
            publish_text();
            dirty = 1;
        }
        if (announce_owed && !npic_wait) announce_page();

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
            nav_from_here();
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

        if (!dirty && video_due && !want_load) {
            ui_theme vt = ui_load_theme();
            int alone = draw_video_frames(&s, win, view_x, view_y, view_w, view_h, vt.well);
            if (alone > 0) { video_due = 0; frames_alone++; browser_wait(win); continue; }
            if (alone == 0) video_due = 0;            /* no video in sight: nothing to draw */
            else dirty = 1;                           /* drawn with the page, and counted so */
        }
        if (!dirty) { browser_wait(win); continue; }
        if (video_due) frames_whole++;
        dirty = 0;
        video_due = 0;

        /* The theme comes off the disk, so it is read on a frame that is
           being drawn rather than on every pass of this loop. */
        ui_theme t = ui_load_theme();

        if (want_load) {
            want_load = 0;
            if (announce_owed) announce_page();  /* the last page, as far as it got */
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
            w_copy(announce_at, sizeof(announce_at), landed, sizeof(announce_at));

            /* And what it came to, on the console, where nobody on the
               desktop sees it: a machine driven over its serial line has no
               other way to know what a page turned into (sitecheck.py). */
            /* What the last page's videos cost: frames drawn on their own,
               and with the whole page (frame_cost.py looks). */
            if (frames_alone || frames_whole) {
                puts("browser: video frames drawn alone ");
                putn(frames_alone);
                puts(", with the page ");
                putn(frames_whole);
                putc('\n');
            }
            frames_alone = frames_whole = 0;
            /* Said once its pictures are in too (announce_page), which is
               what anything reading the console takes this line to mean. */
            announce_owed = 1;
            if (!npic_wait) announce_page();
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
            node_under = page_node(lay_node_at(&page, dx, dy));
        }

        /* :hover is a style, so an element coming under the pointer changes
           what the page looks like and the page has to be laid out again.
           Only when the element actually changed: doing it per frame lays
           out a whole document sixty times a second for a pointer that has
           not left the word it was on. */
        if (node_under != last_hover) {
            last_hover = node_under;
            hover_node = node_under;
            int reach[16], nr = css_hover_reach(&sheet, laid ? laid : &doc, node_under, reach, 16);
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
                    give_focus(node_under);
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
                    give_focus(node_under);
                    jsdom_toggled(node_under);
                } else if (ck == CTL_BUTTON) {
                    const char *t = dom_attr(&doc, node_under, "type");
                    /* The page's keyboard goes to the button; the browser
                       has nothing to type into. */
                    focus_control(-1);
                    jsdom_focused(node_under);
                    page_active = node_under;
                    /* A plain button is the page's; only a submit button
                       sends its form, once the page has heard of it. */
                    int f = form_of(node_under);
                    if (!(t && (lay_same_fold(t, "reset") || lay_same_fold(t, "button")))
                        && !jsdom_submitting(f))
                        submit_form(f);
                } else {
                    give_focus(node_under);
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
            give_focus(-1);

        if (over_link >= 0 && in.released) {
            in.released = 0;
            url_t next;
            const char *href = page.text + page.links[over_link].href;
            /* A stream, to the player: a program of its own (play.c), so the
               page stays where it is and the stream's waiting is not its. */
            if (w_starts_fold(href, "play:")) {
                if (spawn_arg("/bin/play", href + 5) < 0) puts("browser: the player could not be started\n");
            } else if (link_target(over_link, &next)) {
                char text_of[URL_TEXT];
                url_text(&next, text_of, sizeof(text_of));
                set_address(text_of);
                push_history(text_of);
                nav_from_here();
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
                if (link_target(over_link, &u)) {
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
