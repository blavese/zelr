/* Regular expressions.
 *
 * Page scripts reach for one within a few lines, and an engine that does not
 * have them throws on the first `/^\s+|\s+$/` it meets, which is to say on
 * most pages that do anything. So there is one here, written the same way as
 * everything else.
 *
 * --- what is here ----------------------------------------------------------
 *
 * Literal characters and `.`; character classes with ranges, negation and
 * escapes inside them; the escapes `\d \D \w \W \s \S \b \B` and the usual
 * `\n \t \r \f \v \0` and `\xNN` and `\uNNNN`; the anchors `^` and `$`;
 * groups, capturing and `(?:` not; alternation; and the quantifiers `*`
 * `+` `?` `{n}` `{n,}` `{n,m}`, each of them greedy or, with a `?` after,
 * lazy. The flags `g`, `i` and `m`.
 *
 * --- what is not -----------------------------------------------------------
 *
 * No lookahead or lookbehind, no backreferences, no named groups, no sticky
 * flag, no unicode property escapes. Those are absent rather than
 * approximated: a pattern this cannot understand is refused when it is
 * compiled, and the script is told, which is a page that says what is wrong
 * rather than a page that silently matches the wrong thing.
 *
 * --- how it matches --------------------------------------------------------
 *
 * The pattern is parsed into a small array of nodes and matched by
 * backtracking. Where the recursion would ordinarily be "match this, then
 * match the rest", the rest is kept on an explicit stack of continuations,
 * because the rest of a group is the rest of the group and then whatever
 * followed the group, and threading that through a return value is how a
 * matcher ends up almost right.
 *
 * A repetition whose body can match nothing -- `(a?)*` -- would otherwise
 * repeat for ever. Each repetition frame remembers where it started, and a
 * turn that consumed nothing and has already met the minimum stops rather
 * than going round again.
 */
#pragma once
#include "zelr.h"

#define RX_NODES   512
#define RX_CLASSES 64
#define RX_CONTS   512
#define RX_CAPS    10
#define RX_STEPS   400000        /* a pattern that will not finish, stopped */

enum {
    RXN_CHAR = 1, RXN_ANY, RXN_CLASS, RXN_GROUP,
    RXN_BOL, RXN_EOL, RXN_WORDB, RXN_NWORDB
};

/* A continuation: what to do once the thing in hand has matched.
 *
 * `rep` says which sort. Nought or more is a repetition to resume, counting
 * what it has already done; -1 is an ordinary "carry on with this node";
 * -2 closes a capture, and then carries on. */
typedef struct {
    int node;
    int parent;
    int rep;
    int at;                      /* where this turn started */
} rxcont;

typedef struct {
    short kind;
    short greedy;
    int   min, max;              /* max < 0 is no limit */
    int   ch;                    /* RXN_CHAR */
    int   cls;                   /* RXN_CLASS: which class */
    int   alt;                   /* RXN_GROUP: first alternative */
    int   cap;                   /* capture number, or -1 */
    int   next;                  /* next term in this sequence */
    int   alt_next;              /* next alternative, on an alternative head */
} rxnode;

typedef struct {
    rxnode nodes[RX_NODES];
    int    nnodes;
    u8     classes[RX_CLASSES][32];
    int    nclasses;
    int    ncaps;

    int    icase, multiline, global;

    /* set while matching */
    const char *s;
    int    len;
    rxcont cont[RX_CONTS];
    int    ncont;
    int    cap_start[RX_CAPS], cap_end[RX_CAPS];
    int    end;
    u32    steps;

    int    ok;                   /* the pattern compiled */
    char   why[64];              /* and if not, what stopped it */
} rx;

/* --- reading a pattern ---------------------------------------------------- */

static int rx_new(rx *R, int kind) {
    if (R->nnodes >= RX_NODES) return -1;
    int n = R->nnodes++;
    rxnode *x = &R->nodes[n];
    x->kind = (short)kind;
    x->greedy = 1;
    x->min = x->max = 1;
    x->ch = 0;
    x->cls = -1;
    x->alt = -1;
    x->cap = -1;
    x->next = -1;
    x->alt_next = -1;
    return n;
}

static void rx_fail(rx *R, const char *why) {
    if (!R->ok) return;
    R->ok = 0;
    int i = 0;
    while (why[i] && i < (int)sizeof(R->why) - 1) { R->why[i] = why[i]; i++; }
    R->why[i] = 0;
}

