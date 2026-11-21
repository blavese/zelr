/* A calculator.
 *
 * Everything here is a sixty four bit integer of millionths, so that 0.1 and
 * 0.2 make 0.3 as they do on paper, which floating point does not. (This was
 * written when a program could not use floating point at all; ring 3 can now,
 * and Sheet does, but money and a calculator want decimals that are exact.)
 * Six decimal places is more than a calculator this size needs and leaves
 * room for the multiply, which is the one operation that has to hold twice as
 * many digits as it started with.
 *
 * It keeps a number aside (M+ adds what is shown to it, MR shows it, MC
 * clears it) and takes square roots, and says each sum it works out on the
 * console ("calc: 78 / 4 = 19.5"), which is its tape and what calccheck
 * reads.
 *
 * What is on screen is a string being typed, not a number being edited: that
 * is the difference between a calculator that lets you type 1.50 and one that
 * shows you 1.5 while you are still typing the zero.
 */
#include "zelr.h"
#include "ui.h"

#define SCALE     1000000LL       /* millionths */
#define ENTRY_MAX 24

static char  entry[ENTRY_MAX + 1] = "0";
static int   entry_len = 1;
static int   typing = 1;          /* the display is a string being typed */

static i64   acc;                 /* what is waiting for an operator */
static char  pending;             /* 0, or one of + - x / */
static int   error;
static i64   memory;              /* M+ adds to it, MR shows it, MC clears it */

/* --- numbers as text ------------------------------------------------------ */

static i64 parse_entry(void) {
    i64 whole = 0, frac = 0, scale = SCALE;
    int at = 0, neg = 0;

    if (entry[at] == '-') { neg = 1; at++; }
    for (; entry[at] && entry[at] != '.'; at++)
        whole = whole * 10 + (entry[at] - '0');

    if (entry[at] == '.') {
        at++;
        for (; entry[at] && scale > 1; at++) {
            scale /= 10;
            frac += (i64)(entry[at] - '0') * scale;
        }
    }
    i64 v = whole * SCALE + frac;
    return neg ? -v : v;
}

/* Trailing zeros are dropped, because 2 / 4 is 0.5 and not 0.500000.
 *
 * Writing into a buffer the caller names rather than into the display. The
 * first version of this only knew how to write into the display, so drawing
 * the pending operation had to call it, keep a copy of what was there and
 * put it back afterwards. It put the string back and left the flag that says
 * whether the display is being typed into, which meant the digit after an
 * operator was appended to the first number instead of starting the second:
 * 78 / 4 came out as 78 / 784. */
static void format(i64 v, char *out, int cap) {
    int n = 0;
    (void)cap;

    if (v < 0) { out[n++] = '-'; v = -v; }

    i64 whole = v / SCALE;
    i64 frac = v % SCALE;

    char digits[24];
    int d = 0;
    if (!whole) digits[d++] = '0';
    while (whole && d < 20) { digits[d++] = (char)('0' + (int)(whole % 10)); whole /= 10; }
    while (d) out[n++] = digits[--d];

    if (frac) {
        char f[8];
        for (int i = 5; i >= 0; i--) { f[i] = (char)('0' + (int)(frac % 10)); frac /= 10; }
        int last = 5;
        while (last >= 0 && f[last] == '0') last--;
        if (last >= 0) {
            out[n++] = '.';
            for (int i = 0; i <= last && n < ENTRY_MAX; i++) out[n++] = f[i];
        }
    }

    out[n] = 0;
}

static void show(i64 v) {
    format(v, entry, ENTRY_MAX + 1);
    entry_len = 0;
    while (entry[entry_len]) entry_len++;
    typing = 0;
}

/* --- the tape --------------------------------------------------------------- */

/* "calc: " and the words and numbers given, on the console. */
static void tape(const char *a, i64 x, const char *b, i64 y, const char *c, i64 z, int nums) {
    char line[160], num[ENTRY_MAX + 8];
    int n = 0;
    const char *words[3] = { a, b, c };
    i64 vals[3] = { x, y, z };
    for (const char *p = "calc: "; *p; p++) line[n++] = *p;
    for (int i = 0; i < 3; i++) {
        if (words[i]) for (const char *p = words[i]; *p && n < 140; p++) line[n++] = *p;
        if (i < nums) {
            format(vals[i], num, sizeof(num));
            for (const char *p = num; *p && n < 150; p++) line[n++] = *p;
        }
    }
    line[n++] = '\n';
    line[n] = 0;
    puts(line);
}

/* --- the arithmetic ------------------------------------------------------- */

