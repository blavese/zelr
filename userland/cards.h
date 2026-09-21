/* A deck of cards: what one is, what one looks like, and where the order
 * comes from.
 *
 * Two programs need all of this and neither should own it. A card that is
 * drawn one way in blackjack and another way in poker is two programs that
 * came from different places, which is the thing ui.h exists to stop.
 *
 * There are no pictures here. Nothing in this project loads an icon from a
 * file and a playing card is no different: the pips are built out of discs
 * and triangles at whatever size the window gives them, so a bigger card is
 * a bigger drawing rather than a blurrier one.
 */
#pragma once
#include "zelr.h"
#include "alloc.h"
#include "draw.h"
#include "ui.h"

/* --- a card is a number --------------------------------------------------
 *
 * 0..51, rank in the low part and suit in the high. The ranks run 2, 3, ...
 * 10, J, Q, K, A rather than starting at the ace, so that comparing two
 * ranks is comparing two numbers and the ace comes out on top without
 * anybody special-casing it. The one place an ace is low is the wheel --
 * A2345, the worst straight -- and that is handled where straights are
 * found rather than by numbering every card to suit it. */
#define CARD_NONE  (-1)
#define RANK(c)    ((c) % 13)
#define SUIT(c)    ((c) / 13)
#define MAKE_CARD(r, s) ((s) * 13 + (r))

#define R_TWO    0
#define R_FIVE   3
#define R_TEN    8
#define R_JACK   9
#define R_QUEEN 10
#define R_KING  11
#define R_ACE   12

/* Bridge order, which is also alphabetical, so a hand sorted by card number
   groups the suits the way a person would lay them out. */
#define S_CLUB    0
#define S_DIAMOND 1
#define S_HEART   2
#define S_SPADE   3

#define CARD_RED(c) (SUIT(c) == S_DIAMOND || SUIT(c) == S_HEART)

static const char *const RANK_NAME[13] = {
    "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A"
};

/* For a sentence rather than for a corner: "a pair of Kings". */
static const char *const RANK_WORD[13] = {
    "Twos", "Threes", "Fours", "Fives", "Sixes", "Sevens", "Eights",
    "Nines", "Tens", "Jacks", "Queens", "Kings", "Aces"
};

/* --- where the order comes from ------------------------------------------
 *
 * Xorshift, sixty four bits, with a multiply on the way out. It is four
 * lines and it is not a cryptographic generator; what it has to be is a
 * shuffle nobody can see the pattern in, and for that it is plenty.
 *
 * The seeding is the part that matters. A generator seeded from the clock
 * alone deals the same cards to a machine that boots and starts the game at
 * the same moment every time, which on a machine that boots in four seconds
 * flat is not a rare accident. So the clock starts it and the person keeps
 * stirring it: every click and every keystroke mixes the tick it arrived on
 * back in, and the exact tick a hand was asked for is a number nobody can
 * arrange. A game nobody has touched yet is the one case that gets only the
 * clock, and that game has not dealt anything.
 */
typedef struct { u64 s; } rng;

static inline void rng_stir(rng *r, u64 x) {
    r->s ^= x + 0x9E3779B97F4A7C15ULL + (r->s << 6) + (r->s >> 2);
    if (!r->s) r->s = 0x9E3779B97F4A7C15ULL;
}

static inline void rng_start(rng *r) {
    r->s = 0x243F6A8885A308D3ULL;
    rng_stir(r, (u64)ticks());
    rng_stir(r, (u64)getpid() * 2654435761ULL);
    rng_stir(r, (u64)ticks() << 17);
}