static void rx_cls_add(rx *R, int c, int lo, int hi) {
    if (c < 0 || c >= RX_CLASSES) return;
    for (int i = lo; i <= hi && i < 256; i++)
        R->classes[c][i >> 3] |= (u8)(1 << (i & 7));
}

static int rx_digit(int c) { return c >= '0' && c <= '9'; }
static int rx_word(int c) {
    return rx_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c == '_';
}
static int rx_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
        || c == '\v';
}

static int rx_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int rx_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One escape, as the character it stands for, or -1 when it is a class
   rather than a character (\d and friends). */
static int rx_escape_char(const char *p, int len, int *at) {
    int c = (*at) < len ? p[(*at)++] : 0;
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case 'f': return '\f';
        case 'v': return '\v';
        case '0': return 0;
        case 'x': {
            int h = 0;
            for (int i = 0; i < 2 && *at < len; i++) {
                int v = rx_hexval(p[*at]);
                if (v < 0) break;
                h = h * 16 + v;
                (*at)++;
            }
            return h;
        }
        case 'u': {
            int h = 0;
            for (int i = 0; i < 4 && *at < len; i++) {
                int v = rx_hexval(p[*at]);
                if (v < 0) break;
                h = h * 16 + v;
                (*at)++;
            }
            /* One byte is all a character is here, so anything above it is
               taken for its low half rather than refused: a pattern with a
               \u in it is usually matching text this machine cannot hold
               either way. */
            return h & 0xFF;
        }
        default: return c;
    }
}

static int rx_is_class_escape(int c) {
    return c == 'd' || c == 'D' || c == 'w' || c == 'W'
        || c == 's' || c == 'S';
}

static void rx_fill_class_escape(rx *R, int c, int which) {
    int neg = (which == 'D' || which == 'W' || which == 'S');
    int kind = rx_lower(which);
    for (int i = 0; i < 256; i++) {
        int in = kind == 'd' ? rx_digit(i)
               : kind == 'w' ? rx_word(i)
               : rx_space(i);
        if (in != neg) rx_cls_add(R, c, i, i);
    }
}

/* [abc], [^a-z], [\d\-] */
static int rx_parse_class(rx *R, const char *p, int len, int *at) {
    if (R->nclasses >= RX_CLASSES) { rx_fail(R, "too many classes"); return -1; }
    int c = R->nclasses++;
    for (int i = 0; i < 32; i++) R->classes[c][i] = 0;

    int neg = 0;
    if (*at < len && p[*at] == '^') { neg = 1; (*at)++; }

    int any = 0;
    while (*at < len && p[*at] != ']') {
        int lo;
        if (p[*at] == '\\') {
            (*at)++;
            int e = *at < len ? p[*at] : 0;
            if (rx_is_class_escape(e)) {
                (*at)++;
                rx_fill_class_escape(R, c, e);
                any = 1;
                continue;
            }
            if (e == 'b') { (*at)++; lo = '\b'; }
            else lo = rx_escape_char(p, len, at);
        } else {
            lo = (unsigned char)p[(*at)++];
        }

        int hi = lo;
        if (*at + 1 < len && p[*at] == '-' && p[*at + 1] != ']') {
            (*at)++;
            if (p[*at] == '\\') {
                (*at)++;
                hi = rx_escape_char(p, len, at);
            } else {
                hi = (unsigned char)p[(*at)++];
            }
        }
        if (hi < lo) { rx_fail(R, "a range that runs backwards"); return -1; }
        rx_cls_add(R, c, lo, hi);
        any = 1;
    }
    if (*at >= len || p[*at] != ']') { rx_fail(R, "a class with no ]"); return -1; }
    (*at)++;
    if (!any) { rx_fail(R, "an empty class"); return -1; }

    if (neg) {
        for (int i = 0; i < 32; i++) R->classes[c][i] = (u8)~R->classes[c][i];
        /* A negated class still does not cross a line, the same way `.`
           does not, so that `[^a]*` cannot run off the end of one. */
        R->classes[c]['\n' >> 3] &= (u8)~(1 << ('\n' & 7));
    }

    /* Folding is done once, here, rather than on every character tested. */
    if (R->icase) {
        for (int i = 'a'; i <= 'z'; i++) {
            int lower = (R->classes[c][i >> 3] >> (i & 7)) & 1;
            int upper = (R->classes[c][(i - 32) >> 3] >> ((i - 32) & 7)) & 1;
            if (lower || upper) {
                rx_cls_add(R, c, i, i);
                rx_cls_add(R, c, i - 32, i - 32);
            }
        }
    }
    return c;
}

