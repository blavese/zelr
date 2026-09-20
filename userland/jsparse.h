/* Reading JavaScript: from characters to a tree.
 *
 * A lexer that hands out one token at a time, and a recursive descent parser
 * that turns them into the nodes declared in js.h. The expression grammar is
 * precedence climbing, which is the shortest correct way to get sixteen
 * levels of binary operator right without sixteen functions that all look
 * the same.
 *
 * Two things about JavaScript make this harder than it looks.
 *
 * Semicolons are optional, and the rule for where one is inserted is not
 * "wherever it would help": it is at a newline, at a closing brace, and at
 * the end of the input, and only when the next token could not continue the
 * statement. Getting that wrong turns `return` followed by a newline into a
 * return of whatever was on the line below, which is the one case everybody
 * knows about and the one this has to get right.
 *
 * And a slash is either division or the start of a regular expression, and
 * which it is depends on what came before. This does not implement regular
 * expressions, so a slash is always division — but a page that contains one
 * has to fail with a message that says so rather than producing a tree that
 * means something else.
 */
#pragma once
#include "js.h"

typedef enum {
    T_EOF = 0, T_NUM, T_STRING, T_NAME, T_PUNCT, T_KEYWORD
} ttype;

/* The operators, as one number each, so the parser can switch on them. */
typedef enum {
    OP_NONE = 0,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_LT, OP_GT, OP_LE, OP_GE,
    OP_EQ, OP_NE, OP_SEQ, OP_SNE,
    OP_AND, OP_OR, OP_NOT,
    OP_BAND, OP_BOR, OP_BXOR, OP_BNOT, OP_SHL, OP_SHR, OP_USHR,
    OP_ASSIGN, OP_ADDEQ, OP_SUBEQ, OP_MULEQ, OP_DIVEQ, OP_MODEQ,
    OP_OREQ, OP_ANDEQ, OP_NEG, OP_POS, OP_INC, OP_DEC, OP_IN
} jop;

typedef struct {
    ttype  type;
    jop    op;
    double num;
    const char *text;         /* into the source, not a copy */
    u32    len;
    int    line;
    int    nl_before;         /* a newline came before this token */
} jtok;

typedef struct {
    jctx  *J;
    const char *src;
    u32    n, at;
    int    line;
    jtok   tok;               /* the one being looked at */
    int    nl;                /* a newline has been passed since the last */
    int    failed;
} jlex;

/* --- characters ---------------------------------------------------------- */

static int js_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int js_digit(char c) { return c >= '0' && c <= '9'; }
static int js_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
        || c == '$';
}
static int js_alnum(char c) { return js_alpha(c) || js_digit(c); }

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
 * understood, and refused by name. A script that uses `class` gets told this
 * engine does not have classes, which is a fact somebody can act on; a
 * parser that treated it as an identifier would produce a syntax error four
 * lines later about something unrelated. */
static const char *const JS_WORDS[] = {
    "var", "let", "const", "function", "return", "if", "else", "for",
    "while", "do", "break", "continue", "new", "delete", "typeof", "in",
    "this", "null", "true", "false", "undefined", "throw", "try", "catch",
    "finally", "switch", "case", "default", "void", "instanceof", 0
};

static const char *const JS_UNSUPPORTED[] = {
    "class", "extends", "super", "async", "await", "yield", "import",
    "export", "eval", "with", 0
};

static int js_is_word(const char *s, u32 len, const char *w) {
    u32 i = 0;
    for (; i < len; i++) if (s[i] != w[i] || !w[i]) return 0;
    return w[i] == 0;
}

/* --- one token ----------------------------------------------------------- */

