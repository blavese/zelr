/* Blackjack, with the rules written down rather than implied.
 *
 * Six decks in the shoe, dealt until three quarters of it is gone and then
 * shuffled. The dealer stands on every seventeen, including a soft one.
 * Blackjack pays three to two. Double on any two cards and after a split.
 * Split up to four hands; split aces take one card each and that is the
 * hand. Insurance when the dealer shows an ace, paying two to one. Late
 * surrender, which is half the bet back and the hand over.
 *
 * Every one of those is a decision a real table makes differently, and a
 * game that does not say which it made is a game you cannot tell is wrong.
 * The two that cost the player most are here on purpose: the dealer peeks
 * at the hole card when a ten or an ace is showing, so nobody doubles into
 * a blackjack that was already there, and an odd bet paying three to two
 * rounds down, which is what a table does and is not in the player's
 * favour.
 *
 * What is not here: no side bets, no even money as a separate offer (it is
 * the same thing as insuring a blackjack and would be a second button for
 * one decision), and no card counting help. The shoe position is on screen
 * because it says when the shuffle is coming, not to be counted with.
 */
#include "zelr.h"
#include "ui.h"
#include "cards.h"

#define PACKS       6
#define SHOE        (PACKS * 52)
#define CUT         (SHOE * 3 / 4)
#define HAND_MAX    12
#define HANDS_MAX   4

#define DEAL_TICKS  9          /* between one card landing and the next */

typedef struct {
    int card[HAND_MAX];
    int shown[HAND_MAX];       /* the tick it becomes visible */
    int n;
    int bet;
    int doubled;
    int done;                  /* stood, bust, doubled out or surrendered */
    int surrendered;
    int from_split;
    int split_ace;
    int payout;                /* what it returned, once the hand is over */
    const char *verdict;
} hand;

enum { ST_BET, ST_INSURE, ST_PLAY, ST_DEALER, ST_OVER };

static rng   r;
static int   shoe[SHOE];
static int   shoe_at;
static int   shuffled_at;      /* how many were left when it was shuffled */

static hand  player[HANDS_MAX];
static int   hands;            /* how many the player has */
static int   active;           /* which one is being played */

static hand  dealer;
static int   hole_shown;

static int   bank = 1000;
static int   wager = 25;       /* what the next hand will be for */
static int   insurance;
static int   state = ST_BET;
static int   last_swing;       /* what the whole round won or lost */
static char  note[96];

/* --- the shoe ------------------------------------------------------------ */

static void reshuffle(void) {
    deck_fill(shoe, PACKS);
    shuffle(&r, shoe, SHOE);
    shoe_at = 0;
    shuffled_at = 1;
}

static int draw_card(void) {
    if (shoe_at >= SHOE) reshuffle();
    return shoe[shoe_at++];
}

/* --- what a hand is worth ------------------------------------------------
 *
 * Aces count eleven until that busts, and then as many of them as it takes
 * count one. Counting them all as one and then adding ten back is the same
 * arithmetic and reads as though an ace were worth one, which is the thing
 * a beginner gets wrong and a program should not repeat. */
