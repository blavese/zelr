/* What a poker hand is worth, and how to say it out loud.
 *
 * Seven cards, five of which count. The clever way to do this is a table of
 * a few million entries indexed by a product of primes, and it is clever
 * because it is fast; a hand is evaluated a handful of times a second here,
 * so what matters instead is that it is right. So this tries all twenty one
 * ways of choosing five from seven and keeps the best, and the five-card
 * evaluator underneath it is short enough to read in one go and check
 * against the rules by eye.
 *
 * A hand comes back as one number, so comparing two hands is comparing two
 * integers and there is no second function that has to agree with the
 * first. The category is in the top four bits and the tie-breakers follow
 * it, most significant first: the quad's rank then the kicker, the trips
 * then the pair, the five ranks of a flush in order. That works because
 * every tie in poker is broken by rank and never by suit -- no suit beats
 * another anywhere in the game, and two hands that are equal by rank split
 * the pot.
 */
#pragma once
#include "cards.h"

#define HC_HIGH      0
#define HC_PAIR      1
#define HC_TWOPAIR   2
#define HC_TRIPS     3
#define HC_STRAIGHT  4
#define HC_FLUSH     5
#define HC_BOAT      6
#define HC_QUADS     7
#define HC_STRFLUSH  8

#define HAND_CAT(v)  ((int)((v) >> 20))
#define HAND_KEY(v, i) ((int)(((v) >> (16 - (i) * 4)) & 0xF))

static const char *const HAND_SHORT[9] = {
    "high card", "pair", "two pair", "three of a kind", "straight",
    "flush", "full house", "four of a kind", "straight flush"
};

/* --- five cards ---------------------------------------------------------- */

static inline u32 eval5(const int *c) {
    int cnt[13];
    for (int i = 0; i < 13; i++) cnt[i] = 0;

    int suit = SUIT(c[0]), flush = 1;
    for (int i = 0; i < 5; i++) {
        cnt[RANK(c[i])]++;
        if (SUIT(c[i]) != suit) flush = 0;
    }

    /* The distinct ranks, most repeated first and then highest first. That
       single ordering is the tie-breaker for every category at once: quads
       then kicker, trips then pair, pair then three kickers, or just the
       five ranks. Writing it once is why there is no per-category list of
       what to compare next, which is where this sort of code goes wrong. */
    int order[5], n = 0;
    for (int want = 4; want >= 1; want--)
        for (int rk = 12; rk >= 0; rk--)
            if (cnt[rk] == want && n < 5) order[n++] = rk;

    int straight = 0, high = 0;
    if (n == 5) {
        if (order[0] - order[4] == 4) { straight = 1; high = order[0]; }
        else if (order[0] == R_ACE && order[1] == R_FIVE && order[4] == R_TWO) {
            /* The wheel. An ace plays low here and nowhere else, and the
               hand it makes is the worst straight rather than the best,
               which is why the five is what gets recorded as the high. */
            straight = 1; high = R_FIVE;
        }
    }

    int top = cnt[order[0]];
    int cat;
    if (straight && flush)          cat = HC_STRFLUSH;
    else if (top == 4)              cat = HC_QUADS;
    else if (top == 3 && n == 2)    cat = HC_BOAT;
    else if (flush)                 cat = HC_FLUSH;
    else if (straight)              cat = HC_STRAIGHT;
    else if (top == 3)              cat = HC_TRIPS;
    else if (top == 2 && n == 3)    cat = HC_TWOPAIR;
    else if (top == 2)              cat = HC_PAIR;
    else                            cat = HC_HIGH;

    u32 v = (u32)cat << 20;
    if (cat == HC_STRFLUSH || cat == HC_STRAIGHT) {
        /* Only the high card matters. The other four follow from it, and
           including them would make two straights of the same height
           compare unequal, which they are not. */
        return v | ((u32)high << 16);
    }
    for (int i = 0; i < n && i < 5; i++)
        v |= (u32)order[i] << (16 - i * 4);
    return v;
}

/* --- the best five of however many --------------------------------------- */

static inline u32 eval_best(const int *cards, int n) {
    if (n < 5) return 0;
    u32 best = 0;
    int pick[5];

    for (int a = 0; a <= n - 5; a++)
    for (int b = a + 1; b <= n - 4; b++)
    for (int c = b + 1; c <= n - 3; c++)
    for (int d = c + 1; d <= n - 2; d++)
    for (int e = d + 1; e <= n - 1; e++) {
        pick[0] = cards[a]; pick[1] = cards[b]; pick[2] = cards[c];
        pick[3] = cards[d]; pick[4] = cards[e];
        u32 v = eval5(pick);
        if (v > best) best = v;
    }
    return best;
}

/* Which five of the seven made it, so the winning hand can be lifted out of
   the rest at a showdown. Separate from the line above because most calls
   only want the number and this one walks the same loop twice. */
static inline u32 eval_best_five(const int *cards, int n, int *out) {
    if (n < 5) return 0;
    u32 best = 0;
    int pick[5];

    for (int a = 0; a <= n - 5; a++)
    for (int b = a + 1; b <= n - 4; b++)
    for (int c = b + 1; c <= n - 3; c++)
    for (int d = c + 1; d <= n - 2; d++)
    for (int e = d + 1; e <= n - 1; e++) {
        pick[0] = cards[a]; pick[1] = cards[b]; pick[2] = cards[c];
        pick[3] = cards[d]; pick[4] = cards[e];
        u32 v = eval5(pick);
        if (v > best) {
            best = v;
            for (int i = 0; i < 5; i++) out[i] = pick[i];
        }
    }
    return best;
}

/* --- saying it ------------------------------------------------------------
 *
 * "two pair, Kings and Fours" rather than "two pair", because at a showdown
 * the interesting part is which two pair, and a player who has just lost is
 * owed the reason rather than the category. */
static inline void hand_words(u32 v, char *out, int cap) {
    int at = 0;
    #define SAY(str) do { const char *p_ = (str); \
        while (*p_ && at < cap - 1) out[at++] = *p_++; } while (0)

    int cat = HAND_CAT(v);
    int k0 = HAND_KEY(v, 0), k1 = HAND_KEY(v, 1);

    switch (cat) {
    case HC_STRFLUSH:
        if (k0 == R_ACE) { SAY("a royal flush"); break; }
        SAY("a straight flush, "); SAY(RANK_NAME[k0]); SAY(" high");
        break;
    case HC_QUADS:    SAY("four "); SAY(RANK_WORD[k0]); break;
    case HC_BOAT:     SAY("a full house, "); SAY(RANK_WORD[k0]);
                      SAY(" over "); SAY(RANK_WORD[k1]); break;
    case HC_FLUSH:    SAY("a flush, "); SAY(RANK_NAME[k0]); SAY(" high"); break;
    case HC_STRAIGHT: SAY("a straight, "); SAY(RANK_NAME[k0]); SAY(" high"); break;
    case HC_TRIPS:    SAY("three "); SAY(RANK_WORD[k0]); break;
    case HC_TWOPAIR:  SAY("two pair, "); SAY(RANK_WORD[k0]);
                      SAY(" and "); SAY(RANK_WORD[k1]); break;
    case HC_PAIR:     SAY("a pair of "); SAY(RANK_WORD[k0]); break;
    default:          SAY(RANK_NAME[k0]); SAY(" high"); break;
    }
    #undef SAY
    out[at] = 0;
}
