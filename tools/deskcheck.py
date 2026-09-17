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
MENU_ENTRIES = 9         # the launcher's entries, which set its height
MENU_TOP = (SCREEN_H - TASKBAR_H - TASKBAR_GAP
            - (MENU_ENTRIES * MENU_ITEM + 12) - 8)
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
LAUNCHER_RECT = (10, 428, 226, 720)

# The same buttons once the window has been maximised, when its frame is at
# 0,0 and as wide as the screen.
BTN_MAX_WHEN_MAXIMISED = (SCREEN_W - 46 + 7, 5 + 7)
LAUNCHER = (40, SCREEN_H - TASKBAR_H - TASKBAR_GAP + 17)

# The panel, where it sits when it is out, and the band it occupies. A
# maximised window takes the whole screen and the panel tucks itself under
# the bottom edge, so counting its own colour in this band says which of the
# two is happening without reading anything.
PANEL_Y = SCREEN_H - TASKBAR_H - TASKBAR_GAP
PANEL_BAND = (TASKBAR_GAP, PANEL_Y, SCREEN_W - TASKBAR_GAP, PANEL_Y + TASKBAR_H)
PANEL = MENU_PANEL                     # the floating layer, same colour

# The apps kept on the panel: the badge, then an icon every 30 pixels. The
# terminal is the first of them, and since a running program whose app is
# pinned is shown by its icon rather than by a chip of its own, that icon is
# what a click has to land on to bring the terminal back.
PINS_X = TASKBAR_GAP + 8 + 76 + 12
PIN_STEP = 30
TASKBAR_CHIP = (PINS_X + 11, PANEL_Y + 15)


def icon_at(i):
    """The middle of an icon, for clicking."""
    return (PINS_X + i * PIN_STEP + 11, PANEL_Y + 15)


def icon_ink(px, w, i):
    """A pixel of an icon's own colour, left of the letter in it, which is
    what says which app is in that slot."""
    x, y = PINS_X + i * PIN_STEP + 4, PANEL_Y + 15
    o = (y * w + x) * 3
    return tuple(px[o:o + 3])


def right_click(mon, x, y):
    """The monitor's second button, which arrives at the guest as the right
    one. Held, for the same reason click holds the left."""
    mon.move_to(x, y)
    mon.send("mouse_button 2", settle=0.4)
    mon.send("mouse_button 0", settle=0.6)

# Where the pointer is put before a wallpaper is photographed. It is drawn on
# the desktop like everything else, so leaving it wherever the last click
# happened makes two pictures of the same wallpaper differ: the wallpaper
# checks then pass because the mouse moved. Down in the taskbar it is outside
# the part being compared, and it is in the same place every time.
PARK = (1010, SCREEN_H - 10)