static void value_of(const int *c, int n, int *total, int *soft) {
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

static int total_of(const hand *h) { int t; value_of(h->card, h->n, &t, 0); return t; }

static int is_blackjack(const hand *h) {
    return h->n == 2 && !h->from_split && total_of(h) == 21;
}

/* Moving a hand along the array, a byte at a time through a volatile
   pointer.
 *
 * `player[k] = player[k - 1]` is the obvious line and it does not link: at
 * -O2 clang lowers a struct assignment of this size into a call to memcpy,
 * and there is no memcpy in this system to call -- zelr.h has one, but it
 * is a static inline and the call the compiler emitted is to the symbol.
 * Writing the loop by hand is no better on its own, because the same
 * optimiser recognises a byte copying loop and turns it back into the call
 * it just refused to resolve. The volatile is what stops it, and it is the
 * same fix the kernel's own byte movers carry for the same reason. */
static void move_hand(hand *d, const hand *s) {
    volatile unsigned char *a = (volatile unsigned char *)d;
    const volatile unsigned char *b = (const volatile unsigned char *)s;
    for (unsigned i = 0; i < sizeof(hand); i++) a[i] = b[i];
}

static void give(hand *h, int at_tick) {
    if (h->n >= HAND_MAX) return;
    h->card[h->n] = draw_card();
    h->shown[h->n] = at_tick;
    h->n++;
}

/* Whether everything dealt has landed. Nothing can be decided while a card
   is still on its way: a hit that could be taken before the card arrived
   would be a hit on a hand the player has not seen. */
static int settled_on_screen(void) {
    int now = ticks();
    for (int i = 0; i < dealer.n; i++) if (dealer.shown[i] > now) return 0;
    for (int k = 0; k < hands; k++)
        for (int i = 0; i < player[k].n; i++)
            if (player[k].shown[i] > now) return 0;
    return 1;
}

/* --- settling ------------------------------------------------------------ */

static void pay(hand *h, int chips, const char *why) {
    h->payout = chips;
    h->verdict = why;
    bank += chips;
    last_swing += chips - h->bet;
}

static void settle(void) {
    int dt = total_of(&dealer);
    int dbj = is_blackjack(&dealer);
    int dbust = dt > 21;

    for (int k = 0; k < hands; k++) {
        hand *h = &player[k];
        if (h->payout) continue;              /* already dealt with */

        if (h->surrendered) { pay(h, h->bet / 2, "surrendered"); continue; }

        int pt = total_of(h);
        if (pt > 21)                 { pay(h, 0, "bust"); continue; }
        if (is_blackjack(h) && !dbj) { pay(h, h->bet + h->bet * 3 / 2,
                                           "blackjack"); continue; }
        if (dbj)                     { pay(h, is_blackjack(h) ? h->bet : 0,
                                           is_blackjack(h) ? "push"
                                                           : "dealer blackjack");
                                       continue; }
        if (dbust)                   { pay(h, h->bet * 2, "dealer bust"); continue; }
        if (pt > dt)                 { pay(h, h->bet * 2, "won"); continue; }
        if (pt < dt)                 { pay(h, 0, "lost"); continue; }
        pay(h, h->bet, "push");
    }

    if (insurance) {
        if (dbj) { bank += insurance * 3; last_swing += insurance * 2; }
        else     { last_swing -= insurance; }
        insurance = 0;
    }
    state = ST_OVER;
}

static void dealer_plays(void) {
    /* The hole card turns over first, and then the draws are spaced out the
       way the deal was, so what happened is watchable rather than a total
       appearing all at once. */
    int at = ticks() + DEAL_TICKS;
    hole_shown = 1;

    int t, soft;
    for (;;) {
        value_of(dealer.card, dealer.n, &t, &soft);
        if (t >= 17) break;                    /* stands on all seventeens */
        give(&dealer, at);
        at += DEAL_TICKS;
    }
    state = ST_DEALER;
}

/* Whether any hand is still worth playing against. All of them bust or
   surrendered means the dealer does not draw at all, which is a rule and
   also the difference between a game and a slideshow. */
static int anything_live(void) {
    for (int k = 0; k < hands; k++) {
        const hand *h = &player[k];
        if (!h->surrendered && total_of(h) <= 21) return 1;
    }
    return 0;
}

static void finish_hand(void);

static void next_hand(void) {
    while (active < hands && player[active].done) active++;
    if (active < hands) {
        /* A split hand starts with one card and is given its second here,
           rather than at the moment of the split, so the two hands are
           filled one after the other the way a dealer fills them. */
        if (player[active].n == 1) {
            give(&player[active], ticks() + DEAL_TICKS);
            if (player[active].split_ace) player[active].done = 1;
            else if (total_of(&player[active]) == 21) player[active].done = 1;
            if (player[active].done) { next_hand(); return; }
        }
        return;
    }
    finish_hand();
}

static void finish_hand(void) {
    if (anything_live()) dealer_plays();
    else { hole_shown = 1; settle(); }
}

static void stand(void) {
    player[active].done = 1;
    active++;
    next_hand();
}

/* --- what the player can do ---------------------------------------------- */

static int can_hit(void) {
    const hand *h = &player[active];
    return state == ST_PLAY && !h->done && !h->split_ace && total_of(h) < 21;
}

static int can_double(void) {
    const hand *h = &player[active];
    return state == ST_PLAY && !h->done && h->n == 2 && !h->split_ace
           && bank >= h->bet;
}

static int can_split(void) {
    const hand *h = &player[active];
    if (state != ST_PLAY || h->done || h->n != 2 || hands >= HANDS_MAX) return 0;
    if (bank < h->bet) return 0;
    /* By rank, so a king and a queen are not a pair. Two tens are, because
       they are the same rank; a ten and a jack are not, and a table that
       splits any two ten-valued cards is a different rule than this one. */
    return RANK(h->card[0]) == RANK(h->card[1]);
}

static int can_surrender(void) {
    const hand *h = &player[active];
    return state == ST_PLAY && !h->done && h->n == 2 && hands == 1
           && !h->from_split;
}

static void hit(void) {
    hand *h = &player[active];
    give(h, ticks() + DEAL_TICKS);
    if (total_of(h) >= 21) { h->done = 1; active++; next_hand(); }
}

static void double_down(void) {
    hand *h = &player[active];
    bank -= h->bet;
    h->bet *= 2;
    h->doubled = 1;
    give(h, ticks() + DEAL_TICKS);
    h->done = 1;
    active++;
    next_hand();
}

static void split(void) {
    hand *h = &player[active];

    for (int k = hands; k > active + 1; k--) move_hand(&player[k], &player[k - 1]);
    hands++;

    hand *b = &player[active + 1];
    memset(b, 0, sizeof(*b));
    b->card[0] = h->card[1];
    b->shown[0] = ticks();
    b->n = 1;
    b->bet = h->bet;
    b->from_split = 1;
    b->split_ace = RANK(h->card[1]) == R_ACE;

    h->n = 1;
    h->from_split = 1;
    h->split_ace = RANK(h->card[0]) == R_ACE;
    bank -= h->bet;

    give(h, ticks() + DEAL_TICKS);
    if (h->split_ace) { h->done = 1; active++; next_hand(); }
}

static void surrender(void) {
    player[active].surrendered = 1;
    player[active].done = 1;
    active++;
    next_hand();
}

/* --- a round ------------------------------------------------------------- */

static void deal_round(void) {
    if (wager > bank) wager = bank;
    if (wager <= 0) return;

    if (!shuffled_at || shoe_at >= CUT) reshuffle();

    memset(player, 0, sizeof(player));
    memset(&dealer, 0, sizeof(dealer));
    hands = 1;
    active = 0;
    insurance = 0;
    hole_shown = 0;
    last_swing = 0;
    note[0] = 0;

    player[0].bet = wager;
    bank -= wager;

    /* Round the table and then round again, which is the order a hand is
       actually dealt and is why the dealer's second card is the last thing
       on the felt. */
    int at = ticks() + DEAL_TICKS;
    give(&player[0], at); at += DEAL_TICKS;
    give(&dealer,    at); at += DEAL_TICKS;
    give(&player[0], at); at += DEAL_TICKS;
    give(&dealer,    at);

    state = ST_PLAY;

    /* The dealer looks at the hole card when the up card could make a
       blackjack, and the hand is over before anybody doubles into it. An
       ace is asked about first, because that is where insurance is offered
       and the offer has to come before the peek is acted on. */
    int up = RANK(dealer.card[0]);
    if (up == R_ACE && bank >= wager / 2) { state = ST_INSURE; return; }
    if (up == R_ACE || up >= R_TEN) {
        if (is_blackjack(&dealer)) { hole_shown = 1; settle(); return; }
    }
    if (is_blackjack(&player[0])) { player[0].done = 1; finish_hand(); }
}

static void answer_insurance(int take) {
    insurance = take ? wager / 2 : 0;
    if (insurance) bank -= insurance;
    state = ST_PLAY;

    if (is_blackjack(&dealer)) { hole_shown = 1; settle(); return; }
    if (is_blackjack(&player[0])) { player[0].done = 1; finish_hand(); }
}

/* --- what the window says it is showing ----------------------------------
 *
 * So that ctrl+f can find a word in it. A window is a rectangle of pixels
 * and the desktop cannot read one, so a program that wants to be searchable
 * has to say what is on it. */
static void publish(int win) {
    char buf[512];
    int n = 0;

    #define PUT(s) do { const char *p_ = (s); \
        while (*p_ && n < (int)sizeof(buf) - 1) buf[n++] = *p_++; } while (0)
    #define PUTC(c) do { if (n < (int)sizeof(buf) - 1) buf[n++] = (char)(c); } while (0)

    char m[24];
    PUT("Blackjack bankroll "); money(bank, m, sizeof(m)); PUT(m);
    PUT(" bet "); money(wager, m, sizeof(m)); PUT(m); PUTC('\n');

    PUT("dealer ");
    for (int i = 0; i < dealer.n; i++) {
        if (i == 1 && !hole_shown) { PUT("hidden "); continue; }
        PUT(RANK_NAME[RANK(dealer.card[i])]); PUTC(' ');
    }
    PUTC('\n');

    for (int k = 0; k < hands; k++) {
        PUT("hand ");
        for (int i = 0; i < player[k].n; i++) {
            PUT(RANK_NAME[RANK(player[k].card[i])]); PUTC(' ');
        }
        if (player[k].verdict) { PUT("- "); PUT(player[k].verdict); }
        PUTC('\n');
    }
    if (note[0]) { PUT(note); PUTC('\n'); }
    #undef PUT
    #undef PUTC

    win_set_text(win, buf, n);
}

