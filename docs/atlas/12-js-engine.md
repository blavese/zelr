# Atlas 12 -- The JavaScript engine

Source root the repository root (github.com/blavese/zelr main, 2026-09-22). All paths are relative to it; `file:N` means 1-based line N. Everything below was verified by reading the code. Where a behaviour was derived by tracing (not by running) it says so. Nothing here was built or run.

---

## 1. Scope

| File | Lines | Role |
|---|---|---|
| `userland/js.h` | 447 | Values (tagged union `jval`), region allocator, strings, objects (per-object chained hash tables), dense arrays, AST node type `jnode`, completion signals, interpreter context `jctx` |
| `userland/jsparse.h` | 1394 | Lexer (with the regex-vs-divide rule and newline tracking for ASI) and a recursive-descent / precedence-climbing parser that emits a flat `jnode` array |
| `userland/jsrun.h` | 2579 | Conversions, number printing, equality, scope chain, property get/put, calls, `call`/`apply`/`bind`, regex glue, String and Array methods, the tree-walking evaluator (`js_eval`, `js_exec`), globals (console, Math, JSON, Object, Array, ...), entry points `js_init`/`js_done`/`js_run`/`js_eval_text` |
| `userland/jsregex.h` | 611 | A standalone backtracking regex engine: pattern compiler into a node array, and a matcher that uses an explicit continuation stack |
| `userland/jsdom.h` | 1314 | Browser bindings: host hooks for element/document objects, bubbling events, tree changes, classList, querySelector(All) through the CSS engine, timers, XMLHttpRequest, the per-page world lifecycle |
| `userland/jstest.c` | 448 | Ring-3 test program, 144 JS cases, prints `JSTEST_PASS`/`JSTEST_FAIL` |
| `userland/jsprobe.c` | 226 | Diagnostic program: fetches a URL, parses the page, runs each inline script in a bare context and prints what stopped it |

Read for context only (owned by other areas): `userland/pagetest.c` (639 lines, tests jsdom.h), `userland/browser.c` (the embedding), `userland/dom.h` (the DOM primitives jsdom calls), `userland/html.h` (tag table), `userland/web.h` (`w_same`), `userland/build.sh` (compiler flags), `include/fpu.h`, `include/user.h`, `kernel/builtin.S`, `kernel/builtin.c`, `tools/ring3check.py`, `tools/piccheck.py`, `tools/livecheck.py`, `tools/browsercheck.py`, `tools/webserver.py`, `pipeline/gate.sh`, `README.md`.

---

## 2. Big picture

### 2.1 What it is

A from-scratch interpreter for a subset of JavaScript, running only in ring 3. There is no library and no bytecode. The path is source → tokens → a flat array of fixed-size AST nodes → tree-walking evaluation. Every function is `static` and lives in a header, so each program that includes the headers gets its own copy: `jstest.c` and `jsprobe.c` include `js.h`/`jsparse.h`/`jsrun.h` directly, and `browser.c` and `pagetest.c` include `jsdom.h`. The browser has exactly one interpreter instance, the static `jd_J` (`jsdom.h:56`), which lives from page build until the page is left.

```
jsregex.h (needs only zelr.h)
js.h ──► jsparse.h ──► jsrun.h (includes jsregex.h) ──► jsdom.h (+ dom.h, css.h, web.h)
                                                          ▲ browser.c, pagetest.c
jstest.c, jsprobe.c include js.h + jsparse.h + jsrun.h
```

### 2.2 How numbers work (the FPU question, settled)

- Userland is compiled with `zig cc -target x86_64-freestanding-none -O2` and **without** `-mno-sse` (`userland/build.sh:27-35`), so SSE2 is on (it is part of the x86-64 baseline). A JS number is a real IEEE-754 binary64 `double` in hardware (addsd/mulsd/divsd/ucomisd, and cvttsd2si for casts).
- The kernel is compiled with `-mno-sse -mno-sse2 -mno-mmx -mno-80387` (`build.sh:45`). It turns SSE on for each CPU and saves and restores each task's 512-byte FXSAVE area on every context switch (`include/fpu.h:4-52`). The stated reason is that "a number in JavaScript is a double, and one that quietly truncated would be wrong about money" (`fpu.h:12-15`, echoed in `js.h:7-13`).
- The representation is a tagged union `jval` (`js.h:78-86`). There is no NaN-boxing, no fixed point and no integer fast path. `-ffast-math` is not used, so NaN semantics hold. NaN is made at runtime as `zero/zero` from a local variable, Infinity as `1e308 * 10`, and tests use `d != d`.
- There is no libm. `sqrt` is Newton iteration, `floor`/`ceil` go through `(long long)` casts, `pow` is repeated multiplication, and there are no trig/log/exp functions. Parsing decimals (`jsparse.h:209-229`, `jsrun.h:152-197`) and printing them (`jsrun.h:47-135`) are hand-written and not correctly rounded (§10 B5).

### 2.3 Design decisions and the reasons the comments give

1. **A region allocator and no garbage collector** (`js.h:38-53`). The comment: "A collector needs to know every live value, and in a tree-walking interpreter half of them are in local variables of the C functions doing the walking … A region has none of that: nothing is ever freed early because nothing is ever freed at all." The whole region is freed when the world is torn down (page left). The cap `JS_MEM_CAP` = 24 MiB is meant to stop a runaway script "and say so".
2. **Refuse by name rather than approximate** (`js.h:33-36`, `jsparse.h:105-111`). `class` gets "this engine does not have class" rather than a confusing syntax error. In practice several features are approximated silently anyway (§10).
3. **The engine knows nothing about pages.** Two host hooks (`host_get`/`host_set`, `js.h:214-219`) keep it testable on its own (jstest), and `jsdom.h` fills them in (`jsdom.h:9-24`).
4. **Built-ins are written in C**, not bootstrapped in JS, because a JS bootstrap would cost "a parse and a tree walk on every page load" (`jsrun.h:624-629`).
5. **Regexes are compiled fresh at every use** into one static scratch engine (`jsrun.h:647-658`). Patterns are short, an object has no room for a compiled copy, and building patterns in a loop would otherwise leak.
6. **instanceof means "what made me"**, with no prototype chain (`js.h:27-31`, `jsrun.h:1424-1439`): "half a prototype system is worse than none". `new` stamps `__ctor__` on the object it makes.
7. **One world per page, open for the page's whole life** (`jsdom.h:26-42`, `1094-1102`). Handlers run after the scripts have finished, and a function declared in script 1 must be visible from script 2.
8. **Selectors go through the stylesheet engine** (`jsdom.h:564-577`) so a sheet and a script never disagree about what a selector means.
9. **XMLHttpRequest, not fetch**, because the engine has no promises. The request is made on the browser's next pass so that code after `send()` runs first (`jsdom.h:746-765`).
10. **Bubbling only, no capture phase**, per the comment (`jsdom.h:211-217`). The code does not actually refuse capture listeners (§10 B17).
11. **New nodes go in the document's arena**, and removed nodes are unlinked rather than freed (`jsdom.h:372-383`).

### 2.4 Language support summary (details in §3)

**Supported:**
- `var`/`let`/`const`, where all three mean function-scoped var.
- Function declarations (hoisted per block), function expressions, and arrow functions (plain-name parameters only, max 16, lexical `this`).
- Closures.
- `if`, `while`, `do-while`, `for`, `for (var|let|const x in o)`, `switch`, `break`/`continue` with labels, `return`, `throw`, `try`/`catch`/`finally` (the catch parameter is optional).
- Object and array literals.
- The operators: all arithmetic, comparison, equality, logical, bitwise and shift operators, `typeof`, `delete`, `void`, `in`, `instanceof`, comma, `?:`, pre/post `++`/`--`, and the compound assignments `+= -= *= /= %= |= &=`.
- Regex literals, and `new` with constructor functions.

**Refused with a message:**
- `class extends super async await yield import export eval with` (lexer).
- Getters/setters, method shorthand, shorthand properties, computed keys, spread/rest, default parameters, destructuring, `**`, `??`, `?.`, logical assignment, `^= <<= >>= >>>=`, `for-of`, generators, BigInt/binary literals (parse errors).

**Silently approximated:**
- Template literals: no interpolation.
- `let`/`const`: no block scope, and const is mutable.
- Unknown regex flags are dropped.
- More than 24 call arguments are neither evaluated nor passed.
- `for (x in o)` **without** a declaration is a parse error (§10 B2).

---

## 3. File-by-file detail

### 3.1 `userland/js.h` (447 lines)

**Header comment (`:1-54`)** lists what is here and what is not. The "what is not" list says "no regular expressions" (`:30`), which is stale (§10 D1). The memory rationale is at `:38-53`.

**Constants**
- `JS_MEM_CAP (24u*1024*1024)` `:61`: total region bytes per context.
- `JS_STEP_CAP 40000000u` `:62`: steps per run or per callback.
- `JS_DEPTH_CAP 160` `:63`: nested JS function calls.
- `JS_CHUNK (256u*1024u)` `:224`: region chunk size.
- `JS_BUCKETS 8` `:337`: initial hash buckets per object.

**Types**
- `jtype` `:71-73`: `JS_UNDEF=0, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ`. That is six kinds; the comment at `:67` says "Eight kinds".
- `jval` `:78-86`: `{ jtype t; union { double num; int b; jstr *str; jobj *obj; }; }`. It is 16 bytes and passed by value everywhere.
- `jstr` `:92-96`: `u32 len; u32 hash; char s[1]`.
  - One allocation of `12 + len + 1` bytes, NUL-terminated.
  - The hash is 32-bit FNV-1a (`js_hash` `:277-281`, offset 2166136261, prime 16777619), computed when the string is created.
  - Strings are byte strings (page UTF-8 passes through byte-wise), not UTF-16.
  - Strings are not interned; the comment's "Interned lengths" (`:88-91`) is misleading. Equality is pointer, then len, then hash, then bytes.
  - Strings are immutable by convention: natives copy before they modify.
- `jprop` `:98-103`: `key`, `v`, `next` (bucket chain), `enumerable`. `enumerable` is set to 1 at `:391` and nothing ever sets it to 0.
- `jokind` `:105-107`: `JO_PLAIN, JO_ARRAY, JO_FUNC, JO_NATIVE, JO_REGEX`.
- `jnative` `:110`: `jval (*)(jctx *J, jval this_val, jval *argv, int argc)`. There is **no callee parameter**, so a native cannot find its own object. This is the root cause of §10 B1.
- `jobj` `:114-136`, fields:
  - `kind`
  - `buckets`, `nbuckets`, `count`: the property hash.
  - `items`, `len`, `cap`: dense element storage, used only by arrays.
  - `body`, `params`, `nparams`: node indices, used by `JO_FUNC`.
  - `closure`: the defining scope.
  - `fn`: the C function, for natives.
  - `name`
  - `host`: host index; -1 means none.
  - sizeof is 88 bytes, rounded to 96. With 64 bytes of initial buckets, every object costs 160 bytes. There is no prototype field.
- `jchunk` `:140-144`: `next, used, size, data[]`.
- `jscope` `:152-155`: `vars` (a `JO_PLAIN` object used as the variable table) and `parent`.
- `ntype` `:159-170`: `N_NONE` (unused).
  - Expressions: `N_NUM N_STR N_REGEX N_TRUE N_FALSE N_NULL N_UNDEF N_IDENT N_ARRAY N_OBJECT N_FUNC N_CALL N_NEW N_MEMBER N_INDEX N_UNARY N_BINARY N_LOGICAL N_ASSIGN N_COND N_SEQ N_PREINC N_POSTINC N_TYPEOF N_DELETE N_THIS`.
  - Statements: `N_VAR N_BLOCK N_IF N_WHILE N_DO N_FOR N_FORIN N_RETURN N_BREAK N_CONTINUE N_EXPRSTMT N_FUNCDECL N_EMPTY N_THROW N_TRY N_SWITCH N_CASE N_LABEL`.
- `jnode` `:172-179`: `kind; int a,b,c,d` (children or -1)`; double num; jstr *str; int op; int line`. 48 bytes.
- `jsignal` `:183-185`: `JS_OK, JS_RETURN, JS_BREAK, JS_CONTINUE, JS_THROWN, JS_FAILED`.
- `jctx` `:187-220`, fields:
  - `nodes`, `nnodes`, `ncap`: the AST, on the heap via `realloc`.
  - `label`, `pending_label`: the labelled break/continue protocol (§4.5).
  - `chunks`, `allocated`: the region.
  - `global` (a scope) and `global_obj` (which equals `global->vars`).
  - `sig`, and `ret` (the return value or the thrown value).
  - `steps`, `depth`, `error[192]`, `error_line`.
  - `host_get`, `host_set`, `host_data`. `host_data` is not used anywhere.

**Functions**
- `js_alloc(J, n)` `:226-262`:
  - Rounds `n` up to 16.
  - If `allocated + n > JS_MEM_CAP`, sets `sig = JS_FAILED` and `error = "this script asked for more memory than a page is allowed"` (only the first time) and returns 0.
  - Otherwise bump-allocates from the head chunk. A new chunk is `max(n, 256 KiB)` from `malloc`.
  - Memory is zero-filled.
  - If `malloc` fails: `JS_FAILED` with an empty message.
  - `n == 0` returns a valid pointer with zero room (this matters for §10 B7).