static void apply(void) {
    i64 rhs = parse_entry();
    i64 v = acc;
    i64 was = acc;
    char op[4] = { ' ', pending, ' ', 0 };

    switch (pending) {
        case '+': v = acc + rhs; break;
        case '-': v = acc - rhs; break;
        case 'x': v = (acc * rhs) / SCALE; break;
        case '/':
            if (!rhs) { error = 1; tape("", was, op, rhs, " = cannot", 0, 2); return; }
            v = (acc * SCALE) / rhs;
            break;
        default:  v = rhs; break;
    }
    acc = v;
    show(v);
    if (pending) tape("", was, op, rhs, " = ", v, 3);
}

static void digit(char c) {
    if (error) return;
    if (!typing) { entry_len = 0; entry[0] = 0; typing = 1; }
    if (entry_len >= ENTRY_MAX) return;

    /* One leading zero, and no second point. */
    if (c == '.') {
        for (int i = 0; i < entry_len; i++) if (entry[i] == '.') return;
        if (!entry_len) entry[entry_len++] = '0';
    } else if (entry_len == 1 && entry[0] == '0') {
        entry_len = 0;
    }

    entry[entry_len++] = c;
    entry[entry_len] = 0;
}

static void operator(char op) {
    if (error) return;
    if (pending && typing) apply();
    else                   acc = parse_entry();
    pending = op;
    typing = 0;
}

static void equals(void) {
    if (error) return;
    if (!pending) { acc = parse_entry(); return; }
    apply();
    pending = 0;
}

static void clear_all(void) {
    entry[0] = '0'; entry[1] = 0;
    entry_len = 1;
    typing = 1;
    acc = 0;
    pending = 0;
    error = 0;
}

static void backspace(void) {
    if (error || !typing) return;
    if (entry_len > 1) entry[--entry_len] = 0;
    else { entry[0] = '0'; entry[1] = 0; entry_len = 1; }
}

static void negate(void) {
    if (error) return;
    if (entry[0] == '-') {
        for (int i = 0; i <= entry_len; i++) entry[i] = entry[i + 1];
        entry_len--;
    } else if (entry_len < ENTRY_MAX) {
        for (int i = entry_len + 1; i > 0; i--) entry[i] = entry[i - 1];
        entry[0] = '-';
        entry_len++;
    }
}

static void percent(void) {
    if (error) return;
    show(parse_entry() / 100);
}

/* The square root of what is shown, to the millionth below it: the whole
   root of the number times a million, in 128 bits so that numbers up to the
   most this holds have one. Below nought it cannot. */
static void square_root(void) {
    if (error) return;
    i64 v = parse_entry();
    if (v < 0) { error = 1; tape("sqrt ", v, " cannot", 0, 0, 0, 1); return; }
    unsigned __int128 n = (unsigned __int128)(u64)v * (u64)SCALE, lo = 0, hi = (u64)3037000500u * 1000u;
    while (lo < hi) {                     /* the most r with r * r <= n */
        unsigned __int128 mid = (lo + hi + 1) / 2;
        if (mid * mid <= n) lo = mid; else hi = mid - 1;
    }
    show((i64)lo);
    tape("sqrt ", v, " = ", (i64)lo, 0, 0, 2);
}

static void memory_add(void) {
    if (error) return;
    memory += parse_entry();
    typing = 0;                           /* the next digit starts a new number */
    tape("memory ", memory, 0, 0, 0, 0, 1);
}

static void memory_recall(void) {
    if (error) return;
    show(memory);
}

static void memory_clear(void) {
    memory = 0;
    tape("memory ", 0, 0, 0, 0, 0, 1);
}

/* --- the keys ------------------------------------------------------------- */

typedef struct { const char *label; char key; int wide; } key_t;

/* Six rows of four: what is kept aside and the square root, then the
   numbers and what is done with them. The wide one is the zero, which is what
   the extra space in a grid like this is always spent on. */
static const key_t KEYS[] = {
    { "MC", 'L', 0 }, { "MR", 'M', 0 }, { "M+", 'm', 0 }, { "sqrt", 'r', 0 },
    { "C", 'C', 0 }, { "+/-", '~', 0 }, { "%", '%', 0 }, { "/", '/', 0 },
    { "7", '7', 0 }, { "8", '8', 0 },   { "9", '9', 0 }, { "x", 'x', 0 },
    { "4", '4', 0 }, { "5", '5', 0 },   { "6", '6', 0 }, { "-", '-', 0 },
    { "1", '1', 0 }, { "2", '2', 0 },   { "3", '3', 0 }, { "+", '+', 0 },
    { "0", '0', 1 },                    { ".", '.', 0 }, { "=", '=', 0 },
};
#define N_KEYS ((int)(sizeof(KEYS) / sizeof(KEYS[0])))

static void press(char k) {
    if (k >= '0' && k <= '9') { digit(k); return; }
    switch (k) {
        case '.': digit('.'); break;
        case '+': case '-': case 'x': case '/': operator(k); break;
        case '=': equals(); break;
        case 'C': clear_all(); break;
        case '~': negate(); break;
        case '%': percent(); break;
        case 'r': square_root(); break;
        case 'm': memory_add(); break;
        case 'M': memory_recall(); break;
        case 'L': memory_clear(); break;
        case 8:   backspace(); break;
        default:  break;
    }
}