/* --- drawing -------------------------------------------------------------- */

#define FELT_DARK  0x0B3A28
#define FELT_LIT   0x14543A

static void baize(surface *s, int w, int h) {
    /* A cloth rather than a flat green: lighter in the middle, where a lamp
       over a table puts it, and darker at the edges. Cheap to do and it is
       the difference between a table and a rectangle. */
    int span = h > 0 ? h : 1;
    for (int y = 0; y < h; y++) {
        int d = y - h / 3;
        if (d < 0) d = -d;
        /* Out of two hundred rather than out of seventy. The step between
           one row's colour and the next is what the eye sees, and seventy
           levels spread down a maximised window is a step every ten rows,
           which reads as bands of green rather than as a cloth. */
        int a = 200 - d * 200 / span;
        if (a < 0) a = 0;
        rect(s, 0, y, w, 1, mix(FELT_DARK, FELT_LIT, a));
    }
}

/* How far along a fanned hand each card sits. Enough that every card's
   corner index is readable, which is the whole reason a hand is fanned,
   and no more: a hand spread to its full width walks off the table as soon
   as somebody draws to a soft sixteen. */
static int fan_step(int cw) { return cw * 5 / 9; }

static int hand_width(const hand *h, int cw) {
    int n = h->n > 0 ? h->n : 1;
    return fan_step(cw) * (n - 1) + cw;
}

