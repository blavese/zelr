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
 * escapes inside them, `[]` and `[^]` included; the escapes `\d \D \w \W \s
 * \S \b \B` and the usual `\n \t \r \f \v \0` and `\xNN`, `\uNNNN` and
 * `\u{N}`; the anchors `^` and `$`; groups, capturing, named `(?<name>` and
 * `(?:` not; lookahead `(?=` `(?!` and lookbehind `(?<=` `(?<!`;
 * backreferences `\1` and `\k<name>`; alternation; and the quantifiers `*`
 * `+` `?` `{n}` `{n,}` `{n,m}`, each of them greedy or, with a `?` after,
 * lazy. The flags `g`, `i`, `m`, `s` (a dot matches a newline) and `y`
 * (only where lastIndex says); `u` and `d` are accepted.
 *
 * The text is UTF-8, and a dot, a class or a character in the pattern takes
 * a whole character of it, whatever its length; a byte that is not part of a
 * whole character (the strings atob makes) counts as the character with its
 * value. Classes hold code points, not bytes.
 *
 * --- what is not -----------------------------------------------------------
 *
 * A character past U+FFFF is one character, as it is with the u flag; without
 * it the language would see two halves, and a pattern that matches the halves
 * one at a time does not match it here. A case is folded only in ASCII and
 * Latin-1. Unicode property escapes, \p{...}, are known for the classes pages
 * use (letters, cases, numbers, punctuation, spaces, symbols, emoji) as ranges
 * holding the common scripts rather than the whole of the Unicode tables;
 * anything else is refused when the pattern is compiled, and the script is
 * told.
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
 * A single character repeated -- `.*`, `\d+`, `[^"]*` -- is counted in a
 * loop and then given back one at a time, rather than a continuation for
 * each turn: those were what ran out of continuations at about five hundred
 * turns, and a line longer than that did not match `.*`.
 *
 * A repetition whose body can match nothing -- `(a?)*` -- would otherwise
 * repeat for ever. Each repetition frame remembers where it started, and a
 * turn that consumed nothing and has already met the minimum stops rather
 * than going round again.
 */
#pragma once
#include "zelr.h"

#define RX_NODES   4096          /* a list of hundreds of words, each an alternative */
#define RX_CLASSES 96
#define RX_RANGES  512           /* class ranges past U+00FF, all classes together */
#define RX_CONTS   1024
/* Groups, the whole match included. Fifty was not enough for patterns pages
   build out of many alternatives each in its group -- The Verge's stopped a
   script with "more groups than a pattern has room for" -- and a group costs
   its name here and eight bytes a lookaround while matching. Only the groups
   a pattern has are cleared (ngroups), so the room costs nothing per use. */
#define RX_CAPS    256
#define RX_NAME    32
#define RX_STEPS   400000        /* a pattern that will not finish, stopped */
#define RX_SEARCH  4000000       /* and across all the places it is tried */

enum {
    RXN_CHAR = 1, RXN_ANY, RXN_CLASS, RXN_GROUP,
    RXN_BOL, RXN_EOL, RXN_WORDB, RXN_NWORDB, RXN_LOOK, RXN_BACKREF
};

/* A continuation: what to do once the thing in hand has matched.
 *
 * `rep` says which sort. Nought or more is a repetition to resume, counting
 * what it has already done; -1 is an ordinary "carry on with this node";
 * -2 closes a capture, and then carries on; -3 is the end of a lookaround,
 * which has matched once it gets there (at `at`, when it has to end there). */
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
    int   ch;                    /* RXN_CHAR; RXN_LOOK: 1 negative, 2 behind;
                                    RXN_BACKREF: the group */
    int   cls;                   /* RXN_CLASS: which class */
    int   alt;                   /* RXN_GROUP, RXN_LOOK: first alternative */
    int   cap;                   /* capture number, or -1 */
    int   next;                  /* next term in this sequence */
    int   alt_next;              /* next alternative, on an alternative head */
    int   wmin, wmax;            /* RXN_LOOK behind: how long it can be */
} rxnode;

typedef struct {
    rxnode nodes[RX_NODES];
    int    nnodes;
    u8     classes[RX_CLASSES][32];  /* code points 0 to 255, a bit each */
    u8     cneg[RX_CLASSES];         /* the class is turned inside out */
    u32    rlo[RX_RANGES], rhi[RX_RANGES];   /* and past 255, ranges, */
    u8     rcls[RX_RANGES];                  /* each with its class */
    int    nranges;
    int    nclasses;
    int    ncaps;
    int    ngroups;                  /* how many the pattern has, counted first */
    char   names[RX_CAPS][RX_NAME];  /* a named group's name, or empty */
    int    named;                    /* whether any group has one */

    int    icase, multiline, global, dotall, sticky, unicode;

    /* set while matching */
    const char *s;
    int    len;
    rxcont cont[RX_CONTS];
    int    ncont;
    int    cap_start[RX_CAPS], cap_end[RX_CAPS];
    int    end;
    u32    steps, total;

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
    x->wmin = x->wmax = 0;
    return n;
}

static void rx_fail(rx *R, const char *why) {
    if (!R->ok) return;
    R->ok = 0;
    int i = 0;
    while (why[i] && i < (int)sizeof(R->why) - 1) { R->why[i] = why[i]; i++; }
    R->why[i] = 0;
}

