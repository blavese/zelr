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
from harness import (Guest, Checks, build_once, count_in, count_near,
                     row_mean, colour_gap, face_width, FACE_BODY,
                     _count, ROOT)                            # noqa: E402

DISK = os.path.join(ROOT, "deskcheck.%d.img" % os.getpid())

PAGE = (0x10, 0x14, 0x1A)          # the terminal's default background
SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H = 44
TASKBAR_GAP = 14                   # the dock floats clear of the edge
DOCK_SIDE = 16

# The launcher is two columns: the kinds down the left, the things of that
# kind down the right, and as tall as the longer of them.
MENU_ITEM = 32
MENU_PAD = 10
MENU_RAIL = 132
MENU_PANE = 152
MENU_W = MENU_PAD * 2 + MENU_RAIL + MENU_PANE
MENU_ROWS = 6            # six kinds, and no kind has more than four in it
MENU_H = MENU_PAD * 2 + MENU_ROWS * MENU_ITEM

# It opens from the badge against the left of the dock, eight pixels above
# it. Written the way wm.c writes it rather than as the numbers that came
# out of it once.
MENU_LEFT = DOCK_SIDE
MENU_TOP = SCREEN_H - TASKBAR_H - TASKBAR_GAP - MENU_H - 8


def rail_row(i):
    """The middle of the i'th kind, down the left."""
    return (MENU_LEFT + MENU_PAD + 40,
            MENU_TOP + MENU_PAD + i * MENU_ITEM + MENU_ITEM // 2)


def pane_row(j):
    """The middle of the j'th thing of whichever kind is open."""
    return (MENU_LEFT + MENU_PAD + MENU_RAIL + 50,
            MENU_TOP + MENU_PAD + j * MENU_ITEM + MENU_ITEM // 2)


# The name at the left of the dock, which is what opens the launcher.
#
# There used to be a second thing that did: a wide field down the middle of
# the bar, whose only job was to open this same launcher. It is a find
# button in the tray now and it looks for words on the screen rather than
# for programs, so the launcher has one way in again and this is it. What
# is typed into the launcher still narrows the list, which is what the
# second check below is about and is unchanged.
DOCK_BADGE_W = 76
DOCK_BADGE = (DOCK_SIDE + 16 + DOCK_BADGE_W // 2,
              SCREEN_H - TASKBAR_H - TASKBAR_GAP + TASKBAR_H // 2)
MENU_RECT = (MENU_LEFT + 4, MENU_TOP + 4,
             MENU_LEFT + MENU_W - 4, MENU_TOP + MENU_H - 4)

# Well inside Paint's canvas, which is the surface colour over four hundred
# by two hundred and fifty pixels. Nothing else this desktop draws covers
# that rectangle in one colour.
PAINT_CANVAS = (300, 200, 700, 450)

# The desktop's own menu, which the right button opens where it is pressed.
CTX_W, CTX_ITEM, CTX_PAD = 204, 30, 8
CTX_N = 6
CTX_H = CTX_N * CTX_ITEM + CTX_PAD * 2
CTX_CLOSE_ALL = 4        # the row that closes every window
# The surface a menu is drawn on. Nearly the window surface, lifted a
# little, and laid down at an alpha of 250 out of 255 — so a trace of the
# wallpaper comes through it and an exact count finds fewer pixels than are
# there. Every count of it goes through count_near for that reason.
MENU_PANEL = (0xF4, 0xF4, 0xF7)
MENU_TOL = 6

# The terminal as it opens, and the frame around it. Written out the way
# wm.c writes it rather than as four numbers that happened to be right: the
# border went from one pixel to four and every one of these moved with it,
# and then back to one when the chrome was rebuilt.
WM_BORDER = 1
WM_TITLE_H = 32
WM_TOP = WM_BORDER + WM_TITLE_H

# The first window opens past the icon column rather than on top of it,
# which is where winsrv.c starts the cascade: ICON_LEFT + ICON_CELL_W, and
# a gap.
ICON_LEFT, ICON_CELL_W = 18, 92
ICON_TOP, ICON_CELL_H = 20, 88
WIN_X = ICON_LEFT + ICON_CELL_W + 14
WIN_Y, WIN_CW, WIN_CH = 36, 760, 480
OUTER_W = WIN_CW + WM_BORDER * 2
OUTER_H = WIN_CH + WM_TOP + WM_BORDER

# Three buttons against the right hand end of the title bar, right to left.
BTN_W, BTN_H, BTN_GAP = 30, 24, 2
BTN_STEP = BTN_W + BTN_GAP
BTN_Y = WIN_Y + WM_BORDER + (WM_TITLE_H - BTN_H) // 2 + BTN_H // 2


def btn_x(origin_x, outer_w, slot):
    return (origin_x + outer_w - WM_BORDER - 2 - BTN_W
            - slot * BTN_STEP + BTN_W // 2)


BTN_CLOSE = (btn_x(WIN_X, OUTER_W, 0), BTN_Y)
BTN_MAX = (btn_x(WIN_X, OUTER_W, 1), BTN_Y)
BTN_MIN = (btn_x(WIN_X, OUTER_W, 2), BTN_Y)
GRIP = (WIN_X + OUTER_W - 8, WIN_Y + OUTER_H - 8)

# The window's own frame, for counts that are about the window and not about
# whatever else happens to be the same colour somewhere on the desktop.
WIN_RECT = (WIN_X, WIN_Y, WIN_X + OUTER_W, WIN_Y + OUTER_H)

# Where the launcher sits when it opens from the dock badge, inset a little
# from its own edges so the rounded corners and the shadow are outside the
# rectangle being counted.
LAUNCHER_RECT = (MENU_LEFT + 4, MENU_TOP + 4,
                 MENU_LEFT + MENU_W - 4, MENU_TOP + MENU_H - 4)

# The same buttons once the window has been maximised, when its frame is at
# 0,0 and as wide as the screen.
BTN_MAX_WHEN_MAXIMISED = (btn_x(0, SCREEN_W, 1),
                          WM_BORDER + (WM_TITLE_H - BTN_H) // 2 + BTN_H // 2)
LAUNCHER = (DOCK_SIDE + 16 + 38, SCREEN_H - TASKBAR_H - TASKBAR_GAP + 22)

# The panel, where it sits when it is out, and the band it occupies. A
# maximised window takes the whole screen and the panel tucks itself under
# the bottom edge, so counting its own colour in this band says which of the
# two is happening without reading anything.
PANEL_Y = SCREEN_H - TASKBAR_H - TASKBAR_GAP

# The dock holds the brand, then a chip for each window. There are no
# pinned apps on it any more: six letters in circles said which six programs
# somebody had chosen and nothing else, and reaching a seventh meant opening
# the launcher regardless.
CHIPS_X = DOCK_SIDE + 16 + 76 + 18


def chip_at(titles, i):
    """The middle of the ith chip, given the titles in stack order.

    A chip is as wide as its own title, so where the second one starts
    depends on what the first one is called. This used to be one fixed
    point, which was right because it was not a chip at all: it was the
    pinned terminal, in the same slot whatever else was running. With the
    pins gone the terminal is wherever the stack puts it, and after an
    alt-tab that is not the front."""
    x = CHIPS_X
    for n, title in enumerate(titles):
        w = min(face_width(title, FACE_BODY) + 22, 160)
        if n == i:
            return (x + w // 2, PANEL_Y + TASKBAR_H // 2)
        x += w + 6
    raise IndexError(i)


TASKBAR_CHIP = chip_at(["zelr terminal"], 0)

# After alt-tab the terminal is behind paint in the stack, so it is the
# second chip rather than the first.
TERMINAL_CHIP_2 = chip_at(["paint", "zelr terminal"], 1)


def panel_showing(px, w, h):
    """True when the panel is across the bottom of the screen.

    It used to be enough to count the panel's own grey in the bottom band.
    The panel is not one colour any more: it is tinted glass over whatever
    wallpaper is behind it, so its colour changes along its own length and
    changes again when somebody picks a different background.

    What stays true whatever it is made of is that it is a band of its own.
    A row inside it does not look like a row of what is just above it —
    light over a dark desktop, light over a dark terminal page. When it has
    tucked itself away, the two rows are the same thing and the difference
    collapses. So the check is the difference rather than the colour, and it
    survives a theme nobody has thought of yet.

    Away from the badge on the left, the pinned icons next to it and the
    clock on the right, all of which are their own colours whether the panel
    is there or not.
    """
    left, right = 300, w - 220
    if right <= left:
        return False
    inside = row_mean(px, w, h - 6, left, right)
    above = row_mean(px, w, h - TASKBAR_H - 10, left, right)
    return colour_gap(inside, above) > 60


def desk_icon_rect(i):
    """The cell the i'th desktop icon occupies, which is what a click and a
    band both land on. The same arithmetic wm.c uses."""
    y = ICON_TOP + i * ICON_CELL_H
    return (ICON_LEFT, y, ICON_LEFT + ICON_CELL_W, y + ICON_CELL_H - 6)


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
              not panel_showing(px, w, h), shot)

        # And comes back for the pointer, over the window rather than
        # beside it, then goes again when the pointer leaves.
        mon.move_to(600, SCREEN_H - 1)
        w, h, px, shot, up = mon.wait_screen(
            "desk-panel-back",
            lambda w, h, px: panel_showing(px, w, h))
        c.add("and comes back when the pointer reaches the bottom", up, shot)

        mon.move_to(500, 300)
        w, h, px, shot, gone = mon.wait_screen(
            "desk-panel-away",
            lambda w, h, px: not panel_showing(px, w, h))
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
        # drag_for rather than drag: a drag is nine messages to the guest
        # and a loaded host drops some of them, which leaves a window that
        # did not move and a check that says the window manager cannot
        # resize. It said exactly that once under the gate, having passed on
        # its own a minute earlier and a minute later.
        _, _, _, shot, ok = mon.drag_for(
            GRIP, (GRIP[0] - 200, GRIP[1] - 150), "desk-resized",
            lambda w, h, px: page(px, w) < small - 100000)
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
            lambda w, h, px: count_near(px, w, LAUNCHER_RECT, MENU_PANEL,
                                        MENU_TOL) > 8000)
        c.add("the launcher menu opens where it is expected", up)

        # Paint, which is now two moves rather than one: the kind, then
        # the thing. Media is the third kind and Paint the first thing in
        # it, and both are worked out from where the menu is rather than
        # from a number that happened to be right once.
        mon.move_to(*rail_row(2))
        time.sleep(0.6)
        mon.click(*pane_row(0))
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
        mon.click(*TERMINAL_CHIP_2)
        _, _, shot, back = wait_page(mon, "desk-back-wheel",
                                     lambda n: n > 100000)
        c.add("the terminal comes back from its chip on the dock", back, shot)
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
        _, _, _, shot, moved = mon.wait_screen(
            "desk-800-panel",
            lambda w, h, px: w == 800 and panel_showing(px, w, h),
            timeout=30)
        c.add("and the panel is at the bottom of the new one", moved, shot)

        # Back, so nothing after this has to know about it.
        typed(mon, "write /zelr.cfg width 1024\n")
        typed(mon, "append /zelr.cfg height 768\n")
        mon.wait_screen("desk-back-big",
                        lambda w, h, px: w == SCREEN_W and h == SCREEN_H,
                        timeout=30)
        # --- the wallpaper, and the two things pressing it can mean ---------
        #
        # Left until last, because all of it ends with windows closed and
        # icons picked out, and everything above knows what is on the
        # screen.
        #
        # Nothing is decided on the press: a press starts a band and only
        # the release says whether it was a band or a click. So each of
        # these is a press and a release, and the one in the middle is a
        # press, a journey and a release.
        alt(mon, "d")
        time.sleep(1.0)

        # A click on the wallpaper is the launcher, where it was clicked.
        # It is pushed back on screen if it would not fit, which at this
        # height it would not, so the top is where the pushing leaves it.
        click_x, click_y = 600, 620
        here = (click_x + 4, PANEL_Y - MENU_H,
                click_x + MENU_W - 4, PANEL_Y - 8)
        mon.click(click_x, click_y)
        _, _, _, shot, up = mon.wait_screen(
            "desk-wallpaper-menu",
            lambda w, h, px: count_near(px, w, here, MENU_PANEL,
                                        MENU_TOL) > 8000)
        c.add("a click on the wallpaper opens the launcher under it", up, shot)

        mon.click(900, 120)          # and a click off it puts it away
        time.sleep(1.0)

        # The right button is the desktop's own menu, which is a different
        # panel: what can be done here, rather than what can be run.
        cx, cy = 600, 300
        ctx_rect = (cx + 4, cy + 4, cx + CTX_W - 4, cy + CTX_H - 4)
        right_click(mon, cx, cy)
        _, _, _, shot, up = mon.wait_screen(
            "desk-ctx",
            lambda w, h, px: count_near(px, w, ctx_rect, MENU_PANEL,
                                        MENU_TOL) > 4000)
        c.add("the right button opens the desktop's own menu", up, shot)

        # And the rows on it do what they say. Close all windows leaves a
        # desktop with nothing on it but the wallpaper and the icons, which
        # is what the page count already knows how to say.
        mon.click(cx + 60, cy + CTX_PAD + CTX_CLOSE_ALL * CTX_ITEM
                  + CTX_ITEM // 2)
        _, _, shot, ok = wait_page(mon, "desk-ctx-closed", lambda n: n < 1000)
        c.add("and closing every window from it closes every window",
              ok, shot)

        # A press that travels is a band.
        #
        # The wallpaper it is drawn over is compared with itself rather than
        # with another part of the same wallpaper: this one has lights in it
        # and a vignette, so no two stretches of it are the same colour and
        # a check that compared two of them would pass whether a band was
        # drawn or not.
        mon.move_to(620, 520)
        still_desktop(mon, "desk-settled", shots)
        w, h, px, _, _ = mon.wait_screen("desk-noband",
                                         lambda w, h, px: True)
        BAND_IN = (200, 200, 560, 400)
        quiet = patch(px, w, BAND_IN)
        quiet_icons = [patch(px, w, desk_icon_rect(i)) for i in range(1, 5)]

        # Held open for the picture, because a band that has been let go is
        # not on the screen any more.
        mon.send("mouse_button 1", settle=0.3)
        at = [620, 520]
        for step in range(1, 9):
            x = 620 + (60 - 620) * step // 8
            y = 520 + (90 - 520) * step // 8
            mon.send("mouse_move %d %d" % (x - at[0], y - at[1]), settle=0.1)
            at = [x, y]

        w, h, px, shot, drawn = mon.wait_screen(
            "desk-band",
            lambda w, h, px: patch(px, w, BAND_IN) != quiet, timeout=15)
        c.add("a press that travels draws a band on the wallpaper",
              drawn, shot)

        # And the icons it went over are picked out. The pointer ends up
        # over the first one, so that one is left out of the count: a tile
        # lights under the pointer whether it is selected or not, and a
        # check that cannot tell those apart is not a check.
        #
        # Waited for rather than read off the frame above. The band being
        # drawn and the icons lighting are two different redraws, and that
        # frame is the one that satisfied the first of them -- so this used
        # to ask whether the icons had lit in a picture taken at the moment
        # the band appeared, and on a busy host the answer is no. The button
        # is still down, so there is nothing to hurry.
        w, h, px, shot, picked = mon.wait_screen(
            "desk-band-caught",
            lambda w, h, px: all(patch(px, w, desk_icon_rect(i)) != q
                                 for i, q in zip(range(1, 5), quiet_icons)),
            timeout=15)
        c.add("and the icons it went over are picked out", picked, shot)

        mon.send("mouse_button 0", settle=0.6)

        # What it caught stays caught once the button is up, and a band
        # somewhere else lets them go again.
        mon.drag((700, 200), (900, 420))
        mon.move_to(*PARK)
        w, h, px, shot, freed = mon.wait_screen(
            "desk-unband",
            lambda w, h, px: all(patch(px, w, desk_icon_rect(i)) == q
                                 for i, q in zip(range(1, 5), quiet_icons)),
            timeout=15)
        c.add("and a band somewhere else lets them go", freed, shot)

        # --- the launcher, and typing into it ------------------------------
        #
        # Opened from the name at the left of the dock, which is the way in
        # now that the field down the middle has gone. What that field did
        # was open this same launcher, so nothing here is lost: the thing
        # being checked is that a name typed into it narrows the list, and
        # that is the launcher's own field either way.
        mon.click(*DOCK_BADGE)
        _, _, _, shot, up = mon.wait_screen(
            "desk-find",
            lambda w, h, px: count_near(px, w, MENU_RECT, MENU_PANEL,
                                        MENU_TOL) > 8000)
        c.add("the name on the dock opens the launcher beside it", up, shot)

        # Three letters from the middle of a word, so this is a search
        # rather than a prefix: somebody after the browser may well type
        # "web", and a search where one of those works and the other
        # silently finds nothing is one people stop using.
        typed(mon, "ain")
        typed(mon, "\n")
        _, _, _, shot, ran = mon.wait_screen(
            "desk-found",
            lambda w, h, px: count_near(px, w, PAINT_CANVAS, MENU_PANEL,
                                        8) > 90000,
            timeout=40)
        c.add("and what is typed into it finds a program and runs it",
              ran, shot)

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
            lambda w, h, px: panel_showing(px, w, h),
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

        _, _, _, shot, laid = mon.wait_screen(
            "desk-1920-panel",
            lambda w, h, px: w == 1920 and panel_showing(px, w, h),
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
