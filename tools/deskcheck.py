"""Drives the desktop and checks the windows actually moved.

Minimising, maximising, snapping and resizing all end in a window covering a
different part of the screen, and the terminal paints its page one flat
colour. So counting that colour, and where it is, says exactly what the
window manager did without needing to read anything on screen.

The terminal opens as the first window at a known place, which is what makes
the button coordinates below predictable:

    x=40 y=36, content 760x480, so the outer frame is 762x505
    the three title buttons sit 26, 46 and 66 pixels in from the right edge

Every step waits for the window to be where it was told to go rather than
sleeping for a guess at how long that takes. See tools/harness.py.

  python tools/deskcheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Guest, Checks, build_once, count_in, _count,
                     ROOT)      # noqa: E402

DISK = os.path.join(ROOT, "deskcheck.%d.img" % os.getpid())

PAGE = (0x10, 0x14, 0x1A)          # the terminal's default background
SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H = 34
TASKBAR_GAP = 10                   # the panel floats clear of the edge
MENU_ITEM = 30
MENU_TOP = SCREEN_H - TASKBAR_H - TASKBAR_GAP - (8 * MENU_ITEM + 12) - 8
MENU_PANEL = (0x3F, 0x46, 0x4D)    # the floating layer

# The terminal as it opens.
WIN_X, WIN_Y, WIN_CW, WIN_CH = 40, 36, 760, 480
OUTER_W = WIN_CW + 2
OUTER_H = WIN_CH + 24 + 1

BTN_Y = WIN_Y + 5 + 7
BTN_CLOSE = (WIN_X + OUTER_W - 26 + 7, BTN_Y)
BTN_MAX = (WIN_X + OUTER_W - 46 + 7, BTN_Y)
BTN_MIN = (WIN_X + OUTER_W - 66 + 7, BTN_Y)
GRIP = (WIN_X + OUTER_W - 7, WIN_Y + OUTER_H - 7)

# The window's own frame, for counts that are about the window and not about
# whatever else happens to be the same colour somewhere on the desktop.
WIN_RECT = (WIN_X, WIN_Y, WIN_X + OUTER_W, WIN_Y + OUTER_H)

# Where the launcher sits when it opens from the taskbar badge, clear of the
# taskbar below it and of the clock, so nothing in this rectangle changes on
# its own while the menu is coming up.
LAUNCHER_RECT = (10, 458, 226, 720)

# The same buttons once the window has been maximised, when its frame is at
# 0,0 and as wide as the screen.
BTN_MAX_WHEN_MAXIMISED = (SCREEN_W - 46 + 7, 5 + 7)
TASKBAR_CHIP = (150, SCREEN_H - TASKBAR_H + 17)
LAUNCHER = (40, SCREEN_H - TASKBAR_H - TASKBAR_GAP + 17)

# Where the pointer is put before a wallpaper is photographed. It is drawn on
# the desktop like everything else, so leaving it wherever the last click
# happened makes two pictures of the same wallpaper differ: the wallpaper
# checks then pass because the mouse moved. Down in the taskbar it is outside
# the part being compared, and it is in the same place every time.
PARK = (1010, SCREEN_H - 10)

WHOLE = (0, 0, SCREEN_W, SCREEN_H)
LEFT_HALF = (0, 0, SCREEN_W // 2, SCREEN_H - TASKBAR_H)
RIGHT_HALF = (SCREEN_W // 2, 0, SCREEN_W, SCREEN_H - TASKBAR_H)

# What the monitor calls the keys that are not letters.
NAMED = {" ": "spc", "\n": "ret", "/": "slash", ".": "dot"}


def typed(mon, text):
    for ch in text:
        mon.send("sendkey %s" % NAMED.get(ch, ch), settle=0.06)


def alt(mon, key):
    """A chord. The monitor spells these with a dash."""
    mon.send("sendkey alt-%s" % key, settle=0.3)


def show_desktop(mon):
    """Everything away, and the pointer out of the picture."""
    alt(mon, "d")
    mon.move_to(*PARK)


def page(px, w, rect=WHOLE):
    return count_in(px, w, rect, PAGE)


def wait_page(mon, name, want, timeout=30):
    """Waits for the terminal's page to cover the amount of screen want
    accepts, and reports how much it actually covered."""
    w, h, px, shot, ok = mon.wait_screen(
        name, lambda w, h, px: want(page(px, w)), timeout=timeout)
    return page(px, w), (w, h, px), shot, ok


def click_page(mon, at, name, want, timeout=24, rect=WHOLE):
    """Clicks something, and checks it did what it was clicked for."""
    w, h, px, shot, ok = mon.click_for(
        at[0], at[1], name, lambda w, h, px: want(page(px, w, rect)),
        timeout=timeout)
    return page(px, w, rect), shot, ok


def patch(px, w, rect):
    """The pixels inside a rectangle, as bytes, for comparing frames."""
    left, top, right, bottom = rect
    out = bytearray()
    for y in range(top, bottom):
        row = y * w * 3
        out += px[row + left * 3:row + right * 3]
    return bytes(out)


def desktop_bytes(w, h, px):
    """The bare desktop, as raw bytes, so two of them can be compared without
    caring what is actually drawn on it. Every third row is enough to tell
    one wallpaper from another and a third of the work."""
    out = bytearray()
    for y in range(0, SCREEN_H - TASKBAR_H, 3):
        row = y * w * 3
        out += px[row:row + SCREEN_W * 3]
    return bytes(out)


def bare_desktop(mon, name, shots, timeout=30):
    """The desktop with the windows off it.

    Waiting for that matters more than it looks. A picture taken while a
    window is still going away is neither the old desktop nor the new one,
    and two of those in a row are never the same. Compare them and every
    wallpaper check passes whatever the wallpaper is doing, which is what
    these did: with the config parser deliberately broken so that setting a
    wallpaper did nothing at all, all three still said PASS."""
    w, h, px, shot, _ = mon.wait_screen(
        name, lambda w, h, px: page(px, w) < 1000, timeout=timeout)
    shots.append(shot)
    return desktop_bytes(w, h, px), shot


def still_desktop(mon, name, shots, tries=40):
    """The same, and not changing: two pictures in a row that match.

    Only for the wallpapers that hold still. Stars drift on purpose, so
    asking them to settle would wait forever."""
    last = cur = None
    shot = None
    for _ in range(tries):
        cur, shot = bare_desktop(mon, name, shots)
        if cur == last:
            return cur, shot
        last = cur
        time.sleep(0.3)
    return cur, shot


def main():
    keep = "--keep" in sys.argv
    build_once()
    vm = Guest(DISK, memory=64)
    c = Checks("desktop test")
    shots = []

    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(500, 400)

        opened, _, shot, ok = wait_page(
            mon, "desk-open", lambda n: 300000 < n < 380000, timeout=60)
        c.add("the terminal opens at the size it asked for", ok, shot)

        # --- minimise ------------------------------------------------------
        # Counted inside the window's own rectangle rather than across the
        # screen. The terminal's page colour is one the desktop gradient can
        # also land on, so a screen wide count says "the window is still
        # there" when what it found was the wallpaper in the gap beside the
        # panel. Scoped here it can only be answering the question asked.
        _, shot, ok = click_page(mon, BTN_MIN, "desk-minimised",
                                 lambda n: n < 200, rect=WIN_RECT)
        c.add("the minimise button takes the window off screen", ok, shot)

        # --- and back, from the taskbar -------------------------------------
        _, shot, ok = click_page(mon, TASKBAR_CHIP, "desk-restored",
                                 lambda n: abs(n - opened) < 20000)
        c.add("clicking it in the taskbar brings it back", ok, shot)

        # --- maximise -------------------------------------------------------
        #
        # Two things at once: the frame fills the work area, and the program
        # was told to redraw at the new size so its own page reaches the
        # bottom of it. A frame stretched without the program hearing about
        # it would pass the first and fail the second.
        strip = (100, SCREEN_H - TASKBAR_H - 40, 900, SCREEN_H - TASKBAR_H - 10)
        w, h, px, shot, ok = mon.click_for(
            BTN_MAX[0], BTN_MAX[1], "desk-maximised",
            lambda w, h, px: (page(px, w) > 650000
                              and count_in(px, w, strip, PAGE) > 20000))
        c.add("the maximise button fills the screen above the taskbar",
              page(px, w) > 650000, shot)
        c.add("and the program redrew into the space it was given",
              count_in(px, w, strip, PAGE) > 20000, shot)

        # --- restore --------------------------------------------------------
        small, shot, ok = click_page(mon, BTN_MAX_WHEN_MAXIMISED,
                                     "desk-unmaximised",
                                     lambda n: abs(n - opened) < 20000)
        c.add("pressing it again puts the window back", ok, shot)

        # --- resizing by the corner -----------------------------------------
        #
        # Done here, while the window is known to be back at the size and
        # place it opened at, so the grip is where it was.
        mon.drag(GRIP, (GRIP[0] - 200, GRIP[1] - 150))
        _, _, shot, ok = wait_page(mon, "desk-resized",
                                   lambda n: n < small - 100000)
        c.add("dragging the corner makes the window smaller", ok, shot)

        # --- snapping with the keyboard -------------------------------------
        def snapped(left, right):
            return lambda w, h, px: (count_in(px, w, left, PAGE) > 300000
                                     and count_in(px, w, right, PAGE) < 1000)

        alt(mon, "left")
        _, _, _, shot, ok = mon.wait_screen(
            "desk-snap-left", snapped(LEFT_HALF, RIGHT_HALF))
        c.add("alt and left snaps a window to that half", ok, shot)

        alt(mon, "right")
        _, _, _, shot, ok = mon.wait_screen(
            "desk-snap-right", snapped(RIGHT_HALF, LEFT_HALF))
        c.add("and alt and right to the other", ok, shot)

        # --- the wallpapers --------------------------------------------------
        #
        # Set through the config file, from the terminal, which is the same
        # path the settings window uses: a ring 3 program writes a file and
        # the window manager notices. Everything is put away first so what is
        # measured is only the desktop.
        #
        # The default is a gradient and it does not move, so it is a fixed
        # thing to compare against. This check used to be `len(shot) > 0`,
        # which is true of any picture at all: it passed whatever the window
        # manager did, including nothing.
        show_desktop(mon)
        default_paper, shot = still_desktop(mon, "desk-default", shots)

        mon.click(*TASKBAR_CHIP)
        wait_page(mon, "desk-back", lambda n: n > 100000)
        typed(mon, "write /zelr.cfg wallpaper 4\n")     # stars
        show_desktop(mon)

        stars_a = default_paper
        for _ in range(40):
            stars_a, shot = bare_desktop(mon, "desk-stars-a", shots)
            if stars_a != default_paper:
                break
            time.sleep(0.4)
        c.add("a wallpaper written from ring 3 reaches the desktop",
              stars_a != default_paper, shot)

        # Stars drift, so two pictures of them are never the same. A still
        # one means the window manager stopped repainting.
        stars_b = stars_a
        for _ in range(30):
            stars_b, shot = bare_desktop(mon, "desk-stars-b", shots)
            if stars_b != stars_a:
                break
            time.sleep(0.3)
        c.add("and one that drifts is not the same twice",
              stars_b != stars_a, shot)

        mon.click(*TASKBAR_CHIP)
        wait_page(mon, "desk-back-2", lambda n: n > 100000)
        typed(mon, "write /zelr.cfg wallpaper 6\n")     # weave
        show_desktop(mon)

        weave = default_paper
        for _ in range(40):
            weave, shot = still_desktop(mon, "desk-weave", shots, tries=4)
            if weave not in (stars_a, stars_b, default_paper):
                break
            time.sleep(0.4)
        c.add("a different wallpaper draws differently",
              weave not in (stars_a, stars_b, default_paper), shot)

        # --- alt+tab ----------------------------------------------------------
        #
        # Open a second window, then check the front one changes. Paint covers
        # its canvas in its own colour, so which is in front is visible in how
        # much of the terminal is left showing.
        mon.click(*LAUNCHER)
        _, _, _, _, up = mon.wait_screen(
            "desk-menu",
            lambda w, h, px: count_in(px, w, LAUNCHER_RECT, MENU_PANEL) > 8000)
        c.add("the launcher menu opens where it is expected", up)

        # Paint, the fourth entry, worked out from where the menu is rather
        # than from a number. The number said Paint and had been landing on
        # System info, which opens a window of its own, so the check below
        # passed without a second program ever being started.
        mon.click(60, MENU_TOP + 6 + 3 * MENU_ITEM + MENU_ITEM // 2)
        w, h, px, shot, ok = mon.wait_screen(
            "desk-two",
            lambda w, h, px: desktop_bytes(w, h, px) != weave, timeout=40)
        c.add("the launcher opens a second program", ok, shot)
        two_up = page(px, w)

        alt(mon, "tab")
        _, _, _, shot, ok = mon.wait_screen(
            "desk-cycled", lambda w, h, px: page(px, w) != two_up)
        c.add("alt and tab brings the window behind to the front", ok, shot)

        # --- show the desktop --------------------------------------------------
        alt(mon, "d")
        _, _, shot, ok = wait_page(mon, "desk-cleared", lambda n: n < 1000)
        c.add("alt and d puts everything away at once", ok, shot)

    finally:
        vm.stop()

    for s in shots:
        if s not in c.shots:
            c.shots.append(s)
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