/* Code points into a class: up to 255 as bits, past it as a range. They
   were bits only, and \uD800 was taken for its low byte, so [\uD800-\uDBFF]
   became every byte there is -- which is how core-js's JSON.stringify came
   to escape the brackets of every array it wrote. */
static void rx_cls_add(rx *R, int c, u32 lo, u32 hi) {
    if (c < 0 || c >= RX_CLASSES || hi < lo) return;
    for (u32 i = lo; i <= hi && i < 256; i++)
        R->classes[c][i >> 3] |= (u8)(1 << (i & 7));
    if (hi < 256) return;
    if (lo < 256) lo = 256;
    if (R->nranges >= RX_RANGES) { rx_fail(R, "too many ranges in its classes"); return; }
    R->rlo[R->nranges] = lo;
    R->rhi[R->nranges] = hi;
    R->rcls[R->nranges] = (u8)c;
    R->nranges++;
}

static int rx_cls_has(const rx *R, int c, u32 cp) {
    int in = 0;
    if (cp < 256) in = (R->classes[c][cp >> 3] >> (cp & 7)) & 1;
    else
        for (int i = 0; i < R->nranges && !in; i++)
            if (R->rcls[i] == c && cp >= R->rlo[i] && cp <= R->rhi[i]) in = 1;
    return in != R->cneg[c];
}

/* One character of UTF-8 at p, n bytes there: its code point, and its
   length in *k. A byte that does not begin a whole character is taken as
   itself, one byte long -- the strings atob makes are bytes like that. The
   half of a surrogate pair on its own, which this engine keeps as the three
   bytes it would be, is read as that half. */
static u32 rx_utf8(const char *p, int n, int *k) {
    u8 c = (u8)p[0];
    *k = 1;
    if (c < 0x80) return c;
    int need = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xC2 && c <= 0xDF ? 1 : 0;
    if (!need || need >= n) return c;
    u32 v = c & (need == 1 ? 0x1Fu : need == 2 ? 0x0Fu : 0x07u);
    for (int i = 1; i <= need; i++) {
        u8 d = (u8)p[i];
        if ((d & 0xC0) != 0x80) return c;
        v = (v << 6) | (d & 0x3Fu);
    }
    if ((need == 2 && v < 0x800) || (need == 3 && (v < 0x10000 || v > 0x10FFFF))) return c;
    *k = need + 1;
    return v;
}

/* A case folded for matching: ASCII and Latin-1. */
static u32 rx_fold(u32 c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    return c;
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

/* One escape, as the code point it stands for. 😀, the two halves
   of a pair written one after the other, is the one character they make,
   which is the only way this engine's text can hold it. */
static int rx_escape_char(const char *p, int len, int *at) {
    int c = (*at) < len ? p[(*at)++] : 0;
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case 'f': return '\f';
        case 'v': return '\v';
        case '0': return 0;
        case 'c': {
            /* \cJ: a control character, by its letter. */
            if (*at < len && ((p[*at] | 32) >= 'a' && (p[*at] | 32) <= 'z')) return p[(*at)++] & 31;
            return '\\';
        }
        case 'x': {
            int h = 0, k = 0;
            for (; k < 2 && *at < len; k++) {
                int v = rx_hexval(p[*at]);
                if (v < 0) break;
                h = h * 16 + v;
                (*at)++;
            }
            return k ? h : 'x';
        }
        case 'u': {
            int h = 0;
            if (*at < len && p[*at] == '{') {
                int k = *at + 1;
                while (k < len && rx_hexval(p[k]) >= 0 && h <= 0x10FFFF) h = h * 16 + rx_hexval(p[k++]);
                if (k < len && p[k] == '}' && h <= 0x10FFFF) { *at = k + 1; return h; }
                return 'u';
            }
            int k = 0;
            for (; k < 4 && *at < len; k++) {
                int v = rx_hexval(p[*at]);
                if (v < 0) break;
                h = h * 16 + v;
                (*at)++;
            }
            if (!k) return 'u';
            if (k == 4 && h >= 0xD800 && h < 0xDC00 && *at + 6 <= len && p[*at] == '\\' && p[*at + 1] == 'u') {
                int lo = 0, ok = 1;
                for (int i = 2; i < 6 && ok; i++) {
                    int v = rx_hexval(p[*at + i]);
                    if (v < 0) ok = 0; else lo = lo * 16 + v;
                }
                if (ok && lo >= 0xDC00 && lo < 0xE000) {
                    *at += 6;
                    return 0x10000 + ((h - 0xD800) << 10) + (lo - 0xDC00);
                }
            }
            return h;
        }
        default: return c;
    }
}

static int rx_is_class_escape(int c) {
    return c == 'd' || c == 'D' || c == 'w' || c == 'W'
        || c == 's' || c == 'S';
}

/* The spaces \s means past U+00FF, as the language lists them. */
static const u32 RX_WIDE_SPACES[][2] = {
    { 0x1680, 0x1680 }, { 0x2000, 0x200A }, { 0x2028, 0x2029 }, { 0x202F, 0x202F },
    { 0x205F, 0x205F }, { 0x3000, 0x3000 }, { 0xFEFF, 0xFEFF }
};
#define RX_NWIDE ((int)(sizeof(RX_WIDE_SPACES) / sizeof(RX_WIDE_SPACES[0])))

