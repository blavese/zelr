/* Reading JavaScript: from characters to a tree.
 *
 * A lexer that hands out one token at a time, and a recursive descent parser
 * that turns them into the nodes declared in js.h. The expression grammar is
 * precedence climbing, which is the shortest correct way to get twelve
 * levels of binary operator right without twelve functions that all look
 * the same.
 *
 * Three things about JavaScript make this harder than it looks.
 *
 * Semicolons are optional, and the rule for where one is inserted is not
 * "wherever it would help": it is at a newline, at a closing brace, and at
 * the end of the input, and only when the next token could not continue the
 * statement. Getting that wrong turns `return` followed by a newline into a
 * return of whatever was on the line below, which is the one case everybody
 * knows about and the one this has to get right.
 *
 * A slash is either division or the start of a regular expression, and
 * which it is depends on what came before (jlex.ends_expr).
 *
 * And a bracket is an expression until an arrow follows it: `(a, b)` is a
 * comma expression and `(a, b) => a` is a function, and `{a, b}` is an object
 * until an `=` follows it and makes it a pattern. The lexer is one token
 * deep, so neither can be decided before it is read. Both are read as the
 * expression and turned into what they turned out to be afterwards
 * (js_to_pattern, js_arrow_params), which is how the standard itself
 * describes them; nothing is read twice.
 */
#pragma once
#include "js.h"
#include "jsnum.h"

typedef enum {
    T_EOF = 0, T_NUM, T_STRING, T_REGEX, T_NAME, T_PUNCT, T_KEYWORD,
    T_TEMPLATE, T_PRIVATE
} ttype;

/* Which flags a regular expression literal carried, as bits, because the
   node they end up on has one string on it and that is the pattern. */
#define RXF_I 1
#define RXF_G 2
#define RXF_M 4
#define RXF_S 8
#define RXF_U 16
#define RXF_Y 32
#define RXF_D 64
#define RXF_V 128

/* The operators, as one number each, so the parser can switch on them. */
typedef enum {
    OP_NONE = 0,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_LT, OP_GT, OP_LE, OP_GE,
    OP_EQ, OP_NE, OP_SEQ, OP_SNE,
    OP_AND, OP_OR, OP_NOT,
    OP_BAND, OP_BOR, OP_BXOR, OP_BNOT, OP_SHL, OP_SHR, OP_USHR,
    OP_ASSIGN, OP_ADDEQ, OP_SUBEQ, OP_MULEQ, OP_DIVEQ, OP_MODEQ,
    OP_OREQ, OP_ANDEQ, OP_NEG, OP_POS, OP_INC, OP_DEC, OP_IN, OP_ARROW,
    OP_INSTANCEOF,
    OP_POW, OP_NULLISH, OP_POWEQ, OP_XOREQ, OP_SHLEQ, OP_SHREQ, OP_USHREQ,
    OP_LANDEQ, OP_LOREQ, OP_NULLEQ, OP_SPREAD, OP_OPTCHAIN
} jop;

typedef struct {
    ttype  type;
    jop    op;
    double num;
    const char *text;         /* into the source, or a region copy for strings */
    u32    len;
    int    line;
    int    nl_before;         /* a newline came before this token */
    int    flags;             /* T_REGEX: which letters followed it; T_TEMPLATE:
                                 1 when it ended the template */
    const char *raw;          /* T_TEMPLATE: the text as written */
    u32    rawlen;
    int    bad_escape;        /* T_TEMPLATE: an escape a tag may see but a
                                 plain template may not */
    jstr  *str;               /* T_STRING: the string, made as it was read */
} jtok;

typedef struct {
    jctx  *J;
    const char *src;
    u32    n, at;
    int    line;
    jtok   tok;               /* the one being looked at */
    int    nl;                /* a newline has been passed since the last */
    int    failed;

    /* Whether the token just read could end an expression.
     *
     * This is the whole of how a slash is told apart from a regular
     * expression, and there is no other way to tell: `a / b` and `/ab/`
     * are the same three characters and differ only in what came before.
     * After a number, a name, a string or a closing bracket, a slash is
     * division; anywhere else it opens a pattern. */
    int    ends_expr;

    /* Set by the parser when it knows better than the token before: a
       value cannot start with a divide, so a slash there is a pattern, as
       after a block's closing brace -- catch(e){}/x/.test(s). */
    int    want_regex;
} jlex;

/* --- characters ---------------------------------------------------------- */

static int js_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == 11 || c == 12; }
static int js_digit(char c) { return c >= '0' && c <= '9'; }
static int js_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
        || c == '$' || (unsigned char)c >= 0x80;
}
static int js_alnum(char c) { return js_alpha(c) || js_digit(c); }
static int js_hexval(char h) {
    if (js_digit(h)) return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
}

/* The characters past ASCII that the language counts as space or as the end
   of a line, as the UTF-8 they arrive in: how many bytes, 0 when it is not
   one, and whether it ends a line. A page's script begins with a byte order
   mark often enough, and was refused for it. */
static int js_wide_space(const char *s, u32 left, int *newline) {
    const unsigned char *u = (const unsigned char *)s;
    *newline = 0;
    if (left >= 2 && u[0] == 0xC2 && u[1] == 0xA0) return 2;                 /* no-break */
    if (left < 3) return 0;
    if (u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) return 3;               /* BOM */
    if (u[0] == 0xE2 && u[1] == 0x80) {
        if (u[2] == 0xA8 || u[2] == 0xA9) { *newline = 1; return 3; }        /* LS, PS */
        if ((u[2] >= 0x80 && u[2] <= 0x8A) || u[2] == 0xAF) return 3;
    }
    if (u[0] == 0xE2 && u[1] == 0x81 && u[2] == 0x9F) return 3;
    if (u[0] == 0xE3 && u[1] == 0x80 && u[2] == 0x80) return 3;
    if (u[0] == 0xE1 && u[1] == 0x9A && u[2] == 0x80) return 3;
    return 0;
}

static void js_fail_at(jctx *J, int line, const char *what, const char *extra,
                       u32 extra_len) {
    if (J->sig == JS_FAILED) return;
    J->sig = JS_FAILED;
    J->error_line = line;
    int i = 0;
    for (const char *p = what; *p && i < (int)sizeof(J->error) - 1; p++)
        J->error[i++] = *p;
    if (extra) {
        for (u32 k = 0; k < extra_len && i < (int)sizeof(J->error) - 1; k++)
            J->error[i++] = extra[k];
    }
    J->error[i] = 0;
}

/* --- the keywords --------------------------------------------------------
 *
 * The ones that are refused are listed as well as the ones that are
 * understood, and refused by name. A script that uses `export` gets told
 * this engine does not have modules, which is a fact somebody can act on; a
 * parser that treated it as an identifier would produce a syntax error four
 * lines later about something unrelated.
 *
 * async, await, yield, of, get, set and static are not here: they are words
 * only in the places that make them words, and names everywhere else. And
 * neither is undefined, which is a name the global object has and not a
 * keyword -- it was one, so `function(window, undefined){...}`, the first
 * line of half the libraries on the web, would not parse. */
static const char *const JS_WORDS[] = {
    "var", "let", "const", "function", "return", "if", "else", "for",
    "while", "do", "break", "continue", "new", "delete", "typeof", "in",
    "this", "null", "true", "false", "throw", "try", "catch",
    "finally", "switch", "case", "default", "void", "instanceof", "class",
    "extends", "super", "import", "debugger", "with", 0
};

static const char *const JS_UNSUPPORTED[] = { "export", 0 };

static int js_is_word(const char *s, u32 len, const char *w) {
    u32 i = 0;
    for (; i < len; i++) if (s[i] != w[i] || !w[i]) return 0;
    return w[i] == 0;
}

/* --- one token ----------------------------------------------------------- */

/* The one question a slash depends on. A keyword mostly cannot end an
   expression -- `return /x/` is a pattern -- but the ones that are values
   can. */
static int js_tok_ends_expr(const jtok *t) {
    if (t->type == T_NUM || t->type == T_STRING || t->type == T_NAME
        || t->type == T_REGEX || t->type == T_PRIVATE)
        return 1;
    if (t->type == T_TEMPLATE) return t->flags & 1;
    if (t->type == T_KEYWORD)
        return js_is_word(t->text, t->len, "this") || js_is_word(t->text, t->len, "true")
            || js_is_word(t->text, t->len, "null") || js_is_word(t->text, t->len, "false")
            || js_is_word(t->text, t->len, "super");
    if (t->type == T_PUNCT && t->len == 1)
        return t->text[0] == ')' || t->text[0] == ']' || t->text[0] == '}';
    if (t->type == T_PUNCT && t->len == 2)
        return t->op == OP_INC || t->op == OP_DEC;
    return 0;
}

/* A code point, as the UTF-8 this engine's strings are made of. A lone half
   of a pair is written as the three bytes it would be on its own, which is
   what every other engine that keeps UTF-8 does with one. */
