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

/* Nodes, one a character: a list of hundreds of words, each an alternative,
   is thousands, and Al Jazeera's went past the 4096 there were. The table
   grows as a pattern needs it, to this many. */
#define RX_NODES_FIRST 1024
#define RX_NODES   65536
/* Classes and their ranges past U+00FF (all classes together: \p{ID_Continue}
   is 400) grow as a pattern needs them, as the nodes do: a pattern with more
   than 96 classes stopped Netflix's page. */
#define RX_CLASSES_FIRST 96
#define RX_CLASSES 4096
#define RX_RANGES_FIRST 2048
#define RX_RANGES  65536
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
    rxnode *nodes;                   /* capnodes of them, kept from one pattern to the next */
    int    nnodes, capnodes;
    u8   (*classes)[32];             /* code points 0 to 255, a bit each */
    u8    *cneg;                     /* the class is turned inside out */
    int    nclasses, capclasses;
    u32   *rlo, *rhi;                /* and past 255, ranges, */
    u16   *rcls;                     /* each with its class */
    int    nranges, capranges;
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
    if (R->nnodes >= R->capnodes) {
        /* Nodes are named by number, never held by address across a call
           that makes one, so the table can move. */
        if (R->capnodes >= RX_NODES) return -1;
        int cap = R->capnodes ? R->capnodes * 2 : RX_NODES_FIRST;
        if (cap > RX_NODES) cap = RX_NODES;
        rxnode *more = (rxnode *)malloc((u64)cap * sizeof(rxnode));
        if (!more) return -1;
        for (int i = 0; i < R->nnodes; i++) more[i] = R->nodes[i];
        free(R->nodes);
        R->nodes = more;
        R->capnodes = cap;
    }
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
/* Room for one more range, the three lists moved together. */
static int rx_more_ranges(rx *R) {
    if (R->nranges < R->capranges) return 1;
    if (R->capranges >= RX_RANGES) return 0;
    int cap = R->capranges ? R->capranges * 2 : RX_RANGES_FIRST;
    if (cap > RX_RANGES) cap = RX_RANGES;
    u32 *lo = (u32 *)malloc((u64)cap * sizeof(u32)), *hi = (u32 *)malloc((u64)cap * sizeof(u32));
    u16 *cl = (u16 *)malloc((u64)cap * sizeof(u16));
    if (!lo || !hi || !cl) { free(lo); free(hi); free(cl); return 0; }
    for (int i = 0; i < R->nranges; i++) { lo[i] = R->rlo[i]; hi[i] = R->rhi[i]; cl[i] = R->rcls[i]; }
    free(R->rlo); free(R->rhi); free(R->rcls);
    R->rlo = lo; R->rhi = hi; R->rcls = cl;
    R->capranges = cap;
    return 1;
}

