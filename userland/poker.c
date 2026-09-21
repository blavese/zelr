/* Texas hold'em, no limit, against three opponents that cannot see your
 * cards.
 *
 * Which is worth saying first, because it is the thing a card game on a
 * machine like this is always suspected of. The opponents are handed their
 * own two cards and the board and nothing else: there is no function that
 * takes another seat's hand, and the decision each of them makes is a
 * number worked out from what it holds and what the pot is offering. They
 * are not very good. They are honest.
 *
 * The rules that are actually implemented, rather than implied:
 *
 *   Blinds of ten and twenty, the button moving one seat a hand. Four
 *   betting rounds. A raise has to be at least the size of the last raise,
 *   except that a player with less than that may still put it all in, which
 *   does not reopen the betting for anybody who has already called.
 *
 *   Side pots. A player who is all in for less can only win the part of the
 *   pot they paid for, and the rest goes on without them. This is the rule
 *   that separates a poker program from a program that deals cards, and it
 *   is the one that a game with fixed stacks never has to face.
 *
 *   A split pot divides evenly and the odd chip goes to the first winner
 *   left of the button, because a chip cannot be cut in half and somebody
 *   has to have it.
 *
 *   A player out of chips is out of the game. When one seat has all of it,
 *   that is the end, and the same is true when it is not you.
 */
#include "zelr.h"
#include "ui.h"
#include "cards.h"
#include "poker.h"

#define SEATS       4
#define START_STACK 1000
#define SMALL_BLIND 10
#define BIG_BLIND   20
#define DEAL_TICKS  7
#define THINK_MIN   45
#define THINK_SPAN  55

typedef struct {
    const char *name;
    int stack;
    int hole[2];
    int shown[2];        /* the tick each card lands on */
    int street_bet;      /* into this round of betting */
    int total_bet;       /* into this hand, which is what side pots need */
    int folded;
    int allin;
    int acted;           /* has had a turn since the last raise */
    int out;             /* no chips, no longer dealt in */
    int won;             /* what the last hand paid this seat */
    char said[28];
    u32  show;           /* their hand at a showdown, 0 until then */
    char shows[40];
} seat;

enum { PS_DEAL, PS_ACT, PS_SHOWDOWN, PS_OVER, PS_GAMEOVER };

static rng  rg;
static int  deck[52], deck_at;

static seat tbl[SEATS];
static int  board[5], board_at[5], board_n;

static int  button = -1;
static int  street;              /* 0 preflop, 1 flop, 2 turn, 3 river */
static int  level;               /* the bet to match this street */
static int  min_raise;
static int  turn;                /* whose move, or -1 */
static int  think_until;
static int  state = PS_DEAL;
static int  hand_no;
static char headline[96];

static int  raise_to;            /* what the slider is set to */

/* --- the deck ------------------------------------------------------------ */

static int draw_card(void) {
    if (deck_at >= 52) { shuffle(&rg, deck, 52); deck_at = 0; }
    return deck[deck_at++];
}

/* --- money on the table --------------------------------------------------- */

static int pot_total(void) {
    int n = 0;
    for (int i = 0; i < SEATS; i++) n += tbl[i].total_bet;
    return n;
}

static int live_count(void) {
    int n = 0;
    for (int i = 0; i < SEATS; i++)
        if (!tbl[i].out && !tbl[i].folded) n++;
    return n;
}

static int can_act_count(void) {
    int n = 0;
    for (int i = 0; i < SEATS; i++)
        if (!tbl[i].out && !tbl[i].folded && !tbl[i].allin) n++;
    return n;
}

static void say(int i, const char *what, int amount) {
    char *d = tbl[i].said;
    int at = 0;
    while (*what && at < 20) d[at++] = *what++;
    if (amount > 0) {
        char m[20];
        money(amount, m, sizeof(m));
        if (at < 22) d[at++] = ' ';
        for (const char *p = m; *p && at < 27; p++) d[at++] = *p;
    }
    d[at] = 0;
}

/* Moving chips in. The target is what this seat's bet for the street should
   become; what actually moves is whatever is left in front of them, which
   is how an all in for less than a call happens without a separate path. */
static void commit(int i, int target) {
    seat *s = &tbl[i];
    int add = target - s->street_bet;
    if (add < 0) add = 0;
    if (add > s->stack) add = s->stack;
    s->stack -= add;
    s->street_bet += add;
    s->total_bet += add;
    if (s->stack == 0) s->allin = 1;
}