static void rx_fill_class_escape(rx *R, int c, int which) {
    int neg = (which == 'D' || which == 'W' || which == 'S');
    int kind = rx_lower(which);
    for (u32 i = 0; i < 256; i++) {
        int in = kind == 'd' ? rx_digit((int)i)
               : kind == 'w' ? rx_word((int)i)
               : rx_space((int)i) || i == 0xA0;
        if (in != neg) rx_cls_add(R, c, i, i);
    }
    /* Past U+00FF: digits and word characters are ASCII only, so \D and \W
       take in everything there; \s has its listed spaces and \S the rest. */
    if (kind != 's') {
        if (neg) rx_cls_add(R, c, 0x100, 0x10FFFF);
        return;
    }
    if (!neg) {
        for (int i = 0; i < RX_NWIDE; i++) rx_cls_add(R, c, RX_WIDE_SPACES[i][0], RX_WIDE_SPACES[i][1]);
        return;
    }
    u32 from = 0x100;
    for (int i = 0; i < RX_NWIDE; i++) {
        if (RX_WIDE_SPACES[i][0] > from) rx_cls_add(R, c, from, RX_WIDE_SPACES[i][0] - 1);
        from = RX_WIDE_SPACES[i][1] + 1;
    }
    rx_cls_add(R, c, from, 0x10FFFF);
}

/* \p{Name} and \P{Name}: the Unicode classes pages use, as ranges that hold
   the scripts a page is likely to be written in -- Latin, Greek, Cyrillic,
   Armenian, Hebrew, Arabic, Devanagari, Thai, Georgian, Hangul, the kana and
   the CJK ideographs -- rather than the whole of the Unicode tables, which
   would be most of this file. Each list is in order and does not overlap. */
static const u32 RXP_L[][2] = {
    { 'A', 'Z' }, { 'a', 'z' }, { 0xAA, 0xAA }, { 0xB5, 0xB5 }, { 0xBA, 0xBA }, { 0xC0, 0xD6 },
    { 0xD8, 0xF6 }, { 0xF8, 0x2C1 }, { 0x2C6, 0x2D1 }, { 0x2E0, 0x2E4 }, { 0x370, 0x373 },
    { 0x376, 0x377 }, { 0x37B, 0x37D }, { 0x386, 0x386 }, { 0x388, 0x3F5 }, { 0x3F7, 0x481 },
    { 0x48A, 0x52F }, { 0x531, 0x556 }, { 0x561, 0x587 }, { 0x5D0, 0x5EA }, { 0x620, 0x64A },
    { 0x671, 0x6D3 }, { 0x904, 0x939 }, { 0xE01, 0xE30 }, { 0x10A0, 0x10FF }, { 0x1100, 0x11FF },
    { 0x1E00, 0x1FFF }, { 0x3041, 0x3096 }, { 0x30A1, 0x30FA }, { 0x3105, 0x312F }, { 0x3131, 0x318E },
    { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF }, { 0xA000, 0xA48C }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF },
    { 0xFB00, 0xFB06 }, { 0xFF21, 0xFF3A }, { 0xFF41, 0xFF5A }, { 0xFF66, 0xFFDC }, { 0x10000, 0x1EFFF },
    { 0x20000, 0x3134F }
};
static const u32 RXP_LU[][2] = {
    { 'A', 'Z' }, { 0xC0, 0xD6 }, { 0xD8, 0xDE }, { 0x391, 0x3A1 }, { 0x3A3, 0x3A9 }, { 0x400, 0x42F },
    { 0x531, 0x556 }, { 0xFF21, 0xFF3A }
};
static const u32 RXP_LL[][2] = {
    { 'a', 'z' }, { 0xB5, 0xB5 }, { 0xDF, 0xF6 }, { 0xF8, 0xFF }, { 0x3AC, 0x3CE }, { 0x430, 0x45F },
    { 0x561, 0x587 }, { 0xFF41, 0xFF5A }
};
static const u32 RXP_N[][2] = {
    { '0', '9' }, { 0xB2, 0xB3 }, { 0xB9, 0xB9 }, { 0xBC, 0xBE }, { 0x660, 0x669 }, { 0x6F0, 0x6F9 },
    { 0x966, 0x96F }, { 0x2070, 0x2070 }, { 0x2074, 0x2079 }, { 0x2080, 0x2089 }, { 0x2150, 0x2189 },
    { 0x2460, 0x249B }, { 0x3007, 0x3007 }, { 0x3021, 0x3029 }, { 0xFF10, 0xFF19 }
};
static const u32 RXP_ND[][2] = {
    { '0', '9' }, { 0x660, 0x669 }, { 0x6F0, 0x6F9 }, { 0x966, 0x96F }, { 0xFF10, 0xFF19 }
};
static const u32 RXP_P[][2] = {
    { '!', '#' }, { '%', '*' }, { ',', '/' }, { ':', ';' }, { '?', '@' }, { '[', ']' }, { '_', '_' },
    { '{', '{' }, { '}', '}' }, { 0xA1, 0xA1 }, { 0xA7, 0xA7 }, { 0xAB, 0xAB }, { 0xB6, 0xB7 },
    { 0xBB, 0xBB }, { 0xBF, 0xBF }, { 0x2010, 0x2027 }, { 0x2030, 0x2043 }, { 0x2045, 0x2051 },
    { 0x2053, 0x205E }, { 0x3001, 0x3003 }, { 0x3008, 0x3011 }, { 0x3014, 0x301F }, { 0xFE10, 0xFE19 },
    { 0xFE30, 0xFE4F }, { 0xFF01, 0xFF03 }, { 0xFF05, 0xFF0A }, { 0xFF0C, 0xFF0F }
};
static const u32 RXP_Z[][2] = {
    { '\t', '\r' }, { ' ', ' ' }, { 0x85, 0x85 }, { 0xA0, 0xA0 }, { 0x1680, 0x1680 }, { 0x2000, 0x200A },
    { 0x2028, 0x2029 }, { 0x202F, 0x202F }, { 0x205F, 0x205F }, { 0x3000, 0x3000 }
};
static const u32 RXP_S[][2] = {
    { '$', '$' }, { '+', '+' }, { '<', '>' }, { '^', '^' }, { '`', '`' }, { '|', '|' }, { '~', '~' },
    { 0xA2, 0xA6 }, { 0xA8, 0xA9 }, { 0xAC, 0xAC }, { 0xAE, 0xB1 }, { 0xB4, 0xB4 }, { 0xB8, 0xB8 },
    { 0xD7, 0xD7 }, { 0xF7, 0xF7 }, { 0x2044, 0x2044 }, { 0x20A0, 0x20C0 }, { 0x2100, 0x214F },
    { 0x2190, 0x23FF }, { 0x2500, 0x27BF }, { 0x2900, 0x2BFF }, { 0x1F000, 0x1FAFF }
};
static const u32 RXP_EMOJI[][2] = {
    { 0xA9, 0xA9 }, { 0xAE, 0xAE }, { 0x203C, 0x203C }, { 0x2049, 0x2049 }, { 0x2122, 0x2122 },
    { 0x2139, 0x2139 }, { 0x2194, 0x21AA }, { 0x231A, 0x23FF }, { 0x24C2, 0x24C2 }, { 0x25AA, 0x27BF },
    { 0x2934, 0x2935 }, { 0x2B05, 0x2B55 }, { 0x3030, 0x3030 }, { 0x303D, 0x303D }, { 0x3297, 0x3297 },
    { 0x3299, 0x3299 }, { 0x1F000, 0x1FAFF }
};