static void rx_cls_add(rx *R, int c, u32 lo, u32 hi) {
    if (c < 0 || c >= R->nclasses || hi < lo) return;
    for (u32 i = lo; i <= hi && i < 256; i++)
        R->classes[c][i >> 3] |= (u8)(1 << (i & 7));
    if (hi < 256) return;
    if (lo < 256) lo = 256;
    if (!rx_more_ranges(R)) { rx_fail(R, "too many ranges in its classes"); return; }
    R->rlo[R->nranges] = lo;
    R->rhi[R->nranges] = hi;
    R->rcls[R->nranges] = (u16)c;
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
        default:
            /* Anything else escaped is itself, and a character past ASCII is
               all of its bytes. Read as one signed byte, the entity escaper
               many pages carry (a class of every character it escapes, each
               after a backslash) had a range that ran backwards, and Al
               Jazeera's page stopped on it. */
            if ((u8)c >= 0x80) {
                int k;
                u32 cp = rx_utf8(p + *at - 1, len - (*at - 1), &k);
                *at += k - 1;
                return (int)cp;
            }
            return c;
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

/* The marks written on a letter, which identifiers and scripts with vowel
   signs are full of: GitHub's code view stopped on \p{Mn}. */
static const u32 RXP_MN[][2] = {
    { 0x300, 0x36F }, { 0x483, 0x487 }, { 0x591, 0x5BD }, { 0x5BF, 0x5BF }, { 0x5C1, 0x5C2 }, { 0x5C4, 0x5C5 },
    { 0x5C7, 0x5C7 }, { 0x610, 0x61A }, { 0x64B, 0x65F }, { 0x670, 0x670 }, { 0x6D6, 0x6DC }, { 0x6DF, 0x6E4 },
    { 0x6E7, 0x6E8 }, { 0x6EA, 0x6ED }, { 0x711, 0x711 }, { 0x730, 0x74A }, { 0x7A6, 0x7B0 }, { 0x7EB, 0x7F3 },
    { 0x816, 0x819 }, { 0x81B, 0x823 }, { 0x825, 0x827 }, { 0x829, 0x82D }, { 0x859, 0x85B }, { 0x898, 0x89F },
    { 0x8CA, 0x8E1 }, { 0x8E3, 0x902 }, { 0x93A, 0x93A }, { 0x93C, 0x93C }, { 0x941, 0x948 }, { 0x94D, 0x94D },
    { 0x951, 0x957 }, { 0x962, 0x963 }, { 0x981, 0x981 }, { 0x9BC, 0x9BC }, { 0x9C1, 0x9C4 }, { 0x9CD, 0x9CD },
    { 0x9E2, 0x9E3 }, { 0xA01, 0xA02 }, { 0xA3C, 0xA3C }, { 0xA41, 0xA51 }, { 0xA70, 0xA71 }, { 0xA75, 0xA75 },
    { 0xA81, 0xA82 }, { 0xABC, 0xABC }, { 0xAC1, 0xAC8 }, { 0xACD, 0xACD }, { 0xAE2, 0xAE3 }, { 0xB01, 0xB01 },
    { 0xB3C, 0xB3C }, { 0xB3F, 0xB3F }, { 0xB41, 0xB44 }, { 0xB4D, 0xB4D }, { 0xB55, 0xB56 }, { 0xB62, 0xB63 },
    { 0xB82, 0xB82 }, { 0xBC0, 0xBC0 }, { 0xBCD, 0xBCD }, { 0xC00, 0xC00 }, { 0xC04, 0xC04 }, { 0xC3C, 0xC3C },
    { 0xC3E, 0xC40 }, { 0xC46, 0xC56 }, { 0xC62, 0xC63 }, { 0xC81, 0xC81 }, { 0xCBC, 0xCBC }, { 0xCCC, 0xCCD },
    { 0xCE2, 0xCE3 }, { 0xD00, 0xD01 }, { 0xD3B, 0xD3C }, { 0xD41, 0xD44 }, { 0xD4D, 0xD4D }, { 0xD62, 0xD63 },
    { 0xD81, 0xD81 }, { 0xDCA, 0xDCA }, { 0xDD2, 0xDD6 }, { 0xE31, 0xE31 }, { 0xE34, 0xE3A }, { 0xE47, 0xE4E },
    { 0xEB1, 0xEB1 }, { 0xEB4, 0xEBC }, { 0xEC8, 0xECE }, { 0xF18, 0xF19 }, { 0xF35, 0xF35 }, { 0xF37, 0xF37 },
    { 0xF39, 0xF39 }, { 0xF71, 0xF7E }, { 0xF80, 0xF84 }, { 0xF86, 0xF87 }, { 0xF8D, 0xFBC }, { 0xFC6, 0xFC6 },
    { 0x102D, 0x1030 }, { 0x1032, 0x1037 }, { 0x1039, 0x103A }, { 0x103D, 0x103E }, { 0x1058, 0x1059 },
    { 0x105E, 0x1060 }, { 0x1071, 0x1074 }, { 0x1082, 0x1082 }, { 0x1085, 0x1086 }, { 0x108D, 0x108D },
    { 0x109D, 0x109D }, { 0x135D, 0x135F }, { 0x1712, 0x1714 }, { 0x1732, 0x1733 }, { 0x1752, 0x1753 },
    { 0x1772, 0x1773 }, { 0x17B4, 0x17B5 }, { 0x17B7, 0x17BD }, { 0x17C6, 0x17C6 }, { 0x17C9, 0x17D3 },
    { 0x17DD, 0x17DD }, { 0x180B, 0x180D }, { 0x180F, 0x180F }, { 0x1885, 0x1886 }, { 0x18A9, 0x18A9 },
    { 0x1920, 0x1922 }, { 0x1927, 0x1928 }, { 0x1932, 0x1932 }, { 0x1939, 0x193B }, { 0x1A17, 0x1A18 },
    { 0x1A1B, 0x1A1B }, { 0x1A56, 0x1A56 }, { 0x1A58, 0x1A60 }, { 0x1A62, 0x1A62 }, { 0x1A65, 0x1A6C },
    { 0x1A73, 0x1A7F }, { 0x1AB0, 0x1ABD }, { 0x1ABF, 0x1ACE }, { 0x1B00, 0x1B03 }, { 0x1B34, 0x1B34 },
    { 0x1B36, 0x1B3A }, { 0x1B3C, 0x1B3C }, { 0x1B42, 0x1B42 }, { 0x1B6B, 0x1B73 }, { 0x1B80, 0x1B81 },
    { 0x1BA2, 0x1BA5 }, { 0x1BA8, 0x1BA9 }, { 0x1BAB, 0x1BAD }, { 0x1BE6, 0x1BE6 }, { 0x1BE8, 0x1BE9 },
    { 0x1BED, 0x1BED }, { 0x1BEF, 0x1BF1 }, { 0x1C2C, 0x1C33 }, { 0x1C36, 0x1C37 }, { 0x1CD0, 0x1CD2 },
    { 0x1CD4, 0x1CE0 }, { 0x1CE2, 0x1CE8 }, { 0x1CED, 0x1CED }, { 0x1CF4, 0x1CF4 }, { 0x1CF8, 0x1CF9 },
    { 0x1DC0, 0x1DFF }, { 0x20D0, 0x20DC }, { 0x20E1, 0x20E1 }, { 0x20E5, 0x20F0 }, { 0x2CEF, 0x2CF1 },
    { 0x2D7F, 0x2D7F }, { 0x2DE0, 0x2DFF }, { 0x302A, 0x302D }, { 0x3099, 0x309A }, { 0xA66F, 0xA66F },
    { 0xA674, 0xA67D }, { 0xA69E, 0xA69F }, { 0xA6F0, 0xA6F1 }, { 0xA802, 0xA802 }, { 0xA806, 0xA806 },
    { 0xA80B, 0xA80B }, { 0xA825, 0xA826 }, { 0xA82C, 0xA82C }, { 0xA8C4, 0xA8C5 }, { 0xA8E0, 0xA8F1 },
    { 0xA8FF, 0xA8FF }, { 0xA926, 0xA92D }, { 0xA947, 0xA951 }, { 0xA980, 0xA982 }, { 0xA9B3, 0xA9B3 },
    { 0xA9B6, 0xA9B9 }, { 0xA9BC, 0xA9BD }, { 0xA9E5, 0xA9E5 }, { 0xAA29, 0xAA2E }, { 0xAA31, 0xAA32 },
    { 0xAA35, 0xAA36 }, { 0xAA43, 0xAA43 }, { 0xAA4C, 0xAA4C }, { 0xAA7C, 0xAA7C }, { 0xAAB0, 0xAAB0 },
    { 0xAAB2, 0xAAB4 }, { 0xAAB7, 0xAAB8 }, { 0xAABE, 0xAABF }, { 0xAAC1, 0xAAC1 }, { 0xAAEC, 0xAAED },
    { 0xAAF6, 0xAAF6 }, { 0xABE5, 0xABE5 }, { 0xABE8, 0xABE8 }, { 0xABED, 0xABED }, { 0xFB1E, 0xFB1E },
    { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F }, { 0x101FD, 0x101FD }, { 0x102E0, 0x102E0 }, { 0x10376, 0x1037A },
    { 0x10A01, 0x10A0F }, { 0x10A38, 0x10A3F }, { 0x10D24, 0x10D27 }, { 0x11001, 0x11001 }, { 0x11038, 0x11046 },
    { 0x1107F, 0x11081 }, { 0x110B3, 0x110B6 }, { 0x110B9, 0x110BA }, { 0x11100, 0x11102 }, { 0x11127, 0x1112B },
    { 0x1112D, 0x11134 }, { 0x16AF0, 0x16AF4 }, { 0x16B30, 0x16B36 }, { 0x1D167, 0x1D169 }, { 0x1D17B, 0x1D182 },
    { 0x1D185, 0x1D18B }, { 0x1D1AA, 0x1D1AD }, { 0x1E8D0, 0x1E8D6 }, { 0x1E944, 0x1E94A }, { 0xE0100, 0xE01EF }
};
static const u32 RXP_MC[][2] = {
    { 0x903, 0x903 }, { 0x93B, 0x93B }, { 0x93E, 0x940 }, { 0x949, 0x94C }, { 0x94E, 0x94F }, { 0x982, 0x983 },
    { 0x9BE, 0x9C0 }, { 0x9C7, 0x9CC }, { 0x9D7, 0x9D7 }, { 0xA03, 0xA03 }, { 0xA3E, 0xA40 }, { 0xA83, 0xA83 },
    { 0xABE, 0xAC0 }, { 0xAC9, 0xACC }, { 0xB02, 0xB03 }, { 0xB3E, 0xB3E }, { 0xB40, 0xB40 }, { 0xB47, 0xB4C },
    { 0xB57, 0xB57 }, { 0xBBE, 0xBBF }, { 0xBC1, 0xBCC }, { 0xBD7, 0xBD7 }, { 0xC01, 0xC03 }, { 0xC41, 0xC44 },
    { 0xC82, 0xC83 }, { 0xCBE, 0xCC4 }, { 0xCC7, 0xCCB }, { 0xCD5, 0xCD6 }, { 0xD02, 0xD03 }, { 0xD3E, 0xD40 },
    { 0xD46, 0xD4C }, { 0xD57, 0xD57 }, { 0xD82, 0xD83 }, { 0xDCF, 0xDD1 }, { 0xDD8, 0xDDF }, { 0xDF2, 0xDF3 },
    { 0xF3E, 0xF3F }, { 0xF7F, 0xF7F }, { 0x102B, 0x102C }, { 0x1031, 0x1031 }, { 0x1038, 0x1038 },
    { 0x103B, 0x103C }, { 0x1056, 0x1057 }, { 0x1062, 0x1064 }, { 0x1067, 0x106D }, { 0x1083, 0x1084 },
    { 0x1087, 0x108C }, { 0x108F, 0x108F }, { 0x109A, 0x109C }, { 0x17B6, 0x17B6 }, { 0x17BE, 0x17C5 },
    { 0x17C7, 0x17C8 }, { 0x1923, 0x1926 }, { 0x1929, 0x192B }, { 0x1930, 0x1931 }, { 0x1933, 0x1938 },
    { 0x1A19, 0x1A1A }, { 0x1A55, 0x1A55 }, { 0x1A57, 0x1A57 }, { 0x1A61, 0x1A61 }, { 0x1A63, 0x1A64 },
    { 0x1A6D, 0x1A72 }, { 0x1B04, 0x1B04 }, { 0x1B35, 0x1B35 }, { 0x1B3B, 0x1B3B }, { 0x1B3D, 0x1B41 },
    { 0x1B43, 0x1B44 }, { 0x1B82, 0x1B82 }, { 0x1BA1, 0x1BA1 }, { 0x1BA6, 0x1BA7 }, { 0x1BAA, 0x1BAA },
    { 0x1BE7, 0x1BE7 }, { 0x1BEA, 0x1BEC }, { 0x1BEE, 0x1BEE }, { 0x1BF2, 0x1BF3 }, { 0x1C24, 0x1C2B },
    { 0x1C34, 0x1C35 }, { 0x1CE1, 0x1CE1 }, { 0x1CF7, 0x1CF7 }, { 0x302E, 0x302F }, { 0xA823, 0xA824 },
    { 0xA827, 0xA827 }, { 0xA880, 0xA881 }, { 0xA8B4, 0xA8C3 }, { 0xA952, 0xA953 }, { 0xA983, 0xA983 },
    { 0xA9B4, 0xA9B5 }, { 0xA9BA, 0xA9BB }, { 0xA9BE, 0xA9C0 }, { 0xAA2F, 0xAA30 }, { 0xAA33, 0xAA34 },
    { 0xAA4D, 0xAA4D }, { 0xAA7B, 0xAA7B }, { 0xAA7D, 0xAA7D }, { 0xAAEB, 0xAAEB }, { 0xAAEE, 0xAAEF },
    { 0xAAF5, 0xAAF5 }, { 0xABE3, 0xABE4 }, { 0xABE6, 0xABE7 }, { 0xABE9, 0xABEA }, { 0xABEC, 0xABEC }
};
static const u32 RXP_ME[][2] = {
    { 0x488, 0x489 }, { 0x1ABE, 0x1ABE }, { 0x20DD, 0x20E0 }, { 0x20E2, 0x20E4 }, { 0xA670, 0xA672 }
};
static const u32 RXP_LT[][2] = {
    { 0x1C5, 0x1C5 }, { 0x1C8, 0x1C8 }, { 0x1CB, 0x1CB }, { 0x1F2, 0x1F2 }, { 0x1F88, 0x1F8F }, { 0x1F98, 0x1F9F },
    { 0x1FA8, 0x1FAF }, { 0x1FBC, 0x1FBC }, { 0x1FCC, 0x1FCC }, { 0x1FFC, 0x1FFC }
};
static const u32 RXP_LM[][2] = {
    { 0x2B0, 0x2C1 }, { 0x2C6, 0x2D1 }, { 0x2E0, 0x2E4 }, { 0x2EC, 0x2EC }, { 0x2EE, 0x2EE }, { 0x374, 0x374 },
    { 0x37A, 0x37A }, { 0x559, 0x559 }, { 0x640, 0x640 }, { 0x6E5, 0x6E6 }, { 0x7F4, 0x7F5 }, { 0xE46, 0xE46 },
    { 0xEC6, 0xEC6 }, { 0x1D2C, 0x1D6A }, { 0x3005, 0x3005 }, { 0x3031, 0x3035 }, { 0x309D, 0x309E },
    { 0x30FC, 0x30FE }, { 0xA015, 0xA015 }, { 0xFF70, 0xFF70 }, { 0xFF9E, 0xFF9F }
};
/* The letters of the scripts with no case. */
static const u32 RXP_LO[][2] = {
    { 0xAA, 0xAA }, { 0xBA, 0xBA }, { 0x1BB, 0x1BB }, { 0x1C0, 0x1C3 }, { 0x5D0, 0x5EA }, { 0x5EF, 0x5F2 },
    { 0x620, 0x63F }, { 0x641, 0x64A }, { 0x66E, 0x66F }, { 0x671, 0x6D3 }, { 0x6D5, 0x6D5 }, { 0x6EE, 0x6EF },
    { 0x6FA, 0x6FC }, { 0x6FF, 0x6FF }, { 0x710, 0x710 }, { 0x712, 0x72F }, { 0x74D, 0x7A5 }, { 0x904, 0x939 },
    { 0x93D, 0x93D }, { 0x950, 0x950 }, { 0x958, 0x961 }, { 0x972, 0x980 }, { 0x985, 0x9B9 }, { 0xA05, 0xA39 },
    { 0xA85, 0xAB9 }, { 0xB05, 0xB39 }, { 0xB85, 0xBB9 }, { 0xC05, 0xC39 }, { 0xC85, 0xCB9 }, { 0xD05, 0xD3A },
    { 0xD85, 0xDC6 }, { 0xE01, 0xE30 }, { 0xE32, 0xE33 }, { 0xE40, 0xE45 }, { 0xE81, 0xEB0 }, { 0xF00, 0xF00 },
    { 0xF40, 0xF6C }, { 0x1000, 0x102A }, { 0x10D0, 0x10FA }, { 0x10FD, 0x10FF }, { 0x1100, 0x11FF },
    { 0x1200, 0x135A }, { 0x1780, 0x17B3 }, { 0x2135, 0x2138 }, { 0x3006, 0x3006 }, { 0x303C, 0x303C },
    { 0x3041, 0x3096 }, { 0x309F, 0x309F }, { 0x30A1, 0x30FA }, { 0x30FF, 0x30FF }, { 0x3105, 0x312F },
    { 0x3131, 0x318E }, { 0x31A0, 0x31BF }, { 0x31F0, 0x31FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF },
    { 0xA000, 0xA014 }, { 0xA016, 0xA48C }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF }, { 0xFB1D, 0xFB1D },
    { 0xFB1F, 0xFB28 }, { 0xFB2A, 0xFBB1 }, { 0xFBD3, 0xFD3D }, { 0xFD50, 0xFDFB }, { 0xFE70, 0xFEFC },
    { 0xFF66, 0xFF6F }, { 0xFF71, 0xFF9D }, { 0xFFA0, 0xFFDC }, { 0x20000, 0x3134F }
};
static const u32 RXP_NL[][2] = {
    { 0x16EE, 0x16F0 }, { 0x2160, 0x2182 }, { 0x2185, 0x2188 }, { 0x3007, 0x3007 }, { 0x3021, 0x3029 },
    { 0x3038, 0x303A }, { 0xA6E6, 0xA6EF }, { 0x10140, 0x10174 }
};
static const u32 RXP_NO[][2] = {
    { 0xB2, 0xB3 }, { 0xB9, 0xB9 }, { 0xBC, 0xBE }, { 0x9F4, 0x9F9 }, { 0xBF0, 0xBF2 }, { 0xF2A, 0xF33 },
    { 0x1369, 0x137C }, { 0x17F0, 0x17F9 }, { 0x2070, 0x2070 }, { 0x2074, 0x2079 }, { 0x2080, 0x2089 },
    { 0x2150, 0x215F }, { 0x2189, 0x2189 }, { 0x2460, 0x249B }, { 0x24EA, 0x24FF }, { 0x2776, 0x2793 },
    { 0x2CFD, 0x2CFD }, { 0x3192, 0x3195 }, { 0x3220, 0x3229 }, { 0x3248, 0x324F }, { 0x3251, 0x325F },
    { 0x3280, 0x3289 }, { 0x32B1, 0x32BF }
};
static const u32 RXP_PC[][2] = {
    { '_', '_' }, { 0x203F, 0x2040 }, { 0x2054, 0x2054 }, { 0xFE33, 0xFE34 }, { 0xFE4D, 0xFE4F }, { 0xFF3F, 0xFF3F }
};
static const u32 RXP_PD[][2] = {
    { '-', '-' }, { 0x58A, 0x58A }, { 0x5BE, 0x5BE }, { 0x1400, 0x1400 }, { 0x1806, 0x1806 }, { 0x2010, 0x2015 },
    { 0x2E17, 0x2E17 }, { 0x2E1A, 0x2E1A }, { 0x2E3A, 0x2E3B }, { 0x2E40, 0x2E40 }, { 0x301C, 0x301C },
    { 0x3030, 0x3030 }, { 0x30A0, 0x30A0 }, { 0xFE31, 0xFE32 }, { 0xFE58, 0xFE58 }, { 0xFE63, 0xFE63 }, { 0xFF0D, 0xFF0D }
};
static const u32 RXP_PS[][2] = {
    { '(', '(' }, { '[', '[' }, { '{', '{' }, { 0xF3A, 0xF3A }, { 0xF3C, 0xF3C }, { 0x169B, 0x169B },
    { 0x201A, 0x201A }, { 0x201E, 0x201E }, { 0x2045, 0x2045 }, { 0x207D, 0x207D }, { 0x208D, 0x208D },
    { 0x2308, 0x2308 }, { 0x230A, 0x230A }, { 0x2329, 0x2329 }, { 0x3008, 0x3008 }, { 0x300A, 0x300A },
    { 0x300C, 0x300C }, { 0x300E, 0x300E }, { 0x3010, 0x3010 }, { 0x3014, 0x3014 }, { 0x3016, 0x3016 },
    { 0x3018, 0x3018 }, { 0x301A, 0x301A }, { 0x301D, 0x301D }, { 0xFF08, 0xFF08 }, { 0xFF3B, 0xFF3B },
    { 0xFF5B, 0xFF5B }, { 0xFF5F, 0xFF5F }, { 0xFF62, 0xFF62 }
};
static const u32 RXP_PE[][2] = {
    { ')', ')' }, { ']', ']' }, { '}', '}' }, { 0xF3B, 0xF3B }, { 0xF3D, 0xF3D }, { 0x169C, 0x169C },
    { 0x2046, 0x2046 }, { 0x207E, 0x207E }, { 0x208E, 0x208E }, { 0x2309, 0x2309 }, { 0x230B, 0x230B },
    { 0x232A, 0x232A }, { 0x3009, 0x3009 }, { 0x300B, 0x300B }, { 0x300D, 0x300D }, { 0x300F, 0x300F },
    { 0x3011, 0x3011 }, { 0x3015, 0x3015 }, { 0x3017, 0x3017 }, { 0x3019, 0x3019 }, { 0x301B, 0x301B },
    { 0x301E, 0x301F }, { 0xFF09, 0xFF09 }, { 0xFF3D, 0xFF3D }, { 0xFF5D, 0xFF5D }, { 0xFF60, 0xFF60 }, { 0xFF63, 0xFF63 }
};
static const u32 RXP_PI[][2] = {
    { 0xAB, 0xAB }, { 0x2018, 0x2018 }, { 0x201B, 0x201C }, { 0x201F, 0x201F }, { 0x2039, 0x2039 },
    { 0x2E02, 0x2E02 }, { 0x2E04, 0x2E04 }, { 0x2E09, 0x2E09 }, { 0x2E0C, 0x2E0C }, { 0x2E1C, 0x2E1C }, { 0x2E20, 0x2E20 }
};
static const u32 RXP_PF[][2] = {
    { 0xBB, 0xBB }, { 0x2019, 0x2019 }, { 0x201D, 0x201D }, { 0x203A, 0x203A }, { 0x2E03, 0x2E03 },
    { 0x2E05, 0x2E05 }, { 0x2E0A, 0x2E0A }, { 0x2E0D, 0x2E0D }, { 0x2E1D, 0x2E1D }, { 0x2E21, 0x2E21 }
};
static const u32 RXP_PO[][2] = {
    { '!', '#' }, { '%', '\'' }, { '*', '*' }, { ',', ',' }, { '.', '/' }, { ':', ';' }, { '?', '@' },
    { '\\', '\\' }, { 0xA1, 0xA1 }, { 0xA7, 0xA7 }, { 0xB6, 0xB7 }, { 0xBF, 0xBF }, { 0x2016, 0x2017 },
    { 0x2020, 0x2027 }, { 0x2030, 0x2038 }, { 0x203B, 0x203E }, { 0x2041, 0x2043 }, { 0x2047, 0x2051 },
    { 0x2053, 0x2053 }, { 0x2055, 0x205E }, { 0x3001, 0x3003 }, { 0xFE10, 0xFE16 }, { 0xFE19, 0xFE19 },
    { 0xFE30, 0xFE30 }, { 0xFE45, 0xFE46 }, { 0xFE49, 0xFE4C }, { 0xFE50, 0xFE57 }, { 0xFE5F, 0xFE61 },
    { 0xFE68, 0xFE68 }, { 0xFE6A, 0xFE6B }, { 0xFF01, 0xFF03 }, { 0xFF05, 0xFF07 }, { 0xFF0A, 0xFF0A },
    { 0xFF0C, 0xFF0C }, { 0xFF0E, 0xFF0F }, { 0xFF1A, 0xFF1B }, { 0xFF1F, 0xFF20 }, { 0xFF3C, 0xFF3C },
    { 0xFF61, 0xFF61 }, { 0xFF64, 0xFF65 }
};
static const u32 RXP_SM[][2] = {
    { '+', '+' }, { '<', '>' }, { '|', '|' }, { '~', '~' }, { 0xAC, 0xAC }, { 0xB1, 0xB1 }, { 0xD7, 0xD7 },
    { 0xF7, 0xF7 }, { 0x3F6, 0x3F6 }, { 0x606, 0x608 }, { 0x2044, 0x2044 }, { 0x2052, 0x2052 }, { 0x207A, 0x207C },
    { 0x208A, 0x208C }, { 0x2118, 0x2118 }, { 0x2140, 0x2144 }, { 0x214B, 0x214B }, { 0x2190, 0x2194 },
    { 0x219A, 0x219B }, { 0x21A0, 0x21A0 }, { 0x21A3, 0x21A3 }, { 0x21A6, 0x21A6 }, { 0x21AE, 0x21AE },
    { 0x21CE, 0x21CF }, { 0x21D2, 0x21D2 }, { 0x21D4, 0x21D4 }, { 0x21F4, 0x22FF }, { 0x2320, 0x2321 },
    { 0x237C, 0x237C }, { 0x239B, 0x23B3 }, { 0x23DC, 0x23E1 }, { 0x25B7, 0x25B7 }, { 0x25C1, 0x25C1 },
    { 0x25F8, 0x25FF }, { 0x266F, 0x266F }, { 0x27C0, 0x27C4 }, { 0x27C7, 0x27E5 }, { 0x27F0, 0x27FF },
    { 0x2900, 0x2982 }, { 0x2999, 0x29D7 }, { 0x29DC, 0x29FB }, { 0x29FE, 0x2AFF }, { 0x2B30, 0x2B44 },
    { 0x2B47, 0x2B4C }, { 0xFB29, 0xFB29 }, { 0xFE62, 0xFE62 }, { 0xFE64, 0xFE66 }, { 0xFF0B, 0xFF0B },
    { 0xFF1C, 0xFF1E }, { 0xFF5C, 0xFF5C }, { 0xFF5E, 0xFF5E }, { 0xFFE2, 0xFFE2 }, { 0xFFE9, 0xFFEC }
};
static const u32 RXP_SC[][2] = {
    { '$', '$' }, { 0xA2, 0xA5 }, { 0x58F, 0x58F }, { 0x60B, 0x60B }, { 0x7FE, 0x7FF }, { 0x9F2, 0x9F3 },
    { 0x9FB, 0x9FB }, { 0xAF1, 0xAF1 }, { 0xBF9, 0xBF9 }, { 0xE3F, 0xE3F }, { 0x17DB, 0x17DB }, { 0x20A0, 0x20C0 },
    { 0xA838, 0xA838 }, { 0xFDFC, 0xFDFC }, { 0xFE69, 0xFE69 }, { 0xFF04, 0xFF04 }, { 0xFFE0, 0xFFE1 }, { 0xFFE5, 0xFFE6 }
};
static const u32 RXP_SK[][2] = {
    { '^', '^' }, { '`', '`' }, { 0xA8, 0xA8 }, { 0xAF, 0xAF }, { 0xB4, 0xB4 }, { 0xB8, 0xB8 }, { 0x2C2, 0x2C5 },
    { 0x2D2, 0x2DF }, { 0x2E5, 0x2EB }, { 0x2ED, 0x2ED }, { 0x2EF, 0x2FF }, { 0x375, 0x375 }, { 0x384, 0x385 },
    { 0x1FBD, 0x1FBD }, { 0x1FBF, 0x1FC1 }, { 0x1FCD, 0x1FCF }, { 0x1FDD, 0x1FDF }, { 0x1FED, 0x1FEF },
    { 0x1FFD, 0x1FFE }, { 0x309B, 0x309C }, { 0xA700, 0xA716 }, { 0xA720, 0xA721 }, { 0xA789, 0xA78A },
    { 0xAB5B, 0xAB5B }, { 0xFBB2, 0xFBC1 }, { 0xFF3E, 0xFF3E }, { 0xFF40, 0xFF40 }, { 0xFFE3, 0xFFE3 },
    { 0x1F3FB, 0x1F3FF }
};
static const u32 RXP_CC[][2] = { { 0x0, 0x1F }, { 0x7F, 0x9F } };
static const u32 RXP_CF[][2] = {
    { 0xAD, 0xAD }, { 0x600, 0x605 }, { 0x61C, 0x61C }, { 0x6DD, 0x6DD }, { 0x70F, 0x70F }, { 0x8E2, 0x8E2 },
    { 0x180E, 0x180E }, { 0x200B, 0x200F }, { 0x202A, 0x202E }, { 0x2060, 0x2064 }, { 0x2066, 0x206F },
    { 0xFEFF, 0xFEFF }, { 0xFFF9, 0xFFFB }, { 0x110BD, 0x110BD }, { 0x1D173, 0x1D17A }, { 0xE0001, 0xE0001 },
    { 0xE0020, 0xE007F }
};
static const u32 RXP_CO[][2] = { { 0xE000, 0xF8FF }, { 0xF0000, 0xFFFFD }, { 0x100000, 0x10FFFD } };
static const u32 RXP_CS[][2] = { { 0xD800, 0xDFFF } };
static const u32 RXP_ZL[][2] = { { 0x2028, 0x2028 } };
static const u32 RXP_ZP[][2] = { { 0x2029, 0x2029 } };
static const u32 RXP_ANY[][2] = { { 0x0, 0x10FFFF } };
static const u32 RXP_ASCII[][2] = { { 0x0, 0x7F } };
/* What else an identifier may go on with after its first letter. */
static const u32 RXP_IDC_MORE[][2] = { { 0xB7, 0xB7 }, { 0x387, 0x387 }, { 0x1369, 0x1371 }, { 0x19DA, 0x19DA } };