- `js_free_all` `:264-273`: frees every chunk and resets `allocated`.
- `js_str_n` / `js_str` `:283-297`: allocate, copy, hash. **Return 0 on failure**, and many callers do not check (§10 B8).
- `js_str_eq` `:299-305`. `js_str_is(jstr*, const char*)` `:307-312` is NULL-safe.
- `js_undef/js_null/js_bool/js_num/js_from_str/js_from_obj` `:316-333`.
- `js_object(J, kind)` `:339-349`: 8 buckets, `host = -1`, `body = params = -1`.
- `js_find(o, key)` `:351-357`: bucket is `hash & (nbuckets-1)`. It **dereferences `key` unconditionally**.
- `js_rehash` `:362-378`: doubles the table and relinks, which reverses the chains.
- `js_set_prop` `:380-396`: updates in place, or inserts at the head of the bucket. Rehashes when `count+1 > nbuckets*3/4`, which with 8 buckets means when the 7th property is added.
- `js_set(J, o, "name", v)` `:398-400`: allocates a fresh key string on every call.
- `js_get_prop` `:402-405`.
- `js_delete_prop` `:407-420`: unlinks the property. Its memory is never reclaimed.
- `js_arr_reserve` `:424-433`: capacity starts at 8 and doubles; the new vector is `(u32)sizeof(jval)*cap` bytes. The multiplication is in u32 (§10 B7). The old vector is abandoned in the region.
- `js_arr_set` `:435-441`: reserves `i+1`, then guards with `if (i + 1 > a->cap) return;`, fills holes with `undefined`, and extends `len`.
- `js_arr_push` `:443-445`, `js_array` `:447`.

**Property iteration order is hash-bucket order**, and newest-first within a bucket; it is not insertion order. Computed FNV-1a buckets (mod 8):

| Key | Bucket |
|---|---|
| `a` | 4 |
| `b` | 5 |
| `c` | 2 |
| `x` | 7 |
| `y` | 4 |

So `Object.keys({a:1,b:2,c:3})` gives `["c","a","b"]`, and `JSON.stringify({x:1,y:2})` gives `{"y":2,"x":1}`.

### 3.2 `userland/jsparse.h` (1394 lines)

**Tokens and operators**
- `ttype` `:27-29`: `T_EOF, T_NUM, T_STRING, T_REGEX, T_NAME, T_PUNCT, T_KEYWORD`.
- Regex flag bits `RXF_I=1, RXF_G=2, RXF_M=4` `:33-35`.
- `jop` `:38-48`: `OP_NONE, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE, OP_SEQ, OP_SNE, OP_AND, OP_OR, OP_NOT, OP_BAND, OP_BOR, OP_BXOR, OP_BNOT, OP_SHL, OP_SHR, OP_USHR, OP_ASSIGN, OP_ADDEQ, OP_SUBEQ, OP_MULEQ, OP_DIVEQ, OP_MODEQ, OP_OREQ, OP_ANDEQ, OP_NEG, OP_POS, OP_INC, OP_DEC, OP_IN, OP_ARROW, OP_INSTANCEOF`.
- `jtok` `:50-59`: `type, op, num, text, len, line, nl_before, flags`. `text` points into the source, or into a region buffer for strings.
- `jlex` `:61-78`: `J, src, n, at, line, tok, nl, failed, ends_expr`.

**Character classes (`:82-88`)**
- Whitespace is space, tab and CR; newline is handled separately so it can be tracked.
- Identifiers are ASCII only: `[A-Za-z_$][A-Za-z0-9_$]*`. A non-ASCII byte outside a string becomes an `OP_NONE` punctuation token, which then fails to parse.

**Errors.** `js_fail_at(J, line, what, extra, extra_len)` `:90-103` sets `JS_FAILED`, `error_line` and `error` (192-byte buffer). Only the first failure is kept.

**Keywords (`JS_WORDS` `:112-117`):** `var let const function return if else for while do break continue new delete typeof in this null true false undefined throw try catch finally switch case default void instanceof`. Note that **`undefined` is a keyword**, so it cannot be declared or assigned.

**Refused words (`JS_UNSUPPORTED` `:119-122`):** `class extends super async await yield import export eval with`. They are refused **in the lexer** wherever an identifier-shaped token appears, including after a dot or as an object key (`{class: 'x'}` and `a.with` both fail). The error is `"this engine does not have " + word` (`:245-254`).

**Regex-vs-divide rule (`js_tok_ends_expr` `:135-145`).** A slash is division if the previous token could end an expression:
- NUM, STRING, NAME, REGEX;
- a keyword of length 4 starting with `t` or `n` (`this`, `true`, `null`) or of length 5 starting with `f` (`false`);
- one of `)`, `]`, `}`.

Anywhere else the slash starts a pattern. The keyword `undefined` does not count as ending an expression (a minor quirk).

**`js_next` (`:147-442`)**
1. **Whitespace and comments** (`:151-173`): skips spaces and `//` and `/* */` comments. Newlines set `L->nl`, which becomes `tok.nl_before`.
2. **Numbers** (`:186-236`):
   - Decimal digits, an optional `.` fraction, an optional exponent (a power of ten computed by repeated ×10), and hex `0x`.
   - No octal, `0b`, `0o`, BigInt or numeric separators.
   - The fraction is accumulated as `v += d*scale; scale *= 0.1`, which is **not correctly rounded**: the literal `0.3` evaluates to `3*0.1 = 0.30000000000000004`.
3. **Names and keywords** (`:239-263`), with the refusal check first.
4. **Strings** (`:266-338`):
   - Delimiters are `'`, `"` **and backtick**. A backtick string is treated exactly like a quoted string, so `${...}` is kept as literal text (a silent approximation).
   - The string is measured first, then copied into a region buffer.
   - Escapes: `\n \t \r \0 \b \f \v`. `\uXXXX` and `\xXX` keep code points below 128 and fold anything else to `?` (`:303-322`). Any other escaped character is itself, so backslash-newline gives a newline, not a line continuation.
   - An unterminated string gives "a string is not closed" (`:279-284`). Strings may span lines.
5. **Regex literals** (`:349-380`), only when `!ends_expr`:
   - The lexer scans for an unescaped `/` outside `[...]` on the same line; if there is none, the slash falls back to division.
   - The flags `i`, `g`, `m` are recorded. Any other alphanumeric flag letter is **silently ignored** (`:372`).
6. **Punctuation** (`:383-441`):
   - Three-character: `=== !== >>>`.
   - Two-character: `== != => <= >= && || ++ -- += -= *= /= %= |= &= << >>`.
   - Single-character: `+ - * / % < > = ! & | ^ ~`.
   - Everything else (brackets, `. , ; : ?`, stray bytes) is `T_PUNCT` with `OP_NONE` and is identified by its text.
   - **Dead code:** a second `TWO('=','>')` at `:413-419` ("this engine does not have arrow functions") is unreachable because `:397` already returned `OP_ARROW`.
   - Not tokens: `** **= ^= <<= >>= >>>= ?? ?. &&= ||= ...`. Each lexes as separate tokens and then fails to parse.

**Node storage.** `js_node` `:448-465` grows `J->nodes` with `realloc` (512 nodes, then doubling). This storage is on the heap, **outside the region and outside `JS_MEM_CAP`**, and is freed only by `js_done`.

**Parser helpers**
- `js_expect` `:500-518`: error `"expected X, not TOKEN"` (80-byte buffer).
- `js_semicolon` `:525-534`: accepts `;`, or nothing before `}`, EOF, or a token preceded by a newline. Otherwise the error is `"expected a semicolon or a new line, not TOKEN"`. That is the whole of ASI.
- The "restricted productions" are handled at their sites:
  - `return` with a newline after it returns undefined (`:1258-1262`).
  - A `break`/`continue` label must be on the same line (`:1269-1285`).
  - Postfix `++`/`--` is not taken after a newline (`:722-724`).

**Precedence (`js_prec` `:537-552`, climbing in `js_parse_binary` `:802-825`)**

| Level | Operators |
|---|---|
| 1 | `||` |
| 2 | `&&` |
| 3 | `|` |
| 4 | `^` |
| 5 | `&` |
| 6 | `== != === !==` |
| 7 | `< > <= >= in instanceof` |
| 8 | `<< >> >>>` |
| 9 | `+ -` |
| 10 | `* / %` |

All binary operators are left-associative. `&&`/`||` produce `N_LOGICAL` nodes; everything else produces `N_BINARY`. There are 10 levels; the header comment says "sixteen".

**Productions**
- **`js_parse_primary` `:556-669`**
  - Literals: `true false null undefined this`, NUM, STRING, REGEX (`op` holds the flag bits), NAME → `N_IDENT`, and `function` expressions.
  - `( expr )`.
  - Array literals (`:604-619`): elements via `js_parse_assign`; a trailing comma is allowed; holes `[1,,2]` are an error.
  - Object literals (`:622-664`): keys may be NAME, KEYWORD, STRING or NUM. A number key is truncated to an integer string (`{1.5:x}` becomes key `"1"`).
  - Anything else: "this is not something a value can start with".
- **`js_parse_postfix` `:673-735`**
  - `.name`: a NAME or KEYWORD must follow, otherwise "expected a property name after the dot".
  - `[expr]`, `(args)`, and postfix `++`/`--` (not after a newline).
- **`js_parse_unary` `:737-800`**
  - `typeof`, `delete`, `void` (→ `N_UNARY` with `OP_NONE`), `new`, and `! ~ - +` (→ `OP_NOT`, `OP_BNOT`, `OP_NEG`, `OP_POS`), prefix `++`/`--`.
  - `new` (`:759-774`) parses a **full postfix chain** and then borrows the arguments of the outermost call. This is wrong for `new X().y` (§10 B3).
- **`js_parse_cond` `:827-842`**: `test ? assign : assign`.
- **Arrow functions** (`:844-933`)
  - `js_try_arrow` saves the lexer by value, then tries `NAME =>` or `( [NAME {, NAME}] ) =>` (at most `JS_ARROW_PARAMS` = 16 names). On failure it restores the lexer exactly.
  - `js_finish_arrow` makes an `N_FUNC` with `op = 1` (the lexical-`this` marker) and `str = 0`. A block body is parsed as a statement; an expression body is wrapped in an `N_RETURN`.
  - `js_try_arrow` runs at the start of **every** `js_parse_assign`, and it allocates a name string for every expression that starts with a NAME. This is a parse-time memory cost.
- **`js_parse_assign` `:935-957`**: arrow first; otherwise conditional, optionally followed by one of `= += -= *= /= %= |= &=` (right-associative).
- **`js_parse_expr` `:959-973`**: the comma operator, `N_SEQ` with the left side in `a` and the right side in `c`.
- **`js_parse_func` `:976-1018`**
  - Optional name; "a function declaration needs a name" when a declaration has none.
  - Parameters are plain NAMEs separated by commas.
  - The body must be a block: "expected a function body".
- **`js_parse_var_list` / `js_parse_var` `:1030-1062`**: `NAME [= assign] {, ...}`. Error "expected a name to declare". The comment at `:1020-1022` says outright that `let`/`const` "differ from var here only in that they are words".
- **`js_parse_block` `:1064-1082`.**
- **`js_try_label` `:1091-1107`**: `NAME :` with lexer save/restore, giving `N_LABEL`.
- **`js_parse_stmt` `:1109-1367`**, in order:
  1. label, block, `;` (`N_EMPTY`), `var`/`let`/`const`, function declaration;
  2. `if`/`else`, `while`, `do … while` (error "expected while after do");
  3. `for` (`:1171-1253`):
     - declared for-in is found by looking one token past the name (`:1181-1216`);
     - the undeclared for-in branch (`:1220-1233`) is **unreachable** (§10 B2);
     - three-part `for`;
  4. `return`, `break [label]`, `continue [label]`, `throw`;
  5. `try` (`:1294-1310`): the catch parameter is optional; `finally` is supported;
  6. `switch` (`:1312-1359`): case tests use `js_parse_assign`; error "expected case or default";
  7. otherwise an expression statement.
- **`js_parse(J, src, len)` `:1370-1394`**: wraps the whole program in an `N_BLOCK`. Returns the head node, or -1 if parsing failed.
  - `jparse P;` is not zero-initialised, and the first `js_next` reads the uninitialised `P.L.tok` (`:148`) (§10 B18).

**AST field usage (verified against both the parser and the evaluator)**

