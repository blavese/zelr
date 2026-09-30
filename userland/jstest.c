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
           "(function(){ var s = JSON.parse('\"\\\\u00e9\\\\ud83d\\\\ude00\"'); return s.length; })()", "6");
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
    expect("\\u escapes are the characters they name", "'\\u00e9' === 'é' && '\\u{1F600}'.length === 4",
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