/* The scripts pages match by name, and their characters. */
static const u32 RXS_LATIN[][2] = {
    { 'A', 'Z' }, { 'a', 'z' }, { 0xAA, 0xAA }, { 0xBA, 0xBA }, { 0xC0, 0xD6 }, { 0xD8, 0xF6 }, { 0xF8, 0x2B8 },
    { 0x2E0, 0x2E4 }, { 0x1D00, 0x1D25 }, { 0x1D2C, 0x1D5C }, { 0x1D62, 0x1D65 }, { 0x1D6B, 0x1D77 },
    { 0x1D79, 0x1DBE }, { 0x1E00, 0x1EFF }, { 0x2071, 0x2071 }, { 0x207F, 0x207F }, { 0x2090, 0x209C },
    { 0x212A, 0x212B }, { 0x2132, 0x2132 }, { 0x214E, 0x214E }, { 0x2160, 0x2188 }, { 0x2C60, 0x2C7F },
    { 0xA722, 0xA787 }, { 0xA78B, 0xA7FF }, { 0xAB30, 0xAB5A }, { 0xAB5C, 0xAB64 }, { 0xFB00, 0xFB06 },
    { 0xFF21, 0xFF3A }, { 0xFF41, 0xFF5A }
};
static const u32 RXS_GREEK[][2] = {
    { 0x370, 0x373 }, { 0x375, 0x377 }, { 0x37A, 0x37D }, { 0x37F, 0x37F }, { 0x384, 0x384 }, { 0x386, 0x386 },
    { 0x388, 0x3E1 }, { 0x3F0, 0x3FF }, { 0x1D26, 0x1D2A }, { 0x1F00, 0x1FFE }, { 0x2126, 0x2126 }
};
static const u32 RXS_CYRILLIC[][2] = {
    { 0x400, 0x484 }, { 0x487, 0x52F }, { 0x1C80, 0x1C88 }, { 0x1D2B, 0x1D2B }, { 0x2DE0, 0x2DFF }, { 0xA640, 0xA69F }
};
static const u32 RXS_HAN[][2] = {
    { 0x2E80, 0x2E99 }, { 0x2E9B, 0x2EF3 }, { 0x2F00, 0x2FD5 }, { 0x3005, 0x3005 }, { 0x3007, 0x3007 },
    { 0x3021, 0x3029 }, { 0x3038, 0x303B }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF }, { 0xF900, 0xFA6D },
    { 0xFA70, 0xFAD9 }, { 0x20000, 0x2FA1F }, { 0x30000, 0x3134F }
};
static const u32 RXS_HIRAGANA[][2] = { { 0x3041, 0x3096 }, { 0x309D, 0x309F }, { 0x1B001, 0x1B11F } };
static const u32 RXS_KATAKANA[][2] = {
    { 0x30A1, 0x30FA }, { 0x30FD, 0x30FF }, { 0x31F0, 0x31FF }, { 0x32D0, 0x32FE }, { 0x3300, 0x3357 },
    { 0xFF66, 0xFF6F }, { 0xFF71, 0xFF9D }
};
static const u32 RXS_HANGUL[][2] = {
    { 0x1100, 0x11FF }, { 0x302E, 0x302F }, { 0x3131, 0x318E }, { 0x3200, 0x321E }, { 0x3260, 0x327E },
    { 0xA960, 0xA97C }, { 0xAC00, 0xD7A3 }, { 0xD7B0, 0xD7FB }, { 0xFFA0, 0xFFDC }
};
static const u32 RXS_ARABIC[][2] = {
    { 0x600, 0x604 }, { 0x606, 0x60B }, { 0x60D, 0x61A }, { 0x61C, 0x61E }, { 0x620, 0x63F }, { 0x641, 0x64A },
    { 0x656, 0x66F }, { 0x671, 0x6DC }, { 0x6DE, 0x6FF }, { 0x750, 0x77F }, { 0x870, 0x8FF }, { 0xFB50, 0xFD3D },
    { 0xFD40, 0xFDFF }, { 0xFE70, 0xFEFC }
};
static const u32 RXS_HEBREW[][2] = { { 0x591, 0x5C7 }, { 0x5D0, 0x5EA }, { 0x5EF, 0x5F4 }, { 0xFB1D, 0xFB4F } };
static const u32 RXS_DEVANAGARI[][2] = { { 0x900, 0x950 }, { 0x955, 0x963 }, { 0x966, 0x97F }, { 0xA8E0, 0xA8FF } };
static const u32 RXS_THAI[][2] = { { 0xE01, 0xE3A }, { 0xE40, 0xE5B } };
static const u32 RXS_ARMENIAN[][2] = { { 0x531, 0x556 }, { 0x559, 0x58A }, { 0x58D, 0x58F }, { 0xFB13, 0xFB17 } };
static const u32 RXS_GEORGIAN[][2] = { { 0x10A0, 0x10C5 }, { 0x10C7, 0x10C7 }, { 0x10CD, 0x10CD }, { 0x10D0, 0x10FA },
                                       { 0x10FC, 0x10FF }, { 0x1C90, 0x1CBA }, { 0x1CBD, 0x1CBF }, { 0x2D00, 0x2D25 } };

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

