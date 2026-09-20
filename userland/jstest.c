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

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

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

int main(void) {
    puts("javascript, in ring 3\n");

    /* --- numbers, which are doubles ------------------------------------- */
    expect("arithmetic", "1 + 2 * 3", "7");
    expect("a third is not zero", "1 / 3", "0.3333333333");
    expect("and neither is a tenth", "0.1 + 0.2", "0.3");
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
        int ok = js_run(&J, "class Foo {}", 12);
        int said = !ok && J.error[0];
        puts(said ? "  PASS  " : "  FAIL  ");
        puts("a word this engine does not have is refused by name");
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
        int ok = js_run(&J, "while (true) {}", 15);
        int stopped = !ok && J.error[0];
        puts(stopped ? "  PASS  " : "  FAIL  ");
        puts("a script that never finishes is stopped rather than hanging");
        putc('\n');
        if (!stopped) failed++;
        js_done(&J);
    }

    puts("\n");
    putn(ran - failed);
    puts(" of ");
    putn(ran);
    puts(" passed\n");
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

    expect("exec walks a global pattern through its subject",
           "(function(){ var r = /\\d/g, s = 'a1b2', n = 0;"
           " while (r.exec(s) !== null) n++; return n; })()", "2");
    expect("and a pattern that can match nothing still ends",
           "'aaa'.replace(/b*/g, '-')", "-a-a-a-");

    puts(failed ? "JSTEST_FAIL\n" : "JSTEST_PASS\n");
    return failed;
}