static u32 js_utf8(u32 cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    if (cp > 0x10FFFF) cp = 0xFFFD;
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* One escape after a backslash, the backslash already passed: what it
   means, written at buf + *w, with *i moved past it. Returns 0 for an escape
   a template may not have (a legacy octal, a broken \u), which only a tagged
   template is allowed to see. Shared by strings and templates. */
static int js_escape(const char *src, u32 n, u32 *i, char *buf, u32 *w, int in_template) {
    char e = src[*i];
    (*i)++;
    switch (e) {
        case 'n': buf[(*w)++] = '\n'; return 1;
        case 't': buf[(*w)++] = '\t'; return 1;
        case 'r': buf[(*w)++] = '\r'; return 1;
        case 'b': buf[(*w)++] = 8; return 1;
        case 'f': buf[(*w)++] = 12; return 1;
        case 'v': buf[(*w)++] = 11; return 1;
        case '\r':
            if (*i < n && src[*i] == '\n') (*i)++;
            return 1;                                 /* a line continued */
        case '\n': return 1;
        case 'x': {
            int h1 = *i < n ? js_hexval(src[*i]) : -1;
            int h2 = *i + 1 < n ? js_hexval(src[*i + 1]) : -1;
            if (h1 < 0 || h2 < 0) { buf[(*w)++] = 'x'; return 0; }
            *i += 2;
            *w += js_utf8((u32)(h1 * 16 + h2), buf + *w);
            return 1;
        }
        case 'u': {
            u32 cp = 0;
            if (*i < n && src[*i] == '{') {
                u32 k = *i + 1;
                int any = 0;
                while (k < n && js_hexval(src[k]) >= 0 && cp <= 0x10FFFF) {
                    cp = cp * 16 + (u32)js_hexval(src[k]);
                    k++;
                    any = 1;
                }
                if (!any || k >= n || src[k] != '}') { buf[(*w)++] = 'u'; return 0; }
                *i = k + 1;
            } else {
                for (int k = 0; k < 4; k++) {
                    int h = *i + (u32)k < n ? js_hexval(src[*i + k]) : -1;
                    if (h < 0) { buf[(*w)++] = 'u'; return 0; }
                    cp = cp * 16 + (u32)h;
                }
                *i += 4;
                /* A character past the first sixty five thousand comes as
                   two halves, and is one character. */
                if (cp >= 0xD800 && cp < 0xDC00 && *i + 5 < n && src[*i] == '\\'
                    && src[*i + 1] == 'u') {
                    u32 lo = 0;
                    int ok = 1;
                    for (int k = 0; k < 4; k++) {
                        int h = js_hexval(src[*i + 2 + k]);
                        if (h < 0) { ok = 0; break; }
                        lo = lo * 16 + (u32)h;
                    }
                    if (ok && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        *i += 6;
                    }
                }
            }
            *w += js_utf8(cp, buf + *w);
            return 1;
        }
        default:
            if (e >= '0' && e <= '7') {
                /* \0 on its own is a nought; anything else is an old octal,
                   which a template may not have. */
                if (e == '0' && !(*i < n && js_digit(src[*i]))) { buf[(*w)++] = 0; return 1; }
                if (in_template) { buf[(*w)++] = e; return 0; }
                u32 v = (u32)(e - '0');
                int most = e <= '3' ? 2 : 1;
                while (most-- > 0 && *i < n && src[*i] >= '0' && src[*i] <= '7')
                    v = v * 8 + (u32)(src[(*i)++] - '0');
                *w += js_utf8(v, buf + *w);
                return 1;
            }
            if (e == '8' || e == '9') { buf[(*w)++] = e; return !in_template; }
            {
                /* An escaped line separator continues the line too. */
                int nl;
                if ((unsigned char)e >= 0x80 && js_wide_space(src + *i - 1, n - (*i - 1), &nl) && nl) {
                    *i += 2;
                    return 1;
                }
            }
            buf[(*w)++] = e;
            return 1;
    }
}

/* A piece of a template: from just after the backtick or the closing brace
   of a ${...}, to the next ${ or the closing backtick. */
static void js_template_piece(jlex *L) {
    u32 start = L->at;
    u32 i = start;
    int ended = 0;
    while (i < L->n) {
        char c = L->src[i];
        if (c == '`') { ended = 1; break; }
        if (c == '$' && i + 1 < L->n && L->src[i + 1] == '{') break;
        if (c == '\\' && i + 1 < L->n) { i += 2; continue; }
        i++;
    }
    if (i >= L->n) {
        js_fail_at(L->J, L->tok.line, "a template is not closed", 0, 0);
        L->failed = 1;
        L->tok.type = T_EOF;
        return;
    }
    u32 end = i;
    char *buf = (char *)js_alloc(L->J, end - start + 1);
    char *raw = (char *)js_alloc(L->J, end - start + 1);
    if (!buf || !raw) { L->failed = 1; L->tok.type = T_EOF; return; }
    u32 w = 0, rw = 0;
    int bad = 0;
    u32 k = start;
    while (k < end) {
        char c = L->src[k];
        if (c == '\\') {
            u32 from = k;
            k++;
            if (!js_escape(L->src, end, &k, buf, &w, 1)) bad = 1;
            for (u32 q = from; q < k; q++) {
                if (L->src[q] == '\r') {
                    raw[rw++] = '\n';
                    if (q + 1 < k && L->src[q + 1] == '\n') q++;
                } else raw[rw++] = L->src[q];
                if (L->src[q] == '\n') L->line++;
            }
            continue;
        }
        /* A line in a template ends with a newline, however the file ends
           its lines. */
        if (c == '\r') {
            buf[w++] = '\n';
            raw[rw++] = '\n';
            if (k + 1 < end && L->src[k + 1] == '\n') k++;
            L->line++;
            k++;
            continue;
        }
        if (c == '\n') L->line++;
        buf[w++] = c;
        raw[rw++] = c;
        k++;
    }
    buf[w] = 0;
    raw[rw] = 0;
    L->tok.type = T_TEMPLATE;
    L->tok.op = OP_NONE;
    L->tok.text = buf;
    L->tok.len = w;
    L->tok.raw = raw;
    L->tok.rawlen = rw;
    L->tok.bad_escape = bad;
    L->tok.flags = ended;
    L->at = end + (ended ? 1 : 2);
}

static void js_next(jlex *L) {
    L->ends_expr = L->want_regex ? 0 : js_tok_ends_expr(&L->tok);
    L->want_regex = 0;
    /* After a dot, a word is only a property's name: a.with(), o.export. */
    int after_dot = L->tok.type == T_PUNCT && L->tok.len == 1 && L->tok.text[0] == '.';
    L->nl = 0;
    int line_start = L->at == 0;

    for (;;) {
        if (L->at >= L->n) break;
        char c = L->src[L->at];
        if (c == '\n') { L->line++; L->nl = 1; L->at++; line_start = 1; continue; }
        if (js_space(c)) { L->at++; continue; }
        if ((unsigned char)c >= 0x80) {
            int nl;
            int k = js_wide_space(L->src + L->at, L->n - L->at, &nl);
            if (k) {
                L->at += (u32)k;
                if (nl) { L->line++; L->nl = 1; line_start = 1; }
                continue;
            }
            break;
        }
        if (c == '/' && L->at + 1 < L->n && L->src[L->at + 1] == '/') {
            while (L->at < L->n && L->src[L->at] != '\n') L->at++;
            continue;
        }
        if (c == '/' && L->at + 1 < L->n && L->src[L->at + 1] == '*') {
            L->at += 2;
            while (L->at + 1 < L->n
                   && !(L->src[L->at] == '*' && L->src[L->at + 1] == '/')) {
                if (L->src[L->at] == '\n') { L->line++; L->nl = 1; line_start = 1; }
                L->at++;
            }
            L->at = L->at + 2 < L->n ? L->at + 2 : L->n;
            continue;
        }
        /* The comments HTML left behind: a script inside <!-- ... --> was
           how a page hid itself from browsers that had never heard of one,
           and the language still reads both as the start of a comment. */
        if (c == '<' && L->at + 3 < L->n && L->src[L->at + 1] == '!'
            && L->src[L->at + 2] == '-' && L->src[L->at + 3] == '-') {
            while (L->at < L->n && L->src[L->at] != '\n') L->at++;
            continue;
        }
        if (c == '-' && line_start && L->at + 2 < L->n && L->src[L->at + 1] == '-'
            && L->src[L->at + 2] == '>') {
            while (L->at < L->n && L->src[L->at] != '\n') L->at++;
            continue;
        }
        /* #! on the first line is for a shell, not for the language. */
        if (c == '#' && L->at == 0 && L->n > 1 && L->src[1] == '!') {
            while (L->at < L->n && L->src[L->at] != '\n') L->at++;
            continue;
        }
        break;
    }

    L->tok.line = L->line;
    L->tok.nl_before = L->nl;
    L->tok.op = OP_NONE;
    L->tok.len = 0;
    L->tok.flags = 0;
    L->tok.text = L->src + L->at;

    if (L->at >= L->n) { L->tok.type = T_EOF; return; }

    char c = L->src[L->at];

    /* --- a number --------------------------------------------------------- */
    if (js_digit(c) || (c == '.' && L->at + 1 < L->n
                        && js_digit(L->src[L->at + 1]))) {
        u32 start = L->at;
        double v = 0;
        char nx = L->at + 1 < L->n ? L->src[L->at + 1] : 0;
        int base = 0;
        if (c == '0' && (nx == 'x' || nx == 'X')) base = 16;
        else if (c == '0' && (nx == 'b' || nx == 'B')) base = 2;
        else if (c == '0' && (nx == 'o' || nx == 'O')) base = 8;

        if (base) {
            L->at += 2;
            while (L->at < L->n) {
                char d = L->src[L->at];
                if (d == '_') { L->at++; continue; }
                int k = js_hexval(d);
                if (k < 0 || k >= base) break;
                v = v * base + (double)k;
                L->at++;
            }
        } else {
            /* An old octal, 0777, when every digit is one; 08 and 09 are
               decimal. */
            int octal = c == '0' && js_digit(nx);
            for (u32 k = L->at + 1; octal && k < L->n && js_digit(L->src[k]); k++)
                if (L->src[k] > '7') octal = 0;
            if (octal) {
                L->at++;
                while (L->at < L->n && js_digit(L->src[L->at]))
                    v = v * 8 + (L->src[L->at++] - '0');
            } else {
                /* The nearest double, read exactly (jsnum.h), with the
                   underscores a long number is written with left out. */
                char tmp[400];
                u32 w = 0, i = L->at;
                int seen_e = 0, seen_dot = 0;
                while (i < L->n && w < sizeof(tmp) - 1) {
                    char d = L->src[i];
                    if (d == '_' && i + 1 < L->n && js_digit(L->src[i + 1])) { i++; continue; }
                    if (js_digit(d)) { tmp[w++] = d; i++; continue; }
                    /* One point, and not after the exponent: 1..toString()
                       is the number 1. and then a method. */
                    if (d == '.' && !seen_dot && !seen_e) { seen_dot = 1; tmp[w++] = d; i++; continue; }
                    if ((d == 'e' || d == 'E') && !seen_e) {
                        seen_e = 1;
                        tmp[w++] = d;
                        i++;
                        if (i < L->n && (L->src[i] == '+' || L->src[i] == '-')) tmp[w++] = L->src[i++];
                        continue;
                    }
                    break;
                }
                u32 used = 0;
                v = js_decimal(tmp, w, &used);
                /* Count what was used in the source, underscores and all. */
                u32 took = 0, k = L->at;
                while (took < used && k < L->n) {
                    if (L->src[k] != '_') took++;
                    k++;
                }
                L->at = k;
            }
        }
        /* A BigInt is read as the number it names: this engine has no
           arithmetic of any size, and a page that writes 10n nearly always
           means ten. */
        if (L->at < L->n && L->src[L->at] == 'n') L->at++;

        L->tok.type = T_NUM;
        L->tok.num = v;
        L->tok.len = L->at - start;
        return;
    }

    /* --- a name or a word ------------------------------------------------- */
    if (js_alpha(c) || (c == '\\' && L->at + 1 < L->n && L->src[L->at + 1] == 'u')) {
        u32 start = L->at;
        int escaped = 0;
        while (L->at < L->n) {
            char d = L->src[L->at];
            /* A or \u{41} in a name is the letter: GitHub's hotkey
               module is written with them. Stepped over here and decoded
               below. */
            if (d == '\\' && L->at + 1 < L->n && L->src[L->at + 1] == 'u') {
                u32 k = L->at + 2;
                if (k < L->n && L->src[k] == '{') {
                    while (k < L->n && L->src[k] != '}') k++;
                    k++;
                } else {
                    k += 4;
                }
                if (k > L->n) break;
                L->at = k;
                escaped = 1;
                continue;
            }
            if ((unsigned char)d >= 0x80) {
                int nl;
                if (js_wide_space(L->src + L->at, L->n - L->at, &nl)) break;
                L->at++;
                continue;
            }
            if (!js_alnum(d)) break;
            L->at++;
        }
        if (L->at == start) {
            js_fail_at(L->J, L->tok.line, "an escape in a name is not read here", 0, 0);
            L->failed = 1;
            L->tok.type = T_EOF;
            return;
        }
        L->tok.text = L->src + start;
        L->tok.len = L->at - start;
        if (escaped) {
            /* The name as its letters, in a string of its own that the token
               points at; the source goes on from where the name ended. */
            char buf[256];
            u32 w = 0;
            for (u32 i = start; i < L->at && w < sizeof(buf) - 4; ) {
                if (L->src[i] == '\\' && i + 1 < L->at && L->src[i + 1] == 'u') {
                    u32 cp = 0;
                    i += 2;
                    if (i < L->at && L->src[i] == '{') {
                        for (i++; i < L->at && L->src[i] != '}'; i++)
                            if (js_hexval(L->src[i]) >= 0) cp = cp * 16 + (u32)js_hexval(L->src[i]);
                        i++;
                    } else {
                        for (int h = 0; h < 4 && i < L->at; h++, i++)
                            if (js_hexval(L->src[i]) >= 0) cp = cp * 16 + (u32)js_hexval(L->src[i]);
                    }
                    w += js_utf8(cp, buf + w);
                } else {
                    buf[w++] = L->src[i++];
                }
            }
            jstr *s = js_str_n(L->J, buf, w);
            if (s) { L->tok.text = s->s; L->tok.len = s->len; }
        }

        for (int i = 0; JS_UNSUPPORTED[i]; i++) {
            if (js_is_word(L->tok.text, L->tok.len, JS_UNSUPPORTED[i])) {
                /* A word as a property name is only a name: a.with,
                   {export: 1}. */
                u32 k = L->at;
                while (k < L->n && (L->src[k] == ' ' || L->src[k] == '\t')) k++;
                if (after_dot || (k < L->n && L->src[k] == ':')) break;
                js_fail_at(L->J, L->tok.line,
                           "this engine does not have ", JS_UNSUPPORTED[i],
                           (u32)strlen(JS_UNSUPPORTED[i]));
                L->failed = 1;
                L->tok.type = T_EOF;
                return;
            }
        }
        for (int i = 0; JS_WORDS[i]; i++) {
            if (js_is_word(L->tok.text, L->tok.len, JS_WORDS[i])) {
                L->tok.type = T_KEYWORD;
                return;
            }
        }
        L->tok.type = T_NAME;
        return;
    }

    /* --- #name, a class's own ----------------------------------------------- */
    if (c == '#' && L->at + 1 < L->n && js_alpha(L->src[L->at + 1])) {
        u32 start = L->at;
        L->at++;
        while (L->at < L->n && js_alnum(L->src[L->at])) L->at++;
        L->tok.type = T_PRIVATE;
        L->tok.text = L->src + start;
        L->tok.len = L->at - start;
        return;
    }

    /* --- a string --------------------------------------------------------- */
    if (c == '"' || c == '\'') {
        char quote = c;
        L->at++;
        u32 start = L->at;
        u32 scan = L->at;
        while (scan < L->n && L->src[scan] != quote) {
            if (L->src[scan] == '\\' && scan + 1 < L->n) scan++;
            else if (L->src[scan] == '\n') break;
            scan++;
        }
        if (scan >= L->n || L->src[scan] != quote) {
            js_fail_at(L->J, L->tok.line, "a string is not closed", 0, 0);
            L->failed = 1;
            L->tok.type = T_EOF;
            return;
        }

        /* No escape makes more bytes than it was written in, so the copy is
           sized by the source; and it is made as the string itself, so the
           parser does not copy it a second time. */
        jstr *js = (jstr *)js_alloc(L->J, (u32)sizeof(jstr) + (scan - start) + 1);
        if (!js) { L->tok.type = T_EOF; L->failed = 1; return; }
        char *buf = js->s;
        u32 w = 0;
        u32 i = start;
        while (i < scan) {
            char d = L->src[i];
            if (d == '\\') {
                i++;
                if (L->src[i] == '\n' || L->src[i] == '\r') L->line++;
                js_escape(L->src, scan, &i, buf, &w, 0);
            } else {
                buf[w++] = d;
                i++;
            }
        }
        buf[w] = 0;
        js->len = w;
        js->hash = js_hash(buf, w);
        L->at = scan + 1;
        L->tok.type = T_STRING;
        L->tok.text = buf;
        L->tok.len = w;
        L->tok.str = js;
        return;
    }

    /* --- a template ------------------------------------------------------- */
    if (c == '`') {
        L->at++;
        js_template_piece(L);
        return;
    }

    /* --- a regular expression --------------------------------------------
     *
     * Checked before every operator that starts with a slash, including
     * `/=`, because `/=x/` is a perfectly ordinary pattern and reading it
     * as a divide-and-assign is how an engine rejects a line that every
     * browser accepts.
     *
     * A slash inside a class does not close it: `/[/]/` is one character.
     */
    if (c == '/' && !L->ends_expr) {
        u32 i = L->at + 1;
        int in_class = 0, closed = 0;
        while (i < L->n) {
            char d = L->src[i];
            if (d == '\\') { i += 2; continue; }
            if (d == '\n') break;
            if (d == '[') in_class = 1;
            else if (d == ']') in_class = 0;
            else if (d == '/' && !in_class) { closed = 1; break; }
            i++;
        }
        if (closed) {
            L->tok.type = T_REGEX;
            L->tok.text = L->src + L->at + 1;
            L->tok.len = i - (L->at + 1);
            L->tok.flags = 0;
            i++;
            while (i < L->n) {
                char f = L->src[i];
                int bit = f == 'i' ? RXF_I : f == 'g' ? RXF_G : f == 'm' ? RXF_M
                        : f == 's' ? RXF_S : f == 'u' ? RXF_U : f == 'y' ? RXF_Y
                        : f == 'd' ? RXF_D : f == 'v' ? RXF_V : 0;
                if (!bit) {
                    if (js_alnum(f)) {
                        js_fail_at(L->J, L->tok.line, "a pattern flag that does not exist: ", &f, 1);
                        L->failed = 1;
                        L->tok.type = T_EOF;
                        return;
                    }
                    break;
                }
                L->tok.flags |= bit;
                i++;
            }
            L->at = i;
            return;
        }
        /* No closing slash on this line, so it was a divide after all. */
    }

    /* --- punctuation ------------------------------------------------------ */
    L->tok.type = T_PUNCT;
    L->tok.text = L->src + L->at;

    #define AT(k) (L->at + (k) < L->n ? L->src[L->at + (k)] : 0)
    #define TOK(o, n) do { L->tok.op = (o); L->at += (n); L->tok.len = (n); return; } while (0)

    char c1 = AT(1), c2 = AT(2), c3 = AT(3);
    switch (c) {
        case '=':
            if (c1 == '=' && c2 == '=') TOK(OP_SEQ, 3);
            if (c1 == '=') TOK(OP_EQ, 2);
            if (c1 == '>') TOK(OP_ARROW, 2);
            TOK(OP_ASSIGN, 1);
        case '!':
            if (c1 == '=' && c2 == '=') TOK(OP_SNE, 3);
            if (c1 == '=') TOK(OP_NE, 2);
            TOK(OP_NOT, 1);
        case '>':
            if (c1 == '>' && c2 == '>' && c3 == '=') TOK(OP_USHREQ, 4);
            if (c1 == '>' && c2 == '>') TOK(OP_USHR, 3);
            if (c1 == '>' && c2 == '=') TOK(OP_SHREQ, 3);
            if (c1 == '>') TOK(OP_SHR, 2);
            if (c1 == '=') TOK(OP_GE, 2);
            TOK(OP_GT, 1);
        case '<':
            if (c1 == '<' && c2 == '=') TOK(OP_SHLEQ, 3);
            if (c1 == '<') TOK(OP_SHL, 2);
            if (c1 == '=') TOK(OP_LE, 2);
            TOK(OP_LT, 1);
        case '&':
            if (c1 == '&' && c2 == '=') TOK(OP_LANDEQ, 3);
            if (c1 == '&') TOK(OP_AND, 2);
            if (c1 == '=') TOK(OP_ANDEQ, 2);
            TOK(OP_BAND, 1);
        case '|':
            if (c1 == '|' && c2 == '=') TOK(OP_LOREQ, 3);
            if (c1 == '|') TOK(OP_OR, 2);
            if (c1 == '=') TOK(OP_OREQ, 2);
            TOK(OP_BOR, 1);
        case '?':
            if (c1 == '?' && c2 == '=') TOK(OP_NULLEQ, 3);
            if (c1 == '?') TOK(OP_NULLISH, 2);
            /* ?. but not ?.5, which is a question and a decimal. */
            if (c1 == '.' && !js_digit(c2)) TOK(OP_OPTCHAIN, 2);
            break;
        case '*':
            if (c1 == '*' && c2 == '=') TOK(OP_POWEQ, 3);
            if (c1 == '*') TOK(OP_POW, 2);
            if (c1 == '=') TOK(OP_MULEQ, 2);
            TOK(OP_MUL, 1);
        case '+':
            if (c1 == '+') TOK(OP_INC, 2);
            if (c1 == '=') TOK(OP_ADDEQ, 2);
            TOK(OP_ADD, 1);
        case '-':
            if (c1 == '-') TOK(OP_DEC, 2);
            if (c1 == '=') TOK(OP_SUBEQ, 2);
            TOK(OP_SUB, 1);
        case '/':
            if (c1 == '=') TOK(OP_DIVEQ, 2);
            TOK(OP_DIV, 1);
        case '%':
            if (c1 == '=') TOK(OP_MODEQ, 2);
            TOK(OP_MOD, 1);
        case '^':
            if (c1 == '=') TOK(OP_XOREQ, 2);
            TOK(OP_BXOR, 1);
        case '~': TOK(OP_BNOT, 1);
        case '.':
            if (c1 == '.' && c2 == '.') TOK(OP_SPREAD, 3);
            break;
        default: break;
    }
    #undef AT
    #undef TOK
    L->tok.op = OP_NONE;
    L->at++;
    L->tok.len = 1;
}

/* --- the tree ------------------------------------------------------------ */

#define JS_NODES_STEP 1024

static int js_node(jctx *J, ntype kind, int line) {
    if (J->nnodes >= J->ncap) {
        if (J->ncap >= JS_NODES_CAP) {
            js_fail_at(J, line, "this page's scripts are too big to read", 0, 0);
            return -1;
        }
        int want = J->ncap ? J->ncap * 2 : JS_NODES_STEP;
        if (want > JS_NODES_CAP) want = JS_NODES_CAP;
        jnode *fresh = (jnode *)realloc(J->nodes, (u64)want * sizeof(jnode));
        if (!fresh) { js_fail_at(J, line, "no memory left to read this script", 0, 0); return -1; }
        J->nodes = fresh;
        J->ncap = want;
    }
    int i = J->nnodes++;
    jnode *n = &J->nodes[i];
    n->kind = (u8)kind;
    n->flags = 0;
    n->a = n->b = n->c = n->d = -1;
    n->num = 0;
    n->op = OP_NONE;
    n->line = line;
    return i;
}

/* --- the parser ---------------------------------------------------------- */

/* What the parser knows about the function it is inside: what it may say
   (await, yield), what it said that a call to it will need (arguments,
   super, new.target), and the names its var statements declared anywhere in
   it, which the call declares before the first line runs. */
typedef struct {
    int vars, vars_tail;      /* a chain of N_SEQ cells, one name each */
    int flags;                /* FN_ASYNC, FN_GEN, FN_STRICT */
    int uses;                 /* FN_ARGS, FN_SUPER, FN_NEWTARGET */
    int arrow;
} jfnctx;

typedef struct {
    jlex  L;
    jctx *J;

    /* Set while reading the first clause of a `for`, where `in` is not the
     * operator but the word that makes it a for-in. Brackets and function
     * bodies inside the clause put the operator back, because an `in`
     * inside them cannot be the for-in's. */
    int   no_in;

    jfnctx *fc;

    /* The last arrow function read. Its body ran to the end of an
       assignment expression, and nothing -- no call, no operator -- carries
       on from it: `x => {}\n(y)` is two statements. */
    int   last_arrow;

    /* How deep the reading may go on the machine's stack. */
    char *stack_floor;
} jparse;

#define JS_NESTED(P, stmt) do { int keep_in_ = (P)->no_in; (P)->no_in = 0; \
                                stmt; (P)->no_in = keep_in_; } while (0)