/* A list into a class, or everything outside it. */
static void rx_add_list(rx *R, int c, const u32 (*t)[2], int n, int neg) {
    if (!neg) {
        for (int i = 0; i < n; i++) rx_cls_add(R, c, t[i][0], t[i][1]);
        return;
    }
    u32 from = 0;
    for (int i = 0; i < n; i++) {
        if (t[i][0] > from) rx_cls_add(R, c, from, t[i][0] - 1);
        from = t[i][1] + 1;
    }
    if (from <= 0x10FFFF) rx_cls_add(R, c, from, 0x10FFFF);
}

/* 0 when the name is not one of those. */
static int rx_fill_property(rx *R, int c, const char *nm, int n, int neg) {
    #define IS(w) (n == (int)sizeof(w) - 1 && !strncmp(nm, w, n))
    #define LIST(t) rx_add_list(R, c, t, (int)(sizeof(t) / sizeof(t[0])), neg)
    if (IS("L") || IS("Letter") || IS("Alphabetic") || IS("Alpha")) LIST(RXP_L);
    else if (IS("Lu") || IS("Uppercase_Letter") || IS("Uppercase")) LIST(RXP_LU);
    else if (IS("Ll") || IS("Lowercase_Letter") || IS("Lowercase")) LIST(RXP_LL);
    else if (IS("N") || IS("Number")) LIST(RXP_N);
    else if (IS("Nd") || IS("Decimal_Number") || IS("digit")) LIST(RXP_ND);
    else if (IS("P") || IS("Punctuation")) LIST(RXP_P);
    else if (IS("Z") || IS("Zs") || IS("White_Space") || IS("Space_Separator")) LIST(RXP_Z);
    else if (IS("S") || IS("Symbol")) LIST(RXP_S);
    else if (IS("Emoji") || IS("Extended_Pictographic") || IS("Emoji_Presentation")) LIST(RXP_EMOJI);
    else return 0;
    #undef LIST
    #undef IS
    return 1;
}

static int rx_new_class(rx *R) {
    if (R->nclasses >= RX_CLASSES) { rx_fail(R, "too many classes"); return -1; }
    int c = R->nclasses++;
    for (int i = 0; i < 32; i++) R->classes[c][i] = 0;
    R->cneg[c] = 0;
    return c;
}

/* A \p{...} at p + *at, the \p already passed. */
static int rx_property(rx *R, int c, const char *p, int len, int *at, int neg) {
    if (*at >= len || p[*at] != '{') { rx_fail(R, "\\p needs {a name}"); return 0; }
    int k = *at + 1, start = k;
    while (k < len && p[k] != '}') k++;
    if (k >= len) { rx_fail(R, "\\p{ with no }"); return 0; }
    const char *nm = p + start;
    int n = k - start;
    /* General_Category=L and gc=L, and Script= which is answered as letters. */
    for (int i = 0; i < n; i++) if (nm[i] == '=') {
        if (i >= 6 && !strncmp(nm, "Script", 6)) { nm = "L"; n = 1; break; }
        nm += i + 1;
        n -= i + 1;
        break;
    }
    *at = k + 1;
    if (!rx_fill_property(R, c, nm, n, neg)) { rx_fail(R, "a \\p{...} class this does not know"); return 0; }
    return 1;
}

