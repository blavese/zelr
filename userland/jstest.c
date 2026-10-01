/* The JavaScript engine, against scripts with known answers.
 *
 * Each case is a script and the text its last expression should produce.
 * Comparing the printed result rather than an internal value means the
 * conversions are under test too, and those are where this language keeps
 * most of its surprises.
 *
 * The cases that look like trivia are not. `0.1 + 0.2` catches an engine
 * that has faked its numbers with integers; `"5" + 2` against `"5" - 2`
 * catches one that has tidied up the one operator that means two things;
 * and the closure cases catch a scope chain that captures by value.
 */
#include "zelr.h"
#include "alloc.h"
#include "js.h"
#include "jsparse.h"
#include "jsrun.h"

static int failed, ran;

static void show(const char *s) { puts(s); }

/* Runs a script and compares the value of its final expression. */
static void expect(const char *what, const char *src, const char *want) {
    ran++;
    jctx J;
    js_init(&J);

    jval got = js_undef();
    int ok = js_eval_text(&J, src, (u32)strlen(src), &got);

    jstr *text = ok ? js_to_str(&J, got) : 0;
    int same = ok && text && strlen(want) == (int)text->len;
    if (same) {
        for (u32 i = 0; i < text->len; i++)
            if (text->s[i] != want[i]) { same = 0; break; }
    }

    show(same ? "  PASS  " : "  FAIL  ");
    show(what);
    if (!same) {
        show("\n          wanted [");
        show(want);
        show("] got [");
        if (!ok) { show("error: "); show(J.error); }
        else if (text) {
            /* Printed through a buffer, because a jstr is not terminated
               the way puts wants in every path above. */
            char buf[160];
            u32 n = text->len < 159 ? text->len : 159;
            for (u32 i = 0; i < n; i++) buf[i] = text->s[i];
            buf[n] = 0;
            show(buf);
        }
        show("]");
        failed++;
    }
    putc('\n');
    js_done(&J);
}

static void check(const char *what, int good);

/* A bundle of functions of which one is called, read with the bodies left
   for later and read whole: how many nodes each makes, and that both give
   the same answer. The bodies left are most of it, and a library's are. */
static void lazy_reading(void) {
    static char src[24 * 1024];
    int w = 0;
    const char *head = "var lib = {};";
    for (const char *p = head; *p; p++) src[w++] = *p;
    for (int i = 0; i < 100; i++) {
        const char *f = "lib.f = function (a, b) { var t = 0; for (var i = 0; i < a; i++) { t += i * b + (i % 3 ? 1 : 2); }"
                        " if (t > 10) { return { t: t, s: 'x' + t, l: [a, b, t] }; } return null; };";
        for (const char *p = f; *p; p++) src[w++] = *p;
    }
    const char *tail = "result = lib.f(5, 2).t;";
    for (const char *p = tail; *p; p++) src[w++] = *p;
    src[w] = 0;
    int nodes[2];
    char got[2][16];
    for (int eager = 0; eager < 2; eager++) {
        jctx J;
        js_init(&J);
        J.eager = eager;
        int ok = js_run(&J, src, (u32)w);
        jval r = ok ? js_get(&J, js_from_obj(J.global_obj), js_intern(&J, "result", 6)) : js_undef();
        jstr *t = js_to_str(&J, r);
        int n = 0;
        for (; t && n < (int)t->len && n < 15; n++) got[eager][n] = t->s[n];
        got[eager][n] = 0;
        nodes[eager] = J.nnodes;
        js_done(&J);
    }
    check("a bundle read with its bodies left for later gives the answer it gives read whole",
          got[0][0] && got[0][0] == got[1][0] && strlen(got[0]) == strlen(got[1]));
    check("and makes less than a fifth of the nodes", nodes[0] * 5 < nodes[1]);
}

/* A whole script whose answer is its global `result`, for the collector
   (jsgc.h): it must also have run at least `runs` times, and what is left in
   use after it be under `most` bytes. */