static int js_parse_expr(jparse *P);
static int js_parse_assign(jparse *P);
static int js_parse_stmt(jparse *P);
static int js_parse_unary(jparse *P);
static int js_parse_primary(jparse *P);
static int js_parse_function_rest(jparse *P, int flags, jstr *name, int line, ntype kind);
static int js_parse_class(jparse *P, int is_decl);
static int js_parse_template(jparse *P, int tag);

static void js_parse_fail(jparse *P, int line, const char *what, const char *extra, u32 n) {
    js_fail_at(P->J, line, what, extra, n);
    P->L.failed = 1;
}

/* Whether the machine's stack has room for another level. */
static int js_parse_deep(jparse *P) {
    char here;
    if (&here < P->stack_floor) {
        js_parse_fail(P, P->L.tok.line, "this script is nested too deeply to read", 0, 0);
        return 1;
    }
    return P->L.failed;
}

static int js_at_punct(jparse *P, char c) {
    return P->L.tok.type == T_PUNCT && P->L.tok.len == 1
        && P->L.tok.text[0] == c;
}

static int js_at_op(jparse *P, jop op) {
    return P->L.tok.type == T_PUNCT && P->L.tok.op == op;
}

static int js_at_word(jparse *P, const char *w) {
    return P->L.tok.type == T_KEYWORD
        && js_is_word(P->L.tok.text, P->L.tok.len, w);
}

/* A name that is a word only where it is one: async, of, get, static. */
static int js_at_name(jparse *P, const char *w) {
    return P->L.tok.type == T_NAME && js_is_word(P->L.tok.text, P->L.tok.len, w);
}

static int js_eat_punct(jparse *P, char c) {
    if (!js_at_punct(P, c)) return 0;
    js_next(&P->L);
    return 1;
}

static int js_eat_word(jparse *P, const char *w) {
    if (!js_at_word(P, w)) return 0;
    js_next(&P->L);
    return 1;
}

/* The token after this one, without moving. The lexer is a value, so it is
   copied, moved on and thrown away; what the copy allocated for a string is
   left in the region, which is the whole of the cost. */
static jtok js_peek(jparse *P) {
    jlex save = P->L;
    js_next(&P->L);
    jtok t = P->L.tok;
    P->L = save;
    return t;
}

/* Whether the characters right after the current token, before any newline,
   are =>, which is how `x => ...` is known without reading past the name. */
static int js_arrow_follows(jparse *P) {
    u32 i = P->L.at;
    while (i < P->L.n) {
        char c = P->L.src[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '/' && i + 1 < P->L.n && P->L.src[i + 1] == '*') {
            i += 2;
            while (i + 1 < P->L.n && !(P->L.src[i] == '*' && P->L.src[i + 1] == '/')) {
                if (P->L.src[i] == '\n') return 0;
                i++;
            }
            i += 2;
            continue;
        }
        break;
    }
    return i + 1 < P->L.n && P->L.src[i] == '=' && P->L.src[i + 1] == '>';
}

static void js_expect(jparse *P, char c) {
    if (js_eat_punct(P, c)) return;
    /* And what was there instead. "expected )" on its own sends whoever
       reads it back to the source to find out which ) and what stopped it;
       the token is the answer and it is right here. */
    char said[80];
    int w = 0;
    for (const char *q = "expected "; *q; q++) said[w++] = *q;
    said[w++] = c;
    for (const char *q = ", not "; *q; q++) said[w++] = *q;
    if (P->L.tok.type == T_EOF) for (const char *q = "the end"; *q; q++) said[w++] = *q;
    for (u32 k = 0; k < P->L.tok.len && w < (int)sizeof(said) - 1; k++)
        said[w++] = P->L.tok.text[k];
    said[w] = 0;
    js_parse_fail(P, P->L.tok.line, said, 0, 0);
}

/* A semicolon, or somewhere one would be inserted.
 *
 * Not "skip it if it is there": a statement that runs into the next one with
 * neither a semicolon nor a newline between them is a mistake, and accepting
 * it hides the mistake rather than the semicolon. */
static void js_semicolon(jparse *P) {
    if (js_eat_punct(P, ';')) return;
    if (js_at_punct(P, '}')) return;
    if (P->L.tok.type == T_EOF) return;
    if (P->L.tok.nl_before) return;
    js_parse_fail(P, P->L.tok.line, "expected a semicolon or a new line, not ",
                  P->L.tok.text, P->L.tok.len);
}

/* How tightly each binary operator binds. Higher wins. */
static int js_prec(jop op) {
    switch (op) {
        case OP_NULLISH: return 1;
        case OP_OR:   return 2;
        case OP_AND:  return 3;
        case OP_BOR:  return 4;
        case OP_BXOR: return 5;
        case OP_BAND: return 6;
        case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE: return 7;
        case OP_LT: case OP_GT: case OP_LE: case OP_GE: case OP_IN:
        case OP_INSTANCEOF: return 8;
        case OP_SHL: case OP_SHR: case OP_USHR: return 9;
        case OP_ADD: case OP_SUB: return 10;
        case OP_MUL: case OP_DIV: case OP_MOD: return 11;
        case OP_POW: return 12;
        default: return 0;
    }
}

/* A chain of N_SEQ cells, built one at a time. */
typedef struct { int head, tail; } jchain;

static int js_chain_add(jparse *P, jchain *c, int line) {
    int cell = js_node(P->J, N_SEQ, line);
    if (cell < 0) return -1;
    if (c->tail < 0) c->head = cell;
    else P->J->nodes[c->tail].b = cell;
    c->tail = cell;
    return cell;
}

/* A name declared with var, recorded on the function it is in. */
static void js_note_var(jparse *P, jstr *name, int line) {
    if (!P->fc || !name) return;
    int cell = js_node(P->J, N_SEQ, line);
    if (cell < 0) return;
    P->J->nodes[cell].str = name;
    if (P->fc->vars_tail < 0) P->fc->vars = cell;
    else P->J->nodes[P->fc->vars_tail].b = cell;
    P->fc->vars_tail = cell;
}

/* Every name a pattern declares, for var. */
static void js_note_pattern_vars(jparse *P, int node) {
    jctx *J = P->J;
    if (node < 0) return;
    jnode *n = &J->nodes[node];
    if (n->kind == N_IDENT) { js_note_var(P, n->str, n->line); return; }
    if (n->kind == N_APAT || n->kind == N_OPAT)
        for (int cell = n->a; cell >= 0; cell = J->nodes[cell].b)
            js_note_pattern_vars(P, J->nodes[cell].a);
}

/* A name, made once however many times the text says it. A minified bundle
   says `e`, `t` and `n` hundreds of thousands of times, and a copy for each
   put most of a page's memory into the names before a line had run. */
static jstr *js_intern(jctx *J, const char *s, u32 n) {
    u32 h = js_hash(s, n);
    if (J->nintern * 2 >= J->intern_cap) {
        u32 cap = J->intern_cap ? J->intern_cap * 2 : 4096;
        jstr **fresh = (jstr **)malloc((u64)cap * sizeof(jstr *));
        if (!fresh) return js_str_n(J, s, n);
        for (u32 i = 0; i < cap; i++) fresh[i] = 0;
        for (u32 i = 0; i < J->intern_cap; i++) {
            jstr *x = J->intern[i];
            if (!x) continue;
            u32 k = x->hash & (cap - 1);
            while (fresh[k]) k = (k + 1) & (cap - 1);
            fresh[k] = x;
        }
        if (J->intern) free(J->intern);
        J->intern = fresh;
        J->intern_cap = cap;
    }
    u32 k = h & (J->intern_cap - 1);
    for (jstr *x; (x = J->intern[k]); k = (k + 1) & (J->intern_cap - 1)) {
        if (x->hash != h || x->len != n) continue;
        u32 i = 0;
        while (i < n && x->s[i] == s[i]) i++;
        if (i == n) return x;
    }
    jstr *x = js_str_n(J, s, n);
    if (!x) return 0;
    J->intern[k] = x;
    J->nintern++;
    return x;
}

static jstr *js_tok_str(jparse *P) {
    if (P->L.tok.type == T_STRING && P->L.tok.str) return P->L.tok.str;
    if (P->L.tok.len <= 64) return js_intern(P->J, P->L.tok.text, P->L.tok.len);
    return js_str_n(P->J, P->L.tok.text, P->L.tok.len);
}

/* A number as the key it is: 1.5 is "1.5" and 1e21 is "1e+21", which is what
   the property is called when the number is used to find it. */