int main(void) {
    int win = win_create("Calculator", 280, 440);
    if (win < 0) exit(1);
    win_allow_resize(win);
    win_want_escape(win);                   /* it has a use for Escape */

    ui_input in;
    memset(&in, 0, sizeof(in));

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

        /* Typed as well as clicked. A calculator you cannot type at is a
           picture of a calculator. Every key of the frame: the desktop hands
           over all that are waiting at once, and "12+3" typed quickly lost
           everything after the 1. */
        for (int ki = 0; ki < in.nkeys; ki++) {
            u32 k = in.keys[ki];
            if (k == '\n' || k == '=') press('=');
            else if (k == 27) press('C');
            else if (k == '\b') press(8);
            else if (k == '*') press('x');
            else if (k == 'c' || k == 'C') press('C');
            else press((char)k);
        }

        fill(&s, t.bg);

        /* --- what it says -------------------------------------------------- */
        /* A sunk well with paper at the bottom, which is the shape that has
           always meant a thing to be read rather than pressed. The keys
           around it are raised; this is the one part of the window that is
           not, and that alone says which is which. */
        int disp_h = 86;
        int dx, dy, dw, dh;
        ui_well(&s, &t, UI_PAD, UI_PAD, w - UI_PAD * 2, disp_h - UI_PAD * 2,
                &dx, &dy, &dw, &dh);

        const char *text_now = error ? "cannot" : entry;
        int tw = face_w(text_now, UI_FACE_HEAD);
        face_draw(&s, dx + dw - UI_PAD - tw,
                  dy + dh - face_h(UI_FACE_HEAD) - UI_PAD,
                  text_now, error ? t.warn : t.fg, UI_FACE_HEAD);

        /* What is waiting, small, above it: 12 + is a state worth seeing.
           Formatted into a buffer of its own, because drawing must not
           touch what is being typed. */
        if (memory && !error)
            face_draw(&s, dx + dw - UI_PAD - face_w("M", UI_FACE_SMALL), dy + UI_PAD, "M", t.dim, UI_FACE_SMALL);
        if (pending && !error) {
            char wait[ENTRY_MAX + 8];
            format(acc, wait, sizeof(wait));
            int n = 0;
            while (wait[n]) n++;
            wait[n++] = ' ';
            wait[n++] = pending;
            wait[n] = 0;
            face_draw(&s, dx + UI_PAD, dy + UI_PAD, wait, t.dim, UI_FACE_SMALL);
        }

        /* --- the keys ------------------------------------------------------- */
        int gx = UI_PAD, gy = disp_h + UI_PAD;
        int gw = w - UI_PAD * 2, gh = h - gy - UI_PAD;
        int kw = (gw - UI_GAP * 3) / 4;
        int kh = (gh - UI_GAP * 5) / 6;
        if (kh < 20) kh = 20;

        int col = 0, row = 0;
        for (int i = 0; i < N_KEYS; i++) {
            int kx = gx + col * (kw + UI_GAP);
            int ky = gy + row * (kh + UI_GAP);
            int width = KEYS[i].wide ? kw * 2 + UI_GAP : kw;

            int over = ui_hit(&in, kx, ky, width, kh);
            int held = over && in.down;

            /* The operators carry the accent, the rest are surfaces. A key
               is a key, so it is built the way every other button on this
               desktop is: raised until it is held, and then the bevel turns
               over and the label moves a pixel down and right. A calculator
               with flat keys is a picture of a calculator. */
            int is_op = KEYS[i].key == '+' || KEYS[i].key == '-'
                     || KEYS[i].key == 'x' || KEYS[i].key == '/'
                     || KEYS[i].key == '=';
            u32 face = is_op ? t.accent : t.panel;
            if (KEYS[i].key == pending && is_op) face = mix(t.accent, 0, 60);
            if (over && !held) face = mix(face, t.edge_hi, 70);

            rect(&s, kx, ky, width, kh, face);
            if (held) ui_sunken(&s, &t, kx, ky, width, kh);
            else      ui_raised(&s, &t, kx, ky, width, kh);
            face_centred(&s, kx + (held ? 1 : 0), ky + (held ? 1 : 0),
                         width, kh, KEYS[i].label,
                         is_op ? t.accent_fg : t.fg, UI_FACE_HEAD);

            if (over && in.released) { in.released = 0; press(KEYS[i].key); }

            col += KEYS[i].wide ? 2 : 1;
            if (col >= 4) { col = 0; row++; }
        }

        win_commit(win);
        ui_wait(win);          /* until a key or the pointer, not sixty times a second */
    }

    win_close(win);
    exit(0);
}
