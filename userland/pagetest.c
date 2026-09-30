/* The document, and what a script can do to it — with no screen involved.
 *
 * All of this used to be checked by driving the browser, taking a picture of
 * the screen and counting pixels of a particular colour. That works, and it
 * is the only way to know the page was really drawn, and it costs four
 * minutes and a virtual machine with a display. Most of what it was asking
 * has nothing to do with the screen: whether an element can be found by its
 * id, whether writing to one changes the document, whether a script that
 * sets a class ends up with that class on that element.
 *
 * So those are asked here, in a program that prints lines and exits, and the
 * picture is left to ask the one question only a picture can: did it reach
 * the glass.
 */
#include "zelr.h"
#include "alloc.h"
#include "dom.h"
#include "css.h"
#include "fetch.h"
#include "jsdom.h"

static int failed;

static void ok(const char *what, int cond) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    putc('\n');
    if (!cond) failed++;
}

static void oks(const char *what, const char *got, const char *want) {
    /* An attribute that is not there is nothing, and a check that met one
       took the whole program down instead of failing. */
    if (!got) got = "(nothing)";
    int same = 1;
    for (int i = 0;; i++) {
        if (got[i] != want[i]) { same = 0; break; }
        if (!got[i]) break;
    }
    puts(same ? "  PASS  " : "  FAIL  ");
    puts(what);
    if (!same) { puts("  got: "); puts(got); puts("  wanted: "); puts(want); }
    putc('\n');
    if (!same) failed++;
}

static ddoc page;

/* Somewhere for a selector handed to querySelector to be parsed. The browser
   lends its own; this lends one of its own so the tests can ask. */
static csheet qsheet;
static char text[512];

static void load(const char *html) {
    int n = 0;
    while (html[n]) n++;
    dom_parse(&page, html, n);
}

static const char *content_of(int el) {
    dom_text_content(&page, el, text, (int)sizeof(text));
    return text;
}

/* What jsdom_run used to be: open a world, run what is in the page, and say
   whether anything changed. The browser does these three separately now
   because it keeps the world afterwards, and so does everything below here
   that clicks on something. */
static int run_scripts(ddoc *d, char *err, int errcap, int *changed) {
    if (changed) *changed = 0;
    if (err && errcap) err[0] = 0;
    if (!jsdom_open(d, &qsheet)) return 0;
    int ran = jsdom_scripts(err, errcap);
    jsdom_loaded();
    if (changed) *changed = jsdom_changed();
    return ran;
}

/* Runs whatever timers are due until enough of them have, or until the clock
   says it is not going to happen.
 *
 * Polled against the clock rather than written as sleep-then-check, because
 * a sleep that returns without sleeping is something this kernel does when
 * nothing else is ready to run -- and a timer test built on one reports a
 * working timer as broken, which is how the first version of this read. */
static int pump_until(int want, int ms) {
    int ran = 0;
    int until = ticks() + (ms * 100 + 999) / 1000;
    while (ran < want && ticks() - until < 0) ran += jsdom_timers();
    return ran;
}

/* A page run, and what its scripts left in the title -- or, when one of
   them stopped, what stopped it, so a failure says why rather than showing
   the title it never got to write. */
static char said[256];

static const char *titled(const char *html) {
    load(html);
    char err[128];
    run_scripts(&page, err, (int)sizeof(err), 0);
    const char *t = page.title >= 0 ? page.arena + page.title : "";
    const char *from = err[0] ? err : t;
    int i = 0;
    for (; from[i] && i < (int)sizeof(said) - 1; i++) said[i] = from[i];
    said[i] = 0;
    return said;
}

/* Scripts from a src, for the checks that insert one: whatever address is
   asked for, the text it names after "#". */
static int fake_script(const char *src, const char **out) {
    for (const char *p = src; *p; p++)
        if (*p == '#') { *out = p + 1; int n = 0; while (p[1 + n]) n++; return n; }
    return 0;
}

/* Module files, answered by their whole address. */
static const char *const MODFILES[][2] = {
    { "https://site.test/m.js", "export default 'D'; export const a = 1; export let b = 2;" },
    { "https://site.test/live.js", "export let n = 1; export function bump() { n++; }" },
    { "https://site.test/re.js", "export * from './m.js'; export { a as z } from './m.js';" },
    { "https://site.test/meta.js", "export const url = import.meta.url; export const next = import.meta.resolve('./m.js');" },
    { "https://site.test/dir/x.js", "export default 'X';" },
    { "https://site.test/order.js", "import './order2.js'; window.order = (window.order || '') + '1';" },
    { "https://site.test/order2.js", "window.order = (window.order || '') + '2';" },
    { "https://site.test/tla.js", "export let v = 1; await Promise.resolve(); v = 2;" },
    { "https://site.test/tlauser.js", "import { v } from './tla.js'; export const seen = v;" },
    { "https://site.test/tla2.js", "export let v = 'early'; await Promise.resolve(); await null; v = 'late';" },
    { "https://site.test/tlabad.js", "await null; throw new Error('late');" },
    { "https://site.test/throws.js", "throw new Error('boom');" },
    { "https://site.test/tla3.js", "export let v = 'early'; await new Promise(function (r) { setTimeout(r, 0); }); v = 'late';" },
    { "https://site.test/tla3user.js", "import { v } from './tla3.js'; window.seen3 = v;" },
    { "https://site.test/tlabad3.js", "await new Promise(function (r) { setTimeout(r, 0); }); throw new Error('later');" },
    { 0, 0 },
};

static int fake_modules(const char *src, const char **out) {
    for (int i = 0; MODFILES[i][0]; i++)
        if (w_same(src, MODFILES[i][0])) {
            *out = MODFILES[i][1];
            int n = 0;
            while ((*out)[n]) n++;
            return n;
        }
    return 0;
}

/* The browser's own cookie jar (fetch.h), for a page at one address, which
   is what browser.c hands a page too. */
static url_t jar_at;
static int jar_get(char *out, int cap) { return ck_cookies_for(&jar_at, out, cap, 1); }
static void jar_set(const char *line) { ck_take_line(&jar_at, line, 1); }

/* Requests a page makes, written down instead of sent, and answered: JSON
   for an address with "json" in it, a failure for one with "down", and "ok"
   for the rest. */
static char asked[512];
static int fake_request(const char *method, const char *url, const char *body, const char *type,
                        jd_reply *out) {
    int w = 0;
    const char *parts[4] = { method, url, body ? body : "", type ? type : "-" };
    for (int k = 0; k < 4; k++) {
        if (k) asked[w++] = ' ';
        for (const char *p = parts[k]; *p && w < (int)sizeof(asked) - 2; p++) asked[w++] = *p;
    }
    asked[w] = 0;
    int json = 0, down = 0;
    for (const char *p = url; *p; p++) {
        if (p[0] == 'j' && p[1] == 's' && p[2] == 'o' && p[3] == 'n') json = 1;
        if (p[0] == 'd' && p[1] == 'o' && p[2] == 'w' && p[3] == 'n') down = 1;
    }
    out->url = url;
    out->type = json ? "application/json" : "text/plain";
    out->status = down ? 0 : 200;
    out->body = json ? "{\"n\": 7, \"list\": [1, 2]}" : "ok";
    out->len = w_len(out->body);
    return out->len;
}

/* Where the page sent the browser, and what address it said it was at. */
static char went[512], moved[512];
static void fake_navigate(const char *url, int replace) {
    int w = 0;
    for (const char *p = replace ? "replace " : "go "; *p; p++) went[w++] = *p;
    for (const char *p = url; *p && w < (int)sizeof(went) - 1; p++) went[w++] = *p;
    went[w] = 0;
}
static void fake_address(const char *url, int push) {
    int w = 0;
    for (const char *p = push ? "push " : "replace "; *p; p++) moved[w++] = *p;
    for (const char *p = url; *p && w < (int)sizeof(moved) - 1; p++) moved[w++] = *p;
    moved[w] = 0;
}
static int history_moved;
static void fake_history_go(int d) { history_moved = d; }
static int fake_history_length(void) { return 4; }

/* A machine with a little memory free, and one with plenty. */
static long long little_memory(void) { return 2 * 1024 * 1024; }
static long long much_memory(void) { return 512LL * 1024 * 1024; }
static long long some_memory(void) { return 200LL * 1024 * 1024; }
static long long a_little_more(void) { return 64LL * 1024 * 1024; }

/* A machine whose generator has found nothing to be unpredictable with. */
static int no_random(void *buf, int len) { (void)buf; (void)len; return -1; }

/* Where the page asked to be scrolled to. */
static int scrolled_to = -1;
static void fake_scroll(int y) { scrolled_to = y; }

/* A style as a pretend layout worked it out: a flex row in a colour, bold. */
static int fake_style(int node, cstyle *out) {
    (void)node;
    css_default_style(out, 16);
    out->display = D_FLEX;
    out->color = 0x102030;
    out->bold = 1;
    out->mt = 12;
    return 1;
}

/* Where a pretend layout put everything: one box, for every element, as
   wide as box_w (a layout that changes it is a check's to make). */
static int box_w = 50;
static int fake_box(int node, int *x, int *y, int *w, int *h) {
    (void)node;
    *x = 10; *y = 100; *w = box_w; *h = 20;
    return 1;
}

/* What a pretend layout drew at a point of the page: the word inside the
   element with the id "hit", across the top hundred pixels, and nothing
   below them. */
static int fake_point(int x, int y) {
    (void)x;
    int hit = dom_by_id(&page, "hit");
    return y < 100 && hit >= 0 ? page.nodes[hit].first : -1;
}

/* A style with padding and borders: 4 and 2 of padding across and down, a
   border of 1 at each side. */
static int padded_style(int node, cstyle *out) {
    (void)node;
    css_default_style(out, 16);
    out->display = D_BLOCK;
    out->pl = out->pr = 4;
    out->pt = out->pb = 2;
    out->bl = out->br = 1;
    return 1;
}