# Inside the terminal's page and clear of the pointer, which is parked in the
# window so that the wheel has something to land on.
WHEEL_PAGE = (60, 70, 600, 300)

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
        # The whole screen, panel included: it tucks itself away for a
        # window that wants the room. 730000 rather than 650000 is the
        # difference between the two: stopping above the panel is 714000 of
        # this colour and reaching the bottom of the screen is 759000, so
        # the old threshold passed either way and said nothing about which
        # had happened.
        strip = (100, SCREEN_H - 50, 900, SCREEN_H - 12)
        w, h, px, shot, ok = mon.click_for(
            BTN_MAX[0], BTN_MAX[1], "desk-maximised",
            lambda w, h, px: (page(px, w) > 730000
                              and count_in(px, w, strip, PAGE) > 20000))
        c.add("the maximise button fills the whole screen",
              page(px, w) > 730000, shot)
        c.add("and the program redrew into the space it was given",
              count_in(px, w, strip, PAGE) > 20000, shot)
        c.add("the panel tucks itself out of the way",
              count_in(px, w, PANEL_BAND, PANEL) < 500, shot)

        # And comes back for the pointer, over the window rather than
        # beside it, then goes again when the pointer leaves.
        mon.move_to(600, SCREEN_H - 1)
        w, h, px, shot, up = mon.wait_screen(
            "desk-panel-back",
            lambda w, h, px: count_in(px, w, PANEL_BAND, PANEL) > 15000)
        c.add("and comes back when the pointer reaches the bottom", up, shot)

        mon.move_to(500, 300)
        w, h, px, shot, gone = mon.wait_screen(
            "desk-panel-away",
            lambda w, h, px: count_in(px, w, PANEL_BAND, PANEL) < 500)
        c.add("and goes again when the pointer leaves it", gone, shot)

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

        # --- the wheel ---------------------------------------------------------
        #
        # The monitor's dz is the other way round from the packet the mouse
        # sends: dz 1 is a turn away from the hand, which is the scrollback
        # coming down over the page. The pointer is parked inside the window
        # and away from the rectangle being compared, because the wheel goes
        # to whatever is under the pointer and the pointer is drawn.
        mon.click(*TASKBAR_CHIP)
        wait_page(mon, "desk-back-wheel", lambda n: n > 100000)
        typed(mon, "help\n")
        time.sleep(1.5)

        mon.move_to(700, 450)
        time.sleep(0.5)
        w, h, px, shot, _ = mon.wait_screen("desk-wheel-before", lambda w, h, px: True)
        before = patch(px, w, WHEEL_PAGE)

        for _ in range(6):
            mon.send("mouse_move 0 0 1", settle=0.15)
        w, h, px, shot, rolled = mon.wait_screen(
            "desk-wheel-up",
            lambda w, h, px: patch(px, w, WHEEL_PAGE) != before, timeout=20)
        c.add("the wheel scrolls the window under the pointer", rolled, shot)

        for _ in range(12):
            mon.send("mouse_move 0 0 -1", settle=0.15)
        w, h, px, shot, back = mon.wait_screen(
            "desk-wheel-down",
            lambda w, h, px: patch(px, w, WHEEL_PAGE) == before, timeout=20)
        c.add("and the other way brings it back", back, shot)

        # --- the size of the screen -------------------------------------------
        #
        # Written into the same file the colours live in, from ring 3, and
        # applied by the window manager the next time it reads it. The
        # picture that comes back is a different size, which is not
        # something any amount of drawing could fake.
        #
        # 800 by 600 rather than something larger: the back buffer is the
        # whole screen and this machine has 64 MiB, so a size that does not
        # fit would be testing the heap rather than the screen.
        # The terminal is already in front: the wheel checks above brought it
        # back, and clicking its icon again would put it away.
        typed(mon, "write /zelr.cfg width 800\n")
        typed(mon, "append /zelr.cfg height 600\n")

        _, _, _, shot, smaller = mon.wait_screen(
            "desk-800x600", lambda w, h, px: w == 800 and h == 600, timeout=30)
        c.add("a screen size written from ring 3 is the size of the screen",
              smaller, shot)

        # And the desktop is laid out for it rather than still drawn for the
        # old one: the panel is where the bottom of this screen is.
        band = (TASKBAR_GAP, 600 - TASKBAR_H - TASKBAR_GAP,
                800 - TASKBAR_GAP, 600 - TASKBAR_GAP)
        _, _, _, shot, moved = mon.wait_screen(
            "desk-800-panel",
            lambda w, h, px: (w == 800
                              and count_in(px, w, band, PANEL) > 8000),
            timeout=30)
        c.add("and the panel is at the bottom of the new one", moved, shot)

        # Back, so nothing after this has to know about it.
        typed(mon, "write /zelr.cfg width 1024\n")
        typed(mon, "append /zelr.cfg height 768\n")
        mon.wait_screen("desk-back-big",
                        lambda w, h, px: w == SCREEN_W and h == SCREEN_H,
                        timeout=30)
        # --- the apps kept on the panel ---------------------------------------
        #
        # Left until last, because all of it changes what is on the taskbar
        # and everything above knows where the taskbar's first icon is.
        #
        # Each app's icon is a colour worked out from its path, so reading
        # one pixel of each says which app is in which slot, and that is the
        # whole of what dragging one along the panel is supposed to change.
        mon.move_to(*PARK)
        w, h, px, shot = None, None, None, None
        w, h, px, shot, _ = mon.wait_screen(
            "desk-pins", lambda w, h, px: True)
        before = [icon_ink(px, w, i) for i in range(5)]
        c.add("the taskbar starts with the apps the machine ships",
              len(set(before)) >= 3 and all(p != PANEL for p in before), shot)

        # The first one dragged two places along. The two it passes move up
        # to make room, so what lands where is known exactly.
        mon.drag(icon_at(0), icon_at(2))
        w, h, px, shot, moved = mon.wait_screen(
            "desk-pin-moved",
            lambda w, h, px: (icon_ink(px, w, 2) == before[0]
                              and icon_ink(px, w, 0) == before[1]))
        c.add("an icon dragged along the taskbar changes places", moved, shot)

        # Off the panel with the right button, which is the only way back to
        # a taskbar somebody does not want five things on.
        right_click(mon, *icon_at(4))
        w, h, px, shot, dropped = mon.wait_screen(
            "desk-pin-off",
            lambda w, h, px: icon_ink(px, w, 4) == PANEL)
        c.add("and the right button takes one off it", dropped, shot)

        # And back on, from the launcher, with the same button. Settings is
        # the fifth entry and it was the fifth icon.
        mon.click(*LAUNCHER)
        mon.wait_screen(
            "desk-menu-2",
            lambda w, h, px: count_in(px, w, LAUNCHER_RECT, MENU_PANEL) > 8000)
        right_click(mon, 60, MENU_TOP + 6 + 4 * MENU_ITEM + MENU_ITEM // 2)
        w, h, px, shot, backon = mon.wait_screen(
            "desk-pin-on",
            lambda w, h, px: icon_ink(px, w, 4) != PANEL)
        c.add("and an app from the launcher can be put back on", backon, shot)

    finally:
        vm.stop()

    # --- and a machine nobody told to open a desktop --------------------------
    #
    # Everything above boots with console on the kernel command line, because
    # every one of those checks starts by waiting for a shell prompt. This one
    # boots the way a machine booted from a disc does, with no command line at
    # all, and the desktop should be there without anyone asking for it.
    # More memory than the rest of this needs, because the last two checks
    # ask for a screen four times the size and the back buffer for one is
    # eight megabytes of heap.
    auto = Guest(os.path.join(ROOT, "deskauto.%d.img" % os.getpid()), memory=512,
                 args="")
    try:
        mon = auto.monitor()
        _, _, _, shot, up = mon.wait_screen(
            "desk-auto",
            lambda w, h, px: count_in(px, w, PANEL_BAND, PANEL) > 15000,
            timeout=90)
        c.add("a machine nobody told opens the desktop by itself", up, shot)

        # Nothing has printed a prompt, because nothing is at a console.
        c.add("and the console is not what came up",
              "zelr:/home>" not in auto.serial(), shot)

        # Escape is the way out, and the only way out on a machine with no
        # command line, which makes it worth checking rather than assuming.
        auto.type("\x1b")
        c.add("and escape leaves it for the shell",
              auto.wait_prompt(1, timeout=30))

        # --- a screen nothing here was written for ----------------------------
        #
        # Every coordinate in this file is for 1024 by 768, and the desktop
        # itself holds none: the panel, the launcher and every window are
        # laid out from the width and height of the framebuffer each frame.
        # This is what says so. 1920 by 1080 is both larger in every
        # direction and a different shape.
        auto.run("write /zelr.cfg width 1920")
        auto.run("append /zelr.cfg height 1080")
        auto.type("desktop\n")

        mon = auto.monitor()
        _, _, _, shot, big = mon.wait_screen(
            "desk-1920",
            lambda w, h, px: w == 1920 and h == 1080, timeout=60)
        c.add("a screen twice the size is the size it was asked for", big, shot)

        band = (TASKBAR_GAP, 1080 - TASKBAR_H - TASKBAR_GAP,
                1920 - TASKBAR_GAP, 1080 - TASKBAR_GAP)
        _, _, _, shot, laid = mon.wait_screen(
            "desk-1920-panel",
            lambda w, h, px: (w == 1920
                              and count_in(px, w, band, PANEL) > 30000),
            timeout=30)
        c.add("and the desktop is laid out across all of it", laid, shot)
    finally:
        auto.stop()
        try:
            os.remove(os.path.join(ROOT, "deskauto.%d.img" % os.getpid()))
        except OSError:
            pass

    for s in shots:
        if s not in c.shots:
            c.shots.append(s)
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
