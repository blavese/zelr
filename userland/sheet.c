/* Sheet: a grid of cells, each a number, words, or a formula worked out from
 * others (=A1+B1, =SUM(A1:B3)), kept as CSV.
 *
 * Every cell is worked out again after any change, from the text typed into
 * it. A pass marks what it has done, so a cell asked for twice is worked out
 * once, and one asked for while it is still being worked out is in a circle
 * of its own references and says #cycle. A sheet is 26 columns by 200 rows,
 * and working all of it out again is quicker than keeping track of what
 * depends on what.
 *
 * What is chosen is said on the console ("sheet: C2 =SUM(A1:B2) is 90"), and
 * so are saving and opening, for sheetcheck.py.
 *
 * Since 0.211.0 a formula also compares (= <> < > <= >=, giving TRUE or
 * FALSE), joins words with &, holds words in quotes ("say ""hi"""), and
 * knows IF, AND, OR, NOT, ROUND, ABS and LEN. Words a formula makes are
 * kept in a pool that every pass of working out starts afresh. Since
 * 0.227.0 it knows UPPER, LOWER, TRIM, LEFT, RIGHT, MID and FIND as well.
 */
#include "zelr.h"
#include "ui.h"

#define NCOL     26
#define NROW     200
#define TEXT_MAX 96

#define HEAD_W   44             /* the row numbers down the side */
#define CELL_W   92
#define CELL_H   24
#define NAME_W   60             /* the box that says which cell */
#define PATH_MAX 256

enum { V_EMPTY, V_NUM, V_TEXT, V_ERR };

typedef struct {
    int         kind;
    double      num;
    const char *str;            /* words; an error's name; TRUE or FALSE for a number that is one */
} val;

typedef struct {
    char        text[TEXT_MAX];
    val         v;
    int         pass, busy;     /* the pass it was worked out in; being worked out now */
} cell_t;

static cell_t cells[NROW][NCOL];
static int pass_no;

static val number(double d) { val v = { V_NUM, d, 0 }; return v; }
static val error(const char *why) { val v = { V_ERR, 0, why }; return v; }
static val truth(int b) { val v = { V_NUM, b ? 1 : 0, b ? "TRUE" : "FALSE" }; return v; }

/* Words a formula made, kept until the next pass of working out begins.
   A cell holds 95 bytes of words, so a pass can make 5200 cells' worth
   and still have room for what was made on the way. */
static char pool[1 << 20];
static u32 pool_n;