int main(void) {
    puts("a document, and a script on it\n");

    /* --- the document itself ---------------------------------------------- */
    load("<html><head><title>a title</title></head><body>"
         "<h1 id=head>heading</h1><p id=body class=note>words</p>"
         "<p>more words</p></body></html>");

    int head = dom_by_id(&page, "head");
    ok("an element can be found by its id", head >= 0);
    oks("and it is the one that was asked for", dom_tag_name(&page, head), "h1");
    oks("and its text is what was between its tags", content_of(head), "heading");

    int para = dom_by_id(&page, "body");
    oks("an attribute comes back as it was written",
        dom_attr(&page, para, "class"), "note");
    ok("and one that is not there comes back as nothing",
       dom_attr(&page, para, "data-nothing") == 0);

    /* --- changing one ------------------------------------------------------
     *
     * The case that mattered: an element that already had an attribute could
     * not be given another one, because the attributes of an element are one
     * run and only the newest element has room after its own. A script
     * setting a class on an element that had none is the commonest thing a
     * script does, and it was quietly refused. */
    ok("an attribute can be changed", dom_attr_set(&page, para, "class", "lit"));
    oks("and the change is what comes back", dom_attr(&page, para, "class"), "lit");

    ok("a longer value is taken too",
       dom_attr_set(&page, para, "class", "considerably-longer"));
    oks("and it is not truncated",
        dom_attr(&page, para, "class"), "considerably-longer");

    ok("an element with no such attribute can be given one",
       dom_attr_set(&page, head, "class", "added"));
    oks("and it is there afterwards", dom_attr(&page, head, "class"), "added");
    oks("and the attribute it already had is untouched",
        dom_attr(&page, head, "id"), "head");

    /* And the element next door did not move with it. */
    oks("and so is the one next to it", dom_attr(&page, para, "class"),
        "considerably-longer");

    dom_set_text(&page, head, "replaced", 8);
    oks("an element's text can be replaced", content_of(head), "replaced");

    /* --- whitespace, which is content or is not depending on what is beside it */
    {
        load("<body><p><a>Gmail</a> <a>Images</a></p>"
             "<div>one</div>  <div>two</div>"
             "<p>  lots   of   room  </p></body>");

        oks("a space between two inline things separates them",
            content_of(dom_by_tag(&page, T_P, 0)), "Gmail Images");

        /* After a block the line has already ended, so the space would sit
           at the start of the next line where it is dropped anyway. */
        oks("and one after a block element is not kept",
            content_of(dom_by_tag(&page, T_DIV, 1)), "two");

        /* What is inside an element is left exactly as written; collapsing
           runs is the line breaker's business, not the parser's. */
        ok("and a run inside an element is left for the line breaker",
           content_of(dom_by_tag(&page, T_P, 1))[0] != 0);
    }

    /* --- a script on the page ---------------------------------------------- */
    {
        load("<html><head><title>before</title></head><body>"
             "<h1 id=head>before</h1><p id=sum>nothing</p>"
             "<div id=box>a box</div><p>plain</p>"
             "<script>"
             "var t = 0;"
             "for (var i = 1; i <= 10; i++) t = t + i;"
             "document.getElementById('head').textContent = 'after';"
             "document.getElementById('sum').textContent = 'sum ' + t;"
             "document.getElementById('box').className = 'lit';"
             "document.title = 'changed';"
             "</script></body></html>");

        char err[128];
        int changed = 0;
        int ran = run_scripts(&page, err, (int)sizeof(err), &changed);

        ok("a script on the page runs", ran == 1);
        oks("and says nothing went wrong", err, "");
        ok("and says it changed the document", changed == 1);

        oks("an element it wrote to says what it wrote",
            content_of(dom_by_id(&page, "head")), "after");
        oks("including a number it worked out",
            content_of(dom_by_id(&page, "sum")), "sum 55");
        oks("a class it set is on the element",
            dom_attr(&page, dom_by_id(&page, "box"), "class"), "lit");
        oks("and the title it set is the document's",
            page.title >= 0 ? page.arena + page.title : "", "changed");

        /* Nothing it did not touch moved. */
        int last = dom_by_tag(&page, T_P, 1);
        oks("and an element it never named is as it was",
            content_of(last), "plain");
    }

    /* --- reading the document back ----------------------------------------- */
    {
        load("<body><p id=one>first</p><p>second</p><p>third</p>"
             "<script>"
             "var ps = document.getElementsByTagName('p');"
             "document.getElementById('one').textContent = 'of ' + ps.length;"
             "</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("a script can count the elements of a kind",
            content_of(dom_by_id(&page, "one")), "of 3");
    }

    /* --- and what it is told it cannot do ------------------------------------ */
    {
        load("<body><p id=x>here</p>"
             "<script>document.getElementById('nope').textContent = 'no';</script>"
             "</body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);

        /* Reaching into nothing is an error, and the page says so rather
           than carrying on as though it had worked. */
        ok("writing to an element that is not there is an error", err[0] != 0);
        oks("and the rest of the document is untouched",
            content_of(dom_by_id(&page, "x")), "here");
    }

    {
        load("<body><p id=x>here</p><script>this is not javascript;;;</script></body>");
        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        ok("a script that will not parse is reported", err[0] != 0);
        oks("and the page is still the page", content_of(dom_by_id(&page, "x")), "here");
    }

    {
        load("<body><p id=x>here</p></body>");
        char err[128];
        int changed = 0;
        int ran = run_scripts(&page, err, (int)sizeof(err), &changed);
        ok("a page with no script costs nothing", ran == 0 && changed == 0);
    }

    /* --- a page that can be clicked on ---------------------------------------
     *
     * The world stays open once the scripts have run, which is the whole
     * point of it: a handler is a piece of a program that runs after the
     * program has finished, and before this there was nothing left for one
     * to run in.
     */
    {
        load("<body><button id=b onclick=\"document.getElementById('out')"
             ".textContent = 'clicked'\">press</button>"
             "<p id=out>nothing</p></body>");

        char err[128];
        int changed = 0;
        int ran = run_scripts(&page, err, (int)sizeof(err), &changed);

        ok("a page whose only script is a handler attribute still opens one",
           ran == 0 && jsdom_live());
        oks("and nothing has happened to it yet",
            content_of(dom_by_id(&page, "out")), "nothing");

        jsdom_click(dom_by_id(&page, "b"));
        oks("until it is clicked",
            content_of(dom_by_id(&page, "out")), "clicked");
        ok("and the browser is told it has to lay the page out again",
           jsdom_changed() == 1);
    }

    /* A listener put on by a script, fired long after that script returned,
       counting in a variable that had to survive with it. */
    {
        load("<body><div id=outer><span id=inner>x</span></div>"
             "<p id=out>0</p>"
             "<script>"
             "var n = 0;"
             "document.getElementById('outer').addEventListener('click',"
             " function(e){ n = n + 1;"
             " document.getElementById('out').textContent = 'n ' + n; });"
             "</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("a listener does not fire merely for being registered",
            content_of(dom_by_id(&page, "out")), "0");

        jsdom_click(dom_by_id(&page, "inner"));
        oks("a click on a child reaches a listener on its parent",
            content_of(dom_by_id(&page, "out")), "n 1");

        jsdom_click(dom_by_id(&page, "inner"));
        oks("and the variable it counts in outlived the script that made it",
            content_of(dom_by_id(&page, "out")), "n 2");
    }

    /* Which element was hit, rather than which one is listening. */
    {
        load("<body><div id=outer><span id=inner>x</span></div><p id=out>?</p>"
             "<script>"
             "document.getElementById('outer').addEventListener('click',"
             " function(e){ document.getElementById('out').textContent ="
             " e.target.id; });"
             "</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        jsdom_click(dom_by_id(&page, "inner"));
        oks("the event names the element that was actually clicked",
            content_of(dom_by_id(&page, "out")), "inner");
    }

    /* The two answers a click can get, which are the whole behaviour of a
       link that is really a button. */
    {
        load("<body><a id=a href='/next' onclick=\"event.preventDefault()\">"
             "go</a></body>");
        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        ok("a handler that refuses the default says so",
           jsdom_click(dom_by_id(&page, "a")) == 1);
    }
    {
        load("<body><a id=a href='/next'>go</a>"
             "<script>document.getElementById('a').addEventListener('click',"
             " function(e){ });</script></body>");
        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        ok("and one that merely looks at it does not",
           jsdom_click(dom_by_id(&page, "a")) == 0);
    }

    {
        load("<body><div id=outer><span id=inner>x</span></div><p id=out>no</p>"
             "<script>"
             "document.getElementById('outer').addEventListener('click',"
             " function(e){ document.getElementById('out').textContent ="
             " 'reached'; });"
             "document.getElementById('inner').addEventListener('click',"
             " function(e){ e.stopPropagation(); });"
             "</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        jsdom_click(dom_by_id(&page, "inner"));
        oks("a handler that stops the event keeps it from the parent",
            content_of(dom_by_id(&page, "out")), "no");
    }

    {
        load("<body><p id=out>no</p>"
             "<script>document.addEventListener('DOMContentLoaded',"
             " function(){ document.getElementById('out').textContent ="
             " 'ready'; });</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("a page waiting to be told the document is ready is told",
            content_of(dom_by_id(&page, "out")), "ready");
    }

    /* A handler that throws is the page's problem and not the browser's: it
       is reported, and the click after it still works. */
    {
        load("<body><p id=out>fine</p>"
             "<button id=b onclick=\"nothing.at.all()\">press</button>"
             "</body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        jsdom_click(dom_by_id(&page, "b"));
        ok("a handler that throws is reported", jsdom_error()[0] != 0);
        ok("and the world is still running afterwards", jsdom_live());
        oks("and the page is still the page",
            content_of(dom_by_id(&page, "out")), "fine");
    }

    /* --- changing the shape of it --------------------------------------------
     *
     * Rewriting the words in a box is about a third of what a page asks a
     * script for. The rest is adding a row, taking one out, and moving one.
     */
    {
        load("<body><ul id=list><li>a</li></ul>"
             "<script>"
             "var li = document.createElement('li');"
             "li.textContent = 'b';"
             "document.getElementById('list').appendChild(li);"
             "</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("an element a script made and added is in the document",
            content_of(dom_by_id(&page, "list")), "ab");
        ok("and the page has to be laid out again for it", changed == 1);
    }

    {
        load("<body><ul id=list><li id=one>a</li><li>b</li></ul>"
             "<script>document.getElementById('list').removeChild("
             "document.getElementById('one'));</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("one taken out is gone",
            content_of(dom_by_id(&page, "list")), "b");
    }

    {
        load("<body><ul id=list><li id=two>b</li></ul>"
             "<script>var l = document.getElementById('list');"
             "var li = document.createElement('li');"
             "li.textContent = 'a';"
             "l.insertBefore(li, document.getElementById('two'));"
             "</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("and one put before another is before it, not after",
            content_of(dom_by_id(&page, "list")), "ab");
    }

    /* Appending something that is already somewhere moves it. A browser that
       copied instead would leave the page with two of everything a script
       ever reordered. */
    {
        load("<body><div id=a><span id=s>x</span></div><div id=b>-</div>"
             "<script>document.getElementById('b').appendChild("
             "document.getElementById('s'));</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("a node appended elsewhere leaves where it was",
            content_of(dom_by_id(&page, "a")), "");
        oks("and is where it was put",
            content_of(dom_by_id(&page, "b")), "-x");
    }

    /* The one that is not a wrong answer but a machine that stops: a ring is
       not a tree, and everything that walks one walks it forever. */
    {
        load("<body><div id=outer><div id=inner>x</div></div>"
             "<script>document.getElementById('inner').appendChild("
             "document.getElementById('outer'));</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("an element put inside its own child is refused",
            content_of(dom_by_id(&page, "outer")), "x");
    }

    {
        load("<body><div id=d class='one two'>x</div>"
             "<script>var d = document.getElementById('d');"
             "d.classList.add('three');"
             "d.classList.remove('one');"
             "document.title = d.className + '|' + d.classList.contains('two')"
             " + '|' + d.classList.toggle('two');</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("classList adds, removes, asks and toggles",
            page.title >= 0 ? page.arena + page.title : "",
            "two three|true|false");
    }

    {
        load("<body><div id=d><span>a</span><span id=s>b</span></div>"
             "<script>var s = document.getElementById('s');"
             "document.title = s.parentNode.id + ' ' +"
             " document.getElementById('d').children.length;</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("a script can walk from a node to the one that holds it",
            page.title >= 0 ? page.arena + page.title : "", "d 2");
    }

    /* --- and later -----------------------------------------------------------
     *
     * Real time, against the machine's own clock, because a timer that fires
     * when it is asked to rather than when it is due would pass any test
     * written with a fake one.
     */
    {
        load("<body><p id=out>waiting</p>"
             "<script>setTimeout(function(){"
             " document.getElementById('out').textContent = 'later'; },"
             " 300);</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("a timer has not run merely for being set",
            content_of(dom_by_id(&page, "out")), "waiting");
        ok("and does not run before it is due", jsdom_timers() == 0);

        ok("and runs when it is", pump_until(1, 3000) == 1);
        oks("and did the thing it was for",
            content_of(dom_by_id(&page, "out")), "later");
        ok("and does not run again after that", jsdom_timers() == 0);
    }

    {
        load("<body><p id=out>0</p>"
             "<script>var k = 0;"
             "setInterval(function(){ k = k + 1;"
             " document.getElementById('out').textContent = '' + k; }, 10);"
             "</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        int k = pump_until(2, 400);
        ok("a repeating timer comes round more than once", k >= 2);
        oks("a repeating timer runs again each time it comes round",
            content_of(dom_by_id(&page, "out")), "2");
    }

    {
        load("<body><p id=out>waiting</p>"
             "<script>var h = setTimeout(function(){"
             " document.getElementById('out').textContent = 'ran'; }, 20);"
             "clearTimeout(h);</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        ok("a timer the page cancelled does not run",
           pump_until(1, 200) == 0);
        oks("and nothing happened", content_of(dom_by_id(&page, "out")),
            "waiting");
    }

    /* A promise's reaction, and an async function's next step, run once the
       timer or the handler that queued them has returned: the checkpoint a
       browser makes after every callback. The engine makes it itself when
       the outermost call the host made returns (jsrun.h, js_leave), so
       nothing here had to change for it. */
    {
        load("<body><p id=out>waiting</p><p id=b>button</p>"
             "<script>setTimeout(function(){ Promise.resolve().then(function(){"
             " document.getElementById('out').textContent = 'job'; }); }, 10);"
             "document.getElementById('b').addEventListener('click', async function(){"
             " await null; document.getElementById('b').textContent = 'awaited'; });"
             "</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        pump_until(1, 3000);
        oks("a promise a timer queued runs when the timer returns",
            content_of(dom_by_id(&page, "out")), "job");
        jsdom_click(dom_by_id(&page, "b"));
        oks("an async click handler carries on past its await once it has returned",
            content_of(dom_by_id(&page, "b")), "awaited");
    }

    /* What is in a field, as a script sees it. The same attribute the
       browser types into and the layout draws, so all three agree. */
    {
        load("<body><input id=f value='before'><input id=c type=checkbox>"
             "<script>"
             "var f = document.getElementById('f');"
             "document.title = f.value;"
             "f.value = 'after';"
             "document.getElementById('c').checked = true;"
             "</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("a script reads what is in a field",
            page.title >= 0 ? page.arena + page.title : "", "before");
        oks("and writing it puts it there",
            dom_attr(&page, dom_by_id(&page, "f"), "value"), "after");
        oks("and a box it ticks is ticked",
            dom_attr(&page, dom_by_id(&page, "c"), "checked"), "1");
    }

    /* --- asking for elements the way a sheet asks -----------------------------
     *
     * The same parser and the same matcher the style sheets use, so that a
     * page cannot style one element and script another.
     */
    {
        load("<body><div class='row'><a id=one class='current'>a</a>"
             "<a id=two>b</a></div>"
             "<div class='row'><a id=three>c</a></div>"
             "<script>"
             "document.title = document.querySelector('a.current').id"
             " + ' ' + document.querySelectorAll('.row a').length"
             " + ' ' + document.querySelectorAll('div.row').length"
             " + ' ' + (document.querySelector('nope') === null);"
             "</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("a selector picks the element a sheet would have styled",
            page.title >= 0 ? page.arena + page.title : "", "one 3 2 true");
    }

    {
        load("<body><div id=a><span>one</span></div>"
             "<div id=b><span id=wanted>two</span></div>"
             "<script>document.title ="
             " document.getElementById('b').querySelector('span').id;"
             "</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("and an element searches what is under it, not the document",
            page.title >= 0 ? page.arena + page.title : "", "wanted");
    }

    {
        load("<body><p id=p>x</p>"
             "<script>document.title ="
             " document.querySelector('span, p').id;</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("a list of selectors matches any one of them",
            page.title >= 0 ? page.arena + page.title : "", "p");
    }

    /* The selector is parsed on to the end of the browser's own style sheet
       and rolled back off it. If it were not, a page that asks in a loop
       would fill the sheet up and the answers would stop coming -- and the
       page would still be asking the same question. */
    {
        load("<body><b id=t>x</b>"
             "<script>var last = '';"
             "for (var i = 0; i < 400; i++) last ="
             " document.querySelector('b#t').id;"
             "document.title = last;</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("and asking four hundred times does not use the sheet up",
            page.title >= 0 ? page.arena + page.title : "", "t");
    }

    /* --- handlers set as properties, the window, and what is not script ------
     *
     * el.onclick = f and window.onload = f were stored and never called; the
     * window had no addEventListener, so the line that asked for one threw and
     * the rest of its script never ran; `return false` did not refuse the
     * default; and a script element holding data was run as a script. */
    {
        load("<body><p id=b>x</p><p id=out>no</p>"
             "<script>document.getElementById('b').onclick = function(){"
             " document.getElementById('out').textContent = 'yes ' + this.id; };</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        jsdom_click(dom_by_id(&page, "b"));
        oks("a handler set as a property runs, with the element as this",
            content_of(dom_by_id(&page, "out")), "yes b");
    }
    {
        load("<body><p id=out>none</p><p id=two>none</p><p id=three>none</p>"
             "<script>window.addEventListener('load', function(){"
             " document.getElementById('out').textContent = 'loaded'; });"
             "addEventListener('DOMContentLoaded', function(){"
             " document.getElementById('two').textContent = 'ready'; });"
             "window.onload = function(){ document.getElementById('three').textContent = 'onload'; };"
             "</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        oks("the window takes a listener for load", content_of(dom_by_id(&page, "out")), "loaded");
        oks("and so does a bare addEventListener, which is the window's",
            content_of(dom_by_id(&page, "two")), "ready");
        oks("and window.onload runs", content_of(dom_by_id(&page, "three")), "onload");
        ok("and nothing threw on the way", err[0] == 0);
    }
    {
        load("<body onload=\"document.getElementById('out').textContent = 'body'\"><p id=out>no</p></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        oks("the body's onload runs when the page has loaded", content_of(dom_by_id(&page, "out")), "body");
    }
    {
        load("<body><a id=a href='/next' onclick='return false'>go</a>"
             "<a id=c href='/next'>go</a>"
             "<script>document.getElementById('c').onclick = function(){ return false; };</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        ok("returning false from an onclick attribute refuses the default",
           jsdom_click(dom_by_id(&page, "a")) == 1);
        ok("and from a handler set as a property", jsdom_click(dom_by_id(&page, "c")) == 1);
    }
    {
        load("<body><p id=out>no</p><p id=who>?</p>"
             "<script type=\"application/ld+json\">{\"@context\": \"https://schema.org\"}</script>"
             "<script type=\"text/javascript\">document.getElementById('out').textContent = 'ran';"
             "document.addEventListener('click', function(){"
             " document.getElementById('who').textContent = String(this === document); });</script></body>");
        char err[128];
        int ran = run_scripts(&page, err, (int)sizeof(err), 0);
        ok("data in a script element is not run as script", ran == 1 && err[0] == 0);
        oks("while a script that says it is JavaScript is", content_of(dom_by_id(&page, "out")), "ran");
        jsdom_click(dom_by_id(&page, "out"));
        oks("and a listener on the document has the document as this",
            content_of(dom_by_id(&page, "who")), "true");
    }

    /* --- the standard's objects -------------------------------------------------
     *
     * An element is an HTMLDivElement, an HTMLElement, an Element, a Node
     * and an EventTarget, and the document is a Document: that is how
     * libraries tell a node from anything else, and jQuery stopped on
     * document.nodeType, GOV.UK on HTMLScriptElement, Reddit on HTMLElement. */
    oks("an element is an instance of every interface above it",
        titled("<body><div id=d></div><script>var d = document.getElementById('d');"
               "document.title = [d instanceof HTMLDivElement, d instanceof HTMLElement,"
               " d instanceof Element, d instanceof Node, d instanceof EventTarget,"
               " document instanceof Document, document.nodeType, d.nodeType,"
               " document.body.firstChild.nodeType, d.tagName, document.documentElement.nodeName,"
               " Object.prototype.toString.call(d), typeof HTMLScriptElement].join(' ');"
               "</script></body>"),
        "true true true true true true 9 1 1 DIV HTML [object HTMLDivElement] function");
    oks("ShadowRoot is there to be asked about, and nothing is one",
        titled("<body><script>document.title = [typeof ShadowRoot, document.body.parentNode instanceof ShadowRoot,"
               " document.body.shadowRoot, 'attachShadow' in Element.prototype,"
               " ShadowRoot.prototype instanceof DocumentFragment].join(' ');</script></body>"),
        "function false  false true");
    oks("and a page can add a method to Element.prototype that every element has",
        titled("<body><p id=p>x</p><script>Element.prototype.hi = function(){ return 'hi ' + this.id; };"
               "document.title = document.getElementById('p').hi();</script></body>"),
        "hi p");
    oks("the tree walks the way the standard's does, text nodes and all",
        titled("<body><p id=u><b>a</b> <i id=b>b</i></p><script>var u = document.getElementById('u');"
               "document.title = [u.childNodes.length, u.children.length, u.firstChild.nodeName,"
               " u.firstChild.nextSibling.nodeType, u.firstElementChild.nextElementSibling.id,"
               " u.lastChild.previousSibling.nodeName, document.getElementById('b').parentElement.id,"
               " document.documentElement.parentNode === document, u.childElementCount].join(' ');"
               "</script></body>"),
        "3 2 B 3 b #text u true 2");
    /* The page's own <html>, with what it wrote on it, is the document
       element: the parser keeps every page under a root of its own, which a
       script reading documentElement.lang or swapping no-js for js got
       instead, and which has neither. */
    oks("the document element is the page's own html, and nothing stands between it and the document",
        titled("<!doctype html><html lang=en class=no-js><head><title>t</title></head><body><p>x</p><script>"
               "var r = document.documentElement; r.className = r.className.replace('no-js', 'js');"
               "var up = 0; document.body.addEventListener('click', function(e){ up = e.composedPath().length; });"
               "document.body.click();"
               "document.title = [r.lang, r.className, r.firstElementChild.nodeName, document.head.parentNode === r,"
               " r.parentNode === document, r.parentElement, document.querySelectorAll('html').length,"
               " document.querySelector(':root') === r, document.getElementsByTagName('*')[0] === r,"
               " document.firstElementChild === r, document.children[0] === r, up].join(' ');</script></body></html>"),
        "en js HEAD true true  1 true true true true 4");
    oks("children and getElementsByClassName follow the document",
        titled("<body><div id=d><p class='a b'>1</p></div><script>var d = document.getElementById('d');"
               "var kids = d.children, found = document.getElementsByClassName('b a');"
               "var before = kids.length + ',' + found.length;"
               "var p = document.createElement('p'); p.className = 'b a'; d.appendChild(p);"
               "document.title = before + ' ' + kids.length + ',' + found.length + ' '"
               " + (found.item(1) === p) + ' ' + (d.children === kids);</script></body>"),
        "1,1 2,2 true true");
    oks("closest, matches and contains",
        titled("<body><nav class=menu><a id=a href=#><b id=b>x</b></a></nav><script>"
               "var b = document.getElementById('b');"
               "document.title = [b.closest('nav.menu').tagName, b.closest('a').id,"
               " b.matches('a > b'), b.matches('nav > b'), document.body.contains(b),"
               " b.contains(document.body), document.contains(b)].join(' ');</script></body>"),
        "NAV a true false true false true");
    oks("textContent written replaces what was there, and what was there is taken out",
        titled("<body><div id=d><span id=s>old</span></div><script>var d = document.getElementById('d');"
               "var s = document.getElementById('s'); d.textContent = 'new';"
               "document.title = d.textContent + ' ' + (s.parentNode === null) + ' ' + d.childNodes.length;"
               "</script></body>"),
        "new true 1");
    oks("innerHTML reads as markup and writes elements a script can find",
        titled("<body><div id=d><b class=x>a &amp; b</b><br></div><script>var d = document.getElementById('d');"
               "var before = d.innerHTML;"
               "d.innerHTML = '<p id=made title=\"q&quot;\">made <i>here</i></p><script>window.bad = 1<\\/script>';"
               "document.title = before + ' | ' + document.getElementById('made').textContent + ' | '"
               " + document.getElementById('made').title + ' | ' + d.querySelectorAll('i').length"
               " + ' ' + typeof bad;</script></body>"),
        "<b class=\"x\">a &amp; b</b><br> | made here | q\" | 1 undefined");
    oks("outerHTML and insertAdjacentHTML put markup where they say",
        titled("<body><div id=d><p id=p>p</p></div><script>var p = document.getElementById('p');"
               "p.insertAdjacentHTML('beforebegin', '<i>1</i>');"
               "p.insertAdjacentHTML('afterbegin', '<i>2</i>');"
               "p.insertAdjacentHTML('beforeend', '<i>3</i>');"
               "p.insertAdjacentHTML('afterend', '<i>4</i>');"
               "var d = document.getElementById('d'); var s = d.innerHTML;"
               "p.outerHTML = '<em>gone</em>';"
               "document.title = s + ' ' + d.textContent;</script></body>"),
        "<i>1</i><p id=\"p\"><i>2</i>p<i>3</i></p><i>4</i> 1gone4");
    oks("a copy is a copy: its attributes are its own, and deep takes the children",
        titled("<body><div id=d class=one><span>x</span></div><script>var d = document.getElementById('d');"
               "var c = d.cloneNode(true), s = d.cloneNode(false);"
               "c.className = 'two'; c.id = 'c';"
               "document.title = d.className + ' ' + c.className + ' ' + c.innerHTML + ' ['"
               " + s.innerHTML + '] ' + d.id;</script></body>"),
        "one two <span>x</span> [] d");
    oks("dataset, hidden and attributes are views of the attributes",
        titled("<body><div id=d data-user-id=7 hidden></div><script>var d = document.getElementById('d');"
               "var a = d.dataset.userId + ' ' + d.hidden; d.dataset.fooBar = 'z'; d.hidden = false;"
               "var names = []; for (var i = 0; i < d.attributes.length; i++) names.push(d.attributes[i].name);"
               "document.title = a + ' ' + d.getAttribute('data-foo-bar') + ' ' + d.hasAttribute('hidden')"
               " + ' ' + names.join(',');</script></body>"),
        "7 true z false id,data-user-id,data-foo-bar");
    oks("style is an object that writes the style attribute",
        titled("<body><div id=d style='color: red'></div><script>var d = document.getElementById('d');"
               "d.style.display = 'none'; d.style.backgroundColor = 'blue'; d.style.color = '';"
               "var r = d.style.display + ' ' + d.style.getPropertyValue('background-color');"
               "d.style.transition = 'x';"
               "document.title = r + ' | ' + d.getAttribute('style') + ' | ' + ('transition' in d.style)"
               " + ' ' + d.style.length;</script></body>"),
        "none blue | display: none; background-color: blue; | true 2");
    {
        /* And the browser is told which elements' styles to read again. */
        load("<body><div id=d></div><p id=q style='color:red'></p>"
             "<script>document.getElementById('d').style.display = 'none';</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        int first = jsdom_next_restyled();
        ok("an element whose style a script wrote is handed back to be read again, once",
           first == dom_by_id(&page, "d") && jsdom_next_restyled() < 0);
    }
    oks("an element taken out of the page is not found in it",
        titled("<body><p id=gone class=g>x</p><script>var p = document.getElementById('gone'); p.remove();"
               "document.title = [document.getElementById('gone'), document.querySelector('.g'),"
               " document.getElementsByClassName('g').length, p.isConnected].join(' ');</script></body>"),
        "  0 false");
    oks("a form's controls are properties of it by name, and a select says what is chosen",
        titled("<body><form id=f><input name=q value=zelr><select id=s><option value=a>A"
               "<option value=b selected>B</select></form><script>var f = document.getElementById('f');"
               "var s = document.getElementById('s');"
               "var r = f.q.value + ' ' + s.value + ' ' + s.selectedIndex + ' ' + f.elements.length;"
               "s.selectedIndex = 0;"
               "document.title = r + ' ' + s.value + ' ' + s.options[1].selected;</script></body>"),
        "zelr b 1 2 a false");
    {
        /* getBoundingClientRect from where the layout put the element, in the
           window's terms: the page scrolled by thirty is thirty higher. */
        jsdom_boxes_with(fake_box);
        jsdom_view(800, 600, 30);
        oks("a box is where the layout put it, less the scroll",
            titled("<body><div id=d></div><script>var r = document.getElementById('d').getBoundingClientRect();"
                   "document.title = [r.left, r.top, r.width, r.height, r.bottom,"
                   " document.documentElement.clientWidth].join(' ');</script></body>"),
            "10 70 50 20 90 800");
        /* elementFromPoint: the element of what was drawn there, the
           document element where nothing was, nothing outside the window.
           Scrolled by thirty, the window's ten is the page's forty. */
        jsdom_points_with(fake_point);
        oks("elementFromPoint is the element drawn at a point of the window, and what it is inside",
            titled("<html><body><p><b id=hit>word</b></p><script>"
                   "var a = document.elementFromPoint(10, 10), b = document.elementFromPoint(10, 90),"
                   " c = document.elementFromPoint(10, 300), d = document.elementFromPoint(-1, 10),"
                   " e = document.elementFromPoint(900, 10);"
                   "document.title = [a && a.id, b === document.documentElement, c === document.documentElement, d, e,"
                   " document.elementsFromPoint(10, 10).map(function(x){ return x.nodeName; }).join(',')].join(' ');"
                   "</script></body></html>"),
            "hit true true   B,P,BODY,HTML");
        jsdom_points_with(0);
        jsdom_boxes_with(0);
        jsdom_view(0, 0, 0);
        oks("and with nothing laid out it is nowhere, with no size",
            titled("<body><div id=d></div><script>var r = document.getElementById('d').getBoundingClientRect();"
                   "document.title = [r.left, r.top, r.width, r.height].join(' ');</script></body>"),
            "0 0 0 0");
    }

    /* --- scripts a page adds, and ones it must not run ----------------------------- */
    oks("a script inside a template does not run",
        titled("<body><template><script>document.title = 'ran';</script></template>"
               "<script>document.title = document.title || 'kept';</script></body>"),
        "kept");
    oks("a template's content is a fragment a script can stamp out",
        titled("<body><template id=t><li class=row>x</li></template><ul id=u></ul><script>"
               "var t = document.getElementById('t'), u = document.getElementById('u');"
               "u.appendChild(t.content.cloneNode(true)); u.appendChild(document.importNode(t.content, true));"
               "document.title = u.querySelectorAll('.row').length + ' ' + t.content.nodeType + ' '"
               " + document.querySelectorAll('template .row').length;</script></body>"),
        "2 11 0");
    oks("a script a page makes and appends runs when it is put in the page",
        titled("<body><script>var s = document.createElement('script');"
               "s.textContent = 'document.title = \"inserted \" + (document.currentScript === s);';"
               "document.body.appendChild(s);</script></body>"),
        "inserted true");
    {
        jsdom_fetch_with(fake_script);
        load("<body><p id=out>no</p><script>var s = document.createElement('script');"
             "s.src = 'x.js#document.getElementById(\"out\").textContent = \"fetched\";';"
             "s.onload = function(){ document.title = 'loaded'; };"
             "document.head.appendChild(s);</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        oks("and one with a src waits for the next pass rather than running inside appendChild",
            content_of(dom_by_id(&page, "out")), "no");
        pump_until(1, 2000);
        oks("then runs", content_of(dom_by_id(&page, "out")), "fetched");
        oks("and is told it loaded", page.title >= 0 ? page.arena + page.title : "", "loaded");
        jsdom_fetch_with(0);
    }
    oks("document.write while the page is read puts its markup after the script",
        titled("<body><div id=d><script>document.write('<b id=w>written</b>');</script><i>after</i></div>"
               "<script>var d = document.getElementById('d');"
               "document.title = d.children[1].id + ' ' + d.lastChild.textContent;</script></body>"),
        "w after");

    /* --- custom elements ------------------------------------------------------------ */
    oks("a custom element defined after it is in the page is upgraded and told it is connected",
        titled("<body><my-thing id=t name=a></my-thing><script>var log = [];"
               "class Thing extends HTMLElement {"
               " static get observedAttributes(){ return ['name']; }"
               " constructor(){ super(); log.push('made'); }"
               " connectedCallback(){ log.push('in'); }"
               " attributeChangedCallback(n, o, v){ log.push(n + ':' + o + '>' + v); } }"
               "customElements.define('my-thing', Thing);"
               "var t = document.getElementById('t'); t.setAttribute('name', 'b');"
               "var made = new Thing(); document.body.appendChild(made);"
               "document.title = log.join(' ') + ' ' + (t instanceof Thing) + ' ' + made.tagName"
               " + ' ' + (customElements.get('my-thing') === Thing);</script></body>"),
        "made name:null>a in name:a>b made in true MY-THING true");

    /* --- events ----------------------------------------------------------------------- */
    oks("an event a page makes goes where it is sent, bubbling when it asks to",
        titled("<body><div id=o><p id=i>x</p></div><script>var log = [];"
               "var o = document.getElementById('o'), i = document.getElementById('i');"
               "o.addEventListener('ping', function(e){ log.push('o:' + e.detail + ':' + (e.target === i) + ':' + e.eventPhase); });"
               "document.addEventListener('ping', function(e){ log.push('doc'); });"
               "var ok = i.dispatchEvent(new CustomEvent('ping', { bubbles: true, detail: 5 }));"
               "i.dispatchEvent(new Event('ping'));"
               "document.title = log.join(' ') + ' ' + ok + ' ' + (new Event('x') instanceof Event);</script></body>"),
        "o:5:true:3 doc true true");
    oks("capture listeners run on the way down, before the target's",
        titled("<body><div id=o><p id=i>x</p></div><script>var log = [];"
               "var o = document.getElementById('o'), i = document.getElementById('i');"
               "o.addEventListener('go', function(){ log.push('bubble'); });"
               "o.addEventListener('go', function(){ log.push('capture'); }, true);"
               "i.addEventListener('go', function(){ log.push('target'); });"
               "i.dispatchEvent(new Event('go', { bubbles: true }));"
               "document.title = log.join(' ');</script></body>"),
        "capture target bubble");
    oks("once, stopImmediatePropagation, preventDefault and a listener object",
        titled("<body><p id=i>x</p><script>var n = 0, log = [];"
               "var i = document.getElementById('i');"
               "i.addEventListener('t', function(){ n++; }, { once: true });"
               "i.addEventListener('t', { handleEvent: function(e){ log.push('obj'); e.preventDefault(); } });"
               "i.addEventListener('t', function(e){ e.stopImmediatePropagation(); log.push('stop'); });"
               "i.addEventListener('t', function(){ log.push('never'); });"
               "var r1 = i.dispatchEvent(new Event('t', { cancelable: true }));"
               "i.dispatchEvent(new Event('t'));"
               "document.title = n + ' ' + log.join(' ') + ' ' + r1;</script></body>"),
        "1 obj stop obj stop false");
    oks("a passive listener cannot refuse the default",
        titled("<body><p id=i>x</p><script>var i = document.getElementById('i');"
               "i.addEventListener('t', function(e){ e.preventDefault(); }, { passive: true });"
               "var e = new Event('t', { cancelable: true });"
               "document.title = i.dispatchEvent(e) + ' ' + e.defaultPrevented;</script></body>"),
        "true false");
    oks("click() runs the handlers and, for a box, ticks it",
        titled("<body><input id=c type=checkbox><script>var c = document.getElementById('c'), log = [];"
               "c.addEventListener('click', function(e){ log.push('click:' + e.isTrusted); });"
               "c.addEventListener('change', function(){ log.push('change'); });"
               "c.click();"
               "document.title = log.join(' ') + ' ' + c.checked;</script></body>"),
        "click:false change true");
    oks("the old way of making an event still works",
        titled("<body><p id=i>x</p><script>var i = document.getElementById('i'), got = '';"
               "i.addEventListener('old', function(e){ got = e.type + ' ' + e.bubbles; });"
               "var e = document.createEvent('Event'); e.initEvent('old', true, true); i.dispatchEvent(e);"
               "document.title = got;</script></body>"),
        "old true");
    {
        load("<body><form id=f action=/next><input id=q name=q></form><p id=out>no</p>"
             "<script>document.getElementById('f').addEventListener('submit', function(e){"
             " e.preventDefault(); document.getElementById('out').textContent = 'mine'; });</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        ok("a form the reader sends is the page's to cancel", jsdom_submitting(dom_by_id(&page, "f")) == 1);
        oks("and its handler ran", content_of(dom_by_id(&page, "out")), "mine");
    }

    /* --- what the browser says it is ------------------------------------------------
     *
     * Exactly what it sends, and true of this browser: the BBC, Apple and
     * Google stopped at navigator. */
    oks("navigator.userAgent is what every request says (web.h, WEB_USER_AGENT)",
        titled("<script>document.title = navigator.userAgent;</script>"), WEB_USER_AGENT);
    oks("and the rest of navigator is this browser's own",
        titled("<script>var n = navigator;"
               "document.title = [n.platform, n.language, n.languages.length, n.cookieEnabled, n.onLine,"
               " n.webdriver, n.plugins.length, n.mimeTypes.length, n.maxTouchPoints, n.pdfViewerEnabled,"
               " n.javaEnabled(), 'userAgentData' in n, 'serviceWorker' in n, 'geolocation' in n,"
               " n instanceof Navigator, Object.getOwnPropertyNames(n).length,"
               " typeof n.hardwareConcurrency == 'undefined' || n.hardwareConcurrency > 0].join(' ');</script>"),
        "zelr en-GB 1 true true false 0 0 0 false false false false false true 0 true");
    {
        jsdom_request_with(fake_request);
        asked[0] = 0;
        load("<script>document.title = navigator.sendBeacon('/log', 'left at 3');</script>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        oks("sendBeacon takes the request", page.title >= 0 ? page.arena + page.title : "", "true");
        pump_until(1, 2000);
        oks("and makes it on the next pass, as a POST of plain text",
            asked, "POST /log left at 3 text/plain;charset=UTF-8");
        jsdom_request_with(0);
    }
    {
        /* The server's cookies, one of them HttpOnly, then the page's go at
           them. */
        ck_forget_all();
        url_parse("https://shop.example/basket", &jar_at);
        ck_take_one(&jar_at, "sid=s3cret; Path=/; HttpOnly");
        ck_take_one(&jar_at, "theme=dark; Path=/");
        jsdom_cookies_with(jar_get, jar_set);
        oks("document.cookie shows the jar's cookies but not an HttpOnly one",
            titled("<script>var before = document.cookie;"
                   "document.cookie = 'seen=1; path=/';"
                   "document.cookie = 'sid=stolen';"
                   "document.cookie = 'fake=1; HttpOnly';"
                   "document.cookie = 'theme=; expires=Thu, 01 Jan 1970 00:00:00 GMT';"
                   "document.title = before + ' | ' + document.cookie;</script>"),
            "theme=dark | seen=1");
        char sent[512];
        ck_header(&jar_at, sent, (int)sizeof(sent));
        oks("and a script can neither change an HttpOnly cookie nor make one",
            sent, "sid=s3cret; seen=1");
        jsdom_cookies_with(0, 0);
        ck_forget_all();
    }

    /* --- addresses ----------------------------------------------------------------------
     *
     * URL and URLSearchParams as the URL standard reads them: X, python.org
     * and LinkedIn stopped on URL, Reddit and Ars Technica on
     * URLSearchParams. */
    oks("a URL is read into its parts, with the host in lower case and the dots gone",
        titled("<script>var u = new URL('HTTPS://User:Pw@Example.COM:443/a/./b/../c?x=1#f');"
               "document.title = [u.href, u.protocol, u.username, u.host, u.port, u.pathname, u.search,"
               " u.hash, u.origin].join(' ');</script>"),
        "https://User:Pw@example.com/a/c?x=1#f https: User example.com  /a/c ?x=1 #f https://example.com");
    oks("and one written against a base keeps as much of the base as it says",
        titled("<script>var b = 'https://h.test/a/b/c?x#f';"
               "document.title = [new URL('../d?q', b), new URL('//o.test/x', 'http://h.test/'),"
               " new URL('?y', b), new URL('#g', b), new URL('/r', b), new URL('e', b),"
               " new URL('http://h.test:8080/p').host, new URL('mailto:a@b.c').pathname,"
               " new URL('mailto:a@b.c').origin].join(' ');</script>"),
        "https://h.test/a/d?q http://o.test/x https://h.test/a/b/c?y https://h.test/a/b/c?x#g"
        " https://h.test/r https://h.test/a/b/e h.test:8080 a@b.c null");
    oks("an address that is not one is refused, as the standard refuses it",
        titled("<script>var r = 'no';"
               "try { new URL('not an address'); } catch (e) { r = e instanceof TypeError; }"
               "document.title = r + ' ' + URL.canParse('x') + ' ' + URL.canParse('x', 'https://h/');</script>"),
        "true false true");
    oks("a part set writes the whole address, escaped as that part is",
        titled("<script>var u = new URL('https://h.test/p');"
               "u.pathname = 'a b'; u.search = 'q=1 2'; u.hash = 'top'; u.port = '8080';"
               "var one = u.href; u.port = '443'; u.protocol = 'http';"
               "document.title = one + ' ' + u.href;</script>"),
        "https://h.test:8080/a%20b?q=1%202#top http://h.test/a%20b?q=1%202#top");
    oks("searchParams is the URL's query, and writing one writes the other",
        titled("<script>var u = new URL('https://h/?a=1&b=2&a=3');"
               "var p = u.searchParams, all = p.getAll('a').join(',');"
               "p.append('c', 'x y'); var s1 = u.search; p.delete('a');"
               "u.search = '?z=9'; document.title = [all, s1, p.get('z'), p.has('b'), u.searchParams === p].join(' ');"
               "</script>"),
        "1,3 ?a=1&b=2&a=3&c=x+y 9 false true");
    oks("URLSearchParams from a record, pairs and text, sorted and walked",
        titled("<script>var r = [];"
               "r.push(new URLSearchParams({ x: '1', y: 'two words' }).toString());"
               "r.push(new URLSearchParams([['a', 'b']]).get('a'));"
               "var q = new URLSearchParams('?q=%20a+b&k=&z=1&b=2'); r.push('[' + q.get('q') + ']' + q.get('k') + q.size);"
               "q.sort(); r.push([...q.keys()].join(''));"
               "var seen = ''; q.forEach(function(v, k){ seen += k + v; }); r.push(seen);"
               "document.title = r.join(' | ');</script>"),
        "x=1&y=two+words | b | [ a b]4 | bkqz | b2kq a bz1");
    {
        jsdom_at("https://site.test/dir/page?here=1");
        oks("a link's parts are its address made whole against the page's, and setting one writes href",
            titled("<body><a id=a href='/p?x=1#h'>a</a><img id=i src='pic.png'><script>"
                   "var a = document.getElementById('a');"
                   "var r = [a.href, a.pathname, a.search, a.hash, a.host, a.origin, document.getElementById('i').src];"
                   "a.search = '?y=2'; r.push(a.getAttribute('href')); r.push(document.URL);"
                   "document.title = r.join(' ');</script></body>"),
            "https://site.test/p?x=1#h /p ?x=1 #h site.test https://site.test https://site.test/dir/pic.png"
            " https://site.test/p?y=2#h https://site.test/dir/page?here=1");
        jsdom_at("");
    }

    /* --- where the page is, and going somewhere ------------------------------------
     *
     * location and history: python.org, Stack Overflow, CSS-Tricks and The
     * Verge stopped on location. */
    {
        jsdom_at("https://site.test/dir/page?x=1#top");
        jsdom_navigate_with(fake_navigate);
        jsdom_address_with(fake_address);
        jsdom_history_with(fake_history_go, fake_history_length);
        oks("location is the page's address in parts, the one object on window and document",
            titled("<script>var l = location;"
                   "document.title = [l.href, l.protocol, l.host, l.pathname, l.search, l.hash, l.origin,"
                   " String(l), document.location === l, window.location === l, l instanceof Location,"
                   " history.length, history.state].join(' ');</script>"),
            "https://site.test/dir/page?x=1#top https: site.test /dir/page ?x=1 #top https://site.test"
            " https://site.test/dir/page?x=1#top true true true 4 ");
        went[0] = 0;
        titled("<script>location.href = 'next?y=2';</script>");
        oks("setting href sends the browser there, made whole", went, "go https://site.test/dir/next?y=2");
        went[0] = 0;
        titled("<script>location.replace('/other');</script>");
        oks("replace sends it there in the page's place", went, "replace https://site.test/other");
        went[0] = 0;
        titled("<script>location = '/bare';</script>");
        oks("and so does assigning location itself", went, "go https://site.test/bare");
        went[0] = 0;
        titled("<script>location.search = 'q=zelr';</script>");
        oks("a part set goes to the address with that part changed", went, "go https://site.test/dir/page?q=zelr#top");

        went[0] = moved[0] = 0;
        load("<body><p id=out>no</p><script>"
             "window.addEventListener('hashchange', function(e){"
             " document.getElementById('out').textContent = e.oldURL.split('#')[1] + '>' + e.newURL.split('#')[1]; });"
             "location.hash = 'sec'; document.title = location.hash;</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        oks("a new fragment is the same page: nothing is fetched", went, "");
        oks("the address shown is the new one, kept in the history", moved,
            "push https://site.test/dir/page?x=1#sec");
        pump_until(1, 2000);
        oks("and hashchange is sent afterwards", content_of(dom_by_id(&page, "out")), "top>sec");

        moved[0] = went[0] = 0;
        oks("pushState changes the address and the state without loading anything",
            titled("<script>history.pushState({ n: 1 }, '', '/pushed?y');"
                   "var a = location.pathname + location.search + ' ' + history.state.n;"
                   "history.replaceState({ n: 2 }, '');"
                   "var e = 'none'; try { history.pushState(null, '', 'https://elsewhere.test/'); }"
                   " catch (x) { e = x.name; }"
                   "document.title = a + ' ' + history.state.n + ' ' + e;</script>"),
            "/pushed?y 1 2 SecurityError");
        oks("and tells the browser the address to show", moved, "replace https://site.test/pushed?y");
        oks("with nothing fetched", went, "");
        history_moved = 0;
        titled("<script>history.back();</script>");
        ok("history.back is the browser's back button", history_moved == -1);
        jsdom_navigate_with(0);
        jsdom_address_with(0);
        jsdom_history_with(0, 0);
        jsdom_at("");
    }

    /* --- the window ----------------------------------------------------------------------
     *
     * Its clock, frames, size, media queries, screen and messages: Apple
     * stopped on matchMedia. */
    oks("performance counts from the page's start by the machine's clock, and keeps marks",
        titled("<script>var t = performance.now(); performance.mark('a');"
               "performance.measure('m', 'a');"
               "document.title = [typeof t, t >= 0, performance.timeOrigin > 1.7e12,"
               " performance.getEntriesByName('a').length, performance.getEntriesByType('measure')[0].name,"
               " performance.getEntries().length].join(' ');</script>"),
        "number true true 1 m 2");
    {
        load("<body><p id=out>none</p><script>var n = 0;"
             "requestAnimationFrame(function(t){ document.getElementById('out').textContent = typeof t + ' ' + (t >= 0); });"
             "var gone = requestAnimationFrame(function(){ n++; }); cancelAnimationFrame(gone);</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        oks("an animation frame has not come merely for being asked", content_of(dom_by_id(&page, "out")), "none");
        pump_until(1, 2000);
        oks("and comes, with the time", content_of(dom_by_id(&page, "out")), "number true");
        ok("and one cancelled does not", pump_until(1, 200) == 0);
    }
    {
        jsdom_view(800, 600, 40);
        jsdom_scroll_with(fake_scroll);
        scrolled_to = -1;
        oks("matchMedia is the style sheets' reading of a query against the window's width",
            titled("<script>document.title = [matchMedia('(min-width: 600px)').matches,"
                   " matchMedia('(max-width: 500px)').matches, matchMedia('print').matches,"
                   " matchMedia('(prefers-color-scheme: dark)').matches, matchMedia('screen and (min-width:40em)').media,"
                   " innerWidth, innerHeight, devicePixelRatio, scrollY, screen.colorDepth, screen.width > 0].join(' ');"
                   "scrollTo({ top: 250 });</script>"),
            "true false false false screen and (min-width:40em) 800 600 1 40 24 true");
        ok("and scrollTo asks the browser to scroll there", scrolled_to == 250);
        load("<body><p id=out>none</p><script>var m = matchMedia('(min-width: 1000px)');"
             "m.addListener(function(e){ document.getElementById('out').textContent = 'now ' + e.matches; });"
             "</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        jsdom_view(1200, 600, 0);
        oks("a list being listened to hears when the window's width changes its answer",
            content_of(dom_by_id(&page, "out")), "now true");
        jsdom_view(0, 0, 0);
        jsdom_scroll_with(0);
    }
    oks("CSS.supports says what @supports would, and CSS.escape writes an identifier",
        titled("<script>document.title = [CSS.supports('display', 'flex'), CSS.supports('display: grid'),"
               " CSS.escape('1a b')].join(' ');</script>"),
        "true false \\31 a\\ b");
    oks("there are no dialogs and one window, and each says so the standard's way",
        titled("<script>document.title = [alert('x'), confirm('y'), prompt('z'), open('/w'),"
               " window.top === window, window.parent === window, opener].join(' ');</script>"),
        " false   true true ");
    {
        jsdom_at("https://site.test/p");
        load("<body><p id=out>none</p><p id=two>none</p><script>"
             "addEventListener('message', function(e){ document.getElementById('out').textContent = e.data + ' ' + e.origin + ' ' + (e.source === window); });"
             "postMessage('hello', '*');"
             "var ch = new MessageChannel(); ch.port1.onmessage = function(e){ document.getElementById('two').textContent = 'port ' + e.data; };"
             "ch.port2.postMessage(7);</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        oks("postMessage is delivered afterwards", content_of(dom_by_id(&page, "out")), "none");
        pump_until(2, 2000);
        oks("from this page's origin", content_of(dom_by_id(&page, "out")), "hello https://site.test true");
        oks("and a MessageChannel's port hears the other", content_of(dom_by_id(&page, "two")), "port 7");
        jsdom_at("");
    }
    {
        jsdom_styles_with(fake_style);
        oks("getComputedStyle is the style the layout worked out for the element",
            titled("<body><div id=d></div><script>var s = getComputedStyle(document.getElementById('d'));"
                   "document.title = [s.display, s.color, s.getPropertyValue('font-weight'), s.marginTop,"
                   " s.getPropertyValue('flex-wrap'), s.transition].join(' ');</script></body>"),
            "flex rgb(16, 32, 48) 700 12px nowrap ");
        jsdom_styles_with(0);
    }

    /* --- asking the network ------------------------------------------------------------
     *
     * fetch, Headers, Request, Response, AbortController and FormData, and
     * XMLHttpRequest's headers and kinds of reply, through the same door. */
    {
        jsdom_request_with(fake_request);
        jsdom_at("https://site.test/page");
        load("<body><p id=a>none</p><p id=b>none</p><p id=c>none</p><p id=d>none</p><script>"
             "var put = function(id, v){ document.getElementById(id).textContent = v; };"
             "fetch('/api/json?x=1').then(function(r){ put('a', [r.status, r.ok, r.headers.get('Content-Type'), r.url].join(' '));"
             " return r.json(); }).then(function(j){ put('b', j.n + ' ' + j.list.length); });"
             "fetch('/down').catch(function(e){ put('c', e.name); });"
             "var ac = new AbortController(); fetch('/slow', { signal: ac.signal })"
             ".catch(function(e){ put('d', e.name + ' ' + ac.signal.aborted); }); ac.abort();"
             "document.title = 'sent';</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        oks("fetch hands back a promise and asks nothing until the next pass",
            content_of(dom_by_id(&page, "a")), "none");
        oks("an abort before it is made refuses it at once, as AbortError",
            content_of(dom_by_id(&page, "d")), "AbortError true");
        for (int k = 0; k < 4; k++) jsdom_requests();
        oks("then the reply comes as a Response", content_of(dom_by_id(&page, "a")),
            "200 true application/json https://site.test/api/json?x=1");
        oks("whose body reads as JSON", content_of(dom_by_id(&page, "b")), "7 2");
        oks("and a request that got no answer is a failed fetch, a TypeError",
            content_of(dom_by_id(&page, "c")), "TypeError");

        asked[0] = 0;
        load("<body><script>"
             "var fd = new FormData(); fd.append('q', 'a b'); fd.append('n', 1);"
             "fetch('/post', { method: 'POST', body: fd });"
             "</script></body>");
        run_scripts(&page, err, (int)sizeof(err), 0);
        jsdom_requests();
        oks("a POST's body and its type go out with it", asked,
            "POST https://site.test/post q=a+b&n=1 application/x-www-form-urlencoded;charset=UTF-8");
        asked[0] = 0;
        oks("a method this browser cannot send is refused, not sent as another",
            titled("<script>fetch('/x', { method: 'PUT' }).catch(function(e){ document.title = e.name + ': ' + e.message; });"
                   "</script>"),
            "TypeError: this browser sends only GET and POST");
        oks("and nothing was sent", asked, "");

        oks("Headers keep names in lower case and join a name given twice",
            titled("<script>var h = new Headers({ 'X-One': 'a' }); h.append('x-one', 'b'); h.set('Accept', 'text/html');"
                   "var keys = []; for (var e of h) keys.push(e[0] + '=' + e[1]);"
                   "var r = new Request('/r', { method: 'post', headers: h });"
                   "var s = new Response('hi', { status: 201 });"
                   "document.title = [h.get('X-ONE'), h.has('accept'), keys.join(','), r.method, r.url,"
                   " r.headers.get('accept'), s.status, s.ok].join(' ');</script>"),
            "a, b true accept=text/html,x-one=a, b POST https://site.test/r text/html 201 true");

        load("<body><p id=x>none</p><script>"
             "var q = new XMLHttpRequest(); q.open('GET', '/api/json'); q.responseType = 'json';"
             "q.setRequestHeader('X-Test', '1');"
             "q.onload = function(){ document.getElementById('x').textContent ="
             " [q.status, q.readyState, q.response.n, q.getResponseHeader('content-type'), q.responseURL].join(' '); };"
             "q.send();</script></body>");
        run_scripts(&page, err, (int)sizeof(err), 0);
        jsdom_requests();
        oks("XMLHttpRequest gives its reply as the kind asked for, with its type and address",
            content_of(dom_by_id(&page, "x")), "200 4 7 application/json https://site.test/api/json");

        oks("an AbortSignal of its own says it was aborted, and why",
            titled("<script>var s = AbortSignal.abort(); var heard = 0;"
                   "var ac = new AbortController(); ac.signal.addEventListener('abort', function(){ heard++; });"
                   "ac.abort('stop'); ac.abort('again');"
                   "document.title = [s.aborted, s.reason.name, ac.signal.reason, heard].join(' ');</script>"),
            "true AbortError stop 1");
        jsdom_request_with(0);
        jsdom_at("");
    }

    /* --- a script the machine has no room for -------------------------------------
     *
     * Running one costs many times its size, and a machine without the
     * memory ended the browser; now it is not run, and the page says so. */
    {
        jsdom_memory_with(little_memory);
        char err[128];
        load("<body><p id=out>no</p><script>document.getElementById('out').textContent = 'ran';</script></body>");
        run_scripts(&page, err, (int)sizeof(err), 0);
        oks("a script is not run on a machine without the memory for it", content_of(dom_by_id(&page, "out")), "no");
        oks("and the page says why", err, "a script too large for this machine's memory was not run");
        jsdom_memory_with(much_memory);
        oks("while one with the memory runs it",
            titled("<script>document.title = 'ran';</script>"), "ran");
        jsdom_memory_with(0);
    }

    /* --- comments -------------------------------------------------------------------
     *
     * A comment in the page was thrown away as it was read, so a script that
     * found its place by one found nothing: React's streamed pages move each
     * late part in beside the comment in front of its placeholder, and Yahoo's
     * stopped at "cannot set data of null". What follows does what those do,
     * written here. */
    {
        oks("a comment in the page is a node a script can find",
            titled("<body><p id=a>x</p><!--hello--><p id=b>y</p><script>"
                   "var c = document.getElementById('b').previousSibling;"
                   "document.title = [c.nodeType, c.nodeName, c.data].join(' ');</script></body>"),
            "8 #comment hello");
        oks("and its words can be changed, and it goes into markup and comes back out",
            titled("<body><div id=d>a<!--x-->b</div><script>var d = document.getElementById('d');"
                   "d.childNodes[1].data = 'y'; var before = d.innerHTML; d.innerHTML = 'p<!--q-->r';"
                   "document.title = before + ' ' + d.childNodes.length + ' ' + d.innerHTML;</script></body>"),
            "a<!--y-->b 3 p<!--q-->r");
        oks("a late part moved in where its comment says, as a streamed page does",
            titled("<body><div id=w><!--$?--><template id=B0></template><p>loading</p><!--/$--></div>"
                   "<div hidden id=S0><p>arrived</p></div><script>"
                   "var t = document.getElementById('B0'), s = document.getElementById('S0');"
                   "var mark = t.previousSibling; mark.data = '$';"
                   "var n = t, up = n.parentNode;"
                   "while (n && !(n.nodeType == 8 && n.data == '/$')) { var nx = n.nextSibling; up.removeChild(n); n = nx; }"
                   "while (s.firstChild) up.insertBefore(s.firstChild, n);"
                   "s.parentNode.removeChild(s);"
                   "document.title = document.getElementById('w').textContent;</script></body>"),
            "arrived");

        /* A template's children are its content from the start: until its
           content was first read they were children, and a walk of the page
           went into them. */
        oks("a template has no children, only content, from the start",
            titled("<body><template id=t><p id=in>x</p></template><script>var t = document.getElementById('t');"
                   "document.title = [t.firstElementChild, t.childNodes.length, t.content.firstElementChild.id,"
                   " document.getElementById('in'), t.innerHTML].join(' ');</script></body>"),
            " 0 in  <p id=\"in\">x</p>");

        /* Markup set into a script is its code, as the standard reads it
           there; Next.js sets the scripts it makes this way, and a `<` in the
           code began a tag. In a textarea only the entities are read. */
        oks("markup set into a script is its code, and it runs whole",
            titled("<head></head><body><script>var s = document.createElement('script');"
                   "s.innerHTML = 'document.title = (1 < 2) + \" \" + \"a<b>c\".length;';"
                   "document.head.appendChild(s);</script></body>"),
            "true 5");
        oks("and into a style sheet or a textarea it is text",
            titled("<body><style id=st></style><textarea id=ta></textarea><script>"
                   "var st = document.getElementById('st'), ta = document.getElementById('ta');"
                   "st.innerHTML = 'p > b { color: red }'; ta.innerHTML = 'a &amp; <b>';"
                   "document.title = st.textContent + ' | ' + ta.value + ' ' + ta.childNodes.length;</script></body>"),
            "p > b { color: red } | a & <b> 1");

        /* The first DOM_COMMENT_MAX bytes of a long one, and none at all once
           the document is nearly full: the nodes left go to what is drawn. */
        static char big[6000];
        int n = 0;
        for (const char *h = "<body><div id=d><!--"; *h; h++) big[n++] = *h;
        for (int i = 0; i < 5000; i++) big[n++] = 'c';
        for (const char *h = "--></div></body>"; *h; h++) big[n++] = *h;
        big[n] = 0;
        load(big);
        int d0 = dom_by_id(&page, "d"), c0 = d0 >= 0 ? page.nodes[d0].first : -1, kept = 0;
        if (c0 >= 0 && page.nodes[c0].kind == DN_COMMENT)
            while (page.arena[page.nodes[c0].text + kept]) kept++;
        ok("a long comment keeps its first four kilobytes", kept == DOM_COMMENT_MAX);

        static char many[DOM_NODES * 8];
        n = 0;
        for (int i = 0; i < DOM_NODES - 900; i++)
            for (const char *h = "<i></i>"; *h; h++) many[n++] = *h;
        for (const char *h = "<!--late--><p id=end></p>"; *h; h++) many[n++] = *h;
        many[n] = 0;
        load(many);
        int comments = 0;
        for (int i = 0; i < page.count; i++) if (page.nodes[i].kind == DN_COMMENT) comments++;
        ok("and none once the document is nearly full, and what follows is still read",
           comments == 0 && dom_by_id(&page, "end") >= 0);
    }

    /* --- modules ----------------------------------------------------------------------
     *
     * GitHub, MDN, Reddit, Cloudflare and BBC Sport ran none of their scripts:
     * they are modules, and there were none. */
    {
        jsdom_fetch_with(fake_modules);
        jsdom_at("https://site.test/page");
        oks("a module imports a default, names, renamed names and the namespace",
            titled("<body><script type=module>import d, { a, b as c } from './m.js'; import * as ns from './m.js';"
                   "document.title = [d, a, c, ns.a, Object.keys(ns).sort().join('')].join(' ');</script></body>"),
            "D 1 2 1 abdefault");
        oks("a binding is live: what the exporter changes, the importer sees",
            titled("<body><script type=module>import { n, bump } from './live.js'; var before = n; bump();"
                   "document.title = before + ' ' + n;</script></body>"),
            "1 2");
        oks("names are passed on with export * and export from",
            titled("<body><script type=module>import { a, z, b } from './re.js';"
                   "document.title = [a, z, b].join(' ');</script></body>"),
            "1 1 2");
        oks("import.meta knows the module's address and resolves against it",
            titled("<body><script type=module>import { url, next } from './meta.js';"
                   "document.title = url + ' ' + next + ' ' + import.meta.url;</script></body>"),
            "https://site.test/meta.js https://site.test/m.js https://site.test/page");
        oks("import() gives a promise of the namespace",
            titled("<body><script type=module>import('./m.js').then(function (ns) { document.title = ns.a + ns.default; });</script></body>"),
            "1D");
        oks("a module runs after what it imports and after the classic scripts, and nomodule does not run",
            titled("<body><script>document.title = 'classic';</script><script nomodule>document.title += ' old';</script>"
                   "<script type=module>import './order.js'; document.title = document.title + ' ' + window.order;</script></body>"),
            "classic 21");
        oks("an address of any length comes back whole from src and href",
            titled("<body><script>var s = document.createElement('script'); s.src = 'data:text/javascript,' + 'x'.repeat(5000);"
                   "var a = document.createElement('a'); a.href = '/?' + 'q'.repeat(3000);"
                   "document.title = [s.src.length, s.src.slice(-3), a.href.length, a.href.slice(0, 20)].join(' ');</script></body>"),
            "5021 xxx 3019 https://site.test/?q");
        oks("and HTMLScriptElement.supports says so",
            titled("<body><script>document.title = ['classic', 'module', 'importmap', 'speculationrules']"
                   ".map(function (k) { return HTMLScriptElement.supports(k); }).join(' ');</script></body>"),
            "true true true false");
        oks("a bare name is found through the import map, and this is undefined at the top",
            titled("<head><script type=importmap>{\"imports\": {\"lib\": \"./m.js\", \"pre/\": \"./dir/\"}}</script></head>"
                   "<body><script type=module>import { a } from 'lib'; import x from 'pre/x.js';"
                   "document.title = a + x + ' ' + (typeof this);</script></body>"),
            "1X undefined");
        oks("a module may await at its top level",
            titled("<body><script type=module>document.title = 'a'; await Promise.resolve(); document.title += 'b';</script></body>"),
            "ab");
        oks("and one that imports a module that awaits runs when that module has finished",
            titled("<body><script type=module>import { v } from './tla.js'; import { seen } from './tlauser.js';"
                   "document.title = v + ' ' + seen;</script></body>"),
            "2 2");
        oks("and import() of one settles when it has finished",
            titled("<body><script>import('./tla2.js').then(function (ns) { document.title = ns.v; });</script></body>"),
            "late");
        oks("a module may come from a data: address, escaped or in base64, and from a blob:",
            titled("<body><script type=module>import { q } from 'data:text/javascript,export%20const%20q%20=%207;';"
                   "import b from 'data:text/javascript;base64,ZXhwb3J0IGRlZmF1bHQgJ0I2NCc7';"
                   "var u = URL.createObjectURL(new Blob(['export const w = 9;'], { type: 'text/javascript' }));"
                   "import(u).then(function (ns) { document.title = [q, b, ns.w].join(' '); });</script></body>"),
            "7 B64 9");
        /* Longer than any address buffer: a data: address is never copied. */
        static char longsrc[2600];
        {
            int w = 0;
            const char *head = "<body><script type=module src=\"data:text/javascript,/*";
            for (const char *q = head; *q; q++) longsrc[w++] = *q;
            for (int k = 0; k < 1500; k++) longsrc[w++] = 'x';
            const char *tail = "*/document.title='from data'\"></script></body>";
            for (const char *q = tail; *q; q++) longsrc[w++] = *q;
            longsrc[w] = 0;
        }
        oks("and a module script's src may be one, however long", titled(longsrc), "from data");
        const char *late = titled("<body><script type=module>import './tlabad.js'; document.title = 'ran';</script></body>");
        ok("and one that throws after it awaited says what, and what imports it does not run",
           strncmp(late, "Error: late", 11) == 0);
        /* A timer is what makes them wait: the jobs an await leaves are run
           before the script's run is over. */
        char merr[128];
        load("<body><script type=module>import './tla3user.js'; document.title = 'after ' + window.seen3;</script></body>");
        run_scripts(&page, merr, (int)sizeof(merr), 0);
        int early = page.title < 0;
        pump_until(1, 2000);
        ok("one that awaits a timer holds back what imports it until the timer has run",
           early && page.title >= 0 && w_same(page.arena + page.title, "after late"));
        load("<body><script>import('./tla3.js').then(function (ns) { document.title = 'got ' + ns.v; });</script></body>");
        run_scripts(&page, merr, (int)sizeof(merr), 0);
        early = page.title < 0;
        pump_until(1, 2000);
        ok("and import() of it settles then", early && page.title >= 0 && w_same(page.arena + page.title, "got late"));
        load("<body><script type=module>import './tlabad3.js'; document.title = 'ran';</script></body>");
        run_scripts(&page, merr, (int)sizeof(merr), 0);
        pump_until(1, 2000);
        ok("and one that throws after the timer says what, and what imports it does not run",
           page.title < 0 && strncmp(jd_err, "Error: later", 12) == 0);
        load("<body><script type=module>import { a } from './missing.js'; document.title = 'ran';</script></body>");
        run_scripts(&page, merr, (int)sizeof(merr), 0);
        ok("and one whose import would not come does not run, and says why",
           (page.title < 0 || !w_same(page.arena + page.title, "ran"))
           && strncmp(merr, "a module would not come", 23) == 0);
        load("<body><script type=module>import './throws.js'; document.title = 'ran';</script></body>");
        run_scripts(&page, merr, (int)sizeof(merr), 0);
        ok("and one whose import threw as it ran does not run, and says what",
           (page.title < 0 || !w_same(page.arena + page.title, "ran")) && strncmp(merr, "Error: boom", 11) == 0);
        load("<body><script type=module>import { a } from 'nowhere';</script></body>");
        run_scripts(&page, merr, (int)sizeof(merr), 0);
        ok("and a bare name no import map gives is said to be one",
           strncmp(merr, "a module name this cannot resolve: nowhere", 42) == 0);
        jsdom_at("");
        jsdom_fetch_with(fake_script);
    }

    /* --- what a page's scripts may have ---------------------------------------------
     *
     * 24 megabytes whatever the machine had, with nothing given back until the
     * page is left, which is where big sites' scripts stopped. A quarter of
     * what is free now, between 24 and 128. */
    {
        static const char *big =
            "<script>var s = []; for (var i = 0; i < 160; i++) s.push('ab'.repeat(50000) + i);"
            "document.title = 'made ' + s.length;</script>";
        jsdom_memory_with(much_memory);
        oks("a page's scripts may have more than 24 megabytes on a machine with the memory",
            titled(big), "made 160");
        ok("a quarter of what is free, up to 128", jd_J.mem_cap == 128u * 1024 * 1024);
        jsdom_memory_with(some_memory);
        titled("<script>document.title = 'x';</script>");
        ok("and a quarter it is, below that", jd_J.mem_cap == 50u * 1024 * 1024);
        /* In the browser's order: it says what is free after the page has
           opened, and the first page kept 24 megabytes. */
        jsdom_memory_with(0);
        load("<body><script>document.title = 'x';</script></body>");
        if (jsdom_open(&page, &qsheet)) {
            jsdom_memory_with(much_memory);
            char e2[64];
            jsdom_scripts(e2, (int)sizeof(e2));
            jsdom_loaded();
        }
        ok("and it is what is free when the scripts start, told after the page opened",
           jd_J.mem_cap == 128u * 1024 * 1024);
        jsdom_memory_with(a_little_more);
        static const char too_much[] = "this script asked for more memory than a page is allowed";
        const char *stopped = titled(big);
        ok("but no more than 24 on a machine with little to spare",
           strncmp(stopped, too_much, (int)sizeof too_much - 1) == 0);
        jsdom_memory_with(0);
    }

    /* --- storage --------------------------------------------------------------------
     *
     * Strings by name for the page's origin, kept in the browser's memory
     * across pages for as long as it runs. MDN stopped on localStorage. */
    {
        jsdom_at("https://site.test/one");
        oks("localStorage keeps strings by name, and they are strings",
            titled("<script>localStorage.clear(); localStorage.setItem('n', 5); localStorage.theme = 'dark';"
                   "sessionStorage.setItem('n', 'other');"
                   "document.title = [typeof localStorage.getItem('n'), localStorage.getItem('n'), localStorage.theme,"
                   " localStorage.length, localStorage.key(1), localStorage.getItem('none'), sessionStorage.getItem('n'),"
                   " typeof localStorage.getItem, localStorage instanceof Storage].join(' ');</script>"),
            "string 5 dark 2 theme  other function true");
        /* A page later, the world long gone: what it kept is still there. */
        jsdom_at("https://site.test/two");
        oks("and what a page kept is there for the next page of the same site",
            titled("<script>document.title = localStorage.getItem('theme') + ' ' + localStorage.length;</script>"),
            "dark 2");
        jsdom_at("https://elsewhere.test/");
        oks("but not for another site's",
            titled("<script>document.title = localStorage.getItem('theme') + ' ' + localStorage.length;</script>"),
            "null 0");
        oks("and a site may keep a megabyte and is told when it tries to keep more",
            titled("<script>var big = new Array(300001).join('x'), r = 'kept';"
                   "try { for (var i = 0; i < 5; i++) localStorage.setItem('k' + i, big); }"
                   " catch (e) { r = e.name + ' at ' + i; }"
                   "localStorage.clear(); document.title = r + ' ' + localStorage.length;</script>"),
            "QuotaExceededError at 3 0");
        jsdom_at("https://site.test/three");
        titled("<script>localStorage.removeItem('n'); localStorage.clear();</script>");
        jsdom_at("");
    }

    /* --- watching the page --------------------------------------------------------
     *
     * MutationObserver hears of each change once the script that made it has
     * finished; IntersectionObserver reports what the layout drew as in view,
     * once, on the next pass; ResizeObserver reports sizes, and again when a
     * layout changes one. */
    oks("a MutationObserver hears of each change once the script has finished, with the old values",
        titled("<body><div id=d class=a><span id=s>t</span></div><script>"
               "var log = [], d = document.getElementById('d');"
               "var mo = new MutationObserver(function(recs, o){"
               " recs.forEach(function(r){ log.push([r.type, r.target.id || r.target.nodeName,"
               "  r.addedNodes.length, r.removedNodes.length, r.attributeName, r.oldValue].join(':')); });"
               " document.title = log.join(' ') + ' ' + (o === mo) + ' ' + early; });"
               "mo.observe(d, { childList: true, attributes: true, attributeOldValue: true, subtree: true,"
               " characterDataOldValue: true });"
               "d.className = 'b';"
               "d.appendChild(document.createElement('p'));"
               "document.getElementById('s').firstChild.data = 'u';"
               "d.removeChild(document.getElementById('s'));"
               "var early = log.length;</script></body>"),
        "attributes:d:0:0:class:a childList:d:1:0:: characterData:#text:0:0::t childList:d:0:1:: true 0");
    oks("and only what it asked for: a filter, records taken, and nothing after disconnect",
        titled("<body><div id=d></div><script>"
               "var d = document.getElementById('d'), heard = 0;"
               "var mo = new MutationObserver(function(){ heard++; });"
               "mo.observe(d, { attributeFilter: ['title'] });"
               "d.setAttribute('class', 'x'); d.setAttribute('title', 'y'); d.appendChild(document.createElement('i'));"
               "var taken = mo.takeRecords();"
               "d.setAttribute('title', 'z'); mo.disconnect(); d.setAttribute('title', 'w');"
               "var bad = 'none'; try { mo.observe(d, {}); } catch (e) { bad = e.name; }"
               "</script><script>"
               "document.title = [taken.length, taken[0].attributeName, String(taken[0].oldValue), heard, bad].join(' ');"
               "</script></body>"),
        "1 title null 0 TypeError");
    {
        jsdom_boxes_with(fake_box);
        jsdom_view(800, 600, 0);
        load("<body><div id=a></div><div id=b></div><script>"
             "var seen = [];"
             "var io = new IntersectionObserver(function(es){"
             " es.forEach(function(e){ seen.push(e.target.id + ' ' + e.isIntersecting + ' ' + e.intersectionRatio"
             "  + ' ' + e.boundingClientRect.top + ' ' + e.boundingClientRect.width); });"
             " document.title = seen.join(', '); });"
             "io.observe(document.getElementById('a')); io.observe(document.getElementById('b'));"
             "document.title = 'waiting ' + seen.length;</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        oks("an IntersectionObserver says nothing while the script runs",
            page.title >= 0 ? page.arena + page.title : "", "waiting 0");
        pump_until(1, 1000);
        oks("and on the next pass reports what the layout drew as in view, where it drew it",
            page.title >= 0 ? page.arena + page.title : "", "a true 1 100 50, b true 1 100 50");
        pump_until(1, 300);
        oks("once", page.title >= 0 ? page.arena + page.title : "", "a true 1 100 50, b true 1 100 50");

        /* Inside its padding and borders: 50 by 20 less 4 and 1 each side
           across, 2 each side down. */
        jsdom_styles_with(padded_style);
        load("<body><div id=a></div><script>"
             "var n = 0;"
             "new ResizeObserver(function(es){ n++; var e = es[0];"
             " document.title = [n, e.contentRect.left, e.contentRect.width, e.contentRect.height,"
             "  e.borderBoxSize[0].inlineSize].join(' ');"
             "}).observe(document.getElementById('a'));</script></body>");
        run_scripts(&page, err, (int)sizeof(err), 0);
        pump_until(1, 1000);
        oks("a ResizeObserver reports the size the layout gave, inside the padding, on the next pass",
            page.title >= 0 ? page.arena + page.title : "", "1 4 40 16 50");
        jsdom_laid_out();
        pump_until(1, 1000);
        oks("and not again while it stays that size", page.title >= 0 ? page.arena + page.title : "", "1 4 40 16 50");
        box_w = 70;
        jsdom_laid_out();
        oks("but again when a layout changes it, on the pass after the layout",
            page.title >= 0 ? page.arena + page.title : "", "1 4 40 16 50");
        pump_until(1, 1000);
        box_w = 50;
        oks("which says the new size", page.title >= 0 ? page.arena + page.title : "", "2 4 60 16 70");
        jsdom_styles_with(0);
        jsdom_boxes_with(0);
        jsdom_view(0, 0, 0);
    }

    /* --- crypto ----------------------------------------------------------------------
     *
     * From the kernel's generator (SYS_RANDOM), and nothing at all without
     * it. The Verge stopped on crypto.randomUUID. */
    {
        jsdom_at("https://site.test/");
        oks("crypto.getRandomValues fills the array it is given, and never the same twice",
            titled("<script>var a = new Uint8Array(64), b = new Uint32Array(16);"
                   "var r = crypto.getRandomValues(a); crypto.getRandomValues(b);"
                   "var zero = true, same = true, b8 = new Uint8Array(b.buffer);"
                   "for (var i = 0; i < 64; i++) { if (a[i]) zero = false; if (b8[i] !== a[i]) same = false; }"
                   "document.title = [r === a, zero, same, typeof crypto.subtle, crypto instanceof Crypto].join(' ');"
                   "</script>"),
            "true false false undefined true");
        oks("and refuses what the standard refuses",
            titled("<script>var e1 = '', e2 = '';"
                   "try { crypto.getRandomValues(new Float64Array(2)); } catch (e) { e1 = e.name; }"
                   "try { crypto.getRandomValues(new Uint8Array(65537)); } catch (e) { e2 = e.name; }"
                   "document.title = [e1, e2, crypto.getRandomValues(new Uint8Array(65536)).length].join(' ');"
                   "</script>"),
            "TypeMismatchError QuotaExceededError 65536");
        oks("crypto.randomUUID is a random UUID, on a page that came encrypted",
            titled("<script>var u = crypto.randomUUID(), v = crypto.randomUUID();"
                   "document.title = [/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u),"
                   " u !== v, u.length].join(' ');</script>"),
            "true true 36");
        jsdom_at("http://site.test/");
        oks("and not on one that did not",
            titled("<script>document.title = typeof crypto.randomUUID + ' ' + typeof crypto.getRandomValues;</script>"),
            "undefined function");
        jsdom_random_with(no_random);
        oks("with no source of randomness it says so, and gives nothing",
            titled("<script>var a = new Uint8Array(4), n = 'none';"
                   "try { crypto.getRandomValues(a); } catch (e) { n = e.name; }"
                   "document.title = n + ' ' + a.join(',');</script>"),
            "OperationError 0,0,0,0");
        jsdom_random_with(0);
        jsdom_at("");
    }

    /* --- documents that are not the page ----------------------------------------------
     *
     * jQuery makes one with document.implementation.createHTMLDocument before
     * it will load at all, and Ars Technica, CSS-Tricks and Microsoft stopped
     * there; DOMParser reads a fetched page into one. */
    {
        jsdom_at("https://site.test/page");
        oks("createHTMLDocument makes a document of its own, as jQuery asks for one",
            titled("<body><script>var d = document.implementation.createHTMLDocument('made');"
                   "d.body.innerHTML = '<form></form><form></form>';"
                   "var base = d.createElement('base'); base.href = document.location.href; d.head.appendChild(base);"
                   "document.title = [d.body.childNodes.length, d.nodeType, d instanceof Document, d.nodeName,"
                   " d.documentElement.nodeName, d.title, d.head.lastChild.nodeName, d.body.parentNode.parentNode === d,"
                   " d.body.firstChild.ownerDocument === d, document.forms.length, d.forms.length,"
                   " document.implementation.hasFeature('anything', '1.0')].join(' ');</script></body>"),
            "2 9 true #document HTML made BASE true true 0 2 true");
        oks("DOMParser reads a whole page into one, with its head, body and title, and runs none of it",
            titled("<body><script>var p = new DOMParser().parseFromString('<!doctype html><html lang=fr><head>"
                   "<title> A  page </title><meta name=x content=y></head><body><p id=q>hi"
                   "<script>window.ran = 1<\\/script></p></body></html>', 'text/html');"
                   "document.title = [p.title, p.documentElement.lang, p.head.children.length, p.body.children.length,"
                   " p.querySelector('meta').getAttribute('content'), p.getElementById('q').firstChild.data,"
                   " typeof window.ran, document.getElementById('q'), p.URL].join('|');</script></body>"),
            "A page|fr|2|1|y|hi|undefined||https://site.test/page");
        oks("and markup with no head or body goes where a page's would",
            titled("<body><script>var s = new DOMParser().parseFromString('<title>t</title><p>x</p>', 'text/html');"
                   "document.title = [s.title, s.head.children.length, s.body.innerHTML, s.documentElement.children.length].join('|');"
                   "</script></body>"),
            "t|1|<p>x</p>|2");
        oks("a document of its own has no window, and what is written to it does not reach the page",
            titled("<body><script>var d = document.implementation.createHTMLDocument();"
                   "d.write('<p id=w>written</p>');"
                   "document.title = [d.defaultView, d.readyState, d.hasFocus(), d.cookie === '', d.currentScript,"
                   " d.URL, document.getElementById('w'), d.title === ''].join('|');</script></body>"),
            "|complete|false|true||about:blank||true");
        oks("XML is refused rather than read as HTML, and a type nobody knows is an error",
            titled("<body><script>var r = [];"
                   "try { new DOMParser().parseFromString('<a/>', 'text/xml'); } catch (e) { r.push(e.name); }"
                   "try { new DOMParser().parseFromString('x', 'text/plain'); } catch (e) { r.push(e.name); }"
                   "document.title = r.join(' ');</script></body>"),
            "NotSupportedError TypeError");
        oks("and a page with no title does not take one from markup its script reads",
            titled("<body><p id=o></p><script>new DOMParser().parseFromString('<title>X</title>', 'text/html');"
                   "document.getElementById('o').innerHTML = '<title>Y</title>';"
                   "var t = document.title; document.title = '[' + t + ']';</script></body>"),
            "[]");
        jsdom_at("");
    }

    /* --- walks, window.event and PerformanceObserver ------------------------------------
     *
     * Yahoo's advertising loader stopped on NodeFilter, a CSS-Tricks script
     * on a bare `event`, and LinkedIn's bundle on PerformanceObserver. */
    oks("a TreeWalker walks the tree the standard's way, through whatToShow and a filter",
        titled("<body><div id=r><p>a<b>b</b></p><i>d</i>e</div><script>var r = document.getElementById('r');"
               "var w = document.createTreeWalker(r, NodeFilter.SHOW_ELEMENT), fwd = [], back = [];"
               "while (w.nextNode()) fwd.push(w.currentNode.nodeName);"
               "while (w.previousNode()) back.push(w.currentNode.nodeName);"
               "var t = document.createTreeWalker(r, NodeFilter.SHOW_TEXT, { acceptNode: function(n){"
               " return n.data === 'b' ? NodeFilter.FILTER_SKIP : NodeFilter.FILTER_ACCEPT; } }), texts = [];"
               "for (var n = t.firstChild(); n; n = t.nextSibling()) texts.push(n.data);"
               "t.currentNode = r.querySelector('b'); var up = t.parentNode();"
               "document.title = [fwd.join(','), back.join(','), texts.join(','), up, t.root === r,"
               " w.whatToShow, t.filter !== null].join(' ');</script></body>"),
        "P,B,I B,P,DIV a,d,e  true 1 true");
    oks("a NodeIterator goes through everything in order, and a walk of the document starts at its element",
        titled("<html><body><div id=r><p>a<b>b</b></p><i>d</i>e</div><script>var r = document.getElementById('r');"
               "var it = document.createNodeIterator(r, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT), all = [], x;"
               "while ((x = it.nextNode())) all.push(x.nodeName);"
               "var last = it.previousNode().nodeName + ' ' + it.pointerBeforeReferenceNode;"
               "var d = document.createTreeWalker(document, NodeFilter.SHOW_ELEMENT); d.nextNode();"
               "var top = d.currentNode === document.documentElement; d.parentNode();"
               "document.title = [all.join(','), last, top, d.currentNode === document.documentElement,"
               " NodeFilter.SHOW_ALL].join(' ');</script></body></html>"),
        "DIV,P,#text,B,#text,I,#text,#text #text true true true 4294967295");
    oks("window.event is the event being handled, and nothing otherwise",
        titled("<body><script>var inside = '';"
               "document.body.addEventListener('click', function(e){ inside = event.type + ' ' + (window.event === e); });"
               "document.body.click();"
               "document.title = [typeof event, inside, typeof window.event].join(' ');</script></body>"),
        "undefined click true undefined");
    {
        load("<body><script>var got = [];"
             "performance.mark('early');"
             "var po = new PerformanceObserver(function(list, o){"
             " got.push(list.getEntries().map(function(e){ return e.name; }).join(',') + ':' + (o === po));"
             " document.title = got.join(' ') + ' ' + PerformanceObserver.supportedEntryTypes.join(','); });"
             "po.observe({ type: 'mark', buffered: true });"
             "new PerformanceObserver(function(){ document.title = 'told of a long task'; })"
             ".observe({ type: 'longtask', buffered: true });"
             "var both = 'none'; try { po.observe({ type: 'mark', entryTypes: ['mark'] }); } catch (e) { both = e.name; }"
             "performance.mark('one'); performance.measure('span', 'early', 'one');"
             "document.title = 'waiting ' + got.length + ' ' + both;</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        oks("a PerformanceObserver says nothing while the script runs",
            page.title >= 0 ? page.arena + page.title : "", "waiting 0 TypeError");
        pump_until(1, 1000);
        oks("and on the next pass is told of the page's own marks, the earlier ones too, and of no type it does not record",
            page.title >= 0 ? page.arena + page.title : "", "early,one:true mark,measure");
    }

    /* --- data: addresses -------------------------------------------------------------
     *
     * What they name is in them. Instagram writes nearly every script as one,
     * and none ran. */
    oks("a script whose src is a data: address runs what it carries, in base64 or escaped",
        titled("<body><script src='data:text/javascript;base64,d2luZG93LmEgPSAxOw=='></script>"
               "<script src='data:,window.b%20%3D%202%3B'></script>"
               "<script src='data:text/javascript;base64,@@@'>window.c = 3;</script>"
               "<script>document.title = [window.a, window.b, typeof window.c].join(' ');</script></body>"),
        "1 2 undefined");
    {
        jsdom_at("https://site.test/");
        jsdom_request_with(fake_request);
        asked[0] = 0;
        load("<body><p id=x>none</p><script>"
             "fetch('data:application/json,%7B%22n%22%3A5%7D').then(function(r){"
             " return r.json().then(function(j){ document.getElementById('x').textContent ="
             "  [r.status, r.ok, r.headers.get('content-type'), j.n].join(' '); }); });"
             "</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        for (int k = 0; k < 4; k++) jsdom_requests();
        oks("and a fetch of one is answered from the address itself", content_of(dom_by_id(&page, "x")),
            "200 true application/json 5");
        oks("without asking the network", asked, "");
        jsdom_request_with(0);
        jsdom_at("");
    }

    /* --- Blob, File and blob: addresses ------------------------------------------------
     *
     * Instagram makes a Blob for every beacon it sends, and stopped on the
     * name. */
    {
        jsdom_at("https://site.test/");
        oks("a Blob is its parts' bytes, with a type, and can be cut and read",
            titled("<body><script>var b = new Blob(['h\xc3\xa9', new Uint8Array([33]), new Blob(['!'])], { type: 'Text/Plain' });"
                   "var s = b.slice(1, -1, 'x/y'), out = [b.size, b.type, s.size, s.type, b instanceof Blob];"
                   "var f = new File(['abc'], 'a.txt', { type: 'text/plain', lastModified: 5 });"
                   "out.push(f.name, f.lastModified, f.size, f instanceof Blob, f instanceof File);"
                   "b.text().then(function(t){ out.push(t); return s.arrayBuffer(); })"
                   ".then(function(ab){ out.push(new Uint8Array(ab).length); document.title = out.join(' '); });"
                   "</script></body>"),
            "5 text/plain 3 x/y true a.txt 5 3 true true h\xc3\xa9!! 3");
        jsdom_request_with(fake_request);
        asked[0] = 0;
        load("<body><p id=x>none</p><p id=y>none</p><script>"
             "var u = URL.createObjectURL(new Blob(['window.fromBlob = 7;'], { type: 'text/javascript' }));"
             "var sc = document.createElement('script'); sc.src = u; document.body.appendChild(sc);"
             "var j = URL.createObjectURL(new Blob(['{\"k\":1}'], { type: 'application/json' }));"
             "fetch(j).then(function(r){ return r.json().then(function(o){"
             " document.getElementById('x').textContent = [u.indexOf('blob:https://site.test/'), u.length,"
             "  window.fromBlob, o.k, r.headers.get('content-type')].join(' ');"
             " URL.revokeObjectURL(j); fetch(j).catch(function(e){ document.getElementById('y').textContent = e.name; });"
             "}); });</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        pump_until(1, 1000);                /* the inserted script, on the next pass */
        for (int k = 0; k < 4; k++) jsdom_requests();
        oks("a blob: address is answered from its Blob, for a script and a fetch alike",
            content_of(dom_by_id(&page, "x")), "0 59 7 1 application/json");
        oks("until it is revoked, and never from the network", content_of(dom_by_id(&page, "y")), "TypeError");
        oks("so nothing was sent", asked, "");
        load("<body><script>navigator.sendBeacon('/b', new Blob(['a=1'], { type: 'application/x-www-form-urlencoded' }));"
             "</script></body>");
        run_scripts(&page, err, (int)sizeof(err), 0);
        pump_until(1, 1000);
        oks("and sendBeacon sends a Blob as its bytes, with its type", asked,
            "POST https://site.test/b a=1 application/x-www-form-urlencoded");
        jsdom_request_with(0);
        jsdom_at("");
    }

    /* --- a page that uses up its memory ---------------------------------------
     *
     * At the cap a string or a property came back as nothing, and places that
     * used it without asking took the browser down with the page. The script
     * stops and says why; the browser goes on; and the page's handlers do not
     * run again, each of which would only have run out again. */
    {
        load("<body><p id=out>no</p><script>"
             "document.addEventListener('click', function(){"
             " document.getElementById('out').textContent = 'ran ' + {}.a; });"
             "var o = {}; for (var i = 0; ; i++) o['key number ' + i] = 'value ' + i;"
             "</script></body>");
        char err[128];
        run_scripts(&page, err, (int)sizeof(err), 0);
        int said = 0;
        for (int i = 0; err[i]; i++)
            if (err[i] == 'm' && err[i + 1] == 'e' && err[i + 2] == 'm') said = 1;
        ok("a page whose script uses up its memory stops it, and says so, and this is still here", said);
        jsdom_click(dom_by_id(&page, "out"));
        oks("and its handlers do not run again", content_of(dom_by_id(&page, "out")), "no");
        ok("nor is it woken for timers", jsdom_next_due() < 0);
    }

    puts(failed ? "PAGETEST_FAIL\n" : "PAGETEST_PASS\n");
    return failed;
}
