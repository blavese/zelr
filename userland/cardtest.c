/* The card code, against hands whose answer is not a matter of opinion.
 *
 * A poker program that ranks hands slightly wrong is the worst kind of
 * broken: it deals, it bets, it pays somebody, and the only way to notice
 * is to be the player who was robbed and to know the rules well enough to
 * be sure. Nothing on the screen would ever say so. So the ranking is
 * checked here, against hands written out by hand, with the answer being
 * one somebody can look up.
 *
 * The cases that look like trivia are the ones that matter. The wheel is a
 * straight and it is the lowest one, so an evaluator that scores an ace as
 * fourteen makes it the highest and pays the wrong player. A flush beats a
 * straight by one step and the two are next to each other, which is where
 * an off-by-one in the category lands. And two hands that tie have to tie
 * exactly, because a split pot that goes one way is a pot given to
 * somebody by an accident of ordering.
 *
 * It runs inside the machine with no screen.
 */
#include "zelr.h"
#include "cards.h"
#include "poker.h"

static int failed, ran;

static void ok(const char *what, int good) {
    ran++;
    puts(good ? "  PASS  " : "  FAIL  ");
    puts(what);
    puts("\n");
    if (!good) failed++;
}

/* A hand written the way it is spoken: "AS KS QS JS TS". Ranks are the
   usual letters with T for ten, suits are the first letter of the suit. */
static int card_of(const char *s) {
    int rank = -1, suit = -1;
    switch (s[0]) {
    case '2': rank = 0; break;  case '3': rank = 1; break;
    case '4': rank = 2; break;  case '5': rank = 3; break;
    case '6': rank = 4; break;  case '7': rank = 5; break;
    case '8': rank = 6; break;  case '9': rank = 7; break;
    case 'T': rank = 8; break;  case 'J': rank = 9; break;
    case 'Q': rank = 10; break; case 'K': rank = 11; break;
    case 'A': rank = 12; break;
    }
    switch (s[1]) {
    case 'C': suit = S_CLUB; break;    case 'D': suit = S_DIAMOND; break;
    case 'H': suit = S_HEART; break;   case 'S': suit = S_SPADE; break;
    }
    if (rank < 0 || suit < 0) return 0;
    return MAKE_CARD(rank, suit);
}

static int parse_hand(const char *s, int *out) {
    int n = 0;
    while (*s && n < 7) {
        while (*s == ' ') s++;
        if (!*s) break;
        out[n++] = card_of(s);
        while (*s && *s != ' ') s++;
    }
    return n;
}

static u32 value(const char *hand) {
    int c[7];
    int n = parse_hand(hand, c);
    return eval_best(c, n);
}

static void beats(const char *what, const char *win, const char *lose) {
    ok(what, value(win) > value(lose));
}

static void ties(const char *what, const char *a, const char *b) {
    ok(what, value(a) == value(b));
}

static void category(const char *what, const char *hand, int want) {
    ok(what, HAND_CAT(value(hand)) == want);
}

/* --- blackjack's arithmetic, which is the other half of the shared code --- */

static void bj_value(const int *c, int n, int *total, int *soft) {
    int t = 0, aces = 0;
    for (int i = 0; i < n; i++) {
        int rk = RANK(c[i]);
        if (rk == R_ACE)      { aces++; t += 11; }
        else if (rk >= R_TEN) { t += 10; }
        else                  { t += rk + 2; }
    }
    while (t > 21 && aces) { t -= 10; aces--; }
    if (total) *total = t;
    if (soft)  *soft = aces > 0;
}

static void bj(const char *what, const char *hand, int want_total, int want_soft) {
    int c[7];
    int n = parse_hand(hand, c);
    int t, s;
    bj_value(c, n, &t, &s);
    ok(what, t == want_total && s == want_soft);
}