| Node | Fields |
|---|---|
| `N_NUM` | `num` |
| `N_STR` / `N_IDENT` | `str` |
| `N_REGEX` | `str` = pattern source, `op` = RXF bits |
| `N_ARRAY` | `a` = chain of `N_SEQ` cells (`cell.a` = element, `cell.b` = next) |
| `N_OBJECT` | `a` = chain of `N_SEQ` (`cell.str` = key, `cell.a` = value) |
| `N_FUNC` / `N_FUNCDECL` | `str` = name (0 for anonymous/arrow), `a` = body, `b` = param chain (`N_SEQ` with `str`), `c` = count, `op` = 1 for an arrow |
| `N_CALL` / `N_NEW` | `a` = callee, `b` = argument chain (`N_SEQ` with `a`) |
| `N_MEMBER` | `a` = object, `str` = name |
| `N_INDEX` | `a` = object, `b` = index |
| `N_UNARY` | `op` ∈ {`OP_NOT`, `OP_NEG`, `OP_POS`, `OP_BNOT`, `OP_NONE` (= void)}, `a` |
| `N_BINARY` / `N_LOGICAL` | `op`, `a`, `b` |
| `N_ASSIGN` | `op` ∈ {`=`, `+=`, `-=`, `*=`, `/=`, `%=`, `|=`, `&=`}, `a` = target, `b` = value |
| `N_COND` | `a`, `b`, `c` |
| `N_SEQ` (comma) | `a` = left, `c` = right |
| `N_SEQ` (list cell) | `a` / `str`, `b` = next |
| `N_PREINC` / `N_POSTINC` | `op` = `OP_INC` or `OP_DEC`, `a` |
| `N_TYPEOF` / `N_DELETE` | `a` |
| `N_VAR` | `a` = chain (`cell.str` = name, `cell.a` = initialiser or -1) |
| `N_BLOCK` | `a` = chain (`cell.a` = statement) |
| `N_IF` | `a` = test, `b` = then, `c` = else |
| `N_WHILE` / `N_DO` | `a` = test, `b` = body |
| `N_FOR` | `a` = init (`N_VAR` or `N_EXPRSTMT`), `b` = test, `c` = step, `d` = body |
| `N_FORIN` | `str` = variable, `a` = object expression, `b` = body, `d` = declared flag; `c` = target expression (unreachable form) |
| `N_RETURN` / `N_THROW` / `N_EXPRSTMT` | `a` |
| `N_BREAK` / `N_CONTINUE` | `str` = label or NULL |
| `N_LABEL` | `str`, `a` = statement |
| `N_TRY` | `a` = try block, `b` = catch block, `c` = finally block, `str` = catch parameter |
| `N_SWITCH` | `a` = subject, `b` = first `N_CASE` |
| `N_CASE` | `a` = test (-1 for default), `b` = statement chain, `c` = next case |

**What unsupported syntax actually produces** (traced through the code)

| Construct | Result |
|---|---|
| `class`, `async`, `await`, `yield`, `import`, `export`, `eval`, `with`, `super`, `extends` | "this engine does not have <word>" |
| `function* g(){}` as a statement | "a function declaration needs a name" |
| `function*` in an expression | "expected (, not *" |
| `for (const x of a)` | "expected ;, not of" |
| `for (k in o)` (no declaration) | "expected ;, not )" |
| `a ?? b`, `a?.b`, `x ** 2`, `[...a]`, `f(...a)`, `[1,,2]`, `x ^= 1`, `x <<= 1`, `a ||= b`, `new.target`, `<!--` | "this is not something a value can start with" |
| `function f(a = 1)` | "expected ), not =" |
| `function f(...r)` | "expected ), not ." |
| `var {a} = o`, `var [a] = x` | "expected a name to declare" |
| `({a, b})` | "expected :, not ," |
| `({m() {}})` | "expected :, not (" |
| `({get x() {}})` | "expected :, not x" |
| `({[k]: v})` | "expected a property name" |
| `(a = 1) => a` as a statement | "expected a semicolon or a new line, not =>" |
| `0b101`, `10n` | "expected a semicolon or a new line, not b101" / "… not n" |
| `` `a${b}` `` | no error; evaluates to the literal string `a${b}` |
| `/x/u`, `/x/s`, `/x/y` | no error; the flag is dropped |
| `debugger;` | an undeclared identifier; evaluates to undefined, no error |

### 3.3 `userland/jsregex.h` (611 lines)

**Header (`:1-38`)**
- Claims support for: literals, `.`, classes with ranges, negation and escapes; `\d \D \w \W \s \S \b \B`; `\n \t \r \f \v \0 \xNN \uNNNN`; `^ $`; capturing and `(?:` groups; alternation; `* + ? {n} {n,} {n,m}`, greedy or lazy; flags g/i/m.
- Refuses lookaround, backreferences, named groups, sticky, and unicode property escapes, "refused when it is compiled".

**Limits (`:42-46`)**
- `RX_NODES 512`, `RX_CLASSES 64`, `RX_CONTS 512`, `RX_CAPS 10` (group 0 plus groups 1–9), `RX_STEPS 400000` per start position.
- Nesting depth over 24 is refused (`:416`).

**Types**
- `RXN_*` `:48-51`: `CHAR=1, ANY, CLASS, GROUP, BOL, EOL, WORDB, NWORDB`.
- `rxcont` `:58-63`: `node, parent, rep, at`.
  - `rep >= 0` means resume a repetition.
  - `rep == -2` means close a capture; here `node` is the capture number.
  - `rep == -1` ("carry on with a node") is never pushed by the current code, so the path at `:490` is unreachable.
- `rxnode` `:65-75`: `kind, greedy, min, max` (-1 = unbounded), `ch, cls, alt` (first alternative), `cap` (-1 or a group number), `next` (sequence link), `alt_next`.
- `rx` `:77-97`: `nodes[512], nnodes, classes[64][32]` (256-bit sets), `nclasses, ncaps, icase, multiline, global`, plus the match state `s, len, cont[512], ncont, cap_start[10], cap_end[10], end, steps`, and `ok, why[64]`. About 26 KB. There is exactly one instance, `static rx js_rx` (`jsrun.h:659`).

**Compiler**
- `rx_compile(R, pat, len, flags)` `:438-460`:
  - Zeroes the whole struct byte by byte.
  - Sets `ncaps = 1`.
  - Flags: `i`, `m`, `g`. The flags `y`, `u`, `s` give "a flag that is not here"; this path is unreachable through normal JS (§10 B16).
  - The root is an `RXN_GROUP` with `cap = 0`.
  - Trailing input gives "something left over".