/* Several lists as one class, or everything outside all of them: merged
   first, since the outside of each one on its own would be everything. */
typedef struct { const u32 (*t)[2]; int n; } rx_list;
#define RXL(t) { t, (int)(sizeof(t) / sizeof(t[0])) }

static void rx_add_lists(rx *R, int c, const rx_list *ls, int nl, int neg) {
    static u32 m[2048][2];
    int n = 0;
    for (int i = 0; i < nl; i++)
        for (int k = 0; k < ls[i].n && n < 2048; k++) { m[n][0] = ls[i].t[k][0]; m[n][1] = ls[i].t[k][1]; n++; }
    /* Sorted by where each starts, then run together where they meet. */
    for (int i = 1; i < n; i++) {
        u32 lo = m[i][0], hi = m[i][1];
        int j = i - 1;
        while (j >= 0 && m[j][0] > lo) { m[j + 1][0] = m[j][0]; m[j + 1][1] = m[j][1]; j--; }
        m[j + 1][0] = lo;
        m[j + 1][1] = hi;
    }
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (w && m[i][0] <= m[w - 1][1] + 1) { if (m[i][1] > m[w - 1][1]) m[w - 1][1] = m[i][1]; }
        else { m[w][0] = m[i][0]; m[w][1] = m[i][1]; w++; }
    }
    rx_add_list(R, c, (const u32 (*)[2])m, w, neg);
}

