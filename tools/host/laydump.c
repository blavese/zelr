/* Where everything on a real page was laid out, item by item.
 *
 *   laydump ADDRESS [TOP BOTTOM] [WIDTH]
 *
 * Loads the page with the browser's own load() -- the same fetches, sheets,
 * scripts and pictures, laid out at WIDTH (844 by default, the width the
 * browser's window lays out at) -- and prints every item whose top is
 * between TOP and BOTTOM (the first 900 pixels by default): its kind, box,
 * and the element it came from, with the element's id and first classes,
 * and a word's text. For finding which box made a page too tall or too
 * narrow.
 *
 * A host tool (bash tools/host/build.sh laydump; build/host/laydump.exe
 * ...): it is browser.c itself with this main in front of it. Not a test,
 * and not on the machine. */
/* Every block the layout finishes, drawn or not (layout.h's LAY_TRACE),
   kept for the last layout only. */
static void lay_trace(int measuring, int node, int x, int y, int w, int h);
#define LAY_TRACE(L, node, x, y, w, h) lay_trace((L)->measuring, node, x, y, w, h)

#undef main
#define main browser_main
#include "browser.c"
#undef main
#define main zelr_main          /* what the host build calls a program's main */

#define TRACE_MAX 200000
static struct { int node, x, y, w, h; } traced[TRACE_MAX];
static int ntraced;
static unsigned traced_gen;

static void lay_trace(int measuring, int node, int x, int y, int w, int h) {
    if (measuring) return;
    if (traced_gen != lay_gen) { traced_gen = lay_gen; ntraced = 0; }
    if (ntraced >= TRACE_MAX) return;
    traced[ntraced].node = node;
    traced[ntraced].x = x;
    traced[ntraced].y = y;
    traced[ntraced].w = w;
    traced[ntraced].h = h;
    ntraced++;
}

static int number(const char *s) {
    int v = 0, neg = *s == '-';
    if (neg) s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static void put_element(int el) {
    if (el < 0 || el >= doc.count) { puts("-"); return; }
    const dnode *n = &doc.nodes[el];
    if (n->kind != DN_ELEMENT) { puts("#text"); return; }
    puts(n->tag == T_OTHER && n->text >= 0 ? doc.arena + n->text : HTML_TAGS[n->tag]);
    const char *id = dom_attr(&doc, el, "id");
    if (id && *id) { puts("#"); puts(id); }
    const char *cls = dom_attr(&doc, el, "class");
    if (cls && *cls) {
        puts(".");
        int k = 0;
        for (const char *c = cls; *c && k < 48; c++, k++) {
            char one[2] = { *c == ' ' ? '.' : *c, 0 };
            puts(one);
        }
    }
    puts("  ");
    putn(el);
}

int main(int argc, char **argv) {
    if (argc < 2) { puts("laydump ADDRESS [TOP BOTTOM] [WIDTH]\n"); return 2; }
    int top = 0, bottom = 900, width = 844;
    if (argc > 3) { top = number(argv[2]); bottom = number(argv[3]); }
    if (argc > 4) width = number(argv[4]);

    src = (char *)map(SRC_MAX, PROT_READ | PROT_WRITE);
    cssbuf = (char *)map(CSS_MAX, PROT_READ | PROT_WRITE);
    scriptbuf = (char *)map(SCRIPT_MAX, PROT_READ | PROT_WRITE);
    doc_mem = (ddoc *)map(sizeof(ddoc), PROT_READ | PROT_WRITE);
    sheet_mem = (csheet *)map(sizeof(csheet), PROT_READ | PROT_WRITE);
    page_mem = (ldoc *)map(sizeof(ldoc), PROT_READ | PROT_WRITE);
    if (!src || !cssbuf || !doc_mem || !sheet_mem || !page_mem) return 1;
    css_view_h = 540;

    set_address(argv[1]);
    load(address, width, 0);

    static const char *const KIND[] = { "?", "box", "text", "bullet", "image", "field" };
    puts("items ");
    putn(page.nitems);
    puts(", height ");
    putn(page.height);
    putc('\n');
    for (int i = 0; i < page.nitems; i++) {
        const litem *it = &page.items[i];
        if (it->y < top || it->y > bottom) continue;
        putn(i);
        puts(" ");
        puts(it->kind < 6 ? KIND[it->kind] : "?");
        puts(" ");
        putn(it->x);
        puts(",");
        putn(it->y);
        puts(" ");
        putn(it->w);
        puts("x");
        putn(it->h);
        puts("  ");
        put_element(it->node);
        if (it->kind == LK_TEXT && it->at >= 0) {
            puts("  \"");
            puts(page.text + it->at);
            puts("\"");
        }
        putc('\n');
    }

    /* And every block, drawn or not, in the order they were finished
       (children before their parents). */
    puts("\nblocks\n");
    for (int i = 0; i < ntraced; i++) {
        if (traced[i].y < top || traced[i].y > bottom) continue;
        puts("  ");
        putn(traced[i].x);
        puts(",");
        putn(traced[i].y);
        puts(" ");
        putn(traced[i].w);
        puts("x");
        putn(traced[i].h);
        puts("  ");
        put_element(traced[i].node);
        putc('\n');
    }
    return 0;
}