/* [abc], [^a-z], [\d\-], and [] and [^], which match nothing and anything. */
static int rx_parse_class(rx *R, const char *p, int len, int *at) {
    int c = rx_new_class(R);
    if (c < 0) return -1;

    int neg = 0;
    if (*at < len && p[*at] == '^') { neg = 1; (*at)++; }

    while (*at < len && p[*at] != ']') {
        int lo, k;
        if (p[*at] == '\\') {
            (*at)++;
            int e = *at < len ? p[*at] : 0;
            if (rx_is_class_escape(e)) {
                (*at)++;
                rx_fill_class_escape(R, c, e);
                continue;
            }
            if ((e == 'p' || e == 'P') && R->unicode) {
                (*at)++;
                if (!rx_property(R, c, p, len, at, e == 'P')) return -1;
                continue;
            }
            if (e == 'b') { (*at)++; lo = '\b'; }
            else if (e == '-') { (*at)++; lo = '-'; }
            else lo = rx_escape_char(p, len, at);
        } else {
            lo = (int)rx_utf8(p + *at, len - *at, &k);
            *at += k;
        }

        int hi = lo;
        if (*at + 1 < len && p[*at] == '-' && p[*at + 1] != ']') {
            (*at)++;
            if (p[*at] == '\\') {
                (*at)++;
                int e = *at < len ? p[*at] : 0;
                if (rx_is_class_escape(e)) {
                    /* [a-\d]: the dash is itself, then the class. */
                    (*at)++;
                    rx_cls_add(R, c, lo, lo);
                    rx_cls_add(R, c, '-', '-');
                    rx_fill_class_escape(R, c, e);
                    continue;
                }
                hi = rx_escape_char(p, len, at);
            } else {
                hi = (int)rx_utf8(p + *at, len - *at, &k);
                *at += k;
            }
        }
        if (hi < lo) { rx_fail(R, "a range that runs backwards"); return -1; }
        rx_cls_add(R, c, (u32)lo, (u32)hi);
    }
    if (*at >= len || p[*at] != ']') { rx_fail(R, "a class with no ]"); return -1; }
    (*at)++;

    /* Folding is done once, here, rather than on every character tested:
       ASCII and Latin-1, each small letter with its capital. */
    if (R->icase) {
        for (int i = 'a'; i <= 0xFE; i++) {
            if (i > 'z' && i < 0xE0) continue;
            if (i == 0xF7) continue;
            int lower = (R->classes[c][i >> 3] >> (i & 7)) & 1;
            int upper = (R->classes[c][(i - 32) >> 3] >> ((i - 32) & 7)) & 1;
            if (lower || upper) {
                rx_cls_add(R, c, (u32)i, (u32)i);
                rx_cls_add(R, c, (u32)(i - 32), (u32)(i - 32));
            }
        }
    }
    /* A negated class matches a newline, as it does everywhere else; it was
       kept from crossing one, so [^"]* stopped at the end of a line. */
    R->cneg[c] = (u8)neg;
    return c;
}

static int rx_parse_alt(rx *R, const char *p, int len, int *at, int depth);

/* The group a name stands for, or -1. */
static int rx_name_index(rx *R, const char *nm, int n) {
    for (int g = 1; g < R->ngroups && g < RX_CAPS; g++) {
        if (!R->names[g][0]) continue;
        int k = 0;
        while (k < n && k < RX_NAME - 1 && R->names[g][k] == nm[k]) k++;
        if (k == n && !R->names[g][k]) return g;
    }
    return -1;
}

/* Every group's number and name, before the pattern is read, so that \k<a>
   and \2 can name a group that comes later in the text. */
static int rx_prescan(rx *R, const char *p, int len) {
    int n = 1, in_class = 0;
    R->names[0][0] = 0;
    for (int i = 0; i < len; i++) {
        char c = p[i];
        if (c == '\\') { i++; continue; }
        if (in_class) { if (c == ']') in_class = 0; continue; }
        if (c == '[') { in_class = 1; continue; }
        if (c != '(') continue;
        if (i + 1 < len && p[i + 1] == '?') {
            if (!(i + 2 < len && p[i + 2] == '<' && i + 3 < len && p[i + 3] != '=' && p[i + 3] != '!'))
                continue;
        } else if (n < RX_CAPS) {
            R->names[n][0] = 0;                 /* a group with no name */
            n++;
            continue;
        }
        {
            int k = i + 3, w = 0;
            if (n < RX_CAPS) {
                while (k < len && p[k] != '>' && w < RX_NAME - 1) R->names[n][w++] = p[k++];
                R->names[n][w] = 0;
                R->named = 1;
            }
        }
        n++;
    }
    return n;
}

/* The shortest and longest a node's sequence can match, for a lookbehind,
   which is tried at each place it could start: -1 for no end. */
static void rx_width(rx *R, int n, int *mn, int *mx);

static void rx_seq_width(rx *R, int n, int *mn, int *mx) {
    int a = 0, b = 0;
    for (; n >= 0; n = R->nodes[n].next) {
        int x, y;
        rx_width(R, n, &x, &y);
        a += x;
        if (b >= 0) b = y < 0 ? -1 : b + y;
    }
    *mn = a;
    *mx = b;
}

