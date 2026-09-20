"""Boots zelr headless, uses the desktop, and looks at the screen.

The self test can prove the window server hands out a surface, but not that
anything reaches the display or that input comes back. This drives QEMU's
monitor to move the real mouse and to grab the real screen.

Everything it checks crosses a boundary the other tests cannot:

  - the terminal is a ring 3 program, so its window appearing at all means
    the window server, the compositor and the event queue all worked
  - the launcher opens Settings, another ring 3 program
  - clicking an accent in Settings writes a file, and the kernel's window
    manager re-reads it, so the desktop behind changes colour

Nothing here waits for a length of time. Every step waits for the thing it
is checking to be true, and calls it broken only when it never becomes true.
See tools/harness.py for why that distinction cost a day.

  python tools/shotcheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Guest, Checks, build_once, count_all, count_in,     # noqa: E402
                     centre_of, ROOT)

DISK = os.path.join(ROOT, "shotcheck.%d.img" % os.getpid())

TEAL = (0x2C, 0xC7, 0xA0)
# The title bar, which is a surface one layer above the window and not the
# accent. The accent is a one pixel frame around the focused window, so there
# is a couple of thousand of it on screen rather than a title bar's worth.
# The focused window's title bar: the surface one layer above the window.
CHROME = (0x31, 0x38, 0x3F)
# And where that window's title bar is, because counting this colour across
# the whole screen is not a check at all. The first attempt counted a shade
# the wallpaper gradient also passes through, and went on passing with the
# title bar deleted outright, which is how that was found. Inside the bar
# and clear of its text and its border. */
TITLEBAR = (60, 40, 700, 56)
INDIGO = (0x6E, 0x8A, 0xE8)
SLATE = (0x10, 0x14, 0x1A)
SWATCHES = [TEAL, INDIGO, (0xE0, 0xA0, 0x3C), (0xE0, 0x6A, 0x8C),
            (0x8A, 0x9B, 0xB0), (0x9A, 0xD1, 0x4A)]

# Where things are on a 1024x768 screen with the default layout.
SCREEN_W, SCREEN_H = 1024, 768
DOCK_H, DOCK_GAP, DOCK_SIDE = 44, 14, 16
PANEL_Y = SCREEN_H - DOCK_H - DOCK_GAP

BADGE = (DOCK_SIDE + 16 + 38, PANEL_Y + DOCK_H // 2)   # the name on the dock

# The launcher is two columns now: which kind of thing down the left, the
# things of that kind down the right. Reaching a program is therefore two
# moves rather than one -- rest on the kind, click the thing -- and a
# remembered row number means nothing without knowing which column it is in.
MENU_PAD = 10
MENU_ITEM_H = 32
MENU_RAIL = 132
MENU_PANE = 152
MENU_W = MENU_PAD * 2 + MENU_RAIL + MENU_PANE
MENU_LEFT = DOCK_SIDE

# Which kind, and where in it. Settings is the first thing under System,
# which is the fourth kind. Written as names and looked up, so moving a
# program between kinds is one edit here rather than two numbers to work
# out again.
MENU_KINDS = ["Productivity", "Internet", "Media", "System", "Session"]
MENU_IN = {
    "Productivity": ["Terminal", "Files", "Notes", "Calculator"],
    "Internet": ["Browser"],
    "Media": ["Paint", "Music"],
    "System": ["Settings", "Monitor", "System info"],
    "Session": ["Close all", "Leave desktop", "Shut down"],
}

MENU_RECT = (MENU_LEFT + 4, 380, MENU_LEFT + MENU_W - 4, PANEL_Y - 6)


def menu_where(name):
    """Which kind a program is under, and where in that kind."""
    for kind, things in MENU_IN.items():
        if name in things:
            return MENU_KINDS.index(kind), things.index(name)
    raise KeyError(name)


def rail_at(top, i):
    return (MENU_LEFT + MENU_PAD + 40,
            top + MENU_PAD + i * MENU_ITEM_H + MENU_ITEM_H // 2)


def pane_at(top, j):
    return (MENU_LEFT + MENU_PAD + MENU_RAIL + 50,
            top + MENU_PAD + j * MENU_ITEM_H + MENU_ITEM_H // 2)
# The launcher panel. This is the modern look's overlay colour, which is the
# surface lifted one step; under the built look it was the surface itself,
# and the two are different enough that a check written against one sees
# nothing at all under the other.
MENU_PANEL = (0xF3, 0xF3, 0xF6)
PAGE = (120, 120, 700, 480)

# Everything above the dock. Every count below is inside this rather than
# across the screen, which is what they were always asking about: the
# window, not the desktop it is on.
ABOVE = (0, 0, SCREEN_W, PANEL_Y - 2)


def menu_top(px, w, h):
    """The menu's top edge, by looking for it.

    It used to be a number. The menu grows upward from the panel, so where
    it starts depends on how many entries it has and where the panel is
    resting, and both of those changed: four more programs pushed it up and
    the panel moved off the bottom edge. The remembered y then pointed one
    entry down the list, the check clicked the entry under Settings, and
    every check after it failed saying Settings never opened.
    """
    x0, y0, x1, y1 = MENU_RECT

    # Downwards, through a band that begins above the menu and stops before
    # the panel, and only through the menu's own columns.
    #
    # Both halves of that matter and each one has been wrong. Scanning every
    # row on the screen found the top border of a window instead, because the
    # frame is the same grey the menu is. Then the band was made to start
    # below where the menu began, and adding one more program to the launcher
    # grew it upwards past the start of the band: the scan began in the
    # middle of the menu and called that the top, so every row was counted
    # from twenty four pixels too low and the click went one entry down the
    # list. It opened the monitor and reported that settings would not start.
    #
    # The band is the whole space a launcher of any sensible length can
    # occupy. What has to stay true is that nothing pale is drawn in these
    # columns above the menu, which on this desktop is the terminal, and the
    # terminal's page is dark.
    for y in range(max(0, y0), min(h, y1)):
        if count_in(px, w, (x0, y, x1, y + 1), MENU_PANEL) > (x1 - x0) // 2:
            return y
    return None


def settled_menu_top(mon, tries=16):
    """The same, once it has stopped moving.

    The menu rises into place, and enough of it is on screen to say it is
    open before it has finished arriving."""
    last = None
    for _ in range(tries):
        w, h, px, ppm = mon.screen("menu")
        top = menu_top(px, w, h)
        if top is not None and top == last:
            return top
        last = top
        time.sleep(0.25)
    return last


def main():
    keep = "--keep" in sys.argv
    build_once()
    vm = Guest(DISK, memory=64)
    c = Checks("desktop check")

    try:
        vm.wait_boot()
        vm.type("desktop\n")       # typed, not pasted: see shell_test.sh

        mon = vm.monitor()
        mon.move_to(*BADGE)

        # The desktop taking a while to draw is not the same as it never
        # drawing, and the difference is the whole reason this harness was
        # unreliable. Wait for the window, then decide.
        #
        # What is waited for is the window finished rather than started. Its
        # accent chrome appears first and its page is filled a moment later,
        # so waiting on the chrome alone catches the terminal half drawn and
        # every check after it reads a screen that was still being painted.
        # Palette independent on purpose. The chrome's colours are the thing
        # this file kept encoding and the thing that keeps changing, so what
        # is waited for is the terminal's own dark page, which belongs to the
        # program rather than to the theme.
        drawn = lambda w, h, px: count_in(px, w, PAGE, SLATE) > 100000
        w, h, px, shot, up = mon.wait_screen("desktop", drawn, timeout=60)
        c.add("a ring 3 terminal drew its window", drawn(w, h, px),
              shot)
        c.add("the screen is the mode that was asked for",
              (w, h) == (1024, 768), shot)
        c.add("the terminal has a dark page to type on",
              count_in(px, w, PAGE, SLATE) > 100000, shot)

        # --- the launcher --------------------------------------------------
        #
        # The menu is a panel one shade lighter than the window chrome,
        # sitting over the wallpaper in the lower left.
        w, h, px, shot, opened = mon.click_for(
            BADGE[0], BADGE[1], "launcher",
            lambda w, h, px: count_in(px, w, MENU_RECT, MENU_PANEL) > 8000,
            timeout=25)
        c.add("the launcher menu opens", opened, shot)

        # Found on the screen rather than worked out from constants.
        top = settled_menu_top(mon)
        c.add("the menu's top edge is on the screen", top is not None, shot)
        kind, thing = menu_where("Settings")

        # Waited for on all six swatches rather than on the accent. The
        # accent was already on the screen before the click, from the
        # terminal's own prompt, so the wait ended immediately and the
        # picture was taken before Settings had drawn anything: the check
        # passed, and what it was checking had not happened yet.
        drew = lambda w, h, px: all(count_in(px, w, ABOVE, sw) > 200
                                    for sw in SWATCHES)

        # And retried as a whole gesture rather than as a repeated click.
        # A click on a menu entry closes the menu whatever else it does, so
        # a second click at the same place lands on the wallpaper and a
        # third one after that does too: the retry could never work, and
        # what it reported was that Settings would not start.
        ran = False
        for attempt in range(3):
            # The kind first, which opens it: the pointer resting on a row
            # of the left column is what changes the right one, so this is
            # a move rather than a click and the click that follows it has
            # to land on the column that move just filled.
            mon.move_to(*rail_at(top or 0, kind))
            time.sleep(0.6)
            w, h, px, shot, ran = mon.click_for(
                pane_at(top or 0, thing)[0], pane_at(top or 0, thing)[1],
                "settings", drew, timeout=30, tries=1)
            if ran:
                break
            w, h, px, shot, opened = mon.click_for(
                BADGE[0], BADGE[1], "launcher-again",
                lambda w, h, px: count_in(px, w, MENU_RECT, MENU_PANEL) > 8000,
                timeout=25)
            if not opened:
                break
            top = settled_menu_top(mon) or top
        c.add("it launches settings, another ring 3 program", ran, shot)
        c.add("whose accent swatches are all on screen",
              all(count_in(px, w, ABOVE, sw) > 200 for sw in SWATCHES), shot)

        teal_before = count_in(px, w, ABOVE, TEAL)
        indigo_before = count_in(px, w, ABOVE, INDIGO)
        print("      accent pixels before: teal %d, indigo %d"
              % (teal_before, indigo_before))

        spot = centre_of(px, w, h, TEAL, within=ABOVE)
        c.add("the teal swatch is findable on screen", spot is not None, shot)

        # --- changing the accent -------------------------------------------
        #
        # The two accents should trade places: what was teal chrome becomes
        # indigo chrome, and only the one swatch of each is left. Both halves
        # have to be true at once, so the wait is on both and each is then
        # reported on its own.
        if spot is None:
            c.add("choosing an accent repaints the window manager", False, shot)
            c.add("and the old accent is gone from the chrome", False, shot)
        else:
            # How much moved, not how many times more there is. Six swatches
            # of every preset colour are on the screen whatever the theme is,
            # and they are most of both counts, so a ratio barely shifts even
            # when the whole of the chrome has changed colour. What is asked
            # for here is a swing of most of a swatch's worth of pixels, in
            # opposite directions.
            SWING = 600

            def repainted(w, h, px):
                return (count_in(px, w, ABOVE, TEAL) - teal_before > SWING
                        and indigo_before - count_in(px, w, ABOVE, INDIGO) > SWING)

            w, h, px, shot, _ = mon.click_for(spot[0], spot[1],
                                              "recoloured", repainted,
                                              timeout=30)
            teal_after = count_in(px, w, ABOVE, TEAL)
            indigo_after = count_in(px, w, ABOVE, INDIGO)
            print("      accent pixels after:  teal %d, indigo %d"
                  % (teal_after, indigo_after))
            c.add("choosing an accent repaints the window manager",
                  teal_after - teal_before > SWING, shot)
            c.add("and the old accent is gone from the chrome",
                  indigo_before - indigo_after > SWING, shot)
    finally:
        vm.stop()

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
