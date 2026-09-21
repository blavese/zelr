"""The two card games, on the screen, doing what the rules say.

The ranking of poker hands and the arithmetic of an ace are checked inside
the machine by `cardtest`, against hands written out by hand, and that is
where the rules are proved. What cannot be proved from in there is that any
of it reaches a window: that the launcher has a Games row at all, that a
program starts from it, that a deal puts cards on the cloth, and that the
dealer plays its hand out after you stand.

So this counts pixels, and it counts the one colour nothing else on a green
table can be. A playing card here is white; the felt, the rail, the chips
and every label on them are not. Counting white inside the rectangle a hand
is dealt into therefore asks exactly the question -- were cards dealt, and
are they where the program thinks they are -- and it cannot be answered
right by a program that drew nothing and said it had.

  python tools/gamecheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

DISK = os.path.join(ROOT, "gamecheck.%d.img" % os.getpid())

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H, TASKBAR_GAP = 44, 14
DOCK_SIDE = 16
PANEL_Y = SCREEN_H - TASKBAR_H - TASKBAR_GAP

BADGE_W = 76
LAUNCHER = (DOCK_SIDE + 16 + BADGE_W // 2, PANEL_Y + TASKBAR_H // 2)

# The launcher, which is the kinds down the left and the things of that kind
# down the right. Written the way wm.c lays it out rather than as numbers
# that came out of it once.
MENU_ITEM, MENU_PAD = 32, 10
MENU_RAIL, MENU_PANE = 132, 152
MENU_W = MENU_PAD * 2 + MENU_RAIL + MENU_PANE
MENU_LEFT = DOCK_SIDE

# Productivity, Internet, Media, Games, System, Session: Games is the fourth
# row, and it did not exist until there was a game to put in it.
GAMES_ROW = 3
MENU_PANEL = (0xF4, 0xF4, 0xF7)


def menu_rows():
    """As tall as the longest column, which is how wm.c sizes it."""
    return 6


def menu_top():
    return PANEL_Y - (MENU_PAD * 2 + menu_rows() * MENU_ITEM) - 8


def rail_row(i):
    return (MENU_LEFT + MENU_PAD + 40,
            menu_top() + MENU_PAD + i * MENU_ITEM + MENU_ITEM // 2)


def pane_row(j):
    return (MENU_LEFT + MENU_PAD + MENU_RAIL + 50,
            menu_top() + MENU_PAD + j * MENU_ITEM + MENU_ITEM // 2)


WHITE = (0xFF, 0xFF, 0xFF)

# The five dollar chip, which blackjack draws only while it is waiting for
# a bet.
CHIP_RED = (0xD6, 0x45, 0x45)


def cards_in(px, rect):
    """How much card is inside a rectangle.

    White, exactly. The cloth, the rail, the chips, the plates and the
    labels are all something else, and the only white on the table is the
    face of a card -- so this is not a measure of how much was drawn, which
    is the check that goes green against a program that drew the wrong
    thing in the right place."""
    return count_in(px, SCREEN_W, rect, WHITE)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the card games")

    vm = Guest(DISK, memory=256)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        time.sleep(7)
        mon = vm.monitor()

        # --- the launcher has somewhere to put them --------------------
        #
        # A category with nothing in it is worse than no category, so this
        # is not decoration: the row exists because the programs do, and
        # clicking it has to produce them.
        mon.click(*LAUNCHER)
        time.sleep(1.5)
        mon.click(*rail_row(GAMES_ROW))
        time.sleep(1.2)
        w, h, px, shot = mon.screen("gm-menu")
        pane = (MENU_LEFT + MENU_PAD + MENU_RAIL, menu_top() + MENU_PAD,
                MENU_LEFT + MENU_W - MENU_PAD,
                menu_top() + MENU_PAD + 2 * MENU_ITEM)
        from harness import count_near
        c.add("the launcher has a Games row with something under it",
              count_near(px, w, pane, MENU_PANEL, 6) > 3000, shot)

        # --- blackjack ---------------------------------------------------
        mon.click(*pane_row(0))
        time.sleep(6)
        mon.move_to(20, 20)
        time.sleep(1.5)

        w, h, px, shot = mon.screen("bj-open")
        table = (180, 120, 980, 600)
        before = cards_in(px, table)
        c.add("blackjack opens with no cards on the table", before < 200, shot)
        if before >= 200:
            print("      %d white pixels before a deal" % before)

        # Return deals, which is the same key a person reaches for.
        #
        # Sent again if no cards arrived. One keystroke goes down the serial
        # line and this guest drops what it cannot drain in time, so a check
        # that presses Return once and then looks reports a dealer that will
        # not deal when the host is busy -- which it did, under the gate,
        # having passed on its own a minute before. Safe to repeat because
        # the table is looked at first: a hand that was dealt is seen to
        # have been dealt and Return is not pressed again, which matters,
        # because the second one would stand the hand.
        dealt = before
        for _ in range(4):
            vm.type(chr(10))
            deadline = time.time() + 6
            while time.time() < deadline:
                time.sleep(1.0)
                mon.move_to(20, 20)
                w, h, px, shot = mon.screen("bj-deal")
                dealt = cards_in(px, table)
                if dealt > before + 2000:
                    break
            if dealt > before + 2000:
                break
        c.add("and dealing puts cards on it", dealt > before + 2000, shot)
        if dealt <= before + 2000:
            print("      %d white pixels on the table after four tries "
                  "(%d before)" % (dealt, before))

        # The dealer's own hand: one card up and one face down before the
        # player has finished, and both of them plus whatever it draws
        # afterwards. This is the half that is not the player's, and a game
        # that dealt the player in and never played the dealer's hand would
        # pass every check above.
        dealer_band = (180, 130, 980, 260)
        dealer_before = cards_in(px, dealer_band)

        # Stand, and wait for the table to say the hand is over rather than
        # for a number of seconds.
        #
        # The key is sent again if nothing happened. This guest drops
        # keystrokes when it is busy -- a character goes down the serial
        # line and the console is not read often enough to take it -- and a
        # check that sends one and then measures is a check that reports a
        # working game as broken about one run in five. What is being
        # checked is that standing plays the dealer's hand out, not that
        # every byte arrives.
        # The n comes first because the dealer may be showing an ace, and
        # then the only question on the table is insurance and standing is
        # not one of the answers. Declining it when there is nothing to
        # decline does nothing, which is what makes it safe to always send.
        # The first version of this check sent only the s, sat in front of
        # an insurance offer for thirty seconds, and reported that standing
        # does not play the dealer's hand -- against a game that was
        # following the rule the check had not been told about.
        chips = 0
        for attempt in range(5):
            vm.type("n")
            time.sleep(0.6)
            vm.type("s")
            deadline = time.time() + 6
            while time.time() < deadline:
                time.sleep(1.0)
                w, h, px, shot = mon.screen("bj-stood")
                chips = count_in(px, SCREEN_W, (180, 580, 980, 680), CHIP_RED)
                if chips > 150: break
            if chips > 150: break

        dealer_after = cards_in(px, dealer_band)
        c.add("standing turns the dealer's hand over and plays it out",
              dealer_after > dealer_before + 600, shot)
        if dealer_after <= dealer_before + 600:
            print("      dealer %d -> %d white pixels"
                  % (dealer_before, dealer_after))

        # And the round ends: the chips are back out, which is the betting
        # state and not the playing one.
        #
        # By the chips rather than by how much is drawn down there. The
        # first version of this counted white in the bottom band and
        # expected little of it, which the Clear button -- white, and on
        # screen in exactly the state being checked for -- failed for the
        # right reason against a game that was working. The chips are a
        # colour nothing else on this table is, and they are drawn when a
        # bet can be made and at no other time, which is the question.
        c.add("and the hand is over, with the chips back out", chips > 150, shot)
        if chips <= 150:
            print("      %d chip pixels after the hand" % chips)

        # --- poker -------------------------------------------------------
        #
        # Started from the shell rather than the launcher, because the
        # launcher lives on the dock and a maximised blackjack window is in
        # front of it; the row above already proved the launcher offers it.
        vm.type("\x1b")                    # escape leaves the desktop
        time.sleep(2)
        vm.type("desktop\n")
        time.sleep(6)
        vm.type("poker\n")
        time.sleep(9)
        mon.move_to(20, 20)
        time.sleep(1.5)

        w, h, px, shot = mon.screen("pk-open")
        # Where the seat at the bottom of the table is dealt, in a window
        # that opened at its own size in the middle of the screen.
        yours = (330, 380, 700, 560)
        c.add("poker deals you two cards", cards_in(px, yours) > 1500, shot)
        if cards_in(px, yours) <= 1500:
            print("      %d white pixels in your hand" % cards_in(px, yours))

        # The board is empty before the flop and is not afterwards. Calling
        # through a whole hand is the only way to see one, so this calls.
        board = (330, 230, 700, 330)
        flop_before = cards_in(px, board)
        c.add("and the board is empty before the flop", flop_before < 200, shot)

        # Called until a board appears rather than a fixed six times and a
        # look. Each c is one byte down the serial line and a busy guest
        # drops what it cannot drain, so a fixed count reports a game that
        # never deals a flop when the host is loaded -- which it did, under
        # the gate, having passed on its own before and after. Calling again
        # when the board is still empty is only ever another call in the
        # same betting round, so there is nothing to undo.
        flop = 0
        for _ in range(14):
            vm.type("c")
            time.sleep(1.2)
            mon.move_to(20, 20)
            w, h, px, shot = mon.screen("pk-board")
            flop = cards_in(px, board)
            if flop > 1500:
                break
        c.add("and calling round brings a board out", flop > 1500, shot)
        if flop <= 1500:
            print("      %d white pixels on the board after fourteen calls"
                  % flop)
    finally:
        vm.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
