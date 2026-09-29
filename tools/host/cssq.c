/* Which style rules reach an element on a real page, and what they say.
 *
 *   cssq ADDRESS CLASS-OR-#ID [WIDTH]
 *
 * Fetches the page and its style sheets the way the browser does (the
 * browser's own rules, the page's style elements and linked sheets, media
 * attributes and queries read for WIDTH, 826 by default), then for every
 * element with that class (or id, written #id) prints its tag, and each
 * rule that matches it in the order they apply: the browser's own marked
 * UA, then the page's, with each declaration's property and value.
 *
 * A host tool for finding out why a page looks the way it does
 * (bash tools/host/build.sh cssq; build/host/cssq.exe ...). Not a test, and
 * not on the machine. */
#include "zelr.h"
#include "alloc.h"
#include "web.h"
#include "fetch.h"
#include "dom.h"
#include "css.h"

static ddoc   doc;
static csheet sheet;
static cindex index_;
static cmatch match;
static char   pagebuf[3 * 1024 * 1024];
static char   cssbuf[1024 * 1024];

static const char *prop_name(int p) {
    for (int i = 0; CSS_PROPS[i].name; i++) if (CSS_PROPS[i].prop == p) return CSS_PROPS[i].name;
    return "?";
}

/* A rule's selector, written back out from its compound parts. */
static void put_selector(const crule *r) {
    static const char *const JOIN[] = { "", " ", " > ", " + ", " ~ " };
    for (int k = 0; k < r->sel_n; k++) {
        const csel *c = &sheet.sels[r->sel_at + k];
        if (k) puts(JOIN[c->combinator < 5 ? c->combinator : 1]);
        int any = 0;
        if (c->tag > 0) { puts(HTML_TAGS[c->tag]); any = 1; }
        if (c->tname >= 0) { puts(sheet.text + c->tname); any = 1; }
        if (c->id >= 0) { puts("#"); puts(sheet.text + c->id); any = 1; }
        for (int i = 0; i < c->ncls; i++) {
            if (c->cls[i] >= 0) { puts("."); puts(sheet.text + c->cls[i]); }
            else {
                const char *nm = sheet.text + (-c->cls[i] - 2);
                puts("[");
                puts(nm);
                const char *tv = nm + w_len(nm) + 1;
                if (tv[0] != '?') { char op[2] = { tv[0], 0 }; puts(op); puts("="); puts(tv + 2); }
                puts("]");
            }
            any = 1;
        }
        if (c->pseudo) { puts(":"); putn(c->pseudo); any = 1; }
        if (c->neg >= 0) { puts(":not(...)"); any = 1; }
        if (!any) puts("*");
    }
}

static int media_of(int el, int *lo, int *hi) {
    *lo = *hi = -1;
    const char *m = dom_attr(&doc, el, "media");
    if (!m || !*m) return 1;
    return css_mq(m, w_len(m), lo, hi);
}

int main(int argc, char **argv) {
    if (argc < 3) { puts("cssq ADDRESS CLASS-OR-#ID [WIDTH]\n"); return 2; }
    css_view_w = 826;
    if (argc > 3) {
        css_view_w = 0;
        for (const char *q = argv[3]; *q >= '0' && *q <= '9'; q++) css_view_w = css_view_w * 10 + (*q - '0');
    }

    url_t here;
    if (!url_parse(argv[1], &here)) { puts("not an address\n"); return 1; }
    response_t r;
    int rc = web_get(&here, pagebuf, sizeof(pagebuf), &r);
    if (rc < 0 || r.len <= 0) { puts("could not fetch it\n"); return 1; }
    dom_parse(&doc, r.body, r.len);

    css_init(&sheet);
    css_parse(&sheet, CSS_UA, (int)sizeof(CSS_UA) - 1);
    sheet.ua_rules = sheet.nrules;
    int sheets = 0;
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT) continue;
        int lo, hi;
        if (doc.nodes[i].tag == T_STYLE && media_of(i, &lo, &hi)) {
            for (int t = doc.nodes[i].first; t >= 0; t = doc.nodes[t].next)
                if (doc.nodes[t].kind == DN_TEXT && doc.nodes[t].text >= 0) {
                    const char *s = doc.arena + doc.nodes[t].text;
                    css_parse_in(&sheet, s, w_len(s), lo, hi);
                }
        } else if (doc.nodes[i].tag == T_LINK) {
            const char *rel = dom_attr(&doc, i, "rel"), *href = dom_attr(&doc, i, "href");
            if (!rel || !href || !w_same_fold(rel, "stylesheet") || !media_of(i, &lo, &hi)) continue;
            url_t u;
            response_t cr;
            if (!url_join(&here, href, &u)) continue;
            if (web_get(&u, cssbuf, sizeof(cssbuf), &cr) != 200 || cr.len <= 0) continue;
            css_parse_in(&sheet, cr.body, cr.len, lo, hi);
            sheets++;
        }
    }
    css_index(&sheet, &index_);
    match.hover = -1;

    puts("sheets ");
    putn(sheets);
    puts(", rules ");
    putn(sheet.nrules);
    puts(sheet.overflowed ? " (the sheet overflowed)\n" : "\n");

    const char *want = argv[2];
    int by_id = want[0] == '#';
    int shown = 0;
    for (int el = 0; el < doc.count && shown < 8; el++) {
        if (doc.nodes[el].kind != DN_ELEMENT) continue;
        if (by_id) {
            const char *id = dom_attr(&doc, el, "id");
            if (!id || !w_same(id, want + 1)) continue;
        } else if (!dom_has_class(&doc, el, want, w_len(want))) {
            continue;
        }
        shown++;
        puts("\nelement ");
        putn(el);
        puts(" <");
        puts(doc.nodes[el].tag == T_OTHER && doc.nodes[el].text >= 0 ? doc.arena + doc.nodes[el].text
                                                                       : HTML_TAGS[doc.nodes[el].tag]);
        puts(">\n");
        chit hits[CSS_HITS];
        int n = css_collect(&sheet, &index_, &doc, el, &match, 0, hits);
        for (int pass = 0; pass < 2; pass++) {
            for (int k = 0; k < n; k++) {
                int ru = hits[k].rule;
                if ((ru < sheet.ua_rules) != (pass == 0)) continue;
                const crule *cr = &sheet.rules[ru];
                puts(pass == 0 ? "  UA   " : "  page ");
                puts("rule ");
                putn(ru);
                if (cr->mq_lo >= 0 || cr->mq_hi >= 0) {
                    puts(" [");
                    putn(cr->mq_lo);
                    puts("..");
                    putn(cr->mq_hi);
                    puts("]");
                }
                puts("  ");
                put_selector(cr);
                puts("  {");
                for (int dd = 0; dd < cr->decl_n; dd++) {
                    const cdecl *dc = &sheet.decls[cr->decl_at + dd];
                    puts(" ");
                    puts(prop_name(dc->prop));
                    puts("=");
                    puts(sheet.text + dc->value);
                    puts(";");
                }
                putc('\n');
            }
        }
    }
    if (!shown) puts("nothing on the page has that\n");
    return 0;
}