static int rx_parse_alt(rx *R, const char *p, int len, int *at, int depth);

/* One atom with whatever quantifier follows it. */
static int rx_parse_term(rx *R, const char *p, int len, int *at, int depth) {
    if (*at >= len) return -1;
    int c = p[*at];
    int n = -1;

    if (c == '(') {
        (*at)++;
        int cap = -1;
        if (*at + 1 < len && p[*at] == '?' && p[*at + 1] == ':') {
            *at += 2;
        } else if (*at + 1 < len && p[*at] == '?'
                   && (p[*at + 1] == '=' || p[*at + 1] == '!'
                       || p[*at + 1] == '<')) {
            rx_fail(R, "lookaround is not here");
            return -1;
        } else {
            cap = R->ncaps < RX_CAPS ? R->ncaps++ : -1;
        }
        n = rx_new(R, RXN_GROUP);
        if (n < 0) { rx_fail(R, "pattern too big"); return -1; }
        R->nodes[n].cap = cap;
        R->nodes[n].alt = rx_parse_alt(R, p, len, at, depth + 1);
        if (*at >= len || p[*at] != ')') { rx_fail(R, "a group with no )"); return -1; }
        (*at)++;
    } else if (c == '[') {
        (*at)++;
        int cls = rx_parse_class(R, p, len, at);
        if (cls < 0) return -1;
        n = rx_new(R, RXN_CLASS);
        if (n < 0) { rx_fail(R, "pattern too big"); return -1; }
        R->nodes[n].cls = cls;
    } else if (c == '.') {
        (*at)++;
        n = rx_new(R, RXN_ANY);
    } else if (c == '^') {
        (*at)++;
        n = rx_new(R, RXN_BOL);
    } else if (c == '$') {
        (*at)++;
        n = rx_new(R, RXN_EOL);
    } else if (c == '\\') {
        (*at)++;
        int e = *at < len ? p[*at] : 0;
        if (rx_is_class_escape(e)) {
            (*at)++;
            if (R->nclasses >= RX_CLASSES) { rx_fail(R, "too many classes"); return -1; }
            int cls = R->nclasses++;
            for (int i = 0; i < 32; i++) R->classes[cls][i] = 0;
            rx_fill_class_escape(R, cls, e);
            n = rx_new(R, RXN_CLASS);
            if (n < 0) { rx_fail(R, "pattern too big"); return -1; }
            R->nodes[n].cls = cls;
        } else if (e == 'b' || e == 'B') {
            (*at)++;
            n = rx_new(R, e == 'b' ? RXN_WORDB : RXN_NWORDB);
        } else if (e >= '1' && e <= '9') {
            rx_fail(R, "a back reference is not here");
            return -1;
        } else {
            int ch = rx_escape_char(p, len, at);
            n = rx_new(R, RXN_CHAR);
            if (n < 0) { rx_fail(R, "pattern too big"); return -1; }
            R->nodes[n].ch = R->icase ? rx_lower(ch) : ch;
        }
    } else if (c == ')' || c == '|') {
        return -1;                          /* the caller's business */
    } else if (c == '*' || c == '+' || c == '?') {
        rx_fail(R, "a quantifier with nothing before it");
        return -1;
    } else {
        (*at)++;
        n = rx_new(R, RXN_CHAR);
        if (n < 0) { rx_fail(R, "pattern too big"); return -1; }
        R->nodes[n].ch = R->icase ? rx_lower((unsigned char)c)
                                  : (unsigned char)c;
    }

    if (n < 0) { rx_fail(R, "pattern too big"); return -1; }

    /* And what follows it. */
    if (*at < len) {
        int q = p[*at];
        int has = 1;
        if (q == '*')      { R->nodes[n].min = 0; R->nodes[n].max = -1; (*at)++; }
        else if (q == '+') { R->nodes[n].min = 1; R->nodes[n].max = -1; (*at)++; }
        else if (q == '?') { R->nodes[n].min = 0; R->nodes[n].max = 1; (*at)++; }
        else if (q == '{') {
            /* Only when it really is one: `a{b}` is four ordinary
               characters and every browser treats it as such. */
            int save = *at, k = *at + 1, lo = 0, hi = -1, sawlo = 0;
            while (k < len && rx_digit(p[k])) { lo = lo * 10 + (p[k] - '0'); k++; sawlo = 1; }
            if (sawlo && k < len && p[k] == '}') { hi = lo; k++; }
            else if (sawlo && k < len && p[k] == ',') {
                k++;
                if (k < len && p[k] == '}') { hi = -1; k++; }
                else {
                    int h = 0, sawhi = 0;
                    while (k < len && rx_digit(p[k])) { h = h * 10 + (p[k] - '0'); k++; sawhi = 1; }
                    if (sawhi && k < len && p[k] == '}') { hi = h; k++; }
                    else { sawlo = 0; }
                }
            } else sawlo = 0;

            if (sawlo) {
                R->nodes[n].min = lo;
                R->nodes[n].max = hi;
                *at = k;
            } else {
                *at = save;
                has = 0;
            }
        } else has = 0;

        if (has && *at < len && p[*at] == '?') { R->nodes[n].greedy = 0; (*at)++; }
    }

    /* Anchors and boundaries take no quantifier that means anything, and a
       page that writes one is not asking for what it says. */
    if (R->nodes[n].kind == RXN_BOL || R->nodes[n].kind == RXN_EOL
        || R->nodes[n].kind == RXN_WORDB || R->nodes[n].kind == RXN_NWORDB) {
        R->nodes[n].min = R->nodes[n].max = 1;
    }
    return n;
}

