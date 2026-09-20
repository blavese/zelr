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

/* What jsdom_run used to be: open a world, run what is in the page, and say
   whether anything changed. The browser does these three separately now
   because it keeps the world afterwards, and so does everything below here
   that clicks on something. */
static int run_scripts(ddoc *d, char *err, int errcap, int *changed) {
    if (changed) *changed = 0;
    if (err && errcap) err[0] = 0;
    if (!jsdom_open(d)) return 0;
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
             " 20);</script></body>");

        char err[128];
        int changed = 0;
        run_scripts(&page, err, (int)sizeof(err), &changed);
        oks("a timer has not run merely for being set",
            content_of(dom_by_id(&page, "out")), "waiting");
        ok("and does not run before it is due", jsdom_timers() == 0);

        ok("and runs when it is", pump_until(1, 400) == 1);
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

    puts(failed ? "PAGETEST_FAIL\n" : "PAGETEST_PASS\n");
    return failed;
}