/* --- side pots ------------------------------------------------------------
 *
 * Everything anybody put in is in one heap, and the heap is cut into layers
 * by the amounts the players who are still in went to. A player who is all
 * in for sixty can win every layer up to sixty and none above it, and the
 * chips above it came from people who are still betting with each other.
 *
 * The money a folded player left behind belongs to the layers it was put
 * into, which is why this walks every seat rather than only the live ones:
 * a fold does not take its bet back off the table. */
static void award(void) {
    int levels[SEATS], nl = 0;

    for (int i = 0; i < SEATS; i++) {
        if (tbl[i].out || tbl[i].folded) continue;
        int v = tbl[i].total_bet, seen = 0;
        for (int k = 0; k < nl; k++) if (levels[k] == v) seen = 1;
        if (!seen) levels[nl++] = v;
    }
    for (int a = 0; a < nl; a++)
        for (int b = a + 1; b < nl; b++)
            if (levels[b] < levels[a]) { int t = levels[a]; levels[a] = levels[b]; levels[b] = t; }

    int prev = 0;
    for (int k = 0; k < nl; k++) {
        int lv = levels[k], pot = 0;

        for (int i = 0; i < SEATS; i++) {
            int c = tbl[i].total_bet;
            int upper = c < lv ? c : lv;
            int lower = c < prev ? c : prev;
            pot += upper - lower;
        }
        prev = lv;
        if (pot <= 0) continue;

        u32 best = 0;
        for (int i = 0; i < SEATS; i++) {
            if (tbl[i].out || tbl[i].folded || tbl[i].total_bet < lv) continue;
            if (tbl[i].show > best) best = tbl[i].show;
        }

        int winners = 0;
        for (int i = 0; i < SEATS; i++)
            if (!tbl[i].out && !tbl[i].folded && tbl[i].total_bet >= lv
                && tbl[i].show == best) winners++;
        if (!winners) continue;

        int each = pot / winners, odd = pot - each * winners;

        /* The odd chip to the first winner left of the button, which is
           where a dealer puts it and is at least a rule rather than
           whoever happens to be first in the array. */
        for (int step = 1; step <= SEATS && odd; step++) {
            int i = (button + step) % SEATS;
            if (!tbl[i].out && !tbl[i].folded && tbl[i].total_bet >= lv
                && tbl[i].show == best) { tbl[i].stack += odd; tbl[i].won += odd; odd = 0; }
        }
        for (int i = 0; i < SEATS; i++)
            if (!tbl[i].out && !tbl[i].folded && tbl[i].total_bet >= lv
                && tbl[i].show == best) { tbl[i].stack += each; tbl[i].won += each; }
    }
}

static void note_headline(const char *s) {
    int at = 0;
    while (*s && at < (int)sizeof(headline) - 1) headline[at++] = *s++;
    headline[at] = 0;
}

static void finish(void) {
    for (int i = 0; i < SEATS; i++)
        if (!tbl[i].out && tbl[i].stack == 0) tbl[i].out = 1;

    int standing = 0, who = -1;
    for (int i = 0; i < SEATS; i++) if (!tbl[i].out) { standing++; who = i; }

    state = (standing <= 1) ? PS_GAMEOVER : PS_OVER;
    if (state == PS_GAMEOVER)
        note_headline(who == 0 ? "You have all the chips."
                               : "You are out of chips.");
}

/* Everybody folded to one player: no cards are shown, because nobody paid
   to see them. */
static void uncontested(void) {
    int who = -1;
    for (int i = 0; i < SEATS; i++)
        if (!tbl[i].out && !tbl[i].folded) who = i;

    int pot = pot_total();
    if (who >= 0) { tbl[who].stack += pot; tbl[who].won += pot; }

    char line[96];
    int at = 0;
    const char *nm = who == 0 ? "You take" : tbl[who].name;
    for (const char *p = nm; *p && at < 40; p++) line[at++] = *p;
    if (who != 0) { const char *t = " takes"; while (*t && at < 50) line[at++] = *t++; }
    line[at++] = ' ';
    char m[20]; money(pot, m, sizeof(m));
    for (const char *p = m; *p && at < 90; p++) line[at++] = *p;
    line[at] = 0;
    note_headline(line);
    finish();
}