/* Terms until a | or a ) — one branch. */
static int rx_parse_seq(rx *R, const char *p, int len, int *at, int depth) {
    int head = -1, tail = -1;
    while (*at < len && p[*at] != '|' && p[*at] != ')') {
        int n = rx_parse_term(R, p, len, at, depth);
        if (n < 0) return head;
        if (tail < 0) head = n;
        else R->nodes[tail].next = n;
        tail = n;
    }
    return head;
}

/* Branches separated by | — the contents of a group. */
static int rx_parse_alt(rx *R, const char *p, int len, int *at, int depth) {
    if (depth > 24) { rx_fail(R, "nested too deep"); return -1; }

    int first = -1, last = -1;
    for (;;) {
        int seq = rx_parse_seq(R, p, len, at, depth);
        if (seq < 0) {
            /* An empty branch matches nothing at all, which is legal and is
               how `(a|)` says the a is optional. */
            seq = rx_new(R, RXN_GROUP);
            if (seq < 0) { rx_fail(R, "pattern too big"); return -1; }
            R->nodes[seq].min = R->nodes[seq].max = 0;
        }
        if (last < 0) first = seq;
        else R->nodes[last].alt_next = seq;
        last = seq;

        if (*at < len && p[*at] == '|') { (*at)++; continue; }
        break;
    }
    return first;
}

static int rx_compile(rx *R, const char *pat, int len, const char *flags) {
    for (int i = 0; i < (int)sizeof(*R); i++) ((char *)R)[i] = 0;
    R->ok = 1;
    R->ncaps = 1;                           /* nought is the whole match */

    for (const char *f = flags ? flags : ""; *f; f++) {
        if (*f == 'i') R->icase = 1;
        else if (*f == 'm') R->multiline = 1;
        else if (*f == 'g') R->global = 1;
        else if (*f == 'y' || *f == 'u' || *f == 's') {
            rx_fail(R, "a flag that is not here");
            return 0;
        }
    }

    int at = 0;
    int root = rx_new(R, RXN_GROUP);
    if (root < 0) { rx_fail(R, "pattern too big"); return 0; }
    R->nodes[root].cap = 0;
    R->nodes[root].alt = rx_parse_alt(R, pat, len, &at, 0);
    if (at < len) rx_fail(R, "something left over");
    return R->ok;
}

/* --- matching ------------------------------------------------------------- */

static int rx_run(rx *R, int n, int pos, int cont);
static int rx_rep(rx *R, int n, int pos, int cont, int done);

static int rx_at_word(rx *R, int i) {
    return i >= 0 && i < R->len && rx_word((unsigned char)R->s[i]);
}

static int rx_cont_do(rx *R, int cont, int pos) {
    if (cont < 0) { R->end = pos; return 1; }

    rxcont f = R->cont[cont];
    if (f.rep == -2) {
        int save_s = R->cap_start[f.node], save_e = R->cap_end[f.node];
        R->cap_start[f.node] = f.at;
        R->cap_end[f.node] = pos;
        if (rx_cont_do(R, f.parent, pos)) return 1;
        R->cap_start[f.node] = save_s;
        R->cap_end[f.node] = save_e;
        return 0;
    }
    if (f.rep >= 0) {
        /* A turn that consumed nothing, having already done its minimum,
           would go round for ever. */
        if (pos == f.at && f.rep > R->nodes[f.node].min) return 0;
        return rx_rep(R, f.node, pos, f.parent, f.rep);
    }
    return rx_run(R, f.node, pos, f.parent);
}

