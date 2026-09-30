/* What each of a real page's scripts did, one line each.
 *
 *   scriptdump ADDRESS [SECONDS] [text] [tree] ['js=EXPRESSION' ...] [prof]
 *
 * Loads the page with the browser's own load() -- the same fetches, the same
 * limits on how many scripts and how large, the same document and bindings
 * -- and prints, for every script element that is run, where it came from,
 * how long it was, and what stopped it, or "ok". Then how much of the page's
 * script memory was in use, and the line the browser would have printed.
 *
 * With "tree", the page is laid out and printed as it is drawn, an element
 * a line, with the shadow trees its scripts attached in place (browser.c,
 * page_drawn): what a page whose scripts all ran shows, and where.
 *
 * Each js= is evaluated in the page once it has run, and what it came to is
 * printed: a page's own state, asked for the way its console would be.
 *
 * "prof", in a build with HOST_CFLAGS=-DJS_ALLOC_PROFILE, prints where the
 * page's script memory went, by the calls that asked for it (js.h).
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
static int show_tree;            /* "tree": the page as it is drawn */

static void put_cut(const char *s, int most) {
    int k = 0;
    for (; s[k] && k < most; k++) {
        char one[2] = { s[k] == '\n' || s[k] == '\t' ? ' ' : s[k], 0 };
        puts(one);
    }
    if (s[k]) puts("...");
}

/* An element a line, indented by its depth, with its id, its class and the
   shadow tree marks (dom_flat); words in quotes. Scripts, sheets and
   drawings are one line each. */
static void outline(const ddoc *d, int most) {
    int lines = 0;
    for (int i = d->root; i >= 0 && lines < most; ) {
        const dnode *n = &d->nodes[i];
        int depth = 0;
        for (int p = n->parent; p >= 0 && depth < 200; p = d->nodes[p].parent) depth++;
        int skip = 0;
        if (n->kind == DN_ELEMENT) {
            for (int k = 0; k < depth && k < 40; k++) puts(" ");
            puts(dom_tag_name(d, i));
            const char *id = dom_attr(d, i, "id"), *cl = dom_attr(d, i, "class");
            if (id && *id) { puts("#"); put_cut(id, 30); }
            if (cl && *cl) { puts("."); put_cut(cl, 40); }
            if (dom_attr(d, i, "data-zh")) puts(" [host]");
            if (dom_attr(d, i, "data-zl")) puts(" [slotted]");
            putc('\n');
            lines++;
            skip = n->tag == T_SCRIPT || n->tag == T_STYLE || w_same(dom_tag_name(d, i), "svg");
        } else if (n->kind == DN_TEXT && n->text >= 0) {
            const char *t = d->arena + n->text;
            int words = 0;
            for (const char *q = t; *q && !words; q++) words = *q != ' ' && *q != '\n' && *q != '\t' && *q != '\r';
            if (words) {
                for (int k = 0; k < depth && k < 40; k++) puts(" ");
                puts("\"");
                while (*t == ' ' || *t == '\n' || *t == '\t' || *t == '\r') t++;
                put_cut(t, 60);
                puts("\"\n");
                lines++;
            }
        }
        if (!skip) { i = dom_next(d, i, -1); continue; }
        /* Past what is inside it. */
        while (i >= 0 && d->nodes[i].next < 0) i = d->nodes[i].parent;
        if (i >= 0) i = d->nodes[i].next;
    }
    if (lines >= most) puts("...\n");
}

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
    for (int a = 2; a < argc; a++) if (w_same(argv[a], "tree")) show_tree = 1;

    src = (char *)map(SRC_MAX, PROT_READ | PROT_WRITE);
    cssbuf = (char *)map(CSS_MAX, PROT_READ | PROT_WRITE);
    scriptbuf = (char *)map(SCRIPT_MAX, PROT_READ | PROT_WRITE);
    replybuf = (char *)map(REPLY_MAX, PROT_READ | PROT_WRITE);
    doc_mem = (ddoc *)map(sizeof(ddoc), PROT_READ | PROT_WRITE);
    char *arena = (char *)map(DOC_ARENA, PROT_READ | PROT_WRITE);   /* as the browser's (main) */
    if (doc_mem) dom_use_arena(doc_mem, arena, DOC_ARENA);
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

    for (int a = 2; a < argc; a++) {
        if (!w_starts_fold(argv[a], "js=") || !jsdom_live()) continue;
        const char *code = argv[a] + 3;
        jd_J.error[0] = 0;
        jval v = js_eval_source(&jd_J, js_str_n(&jd_J, code, (u32)w_len(code)), jd_J.global, jd_J.global_lex,
                                js_from_obj(jd_J.global_obj));
        puts("js: ");
        if (jd_J.sig != JS_OK) {
            puts("stopped: ");
            puts(jd_J.error);
            jd_J.sig = JS_OK;
        } else {
            jstr *t = js_to_str(&jd_J, v);
            for (u32 i = 0; t && i < t->len; i++) { char one[2] = { t->s[i], 0 }; puts(one); }
        }
        puts("\n");
    }

    for (int a = 2; a < argc; a++) {
        if (!w_same(argv[a], "prof")) continue;
#ifdef JS_ALLOC_PROFILE
        js_prof_report(60);
#else
        puts("prof: built without HOST_CFLAGS=-DJS_ALLOC_PROFILE" "\n");
#endif
    }

    if (show_tree) {
        relayout(844);
        const ddoc *d = laid ? laid : &doc;
        puts("shadow trees ");
        putn(trees_n);
        puts(", laid out as ");
        putn(page.nitems);
        puts(" items and ");
        putn(lay_words(&page));
        puts(" words, ");
        putn(page.height);
        puts(" pixels down\n");
        outline(d, 1500);
    }
    return 0;
}