static jstr *js_num_key(jctx *J, double d) {
    char tmp[40];
    u32 n = js_num_text(d, tmp, sizeof(tmp));
    return js_str_n(J, tmp, n);
}

/* --- patterns --------------------------------------------------------------
 *
 * `[a, b] = pair` and `({x, y} = point)` are an array and an object up to
 * the `=`, and are read as them; this turns what was read into the pattern it
 * turned out to be, in place. The same turns an arrow's bracket into its
 * parameters. `binding` is for a declaration or a parameter list, where only
 * names may be bound; an assignment may also put values into properties. */
static int js_to_pattern(jparse *P, int node, int binding) {
    jctx *J = P->J;
    if (node < 0) return -1;
    jnode *n = &J->nodes[node];
    switch (n->kind) {
        case N_IDENT:
            return node;
        case N_MEMBER: case N_INDEX: case N_SUPERMEMBER:
            if (binding) break;
            return node;
        case N_ARRAY: {
            n->kind = N_APAT;
            for (int cell = n->a; cell >= 0; cell = J->nodes[cell].b) {
                int e = J->nodes[cell].a;
                if (e < 0) continue;                     /* a hole */
                if (J->nodes[e].kind == N_SPREAD) {
                    J->nodes[cell].op = 1;
                    int t = js_to_pattern(P, J->nodes[e].a, binding);
                    J->nodes[cell].a = t;
                    if (J->nodes[cell].b >= 0) {
                        js_parse_fail(P, J->nodes[e].line, "a rest element has to be the last", 0, 0);
                        return -1;
                    }
                    continue;
                }
                if (J->nodes[e].kind == N_ASSIGN && J->nodes[e].op == OP_ASSIGN) {
                    J->nodes[cell].c = J->nodes[e].b;
                    e = J->nodes[e].a;
                }
                int t = js_to_pattern(P, e, binding);
                J->nodes[cell].a = t;
                if (t < 0) return -1;
            }
            return node;
        }
        case N_OBJECT: {
            n->kind = N_OPAT;
            for (int cell = n->a; cell >= 0; cell = J->nodes[cell].b) {
                jnode *cn = &J->nodes[cell];
                if (cn->op == PK_SPREAD) {
                    cn->op = 1;                          /* rest */
                    int t = js_to_pattern(P, cn->a, binding);
                    J->nodes[cell].a = t;
                    continue;
                }
                if (cn->op == PK_SHORT) {
                    cn->op = 0;
                    /* The default, written {a = 1}, is already in d. */
                    cn->c = -1;
                    int d = cn->d;
                    cn->d = -1;
                    J->nodes[cell].c = d;
                    J->nodes[cell].d = -2;               /* static key in str */
                    continue;
                }
                if (cn->op != PK_INIT && cn->op != PK_PROTO) {
                    js_parse_fail(P, cn->line, "this cannot be a pattern", 0, 0);
                    return -1;
                }
                /* The key is either str or a computed one in c; it moves to
                   d, so c can hold the default. */
                int key = cn->c;
                int v = cn->a;
                int dflt = -1;
                if (v >= 0 && J->nodes[v].kind == N_ASSIGN && J->nodes[v].op == OP_ASSIGN) {
                    dflt = J->nodes[v].b;
                    v = J->nodes[v].a;
                }
                int t = js_to_pattern(P, v, binding);
                J->nodes[cell].op = 0;
                J->nodes[cell].a = t;
                J->nodes[cell].d = key >= 0 ? key : -2;
                J->nodes[cell].c = dflt;
                if (t < 0) return -1;
            }
            return node;
        }
        case N_APAT: case N_OPAT:
            return node;
        default:
            break;
    }
    js_parse_fail(P, n->line, "this cannot be assigned to", 0, 0);
    return -1;
}

/* A name or a pattern, for a declaration, a parameter or a catch. */
static int js_parse_binding(jparse *P) {
    jctx *J = P->J;
    int line = P->L.tok.line;
    if (P->L.tok.type == T_NAME) {
        int n = js_node(J, N_IDENT, line);
        if (n >= 0) J->nodes[n].str = js_tok_str(P);
        js_next(&P->L);
        return n;
    }
    if (js_at_punct(P, '[') || js_at_punct(P, '{')) {
        int e;
        JS_NESTED(P, e = js_parse_primary(P));
        return js_to_pattern(P, e, 1);
    }
    /* let, of and yield, where they are only names. */
    if (P->L.tok.type == T_KEYWORD && js_is_word(P->L.tok.text, P->L.tok.len, "let")) {
        int n = js_node(J, N_IDENT, line);
        if (n >= 0) J->nodes[n].str = js_tok_str(P);
        js_next(&P->L);
        return n;
    }
    js_parse_fail(P, line, "expected a name to declare, not ", P->L.tok.text, P->L.tok.len);
    return -1;
}

/* --- the parts of an expression ------------------------------------------- */

/* A property's name where one is written: a name, a word, a string, a
   number or [an expression]. The static name in *key, or the expression's
   node in *computed. */
static int js_parse_prop_key(jparse *P, jstr **key, int *computed, int *priv) {
    jctx *J = P->J;
    *key = 0;
    *computed = -1;
    if (priv) *priv = 0;
    ttype t = P->L.tok.type;
    if (t == T_NAME || t == T_KEYWORD || t == T_STRING) {
        *key = js_tok_str(P);
        js_next(&P->L);
        return 1;
    }
    if (t == T_NUM) {
        *key = js_num_key(J, P->L.tok.num);
        js_next(&P->L);
        return 1;
    }
    if (t == T_PRIVATE && priv) {
        *key = js_tok_str(P);
        *priv = 1;
        js_next(&P->L);
        return 1;
    }
    if (js_eat_punct(P, '[')) {
        int e;
        JS_NESTED(P, e = js_parse_assign(P));
        js_expect(P, ']');
        *computed = e;
        return 1;
    }
    js_parse_fail(P, P->L.tok.line, "expected a property name, not ", P->L.tok.text, P->L.tok.len);
    return 0;
}

static int js_parse_object(jparse *P, int line) {
    jctx *J = P->J;
    int head = js_node(J, N_OBJECT, line);
    jchain ch = { -1, -1 };
    while (!js_at_punct(P, '}') && P->L.tok.type != T_EOF && !P->L.failed) {
        int cline = P->L.tok.line;
        if (js_at_op(P, OP_SPREAD)) {
            js_next(&P->L);
            int e;
            JS_NESTED(P, e = js_parse_assign(P));
            int cell = js_chain_add(P, &ch, cline);
            if (cell < 0) break;
            J->nodes[cell].op = PK_SPREAD;
            J->nodes[cell].a = e;
            if (!js_eat_punct(P, ',')) break;
            continue;
        }

        int fflags = FN_METHOD;
        int kind = PK_INIT;
        /* get, set and async are words only when a name follows them. */
        if ((js_at_name(P, "get") || js_at_name(P, "set")) ) {
            jtok nx = js_peek(P);
            int follows = nx.type == T_NAME || nx.type == T_KEYWORD || nx.type == T_STRING
                       || nx.type == T_NUM || (nx.type == T_PUNCT && nx.len == 1 && nx.text[0] == '[');
            if (follows) {
                kind = P->L.tok.text[0] == 'g' ? PK_GET : PK_SET;
                fflags |= kind == PK_GET ? FN_GETTER : FN_SETTER;
                js_next(&P->L);
            }
        } else if (js_at_name(P, "async")) {
            jtok nx = js_peek(P);
            int follows = !nx.nl_before && (nx.type == T_NAME || nx.type == T_KEYWORD
                       || nx.type == T_STRING || nx.type == T_NUM
                       || (nx.type == T_PUNCT && nx.len == 1 && (nx.text[0] == '[' || nx.text[0] == '*')));
            if (follows) {
                kind = PK_METHOD;
                fflags |= FN_ASYNC;
                js_next(&P->L);
            }
        }
        if (js_at_op(P, OP_MUL)) {
            kind = PK_METHOD;
            fflags |= FN_GEN;
            js_next(&P->L);
        }

        int short_ok = P->L.tok.type == T_NAME || P->L.tok.type == T_KEYWORD;
        jstr *key;
        int computed;
        const char *keytext = P->L.tok.text;
        u32 keylen = P->L.tok.len;
        ttype keytype = P->L.tok.type;
        if (!js_parse_prop_key(P, &key, &computed, 0)) break;

        int cell = js_chain_add(P, &ch, cline);
        if (cell < 0) break;
        J->nodes[cell].str = key;
        J->nodes[cell].c = computed;

        if (js_at_punct(P, '(')) {
            if (kind == PK_INIT) kind = PK_METHOD;
            int f = js_parse_function_rest(P, fflags, key, cline, N_FUNC);
            J->nodes[cell].op = (u16)kind;
            J->nodes[cell].a = f;
        } else if (kind != PK_INIT) {
            js_parse_fail(P, P->L.tok.line, "expected (, not ", P->L.tok.text, P->L.tok.len);
            break;
        } else if (js_eat_punct(P, ':')) {
            int v;
            JS_NESTED(P, v = js_parse_assign(P));
            /* A function written as a property's value is named after it. */
            if (v >= 0 && key && (J->nodes[v].kind == N_FUNC || J->nodes[v].kind == N_CLASS)
                && !J->nodes[v].str)
                J->nodes[v].str = key;
            J->nodes[cell].a = v;
            J->nodes[cell].op = computed < 0 && key && js_str_is(key, "__proto__")
                              && keytype != T_NUM ? PK_PROTO : PK_INIT;
        } else if (short_ok && computed < 0 && keytype != T_STRING) {
            /* {a} is {a: a}; {a = 1} is only ever a pattern. */
            int id = js_node(J, N_IDENT, cline);
            if (id >= 0) J->nodes[id].str = js_intern(J, keytext, keylen);
            J->nodes[cell].op = PK_SHORT;
            J->nodes[cell].a = id;
            if (js_at_op(P, OP_ASSIGN)) {
                js_next(&P->L);
                int d;
                JS_NESTED(P, d = js_parse_assign(P));
                J->nodes[cell].d = d;
            }
        } else {
            js_parse_fail(P, P->L.tok.line, "expected :, not ", P->L.tok.text, P->L.tok.len);
            break;
        }
        if (!js_eat_punct(P, ',')) break;
    }
    js_expect(P, '}');
    if (head >= 0) J->nodes[head].a = ch.head;
    return head;
}

/* An array literal. The elements hang off a chain of nodes rather than a
   list, because the tree is a flat array of fixed size nodes and a chain
   needs no second allocation. A hole, [1,,2], is an element of -1. */
static int js_parse_array(jparse *P, int line) {
    jctx *J = P->J;
    int head = js_node(J, N_ARRAY, line);
    jchain ch = { -1, -1 };
    while (!js_at_punct(P, ']') && P->L.tok.type != T_EOF && !P->L.failed) {
        int cline = P->L.tok.line;
        if (js_at_punct(P, ',')) {
            js_next(&P->L);
            if (js_chain_add(P, &ch, cline) < 0) break;
            continue;
        }
        int e;
        if (js_at_op(P, OP_SPREAD)) {
            js_next(&P->L);
            int inner;
            JS_NESTED(P, inner = js_parse_assign(P));
            e = js_node(J, N_SPREAD, cline);
            if (e >= 0) J->nodes[e].a = inner;
        } else {
            JS_NESTED(P, e = js_parse_assign(P));
        }
        int cell = js_chain_add(P, &ch, cline);
        if (cell < 0) break;
        J->nodes[cell].a = e;
        if (!js_eat_punct(P, ',')) break;
    }
    js_expect(P, ']');
    if (head >= 0) J->nodes[head].a = ch.head;
    return head;
}

/* An arrow's parameters, from what the bracket before it held: a list of
   nodes, each a name, a name = default, a pattern or ...rest. */
static int js_arrow_params(jparse *P, int *items, int count, int *simple) {
    jctx *J = P->J;
    jchain ch = { -1, -1 };
    *simple = 1;
    for (int i = 0; i < count; i++) {
        int e = items[i];
        int line = J->nodes[e].line;
        int cell = js_chain_add(P, &ch, line);
        if (cell < 0) return -1;
        if (J->nodes[e].kind == N_SPREAD) {
            J->nodes[cell].op = 1;
            e = J->nodes[e].a;
            *simple = 0;
        } else if (J->nodes[e].kind == N_ASSIGN && J->nodes[e].op == OP_ASSIGN) {
            J->nodes[cell].c = J->nodes[e].b;
            e = J->nodes[e].a;
            *simple = 0;
        }
        int t = js_to_pattern(P, e, 1);
        if (t < 0) return -1;
        if (J->nodes[t].kind != N_IDENT) *simple = 0;
        else J->nodes[cell].str = J->nodes[t].str;
        J->nodes[cell].a = t;
    }
    return ch.head;
}

/* The body of an arrow, its parameters already read. */
static int js_parse_arrow_body(jparse *P, int params, int nparams, int simple, int flags, int line) {
    jctx *J = P->J;
    if (!js_at_op(P, OP_ARROW)) {
        js_parse_fail(P, P->L.tok.line, "expected =>, not ", P->L.tok.text, P->L.tok.len);
        return -1;
    }
    if (P->L.tok.nl_before) {
        js_parse_fail(P, P->L.tok.line, "an arrow has to be on the line its parameters end on", 0, 0);
        return -1;
    }
    js_next(&P->L);

    int n = js_node(J, N_FUNC, line);
    if (n < 0) return -1;
    jfnctx fc = { -1, -1, flags | (P->fc ? (P->fc->flags & FN_STRICT) : 0), 0, 1 };
    jfnctx *outer = P->fc;
    P->fc = &fc;

    int body;
    int fl = FN_ARROW | (flags & (FN_ASYNC)) | (simple ? FN_SIMPLE : 0);
    if (js_at_punct(P, '{')) {
        body = js_parse_stmt(P);
        if (body >= 0) {
            J->nodes[body].flags |= NF_BODY;
            J->nodes[body].c = fc.vars;
        }
    } else {
        /* A body that is one expression is that expression returned, which
           is the whole reason anybody writes one of these. */
        int keep = P->no_in;
        body = js_parse_assign(P);
        P->no_in = keep;
        fl |= FN_EXPR;
    }
    P->fc = outer;
    /* An arrow has no arguments, super or new.target of its own: they are
       the function's round it, which therefore needs them. */
    if (outer) outer->uses |= fc.uses;
    fl |= fc.flags & FN_STRICT;

    J->nodes[n].a = body;
    J->nodes[n].b = params;
    J->nodes[n].d = nparams;
    J->nodes[n].op = (u16)fl;
    P->last_arrow = n;
    return n;
}