static void js_next(jlex *L) {
    L->nl = 0;

    for (;;) {
        while (L->at < L->n) {
            char c = L->src[L->at];
            if (c == '\n') { L->line++; L->nl = 1; L->at++; continue; }
            if (js_space(c)) { L->at++; continue; }
            if (c == '/' && L->at + 1 < L->n && L->src[L->at + 1] == '/') {
                while (L->at < L->n && L->src[L->at] != '\n') L->at++;
                continue;
            }
            if (c == '/' && L->at + 1 < L->n && L->src[L->at + 1] == '*') {
                L->at += 2;
                while (L->at + 1 < L->n
                       && !(L->src[L->at] == '*' && L->src[L->at + 1] == '/')) {
                    if (L->src[L->at] == '\n') { L->line++; L->nl = 1; }
                    L->at++;
                }
                L->at = L->at + 2 < L->n ? L->at + 2 : L->n;
                continue;
            }
            break;
        }
        break;
    }

    L->tok.line = L->line;
    L->tok.nl_before = L->nl;
    L->tok.op = OP_NONE;
    L->tok.len = 0;
    L->tok.text = L->src + L->at;

    if (L->at >= L->n) { L->tok.type = T_EOF; return; }

    char c = L->src[L->at];

    /* --- a number --------------------------------------------------------- */
    if (js_digit(c) || (c == '.' && L->at + 1 < L->n
                        && js_digit(L->src[L->at + 1]))) {
        u32 start = L->at;
        double v = 0;

        if (c == '0' && L->at + 1 < L->n
            && (L->src[L->at + 1] == 'x' || L->src[L->at + 1] == 'X')) {
            L->at += 2;
            while (L->at < L->n) {
                char d = L->src[L->at];
                int k;
                if (js_digit(d)) k = d - '0';
                else if (d >= 'a' && d <= 'f') k = d - 'a' + 10;
                else if (d >= 'A' && d <= 'F') k = d - 'A' + 10;
                else break;
                v = v * 16.0 + (double)k;
                L->at++;
            }
        } else {
            while (L->at < L->n && js_digit(L->src[L->at])) {
                v = v * 10.0 + (double)(L->src[L->at] - '0');
                L->at++;
            }
            if (L->at < L->n && L->src[L->at] == '.') {
                L->at++;
                double scale = 0.1;
                while (L->at < L->n && js_digit(L->src[L->at])) {
                    v += (double)(L->src[L->at] - '0') * scale;
                    scale *= 0.1;
                    L->at++;
                }
            }
            if (L->at < L->n && (L->src[L->at] == 'e' || L->src[L->at] == 'E')) {
                L->at++;
                int neg = 0;
                if (L->at < L->n && (L->src[L->at] == '+' || L->src[L->at] == '-'))
                    neg = L->src[L->at++] == '-';
                int e = 0;
                while (L->at < L->n && js_digit(L->src[L->at]))
                    e = e * 10 + (L->src[L->at++] - '0');
                double p = 1.0;
                for (int i = 0; i < e; i++) p *= 10.0;
                v = neg ? v / p : v * p;
            }
        }

        L->tok.type = T_NUM;
        L->tok.num = v;
        L->tok.len = L->at - start;
        return;
    }

    /* --- a name or a word ------------------------------------------------- */
    if (js_alpha(c)) {
        u32 start = L->at;
        while (L->at < L->n && js_alnum(L->src[L->at])) L->at++;
        L->tok.text = L->src + start;
        L->tok.len = L->at - start;

        for (int i = 0; JS_UNSUPPORTED[i]; i++) {
            if (js_is_word(L->tok.text, L->tok.len, JS_UNSUPPORTED[i])) {
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

    /* --- a string --------------------------------------------------------- */
    if (c == '"' || c == '\'' || c == '`') {
        char quote = c;
        L->at++;
        u32 start = L->at;
        /* Measured first, then copied, because an escape makes the result
           shorter than the source and the copy needs a size. */
        u32 out = 0;
        u32 scan = L->at;
        while (scan < L->n && L->src[scan] != quote) {
            if (L->src[scan] == '\\' && scan + 1 < L->n) scan++;
            scan++;
            out++;
        }
        if (scan >= L->n) {
            js_fail_at(L->J, L->tok.line, "a string is not closed", 0, 0);
            L->failed = 1;
            L->tok.type = T_EOF;
            return;
        }

        char *buf = (char *)js_alloc(L->J, out + 1);
        if (!buf) { L->tok.type = T_EOF; L->failed = 1; return; }
        u32 w = 0;
        u32 i = start;
        while (i < L->n && L->src[i] != quote) {
            char d = L->src[i];
            if (d == '\\' && i + 1 < L->n) {
                i++;
                char e = L->src[i];
                switch (e) {
                    case 'n': buf[w++] = '\n'; break;
                    case 't': buf[w++] = '\t'; break;
                    case 'r': buf[w++] = '\r'; break;
                    case '0': buf[w++] = 0; break;
                    case 'b': buf[w++] = 8; break;
                    case 'f': buf[w++] = 12; break;
                    case 'v': buf[w++] = 11; break;
                    case 'u': case 'x': {
                        /* Read and folded to a question mark: this machine
                           draws the printable half of ASCII, and a glyph it
                           cannot draw is better as one character somebody
                           can see than as four they have to decode. */
                        int digits = e == 'u' ? 4 : 2;
                        u32 cp = 0;
                        for (int k = 0; k < digits && i + 1 < L->n; k++) {
                            char h = L->src[i + 1];
                            int hv;
                            if (js_digit(h)) hv = h - '0';
                            else if (h >= 'a' && h <= 'f') hv = h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') hv = h - 'A' + 10;
                            else break;
                            cp = cp * 16 + (u32)hv;
                            i++;
                        }
                        buf[w++] = cp < 128 ? (char)cp : '?';
                        break;
                    }
                    default: buf[w++] = e; break;
                }
                i++;
            } else {
                if (d == '\n') L->line++;
                buf[w++] = d;
                i++;
            }
        }
        buf[w] = 0;
        L->at = i + 1;
        L->tok.type = T_STRING;
        L->tok.text = buf;
        L->tok.len = w;
        return;
    }

    /* --- punctuation ------------------------------------------------------ */
    L->tok.type = T_PUNCT;
    L->tok.text = L->src + L->at;

    #define TWO(a, b) (c == (a) && L->at + 1 < L->n && L->src[L->at + 1] == (b))
    #define THREE(a, b, x) (TWO(a, b) && L->at + 2 < L->n \
                            && L->src[L->at + 2] == (x))

    if (THREE('=', '=', '=')) { L->tok.op = OP_SEQ; L->at += 3; L->tok.len = 3; return; }
    if (THREE('!', '=', '=')) { L->tok.op = OP_SNE; L->at += 3; L->tok.len = 3; return; }
    if (THREE('>', '>', '>')) { L->tok.op = OP_USHR; L->at += 3; L->tok.len = 3; return; }

    if (TWO('=', '=')) { L->tok.op = OP_EQ;  L->at += 2; L->tok.len = 2; return; }
    if (TWO('!', '=')) { L->tok.op = OP_NE;  L->at += 2; L->tok.len = 2; return; }
    if (TWO('<', '=')) { L->tok.op = OP_LE;  L->at += 2; L->tok.len = 2; return; }
    if (TWO('>', '=')) { L->tok.op = OP_GE;  L->at += 2; L->tok.len = 2; return; }
    if (TWO('&', '&')) { L->tok.op = OP_AND; L->at += 2; L->tok.len = 2; return; }
    if (TWO('|', '|')) { L->tok.op = OP_OR;  L->at += 2; L->tok.len = 2; return; }
    if (TWO('+', '+')) { L->tok.op = OP_INC; L->at += 2; L->tok.len = 2; return; }
    if (TWO('-', '-')) { L->tok.op = OP_DEC; L->at += 2; L->tok.len = 2; return; }
    if (TWO('+', '=')) { L->tok.op = OP_ADDEQ; L->at += 2; L->tok.len = 2; return; }
    if (TWO('-', '=')) { L->tok.op = OP_SUBEQ; L->at += 2; L->tok.len = 2; return; }
    if (TWO('*', '=')) { L->tok.op = OP_MULEQ; L->at += 2; L->tok.len = 2; return; }
    if (TWO('/', '=')) { L->tok.op = OP_DIVEQ; L->at += 2; L->tok.len = 2; return; }
    if (TWO('%', '=')) { L->tok.op = OP_MODEQ; L->at += 2; L->tok.len = 2; return; }
    if (TWO('|', '=')) { L->tok.op = OP_OREQ;  L->at += 2; L->tok.len = 2; return; }
    if (TWO('&', '=')) { L->tok.op = OP_ANDEQ; L->at += 2; L->tok.len = 2; return; }
    if (TWO('<', '<')) { L->tok.op = OP_SHL; L->at += 2; L->tok.len = 2; return; }
    if (TWO('>', '>')) { L->tok.op = OP_SHR; L->at += 2; L->tok.len = 2; return; }
    if (TWO('=', '>')) {
        js_fail_at(L->J, L->tok.line,
                   "this engine does not have arrow functions", 0, 0);
        L->failed = 1;
        L->tok.type = T_EOF;
        return;
    }

    #undef TWO
    #undef THREE

    switch (c) {
        case '+': L->tok.op = OP_ADD; break;
        case '-': L->tok.op = OP_SUB; break;
        case '*': L->tok.op = OP_MUL; break;
        case '/': L->tok.op = OP_DIV; break;
        case '%': L->tok.op = OP_MOD; break;
        case '<': L->tok.op = OP_LT; break;
        case '>': L->tok.op = OP_GT; break;
        case '=': L->tok.op = OP_ASSIGN; break;
        case '!': L->tok.op = OP_NOT; break;
        case '&': L->tok.op = OP_BAND; break;
        case '|': L->tok.op = OP_BOR; break;
        case '^': L->tok.op = OP_BXOR; break;
        case '~': L->tok.op = OP_BNOT; break;
        default:  L->tok.op = OP_NONE; break;
    }
    L->at++;
    L->tok.len = 1;
}

/* --- the tree ------------------------------------------------------------ */

#define JS_NODES_STEP 512

static int js_node(jctx *J, ntype kind, int line) {
    if (J->nnodes >= J->ncap) {
        int want = J->ncap ? J->ncap * 2 : JS_NODES_STEP;
        jnode *fresh = (jnode *)realloc(J->nodes, (u64)want * sizeof(jnode));
        if (!fresh) { J->sig = JS_FAILED; return -1; }
        J->nodes = fresh;
        J->ncap = want;
    }
    int i = J->nnodes++;
    jnode *n = &J->nodes[i];
    n->kind = kind;
    n->a = n->b = n->c = n->d = -1;
    n->num = 0;
    n->str = 0;
    n->op = OP_NONE;
    n->line = line;
    return i;
}

/* --- the parser ---------------------------------------------------------- */

typedef struct {
    jlex  L;
    jctx *J;
} jparse;

static int js_parse_expr(jparse *P);
static int js_parse_assign(jparse *P);
static int js_parse_stmt(jparse *P);

static int js_at_punct(jparse *P, char c) {
    return P->L.tok.type == T_PUNCT && P->L.tok.len == 1
        && P->L.tok.text[0] == c;
}

static int js_at_word(jparse *P, const char *w) {
    return P->L.tok.type == T_KEYWORD
        && js_is_word(P->L.tok.text, P->L.tok.len, w);
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

static void js_expect(jparse *P, char c) {
    if (js_eat_punct(P, c)) return;
    char want[2];
    want[0] = c;
    want[1] = 0;
    js_fail_at(P->J, P->L.tok.line, "expected ", want, 1);
    P->L.failed = 1;
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
    js_fail_at(P->J, P->L.tok.line,
               "expected a semicolon or a new line after this", 0, 0);
    P->L.failed = 1;
}

/* How tightly each binary operator binds. Higher wins. */
static int js_prec(jop op) {
    switch (op) {
        case OP_OR:   return 1;
        case OP_AND:  return 2;
        case OP_BOR:  return 3;
        case OP_BXOR: return 4;
        case OP_BAND: return 5;
        case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE: return 6;
        case OP_LT: case OP_GT: case OP_LE: case OP_GE: case OP_IN: return 7;
        case OP_SHL: case OP_SHR: case OP_USHR: return 8;
        case OP_ADD: case OP_SUB: return 9;
        case OP_MUL: case OP_DIV: case OP_MOD: return 10;
        default: return 0;
    }
}

static int js_parse_func(jparse *P, int want_name);

static int js_parse_primary(jparse *P) {
    jctx *J = P->J;
    int line = P->L.tok.line;

    if (P->L.tok.type == T_NUM) {
        int n = js_node(J, N_NUM, line);
        if (n >= 0) J->nodes[n].num = P->L.tok.num;
        js_next(&P->L);
        return n;
    }
    if (P->L.tok.type == T_STRING) {
        int n = js_node(J, N_STR, line);
        if (n >= 0) J->nodes[n].str = js_str_n(J, P->L.tok.text, P->L.tok.len);
        js_next(&P->L);
        return n;
    }
    if (P->L.tok.type == T_NAME) {
        int n = js_node(J, N_IDENT, line);
        if (n >= 0) J->nodes[n].str = js_str_n(J, P->L.tok.text, P->L.tok.len);
        js_next(&P->L);
        return n;
    }

    if (js_at_word(P, "true"))  { js_next(&P->L); return js_node(J, N_TRUE, line); }
    if (js_at_word(P, "false")) { js_next(&P->L); return js_node(J, N_FALSE, line); }
    if (js_at_word(P, "null"))  { js_next(&P->L); return js_node(J, N_NULL, line); }
    if (js_at_word(P, "undefined")) { js_next(&P->L); return js_node(J, N_UNDEF, line); }
    if (js_at_word(P, "this"))  { js_next(&P->L); return js_node(J, N_THIS, line); }
    if (js_at_word(P, "function")) return js_parse_func(P, 0);

    if (js_eat_punct(P, '(')) {
        int e = js_parse_expr(P);
        js_expect(P, ')');
        return e;
    }

    /* An array literal. The elements hang off a chain of nodes rather than a
       list, because the tree is a flat array of fixed size nodes and a chain
       needs no second allocation. */
    if (js_eat_punct(P, '[')) {
        int head = js_node(J, N_ARRAY, line);
        int tail = -1;
        while (!js_at_punct(P, ']') && P->L.tok.type != T_EOF) {
            int e = js_parse_assign(P);
            int cell = js_node(J, N_SEQ, line);
            if (cell < 0 || e < 0) break;
            J->nodes[cell].a = e;
            if (tail < 0) J->nodes[head].a = cell;
            else J->nodes[tail].b = cell;
            tail = cell;
            if (!js_eat_punct(P, ',')) break;
        }
        js_expect(P, ']');
        return head;
    }

    /* An object literal. Keys may be names, strings or numbers. */
    if (js_eat_punct(P, '{')) {
        int head = js_node(J, N_OBJECT, line);
        int tail = -1;
        while (!js_at_punct(P, '}') && P->L.tok.type != T_EOF) {
            jstr *key = 0;
            if (P->L.tok.type == T_NAME || P->L.tok.type == T_KEYWORD)
                key = js_str_n(J, P->L.tok.text, P->L.tok.len);
            else if (P->L.tok.type == T_STRING)
                key = js_str_n(J, P->L.tok.text, P->L.tok.len);
            else if (P->L.tok.type == T_NUM) {
                char tmp[32];
                int w = 0;
                double d = P->L.tok.num;
                long long whole = (long long)d;
                if (whole < 0) { tmp[w++] = '-'; whole = -whole; }
                char rev[24];
                int r = 0;
                if (!whole) rev[r++] = '0';
                while (whole) { rev[r++] = (char)('0' + whole % 10); whole /= 10; }
                while (r) tmp[w++] = rev[--r];
                tmp[w] = 0;
                key = js_str_n(J, tmp, (u32)w);
            } else {
                js_fail_at(J, P->L.tok.line, "expected a property name", 0, 0);
                P->L.failed = 1;
                break;
            }
            js_next(&P->L);
            js_expect(P, ':');
            int v = js_parse_assign(P);

            int cell = js_node(J, N_SEQ, line);
            if (cell < 0 || v < 0) break;
            J->nodes[cell].str = key;
            J->nodes[cell].a = v;
            if (tail < 0) J->nodes[head].a = cell;
            else J->nodes[tail].b = cell;
            tail = cell;
            if (!js_eat_punct(P, ',')) break;
        }
        js_expect(P, '}');
        return head;
    }

    js_fail_at(J, line, "this is not something a value can start with", 0, 0);
    P->L.failed = 1;
    return -1;
}

/* Calls, member access and indexing, which all bind tighter than any
   operator and chain left to right. */
static int js_parse_postfix(jparse *P, int left) {
    jctx *J = P->J;
    for (;;) {
        int line = P->L.tok.line;
        if (js_eat_punct(P, '.')) {
            if (P->L.tok.type != T_NAME && P->L.tok.type != T_KEYWORD) {
                js_fail_at(J, line, "expected a property name after the dot",
                           0, 0);
                P->L.failed = 1;
                return left;
            }
            int n = js_node(J, N_MEMBER, line);
            if (n < 0) return left;
            J->nodes[n].a = left;
            J->nodes[n].str = js_str_n(J, P->L.tok.text, P->L.tok.len);
            js_next(&P->L);
            left = n;
            continue;
        }
        if (js_eat_punct(P, '[')) {
            int idx = js_parse_expr(P);
            js_expect(P, ']');
            int n = js_node(J, N_INDEX, line);
            if (n < 0) return left;
            J->nodes[n].a = left;
            J->nodes[n].b = idx;
            left = n;
            continue;
        }
        if (js_at_punct(P, '(')) {
            js_next(&P->L);
            int n = js_node(J, N_CALL, line);
            if (n < 0) return left;
            J->nodes[n].a = left;
            int tail = -1;
            while (!js_at_punct(P, ')') && P->L.tok.type != T_EOF) {
                int e = js_parse_assign(P);
                int cell = js_node(J, N_SEQ, line);
                if (cell < 0 || e < 0) break;
                J->nodes[cell].a = e;
                if (tail < 0) J->nodes[n].b = cell;
                else J->nodes[tail].b = cell;
                tail = cell;
                if (!js_eat_punct(P, ',')) break;
            }
            js_expect(P, ')');
            left = n;
            continue;
        }
        if (P->L.tok.type == T_PUNCT
            && (P->L.tok.op == OP_INC || P->L.tok.op == OP_DEC)
            && !P->L.tok.nl_before) {
            int n = js_node(J, N_POSTINC, line);
            if (n < 0) return left;
            J->nodes[n].a = left;
            J->nodes[n].op = P->L.tok.op;
            js_next(&P->L);
            left = n;
            continue;
        }
        return left;
    }
}

static int js_parse_unary(jparse *P) {
    jctx *J = P->J;
    int line = P->L.tok.line;

    if (js_at_word(P, "typeof")) {
        js_next(&P->L);
        int n = js_node(J, N_TYPEOF, line);
        if (n >= 0) J->nodes[n].a = js_parse_unary(P);
        return n;
    }
    if (js_at_word(P, "delete")) {
        js_next(&P->L);
        int n = js_node(J, N_DELETE, line);
        if (n >= 0) J->nodes[n].a = js_parse_unary(P);
        return n;
    }
    if (js_at_word(P, "void")) {
        js_next(&P->L);
        int n = js_node(J, N_UNARY, line);
        if (n >= 0) { J->nodes[n].op = OP_NONE; J->nodes[n].a = js_parse_unary(P); }
        return n;
    }
    if (js_at_word(P, "new")) {
        js_next(&P->L);
        int callee = js_parse_postfix(P, js_parse_primary(P));
        int n = js_node(J, N_NEW, line);
        if (n >= 0) {
            /* `new X(a, b)` parses as a call; the arguments are already on
               it, so the node borrows them. */
            if (callee >= 0 && J->nodes[callee].kind == N_CALL) {
                J->nodes[n].a = J->nodes[callee].a;
                J->nodes[n].b = J->nodes[callee].b;
            } else {
                J->nodes[n].a = callee;
            }
        }
        return js_parse_postfix(P, n);
    }

    if (P->L.tok.type == T_PUNCT) {
        jop op = P->L.tok.op;
        if (op == OP_NOT || op == OP_BNOT || op == OP_SUB || op == OP_ADD) {
            js_next(&P->L);
            int n = js_node(J, N_UNARY, line);
            if (n >= 0) {
                J->nodes[n].op = op == OP_SUB ? OP_NEG
                               : (op == OP_ADD ? OP_POS : op);
                J->nodes[n].a = js_parse_unary(P);
            }
            return n;
        }
        if (op == OP_INC || op == OP_DEC) {
            js_next(&P->L);
            int n = js_node(J, N_PREINC, line);
            if (n >= 0) {
                J->nodes[n].op = op;
                J->nodes[n].a = js_parse_unary(P);
            }
            return n;
        }
    }

    return js_parse_postfix(P, js_parse_primary(P));
}

static int js_parse_binary(jparse *P, int min_prec) {
    jctx *J = P->J;
    int left = js_parse_unary(P);

    for (;;) {
        jop op = OP_NONE;
        if (P->L.tok.type == T_PUNCT) op = P->L.tok.op;
        else if (js_at_word(P, "in")) op = OP_IN;
        else if (js_at_word(P, "instanceof")) op = OP_NONE;

        int prec = js_prec(op);
        if (!prec || prec < min_prec) return left;
        js_next(&P->L);

        int right = js_parse_binary(P, prec + 1);
        int n = js_node(J, (op == OP_AND || op == OP_OR) ? N_LOGICAL : N_BINARY,
                        J->nodes[left >= 0 ? left : 0].line);
        if (n < 0) return left;
        J->nodes[n].op = op;
        J->nodes[n].a = left;
        J->nodes[n].b = right;
        left = n;
    }
}

static int js_parse_cond(jparse *P) {
    jctx *J = P->J;
    int test = js_parse_binary(P, 1);
    if (!js_at_punct(P, '?')) return test;
    int line = P->L.tok.line;
    js_next(&P->L);
    int yes = js_parse_assign(P);
    js_expect(P, ':');
    int no = js_parse_assign(P);
    int n = js_node(J, N_COND, line);
    if (n < 0) return test;
    J->nodes[n].a = test;
    J->nodes[n].b = yes;
    J->nodes[n].c = no;
    return n;
}

static int js_parse_assign(jparse *P) {
    jctx *J = P->J;
    int left = js_parse_cond(P);
    if (P->L.tok.type != T_PUNCT) return left;

    jop op = P->L.tok.op;
    if (op != OP_ASSIGN && op != OP_ADDEQ && op != OP_SUBEQ && op != OP_MULEQ
        && op != OP_DIVEQ && op != OP_MODEQ && op != OP_OREQ && op != OP_ANDEQ)
        return left;

    int line = P->L.tok.line;
    js_next(&P->L);
    int right = js_parse_assign(P);
    int n = js_node(J, N_ASSIGN, line);
    if (n < 0) return left;
    J->nodes[n].op = op;
    J->nodes[n].a = left;
    J->nodes[n].b = right;
    return n;
}

static int js_parse_expr(jparse *P) {
    jctx *J = P->J;
    int e = js_parse_assign(P);
    while (js_at_punct(P, ',')) {
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

/* function name(a, b) { ... } */
static int js_parse_func(jparse *P, int want_name) {
    jctx *J = P->J;
    int line = P->L.tok.line;
    js_eat_word(P, "function");

    jstr *name = 0;
    if (P->L.tok.type == T_NAME) {
        name = js_str_n(J, P->L.tok.text, P->L.tok.len);
        js_next(&P->L);
    } else if (want_name) {
        js_fail_at(J, line, "a function declaration needs a name", 0, 0);
        P->L.failed = 1;
        return -1;
    }

    int n = js_node(J, want_name ? N_FUNCDECL : N_FUNC, line);
    if (n < 0) return -1;
    J->nodes[n].str = name;

    js_expect(P, '(');
    int tail = -1, count = 0;
    while (P->L.tok.type == T_NAME) {
        int cell = js_node(J, N_SEQ, line);
        if (cell < 0) break;
        J->nodes[cell].str = js_str_n(J, P->L.tok.text, P->L.tok.len);
        if (tail < 0) J->nodes[n].b = cell;
        else J->nodes[tail].b = cell;
        tail = cell;
        count++;
        js_next(&P->L);
        if (!js_eat_punct(P, ',')) break;
    }
    js_expect(P, ')');
    J->nodes[n].c = count;

    if (!js_at_punct(P, '{')) {
        js_fail_at(J, P->L.tok.line, "expected a function body", 0, 0);
        P->L.failed = 1;
        return n;
    }
    J->nodes[n].a = js_parse_stmt(P);
    return n;
}

/* var a = 1, b; and let and const, which differ from var here only in that
   they are words: there are no blocks with their own scopes in this engine,
   so all three declare in the function they are written in. */
/* The declarations themselves, with the `var` already behind us.
 *
 * Split out because the for loop has to look past the keyword to find out
 * whether it is looking at `for (var i = 0; ...)` or `for (var k in o)`, and
 * by the time it knows, the keyword is gone. The first version called the
 * whole of the statement parser from there, which consumed the name as
 * though it were the keyword and then complained that there was no name. */
static int js_parse_var_list(jparse *P, int line) {
    jctx *J = P->J;
    int head = js_node(J, N_VAR, line);
    int tail = -1;
    for (;;) {
        if (P->L.tok.type != T_NAME) {
            js_fail_at(J, P->L.tok.line, "expected a name to declare", 0, 0);
            P->L.failed = 1;
            break;
        }
        int cell = js_node(J, N_SEQ, P->L.tok.line);
        if (cell < 0) break;
        J->nodes[cell].str = js_str_n(J, P->L.tok.text, P->L.tok.len);
        js_next(&P->L);
        if (js_at_punct(P, '=') && P->L.tok.op == OP_ASSIGN) {
            js_next(&P->L);
            J->nodes[cell].a = js_parse_assign(P);
        }
        if (tail < 0) J->nodes[head].a = cell;
        else J->nodes[tail].b = cell;
        tail = cell;
        if (!js_eat_punct(P, ',')) break;
    }
    return head;
}

static int js_parse_var(jparse *P) {
    int line = P->L.tok.line;
    js_next(&P->L);                       /* the var, let or const */
    int n = js_parse_var_list(P, line);
    js_semicolon(P);
    return n;
}

static int js_parse_block(jparse *P) {
    jctx *J = P->J;
    int line = P->L.tok.line;
    js_expect(P, '{');
    int head = js_node(J, N_BLOCK, line);
    int tail = -1;
    while (!js_at_punct(P, '}') && P->L.tok.type != T_EOF && !P->L.failed) {
        int st = js_parse_stmt(P);
        if (st < 0) break;
        int cell = js_node(J, N_SEQ, line);
        if (cell < 0) break;
        J->nodes[cell].a = st;
        if (tail < 0) J->nodes[head].a = cell;
        else J->nodes[tail].b = cell;
        tail = cell;
    }
    js_expect(P, '}');
    return head;
}

static int js_parse_stmt(jparse *P) {
    jctx *J = P->J;
    int line = P->L.tok.line;
    if (P->L.failed) return -1;

    if (js_at_punct(P, '{')) return js_parse_block(P);
    if (js_eat_punct(P, ';')) return js_node(J, N_EMPTY, line);

    if (js_at_word(P, "var") || js_at_word(P, "let") || js_at_word(P, "const"))
        return js_parse_var(P);

    if (js_at_word(P, "function")) return js_parse_func(P, 1);

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
            js_fail_at(J, P->L.tok.line, "expected while after do", 0, 0);
            P->L.failed = 1;
            return -1;
        }
        js_expect(P, '(');
        int test = js_parse_expr(P);
        js_expect(P, ')');
        js_semicolon(P);
        int n = js_node(J, N_DO, line);
        if (n < 0) return -1;
        J->nodes[n].a = test;
        J->nodes[n].b = body;
        return n;
    }

    if (js_eat_word(P, "for")) {
        js_expect(P, '(');

        /* for (x in y) and for (var x in y) are a different statement from
           the three part one, and which it is cannot be known until the
           `in` is reached. */
        int init = -1;
        jstr *loop_name = 0;
        int declared = 0;

        if (js_at_word(P, "var") || js_at_word(P, "let")
            || js_at_word(P, "const")) {
            declared = 1;
            js_next(&P->L);
            if (P->L.tok.type == T_NAME) {
                loop_name = js_str_n(J, P->L.tok.text, P->L.tok.len);
                jtok saved = P->L.tok;
                u32 save_at = P->L.at;
                int save_line = P->L.line;
                js_next(&P->L);
                if (js_at_word(P, "in")) {
                    js_next(&P->L);
                    int obj = js_parse_expr(P);
                    js_expect(P, ')');
                    int body = js_parse_stmt(P);
                    int n = js_node(J, N_FORIN, line);
                    if (n < 0) return -1;
                    J->nodes[n].str = loop_name;
                    J->nodes[n].a = obj;
                    J->nodes[n].b = body;
                    J->nodes[n].d = 1;
                    return n;
                }
                /* Not a for-in, so put the name back and read the
                   declarations from there. The keyword is already behind
                   us, which is why this calls the list and not the
                   statement. */
                P->L.at = save_at;
                P->L.line = save_line;
                P->L.tok = saved;
                init = js_parse_var_list(P, line);
                /* The separator after the init clause still has to go,
                   and the label below is past the place that eats it. */
                js_expect(P, ';');
                goto have_init;
            }
        }
        (void)declared;

        if (!js_at_punct(P, ';')) {
            init = js_parse_expr(P);
            if (js_at_word(P, "in")) {
                js_next(&P->L);
                int obj = js_parse_expr(P);
                js_expect(P, ')');
                int body = js_parse_stmt(P);
                int n = js_node(J, N_FORIN, line);
                if (n < 0) return -1;
                J->nodes[n].c = init;       /* the target, as an expression */
                J->nodes[n].a = obj;
                J->nodes[n].b = body;
                return n;
            }
            int wrap = js_node(J, N_EXPRSTMT, line);
            if (wrap >= 0) { J->nodes[wrap].a = init; init = wrap; }
        }
        js_expect(P, ';');

    have_init:;
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
        return n;
    }

    if (js_eat_word(P, "return")) {
        int n = js_node(J, N_RETURN, line);
        if (n < 0) return -1;
        /* The one place the optional semicolon rule is not a convenience.
           A newline after return ends the statement, whatever is below. */
        if (!js_at_punct(P, ';') && !js_at_punct(P, '}')
            && P->L.tok.type != T_EOF && !P->L.tok.nl_before)
            J->nodes[n].a = js_parse_expr(P);
        js_semicolon(P);
        return n;
    }

    if (js_eat_word(P, "break")) {
        int n = js_node(J, N_BREAK, line);
        js_semicolon(P);
        return n;
    }
    if (js_eat_word(P, "continue")) {
        int n = js_node(J, N_CONTINUE, line);
        js_semicolon(P);
        return n;
    }
    if (js_eat_word(P, "throw")) {
        int n = js_node(J, N_THROW, line);
        if (n >= 0) J->nodes[n].a = js_parse_expr(P);
        js_semicolon(P);
        return n;
    }

    if (js_eat_word(P, "try")) {
        int n = js_node(J, N_TRY, line);
        if (n < 0) return -1;
        J->nodes[n].a = js_parse_block(P);
        if (js_eat_word(P, "catch")) {
            if (js_eat_punct(P, '(')) {
                if (P->L.tok.type == T_NAME) {
                    J->nodes[n].str = js_str_n(J, P->L.tok.text, P->L.tok.len);
                    js_next(&P->L);
                }
                js_expect(P, ')');
            }
            J->nodes[n].b = js_parse_block(P);
        }
        if (js_eat_word(P, "finally")) J->nodes[n].c = js_parse_block(P);
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
                test = js_parse_assign(P);
            } else if (!js_eat_word(P, "default")) {
                js_fail_at(J, cline, "expected case or default", 0, 0);
                P->L.failed = 1;
                break;
            }
            js_expect(P, ':');

            int arm = js_node(J, N_CASE, cline);
            if (arm < 0) break;
            J->nodes[arm].a = test;

            int btail = -1;
            while (!js_at_punct(P, '}') && !js_at_word(P, "case")
                   && !js_at_word(P, "default") && P->L.tok.type != T_EOF
                   && !P->L.failed) {
                int st = js_parse_stmt(P);
                if (st < 0) break;
                int cell = js_node(J, N_SEQ, cline);
                if (cell < 0) break;
                J->nodes[cell].a = st;
                if (btail < 0) J->nodes[arm].b = cell;
                else J->nodes[btail].b = cell;
                btail = cell;
            }

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

/* The whole of a script: statements until the end, as one block. */
static int js_parse(jctx *J, const char *src, u32 len) {
    jparse P;
    P.J = J;
    P.L.J = J;
    P.L.src = src;
    P.L.n = len;
    P.L.at = 0;
    P.L.line = 1;
    P.L.failed = 0;
    js_next(&P.L);

    int head = js_node(J, N_BLOCK, 1);
    int tail = -1;
    while (P.L.tok.type != T_EOF && !P.L.failed && J->sig != JS_FAILED) {
        int st = js_parse_stmt(&P);
        if (st < 0) break;
        int cell = js_node(J, N_SEQ, 1);
        if (cell < 0) break;
        J->nodes[cell].a = st;
        if (tail < 0) J->nodes[head].a = cell;
        else J->nodes[tail].b = cell;
        tail = cell;
    }
    return P.L.failed || J->sig == JS_FAILED ? -1 : head;
}
