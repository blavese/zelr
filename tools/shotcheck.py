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
BADGE = (40, 745)             # the taskbar launcher
# The launcher's entries, in the order wm.c lists them. The menu grows
# upward from the taskbar, so adding a program moves everything above it and
# a remembered y is wrong from then on.
MENU_ENTRIES = ["Terminal", "Files", "Notes", "Paint", "Settings",
                "Monitor", "Music", "Calculator",
                "System info", "Close all", "Leave desktop", "Shut down"]
MENU_ITEM_H = 30
MENU_PAD = 6                  # the inset above the first entry
MENU_RECT = (10, 400, 210, 716)
MENU_PANEL = (0x3F, 0x46, 0x4D)   # the floating layer
PAGE = (120, 120, 700, 480)

# Everything above the panel.
#
# The apps kept on the taskbar are drawn in colours from the same six the
# themes use, so a preset colour is on screen whether or not the window that
# offers it is open. Every count below is inside this rather than across the
# screen, which is what they were always asking about: the window, not the
# desktop it is on.
ABOVE = (0, 0, 1024, 700)


def menu_top(px, w, h):
    """The menu's top edge, by looking for it.

    It used to be a number. The menu grows upward from the panel, so where
    it starts depends on how many entries it has and where the panel is
    resting, and both of those changed: four more programs pushed it up and
    the panel moved off the bottom edge. The remembered y then pointed one
    entry down the list, the check clicked the entry under Settings, and
    every check after it failed saying Settings never opened.
    """
    x0, x1 = MENU_RECT[0], MENU_RECT[2]
    for y in range(h):
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
        drawn = lambda w, h, px: (count_in(px, w, TITLEBAR, CHROME) > 6000
                                  and count_in(px, w, ABOVE, TEAL) > 1200
                                  and count_in(px, w, PAGE, SLATE) > 100000)
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
        idx = MENU_ENTRIES.index("Settings")
        w, h, px, shot, ran = mon.click_for(
            60, (top or 0) + MENU_PAD + idx * MENU_ITEM_H + MENU_ITEM_H // 2,
            "settings", lambda w, h, px: count_in(px, w, ABOVE, INDIGO) > 500,
            timeout=40)
        c.add("it launches settings, another ring 3 program", ran, shot)
        c.add("whose accent swatches are all on screen",
              all(count_in(px, w, ABOVE, s) > 200 for s in SWATCHES), shot)

        teal_before = count_in(px, w, ABOVE, TEAL)
        indigo_before = count_in(px, w, ABOVE, INDIGO)

        spot = centre_of(px, w, h, INDIGO, within=ABOVE)
        c.add("the indigo swatch is findable on screen", spot is not None, shot)

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
            def repainted(w, h, px):
                return (count_in(px, w, ABOVE, INDIGO) > indigo_before * 4
                        and count_in(px, w, ABOVE, TEAL) < teal_before / 4)

            w, h, px, shot, _ = mon.click_for(spot[0], spot[1],
                                              "recoloured", repainted,
                                              timeout=30)
            c.add("choosing an accent repaints the window manager",
                  count_in(px, w, ABOVE, INDIGO) > indigo_before * 4, shot)
            c.add("and the old accent is gone from the chrome",
                  count_in(px, w, ABOVE, TEAL) < teal_before / 4, shot)
    finally:
        vm.stop()

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