static int rx_push(rx *R, int node, int parent, int rep, int at) {
    if (R->ncont >= RX_CONTS) return -1;
    int f = R->ncont++;
    R->cont[f].node = node;
    R->cont[f].parent = parent;
    R->cont[f].rep = rep;
    R->cont[f].at = at;
    return f;
}

/* The atom once, and then whatever comes after it. */
static int rx_atom(rx *R, int n, int pos, int cont) {
    if (++R->steps > RX_STEPS) return 0;

    rxnode *x = &R->nodes[n];
    switch (x->kind) {
        case RXN_CHAR: {
            if (pos >= R->len) return 0;
            int c = (unsigned char)R->s[pos];
            if (R->icase) c = rx_lower(c);
            if (c != x->ch) return 0;
            return rx_cont_do(R, cont, pos + 1);
        }
        case RXN_ANY:
            if (pos >= R->len || R->s[pos] == '\n') return 0;
            return rx_cont_do(R, cont, pos + 1);
        case RXN_CLASS: {
            if (pos >= R->len) return 0;
            int c = (unsigned char)R->s[pos];
            if (!((R->classes[x->cls][c >> 3] >> (c & 7)) & 1)) return 0;
            return rx_cont_do(R, cont, pos + 1);
        }
        case RXN_BOL:
            if (pos == 0) return rx_cont_do(R, cont, pos);
            if (R->multiline && R->s[pos - 1] == '\n')
                return rx_cont_do(R, cont, pos);
            return 0;
        case RXN_EOL:
            if (pos == R->len) return rx_cont_do(R, cont, pos);
            if (R->multiline && R->s[pos] == '\n')
                return rx_cont_do(R, cont, pos);
            return 0;
        case RXN_WORDB:
            if (rx_at_word(R, pos - 1) != rx_at_word(R, pos))
                return rx_cont_do(R, cont, pos);
            return 0;
        case RXN_NWORDB:
            if (rx_at_word(R, pos - 1) == rx_at_word(R, pos))
                return rx_cont_do(R, cont, pos);
            return 0;
        case RXN_GROUP: {
            int save = R->ncont;
            for (int a = x->alt; a >= 0; a = R->nodes[a].alt_next) {
                R->ncont = save;
                int f = cont;
                if (x->cap >= 0) {
                    f = rx_push(R, x->cap, cont, -2, pos);
                    if (f < 0) return 0;
                }
                if (rx_run(R, a, pos, f)) return 1;
            }
            R->ncont = save;
            return 0;
        }
    }
    return 0;
}

static int rx_rep(rx *R, int n, int pos, int cont, int done) {
    if (++R->steps > RX_STEPS) return 0;

    rxnode *x = &R->nodes[n];
    int more = (x->max < 0 || done < x->max);

    if (done >= x->min && !x->greedy)
        if (rx_run(R, x->next, pos, cont)) return 1;

    if (more) {
        int save = R->ncont;
        int f = rx_push(R, n, cont, done + 1, pos);
        if (f >= 0 && rx_atom(R, n, pos, f)) return 1;
        R->ncont = save;
    }

    if (done >= x->min && x->greedy)
        if (rx_run(R, x->next, pos, cont)) return 1;

    return 0;
}

static int rx_run(rx *R, int n, int pos, int cont) {
    if (n < 0) return rx_cont_do(R, cont, pos);
    return rx_rep(R, n, pos, cont, 0);
}

/* The first match at or after `from`. Returns where it started, or -1. */
static int rx_search(rx *R, const char *s, int len, int from) {
    if (!R->ok) return -1;
    R->s = s;
    R->len = len;

    for (int start = from < 0 ? 0 : from; start <= len; start++) {
        R->ncont = 0;
        R->steps = 0;
        for (int i = 0; i < RX_CAPS; i++) {
            R->cap_start[i] = -1;
            R->cap_end[i] = -1;
        }
        R->end = -1;
        if (rx_run(R, 0, start, -1)) {
            /* Nought is the whole of it, whatever the group bookkeeping
               made of it along the way. */
            R->cap_start[0] = start;
            R->cap_end[0] = R->end;
            return start;
        }
    }
    return -1;
}
