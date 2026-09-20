/* What a real page's scripts ask for that this engine has not got.
 *
 * The browser says "a script stopped" on its status line and then runs out
 * of room, which is the right amount to say to somebody reading a page and
 * no use at all to somebody trying to make the page work. This fetches a
 * page, finds the scripts in it, runs each one on its own, and says exactly
 * what stopped it and how far it got.
 *
 * Each script separately rather than in one world, because the first
 * failure otherwise hides every other one, and what is wanted here is the
 * whole list.
 *
 *   exec /bin/jsprobe https://www.google.com/
 */
#include "zelr.h"
#include "alloc.h"
#include "web.h"
#include "fetch.h"
#include "dom.h"
#include "js.h"
#include "jsparse.h"
#include "jsrun.h"

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

static char   page[512 * 1024];
static ddoc   doc;
static response_t reply;

static void number(int v) {
    char tmp[16];
    int t = 0;
    if (!v) tmp[t++] = '0';
    while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    char out[17];
    int w = 0;
    while (t) out[w++] = tmp[--t];
    out[w] = 0;
    puts(out);
}

int main(void) {
    char addr[URL_TEXT];
    if (getarg(addr, sizeof(addr)) <= 0 || !addr[0]) {
        puts("jsprobe needs an address\nJSPROBE_DONE\n");
        return 1;
    }

    url_t u;
    if (!url_parse(addr, &u)) {
        puts("not an address\nJSPROBE_DONE\n");
        return 1;
    }

    int rc = web_get(&u, page, (int)sizeof(page), &reply);
    if (rc < 0) {
        puts("fetch failed: ");
        number(-rc);
        puts("\nJSPROBE_DONE\n");
        return 1;
    }

    puts("status ");
    number(rc);
    puts(", body ");
    number(reply.len);
    puts(" bytes\n");

    dom_parse(&doc, reply.body, reply.len);

    /* What the parse made of it, before anything is laid out: a page that
       renders empty is either one that did not parse or one that parsed and
       was then all hidden, and these two numbers say which. */
    int els = 0, anchors = 0, divs = 0, texts = 0, textlen = 0;
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind == DN_TEXT) {
            texts++;
            const char *t = doc.arena + doc.nodes[i].text;
            while (*t++) textlen++;
            continue;
        }
        if (doc.nodes[i].kind != DN_ELEMENT) continue;
        els++;
        if (doc.nodes[i].tag == T_A) anchors++;
        if (doc.nodes[i].tag == T_DIV) divs++;
    }
    puts("parsed ");
    number(doc.count);
    puts(" nodes: ");
    number(els);
    puts(" elements, ");
    number(divs);
    puts(" divs, ");
    number(anchors);
    puts(" links, ");
    number(texts);
    puts(" runs of text, ");
    number(textlen);
    puts(" characters");
    if (doc.overflowed) puts(" (the document did not fit)");
    putc('\n');

    /* How a page that redirects says so. There are two ways and they need
       different things of a browser: a meta refresh is markup and costs
       nothing to honour, and a script setting location needs the script to
       run. Which one it is decides whether this is a small thing to add. */
    {
        for (int i = 0; i < doc.count; i++) {
            if (doc.nodes[i].kind != DN_ELEMENT) continue;
            if (doc.nodes[i].tag == T_META) {
                const char *eq = dom_attr(&doc, i, "http-equiv");
                if (eq) {
                    puts("meta http-equiv=");
                    puts(eq);
                    puts(" content=");
                    const char *c = dom_attr(&doc, i, "content");
                    puts(c ? c : "(none)");
                    putc('\n');
                }
            }
            if (doc.nodes[i].tag == T_A) {
                const char *h = dom_attr(&doc, i, "href");
                if (h) {
                    puts("link: ");
                    for (int k = 0; h[k] && k < 160; k++) putc(h[k]);
                    putc('\n');
                }
            }
        }
    }

    /* What the page says, with the scripts left out, which is what a reader
       would see: a page whose words are all inside script tags has not been
       rendered badly, it has been sent without any. */
    {
        puts("the page reads: ");
        int w = 0;
        for (int i = 0; i < doc.count && w < 400; i++) {
            if (doc.nodes[i].kind != DN_TEXT) continue;
            int up = doc.nodes[i].parent;
            int inside_script = 0;
            for (int q = up; q >= 0; q = doc.nodes[q].parent)
                if (doc.nodes[q].tag == T_SCRIPT || doc.nodes[q].tag == T_STYLE) {
                    inside_script = 1;
                    break;
                }
            if (inside_script) continue;
            const char *t = doc.arena + doc.nodes[i].text;
            for (int k = 0; t[k] && w < 400; k++) {
                char c = t[k];
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                putc(c);
                w++;
            }
            putc(' ');
            w++;
        }
        if (!w) puts("(nothing outside its scripts)");
        putc('\n');
    }

    /* The biggest run of text, and what is holding it. A page that has been
       swallowed whole shows up here as one enormous run inside whichever
       element the reader failed to find the end of. */
    {
        int big = -1, biglen = 0;
        for (int i = 0; i < doc.count; i++) {
            if (doc.nodes[i].kind != DN_TEXT) continue;
            const char *t = doc.arena + doc.nodes[i].text;
            int k = 0;
            while (t[k]) k++;
            if (k > biglen) { biglen = k; big = i; }
        }
        if (big >= 0) {
            puts("the largest run of text is ");
            number(biglen);
            puts(" characters, inside <");
            puts(dom_tag_name(&doc, doc.nodes[big].parent));
            puts(">, and starts: ");
            const char *t = doc.arena + doc.nodes[big].text;
            for (int k = 0; k < 110 && t[k]; k++)
                putc(t[k] == '\n' ? ' ' : t[k]);
            putc('\n');
        }
    }

    int n = 0, ran = 0, broke = 0;
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT) continue;
        if (doc.nodes[i].tag != T_SCRIPT) continue;
        n++;
        if (dom_attr(&doc, i, "src")) continue;

        int child = doc.nodes[i].first;
        if (child < 0 || doc.nodes[child].kind != DN_TEXT) continue;
        const char *text = doc.arena + doc.nodes[child].text;
        u32 len = 0;
        while (text[len]) len++;
        if (!len) continue;

        ran++;
        static jctx J;
        js_init(&J);
        if (!js_run(&J, text, len)) {
            broke++;
            puts("  script ");
            number(ran);
            puts(" (");
            number((int)len);
            puts(" bytes) stopped at line ");
            number(J.error_line);
            puts(": ");
            puts(J.error);
            putc('\n');
        }
        js_done(&J);
    }

    puts("scripts: ");
    number(n);
    puts(" on the page, ");
    number(ran);
    puts(" with code in them, ");
    number(broke);
    puts(" stopped\n");
    puts("JSPROBE_DONE\n");
    return 0;
}