static void showdown(void) {
    int seven[7];
    for (int i = 0; i < SEATS; i++) {
        if (tbl[i].out || tbl[i].folded) continue;
        seven[0] = tbl[i].hole[0];
        seven[1] = tbl[i].hole[1];
        for (int b = 0; b < board_n; b++) seven[2 + b] = board[b];
        tbl[i].show = eval_best(seven, 2 + board_n);
        hand_words(tbl[i].show, tbl[i].shows, sizeof(tbl[i].shows));
    }
    award();

    u32 best = 0;
    for (int i = 0; i < SEATS; i++)
        if (!tbl[i].out && !tbl[i].folded && tbl[i].show > best) best = tbl[i].show;

    int winners = 0, only = -1;
    for (int i = 0; i < SEATS; i++)
        if (!tbl[i].out && !tbl[i].folded && tbl[i].show == best) {
            winners++;
            only = i;
        }

    char line[96];
    int at = 0;
    int first = 1;
    for (int i = 0; i < SEATS; i++) {
        if (tbl[i].out || tbl[i].folded || tbl[i].show != best) continue;
        if (!first) { const char *a = " and "; while (*a && at < 60) line[at++] = *a++; }
        const char *nm = i == 0 ? "You" : tbl[i].name;
        for (const char *p = nm; *p && at < 60; p++) line[at++] = *p;
        first = 0;
    }
    /* One winner who is not you takes the s. Two of anybody, or you on your
       own, do not. Writing "Bishop win with a pair of Queens" is a small
       thing and it is the sort of small thing that makes a program read as
       though nobody looked at it. */
    const char *w = (winners == 1 && only != 0) ? " wins with " : " win with ";
    while (*w && at < 74) line[at++] = *w++;
    char words[40];
    hand_words(best, words, sizeof(words));
    for (const char *p = words; *p && at < 94; p++) line[at++] = *p;
    line[at] = 0;
    note_headline(line);

    state = PS_SHOWDOWN;
    finish();
    if (state == PS_OVER) state = PS_SHOWDOWN;
}

/* --- the order of play ---------------------------------------------------- */

static int next_seat(int from) {
    for (int step = 1; step <= SEATS; step++) {
        int i = (from + step) % SEATS;
        if (!tbl[i].out && !tbl[i].folded && !tbl[i].allin) return i;
    }
    return -1;
}

static void advance_street(void);

static int betting_done(void) {
    if (live_count() <= 1) return 1;
    for (int i = 0; i < SEATS; i++) {
        const seat *s = &tbl[i];
        if (s->out || s->folded || s->allin) continue;
        if (!s->acted) return 0;
        if (s->street_bet < level) return 0;
    }
    return 1;
}

static void pass_turn(void) {
    if (live_count() <= 1) { uncontested(); return; }
    if (betting_done()) { advance_street(); return; }

    int i = next_seat(turn);
    /* next_seat skips the seat it was given, which is what is wanted after
       an action and wrong if that seat is the only one left to act. */
    if (i < 0) { advance_street(); return; }
    turn = i;
    if (turn != 0) think_until = ticks() + THINK_MIN
                               + (int)rng_below(&rg, THINK_SPAN);
}

static void advance_street(void) {
    if (live_count() <= 1) { uncontested(); return; }

    street++;
    if (street > 3) { showdown(); return; }

    for (int i = 0; i < SEATS; i++) {
        tbl[i].street_bet = 0;
        tbl[i].acted = 0;
        tbl[i].said[0] = 0;
    }
    level = 0;
    min_raise = BIG_BLIND;

    int at = ticks();
    if (street == 1) {
        for (int k = 0; k < 3; k++) { board[board_n] = draw_card();
                                      board_at[board_n] = at + k * DEAL_TICKS;
                                      board_n++; }
        at += 3 * DEAL_TICKS;
    } else {
        board[board_n] = draw_card();
        board_at[board_n] = at + DEAL_TICKS;
        board_n++;
        at += DEAL_TICKS;
    }

    /* With one player left who can still bet, there is nothing to bet
       into: everybody else is all in and the rest of the board is dealt
       without another word. */
    if (can_act_count() < 2) { advance_street(); return; }

    turn = next_seat(button);
    if (turn < 0) { advance_street(); return; }
    if (turn != 0) think_until = at + THINK_MIN + (int)rng_below(&rg, THINK_SPAN);
    state = PS_ACT;
}

/* --- what a player does --------------------------------------------------- */