/* 0 when the name is not one of those. */
static int rx_fill_property(rx *R, int c, const char *nm, int n, int neg) {
    #define IS(w) (n == (int)sizeof(w) - 1 && !strncmp(nm, w, n))
    #define LIST(t) rx_add_list(R, c, t, (int)(sizeof(t) / sizeof(t[0])), neg)
    if (IS("L") || IS("Letter") || IS("Alphabetic") || IS("Alpha")) LIST(RXP_L);
    else if (IS("Lu") || IS("Uppercase_Letter") || IS("Uppercase")) LIST(RXP_LU);
    else if (IS("Ll") || IS("Lowercase_Letter") || IS("Lowercase")) LIST(RXP_LL);
    else if (IS("Lt") || IS("Titlecase_Letter")) LIST(RXP_LT);
    else if (IS("Lm") || IS("Modifier_Letter")) LIST(RXP_LM);
    else if (IS("Lo") || IS("Other_Letter")) LIST(RXP_LO);
    else if (IS("LC") || IS("Cased_Letter")) { rx_list l[] = { RXL(RXP_LU), RXL(RXP_LL), RXL(RXP_LT) }; rx_add_lists(R, c, l, 3, neg); }
    else if (IS("M") || IS("Mark") || IS("Combining_Mark")) { rx_list l[] = { RXL(RXP_MN), RXL(RXP_MC), RXL(RXP_ME) }; rx_add_lists(R, c, l, 3, neg); }
    else if (IS("Mn") || IS("Nonspacing_Mark")) LIST(RXP_MN);
    else if (IS("Mc") || IS("Spacing_Mark")) LIST(RXP_MC);
    else if (IS("Me") || IS("Enclosing_Mark")) LIST(RXP_ME);
    else if (IS("N") || IS("Number")) LIST(RXP_N);
    else if (IS("Nd") || IS("Decimal_Number") || IS("digit")) LIST(RXP_ND);
    else if (IS("Nl") || IS("Letter_Number")) LIST(RXP_NL);
    else if (IS("No") || IS("Other_Number")) LIST(RXP_NO);
    else if (IS("P") || IS("Punctuation") || IS("punct")) LIST(RXP_P);
    else if (IS("Pc") || IS("Connector_Punctuation")) LIST(RXP_PC);
    else if (IS("Pd") || IS("Dash_Punctuation")) LIST(RXP_PD);
    else if (IS("Ps") || IS("Open_Punctuation")) LIST(RXP_PS);
    else if (IS("Pe") || IS("Close_Punctuation")) LIST(RXP_PE);
    else if (IS("Pi") || IS("Initial_Punctuation")) LIST(RXP_PI);
    else if (IS("Pf") || IS("Final_Punctuation")) LIST(RXP_PF);
    else if (IS("Po") || IS("Other_Punctuation")) LIST(RXP_PO);
    else if (IS("Z") || IS("Zs") || IS("White_Space") || IS("Space_Separator") || IS("Separator")) LIST(RXP_Z);
    else if (IS("Zl") || IS("Line_Separator")) LIST(RXP_ZL);
    else if (IS("Zp") || IS("Paragraph_Separator")) LIST(RXP_ZP);
    else if (IS("S") || IS("Symbol") || IS("So") || IS("Other_Symbol")) LIST(RXP_S);
    else if (IS("Sm") || IS("Math_Symbol") || IS("Math")) LIST(RXP_SM);
    else if (IS("Sc") || IS("Currency_Symbol")) LIST(RXP_SC);
    else if (IS("Sk") || IS("Modifier_Symbol")) LIST(RXP_SK);
    else if (IS("C") || IS("Other")) { rx_list l[] = { RXL(RXP_CC), RXL(RXP_CF), RXL(RXP_CO), RXL(RXP_CS) }; rx_add_lists(R, c, l, 4, neg); }
    else if (IS("Cc") || IS("Control") || IS("cntrl")) LIST(RXP_CC);
    else if (IS("Cf") || IS("Format")) LIST(RXP_CF);
    else if (IS("Co") || IS("Private_Use")) LIST(RXP_CO);
    else if (IS("Cs") || IS("Surrogate")) LIST(RXP_CS);
    else if (IS("Any")) LIST(RXP_ANY);
    else if (IS("ASCII")) LIST(RXP_ASCII);
    else if (IS("ID_Start") || IS("IDS") || IS("XID_Start") || IS("XIDS")) {
        rx_list l[] = { RXL(RXP_L), RXL(RXP_NL) };
        rx_add_lists(R, c, l, 2, neg);
    } else if (IS("ID_Continue") || IS("IDC") || IS("XID_Continue") || IS("XIDC")) {
        rx_list l[] = { RXL(RXP_L), RXL(RXP_NL), RXL(RXP_MN), RXL(RXP_MC), RXL(RXP_ND), RXL(RXP_PC), RXL(RXP_IDC_MORE) };
        rx_add_lists(R, c, l, 7, neg);
    } else if (IS("Emoji") || IS("Extended_Pictographic") || IS("Emoji_Presentation")) LIST(RXP_EMOJI);
    else return 0;
    #undef LIST
    #undef IS
    return 1;
}