static val words2(const char *a, int na, const char *b, int nb) {
    if (na + nb > TEXT_MAX - 1) nb = TEXT_MAX - 1 - na;
    if (nb < 0) { nb = 0; na = TEXT_MAX - 1; }
    if (pool_n + (u32)(na + nb) + 1 > sizeof(pool)) return error("#room");
    char *out = pool + pool_n;
    for (int i = 0; i < na; i++) out[i] = a[i];
    for (int i = 0; i < nb; i++) out[na + i] = b[i];
    out[na + nb] = 0;
    pool_n += (u32)(na + nb) + 1;
    val v = { V_TEXT, 0, out };
    return v;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

/* --- numbers -------------------------------------------------------------- */

/* A number as written, digits with a point and an exponent: how much of s it
   took, or 0 when there is none there. The digits are gathered whole and
   divided once, so 0.1 is the nearest double to a tenth and not a sum of
   rounded tenths. */
static int read_number(const char *s, double *out) {
    int i = 0, digits = 0, after = 0;
    double v = 0;
    while (is_digit(s[i])) { v = v * 10 + (s[i] - '0'); i++; digits++; }
    if (s[i] == '.') {
        i++;
        while (is_digit(s[i])) { v = v * 10 + (s[i] - '0'); i++; digits++; after++; }
    }
    if (!digits) return 0;
    double scale = 1;
    while (after--) scale *= 10;
    v /= scale;
    if ((s[i] == 'e' || s[i] == 'E')
        && (is_digit(s[i + 1]) || ((s[i + 1] == '-' || s[i + 1] == '+') && is_digit(s[i + 2])))) {
        i++;
        int neg = 0, e = 0;
        if (s[i] == '-' || s[i] == '+') { neg = s[i] == '-'; i++; }
        while (is_digit(s[i])) { if (e < 400) e = e * 10 + (s[i] - '0'); i++; }
        while (e--) v = neg ? v / 10 : v * 10;
    }
    *out = v;
    return i;
}

/* Six places at most, and none of them trailing zeros: a third is 0.333333,
   a half 0.5, and 90 is 90. Past a million million the places go, and past a
   thousand million million it is written with an exponent. */
static int fixed_text(double v, char *out) {
    int n = 0;
    u64 whole, frac = 0;
    if (v < 1e12) {
        u64 scaled = (u64)(v * 1000000.0 + 0.5);
        whole = scaled / 1000000;
        frac = scaled % 1000000;
    } else {
        whole = (u64)(v + 0.5);
    }
    char d[24];
    int k = 0;
    if (!whole) d[k++] = '0';
    while (whole) { d[k++] = (char)('0' + whole % 10); whole /= 10; }
    while (k) out[n++] = d[--k];
    if (frac) {
        char f[6];
        for (int i = 5; i >= 0; i--) { f[i] = (char)('0' + frac % 10); frac /= 10; }
        int last = 5;
        while (last >= 0 && f[last] == '0') last--;
        out[n++] = '.';
        for (int i = 0; i <= last; i++) out[n++] = f[i];
    }
    out[n] = 0;
    return n;
}

static void num_text(double v, char *out) {
    int n = 0;
    if (v < 0) { out[n++] = '-'; v = -v; }
    if (v >= 1e15) {
        int e = 0;
        while (v >= 10) { v /= 10; e++; }
        n += fixed_text(v, out + n);
        out[n++] = 'e';
        n += utoa((u32)e, out + n);
        out[n] = 0;
    } else {
        fixed_text(v, out + n);
    }
    if (!strcmp(out, "-0")) strcpy(out, "0");
}

/* --- working a cell out --------------------------------------------------- */

static val cell_value(int r, int c);
static void num_text(double v, char *out);

/* A value as words, for & and LEN: a number as it is shown, TRUE or FALSE,
   nothing for an empty cell. */
static const char *as_words(val v, char *buf) {
    if (v.kind == V_TEXT) return v.str;
    if (v.kind == V_NUM && v.str) return v.str;
    if (v.kind == V_NUM) { num_text(v.num, buf); return buf; }
    return "";
}

/* What a cell's text is when it is not a formula: nothing, a number (all of
   it, with a sign), or words. */
static val plain(const char *t) {
    const char *s = t;
    while (*s == ' ') s++;
    if (!*s) { val v = { V_EMPTY, 0, 0 }; return v; }
    int neg = 0;
    if (*s == '-' || *s == '+') { neg = *s == '-'; s++; }
    double d;
    int n = read_number(s, &d);
    if (n) {
        const char *e = s + n;
        while (*e == ' ') e++;
        if (!*e) return number(neg ? -d : d);
    }
    val v = { V_TEXT, 0, t };
    return v;
}

typedef struct {
    const char *p;
    int bad;                    /* it does not read as a formula */
} parse_t;

static void skip(parse_t *P) { while (*P->p == ' ') P->p++; }
static val syntax(parse_t *P) { P->bad = 1; return error("#error"); }

/* Nothing counts as nought in sums; words count as nothing that can be
   added. An error wins over everything after it. */
static val arith(val a, char op, val b) {
    if (a.kind == V_ERR) return a;
    if (b.kind == V_ERR) return b;
    if (a.kind == V_TEXT || b.kind == V_TEXT) return error("#value");
    double x = a.kind == V_NUM ? a.num : 0, y = b.kind == V_NUM ? b.num : 0, z;
    switch (op) {
        case '+': z = x + y; break;
        case '-': z = x - y; break;
        case '*': z = x * y; break;
        default:
            if (y == 0) return error("#div/0");
            z = x / y;
    }
    if (z - z != 0) return error("#num");          /* too big to hold */
    return number(z);
}

/* A reference as a formula has it: letters then digits, each part with a $
   before it when it stays put as the formula is copied ($A$1, A$1, $A1).
   Its length, or 0 when s does not start with one; in range is whether it
   names a cell on the sheet (one letter, a row from 1 to 200). */
typedef struct { int len, row, col, row_fixed, col_fixed, in_range; } ref_t;

static int ref_at(const char *s, ref_t *f) {
    int i = 0, l0, d0;
    memset(f, 0, sizeof(*f));
    if (s[i] == '$') { f->col_fixed = 1; i++; }
    l0 = i;
    while (is_letter(s[i])) i++;
    if (i == l0) return 0;
    int letters = i - l0;
    if (s[i] == '$') { f->row_fixed = 1; i++; }
    d0 = i;
    while (is_digit(s[i])) i++;
    if (i == d0) return 0;
    int n = 0;
    for (int k = d0; k < i && k < d0 + 6; k++) n = n * 10 + (s[k] - '0');
    f->len = i;
    f->col = upper(s[l0]) - 'A';
    f->row = n - 1;
    f->in_range = letters == 1 && i - d0 <= 4 && n >= 1 && n <= NROW;
    return i;
}

/* A cell's name, B3 in either case: its row and column, or 0 when it is past
   the sheet's edges. */
static int cell_named(const char *s, int letters, int digits, int *r, int *c) {
    if (letters != 1 || digits < 1 || digits > 4) return 0;
    int n = 0;
    for (int i = 1; i <= digits; i++) n = n * 10 + (s[i] - '0');
    if (n < 1 || n > NROW) return 0;
    *c = upper(s[0]) - 'A';
    *r = n - 1;
    return 1;
}

/* The length of a run of letters then digits at s, and of each part. */
static int word_at(const char *s, int *letters, int *digits) {
    int l = 0, d = 0;
    while (is_letter(s[l])) l++;
    while (is_digit(s[l + d])) d++;
    *letters = l;
    *digits = d;
    return l + d;
}

static val expr(parse_t *P);

/* A comparison, as spreadsheets make it: numbers by size, words by their
   letters with case aside, any number below any words, an empty cell as
   nought or as no words, whichever it is compared with. */
static int low_ch(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

static val compare(val a, const char *op, val b) {
    if (a.kind == V_ERR) return a;
    if (b.kind == V_ERR) return b;
    int c;
    if (a.kind == V_EMPTY) a = b.kind == V_TEXT ? (val){ V_TEXT, 0, "" } : number(0);
    if (b.kind == V_EMPTY) b = a.kind == V_TEXT ? (val){ V_TEXT, 0, "" } : number(0);
    if (a.kind == V_NUM && b.kind == V_NUM) c = a.num < b.num ? -1 : a.num > b.num ? 1 : 0;
    else if (a.kind == V_NUM) c = -1;
    else if (b.kind == V_NUM) c = 1;
    else {
        const char *x = a.str, *y = b.str;
        while (*x && low_ch((u8)*x) == low_ch((u8)*y)) { x++; y++; }
        c = low_ch((u8)*x) - low_ch((u8)*y);
        c = c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (op[0] == '=') return truth(c == 0);
    if (op[0] == '<' && op[1] == '>') return truth(c != 0);
    if (op[0] == '<' && op[1] == '=') return truth(c <= 0);
    if (op[0] == '>' && op[1] == '=') return truth(c >= 0);
    if (op[0] == '<') return truth(c < 0);
    return truth(c > 0);
}

/* Whether a value counts as true: a number not nought; words are not a
   truth (#value). */
static int truthy(val v, val *bad) {
    if (v.kind == V_ERR) { *bad = v; return -1; }
    if (v.kind == V_TEXT) { *bad = error("#value"); return -1; }
    return v.kind == V_NUM && v.num != 0;
}

/* Rounded half away from nought, to n places (n below nought rounds to
   tens, hundreds and so on). */
static double round_to(double x, int n) {
    double m = 1;
    for (int i = 0; i < (n < 0 ? -n : n) && i < 15; i++) m *= 10;
    double y = n >= 0 ? x * m : x / m;
    double r = y >= 0 ? (double)(long long)(y + 0.5) : -(double)(long long)(-y + 0.5);
    return n >= 0 ? r / m : r * m;
}

typedef struct { double sum, lo, hi; int nums; val err; int erred; } acc_t;

/* A value a function was given: numbers are counted, words and empty cells
   passed over, and the first error kept. */
static void take(acc_t *a, val v) {
    if (v.kind == V_ERR) { if (!a->erred) { a->err = v; a->erred = 1; } return; }
    if (v.kind != V_NUM) return;
    if (!a->nums || v.num < a->lo) a->lo = v.num;
    if (!a->nums || v.num > a->hi) a->hi = v.num;
    a->sum += v.num;
    a->nums++;
}

static int named(const char *s, int n, const char *name) {
    for (int i = 0; i < n; i++) if (upper(s[i]) != name[i]) return 0;
    return name[n] == 0;
}

/* IF, AND, OR, NOT, ROUND, ABS, LEN and the words' functions: each argument
   worked out whole. */
static val scalar(parse_t *P, const char *name, int len) {
    val arg[8];
    int n = 0;
    skip(P);
    if (*P->p != ')') {
        for (;;) {
            val v = expr(P);
            if (n < 8) arg[n++] = v;
            skip(P);
            if (*P->p == ',') { P->p++; continue; }
            if (*P->p == ')') break;
            return syntax(P);
        }
    }
    P->p++;
    val bad = error("#value");
    if (named(name, len, "IF")) {
        if (n < 2 || n > 3) return error("#value");
        int t = truthy(arg[0], &bad);
        if (t < 0) return bad;
        return t ? arg[1] : n == 3 ? arg[2] : truth(0);
    }
    if (named(name, len, "AND") || named(name, len, "OR")) {
        if (!n) return error("#value");
        int want_all = named(name, len, "AND"), all = 1, any = 0;
        for (int i = 0; i < n; i++) {
            int t = truthy(arg[i], &bad);
            if (t < 0) return bad;
            all = all && t;
            any = any || t;
        }
        return truth(want_all ? all : any);
    }
    if (named(name, len, "NOT")) {
        if (n != 1) return error("#value");
        int t = truthy(arg[0], &bad);
        return t < 0 ? bad : truth(!t);
    }
    if (named(name, len, "LEN")) {
        if (n != 1) return error("#value");
        if (arg[0].kind == V_ERR) return arg[0];
        char buf[40];
        return number(strlen(as_words(arg[0], buf)));
    }
    if (named(name, len, "ABS") || named(name, len, "ROUND")) {
        int round = named(name, len, "ROUND");
        if (n < 1 || n > (round ? 2 : 1)) return error("#value");
        for (int i = 0; i < n; i++) {
            if (arg[i].kind == V_ERR) return arg[i];
            if (arg[i].kind == V_TEXT) return error("#value");
        }
        double x = arg[0].kind == V_NUM ? arg[0].num : 0;
        if (!round) return number(x < 0 ? -x : x);
        double places = n == 2 && arg[1].kind == V_NUM ? arg[1].num : 0;
        return number(round_to(x, (int)places));
    }
    /* Words: UPPER, LOWER and TRIM (spaces at the ends gone, a run of them
       inside made one) of a value; LEFT and RIGHT of it, one letter or as
       many as asked; MID from a place counted from 1; FIND of some words in
       others, case counting as spreadsheets have it, their place from 1 or
       #value when they are not there. A number is taken as it is shown, and
       a count or place that is not a number, or below nought, is #value. */
    if (named(name, len, "UPPER") || named(name, len, "LOWER") || named(name, len, "TRIM")) {
        if (n != 1) return error("#value");
        if (arg[0].kind == V_ERR) return arg[0];
        char buf[40], out[TEXT_MAX];
        const char *w = as_words(arg[0], buf);
        int k = 0;
        if (named(name, len, "TRIM")) {
            for (int i = 0; w[i] && k < TEXT_MAX - 1; i++) {
                if (w[i] == ' ' && (k == 0 || out[k - 1] == ' ')) continue;
                out[k++] = w[i];
            }
            while (k && out[k - 1] == ' ') k--;
        } else {
            int up = named(name, len, "UPPER");
            for (int i = 0; w[i] && k < TEXT_MAX - 1; i++) {
                char ch = w[i];
                if (up && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                if (!up && ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
                out[k++] = ch;
            }
        }
        return words2(out, k, "", 0);
    }
    if (named(name, len, "LEFT") || named(name, len, "RIGHT") || named(name, len, "MID")) {
        int mid = named(name, len, "MID");
        if (mid ? n != 3 : (n < 1 || n > 2)) return error("#value");
        for (int i = 0; i < n; i++) if (arg[i].kind == V_ERR) return arg[i];
        for (int i = 1; i < n; i++) if (arg[i].kind != V_NUM || arg[i].num < 0) return error("#value");
        char buf[40];
        const char *w = as_words(arg[0], buf);
        int wl = strlen(w), from = 0, count;
        if (mid) {
            if (arg[1].num < 1) return error("#value");
            from = arg[1].num - 1 > wl ? wl : (int)arg[1].num - 1;
            count = arg[2].num > wl ? wl : (int)arg[2].num;
        } else {
            double c = n == 2 ? arg[1].num : 1;
            count = c > wl ? wl : (int)c;
            if (named(name, len, "RIGHT")) from = wl - count;
        }
        if (count > wl - from) count = wl - from;
        return words2(w + from, count, "", 0);
    }
    if (named(name, len, "FIND")) {
        if (n < 2 || n > 3) return error("#value");
        for (int i = 0; i < n; i++) if (arg[i].kind == V_ERR) return arg[i];
        if (n == 3 && (arg[2].kind != V_NUM || arg[2].num < 1)) return error("#value");
        char b1[40], b2[40];
        const char *needle = as_words(arg[0], b1), *hay = as_words(arg[1], b2);
        int nl = strlen(needle), hl = strlen(hay);
        if (n == 3 && arg[2].num - 1 > hl) return error("#value");
        for (int i = n == 3 ? (int)arg[2].num - 1 : 0; i + nl <= hl; i++)
            if (!strncmp(hay + i, needle, nl)) return number(i + 1);
        return error("#value");
    }
    return error("#name");
}

/* SUM, AVG (or AVERAGE), MIN, MAX and COUNT, of ranges (A1:B3) and of
   anything else, separated by commas. COUNT counts the numbers and passes
   errors over, as the others do not. */
static val call(parse_t *P, const char *name, int len) {
    int fn = named(name, len, "SUM") ? 0 : named(name, len, "AVG") || named(name, len, "AVERAGE") ? 1
           : named(name, len, "MIN") ? 2 : named(name, len, "MAX") ? 3 : named(name, len, "COUNT") ? 4 : -1;
    if (fn < 0) return scalar(P, name, len);
    acc_t a;
    memset(&a, 0, sizeof(a));
    skip(P);
    if (*P->p != ')') {
        for (;;) {
            skip(P);
            ref_t f1, f2;
            const char *s = P->p;
            int n1 = ref_at(s, &f1);
            const char *colon = s + n1;
            while (*colon == ' ') colon++;
            if (n1 && *colon == ':') {
                const char *t = colon + 1;
                while (*t == ' ') t++;
                int n2 = ref_at(t, &f2);
                if (!n2) return syntax(P);
                P->p = t + n2;
                int r1 = f1.row, c1 = f1.col, r2 = f2.row, c2 = f2.col;
                if (!f1.in_range || !f2.in_range) take(&a, error("#ref"));
                else {
                    if (r1 > r2) { int x = r1; r1 = r2; r2 = x; }
                    if (c1 > c2) { int x = c1; c1 = c2; c2 = x; }
                    for (int r = r1; r <= r2; r++)
                        for (int c = c1; c <= c2; c++) take(&a, cell_value(r, c));
                }
            } else {
                take(&a, expr(P));
            }
            skip(P);
            if (*P->p == ',') { P->p++; continue; }
            if (*P->p == ')') break;
            return syntax(P);
        }
    }
    P->p++;
    if (fn < 0) return error("#name");
    if (fn == 4) return number(a.nums);
    if (a.erred) return a.err;
    if (fn == 0) return number(a.sum);
    if (fn == 1) return a.nums ? number(a.sum / a.nums) : error("#div/0");
    if (!a.nums) return number(0);
    return number(fn == 2 ? a.lo : a.hi);
}

static val atom(parse_t *P) {
    skip(P);
    const char *s = P->p;
    if (*s == '(') {
        P->p++;
        val v = expr(P);
        skip(P);
        if (*P->p != ')') return syntax(P);
        P->p++;
        return v;
    }
    if (is_digit(*s) || (*s == '.' && is_digit(s[1]))) {
        double d;
        int n = read_number(s, &d);
        if (!n) return syntax(P);
        P->p += n;
        return number(d);
    }
    /* Words in quotes, a quote in them doubled. */
    if (*s == '"') {
        char buf[TEXT_MAX];
        int k = 0;
        const char *q = s + 1;
        for (;;) {
            if (!*q) return syntax(P);
            if (*q == '"') {
                if (q[1] != '"') break;
                q++;
            }
            if (k < TEXT_MAX - 1) buf[k++] = *q;
            q++;
        }
        P->p = q + 1;
        return words2(buf, k, "", 0);
    }
    /* What a reference copied off the sheet's edge was written as. */
    if (s[0] == '#' && upper(s[1]) == 'R' && upper(s[2]) == 'E' && upper(s[3]) == 'F' && !is_letter(s[4])) {
        P->p = s + 4;
        return error("#ref");
    }
    ref_t f;
    int n = ref_at(s, &f);
    if (n) {
        P->p = s + n;
        return f.in_range ? cell_value(f.row, f.col) : error("#ref");
    }
    if (is_letter(*s)) {
        int l = 0;
        while (is_letter(s[l])) l++;
        P->p = s + l;
        skip(P);
        if (*P->p != '(' && named(s, l, "TRUE")) return truth(1);
        if (*P->p != '(' && named(s, l, "FALSE")) return truth(0);
        if (*P->p != '(') return error("#name");
        P->p++;
        return call(P, s, l);
    }
    return syntax(P);
}

static val unary(parse_t *P) {
    skip(P);
    if (*P->p == '-' || *P->p == '+') {
        char op = *P->p++;
        val v = unary(P);
        return arith(number(0), op, v);
    }
    return atom(P);
}

static val term(parse_t *P) {
    val v = unary(P);
    for (;;) {
        skip(P);
        char op = *P->p;
        if (op != '*' && op != '/') return v;
        P->p++;
        v = arith(v, op, unary(P));
    }
}

static val sum_expr(parse_t *P) {
    val v = term(P);
    for (;;) {
        skip(P);
        char op = *P->p;
        if (op != '+' && op != '-') return v;
        P->p++;
        v = arith(v, op, term(P));
    }
}

/* & joins two values' words, below + and - as in other spreadsheets:
   =1+2&3 is 33. */
static val join_expr(parse_t *P) {
    val v = sum_expr(P);
    for (;;) {
        skip(P);
        if (*P->p != '&') return v;
        P->p++;
        val w = sum_expr(P);
        if (v.kind == V_ERR) continue;
        if (w.kind == V_ERR) { v = w; continue; }
        char ba[40], bb[40];
        const char *a = as_words(v, ba), *b = as_words(w, bb);
        v = words2(a, strlen(a), b, strlen(b));
    }
}

/* Comparisons are the lowest of all: =A1+1>B1 compares a sum. */
static val expr(parse_t *P) {
    val v = join_expr(P);
    for (;;) {
        skip(P);
        const char *p = P->p;
        char op[3] = { 0, 0, 0 };
        if (p[0] == '<' && (p[1] == '=' || p[1] == '>')) { op[0] = p[0]; op[1] = p[1]; }
        else if (p[0] == '>' && p[1] == '=') { op[0] = '>'; op[1] = '='; }
        else if (p[0] == '<' || p[0] == '>' || p[0] == '=') op[0] = p[0];
        else return v;
        P->p += op[1] ? 2 : 1;
        v = compare(v, op, join_expr(P));
    }
}

/* A formula, after its =. What does not read as one is #error, whatever
   else it would have said; a formula of an empty cell is nought. */
static val formula(const char *src) {
    parse_t P = { src, 0 };
    val v = expr(&P);
    skip(&P);
    if (*P.p) P.bad = 1;
    if (P.bad) return error("#error");
    if (v.kind == V_EMPTY) return number(0);
    return v;
}

static val cell_value(int r, int c) {
    cell_t *x = &cells[r][c];
    if (x->pass == pass_no) return x->v;
    if (x->busy) return error("#cycle");
    val v;
    if (x->text[0] == '=') {
        x->busy = 1;
        v = formula(x->text + 1);
        x->busy = 0;
    } else {
        v = plain(x->text);
    }
    x->v = v;
    x->pass = pass_no;
    return v;
}

static void recalc(void) {
    pass_no++;
    pool_n = 0;
    for (int r = 0; r < NROW; r++)
        for (int c = 0; c < NCOL; c++) cell_value(r, c);
}

/* A formula as it reads copied dr rows down and dc columns along: each
   reference moves with it but for a part with a $ before it, and one moved
   off the sheet is written #ref, as it then says. Words and numbers are
   copied as they are. */
static void moved(const char *src, int dr, int dc, char *out, int cap) {
    int n = 0;
    if (src[0] != '=') { strncpy(out, src, cap - 1); out[cap - 1] = 0; return; }
    for (int i = 0; src[i] && n < cap - 1; ) {
        if (src[i] == '"') {
            out[n++] = src[i++];
            while (src[i] && n < cap - 1) {
                char ch = src[i++];
                out[n++] = ch;
                if (ch != '"') continue;
                if (src[i] != '"') break;
                if (n < cap - 1) out[n++] = src[i++];
            }
            continue;
        }
        ref_t f;
        int after_word = i > 0 && (is_letter(src[i - 1]) || is_digit(src[i - 1]) || src[i - 1] == '.'
                                   || src[i - 1] == '#');
        if (!after_word && ref_at(src + i, &f) && f.in_range) {
            int r = f.row + (f.row_fixed ? 0 : dr), c = f.col + (f.col_fixed ? 0 : dc);
            char ref[16];
            int k = 0;
            if (r < 0 || r >= NROW || c < 0 || c >= NCOL) {
                for (const char *p = "#ref"; *p; p++) ref[k++] = *p;
            } else {
                if (f.col_fixed) ref[k++] = '$';
                ref[k++] = (char)('A' + c);
                if (f.row_fixed) ref[k++] = '$';
                k += utoa((u32)(r + 1), ref + k);
            }
            for (int j = 0; j < k && n < cap - 1; j++) out[n++] = ref[j];
            i += f.len;
            continue;
        }
        out[n++] = src[i++];
    }
    out[n] = 0;
}

/* What a cell shows. */
static void shown_text(const cell_t *x, char *out) {
    switch (x->v.kind) {
        case V_NUM:  if (x->v.str) strcpy(out, x->v.str); else num_text(x->v.num, out); break;
        case V_TEXT: strcpy(out, x->v.str); break;
        case V_ERR:  strcpy(out, x->v.str); break;
        default:     out[0] = 0;
    }
}

static void cell_name(int r, int c, char *out) {
    out[0] = (char)('A' + c);
    out[1 + utoa((u32)(r + 1), out + 1)] = 0;
}

/* --- files ---------------------------------------------------------------- */

static char path[PATH_MAX];
static char note[160];          /* the last thing said, on the status bar */
static int changed;

static void tell(const char *what, const char *p, int rows) {
    int n = 0;
    for (const char *s = what; *s; s++) note[n++] = *s;
    for (const char *s = p; *s && n < 140; s++) note[n++] = *s;
    if (rows >= 0) {
        for (const char *s = ", "; *s; s++) note[n++] = *s;
        n += utoa((u32)rows, note + n);
        for (const char *s = rows == 1 ? " row" : " rows"; *s; s++) note[n++] = *s;
    }
    note[n] = 0;
    puts("sheet: ");
    puts(note);
    puts("\n");
}

/* Comma separated, a line a row, each row up to its last cell with anything
   in it. A field with a comma or a quote in it, or spaces at its ends, is
   quoted, its quotes doubled. Formulas are kept as they were typed. */
static void save(void) {
    static char out[NROW * NCOL * (TEXT_MAX * 2 + 3) + 16];
    int last = -1;
    for (int r = 0; r < NROW; r++)
        for (int c = 0; c < NCOL; c++) if (cells[r][c].text[0]) last = r;
    int n = 0;
    for (int r = 0; r <= last; r++) {
        int end = -1;
        for (int c = 0; c < NCOL; c++) if (cells[r][c].text[0]) end = c;
        for (int c = 0; c <= end; c++) {
            const char *t = cells[r][c].text;
            if (c) out[n++] = ',';
            int len = strlen(t), quote = len && (t[0] == ' ' || t[len - 1] == ' ');
            for (int i = 0; i < len; i++) if (t[i] == ',' || t[i] == '"') quote = 1;
            if (quote) out[n++] = '"';
            for (int i = 0; i < len; i++) {
                if (t[i] == '"') out[n++] = '"';
                out[n++] = t[i];
            }
            if (quote) out[n++] = '"';
        }
        out[n++] = '\n';
    }
    if (spit(path, out, n) != n) { tell("could not save ", path, -1); return; }
    changed = 0;
    tell("saved ", path, last + 1);
}

/* CSV as anything writes it: quoted fields with doubled quotes, line ends of
   either kind. A line break inside quotes is a space here, since a cell is
   one line. What does not fit the sheet is left out. */
static void load(const char *p) {
    static char in[1 << 20];
    strncpy(path, p, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    memset(cells, 0, sizeof(cells));
    changed = 0;
    int n = slurp(path, in, (int)sizeof(in));
    if (n < 0) { recalc(); tell("new ", path, -1); return; }
    int r = 0, c = 0, i = 0;
    while (i < n && r < NROW) {
        char field[TEXT_MAX];
        int k = 0;
        if (in[i] == '"') {
            i++;
            while (i < n) {
                char ch = in[i];
                if (ch == '"') {
                    if (i + 1 < n && in[i + 1] == '"') { i += 2; if (k < TEXT_MAX - 1) field[k++] = '"'; continue; }
                    i++;
                    break;
                }
                if (ch == '\n' || ch == '\r') ch = ' ';
                if (k < TEXT_MAX - 1) field[k++] = ch;
                i++;
            }
            while (i < n && in[i] != ',' && in[i] != '\n' && in[i] != '\r') i++;
        } else {
            while (i < n && in[i] != ',' && in[i] != '\n' && in[i] != '\r') {
                if (k < TEXT_MAX - 1) field[k++] = in[i];
                i++;
            }
        }
        field[k] = 0;
        if (c < NCOL) strcpy(cells[r][c].text, field);
        if (i < n && in[i] == ',') { i++; c++; continue; }
        if (i < n && in[i] == '\r') i++;
        if (i < n && in[i] == '\n') i++;
        r++;
        c = 0;
    }
    recalc();
    tell("opened ", path, r);
}

/* --- the window ----------------------------------------------------------- */

enum { M_GRID, M_EDIT, M_GOTO };

/* ctrl+O: another sheet, chosen from a list of the folder's .csv files (or
   a path typed into it). */
static ui_chooser chooser;

static int mode = M_GRID;
static int row, col, top, left;
static int tab_from = -1;       /* the column a row of Tabs began in, for Return */
static char copied[TEXT_MAX];   /* what ctrl+C took, and from where, so a paste can move it */
static int copied_r = -1, copied_c;
static char edit_buf[TEXT_MAX];
static char ask_buf[PATH_MAX];
static ui_field edit, ask;

static void field_set(ui_field *f, const char *text) {
    strncpy(f->buf, text, f->cap - 1);
    f->buf[f->cap - 1] = 0;
    f->len = strlen(f->buf);
    f->cursor = f->len;
}

static void go(int r, int c) {
    if (r < 0) r = 0;
    if (r >= NROW) r = NROW - 1;
    if (c < 0) c = 0;
    if (c >= NCOL) c = NCOL - 1;
    row = r;
    col = c;
}

static void begin_edit(int keep) {
    mode = M_EDIT;
    field_set(&edit, keep ? cells[row][col].text : "");
    edit.focused = 1;
}

static void commit(void) {
    if (strcmp(cells[row][col].text, edit_buf)) {
        strcpy(cells[row][col].text, edit_buf);
        changed = 1;
        recalc();
    }
    mode = M_GRID;
    edit.focused = 0;
}

static void set_text(const char *t) {
    strncpy(cells[row][col].text, t, TEXT_MAX - 1);
    cells[row][col].text[TEXT_MAX - 1] = 0;
    changed = 1;
    recalc();
}

/* --- sorting ---------------------------------------------------------------
 *
 * ctrl+T puts the rows from the chosen one down to the last with anything in
 * it in order by the chosen column: numbers smallest first, then words A to
 * Z with case aside, then truths, then errors, and empty cells last whichever
 * way it goes. ctrl+T again on the same column from the same row goes the
 * other way. Rows that tie keep
 * their order. Each row is moved whole, its formulas as a copy of it would
 * be (moved), so =B2*C2 moved to row 5 is =B5*C5; one that names another row
 * names whatever is there afterwards, as in other spreadsheets. */
static int sort_col = -1, sort_row, sort_desc;

static int sort_class(const val *v) {
    switch (v->kind) {
        case V_NUM:  return v->str ? 2 : 0;
        case V_TEXT: return 1;
        case V_ERR:  return 3;
        default:     return 4;
    }
}

static int sort_cmp(const val *a, const val *b, int desc) {
    int ca = sort_class(a), cb = sort_class(b), c;
    if (ca == 4 || cb == 4) return ca == cb ? 0 : ca == 4 ? 1 : -1;
    if (ca != cb) c = ca < cb ? -1 : 1;
    else if (ca == 1) {
        const char *x = a->str, *y = b->str;
        while (*x && low_ch((u8)*x) == low_ch((u8)*y)) { x++; y++; }
        c = low_ch((u8)*x) - low_ch((u8)*y);
        c = c < 0 ? -1 : c > 0;
    } else if (ca == 3) c = 0;
    else c = a->num < b->num ? -1 : a->num > b->num;
    return desc ? -c : c;
}

static void tell(const char *what, const char *p, int rows);
static void cell_name(int r, int c, char *out);

static void sort_rows(int row, int col) {
    int last = -1;
    for (int r = 0; r < NROW; r++)
        for (int c = 0; c < NCOL; c++) if (cells[r][c].text[0]) last = r;
    if (last <= row) { tell("nothing below to sort", "", -1); sort_col = -1; return; }
    int desc = sort_col == col && sort_row == row ? !sort_desc : 0;
    int n = last - row + 1;
    static int order[NROW];
    for (int i = 0; i < n; i++) order[i] = row + i;
    for (int i = 1; i < n; i++) {
        int x = order[i], j = i;
        while (j > 0 && sort_cmp(&cells[order[j - 1]][col].v, &cells[x][col].v, desc) > 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = x;
    }
    static char texts[NROW][NCOL][TEXT_MAX];
    for (int i = 0; i < n; i++)
        for (int c = 0; c < NCOL; c++) moved(cells[order[i]][c].text, row + i - order[i], 0, texts[i][c], TEXT_MAX);
    for (int i = 0; i < n; i++)
        for (int c = 0; c < NCOL; c++) strcpy(cells[row + i][c].text, texts[i][c]);
    changed = 1;
    recalc();
    sort_col = col;
    sort_row = row;
    sort_desc = desc;

    char line[96], name[8];
    int k = 0;
    for (const char *p = "rows "; *p; p++) line[k++] = *p;
    k += utoa((u32)(row + 1), line + k);
    for (const char *p = " to "; *p; p++) line[k++] = *p;
    k += utoa((u32)(last + 1), line + k);
    for (const char *p = " by "; *p; p++) line[k++] = *p;
    cell_name(0, col, name);
    line[k++] = name[0];
    for (const char *p = desc ? ", largest first" : ", smallest first"; *p; p++) line[k++] = *p;
    line[k] = 0;
    tell("sorted ", line, -1);
}

/* A ctrl chord, from the keyboard or as the control character a serial line
   sends for it. */
static int chord(u32 k, char letter) {
    if (KEY_CTRL(k)) return key_ctrl_letter(k) == letter;
    return KEY_CODE(k) == (u32)(letter - 'a' + 1);
}

static void return_key(void) {
    go(row + 1, tab_from >= 0 ? tab_from : col);
    tab_from = -1;
}

static void ask_for(int m) {
    mode = m;
    field_set(&ask, "");
    ask.focused = 1;
}

static void answer(void) {
    if (mode == M_GOTO) {
        int l, d, r, c;
        const char *s = ask_buf;
        while (*s == ' ') s++;
        int n = word_at(s, &l, &d);
        if (n && !s[n] && cell_named(s, l, d, &r, &c)) { go(r, c); tab_from = -1; }
        else tell("there is no cell ", ask_buf, -1);
    }
    mode = M_GRID;
    ask.focused = 0;
}

static void grid_key(u32 k) {
    u32 code = KEY_CODE(k);
    if (code == KEY_UP) { go(row - 1, col); tab_from = -1; }
    else if (code == KEY_DOWN) { go(row + 1, col); tab_from = -1; }
    else if (code == KEY_LEFT) { go(row, col - 1); tab_from = -1; }
    else if (code == KEY_RIGHT) { go(row, col + 1); tab_from = -1; }
    else if (code == KEY_PAGE_UP) go(row - 10, col);
    else if (code == KEY_PAGE_DOWN) go(row + 10, col);
    else if (code == KEY_HOME) go(KEY_CTRL(k) ? 0 : row, 0);
    else if (k == '\t') { if (tab_from < 0) tab_from = col; go(row, col + 1); }
    else if (k == '\n') return_key();
    else if (code == KEY_DELETE || k == '\b') { if (cells[row][col].text[0]) set_text(""); }
    else if (code == KEY_F1 + 1) begin_edit(1);
    else if (chord(k, 's')) save();
    else if (chord(k, 'g')) ask_for(M_GOTO);
    else if (chord(k, 't')) sort_rows(row, col);
    else if (chord(k, 'o')) ui_chooser_open_on(&chooser, path, ".csv", "sheet");
    else if (chord(k, 'c') || chord(k, 'x')) {
        clip_set(cells[row][col].text, strlen(cells[row][col].text));
        strcpy(copied, cells[row][col].text);
        copied_r = row;
        copied_c = col;
        if (chord(k, 'x')) { set_text(""); copied_r = -1; }
    } else if (chord(k, 'v')) {
        /* What this sheet copied moves its references to where it lands; what
           came from anywhere else goes in as it is. */
        char got[TEXT_MAX], put[TEXT_MAX];
        int n = clip_get(got, (int)sizeof(got) - 1);
        if (n < 0) n = 0;
        got[n] = 0;
        for (int i = 0; i < n; i++) if (got[i] == '\n' || got[i] == '\r') { got[i] = 0; break; }
        if (copied_r >= 0 && !strcmp(got, copied)) moved(got, row - copied_r, col - copied_c, put, sizeof(put));
        else strcpy(put, got);
        set_text(put);
    } else if (chord(k, 'd') && row > 0) {
        char put[TEXT_MAX];
        moved(cells[row - 1][col].text, 1, 0, put, sizeof(put));
        set_text(put);
    } else if (chord(k, 'r') && col > 0) {
        char put[TEXT_MAX];
        moved(cells[row][col - 1].text, 0, 1, put, sizeof(put));
        set_text(put);
    } else if (!KEY_CTRL(k) && !KEY_IS_SPECIAL(k) && k >= 32 && k < 127) {
        begin_edit(0);
        ui_field_key(&edit, k);
    }
}

static void edit_key(u32 k) {
    u32 code = KEY_CODE(k);
    if (k == '\n') { commit(); return_key(); }
    else if (k == '\t') { commit(); if (tab_from < 0) tab_from = col; go(row, col + 1); }
    else if (k == 27) { mode = M_GRID; edit.focused = 0; }
    else if (code == KEY_UP) { commit(); go(row - 1, col); tab_from = -1; }
    else if (code == KEY_DOWN) { commit(); go(row + 1, col); tab_from = -1; }
    else ui_field_key(&edit, k);
}

static void ask_key(u32 k) {
    if (k == '\n') answer();
    else if (k == 27) { mode = M_GRID; ask.focused = 0; }
    else ui_field_key(&ask, k);
}

/* What is chosen, on the console when it changes. */
static void say_chosen(void) {
    static char said[TEXT_MAX + 160];
    char line[TEXT_MAX + 160], name[8], shown[TEXT_MAX + 32];
    const cell_t *x = &cells[row][col];
    cell_name(row, col, name);
    int n = 0;
    for (const char *s = name; *s; s++) line[n++] = *s;
    line[n++] = ' ';
    if (!x->text[0]) {
        for (const char *s = "empty"; *s; s++) line[n++] = *s;
    } else {
        for (const char *s = x->text; *s; s++) line[n++] = *s;
        if (x->text[0] == '=') {
            shown_text(x, shown);
            for (const char *s = " is "; *s; s++) line[n++] = *s;
            for (const char *s = shown; *s; s++) line[n++] = *s;
        }
    }
    line[n] = 0;
    if (strcmp(line, said)) {
        strcpy(said, line);
        puts("sheet: ");
        puts(line);
        puts("\n");
    }
}

int main(int argc, char **argv) {
    int win = win_create("Sheet", 780, 520);
    if (win < 0) exit(1);
    win_allow_resize(win);
    win_want_escape(win);               /* it lets an edit go */

    memset(&edit, 0, sizeof(edit));
    edit.buf = edit_buf;
    edit.cap = sizeof(edit_buf);
    memset(&ask, 0, sizeof(ask));
    ask.buf = ask_buf;
    ask.cap = sizeof(ask_buf);

    load(argc > 1 && argv[1][0] ? argv[1] : "/home/sheet.csv");

    ui_input in;
    memset(&in, 0, sizeof(in));
    int follow = 1;

    for (;;) {
        int w = win_width(win), h = win_height(win);
        u32 *px = win_surface(win);
        if (!px || w <= 0 || h <= 0) break;
        surface s = { px, w, h };
        ui_theme t = ui_load_theme();

        ui_begin(&in);
        win_event ev;
        int closing = 0;
        while (win_poll(win, &ev)) {
            if (ev.type == WIN_EV_CLOSE) { closing = 1; break; }
            ui_feed(&in, &ev);
        }
        if (closing) break;

        for (int ki = 0; ki < in.nkeys; ki++) {
            u32 k = in.keys[ki];
            if (mode == M_EDIT) edit_key(k);
            else if (chooser.open) {
                if (ui_chooser_key(&chooser, k) == 1) { load(chooser.picked); go(0, 0); }
            }
            else if (mode == M_GOTO) ask_key(k);
            else grid_key(k);
            follow = 1;
        }

        int gy = UI_PAD + UI_BTN_H + UI_PAD;           /* the column letters */
        int gh = h - UI_ROW - gy;
        int vis_r = gh / CELL_H - 1, vis_c = (w - HEAD_W) / CELL_W;
        if (vis_r < 1) vis_r = 1;
        if (vis_c < 1) vis_c = 1;

        /* A click on a cell chooses it; one on the bar edits it. */
        if (!chooser.open && in.pressed && in.my >= gy + CELL_H && in.my < gy + gh && in.mx >= HEAD_W) {
            int r = top + (in.my - gy - CELL_H) / CELL_H, c = left + (in.mx - HEAD_W) / CELL_W;
            if (mode == M_EDIT) commit();
            mode = M_GRID;
            ask.focused = 0;
            go(r, c);
            tab_from = -1;
            follow = 1;
        }
        if (in.scroll) {
            top += in.scroll * 3;
            follow = 0;
        }
        if (follow) {
            if (row < top) top = row;
            if (row >= top + vis_r) top = row - vis_r + 1;
            if (col < left) left = col;
            if (col >= left + vis_c) left = col - vis_c + 1;
        }
        if (top > NROW - vis_r) top = NROW - vis_r;
        if (top < 0) top = 0;
        if (left > NCOL - vis_c) left = NCOL - vis_c;
        if (left < 0) left = 0;

        fill(&s, t.bg);

        /* The bar: which cell, and what is in it. */
        char name[8];
        cell_name(row, col, name);
        const char *box = mode == M_GOTO ? "go to" : name;
        rect(&s, UI_PAD, UI_PAD, NAME_W, UI_BTN_H, t.well);
        ui_sunken(&s, &t, UI_PAD, UI_PAD, NAME_W, UI_BTN_H);
        face_draw(&s, UI_PAD + (NAME_W - face_w(box, UI_FACE_BOLD)) / 2,
                  UI_PAD + (UI_BTN_H - face_h(UI_FACE_BOLD)) / 2, box, t.fg, UI_FACE_BOLD);
        int fx = UI_PAD * 2 + NAME_W, fw = w - fx - UI_PAD;
        if (mode == M_GOTO) {
            ui_field_draw(&s, &in, &t, fx, UI_PAD, fw, &ask,
                          "a cell, as B3");
        } else {
            if (mode == M_GRID) field_set(&edit, cells[row][col].text);
            int was = edit.focused;
            if (ui_field_draw(&s, &in, &t, fx, UI_PAD, fw, &edit, "a number, words, or = and a formula")
                && mode == M_GRID) {
                begin_edit(1);
            } else if (mode == M_GRID) {
                edit.focused = 0;
            } else if (was && !edit.focused) {
                edit.focused = 1;                      /* a click elsewhere is handled above */
            }
        }

        /* The grid: letters along the top, numbers down the side. */
        rect(&s, 0, gy, w, CELL_H, t.panel);
        rect(&s, 0, gy, HEAD_W, gh, t.panel);
        for (int c = 0; c < vis_c && left + c < NCOL; c++) {
            int x = HEAD_W + c * CELL_W;
            char l[2] = { (char)('A' + left + c), 0 };
            if (left + c == col) rect(&s, x, gy, CELL_W, CELL_H, t.soft);
            face_draw(&s, x + (CELL_W - face_w(l, UI_FACE_BODY)) / 2, gy + (CELL_H - face_h(UI_FACE_BODY)) / 2,
                      l, t.dim, UI_FACE_BODY);
        }
        for (int r = 0; r < vis_r && top + r < NROW; r++) {
            int y = gy + CELL_H + r * CELL_H;
            char num[8];
            num[utoa((u32)(top + r + 1), num)] = 0;
            if (top + r == row) rect(&s, 0, y, HEAD_W, CELL_H, t.soft);
            face_draw(&s, HEAD_W - 8 - face_w(num, UI_FACE_BODY), y + (CELL_H - face_h(UI_FACE_BODY)) / 2,
                      num, t.dim, UI_FACE_BODY);
            for (int c = 0; c < vis_c && left + c < NCOL; c++) {
                int x = HEAD_W + c * CELL_W;
                const cell_t *cell = &cells[top + r][left + c];
                rect(&s, x, y, CELL_W, CELL_H, t.well);
                rect(&s, x + CELL_W - 1, y, 1, CELL_H, t.line);
                rect(&s, x, y + CELL_H - 1, CELL_W, 1, t.line);
                if (cell->v.kind == V_EMPTY) continue;
                char shown[TEXT_MAX + 32];
                shown_text(cell, shown);
                int tw = face_w(shown, UI_FACE_BODY);
                int tx = cell->v.kind == V_NUM || cell->v.kind == V_ERR ? x + CELL_W - 6 - tw : x + 5;
                if (tx < x + 5) tx = x + 5;
                face_draw_clip(&s, tx, y + (CELL_H - face_h(UI_FACE_BODY)) / 2, x + 2, x + CELL_W - 3, shown,
                               cell->v.kind == V_ERR ? t.warn : t.fg, UI_FACE_BODY);
            }
        }
        if (row >= top && row < top + vis_r && col >= left && col < left + vis_c) {
            int x = HEAD_W + (col - left) * CELL_W, y = gy + CELL_H + (row - top) * CELL_H;
            rect(&s, x - 1, y - 1, CELL_W + 1, 2, t.accent);
            rect(&s, x - 1, y + CELL_H - 2, CELL_W + 1, 2, t.accent);
            rect(&s, x - 1, y - 1, 2, CELL_H + 1, t.accent);
            rect(&s, x + CELL_W - 2, y - 1, 2, CELL_H + 1, t.accent);
        }

        /* What was last said until the sheet is changed, then where it is
           kept and that it has changed since. */
        static char status[PATH_MAX + 40];
        if (note[0] && !changed) strcpy(status, note);
        else strcpy(status, path);
        if (changed) {
            int n = strlen(status);
            for (const char *p2 = " (changed)"; *p2 && n < (int)sizeof(status) - 1; p2++) status[n++] = *p2;
            status[n] = 0;
        }
        if (ui_chooser_draw(&s, &in, &t, &chooser, 0, gy, w, gh)) { load(chooser.picked); go(0, 0); }
        ui_statusbar(&s, &t, w, h, status, "ctrl+S keeps it");
        say_chosen();

        win_commit(win);
        ui_wait(win);
    }

    win_close(win);
    exit(0);
}