- `rx_parse_alt` `:415-436`: alternatives joined by `alt_next`. An empty branch becomes a GROUP with `min = max = 0`, which is how `(a|)` works.
- `rx_parse_seq` `:402-412`.
- `rx_parse_term` `:276-399`:
  - `(`: `(?:` is non-capturing; `(?=`, `(?!` and `(?<` give "lookaround is not here", so named groups get that message too. Otherwise the group captures while `ncaps < 10`; **later groups silently do not capture** (`:292`). Missing `)` gives "a group with no )".
  - `[`: a class.
  - `.`, `^`, `$`.
  - `\`: class escapes; `\b`/`\B`; `\1`–`\9` give "a back reference is not here"; anything else goes through `rx_escape_char` (`:154-189`), where `\u` keeps only the low byte and unknown escapes such as `\k` or `\c` become the literal letter.
  - A leading `* + ?` gives "a quantifier with nothing before it". A `)` or `|` is left for the caller.
  - Quantifiers `* + ? {n} {n,} {n,m}`; a `{` that is not a valid quantifier is literal. A trailing `?` makes the quantifier lazy.
  - A quantifier on an anchor or boundary is reset to `{1,1}`.
  - "pattern too big" when the node array is full.
- `rx_parse_class` `:208-271`:
  - Ranges; class escapes inside a class; `\b` inside a class is backspace.
  - Errors: "a range that runs backwards", "a class with no ]", "an empty class" (so `[]` and `[^]` are refused), "too many classes".
  - **A negated class never matches `\n`** (`:252-257`); JS's does.
  - With `i`, the class is case-folded once at compile time (`:260-269`).
  - Literal characters are lowered at compile time and subject characters at match time. All folding is ASCII only.

**Matcher**
- `rx_search(R, s, len, from)` `:589-611` tries each start position from `from` to `len`, resetting the continuation stack, `steps` and the captures each time, and calls `rx_run(root)`. On success `cap_start[0] = start` and `cap_end[0] = end`.
- `rx_rep(n, pos, cont, done)` `:561-581`: `more = (max < 0 || done < max)`.
  - Lazy: try the rest first (`rx_run(next)`), then one more iteration.
  - Greedy: push a frame `{n, cont, done+1, pos}` and try the atom, then fall back to the rest.
- `rx_atom` `:504-559` matches one atom and then calls `rx_cont_do`. A GROUP tries each alternative, pushing a capture-close frame for capturing groups.
- `rx_cont_do` `:471-491`: when `cont < 0` the match is done. A capture-close frame sets the capture and restores it on failure. A repetition frame has a zero-progress guard (`pos == at && rep > min` fails) and then resumes the repetition.
- **Frames are popped only on backtrack.** One frame is used per quantifier iteration along the success path, so an atom can iterate at most about 511 times in one attempt (§10 B16).
- C recursion is about 3 frames per iteration and is bounded by the same 512-frame limit.
- Captures inside repeated groups keep the last iteration and are not reset per iteration.

### 3.4 `userland/jsrun.h` (2579 lines)

#### Errors and numbers

**`js_throw(J, what, line)` `:24-33`** sets `JS_THROWN`, `error`, `error_line`, and `ret = js_str(what)`. **Engine errors are therefore thrown as strings**: `catch (e) { e.message }` gives undefined.

**`js_num_text(d, out, cap)` `:47-135`** (`cap` must be at least 32):
- NaN → `"NaN"`.
- `|d| > 1.7e308` → `"±Infinity"` (note 1.7e308 is below DBL_MAX).
- Whole numbers with `|d| < 2^53` → exact integer text.
- `|d| >= 1e21` or `|d| < 1e-6` → exponent form: a lead digit and up to 9 more digits, truncated.
- Otherwise the whole part via `(long long)` plus **10 fraction digits, truncated, trailing zeros trimmed**.
- The comment (`:41-45`) says "ten significant places"; the code prints ten fraction digits (§10 B5).

#### Conversions

| Conversion | Rule |
|---|---|
| `js_to_bool` `:141-150` | undefined, null, false, 0, NaN, `""` → false; objects → true |
| `js_str_to_num` `:152-197` | Trims space, tab, `\n` (not `\r`); empty string → 0; optional sign, decimal digits, fraction, exponent; any trailing junk → NaN. No hex, no `"Infinity"` |
| `js_to_num` `:199-220` | number; bool → 0/1; null → 0; string as above; array `[]` → 0; `[x]` → ToNumber(x) (so `[undefined]` → NaN, where JS gives 0); other objects and undefined → NaN |
| `js_to_i32` `:224-229` | `|d| > 1e18` or NaN → 0; otherwise `(int)(u32)((long long)d & 0xFFFFFFFF)`. `js_to_u32` `:231` |
| `js_to_str` `:233-276` | undefined, null, bool text; numbers via `js_num_text`; functions → `"function"`; arrays → comma-joined, with null/undefined elements empty, **recursive with no cycle guard**; every other object (regex included) → `"[object Object]"`. No user `toString` is ever called |
| `js_concat` `:278-289` | Always allocates a new string. Returns `a` unchanged if allocation fails |

#### Equality

- **`js_strict_eq`** `:296-305`: the kinds must match; numbers compare with `==` (so NaN ≠ NaN); strings by content; objects by identity.
- **`js_loose_eq`** `:307-319`:
  - same kind → strict;
  - `null == undefined`;
  - null or undefined against anything else → false;
  - **either side an object → compare both as strings** (so `[1] == true` is false; JS says true);
  - otherwise compare as numbers.

#### Scopes (`:323-350`)

- `js_scope` allocates a scope plus a variable object (176 bytes). **If allocation fails it returns `parent`.**
- `js_lookup` walks the scope chain.
- `js_declare` writes into the given scope.
- `js_assign_name` assigns to the nearest binding, or creates a global if there is none.

#### Property access

**`js_get(J, target, name)` `:404-475`**
1. `"length"` on a string or array → its length.
2. String: an index gives a 1-character string; anything else goes to `js_string_method`.
3. null or undefined → throw "cannot read NAME of null" / "… of undefined" (`js_nothing` `:392-402`).
4. Other primitives (numbers, booleans) → undefined. **Numbers have no methods**, so `x.toFixed(2)` gives "toFixed is not a function".
5. Array: an index gives the element; otherwise `js_array_method`.
6. Function or native: `call`/`apply`/`bind` → a fresh native wrapper with `__fn__ = target` (`:450-460`).
7. `host >= 0` and `host_get` is set → ask the host first (the name is truncated to 63 characters).
8. Own property table. There is no prototype lookup.

**`js_put`** `:477-505`
- null or undefined → throw "cannot set NAME of …".
- Non-objects are ignored.
- Array: an index goes to `js_arr_set`. `length` truncates when smaller; when larger it only reserves and **does not change `len`**.
- Host objects → `host_set` if it handles the name.
- Otherwise the own property table.

#### Calls

**`js_call(J, fn, this, argv, argc)`** `:511-559`
1. A non-function throws "this is not a function".
2. `depth >= 160` throws "too many nested calls".
3. A native is called directly as `f->fn(J, this, argv, argc)`. Natives do not count toward the depth, and **their `__this__` is not consulted here**.
4. For a JS function, an own `__this__` property (set on arrows) overrides `this`.
5. A new scope is created with the closure as parent. Each parameter is declared from `argv`, or undefined.
6. **`arguments` is a real array of all the passed arguments**, declared in the call scope, arrows included.
7. The body runs with `depth++`.
8. `JS_RETURN` → the return value, and `sig` is reset to OK. Anything else gives undefined with `sig` left as it was, so a stray `JS_BREAK` escapes into the caller (§10 B25).

**`call` / `apply` / `bind`** `:561-622`
- The natives are `nat_fn_call`, `nat_fn_apply` (the argument array is truncated to 24), `nat_fn_bind` (a native "bound" holding `__fn__` and `__bound__`; the partial-application arguments are dropped) and `nat_fn_bound`.
- All four read the target with `fn_held(J, t)`, which looks up `__fn__` on **the `this` value they were called with**. That value is the function itself, not the wrapper, **so none of them work** (§10 B1).

**Natives**
- `js_native` `:631-637`, `js_arg` `:639-641`.
- `static jval js_bound_this` `:645` is written at `:1099` and never read (dead).

#### Regex glue (`:647-788`)

- `js_rx_load` `:669-688` reads the own properties `source` and `flags`, compiles into `js_rx`, and on failure throws `rx.why` (or "a pattern this cannot read").
- `js_rx_result` `:694-709` builds the exec/match array: element 0 is the whole match, then the groups (undefined for a group that did not match), plus `index` and `input` properties.
- `nat_re_test` `:711-715` searches from 0 and **ignores lastIndex**.
- `nat_re_exec` `:719-744`: when global, starts at `lastIndex` (if it is a number and in range) and updates it, advancing by one on an empty match; on failure it resets to 0.
- `js_regex_new` `:746-766` gives each regex object the own properties `source`, `flags` (letters in g, i, m order), `global`, `ignoreCase`, `multiline`, `lastIndex = 0`, and native `test` and `exec` methods. About 1.2 KB per object.
- `nat_regexp_make` `:771-788` implements `RegExp(p, f)` with or without `new`. When `p` is a regex its `source` is reused; flags other than g/i/m are dropped silently.

#### String methods

Implemented by `js_string_method` `:1098-1124`. Each fetch allocates a native with `__this__ = target`. `N_CALL` substitutes that receiver (`:1595-1598`).

| Method | Behaviour |
|---|---|
| `charAt(i)` `:790` | `(int)ToNumber`; NaN becomes INT_MIN, so `charAt()` → `""` (JS gives the first character) |
| `charCodeAt(i)` `:797` | The byte value 0–255, or NaN |
| `indexOf(s)` `:819` | **No position argument** |
| `includes(s)` `:824` | No position argument |
| `startsWith(s)` `:829` | No position argument |
| `slice(a, b)` `:836` | JS semantics, negative offsets from the end |
| `substring` `:850` | **Same as slice** (§10 B11) |
| `substr` `:1105` | **Same as slice** |
| `toUpperCase`, `toLowerCase` `:854-874` | ASCII only |
| `trim` `:876` | Space, tab, `\n`, `\r` |
| `split(sep)` `:888-939` | No argument → `[s]`; a regex (empty matches split between characters, captures not included); `""` → characters; a string. No limit argument |
| `replace(pat, rep)` `:1039` | String pattern: first occurrence, `rep` taken literally (a function becomes the text "function"). Regex: `nat_str_replace_re` `:978-1037`, with `$$ $& $1–$99` (`js_rx_expand` `:944-976`) or a function called with (match, groups…, index, string), one match or all with `g`, output in a 16 KB static buffer |
| `replaceAll` `:1108` | **Mapped to replace** |
| `repeat(n)` `:1052` | Capped at 4096 repetitions; quadratic concatenation |
| `match(re)` `:1063` | Global → array of match strings, or null. Non-global → the exec-style array. A non-regex argument → null |
| `search(re)` `:1090` | Index or -1. A non-regex argument → -1 |

Plus `.length` and `s[i]`. Missing: `lastIndexOf`, `endsWith`, `padStart`/`padEnd`, `trimStart`/`trimEnd`, `concat`, `localeCompare`, `codePointAt`, `at`, `toString`, and `String.fromCharCode`.

#### Array methods

Implemented by `js_array_method` `:1271-1289` (same `__this__` trick).

| Method | Behaviour |
|---|---|
| `push(...)` `:1128` | Returns the new length |
| `pop` `:1134` | |
| `shift` `:1140` | O(n) |
| `join(sep = ",")` `:1149` | null/undefined elements are empty; **quadratic** concatenation |
| `indexOf(v)` `:1162` | Strict equality; no fromIndex |
| `slice(a, b)` `:1171` | |
| `forEach(fn)` `:1185` | Callback gets (item, index) only; stops on a throw; `thisArg` ignored |
| `map(fn)` `:1197` | Same callback arguments |
| `filter(fn)` `:1210` | Same callback arguments |
| `reverse` `:1224` | In place |
| `sort(cmp)` `:1238-1269` | Stable **insertion sort**, O(n²). With a comparator (`cmp(a, b) > 0` means swap); otherwise by byte-wise string comparison |

Plus `.length` (get and set) and `a[i]`. Missing: `unshift`, `splice`, `concat`, `reduce`/`reduceRight`, `some`, `every`, `find`, `findIndex`, `includes`, `lastIndexOf`, `fill`, `flat`, `toString`.

#### Evaluator

**`js_tick` `:1293-1305`** increments `steps` once per `js_eval`/`js_exec` entry and once per loop iteration. Above `JS_STEP_CAP` it sets `JS_FAILED` with "this script ran for too long and was stopped". It returns `sig == JS_OK`, so any pending signal short-circuits evaluation.

**Assignment targets.** `jplace` / `js_place` `:1309-1341` classify a target as kind 0 (name), kind 1 (object plus name string; an index value is converted with `js_to_str`, which allocates) or kind 2 (not assignable). `js_place_get` `:1351` and `js_place_put` `:1360` read and write through it.

**`js_binary` `:1365-1477`**
- `+`: concatenates if either side is a string **or a non-array object**; otherwise adds numbers. So `[1,2]+3` is NaN (§10 B12).
- `- * /`: numeric.
- `%` `:1379-1385`: truncating quotient through `(long long)`; a zero divisor gives NaN.
- Relational operators: two strings compare byte-wise; otherwise numerically.
- `== != === !==`.
- `& | ^ << >> >>>`: ToInt32, with shift counts masked by 31.
- `instanceof` `:1440-1462`:
  - The left side must be an object (otherwise false). The right side must be an object, otherwise throw "the right of instanceof is not a constructor".
  - True if `l.__ctor__ === r`.
  - Otherwise, by the right side's `name`: `Array` → is an array; `RegExp` → is a regex; `Function` → is a function or native; `Object` → always true.
  - There is no inheritance.
- `in` `:1464-1472`: for arrays, an index `< len`; otherwise the **own property table only** (host properties and built-in methods are not seen).
- The default case "this operator is not one this engine has" is effectively unreachable.

**`js_eval` `:1479-1761`**
- Literals; `N_REGEX` creates a new object on each evaluation (`:1487-1494`) so that loops do not share `lastIndex`; `N_THIS`.
- `N_IDENT`: an unbound name gives **undefined, never a ReferenceError** (`:1501-1508`).
- `N_ARRAY`, `N_OBJECT`.
- `N_FUNC` / `N_FUNCDECL` `:1531-1544`: the closure is the current scope. For an arrow, `__this__` is set to the current `this`.
- `N_MEMBER` `:1546-1561`: a null or undefined object throws "cannot read NAME of nothing".
- `N_INDEX` `:1563-1574`: fast path for an array indexed by an integral number; otherwise `js_get` with `js_to_str(index)`.
- `N_CALL` `:1576-1634`:
  1. For a member or index callee, `self` is the object; a null or undefined object throws "cannot call a method on nothing".
  2. A native with its own `__this__` replaces `self`.
  3. At most 24 arguments are evaluated.
  4. Calling a non-function whose callee is a `N_MEMBER` or `N_IDENT` throws "NAME is not a function".
- `N_NEW` `:1636-1654`:
  1. Evaluate the callee and up to 24 arguments.
  2. Create a fresh `JO_PLAIN` object and set `__ctor__` to the callee (only if the callee is an object).
  3. `js_call` with `this` = the fresh object. A non-function callee gives the generic "this is not a function".
  4. Return the call's result if it is an object; otherwise the fresh object.
- `N_UNARY`: `! - + ~ void`.
- `N_TYPEOF`: evaluates its operand normally, which is safe on undeclared names because they are undefined. Results: `undefined object boolean number string function`; `typeof null` is `"object"`; the global `Object` is an object.
- `N_DELETE` `:1687-1692`: only member or index places on objects, via `js_delete_prop`. Array elements are not affected (returns false). Names give false.
- `N_BINARY`, `N_LOGICAL` (short-circuit, returns the deciding value), `N_COND`.
- `N_ASSIGN` `:1716-1741`: the place is evaluated before the right side. Compound operators map `|=` to `BOR` and `&=` to `BAND`. A non-place throws "this cannot be assigned to".
- `N_PREINC` / `N_POSTINC` `:1743-1750`: a non-place silently gives undefined.
- `N_SEQ`.

**`js_hoist` `:1768-1776`** evaluates every `N_FUNCDECL` that is a **direct child** of the block and declares it in the current scope. It runs **every time the block is entered**, so a loop body re-creates its functions on each iteration. `var` declarations are not hoisted.

**`js_exec` `:1778-2023`**

| Statement | Behaviour |
|---|---|
| `N_BLOCK` | Hoist, then run each statement; any non-OK signal returns. **No new scope** |
| `N_VAR` `:1799-1806` | Declares in the current scope at execution time; `var x;` **resets** an existing x to undefined |
| `N_IF` | |
| `N_WHILE` / `N_DO` / `N_FOR` | Each takes `mine = pending_label` and clears it. On break/continue: if the label matches (`js_label_mine` `:1346-1349`; an unlabelled one always matches) it is consumed, otherwise the signal propagates. An extra `js_tick` per iteration |
| `N_FORIN` `:1878-1916` | Collects the keys first (array indices as strings, then enumerable own properties in bucket order); assigns or declares each; **always consumes break/continue and ignores labels and `pending_label`** (§10 B4) |
| `N_RETURN` | Sets `ret` and `JS_RETURN` |
| `N_BREAK` / `N_CONTINUE` | `label = str` |
| `N_LABEL` `:1937-1947` | Sets `pending_label`; afterwards catches a break carrying its own name |
| `N_THROW` | `ret = value`, `error = ToString(value)`, `JS_THROWN` |
| `N_TRY` `:1963-1986` | A catch runs only for `JS_THROWN`, in a **new child scope** holding the parameter. `finally` always runs; if it completes normally the earlier signal and `ret` are restored, otherwise its own signal wins. `JS_FAILED` (step or memory cap) is never caught |
| `N_SWITCH` `:1988-2017` | Pass 0 compares case tests in order with **strict** equality; pass 1 goes to `default`; execution falls through from there. A `break` is swallowed, **including a labelled break meant for an outer loop** |

#### Globals (`js_globals` `:2449-2517`)

| Global | Implementation and notes |
|---|---|
| `console.log`, `.warn`, `.error` | All `nat_log` `:2036`. Arguments are space-joined and written to `js_print_hook` `:2034`, **which nothing ever sets**, so all console output is dropped |
| `RegExp` | See the regex glue above |
| `Math.floor`, `ceil`, `round` | `(long long)`-based `:2122-2157`. `round` is `floor(x + 0.5)`. NaN, Infinity or `|x| ≥ 2^63` give garbage (§10 B6) |
| `Math.abs` | |
| `Math.sqrt` | Newton, 60 iterations `:2134-2144` |
| `Math.min`, `Math.max` | `Math.min()` gives NaN (§10 B6) |
| `Math.pow` | Integer exponents with `|e| ≤ 1024` by repeated multiplication; `e = 0.5` → sqrt; any other non-integer → NaN (deliberate, `:2188-2190`) |
| `Math.random` | xorshift32, seed `0x5A4C5200`, 24-bit resolution `:2206-2213`. Static per process: the same sequence on every browser launch, continuing across pages |
| `Math.PI`, `Math.E` | |
| `JSON.stringify` | `:2217-2276` (see below) |
| `JSON.parse` | `:2285-2376` (see below) |
| `Object` | A **plain object**, not callable, with `name = "Object"`. Methods `keys` `:2380` and `values` `:2399` ignore `enumerable` and use bucket order; arrays give index strings / elements. `new Object()` → "this is not a function" |
| `Array(n)` / `Array(a, b, …)` | `nat_array_make` `:2419-2432`. `n` is capped at 100000 **silently**; `Array.isArray` |
| `Function` | Throws "a function built out of text is not here" |
| `parseInt(s, r)` `:2047-2080` | Skips spaces and tabs; sign; `0x` stripped **only when the radix is explicitly 16** (so `parseInt("0x1F")` is 0); radix outside 2–36 → 10 |
| `parseFloat` `:2082-2095` | Takes the longest prefix of `[0-9.+-eE]`, then applies the strict converter (`"1-2"` → NaN) |
| `isNaN` | |
| `String`, `Number`, `Boolean` | Return primitives. With `new` they return the empty fresh object |
| `NaN`, `Infinity` | |

`JSON.stringify` details:
- Escapes only `" \ \n \t`.
- `undefined` is written as the text `undefined`, and NaN/Infinity as `NaN`/`Infinity`.
- Functions become `null`.
- Object keys come in bucket order and non-enumerable internal properties are included.
- No replacer or indent.
- **One `js_concat` per character**, so the cost is O(S²).
- No cycle guard.

`JSON.parse` details:
- Lenient: garbage gives undefined rather than a throw.
- Escapes: `\n \t \r` are decoded; any other escaped character stands for itself, so `\uXXXX` is **not decoded**.
- Any word starting with t, f or n is read as true, false or null.

jsdom.h adds: `document`, `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`, `XMLHttpRequest`, `window`.

**Absent** (a reference gives undefined; a call gives "X is not a function" or, with `new`, "this is not a function"): `Date`, `Error` and its relatives, `Promise`, `Map`, `Set`, `Symbol`, `encodeURIComponent`/`decodeURIComponent`, `escape`, `isFinite`, `Number.*`, `Math` trig/log/exp/trunc/sign, `Object.assign/create/defineProperty/entries/freeze/getPrototypeOf`, `Array.from/of`, `globalThis`, `self`, `location`, `navigator`, `localStorage`, `alert`, `requestAnimationFrame`, `getComputedStyle`.

#### Entry points

- `js_init` `:2521-2528`: `memset`s the whole `jctx`, creates the global scope and globals. Set any host hooks **after** calling it.
- `js_done` `:2530-2535`: frees `nodes` (heap) and all region chunks.
- `js_run(J, src, len)` `:2539-2553`:
  1. Reset `sig`, `steps` and `error`.
  2. Parse, appending to `J->nodes`. The nodes of earlier scripts stay, because functions reference them.
  3. Execute in the global scope with **`this = undefined`**.
  4. Return 0 on a throw or failure.
- `js_eval_text` `:2557-2579`: parses **one expression** with no end-of-input check (`"1 2"` evaluates to 1), evaluates it in the global scope, and returns the value. Used by jstest and by jsdom's attribute handlers.

### 3.5 `userland/jsdom.h` (1314 lines)

**Global state**
- `jd_doc`, `jd_sheet` (borrowed), `jd_wrap` (one wrapper per node; `DOM_NODES` = 20000 entries, allocated from the region = 160,000 bytes), `jd_dirty` `:50-53`.
- `jd_J` (the world), `jd_open`, `jd_err[128]` `:56-58`.
- Listeners: `jd_listen[256]`, `jd_nlisten`; `jd_prevented`, `jd_stopped` `:228-234`.
- Timers: `jd_timer[64]`, `jd_ntimer`, `jd_timer_id` `:695-697`.
- Requests: `jd_req[8]`, `jd_nreq` `:773-774`.
- Host callbacks: `jd_do_request` `:778`, `jd_get_script` `:1227`, `jd_outside`/`jd_outside_failed` `:1235`.

**Host numbers**
- An element or text node has `host` = its node index.
- The document object has `JD_DOCUMENT` = 0x1000000 `:48`.
- A classList object has `JD_CLASSLIST + el`, with `JD_CLASSLIST` = 0x2000000 `:463`.

**Error recording.** `jd_note_error` `:65-85` copies only the **first** error into `jd_err`, appending " at line N" (N capped at 999999).

**Element wrappers (`jd_element` `:105-139`)** are made lazily and cached in `jd_wrap[node]`, so `a === b` holds for two lookups of the same node. Each wrapper carries 10 native **own, enumerable** properties: `getAttribute, setAttribute, addEventListener, removeEventListener, appendChild, insertBefore, removeChild, remove, querySelector, querySelectorAll`. That is about 3.2 KB per wrapper.

**Document object (`jsdom_open` `:1162-1189`)**
- Own properties: `getElementById, getElementsByTagName, addEventListener, removeEventListener, createElement, createTextNode, querySelector, querySelectorAll`.
- Host get: `title`, `body`. Host set: `title`.

**Element properties through the hooks**

| Property | `jd_host_get` `:921-1044` | `jd_host_set` `:1046-1092` |
|---|---|---|
| `textContent` | `dom_text_content` into a static 4096-byte buffer (truncates at 4095) | `dom_set_text` (allocates a new text node on every write) |
| `tagName` | `dom_tag_name`, **lowercase** for known tags | -- |
| `id` | the `id` attribute or `""` | -- (the write goes to a shadowed JS property) |
| `className` | the `class` attribute | writes the `class` attribute |
| `value` | the `value` attribute; for a textarea its text; `""` for input, select, button without the attribute; otherwise falls through to own properties | writes the `value` attribute on any element |
| `checked` | the attribute exists and is not `"0"` | writes the attribute as `"1"` or `"0"` |
| `classList` | a **new** object on every access, with `add`, `remove`, `contains`, `toggle` | -- |
| `parentNode`, `parentElement` | wrapper or null | -- |
| `firstChild`, `lastChild`, `nextSibling`, `previousSibling` | wrapper or null (text nodes included) | -- |
| `children` | a snapshot array of element children | -- |

**Methods**
- `getAttribute`/`setAttribute` `:175-196`: `setAttribute` goes through `dom_attr_set` and sets dirty.
- `getElementById` `:148-155` goes through `dom_by_id`, which scans every node index.
- `getElementsByTagName` `:157-173` uses `html_tag_of`; unknown names become `T_OTHER`.
- `createElement` `:384-396` uses `dom_create_element` and does not set dirty.
- `createTextNode` `:398-408`.
- `appendChild` `:418-425`, `insertBefore` `:427-434` (a reference that is null or not a child means append), `removeChild` `:436-444` (undefined unless the argument is a child), `remove` `:446-453`.
- The move and cycle rules live in `dom.h`:
  - `dom_append` (`dom.h:144-157`) unlinks a node that is already somewhere, then appends it.
  - `dom_contains` refuses to put an element inside its own descendant (or itself) -- **silently**; the child is still returned.
  - `dom_insert_before` (`dom.h:161-176`) applies the same rules.
  - Nothing is ever freed.
- classList (`:465-562`): one class per call, split on space or tab; the attribute is rewritten through a 512-byte static buffer (truncated at 511); `toggle` ignores its force argument and returns the new state.

**Selectors (`jd_sel_matches` `:579-619`, `jd_query` `:623-649`)**
- The selector is parsed onto the end of the browser's sheet with `css_parse_selector`, then `nsels`/`used`/`overflowed` are restored.
- Commas separate a list, and a match on any item counts.
- `hover = -1` and no visited links, so `:hover` never matches.
- On the document the search covers **every node index**; on an element it walks the subtree with `dom_next`.
- Results are arrays (static snapshots). `querySelector` returns null when there is no match.

**Events**
- `nat_add_listener` `:248-265` stores `{node: this.host, type (≤ 23 chars), fn}` and **ignores the third argument**. Slots are never reused.
- `nat_remove_listener` `:267-279` zeroes `fn` when the fn pointer, node and type all match.
- The event object (`jd_event_object` `:283-294`) has `type`, `target` (null when the node is -1), `preventDefault()` (sets `jd_prevented`) and `stopPropagation()` (sets `jd_stopped`).
- **`jd_dispatch(node, type)` `:350-370`**:
  1. Reset `jd_prevented`, `jd_stopped`, `sig` and `steps`.
  2. Build the event object.
  3. For `at = node` up through its parents: run the attribute handler (`jd_run_attr` `:300-328`), then the listeners (`jd_fire` `:333-346`).
  4. Then the document listeners.
  5. Return `jd_prevented`.
- `jd_run_attr` builds `"(function(event){" + attr + "\n})"` in a static 4096-byte buffer, compiles it with `js_eval_text` **on every firing**, and calls it with `this` = the element. **The return value is ignored.**
- `jd_fire` snapshots the listener count, resets `steps` for each listener, and uses `this` = the element (**undefined for document listeners**). `stopPropagation` also stops the remaining listeners on the same node.
- Errors are noted and `sig` is reset to OK after each handler.

**Timers (`:672-744`, `jsdom_timers` `:894-914`)**
- `JD_TIMERS` 64, `JD_HZ` 100, which matches `timer_init(100)` at `kernel/main.c:459`.
- `jd_ticks_for` `:699-707` clamps the delay to 0–3,600,000 ms, rounds up to 10 ms ticks, and never returns fewer than 1 tick.
- The callback must be a function; a string of code returns 0 and is not scheduled. Extra arguments are ignored. IDs come from a counter that is never reset.
- `clearTimeout` and `clearInterval` are the same native.
- On each pass `jsdom_timers` runs every due timer with `sig = OK`, `steps = 0` and `this = undefined`. A repeating timer is rebooked from now. It returns the number of timers that ran.

**XMLHttpRequest (`:746-885`)**
- `new XMLHttpRequest()` (`nat_xhr_new` `:830-842`) decorates `this` with `readyState = 0`, `status = 0`, `responseText = ""`, `open`, `send` and `setRequestHeader`.
- `open(method, url)` sets `__method__`, `__url__` and `readyState = 1`; the async argument is ignored.
- `setRequestHeader` is accepted and dropped.
- `send(body)` stores `__body__ = String(body)` (unless null or undefined) and queues the request in one of `JD_REQUESTS` = 8 slots; with no free slot the request is **silently not sent**.
- `jsdom_requests` `:847-885` performs **one** request per browser pass through `jd_do_request` (`browser.c:636-655`). It then sets `status`, `readyState = 4` and `responseText`, and calls **`onload` only**, with `this` = the xhr and no arguments, even when the request failed (status 0).

**World lifecycle**
- `jd_page_scripts` `:1109-1120` returns true if the page has any `<script>` element, or any attribute whose name starts with "on" and has a third character.
- `jsdom_open(d, sheet)` `:1139-1213`:
  1. `jsdom_close`.
  2. Return 0 if there is nothing to run.
  3. `js_init`, then install the hooks.
  4. Allocate `jd_wrap` and zero it.
  5. Create `document`, then the globals `setTimeout`, `setInterval`, `XMLHttpRequest`, `clearTimeout`, `clearInterval`, and `window = global_obj` (the global variable object itself).
- `jsdom_close` `:1125-1137`: clears the request table, then `js_done` and resets the pointers and counts.
- `jsdom_scripts(err, cap)` `:1240-1289`:
  1. For each `T_SCRIPT` element in index order (`d->count` is re-read on every iteration): if it has `src`, fetch it via `jd_get_script` and ignore any inline text; otherwise use its first text child.
  2. `js_run` each one and note the first error.
  3. The `type` attribute is **never checked**.
  4. Return the number of scripts run.
- `jsdom_loaded` `:1293-1297` dispatches `DOMContentLoaded` and then `load` with node -1, which reaches **document listeners only**.
- `jsdom_click(node)` `:1301`, `jsdom_live` `:1303`, `jsdom_changed` (read and clear) `:1308-1312`, `jsdom_error` `:1314`.
- `jsdom_request_with` `:781` and `jsdom_fetch_with` `:1229` are the only non-static functions.

### 3.6 `userland/jstest.c` (448 lines)

- `expect(what, src, want)` `:24-60` runs `js_init`, then `js_eval_text` on one expression, then compares `js_to_str(result)` with `want`. `script(what, src, want)` `:64-101` runs `js_run` and reads the global `result`. Every case uses a fresh `jctx`.
- Three refusal blocks follow (`:264-300`): `class Foo {}` must set an error; `var x = ;` must fail; `while (true) {}` must hit the step cap.
- The summary line "N of M passed" is printed at `:302-306`, **before** the last 58 cases (regex 41, arrows 8, instanceof 6, labels 3), so it reports 86.
- The final marker is `JSTEST_PASS` or `JSTEST_FAIL` (`:446`), and the program returns the failure count. Total: 144 cases.
- Several expected strings encode the engine's non-standard number printing: `"1 / 3"` → `"0.3333333333"` `:108` and `"0.1 + 0.2"` → `"0.3"` `:109`. The latter only passes because printing truncates (real JS prints `0.30000000000000004`).

### 3.7 `userland/jsprobe.c` (226 lines)

Usage: `exec /bin/jsprobe <url>` (example `https://www.google.com/`, `:13`).

1. `url_parse`, then `web_get` into a 512 KB static buffer, then `dom_parse` into a static `ddoc`.
2. Print the HTTP status and body size, and node counts (elements, divs, links, text runs, characters, and whether the document overflowed).
3. Print every `meta http-equiv`/`content` pair and every link href (≤ 160 characters).
4. Print up to 400 characters of text outside `<script>`/`<style>`, and the largest text run with its parent tag.
5. For each **inline** script (`src` scripts are counted but skipped), run it in a **fresh bare `jctx`** -- no `document` or `window` -- and print its index, size, error line and error.
6. Print totals, then `JSPROBE_DONE`.

What it probes is how far each inline script gets on the engine alone: DOM-free failures such as missing language features or built-ins. Any script that touches the DOM stops at its first `document`/`window` access. No tool runs it. It is embedded as `/bin/jsprobe` (`kernel/builtin.S:243-247`, `kernel/builtin.c:103`); jstest is embedded at `builtin.S:49-53`, `builtin.c:75`.

---

## 4. Control flow and lifecycles

### 4.1 Standalone (jstest, jsprobe)

1. `jctx J;`
2. `js_init(&J)`
3. `js_run` (a whole program) or `js_eval_text` (one expression). The result is in `J.sig`, `J.error`, `J.error_line`.
4. `js_done(&J)`

### 4.2 Browser page lifecycle

`browser.c:769-814` (`build`):
1. `jsdom_close()`.
2. `dom_parse`, then CSS (UA sheet, accent, inline sheets, linked sheets, inline styles, index), then pictures.
3. `jsdom_open(&doc, &sheet)`. Only if that returns 1: `jsdom_fetch_with(fetch_script)` (`browser.c:611-624`: relative to the page, `SCRIPT_MAX` 128 KiB, `SCRIPTS_MAX` 8), `jsdom_request_with(do_request)` (`browser.c:636-655`: `ASKS_MAX` 64, `REPLY_MAX` 128 KiB, a method starting with 'P' or 'p' means POST, so PUT and PATCH are sent as POST), then `jsdom_scripts`, then `jsdom_loaded`, then `jsdom_changed`.
4. `relayout`.
5. The status line gets ", a script stopped: …" or "N scripts ran (M from files)" (`browser.c:960-981`).

Main loop:
- `if (jsdom_live() && jsdom_timers() && jsdom_changed()) relayout` (`browser.c:1487-1490`)
- `jsdom_requests()` likewise (`browser.c:1495-1498`)
- On a mouse release over a node: `stop = jsdom_click(node)`; relayout if changed; report the first handler error once; if `stop`, the click is consumed (the link is not followed and controls do not get it) (`browser.c:1602-1619`).

The only events the browser ever produces are `click`, `DOMContentLoaded` and `load`. There are no keyboard, input, change, submit, focus or mouseover events.

Leaving the page (the next `build`) calls `jsdom_close`, which frees everything.

### 4.3 Evaluating a call `f(a, b)` (a JS function)

1. `N_CALL`: evaluate the callee (and `self` for a member call), then evaluate up to 24 arguments.
2. `js_call`:
   1. Check the depth.
   2. Look up `__this__` on `f`, which allocates the string `"__this__"`.
   3. Make a scope (176 bytes).
   4. Declare the parameters (48 bytes each).
   5. Build `arguments` (a 160-byte array, 128 bytes of items, plus the name and property).
   6. `js_exec(body)`: this hoists the body block's functions and runs its statements.
   7. On `JS_RETURN`, fetch `ret` and reset `sig`.

### 4.4 Completion signals

`J->sig` holds the current completion. Every evaluator path checks it after a sub-evaluation, and `js_tick` refuses to go on while it is not OK.

| Signal | Set by | Consumed by |
|---|---|---|
| `JS_RETURN` | `N_RETURN` | `js_call` |
| `JS_BREAK` / `JS_CONTINUE` | `N_BREAK` / `N_CONTINUE` | loops (label-aware), for-in and switch (label-blind), `N_LABEL` (break only) |
| `JS_THROWN` | `js_throw`, `N_THROW` | `N_TRY` (catch); `js_run` returns 0 |
| `JS_FAILED` | `js_alloc` cap, `js_tick` cap, parse errors | nothing inside the language; `js_run` returns 0. jsdom resets it to OK before every callback |

### 4.5 Labels protocol

1. `N_LABEL` sets `pending_label`.
2. The next loop to **start** takes it as `mine` and clears it.
3. `N_BREAK lbl` / `N_CONTINUE lbl` set `J->label`.
4. A loop consumes the signal if `js_label_mine`: `J->label` is NULL, or equal to `mine`.
5. After its statement, `N_LABEL` catches `JS_BREAK` carrying its own name.

Only loops clear `pending_label`, so a labelled non-loop lends its label to the first loop inside it (§10 B4).

### 4.6 Event dispatch

`jsdom_click(n)` → `jd_dispatch(n, "click")`:
1. Build the event object.
2. For `n` up through each ancestor: the `on<type>` attribute, compiled and called; if `stopPropagation` was called, stop; otherwise every listener for `(node, type)`.
3. Then `(JD_DOCUMENT, type)` listeners.
4. Return whether `preventDefault` was called.

### 4.7 Timer lifecycle

`setTimeout`/`setInterval` fill the first free slot (`fn == 0`), or grow `jd_ntimer` up to 64. The due time is `ticks() + t`. `jsdom_timers` runs each due timer: a one-shot clears its `fn`, a repeating timer is rebooked `every` ticks from now.

### 4.8 XHR lifecycle

`readyState` goes 0 (constructed) → 1 (`open`) → queued at `send` → on a later pass (one request per pass) → 4, with `status` and `responseText` set → `onload()`.

### 4.9 Regex use

Every method call:
1. `js_rx_load` reads `source` and `flags`.
2. `rx_compile` into `js_rx`.
3. `rx_search` from the start position.
4. Captures are read from `js_rx`.
5. A JS replacement callback may clobber `js_rx`, so `nat_str_replace_re` recompiles afterwards and restores `cap[0]`.

---

## 5. Interfaces

### 5.1 Exported by the engine (static header functions) and who uses them

| Function | Used by |
|---|---|
| `js_init`, `js_run`, `js_done` | jstest.c, jsprobe.c, jsdom.h |
| `js_eval_text` | jstest.c, jsdom.h (`jd_run_attr`) |
| `js_to_str`, `js_find`, `js_str` | jstest.c |
| `js_get`, `js_set`, `js_declare`, `js_call`, `js_object`, `js_native`, `js_array`, `js_arr_set`, `js_arg`, `js_to_num`, `js_to_bool`, `js_str_is`, `js_str_n`, `js_from_*` | jsdom.h |

**jsdom.h API used by browser.c and pagetest.c:** `jsdom_open`, `jsdom_close`, `jsdom_scripts`, `jsdom_loaded`, `jsdom_changed`, `jsdom_click`, `jsdom_live`, `jsdom_timers`, `jsdom_requests`, `jsdom_error`, `jsdom_outside`, `jsdom_outside_failed`, `jsdom_fetch_with`, `jsdom_request_with`. pagetest.c uses no requests and no outside counters. The coordinator's build reports those three functions (`jsdom.h:847`, `:1237`, `:1238`) as unused-function warnings in pagetest.c, and `jsrun.h:2557` `js_eval_text` as unused in jsprobe.c.

### 5.2 Hooks

- `jctx.host_get` / `host_set`, set by jsdom to `jd_host_get` / `jd_host_set`.
- `js_print_hook` (never set by any program).
- `jd_get_script` (`jsdom_fetch_with`).
- `jd_do_request` (`jsdom_request_with`).
- `jctx.host_data` (unused).

### 5.3 Dependencies

- `sdk/zelr.h`: `memset(void*,int,int)`, `strlen`, `puts`, `putc`, `putn`, `ticks()` (`SYS_TICKS`, `zelr.h:230`).
- `userland/alloc.h`: `malloc`, `free`, `realloc` (userland first-fit heap).
- jsdom.h additionally needs:
  - `dom.h` (`DOM_NODES` = 20000 `:37`, `dom_by_id`, `dom_attr`, `dom_attr_set`, `dom_append`, `dom_insert_before`, `dom_unlink`, `dom_create_element`, `dom_new`, `dom_str`, `dom_set_text`, `dom_text_content`, `dom_tag_name`, `dom_next`);
  - `css.h` (`css_parse_selector` `:479`, `css_matches` `:977`, `css_space` `:435`, and the `csheet` fields `nsels`/`used`/`overflowed`);
  - `html.h` (`html_tag_of` `:79-88`, `HTML_TAGS` `:65-77`);
  - `web.h` (`w_same` `:41-44`, exact case-sensitive comparison).
- Build: `userland/build.sh` compiles each `.c` file on its own with `-O2 -std=gnu11 -fno-builtin -fno-sanitize=undefined -mcmodel=large -mno-red-zone -Wall -Wextra`.
- Runtime: the user stack grows on demand up to `USER_STACK_MAX` = 1 MiB (`include/user.h:104`); a deeper stack faults and kills the process.

---

## 6. Concurrency, locking, memory ownership, invariants

### Threads and reentrancy

The engine is single-threaded ring-3 code with no locks. Several statics are **not reentrant**:
- `js_rx` (compensated for in replace callbacks only);
- `nat_str_replace_re`'s `out[16384]` (§10 B10);
- `jd_run_attr`'s `wrapped[4096]`;
- `jd_host_get`'s `textContent`/`value` buffers `buf[4096]`;
- `jd_class_write`'s `out[512]`;
- `jd_prevented` / `jd_stopped` (one event at a time);
- `js_rand_state`.

No path today dispatches an event from inside a script (there is no `el.click()`), so there is no nested dispatch.

### Ownership

| What | Where it lives | Freed |
|---|---|---|
| Values, strings, objects, scopes, element wrappers, `jd_wrap` | the region (`jctx.chunks`, `malloc`'d 256 KiB chunks) | only by `js_done` |
| AST | `J->nodes` (`realloc`'d heap) | only by `js_done` |
| DOM nodes, attributes, strings a script wrote | the document's own static arenas | when the page is next parsed |
| Listener, timer and request tables | statics holding `jobj*` pointers into the region | counts reset in `jsdom_close`/`jsdom_open`, so no stale pointer is used |

### Invariants and pitfalls

1. **Node indices are stable, but `jnode*` pointers are not.** Any `js_node` call can `realloc` the array.
   - `js_eval`/`js_exec` hold `jnode *n` across recursion (`jsrun.h:1481`, `:1780`). This is safe only because nothing parses during evaluation today.
   - Adding `eval`, `new Function`, synchronous script insertion, or compiling an attribute handler from inside a running script would create use-after-free bugs.
   - The parser writes `J->nodes[n].a = js_parse_x(P)` (§10 B19).
2. Nodes must outlive every function that references them, so they are never freed per script.
3. The region never shrinks. Everything the interpreter does allocates, and the 24 MiB cap is a **lifetime budget per page**, not a live-memory limit (§7).
4. After an allocation fails, `js_str` returns NULL, and several callers pass that straight to `js_find` (§10 B8). The code is not safe to run on after the cap is reached, yet jsdom keeps calling into it.
5. Wrapper identity comes from `jd_wrap[node]`.
6. `enumerable` is always 1. The internal properties (`__ctor__`, `__this__`, `__fn__`, `__bound__`, `__method__`, `__url__`, `__body__`) are ordinary visible properties.
7. `global_obj == global->vars == window`. Built-in globals are ordinary variables and a page can overwrite them.
8. Strings are never mutated after creation; natives copy first (`nat_str_upper`/`nat_str_lower` modify their own copy and rehash it).

---

## 7. Limits and magic numbers

| Name / value | Where | Meaning |
|---|---|---|
| `JS_MEM_CAP` 24 MiB | `js.h:61` | Region bytes per context over its whole life. At the cap: `JS_FAILED`, "this script asked for more memory than a page is allowed" |
| `JS_STEP_CAP` 40,000,000 | `js.h:62` | Evaluator steps per `js_run`, per listener, per timer callback, per onload. An attribute handler shares its dispatch's budget |
| `JS_DEPTH_CAP` 160 | `js.h:63` | Nested JS function calls; "too many nested calls" (catchable) |
| `JS_CHUNK` 256 KiB | `js.h:224` | Region chunk size ("a quarter megabyte", `jsdom.h:37`) |
| `JS_BUCKETS` 8 | `js.h:337` | Initial buckets; rehash at 3/4 load |
| `JS_NODES_STEP` 512 | `jsparse.h:446` | Initial AST node capacity (48 B per node) |
| `JS_ARGS_MAX` 24 | `jsrun.h:509` | Arguments evaluated per call and passed by apply; extras are dropped unevaluated |
| `JS_ARROW_PARAMS` 16 | `jsparse.h:857` | Maximum arrow parameters |
| `error[192]`, `jd_err[128]` | `js.h:211`, `jsdom.h:58` | Error text buffers |
| 80 bytes | `jsparse.h:508` | "expected X, not …" buffer |
| 96 bytes | `jsrun.h:393`, `1549`, `1621` | Runtime message buffers (names truncated) |
| 63 characters | `jsrun.h:467`, `498` | Property name passed to the host hooks |
| 100,000 | `jsrun.h:2426` | `Array(n)` cap (silent) |
| 4096 | `jsrun.h:1057` | `repeat` cap (silent) |
| 16,384 | `jsrun.h:985` | Regex replace output (silently truncated) |
| ±1e18 | `jsrun.h:226` | ToInt32 domain (outside it the result is 0) |
| 1024 | `jsrun.h:2191` | `Math.pow` exponent limit |
| 60 | `jsrun.h:2138` | sqrt Newton iterations |
| `0x5A4C5200` | `jsrun.h:2206` | `Math.random` seed |
| 1.7e308 | `jsrun.h:59` | Printed as Infinity above this |
| 2^53, 1e21, 1e-6, 10 digits | `jsrun.h:70`, `83`, `124` | Number printing thresholds |
| `RX_NODES` 512, `RX_CLASSES` 64, `RX_CONTS` 512, `RX_CAPS` 10, `RX_STEPS` 400,000, depth 24 | `jsregex.h:42-46`, `416` | Regex limits (repetitions per attempt ≲ 511) |
| `JD_DOCUMENT` 0x1000000, `JD_CLASSLIST` 0x2000000 | `jsdom.h:48`, `463` | Host number spaces |
| `JD_WRAPS` = `DOM_NODES` = 20000 | `jsdom.h:63`, `dom.h:37` | Wrapper table (160,000 B from the region) |
| `JD_LISTENERS` 256, `JD_TYPE_MAX` 24 | `jsdom.h:219-220` | Listener registrations per page life; event type length |
| `JD_TIMERS` 64, `JD_HZ` 100, 3,600,000 ms | `jsdom.h:685-686`, `701` | Timers |
| `JD_REQUESTS` 8 | `jsdom.h:766` | Pending XHRs |
| 4096 B | `jsdom.h:311`, `943`, `974` | Attribute handler wrapper; `textContent` read (4095 characters) |
| 512 B | `jsdom.h:478` | classList rewrite (511 characters) |
| `SCRIPT_MAX` 128 KiB, `SCRIPTS_MAX` 8, `REPLY_MAX` 128 KiB, `ASKS_MAX` 64 | `browser.c:66-77` | Browser-side script and XHR limits |
| `USER_STACK_MAX` 1 MiB | `include/user.h:104` | C recursion ceiling |

**What 24 MiB buys.** These figures are derived from struct sizes and `js_alloc`'s 16-byte rounding. None of this memory is ever reclaimed during a page's life.

| Operation | Region cost |
|---|---|
| One JS function call | about 624 B before the body allocates anything: `"__this__"` 32 + scope 176 + 48 per parameter + `arguments` 160 + items 128 + name 32 + property 48 |
| One built-in method call (`s.charAt(i)`, `a.push(x)`) | about 304 B |
| `a[i] = v` | 16–32 B for the index string |
| Array growth | about 32 B per final element (abandoned doubling copies) |
| One element wrapper | about 3.2 KB, once per node |
| Each `el.classList` read | about 1.3 KB |
| Each dispatched event | about 0.9 KB |
| Each regex literal evaluation | about 1.2 KB |
| `JSON.stringify` | about S²/2 bytes for S bytes of output (a character at a time) |
| `join`, and `+=` string building | quadratic |

What that means in practice:
- About 40,000 JS function calls, or about 80,000 built-in method calls, per page.
- Sorting a few hundred elements with a JS comparator: about n²/4 calls, so roughly 400 elements exhaust the region.
- `JSON.stringify` output of about 7 KB.
- Building a string of about 30 KB with `+=` in 20-byte pieces.
- A `setInterval` that runs every 10–100 ms exhausts the region in minutes to hours, and the next callback then crashes the browser (§10 B8).

---

## 8. Tests

### 8.1 jstest.c -- 144 cases, fresh context per case

Run by `tools/ring3check.py` (suite entry `("jstest", "JSTEST_PASS", "javascript", 180)` at `:33`). It checks the marker, runs jstest a second time, parses "N of M passed" and requires M ≥ 86 (`:84-97`). That line is printed before the last 58 cases, so the count guard only covers the first 86 (§10 D16). `ring3check.py` runs on every gate (`pipeline/gate.sh:302-303`). The kernel selftest (`-append selftest`) covers **none** of this area, because it is all ring 3 (`gate.sh:296-301`).

| Group | Count |
|---|---|
| Numbers (doubles) | 7 |
| Plus vs minus | 3 |
| Comparison and equality | 7 |
| Truthiness | 5 |
| Bitwise | 3 |
| Strings (length, index, slice, upper, split/join, replace, indexOf, escape) | 8 |
| Arrays (literal, join, map, filter, sort ×2, String(arr)) | 7 |
| Objects | 4 |
| typeof | 5 |
| Math and JSON (floor, round, sqrt, max, pow, stringify, parse, round trip, parseInt, Number) | 10 |
| Functions and closures (return, hoisting, closure capture, independent closures, recursion, arguments) | 6 |
| Statements (for, while/break, continue, do, for-in object, for-in array, switch fallthrough/default, throw/catch, finally, throw across a call) | 11 |
| Assignment and mutation (compound, post/pre inc, reference, push, delete + keys) | 6 |
| A small program (`new`, `this.items`) | 1 |
| Refusals (class, bad syntax, infinite loop → step cap) | 3 |
| Regex (divide vs pattern, test, i, `\d`, classes, alternation, empty branch, `{}` quantifiers, literal brace, greedy/lazy, `\b`, exec groups/index/unmatched group, replace `$n`/`$&`/g/function, match g/groups/null, search, split, trim idiom, m flag, RegExp ctor, exec loop with lastIndex, empty-match replace) | 41 |
| Arrow functions (incl. lexical `this`, curried, paren disambiguation) | 8 |
| instanceof | 6 |
| Labelled break/continue in nested `for` | 3 |

### 8.2 pagetest.c -- 71 checks, 54 of them on jsdom

Lines `:156-635`. Run by `ring3check.py` (`:36`, `PAGETEST_PASS`, 120 s) and `tools/piccheck.py` (`:39`). What it covers:
- A script writes `textContent`, `className` and `document.title` and computes a sum.
- `getElementsByTagName` count.
- A missing element and unparsable script are reported.
- A page with no script opens no world.
- An onclick attribute fires on click and sets the dirty flag.
- Listener bubbling, with the closure state surviving.
- `event.target`.
- `preventDefault` via an attribute, and the default not prevented otherwise.
- `stopPropagation`.
- `DOMContentLoaded`.
- A throwing handler is reported and the world stays live.
- `createElement` + `appendChild`, `removeChild`, `insertBefore`, move semantics, cycle refused.
- classList add/remove/contains/toggle.
- `parentNode.id` + `children.length`.
- `setTimeout` (not before it is due, runs once), `setInterval` (runs at least twice), `clearTimeout`.
- `value` read/write and `checked` write.
- `querySelector`/`querySelectorAll` (compound selectors, subtree root, selector lists, 400 repeats not exhausting the sheet).

### 8.3 Screen-based checks (need QEMU)

- `tools/browsercheck.py:359-371`: `/scripted` against `/unscripted` (`tools/webserver.py:134-154`, `:221`). The script adds a class that paints a band, and the check counts that band's colour.
- `tools/livecheck.py`: `/live` (`webserver.py:164-205`), compared with the `/live-quiet` control.
  - A timer turns a band orange.
  - An external `/live.js` turns one violet.
  - An XHR, with `onload` assigned **after** `send()`, turns one teal.
  - A click on a listener turns one green.
- Both run in the extended gate (`gate.sh:496`, `:502`).

### 8.4 jsprobe

A manual diagnostic only. It prints `JSPROBE_DONE` and asserts nothing.

### 8.5 Not covered by any test (from reading the tests)

- `call`/`apply`/`bind` (broken, B1);
- `for (x in o)` without `var` (broken, B2);
- `new X().y` (B3);
- labelled blocks, labelled for-in, labelled break through a switch (B4);
- printing everyday decimals such as `0.57` (B5);
- `Math.min()`, and floor/round of NaN;
- `replaceAll`, `substring`, `substr`, `indexOf` with a position;
- JSON escapes, key order, undefined;
- `Object.values`, `Array.isArray`, `parseFloat`, `isNaN`, `String`/`Number`/`Boolean`;
- the memory-cap and depth-cap paths;
- `removeEventListener`, `remove()`, `createTextNode`, `getAttribute`/`setAttribute` from script;
- sibling and `firstChild`/`lastChild` navigation, `tagName`;
- listener ordering;
- XHR outside livecheck.

---

## 9. How to extend

### Where each kind of addition goes

- **A String or Array method.** Add `{ "name", nat_fn }` to the `M[]` table in `js_string_method` (`jsrun.h:1100-1112`) or `js_array_method` (`:1272-1280`) and write `static jval nat_fn(jctx *J, jval t, jval *a, int n)`. `t` is the receiver only when the method is called as `x.m(...)`, because `N_CALL` substitutes `__this__` (`:1595-1598`). When a native calls it through `js_call` directly (for example as a forEach callback), `t` is whatever was passed.
- **A global or namespace member.** Add it in `js_globals` (`jsrun.h:2449-2517`) with `js_declare(J, g, js_str(J,"X"), ...)`, or `js_set` it onto the Math/JSON/Object object. Browser-only globals go in `jsdom_open` (`jsdom.h:1191-1211`).
- **Number methods** (`toFixed`, `toString`). `js_get` returns undefined for any non-object primitive (`jsrun.h:424-426`). Add a `js_number_method` in the style of `js_string_method` and call it there.
- **Methods that need their own object** (`call`/`apply`/`bind`, and anything like them).
  - Best fix: give `jnative` a callee parameter.
  - Minimal fix: have `js_call` (`:520`) and `N_CALL` pass the native object itself (or its `__fn__` holder) as the callee. Moving the `__this__` substitution from `N_CALL` into `js_call` would also fix natives called from other natives.
- **A DOM property.** Add a `w_same(name, "...")` branch in `jd_host_get` (`jsdom.h:921`) and/or `jd_host_set` (`:1046`) and return 1. Every name that `jd_host_get` handles and `jd_host_set` does not will make script writes silently invisible (for example `id`), so add both sides. Prefer handing out methods lazily from `host_get` over adding more own properties in `jd_element`, which costs about 272 B per method per wrapper.
- **A new event type.** Add a `jsdom_<event>(node)` wrapper around `jd_dispatch` and call it from browser.c's input handling.
- **`on*` properties.** In `jd_dispatch`, also look up an own function-valued property `"on"+type` on each node's wrapper; for `load`, on `window`.
- **A syntax feature.**
  1. Add a token if needed (`js_next` punctuation, `jsparse.h:383-441`).
  2. Add a node kind (`js.h:159-170`).
  3. Add the parser production.
  4. Add a `js_eval`/`js_exec` case.
  5. Remove the word from `JS_UNSUPPORTED` if it is there.
  6. For a keyword that can end an expression (for example `super`), update `js_tok_ends_expr`.
  7. Add a jstest case. `expect` takes a single expression; wrap statements in an IIFE, or use `script` with a global `result`.
- **Fixing for-in without `var`.** Add a "no in" flag to `js_parse_binary`/`js_parse_expr` for the for-init position.
- **Fixing `new`.** Parse the callee as a member expression without calls, then take at most one argument list.

### Pitfalls the comments and code point to

- **Memory.** Every feature allocates from a region that is never reclaimed, so per-call allocations multiply over a page's life. Avoid calling `js_str()` for fixed keys on hot paths: `js_call`, `N_CALL` and `fn_held` each make a new `"__this__"`/`"__fn__"` string per call. Intern these once in `jctx` instead.
- **Check for NULL.** Always check `js_str`/`js_array`/`js_object` for NULL before passing the result to `js_find`/`js_arr_push` (B8).
- **Never parse while evaluating** unless you stop holding `jnode *` across calls (§6).
- **Scratch state.** A native that calls back into JS must assume `js_rx` and any static buffer were clobbered.
- **Property visibility.** Internal properties are visible to `for-in`, `Object.keys` and `JSON.stringify`. Either mark them non-enumerable (and make keys/values/stringify honour the flag), or keep them in `jobj` fields.
- **The step cap only counts evaluator steps.** Natives, regex matching and array growth do not tick, so a long native loop is invisible to it.
- **Build hygiene.** Everything is `static` in headers, so a program that includes a header without using every function gets `-Wunused-function` warnings. The coordinator's build shows these in jsprobe.c (`js_eval_text`) and pagetest.c (`jsdom_requests`, `jsdom_outside`, `jsdom_outside_failed`).

---

## 10. Doc drift and suspicious code (verified by reading; traces spelled out)

### Bugs

**B1. `call`, `apply` and `bind` do not work.** This is the headline bug of the most recent JS commit.
- `js_get` (`jsrun.h:450-460`) returns a wrapper native `W` with `W.__fn__ = f`.
- `N_CALL` then calls `js_call(W, self = f, …)`. `W` has no `__this__`, so `self` stays `f` (`:1581`, `:1595-1598`).
- `js_call` invokes `nat_fn_call(J, t = f, …)` (`:520`).
- `fn_held(J, t)` looks up `f.__fn__` (`:573-577`), which does not exist, so the result is undefined.
- `js_call(undefined, …)` then throws **"this is not a function"** (`:514`).
- `apply` fails the same way. `bind` returns a function whose `__fn__` is undefined, and `nat_fn_bound` reads `__fn__`/`__bound__` from the receiver at call time (`:605-613`), so calling the bound function throws too.
- Only `f.call.call(g, x)` works -- one level off.
- No test covers any of this. The comment at `:438-449` says Google's page needed `call`.

**B2. `for (k in o)` without `var`/`let`/`const` does not parse.** In `js_parse_stmt`'s for branch, `init = js_parse_expr(P)` (`jsparse.h:1221`) lets `js_parse_binary` consume `in` as a binary operator (`:809`). The check `js_at_word(P,"in")` at `:1222` is therefore never true, and parsing ends with "expected ;, not )". The `N_FORIN.c` target form (`:1229`, and `jsrun.h:1906-1909`) is dead code. Minified code uses `var k; for (k in o)` often.

**B3. `new X(args).more` is mis-parsed** (`jsparse.h:759-774`). The operand is parsed with the full postfix loop, so `new Foo().bar()` becomes `new (Foo().bar)()`: `Foo` is called without `new` (with `this` undefined), then `new` is applied to the result's `bar`. For example, `new RegExp("a").test("xyz")` becomes `new (RegExp("a").test)("xyz")`, which returns the fresh plain object (truthy) instead of `false`.

**B4. Label handling has three holes.**
- (a) `N_LABEL` (`jsrun.h:1937-1947`) on a block or `if` leaves `pending_label` set, and the first loop inside takes it (`:1815`, `:1835`, `:1854`). `lbl: { for(;;){ break lbl; } after(); }` then runs `after()`.
- (b) `N_FORIN` (`:1878-1916`) never takes `pending_label` and consumes every break/continue whatever its label. `lbl: for (k in o) { for(;;) continue lbl; }` hands the label to the inner `for`, which loops until the step cap. `break lbl` exits only the inner loop.
- (c) `N_SWITCH` (`:2011`) swallows `break outer` inside `outer: for (...) { switch (x) { case 1: break outer; } }`.

**B5. Number printing is wrong for everyday decimals** (`jsrun.h:111-133`). The code emits 10 fraction digits by **truncation**, so doubles that sit just below a decimal print as a run of 9s: `0.57` → `"0.5699999999"`, `19.99` → `"19.9899999999"`, `1.15` → `"1.1499999999"`. For 2^63 ≤ |d| < 1e21, `(long long)d` overflows (cvttsd2si gives INT64_MIN) and the digit loop emits non-digit bytes (for example `String(1e20)`). Values between 1.7e308 and DBL_MAX print as "Infinity" (`:59`). Parsing is also not correctly rounded (the literal `0.3` becomes `3*0.1`), so `0.1 + 0.2 === 0.3` is **true** here.

**B6. Math edge cases.**
- `Math.floor`/`ceil`/`round` of NaN, ±Infinity or |x| ≥ 2^63 return -9223372036854775808 (`jsrun.h:2122-2130`: an undefined-behaviour cast, which becomes cvttsd2si in practice). Printing that value then hits B5.
- `Math.min()` returns NaN (`:2166`: `1.0/0.0*0.0` is NaN) instead of Infinity.
- `Math.sqrt` is wrong outside roughly [1e-36, 1e36] (60 Newton iterations from `max(x, 1)`; `:2137-2143`).
- `Math.min`/`max` ignore a NaN that is not the first argument.

**B7. Array index and length can corrupt the heap or hang the browser** (`js.h:424-441`, `jsrun.h:487-491`).
- If `i+1` is in (2^27, 2^31], `(u32)(16*cap)` wraps to 0, so `js_alloc(0)` returns a zero-length block while `cap` becomes huge. The fill loop then writes past the chunk: `var a=[]; a[150000000]=0` corrupts the region or faults the process.
- If `i+1 > 2^31`, `cap *= 2` wraps to 0 and `js_arr_reserve` loops forever (natives do not tick).
- If `i = 4294967295`, `i+1` wraps to 0 and the guard is bypassed.
- `a.length = N` reaches the same code for the same ranges. Growing `length` never updates `len`.
- `js_index_of` accepts up to 10 digits and wraps indices modulo 2^32.

**B8. After the memory cap, NULL dereferences crash the browser.**
- `js_str` returns 0 at the cap, and `js_find` dereferences `key->hash` (`js.h:353`).
- Unchecked call sites: `js_call` `jsrun.h:524` (the first thing every JS-function call does), `N_CALL` `:1596`, `instanceof` `:1446`, `js_rx_load` `:671-672`, `fn_held` `:575`, `nat_re_exec` `:725`, and `jd_prop_str` → `js_get` → `jd_host_get` (`name->len`).
- `js_call`'s `arguments` array can be NULL and is passed to `js_arr_push` (`:542-543` → `js.h:425`).
- jsdom resets `sig` to OK before every timer, listener and onload (`jsdom.h:339`, `356`, `877`, `907`). A long-running page that has used its 24 MiB therefore crashes the browser process on its next callback instead of reporting the error.

**B9. Unbounded C recursion overflows the 1 MiB stack.**
- `js_to_str` on a cyclic array recurses before allocating anything (`jsrun.h:252-265`): `var a=[1]; a[0]=a; ""+a`.
- `JSON.stringify` of a cyclic object (`:2217-2269`) keeps recursing once memory is exhausted, because `js_concat` returns the old string on failure.
- Parser recursion has no depth limit.

**B10. `nat_str_replace_re`'s `static char out[16384]` (`jsrun.h:985`)**
- Truncates results silently at 16,383 bytes.
- Is not reentrant: a replacement callback that itself does a regex replace overwrites the outer call's prefix.

**B11. String methods with wrong semantics.**
- `replaceAll` is mapped to `replace` (`jsrun.h:1108`), so with a string pattern it replaces only the first match.
- `substring` and `substr` are both `slice` (`:850-852`, `:1105`): `"hello".substring(3,1)` → `""` (JS `"el"`); `"hello".substr(1,3)` → `"el"` (JS `"ell"`).
- `indexOf`, `includes` and `startsWith` ignore their position argument.
- A string-pattern `replace` ignores `$&` and turns a function replacement into the text "function".

**B12. Coercions that differ from JS.**
- `+` on arrays is numeric (`jsrun.h:1370-1374`): `[1,2]+3` → NaN, `[]+[]` → 0, `[1]+[2]` → 3.
- Loose equality between an object and a primitive compares strings (`:313-316`): `[1]==true`, `[0]==false` and `[]==false` are all false here.

**B13. Internal bookkeeping leaks into what scripts see.**
- `__ctor__` (on every object made by `new`), `__this__`, `__fn__`, `__bound__`, and the XHR `__method__`/`__url__`/`__body__` are enumerable own properties (`js.h:391`).
- `Object.keys`/`values` and `JSON.stringify` do not look at `enumerable` at all (`jsrun.h:2253-2261`, `2393-2395`, `2408-2410`).
- So `JSON.stringify(new Foo())` contains `"__ctor__":null`.

**B14. Key order is hash-bucket order**, not insertion order, for for-in, `Object.keys`/`values` and `JSON.stringify` (§3.1 has examples).

**B15. JSON problems.**
- `stringify`: emits raw `\r` and control characters, writes `undefined`/`NaN`/`Infinity` as text, and is O(S²) in region memory, so about 7 KB of output exhausts the page.
- `parse`: does not decode `\u`, never throws on malformed input, and accepts any word starting with t/f/n (`jsrun.h:2217-2376`).

**B16. Regex gaps.**
- About 511 iterations per quantifier per attempt: `RX_CONTS` frames are released only on backtrack (`jsregex.h:493-501`, `561-581`). Longer runs are silently truncated (`/\d+/`) or fail (`/^a*$/.test("a".repeat(600))` is false).
- `RX_STEPS` gives up silently with a non-match, and the budget is per start position, so a pathological pattern costs up to len × 400,000 steps that the JS step cap never sees (a browser stall).
- The flags u, s and y are dropped by the lexer (`jsparse.h:372`) and by `RegExp()` (`jsrun.h:780-784`), so `rx_compile`'s refusal (`jsregex.h:447-449`) is unreachable.
- Groups after the 9th silently stop capturing (`jsregex.h:292`).
- Negated classes never match `\n` (`:254-256`).
- `(?<name>` is refused with the misleading message "lookaround is not here".
- `\k` and `\c` match the literal letter.
- `test()` ignores `lastIndex` (`jsrun.h:711-715`).

**B17. jsdom gaps.**
- `el.onclick = fn` and `window.onload = fn` never fire: dispatch looks only at attributes and the listener table (`jsdom.h:350-370`).
- `window` (the global variable object, `:1209-1211`) has no `addEventListener`.
- `load` and `DOMContentLoaded` reach document listeners only; `<body onload>` never fires.
- `return false` from an `on*` attribute does not cancel the default action (`:326` ignores it).
- Capture listeners are registered and run at bubble time (`:248-265` ignores the third argument).
- `stopPropagation` also stops the remaining listeners on the same node (`:335`).
- Removed nodes, nodes orphaned by `textContent` (`dom.h:325-334` leaves their `parent` set) and created-but-uninserted nodes are still returned by `getElementById` (`dom.h:675-681`), `getElementsByTagName` (`jsdom.h:167-171`) and document-level `querySelector(All)` (`:632-638`), in index order rather than tree order (the comment at `:621-622` assumes index order is document order).
- A write to a name that host_get serves but host_set does not (for example `el.id = "x"`) goes to a shadowed JS property and is invisible. `el.innerHTML = …` silently does nothing.
- `tagName` is lowercase.
- `getElementsByTagName('*')`, or any tag not in the table, returns every `T_OTHER` element (`html.h:79-88`).
- `select.value` is `""` unless the element has a `value` attribute.
- `this` in document listeners is undefined.
- XHR: only `onload` is called; the async flag is ignored; a 9th pending request is silently dropped.
- Scripts run regardless of their `type` attribute (`:1248-1281`). JSON-LD, template and module scripts are executed, and their parse error becomes the page's only reported error.
- A script element created by an earlier script runs in the same pass even if it was never inserted.
- Attribute handlers are recompiled on every firing, so `J->nodes` grows on the heap outside the cap.
- classList objects (about 1.3 KB) and element wrappers (about 3.2 KB) are expensive.

**B18. Uninitialised token read.** `jparse P;` is not initialised, and the first `js_next` calls `js_tok_ends_expr(&L->tok)` on garbage (`jsparse.h:148`, `1371-1379`; `jsrun.h:2562-2570`). This is undefined behaviour. When the garbage type is `T_KEYWORD` or `T_PUNCT` with a matching length, it dereferences a garbage `text` pointer. Low probability.

**B19. Stores whose ordering depends on the compiler.** `J->nodes[n].a = js_parse_x(P)`, with `js_parse_x` able to `realloc` `J->nodes`, occurs at `jsparse.h:744, 750, 756, 784, 793, 882, 1016, 1046, 1105, 1262, 1289, 1297, 1306, 1308`. It works only because clang evaluates the right-hand side of a scalar assignment before the left-hand lvalue; C leaves the order unspecified. A hazard for any other compiler.

**B20. Silent truncation to 24 arguments.** Calls evaluate at most 24 argument expressions, so side effects in the rest are lost (`jsrun.h:1602-1606`, `1640-1644`). `apply` truncates its array to 24 (`Math.max.apply(null, big)` is wrong).

**B21. Poor diagnostics for errors.** Engine errors are thrown as strings, so `e.message` is undefined. `throw new Error("x")` reports "this is not a function" (Error is undefined, and `N_NEW` never names the callee) (`jsrun.h:1636-1654`).

**B22. var, let and const semantics.**
- `var x;` resets `x` to undefined (`jsrun.h:1799-1806`).
- There is no `var` hoisting, so assigning before the declaration from a nested function creates a global.
- `let`/`const` are not block-scoped, and `const` can be reassigned. The comment at `jsparse.h:1020-1022` admits this; the header at `js.h:17` implies full support.

**B23. `this` is undefined at top level and in plain calls** (`jsrun.h:2547`). UMD wrappers `(function(root){…})(this)` therefore fail with "cannot set X of undefined".

**B24. Dead or unused state.** `js_bound_this` (`jsrun.h:645`) is dead. `js_print_hook` (`:2034`) is never set, so console output is dropped everywhere. `host_data` is unused. The `rep == -1` path in `rx_cont_do` (`jsregex.h:490`) is unreachable.

**B25. break/continue placement is not checked.** A `break` inside a function body but outside any loop escapes `js_call` as `JS_BREAK` (`:551-558`) and breaks the caller's loop. Top-level `return` silently ends the script.

### Documentation drift

**D1.** `js.h:30` says "no regular expressions", and `jsparse.h:18-22` says "a slash is always division". Both are false: jsregex.h and the lexer's regex branch exist. The same stale claim is on the website (per the coordinator; the website is not in this tree).

**D2.** `js.h:67` says "Eight kinds"; `jtype` has 6.

**D3.** `js.h:88-91` says "Interned lengths"; there is no interning.

**D4.** `jsparse.h:5-6` says "sixteen levels" of binary operator; there are 10. The arrow-function refusal at `jsparse.h:413-419` is dead code.

**D5.** `jsrun.h:41-45` says "up to ten significant places"; the code prints ten fraction digits, truncated.

**D6.** `jsrun.h:567-571` says bind "says so rather than dropping them silently"; the bound arguments are dropped silently (and bind does not work at all, B1).

**D7.** `jsdom.h:46-47` and `dom.h:117` say "twelve thousand" nodes; `DOM_NODES` is 20000 (`dom.h:37`).

**D8.** `jsdom.h:211-217` and `README.md:772-774` say there is no capture phase because running a capture listener at bubble time is worse; that is exactly what happens (B17).

**D9.** `README.md:749-750` says "`test` and `exec` with `lastIndex`"; `test` ignores it. `README.md:731`'s "`onclick` and its relatives" applies to attributes only.

**D10.** `jsregex.h:19-23` and `js.h:33-36` promise "absent rather than approximated". Silent approximations remain: template literals, unknown regex flags, capture groups after the 9th, repetitions past about 511, `let`/`const`, 24-argument truncation, `Array(n)` capped at 100k, `repeat` capped at 4096, replace output capped at 16 KB, `innerHTML` writes, `el.onclick` properties.

**D11.** `jsdom.h:22-24`: "A missing property is better than one that returns undefined forever and lets a page believe it worked". Unknown-property writes silently succeed.

**D12.** `jsrun.h:2555-2556`: `js_eval_text` is "for … an event handler written in an attribute", but it ignores trailing tokens. `jd_run_attr` copes by wrapping the handler in a function expression.

**D13.** `js.h:15-23` ("what is here") omits arrows, regex, labels, `instanceof` and `switch`.

**D14.** `jsdom.h:621-622`: "for the whole document is index order" is false once scripts insert nodes.

**D15.** The README does not mention `call`/`apply`/`bind`. The code comment claims them (`jsrun.h:438-449`), and they are broken (B1).

**D16.** `jstest.c:302-306` prints the summary before 58 more cases, so the `total >= 86` check at `ring3check.py:96-97` guards only the first 86 of 144. Two expected results (`jstest.c:108-109`) encode non-standard printing.

---

## 11. Open questions

1. How long does `JS_STEP_CAP` take in wall-clock time on the target and in QEMU TCG? It is the only runaway guard for the browser UI loop, and natives and regex are not counted.
2. Was call/apply/bind ever exercised against Google's page after the change? There is no test, and by trace it cannot work (B1).
3. The kernel's handling of a ring-3 stack overflow past 1 MiB and of a NULL page fault: the browser should just die (per faulttest, "a program that faults, and a machine that does not"). Should jsdom instead refuse to run callbacks once `J->allocated` is near the cap?
4. With `-O2`, clang is free to optimise the undefined-behaviour float→integer casts (NaN or out-of-range `(long long)` and `(int)` in `js_floor`, `js_num_text`, `%`, `charAt`, `slice` and others). The analysis above assumes it emits cvttsd2si.
5. Is the lowercase `tagName` a deliberate choice, since `dom.h` stores lowercase names for CSS?
6. `jsdom_click` may receive a text node index from layout hit-testing; then `event.target` would be the text node's wrapper. Worth checking with the browser agent.
7. The website text that claims "no regex, no arrows" is not in this repository; its location and owner are unknown here.