/* Script=Name (and Script_Extensions=, which is answered the same), by its
   long name or its four letters. 0 for a script not listed here. */
static int rx_fill_script(rx *R, int c, const char *nm, int n, int neg) {
    #define IS(w) (n == (int)sizeof(w) - 1 && !strncmp(nm, w, n))
    #define LIST(t) rx_add_list(R, c, t, (int)(sizeof(t) / sizeof(t[0])), neg)
    if (IS("Latin") || IS("Latn")) LIST(RXS_LATIN);
    else if (IS("Greek") || IS("Grek")) LIST(RXS_GREEK);
    else if (IS("Cyrillic") || IS("Cyrl")) LIST(RXS_CYRILLIC);
    else if (IS("Han") || IS("Hani")) LIST(RXS_HAN);
    else if (IS("Hiragana") || IS("Hira")) LIST(RXS_HIRAGANA);
    else if (IS("Katakana") || IS("Kana")) LIST(RXS_KATAKANA);
    else if (IS("Hangul") || IS("Hang")) LIST(RXS_HANGUL);
    else if (IS("Arabic") || IS("Arab")) LIST(RXS_ARABIC);
    else if (IS("Hebrew") || IS("Hebr")) LIST(RXS_HEBREW);
    else if (IS("Devanagari") || IS("Deva")) LIST(RXS_DEVANAGARI);
    else if (IS("Thai")) LIST(RXS_THAI);
    else if (IS("Armenian") || IS("Armn")) LIST(RXS_ARMENIAN);
    else if (IS("Georgian") || IS("Geor")) LIST(RXS_GEORGIAN);
    else return 0;
    #undef LIST
    #undef IS
    return 1;
}

