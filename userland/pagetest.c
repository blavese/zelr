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
#include "jsdom.h"

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

static int failed;

static void ok(const char *what, int cond) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    putc('\n');
    if (!cond) failed++;
}

static void oks(const char *what, const char *got, const char *want) {
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
        int ran = jsdom_run(&page, err, (int)sizeof(err), &changed);

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
        jsdom_run(&page, err, (int)sizeof(err), &changed);
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
        jsdom_run(&page, err, (int)sizeof(err), &changed);

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
        jsdom_run(&page, err, (int)sizeof(err), &changed);
        ok("a script that will not parse is reported", err[0] != 0);
        oks("and the page is still the page", content_of(dom_by_id(&page, "x")), "here");
    }

    {
        load("<body><p id=x>here</p></body>");
        char err[128];
        int changed = 0;
        int ran = jsdom_run(&page, err, (int)sizeof(err), &changed);
        ok("a page with no script costs nothing", ran == 0 && changed == 0);
    }

    puts(failed ? "PAGETEST_FAIL\n" : "PAGETEST_PASS\n");
    return failed;
}