static void act_fold(int i) {
    tbl[i].folded = 1;
    tbl[i].acted = 1;
    say(i, "folds", 0);
    pass_turn();
}

static void act_call(int i) {
    int owed = level - tbl[i].street_bet;
    if (owed <= 0) { say(i, "checks", 0); }
    else {
        int had = tbl[i].stack;
        commit(i, level);
        say(i, had <= owed ? "all in" : "calls", had <= owed ? had : owed);
    }
    tbl[i].acted = 1;
    pass_turn();
}

/* `to` is what this seat's bet for the street becomes. A raise that cannot
   reach the minimum is still allowed when it is everything the player has,
   and it does not reopen the betting for anyone who already called -- which
   is why `acted` is only cleared when the raise was a full one. */
static void act_raise(int i, int to) {
    int most = tbl[i].street_bet + tbl[i].stack;
    if (to > most) to = most;
    if (to <= level) { act_call(i); return; }

    int full = (to - level) >= min_raise;
    int had = tbl[i].stack;
    commit(i, to);

    if (full) {
        min_raise = to - level;
        for (int k = 0; k < SEATS; k++)
            if (k != i && !tbl[k].out && !tbl[k].folded && !tbl[k].allin)
                tbl[k].acted = 0;
    }
    level = to;
    tbl[i].acted = 1;

    if (tbl[i].allin)        say(i, "all in", had);
    else if (level == to && street == 0 && to == BIG_BLIND) say(i, "calls", to);
    else                     say(i, "raises to", to);
    pass_turn();
}

/* --- the opponents --------------------------------------------------------
 *
 * A number for how good the hand looks, and then a decision made against
 * what the pot is offering. Nothing here reads another seat's cards; the
 * inputs are this seat's two cards, the board, and the money.
 *
 * Before the flop there is no board to evaluate, so the hand is scored the
 * way a person does it: a pair is worth a lot, two high cards are worth
 * something, and suited or connected cards are worth a little more because
 * of what they might become. After the flop the hand is simply evaluated
 * and the category is most of the answer.
 *
 * They bluff, rarely, and they do it by betting a hand they would otherwise
 * check. An opponent that never bluffs is one you can fold against forever,
 * which is duller than losing. */
static int strength_of(int i) {
    const seat *s = &tbl[i];

    if (board_n == 0) {
        int a = RANK(s->hole[0]), b = RANK(s->hole[1]);
        int hi = a > b ? a : b, lo = a > b ? b : a;
        int v;
        if (a == b) v = 520 + hi * 38;
        else {
            v = hi * 30 + lo * 12;
            if (SUIT(s->hole[0]) == SUIT(s->hole[1])) v += 70;
            int gap = hi - lo;
            if (gap == 1) v += 55;
            else if (gap == 2) v += 25;
            else if (gap > 4) v -= 40;
        }
        if (v < 0) v = 0;
        if (v > 1000) v = 1000;
        return v;
    }

    int seven[7];
    seven[0] = s->hole[0];
    seven[1] = s->hole[1];
    for (int b = 0; b < board_n; b++) seven[2 + b] = board[b];
    u32 mine = eval_best(seven, 2 + board_n);

    int v = HAND_CAT(mine) * 105 + HAND_KEY(mine, 0) * 6;

    /* A hand that is entirely the board is a hand everybody has. Betting it
       is betting on nobody holding a card, which is what a program that
       only looked at its own evaluation would do with great confidence. */
    if (board_n >= 5) {
        u32 shared = eval_best(board, board_n);
        if (mine == shared) v /= 4;
    }
    if (v > 1000) v = 1000;
    return v;
}

static void ai_move(int i) {
    seat *s = &tbl[i];
    int owed = level - s->street_bet;
    int pot = pot_total();
    int str = strength_of(i);
    int roll = (int)rng_below(&rg, 100);

    if (owed <= 0) {
        if (str > 620 && roll < 62) {
            int want = s->street_bet + pot / 2 + BIG_BLIND;
            act_raise(i, want);
        } else if (str < 260 && roll < 11) {
            act_raise(i, s->street_bet + pot / 2);   /* the bluff */
        } else {
            act_call(i);
        }
        return;
    }

    /* What share of the pot the call costs, which is the share of the time
       it has to be good to be worth making. */
    int need = owed * 100 / (pot + owed ? pot + owed : 1);
    int est = str / 10;

    if (est + roll % 12 < need - 4 && owed > s->stack / 25) { act_fold(i); return; }
    if (str > 720 && roll < 50) {
        act_raise(i, level + (pot / 2 > min_raise ? pot / 2 : min_raise));
        return;
    }
    act_call(i);
}