static void rx_width(rx *R, int n, int *mn, int *mx) {
    rxnode *x = &R->nodes[n];
    int one_min = 0, one_max = 0;
    switch (x->kind) {
        case RXN_CHAR: case RXN_ANY: case RXN_CLASS: one_min = one_max = 1; break;
        case RXN_GROUP: {
            int lo = -1, hi = 0;
            for (int a = x->alt; a >= 0; a = R->nodes[a].alt_next) {
                int p, q;
                rx_seq_width(R, a, &p, &q);
                if (lo < 0 || p < lo) lo = p;
                if (hi >= 0) hi = q < 0 ? -1 : (q > hi ? q : hi);
            }
            one_min = lo < 0 ? 0 : lo;
            one_max = hi;
            break;
        }
        case RXN_BACKREF: one_min = 0; one_max = -1; break;
        default: one_min = one_max = 0; break;
    }
    *mn = one_min * x->min;
    if (one_max < 0 || x->max < 0) *mx = one_max == 0 ? 0 : -1;
    else *mx = one_max * x->max;
}

/* One atom with whatever quantifier follows it. */
static int rx_parse_term(rx *R, const char *p, int len, int *at, int depth) {
    if (*at >= len) return -1;
    int c = p[*at];
    int n = -1;

    if (c == '(') {
        (*at)++;
        int cap = -1, look = -1;
        if (*at + 1 < len && p[*at] == '?' && p[*at + 1] == ':') {
            *at += 2;
        } else if (*at + 1 < len && p[*at] == '?' && (p[*at + 1] == '=' || p[*at + 1] == '!')) {
            look = p[*at + 1] == '!' ? 1 : 0;
            *at += 2;
        } else if (*at + 2 < len && p[*at] == '?' && p[*at + 1] == '<'
                   && (p[*at + 2] == '=' || p[*at + 2] == '!')) {
            look = 2 | (p[*at + 2] == '!' ? 1 : 0);
            *at += 3;
        } else if (*at + 1 < len && p[*at] == '?' && p[*at + 1] == '<') {
            /* (?<name>...): numbered as any other group, named as well. */
            int k = *at + 2;
            while (k < len && p[k] != '>') k++;
            if (k >= len) { rx_fail(R, "a group name with no >"); return -1; }
            *at = k + 1;
            if (R->ncaps >= RX_CAPS) { rx_fail(R, "more groups than a pattern has room for"); return -1; }
            cap = R->ncaps++;
        } else if (*at < len && p[*at] == '?') {
            rx_fail(R, "a (? this does not know");
            return -1;
        } else {
            if (R->ncaps >= RX_CAPS) { rx_fail(R, "more groups than a pattern has room for"); return -1; }
            cap = R->ncaps++;
        }
        n = rx_new(R, look >= 0 ? RXN_LOOK : RXN_GROUP);
        if (n < 0) { rx_fail(R, "pattern too big"); return -1; }
        R->nodes[n].cap = cap;
        if (look >= 0) R->nodes[n].ch = look;
        int alt = rx_parse_alt(R, p, len, at, depth + 1);
        R->nodes[n].alt = alt;
        if (*at >= len || p[*at] != ')') { rx_fail(R, "a group with no )"); return -1; }
        (*at)++;
        if (look >= 2) {
            int lo = -1, hi = 0;
            for (int a = alt; a >= 0; a = R->nodes[a].alt_next) {
                int q1, q2;
                rx_seq_width(R, a, &q1, &q2);
                if (lo < 0 || q1 < lo) lo = q1;
                if (hi >= 0) hi = q2 < 0 ? -1 : (q2 > hi ? q2 : hi);
            }
            R->nodes[n].wmin = lo < 0 ? 0 : lo;
            R->nodes[n].wmax = hi;
        }
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
        if (rx_is_class_escape(e) || ((e == 'p' || e == 'P') && R->unicode)) {
            (*at)++;
            int cls = rx_new_class(R);
            if (cls < 0) return -1;
            if (e == 'p' || e == 'P') {
                if (!rx_property(R, cls, p, len, at, e == 'P')) return -1;
            } else rx_fill_class_escape(R, cls, e);
            n = rx_new(R, RXN_CLASS);
            if (n < 0) { rx_fail(R, "pattern too big"); return -1; }
            R->nodes[n].cls = cls;
        } else if (e == 'b' || e == 'B') {
            (*at)++;
            n = rx_new(R, e == 'b' ? RXN_WORDB : RXN_NWORDB);
        } else if (e >= '1' && e <= '9') {
            /* \1, or \12 when there are that many groups; otherwise, as the
               old rule has it, an octal character. */
            int g = 0, k = *at;
            while (k < len && rx_digit(p[k]) && g * 10 + (p[k] - '0') < RX_CAPS) g = g * 10 + (p[k++] - '0');
            *at = k;
            n = rx_new(R, RXN_BACKREF);
            if (n < 0) { rx_fail(R, "pattern too big"); return -1; }
            R->nodes[n].ch = g;
        } else if (e == 'k' && R->named && *at + 1 < len && p[*at + 1] == '<') {
            int k = *at + 2, start = k;
            while (k < len && p[k] != '>') k++;
            int g = rx_name_index(R, p + start, k - start);
            if (g < 0) { rx_fail(R, "\\k names a group there is not"); return -1; }
            *at = k + 1;
            n = rx_new(R, RXN_BACKREF);
            if (n < 0) { rx_fail(R, "pattern too big"); return -1; }
            R->nodes[n].ch = g;
        } else {
            int ch = rx_escape_char(p, len, at);
            n = rx_new(R, RXN_CHAR);
            if (n < 0) { rx_fail(R, "pattern too big"); return -1; }
            R->nodes[n].ch = R->icase ? (int)rx_fold((u32)ch) : ch;
        }
    } else if (c == ')' || c == '|') {
        return -1;                          /* the caller's business */
    } else if (c == '*' || c == '+' || c == '?') {
        rx_fail(R, "a quantifier with nothing before it");
        return -1;
    } else {
        /* A character, all of it: é in a pattern is one thing to match. */
        int k;
        u32 cp = rx_utf8(p + *at, len - *at, &k);
        *at += k;
        n = rx_new(R, RXN_CHAR);
        if (n < 0) { rx_fail(R, "pattern too big"); return -1; }
        R->nodes[n].ch = (int)(R->icase ? rx_fold(cp) : cp);
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

    /* Anchors, boundaries and lookarounds take no quantifier that means
       anything, and a page that writes one is not asking for what it says. */
    if (R->nodes[n].kind == RXN_BOL || R->nodes[n].kind == RXN_EOL
        || R->nodes[n].kind == RXN_WORDB || R->nodes[n].kind == RXN_NWORDB
        || R->nodes[n].kind == RXN_LOOK) {
        R->nodes[n].min = R->nodes[n].max = 1;
    }
    return n;
}

/* Terms until a | or a ) -- one branch. */
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

/* Branches separated by | -- the contents of a group. */
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
    /* Only what is read before matching needs clearing; the matching state
       is set by rx_search. */
    volatile char *z = (volatile char *)R->classes;
    for (u32 i = 0; i < sizeof(R->classes); i++) z[i] = 0;
    R->nnodes = R->nclasses = R->named = R->nranges = 0;
    R->icase = R->multiline = R->global = R->dotall = R->sticky = R->unicode = 0;
    R->ok = 1;
    R->why[0] = 0;
    R->ncaps = 1;                           /* nought is the whole match */

    for (const char *f = flags ? flags : ""; *f; f++) {
        if (*f == 'i') R->icase = 1;
        else if (*f == 'm') R->multiline = 1;
        else if (*f == 'g') R->global = 1;
        else if (*f == 's') R->dotall = 1;
        else if (*f == 'y') R->sticky = 1;
        else if (*f == 'u' || *f == 'v') R->unicode = 1;
        else if (*f == 'd') { /* indices: accepted, not given */ }
        else { rx_fail(R, "a flag that is not here"); return 0; }
    }
    R->ngroups = 0;
    int groups = rx_prescan(R, pat, len);
    if (groups > RX_CAPS) {
        rx_fail(R, "more groups than a pattern has room for");
        return 0;
    }
    R->ngroups = groups;

    int at = 0;
    int root = rx_new(R, RXN_GROUP);
    if (root < 0) { rx_fail(R, "pattern too big"); return 0; }
    R->nodes[root].cap = 0;
    R->nodes[root].alt = rx_parse_alt(R, pat, len, &at, 0);
    if (at < len) rx_fail(R, at < len && pat[at] == ')' ? "a ) with no ( before it" : "something left over");
    return R->ok;
}