static inline u32 rng_next(rng *r) {
    u64 x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return (u32)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

/* A number below n, with no bias.
 *
 * rng_next() % n is the obvious way and it is wrong: 2^32 does not divide
 * by 52, so the first few values come up one time in 82,595,525 more often
 * than the rest. Nobody would ever see it at a card table and it is still
 * a deck that is not evenly shuffled, which is the one thing a deck has to
 * be. So the top of the range that would not divide evenly is thrown away
 * and drawn again. */
static inline u32 rng_below(rng *r, u32 n) {
    if (n <= 1) return 0;
    u32 limit = 0xFFFFFFFFu - (0xFFFFFFFFu % n) - (n - 1);
    u32 v;
    do { v = rng_next(r); } while (v > limit);
    return v % n;
}

/* Fisher-Yates, walking down. Every arrangement of the deck is as likely as
   every other, which the same loop walking up is famously not. */
static inline void shuffle(rng *r, int *deck, int n) {
    for (int i = n - 1; i > 0; i--) {
        u32 j = rng_below(r, (u32)(i + 1));
        int t = deck[i]; deck[i] = deck[(int)j]; deck[(int)j] = t;
    }
}

/* A shoe of several decks, which is what a casino deals blackjack from and
   what makes counting the cards a thing that can be done at all. */
static inline void deck_fill(int *deck, int packs) {
    int at = 0;
    for (int p = 0; p < packs; p++)
        for (int c = 0; c < 52; c++) deck[at++] = c;
}

/* --- shapes --------------------------------------------------------------
 *
 * A filled triangle, by rows. Everything with a point on it is built from
 * this and a disc: a heart is two discs and a triangle pointing down, a
 * spade is the same drawing turned over with a stem under it, a diamond is
 * two triangles base to base, and a club is three discs and a stem.
 *
 * The edges are stepped rather than smoothed. At the size a pip is drawn
 * the step is a pixel, and a pip with soft edges at this size reads as a
 * smudge rather than as a shape. */
static inline void tri(surface *s, int x0, int y0, int x1, int y1,
                       int x2, int y2, u32 c) {
    int top = y0, bot = y0;
    if (y1 < top) top = y1;
    if (y2 < top) top = y2;
    if (y1 > bot) bot = y1;
    if (y2 > bot) bot = y2;

    for (int y = top; y <= bot; y++) {
        /* Where each of the three edges crosses this row. Two of them
           always do, unless the triangle is flat, and the span between the
           leftmost and the rightmost crossing is the row. */
        int lo = 1 << 30, hi = -(1 << 30);
        int xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
        for (int e = 0; e < 3; e++) {
            int ax = xs[e], ay = ys[e];
            int bx = xs[(e + 1) % 3], by = ys[(e + 1) % 3];
            if (ay == by) {
                if (ay != y) continue;
                if (ax < lo) lo = ax;
                if (bx < lo) lo = bx;
                if (ax > hi) hi = ax;
                if (bx > hi) hi = bx;
                continue;
            }
            if (y < (ay < by ? ay : by) || y > (ay < by ? by : ay)) continue;
            int x = ax + (bx - ax) * (y - ay) / (by - ay);
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        if (hi >= lo) rect(s, lo, y, hi - lo + 1, 1, c);
    }
}

/* The four pips, each drawn inside a box of the given size so that a caller
   asking for a pip in a corner and a caller asking for one in the middle of
   the card write the same line. */
static inline void pip(surface *s, int cx, int cy, int size, int suit, u32 c) {
    if (size < 4) size = 4;
    int h = size / 2;                 /* half the width */
    int q = size / 4;

    switch (suit) {
    case S_HEART:
        disc(s, cx - q, cy - q + 1, q + 1, c);
        disc(s, cx + q, cy - q + 1, q + 1, c);
        tri(s, cx - h, cy - q + 1, cx + h, cy - q + 1, cx, cy + h, c);
        break;

    case S_DIAMOND:
        tri(s, cx, cy - h, cx - h + 1, cy, cx + h - 1, cy, c);
        tri(s, cx, cy + h, cx - h + 1, cy, cx + h - 1, cy, c);
        break;

    case S_SPADE:
        tri(s, cx, cy - h, cx - h, cy + q, cx + h, cy + q, c);
        disc(s, cx - q, cy + q - 1, q + 1, c);
        disc(s, cx + q, cy + q - 1, q + 1, c);
        /* The stem, which is what tells a spade from a heart upside down at
           a glance: it flares at the bottom. */
        rect(s, cx - 1, cy + q, 3, h - q, c);
        tri(s, cx, cy + q + 1, cx - q - 1, cy + h, cx + q + 1, cy + h, c);
        break;

    default: /* clubs */
        disc(s, cx, cy - q - 1, q + 1, c);
        disc(s, cx - q - 1, cy + q - 1, q + 1, c);
        disc(s, cx + q + 1, cy + q - 1, q + 1, c);
        rect(s, cx - 1, cy, 3, h, c);
        tri(s, cx, cy + q, cx - q - 1, cy + h, cx + q + 1, cy + h, c);
        break;
    }
}

/* --- a card --------------------------------------------------------------
 *
 * White, with the rank and the suit in the top left and the same pair
 * upside down in the bottom right, because that is how a card is read in a
 * hand held fanned in one direction or the other. The middle carries one
 * big pip rather than the proper arrangement of two through ten: at sixty
 * pixels across, ten small pips are ten smudges, and the number is already
 * written twice in the corners where a player actually looks.
 */
#define CARD_W 62
#define CARD_H 88

static inline void card_face(surface *s, int x, int y, int w, int h, int card) {
    u32 ink = CARD_RED(card) ? 0xC02030 : 0x1A1A22;

    /* A hairline rather than a bevel. A card is a piece of paper and the
       thing that says where its edge is, is the shadow of the next one. */
    ui_round_outline(s, x, y, w, h, 6, 0xFFFFFF, 0xC8C8D0);

    if (card < 0 || card > 51) return;

    const char *r = RANK_NAME[RANK(card)];
    int face = (w >= 50) ? UI_FACE_BOLD : UI_FACE_SMALL;
    int rw = face_w(r, face);

    face_draw(s, x + 5, y + 4, r, ink, face);
    pip(s, x + 5 + rw / 2, y + 4 + face_h(face) + w / 11, w / 5, SUIT(card), ink);

    /* The middle, at whatever is left after the corners. */
    pip(s, x + w / 2, y + h / 2, w * 2 / 5, SUIT(card), ink);

    /* And the corner again, the other way up. Drawn as the same two calls
       mirrored through the centre rather than as a rotated glyph, because
       there is no rotated glyph and a card only has to look right. */
    face_draw(s, x + w - 5 - rw, y + h - 4 - face_h(face), r, ink, face);
    pip(s, x + w - 5 - rw / 2, y + h - 4 - face_h(face) - w / 11,
        w / 5, SUIT(card), ink);
}

static inline void card_back(surface *s, int x, int y, int w, int h,
                             u32 accent) {
    ui_round_outline(s, x, y, w, h, 6, mix(accent, 0x000000, 90), 0xF0F0F4);
    round_rect(s, x + 4, y + 4, w - 8, h - 8, 3, mix(accent, 0xFFFFFF, 30));

    /* A lattice, which is a back rather than a coloured rectangle. Drawn as
       two sets of diagonals across the inner panel and clipped to it by
       being drawn only where the panel is. */
    u32 ink = mix(accent, 0x000000, 60);
    for (int i = -h; i < w; i += 7) {
        for (int k = 0; k < 2; k++) {
            int step = 0;
            int px = x + 4 + i, py = y + 4;
            while (px >= x + 4 && px < x + w - 4 && py < y + h - 4) {
                if (px >= x + 5 && py >= y + 5)
                    rect(s, px, py, 1, 1, ink);
                px += k ? -1 : 1;
                py++;
                step++;
                if (step > h) break;
            }
        }
    }
}

/* A ring around something, and nothing inside it.
 *
 * ui_round_outline fills as well as outlines, which is right for a control
 * and wrong over a cloth: the fill would put a flat patch where the shading
 * was. Two nested frames is a two pixel border and leaves what is behind it
 * alone, which is the whole requirement. */
static inline void card_ring(surface *s, int x, int y, int w, int h, u32 c) {
    frame(s, x, y, w, h, c);
    frame(s, x + 1, y + 1, w - 2, h - 2, c);
}

/* --- money ---------------------------------------------------------------
 *
 * Whole units. There is no floating point anywhere in this system and a
 * chip is not divisible anyway, so a bet is a count of them and a blackjack
 * paying three to two on an odd bet rounds the way a table does, which is
 * down, in the house's favour. Saying so is the point: silently rounding a
 * payout up would be a generous bug and it would still be a bug. */
static inline void money(int n, char *out, int cap) {
    char tmp[16];
    int at = 0, neg = n < 0;
    u32 v = (u32)(neg ? -n : n);

    if (!v) tmp[at++] = '0';
    while (v && at < 12) { tmp[at++] = (char)('0' + v % 10); v /= 10; }

    int o = 0;
    if (o < cap - 1) out[o++] = '$';
    if (neg && o < cap - 1) out[o++] = '-';
    /* Thousands separated, because a bankroll that reads 10000 and one that
       reads 100000 look the same at a glance and one of them is ten times
       the other. */
    for (int i = at - 1; i >= 0 && o < cap - 1; i--) {
        out[o++] = tmp[i];
        if (i && i % 3 == 0 && o < cap - 1) out[o++] = ',';
    }
    out[o] = 0;
}

/* --- drawing somewhere nobody is looking ----------------------------------
 *
 * Here because both games need it, and only because both games need it: it
 * is not about cards and it belongs somewhere more general the day a third
 * program wants it.
 *
 *
 * A window's surface is the pixels the desktop composites from. There is no
 * second buffer and win_commit does not swap one: it says the window has
 * changed and the compositor reads the same memory the program is writing.
 *
 * Which is fine for a program whose frame is a few small rectangles, and it
 * is not fine for this one. The first thing a frame does is paint the table
 * over everything, and until the cards go back on top there is a stretch of
 * time when the window genuinely contains a table and nothing else. The
 * compositor is a task like any other and it runs in that gap -- reliably,
 * because a sleep from ring 3 can return with no time passed, so this loop
 * never idles and is always somewhere in the middle of a frame. What that
 * looked like was a maximised poker game with no cards, no seats and no
 * buttons in it, which reads as a drawing bug and was a timing one.
 *
 * So the frame is drawn into memory of this program's own and copied over
 * at the end. The copy can be interrupted like anything else, but what the
 * compositor sees then is part of one finished frame and part of another,
 * and between two frames of a card table that is nothing at all.
 *
 * The copy is written through a volatile pointer on purpose: without it the
 * optimiser recognises the loop and replaces it with a call to memcpy,
 * which is a symbol this system does not have. */
static u32 *shadow;
static int  shadow_w, shadow_h;

static inline surface *frame_surface(u32 *px, int w, int h) {
    static surface s;
    if (!shadow || shadow_w != w || shadow_h != h) {
        u32 *fresh = (u32 *)malloc((u64)w * (u64)h * 4);
        if (!fresh) { s.px = px; s.w = w; s.h = h; return &s; }
        if (shadow) free(shadow);
        shadow = fresh;
        shadow_w = w;
        shadow_h = h;
    }
    s.px = shadow; s.w = w; s.h = h;
    return &s;
}

static inline void show_frame(u32 *px, int w, int h) {
    if (!shadow || shadow_w != w || shadow_h != h) return;
    volatile u32 *d = (volatile u32 *)px;
    const volatile u32 *b = (const volatile u32 *)shadow;
    u32 n = (u32)w * (u32)h;
    for (u32 i = 0; i < n; i++) d[i] = b[i];
}