/* The bracket after an expression that might be an arrow's: `(a, b)`. Reads
   the list, and then either makes the arrow (when => follows) or the
   expression it is. `async_ident` is the node for a preceding `async`, which
   makes a call of the list if no arrow follows. */
static int js_parse_paren(jparse *P, int line, int async_ident) {
    jctx *J = P->J;
    /* Small, because brackets nest and every level of them has one of
       these on the stack; a longer list moves to the region. */
    int items[12];
    int count = 0, trailing = 0, rest = 0;
    int *list = items;
    int cap = 12;
    while (!js_at_punct(P, ')') && P->L.tok.type != T_EOF && !P->L.failed) {
        int e;
        int eline = P->L.tok.line;
        if (js_at_op(P, OP_SPREAD)) {
            js_next(&P->L);
            int inner;
            JS_NESTED(P, inner = js_parse_assign(P));
            e = js_node(J, N_SPREAD, eline);
            if (e >= 0) J->nodes[e].a = inner;
            rest = 1;
        } else {
            JS_NESTED(P, e = js_parse_assign(P));
        }
        if (e < 0) break;
        if (count >= cap) {
            /* A long list, kept in the region rather than refused. */
            int *more = (int *)js_alloc(J, (u32)(cap * 2) * (u32)sizeof(int));
            if (!more) break;
            for (int i = 0; i < count; i++) more[i] = list[i];
            list = more;
            cap *= 2;
        }
        list[count++] = e;
        if (!js_eat_punct(P, ',')) break;
        if (js_at_punct(P, ')')) trailing = 1;
    }
    js_expect(P, ')');
    if (P->L.failed) return -1;

    if (js_at_op(P, OP_ARROW)) {
        int simple;
        int params = js_arrow_params(P, list, count, &simple);
        if (P->L.failed) return -1;
        int nformal = 0;
        for (int p = params; p >= 0; p = J->nodes[p].b) {
            if (J->nodes[p].op == 1 || J->nodes[p].c >= 0) break;
            nformal++;
        }
        return js_parse_arrow_body(P, params, nformal, simple,
                                   async_ident >= 0 ? FN_ASYNC : 0, line);
    }

    if (async_ident >= 0) {
        /* Not an arrow, so async was a function being called. */
        int call = js_node(J, N_CALL, line);
        if (call < 0) return -1;
        J->nodes[call].a = async_ident;
        jchain ch = { -1, -1 };
        for (int i = 0; i < count; i++) {
            int cell = js_chain_add(P, &ch, line);
            if (cell >= 0) J->nodes[cell].a = list[i];
        }
        J->nodes[call].b = ch.head;
        return call;
    }

    if (count == 0 || rest || trailing) {
        js_parse_fail(P, line, "expected => after this bracket", 0, 0);
        return -1;
    }
    /* An arrow inside the bracket is finished; the bracket may be called or
       added to like anything else, (x => x)(1). */
    P->last_arrow = -1;
    int e = list[0];
    for (int i = 1; i < count; i++) {
        int s = js_node(J, N_SEQ, line);
        if (s < 0) return -1;
        J->nodes[s].a = e;
        J->nodes[s].c = list[i];       /* c, so b stays the list link */
        e = s;
    }
    J->nodes[e].flags |= NF_PAREN;
    return e;
}

static int js_parse_func(jparse *P, int is_decl, int flags);
static int js_parse_arglist(jparse *P, int line);

/* The current token, a divide, read again as the pattern it has to be. */
static void js_relex_regex(jparse *P) {
    int line = P->L.tok.line, nl = P->L.tok.nl_before;
    P->L.at = (u32)(P->L.tok.text - P->L.src);
    P->L.line = line;
    P->L.want_regex = 1;
    js_next(&P->L);
    P->L.tok.nl_before = nl;
}

static int js_parse_primary(jparse *P) {
    jctx *J = P->J;
    int line = P->L.tok.line;
    if (js_parse_deep(P)) return -1;
    if (P->L.tok.type == T_PUNCT && (P->L.tok.op == OP_DIV || P->L.tok.op == OP_DIVEQ))
        js_relex_regex(P);

    if (P->L.tok.type == T_NUM) {
        int n = js_node(J, N_NUM, line);
        if (n >= 0) J->nodes[n].num = P->L.tok.num;
        js_next(&P->L);
        return n;
    }
    if (P->L.tok.type == T_STRING) {
        int n = js_node(J, N_STR, line);
        if (n >= 0) J->nodes[n].str = js_tok_str(P);
        js_next(&P->L);
        return n;
    }
    if (P->L.tok.type == T_TEMPLATE) return js_parse_template(P, -1);
    if (P->L.tok.type == T_REGEX) {
        int n = js_node(J, N_REGEX, line);
        if (n >= 0) {
            J->nodes[n].str = js_tok_str(P);
            J->nodes[n].op = (u16)P->L.tok.flags;
        }
        js_next(&P->L);
        return n;
    }
    if (P->L.tok.type == T_PRIVATE) {
        /* #x in o, the one place a private name stands on its own. */
        int n = js_node(J, N_PRIVNAME, line);
        if (n >= 0) J->nodes[n].str = js_tok_str(P);
        js_next(&P->L);
        return n;
    }
    if (P->L.tok.type == T_NAME) {
        /* async function, and the async arrows. */
        if (js_at_name(P, "async")) {
            jtok nx = js_peek(P);
            if (!nx.nl_before && nx.type == T_KEYWORD && js_is_word(nx.text, nx.len, "function")) {
                js_next(&P->L);
                return js_parse_func(P, 0, FN_ASYNC);
            }
            if (!nx.nl_before && nx.type == T_NAME) {
                /* async x => ... */
                jlex save = P->L;
                js_next(&P->L);
                if (js_arrow_follows(P)) {
                    int id = js_node(J, N_IDENT, line);
                    if (id < 0) return -1;
                    J->nodes[id].str = js_tok_str(P);
                    js_next(&P->L);
                    int cell = js_node(J, N_SEQ, line);
                    if (cell < 0) return -1;
                    J->nodes[cell].a = id;
                    J->nodes[cell].str = J->nodes[id].str;
                    return js_parse_arrow_body(P, cell, 1, 1, FN_ASYNC, line);
                }
                P->L = save;
            }
            if (!nx.nl_before && nx.type == T_PUNCT && nx.len == 1 && nx.text[0] == '(') {
                int id = js_node(J, N_IDENT, line);
                if (id < 0) return -1;
                J->nodes[id].str = js_tok_str(P);
                js_next(&P->L);
                js_next(&P->L);                        /* the ( */
                return js_parse_paren(P, line, id);
            }
        }
        /* x => ... */
        if (js_arrow_follows(P)) {
            int id = js_node(J, N_IDENT, line);
            if (id < 0) return -1;
            J->nodes[id].str = js_tok_str(P);
            js_next(&P->L);
            int cell = js_node(J, N_SEQ, line);
            if (cell < 0) return -1;
            J->nodes[cell].a = id;
            J->nodes[cell].str = J->nodes[id].str;
            return js_parse_arrow_body(P, cell, 1, 1, 0, line);
        }
        int n = js_node(J, N_IDENT, line);
        if (n >= 0) J->nodes[n].str = js_tok_str(P);
        if (P->fc && js_is_word(P->L.tok.text, P->L.tok.len, "arguments")) P->fc->uses |= FN_ARGS;
        js_next(&P->L);
        return n;
    }

    if (js_at_word(P, "true"))  { js_next(&P->L); return js_node(J, N_TRUE, line); }
    if (js_at_word(P, "false")) { js_next(&P->L); return js_node(J, N_FALSE, line); }
    if (js_at_word(P, "null"))  { js_next(&P->L); return js_node(J, N_NULL, line); }
    if (js_at_word(P, "this"))  { js_next(&P->L); return js_node(J, N_THIS, line); }
    if (js_at_word(P, "function")) return js_parse_func(P, 0, 0);
    if (js_at_word(P, "class")) return js_parse_class(P, 0);

    if (js_at_word(P, "super")) {
        js_next(&P->L);
        if (P->fc) P->fc->uses |= FN_SUPER;
        if (js_at_punct(P, '(')) {
            js_next(&P->L);
            int n = js_node(J, N_SUPERCALL, line);
            if (n < 0) return -1;
            int args = js_parse_arglist(P, line);
            J->nodes[n].b = args;
            return n;
        }
        int n = js_node(J, N_SUPERMEMBER, line);
        if (n < 0) return -1;
        if (js_eat_punct(P, '.')) {
            if (P->L.tok.type != T_NAME && P->L.tok.type != T_KEYWORD) {
                js_parse_fail(P, line, "expected a property name after super.", 0, 0);
                return -1;
            }
            J->nodes[n].str = js_tok_str(P);
            js_next(&P->L);
        } else if (js_eat_punct(P, '[')) {
            int e;
            JS_NESTED(P, e = js_parse_expr(P));
            js_expect(P, ']');
            J->nodes[n].b = e;
        } else {
            js_parse_fail(P, line, "super has to be called or have a property read", 0, 0);
            return -1;
        }
        return n;
    }

    if (js_at_word(P, "import")) {
        js_next(&P->L);
        if (js_eat_punct(P, '(')) {
            int e;
            JS_NESTED(P, e = js_parse_assign(P));
            js_expect(P, ')');
            int n = js_node(J, N_IMPORT, line);
            if (n >= 0) J->nodes[n].a = e;
            return n;
        }
        js_parse_fail(P, line, "this engine does not have modules", 0, 0);
        return -1;
    }

    if (js_at_word(P, "new")) {
        /* new.target, which is the one thing `new` can be followed by that is
           not a constructor. */
        jtok nx = js_peek(P);
        if (nx.type == T_PUNCT && nx.len == 1 && nx.text[0] == '.') {
            js_next(&P->L);
            js_next(&P->L);
            if (!js_at_name(P, "target")) {
                js_parse_fail(P, line, "new. can only be new.target", 0, 0);
                return -1;
            }
            js_next(&P->L);
            if (P->fc) P->fc->uses |= FN_NEWTARGET;
            return js_node(J, N_NEWTARGET, line);
        }
    }

    if (js_eat_punct(P, '(')) return js_parse_paren(P, line, -1);
    if (js_eat_punct(P, '[')) return js_parse_array(P, line);
    if (js_eat_punct(P, '{')) return js_parse_object(P, line);

    if (P->L.tok.type == T_EOF && !P->L.failed)
        js_parse_fail(P, line, "the script ends in the middle of something", 0, 0);
    else
        js_parse_fail(P, line, "this is not something a value can start with: ",
                      P->L.tok.text, P->L.tok.len);
    return -1;
}

/* The arguments of a call, its '(' already read, as a chain of N_SEQ cells:
   -1 for none. */
static int js_parse_arglist(jparse *P, int line) {
    jctx *J = P->J;
    jchain ch = { -1, -1 };
    while (!js_at_punct(P, ')') && P->L.tok.type != T_EOF && !P->L.failed) {
        int e;
        int cl = P->L.tok.line;
        if (js_at_op(P, OP_SPREAD)) {
            js_next(&P->L);
            int inner;
            JS_NESTED(P, inner = js_parse_assign(P));
            e = js_node(J, N_SPREAD, cl);
            if (e >= 0) J->nodes[e].a = inner;
        } else {
            JS_NESTED(P, e = js_parse_assign(P));
        }
        int cell = js_chain_add(P, &ch, line);
        if (cell < 0 || e < 0) break;
        J->nodes[cell].a = e;
        if (!js_eat_punct(P, ',')) break;
    }
    js_expect(P, ')');
    return ch.head;
}

/* Calls, member access, indexing and ?., which all bind tighter than any
   operator and chain left to right. With `member_only`, dots and brackets
   and nothing else, which is what may follow `new` before its arguments. */
static int js_parse_chain(jparse *P, int left, int member_only) {
    jctx *J = P->J;
    int optional = 0;
    if (left >= 0 && left == P->last_arrow) return left;
    for (;;) {
        if (P->L.failed) return left;
        int line = P->L.tok.line;
        int opt = 0;
        if (js_at_op(P, OP_OPTCHAIN)) {
            if (member_only) break;
            js_next(&P->L);
            opt = 1;
            optional = 1;
            /* a?.b, a?.[x] and a?.(y). */
            if (js_at_punct(P, '(')) {
                js_next(&P->L);
                int n = js_node(J, N_CALL, line);
                if (n < 0) return left;
                int args = js_parse_arglist(P, line);
                J->nodes[n].a = left;
                J->nodes[n].b = args;
                J->nodes[n].flags |= NF_OPT;
                left = n;
                continue;
            }
            if (!js_at_punct(P, '[')) goto member_name;
        }
        if (js_eat_punct(P, '.')) {
        member_name:
            if (P->L.tok.type != T_NAME && P->L.tok.type != T_KEYWORD
                && P->L.tok.type != T_PRIVATE) {
                js_parse_fail(P, line, "expected a property name after the dot, not ",
                              P->L.tok.text, P->L.tok.len);
                return left;
            }
            int n = js_node(J, N_MEMBER, line);
            if (n < 0) return left;
            J->nodes[n].a = left;
            J->nodes[n].str = js_tok_str(P);
            if (P->L.tok.type == T_PRIVATE) J->nodes[n].flags |= NF_PRIVATE;
            if (opt) J->nodes[n].flags |= NF_OPT;
            js_next(&P->L);
            left = n;
            continue;
        }
        if (js_eat_punct(P, '[')) {
            int idx;
            JS_NESTED(P, idx = js_parse_expr(P));
            js_expect(P, ']');
            int n = js_node(J, N_INDEX, line);
            if (n < 0) return left;
            J->nodes[n].a = left;
            J->nodes[n].b = idx;
            if (opt) J->nodes[n].flags |= NF_OPT;
            left = n;
            continue;
        }
        if (P->L.tok.type == T_TEMPLATE) {
            if (optional) {
                js_parse_fail(P, line, "a template cannot follow ?.", 0, 0);
                return left;
            }
            left = js_parse_template(P, left);
            continue;
        }
        if (member_only) break;
        if (js_at_punct(P, '(')) {
            js_next(&P->L);
            int n = js_node(J, N_CALL, line);
            if (n < 0) return left;
            int args = js_parse_arglist(P, line);
            J->nodes[n].a = left;
            J->nodes[n].b = args;
            left = n;
            continue;
        }
        if (P->L.tok.type == T_PUNCT
            && (P->L.tok.op == OP_INC || P->L.tok.op == OP_DEC)
            && !P->L.tok.nl_before && !optional) {
            int n = js_node(J, N_POSTINC, line);
            if (n < 0) return left;
            J->nodes[n].a = left;
            J->nodes[n].op = (u16)P->L.tok.op;
            js_next(&P->L);
            left = n;
            continue;
        }
        break;
    }
    if (optional) {
        /* The whole chain is one thing that a ?. can cut short, and this is
           where it ends: past here, a null that stopped it is undefined. */
        int n = js_node(J, N_OPTCHAIN, J->nodes[left >= 0 ? left : 0].line);
        if (n < 0) return left;
        J->nodes[n].a = left;
        return n;
    }
    return left;
}