/* --- matching ------------------------------------------------------------- */

static int rx_run(rx *R, int n, int pos, int cont);
static int rx_rep(rx *R, int n, int pos, int cont, int done);

static int rx_at_word(rx *R, int i) {
    return i >= 0 && i < R->len && rx_word((unsigned char)R->s[i]);
}

static int rx_budget(rx *R) {
    return ++R->steps <= RX_STEPS && ++R->total <= RX_SEARCH;
}

static int rx_cont_do(rx *R, int cont, int pos) {
    if (cont < 0) { R->end = pos; return 1; }

    rxcont f = R->cont[cont];
    if (f.rep == -3) return f.at < 0 || pos == f.at;
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

/* How many bytes one character atom takes at pos -- the whole of a UTF-8
   character -- or 0 when it does not match there. */
static int rx_one(rx *R, rxnode *x, int pos) {
    if (pos >= R->len) return 0;
    int k = 1;
    u32 c = (u8)R->s[pos];
    if (c >= 0x80) c = rx_utf8(R->s + pos, R->len - pos, &k);
    switch (x->kind) {
        case RXN_CHAR: return (R->icase ? rx_fold(c) : c) == (u32)x->ch ? k : 0;
        case RXN_ANY: return R->dotall || (c != '\n' && c != '\r' && c != 0x2028 && c != 0x2029) ? k : 0;
        default: return rx_cls_has(R, x->cls, c) ? k : 0;
    }
}

/* The start of the character that ends at q, no further back than lo: the
   longest whole one there is, as reading forward would have found it. */
static int rx_back(rx *R, int lo, int q) {
    for (int b = 4; b >= 2; b--) {
        if (q - b < lo) continue;
        int k;
        rx_utf8(R->s + q - b, R->len - (q - b), &k);
        if (k == b) return q - b;
    }
    return q - 1;
}

/* The atom once, and then whatever comes after it. */
static int rx_atom(rx *R, int n, int pos, int cont) {
    if (!rx_budget(R)) return 0;

    rxnode *x = &R->nodes[n];
    switch (x->kind) {
        case RXN_CHAR: case RXN_ANY: case RXN_CLASS: {
            int k = rx_one(R, x, pos);
            if (!k) return 0;
            return rx_cont_do(R, cont, pos + k);
        }
        case RXN_BOL:
            if (pos == 0) return rx_cont_do(R, cont, pos);
            if (R->multiline && (R->s[pos - 1] == '\n' || R->s[pos - 1] == '\r'))
                return rx_cont_do(R, cont, pos);
            return 0;
        case RXN_EOL:
            if (pos == R->len) return rx_cont_do(R, cont, pos);
            if (R->multiline && (R->s[pos] == '\n' || R->s[pos] == '\r'))
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
        case RXN_BACKREF: {
            /* What the group caught, again; nothing at all when the group
               took no part. */
            int g = x->ch;
            int in = (g < R->ngroups || g < R->ncaps) && g < RX_CAPS;
            int s = in ? R->cap_start[g] : -1, e = in ? R->cap_end[g] : -1;
            if (s < 0 || e < s) return rx_cont_do(R, cont, pos);
            int L = e - s;
            if (pos + L > R->len) return 0;
            for (int i = 0; i < L; i++) {
                int a = (unsigned char)R->s[s + i], b = (unsigned char)R->s[pos + i];
                if (R->icase ? rx_lower(a) != rx_lower(b) : a != b) return 0;
            }
            return rx_cont_do(R, cont, pos + L);
        }
        case RXN_LOOK: {
            /* Matched where it stands without moving, and not backtracked
               into once it has: ahead from here, or behind, ending here. */
            int neg = x->ch & 1, behind = x->ch & 2;
            int save = R->ncont;
            int cs[RX_CAPS], ce[RX_CAPS];
            if (neg) for (int i = 0; i < R->ncaps; i++) { cs[i] = R->cap_start[i]; ce[i] = R->cap_end[i]; }
            int f = rx_push(R, -1, -1, -3, behind ? pos : -1);
            if (f < 0) return 0;
            int matched = 0;
            if (!behind) {
                for (int a = x->alt; a >= 0 && !matched; a = R->nodes[a].alt_next) {
                    R->ncont = f + 1;
                    if (rx_run(R, a, pos, f)) matched = 1;
                }
            } else {
                /* The widths are in characters, and a character is one to
                   four bytes. */
                int from = pos - x->wmin;
                int to = x->wmax < 0 ? 0 : pos - 4 * x->wmax;
                if (to < 0) to = 0;
                for (int k = from; k >= to && !matched; k--)
                    for (int a = x->alt; a >= 0 && !matched; a = R->nodes[a].alt_next) {
                        R->ncont = f + 1;
                        if (rx_run(R, a, k, f)) matched = 1;
                    }
            }
            R->ncont = save;
            if (neg) {
                for (int i = 0; i < R->ncaps; i++) { R->cap_start[i] = cs[i]; R->cap_end[i] = ce[i]; }
                return matched ? 0 : rx_cont_do(R, cont, pos);
            }
            return matched ? rx_cont_do(R, cont, pos) : 0;
        }
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
    if (!rx_budget(R)) return 0;

    rxnode *x = &R->nodes[n];

    /* One character, repeated: counted and given back in a loop, a whole
       character at a time. */
    if (done == 0 && (x->kind == RXN_CHAR || x->kind == RXN_ANY || x->kind == RXN_CLASS)) {
        int most = x->max < 0 ? R->len - pos : x->max;
        int k = 0, q = pos, w;
        if (x->greedy) {
            while (k < most && (w = rx_one(R, x, q))) { q += w; k++; }
            if (k < x->min) return 0;
            for (int i = k; i >= x->min; i--) {
                if (rx_run(R, x->next, q, cont)) return 1;
                if (!rx_budget(R)) return 0;
                if (i > x->min) q = rx_back(R, pos, q);
            }
        } else {
            while (k < x->min && (w = rx_one(R, x, q))) { q += w; k++; }
            if (k < x->min) return 0;
            for (;;) {
                if (rx_run(R, x->next, q, cont)) return 1;
                if (!rx_budget(R)) return 0;
                if (k >= most || !(w = rx_one(R, x, q))) break;
                q += w;
                k++;
            }
        }
        return 0;
    }

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

/* The first match at or after `from` -- or only at `from`, for a sticky
   pattern. Returns where it started, or -1. */
static int rx_search(rx *R, const char *s, int len, int from) {
    if (!R->ok) return -1;
    R->s = s;
    R->len = len;
    R->total = 0;

    /* Tried at the start of each character, not in the middle of one. */
    for (int start = from < 0 ? 0 : from, k = 1; start <= len; start += k) {
        k = 1;
        if (start < len && (u8)s[start] >= 0x80) rx_utf8(s + start, len - start, &k);
        R->ncont = 0;
        R->steps = 0;
        int upto = R->ncaps > R->ngroups ? R->ncaps : R->ngroups;
        for (int i = 0; i < upto && i < RX_CAPS; i++) {
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
        if (R->sticky || R->total > RX_SEARCH) break;
    }
    return -1;
}