/* --- a hand ---------------------------------------------------------------- */

static void new_hand(void) {
    shuffle(&rg, deck, 52);
    deck_at = 0;
    board_n = 0;
    street = 0;
    level = BIG_BLIND;
    min_raise = BIG_BLIND;
    headline[0] = 0;
    hand_no++;

    for (int i = 0; i < SEATS; i++) {
        tbl[i].street_bet = 0;
        tbl[i].total_bet = 0;
        tbl[i].folded = tbl[i].out;
        tbl[i].allin = 0;
        tbl[i].acted = 0;
        tbl[i].won = 0;
        tbl[i].show = 0;
        tbl[i].shows[0] = 0;
        tbl[i].said[0] = 0;
        tbl[i].hole[0] = tbl[i].hole[1] = CARD_NONE;
    }

    do { button = (button + 1) % SEATS; } while (tbl[button].out);

    int at = ticks() + DEAL_TICKS;
    for (int round = 0; round < 2; round++)
        for (int step = 1; step <= SEATS; step++) {
            int i = (button + step) % SEATS;
            if (tbl[i].out) continue;
            tbl[i].hole[round] = draw_card();
            tbl[i].shown[round] = at;
            at += DEAL_TICKS;
        }

    /* Two left is not three with a gap in it. Heads up, the button posts
       the small blind, which puts it first to speak before the flop and
       last after it -- the opposite of every other seat count, and the
       reason a game that just walks left from the button plays the last
       two hands of every tournament wrong. */
    int sb, bb;
    if (live_count() == 2) { sb = button; bb = next_seat(button); }
    else                   { sb = next_seat(button); bb = next_seat(sb); }

    commit(sb, SMALL_BLIND); say(sb, "small blind", SMALL_BLIND);
    commit(bb, BIG_BLIND);   say(bb, "big blind", BIG_BLIND);

    turn = next_seat(bb);
    if (turn < 0) turn = bb;
    if (turn != 0) think_until = at + THINK_MIN + (int)rng_below(&rg, THINK_SPAN);
    state = PS_ACT;

    raise_to = level + min_raise;
}

static void new_game(void) {
    for (int i = 0; i < SEATS; i++) {
        tbl[i].stack = START_STACK;
        tbl[i].out = 0;
    }
    button = -1;
    hand_no = 0;
    new_hand();
}

/* --- drawing ---------------------------------------------------------------- */

#define FELT_DARK 0x0A3326
#define FELT_LIT  0x145B40
#define RAIL      0x3A2418

static int cards_landed(void) {
    int now = ticks();
    for (int i = 0; i < SEATS; i++) {
        if (tbl[i].out) continue;
        if (tbl[i].shown[0] > now || tbl[i].shown[1] > now) return 0;
    }
    for (int b = 0; b < board_n; b++) if (board_at[b] > now) return 0;
    return 1;
}

/* The table: a rail, a cloth, and a light over the middle of it.
 *
 * The lit part is three rounded rectangles laid on top of each other,
 * smaller each time, rather than a gradient worked out per row: a gradient
 * inside a rounded shape has to know where the rounding is and this does
 * not, because each layer is the same shape and the glow follows the
 * table's own outline for free.
 *
 * The colours are worked out first and then drawn solid. Drawing them
 * translucent is the obvious way and it cost the game: every layer was a
 * read, a blend and a write for every pixel under it, six times over the
 * whole window, and at the size a maximised window is that was most of a
 * frame. What the screen showed was the table and nothing else, because
 * the compositor kept catching the program still painting it -- a picture
 * of a poker game with no cards in it, which looks like a drawing bug and
 * was a speed one. Mixing two colours once and storing the answer is the
 * same picture for a fraction of the work. */
static void table_felt(surface *s, int w, int h) {
    rect(s, 0, 0, w, h, 0x14100C);

    int m = 22;
    int bot = h - 150;
    if (bot < m + 220) bot = m + 220;

    ui_round(s, m - 12, m - 12, w - (m - 12) * 2, bot - m + 24, 74, RAIL, 255);
    ui_round(s, m, m, w - m * 2, bot - m, 62, FELT_DARK, 255);

    u32 tone = FELT_DARK;
    for (int i = 0; i < 3; i++) {
        int ix = 58 + i * 58, iy = 32 + i * 32;
        if (w - m * 2 - ix * 2 < 120 || bot - m - iy * 2 < 90) break;
        tone = mix(tone, FELT_LIT, 70);
        ui_round(s, m + ix, m + iy, w - m * 2 - ix * 2, bot - m - iy * 2,
                 56, tone, 255);
    }
}