static int js_parse_postfix(jparse *P, int left) { return js_parse_chain(P, left, 0); }

/* What follows `new`: the constructor, which is names, dots and brackets
   or another `new`, and never a call; then its arguments, if it has any.
   The whole chain used to be read first and the last call in it taken as
   the arguments, so `new X().y` was new of X().y rather than the y of
   new X(), and new RegExp('a').test(s) constructed the test. */
static int js_parse_new(jparse *P, int line) {
    jctx *J = P->J;
    int callee;
    if (js_at_word(P, "new")) {
        jtok nx = js_peek(P);
        if (nx.type == T_PUNCT && nx.len == 1 && nx.text[0] == '.') {
            callee = js_parse_chain(P, js_parse_primary(P), 1);
        } else {
            int inner = P->L.tok.line;
            js_next(&P->L);
            callee = js_parse_new(P, inner);
        }
    } else {
        callee = js_parse_chain(P, js_parse_primary(P), 1);
    }
    int n = js_node(J, N_NEW, line);
    if (n < 0) return callee;
    J->nodes[n].a = callee;
    if (js_eat_punct(P, '(')) {
        int args = js_parse_arglist(P, line);
        J->nodes[n].b = args;
    }
    return n;
}

/* An operator and what it applies to. The operand is read into a local
   before it is stored: reading it can grow the node array, which moves it,
   and an assignment straight into J->nodes[n] may have worked out where
   J->nodes[n] was before the move. */
static int js_parse_prefix(jparse *P, ntype kind, int op, int line) {
    jctx *J = P->J;
    int n = js_node(J, kind, line);
    int a = js_parse_unary(P);
    if (n >= 0) { J->nodes[n].op = (u16)op; J->nodes[n].a = a; }
    return n;
}

static int js_parse_unary(jparse *P) {
    int line = P->L.tok.line;
    if (js_parse_deep(P)) return -1;

    if (js_at_word(P, "typeof")) {
        js_next(&P->L);
        return js_parse_prefix(P, N_TYPEOF, OP_NONE, line);
    }
    if (js_at_word(P, "delete")) {
        js_next(&P->L);
        return js_parse_prefix(P, N_DELETE, OP_NONE, line);
    }
    if (js_at_word(P, "void")) {
        js_next(&P->L);
        return js_parse_prefix(P, N_UNARY, OP_NONE, line);
    }
    if (js_at_word(P, "new")) {
        jtok nx = js_peek(P);
        if (!(nx.type == T_PUNCT && nx.len == 1 && nx.text[0] == '.')) {
            js_next(&P->L);
            return js_parse_postfix(P, js_parse_new(P, line));
        }
    }
    /* await, inside an async function, where it is an operator and not a
       name. */
    if (P->fc && (P->fc->flags & FN_ASYNC) && js_at_name(P, "await")) {
        js_next(&P->L);
        return js_parse_prefix(P, N_AWAIT, OP_NONE, line);
    }

    if (P->L.tok.type == T_PUNCT) {
        jop op = P->L.tok.op;
        if (op == OP_NOT || op == OP_BNOT || op == OP_SUB || op == OP_ADD) {
            js_next(&P->L);
            return js_parse_prefix(P, N_UNARY,
                                   op == OP_SUB ? OP_NEG : (op == OP_ADD ? OP_POS : op),
                                   line);
        }
        if (op == OP_INC || op == OP_DEC) {
            js_next(&P->L);
            return js_parse_prefix(P, N_PREINC, op, line);
        }
    }

    return js_parse_postfix(P, js_parse_primary(P));
}

static int js_parse_binary(jparse *P, int min_prec) {
    jctx *J = P->J;
    int left = js_parse_unary(P);
    if (left >= 0 && left == P->last_arrow) return left;

    for (;;) {
        if (P->L.failed) return left;
        jop op = OP_NONE;
        if (P->L.tok.type == T_PUNCT) op = P->L.tok.op;
        else if (!P->no_in && js_at_word(P, "in")) op = OP_IN;
        else if (js_at_word(P, "instanceof")) op = OP_INSTANCEOF;

        int prec = js_prec(op);
        if (!prec || prec < min_prec) return left;
        js_next(&P->L);

        /* ** is the one that goes the other way: 2 ** 3 ** 2 is 2 ** 9. */
        int right = js_parse_binary(P, op == OP_POW ? prec : prec + 1);
        int n = js_node(J, (op == OP_AND || op == OP_OR || op == OP_NULLISH) ? N_LOGICAL : N_BINARY,
                        J->nodes[left >= 0 ? left : 0].line);
        if (n < 0) return left;
        J->nodes[n].op = (u16)op;
        J->nodes[n].a = left;
        J->nodes[n].b = right;
        left = n;
    }
}

static int js_parse_cond(jparse *P) {
    jctx *J = P->J;
    int test = js_parse_binary(P, 1);
    if (test >= 0 && test == P->last_arrow) return test;
    if (!js_at_punct(P, '?')) return test;
    int line = P->L.tok.line;
    js_next(&P->L);
    int yes;
    JS_NESTED(P, yes = js_parse_assign(P));
    js_expect(P, ':');
    int no = js_parse_assign(P);
    int n = js_node(J, N_COND, line);
    if (n < 0) return test;
    J->nodes[n].a = test;
    J->nodes[n].b = yes;
    J->nodes[n].c = no;
    return n;
}

static int js_is_assign_op(jop op) {
    switch (op) {
        case OP_ASSIGN: case OP_ADDEQ: case OP_SUBEQ: case OP_MULEQ: case OP_DIVEQ:
        case OP_MODEQ: case OP_OREQ: case OP_ANDEQ: case OP_POWEQ: case OP_XOREQ:
        case OP_SHLEQ: case OP_SHREQ: case OP_USHREQ: case OP_LANDEQ: case OP_LOREQ:
        case OP_NULLEQ:
            return 1;
        default: return 0;
    }
}

static int js_parse_assign(jparse *P) {
    jctx *J = P->J;
    if (js_parse_deep(P)) return -1;
    int line = P->L.tok.line;

    /* yield, in a generator, which is an operator of the lowest kind: it
       takes a whole assignment expression, or nothing. */
    if (P->fc && (P->fc->flags & FN_GEN) && js_at_name(P, "yield")) {
        js_next(&P->L);
        int n = js_node(J, N_YIELD, line);
        if (n < 0) return -1;
        if (js_at_op(P, OP_MUL)) {
            js_next(&P->L);
            J->nodes[n].op = 1;
        }
        int ends = P->L.tok.nl_before || P->L.tok.type == T_EOF
                || js_at_punct(P, ')') || js_at_punct(P, ']') || js_at_punct(P, '}')
                || js_at_punct(P, ',') || js_at_punct(P, ';') || js_at_punct(P, ':')
                || (js_at_word(P, "in") && P->no_in);
        if (!ends || J->nodes[n].op) {
            int e = js_parse_assign(P);
            J->nodes[n].a = e;
        }
        return n;
    }

    int left = js_parse_cond(P);
    if (left >= 0 && left == P->last_arrow) return left;
    if (P->L.tok.type != T_PUNCT || !js_is_assign_op(P->L.tok.op)) return left;

    jop op = P->L.tok.op;
    line = P->L.tok.line;
    if (left >= 0) {
        int k = J->nodes[left].kind;
        if (op == OP_ASSIGN && (k == N_ARRAY || k == N_OBJECT) && !(J->nodes[left].flags & NF_PAREN))
            left = js_to_pattern(P, left, 0);
        else if (k != N_IDENT && k != N_MEMBER && k != N_INDEX && k != N_SUPERMEMBER) {
            js_parse_fail(P, line, "this cannot be assigned to", 0, 0);
            return -1;
        }
    }
    js_next(&P->L);
    int right = js_parse_assign(P);
    /* A function assigned to a name is called by it. */
    if (left >= 0 && right >= 0 && J->nodes[left].kind == N_IDENT && op == OP_ASSIGN
        && (J->nodes[right].kind == N_FUNC || J->nodes[right].kind == N_CLASS)
        && !J->nodes[right].str)
        J->nodes[right].str = J->nodes[left].str;
    int n = js_node(J, N_ASSIGN, line);
    if (n < 0) return left;
    J->nodes[n].op = (u16)op;
    J->nodes[n].a = left;
    J->nodes[n].b = right;
    return n;
}

static int js_parse_expr(jparse *P) {
    jctx *J = P->J;
    int e = js_parse_assign(P);
    while (js_at_punct(P, ',') && !P->L.failed) {
        int line = P->L.tok.line;
        js_next(&P->L);
        int rhs = js_parse_assign(P);
        int n = js_node(J, N_SEQ, line);
        if (n < 0) break;
        J->nodes[n].a = e;
        J->nodes[n].c = rhs;       /* c, so b stays the list link */
        e = n;
    }
    return e;
}

/* --- templates -------------------------------------------------------------
 *
 * `a ${b} c` is a chain of pieces: each cell has the text before an
 * expression (str), the text as it was written (a N_STR in d), and the
 * expression (a), -1 after the last piece. A tag, when there is one, gets the
 * pieces and the values rather than the joined text. */
static int js_parse_template(jparse *P, int tag) {
    jctx *J = P->J;
    int line = P->L.tok.line;
    int n = js_node(J, N_TEMPLATE, line);
    if (n < 0) return -1;
    jchain ch = { -1, -1 };
    for (;;) {
        if (P->L.tok.type != T_TEMPLATE) {
            js_parse_fail(P, P->L.tok.line, "a template is not closed", 0, 0);
            return -1;
        }
        if (P->L.tok.bad_escape && tag < 0) {
            js_parse_fail(P, P->L.tok.line, "an escape a template may not have", 0, 0);
            return -1;
        }
        int cell = js_chain_add(P, &ch, P->L.tok.line);
        if (cell < 0) return -1;
        jstr *cooked = P->L.tok.bad_escape ? 0 : js_str_n(J, P->L.tok.text, P->L.tok.len);
        int raw = js_node(J, N_STR, line);
        if (raw >= 0) J->nodes[raw].str = js_str_n(J, P->L.tok.raw, P->L.tok.rawlen);
        J->nodes[cell].str = cooked;
        J->nodes[cell].d = raw;
        J->nodes[cell].op = (u16)(cooked ? 0 : 1);
        if (P->L.tok.flags & 1) {
            js_next(&P->L);
            break;
        }
        js_next(&P->L);
        int e;
        JS_NESTED(P, e = js_parse_expr(P));
        J->nodes[cell].a = e;
        if (!js_at_punct(P, '}')) {
            js_parse_fail(P, P->L.tok.line, "expected } to end ${ in a template", 0, 0);
            return -1;
        }
        /* The } is the lexer's current token and its position is just past
           it, which is where the template carries on. */
        js_template_piece(&P->L);
        if (P->L.failed) return -1;
    }
    J->nodes[n].a = ch.head;
    if (tag < 0) return n;
    int t = js_node(J, N_TAGGED, line);
    if (t < 0) return -1;
    J->nodes[t].a = tag;
    J->nodes[t].b = n;
    return t;
}

/* --- functions -------------------------------------------------------------
 *
 * The parameters and the body, the name and the `function` already behind.
 * Each function gets a context of its own for what its body says and
 * declares; `flags` is what kind of function it is. */
static int js_parse_params(jparse *P, int *nformal, int *simple) {
    jctx *J = P->J;
    jchain ch = { -1, -1 };
    int counting = 1;
    *nformal = 0;
    *simple = 1;
    js_expect(P, '(');
    while (!js_at_punct(P, ')') && P->L.tok.type != T_EOF && !P->L.failed) {
        int line = P->L.tok.line;
        int cell = js_chain_add(P, &ch, line);
        if (cell < 0) break;
        if (js_at_op(P, OP_SPREAD)) {
            js_next(&P->L);
            J->nodes[cell].op = 1;
            *simple = 0;
            counting = 0;
        }
        int t = js_parse_binding(P);
        if (t < 0) break;
        J->nodes[cell].a = t;
        if (J->nodes[t].kind == N_IDENT) J->nodes[cell].str = J->nodes[t].str;
        else { *simple = 0; }
        if (js_at_op(P, OP_ASSIGN)) {
            js_next(&P->L);
            int d;
            JS_NESTED(P, d = js_parse_assign(P));
            J->nodes[cell].c = d;
            *simple = 0;
            counting = 0;
        }
        if (counting) (*nformal)++;
        if (!js_eat_punct(P, ',')) break;
    }
    js_expect(P, ')');
    return ch.head;
}

/* "use strict" at the head of a body. */
static int js_directive_strict(jparse *P) {
    jlex save = P->L;
    int strict = 0;
    while (P->L.tok.type == T_STRING) {
        if (js_is_word(P->L.tok.text, P->L.tok.len, "use strict")) strict = 1;
        js_next(&P->L);
        if (js_at_punct(P, ';')) js_next(&P->L);
        else if (!(P->L.tok.nl_before || js_at_punct(P, '}') || P->L.tok.type == T_EOF)) break;
    }
    P->L = save;
    return strict;
}

