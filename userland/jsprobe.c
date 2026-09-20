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