static void chips_at(surface *s, int cx, int cy, int amount) {
    if (amount <= 0) return;
    static const u32 face[4] = { 0xD64545, 0x3D8B4F, 0x2F5FA8, 0x7A4BB5 };
    int n = amount / 100;
    if (n > 4) n = 4;
    if (n < 1) n = 1;
    for (int i = 0; i < n; i++) {
        disc(s, cx, cy - i * 3, 9, 0x101010);
        disc(s, cx, cy - i * 3 - 1, 8, face[i % 4]);
    }
    char m[20];
    money(amount, m, sizeof(m));
    int tw = face_w(m, UI_FACE_SMALL);
    round_rect(s, cx + 12, cy - 9, tw + 10, 18, 5, 0x0A1C14);
    face_draw(s, cx + 17, cy - 8, m, 0xE8E8EE, UI_FACE_SMALL);
}

int main(void) {
    int win = win_create("Poker", 940, 660);
    if (win < 0) exit(1);
    win_allow_resize(win);

    rng_start(&rg);
    for (int i = 0; i < 52; i++) deck[i] = i;
    shuffle(&rg, deck, 52);

    tbl[0].name = "You";
    tbl[1].name = "Marge";
    tbl[2].name = "Bishop";
    tbl[3].name = "Cordelia";
    new_game();

    ui_input in;
    memset(&in, 0, sizeof(in));
    int last_published = 0;

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
            if (ev.type == WIN_EV_MOUSE || ev.type == WIN_EV_KEY)
                rng_stir(&rg, (u64)ticks() * 31 + (u64)ev.x * 7 + (u64)ev.y);
            ui_feed(&in, &ev);
        }
        if (closing) break;

        table_felt(&s, w, h);

        int cw = 54, ch = 76;
        int now = ticks();

        /* --- the board ---------------------------------------------------- */
        int bw = 5 * cw + 4 * 8;
        int bx = (w - bw) / 2, by = h / 2 - 110;
        if (by < 120) by = 120;

        for (int i = 0; i < 5; i++) {
            int x = bx + i * (cw + 8);
            if (i < board_n && board_at[i] <= now)
                card_face(&s, x, by, cw, ch, board[i]);
            else
                card_ring(&s, x, by, cw, ch, 0x1E6A4C);
        }

        int pot = pot_total();
        if (pot > 0) {
            char m[20];
            money(pot, m, sizeof(m));
            int pw = face_w(m, UI_FACE_HEAD) + 24;
            round_rect(&s, (w - pw) / 2, by + ch + 12, pw, 30, 8,
                       mix(FELT_DARK, 0x000000, 55));
            face_centred(&s, (w - pw) / 2, by + ch + 12, pw, 30, m,
                         0xFFE9A8, UI_FACE_HEAD);
        }

        if (headline[0]) {
            int hw = face_w(headline, UI_FACE_BODY) + 24;
            round_rect(&s, (w - hw) / 2, by - 44, hw, 28, 8,
                       mix(FELT_DARK, 0x000000, 55));
            face_centred(&s, (w - hw) / 2, by - 44, hw, 28, headline,
                         0xDCF0E4, UI_FACE_BODY);
        }

        /* --- the seats ----------------------------------------------------- */
        int sx[SEATS], sy[SEATS];
        sx[0] = w / 2 - cw - 4;      sy[0] = h - 250;
        sx[1] = 44;                  sy[1] = by - 20;
        sx[2] = w / 2 - cw - 4;      sy[2] = 46;
        sx[3] = w - 44 - cw * 2 - 8; sy[3] = by - 20;

        for (int i = 0; i < SEATS; i++) {
            seat *p = &tbl[i];
            int x = sx[i], y = sy[i];

            int mine = (i == 0);
            int reveal = mine || state == PS_SHOWDOWN || state == PS_GAMEOVER;

            if (!p->out) {
                for (int k = 0; k < 2; k++) {
                    int cx = x + k * (cw + 6);
                    if (p->hole[k] < 0 || p->shown[k] > now) continue;
                    if (p->folded) {
                        card_ring(&s, cx, y, cw, ch, 0x1A5540);
                    } else if (reveal) {
                        card_face(&s, cx, y, cw, ch, p->hole[k]);
                    } else {
                        card_back(&s, cx, y, cw, ch, t.accent);
                    }
                }
            }

            /* The plate: who it is and what they have left. */
            int pw = cw * 2 + 6;
            int py = y + ch + 6;
            u32 plate = (turn == i && state == PS_ACT)
                      ? mix(t.accent, 0x000000, 35)
                      : mix(FELT_DARK, 0x000000, 55);
            round_rect(&s, x, py, pw, 38, 8, plate);
            if (turn == i && state == PS_ACT)
                card_ring(&s, x, py, pw, 38, mix(t.accent, 0xFFFFFF, 60));

            face_draw(&s, x + 8, py + 3, p->name,
                      p->out ? 0x7A8A82 : 0xFFFFFF, UI_FACE_SMALL);
            char m[20];
            money(p->stack, m, sizeof(m));
            face_draw(&s, x + 8, py + 19, p->out ? "out" : m,
                      p->out ? 0x7A8A82 : 0xA9E0C4, UI_FACE_SMALL);

            if (i == button && !p->out) {
                disc(&s, x + pw - 12, py + 12, 10, 0xF2F2F5);
                face_centred(&s, x + pw - 22, py + 4, 20, 16, "D",
                             0x202028, UI_FACE_SMALL);
            }

            /* Two things want the space beside a seat: what they just did,
               and what they pushed forward doing it. They went in the same
               place to begin with and drew on top of each other, so each
               seat now has a side -- the label out from the table, the
               chips in towards the middle, which is where chips go. */
            int label_y, chip_x, chip_y;
            if (i == 2)      { label_y = py + 44;  chip_x = x + pw / 2;
                               chip_y = py + 82; }
            else if (i == 0) { label_y = y - 62;   chip_x = x + pw / 2;
                               chip_y = y - 30; }
            else if (i == 1) { label_y = y - 26;   chip_x = x + pw + 26;
                               chip_y = y + ch / 2; }
            else             { label_y = y - 26;   chip_x = x - 30;
                               chip_y = y + ch / 2; }

            const char *tag = 0;
            u32 tag_bg = mix(FELT_DARK, 0x000000, 70);
            if (p->shows[0] && reveal && !p->folded) {
                tag = p->shows;
                if (p->won) tag_bg = 0x2E7D52;
            } else if (p->said[0] && !p->out) {
                tag = p->said;
            }
            if (tag) {
                int sw = face_w(tag, UI_FACE_SMALL) + 14;
                int lx = x + pw / 2 - sw / 2;
                if (lx < 4) lx = 4;
                if (lx + sw > w - 4) lx = w - 4 - sw;
                round_rect(&s, lx, label_y, sw, 20, 6, tag_bg);
                face_draw(&s, lx + 7, label_y + 2, tag, 0xE2F0E8, UI_FACE_SMALL);
            }

            if (p->street_bet > 0) chips_at(&s, chip_x, chip_y, p->street_bet);
        }

        /* --- what you can do ------------------------------------------------ */
        int by2 = h - 44;
        int owed = level - tbl[0].street_bet;
        int my_turn = (state == PS_ACT && turn == 0 && cards_landed());

        if (my_turn) {
            int most = tbl[0].street_bet + tbl[0].stack;
            int least = level + min_raise;
            if (least > most) least = most;
            if (raise_to < least) raise_to = least;
            if (raise_to > most) raise_to = most;

            if (ui_button(&s, &in, &t, 16, by2, 90, "Fold")) act_fold(0);

            char cl[28];
            if (owed <= 0) { cl[0] = 'C'; cl[1] = 'h'; cl[2] = 'e'; cl[3] = 'c';
                             cl[4] = 'k'; cl[5] = 0; }
            else {
                int at = 0;
                const char *p = owed >= tbl[0].stack ? "All in " : "Call ";
                while (*p) cl[at++] = *p++;
                char m[20];
                money(owed >= tbl[0].stack ? tbl[0].stack : owed, m, sizeof(m));
                for (const char *q = m; *q && at < 26; q++) cl[at++] = *q;
                cl[at] = 0;
            }
            if (ui_button(&s, &in, &t, 112, by2, 132, cl)) act_call(0);

            if (most > level) {
                int slx = 260, slw = w - 260 - 190;
                if (slw < 80) slw = 80;
                raise_to = ui_slider(&s, &in, &t, slx, by2 + 6, slw,
                                     raise_to, least, most);

                /* The three bets anybody actually makes, so the slider is
                   for the fourth. */
                int qy = by2 - 30;
                if (ui_button(&s, &in, &t, slx, qy, 58, "Min")) raise_to = least;
                if (ui_button(&s, &in, &t, slx + 64, qy, 58, "Half"))
                    raise_to = level + pot / 2;
                if (ui_button(&s, &in, &t, slx + 128, qy, 58, "Pot"))
                    raise_to = level + pot;
                if (ui_button(&s, &in, &t, slx + 192, qy, 70, "All in"))
                    raise_to = most;
                if (raise_to < least) raise_to = least;
                if (raise_to > most) raise_to = most;

                char rl[28];
                int at = 0;
                const char *p = (raise_to >= most) ? "All in " :
                                (level > 0 ? "Raise to " : "Bet ");
                while (*p) rl[at++] = *p++;
                char m[20];
                money(raise_to, m, sizeof(m));
                for (const char *q = m; *q && at < 26; q++) rl[at++] = *q;
                rl[at] = 0;
                if (ui_button_primary(&s, &in, &t, w - 176, by2, 160, rl))
                    act_raise(0, raise_to);
            }
        } else if (state == PS_OVER || state == PS_SHOWDOWN) {
            if (ui_button_primary(&s, &in, &t, w - 176, by2, 160, "Next hand")) {
                new_hand();
                raise_to = level + min_raise;
            }
        } else if (state == PS_GAMEOVER) {
            if (ui_button_primary(&s, &in, &t, w - 176, by2, 160, "New game"))
                new_game();
        }

        /* --- keys ------------------------------------------------------------ */
        if (in.key) {
            u32 k = in.key;
            if (k >= 'A' && k <= 'Z') k += 32;
            if (my_turn) {
                if (k == 'f') act_fold(0);
                else if (k == 'c' || k == '\n') act_call(0);
                else if (k == 'r') act_raise(0, raise_to);
                else if (k == 'a') act_raise(0, tbl[0].street_bet + tbl[0].stack);
            } else if (k == '\n' || k == ' ') {
                if (state == PS_OVER || state == PS_SHOWDOWN) {
                    new_hand(); raise_to = level + min_raise;
                } else if (state == PS_GAMEOVER) new_game();
            }
        }

        /* --- the opponents take their turn ----------------------------------- */
        if (state == PS_ACT && turn > 0 && cards_landed()
            && ticks() >= think_until)
            ai_move(turn);

        if (ticks() - last_published > 25) {
            last_published = ticks();
            char buf[512];
            int n = 0;
            #define PUT(str) do { const char *p_ = (str); \
                while (*p_ && n < (int)sizeof(buf) - 1) buf[n++] = *p_++; } while (0)
            PUT("Poker hand "); {
                char d[8]; int a = 0, v = hand_no;
                if (!v) d[a++] = '0';
                char r2[8]; int b2 = 0;
                while (v) { r2[b2++] = (char)('0' + v % 10); v /= 10; }
                while (b2) d[a++] = r2[--b2];
                d[a] = 0; PUT(d);
            }
            PUT("\nboard ");
            for (int i = 0; i < board_n; i++) { PUT(RANK_NAME[RANK(board[i])]); PUT(" "); }
            PUT("\nyou ");
            if (tbl[0].hole[0] >= 0) { PUT(RANK_NAME[RANK(tbl[0].hole[0])]); PUT(" ");
                                       PUT(RANK_NAME[RANK(tbl[0].hole[1])]); }
            PUT("\n");
            for (int i = 0; i < SEATS; i++) {
                PUT(tbl[i].name); PUT(" ");
                if (tbl[i].out) PUT("out");
                else { char m[20]; money(tbl[i].stack, m, sizeof(m)); PUT(m);
                       if (tbl[i].said[0]) { PUT(" "); PUT(tbl[i].said); } }
                PUT("\n");
            }
            if (headline[0]) { PUT(headline); PUT("\n"); }
            #undef PUT
            win_set_text(win, buf, n);
        }

        win_commit(win);
        sleep_ms(16);
    }

    win_close(win);
    exit(0);
}