static int js_parse_block(jparse *P);

static int js_parse_function_rest(jparse *P, int flags, jstr *name, int line, ntype kind) {
    jctx *J = P->J;
    int n = js_node(J, kind, line);
    if (n < 0) return -1;
    J->nodes[n].str = name;

    jfnctx fc = { -1, -1, (flags & (FN_ASYNC | FN_GEN)) | (P->fc ? (P->fc->flags & FN_STRICT) : 0)
                          | (flags & FN_STRICT), 0, 0 };
    jfnctx *outer = P->fc;
    P->fc = &fc;
    int keep_in = P->no_in;
    P->no_in = 0;

    int nformal, simple;
    int params = js_parse_params(P, &nformal, &simple);
    if (!js_at_punct(P, '{')) {
        js_parse_fail(P, P->L.tok.line, "expected a function body, not ", P->L.tok.text,
                      P->L.tok.len);
        P->fc = outer;
        return n;
    }
    js_next(&P->L);
    if (js_directive_strict(P)) fc.flags |= FN_STRICT;
    /* The body, read as a block but without the brace already taken. */
    int body = js_node(J, N_BLOCK, line);
    jchain ch = { -1, -1 };
    while (!js_at_punct(P, '}') && P->L.tok.type != T_EOF && !P->L.failed) {
        int st = js_parse_stmt(P);
        if (st < 0) break;
        int cell = js_chain_add(P, &ch, line);
        if (cell < 0) break;
        J->nodes[cell].a = st;
    }
    js_expect(P, '}');
    P->fc = outer;
    P->no_in = keep_in;

    if (body >= 0) {
        J->nodes[body].a = ch.head;
        J->nodes[body].flags |= NF_BODY;
        J->nodes[body].c = fc.vars;
    }
    int fl = (flags & ~FN_STRICT) | (fc.flags & FN_STRICT) | fc.uses | (simple ? FN_SIMPLE : 0);
    J->nodes[n].a = body;
    J->nodes[n].b = params;
    J->nodes[n].d = nformal;
    J->nodes[n].op = (u16)fl;
    return n;
}

/* function name(a, b) { ... }, function* and async function, the async
   already behind us. */
static int js_parse_func(jparse *P, int is_decl, int flags) {
    int line = P->L.tok.line;
    js_eat_word(P, "function");
    if (js_at_op(P, OP_MUL)) {
        js_next(&P->L);
        flags |= FN_GEN;
    }

    jstr *name = 0;
    if (P->L.tok.type == T_NAME) {
        name = js_tok_str(P);
        js_next(&P->L);
    } else if (is_decl) {
        js_parse_fail(P, line, "a function declaration needs a name", 0, 0);
        return -1;
    }
    int f = js_parse_function_rest(P, flags, name, line, is_decl ? N_FUNCDECL : N_FUNC);
    if (f >= 0 && !is_decl && name) P->J->nodes[f].op |= FN_SELFNAME;
    return f;
}

/* --- classes ---------------------------------------------------------------
 *
 * The constructor goes in c (one is made up when there is none), what it
 * extends in a, and every other member in the chain at b: a method, getter or
 * setter with its function in a, or a field with its initialiser -- a
 * function returning the value, so that it runs with the new object as
 * `this` -- or a static block. */
static int js_parse_class(jparse *P, int is_decl) {
    jctx *J = P->J;
    int line = P->L.tok.line;
    js_next(&P->L);                                    /* class */
    jstr *name = 0;
    if (P->L.tok.type == T_NAME) {
        name = js_tok_str(P);
        js_next(&P->L);
    } else if (is_decl) {
        js_parse_fail(P, line, "a class declaration needs a name", 0, 0);
        return -1;
    }
    int n = js_node(J, is_decl ? N_CLASSDECL : N_CLASS, line);
    if (n < 0) return -1;
    J->nodes[n].str = name;
    int heritage = -1;
    if (js_eat_word(P, "extends")) {
        heritage = js_parse_chain(P, js_parse_primary(P), 0);
    }
    J->nodes[n].a = heritage;
    js_expect(P, '{');

    /* A class body is strict, whatever is round it. */
    jfnctx fc = { -1, -1, FN_STRICT, 0, 0 };
    int keep_strict = P->fc ? P->fc->flags : 0;
    if (P->fc) P->fc->flags |= FN_STRICT;
    (void)fc;

    int ctor = -1;
    jchain ch = { -1, -1 };
    while (!js_at_punct(P, '}') && P->L.tok.type != T_EOF && !P->L.failed) {
        if (js_eat_punct(P, ';')) continue;
        int mline = P->L.tok.line;
        int is_static = 0;
        int kind = PK_METHOD;
        int fflags = FN_METHOD | FN_STRICT;

        if (js_at_name(P, "static")) {
            jtok nx = js_peek(P);
            int follows = !(nx.type == T_PUNCT && nx.len == 1
                            && (nx.text[0] == '(' || nx.text[0] == '=' || nx.text[0] == ';'
                                || nx.text[0] == '}'));
            if (follows) {
                is_static = 1;
                js_next(&P->L);
                if (js_at_punct(P, '{')) {
                    /* static { ... }, run once with the class as `this`. */
                    int f = js_node(J, N_FUNC, mline);
                    jfnctx bc = { -1, -1, FN_STRICT, 0, 0 };
                    jfnctx *outer = P->fc;
                    P->fc = &bc;
                    int body = js_parse_block(P);
                    P->fc = outer;
                    if (body >= 0) {
                        J->nodes[body].flags |= NF_BODY;
                        J->nodes[body].flags &= (u8)~NF_SCOPE;
                        J->nodes[body].c = bc.vars;
                    }
                    if (f >= 0) {
                        J->nodes[f].a = body;
                        J->nodes[f].b = -1;
                        J->nodes[f].d = 0;
                        J->nodes[f].op = (u16)(FN_METHOD | FN_STRICT | FN_SIMPLE | bc.uses);
                    }
                    int cell = js_chain_add(P, &ch, mline);
                    if (cell < 0) break;
                    J->nodes[cell].op = PK_BLOCK;
                    J->nodes[cell].flags |= NF_STATIC;
                    J->nodes[cell].a = f;
                    continue;
                }
            }
        }
        if (js_at_name(P, "get") || js_at_name(P, "set")) {
            jtok nx = js_peek(P);
            int follows = nx.type == T_NAME || nx.type == T_KEYWORD || nx.type == T_STRING
                       || nx.type == T_NUM || nx.type == T_PRIVATE
                       || (nx.type == T_PUNCT && nx.len == 1 && nx.text[0] == '[');
            if (follows) {
                kind = P->L.tok.text[0] == 'g' ? PK_GET : PK_SET;
                fflags |= kind == PK_GET ? FN_GETTER : FN_SETTER;
                js_next(&P->L);
            }
        } else if (js_at_name(P, "async")) {
            jtok nx = js_peek(P);
            int follows = !nx.nl_before && (nx.type == T_NAME || nx.type == T_KEYWORD
                       || nx.type == T_STRING || nx.type == T_NUM || nx.type == T_PRIVATE
                       || (nx.type == T_PUNCT && nx.len == 1 && (nx.text[0] == '[' || nx.text[0] == '*')));
            if (follows) {
                fflags |= FN_ASYNC;
                js_next(&P->L);
            }
        }
        if (js_at_op(P, OP_MUL)) {
            fflags |= FN_GEN;
            js_next(&P->L);
        }

        jstr *key;
        int computed, priv;
        if (!js_parse_prop_key(P, &key, &computed, &priv)) break;

        if (js_at_punct(P, '(')) {
            int is_ctor = !is_static && computed < 0 && !priv && kind == PK_METHOD
                       && !(fflags & (FN_ASYNC | FN_GEN)) && js_str_is(key, "constructor");
            if (is_ctor) {
                int cf = FN_CTOR | FN_STRICT | (heritage >= 0 ? FN_DERIVED : 0);
                ctor = js_parse_function_rest(P, cf, name, mline, N_FUNC);
                continue;
            }
            int f = js_parse_function_rest(P, fflags, key, mline, N_FUNC);
            int cell = js_chain_add(P, &ch, mline);
            if (cell < 0) break;
            J->nodes[cell].op = (u16)kind;
            J->nodes[cell].str = key;
            J->nodes[cell].c = computed;
            J->nodes[cell].a = f;
            if (is_static) J->nodes[cell].flags |= NF_STATIC;
            if (priv) J->nodes[cell].flags |= NF_PRIVATE;
            continue;
        }
        if (kind != PK_METHOD || (fflags & (FN_ASYNC | FN_GEN))) {
            js_parse_fail(P, P->L.tok.line, "expected (, not ", P->L.tok.text, P->L.tok.len);
            break;
        }
        /* A field: `name = value;` or just `name;`. */
        int cell = js_chain_add(P, &ch, mline);
        if (cell < 0) break;
        J->nodes[cell].op = PK_FIELD;
        J->nodes[cell].str = key;
        J->nodes[cell].c = computed;
        if (is_static) J->nodes[cell].flags |= NF_STATIC;
        if (priv) J->nodes[cell].flags |= NF_PRIVATE;
        if (js_at_op(P, OP_ASSIGN)) {
            js_next(&P->L);
            int f = js_node(J, N_FUNC, mline);
            jfnctx ic = { -1, -1, FN_STRICT, 0, 0 };
            jfnctx *outer = P->fc;
            P->fc = &ic;
            int e;
            JS_NESTED(P, e = js_parse_assign(P));
            P->fc = outer;
            if (e >= 0 && (J->nodes[e].kind == N_FUNC || J->nodes[e].kind == N_CLASS)
                && !J->nodes[e].str)
                J->nodes[e].str = key;
            if (f >= 0) {
                J->nodes[f].a = e;
                J->nodes[f].b = -1;
                J->nodes[f].d = 0;
                J->nodes[f].op = (u16)(FN_METHOD | FN_STRICT | FN_SIMPLE | FN_EXPR | FN_FIELD
                                       | (ic.uses & (FN_SUPER | FN_NEWTARGET)));
            }
            J->nodes[cell].a = f;
        }
        js_semicolon(P);
    }
    js_expect(P, '}');
    if (P->fc) P->fc->flags = keep_strict;

    if (ctor < 0) {
        /* Made up: an empty constructor, or for a class that extends
           another, one that hands its arguments to the other's. */
        ctor = js_node(J, N_FUNC, line);
        int body = js_node(J, N_BLOCK, line);
        if (ctor >= 0 && body >= 0) {
            J->nodes[body].flags |= NF_BODY;
            J->nodes[ctor].str = name;
            J->nodes[ctor].a = body;
            J->nodes[ctor].b = -1;
            J->nodes[ctor].d = 0;
            J->nodes[ctor].flags |= NF_STATIC;         /* made up */
            J->nodes[ctor].op = (u16)(FN_CTOR | FN_STRICT | FN_SIMPLE
                                      | (heritage >= 0 ? FN_DERIVED : 0));
        }
    }
    J->nodes[n].b = ch.head;
    J->nodes[n].c = ctor;
    return n;
}

/* --- statements -------------------------------------------------------------
 *
 * var a = 1, b; and let and const, with a name or a pattern for each. The
 * declarations themselves, with the keyword already behind us: the for loop
 * has to look past the keyword to find out what kind of loop it is. */
static int js_parse_var_list(jparse *P, int line, int kind, int in_for) {
    jctx *J = P->J;
    int head = js_node(J, N_VAR, line);
    if (head < 0) return -1;
    J->nodes[head].d = kind;
    jchain ch = { -1, -1 };
    for (;;) {
        int cline = P->L.tok.line;
        int t = js_parse_binding(P);
        if (t < 0) break;
        int cell = js_chain_add(P, &ch, cline);
        if (cell < 0) break;
        J->nodes[cell].c = t;
        if (J->nodes[t].kind == N_IDENT) J->nodes[cell].str = J->nodes[t].str;
        if (kind == VK_VAR) js_note_pattern_vars(P, t);
        if (js_at_op(P, OP_ASSIGN)) {
            js_next(&P->L);
            int init = js_parse_assign(P);
            if (init >= 0 && J->nodes[t].kind == N_IDENT
                && (J->nodes[init].kind == N_FUNC || J->nodes[init].kind == N_CLASS)
                && !J->nodes[init].str)
                J->nodes[init].str = J->nodes[t].str;
            J->nodes[cell].a = init;
        } else if (J->nodes[t].kind != N_IDENT && !in_for) {
            js_parse_fail(P, cline, "a pattern has to be given a value", 0, 0);
            break;
        }
        if (!js_eat_punct(P, ',')) break;
    }
    J->nodes[head].a = ch.head;
    return head;
}

static int js_decl_kind(jparse *P) {
    if (js_at_word(P, "var")) return VK_VAR;
    if (js_at_word(P, "const")) return VK_CONST;
    if (js_at_word(P, "let")) {
        /* let is a keyword here, but `let` on its own as a name is rare
           enough that it is always the declaration. */
        return VK_LET;
    }
    return 0;
}

static int js_parse_var(jparse *P) {
    int line = P->L.tok.line;
    int kind = js_decl_kind(P);
    js_next(&P->L);                       /* the var, let or const */
    int n = js_parse_var_list(P, line, kind, 0);
    js_semicolon(P);
    return n;
}

/* Whether a statement declares something block-scoped, which gives the
   block it is in a scope of its own. */
static int js_is_lexical(jctx *J, int st) {
    if (st < 0) return 0;
    int k = J->nodes[st].kind;
    return (k == N_VAR && J->nodes[st].d != VK_VAR) || k == N_CLASSDECL || k == N_FUNCDECL;
}

