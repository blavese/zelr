/* What each of a real page's scripts did, one line each.
 *
 *   scriptdump ADDRESS [SECONDS] [text]
 *
 * Loads the page with the browser's own load() -- the same fetches, the same
 * limits on how many scripts and how large, the same document and bindings
 * -- and prints, for every script element that is run, where it came from,
 * how long it was, and what stopped it, or "ok". Then how much of the page's
 * script memory was in use, and the line the browser would have printed.
 *
 * The browser's own line names only the first thing that stopped, which is
 * the right thing to show a reader and not enough to tell which of thirty
 * scripts to look at next.
 *
 * A host tool (bash tools/host/build.sh scriptdump), browser.c itself with
 * this main in front of it, as laydump is. Not a test, and not on the
 * machine. */
#undef main
#define main browser_main
#include "browser.c"
#undef main
#define main zelr_main

static int dumped;
static int show_text;            /* "text" after the address */

static int number_of(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

static void one_done(int index, const char *from, u32 len, const char *err) {
    dumped++;
    puts("script ");
    putn(dumped);
    puts(" (node ");
    putn(index);
    puts(") ");
    if (from && *from) {
        int k = 0;
        for (const char *p = from; *p && k < 90; p++, k++) {
            char one[2] = { *p, 0 };
            puts(one);
        }
        if (w_len(from) > 90) puts("...");
    } else {
        puts("inline");
    }
    puts(" ");
    putn((int)len);
    puts(" bytes: ");
    puts(err && *err ? (len ? "stopped" : err) : "ok");
    puts("  [");
    putn((int)(jd_J.allocated / 1024));
    puts(" KB]\n");

    /* With "text": an inline script that stopped is printed whole between
       markers, to find what the engine could not read. One from a file has
       its address above, to fetch. */
    if (show_text && err && *err && len && !(from && *from)) {
        puts("---- script text\n");
        for (int c = jd_doc->nodes[index].first; c >= 0; c = jd_doc->nodes[c].next)
            if (jd_doc->nodes[c].kind == DN_TEXT && jd_doc->nodes[c].text >= 0)
                puts(jd_doc->arena + jd_doc->nodes[c].text);
        puts("\n---- end\n");
    }
}

/* Every error, as it happens, a script's own just before its line and a
   handler's, timer's or request's wherever it came. */
static void any_error(const char *err, int line) {
    puts("  error: ");
    puts(err);
    if (line > 0) { puts(" at line "); putn(line); }
    putc('\n');
}

/* What the page's scripts write to the console, which is where a library
   that catches its own errors says what they were (Alpine does). */
static void console_line(const char *s, u32 n) {
    static int fresh = 1;
    for (u32 i = 0; i < n; i++) {
        if (fresh) { puts("  console: "); fresh = 0; }
        char one[2] = { s[i], 0 };
        puts(one);
        if (s[i] == '\n') fresh = 1;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { puts("scriptdump ADDRESS [SECONDS] [text]\n"); return 2; }
    for (int a = 2; a < argc; a++) if (w_same(argv[a], "text")) show_text = 1;

    src = (char *)map(SRC_MAX, PROT_READ | PROT_WRITE);
    cssbuf = (char *)map(CSS_MAX, PROT_READ | PROT_WRITE);
    scriptbuf = (char *)map(SCRIPT_MAX, PROT_READ | PROT_WRITE);
    replybuf = (char *)map(REPLY_MAX, PROT_READ | PROT_WRITE);
    doc_mem = (ddoc *)map(sizeof(ddoc), PROT_READ | PROT_WRITE);
    sheet_mem = (csheet *)map(sizeof(csheet), PROT_READ | PROT_WRITE);
    page_mem = (ldoc *)map(sizeof(ldoc), PROT_READ | PROT_WRITE);
    if (!src || !cssbuf || !doc_mem || !sheet_mem || !page_mem) return 1;
    css_view_h = 540;

    jd_script_done = one_done;
    jd_on_error = any_error;
    js_print_hook = console_line;
    set_address(argv[1]);
    int t0 = ticks();
    load(address, 844, 0);
    int t1 = ticks();

    /* Then what the page does once it is up, for a while: its timers and
       its requests, as the browser's loop would run them. */
    int secs = argc > 2 ? number_of(argv[2]) : 3;
    int until = ticks() + secs * 100, timers = 0, asked = 0;
    while (jsdom_live() && ticks() - until < 0) {
        int t = jsdom_timers(), r = jsdom_requests();
        timers += t;
        asked += r;
        if (!t && !r) sleep_ms(10);
    }
    puts("afterwards: ");
    putn(timers);
    puts(" timers ran, ");
    putn(asked);
    puts(" requests made\n");

    /* And the ones that were not run at all, with the type that kept them
       out: data, modules not started, and templates' scripts. */
    for (int i = 0; i < doc.count; i++) {
        if (doc.nodes[i].kind != DN_ELEMENT || doc.nodes[i].tag != T_SCRIPT) continue;
        const char *ty = dom_attr(&doc, i, "type");
        if (jd_script_type_runs(ty)) continue;
        if (jd_module_type(ty) && jd_is_started(i)) continue;
        const char *s = dom_attr(&doc, i, "src");
        puts("not run (node ");
        putn(i);
        puts(") type=");
        puts(ty ? ty : "");
        if (s) { puts(" src="); puts(s); }
        putc('\n');
    }

    /* The module map: how each one ended, and the ones that did not
       finish, by address. */
    static const char *const MSTATE[] = { "never fetched", "read", "linked", "running", "waiting",
                                          "awaiting", "done", "failed" };
    int by[8] = { 0 };
    for (int i = 0; i < jd_nmods; i++) if (jd_mods[i].state >= 0 && jd_mods[i].state < 8) by[jd_mods[i].state]++;
    if (jd_nmods) {
        puts("modules ");
        putn(jd_nmods);
        for (int k = 0; k < 8; k++) {
            if (!by[k]) continue;
            puts(", ");
            putn(by[k]);
            putc(' ');
            puts(MSTATE[k]);
        }
        putc('\n');
        for (int i = 0; i < jd_nmods; i++) {
            int st = jd_mods[i].state;
            if (st == JM_DONE) continue;
            puts("  module ");
            puts(st >= 0 && st < 8 ? MSTATE[st] : "?");
            puts(": ");
            puts(jd_mods[i].key);
            putc('\n');
        }
    }

    puts("scripts ");
    putn(dumped);
    puts(", memory ");
    putn((int)(jd_J.allocated / 1024));
    puts(" KB, tree ");
    putn(jd_J.nnodes);
    puts(" nodes (");
    putn((int)((long long)jd_J.ncap * (long long)sizeof(jnode) / 1024));
    puts(" KB held), ");
    putn((t1 - t0) * 10);
    puts(" ms, script files ");
    putn(scripts_bytes / 1024);
    puts(" KB\n");
    puts("status: ");
    puts(status);
    putc('\n');
    return 0;
}