static void collected(const char *what, const char *src, const char *want, u32 runs, u32 most) {
    ran++;
    jctx J;
    js_init(&J);
    int ok = js_run(&J, src, (u32)strlen(src));
    jstr *text = 0;
    if (ok) {
        jval r = js_get(&J, js_from_obj(J.global_obj), js_intern(&J, "result", 6));
        text = js_to_str(&J, r);
    }
    int same = ok && text && strlen(want) == (int)text->len && J.gc_runs >= runs && J.allocated < most;
    for (u32 i = 0; same && i < text->len; i++) if (text->s[i] != want[i]) same = 0;
    show(same ? "  PASS  " : "  FAIL  ");
    show(what);
    if (!same) {
        char buf[200];
        u32 n = 0;
        if (!ok) { show("\n          error: "); show(J.error); }
        else if (text) {
            n = text->len < 150 ? text->len : 150;
            for (u32 i = 0; i < n; i++) buf[i] = text->s[i];
            buf[n] = 0;
            show("\n          got ["); show(buf); show("]");
        }
        u32 v = J.gc_runs, k = 0;
        char num[16];
        do { num[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        show(" collections ");
        while (k) { char one[2] = { num[--k], 0 }; show(one); }
        v = J.allocated / 1024; k = 0;
        do { num[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        show(", in use KB ");
        while (k) { char one[2] = { num[--k], 0 }; show(one); }
        failed++;
    }
    putc('\n');
    js_done(&J);
}

static void check(const char *what, int good) {
    ran++;
    show(good ? "  PASS  " : "  FAIL  ");
    show(what);
    if (!good) failed++;
    putc('\n');
}

/* Where a program's memory is (zelr.h), which a collector looks through: the
   stack from here to where it begins, and the data these words are in, one
   given a value and one not. */
static int a_static_word = 1;
static int a_zero_word;

static void where_memory_is(void) {
    volatile int here = 0;
    check("a program knows where its stack begins and where its data lies",
          zelr_stack_top && (char *)&here < zelr_stack_top
          && zelr_data_lo() <= (char *)&a_static_word && (char *)&a_static_word < zelr_data_hi()
          && zelr_data_lo() <= (char *)&a_zero_word && (char *)&a_zero_word < zelr_data_hi());
}

/* The collector from the inside (jsgc.h): what only a pointer into the
   middle of a block holds, the texts and names a context keeps, and a
   context whose own record is in memory from malloc, as a worker's is.
   What is made for the first two is made in calls of their own, and the
   stack they used is written over, so that no copy of the block's start is
   left behind on it for the collector to find instead. */
static volatile const char *held_inside;
static volatile u64 name_hidden;              /* turned inside out: as an address it would hold it */

__attribute__((noinline)) static void make_inside(jctx *J) {
    jstr *s = js_str_n(J, "a string held from inside", 25);
    held_inside = s->s + 9;
}

__attribute__((noinline)) static void make_name(jctx *J) {
    name_hidden = (u64)js_intern(J, "a name only the table holds", 27) ^ 0xFFFFFFFFFFFFFFFFull;
}

__attribute__((noinline)) static void scrub_stack(void) {
    volatile char pad[16384];
    for (int i = 0; i < (int)sizeof(pad); i++) pad[i] = 0;
}

static void collector_inside(void) {
    jctx *J = (jctx *)malloc(sizeof(jctx));
    js_init(J);
    const char *setup = "function kept(a) { return a * 2; } var names = []; for (var i = 0; i < 50; i++) names.push('n' + i);";
    js_run(J, setup, (u32)strlen(setup));

    /* A string held by nothing but a pointer nine bytes into its text. */
    make_inside(J);
    make_name(J);
    scrub_stack();
    js_gc(J);
    for (int i = 0; i < 2000; i++) js_str_n(J, "something else entirely!", 25);
    int held = held_inside[0] == 'h' && held_inside[1] == 'e' && held_inside[2] == 'l';
    check("a block held only from inside is kept", held);

    scrub_stack();
    js_gc(J);
    int texts = J->nsrcs > 0;
    for (int i = 0; i < J->nsrcs; i++) if (js_is_free(J->srcs[i])) texts = 0;
    int names = J->nintern > 0 && !js_is_free((void *)(name_hidden ^ 0xFFFFFFFFFFFFFFFFull));
    for (u32 i = 0; i < J->intern_cap; i++) if (J->intern[i] && js_is_free(J->intern[i])) names = 0;
    check("the texts and the names a context keeps are kept", texts && names);

    int own = !js_is_free(J->global_obj) && !js_is_free(J->s_length) && !js_is_free(J->p_array) && J->gc_runs >= 2;
    jval v = js_undef();
    const char *ask = "[1, 2, 3].length + ':' + kept.toString().length + ':' + names[49]";
    js_eval_text(J, ask, (u32)strlen(ask), &v);
    jstr *t = js_to_str(J, v);
    const char *want = "3:34:n49";
    own = own && t && t->len == (u32)strlen(want);
    for (u32 i = 0; own && i < t->len; i++) if (t->s[i] != want[i]) own = 0;
    check("and a context whose own record is in memory from malloc", own);
    js_done(J);
    free(J);
}

/* And runs a whole script, where the answer is whatever it leaves in a
   global called `result`. */
static void script(const char *what, const char *src, const char *want) {
    ran++;
    jctx J;
    js_init(&J);

    int ok = js_run(&J, src, (u32)strlen(src));
    jstr *text = 0;
    if (ok) {
        jprop *p = js_find(J.global->vars, js_str(&J, "result"));
        text = js_to_str(&J, p ? p->v : js_undef());
    }

    int same = ok && text && strlen(want) == (int)text->len;
    if (same) {
        for (u32 i = 0; i < text->len; i++)
            if (text->s[i] != want[i]) { same = 0; break; }
    }

    show(same ? "  PASS  " : "  FAIL  ");
    show(what);
    if (!same) {
        show("\n          wanted [");
        show(want);
        show("] got [");
        if (!ok) { show("error: "); show(J.error); }
        else if (text) {
            char buf[160];
            u32 n = text->len < 159 ? text->len : 159;
            for (u32 i = 0; i < n; i++) buf[i] = text->s[i];
            buf[n] = 0;
            show(buf);
        }
        show("]");
        failed++;
    }
    putc('\n');
    js_done(&J);
}

/* Runs a setup and then a loop in the same page, and fails if the loop took
   more than `most` bytes of the page's memory or left `result` other than
   `want`. Something made afresh every time round shows here long before it
   runs a page out of memory. */
static void thrift(const char *what, const char *setup, const char *loop, const char *want, u32 most) {
    ran++;
    jctx J;
    js_init(&J);
    int ok = js_run(&J, setup, (u32)strlen(setup));
    u32 before = J.allocated;
    if (ok) ok = js_run(&J, loop, (u32)strlen(loop));
    u32 took = J.allocated - before;
    jstr *text = 0;
    if (ok) {
        jprop *p = js_find(J.global->vars, js_str(&J, "result"));
        text = js_to_str(&J, p ? p->v : js_undef());
    }
    int same = ok && text && strlen(want) == (int)text->len && took <= most;
    for (u32 i = 0; same && i < text->len; i++)
        if (text->s[i] != want[i]) same = 0;
    show(same ? "  PASS  " : "  FAIL  ");
    show(what);
    if (!same) {
        show("\n          ");
        if (!ok) { show("error: "); show(J.error); }
        else { show("took "); putn((int)took); show(" bytes"); }
        failed++;
    }
    putc('\n');
    js_done(&J);
}

int main(void) {
    puts("javascript, in ring 3\n");

    /* --- numbers, which are doubles ------------------------------------- */
    expect("arithmetic", "1 + 2 * 3", "7");
    expect("a third is not zero", "1 / 3", "0.3333333333333333");
    expect("and neither is a tenth", "0.1 + 0.2", "0.30000000000000004");
    expect("division is not integer division", "7 / 2", "3.5");
    expect("the remainder keeps its sign", "-7 % 3", "-1");
    expect("precedence and brackets", "(1 + 2) * 3", "9");
    expect("unary minus", "-(4 - 9)", "5");

    /* --- the one operator that is two ----------------------------------- */
    expect("plus concatenates when a string is involved", "'5' + 2", "52");
    expect("minus never does", "'5' - 2", "3");
    expect("and a number joins a string", "1 + '1'", "11");

    /* --- comparison and equality ---------------------------------------- */
    expect("loose equality converts", "1 == '1'", "true");
    expect("strict equality does not", "1 === '1'", "false");
    expect("not a number equals nothing, not even itself", "NaN === NaN", "false");
    expect("null and undefined are loosely equal", "null == undefined", "true");
    expect("but not strictly", "null === undefined", "false");
    expect("strings compare as text", "'10' < '9'", "true");
    expect("numbers compare as numbers", "10 < 9", "false");

    /* --- truthiness ------------------------------------------------------ */
    expect("an empty string is false", "'' ? 'y' : 'n'", "n");
    expect("zero is false", "0 ? 'y' : 'n'", "n");
    expect("an empty array is true", "[] ? 'y' : 'n'", "y");
    expect("or gives back the value", "0 || 'fallback'", "fallback");
    expect("and short circuits", "null && nothing.here", "null");

    /* --- bitwise, which is briefly integers ------------------------------ */
    expect("bitwise and", "12 & 10", "8");
    expect("shifts", "1 << 10", "1024");
    expect("truncation to thirty-two bits", "~0", "-1");

    /* --- strings --------------------------------------------------------- */
    expect("length", "'hello'.length", "5");
    expect("indexing", "'hello'[1]", "e");
    expect("slice", "'hello world'.slice(6)", "world");
    expect("upper case", "'abc'.toUpperCase()", "ABC");
    expect("split and join", "'a,b,c'.split(',').join('-')", "a-b-c");
    expect("replace", "'one two'.replace('two', 'three')", "one three");
    expect("indexOf when absent", "'abc'.indexOf('z')", "-1");
    expect("a string with an escape in it", "'a\\nb'.length", "3");

    /* --- arrays ---------------------------------------------------------- */
    expect("a literal", "[1,2,3].length", "3");
    expect("joined", "[1,2,3].join('+')", "1+2+3");
    expect("map", "[1,2,3].map(function(x){return x*2}).join(',')", "2,4,6");
    expect("filter", "[1,2,3,4].filter(function(x){return x%2==0}).join(',')",
           "2,4");
    expect("sort is textual by default", "[1,10,2].sort().join(',')", "1,10,2");
    expect("and numeric when told",
           "[1,10,2].sort(function(a,b){return a-b}).join(',')", "1,2,10");
    expect("an array as text", "String([1,2])", "1,2");

    /* --- objects --------------------------------------------------------- */
    expect("a literal and a dot", "({a: 1, b: 2}).b", "2");
    expect("and an index", "({a: 1})['a']", "1");
    expect("a missing property", "typeof ({}).nothing", "undefined");
    expect("keys", "Object.keys({x:1, y:2}).length", "2");

    /* --- typeof ---------------------------------------------------------- */
    expect("of a number", "typeof 1", "number");
    expect("of a string", "typeof 'a'", "string");
    expect("of null, which is the famous mistake", "typeof null", "object");
    expect("of a function", "typeof function(){}", "function");
    expect("of something never declared", "typeof neverDeclared", "undefined");

    /* --- Math and JSON ---------------------------------------------------- */
    expect("floor of a negative", "Math.floor(-1.5)", "-2");
    expect("round sends halves up", "Math.round(2.5)", "3");
    expect("square root", "Math.sqrt(16)", "4");
    expect("max of several", "Math.max(3, 9, 4)", "9");
    expect("pow", "Math.pow(2, 10)", "1024");
    expect("stringify", "JSON.stringify([1,'a',true])", "[1,\"a\",true]");
    expect("parse and read back", "JSON.parse('{\"n\": 42}').n", "42");
    expect("a round trip", "JSON.parse(JSON.stringify({a:[1,2]})).a.join('')",
           "12");
    expect("parseInt stops at the first thing it cannot use",
           "parseInt('12px')", "12");
    expect("but a plain conversion does not", "Number('12px')", "NaN");

    /* --- functions and closures ------------------------------------------ */
    script("a function returns",
           "function add(a, b) { return a + b } result = add(2, 3)", "5");
    script("a function declared below is still callable above",
           "result = later(); function later() { return 'yes' }", "yes");
    script("a closure captures the variable, not its value",
           "function counter() { var n = 0;"
           "  return function() { n = n + 1; return n } }"
           "var c = counter(); c(); c(); result = c()", "3");
    script("two closures do not share",
           "function mk(){var n=0; return function(){return ++n}}"
           "var a=mk(), b=mk(); a(); a(); result = a() + ':' + b()", "3:1");
    script("recursion",
           "function fact(n){ return n <= 1 ? 1 : n * fact(n-1) }"
           "result = fact(10)", "3628800");
    script("arguments",
           "function n(){ return arguments.length } result = n(1,2,3)", "3");

    /* A function's body is read when it is first called (jsparse.h,
       js_parse_lazy): what is passed over must be passed over exactly --
       braces in strings, templates and patterns are not the body's -- and
       what is read later must be what was written. */
    script("a body passed over reads the same when it is called: braces in templates, strings and patterns",
           "function f(x) { var t = `a${ {b: 1}.b }c${ `in${ x }ner` }d}`; var s = '}{'; var r = /[}{]+/g;"
           "  if (x) { return t + s + '}'.replace(r, '!') + (function () { return '{'; })(); } return 0; }"
           "result = f(5)", "a1cin5nerd}}{!{");
    script("and a division after a bracket is not a pattern",
           "function g(a, b) { var q = (a) / b / 2; return q; } result = g(8, 2)", "2");
    script("its strictness, its arguments and its vars are its own",
           "function s() { 'use strict'; return this === undefined; }"
           "function a() { var v = 1; function inner() { return v + arguments.length; } return inner(1, 2); }"
           "function h() { return typeof later; var later = 1; }"
           "result = [s(), a(), h()].join(' ')", "true 3 undefined");
    script("a generator, an async function and a function that makes functions, each read when first run",
           "function* gen() { yield 1; yield 2; } function mk(n) { return function () { return n * 2; }; }"
           "async function as() { return 7; } var out = [];"
           "as().then(function (v) { out.push(v); });"
           "result = [...gen()].join('') + ' ' + mk(4)() + ' ' + mk(5)()", "12 8 10");
    script("a mistake in a body is told when the function is called, as a SyntaxError the page can catch",
           "function broken() { var = 1; } var r = 'read';"
           "try { broken(); } catch (e) { r += ' ' + e.name; } try { broken(); } catch (e) { r += ' again'; }"
           "result = r", "read SyntaxError again");
    script("and a function passed over still says what it was written as",
           "function shown(a, b) { return `${a}+${b}`; } result = shown.toString() + ' ' + shown(1, 2)",
           "function shown(a, b) { return `${a}+${b}`; } 1+2");

    /* --- statements ------------------------------------------------------- */
    script("a for loop",
           "var s = 0; for (var i = 1; i <= 100; i++) s += i; result = s",
           "5050");
    script("while with a break",
           "var i = 0; while (true) { i++; if (i > 5) break } result = i", "6");
    script("continue skips",
           "var s=0; for (var i=0;i<10;i++){ if(i%2) continue; s+=i } result=s",
           "20");
    script("do runs once even when false",
           "var n = 0; do { n++ } while (false); result = n", "1");
    script("for in over an object",
           "var o = {a:1,b:2,c:3}, n = 0; for (var k in o) n += o[k];"
           "result = n", "6");
    script("for in over an array gives indices",
           "var a = ['x','y'], s = ''; for (var i in a) s += i; result = s",
           "01");
    script("switch falls through",
           "var s=''; switch(2){ case 1: s+='a'; case 2: s+='b';"
           " case 3: s+='c'; break; case 4: s+='d' } result = s", "bc");
    script("switch takes the default",
           "var s='no'; switch(9){ case 1: s='a'; break; default: s='fell' }"
           "result = s", "fell");
    script("throw and catch",
           "var r; try { throw 'bang' } catch (e) { r = 'caught ' + e }"
           "result = r", "caught bang");
    script("finally runs anyway",
           "var s=''; try { s+='t'; throw 1 } catch(e) { s+='c' }"
           " finally { s+='f' } result = s", "tcf");
    script("a thrown value from a function is caught outside it",
           "function bad(){ throw 'inner' }"
           "try { bad() } catch(e) { result = e }", "inner");

    /* --- assignment and mutation ------------------------------------------ */
    script("compound assignment", "var n = 10; n += 5; n *= 2; result = n", "30");
    script("post increment returns the old value",
           "var n = 5; var was = n++; result = was + ':' + n", "5:6");
    script("pre increment returns the new one",
           "var n = 5; var now = ++n; result = now + ':' + n", "6:6");
    script("an object is a reference",
           "var a = {n:1}; var b = a; b.n = 9; result = a.n", "9");
    script("an array grows",
           "var a = []; for (var i=0;i<5;i++) a.push(i*i); result = a.join(',')",
           "0,1,4,9,16");
    script("delete removes a property",
           "var o = {a:1,b:2}; delete o.a; result = Object.keys(o).join(',')",
           "b");

    /* --- something with a bit of everything ------------------------------- */
    script("a small program",
           "function Tally(){ this.items = []; }"
           "var t = new Tally();"
           "for (var i = 1; i <= 20; i++) { if (i % 3 === 0) t.items.push(i); }"
           "result = t.items.length + ':' + t.items.join(',')",
           "6:3,6,9,12,15,18");

    /* --- the refusals, which have to be refusals -------------------------- */
    {
        ran++;
        jctx J;
        js_init(&J);
        int ok = js_run(&J, "export default 1", 16);
        int said = 0;
        for (const char *q = J.error; !ok && *q; q++)
            if (q[0] == 'm' && q[1] == 'o' && q[2] == 'd' && q[3] == 'u' && q[4] == 'l' && q[5] == 'e') said = 1;
        puts(said ? "  PASS  " : "  FAIL  ");
        puts("export outside a module is refused, and says where it belongs");
        if (said) { puts(" ("); puts(J.error); puts(")"); }
        putc('\n');
        if (!said) failed++;
        js_done(&J);
    }
    {
        ran++;
        jctx J;
        js_init(&J);
        int ok = js_run(&J, "var x = ;", 9);
        int said = !ok && J.error[0];
        puts(said ? "  PASS  " : "  FAIL  ");
        puts("and so is something that is not JavaScript at all");
        putc('\n');
        if (!said) failed++;
        js_done(&J);
    }
    {
        ran++;
        jctx J;
        js_init(&J);
        J.time_cap = 50;
        int ok = js_run(&J, "while (true) {}", 15);
        int stopped = !ok && J.error[0];
        puts(stopped ? "  PASS  " : "  FAIL  ");
        puts("a script that never finishes is stopped rather than hanging");
        putc('\n');
        if (!stopped) failed++;
        js_done(&J);
    }
    {
        /* Stopped by the clock, not by a count: more steps than the forty
           million that used to be all a run had finish, and one that never
           ends stops about when its time is up. */
        ran++;
        jctx J;
        js_init(&J);
        J.time_cap = 6000;          /* the count is what is asked about, on however slow a machine */
        const char *src = "var s = 0; for (var i = 0; i < 16000000; i++) s += i & 1; result = s;";
        int ok = js_run(&J, src, (u32)strlen(src));
        jval got = js_get(&J, js_from_obj(J.global_obj), js_str(&J, "result"));
        int whole = ok && got.t == JS_NUM && got.num == 8000000;
        js_done(&J);
        js_init(&J);
        J.time_cap = 30;
        int t0 = ticks();
        ok = js_run(&J, "while (true) {}", 15);
        int took = ticks() - t0;
        int in_time = !ok && took >= 30 && took < 300;
        js_done(&J);
        puts(whole && in_time ? "  PASS  " : "  FAIL  ");
        puts("a long run finishes, and one that never ends is stopped when its time is up");
        putc('\n');
        if (!(whole && in_time)) {
            failed++;
            puts("          finished ");
            puts(whole ? "yes" : "no");
            puts(", stopped after ");
            char b[12]; int k = 0, v = took < 0 ? 0 : took; char t[12];
            do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
            int w = 0; while (k) b[w++] = t[--k]; b[w] = 0;
            puts(b);
            puts(" ticks\n");
        }
    }

    /* --- regular expressions -------------------------------------------
     *
     * The first two are the ones that decide whether any of the rest can be
     * trusted, because a slash and a pattern are the same character and
     * telling them apart is not a matter of looking at the slash.
     */
    expect("a slash after a value is a divide", "(6 / 2)", "3");
    expect("and a slash where no value can be is a pattern",
           "/2/.source", "2");

    expect("a pattern says when it matches", "/b/.test('abc')", "true");
    expect("and when it does not", "/z/.test('abc')", "false");
    expect("case folded when it is asked to be", "/ABC/i.test('xabcx')",
           "true");
    expect("and not when it is not", "/ABC/.test('xabcx')", "false");

    expect("digits", "/^\\d+$/.test('12345')", "true");
    expect("and something that is not one", "/^\\d+$/.test('12a45')",
           "false");
    expect("a class with a range", "/^[a-f]+$/.test('faded')", "true");
    expect("a class turned inside out", "/^[^0-9]+$/.test('abc')", "true");
    expect("alternation", "/^(cat|dog)$/.test('dog')", "true");
    expect("and an empty branch, which is how a page says optional",
           "/^(ab|)$/.test('')", "true");

    expect("a counted quantifier", "/^a{3}$/.test('aaa')", "true");
    expect("one that is not met", "/^a{3}$/.test('aa')", "false");
    expect("a range of counts", "/^a{2,4}$/.test('aaa')", "true");
    expect("a brace that is not a quantifier is four characters",
           "/^a{b}$/.test('a{b}')", "true");

    expect("greedy takes as much as it can",
           "'<<a>><<b>>'.replace(/<<.+>>/, 'x')", "x");
    expect("lazy takes as little", "'<<a>><<b>>'.replace(/<<.+?>>/, 'x')",
           "x<<b>>");

    expect("a word boundary", "/\\bcat\\b/.test('the cat sat')", "true");
    expect("and not one inside a word", "/\\bcat\\b/.test('concatenate')",
           "false");

    expect("a group comes back out", "/(\\d+)-(\\d+)/.exec('x 12-34')[2]",
           "34");
    expect("and so does where it was found",
           "/(\\d+)/.exec('ab 99').index", "3");
    expect("a group that matched nothing is undefined",
           "typeof /(a)|(b)/.exec('b')[1]", "undefined");

    expect("replace puts a group back with a dollar",
           "'John Smith'.replace(/(\\w+) (\\w+)/, '$2, $1')", "Smith, John");
    expect("and the whole match with an ampersand",
           "'abc'.replace(/b/, '[$&]')", "a[b]c");
    expect("one at a time without g", "'a b c'.replace(/\\s/, '-')",
           "a-b c");
    expect("and all of them with it", "'a b c'.replace(/\\s/g, '-')",
           "a-b-c");
    expect("a function decides the replacement",
           "'a1b2'.replace(/\\d/g, function(m){ return '[' + m + ']'; })",
           "a[1]b[2]");

    expect("match gives every one when global",
           "'a1b22c333'.match(/\\d+/g).join(',')", "1,22,333");
    expect("and the groups when it is not",
           "'ab 99'.match(/(\\d)(\\d)/)[2]", "9");
    expect("no match is null", "'abc'.match(/\\d/)", "null");
    expect("search gives where it starts", "'hello world'.search(/world/)",
           "6");
    expect("and minus one when it is not there", "'hello'.search(/z/)",
           "-1");
    expect("split on a pattern", "'a1b22c'.split(/\\d+/).join('-')",
           "a-b-c");

    expect("the trim every page writes",
           "'  hi  '.replace(/^\\s+|\\s+$/g, '')", "hi");
    expect("anchors that mean the line, with m",
           "'a\\nb'.match(/^b$/m)[0]", "b");
    expect("and the string without it", "/^b$/.test('a\\nb')", "false");

    expect("a pattern built out of a string",
           "RegExp('^a+$').test('aaa')", "true");
    expect("with its flags", "RegExp('abc', 'i').test('ABC')", "true");

    /* Fifty groups was the most a pattern could have, and pages build them
       out of many alternatives each in its group. */
    expect("a pattern with a hundred groups, each caught and the last named",
           "(function(){ var m = new RegExp('(b)'.repeat(99) + '(?<z>c)').exec('x' + 'b'.repeat(99) + 'c');"
           " return [m.length, m[1], m[99], m[100], m.groups.z].join(' '); })()", "101 b b c c");
    expect("a pattern of three hundred words, each an alternative, finds the last",
           "(function(){ var w = []; for (var i = 0; i < 300; i++) w.push('wd' + i + 'x');"
           " var r = new RegExp('\\\\b(?:' + w.join('|') + ')\\\\b'); return r.exec('a wd299x b')[0]; })()",
           "wd299x");
    /* A pattern with more than 96 classes stopped Netflix's page ("too many
       classes"), and one past 2048 ranges above U+00FF would have next: each
       class here holds the marks, 321 ranges past U+00FF, so eight hold 2568. */
    expect("a pattern of two hundred classes, and one whose classes hold 2568 ranges past U+00FF",
           "(function(){ var r = new RegExp('^' + '[a-c]'.repeat(200) + '$'), s = 'abc'.repeat(67).slice(0, 200);"
           " var u = new RegExp('^' + '[\\\\p{Mn}\\\\p{Mc}a-z]'.repeat(8) + '$', 'u');"
           " return [r.test(s), r.test(s + 'a'), u.test('abcdefgh'), u.test('abcdefg1'), u.test('abcdefg\\u0301')].join(' '); })()",
           "true false true false true");
    expect("and a reference back to the ninetieth of them",
           "new RegExp('(a)'.repeat(95) + '\\\\90').test('a'.repeat(96))", "true");

    expect("exec walks a global pattern through its subject",
           "(function(){ var r = /\\d/g, s = 'a1b2', n = 0;"
           " while (r.exec(s) !== null) n++; return n; })()", "2");
    expect("and a pattern that can match nothing still ends",
           "'aaa'.replace(/b*/g, '-')", "-a-a-a-");

    /* --- arrow functions -------------------------------------------------
     *
     * The one that matters most is the last: a bracket is a bracket until a
     * => follows it, and an engine that guesses wrong breaks ordinary
     * arithmetic everywhere.
     */
    expect("an arrow with one argument", "(x => x * 2)(4)", "8");
    expect("and with several", "((a, b) => a + b)(2, 3)", "5");
    expect("and with none at all", "(() => 7)()", "7");
    expect("a body in braces says what it returns",
           "((x) => { return x + 1; })(4)", "5");
    expect("one arrow can return another",
           "(a => b => a + b)(1)(2)", "3");
    expect("this comes from where it was written, not where it is called",
           "(function(){ var o = { v: 5, get: function(){"
           " var f = () => this.v; return f(); } }; return o.get(); })()",
           "5");
    expect("and a bracket that is not an arrow is still a bracket",
           "(1 + 2) * 3", "9");
    expect("even when it holds a list", "(function(a,b){return b;})(1, 2)",
           "2");

    /* --- instanceof, and labels ------------------------------------------ */
    expect("an object knows what made it",
           "(function(){ function Thing(){} return new Thing() instanceof"
           " Thing; })()", "true");
    expect("and what did not",
           "(function(){ function A(){} function B(){} return new A()"
           " instanceof B; })()", "false");
    expect("an array is an Array", "[1,2] instanceof Array", "true");
    expect("and an object is not", "({}) instanceof Array", "false");
    expect("a function is a Function",
           "(function(){}) instanceof Function", "true");
    expect("something that is not an object is not an instance of anything",
           "5 instanceof Object", "false");

    expect("break leaves the loop it names, not the one it is in",
           "(function(){ var n = 0;"
           " outer: for (var i = 0; i < 3; i++) {"
           "   for (var j = 0; j < 3; j++) { n++; if (j == 1) break outer; }"
           " } return n; })()", "2");
    expect("and continue goes round the one it names",
           "(function(){ var n = 0;"
           " outer: for (var i = 0; i < 3; i++) {"
           "   for (var j = 0; j < 3; j++) { n++; continue outer; }"
           " } return n; })()", "3");
    expect("a plain break still leaves the nearest loop",
           "(function(){ var n = 0;"
           " for (var i = 0; i < 3; i++) {"
           "   for (var j = 0; j < 3; j++) { n++; break; }"
           " } return n; })()", "3");

    /* --- call, apply and bind ---------------------------------------------
       All three threw "this is not a function" until the target was taken
       from `this`, where the call hands it over, rather than looked for on
       it under a name only the wrapper had. */
    expect("call runs a function with the receiver it is given",
           "(function(){ function f(a, b){ return this.v + a + b; }"
           " return f.call({ v: 1 }, 2, 3); })()", "6");
    expect("apply does the same with the arguments in an array",
           "(function(){ function f(a, b){ return this.v + a + b; }"
           " return f.apply({ v: 1 }, [2, 3]); })()", "6");
    expect("bind keeps the receiver for later",
           "(function(){ function f(a, b){ return this.v + a + b; }"
           " var g = f.bind({ v: 10 }); return g(2, 3); })()", "15");
    expect("and the arguments given with it, in front of the later ones",
           "(function(){ function f(a, b){ return this.v + a + b; }"
           " var g = f.bind({ v: 10 }, 5); return g(1); })()", "16");
    expect("a built-in can be applied too", "Math.max.apply(null, [3, 7, 5])", "7");
    expect("and a method borrowed from one object runs on another",
           "(function(){ var a = { n: 'a', who: function(){ return this.n; } };"
           " return a.who.call({ n: 'b' }); })()", "b");

    /* --- for-in over a name declared earlier ------------------------------ */
    expect("for (k in o) with k declared before the loop",
           "(function(){ var o = { a: 1, b: 2 }, k, n = 0;"
           " for (k in o) n += o[k]; return n; })()", "3");
    expect("and in is still an operator inside brackets in a for clause",
           "(function(){ var o = { a: 1 }, n = 0;"
           " for (var i = ('a' in o) ? 1 : 0; i < 3; i++) n++; return n; })()",
           "2");
    expect("and inside a function written there",
           "(function(){ var n = 0; for (var f = function(o){ return 'a' in o; };"
           " n < 1; n++) {} return f({ a: 1 }); })()", "true");

    /* --- decimals, printed as they were written -------------------------- */
    expect("a price prints as it was written", "0.57", "0.57");
    expect("another", "19.99", "19.99");
    expect("and one that sits just below its decimal", "1.15", "1.15");
    expect("a long fraction is rounded, not cut", "100 / 3", "33.333333333333336");
    expect("a negative one", "-2.5", "-2.5");
    expect("a millionth is still a decimal", "0.000001", "0.000001");
    expect("a ten millionth is not", "1.5e-7", "1.5e-7");
    expect("a large whole number prints in full", "1e20", "100000000000000000000");
    expect("and one past that gets an exponent", "1e21", "1e+21");

    /* --- decimals, read and written exactly -------------------------------
       The shortest text that reads back as the same double, and a literal
       read as the double nearest it. Both were done in doubles: 0.3 read as
       the one above it, fifteen digits hid that, and 0.1 + 0.2 printed as
       0.3 while comparing unequal to it. */
    expect("a sum of tenths is not three tenths", "0.1 + 0.2 === 0.3", "false");
    expect("but three tenths is", "0.3", "0.3");
    expect("and reads as the double nearest it", "0.3 === 3 / 10", "true");
    expect("two thirds", "2 / 3", "0.6666666666666666");
    expect("seventeen digits where it takes seventeen",
           "0.1234567890123456789", "0.12345678901234568");
    expect("the smallest number there is", "5e-324", "5e-324");
    expect("and the largest", "1.7976931348623157e308", "1.7976931348623157e+308");
    expect("past that is infinity", "1.8e308", "Infinity");
    expect("a whole number past two to the fifty three",
           "Math.pow(2, 60)", "1152921504606847000");
    expect("a halfway case read to even", "9007199254740993", "9007199254740992");
    expect("and one a hair past halfway read up",
           "9007199254740993.0000000001", "9007199254740994");
    expect("a string reads the same way", "Number('0.1') + Number('0.2')",
           "0.30000000000000004");
    expect("parseFloat takes the decimal at the front", "parseFloat('1-2')", "1");
    expect("and knows infinity", "parseFloat('-Infinity')", "-Infinity");

    /* --- new, and what follows it -----------------------------------------
       The constructor after `new` is names, dots and brackets; the first
       parentheses are its arguments and anything after belongs to the
       object made. The whole chain was read first, so new X().y was new of
       X().y. */
    expect("a member of something just made",
           "(function(){ function X(){ this.y = 7; } return new X().y; })()", "7");
    expect("a method called on something just made",
           "new RegExp('a').test('xyz')", "false");
    expect("a constructor found through a namespace",
           "(function(){ var ns = { K: function(){ this.v = 3; } }; return new ns.K().v; })()",
           "3");
    expect("and new with no arguments at all",
           "(function(){ function X(){ this.y = 2; } var o = new X; return o.y; })()", "2");

    /* --- keys, in the order they were made --------------------------------
       Walked by hash bucket they came out in any order, so a page that
       built a menu from an object's keys shuffled it. The index-like keys
       come first, smallest first, as the standard says. */
    expect("keys in the order they were made", "Object.keys({ x: 1, y: 2, z: 3 }).join()",
           "x,y,z");
    expect("and JSON writes them that way", "JSON.stringify({ b: 1, a: 2 })", "{\"b\":1,\"a\":2}");
    expect("and for-in walks them that way",
           "(function(){ var s = ''; for (var k in { b: 1, a: 2, c: 3 }) s += k; return s; })()",
           "bac");
    expect("with the numbered ones first, in number order",
           "Object.keys({ b: 1, 2: 1, 1: 1, a: 1 }).join()", "1,2,b,a");
    expect("in number order, not text order", "Object.keys({ 10: 1, 2: 1 }).join()", "2,10");
    expect("a key deleted and made again goes to the end",
           "(function(){ var o = { a: 1, b: 2 }; delete o.a; o.a = 3; return Object.keys(o).join(); })()",
           "b,a");
    expect("and the engine's own bookkeeping is not among them",
           "(function(){ function X(){ this.v = 1; } return Object.keys(new X()).join(); })()", "v");
    expect("JSON leaves out a key it cannot write",
           "JSON.stringify({ a: 1, b: undefined, c: function(){} })", "{\"a\":1}");

    /* --- errors are objects -----------------------------------------------
       The engine threw strings, so e.message was undefined, nothing it
       raised was an Error, and there was no Error for a page to throw. */
    expect("reading a property of null throws a TypeError, which is an Error",
           "(function(){ try { null.x; } catch (e) {"
           " return (e instanceof TypeError) + ':' + (e instanceof Error) + ':' + e.name; } })()",
           "true:true:TypeError");
    expect("a page's own Error has a message and prints with its name",
           "(function(){ try { throw new Error('boom'); } catch (e) {"
           " return String(e) + '|' + e.message; } })()", "Error: boom|boom");
    expect("Error works without new", "Error('x').message", "x");
    expect("and what is thrown through a return reaches the catch",
           "(function(){ function g(){ throw new Error('deep'); } function f(){ return g(); }"
           " try { f(); } catch (e) { return e.message; } })()", "deep");
    expect("a call too deep is a RangeError",
           "(function(){ function r(){ return r(); } try { r(); } catch (e) {"
           " return e instanceof RangeError; } })()", "true");
    expect("calling what is not a function says which, as a TypeError",
           "(function(){ var o = {}; try { o.nope(); } catch (e) {"
           " return e.name + ': ' + e.message; } })()", "TypeError: nope is not a function");
    expect("an error's name and message are not among its keys",
           "Object.keys(new TypeError('t')).length", "0");

    /* --- memory a call gives back -----------------------------------------
       A call's scope, its variables and its arguments object were never
       given back, six hundred bytes a call, and a page that called a small
       function in a loop ran out of memory in about forty thousand calls.
       The later cases are what must not be given back: each makes the
       thing to keep and then a thousand calls that would reuse it. */
    expect("two hundred thousand calls to a small function",
           "(function(){ function f(x){ return x + 1; } var n = 0;"
           " for (var i = 0; i < 200000; i++) n = f(n); return n; })()", "200000");
    expect("and a string method called as many times",
           "(function(){ var s = 'abc', n = 0;"
           " for (var i = 0; i < 200000; i++) n += s.charCodeAt(i % 3); return n; })()",
           "19599999");
    expect("and an array method",
           "(function(){ var a = []; for (var i = 0; i < 200000; i++) a.push(i);"
           " return a.length; })()", "200000");
    expect("a closure still sees the call that made it",
           "(function(){ function mk(v){ return function(){ return v; }; }"
           " var c = mk('kept'); for (var i = 0; i < 1000; i++) mk('other' + i);"
           " return c(); })()", "kept");
    expect("arguments handed back outlive the call",
           "(function(){ function a(){ return arguments; } var x = a(1, 2, 3);"
           " for (var i = 0; i < 1000; i++) a(9, 9, 9); return Array.prototype.join.call(x); })()", "1,2,3");
    expect("a closure made in a catch keeps what it caught",
           "(function(){ var f; try { throw 7; } catch (e) { f = function(){ return e; }; }"
           " for (var i = 0; i < 1000; i++) { try { throw i; } catch (e) {} }"
           " return f(); })()", "7");
    expect("and a method taken away to call later keeps its receiver",
           "(function(){ var s = 'xyz'; var at = s.charAt; return at.call(s, 1); })()", "y");

    /* --- string methods as the language has them ---------------------------
     *
     * substring, substr and slice were one function, positions were ignored,
     * replaceAll replaced once, and a string pattern's replacement was taken
     * as plain text. */
    expect("substring takes its ends in either order", "'hello'.substring(3, 1)", "el");
    expect("and holds a negative at the start", "'hello'.substring(-2, 2)", "he");
    expect("substr is a start and a length", "'hello'.substr(1, 3)", "ell");
    expect("counted from the end when the start is negative", "'hello'.substr(-3, 2)", "ll");
    expect("indexOf starts where it is told", "'abcabc'.indexOf('b', 2)", "4");
    expect("and lastIndexOf looks back from where it is told",
           "'abcabc'.lastIndexOf('b') + ',' + 'abcabc'.lastIndexOf('b', 3)", "4,1");
    expect("includes and startsWith take a position",
           "'abcabc'.includes('a', 4) + ',' + 'abcabc'.startsWith('ca', 2)", "false,true");
    expect("endsWith, with and without an end",
           "'hello.txt'.endsWith('.txt') + ',' + 'hello.txt'.endsWith('lo', 5)", "true,true");
    expect("padStart and padEnd", "'5'.padStart(3, '0') + '|' + 'ab'.padEnd(5, '.-')", "005|ab.-.");
    expect("trimStart and trimEnd", "'[' + '  x  '.trimStart() + '][' + '  x  '.trimEnd() + ']'",
           "[x  ][  x]");
    expect("at counts from the end", "'hello'.at(-1) + 'hello'.at(0)", "oh");
    expect("concat", "'a'.concat('b', 1, true)", "ab1true");
    expect("replaceAll with a string replaces them all", "'a-b-c'.replaceAll('-', '+')", "a+b+c");
    expect("and replace still replaces the first", "'a-b-c'.replace('-', '+')", "a+b-c");
    expect("a string pattern's replacement knows $& and $$",
           "'cost 5'.replace('5', '[$&] $$')", "cost [5] $");
    expect("and a function replacement is called with the match and where it was",
           "'a-b'.replaceAll('-', function(m, at){ return '(' + m + at + ')'; })", "a(-1)b");
    expect("var with no value leaves a value that is there",
           "(function(){ var n = 5; var n; return n; })()", "5");
    expect("so a total declared again inside a loop keeps adding up",
           "(function(){ var t = 0; for (var i = 0; i < 3; i++) { var acc; acc = (acc || 0) + 1;"
           " t = acc; } return t; })()", "3");
    expect("but let with no value starts again each time round",
           "(function(){ var r = ''; for (var i = 0; i < 2; i++) { let x; r += x + ';'; x = 1; }"
           " return r; })()", "undefined;undefined;");
    script("this at the top of a script is the global object",
           "var g = this; g.seen = 5; var result = typeof g + ',' + seen;", "object,5");
    script("so the wrapper a library ships in works",
           "(function(root){ root.lib = { v: 7 }; })(this); var result = lib.v;", "7");
    /* --- things that ran the stack out, and JSON as it is written -------------- */
    expect("an array inside itself becomes text, empty where it comes round again",
           "(function(){ var a = [1, 2]; a.push(a); return String(a); })()", "1,2,");
    expect("and an array two hundred deep becomes text at once",
           "(function(){ var a = [1]; for (var i = 0; i < 200; i++) a = [a]; return String(a); })()", "1");
    expect("JSON refuses an object inside itself rather than running out of stack",
           "(function(){ var o = {}; o.me = o; try { JSON.stringify(o); return 'wrote it'; }"
           " catch (e) { return e.name; } })()", "TypeError");
    expect("JSON escapes control characters",
           "JSON.stringify('a\\u0001b\\r\\n')", "\"a\\u0001b\\r\\n\"");
    expect("and writes what is not a number as null",
           "JSON.stringify([NaN, 1/0, undefined, function(){}, 2])", "[null,null,null,null,2]");
    expect("and nothing at all for undefined", "typeof JSON.stringify(undefined)", "undefined");
    expect("and a long answer without using up the page",
           "(function(){ var a = []; for (var i = 0; i < 20000; i++) a.push({ n: i, s: 'text' });"
           " return JSON.stringify(a).length; })()", "448891");
    expect("JSON.parse undoes \\u escapes, a pair of halves as one character",
           "(function(){ var s = JSON.parse('\"\\\\u00e9\\\\ud83d\\\\ude00\"'); return s.length; })()", "3");
    expect("and refuses what is not JSON",
           "(function(){ var bad = ['tru', '{a:1}', '[1,]', '01', '\"x', 'nul', '1 2'], n = 0;"
           " for (var i = 0; i < bad.length; i++) { try { JSON.parse(bad[i]); } catch (e) { n++; } }"
           " return n; })()", "7");
    expect("but reads what is",
           "JSON.stringify(JSON.parse(' {\"a\": [1, -2.5e3, true, null], \"b\": \"x\\\\ny\"} '))",
           "{\"a\":[1,-2500,true,null],\"b\":\"x\\ny\"}");
    expect("an array added to a number is its text and the number", "[1, 2] + 3", "1,23");
    expect("two arrays added are their texts", "([] + []) === '' && [1] + [2] === '12'", "true");
    expect("an array loosely equal to true, false and a number, as numbers",
           "[1] == true && [0] == false && [] == false && [2] == 2", "true");
    expect("and to its own text", "[1, 2] == '1,2' && ({}) == '[object Object]'", "true");
    expect("a label on a block is the block's, not the first loop's inside it",
           "(function(){ var r = 'none'; lbl: { for (;;) { break lbl; } r = 'after'; } return r; })()", "none");
    expect("break with a label leaves a for-in and what it is inside",
           "(function(){ var n = 0; outer: for (var i = 0; i < 3; i++) { for (var k in {a:1, b:2}) {"
           " n++; continue outer; } } return n; })()", "3");
    expect("and a labelled break in a switch leaves the loop round it",
           "(function(){ var n = 0; outer: for (var i = 0; i < 5; i++) { switch (i) {"
           " case 2: break outer; default: n++; } } return n; })()", "2");
    expect("while a plain break in a switch leaves only the switch",
           "(function(){ var n = 0; for (var i = 0; i < 3; i++) { switch (i) { case 1: break;"
           " default: n++; } } return n; })()", "2");
    expect("an index too far out to keep among the elements is kept all the same, and nothing written past",
           "(function(){ var a = [1]; a[150000000] = 5; return a[150000000] + ',' + a[0] + ',' + a.length; })()",
           "5,1,1");
    expect("the last name that is not an index is a property, not an element",
           "(function(){ var a = []; a[4294967295] = 1; return a[4294967295] + ',' + a.length; })()", "1,0");
    expect("and one past four billion does not wrap round to the first element",
           "(function(){ var a = [9]; a['4294967296'] = 2; return a[0] + ',' + a['4294967296']; })()", "9,2");
    expect("and a name with a leading zero is a name",
           "(function(){ var a = [0, 1]; a['01'] = 7; return a[1] + ',' + a['01']; })()", "1,7");
    expect("a longer length becomes the length, filled with nothing",
           "(function(){ var a = [1]; a.length = 3; return a.length + ',' + a[2]; })()", "3,undefined");
    expect("and a length that is not one is refused",
           "(function(){ try { [].length = -1; return 'kept'; } catch (e) { return e.name; } })()", "RangeError");
    expect("a loop walking a string by its matches ends",
           "(function(){ var s = 'x.y.z', n = 0, at = s.indexOf('.');"
           " while (at >= 0 && n < 10) { n++; at = s.indexOf('.', at + 1); } return n; })()", "2");

    /* --- the language of 2015 to 2021 ---------------------------------------
     *
     * Pages stopped at their first line on every one of these: a minified
     * bundle is arrow functions, let, spread and destructuring from one end to
     * the other. */
    expect("an arrow's arguments are the ones of the function round it",
           "(function(){ return (() => arguments[1])(); })(4, 5)", "5");
    expect("an arrow's body can be an object in brackets", "(() => ({ a: 1 }))().a", "1");
    expect("a template puts its values in",
           "(function(){ var n = 'w'; return `a${n}b${1 + 2}c`; })()", "awb3c");
    expect("and one template can sit inside another",
           "(function(){ var n = 'w'; return `x${`y${n}`}z`; })()", "xywz");
    expect("a tag gets the pieces as they were written and the values",
           "(function(){ function tag(s, ...v) { return s.raw.join('|') + ':' + v.join(','); }"
           " return tag`a\\n${1}b${2}`; })()", "a\\n|b|:1,2");
    expect("and the same pieces every time that place runs",
           "(function(){ var seen = []; function t(s) { seen.push(s); } for (var i = 0; i < 2; i++) t`x`;"
           " return seen[0] === seen[1]; })()", "true");
    expect("String.raw leaves escapes alone", "String.raw`a\\tb${1}`", "a\\tb1");
    expect("let inside a block is the block's own",
           "(function(){ let x = 1; { let x = 2; } return x; })()", "1");
    expect("each turn of a for loop has a let of its own",
           "(function(){ var fs = []; for (let i = 0; i < 3; i++) fs.push(() => i);"
           " return fs.map(f => f()).join(); })()", "0,1,2");
    expect("while var is one for the whole function",
           "(function(){ var fs = []; for (var i = 0; i < 3; i++) fs.push(() => i);"
           " return fs.map(f => f()).join(); })()", "3,3,3");
    expect("and for-of and for-in with const and let each have their own too",
           "(function(){ var fs = []; for (const k of [1, 2]) fs.push(() => k);"
           " for (let k in { a: 1 }) fs.push(() => k); return fs.map(f => f()).join(); })()", "1,2,a");
    expect("a const cannot be assigned to",
           "(function(){ const c = 1; try { c = 2; } catch (e) { return e.name + ':' + c; } })()",
           "TypeError:1");
    expect("a let cannot be used before its line",
           "(function(){ try { x; } catch (e) { return e.name; } let x = 1; })()", "ReferenceError");
    expect("a var is declared from the top of its function, so a closure finds it",
           "(function(){ function set() { v = 7; } set(); var v; return typeof globalThis.v + ':' + v; })()",
           "undefined:7");
    expect("a function declared in a block is there after the block",
           "(function(){ if (true) { function f() { return 3; } } return f(); })()", "3");
    expect("reading a name never declared is a ReferenceError",
           "(function(){ try { return neverWasDeclared; } catch (e) { return e.name; } })()",
           "ReferenceError");
    expect("undefined is a name that can be a parameter",
           "(function(window, undefined){ return typeof undefined; })(1)", "undefined");
    expect("a default fills a parameter that was not given",
           "(function(a, b = a + 1){ return a + b; })(1)", "3");
    expect("rest parameters collect what is left",
           "(function(a, ...r){ return r.length + ':' + r.join(); })(1, 2, 3)", "2:2,3");
    expect("spread hands an array over as the arguments", "Math.max(...[1, 5, 3])", "5");
    expect("spread in an array literal, of a string too",
           "[0, ...[1, 2], ...'ab'].join()", "0,1,2,a,b");
    expect("spread in an object literal",
           "JSON.stringify({ a: 1, ...{ b: 2 }, c: 3 })", "{\"a\":1,\"b\":2,\"c\":3}");
    expect("a call with a hundred and fifty arguments gets all of them",
           "(function(){ var a = []; for (var i = 0; i < 150; i++) a.push(i);"
           " return ((...r) => r.length + ':' + r[149])(...a) + ',' + Math.max.apply(null, a); })()",
           "150:149,149");
    expect("destructuring with defaults, rest and nesting",
           "(function(){ var { a, b: { c = 5 } = {}, ...more } = { a: 1, d: 4 };"
           " var [p, [q, r = 9] = [], ...t] = [1, [2], 3, 4];"
           " return [a, c, Object.keys(more), p, q, r, t.join('|')].join(); })()", "1,5,d,1,2,9,3|4");
    expect("a parameter can be a pattern, with a default of its own",
           "(function({ a = 1, b } = {}, [c] = [7]){ return a + (b || 0) + c; })()", "8");
    expect("destructuring assignment swaps two names",
           "(function(){ var x = 1, y = 2; [x, y] = [y, x]; return '' + x + y; })()", "21");
    expect("and puts values into properties",
           "(function(){ var o = {}; ({ a: o.x, b: o['y'] } = { a: 1, b: 2 }); return o.x + o.y; })()", "3");
    expect("shorthand properties, methods and computed keys",
           "(function(){ var a = 1, k = 'dyn'; var o = { a, [k + 1]: 2, m() { return 3; } };"
           " return o.a + o.dyn1 + o.m(); })()", "6");
    expect("a getter and a setter in an object literal",
           "(function(){ var o = { get g() { return 4; }, set s(v) { this.t = v * 2; } };"
           " o.s = 5; return o.g + o.t; })()", "14");
    expect("Object.defineProperty makes an accessor that keys leave out",
           "(function(){ var o = {}; Object.defineProperty(o, 'x', { get() { return 42; } });"
           " return o.x + ':' + Object.keys(o).length; })()", "42:0");
    expect("?. stops at null and undefined, and the rest of the chain with it",
           "(function(){ var n = null, d = { a: { b: 1 } };"
           " return [n?.x, n?.a.b.c, d?.a?.b, d?.['a'].b].join(); })()", ",,1,1");
    expect("?.() calls only a function that is there",
           "(function(){ var o = { f() { return 5; } }; return [o.g?.(), o.f?.()].join(); })()", ",5");
    expect("?? keeps nought and replaces only null and undefined",
           "[0 ?? 1, '' ?? 1, null ?? 2, undefined ?? 3].join()", "0,,2,3");
    expect("?\?=, ||= and &&= assign only when they have to",
           "(function(){ var a = null, b = 0, c = 1, d = 5; a ?\?= 1; b ||= 2; c &&= 3; d ?\?= 9;"
           " return [a, b, c, d].join(); })()", "1,2,3,5");
    expect("** raises, from the right", "2 ** 3 ** 2", "512");
    expect("numbers with underscores, 0b and 0o", "1_000 + 0b101 + 0o17", "1020");
    expect("a plain call's this is the global object, and undefined in strict code",
           "(function(){ function p() { return this; } function s() { 'use strict'; return this; }"
           " return (p() === globalThis) + ',' + s(); })()", "true,undefined");
    expect("a string's length and positions are UTF-16 units: an accent is one, an emoji two",
           "(function(){ var s = 'caf\\u00e9 \\u{1F600}!'; return [s.length, s.indexOf('!'), s.lastIndexOf(' '), s.charCodeAt(3).toString(16), s.charCodeAt(5).toString(16), s.charCodeAt(6).toString(16), s.codePointAt(5).toString(16), s.at(-1), s[3] === '\\u00e9'].join(); })()",
           "8,7,4,e9,d83d,de00,1f600,!,true");
    expect("slice, substring, substr, padStart and split count units, and halves join back into the character",
           "(function(){ var s = '\\u00e9t\\u00e9 \\u{1F600}x'; var h = s.slice(4, 5), l = s.slice(5, 6); return [s.slice(1, 3) === 't\\u00e9', s.substring(3, 0) === '\\u00e9t\\u00e9', s.substr(-1), h.length, l.length, (h + l) === '\\u{1F600}', (h + l).length, 'ab'.padStart(5, '\\u00e9').length, s.split('').length, [...s].length].join(); })()",
           "true,true,x,1,1,true,2,5,7,6");
    expect("a match's index and lastIndex are units, and a replacer is told the unit it is at",
           "(function(){ var s = '\\u00e9\\u00e9a\\u{1F600}b'; var r = /[ab]/g; var m1 = r.exec(s), i1 = r.lastIndex; var m2 = r.exec(s); var at = []; s.replace(/b/, function(m, o){ at.push(o); }); return [m1.index, i1, m2.index, r.lastIndex, s.search(/b/), at[0], s.match(/b/).index].join(); })()",
           "2,3,5,6,5,5,5");
    expect("walked backwards a string gives the units it gives forwards, without starting again at every step",
           "(function(){ var s = ''; for (var i = 0; i < 300; i++) s += ['a', '\\u00e9', '\\u{1F600}', '\\u20ac', '\\udc00x'][i % 5]; var f = [], b = []; for (var i = 0; i < s.length; i++) f.push(s.charCodeAt(i)); for (var i = s.length - 1; i >= 0; i--) b.unshift(s.charCodeAt(i)); var t0 = Date.now(), n = 0; var big = s; for (var k = 0; k < 6; k++) big += big; for (var i = big.length - 1; i >= 0; i--) n += big.charCodeAt(i) & 1; return [s.length, f.join() === b.join(), f[3], f[5], f[6], Date.now() - t0 < 4000].join(); })()",
           "420,true,56832,56320,120,true");
    expect("a segment's index is in units, and so is what containing is asked",
           "(function(){ var g = new Intl.Segmenter('en', { granularity: 'word' }).segment('caf\\u00e9 au lait'); var at = []; for (var x of g) at.push(x.index); return at.join() + ';' + g.containing(6).segment; })()",
           "0,4,5,7,8;au");
    expect("atob gives a character for each byte and btoa takes them back",
           "(function(){ var b = atob('6cOp/w=='); return [b.length, b.charCodeAt(0), b.charCodeAt(1), b.charCodeAt(3), btoa(b), btoa(String.fromCharCode(233, 255))].join(); })()",
           "4,233,195,255,6cOp/w==,6f8=");
    expect("a function's own prototype is written even when its parent's is read only",
           "(function(){ function P(s){ if (!(this instanceof P)) throw new TypeError('called'); this.s = s; } Object.defineProperty(P, 'prototype', { writable: false }); var ext = function (d, b) { Object.setPrototypeOf(d, b); function n() { this.constructor = d; } n.prototype = b.prototype; d.prototype = new n(); }; var C = (function (e) { function t(x) { return e.call(this, x) || this; } ext(t, e); t.prototype.m = function () { return 2; }; return t; })(P); var q = new C(1), a = () => 0; Object.setPrototypeOf(a, P); a.prototype = 5; return [q.s, q instanceof P, q.m(), C.prototype instanceof P, a.prototype === P.prototype].join(); })()",
           "1,true,2,true,true");
    expect("an escaped character past ASCII is that character, in a class and out of one",
           "(function(){ var b = String.fromCharCode(92); var cls = new RegExp('[' + b + '\\u00e9-' + b + '\\u00fc]', 'g'), one = new RegExp(b + '\\u20ac'); return [cls.test('\\u00f1'), 'a\\u00e9b\\u00fc'.replace(cls, '_'), one.test('5\\u20ac'), one.test('5e')].join(); })()",
           "true,a_b_,true,false");
    expect("\\u escapes are the characters they name", "'\\u00e9' === 'é' && '\\u{1F600}'.length === 2",
           "true");
    expect("eval sees the variables round it, and Function builds at the top level",
           "(function(){ var v = 4; return eval('v * 2') + Function('a', 'b', 'return a + b')(1, 2); })()",
           "11");
    expect("a pattern right after a block's closing brace",
           "(function(){ var r; try {} catch (e) {} /a+/.test('xaa') ? r = 'yes' : r = 'no'; return r; })()",
           "yes");
    script("with makes an object's properties names",
           "var o = { a: 1 }; with (o) { a = a + 1; } var result = o.a;", "2");
    {
        /* Nested past what the machine's stack can walk: refused with a
           message, where it used to run the program off the end of its
           stack. */
        ran++;
        static char deep[40010];
        int w = 0;
        for (int i = 0; i < 20000; i++) deep[w++] = '[';
        for (int i = 0; i < 20000; i++) deep[w++] = ']';
        deep[w] = 0;
        jctx J;
        js_init(&J);
        int ok = js_run(&J, deep, (u32)w);
        const char *want = "this script is nested too deeply to read";
        int said = !ok;
        for (int i = 0; said && want[i]; i++) if (J.error[i] != want[i]) said = 0;
        puts(said ? "  PASS  " : "  FAIL  ");
        puts("twenty thousand nested brackets are refused, not the end of the program");
        putc('\n');
        if (!said) failed++;
        js_done(&J);
    }
    /* --- classes ----------------------------------------------------------- */
    expect("a class with a constructor, a method and a static method",
           "(function(){ class P { constructor(x) { this.x = x; } twice() { return this.x * 2; }"
           " static make() { return new P(4); } } return P.make().twice(); })()", "8");
    expect("extends, super() and super.method()",
           "(function(){ class A { constructor(v) { this.v = v; } m() { return 'a' + this.v; } }"
           " class B extends A { constructor(v) { super(v + 1); } m() { return super.m() + 'b'; } }"
           " return new B(1).m(); })()", "a2b");
    expect("a static method is inherited, and super reaches the parent's",
           "(function(){ class A { static who() { return 'A'; } }"
           " class B extends A { static who() { return super.who() + 'B'; } } return B.who(); })()", "AB");
    expect("fields, on each instance and on the class",
           "(function(){ class C { a = 1; b = this.a + 1; static s = 5; }"
           " var c = new C(); return [c.a, c.b, C.s, Object.keys(c)].join(); })()", "1,2,5,a,b");
    expect("a subclass's fields come after super() returns",
           "(function(){ class A { x = 1; } class B extends A { y = this.x + 1; constructor() { super(); this.z = this.y + 1; } }"
           " var b = new B(); return [b.x, b.y, b.z].join(); })()", "1,2,3");
    expect("getters and setters in a class",
           "(function(){ class T { get v() { return this._v * 10; } set v(x) { this._v = x; } }"
           " var t = new T(); t.v = 4; return t.v; })()", "40");
    expect("#private fields and methods, seen only from inside, and #x in",
           "(function(){ class K { #n = 3; #dbl() { return this.#n * 2; } get() { return this.#dbl(); }"
           " static has(o) { return #n in o; } }"
           " var k = new K(); return [k.get(), K.has(k), K.has({}), Object.keys(k).length, k['#n']].join(); })()",
           "6,true,false,0,");
    expect("a static block runs once, with the class as this",
           "(function(){ class S { static n = 1; static { this.n += 1; } } return S.n; })()", "2");
    expect("new.target is the class new was given",
           "(function(){ class A { constructor() { this.t = new.target === B; } } class B extends A {}"
           " return new B().t; })()", "true");
    expect("instanceof walks the whole chain",
           "(function(){ class A {} class B extends A {} class C extends B {} var c = new C();"
           " return [c instanceof A, c instanceof B, new A() instanceof C].join(); })()", "true,true,false");
    expect("a class cannot be called without new",
           "(function(){ class A {} try { A(); return 'called'; } catch (e) { return e.name; } })()", "TypeError");
    expect("a class that extends Error is an Error with its message",
           "(function(){ class E extends Error { constructor(m) { super(m); this.name = 'E'; } }"
           " var e = new E('boom'); return [e instanceof Error, e instanceof E, e.message, String(e)].join(); })()",
           "true,true,boom,E: boom");
    expect("a class expression, and a class's name",
           "(function(){ var K = class { f() { return 1; } }; class N {} return new K().f() + N.name + K.name; })()",
           "1NK");
    expect("a derived constructor that never calls super is an error",
           "(function(){ class A {} class B extends A { constructor() {} }"
           " try { new B(); return 'made'; } catch (e) { return e.name; } })()", "ReferenceError");
    expect("an old-style constructor's prototype is shared by what it makes",
           "(function(){ function F() {} F.prototype.hi = function(){ return 'hi'; };"
           " var f = new F(); return f.hi() + (f.constructor === F) + (Object.getPrototypeOf(f) === F.prototype); })()",
           "hitruetrue");
    /* --- symbols, iteration and collections --------------------------------- */
    expect("a symbol is a key of its own, that keys and JSON leave out",
           "(function(){ var s = Symbol('d'), t = Symbol('d'); var o = { [s]: 1, a: 2 };"
           " return [o[s], o[t], Object.keys(o), JSON.stringify(o), typeof s, s.description, String(s)].join(); })()",
           "1,,a,{\"a\":2},symbol,d,Symbol(d)");
    expect("Symbol.for gives the same symbol for the same name",
           "Symbol.for('app') === Symbol.for('app') && Symbol.keyFor(Symbol.for('app')) === 'app'", "true");
    expect("for-of walks an array, a string, arguments, a Map and a Set",
           "(function(){ var r = []; for (var x of [1, 2]) r.push(x); for (var c of 'ab') r.push(c);"
           " (function(){ for (var a of arguments) r.push(a); })(3);"
           " for (var [k, v] of new Map([['m', 4]])) r.push(k + v); for (var s of new Set([5, 5])) r.push(s);"
           " return r.join(); })()", "1,2,a,b,3,m4,5");
    expect("and anything with a Symbol.iterator",
           "(function(){ var it = { [Symbol.iterator]() { var i = 0;"
           " return { next: () => ({ value: i, done: i++ >= 3 }) }; } }; return [...it].join(); })()", "0,1,2");
    expect("a loop left early tells the iterator",
           "(function(){ var closed = 0; var it = { [Symbol.iterator]() { return { next: () => ({ value: 1, done: false }),"
           " return() { closed++; return {}; } }; } }; for (var x of it) break; return closed; })()", "1");
    expect("a Map keeps any key in the order it was set, NaN included",
           "(function(){ var o = {}, m = new Map(); m.set(o, 'obj').set('1', 's').set(1, 'n').set(NaN, 'nan');"
           " m.set('1', 's2'); m.delete(1); return [m.get(o), m.get('1'), m.get(NaN), m.size, [...m.keys()].length].join(); })()",
           "obj,s2,nan,3,3");
    expect("a Set keeps each value once",
           "(function(){ var s = new Set([1, 2, 2, '2']); s.add(1); s.delete(2);"
           " return [...s].join() + ':' + s.size + ':' + s.has('2'); })()", "1,2:2:true");
    expect("WeakMap and WeakSet take objects",
           "(function(){ var k = {}, m = new WeakMap(), s = new WeakSet(); m.set(k, 1); s.add(k);"
           " try { m.set('x', 1); } catch (e) { return m.get(k) + ',' + s.has(k) + ',' + e.name; } })()",
           "1,true,TypeError");
    expect("Array.from and Array.of",
           "Array.from(new Set('aab')).join('') + Array.from({ length: 3 }, (_, i) => i * 2).join('') + Array.of(7).length",
           "ab0241");
    expect("includes, find, findIndex, findLast and findLastIndex",
           "[[NaN].includes(NaN), [1, 2, 3].find(x => x > 1), [1, 2, 3].findIndex(x => x > 1),"
           " [1, 2, 3].findLast(x => x < 3), [1, 2, 3].findLastIndex(x => x > 5)].join()", "true,2,1,2,-1");
    expect("flat, flatMap and fill",
           "[1, [2, [3, [4]]]].flat(2).length + ':' + [1, 2].flatMap(x => [x, x * 10]).join('') + new Array(3).fill(0).join('')",
           "4:110220000");
    expect("entries, keys and values of an array",
           "[...['a', 'b'].entries()].join('|') + ';' + [...['a', 'b'].keys()].join() + ';' + [...['a'].values()]",
           "0,a|1,b;0,1;a");
    expect("splice, unshift, some, every and reduce",
           "(function(){ var a = [1, 2, 3, 4], r = a.splice(1, 2, 9); a.unshift(0);"
           " return [a.join(''), r.join(''), a.some(x => x > 8), a.every(x => x > 0), a.reduce((s, x) => s + x)].join(); })()",
           "0194,23,true,false,14");
    expect("array methods borrowed for something that is only like an array",
           "Array.prototype.map.call({ length: 2, 0: 'a', 1: 'b' }, x => x.toUpperCase()).join('')", "AB");
    expect("sort is stable, and sorts more than a handful",
           "(function(){ var a = []; for (var i = 0; i < 50; i++) a.push({ k: i % 3, i: i });"
           " a.sort((x, y) => x.k - y.k); for (var i = 1; i < 50; i++) if (a[i - 1].k === a[i].k && a[i - 1].i > a[i].i) return 'unstable';"
           " return a[0].i + ',' + a[49].i; })()", "0,47");
    expect("Object.assign, entries, values and fromEntries",
           "JSON.stringify(Object.assign({}, { a: 1 }, null, { b: 2 })) + JSON.stringify(Object.entries({ c: 3 }))"
           " + Object.values({ d: 4 }) + JSON.stringify(Object.fromEntries([['e', 5]]))",
           "{\"a\":1,\"b\":2}[[\"c\",3]]4{\"e\":5}");
    expect("Object.freeze, and isFrozen",
           "(function(){ var o = Object.freeze({ a: 1 }); o.a = 2; o.b = 3; var a = Object.freeze([1]); a[0] = 5;"
           " return [o.a, o.b, a[0], Object.isFrozen(o)].join(); })()", "1,,1,true");
    expect("Object.create, getPrototypeOf and setPrototypeOf",
           "(function(){ var base = { hi: 1 }, o = Object.create(base, { own: { value: 2, enumerable: true } });"
           " var p = Object.setPrototypeOf({}, base);"
           " return [o.hi + o.own, Object.keys(o), Object.getPrototypeOf(o) === base, p.hi].join(); })()",
           "3,own,true,1");
    expect("getOwnPropertyNames and getOwnPropertyDescriptor",
           "(function(){ var o = { a: 1 }; Object.defineProperty(o, 'h', { value: 2 });"
           " var d = Object.getOwnPropertyDescriptor(o, 'h');"
           " return Object.getOwnPropertyNames(o).join() + ':' + [d.value, d.writable, d.enumerable].join(); })()",
           "a,h:2,false,false");
    expect("hasOwnProperty and in see the difference between own and inherited",
           "(function(){ var o = Object.create({ inh: 1 }); o.own = 2;"
           " return [o.hasOwnProperty('own'), o.hasOwnProperty('inh'), 'inh' in o, Object.hasOwn(o, 'own')].join(); })()",
           "true,false,true,true");
    expect("String methods: includes, startsWith, endsWith, repeat, matchAll, replaceAll",
           "['abc'.includes('b'), 'abc'.startsWith('ab'), 'abc'.endsWith('bc'), 'ab'.repeat(2),"
           " [...'a1b2'.matchAll(/\\d/g)].map(m => m[0] + m.index).join(''), 'a.b.c'.replaceAll('.', '-')].join()",
           "true,true,true,abab,1123,a-b-c");
    expect("Number.isInteger, isFinite and parseFloat, and toFixed rounding the exact value",
           "[Number.isInteger(5), Number.isInteger(5.5), Number.isFinite('5'), Number.parseFloat('1.5x'),"
           " (1.005).toFixed(2), (1234.5678).toFixed(1), (255).toString(16)].join()",
           "true,false,false,1.5,1.00,1234.6,ff");
    expect("globalThis is the global object", "(function(){ globalThis.gt = 3; return gt; })()", "3");
    expect("Object.prototype.toString names what a thing is",
           "[[], null, new Map(), function(){}, 1].map(x => Object.prototype.toString.call(x)).join()",
           "[object Array],[object Null],[object Map],[object Function],[object Number]");
    expect("a Date made from its parts, read back as ISO text and parsed again",
           "(function(){ var d = new Date(Date.UTC(2020, 1, 29, 12, 30, 15, 250));"
           " return d.toISOString() + ',' + (Date.parse(d.toISOString()) === d.getTime()) + ',' + d.getUTCDay(); })()",
           "2020-02-29T12:30:15.250Z,true,6");
    expect("Math has its functions, to the last place where it counts",
           "[Math.trunc(-4.7), Math.sign(-3), Math.hypot(3, 4), Math.cbrt(27), Math.log2(8), Math.exp(1) === Math.E,"
           " Math.log(Math.E), Math.atan2(1, 1) === Math.PI / 4, Math.round(-2.5), 2 ** 0.5 === Math.SQRT2].join()",
           "-4,-1,5,3,3,true,1,true,-2,true");
    expect("JSON.stringify indents, and JSON.parse revives",
           "JSON.stringify({ a: [1] }, null, 1) + JSON.parse('{\"b\":2}', (k, v) => typeof v === 'number' ? v * 10 : v).b",
           "{\n \"a\": [\n  1\n ]\n}20");
    /* --- generators, promises and async functions ---------------------------
     *
     * Each generator and each async function runs on a small stack of its
     * own and gives it back at every yield and await (jsco.h); the promise
     * jobs run once the script has finished, before js_run returns. */
    expect("a generator stops at each yield and is handed what next is given",
           "(function(){ function* g() { var x = yield 1; yield x * 2; return 3; } var it = g();"
           " var a = it.next(), b = it.next(5), c = it.next(), d = it.next();"
           " return [a.value, b.value, c.value, c.done, d.value, d.done].join(); })()", "1,10,3,true,,true");
    expect("yield* walks another generator, and a generator can be spread",
           "(function(){ function* i() { yield 'a'; yield 'b'; } function* o() { yield* i(); yield 'c'; }"
           " return [...o()].join(''); })()", "abc");
    expect("a generator's finally runs when a loop leaves it early",
           "(function(){ var log = []; function* g() { try { yield 1; yield 2; } finally { log.push('f'); } }"
           " for (var x of g()) break; return log.join(); })()", "f");
    expect("throw and return reach a generator where it stopped",
           "(function(){ function* g() { try { yield 1; } catch (e) { yield 'caught ' + e; } }"
           " var it = g(); it.next(); var t = it.throw('x').value; var r = it.return(7);"
           " return t + ',' + r.value + ',' + r.done; })()", "caught x,7,true");
    expect("a generator that never ends, stepped a thousand times",
           "(function(){ function* n() { var i = 0; while (true) yield i++; } var it = n(), s = 0;"
           " for (var k = 0; k < 1000; k++) s += it.next().value; return s; })()", "499500");
    expect("a generator method on a class, and one that calls deeply",
           "(function(){ class C { *each() { function depth(n) { return n ? depth(n - 1) + 1 : 0; }"
           " yield depth(300); } } return [...new C().each()][0]; })()", "300");
    script("promise jobs run after the script, in the order they were queued",
           "var order = []; Promise.resolve().then(() => order.push('a')).then(() => order.push('c'));"
           " Promise.resolve().then(() => order.push('b')); queueMicrotask(() => order.push('q'));"
           " order.push('sync'); var result; Promise.resolve().then(() => 0).then(() => 0).then(() => { result = order.join(''); });",
           "syncabqc");
    script("then, catch and finally, and a value passed through a handler that is not there",
           "var m = [], s = '', result; new Promise((ok) => ok(1)).then(v => { m.push(v); throw new Error('e'); })"
           " .then(() => m.push('skipped')).catch(e => { m.push(e.message); return 2; })"
           " .finally(() => m.push('f')).then(v => { m.push(v); result = m.join() + '|' + s; });"
           " Promise.reject(3).then(null).catch(v => { s += v; });", "1,e,f,2|3");
    script("Promise.all, allSettled, race and any",
           "var result = ''; Promise.all([1, Promise.resolve(2)]).then(v => result += v.join('') + ';');"
           " Promise.allSettled([1, Promise.reject(2)]).then(v => result += v.map(x => x.status[0]).join('') + ';');"
           " Promise.race([new Promise(() => {}), Promise.resolve('r')]).then(v => result += v + ';');"
           " Promise.any([Promise.reject(1), Promise.resolve('y')]).then(v => result += v + ';');",
           "12;fr;r;y;");
    script("an object with a then is followed like a promise",
           "var result; Promise.resolve({ then(ok) { ok('th'); } }).then(v => { result = v; });", "th");
    script("an async function awaits, returns and throws through its promise",
           "async function add(a, b) { await null; return a + b; }"
           " async function bad() { await 1; throw new Error('no'); }"
           " var result = ''; add(1, 2).then(v => result += v); bad().catch(e => result += e.message);",
           "3no");
    script("await in a loop and inside try, catch and finally",
           "async function f() { var s = ''; for (var i = 0; i < 3; i++) s += await Promise.resolve(i);"
           " try { await Promise.reject(new Error('x')); } catch (e) { s += e.message; } finally { s += 'f'; }"
           " return s; } var result; f().then(v => { result = v; });", "012xf");
    script("an async function runs up to its first await before the caller carries on",
           "var r = []; (async () => { r.push(1); await null; r.push(3); })(); r.push(2);"
           " var result; Promise.resolve().then(() => 0).then(() => { result = r.join(''); });", "123");
    script("async arrows and async methods, and await of a thenable",
           "class C { constructor() { this.v = 7; } async m() { return this.v + await { then(ok) { ok(1); } }; } }"
           " var result; (async x => (await x) * 2)(new C().m()).then(v => { result = v; });", "16");
    script("an async generator and for await",
           "async function* g() { yield 1; await null; yield Promise.resolve(2); }"
           " var result; (async () => { var r = []; for await (var x of g()) r.push(x);"
           " for await (var y of [Promise.resolve('p'), 'q']) r.push(y); result = r.join(); })();", "1,2,p,q");
    script("fifty async functions waiting at once, each finished",
           "var fs = [], waiting = []; for (var k = 0; k < 50; k++)"
           " fs.push((async (n) => { await new Promise(ok => waiting.push(ok)); return n; })(k));"
           " Promise.resolve().then(() => waiting.forEach((ok, i) => ok(i)));"
           " var result; Promise.all(fs).then(v => { result = v.reduce((a, b) => a + b, 0); });", "1225");
    {
        /* Jobs that queue themselves for ever: stopped by the page's limits,
           with the reason said, rather than the browser spinning. */
        ran++;
        jctx J;
        js_init(&J);
        J.time_cap = 50;
        const char *src = "var n = 0; function spin() { n++; Promise.resolve().then(spin); } spin();";
        int ok = js_run(&J, src, (u32)strlen(src));
        const char *want = "this script ";
        int said = !ok;
        for (int i = 0; said && want[i]; i++) if (J.error[i] != want[i]) said = 0;
        puts(said ? "  PASS  " : "  FAIL  ");
        puts("a promise loop that never ends is stopped rather than hanging");
        putc('\n');
        if (!said) failed++;
        js_done(&J);
    }
    /* --- patterns: lookaround, backreferences, names, flags ------------------ */
    expect("lookahead, and one that must not match",
           "'100em 200px'.match(/\\d+(?=px)/)[0] + ',' + '100px 200em'.match(/\\d+(?!px|\\d)/)[0]", "200,200");
    expect("lookbehind, and one that must not match",
           "'$10 20'.match(/(?<=\\$)\\d+/)[0] + ',' + '$10 20'.match(/(?<!\\$)\\b\\d+/)[0]", "10,20");
    expect("a backreference, and one written before its group",
           "[/(\\w)\\1/.test('hello'), /(\\w)\\1/.test('helo'), /(a)\\1/i.test('aA')].join()", "true,false,true");
    expect("named groups on the match, in \\k and in $<name>",
           "(function(){ var m = '2024-05-17'.match(/(?<y>\\d{4})-(?<m>\\d\\d)-(?<d>\\d\\d)/);"
           " return m.groups.m + ',' + /(?<q>[\"'])x\\k<q>/.test(\"'x'\") + ','"
           " + '2024-05-17'.replace(/(?<y>\\d{4})-(?<m>\\d\\d)-(?<d>\\d\\d)/, '$<d>/$<m>/$<y>'); })()",
           "05,true,17/05/2024");
    expect("a replacement function is handed the named groups last",
           "'a1'.replace(/(?<n>\\d)/, (m, p1, at, s, g) => g.n + at)", "a11");
    expect("s makes a dot match a newline, and y matches only at lastIndex",
           "(function(){ var y = /\\d/y; y.lastIndex = 1;"
           " return [/a.b/s.test('a\\nb'), /a.b/.test('a\\nb'), y.test('a1'), y.lastIndex, /\\d/y.test('a1')].join(); })()",
           "true,false,true,2,false");
    expect("u reads \\u{...} and knows \\p{L}",
           "[/\\u{61}/u.test('a'), /^\\p{L}+$/u.test('abc'), /^\\p{L}+$/u.test('a1')].join()", "true,true,false");
    expect("a star runs past five hundred characters",
           "/^.*$/.test('x'.repeat(5000)) + ',' + '1'.repeat(3000).match(/\\d+/)[0].length", "true,3000");
    expect("a class turned inside out matches a newline, and [] and [^]",
           "[/[^a]/.test('\\n'), /[]/.test('a'), /[^]/.test('\\n')].join()", "true,false,true");
    expect("more than nine groups all capture",
           "/(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)/.exec('abcdefghijkl')[12]", "l");
    expect("split on a pattern keeps its groups and stops at the limit",
           "'a1b2c'.split(/(\\d)/).join('|') + ';' + 'abc'.split(/(?:)/).join('|') + ';' + 'a,b,c'.split(/,/, 2).join('|')",
           "a|1|b|2|c;a|b|c;a|b");
    expect("flags, source and a pattern as text",
           "/a/gimsuy.flags + ' ' + /a\\/b/.source + ' ' + String(/x/g) + ' ' + new RegExp(/a/g).global", "gimsuy a\\/b /x/g true");
    /* --- bytes: ArrayBuffer, the typed arrays, DataView ---------------------- */
    expect("a byte array wraps what is put in it, and a clamped one rounds to even and clamps",
           "(function(){ var u = new Uint8Array(4); u[0] = 257; u[1] = -1; u[2] = 3.7;"
           " return [...u].join() + ';' + [...new Uint8ClampedArray([300, -5, 1.5, 2.5])].join(); })()",
           "1,255,3,0;255,0,2,2");
    expect("sixteen and thirty-two bit and float elements keep their own widths",
           "[new Int16Array([40000])[0], new Uint32Array([-1])[0], new Float32Array([0.1])[0],"
           " new Int8Array([200])[0], new Float64Array(2).byteLength].join()",
           "-25536,4294967295,0.10000000149011612,-56,16");
    expect("a data view reads and writes either byte order",
           "(function(){ var b = new ArrayBuffer(16), d = new DataView(b); d.setUint16(0, 0x1234);"
           " d.setUint32(4, 0xdeadbeef, true); d.setFloat64(8, Math.PI); var u = new Uint8Array(b);"
           " return [u[0], u[1], u[4], d.getUint32(4, true).toString(16), d.getInt8(7), u[8],"
           " d.getFloat64(8) === Math.PI].join(); })()",
           "18,52,239,deadbeef,-34,64,true");
    expect("views share their buffer, and slice copies it",
           "(function(){ var s = new Uint8Array([1, 2, 3, 4, 5]); s.subarray(1, 3)[0] = 9;"
           " s.slice(1, 3)[0] = 7; new Uint8Array(s.buffer, 3, 2)[0] = 6;"
           " var c = new Uint8Array(s.buffer.slice(3)); c[1] = 0;"
           " return [...s].join() + ';' + s.subarray(2).byteOffset + ';' + [...c].join(); })()",
           "1,9,3,6,5;2;6,0");
    expect("typed array methods keep their kind and sort as numbers",
           "(function(){ var s = new Uint8Array([1, 2, 3]); var m = s.map(x => x * 100);"
           " return [m instanceof Uint8Array, m[2], [...new Float64Array([10, 2, 1]).sort()].join(' '),"
           " Int8Array.of(1, 2, -3)[2], Uint16Array.from([1, 2]).length, s.join('-'), s.indexOf(3),"
           " s.filter(x => x > 1).length, Object.prototype.toString.call(s)].join(); })()",
           "true,44,1 2 10,-3,2,1-2-3,2,2,[object Uint8Array]");
    expect("set, for-of, keys and an element past the end",
           "(function(){ var a = new Int32Array(4); a.set([7, 8], 1); a[9] = 1; var t = 0;"
           " for (var x of a) t += x;"
           " return [t, Object.keys(a).join(''), a[9], a.length, ArrayBuffer.isView(a),"
           " ArrayBuffer.isView(a.buffer), Float64Array.BYTES_PER_ELEMENT].join(); })()",
           "15,0123,,4,true,false,8");
    expect("compound assignment, increments and delete through an index",
           "(function(){ var a = [1, 2, 3], o = {}, t = new Uint8Array(2), i = 1;"
           " a[i] += 10; a[i]++; ++a[0]; a[2] **= 2; o[5] = 1; o[5] += 1; o[i + 4]++;"
           " t[0] += 300; t[1]--; delete a[0];"
           " var f = Object.freeze([1, 2]); f[0] = 5; f[1] += 1; var g = [1]; g[3] = 4;"
           " return [a.join(), a.length, o[5], t[0], t[1], f.join(), g.length, g[3]].join(';'); })()",
           ",13,9;3;3;44;255;1,2;4;4");
    thrift("writing numbered keys over and over makes each key once",
           "var o = {}, result;",
           "for (var n = 0; n < 200000; n++) o[n % 1000] = n; result = o[999];",
           "199999", 1u << 20);
    thrift("counting up every element of long arrays makes nothing for each element",
           "var a = []; for (var i = 0; i < 100000; i++) a.push(i);"
           " var t = new Uint32Array(100000), result;",
           "for (var i = 0; i < a.length; i++) { a[i] += 1; a[i]++; t[i] = a[i] * 3; }"
           " var s = 0; for (var i = 0; i < t.length; i++) s += t[i]; result = a[99999] + ',' + s;",
           "100001,15000450000", 1u << 18);
    /* --- text: atob, btoa, TextEncoder, TextDecoder, self --------------------- */
    expect("atob and btoa, with and without padding, and what they refuse",
           "[btoa('hello'), btoa('a'), atob('aGVsbG8'), atob(' aGVs\\nbG8= '),"
           " (function(){ try { atob('a'); } catch (e) { return e.name; } })(),"
           " (function(){ try { btoa('\\u20ac'); } catch (e) { return e.name; } })()].join()",
           "aGVsbG8=,YQ==,hello,hello,InvalidCharacterError,InvalidCharacterError");
    expect("every byte through btoa and back as a string of bytes",
           "(function(){ var s = ''; for (var i = 0; i < 256; i++) s += String.fromCharCode(i);"
           " var b = btoa(s), d = atob(b); for (var i = 0; i < 256; i++) if (d.charCodeAt(i) !== i) return 'byte ' + i;"
           " return b.length + ',' + d.length + ',' + b.slice(-8) + ',' + btoa(atob('AAECA/7/gIE=')); })()",
           "344,256,/P3+/w==,AAECA/7/gIE=");
    expect("TextEncoder writes UTF-8, and encodeInto stops at a whole character",
           "(function(){ var te = new TextEncoder(), into = new Uint8Array(5), r = te.encodeInto('a\\u20ac\\u20ac', into);"
           " return [te.encoding, Array.from(te.encode('a\\u00e9\\u20ac\\ud83d\\ude00')).join(' '), r.read, r.written,"
           " into[4]].join(); })()",
           "utf-8,97 195 169 226 130 172 240 159 152 128,2,4,0");
    expect("TextDecoder replaces what is not UTF-8, drops a BOM and keeps half a character for the next call",
           "(function(){ var d = new TextDecoder(), s = new TextDecoder();"
           " return [d.decode(new Uint8Array([0x61, 0xff, 0x62, 0xe2, 0x82])) === 'a\\ufffdb\\ufffd',"
           " d.decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x61]).buffer),"
           " s.decode(new Uint8Array([0x61, 0xe2, 0x82]), { stream: true }) + '|' + s.decode(new Uint8Array([0xac])) === 'a|\\u20ac',"
           " d.decode(new DataView(new Uint8Array([0x61, 0x62, 0x63]).buffer, 1))].join(); })()",
           "true,a,true,bc");
    expect("other encodings, a fatal decoder and a label nobody knows",
           "(function(){ var r = [];"
           " try { new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xff])); } catch (e) { r.push(e.name); }"
           " try { new TextDecoder('klingon'); } catch (e) { r.push(e.name); }"
           " r.push(new TextDecoder('latin1').decode(new Uint8Array([0x41, 0xe9, 0x80])) === 'A\\u00e9\\u20ac');"
           " r.push(new TextDecoder('utf-16le').decode(new Uint8Array([0x41, 0, 0x3d, 0xd8, 0, 0xde])) === 'A\\ud83d\\ude00');"
           " r.push(new TextDecoder('ascii').encoding); return r.join(); })()",
           "TypeError,RangeError,true,true,windows-1252");
    expect("self is the global object",
           "[self === globalThis, self.Math === Math, typeof self].join()", "true,true,object");
    /* --- what core-js asks of an engine, and what its answers broke ----------- */
    expect("Reflect.set writes on the receiver, runs a setter with it, and says when it could not",
           "(function(){ var m = Object.defineProperty({}, 'a', { configurable: true });"
           " var r = [Reflect.set(Object.getPrototypeOf(m), 'a', 1, m)];"
           " var k = []; for (var x in {}) k.push(x); r.push(k.length);"
           " var o = {}, recv = {}; r.push(Reflect.set(o, 'x', 5, recv), o.x, recv.x);"
           " r.push(Reflect.set(Object.freeze({ z: 1 }), 'z', 2));"
           " var seen; r.push(Reflect.set({ set s(v) { seen = this; } }, 's', 1, recv) && seen === recv);"
           " r.push(Reflect.set({ get g() { return 1; } }, 'g', 2)); return r.join(); })()",
           "false,0,true,,5,false,true,false");
    expect("JSON.stringify writes a lone half of a pair as its escape, and a boxed symbol as an object",
           "[JSON.stringify('\\uDF06\\uD834'), JSON.stringify('\\uDEAD'),"
           " JSON.stringify('\\ud83d' + '\\ude00') === JSON.stringify('\\ud83d\\ude00'),"
           " JSON.stringify([Symbol()]) + JSON.stringify({ a: Symbol() }) + JSON.stringify(Object(Symbol()))].join(' ')",
           "\"\\udf06\\ud834\" \"\\udead\" true [null]{}{}");
    expect("a class holds code points past U+00FF, not their low bytes",
           "[/[\\uD800-\\uDBFF]/.test('['), /[\\u0080-\\uFFFF]/.test('\\u00e9'), /[\\u0080-\\uFFFF]/.test('a'),"
           " 'x\\u65e5\\u672cy'.match(/[\\u4e00-\\u9fff]+/)[0] === '\\u65e5\\u672c',"
           " '\\u00a0 \\u00e0 \\ufeff'.replace(/^[\\s\\uFEFF\\xA0]+|[\\s\\uFEFF\\xA0]+$/g, '') === '\\u00e0'].join()",
           "false,true,false,true,true");
    expect("a dot, a literal and a repeat take whole characters",
           "[/^.$/.test('\\u00e9'), 'a\\u00e9b'.match(/./g).length, /caf\\u00e9/.test('un caf\\u00e9'),"
           " '\\u00e9'.replace(/[^a-z]/g, '-'), '\\u00e9\\u00e9\\u00e9'.match(/^(.+)(.)$/)[2] === '\\u00e9',"
           " '\\u00e9\\u00e9'.match(/.+?/)[0] === '\\u00e9', /\\u00e9/i.test('\\u00c9')].join()",
           "true,3,true,-,true,true,true");
    expect("an empty match steps over a whole character",
           "['a\\u00e9'.split(/(?:)/).length, '\\u00e9'.replace(/(?:)/g, '-') === '-\\u00e9-',"
           " [...'a\\u00e9'.matchAll(/(?:)/g)].length, 'a\\u00e9'.match(/(?:)/g).length].join()",
           "2,true,3,3");
    expect("\\s knows the wide spaces, \\p{L} the scripts, and a byte from atob is itself",
           "[/\\s/.test('\\u2003'), /\\S/.test('\\u2003'), /\\W/.test('\\u00e9'), /\\p{L}+/u.exec('h\\u00e9llo w')[0] === 'h\\u00e9llo',"
           " /\\uD83D\\uDE00/.test('\\ud83d\\ude00'), /[\\x80-\\xff]/.test(atob('gA==')), /[\\x80-\\xbf]/.test('\\u00e9')].join()",
           "true,false,true,true,true,true,false");
    expect("DOMException is an Error with a name, a message and the old code, and atob throws one",
           "(function(){ var e = new DOMException('gone', 'NotFoundError'), r = [e.name, e.message, e.code,"
           " e instanceof Error, String(e), Object.prototype.toString.call(e), DOMException.DATA_CLONE_ERR,"
           " new DOMException().name]; try { atob('*'); } catch (x) { r.push(x instanceof DOMException, x.code); }"
           " return r.join(); })()",
           "NotFoundError,gone,8,true,NotFoundError: gone,[object DOMException],25,Error,true,5");
    expect("while two hundred nested brackets are an ordinary array",
           "(function(){ var s = ''; for (var i = 0; i < 200; i++) s += '['; s += '1';"
           " for (var i = 0; i < 200; i++) s += ']'; var a = eval(s);"
           " for (var i = 0; i < 199; i++) a = a[0]; return a[0]; })()", "1");

    /* \u escapes in names are their letters: GitHub's hotkey module is
       written with them, and "an escape in a name is not read here". */
    expect("a name written with escapes is the name",
           "(function(){ var \\u0061bc = 1, x\\u{62} = 2, o = { \\u0066oo: 3 }; return [abc, xb, o.foo].join(' '); })()",
           "1 2 3");

    /* A pattern of more characters than there were nodes for: a word list,
       as Al Jazeera's is. */
    expect("a pattern of fifteen hundred words is read and matched",
           "(function(){ var w = []; for (var i = 0; i < 1500; i++) w.push('w' + i);"
           " var re = new RegExp('^(?:' + w.join('|') + ')$');"
           " return [re.test('w1499'), re.test('w1500'), re.test('w7'), /^(a|b)+$/.test('abab')].join(' '); })()",
           "true false true true");

    /* Unicode's classes beyond letters and numbers: GitHub's code view
       stopped on "a \\p{...} class this does not know: Mn". */
    expect("\\p{...} knows marks, the kinds of punctuation and symbol, scripts and identifiers",
           "[/^\\p{Mn}$/u.test('\\u0301'), /^\\p{M}$/u.test('\\u0903'), /^\\P{M}$/u.test('a'), /^\\P{M}$/u.test('\\u0301'),"
           " /^[\\p{L}\\p{Mn}\\p{Nd}\\p{Pc}]+$/u.test('a\\u0301_9'), /^\\p{Script=Han}+$/u.test('\\u4e2d\\u6587'),"
           " /\\p{sc=Latin}/u.test('\\u4e2d'), /^\\p{ID_Start}\\p{ID_Continue}*$/u.test('x_1'), /^\\p{Lu}\\p{Ll}$/u.test('Ab'),"
           " /\\p{Sc}/u.test('$'), /\\p{Pd}/u.test('-'), /\\p{Ps}\\p{Pe}/u.test('()'), /\\p{Cc}/u.test('\\n'),"
           " /^\\p{Script=Cyrillic}+$/u.test('\\u0434\\u0430'), (function () { try { new RegExp('\\\\p{Nope}', 'u'); } catch (e) { return e.message.indexOf('Nope') > 0; } })()].join(' ')",
           "true true true false true true false true true true true true true true true");

    /* BigInt, which GitHub's protocol buffers and hashes and Spotify's player
       stopped at. Each answer is what the language defines. */
    expect("BigInt literals and BigInt() make whole numbers of any size, and typeof says bigint",
           "[typeof 10n, 10n + 5n, 2n ** 100n, BigInt('123456789012345678901234567890') * 10n,"
           " BigInt(Number.MAX_SAFE_INTEGER) + 2n, 0x1fn, 0b101n, 0o17n, 1_000n, BigInt(' -42 '), BigInt(true),"
           " String(-0n), (255n).toString(16), (-255n).toString(2), BigInt('0x10'), BigInt(''), typeof Object(1n)].join(' ')",
           "bigint 15 1267650600228229401496703205376 1234567890123456789012345678900 9007199254740993 31 5 15 1000 -42 1 0 ff -11111111 16 0 object");
    expect("and divides, takes remainders and shifts as the language defines them, negatives included",
           "[7n / 2n, -7n / 2n, 7n % -2n, -7n % 2n, 1n << 70n, -5n >> 1n, 5n >> 1n, -1n >> 100n, 5n & -2n, -6n | 3n,"
           " 5n ^ -1n, ~5n, -(-3n), BigInt.asUintN(64, -1n), BigInt.asIntN(8, 255n), BigInt.asIntN(64, 2n ** 63n),"
           " 10n ** 30n / 10n ** 28n, (2n ** 200n + 5n) % (2n ** 100n + 1n),"
           /* The division that has to add back a step it took too far,
              which long division comes to about twice in 2^32 digits. */
           " 0x7fffffff800000000000000000000000n / 0x800000000000000000000001n,"
           " (0x7fffffff800000000000000000000000n % 0x800000000000000000000001n).toString(16)].join(' ')",
           "3 -3 1 -1 1180591620717411303424 -3 2 -1 4 -5 -6 -6 3 18446744073709551615 -1 -9223372036854775808 100 6 "
           "4294967294 7fffffffffffffff00000002");
    expect("and compares exactly with numbers and strings, and strictly equals only a BigInt of its value",
           "[1n == 1, 1n === 1, 2n > 1, 1n < 1.5, 2n ** 64n > 1e19, 10n == '10', 10n < '11', 0n == false, 1n == true,"
           " [1n, 2n].includes(2n), new Set([1n, 1n, 2n]).size, new Map([[5n, 'x']]).get(5n), Object.is(0n, -0n), !!0n, !!1n,"
           " 3n > 2n && -3n < -2n, 1n < NaN, 2n ** 53n + 1n > 2 ** 53, 5n === 5n, 'x' < 1n].join(' ')",
           "true false true true true true true true true true 2 x true false true true false true true false");
    expect("and refuses to be mixed with numbers, and is written as JSON only through a toJSON",
           "(function(){ var r = [], t = function (f) { try { f(); r.push('none'); } catch (e) { r.push(e.name); } };"
           " t(function () { return 1n + 1; }); t(function () { return +1n; }); t(function () { return Math.abs(1n); });"
           " t(function () { return 1n >>> 0n; }); t(function () { return BigInt(1.5); }); t(function () { return BigInt('1.5'); });"
           " t(function () { return new BigInt(1); }); t(function () { return JSON.stringify({ a: 1n }); }); t(function () { return 1n / 0n; });"
           " r.push('' + 5n, `${6n}`, Number(2n ** 64n), parseInt('7n'), 2n + '3');"
           " BigInt.prototype.toJSON = function () { return this.toString(); }; r.push(JSON.stringify({ a: 1n }));"
           " return r.join(' '); })()",
           "TypeError TypeError TypeError TypeError RangeError SyntaxError TypeError TypeError RangeError 5 6 18446744073709552000 7 23 {\"a\":\"1\"}");
    expect("and counts, hashes and is written by Intl from its own digits",
           "(function(){ var x = 5n; x++; x += 10n; x **= 2n; x--;"
           " var h = 14695981039346656037n, p = 1099511628211n;"
           " for (var c of 'hello') { h ^= BigInt(c.charCodeAt(0)); h = BigInt.asUintN(64, h * p); }"
           " return [x, typeof x, h.toString(16), new Intl.NumberFormat().format(12345678901234567890n), (1234n).toLocaleString(),"
           " new Intl.NumberFormat('en', { style: 'currency', currency: 'USD' }).format(10n ** 20n),"
           " new Intl.NumberFormat('en', { notation: 'compact' }).format(1500000n), new Intl.NumberFormat('en', { style: 'percent' }).format(5n)].join(' '); })()",
           "255 bigint a430d84680aabd0b 12,345,678,901,234,567,890 1,234 $100,000,000,000,000,000,000.00 1.5M 500%");
    expect("and DataView reads and writes 64-bit BigInts, either end first",
           "(function(){ var v = new DataView(new ArrayBuffer(16)); v.setBigInt64(0, -2n); v.setBigUint64(8, 2n ** 64n - 1n, true);"
           " var w = new DataView(new ArrayBuffer(8)); w.setBigUint64(0, 0x0102030405060708n, true);"
           " return [v.getBigInt64(0), v.getBigUint64(0), v.getBigUint64(8, true), v.getInt32(4), v.getUint8(15), w.getUint8(0),"
           " w.getBigUint64(0).toString(16), typeof v.getBigInt64].join(' '); })()",
           "-2 18446744073709551614 18446744073709551615 -2 255 8 807060504030201 function");

    /* structuredClone, which libraries copy state with, and Mozilla's consent
       manager takes off the window before it does anything. */
    expect("structuredClone copies deeply, keeping kinds, shared parts and cycles",
           "(function(){ var shared = { n: 1 }; var src = { a: [1, 2, { b: 'x' }], d: new Date(5), r: /x/gi,"
           " m: new Map([[1, shared]]), s: new Set([shared]), e: new TypeError('bad'), big: 10n, u8: new Uint8Array([1, 2, 3]),"
           " one: shared, two: shared };"
           " src.self = src; var c = structuredClone(src);"
           " return [c !== src, c.a[2].b, c.a[2] !== src.a[2], c.d instanceof Date, c.d.getTime(), c.r.source + c.r.flags,"
           " c.m.get(1).n, c.m.get(1) === c.one, c.s.has(c.one), c.one === c.two, c.one !== shared, c.self === c,"
           " c.e instanceof TypeError, c.e.message, typeof c.big, c.u8[2], c.u8.buffer !== src.u8.buffer].join(' '); })()",
           "true x true true 5 xgi 1 true true true true true true bad bigint 3 true");
    expect("and refuses what cannot be copied",
           "(function(){ var r = [], t = function (v) { try { structuredClone(v); r.push('none'); } catch (e) { r.push(e.name); } };"
           " t(function () {}); t(Symbol('x')); t({ f: function () {} }); t(new WeakMap()); t(Promise.resolve(1));"
           " t(new Proxy({}, {})); t({ ok: [1, 'a', null, undefined, true] }); return r.join(' '); })()",
           "DataCloneError DataCloneError DataCloneError DataCloneError DataCloneError DataCloneError none");

    /* A function's prototype is made when first asked for. Babel makes every
       class's prototype read-only with a descriptor that has no value in it,
       and a class with no methods was left with none, so a class extending it
       failed in Object.create (Stripe's script, on NPR). */
    expect("a function's prototype is there to defineProperty, delete, freeze and getOwnPropertyNames",
           "(function(){ function F() {} Object.defineProperty(F, 'prototype', { writable: false });"
           " function Sub() {} Sub.prototype = Object.create(F.prototype);"
           " function N() {} function D() {} function Z() {} Object.freeze(Z);"
           " return [typeof F.prototype, F.prototype.constructor === F, Object.getOwnPropertyDescriptor(F, 'prototype').writable,"
           " new Sub() instanceof F, Object.getOwnPropertyNames(N).sort().join(','), delete D.prototype, typeof D.prototype,"
           " Object.getOwnPropertyDescriptor(Z, 'prototype').writable].join(' '); })()",
           "object true false true length,name,prototype false object false");
    /* A function gives back its own text: MapLibre makes its worker out of
       its functions' text, which came back as "{ ... }". */
    expect("a function's toString is its text, whatever kind of function it is",
           "(function(){ function plain(a, b) { return a + b; } async  function af(x) { await x; }"
           " var arrow = (a, b) => a * b, one = x => x + 1;"
           " var o = { m(a) { return a; }, get g() { return 1; }, async am() {}, *gm() {} };"
           " class A { constructor(v) { this.v = v; } static s() { return 2; } }"
           " return [plain, af, arrow, one, o.m, Object.getOwnPropertyDescriptor(o, 'g').get, o.am, o.gm, A, A.s,"
           " new Function('a', 'return a')].map(String).join(' # ').replace(/\\n/g, '|')"
           " + ' # ' + eval('(' + plain + ')')(2, 3); })()",
           "function plain(a, b) { return a + b; } # async  function af(x) { await x; } # (a, b) => a * b # x => x + 1 # m(a) { return a; } # "
           "get g() { return 1; } # async am() {} # *gm() {} # class A { constructor(v) { this.v = v; } static s() { return 2; } } # "
           "s() { return 2; } # function anonymous(a|) {|return a|} # 5");

    /* export is only a statement at a module's top level: a method may be
       called it, and Next.js's bloom filter has one, which stopped the BBC's
       and The Verge's pages ("this engine does not have export"). */
    expect("a method, a property and a key may be called export",
           "(function(){ class B { export() { return 'x'; } } var o = { export() { return 'y'; } };"
           " o.export2 = { 'export': 'z' }; return new B().export() + o.export() + o.export.name + o.export2.export; })()",
           "xyexportz");

    /* A function expression's own name, which GSAP's recursion is written
       with: "u is not defined". */
    expect("a function expression written with a name knows itself by it, and only inside",
           "(function(){ var f = function u(t) { return t > 0 ? u(t - 1) : 'done'; };"
           " var g = function v(v) { return typeof v; }, h = function w() { var w = 2; return w; };"
           " return [f(3), typeof u, g(1), h(), (function r() { return typeof r; })()].join(' '); })()",
           "done undefined number 2 function");

    /* Intl, which GitHub and Spotify stopped at: English as the United States
       writes it, numbers rounded as the browsers round them, and times in
       the zones it knows. Each answer is what Chrome gives. */
    expect("Intl.NumberFormat writes decimals, currencies, percents and units",
           "(function(){ var f = function (o, x) { return new Intl.NumberFormat('en-US', o).format(x); };"
           " return [f({}, 1234567.891), f({ style: 'currency', currency: 'USD' }, -1234.5),"
           " encodeURIComponent(f({ style: 'currency', currency: 'EUR' }, 9.995)), encodeURIComponent(f({ style: 'currency', currency: 'JPY' }, 12345.6)),"
           " f({ style: 'percent' }, 0.256), f({ maximumSignificantDigits: 3 }, 123456), f({ minimumFractionDigits: 2 }, 5),"
           " (1.005).toLocaleString('en', { maximumFractionDigits: 2 }), (-0).toLocaleString(), f({ signDisplay: 'always' }, 3),"
           " f({ signDisplay: 'exceptZero' }, 0), f({ style: 'currency', currency: 'USD', currencySign: 'accounting' }, -5),"
           " f({ style: 'currency', currency: 'USD', currencyDisplay: 'name' }, 1), f({ style: 'currency', currency: 'USD', currencyDisplay: 'name' }, 2),"
           " f({ style: 'currency', currency: 'CHF' }, 10).replace(/\\u00a0/, '_'), f({ style: 'unit', unit: 'kilometer-per-hour' }, 50),"
           " f({ style: 'unit', unit: 'megabyte', unitDisplay: 'long' }, 1), f({ style: 'unit', unit: 'second', unitDisplay: 'narrow' }, 5),"
           " f({ useGrouping: false }, 12345), f({ minimumIntegerDigits: 3 }, 7), f({}, NaN)].join('|'); })()",
           "1,234,567.891|-$1,234.50|%E2%82%AC10.00|%C2%A512%2C346|26%|123,000|5.00|1.01|-0|+3|0|($5.00)|1.00 US dollars|2.00 US dollars|"
           "CHF_10.00|50 km/h|1 megabyte|5s|12345|007|NaN");
    expect("and compact, scientific and engineering notation, with its parts and what it resolved",
           "(function(){ var c = new Intl.NumberFormat('en', { notation: 'compact' });"
           " var l = new Intl.NumberFormat('en', { notation: 'compact', compactDisplay: 'long' });"
           " var r = new Intl.NumberFormat('de-DE', { style: 'currency', currency: 'EUR' }).resolvedOptions();"
           " return [[999, 1234, 12345, 123456, 1500000, 999999, 2e9].map(c.format).join(' '), l.format(2500000000),"
           " new Intl.NumberFormat('en', { notation: 'scientific' }).format(123456), new Intl.NumberFormat('en', { notation: 'engineering' }).format(123456),"
           " new Intl.NumberFormat().formatToParts(-1234.5).map(function (p) { return p.type + ':' + p.value; }).join(','),"
           " r.locale, r.currency, r.minimumFractionDigits, r.maximumFractionDigits, r.style, r.useGrouping,"
           " [1, 2, 3000].map(new Intl.NumberFormat().format).join(';'), new Intl.NumberFormat().formatRange(3, 5)].join('|'); })()",
           "999 1.2K 12K 123K 1.5M 1M 2B|2.5 billion|1.235E5|123.456E3|minusSign:-,integer:1,group:,,integer:234,decimal:.,fraction:5|"
           "en-US|EUR|2|2|currency|auto|1;2;3,000|3\xE2\x80\x93" "5");
    expect("and refuses what it cannot do, as the standard says",
           "(function(){ var r = []; var t = function (f) { try { f(); r.push('none'); } catch (e) { r.push(e.name); } };"
           " t(function () { new Intl.NumberFormat('en', { style: 'currency' }); });"
           " t(function () { new Intl.NumberFormat('not a tag!'); });"
           " t(function () { new Intl.NumberFormat('en', { style: 'money' }); });"
           " t(function () { new Intl.NumberFormat('en', { minimumFractionDigits: 3, maximumFractionDigits: 1 }); });"
           " t(function () { Intl.NumberFormat.prototype.format.call({}, 1); });"
           " return r.join(' '); })()",
           "TypeError RangeError RangeError RangeError TypeError");
    expect("Intl.DateTimeFormat writes a date and a time as en-US does, in its styles and parts",
           "(function(){ var d = new Date(Date.UTC(2026, 8, 30, 19, 5, 3, 45));"
           " var f = function (o) { return new Intl.DateTimeFormat('en-US', o).format(d); };"
           " return [f(), d.toLocaleDateString(), d.toLocaleTimeString(), d.toLocaleString(),"
           " ['full', 'long', 'medium', 'short'].map(function (s) { return f({ dateStyle: s, timeStyle: s }); }).join(' / '),"
           " f({ month: 'long', day: 'numeric', year: 'numeric' }), f({ weekday: 'short', month: 'short', day: 'numeric' }),"
           " f({ hour: 'numeric', minute: '2-digit' }), f({ hour: '2-digit', minute: '2-digit', hour12: false }),"
           " f({ month: 'long', year: 'numeric' }), f({ hour: 'numeric', minute: '2-digit', second: '2-digit', fractionalSecondDigits: 2 }),"
           " d.toLocaleDateString('en-US', { weekday: 'long' }), f({ year: '2-digit', month: '2-digit', day: '2-digit' })].join(' | '); })()",
           "9/30/2026 | 9/30/2026 | 7:05:03 PM | 9/30/2026, 7:05:03 PM | "
           "Wednesday, September 30, 2026 at 7:05:03 PM Coordinated Universal Time / September 30, 2026 at 7:05:03 PM UTC / "
           "Sep 30, 2026, 7:05:03 PM / 9/30/26, 7:05 PM | September 30, 2026 | Wed, Sep 30 | 7:05 PM | 19:05 | September 2026 | "
           "7:05:03.04 PM | Wednesday | 09/30/26");
    expect("and in the time zones it knows, summer and winter",
           "(function(){ var s = new Date(Date.UTC(2026, 8, 30, 19, 5)), w = new Date(Date.UTC(2026, 0, 15, 19, 5));"
           " var f = function (z, d, o) { o = o || { hour: 'numeric', minute: 'numeric', timeZoneName: 'short' }; o.timeZone = z;"
           " return new Intl.DateTimeFormat('en-US', o).format(d); };"
           " return [f('America/New_York', s), f('America/New_York', w), f('america/los_angeles', w), f('Europe/Paris', s), f('Europe/London', w),"
           " f('Europe/London', s), f('Australia/Sydney', s), f('Australia/Sydney', w), f('Asia/Kolkata', s), f('Pacific/Auckland', w),"
           " f('UTC', s), f('Europe/Paris', s, { timeStyle: 'full' }), new Intl.DateTimeFormat().resolvedOptions().timeZone,"
           " new Intl.DateTimeFormat('en', { timeZone: 'Mars/Olympus' }).resolvedOptions().timeZone,"
           " (function () { try { new Intl.DateTimeFormat('en', { timeZone: 'Nowhere' }); return 'none'; } catch (e) { return e.name; } })()].join(' | '); })()",
           "3:05 PM EDT | 2:05 PM EST | 11:05 AM PST | 9:05 PM GMT+2 | 7:05 PM GMT | 8:05 PM GMT+1 | 5:05 AM GMT+10 | "
           "6:05 AM GMT+11 | 12:35 AM GMT+5:30 | 8:05 AM GMT+13 | 7:05 PM UTC | 9:05:00 PM Central European Summer Time | UTC | UTC | RangeError");
    expect("and its parts, ranges and resolved options",
           "(function(){ var d = new Date(Date.UTC(2026, 8, 30, 19, 5)), e = new Date(Date.UTC(2026, 9, 2));"
           " var f = new Intl.DateTimeFormat('en', { year: 'numeric', month: 'short', day: 'numeric' }), r = f.resolvedOptions();"
           " return [f.formatToParts(d).map(function (p) { return p.type + ':' + p.value; }).join(','),"
           " encodeURIComponent(f.formatRange(d, e)), f.formatRange(d, d), r.locale, r.calendar, r.timeZone, r.month, r.year,"
           " [d, e].map(f.format).join(';'), Object.prototype.toString.call(f)].join(' | '); })()",
           "month:Sep,literal: ,day:30,literal:, ,year:2026 | Sep%2030%2C%202026%E2%80%89%E2%80%93%E2%80%89Oct%202%2C%202026 | "
           "Sep 30, 2026 | en-US | gregory | UTC | short | numeric | Sep 30, 2026;Oct 2, 2026 | [object Intl.DateTimeFormat]");
    expect("Intl.PluralRules and Intl.RelativeTimeFormat speak English",
           "(function(){ var p = new Intl.PluralRules('en'), o = new Intl.PluralRules('en', { type: 'ordinal' });"
           " var r = new Intl.RelativeTimeFormat('en'), a = new Intl.RelativeTimeFormat('en', { numeric: 'auto' });"
           " return [[0, 1, 1.5, 2].map(function (n) { return p.select(n); }).join(','), new Intl.PluralRules('en', { minimumFractionDigits: 1 }).select(1),"
           " [1, 2, 3, 4, 11, 12, 13, 21, 22, 23, 101, 111].map(function (n) { return o.select(n); }).join(','),"
           " o.resolvedOptions().pluralCategories.join(','), r.format(3, 'day'), r.format(-1, 'day'), r.format(-2, 'hours'), r.format(1000, 'year'),"
           " a.format(-1, 'day'), a.format(1, 'day'), a.format(0, 'day'), a.format(-1, 'year'), a.format(0, 'second'), a.format(1, 'month'),"
           " new Intl.RelativeTimeFormat('en', { style: 'short' }).format(-5, 'minute'), new Intl.RelativeTimeFormat('en', { style: 'narrow' }).format(3, 'day'),"
           " r.formatToParts(-2, 'week').map(function (x) { return x.type + ':' + x.value + (x.unit ? '/' + x.unit : ''); }).join(',')].join(' | '); })()",
           "other,one,other,other | other | one,two,few,other,other,other,other,one,two,few,one,other | few,one,two,other | "
           "in 3 days | 1 day ago | 2 hours ago | in 1,000 years | yesterday | tomorrow | today | last year | now | next month | "
           "5 min. ago | in 3d | integer:2/week,literal: weeks ago");
    expect("Intl.Collator sorts as a reader expects: letters, then accents, then case, lower first",
           "(function(){ var c = new Intl.Collator();"
           " return [encodeURIComponent(['b', 'A', 'a', 'B', '\\u00e9', 'e', 'E', '\\u00c9', 'z', '\\u00e6', 'ae', '10', '9', '_x'].sort(c.compare).join(' ')),"
           " ['item10', 'item9', 'item1'].sort(new Intl.Collator('en', { numeric: true }).compare).join(' '),"
           " 'a'.localeCompare('A'), 'a'.localeCompare('b'), 'r\\u00e9sum\\u00e9'.localeCompare('resume'),"
           " 'r\\u00e9sum\\u00e9'.localeCompare('Resume', undefined, { sensitivity: 'base' }),"
           " 'r\\u00e9sum\\u00e9'.localeCompare('R\\u00e9sum\\u00e9', 'en', { sensitivity: 'accent' }),"
           " 'e\\u0301'.localeCompare('\\u00e9', 'en', { sensitivity: 'base' }), '\\u00df'.localeCompare('ss', 'de', { sensitivity: 'base' }),"
           " ['a', 'A'].sort(new Intl.Collator('en', { caseFirst: 'upper' }).compare).join(''),"
           " new Intl.Collator('en', { ignorePunctuation: true }).compare('a-b', 'ab'), new Intl.Collator().resolvedOptions().sensitivity].join(' | '); })()",
           "_x%2010%209%20a%20A%20ae%20%C3%A6%20b%20B%20e%20E%20%C3%A9%20%C3%89%20z | item1 item9 item10 | -1 | -1 | 1 | 0 | 0 | 0 | 0 | Aa | 0 | variant");
    expect("Intl.ListFormat joins with and, or and commas as English does",
           "(function(){ var f = function (o, l) { return new Intl.ListFormat('en', o).format(l); };"
           " return [f({}, ['a', 'b', 'c']), f({}, ['a', 'b']), f({}, ['a']), f({}, []), f({ type: 'disjunction' }, ['a', 'b', 'c']),"
           " f({ style: 'short' }, ['a', 'b', 'c']), f({ style: 'narrow' }, ['a', 'b', 'c']), f({ type: 'unit' }, ['a', 'b']),"
           " f({ type: 'unit', style: 'narrow' }, ['a', 'b', 'c']),"
           " new Intl.ListFormat('en').formatToParts(['x', 'y']).map(function (p) { return p.type + ':' + p.value; }).join(','),"
           " (function () { try { f({}, [1]); return 'none'; } catch (e) { return e.name; } })()].join(' | '); })()",
           "a, b, and c | a and b | a |  | a, b, or c | a, b, & c | a, b, c | a, b | a b c | element:x,literal: and ,element:y | TypeError");
    expect("Intl.Segmenter finds characters, words and sentences",
           "(function(){ var g = new Intl.Segmenter(), w = new Intl.Segmenter('en', { granularity: 'word' });"
           " var s = new Intl.Segmenter('en', { granularity: 'sentence' });"
           " var words = Array.from(w.segment(\"Hello, world! It's 3.14 now.\"));"
           " return [Array.from(g.segment('ae\\u0301b')).length, Array.from(g.segment('\\ud83d\\udc4d\\ud83c\\udffd!')).length,"
           " words.filter(function (x) { return x.isWordLike; }).map(function (x) { return x.segment; }).join('/'), words.length,"
           " Array.from(s.segment('One. Two! Three')).map(function (x) { return x.segment; }).join('|'),"
           " w.segment('ab cd').containing(3).segment, w.segment('ab cd').containing(3).index].join(' | '); })()",
           "3 | 2 | Hello/world/It's/3.14/now | 12 | One. |Two! |Three | cd | 3");
    expect("and Intl says it has one locale, names things in English, and lists what it knows",
           "(function(){ var l = new Intl.Locale('EN-latn-us');"
           " return [l.toString(), l.language, l.script, l.region, new Intl.Locale('fr').maximize().toString(),"
           " Intl.getCanonicalLocales(['EN-us', 'zh-hant-TW']).join(','), Intl.DateTimeFormat.supportedLocalesOf(['fr', 'en-GB', 'de']).join(','),"
           " new Intl.NumberFormat('fr-FR').resolvedOptions().locale, new Intl.DisplayNames(['en'], { type: 'region' }).of('US'),"
           " new Intl.DisplayNames(['en'], { type: 'language' }).of('en-GB'), new Intl.DisplayNames(['en'], { type: 'currency' }).of('EUR'),"
           " new Intl.DisplayNames(['en'], { type: 'language' }).of('de-AT'), Intl.supportedValuesOf('calendar').join(','),"
           " Intl.supportedValuesOf('timeZone').indexOf('Europe/Paris') >= 0, Object.prototype.toString.call(Intl)].join(' | '); })()",
           "en-Latn-US | en | Latn | US | fr-Latn-FR | en-US,zh-Hant-TW | en-GB | en-US | United States | British English | Euro | "
           "German (Austria) | gregory | true | [object Intl]");

    /* Which value it was that had nothing to read, when that is a name or a
       chain of them: a bundle on one line said only "at line 1". */
    expect("reading from undefined says which value it was",
           "(function(){ var out = []; var e = {};"
           " try { e.addedNodes.forEach(1); } catch (x) { out.push(x.message); }"
           " try { var n = null; n.x; } catch (x) { out.push(x.message); }"
           " try { [][0].y; } catch (x) { out.push(x.message); }"
           " try { var o = { f: function() {} }; o.f(2).z; } catch (x) { out.push(x.message); }"
           " return out.join(' | '); })()",
           "cannot read forEach of undefined (e.addedNodes) | cannot read x of null (n) | cannot read y of undefined"
           " | cannot read z of undefined (o.f(...))");

    /* The constructors of async functions and generators, reached as the
       constructor of one: they were Function, so an "async" function made
       that way -- as Alpine makes every expression in a page's attributes --
       gave back no promise. */
    expect("an async function's constructor makes async functions, and a generator's generators",
           "(function(){ var AF = Object.getPrototypeOf(async function(){}).constructor;"
           " var GF = Object.getPrototypeOf(function*(){}).constructor;"
           " var f = new AF('a', 'return a * 2'), g = new GF('yield 1; yield 2');"
           " return [AF === Function, AF.name, f(21) instanceof Promise, f instanceof AF, [...g()].join()].join(' '); })()",
           "false AsyncFunction true true 1,2");

    /* A with statement's names are what its object has, inherited or through
       a proxy, and a function found there is called on the object: Alpine
       evaluates every expression in a page's attributes that way, in a
       proxy over its components' data. */
    expect("with finds names an object inherits and a proxy says it has, and calls on the object",
           "(function(){ var base = { hi: function() { return 'hi ' + this.who; } };"
           " var o = Object.create(base); o.who = 'o';"
           " var seen = [], p = new Proxy({ n: 1 }, { has: function(t, k) { seen.push(k); return k == 'n' || k == 'm'; },"
           " get: function(t, k) { return k == 'm' ? 'made' : t[k]; } });"
           " var out = []; with (o) { out.push(hi()); } with (p) { out.push(n + 1, m); n = 5; }"
           " return out.join(' ') + ' ' + p.n + ' ' + (seen.indexOf('n') >= 0); })()",
           "hi o 2 made 5 true");

    /* Proxy, which was not there: Ars Technica's and Yahoo's scripts
       stopped at "Proxy is not defined". Each trap, and each thing done
       to the target when there is none. */
    expect("a proxy's get trap answers every read",
           "(function(){ var p = new Proxy({}, { get: function(t, k) { return 'got ' + String(k); } });"
           " return p.x + ', ' + p[1]; })()", "got x, got 1");
    expect("with no traps a proxy reads, writes, asks and deletes on its target",
           "(function(){ var o = { a: 1 }, p = new Proxy(o, {}); p.b = 2;"
           " return [p.a, o.b, 'a' in p, delete p.a, o.a].join(' '); })()", "1 2 true true ");
    expect("set, has and deleteProperty traps are asked",
           "(function(){ var log = [], p = new Proxy({}, {"
           " set: function(t, k, v) { log.push('set ' + k + '=' + v); t[k] = v * 2; return true; },"
           " has: function(t, k) { log.push('has ' + k); return k == 'z'; },"
           " deleteProperty: function(t, k) { log.push('del ' + k); return true; } });"
           " p.q = 3; var h = ('z' in p) + ',' + ('y' in p); delete p.q; return log.join(';') + ' ' + h; })()",
           "set q=3;has z;has y;del q true,false");
    expect("its keys are what ownKeys and getOwnPropertyDescriptor say",
           "Object.keys(new Proxy({}, { ownKeys: function() { return ['x', 'y']; },"
           " getOwnPropertyDescriptor: function(t, k) { return { value: k, enumerable: true, configurable: true }; } })).join()",
           "x,y");
    expect("a proxy of a function is called through apply, and made through construct",
           "(function(){ var f = new Proxy(function(a, b) { return a + b; }, { apply: function(t, th, args) { return t.apply(th, args) * 10; } });"
           " class K { constructor(x) { this.x = x; } }"
           " var C = new Proxy(K, { construct: function(t, args) { return { made: args[0] }; } });"
           " return [f(1, 2), new C(5).made, new (new Proxy(Date, {}))(0).getTime(), new (new Proxy(K, {}))(7) instanceof K].join(' '); })()",
           "30 5 0 true");
    expect("it is a function or an object as its target is, of its target's kind, with its prototype",
           "[typeof new Proxy(function(){}, {}), typeof new Proxy({}, {}), new Proxy(new Date(0), {}) instanceof Date,"
           " Object.getPrototypeOf(new Proxy({}, { getPrototypeOf: function() { return Array.prototype; } })) === Array.prototype,"
           " Array.isArray(new Proxy([], {}))].join(' ')", "function object true true true");
    expect("and its elements spread, its keys walk, its properties copy and it writes out as JSON",
           "(function(){ var ks = []; for (var k in new Proxy({ a: 1, b: 2 }, {})) ks.push(k);"
           " var o = { get me() { return this; } }, p = new Proxy(o, {});"
           " return [[...new Proxy([1, 2, 3], {})].join(), ks.join(), p.me === p, Object.assign({}, new Proxy({ x: 1, y: 2 }, {})).y,"
           " ({ ...new Proxy({ z: 3 }, {}) }).z, JSON.stringify(new Proxy({ a: 1, b: [2] }, {}))].join(' '); })()",
           "1,2,3 a,b true 2 3 {\"a\":1,\"b\":[2]}");
    expect("a proxy's descriptors are read without running its getters, and ownKeys lists what the trap says",
           "(function(){ var calls = 0, p = new Proxy({ get g() { calls++; return 1; } }, {});"
           " var d = Object.getOwnPropertyDescriptors(p);"
           " return [calls, typeof d.g.get, Reflect.ownKeys(new Proxy({}, { ownKeys: function() { return ['a', 'b']; } })).join('')].join(' '); })()",
           "0 function ab");
    expect("a set trap that sets with Reflect.set and the proxy as receiver is asked once a write",
           "(function(){ var calls = 0, h = { set: function(t, k, v, r) { calls++; return Reflect.set(t, k, v, r); } };"
           " var p = new Proxy({ a: 1 }, h); p.a = 2; p.b = 3; return [calls, p.a, p.b, Object.keys(p).join('')].join(' '); })()",
           "2 2 3 ab");
    expect("a revoked proxy refuses, and Proxy without new is an error",
           "(function(){ var r = Proxy.revocable({}, {}); r.proxy.a = 1; var before = r.proxy.a; r.revoke(); var out = [before];"
           " try { r.proxy.x; out.push('read'); } catch (e) { out.push(e instanceof TypeError); }"
           " try { Proxy({}, {}); out.push('made'); } catch (e) { out.push(e instanceof TypeError); }"
           " return out.join(' '); })()", "1 true true");

    /* An array kept no holes: [1, , 3] had an element at 1, and a write past
       the end filled the gap, so `in`, the keys and forEach all found them. */
    expect("an array keeps its holes: not in it, not among its keys, skipped by forEach, undefined when read",
           "(function(){ var a = [1, , 3], b = [1]; b[4] = 5; var c = new Array(3), d = [1, 2, 3]; delete d[1];"
           " var e = [0]; e.length = 3; var seen = []; b.forEach(function (x, i) { seen.push(i); });"
           " return [1 in a, String(a[1]), a.length, Object.keys(b).join(), seen.join(), 0 in c, c.length, 1 in d, d.length, 2 in e,"
           " JSON.stringify(a), a.map(function (x) { return x * 2; }).join('|'), 1 in a.map(String), [...a].join('|'),"
           " a.indexOf(undefined), a.includes(undefined), Object.values([4, , 6]).join(), String(a), b.hasOwnProperty(2)].join(' '); })()",
           "false undefined 3 0,4 0,4 false 3 false 3 false [1,null,3] 2||6 false 1||3 -1 true 4,6 1,,3 false");
    expect("sort puts the holes last, after undefined, and copyWithin carries them",
           "(function(){ var a = [3, , undefined, 1, , 2]; a.sort(); var b = [1, 2, , 4, 5]; b.copyWithin(0, 2, 4);"
           " var c = [, 'b', 'a']; c.sort(function (x, y) { return x < y ? -1 : 1; });"
           " return [a.length, String(a[3]), 3 in a, 4 in a, 5 in a, a.slice(0, 3).join(), 0 in b, String(b[1]), b.length,"
           " c.join('|'), 2 in c].join(' '); })()",
           "6 undefined true false false 1,2,3 false 4 5 a|b| false");

    /* --- the collector (jsgc.h) ---------------------------------------------------------
     *
     * Nothing was given back but a call's own scope, and a page ran out when
     * it had made as much as it was allowed, however little of it it still
     * held. A context here has 24 megabytes. */
    collected("a script that makes 180 megabytes it drops runs in 24, the collector giving them back",
              "var total = 0; for (var i = 0; i < 600; i++) { var s = 'ab'.repeat(50000) + i; total += s.length; }"
              " function mk(n) { var a = [n, n + 1]; return function () { return a[0] + a[1]; }; }"
              " var sum = 0; for (var j = 0; j < 100000; j++) sum += mk(j)();"
              " result = total + ' ' + sum;",
              "60001690 10000000000", 5, 12u * 1024 * 1024);
    collected("and what it can still reach is kept through them: objects, long arrays, maps, sets, closures, typed arrays, "
              "a suspended generator's locals and an awaiting async function's",
              "var keep = { list: [], map: new Map(), set: new Set(), buf: new Uint8Array(1000) };"
              " for (var i = 0; i < 2000; i++) { var o = { n: i, name: 'item ' + i, sub: [i, { d: i * 2 }] };"
              "  keep.list.push(o); keep.map.set('k' + i, o); keep.set.add(o.name); }"
              " keep.list[5000] = 'far'; keep.buf[999] = 7;"
              " var counter = (function () { var c = 0; return function () { return ++c; }; })();"
              " function* gen() { var local = { word: 'gen' + 'erator', arr: [1, 2, 3] }; var x = yield 1;"
              "  yield local.word + x + local.arr.join(''); }"
              " var g = gen(); g.next();"
              " var open; var gate = new Promise(function (r) { open = r; }); var fromAsync = '';"
              " (async function () { var mine = { text: 'as' + 'ync', n: [4, 5] }; await gate; fromAsync = mine.text + mine.n.join(''); })();"
              " for (var j = 0; j < 300; j++) { var junk = 'x'.repeat(100000) + j; var arr = []; for (var k = 0; k < 100; k++) arr.push({ k: k }); }"
              " open();"
              " Promise.resolve().then(function () { return 0; }).then(function () {"
              "  var ok = keep.list.length === 5001 && keep.list[1999].sub[1].d === 3998 && keep.map.get('k777').name === 'item 777'"
              "   && keep.set.has('item 1500') && keep.list[5000] === 'far' && keep.list[3000] === undefined && keep.buf[999] === 7;"
              "  result = [ok, counter(), counter(), g.next('!').value, fromAsync].join(' '); });",
              "true 1 2 generator!123 async45", 3, 20u * 1024 * 1024);
    collected("arguments gathered for a call, jobs waiting and a function's own text are kept while more is made",
              "function* many() { for (var i = 0; i < 60; i++) { var big = 'y'.repeat(150000) + i; yield { n: i, tail: big.slice(-2) }; } }"
              " function count() { var s = 0; for (var i = 0; i < arguments.length; i++) s += arguments[i].n; return s + ':' + arguments[59].tail; }"
              " var gathered = count(...many());"
              " var done = []; for (var p = 0; p < 200; p++) (function (k) { Promise.resolve(k).then(function (v) { done.push({ v: v * 2 }); }); })(p);"
              " function kept(a, b) { return a + b; }"
              " for (var j = 0; j < 200; j++) { var junk = 'z'.repeat(100000) + j; }"
              " var text = kept.toString();"
              " Promise.resolve().then(function () {"
              "  result = [gathered, done.length, done[199].v, text === 'function kept(a, b) { return a + b; }'].join(' '); });",
              "1770:59 200 398 true", 3, 20u * 1024 * 1024);
    collected("what sort is sorting and JSON is writing are kept, though the array is emptied and the keys were made",
              "var a = []; for (var i = 0; i < 60; i++) a.push({ n: 60 - i, pad: 'q'.repeat(20000) + i });"
              " var first = true; a.sort(function (x, y) { if (first) { first = false; a.length = 0; for (var z = 0; z < 3000; z++) a.push(z); }"
              "  var junk = 'j'.repeat(60000) + x.n; return x.n - y.n; });"
              " var o = { 5001: 'one', 5002: { get x() { for (var k = 0; k < 120; k++) { var junk = 'k'.repeat(100000) + k; } return 'two'; } }, 5003: 'three' };"
              " result = a.slice(0, 60).map(function (e) { return e.n; }).slice(0, 5).join() + ' ' + a.length + ' ' + JSON.stringify(o, [5001, 5002, 'x']);",
              "1,2,3,4,5 3000 {\"5001\":\"one\",\"5002\":{\"x\":\"two\"}}", 2, 20u * 1024 * 1024);
    where_memory_is();
    lazy_reading();
    collector_inside();

    /* The count, at the end. It used to be printed half way down, so every
       case after the regular expressions ran without being counted, and a
       suite that lost them would have reported the same total. */
    puts("\n");
    putn(ran - failed);
    puts(" of ");
    putn(ran);
    puts(" passed\n");
    puts(failed ? "JSTEST_FAIL\n" : "JSTEST_PASS\n");
    return failed;
}