static int js_parse_block(jparse *P) {
    jctx *J = P->J;
    int line = P->L.tok.line;
    js_expect(P, '{');
    int head = js_node(J, N_BLOCK, line);
    jchain ch = { -1, -1 };
    int lexical = 0;
    while (!js_at_punct(P, '}') && P->L.tok.type != T_EOF && !P->L.failed) {
        int st = js_parse_stmt(P);
        if (st < 0) break;
        if (js_is_lexical(J, st)) {
            lexical = 1;
            /* A function declared in a block belongs to the block, and is
               also a var of the function round it, as the old rule says. */
            if (J->nodes[st].kind == N_FUNCDECL && !(P->fc && (P->fc->flags & FN_STRICT))) {
                J->nodes[st].flags |= NF_PAREN;          /* marks: copy out */
                js_note_var(P, J->nodes[st].str, J->nodes[st].line);
            }
        }
        int cell = js_chain_add(P, &ch, line);
        if (cell < 0) break;
        J->nodes[cell].a = st;
    }
    js_expect(P, '}');
    if (head >= 0) {
        J->nodes[head].a = ch.head;
        if (lexical) J->nodes[head].flags |= NF_SCOPE;
    }
    return head;
}

/* `outer: for (;;) { ... break outer; }`
 *
 * Minified script is full of these, and a name followed by a colon is
 * otherwise a statement that begins with an expression and then runs into
 * something that cannot follow it -- which is exactly the error this used
 * to give. */
static int js_try_label(jparse *P) {
    jctx *J = P->J;
    if (P->L.tok.type != T_NAME) return -1;
    jtok nx = js_peek(P);
    if (!(nx.type == T_PUNCT && nx.len == 1 && nx.text[0] == ':')) return -1;
    int line = P->L.tok.line;
    jstr *name = js_tok_str(P);
    js_next(&P->L);
    js_next(&P->L);

    int n = js_node(J, N_LABEL, line);
    if (n < 0) return -1;
    J->nodes[n].str = name;
    int body = js_parse_stmt(P);
    J->nodes[n].a = body;
    return n;
}

static int js_parse_stmt_in(jparse *P);

/* A statement is outside the reach of any for clause around it: a function
   body written inside one has the `in` operator back. */
static int js_parse_stmt(jparse *P) {
    int r;
    if (js_parse_deep(P)) return -1;
    JS_NESTED(P, r = js_parse_stmt_in(P));
    return r;
}

/* The head of a for-in or for-of, once the left side is known: what it
   iterates and the body. */
static int js_parse_for_rest(jparse *P, ntype kind, int target, int decl, int is_await, int line) {
    jctx *J = P->J;
    js_next(&P->L);                                    /* in or of */
    int obj = kind == N_FOROF ? js_parse_assign(P) : js_parse_expr(P);
    js_expect(P, ')');
    int body = js_parse_stmt(P);
    int n = js_node(J, kind, line);
    if (n < 0) return -1;
    J->nodes[n].a = obj;
    J->nodes[n].b = body;
    J->nodes[n].c = target;
    J->nodes[n].d = decl;
    J->nodes[n].op = (u16)is_await;
    return n;
}

static int js_parse_stmt_in(jparse *P) {
    jctx *J = P->J;
    {
        int lab = js_try_label(P);
        if (lab >= 0) return lab;
    }
    int line = P->L.tok.line;
    if (P->L.failed) return -1;

    if (js_at_punct(P, '{')) return js_parse_block(P);
    if (js_eat_punct(P, ';')) return js_node(J, N_EMPTY, line);

    if (js_decl_kind(P)) {
        /* `let` followed by something that cannot be declared is the name. */
        return js_parse_var(P);
    }

    if (js_at_word(P, "function")) return js_parse_func(P, 1, 0);
    if (js_at_name(P, "async")) {
        jtok nx = js_peek(P);
        if (!nx.nl_before && nx.type == T_KEYWORD && js_is_word(nx.text, nx.len, "function")) {
            js_next(&P->L);
            return js_parse_func(P, 1, FN_ASYNC);
        }
    }
    if (js_at_word(P, "class")) return js_parse_class(P, 1);

    if (js_eat_word(P, "debugger")) {
        js_semicolon(P);
        return js_node(J, N_EMPTY, line);
    }

    /* with (o) body: o's properties as names, in old sloppy scripts. */
    if (js_eat_word(P, "with")) {
        js_expect(P, '(');
        int obj = js_parse_expr(P);
        js_expect(P, ')');
        int body = js_parse_stmt(P);
        int n = js_node(J, N_WITH, line);
        if (n < 0) return -1;
        J->nodes[n].a = obj;
        J->nodes[n].b = body;
        return n;
    }

    if (js_eat_word(P, "if")) {
        js_expect(P, '(');
        int test = js_parse_expr(P);
        js_expect(P, ')');
        int yes = js_parse_stmt(P);
        int no = -1;
        if (js_eat_word(P, "else")) no = js_parse_stmt(P);
        int n = js_node(J, N_IF, line);
        if (n < 0) return -1;
        J->nodes[n].a = test;
        J->nodes[n].b = yes;
        J->nodes[n].c = no;
        return n;
    }

    if (js_eat_word(P, "while")) {
        js_expect(P, '(');
        int test = js_parse_expr(P);
        js_expect(P, ')');
        int body = js_parse_stmt(P);
        int n = js_node(J, N_WHILE, line);
        if (n < 0) return -1;
        J->nodes[n].a = test;
        J->nodes[n].b = body;
        return n;
    }

    if (js_eat_word(P, "do")) {
        int body = js_parse_stmt(P);
        if (!js_eat_word(P, "while")) {
            js_parse_fail(P, P->L.tok.line, "expected while after do", 0, 0);
            return -1;
        }
        js_expect(P, '(');
        int test = js_parse_expr(P);
        js_expect(P, ')');
        /* A do-while needs no semicolon even on the same line. */
        js_eat_punct(P, ';');
        int n = js_node(J, N_DO, line);
        if (n < 0) return -1;
        J->nodes[n].a = test;
        J->nodes[n].b = body;
        return n;
    }

    if (js_eat_word(P, "for")) {
        int is_await = 0;
        if (P->fc && (P->fc->flags & FN_ASYNC) && js_at_name(P, "await")) {
            js_next(&P->L);
            is_await = 1;
        }
        js_expect(P, '(');

        /* for (x in y), for (x of y) and the three part one are different
           statements, and which it is cannot be known until the `in` or the
           `of` is reached. */
        int init = -1;
        int decl = js_decl_kind(P);
        if (decl) {
            int dline = P->L.tok.line;
            js_next(&P->L);
            P->no_in = 1;
            int list = js_parse_var_list(P, dline, decl, 1);
            P->no_in = 0;
            if (P->L.failed) return -1;
            int first = list >= 0 ? J->nodes[list].a : -1;
            if (first >= 0 && J->nodes[first].b < 0 && J->nodes[first].a < 0
                && (js_at_word(P, "in") || js_at_name(P, "of"))) {
                ntype k = js_at_word(P, "in") ? N_FORIN : N_FOROF;
                return js_parse_for_rest(P, k, J->nodes[first].c, decl, is_await, line);
            }
            if (first >= 0 && J->nodes[first].b < 0 && js_at_word(P, "in") && decl == VK_VAR) {
                /* for (var i = 0 in o), which an old rule still allows. */
                return js_parse_for_rest(P, N_FORIN, J->nodes[first].c, decl, 0, line);
            }
            init = list;
            js_expect(P, ';');
        } else if (!js_at_punct(P, ';')) {
            P->no_in = 1;
            int e = js_parse_expr(P);
            P->no_in = 0;
            if (js_at_word(P, "in") || js_at_name(P, "of")) {
                ntype k = js_at_word(P, "in") ? N_FORIN : N_FOROF;
                int t = e;
                if (e >= 0 && (J->nodes[e].kind == N_ARRAY || J->nodes[e].kind == N_OBJECT))
                    t = js_to_pattern(P, e, 0);
                return js_parse_for_rest(P, k, t, 0, is_await, line);
            }
            int wrap = js_node(J, N_EXPRSTMT, line);
            if (wrap >= 0) { J->nodes[wrap].a = e; init = wrap; }
            js_expect(P, ';');
        } else {
            js_expect(P, ';');
        }

        int test = js_at_punct(P, ';') ? -1 : js_parse_expr(P);
        js_expect(P, ';');
        int step = js_at_punct(P, ')') ? -1 : js_parse_expr(P);
        js_expect(P, ')');
        int body = js_parse_stmt(P);

        int n = js_node(J, N_FOR, line);
        if (n < 0) return -1;
        J->nodes[n].a = init;
        J->nodes[n].b = test;
        J->nodes[n].c = step;
        J->nodes[n].d = body;
        if (init >= 0 && J->nodes[init].kind == N_VAR && J->nodes[init].d != VK_VAR)
            J->nodes[n].flags |= NF_SCOPE;
        return n;
    }

    if (js_eat_word(P, "return")) {
        int n = js_node(J, N_RETURN, line);
        if (n < 0) return -1;
        /* The one place the optional semicolon rule is not a convenience.
           A newline after return ends the statement, whatever is below. */
        if (!js_at_punct(P, ';') && !js_at_punct(P, '}')
            && P->L.tok.type != T_EOF && !P->L.tok.nl_before) {
            int value = js_parse_expr(P);
            J->nodes[n].a = value;
        }
        js_semicolon(P);
        return n;
    }

    if (js_at_word(P, "break") || js_at_word(P, "continue")) {
        int is_continue = js_at_word(P, "continue");
        js_next(&P->L);
        int n = js_node(J, is_continue ? N_CONTINUE : N_BREAK, line);
        /* `break outer`, which only means anything on the same line: a name
           on the next line is the next statement. */
        if (n >= 0 && P->L.tok.type == T_NAME && !P->L.tok.nl_before) {
            J->nodes[n].str = js_tok_str(P);
            js_next(&P->L);
        }
        js_semicolon(P);
        return n;
    }
    if (js_eat_word(P, "throw")) {
        int n = js_node(J, N_THROW, line);
        int value = js_parse_expr(P);
        if (n >= 0) J->nodes[n].a = value;
        js_semicolon(P);
        return n;
    }

    if (js_eat_word(P, "try")) {
        int n = js_node(J, N_TRY, line);
        if (n < 0) return -1;
        int block = js_parse_block(P);
        J->nodes[n].a = block;
        if (js_eat_word(P, "catch")) {
            if (js_eat_punct(P, '(')) {
                int t = js_parse_binding(P);
                J->nodes[n].d = t;
                if (t >= 0 && J->nodes[t].kind == N_IDENT) J->nodes[n].str = J->nodes[t].str;
                js_expect(P, ')');
            }
            block = js_parse_block(P);
            J->nodes[n].b = block;
        }
        if (js_eat_word(P, "finally")) {
            block = js_parse_block(P);
            J->nodes[n].c = block;
        }
        if (J->nodes[n].b < 0 && J->nodes[n].c < 0) {
            js_parse_fail(P, line, "a try needs a catch or a finally", 0, 0);
            return -1;
        }
        return n;
    }

    if (js_eat_word(P, "switch")) {
        js_expect(P, '(');
        int subject = js_parse_expr(P);
        js_expect(P, ')');
        js_expect(P, '{');

        int n = js_node(J, N_SWITCH, line);
        if (n < 0) return -1;
        J->nodes[n].a = subject;

        int tail = -1;
        while (!js_at_punct(P, '}') && P->L.tok.type != T_EOF && !P->L.failed) {
            int cline = P->L.tok.line;
            int test = -1;
            if (js_eat_word(P, "case")) {
                test = js_parse_expr(P);
            } else if (!js_eat_word(P, "default")) {
                js_parse_fail(P, cline, "expected case or default", 0, 0);
                break;
            }
            js_expect(P, ':');

            int arm = js_node(J, N_CASE, cline);
            if (arm < 0) break;
            J->nodes[arm].a = test;

            jchain body = { -1, -1 };
            while (!js_at_punct(P, '}') && !js_at_word(P, "case")
                   && !js_at_word(P, "default") && P->L.tok.type != T_EOF
                   && !P->L.failed) {
                int st = js_parse_stmt(P);
                if (st < 0) break;
                if (js_is_lexical(J, st)) J->nodes[n].flags |= NF_SCOPE;
                int cell = js_chain_add(P, &body, cline);
                if (cell < 0) break;
                J->nodes[cell].a = st;
            }
            J->nodes[arm].b = body.head;

            if (tail < 0) J->nodes[n].b = arm;
            else J->nodes[tail].c = arm;
            tail = arm;
        }
        js_expect(P, '}');
        return n;
    }

    int e = js_parse_expr(P);
    js_semicolon(P);
    int n = js_node(J, N_EXPRSTMT, line);
    if (n < 0) return -1;
    J->nodes[n].a = e;
    return n;
}

/* The parser's state at the start of some text. It was left uninitialised
   and the first token was worked out from whatever the stack held. */
static void js_parse_begin(jparse *P, jctx *J, const char *src, u32 len, jfnctx *fc) {
    memset(P, 0, (int)sizeof(*P));
    P->J = J;
    P->no_in = 0;
    P->last_arrow = -1;
    P->fc = fc;
    P->L.J = J;
    P->L.src = src;
    P->L.n = len;
    P->L.at = 0;
    P->L.line = 1;
    P->L.failed = 0;
    P->L.tok.type = T_EOF;
    char here;
    P->stack_floor = J->stack_limit ? J->stack_limit : &here - JS_STACK_BUDGET;
    js_next(&P->L);
}

/* The whole of a script: statements until the end, as one block, with the
   names its var statements declared in c. */
static int js_parse(jctx *J, const char *src, u32 len) {
    jparse P;
    jfnctx fc = { -1, -1, 0, 0, 0 };
    js_parse_begin(&P, J, src, len, &fc);
    if (js_directive_strict(&P)) fc.flags |= FN_STRICT;

    int head = js_node(J, N_BLOCK, 1);
    jchain ch = { -1, -1 };
    while (P.L.tok.type != T_EOF && !P.L.failed && J->sig != JS_FAILED) {
        int st = js_parse_stmt(&P);
        if (st < 0) break;
        int cell = js_chain_add(&P, &ch, 1);
        if (cell < 0) break;
        J->nodes[cell].a = st;
    }
    if (head >= 0) {
        J->nodes[head].a = ch.head;
        J->nodes[head].flags |= NF_BODY;
        J->nodes[head].c = fc.vars;
        /* A strict script's functions are strict; the flag rides on the
           block for whoever runs it. */
        if (fc.flags & FN_STRICT) J->nodes[head].op = FN_STRICT;
    }
    return P.L.failed || J->sig == JS_FAILED ? -1 : head;
}