int main(void) {
    puts("=== cards ===\n");

    /* --- the ladder, one step at a time ---------------------------------- */
    category("five to the ace in one suit is a straight flush",
             "AS KS QS JS TS", HC_STRFLUSH);
    category("four of a rank is four of a kind", "9C 9D 9H 9S 2C", HC_QUADS);
    category("three and two is a full house", "KC KD KH 4S 4C", HC_BOAT);
    category("five of one suit is a flush", "KC 9C 7C 4C 2C", HC_FLUSH);
    category("five in a row is a straight", "9C 8D 7H 6S 5C", HC_STRAIGHT);
    category("three of a rank is three of a kind", "QC QD QH 7S 2C", HC_TRIPS);
    category("two and two is two pair", "JC JD 5H 5S 9C", HC_TWOPAIR);
    category("two of a rank is a pair", "TC TD 8H 5S 2C", HC_PAIR);
    category("and none of those is a high card", "AC JD 8H 5S 2C", HC_HIGH);

    beats("a straight flush beats four of a kind",
          "9C 8C 7C 6C 5C", "AC AD AH AS KC");
    beats("four of a kind beats a full house",
          "2C 2D 2H 2S 3C", "AC AD AH KS KC");
    beats("a full house beats a flush",
          "2C 2D 2H 3S 3C", "AC KC QC JC 9C");
    beats("a flush beats a straight",
          "8C 6C 4C 3C 2C", "AC KD QH JS TC");
    beats("a straight beats three of a kind",
          "6C 5D 4H 3S 2C", "AC AD AH KS QC");
    beats("three of a kind beats two pair",
          "2C 2D 2H 3S 4C", "AC AD KH KS QC");
    beats("two pair beats a pair",
          "2C 2D 3H 3S 4C", "AC AD KH QS JC");
    beats("a pair beats a high card",
          "2C 2D 3H 4S 5C", "AC KD QH JS 9C");

    /* --- the ace at both ends --------------------------------------------- */
    category("the wheel is a straight", "AC 2D 3H 4S 5C", HC_STRAIGHT);
    beats("and it is the lowest one: a six-high straight beats it",
          "6C 5D 4H 3S 2C", "AC 2D 3H 4S 5C");
    beats("while the ace at the top makes the highest",
          "AC KD QH JS TC", "KC QD JH TS 9C");
    beats("a wheel in one suit is still a straight flush",
          "AC 2C 3C 4C 5C", "AD AH AS KD KH");
    beats("and the king-high one beats it",
          "KC QC JC TC 9C", "AC 2C 3C 4C 5C");

    /* --- the tie-breakers -------------------------------------------------- */
    beats("the higher pair wins", "KC KD 4H 3S 2C", "QC QD AH KS JC");
    beats("the same pair is decided by the kicker",
          "KC KD AH 3S 2C", "KH KS QD JC 9H");
    beats("the higher of two pair wins first",
          "AC AD 2H 2S 9C", "KC KD QH QS JC");
    beats("then the lower of the two",
          "AC AD 3H 3S 4C", "AH AS 2D 2C KH");
    beats("and then the fifth card",
          "AC AD 3H 3S KC", "AH AS 3D 3C QH");
    beats("the higher flush wins by its top card",
          "AC 9C 7C 4C 2C", "KC QC JC 9C 8C");
    beats("and by the next one down when the top is shared",
          "AC KC 7C 4C 2C", "AD QD JD 9D 8D");

    ties("two hands with the same ranks are equal, whatever the suits",
         "AC KD QH JS 9C", "AH KS QC JD 9H");
    ties("and a straight is a straight in any suits",
         "9C 8D 7H 6S 5C", "9H 8S 7C 6D 5H");

    /* --- seven cards, five of which count ---------------------------------- */
    ok("the best five of seven are found, not the first five",
       HAND_CAT(value("2C 7D AC AD AH AS KC")) == HC_QUADS);
    ok("a flush inside seven cards is found",
       HAND_CAT(value("AC 9C 7C 4C 2C KD QH")) == HC_FLUSH);
    ok("and a straight that needs both hole cards",
       HAND_CAT(value("9C 8D 7H 2S 3C 6D 5S")) == HC_STRAIGHT);
    beats("seven cards make the better hand out of the same board",
          "AC AD KH QS JC TD 9H", "2C 3D KH QS JC TD 9H");

    /* --- what it is called -------------------------------------------------- */
    {
        char words[40];
        hand_words(value("AS KS QS JS TS"), words, sizeof(words));
        ok("a royal flush is called one", strcmp(words, "a royal flush") == 0);
        hand_words(value("KC KD 5H 5S 2C"), words, sizeof(words));
        ok("and two pair says which two",
           strcmp(words, "two pair, Kings and Fives") == 0);
        hand_words(value("9C 9D 9H 9S 2C"), words, sizeof(words));
        ok("and quads say of what", strcmp(words, "four Nines") == 0);
    }

    /* --- the deck ------------------------------------------------------------
     *
     * A shuffle that loses a card or deals one twice is a game that cannot
     * be won or lost fairly, and neither shows up on the screen. */
    {
        rng r;
        rng_start(&r);
        int deck[52], seen[52];
        int good = 1;
        for (int round = 0; round < 40; round++) {
            for (int i = 0; i < 52; i++) { deck[i] = i; seen[i] = 0; }
            shuffle(&r, deck, 52);
            for (int i = 0; i < 52; i++) {
                if (deck[i] < 0 || deck[i] > 51) { good = 0; break; }
                seen[deck[i]]++;
            }
            for (int i = 0; i < 52; i++) if (seen[i] != 1) good = 0;
        }
        ok("forty shuffles each hold all fifty two cards once", good);

        /* And that it is a shuffle rather than a rotation: two shuffles in a
           row that came out identical would pass every check above. */
        int a[52], b[52];
        for (int i = 0; i < 52; i++) { a[i] = i; b[i] = i; }
        shuffle(&r, a, 52);
        shuffle(&r, b, 52);
        int same = 1;
        for (int i = 0; i < 52; i++) if (a[i] != b[i]) same = 0;
        ok("and two of them are not the same order", !same);

        /* The range, which is where a modulo would have put the bias. */
        int counts[6];
        for (int i = 0; i < 6; i++) counts[i] = 0;
        int in_range = 1;
        for (int i = 0; i < 6000; i++) {
            u32 v = rng_below(&r, 6);
            if (v > 5) { in_range = 0; break; }
            counts[v]++;
        }
        ok("a number below six is always below six", in_range);
        int flat = 1;
        for (int i = 0; i < 6; i++) if (counts[i] < 800 || counts[i] > 1200) flat = 0;
        ok("and six thousand of them land roughly evenly", flat);
    }

    /* --- blackjack's aces ------------------------------------------------- */
    bj("an ace and a king is twenty one, soft", "AC KD", 21, 1);
    bj("two aces is twelve, still soft", "AC AD", 12, 1);
    bj("three aces is thirteen", "AC AD AH", 13, 1);
    bj("an ace stops counting as eleven when it would bust",
       "AC 9D 5H", 15, 0);
    bj("a hand with no ace is hard", "TC 7D", 17, 0);
    bj("and the face cards are all ten", "KC QD", 20, 0);
    bj("a soft seventeen is seventeen", "AC 6D", 17, 1);
    bj("and drawing to it can leave it soft", "AC 2D 4H", 17, 1);
    bj("twenty two is a bust", "KC QD 2H", 22, 0);

    /* --- money ------------------------------------------------------------- */
    {
        char m[24];
        money(0, m, sizeof(m));       ok("nothing is $0", strcmp(m, "$0") == 0);
        money(25, m, sizeof(m));      ok("small is $25", strcmp(m, "$25") == 0);
        money(1000, m, sizeof(m));    ok("a thousand is separated",
                                         strcmp(m, "$1,000") == 0);
        money(1234567, m, sizeof(m)); ok("and so is a million",
                                         strcmp(m, "$1,234,567") == 0);
        money(-50, m, sizeof(m));     ok("a loss carries its sign",
                                         strcmp(m, "$-50") == 0);
    }

    puts(failed ? "CARDTEST_FAIL\n" : "CARDTEST_PASS\n");
    return failed;
}