static void draw_hand(surface *s, const hand *h, int x, int y, int cw, int ch,
                      int hidden_from, u32 accent, int highlight) {
    int now = ticks();
    int step = fan_step(cw);

    if (highlight) {
        int wide = hand_width(h, cw);
        card_ring(s, x - 5, y - 5, wide + 10, ch + 10,
                  mix(accent, 0xFFFFFF, 60));
    }

    for (int i = 0; i < h->n; i++) {
        if (h->shown[i] > now) break;
        int cx = x + i * step;
        /* A card sits on the one before it, so the shadow goes down and to
           the right of each rather than around the whole fan. */
        round_rect(s, cx + 2, y + 3, cw, ch, 6, 0x062418);
        if (i >= hidden_from) card_back(s, cx, y, cw, ch, accent);
        else                  card_face(s, cx, y, cw, ch, h->card[i]);
    }
}

static void count_label(surface *s, const hand *h, int x, int y, int upto,
                        u32 fg, u32 panel) {
    int now = ticks(), shown = 0;
    for (int i = 0; i < h->n && i < upto; i++) if (h->shown[i] <= now) shown++;
    if (!shown) return;

    int t, soft;
    value_of(h->card, shown, &t, &soft);

    char b[16];
    int n = 0;
    if (soft && t <= 21) { b[n++] = 's'; b[n++] = 'o'; b[n++] = 'f'; b[n++] = 't'; b[n++] = ' '; }
    if (t >= 100) b[n++] = (char)('0' + t / 100);
    if (t >= 10)  b[n++] = (char)('0' + t / 10 % 10);
    b[n++] = (char)('0' + t % 10);
    b[n] = 0;

    int w = face_w(b, UI_FACE_BOLD) + 14;
    round_rect(s, x, y, w, 22, 6, panel);
    face_centred(s, x, y, w, 22, b, t > 21 ? 0xFF6B6B : fg, UI_FACE_BOLD);
}