static int rx_new_class(rx *R) {
    if (R->nclasses >= R->capclasses) {
        /* Named by number, like the nodes, so the table can move. */
        int cap = R->capclasses ? R->capclasses * 2 : RX_CLASSES_FIRST;
        if (cap > RX_CLASSES) cap = RX_CLASSES;
        u8 (*more)[32] = R->capclasses < RX_CLASSES ? (u8 (*)[32])malloc((u64)cap * 32) : 0;
        u8 *neg = more ? (u8 *)malloc((u64)cap) : 0;
        if (!more || !neg) { free(more); rx_fail(R, "too many classes"); return -1; }
        for (int i = 0; i < R->nclasses; i++) {
            for (int k = 0; k < 32; k++) more[i][k] = R->classes[i][k];
            neg[i] = R->cneg[i];
        }
        free(R->classes); free(R->cneg);
        R->classes = more; R->cneg = neg;
        R->capclasses = cap;
    }
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
    /* General_Category=L and gc=L; Script=, sc=, Script_Extensions= and scx=
       by the script's characters, and a script not listed as letters. */
    int script = 0;
    for (int i = 0; i < n; i++) if (nm[i] == '=') {
        script = (i == 6 && !strncmp(nm, "Script", 6)) || (i == 2 && !strncmp(nm, "sc", 2))
              || (i == 17 && !strncmp(nm, "Script_Extensions", 17)) || (i == 3 && !strncmp(nm, "scx", 3));
        nm += i + 1;
        n -= i + 1;
        break;
    }
    *at = k + 1;
    if (script) {
        if (!rx_fill_script(R, c, nm, n, neg)) rx_fill_property(R, c, "L", 1, neg);
        return R->ok;
    }
    if (!rx_fill_property(R, c, nm, n, neg)) {
        /* Which one, so that a page's failure says what is missing. */
        char why[64];
        const char *lead = "a \\p{...} class this does not know: ";
        int w = 0;
        for (; lead[w]; w++) why[w] = lead[w];
        for (int i = start; i < k && w < (int)sizeof(why) - 1; i++) why[w++] = p[i];
        why[w] = 0;
        rx_fail(R, why);
        return 0;
    }
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
    /* Each class is cleared when it is made (rx_new_class). */
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
    int top = rx_parse_alt(R, pat, len, &at, 0);
    R->nodes[root].alt = top;
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