/* A chip, which is a disc with a ring of dashes round it. */
static void chip(surface *s, int cx, int cy, int rad, u32 face, u32 edge) {
    disc(s, cx, cy, rad, edge);
    disc(s, cx, cy, rad - 3, face);
    for (int i = 0; i < 6; i++) {
        /* Six marks, at the corners of a hexagon, worked out from a small
           table rather than from a sine nobody here has. */
        static const int dx[6] = { 0, 87, 87, 0, -87, -87 };
        static const int dy[6] = { -100, -50, 50, 100, 50, -50 };
        int px = cx + dx[i] * (rad - 1) / 100;
        int py = cy + dy[i] * (rad - 1) / 100;
        disc(s, px, py, 2, edge);
    }
    disc(s, cx, cy, rad / 2, mix(face, 0xFFFFFF, 40));
}

static const struct { int v; u32 face, edge; } CHIPS[] = {
    {   5, 0xD64545, 0x8C1F1F },
    {  25, 0x3D8B4F, 0x1E5730 },
    { 100, 0x2F5FA8, 0x18386A },
    { 500, 0x7A4BB5, 0x452A6B },
};
#define N_CHIPS ((int)(sizeof(CHIPS) / sizeof(CHIPS[0])))

int main(void) {
    int win = win_create("Blackjack", 800, 560);
    if (win < 0) exit(1);
    win_allow_resize(win);

    rng_start(&r);
    reshuffle();

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
            /* Every event stirs the generator. The tick a click lands on is
               a number the player produced and nobody can arrange, which is
               what keeps two games started on the same second apart. */
            if (ev.type == WIN_EV_MOUSE || ev.type == WIN_EV_KEY)
                rng_stir(&r, (u64)ticks() * 31 + (u64)ev.x * 7 + (u64)ev.y);
            ui_feed(&in, &ev);
        }
        if (closing) break;

        baize(&s, w, h);

        int cw = 62, ch = 88;
        if (w < 640) { cw = 48; ch = 68; }

        /* --- the money, along the top ---------------------------------- */
        rect(&s, 0, 0, w, 34, mix(FELT_DARK, 0x000000, 60));
        char b[32];
        money(bank, b, sizeof(b));
        face_draw(&s, 12, 8, "Bankroll", 0x9FD8BE, UI_FACE_SMALL);
        face_draw(&s, 12 + face_w("Bankroll", UI_FACE_SMALL) + 8, 6, b,
                  0xFFFFFF, UI_FACE_BOLD);

        /* How much of the shoe is left, as a bar. It says when the shuffle
           is coming, which changes nothing about the odds and is the sort
           of thing a player at a real table can see. */
        int left = SHOE - shoe_at;
        int bw = 120;
        rect(&s, w - bw - 12, 12, bw, 10, mix(FELT_DARK, 0x000000, 40));
        rect(&s, w - bw - 12, 12, bw * left / SHOE, 10, 0x6FBF95);
        face_draw(&s, w - bw - 12 - face_w("shoe", UI_FACE_SMALL) - 8, 8,
                  "shoe", 0x9FD8BE, UI_FACE_SMALL);

        /* --- the dealer ------------------------------------------------- */
        int dy = 58;
        if (dealer.n) {
            int dx = (w - hand_width(&dealer, cw)) / 2;
            face_draw(&s, dx, dy - 22, "Dealer", 0xBFE6D2, UI_FACE_SMALL);
            draw_hand(&s, &dealer, dx, dy, cw, ch, hole_shown ? 99 : 1,
                      t.accent, 0);
            count_label(&s, &dealer, dx, dy + ch + 6,
                        hole_shown ? dealer.n : 1, 0xFFFFFF,
                        mix(FELT_DARK, 0x000000, 50));
        } else {
            face_centred(&s, 0, dy + ch / 2 - 10, w, 20,
                         "Place a bet and deal", 0x6FA88C, UI_FACE_BODY);
        }

        /* --- the player's hands ----------------------------------------- */
        int py = h - ch - 128;
        if (py < dy + ch + 48) py = dy + ch + 48;

        /* All the hands together, centred, rather than one column of the
           window each: with one hand that is the middle of the table, and
           with four it is still symmetrical, which a fixed share of the
           width is not. */
        int gap = 26, spread = 0;
        for (int k = 0; k < hands; k++)
            spread += hand_width(&player[k], cw) + (k ? gap : 0);
        int hx0 = (w - spread) / 2;
        if (hx0 < 12) hx0 = 12;

        for (int k = 0; k < hands; k++) {
            int hx = hx0;
            for (int j = 0; j < k; j++) hx += hand_width(&player[j], cw) + gap;
            draw_hand(&s, &player[k], hx, py, cw, ch, 99, t.accent,
                      state == ST_PLAY && k == active && hands > 1);
            count_label(&s, &player[k], hx, py + ch + 6, player[k].n,
                        0xFFFFFF, mix(FELT_DARK, 0x000000, 50));

            char bet[24];
            money(player[k].bet, bet, sizeof(bet));
            face_draw(&s, hx + 54, py + ch + 8, bet, 0xBFE6D2, UI_FACE_SMALL);

            if (player[k].verdict) {
                int vw = face_w(player[k].verdict, UI_FACE_BOLD) + 16;
                u32 col = player[k].payout > player[k].bet ? 0x2E7D52
                        : player[k].payout ? 0x4A4A55 : 0x8C2B2B;
                round_rect(&s, hx, py - 30, vw, 24, 6, col);
                face_centred(&s, hx, py - 30, vw, 24, player[k].verdict,
                             0xFFFFFF, UI_FACE_BOLD);
            }
        }

        /* --- the controls ------------------------------------------------ */
        int by = h - 44;
        int bx = 16;
        int bwid = 92;

        if (state == ST_BET || state == ST_OVER) {
            /* Chips to bet with, and the one thing to press. */
            for (int i = 0; i < N_CHIPS; i++) {
                int cx = bx + 30 + i * 62, cy = by + 17;
                int over = ui_hit(&in, cx - 26, cy - 26, 52, 52);
                chip(&s, cx, cy, over ? 25 : 23, CHIPS[i].face, CHIPS[i].edge);
                char cl[8];
                int n = 0, v = CHIPS[i].v;
                if (v >= 100) cl[n++] = (char)('0' + v / 100);
                if (v >= 10)  cl[n++] = (char)('0' + v / 10 % 10);
                cl[n++] = (char)('0' + v % 10);
                cl[n] = 0;
                face_centred(&s, cx - 26, cy - 8, 52, 16, cl, 0xFFFFFF,
                             UI_FACE_BOLD);
                if (over && in.released) {
                    in.released = 0;
                    if (wager + CHIPS[i].v <= bank) wager += CHIPS[i].v;
                    else wager = bank;
                }
            }

            int tx = bx + 30 + N_CHIPS * 62;
            char wtext[24];
            money(wager, wtext, sizeof(wtext));
            face_draw(&s, tx, by + 8, wtext, 0xFFFFFF, UI_FACE_HEAD);

            int rx = w - 16 - bwid * 2 - UI_GAP;
            if (ui_button(&s, &in, &t, rx, by, bwid, "Clear")) wager = 0;
            if (ui_button_primary(&s, &in, &t, rx + bwid + UI_GAP, by,
                                  bwid, "Deal")
                && wager > 0 && wager <= bank)
                deal_round();
        } else if (state == ST_INSURE) {
            face_draw(&s, bx, by + 8, "Dealer shows an ace. Insurance?",
                      0xFFFFFF, UI_FACE_BODY);
            int rx = w - 16 - bwid * 2 - UI_GAP;
            if (ui_button(&s, &in, &t, rx, by, bwid, "No thanks"))
                answer_insurance(0);
            if (ui_button_primary(&s, &in, &t, rx + bwid + UI_GAP, by,
                                  bwid, "Insure"))
                answer_insurance(1);
        } else if (state == ST_PLAY && settled_on_screen()) {
            int x = bx;
            if (can_hit() && ui_button_primary(&s, &in, &t, x, by, bwid, "Hit")) hit();
            x += bwid + UI_GAP;
            if (ui_button(&s, &in, &t, x, by, bwid, "Stand")) stand();
            x += bwid + UI_GAP;
            if (can_double() && ui_button(&s, &in, &t, x, by, bwid, "Double")) double_down();
            x += bwid + UI_GAP;
            if (can_split() && ui_button(&s, &in, &t, x, by, bwid, "Split")) split();
            x += bwid + UI_GAP;
            if (can_surrender() && ui_button(&s, &in, &t, x, by, bwid + 20, "Surrender")) surrender();
        }

        /* --- what just happened ------------------------------------------ */
        if (state == ST_OVER) {
            const char *said = last_swing > 0 ? "You win"
                             : last_swing < 0 ? "You lose" : "Push";
            char line[48];
            int n = 0;
            for (const char *p = said; *p && n < 20; p++) line[n++] = *p;
            if (last_swing) {
                line[n++] = ' ';
                char amt[24];
                money(last_swing < 0 ? -last_swing : last_swing, amt, sizeof(amt));
                for (const char *p = amt; *p && n < 44; p++) line[n++] = *p;
            }
            line[n] = 0;
            int lw = face_w(line, UI_FACE_HEAD) + 28;
            round_rect(&s, (w - lw) / 2, py - 74, lw, 34, 8,
                       mix(FELT_DARK, 0x000000, 55));
            face_centred(&s, (w - lw) / 2, py - 74, lw, 34, line,
                         last_swing > 0 ? 0x7FE0A8
                         : last_swing < 0 ? 0xFF9B9B : 0xE8E8EE,
                         UI_FACE_HEAD);
        }

        if (bank <= 0 && state == ST_OVER) {
            face_centred(&s, 0, py - 110, w, 24,
                         "Out of chips. The house always was going to win.",
                         0xFFD9A0, UI_FACE_BODY);
        }

        /* --- keys --------------------------------------------------------- */
        if (in.key) {
            u32 k = in.key;
            if (k >= 'A' && k <= 'Z') k += 32;
            if (state == ST_BET || state == ST_OVER) {
                if (k == '\n' || k == ' ') { if (wager > 0 && wager <= bank) deal_round(); }
                else if (k >= '1' && k <= '4') {
                    int v = CHIPS[k - '1'].v;
                    if (wager + v <= bank) wager += v; else wager = bank;
                } else if (k == 8 || k == 27) wager = 0;
            } else if (state == ST_INSURE) {
                if (k == 'y') answer_insurance(1);
                else if (k == 'n' || k == 27) answer_insurance(0);
            } else if (state == ST_PLAY && settled_on_screen()) {
                if (k == 'h' && can_hit()) hit();
                else if (k == 's') stand();
                else if (k == 'd' && can_double()) double_down();
                else if (k == 'p' && can_split()) split();
                else if (k == 'r' && can_surrender()) surrender();
            }
        }

        /* The dealer's draws land on a timer, so the state only moves on
           once the last of them is on the felt. */
        if (state == ST_DEALER && settled_on_screen()) settle();

        if (ticks() - last_published > 25) {
            last_published = ticks();
            publish(win);
        }

        win_commit(win);
        sleep_ms(16);
    }

    win_close(win);
    exit(0);
}
